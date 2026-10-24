/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>

namespace renodx::addons::dlss5::present_restore_scope {

enum class Domain : std::uint32_t {
  FINAL_PRESENT = 0,
  PRE_FG_HUDLESS,
  PRE_FG_BACKBUFFER,
  PRE_FG_COMPOSE,
  UPSCALED_FALLBACK,
  OTHER,
};

enum class Scope : std::uint32_t {
  FINAL_PRESENT_ONLY = 0,
  FINAL_PRESENT_AND_PRE_FG = 1,
};

enum class Reason : std::uint32_t {
  NONE = 0,
  NEW_CONFIGURATION,
  NO_SCOPE_KEYS,
  INCOMPLETE_PAIR,
  MALFORMED_SCOPE,
  DAMAGED_VERSION,
  FUTURE_SCOPE,
  FUTURE_VERSION,
  FUTURE_GLOBAL,
  FINAL_PRESENT,
  INHERITED_PRE_FG,
  EXPLICIT_PRE_FG_SCOPE,
  FINAL_PRESENT_ONLY_SCOPE,
  INACTIVE_DOMAIN,
};

enum class Source : std::uint32_t {
  UNKNOWN = 0,
  NEW_EMPTY_SECTION,
  EXPLICIT_SCOPE_PAIR,
  EXISTING_SECTION,
  UNKNOWN_SECTION,
  INCOMPLETE_SCOPE_PAIR,
  MALFORMED_SCOPE_KEY,
  DAMAGED_VERSION_KEY,
  FUTURE_SCOPE_KEY,
  FUTURE_SCOPE_VERSION,
  FUTURE_GLOBAL_CONFIGURATION,
};

inline constexpr std::uint32_t CONFIG_VERSION = 1;

struct RawConfiguration {
  bool section_known = false;
  bool section_has_settings = false;
  bool global_future = false;
  bool scope_present = false;
  bool scope_valid = true;
  std::uint32_t scope_value = 0;
  bool version_present = false;
  bool version_valid = true;
  std::uint32_t version_value = CONFIG_VERSION;
};

struct Configuration {
  Scope scope = Scope::FINAL_PRESENT_ONLY;
  bool inherited = true;
  bool writable = false;
  bool initialize_new = false;
  Reason reason = Reason::NO_SCOPE_KEYS;
  Source source = Source::UNKNOWN;
};

struct Decision {
  bool sdr_policy = false;
  bool hdr_policy = false;
  Reason reason = Reason::INACTIVE_DOMAIN;
};

inline constexpr Configuration Load(const RawConfiguration& raw) {
  if (raw.global_future) {
    return {.reason = Reason::FUTURE_GLOBAL, .source = Source::FUTURE_GLOBAL_CONFIGURATION};
  }
  if (raw.version_present && !raw.version_valid) {
    return {.reason = Reason::DAMAGED_VERSION, .source = Source::DAMAGED_VERSION_KEY};
  }
  if (raw.version_present && raw.version_value > CONFIG_VERSION) {
    return {.reason = Reason::FUTURE_VERSION, .source = Source::FUTURE_SCOPE_VERSION};
  }
  if (raw.version_present && raw.version_value < CONFIG_VERSION) {
    return {.reason = Reason::DAMAGED_VERSION, .source = Source::DAMAGED_VERSION_KEY};
  }
  if (raw.scope_present && !raw.scope_valid) {
    return {.writable = true, .reason = Reason::MALFORMED_SCOPE, .source = Source::MALFORMED_SCOPE_KEY};
  }
  if (raw.scope_present && raw.scope_value > static_cast<std::uint32_t>(Scope::FINAL_PRESENT_AND_PRE_FG)) {
    return {.reason = Reason::FUTURE_SCOPE, .source = Source::FUTURE_SCOPE_KEY};
  }
  if (raw.scope_present != raw.version_present) {
    return {.writable = true, .reason = Reason::INCOMPLETE_PAIR, .source = Source::INCOMPLETE_SCOPE_PAIR};
  }
  if (raw.scope_present) {
    return {.scope = static_cast<Scope>(raw.scope_value),
            .inherited = false,
            .writable = true,
            .reason = Reason::NONE,
            .source = Source::EXPLICIT_SCOPE_PAIR};
  }
  if (raw.section_known && !raw.section_has_settings) {
    return {.scope = Scope::FINAL_PRESENT_AND_PRE_FG,
            .inherited = false,
            .writable = true,
            .initialize_new = true,
            .reason = Reason::NEW_CONFIGURATION,
            .source = Source::NEW_EMPTY_SECTION};
  }
  return {.writable = true, .reason = Reason::NO_SCOPE_KEYS,
          .source = (raw.section_known ? Source::EXISTING_SECTION : Source::UNKNOWN_SECTION)};
}

inline constexpr Decision Decide(const Configuration& configuration, Domain domain) {
  switch (domain) {
    case Domain::FINAL_PRESENT:
      return {.sdr_policy = true, .hdr_policy = true, .reason = Reason::FINAL_PRESENT};
    case Domain::PRE_FG_HUDLESS:
    case Domain::PRE_FG_BACKBUFFER:
      if (configuration.inherited) {
        return {.sdr_policy = true, .reason = Reason::INHERITED_PRE_FG};
      }
      if (configuration.scope == Scope::FINAL_PRESENT_AND_PRE_FG) {
        return {.sdr_policy = true, .hdr_policy = true, .reason = Reason::EXPLICIT_PRE_FG_SCOPE};
      }
      return {.reason = Reason::FINAL_PRESENT_ONLY_SCOPE};
    case Domain::PRE_FG_COMPOSE:
    case Domain::UPSCALED_FALLBACK:
    case Domain::OTHER:
      return {.reason = Reason::INACTIVE_DOMAIN};
  }
  return {.reason = Reason::INACTIVE_DOMAIN};
}

inline constexpr const char* ReasonText(Reason reason) {
  switch (reason) {
    case Reason::NONE: return "none";
    case Reason::NEW_CONFIGURATION: return "new_configuration";
    case Reason::NO_SCOPE_KEYS: return "scope_keys_absent";
    case Reason::INCOMPLETE_PAIR: return "incomplete_scope_version_pair";
    case Reason::MALFORMED_SCOPE: return "malformed_scope";
    case Reason::DAMAGED_VERSION: return "damaged_version";
    case Reason::FUTURE_SCOPE: return "future_scope";
    case Reason::FUTURE_VERSION: return "future_scope_version";
    case Reason::FUTURE_GLOBAL: return "future_global_configuration";
    case Reason::FINAL_PRESENT: return "final_present";
    case Reason::INHERITED_PRE_FG: return "inherited_pre_fg_sdr_only";
    case Reason::EXPLICIT_PRE_FG_SCOPE: return "explicit_pre_fg_scope";
    case Reason::FINAL_PRESENT_ONLY_SCOPE: return "final_present_only_scope";
    case Reason::INACTIVE_DOMAIN: return "inactive_domain";
  }
  return "unknown";
}

inline constexpr const char* SourceText(Source source) {
  switch (source) {
    case Source::UNKNOWN: return "unknown";
    case Source::NEW_EMPTY_SECTION: return "new_empty_section";
    case Source::EXPLICIT_SCOPE_PAIR: return "explicit_scope_version_pair";
    case Source::EXISTING_SECTION: return "existing_section";
    case Source::UNKNOWN_SECTION: return "unknown_section";
    case Source::INCOMPLETE_SCOPE_PAIR: return "incomplete_scope_version_pair";
    case Source::MALFORMED_SCOPE_KEY: return "malformed_scope_key";
    case Source::DAMAGED_VERSION_KEY: return "damaged_version_key";
    case Source::FUTURE_SCOPE_KEY: return "future_scope_key";
    case Source::FUTURE_SCOPE_VERSION: return "future_scope_version";
    case Source::FUTURE_GLOBAL_CONFIGURATION: return "future_global_configuration";
  }
  return "unknown";
}

inline constexpr const char* DomainText(Domain domain) {
  switch (domain) {
    case Domain::FINAL_PRESENT: return "final_present";
    case Domain::PRE_FG_HUDLESS: return "pre_fg_hudless";
    case Domain::PRE_FG_BACKBUFFER: return "pre_fg_backbuffer";
    case Domain::PRE_FG_COMPOSE: return "pre_fg_compose";
    case Domain::UPSCALED_FALLBACK: return "upscaled_fallback";
    case Domain::OTHER: return "other";
  }
  return "unknown";
}

}  // namespace renodx::addons::dlss5::present_restore_scope
