/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>

// Uses gpu_timer_cases' real WARP queue, recording hooks and bounded finish callback.
namespace gpu_completion_cases {

template <typename Gpu, typename Finish>
inline void Run(Gpu& gpu, const Finish& finish) {
  namespace addon = renodx::addons::dlss5;
  namespace timers = addon::gpu_timers;
  namespace submission = addon::submission;
  namespace native = addon::internal;
  using State = timers::SampleState;
  Check(timers::internal::heap == nullptr && timers::internal::mapped == nullptr,
        "completion fixture starts without timestamp resources");
  std::array<unsigned char, sizeof(timers::stage_stats)> saved_stages{};
  std::array<unsigned char, sizeof(timers::stats)> saved_stats{};
  const auto published = timers::published_segments;
  const auto measured = timers::measured_total_us;
  const auto display_count = timers::internal::display_windows[1].count;
  std::array<unsigned char, sizeof(timers::comparison_result)> saved_comparison{};
  std::memcpy(saved_comparison.data(), &timers::comparison_result, saved_comparison.size());

  auto upload = gpu.Buffer(4, D3D12_HEAP_TYPE_UPLOAD);
  auto readback = gpu.Buffer(4, D3D12_HEAP_TYPE_READBACK);
  void* data = nullptr;
  Check(SUCCEEDED(upload->Map(0, nullptr, &data)), "completion fixture upload map");
  *static_cast<uint32_t*>(data) = 0x1234ABCD;
  upload->Unmap(0, nullptr);
  bool first_recording = true;
  const auto record = [&](uint32_t completed_passes = 2, bool failed_model = false,
                          bool missing_resolve = false, bool wrong_id = false) {
    if (first_recording) {
      struct SavedSamplingState {
        bool profiling = native::gpu_timers_enabled.load();
        bool cost_meter = native::cost_meter_enabled.load();
        bool unavailable = native::cost_meter_unavailable;
        int64_t due = native::cost_sample_due_ns;
        int64_t stage_due = native::stage_cost_sample_due_ns[1];
        ~SavedSamplingState() {
          native::gpu_timers_enabled.store(profiling);
          native::cost_meter_enabled.store(cost_meter);
          native::cost_meter_unavailable = unavailable;
          native::cost_sample_due_ns = due;
          native::stage_cost_sample_due_ns[1] = stage_due;
        }
      } saved_sampling;
      native::gpu_timers_enabled.store(false);
      native::cost_meter_enabled.store(false);
      native::cost_meter_unavailable = false;
      native::stage_cost_sample_due_ns[1] = 0;
      const addon::stage_plan::Group group{.begin = 1, .count = 2};
      Check(native::BeginChainTimers(gpu.list.Get(), gpu.device.Get(), "postSR", 16, 16, group),
            "production wrapper samples GPU completion with both optional timers disabled");
      const auto index = timers::internal::current.idx;
      const auto due = native::stage_cost_sample_due_ns[1];
      Check(timers::internal::current.active && !timers::internal::segments[index].measure
                && timers::internal::heap == nullptr && timers::internal::mapped == nullptr
                && due > 0 && due == native::cost_sample_due_ns,
            "production wrapper activates completion-only sampling without timestamp resources");
      Check(!native::BeginChainTimers(gpu.list.Get(), gpu.device.Get(), "postSR", 16, 16, group)
                && native::stage_cost_sample_due_ns[1] == due
                && timers::internal::current.active && timers::internal::current.idx == index,
            "production wrapper rejects another sample within the same due interval");
      first_recording = false;
    } else {
      Check(timers::Begin(gpu.list.Get(), gpu.device.Get(), "postSR", 16, 16, 1, 2, false),
            "completion-only begin");
    }
    const auto index = timers::internal::current.idx;
    Check(timers::internal::segments[index].count == timers::kSlotFrameStart + 1
              && timers::internal::segments[index].written == (uint64_t{1} << timers::kSlotFrameStart),
          "completion frame-start bit is present without a query");
    for (uint32_t pass = 0; pass < completed_passes; ++pass) {
      timers::SetPass(pass, (wrong_id ? 0 : 1) + pass, 1, 16, 16, 16, 16);
      timers::Mark(gpu.list.Get(), timers::PassEncodeStart(pass));
      timers::Mark(gpu.list.Get(), timers::PassEncodeEnd(pass));
      timers::Mark(gpu.list.Get(), timers::PassEvalStart(pass));
      gpu.list->CopyBufferRegion(readback.Get(), 0, upload.Get(), 0, 4);
      if (failed_model && pass + 1 == completed_passes) break;
      timers::Mark(gpu.list.Get(), timers::PassEvalEnd(pass));
      if (!missing_resolve || pass + 1 != completed_passes) {
        timers::Mark(gpu.list.Get(), timers::PassResolveEnd(pass));
      }
    }
    timers::End(gpu.list.Get());
    return index;
  };
  const auto unchanged_timing = [&] {
    Check(std::memcmp(saved_stages.data(), timers::stage_stats, saved_stages.size()) == 0
              && std::memcmp(saved_stats.data(), &timers::stats, saved_stats.size()) == 0
              && timers::published_segments == published && timers::measured_total_us == measured
              && timers::internal::display_windows[1].count == display_count
              && std::memcmp(saved_comparison.data(), &timers::comparison_result, saved_comparison.size()) == 0,
          "completion-only samples do not publish timings, means or A/B results");
  };

  const auto index = record();
  std::memcpy(saved_stages.data(), timers::stage_stats, saved_stages.size());
  std::memcpy(saved_stats.data(), &timers::stats, saved_stats.size());
  const auto identity = timers::internal::segments[index].comparison.identity;
  const auto sequence = timers::internal::segments[index].sequence;
  timers::Poll(true, false);
  Check(!timers::execution_stats[1].valid && timers::execution_stats[1].state == State::PENDING
            && !submission::ResourceReleasable(&timers::internal::segments[index]),
        "CPU marks alone cannot prove GPU completion");
  finish(1, true, false);
  Check(!timers::execution_stats[1].valid, "closed GPU receipt awaits Poll");
  timers::Poll(true, false);
  const auto& proof = timers::execution_stats[1];
  Check(proof.valid && proof.state == State::VALID && proof.sequence == sequence
            && proof.identity == identity && proof.measured_ns > 0
            && proof.passes[0].id == 1 && proof.passes[1].id == 2
            && proof.passes[0].state == State::VALID && proof.passes[1].state == State::VALID
            && proof.passes[2].state == State::NOT_EXECUTED
            && proof.total_us == 0 && proof.passes[0].eval_us == 0 && proof.samples == 0,
        "closed single submission proves requested global passes without invented timing");
  Check(SUCCEEDED(readback->Map(0, nullptr, &data)), "completion fixture readback map");
  Check(*static_cast<const uint32_t*>(data) == 0x1234ABCD, "completion fixture real WARP copy executed");
  readback->Unmap(0, nullptr);
  Check(timers::internal::heap == nullptr && timers::internal::readback == nullptr
            && timers::internal::mapped == nullptr, "proof did not initialize timestamps");
  unchanged_timing();

  record(0);
  finish();
  Check(!proof.valid && proof.state == State::NOT_EXECUTED, "no model marks cannot prove execution");
  record(1);
  finish();
  Check(!proof.valid && proof.state == State::NOT_EXECUTED, "partial requested group cannot prove completion");
  record(2, true);
  finish();
  Check(!proof.valid && proof.state == State::NOT_EXECUTED, "failed model without eval-end cannot prove completion");
  record(2, false, true);
  finish();
  Check(!proof.valid && proof.state == State::NOT_EXECUTED, "missing resolve-end cannot prove completion");
  record(2, false, false, true);
  finish();
  Check(!proof.valid && proof.state == State::NOT_EXECUTED, "mismatched global pass ids cannot prove completion");
  record();
  finish(2);
  Check(!proof.valid && proof.state == State::REPLAYED, "real double submission rejects replay");
  record();
  finish(0);
  Check(!proof.valid && proof.state == State::DISCARDED, "Reset without submission rejects discard");
  record();
  finish(1, false);
  Check(!proof.valid && proof.state == State::PROOF_UNAVAILABLE, "missing preserved receipt rejects proof");
  record();
  finish(1, true, false);
  timers::Poll(false, false);
  Check(!proof.valid && proof.state == State::PROOF_UNAVAILABLE, "unavailable exact proofs reject completion");

  const auto expired = record();
  for (uint64_t tick = 0; tick < timers::kStaleTicks; ++tick) timers::Poll(true, false);
  Check(!proof.valid && proof.state == State::EXPIRED
            && !submission::ResourceReleasable(&timers::internal::segments[expired]),
        "expired proof retains replayable segment ownership");
  timers::internal::head = expired;
  const auto next = record();
  Check(next != expired && timers::internal::segments[expired].expired,
        "completion-only ring reuse skips expired live recording");
  finish();
  Check(proof.valid && !timers::internal::segments[expired].recorded,
        "late expired completion releases ownership while a fresh recording proves completion");
  timers::execution_stats[1].measured_ns = timers::NowNs() - timers::internal::kSampleAgeNs - 1;
  timers::Poll(true, false);
  Check(!proof.valid && proof.state == State::EXPIRED, "published completion proof expires");
  unchanged_timing();

  const auto pending = record();
  const auto pending_sequence = timers::internal::segments[pending].sequence;
  Check(timers::internal::Ensure(gpu.device.Get()), "fixture timestamp initialization");
  Check(timers::internal::segments[pending].recorded
            && timers::internal::segments[pending].sequence == pending_sequence
            && !submission::ResourceReleasable(&timers::internal::segments[pending]),
        "first timestamp initialization preserves in-flight completion-only recording");
  finish();
  Check(proof.valid && proof.sequence == pending_sequence, "preserved proof survives timestamp initialization");
  unchanged_timing();
  timers::ReleaseAll();
  Check(!timers::execution_stats[1].valid && timers::execution_stats[1].sequence == 0,
        "ReleaseAll clears independent completion proofs");
  std::cout << "PASS completion-only WARP: single closed submission, no timestamps, partial/fail/replay/discard/expiry and timing isolation\n";
}

}  // namespace gpu_completion_cases
