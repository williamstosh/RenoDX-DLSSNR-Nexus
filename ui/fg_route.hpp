/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstdint>

namespace renodx::addons::dlss5::ui {

enum class FgRoute { kUnknown,
                     kBeforeFg,
                     kEveryPresent,
                     kGameFrames,
                     kFallbackPresent,
                     kFallbackUpscaled,
                     kMixed };

struct FgRouteInputs {
  bool enabled = false;
  bool present = false;
  bool prefg_active = false;
  bool upscaled_active = false;
  bool submitted_before_fg = false;
  uint32_t multiplier = 1;
  uint32_t prefg_policy = 0;
  uint32_t frame_policy = 0;
  uint64_t prefg_frames = 0;
  uint64_t prefg_evaluates = 0;
  uint64_t fallbacks = 0;
  uint64_t upscaled_frames = 0;
  uint64_t captures = 0;
  uint64_t nr_successes = 0;
  uint64_t fg_submitted = 0;
};

// Observe only while the menu is drawn. Two consecutive, non-empty 250 ms
// windows are required; a lone flag or historical cumulative count is not
// evidence of the current route. No rendering policy is written here.
struct FgRouteObserver {
  FgRoute route = FgRoute::kUnknown;
  FgRoute candidate = FgRoute::kUnknown;
  FgRouteInputs previous;
  int64_t sampled_ns = 0;
  unsigned streak = 0;

  void Update(const FgRouteInputs& now, int64_t ns) {
    const bool reset = sampled_ns == 0 || ns < sampled_ns || ns - sampled_ns > 2'000'000'000LL
                       || now.enabled != previous.enabled || now.present != previous.present
                       || now.multiplier != previous.multiplier || now.prefg_policy != previous.prefg_policy
                       || now.frame_policy != previous.frame_policy || now.nr_successes < previous.nr_successes
                       || now.prefg_frames < previous.prefg_frames || now.fallbacks < previous.fallbacks
                       || now.prefg_evaluates < previous.prefg_evaluates
                       || now.captures < previous.captures || now.upscaled_frames < previous.upscaled_frames
                       || now.fg_submitted < previous.fg_submitted;
    if (reset || !now.enabled || !now.present) {
      route = candidate = FgRoute::kUnknown;
      streak = 0;
      previous = now;
      sampled_ns = ns;
      return;
    }
    if (ns - sampled_ns < 250'000'000LL) return;
    FgRoute observed = FgRoute::kUnknown;
    if (now.nr_successes > previous.nr_successes || now.fg_submitted > previous.fg_submitted) {
      const bool submitted = now.submitted_before_fg && now.fg_submitted > previous.fg_submitted;
      const bool served = now.prefg_frames > previous.prefg_frames;
      const bool rejected = now.prefg_policy != 0 && now.fallbacks > previous.fallbacks;
      if (submitted) {
        // Native FG/XeFG own the before-FG route. Legacy Present refusals
        // can rise while that owner submits successfully; they are not its fallback.
        observed = FgRoute::kBeforeFg;
      } else if (now.multiplier > 1 && now.upscaled_active && now.upscaled_frames > previous.upscaled_frames) {
        observed = FgRoute::kFallbackUpscaled;
      } else if (now.multiplier > 1 && served && rejected) {
        observed = FgRoute::kMixed;
      } else if (served && now.prefg_active && now.prefg_evaluates > previous.prefg_evaluates) {
        observed = FgRoute::kBeforeFg;
      } else if (now.multiplier > 1 && !served && !now.prefg_active && now.captures > previous.captures) {
        observed = rejected                ? FgRoute::kFallbackPresent
                   : now.frame_policy == 0 ? FgRoute::kEveryPresent
                                           : FgRoute::kGameFrames;
      }
    }
    if (observed == candidate) {
      ++streak;
    } else {
      candidate = observed;
      streak = 1;
    }
    if (streak >= 2) route = candidate;
    previous = now;
    sampled_ns = ns;
  }
};

}  // namespace renodx::addons::dlss5::ui
