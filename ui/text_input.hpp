/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstring>
#include <cstdint>
#include <optional>
#include <string>

#include "../brand.h"
#include "../unicode_input.hpp"
#include "i18n.hpp"
#include "widgets.hpp"

namespace renodx::addons::dlss5::ui::text_input {
struct Pending {
  ImGuiID field = 0;
  std::uintptr_t context = 0;
  std::string initial;
  std::optional<unicode_input::Request> result;
};
static Pending pending;

// Draw-list geometry stays available when a player-selected font has no icon glyphs.
inline void DrawKeyboard(ImVec2 begin, ImVec2 end, ImU32 color) {
  const float width = end.x - begin.x;
  const float height = end.y - begin.y;
  const float stroke = (std::max)(1.f, height * 0.05f);
  auto* draw = ImGui::GetWindowDrawList();
  draw->AddRect(ImVec2(begin.x + width * 0.12f, begin.y + height * 0.20f),
                ImVec2(begin.x + width * 0.88f, begin.y + height * 0.80f), color, height * 0.08f, 0, stroke);
  for (int row = 0; row < 2; ++row) {
    for (int key = 0; key < 4; ++key) {
      const float x = begin.x + width * (0.24f + key * 0.17f);
      const float y = begin.y + height * (0.36f + row * 0.15f);
      draw->AddLine(ImVec2(x - width * 0.025f, y), ImVec2(x + width * 0.025f, y), color, stroke);
    }
  }
  draw->AddLine(ImVec2(begin.x + width * 0.35f, begin.y + height * 0.66f),
                ImVec2(begin.x + width * 0.65f, begin.y + height * 0.66f), color, stroke);
}

// The result is consumed only by its original ImGui ID. No widget buffer or
// ImGui pointer is retained by the worker, including while the popup is closed.
inline bool Draw(const char* label, char* buffer, std::size_t capacity,
                 bool multiline = false, ImVec2 size = ImVec2(), const char* caption = nullptr,
                 const char* hint = nullptr) {
  const auto field = ImGui::GetID(label);
  const auto context = reinterpret_cast<std::uintptr_t>(&ImGui::GetIO());
  if (auto result = unicode_input::TakeResult()) {
    if (result->accepted || result->failed) {
      pending.result = std::move(result);
    } else {
      pending = {};
    }
  }
  bool changed = false;
  const bool finishing = pending.result && pending.field == field && pending.context == context;
  bool failed = false;
  if (finishing) {
    failed = pending.result->failed;
    if (pending.result->accepted) {
      if (pending.initial == buffer && pending.result->value.size() < capacity) {
        std::memcpy(buffer, pending.result->value.c_str(), pending.result->value.size() + 1);
        changed = true;
      } else {
        failed = true;
      }
    }
    pending = {};
  }
  bool open_editor = false;
  const bool blocked = unicode_input::Busy() || finishing;
  if (caption != nullptr) {
    const auto& style = ImGui::GetStyle();
    const float available = (std::max)(0.f, ImGui::GetContentRegionAvail().x);
    const float button_width = (std::min)(available, ImGui::GetTextLineHeight() * 1.25f);
    const float title_width = (std::max)(0.f, available - button_width - style.ItemInnerSpacing.x);
    if (title_width >= ImGui::CalcTextSize("...").x) {
      ImGui::TextUnformatted(Ellipsize(caption, title_width).c_str());
      if (ImGui::CalcTextSize(caption).x > title_width) ItemTooltip("%s", caption);
    } else {
      ImGui::Dummy(ImVec2(0.f, ImGui::GetTextLineHeight()));
    }
    ImGui::SameLine(0.f, title_width >= ImGui::CalcTextSize("...").x ? style.ItemInnerSpacing.x : 0.f);
    ImGui::PushID(label);
    ImGui::BeginDisabled(blocked);
    {
      ImVec4 tint = CurrentTheme().accent;
      ImVec4 hover = CurrentTheme().accent_hover;
      ImVec4 active = CurrentTheme().accent_active;
      ImVec4 border = CurrentTheme().accent;
      tint.w = 0.12f;
      hover.w = 0.26f;
      active.w = 0.38f;
      border.w = 0.55f;
      StyleScope button_style;
      button_style.Color(ImGuiCol_Button, tint).Color(ImGuiCol_ButtonHovered, hover)
          .Color(ImGuiCol_ButtonActive, active).Color(ImGuiCol_Border, border)
          .Var(ImGuiStyleVar_FramePadding, ImVec2(0.f, 0.f))
          .Var(ImGuiStyleVar_FrameBorderSize, (std::max)(1.f, ImGui::GetFontSize() / 24.f));
      open_editor = ImGui::Button("###assisted_input", ImVec2(button_width, ImGui::GetTextLineHeight()));
      DrawKeyboard(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ToU32(CurrentTheme().text_primary));
    }
    ItemTooltip("%s", Tr("Assisted input: Open the compatibility text editor for Chinese and other text."));
    ImGui::EndDisabled();
    ImGui::PopID();
    ImGui::SetNextItemWidth(-1.f);
  }
  // Deactivating the original widget also retires its private ImGui edit state.
  // Keep it disabled on the return frame, so stale state cannot overwrite text.
  ImGui::BeginDisabled(blocked);
  if (multiline) {
    changed |= ImGui::InputTextMultiline(label, buffer, capacity, size);
  } else if (hint != nullptr) {
    changed |= ImGui::InputTextWithHint(label, hint, buffer, capacity);
  } else {
    changed |= ImGui::InputText(label, buffer, capacity);
  }
  ImGui::EndDisabled();
  if (ImGui::BeginPopupContextItem(label)) {
    if (ImGui::MenuItem(Tr("Assisted input..."), nullptr, false, !blocked)) open_editor = true;
    ImGui::EndPopup();
  }
  if (open_editor) {
    // Choosing another field explicitly discards a result for a closed page.
    pending = {};
    const char* visible = (caption == nullptr ? label : caption);
    while (*visible == '#') ++visible;
    HWND return_window = GetForegroundWindow();
    DWORD return_process = 0;
    if (return_window == nullptr || GetWindowThreadProcessId(return_window, &return_process) == 0
        || return_process != GetCurrentProcessId()) return_window = nullptr;
    if (unicode_input::Start({.field = field, .value = buffer, .title = NR_NEXUS_NAME,
            .label = visible, .apply_label = Tr("Apply"), .cancel_label = Tr("Cancel"),
            .invalid_label = Tr("Text is invalid or exceeds the UTF-8 byte limit."),
            .max_bytes = capacity - 1, .multiline = multiline, .return_window = return_window})) {
      pending.field = field;
      pending.context = context;
      pending.initial = buffer;
    } else {
      failed = true;
    }
  }
  if (failed) {
    ImGui::TextWrapped("%s", Tr("Unicode input failed or the original text changed. Please try again."));
  }
  return changed;
}

inline bool DrawLabeled(const char* title, char* buffer, std::size_t capacity) {
  return Draw((std::string("##") + title).c_str(), buffer, capacity, false, ImVec2(), title);
}

inline void Hint() {
  const std::string text = Tr("For Chinese and other text, click %s beside the field.");
  const auto marker = text.find("%s");
  const std::string before = text.substr(0, marker);
  const std::string after = text.substr(marker + 2);
  const float available = (std::max)(0.f, ImGui::GetContentRegionAvail().x);
  const float icon_width = (std::min)(available, ImGui::GetTextLineHeight() * 1.25f);
  const float text_width = (std::max)(0.f, available - icon_width);
  const float before_width = ImGui::CalcTextSize(before.c_str()).x;
  const float after_width = ImGui::CalcTextSize(after.c_str()).x;
  const float scale = (std::min)(1.f, text_width / (std::max)(1.f, before_width + after_width));
  const float ellipsis_width = ImGui::CalcTextSize("...").x;
  const std::string shown_before = (scale >= 1.f ? before : (before_width * scale >= ellipsis_width ? Ellipsize(before.c_str(), before_width * scale) : ""));
  const std::string shown_after = (scale >= 1.f ? after : (after_width * scale >= ellipsis_width ? Ellipsize(after.c_str(), after_width * scale) : ""));
  StyleScope hint_style;
  hint_style.Color(ImGuiCol_Text, CurrentTheme().text_secondary);
  ImGui::BeginGroup();
  ImGui::TextUnformatted(shown_before.c_str());
  ImGui::SameLine(0.f, 0.f);
  const ImVec2 icon_at = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(icon_width, ImGui::GetTextLineHeight()));
  DrawKeyboard(icon_at, ImGui::GetItemRectMax(), ToU32(CurrentTheme().text_secondary));
  ImGui::SameLine(0.f, 0.f);
  ImGui::TextUnformatted(shown_after.c_str());
  ImGui::EndGroup();
}
}  // namespace renodx::addons::dlss5::ui::text_input
