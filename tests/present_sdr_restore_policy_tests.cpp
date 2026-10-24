/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>

#include <present_sdr_restore_policy.hpp>

namespace {
namespace restore = renodx::addons::dlss5::present_sdr_restore;

uint32_t checks = 0;
uint32_t failures = 0;

void Check(bool condition, const char* description) {
  ++checks;
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << description << '\n';
  }
}
}  // namespace

int main() {
  using restore::Policy;
  constexpr std::array policies = {
      Policy::LEGACY, Policy::OFF, Policy::LEGACY_AUTO,
      Policy::LEGACY_EVERY_BLOCK, Policy::LEGACY_SMOOTH};
  constexpr std::array<std::array<uint32_t, 3>, 5> expected_modes = {{
      {0, 1, 2}, {0, 1, 2}, {0, 0, 0}, {1, 1, 1}, {2, 2, 2}}};

  Check(restore::DEFAULT_POLICY == Policy::OFF, "implicit or new configuration defaults to Off");
  Check(restore::CONFIG_VERSION == 1, "candidate uses explicit config schema");
  for (const uint32_t version : {0u, 1u}) {
    const auto missing = restore::LoadPolicy(0, version, false, false);
    Check(missing.policy == Policy::OFF && missing.writable,
          "absent new and legacy keys select Off without an automatic config write");
    for (uint32_t old_mode = 0; old_mode <= 2; ++old_mode) {
      const auto legacy = restore::LoadPolicy(1, version, false, true);
      Check(legacy.policy == Policy::LEGACY && legacy.writable
                && restore::EffectiveLegacyMode(legacy.policy, old_mode) == old_mode,
            "every explicit old mode is preserved instead of silently migrating to Off");
    }
  }
  for (uint32_t policy = 0; policy < policies.size(); ++policy) {
    Check(restore::SanitizePolicy(policy) == policies[policy], "serialized policy values are stable");
    const auto explicit_choice = restore::LoadPolicy(policy, 1, true, true);
    Check(explicit_choice.policy == policies[policy], "explicit new policy wins over explicit old key");
    for (uint32_t mode = 0; mode <= 2; ++mode) {
      Check(restore::EffectiveLegacyMode(policies[policy], mode) == expected_modes[policy][mode],
            "explicit legacy modes and inherited user selections retain semantics");
    }
    Check(restore::ShouldRestore(policies[policy], 0, false) == (policies[policy] != Policy::OFF),
          "formal Off suppresses SDR restore without a diagnostic switch");
    Check(!restore::ShouldRestore(policies[policy], 0, true), "diagnostic bypass still wins");
    Check(!restore::ShouldRestore(policies[policy], 1, false), "scRGB never enters SDR restore");
    Check(!restore::ShouldRestore(policies[policy], 2, false), "HDR10 never enters SDR restore");
    for (const uint32_t version : {0u, 1u}) {
      const auto config = restore::LoadPolicy(policy, version);
      Check(config.policy == policies[policy] && config.version == 1 && config.writable,
            "known and unversioned config round trips without changing a chosen policy");
    }
    for (const uint32_t version : {2u, std::numeric_limits<uint32_t>::max()}) {
      const auto config = restore::LoadPolicy(policy, version);
      Check(config.policy == Policy::LEGACY && config.version == version && !config.writable,
            "future config is not reinterpreted or marked writable");
    }
  }
  for (const uint32_t bad : {5u, 255u, std::numeric_limits<uint32_t>::max()}) {
    Check(restore::SanitizePolicy(bad) == Policy::LEGACY, "invalid policy falls back to legacy");
    const auto config = restore::LoadPolicy(bad, 1);
    Check(config.policy == Policy::LEGACY && config.version == 1 && config.writable,
          "invalid known-schema policy has a defined compatible fallback");
  }
  Check(restore::EffectiveLegacyMode(Policy::LEGACY, 3) == 0, "invalid old mode has old Auto fallback");
  const auto future_missing = restore::LoadPolicy(1, 2, false, false);
  Check(future_missing.policy == Policy::LEGACY && !future_missing.writable,
        "future schema remains protected even with a missing policy key");
  Check(restore::EncodedGate(0) == .061f, "Auto gate stays in encoded units");
  Check(restore::EncodedGate(1) == 2.f, "Every block keeps its unrestricted gate");
  Check(restore::EncodedGate(2) == .061f, "Smooth retains Auto gate");
  Check(restore::ENCODED_CAP == .025f, "legacy cap retains encoded meaning");
  std::cout << checks << " checks, " << failures << " failures\n";
  return (failures == 0 ? 0 : 1);
}
