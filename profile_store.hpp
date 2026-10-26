/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "preset_store.hpp"

namespace renodx::addons::dlss5::profiles {

using Json = presets::Json;
using Validator = std::function<bool(std::string_view, const Json&)>;

inline constexpr std::array<std::string_view, 6> SLOT_IDS = {
    "official_lite", "official_balanced", "official_max", "custom_slot_1", "custom_slot_2", "custom_slot_3"};

struct Slot {
  std::string name;
  bool populated = false;
  std::uint32_t template_version = 1;
  Json overrides = Json::object();
  Json snapshot = Json::object();
};

struct State {
  std::uint64_t revision = 0;
  std::string owner;
  std::string selected;
  Json last_applied = Json::object();
  std::array<Slot, 6> slots;
};

inline State EmptyState(std::string_view owner) {
  State state{.owner = std::string(owner)};
  for (std::size_t index = 0; index < state.slots.size(); ++index) {
    state.slots[index].populated = index < 3;
  }
  return state;
}

namespace internal {

using presets::internal::Fail;

inline bool ValidateSettings(const Json& settings, const Validator& validator, std::string* error) {
  if (!validator) return Fail(error, "A Slot settings validator is required.");
  if (!settings.is_object() || settings.size() > presets::MAX_SETTINGS) {
    return Fail(error, "Slot settings must be an object with at most 512 entries.");
  }
  for (const auto& [key, value] : settings.items()) {
    if (key.empty() || key.size() > 4096
        || !(value.is_boolean() || value.is_number() || value.is_string())
        || (value.is_number_float() && !std::isfinite(value.get<double>()))
        || (value.is_string() && value.get_ref<const std::string&>().size() > 128)) {
      return Fail(error, "Invalid Slot setting key or scalar value.");
    }
    if (!validator(key, value)) return Fail(error, "Unknown or invalid Slot setting: " + key);
  }
  return true;
}

// The exact key sets are also used on generated documents before publishing.
inline bool Decode(const Json& root, const Validator& validator, State* output, std::string* error) {
  if (!root.is_object() || root.size() != 7 || !root.contains("format") || !root.contains("schema_version")
      || !root.contains("revision") || !root.contains("owner") || !root.contains("selected")
      || !root.contains("last_applied") || !root.contains("slots")) {
    return Fail(error, "Invalid profiles manifest fields.");
  }
  if (root["format"] != "nr-nexus-profiles" || !root["schema_version"].is_number_unsigned()
      || root["schema_version"] != 1) return Fail(error, "Unsupported profiles format or schema version.");
  if (!root["revision"].is_number_unsigned() || !root["owner"].is_string() || !root["selected"].is_string()) {
    return Fail(error, "Invalid profiles revision, owner or selection.");
  }
  State state{.revision = root["revision"].get<std::uint64_t>(),
              .owner = root["owner"].get<std::string>(), .selected = root["selected"].get<std::string>(),
              .last_applied = root["last_applied"]};
  if (state.owner.empty() || state.owner.size() > 128 || state.owner.find('\0') != std::string::npos
      || (!state.selected.empty() && std::find(SLOT_IDS.begin(), SLOT_IDS.end(), state.selected) == SLOT_IDS.end())) {
    return Fail(error, "Invalid profiles owner or Slot ID.");
  }
  if (!root["slots"].is_object() || root["slots"].size() != SLOT_IDS.size()) {
    return Fail(error, "Profiles must contain exactly the six fixed Slot IDs.");
  }
  if (!ValidateSettings(state.last_applied, validator, error)) return false;
  for (std::size_t index = 0; index < SLOT_IDS.size(); ++index) {
    const auto id = SLOT_IDS[index];
    if (!root["slots"].contains(id)) return Fail(error, "Missing fixed Slot ID.");
    const auto& entry = root["slots"][id];
    if (!entry.is_object() || entry.size() != 5 || !entry.contains("name") || !entry.contains("populated")
        || !entry.contains("template_version") || !entry.contains("overrides") || !entry.contains("snapshot")
        || !entry["name"].is_string() || !entry["populated"].is_boolean()
        || !entry["template_version"].is_number_unsigned()) return Fail(error, "Invalid Slot fields or types.");
    const auto version = entry["template_version"].get<std::uint64_t>();
    const auto& name = entry["name"].get_ref<const std::string&>();
    if (version == 0 || version > (std::numeric_limits<std::uint32_t>::max)()
        || name.size() > 128 || name.find('\0') != std::string::npos) return Fail(error, "Invalid Slot name or template version.");
    auto& slot = state.slots[index];
    slot = Slot{.name = name, .populated = entry["populated"].get<bool>(),
                .template_version = static_cast<std::uint32_t>(version),
                .overrides = entry["overrides"], .snapshot = entry["snapshot"]};
    if (!ValidateSettings(slot.overrides, validator, error) || !ValidateSettings(slot.snapshot, validator, error)) return false;
    if (index < 3 && !slot.populated) return Fail(error, "Official Slots must be populated.");
  }
  if (!state.selected.empty()) {
    const auto index = static_cast<std::size_t>(std::find(SLOT_IDS.begin(), SLOT_IDS.end(), state.selected) - SLOT_IDS.begin());
    if (!state.slots[index].populated) return Fail(error, "Selected Slot is empty.");
  }
  *output = std::move(state);
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

inline Json Encode(const State& state) {
  Json slots = Json::object();
  for (std::size_t index = 0; index < SLOT_IDS.size(); ++index) {
    const auto& slot = state.slots[index];
    slots[SLOT_IDS[index]] = {{"name", slot.name}, {"populated", slot.populated},
                            {"template_version", slot.template_version}, {"overrides", slot.overrides}, {"snapshot", slot.snapshot}};
  }
  return {{"format", "nr-nexus-profiles"}, {"schema_version", 1u}, {"revision", state.revision},
          {"owner", state.owner}, {"selected", state.selected}, {"last_applied", state.last_applied},
          {"slots", std::move(slots)}};
}

}  // namespace internal

inline bool StoragePath(std::string_view game_id, std::filesystem::path* output, std::string* error = nullptr) {
  if (output == nullptr) return internal::Fail(error, "Missing profiles path output.");
  if (game_id.size() != 32 || game_id.find_first_not_of("0123456789abcdefABCDEF") != std::string_view::npos) {
    return internal::Fail(error, "Game ID must contain exactly 32 hexadecimal digits.");
  }
  try {
    const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (required == 0 || required > 32768) return internal::Fail(error, "LOCALAPPDATA is unavailable or invalid.");
    std::wstring local(required, L'\0');
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local.data(), required);
    if (length == 0 || length >= required) return internal::Fail(error, "LOCALAPPDATA changed or is invalid.");
    local.resize(length);
    std::string canonical_id(game_id);
    for (auto& digit : canonical_id) {
      if (digit >= 'A' && digit <= 'F') {
        digit += 'a' - 'A';
      }
    }
    const auto directory = std::filesystem::path(local) / NR_NEXUS_NAME / L"Profiles" / canonical_id;
    std::vector<presets::internal::Handle> ancestors;
    if (!presets::internal::OpenDirectory(directory, true, &ancestors, error)) return false;
    *output = directory / L"slots.json";
    if (error != nullptr) {
      error->clear();
    }
    return true;
  } catch (const std::exception&) {
    return internal::Fail(error, "Cannot resolve profiles path.");
  }
}

inline bool Read(const std::filesystem::path& path, const Validator& validator, State* output, std::string* error = nullptr) {
  if (output == nullptr) return internal::Fail(error, "Missing profiles output.");
  try {
    Json root;
    if (!presets::ReadJsonFile(path, &root, error)) return false;
    return internal::Decode(root, validator, output, error);
  } catch (const std::exception&) {
    return internal::Fail(error, "Cannot read profiles or validate settings.");
  }
}

inline bool Write(const std::filesystem::path& path, State* state, std::uint64_t expected_revision,
                  const Validator& validator, std::string* error = nullptr) {
  if (state == nullptr) return internal::Fail(error, "Missing profiles state.");
  try {
    if (!presets::internal::IsComponent(path.filename().native())) return internal::Fail(error, "Unsafe profiles filename.");
    if (state->revision != expected_revision || expected_revision == (std::numeric_limits<std::uint64_t>::max)()) {
      return internal::Fail(error, "Profiles revision conflict or overflow.");
    }
    State checked;
    if (!internal::Decode(internal::Encode(*state), validator, &checked, error)) return false;
    std::vector<presets::internal::Handle> ancestors;
    if (!presets::internal::OpenDirectory(path.parent_path(), false, &ancestors, error)) return false;
    auto lock_path = path;
    lock_path += L".lock";
    // Keep the lock file: deleting it would allow another writer to lock a different object.
    presets::internal::Handle lock(CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                              OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (lock.value == INVALID_HANDLE_VALUE) return presets::internal::WindowsError(error, "Profiles lock unavailable (busy or unsafe)");
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(lock.value, &information)) return presets::internal::WindowsError(error, "Cannot inspect profiles lock");
    if ((information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0
        || information.nNumberOfLinks != 1 || GetFileType(lock.value) != FILE_TYPE_DISK) {
      return internal::Fail(error, "Profiles lock must be a regular file without reparse points or hard links.");
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    const bool exists = attributes != INVALID_FILE_ATTRIBUTES;
    if (exists) {
      State current;
      if (!Read(path, validator, &current, error)) return false;
      if (current.revision != expected_revision) return internal::Fail(error, "Profiles revision conflict; reload before saving.");
      if (current.owner != state->owner) return internal::Fail(error, "Profiles owner mismatch.");
    } else {
      if (GetLastError() != ERROR_FILE_NOT_FOUND) return presets::internal::WindowsError(error, "Cannot inspect profiles destination");
      if (expected_revision != 0) return internal::Fail(error, "Profiles revision conflict; destination is missing.");
    }
    checked.revision = expected_revision + 1;
    const auto text = internal::Encode(checked).dump(2) + "\n";
    if (!presets::WriteJsonFile(path, text, exists,
                               [&](const Json& root, std::string* message) {
                                 State parsed;
                                 return internal::Decode(root, validator, &parsed, message);
                               }, error)) return false;
    state->revision = checked.revision;
    return true;
  } catch (const std::exception&) {
    return internal::Fail(error, "Cannot write profiles or validate settings.");
  }
}

}  // namespace renodx::addons::dlss5::profiles
