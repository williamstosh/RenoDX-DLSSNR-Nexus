/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <cmath>
#include <string>

#include <imgui.h>
#include <imgui_internal.h>

#include "ui/widgets.hpp"

template <typename Check>
void RunChainUxCases(const Check& check) {
  namespace ui = renodx::addons::dlss5::ui;
  ImGuiIO& io = ImGui::GetIO();
  const auto compare_text = [&](const ui::ProcessingChainNode& node, ImVec2 size, bool old_button) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (old_button) {
      ImGui::Button("Pass 1\nB (Small FP8) *\nEditing###old_chain", size);
    } else {
      ui::ProcessingChainButton(node, size, true);
    }
    const ImVec2 begin = ImGui::GetItemRectMin();
    const ImVec2 end = ImGui::GetItemRectMax();
    const int actual_end = draw->VtxBuffer.Size;
    draw->PushClipRect(begin, end, true);
    float y = begin.y + (size.y - node.content_height) * 0.5f;
    for (const auto& line : node.lines) {
      draw->AddText(ImVec2(begin.x + (size.x - line.size.x) * 0.5f, y),
                    ImGui::GetColorU32(ImGuiCol_Text), line.text.c_str());
      y += line.size.y + ImGui::GetStyle().ItemInnerSpacing.y;
    }
    draw->PopClipRect();
    const int reference_count = draw->VtxBuffer.Size - actual_end;
    if (reference_count == 0 || actual_end < reference_count) return false;
    bool matches = true;
    for (int vertex = 0; vertex < reference_count; ++vertex) {
      const ImVec2 actual = draw->VtxBuffer[actual_end - reference_count + vertex].pos;
      const ImVec2 reference = draw->VtxBuffer[actual_end + vertex].pos;
      matches &= std::abs(actual.x - reference.x) < 0.01f && std::abs(actual.y - reference.y) < 0.01f;
    }
    return matches;
  };

  ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
  ImGui::SetNextWindowSize(ImVec2(1000.f, 1300.f));
  ImGui::Begin("chain regression", nullptr, ImGuiWindowFlags_NoSavedSettings);
  {
    ui::StyleScope spacing;
    spacing.Var(ImGuiStyleVar_ItemInnerSpacing, ImVec2(4.f, 0.f));
    const auto node = ui::MeasureProcessingChainNode({"Pass 1", "B (Small FP8) *", "Editing"}, 500.f);
    check(!compare_text(node, ImVec2(500.f, 100.f), true),
          "old multiline Button reproduces independently misaligned short rows");
    check(compare_text(node, ImVec2(500.f, 100.f), false),
          "replacement centers each measured row and the full vertical block");
  }
  ImGui::End();
  ImGui::Render();

  for (const auto& theme : ui::kThemes) {
    ui::SetTheme(theme.id);
    for (int language = 0; language < ui::kLanguageCount; ++language) {
      ui::SelectLanguage(language);
      for (float font : {13.f, 26.f, 39.f}) {
        ImGui::GetStyle().FontSizeBase = font;
        for (float width : {120.f, 260.f, 760.f}) {
          ImGui::NewFrame();
          ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
          ImGui::SetNextWindowSize(ImVec2(1000.f, 1300.f));
          ImGui::Begin("chain regression", nullptr, ImGuiWindowFlags_NoSavedSettings);
          {
            ui::StyleScope panel;
            ui::PushPanelTheme(panel);
            const auto node = ui::MeasureProcessingChainNode(
                {ui::Tr("Pass 1"), std::string(ui::Tr(ui::text::kInheritModel)) + " *", ui::Tr(ui::text::kConfigured)}, width);
            const float height = node.content_height + ImGui::GetStyle().FramePadding.y * 2.f;
            check(node.lines.size() >= 3, "wrapped card retains title, model and configuration status");
            for (const auto& line : node.lines) {
              check(line.size.x <= width - ImGui::GetStyle().FramePadding.x * 2.f + 0.01f,
                    "localized wrapped text stays within horizontal padding");
              check(line.text.empty() || (static_cast<unsigned char>(line.text.front()) & 0xC0) != 0x80,
                    "wrapped model names retain complete UTF-8 code points");
            }
            check(compare_text(node, ImVec2(width, height), false),
                  "all themes, languages, widths and font sizes render independently centered lines");
            check(ImGui::GetItemRectSize().x == width && ImGui::GetItemRectSize().y == height,
                  "card hit rectangle covers the full uniform card geometry");
            check(ImGui::GetItemID() == ImGui::GetID("###chain_pass"),
                  "chain editor selection retains its existing button identity");
          }
          ImGui::End();
          ImGui::Render();
        }
      }
    }
  }

  ui::SelectLanguage(0);
  ImGui::GetStyle().FontSizeBase = 26.f;
  ImVec2 center;
  int selected_pass = -1;
  for (int tick = 0; tick < 4; ++tick) {
    if (tick > 0) io.AddMousePosEvent(center.x, center.y);
    if (tick == 2) io.AddMouseButtonEvent(0, true);
    if (tick == 3) io.AddMouseButtonEvent(0, false);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
    ImGui::SetNextWindowSize(ImVec2(220.f, 1300.f));
    ImGui::Begin("narrow chain click", nullptr, ImGuiWindowFlags_NoSavedSettings);
    const auto node = ui::MeasureProcessingChainNode({"Pass 4", "Long localized model name", "Editing"}, 120.f);
    for (int pass = 0; pass < 4; ++pass) {
      ImGui::PushID(pass);
      if (ui::ProcessingChainButton(node, ImVec2(120.f, node.content_height + 16.f), selected_pass == pass)) {
        selected_pass = pass;
      }
      const ImVec2 begin = ImGui::GetItemRectMin();
      const ImVec2 end = ImGui::GetItemRectMax();
      if (pass == 3) center = ImVec2((begin.x + end.x) * 0.5f, (begin.y + end.y) * 0.5f);
      ImGui::PopID();
    }
    ImGui::End();
    ImGui::Render();
  }
  check(selected_pass == 3, "four narrow wrapped pass cards keep independent clickable editor selection");
  io.AddMousePosEvent(-100.f, -100.f);
  check(ui::style_depth == 0, "chain geometry leaves style stacks balanced");
}
