/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cmath>
#include <iostream>

#include "../adaptive_resolution.hpp"

namespace adaptive_resolution_cases {
namespace adaptive = renodx::addons::dlss5::adaptive_resolution;

inline bool Run() {
  bool passed = true;
  const auto check = [&](bool condition, const char* name) {
    if (!condition) {
      std::cerr << "adaptive-resolution: " << name << '\n';
      passed = false;
    }
  };
  adaptive::Sizes upper{}, floor{};
  upper[0] = {.width = 2560, .height = 1440};
  floor[0] = {.width = 1267, .height = 713};
  auto ladder = adaptive::BuildLadder(upper, floor, 0.67f);
  check(ladder.count == 8 && std::abs(ladder.coefficients[7] - 0.67f) < 0.001f,
        "ladder includes the exact relative lower bound");
  check(ladder.sizes[7][0] == adaptive::Size{.width = 1715, .height = 965}, "both axes share one coefficient");
  upper[1] = {.width = 10, .height = 10};
  floor[1] = {.width = 20, .height = 20};
  ladder = adaptive::BuildLadder(upper, floor, 0.33f);
  check(ladder.sizes[ladder.count - 1][0] == floor[0], "stage floor constrains relative minimum");
  check(ladder.sizes.back()[1].width == 0 || ladder.sizes[ladder.count - 1][1] == upper[1],
        "a Follow upper bound below the floor never grows");
  upper[1] = floor[1] = {};

  adaptive::Session session;
  session.Prepare({.enabled = true, .target_fps = 20.f}, upper, floor, 1, {}, 1, true, false);
  check(session.settings.target_fps == 30.f, "controller target lower bound is 30 FPS");
  session = {};
  adaptive::Output output{.available = true, .source = 1, .generation = 1, .configuration = 1};
  uint64_t sequence = 0;
  constexpr int64_t start = 1'000'000'000;
  const auto tick = [&](int64_t now, double fps, double gpu_us) {
    session.ObserveGpu({.generation = session.status.generation, .frame = ++sequence,
        .sequence = sequence, .context = 1, .recorded_ns = now, .completed_ns = now,
        .total_us = gpu_us, .valid = true, .stable = true}, now);
    output.serial++;
    output.begin_ns = now - 2'000'000'000;
    output.end_ns = now;
    output.fps = fps;
    session.Prepare({.enabled = true}, upper, floor, 1, output, now, true, false);
    session.Applied(session.status.generation, session.status.requested, now);
  };
  tick(start, 50., 3000.);
  for (int i = 1; i <= 13; ++i) tick(start + i * adaptive::SAMPLE_INTERVAL_NS, 50., 3000.);
  check(session.level == 2 && session.trial, "sustained low FPS probes without a prior NR cost increase");
  const auto lower_generation = session.status.generation;
  for (int i = 14; i <= 27; ++i) tick(start + i * adaptive::SAMPLE_INTERVAL_NS, 50., 2400.);
  check(session.level == 0 && session.blocked && !session.trial,
        "lower NR cost without output FPS benefit restores quality and stops probing");
  for (int i = 28; i <= 60; ++i) tick(start + i * adaptive::SAMPLE_INTERVAL_NS, 50., 3000.);
  check(session.level == 0 && session.blocked, "a steady cap does not trigger repeated downscaling");
  const size_t samples = session.gpu_count;
  session.ObserveGpu({.generation = lower_generation, .frame = ++sequence, .sequence = sequence,
      .recorded_ns = start, .completed_ns = start, .total_us = 1., .valid = true, .stable = true},
      start + 20'000'000'000);
  check(session.gpu_count == samples, "old generation completions cannot drive the new size");

  session = {};
  sequence = 0;
  for (int i = 0; i <= 13; ++i) tick(start + i * adaptive::SAMPLE_INTERVAL_NS, 50., 3000.);
  for (int i = 14; i <= 27; ++i) tick(start + i * adaptive::SAMPLE_INTERVAL_NS, 60., 2400.);
  check(session.level == 2 && !session.trial, "useful lower resolution is accepted");
  for (int i = 28; i <= 54; ++i) tick(start + i * adaptive::SAMPLE_INTERVAL_NS, 60., 2400.);
  check(session.level == 1 && session.trial && session.raising,
        "quality recovers at a locked target after the conservative hold");
  for (int i = 55; i <= 68; ++i) tick(start + i * adaptive::SAMPLE_INTERVAL_NS, 50., 3000.);
  check(session.level == 2 && !session.trial && session.failed_raise_until_ns != 0,
        "unsuccessful quality recovery rolls back with a boundary cooldown");
  session.Prepare({.enabled = false}, upper, floor, 1, output, start + 20'000'000'000, true, false);
  check(session.level == 0 && session.status.requested == upper
      && session.status.reason == adaptive::Reason::DISABLED, "disabling restores the manual upper bound");

  session = {};
  sequence = 0;
  output.observed_multiplier = 2.;
  for (int i = 0; i <= 13; ++i) tick(start + i * adaptive::SAMPLE_INTERVAL_NS, 50., 3000.);
  output.observed_multiplier = 4.;
  for (int i = 14; i <= 27; ++i) tick(start + i * adaptive::SAMPLE_INTERVAL_NS, 100., 2900.);
  check(session.level == 2 && session.trial && session.extra_window,
        "an FG ratio increase gets one extra window instead of accepting an apparent NR benefit");
  for (int i = 28; i <= 38; ++i) tick(start + i * adaptive::SAMPLE_INTERVAL_NS, 100., 2900.);
  check(session.level == 0 && session.blocked && !session.trial,
        "a sustained FG ratio change restores the original quality after the bounded comparison");
  output.observed_multiplier = 0.;

  for (const int frequency : {60, 120, 240}) {
    adaptive::Session profiling;
    profiling.status.generation = 1;
    profiling.applied_ns = start;
    profiling.stable_after_ns = start;
    for (int i = 0; i < frequency * 2; ++i) {
      const int64_t now = start + static_cast<int64_t>(i) * 1'000'000'000 / frequency;
      profiling.ObserveGpu({.generation = 1, .frame = static_cast<uint64_t>(i + 1),
          .sequence = static_cast<uint64_t>(i + 1), .context = 1,
          .recorded_ns = now, .completed_ns = now, .total_us = 3000., .valid = true, .stable = true}, now);
    }
    check(profiling.gpu_count == 5
        && profiling.gpu_recorded_ns.back() - profiling.gpu_recorded_ns.front() >= 1'000'000'000,
        "profiling is downsampled by recording time at 60/120/240 Hz");
  }
  adaptive::Session dual;
  dual.status.generation = 1;
  dual.applied_ns = dual.stable_after_ns = start;
  dual.ObserveGpu({.generation = 1, .frame = 4, .sequence = 2, .context = 1,
      .recorded_ns = start, .completed_ns = start, .total_us = 2000.,
      .role = 1, .required_roles = 3, .valid = true, .stable = true}, start);
  check(dual.gpu_count == 0, "a partial dual-color chain never becomes a GPU sample");
  dual.ObserveGpu({.generation = 1, .frame = 4, .sequence = 1, .context = 1,
      .recorded_ns = start, .completed_ns = start, .total_us = 1000.,
      .role = 2, .required_roles = 3, .valid = true, .stable = true}, start);
  check(dual.gpu_count == 1 && dual.gpu_us[0] == 3000., "out-of-order color completion produces one complete sum");
  return passed;
}
}  // namespace adaptive_resolution_cases
