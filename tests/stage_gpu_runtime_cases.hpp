/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Composed inside capability-ui after stage_edit_runtime_cases.hpp. Only the
// timer observations are synthetic; collection, card ticks and F6 are production.
{
  namespace timers = renodx::addons::dlss5::gpu_timers;
  namespace cards = renodx::addons::dlss5::ui;
  namespace nr = renodx::addons::dlss5;
  const auto saved_settings = profile::Snapshot();
  const auto saved_enabled = runtime::enabled.load();
  const auto saved_enabled_ini = Read("NeuralUplift");
  const auto saved_samples = std::array{timers::stage_stats[0], timers::stage_stats[1], timers::stage_stats[2]};
  const auto saved_proofs = std::array{timers::execution_stats[0], timers::execution_stats[1], timers::execution_stats[2]};
  const auto saved_profiling = runtime::gpu_timers_enabled.load();
  const auto saved_cost_meter = runtime::cost_meter_enabled.load();
  const auto saved_sequence = timers::internal::next_sequence;
  const auto saved_window = runtime::card_observation.window;
  const auto saved_configuration = runtime::card_observation.configuration;
  const auto saved_declines = runtime::card_observation.declines;
  const auto saved_declines_sampled = runtime::card_observation.declines_sampled_ns;
  const auto saved_last_decline_ns = runtime::card_observation.last_decline_ns;
  const auto* saved_last_decline = runtime::card_observation.last_decline;
  const auto saved_features = runtime::active_features.load();
  const auto saved_health = runtime::health_active_mask.load();
  const auto saved_partial = runtime::card_partial_latched.load();
  const auto saved_hud_enabled = runtime::hud_enabled.load();
  const auto saved_pill = runtime::hud_pill.load();
  const auto saved_recovery = runtime::hud_recovery_pending;
  const auto saved_toast = runtime::hud_toast_until_ns.load();
  const auto saved_card = runtime::last_emitted_card.load();
  const auto saved_tokens = runtime::card_line_tokens;
  const auto saved_refill = runtime::card_line_refill_ns;
  const auto saved_feedback_deadline = runtime::profile_feedback_until_ns.load();
  std::string saved_feedback_message;
  const char* saved_operation = nullptr;
  {
    std::shared_lock lock(runtime::profile_feedback.mutex);
    saved_feedback_message = runtime::profile_feedback.message;
    saved_operation = runtime::profile_feedback.operation;
  }

  Check(apply(pair), "GPU card fixture applies the supported same-stage pair.");
  runtime::active_features = 1;
  runtime::health_active_mask = 0;
  runtime::card_partial_latched = false;
  runtime::hud_enabled = true;
  runtime::hud_pill = false;
  runtime::hud_recovery_pending = false;
  runtime::hud_toast_until_ns = 0;
  runtime::card_line_tokens = runtime::kCardLineBurst;
  runtime::card_line_refill_ns = 0;
  runtime::last_emitted_card = 0xff;
  runtime::card_observation.window = {};
  for (auto& sample : timers::stage_stats) {
    sample = {};
  }
  for (auto& proof : timers::execution_stats) {
    proof = {};
  }
  int64_t tick_ns = runtime::SteadyNowNs();
  nr::NrVerdict verdict{.state = nr::NrState::kEngaged, .seen = 1, .eligible = 1,
                        .injected = 1, .ratio = 1.0, .api = "d3d12", .path = "inline"};

  // Sample publication is the substituted boundary. Do not reproduce the
  // collector's planner, freshness or identity validation in this fixture.
  const auto publish = [&](plan::Stage stage, uint32_t first_pass, uint32_t count) {
    auto& sample = timers::stage_stats[static_cast<uint32_t>(stage)];
    sample = {};
    sample.valid = true;
    sample.state = timers::SampleState::VALID;
    sample.sequence = ++timers::internal::next_sequence;
    sample.measured_ns = tick_ns;
    sample.recorded_ns = tick_ns;
    sample.identity.device_session = timers::device_session;
    runtime::comparison_ui::CaptureSettings(&sample.identity);
    for (uint32_t local = 0; local < count; ++local) {
      sample.passes[local].id = first_pass + local;
      sample.passes[local].state = timers::SampleState::VALID;
    }
  };
  const auto expect_checking = [&](const char* description) {
    const auto inputs = runtime::CollectCardInputs(verdict, tick_ns);
    Check(inputs.recent.has_value() && !inputs.recent->gpu_completed
              && cards::ComputeStatusCard(inputs).id == cards::CardId::kStarting, description);
    runtime::card_line_tokens = runtime::kCardLineBurst;
    runtime::MaybeEmitStatusCard(verdict, tick_ns);
    Check(runtime::last_emitted_card.load() == static_cast<uint8_t>(cards::CardId::kStarting),
          "Production status tick keeps unproven GPU execution in Checking.");
  };
  const auto expect_active = [&](const char* description) {
    const auto inputs = runtime::CollectCardInputs(verdict, tick_ns);
    Check(inputs.recent.has_value() && inputs.recent->gpu_completed
              && cards::ComputeStatusCard(inputs).id == cards::CardId::kActive, description);
    runtime::card_line_tokens = runtime::kCardLineBurst;
    runtime::MaybeEmitStatusCard(verdict, tick_ns);
    Check(runtime::last_emitted_card.load() == static_cast<uint8_t>(cards::CardId::kActive),
          "Production status tick publishes Active from current GPU evidence.");
  };

  publish(plan::Stage::UPSCALED, 0, 2);
  expect_checking("A completed sample already present at a new configuration baseline is not new evidence.");
  runtime::RecreateFeatures();
  ++tick_ns;
  publish(plan::Stage::UPSCALED, 0, 2);
  auto& upscaled = timers::stage_stats[static_cast<uint32_t>(plan::Stage::UPSCALED)];
  upscaled.valid = false;
  upscaled.state = timers::SampleState::PENDING;
  expect_checking("A recording pending when the new baseline is taken is not completed evidence.");
  ++tick_ns;
  upscaled.valid = true;
  upscaled.state = timers::SampleState::VALID;
  upscaled.measured_ns = tick_ns;
  expect_checking("Late completion of the baseline's pending sequence cannot activate the card.");

  ++tick_ns;
  publish(plan::Stage::UPSCALED, 0, 2);
  upscaled.state = timers::SampleState::PENDING;
  expect_checking("A new sequence still marked Pending cannot prove GPU completion.");
  upscaled.state = timers::SampleState::VALID;
  upscaled.passes[1].state = timers::SampleState::NOT_EXECUTED;
  expect_checking("A valid group timing without every requested Pass executed cannot activate NR.");
  upscaled.passes[1].state = timers::SampleState::VALID;
  upscaled.passes[1].id = 0;
  expect_checking("Duplicate Pass IDs cannot prove both Passes of a single Stage.");
  upscaled.passes[1].id = 1;
  expect_active("All current Passes in one Stage confirm GPU execution before the CPU window matures.");
  {
    const auto inputs = runtime::CollectCardInputs(verdict, tick_ns);
    Check(!inputs.recent->ready && inputs.recent->eligible < cards::RecentHealthWindow::kMinSamples,
          "Early GPU confirmation requires neither 60 source frames nor the 750ms CPU window.");
  }

  auto chain = pair;
  chain["NRHookPoint"] = 1u;
  chain["NRPass2Stage"] = 2u;
  Check(apply(chain), "GPU card fixture applies the supported Render to Upscaled chain.");
  ++tick_ns;
  expect_checking("A Stage change starts a new observation baseline.");
  ++tick_ns;
  publish(plan::Stage::RENDER, 0, 1);
  expect_checking("Current Render evidence alone cannot prove the Render to Upscaled chain.");
  timers::stage_stats[static_cast<uint32_t>(plan::Stage::RENDER)].valid = false;
  publish(plan::Stage::UPSCALED, 1, 1);
  expect_checking("Current Upscaled evidence alone cannot prove the Render to Upscaled chain.");
  publish(plan::Stage::RENDER, 0, 1);
  upscaled.passes[0].id = 0;
  expect_checking("A group-local zero ID in Upscaled cannot replace the requested global Pass 1.");
  upscaled.passes[0].id = 1;
  expect_active("Both StageGroups with matching global Pass IDs prove the current chain.");

  // Use actual setting / recreation entry points, leaving the old published
  // samples in place so rejection cannot be explained by clearing the fixture.
  for (uint32_t change = 0; change < 3; ++change) {
    const auto old_identity = upscaled.identity;
    const auto generation = runtime::configuration_generation.load();
    if (change == 0) {
      runtime::kSetPreset.Commit(2.f);
    } else if (change == 1) {
      runtime::kSetPassResolutionScale[0].Commit(.5f);
    } else {
      // Exercise the production rebuild boundary without polling OS keys.
      runtime::RecreateFeatures();
      Check(runtime::configuration_generation.load() == generation + 1,
            "Feature recreation advances the configuration generation.");
    }
    ++tick_ns;
    Check(upscaled.identity.configuration_generation == old_identity.configuration_generation
              && upscaled.identity.settings_hash == old_identity.settings_hash
              && upscaled.identity.resolution_hash == old_identity.resolution_hash
              && !runtime::CurrentGpuSample(upscaled, tick_ns),
          "Model / resolution / rebuild rejects unchanged old sample identity.");
    expect_checking("Model / resolution / feature recreation cannot reuse the previous chain's samples.");
    ++tick_ns;
    publish(plan::Stage::RENDER, 0, 1);
    expect_checking("Refreshing just Render after a settings change still cannot prove the whole chain.");
    publish(plan::Stage::UPSCALED, 1, 1);
    expect_active("Fresh samples for every StageGroup restore confirmation after a settings change.");
  }

  // Keep refreshing one group across expiry: its sample cannot hide an expired
  // peer, even when the CPU counters describe a fully healthy source window.
  const auto completed_ns = tick_ns;
  tick_ns += 800'000'000LL;
  verdict.seen = verdict.eligible = verdict.injected = 81;
  expect_active("Current GPU evidence remains usable while the healthy CPU window matures.");
  tick_ns = completed_ns + 2'000'000'000LL;
  verdict.seen = verdict.eligible = verdict.injected = 161;
  expect_active("GPU completion remains current at the inclusive two-second boundary.");
  ++tick_ns;
  publish(plan::Stage::RENDER, 0, 1);
  expect_checking("A fresh Render sample cannot hide Upscaled evidence older than two seconds.");
  {
    const auto inputs = runtime::CollectCardInputs(verdict, tick_ns);
    Check(inputs.recent->ready && !inputs.recent->partial && !inputs.recent->gpu_completed,
          "A healthy mature CPU window never substitutes for expired GPU execution evidence.");
  }
  publish(plan::Stage::UPSCALED, 1, 1);
  expect_active("Refreshing the expired peer restores current full-chain confirmation.");

  runtime::gpu_timers_enabled = false;
  runtime::cost_meter_enabled = false;
  for (auto& sample : timers::stage_stats) sample = {};
  ++tick_ns;
  expect_checking("Turning off optional timing does not invent completion evidence.");
  const auto publish_proof = [&](plan::Stage stage, uint32_t first_pass, uint32_t count) {
    publish(stage, first_pass, count);
    const auto index = static_cast<uint32_t>(stage);
    timers::execution_stats[index] = timers::stage_stats[index];
    timers::stage_stats[index] = {};
  };
  publish_proof(plan::Stage::RENDER, 0, 1);
  expect_checking("Completion-only proof still requires every requested StageGroup.");
  publish_proof(plan::Stage::UPSCALED, 1, 1);
  expect_active("The full completion-only chain confirms NR with both timing options off.");
  Check(!timers::stage_stats[0].valid && !timers::stage_stats[1].valid
            && timers::execution_stats[0].total_us == 0 && timers::execution_stats[1].total_us == 0,
        "Completion-only status never publishes a zero-duration timing measurement.");
  runtime::RecreateFeatures();
  ++tick_ns;
  expect_checking("Recreation rejects old completion-only evidence just like measured evidence.");
  publish_proof(plan::Stage::RENDER, 0, 1);
  publish_proof(plan::Stage::UPSCALED, 1, 1);
  expect_active("New completion-only evidence restores the full-chain status after recreation.");

  // F6 must report the operation immediately, independently of execution and
  // independently of whether ReShade's overlay reader handled the key.
  const auto retained_sample = timers::execution_stats[static_cast<uint32_t>(plan::Stage::UPSCALED)];
  for (const bool overlay_open : {false, true}) {
    for (const bool next : {false, true}) {
      runtime::profile_feedback_until_ns = 0;
      {
        std::unique_lock lock(runtime::profile_feedback.mutex);
        runtime::profile_feedback.operation = nullptr;
      }
      runtime::hud_toast_until_ns = 0;
      const auto before_toggle = runtime::SteadyNowNs();
      runtime::ToggleNrFromHotkey(VK_F6, overlay_open);
      const auto after_toggle = runtime::SteadyNowNs();
      Check(runtime::enabled.load() == next && Read("NeuralUplift") == (next ? "1" : "0"),
            "The production F6 entry toggles and persists NR through the isolated INI boundary.");
      {
        std::shared_lock lock(runtime::profile_feedback.mutex);
        Check(runtime::profile_feedback.operation != nullptr
                  && std::strcmp(runtime::profile_feedback.operation, next ? "NR enabled" : "NR disabled") == 0,
              "F6 immediately publishes the enabled / disabled operation through existing profile_feedback.");
      }
      Check(runtime::profile_feedback_until_ns.load() >= before_toggle + runtime::kHudToastNs
                && runtime::profile_feedback_until_ns.load() <= after_toggle + runtime::kHudToastNs
                && runtime::hud_toast_until_ns.load() == 0,
            "F6 opens the existing operation deadline without requiring a status recovery Toast.");
      Check(!runtime::CurrentGpuSample(retained_sample, tick_ns),
            "Disabled NR / re-enabled recreation cannot reuse the old GPU sample.");
      ++tick_ns;
      if (next) {
        expect_checking("The F6 On operation is visible while execution remains unconfirmed.");
      } else {
        auto off = verdict;
        off.state = nr::NrState::kOff;
        const auto inputs = runtime::CollectCardInputs(off, tick_ns);
        Check(inputs.recent.has_value() && !inputs.recent->gpu_completed
                  && cards::ComputeStatusCard(inputs).id == cards::CardId::kOffByUser,
              "F6 Off invalidates execution evidence and reports the user's disabled state.");
        runtime::MaybeEmitStatusCard(off, tick_ns);
        Check(runtime::last_emitted_card.load() == static_cast<uint8_t>(cards::CardId::kOffByUser),
              "The production status tick publishes Off after F6.");
      }
    }
  }

  Check(apply(saved_settings), "GPU card cases restore settings for the remaining capability fixture.");
  runtime::enabled = saved_enabled;
  Check(WritePrivateProfileStringA(runtime::kConfigSection, "NeuralUplift",
                                  saved_enabled_ini == "<absent>" ? nullptr : saved_enabled_ini.c_str(),
                                  ini_path.c_str()) != 0,
        "F6 cases restore the fixture's original enabled key, including absence.");
  for (uint32_t stage = 0; stage < saved_samples.size(); ++stage) {
    timers::stage_stats[stage] = saved_samples[stage];
    timers::execution_stats[stage] = saved_proofs[stage];
  }
  runtime::gpu_timers_enabled = saved_profiling;
  runtime::cost_meter_enabled = saved_cost_meter;
  timers::internal::next_sequence = saved_sequence;
  runtime::card_observation.window = saved_window;
  runtime::card_observation.configuration = saved_configuration;
  runtime::card_observation.declines = saved_declines;
  runtime::card_observation.declines_sampled_ns = saved_declines_sampled;
  runtime::card_observation.last_decline_ns = saved_last_decline_ns;
  runtime::card_observation.last_decline = saved_last_decline;
  runtime::active_features = saved_features;
  runtime::health_active_mask = saved_health;
  runtime::card_partial_latched = saved_partial;
  runtime::hud_enabled = saved_hud_enabled;
  runtime::hud_pill = saved_pill;
  runtime::hud_recovery_pending = saved_recovery;
  runtime::hud_toast_until_ns = saved_toast;
  runtime::last_emitted_card = saved_card;
  runtime::card_line_tokens = saved_tokens;
  runtime::card_line_refill_ns = saved_refill;
  {
    std::unique_lock lock(runtime::profile_feedback.mutex);
    runtime::profile_feedback.message = saved_feedback_message;
    runtime::profile_feedback.operation = saved_operation;
    runtime::profile_feedback_until_ns = saved_feedback_deadline;
  }
}
