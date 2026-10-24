/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// One-shot NR screenshot pair of ONE final presentation image, copied before
// and after its NR pass. Inline DLSS textures are not display screenshots:
// the game's downstream exposure, tone map and UI cannot be reconstructed
// from them. The present path temporarily supplies the comparison for F5.
// A separate diagnostic mode copies an evaluated model input/output pair;
// those files explicitly exclude the game's later processing and display UI.
//
// Design contract (v2.7 rewrite): the feature must be inert while idle. The
// present path never touches D3D12 objects, user32 (beyond the one hotkey
// poll), std::filesystem, or the heap on behalf of this feature until the user
// arms it. Concretely:
//   - No QueryInterface and no command queue anywhere in this file.  GPU
//     completion is proven by the submission tracker (submission_tracker.hpp):
//     every copy is a tracked use of its readback on the list that records
//     it, and a capture settles once no recording holding one of its
//     readbacks can still execute (rc11, TIME-01/03).  A present proves
//     nothing: a generated frame presents between the evaluate and its
//     submit, and an unrelated list completes first.  Only a session without
//     the queue hook falls back to a fence-completed lease (gpu_lease.hpp)
//     acquired once the capture frame has presented - a poll of
//     ID3D12Fence::GetCompletedValue, never a CPU wait - and to the
//     present-generation delay (kSettlePresents) for queues without a
//     tracked fence.
//   - Every readback is stamped at creation (kUnwrittenStamp): a stamp still
//     there when the worker reads it is a copy the GPU never ran, and the
//     capture is refused (counted) instead of written as a black pair.
//   - The render/evaluate thread only creates two addon-owned READBACK heap
//     buffers (Prepare stage). No filesystem calls there either.
//   - Readback mapping, display coding, PNG/JPEG encoding, and file IO run on a
//     dedicated worker thread (created lazily on the first hand-off). The
//     present thread only moves a finished capture into the worker's job slot.
//   - runtime_mutex (dlssnr.hpp) is never held across any of that work. This
//     module has its own state_mutex; the only legal ordering is
//     runtime_mutex -> state_mutex -> job_mutex (state_mutex -> the
//     submission tracker's mutex for TrackUse and ReadbacksReleasable), and
//     the worker takes job_mutex alone.
//   - The worker is detached and never joined: Shutdown runs under the DLL
//     loader lock (DllMain PROCESS_DETACH), where joining any running thread
//     deadlocks. Shutdown drains owned state and raises shutdown_requested;
//     the worker exits on its own and the process reclaims it at exit.
//
// Compatibility notes (v4.1.5): the files carry no alpha - HDR intermediates
// routinely carry alpha 0/NaN, and an alpha-respecting viewer composites
// those to a fully black image.  The pair may be cropped to
// the engine-declared output subrect when the DLSS output resource is larger
// than what the engine actually renders (black-margin titles).

#pragma once

#include "brand.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <array>
#include <deque>
#include <memory>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <d3d12.h>
#include <d3d11_4.h>

#include <include/reshade.hpp>

#include "../../utils/path.hpp"
#include "../../utils/png.hpp"
#include "gpu_lease.hpp"
#include "submission_tracker.hpp"

namespace renodx::addons::dlss5::screenshot {

enum class RequestKind : uint32_t { FINAL_PRESENTATION, PRE_FG_SERVICE };

enum class SourceMode : uint32_t { kAuto = 0, kFinalPresent = 1, kEvaluatedBuffer = 2 };
inline std::atomic_uint32_t source_mode = 0;
// Set only by an explicit producer contract identifying a separate host
// presentation window. An absent/unknown contract keeps the final frame.
inline std::atomic_bool separate_presentation = false;
// Independent P0 switch; frozen into PendingCapture at PrepareCapture.
inline std::atomic_bool stage_planes_enabled = false;

enum class HdrCaptureMode { COMMIT_CORE, MODEL_TRACE };
struct CaptureRect {
  uint32_t x = 0, y = 0, width = 0, height = 0;
};
struct HdrCapturePlan {
  HdrCaptureMode mode = HdrCaptureMode::MODEL_TRACE;
  CaptureRect roi;
  bool sequence = false;
};

inline CaptureRect HdrPlaneRect(const HdrCapturePlan& plan, const char* name,
                               uint32_t full_width, uint32_t full_height) {
  if (name == nullptr) return {};
  const std::string_view plane(name);
  if (plane == "hdr_block_statistics") {
    if (!plan.sequence && plan.mode == HdrCaptureMode::COMMIT_CORE) {
      return {.width = (full_width + 31) / 32, .height = (full_height + 31) / 32};
    }
    if (full_width == 0 || full_height == 0 || plan.roi.width == 0 || plan.roi.height == 0
        || static_cast<uint64_t>(plan.roi.x) + plan.roi.width > full_width
        || static_cast<uint64_t>(plan.roi.y) + plan.roi.height > full_height) return {};
    const uint32_t grid_max_x = (full_width - 1) / 32;
    const uint32_t grid_max_y = (full_height - 1) / 32;
    // commit: block_pos=(pixel+0.5)/32-0.5, b0=clamp(floor(block_pos)), b1=min(b0+1).
    // Preserve every loaded neighbour, including the zero-weight edge loads.
    const uint32_t first_x = std::min(plan.roi.x >= 16 ? (plan.roi.x - 16) / 32 : 0, grid_max_x);
    const uint32_t first_y = std::min(plan.roi.y >= 16 ? (plan.roi.y - 16) / 32 : 0, grid_max_y);
    const uint32_t last_pixel_x = plan.roi.x + plan.roi.width - 1;
    const uint32_t last_pixel_y = plan.roi.y + plan.roi.height - 1;
    const uint32_t last_x = std::min((last_pixel_x >= 16 ? (last_pixel_x - 16) / 32 : 0) + 1, grid_max_x);
    const uint32_t last_y = std::min((last_pixel_y >= 16 ? (last_pixel_y - 16) / 32 : 0) + 1, grid_max_y);
    return {.x = first_x, .y = first_y, .width = last_x - first_x + 1, .height = last_y - first_y + 1};
  }
  if (!plan.sequence && plan.mode == HdrCaptureMode::COMMIT_CORE
      && (plane == "hdr_original_linear" || plane == "hdr_pre_commit"
          || plane == "hdr_post_pedestal_linear" || plane == "hdr_encoded_float"
          || plane == "packed_final")) {
    return {.width = full_width, .height = full_height};
  }
  return plan.roi;
}

// Output files (dlssnr.hpp persists them; frozen when a request arms).
// NRScreenshotFormat: 0 PNG, 1 JPEG, 2 both.  NRScreenshotLayout: 0 the
// pair, 1 the pair plus one side-by-side file (NR off | NR on) - one upload.
// NRScreenshotJpegQuality 80-100; NRScreenshotMaxMB caps each JPEG
// (0 = no cap; 10 is Discord's upload limit without Nitro).
enum class FileFormat : uint32_t { kPng = 0, kJpeg = 1, kPngAndJpeg = 2 };
inline std::atomic_uint32_t file_format = 0;
inline std::atomic_uint32_t layout = 0;
inline std::atomic_uint32_t jpeg_quality = 95;
inline std::atomic_uint32_t max_megabytes = 10;

inline constexpr bool HasSeparatePresentationContract(
    uint32_t version, uint32_t provider, uint32_t host, uint32_t cadence) {
  return version == 1u && provider == 0x444C3546u && host == 1u && cadence == 1u;
}

inline constexpr SourceMode ResolveSource(uint32_t mode, bool separate) {
  return mode == static_cast<uint32_t>(SourceMode::kEvaluatedBuffer)
                 || (mode == static_cast<uint32_t>(SourceMode::kAuto) && separate)
      ? SourceMode::kEvaluatedBuffer : SourceMode::kFinalPresent;
}

namespace internal {

// Fallback for queues without a tracked completion fence: presents that must
// pass after the copies were recorded before the readback buffers are
// mapped. Matches the retire-settle window the resource-release path uses;
// with fences the lease replaces this age rule.
constexpr uint64_t kSettlePresents = 4;
// Presents a capture with exact proofs waits for them before it gives up on
// delivery.  The proof needs the recording list submitted, completed AND
// reset, and an engine resets a list only when its allocator comes round
// again (2-3 frames, 4 presents each under 4x frame generation).  Giving up
// cancels the files only: the buffers stay retained until their own proof.
constexpr uint64_t kExactSettlePresents = 240;
// Written into every readback at creation; the copy overwrites it (its first
// row starts at offset 0).  Still there when the worker reads the buffer =
// the GPU never ran the copy.  As float data these are NaN payloads; as
// 8-bit data, four exact pixel values in sequence.
constexpr uint32_t kUnwrittenStamp[4] = {0x7FC0DEADu, 0x7FC0BEEFu, 0x7FC0CAFEu, 0x7FC0F00Du};
// Presents an armed capture may wait without a single DLSS evaluation before
// it disarms itself (~10 s at 60 fps).  Without this, F5 pressed during a
// loading screen or with NR off fires much later on the first evaluate -
// typically a black frame the user cannot make sense of.
constexpr uint32_t kArmTimeoutPresents = 600;
constexpr char kLogPrefix[] = NR_NEXUS_NAME;
constexpr UINT64 MAX_STAGE_READBACK_BYTES = 512ull * 1024ull * 1024ull;
constexpr size_t MAX_STAGE_PLANES = 24;
// Cover delayed exact completion/recording retirement; byte budgets remain independent.
constexpr size_t MAX_HDR_IN_FLIGHT = 16;
constexpr uint64_t MAX_HDR_TEMPORARY_BYTES = 256ull * 1024 * 1024;
constexpr uint64_t MAX_HDR_CPU_EXPORT_BYTES = 8ull * 1024 * 1024;
inline std::atomic_uint64_t aggregate_bytes = 0;
inline std::atomic_uint64_t aggregate_peak_bytes = 0;
inline std::atomic_uint64_t readback_reserved_bytes = 0, readback_peak_bytes = 0;
inline std::atomic_uint64_t temporary_reserved_bytes = 0, temporary_peak_bytes = 0;
inline std::atomic_uint64_t cpu_reserved_bytes = 0, cpu_peak_bytes = 0;
inline std::atomic_uint32_t hdr_live_sets = 0;
struct CaptureReservation {
  bool holds_hdr_slot = false;
  uint64_t bytes = 0;
  uint64_t temporary_bytes = 0, cpu_bytes = 0;
  ~CaptureReservation() {
    readback_reserved_bytes.fetch_sub(bytes);
    temporary_reserved_bytes.fetch_sub(temporary_bytes);
    cpu_reserved_bytes.fetch_sub(cpu_bytes);
    aggregate_bytes.fetch_sub(bytes + temporary_bytes + cpu_bytes);
    if (holds_hdr_slot) hdr_live_sets.fetch_sub(1);
  }
  bool ReserveCategory(std::atomic_uint64_t* counter, std::atomic_uint64_t* peak_counter,
                       uint64_t limit, uint64_t amount) {
    auto current = counter->load();
    do {
      if (amount > limit || current > limit - amount) return false;
    } while (!counter->compare_exchange_weak(current, current + amount));
    auto category_peak = peak_counter->load();
    while (category_peak < current + amount && !peak_counter->compare_exchange_weak(category_peak, current + amount)) {}
    const uint64_t total = aggregate_bytes.fetch_add(amount) + amount;
    auto peak = aggregate_peak_bytes.load();
    while (peak < total && !aggregate_peak_bytes.compare_exchange_weak(peak, total)) {}
    return true;
  }
  bool Reserve(uint64_t amount) {
    if (!ReserveCategory(&readback_reserved_bytes, &readback_peak_bytes, MAX_STAGE_READBACK_BYTES, amount)) return false;
    bytes += amount; return true;
  }
  bool ReserveTemporary(uint64_t amount) {
    if (!ReserveCategory(&temporary_reserved_bytes, &temporary_peak_bytes, MAX_HDR_TEMPORARY_BYTES, amount)) return false;
    temporary_bytes += amount; return true;
  }
  bool ReserveCpu(uint64_t amount) {
    if (!ReserveCategory(&cpu_reserved_bytes, &cpu_peak_bytes, MAX_HDR_CPU_EXPORT_BYTES, amount)) return false;
    cpu_bytes += amount; return true;
  }
};

// Raw float32 RGBA diagnostic planes captured alongside the PNG pair:
// the sRGB proxy the NR model actually sees, and the resolved linear work
// surface before the commit.  PNGs cannot establish exposure/units; these
// dumps (plus the sidecar meta) can.
struct DiagnosticPlane {
  std::string name;
  ID3D12Resource* readback = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 total_bytes = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  bool recorded = false;
  std::string error;
  GpuLease submission_proof;
  CaptureRect source_rect;
  CaptureRect copy_rect;
  uint32_t source_width = 0, source_height = 0;
  uint32_t full_width = 0, full_height = 0;
  bool region_copy = false;
  bool required = false;
  bool explicitly_skipped = false;
  uintptr_t source_identity = 0;
  DXGI_FORMAT source_format = DXGI_FORMAT_UNKNOWN;
};

struct OutputOptions {
  FileFormat format = FileFormat::kPng;
  bool side_by_side = false;
  uint32_t jpeg_quality = 95;
  uint32_t max_megabytes = 10;
};

struct NativeReceiver {
  std::shared_ptr<CaptureReservation> reservation;
  ID3D11DeviceContext* context = nullptr;
  ID3D11Texture2D* staging = nullptr;
  ID3D11Texture2D* source = nullptr;
  ID3D11Query* event = nullptr;
  CaptureRect rect;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  uint64_t bytes = 0;
  std::atomic_bool completed = false;
  std::string error;
  ~NativeReceiver() {
    if (event != nullptr) event->Release();
    if (staging != nullptr) staging->Release();
    if (source != nullptr) source->Release();
    if (context != nullptr) context->Release();
  }
  bool Poll() {
    if (completed) return true;
    if (context == nullptr || event == nullptr) return false;
    ID3D11Multithread* guard = nullptr;
    context->QueryInterface(IID_PPV_ARGS(&guard));
    if (guard != nullptr) guard->Enter();
    BOOL done = FALSE;
    const HRESULT result = context->GetData(event, &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (guard != nullptr) { guard->Leave(); guard->Release(); }
    completed = result == S_OK && done;
    if (FAILED(result)) error = "native_receiver_query_failed";
    return completed;
  }
};

struct PreFgRole {
  bool recorded = false;
  bool model_evaluated = false;
  uint32_t hdr_mode = 0;
  uint32_t passes = 0;
  uint32_t requested_passes = 0;
  bool norm_scale_required = false;
  uint64_t epoch = 0;
};

struct PendingCapture {
  RequestKind request_kind = RequestKind::FINAL_PRESENTATION;
  uint64_t service_serial = 0;
  uint64_t restore_config_epoch = 0;
  bool service_sealed = false;
  bool restoration_required = false;
  bool restoration_verified = false;
  uintptr_t restoration_original_identity = 0;
  uintptr_t restoration_parameter_identity = 0;
  PreFgRole backbuffer_role;
  PreFgRole hudless_role;
  bool hudless_present = false;
  bool compose_attempted = false;
  bool compose_used = false;
  uint32_t compose_serial = 0;
  uint64_t compose_area = 0;
  uint32_t compose_mode = 0;
  uint32_t compose_hud_limit = 0;
  bool compose_residual = false;
  bool compose_statistics_verified = false;
  bool fg_inputs_verified = false;
  std::optional<HdrCapturePlan> hdr_plan;
  std::shared_ptr<CaptureReservation> reservation;
  uint64_t temporary_bytes = 0;
  uint64_t cpu_export_bytes = 0;
  uint64_t cpu_row_bytes = 0;
  bool required_planes_complete = true;
  bool sequence_contract_validated = false;
  std::shared_ptr<NativeReceiver> native_receiver;
  bool native_receiver_required = false;
  uint32_t full_width = 0, full_height = 0;
  bool stage_planes = false;
  std::vector<ID3D12Resource*> temporary_resources;
  bool completion_proven = false;
  bool metadata_only = false;
  uint64_t completion_generation = 0;
  UINT64 allocated_bytes = 0;
  size_t rejected_planes = 0;
  std::string rejection_meta;
  bool model_diagnostic = false;
  ID3D12GraphicsCommandList* model_recording_list = nullptr;  // identity only
  OutputOptions output;
  ID3D12Resource* pre_nr_readback = nullptr;
  ID3D12Resource* nr_output_readback = nullptr;
  std::vector<DiagnosticPlane> planes;
  std::string meta;  // sidecar .meta.txt content (divisor, encoding, meter)
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 total_bytes = 0;
  UINT row_pitch = 0;
  // Captured subrect: engines can render into a subrect of a larger DLSS
  // output resource; width/height are the CROP dimensions written to the PNG,
  // while the footprint/row_pitch still describe the full resource rows.
  uint32_t crop_x = 0;
  uint32_t crop_y = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  uint8_t hdr_mode = 0;  // 0 = SDR, 1 = linear HDR, 2 = PQ HDR
  float linear_unit_nits = 80.f;  // scRGB: 1.0 is 80 nits, never scene exposure
  uint64_t timestamp_ms = 0;  // filename stem components; paths are only built
  uint64_t serial = 0;        // on the worker so Prepare never touches the fs
  uint64_t creation_generation = 0;  // present_generation when recorded
  // The proof is chosen once, at the capture frame's first present
  // (`lease_taken`, stamped `lease_generation`): `exact` when the queue hook
  // is live, which settles on the tracker alone; otherwise the lease, taken
  // then and empty when no queue fence exists, and a fence that never
  // advances (present-starved session) settles on the bounded fallback.
  GpuLease lease;
  uint64_t lease_generation = 0;
  bool lease_taken = false;
  bool exact = false;
  bool ready = false;                // both copies have been recorded
  bool aborted = false;
  bool pre_recorded = false;
  GpuLease pre_submission_proof;
  GpuLease post_submission_proof;
};

// Counted degraded outcomes (telemetry screenshot[...]): captures cancelled
// on a timeout, captures refused because a copy never ran, and buffer sets
// retained for want of a proof (superseded, cancelled or refused).
inline std::atomic_uint64_t captures_cancelled = 0;
inline std::atomic_uint64_t captures_unexecuted = 0;
inline std::atomic_uint64_t retained_sets = 0;

inline std::atomic_bool capture_requested = false;
inline std::atomic<RequestKind> requested_kind = RequestKind::FINAL_PRESENTATION;
inline std::atomic<SourceMode> requested_source = SourceMode::kFinalPresent;
inline std::atomic_uint32_t capture_serial = 0;
inline std::atomic_bool shutdown_requested = false;
inline std::atomic_bool hotkey_was_down = false;
// Presents counted while armed without any evaluate having recorded a pair.
inline std::atomic_uint32_t presents_while_armed = 0;
inline std::atomic_bool requires_passthrough_frame = false;
inline std::atomic_uint64_t request_id = 0;
// The file settings as they stood when the request armed; state_mutex.
inline OutputOptions requested_output;
inline std::atomic_bool stage_busy = false;
inline std::atomic_bool worker_busy = false;

// pending is guarded by state_mutex; has_pending mirrors pending.has_value()
// so the idle present path stays a single atomic load.
inline std::mutex state_mutex;
inline std::optional<PendingCapture> pending;
inline std::optional<HdrCapturePlan> configured_hdr_plan;
inline std::deque<PendingCapture> hdr_in_flight;
enum class SequenceOutcome { ACCEPTED, DROPPED, INVALID, CANCELLED, RESIZE, WRITTEN };
struct SequenceFrame { uint64_t generation = 0; SequenceOutcome outcome = SequenceOutcome::DROPPED; };
struct SequenceState {
  bool active = false;
  uint64_t id = 0;
  std::string contract_signature;
  HdrCapturePlan plan;
  uint64_t next_generation = 0;
  uint32_t attempted = 0;
  uint32_t full_width = 0, full_height = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  std::array<SequenceFrame, 120> frames{};
};
inline SequenceState sequence;
inline std::atomic_bool has_pending = false;

// The last capture that ended, however it ended, and the files it wrote: the
// issue report (report_flow.hpp) bundles exactly these and states why a
// capture is missing.  `outcome`: "written", "write_failed", "aborted",
// "not_ready", "gpu_unconfirmed" (a settle timeout cancelled it), "exception".
// finished_serial is the serial of `finished` and rises monotonically.
// Lock order: state_mutex -> finished_mutex; readers take finished_mutex alone.
struct FinishedCapture {
  uint64_t serial = 0;
  const char* outcome = "none";
  bool model_diagnostic = false;
  // The pair's lossless files when PNG was written, else its JPEGs.
  std::filesystem::path pre_image, post_image, meta;  // empty when not written
};
inline std::mutex finished_mutex;
inline FinishedCapture finished;
inline std::atomic_uint64_t finished_serial = 0;

inline void PublishFinished(FinishedCapture record) noexcept {
  try {
    std::scoped_lock lock(finished_mutex);
    const uint64_t serial = record.serial;
    if (serial < finished.serial) return;
    finished = std::move(record);
    finished_serial.store(serial, std::memory_order_release);
  } catch (...) {
  }
}

// Buffers from evaluates that were superseded within the same capture-frame
// (the capture re-records on every DLSS-family evaluate so the LAST one wins).
// They share the frame's command list, so they are released at the same settle
// window as the final capture rather than immediately.  This owns the ENTIRE
// superseded buffer set - the PNG pair AND every diagnostic plane: replacing
// `pending` used to destroy the old planes vector without releasing its raw
// COM readback pointers, leaking every re-recorded plane (a 4K FP16 plane is
// ~63 MiB; v6 audit issue 13).
struct RetiredCaptureBuffers {
  std::shared_ptr<CaptureReservation> reservation;
  std::shared_ptr<NativeReceiver> native_receiver;
  std::vector<ID3D12Resource*> temporary_resources;
  bool stage_planes = false;
  ID3D12Resource* pre_nr_readback = nullptr;
  ID3D12Resource* nr_output_readback = nullptr;
  std::vector<DiagnosticPlane> planes;

  void Release() {
    if (pre_nr_readback != nullptr) pre_nr_readback->Release();
    if (nr_output_readback != nullptr) nr_output_readback->Release();
    for (DiagnosticPlane& plane : planes) {
      if (plane.readback != nullptr) plane.readback->Release();
    }
    pre_nr_readback = nullptr;
    nr_output_readback = nullptr;
    planes.clear();
    for (auto* resource : temporary_resources) resource->Release();
    temporary_resources.clear();
    reservation.reset();
    native_receiver.reset();
  }

  // Takes ownership of a cancelled PendingCapture's raw buffers for
  // retention (unproven GPU completion: release happens on the set's own
  // tracker proof, or without the queue hook behind the next proven settle,
  // never on a timeout).
  void Take(PendingCapture&& capture) {
    reservation = std::move(capture.reservation);
    native_receiver = std::move(capture.native_receiver);
    temporary_resources = std::move(capture.temporary_resources);
    stage_planes = capture.stage_planes;
    pre_nr_readback = capture.pre_nr_readback;
    nr_output_readback = capture.nr_output_readback;
    planes = std::move(capture.planes);
    capture.pre_nr_readback = nullptr;
    capture.nr_output_readback = nullptr;
    capture.planes.clear();
  }
};
inline std::vector<RetiredCaptureBuffers> retired_buffers;
// Refused captures of a session without the queue hook: the GPU may still
// run their copies, so they are never released (worker-owned).
inline std::vector<RetiredCaptureBuffers> held_unexecuted;

// True when no recording that copies into one of the set's readbacks can
// still execute or replay (a PendingCapture or a RetiredCaptureBuffers).
template <typename Buffers>
inline bool ReadbacksReleasable(const Buffers& buffers) {
  if (buffers.native_receiver && buffers.native_receiver->context != nullptr && !buffers.native_receiver->completed.load()) return false;
  if (!submission::ResourceReleasable(buffers.pre_nr_readback)
      || !submission::ResourceReleasable(buffers.nr_output_readback)) {
    return false;
  }
  for (const DiagnosticPlane& plane : buffers.planes) {
    if (!submission::ResourceReleasable(plane.readback)) return false;
  }
  for (auto* resource : buffers.temporary_resources) {
    if (!submission::ResourceReleasable(resource)) return false;
  }
  return true;
}

// Worker hand-off slot. Guarded by job_mutex; the worker moves the job out
// under the lock and processes it outside.
inline std::mutex job_mutex;
inline std::condition_variable job_signal;
inline std::optional<PendingCapture> job;
inline std::deque<PendingCapture> hdr_jobs;
inline std::atomic_uint32_t outstanding_jobs = 0;
inline std::atomic_bool worker_started = false;
inline std::atomic_bool sequence_manifest_dirty = false;
inline std::atomic_bool sequence_manifest_busy = false;
inline std::atomic_bool sequence_manifest_failed = false;
// One coalescing mailbox, never a per-Present IO queue. Called with state_mutex held.
inline void QueueSequenceManifest() {
  if (sequence.id == 0) return;
  {
    std::scoped_lock lock(job_mutex);
    sequence_manifest_dirty.store(true);
  }
  job_signal.notify_one();
}

inline void SafeLog(reshade::log::level level, const std::string& message) {
  if (shutdown_requested.load(std::memory_order_acquire)) return;
  reshade::log::message(level, (std::string(kLogPrefix) + ": " + message).c_str());
}

// std::filesystem::path::string() throws for characters the ANSI code page
// cannot represent; logging must never throw.
inline std::string PathToLogString(const std::filesystem::path& path) {
  try {
    return path.string();
  } catch (...) {
  }
  try {
    const std::wstring wide = path.wstring();
    std::string lossy;
    lossy.reserve(wide.size());
    for (wchar_t c : wide) lossy.push_back(static_cast<char>(c));
    return lossy;
  } catch (...) {
    return "<unprintable path>";
  }
}

inline void ReleaseReadbacks(PendingCapture& capture) {
  if (capture.pre_nr_readback != nullptr) capture.pre_nr_readback->Release();
  if (capture.nr_output_readback != nullptr) capture.nr_output_readback->Release();
  capture.pre_nr_readback = nullptr;
  capture.nr_output_readback = nullptr;
  for (DiagnosticPlane& plane : capture.planes) {
    if (plane.readback != nullptr) plane.readback->Release();
  }
  capture.planes.clear();
  for (auto* resource : capture.temporary_resources) resource->Release();
  capture.temporary_resources.clear();
  capture.reservation.reset();
  capture.native_receiver.reset();
}

// Shared readback-buffer allocator for the PNG pair and the float planes.
inline bool MakeReadbackBuffer(
    ID3D12Device* device,
    UINT64 bytes,
    ID3D12Resource** result,
    bool initialize_stamp = true) {
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = bytes;
  buffer.Height = 1;
  buffer.DepthOrArraySize = 1;
  buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(device->CreateCommittedResource(
          &heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
          nullptr, IID_PPV_ARGS(result)))) {
    return false;
  }
  if (!initialize_stamp) return true;
  // kUnwrittenStamp.  A READBACK buffer maps for CPU writes too; nothing has
  // been recorded against it yet.
  const D3D12_RANGE nothing_read{0, 0};
  void* mapped = nullptr;
  if (SUCCEEDED((*result)->Map(0, &nothing_read, &mapped)) && mapped != nullptr) {
    const D3D12_RANGE written{
        0, static_cast<SIZE_T>(std::min<UINT64>(sizeof(kUnwrittenStamp), bytes))};
    std::memcpy(mapped, kUnwrittenStamp, written.End);
    (*result)->Unmap(0, &written);
  }
  return true;
}

inline uint64_t ReadbackAllocationBytes(ID3D12Device* device, uint64_t bytes) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = bytes; desc.Height = 1; desc.DepthOrArraySize = 1;
  desc.MipLevels = 1; desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  return device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
}

// The stamp is intact: the copy into this mapped readback never ran.
inline bool Unwritten(const void* mapped, UINT64 bytes) {
  return std::memcmp(mapped, kUnwrittenStamp,
                     static_cast<size_t>(std::min<UINT64>(sizeof(kUnwrittenStamp), bytes)))
         == 0;
}

// ---------------------------------------------------------------------------
// PNG byte plumbing: chunks and a stored-block zlib stream.  WIC compresses
// the image data (EncodeImageFiles); these write the chunks this module owns
// byte for byte, the iCCP profile, and the fallback PNG when WIC is absent.

inline void AppendPngBe32(std::vector<uint8_t>& bytes, uint32_t value) {
  bytes.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
  bytes.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
  bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
  bytes.push_back(static_cast<uint8_t>(value & 0xff));
}

inline uint32_t ReadBe32(const uint8_t* bytes) {
  return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16)
         | (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
}

inline uint32_t PngCrc32(const uint8_t* data, size_t size) {
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (uint32_t bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320u & (-(crc & 1u)));
  }
  return ~crc;
}

inline uint32_t PngAdler32(const uint8_t* data, size_t size) {
  uint32_t a = 1;
  uint32_t b = 0;
  for (size_t i = 0; i < size; ++i) {
    a = (a + data[i]) % 65521u;
    b = (b + a) % 65521u;
  }
  return (b << 16) | a;
}

inline void AppendPngChunk(
    std::vector<uint8_t>& png,
    const char type[4],
    const std::vector<uint8_t>& payload) {
  AppendPngBe32(png, static_cast<uint32_t>(payload.size()));
  const size_t type_offset = png.size();
  png.insert(png.end(), type, type + 4);
  png.insert(png.end(), payload.begin(), payload.end());
  AppendPngBe32(png, PngCrc32(png.data() + type_offset, 4 + payload.size()));
}

inline std::vector<uint8_t> ZlibStored(const uint8_t* data, size_t size) {
  std::vector<uint8_t> zlib = {0x78, 0x01};
  zlib.reserve(size + size / 65535 * 5 + 16);
  size_t offset = 0;
  do {
    const size_t block_size = std::min<size_t>(65535, size - offset);
    zlib.push_back(offset + block_size == size ? 1u : 0u);
    const uint16_t length = static_cast<uint16_t>(block_size);
    const uint16_t inverse = static_cast<uint16_t>(~length);
    zlib.push_back(static_cast<uint8_t>(length & 0xff));
    zlib.push_back(static_cast<uint8_t>(length >> 8));
    zlib.push_back(static_cast<uint8_t>(inverse & 0xff));
    zlib.push_back(static_cast<uint8_t>(inverse >> 8));
    zlib.insert(zlib.end(), data + offset, data + offset + block_size);
    offset += block_size;
  } while (offset < size);
  AppendPngBe32(zlib, PngAdler32(data, size));
  return zlib;
}

// ---------------------------------------------------------------------------
// Readback decoding.

inline float HalfToFloat(uint16_t bits) {
  const uint32_t sign = (bits & 0x8000u) << 16;
  const uint32_t exponent = (bits >> 10) & 0x1fu;
  const uint32_t mantissa = bits & 0x3ffu;
  uint32_t value = 0;
  if (exponent == 0) {
    if (mantissa != 0) {
      float result = std::ldexp(static_cast<float>(mantissa), -24);
      return (sign != 0 ? -result : result);
    }
    value = sign;
  } else if (exponent == 31) {
    value = sign | 0x7f800000u | (mantissa << 13);
  } else {
    value = sign | ((exponent + (127 - 15)) << 23) | (mantissa << 13);
  }
  float result = 0.f;
  std::memcpy(&result, &value, sizeof(result));
  return result;
}

inline uint32_t DiagnosticPixelBytes(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R8_UNORM: return 1;
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_FLOAT: return 2;
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R32_UINT: return 4;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
      return 4;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32_FLOAT:
      return 8;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
      return 16;
    default:
      return 0;
  }
}

inline bool DiagnosticFootprintValid(const DiagnosticPlane& plane) {
  const auto& footprint = plane.footprint.Footprint;
  const uint32_t pixel_bytes = DiagnosticPixelBytes(plane.format);
  if (pixel_bytes == 0 || plane.width == 0 || plane.height == 0
      || plane.width > footprint.Width || plane.height > footprint.Height
      || footprint.Depth != 1 || footprint.Format != plane.format
      || static_cast<UINT64>(plane.width) * pixel_bytes > footprint.RowPitch
      || plane.footprint.Offset > plane.total_bytes) return false;
  const UINT64 required_bytes = static_cast<UINT64>(plane.height - 1) * footprint.RowPitch
                                + static_cast<UINT64>(plane.width) * pixel_bytes;
  return required_bytes <= plane.total_bytes - plane.footprint.Offset;
}

// Stored code values for UNORM (including SRGB views), untouched floats otherwise.
inline bool DecodeDiagnosticRow(const uint8_t* source, uint32_t width,
                                DXGI_FORMAT format, float* rgba) {
  const uint32_t pixel_bytes = DiagnosticPixelBytes(format);
  if (source == nullptr || rgba == nullptr || pixel_bytes == 0) return false;
  for (uint32_t x = 0; x < width; ++x) {
    const uint8_t* pixel = source + static_cast<size_t>(x) * pixel_bytes;
    float* output = rgba + static_cast<size_t>(x) * 4;
    switch (format) {
      case DXGI_FORMAT_R8_UNORM:
        output[0] = pixel[0] / 255.f;
        output[1] = output[2] = 0.f; output[3] = 1.f;
        break;
      case DXGI_FORMAT_R16_UNORM:
      case DXGI_FORMAT_R16_FLOAT: {
        uint16_t value = 0;
        std::memcpy(&value, pixel, sizeof(value));
        output[0] = (format == DXGI_FORMAT_R16_FLOAT ? HalfToFloat(value) : value / 65535.f);
        output[1] = output[2] = 0.f; output[3] = 1.f;
        break;
      }
      case DXGI_FORMAT_R32_UINT: {
        uint32_t value = 0;
        std::memcpy(&value, pixel, sizeof(value));
        output[0] = static_cast<float>(value);
        output[1] = output[2] = 0.f; output[3] = 1.f;
        break;
      }
      case DXGI_FORMAT_R32_FLOAT:
      case DXGI_FORMAT_R32_TYPELESS:
      case DXGI_FORMAT_D32_FLOAT:
        // Depth storage is read as float bits, without a color or depth transform.
        std::memcpy(output, pixel, sizeof(float));
        output[1] = output[2] = 0.f; output[3] = 1.f;
        break;
      case DXGI_FORMAT_R16G16_FLOAT: {
        uint16_t channels[2];
        std::memcpy(channels, pixel, sizeof(channels));
        output[0] = HalfToFloat(channels[0]);
        output[1] = HalfToFloat(channels[1]);
        output[2] = 0.f; output[3] = 1.f;
        break;
      }
      case DXGI_FORMAT_R32G32_FLOAT:
        std::memcpy(output, pixel, sizeof(float) * 2);
        output[2] = 0.f; output[3] = 1.f;
        break;
      case DXGI_FORMAT_R8G8B8A8_UNORM:
      case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      case DXGI_FORMAT_B8G8R8A8_TYPELESS:
      case DXGI_FORMAT_B8G8R8A8_UNORM:
      case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: {
        // Typeless BGRA exports stored UNORM codes, without inferring a transfer function.
        const bool bgra = format == DXGI_FORMAT_B8G8R8A8_TYPELESS
            || format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        output[0] = pixel[bgra ? 2 : 0] / 255.f;
        output[1] = pixel[1] / 255.f;
        output[2] = pixel[bgra ? 0 : 2] / 255.f;
        output[3] = pixel[3] / 255.f;
        break;
      }
      case DXGI_FORMAT_R10G10B10A2_UNORM: {
        uint32_t packed = 0;
        std::memcpy(&packed, pixel, sizeof(packed));
        output[0] = (packed & 1023u) / 1023.f;
        output[1] = ((packed >> 10) & 1023u) / 1023.f;
        output[2] = ((packed >> 20) & 1023u) / 1023.f;
        output[3] = (packed >> 30) / 3.f;
        break;
      }
      case DXGI_FORMAT_R16G16B16A16_FLOAT: {
        uint16_t channels[4];
        std::memcpy(channels, pixel, sizeof(channels));
        for (size_t channel = 0; channel < 4; ++channel) {
          output[channel] = HalfToFloat(channels[channel]);
        }
        break;
      }
      case DXGI_FORMAT_R32G32B32A32_FLOAT:
        std::memcpy(output, pixel, sizeof(float) * 4);
        break;
      default:
        return false;
    }
  }
  return true;
}

inline bool ValidatePreFgPlanes(const PendingCapture& capture, std::string* errors) {
  bool complete = capture.service_serial != 0 && capture.service_sealed && capture.ready && !capture.aborted
      && capture.rejected_planes == 0 && capture.rejection_meta.empty()
      && capture.restoration_required == capture.hudless_present
      && capture.fg_inputs_verified && (!capture.restoration_required || capture.restoration_verified);
  const auto require = [&](const std::string& name, bool optional_absent = false) {
    const auto found = std::find_if(capture.planes.begin(), capture.planes.end(),
                                  [&](const auto& plane) { return plane.name == name; });
    const bool valid = found != capture.planes.end() && found->recorded && found->error.empty()
        && found->readback != nullptr && DiagnosticFootprintValid(*found);
    const bool legal_skip = found != capture.planes.end() && found->explicitly_skipped
        && ((name.ends_with("hdr_post_gamut_linear") && found->error == "scRGB_gamut_conversion_skipped")
            || (name.ends_with("hdr_block_statistics")
                && (found->error == "legacy_measurement_disabled" || found->error == "legacy_predicate_disabled"))
            || (optional_absent && found->error == "parameter_resource_absent"));
    if (!valid && !legal_skip) {
      complete = false;
      if (errors != nullptr) *errors += "required_plane." + name + "=missing_or_invalid\n";
    }
  };
  require("original_bb");
  if (capture.hudless_present) require("original_hl");
  const auto validate_role = [&](const PreFgRole& role, const char* name, bool required) {
    if (!required) return;
    if (!role.recorded || !role.model_evaluated || role.passes == 0 || role.passes > 4
        || role.passes != role.requested_passes || role.epoch != capture.restore_config_epoch) {
      complete = false;
      if (errors != nullptr) *errors += "role." + std::string(name) + ".error=incomplete_stack_or_epoch\n";
    }
    const std::string prefix = std::string(name) + "_";
    if (role.hdr_mode == 0) {
      for (uint32_t pass = 0; pass < std::min(role.passes, 4u); ++pass) {
        require(prefix + "pre_restore_p" + std::to_string(pass));
        require(prefix + "post_restore_p" + std::to_string(pass));
      }
    } else {
      for (const char* stage : {"hdr_original_linear", "hdr_pre_commit", "hdr_post_pedestal_linear",
             "hdr_post_gamut_linear", "hdr_encoded_float", "hdr_pedestal_field", "hdr_block_statistics"}) {
        require(prefix + stage);
      }
      if (role.norm_scale_required) require(prefix + "hdr_norm_scale");
    }
    require(prefix + "packed_final");
    require(prefix + "hdr_receiver_roi");
  };
  validate_role(capture.backbuffer_role, "backbuffer", !capture.compose_used);
  validate_role(capture.hudless_role, "hudless", capture.hudless_present);
  if (capture.compose_attempted) {
    require("compose_statistics");
    require("compose_ui", capture.compose_mode == 2);
  }
  if (capture.compose_used) {
    require("fg_input_backbuffer");
    if (!capture.compose_attempted) complete = false;
  }
  return complete;
}

inline bool CaptureCompletionProven(const PendingCapture& capture) {
  if (!capture.lease_taken) return false;
  if (capture.exact) return ReadbacksReleasable(capture);
  if (capture.stage_planes) return false;
  return !capture.lease.empty() && GpuLeaseCompleted(capture.lease);
}

inline bool VerifyPreFgReceivers(const PendingCapture& capture) {
  if (!capture.completion_proven || capture.metadata_only || capture.aborted) return false;
  bool recorded_role = false;
  for (const char* role : {"backbuffer", "hudless"}) {
    const auto& ledger = (std::string_view(role) == "backbuffer" ? capture.backbuffer_role : capture.hudless_role);
    if (!ledger.recorded) continue;
    recorded_role = true;
    const std::string prefix = std::string(role) + "_";
    const auto packed = std::find_if(capture.planes.begin(), capture.planes.end(),
        [&](const auto& plane) { return plane.name == prefix + "packed_final"; });
    const auto receiver = std::find_if(capture.planes.begin(), capture.planes.end(),
        [&](const auto& plane) { return plane.name == prefix + "hdr_receiver_roi"; });
    if (packed == capture.planes.end() || receiver == capture.planes.end()
        || packed->format != receiver->format || packed->width != receiver->width || packed->height != receiver->height
        || !DiagnosticFootprintValid(*packed) || !DiagnosticFootprintValid(*receiver)
        || packed->readback == nullptr || receiver->readback == nullptr) return false;
    const void* packed_data = nullptr;
    const void* receiver_data = nullptr;
    if (FAILED(packed->readback->Map(0, nullptr, const_cast<void**>(&packed_data)))) return false;
    if (FAILED(receiver->readback->Map(0, nullptr, const_cast<void**>(&receiver_data)))) {
      packed->readback->Unmap(0, nullptr);
      return false;
    }
    bool equal = packed_data != nullptr && receiver_data != nullptr;
    for (uint32_t y = 0; y < packed->height && equal; ++y) {
      equal = std::memcmp(static_cast<const uint8_t*>(packed_data) + packed->footprint.Offset
                              + uint64_t(y) * packed->footprint.Footprint.RowPitch,
                          static_cast<const uint8_t*>(receiver_data) + receiver->footprint.Offset
                              + uint64_t(y) * receiver->footprint.Footprint.RowPitch,
                          size_t(packed->width) * DiagnosticPixelBytes(packed->format)) == 0;
    }
    receiver->readback->Unmap(0, nullptr);
    packed->readback->Unmap(0, nullptr);
    if (!equal) return false;
  }
  return recorded_role;
}

// Retain the exact recording's fence receipts before the tracker prunes them.
// Without a stamp or an observed receipt, a reset-only recording is ambiguous.
inline bool StageSubmissionCompleted(PendingCapture* capture) {
  const auto observe = [](ID3D12Resource* resource, GpuLease* proof) {
    std::vector<std::pair<FenceRef, uint64_t>> tokens;
    if (submission::UseSubmissionTokens(resource, &tokens) == submission::UseSubmission::kTokens) {
      proof->waits = std::move(tokens);
      proof->coverage_complete = true;
    }
    return !proof->empty() && GpuLeaseCompleted(*proof);
  };
  bool completed = capture->pre_recorded && capture->ready;
  if (capture->pre_recorded) {
    completed = observe(capture->pre_nr_readback, &capture->pre_submission_proof) && completed;
  }
  if (capture->ready) {
    completed = observe(capture->nr_output_readback, &capture->post_submission_proof) && completed;
  }
  for (auto& plane : capture->planes) {
    if (plane.recorded) completed = observe(plane.readback, &plane.submission_proof) && completed;
  }
  return completed;
}

inline float DecodeUnsignedFloat(uint32_t bits, uint32_t mantissa_bits) {
  const uint32_t mantissa_mask = (1u << mantissa_bits) - 1u;
  const uint32_t mantissa = bits & mantissa_mask;
  const uint32_t exponent = (bits >> mantissa_bits) & 0x1fu;
  if (exponent == 0) {
    return std::ldexp(static_cast<float>(mantissa), -14 - static_cast<int>(mantissa_bits));
  }
  if (exponent == 31) return std::numeric_limits<float>::infinity();
  return std::ldexp(
      1.f + static_cast<float>(mantissa) / static_cast<float>(1u << mantissa_bits),
      static_cast<int>(exponent) - 15);
}

// ---------------------------------------------------------------------------
// Display encoding. SDR is already sRGB-coded, independent of storage type.
// HDR10 is already PQ/BT.2020 and is preserved, not decoded through an SDR
// approximation. scRGB is BT.709 linear at 80 nits per unit; convert primaries
// in linear light, then encode absolute nits to PQ. Do not normalize a capture
// by its maximum: doing so gives the two halves different exposure.
inline float LinearNitsToPq(float nits) {
  const float p = std::pow(std::clamp(nits / 10000.f, 0.f, 1.f), 2610.f / 16384.f);
  return std::pow((3424.f / 4096.f + (2413.f / 128.f) * p)
                      / (1.f + (2392.f / 128.f) * p),
                  2523.f / 32.f);
}

inline float PqToNits(float code) {
  const float e = std::pow(std::clamp(code, 0.f, 1.f), 32.f / 2523.f);
  return 10000.f
         * std::pow(std::max(e - 3424.f / 4096.f, 0.f)
                        / (2413.f / 128.f - (2392.f / 128.f) * e),
                    16384.f / 2610.f);
}

// Display-coded RGB, 16 bits a channel, in the swapchain's own signal: sRGB
// codes for SDR, PQ BT.2020 codes for HDR.  Every file derives from it, so
// PNG and JPEG carry the same exposure by construction.
struct CodedImage {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint16_t> rgb;  // row-major R, G, B
};

// Swapchain formats whose codes are exact at 8 bits: their 8-bit files need
// no dither.
inline bool EightBitFormat(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
         || format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

inline bool ReadbackToCoded(
    const PendingCapture& capture,
    ID3D12Resource* readback,
    CodedImage* image,
    bool* any_nonzero,
    bool* unwritten) {
  if (readback == nullptr || capture.width == 0 || capture.height == 0
      || capture.row_pitch == 0)
    return false;
  uint32_t bytes_per_pixel = 0;
  switch (capture.format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R11G11B10_FLOAT:
      bytes_per_pixel = 4;
      break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
      bytes_per_pixel = 8;
      break;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
      bytes_per_pixel = 16;
      break;
    default:
      return false;
  }
  D3D12_RANGE range{0, static_cast<SIZE_T>(capture.total_bytes)};
  void* mapped = nullptr;
  if (FAILED(readback->Map(0, &range, &mapped)) || mapped == nullptr) return false;
  *unwritten = !capture.stage_planes && Unwritten(mapped, capture.total_bytes);
  if (*unwritten) {
    readback->Unmap(0, nullptr);
    return false;
  }

  bool saw_nonzero = false;
  image->width = capture.width;
  image->height = capture.height;
  image->rgb.assign(static_cast<size_t>(capture.width) * capture.height * 3u, 0);
  // Rows advance by the full-resource row pitch; the crop base lands once.
  const uint8_t* base = static_cast<const uint8_t*>(mapped)
      + (capture.hdr_plan ? 0 : static_cast<size_t>(capture.crop_y) * capture.row_pitch
                               + static_cast<size_t>(capture.crop_x) * bytes_per_pixel);
  for (uint32_t y = 0; y < capture.height; ++y) {
    const uint8_t* row = base + static_cast<size_t>(y) * capture.row_pitch;
    for (uint32_t x = 0; x < capture.width; ++x) {
      float r = 0.f, g = 0.f, b = 0.f;
      switch (capture.format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
          r = row[x * 4u] / 255.f; g = row[x * 4u + 1] / 255.f;
          b = row[x * 4u + 2] / 255.f;
          break;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
          b = row[x * 4u] / 255.f; g = row[x * 4u + 1] / 255.f;
          r = row[x * 4u + 2] / 255.f;
          break;
        case DXGI_FORMAT_R10G10B10A2_UNORM: {
          const uint32_t v = *reinterpret_cast<const uint32_t*>(row + x * 4u);
          r = (v & 0x3ffu) / 1023.f; g = ((v >> 10) & 0x3ffu) / 1023.f;
          b = ((v >> 20) & 0x3ffu) / 1023.f;
          break;
        }
        case DXGI_FORMAT_R11G11B10_FLOAT: {
          const uint32_t v = *reinterpret_cast<const uint32_t*>(row + x * 4u);
          r = DecodeUnsignedFloat(v & 0x7ffu, 6);
          g = DecodeUnsignedFloat((v >> 11) & 0x7ffu, 6);
          b = DecodeUnsignedFloat((v >> 22) & 0x3ffu, 5);
          break;
        }
        case DXGI_FORMAT_R16G16B16A16_FLOAT: {
          const auto* q = reinterpret_cast<const uint16_t*>(row + x * 8u);
          r = HalfToFloat(q[0]); g = HalfToFloat(q[1]);
          b = HalfToFloat(q[2]);
          break;
        }
        case DXGI_FORMAT_R32G32B32A32_FLOAT: {
          const auto* q = reinterpret_cast<const float*>(row + x * 16u);
          r = q[0]; g = q[1]; b = q[2];
          break;
        }
        default:
          readback->Unmap(0, nullptr);
          image->rgb.clear();
          return false;
      }

      if (!std::isfinite(r)) r = 0.f;
      if (!std::isfinite(g)) g = 0.f;
      if (!std::isfinite(b)) b = 0.f;
      if (capture.hdr_mode == 1) {
        // Preserve scRGB's negative wide-gamut channels until after the
        // BT.709 -> BT.2020 matrix. Clamp only at the PQ output boundary.
        const float r2020 = 0.627404f * r + 0.329283f * g + 0.043313f * b;
        const float g2020 = 0.069097f * r + 0.919540f * g + 0.011362f * b;
        const float b2020 = 0.016391f * r + 0.088013f * g + 0.895595f * b;
        r = LinearNitsToPq(r2020 * capture.linear_unit_nits);
        g = LinearNitsToPq(g2020 * capture.linear_unit_nits);
        b = LinearNitsToPq(b2020 * capture.linear_unit_nits);
      }
      // Game alpha does not describe screenshot transparency: no alpha.
      uint16_t* dst = image->rgb.data()
          + (static_cast<size_t>(y) * capture.width + x) * 3u;
      dst[0] = static_cast<uint16_t>(std::lround(std::clamp(r, 0.f, 1.f) * 65535.f));
      dst[1] = static_cast<uint16_t>(std::lround(std::clamp(g, 0.f, 1.f) * 65535.f));
      dst[2] = static_cast<uint16_t>(std::lround(std::clamp(b, 0.f, 1.f) * 65535.f));
      // Nonzero at 8 bits, the precision an SDR file keeps.
      if (dst[0] >= 129 || dst[1] >= 129 || dst[2] >= 129) saw_nonzero = true;
    }
  }
  readback->Unmap(0, nullptr);
  if (any_nonzero != nullptr) *any_nonzero = saw_nonzero;
  return true;
}

// ---------------------------------------------------------------------------
// HDR signaling for sharing.  Discord shows an upload through an 8-bit WebP
// preview that keeps an ICC profile but has no native CICP, and Chromium
// (Discord's client and every browser that opens the original) reads an ICC
// cicp tag as the HDR signal (Chromium issue 40239687).  So the HDR files
// carry the same PQ BT.2020 codes the game presented, plus this profile.
//
// ICC v4.4 display profile.  cicp = 9/16/0/1: BT.2020 primaries, ST 2084,
// RGB, full range - the same tuple as the PNG cICP chunk.  A viewer that
// ignores cicp uses the matrix/TRC: BT.2020 primaries (Bradford-adapted to
// the D50 PCS) and PQ read against a 203-nit reference white with a soft
// roll-off above 75 %.  That fallback only describes the pixels for an SDR
// viewer; the pixels themselves are never tone mapped.
inline const std::vector<uint8_t>& Bt2100PqIcc() {
  static const std::vector<uint8_t> kProfile = [] {
    const auto s15 = [](std::vector<uint8_t>& out, double value) {
      AppendPngBe32(out, static_cast<uint32_t>(static_cast<int32_t>(std::lround(value * 65536.0))));
    };
    const auto signature = [](const char* name) {
      return ReadBe32(reinterpret_cast<const uint8_t*>(name));
    };
    const auto element = [&](const char* type) {
      std::vector<uint8_t> out;
      AppendPngBe32(out, signature(type));
      AppendPngBe32(out, 0);
      return out;
    };
    const auto text = [&](const char* ascii) {
      std::vector<uint8_t> out = element("mluc");
      const size_t length = std::strlen(ascii);
      AppendPngBe32(out, 1);   // one record
      AppendPngBe32(out, 12);  // record size
      out.insert(out.end(), {'e', 'n', 'U', 'S'});
      AppendPngBe32(out, static_cast<uint32_t>(length * 2));
      AppendPngBe32(out, 28);  // string offset from the element start
      for (size_t i = 0; i < length; ++i) {
        out.push_back(0);
        out.push_back(static_cast<uint8_t>(ascii[i]));
      }
      return out;
    };
    const auto xyz = [&](double x, double y, double z) {
      std::vector<uint8_t> out = element("XYZ ");
      s15(out, x);
      s15(out, y);
      s15(out, z);
      return out;
    };
    // Bradford D65 -> D50, and BT.2020 RGB -> XYZ (D65).
    constexpr double kBradford[3][3] = {{1.0478112, 0.0228866, -0.0501270},
                                        {0.0295424, 0.9904844, -0.0170491},
                                        {-0.0092345, 0.0150436, 0.7521316}};
    constexpr double kBt2020ToXyz[3][3] = {{0.6369580, 0.1446169, 0.1688810},
                                           {0.2627002, 0.6779981, 0.0593017},
                                           {0.0000000, 0.0280727, 1.0609851}};
    double adapted[3][3] = {};
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        for (int k = 0; k < 3; ++k) adapted[i][j] += kBradford[i][k] * kBt2020ToXyz[k][j];
      }
    }
    std::vector<uint8_t> chad = element("sf32");
    for (const auto& row : kBradford) {
      for (const double value : row) s15(chad, value);
    }
    std::vector<uint8_t> curve = element("curv");
    constexpr uint32_t kCurvePoints = 1024;
    AppendPngBe32(curve, kCurvePoints);
    for (uint32_t i = 0; i < kCurvePoints; ++i) {
      const double relative =
          PqToNits(static_cast<float>(i) / (kCurvePoints - 1)) / 203.0;
      const double shown = relative <= 0.75
          ? relative : 0.75 + 0.25 * (1.0 - std::exp(-(relative - 0.75) / 0.25));
      const auto code = static_cast<uint16_t>(std::lround(std::clamp(shown, 0.0, 1.0) * 65535.0));
      curve.push_back(static_cast<uint8_t>(code >> 8));
      curve.push_back(static_cast<uint8_t>(code & 0xff));
    }
    std::vector<uint8_t> cicp = element("cicp");
    cicp.insert(cicp.end(), {9, 16, 0, 1});
    const std::vector<uint8_t> desc = text("BT.2100 PQ (" NR_NEXUS_NAME " screenshot)");
    const std::vector<uint8_t> cprt = text("No copyright, use freely");
    const std::vector<uint8_t> wtpt = xyz(0.9642, 1.0, 0.8249);
    const std::vector<uint8_t> red = xyz(adapted[0][0], adapted[1][0], adapted[2][0]);
    const std::vector<uint8_t> green = xyz(adapted[0][1], adapted[1][1], adapted[2][1]);
    const std::vector<uint8_t> blue = xyz(adapted[0][2], adapted[1][2], adapted[2][2]);
    const std::pair<const char*, const std::vector<uint8_t>*> tags[] = {
        {"desc", &desc}, {"cprt", &cprt}, {"wtpt", &wtpt},   {"chad", &chad},
        {"rXYZ", &red},  {"gXYZ", &green}, {"bXYZ", &blue},  {"rTRC", &curve},
        {"gTRC", &curve}, {"bTRC", &curve}, {"cicp", &cicp}};

    std::vector<uint8_t> profile(128, 0);
    profile[8] = 0x04;  // version 4.4
    profile[9] = 0x40;
    std::memcpy(&profile[12], "mntr", 4);
    std::memcpy(&profile[16], "RGB ", 4);
    std::memcpy(&profile[20], "XYZ ", 4);
    const uint8_t created[12] = {0x07, 0xEA, 0, 9, 0, 26, 0, 0, 0, 0, 0, 0};
    std::memcpy(&profile[24], created, sizeof(created));
    std::memcpy(&profile[36], "acsp", 4);
    const uint8_t d50[12] = {0, 0, 0xF6, 0xD6, 0, 1, 0, 0, 0, 0, 0xD3, 0x2D};
    std::memcpy(&profile[68], d50, sizeof(d50));
    AppendPngBe32(profile, static_cast<uint32_t>(std::size(tags)));
    const size_t data_start = profile.size() + 12 * std::size(tags);
    std::vector<uint8_t> data;
    const std::vector<uint8_t>* previous = nullptr;
    uint32_t offset = 0;
    for (const auto& [name, element_data] : tags) {
      if (element_data != previous) {  // the three TRCs share one element
        while (data.size() % 4 != 0) data.push_back(0);
        offset = static_cast<uint32_t>(data_start + data.size());
        data.insert(data.end(), element_data->begin(), element_data->end());
        previous = element_data;
      }
      AppendPngBe32(profile, signature(name));
      AppendPngBe32(profile, offset);
      AppendPngBe32(profile, static_cast<uint32_t>(element_data->size()));
    }
    while (data.size() % 4 != 0) data.push_back(0);
    profile.insert(profile.end(), data.begin(), data.end());
    const auto size = static_cast<uint32_t>(profile.size());
    for (int i = 0; i < 4; ++i) profile[i] = static_cast<uint8_t>(size >> (24 - 8 * i));
    return profile;
  }();
  return kProfile;
}

// CTA-861.3 MaxCLL and MaxFALL of one PQ image, in nits: the brightest
// pixel's largest channel, and the frame average of each pixel's largest.
// Written as PNG cLLi, so a viewer maps the capture to its own display from
// what the frame holds instead of assuming a 10,000-nit master.
struct ContentLight {
  float max_cll = 0.f;
  float max_fall = 0.f;
};

inline ContentLight MeasureContentLight(const CodedImage& image) {
  static const std::vector<float> kNits = [] {
    std::vector<float> nits(65536);
    for (uint32_t code = 0; code < nits.size(); ++code) nits[code] = PqToNits(code / 65535.f);
    return nits;
  }();
  ContentLight light;
  double sum = 0.0;
  for (size_t i = 0; i + 2 < image.rgb.size(); i += 3) {
    const float nits = kNits[std::max({image.rgb[i], image.rgb[i + 1], image.rgb[i + 2]})];
    light.max_cll = std::max(light.max_cll, nits);
    sum += nits;
  }
  if (!image.rgb.empty()) light.max_fall = static_cast<float>(sum / (image.rgb.size() / 3));
  return light;
}

// Rebuilds a PNG with this capture's colour chunks right after IHDR, and
// without any colour chunk the encoder wrote.  Empty when the stream is
// malformed.  SDR: sRGB.  HDR: cICP (which conforming decoders prefer),
// iCCP with the same signal (for Discord's preview), cLLi.
inline std::vector<uint8_t> WithColorChunks(
    const std::vector<uint8_t>& png, bool hdr, const ContentLight& light) {
  static constexpr uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  if (png.size() < 8 || std::memcmp(png.data(), kSignature, 8) != 0) return {};
  std::vector<uint8_t> out(png.begin(), png.begin() + 8);
  out.reserve(png.size() + Bt2100PqIcc().size() + 256);
  bool placed = false;
  size_t at = 8;
  while (at + 12 <= png.size()) {
    const uint32_t length = ReadBe32(png.data() + at);
    if (length > png.size() - at - 12) return {};
    const char* type = reinterpret_cast<const char*>(png.data() + at + 4);
    const size_t end = at + 12 + length;
    bool colour = false;
    for (const char* owned : {"sRGB", "gAMA", "cHRM", "iCCP", "cICP", "cLLi", "mDCv"}) {
      colour = colour || std::memcmp(type, owned, 4) == 0;
    }
    if (!colour) out.insert(out.end(), png.begin() + at, png.begin() + end);
    at = end;
    if (std::memcmp(type, "IHDR", 4) == 0 && !placed) {
      placed = true;
      if (!hdr) {
        AppendPngChunk(out, "sRGB", {0});
        continue;
      }
      AppendPngChunk(out, "cICP", {9, 16, 0, 1});
      static constexpr char kName[] = "BT.2100 PQ";
      std::vector<uint8_t> iccp(kName, kName + sizeof(kName));  // name + NUL
      iccp.push_back(0);  // deflate
      const std::vector<uint8_t> zlib = ZlibStored(Bt2100PqIcc().data(), Bt2100PqIcc().size());
      iccp.insert(iccp.end(), zlib.begin(), zlib.end());
      AppendPngChunk(out, "iCCP", iccp);
      std::vector<uint8_t> clli;  // units of 0.0001 cd/m2
      AppendPngBe32(clli, static_cast<uint32_t>(std::lround(light.max_cll * 10000.0)));
      AppendPngBe32(clli, static_cast<uint32_t>(std::lround(light.max_fall * 10000.0)));
      AppendPngChunk(out, "cLLi", clli);
    }
    if (std::memcmp(type, "IEND", 4) == 0) return placed ? out : std::vector<uint8_t>{};
  }
  return {};
}

// Adds the profile as an APP2 ICC_PROFILE segment after SOI and APP0 (JFIF).
// Empty when the stream is not a JPEG.
inline std::vector<uint8_t> WithIccSegment(const std::vector<uint8_t>& jpeg) {
  const std::vector<uint8_t>& icc = Bt2100PqIcc();
  if (jpeg.size() < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) return {};
  size_t at = 2;
  while (at + 4 <= jpeg.size() && jpeg[at] == 0xFF && jpeg[at + 1] == 0xE0) {
    at += 2 + ((static_cast<size_t>(jpeg[at + 2]) << 8) | jpeg[at + 3]);
  }
  static constexpr char kMarker[] = "ICC_PROFILE";  // with its NUL
  const size_t length = 2 + sizeof(kMarker) + 2 + icc.size();
  if (at > jpeg.size() || length > 0xFFFF) return {};
  std::vector<uint8_t> out(jpeg.begin(), jpeg.begin() + at);
  out.reserve(jpeg.size() + length + 2);
  out.insert(out.end(), {0xFF, 0xE2, static_cast<uint8_t>(length >> 8),
                         static_cast<uint8_t>(length & 0xff)});
  out.insert(out.end(), kMarker, kMarker + sizeof(kMarker));
  out.insert(out.end(), {1, 1});  // chunk 1 of 1
  out.insert(out.end(), icc.begin(), icc.end());
  out.insert(out.end(), jpeg.begin() + at, jpeg.end());
  return out;
}

// Triangular dither in (-1, 1) LSB from a hash of the position.
inline float DitherOffset(uint32_t x, uint32_t y, uint32_t channel) {
  uint32_t h = x * 0x8da6b343u ^ y * 0xd8163841u ^ (channel + 1u) * 0xcb1ab31fu;
  h ^= h >> 16;
  h *= 0x7feb352du;
  h ^= h >> 15;
  h *= 0x846ca68bu;
  h ^= h >> 16;
  return static_cast<float>((h & 0xffffu) + (h >> 16)) / 65535.f - 1.f;
}

// 8-bit BGR for WIC (every JPEG, the SDR PNG).  `dither` is for sources
// finer than 8 bits: an 8-bit PQ step is visible in dark gradients and sky,
// and the dither trades it for noise at the level JPEG's quantizer already
// adds.  The position hash repeats every `period` columns, so both halves of
// a side-by-side file carry identical noise.  8-bit sources are exact.
inline std::vector<uint8_t> ToBgr8(const CodedImage& image, bool dither, uint32_t period) {
  std::vector<uint8_t> bgr(image.rgb.size());
  for (uint32_t y = 0; y < image.height; ++y) {
    for (uint32_t x = 0; x < image.width; ++x) {
      const size_t at = (static_cast<size_t>(y) * image.width + x) * 3u;
      for (uint32_t channel = 0; channel < 3; ++channel) {
        float value = image.rgb[at + channel] / 257.f;
        if (dither) value += DitherOffset(x % period, y, channel);
        bgr[at + 2u - channel] = static_cast<uint8_t>(std::clamp(std::lround(value), 0l, 255l));
      }
    }
  }
  return bgr;
}

inline CodedImage SideBySide(const CodedImage& left, const CodedImage& right) {
  CodedImage both;
  both.width = left.width + right.width;
  both.height = std::min(left.height, right.height);
  both.rgb.reserve(static_cast<size_t>(both.width) * both.height * 3u);
  for (uint32_t y = 0; y < both.height; ++y) {
    for (const CodedImage* half : {&left, &right}) {
      const auto row = half->rgb.begin() + static_cast<ptrdiff_t>(y) * half->width * 3;
      both.rgb.insert(both.rgb.end(), row, row + static_cast<ptrdiff_t>(half->width) * 3);
    }
  }
  return both;
}

// One WIC encode into memory: JPEG from 24bpp BGR at `quality` (0-1, 4:4:4
// chroma where the OS offers it - *chroma_444 says whether it did), or PNG
// from 24bpp BGR or 48bpp RGB with adaptive filtering.  False on any
// failure; the caller falls back.  4:2:0 would smear UI text and red/blue
// edges, exactly what a comparison is looked at for.
inline bool WicEncode(
    IWICImagingFactory* factory, bool jpeg, uint32_t width, uint32_t height,
    const void* pixels, bool sixteen, float quality, std::vector<uint8_t>* out,
    bool* chroma_444 = nullptr) {
  using Microsoft::WRL::ComPtr;
  using CreateStreamFn = HRESULT(WINAPI*)(HGLOBAL, BOOL, LPSTREAM*);
  static const auto create_stream =
      renodx::utils::png::internal::LoadModuleProc<CreateStreamFn>(L"ole32.dll", "CreateStreamOnHGlobal");
  // wincodec.h's GUIDs, spelled out: their definitions live in a library
  // this addon does not link (as utils/png.hpp does).
  static constexpr GUID kJpegContainer = {0x19e4a5aa, 0x5662, 0x4fc5, {0xa0, 0xc0, 0x17, 0x58, 0x02, 0x8e, 0x10, 0x57}};
  static constexpr GUID kPngContainer = {0x1b7cfaf4, 0x713f, 0x473c, {0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf}};
  static constexpr GUID kBgr24 = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0c}};
  static constexpr GUID kRgb48 = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x15}};
  if (factory == nullptr || create_stream == nullptr || (jpeg && sixteen)) return false;
  ComPtr<IStream> stream;
  if (FAILED(create_stream(nullptr, TRUE, &stream))) return false;
  ComPtr<IWICBitmapEncoder> encoder;
  if (FAILED(factory->CreateEncoder(jpeg ? kJpegContainer : kPngContainer, nullptr, &encoder))
      || FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) {
    return false;
  }
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> options;
  if (FAILED(encoder->CreateNewFrame(&frame, &options))) return false;
  PROPBAG2 option{};
  VARIANT value{};
  if (jpeg) {
    option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    value.vt = VT_R4;
    value.fltVal = quality;
    if (FAILED(options->Write(1, &option, &value))) return false;
    option.pstrName = const_cast<LPOLESTR>(L"JpegYCrCbSubsampling");
    value.vt = VT_UI1;
    value.bVal = 3;  // WICJpegYCrCbSubsampling444 (Windows 8.1+)
    const bool full_chroma = SUCCEEDED(options->Write(1, &option, &value));
    if (chroma_444 != nullptr) *chroma_444 = full_chroma;
  } else {
    option.pstrName = const_cast<LPOLESTR>(L"FilterOption");
    value.vt = VT_UI1;
    value.bVal = 6;  // WICPngFilterAdaptive; the encoder's default otherwise
    options->Write(1, &option, &value);
  }
  const GUID wanted = sixteen ? kRgb48 : kBgr24;
  WICPixelFormatGUID format = wanted;
  const UINT stride = width * (sixteen ? 6u : 3u);
  if (FAILED(frame->Initialize(options.Get())) || FAILED(frame->SetSize(width, height))
      || FAILED(frame->SetPixelFormat(&format)) || !InlineIsEqualGUID(format, wanted)
      || FAILED(frame->WritePixels(height, stride, stride * height,
                                   static_cast<BYTE*>(const_cast<void*>(pixels))))
      || FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
    return false;
  }
  STATSTG stat{};
  const LARGE_INTEGER start{};
  if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart == 0
      || stat.cbSize.QuadPart > 0x7fffffffull
      || FAILED(stream->Seek(start, STREAM_SEEK_SET, nullptr))) {
    return false;
  }
  out->resize(static_cast<size_t>(stat.cbSize.QuadPart));
  ULONG read = 0;
  return SUCCEEDED(stream->Read(out->data(), static_cast<ULONG>(out->size()), &read))
         && read == out->size();
}

// The PNG without WIC: stored deflate blocks, larger but never lost.
inline std::vector<uint8_t> StoredPng(const CodedImage& image, const std::vector<uint8_t>* bgr8) {
  const bool sixteen = bgr8 == nullptr;
  std::vector<uint8_t> raw;
  raw.reserve(static_cast<size_t>(image.height) * (1u + image.width * (sixteen ? 6u : 3u)));
  for (uint32_t y = 0; y < image.height; ++y) {
    raw.push_back(0);  // PNG filter: none
    for (uint32_t x = 0; x < image.width; ++x) {
      const size_t at = (static_cast<size_t>(y) * image.width + x) * 3u;
      for (uint32_t channel = 0; channel < 3; ++channel) {
        if (sixteen) {
          raw.push_back(static_cast<uint8_t>(image.rgb[at + channel] >> 8));
          raw.push_back(static_cast<uint8_t>(image.rgb[at + channel] & 0xff));
        } else {
          raw.push_back((*bgr8)[at + 2u - channel]);
        }
      }
    }
  }
  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  std::vector<uint8_t> ihdr;
  AppendPngBe32(ihdr, image.width);
  AppendPngBe32(ihdr, image.height);
  ihdr.insert(ihdr.end(), {static_cast<uint8_t>(sixteen ? 16 : 8), 2, 0, 0, 0});
  AppendPngChunk(png, "IHDR", ihdr);
  AppendPngChunk(png, "IDAT", ZlibStored(raw.data(), raw.size()));
  AppendPngChunk(png, "IEND", {});
  return png;
}

// A finished PNG: RGB16 PQ for HDR, RGB8 for SDR (from `bgr8`), WIC deflate
// when WIC is available (*compressed), stored blocks otherwise, and this
// capture's colour chunks either way.
inline std::vector<uint8_t> EncodePng(
    IWICImagingFactory* factory, const CodedImage& image, bool hdr,
    const std::vector<uint8_t>& bgr8, const ContentLight& light, bool* compressed) {
  std::vector<uint8_t> png;
  *compressed = WicEncode(factory, false, image.width, image.height,
                          hdr ? static_cast<const void*>(image.rgb.data()) : bgr8.data(),
                          hdr, 0.f, &png);
  if (*compressed) {
    std::vector<uint8_t> tagged = WithColorChunks(png, hdr, light);
    if (!tagged.empty()) return tagged;
    *compressed = false;
  }
  return WithColorChunks(StoredPng(image, hdr ? nullptr : &bgr8), hdr, light);
}

inline bool WriteBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) return false;
  file.write(reinterpret_cast<const char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  return file.good();
}

// The panel's own UI captures (dlssnr.hpp): RGBA8 in, an sRGB RGB8 PNG out.
inline bool WritePngRgba8(const std::filesystem::path& path,
                          uint32_t width, uint32_t height,
                          const std::vector<uint8_t>& rgba) {
  if (width == 0 || height == 0 || rgba.size() != static_cast<size_t>(width) * height * 4u) {
    return false;
  }
  CodedImage image;
  image.width = width;
  image.height = height;
  std::vector<uint8_t> bgr(static_cast<size_t>(width) * height * 3u);
  for (size_t pixel = 0; pixel < bgr.size() / 3u; ++pixel) {
    bgr[pixel * 3u] = rgba[pixel * 4u + 2u];
    bgr[pixel * 3u + 1u] = rgba[pixel * 4u + 1u];
    bgr[pixel * 3u + 2u] = rgba[pixel * 4u];
  }
  renodx::utils::png::internal::ScopedComInitialization com;
  bool compressed = false;
  return WriteBytes(path, EncodePng(com.IsUsable()
                                        ? renodx::utils::png::internal::CreateWicFactory().Get()
                                        : nullptr,
                                    image, false, bgr, {}, &compressed));
}


// Every file of one capture, encoded and written; true when the pair was.
// The pair is all or nothing: a lone half is removed.  The side-by-side
// file is extra, and its failure is logged, never a lost pair.  JPEGs share
// one quality - the requested one, lowered in steps of 3 (to 85, or the
// request when lower) until each file fits the size cap; the image is never
// downscaled.  A pair at two qualities would bias the comparison.  Appends
// what it did to *meta.
inline bool WriteCaptureFiles(
    const PendingCapture& work, const CodedImage& pre, const CodedImage& post,
    const std::filesystem::path& base, FinishedCapture* record, std::string* meta) {
  const bool hdr = work.hdr_mode != 0;
  const bool dither = hdr || !EightBitFormat(work.format);
  renodx::utils::png::internal::ScopedComInitialization com;
  const Microsoft::WRL::ComPtr<IWICImagingFactory> factory =
      com.IsUsable() && !work.hdr_plan ? renodx::utils::png::internal::CreateWicFactory() : nullptr;
  // JPEG needs WIC; without it the capture is still kept, as PNG.
  const bool want_jpeg = work.output.format != FileFormat::kPng && factory != nullptr;
  const bool want_png = work.output.format != FileFormat::kJpeg || !want_jpeg;
  if (work.output.format != FileFormat::kPng && !want_jpeg) {
    SafeLog(reshade::log::level::warning,
            "NR screenshot: JPEG needs the Windows Imaging Component, which did not load;"
            " writing PNG instead");
  }

  struct Picture {
    const wchar_t* suffix;
    const CodedImage* image;
    bool pair;
    std::vector<uint8_t> bgr8;
    ContentLight light;
  };
  CodedImage both;
  std::vector<Picture> pictures;
  pictures.push_back({work.model_diagnostic ? L"_MODEL_INPUT" : L"_NR_OFF", &pre, true, {}, {}});
  pictures.push_back({work.model_diagnostic ? L"_MODEL_OUTPUT" : L"", &post, true, {}, {}});
  if (work.output.side_by_side) {
    both = SideBySide(pre, post);
    pictures.push_back({work.model_diagnostic ? L"_MODEL_COMPARE" : L"_COMPARE", &both, false, {}, {}});
  }
  for (Picture& picture : pictures) {
    if (want_jpeg || !hdr) picture.bgr8 = ToBgr8(*picture.image, dither, pre.width);
    if (hdr) picture.light = MeasureContentLight(*picture.image);
  }

  struct File {
    std::filesystem::path path;
    std::vector<uint8_t> bytes;
    bool pair;
  };
  std::vector<File> files;
  bool compressed = true;
  if (want_png) {
    for (const Picture& picture : pictures) {
      bool wic = false;
      files.push_back({base.wstring() + picture.suffix + L".png",
                       EncodePng(factory.Get(), *picture.image, hdr, picture.bgr8, picture.light, &wic),
                       picture.pair});
      compressed = compressed && wic;
    }
  }
  const uint32_t requested = std::clamp(work.output.jpeg_quality, 80u, 100u);
  const uint32_t lowest = std::min(85u, requested);
  const uint64_t budget = static_cast<uint64_t>(work.output.max_megabytes) * 1000000ull;
  uint32_t pair_quality = 0;
  uint32_t compare_quality = 0;
  bool chroma_444 = false;
  bool over_budget = false;
  if (want_jpeg) {
    for (const bool pair : {true, false}) {
      std::vector<File> encoded;
      for (uint32_t quality = requested;; quality = std::max(lowest, quality - 3u)) {
        encoded.clear();
        bool fits = true;
        for (const Picture& picture : pictures) {
          if (picture.pair != pair) continue;
          std::vector<uint8_t> jpeg;
          if (WicEncode(factory.Get(), true, picture.image->width, picture.image->height,
                        picture.bgr8.data(), false, quality / 100.f, &jpeg, &chroma_444)
              && hdr) {
            jpeg = WithIccSegment(jpeg);
          }
          if (jpeg.empty()) {
            SafeLog(reshade::log::level::error, "NR screenshot JPEG encode failed");
            encoded.clear();
            break;
          }
          fits = fits && (budget == 0 || jpeg.size() <= budget);
          encoded.push_back({base.wstring() + picture.suffix + L".jpg", std::move(jpeg), pair});
        }
        if (encoded.empty() || fits || quality == lowest) {
          uint32_t& used = pair ? pair_quality : compare_quality;
          used = encoded.empty() ? 0 : quality;
          over_budget = over_budget || (!encoded.empty() && !fits);
          break;
        }
      }
      if (pair && encoded.empty() && !want_png) return false;  // nothing to write
      for (File& file : encoded) files.push_back(std::move(file));
    }
  }

  bool pair_ok = true;
  bool extra_ok = true;
  std::string names;
  std::string sizes;
  for (const File& file : files) {
    bool& ok = file.pair ? pair_ok : extra_ok;
    ok = WriteBytes(file.path, file.bytes) && ok;
    const std::string name = PathToLogString(file.path.filename());
    names += (names.empty() ? "" : ",") + name;
    sizes += (sizes.empty() ? "" : ",") + name + ":" + std::to_string(file.bytes.size());
  }
  if (!pair_ok) {
    // Never leave a lone A image without its B counterpart.
    for (const File& file : files) {
      std::error_code ignored;
      if (file.pair) std::filesystem::remove(file.path, ignored);
    }
    return false;
  }
  if (!extra_ok) {
    SafeLog(reshade::log::level::warning, "NR screenshot side-by-side file could not be written");
  }
  // The report bundles the lossless pair when there is one.
  const std::wstring primary = want_png ? L".png" : L".jpg";
  record->pre_image = base.wstring() + pictures[0].suffix + primary;
  record->post_image = base.wstring() + pictures[1].suffix + primary;
  if (over_budget) {
    SafeLog(reshade::log::level::warning,
            "NR screenshot JPEG is larger than NRScreenshotMaxMB="
                + std::to_string(work.output.max_megabytes) + " even at quality "
                + std::to_string(lowest) + "; written anyway (the image is never downscaled)");
  }
  SafeLog(reshade::log::level::info,
          "NR screenshot files: " + sizes
              + (want_jpeg ? ", JPEG quality " + std::to_string(pair_quality)
                                 + (chroma_444 ? " 4:4:4" : " (encoder-default chroma)")
                           : std::string())
              + (hdr ? ", HDR PQ BT.2020 signalled by cICP and ICC cicp 9/16/0/1" : ", SDR sRGB"));

  std::string lines = std::string("file_format=")
      + (want_png && want_jpeg ? "png+jpeg" : want_png ? "png" : "jpeg")
      + "\nlayout=" + (work.output.side_by_side ? "pair+side-by-side" : "pair")
      + "\nfiles=" + names + "\nbytes=" + sizes
      + "\npng_bits=" + (hdr ? "16" : "8")
      + "\npng_encoder=" + (!want_png ? "none" : compressed ? "wic-deflate" : "stored")
      + "\ndither_8bit=" + (dither ? "1" : "0");
  if (want_jpeg) {
    lines += "\njpeg_quality=" + std::to_string(pair_quality)
             + "\njpeg_requested_quality=" + std::to_string(requested)
             + "\njpeg_budget_bytes=" + std::to_string(budget)
             + "\njpeg_over_budget=" + (over_budget ? "1" : "0")
             + "\njpeg_chroma=" + (chroma_444 ? "444" : "encoder-default")
             + "\njpeg_icc=" + (hdr ? "bt2100-pq-cicp-9-16-0-1" : "none-srgb");
    if (work.output.side_by_side) lines += "\njpeg_compare_quality=" + std::to_string(compare_quality);
  }
  if (hdr) {
    char light[160];
    std::snprintf(light, sizeof(light),
                  "\nmax_cll_nits=%.1f,%.1f\nmax_fall_nits=%.1f,%.1f",
                  pictures[0].light.max_cll, pictures[1].light.max_cll,
                  pictures[0].light.max_fall, pictures[1].light.max_fall);
    lines += light;
  }
  *meta += lines + "\n";
  return true;
}

// ---------------------------------------------------------------------------
// Worker thread. Owns everything after the hand-off; must never throw across
// its boundary.

inline void ProcessJob(std::optional<PendingCapture> work) noexcept {
  if (!work.has_value()) return;
  struct BusyReset {
    bool stage_planes;
    bool hdr;
    ~BusyReset() {
      if (hdr) outstanding_jobs.fetch_sub(1);
      worker_busy.store(outstanding_jobs.load() != 0, std::memory_order_release);
      if (stage_planes) stage_busy.store(false, std::memory_order_release);
    }
  } busy_reset{.stage_planes = work->stage_planes, .hdr = work->hdr_plan.has_value()};
  struct SequenceResult {
    PendingCapture* capture;
    bool written = false;
    ~SequenceResult() {
      if (!capture->hdr_plan || !capture->hdr_plan->sequence) return;
      std::scoped_lock lock(state_mutex);
      for (auto& frame : sequence.frames) {
        if (frame.generation == capture->creation_generation && frame.outcome == SequenceOutcome::ACCEPTED) {
          frame.outcome = (capture->aborted ? SequenceOutcome::CANCELLED
                                          : (written ? SequenceOutcome::WRITTEN : SequenceOutcome::INVALID));
        }
      }
      QueueSequenceManifest();
    }
  } sequence_result{.capture = &*work};
  FinishedCapture record;
  record.serial = work->serial;
  record.model_diagnostic = work->model_diagnostic;
  try {
    if (work->stage_planes && !work->metadata_only
        && (!work->completion_proven || !CaptureCompletionProven(*work) || !StageSubmissionCompleted(&*work))) {
      PendingCapture metadata = *work;
      metadata.metadata_only = true;
      metadata.completion_proven = false;
      metadata.pre_nr_readback = nullptr;
      metadata.nr_output_readback = nullptr;
      metadata.temporary_resources.clear();
      metadata.reservation.reset();
      metadata.native_receiver.reset();
      metadata.meta += "\ncapture_error=worker_completion_or_submission_unconfirmed\n";
      for (auto& plane : metadata.planes) {
        plane.readback = nullptr;
        if (plane.error.empty()) plane.error = "gpu_unconfirmed";
      }
      RetiredCaptureBuffers retained;
      retained.Take(std::move(*work));
      {
        std::scoped_lock lock(state_mutex);
        retired_buffers.push_back(std::move(retained));
        retained_sets.fetch_add(1, std::memory_order_relaxed);
      }
      *work = std::move(metadata);
    }
    if (!work->stage_planes && (work->aborted || !work->ready)) {
      if (work->aborted) {
        SafeLog(reshade::log::level::info, "NR screenshot aborted; no files written");
      } else {
        SafeLog(reshade::log::level::error, "NR screenshot not ready; no files written");
      }
      // Settled on the same proof as a delivered capture (this path used to
      // return without releasing them).
      ReleaseReadbacks(*work);
      record.outcome = work->aborted ? "aborted" : "not_ready";
      PublishFinished(std::move(record));
      return;
    }

    // All filesystem work happens here on the worker, never on the render or
    // present thread. Wide paths only: narrowing to ANSI can throw.
    std::filesystem::path directory;
    try {
      directory =
          renodx::utils::path::GetReShadeBasePath() / std::filesystem::path(L"DLSS5 Screenshots");
      std::error_code ec;
      std::filesystem::create_directories(directory, ec);
    } catch (...) {
      directory.clear();
    }

    std::filesystem::path base = directory.empty()
        ? std::filesystem::path(
              std::wstring(work->model_diagnostic ? L"NR_DIAGNOSTIC_" : L"NR_CAPTURE_")
              + std::to_wstring(work->timestamp_ms) + L"_"
              + std::to_wstring(work->serial))
        : directory / std::filesystem::path(
              std::wstring(work->model_diagnostic ? L"NR_DIAGNOSTIC_" : L"NR_CAPTURE_")
              + std::to_wstring(work->timestamp_ms) + L"_"
              + std::to_wstring(work->serial));
    CodedImage pre_image;
    CodedImage post_image;
    bool pre_nonzero = false;
    bool post_nonzero = false;
    bool pre_unwritten = false;
    bool post_unwritten = false;
    const bool read_ok =
        !work->metadata_only && !work->aborted && work->ready
        && ReadbackToCoded(*work, work->pre_nr_readback, &pre_image, &pre_nonzero,
                           &pre_unwritten)
        && ReadbackToCoded(*work, work->nr_output_readback, &post_image, &post_nonzero,
                           &post_unwritten);
    if (!work->stage_planes && (pre_unwritten || post_unwritten)) {
      captures_unexecuted.fetch_add(1, std::memory_order_relaxed);
      SafeLog(
          reshade::log::level::warning,
          std::string("NR screenshot refused: the GPU had not run its copies when"
                      " they were read")
              + (work->exact
                     ? " (the command list that recorded them was reset without"
                       " being submitted)"
                     : " (this session has no exact submission proof, so the"
                       " buffers stay allocated)")
              + "; no files written - take the screenshot again");
      if (work->exact) {
        ReleaseReadbacks(*work);
      } else {
        RetiredCaptureBuffers held;
        held.Take(std::move(*work));
        std::scoped_lock lock(state_mutex);
        held_unexecuted.push_back(std::move(held));
        retained_sets.fetch_add(1, std::memory_order_relaxed);
      }
      record.outcome = "unexecuted";
      PublishFinished(std::move(record));
      return;
    }
    if (read_ok && (!pre_nonzero || !post_nonzero)) {
      // The copies ran (the stamp is gone), so the frame itself was black.
      // Said anyway, so a black PNG is never a mystery.
      SafeLog(
          reshade::log::level::warning,
          std::string("NR screenshot pair contains an entirely black image")
              + (!pre_nonzero ? " [pre-NR]" : "")
              + (!post_nonzero ? " [NR on]" : "")
              + "; the GPU ran the copies, so the captured frame itself was"
                " black");
    }
    std::string meta = work->meta;
    if (work->request_kind == RequestKind::PRE_FG_SERVICE) {
      work->required_planes_complete = ValidatePreFgPlanes(*work, &meta);
      const bool receivers_verified = VerifyPreFgReceivers(*work);
      work->required_planes_complete = work->required_planes_complete && receivers_verified;
      meta += "role_receivers_native_byte_verified=" + std::to_string(receivers_verified) + "\n";
    }
    if (work->hdr_plan && work->request_kind != RequestKind::PRE_FG_SERVICE) {
      bool complete = work->required_planes_complete;
      if (work->hdr_plan->sequence && !work->sequence_contract_validated) {
        complete = false;
        meta += "capture_error=sequence_contract_missing\n";
      }
      constexpr std::array names{"hdr_original_linear", "hdr_pre_commit", "hdr_post_pedestal_linear",
                                 "hdr_post_gamut_linear", "hdr_encoded_float", "packed_final",
                                 "hdr_receiver_roi", "hdr_block_statistics", "hdr_pedestal_field"};
      for (const char* name : names) {
        const auto found = std::find_if(work->planes.begin(), work->planes.end(),
                                       [name](const auto& plane) { return plane.name == name; });
        const bool skipped_gamut = found != work->planes.end() && found->explicitly_skipped
                                   && ((std::string_view(name) == "hdr_post_gamut_linear" && work->hdr_mode == 1)
                                       || (std::string_view(name) == "hdr_block_statistics"
                                           && (found->error == "legacy_measurement_disabled"
                                               || found->error == "legacy_predicate_disabled")));
        if (skipped_gamut && std::string_view(name) == "hdr_block_statistics") {
          meta += "pedestal_statistics_replayable=0\n";
        }
        const bool valid = found != work->planes.end() && found->recorded && found->error.empty();
        if (!valid && !skipped_gamut) {
          complete = false;
          meta += "required_plane." + std::string(name) + "=missing_or_invalid\n";
        }
      }
      if (work->native_receiver_required
          && (!work->native_receiver || !work->native_receiver->completed)) complete = false;
      work->required_planes_complete = complete;
      meta += "capture_schema=2\nfull_chain_validated=0\npost_restore_semantics=pre_commit_alias\n"
              "temporary_reserved_bytes=" + std::to_string(work->temporary_bytes)
              + "\ncpu_export_reserved_bytes=" + std::to_string(work->cpu_export_bytes)
              + "\naggregate_peak_bytes=" + std::to_string(aggregate_peak_bytes.load()) + "\n";
      meta += "readback_aggregate_peak_bytes=" + std::to_string(readback_peak_bytes.load())
              + "\ntemporary_aggregate_peak_bytes=" + std::to_string(temporary_peak_bytes.load())
              + "\ncpu_export_aggregate_peak_bytes=" + std::to_string(cpu_peak_bytes.load())
              + "\ntemporary_budget_bytes=" + std::to_string(MAX_HDR_TEMPORARY_BYTES)
              + "\ncpu_export_budget_bytes=" + std::to_string(MAX_HDR_CPU_EXPORT_BYTES) + "\n";
    }
    if (work->stage_planes) {
      meta += "\npair_output_ready=" + std::to_string(work->ready)
              + "\npair_aborted=" + std::to_string(work->aborted)
              + "\npair_unwritten=" + std::to_string(pre_unwritten || post_unwritten) + "\n";
    }
    const bool wrote = read_ok
        && WriteCaptureFiles(*work, pre_image, post_image, base, &record, &meta);
    if (work->hdr_plan && !wrote) work->required_planes_complete = false;
    if (work->hdr_plan) meta += "hdr_export_options=session_only_png_roi_pair_stored_encoder\n";
    record.outcome = (work->metadata_only ? "gpu_unconfirmed" : (wrote ? "written" : "write_failed"));
    if (work->request_kind == RequestKind::PRE_FG_SERVICE) record.outcome = "incomplete";
    if (wrote) {
      SafeLog(
          reshade::log::level::info,
          "NR screenshot pair written: " + PathToLogString(record.pre_image) + " and "
              + PathToLogString(record.post_image));
    } else {
      SafeLog(
          reshade::log::level::error,
          "NR screenshot readback or image write failed; no pair written ("
              + PathToLogString(directory)
              + "). Check the folder exists and is writable, and that disk"
                " space is available");
    }
    // Raw float planes + sidecar meta: the exposure/units evidence the PNGs
    // cannot carry.  Row-major float32 RGBA, no header; dims live in the meta.
    if (work->stage_planes || !work->planes.empty()) {
      meta += work->rejection_meta;
      meta += "\ncapture_serial=" + std::to_string(work->serial)
              + "\ncapture_generation=" + std::to_string(work->creation_generation)
              + "\ncompletion_generation=" + std::to_string(work->completion_generation)
              + "\ncompletion_proven=" + std::to_string(work->completion_proven)
              + "\ncompletion_proof=" + (work->completion_proven ? (work->exact ? "exact_submission_resource_releasable" : "nonempty_completed_lease") : "none")
              + "\nreadback_allocated_bytes=" + std::to_string(work->allocated_bytes)
              + "\nreadback_budget_bytes=" + std::to_string(MAX_STAGE_READBACK_BYTES)
              + "\nrejected_plane_requests=" + std::to_string(work->rejected_planes)
              + "\nplane_request_limit_reason=max_24_planes_or_invalid_name\n";
    }
    for (const DiagnosticPlane& plane : work->planes) {
      const std::string prefix = "plane." + plane.name + ".";
      meta += prefix + "format_dxgi=" + std::to_string(plane.format) + "\n"
              + prefix + "source_format="
              + std::to_string(plane.source_format != DXGI_FORMAT_UNKNOWN ? plane.source_format : plane.format) + "\n"
              + prefix + "footprint_format=" + std::to_string(plane.footprint.Footprint.Format) + "\n"
              + prefix + "width=" + std::to_string(plane.width) + "\n"
              + prefix + "height=" + std::to_string(plane.height) + "\n"
              + prefix + "footprint_width=" + std::to_string(plane.footprint.Footprint.Width) + "\n"
              + prefix + "footprint_height=" + std::to_string(plane.footprint.Footprint.Height) + "\n"
              + prefix + "row_pitch=" + std::to_string(plane.footprint.Footprint.RowPitch) + "\n"
              + prefix + "readback_bytes=" + std::to_string(plane.readback != nullptr ? plane.total_bytes : 0) + "\n"
              + prefix + "recorded=" + std::to_string(plane.recorded) + "\n"
              + prefix + "copy_submission_completed="
              + std::to_string(!plane.submission_proof.empty() && GpuLeaseCompleted(plane.submission_proof)) + "\n"
              + prefix + "code_domain="
              + (plane.format == DXGI_FORMAT_R32_UINT ? "uint32_statistics_no_color_conversion"
                 : (DiagnosticPixelBytes(plane.format) == 0 ? "unsupported"
                                                         : (plane.format == DXGI_FORMAT_R16G16B16A16_FLOAT || plane.format == DXGI_FORMAT_R32G32B32A32_FLOAT
                                                                || plane.format == DXGI_FORMAT_R16_FLOAT || plane.format == DXGI_FORMAT_R32_FLOAT
                                                                || plane.format == DXGI_FORMAT_R16G16_FLOAT || plane.format == DXGI_FORMAT_R32G32_FLOAT
                                                                || plane.format == DXGI_FORMAT_R32_TYPELESS || plane.format == DXGI_FORMAT_D32_FLOAT
                                                                ? "stored_float_no_tonemap"
                                                                : "normalized_unorm_no_srgb_decode")))
              + "\n";
      std::string error = plane.error;
      std::filesystem::path plane_path = base;
      plane_path += L"_" + std::wstring(plane.name.begin(), plane.name.end())
          + L".f32";
      std::error_code plane_ec;
      std::filesystem::remove(plane_path, plane_ec);
      std::ofstream out(plane_path, std::ios::binary | std::ios::trunc);
      bool plane_ok = out.good() && plane.recorded && plane.readback != nullptr
                      && error.empty() && DiagnosticFootprintValid(plane) && !work->metadata_only && !work->aborted;
      std::filesystem::path raw_path = base;
      raw_path += L"_" + std::wstring(plane.name.begin(), plane.name.end()) + L".raw";
      const bool preserve_raw = work->request_kind == RequestKind::PRE_FG_SERVICE
          || plane.name == "packed_final" || plane.name == "hdr_receiver_roi";
      std::ofstream raw;
      if (preserve_raw && plane_ok) {
        raw.open(raw_path, std::ios::binary | std::ios::trunc);
      }
      bool raw_ok = preserve_raw && plane_ok && raw.good();
      uint64_t hash = 14695981039346656037ull;
      uint64_t exported_bytes = 0;
      if (plane_ok) {
        // Allocation precedes Map so an exception cannot strand a mapping.
        std::vector<float> row(static_cast<size_t>(plane.width) * 4u);
        void* mapped = nullptr;
        if (SUCCEEDED(plane.readback->Map(0, nullptr, &mapped))
            && mapped != nullptr) {
          plane_ok = work->stage_planes || !Unwritten(mapped, plane.total_bytes);
          raw_ok = raw_ok && plane_ok;
          const auto* base_row = static_cast<const uint8_t*>(mapped) + plane.footprint.Offset;
          if (work->request_kind == RequestKind::PRE_FG_SERVICE && plane.name == "compose_statistics") {
            uint32_t words[3]{};
            std::memcpy(words, base_row, sizeof(words));
            work->compose_statistics_verified = words[1] == work->compose_serial
                && words[0] <= work->compose_area && words[2] <= work->compose_area;
            meta += "compose.current_gpu_count=" + std::to_string(words[0])
                + "\ncompose.current_gpu_serial=" + std::to_string(words[1])
                + "\ncompose.current_gpu_coarse_count=" + std::to_string(words[2])
                + "\ncompose.current_statistics_verified=" + std::to_string(work->compose_statistics_verified) + "\n";
            const char* actual = (work->compose_mode == 0 ? "native_alpha"
                : (work->compose_mode == 1 ? "ui_rgba_alpha"
                   : (words[0] <= work->compose_hud_limit ? "mask"
                      : (work->compose_residual && words[2] <= work->compose_hud_limit ? "residual" : "enhanced_whole"))));
            meta += "compose.actual_gpu_operation=" + std::string(actual)
                + "\ncompose.used_for_FG=" + std::to_string(work->compose_used) + "\n";
          }
          for (uint32_t y = 0; y < plane.height && plane_ok; ++y) {
            const auto* src = base_row + static_cast<size_t>(y) * plane.footprint.Footprint.RowPitch;
            const size_t native_bytes = static_cast<size_t>(plane.width) * DiagnosticPixelBytes(plane.format);
            for (size_t byte = 0; byte < native_bytes; ++byte) { hash ^= src[byte]; hash *= 1099511628211ull; }
            exported_bytes += native_bytes;
            plane_ok = DecodeDiagnosticRow(src, plane.width, plane.format, row.data());
            if (!plane_ok) break;
            out.write(
                reinterpret_cast<const char*>(row.data()),
                static_cast<std::streamsize>(row.size() * sizeof(float)));
            plane_ok = out.good();
            if (raw_ok) {
              raw.write(reinterpret_cast<const char*>(src),
                        static_cast<std::streamsize>(plane.width) * DiagnosticPixelBytes(plane.format));
              raw_ok = raw.good();
            }
          }
          plane.readback->Unmap(0, nullptr);
        } else {
          plane_ok = false;
        }
      }
      out.close();
      plane_ok = plane_ok && !out.fail();
      if (preserve_raw) {
        raw.close();
        raw_ok = raw_ok && !raw.fail();
      }
      if (!plane_ok) {
        std::filesystem::remove(plane_path, plane_ec);
        if (error.empty()) error = (plane.recorded ? "unwritten_map_or_output_failed" : "copy_not_recorded");
      }
      if (!raw_ok) {
        std::filesystem::remove(raw_path, plane_ec);
        if (preserve_raw && error.empty()) error = "raw_output_failed";
      }
      if (plane.required && !plane.explicitly_skipped && (!plane_ok || (preserve_raw && !raw_ok))) {
        work->required_planes_complete = false;
      }
      meta += prefix + "output_success=" + std::to_string(plane_ok) + "\n"
              + prefix + "roi_x=" + std::to_string(plane.source_rect.x) + "\n"
              + prefix + "roi_y=" + std::to_string(plane.source_rect.y) + "\n"
              + prefix + "source_width=" + std::to_string(plane.source_width) + "\n"
              + prefix + "source_height=" + std::to_string(plane.source_height) + "\n"
              + prefix + "full_width=" + std::to_string(plane.full_width) + "\n"
              + prefix + "full_height=" + std::to_string(plane.full_height) + "\n"
              + prefix + "required=" + std::to_string(plane.required) + "\n"
              + prefix + "native_hash_fnv1a64=" + std::to_string(hash) + "\n"
              + prefix + "native_byte_count=" + std::to_string(exported_bytes) + "\n"
              + prefix + "raw_output_success=" + std::to_string(raw_ok) + "\n"
              + prefix + "raw_layout=tight_rows_native_dxgi_little_endian\n"
              + prefix + "skipped=" + std::to_string(!plane.recorded || work->metadata_only || plane.readback == nullptr) + "\n"
              + prefix + "error=" + error + "\n";
      if (plane_ok) {
        SafeLog(
            reshade::log::level::info,
            "NR diagnostic plane written: " + PathToLogString(plane_path)
                + " (" + plane.name + ", " + std::to_string(plane.width) + "x"
                + std::to_string(plane.height) + " f32)");
      } else {
        if (plane.required && !plane.explicitly_skipped) work->required_planes_complete = false;
        SafeLog(
            reshade::log::level::error,
            "NR diagnostic plane write failed: " + PathToLogString(plane_path));
      }
    }
    if (work->hdr_plan && work->native_receiver_required) {
      auto receiver = work->native_receiver;
      bool success = receiver && receiver->completed && receiver->error.empty() && !work->metadata_only && !work->aborted;
      std::string error = (receiver ? receiver->error : "native_receiver_missing");
      uint64_t hash = 14695981039346656037ull, bytes = 0;
      auto raw_path = base; raw_path += L"_hdr_native_receiver_roi.raw";
      auto float_path = base; float_path += L"_hdr_native_receiver_roi.f32";
      if (success) {
        std::ofstream raw(raw_path, std::ios::binary | std::ios::trunc);
        std::ofstream floats(float_path, std::ios::binary | std::ios::trunc);
        std::vector<float> row(static_cast<size_t>(receiver->rect.width) * 4);
        ID3D11Multithread* guard = nullptr;
        receiver->context->QueryInterface(IID_PPV_ARGS(&guard));
        if (guard != nullptr) guard->Enter();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT mapped_result = receiver->context->Map(receiver->staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (SUCCEEDED(mapped_result)) {
          const size_t row_bytes = static_cast<size_t>(receiver->rect.width) * DiagnosticPixelBytes(receiver->format);
          success = mapped.pData != nullptr && mapped.RowPitch >= row_bytes
                    && static_cast<uint64_t>(mapped.RowPitch) * receiver->rect.height <= receiver->bytes;
          for (uint32_t y = 0; y < receiver->rect.height && success; ++y) {
            const auto* source = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
            success = DecodeDiagnosticRow(source, receiver->rect.width, receiver->format, row.data());
            for (size_t i = 0; i < row_bytes; ++i) { hash ^= source[i]; hash *= 1099511628211ull; }
            bytes += row_bytes;
            raw.write(reinterpret_cast<const char*>(source), row_bytes);
            floats.write(reinterpret_cast<const char*>(row.data()), row.size() * sizeof(float));
            success = success && raw.good() && floats.good();
          }
          receiver->context->Unmap(receiver->staging, 0);
        } else { success = false; error = "native_receiver_nonblocking_map_failed"; }
        if (guard != nullptr) { guard->Leave(); guard->Release(); }
        raw.close(); floats.close(); success = success && !raw.fail() && !floats.fail();
      }
      if (!success) {
        std::error_code ec;
        std::filesystem::remove(raw_path, ec); std::filesystem::remove(float_path, ec);
        work->required_planes_complete = false;
        if (error.empty()) error = "native_receiver_completion_or_export_missing";
      }
      meta += "native_receiver.output_success=" + std::to_string(success)
              + "\nnative_receiver.completion_proof=" + (receiver && receiver->completed ? "d3d11_event_donotflush" : "missing")
              + "\nnative_receiver.native_hash_fnv1a64=" + std::to_string(hash)
              + "\nnative_receiver.native_byte_count=" + std::to_string(bytes)
              + "\nnative_receiver.error=" + error + "\n";
      if (receiver) {
        meta += "native_receiver.roi_x=" + std::to_string(receiver->rect.x)
                + "\nnative_receiver.roi_y=" + std::to_string(receiver->rect.y)
                + "\nnative_receiver.format_dxgi=" + std::to_string(receiver->format) + "\n";
      }
    }
    if (work->hdr_plan) {
      if (work->request_kind == RequestKind::PRE_FG_SERVICE && work->compose_attempted
          && !work->compose_statistics_verified) {
        work->required_planes_complete = false;
        meta += "capture_error=compose_current_statistics_unconfirmed\n";
      }
      const bool valid = work->required_planes_complete && work->completion_proven && !work->metadata_only && !work->aborted;
      if (work->request_kind == RequestKind::PRE_FG_SERVICE) {
        meta += "pre_fg_planes_complete=" + std::to_string(valid)
            + "\npre_fg_capture_valid=" + std::to_string(valid)
            + "\nfull_pre_fg_chain_complete=" + std::to_string(valid)
            + "\npre_fg_stage_instrumentation_complete=" + std::to_string(valid) + "\n";
        record.outcome = (valid ? "written" : "incomplete");
      }
      meta += "required_planes_complete=" + std::to_string(work->required_planes_complete)
              + "\nhdr_capture_valid=" + std::to_string(valid && work->request_kind != RequestKind::PRE_FG_SERVICE) + "\n";
    }
    if (work->model_diagnostic && work->model_recording_list != nullptr) {
      meta += "required_planes_complete=" + std::to_string(work->required_planes_complete) + "\n";
      if (!work->required_planes_complete) record.outcome = "incomplete";
    }
    if (!meta.empty()) {
      std::filesystem::path meta_path = base;
      meta_path += L".meta.txt";
      std::ofstream meta_out(meta_path, std::ios::trunc);
      if (meta_out.good()) {
        meta_out << meta;
        meta_out.close();
        if (!meta_out.fail()) {
          record.meta = meta_path;
          sequence_result.written = work->required_planes_complete && work->completion_proven
                                    && !work->metadata_only && !work->aborted;
        }
        SafeLog(
            reshade::log::level::info,
            "NR capture meta written: " + PathToLogString(meta_path));
      }
    }
  } catch (const std::exception& ex) {
    record.outcome = "exception";
    SafeLog(reshade::log::level::error, std::string("NR screenshot exception: ") + ex.what());
  } catch (...) {
    record.outcome = "exception";
    SafeLog(reshade::log::level::error, "NR screenshot unknown exception");
  }
  PublishFinished(std::move(record));
  ReleaseReadbacks(*work);
}

inline void WorkerLoop() noexcept {
  // Pin the addon module for the lifetime of this thread.  The worker is
  // detached by design (see the header contract), so a mid-session FreeLibrary
  // of the addon - a ReShade addon reload - would otherwise unmap the code
  // this loop is executing.  The pinned reference is deliberately never
  // released; the OS reclaims it at process end.
  HMODULE pinned_module = nullptr;
  GetModuleHandleExW(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
      reinterpret_cast<LPCWSTR>(&WorkerLoop),
      &pinned_module);
  for (;;) {
    std::optional<PendingCapture> work;
    {
      std::unique_lock lock(job_mutex);
      job_signal.wait(lock, [] {
        return shutdown_requested.load(std::memory_order_acquire) || job.has_value() || !hdr_jobs.empty()
               || sequence_manifest_dirty.load();
      });
      if (job) { work = std::move(*job); job.reset(); }
      else if (!hdr_jobs.empty()) { work = std::move(hdr_jobs.front()); hdr_jobs.pop_front(); }
      else if (!sequence_manifest_dirty.load()) return;
    }
    if (work) ProcessJob(std::move(work));
    sequence_manifest_busy.store(true);
    if (sequence_manifest_dirty.exchange(false)) {
      try {
        SequenceState snapshot;
        {
          std::scoped_lock lock(state_mutex);
          snapshot = sequence;
        }
        const auto directory = renodx::utils::path::GetReShadeBasePath() / L"DLSS5 Screenshots";
        std::filesystem::create_directories(directory);
        const auto path = directory / (L"HDR_SEQUENCE_" + std::to_wstring(snapshot.id) + L".manifest.txt");
        auto temporary = path;
        temporary += L".tmp";
        std::ofstream output(temporary, std::ios::trunc);
        output << "capture_schema=2\nsequence_id=" << snapshot.id << "\nactive=" << snapshot.active
               << "\nattempted=" << snapshot.attempted << "\ncontract_signature=" << snapshot.contract_signature << '\n';
        output << "roi_x=" << snapshot.plan.roi.x << "\nroi_y=" << snapshot.plan.roi.y
               << "\nroi_width=" << snapshot.plan.roi.width << "\nroi_height=" << snapshot.plan.roi.height
               << "\nfull_width=" << snapshot.full_width << "\nfull_height=" << snapshot.full_height
               << "\nformat_dxgi=" << snapshot.format << '\n';
        constexpr std::array outcomes{"ACCEPTED", "DROPPED", "INVALID", "CANCELLED", "RESIZE", "WRITTEN"};
        for (uint32_t i = 0; i < snapshot.attempted; ++i) {
          output << "frame." << snapshot.frames[i].generation << '='
                 << outcomes[static_cast<size_t>(snapshot.frames[i].outcome)] << '\n';
        }
        output.close();
        if (output.fail() || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
          throw std::runtime_error("sequence manifest write failed");
        }
        sequence_manifest_failed.store(false);
      } catch (...) {
        sequence_manifest_failed.store(true);
        SafeLog(reshade::log::level::error, "HDR sequence manifest write failed");
      }
    }
    sequence_manifest_busy.store(false);
  }
}

// Hands a settled capture to the worker. Called on the present thread with no
// other lock held; must not throw.
inline void HandoffToWorker(std::optional<PendingCapture> work) noexcept {
  if (!work.has_value()) return;
  if (shutdown_requested.load(std::memory_order_acquire)) {
    ReleaseReadbacks(*work);
    worker_busy.store(false, std::memory_order_release);
    if (work->stage_planes) stage_busy.store(false, std::memory_order_release);
    return;
  }
  {
    std::scoped_lock lock(job_mutex);
    if (work->hdr_plan) {
      outstanding_jobs.fetch_add(1);
      hdr_jobs.push_back(std::move(*work));
    } else if (job.has_value()) {
      // A newer capture supersedes one the worker has not picked up yet.
      ReleaseReadbacks(*job);
      job.reset();
      SafeLog(reshade::log::level::info, "NR screenshot superseded before write");
    }
    if (!work->hdr_plan) job = std::move(work);
  }
  if (!worker_started.exchange(true, std::memory_order_acq_rel)) {
    bool created = false;
    try {
      std::thread(WorkerLoop).detach();
      created = true;
    } catch (...) {
    }
    if (!created) {
      // No worker available: degrade to synchronous completion rather than
      // leaving the feature wedged. One present-thread stall, never a crash.
      worker_started.store(false, std::memory_order_release);
      std::optional<PendingCapture> fallback;
      std::deque<PendingCapture> failed_hdr;
      {
        std::scoped_lock lock(job_mutex);
        fallback = std::move(job);
        job.reset();
        failed_hdr.swap(hdr_jobs);
      }
      for (auto& capture : failed_hdr) {
        const auto serial = capture.serial;
        ReleaseReadbacks(capture);
        outstanding_jobs.fetch_sub(1);
        PublishFinished({.serial = serial, .outcome = "worker_unavailable"});
      }
      if (fallback.has_value() && fallback->stage_planes) {
        ReleaseReadbacks(*fallback);  // settled proof; no render-thread file IO
        worker_busy.store(false, std::memory_order_release);
        stage_busy.store(false, std::memory_order_release);
        FinishedCapture failed{.serial = fallback->serial, .outcome = "worker_unavailable"};
        PublishFinished(std::move(failed));
      } else {
        ProcessJob(std::move(fallback));
      }
      return;
    }
  }
  job_signal.notify_one();
}

}  // namespace internal

// ---------------------------------------------------------------------------
// Public API. Every entry point is noexcept-safe; callers on the render path
// (ProcessInline) hold dlssnr's runtime_mutex, and state_mutex is always
// nested inside it, never the other way around.

inline bool IsArmed() {
  return internal::capture_requested.load(std::memory_order_acquire);
}

inline bool WantsFinalPresent() {
  return IsArmed() && internal::requested_kind.load(std::memory_order_relaxed) == RequestKind::FINAL_PRESENTATION
      && internal::requested_source.load(std::memory_order_relaxed) == SourceMode::kFinalPresent;
}

inline bool WantsEvaluatedBuffer() {
  return IsArmed() && internal::requested_kind.load(std::memory_order_relaxed) == RequestKind::FINAL_PRESENTATION
      && internal::requested_source.load(std::memory_order_relaxed) == SourceMode::kEvaluatedBuffer;
}

// The request must precede the OUTER game evaluation. A request that arrives
// after pre-SR NR already ran cannot claim that evaluation's later guide copy
// is an untouched source. Nested NGX/Streamline forwards keep the outer stamp.
inline thread_local uint32_t evaluation_depth = 0;
inline thread_local uint64_t evaluation_request = 0;
struct EvaluationScope {
  EvaluationScope() {
    if (evaluation_depth++ == 0) {
      evaluation_request = IsArmed()
          ? internal::request_id.load(std::memory_order_acquire) : 0;
    }
  }
  ~EvaluationScope() {
    if (--evaluation_depth == 0) evaluation_request = 0;
  }
};

inline bool HasPending() {
  return internal::has_pending.load(std::memory_order_acquire);
}

inline bool HdrCaptureSlotsFull(size_t slots) {
  return slots >= internal::MAX_HDR_IN_FLIGHT;
}

using SequenceOutcome = internal::SequenceOutcome;
struct SequenceStatus {
  bool active = false;
  uint64_t id = 0;
  bool manifest_failed = false;
  uint32_t attempted = 0;
  std::array<internal::SequenceFrame, 120> frames{};
};

inline bool ConfigureHdrCapture(const HdrCapturePlan& plan) {
  if (plan.roi.width == 0 || plan.roi.height == 0
      || plan.roi.x > UINT32_MAX - plan.roi.width || plan.roi.y > UINT32_MAX - plan.roi.height) return false;
  std::scoped_lock lock(internal::state_mutex);
  if (internal::capture_requested.load() && internal::requested_kind.load() == RequestKind::PRE_FG_SERVICE) return false;
  if (internal::pending || !internal::hdr_in_flight.empty() || internal::sequence.active
      || internal::aggregate_bytes.load() != 0 || internal::outstanding_jobs.load() != 0
      || internal::sequence_manifest_dirty.load() || internal::sequence_manifest_busy.load()
      || internal::sequence_manifest_failed.load()) return false;
  internal::configured_hdr_plan = plan;
  return true;
}

inline std::optional<HdrCapturePlan> PendingHdrCapturePlan() {
  std::scoped_lock lock(internal::state_mutex);
  return internal::pending ? internal::pending->hdr_plan : std::nullopt;
}

inline bool StartHdrSequence(const HdrCapturePlan& plan, uint64_t first_generation) {
  if (IsArmed() && internal::requested_kind.load() == RequestKind::PRE_FG_SERVICE) return false;
  auto frozen = plan;
  frozen.sequence = true;
  if (!ConfigureHdrCapture(frozen)) return false;
  std::scoped_lock lock(internal::state_mutex);
  if (!internal::worker_started.exchange(true)) {
    try {
      std::thread(internal::WorkerLoop).detach();
    } catch (...) {
      internal::worker_started.store(false);
      return false;
    }
  }
  FILETIME now{};
  GetSystemTimeAsFileTime(&now);
  internal::sequence = {.active = true, .id = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime,
                        .plan = frozen,
                        .next_generation = first_generation};
  internal::QueueSequenceManifest();
  internal::requested_kind.store(RequestKind::FINAL_PRESENTATION);
  internal::requested_source.store(SourceMode::kFinalPresent);
  internal::capture_requested.store(true);
  internal::request_id.fetch_add(1);
  return true;
}

inline SequenceStatus GetHdrSequenceStatus() {
  std::scoped_lock lock(internal::state_mutex);
  return {.active = internal::sequence.active, .id = internal::sequence.id,
          .manifest_failed = internal::sequence_manifest_failed.load(), .attempted = internal::sequence.attempted,
          .frames = internal::sequence.frames};
}

inline void CancelHdrSequence() {
  std::scoped_lock lock(internal::state_mutex);
  auto& sequence = internal::sequence;
  while (sequence.active && sequence.attempted < sequence.frames.size()) {
    sequence.frames[sequence.attempted++] = {.generation = sequence.next_generation++, .outcome = SequenceOutcome::CANCELLED};
  }
  sequence.active = false;
  internal::capture_requested.store(false);
  if (internal::pending && internal::pending->hdr_plan) internal::pending->aborted = true;
  for (auto& capture : internal::hdr_in_flight) capture.aborted = true;
  internal::QueueSequenceManifest();
}

// Called with the stable effective options snapshot after SetPendingDiagnosticMeta.
// Generation, guide serial and ordinary warmup resets must not be in this signature.
inline bool SetHdrSequenceContract(const std::string& signature) {
  std::scoped_lock lock(internal::state_mutex);
  if (!internal::pending || !internal::pending->hdr_plan || !internal::pending->hdr_plan->sequence) return true;
  auto& sequence = internal::sequence;
  if (!signature.empty() && signature.size() <= 4096 && signature.find_first_of("\r\n") == std::string::npos) {
    if (sequence.contract_signature.empty()) {
      sequence.contract_signature = signature;
      internal::pending->sequence_contract_validated = true;
      internal::QueueSequenceManifest();
      return true;
    }
    if (sequence.contract_signature == signature) {
      internal::pending->sequence_contract_validated = true;
      return true;
    }
  }
  for (auto& frame : sequence.frames) {
    if (frame.generation == internal::pending->creation_generation) frame.outcome = SequenceOutcome::INVALID;
  }
  internal::pending->aborted = true;
  internal::pending->required_planes_complete = false;
  internal::pending->meta += "\ncapture_error=sequence_contract_changed_or_invalid\n";
  while (sequence.attempted < sequence.frames.size()) {
    sequence.frames[sequence.attempted++] = {.generation = sequence.next_generation++, .outcome = SequenceOutcome::CANCELLED};
  }
  sequence.active = false;
  internal::capture_requested.store(false);
  internal::QueueSequenceManifest();
  return false;
}

// Call immediately before PruneCompletedGenerations, outside tracker_mutex.
// Lock order remains screenshot state -> submission tracker. No readback or IO.
inline void PreserveStageSubmissionProofs() {
  std::scoped_lock lock(internal::state_mutex);
  for (auto& capture : internal::hdr_in_flight) internal::StageSubmissionCompleted(&capture);
  if (internal::pending.has_value() && internal::pending->stage_planes) {
    internal::StageSubmissionCompleted(&*internal::pending);
  }
}

inline void Disarm() {
  std::scoped_lock lock(internal::state_mutex);
  if (internal::requested_kind.load() == RequestKind::PRE_FG_SERVICE) internal::configured_hdr_plan.reset();
  internal::capture_requested.store(false, std::memory_order_release);
  if (!internal::pending.has_value()) internal::stage_busy.store(false, std::memory_order_release);
}

// Arms the one-shot capture. Ignored while a capture is already armed or
// waiting for its GPU completion window.
inline bool RequestCapture() {
  std::scoped_lock lock(internal::state_mutex);
  if (internal::capture_requested.load(std::memory_order_acquire)
      || internal::pending.has_value()) {
    return false;
  }
  const bool stage = stage_planes_enabled.load(std::memory_order_relaxed);
  if (internal::stage_busy.load(std::memory_order_acquire)) return false;
  if (stage) {
    std::scoped_lock job_lock(internal::job_mutex);
    if (internal::job.has_value() || internal::worker_busy.load(std::memory_order_acquire)
        || internal::retained_sets.load(std::memory_order_relaxed) != 0) return false;
    internal::stage_busy.store(true, std::memory_order_release);
  }
  internal::requested_kind.store(RequestKind::FINAL_PRESENTATION, std::memory_order_relaxed);
  internal::requires_passthrough_frame.store(false, std::memory_order_relaxed);
  internal::requested_source.store(
      ResolveSource(source_mode.load(std::memory_order_relaxed),
                    separate_presentation.load(std::memory_order_relaxed)),
      std::memory_order_relaxed);
  internal::request_id.fetch_add(1, std::memory_order_relaxed);
  const uint32_t format = file_format.load(std::memory_order_relaxed);
  internal::requested_output = {
      format <= 2u ? static_cast<FileFormat>(format) : FileFormat::kPng,
      layout.load(std::memory_order_relaxed) == 1u,
      jpeg_quality.load(std::memory_order_relaxed),
      max_megabytes.load(std::memory_order_relaxed)};
  internal::presents_while_armed.store(0, std::memory_order_relaxed);
  internal::capture_requested.store(true, std::memory_order_release);
  internal::SafeLog(
      reshade::log::level::info,
      WantsFinalPresent()
          ? "NR screenshot armed: one final presentation image, before and after NR"
            " (temporarily uses On Present; HDR stays HDR)"
          : "NR diagnostic armed: evaluated model input/output from one pass;"
            " excludes look, transfer, game tone mapping and UI");
  return true;
}

inline bool WantsPreFgService() {
  return IsArmed() && internal::requested_kind.load(std::memory_order_relaxed) == RequestKind::PRE_FG_SERVICE;
}

// One complete service is bound once; it cannot supersede a previous evaluate.
inline bool RequestPreFgService(const HdrCapturePlan& plan) {
  if (plan.sequence || plan.roi.width == 0 || plan.roi.height == 0
      || plan.roi.x > UINT32_MAX - plan.roi.width || plan.roi.y > UINT32_MAX - plan.roi.height) return false;
  std::scoped_lock lock(internal::state_mutex);
  if (internal::capture_requested.load() || internal::pending || internal::sequence.active
      || internal::stage_busy.load() || !internal::hdr_in_flight.empty()
      || internal::aggregate_bytes.load() != 0 || internal::outstanding_jobs.load() != 0
      || internal::retained_sets.load() != 0 || internal::sequence_manifest_dirty.load()
      || internal::sequence_manifest_busy.load() || internal::sequence_manifest_failed.load()) return false;
  {
    std::scoped_lock job_lock(internal::job_mutex);
    if (internal::job || internal::worker_busy.load()) return false;
  }
  internal::configured_hdr_plan = plan;
  internal::requested_kind.store(RequestKind::PRE_FG_SERVICE);
  internal::request_id.fetch_add(1);
  internal::presents_while_armed.store(0);
  internal::requested_output = {FileFormat::kPng, false, 95u, 0u};
  internal::capture_requested.store(true, std::memory_order_release);
  return true;
}

// Unsupported presentation APIs finish the request explicitly. They must not
// substitute a pre-tonemap NGX texture and call it a displayed NR off/on pair.
inline void FailRequest(const char* outcome, const char* explanation) {
  std::scoped_lock lock(internal::state_mutex);
  if (internal::requested_kind.load() == RequestKind::PRE_FG_SERVICE) internal::configured_hdr_plan.reset();
  if (!internal::capture_requested.exchange(false, std::memory_order_acq_rel) && !internal::pending) return;
  if (internal::pending.has_value()) {
    internal::pending->aborted = true;
    internal::pending->meta += "\ncapture_error=" + std::string(explanation) + "\n";
    if (internal::pending->hdr_plan && internal::pending->hdr_plan->sequence) {
      auto& sequence = internal::sequence;
      while (sequence.attempted < sequence.frames.size()) {
        sequence.frames[sequence.attempted++] = {.generation = sequence.next_generation++, .outcome = SequenceOutcome::CANCELLED};
      }
      sequence.active = false;
      internal::QueueSequenceManifest();
    }
  } else {
    internal::stage_busy.store(false, std::memory_order_release);
  }
  internal::FinishedCapture record;
  record.serial = internal::capture_serial.fetch_add(1) + 1;
  record.outcome = outcome;
  internal::PublishFinished(std::move(record));
  internal::SafeLog(reshade::log::level::warning,
                    std::string("NR screenshot unavailable: ") + explanation);
}

// English; the modern panel draws it through ui::Tr (the Developer view as
// is).
// i18n: begin - display strings (tools/i18n/ui_strings.py catalog)
inline const char* StatusText() {
  if (HasPending()) return "waiting for GPU completion";
  if (WantsPreFgService()) return "armed, waiting for one pre-FG service transaction (partial native diagnostics)";
  if (WantsFinalPresent()) return "armed, waiting for one final presentation frame";
  if (IsArmed()) return "armed, waiting for an evaluated model buffer pair";
  std::scoped_lock lock(internal::finished_mutex);
  const char* outcome = internal::finished.outcome;
  if (std::strcmp(outcome, "written") == 0) {
    return internal::finished.model_diagnostic
        ? "saved: model input/output diagnostic; not the displayed game frame"
        : "saved: same frame before and after NR";
  }
  if (std::strcmp(outcome, "unsupported_present") == 0) return "unavailable on this presentation path";
  if (std::strcmp(outcome, "unsupported_diagnostic") == 0) return "evaluated-buffer capture is unavailable on this API";
  if (std::strcmp(outcome, "unproven_baseline") == 0) return "exact comparison unavailable with this frame-generation route; disable frame generation for the capture";
  if (std::strcmp(outcome, "nr_disabled") == 0) return "enable NR before taking a comparison";
  if (std::strcmp(outcome, "foreign_nr") == 0) return "unavailable: another NR implementation owns this frame";
  if (std::strcmp(outcome, "none") == 0) return "idle";
  return "capture failed; see the log for its cause";
}
// i18n: end

// Edge-detected capture-hotkey poll, called once per present with the
// configurable virtual-key code (owned by dlssnr.hpp) and whether the game
// has the foreground: GetAsyncKeyState is system-wide, so without it the key
// pressed in another application captured the game.  Only a single user32
// call plus two atomic accesses when idle.
inline void PollHotkey(uint32_t virtual_key, bool focused) {
  const bool down = focused
      && (GetAsyncKeyState(static_cast<int>(virtual_key)) & 0x8000) != 0;
  if (!down) {
    internal::hotkey_was_down.store(false, std::memory_order_relaxed);
    return;
  }
  if (internal::hotkey_was_down.exchange(true, std::memory_order_relaxed)) return;
  RequestCapture();
}

// Area (crop dimensions) of the currently pending capture, 0 when none.  The
// evaluate-side gate uses it to keep a larger main-pass capture from being
// superseded by a smaller secondary pass in the same frame.
inline uint64_t PendingCaptureArea() {
  if (!internal::has_pending.load(std::memory_order_acquire)) return 0;
  std::scoped_lock lock(internal::state_mutex);
  if (!internal::pending.has_value()) return 0;
  return static_cast<uint64_t>(internal::pending->width)
      * internal::pending->height;
}

// Allocates the two addon-owned READBACK buffers for one evaluation pair.
// Runs on the render thread with runtime_mutex held; performs no filesystem
// work and can therefore not throw.  width/height are the CAPTURE (crop)
// dimensions; crop_x/crop_y select a subrect when the engine renders into
// part of a larger DLSS output resource (clamped against the resource here).
inline bool PrepareCapture(
    ID3D12Device* device,
    ID3D12Resource* source,
    uint32_t crop_x,
    uint32_t crop_y,
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format,
    uint8_t hdr_mode,
    uint64_t generation,
    float linear_unit_nits = 80.f,
    bool model_diagnostic = false) {
  if (device == nullptr || source == nullptr || width == 0 || height == 0)
    return false;
  const D3D12_RESOURCE_DESC source_desc = source->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT rows = 0;
  UINT64 total_bytes = 0;
  device->GetCopyableFootprints(
      &source_desc, 0, 1, 0, &footprint, &rows, nullptr, &total_bytes);
  if (total_bytes == 0 || footprint.Footprint.RowPitch == 0) return false;
  if (crop_x >= footprint.Footprint.Width
      || crop_y >= footprint.Footprint.Height) {
    return false;
  }
  if (crop_x + width > footprint.Footprint.Width) {
    width = footprint.Footprint.Width - crop_x;
  }
  if (crop_y + height > footprint.Footprint.Height) {
    height = footprint.Footprint.Height - crop_y;
  }
  if (width == 0 || height == 0) return false;

  std::scoped_lock lock(internal::state_mutex);
  const bool prefg = internal::requested_kind.load() == RequestKind::PRE_FG_SERVICE;
  const auto hdr_plan = ((hdr_mode != 0 || prefg) ? internal::configured_hdr_plan : std::nullopt);
  if (prefg && (internal::pending || !internal::capture_requested.load() || !hdr_plan)) return false;
  const bool capture_stages = stage_planes_enabled.load(std::memory_order_relaxed) || hdr_plan.has_value();
  if (hdr_plan) {
    const auto roi = hdr_plan->roi;
    if (roi.x >= source_desc.Width || roi.y >= source_desc.Height
        || roi.width > source_desc.Width - roi.x || roi.height > source_desc.Height - roi.y) return false;
    if (internal::pending || HdrCaptureSlotsFull(internal::hdr_live_sets.load())) return false;
    if (internal::sequence.active) {
      auto& sequence = internal::sequence;
      if (generation != sequence.next_generation || sequence.attempted >= 120) return false;
      if (sequence.full_width == 0) {
        sequence.full_width = static_cast<uint32_t>(source_desc.Width);
        sequence.full_height = source_desc.Height;
        sequence.format = format;
      } else if (sequence.full_width != source_desc.Width || sequence.full_height != source_desc.Height || sequence.format != format) {
        sequence.frames[sequence.attempted++] = {.generation = generation, .outcome = SequenceOutcome::RESIZE};
        ++sequence.next_generation;
        sequence.active = false;
        internal::capture_requested.store(false);
        while (sequence.attempted < sequence.frames.size()) {
          sequence.frames[sequence.attempted++] = {.generation = sequence.next_generation++, .outcome = SequenceOutcome::CANCELLED};
        }
        internal::QueueSequenceManifest();
        return false;
      }
    }
    auto roi_desc = source_desc;
    roi_desc.Width = roi.width;
    roi_desc.Height = roi.height;
    device->GetCopyableFootprints(&roi_desc, 0, 1, 0, &footprint, &rows, nullptr, &total_bytes);
    crop_x = roi.x; crop_y = roi.y; width = roi.width; height = roi.height;
  }
  if (capture_stages) {
    std::scoped_lock job_lock(internal::job_mutex);
    if (internal::pending.has_value() || (!hdr_plan && (internal::job.has_value()
        || internal::worker_busy.load(std::memory_order_acquire)
        || internal::retained_sets.load(std::memory_order_relaxed) != 0))
        || total_bytes > internal::MAX_STAGE_READBACK_BYTES / 2
        || source_desc.Format != format || source_desc.SampleDesc.Count != 1
        || source_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D
        || source_desc.DepthOrArraySize != 1) return false;
  } else if (!internal::pending.has_value()) {
    internal::stage_busy.store(false, std::memory_order_release);
  }
  internal::PendingCapture capture;
  capture.request_kind = internal::requested_kind.load();
  capture.full_width = static_cast<uint32_t>(source_desc.Width);
  capture.full_height = source_desc.Height;
  capture.hdr_plan = hdr_plan;
  if (hdr_plan) {
    capture.reservation = std::make_shared<internal::CaptureReservation>();
    capture.reservation->holds_hdr_slot = true;
    internal::hdr_live_sets.fetch_add(1);
    capture.cpu_row_bytes = static_cast<uint64_t>(width) * 16;
    // Diagnostic sessions use only stored RGB16 PNG ROI pairs. 128 bytes/pixel
    // covers both coded payloads, retained PNGs, raw/zlib/tagging vectors and
    // their growth transients; fixed slack covers profiles, streams and metadata.
    capture.cpu_export_bytes = static_cast<uint64_t>(width) * height * 128 + 262144 + capture.cpu_row_bytes;
    const auto allocation_bytes = internal::ReadbackAllocationBytes(device, total_bytes);
    if (allocation_bytes > internal::MAX_STAGE_READBACK_BYTES / 2
        || !capture.reservation->Reserve(allocation_bytes * 2)) {
      internal::SafeLog(reshade::log::level::warning, "HDR capture refused: aggregate_readback_budget_exceeded");
      return false;
    }
    if (!capture.reservation->ReserveCpu(capture.cpu_export_bytes)) {
      internal::SafeLog(reshade::log::level::warning, "HDR capture refused: cpu_export_budget_exceeded; reduce ROI");
      return false;
    }
  }
  capture.stage_planes = capture_stages;
  capture.allocated_bytes = total_bytes * 2;
  capture.model_diagnostic = model_diagnostic;
  capture.width = width;
  capture.height = height;
  capture.crop_x = crop_x;
  capture.crop_y = crop_y;
  capture.footprint = footprint;
  capture.total_bytes = total_bytes;
  capture.row_pitch = footprint.Footprint.RowPitch;
  capture.format = format;
  capture.hdr_mode = hdr_mode;
  capture.linear_unit_nits = linear_unit_nits;
  capture.timestamp_ms = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  capture.serial = internal::capture_serial.fetch_add(1) + 1;
  capture.creation_generation = generation;

  if (!internal::MakeReadbackBuffer(device, total_bytes, &capture.pre_nr_readback, !capture.stage_planes)
      || !internal::MakeReadbackBuffer(device, total_bytes, &capture.nr_output_readback, !capture.stage_planes)) {
    internal::ReleaseReadbacks(capture);
    return false;
  }
  if (internal::pending.has_value()) {
    // A prior evaluate in this same capture-frame already recorded a pair into
    // its own buffers. Retire that ENTIRE buffer set - the PNG pair and every
    // diagnostic plane (still GPU-pending on the frame's command list) - so it
    // is released at the same settle window as the final capture; the latest
    // evaluate's pair wins.
    internal::RetiredCaptureBuffers retired;
    retired.pre_nr_readback = internal::pending->pre_nr_readback;
    retired.nr_output_readback = internal::pending->nr_output_readback;
    retired.planes = std::move(internal::pending->planes);
    internal::retired_buffers.push_back(std::move(retired));
    internal::retained_sets.fetch_add(1, std::memory_order_relaxed);
  }
  capture.output = internal::requested_output;
  if (hdr_plan) {
    capture.output.format = FileFormat::kPng;
    capture.output.side_by_side = false;
    capture.meta += "hdr_export_options=session_only_png_roi_pair_stored_encoder\n";
  }
  if (hdr_plan && internal::sequence.active) {
    auto& sequence = internal::sequence;
    sequence.frames[sequence.attempted++] = {.generation = generation, .outcome = SequenceOutcome::ACCEPTED};
    ++sequence.next_generation;
    if (sequence.attempted == 120) sequence.active = false;
    internal::QueueSequenceManifest();
  }
  internal::pending = std::move(capture);
  if (prefg) internal::configured_hdr_plan.reset();
  if (capture_stages) internal::stage_busy.store(true, std::memory_order_release);
  internal::has_pending.store(true, std::memory_order_release);
  return true;
}

// Records the pre-NR copy: pre_nr_source must hold the untouched DLSS output
// and be in D3D12_RESOURCE_STATE_COPY_SOURCE when called.
inline void RecordPreCopy(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* pre_nr_source) {
  if (command_list == nullptr || pre_nr_source == nullptr) return;
  std::scoped_lock lock(internal::state_mutex);
  if (!internal::pending.has_value()) return;
  if (internal::pending->stage_planes && internal::pending->pre_recorded) return;
  D3D12_TEXTURE_COPY_LOCATION dst{};
  dst.pResource = internal::pending->pre_nr_readback;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = internal::pending->footprint;
  D3D12_TEXTURE_COPY_LOCATION src{};
  src.pResource = pre_nr_source;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.SubresourceIndex = 0;
  const auto& capture = *internal::pending;
  const D3D12_BOX box{capture.crop_x, capture.crop_y, 0,
                      capture.crop_x + capture.width, capture.crop_y + capture.height, 1};
  command_list->CopyTextureRegion(&dst, 0, 0, 0, &src, capture.hdr_plan ? &box : nullptr);
  submission::TrackUse(command_list, dst.pResource);
  internal::pending->pre_recorded = true;
}

// Records the post-NR copy and completes the pair: nr_source must hold the
// final decoded result and be in D3D12_RESOURCE_STATE_COPY_SOURCE.
inline void RecordPostCopy(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* nr_source) {
  if (command_list == nullptr || nr_source == nullptr) return;
  std::scoped_lock lock(internal::state_mutex);
  if (!internal::pending.has_value()) return;
  if (internal::pending->stage_planes && internal::pending->ready) return;
  D3D12_TEXTURE_COPY_LOCATION dst{};
  dst.pResource = internal::pending->nr_output_readback;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = internal::pending->footprint;
  D3D12_TEXTURE_COPY_LOCATION src{};
  src.pResource = nr_source;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.SubresourceIndex = 0;
  const auto& capture = *internal::pending;
  const D3D12_BOX box{capture.crop_x, capture.crop_y, 0,
                      capture.crop_x + capture.width, capture.crop_y + capture.height, 1};
  command_list->CopyTextureRegion(&dst, 0, 0, 0, &src, capture.hdr_plan ? &box : nullptr);
  submission::TrackUse(command_list, dst.pResource);
  internal::pending->ready = true;
  // Freeze this exact pair immediately. Waiting for a later Present lets
  // another swapchain or generated frame replace one the user requested.
  internal::capture_requested.store(false, std::memory_order_release);
}

// The float planes are a developer's evidence, ini-only (NRScreenshotPlanes,
// set by dlssnr.hpp's LoadConfiguration; off by default since v7.0.0-rc10).
// An HDR capture records four or more of them, each ~130 MB on disk at 4K
// plus its readback heap, which a player's screenshot key should not cost.
inline std::atomic_bool diagnostic_planes_enabled = false;

// Allocates the readback buffer for one named float plane (e.g. the sRGB
// proxy, the resolved work surface).  Call while a capture is pending, before
// the chain records its copies; no-op when no capture is in flight or the
// planes are off, and every plane's copy then records nothing.
inline thread_local const char* prefg_capture_role = nullptr;

struct PreFgRoleScope {
  const char* previous = prefg_capture_role;
  explicit PreFgRoleScope(const char* role) { prefg_capture_role = role; }
  ~PreFgRoleScope() { prefg_capture_role = previous; }
  PreFgRoleScope(const PreFgRoleScope&) = delete;
  PreFgRoleScope& operator=(const PreFgRoleScope&) = delete;
};

inline std::string DiagnosticPlaneName(const char* name) {
  return (prefg_capture_role == nullptr ? std::string(name) : std::string(prefg_capture_role) + "_" + name);
}

inline uint64_t PendingModelCaptureSerial(ID3D12GraphicsCommandList* command_list) {
  if (!stage_planes_enabled.load(std::memory_order_relaxed) || !HasPending()) return 0;
  std::scoped_lock lock(internal::state_mutex);
  if (!internal::pending || !internal::pending->stage_planes || !internal::pending->model_diagnostic
      || command_list == nullptr || internal::pending->model_recording_list != command_list) return 0;
  return internal::pending->serial;
}

inline bool PrepareDiagnosticPlane(
    ID3D12Device* device,
    const char* name,
    ID3D12Resource* texture,
    uint32_t width,
    uint32_t height,
    std::optional<CaptureRect> requested_rect = std::nullopt,
    bool required = false,
    uint64_t expected_capture_serial = 0) {
  if (device == nullptr || name == nullptr || texture == nullptr) return false;
  const std::string canonical_name(name);
  const std::string qualified_name = DiagnosticPlaneName(name);
  name = qualified_name.c_str();
  std::scoped_lock lock(internal::state_mutex);
  if (!internal::pending.has_value()) return false;
  if (expected_capture_serial != 0
      && (internal::pending->serial != expected_capture_serial || !internal::pending->stage_planes
          || !internal::pending->model_diagnostic)) return false;
  if (internal::pending->request_kind == RequestKind::PRE_FG_SERVICE && internal::pending->service_sealed) return false;
  if (!internal::pending->stage_planes && !diagnostic_planes_enabled.load(std::memory_order_relaxed)) return false;
  if (std::strlen(name) == 0 || std::strlen(name) > 63
      || std::strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::strlen(name)) {
    ++internal::pending->rejected_planes;
    return false;
  }
  for (const internal::DiagnosticPlane& plane : internal::pending->planes) {
    if (plane.name == name) return plane.error.empty() && !plane.recorded;
  }
  const D3D12_RESOURCE_DESC desc = texture->GetDesc();
  if (!requested_rect && internal::pending->hdr_plan) {
    requested_rect = HdrPlaneRect(*internal::pending->hdr_plan, canonical_name.c_str(),
                                 internal::pending->full_width, internal::pending->full_height);
    required = true;
  }
  auto footprint_desc = desc;
  if (requested_rect) {
    width = requested_rect->width; height = requested_rect->height;
    footprint_desc.Width = width; footprint_desc.Height = height;
  }
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 total_bytes = 0;
  device->GetCopyableFootprints(&footprint_desc, 0, 1, 0, &footprint, nullptr, nullptr,
                                &total_bytes);
  if (internal::pending->planes.size() >= internal::MAX_STAGE_PLANES) {
    if (internal::pending->rejected_planes < internal::MAX_STAGE_PLANES) {
      const std::string prefix = "plane." + std::string(name) + ".";
      internal::pending->rejection_meta += prefix + "format_dxgi=" + std::to_string(desc.Format) + "\n"
                                           + prefix + "width=" + std::to_string(width) + "\n"
                                           + prefix + "height=" + std::to_string(height) + "\n"
                                           + prefix + "recorded=0\n" + prefix + "output_success=0\n"
                                           + prefix + "skipped=1\n" + prefix + "error=plane_limit_exceeded\n";
    }
    ++internal::pending->rejected_planes;
    return false;
  }
  internal::DiagnosticPlane plane{
      .name = name, .width = width, .height = height, .footprint = footprint, .total_bytes = total_bytes, .format = desc.Format};
  plane.source_format = desc.Format;
  if (desc.Format == DXGI_FORMAT_R32G8X24_TYPELESS || desc.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) {
    // D3D12 subresource 0 is the depth plane: its buffer footprint stores one float per pixel, without stencil.
    plane.format = footprint.Footprint.Format;
  }
  plane.source_width = static_cast<uint32_t>(desc.Width);
  plane.source_identity = reinterpret_cast<uintptr_t>(texture);
  plane.source_height = desc.Height;
  plane.full_width = internal::pending->full_width;
  plane.full_height = internal::pending->full_height;
  plane.required = required;
  plane.region_copy = requested_rect.has_value();
  plane.source_rect = requested_rect.value_or(CaptureRect{.width = width, .height = height});
  plane.copy_rect = plane.source_rect;
  // Shader diagnostic UAVs contain a tight ROI but retain full-grid metadata.
  const std::string_view plane_name(canonical_name);
  const bool diagnostic_uav = plane_name == "hdr_post_pedestal_linear" || plane_name == "hdr_post_gamut_linear"
                              || plane_name == "hdr_encoded_float" || plane_name == "hdr_pedestal_field";
  if (requested_rect && diagnostic_uav && desc.Width == width && desc.Height == height) {
    plane.copy_rect.x = 0; plane.copy_rect.y = 0;
  }
  const uint64_t cpu_bytes = static_cast<uint64_t>(width) * 16;
  const uint64_t extra_cpu = (cpu_bytes > internal::pending->cpu_row_bytes
                                 ? cpu_bytes - internal::pending->cpu_row_bytes : 0);
  if (diagnostic_uav && plane.format != DXGI_FORMAT_R32G32B32A32_FLOAT) {
    plane.error = "hdr_diagnostic_uav_requires_rgba32f";
  } else if (internal::DiagnosticPixelBytes(plane.format) == 0) {
    plane.error = "unsupported_format";
  } else if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D
             || desc.SampleDesc.Count != 1 || desc.DepthOrArraySize != 1
             || !internal::DiagnosticFootprintValid(plane)
             || plane.copy_rect.x >= desc.Width || plane.copy_rect.y >= desc.Height
             || width > desc.Width - plane.copy_rect.x || height > desc.Height - plane.copy_rect.y) {
    plane.error = "invalid_dimensions_or_footprint";
  } else if (total_bytes > internal::MAX_STAGE_READBACK_BYTES
             || internal::pending->allocated_bytes > internal::MAX_STAGE_READBACK_BYTES - total_bytes) {
    plane.error = "readback_budget_exceeded";
  } else if (internal::pending->reservation
             && (!internal::pending->reservation->Reserve(internal::ReadbackAllocationBytes(device, total_bytes))
                 || !internal::pending->reservation->ReserveCpu(extra_cpu))) {
    plane.error = "aggregate_budget_exceeded";
  } else if (!internal::MakeReadbackBuffer(device, total_bytes, &plane.readback, !internal::pending->stage_planes)) {
    plane.error = "allocation_failed";
  } else {
    internal::pending->allocated_bytes += total_bytes;
    internal::pending->cpu_export_bytes += extra_cpu;
    internal::pending->cpu_row_bytes = std::max(internal::pending->cpu_row_bytes, cpu_bytes);
  }
  const bool prepared = plane.error.empty();
  if (required && !prepared) internal::pending->required_planes_complete = false;
  internal::pending->planes.push_back(std::move(plane));
  return prepared;
}

inline bool PrepareHdrPlane(ID3D12Device* device, const char* name, ID3D12Resource* texture,
                            CaptureRect source_rect, bool required = true) {
  return PrepareDiagnosticPlane(device, name, texture, source_rect.width, source_rect.height, source_rect, required);
}

inline bool ReserveHdrTemporaryBytes(uint64_t bytes) {
  std::scoped_lock lock(internal::state_mutex);
  if (!internal::pending || !internal::pending->reservation) return false;
  if (!internal::pending->reservation->ReserveTemporary(bytes)) {
    internal::pending->required_planes_complete = false;
    internal::pending->rejection_meta += "capture_error=temporary_uav_budget_exceeded\n";
    return false;
  }
  internal::pending->temporary_bytes += bytes;
  return true;
}

// Call after successful reservation/allocation, before the UAV dispatch.
// Capture owns an extra reference until every recording using it is retired.
inline void TrackHdrTemporaryResource(ID3D12GraphicsCommandList* command_list, ID3D12Resource* texture) {
  if (command_list == nullptr || texture == nullptr) return;
  std::scoped_lock lock(internal::state_mutex);
  if (!internal::pending || !internal::pending->hdr_plan) return;
  auto& resources = internal::pending->temporary_resources;
  if (std::find(resources.begin(), resources.end(), texture) == resources.end()) {
    texture->AddRef(); resources.push_back(texture);
  }
  submission::TrackUse(command_list, texture);
}

inline void SkipHdrPlane(const char* name, const char* reason) {
  if (name == nullptr || reason == nullptr) return;
  const std::string qualified_name = DiagnosticPlaneName(name);
  name = qualified_name.c_str();
  std::scoped_lock lock(internal::state_mutex);
  if (!internal::pending) return;
  if (internal::pending->request_kind == RequestKind::PRE_FG_SERVICE && internal::pending->service_sealed) return;
  for (auto& plane : internal::pending->planes) {
    if (plane.name == name) { plane.error = reason; plane.explicitly_skipped = true; return; }
  }
  if (internal::pending->planes.size() < internal::MAX_STAGE_PLANES) {
    internal::DiagnosticPlane plane{.name = name};
    plane.error = reason;
    plane.explicitly_skipped = true;
    internal::pending->planes.push_back(std::move(plane));
  } else {
    internal::pending->required_planes_complete = false;
    internal::pending->rejection_meta += "plane." + qualified_name + ".error=plane_limit_exceeded\n";
    ++internal::pending->rejected_planes;
  }
}

// Records one float-plane copy: texture must be in COPY_SOURCE for the call.
inline void RecordDiagnosticPlaneCopy(
    ID3D12GraphicsCommandList* command_list,
    const char* name,
    ID3D12Resource* texture,
    uint64_t expected_capture_serial = 0) {
  if (command_list == nullptr || name == nullptr || texture == nullptr) return;
  const std::string qualified_name = DiagnosticPlaneName(name);
  name = qualified_name.c_str();
  std::scoped_lock lock(internal::state_mutex);
  if (!internal::pending.has_value()) return;
  if (expected_capture_serial != 0
      && (internal::pending->serial != expected_capture_serial || !internal::pending->stage_planes
          || !internal::pending->model_diagnostic
          || internal::pending->model_recording_list != command_list)) return;
  if (internal::pending->request_kind == RequestKind::PRE_FG_SERVICE && internal::pending->service_sealed) return;
  for (internal::DiagnosticPlane& plane : internal::pending->planes) {
    if (plane.name != name || plane.readback == nullptr || plane.recorded) continue;
    const auto desc = texture->GetDesc();
    if (reinterpret_cast<uintptr_t>(texture) != plane.source_identity
        || desc.Format != (plane.source_format != DXGI_FORMAT_UNKNOWN ? plane.source_format : plane.format)
        || desc.Width != plane.source_width
        || desc.Height != plane.source_height || desc.SampleDesc.Count != 1
        || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1) {
      plane.error = "record_source_mismatch";
      return;
    }
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = plane.readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = plane.footprint;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = texture;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    const auto rect = plane.copy_rect;
    const D3D12_BOX box{rect.x, rect.y, 0, rect.x + rect.width, rect.y + rect.height, 1};
    command_list->CopyTextureRegion(&dst, 0, 0, 0, &src, plane.region_copy ? &box : nullptr);
    submission::TrackUse(command_list, plane.readback);
    plane.recorded = true;
    return;
  }
}

// Per-stack-pass diagnostic copy: under a multi-pass stack each pass records
inline void RecordHdrReceiverCopy(ID3D12Device* device, ID3D12GraphicsCommandList* command_list,
                                  ID3D12Resource* output, uint32_t output_x, uint32_t output_y) {
  const auto plan = PendingHdrCapturePlan();
  if (!plan || device == nullptr || command_list == nullptr || output == nullptr) return;
  auto rect = plan->roi;
  if (rect.x > UINT32_MAX - output_x || rect.y > UINT32_MAX - output_y) {
    SkipHdrPlane("hdr_receiver_roi", "output_offset_overflow"); return;
  }
  rect.x += output_x; rect.y += output_y;
  if (!PrepareHdrPlane(device, "hdr_receiver_roi", output, rect)) return;
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {.pResource = output, .Subresource = 0,
                        .StateBefore = D3D12_RESOURCE_STATE_COPY_DEST, .StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE};
  command_list->ResourceBarrier(1, &barrier);
  RecordDiagnosticPlaneCopy(command_list, "hdr_receiver_roi", output);
  std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
  command_list->ResourceBarrier(1, &barrier);
}

inline bool RecordNativeHdrReceiverCopy(ID3D11DeviceContext* context, ID3D11Texture2D* output,
                                       uint32_t output_x = 0, uint32_t output_y = 0) {
  if (context == nullptr || output == nullptr) return false;
  std::scoped_lock lock(internal::state_mutex);
  if (!internal::pending || !internal::pending->hdr_plan) return false;
  auto& capture = *internal::pending;
  capture.native_receiver_required = true;
  if (capture.native_receiver) return false;
  auto receiver = std::make_shared<internal::NativeReceiver>();
  receiver->reservation = capture.reservation;
  capture.native_receiver = receiver;
  auto rect = capture.hdr_plan->roi;
  D3D11_TEXTURE2D_DESC desc{};
  output->GetDesc(&desc);
  receiver->format = desc.Format;
  if (rect.x > UINT32_MAX - output_x || rect.y > UINT32_MAX - output_y) {
    receiver->error = "native_receiver_offset_overflow"; return false;
  }
  rect.x += output_x; rect.y += output_y;
  receiver->rect = rect;
  if (rect.x >= desc.Width || rect.y >= desc.Height || rect.width > desc.Width - rect.x
      || rect.height > desc.Height - rect.y || desc.SampleDesc.Count != 1 || desc.ArraySize != 1
      || internal::DiagnosticPixelBytes(desc.Format) == 0) {
    receiver->error = "native_receiver_invalid_roi_or_format"; return false;
  }
  ID3D11Device* device = nullptr;
  context->GetDevice(&device);
  if (device == nullptr) { receiver->error = "native_receiver_device_missing"; return false; }
  desc.Width = rect.width; desc.Height = rect.height; desc.MipLevels = 1;
  desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
  // Reserve conservative native pitch/alignment before allocation; actual pitch is checked on Map.
  receiver->bytes = ((static_cast<uint64_t>(rect.width) * internal::DiagnosticPixelBytes(desc.Format) + 255) & ~255ull)
                    * rect.height + 65536;
  const D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
  if (!capture.reservation->Reserve(receiver->bytes)) {
    receiver->error = "native_receiver_budget_exceeded"; device->Release(); return false;
  }
  const HRESULT texture_result = device->CreateTexture2D(&desc, nullptr, &receiver->staging);
  const HRESULT query_result = device->CreateQuery(&query, &receiver->event);
  device->Release();
  if (FAILED(texture_result) || FAILED(query_result)) {
    receiver->error = "native_receiver_allocation_failed"; return false;
  }
  receiver->context = context; context->AddRef();
  receiver->source = output; output->AddRef();
  const D3D11_BOX box{rect.x, rect.y, 0, rect.x + rect.width, rect.y + rect.height, 1};
  context->CopySubresourceRegion(receiver->staging, 0, 0, 0, 0, output, 0, &box);
  context->End(receiver->event);
  return true;
}

// Per-stack-pass diagnostic copy: under a multi-pass stack each pass records
// its own plane ("proxy_p2"), exposing what each stacked pass sees and
// returns; a single-pass stack keeps the canonical name ("proxy").  Prepares
// the plane on first use (name-deduped), so stack-1 captures are unchanged.
// Pass suffixes preserve the caller's existing zero-based pass numbering.
inline void RecordStackPlaneCopy(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* texture,
    uint32_t width,
    uint32_t height,
    const char* base_name,
    uint32_t pass,
    uint32_t stack) {
  if (base_name == nullptr) return;
  char name[64];
  if (stack > 1) {
    if (std::snprintf(name, sizeof(name), "%s_p%u", base_name, pass) >= static_cast<int>(sizeof(name))) return;
  } else {
    if (std::snprintf(name, sizeof(name), "%s", base_name) >= static_cast<int>(sizeof(name))) return;
  }
  if (!PrepareDiagnosticPlane(device, name, texture, width, height)) return;
  RecordDiagnosticPlaneCopy(command_list, name, texture);
}

// Attaches the sidecar metadata (divisor, encoding, meter state) that the
// .f32 planes need to be interpretable.  No-op when no capture is pending.
inline void SetPendingDiagnosticMeta(std::string meta, bool require_native_receiver = false) {
  std::scoped_lock lock(internal::state_mutex);
  if (internal::pending.has_value()) {
    if (internal::pending->request_kind == RequestKind::PRE_FG_SERVICE) {
      if (prefg_capture_role != nullptr) {
        size_t offset = 0;
        while (offset < meta.size()) {
          const size_t end = meta.find('\n', offset);
          internal::pending->meta += "role." + std::string(prefg_capture_role) + "."
              + meta.substr(offset, end == std::string::npos ? end : end - offset) + "\n";
          if (end == std::string::npos) break;
          offset = end + 1;
        }
      }
    } else {
      internal::pending->meta = std::move(meta);
    }
    if (require_native_receiver && internal::pending->hdr_plan) {
      internal::pending->native_receiver_required = true;
    }
  }
}

inline bool PreFgCaptureMatches(uint64_t service_serial) {
  std::scoped_lock lock(internal::state_mutex);
  return internal::pending && internal::pending->request_kind == RequestKind::PRE_FG_SERVICE
      && internal::pending->service_serial == service_serial && !internal::pending->aborted
      && !internal::pending->service_sealed;
}

inline void AppendPreFgMeta(uint64_t service_serial, const std::string& meta) {
  std::scoped_lock lock(internal::state_mutex);
  if (internal::pending && internal::pending->request_kind == RequestKind::PRE_FG_SERVICE
      && internal::pending->service_serial == service_serial && !internal::pending->service_sealed) {
    internal::pending->meta += meta;
    return;
  }
  for (auto& capture : internal::hdr_in_flight) {
    if (capture.request_kind == RequestKind::PRE_FG_SERVICE && capture.service_serial == service_serial
        && !capture.service_sealed) {
      capture.meta += meta;
      return;
    }
  }
}

// A submitted service retains its serial until the real Evaluate returns. The
// worker cannot own the metadata before the parameter getter verifies restoration.
inline bool SealPreFgCapture(uint64_t service_serial, bool restoration_verified,
    uintptr_t original_identity = 0, uintptr_t parameter_identity = 0) {
  std::scoped_lock lock(internal::state_mutex);
  auto seal = [&](internal::PendingCapture* capture) {
    if (capture->request_kind != RequestKind::PRE_FG_SERVICE
        || capture->service_serial != service_serial || capture->service_sealed) return false;
    if (capture->restoration_required) {
      restoration_verified = restoration_verified && original_identity != 0
          && original_identity == capture->restoration_original_identity
          && parameter_identity == capture->restoration_parameter_identity;
    }
    capture->restoration_verified = restoration_verified;
    capture->service_sealed = true;
    capture->meta += "fg_parameter_restoration_readback_verified=" + std::to_string(restoration_verified) + "\n";
    if (capture->restoration_required && !restoration_verified) {
      capture->required_planes_complete = false;
      capture->meta += "capture_error=HUDLess_restoration_readback_failed\n";
    }
    return true;
  };
  if (internal::pending && seal(&*internal::pending)) return true;
  for (auto& capture : internal::hdr_in_flight) {
    if (seal(&capture)) return true;
  }
  return false;
}

inline void RecordPreFgRole(uint64_t serial, uint64_t epoch, bool hudless,
    uint32_t hdr_mode, uint32_t passes, uint32_t requested_passes, bool model_evaluated, bool norm_scale_required) {
  std::scoped_lock lock(internal::state_mutex);
  if (!internal::pending || internal::pending->service_serial != serial
      || internal::pending->request_kind != RequestKind::PRE_FG_SERVICE || internal::pending->service_sealed) return;
  auto& role = (hudless ? internal::pending->hudless_role : internal::pending->backbuffer_role);
  if (role.recorded || epoch != internal::pending->restore_config_epoch) {
    internal::pending->aborted = true;
    internal::pending->meta += "capture_error=role_duplicate_or_epoch_mismatch\n";
    return;
  }
  role = {.recorded = true, .model_evaluated = model_evaluated, .hdr_mode = hdr_mode,
          .passes = passes, .requested_passes = requested_passes,
          .norm_scale_required = norm_scale_required, .epoch = epoch};
}

// Copy this dispatch's immutable three words, not the persistently mapped previous slot.
inline bool RecordPreFgComposeStatistics(ID3D12Device* device, ID3D12GraphicsCommandList* list,
    ID3D12Resource* source, uint64_t offset, uint64_t service_serial, uint32_t compose_serial,
    uint32_t mode, uint64_t area, uint32_t hud_limit = 0, bool residual = false) {
  if (device == nullptr || list == nullptr || source == nullptr) return false;
  std::scoped_lock lock(internal::state_mutex);
  if (!internal::pending || internal::pending->service_serial != service_serial
      || internal::pending->request_kind != RequestKind::PRE_FG_SERVICE) return false;
  auto& capture = *internal::pending;
  capture.compose_attempted = true;
  capture.compose_serial = compose_serial;
  capture.compose_area = area;
  capture.compose_mode = mode;
  capture.compose_hud_limit = hud_limit;
  capture.compose_residual = residual;
  const auto desc = source->GetDesc();
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || offset > desc.Width || desc.Width - offset < 12
      || capture.planes.size() >= internal::MAX_STAGE_PLANES
      || capture.allocated_bytes > internal::MAX_STAGE_READBACK_BYTES - 256
      || !capture.reservation || !capture.reservation->Reserve(internal::ReadbackAllocationBytes(device, 256))
      || !capture.reservation->ReserveCpu(capture.cpu_row_bytes < 48 ? 48 - capture.cpu_row_bytes : 0)) {
    capture.required_planes_complete = false;
    capture.rejection_meta += "plane.compose_statistics.error=source_or_capacity_rejected\n";
    return false;
  }
  internal::DiagnosticPlane plane{.name = "compose_statistics", .width = 3, .height = 1,
      .footprint = {.Footprint = {.Format = DXGI_FORMAT_R32_UINT, .Width = 3, .Height = 1, .Depth = 1, .RowPitch = 256}},
      .total_bytes = 256, .format = DXGI_FORMAT_R32_UINT};
  plane.required = true;
  if (!internal::MakeReadbackBuffer(device, 256, &plane.readback, false)) {
    plane.error = "allocation_failed";
    capture.required_planes_complete = false;
  } else {
    list->CopyBufferRegion(plane.readback, 0, source, offset, 12);
    submission::TrackUse(list, plane.readback);
    plane.recorded = true;
    capture.allocated_bytes += 256;
  }
  const bool success = plane.recorded;
  capture.cpu_export_bytes += (capture.cpu_row_bytes < 48 ? 48 - capture.cpu_row_bytes : 0);
  capture.cpu_row_bytes = std::max(capture.cpu_row_bytes, uint64_t{48});
  capture.planes.push_back(std::move(plane));
  return success;
}

inline void SetPreFgComposition(uint64_t serial, bool hudless_present, bool compose_used) {
  std::scoped_lock lock(internal::state_mutex);
  if (internal::pending && internal::pending->service_serial == serial) {
    internal::pending->hudless_present = hudless_present;
    internal::pending->compose_used = compose_used;
  }
}

inline void VerifyPreFgInputBinding(uint64_t serial, bool verified) {
  std::scoped_lock lock(internal::state_mutex);
  auto bind = [&](internal::PendingCapture* capture) {
    if (capture->request_kind != RequestKind::PRE_FG_SERVICE || capture->service_serial != serial
        || capture->service_sealed) return false;
    capture->fg_inputs_verified = verified;
    capture->meta += "fg_parameter_inputs_readback_verified=" + std::to_string(verified) + "\n";
    if (!verified) {
      capture->meta += "capture_error=fg_parameter_inputs_readback_mismatch\n";
    }
    return true;
  };
  if (internal::pending && bind(&*internal::pending)) return;
  for (auto& capture : internal::hdr_in_flight) {
    if (bind(&capture)) return;
  }
}

inline bool BeginPreFgCapture(ID3D12Device* device, ID3D12GraphicsCommandList* list,
    ID3D12Resource* backbuffer, D3D12_RESOURCE_STATES state, uint8_t hdr_mode,
    float unit_nits, uint64_t generation, uint64_t service_serial, uint64_t config_epoch) {
  if (device == nullptr || list == nullptr || backbuffer == nullptr
      || !WantsPreFgService() || HasPending() || service_serial == 0) return false;
  const auto desc = backbuffer->GetDesc();
  if (!PrepareCapture(device, backbuffer, 0, 0, static_cast<uint32_t>(desc.Width), desc.Height,
          desc.Format, hdr_mode, generation, unit_nits)) return false;
  {
    std::scoped_lock lock(internal::state_mutex);
    internal::pending->service_serial = service_serial;
    internal::pending->restore_config_epoch = config_epoch;
    internal::pending->required_planes_complete = false;
    internal::pending->meta += "request_kind=PreFgService\npath=present\nservice_serial=" + std::to_string(service_serial)
        + "\nrestore_config_epoch=" + std::to_string(config_epoch)
        + "\ncapture_build_id=B-FG-F0B-CORE-20261011-02\npre_fg_initial_state=awaiting_stages_gpu_and_restoration\n"
          "fg_output_real.skipped=1\nfg_output_real.reason=output_recording_and_state_not_proven\n"
          "fg_output_interpolated.skipped=1\nfg_output_interpolated.reason=output_recording_and_state_not_proven\n";
  }
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {.pResource = backbuffer, .Subresource = 0, .StateBefore = state,
                        .StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE};
  if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) list->ResourceBarrier(1, &barrier);
  RecordPreCopy(list, backbuffer);
  std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
  if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) list->ResourceBarrier(1, &barrier);
  return true;
}

inline void RecordPreFgPlane(ID3D12Device* device, ID3D12GraphicsCommandList* list,
    const char* name, ID3D12Resource* source, D3D12_RESOURCE_STATES state, uint64_t service_serial) {
  if (device == nullptr || list == nullptr || name == nullptr || !PreFgCaptureMatches(service_serial)) return;
  if (source == nullptr) {
    SkipHdrPlane(name, "parameter_resource_absent");
    return;
  }
  const auto plan = PendingHdrCapturePlan();
  if (!plan || !PrepareHdrPlane(device, name, source, plan->roi)) return;
  AppendPreFgMeta(service_serial, "plane." + std::string(name) + ".resource_identity="
      + std::to_string(reinterpret_cast<uintptr_t>(source)) + "\n");
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {.pResource = source, .Subresource = 0, .StateBefore = state,
                        .StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE};
  if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) list->ResourceBarrier(1, &barrier);
  RecordDiagnosticPlaneCopy(list, name, source);
  std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
  if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) list->ResourceBarrier(1, &barrier);
}

inline void EndPreFgCapture(ID3D12GraphicsCommandList* list, ID3D12Resource* backbuffer,
    D3D12_RESOURCE_STATES state, uint64_t service_serial, bool restoration_required = false,
    uintptr_t original_identity = 0, uintptr_t parameter_identity = 0) {
  if (list == nullptr || backbuffer == nullptr || !PreFgCaptureMatches(service_serial)) return;
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {.pResource = backbuffer, .Subresource = 0, .StateBefore = state,
                        .StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE};
  if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) list->ResourceBarrier(1, &barrier);
  RecordPostCopy(list, backbuffer);
  std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
  if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) list->ResourceBarrier(1, &barrier);
  {
    std::scoped_lock lock(internal::state_mutex);
    if (internal::pending && internal::pending->service_serial == service_serial) {
      internal::pending->restoration_required = restoration_required;
      internal::pending->restoration_original_identity = original_identity;
      internal::pending->restoration_parameter_identity = parameter_identity;
    }
  }
  if (!restoration_required) {
    SealPreFgCapture(service_serial, true);
  }
}

// A helper's presentation window is not the evaluated game image. Copy
// only addon-owned model buffers, whose states and equal extents are known;
// the game's pre-SR Color is merely shader-readable, with no copy-state
// proof. Both PNGs are explicitly sRGB previews, not calibrated HDR or the
// game's final NR-off/on result. Call after the full chain succeeds, while
// the last evaluated pass's proxy/output still exist unmodified.
inline void CaptureModelPair(
    ID3D12Device* device, ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* proxy, D3D12_RESOURCE_STATES proxy_state,
    ID3D12Resource* neural, D3D12_RESOURCE_STATES neural_state,
    uint64_t generation, const char* path, uint32_t pass, uint32_t stack) {
  if (!WantsEvaluatedBuffer() || HasPending()) return;
  const D3D12_RESOURCE_DESC desc = proxy->GetDesc();
  const D3D12_RESOURCE_DESC neural_desc = neural->GetDesc();
  if (desc.Width != neural_desc.Width || desc.Height != neural_desc.Height
      || desc.Format != neural_desc.Format) {
    FailRequest("unsupported_diagnostic", "model input and output have different extents or formats");
    return;
  }
  if (!PrepareCapture(device, proxy, 0, 0, static_cast<uint32_t>(desc.Width),
                      desc.Height, desc.Format, 0, generation, 0.f, true)) {
    FailRequest("allocation_failed", "model diagnostic readback buffers could not be allocated");
    return;
  }
  SetPendingDiagnosticMeta(
      std::string("path=") + path
      + "\ncomparison=model-pass-input-output\ndisplay_reference=0"
        "\nsource=model-proxy-and-neural-output\npng=rgb8-srgb-clipped-preview"
        "\nencoding=srgb\nunits=model-proxy-relative"
        "\nstage=before-look-transfer-and-game-tonemap"
        "\nexcluded=look,transfer,pedestal,game-upscale,game-tonemap,UI"
      + "\ninput_includes_previous_stack_passes=" + (pass != 0 ? "1" : "0")
      + "\nstack_pass=" + std::to_string(pass + 1)
      + "\nstack=" + std::to_string(stack)
      + "\npresent_generation=" + std::to_string(generation)
      + "\nwidth=" + std::to_string(desc.Width)
      + "\nheight=" + std::to_string(desc.Height)
      + "\nsource_format=" + std::to_string(static_cast<uint32_t>(desc.Format)) + "\n");
  {
    std::scoped_lock lock(internal::state_mutex);
    if (internal::pending && internal::pending->model_diagnostic) {
      internal::pending->model_recording_list = command_list;
    }
  }
  D3D12_RESOURCE_BARRIER barriers[2]{};
  for (auto& barrier : barriers) {
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.Subresource = 0;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  }
  barriers[0].Transition.pResource = proxy;
  barriers[0].Transition.StateBefore = proxy_state;
  barriers[1].Transition.pResource = neural;
  barriers[1].Transition.StateBefore = neural_state;
  command_list->ResourceBarrier(2, barriers);
  RecordPreCopy(command_list, proxy);
  RecordPostCopy(command_list, neural);
  for (auto& barrier : barriers) {
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
  }
  command_list->ResourceBarrier(2, barriers);
}

// Marks an armed or in-flight capture as dead (device loss, NR evaluate
// failure, new device). The buffers are released by the worker after the
// settle window.
inline void AbortPending() {
  CancelHdrSequence();
  std::scoped_lock lock(internal::state_mutex);
  if (internal::pending.has_value()) internal::pending->aborted = true;
  internal::capture_requested.store(false, std::memory_order_release);
}

// Called once per present with the new generation, and whether the queue
// hook's exact submission proofs are live (dlssnr's queue_tracking_active).
// Fast path when idle is a few atomic loads. Settled captures are moved to the
// worker; all heavy work happens off this thread.
inline void OnPresent(uint64_t generation, bool exact_proofs) noexcept {
  // Never acquire the game's immediate-context lock under state_mutex:
  // bridge recording takes those locks in the opposite order.
  std::vector<std::shared_ptr<internal::NativeReceiver>> native_polls;
  {
    std::scoped_lock lock(internal::state_mutex);
    if (internal::pending && internal::pending->native_receiver) native_polls.push_back(internal::pending->native_receiver);
    for (const auto& capture : internal::hdr_in_flight) {
      if (capture.native_receiver) native_polls.push_back(capture.native_receiver);
    }
    for (const auto& retained : internal::retired_buffers) {
      if (retained.native_receiver) native_polls.push_back(retained.native_receiver);
    }
  }
  for (const auto& receiver : native_polls) receiver->Poll();
  std::deque<internal::PendingCapture> hdr_ready;
  bool hdr_owned = false;
  {
    std::scoped_lock lock(internal::state_mutex);
    hdr_owned = internal::sequence.active || !internal::hdr_in_flight.empty()
                || (internal::pending && internal::pending->hdr_plan);
    if (internal::pending && internal::pending->hdr_plan && generation > internal::pending->creation_generation
        && (internal::pending->request_kind != RequestKind::PRE_FG_SERVICE || internal::pending->ready
            || internal::pending->aborted
            || generation >= internal::pending->creation_generation + internal::kExactSettlePresents)) {
      auto& capture = *internal::pending;
      capture.lease_taken = true; capture.exact = exact_proofs; capture.lease_generation = generation;
      if (!capture.ready) capture.aborted = true;
      internal::hdr_in_flight.push_back(std::move(capture));
      internal::pending.reset(); internal::has_pending.store(false);
    }
    auto& sequence = internal::sequence;
    while (sequence.active && sequence.next_generation < generation && sequence.attempted < 120) {
      sequence.frames[sequence.attempted++] = {.generation = sequence.next_generation++, .outcome = SequenceOutcome::DROPPED};
    }
    if (sequence.attempted == 120) sequence.active = false;
    if (hdr_owned) {
      internal::QueueSequenceManifest();
      internal::capture_requested.store(sequence.active);
    }
    for (auto it = internal::hdr_in_flight.begin(); it != internal::hdr_in_flight.end();) {
      const bool submitted = internal::StageSubmissionCompleted(&*it);
      const bool releasable = internal::ReadbacksReleasable(*it);
      const bool timeout = generation >= it->lease_generation + internal::kExactSettlePresents;
      const bool service_closed = it->request_kind != RequestKind::PRE_FG_SERVICE || it->service_sealed;
      if (it->exact && submitted && releasable && service_closed) {
        it->completion_proven = true; it->completion_generation = generation;
        hdr_ready.push_back(std::move(*it));
        it = internal::hdr_in_flight.erase(it);
      } else if ((!it->exact || timeout || (releasable && !submitted)) && (service_closed || timeout)) {
        auto metadata = *it;
        metadata.metadata_only = true; metadata.completion_generation = generation;
        metadata.pre_nr_readback = nullptr; metadata.nr_output_readback = nullptr;
        metadata.native_receiver.reset(); metadata.reservation.reset();
        metadata.temporary_resources.clear();
        metadata.meta += "\ncapture_error=hdr_copy_completion_unconfirmed\n";
        if (!service_closed) {
          metadata.meta += "capture_error=HUDLess_restoration_not_sealed_before_timeout\n";
        }
        for (auto& plane : metadata.planes) { plane.readback = nullptr; plane.error = "gpu_unconfirmed"; }
        internal::RetiredCaptureBuffers retained;
        retained.Take(std::move(*it));
        internal::retired_buffers.push_back(std::move(retained)); internal::retained_sets.fetch_add(1);
        for (auto& frame : sequence.frames) {
          if (frame.generation == metadata.creation_generation && frame.outcome == SequenceOutcome::ACCEPTED) frame.outcome = SequenceOutcome::INVALID;
        }
        hdr_ready.push_back(std::move(metadata));
        it = internal::hdr_in_flight.erase(it);
      } else { ++it; }
    }
  }
  for (auto& capture : hdr_ready) {
    internal::worker_busy.store(true);
    internal::HandoffToWorker(std::move(capture));
  }
  // A request without an eligible final frame must not fire unexpectedly
  // after a loading screen, long after the user pressed F5.
  if (internal::capture_requested.load(std::memory_order_acquire)
      && !internal::has_pending.load(std::memory_order_acquire)) {
    if (internal::presents_while_armed.fetch_add(1, std::memory_order_relaxed) + 1
        > internal::kArmTimeoutPresents) {
      internal::presents_while_armed.store(0, std::memory_order_relaxed);
      FailRequest("no_nr_frame", "no eligible capture frame arrived within 600 presents");
    }
  } else {
    internal::presents_while_armed.store(0, std::memory_order_relaxed);
  }
  // With exact proofs every retained set - superseded in its frame, or
  // cancelled - is released on its own proof, whatever the pending capture
  // does (a starved or cancelled capture used to keep them until a later
  // capture settled, or forever).
  if (exact_proofs && internal::retained_sets.load(std::memory_order_relaxed) != 0) {
    std::scoped_lock lock(internal::state_mutex);
    std::erase_if(internal::retired_buffers,
                  [](internal::RetiredCaptureBuffers& retired) {
                    if (!internal::ReadbacksReleasable(retired)) return false;
                    retired.Release();
                    internal::retained_sets.fetch_sub(1, std::memory_order_relaxed);
                    return true;
                  });
  }
  if (hdr_owned || !internal::has_pending.load(std::memory_order_acquire)) return;
  std::optional<internal::PendingCapture> work;
  {
    std::scoped_lock lock(internal::state_mutex);
    if (!internal::pending.has_value()) {
      internal::has_pending.store(false, std::memory_order_release);
      return;
    }
    // A timeout cancels delivery and reports failure - it never establishes
    // GPU completion, so the buffers are not mapped and not released: they
    // move to the retired set.  A frame count cannot prove a fence.
    const auto cancel = [&](const std::string& why) {
      internal::captures_cancelled.fetch_add(1, std::memory_order_relaxed);
      static std::atomic_bool logged_cancel_timeout{false};
      if (!logged_cancel_timeout.exchange(true)) {
        internal::SafeLog(reshade::log::level::warning,
                          std::string("NR screenshot cancelled: ") + why
                              + " (buffers retained, no files written; counted"
                                " as screenshot[cancelled=])");
      }
      // The issue report names why a capture is missing (report_flow.hpp).
      internal::FinishedCapture cancelled;
      cancelled.serial = internal::pending->serial;
      cancelled.outcome = "gpu_unconfirmed";
      internal::PublishFinished(std::move(cancelled));
      std::optional<internal::PendingCapture> metadata;
      if (internal::pending->stage_planes) {
        metadata = *internal::pending;
        metadata->metadata_only = true;
        metadata->completion_proven = false;
        metadata->completion_generation = generation;
        metadata->pre_nr_readback = nullptr;
        metadata->nr_output_readback = nullptr;
        metadata->meta += "\ncapture_error=" + why + "\n";
        for (auto& plane : metadata->planes) {
          plane.readback = nullptr;
          if (plane.error.empty()) plane.error = "gpu_unconfirmed";
        }
      }
      internal::RetiredCaptureBuffers retained;
      retained.Take(std::move(*internal::pending));
      internal::pending.reset();
      internal::has_pending.store(false, std::memory_order_release);
      internal::retired_buffers.push_back(std::move(retained));
      internal::retained_sets.fetch_add(1, std::memory_order_relaxed);
      if (metadata.has_value()) {
        internal::worker_busy.store(true, std::memory_order_release);
        internal::HandoffToWorker(std::move(metadata));
      }
    };
    // The capture-frame's present has occurred: no further evaluates for this
    // frame will run, so the pair already in `pending` is the LAST evaluate's
    // and is the one we keep. Stop arming further frames. (OnPresent bumps the
    // generation before it reaches here, so the capture-frame's present arrives
    // with generation > creation_generation.) If the frame never completed a
    // pair (the last evaluate failed), drop the capture.
    if (generation > internal::pending->creation_generation
        && internal::capture_requested.load(std::memory_order_acquire)) {
      internal::capture_requested.store(false, std::memory_order_release);
      if (!internal::pending->ready && !internal::pending->aborted) {
        internal::pending->aborted = true;
      }
    }
    // The proof is chosen at the capture frame's first present.  Covers
    // aborted captures too - their buffers may hold a recorded pre-copy.
    if (generation > internal::pending->creation_generation
        && !internal::pending->lease_taken) {
      internal::pending->lease_taken = true;
      internal::pending->lease_generation = generation;
      internal::pending->exact = exact_proofs;
      if (!exact_proofs) internal::pending->lease = AcquireGpuLease();
    }
    if (internal::pending->stage_planes && internal::pending->lease_taken && !internal::pending->exact) {
      cancel("stage capture requires exact submission proof; queue-wide leases cannot identify its recording");
      return;
    }
    if (internal::pending->exact) {
      const bool stage_submitted = !internal::pending->stage_planes
                                   || internal::StageSubmissionCompleted(&*internal::pending);
      // Every copy was tracked on the list that recorded it: settled once no
      // recording holding a readback can still execute.  A present, or an
      // unrelated submission completing, proves nothing about that list.
      if (!internal::ReadbacksReleasable(*internal::pending)) {
        if (generation >= internal::pending->lease_generation
                              + internal::kExactSettlePresents) {
          cancel("the command list carrying the copies was not submitted,"
                 " completed and reset within "
                 + std::to_string(internal::kExactSettlePresents) + " presents");
        }
        return;
      }
      if (!stage_submitted) {
        cancel("exact recording retired without observed completed copy-submission receipts");
        return;
      }
    } else {
      const bool empty_lease_timeout =
          internal::pending->lease.empty()
          && generation
                 >= internal::pending->creation_generation
                        + internal::kSettlePresents;
      const bool stalled_lease_timeout =
          !internal::pending->lease.empty()
          && !GpuLeaseCompleted(internal::pending->lease)
          && generation
                 >= internal::pending->lease_generation
                        + kCaptureLeaseFallbackTicks;
      if (empty_lease_timeout || stalled_lease_timeout) {
        cancel(internal::pending->lease.empty()
                   ? "no tracked queue fence proved the copies executed before"
                     " the timeout"
                   : "a tracked queue's fence did not advance before the"
                     " timeout (an idle queue, or a present-starved session)");
        return;
      }
      if (internal::pending->lease.empty()) return;  // still waiting for fences
      if (!GpuLeaseCompleted(internal::pending->lease)) return;
    }
    if (!internal::pending->ready && !internal::pending->aborted) return;
    internal::pending->completion_proven = internal::CaptureCompletionProven(*internal::pending);
    internal::pending->completion_generation = generation;
    if (internal::pending->stage_planes && !internal::pending->completion_proven) return;
    internal::worker_busy.store(true, std::memory_order_release);
    work = std::move(*internal::pending);
    internal::pending.reset();
    internal::has_pending.store(false, std::memory_order_release);
    // Without exact proofs, retired buffers from earlier evaluates in the
    // same frame share the frame's command list and settle together with the
    // final capture.
    if (!work->exact && !work->stage_planes) {
      std::erase_if(internal::retired_buffers, [](internal::RetiredCaptureBuffers& retired) {
        if (retired.stage_planes) return false;
        retired.Release();
        internal::retained_sets.fetch_sub(1, std::memory_order_relaxed);
        return true;
      });
    }
  }
  internal::HandoffToWorker(std::move(work));
}

// Flag-only shutdown signal: no locks, no state release, no worker join.
// Safe from DLL_PROCESS_DETACH where blocking on a worker-owned mutex could
// stall the loader lock on an in-flight job.  The worker exits after its
// current job; any queued job's readbacks are reclaimed by the OS at process
// end (device rebuilds use AbortPending instead, which does drain state).
inline void SignalShutdown() noexcept {
  internal::capture_requested.store(false, std::memory_order_release);
  internal::shutdown_requested.store(true, std::memory_order_release);
  internal::job_signal.notify_all();
}

}  // namespace renodx::addons::dlss5::screenshot
