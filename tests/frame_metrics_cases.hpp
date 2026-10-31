/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

#include "ui/frame_metrics.hpp"
#include "ui/monitor_history.hpp"
#include "ui/state_card.hpp"

template <typename Check>
void RunFrameMetricsCases(const Check& check) {
  namespace ui = renodx::addons::dlss5::ui;
  namespace metrics = ui::frame_metrics;
  constexpr int64_t start = 1'000'000'000;
  constexpr int64_t step = 10'000'000;
  const metrics::Identity target{.device = 1, .surface = 2, .runtime = 3, .epoch = 4, .configuration = 5};
  auto present = std::make_unique<metrics::Stream>();
  auto fallback = std::make_unique<metrics::Stream>();
  present->Observe(metrics::Source::PRESENT, target, start);
  check(!present->Read(start).valid, "one Present cannot establish a callback interval or frame rate");
  present->Observe(metrics::Source::PRESENT, target, start + step);
  check(present->latest.raw_valid && !present->latest.valid,
      "telemetry retains startup raw intervals independently of UI frame-rate warm-up");
  for (int64_t index = 2; index <= 80; ++index) {
    present->Observe(metrics::Source::PRESENT, target, start + index * step);
    fallback->Observe(metrics::Source::IMGUI, target, start + index * step, 20.f);
  }
  const auto now = start + 80 * step;
  const auto sample = present->Read(now);
  check(sample.valid && sample.interval_ms == 10.f && sample.Fps() == 100.f,
      "Present FPS and interval use one measured callback interval");
  auto telemetry = std::make_unique<metrics::Stream>();
  const metrics::Identity no_ui_identity{.device = 1, .surface = 2, .configuration = 5, .queue = 6};
  for (int64_t index = 0; index <= 80; ++index) {
    telemetry->Observe(metrics::Source::PRESENT, no_ui_identity, start + index * step);
  }
  check(telemetry->Read(now).valid && telemetry->Read(now).Fps() == 100.f
          && telemetry->identity.runtime == 0 && telemetry->identity.epoch == 0,
      "independent Present telemetry warms without any GUI runtime target or display lifecycle");
  telemetry->Observe(metrics::Source::PRESENT, no_ui_identity, now + 100'000'000);
  check(telemetry->latest.raw_interval_ms == 100.f
          && std::abs(telemetry->latest.interval_ms - 15.625f) < 0.001f
          && std::abs(telemetry->latest.Fps() - 64.f) < 0.001f,
      "raw spikes remain visible while summary and FPS share the latest sixteen-interval mean");
  check(std::strcmp(metrics::SourceName(metrics::Source::PRESENT), "Present") == 0
          && std::strstr(metrics::SourceName(metrics::Source::IMGUI), "ImGui") != nullptr
          && std::strstr(metrics::SourceTip(metrics::Source::IMGUI), "Fallback") != nullptr,
      "short source labels and fallback explanations remain explicit");
  check(metrics::Select(sample, fallback->Read(now), target, now).source == metrics::Source::PRESENT,
      "verified Present takes precedence over a valid ImGui fallback");
  check(metrics::Select({}, fallback->Read(now), target, now).source == metrics::Source::IMGUI
          && fallback->Read(now).Fps() == 50.f,
      "fallback derives FPS from DeltaTime rather than an unrelated smoothed IO FPS");
  check(!metrics::Select(sample, fallback->Read(now), target, now + metrics::Stream::STALE_NS + 1).valid,
      "expired Present and fallback samples cannot keep a cached numeric FPS alive");
  for (unsigned field = 0; field < 5; ++field) {
    auto other = target;
    switch (field) {
      case 0: ++other.device; break;
      case 1: ++other.surface; break;
      case 2: ++other.runtime; break;
      case 3: ++other.epoch; break;
      case 4: ++other.configuration; break;
    }
    check(!metrics::Select(sample, fallback->Read(now), other, now).valid,
        "device/surface/runtime/session/configuration identity cannot leak between selected sources");
  }
  auto invalid = sample;
  invalid.interval_ms = std::numeric_limits<float>::quiet_NaN();
  check(!metrics::Select(invalid, {}, target, now).valid, "nonfinite intervals are not selectable");
  invalid.interval_ms = 0.f;
  check(!metrics::Select(invalid, {}, target, now).valid, "zero intervals cannot become infinite FPS");
  check(!present->Read(now - 1).valid, "future samples cannot be read after clock reversal");
  const auto generation = present->generation;
  present->Observe(metrics::Source::PRESENT, target, now + metrics::Stream::STALE_NS + 1);
  check(present->generation != generation && !present->Read(now + metrics::Stream::STALE_NS + 1).valid,
      "a Present stall starts a fresh warm-up rather than averaging over a pause");
  auto other_queue = target;
  other_queue.queue = 9;
  const auto before_queue = present->generation;
  present->Observe(metrics::Source::PRESENT, other_queue, now + metrics::Stream::STALE_NS + 1 + step);
  check(present->generation != before_queue && !present->latest.valid,
      "a presenting queue change is a new measurement contract");
  present->Observe(metrics::Source::PRESENT, other_queue, now);
  check(!present->latest.valid && present->first_ns == now, "out-of-order callbacks restart warming");
  for (int64_t index = 1; index <= 1500; ++index) {
    present->Observe(metrics::Source::PRESENT, other_queue, now + index * step);
  }
  check(present->latest.valid && present->latest.sequence == present->published
          && present->events[(present->published - 1) % present->CAPACITY].sequence == present->published,
      "the bounded event ring wraps without manufacturing samples");
  fallback->Observe(metrics::Source::IMGUI, target, now + 1, std::numeric_limits<float>::infinity());
  check(!fallback->latest.valid, "invalid fallback DeltaTime invalidates the previous sample");
  auto history = std::make_unique<ui::MonitorHistory>();
  history->Add(ui::MonitorMetric::GPU, now, 2.f);
  history->Add(ui::MonitorMetric::FPS, now, sample.Fps());
  history->Add(ui::MonitorMetric::INTERVAL, now, sample.interval_ms);
  history->ClearCadence();
  check(history->series[0].count == 0 && history->series[1].count == 0 && history->series[2].count == 1,
      "a cadence source switch clears both CPU curves without deleting independent GPU events");
  history->Add(ui::MonitorMetric::FPS, now + step, 100.f);
  history->Add(ui::MonitorMetric::FPS, now + 2 * step, 0.f, false);
  history->Add(ui::MonitorMetric::FPS, now + 3 * step, 80.f);
  check(history->series[0].count == 3 && !history->series[0].At(1).valid,
      "a stale interval is an explicit history gap, not zero FPS or a held value");
  char cost[256];
  check(ui::FormatNrCost(cost, sizeof(cost), 4.f, 4.f, 16.f, false) > 0
          && std::strstr(cost, "fps") == nullptr && std::strstr(cost, "FPS") == nullptr
          && std::strstr(cost, "without NR") == nullptr,
      "a plausible subtraction cannot invent GPU-bound or uncapped evidence");
  check(ui::FormatNrCost(cost, sizeof(cost), 4.f, 0.f, 0.f, false) > 0,
      "measured group cost remains available independently of frame timing or an NR share");
  check(ui::FormatNrCost(cost, sizeof(cost), std::numeric_limits<float>::quiet_NaN(), 4.f, 16.f, false) == 0
          && cost[0] == 0,
      "nonfinite GPU cost cannot be formatted as a valid measurement");
  check(ui::FormatNrCost(cost, sizeof(cost), 0.f, 4.f, 16.f, false) == 0,
      "unmeasured group cost remains unavailable");
}
