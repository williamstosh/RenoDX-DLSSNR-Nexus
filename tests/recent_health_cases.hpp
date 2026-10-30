/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "ui/state_card.hpp"

template <typename Check>
void RunRecentHealthCases(const Check& check) {
  namespace ui = renodx::addons::dlss5::ui;
  ui::RecentHealthWindow window;
  std::uint64_t eligible = 100'000, injected = 50'000, gaps = 10'000;
  std::int64_t now = 1'000'000'000LL;
  auto sample = window.Observe(1, eligible, injected, gaps, now);
  check(!sample.ready, "historical totals cannot prove a new observation");
  const auto advance = [&](std::uint64_t successes, std::uint64_t failures) {
    now += 100'000'000LL;
    eligible += successes + failures;
    injected += successes;
    gaps += failures;
    return window.Observe(1, eligible, injected, gaps, now);
  };
  for (int i = 0; i < 12; ++i) sample = advance(3, 3);
  check(sample.ready && sample.partial, "current sustained source skips turn yellow");
  for (int i = 0; i < 40; ++i) sample = advance(6, 0);
  check(sample.ready && !sample.partial && sample.gaps == 0,
        "bounded clean window recovers despite large historical failures and gaps");
  check(eligible - injected > 50'000 && gaps > 10'000, "recovery preserves cumulative diagnostics");
  for (int i = 0; i < 12; ++i) sample = advance(2, 4);
  check(sample.ready && sample.partial, "a recovered session can become partial again");
  now += 2'100'000'000LL;
  sample = window.Observe(1, eligible, injected, gaps, now);
  check(!sample.ready, "no new source frames never means recovered");
  sample = window.Observe(2, eligible, injected, gaps, now);
  check(!sample.ready && sample.eligible == 0, "configuration change starts fresh confirmation");
  for (int i = 0; i < 12; ++i) {
    now += 100'000'000LL;
    eligible += 6;
    injected += 6;
    sample = window.Observe(2, eligible, injected, gaps, now);
  }
  check(sample.ready && !sample.partial, "new configuration requires sufficient fresh evidence");
  renodx::addons::dlss5::NrVerdict verdict;
  verdict.state = renodx::addons::dlss5::NrState::kEngaged;
  verdict.api = "d3d12";
  verdict.path = "inline";
  verdict.eligible = eligible;
  verdict.injected = injected;
  verdict.ratio = 0.5;
  ui::CardInputs inputs{.verdict = verdict, .active_features = 1};
  inputs.recent = sample;
  check(ui::ComputeStatusCard(inputs).tone == ui::CardTone::kGood,
        "status uses recent source ratio instead of session history");
  inputs.recent->ready = false;
  check(ui::ComputeStatusCard(inputs).id == ui::CardId::kStarting,
        "stale or insufficient samples display neutral confirmation");
  inputs.active_features = 0;
  check(ui::ComputeStatusCard(inputs).id == ui::CardId::kWaitingForDlss,
        "DLSS pause is waiting, never recovered");
  inputs.verdict.state = renodx::addons::dlss5::NrState::kOff;
  check(ui::ComputeStatusCard(inputs).id == ui::CardId::kOffByUser,
        "NR disabled remains off");
  window.Observe(3, 0, 0, 0, now);
  for (int i = 0; i < 12; ++i) {
    now += 100'000'000LL;
    sample = window.Observe(3, (i + 1) * 6, (i + 1) * 12, 0, now);
  }
  check(!sample.ready, "model pass counts cannot masquerade as successful source frames");
}
