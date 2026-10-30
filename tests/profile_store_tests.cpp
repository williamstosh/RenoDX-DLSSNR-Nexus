/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#include <Windows.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include "preset_fields.hpp"
#include "profile_templates.hpp"

namespace {
namespace profiles = renodx::addons::dlss5::profiles;
namespace presets = renodx::addons::dlss5::presets;
namespace fs = std::filesystem;
using profiles::Json;

int checks = 0;

void Check(bool condition, std::string_view message) {
  ++checks;
  if (!condition) throw std::runtime_error(std::string(message));
}

bool Validate(std::string_view key, const Json& value) {
  for (unsigned pass = 2; pass <= 4; ++pass) {
    const auto prefix = "NRPass" + std::to_string(pass);
    if (key == prefix + "ResolutionMode") return value.is_number_integer() && value >= 0 && value <= 2;
    if (key == prefix + "ResolutionScale") return value.is_number() && value >= 0.33f && value <= 1;
  }
  if (key == "NRFollowInputRes") return value.is_boolean();
  if (key == "NRResolutionScale") return value.is_number() && value >= 0.25 && value <= 2.0;
  if (key == "NRIntensity") return value.is_number() && value >= 0 && value <= 1;
  if (key == "NRAutoMask" || key == "NRPass2AutoMask" || key == "NRPass2FollowPass1") return value.is_boolean();
  if (key == "NRSkinStructure" || key == "NRPass2Skin") return value.is_number() && value >= -1 && value <= 1;
  if (!value.is_number_integer()) return false;
  if (key == "NRHookPoint") return value >= 0 && value <= 2;
  if (key == "NRPasses") return value >= 1 && value <= 4;
  if (key == "NRStyle" || key == "NRPass2Style") return value >= 0 && value <= 2;
  if (key == "NRPass2Stage") return value >= 0 && value <= 3;
  if (key == "NRPreset" || key == "NRPass2Preset") return value >= 0 && value <= 3;
  return false;
}

std::string Bytes(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void Put(const fs::path& path, std::string_view bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!file) throw std::runtime_error("Cannot create test fixture.");
}

void Reject(const fs::path& path, profiles::State state, std::string_view label) {
  const auto before = Bytes(path);
  const auto original = profiles::internal::Encode(state);
  std::string error;
  Check(!profiles::Write(path, &state, state.revision, Validate, &error), label);
  Check(!error.empty(), "Failure has a diagnostic");
  Check(Bytes(path) == before, "Failed write preserves original bytes");
  Check(profiles::internal::Encode(state) == original, "Failed write preserves caller state");
}

void BadDocument(const fs::path& path, std::string_view text, const profiles::State& state) {
  Put(path, text);
  auto output = profiles::EmptyState("sentinel");
  const auto original = profiles::internal::Encode(output);
  std::string error;
  Check(!profiles::Read(path, Validate, &output, &error), "Invalid manifest rejected on read");
  Check(!error.empty() && profiles::internal::Encode(output) == original, "Read failure preserves output");
  Reject(path, state, "Invalid existing manifest cannot be overwritten");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    if (argc < 3) throw std::runtime_error("Usage: profile_store_tests root mode [gate]");
    const fs::path root(argv[1]);
    Check(SetEnvironmentVariableW(L"LOCALAPPDATA", (root / L"local-appdata").c_str()) != FALSE, "Set isolated appdata");
    fs::path path;
    std::string error;
    Check(profiles::StoragePath("0123456789abcdef0123456789abcdef", &path, &error), error);
    const std::wstring_view mode(argv[2]);
    if (mode == L"restore") {
      profiles::State restored;
      Check(profiles::Read(path, Validate, &restored, &error), error);
      Check(restored.revision == 2 && restored.selected == "custom_slot_2", "Fresh process revision and selection");
      Check(restored.slots[4].name == "Renamed 2" && restored.slots[4].snapshot["NRIntensity"] == 0.7, "Fresh process data");
      Check(restored.slots[3].name == restored.slots[5].name, "Duplicate display names retain separate IDs");
      for (std::size_t index = 0; index < restored.slots.size(); ++index) {
        const auto& settings = (index < 3 ? restored.slots[index].overrides : restored.slots[index].snapshot);
        for (unsigned pass = 2; pass <= 4; ++pass) {
          const auto prefix = "NRPass" + std::to_string(pass);
          Check(settings[prefix + "ResolutionMode"] == static_cast<unsigned>((index + pass) % 3)
                    && settings[prefix + "ResolutionScale"] == 0.5f + 0.0625f * static_cast<float>(index),
                "Fresh process restores every pass resolution for all six Slots");
        }
      }
      std::cout << checks << " fresh process checks passed\n";
      return 0;
    }
    if (mode == L"race") {
      if (argc != 4) throw std::runtime_error("Missing race gate");
      path = root / L"race.json";
      profiles::State state;
      Check(profiles::Read(path, Validate, &state, &error), error);
      presets::internal::Handle gate(OpenEventW(SYNCHRONIZE, FALSE, argv[3]));
      Check(gate.value != nullptr && gate.value != INVALID_HANDLE_VALUE, "Open race gate");
      const auto suffix = std::to_wstring(GetCurrentProcessId());
      Put(root / (L"ready-" + suffix), "ready");
      Check(WaitForSingleObject(gate.value, 15000) == WAIT_OBJECT_0, "Race gate timeout");
      const bool saved = profiles::Write(path, &state, 1, Validate, &error);
      Check(saved ? state.revision == 2 : state.revision == 1, "Race preserves revision on failure");
      Check(saved || !error.empty(), "Race conflict diagnostic");
      Put(root / (L"result-" + suffix), saved ? "saved" : "conflict");
      return 0;
    }

    auto state = profiles::EmptyState("game-1");
    Check(state.revision == 0 && state.owner == "game-1" && state.selected.empty(), "Empty state identity");
    for (std::size_t index = 0; index < 6; ++index) {
      Check(state.slots[index].populated == (index < 3), "Official and custom population");
      state.slots[index].name = "Slot " + std::to_string(index);
      if (index < 3) {
        state.slots[index].overrides = {{"NRIntensity", 0.1 * static_cast<double>(index + 1)}};
      } else {
        state.slots[index].populated = true;
        state.slots[index].snapshot = {{"NRIntensity", 0.1 * static_cast<double>(index + 1)}};
      }
      auto& settings = (index < 3 ? state.slots[index].overrides : state.slots[index].snapshot);
      for (unsigned pass = 2; pass <= 4; ++pass) {
        const auto prefix = "NRPass" + std::to_string(pass);
        settings[prefix + "ResolutionMode"] = static_cast<unsigned>((index + pass) % 3);
        settings[prefix + "ResolutionScale"] = 0.5f + 0.0625f * static_cast<float>(index);
      }
    }
    state.slots[3].name = state.slots[5].name = "Same name";
    state.slots[4].snapshot["NRIntensity"] = 0.7;
    state.last_applied = state.slots[4].snapshot;
    state.selected = "custom_slot_2";
    Check(profiles::Write(path, &state, 0, Validate, &error), error);
    Check(state.revision == 1, "First write increments revision");
    profiles::State read;
    Check(profiles::Read(path, Validate, &read, &error), error);
    Check(profiles::internal::Encode(read) == profiles::internal::Encode(state), "Six Slot roundtrip");
    const auto manifest = Json::parse(Bytes(path));
    for (const auto id : profiles::SLOT_IDS) Check(manifest["slots"].contains(id), "Fixed Slot ID persisted");
    Check(manifest["format"] == "nr-nexus-profiles" && manifest["schema_version"] == 1, "Independent manifest schema");
    state.slots[4].name = "Renamed 2";
    Check(profiles::Write(path, &state, 1, Validate, &error), error);
    Check(state.slots[4].snapshot == read.slots[4].snapshot, "Rename independent of snapshot and ID");
    Reject(path, read, "Stale revision rejected");

    Json defaults{{"NRHookPoint", 0u}, {"NRPasses", 4u}, {"NRPreset", 0u}, {"NRPass2Preset", 0u},
                        {"NRFollowInputRes", false}, {"NRResolutionScale", 0.5}, {"NRIntensity", 0.9},
                        {"NRStyle", 0u}, {"NRAutoMask", false}, {"NRSkinStructure", 0.f},
                        {"NRPass2Stage", 0u}, {"NRPass2FollowPass1", true}, {"NRPass2Style", 0u},
                        {"NRPass2AutoMask", false}, {"NRPass2Skin", 0.f}};
    for (unsigned pass = 2; pass <= 4; ++pass) {
      const auto prefix = "NRPass" + std::to_string(pass);
      const auto* mode_rule = presets::FindRule(prefix + "ResolutionMode");
      const auto* scale = presets::FindRule(prefix + "ResolutionScale");
      Check(mode_rule != nullptr && mode_rule->category == presets::FieldRule::MULTIPASS && mode_rule->pass == pass
                && mode_rule->minimum == 0 && mode_rule->maximum == 2, "Resolution mode allowlist and pass identity");
      Check(scale != nullptr && scale->category == presets::FieldRule::MULTIPASS && scale->pass == pass
                && scale->minimum == 0.33f && scale->maximum == 1, "Resolution scale allowlist and bounds");
      defaults[prefix + "ResolutionMode"] = 0u;
      defaults[prefix + "ResolutionScale"] = 1.f;
    }
    const auto defaults_before = defaults;
    auto nondefault_styles = defaults;
    nondefault_styles["NRStyle"] = 1u;
    nondefault_styles["NRPass2Style"] = 2u;
    Check(profiles::Template(0, nondefault_styles)["NRStyle"] == 0u
              && profiles::Template(1, nondefault_styles)["NRStyle"] == 0u
              && profiles::Template(2, nondefault_styles)["NRPass2Style"] == 0u,
          "Candidate default style is explicit even when registered fallback changes");
    const auto lite = profiles::Template(0, defaults);
    const auto balanced = profiles::Template(1, defaults);
    const auto maximum = profiles::Template(2, defaults);
    Check(lite["NRHookPoint"] == 1 && lite["NRPreset"] == 2 && lite["NRPasses"] == 1 && lite["NRFollowInputRes"] == false, "Lite candidate");
    Check(balanced["NRHookPoint"] == 0 && balanced["NRPreset"] == 1 && balanced["NRPasses"] == 1 && balanced["NRFollowInputRes"] == false, "Balanced candidate");
    Check(maximum["NRHookPoint"] == 1 && maximum["NRPreset"] == 2 && maximum["NRPass2Preset"] == 1
              && maximum["NRPasses"] == 2 && maximum["NRFollowInputRes"] == false, "Max candidate");
    Check(maximum["NRStyle"] == 2u && maximum["NRAutoMask"] == true && maximum["NRSkinStructure"] == 0.75f,
          "Max first pass cinematic auto-mask and skin");
    Check(maximum["NRPass2Stage"] == 2u && maximum["NRPass2FollowPass1"] == false
              && maximum["NRPass2Style"] == 0u && maximum["NRPass2AutoMask"] == true && maximum["NRPass2Skin"] == 0.75f,
          "Max second pass has independent Upscaled default-style auto-mask and skin");
    for (std::size_t index = 0; index < 3; ++index) {
      const auto candidate = profiles::Template(index, defaults);
      Check(candidate["NRResolutionScale"] == 0.67f && candidate["NRIntensity"] == 0.9, "Only candidate fields override defaults");
      for (unsigned pass = 2; pass <= 4; ++pass) {
        const auto prefix = "NRPass" + std::to_string(pass);
        Check(candidate[prefix + "ResolutionMode"] == (index == 2 && pass == 2 ? 2u : 0u)
                  && candidate[prefix + "ResolutionScale"] == (index == 2 && pass == 2 ? 0.67f : 1.f),
              "Only Max Pass2 overrides inherited resolution; inactive passes retain defaults");
      }
      const auto resolved = profiles::Resolve(index, state.slots[index], defaults, Validate);
      Check(resolved["NRIntensity"] == state.slots[index].overrides["NRIntensity"], "Official overrides isolated");
    }
    auto updated_defaults = defaults;
    updated_defaults["NRIntensity"] = 0.6;
    auto override_slot = state.slots[0];
    override_slot.overrides["NRPreset"] = 1;
    const auto override_before = override_slot.overrides;
    Check(profiles::Resolve(0, override_slot, updated_defaults, Validate)["NRPreset"] == 1, "Explicit override wins over template");
    Check(profiles::Resolve(0, override_slot, updated_defaults, Validate)["NRIntensity"] == 0.1, "Override survives changed registered defaults");
    Check(override_slot.overrides == override_before && defaults == defaults_before, "Resolution does not mutate inputs");
    override_slot.overrides.erase("NRIntensity");
    Check(profiles::Resolve(0, override_slot, updated_defaults, Validate)["NRIntensity"] == 0.6, "Missing override inherits new default");
    Check(profiles::Resolve(4, state.slots[4], updated_defaults, Validate) == state.slots[4].snapshot, "Custom snapshot remains independent");
    Check(profiles::TEMPLATE_VERSION == 2 && profiles::DEVELOPMENT_CANDIDATE, "Independent candidate template version");
    auto old_max = state.slots[2];
    old_max.template_version = 1;
    old_max.snapshot = {{"NRHookPoint", 2u}, {"NRPass2Preset", 2u}};
    old_max.overrides = {{"NRPass2Skin", 0.5f}, {"NRResolutionScale", 0.8f}};
    const auto upgraded = profiles::Resolve(2, old_max, defaults, Validate);
    Check(upgraded["NRHookPoint"] == 1u && upgraded["NRPass2Preset"] == 1u
              && upgraded["NRPass2Skin"] == 0.5f && upgraded["NRResolutionScale"] == 0.8f
              && upgraded["NRPass2ResolutionScale"] == 0.67f, "Upgrade combines new template with field-level explicit overrides, not old snapshot");
    auto old_custom = old_max;
    old_custom.snapshot = {{"NRStyle", 1u}, {"NRPass2Style", 2u}, {"NRPasses", 4u}};
    Check(profiles::Resolve(3, old_custom, defaults, Validate) == old_custom.snapshot,
          "v1 custom snapshots preserve styles, pass count and missing fields during v2 migration");
    old_max.overrides = Json::object();
    Check(profiles::Resolve(2, old_max, defaults, Validate) == maximum, "Restore removes overrides and resolves current official template");
    auto throws = [&](auto action) {
      bool rejected = false;
      try { action(); } catch (const std::exception&) { rejected = true; }
      Check(rejected, "Invalid application input rejected");
    };
    auto unknown_slot = state.slots[0];
    unknown_slot.overrides["UnknownSetting"] = 1;
    throws([&] { profiles::Resolve(0, unknown_slot, defaults, Validate); });
    auto unknown_defaults = defaults;
    unknown_defaults["UnknownSetting"] = 1;
    throws([&] { profiles::Resolve(0, state.slots[0], unknown_defaults, Validate); });
    throws([&] { profiles::Resolve(6, state.slots[0], defaults, Validate); });
    throws([&] { profiles::Resolve(0, state.slots[0], defaults, {}); });
    throws([&] { profiles::Template(0, Json::object()); });

    fs::path other;
    Check(profiles::StoragePath("fedcba9876543210fedcba9876543210", &other, &error) && other != path, "Game IDs isolated");
    auto other_state = profiles::EmptyState("game-2");
    Check(profiles::Write(other, &other_state, 0, Validate, &error), error);
    Check(profiles::Read(path, Validate, &read, &error) && read.owner == "game-1" && read.revision == 2, "Second game preserves first");
    const auto preserved_path = other;
    fs::path canonical;
    Check(profiles::StoragePath("0123456789ABCDEF0123456789ABCDEF", &canonical, &error) && canonical == path, "Hex ID case canonicalized");
    for (const auto bad_id : {"", "../outside", "0123456789abcdef0123456789abcdeg"}) {
      Check(!profiles::StoragePath(bad_id, &other, &error) && other == preserved_path, "Unsafe game ID rejected");
    }
    Check(!profiles::StoragePath("0123456789abcdef0123456789abcdef", nullptr, &error), "Null path output");

    for (int field = 0; field < 3; ++field) {
      auto invalid = state;
      if (field == 0) invalid.last_applied["UnknownSetting"] = 1;
      if (field == 1) invalid.slots[0].overrides["UnknownSetting"] = 1;
      if (field == 2) invalid.slots[4].snapshot["UnknownSetting"] = 1;
      Reject(path, invalid, "Every settings object validates unknown fields");
    }
    auto invalid = state;
    invalid.slots[0].overrides["NRPasses"] = 99;
    Reject(path, invalid, "Invalid settings range");
    invalid = state;
    invalid.slots[4].name = std::string(129, 'x');
    Reject(path, invalid, "Name byte limit");
    invalid = state;
    invalid.owner = "other-owner";
    Reject(path, invalid, "Owner mismatch");
    invalid = state;
    invalid.revision = (std::numeric_limits<std::uint64_t>::max)();
    Reject(path, invalid, "Revision overflow");
    invalid = state;
    invalid.slots[4].snapshot["NRIntensity"] = (std::numeric_limits<double>::infinity)();
    Reject(path, invalid, "Nonfinite scalar");
    invalid.slots[4].snapshot["NRIntensity"] = std::string(129, 'x');
    Reject(path, invalid, "Setting string byte limit");
    invalid = state;
    invalid.slots[4].snapshot["NRIntensity"] = Json::array();
    Reject(path, invalid, "Nested setting value");
    invalid = state;
    invalid.slots[4].populated = false;
    Reject(path, invalid, "Selection cannot reference empty Slot");
    const auto original = Bytes(path);
    auto empty_validator_state = state;
    Check(!profiles::Write(path, &empty_validator_state, 2, {}, &error) && Bytes(path) == original, "Validator required");
    Check(!profiles::Read(path, {}, &read, &error), "Read requires validator");
    Check(!profiles::Read(path, Validate, nullptr, &error) && !profiles::Write(path, nullptr, 0, Validate, &error), "Null state handling");

    const auto bad_path = root / L"bad.json";
    const auto valid_root = profiles::internal::Encode(state);
    BadDocument(bad_path, "{", state);
    BadDocument(bad_path, "{\"format\":1,\"format\":2}", state);
    BadDocument(bad_path, std::string(presets::MAX_FILE_BYTES + 1, ' '), state);
    BadDocument(bad_path, "[[[[[[[[[0]]]]]]]]]", state);
    for (int variant = 0; variant < 14; ++variant) {
      auto document = valid_root;
      switch (variant) {
        case 0: document["schema_version"] = 2; break;
        case 1: document["unknown"] = 1; break;
        case 2: document["slots"].erase("custom_slot_3"); break;
        case 3: document["slots"]["custom_slot_3"]["unknown"] = 1; break;
        case 4: document["last_applied"]["UnknownSetting"] = 1; break;
        case 5: document["slots"]["official_lite"]["overrides"]["NRPasses"] = 0; break;
        case 6: document["slots"]["custom_slot_1"]["snapshot"]["UnknownSetting"] = 1; break;
        case 7: document["slots"]["official_lite"]["populated"] = false; break;
        case 8: document["selected"] = "unknown"; break;
        case 9: document["revision"] = -1; break;
        case 10: document["revision"] = 1.0; break;
        case 11: document["slots"]["official_lite"]["template_version"] = 4294967296ULL; break;
        case 12: document["format"] = "nr-nexus-preset"; break;
        case 13: document["slots"]["official_lite"]["overrides"] = Json::array(); break;
      }
      BadDocument(bad_path, document.dump(), state);
    }
    auto duplicate = original;
    duplicate.insert(duplicate.find('{') + 1, "\"revision\":0,");
    BadDocument(bad_path, duplicate, state);

    // Real directory junctions are prepared by the runner without requiring symlink privileges.
    auto fresh = profiles::EmptyState("fresh");
    Reject(root / L"ancestor-link" / L"slots.json", fresh, "Ancestor reparse point");
    Reject(root / L"destination-link", fresh, "Destination reparse point");
    Reject(root / L".." / L"escape.json", fresh, "Traversal path");
    Reject(fs::path(L"relative.json"), fresh, "Relative path");
    const auto hard = root / L"hard.json";
    Check(CreateHardLinkW(hard.c_str(), path.c_str(), nullptr) != FALSE, "Create hardlink fixture");
    Reject(hard, state, "Destination hardlink");
    Check(!profiles::Read(hard, Validate, &read, &error), "Read rejects hardlink");
    Check(DeleteFileW(hard.c_str()) != FALSE, "Remove owned hardlink fixture");
    const auto lock_hard = root / L"lock-hard.json";
    const auto lock_sentinel = root / L"lock-sentinel";
    Put(lock_sentinel, "lock sentinel");
    Check(CreateHardLinkW((root / L"lock-hard.json.lock").c_str(), lock_sentinel.c_str(), nullptr) != FALSE, "Create lock hardlink");
    Reject(lock_hard, fresh, "Lock hardlink");
    Check(Bytes(lock_sentinel) == "lock sentinel", "Lock target unchanged");
    Reject(root / L"lock-reparse.json", fresh, "Lock reparse point");
    const auto outside = Bytes(root / L"outside" / L"sentinel");
    Check(outside == "outside sentinel", "Reparse destination remains intact");
    fs::path unchanged = preserved_path;
    Check(SetEnvironmentVariableW(L"LOCALAPPDATA", (root / L"ancestor-link").c_str()) != FALSE, "Set reparse appdata");
    Check(!profiles::StoragePath("0123456789abcdef0123456789abcdef", &unchanged, &error) && unchanged == preserved_path, "StoragePath rejects reparse ancestor");
    Check(SetEnvironmentVariableW(L"LOCALAPPDATA", L"relative") != FALSE, "Set relative appdata");
    Check(!profiles::StoragePath("0123456789abcdef0123456789abcdef", &unchanged, &error), "StoragePath rejects relative appdata");
    Check(SetEnvironmentVariableW(L"LOCALAPPDATA", (root / L"local-appdata").c_str()) != FALSE, "Restore isolated appdata");

    auto lock_path = path;
    lock_path += L".lock";
    {
      presets::internal::Handle held(CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
      Check(held.value != INVALID_HANDLE_VALUE, "Acquire held lock");
      Reject(path, state, "Concurrent lock conflict");
    }
    {
      presets::internal::Handle held(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
      Check(held.value != INVALID_HANDLE_VALUE, "Acquire destination handle");
      Reject(path, state, "Atomic publication failure");
    }
    Check(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE, "Set readonly fixture");
    Reject(path, state, "Readonly publication failure");
    Check(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL) != FALSE, "Restore fixture attributes");
    for (const auto& entry : fs::directory_iterator(path.parent_path())) {
      Check(!entry.path().filename().native().ends_with(L".tmp"), "Failed writes clean their temporary file");
    }

    auto race_state = profiles::EmptyState("race-owner");
    const auto race_path = root / L"race.json";
    Check(profiles::Write(race_path, &race_state, 0, Validate, &error), error);
    const auto gate_name = L"Local\\NRNexusProfileTest-" + std::to_wstring(GetCurrentProcessId());
    presets::internal::Handle gate(CreateEventW(nullptr, TRUE, FALSE, gate_name.c_str()));
    Check(gate.value != nullptr && gate.value != INVALID_HANDLE_VALUE, "Create race gate");
    wchar_t executable[32768]{};
    Check(GetModuleFileNameW(nullptr, executable, 32768) != 0, "Get test executable");
    std::array<presets::internal::Handle, 2> processes;
    std::array<DWORD, 2> ids{};
    for (std::size_t index = 0; index < processes.size(); ++index) {
      std::wstring command = L"\"" + std::wstring(executable) + L"\" \"" + root.native() + L"\" race \"" + gate_name + L"\"";
      STARTUPINFOW startup{};
      startup.cb = sizeof(startup);
      PROCESS_INFORMATION process{};
      Check(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE, "Start independent CAS writer");
      processes[index] = presets::internal::Handle(process.hProcess);
      presets::internal::Handle thread(process.hThread);
      ids[index] = process.dwProcessId;
    }
    const auto deadline = GetTickCount64() + 10000;
    for (const auto id : ids) {
      while (!fs::exists(root / (L"ready-" + std::to_wstring(id))) && GetTickCount64() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      Check(fs::exists(root / (L"ready-" + std::to_wstring(id))), "Both writers read same revision");
    }
    Check(SetEvent(gate.value) != FALSE, "Release concurrent writers");
    int saved = 0;
    for (std::size_t index = 0; index < processes.size(); ++index) {
      Check(WaitForSingleObject(processes[index].value, 15000) == WAIT_OBJECT_0, "Concurrent writer completes");
      DWORD code = 1;
      Check(GetExitCodeProcess(processes[index].value, &code) && code == 0, "Concurrent writer checks passed");
      const auto result = Bytes(root / (L"result-" + std::to_wstring(ids[index])));
      Check(result == "saved" || result == "conflict", "CAS result recorded");
      if (result == "saved") ++saved;
    }
    Check(saved == 1, "Exactly one concurrent CAS writer succeeds");
    Check(profiles::Read(race_path, Validate, &read, &error) && read.revision == 2, "Concurrent CAS stores single increment");
    Check(Bytes(path) == original, "All rejected writes preserve primary fixture");

    // Exercise legacy API after the shared strict parser/file-IO extraction.
    presets::Document preset{.settings = {{"NRIntensity", 0.4}}};
    const auto preset_path = root / L"legacy.nrnexus.json";
    Check(presets::WriteFile(preset_path, preset, false, &error), error);
    presets::Document restored_preset;
    Check(presets::ReadFile(preset_path, &restored_preset, &error) && restored_preset.settings == preset.settings, "Legacy preset roundtrip");
    Check(!presets::WriteFile(preset_path, preset, false, &error), "Legacy explicit overwrite required");
    Check(presets::WriteFile(preset_path, preset, true, &error), "Legacy atomic overwrite");
    preset.metadata["unknown"] = "invalid";
    Check(!presets::WriteFile(preset_path, preset, true, &error), "Legacy schema validation retained");
    std::cout << checks << " profile store checks passed\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "Check " << checks << ": " << exception.what() << '\n';
    return 1;
  }
}
