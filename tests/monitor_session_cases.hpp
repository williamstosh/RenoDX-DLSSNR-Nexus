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
#include <cstring>
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
  preserve_atomic(r::monitor_mode_key);
  preserve_atomic(r::monitor_mode_cycle_request);
  preserve_atomic(r::monitor_settings_epoch);
  preserve_atomic(r::hotkey_capture_target);
  preserve_atomic(r::hud_overlay_open);
  preserve_atomic(r::monitor_requested);
  preserve_atomic(r::monitor_visible_preference);
  preserve_atomic(r::monitor_visibility_dirty);
  preserve_atomic(r::monitor_visibility_initialized);
  preserve_atomic(r::monitor_keep_visible);
  preserve_atomic(r::monitor_observation_epoch);
  preserve_atomic(r::monitor_history_epoch);
  preserve_atomic(r::monitor_runtime);
  preserve_atomic(r::enabled);
  preserve_atomic(r::hook_point);
  preserve_atomic(r::stack_passes);
  preserve_atomic(r::stage_source_rr);
  preserve_atomic(r::configuration_generation);
  preserve_atomic(r::gpu_timers_enabled);
  preserve_atomic(r::cost_meter_enabled);
  preserve_atomic(r::preset);
  preserve_atomic(r::nr_resolution_scale);
  preserve_atomic(r::hud_enabled);
  preserve_atomic(r::profile_feedback_until_ns);
  preserve(r::profile_feedback.message);
  preserve(r::profile_feedback.operation);
  preserve_atomic(r::nr_cost_gpu_ms);
  preserve_atomic(r::nr_cost_present_ms);
  preserve_atomic(r::nr_cost_frame_ms);
  preserve_atomic(r::ngx_ever_hooked);
  preserve_atomic(r::cmd_hooks_ever_installed);
  preserve_atomic(r::ui_welcomed);
  preserve(r::monitor_key_edge);
  preserve(r::monitor_mode_key_edge);
  preserve(r::live_effect_runtimes);
  preserve(r::monitor_snapshot);
  preserve(r::monitor_cadence);
  preserve(ImGui::GetIO().Framerate);
  preserve(r::ui_monitor_seen_epoch);
  preserve(r::ui_monitor_drawn_frame);
  preserve(r::ui_monitor_was_interactive);
  preserve(r::ui_monitor_save_batches);
  preserve(r::ui_runtime_monitor_state);
  preserve(r::ui_monitor_geometry_dirty);
  preserve(r::ui_monitor_saved_pin);
  preserve(r::ui_fg_route);
  for (auto& signature : r::model_adjustment_notice_signatures) preserve_atomic(signature);
  preserve(r::profile_ui::writable);
  preserve(r::flush_profile_storage);
  preserve(r::card_observation.window);
  preserve(r::card_observation.configuration);
  preserve(r::card_observation.declines);
  preserve(r::card_observation.declines_sampled_ns);
  preserve(r::card_observation.last_decline_ns);
  preserve(r::card_observation.last_decline);
  preserve(timers::comparison_settings);
  preserve(r::stage_telemetry);
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
  // This case exercises the monitor session in isolation. Profile loading and
  // first-run/template migration belong to the profile tests and may persist
  // ProfileGameId while a monitor frame is being rendered.
  r::profile_ui::writable = false;
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
    r::monitor_visible_preference = false;
    r::monitor_visibility_dirty = false;
    r::hud_overlay_open = false;
    std::scoped_lock lock(r::monitor_key_mutex);
    r::monitor_key_edge = {};
  };
  reset_key();
  const auto toggle_epoch = r::monitor_observation_epoch.load();
  r::ToggleMonitor();
  check(r::monitor_requested.load() && r::monitor_observation_epoch.load() == toggle_epoch + 1,
        "ToggleMonitor opens once and invalidates the observation epoch");
  check(r::monitor_visible_preference.load() && r::monitor_visibility_dirty.load(),
        "an explicit opening queues the visible preference without an exit-time write");
  r::ToggleMonitor();
  check(!r::monitor_requested.load() && r::monitor_observation_epoch.load() == toggle_epoch + 2,
        "ToggleMonitor closes once and invalidates the observation epoch");
  check(!r::monitor_visible_preference.load() && r::monitor_visibility_dirty.load(),
        "an explicit closing queues the hidden preference");
  {
    const auto toggle_many = [] {
      for (int index = 0; index < 50; ++index) r::ToggleMonitor();
    };
    std::thread first(toggle_many), second(toggle_many);
    first.join();
    second.join();
    check(!r::monitor_requested.load() && !r::monitor_visible_preference.load(),
          "concurrent user toggles cannot persist an out-of-order display intent");
  }
  reset_key();
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
    const auto history_epoch = r::monitor_history_epoch.load();
    check(!r::OnReshadeOpenOverlay(nullptr, false, reshade::api::input_source::keyboard)
              && !r::hud_overlay_open.load() && r::monitor_requested.load() == keep
              && r::monitor_observation_epoch.load() == epoch + 1 && r::flush_profile_storage == nullptr
              && r::hotkey_capture_target.load() == 0,
          "closing Home honors keep-visible, cancels monitor rebinding and never flushes profiles");
    check(r::monitor_history_epoch.load() == history_epoch + (keep ? 0u : 1u),
          "Home editing preserves HUD history unless closing the menu actually closes the monitor");
  }
  {
    r::monitor_mode_key = VK_F8;
    r::monitor_mode_key_edge = {};
    r::monitor_mode_cycle_request = 0;
    r::PollMonitorModeKey(false, true, false);
    r::PollMonitorModeKey(true, true, false);
    check(r::monitor_mode_cycle_request.exchange(0) == 1, "fresh F8 queues one mode cycle");
    r::PollMonitorModeKey(true, true, false);
    check(r::monitor_mode_cycle_request.load() == 0, "held F8 never repeats a mode cycle");
    r::PollMonitorModeKey(false, true, true);
    r::PollMonitorModeKey(true, true, true);
    check(r::monitor_mode_cycle_request.load() == 0, "Home input context transition cannot reuse held F8");
    r::PollMonitorModeKey(false, true, true);
    r::PollMonitorModeKey(true, true, true);
    check(r::monitor_mode_cycle_request.exchange(0) == 1, "fresh F8 in the Home context queues one mode cycle");
    r::PollMonitorModeKey(false, false, true);
    r::PollMonitorModeKey(true, true, true);
    check(r::monitor_mode_cycle_request.load() == 0, "focus or text input exclusion requires release before rearming F8");
    for (uint32_t binding : {0u, uint32_t{VK_F11}, uint32_t{VK_F6}, uint32_t{VK_HOME}}) {
      r::monitor_mode_key = binding;
      r::monitor_mode_key_edge = {};
      for (int tick = 0; tick < 3; ++tick) {
        r::PollMonitorModeKey(false, true, false);
        r::PollMonitorModeKey(true, true, false);
      }
      check(r::monitor_mode_cycle_request.load() == 0, "unbound or conflicting F8 action cannot cycle modes");
    }
    r::monitor_mode_key = VK_F8;
    const bool visible = r::monitor_requested.load();
    for (unsigned mode = 0; mode < 4; ++mode) {
      const auto before = ui::MonitorModeIndex(r::ui_runtime_monitor_state);
      r::CycleMonitorMode();
      check(ui::MonitorModeIndex(r::ui_runtime_monitor_state) == (before + 1u) % 4u
                && r::monitor_requested.load() == visible, "production mode cycle preserves F11 visibility");
      bool detailed = !r::ui_runtime_monitor_state.detailed;
      bool trends = !r::ui_runtime_monitor_state.trends;
      bool auto_fit = !r::ui_runtime_monitor_state.auto_fit;
      reshade::get_config_value(nullptr, r::kConfigSection, "UiMonitorDetailed", detailed);
      reshade::get_config_value(nullptr, r::kConfigSection, "UiMonitorTrends", trends);
      reshade::get_config_value(nullptr, r::kConfigSection, "UiMonitorAutoFit", auto_fit);
      check(detailed == r::ui_runtime_monitor_state.detailed && trends == r::ui_runtime_monitor_state.trends
                && auto_fit == r::ui_runtime_monitor_state.auto_fit, "each F8 mode persists existing detailed, trends and fit preferences");
    }
  }
  // The mode action intentionally persists. Keep this isolated input test
  // separate from the read-only monitor sampling assertions below.
  if (read_ini() != original_ini) {
    WritePrivateProfileStringA(nullptr, nullptr, nullptr, ini_path.string().c_str());
    if (original_ini) {
      std::ofstream output(ini_path, std::ios::binary | std::ios::trunc);
      output << *original_ini;
    } else {
      std::filesystem::remove(ini_path);
    }
    WritePrivateProfileStringA(nullptr, nullptr, nullptr, ini_path.string().c_str());
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

  // Opaque census tokens have no runtime vtable. Source selection and warming
  // are exercised with deterministic timestamps in RunFrameMetricsCases.
  r::monitor_cadence = {};
  ImGui::GetIO().Framerate = 144.f;
  r::UpdateMonitorCadence(nullptr);
  check(r::monitor_cadence.text.empty(), "offscreen null-runtime fixtures cannot invent a real FPS source");
  const auto visible_intent = r::monitor_visible_preference.load();
  const auto runtime_count = r::live_effect_runtimes.size();
  r::OnInitEffectRuntime(runtime_a);
  check(r::live_effect_runtimes.size() == runtime_count && r::monitor_visible_preference.load() == visible_intent,
        "repeated runtime initialization keeps a single owner and cannot overwrite player intent");

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
  measured.passes[0] = {.id = 0, .model = addon::nr_model::ResolvePreset(r::preset.load(), 0, r::direct_runtime_requirements.dual_models),
                        .width = 2560, .height = 1440, .output_width = 2560, .output_height = 1440, .total_us = 1250,
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
  {
    const auto requested = r::preset.load();
    const auto generation = r::configuration_generation.load();
    const auto dual_models = r::direct_runtime_requirements.dual_models;
    r::direct_runtime_requirements.dual_models = false;
    r::preset = 2;
    measured.passes[0].model = 1;
    r::monitor_requested = false;
    for (auto& signature : r::model_adjustment_notice_signatures) signature = 0;
    r::profile_feedback_until_ns = 0;
    r::NotifyCompletedModelAdjustments(r::SteadyNowNs());
    const auto deadline = r::profile_feedback_until_ns.load();
    check(deadline > r::SteadyNowNs() && deadline <= r::SteadyNowNs() + r::kHudToastNs
              && r::profile_feedback.message.find(std::string(addon::nr_model::PresetName(2)) + " -> "
                  + addon::nr_model::PresetName(1)) != std::string::npos,
          "completed adjustment produces a bounded notification with the monitor hidden");
    r::NotifyCompletedModelAdjustments(r::SteadyNowNs());
    r::monitor_snapshot.attempted_ns = 0;
    check(r::RefreshMonitorSnapshot(true).monitor.passes[0].model_adjustment_reason
              == "Runtime compatibility fallback.", "full adjustment reason remains in current diagnostic evidence");
    check(r::profile_feedback_until_ns.load() == deadline, "repeated evidence and monitor views cannot extend or retrigger the notification");
    ++r::configuration_generation;
    identity.configuration_generation = r::configuration_generation.load();
    r::NotifyCompletedModelAdjustments(r::SteadyNowNs());
    check(r::profile_feedback_until_ns.load() == deadline, "old completed evidence cannot notify for a new configuration");
    measured.identity = identity;
    r::profile_feedback_until_ns = 0;
    r::NotifyCompletedModelAdjustments(r::SteadyNowNs());
    check(r::profile_feedback_until_ns.load() > 0, "new current configuration evidence can notify again");
    const bool notices_enabled = r::hud_enabled.load();
    r::hud_enabled = false;
    const auto windows = ImGui::GetCurrentContext()->Windows.Size;
    r::OnHudOverlay(nullptr);
    check(ImGui::GetCurrentContext()->Windows.Size == windows, "disabled on-screen notifications draw no adjustment window");
    r::hud_enabled = notices_enabled;
    r::preset = requested;
    r::direct_runtime_requirements.dual_models = dual_models;
    r::configuration_generation = generation;
    identity.configuration_generation = generation;
    measured = saved_measured;
  }
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
  std::strcpy(render.path, "preSR");
  measured.passes[0].id = 1;
  measured.passes[0].model = addon::nr_model::ResolvePreset(r::preset.load(), r::pass_preset[0].load(), r::direct_runtime_requirements.dual_models);
  for (auto* sample : {&render, &measured}) {
    sample->queue = reinterpret_cast<void*>(uintptr_t{0x1234});
    sample->identity.source_generation = 77;
    sample->identity.sr_width = 1280;
    sample->identity.sr_height = 720;
    sample->identity.out_width = 2560;
    sample->identity.out_height = 1440;
    const auto sample_stage = timers::StageIndex(sample->path);
    sample->identity.stable_epoch = r::comparison_stable_epochs[sample_stage];
    r::stage_telemetry[sample_stage].last_evaluate.source_generation = 77;
    r::stage_telemetry[sample_stage].last_evaluate.source_released = false;
  }
  render.total_us = 1809;
  measured.total_us = 4781;
  r::monitor_snapshot.attempted_ns = 0;
  const auto cross_stage = r::RefreshMonitorSnapshot(true);
  check(cross_stage.monitor.passes[0].execution_confirmed && cross_stage.monitor.passes[1].execution_confirmed
            && cross_stage.summary.gpu == "6.6 ms" && cross_stage.summary.gpu_reference && cross_stage.card.nr_gpu_ms == 0.f
            && cross_stage.card.nr_present_ms == 0.f,
        "complete compatible stage spans expose a reference sum without claiming common-cycle latency");
  const auto valid_cross = measured;
  for (int rejection = 0; rejection < 12; ++rejection) {
    measured = valid_cross;
    switch (rejection) {
      case 0: measured.queue = reinterpret_cast<void*>(uintptr_t{0x5678}); break;
      case 1: ++measured.identity.configuration_generation; break;
      case 2: ++measured.identity.device_session; break;
      case 3: ++measured.identity.source_generation; break;
      case 4: measured.recorded_ns -= timers::internal::kSampleAgeNs + 1; break;
      case 5: measured.state = timers::SampleState::REPLAYED; break;
      case 6: measured.passes[0].state = timers::SampleState::NOT_EXECUTED; break;
      case 7: measured.passes[0].id = 0; break;
      case 8: measured.passes[0].model = 99; break;
      case 9: measured.passes[0].width = 0; break;
      case 10: ++measured.identity.hdr; break;
      case 11: measured.passes[1] = measured.passes[0]; break;
    }
    r::monitor_snapshot.attempted_ns = 0;
    const auto unavailable = r::RefreshMonitorSnapshot(true);
    check(unavailable.summary.gpu.empty() && !unavailable.summary.gpu_reference
              && unavailable.monitor.passes[0].gpu_state == ui::MonitorGpuState::CURRENT,
          "missing, stale, mismatched, replayed, duplicate or incompatible stage data never produces a partial sum");
  }
  measured = valid_cross;
  // Group totals include common processing; pass totals and Look are not added.
  render.passes[0].total_us = 1000;
  render.passes[0].look_us = 200;
  measured.passes[0].total_us = 4000;
  measured.passes[0].look_us = 300;
  r::monitor_snapshot.attempted_ns = 0;
  check(r::RefreshMonitorSnapshot(true).summary.gpu == "6.6 ms",
        "cross-stage reference counts each group's common overhead and nested Look exactly once");
  {
    const auto saved_history = std::make_shared<r::MonitorHistoryRuntime>(r::monitor_history);
    const auto saved_events = std::make_shared<decltype(timers::monitor_events.samples)>(timers::monitor_events.samples);
    const auto saved_published = timers::monitor_events.published;
    const bool saved_recording = timers::monitor_events.enabled.load();
    restore_values.emplace_back([saved_history, saved_events, saved_published, saved_recording] {
      r::monitor_history = *saved_history;
      timers::monitor_events.samples = *saved_events;
      timers::monitor_events.published = saved_published;
      timers::monitor_events.enabled = saved_recording;
    });
    auto& history = r::monitor_history;
    r::monitor_requested = true;
    history.values.Clear();
    history.active = true;
    history.needs_baseline = false;
    history.session = timers::device_session;
    history.consumed = timers::monitor_events.published;
    history.latest = {};
    history.contracts = {};
    history.summed = {};
    timers::monitor_events.enabled = true;
    auto raw_render = render;
    auto raw_upscaled = measured;
    raw_render.sequence = 100;
    raw_upscaled.sequence = 101;
    raw_render.measured_ns = raw_upscaled.measured_ns = r::SteadyNowNs();
    timers::QueueMonitorSample(raw_render);
    timers::QueueMonitorSample(raw_upscaled);
    r::monitor_snapshot.attempted_ns = 0;
    r::RefreshMonitorSnapshot(true);
    const auto& gpu_history = history.values.series[static_cast<unsigned>(ui::MonitorMetric::GPU)];
    const auto valid_points = [&] {
      size_t count = 0;
      for (size_t index = 0; index < gpu_history.count; ++index) count += gpu_history.At(index).valid;
      return count;
    };
    check(valid_points() == 1 && gpu_history.At(gpu_history.count - 1).reference,
          "new raw completion events produce one reference-history sample");
    for (int tick = 0; tick < 3; ++tick) {
      r::monitor_snapshot.attempted_ns = 0;
      r::RefreshMonitorSnapshot(true);
    }
    check(valid_points() == 1, "repeated UI cache refreshes cannot duplicate a real GPU measurement");
    ++raw_render.sequence;
    timers::QueueMonitorSample(raw_render);
    r::monitor_snapshot.attempted_ns = 0;
    r::RefreshMonitorSnapshot(true);
    check(valid_points() == 1, "cross-stage history waits for a new event from every required stage");
    raw_upscaled.sequence += 2;
    const auto recovered_upscaled = raw_upscaled;
    raw_upscaled.valid = false;
    raw_upscaled.state = timers::SampleState::REPLAYED;
    raw_upscaled.queue = nullptr;
    raw_upscaled.width = raw_upscaled.height = 0;
    timers::QueueMonitorSample(raw_upscaled);
    r::monitor_snapshot.attempted_ns = 0;
    r::RefreshMonitorSnapshot(true);
    check(!gpu_history.At(gpu_history.count - 1).valid, "rejected GPU events leave explicit curve gaps");
    const auto history_generation = history.values.generation;
    raw_upscaled = recovered_upscaled;
    ++raw_upscaled.sequence;
    timers::QueueMonitorSample(raw_upscaled);
    r::monitor_snapshot.attempted_ns = 0;
    r::RefreshMonitorSnapshot(true);
    check(valid_points() == 2 && history.values.generation == history_generation,
          "same-contract recovery preserves earlier GPU history and the rejected-event gap");
    const auto published = timers::monitor_events.published;
    timers::monitor_events.enabled = false;
    timers::QueueMonitorSample(raw_render);
    check(timers::monitor_events.published == published, "closed trend recording does no dedicated GPU history work");

    // Exercise configuration boundaries through the production preparation
    // and completion-event consumers, retaining the original time axis.
    history.configuration = r::configuration_generation.load();
    history.epoch = r::monitor_history_epoch.load();
    history.active = true;
    timers::monitor_events.enabled = true;
    for (const auto hook : {r::kHookRender, r::kHookUpscaled, r::kHookPresent, r::kHookRender}) {
      r::hook_point = hook;
      r::stack_passes = 1;
      const auto count = gpu_history.count;
      const auto segment = history.values.segment;
      ++r::configuration_generation;
      identity.configuration_generation = r::configuration_generation.load();
      r::PrepareMonitorHistory(true);
      check(gpu_history.count == count && history.values.segment > segment,
            "Render, Upscaled and Present configuration boundaries retain previous GPU history");
      r::monitor_snapshot.attempted_ns = 0;
      check(r::RefreshMonitorSnapshot(true).summary.gpu.empty(), "stage changes cannot use previous current GPU numbers");
      auto fresh = raw_render;
      fresh.identity = identity;
      fresh.measured_ns = fresh.recorded_ns = r::SteadyNowNs();
      fresh.sequence = ++raw_render.sequence;
      std::strcpy(fresh.path, hook == r::kHookRender ? "preSR" : hook == r::kHookPresent ? "present" : "upscaled");
      fresh.identity.stable_epoch = r::comparison_stable_epochs[timers::StageIndex(fresh.path)];
      timers::QueueMonitorSample(fresh);
      r::monitor_snapshot.attempted_ns = 0;
      r::RefreshMonitorSnapshot(true);
      check(gpu_history.count > count && gpu_history.At(gpu_history.count - 1).valid
                && gpu_history.At(gpu_history.count - 1).segment == history.values.segment,
            "fresh stage completion starts a separate valid segment on the same time axis");
    }
    for (int change = 0; change < 5; ++change) {
      const auto count = gpu_history.count;
      const auto segment = history.values.segment;
      if (change < 2) r::stack_passes = change == 0 ? 2 : 1;
      else if (change < 4) r::preset = change == 2 ? 1 : 2;
      else r::nr_resolution_scale = .67f;
      ++r::configuration_generation;
      identity.configuration_generation = r::configuration_generation.load();
      r::PrepareMonitorHistory(true);
      check(gpu_history.count == count && history.values.segment > segment,
            "pass count, model and scale changes retain history with a new configuration segment");
    }
    const auto retained = gpu_history.count;
    r::ToggleMonitor();
    r::ToggleMonitor();
    auto segment = history.values.segment;
    r::PrepareMonitorHistory(true);
    check(gpu_history.count == retained && history.values.segment > segment, "F11 reopen retains history and begins a new segment");
    r::PrepareMonitorHistory(false);
    segment = history.values.segment;
    r::PrepareMonitorHistory(true);
    check(gpu_history.count == retained && history.values.segment > segment, "trend pause and resume retain history without joining the gap");
    const auto session = history.session;
    history.session = session + 1;
    r::monitor_snapshot.attempted_ns = 0;
    r::RefreshMonitorSnapshot(true);
    check(valid_points() == 0 && history.session == timers::device_session, "device-session mismatch safely resets incompatible GPU history");
    history.active = false;
  }
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
