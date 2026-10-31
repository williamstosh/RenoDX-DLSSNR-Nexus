/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace renodx::addons::dlss5::ui {

struct RecentGpuCompletion {
  // The adapter validates the identity frozen at recording against live settings.
  bool current = false;
  std::uint64_t sequence = 0;
  std::int64_t completed_ns = 0;
};

struct RecentHealth {
  bool ready = false;
  bool partial = false;
  std::uint64_t eligible = 0;
  std::uint64_t injected = 0;
  std::uint64_t gaps = 0;
  std::int64_t last_sample_ns = 0;
  bool gpu_completed = false;
};

// Source frames, not individual model/StageGroup calls. Session counters stay
// untouched. A configuration change discards only this bounded observation.
class RecentHealthWindow {
 public:
  static constexpr std::int64_t kWindowNs = 3'000'000'000LL;
  static constexpr std::int64_t kStaleNs = 2'000'000'000LL;
  static constexpr std::uint64_t kMinSamples = 60;

  RecentHealth Observe(std::uint64_t configuration, std::uint64_t eligible,
                       std::uint64_t injected, std::uint64_t gaps, std::int64_t now_ns,
                       std::uint64_t gpu_recorded_sequence = 0, const RecentGpuCompletion& gpu_completion = {}) {
    const Point point{now_ns, eligible, injected, gaps};
    if (count_ == 0 || configuration != configuration_ || eligible < latest_.eligible
        || injected < latest_.injected || gaps < latest_.gaps || now_ns < latest_.ns) {
      configuration_ = configuration;
      points_ = {};
      points_[0] = point;
      count_ = 1;
      next_ = 1;
      partial_ = false;
      last_sample_ns_ = 0;
      started_ns_ = now_ns;
      // Include pending recordings: late completion of old work is not new evidence.
      gpu_sequence_baseline_ = gpu_recorded_sequence;
    } else {
      if (eligible > latest_.eligible) {
        last_sample_ns_ = now_ns;
      }
      // At most ten checkpoints per second, regardless of render frame rate.
      const auto previous = (next_ + points_.size() - 1) % points_.size();
      if (now_ns - points_[previous].ns >= 100'000'000LL) {
        points_[next_] = point;
        next_ = (next_ + 1) % points_.size();
        if (count_ < points_.size()) ++count_;
      }
    }
    latest_ = point;
    const Point* base = &point;
    for (std::size_t i = 0; i < count_; ++i) {
      if (now_ns - points_[i].ns <= kWindowNs && points_[i].ns < base->ns) {
        base = &points_[i];
      }
    }
    RecentHealth result{.partial = partial_, .eligible = eligible - base->eligible,
                         .injected = injected - base->injected, .gaps = gaps - base->gaps,
                         .last_sample_ns = last_sample_ns_};
    result.ready = result.eligible >= kMinSamples && result.injected <= result.eligible
        && now_ns - base->ns >= 750'000'000LL && last_sample_ns_ != 0
        && now_ns - last_sample_ns_ <= kStaleNs;
    result.gpu_completed = gpu_completion.current && gpu_completion.sequence > gpu_sequence_baseline_
        && gpu_completion.sequence <= gpu_recorded_sequence && gpu_completion.completed_ns > 0
        && gpu_completion.completed_ns >= started_ns_ && gpu_completion.completed_ns <= now_ns
        && now_ns - gpu_completion.completed_ns <= kStaleNs;
    if (result.ready) {
      const double ratio = static_cast<double>(result.injected) / result.eligible;
      partial_ = ratio < (partial_ ? 0.99 : 0.98);
      result.partial = partial_;
    }
    return result;
  }

 private:
  struct Point {
    std::int64_t ns = 0;
    std::uint64_t eligible = 0;
    std::uint64_t injected = 0;
    std::uint64_t gaps = 0;
  };
  std::array<Point, 40> points_{};
  Point latest_{};
  std::size_t count_ = 0;
  std::size_t next_ = 0;
  std::uint64_t configuration_ = 0;
  std::int64_t last_sample_ns_ = 0;
  std::int64_t started_ns_ = 0;
  std::uint64_t gpu_sequence_baseline_ = 0;
  bool partial_ = false;
};

}  // namespace renodx::addons::dlss5::ui
