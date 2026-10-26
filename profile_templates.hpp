/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "profile_store.hpp"

namespace renodx::addons::dlss5::profiles {

inline constexpr std::uint32_t TEMPLATE_VERSION = 1;

// Development candidates, not a claim of measured image quality. Runtime supplies
// actual registered Setting fallbacks for the Slot-managed fields only.
inline Json Template(std::size_t index, const Json& registered_defaults) {
  if (index >= 3 || !registered_defaults.is_object()) throw std::invalid_argument("Invalid official template input.");
  for (const auto key : {"NRHookPoint", "NRPasses", "NRPreset", "NRPass2Preset", "NRFollowInputRes", "NRResolutionScale"}) {
    if (!registered_defaults.contains(key)) throw std::invalid_argument("Missing registered template setting.");
  }
  auto settings = registered_defaults;
  // dlssnr.hpp: Upscaled=0, Render=1, Present=2; requested model A=1, B=2.
  settings["NRHookPoint"] = (index == 0 ? 1u : (index == 1 ? 0u : 2u));
  settings["NRPasses"] = (index == 2 ? 2u : 1u);
  settings["NRPreset"] = (index == 0 ? 2u : 1u);
  if (index == 2) {
    settings["NRPass2Preset"] = 2u;
  }
  settings["NRFollowInputRes"] = index != 2;
  settings["NRResolutionScale"] = 1.0;
  return settings;
}

// Validation is mandatory at the application boundary as well as at disk IO.
// Template version is provenance; explicit overrides win when defaults/templates change.
inline Json Resolve(std::size_t index, const Slot& slot, const Json& registered_defaults, const Validator& validator) {
  if (index >= SLOT_IDS.size() || !slot.populated) throw std::invalid_argument("Cannot resolve an invalid or empty Slot.");
  if (slot.template_version > TEMPLATE_VERSION) throw std::invalid_argument("This profile requires a newer template version.");
  std::string error;
  if (!internal::ValidateSettings(registered_defaults, validator, &error)
      || !internal::ValidateSettings(slot.overrides, validator, &error)
      || !internal::ValidateSettings(slot.snapshot, validator, &error)) throw std::invalid_argument(error);
  Json settings;
  if (index < 3) {
    settings = Template(index, registered_defaults);
    settings.update(slot.overrides);
  } else {
    settings = slot.snapshot;
  }
  if (!internal::ValidateSettings(settings, validator, &error)) throw std::invalid_argument(error);
  // Strict encoding also rejects malformed UTF-8 in caller-owned JSON values/keys.
  Json checked;
  if (!presets::ParseJson(settings.dump(), &checked, &error)) throw std::invalid_argument(error);
  return checked;
}

}  // namespace renodx::addons::dlss5::profiles
