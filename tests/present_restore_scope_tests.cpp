/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>

#include <present_restore_scope.hpp>

namespace {
namespace scope = renodx::addons::dlss5::present_restore_scope;

std::uint32_t checks = 0;
std::uint32_t failures = 0;

void Check(bool condition, const char* description) {
  ++checks;
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << description << '\n';
  }
}
}  // namespace

int main() {
  using scope::Configuration;
  using scope::Domain;
  using scope::Reason;
  using scope::Scope;
  using scope::Source;

  constexpr std::array domains = {
      Domain::FINAL_PRESENT,
      Domain::PRE_FG_HUDLESS,
      Domain::PRE_FG_BACKBUFFER,
      Domain::PRE_FG_COMPOSE,
      Domain::UPSCALED_FALLBACK,
      Domain::OTHER,
  };
  constexpr std::array boundary_values = {
      0u,
      1u,
      2u,
      std::numeric_limits<std::uint32_t>::max(),
  };

  constexpr scope::RawConfiguration default_raw{};
  constexpr auto default_config = scope::Load(default_raw);
  static_assert(default_config.inherited);
  static_assert(!default_config.initialize_new);
  Check(default_raw.version_value == scope::CONFIG_VERSION, "raw version defaults to the current schema");
  Check(default_config.inherited && !default_config.initialize_new,
        "unknown section state without scope keys inherits conservatively");
  Check(default_config.source == Source::UNKNOWN_SECTION, "unknown section state is reported as the source");

  constexpr auto new_config = scope::Load({.section_known = true});
  static_assert(!new_config.inherited && new_config.initialize_new);
  Check(new_config.scope == Scope::FINAL_PRESENT_AND_PRE_FG && !new_config.inherited && new_config.writable,
        "known empty section initializes to scope one and can be saved");
  Check(new_config.initialize_new && new_config.reason == Reason::NEW_CONFIGURATION
            && new_config.source == Source::NEW_EMPTY_SECTION,
        "new section initialization is distinguishable in metadata");

  const auto existing_without_keys = scope::Load({.section_known = true, .section_has_settings = true});
  Check(existing_without_keys.inherited && !existing_without_keys.initialize_new
            && existing_without_keys.reason == Reason::NO_SCOPE_KEYS
            && existing_without_keys.source == Source::EXISTING_SECTION,
        "existing configuration without scope keys retains compatibility inheritance");
  Check(existing_without_keys.writable, "old configuration can accept an explicit user scope choice");

  const auto unknown_without_keys = scope::Load({});
  Check(unknown_without_keys.inherited && !unknown_without_keys.initialize_new
            && unknown_without_keys.source == Source::UNKNOWN_SECTION,
        "unknown configuration state is never treated as a new section");

  const auto incomplete_scope = scope::Load({
      .scope_present = true,
      .scope_valid = true,
      .scope_value = 1,
  });
  const auto incomplete_version = scope::Load({
      .version_present = true,
      .version_valid = true,
      .version_value = 1,
  });
  for (const auto& config : {incomplete_scope, incomplete_version}) {
    Check(config.inherited && !config.initialize_new && config.writable
              && config.reason == Reason::INCOMPLETE_PAIR
              && config.source == Source::INCOMPLETE_SCOPE_PAIR,
          "a partial scope/version pair inherits and waits for an explicit supported choice");
  }

  const auto malformed_scope = scope::Load({
      .scope_present = true,
      .scope_valid = false,
      .version_present = true,
      .version_valid = true,
      .version_value = 1,
  });
  Check(malformed_scope.inherited && malformed_scope.writable
            && malformed_scope.reason == Reason::MALFORMED_SCOPE
            && malformed_scope.source == Source::MALFORMED_SCOPE_KEY,
        "malformed scope inherits without blocking an explicit correction");

  const auto damaged_version = scope::Load({
      .scope_present = true,
      .scope_valid = true,
      .scope_value = 1,
      .version_present = true,
      .version_valid = false,
  });
  Check(damaged_version.inherited && !damaged_version.writable
            && damaged_version.reason == Reason::DAMAGED_VERSION
            && damaged_version.source == Source::DAMAGED_VERSION_KEY,
        "damaged version inherits and is read-only");

  const auto future_scope = scope::Load({
      .scope_present = true,
      .scope_valid = true,
      .scope_value = 2,
      .version_present = true,
      .version_valid = true,
      .version_value = 1,
  });
  Check(future_scope.inherited && !future_scope.writable
            && future_scope.reason == Reason::FUTURE_SCOPE
            && future_scope.source == Source::FUTURE_SCOPE_KEY,
        "unsupported future scope value inherits read-only");

  const auto future_global = scope::Load({.global_future = true});
  Check(future_global.inherited && !future_global.writable && !future_global.initialize_new
            && future_global.reason == Reason::FUTURE_GLOBAL
            && future_global.source == Source::FUTURE_GLOBAL_CONFIGURATION,
        "future global configuration inherits read-only even when the section appears empty");

  for (const std::uint32_t scope_value : boundary_values) {
    for (const std::uint32_t version_value : boundary_values) {
      const auto config = scope::Load({
          .section_known = true,
          .section_has_settings = true,
          .scope_present = true,
          .scope_valid = true,
          .scope_value = scope_value,
          .version_present = true,
          .version_valid = true,
          .version_value = version_value,
      });
      const bool supported_scope = scope_value <= 1u;
      const bool supported_version = version_value == scope::CONFIG_VERSION;
      const bool explicit_pair = supported_scope && supported_version;
      Check(config.inherited == !explicit_pair && !config.initialize_new,
            "bounded scope/version matrix classifies explicit pairs and inherited states");
      Check(config.writable == (supported_scope && supported_version),
            "bounded scope/version matrix protects future values from writes");
      if (explicit_pair) {
        Check(config.scope == static_cast<Scope>(scope_value)
                  && config.reason == Reason::NONE
                  && config.source == Source::EXPLICIT_SCOPE_PAIR,
              "supported explicit pair preserves the selected scope");
      } else if (version_value < scope::CONFIG_VERSION) {
        Check(config.reason == Reason::DAMAGED_VERSION
                  && config.source == Source::DAMAGED_VERSION_KEY,
              "unsupported older scope schema inherits read-only");
      } else if (!supported_version) {
        Check(config.reason == Reason::FUTURE_VERSION
                  && config.source == Source::FUTURE_SCOPE_VERSION,
              "future schema takes precedence over scope interpretation");
      } else {
        Check(config.reason == Reason::FUTURE_SCOPE
                  && config.source == Source::FUTURE_SCOPE_KEY,
              "future scope values are not reinterpreted");
      }

      for (const Domain domain : domains) {
        const auto decision = scope::Decide(config, domain);
        if (domain == Domain::FINAL_PRESENT) {
          Check(decision.sdr_policy && decision.hdr_policy && decision.reason == Reason::FINAL_PRESENT,
                "final Present always retains both existing policy paths");
        } else if (domain == Domain::PRE_FG_HUDLESS || domain == Domain::PRE_FG_BACKBUFFER) {
          const bool expected_sdr = (!explicit_pair || scope_value == 1u);
          const bool expected_hdr = (explicit_pair && scope_value == 1u);
          const Reason expected_reason = (!explicit_pair ? Reason::INHERITED_PRE_FG
              : (scope_value == 1u ? Reason::EXPLICIT_PRE_FG_SCOPE : Reason::FINAL_PRESENT_ONLY_SCOPE));
          Check(decision.sdr_policy == expected_sdr && decision.hdr_policy == expected_hdr
                    && decision.reason == expected_reason,
                "pre-FG policy eligibility follows inherited and explicit scope rules");
        } else {
          Check(!decision.sdr_policy && !decision.hdr_policy && decision.reason == Reason::INACTIVE_DOMAIN,
                "compose, fallback, and other domains stay inactive");
        }
      }
    }
  }

  constexpr auto inherited_decision = scope::Decide(default_config, Domain::PRE_FG_BACKBUFFER);
  static_assert(inherited_decision.sdr_policy && !inherited_decision.hdr_policy);
  Check(scope::Decide(new_config, Domain::PRE_FG_HUDLESS).sdr_policy
            && scope::Decide(new_config, Domain::PRE_FG_HUDLESS).hdr_policy
            && scope::Decide(new_config, Domain::PRE_FG_BACKBUFFER).hdr_policy,
        "new scope one includes SDR and HDR policies on pre-FG paths");
  Check(scope::Decide(existing_without_keys, Domain::PRE_FG_HUDLESS).sdr_policy
            && !scope::Decide(existing_without_keys, Domain::PRE_FG_HUDLESS).hdr_policy,
        "inherited pre-FG behavior retains only SDR policy eligibility");

  Check(std::string_view(scope::ReasonText(Reason::INCOMPLETE_PAIR)) == "incomplete_scope_version_pair",
        "reason metadata exposes incomplete scope/version pairs");
  Check(std::string_view(scope::SourceText(Source::EXPLICIT_SCOPE_PAIR)) == "explicit_scope_version_pair",
        "source metadata exposes explicit scope/version keys");
  for (const Domain domain : domains) {
    Check(std::string_view(scope::DomainText(domain)) != "unknown", "every supported domain has metadata text");
  }
  Check(std::string_view(scope::ReasonText(static_cast<Reason>(999u))) == "unknown"
            && std::string_view(scope::SourceText(static_cast<Source>(999u))) == "unknown"
            && std::string_view(scope::DomainText(static_cast<Domain>(999u))) == "unknown",
        "metadata text functions safely label unknown enum values");

  std::cout << checks << " checks, " << failures << " failures\n";
  return (failures == 0 ? 0 : 1);
}
