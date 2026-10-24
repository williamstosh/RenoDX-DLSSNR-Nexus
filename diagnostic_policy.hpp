/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstdint>
#include <charconv>
#include <optional>
#include <string>

#include "present_restore_scope.hpp"

namespace renodx::addons::dlss5::diagnostics {

enum class HdrCaptureMode : uint32_t { COMMIT_CORE = 0, MODEL_TRACE = 1 };

// The callbacks read exact raw values and write only the two scope keys.
// No typed partial parse or generic default stamp may precede classification.
template <typename ReadRaw>
inline present_restore_scope::Configuration ReadRestoreScope(
    const ReadRaw& read_raw, bool section_known, bool section_has_settings, bool global_future) {
  present_restore_scope::RawConfiguration raw{
      .section_known = section_known, .section_has_settings = section_has_settings,
      .global_future = global_future};
  const auto read_uint = [&](const char* key, bool* present, bool* valid, uint32_t* value) {
    const auto text = read_raw(key);
    *present = text.has_value();
    if (!text) return;
    const auto parsed = std::from_chars(text->data(), text->data() + text->size(), *value);
    *valid = !text->empty() && parsed.ec == std::errc{}
        && parsed.ptr == text->data() + text->size();
  };
  read_uint("NRPresentRestoreScope", &raw.scope_present, &raw.scope_valid, &raw.scope_value);
  read_uint("NRPresentRestoreScopeConfigVersion", &raw.version_present, &raw.version_valid, &raw.version_value);
  return present_restore_scope::Load(raw);
}

template <typename WriteValue, typename ReadRaw>
inline bool PersistRestoreScope(present_restore_scope::Configuration* configuration,
    present_restore_scope::Scope scope, const WriteValue& write_value, const ReadRaw& read_raw) {
  if (configuration == nullptr || !configuration->writable
      || static_cast<uint32_t>(scope) > 1u) return false;
  write_value("NRPresentRestoreScope", static_cast<uint32_t>(scope));
  write_value("NRPresentRestoreScopeConfigVersion", present_restore_scope::CONFIG_VERSION);
  const auto verified = ReadRestoreScope(read_raw, true, true, false);
  if (verified.inherited || !verified.writable || verified.scope != scope) return false;
  *configuration = verified;
  return true;
}

struct Options {
  bool bypass_sdr_restore = false;
  bool bypass_host_look = false;
  uint32_t model_ui = 0;  // 0 inherit, 1 force off, 2 force on.
  uint32_t guides = 0;    // 0 inherit, 1 require real, 2 neutral.
  bool capture_stages = false;
  bool bypass_hdr_pedestal = false;
  HdrCaptureMode hdr_capture_mode = HdrCaptureMode::COMMIT_CORE;
};

inline constexpr bool ModelUi(uint32_t mode, bool present_ui, bool pass_ui) {
  switch (mode) {
    case 1: return false;
    case 2: return true;
    default: return present_ui || pass_ui;
  }
}

// Existing Present guide modes: optional=0, required=1, never=2.
inline constexpr uint32_t GuideMode(uint32_t debug_mode, uint32_t inherited) {
  return (debug_mode == 1u || debug_mode == 2u ? debug_mode : inherited);
}

}  // namespace renodx::addons::dlss5::diagnostics
