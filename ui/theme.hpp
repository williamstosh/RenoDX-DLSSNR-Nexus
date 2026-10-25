/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Design tokens and the scoped theme (PLAN_UI_V7.md 1).
//
// Everything here is pushed inside this addon's overlay callback and popped
// before it returns, so ReShade's own tabs and other add-ons never see it.
// Two constraints decided the shape (U0):
//   - ReShade calls Begin() for our window before our callback runs
//     (runtime_gui.cpp), so the window background is already drawn; the dark
//     background is a full-size child region, not a style push;
//   - an add-on cannot load a font (the add-on ImGui table has no ImFontAtlas
//     entry points and ReShade rebuilds its own atlas), so text sizes are
//     ReShade's font through PushFont(nullptr, size), and icons are the
//     ForkAwesome glyphs ReShade already merges into it.
// The capture lane (dlss5_e2e_ui_capture) renders the panel through a real
// ReShade overlay, reads the pixels back, and logs style_depth, which must be
// zero at the end of every callback.
//
// Include after <imgui.h> and reshade.hpp, like the rest of the overlay.

#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace renodx::addons::dlss5::ui {

namespace tokens {

constexpr ImVec4 Hex(std::uint32_t rgb, float alpha = 1.f) {
  return ImVec4(static_cast<float>((rgb >> 16) & 0xff) / 255.f,
                static_cast<float>((rgb >> 8) & 0xff) / 255.f,
                static_cast<float>(rgb & 0xff) / 255.f, alpha);
}

inline constexpr float kRadiusCard = 8.f;
inline constexpr float kRadiusControl = 6.f;
inline constexpr float kRadiusSmall = 4.f;
inline constexpr float kRowGap = 6.f;
inline constexpr float kSectionGap = 12.f;
inline constexpr float kCardPadding = 12.f;
// Row geometry, in units of the font size so it follows the player's
// ReShade font.  alpha45 gave labels 42 % of the panel, which at 4K put
// every control in the middle of the screen; the label column is now a fixed
// width, and never more than kLabelShare of a narrow panel.  The panel is
// split into two columns at kWideEm, and a slider's value has its own column
// (kValueEm) instead of sitting under the knob.  A panel narrower than
// kStackEm has no room for a label column beside a usable control - ReShade
// docks it at ~450 px and doubles its font at 2160p - so each label goes on
// its own line over a control as wide as the panel.
inline constexpr float kLabelEm = 12.f;
inline constexpr float kLabelShare = 0.42f;
inline constexpr float kWideEm = 60.f;
inline constexpr float kPassNodeEm = 12.f;
// Initial undocked window width; independent of the responsive content layout.
inline constexpr float kColumnEm = 40.f;
inline constexpr float kValueEm = 3.5f;
inline constexpr float kStackEm = 24.f;
// A tooltip's text wraps at this width: ImGui sizes a tooltip to its text and
// never wraps it, so a paragraph tooltip ran as one line across the screen
// (Hook point at 2160p, v8.5.0-rc9).
inline constexpr float kTooltipEm = 35.f;

}  // namespace tokens

enum class ThemeId : std::uint8_t { CYBER_NEON, NEXUS_DARK, FLUENT_LIGHT, MIDNIGHT_OLED };

struct ThemeTokens {
  ThemeId id;
  const char* slug;
  const char* name;
  const char* description;
  ImVec4 bg_window, bg_card, bg_control, bg_control_hover, bg_control_active;
  ImVec4 stroke, text_primary, text_secondary;
  ImVec4 good, warn, bad, idle;
  ImVec4 accent, accent_hover, accent_active, on_accent;
  ImVec4 node_selected, node_border, connector, slider_knob, switch_knob, slider_rail;
  ImVec4 text_selection;
  float control_border_size = 0.f;
};

// Immutable palettes, shared geometry. Theme changes select a table entry;
// widgets never rebuild a palette or branch on a theme's identity.
inline constexpr std::array<ThemeTokens, 4> kThemes = {{
    {.id = ThemeId::CYBER_NEON, .slug = "cyber_neon", .name = "Cyber Neon",
     .description = "Precision cyan on deep blue.",
     .bg_window = tokens::Hex(0x080F1C), .bg_card = tokens::Hex(0x121D30),
     .bg_control = tokens::Hex(0x1B2B42), .bg_control_hover = tokens::Hex(0x263B55),
     .bg_control_active = tokens::Hex(0x304B66), .stroke = tokens::Hex(0x33465E),
     .text_primary = tokens::Hex(0xE1F7FF), .text_secondary = tokens::Hex(0xA4B9CD),
     .good = tokens::Hex(0x9CCF54), .warn = tokens::Hex(0xFFC16B),
     .bad = tokens::Hex(0xFF8A91), .idle = tokens::Hex(0xA4B9CD),
     .accent = tokens::Hex(0x17D6F5), .accent_hover = tokens::Hex(0x56E5FF),
     .accent_active = tokens::Hex(0x0FB2D0), .on_accent = tokens::Hex(0x06111A),
     .node_selected = tokens::Hex(0x153449), .node_border = tokens::Hex(0x466580),
     .connector = tokens::Hex(0x7D9CB6), .slider_knob = tokens::Hex(0xE1F7FF),
     .switch_knob = tokens::Hex(0x06111A), .slider_rail = tokens::Hex(0x466580), .text_selection = tokens::Hex(0x245A74)},
    {.id = ThemeId::NEXUS_DARK, .slug = "nexus_dark", .name = "Nexus Dark",
     .description = "Classic teal and charcoal.",
     .bg_window = tokens::Hex(0x14161A, 0.94f), .bg_card = tokens::Hex(0x1D2026),
     .bg_control = tokens::Hex(0x262A31), .bg_control_hover = tokens::Hex(0x2F343C),
     .bg_control_active = tokens::Hex(0x37444A), .stroke = tokens::Hex(0x2A2E36),
     .text_primary = tokens::Hex(0xE8EAED), .text_secondary = tokens::Hex(0x9AA0A8),
     .good = tokens::Hex(0x76B900), .warn = tokens::Hex(0xF5A623),
     .bad = tokens::Hex(0xEB5757), .idle = tokens::Hex(0x6E7681),
     .accent = tokens::Hex(0x4CA6A0), .accent_hover = tokens::Hex(0x5BB8B2),
     .accent_active = tokens::Hex(0x3E8C87), .on_accent = tokens::Hex(0x101214),
     .node_selected = tokens::Hex(0x233D3E), .node_border = tokens::Hex(0x57646A),
     .connector = tokens::Hex(0x9AA0A8), .slider_knob = tokens::Hex(0xE8EAED),
     .switch_knob = tokens::Hex(0xE8EAED), .slider_rail = tokens::Hex(0x2F343C), .text_selection = tokens::Hex(0x335F60)},
    {.id = ThemeId::FLUENT_LIGHT, .slug = "fluent_light", .name = "Fluent Light",
     .description = "Clear blue on bright surfaces.",
     .bg_window = tokens::Hex(0xF3F5F8), .bg_card = tokens::Hex(0xFFFFFF),
     .bg_control = tokens::Hex(0xE5EAF0), .bg_control_hover = tokens::Hex(0xD5E2EF),
     .bg_control_active = tokens::Hex(0xC4D8EB), .stroke = tokens::Hex(0x7A899B),
     .text_primary = tokens::Hex(0x20242A), .text_secondary = tokens::Hex(0x485563),
     .good = tokens::Hex(0x3F6C00), .warn = tokens::Hex(0x875000),
     .bad = tokens::Hex(0xB02032), .idle = tokens::Hex(0x485563),
     .accent = tokens::Hex(0x0078D4), .accent_hover = tokens::Hex(0x006CBE),
     .accent_active = tokens::Hex(0x005A9E), .on_accent = tokens::Hex(0xFFFFFF),
     .node_selected = tokens::Hex(0xE1EFFB), .node_border = tokens::Hex(0x7A899B),
     .connector = tokens::Hex(0x536A82), .slider_knob = tokens::Hex(0x005A9E),
     .switch_knob = tokens::Hex(0xFFFFFF), .slider_rail = tokens::Hex(0x7A899B), .text_selection = tokens::Hex(0xA4CCF0),
     .control_border_size = 1.f},
    {.id = ThemeId::MIDNIGHT_OLED, .slug = "midnight_oled", .name = "Midnight OLED",
     .description = "Pure black with restrained green.",
     .bg_window = tokens::Hex(0x050505), .bg_card = tokens::Hex(0x141414),
     .bg_control = tokens::Hex(0x202020), .bg_control_hover = tokens::Hex(0x303030),
     .bg_control_active = tokens::Hex(0x3B3B3B), .stroke = tokens::Hex(0x505050),
     .text_primary = tokens::Hex(0xF5F5F5), .text_secondary = tokens::Hex(0xB4B4B4),
     .good = tokens::Hex(0x9DCA67), .warn = tokens::Hex(0xE9B65E),
     .bad = tokens::Hex(0xF38A8A), .idle = tokens::Hex(0xB4B4B4),
     .accent = tokens::Hex(0x76B900), .accent_hover = tokens::Hex(0x91D42B),
     .accent_active = tokens::Hex(0x659E00), .on_accent = tokens::Hex(0x050505),
     .node_selected = tokens::Hex(0x18200C), .node_border = tokens::Hex(0x626262),
     .connector = tokens::Hex(0xA3A3A3), .slider_knob = tokens::Hex(0xF5F5F5),
     .switch_knob = tokens::Hex(0x050505), .slider_rail = tokens::Hex(0x626262), .text_selection = tokens::Hex(0x365414),
     .control_border_size = 1.f},
}};

inline const ThemeTokens& GetTheme(ThemeId id) {
  const auto index = static_cast<std::size_t>(id);
  return kThemes[index < kThemes.size() ? index : 0];
}
static const ThemeTokens* current_theme = &kThemes[0];
inline const ThemeTokens& CurrentTheme() { return *current_theme; }
inline ThemeId CurrentThemeId() { return current_theme->id; }
inline void SetTheme(ThemeId id) { current_theme = &GetTheme(id); }

inline ThemeId ParseTheme(std::string_view slug) {
  for (const auto& theme : kThemes) {
    if (slug == theme.slug) return theme.id;
  }
  return ThemeId::CYBER_NEON;
}

// ForkAwesome glyphs, as ReShade merges them (external/reshade/res/fonts/
// forkawesome.h: the subset ReShade ships, which is all the atlas holds).
namespace icons {
inline constexpr const char* kCheck = "\xef\x80\x8c";          // U+F00C ok
inline constexpr const char* kWarning = "\xef\x81\xb1";        // U+F071 warning
inline constexpr const char* kCross = "\xef\x80\x8d";          // U+F00D cancel
inline constexpr const char* kCircle = "\xef\x84\x8c";         // U+F10C circle_empty
inline constexpr const char* kHourglass = "\xef\x89\x92";      // U+F252 hourglass_half
inline constexpr const char* kClipboard = "\xef\x83\xaa";      // U+F0EA clipboard
inline constexpr const char* kFolderOpen = "\xef\x84\x95";     // U+F115 folder_open
inline constexpr const char* kInfo = "\xef\x81\x9a";           // U+F05A info_circle
inline constexpr const char* kWrench = "\xef\x82\xad";         // U+F0AD wrench
inline constexpr const char* kPower = "\xef\x80\x91";          // U+F011 off
inline constexpr const char* kUndo = "\xef\x83\xa2";           // U+F0E2 undo
}  // namespace icons

// Text sizes, relative to the size the player chose for ReShade (its
// FontSize setting, 13 by default): 15 / 13 / 12 at the default, scaled with
// it otherwise, so a player who enlarged ReShade's font keeps that here.
inline float BaseFontSize() {
  const float base = ImGui::GetStyle().FontSizeBase;
  return base > 0.f ? base : 13.f;
}
inline float TitleFontSize() { return BaseFontSize() * (15.f / 13.f); }
inline float BodyFontSize() { return BaseFontSize(); }
inline float SmallFontSize() { return BaseFontSize() * (12.f / 13.f); }

inline ImU32 ToU32(const ImVec4& color) { return ImGui::GetColorU32(color); }

// Pushes and pops in one object, so a return from the middle of a panel
// cannot leave a colour or a variable on ReShade's stacks.  The depth
// counter is the U0 witness: the capture lane logs it at the end of every
// callback, and anything but zero is a leak.
inline int style_depth = 0;

class StyleScope {
 public:
  StyleScope() = default;
  StyleScope(const StyleScope&) = delete;
  StyleScope& operator=(const StyleScope&) = delete;
  ~StyleScope() {
    if (vars_ != 0) ImGui::PopStyleVar(vars_);
    if (colors_ != 0) ImGui::PopStyleColor(colors_);
    if (fonts_ != 0) {
      for (int i = 0; i < fonts_; ++i) ImGui::PopFont();
    }
    style_depth -= vars_ + colors_ + fonts_;
  }
  StyleScope& Color(ImGuiCol index, const ImVec4& color) {
    ImGui::PushStyleColor(index, color);
    ++colors_;
    ++style_depth;
    return *this;
  }
  StyleScope& Var(ImGuiStyleVar index, float value) {
    ImGui::PushStyleVar(index, value);
    ++vars_;
    ++style_depth;
    return *this;
  }
  StyleScope& Var(ImGuiStyleVar index, const ImVec2& value) {
    ImGui::PushStyleVar(index, value);
    ++vars_;
    ++style_depth;
    return *this;
  }
  StyleScope& FontSize(float size) {
    ImGui::PushFont(nullptr, size);
    ++fonts_;
    ++style_depth;
    return *this;
  }

 private:
  int colors_ = 0;
  int vars_ = 0;
  int fonts_ = 0;
};

// The panel theme: every colour and shape token ImGui draws our widgets
// with.  Pushed once at the top of the callback.
inline void PushPanelTheme(StyleScope& scope) {
  using namespace tokens;
  scope.Color(ImGuiCol_Text, CurrentTheme().text_primary)
      .Color(ImGuiCol_TextDisabled, CurrentTheme().text_secondary)
      .Color(ImGuiCol_ChildBg, CurrentTheme().bg_window)
      .Color(ImGuiCol_WindowBg, CurrentTheme().bg_card)
      .Color(ImGuiCol_Border, CurrentTheme().stroke)
      .Color(ImGuiCol_BorderShadow, tokens::Hex(0, 0.f))
      .Color(ImGuiCol_Separator, CurrentTheme().stroke)
      .Color(ImGuiCol_FrameBg, CurrentTheme().bg_control)
      .Color(ImGuiCol_FrameBgHovered, CurrentTheme().bg_control_hover)
      .Color(ImGuiCol_FrameBgActive, CurrentTheme().bg_control_active)
      .Color(ImGuiCol_Button, CurrentTheme().bg_control)
      .Color(ImGuiCol_ButtonHovered, CurrentTheme().bg_control_hover)
      .Color(ImGuiCol_ButtonActive, CurrentTheme().bg_control_active)
      .Color(ImGuiCol_Header, CurrentTheme().bg_card)
      .Color(ImGuiCol_HeaderHovered, CurrentTheme().bg_control_hover)
      .Color(ImGuiCol_HeaderActive, CurrentTheme().bg_control_active)
      .Color(ImGuiCol_SliderGrab, CurrentTheme().accent)
      .Color(ImGuiCol_SliderGrabActive, CurrentTheme().accent_hover)
      .Color(ImGuiCol_CheckMark, CurrentTheme().accent)
      .Color(ImGuiCol_PopupBg, CurrentTheme().bg_card)
      .Color(ImGuiCol_TextLink, CurrentTheme().accent)
      .Color(ImGuiCol_TextSelectedBg, CurrentTheme().text_selection)
      .Color(ImGuiCol_ScrollbarBg, CurrentTheme().bg_window)
      .Color(ImGuiCol_ScrollbarGrab, CurrentTheme().node_border)
      .Color(ImGuiCol_ScrollbarGrabHovered, CurrentTheme().connector)
      .Color(ImGuiCol_ScrollbarGrabActive, CurrentTheme().accent)
      .Color(ImGuiCol_SeparatorHovered, CurrentTheme().accent_hover)
      .Color(ImGuiCol_SeparatorActive, CurrentTheme().accent)
      .Color(ImGuiCol_TableHeaderBg, CurrentTheme().bg_control)
      .Color(ImGuiCol_TableBorderStrong, CurrentTheme().stroke)
      .Color(ImGuiCol_TableBorderLight, CurrentTheme().stroke)
      .Color(ImGuiCol_TableRowBg, tokens::Hex(0, 0.f))
      .Color(ImGuiCol_TableRowBgAlt, CurrentTheme().bg_card)
      .Color(ImGuiCol_NavCursor, CurrentTheme().accent)
      .Color(ImGuiCol_InputTextCursor, CurrentTheme().text_primary)
      .Color(ImGuiCol_PlotLines, CurrentTheme().connector)
      .Color(ImGuiCol_PlotLinesHovered, CurrentTheme().accent)
      .Color(ImGuiCol_PlotHistogram, CurrentTheme().accent)
      .Color(ImGuiCol_PlotHistogramHovered, CurrentTheme().accent_hover)
      .Var(ImGuiStyleVar_DisabledAlpha, 0.65f)
      .Var(ImGuiStyleVar_FrameRounding, kRadiusControl)
      .Var(ImGuiStyleVar_FrameBorderSize, CurrentTheme().control_border_size)
      .Var(ImGuiStyleVar_GrabRounding, kRadiusSmall)
      .Var(ImGuiStyleVar_ChildRounding, kRadiusCard)
      .Var(ImGuiStyleVar_PopupRounding, kRadiusControl)
      .Var(ImGuiStyleVar_ItemSpacing, ImVec2(kRowGap, kRowGap))
      .Var(ImGuiStyleVar_FramePadding, ImVec2(8.f, 4.f));
}

}  // namespace renodx::addons::dlss5::ui
