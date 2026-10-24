/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#define DEBUG_LEVEL_0

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include <dlssnr.hpp>

// Only the ReShade host boundary is substituted. Production LoadConfiguration,
// raw section discovery, registry, typed getters and save decisions remain real.
// Expected integration API: bool internal::SavePresentRestoreScope(scope::Scope).
// The wrapper can select another function name with the same signature.
#ifndef CONFIG_TEST_SCOPE_SAVE
#define CONFIG_TEST_SCOPE_SAVE SavePresentRestoreScope
#endif

namespace {
namespace runtime = renodx::addons::dlss5::internal;
namespace scope = renodx::addons::dlss5::present_restore_scope;

constexpr const char* SCOPE_KEY = "NRPresentRestoreScope";
constexpr const char* VERSION_KEY = "NRPresentRestoreScopeConfigVersion";
std::filesystem::path base_directory;
std::string ini_path;
std::string failed_key;
uint32_t checks = 0;
uint32_t failures = 0;
uint32_t reads = 0;
uint32_t base_queries = 0;
uint32_t non_global_calls = 0;

struct WriteEvent {
  std::string key;
  std::string value;
  bool stored = false;
  bool pair_present_at_global = false;
};
std::vector<WriteEvent> writes;

void Check(bool condition, const char* description) {
  ++checks;
  if (condition) return;
  ++failures;
  std::cerr << "FAIL: " << description << '\n';
}

std::string Read(const char* key) {
  char value[4096]{};
  GetPrivateProfileStringA(runtime::kConfigSection, key, "<absent>", value, sizeof(value), ini_path.c_str());
  return value;
}

std::string Bytes() {
  std::ifstream file(ini_path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

bool WasWritten(const char* key) {
  return std::any_of(writes.begin(), writes.end(), [key](const WriteEvent& event) { return event.key == key; });
}

scope::Configuration Configuration() {
  std::shared_lock lock(runtime::present_restore_configuration.mutex);
  return runtime::present_restore_configuration.scope;
}

void CheckPair(const char* value) {
  Check(Read(SCOPE_KEY) == value, "persisted scope value");
  Check(Read(VERSION_KEY) == "1", "persisted scope schema 1");
}

void CheckInherited() {
  Check(Configuration().inherited, "production load uses CompatibilityInherited");
  Check(!WasWritten(SCOPE_KEY) && !WasWritten(VERSION_KEY), "load does not write scope pair");
}
}  // namespace

extern "C" __declspec(dllexport) void ReShadeLogMessage(void*, int, const char* message) {
  std::cout << "production: " << message << '\n';
}

extern "C" __declspec(dllexport) bool ReShadeGetBasePath(char* path, size_t* size) {
  ++base_queries;
  const auto base = base_directory.string();
  if (!path) {
    *size = base.size() + 1;
  } else if (*size != 0) {
    const size_t copied = std::min(*size - 1, base.size());
    std::memcpy(path, base.data(), copied);
    path[copied] = '\0';
    *size = copied;
  }
  return true;
}

extern "C" __declspec(dllexport) bool ReShadeGetConfigValue(
    void*, reshade::api::effect_runtime* effect_runtime, const char* section,
    const char* key, char* value, size_t* size) {
  ++reads;
  if (effect_runtime) ++non_global_calls;
  char buffer[32768]{};
  // Profile enumeration returns the same double-NUL convention as INI lists.
  const DWORD length = GetPrivateProfileStringA(section, key, "", buffer, sizeof(buffer), ini_path.c_str());
  if (length == 0) {
    *size = 0;
    return false;
  }
  // ReShade splits comma-separated config values into a NUL-separated list.
  if (section && key) {
    std::replace(buffer, buffer + length, ',', '\0');
  }
  // Match source/addon.cpp: a query includes an extra terminator; a read
  // reports bytes copied, including the last element's terminator.
  if (!value) {
    *size = length + 2;
  } else if (*size != 0) {
    const size_t copied = std::min(*size - 1, static_cast<size_t>(length) + 1);
    std::memcpy(value, buffer, copied);
    value[copied] = '\0';
    *size = copied;
  }
  return true;
}

extern "C" __declspec(dllexport) void ReShadeSetConfigValue(
    void*, reshade::api::effect_runtime* effect_runtime, const char* section,
    const char* key, const char* value) {
  if (effect_runtime) ++non_global_calls;
  const bool stored = failed_key != key
                      && WritePrivateProfileStringA(section, key, value, ini_path.c_str()) != FALSE;
  WritePrivateProfileStringA(nullptr, nullptr, nullptr, ini_path.c_str());
  writes.push_back({.key = key, .value = (value ? value : "<deleted>"), .stored = stored,
                    .pair_present_at_global = Read(SCOPE_KEY) == "1" && Read(VERSION_KEY) == "1"
                        && Read("NRPresentSdrRestorePolicy") == "1"
                        && Read("NRPresentHdrPedestalPolicy") == "1"
                        && Read("NRDebugBypassSdrRestore") == "0"
                        && Read("NRDebugBypassHdrPedestal") == "0"});
}

int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::runtime_error("usage: configuration-tests.exe CASE (private cwd required)");
    const std::string test_case = argv[1];
    base_directory = std::filesystem::current_path();
    ini_path = (base_directory / "ReShade.ini").string();
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    const std::string policy_keys =
        "NRPresentSdrRestorePolicy=0\r\nNRPresentSdrRestoreConfigVersion=1\r\n"
        "NRSdrBlackRestore=2\r\nNRPresentHdrPedestalPolicy=2\r\n"
        "NRPresentHdrPedestalConfigVersion=1\r\nNRDisplayPedestal=2\r\n"
        "NRDebugBypassSdrRestore=1\r\nNRDebugBypassHdrPedestal=1\r\n";
    const std::string current_global = "ConfigVersion=" + std::to_string(runtime::kConfigVersion) + "\r\n";
    Check(runtime::kConfigVersion == 8, "production global schema is v8");
    const bool migration = test_case.starts_with("pre8-");
    std::string addon;
    if (migration) {
      addon = "ConfigVersion=7\r\nNRIntensity=0.75\r\nUnregisteredPersistentKnob=42\r\n"
              "NRDebugBypassSdrRestore=1\r\nNRDebugBypassHdrPedestal=1\r\n"
              "NRSdrBlackRestore=2\r\nNRDisplayPedestal=2\r\n";
      if (test_case != "pre8-legacy-key") {
        addon += "NRPresentRestoreScope=0\r\nNRPresentRestoreScopeConfigVersion=1\r\n"
                 "NRPresentSdrRestorePolicy=0\r\nNRPresentSdrRestoreConfigVersion=1\r\n"
                 "NRPresentHdrPedestalPolicy=0\r\nNRPresentHdrPedestalConfigVersion=1\r\n";
      }
      if (test_case == "pre8-future-scope") {
        addon.replace(addon.find("NRPresentRestoreScopeConfigVersion=1"),
                      std::strlen("NRPresentRestoreScopeConfigVersion=1"), "NRPresentRestoreScopeConfigVersion=99");
      }
      if (test_case == "pre8-future-policy") {
        addon.replace(addon.find("NRPresentSdrRestoreConfigVersion=1"),
                      std::strlen("NRPresentSdrRestoreConfigVersion=1"), "NRPresentSdrRestoreConfigVersion=99");
        addon.replace(addon.find("NRPresentHdrPedestalConfigVersion=1"),
                      std::strlen("NRPresentHdrPedestalConfigVersion=1"), "NRPresentHdrPedestalConfigVersion=99");
      }
      if (test_case == "pre8-malformed-sdr-version" || test_case == "pre8-future-sdr-version") {
        addon.replace(addon.find("NRPresentSdrRestoreConfigVersion=1"),
                      std::strlen("NRPresentSdrRestoreConfigVersion=1"),
                      (test_case == "pre8-malformed-sdr-version"
                           ? "NRPresentSdrRestoreConfigVersion=garbage" : "NRPresentSdrRestoreConfigVersion=99"));
      }
    } else if (test_case == "old-arbitrary") {
      addon = "UnregisteredPersistentKnob=42\r\n";
    } else if (test_case == "old-registry") {
      addon = current_global + "NRIntensity=0.75\r\n";
    } else if (test_case == "v8-legacy-key") {
      addon = current_global + "NRSdrBlackRestore=2\r\n"
              "NRPresentRestoreScope=0\r\nNRPresentRestoreScopeConfigVersion=1\r\n";
    } else if (test_case == "new" || test_case == "fail-scope" || test_case == "fail-version"
               || test_case == "new-reload" || test_case == "empty-section"
               || test_case == "fail-sdr" || test_case == "fail-hdr" || test_case == "fail-bypass"
               || test_case == "fail-sdr-version" || test_case == "fail-hdr-version") {
      // Empty add-on settings; other ReShade sections do not make it an old add-on.
    } else {
      addon = current_global + policy_keys;
      if (test_case == "scope0" || test_case == "select" || test_case == "save-fail-scope"
          || test_case == "save-fail-version" || test_case == "reset") {
        addon += "NRPresentRestoreScope=0\r\nNRPresentRestoreScopeConfigVersion=1\r\n";
        if (test_case == "reset") addon += "UnregisteredPersistentKnob=42\r\n";
      } else if (test_case == "scope1") {
        addon += "NRPresentRestoreScope=1\r\nNRPresentRestoreScopeConfigVersion=1\r\n";
      } else if (test_case == "malformed-scope") {
        addon += "NRPresentRestoreScope=garbage\r\nNRPresentRestoreScopeConfigVersion=1\r\n";
      } else if (test_case == "malformed-version") {
        addon += "NRPresentRestoreScope=1\r\nNRPresentRestoreScopeConfigVersion=garbage\r\n";
      } else if (test_case == "future-scope") {
        addon += "NRPresentRestoreScope=2\r\nNRPresentRestoreScopeConfigVersion=1\r\n";
      } else if (test_case == "future-version") {
        addon += "NRPresentRestoreScope=1\r\nNRPresentRestoreScopeConfigVersion=99\r\n";
      } else if (test_case == "future-global" || test_case == "malformed-global") {
        addon = "ConfigVersion=" + std::string(test_case == "future-global" ? "999" : "garbage")
                + "\r\n" + policy_keys + "NRPresentRestoreScope=1\r\nNRPresentRestoreScopeConfigVersion=1\r\n";
      } else if (test_case == "future-policy") {
        addon = current_global + "NRPresentSdrRestorePolicy=99\r\nNRPresentSdrRestoreConfigVersion=99\r\n"
                "NRPresentHdrPedestalPolicy=99\r\nNRPresentHdrPedestalConfigVersion=99\r\n"
                "NRPresentRestoreScope=0\r\nNRPresentRestoreScopeConfigVersion=1\r\n";
      } else if (test_case.starts_with("nr-model-")) {
        addon += "NRPresentRestoreScope=1\r\nNRPresentRestoreScopeConfigVersion=1\r\n";
        if (test_case == "nr-model-explicit") {
          addon += "NRPreset=1\r\nNRPass2Preset=2\r\nNRPass3Preset=1\r\nNRPass4Preset=0\r\nNRPass2FollowPass1=1\r\n";
        } else if (test_case == "nr-model-invalid") {
          addon += "NRPreset=99\r\nNRPass2Preset=99\r\nNRPass3Preset=3\r\n";
        } else if (test_case == "nr-model-legacy") {
          addon += "NRPreset=3\r\nNRPass2Preset=2\r\n";
        }
      } else if (test_case == "incomplete-pair") {
        addon += "NRPresentRestoreScope=1\r\n";
      } else {
        throw std::runtime_error("unknown case: " + test_case);
      }
    }
    if (test_case != "new-reload") {
      std::ofstream fixture(ini_path, std::ios::binary | std::ios::trunc);
      fixture << "[GENERAL]\r\nPresetPath=other.ini\r\n[OTHER.ADDON]\r\nPersistent=17\r\n";
      if (!addon.empty() || test_case == "empty-section") fixture << "[RenoDX.DLSS5]\r\n" << addon;
      if (!fixture) throw std::runtime_error("fixture write failed");
    }
    const auto before = Bytes();
    if (test_case == "fail-scope" || test_case == "pre8-fail-scope") failed_key = SCOPE_KEY;
    if (test_case == "fail-version") failed_key = VERSION_KEY;
    if (test_case == "pre8-fail-sdr" || test_case == "fail-sdr") failed_key = "NRPresentSdrRestorePolicy";
    if (test_case == "pre8-fail-hdr" || test_case == "fail-hdr") failed_key = "NRPresentHdrPedestalPolicy";
    if (test_case == "pre8-fail-bypass" || test_case == "fail-bypass") failed_key = "NRDebugBypassHdrPedestal";
    if (test_case == "pre8-fail-sdr-bypass") failed_key = "NRDebugBypassSdrRestore";
    if (test_case == "fail-sdr-version") failed_key = "NRPresentSdrRestoreConfigVersion";
    if (test_case == "fail-hdr-version") failed_key = "NRPresentHdrPedestalConfigVersion";
    runtime::LoadConfiguration();
    if (test_case.starts_with("nr-model-")) {
      namespace model = renodx::addons::dlss5::nr_model;
      Check(model::SupportsSmallModel(310, 9, 16, 0), "identified 310.9.16 supports B");
      Check(!model::SupportsSmallModel(310, 8, 3, 0) && !model::SupportsSmallModel(310, 9, 15, 0)
                && !model::SupportsSmallModel(310, 9, 17, 0) && !model::SupportsSmallModel(0, 0, 0, 0),
            "old and unknown runtimes do not expose unverified B capability");
      Check(model::ResolvePreset(0, 0, true) == 2 && model::ResolvePreset(0, 0, false) == 0,
            "automatic is B only on the identified runtime");
      Check(model::ResolvePreset(1, 0, true) == 1 && model::ResolvePreset(2, 0, true) == 2,
            "stack inheritance preserves explicit main A/B");
      Check(model::ResolvePreset(1, 2, true) == 2 && model::ResolvePreset(2, 1, true) == 1,
            "A to B and B to A select independent networks");
      Check(model::ResolvePreset(2, 2, true) == 2 && model::ResolvePreset(1, 1, true) == 1,
            "B to B and A to A remain supported");
      Check(model::ResolvePreset(2, 0, false) == 1 && model::ResolvePreset(1, 2, false) == 1,
            "unsupported B falls back to A for main and explicit stack pass");
      Check(model::ResolvePreset(3, 0, false) == 3 && model::ResolvePreset(3, 0, true) == 1,
            "legacy preset 3 round trips and safely falls back on 310.9.16");
      if (test_case == "nr-model-explicit") {
        Check(runtime::preset == 1 && runtime::pass_preset[0] == 2 && runtime::pass_preset[1] == 1
                  && runtime::pass_preset[2] == 0 && runtime::pass_follow[0],
              "production load preserves explicit pass models independently of appearance sharing");
      } else if (test_case == "nr-model-legacy") {
        Check(runtime::preset == 3 && runtime::pass_preset[0] == 2, "production load preserves legacy main and unsupported B request");
      } else {
        Check(runtime::preset == 0 && runtime::pass_preset[0] == 0 && runtime::pass_preset[1] == 0
                  && runtime::pass_preset[2] == 0, "missing/invalid selections use automatic and inherit defaults");
      }
      runtime::PersistConfig();
      Check(Read("NRPreset") == std::to_string(runtime::preset.load())
                && Read("NRPass2Preset") == std::to_string(runtime::pass_preset[0].load())
                && Read("NRPass3Preset") == std::to_string(runtime::pass_preset[1].load())
                && Read("NRPass4Preset") == std::to_string(runtime::pass_preset[2].load()),
            "production persistence saves raw model choices, not runtime fallback values");
      const uint64_t generation = runtime::configuration_generation.load();
      runtime::kSetPreset.Commit(2.f);
      runtime::kSetPassPreset[0].Commit(1.f);
      Check(runtime::configuration_generation.load() == generation + 2,
            "main and per-pass model changes each trigger production feature recreation");
      runtime::kSetPassPreset[0].Commit(0.f);
      Check(runtime::pass_preset[0] == 0 && Read("NRPass2Preset") == "0", "inherit reset is persisted");
      renodx::addons::dlss5::bridge::OwnedParameters parameters;
      for (uint32_t selected : {1u, 2u}) {
        runtime::SetNrCreateParameters(&parameters, 640, 360, 640, 360, 0, false, selected);
        uint32_t sent = 99;
        Check(NVSDK_NGX_SUCCEED(parameters.Get("DLSSNR.Hint.Render.Preset", &sent)) && sent == selected,
              "production create block writes the resolved network as an unsigned NGX parameter");
      }
    }
    Check(reads > 0, "production load called real ReShade typed/raw boundary");
    Check(runtime::ReShadeBaseDirectory() == base_directory && base_queries > 0,
          "production raw INI base path is the controlled fixture directory");
    Check(non_global_calls == 0, "configuration I/O targets global INI");

    if (migration || test_case == "old-arbitrary") {
      const bool failed = test_case.find("-fail-") != std::string::npos;
      const bool read_only = test_case == "pre8-future-scope" || test_case == "pre8-future-policy"
          || test_case == "pre8-malformed-sdr-version" || test_case == "pre8-future-sdr-version";
      if (read_only) {
        Check(Read("ConfigVersion") == "7" && !WasWritten("ConfigVersion"), "unsupported schema is not stamped v8");
        if (test_case == "pre8-future-scope") {
          Check(Read(VERSION_KEY) == "99" && Read(SCOPE_KEY) == "0", "future scope pair preserved");
          Check(!WasWritten(SCOPE_KEY) && !WasWritten(VERSION_KEY), "future scope receives no writes");
        } else if (test_case == "pre8-future-policy") {
          Check(Read("NRPresentSdrRestorePolicy") == "0" && Read("NRPresentHdrPedestalPolicy") == "0",
                "future policy values preserved");
          Check(!WasWritten("NRPresentSdrRestorePolicy") && !WasWritten("NRPresentHdrPedestalPolicy"),
                "future policies receive no writes");
        } else {
          Check(Read("NRPresentSdrRestoreConfigVersion")
                        == (test_case == "pre8-malformed-sdr-version" ? "garbage" : "99")
                    && Read("NRPresentSdrRestorePolicy") == "0",
                "malformed SDR schema and value preserved");
          Check(!WasWritten("NRPresentSdrRestoreConfigVersion") && !WasWritten("NRPresentSdrRestorePolicy"),
                "malformed SDR schema receives no writes");
          Check(!runtime::present_sdr_restore_config_writable.load(), "malformed SDR schema is runtime read-only");
        }
      } else if (failed) {
        Check(Read("ConfigVersion") == "7" && !WasWritten("ConfigVersion"), "failed migration does not stamp success");
        Check(WasWritten(failed_key.c_str()), "migration failure injection reached actual writer");
        Check(Configuration().scope == scope::Scope::FINAL_PRESENT_ONLY && !Configuration().inherited,
              "failed migration retains previously loaded explicit scope0");
        Check(runtime::present_sdr_restore_policy.load() == 0, "failed migration retains previously loaded SDR policy");
        Check(runtime::debug_bypass_sdr_restore.load() && runtime::debug_bypass_hdr_pedestal.load(),
              "failed migration retains previously loaded bypass state");
      } else {
        CheckPair("1");
        Check(!Configuration().inherited && Configuration().scope == scope::Scope::FINAL_PRESENT_AND_PRE_FG,
              "old configuration migrates to explicit scope1");
        Check(Read("NRPresentSdrRestorePolicy") == "1" && Read("NRPresentHdrPedestalPolicy") == "1",
              "pre8 Legacy and old NRSdrBlackRestore migrate to Off policies");
        Check(Read("NRPresentSdrRestoreConfigVersion") == "1" && Read("NRPresentHdrPedestalConfigVersion") == "1",
              "migrated policies store current schemas");
        Check(Read("NRDebugBypassSdrRestore") == "0" && Read("NRDebugBypassHdrPedestal") == "0",
              "migration clears restore bypass flags");
        Check(Read("ConfigVersion") == "8", "successful migration stamps v8");
        for (const auto& event : writes) {
          if (event.key == "ConfigVersion") Check(event.pair_present_at_global, "recommended settings verified before stamp");
        }
      }
      Check(Read("UnregisteredPersistentKnob") == "42", "migration preserves unknown setting");
      if (migration) {
        Check(Read("NRIntensity") == "0.75" && Read("NRDisplayPedestal") == "2", "migration preserves unrelated tuning");
      }
    } else if (test_case == "reset") {
      Check(Configuration().scope == scope::Scope::FINAL_PRESENT_ONLY
                && runtime::present_sdr_restore_policy.load() == 0
                && runtime::debug_bypass_sdr_restore.load() && runtime::debug_bypass_hdr_pedestal.load(),
            "reset starts from explicit diagnostic choices");
      Check(runtime::features.empty(), "reset host has no GPU features to release");
      const auto generation = runtime::configuration_generation.load();
      writes.clear();
      runtime::ResetToDefaults(nullptr);
      Check(runtime::configuration_generation.load() == generation + 1,
            "real ResetToDefaults executed real RecreateFeatures");
      CheckPair("1");
      Check(!Configuration().inherited && Configuration().scope == scope::Scope::FINAL_PRESENT_AND_PRE_FG,
            "reset publishes explicit scope1");
      Check(Read("NRPresentSdrRestorePolicy") == "1" && Read("NRPresentHdrPedestalPolicy") == "1",
            "reset persists SDR Off and HDR Off");
      Check(Read("NRDebugBypassSdrRestore") == "0" && Read("NRDebugBypassHdrPedestal") == "0",
            "reset persists false restore bypass flags");
      Check(Read("ConfigVersion") == "8" && WasWritten("ConfigVersion"), "reset persists schema8");
      for (const auto& event : writes) {
        if (event.key == "ConfigVersion") Check(event.pair_present_at_global, "reset verifies restore defaults before stamp");
      }
      Check(Read("UnregisteredPersistentKnob") == "42", "reset preserves unregistered key");
    } else if (test_case.starts_with("nr-model-")) {
      CheckPair("1");
      Check(!Configuration().inherited && Configuration().scope == scope::Scope::FINAL_PRESENT_AND_PRE_FG,
            "model selection leaves explicit restore scope unchanged");
    } else if (test_case == "v8-legacy-key") {
      CheckPair("0");
      Check(!Configuration().inherited && Configuration().scope == scope::Scope::FINAL_PRESENT_ONLY,
            "v8 explicit diagnostic scope0 retained with missing SDR policy");
      Check(runtime::present_sdr_restore_policy.load() == 1,
            "v8 missing SDR formal policy loads Off despite legacy NRSdrBlackRestore");
      Check(Bytes() == before && std::all_of(writes.begin(), writes.end(), [](const WriteEvent& event) {
              return event.key == "ConfigVersion" && event.value == "8";
            }), "v8 missing SDR policy preserves explicit configuration; same-version stamp is allowed");
    } else if (test_case == "new" || test_case == "empty-section" || test_case == "old-registry") {
      Check(!Configuration().inherited && Configuration().scope == scope::Scope::FINAL_PRESENT_AND_PRE_FG,
            "new add-on defaults to explicit scope 1");
      CheckPair("1");
      Check(Read("NRPresentSdrRestorePolicy") == "1" && Read("NRPresentHdrPedestalPolicy") == "1",
            "new policies default to Off");
      Check(Read("NRDebugBypassSdrRestore") == "0" && Read("NRDebugBypassHdrPedestal") == "0",
            "new restore bypass defaults false");
      Check(Read("ConfigVersion") == std::to_string(runtime::kConfigVersion), "new initialization global stamp");
      bool saw_global = false;
      for (const auto& event : writes) {
        if (event.key == "ConfigVersion") {
          saw_global = true;
          Check(event.pair_present_at_global, "scope pair durably stored BEFORE global stamp");
        }
      }
      Check(saw_global, "first initialization actually wrote global stamp");
      if (test_case == "old-registry") Check(Read("NRIntensity") == "0.75", "v8 missing scope initialization preserves tuning");
    } else if (test_case == "fail-scope" || test_case == "fail-version" || test_case == "fail-sdr"
               || test_case == "fail-hdr" || test_case == "fail-bypass"
               || test_case == "fail-sdr-version" || test_case == "fail-hdr-version") {
      Check(Read("ConfigVersion") == "<absent>" && !WasWritten("ConfigVersion"), "failed pair has no global stamp");
      const auto loaded = scope::Load({.section_known = true});
      Check(Configuration().scope == loaded.scope && Configuration().inherited == loaded.inherited
                && Configuration().source == loaded.source && !Configuration().initialize_new,
            "failed initialization retains loaded scope classification and clears pending initialization");
      Check(WasWritten(failed_key.c_str()), "failure injection exercised actual pair writer");
    } else if (test_case == "scope0" || test_case == "scope1" || test_case == "new-reload") {
      const bool expanded = test_case == "scope1" || test_case == "new-reload";
      Check(!Configuration().inherited, "persisted explicit scope survives production load/restart");
      Check(Configuration().scope == (expanded ? scope::Scope::FINAL_PRESENT_AND_PRE_FG
                                               : scope::Scope::FINAL_PRESENT_ONLY), "correct loaded scope");
      CheckPair(expanded ? "1" : "0");
      Check(!WasWritten(SCOPE_KEY) && !WasWritten(VERSION_KEY), "valid pair is not reinitialized");
      Check(Bytes() == before, "valid explicit configuration remains byte-preserved on load");
      if (test_case != "new-reload") {
        Check(Read("NRPresentSdrRestorePolicy") == "0" && Read("NRPresentHdrPedestalPolicy") == "2",
              "v8 explicit diagnostic policies retained");
        Check(Read("NRDebugBypassSdrRestore") == "1" && Read("NRDebugBypassHdrPedestal") == "1",
              "v8 explicit diagnostic bypass choices retained");
      }
    } else if (test_case == "select" || test_case == "future-policy" || test_case == "save-fail-scope"
               || test_case == "save-fail-version") {
      const std::vector<const char*> policies = {"NRPresentSdrRestorePolicy", "NRPresentSdrRestoreConfigVersion",
          "NRSdrBlackRestore", "NRPresentHdrPedestalPolicy", "NRPresentHdrPedestalConfigVersion", "NRDisplayPedestal"};
      std::vector<std::string> previous;
      for (const auto* key : policies) previous.push_back(Read(key));
      Check(Bytes() == before, "production load preserves policy fixtures before explicit selection");
      for (const auto* key : policies) Check(!WasWritten(key), "load does not rewrite policy keys");
      writes.clear();
      if (test_case == "save-fail-scope") failed_key = SCOPE_KEY;
      if (test_case == "save-fail-version") failed_key = VERSION_KEY;
      const bool saved = runtime::CONFIG_TEST_SCOPE_SAVE(scope::Scope::FINAL_PRESENT_AND_PRE_FG);
      // ReShade's void setter cannot report a dropped idempotent version write.
      // A previously stored version1 still verifies the complete selected pair.
      const bool expected_saved = test_case != "save-fail-scope";
      Check(saved == expected_saved, "actual explicit save reports verified pair success/failure");
      if (saved) {
        CheckPair("1");
        Check(!Configuration().inherited && Configuration().scope == scope::Scope::FINAL_PRESENT_AND_PRE_FG,
              "successful explicit save publishes selected scope");
      }
      if (!saved) Check(Configuration().scope == scope::Scope::FINAL_PRESENT_ONLY, "failed save does not publish selection");
      for (size_t index = 0; index < policies.size(); ++index) {
        Check(Read(policies[index]) == previous[index] && !WasWritten(policies[index]), "scope selection preserves policy key");
      }
      Check(!WasWritten("ConfigVersion"), "explicit scope save does not stamp global schema");
    } else {
      CheckInherited();
      Check(Bytes() == before, "invalid/future INI remains byte-preserved");
      if (test_case != "malformed-scope" && test_case != "incomplete-pair") {
        Check(!Configuration().writable, "damaged version/future configuration read-only");
        writes.clear();
        Check(!runtime::CONFIG_TEST_SCOPE_SAVE(scope::Scope::FINAL_PRESENT_ONLY), "read-only production save refused");
        Check(writes.empty() && Bytes() == before, "refused save makes no writes");
      }
    }
    if ((migration && test_case.find("-fail-") == std::string::npos
         && test_case != "pre8-future-scope" && test_case != "pre8-future-policy"
         && test_case != "pre8-malformed-sdr-version" && test_case != "pre8-future-sdr-version")
        || test_case == "old-arbitrary" || test_case == "new" || test_case == "empty-section"
        || test_case == "new-reload" || test_case == "old-registry" || test_case == "reset") {
      Check(runtime::present_sdr_restore_policy.load() == 1, "loaded SDR policy is Off after defaults/migration");
      {
        std::shared_lock lock(runtime::hdr_pedestal_policy_state.mutex);
        Check(runtime::hdr_pedestal_policy_state.configuration.policy
                  == renodx::addons::dlss5::present_hdr_pedestal::Policy::OFF,
              "loaded HDR policy is Off after defaults/migration");
      }
      Check(!runtime::debug_bypass_sdr_restore.load() && !runtime::debug_bypass_hdr_pedestal.load(),
            "loaded restore bypass flags are false after defaults/migration");
    }
    if (test_case == "scope0" || test_case == "scope1") {
      Check(runtime::present_sdr_restore_policy.load() == 0, "v8 explicit SDR Legacy remains loaded");
      {
        std::shared_lock lock(runtime::hdr_pedestal_policy_state.mutex);
        Check(runtime::hdr_pedestal_policy_state.configuration.policy
                  == renodx::addons::dlss5::present_hdr_pedestal::Policy::LEGACY_SMOOTH,
              "v8 explicit HDR Smooth remains loaded");
      }
      Check(runtime::debug_bypass_sdr_restore.load() && runtime::debug_bypass_hdr_pedestal.load(),
            "v8 explicit bypass choices remain loaded");
    }
    char unrelated[64]{};
    GetPrivateProfileStringA("OTHER.ADDON", "Persistent", "", unrelated, sizeof(unrelated), ini_path.c_str());
    Check(std::string(unrelated) == "17", "unrelated add-on section preserved");
    Check(non_global_calls == 0, "all save calls remain global");
    std::ofstream receipt(test_case + ".write-events.tsv");
    for (const auto& event : writes) {
      receipt << event.key << '\t' << event.value << '\t' << event.stored << '\t' << event.pair_present_at_global << '\n';
    }
    std::ofstream before_receipt(test_case + ".before.ini", std::ios::binary);
    before_receipt << before;
    std::ofstream after_receipt(test_case + ".after.ini", std::ios::binary);
    after_receipt << Bytes();
    std::cout << "case=" << test_case << " reads=" << reads << " writes=" << writes.size() << '\n'
              << checks << " checks, " << failures << " failures\n";
    return (failures == 0 ? 0 : 1);
  } catch (const std::exception& exception) {
    std::cerr << "ERROR: " << exception.what() << '\n';
    return 2;
  }
}
