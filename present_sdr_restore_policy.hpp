/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>

namespace renodx::addons::dlss5::present_sdr_restore {

enum class Policy : uint32_t {
  LEGACY = 0,
  OFF = 1,
  LEGACY_AUTO = 2,
  LEGACY_EVERY_BLOCK = 3,
  LEGACY_SMOOTH = 4,
};

inline constexpr uint32_t CONFIG_VERSION = 1;
inline constexpr Policy DEFAULT_POLICY = Policy::OFF;
inline constexpr Policy FALLBACK_POLICY = Policy::LEGACY;
// This is the unchanged legacy shader cap in encoded SDR units, not linear light.
inline constexpr float ENCODED_CAP = 0.025f;

struct Configuration {
  Policy policy = DEFAULT_POLICY;
  uint32_t version = CONFIG_VERSION;
  bool writable = true;
};

inline constexpr Policy SanitizePolicy(uint32_t value) {
  return (value <= static_cast<uint32_t>(Policy::LEGACY_SMOOTH)
              ? static_cast<Policy>(value) : FALLBACK_POLICY);
}

inline constexpr Configuration LoadPolicy(uint32_t value, uint32_t version,
                                          bool policy_present = true, bool legacy_explicit = false) {
  // A newer schema owns both keys; do not reinterpret or overwrite it.
  if (version > CONFIG_VERSION) {
    return {.policy = FALLBACK_POLICY, .version = version, .writable = false};
  }
  if (!policy_present) {
    return {.policy = (legacy_explicit ? Policy::LEGACY : DEFAULT_POLICY),
            .version = CONFIG_VERSION, .writable = true};
  }
  return {.policy = SanitizePolicy(value), .version = CONFIG_VERSION, .writable = true};
}

inline constexpr uint32_t EffectiveLegacyMode(Policy policy, uint32_t legacy_mode) {
  switch (policy) {
    case Policy::LEGACY_AUTO: return 0;
    case Policy::LEGACY_EVERY_BLOCK: return 1;
    case Policy::LEGACY_SMOOTH: return 2;
    default: return (legacy_mode <= 2u ? legacy_mode : 0u);
  }
}

inline constexpr bool ShouldRestore(Policy policy, uint8_t hdr_mode, bool diagnostic_bypass) {
  return hdr_mode == 0 && policy != Policy::OFF && !diagnostic_bypass;
}

inline constexpr float EncodedGate(uint32_t legacy_mode) {
  return (legacy_mode == 1u ? 2.f : 0.061f);
}

}  // namespace renodx::addons::dlss5::present_sdr_restore
