/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include <imgui.h>
#include <imgui_internal.h>

#include "ui/runtime_monitor.hpp"

// Call between frames in the existing real-ImGui fixture, with fonts initialized.
// This function advances frames and owns neither creation nor destruction of the context.
template <typename Check>
void RunMonitorHudCases(const Check& check) {
  namespace ui = renodx::addons::dlss5::ui;
  auto& io = ImGui::GetIO();
  auto& context = *ImGui::GetCurrentContext();
  const auto original_display = io.DisplaySize;
  const auto original_config = io.ConfigFlags;
  const float original_font = ImGui::GetStyle().FontSizeBase;
  const auto original_theme = ui::CurrentThemeId();
  const int original_language = ui::CurrentLanguage();
  ui::SelectLanguage(0);
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
  io.AddMousePosEvent(-100.f, -100.f);

  ui::MonitorInputs inputs;
  inputs.card.title = "Neural Rendering is on";
  inputs.current_conclusion = "Confirmed source frames";
  inputs.recent = {.available = true, .ready = true, .eligible = 60, .injected = 60,
                   .last_sample_ns = 1'000'000'000LL};
  inputs.now_ns = 1'500'000'000LL;
  inputs.gpu_scope = "Completed submission";
  inputs.current_limit = "Synthetic evidence only";
  inputs.summary = {.resolution = "2560x1440 -> 3840x2160", .stage = "Render", .gpu = "4.8 ms"};
  inputs.summary.fps = "SOURCE_FPS";
  inputs.summary.fg = "FG_STATUS";
  for (auto& pass : inputs.passes) {
    pass = {.requested_stage = "Render", .effective_stage = "Render",
            .execution_proof = "Completed submission", .requested_model = "Transformer",
            .created_model_request = "Transformer", .requested_size = "67%",
            .actual_size = "2560x1440", .gpu_cost = "1.2 ms", .gpu_state = ui::MonitorGpuState::CURRENT,
            .execution_confirmed = true};
  }
  ui::MonitorState state{.open = true};
  bool interactive = false;
  bool parent = false;
  const ui::MonitorGeometry* saved_geometry = nullptr;
  ImGuiWindow* window = nullptr;
  std::string output;
  const auto same_geometry = [](const ui::MonitorGeometry& first, const ui::MonitorGeometry& second) {
    return first.position.x == second.position.x && first.position.y == second.position.y
        && first.size.x == second.size.x && first.size.y == second.size.y
        && first.ui_scale == second.ui_scale && first.valid == second.valid;
  };
  const auto frame = [&] {
    ImGui::NewFrame();
    if (parent) {
      ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
      ImGui::SetNextWindowSize(io.DisplaySize);
      ImGui::Begin("monitor HUD parent", nullptr, ImGuiWindowFlags_NoSavedSettings);
      ImGui::SetWindowFocus();
    }
    const auto* focus = context.NavWindow;
    const ImGuiID active_id = context.ActiveId;
    const float before_y = ImGui::GetCursorPosY();
    ImGui::LogToBuffer();
    const auto actions = ui::DrawRuntimeMonitor(inputs, &state, saved_geometry, interactive, true);
    output = context.LogBuffer.c_str();
    ImGui::LogFinish();
    check(ImGui::GetCursorPosY() == before_y, "independent HUD consumes no parent layout even on a narrow screen");
    if (!interactive) {
      check(context.NavWindow == focus && context.ActiveId == active_id,
            "passive HUD leaves the current focus and active item unchanged");
      check(!actions.open_diagnostics && !actions.settings_changed, "passive HUD produces no interactive actions");
    }
    window = ImGui::FindWindowByName("###nr_runtime_monitor");
    if (parent) ImGui::End();
    ImGui::Render();
    check(ui::style_depth == 0, "HUD restores all theme scopes");
    return actions;
  };

  // Fresh state intentionally reuses the same ImGui window ID: state, not old
  // window contents, determines the first size (including a new settings session).
  for (const auto& theme : ui::kThemes) {
    ui::SetTheme(theme.id);
    for (float scale : {1.f, 1.5f, 2.f}) {
      ImGui::GetStyle().FontSizeBase = 13.f * scale;
      for (const ImVec2 display : {ImVec2(2400.f, 1400.f), ImVec2(700.f, 900.f), ImVec2(260.f, 180.f)}) {
        io.DisplaySize = ImVec2(display.x * scale, display.y * scale);
        parent = display.x == 700.f;
        for (bool detailed : {false, true}) {
          float previous_height = 0.f;
          for (unsigned count = 1; count <= 4; ++count) {
            inputs.pass_count = count;
            state = {.open = true, .detailed = detailed};
            const auto initial = frame();
            frame();
            check(initial.geometry_changed, "first independent geometry is returned for persistence");
            check(window != nullptr && window->Active && !window->HasCloseButton,
                  "passive HUD is an independent window without a close control");
            const ImGuiWindowFlags required = ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing
                | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar
                | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus;
            check((window->Flags & required) == required
                      && (window->Flags & (ImGuiWindowFlags_Modal | ImGuiWindowFlags_AlwaysAutoResize)) == 0,
                  "passive flags prohibit mouse, navigation, focus on appearance, movement, resizing and auto-size");
            check(state.geometry.valid && state.geometry.position.x >= 0.f && state.geometry.position.y >= 0.f
                      && state.geometry.position.x + state.geometry.size.x <= io.DisplaySize.x + 1.f
                      && state.geometry.position.y + state.geometry.size.y <= io.DisplaySize.y + 1.f,
                  "one through four passes fit wide, narrow and tiny displays at every tested DPI");
            check(state.geometry.size.x == (std::min)((detailed ? 620.f : 480.f) * scale, io.DisplaySize.x)
                      && state.geometry.size.y >= previous_height,
                  "first size respects mode, pass count and available screen space");
            previous_height = state.geometry.size.y;
            if (display.x == 2400.f) {
              check(std::abs(state.geometry.size.y
                                 - (detailed ? 340.f + 156.f * count : 132.f + 62.f * count) * scale) <= 1.f,
                    "first height grows with pass count rather than previous window contents");
            }
            check(output.find("Confirmed source frames") != std::string::npos
                      && output.find("Execution chain") != std::string::npos,
                  "both modes preserve the existing conclusion and stage evidence");
            check(output.find("Created model request") == std::string::npos
                      && output.find("Eligible / injected / gaps") == std::string::npos,
                  "neither passive dashboard submits raw tables or counters");
            check(output.find("ReShade FPS") != std::string::npos
                      && output.find("SOURCE_FPS") != std::string::npos && output.find("FG_STATUS") != std::string::npos
                      && output.find("4.8 ms") != std::string::npos && output.find("67%") != std::string::npos
                      && output.find("2560x1440") != std::string::npos,
                  "dashboard consumes formatted metrics, configured ratio and observed size");
            for (unsigned pass = 0; pass < count; ++pass) {
              check(output.find("P" + std::to_string(pass + 1)) != std::string::npos,
                    "both dashboards retain every vertical pass node");
            }
            if (!detailed && display.x != 260.f) {
              check(window->ContentSize.y <= window->InnerRect.GetHeight() + 1.f,
                    "first compact geometry fits its evidence without gameplay scrolling");
            }
            check(output.find("Open Diagnostics") == std::string::npos
                      && output.find("Close monitor") == std::string::npos
                      && output.find("Lock position and size") == std::string::npos
                      && output.find("Detailed monitor") == std::string::npos,
                  "passive mode submits no interactive controls");
            const auto geometry = state.geometry;
            inputs.current_conclusion.assign(1000, 'W');
            const auto changed_text = frame();
            check(same_geometry(geometry, state.geometry) && !changed_text.geometry_changed,
                  "changing runtime text never resizes or moves the HUD");
            inputs.current_conclusion = "Confirmed source frames";
          }
        }
      }
    }
  }

  parent = false;
  io.DisplaySize = ImVec2(2400.f, 1400.f);
  ImGui::GetStyle().FontSizeBase = 13.f;
  inputs.pass_count = 4;
  state = {.open = true, .detailed = false};
  check(!ui::MonitorState{}.detailed, "new monitor sessions default to the compact dashboard");
  frame();
  check(output.find("Open the menu to edit the monitor.") != std::string::npos
            && output.find("F11") == std::string::npos && output.find("Home") == std::string::npos,
        "passive edit hint does not assume default or custom shortcut bindings");
  inputs.summary.fps.clear();
  inputs.summary.fg.clear();
  frame();
  check(output.find("SOURCE_FPS") == std::string::npos && output.find("FG_STATUS") == std::string::npos
            && output.find("—") != std::string::npos,
        "missing formatted FPS and FG remain unavailable without derived metrics");
  inputs.passes[0].effective_stage = "Upscaled";
  inputs.passes[0].stage_reason = "RR fallback requires Upscaled";
  inputs.passes[0].execution_confirmed = false;
  inputs.passes[0].created_model_request = "CNN";
  frame();
  const auto adjustment = output.find("Stage adjusted");
  check(adjustment != std::string::npos && output.find("Stage adjusted", adjustment + 1) == std::string::npos
            && output.find("RR fallback requires Upscaled") != std::string::npos
            && output.find("Model request adjusted") != std::string::npos
            && output.find("CNN") != std::string::npos && output.find("Pending confirmation") != std::string::npos,
        "only the mismatched pass shows its stage reason, model request and pending execution");
  inputs.passes[0].effective_stage = "Render";
  inputs.passes[0].stage_reason.clear();
  inputs.passes[0].created_model_request = "Transformer";
  inputs.passes[0].execution_confirmed = true;
  inputs.summary.fps = "SOURCE_FPS";
  inputs.summary.fg = "FG_STATUS";
  inputs.passes[0].gpu_cost = "CURRENT_GPU_COST";
  inputs.passes[1].gpu_state = ui::MonitorGpuState::STALE;
  inputs.passes[1].gpu_cost = "STALE_GPU_MUST_NOT_BE_CURRENT";
  inputs.passes[2].gpu_state = ui::MonitorGpuState::UNAVAILABLE;
  inputs.passes[2].gpu_cost = "UNAVAILABLE_GPU_MUST_NOT_BE_CURRENT";
  state.detailed = true;
  frame();
  check(output.find("CURRENT_GPU_COST") != std::string::npos
            && output.find("Stale GPU sample") != std::string::npos
            && output.find("Pending confirmation") != std::string::npos
            && output.find("STALE_GPU_MUST_NOT_BE_CURRENT") == std::string::npos
            && output.find("UNAVAILABLE_GPU_MUST_NOT_BE_CURRENT") == std::string::npos,
        "detailed pass costs distinguish current, stale and missing evidence");
  const auto compact_geometry = state.geometry;
  inputs.pass_count = 1;
  check(!frame().geometry_changed && same_geometry(compact_geometry, state.geometry),
        "later pass-count changes do not auto-size established geometry");
  inputs.pass_count = 4;
  state.detailed = false;
  frame();
  check(same_geometry(compact_geometry, state.geometry)
            && output.find("Created model request") == std::string::npos,
        "mode changes replace content without auto-sizing established geometry");

  // Establish no foreground ImGui window, then move/click/type over the HUD.
  ImGui::SetWindowFocus(nullptr);
  io.AddMousePosEvent(window->Pos.x + 25.f, window->Pos.y + 25.f);
  frame();
  frame();
  io.AddMouseButtonEvent(0, true);
  io.AddKeyEvent(ImGuiKey_Tab, true);
  frame();
  check(!io.WantCaptureMouse && !io.WantCaptureKeyboard && !io.WantTextInput
            && context.ActiveId == 0 && context.NavWindow == nullptr,
        "passive HUD does not capture mouse, keyboard, text or navigation focus");
  io.AddMouseButtonEvent(0, false);
  io.AddKeyEvent(ImGuiKey_Tab, false);
  io.AddMousePosEvent(-100.f, -100.f);
  frame();

  interactive = true;
  state.detailed = false;
  frame();
  frame();
  check(window->HasCloseButton && (window->Flags & (ImGuiWindowFlags_NoInputs
            | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) == 0,
        "editing restores title close, mouse input, movement and resizing");
  check(output.find("Details") != std::string::npos && output.find("Lock") != std::string::npos,
        "editing exposes mode and pin settings");
  const auto click_tool = [&](unsigned index) {
    const char* labels[] = {state.pin ? "Unlock" : "Lock", state.detailed ? "Compact" : "Details",
                            "Diagnostics", "Close"};
    const char* ids[] = {"###monitor_pin", "###monitor_details", "###status_open_diagnostics", "###status_close"};
    ImVec2 target = window->DC.CursorStartPos;
    {
      ui::StyleScope theme;
      ui::PushPanelTheme(theme);
      float previous_width = 0.f;
      for (unsigned item = 0; item <= index; ++item) {
        const float width = ImGui::CalcTextSize(labels[item]).x + ImGui::GetStyle().FramePadding.x * 2.f;
        if (item != 0) {
          const float next = target.x + previous_width + ImGui::GetStyle().ItemSpacing.x;
          if (next + width <= window->WorkRect.Max.x) {
            target.x = next;
          } else {
            target.x = window->DC.CursorStartPos.x;
            target.y += ImGui::GetFontSize() + ImGui::GetStyle().ItemSpacing.y;
          }
        }
        previous_width = width;
      }
    }
    io.AddMousePosEvent(target.x + 5.f, target.y + 5.f);
    frame();
    check(context.HoveredId == window->GetID(ids[index]), "toolbar uses its stable ID for hit testing");
    io.AddMouseButtonEvent(0, true);
    frame();
    io.AddMouseButtonEvent(0, false);
    const auto actions = frame();
    io.AddMousePosEvent(-100.f, -100.f);
    frame();
    return actions;
  };
  const auto mode_actions = click_tool(1);
  check(state.detailed && mode_actions.settings_changed, "details tool reports a persistable settings change");
  check(same_geometry(compact_geometry, state.geometry), "interactive mode toggle preserves geometry");
  const auto pin_actions = click_tool(0);
  check(state.pin && pin_actions.settings_changed && pin_actions.geometry_changed
            && (window->Flags & (ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize))
                == (ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize),
        "pin tool reports settings changes and retains legacy geometry action");
  const auto unpin_actions = click_tool(0);
  check(!state.pin && unpin_actions.settings_changed
            && (window->Flags & (ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) == 0,
        "unpin restores movement and resizing");
  check(output.find("Eligible / injected / gaps") == std::string::npos
            && output.find("Execution proof") == std::string::npos,
        "editable detailed dashboard starts with raw evidence collapsed");
  window->StateStorage.SetInt(window->GetID("###monitor_raw_evidence"), 1);
  frame();
  check(output.find("Eligible / injected / gaps") != std::string::npos
            && output.find("Execution proof") != std::string::npos,
        "explicit raw disclosure preserves the original counters and six-column evidence");
  window->StateStorage.SetInt(window->GetID("###monitor_raw_evidence"), 0);
  ImGui::SetWindowSize(window->Name, ImVec2(320.f, 800.f));
  frame();
  check(click_tool(2).open_diagnostics, "narrow toolbar keeps the existing diagnostics action reachable");
  check(window->DC.CursorMaxPos.x <= window->InnerRect.Max.x + 1.f,
        "narrow toolbar and vertical nodes fit the available horizontal space");
  click_tool(3);
  check(!state.open, "narrow toolbar keeps the existing close action reachable");
  state.open = true;
  frame();
  const ImVec2 moved(1000.f, 90.f), resized(730.f, 580.f);
  ImGui::SetWindowPos(window->Name, moved);
  ImGui::SetWindowSize(window->Name, resized);
  check(frame().geometry_changed && state.geometry.position.x == moved.x
            && state.geometry.size.x == resized.x, "edited geometry is returned without overwriting it");
  interactive = false;
  const auto edited_geometry = state.geometry;
  ImGui::SetWindowCollapsed(window->Name, true);
  frame();
  check(!window->Collapsed && same_geometry(edited_geometry, state.geometry),
        "returning to gameplay recovers collapsed contents and preserves edited geometry");
  state.open = false;
  frame();
  check(!window->Active, "hidden HUD submits no window");
  state = {.open = true, .pin = true, .detailed = false};
  saved_geometry = &edited_geometry;
  frame();
  check(same_geometry(edited_geometry, state.geometry), "saved geometry wins over first-size mode and pass heuristics");
  ImGui::GetStyle().FontSizeBase = 26.f;
  io.DisplaySize = ImVec2(1600.f, 900.f);
  const auto expected = ui::ClampMonitorGeometry(edited_geometry, ImVec2(0.f, 0.f), io.DisplaySize, 2.f);
  check(frame().geometry_changed && same_geometry(expected, state.geometry),
        "saved geometry uses the existing DPI and screen-boundary clamp");

  state.open = false;
  frame();
  io.DisplaySize = original_display;
  io.ConfigFlags = original_config;
  ImGui::GetStyle().FontSizeBase = original_font;
  ui::SetTheme(original_theme);
  ui::SelectLanguage(original_language);
  check(ImGui::GetCurrentContext() == &context && !context.WithinFrameScope,
        "HUD cases return the caller's existing context between frames");
}

// A focused executable may compile this header as C++ with this define. The
// normal fixture simply includes the header and calls RunMonitorHudCases(check).
#ifdef NR_NEXUS_MONITOR_HUD_TEST_MAIN

int main() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  auto& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.DisplaySize = ImVec2(2400.f, 1400.f);
  io.DeltaTime = 1.f / 60.f;
  io.Fonts->AddFontDefault();
  unsigned char* pixels;
  int width, height;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
  unsigned checks = 0;
  unsigned failures = 0;
  const auto check = [&](bool result, const char* message) {
    ++checks;
    if (!result) {
      ++failures;
      std::printf("FAIL: %s\n", message);
    }
  };
  RunMonitorHudCases(check);
  std::printf("monitor HUD checks=%u failures=%u\n", checks, failures);
  ImGui::DestroyContext();
  return failures == 0 ? 0 : 1;
}
#endif
