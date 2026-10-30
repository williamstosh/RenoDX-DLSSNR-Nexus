/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Include after dlssnr.hpp and the real ImGui internal header in ui_menu_tests.
// Call between frames, with no concurrent production workers or pending input.
// Frame accepts const ui::MonitorInputs& and renders it through the existing
// monitor_fixture branch, restoring the caller's fixture selectors afterward.
// The existing fixture must own ./ReShade.ini and ./profile-fixture-local.
template <typename Check, typename Frame>
void RunMonitorSessionCases(const Check& check, const Frame& frame) {
  namespace addon = renodx::addons::dlss5;
  namespace r = addon::internal;
  namespace ui = addon::ui;
  namespace timers = addon::gpu_timers;
  const auto* original_context = ImGui::GetCurrentContext();
  if (original_context == nullptr || original_context->WithinFrameScope
      || addon::runtime_lock::State::HeldByThisThread()) {
    check(false, "monitor session cases require an initialized idle ImGui context and no runtime lock");
    return;
  }
  const auto fixture_directory = std::filesystem::current_path();
  std::array<wchar_t, 32768> appdata{};
  const auto appdata_length = GetEnvironmentVariableW(L"LOCALAPPDATA", appdata.data(),
                                                      static_cast<DWORD>(appdata.size()));
  if (appdata_length == 0 || appdata_length >= appdata.size()
      || std::filesystem::path(appdata.data()).lexically_normal()
          != (fixture_directory / "profile-fixture-local").lexically_normal()) {
    check(false, "monitor session cases require the existing isolated menu fixture environment");
    return;
  }
  const auto ini_path = fixture_directory / "ReShade.ini";
  const auto read_ini = [&]() -> std::optional<std::string> {
    if (!std::filesystem::exists(ini_path)) return std::nullopt;
    std::ifstream input(ini_path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read the menu fixture INI.");
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  };
  const auto original_ini = read_ini();
  std::vector<std::function<void()>> restore_values;
  const auto preserve = [&](auto& value) {
    restore_values.emplace_back([&value, saved = value]() mutable { value = std::move(saved); });
  };
  const auto preserve_atomic = [&](auto& value) {
    restore_values.emplace_back([&value, saved = value.load()] { value.store(saved); });
  };
  const auto restore = [&] {
    {
      std::scoped_lock lock(r::runtime_mutex, r::overlay_listener_mutex,
                            r::monitor_key_mutex, r::card_observation.mutex);
      for (auto it = restore_values.rbegin(); it != restore_values.rend(); ++it) (*it)();
    }
    // Normally no write occurs: these callbacks and the diagnostic frame must
    // leave the fixture INI untouched. Restore even if a throwing check unwinds.
    if (read_ini() != original_ini) {
      WritePrivateProfileStringA(nullptr, nullptr, nullptr, ini_path.string().c_str());
      if (original_ini) {
        std::ofstream output(ini_path, std::ios::binary | std::ios::trunc);
        output.write(original_ini->data(), static_cast<std::streamsize>(original_ini->size()));
        output.close();
        if (!output) throw std::runtime_error("Cannot restore the menu fixture INI.");
      } else {
        std::filesystem::remove(ini_path);
      }
      WritePrivateProfileStringA(nullptr, nullptr, nullptr, ini_path.string().c_str());
    }
  };
  struct Restore {
    const decltype(restore)* callback;
    ~Restore() noexcept(false) { (*callback)(); }
  } restore_scope{&restore};

  preserve_atomic(r::monitor_hotkey);
  preserve_atomic(r::host_overlay_hotkey);
  preserve_atomic(r::toggle_hotkey);
  preserve_atomic(r::screenshot_hotkey);
  preserve_atomic(r::official_cycle_key);
  preserve_atomic(r::custom_cycle_key);
  preserve_atomic(r::official_cycle_enabled);
  preserve_atomic(r::custom_cycle_enabled);
  preserve_atomic(r::hotkey_capture_target);
  preserve_atomic(r::hud_overlay_open);
  preserve_atomic(r::monitor_requested);
  preserve_atomic(r::monitor_keep_visible);
  preserve_atomic(r::monitor_observation_epoch);
  preserve_atomic(r::monitor_runtime);
  preserve_atomic(r::enabled);
  preserve_atomic(r::hook_point);
  preserve_atomic(r::stack_passes);
  preserve_atomic(r::stage_source_rr);
  preserve_atomic(r::configuration_generation);
  preserve_atomic(r::gpu_timers_enabled);
  preserve_atomic(r::cost_meter_enabled);
  preserve_atomic(r::nr_cost_gpu_ms);
  preserve_atomic(r::nr_cost_present_ms);
  preserve_atomic(r::nr_cost_frame_ms);
  preserve_atomic(r::ngx_ever_hooked);
  preserve_atomic(r::cmd_hooks_ever_installed);
  preserve_atomic(r::ui_welcomed);
  preserve(r::monitor_key_edge);
  preserve(r::live_effect_runtimes);
  preserve(r::monitor_snapshot);
  preserve(r::monitor_cadence);
  preserve(ImGui::GetIO().Framerate);
  preserve(r::ui_monitor_seen_epoch);
  preserve(r::ui_monitor_drawn_frame);
  preserve(r::ui_monitor_was_interactive);
  preserve(r::ui_monitor_save_batches);
  preserve(r::ui_fg_route);
  preserve(r::flush_profile_storage);
  preserve(r::card_observation.window);
  preserve(r::card_observation.configuration);
  preserve(r::card_observation.declines);
  preserve(r::card_observation.declines_sampled_ns);
  preserve(r::card_observation.last_decline_ns);
  preserve(r::card_observation.last_decline);
  preserve(timers::comparison_settings);
  for (auto& stats : timers::stage_stats) preserve(stats);
  for (auto& proof : timers::execution_stats) preserve(proof);
  preserve(ImGui::GetStyle());
  const auto original_theme = ui::CurrentThemeId();
  const int original_language = ui::CurrentLanguage();
  restore_values.emplace_back([=] {
    ui::SetTheme(original_theme);
    ui::SelectLanguage(original_language);
  });
  // Home-close must never flush profile storage or touch user profile files.
  r::flush_profile_storage = nullptr;
  ui::SelectLanguage(0);
  r::ui_welcomed = true;

  const auto reset_key = [&](std::uint32_t binding = VK_F11) {
    r::monitor_hotkey = binding;
    r::host_overlay_hotkey = VK_INSERT;
    r::toggle_hotkey = VK_F6;
    r::screenshot_hotkey = VK_F5;
    r::official_cycle_key = VK_F9;
    r::custom_cycle_key = VK_F10;
    r::monitor_requested = false;
    r::hud_overlay_open = false;
    std::scoped_lock lock(r::monitor_key_mutex);
    r::monitor_key_edge = {};
  };
  reset_key();
  const auto toggle_epoch = r::monitor_observation_epoch.load();
  r::ToggleMonitor();
  check(r::monitor_requested.load() && r::monitor_observation_epoch.load() == toggle_epoch + 1,
        "ToggleMonitor opens once and invalidates the observation epoch");
  r::ToggleMonitor();
  check(!r::monitor_requested.load() && r::monitor_observation_epoch.load() == toggle_epoch + 2,
        "ToggleMonitor closes once and invalidates the observation epoch");
  r::PollMonitorKey(true, true, false);
  check(!r::monitor_requested.load(), "initially held default F11 is consumed");
  r::PollMonitorKey(false, true, false);
  r::PollMonitorKey(true, true, false);
  check(r::monitor_requested.load(), "a fresh default F11 press opens the monitor");
  const auto pressed_epoch = r::monitor_observation_epoch.load();
  for (int tick = 0; tick < 5; ++tick) r::PollMonitorKey(true, true, false);
  check(r::monitor_requested.load() && r::monitor_observation_epoch.load() == pressed_epoch,
        "holding F11 produces exactly one toggle and one epoch change");
  r::monitor_hotkey = 'M';
  r::PollMonitorKey(true, true, false);
  check(r::monitor_requested.load(), "rebinding to a held custom key does not toggle");
  r::PollMonitorKey(false, true, false);
  r::PollMonitorKey(true, true, false);
  check(!r::monitor_requested.load(), "a released and pressed custom monitor key works");
  for (std::uint32_t binding : {VK_F6, VK_F5, VK_F9, VK_F10, VK_HOME, VK_INSERT}) {
    for (bool cycles_enabled : {false, true}) {
      reset_key(binding);
      r::official_cycle_enabled = cycles_enabled;
      r::custom_cycle_enabled = cycles_enabled;
      const auto epoch = r::monitor_observation_epoch.load();
      r::PollMonitorKey(false, true, false);
      r::PollMonitorKey(true, true, false);
      check(!r::monitor_requested.load() && r::monitor_observation_epoch.load() == epoch,
            "duplicate action bindings, Home and the custom host-overlay key never toggle the monitor");
    }
  }
  reset_key();
  r::PollMonitorKey(false, true, false);
  r::PollMonitorKey(true, true, false);
  const auto focus_epoch = r::monitor_observation_epoch.load();
  r::PollMonitorKey(true, false, false);
  r::PollMonitorKey(true, true, false);
  check(r::monitor_requested.load() && r::monitor_observation_epoch.load() == focus_epoch,
        "a key held across focus loss and regain never repeats");
  r::PollMonitorKey(false, false, false);
  r::PollMonitorKey(true, true, false);
  check(r::monitor_requested.load() && r::monitor_observation_epoch.load() == focus_epoch,
        "a fake release while unfocused cannot rearm a physically held monitor key");
  r::PollMonitorKey(false, true, false);
  r::PollMonitorKey(true, true, false);
  check(!r::monitor_requested.load(), "a later focused release rearms the next real press");

  reset_key();
  r::PollMonitorKey(false, true, false);
  r::PollMonitorKey(true, true, false);
  r::OnReshadeOpenOverlay(nullptr, true, reshade::api::input_source::keyboard);
  const auto home_epoch = r::monitor_observation_epoch.load();
  r::PollMonitorKey(false, true, true);
  r::PollMonitorKey(true, true, true);
  check(r::hud_overlay_open.load() && r::monitor_requested.load()
            && r::monitor_observation_epoch.load() == home_epoch,
        "Home reader switch consumes a fake release/repress without a second toggle");
  r::PollMonitorKey(false, true, true);
  r::PollMonitorKey(true, true, true);
  check(!r::monitor_requested.load(), "a real overlay-reader press toggles after release");
  r::monitor_keep_visible = true;
  r::OnReshadeOpenOverlay(nullptr, false, reshade::api::input_source::keyboard);
  const auto close_epoch = r::monitor_observation_epoch.load();
  r::PollMonitorKey(false, true, false);
  r::PollMonitorKey(true, true, false);
  check(!r::monitor_requested.load() && r::monitor_observation_epoch.load() == close_epoch,
        "returning to the gameplay reader cannot double-trigger the held key");
  for (bool keep : {true, false}) {
    r::monitor_keep_visible = keep;
    r::monitor_requested = true;
    r::OnReshadeOpenOverlay(nullptr, true, reshade::api::input_source::keyboard);
    r::hotkey_capture_target = 5;
    const auto epoch = r::monitor_observation_epoch.load();
    check(!r::OnReshadeOpenOverlay(nullptr, false, reshade::api::input_source::keyboard)
              && !r::hud_overlay_open.load() && r::monitor_requested.load() == keep
              && r::monitor_observation_epoch.load() == epoch + 1 && r::flush_profile_storage == nullptr
              && r::hotkey_capture_target.load() == 0,
          "closing Home honors keep-visible, cancels monitor rebinding and never flushes profiles");
  }

  // The holder has a bounded escape so a regression to blocking acquisition
  // fails an assertion instead of hanging the entire production UI suite.
  const auto refresh_while_locked = [&](bool force_refresh = true) {
    std::promise<void> acquired, release;
    auto acquired_future = acquired.get_future();
    auto release_future = release.get_future();
    std::atomic_bool holder_released{false};
    const auto refreshes = r::monitor_snapshot.refreshes;
    const auto deferred = r::monitor_snapshot.deferred;
    if (force_refresh) r::monitor_snapshot.attempted_ns = 0;
    std::jthread holder([&] {
      std::unique_lock lock(r::runtime_mutex);
      acquired.set_value();
      release_future.wait_for(std::chrono::seconds(2));
      holder_released = true;
    });
    acquired_future.wait();
    const auto snapshot = r::RefreshMonitorSnapshot(true);
    const bool returned_while_locked = !holder_released.load();
    release.set_value();
    holder.join();
    check(returned_while_locked && snapshot.deferred == deferred + 1 && snapshot.refreshes == refreshes,
          "RefreshMonitorSnapshot defers under another thread's lock without blocking or sampling");
    return snapshot;
  };
  const auto seed_old_display = [&] {
    auto& cached = r::monitor_snapshot;
    cached.key = r::MonitorSnapshotKey();
    cached.sampled_ns = r::SteadyNowNs();
    cached.valid_until_ns = INT64_MAX;
    cached.complete = true;
    cached.card.nr_gpu_ms = 99.f;
    cached.monitor.pass_count = 1;
    cached.monitor.passes[0].gpu_state = ui::MonitorGpuState::CURRENT;
    cached.monitor.passes[0].gpu_cost = "OLD_GPU_CACHE";
  };
  const auto no_old_gpu = [](const r::MonitorSnapshot& snapshot) {
    bool current = false;
    for (const auto& pass : snapshot.monitor.passes) {
      current |= pass.gpu_state == ui::MonitorGpuState::CURRENT || !pass.gpu_cost.empty();
    }
    return !current && snapshot.card.nr_gpu_ms == 0.f && !snapshot.complete;
  };

  // Opaque census identities only: no virtual methods, desktop, device or GPU.
  std::array<std::byte, 2> runtime_tokens{};
  auto* runtime_a = reinterpret_cast<reshade::api::effect_runtime*>(&runtime_tokens[0]);
  auto* runtime_b = reinterpret_cast<reshade::api::effect_runtime*>(&runtime_tokens[1]);
  r::live_effect_runtimes.clear();
  r::monitor_runtime = nullptr;
  const auto initial_epoch = r::monitor_observation_epoch.load();
  r::OnInitEffectRuntime(runtime_a);
  check(r::monitor_runtime.load() == runtime_a && r::live_effect_runtimes.size() == 1
            && r::monitor_observation_epoch.load() == initial_epoch + 1,
        "first runtime becomes monitor owner and advances the epoch");
  r::OnInitEffectRuntime(runtime_b);
  check(r::monitor_runtime.load() == runtime_a && r::live_effect_runtimes.size() == 2
            && r::monitor_observation_epoch.load() == initial_epoch + 2,
        "a second runtime preserves the current owner and invalidates observations");
  const auto second_epoch = r::monitor_observation_epoch.load();
  r::OnDestroyEffectRuntime(runtime_b);
  check(r::monitor_runtime.load() == runtime_a && r::live_effect_runtimes.size() == 1
            && r::monitor_observation_epoch.load() == second_epoch,
        "destroying a non-owner preserves owner and observation epoch");
  r::OnInitEffectRuntime(runtime_b);
  seed_old_display();
  const auto owner_epoch = r::monitor_observation_epoch.load();
  r::OnDestroyEffectRuntime(runtime_a);
  check(r::monitor_runtime.load() == runtime_b && r::monitor_observation_epoch.load() == owner_epoch + 1
            && r::monitor_key_edge.binding == 0,
        "destroying the owner switches to the survivor and resets its key latch");
  check(no_old_gpu(refresh_while_locked()), "owner handoff cannot display the previous runtime's cached GPU result");
  seed_old_display();
  const auto last_epoch = r::monitor_observation_epoch.load();
  r::OnDestroyEffectRuntime(runtime_b);
  check(r::monitor_runtime.load() == nullptr && r::live_effect_runtimes.empty()
            && r::monitor_observation_epoch.load() == last_epoch + 1,
        "destroying the last runtime removes the owner and advances the epoch");
  r::OnInitEffectRuntime(runtime_a);
  check(r::monitor_runtime.load() == runtime_a && r::monitor_observation_epoch.load() == last_epoch + 2,
        "recreating a runtime establishes a fresh owner epoch");
  check(no_old_gpu(refresh_while_locked()), "runtime recreation cannot reuse old cached GPU evidence");

  // Test host cadence interpretation using explicit synthetic IO data.
  r::monitor_cadence = {};
  ImGui::GetIO().Framerate = 144.f;
  r::UpdateMonitorCadence(runtime_a);
  check(r::monitor_cadence.text.empty(), "new runtime never presents startup ImGui FPS as observed cadence");
  r::monitor_cadence.first_ns = r::SteadyNowNs() - 1'000'000'000;
  r::monitor_cadence.last_ns = r::SteadyNowNs();
  r::monitor_cadence.samples = 8;
  r::monitor_cadence.frame = -1;
  r::UpdateMonitorCadence(runtime_a);
  check(r::monitor_cadence.text == "144", "warmed cadence uses the exact ReShade IO value without FG multiplication");
  const auto cadence_samples = r::monitor_cadence.samples;
  r::UpdateMonitorCadence(runtime_a);
  check(r::monitor_cadence.samples == cadence_samples, "panel and HUD count one cadence sample per ImGui frame");
  r::monitor_cadence.last_ns = r::SteadyNowNs() - 600'000'000;
  r::UpdateMonitorCadence(runtime_a);
  check(r::monitor_cadence.text.empty(), "cadence gap invalidates old FPS before warming again");
  r::UpdateMonitorCadence(nullptr);
  check(r::monitor_cadence.text.empty(), "offscreen null-runtime fixtures cannot invent a real FPS source");

  // Synthetic published metadata, never a model invocation or measured GPU cost.
  static thread_local addon::gpu_comparison::Identity identity;
  static thread_local std::uint64_t identity_reads = 0;
  preserve(identity);
  preserve(identity_reads);
  identity = {.configuration_generation = r::configuration_generation.load(),
              .settings_hash = 17, .resolution_hash = 23};
  identity.device_session = timers::device_session;
  identity_reads = 0;
  timers::comparison_settings = +[](addon::gpu_comparison::Identity* output) {
    ++identity_reads;
    *output = identity;
  };
  r::enabled = true;
  r::hook_point = r::kHookUpscaled;
  r::stack_passes = 1;
  r::stage_source_rr = false;
  r::gpu_timers_enabled = true;
  r::cost_meter_enabled = true;
  r::nr_cost_gpu_ms = 1.25f;
  r::nr_cost_present_ms = 0.f;
  r::nr_cost_frame_ms = 16.f;
  for (auto& stats : timers::stage_stats) stats = {};
  for (auto& proof : timers::execution_stats) proof = {};
  const auto plan = r::RequestedStagePlan();
  check(plan.valid && plan.pass_count == 1, "fixture has a valid single-pass production stage plan");
  const auto stage = static_cast<std::size_t>(plan.effective[0]);
  auto& measured = timers::stage_stats[stage];
  measured.valid = true;
  measured.state = timers::SampleState::VALID;
  measured.measured_ns = r::SteadyNowNs();
  measured.recorded_ns = measured.measured_ns;
  measured.identity = identity;
  measured.total_us = 1250;
  measured.sequence = 1;
  measured.passes[0] = {.id = 0, .width = 2560, .height = 1440, .total_us = 1250,
                        .state = timers::SampleState::VALID};
  r::monitor_snapshot = {};
  const auto current = r::RefreshMonitorSnapshot(true);
  check(current.complete && current.monitor.pass_count == 1
            && current.monitor.passes[0].gpu_state == ui::MonitorGpuState::CURRENT
            && current.monitor.passes[0].gpu_cost == "1.250 ms" && identity_reads != 0,
        "fresh production snapshot displays the exact current synthetic timestamp sample");
  r::nr_cost_gpu_ms = 99.f;
  r::nr_cost_present_ms = 88.f;
  r::monitor_snapshot.attempted_ns = 0;
  const auto exact_cost = r::RefreshMonitorSnapshot(true);
  check(exact_cost.card.nr_gpu_ms == 1.25f && exact_cost.card.nr_present_ms == 0.f
            && exact_cost.summary.gpu == "1.2 ms",
        "single-stage monitor cost comes from its current completed group, never retained global cost");
  const auto saved_measured = measured;
  for (const auto state : {timers::SampleState::PENDING, timers::SampleState::REPLAYED,
                           timers::SampleState::EXPIRED}) {
    measured.state = state;
    r::monitor_snapshot.attempted_ns = 0;
    const auto rejected = r::RefreshMonitorSnapshot(true);
    check(rejected.summary.gpu.empty() && !rejected.monitor.passes[0].execution_confirmed,
          "valid flag alone cannot promote pending, replayed or expired samples");
  }
  measured = saved_measured;
  auto& newer_proof = timers::execution_stats[stage];
  newer_proof = measured;
  newer_proof.sequence = measured.sequence + 1;
  newer_proof.passes[0].width = 1715;
  newer_proof.passes[0].height = 965;
  r::monitor_snapshot.attempted_ns = 0;
  const auto newer_size = r::RefreshMonitorSnapshot(true);
  check(newer_size.monitor.passes[0].actual_size == "1715 x 965"
            && newer_size.monitor.passes[0].gpu_cost.empty() && newer_size.summary.gpu.empty(),
        "a newer completed working size cannot inherit an older timed contract's GPU cost");
  newer_proof = {};
  r::stack_passes = 2;
  r::hook_point = r::kHookRender;
  preserve_atomic(r::pass_stage[0]);
  r::pass_stage[0] = 2;
  auto& render = timers::stage_stats[0];
  render = measured;
  measured.passes[0].id = 1;
  r::monitor_snapshot.attempted_ns = 0;
  const auto cross_stage = r::RefreshMonitorSnapshot(true);
  check(cross_stage.monitor.passes[0].execution_confirmed && cross_stage.monitor.passes[1].execution_confirmed
            && cross_stage.summary.gpu.empty() && cross_stage.card.nr_gpu_ms == 0.f
            && cross_stage.card.nr_present_ms == 0.f,
        "same-configuration same-time Stage samples cannot establish a common execution cycle or total");
  render = {};
  measured = saved_measured;
  r::stack_passes = 1;
  r::hook_point = r::kHookUpscaled;
  r::monitor_snapshot.attempted_ns = 0;
  r::RefreshMonitorSnapshot(true);
  const auto reads_after_refresh = identity_reads;
  const auto refreshes_after_refresh = r::monitor_snapshot.refreshes;
  r::monitor_snapshot.attempted_ns = r::SteadyNowNs();
  const auto& hit = r::RefreshMonitorSnapshot(true);
  check(hit.refreshes == refreshes_after_refresh && identity_reads == reads_after_refresh,
        "a cache hit does not reread GPU identities or repeat snapshot collection");
  seed_old_display();
  r::monitor_snapshot.valid_until_ns = r::SteadyNowNs() - 1;
  r::monitor_snapshot.attempted_ns = r::SteadyNowNs();
  check(no_old_gpu(refresh_while_locked(false)),
        "GPU evidence expires immediately inside the 100ms cache interval even while the runtime lock is held");
  seed_old_display();
  r::monitor_snapshot.sampled_ns = r::SteadyNowNs() - ui::RecentHealthWindow::kStaleNs - 1;
  check(no_old_gpu(refresh_while_locked()), "an expired snapshot clears current GPU costs even when refresh is deferred");
  seed_old_display();
  r::configuration_generation.fetch_add(1);
  identity.configuration_generation = r::configuration_generation.load();
  check(no_old_gpu(refresh_while_locked()), "a configuration change clears current GPU costs before lock acquisition");
  r::monitor_snapshot.attempted_ns = 0;
  const auto changed = r::RefreshMonitorSnapshot(true);
  check(changed.complete && changed.monitor.passes[0].gpu_state != ui::MonitorGpuState::CURRENT
            && changed.monitor.passes[0].gpu_cost.empty(),
        "old recorded configuration identity is not relabeled as a current measurement");
  measured.measured_ns = r::SteadyNowNs() - timers::internal::kSampleAgeNs - 1;
  identity.configuration_generation = r::configuration_generation.load();
  measured.identity = identity;
  r::monitor_snapshot.attempted_ns = 0;
  const auto stale = r::RefreshMonitorSnapshot(true);
  check(stale.monitor.passes[0].gpu_state != ui::MonitorGpuState::CURRENT
            && stale.monitor.passes[0].gpu_cost.empty(),
        "expired GPU metadata never displays as a current timestamp cost");

  r::gpu_timers_enabled = false;
  r::cost_meter_enabled = false;
  measured = {};
  auto& proof = timers::execution_stats[stage];
  proof.valid = true;
  proof.state = timers::SampleState::VALID;
  proof.measured_ns = r::SteadyNowNs();
  proof.recorded_ns = proof.measured_ns;
  proof.identity = identity;
  proof.sequence = 2;
  proof.passes[0] = {.id = 0, .width = 2560, .height = 1440, .state = timers::SampleState::VALID};
  r::monitor_snapshot.attempted_ns = 0;
  const auto proof_only = r::RefreshMonitorSnapshot(true);
  check(proof_only.monitor.passes[0].execution_proof == ui::Tr("GPU complete: single submission")
            && proof_only.monitor.passes[0].actual_size == "2560 x 1440"
            && proof_only.execution == addon::stage_plan::Name(plan.effective[0]),
        "disabled optional timing still exposes completed single-submission execution proof");
  check(proof_only.card.nr_gpu_ms == 0.f && proof_only.card.nr_present_ms == 0.f
            && proof_only.monitor.passes[0].gpu_cost.empty()
            && proof_only.monitor.passes[0].gpu_state != ui::MonitorGpuState::CURRENT,
        "completion proof cannot inherit old costs or invent a zero-millisecond GPU measurement");
  frame(proof_only.monitor);
  check(ImGui::GetCurrentContext() == original_context && !original_context->WithinFrameScope,
        "production monitor frame reuses the caller's ImGui context");
  proof.measured_ns = r::SteadyNowNs() - timers::internal::kSampleAgeNs - 1;
  r::monitor_snapshot.attempted_ns = 0;
  const auto expired_proof = r::RefreshMonitorSnapshot(true);
  check(expired_proof.monitor.passes[0].execution_proof != ui::Tr("GPU complete: single submission")
            && expired_proof.monitor.passes[0].gpu_cost.empty(),
        "expired proof loses its execution confirmation without inventing a cost");
  frame(expired_proof.monitor);
  check(read_ini() == original_ini, "session callbacks and monitor frames leave the isolated fixture INI unchanged");
  check(r::flush_profile_storage == nullptr, "profile storage remains disconnected for the entire session test");
}
