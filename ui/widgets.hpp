/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// The status card, drawn (PLAN_UI_V7.md 1 and 3).
//
// state_card.hpp decides the card; this draws it: a rounded card with a
// stripe in the card's tone down its left edge, the glyph and title at the
// title size, the body, the counts line, the path note, the numbered fix
// steps and the card's button.  Nothing here decides anything, so a card the
// unit tests have pinned is the card the player sees.  Its text is drawn in
// the panel's language (i18n.hpp); the card itself stays English.
//
// Include after <imgui.h> and reshade.hpp, like theme.hpp.

#pragma once

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "state_card.hpp"
#include "strings.hpp"
#include "theme.hpp"

namespace renodx::addons::dlss5::ui {

// i18n: begin - display strings (tools/i18n/ui_strings.py catalog)

// ImGui::SetItemTooltip, with the text wrapped at kTooltipEm.  The one way
// the add-on shows a tooltip.
inline void ItemTooltip(const char* fmt, ...) {
  if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) return;
  StyleScope tooltip;
  tooltip.Color(ImGuiCol_Text, CurrentTheme().text_primary);
  ImGui::BeginTooltip();
  ImGui::PushTextWrapPos((std::min)(ImGui::GetFontSize() * 28.f, ImGui::GetIO().DisplaySize.x * 0.65f));
  va_list args;
  va_start(args, fmt);
  ImGui::TextV(fmt, args);
  va_end(args);
  ImGui::PopTextWrapPos();
  ImGui::EndTooltip();
}

inline ImVec4 ToneColor(CardTone tone) {
  switch (tone) {
    case CardTone::kGood: return CurrentTheme().good;
    case CardTone::kWarn: return CurrentTheme().warn;
    case CardTone::kBad: return CurrentTheme().bad;
    case CardTone::kIdle: return CurrentTheme().idle;
  }
  return CurrentTheme().idle;
}

// The tone as 8-bit RGB, for the capture lane's pixel count.
inline std::uint32_t ToneRgb(CardTone tone) {
  const ImVec4 color = ToneColor(tone);
  const auto channel = [](float value) {
    return static_cast<std::uint32_t>(value * 255.f + 0.5f);
  };
  return (channel(color.x) << 16) | (channel(color.y) << 8) | channel(color.z);
}

inline const char* GlyphIcon(CardGlyph glyph) {
  switch (glyph) {
    case CardGlyph::kCheck: return icons::kCheck;
    case CardGlyph::kWarning: return icons::kWarning;
    case CardGlyph::kCross: return icons::kCross;
    case CardGlyph::kIdle: return icons::kCircle;
    case CardGlyph::kWaiting: return icons::kHourglass;
    case CardGlyph::kInfo: return icons::kInfo;
  }
  return icons::kCircle;
}

inline const char* ActionLabel(CardAction action) {
  switch (action) {
    case CardAction::kTurnOn: return Tr("Turn on");
    case CardAction::kOpenDiagnostics: return Tr("Open Diagnostics");
    case CardAction::kDismiss: return Tr("Got it");
    case CardAction::kNone: break;
  }
  return nullptr;
}

inline constexpr float kStripeWidth = 4.f;

inline const char* FrameGenText(const StatusCard& card) {
  if (card.frame_gen_multiplier <= 1) {
    return card.observed_fg_route == FgRoute::kBeforeFg ? text::kFgBeforeUnknown
           : card.fg_declared                           ? text::kFgDeclared
                                                        : text::kFgStatusUnknown;
  }
  switch (card.observed_fg_route) {
    case FgRoute::kBeforeFg:         return text::kFrameGenBeforeFg;
    case FgRoute::kEveryPresent:
    case FgRoute::kFallbackPresent:  return text::kFrameGenEvery;
    case FgRoute::kGameFrames:       return text::kFrameGenGameFrames;
    case FgRoute::kFallbackUpscaled: return text::kFrameGenFallback;
    default:                         return text::kFrameGenUnconfirmed;
  }
}

// Keep complete UTF-8 characters when a single-line summary or label is shortened.
inline std::string Ellipsize(const char* text, float width) {
  std::string shown(text);
  if (ImGui::CalcTextSize(text).x <= width) return shown;
  while (!shown.empty() && ImGui::CalcTextSize((shown + "...").c_str()).x > width) {
    while (!shown.empty() && (static_cast<unsigned char>(shown.back()) & 0xC0) == 0x80) {
      shown.pop_back();
    }
    if (!shown.empty()) shown.pop_back();
  }
  return shown + "...";
}

inline bool ProfileSlotButton(const char* id, const char* label, ImVec2 size, bool selected) {
  StyleScope style;
  if (selected) {
    style.Color(ImGuiCol_Button, CurrentTheme().accent)
        .Color(ImGuiCol_ButtonHovered, CurrentTheme().accent_hover)
        .Color(ImGuiCol_ButtonActive, CurrentTheme().accent_active)
        .Color(ImGuiCol_Text, CurrentTheme().on_accent);
  }
  bool pressed;
  {
    StyleScope hidden;
    hidden.Color(ImGuiCol_Text, ImVec4(0.f, 0.f, 0.f, 0.f));
    pressed = ImGui::Button(id, size);
  }
  const ImVec2 begin = ImGui::GetItemRectMin();
  const ImVec2 end = ImGui::GetItemRectMax();
  const std::string shown = Ellipsize(label, end.x - begin.x - ImGui::GetStyle().FramePadding.x * 2.f);
  const ImVec2 text = ImGui::CalcTextSize(shown.c_str());
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->PushClipRect(begin, end, true);
  draw->AddText(ImVec2(begin.x + (end.x - begin.x - text.x) * 0.5f,
                       begin.y + (end.y - begin.y - text.y) * 0.5f),
                ImGui::GetColorU32(ImGuiCol_Text), shown.c_str());
  draw->PopClipRect();
  return pressed;
}

struct ProcessingChainLine {
  std::string text;
  ImVec2 size;
};

struct ProcessingChainNode {
  std::vector<ProcessingChainLine> lines;
  float content_height = 0.f;
};

// Measure the actual wrapped lines, rather than centering a multiline button's
// widest bounding box. Use ImGui's exported measurement API for ReShade hosts.
inline ProcessingChainNode MeasureProcessingChainNode(const std::array<std::string, 3>& text, float width) {
  ProcessingChainNode node;
  const float available = (std::max)(1.f, width - ImGui::GetStyle().FramePadding.x * 2.f);
  for (const auto& section : text) {
    size_t begin = 0;
    do {
      size_t end = begin;
      size_t word_break = begin;
      while (end < section.size() && section[end] != '\n') {
        size_t next = end + 1;
        while (next < section.size() && (static_cast<unsigned char>(section[next]) & 0xC0) == 0x80) {
          ++next;
        }
        if (end > begin && ImGui::CalcTextSize(section.data() + begin, section.data() + next, false).x > available) {
          if (word_break > begin) end = word_break;
          break;
        }
        if (section[end] == ' ') word_break = end;
        end = next;
      }
      std::string line = section.substr(begin, end - begin);
      const ImVec2 size = ImGui::CalcTextSize(line.c_str(), nullptr, false);
      node.lines.push_back({.text = std::move(line), .size = size});
      node.content_height += size.y;
      begin = end;
      if (begin < section.size() && section[begin] == '\n') ++begin;
      while (begin < section.size() && section[begin] == ' ') ++begin;
    } while (begin < section.size());
  }
  node.content_height += ImGui::GetStyle().ItemInnerSpacing.y * (node.lines.size() - 1);
  return node;
}

inline bool ProcessingChainButton(const ProcessingChainNode& node, ImVec2 size, bool selected) {
  StyleScope style;
  style.Color(ImGuiCol_Button, selected ? CurrentTheme().node_selected : CurrentTheme().bg_card)
      .Color(ImGuiCol_Border, selected ? CurrentTheme().accent : CurrentTheme().node_border)
      .Var(ImGuiStyleVar_FrameBorderSize, selected ? 2.f : 1.f);
  // Keep a real button for keyboard navigation, hit testing and tooltips.
  const bool pressed = ImGui::Button("###chain_pass", size);
  const ImVec2 begin = ImGui::GetItemRectMin();
  const ImVec2 end = ImGui::GetItemRectMax();
  float y = begin.y + (size.y - node.content_height) * 0.5f;
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->PushClipRect(begin, end, true);
  for (const auto& line : node.lines) {
    draw->AddText(ImVec2(begin.x + (size.x - line.size.x) * 0.5f, y),
                  ImGui::GetColorU32(ImGuiCol_Text), line.text.c_str());
    y += line.size.y + ImGui::GetStyle().ItemInnerSpacing.y;
  }
  draw->PopClipRect();
  return pressed;
}

struct StatusSummaryFields {
  // Already formatted snapshot values. No runtime state is read by this widget.
  std::string resolution;
  std::string stage;
  std::string gpu;
  std::string fps;
  std::string fg;
  const char* fps_tip = "ReShade overlay cadence. This is not a separate measurement of game render FPS or generated output FPS.";
  const char* fg_tip = "Observed frame generation route. FG GPU duration is unavailable.";
  const char* resolution_tip = "Configured working ratio and current completed working dimensions when available.";
  const char* stage_tip = "Execution evidence for the current chain. A requested stage is not proof of execution.";
  const char* gpu_tip = "Completed GPU timestamp samples only. Missing or stale samples are not current execution cost.";
};

// Only the compact child participates in settings layout. The caller opens
// complete runtime details without expanding the summary's default height.
inline bool DrawStatusSummary(const StatusCard& card, unsigned warning_count, const StatusSummaryFields& fields) {
  StyleScope frame;
  frame.FontSize(BodyFontSize())
      .Color(ImGuiCol_ChildBg, CurrentTheme().bg_card)
      .Var(ImGuiStyleVar_WindowPadding, ImVec2(tokens::kCardPadding + kStripeWidth, tokens::kCardPadding));
  const float font = ImGui::GetFontSize();
  const float available = (std::max)(1.f, ImGui::GetContentRegionAvail().x - ImGui::GetStyle().WindowPadding.x * 2.f);
  const float gap = ImGui::GetStyle().ItemSpacing.x;
  const float action_width = (std::min)(available, ImGui::CalcTextSize((std::string(Tr("Runtime monitor")) + " !").c_str()).x
      + ImGui::GetStyle().FramePadding.x * 2.f);
  const float title_width = (std::max)(1.f, available - action_width - gap);
  const bool auxiliary = available >= font * 56.f;
  const int columns = (available >= font * 28.f ? 3 : 2);
  const float field_width = (available - gap * (columns - 1)) / columns;
  const float heading_height = ImGui::GetFrameHeight();
  const float height = ImGui::GetStyle().WindowPadding.y * 2.f + heading_height
      + ImGui::GetStyle().ItemSpacing.y + ImGui::GetTextLineHeight()
      + (auxiliary ? ImGui::GetTextLineHeightWithSpacing() : 0.f);
  char title[256];
  FormatCardTitle(title, sizeof(title), card);
  const char* short_title = title;
  switch (card.id) {
    case CardId::kOffByUser: short_title = Tr("NR disabled"); break;
    case CardId::kActive: short_title = Tr(card.fg_submitted_frames.has_value() ? "NR submitted before FG" : "NR running normally"); break;
    case CardId::kActiveWithIssues: short_title = Tr("Active with warnings"); break;
    case CardId::kPartlyActive: short_title = Tr("Partly active"); break;
    case CardId::kStarting: short_title = Tr(card.tone == CardTone::kWarn ? "Needs attention" : "Checking NR"); break;
    default: break;
  }
  bool details = false;
  if (ImGui::BeginChild("##nr_status_summary", ImVec2(0.f, height), ImGuiChildFlags_AlwaysUseWindowPadding,
                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
    const ImVec2 origin = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddRectFilled(origin, ImVec2(origin.x + kStripeWidth, origin.y + ImGui::GetWindowSize().y),
        ToU32(ToneColor(card.tone)), tokens::kRadiusCard, ImDrawFlags_RoundCornersLeft);
    const ImVec2 heading_at = ImGui::GetCursorPos();
    ImGui::AlignTextToFramePadding();
    {
      StyleScope tone;
      tone.Color(ImGuiCol_Text, ToneColor(card.tone));
      const std::string heading = std::string(GlyphIcon(card.glyph)) + " " + short_title;
      ImGui::TextUnformatted(Ellipsize(heading.c_str(), title_width).c_str());
      ItemTooltip("%s\n%s", title, card.body != nullptr ? Tr(card.body) : "");
    }
    ImGui::SetCursorPos(ImVec2(heading_at.x + available - action_width, heading_at.y));
    const std::string action = std::string(Tr("Runtime monitor")) + (warning_count != 0 ? " !" : "");
    details = ImGui::Button((action + "###status_details").c_str(), ImVec2(action_width, 0.f));
    ItemTooltip("%s", Tr("Open the movable runtime monitor. Diagnostics remain available separately."));
    const float metric_y = heading_at.y + heading_height + ImGui::GetStyle().ItemSpacing.y;
    const char* names[] = {"NR GPU", "ReShade FPS", "FG"};
    const std::string* values[] = {&fields.gpu, &fields.fps, &fields.fg};
    const char* tips[] = {fields.gpu_tip, fields.fps_tip, fields.fg_tip};
    for (int column = 0; column < columns; ++column) {
      const int index = (columns == 2 && column == 1 && fields.fps.empty() ? 2 : column);
      const std::string metric = std::string(Tr(names[index])) + "  "
          + (values[index]->empty() ? "—" : values[index]->c_str());
      ImGui::SetCursorPos(ImVec2(heading_at.x + column * (field_width + gap), metric_y));
      const char* value = values[index]->empty() ? "—" : values[index]->c_str();
      const float value_width = ImGui::CalcTextSize(value).x;
      const auto label = Ellipsize(Tr(names[index]), (std::max)(0.f, field_width - value_width - font * 0.4f));
      const std::string shown = label.empty() ? value : label + " " + value;
      ImGui::TextUnformatted(Ellipsize(shown.c_str(), field_width).c_str());
      ItemTooltip("%s\n%s", metric.c_str(), tips[index] != nullptr ? Tr(tips[index]) : "");
    }
    if (auxiliary) {
      const float width = (available - gap) * 0.5f;
      const char* labels[] = {"Working resolution", "Execution stage"};
      const std::string* values[] = {&fields.resolution, &fields.stage};
      const char* tips[] = {fields.resolution_tip, fields.stage_tip};
      StyleScope muted;
      muted.Color(ImGuiCol_Text, CurrentTheme().text_secondary);
      for (int column = 0; column < 2; ++column) {
        const std::string text = std::string(Tr(labels[column])) + "  "
            + (values[column]->empty() ? "—" : values[column]->c_str());
        ImGui::SetCursorPos(ImVec2(heading_at.x + column * (width + gap), metric_y + ImGui::GetTextLineHeightWithSpacing()));
        ImGui::TextUnformatted(Ellipsize(text.c_str(), width).c_str());
        ItemTooltip("%s\n%s", text.c_str(), tips[column] != nullptr ? Tr(tips[column]) : "");
      }
    }
  }
  ImGui::EndChild();
  return details;
}

inline bool DrawStatusSummary(const StatusCard& card, unsigned warning_count, const char* execution = nullptr) {
  StatusSummaryFields fields;
  const bool off = card.id == CardId::kOffByUser;
  if (!off && card.input_width != 0 && card.input_height != 0 && card.output_width != 0 && card.output_height != 0) {
    char sizes[96];
    std::snprintf(sizes, sizeof(sizes), "%ux%u -> %ux%u", card.input_width, card.input_height,
                  card.output_width, card.output_height);
    fields.resolution = sizes;
  }
  if (!off && execution != nullptr) fields.stage = execution;
  if (!off && card.nr_gpu_ms > 0.f) {
    char gpu[96];
    std::snprintf(gpu, sizeof(gpu), "%.1f ms", card.nr_gpu_ms);
    fields.gpu = gpu;
  }
  fields.fg = Tr(card.fg_declared || card.frame_gen_multiplier > 1 ? "On" : "Unknown");
  return DrawStatusSummary(card, warning_count, fields);
}

// The glyph in the card's tone and the title, at the title size: the card's
// first line, and all of the HUD pill but its hint.
inline void CardHeading(const StatusCard& card) {
  {
    StyleScope title;
    title.FontSize(TitleFontSize()).Color(ImGuiCol_Text, ToneColor(card.tone));
    ImGui::TextUnformatted(GlyphIcon(card.glyph));
  }
  ImGui::SameLine();
  StyleScope title;
  title.FontSize(TitleFontSize());
  char text[256];
  FormatCardTitle(text, sizeof(text), card);
  ImGui::TextUnformatted(text);
}

// Draws the card; returns true on the frame its button was pressed.
inline bool DrawStatusCard(const StatusCard& card, bool show_metrics = true) {
  using namespace tokens;
  bool pressed = false;

  StyleScope frame;
  frame.Color(ImGuiCol_ChildBg, CurrentTheme().bg_card)
      .Var(ImGuiStyleVar_WindowPadding,
           ImVec2(kCardPadding + kStripeWidth, kCardPadding));
  const bool visible = ImGui::BeginChild(
      "##nr_status_card", ImVec2(0.f, 0.f),
      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
  if (visible) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    draw->AddRectFilled(origin,
                        ImVec2(origin.x + kStripeWidth, origin.y + size.y),
                        ToU32(ToneColor(card.tone)), kRadiusCard,
                        ImDrawFlags_RoundCornersLeft);

    // Before the title: a long title in a narrow docked panel wraps inside
    // the card instead of running past its edge.
    ImGui::PushTextWrapPos(0.f);
    CardHeading(card);
    ImGui::TextUnformatted(Tr(card.body));
    char detail[160];
    const bool has_detail = show_metrics && FormatCardDetail(detail, sizeof(detail), card) > 0;
    char cost[256];
    const bool has_cost =
        show_metrics && FormatNrCost(cost, sizeof(cost), card.nr_gpu_ms, card.nr_present_ms, card.frame_ms, true) > 0;
    const bool frame_gen = show_metrics && (card.frame_gen_multiplier > 1 || card.fg_declared || card.observed_fg_route != FgRoute::kUnknown);
    if (has_detail || (show_metrics && card.path_note != nullptr) || has_cost || frame_gen) {
      StyleScope secondary;
      secondary.FontSize(SmallFontSize()).Color(ImGuiCol_Text, CurrentTheme().text_secondary);
      if (has_detail) ImGui::TextUnformatted(detail);
      if (show_metrics && card.path_note != nullptr) ImGui::TextUnformatted(Tr(card.path_note));
      // The Present hook point with frame generation: its multiplier, and
      // whether the generated frames get NR too.
      if (frame_gen) {
        ImGui::Text(Tr(FrameGenText(card)),
                    card.frame_gen_multiplier);
      }
      if (has_cost) {
        ImGui::TextUnformatted(cost);
        ItemTooltip("%s", Tr(text::kNrCostTip));
      }
    }
    // The flicker at Present, in the warning colour with its most frequent
    // reason: the one Present counter a player has to see.
    if (card.present_gaps != 0) {
      StyleScope warn;
      warn.Color(ImGuiCol_Text, CurrentTheme().warn);
      ImGui::Text(Tr(text::kPresentGapsCard),
                  static_cast<unsigned long long>(card.present_gaps),
                  card.gap_reason != nullptr ? Tr(card.gap_reason) : Tr(text::kNoneYet));
    }
    for (std::size_t step = 0; step < card.fix_count; ++step) {
      if (card.fix[step] == nullptr) continue;
      ImGui::Text("%u. %s", static_cast<unsigned>(step + 1), Tr(card.fix[step]));
    }
    ImGui::PopTextWrapPos();

    if (const char* label = ActionLabel(card.action); label != nullptr) {
      StyleScope button;
      button.Color(ImGuiCol_Button, CurrentTheme().accent)
          .Color(ImGuiCol_ButtonHovered, CurrentTheme().accent_hover)
          .Color(ImGuiCol_ButtonActive, CurrentTheme().accent_active)
          .Color(ImGuiCol_Text, CurrentTheme().on_accent);
      pressed = ImGui::Button(label);
    }
  }
  ImGui::EndChild();
  return pressed;
}

// ---- the modern layout's controls (U2; rows reworked in alpha46) -----------
//
// Positions here are screen positions: the panel is a one-column table (the
// width cap), and SameLine's window-relative offsets are shifted by a table
// cell's own offset.

// The reset slot of the row RowLabel started, as a screen x.
inline float row_slot_x = 0.f;

// Test-only (RENODX_NR_TEST_UI_EXPAND): every section header opens, so a
// capture lane draws the collapsed sections' controls too.
inline bool expand_all_sections = false;

// This frame's count of drawn reset buttons: the capture line states it
// (resets=), so a lane can tell a changed setting's row from a stock one.
inline int reset_buttons_drawn = 0;

// A settings row: the label in the secondary colour in a fixed-width left
// column, then the control, then a reset slot one frame high at the end.
// The slot is reserved whether or not the row shows its reset button, so
// every control on the panel starts and ends at the same x.  A label longer
// than its column ends in "..." and shows in full on hover, and a row's
// `tooltip` shows over its label as well as over its control.  On a panel
// narrower than kStackEm the label takes its own line and the control the
// panel's width under it, a little apart from the row above.  Call right
// before a control whose own label is hidden ("##id"); the next item's width
// is the control column.  Both texts are English; the row draws them in the
// panel's language.
inline void RowLabel(const char* label, const char* tooltip = nullptr) {
  label = Tr(label);
  tooltip = Tr(tooltip);
  const ImGuiStyle& style = ImGui::GetStyle();
  const float avail = ImGui::GetContentRegionAvail().x;
  const float font = ImGui::GetFontSize();
  const bool stacked = avail < font * tokens::kStackEm;
  const float column =
      stacked ? 0.f : (std::min)(font * tokens::kLabelEm, avail * tokens::kLabelShare);
  if (stacked) ImGui::Dummy(ImVec2(0.f, 0.f));
  const ImVec2 at = ImGui::GetCursorScreenPos();
  row_slot_x = at.x + avail - ImGui::GetFrameHeight();
  const float fit = stacked ? avail : column - style.ItemSpacing.x;
  const bool clipped = ImGui::CalcTextSize(label).x > fit;
  if (!stacked) ImGui::AlignTextToFramePadding();
  // A stacked row without a label ("Custom" under Resolution) draws no
  // empty line; a side-by-side one keeps its label cell for SameLine.
  if (!stacked || *label != '\0') {
    {
      StyleScope secondary;
      secondary.Color(ImGuiCol_Text, CurrentTheme().text_secondary);
      ImGui::TextUnformatted(Ellipsize(label, fit).c_str());
    }
    if (clipped && tooltip != nullptr) {
      ItemTooltip("%s\n%s", label, tooltip);
    } else if (clipped || tooltip != nullptr) {
      ItemTooltip("%s", clipped ? label : tooltip);
    }
  }
  if (!stacked) {
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(at.x + column, ImGui::GetCursorScreenPos().y));
  }
  ImGui::SetNextItemWidth(row_slot_x - style.ItemSpacing.x - (at.x + column));
}

// SameLine when an item `width` wide still fits after the last one, so a
// pair of buttons wraps onto two lines on a narrow panel instead of running
// off its edge.
inline void SameLineIfFits(float width) {
  const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
  if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width <= right) {
    ImGui::SameLine();
  }
}

// Native controls keep their values and keyboard interaction; placing long
// labels above them lets the flat Developer view fit a narrow ReShade dock.
inline std::string ControlLabel(const char* label) {
  ImGui::TextWrapped("%s", label);
  ImGui::SetNextItemWidth(-1.f);
  return std::string("##") + label;
}

inline bool Checkbox(const char* label, bool* value) {
  if (ImGui::CalcTextSize(label).x + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x
      <= ImGui::GetContentRegionAvail().x) {
    return ImGui::Checkbox(label, value);
  }
  ImGui::BeginGroup();
  bool changed = ImGui::Checkbox((std::string("##") + label).c_str(), value);
  ImGui::SameLine();
  ImGui::TextWrapped("%s", label);
  if (ImGui::IsItemClicked()) {
    *value = !*value;
    changed = true;
  }
  ImGui::EndGroup();
  return changed;
}

// Native button interaction with a fully wrapped action label. Long repair
// actions must remain readable at large font sizes in a narrow dock.
inline bool ActionButton(const char* label, const char* id = nullptr) {
  const ImVec2 padding = ImGui::GetStyle().FramePadding;
  const float width = (std::min)(ImGui::GetContentRegionAvail().x,
                                 ImGui::CalcTextSize(label).x + padding.x * 2.f);
  const float wrap_width = (std::max)(1.f, width - padding.x * 2.f);
  const ImVec2 text_size = ImGui::CalcTextSize(label, nullptr, false, wrap_width);
  const bool clicked = ImGui::Button((std::string("###") + (id != nullptr ? id : label)).c_str(),
                                     ImVec2(width, text_size.y + padding.y * 2.f));
  const ImVec2 origin = ImGui::GetItemRectMin();
  ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                                      ImVec2(origin.x + (width - text_size.x) * 0.5f, origin.y + padding.y),
                                      ImGui::GetColorU32(ImGuiCol_Text), label, nullptr, wrap_width);
  return clicked;
}

// Player commands are one-shot epochs, separate from capture-only forcing.
// Scopes separate pages and their mode-specific controls. Hidden descendants
// receive a pending command when reached; later individual toggles remain free.
struct FoldState {
  unsigned epoch = 0;
  bool seen = false;
};
struct FoldScope {
  unsigned epoch = 0;
  bool target = false;
  std::unordered_map<ImGuiID, FoldState> nodes;
};
inline std::array<FoldScope, 32> fold_scopes;
inline int fold_scope = -1;
inline void PrepareFold(const char* id, bool changed = false) {
  if (expand_all_sections) {
    ImGui::SetNextItemOpen(true);
    return;
  }
  if (fold_scope < 0) return;
  FoldScope& scope = fold_scopes[fold_scope];
  FoldState& state = scope.nodes[ImGui::GetID(id)];
  if (state.epoch != scope.epoch) {
    ImGui::SetNextItemOpen(scope.target);
    state.epoch = scope.epoch;
  } else if (!state.seen && changed && scope.epoch == 0) {
    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
  }
  state.seen = true;
}
inline bool TreeNode(const char* id, const char* title, ImGuiTreeNodeFlags flags = 0, bool changed = false) {
  PrepareFold(id, changed);
  const float width = (std::max)(0.f, ImGui::GetContentRegionAvail().x
                                          - ImGui::GetFontSize() - ImGui::GetStyle().FramePadding.x * 3.f);
  const std::string caption = std::string(title) + (changed ? std::string(" (") + Tr(text::kChanged) + ")" : "");
  const std::string shown = Ellipsize(caption.c_str(), width);
  const bool open = ImGui::TreeNodeEx(id, flags | ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth,
                                      "%s", shown.c_str());
  if (open) {
    const ImVec2 begin = ImGui::GetItemRectMin();
    const ImVec2 end = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRectFilled(begin, ImVec2(begin.x + 2.f, end.y), ToU32(CurrentTheme().accent));
  }
  if (shown != caption) {
    ItemTooltip("%s", caption.c_str());
  }
  return open;
}

// The reset button in the row's slot: an undo arrow, drawn only for a
// setting that differs from its default (the caller decides).  `id` keeps
// the buttons apart; `default_text` names what a click restores.  Returns
// true when clicked.
inline bool ResetButton(const char* id, const char* default_text) {
  ++reset_buttons_drawn;
  ImGui::SameLine();
  ImGui::SetCursorScreenPos(ImVec2(row_slot_x, ImGui::GetCursorScreenPos().y));
  bool clicked = false;
  {
    StyleScope flat;
    flat.Color(ImGuiCol_Button, CurrentTheme().bg_control)
        .Color(ImGuiCol_Text, CurrentTheme().accent)
        .Var(ImGuiStyleVar_FramePadding, ImVec2(0.f, ImGui::GetStyle().FramePadding.y));
    ImGui::PushID(id);
    clicked = ImGui::Button(icons::kUndo, ImVec2(ImGui::GetFrameHeight(), 0.f));
    ImGui::PopID();
  }
  ItemTooltip(Tr("Reset to default (%s)"), Tr(default_text));
  return clicked;
}

// A slider drawn the modern way: a thin rail filled up to the value, a round
// knob, and the value in its own column on the right, so the number never
// sits under the knob (alpha45 drew Strength's 1.00 under the grab).  ImGui's
// slider still runs underneath, invisible, so dragging, keyboard and gamepad
// steps and Ctrl+click typing behave as before; while it is being typed
// into, it is drawn as ImGui draws it.  Call after RowLabel.  Returns true
// on the frames *value changed.
inline bool Slider(const char* id, float* value, float min, float max,
                   const char* format, const char* tooltip) {
  static ImGuiID typing = 0;
  format = Tr(format);
  tooltip = Tr(tooltip);
  const ImGuiID item = ImGui::GetID(id);
  const bool was_typing = typing == item;
  const float height = ImGui::GetFrameHeight();
  const float knob = height * 0.32f;
  char text[32];
  std::snprintf(text, sizeof(text), format, *value);
  const float text_width = ImGui::CalcTextSize(text).x;
  const float value_width = (std::max)(ImGui::GetFontSize() * tokens::kValueEm,
                                        text_width + ImGui::GetStyle().ItemSpacing.x);
  const float width = ImGui::CalcItemWidth();
  bool changed = false;
  {
    StyleScope hidden;
    // ImGui centres a grab of GrabMinSize on the value: the knob's diameter,
    // so the knob drawn below sits where the mouse drags it.
    hidden.Var(ImGuiStyleVar_GrabMinSize, knob * 2.f);
    if (!was_typing) {
      const ImVec4 clear(0.f, 0.f, 0.f, 0.f);
      hidden.Color(ImGuiCol_FrameBg, clear)
          .Color(ImGuiCol_FrameBgHovered, clear)
          .Color(ImGuiCol_FrameBgActive, clear)
          .Color(ImGuiCol_SliderGrab, clear)
          .Color(ImGuiCol_SliderGrabActive, clear)
          .Color(ImGuiCol_Text, clear);
    }
    ImGui::SetNextItemWidth(was_typing ? width : (std::max)(width - value_width, height * 2.f));
    changed = ImGui::SliderFloat(id, value, min, max, format);
  }
  if (tooltip != nullptr) ItemTooltip("%s", tooltip);
  const bool active = ImGui::IsItemActive();
  // A Ctrl+click turns ImGui's slider into a text field; the text field
  // raises WantTextInput from the next frame on.
  if (active && ImGui::GetIO().WantTextInput) {
    typing = item;
  } else if (was_typing && !active) {
    typing = 0;
  }
  if (was_typing) return changed;

  const ImVec2 lo = ImGui::GetItemRectMin();
  const ImVec2 hi = ImGui::GetItemRectMax();
  // ImGui's own grab geometry: 2 px of padding, then half the grab.
  const float x0 = lo.x + 2.f + knob;
  const float x1 = hi.x - 2.f - knob;
  const float t = max > min ? std::clamp((*value - min) / (max - min), 0.f, 1.f) : 0.f;
  const float x = x0 + t * (x1 - x0);
  const float y = (lo.y + hi.y) * 0.5f;
  const float rail = (std::max)(2.f, height * 0.14f);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(ImVec2(x0, y - rail * 0.5f), ImVec2(x1, y + rail * 0.5f),
                      ToU32(CurrentTheme().slider_rail), rail);
  draw->AddRectFilled(ImVec2(x0, y - rail * 0.5f), ImVec2(x, y + rail * 0.5f),
                      ToU32(CurrentTheme().accent), rail);
  if (active || ImGui::IsItemHovered()) {
    draw->AddCircleFilled(ImVec2(x, y), knob + 3.f, ToU32(ImVec4(CurrentTheme().accent.x,
                          CurrentTheme().accent.y, CurrentTheme().accent.z, 0.35f)));
  }
  draw->AddCircleFilled(ImVec2(x, y), knob, ToU32(CurrentTheme().slider_knob));

  ImGui::SameLine(0.f, 0.f);
  ImGui::SetCursorScreenPos(
      ImVec2(hi.x + value_width - text_width, ImGui::GetCursorScreenPos().y));
  ImGui::TextUnformatted(text);
  return changed;
}

// Static captions have no disclosure arrow or interactive background.
inline void SubCaption(const char* caption) {
  StyleScope caption_text;
  caption_text.FontSize(SmallFontSize()).Color(ImGuiCol_Text, CurrentTheme().text_primary);
  ImGui::TextUnformatted(Tr(caption));
}

// A link at the right end of the current line (screen x `right`).  Returns
// true when clicked.
inline bool RightLink(const char* label, float right) {
  ImGui::SameLine();
  ImGui::SetCursorScreenPos(
      ImVec2(right - ImGui::CalcTextSize(label).x, ImGui::GetCursorScreenPos().y));
  return ImGui::TextLink(label);
}

// An always-open section's caption: small text in the primary colour
// over a hairline, and at the right end a link that resets the section,
// shown while `modified`.  Returns true on the frame the link was clicked.
inline bool SectionCaption(const char* caption, const char* reset_label, bool modified) {
  StyleScope caption_font;
  caption_font.FontSize(ModuleFontSize());
  const char* title = Tr(caption);
  reset_label = Tr(reset_label);
  ImGui::Dummy(ImVec2(0.f, tokens::kRowGap * 0.5f));
  const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
  const float width = ImGui::GetContentRegionAvail().x;
  const bool words = !modified || ImGui::CalcTextSize(title).x + ImGui::CalcTextSize(reset_label).x + ImGui::GetFontSize() <= width;
  const char* shown_reset = (words ? reset_label : icons::kUndo);
  const float title_width = width - (modified ? ImGui::CalcTextSize(shown_reset).x + ImGui::GetFontSize() : 0.f);
  bool clicked = false;
  {
    StyleScope caption_text;
    {
      StyleScope secondary;
      secondary.Color(ImGuiCol_Text, CurrentTheme().text_primary);
      ImGui::TextUnformatted(Ellipsize(title, title_width).c_str());
      ItemTooltip("%s", title);
    }
    if (modified) {
      clicked = RightLink(shown_reset, right);
      ItemTooltip("%s", reset_label);
    }
  }
  ImGui::Separator();
  return clicked;
}

// The gap between two sections: kSectionGap in all, counting the item
// spacing ImGui adds before and after the spacer.
inline void SectionGap() {
  ImGui::Dummy(ImVec2(
      0.f, (std::max)(0.f, tokens::kSectionGap - 2.f * ImGui::GetStyle().ItemSpacing.y)));
}

// A collapsible section: ImGui's header row, a little taller, a hint in the
// secondary colour after the title, and the same reset link at the right end
// while the section holds a changed setting.  A header too narrow for the
// link's words after the title shows the undo arrow instead, with the words
// on hover; the hint shows only while enough of it fits to say something.
// Returns whether the section is open; *reset is true on the frame the link
// was clicked.  The header's id is its English title, so a section keeps
// its open state across a language change.
inline bool SectionHeader(const char* english, const char* hint, const char* reset_label,
                          bool modified, bool* reset) {
  const std::string caption = std::string(Tr(english)) + (modified ? std::string(" (") + Tr(text::kChanged) + ")" : "");
  const char* title = caption.c_str();
  hint = Tr(hint);
  reset_label = Tr(reset_label);
  // The header's vertical padding, which is also where the hint's baseline
  // sits: the two must match or the hint drifts off the title line.
  constexpr float kPadY = 7.f;
  const float pad = tokens::kCardPadding * 0.75f;
  const float font = ImGui::GetFontSize();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float right = at.x + ImGui::GetContentRegionAvail().x - pad;
  // ImGui draws a framed header's title after the arrow: one font size plus
  // three paddings in.
  const float full_title_end = at.x + font + pad * 3.f + ImGui::CalcTextSize(title).x;
  const bool reset_words = reset_label != nullptr
                           && full_title_end + font + ImGui::CalcTextSize(reset_label).x <= right;
  const char* shown_reset = (reset_words ? reset_label : icons::kUndo);
  const float reserved = (modified && reset_label != nullptr ? ImGui::CalcTextSize(shown_reset).x + font : 0.f);
  const std::string shown_title = Ellipsize(title, (std::max)(0.f, right - at.x - font - pad * 3.f - reserved));
  const float title_end = at.x + font + pad * 3.f + ImGui::CalcTextSize(shown_title.c_str()).x;
  bool open = false;
  PrepareFold((std::string("###") + english).c_str(), modified);
  {
    StyleScope tall;
    tall.Var(ImGuiStyleVar_FramePadding, ImVec2(pad, kPadY));
    open = ImGui::CollapsingHeader((shown_title + "###" + english).c_str(),
                                   ImGuiTreeNodeFlags_AllowOverlap);
  }
  if (shown_title != title) {
    ItemTooltip("%s", title);
  }
  const ImVec2 lo = ImGui::GetItemRectMin();
  if (open) {
    ImGui::GetWindowDrawList()->AddRectFilled(lo, ImVec2(lo.x + 2.f, ImGui::GetItemRectMax().y), ToU32(CurrentTheme().accent));
  }
  float hint_end = right;
  *reset = false;
  if (modified && reset_label != nullptr) {
    const char* shown = shown_reset;
    hint_end -= ImGui::CalcTextSize(shown).x;
    ImGui::PushID(reset_label);
    *reset = RightLink(shown, right);
    ImGui::PopID();
    if (!reset_words) ItemTooltip("%s", reset_label);
  }
  if (hint != nullptr) {
    const ImVec2 hint_at(title_end + font, lo.y + kPadY);
    const float room = hint_end - font - hint_at.x;
    if (room >= (std::min)(ImGui::CalcTextSize(hint).x, font * 6.f)) {
      ImGui::GetWindowDrawList()->AddText(hint_at, ToU32(CurrentTheme().text_secondary),
                                          Ellipsize(hint, room).c_str());
    }
  }
  return open;
}

// An on/off switch: a rounded track in the accent colour when on, the knob
// at the end it points to.  Returns true on the frame it flipped *value.
inline bool Switch(const char* id, bool* value) {
  const float height = ImGui::GetFrameHeight();
  const float width = height * 1.9f;
  const float radius = height * 0.5f;
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton(id, ImVec2(width, height), ImGuiButtonFlags_EnableNav);
  if (pressed) *value = !*value;
  const bool hovered = ImGui::IsItemHovered();
  const ImVec4& track = *value ? (hovered ? CurrentTheme().accent_hover : CurrentTheme().accent)
                               : (hovered ? CurrentTheme().bg_control_hover : CurrentTheme().bg_control);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(at, ImVec2(at.x + width, at.y + height), ToU32(track), radius);
  draw->AddRect(at, ImVec2(at.x + width, at.y + height), ToU32(CurrentTheme().node_border), radius);
  draw->AddCircleFilled(
      ImVec2(*value ? at.x + width - radius : at.x + radius, at.y + radius),
      radius - 3.f, ToU32(*value ? CurrentTheme().switch_knob : CurrentTheme().slider_knob));
  return pressed;
}

// Equal-width buttons in one row, the selected one in the accent colour.
// `current` is -1 when no segment matches the setting; `tooltip`, when not
// null, shows over every segment.  The row is the next item's width, which
// RowLabel sets.  Returns the index clicked this frame, or -1.  A row too
// narrow for its longest label (ReShade docks the panel at ~450 px) draws a
// dropdown of the same choices instead of clipped words.
inline int Segmented(const char* id, const char* const* english, int count,
                     int current, const char* tooltip, const char* const* option_tips = nullptr, const bool* disabled = nullptr, bool compact_combo = false) {
  constexpr float kGap = 2.f;
  constexpr int kMaxSegments = 8;
  const char* labels[kMaxSegments] = {};
  count = (std::min)(count, kMaxSegments);
  for (int i = 0; i < count; ++i) labels[i] = Tr(english[i]);
  tooltip = Tr(tooltip);
  const float width =
      (ImGui::CalcItemWidth() - kGap * static_cast<float>(count - 1))
      / static_cast<float>(count);
  float widest = 0.f;
  for (int i = 0; i < count; ++i) {
    widest = (std::max)(widest, ImGui::CalcTextSize(labels[i], nullptr, true).x);
  }
  int clicked = -1;
  if (compact_combo || widest + ImGui::GetStyle().FramePadding.x * 2.f > width) {
    const bool open = ImGui::BeginCombo(id, current >= 0 && current < count ? labels[current] : Tr("Custom"));
    if (open) {
      for (int i = 0; i < count; ++i) {
        ImGui::BeginDisabled(disabled != nullptr && disabled[i]);
        if (ImGui::Selectable(labels[i], i == current)) clicked = i;
        ImGui::EndDisabled();
        ItemTooltip("%s", option_tips != nullptr ? Tr(option_tips[i]) : labels[i]);
      }
      ImGui::EndCombo();
    } else if (current >= 0 && current < count) {
      ItemTooltip("%s", option_tips != nullptr ? Tr(option_tips[current]) : labels[current]);
    }
    return clicked;
  }
  ImGui::PushID(id);
  StyleScope row;
  row.Var(ImGuiStyleVar_ItemSpacing, ImVec2(kGap, ImGui::GetStyle().ItemSpacing.y));
  for (int i = 0; i < count; ++i) {
    if (i != 0) ImGui::SameLine();
    {
      StyleScope segment;
      if (i == current) {
        segment.Color(ImGuiCol_Button, CurrentTheme().accent)
            .Color(ImGuiCol_ButtonHovered, CurrentTheme().accent_hover)
            .Color(ImGuiCol_ButtonActive, CurrentTheme().accent_active)
            .Color(ImGuiCol_Text, CurrentTheme().on_accent);
      }
      ImGui::BeginDisabled(disabled != nullptr && disabled[i]);
      if (ImGui::Button(labels[i], ImVec2(width, 0.f))) clicked = i;
      ImGui::EndDisabled();
    }
    // After the segment's colours pop: a tooltip draws its text in the
    // current ImGuiCol_Text, and the selected segment's is the window
    // background, which on the popup's dark fill left the text unreadable.
    if (option_tips != nullptr)
      ItemTooltip("%s", Tr(option_tips[i]));
    else if (tooltip != nullptr)
      ItemTooltip("%s", labels[i]);
  }
  ImGui::PopID();
  return clicked;
}

// i18n: end

}  // namespace renodx::addons::dlss5::ui
