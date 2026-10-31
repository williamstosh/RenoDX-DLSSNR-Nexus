/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace renodx::addons::dlss5::ui::frame_metrics {

enum class Source { NONE, PRESENT, IMGUI };

struct Identity {
  uint64_t device = 0;
  uint64_t surface = 0;
  uint64_t runtime = 0;
  uint64_t epoch = 0;
  uint64_t configuration = 0;
  uint64_t queue = 0;
  bool operator==(const Identity&) const = default;
};

struct Sample {
  Source source = Source::NONE;
  Identity identity;
  uint64_t generation = 0;
  uint64_t sequence = 0;
  int64_t time_ns = 0;
  float raw_interval_ms = 0.f;
  float interval_ms = 0.f;
  bool raw_valid = false;
  bool valid = false;

  float Fps() const { return (valid ? 1000.f / interval_ms : 0.f); }
};

// Actual callback intervals, not display/FG output or GPU frame durations.
// The caller owns synchronization; fixed storage also bounds UI catch-up.
struct Stream {
  static constexpr size_t CAPACITY = 512;
  static constexpr int64_t STALE_NS = 500'000'000;
  std::array<Sample, CAPACITY> events{};
  Sample latest;
  int64_t previous_ns = 0;
  int64_t first_ns = 0;
  uint64_t samples = 0;
  uint64_t published = 0;
  uint64_t generation = 0;
  Source source = Source::NONE;
  Identity identity;
  std::array<float, 16> intervals{};
  size_t interval_count = 0;
  size_t interval_next = 0;
  double interval_sum_ms = 0.0;

  void Reset(Source next_source, const Identity& next_identity) {
    source = next_source;
    identity = next_identity;
    previous_ns = first_ns = 0;
    samples = 0;
    interval_count = interval_next = 0;
    interval_sum_ms = 0.0;
    latest = {};
    ++generation;
  }

  void Observe(Source next_source, const Identity& next_identity, int64_t now_ns, float fallback_ms = 0.f) {
    if (source != next_source || identity != next_identity || now_ns <= previous_ns
        || (previous_ns != 0 && now_ns - previous_ns > STALE_NS)) {
      Reset(next_source, next_identity);
    }
    const float interval_ms = (source == Source::PRESENT
        ? static_cast<float>((now_ns - previous_ns) / 1e6) : fallback_ms);
    const bool interval_valid = now_ns > 0 && previous_ns > 0 && std::isfinite(interval_ms)
        && interval_ms > 0.f && interval_ms <= STALE_NS / 1e6f;
    if (!interval_valid) {
      first_ns = now_ns;
      samples = 0;
      interval_count = interval_next = 0;
      interval_sum_ms = 0.0;
    } else {
      ++samples;
      if (interval_count == intervals.size()) {
        interval_sum_ms -= intervals[interval_next];
      } else {
        ++interval_count;
      }
      intervals[interval_next] = interval_ms;
      interval_sum_ms += interval_ms;
      interval_next = (interval_next + 1) % intervals.size();
    }
    previous_ns = now_ns;
    latest = {.source = source, .identity = identity, .generation = generation,
        .sequence = ++published, .time_ns = now_ns, .raw_interval_ms = interval_ms,
        .interval_ms = (interval_count != 0 ? static_cast<float>(interval_sum_ms / interval_count) : 0.f),
        .raw_valid = interval_valid,
        .valid = interval_valid && samples >= 8 && now_ns - first_ns >= 750'000'000};
    events[(published - 1) % CAPACITY] = latest;
  }

  Sample Read(int64_t now_ns) const {
    auto result = latest;
    const bool fresh = now_ns >= result.time_ns && now_ns - result.time_ns <= STALE_NS;
    result.raw_valid &= fresh;
    result.valid &= fresh;
    return result;
  }
};

inline Sample Select(const Sample& present, const Sample& fallback, const Identity& target, int64_t now_ns) {
  const auto matches = [&](const Sample& sample) {
    return sample.valid && std::isfinite(sample.interval_ms) && sample.interval_ms > 0.f
        && std::isfinite(sample.Fps()) && sample.time_ns > 0 && now_ns >= sample.time_ns
        && now_ns - sample.time_ns <= Stream::STALE_NS
        && sample.identity.device == target.device && sample.identity.surface == target.surface
        && sample.identity.runtime == target.runtime && sample.identity.epoch == target.epoch
        && sample.identity.configuration == target.configuration;
  };
  if (present.source == Source::PRESENT && matches(present)) return present;
  if (fallback.source == Source::IMGUI && matches(fallback)) return fallback;
  return {};
}

inline const char* SourceName(Source source) {
  switch (source) {
    case Source::PRESENT: return "Present";
    case Source::IMGUI: return "ImGui fallback";
    default: return "Frame rate unavailable";
  }
}

inline const char* SourceTip(Source source) {
  switch (source) {
    case Source::PRESENT:
      return "Present callback cadence for this runtime and surface. Does not measure final frame-generated output FPS.";
    case Source::IMGUI:
      return "Fallback: ImGui DeltaTime update cadence for this runtime. Does not measure final frame-generated output FPS.";
    default:
      return "Frame rate unavailable: waiting for fresh samples for this runtime.";
  }
}

}  // namespace renodx::addons::dlss5::ui::frame_metrics
