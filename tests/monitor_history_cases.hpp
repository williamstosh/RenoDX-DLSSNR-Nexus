/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <limits>
#include <memory>

#include "ui/monitor_history.hpp"

template <typename Check>
void RunMonitorHistoryCases(const Check& check) {
  namespace ui = renodx::addons::dlss5::ui;
  // Heap storage also exercises the fixed capacity without consuming the test
  // executable's small default Windows stack. Production owns thread-local data.
  auto history = std::make_unique<ui::MonitorHistory>();
  auto& series = history->series[static_cast<unsigned>(ui::MonitorMetric::GPU)];
  history->Add(ui::MonitorMetric::GPU, 1'000'000'000, 2.f);
  history->Add(ui::MonitorMetric::GPU, 1'001'000'000, 9.f);
  history->Add(ui::MonitorMetric::GPU, 1'002'000'000, 3.f);
  check(series.count == 1 && series.At(0).events == 3 && series.At(0).low == 2.f
            && series.At(0).high == 9.f && series.At(0).value == 3.f,
        "history buckets preserve actual events and short peaks rather than smoothing them away");
  history->Add(ui::MonitorMetric::GPU, 1'003'000'000, 0.f, false);
  history->Add(ui::MonitorMetric::GPU, 1'004'000'000, 0.f, false);
  history->Add(ui::MonitorMetric::GPU, 1'005'000'000, 4.f);
  check(series.count == 3 && !series.At(1).valid && series.At(1).events == 0
            && series.At(2).valid && series.At(2).events == 1,
        "a missing sample is one explicit gap, never zero cost or a repeated cached measurement");
  history->Add(ui::MonitorMetric::GPU, 1'004'000'000, 99.f);
  check(series.count == 3 && series.At(2).value == 4.f, "out-of-order events cannot rewrite history");
  history->Add(ui::MonitorMetric::GPU, 1'006'000'000, 5.f, true, true);
  check(series.count == 4 && series.At(3).reference, "reference sums cannot merge with single-group measurements");
  history->Add(ui::MonitorMetric::GPU, 1'007'000'000, std::numeric_limits<float>::quiet_NaN());
  check(!series.At(series.count - 1).valid, "nonfinite values become gaps");
  history->Clear();
  for (int64_t tick = 1; tick <= 80'000; ++tick) {
    history->Add(ui::MonitorMetric::GPU, tick * 1'000'000, static_cast<float>(tick % 13));
  }
  check(series.count <= 1202 && series.At(0).time_ns >= 19'999'000'000LL
            && series.At(series.count - 1).time_ns == 80'000'000'000LL,
        "high-frequency events retain a bounded rolling sixty seconds");
  for (int64_t tick = 80'001; tick <= 84'000; ++tick) {
    history->Add(ui::MonitorMetric::GPU, tick * 1'000'000, 1.f, tick % 2 == 0);
  }
  check(series.count == ui::MonitorSeries::CAPACITY, "rapid valid/gap transitions remain bounded on ring rotation");
  history->Add(ui::MonitorMetric::FPS, 84'000'000'000LL, 120.f);
  const auto generation = history->generation;
  history->BeginSegment();
  history->Add(ui::MonitorMetric::GPU, 84'001'000'000LL, 2.f);
  check(series.count >= 2 && series.At(series.count - 1).segment != series.At(series.count - 2).segment,
        "configuration boundaries retain old samples while marking a new history segment");
  history->ClearGpu();
  check(series.count == 0 && history->series[0].count == 1 && history->generation == generation + 2,
        "GPU source changes clear GPU curves while preserving independent UI cadence");
  history->Clear();
  check(history->series[0].count == 0, "configuration/runtime changes clear all history");
}
