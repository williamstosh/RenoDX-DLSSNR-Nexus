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
  inputs.recent->gpu_completed = true;
  check(ui::ComputeStatusCard(inputs).tone == ui::CardTone::kGood,
        "status uses recent source ratio instead of session history");
  inputs.recent->ready = false;
  inputs.recent->gpu_completed = false;
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

  // A proven current GPU completion should not wait for the source-rate window.
  inputs.verdict = verdict;
  inputs.active_features = 1;
  inputs.recent = ui::RecentHealth{.eligible = 1, .injected = 1};
  inputs.issues = 1;
  check(ui::ComputeStatusCard(inputs).tone == ui::CardTone::kWarn,
        "insufficient source samples cannot hide current warnings");
  inputs.issues = 0;
  inputs.recent->partial = true;
  check(ui::ComputeStatusCard(inputs).id == ui::CardId::kPartlyActive,
        "insufficient source samples cannot hide a latched partial result");

  ui::RecentHealthWindow gpu_window;
  now = 1'000'000'000LL;
  ui::RecentGpuCompletion completion{.current = true, .sequence = 50, .completed_ns = now};
  sample = gpu_window.Observe(1, eligible, injected, gaps, now, 50, completion);
  check(!sample.ready && !sample.gpu_completed, "a completed sample at the baseline is historical");
  now += 100'000'000LL;
  sample = gpu_window.Observe(1, ++eligible, ++injected, gaps, now, 51);
  inputs.recent = sample;
  check(ui::ComputeStatusCard(inputs).id == ui::CardId::kStarting,
        "CPU injection alone cannot confirm the new settings early");
  completion.sequence = 51;
  completion.completed_ns = now;
  sample = gpu_window.Observe(1, eligible, injected, gaps, now, 51, completion);
  inputs.recent = sample;
  check(!sample.ready && sample.gpu_completed && ui::ComputeStatusCard(inputs).id == ui::CardId::kActive,
        "fresh matching GPU completion confirms execution before 60 frames or 750 ms");
  inputs.issues = 1;
  check(ui::ComputeStatusCard(inputs).id == ui::CardId::kActiveWithIssues,
        "early GPU confirmation still exposes current warnings");
  inputs.issues = 0;
  inputs.recent->partial = true;
  check(ui::ComputeStatusCard(inputs).id == ui::CardId::kPartlyActive,
        "early GPU confirmation cannot erase a partial result");
  inputs.verdict.state = renodx::addons::dlss5::NrState::kNotEngaged;
  check(ui::ComputeStatusCard(inputs).id != ui::CardId::kActive,
        "GPU completion cannot override a non-engaged verdict");
  inputs.verdict.state = renodx::addons::dlss5::NrState::kOff;
  check(ui::ComputeStatusCard(inputs).id == ui::CardId::kOffByUser,
        "GPU completion cannot override the disabled state");
  inputs.verdict = verdict;
  inputs.active_features = 0;
  check(ui::ComputeStatusCard(inputs).id == ui::CardId::kWaitingForDlss,
        "GPU completion cannot override missing live features");
  inputs.active_features = 1;

  completion.current = false;
  check(!gpu_window.Observe(1, eligible, injected, gaps, now, 51, completion).gpu_completed,
        "a GPU sample with a mismatched recording identity is rejected");
  completion.current = true;
  completion.completed_ns = now + 1;
  check(!gpu_window.Observe(1, eligible, injected, gaps, now, 51, completion).gpu_completed,
        "a future GPU completion is rejected");
  completion.completed_ns = 0;
  check(!gpu_window.Observe(1, eligible, injected, gaps, now, 51, completion).gpu_completed,
        "an undated GPU completion is rejected");
  completion.completed_ns = now;
  completion.sequence = 0;
  check(!gpu_window.Observe(1, eligible, injected, gaps, now, 51, completion).gpu_completed,
        "an unsequenced GPU completion is rejected");
  completion.sequence = 52;
  check(!gpu_window.Observe(1, eligible, injected, gaps, now, 51, completion).gpu_completed,
        "a GPU completion beyond the recorded sequence is rejected");
  completion.sequence = 51;
  const auto completed_ns = now;
  now += ui::RecentHealthWindow::kStaleNs;
  check(gpu_window.Observe(1, eligible, injected, gaps, now, 51, completion).gpu_completed,
        "a matching GPU completion is valid at the two-second boundary");
  ++now;
  sample = gpu_window.Observe(1, eligible, injected, gaps, now, 51, completion);
  inputs.recent = sample;
  check(!sample.gpu_completed && ui::ComputeStatusCard(inputs).id == ui::CardId::kStarting,
        "expired GPU evidence returns to checking when the health window is insufficient");
  completion.completed_ns = completed_ns;
  completion.sequence = 52;
  gpu_window.Observe(2, eligible, injected, gaps, now, 51);
  check(!gpu_window.Observe(2, eligible, injected, gaps, now, 52, completion).gpu_completed,
        "completion before the new observation baseline is rejected");

  // These actions must each change the observation identity at the adapter.
  const char* reset_cases[] = {"enable invalidates old GPU evidence", "disable invalidates old GPU evidence",
      "rebuild invalidates old GPU evidence", "F5 invalidates old GPU evidence",
      "configuration change invalidates old GPU evidence"};
  std::uint64_t sequence = 60;
  for (std::size_t i = 0; i < std::size(reset_cases); ++i) {
    now += 100'000'000LL;
    completion = {.current = true, .sequence = sequence, .completed_ns = now};
    sample = gpu_window.Observe(3 + i, eligible, injected, gaps, now, sequence, completion);
    check(!sample.ready && !sample.gpu_completed && sample.eligible == 0, reset_cases[i]);
    completion.sequence = ++sequence;
    now += 10'000'000LL;
    completion.completed_ns = now;
    check(gpu_window.Observe(3 + i, eligible, injected, gaps, now, sequence, completion).gpu_completed,
          "each reset accepts only a subsequently recorded GPU completion");
  }
}
