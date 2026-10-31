/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <limits>

#include "ui/runtime_monitor.hpp"
#include "ui/profile_hotkeys.hpp"

template <typename Check>
void RunRuntimeMonitorCases(const Check& check) {
  namespace ui = renodx::addons::dlss5::ui;
  auto& io = ImGui::GetIO();
  const auto original_display = io.DisplaySize;
  for (int language = 0; language < ui::kLanguageCount; ++language) {
    ui::SelectLanguage(language);
    for (auto stage : {renodx::addons::dlss5::stage_plan::Stage::RENDER,
                       renodx::addons::dlss5::stage_plan::Stage::UPSCALED,
                       renodx::addons::dlss5::stage_plan::Stage::PRESENT}) {
      check(std::strcmp(ui::StageDisplayName(stage), ui::Tr(renodx::addons::dlss5::stage_plan::Name(stage))) == 0,
            "all supported languages share the menu and monitor stage vocabulary");
    }
  }
  ui::SelectLanguage(ui::kLanguageCount - 2);
  check(std::strcmp(ui::StageDisplayName(renodx::addons::dlss5::stage_plan::Stage::RENDER), "渲染") == 0
            && std::strcmp(ui::StageDisplayName(renodx::addons::dlss5::stage_plan::Stage::UPSCALED), "超分后") == 0
            && std::strcmp(ui::StageDisplayName(renodx::addons::dlss5::stage_plan::Stage::PRESENT), "呈现") == 0,
        "Simplified Chinese uses the existing player-facing stage names");
  ui::SelectLanguage(0);
  ui::StatusSummaryFields fields{.resolution = "3840x2160 -> 7680x4320",
                                 .stage = "Render -> Upscaled -> Present", .gpu = "1.2 ms",
                                 .resolution_tip = "Resolution evidence help",
                                 .stage_tip = "Stage evidence help", .gpu_tip = "GPU evidence help"};
  ui::StatusCard summary_card{.title = "Neural Rendering is on"};
  ImGuiWindow* summary_window = nullptr;
  const auto summary_frame = [&](float width, float font) {
    ImGui::GetStyle().FontSizeBase = font;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
    ImGui::SetNextWindowSize(ImVec2(width, 1400.f));
    ImGui::Begin("adjacent summary widths", nullptr, ImGuiWindowFlags_NoSavedSettings);
    const float available = ImGui::GetContentRegionAvail().x
        - (ui::tokens::kCardPadding + ui::kStripeWidth) * 2.f;
    const float measured_font = ImGui::GetFontSize();
    ImGui::LogToBuffer();
    ui::DrawStatusSummary(summary_card, 0, fields);
    const std::string log = ImGui::GetCurrentContext()->LogBuffer.c_str();
    ImGui::LogFinish();
    for (auto* candidate : ImGui::GetCurrentContext()->Windows) {
      if (candidate->Active && std::strstr(candidate->Name, "##nr_status_summary") != nullptr) {
        summary_window = candidate;
        check(candidate->DC.CursorMaxPos.x <= candidate->InnerRect.Max.x + 1.f,
              "adjacent-width structured summary never overflows horizontally");
        check(candidate->DC.CursorMaxPos.y <= candidate->InnerRect.Max.y + 1.f,
              "independent fields wrap inside their allocated rows");
      }
    }
    ImGui::End();
    ImGui::Render();
    check(log.find(fields.gpu) != std::string::npos,
          "NR GPU remains visible at all supported summary widths");
    check((log.find(fields.stage) != std::string::npos) == (available >= measured_font * 56.f),
          "stage is auxiliary while metrics keep priority");
    check((log.find(fields.resolution) != std::string::npos) == (available >= measured_font * 56.f),
          "working resolution is auxiliary rather than the sole narrow metric");
    return log;
  };
  for (const auto& theme : ui::kThemes) {
    ui::SetTheme(theme.id);
    for (int language : {0, 3, 13}) {
      ui::SelectLanguage(language);
      for (float font : {13.f, 19.5f, 26.f}) {
        for (float boundary : {28.f, 56.f}) {
          for (float offset : {-1.f, 0.f, 1.f}) {
            summary_frame(boundary * std::round(font) + 48.f + offset, font);
          }
        }
      }
    }
  }
  ui::SelectLanguage(0);
  summary_frame(1600.f, 13.f);
  io.AddMousePosEvent(summary_window->Pos.x + summary_window->WindowPadding.x + 5.f,
                      summary_window->Pos.y + summary_window->WindowPadding.y + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y + 5.f);
  std::string tooltip_log;
  for (int tick = 0; tick < 60; ++tick) tooltip_log = summary_frame(1600.f, 13.f);
  const auto help = tooltip_log.find("GPU evidence help");
  check(help != std::string::npos && tooltip_log.find("GPU evidence help", help + 1) == std::string::npos,
        "hovering a structured field shows its explanation exactly once");
  check(tooltip_log.find("Stage evidence help") == std::string::npos
        && tooltip_log.find("Resolution evidence help") == std::string::npos,
        "neighboring field tooltips do not repeat in the hovered tooltip");
  io.AddMousePosEvent(-100.f, -100.f);
  ui::MonitorGeometry invalid;
  invalid.size.x = std::numeric_limits<float>::quiet_NaN();
  auto safe = ui::ClampMonitorGeometry(invalid, ImVec2(100.f, 50.f), ImVec2(800.f, 600.f), 1.f);
  check(safe.valid && safe.size.x == 800.f && safe.position.x == 100.f,
        "invalid geometry starts with a bounded useful default");
  safe.position = ImVec2(200.f, 100.f);
  safe.size = ImVec2(400.f, 300.f);
  const auto scaled = ui::ClampMonitorGeometry(safe, ImVec2(-1200.f, 100.f), ImVec2(1600.f, 1200.f), 2.f);
  check(scaled.position.x == -1000.f && scaled.position.y == 200.f
        && scaled.size.x == 800.f && scaled.size.y == 600.f,
        "viewport relocation and DPI changes preserve relative geometry");
  const auto tiny = ui::ClampMonitorGeometry(scaled, ImVec2(0.f, 0.f), ImVec2(200.f, 150.f), 1.f);
  check(tiny.position.x == 0.f && tiny.position.y == 0.f
        && tiny.size.x <= 200.f && tiny.size.y <= 150.f, "small work area keeps the entire monitor reachable");
  for (int anchor = 0; anchor < 9; ++anchor) {
    const ui::MonitorGeometry source{.size = ImVec2(400.f, 300.f), .ui_scale = 1.f, .valid = true};
    const auto positioned = ui::PositionMonitorGeometry(source, anchor, ImVec2(10.f, -5.f),
        ImVec2(-1200.f, 100.f), ImVec2(1600.f, 1200.f), 2.f);
    const float expected_x = std::clamp(-1200.f + 400.f * (anchor % 3) + 20.f, -1200.f, -400.f);
    const float expected_y = std::clamp(100.f + 300.f * (anchor / 3) - 10.f, 100.f, 700.f);
    check(positioned.position.x == expected_x && positioned.position.y == expected_y
              && positioned.size.x == 800.f && positioned.size.y == 600.f,
          "all nine anchors share scaled offsets and the viewport clamp");
  }
  const auto custom = ui::PositionMonitorGeometry(scaled, -1, ImVec2(999.f, 999.f),
      ImVec2(-1200.f, 100.f), ImVec2(1600.f, 1200.f), 2.f);
  check(custom.position.x == scaled.position.x && custom.position.y == scaled.position.y,
        "custom geometry ignores anchor offsets");
  const ui::MonitorPoint previous{.time_ns = 1'000'000'000LL, .value = 1.f, .valid = true};
  ui::MonitorPoint next{.time_ns = 1'200'000'000LL, .value = 2.f, .valid = true};
  check(ui::CanConnectMonitorPoints(previous, next, ui::MonitorMetric::FPS),
        "cadence points connect at the inclusive 200 ms boundary");
  ++next.time_ns;
  check(!ui::CanConnectMonitorPoints(previous, next, ui::MonitorMetric::FPS)
            && ui::CanConnectMonitorPoints(previous, next, ui::MonitorMetric::GPU),
        "cadence and GPU use independent connection windows");
  next.time_ns = 1'750'000'000LL;
  check(ui::CanConnectMonitorPoints(previous, next, ui::MonitorMetric::PASS_4),
        "GPU points connect at the inclusive 750 ms boundary");
  ++next.time_ns;
  check(!ui::CanConnectMonitorPoints(previous, next, ui::MonitorMetric::GPU),
        "GPU points do not bridge long gaps");
  next.time_ns = 1'100'000'000LL;
  next.reference = true;
  check(!ui::CanConnectMonitorPoints(previous, next, ui::MonitorMetric::GPU),
        "reference and current series never connect");
  next.reference = false;
  next.valid = false;
  check(!ui::CanConnectMonitorPoints(previous, next, ui::MonitorMetric::GPU),
        "invalid points break the curve rather than becoming zero measurements");
  next.valid = true;
  next.time_ns = previous.time_ns;
  check(!ui::CanConnectMonitorPoints(previous, next, ui::MonitorMetric::GPU),
        "equal timestamps do not invent an event interval");

  ui::MonitorInputs inputs;
  inputs.card.title = "Neural Rendering is on";
  inputs.current_conclusion = "Confirmed source frames";
  inputs.recent = {.available = true, .ready = true, .eligible = 120, .injected = 120,
                   .last_sample_ns = 1'000'000'000LL};
  inputs.now_ns = 1'500'000'000LL;
  inputs.gpu_scope = "Completed submission";
  inputs.current_limit = "Synthetic evidence only";
  for (auto& pass : inputs.passes) {
    pass = {.requested_stage = "Render", .effective_stage = "Upscaled",
            .execution_proof = "Completed submission", .requested_model = "Transformer",
            .created_model_request = "Transformer", .requested_size = "2560x1440",
            .actual_size = "2560x1440", .gpu_cost = "1.2 ms", .gpu_state = ui::MonitorGpuState::CURRENT};
  }
  ui::MonitorState state{.detailed = true};
  float scale = 1.f;
  bool narrow = false;
  std::string output;
  const auto draw = [&](const ImVec2* next_pos = nullptr, const ImVec2* next_size = nullptr) {
    io.DisplaySize = ImVec2((narrow ? 700.f : 2400.f) * scale, 1400.f * scale);
    ImGui::GetStyle().FontSizeBase = 13.f * scale;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
    ImGui::SetNextWindowSize(ImVec2((narrow ? 680.f : 650.f) * scale, 1300.f * scale));
    ImGui::Begin("monitor test host", nullptr, ImGuiWindowFlags_NoSavedSettings);
    if (next_pos != nullptr) ImGui::SetWindowPos("###nr_runtime_monitor", *next_pos);
    if (next_size != nullptr) ImGui::SetWindowSize("###nr_runtime_monitor", *next_size);
    ImGui::LogToBuffer();
    const float before_y = ImGui::GetCursorPosY();
    if (auto* raw = ImGui::FindWindowByName("###nr_runtime_monitor")) {
      raw->StateStorage.SetInt(raw->GetID("###monitor_raw_evidence"), 1);
    }
    const auto actions = ui::DrawRuntimeMonitor(inputs, &state);
    output = ImGui::GetCurrentContext()->LogBuffer.c_str();
    ImGui::LogFinish();
    const float after_y = ImGui::GetCursorPosY();
    if (!narrow) check(before_y == after_y, "floating monitor does not consume parameter layout");
    ImGui::End();
    ImGui::Render();
    check(ui::style_depth == 0, "monitor restores theme scopes");
    return actions;
  };
  for (const auto& theme : ui::kThemes) {
    ui::SetTheme(theme.id);
    for (int language : {0, 3, 13}) {
      ui::SelectLanguage(language);
      for (float next_scale : {1.f, 1.5f, 2.f}) {
        scale = next_scale;
        for (unsigned count = 1; count <= 4; ++count) {
          inputs.pass_count = count;
          state.open = true;
          draw();
          draw();
          auto* window = ImGui::FindWindowByName("###nr_runtime_monitor");
          check(window != nullptr && window->Active, "monitor uses a nonmodal independent Begin window");
          check((window->Flags & ImGuiWindowFlags_Modal) == 0, "monitor never captures a modal stack");
          check(window->Pos.x >= 0.f && window->Pos.y >= 0.f
                && window->Pos.x + window->Size.x <= io.DisplaySize.x + 1.f
                && window->Pos.y + window->Size.y <= io.DisplaySize.y + 1.f,
                "all themes/languages/scales keep the monitor in its viewport");
        }
      }
    }
  }
  ui::SelectLanguage(0);
  scale = 1.f;
  draw();
  draw();
  const ImVec2 moved(900.f, 130.f), resized(800.f, 610.f);
  draw(&moved, &resized);
  check(state.geometry.position.x == moved.x && state.geometry.position.y == moved.y
        && state.geometry.size.x == resized.x && state.geometry.size.y == resized.y,
        "dragged/resized geometry is captured instead of overwritten each frame");
  draw();
  check(state.geometry.position.x == moved.x && state.geometry.size.x == resized.x,
        "geometry remains stable on the following frame");
  state.pin = true;
  draw();
  auto* window = ImGui::FindWindowByName("###nr_runtime_monitor");
  check((window->Flags & (ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize))
        == (ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize), "pin locks both move and resize");
  state.pin = false;
  draw();
  check((window->Flags & (ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) == 0,
        "unpin restores dragging and resizing");
  const auto saved = state.geometry;
  state.open = false;
  draw();
  check(!window->Active, "closed monitor stops submitting its window");
  state.open = true;
  draw();
  draw();
  check(window->Active && state.geometry.position.x == saved.position.x
        && state.geometry.size.x == saved.size.x, "reopen remembers geometry");
  check(output.find("0.5 s ago") != std::string::npos, "recent evidence displays monotonic sample age");
  inputs.now_ns = 4'000'000'000LL;
  inputs.passes[0].gpu_state = ui::MonitorGpuState::STALE;
  inputs.passes[1].gpu_state = ui::MonitorGpuState::UNAVAILABLE;
  draw();
  check(output.find("Stale recent sample") != std::string::npos
        && output.find("Stale GPU sample") != std::string::npos
        && output.find("Pending confirmation") != std::string::npos,
        "stale samples and missing evidence remain distinct");
  const std::string explanation = "A created model request does not prove the internal network actually used.";
  const auto first = output.find(explanation);
  check(first != std::string::npos && output.find(explanation, first + 1) == std::string::npos,
        "internal network limitation appears only once");
  narrow = true;
  draw();
  check(output.find("Confirmed source frames") == std::string::npos,
        "narrow fallback is collapsed by default");
  // Explicit disclosure must still show only key evidence, never the long tables.
  ImGui::NewFrame();
  ImGui::Begin("monitor test host", nullptr, ImGuiWindowFlags_NoSavedSettings);
  ImGui::SetNextItemOpen(true, ImGuiCond_Always);
  ImGui::LogToBuffer();
  const float start = ImGui::GetCursorPosY();
  ui::DrawRuntimeMonitor(inputs, &state);
  output = ImGui::GetCurrentContext()->LogBuffer.c_str();
  ImGui::LogFinish();
  check(ImGui::GetCursorPosY() - start < 350.f, "narrow disclosure leaves space for parameters");
  check(output.find("Confirmed source frames") != std::string::npos
        && output.find("NR GPU") != std::string::npos
        && output.find("Created model request") == std::string::npos
        && output.find("Observed working size") == std::string::npos, "narrow mode shows only key summary");
  ImGui::End();
  ImGui::Render();
  io.DisplaySize = original_display;
  if (auto* raw = ImGui::FindWindowByName("###nr_runtime_monitor")) {
    raw->StateStorage.SetInt(raw->GetID("###monitor_raw_evidence"), 0);
  }

  // The pure hotkey contract is exercised here without changing profile code.
  ui::ProfileKeyEdge key;
  check(!key.Poll(65, true, true, true), "binding a held key never fires");
  check(!key.Poll(65, true, false, true), "release rearms a binding");
  check(key.Poll(65, true, true, true), "new press fires once");
  check(!key.Poll(65, true, true, true), "holding does not repeat");
  key.Poll(65, true, false, false);
  check(!key.Poll(65, true, true, false) && !key.Poll(65, true, true, true),
        "a blocked edge is consumed instead of firing after focus returns");
  check(!key.Poll(66, true, true, true), "rebinding a held key does not fire");
  check(!key.Poll(66, false, false, true) && !key.Poll(66, true, true, true),
        "enabling while held does not fire");
  ui::ProfileKeyEdge context_key;
  context_key.Poll(65, true, false, true);
  check(context_key.Poll(65, true, true, true), "gameplay edge fires before the overlay transition");
  check(!context_key.Poll(65, true, false, true, true)
        && !context_key.Poll(65, true, true, true, true),
        "context switch fake release/repress cannot cycle a profile");
  check(!context_key.Poll(65, true, false, true, true)
        && context_key.Poll(65, true, true, true, true),
        "a later release rearms a real press in overlay context");
  check(!context_key.Poll(65, true, true, true, false)
        && !context_key.Poll(65, true, true, true, false),
        "returning to gameplay while held never repeats");
  context_key.Poll(65, true, false, true, false);
  check(context_key.Poll(65, true, true, true, false), "gameplay rearmed after release");
  const std::array<std::uint32_t, 4> bindings = {65, 66, 65, 67};
  check(ui::HotkeyConflict(bindings, {true, true, true, true}, 0), "duplicate enabled hotkeys conflict");
  check(!ui::HotkeyConflict(bindings, {true, true, false, true}, 0), "disabled duplicates do not conflict");
}
