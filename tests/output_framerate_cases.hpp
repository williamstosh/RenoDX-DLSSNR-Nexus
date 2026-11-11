/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cmath>
#include "adaptive_resolution.hpp"
#include "output_framerate.hpp"

template <typename Check>
void RunOutputFramerateCases(const Check& check) {
  namespace rate = renodx::addons::dlss5::output_framerate;
  constexpr int64_t start = 1'000'000'000;
  constexpr int64_t step = 10'000'000;
  rate::Stream stream;
  for (int64_t index = 0; index <= 200; ++index) {
    stream.Observe(rate::Source::XEFG_STATUS, 1, 2, start + index * step, 3, 4, 3);
  }
  auto snapshot = stream.Read(start + 200 * step);
  check(snapshot.available && std::abs(snapshot.fps - 300) < 0.001
          && snapshot.window_begin_ns == start && snapshot.window_end_ns == start + 200 * step
          && snapshot.fps_mad == 0 && snapshot.requested_multiplier == 4 && snapshot.observed_multiplier == 3,
      "SDK counts establish four complete output bins independently of requested multiplier");
  const auto generation = snapshot.source_generation;
  stream.Observe(rate::Source::XEFG_STATUS, 1, 3, start + 201 * step, 4, 4, 4);
  check(!stream.Read(start + 201 * step).available && stream.snapshot.source_generation > generation,
      "configuration changes discard the previous output window");
  for (int64_t index = 202; index <= 401; ++index) {
    stream.Observe(rate::Source::XEFG_STATUS, 1, 3, start + index * step, 0, 4, 0);
  }
  snapshot = stream.Read(start + 401 * step);
  check(snapshot.available && snapshot.fps == 0 && snapshot.observed_multiplier == 0,
      "zero SDK frames remain a valid measured zero rather than missing telemetry");
  check(!stream.Read(start + 502 * step).available, "stale output samples are unavailable");
  stream.Reset(rate::Reason::kDUPLICATE);
  check(!stream.snapshot.available && stream.snapshot.reason == rate::Reason::kDUPLICATE,
      "duplicate application frames clear the output window");
  stream.Observe(rate::Source::PRESENT, 2, 3, start + 503 * step, 1, 1, 1);
  check(stream.snapshot.source == rate::Source::PRESENT && !stream.snapshot.available,
      "source changes warm a fresh window instead of retaining SDK counts");
  stream.Observe(rate::Source::PRESENT, 2, 3, start + 503 * step, 1, 1, 1);
  check(!stream.snapshot.available, "non-monotonic observations cannot complete a window");
  rate::Stream fractional;
  for (int64_t index = 0; index <= 200; ++index) {
    fractional.Observe(rate::Source::XEFG_STATUS, 1, 1, start + index * step,
        (index % 10 < 7 ? 4 : 3), 4);
  }
  check(std::abs(fractional.Read(start + 200 * step).observed_multiplier - 3.7) < 0.001,
      "observed XeFG multiplier is the full-window output to application count ratio");

  {
    std::unique_lock lock(rate::internal::state.mutex);
    auto& state = rate::internal::state;
    state.stream = {};
    state.native_options_known = false;
    state.native_enabled = false;
    state.native_state_ns = state.native_evaluation_ns = state.xefg_ns = 0;
    state.native_application_identity = state.native_applications = state.native_state_applications = 0;
    state.native_application_ns = 0;
    state.native_state_application_coverage = false;
  }
  rate::ObserveNativeOptions(7, 5, 1, 3);
  for (int64_t index = 0; index <= 200; ++index) {
    rate::ObserveNativeEvaluation(3, true, start + index * step);
    rate::ObserveNativePresent(9, true, start + index * step);
  }
  snapshot = rate::Read(start + 200 * step);
  check(snapshot.available && snapshot.source == rate::Source::NATIVE_FIXED
          && std::abs(snapshot.fps - 300) < 0.001 && snapshot.observed_multiplier == 0,
      "known fixed native mode uses only outer SDK application Presents");
  const auto fixed_generation = snapshot.source_generation;
  rate::ObserveNativeEvaluation(4, true, start + 200 * step + 1);
  rate::ObserveNativePresent(9, true, start + 200 * step + 1);
  snapshot = rate::Read(start + 200 * step + 1);
  check(!snapshot.available && snapshot.reason == rate::Reason::kUNKNOWN_FG
          && snapshot.source_generation > fixed_generation,
      "conflicting successful native declaration cancels the fixed estimate and its control generation");
  rate::ObserveNativeEvaluation(1, true, start + 200 * step + 2);
  rate::ObserveNativePresent(9, true, start + 200 * step + 2);
  check(!rate::Read(start + 200 * step + 2).available,
      "a successful one-frame declaration cannot retain a requested multi-frame estimate");
  rate::ObserveNativeOptions(7, 5, 1, 4);
  check(!rate::Read(start + 200 * step).available,
      "On to On native multiplier changes reset output cadence");
  rate::ObserveNativeOptions(7, 5, 3, 4);
  rate::ObserveNativePresent(9, true, start + 201 * step);
  check(!rate::Read(start + 201 * step).available
          && rate::Read(start + 201 * step).reason == rate::Reason::kUNKNOWN_FG,
      "dynamic mode cannot fall back to a fixed multiplier");
  for (int64_t index = 0; index <= 200; ++index) {
    rate::ObserveNativeState(7, true, 2, start + (202 + index) * step);
  }
  snapshot = rate::Read(start + 402 * step);
  check(snapshot.available && snapshot.source == rate::Source::NATIVE_STATE && snapshot.dynamic
          && std::abs(snapshot.fps - 200) < 0.001 && snapshot.multiplier_variable
          && snapshot.observed_multiplier == 0,
      "passive SDK output counts support dynamic FG independently of its request");
  rate::ObserveNativeState(8, true, 100, start + 403 * step);
  check(!rate::Read(start + 403 * step).available,
      "a different native viewport invalidates ambiguous SDK counts");
  rate::ObserveXeFG(11, 1, start + 404 * step, false, true, false, 4, 0, 4);
  check(!rate::Read(start + 404 * step).available,
      "failed or occluded proxy Present cannot publish the SDK count");
  for (int64_t index = 0; index <= 200; ++index) {
    rate::ObserveXeFG(11, 1, start + (405 + index) * step, true, true, false,
        (index % 10 < 7 ? 4 : 3), 0, 0, 4);
  }
  snapshot = rate::Read(start + 605 * step);
  check(snapshot.available && snapshot.requested_multiplier == 0 && snapshot.maximum_multiplier == 4
          && std::abs(snapshot.observed_multiplier - 3.7) < 0.001,
      "XeFG without optional setter retains observed fractional multiplier and separate init ceiling");
  rate::ObserveXeFG(11, 1, start + 606 * step, true, true, true, 4, 0, 4, 4);
  snapshot = rate::Read(start + 606 * step);
  check(!snapshot.available && snapshot.reason == rate::Reason::kDUPLICATE
          && snapshot.observed_multiplier == 0 && snapshot.maximum_multiplier == 0,
      "duplicate XeFG status clears the full window and stale observed multiplier metadata");
  rate::ObserveNativeOptions(7, 5, 0, 1);
  rate::ObserveNativeEvaluation(4, true, start + 607 * step);
  snapshot = rate::Read(start + 607 * step);
  check(!snapshot.available && snapshot.declared_multiplier == 4,
      "a successful NGX multiplier declaration alone does not create an output FPS source");
  rate::ReleaseNative();
  for (int64_t index = 0; index <= 200; ++index) {
    rate::ObserveNativeOptions(7, 5, 0, 2);
    rate::ObservePresent(21, 1, false, start + (800 + index) * step);
  }
  check(rate::Read(start + 1000 * step).available,
      "native release and repeated Off options permit a fresh no-FG Present window");
  for (int64_t index = 0; index <= 305; ++index) {
    rate::ObservePresent(22, 1, false, start + (1001 + index) * step);
  }
  snapshot = rate::Read(start + 1306 * step);
  check(snapshot.available && snapshot.source == rate::Source::PRESENT
          && std::abs(snapshot.fps - 100) < 0.001,
      "a replacement primary warms after the old primary stops without extending its stale timestamp");

  rate::Stream slow_sdk;
  constexpr int64_t sdk_period = 1'050'000'000;
  for (int64_t index = 0; index <= 4; ++index) {
    slow_sdk.Observe(rate::Source::NATIVE_STATE, 1, 1, start + index * sdk_period, 105);
  }
  snapshot = slow_sdk.Read(start + 4 * sdk_period + 1'100'000'000);
  check(snapshot.available && std::abs(snapshot.fps - 100.) < 0.001
          && snapshot.window_end_ns - snapshot.window_begin_ns == 4 * sdk_period,
      "native SDK intervals slightly slower than 1 Hz retain complete intervals and a fresh window");
  check(!slow_sdk.Read(start + 4 * sdk_period + 2'000'000'001).available,
      "native SDK observations still expire after the bounded two-second interval");
  rate::Stream fast_present;
  for (int64_t index = 0; index <= 200; ++index) {
    fast_present.Observe(rate::Source::PRESENT, 1, 1, start + index * step, 1);
  }
  check(!fast_present.Read(start + 200 * step + 1'100'000'000).available,
      "the longer SDK allowance does not extend ordinary Present freshness");

  namespace adaptive = renodx::addons::dlss5::adaptive_resolution;
  const auto controller_output = [](const rate::Snapshot& sample) {
    return adaptive::Output{.available = sample.available, .source = static_cast<uint64_t>(sample.source),
        .generation = sample.source_generation, .configuration = sample.configuration_generation,
        .serial = sample.window_serial, .begin_ns = sample.window_begin_ns, .end_ns = sample.window_end_ns,
        .fps = sample.fps, .mad = sample.fps_mad, .observed_multiplier = sample.observed_multiplier,
        .multiplier_variable = sample.multiplier_variable};
  };
  adaptive::Sizes upper{}, floor{};
  upper[0] = {.width = 1920, .height = 1080};
  floor[0] = {.width = 634, .height = 356};
  rate::ReleaseNative();
  rate::ObserveNativeOptions(7, 5, 3, 4);
  adaptive::Session unpaired;
  for (int64_t index = 0; index <= 400; ++index) {
    const int64_t now = start + index * step;
    rate::ObserveNativeState(7, true, 2, now);
    unpaired.ObserveGpu({.generation = unpaired.status.generation,
        .frame = static_cast<uint64_t>(index + 1), .sequence = static_cast<uint64_t>(index + 1),
        .context = 1, .recorded_ns = now, .completed_ns = now,
        .total_us = 3000., .valid = true, .stable = true}, now);
    unpaired.Prepare({.enabled = true, .target_fps = 250.f}, upper, floor, 1,
        controller_output(rate::Read(now)), now, true, false);
    unpaired.Applied(unpaired.status.generation, unpaired.status.requested, now);
  }
  check(rate::Read(start + 400 * step).available && unpaired.level == 0 && !unpaired.trial
          && unpaired.status.reason == adaptive::Reason::COLLECTING,
      "native dynamic output without a matching application count cannot start an unguarded quality trial");

  rate::ReleaseNative();
  rate::ObserveNativeOptions(7, 5, 3, 4);
  rate::ObserveNativePresent(9, true, start);
  rate::ObserveNativeState(7, true, 0, start + 1);
  adaptive::Session paired;
  bool raised_multiplier = false, accepted_confounded_quality = false, saw_native_ratio = false;
  uint32_t queued_frames = 0;
  for (int64_t index = 1; index <= 2200; ++index) {
    const int64_t now = start + index * step;
    rate::ObserveNativePresent(9, true, now);
    queued_frames += (raised_multiplier ? 4u : 2u);
    if (index % 105 == 0) {
      rate::ObserveNativeState(7, true, queued_frames, now + 1);
      queued_frames = 0;
    }
    const auto sample = rate::Read(now + 1);
    saw_native_ratio |= sample.available && std::abs(sample.observed_multiplier - 2.) < 0.001;
    paired.ObserveGpu({.generation = paired.status.generation,
        .frame = static_cast<uint64_t>(index), .sequence = static_cast<uint64_t>(index),
        .context = 1, .recorded_ns = now, .completed_ns = now + 1,
        .total_us = (paired.level == 0 ? 3000. : 2900.), .valid = true, .stable = true}, now + 1);
    paired.Prepare({.enabled = true, .target_fps = 250.f}, upper, floor, 1,
        controller_output(sample), now + 1, true, false);
    paired.Applied(paired.status.generation, paired.status.requested, now + 1);
    if (paired.trial && paired.level != 0) raised_multiplier = true;
    accepted_confounded_quality |= raised_multiplier && paired.level != 0 && !paired.trial;
  }
  snapshot = rate::Read(start + 2200 * step + 1);
  check(saw_native_ratio && raised_multiplier && !accepted_confounded_quality && paired.level == 0
          && snapshot.available && snapshot.source == rate::Source::NATIVE_STATE
          && std::abs(snapshot.observed_multiplier - 4.) < 0.001,
      "native 1 Hz SDK counts and matching base Presents reject a 2x to 4x false NR benefit");
  const auto native_generation = snapshot.source_generation;
  rate::ObserveNativePresent(10, true, start + 2201 * step);
  snapshot = rate::Read(start + 2201 * step);
  check(!snapshot.available && snapshot.source_generation > native_generation,
      "native application-source changes invalidate the old SDK multiplier window");
}
