/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include <present_hdr_pedestal_policy.hpp>

namespace {
namespace hdr = renodx::addons::dlss5::present_hdr_pedestal;

uint32_t checks = 0;
uint32_t failures = 0;

void Check(bool condition, std::string_view description) {
  ++checks;
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << description << '\n';
  }
}

hdr::Context MakeContext(uint8_t hdr_mode, bool display_codec, uint32_t legacy_mode,
                         bool absolute_units, float unit_nits) {
  return {.actual_present = true,
          .hdr_mode = hdr_mode,
          .v6_available = true,
          .encoding_valid = true,
          .absolute_units = absolute_units,
          .unit_nits = unit_nits,
          .display_codec = display_codec,
          .legacy_mode = legacy_mode};
}

void CheckInactive(const hdr::Snapshot& snapshot, hdr::Reason expected_reason,
                   bool expected_legacy_measure, const char* case_name) {
  const std::string prefix = std::string(case_name) + ": ";
  const float expected_gate = expected_legacy_measure ? 1.f : 0.f;
  Check(!snapshot.active, prefix + "stays inactive");
  Check(snapshot.reason == expected_reason, prefix + "reports the inactive reason");
  Check(snapshot.measure_legacy == expected_legacy_measure,
        prefix + "preserves the old measurement predicate");
  Check(snapshot.legacy_gate == expected_gate, prefix + "preserves the old legacy gate");
  Check(snapshot.formal_gate == expected_gate, prefix + "does not apply formal policy while inactive");
  Check(snapshot.effective_gate == expected_gate,
        prefix + "does not apply the debug or formal gate while inactive");
}
}  // namespace

int main() {
  using hdr::Policy;
  using hdr::Reason;

  Check(static_cast<uint32_t>(Policy::LEGACY) == 0, "Legacy enum value remains 0");
  Check(static_cast<uint32_t>(Policy::OFF) == 1, "Off enum value remains 1");
  Check(static_cast<uint32_t>(Policy::LEGACY_SMOOTH) == 2,
        "LegacySmooth enum value remains 2");
  Check(hdr::DEFAULT_POLICY == Policy::OFF, "HDR policy defaults to Off");
  Check(hdr::CONFIG_VERSION == 1, "HDR policy has its own version 1 schema");

  const std::array<Policy, 3> expected_policies = {
      Policy::LEGACY, Policy::OFF, Policy::LEGACY_SMOOTH};
  for (uint32_t value = 0; value < expected_policies.size(); ++value) {
    const auto loaded = hdr::LoadPolicy(value, hdr::CONFIG_VERSION, true);
    Check(loaded.policy == expected_policies[value],
          "explicit new policy value loads without changing its enum meaning");
    Check(loaded.reason == Reason::ACTIVE && loaded.writable,
          "explicit known-schema policy is recognized and writable");
  }

  const auto missing_without_version = hdr::LoadPolicy(1, 0, false);
  Check(missing_without_version.policy == Policy::OFF
            && missing_without_version.reason == Reason::ABSENT_KEY,
        "missing policy key defaults to Off even without a version key");
  for (uint32_t old_mode = 0; old_mode <= 2; ++old_mode) {
    const auto missing = hdr::LoadPolicy(old_mode, hdr::CONFIG_VERSION, false);
    Check(missing.policy == Policy::OFF && missing.reason == Reason::ABSENT_KEY,
          "missing new key defaults to Off independently of old mode");
    auto context = MakeContext(2, true, old_mode, true, 10000.f);
    const auto decision = hdr::Decide(missing, context);
    Check(decision.active && decision.legacy_mode == old_mode
              && decision.effective_mode == old_mode,
          "no-key Off keeps the old measurement mode");
    Check(decision.measure_legacy && decision.legacy_gate == 1.f
              && decision.formal_gate == 0.f && decision.effective_gate == 0.f,
          "no-key Off disables application for each old mode");
  }

  const auto pq_config = hdr::LoadPolicy(0, hdr::CONFIG_VERSION, true);
  const std::array<std::pair<hdr::Context, const char*>, 2> valid_hdr_cases = {{
      {MakeContext(2, false, 0, true, 10000.f), "Present PQ with absolute 10000-nit units"},
      {MakeContext(1, false, 0, true, 80.f), "Present scRGB with absolute 80-nit units"},
  }};
  for (const auto& [context, name] : valid_hdr_cases) {
    const auto decision = hdr::Decide(pq_config, context);
    const std::string prefix = std::string(name) + ": ";
    Check(decision.active, prefix + "activates for actual Present and v6");
    Check(decision.reason == Reason::ACTIVE, prefix + "reports active policy reason");
    Check(decision.measure_legacy && decision.legacy_gate == 1.f
              && decision.formal_gate == 1.f && decision.effective_gate == 1.f,
          prefix + "valid absolute units preserve Legacy gate behavior");
  }

  auto not_present = MakeContext(2, true, 0, false, 80.f);
  not_present.actual_present = false;
  CheckInactive(hdr::Decide(pq_config, not_present), Reason::NOT_PRESENT, false,
                "non-Present path");

  const auto sdr = MakeContext(0, false, 0, true, 80.f);
  CheckInactive(hdr::Decide(pq_config, sdr), Reason::SDR, true, "SDR path");

  auto old_codec = MakeContext(2, true, 1, false, 80.f);
  old_codec.v6_available = false;
  CheckInactive(hdr::Decide(pq_config, old_codec), Reason::NON_V6, true,
                "old codec path");

  auto unknown_encoding = MakeContext(2, true, 0, false, 80.f);
  unknown_encoding.encoding_valid = false;
  CheckInactive(hdr::Decide(pq_config, unknown_encoding), Reason::UNKNOWN_ENCODING, false,
                "unknown encoding");

  const auto zero_units = MakeContext(2, true, 0, true, 0.f);
  CheckInactive(hdr::Decide(pq_config, zero_units), Reason::INVALID_UNITS, false,
                "zero units");

  const auto negative_units = MakeContext(2, true, 1, true, -1.f);
  CheckInactive(hdr::Decide(pq_config, negative_units), Reason::INVALID_UNITS, true,
                "negative units");

  const auto nan_units = MakeContext(2, true, 0, true,
                                     std::numeric_limits<float>::quiet_NaN());
  CheckInactive(hdr::Decide(pq_config, nan_units), Reason::INVALID_UNITS, false,
                "NaN units");

  const auto infinite_units = MakeContext(2, true, 0, true,
                                          std::numeric_limits<float>::infinity());
  CheckInactive(hdr::Decide(pq_config, infinite_units), Reason::INVALID_UNITS, true,
                "infinite units");

  const auto relative_units = MakeContext(2, true, 0, false, 80.f);
  CheckInactive(hdr::Decide(pq_config, relative_units), Reason::INVALID_UNITS, false,
                "relative units without an absolute-unit contract");

  const auto unknown_hdr_mode = MakeContext(3, true, 1, true, 10000.f);
  const auto unknown_hdr_decision = hdr::Decide(pq_config, unknown_hdr_mode);
  Check(!unknown_hdr_decision.active && unknown_hdr_mode.encoding_valid
            && unknown_hdr_decision.reason == Reason::UNKNOWN_ENCODING,
        "unknown hdr_mode is rejected even when encoding_valid is true");
  CheckInactive(unknown_hdr_decision, Reason::UNKNOWN_ENCODING, true,
                "unknown hdr_mode with valid encoding flag");

  struct LegacyPredicateCase {
    hdr::Context context;
    bool expected_measure;
    const char* name;
  };
  const std::array<LegacyPredicateCase, 7> legacy_predicate_cases = {{
      {MakeContext(2, false, 0, false, 80.f), true, "non-Display Auto is always measured"},
      {MakeContext(2, false, 2, false, 80.f), true, "non-Display Smooth is always measured"},
      {MakeContext(2, true, 1, false, 80.f), true, "Display Always is measured"},
      {MakeContext(2, true, 0, true, 10000.f), true, "Display Auto measures absolute units"},
      {MakeContext(2, true, 0, false, 80.f), false, "Display Auto skips relative units"},
      {MakeContext(2, true, 2, true, 80.f), true, "Display Smooth Auto measures absolute units"},
      {MakeContext(2, true, 2, false, 80.f), false, "Display Smooth Auto skips relative units"},
  }};
  for (const auto& test_case : legacy_predicate_cases) {
    const auto decision = hdr::Decide(pq_config, test_case.context);
    const float expected_gate = test_case.expected_measure ? 1.f : 0.f;
    const std::string prefix = std::string(test_case.name) + ": ";
    Check(decision.measure_legacy == test_case.expected_measure,
          prefix + "matches the old FramePedestalRemoval predicate");
    Check(decision.legacy_gate == expected_gate && decision.formal_gate == expected_gate
              && decision.effective_gate == expected_gate,
          prefix + "keeps all gate values equal to the old predicate");
  }

  const auto explicit_off = hdr::LoadPolicy(static_cast<uint32_t>(Policy::OFF),
                                             hdr::CONFIG_VERSION, true);
  const auto always_context = MakeContext(2, true, 1, true, 10000.f);
  const auto legacy_always = hdr::Decide(pq_config, always_context);
  const auto off_always = hdr::Decide(explicit_off, always_context);
  Check(explicit_off.policy == Policy::OFF && off_always.active,
        "explicit new Off is selected on a supported Present HDR frame");
  Check(legacy_always.measure_legacy && off_always.measure_legacy
            && off_always.legacy_mode == 1,
        "formal Off preserves old Always measurement and mode snapshot");
  Check(off_always.legacy_gate == legacy_always.legacy_gate
            && off_always.formal_gate == 0.f && off_always.effective_gate == 0.f,
        "explicit new Off overrides application while retaining legacy measurement");

  for (uint32_t old_mode = 0; old_mode <= 2; ++old_mode) {
    const auto context = MakeContext(2, true, old_mode, true, 10000.f);
    const auto legacy = hdr::Decide(pq_config, context);
    const auto off = hdr::Decide(explicit_off, context);
    Check(off.measure_legacy == legacy.measure_legacy && off.legacy_gate == legacy.legacy_gate,
          "formal Off retains the Legacy measurement and legacy gate for every old mode");
    Check(off.formal_gate == 0.f && off.effective_gate == 0.f,
          "formal Off zeros the application gate for every old mode");
  }

  const auto smooth_config = hdr::LoadPolicy(static_cast<uint32_t>(Policy::LEGACY_SMOOTH),
                                              hdr::CONFIG_VERSION, true);
  const auto auto_absolute = MakeContext(2, true, 0, true, 10000.f);
  const auto auto_legacy = hdr::Decide(pq_config, auto_absolute);
  const auto auto_smooth = hdr::Decide(smooth_config, auto_absolute);
  Check(auto_smooth.active && auto_smooth.effective_mode == 2,
        "active LegacySmooth selects effective weight mode 2");
  Check(auto_smooth.measure_legacy == auto_legacy.measure_legacy
            && auto_smooth.legacy_gate == auto_legacy.legacy_gate
            && auto_smooth.formal_gate == auto_legacy.formal_gate
            && auto_smooth.effective_gate == auto_legacy.effective_gate,
        "LegacySmooth changes the weight mode without changing the legacy gate");

  const auto smooth_relative = hdr::Decide(smooth_config, relative_units);
  Check(!smooth_relative.active && !smooth_relative.measure_legacy
            && smooth_relative.legacy_gate == 0.f && smooth_relative.formal_gate == 0.f
            && smooth_relative.effective_gate == 0.f,
        "LegacySmooth does not enable Display Auto when old relative-unit rules disable it");
  Check(smooth_relative.effective_mode == relative_units.legacy_mode,
        "inactive LegacySmooth preserves the old disabled mode");

  for (uint32_t policy_value = 0; policy_value <= 2; ++policy_value) {
    const auto configuration = hdr::LoadPolicy(policy_value, hdr::CONFIG_VERSION, true);
    auto context = MakeContext(2, true, 1, true, 10000.f);
    const auto baseline = hdr::Decide(configuration, context);
    context.debug_bypass = true;
    const auto bypass = hdr::Decide(configuration, context);
    Check(bypass.configuration.policy == baseline.configuration.policy
              && bypass.configuration.version == baseline.configuration.version
              && bypass.configuration.writable == baseline.configuration.writable
              && bypass.configuration.reason == baseline.configuration.reason,
          "debug bypass does not mutate the loaded configuration");
    Check(bypass.measure_legacy == baseline.measure_legacy
              && bypass.legacy_gate == baseline.legacy_gate
              && bypass.formal_gate == baseline.formal_gate
              && bypass.legacy_mode == baseline.legacy_mode
              && bypass.effective_mode == baseline.effective_mode,
          "debug bypass does not mutate legacy measurement, mode, or formal gate");
    Check(bypass.debug_bypass && bypass.effective_gate == 0.f,
          "debug bypass overrides the effective gate to zero");
  }

  for (const uint32_t invalid_value : {3u, std::numeric_limits<uint32_t>::max()}) {
    const auto invalid = hdr::LoadPolicy(invalid_value, hdr::CONFIG_VERSION, true);
    Check(invalid.policy == Policy::LEGACY && invalid.version == hdr::CONFIG_VERSION
              && invalid.reason == Reason::INVALID_POLICY,
          "invalid policy falls back to Legacy and records its reason");
    Check(invalid.writable,
          "known-schema invalid policy allows an explicit user selection or save");
    const auto decision = hdr::Decide(invalid, always_context);
    Check(decision.active && decision.reason == Reason::INVALID_POLICY
              && decision.measure_legacy && decision.effective_gate == 1.f,
          "invalid policy continues with conservative Legacy behavior");
  }

  for (const uint32_t future_version : {2u, std::numeric_limits<uint32_t>::max()}) {
    const auto future = hdr::LoadPolicy(static_cast<uint32_t>(Policy::OFF), future_version, true);
    Check(future.policy == Policy::LEGACY && future.version == future_version
              && future.reason == Reason::FUTURE_VERSION,
          "future config version is not reinterpreted and falls back to Legacy");
    Check(!future.writable, "future config version is read-only");
    for (uint32_t old_mode = 0; old_mode <= 2; ++old_mode) {
      const auto context = MakeContext(2, true, old_mode, true, 10000.f);
      const auto decision = hdr::Decide(future, context);
      Check(decision.reason == Reason::FUTURE_VERSION
                && decision.configuration.version == future_version
                && !decision.configuration.writable,
            "future version remains unexplained and read-only in the frame snapshot");
      Check(decision.legacy_mode == old_mode && decision.effective_mode == old_mode
                && decision.measure_legacy && decision.legacy_gate == 1.f
                && decision.formal_gate == 1.f && decision.effective_gate == 1.f,
            "future version falls back to the old 0/1/2 behavior without applying Off");
    }
  }
  const auto future_without_key = hdr::LoadPolicy(1, 2, false);
  Check(future_without_key.policy == Policy::LEGACY && future_without_key.version == 2
            && future_without_key.reason == Reason::FUTURE_VERSION
            && !future_without_key.writable,
        "future version remains read-only Legacy even when the new policy key is absent");

  const auto default_decision = hdr::Decide({}, auto_absolute);
  Check(default_decision.configuration.policy == Policy::OFF
            && default_decision.reason == Reason::ABSENT_KEY
            && default_decision.effective_mode == 0u && default_decision.effective_gate == 0.f,
        "default Configuration and Decide apply Off");

  const std::array<std::pair<hdr::Context, Reason>, 11> inactive_cases = {{
      {not_present, Reason::NOT_PRESENT}, {sdr, Reason::SDR},
      {old_codec, Reason::NON_V6}, {unknown_encoding, Reason::UNKNOWN_ENCODING},
      {unknown_hdr_mode, Reason::UNKNOWN_ENCODING}, {zero_units, Reason::INVALID_UNITS},
      {negative_units, Reason::INVALID_UNITS}, {nan_units, Reason::INVALID_UNITS},
      {infinite_units, Reason::INVALID_UNITS}, {relative_units, Reason::INVALID_UNITS},
      {MakeContext(2, true, 0, true, -std::numeric_limits<float>::infinity()), Reason::INVALID_UNITS},
  }};
  for (const auto& [context, reason] : inactive_cases) {
    const auto legacy = hdr::Decide(pq_config, context);
    for (uint32_t policy_value = 0; policy_value <= 2; ++policy_value) {
      const auto configuration = hdr::LoadPolicy(policy_value, hdr::CONFIG_VERSION);
      for (const bool bypass : {false, true}) {
        auto inactive_context = context;
        inactive_context.debug_bypass = bypass;
        const auto decision = hdr::Decide(configuration, inactive_context);
        CheckInactive(decision, reason, legacy.measure_legacy, "inactive policy/bypass matrix");
        Check(decision.legacy_mode == context.legacy_mode
                  && decision.effective_mode == context.legacy_mode,
              "inactive policies and bypass preserve the old mode");
      }
    }
  }

  for (uint32_t old_mode = 0; old_mode <= 2; ++old_mode) {
    const auto context = MakeContext(1, false, old_mode, true, 80.f);
    const auto legacy = hdr::Decide(pq_config, context);
    const auto off = hdr::Decide(explicit_off, context);
    const auto smooth = hdr::Decide(smooth_config, context);
    Check(off.active && off.effective_mode == old_mode && off.measure_legacy == legacy.measure_legacy
              && off.legacy_gate == legacy.legacy_gate && off.effective_gate == 0.f,
          "scRGB Off changes only the application gate for each old mode");
    Check(smooth.active && smooth.effective_mode == 2u && smooth.legacy_mode == old_mode
              && smooth.measure_legacy == legacy.measure_legacy
              && smooth.legacy_gate == legacy.legacy_gate && smooth.formal_gate == legacy.formal_gate
              && smooth.effective_gate == legacy.effective_gate,
          "scRGB Smooth changes only the effective weight mode for each old mode");
  }

  std::cout << checks << " checks, " << failures << " failures\n";
  return failures == 0 ? 0 : 1;
}
