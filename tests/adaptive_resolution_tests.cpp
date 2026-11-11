/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#include "adaptive_resolution_cases.hpp"
#include "output_framerate_cases.hpp"

int main() {
  bool passed = adaptive_resolution_cases::Run();
  RunOutputFramerateCases([&](bool condition, const char* name) {
    if (!condition) {
      std::cerr << "output-framerate: " << name << '\n';
      passed = false;
    }
  });
  namespace adaptive = renodx::addons::dlss5::adaptive_resolution;
  for (const uint32_t count : {4u, static_cast<uint32_t>(adaptive::MAX_PREPARED_LEVELS)}) {
    const auto check = [&](bool condition, const char* name) {
      if (!condition) {
        std::cerr << "prepared-tiers[" << count << "]: " << name << '\n';
        passed = false;
      }
    };
    adaptive::Sizes upper{}, floor{};
    upper[0] = upper[1] = {.width = 3840, .height = 2160};
    floor[0] = floor[1] = {.width = 1267, .height = 713};
    adaptive::Session session;
    const adaptive::Settings settings{.enabled = true, .minimum_scale = .33f, .prepared_levels = count};
    adaptive::Output output{.available = true, .source = 1, .generation = 1, .configuration = 1};
    constexpr int64_t start = 1'000'000'000;
    uint64_t sequence = 0;
    const auto tick = [&](uint32_t step, double fps, double gpu_us) {
      const int64_t now = start + step * adaptive::SAMPLE_INTERVAL_NS;
      session.ObserveGpu({.generation = session.status.generation, .frame = ++sequence,
          .sequence = sequence, .context = 1, .recorded_ns = now, .completed_ns = now,
          .total_us = gpu_us, .valid = true, .stable = true}, now);
      ++output.serial;
      output.begin_ns = now - 2'000'000'000;
      output.end_ns = now;
      output.fps = fps;
      session.Prepare(settings, upper, floor, 1, output, now, true, false);
      session.Applied(session.status.generation, session.status.requested, now);
    };
    tick(0, 50., 3000.);
    check(session.ladder.count == count && session.ladder.sizes[0] == upper
        && session.ladder.sizes[count - 1] == floor,
        "the bounded physical tiers retain both manual upper and exact minimum endpoints");
    for (uint32_t tier = 1; tier < count; ++tier) {
      check(session.ladder.coefficients[tier] < session.ladder.coefficients[tier - 1]
          && session.ladder.sizes[tier][0].width < session.ladder.sizes[tier - 1][0].width
          && session.ladder.sizes[tier][0] == session.ladder.sizes[tier][1],
          "each tier is distinct and both Passes preserve the shared group coefficient");
    }
    for (uint32_t step = 1; step <= 13; ++step) tick(step, 50., 3000.);
    check(session.level == 1 && session.trial && !session.raising,
        "a sustained deficit tries the adjacent prepared tier");
    for (uint32_t step = 14; step <= 27; ++step) tick(step, 60., 2400.);
    check(session.level == 1 && !session.trial, "a useful lower prepared tier is accepted");
    for (uint32_t step = 28; step <= 54; ++step) tick(step, 60., 2400.);
    check(session.level == 0 && session.trial && session.raising,
        "the conservative target hold recovers the adjacent higher tier");
    for (uint32_t step = 55; step <= 68; ++step) tick(step, 60., 3000.);
    check(session.level == 0 && !session.trial, "successful quality recovery returns to the manual upper tier");
  }
  return passed ? 0 : 1;
}
