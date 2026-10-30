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
  bool pin = false;  // Locks movement/resizing; does not outlive the overlay callback.
  MonitorGeometry geometry;
};

struct MonitorActions {
  bool open_diagnostics = false;
  bool geometry_changed = false;
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
      (std::min)(240.f * ui_scale, work_size.y), work_size.y);
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

// Call inside the panel on every frame. Narrow panels use an inline disclosure;
// wide panels use an independent Begin window. The caller owns reopening via state.open.
inline MonitorActions DrawRuntimeMonitor(const MonitorInputs& inputs, MonitorState* state,
                                        const MonitorGeometry* saved_geometry = nullptr) {
  MonitorActions actions;
  if (state == nullptr || !state->open) return actions;
  const ImVec2 menu_pos = ImGui::GetWindowPos();
  const ImVec2 menu_size = ImGui::GetWindowSize();
  // The pinned ReShade function table exposes neither viewport getter.
  // Its overlay coordinates cover the single main display surface.
  const ImVec2 work_pos(0.f, 0.f);
  const ImVec2 work_size = ImGui::GetIO().DisplaySize;
  const float ui_scale = BodyFontSize() / 13.f;
  const float gap = ImGui::GetFontSize();
  const float right_space = work_pos.x + work_size.x - menu_pos.x - menu_size.x - gap;
  const float left_space = menu_pos.x - work_pos.x - gap;
  const bool inline_mode = (std::max)(right_space, left_space) < 320.f * ui_scale
      || ImGui::GetContentRegionAvail().x < ImGui::GetFontSize() * 25.f;
  StyleScope theme;
  PushPanelTheme(theme);
  theme.Color(ImGuiCol_TitleBg, CurrentTheme().bg_control)
      .Color(ImGuiCol_TitleBgActive, CurrentTheme().bg_control_hover)
      .Color(ImGuiCol_TitleBgCollapsed, CurrentTheme().bg_control);
  bool visible = true;
  if (inline_mode) {
    visible = ImGui::CollapsingHeader((std::string(Tr("Runtime monitor")) + "###nr_monitor_inline").c_str());
  } else {
    if (!state->geometry.valid) {
      if (saved_geometry != nullptr && saved_geometry->valid) {
        state->geometry = *saved_geometry;
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
    actions.geometry_changed = corrected;
    // Only apply corrected geometry. Reapplying it each frame would defeat dragging.
    ImGui::SetNextWindowPos(state->geometry.position, corrected ? ImGuiCond_Always : ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(state->geometry.size, corrected ? ImGuiCond_Always : ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(
        ImVec2((std::min)(320.f * ui_scale, work_size.x), (std::min)(240.f * ui_scale, work_size.y)),
        work_size);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings;
    if (state->pin) {
      flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
    }
    visible = ImGui::Begin((std::string(Tr("Runtime monitor")) + "###nr_runtime_monitor").c_str(), &state->open, flags);
  }
  if (visible) {
    if (!inline_mode) {
      actions.geometry_changed |= ImGui::Checkbox(Tr("Lock position and size"), &state->pin);
      ItemTooltip("%s", Tr("Locks movement and resizing. Visibility follows the host overlay."));
    }
    actions.open_diagnostics = ImGui::Button((std::string(Tr("Open Diagnostics")) + "###status_open_diagnostics").c_str());
    if (ImGui::Button((std::string(Tr("Close monitor")) + "###status_close").c_str())) {
      state->open = false;
    }
    const auto row = [](const char* label, const std::string& value) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextWrapped("%s", Tr(label));
      ImGui::TableNextColumn();
      ImGui::TextWrapped("%s", value.empty() ? Tr("Pending confirmation") : value.c_str());
    };
    const auto begin_section = [](const char* id, const char* label) {
      ImGui::SeparatorText(Tr(label));
      if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) return false;
      ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.38f);
      ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.62f);
      return true;
    };
    if (begin_section("conclusion", "Current conclusion")) {
      row("Status", inputs.current_conclusion.empty() ? Tr(inputs.card.title) : inputs.current_conclusion);
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
    if (inline_mode && begin_section("compact_execution", "Execution chain")) {
      std::string stages;
      for (unsigned i = 0; i < count; ++i) {
        if (!stages.empty()) stages += " / ";
        stages += "P" + std::to_string(i + 1) + " " + inputs.passes[i].requested_stage;
      }
      row("Requested stage", stages);
      row("GPU cost", count != 0 && inputs.passes[0].gpu_state == MonitorGpuState::CURRENT
          ? inputs.passes[0].gpu_cost : Tr("Pending confirmation"));
      ImGui::EndTable();
    }
    if (!inline_mode) {
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
      if (begin_section("limits", "Current limits")) {
        row("Limitation", inputs.current_limit);
        row("Model evidence", Tr("A created model request does not prove the internal network actually used."));
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
