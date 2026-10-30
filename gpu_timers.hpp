/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Per-stage GPU timestamps around the injected NR chain: every chain under
// [RenoDX.DLSS5] NRGpuTimers (diagnostics), one chain every 250 ms under
// NRCostMeter (on by default since rc11: the panel's NR cost line).  A timed
// chain pays a handful of EndQuery calls plus one ResolveQueryData per run of
// written slots. Completion-only samples freeze CPU markers and reuse the
// recording tracker without timestamp queries or readback resources.
//
// Mechanism: every injected evaluate writes up to kSlotCount TIMESTAMP
// EndQuery markers into one segment of a ringed query heap and resolves its
// segment into a persistently-mapped READBACK buffer on the game's own command
// list (ResolveQueryData executes on the GPU with the list - no CPU wait).
// Completion is polled from the per-present lifecycle tick: the exact proof -
// the recording that carried the resolve was submitted and that submission
// completed exactly once and closed against replay (submission_tracker.hpp).
// A fence lease alone cannot prove single execution, so it never publishes a
// readback. End stamps the segment's frame-start
// texel with kUnwritten, so a segment the resolve never reached is counted
// as unexecuted instead of publishing a lap-old timing.  A segment whose
// proof never lands expires unread after a bounded tick window. Its slots
// remain reserved until the recording cannot execute them again.
// Everything below is guarded by runtime_mutex: Begin/Mark/End run inside the
// evaluate hook's lock, Poll runs in RunLifecycleTick's lock.

#pragma once

#include "brand.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>

#include <d3d12.h>

#include <include/reshade.hpp>

#include "../../utils/directx.hpp"
#include "gpu_lease.hpp"
#include "gpu_comparison.hpp"
#include "submission_tracker.hpp"

namespace renodx::addons::dlss5::gpu_timers {

// Marker slots within one evaluated chain.  Stages that do not run on a given
// workset (copy-in is after-path only, linearize/commit are HDR-only) simply
// stay absent: `written` has a bit per slot the chain wrote, and every delta
// below re-checks presence against it.  (Presence by "below the highest slot
// written" read unwritten slots - another evaluate's timestamps - and the
// pre-SR chain published linearize+scale 2052279 ms.)
constexpr uint32_t kSlotFrameStart = 0;
constexpr uint32_t kSlotCopyInEnd = 1;
constexpr uint32_t kSlotLinearizeEnd = 2;
constexpr uint32_t kSlotPassBase = 3;
// Per stack pass: encode start/end, NGX evaluate start/end, resolve end, and
// the end of the NR look stage, which runs between the evaluate and the
// resolve and marks only when it recorded work.
constexpr uint32_t kSlotPerPass = 6;
constexpr uint32_t kTimerMaxPasses = 4;
constexpr uint32_t kSlotCommitStart =
    kSlotPassBase + kSlotPerPass * kTimerMaxPasses;
constexpr uint32_t kSlotCommitEnd = kSlotCommitStart + 1;
constexpr uint32_t kSlotCopyBackEnd = kSlotCommitEnd + 1;
constexpr uint32_t kSlotCount = kSlotCopyBackEnd + 1;

// 64 slots per segment keeps the readback stride at 512 bytes, the alignment
// ResolveQueryData requires for its buffer offset.
constexpr uint32_t kSlotsPerEval = 64;
// Evaluations that may be awaiting their completion proof at once.  At one
// (or two, DLSS+RR) evaluates per frame this is several frames of slack even
// with the present thread stalled.
constexpr uint32_t kSegmentDepth = 32;
constexpr uint32_t kHeapSlots = kSlotsPerEval * kSegmentDepth;
// Lifecycle ticks a segment may stay pending before it is dropped unread
// (~1 s of presents; the starvation fallback keeps ticks flowing when
// presents themselves stop).
constexpr uint64_t kStaleTicks = 64;
// Written by the CPU over a segment's frame-start texel at End; the resolve
// replaces it with a timestamp.
constexpr uint64_t kUnwritten = UINT64_MAX;
constexpr char kLogPrefix[] = NR_NEXUS_NAME;

constexpr uint32_t PassEncodeStart(uint32_t pass) {
  return kSlotPassBase + pass * kSlotPerPass;
}
constexpr uint32_t PassEncodeEnd(uint32_t pass) { return PassEncodeStart(pass) + 1; }
constexpr uint32_t PassEvalStart(uint32_t pass) { return PassEncodeStart(pass) + 2; }
constexpr uint32_t PassEvalEnd(uint32_t pass) { return PassEncodeStart(pass) + 3; }
constexpr uint32_t PassResolveEnd(uint32_t pass) { return PassEncodeStart(pass) + 4; }
constexpr uint32_t PassLookEnd(uint32_t pass) { return PassEncodeStart(pass) + 5; }
static_assert(kSlotCount <= kSlotsPerEval);

inline void SafeLog(reshade::log::level level, const std::string& message) {
  reshade::log::message(
      level, (std::string(kLogPrefix) + ": " + message).c_str());
}

enum class SampleState : uint8_t {
  NOT_MEASURED, PENDING, VALID, NOT_EXECUTED, BYPASSED, DISCARDED,
  REPLAYED, EXPIRED, DEVICE_REMOVED, PROOF_UNAVAILABLE, INVALID_TIMESTAMPS,
};

inline const char* StateText(SampleState state) {
  switch (state) {
    case SampleState::NOT_MEASURED: return "Not measured";
    case SampleState::PENDING: return "Awaiting closed recording and GPU completion";
    case SampleState::VALID: return "GPU complete: single submission";
    case SampleState::NOT_EXECUTED: return "Not executed";
    case SampleState::BYPASSED: return "Skipped: zero strength";
    case SampleState::DISCARDED: return "Discarded: recording not submitted";
    case SampleState::REPLAYED: return "Rejected: command list replayed";
    case SampleState::EXPIRED: return "Expired: no recent proven sample";
    case SampleState::DEVICE_REMOVED: return "Unavailable: device removed";
    case SampleState::PROOF_UNAVAILABLE: return "Unavailable: exact submission or queue frequency missing";
    case SampleState::INVALID_TIMESTAMPS: return "Rejected: incomplete or invalid timestamps";
  }
  return "Not measured";
}

struct PassStats {
  uint32_t id = UINT32_MAX;  // Global pass id; timestamp slots are group-local.
  uint32_t model = 0;  // Resolved NGX creation request, not internal network proof.
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t output_width = 0;
  uint32_t output_height = 0;
  uint32_t encode_us = 0;
  uint32_t eval_us = 0;
  uint32_t resolve_us = 0;
  uint32_t look_us = 0;  // Included in resolve_us.
  uint32_t total_us = 0;
  bool look_measured = false;
  SampleState state = SampleState::NOT_EXECUTED;
};

struct Segment {
  uint32_t base = 0;   // first heap slot (== index * kSlotsPerEval)
  uint32_t count = 0;  // highest slot written + 1 (the resolve range)
  uint64_t written = 0;  // bit per slot written (kSlotsPerEval == 64)
  bool recorded = false;
  bool bypass = false;  // zero-strength passthrough frame: no stats
  bool measure = true;
  uint32_t pass_begin = 0;
  uint32_t pass_count = 0;
  GpuLease lease;
  uint64_t tick = 0;
  char path[8] = {};
  uint32_t width = 0;
  uint32_t height = 0;
  std::array<PassStats, kTimerMaxPasses> passes{};
  uint64_t sequence = 0;
  SampleState proof = SampleState::PENDING;
  FenceRef proof_fence;
  bool expired = false;
  gpu_comparison::Sample comparison;
};

// Latest published per-evaluate timing, in microseconds.  Guarded by
// runtime_mutex; the overlay copies it under that lock.
struct Stats {
  uint32_t total_us = 0;
  uint32_t own_us = 0;  // total minus the model evaluates
  uint32_t eval_us = 0;
  uint32_t encode_us = 0;
  uint32_t resolve_us = 0;
  uint32_t look_us = 0;  // part of resolve_us: the look stage's own span
  uint32_t linearize_us = 0;
  uint32_t commit_us = 0;
  uint32_t copies_us = 0;
  char path[8] = {};
  uint32_t width = 0;
  uint32_t height = 0;
  bool valid = false;
  std::array<PassStats, kTimerMaxPasses> passes{};
  SampleState state = SampleState::NOT_MEASURED;
  uint64_t sequence = 0;
  int64_t measured_ns = 0;
  const void* queue = nullptr;  // Compared only; never dereferenced.
  uint32_t samples = 0;
  // Frozen at recording; UI consumers must not match a newer configuration to
  // an old completed sample merely because stage/model/dimensions are equal.
  gpu_comparison::Identity identity;
};
inline Stats stats;
inline Stats stage_stats[3];
// Completion proofs have no measured durations and are consumed only by status collectors.
inline Stats execution_stats[3];
inline uint64_t stage_published[3] = {};
inline uint64_t measured_total_us = 0;
inline uint64_t dropped_segments = 0;
inline uint64_t published_segments = 0;
inline uint64_t unexecuted_segments = 0;
inline uint64_t lease_starved_segments = 0;
// Owned by runtime_mutex, like the timer ring. No persistent game/device ID.
inline gpu_comparison::Engine comparison;
inline gpu_comparison::Comparison comparison_result;
inline uint64_t device_session = 0;
inline void (*comparison_settings)(gpu_comparison::Identity*) = nullptr;
inline gpu_comparison::Identity comparison_capture_settings;

inline bool ComparisonActive() {
  for (const auto* windows : {&comparison.baseline, &comparison.control}) {
    for (const auto& window : *windows) {
      if (window.status == gpu_comparison::Status::COLLECTING) return true;
    }
  }
  return false;
}

inline int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline uint32_t StageIndex(const char* path) {
  return (std::strcmp(path, "preSR") == 0 ? 0u : (std::strcmp(path, "present") == 0 ? 2u : 1u));
}

namespace internal {

inline Segment segments[kSegmentDepth];
inline uint32_t head = 0;  // next segment Begin will claim
// The device's native identity (utils::directx::NativeIdentity), compared
// only.  The raw pointer is whichever face the evaluate's list reports -
// ReShade's device proxy or the native device - and a change of face is not a
// change of device: releasing the query heap there would free it under a
// resolve the GPU may still be executing.  Query heaps and readback buffers
// are never proxied, so the ones created through either face serve both.
inline IUnknown* context_device = nullptr;
inline ID3D12QueryHeap* heap = nullptr;
inline ID3D12Resource* readback = nullptr;
inline uint64_t* mapped = nullptr;
inline double frequency = 0.0;
inline std::atomic_uint64_t poll_ticks{0};
inline std::atomic<int64_t> last_log_ns{0};

struct EvalState {
  bool active = false;
  uint32_t idx = 0;
};
// No nesting: ProcessInline is serialized by runtime_mutex and the internal
// stack-pass evaluates never re-enter the injection path.
inline EvalState current;
inline uint64_t next_sequence = 0;
constexpr int64_t kDisplayIntervalNs = 500'000'000;
constexpr int64_t kSampleAgeNs = 2'000'000'000;
constexpr uint32_t kDisplaySamples = 4;
struct DisplayWindow {
  std::array<Stats, kDisplaySamples> samples{};
  uint32_t count = 0;
  uint32_t next = 0;
  int64_t updated_ns = 0;
};
inline std::array<DisplayWindow, 3> display_windows;

inline void Release() {
  if (mapped != nullptr) {
    readback->Unmap(0, nullptr);
    mapped = nullptr;
  }
  if (readback != nullptr) {
    readback->Release();
    readback = nullptr;
  }
  if (heap != nullptr) {
    heap->Release();
    heap = nullptr;
  }
  context_device = nullptr;
  frequency = 0.0;
  head = 0;
  current.active = false;
  for (Segment& s : segments) s = Segment{};
  stats.valid = false;
  stats.state = SampleState::NOT_MEASURED;
  for (auto& stage : stage_stats) stage = Stats{};
  for (auto& stage : execution_stats) stage = Stats{};
  display_windows = {};
}

inline bool Ensure(ID3D12Device* device) {
  IUnknown* const identity = renodx::utils::directx::NativeIdentity(device);
  if (context_device != identity) {
    Release();
    context_device = identity;
    ++device_session;
  }
  // A same-device retry must preserve completion-only recordings in the ring.
  if (heap != nullptr) return true;
  D3D12_QUERY_HEAP_DESC heap_desc{};
  heap_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
  heap_desc.Count = kHeapSlots;
  if (FAILED(device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&heap)))) {
    heap = nullptr;
    return false;
  }
  D3D12_HEAP_PROPERTIES rb_heap{};
  rb_heap.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC rb_desc{};
  rb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rb_desc.Width = static_cast<UINT64>(kHeapSlots) * sizeof(uint64_t);
  rb_desc.Height = 1;
  rb_desc.DepthOrArraySize = 1;
  rb_desc.MipLevels = 1;
  rb_desc.SampleDesc.Count = 1;
  rb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(device->CreateCommittedResource(
          &rb_heap, D3D12_HEAP_FLAG_NONE, &rb_desc,
          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) {
    readback = nullptr;
    heap->Release();
    heap = nullptr;
    return false;
  }
  if (FAILED(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) {
    mapped = nullptr;
    readback->Release();
    readback = nullptr;
    heap->Release();
    heap = nullptr;
    return false;
  }
  return true;
}

// False for a segment the resolve never reached (counted as unexecuted).
// `log`: the "NR gpu timers" lines, NRGpuTimers' own; the cost meter's
// samples publish silently (the addon logs their sum as "NR cost:").
inline void Reject(const Segment& s, SampleState state) {
  if (!s.measure) {
    auto& target = execution_stats[StageIndex(s.path)];
    if (s.sequence >= target.sequence) {
      target = Stats{.width = s.width, .height = s.height, .passes = s.passes,
                     .state = state, .sequence = s.sequence, .identity = s.comparison.identity};
      std::memcpy(target.path, s.path, sizeof(target.path));
    }
    return;
  }
  auto& target = stage_stats[StageIndex(s.path)];
  if (s.sequence < target.sequence) return;
  target = Stats{.passes = s.passes, .state = state, .sequence = s.sequence};
  display_windows[StageIndex(s.path)] = {};
  if (s.sequence >= stats.sequence) stats = target;
}

inline bool Publish(const Segment& s, bool log, const void* queue) {
  if (s.sequence < stage_stats[StageIndex(s.path)].sequence) return false;
  if (frequency <= 0.0 || mapped == nullptr) return false;
  const uint64_t* q = mapped + s.base;
  if (q[kSlotFrameStart] == kUnwritten) {
    ++unexecuted_segments;
    Reject(s, SampleState::DISCARDED);
    return false;
  }
  const double inv = 1000000.0 / frequency;  // ticks -> microseconds
  const auto delta_us = [&](uint32_t a, uint32_t b) -> uint32_t {
    if ((s.written >> a & 1) == 0 || (s.written >> b & 1) == 0 || q[b] <= q[a]) {
      return 0;
    }
    return static_cast<uint32_t>((q[b] - q[a]) * inv + 0.5);
  };

  Stats next;
  const auto has_span = [&](uint32_t a, uint32_t b) {
    return (s.written >> a & 1) != 0 && (s.written >> b & 1) != 0 && q[b] >= q[a];
  };
  if (!has_span(kSlotFrameStart, kSlotCopyBackEnd)
      || (q[kSlotCopyBackEnd] - q[kSlotFrameStart]) * inv > UINT32_MAX) {
    Reject(s, SampleState::INVALID_TIMESTAMPS);
    return false;
  }
  next.total_us = delta_us(kSlotFrameStart, kSlotCopyBackEnd);
  next.copies_us = delta_us(kSlotFrameStart, kSlotCopyInEnd)
                   + delta_us(kSlotCommitEnd, kSlotCopyBackEnd);
  next.linearize_us = delta_us(kSlotCopyInEnd, kSlotLinearizeEnd);
  if ((s.written >> kSlotCopyInEnd & 1) == 0) {
    // Pre-SR has no copy-in: the linearize span starts at the frame mark.
    next.linearize_us = delta_us(kSlotFrameStart, kSlotLinearizeEnd);
  }
  for (uint32_t pass = 0; pass < kTimerMaxPasses; ++pass) {
    auto& layer = next.passes[pass];
    layer = s.passes[pass];
    if ((s.written >> PassEncodeStart(pass) & 1) == 0) continue;
    if (layer.width == 0 || layer.height == 0
        || !has_span(PassEncodeStart(pass), PassEncodeEnd(pass))
        || !has_span(PassEncodeEnd(pass), PassEvalStart(pass))
        || !has_span(PassEvalStart(pass), PassEvalEnd(pass))
        || !has_span(PassEvalEnd(pass), PassResolveEnd(pass))
        || q[PassEncodeStart(pass)] < q[kSlotFrameStart]
        || q[PassResolveEnd(pass)] > q[kSlotCopyBackEnd]
        || ((s.written >> PassLookEnd(pass) & 1) != 0
            && (!has_span(PassEvalEnd(pass), PassLookEnd(pass))
                || !has_span(PassLookEnd(pass), PassResolveEnd(pass))))) {
      Reject(s, SampleState::INVALID_TIMESTAMPS);
      return false;
    }
    layer.encode_us = delta_us(PassEncodeStart(pass), PassEncodeEnd(pass));
    layer.eval_us = delta_us(PassEvalStart(pass), PassEvalEnd(pass));
    layer.resolve_us = delta_us(PassEvalEnd(pass), PassResolveEnd(pass));
    layer.look_us = delta_us(PassEvalEnd(pass), PassLookEnd(pass));
    layer.look_measured = (s.written >> PassLookEnd(pass) & 1) != 0;
    layer.total_us = delta_us(PassEncodeStart(pass), PassResolveEnd(pass));
    layer.state = SampleState::VALID;
    next.encode_us += layer.encode_us;
    next.eval_us += layer.eval_us;
    next.resolve_us += layer.resolve_us;
    next.look_us += layer.look_us;
  }
  if (std::none_of(next.passes.begin(), next.passes.end(), [](const auto& pass) {
        return pass.state == SampleState::VALID;
      })) {
    Reject(s, SampleState::NOT_EXECUTED);
    return false;
  }
  if (next.eval_us > next.total_us) {
    Reject(s, SampleState::INVALID_TIMESTAMPS);
    return false;
  }
  next.commit_us = delta_us(kSlotCommitStart, kSlotCommitEnd);
  next.own_us = next.total_us - next.eval_us;
  next.width = s.width;
  next.height = s.height;
  std::memcpy(next.path, s.path, sizeof(next.path));
  next.valid = true;
  next.state = SampleState::VALID;
  next.sequence = s.sequence;
  next.measured_ns = NowNs();
  next.identity = s.comparison.identity;
  next.queue = queue;
  next.samples = 1;
  if (s.comparison.sequence != 0 && ComparisonActive()) {
    auto sample = s.comparison;
    sample.completed_ns = next.measured_ns;
    sample.valid = true;
    sample.identity.queue = reinterpret_cast<uint64_t>(queue);
    sample.total_us = next.total_us;
    for (uint32_t pass = 0; pass < sample.pass_count; ++pass) {
      const auto& layer = next.passes[pass];
      sample.passes[pass] = {
          .id = layer.id, .model = layer.model, .width = layer.width, .height = layer.height,
          .output_width = layer.output_width, .output_height = layer.output_height,
          .eval_us = static_cast<double>(layer.eval_us),
          .state = (layer.state == SampleState::VALID ? gpu_comparison::PassState::VALID
                                                     : gpu_comparison::PassState::NOT_EXECUTED),
          .look_executed = layer.look_measured};
    }
    comparison.Observe(sample, next.measured_ns);
    if (!ComparisonActive()) comparison_result = comparison.Results();
  }
  if (s.sequence >= stats.sequence) stats = next;
  const uint32_t stage = StageIndex(s.path);
  auto& window = display_windows[stage];
  // Never mix queues, pass identities, models or working sizes in one average.
  const auto& previous = window.samples[(window.next + kDisplaySamples - 1) % kDisplaySamples];
  bool same_contract = window.count != 0 && previous.queue == next.queue
      && previous.identity == next.identity
      && previous.width == next.width && previous.height == next.height
      && next.measured_ns - previous.measured_ns <= kSampleAgeNs;
  for (uint32_t pass = 0; pass < kTimerMaxPasses && same_contract; ++pass) {
    const auto& a = previous.passes[pass];
    const auto& b = next.passes[pass];
    same_contract = a.id == b.id && a.model == b.model && a.width == b.width && a.height == b.height
        && a.output_width == b.output_width && a.output_height == b.output_height
        && a.state == b.state && a.look_measured == b.look_measured;
  }
  if (!same_contract) window = {};
  window.samples[window.next] = next;
  window.next = (window.next + 1) % kDisplaySamples;
  window.count = std::min(window.count + 1, kDisplaySamples);
  if (!same_contract || next.measured_ns - window.updated_ns >= kDisplayIntervalNs) {
    Stats display = next;
    const auto average = [&](auto member) {
      uint64_t sum = 0;
      for (uint32_t i = 0; i < window.count; ++i) sum += window.samples[i].*member;
      return static_cast<uint32_t>(sum / window.count);
    };
    display.total_us = average(&Stats::total_us);
    display.eval_us = average(&Stats::eval_us);
    display.own_us = display.total_us - display.eval_us;
    display.encode_us = average(&Stats::encode_us);
    display.resolve_us = average(&Stats::resolve_us);
    display.look_us = average(&Stats::look_us);
    display.linearize_us = average(&Stats::linearize_us);
    display.commit_us = average(&Stats::commit_us);
    display.copies_us = average(&Stats::copies_us);
    for (uint32_t pass = 0; pass < kTimerMaxPasses; ++pass) {
      const auto pass_average = [&](auto member) {
        uint64_t sum = 0;
        for (uint32_t i = 0; i < window.count; ++i) sum += window.samples[i].passes[pass].*member;
        return static_cast<uint32_t>(sum / window.count);
      };
      auto& layer = display.passes[pass];
      layer.total_us = pass_average(&PassStats::total_us);
      layer.eval_us = pass_average(&PassStats::eval_us);
      layer.encode_us = pass_average(&PassStats::encode_us);
      layer.resolve_us = pass_average(&PassStats::resolve_us);
      layer.look_us = pass_average(&PassStats::look_us);
    }
    display.samples = window.count;
    stage_stats[stage] = display;
    window.updated_ns = next.measured_ns;
  }
  stage_stats[stage].measured_ns = next.measured_ns;
  stage_stats[stage].sequence = next.sequence;
  ++published_segments;
  ++stage_published[stage];
  measured_total_us += next.total_us;

  const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
  int64_t last = last_log_ns.load(std::memory_order_relaxed);
  if (!log || now_ns - last < 5000000000LL
      || !last_log_ns.compare_exchange_strong(last, now_ns,
                                              std::memory_order_relaxed)) {
    return true;
  }
  SafeLog(
      reshade::log::level::info,
      "NR gpu timers [" + std::string(s.path) + " " + std::to_string(s.width)
          + "x" + std::to_string(s.height)
          + "]: total " + std::to_string(next.total_us / 1000.0)
          + "ms, model " + std::to_string(next.eval_us / 1000.0)
          + "ms, own overhead " + std::to_string(next.own_us / 1000.0)
          + "ms (encode " + std::to_string(next.encode_us / 1000.0)
          + ", resolve " + std::to_string(next.resolve_us / 1000.0)
          + ", linearize+scale " + std::to_string(next.linearize_us / 1000.0)
          + ", commit " + std::to_string(next.commit_us / 1000.0)
          + ", copies+park " + std::to_string(next.copies_us / 1000.0) + ")");
  // The look stage on its own line, so the line above keeps its shape for
  // every reader (analyze_longrun.py, the e2e lanes); its span is inside
  // "resolve" there.
  if (next.look_us != 0) {
    SafeLog(
        reshade::log::level::info,
        "NR gpu timers look [" + std::string(s.path) + " " + std::to_string(s.width) + "x"
            + std::to_string(s.height) + "]: look stage "
            + std::to_string(next.look_us / 1000.0) + "ms, inside resolve "
            + std::to_string(next.resolve_us / 1000.0) + "ms");
  }
  return true;
}

}  // namespace internal

// Completion-only samples use the same recording ownership without GPU queries.
// Instrumentation failure never blocks the image path.
inline bool Begin(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Device* device,
    const char* path,
    uint32_t width,
    uint32_t height, uint32_t pass_begin = 0, uint32_t pass_count = 0, bool measure = true) {
  if (measure) {
    if (!internal::Ensure(device)) return false;
  } else {
    IUnknown* const identity = renodx::utils::directx::NativeIdentity(device);
    if (internal::context_device != identity) {
      internal::Release();
      internal::context_device = identity;
      ++device_session;
    }
  }
  uint32_t available = 0;
  while (available < kSegmentDepth) {
    const auto& candidate = internal::segments[internal::head];
    if (!candidate.recorded && submission::ResourceReleasable(&candidate)) break;
    internal::head = (internal::head + 1) % kSegmentDepth;
    ++available;
  }
  if (available == kSegmentDepth) {
    // A stale or replayable recording still owns these query/readback slots.
    // Skip instrumentation rather than overwrite memory its GPU work can touch.
    ++dropped_segments;
    return false;
  }
  Segment& s = internal::segments[internal::head];
  s.base = internal::head * kSlotsPerEval;
  s.count = 0;
  s.bypass = false;
  s.measure = measure;
  s.pass_begin = pass_begin;
  s.pass_count = pass_count;
  s.lease = GpuLease{};
  s.tick = internal::poll_ticks.load(std::memory_order_relaxed);
  std::strncpy(s.path, path, sizeof(s.path) - 1);
  s.path[sizeof(s.path) - 1] = '\0';
  s.width = width;
  s.height = height;
  s.sequence = ++internal::next_sequence;
  s.proof = SampleState::PENDING;
  s.proof_fence = {};
  s.expired = false;
  s.passes = {};
  s.comparison = {};
  s.comparison.identity.device_session = device_session;
  if (comparison_settings != nullptr) {
    comparison_settings(&s.comparison.identity);
  }
  if (measure && ComparisonActive()) {
    s.comparison.stage = StageIndex(path);
    s.comparison.sequence = s.sequence;
    s.comparison.recorded_ns = NowNs();
    s.comparison.pass_count = pass_count;
  }
  for (uint32_t pass = 0; pass < std::min(pass_count, kTimerMaxPasses); ++pass) {
    s.passes[pass].id = pass_begin + pass;
  }
  internal::current.active = true;
  internal::current.idx = internal::head;
  if (s.measure) {
    command_list->EndQuery(
        internal::heap, D3D12_QUERY_TYPE_TIMESTAMP, s.base + kSlotFrameStart);
  }
  s.count = kSlotFrameStart + 1;
  s.written = uint64_t{1} << kSlotFrameStart;
  return true;
}

inline void SetPass(uint32_t pass, uint32_t id, uint32_t model, uint32_t width,
                    uint32_t height, uint32_t output_width, uint32_t output_height) {
  if (!internal::current.active || pass >= kTimerMaxPasses) return;
  internal::segments[internal::current.idx].passes[pass] = PassStats{
      .id = id, .model = model, .width = width, .height = height,
      .output_width = output_width, .output_height = output_height};
}

inline void Mark(ID3D12GraphicsCommandList* command_list, uint32_t slot) {
  if (!internal::current.active || slot >= kSlotsPerEval) return;
  Segment& s = internal::segments[internal::current.idx];
  if (s.measure) {
    command_list->EndQuery(
        internal::heap, D3D12_QUERY_TYPE_TIMESTAMP, s.base + slot);
  }
  if (slot >= s.count) s.count = slot + 1;
  s.written |= uint64_t{1} << slot;
}

// Flags the current segment as a zero-strength passthrough frame: it resolves
// and frees the ring slot, but never publishes stats (the chain recorded no
// work worth timing).
inline void MarkBypassed() {
  if (!internal::current.active) return;
  internal::segments[internal::current.idx].bypass = true;
}

// Resolve on the original list and retain its slots through the existing
// recording tracker. The lease is delivery telemetry only, never readback proof.
inline void End(ID3D12GraphicsCommandList* command_list) {
  if (!internal::current.active) return;
  internal::current.active = false;
  Segment& s = internal::segments[internal::current.idx];
  if (s.comparison.sequence != 0 && comparison_settings != nullptr) {
    auto identity = s.comparison.identity;
    comparison_settings(&identity);
    if (identity != s.comparison.identity) s.comparison.stable = false;
  }
  // Even a start-only recording owns its query slot until Reset and GPU
  // completion. It will reject as incomplete, but cannot be reused yet.
  // Only the slots this chain wrote.  A chain writes a sparse set - one stack
  // pass leaves the other passes' slots unwritten, a pass without look work
  // its look slot - and resolving a query that was never performed is invalid
  // D3D12 ("Cannot Resolve query that has never been performed"): through
  // alpha47 every timed evaluate with fewer than four passes raised it once
  // per unwritten slot under the debug layer.  Each run of written slots
  // resolves on its own; the readers skip unwritten slots by `written`.
  for (uint32_t first = 0; s.measure && first < s.count;) {
    if ((s.written >> first & 1) == 0) {
      ++first;
      continue;
    }
    uint32_t end = first + 1;
    while (end < s.count && (s.written >> end & 1) != 0) ++end;
    command_list->ResolveQueryData(
        internal::heap, D3D12_QUERY_TYPE_TIMESTAMP, s.base + first, end - first,
        internal::readback, static_cast<UINT64>(s.base + first) * sizeof(uint64_t));
    first = end;
  }
  // The segment is its own tracked object, as in norm_trace::Record; the CPU
  // stamp lands before this list can be submitted.
  if (s.measure) {
    internal::mapped[s.base + kSlotFrameStart] = kUnwritten;
  }
  submission::TrackUse(command_list, &s);
  s.lease = AcquireGpuLease();
  s.tick = internal::poll_ticks.load(std::memory_order_relaxed);
  s.recorded = true;
  internal::head = (internal::head + 1) % kSegmentDepth;
  const auto& previous = (s.measure ? stage_stats[StageIndex(s.path)] : execution_stats[StageIndex(s.path)]);
  if (!previous.valid) internal::Reject(s, SampleState::PENDING);
}

// Called by the existing tracker before it forgets a closed recording.
// No tracker re-entry or queue-map lock here: Poll resolves the frequency later.
inline void PreserveSubmissionProof(
    const void* marker, const std::vector<submission::GenerationSubmission>& submissions) {
  const auto address = reinterpret_cast<uintptr_t>(marker);
  const auto first = reinterpret_cast<uintptr_t>(internal::segments);
  if (address < first || address - first >= sizeof(internal::segments)
      || (address - first) % sizeof(Segment) != 0) return;
  auto& s = internal::segments[(address - first) / sizeof(Segment)];
  if (!s.recorded) return;
  if (submissions.empty()) {
    s.proof = SampleState::DISCARDED;
    return;
  }
  if (submissions.size() != 1 || s.proof != SampleState::PENDING) {
    s.proof = SampleState::REPLAYED;
    return;
  }
  const auto& receipt = submissions.front();
  if (receipt.pending || receipt.fence.empty()) {
    s.proof = SampleState::PROOF_UNAVAILABLE;
    return;
  }
  const auto completed = receipt.fence->GetCompletedValue();
  if (completed == UINT64_MAX) {
    s.proof = SampleState::DEVICE_REMOVED;
  } else if (completed >= receipt.value) {
    s.proof = SampleState::VALID;
    s.proof_fence = receipt.fence;
  }
}

// Lifecycle-tick side (runtime_mutex held), after DrainRetiredResources
// preserved closed-recording receipts. Expiry rejects a measurement without
// releasing slots a parked recording can still reference. `log`: as Publish's.
inline void Poll(bool exact_proofs, bool log) {
  if (ComparisonActive()) {
    comparison.Tick(NowNs());
    if (!ComparisonActive()) comparison_result = comparison.Results();
  }
  internal::poll_ticks.fetch_add(1, std::memory_order_relaxed);
  for (auto& timing : stage_stats) {
    if (timing.valid && NowNs() - timing.measured_ns > internal::kSampleAgeNs) {
      timing.valid = false;
      timing.state = SampleState::EXPIRED;
    }
  }
  for (auto& proof : execution_stats) {
    if (proof.valid && NowNs() - proof.measured_ns > internal::kSampleAgeNs) {
      proof.valid = false;
      proof.state = SampleState::EXPIRED;
    }
  }
  if (stats.valid && NowNs() - stats.measured_ns > internal::kSampleAgeNs) {
    stats.valid = false;
    stats.state = SampleState::EXPIRED;
  }
  // Oldest-first, but one parked recording must not suppress other stages.
  for (uint32_t offset = 0; offset < kSegmentDepth; ++offset) {
    Segment& s = internal::segments[(internal::head + offset) % kSegmentDepth];
    if (!s.recorded) continue;
    if (internal::poll_ticks.load(std::memory_order_relaxed) - s.tick >= kStaleTicks && !s.expired) {
      s.expired = true;
      ++dropped_segments;
      internal::Reject(s, SampleState::EXPIRED);
    }
    // Even a completed fence does not exclude future replay. Keep slots until
    // the recording closes; expired slots are retained too, but never read.
    if (!submission::ResourceReleasable(&s)) continue;
    if (!s.expired) {
      if (!exact_proofs || s.proof == SampleState::PENDING) s.proof = SampleState::PROOF_UNAVAILABLE;
      if (s.bypass) s.proof = SampleState::BYPASSED;
      if (s.proof == SampleState::VALID && !s.measure) {
        bool complete = s.pass_count != 0 && s.pass_count <= kTimerMaxPasses
            && (s.written & (uint64_t{1} << kSlotFrameStart)) != 0;
        for (uint32_t pass = 0; pass < std::min(s.pass_count, kTimerMaxPasses); ++pass) {
          const auto& layer = s.passes[pass];
          const uint64_t required = (uint64_t{1} << PassEncodeStart(pass))
              | (uint64_t{1} << PassEncodeEnd(pass)) | (uint64_t{1} << PassEvalStart(pass))
              | (uint64_t{1} << PassEvalEnd(pass)) | (uint64_t{1} << PassResolveEnd(pass));
          if (layer.id != s.pass_begin + pass || layer.width == 0 || layer.height == 0
              || (s.written & required) != required) {
            complete = false;
          }
        }
        if (complete) {
          auto& target = execution_stats[StageIndex(s.path)];
          if (s.sequence >= target.sequence) {
            target = Stats{.width = s.width, .height = s.height, .valid = true, .passes = s.passes,
                           .state = SampleState::VALID, .sequence = s.sequence, .measured_ns = NowNs(),
                           .identity = s.comparison.identity};
            std::memcpy(target.path, s.path, sizeof(target.path));
            for (uint32_t pass = 0; pass < s.pass_count; ++pass) {
              target.passes[pass].state = SampleState::VALID;
            }
          }
        } else {
          internal::Reject(s, SampleState::NOT_EXECUTED);
        }
      } else if (s.proof == SampleState::VALID) {
        internal::frequency = 0;
        const void* measured_queue = nullptr;
        {
          std::shared_lock lock(queue_completion_mutex);
          for (const auto& [queue, tracker] : queue_completions) {
            if (tracker.fence != s.proof_fence.fence) continue;
            UINT64 rate = 0;
            if (SUCCEEDED(queue->GetTimestampFrequency(&rate)) && rate != 0) {
              internal::frequency = static_cast<double>(rate);
              measured_queue = queue;
            }
            break;
          }
        }
        if (internal::frequency > 0) {
          if (internal::Publish(s, log, measured_queue) && QueryGpuLease(s.lease) == GpuLeaseState::kPending) {
            ++lease_starved_segments;
          }
        } else {
          internal::Reject(s, SampleState::PROOF_UNAVAILABLE);
        }
      } else {
        if (s.proof == SampleState::DISCARDED) ++unexecuted_segments;
        internal::Reject(s, s.proof);
      }
    }
    s.proof_fence = {};
    s.lease = {};
    s.recorded = false;
  }
}

// Device teardown / shutdown.
inline void ReleaseAll() { internal::Release(); }

}  // namespace renodx::addons::dlss5::gpu_timers
