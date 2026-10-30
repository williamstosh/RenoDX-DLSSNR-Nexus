/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace renodx::addons::dlss5::ui {
struct ProfileKeyEdge {
  bool held = true;
  std::uint32_t binding = 0;
  bool enabled = false;
  bool input_context = false;
  bool awaiting_context_release = false;

  bool Poll(std::uint32_t key, bool on, bool down, bool permitted, bool current_input_context = false) {
    if (input_context != current_input_context) {
      input_context = current_input_context;
      awaiting_context_release = true;
      binding = key;
      enabled = on;
      held = down;
      return false;
    }
    if (awaiting_context_release) {
      if (!down) awaiting_context_release = false;
      held = down;
      return false;
    }
    if (binding != key || enabled != on) {
      binding = key;
      enabled = on;
      held = down;
      return false;
    }
    const bool pressed = down && !held;
    held = down;
    return pressed && on && permitted;
  }
};

inline bool HotkeyConflict(const std::array<std::uint32_t, 4>& keys,
                           const std::array<bool, 4>& enabled, std::size_t action) {
  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (i != action && enabled[i] && keys[i] == keys[action]) return true;
  }
  return false;
}
}  // namespace renodx::addons::dlss5::ui
