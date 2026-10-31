/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <algorithm>
#include <cmath>

// Include after the host's ImGui declarations, as with widgets.hpp.

namespace renodx::addons::dlss5::ui {

struct NoticePosition {
  // Old percentages also determine the pivot; rounding them changes placement.
  int anchor = -1;
  ImVec2 legacy_percent{50.f, 0.f};
  ImVec2 offset{0.f, 0.f};
};

inline NoticePosition NormalizeNoticePosition(NoticePosition position) {
  if (position.anchor < -1 || position.anchor > 8) {
    position.anchor = -1;
  }
  const auto finite_clamp = [](float value, float fallback, float minimum, float maximum) {
    return (std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback);
  };
  position.legacy_percent.x = finite_clamp(position.legacy_percent.x, 50.f, 0.f, 100.f);
  position.legacy_percent.y = finite_clamp(position.legacy_percent.y, 0.f, 0.f, 100.f);
  position.offset.x = finite_clamp(position.offset.x, 0.f, -16384.f, 16384.f);
  position.offset.y = finite_clamp(position.offset.y, 0.f, -16384.f, 16384.f);
  return position;
}

inline ImVec2 NoticePivot(const NoticePosition& position) {
  if (position.anchor < 0) {
    return ImVec2(position.legacy_percent.x / 100.f, position.legacy_percent.y / 100.f);
  }
  return ImVec2(static_cast<float>(position.anchor % 3) * .5f,
                static_cast<float>(position.anchor / 3) * .5f);
}

inline ImVec2 NoticeTopLeft(const NoticePosition& position, ImVec2 display, ImVec2 size,
                             float gap, float scale) {
  const auto pivot = NoticePivot(position);
  ImVec2 result(gap + (display.x - 2.f * gap - size.x) * pivot.x,
                gap + (display.y - 2.f * gap - size.y) * pivot.y);
  if (position.anchor >= 0) {
    result.x = std::clamp(result.x + position.offset.x * scale, gap,
        (std::max)(gap, display.x - gap - size.x));
    result.y = std::clamp(result.y + position.offset.y * scale, gap,
        (std::max)(gap, display.y - gap - size.y));
  }
  return result;
}

}  // namespace renodx::addons::dlss5::ui
