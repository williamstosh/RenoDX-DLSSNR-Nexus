/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>

namespace renodx::addons::dlss5::nr_model {

// Feature 18 has no public capability query for network presets. Only the
// identified runtime is enabled; an unknown/newer build needs verification.
inline constexpr bool SupportsSmallModel(uint32_t major, uint32_t minor,
                                         uint32_t build, uint32_t revision) {
  return major == 310 && minor == 9 && build == 16 && revision == 0;
}

// Main 0 is automatic; a stacked pass's 0 inherits the main selection.
// Preset 3 remains round-trippable for older configurations/runtimes.
inline constexpr uint32_t ResolvePreset(uint32_t main, uint32_t pass, bool dual_models) {
  const uint32_t requested = (pass == 0 ? main : pass);
  if (requested == 0) return dual_models ? 2u : 0u;
  if (requested == 2 && !dual_models) return 1u;
  if (requested == 3 && dual_models) return 1u;
  return requested;
}

inline constexpr const char* PresetName(uint32_t preset) {
  switch (preset) {
    case 0: return "runtime default";
    case 1: return "A (Legacy)";
    case 2: return "B (Small FP8)";
    default: return "legacy preset #3";
  }
}

}  // namespace renodx::addons::dlss5::nr_model
