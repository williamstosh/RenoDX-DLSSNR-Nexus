/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstdint>
#include <limits>

namespace renodx::addons::dlss5::present_hdr_pedestal {

enum class Policy : uint32_t { LEGACY = 0, OFF = 1, LEGACY_SMOOTH = 2 };
enum class Reason : uint32_t {
  ACTIVE = 0, NOT_PRESENT, SDR, NON_V6, UNKNOWN_ENCODING, INVALID_UNITS,
  ABSENT_KEY, INVALID_POLICY, FUTURE_VERSION,
  SCOPE_INHERITED_HDR_LEGACY, SCOPE_FINAL_ONLY_HDR_LEGACY, CONTRACT_REJECTED,
};
inline constexpr uint32_t CONFIG_VERSION = 1;
inline constexpr Policy DEFAULT_POLICY = Policy::OFF;

struct Configuration {
  Policy policy = DEFAULT_POLICY;
  uint32_t version = CONFIG_VERSION;
  bool writable = true;
  Reason reason = Reason::ABSENT_KEY;
};

inline constexpr Configuration LoadPolicy(uint32_t value, uint32_t version,
                                          bool policy_present = true) {
  if (version > CONFIG_VERSION) {
    return {.policy = Policy::LEGACY, .version = version, .writable = false,
            .reason = Reason::FUTURE_VERSION};
  }
  if (!policy_present) return {};
  if (value > static_cast<uint32_t>(Policy::LEGACY_SMOOTH)) {
    return {.policy = Policy::LEGACY, .reason = Reason::INVALID_POLICY};
  }
  return {.policy = static_cast<Policy>(value), .reason = Reason::ACTIVE};
}

struct Context {
  bool actual_present = false;
  uint8_t hdr_mode = 0;
  bool v6_available = false;
  bool encoding_valid = false;
  bool absolute_units = false;
  float unit_nits = 0.f;
  bool display_codec = false;
  uint32_t legacy_mode = 0;
  bool debug_bypass = false;
  bool scope_eligible = false;
};

struct Snapshot {
  Configuration configuration;
  uint32_t legacy_mode = 0;
  uint32_t effective_mode = 0;
  bool active = false;
  Reason reason = Reason::NOT_PRESENT;
  bool debug_bypass = false;
  bool measure_legacy = false;
  float legacy_gate = 0.f;
  float formal_gate = 0.f;
  float effective_gate = 0.f;
};

inline constexpr Snapshot Decide(const Configuration& configuration, const Context& context) {
  Snapshot result{.configuration = configuration, .legacy_mode = context.legacy_mode,
                  .effective_mode = context.legacy_mode, .debug_bypass = context.debug_bypass};
  // This is FramePedestalRemoval's original inclusion predicate, including relative Display Auto.
  result.measure_legacy = !context.display_codec || context.legacy_mode == 1u
      || (context.absolute_units && context.unit_nits > 0.f);
  result.legacy_gate = (result.measure_legacy ? 1.f : 0.f);
  result.formal_gate = result.legacy_gate;
  result.effective_gate = result.legacy_gate;
  if (!context.actual_present && !context.scope_eligible) result.reason = Reason::NOT_PRESENT;
  else if (context.hdr_mode == 0) result.reason = Reason::SDR;
  else if (!context.v6_available) result.reason = Reason::NON_V6;
  else if ((context.hdr_mode != 1 && context.hdr_mode != 2) || !context.encoding_valid) result.reason = Reason::UNKNOWN_ENCODING;
  else if (!context.absolute_units || !(context.unit_nits > 0.f)
           || context.unit_nits > std::numeric_limits<float>::max()) result.reason = Reason::INVALID_UNITS;
  else {
    result.active = true;
    result.reason = configuration.reason;
    if (configuration.policy == Policy::OFF) result.formal_gate = 0.f;
    if (configuration.policy == Policy::LEGACY_SMOOTH) result.effective_mode = 2u;
    result.effective_gate = (context.debug_bypass ? 0.f : result.formal_gate);
  }
  return result;
}

inline constexpr const char* ReasonText(Reason reason) {
  switch (reason) {
    case Reason::ACTIVE: return "active";
    case Reason::NOT_PRESENT: return "not_present";
    case Reason::SDR: return "sdr";
    case Reason::NON_V6: return "non_v6";
    case Reason::UNKNOWN_ENCODING: return "unknown_encoding";
    case Reason::INVALID_UNITS: return "invalid_units";
    case Reason::ABSENT_KEY: return "compatibility_default";
    case Reason::INVALID_POLICY: return "invalid_policy_legacy";
    case Reason::FUTURE_VERSION: return "future_version_legacy";
    case Reason::SCOPE_INHERITED_HDR_LEGACY: return "scope_inherited_hdr_legacy";
    case Reason::SCOPE_FINAL_ONLY_HDR_LEGACY: return "scope_final_only_hdr_legacy";
    case Reason::CONTRACT_REJECTED: return "contract_rejected";
  }
  return "unknown";
}

}  // namespace renodx::addons::dlss5::present_hdr_pedestal
