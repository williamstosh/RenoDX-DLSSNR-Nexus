/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstdint>
#include <limits>

#include "../gpu_comparison.hpp"

// Include after the host's Check(bool, const char*) declaration.
namespace gpu_comparison_cases {
namespace comparison = renodx::addons::dlss5::gpu_comparison;

inline void Run() {
  using namespace comparison;
  const auto make_sample = [](uint32_t stage, uint64_t sequence, uint64_t recorded_ns,
                              bool lower = false, double cost = 100.0) {
    Sample sample{
        .stage = stage, .sequence = sequence, .recorded_ns = recorded_ns, .completed_ns = recorded_ns + 1,
        .valid = true, .stable = true,
        .identity = {.configuration_generation = 1, .settings_epoch = 1, .settings_hash = 42,
                     .resolution_hash = (lower ? 2u : 1u), .config_epoch = 1, .stable_epoch = 1,
                     .source_generation = 8, .source_feature = 9, .device_session = 10, .queue = 11,
                     .route = 1, .fg = 2, .hdr = 3, .sr_mode = 4, .processing_hash = 5, .display_format = 6,
                     .sr_width = 1920, .sr_height = 1080, .out_width = 3840, .out_height = 2160},
        .pass_count = 1, .total_us = cost + 20};
    sample.passes[0] = Pass{.id = stage, .model = 2, .width = (lower ? 960u : 1920u),
                            .height = (lower ? 540u : 1080u), .output_width = 3840, .output_height = 2160,
                            .eval_us = cost, .state = PassState::VALID, .look_executed = true};
    return sample;
  };
  const auto fill = [&](Engine* engine, uint64_t start_ns, uint32_t mask, bool lower = false, double cost = 100.0) {
    for (uint64_t i = 0; i < SAMPLE_COUNT; ++i) {
      for (uint32_t stage = 0; stage < STAGE_COUNT; ++stage) {
        if ((mask & (1u << stage)) == 0) continue;
        const auto recorded_ns = start_ns + STABILITY_NS + i * INTERVAL_NS;
        engine->Observe(make_sample(stage, i + 10, recorded_ns, lower && stage == 0, cost), recorded_ns + 1);
      }
    }
  };
  constexpr uint64_t B_START = 6'000'000'000;
  Engine engine{};
  Check(!engine.Start(true, 0, 0, 0) && !engine.Start(true, 8, 0, 0), "comparison rejects empty/unknown stage mask");
  Check(!engine.Start(false, 1, 0, 0), "comparison B requires completed A");
  Check(engine.Start(true, 1, 0, 10), "comparison starts A");
  Check(!engine.Start(false, 1, 0, 10), "comparison cannot switch active A to B");
  engine.Observe(make_sample(0, 9, STABILITY_NS), STABILITY_NS + 1);
  engine.Observe(make_sample(0, 10, STABILITY_NS - 1), STABILITY_NS);
  Check(engine.baseline[0].count == 0, "comparison minimum sequence and initial warmup");
  fill(&engine, 0, 1);
  // Sequence 10 was already observed during warmup, so only 15 fresh records remain.
  Check(engine.baseline[0].count == 15, "comparison rejected records cannot later replay as fresh");
  engine.Observe(make_sample(0, 30, 5'000'000'000), 5'000'000'001);
  Check(engine.baseline[0].status == Status::COMPLETE && engine.control[0].count == 0, "comparison A/B storage ownership");
  Check(!engine.Start(false, 3, B_START, 0), "comparison stage mask must match baseline");
  Check(engine.Start(false, 1, B_START, 10), "comparison starts independent B window");
  engine.Observe(make_sample(0, 100, 5'500'000'000), B_START + 1);
  Check(engine.control[0].count == 0, "comparison late A callback cannot enter B");
  fill(&engine, B_START, 1, true, 80);
  auto result = engine.Results();
  Check(result.reason == Reason::NONE && result.stages[0].selected && !result.stages[1].selected,
        "comparison selected stages stay independent");
  Check(result.stages[0].passes[0].a.median_us == 100 && result.stages[0].passes[0].b.median_us == 80
            && result.stages[0].passes[0].a.mad_us == 0 && result.stages[0].passes[0].delta_us == -20
            && result.stages[0].passes[0].percent_valid && result.stages[0].passes[0].percent == -20,
        "comparison median MAD delta and baseline-relative percent");
  Check(result.stages[0].passes[0].advice == Advice::LOWER_RESOLUTION_LOWER_MEASURED_COST_CHECK_QUALITY
            && result.stages[0].total.a.median_us == 120 && result.stages[0].total.b.median_us == 100,
        "comparison only measured GPU cost advice and stage total");

  engine.Reset();
  Check(engine.baseline[0].status == Status::IDLE && engine.Results().reason == Reason::MISSING_DATA,
        "comparison Reset clears both windows");
  Check(engine.Start(true, 3, 0, 0), "comparison multi-stage A");
  fill(&engine, 0, 3);
  Check(engine.Start(false, 3, B_START, 0), "comparison multi-stage B");
  fill(&engine, B_START, 3, true, 80);
  result = engine.Results();
  Check(result.reason == Reason::NONE && result.stages[0].total.b.median_us == 100
            && result.stages[1].total.b.median_us == 100
            && result.stages[1].passes[0].advice == Advice::NONE,
        "comparison never sums cross-stage intervals or recommends unchanged pass");

  // Mutate completed synthetic evidence to isolate each cross-window comparison condition.
  const auto reference = engine;
  engine.control[0].identity.configuration_generation++;
  engine.control[0].identity.settings_epoch++;
  engine.control[0].identity.config_epoch++;
  engine.control[0].identity.stable_epoch++;
  engine.control[0].identity.resolution_hash++;
  Check(engine.Results().reason == Reason::NONE, "comparison rebuild/config epochs are local to A or B");
  for (auto member : {&Identity::settings_hash, &Identity::source_generation, &Identity::source_feature,
                      &Identity::device_session, &Identity::queue, &Identity::route, &Identity::fg, &Identity::hdr,
                      &Identity::sr_mode, &Identity::processing_hash, &Identity::display_format}) {
    engine = reference;
    engine.control[0].identity.*member += 1;
    Check(engine.Results().reason == Reason::INCOMPATIBLE_IDENTITY, "comparison rejects fixed identity confounding");
  }
  for (auto member : {&Identity::sr_width, &Identity::sr_height, &Identity::out_width, &Identity::out_height}) {
    engine = reference;
    engine.control[0].identity.*member += 1;
    Check(engine.Results().reason == Reason::INCOMPATIBLE_IDENTITY, "comparison rejects SR/display changes");
  }
  engine = reference;
  engine.control[1].passes[0].width /= 2;
  Check(engine.Results().reason == Reason::MULTIPLE_RESOLUTION_CHANGES, "comparison one changed pass globally");
  engine = reference;
  engine.control[0].passes[0].width = 1920;
  engine.control[0].passes[0].height = 1080;
  Check(engine.Results().reason == Reason::NO_RESOLUTION_CHANGE, "comparison actual dimensions must change");
  engine = reference;
  engine.control[0].passes[0].height = 2160;
  Check(engine.Results().reason == Reason::AMBIGUOUS_RESOLUTION, "comparison mixed dimension direction is ambiguous");
  for (auto member : {&Pass::id, &Pass::model}) {
    engine = reference;
    engine.control[0].passes[0].*member += 1;
    Check(engine.Results().reason == Reason::PASS_MISMATCH, "comparison model/pass/output identity");
  }
  engine = reference;
  engine.control[0].passes[0].output_width /= 2;
  engine.control[0].passes[0].output_height /= 2;
  Check(engine.Results().reason == Reason::NONE, "comparison NN output may follow working resolution");
  engine.control[1].passes[0].output_width /= 2;
  Check(engine.Results().reason == Reason::AMBIGUOUS_RESOLUTION, "comparison unchanged pass NN output cannot change");
  engine = reference;
  engine.control[0].passes[0].look_executed = false;
  Check(engine.Results().reason == Reason::PASS_MISMATCH, "comparison look execution must match");
  engine = reference;
  engine.control[0].status = Status::COLLECTING;
  engine.control[0].count = 15;
  Check(engine.Results().reason == Reason::MISSING_DATA, "comparison all sixteen samples required in every stage");

  for (auto member : {&Identity::configuration_generation, &Identity::settings_epoch, &Identity::config_epoch,
                      &Identity::stable_epoch, &Identity::settings_hash, &Identity::resolution_hash}) {
    engine.Reset();
    Check(engine.Start(true, 1, 0, 0), "comparison local identity setup");
    engine.Observe(make_sample(0, 1, STABILITY_NS), STABILITY_NS + 1);
    auto changed = make_sample(0, 2, STABILITY_NS + INTERVAL_NS);
    changed.identity.*member += 1;
    engine.Observe(changed, changed.completed_ns);
    Check(engine.baseline[0].status == Status::FAILED && engine.baseline[0].reason == Reason::IDENTITY_CHANGED,
          "comparison configuration/reset change after first sample fails window");
  }
  engine.Reset();
  Check(engine.Start(true, 1, 0, 0), "comparison warmup setup");
  auto warmup = make_sample(0, 1, STABILITY_NS);
  warmup.stable = false;
  engine.Observe(warmup, warmup.completed_ns);
  auto rebuilt = make_sample(0, 2, STABILITY_NS + INTERVAL_NS);
  rebuilt.identity.stable_epoch++;
  rebuilt.stable = false;
  rebuilt.stable_after_ns = rebuilt.recorded_ns + STABILITY_NS;
  engine.Observe(rebuilt, rebuilt.completed_ns);
  rebuilt.sequence++;
  rebuilt.recorded_ns = 2 * STABILITY_NS;
  rebuilt.completed_ns = rebuilt.recorded_ns + 1;
  rebuilt.stable = true;
  engine.Observe(rebuilt, rebuilt.completed_ns);
  Check(engine.baseline[0].count == 0, "comparison delayed publication cannot bypass reset warmup");
  rebuilt.sequence++;
  rebuilt.recorded_ns += INTERVAL_NS;
  rebuilt.completed_ns = rebuilt.recorded_ns + 1;
  engine.Observe(rebuilt, rebuilt.completed_ns);
  Check(engine.baseline[0].count == 1, "comparison stable epoch may change before first sample");
  const auto accepted_after_reset = engine.baseline[0].samples[0].recorded_ns;
  Check(accepted_after_reset == rebuilt.stable_after_ns,
        "comparison accepts exact upstream settling deadline without another one-second delay");
  rebuilt.sequence++;
  rebuilt.recorded_ns += INTERVAL_NS;
  rebuilt.completed_ns = rebuilt.recorded_ns + 1;
  rebuilt.stable = false;
  engine.Observe(rebuilt, rebuilt.completed_ns);
  Check(engine.baseline[0].status == Status::FAILED, "comparison mid-window instability cannot mix samples");

  engine.Reset();
  Check(engine.Start(true, 1, 0, 0), "comparison rejection setup");
  for (uint64_t i = 0; i < 6; ++i) {
    auto invalid = make_sample(0, i + 1, STABILITY_NS + i * INTERVAL_NS);
    invalid.valid = false;
    engine.Observe(invalid, invalid.completed_ns);
  }
  Check(engine.baseline[0].count == 0, "comparison upstream unproven/discard/replay/expired/warmup cannot count");
  auto sample = make_sample(0, 10, 3 * STABILITY_NS);
  engine.Observe(sample, sample.completed_ns);
  engine.Observe(sample, sample.completed_ns + 100);
  sample.sequence = 9;
  engine.Observe(sample, sample.completed_ns + 100);
  sample.sequence = 11;
  sample.recorded_ns += INTERVAL_NS - 1;
  sample.completed_ns = sample.recorded_ns + 1;
  engine.Observe(sample, sample.completed_ns);
  Check(engine.baseline[0].count == 1, "comparison sequence replay/old/250ms cadence exclusions");
  sample.sequence++;
  sample.recorded_ns++;
  sample.completed_ns++;
  engine.Observe(sample, sample.completed_ns);
  Check(engine.baseline[0].count == 2, "comparison cadence accepts exact boundary");
  engine.Observe(Sample{.sequence = 999, .recorded_ns = sample.recorded_ns, .completed_ns = sample.completed_ns},
                 sample.completed_ns);
  Check(engine.baseline[0].status == Status::COLLECTING && engine.baseline[0].last_sequence == 12,
        "comparison invalid empty identity cannot fail window or poison sequence");
  sample.sequence++;
  sample.recorded_ns += INTERVAL_NS;
  sample.completed_ns = sample.recorded_ns - 1;
  engine.Observe(sample, sample.recorded_ns + 1);
  sample.completed_ns = sample.recorded_ns + 2;
  engine.Observe(sample, sample.recorded_ns + 1);
  sample.completed_ns = sample.recorded_ns + 1;
  sample.passes[0].eval_us = std::numeric_limits<double>::quiet_NaN();
  engine.Observe(sample, sample.completed_ns);
  sample.sequence++;
  sample.passes[0].eval_us = -1;
  engine.Observe(sample, sample.completed_ns);
  sample.sequence++;
  sample.passes[0].eval_us = 100;
  sample.total_us = std::numeric_limits<double>::infinity();
  engine.Observe(sample, sample.completed_ns);
  Check(engine.baseline[0].count == 2, "comparison invalid chronology and numeric data excluded");
  engine.Tick(TIMEOUT_NS);
  Check(engine.baseline[0].status == Status::FAILED && engine.baseline[0].reason == Reason::TIMEOUT,
        "comparison bounded timeout exact boundary");
  engine = reference;
  engine.control[1].status = Status::COLLECTING;
  engine.Tick(B_START + TIMEOUT_NS);
  Check(engine.Results().reason == Reason::TIMEOUT, "comparison one missing stage fails aggregate");

  engine = reference;
  for (size_t i = 0; i < SAMPLE_COUNT; ++i) {
    engine.baseline[0].samples[i].eval_us[0] = (i < 8 ? 60 : 140);
  }
  result = engine.Results();
  Check(result.stages[0].passes[0].a.median_us == 100 && result.stages[0].passes[0].a.mad_us == 40
            && result.stages[0].passes[0].a.relative_mad == 0.4
            && result.stages[0].passes[0].reason == Reason::HIGH_VARIATION
            && result.stages[0].passes[0].advice == Advice::NONE, "comparison high MAD refuses advice");
  engine = reference;
  for (size_t i = 0; i < SAMPLE_COUNT; ++i) {
    engine.baseline[0].samples[i].eval_us[0] = (i < 8 ? 94 : 106);
    engine.control[0].samples[i].eval_us[0] = (i < 8 ? 84 : 96);
  }
  Check(engine.Results().stages[0].passes[0].reason == Reason::INSUFFICIENT_DIFFERENCE,
        "comparison difference must cover twice combined MAD");
  for (double baseline_cost : {0.0, 10.0, 100.0}) {
    engine = reference;
    for (auto& raw : engine.baseline[0].samples) raw.eval_us[0] = baseline_cost;
    for (auto& raw : engine.control[0].samples) raw.eval_us[0] = baseline_cost * 0.98;
    const auto metric = engine.Results().stages[0].passes[0];
    Check(metric.advice == Advice::NONE, "comparison zero/sub-us/sub-three-percent refuses advice");
    Check(metric.reason == (baseline_cost == 0 ? Reason::ZERO_BASELINE : Reason::INSUFFICIENT_DIFFERENCE),
          "comparison zero and insufficient reason");
    Check(metric.percent_valid == (baseline_cost != 0), "comparison zero baseline never divides");
  }
  engine = reference;
  for (auto& raw : engine.control[0].samples) raw.eval_us[0] = 120;
  Check(engine.Results().stages[0].passes[0].advice == Advice::NONE,
        "comparison lower resolution with higher measured cost has no positive suggestion");
  engine = reference;
  engine.baseline[0].passes[0].width = 480;
  engine.baseline[0].passes[0].height = 270;
  for (auto& raw : engine.control[0].samples) raw.eval_us[0] = 120;
  Check(engine.Results().stages[0].passes[0].advice == Advice::LOWER_RESOLUTION_LOWER_MEASURED_COST_CHECK_QUALITY,
        "comparison A can be the lower resolution lower cost side");

  engine.Reset();
  Check(engine.Start(true, 1, 0, 0), "comparison multi-pass A setup");
  for (uint32_t side = 0; side < 2; ++side) {
    const auto start_ns = (side == 0 ? uint64_t{0} : B_START);
    if (side != 0) Check(engine.Start(false, 1, start_ns, 0), "comparison multi-pass B setup");
    for (uint64_t i = 0; i < SAMPLE_COUNT; ++i) {
      auto multi = make_sample(0, i + 1, start_ns + STABILITY_NS + i * INTERVAL_NS, side != 0);
      multi.pass_count = 2;
      multi.passes[1] = multi.passes[0];
      multi.passes[1].id = 3;
      multi.passes[1].width = 1920;
      multi.passes[1].height = 1080;
      multi.passes[1].eval_us = 10 + static_cast<double>(i);
      multi.total_us = 200 + static_cast<double>(i);
      engine.Observe(multi, multi.completed_ns);
    }
  }
  result = engine.Results();
  Check(result.reason == Reason::NONE && result.stages[0].pass_count == 2
            && result.stages[0].passes[1].a.median_us == 17.5 && result.stages[0].passes[1].a.mad_us == 4
            && result.stages[0].total.a.median_us == 207.5 && result.stages[0].total.a.mad_us == 4,
        "comparison raw per-pass and stage medians/MAD computed independently");
  engine.Reset();
  Check(engine.Start(true, 1, 0, 0), "comparison malformed pass setup");
  for (uint64_t i = 0; i < 5; ++i) {
    auto malformed = make_sample(0, i + 1, STABILITY_NS + i * INTERVAL_NS);
    switch (i) {
      case 0: malformed.pass_count = 0; break;
      case 1: malformed.pass_count = MAX_PASSES + 1; break;
      case 2: malformed.passes[0].state = PassState::NOT_EXECUTED; break;
      case 3: malformed.passes[0].width = 0; break;
      case 4:
        malformed.pass_count = 2;
        malformed.passes[1] = malformed.passes[0];
        break;
    }
    engine.Observe(malformed, malformed.completed_ns);
  }
  Check(engine.baseline[0].count == 0, "comparison missing/unexecuted/zero-sized/duplicate passes excluded");
  engine.Tick(TIMEOUT_NS - 1);
  Check(engine.baseline[0].status == Status::COLLECTING, "comparison timeout does not fire early");
}

}  // namespace gpu_comparison_cases
