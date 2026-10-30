/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Composed after the configuration adapters. Only the selected GUI runtime
// owns this cache and ImGui geometry; other callbacks publish atomic intent.
struct MonitorSnapshot {
  NrVerdict verdict;
  ui::CardInputs inputs;
  ui::StatusCard card;
  ui::MonitorInputs monitor;
  ui::StatusSummaryFields summary;
  std::string execution;
  unsigned warnings = 0;
  uint64_t key = 0;
  int64_t sampled_ns = 0;
  int64_t attempted_ns = 0;
  int64_t valid_until_ns = INT64_MAX;
  bool complete = false;
  uint64_t refreshes = 0;
  uint64_t deferred = 0;
};
inline thread_local MonitorSnapshot monitor_snapshot;
inline constexpr int64_t kMonitorRefreshNs = 100'000'000;
inline uint64_t ui_monitor_seen_epoch = UINT64_MAX;
inline int ui_monitor_drawn_frame = -1;
inline bool ui_monitor_was_interactive = false;
inline bool ui_monitor_saved_detailed = false;
inline uint64_t ui_monitor_save_batches = 0;

struct MonitorCadence {
  const reshade::api::effect_runtime* owner = nullptr;
  int frame = -1;
  uint64_t epoch = 0;
  unsigned samples = 0;
  int64_t first_ns = 0;
  int64_t last_ns = 0;
  int64_t formatted_ns = 0;
  std::string text;
};
inline thread_local MonitorCadence monitor_cadence;

// ReShade uses IO.Framerate in its OSD, from its host frame duration.
// Warm up after a gap; this cadence does not identify FG output.
inline void UpdateMonitorCadence(reshade::api::effect_runtime* runtime) {
  const auto owner = monitor_runtime.load();
  if (runtime != nullptr && owner != nullptr && runtime != owner) return;
  const auto now = SteadyNowNs();
  const auto epoch = monitor_observation_epoch.load();
  auto& cadence = monitor_cadence;
  if (runtime == nullptr || cadence.owner != runtime || cadence.epoch != epoch
      || now < cadence.last_ns || now - cadence.last_ns > 500'000'000) {
    cadence = {.owner = runtime, .epoch = epoch, .first_ns = now};
  }
  if (runtime == nullptr || cadence.frame == ImGui::GetFrameCount()) return;
  cadence.frame = ImGui::GetFrameCount();
  cadence.last_ns = now;
  ++cadence.samples;
  const auto fps = ImGui::GetIO().Framerate;
  if (cadence.samples < 8 || now - cadence.first_ns < 750'000'000 || !std::isfinite(fps) || fps <= 0.f) {
    cadence.text.clear();
    return;
  }
  if (now - cadence.formatted_ns >= kMonitorRefreshNs) {
    char value[32];
    std::snprintf(value, sizeof(value), "%.0f", fps);
    cadence.text = value;
    cadence.formatted_ns = now;
  }
}

inline uint64_t MonitorSnapshotKey() {
  uint64_t key = 1469598103934665603ULL;
  const auto mix = [&](uint64_t value) { key = (key ^ value) * 1099511628211ULL; };
  mix(monitor_observation_epoch.load());
  mix(monitor_settings_epoch.load(std::memory_order_acquire));
  mix(configuration_generation.load());
  mix(profile_commit_epoch.load());
  mix(profile_reload_epoch.load());
  mix(enabled.load());
  mix(hook_point.load());
  mix(stack_passes.load());
  mix(stage_source_rr.load());
  mix(health_active_mask.load());
  mix(nr_runtime_fault.load());
  mix(active_features.load());
  mix(ui_language_choice.load());
  return key;
}

inline void InvalidateMonitorDisplay() {
  auto& cached = monitor_snapshot;
  cached.verdict = ComputeNrVerdict(CollectFunnelInputs());
  ui::CardInputs pending{.verdict = cached.verdict, .active_features = active_features.load()};
  pending.recent = ui::RecentHealth{};
  pending.runtime_fault = static_cast<ui::RuntimeFault>(nr_runtime_fault.load());
  cached.card = ui::ComputeStatusCard(pending);
  if (cached.card.tone == ui::CardTone::kGood || cached.card.title[0] == 0) {
    ui::card_internal::Set(cached.card, ui::CardId::kStarting, ui::CardTone::kIdle, ui::CardGlyph::kWaiting,
        "Checking Neural Rendering", "Waiting for a fresh GPU result for the current settings.");
  }
  cached.execution = ui::Tr("Execution stage pending confirmation");
  cached.monitor = {.card = cached.card};
  cached.summary = {};
  cached.summary.fg = ui::Tr(cached.card.fg_declared ? "On" : "Unknown");
  cached.complete = false;
  cached.valid_until_ns = INT64_MAX;
}

inline const MonitorSnapshot& RefreshMonitorSnapshot(bool need_monitor) {
  auto& cached = monitor_snapshot;
  cached.summary.fps = monitor_cadence.text;
  cached.monitor.summary.fps = monitor_cadence.text;
  const auto now = SteadyNowNs();
  const auto key = MonitorSnapshotKey();
  const bool changed = key != cached.key;
  if (!changed && (!need_monitor || cached.complete) && now <= cached.valid_until_ns && now >= cached.attempted_ns
      && now - cached.attempted_ns < kMonitorRefreshNs) return cached;
  if (changed || now > cached.valid_until_ns
      || (cached.sampled_ns != 0 && now - cached.sampled_ns > ui::RecentHealthWindow::kStaleNs)) {
    InvalidateMonitorDisplay();
  }
  cached.key = key;
  cached.attempted_ns = now;
  RuntimeTryLock lock(runtime_mutex);
  if (!lock.owns_lock()) {
    ++cached.deferred;
    return cached;
  }
  const auto verdict = ComputeNrVerdict(CollectFunnelInputs());
  const auto inputs = CollectCardInputs(verdict, now);
  auto card = ui::ComputeStatusCard(inputs);
  const auto health_mask = health_active_mask.load();
  const auto plan = RequestedStagePlan();
  const auto queried = need_monitor ? QueryConfigurationCapability() : capability::Result{};
  const auto timings = std::array{gpu_timers::stage_stats[0], gpu_timers::stage_stats[1], gpu_timers::stage_stats[2]};
  const auto proofs = std::array{gpu_timers::execution_stats[0], gpu_timers::execution_stats[1], gpu_timers::execution_stats[2]};
  std::array<bool, 3> timing_current{}, proof_current{};
  for (size_t stage = 0; stage < timings.size(); ++stage) {
    timing_current[stage] = CurrentGpuSample(timings[stage], now)
        && timings[stage].state == gpu_timers::SampleState::VALID;
    proof_current[stage] = CurrentGpuSample(proofs[stage], now)
        && proofs[stage].state == gpu_timers::SampleState::VALID;
    // A newer completion can prove a different working contract while an
    // older timed recording is still fresh. Do not attach its cost to that size.
    if (timing_current[stage] && proof_current[stage] && proofs[stage].sequence > timings[stage].sequence) {
      for (size_t pass = 0; pass < timings[stage].passes.size(); ++pass) {
        const auto& timed = timings[stage].passes[pass];
        const auto& completed = proofs[stage].passes[pass];
        timing_current[stage] &= timed.id == completed.id && timed.model == completed.model
            && timed.width == completed.width && timed.height == completed.height
            && timed.output_width == completed.output_width && timed.output_height == completed.output_height
            && timed.state == completed.state;
      }
    }
  }
  std::array<PassResolutionSettings, kMaxNrPasses> resolutions;
  std::array<uint32_t, kMaxNrPasses> models{};
  for (uint32_t pass = 0; pass < plan.pass_count; ++pass) {
    resolutions[pass] = CurrentPassResolution(pass);
    models[pass] = pass == 0 || pass_preset[pass - 1].load() == 0 ? preset.load() : pass_preset[pass - 1].load();
  }
  lock.unlock();

  ui_fg_route.Update({.enabled = enabled.load(), .present = hook_point.load() == kHookPresent,
      .prefg_active = present_prefg_active.load(), .upscaled_active = present_prefg_upscaled_active.load(),
      .submitted_before_fg = inputs.fg_continuing_submission, .multiplier = inputs.frame_gen_multiplier,
      .prefg_policy = present_pre_fg.load(), .frame_policy = present_frames.load(),
      .prefg_frames = present_prefg_frames.load(), .prefg_evaluates = present_prefg_evaluates.load(),
      .fallbacks = present_prefg_fallbacks.load(), .upscaled_frames = present_prefg_upscaled.load(),
      .captures = present_captures.load(), .nr_successes = successful_evaluations.load() + successful_pre_sr_evaluations.load(),
      .fg_submitted = TotalFgSubmittedFrames()}, now);
  card.observed_fg_route = ui_fg_route.route;
  card.fg_declared = fg::xefg::OwnsStream() || native_fg_declaration.load() == NativeFgDeclaration::kOn;
  unsigned warnings = card.tone == ui::CardTone::kWarn || card.tone == ui::CardTone::kBad ? 1u : 0u;
  for (size_t issue = 0; issue < health::kRowCount; ++issue) {
    warnings += ((health_mask >> issue) & 1u) != 0 && health::kRows[issue].tone == health::Tone::kWarn;
  }
  warnings += streamline_hooks_enabled.load() && hooks_enabled.load();
  warnings += card.present_gaps != 0;
  std::string execution;
  for (size_t stage = 0; stage < timings.size(); ++stage) {
    if (!timing_current[stage] && !proof_current[stage]) continue;
    if (!execution.empty()) execution += " + ";
    execution += stage_plan::Name(static_cast<stage_plan::Stage>(stage));
  }
  if (execution.empty()) execution = ui::Tr("Execution stage pending confirmation");
  // Stage samples have independent cadence and smoothing windows. Neither
  // completion time nor queue equality establishes a shared execution cycle.
  card.nr_gpu_ms = card.nr_present_ms = 0.f;
  if (plan.valid && plan.group_count == 1) {
    const auto& group = plan.groups[0];
    const auto stage = static_cast<size_t>(group.stage);
    const auto& timing = timings[stage];
    bool complete = timing_current[stage];
    for (uint32_t local = 0; local < gpu_timers::kTimerMaxPasses; ++local) {
      const auto& pass = timing.passes[local];
      if (local < group.count) {
        complete &= pass.id == group.begin + local && pass.state == gpu_timers::SampleState::VALID
            && pass.width != 0 && pass.height != 0;
      } else {
        complete &= pass.state != gpu_timers::SampleState::VALID;
      }
    }
    if (complete) card.nr_gpu_ms = timing.total_us / 1000.f;
  }
  ui::StatusSummaryFields summary;
  summary.fps = monitor_cadence.text;
  summary.stage = execution;
  summary.fg = ui::Tr(card.fg_declared || card.frame_gen_multiplier > 1 ? "On"
      : native_fg_declaration.load() == NativeFgDeclaration::kOff ? "Off" : "Unknown");
  if (card.nr_gpu_ms > 0.f) {
    char cost[64];
    std::snprintf(cost, sizeof(cost), "%.1f ms", card.nr_gpu_ms);
    summary.gpu = cost;
  }
  const auto base = resolutions[0];
  summary.resolution = base.follow_input ? ui::Tr("Follow input")
      : std::to_string(static_cast<int>(std::lround(base.scale * 100.f))) + "%";
  ui::MonitorInputs monitor{.card = card, .now_ns = now};
  monitor.summary = summary;
  if (need_monitor) {
    if (inputs.recent) {
      const auto& recent = *inputs.recent;
      monitor.recent = {.available = true, .ready = recent.ready, .partial = recent.partial,
          .eligible = recent.eligible, .injected = recent.injected, .gaps = recent.gaps, .last_sample_ns = recent.last_sample_ns};
    }
    monitor.pass_count = plan.pass_count;
    monitor.gpu_scope = ui::Tr("Completed single submissions; stage spans are not frame latency.");
    monitor.current_limit = queried.CanSubmit() ? ""
        : ui::Tr(queried.configuration.reason == capability::Reason::PLAN_REJECTED
            ? stage_plan::ReasonText(queried.plan_reason) : capability::ReasonText(queried.configuration.reason));
    if (inputs.gap_reason != nullptr) monitor.current_limit += std::string("\n") + ui::Tr(inputs.gap_reason);
    for (size_t issue = 0; issue < health::kRowCount; ++issue) {
      if (((health_mask >> issue) & 1u) != 0 && health::Counted(health::kRows[issue])) {
        monitor.current_limit += std::string("\n") + ui::Tr(health::kRows[issue].title);
        break;
      }
    }
    for (uint32_t pass = 0; pass < plan.pass_count; ++pass) {
      auto& row = monitor.passes[pass];
      const auto stage = static_cast<size_t>(plan.effective[pass]);
      row.requested_stage = stage_plan::Name(plan.requested[pass]);
      row.effective_stage = plan.valid ? stage_plan::Name(plan.effective[pass]) : ui::Tr("Blocked");
      if (!plan.valid || plan.requested[pass] != plan.effective[pass]) {
        row.stage_reason = ui::Tr(stage_plan::ReasonText(plan.reason));
      }
      row.requested_model = nr_model::PresetName(models[pass]);
      const auto resolution = resolutions[pass];
      row.requested_size = resolution.follow_input ? ui::Tr("Follow input")
          : std::to_string(static_cast<int>(std::lround(resolution.scale * 100.f))) + "%";
      row.execution_proof = row.actual_model_proof = ui::Tr("Pending confirmation");
      row.actual_size = ui::Tr("No current completed sample");
      const auto& timing = timings[stage];
      const auto& proof = proofs[stage];
      const auto* completed = timing_current[stage] ? &timing : proof_current[stage] ? &proof : nullptr;
      if (proof_current[stage] && (completed == nullptr || proof.sequence > completed->sequence)) completed = &proof;
      if (completed != nullptr) {
        for (const auto& sample : completed->passes) {
          if (sample.id != pass || sample.state != gpu_timers::SampleState::VALID
              || sample.width == 0 || sample.height == 0) continue;
          row.created_model_request = nr_model::PresetName(sample.model);
          row.execution_proof = ui::Tr("GPU complete: single submission");
          row.execution_confirmed = true;
          row.actual_size = std::to_string(sample.width) + " x " + std::to_string(sample.height);
          if (pass == 0) {
            summary.resolution += " (" + row.actual_size + ")";
          }
        }
      }
      if (timing_current[stage]) {
        for (const auto& sample : timing.passes) {
          if (sample.id != pass || sample.state != gpu_timers::SampleState::VALID) continue;
          char cost[64];
          std::snprintf(cost, sizeof(cost), "%.3f ms", sample.total_us / 1000.f);
          row.gpu_cost = cost;
          row.gpu_state = ui::MonitorGpuState::CURRENT;
        }
      }
    }
  }
  cached.verdict = verdict;
  cached.inputs = inputs;
  cached.card = card;
  cached.summary = summary;
  monitor.summary = summary;
  cached.monitor = std::move(monitor);
  cached.execution = std::move(execution);
  cached.warnings = warnings;
  cached.sampled_ns = now;
  cached.valid_until_ns = now + ui::RecentHealthWindow::kStaleNs;
  for (size_t stage = 0; stage < timings.size(); ++stage) {
    if (timing_current[stage]) cached.valid_until_ns = (std::min)(cached.valid_until_ns,
        timings[stage].recorded_ns + gpu_timers::internal::kSampleAgeNs);
    if (proof_current[stage]) cached.valid_until_ns = (std::min)(cached.valid_until_ns,
        proofs[stage].recorded_ns + gpu_timers::internal::kSampleAgeNs);
  }
  cached.complete = need_monitor;
  ++cached.refreshes;
  return cached;
}

inline ui::StatusCard MonitorDisplayCard() {
  return RefreshMonitorSnapshot(true).card;
}

inline void DrawIndependentMonitor(reshade::api::effect_runtime* runtime, bool menu) {
  if (shutting_down.load() || !monitor_requested.load()) return;
  const auto* owner = monitor_runtime.load();
  if (runtime != nullptr && owner != nullptr && runtime != owner) return;
  const auto epoch = monitor_observation_epoch.load();
  if (ui_monitor_seen_epoch != epoch) {
    ui_monitor_seen_epoch = epoch;
    ui_monitor_drawn_frame = -1;
    ui_monitor_was_interactive = false;
    monitor_snapshot.attempted_ns = 0;
  }
  if (ui_monitor_drawn_frame == ImGui::GetFrameCount()) return;
  ui_monitor_drawn_frame = ImGui::GetFrameCount();
  RefreshUiLanguage();
  ui::SetTheme(ui_theme_choice.load());
  ui_runtime_monitor_state.open = true;
  UpdateMonitorCadence(runtime);
  const auto& snapshot = RefreshMonitorSnapshot(true);
  const bool interactive = menu || hud_overlay_open.load();
  if (interactive && !ui_monitor_was_interactive) ImGui::SetNextWindowFocus();
  ui_monitor_was_interactive = interactive;
  const auto actions = ui::DrawRuntimeMonitor(snapshot.monitor, &ui_runtime_monitor_state, nullptr, interactive, true);
  if (actions.open_diagnostics) ui_open_diagnostics = true;
  if (!ui_runtime_monitor_state.open) {
    monitor_requested = false;
    monitor_observation_epoch.fetch_add(1);
  }
  ui_monitor_geometry_dirty |= actions.geometry_changed || actions.settings_changed
      || ui_monitor_saved_pin != ui_runtime_monitor_state.pin
      || ui_monitor_saved_detailed != ui_runtime_monitor_state.detailed;
  if (ui_monitor_geometry_dirty && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    ++ui_monitor_save_batches;
    const auto& geometry = ui_runtime_monitor_state.geometry;
    reshade::set_config_value(nullptr, kConfigSection, "UiMonitorX", geometry.position.x);
    reshade::set_config_value(nullptr, kConfigSection, "UiMonitorY", geometry.position.y);
    reshade::set_config_value(nullptr, kConfigSection, "UiMonitorWidth", geometry.size.x);
    reshade::set_config_value(nullptr, kConfigSection, "UiMonitorHeight", geometry.size.y);
    reshade::set_config_value(nullptr, kConfigSection, "UiMonitorScale", geometry.ui_scale);
    reshade::set_config_value(nullptr, kConfigSection, "UiMonitorPinned", ui_runtime_monitor_state.pin);
    reshade::set_config_value(nullptr, kConfigSection, "UiMonitorDetailed", ui_runtime_monitor_state.detailed);
    ui_monitor_geometry_dirty = false;
    ui_monitor_saved_pin = ui_runtime_monitor_state.pin;
    ui_monitor_saved_detailed = ui_runtime_monitor_state.detailed;
  }
}
