/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace renodx::addons::dlss5::ui {

enum class MonitorMetric : unsigned { FPS, INTERVAL, GPU, PASS_1, PASS_2, PASS_3, PASS_4, COUNT };

struct MonitorPoint {
  int64_t time_ns = 0;
  float value = 0.f;
  float low = 0.f;
  float high = 0.f;
  uint32_t events = 0;
  bool valid = false;
  bool reference = false;
  uint64_t segment = 0;
};

// 50 ms buckets retain the extrema of actual events, including short spikes.
// A gap is an explicit event, never a zero or a held previous measurement.
struct MonitorSeries {
  static constexpr size_t CAPACITY = 2402;
  static constexpr int64_t BUCKET_NS = 50'000'000;
  std::array<MonitorPoint, CAPACITY> points{};
  size_t begin = 0;
  size_t count = 0;

  const MonitorPoint& At(size_t index) const { return points[(begin + index) % CAPACITY]; }

  void Add(int64_t time_ns, float value, bool valid = true, bool reference = false,
           uint64_t segment = 0) {
    if (time_ns <= 0) return;
    valid &= std::isfinite(value) && value >= 0.f;
    if (count != 0) {
      auto& last = points[(begin + count - 1) % CAPACITY];
      if (time_ns < last.time_ns) return;
      if (!valid && !last.valid) return;
      if (valid && last.valid && last.reference == reference && last.segment == segment
          && time_ns / BUCKET_NS == last.time_ns / BUCKET_NS) {
        last.time_ns = time_ns;
        last.value = value;
        last.low = (std::min)(last.low, value);
        last.high = (std::max)(last.high, value);
        ++last.events;
        return;
      }
    }
    if (count == CAPACITY) {
      begin = (begin + 1) % CAPACITY;
      --count;
    }
    points[(begin + count++) % CAPACITY] = {
        .time_ns = time_ns, .value = (valid ? value : 0.f),
        .low = (valid ? value : 0.f), .high = (valid ? value : 0.f),
        .events = (valid ? 1u : 0u), .valid = valid, .reference = reference, .segment = segment};
    while (count > 1 && time_ns - At(0).time_ns > 60'000'000'000LL) {
      begin = (begin + 1) % CAPACITY;
      --count;
    }
  }
};

struct MonitorHistory {
  std::array<MonitorSeries, static_cast<size_t>(MonitorMetric::COUNT)> series{};
  uint64_t generation = 0;
  uint64_t segment = 0;

  void Clear() {
    for (auto& values : series) {
      values.begin = values.count = 0;
    }
    ++generation;
    segment = 0;
  }

  void BeginSegment() {
    ++segment;
    ++generation;
  }

  void ClearGpu() {
    for (size_t metric = static_cast<size_t>(MonitorMetric::GPU); metric < series.size(); ++metric) {
      series[metric].begin = series[metric].count = 0;
    }
    ++generation;
  }

  void ClearCadence() {
    for (auto metric : {MonitorMetric::FPS, MonitorMetric::INTERVAL}) {
      auto& values = series[static_cast<size_t>(metric)];
      values.begin = values.count = 0;
    }
    ++generation;
  }

  void Add(MonitorMetric metric, int64_t time_ns, float value, bool valid = true, bool reference = false) {
    series[static_cast<size_t>(metric)].Add(time_ns, value, valid, reference, segment);
  }
};

}  // namespace renodx::addons::dlss5::ui
