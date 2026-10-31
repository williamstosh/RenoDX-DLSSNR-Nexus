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

struct MonitorHistoryRuntime {
  ui::MonitorHistory values;
  bool active = false;
  bool needs_baseline = true;
  uint64_t epoch = UINT64_MAX;
  uint64_t configuration = UINT64_MAX;
  uint64_t session = UINT64_MAX;
  uint64_t consumed = 0;
  std::array<gpu_timers::Stats, 3> latest{};
  std::array<gpu_timers::Stats, 3> contracts{};
  std::array<uint64_t, 3> summed{};
};
inline thread_local MonitorHistoryRuntime monitor_history;

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
  const auto epoch = monitor_history_epoch.load();
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
  if (monitor_history.active && monitor_requested.load()) {
    const float interval_ms = ImGui::GetIO().DeltaTime * 1000.f;
    monitor_history.values.Add(ui::MonitorMetric::INTERVAL, now, interval_ms,
        runtime != nullptr && std::isfinite(interval_ms) && interval_ms > 0.f);
    monitor_history.values.Add(ui::MonitorMetric::FPS, now, fps,
        runtime != nullptr && cadence.samples >= 8 && now - cadence.first_ns >= 750'000'000
            && std::isfinite(fps) && fps > 0.f);
  }
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
  gpu_comparison::Identity current_identity;
  if (gpu_timers::comparison_settings != nullptr) gpu_timers::comparison_settings(&current_identity);
  std::array<bool, 3> timing_current{}, proof_current{};
  for (size_t stage = 0; stage < timings.size(); ++stage) {
    timing_current[stage] = CurrentGpuSample(timings[stage], now, &current_identity)
        && timings[stage].state == gpu_timers::SampleState::VALID
        && gpu_timers::StageIndex(timings[stage].path) == stage;
    proof_current[stage] = CurrentGpuSample(proofs[stage], now, &current_identity)
        && proofs[stage].state == gpu_timers::SampleState::VALID
        && gpu_timers::StageIndex(proofs[stage].path) == stage;
    // A newer completion can prove a different working contract while an
    // older timed recording is still fresh. Do not attach its cost to that size.
    if (timing_current[stage] && proof_current[stage] && proofs[stage].sequence > timings[stage].sequence) {
      for (size_t pass = 0; pass < timings[stage].passes.size(); ++pass) {
        const auto& timed = timings[stage].passes[pass];
        const auto& completed = proofs[stage].passes[pass];
        timing_current[stage] &= timed.id == completed.id && timed.model == completed.model
            && timed.width == completed.width && timed.height == completed.height
            && timed.output_width == completed.output_width && timed.output_height == completed.output_height
            && timed.state == completed.state && timed.look_measured == completed.look_measured;
      }
    }
  }
  std::array<PassResolutionSettings, kMaxNrPasses> resolutions;
  std::array<uint32_t, kMaxNrPasses> models{};
  std::array<uint32_t, kMaxNrPasses> resolved_models{};
  for (uint32_t pass = 0; pass < plan.pass_count; ++pass) {
    resolutions[pass] = CurrentPassResolution(pass);
    models[pass] = pass == 0 || pass_preset[pass - 1].load() == 0 ? preset.load() : pass_preset[pass - 1].load();
    resolved_models[pass] = nr_model::ResolvePreset(models[pass], 0, direct_runtime_requirements.dual_models);
  }
  if (monitor_history.active && monitor_requested.load()) {
    auto& history = monitor_history;
    auto& events = gpu_timers::monitor_events;
    if (history.needs_baseline || history.session != gpu_timers::device_session) {
      history.values.ClearGpu();
      history.latest = {};
      history.contracts = {};
      history.summed = {};
      history.consumed = events.published;
      history.session = gpu_timers::device_session;
      history.needs_baseline = false;
    }
    if (events.published < history.consumed || events.published - history.consumed > events.CAPACITY) {
      history.values.ClearGpu();
      history.latest = {};
      history.contracts = {};
      history.summed = {};
      history.consumed = (events.published > events.CAPACITY ? events.published - events.CAPACITY : 0);
    }
    for (; history.consumed < events.published; ++history.consumed) {
      const auto& event = events.samples[history.consumed % events.CAPACITY];
      const size_t stage = gpu_timers::StageIndex(event.path);
      const auto group = plan.At(static_cast<stage_plan::Stage>(stage));
      if (group.count == 0) continue;
      const bool current = CurrentGpuSample(event, now, &current_identity);
      const auto& previous = history.contracts[stage];
      bool same_contract = previous.sequence == 0 || (previous.identity == event.identity
          && previous.queue == event.queue && previous.width == event.width && previous.height == event.height);
      for (size_t pass = 0; pass < event.passes.size() && same_contract && previous.sequence != 0; ++pass) {
        const auto& a = previous.passes[pass];
        const auto& b = event.passes[pass];
        same_contract = a.id == b.id && a.model == b.model && a.width == b.width && a.height == b.height
            && a.output_width == b.output_width && a.output_height == b.output_height && a.look_measured == b.look_measured;
      }
      if (current && !same_contract) {
        history.values.ClearGpu();
        history.latest = {};
        history.contracts = {};
        history.summed = {};
      }
      // Rejected events invalidate the latest measurement and leave a gap;
      // their absent queue/dimensions cannot replace a known valid contract.
      if (current) history.contracts[stage] = event;
      history.latest[stage] = event;
      for (uint32_t local = 0; local < group.count; ++local) {
        const auto id = group.begin + local;
        const auto& pass = event.passes[local];
        history.values.Add(static_cast<ui::MonitorMetric>(static_cast<unsigned>(ui::MonitorMetric::PASS_1) + id),
            event.measured_ns, pass.total_us / 1000.f, current && pass.id == id
                && pass.model == resolved_models[id] && pass.state == gpu_timers::SampleState::VALID
                && pass.width != 0 && pass.height != 0 && pass.output_width != 0 && pass.output_height != 0);
      }
      std::array<bool, 3> raw_current{};
      bool all_new = true;
      for (uint32_t index = 0; index < plan.group_count; ++index) {
        const auto required = static_cast<size_t>(plan.groups[index].stage);
        raw_current[required] = CurrentGpuSample(history.latest[required], now, &current_identity);
        all_new &= history.latest[required].sequence > history.summed[required];
      }
      const auto raw_total = SummarizeMonitorGpu(plan, history.latest, raw_current, resolved_models);
      if (raw_total.valid && all_new) {
        history.values.Add(ui::MonitorMetric::GPU, event.measured_ns, raw_total.milliseconds, true, raw_total.reference);
        for (size_t index = 0; index < history.latest.size(); ++index) history.summed[index] = history.latest[index].sequence;
      } else if (!raw_total.valid) {
        history.values.Add(ui::MonitorMetric::GPU, event.measured_ns, 0.f, false);
      }
    }
    // Expiry marks a gap once. A UI cache read never appends a measurement.
    for (uint32_t pass = 0; pass < plan.pass_count; ++pass) {
      const auto stage = static_cast<size_t>(plan.effective[pass]);
      if (!CurrentGpuSample(history.latest[stage], now, &current_identity)) {
        history.values.Add(static_cast<ui::MonitorMetric>(static_cast<unsigned>(ui::MonitorMetric::PASS_1) + pass), now, 0.f, false);
        history.values.Add(ui::MonitorMetric::GPU, now, 0.f, false);
      }
    }
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
  // Independent stage windows support a reference work sum, never a shared
  // execution-cycle latency. Keep that distinction out of the legacy FPS estimate.
  card.nr_gpu_ms = card.nr_present_ms = 0.f;
  const auto gpu_total = SummarizeMonitorGpu(plan, timings, timing_current, resolved_models);
  if (gpu_total.valid && !gpu_total.reference) card.nr_gpu_ms = gpu_total.milliseconds;
  ui::StatusSummaryFields summary;
  summary.fps = monitor_cadence.text;
  summary.stage = execution;
  summary.fg = ui::Tr(card.fg_declared || card.frame_gen_multiplier > 1 ? "On"
      : native_fg_declaration.load() == NativeFgDeclaration::kOff ? "Off" : "Unknown");
  summary.gpu_reference = gpu_total.reference;
  if (gpu_total.reference) {
    summary.gpu_tip = "Reference sum of recent stage GPU times, including each stage's shared work once. Not same-frame or FG end-to-end latency.";
  } else if (gpu_total.incompatible) {
    summary.gpu_tip = "Stage measurements use different queues or conditions. See per-pass GPU times.";
  } else if (gpu_total.missing_stages != 0) {
    summary.gpu_tip = "Some stages have no current GPU measurement. See per-pass GPU times.";
  }
  if (gpu_total.valid) {
    char cost[64];
    std::snprintf(cost, sizeof(cost), "%.1f ms", gpu_total.milliseconds);
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
    monitor.gpu_scope = ui::Tr(summary.gpu_tip);
    if (gpu_total.incompatible) monitor.gpu_notice = ui::Tr(summary.gpu_tip);
    if (gpu_total.missing_stages != 0) {
      monitor.gpu_notice = std::string(ui::Tr("GPU timing unavailable")) + ": ";
      bool first = true;
      for (size_t stage = 0; stage < timings.size(); ++stage) {
        if ((gpu_total.missing_stages & (1u << stage)) == 0) continue;
        if (!first) monitor.gpu_notice += ", ";
        monitor.gpu_notice += stage_plan::Name(static_cast<stage_plan::Stage>(stage));
        first = false;
      }
      monitor.gpu_scope += std::string("\n") + monitor.gpu_notice;
    }
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
          if (sample.id != pass || sample.state != gpu_timers::SampleState::VALID
              || sample.model != resolved_models[pass] || sample.width == 0 || sample.height == 0
              || sample.output_width == 0 || sample.output_height == 0) continue;
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

inline void SaveMonitorSettings() {
  if (!ui_monitor_geometry_dirty || ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsAnyItemActive()) return;
  {
    std::shared_lock config_lock(present_restore_configuration.mutex);
    if (!present_restore_configuration.global_writable) return;
  }
  ++ui_monitor_save_batches;
  const auto& state = ui_runtime_monitor_state;
  const auto& geometry = state.geometry;
  if (geometry.valid) {
    reshade::set_config_value(nullptr, kConfigSection, "UiMonitorX", geometry.position.x);
    reshade::set_config_value(nullptr, kConfigSection, "UiMonitorY", geometry.position.y);
    reshade::set_config_value(nullptr, kConfigSection, "UiMonitorWidth", geometry.size.x);
    reshade::set_config_value(nullptr, kConfigSection, "UiMonitorHeight", geometry.size.y);
    reshade::set_config_value(nullptr, kConfigSection, "UiMonitorScale", geometry.ui_scale);
  }
  reshade::set_config_value(nullptr, kConfigSection, "UiMonitorPinned", state.pin);
  reshade::set_config_value(nullptr, kConfigSection, "UiMonitorDetailed", state.detailed);
  reshade::set_config_value(nullptr, kConfigSection, "UiMonitorOpacity", state.background_opacity);
  reshade::set_config_value(nullptr, kConfigSection, "UiMonitorTrends", state.trends);
  reshade::set_config_value(nullptr, kConfigSection, "UiMonitorHistorySeconds", state.history_seconds);
  reshade::set_config_value(nullptr, kConfigSection, "UiMonitorTrendMask", state.trend_mask);
  reshade::set_config_value(nullptr, kConfigSection, "UiMonitorAnchor", state.anchor);
  reshade::set_config_value(nullptr, kConfigSection, "UiMonitorOffsetX", state.offset.x);
  reshade::set_config_value(nullptr, kConfigSection, "UiMonitorOffsetY", state.offset.y);
  ui_monitor_geometry_dirty = false;
  ui_monitor_saved_pin = state.pin;
  ui_monitor_saved_detailed = state.detailed;
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
  auto& history = monitor_history;
  const bool record_history = ui_runtime_monitor_state.trends;
  const auto history_epoch = monitor_history_epoch.load();
  const auto configuration = configuration_generation.load();
  if (history.active != record_history || history.epoch != history_epoch || history.configuration != configuration) {
    history.values.Clear();
    history.latest = {};
    history.contracts = {};
    history.summed = {};
    history.needs_baseline = true;
    history.epoch = history_epoch;
    history.configuration = configuration;
    monitor_snapshot.attempted_ns = 0;
  }
  history.active = record_history;
  gpu_timers::monitor_events.enabled = record_history;
  UpdateMonitorCadence(runtime);
  const auto& snapshot = RefreshMonitorSnapshot(true);
  monitor_snapshot.monitor.history = (record_history ? &history.values : nullptr);
  monitor_snapshot.monitor.now_ns = SteadyNowNs();
  const bool interactive = menu || hud_overlay_open.load();
  if (interactive && !ui_monitor_was_interactive) ImGui::SetNextWindowFocus();
  ui_monitor_was_interactive = interactive;
  const auto actions = ui::DrawRuntimeMonitor(snapshot.monitor, &ui_runtime_monitor_state, nullptr, interactive, true);
  if (actions.open_diagnostics) ui_open_diagnostics = true;
  if (!ui_runtime_monitor_state.open) {
    monitor_requested = false;
    gpu_timers::monitor_events.enabled = false;
    history.active = false;
    monitor_observation_epoch.fetch_add(1);
    monitor_history_epoch.fetch_add(1);
  }
  ui_monitor_geometry_dirty |= actions.geometry_changed || actions.settings_changed
      || ui_monitor_saved_pin != ui_runtime_monitor_state.pin
      || ui_monitor_saved_detailed != ui_runtime_monitor_state.detailed;
  SaveMonitorSettings();
}
