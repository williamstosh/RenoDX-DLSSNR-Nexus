/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#include <imgui.h>
#include <imgui_internal.h>

#include <cmath>
#include <cstdio>
#include <string>

#include "ui/widgets.hpp"

namespace ui = renodx::addons::dlss5::ui;

int main() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.DisplaySize = ImVec2(2200.f, 1400.f);
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
      if (failures < 12) {
        std::printf("FAIL: %s\n", message);
      }
    }
  };
  const auto frame = [&](const ui::StatusCard& card, float panel_width, float font,
                         unsigned warnings, bool summary, bool popup) {
    ImGui::GetStyle().FontSizeBase = font;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
    ImGui::SetNextWindowSize(ImVec2(panel_width, 1300.f));
    ImGui::Begin("layout", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);
    float next_y = 0.f;
    {
      ui::StyleScope theme;
      ui::PushPanelTheme(theme);
      ImGui::PushTextWrapPos(0.f);
      if (summary) {
        ui::DrawStatusSummary(card, warnings);
      } else {
        ui::DrawStatusCard(card);
      }
      if (popup) {
        ImGui::OpenPopup("details");
      }
      ImGui::SetNextWindowSize(ImVec2(400.f, 300.f), ImGuiCond_Appearing);
      if (ImGui::BeginPopup("details")) {
        ui::DrawStatusCard(card);
        ImGui::EndPopup();
      }
      next_y = ImGui::GetCursorScreenPos().y;
      ImGui::Button("main settings", ImVec2(120.f, 0.f));
      ImGui::PopTextWrapPos();
    }
    ImGui::End();
    ImGui::Render();
    check(ui::style_depth == 0, "balanced theme stack");
    if (summary) {
      for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows) {
        if (window->Active && std::strstr(window->Name, "##nr_status_summary") != nullptr) {
          const float overflow = window->DC.CursorMaxPos.y - (window->Pos.y + window->Size.y - window->WindowPadding.y);
          if (overflow > 1.f && failures < 12) {
            std::printf("overflow font=%.0f width=%.0f delta=%.1f\n", font, panel_width, overflow);
          }
          check(overflow <= 1.f, "summary contents fit without clipping");
        }
      }
    }
    return next_y;
  };
  ui::StatusCard idle{.tone = ui::CardTone::kIdle, .title = "Neural Rendering is off", .body = "Off"};
  ui::StatusCard active{.tone = ui::CardTone::kWarn,
                        .title = "Neural Rendering is on",
                        .body = "A long status message that wraps on narrow panels.\nA second line.",
                        .path_note = "Direct3D 11 bridge, at the present",
                        .fix = {"A complete repair instruction that must remain accessible in the details.",
                                "A second repair instruction.", "A third repair instruction."},
                        .fix_count = 3,
                        .frames_enhanced = 9999999999ull,
                        .frames_eligible = 99999999999ull,
                        .input_width = 3840,
                        .input_height = 2160,
                        .output_width = 7680,
                        .output_height = 4320,
                        .nr_gpu_ms = 13.4f,
                        .nr_present_ms = 3.3f,
                        .frame_ms = 16.7f,
                        .frame_gen_multiplier = 4,
                        .present_gaps = 999999};
  // Preserve a measured baseline: the full detail card is still auto-sized.
  float old_idle = 0.f, old_active = 0.f;
  for (int tick = 0; tick < 4; ++tick) {
    old_idle = frame(idle, 450.f, 26.f, 0, false, false);
  }
  for (int tick = 0; tick < 4; ++tick) {
    old_active = frame(active, 450.f, 26.f, 4, false, false);
  }
  std::printf("auto-height baseline delta_y=%.1f px\n", old_active - old_idle);
  check(old_active - old_idle > 20.f, "baseline reproduces automatic height movement");
  float max_delta = 0.f;
  unsigned scenarios = 0;
  for (int language = 0; language < ui::kLanguageCount; ++language) {
    ui::SelectLanguage(language);
    for (float font : {13.f, 26.f, 39.f}) {
      for (float panel_width : {320.f, 450.f, 900.f, 1600.f}) {
        const float anchor = frame(idle, panel_width, font, 0, true, false);
        for (unsigned state = 0; state < ui::kCardIdCount; ++state) {
          for (unsigned variant = 0; variant < 8; ++variant) {
            ui::StatusCard card = (variant == 0 ? idle : active);
            card.id = static_cast<ui::CardId>(state);
            card.tone = static_cast<ui::CardTone>(variant % 4);
            card.frames_enhanced += state * 1000000u;
            card.nr_gpu_ms = (variant % 2 == 0 ? 0.f : 0.1f + state * 1.7f);
            card.frame_gen_multiplier = variant;
            card.present_every_frame = (variant % 2 != 0);
            card.present_before_fg = (variant % 3 == 0);
            const float y = frame(card, panel_width, font, variant, true, variant == 7);
            max_delta = (std::max)(max_delta, std::abs(y - anchor));
            check(std::abs(y - anchor) <= 0.01f, "main settings Y remains stable across live status changes");
            ++scenarios;
          }
        }
      }
    }
  }
  std::printf("summary scenarios=%u languages=%d max_delta_y=%.3f px checks=%u failures=%u\n",
              scenarios, ui::kLanguageCount, max_delta, checks, failures);
  ImGui::DestroyContext();
  return failures == 0 ? 0 : 1;
}
