/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cmath>
#include <cstdio>
#include <limits>

#include <imgui.h>

#include "ui/notice_position.hpp"
#include "ui/widgets.hpp"

template <typename Check>
void RunNoticePositionCases(const Check& check) {
  namespace ui = renodx::addons::dlss5::ui;
  const auto near = [](float a, float b) { return std::abs(a - b) < .001f; };
  for (const ImVec2 display : {ImVec2(1920.f, 1080.f), ImVec2(3840.f, 2160.f), ImVec2(320.f, 180.f)}) {
    for (const ImVec2 size : {ImVec2(260.f, 46.f), ImVec2(600.f, 80.f)}) {
      for (float x : {0.f, 1.f, 25.f, 33.3f, 50.f, 73.f, 100.f}) {
        for (float y : {0.f, 20.f, 50.f, 87.f, 100.f}) {
          const ui::NoticePosition legacy{.legacy_percent = ImVec2(x, y)};
          const auto pivot = ui::NoticePivot(legacy);
          const auto lo = ui::NoticeTopLeft(legacy, display, size, 8.f, 1.f);
          check(near(pivot.x, x / 100.f) && near(pivot.y, y / 100.f),
              "legacy percentages retain their original pivot");
          check(near(lo.x, 8.f + (display.x - 16.f - size.x) * (x / 100.f))
                  && near(lo.y, 8.f + (display.y - 16.f - size.y) * (y / 100.f)),
              "legacy operation-notice top-left remains unchanged, including oversized notices");
          const auto window_anchor = ui::NoticeTopLeft(legacy, display, ImVec2(), 8.f, 1.f);
          check(near(window_anchor.x - size.x * pivot.x, lo.x)
                  && near(window_anchor.y - size.y * pivot.y, lo.y),
              "legacy auto-sized HUD and foreground notices use equivalent placement");
        }
      }
    }
    for (int anchor = 0; anchor < 9; ++anchor) {
      for (float scale : {1.f, 1.5f, 2.f}) {
        for (const ImVec2 offset : {ImVec2(), ImVec2(30.f, -20.f), ImVec2(-8000.f, 8000.f)}) {
          const ui::NoticePosition position{.anchor = anchor, .offset = offset};
          const ImVec2 size(200.f, 60.f);
          const auto lo = ui::NoticeTopLeft(position, display, size, 8.f, scale);
          check(lo.x >= 8.f && lo.y >= 8.f && lo.x + size.x <= display.x - 8.f
                  && lo.y + size.y <= display.y - 8.f, "nine anchors and scaled offsets remain on screen");
          if (offset.x == 0.f && offset.y == 0.f) {
            check(near(lo.x, 8.f + (display.x - 16.f - size.x) * static_cast<float>(anchor % 3) * .5f)
                    && near(lo.y, 8.f + (display.y - 16.f - size.y) * static_cast<float>(anchor / 3) * .5f),
                "grid anchors run left-to-right and top-to-bottom");
          }
        }
      }
    }
  }
  const auto normalized = ui::NormalizeNoticePosition({.anchor = 42,
      .legacy_percent = ImVec2(std::numeric_limits<float>::quiet_NaN(), 200.f),
      .offset = ImVec2(std::numeric_limits<float>::infinity(), -20000.f)});
  check(normalized.anchor == -1 && normalized.legacy_percent.x == 50.f
          && normalized.legacy_percent.y == 100.f && normalized.offset.x == 0.f
          && normalized.offset.y == -16384.f, "invalid stored placement is normalized without selecting a new anchor");
  const auto reset = ui::NoticeTopLeft({.anchor = 1}, ImVec2(1920.f, 1080.f), ImVec2(200.f, 60.f), 8.f, 1.f);
  check(near(reset.x, 860.f) && near(reset.y, 8.f), "reset restores top-center with zero offsets");
  const auto shifted = ui::NoticeTopLeft({.anchor = 4, .offset = ImVec2(12.f, -8.f)},
      ImVec2(1920.f, 1080.f), ImVec2(200.f, 60.f), 8.f, 2.f);
  check(near(shifted.x, 884.f) && near(shifted.y, 494.f), "offsets use unscaled UI pixels in both axes");

  auto& io = ImGui::GetIO();
  const auto display_before = io.DisplaySize;
  io.DisplaySize = ImVec2(900.f, 800.f);
  int first = -1;
  int second = 8;
  ImVec2 target;
  const int style_before = ui::style_depth;
  const auto frame = [&] {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(20.f, 20.f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(500.f, 650.f), ImGuiCond_Always);
    ImGui::Begin("Notice position fixture", nullptr, ImGuiWindowFlags_NoSavedSettings);
    ImGui::PushID("first");
    ImGui::SetNextItemWidth(180.f);
    target = ImGui::GetCursorScreenPos();
    target.x += ImGui::GetFrameHeight() * .75f;
    target.y += ImGui::GetFrameHeight() * .75f;
    const bool changed = ui::DrawPositionGrid(&first);
    ImGui::PopID();
    ImGui::PushID("second");
    ImGui::SetNextItemWidth(180.f);
    check(!ui::DrawPositionGrid(&second) && second == 8, "caller ID scopes isolate simultaneous grids");
    ImGui::PopID();
    ImGui::End();
    ImGui::Render();
    check(ui::style_depth == style_before, "position grid restores theme style depth");
    return changed;
  };
  io.AddMousePosEvent(-100.f, -100.f);
  io.AddMouseButtonEvent(0, false);
  check(!frame() && first == -1, "custom position stays unselected without input");
  io.AddMousePosEvent(target.x, target.y);
  frame();
  io.AddMouseButtonEvent(0, true);
  frame();
  io.AddMouseButtonEvent(0, false);
  check(frame() && first == 0, "clicking top-left selects anchor zero and reports changed");
  check(!frame() && first == 0, "unchanged selected anchor does not report a change");
  io.DisplaySize = display_before;
  io.AddMousePosEvent(-100.f, -100.f);
}

#ifdef NOTICE_POSITION_STANDALONE
int main() {
  ImGui::CreateContext();
  auto& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.DeltaTime = 1.f / 60.f;
  io.Fonts->AddFontDefault();
  unsigned char* pixels;
  int width, height;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
  unsigned checks = 0, failures = 0;
  RunNoticePositionCases([&](bool result, const char* message) {
    ++checks;
    if (!result) {
      ++failures;
      std::printf("FAIL: %s\n", message);
    }
  });
  std::printf("notice position checks=%u failures=%u\n", checks, failures);
  ImGui::DestroyContext();
  return failures == 0 ? 0 : 1;
}
#endif
