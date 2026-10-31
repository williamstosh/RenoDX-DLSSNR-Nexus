/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

// Offscreen CPU measurements only. Profiling is a UI setting here, with no
// GPU submission, game frame pacing or NVIDIA model work in this fixture.
template <typename Check>
void RunMonitorPerformanceCases(const Check& check) {
  namespace runtime = renodx::addons::dlss5::internal;
  namespace ui = renodx::addons::dlss5::ui;
  auto& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(1600.f, 1000.f);
  io.AddMousePosEvent(-100.f, -100.f);
  runtime::ui_menu_page = 0;
  runtime::profile_feedback_until_ns = 0;
  runtime::hud_toast_until_ns = 0;
  runtime::health_notice_until_ns = 0;
  runtime::hud_enabled = false;
  runtime::monitor_runtime = nullptr;
  constexpr unsigned frames = 180;
  const bool paced = std::getenv("NR_NEXUS_MONITOR_PERFORMANCE_PACED") != nullptr;
  for (const unsigned passes : {1u, 4u}) {
    for (const bool profiling : {false, true}) {
      runtime::stack_passes = passes;
      runtime::gpu_timers_enabled = profiling;
      for (unsigned scene = 0; scene < 10; ++scene) {
        const bool menu = scene == 1 || scene == 2 || (scene >= 4 && scene <= 6) || scene == 9;
        const bool hud = scene >= 2;
        const char* label = scene == 0 ? "closed" : scene == 1 ? "menu" : scene == 2 ? "menu_monitor"
            : scene == 3 ? "passive_hud" : scene == 4 ? "drag" : scene == 5 ? "resize" : scene == 6 ? "view_switch"
            : scene == 7 ? "passive_detailed" : scene == 8 ? "passive_trends" : "menu_trends";
        runtime::monitor_requested = hud;
        runtime::hud_overlay_open = menu;
        runtime::monitor_snapshot = {};
        runtime::monitor_observation_epoch.fetch_add(1);
        runtime::ui_runtime_monitor_state = {.open = hud, .detailed = scene == 7};
        runtime::ui_runtime_monitor_state.trends = scene >= 8;
        runtime::ui_monitor_saved_detailed = scene == 7;
        runtime::ui_monitor_saved_pin = false;
        runtime::ui_monitor_geometry_dirty = false;
        std::vector<double> times;
        times.reserve(frames);
        uint64_t before_refresh = 0, before_deferred = 0, before_saves = 0;
        for (unsigned frame = 0; frame < frames + 24; ++frame) {
          if (frame == 24) {
            before_refresh = runtime::monitor_snapshot.refreshes;
            before_deferred = runtime::monitor_snapshot.deferred;
            before_saves = runtime::ui_monitor_save_batches;
          }
          rendered_text.clear();
          tooltip_text.clear();
          const auto start = std::chrono::steady_clock::now();
          ImGui::NewFrame();
          if (frame == 0) ImGui::SetWindowFocus(nullptr);
          if (scene == 4 || scene == 5) io.MouseDown[0] = frame + 1 < frames + 24;
          if (frame >= 24) {
            if (auto* window = ImGui::FindWindowByName("###nr_runtime_monitor")) {
              if (scene == 4) ImGui::SetWindowPos(window, ImVec2(12.f + frame % 120, 12.f));
              if (scene == 5) ImGui::SetWindowSize(window, ImVec2(500.f + frame % 120, 500.f));
            }
            if (scene == 6 && frame % 30 == 0) runtime::ui_runtime_monitor_state.detailed = !runtime::ui_runtime_monitor_state.detailed;
          }
          if (menu) {
            ImGui::SetNextWindowPos(ImVec2(760.f, 10.f));
            ImGui::SetNextWindowSize(ImVec2(820.f, 960.f));
            ImGui::Begin("Monitor performance Home fixture", nullptr, ImGuiWindowFlags_NoSavedSettings);
            runtime::OnOverlay(nullptr);
            ImGui::End();
          }
          const auto* focus_before = ImGui::GetCurrentContext()->NavWindow;
          if (hud) runtime::OnHudOverlay(nullptr);
          if (hud && !menu) check(ImGui::GetCurrentContext()->NavWindow == focus_before, "passive monitor never changes focus");
          ImGui::Render();
          const auto elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
          if (scene >= 8 && frame == 23) {
            const auto now = runtime::SteadyNowNs();
            for (int64_t point = 0; point < 1200; ++point) {
              const auto time = now - (1200 - point) * ui::MonitorSeries::BUCKET_NS;
              for (unsigned metric = 0; metric < static_cast<unsigned>(ui::MonitorMetric::COUNT); ++metric) {
                runtime::monitor_history.values.Add(static_cast<ui::MonitorMetric>(metric), time,
                    metric == 0 ? 120.f + static_cast<float>(point % 20) : 2.f + static_cast<float>(point % 11));
              }
            }
          }
          if (frame >= 24) times.push_back(elapsed);
          if (paced) std::this_thread::sleep_until(start + std::chrono::microseconds(16667));
        }
        io.MouseDown[0] = false;
        std::sort(times.begin(), times.end());
        const auto percentile = [&](double fraction) { return times[static_cast<size_t>(std::ceil(fraction * times.size())) - 1]; };
        const auto refreshes = runtime::monitor_snapshot.refreshes - before_refresh;
        const auto deferred = runtime::monitor_snapshot.deferred - before_deferred;
        const auto saves = runtime::ui_monitor_save_batches - before_saves;
        check(times.size() == frames, "monitor benchmark includes every measured CPU frame");
        if (scene <= 3 || scene >= 7) check(saves == 0, "stationary monitor never persists settings on a timer");
        if (!menu && !hud) check(refreshes == 0, "closed UI does not collect monitor snapshots");
        std::printf("MONITOR_CPU scene=%s passes=%u profiling_setting=%u frames=%u p50_us=%.3f p95_us=%.3f p99_us=%.3f max_us=%.3f refreshes=%llu deferred=%llu save_batches=%llu paced_hz=%u gpu_measured=0\n",
            label, passes, profiling, frames, percentile(.50), percentile(.95), percentile(.99), times.back(),
            static_cast<unsigned long long>(refreshes), static_cast<unsigned long long>(deferred), static_cast<unsigned long long>(saves), paced ? 60u : 0u);
      }
    }
  }
  runtime::monitor_requested = false;
  runtime::hud_overlay_open = false;
  std::puts("Monitor CPU benchmark passed; synthetic host/model inputs, no GPU timings or game P95/P99 claim.");
}
