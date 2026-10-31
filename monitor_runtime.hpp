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
  ui::frame_metrics::Sample frame;
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
  bool reset_cursor = false;
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
  ui::frame_metrics::Stream fallback;
  ui::frame_metrics::Stream present;
  ui::frame_metrics::Sample selected;
  uint64_t consumed = 0;
  bool recording = false;
};
inline thread_local MonitorCadence monitor_cadence;

// One source for the summary, monitor and both cadence curves. Present is
// accepted only for the selected runtime's device/backbuffer identity.
inline void UpdateMonitorCadence(reshade::api::effect_runtime* runtime) {
  const auto owner = monitor_runtime.load();
  if (runtime != nullptr && owner != nullptr && runtime != owner) return;
  const auto now = SteadyNowNs();
  const auto epoch = monitor_history_epoch.load();
  auto& cadence = monitor_cadence;
  if (runtime == nullptr || cadence.owner != runtime || cadence.epoch != epoch) {
    if (runtime == nullptr || cadence.owner != runtime) monitor_history.values.ClearCadence();
    cadence = {.owner = runtime, .epoch = epoch, .first_ns = now};
  }
  if (runtime == nullptr || cadence.frame == ImGui::GetFrameCount()) return;
  cadence.frame = ImGui::GetFrameCount();
  cadence.last_ns = now;
  ++cadence.samples;
  const ui::frame_metrics::Identity target{
      .device = reinterpret_cast<uint64_t>(runtime->get_device()),
      .surface = (runtime->get_back_buffer_count() != 0 ? runtime->get_back_buffer(0).handle : 0),
      .runtime = reinterpret_cast<uint64_t>(runtime), .epoch = epoch,
      .configuration = configuration_generation.load()};
  cadence.fallback.Observe(ui::frame_metrics::Source::IMGUI, target, now, ImGui::GetIO().DeltaTime * 1000.f);
  std::unique_lock pacing_lock(present_metrics.mutex);
  if (present_metrics.target != target) {
    present_metrics.target = target;
    present_metrics.stream.Reset(ui::frame_metrics::Source::PRESENT, target);
  }
  cadence.present = present_metrics.stream;
  pacing_lock.unlock();
  const auto selected = ui::frame_metrics::Select(cadence.present.Read(now), cadence.fallback.Read(now), target, now);
  const bool changed = selected.source != cadence.selected.source || selected.identity != cadence.selected.identity
      || selected.generation != cadence.selected.generation;
  const bool record = monitor_history.active && monitor_requested.load();
  const auto& stream = (selected.source == ui::frame_metrics::Source::PRESENT ? cadence.present : cadence.fallback);
  if (changed || record != cadence.recording || stream.published < cadence.consumed
      || stream.published - cadence.consumed > stream.CAPACITY) {
    if (cadence.recording || record) monitor_history.values.BeginSegment();
    cadence.consumed = stream.published;
  }
  cadence.selected = selected;
  cadence.recording = record;
  if (record) {
    for (; cadence.consumed < stream.published; ++cadence.consumed) {
      const auto& sample = stream.events[cadence.consumed % stream.CAPACITY];
      if (sample.generation != selected.generation || sample.identity != selected.identity) continue;
      monitor_history.values.Add(ui::MonitorMetric::INTERVAL, sample.time_ns, sample.raw_interval_ms, sample.valid);
      monitor_history.values.Add(ui::MonitorMetric::FPS, sample.time_ns, sample.Fps(), sample.valid);
    }
    if (!selected.valid) {
      monitor_history.values.Add(ui::MonitorMetric::INTERVAL, now, 0.f, false);
      monitor_history.values.Add(ui::MonitorMetric::FPS, now, 0.f, false);
    }
  } else {
    cadence.consumed = stream.published;
  }
  if (!selected.valid) {
    cadence.text.clear();
    return;
  }
  if (changed || now - cadence.formatted_ns >= kMonitorRefreshNs) {
    char value[96];
    std::snprintf(value, sizeof(value), "%.0f (%s)", selected.Fps(), ui::Tr(ui::frame_metrics::SourceName(selected.source)));
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

inline void NotifyCompletedModelAdjustments(int64_t now) {
  RuntimeTryLock lock(runtime_mutex);
  if (!lock.owns_lock()) return;
  const auto plan = RequestedStagePlan();
  gpu_comparison::Identity current_identity;
  if (gpu_timers::comparison_settings != nullptr) gpu_timers::comparison_settings(&current_identity);
  for (size_t stage = 0; plan.valid && stage < stage_telemetry.size(); ++stage) {
    const gpu_timers::Stats* latest = nullptr;
    for (const auto* completed : {&gpu_timers::execution_stats[stage], &gpu_timers::stage_stats[stage]}) {
      if (!CurrentGpuSample(*completed, now, &current_identity)
          || gpu_timers::StageIndex(completed->path) != stage) continue;
      if (latest == nullptr || completed->sequence > latest->sequence) latest = completed;
    }
    if (latest == nullptr) continue;
    for (const auto& pass : latest->passes) {
      if (pass.state != gpu_timers::SampleState::VALID || pass.id >= plan.pass_count
          || static_cast<size_t>(plan.effective[pass.id]) != stage
          || pass.width == 0 || pass.height == 0) continue;
      const uint32_t requested = (pass.id == 0 || pass_preset[pass.id - 1].load() == 0
          ? preset.load() : pass_preset[pass.id - 1].load());
      NotifyModelRequestAdjustment(nr_model::PresetName(requested), nr_model::PresetName(pass.model), pass.id);
    }
  }
}

inline const MonitorSnapshot& RefreshMonitorSnapshot(bool need_monitor) {
  auto& cached = monitor_snapshot;
  const auto now = SteadyNowNs();
  const auto& frame = monitor_cadence.selected;
  const bool frame_current = frame.valid && now >= frame.time_ns
      && now - frame.time_ns <= ui::frame_metrics::Stream::STALE_NS
      && frame.identity.runtime == reinterpret_cast<uint64_t>(monitor_runtime.load())
      && frame.identity.epoch == monitor_history_epoch.load()
      && frame.identity.configuration == configuration_generation.load();
  if (!frame_current) monitor_cadence.text.clear();
  cached.frame = frame;
  cached.frame.valid = frame_current;
  if (!frame_current && monitor_history.active && monitor_requested.load()) {
    monitor_history.values.Add(ui::MonitorMetric::INTERVAL, now, 0.f, false);
    monitor_history.values.Add(ui::MonitorMetric::FPS, now, 0.f, false);
  }
  cached.summary.fps = monitor_cadence.text;
  cached.monitor.summary.fps = monitor_cadence.text;
  cached.summary.fps_tip = cached.monitor.summary.fps_tip = ui::frame_metrics::SourceTip(
      frame_current ? frame.source : ui::frame_metrics::Source::NONE);
  cached.card.frame_ms = cached.monitor.card.frame_ms = (frame_current ? frame.interval_ms : 0.f);
  const auto key = MonitorSnapshotKey();
  const bool changed = key != cached.key;
  if (!changed && (!need_monitor || cached.complete) && now <= cached.valid_until_ns && now >= cached.attempted_ns
      && now - cached.attempted_ns < kMonitorRefreshNs) return cached;
  if (changed || now > cached.valid_until_ns
      || (cached.sampled_ns != 0 && now - cached.sampled_ns > ui::RecentHealthWindow::kStaleNs)) {
    InvalidateMonitorDisplay();
    cached.summary.fps = cached.monitor.summary.fps = monitor_cadence.text;
    cached.summary.fps_tip = cached.monitor.summary.fps_tip = ui::frame_metrics::SourceTip(
        frame_current ? frame.source : ui::frame_metrics::Source::NONE);
    cached.card.frame_ms = cached.monitor.card.frame_ms = (frame_current ? frame.interval_ms : 0.f);
  }
  cached.key = key;
  cached.attempted_ns = now;
  RuntimeTryLock lock(runtime_mutex);
  if (!lock.owns_lock()) {
    ++cached.deferred;
    return cached;
  }
  if (monitor_history.reset_cursor) {
    monitor_history.consumed = gpu_timers::monitor_events.published;
    monitor_history.reset_cursor = false;
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
      if (history.session != UINT64_MAX && history.session != gpu_timers::device_session) {
        history.values.BeginSegment();
      }
      history.values.ClearGpu();
      history.latest = {};
      history.contracts = {};
      history.summed = {};
      history.consumed = events.published;
      history.session = gpu_timers::device_session;
      history.needs_baseline = false;
    }
    if (events.published < history.consumed || events.published - history.consumed > events.CAPACITY) {
      history.values.BeginSegment();
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
        history.values.BeginSegment();
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
    execution += ui::StageDisplayName(static_cast<stage_plan::Stage>(stage));
  }
  if (execution.empty()) execution = ui::Tr("Execution stage pending confirmation");
  // Independent stage windows support a reference work sum, never a shared
  // execution-cycle latency. Keep that distinction out of the legacy FPS estimate.
  card.nr_gpu_ms = card.nr_present_ms = 0.f;
  card.frame_ms = (frame_current ? frame.interval_ms : 0.f);
  const auto gpu_total = SummarizeMonitorGpu(plan, timings, timing_current, resolved_models);
  if (gpu_total.valid && !gpu_total.reference) card.nr_gpu_ms = gpu_total.milliseconds;
  ui::StatusSummaryFields summary;
  summary.fps = monitor_cadence.text;
  summary.fps_tip = ui::frame_metrics::SourceTip(frame_current ? frame.source : ui::frame_metrics::Source::NONE);
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
        monitor.gpu_notice += ui::StageDisplayName(static_cast<stage_plan::Stage>(stage));
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
      row.requested_stage = ui::StageDisplayName(plan.requested[pass]);
      row.effective_stage = plan.valid ? ui::StageDisplayName(plan.effective[pass]) : ui::Tr("Blocked");
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
          row.model_adjustment_reason = ui::Tr(ModelRequestAdjustmentReason(models[pass], sample.model));
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

inline void SaveMonitorVisibility() {
  if (!monitor_visibility_dirty.load()) return;
  {
    std::shared_lock config_lock(present_restore_configuration.mutex);
    if (!present_restore_configuration.global_writable) return;
  }
  std::optional<bool> visible_preference;
  {
    std::unique_lock visibility_lock(monitor_visibility_mutex);
    if (monitor_visibility_dirty.exchange(false)) {
      visible_preference = monitor_visible_preference.load();
    }
  }
  if (visible_preference.has_value()) {
    reshade::set_config_value(nullptr, kConfigSection, "UiMonitorVisible", *visible_preference);
  }
}

inline void SaveMonitorSettings() {
  SaveMonitorVisibility();
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
  reshade::set_config_value(nullptr, kConfigSection, "UiMonitorAutoFit", state.auto_fit);
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

inline void PrepareMonitorHistory(bool record_history) {
  auto& history = monitor_history;
  const auto history_epoch = monitor_history_epoch.load();
  const auto configuration = configuration_generation.load();
  if (history.configuration != UINT64_MAX && (history.configuration != configuration
      || history.epoch != history_epoch || history.active != record_history)) {
    history.values.BeginSegment();
    history.latest = {};
    history.contracts = {};
    history.summed = {};
    // Old GPU samples remain in their segment; queued samples belong to the
    // previous contract and cannot seed a current number after the switch.
    history.reset_cursor = true;
    monitor_snapshot.attempted_ns = 0;
  }
  // Visibility, F11 and trend toggles stop sampling but do not erase the
  // previous time axis. The next valid sample naturally leaves a gap.
  history.epoch = history_epoch;
  history.configuration = configuration;
  history.active = record_history;
  gpu_timers::monitor_events.enabled = record_history;
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
  const bool record_history = ui_runtime_monitor_state.trends;
  PrepareMonitorHistory(record_history);
  UpdateMonitorCadence(runtime);
  const auto& snapshot = RefreshMonitorSnapshot(true);
  monitor_snapshot.monitor.history = (record_history ? &monitor_history.values : nullptr);
  monitor_snapshot.monitor.now_ns = SteadyNowNs();
  const auto binding = monitor_hotkey.load();
  monitor_snapshot.monitor.hotkey = (binding != 0 && !MonitorKeyConflict(binding)
      ? VirtualKeyName(binding) : "");
  const auto mode_binding = monitor_mode_key.load();
  monitor_snapshot.monitor.mode_hotkey = (mode_binding != 0 && !MonitorModeKeyConflict(mode_binding)
      ? VirtualKeyName(mode_binding) : "");
  const bool interactive = menu || hud_overlay_open.load();
  if (interactive && !ui_monitor_was_interactive) ImGui::SetNextWindowFocus();
  ui_monitor_was_interactive = interactive;
  const auto actions = ui::DrawRuntimeMonitor(snapshot.monitor, &ui_runtime_monitor_state, nullptr, interactive, true);
  if (actions.open_diagnostics) ui_open_diagnostics = true;
  if (!ui_runtime_monitor_state.open) {
    std::unique_lock visibility_lock(monitor_visibility_mutex);
    monitor_requested = false;
    monitor_visible_preference = false;
    monitor_visibility_dirty = true;
    gpu_timers::monitor_events.enabled = false;
    monitor_history.active = false;
    monitor_observation_epoch.fetch_add(1);
    monitor_history_epoch.fetch_add(1);
  }
  ui_monitor_geometry_dirty |= actions.geometry_changed || actions.settings_changed
      || ui_monitor_saved_pin != ui_runtime_monitor_state.pin
      || ui_monitor_saved_detailed != ui_runtime_monitor_state.detailed;
  SaveMonitorSettings();
}
