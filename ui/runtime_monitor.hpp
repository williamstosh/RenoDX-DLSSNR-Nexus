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
      (std::min)(160.f * ui_scale, work_size.y), work_size.y);
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
  bool visible = true;
  if (inline_mode) {
    visible = ImGui::CollapsingHeader((std::string(Tr("Runtime monitor")) + "###nr_monitor_inline").c_str());
  } else {
    const bool initialize_geometry = !state->geometry.valid;
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
    state->geometry = ClampMonitorGeometry(previous, work_pos, work_size, ui_scale);
    const bool corrected = previous.position.x != state->geometry.position.x || previous.position.y != state->geometry.position.y
        || previous.size.x != state->geometry.size.x || previous.size.y != state->geometry.size.y
        || previous.ui_scale != state->geometry.ui_scale;
    actions.geometry_changed = initialize_geometry || corrected;
    // Only apply corrected geometry. Reapplying it each frame would defeat dragging.
    ImGui::SetNextWindowPos(state->geometry.position,
                            initialize_geometry || corrected ? ImGuiCond_Always : ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(state->geometry.size,
                             initialize_geometry || corrected ? ImGuiCond_Always : ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(
        ImVec2((std::min)(320.f * ui_scale, work_size.x), (std::min)(160.f * ui_scale, work_size.y)),
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
        metric_width("ReShade FPS", inputs.summary.fps), metric_width("FG", inputs.summary.fg)});
    const float metric_available = ImGui::GetContentRegionAvail().x;
    const int metric_columns = (metric_available >= widest_metric * 3.f ? 3
                                : (metric_available >= widest_metric * 2.f ? 2 : 1));
    if (ImGui::BeginTable("monitor_dashboard", metric_columns, ImGuiTableFlags_SizingStretchSame)) {
      const auto metric = [](const char* label, const std::string& value) {
        ImGui::TableNextColumn();
        ImGui::TextWrapped("%s  %s", Tr(label), value.empty() ? "—" : value.c_str());
      };
      metric("NR GPU", inputs.summary.gpu);
      metric("ReShade FPS", inputs.summary.fps);
      metric("FG", inputs.summary.fg);
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
    if (!inline_mode && state->detailed) {
      field("Dimensions", inputs.summary.resolution);
      field("Measurement scope", inputs.gpu_scope);
      field("Limitation", inputs.current_limit);
      if (interactive) {
        ImGui::TextWrapped("%s", Tr("A created model request does not prove the internal network actually used."));
      }
    }
    // Passive gameplay submits no disclosure controls; edit mode owns raw evidence.
    if (!inline_mode && state->detailed && interactive
        && ImGui::CollapsingHeader((std::string(Tr("Raw evidence")) + "###monitor_raw_evidence").c_str(),
                                   ImGuiTreeNodeFlags_NoAutoOpenOnLog)) {
      ImGui::SeparatorText(Tr("Recent evidence"));
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
    const ImVec2 pos = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    actions.geometry_changed |= pos.x != state->geometry.position.x || pos.y != state->geometry.position.y
        || size.x != state->geometry.size.x || size.y != state->geometry.size.y;
    state->geometry = {.position = pos, .size = size, .ui_scale = ui_scale, .valid = true,
                       .work_position = work_pos, .work_size = work_size};
    ImGui::End();
  }
  return actions;
}

// i18n: end

}  // namespace renodx::addons::dlss5::ui
