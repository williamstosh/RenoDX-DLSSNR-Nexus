/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "ui/fg_route.hpp"
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

  const auto luminance = [](const ImVec4& color) {
    const auto linear = [](float value) {
      return (value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f));
    };
    return 0.2126f * linear(color.x) + 0.7152f * linear(color.y) + 0.0722f * linear(color.z);
  };
  const auto contrast = [&](const ImVec4& foreground, const ImVec4& background) {
    const float a = luminance(foreground);
    const float b = luminance(background);
    return ((std::max)(a, b) + 0.05f) / ((std::min)(a, b) + 0.05f);
  };
  const ui::ThemeId expected_ids[] = {ui::ThemeId::CYBER_NEON, ui::ThemeId::NEXUS_DARK,
                                    ui::ThemeId::FLUENT_LIGHT, ui::ThemeId::MIDNIGHT_OLED};
  check(ui::kThemes.size() == 4, "four public themes are available");
  for (size_t index = 0; index < ui::kThemes.size(); ++index) {
    const auto& palette = ui::kThemes[index];
    check(palette.id == expected_ids[index], "theme IDs retain their public order");
    check(&ui::GetTheme(palette.id) == &palette, "theme lookup returns the registered palette");
    check(ui::ParseTheme(palette.slug) == palette.id, "stored slug parses to its public theme ID");
    check(palette.slug[0] != '\0' && palette.name[0] != '\0' && palette.description[0] != '\0',
          "theme identity and description are present");
    for (size_t previous = 0; previous < index; ++previous) {
      check(std::string_view(palette.slug) != ui::kThemes[previous].slug, "theme slugs are unique");
      check(std::string_view(palette.name) != ui::kThemes[previous].name, "theme names are unique");
    }
    for (const ImVec4 background : {palette.bg_window, palette.bg_card, palette.bg_control,
                                     palette.bg_control_hover, palette.bg_control_active}) {
      check(contrast(palette.text_primary, background) >= 4.5f, "primary text contrast includes active controls");
      check(contrast(palette.text_secondary, background) >= 3.f, "secondary text contrast includes active controls");
    }
    for (const ImVec4 accent : {palette.accent, palette.accent_hover, palette.accent_active}) {
      check(contrast(palette.on_accent, accent) >= 4.5f, "on-accent text contrast covers normal/hover/active");
    }
    check(contrast(palette.text_primary, palette.node_selected) >= 4.5f,
          "selected processing node retains readable text");
    for (const ImVec4 background : {palette.bg_window, palette.bg_card}) {
      for (const ImVec4 status : {palette.good, palette.warn, palette.bad, palette.idle}) {
        check(contrast(status, background) >= 3.f, "semantic status indicators contrast with panel and card");
      }
    }
    for (const ImVec4 color : {palette.bg_window, palette.bg_card, palette.bg_control,
                              palette.bg_control_hover, palette.bg_control_active, palette.stroke,
                              palette.text_primary, palette.text_secondary, palette.good, palette.warn,
                              palette.bad, palette.idle, palette.accent, palette.accent_hover,
                              palette.accent_active, palette.on_accent, palette.node_selected,
                              palette.node_border, palette.connector, palette.slider_knob, palette.switch_knob,
                              palette.slider_rail, palette.text_selection}) {
      for (float channel : {color.x, color.y, color.z, color.w}) {
        check(std::isfinite(channel) && channel >= 0.f && channel <= 1.f, "palette channels are finite normalized values");
      }
    }
    ui::SetTheme(palette.id);
    check(ui::CurrentThemeId() == palette.id && &ui::CurrentTheme() == &palette, "theme selection exposes the selected palette");
    ImGui::NewFrame();
    ImGui::Begin("theme scope");
    const ImGuiStyle before = ImGui::GetStyle();
    ImGuiContext& context = *ImGui::GetCurrentContext();
    const int colors = context.ColorStack.Size;
    const int vars = context.StyleVarStack.Size;
    const int fonts = context.FontStack.Size;
    {
      ui::StyleScope scope;
      ui::PushPanelTheme(scope);
      scope.FontSize(21.f);
      check(ImGui::GetStyle().Colors[ImGuiCol_FrameBgActive].x == palette.bg_control_active.x
                && ImGui::GetStyle().Colors[ImGuiCol_FrameBgActive].y == palette.bg_control_active.y
                && ImGui::GetStyle().Colors[ImGuiCol_FrameBgActive].z == palette.bg_control_active.z,
            "active frame uses its own palette token");
      const float alpha = ImGui::GetStyle().Alpha;
      ImGui::BeginDisabled();
      check(ImGui::GetStyle().Alpha < alpha && context.DisabledStackSize > 0,
            "disabled controls retain the ImGui disabled state and dimming");
      ImGui::Button("disabled themed control");
      ImGui::EndDisabled();
    }
    check(std::memcmp(&before, &ImGui::GetStyle(), sizeof(before)) == 0, "complete ImGuiStyle restored after themed widgets");
    check(context.ColorStack.Size == colors && context.StyleVarStack.Size == vars
              && context.FontStack.Size == fonts && context.DisabledStackSize == 0 && ui::style_depth == 0,
          "color, variable, font and disabled stacks restored");
    ImGui::End();
    ImGui::Render();
  }
  check(ui::ParseTheme("") == ui::ThemeId::CYBER_NEON
            && ui::ParseTheme("invalid-theme") == ui::ThemeId::CYBER_NEON,
        "empty and unknown stored slugs use Cyber Neon");
  ui::SetTheme(ui::ThemeId::CYBER_NEON);

  {
    ui::StatusCard card;
    card.frame_gen_multiplier = 4;
    card.present_before_fg = true;
    check(ui::FrameGenText(card) == ui::text::kFrameGenUnconfirmed,
          "a single old Pre-FG flag must not confirm the summary route");
    card.observed_fg_route = ui::FgRoute::kBeforeFg;
    check(ui::FrameGenText(card) == ui::text::kFrameGenBeforeFg, "confirmed Pre-FG summary missing");
    card.observed_fg_route = ui::FgRoute::kGameFrames;
    check(ui::FrameGenText(card) == ui::text::kFrameGenGameFrames, "summary ignores observed game-frame route");
    card.frame_gen_multiplier = 1;
    card.observed_fg_route = ui::FgRoute::kBeforeFg;
    check(ui::FrameGenText(card) == ui::text::kFgBeforeUnknown, "unknown multiplier must not hide confirmed Pre-FG");
    card.observed_fg_route = ui::FgRoute::kUnknown;
    card.fg_declared = true;
    check(ui::FrameGenText(card) == ui::text::kFgDeclared, "declared FG is distinct from observed activity");
    card.fg_declared = false;
    check(ui::FrameGenText(card) == ui::text::kFgStatusUnknown, "missing multiplier is not proof that FG is disabled");
  }
  {
    ui::FgRouteObserver observation;
    ui::FgRouteInputs input{.enabled = true, .present = true, .prefg_active = true, .multiplier = 4, .prefg_policy = 3};
    int64_t ns = 1;
    observation.Update(input, ns);
    const auto sample = [&] {
      ns += 300'000'000LL;
      input.nr_successes += 8;
      input.prefg_evaluates += 4;
      observation.Update(input, ns);
    };
    input.prefg_frames += 4;
    sample();
    check(observation.route == ui::FgRoute::kUnknown, "one Pre-FG window is not stable evidence");
    input.prefg_frames += 4;
    sample();
    check(observation.route == ui::FgRoute::kBeforeFg, "repeated successful Pre-FG is recognized");
    input.prefg_active = false;
    input.fallbacks += 4;
    input.captures += 4;
    sample();
    check(observation.route == ui::FgRoute::kBeforeFg, "a transient miss does not flash a fallback warning");
    input.fallbacks += 4;
    input.captures += 4;
    sample();
    check(observation.route == ui::FgRoute::kFallbackPresent, "repeated Present fallback is recognized");
    input.upscaled_active = true;
    for (int i = 0; i < 2; ++i) {
      input.upscaled_frames += 4;
      sample();
    }
    check(observation.route == ui::FgRoute::kFallbackUpscaled, "Upscaled fallback must not be labeled ordinary Present");
    input.upscaled_active = false;
    input.prefg_policy = 0;
    observation.Update(input, ++ns);
    for (int i = 0; i < 2; ++i) {
      input.captures += 4;
      sample();
    }
    check(observation.route == ui::FgRoute::kEveryPresent, "explicit Present uses the every-present policy");
    input.frame_policy = 1;
    observation.Update(input, ++ns);
    for (int i = 0; i < 2; ++i) {
      input.captures += 4;
      sample();
    }
    check(observation.route == ui::FgRoute::kGameFrames, "game-frame policy is distinguished");
    ns += 300'000'000LL;
    observation.Update(input, ns);
    ns += 300'000'000LL;
    observation.Update(input, ns);
    check(observation.route == ui::FgRoute::kUnknown, "stale counters cannot confirm an active route");
    input.prefg_active = true;
    input.prefg_policy = 3;
    input.prefg_frames += 1000;
    observation.Update(input, ++ns);
    sample();
    sample();
    check(observation.route == ui::FgRoute::kUnknown, "configuration and old successful totals are insufficient");
    for (int i = 0; i < 2; ++i) {
      input.prefg_frames += 3;
      input.fallbacks += 1;
      sample();
    }
    check(observation.route == ui::FgRoute::kMixed, "simultaneous success and fallback remains mixed");
    input.submitted_before_fg = true;
    for (int i = 0; i < 2; ++i) {
      input.fg_submitted += 4;
      input.fallbacks += 1;
      sample();
    }
    check(observation.route == ui::FgRoute::kBeforeFg,
          "queue-confirmed native/XeFG handoff takes precedence over legacy Pre-FG misses");
    input.multiplier = 1;
    observation.Update(input, ++ns);
    input.fg_submitted += 4;
    sample();
    check(observation.route == ui::FgRoute::kUnknown, "one unknown-multiplier handoff cannot confirm the route");
    input.fg_submitted += 4;
    sample();
    check(observation.route == ui::FgRoute::kBeforeFg, "native Pre-FG must not depend on legacy Present multiplier");
    input.submitted_before_fg = false;
    for (int i = 0; i < 2; ++i) {
      input.captures += 4;
      sample();
    }
    check(observation.route == ui::FgRoute::kUnknown, "ordinary NR work without multiplier evidence is not FG activity");
    input.submitted_before_fg = false;
    input.enabled = false;
    observation.Update(input, ++ns);
    check(observation.route == ui::FgRoute::kUnknown, "disabled NR clears the route");
  }
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
  for (const auto& theme : ui::kThemes) {
    ui::SetTheme(theme.id);
    for (int language : {0, 3, 13}) {
      ui::SelectLanguage(language);
      for (const auto& size : {ImVec2(320.f, 13.f), ImVec2(450.f, 26.f), ImVec2(1600.f, 39.f)}) {
        const float anchor = frame(idle, size.x, size.y, 0, true, false);
        for (ui::CardTone tone : {ui::CardTone::kGood, ui::CardTone::kWarn, ui::CardTone::kBad, ui::CardTone::kIdle}) {
          auto card = active;
          card.tone = tone;
          check(std::abs(frame(card, size.x, size.y, 4, true, false) - anchor) <= 0.01f,
                "all-theme summary geometry remains stable");
        }
      }
    }
  }
  ui::SetTheme(ui::ThemeId::CYBER_NEON);
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
