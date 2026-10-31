/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "widgets.hpp"
#include "recent_health.hpp"
#include "monitor_history.hpp"

namespace renodx::addons::dlss5::ui {

// i18n: begin - display strings (tools/i18n/ui_strings.py catalog)

enum class MonitorGpuState { UNAVAILABLE, CURRENT, STALE };

struct MonitorPass {
  std::string requested_stage;
  std::string effective_stage;
  std::string execution_proof;
  std::string requested_model;
  std::string created_model_request;
  std::string actual_model_proof;
  std::string requested_size;
  std::string actual_size;
  std::string gpu_cost;
  MonitorGpuState gpu_state = MonitorGpuState::UNAVAILABLE;
  bool execution_confirmed = false;
  std::string stage_reason;
};

struct MonitorRecentHealth {
  bool available = false;
  bool ready = false;
  bool partial = false;
  std::uint64_t eligible = 0;
  std::uint64_t injected = 0;
  std::uint64_t gaps = 0;
  std::int64_t last_sample_ns = 0;
};

struct MonitorInputs {
  StatusCard card;
  MonitorRecentHealth recent;
  std::array<MonitorPass, 4> passes;
  unsigned pass_count = 0;
  std::string current_conclusion;
  std::string gpu_scope;
  std::string current_limit;
  // Same monotonic nanosecond clock as recent.last_sample_ns; zero means unknown.
  std::int64_t now_ns = 0;
  StatusSummaryFields summary;
  const MonitorHistory* history = nullptr;
  std::string gpu_notice;
};

// Physical screen coordinates with the UI scale used when they were saved.
// The caller may copy this value to its existing settings persistence layer.
struct MonitorGeometry {
  ImVec2 position = ImVec2(0.f, 0.f);
  ImVec2 size = ImVec2(0.f, 0.f);
  float ui_scale = 1.f;
  bool valid = false;
  ImVec2 work_position = ImVec2(0.f, 0.f);
  ImVec2 work_size = ImVec2(0.f, 0.f);
};

struct MonitorState {
  bool open = false;
  bool pin = false;  // Locks movement/resizing while editing.
  MonitorGeometry geometry;
  bool detailed = false;  // The caller persists this together with pin and geometry.
  float background_opacity = .8f;
  bool trends = false;
  int history_seconds = 30;
  uint32_t trend_mask = 0x7f;
  bool apply_geometry = false;
  bool fit_content = false;
  int anchor = -1;
  ImVec2 offset{};
};

struct MonitorActions {
  bool open_diagnostics = false;
  bool geometry_changed = false;
  bool settings_changed = false;
};

inline MonitorGeometry ClampMonitorGeometry(MonitorGeometry geometry, ImVec2 work_pos, ImVec2 work_size,
                                            float ui_scale) {
  ui_scale = (std::isfinite(ui_scale) && ui_scale > 0.f ? ui_scale : 1.f);
  const auto finite = [](float value, float fallback) { return (std::isfinite(value) ? value : fallback); };
  work_size.x = (std::max)(1.f, finite(work_size.x, 1.f));
  work_size.y = (std::max)(1.f, finite(work_size.y, 1.f));
  const float previous_scale = (std::isfinite(geometry.ui_scale) && geometry.ui_scale > 0.f ? geometry.ui_scale : ui_scale);
  const float ratio = ui_scale / previous_scale;
  work_pos.x = finite(work_pos.x, 0.f);
  work_pos.y = finite(work_pos.y, 0.f);
  if (!geometry.valid) {
    geometry.position = work_pos;
    geometry.size = ImVec2(820.f * previous_scale, 600.f * previous_scale);
  } else if (geometry.work_size.x > 0.f && geometry.work_size.y > 0.f) {
    geometry.position.x = work_pos.x + (geometry.position.x - geometry.work_position.x) * ratio;
    geometry.position.y = work_pos.y + (geometry.position.y - geometry.work_position.y) * ratio;
  }
  geometry.size.x = std::clamp(finite(geometry.size.x * ratio, 520.f * ui_scale),
      (std::min)(320.f * ui_scale, work_size.x), work_size.x);
  geometry.size.y = std::clamp(finite(geometry.size.y * ratio, 640.f * ui_scale),
      (std::min)(120.f * ui_scale, work_size.y), work_size.y);
  geometry.position.x = std::clamp(finite(geometry.position.x, work_pos.x), work_pos.x,
                                 work_pos.x + work_size.x - geometry.size.x);
  geometry.position.y = std::clamp(finite(geometry.position.y, work_pos.y), work_pos.y,
                                 work_pos.y + work_size.y - geometry.size.y);
  geometry.ui_scale = ui_scale;
  geometry.valid = true;
  geometry.work_position = work_pos;
  geometry.work_size = work_size;
  return geometry;
}

// Anchors run left-to-right, top-to-bottom. Offsets are unscaled UI pixels.
inline MonitorGeometry PositionMonitorGeometry(MonitorGeometry geometry, int anchor, ImVec2 offset,
                                               ImVec2 work_pos, ImVec2 work_size, float scale) {
  geometry = ClampMonitorGeometry(geometry, work_pos, work_size, scale);
  if (anchor < 0 || anchor > 8) return geometry;
  const float x = static_cast<float>(anchor % 3) * .5f;
  const float y = static_cast<float>(anchor / 3) * .5f;
  geometry.position = ImVec2(geometry.work_position.x + (geometry.work_size.x - geometry.size.x) * x
                                + (std::isfinite(offset.x) ? offset.x * geometry.ui_scale : 0.f),
                            geometry.work_position.y + (geometry.work_size.y - geometry.size.y) * y
                                + (std::isfinite(offset.y) ? offset.y * geometry.ui_scale : 0.f));
  return ClampMonitorGeometry(geometry, geometry.work_position, geometry.work_size, geometry.ui_scale);
}

inline bool CanConnectMonitorPoints(const MonitorPoint& previous, const MonitorPoint& current, MonitorMetric metric) {
  const int64_t max_gap = (metric == MonitorMetric::FPS || metric == MonitorMetric::INTERVAL
      ? 200'000'000LL : 750'000'000LL);
  return previous.valid && current.valid && previous.reference == current.reference
      && current.time_ns > previous.time_ns && current.time_ns - previous.time_ns <= max_gap;
}

// Legacy callers draw inside a panel and may use a narrow inline disclosure.
// Independent callers need no parent window. Passive drawing never accepts input;
// the caller owns visibility and supplies interactive=true when Home opens editing.
inline MonitorActions DrawRuntimeMonitor(const MonitorInputs& inputs, MonitorState* state,
                                        const MonitorGeometry* saved_geometry = nullptr,
                                        bool interactive = true, bool independent = false) {
  MonitorActions actions;
  if (state == nullptr || !state->open) return actions;
  const bool standalone = independent || !interactive;
  const ImVec2 menu_pos = (interactive ? ImGui::GetWindowPos() : ImVec2(0.f, 0.f));
  const ImVec2 menu_size = (interactive ? ImGui::GetWindowSize() : ImVec2(0.f, 0.f));
  // The pinned ReShade function table exposes neither viewport getter.
  // Its overlay coordinates cover the single main display surface.
  const ImVec2 work_pos(0.f, 0.f);
  const ImVec2 work_size = ImGui::GetIO().DisplaySize;
  const float ui_scale = BodyFontSize() / 13.f;
  const float gap = ImGui::GetFontSize();
  const float right_space = work_pos.x + work_size.x - menu_pos.x - menu_size.x - gap;
  const float left_space = menu_pos.x - work_pos.x - gap;
  const bool inline_mode = !standalone && ((std::max)(right_space, left_space) < 320.f * ui_scale
      || ImGui::GetContentRegionAvail().x < ImGui::GetFontSize() * 25.f);
  StyleScope theme;
  PushPanelTheme(theme);
  theme.Color(ImGuiCol_TitleBg, CurrentTheme().bg_control)
      .Color(ImGuiCol_TitleBgActive, CurrentTheme().bg_control_hover)
      .Color(ImGuiCol_TitleBgCollapsed, CurrentTheme().bg_control);
  const float opacity = (std::isfinite(state->background_opacity)
      ? std::clamp(state->background_opacity, 0.f, 1.f) : .8f);
  for (ImGuiCol color : {ImGuiCol_WindowBg, ImGuiCol_ChildBg, ImGuiCol_Border, ImGuiCol_BorderShadow,
                         ImGuiCol_TitleBg, ImGuiCol_TitleBgActive, ImGuiCol_TitleBgCollapsed,
                         ImGuiCol_TableHeaderBg, ImGuiCol_TableBorderStrong, ImGuiCol_TableBorderLight,
                         ImGuiCol_TableRowBg, ImGuiCol_TableRowBgAlt}) {
    ImVec4 background = ImGui::GetStyle().Colors[color];
    if (color == ImGuiCol_WindowBg || color == ImGuiCol_ChildBg || color == ImGuiCol_TitleBg
        || color == ImGuiCol_TitleBgActive || color == ImGuiCol_TitleBgCollapsed) {
      background.w = opacity;
    } else {
      background.w *= opacity;
    }
    theme.Color(color, background);
  }
  bool visible = true;
  bool geometry_applied = false;
  bool fit_content = state->fit_content;
  if (inline_mode) {
    visible = ImGui::CollapsingHeader((std::string(Tr("Runtime monitor")) + "###nr_monitor_inline").c_str());
  } else {
    const bool initialize_geometry = !state->geometry.valid;
    fit_content |= initialize_geometry && (saved_geometry == nullptr || !saved_geometry->valid);
    if (!state->geometry.valid) {
      if (saved_geometry != nullptr && saved_geometry->valid) {
        state->geometry = *saved_geometry;
      } else if (standalone) {
        const float count = static_cast<float>(std::clamp(inputs.pass_count, 1u, 4u));
        float adjustment_height = 0.f;
        for (unsigned i = 0; i < (std::min)(inputs.pass_count, 4u); ++i) {
          const auto& pass = inputs.passes[i];
          if (!pass.requested_stage.empty() && !pass.effective_stage.empty()
              && pass.requested_stage != pass.effective_stage) {
            adjustment_height += 60.f;
          } else if (!pass.stage_reason.empty()) {
            adjustment_height += 22.f;
          }
          if (!pass.requested_model.empty() && !pass.created_model_request.empty()
              && pass.requested_model != pass.created_model_request) {
            adjustment_height += 44.f;
          }
        }
        const bool side = interactive && (std::max)(right_space, left_space) >= 320.f * ui_scale;
        const bool right = right_space >= left_space;
        state->geometry = {.position = side ? ImVec2(right ? menu_pos.x + menu_size.x + gap : work_pos.x, menu_pos.y)
                                           : ImVec2(work_pos.x + gap, work_pos.y + gap),
                           .size = ImVec2((std::min)((state->detailed ? 620.f : 480.f) * ui_scale,
                                                     side ? (right ? right_space : left_space) : work_size.x),
                                         ((state->detailed ? 340.f + 156.f * count : 132.f + 62.f * count)
                                             + adjustment_height) * ui_scale),
                           .ui_scale = ui_scale, .valid = true};
      } else {
        const bool right = right_space >= left_space;
        const float side_width = (right ? right_space : left_space);
        state->geometry = {.position = ImVec2(right ? menu_pos.x + menu_size.x + gap
                                                   : work_pos.x, menu_pos.y),
                           .size = ImVec2((std::min)(820.f * ui_scale, side_width), 600.f * ui_scale),
                           .ui_scale = ui_scale, .valid = true};
      }
    }
    const auto previous = state->geometry;
    const bool area_changed = previous.work_position.x != work_pos.x || previous.work_position.y != work_pos.y
        || previous.work_size.x != work_size.x || previous.work_size.y != work_size.y
        || previous.ui_scale != ui_scale;
    state->geometry = (state->anchor >= 0 && (initialize_geometry || state->apply_geometry || area_changed)
        ? PositionMonitorGeometry(previous, state->anchor, state->offset, work_pos, work_size, ui_scale)
        : ClampMonitorGeometry(previous, work_pos, work_size, ui_scale));
    const bool corrected = previous.position.x != state->geometry.position.x || previous.position.y != state->geometry.position.y
        || previous.size.x != state->geometry.size.x || previous.size.y != state->geometry.size.y
        || previous.ui_scale != state->geometry.ui_scale;
    geometry_applied = initialize_geometry || corrected || state->apply_geometry;
    actions.geometry_changed = geometry_applied;
    // Only apply corrected geometry. Reapplying it each frame would defeat dragging.
    ImGui::SetNextWindowPos(state->geometry.position,
                            geometry_applied ? ImGuiCond_Always : ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(state->geometry.size,
                             geometry_applied ? ImGuiCond_Always : ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(
        ImVec2((std::min)(320.f * ui_scale, work_size.x), (std::min)(120.f * ui_scale, work_size.y)),
        work_size);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings;
    if (standalone) {
      flags |= ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoCollapse;
      // A previous editable/legacy window may have been collapsed. Gameplay
      // has no title control with which to recover its contents.
      ImGui::SetNextWindowCollapsed(false, ImGuiCond_Always);
    }
    if (!interactive) {
      flags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse
          | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBringToFrontOnFocus;
    }
    if (state->pin) {
      flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
    }
    visible = ImGui::Begin((std::string(Tr("Runtime monitor")) + "###nr_runtime_monitor").c_str(),
                           interactive ? &state->open : nullptr, flags);
  }
  if (visible) {
    if (interactive) {
      bool first_tool = true;
      const auto tool = [&](const char* label, const char* id, const char* tip) {
        const float width = ImGui::CalcTextSize(Tr(label)).x + ImGui::GetStyle().FramePadding.x * 2.f;
        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        if (!first_tool && ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width <= right) {
          ImGui::SameLine();
        }
        first_tool = false;
        const bool pressed = ImGui::SmallButton((std::string(Tr(label)) + id).c_str());
        ItemTooltip("%s", Tr(tip));
        return pressed;
      };
      if (!inline_mode) {
        const bool pin_changed = tool(state->pin ? "Unlock" : "Lock", "###monitor_pin", "Lock position and size");
        if (pin_changed) {
          state->pin = !state->pin;
        }
        actions.geometry_changed |= pin_changed;
        actions.settings_changed |= pin_changed;
        if (tool(state->detailed ? "Compact" : "Details", "###monitor_details", "Detailed monitor")) {
          state->detailed = !state->detailed;
          actions.settings_changed = true;
        }
      }
      actions.open_diagnostics = tool("Diagnostics", "###status_open_diagnostics", "Open Diagnostics");
      if (tool("Close", "###status_close", "Close monitor")) {
        state->open = false;
      }
      if (!inline_mode) {
        if (tool("Fit to content", "###monitor_fit", "Fit height to current content.")) {
          fit_content = true;
        }
        if (tool("Trends", "###monitor_trends", "Show performance history")) {
          state->trends = !state->trends;
          actions.settings_changed = true;
        }
      }
    }
    const auto row = [](const char* label, const std::string& value) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextWrapped("%s", Tr(label));
      ImGui::TableNextColumn();
      ImGui::TextWrapped("%s", value.empty() ? Tr("Pending confirmation") : value.c_str());
    };
    ImGui::PushStyleColor(ImGuiCol_Text, ToneColor(inputs.card.tone));
    const char* conclusion = (inputs.current_conclusion.empty() ? Tr(inputs.card.title) : inputs.current_conclusion.c_str());
    if (inputs.current_conclusion.empty()) {
      switch (inputs.card.id) {
        case CardId::kOffByUser: conclusion = Tr("NR disabled"); break;
        case CardId::kActive: conclusion = Tr(inputs.card.fg_submitted_frames.has_value()
            ? "NR submitted before FG" : "NR running normally"); break;
        case CardId::kStarting: conclusion = Tr(inputs.card.tone == CardTone::kWarn ? "Needs attention" : "Checking NR"); break;
        default: break;
      }
    }
    ImGui::TextWrapped("%s", conclusion);
    ImGui::PopStyleColor();
    const float font = ImGui::GetFontSize();
    const auto metric_width = [](const char* label, const std::string& value) {
      return ImGui::CalcTextSize(Tr(label)).x + ImGui::CalcTextSize(value.empty() ? "—" : value.c_str()).x
          + ImGui::GetStyle().ItemSpacing.x * 3.f;
    };
    const float widest_metric = (std::max)({metric_width("NR GPU", inputs.summary.gpu),
        metric_width("FPS", inputs.summary.fps), metric_width("FG", inputs.summary.fg)});
    const float metric_available = ImGui::GetContentRegionAvail().x;
    const int metric_columns = (metric_available >= widest_metric * 3.f ? 3
                                : (metric_available >= widest_metric * 2.f ? 2 : 1));
    if (ImGui::BeginTable("monitor_dashboard", metric_columns, ImGuiTableFlags_SizingStretchSame)) {
      const auto metric = [](const char* label, const std::string& value, const char* tip) {
        ImGui::TableNextColumn();
        ImGui::TextWrapped("%s  %s", Tr(label), value.empty() ? "—" : value.c_str());
        ItemTooltip("%s", Tr(tip));
      };
      metric("NR GPU", (inputs.summary.gpu_reference && !inputs.summary.gpu.empty() ? "~ " : "")
          + inputs.summary.gpu, inputs.summary.gpu_tip);
      metric("FPS", inputs.summary.fps, inputs.summary.fps_tip);
      metric("FG", inputs.summary.fg, inputs.summary.fg_tip);
      ImGui::EndTable();
    }
    const auto field = [](const char* label, const std::string& value) {
      ImGui::TextWrapped("%s: %s", Tr(label), value.empty() ? Tr("Pending confirmation") : value.c_str());
    };
    if (!inline_mode) {
      ImGui::SeparatorText(Tr("Execution chain"));
      const unsigned count = (std::min)(inputs.pass_count, 4u);
      if (count == 0) {
        ImGui::TextWrapped("%s", Tr("Pending confirmation"));
      }
      float pass_width = font * 16.f;
      for (unsigned i = 0; i < count; ++i) {
        const auto& pass = inputs.passes[i];
        pass_width = (std::max)(pass_width, ImGui::CalcTextSize((pass.requested_model + "  |  " + pass.requested_size).c_str()).x
            + font * 2.f);
      }
      const float pass_available = ImGui::GetContentRegionAvail().x;
      const int pass_columns = (count >= 3 && pass_available >= pass_width * 3.f ? 3
                                : (count >= 2 && pass_available >= pass_width * 2.f ? 2 : 1));
      const bool pass_table = ImGui::BeginTable("monitor_passes", pass_columns, ImGuiTableFlags_SizingStretchSame);
      ImVec2 previous_node;
      for (unsigned i = 0; i < count; ++i) {
        if (pass_table) ImGui::TableNextColumn();
        const auto& pass = inputs.passes[i];
        ImGui::PushID(static_cast<int>(i));
        const ImVec2 node(ImGui::GetCursorScreenPos().x + font * 0.4f,
                          ImGui::GetCursorScreenPos().y + font * 0.5f);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        if (i != 0 && pass_columns == 1) {
          draw->AddLine(previous_node, node, ImGui::GetColorU32(CurrentTheme().node_border), 2.f);
        }
        draw->AddCircleFilled(node, font * 0.25f,
                              ImGui::GetColorU32(pass.execution_confirmed ? CurrentTheme().good : CurrentTheme().idle));
        previous_node = node;
        ImGui::Indent(font * 1.3f);
        ImGui::TextColored(CurrentTheme().accent, "P%u  %s", i + 1,
            state->detailed ? "" : pass.effective_stage.c_str());
        const bool stage_adjusted = !pass.requested_stage.empty() && !pass.effective_stage.empty()
            && pass.requested_stage != pass.effective_stage;
        if (state->detailed) {
          field("Stage", pass.effective_stage);
          ImGui::TextColored(pass.execution_confirmed ? CurrentTheme().good : CurrentTheme().idle, "%s",
                             Tr(pass.execution_confirmed ? "Execution confirmed" : "Pending confirmation"));
        }
        if (stage_adjusted) {
          ImGui::TextColored(CurrentTheme().warn, "%s", Tr("Stage adjusted"));
          field("Requested stage", pass.requested_stage);
          ImGui::TextWrapped("%s", pass.stage_reason.empty()
              ? Tr("Requested stage differs from effective stage.") : pass.stage_reason.c_str());
        } else if (!pass.stage_reason.empty()) {
          ImGui::TextWrapped("%s", pass.stage_reason.c_str());
        }
        if (state->detailed) {
          field("Configured model", pass.requested_model);
        } else {
          ImGui::TextWrapped("%s  |  %s", pass.requested_model.c_str(), pass.requested_size.c_str());
          ItemTooltip("%s / %s", Tr("Configured model"), Tr("Working ratio"));
        }
        if (!pass.requested_model.empty() && !pass.created_model_request.empty()
            && pass.requested_model != pass.created_model_request) {
          ImGui::TextColored(CurrentTheme().warn, "%s", Tr("Model request adjusted"));
          field("Created model request", pass.created_model_request);
        }
        if (state->detailed) {
          field("Working ratio", pass.requested_size);
          field("Observed working size", pass.actual_size);
        } else {
          const std::string completion = std::string(Tr(pass.execution_confirmed
              ? "Execution confirmed" : "Pending confirmation"))
              + (pass.execution_confirmed && !pass.actual_size.empty() ? "  " + pass.actual_size : "");
          ImGui::TextColored(pass.execution_confirmed ? CurrentTheme().good : CurrentTheme().idle, "%s", completion.c_str());
          if (pass.gpu_state == MonitorGpuState::CURRENT && !pass.gpu_cost.empty()) {
            ImGui::TextWrapped("%s: %s", Tr("GPU cost"), pass.gpu_cost.c_str());
          }
        }
        if (state->detailed) {
          field("GPU cost", pass.gpu_state == MonitorGpuState::CURRENT ? pass.gpu_cost
              : Tr(pass.gpu_state == MonitorGpuState::STALE ? "Stale GPU sample" : "Pending confirmation"));
          if (interactive && ImGui::TreeNodeEx((std::string(Tr("Pass evidence")) + "###monitor_pass_evidence").c_str(),
                                               ImGuiTreeNodeFlags_NoAutoOpenOnLog)) {
            field("Execution proof", pass.execution_proof);
            field("Created model request", pass.created_model_request);
            field("Model evidence", pass.actual_model_proof);
            ImGui::TreePop();
          }
        }
        ImGui::Unindent(font * 1.3f);
        ImGui::Spacing();
        ImGui::PopID();
      }
      if (pass_table) ImGui::EndTable();
    } else {
      field("Stage", inputs.summary.stage);
    }
    if (!inline_mode && state->trends) {
      ImGui::SeparatorText(Tr("Trends"));
      const int seconds = (state->history_seconds == 10 || state->history_seconds == 60 ? state->history_seconds : 30);
      if (interactive) {
        const char* durations[] = {"10 s", "30 s", "60 s"};
        const int durations_seconds[] = {10, 30, 60};
        for (int index = 0; index < 3; ++index) {
          if (index != 0) {
            SameLineIfFits(ImGui::CalcTextSize(Tr(durations[index])).x + ImGui::GetFrameHeight()
                + ImGui::GetStyle().ItemInnerSpacing.x);
          }
          if (ImGui::RadioButton(Tr(durations[index]), seconds == durations_seconds[index])) {
            state->history_seconds = durations_seconds[index];
            actions.settings_changed = true;
          }
        }
      }
      const char* names[] = {"FPS", "Update interval (ms)", "NR GPU", "Pass 1 GPU", "Pass 2 GPU", "Pass 3 GPU", "Pass 4 GPU"};
      const unsigned metric_count = 3u + (std::min)(inputs.pass_count, 4u);
      const ImVec4 colors[] = {CurrentTheme().accent, CurrentTheme().good, CurrentTheme().accent,
          CurrentTheme().good, CurrentTheme().warn, CurrentTheme().text_primary, CurrentTheme().text_secondary};
      if (interactive) {
        for (unsigned metric = 0; metric < metric_count; ++metric) {
          if (metric != 0) SameLineIfFits(ImGui::CalcTextSize(Tr(names[metric])).x + ImGui::GetFrameHeight()
              + ImGui::GetStyle().ItemInnerSpacing.x);
          bool selected = (state->trend_mask & (1u << metric)) != 0;
          StyleScope legend;
          legend.Color(ImGuiCol_Text, colors[metric]);
          if (Checkbox(Tr(names[metric]), &selected)) {
            state->trend_mask ^= 1u << metric;
            actions.settings_changed = true;
          }
        }
      }
      const int64_t duration_ns = static_cast<int64_t>(seconds) * 1'000'000'000LL;
      const int64_t begin_ns = inputs.now_ns - duration_ns;
      const auto in_window = [&](const MonitorPoint& point) {
        return point.valid && point.time_ns >= begin_ns && point.time_ns <= inputs.now_ns
            && std::isfinite(point.value) && std::isfinite(point.low) && std::isfinite(point.high);
      };
      const auto plot = [&](unsigned first, unsigned end, const char* unit) {
        const uint32_t mask = ((1u << end) - (1u << first)) & state->trend_mask;
        if (mask == 0) return;
        float maximum = 0.f;
        bool available = false;
        bool first_label = true;
        for (unsigned metric = first; metric < end; ++metric) {
          if ((mask & (1u << metric)) == 0) continue;
          bool reference = false;
          if (inputs.history != nullptr) {
            const auto& series = inputs.history->series[metric];
            for (size_t index = 0; index < series.count; ++index) {
              const auto& point = series.At(index);
              if (!in_window(point)) continue;
              maximum = (std::max)(maximum, point.high);
              available = true;
              reference |= point.reference;
            }
          }
          if (!first_label) {
            SameLineIfFits(ImGui::CalcTextSize(Tr(names[metric])).x + font * 2.f);
          }
          ImGui::TextColored(colors[metric], "%s%s", reference ? "~ " : "", Tr(names[metric]));
          first_label = false;
        }
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 size((std::max)(1.f, ImGui::GetContentRegionAvail().x), font * 6.f);
        ImGui::Dummy(size);
        ItemTooltip("%s", Tr("FPS is smoothed UI cadence; interval is the actual ImGui update interval. GPU points are completed events. Bucket ranges preserve peaks; gaps stay empty."));
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 edge(origin.x + size.x, origin.y + size.y);
        draw->AddRectFilled(origin, edge, ImGui::GetColorU32(ImGuiCol_ChildBg), 3.f);
        draw->AddRect(origin, edge, ImGui::GetColorU32(ImGuiCol_Border), 3.f);
        const float top = origin.y + font * 1.5f;
        const float bottom = edge.y - font * 1.4f;
        const float left = origin.x + 4.f;
        const float right = edge.x - 4.f;
        maximum = (std::max)(maximum * 1.05f, .001f);
        char range[96];
        std::snprintf(range, sizeof(range), "0–%.1f %s", maximum, unit);
        draw->AddText(ImVec2(left, origin.y + 2.f), ImGui::GetColorU32(ImGuiCol_Text), available ? range : "—");
        char span[32];
        std::snprintf(span, sizeof(span), "-%d s", seconds);
        draw->AddText(ImVec2(left, bottom + 3.f), ImGui::GetColorU32(ImGuiCol_Text), span);
        draw->AddText(ImVec2(right - ImGui::CalcTextSize("0 s").x, bottom + 3.f), ImGui::GetColorU32(ImGuiCol_Text), "0 s");
        if (inputs.history == nullptr || !available) return;
        const auto position = [&](int64_t time, float value) {
          return ImVec2(left + static_cast<float>(static_cast<double>(time - begin_ns) / duration_ns) * (right - left),
                        bottom - value / maximum * (bottom - top));
        };
        draw->PushClipRect(ImVec2(left, top), ImVec2(right + 1.f, bottom + 1.f), true);
        const ImDrawListFlags draw_flags = draw->Flags;
        draw->Flags &= ~ImDrawListFlags_AntiAliasedLines;
        // Bound the mesh for long windows with all seven curves. Display columns
        // retain raw extrema and split at every gap or reference transition.
        const float column_width = (std::max)(1.f, (right - left) / 256.f);
        for (unsigned metric = first; metric < end; ++metric) {
          if ((mask & (1u << metric)) == 0) continue;
          const auto& series = inputs.history->series[metric];
          const ImU32 color = ImGui::GetColorU32(colors[metric]);
          const MonitorPoint* previous = nullptr;
          MonitorPoint column;
          int column_index = -1;
          bool pending = false, connect = false, have_previous = false;
          ImVec2 previous_at;
          MonitorPoint previous_column;
          const auto flush = [&] {
            if (!pending) return;
            const ImVec2 at = position(column.time_ns, column.value);
            if (column.low != column.high) {
              draw->AddLine(position(column.time_ns, column.low), position(column.time_ns, column.high), color, 1.f);
            }
            draw->AddRectFilled(ImVec2(at.x - 1.f, at.y - 1.f), ImVec2(at.x + 1.f, at.y + 1.f), color);
            if (connect && have_previous
                && CanConnectMonitorPoints(previous_column, column, static_cast<MonitorMetric>(metric))) {
              draw->AddLine(previous_at, at, color, 1.f);
            }
            previous_at = at;
            previous_column = column;
            have_previous = true;
            pending = false;
          };
          for (size_t index = 0; index < series.count; ++index) {
            const auto& point = series.At(index);
            if (!in_window(point)) {
              flush();
              previous = nullptr;
              have_previous = false;
              continue;
            }
            const bool continuous = previous != nullptr
                && CanConnectMonitorPoints(*previous, point, static_cast<MonitorMetric>(metric));
            const int next_column = static_cast<int>((position(point.time_ns, point.value).x - left) / column_width);
            if (pending && continuous && column_index == next_column) {
              column.time_ns = point.time_ns;
              column.value = point.value;
              column.low = (std::min)(column.low, point.low);
              column.high = (std::max)(column.high, point.high);
            } else {
              flush();
              column = point;
              column_index = next_column;
              pending = true;
              connect = continuous;
            }
            previous = &point;
          }
          flush();
        }
        draw->Flags = draw_flags;
        draw->PopClipRect();
      };
      plot(0, 1, "FPS");
      plot(1, 2, "ms");
      plot(2, metric_count, "ms");
    }
    if (!inline_mode) {
      if (!inputs.current_limit.empty()) {
        ImGui::TextWrapped("%s", inputs.current_limit.c_str());
      }
      if (!inputs.gpu_notice.empty()) {
        StyleScope muted;
        muted.Color(ImGuiCol_Text, CurrentTheme().text_secondary);
        ImGui::TextWrapped("%s", inputs.gpu_notice.c_str());
      }
    }
    // Passive gameplay submits no disclosure controls; edit mode owns raw evidence.
    if (!inline_mode && state->detailed && interactive
        && ImGui::CollapsingHeader((std::string(Tr("Raw evidence")) + "###monitor_raw_evidence").c_str(),
                                   ImGuiTreeNodeFlags_NoAutoOpenOnLog)) {
      ImGui::SeparatorText(Tr("Recent evidence"));
      field("Dimensions", inputs.summary.resolution);
      ImGui::TextWrapped("%s", Tr("A created model request does not prove the internal network actually used."));
      if (ImGui::BeginTable("conclusion", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.38f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.62f);
        const bool has_age = inputs.recent.available && inputs.recent.last_sample_ns > 0
            && inputs.now_ns >= inputs.recent.last_sample_ns;
        const bool stale = has_age && inputs.now_ns - inputs.recent.last_sample_ns > RecentHealthWindow::kStaleNs;
        row("Recent evidence", Tr(stale ? "Stale recent sample"
            : (!inputs.recent.available || !inputs.recent.ready ? "Pending confirmation"
               : (inputs.recent.partial ? "Partial window" : "Ready"))));
        if (inputs.recent.available) {
          row("Eligible / injected / gaps", std::to_string(inputs.recent.eligible) + " / "
              + std::to_string(inputs.recent.injected) + " / " + std::to_string(inputs.recent.gaps));
        }
        if (has_age) {
          char age[64];
          std::snprintf(age, sizeof(age), Tr("%.1f s ago"),
                        static_cast<double>(inputs.now_ns - inputs.recent.last_sample_ns) / 1'000'000'000.);
          row("Recent sample age", age);
        } else {
          row("Recent sample age", "");
        }
        ImGui::EndTable();
      }
      const unsigned count = (std::min)(inputs.pass_count, 4u);
      const MonitorPass empty_pass;
      // Wrapped headers keep adjacent columns readable when the window is resized.
      const auto begin_pass_table = [](const char* id, const char* title,
                                       const char* const* headers, int columns) {
        if (title[0] != '\0') ImGui::SeparatorText(Tr(title));
        if (!ImGui::BeginTable(id, columns, ImGuiTableFlags_SizingStretchProp
            | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) return false;
        for (int column = 0; column < columns; ++column) {
          ImGui::TableSetupColumn(headers[column], ImGuiTableColumnFlags_WidthStretch,
                                  column == 0 ? 0.45f : 1.f);
        }
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        for (int column = 0; column < columns; ++column) {
          ImGui::TableNextColumn();
          ImGui::TextWrapped("%s", Tr(headers[column]));
        }
        return true;
      };
      const auto cell = [](const std::string& value) {
        ImGui::TableNextColumn();
        ImGui::TextWrapped("%s", value.empty() ? Tr("Pending confirmation") : value.c_str());
      };
      const char* chain_headers[] = {"Pass", "Requested stage", "Effective stage",
                                    "Requested model", "Created model request", "Execution proof"};
      if (begin_pass_table("chain", "Execution chain", chain_headers, 6)) {
        for (unsigned i = 0; i < (std::max)(count, 1u); ++i) {
          const auto& pass = (count == 0 ? empty_pass : inputs.passes[i]);
          ImGui::TableNextRow();
          cell(count == 0 ? "-" : std::to_string(i + 1));
          cell(pass.requested_stage);
          cell(pass.effective_stage);
          cell(pass.requested_model);
          cell(pass.created_model_request);
          cell(pass.execution_proof);
        }
        ImGui::EndTable();
      }
      const char* size_headers[] = {"Pass", "Requested working size", "Observed working size"};
      if (begin_pass_table("dimensions", "Dimensions", size_headers, 3)) {
        for (unsigned i = 0; i < (std::max)(count, 1u); ++i) {
          const auto& pass = (count == 0 ? empty_pass : inputs.passes[i]);
          ImGui::TableNextRow();
          cell(count == 0 ? "-" : std::to_string(i + 1));
          cell(pass.requested_size);
          cell(pass.actual_size);
        }
        ImGui::EndTable();
      }
      ImGui::SeparatorText(Tr("GPU"));
      ImGui::TextWrapped("%s: %s", Tr("Measurement scope"),
                         inputs.gpu_scope.empty() ? Tr("Pending confirmation") : inputs.gpu_scope.c_str());
      const char* gpu_headers[] = {"Pass", "Status", "GPU cost"};
      // GPU already has its section heading and scope above the table.
      if (begin_pass_table("gpu", "", gpu_headers, 3)) {
        for (unsigned i = 0; i < (std::max)(count, 1u); ++i) {
          const auto& pass = (count == 0 ? empty_pass : inputs.passes[i]);
          ImGui::TableNextRow();
          cell(count == 0 ? "-" : std::to_string(i + 1));
          switch (pass.gpu_state) {
            case MonitorGpuState::CURRENT: cell(Tr("Current")); break;
            case MonitorGpuState::STALE: cell(Tr("Stale GPU sample")); break;
            case MonitorGpuState::UNAVAILABLE: cell(Tr("Pending confirmation")); break;
          }
          cell(pass.gpu_state == MonitorGpuState::UNAVAILABLE ? "" : pass.gpu_cost);
        }
        ImGui::EndTable();
      }
    }
  }
  if (!inline_mode) {
    if (fit_content && visible) {
      auto geometry = state->geometry;
      geometry.position = ImGui::GetWindowPos();
      geometry.size = ImVec2(ImGui::GetWindowSize().x, ImGui::GetCursorPosY()
          + ImGui::GetStyle().WindowPadding.y - ImGui::GetStyle().ItemSpacing.y);
      geometry = PositionMonitorGeometry(geometry, state->anchor, state->offset, work_pos, work_size, ui_scale);
      ImGui::SetWindowSize(geometry.size, ImGuiCond_Always);
      ImGui::SetWindowPos(geometry.position, ImGuiCond_Always);
      geometry_applied = true;
      actions.geometry_changed = true;
      state->fit_content = false;
    }
    const ImVec2 pos = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    const bool geometry_changed = std::abs(pos.x - state->geometry.position.x) > .5f
        || std::abs(pos.y - state->geometry.position.y) > .5f
        || std::abs(size.x - state->geometry.size.x) > .5f || std::abs(size.y - state->geometry.size.y) > .5f;
    if (geometry_changed && interactive && !state->pin && !geometry_applied) {
      state->anchor = -1;
      actions.settings_changed = true;
    }
    actions.geometry_changed |= geometry_changed;
    state->geometry = {.position = pos, .size = size, .ui_scale = ui_scale, .valid = true,
                       .work_position = work_pos, .work_size = work_size};
    state->apply_geometry = false;
    ImGui::End();
  }
  return actions;
}

// i18n: end

}  // namespace renodx::addons::dlss5::ui
