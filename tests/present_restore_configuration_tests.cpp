/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#define DEBUG_LEVEL_0

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
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
      if (test_case == "native-prefg-resolution-config") {
        addon += "NRPresentRestoreScope=1\r\nNRPresentRestoreScopeConfigVersion=1\r\n"
                 "NRExperimentalNativePreFgResolution=1\r\n";
      } else if (test_case == "presets") {
        addon += "NRPresentRestoreScope=1\r\nNRPresentRestoreScopeConfigVersion=1\r\n"
                 "NeuralUplift=0\r\nNRIntensity=0.75\r\nNRPreset=1\r\nNRPasses=2\r\n"
                 "NRLookMode=1\r\nNRLookStrength=1.4\r\nNRDetailStability=1\r\n"
                 "NRPass4LookFollowPass1=0\r\nNRPass4LookMode=1\r\nNRPass4LookStrength=0.42\r\n"
                 "UnregisteredPersistentKnob=42\r\nEnableHooks=0\r\nDX11Source=auto\r\n";
      } else if (test_case == "scope0" || test_case == "select" || test_case == "save-fail-scope"
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
      } else if (test_case.starts_with("pass-result-")) {
        addon += "NRPresentRestoreScope=1\r\nNRPresentRestoreScopeConfigVersion=1\r\nNRLookMode=1\r\nNRLookStrength=1.4\r\nNRLookHalo=0.3\r\nNRLookStabilize=1\r\nNRDetailStability=1\r\n";
        if (test_case == "pass-result-custom") {
          addon += "NRPass2LookFollowPass1=0\r\nNRPass2LookMode=1\r\nNRPass2LookStrength=0.4\r\nNRPass2LookHalo=0.8\r\nNRPass2LookStabilize=2\r\nNRPass2LookStabilizeMs=120\r\nNRPass2DetailStability=1\r\nNRPass3LookFollowPass1=0\r\nNRPass3LookMode=0\r\nNRPass3DetailStability=1\r\n";
        } else if (test_case == "pass-result-invalid") {
          addon += "NRPass2LookFollowPass1=0\r\nNRPass2LookMode=1\r\nNRPass2LookStrength=nan\r\nNRPass2LookHalo=99\r\nNRPass2LookDetailRadius=-1\r\nNRPass2LookStabilize=9\r\nNRPass2LookStabilizeMs=9999\r\nNRPass2DetailStability=9\r\n";
        }
      } else if (test_case.starts_with("nr-resolution-") || test_case == "profiles") {
        addon += "NRPresentRestoreScope=1\r\nNRPresentRestoreScopeConfigVersion=1\r\n"
                 "NRFollowInputRes=0\r\nNRResolutionScale=0.58\r\n";
        if (test_case == "nr-resolution-explicit") {
          addon += "NRPass2ResolutionMode=2\r\nNRPass2ResolutionScale=0.67\r\n"
                   "NRPass3ResolutionMode=1\r\nNRPass3ResolutionScale=0.5\r\n"
                   "NRPass4ResolutionMode=0\r\nNRPass4ResolutionScale=0.33\r\n";
        } else if (test_case == "nr-resolution-invalid") {
          addon += "NRPass2ResolutionMode=99\r\nNRPass2ResolutionScale=nan\r\n"
                   "NRPass3ResolutionMode=99\r\nNRPass3ResolutionScale=2\r\n"
                   "NRPass4ResolutionMode=99\r\nNRPass4ResolutionScale=0.1\r\n";
        }
      } else if (test_case.starts_with("nr-stage-")) {
        addon += "NRPresentRestoreScope=1\r\nNRPresentRestoreScopeConfigVersion=1\r\nNRHookPoint=1\r\nNRPreset=1\r\nNRPass2Preset=2\r\nNRPass2FollowPass1=1\r\nNRPass2LookFollowPass1=1\r\n";
        if (test_case == "nr-stage-explicit") {
          addon += "NRPass2Stage=2\r\nNRPass3Stage=0\r\nNRPass4Stage=3\r\n";
        } else if (test_case == "nr-stage-invalid") {
          addon += "NRPass2Stage=99\r\nNRPass3Stage=99\r\nNRPass4Stage=99\r\n";
        }
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
    if (test_case == "native-prefg-resolution-config") {
      Check(runtime::experimental_native_prefg_resolution.load(), "explicit INI enables the experimental PreFG pair");
      Check(Read("NRExperimentalNativePreFgResolution") == "1", "registered experimental key survives configuration load");
      Check(WritePrivateProfileStringA("RenoDX.DLSS5", "NRExperimentalNativePreFgResolution", "0", ini_path.c_str()),
            "write explicit prototype Off");
      runtime::LoadConfiguration();
      Check(!runtime::experimental_native_prefg_resolution.load(), "explicit Off disables the prototype");
      runtime::experimental_native_prefg_resolution.store(true);
      Check(WritePrivateProfileStringA("RenoDX.DLSS5", "NRExperimentalNativePreFgResolution", nullptr, ini_path.c_str()),
            "remove experimental override");
      runtime::LoadConfiguration();
      Check(!runtime::experimental_native_prefg_resolution.load(), "missing key resets to default Off after reload");
    }
    if (test_case.starts_with("nr-resolution-")) {
      const bool explicit_values = test_case == "nr-resolution-explicit";
      for (uint32_t pass = 1; pass < runtime::kMaxNrPasses; ++pass) {
        Check(runtime::pass_resolution_mode[pass - 1].load() == (explicit_values ? (pass == 1 ? 2u : pass == 2 ? 1u : 0u) : 0u),
              "INI resolution mode defaults to inheritance and loads each pass independently");
        const float expected = (explicit_values ? (pass == 1 ? .67f : pass == 2 ? .5f : .33f)
                                               : (test_case == "nr-resolution-invalid" && pass == 3 ? .33f : 1.f));
        Check(std::abs(runtime::pass_resolution_scale[pass - 1].load() - expected) < .0001f,
              "INI resolution scale is independent, bounded and defaults to one");
        uint32_t width = 0, height = 0;
        runtime::ComputeNrWorkingResolution(1920, 1080, 1280, 720, width, height, pass);
        Check(width == (explicit_values ? (pass == 1 ? 1286u : pass == 2 ? 1280u : 1114u) : 1114u)
                  && height == (explicit_values ? (pass == 1 ? 724u : pass == 2 ? 720u : 626u) : 626u),
              "effective settings preserve the original global reference and rounding rules");
      }
      Check(!runtime::nr_follow_input_res.load() && std::abs(runtime::nr_resolution_scale.load() - .58f) < .0001f,
            "pass overrides do not change global resolution");
    }
    if (test_case == "profiles") {
#include "profile_runtime_cases.hpp"
      RunProfileRuntimeCases(base_directory / "profile-runtime-cases");
    }
    if (test_case == "presets") {
#include "profile_runtime_cases.hpp"
      RunProfileRuntimeCases(base_directory / "profile-runtime-cases");
      namespace presets = renodx::addons::dlss5::presets;
      namespace adapter = runtime::preset_ui;
      namespace ui = renodx::addons::dlss5::ui;
      namespace model = renodx::addons::dlss5::nr_model;
      using Json = presets::Json;
      using Category = presets::FieldRule::Category;
      const auto fixture_root = base_directory;
      const auto fixture_ini = ini_path;
      const auto capture = []() { return adapter::Capture(presets::Scope::FULL, {{"name", "Synthetic test"}}); };
      const auto apply = [](const Json& settings, presets::Scope selected_scope = presets::Scope::FULL,
                            bool compatibility = true) {
        std::string error;
        const bool result = adapter::Apply({.metadata = {{"name", "Synthetic test"}}, .scope = selected_scope,
                                           .settings = settings}, compatibility, true, &error);
        if (!result) {
          std::cerr << "Preset apply: " << error << '\n';
        }
        return result;
      };
      const auto effect_name = [](runtime::SettingEffect effect) {
        switch (effect) {
          case runtime::SettingEffect::kNone: return "none";
          case runtime::SettingEffect::kHostHistory: return "host-history";
          case runtime::SettingEffect::kHistory: return "history";
          case runtime::SettingEffect::kRecreate: return "recreate";
        }
        return "unknown";
      };
      const auto category_name = [](Category category) {
        switch (category) {
          case Category::QUALITY: return "quality";
          case Category::MULTIPASS: return "multipass";
          case Category::COMPATIBILITY: return "compatibility";
          case Category::GENERAL: return "general";
          case Category::EXCLUDED: return "excluded";
        }
        return "unknown";
      };
      std::map<std::string, unsigned> rule_counts;
      for (const auto& rule : presets::FieldRules()) {
        ++rule_counts[rule.key];
      }
      Json inventory = Json::array();
      for (const auto& rule : presets::FieldRules()) {
        const std::string key(rule.key);
        Check(rule_counts[key] == 1, ("Exactly one rule: " + key).c_str());
        Json fallback = nullptr;
        const char* type = "nullable";
        const char* effect = "none";
        const char* source = "native default/nullable";
        if (const auto* setting = adapter::FindSetting(key)) {
          if (setting->b != nullptr) {
            fallback = setting->fallback != 0.f;
            type = "boolean";
          } else if (setting->u != nullptr) {
            fallback = static_cast<uint32_t>(setting->fallback);
            type = "integer";
          } else {
            fallback = setting->fallback;
            type = "number";
          }
          effect = effect_name(setting->effect);
          source = "production Setting::fallback/effect (registry or extras)";
        } else if (key == "UiTheme") {
          fallback = ui::GetTheme(ui::ThemeId::CYBER_NEON).slug;
          type = "string";
        } else if (key == "UiLanguage") {
          fallback = "auto";
          type = "string";
        } else if (key == "UiMode") {
          fallback = "modern";
          type = "string";
        } else if (key == "DedupeMode") {
          fallback = runtime::defaults::kDedupeMode;
          type = "integer";
        } else if (key == "ListStateMode") {
          fallback = runtime::defaults::kListStateMode;
          type = "integer";
        } else if (key == "NRPresentHdrPedestalPolicy") {
          fallback = static_cast<uint32_t>(renodx::addons::dlss5::present_hdr_pedestal::DEFAULT_POLICY);
          type = "integer";
          effect = "host-history";
        } else if (key == "NRPresentRestoreScope") {
          fallback = static_cast<uint32_t>(scope::Configuration{}.scope);
          type = "integer";
          effect = "host-history";
        } else if (key == "EnableHooks") {
          type = "nullable integer";
          effect = "restart";
        } else if (key == "DX11Source" || key == "ForeignNr") {
          type = "nullable string";
          effect = "restart";
        }
        if (!fallback.is_null()) {
          Check(adapter::ValidateValue(rule, fallback), ("Production default fits rule: " + key).c_str());
        }
        const Json current = adapter::ReadSetting(key);
        if (rule.category != Category::EXCLUDED && !current.is_null()) {
          Check(adapter::ValidateValue(rule, current), ("Loaded value fits rule: " + key).c_str());
        }
        inventory.push_back({{"key", key}, {"type", type}, {"default", fallback}, {"default_source", source},
                             {"range", {{"minimum", (std::isfinite(rule.minimum) ? Json(rule.minimum) : Json(nullptr))},
                                        {"maximum", (std::isfinite(rule.maximum) ? Json(rule.maximum) : Json(nullptr))}}},
                             {"category", category_name(rule.category)}, {"pass", rule.pass}, {"effect", effect},
                             {"environment", (rule.category == Category::COMPATIBILITY ? "game/device/runtime; explicit opt-in"
                                               : (rule.category == Category::EXCLUDED ? "session/private/schema; excluded"
                                                                                     : "game INI; portable preset data"))}});
      }
      for (const auto& section : runtime::kSettingSections) {
        for (const auto* setting : section) {
          Check(rule_counts[setting->key] == 1, (std::string("Registry coverage: ") + setting->key).c_str());
        }
      }
      for (const auto& setting : adapter::kExtraSettings) {
        Check(rule_counts[setting.key] == 1, (std::string("Extra setting coverage: ") + setting.key).c_str());
      }
      {
        std::ofstream report("preset-field-inventory.json", std::ios::binary);
        report << Json{{"source", "production FieldRules + Setting registry/extras"}, {"fields", inventory}}.dump(2) << '\n';
        Check(static_cast<bool>(report), "Write full private field inventory.");
      }
      for (const auto selected_scope : {presets::Scope::QUALITY, presets::Scope::QUALITY_MULTIPASS, presets::Scope::FULL}) {
        const auto document = adapter::Capture(selected_scope, {{"name", "Scope test"}});
        for (const auto& rule : presets::FieldRules()) {
          const bool expected = rule.category != Category::EXCLUDED
                                && (selected_scope == presets::Scope::FULL || rule.category == Category::QUALITY
                                    || (selected_scope == presets::Scope::QUALITY_MULTIPASS && rule.category == Category::MULTIPASS
                                        && (rule.pass == 0 || rule.pass <= runtime::stack_passes.load())))
                                && (std::string_view(rule.key) != "NRPresentRestoreScope" || !Configuration().inherited)
                                && !adapter::ReadSetting(rule.key).is_null();
          Check(document.settings.contains(rule.key) == expected, (std::string("Scope capture: ") + rule.key).c_str());
        }
      }
      const auto stage_original = capture();
      const Json stage_settings{{"NRPasses", 4u}, {"NRPass2Stage", 2u}, {"NRPass3Stage", 0u}, {"NRPass4Stage", 3u}};
      const presets::Document stage_request{.scope = presets::Scope::QUALITY_MULTIPASS, .settings = stage_settings};
      const auto stage_rows = adapter::Preview(stage_request, false);
      for (const auto& [key, value] : stage_settings.items()) {
        Check(std::any_of(stage_rows.begin(), stage_rows.end(), [&](const auto& row) {
          return row.key == key && row.after == value
              && (std::string_view(row.status) == "Will change" || std::string_view(row.status) == "Unchanged");
        }), ("Stage appears in multipass difference preview: " + key).c_str());
      }
      Check(apply(stage_settings, presets::Scope::QUALITY_MULTIPASS, false), "Apply independent stage settings.");
      const auto stage_captured = capture();
      presets::Document stage_roundtrip;
      Check(presets::Parse(presets::Serialize(stage_captured), &stage_roundtrip)
                && stage_roundtrip.settings == stage_captured.settings, "Stages survive preset export/import.");
      runtime::LoadConfiguration();
      for (const auto& [key, value] : stage_settings.items()) {
        Check(adapter::ReadSetting(key) == value, ("Stage survives configuration reload: " + key).c_str());
      }
      for (const char* key : {"NRPreset", "NRPass2Preset", "NRPass2FollowPass1", "NRPass2LookFollowPass1"}) {
        Check(adapter::ReadSetting(key) == stage_original.settings[key], "Stage edits preserve model/Look/result inheritance.");
      }
      Check(apply({{"NRIntensity", 0.875f}}), "Apply old partial preset without stage fields.");
      for (const auto& [key, value] : stage_settings.items()) {
        Check(adapter::ReadSetting(key) == value, "Absent preset stage fields preserve local choices.");
      }
      std::string stage_error;
      Check(adapter::Undo(&stage_error), "Undo partial preset without changing stages.");
      Check(adapter::Apply(stage_original, true, true, &stage_error), "Restore original configuration before stage undo.");
      Check(apply(stage_settings, presets::Scope::QUALITY_MULTIPASS, false) && adapter::Undo(&stage_error)
                && capture().settings == stage_original.settings, "Stage batch undo restores exact original snapshot.");
      const auto quality_original = capture();
      const Json quality_baseline{{"NRHookPoint", 0u}, {"NRFollowInputRes", true},
                                  {"NRResolutionScale", 1.f}, {"NRLookUpsample", 0u}};
      const Json quality_target{{"NRHookPoint", 2u}, {"NRFollowInputRes", false},
                                {"NRResolutionScale", 0.5f}, {"NRLookUpsample", 3u}};
      for (const auto& [key, value] : quality_target.items()) {
        const auto* rule = presets::FindRule(key);
        Check(rule != nullptr && rule->category == Category::QUALITY,
              ("Explicit Quality classification: " + key).c_str());
      }
      Check(presets::FindRule("NRStyle")->category == Category::QUALITY, "Style remains Quality.");
      for (const auto key : {"NRPresentEncoding", "NRCodecMode", "NRPresentHdrPedestalPolicy", "NRDepthMode",
                             "NRMVecScaleX", "NRMVecScaleY", "NRPresentGuides", "NRPresentFrames",
                             "NRPresentFgSource", "NRPresentUICorrection", "NROutputRect"}) {
        Check(presets::FindRule(key)->category == Category::COMPATIBILITY,
              (std::string("Dedicated compatibility classification remains: ") + key).c_str());
      }
      for (const auto selected_scope : {presets::Scope::QUALITY, presets::Scope::QUALITY_MULTIPASS}) {
        Check(apply(quality_baseline), "Prepare four-field Quality baseline.");
        const auto before_quality = capture();
        const auto before_preview = Bytes();
        const auto before_preview_writes = writes.size();
        const presets::Document requested{.scope = selected_scope, .settings = quality_target};
        const auto quality_rows = adapter::Preview(requested, false);
        for (const auto& [key, value] : quality_target.items()) {
          Check(std::any_of(quality_rows.begin(), quality_rows.end(), [&](const auto& row) {
            return row.key == key && row.apply && row.after == value;
          }), ("Quality preview accepts without compatibility opt-in: " + key).c_str());
        }
        Check(Bytes() == before_preview && writes.size() == before_preview_writes,
              "Four-field preview does not write configuration.");
        Check(apply(quality_target, selected_scope, false), "Quality batch applies all four fields without compatibility opt-in.");
        const auto captured_quality = adapter::Capture(selected_scope, {{"name", "Quality fields"}});
        std::string quality_error;
        const auto quality_path = fixture_root / "quality-fields.nrnexus.json";
        presets::Document reloaded_quality;
        Check(presets::WriteFile(quality_path, captured_quality, true, &quality_error)
                  && presets::ReadFile(quality_path, &reloaded_quality, &quality_error)
                  && reloaded_quality.settings == captured_quality.settings,
              "Quality capture survives production preset file save/reload.");
        runtime::LoadConfiguration();
        for (const auto& [key, value] : quality_target.items()) {
          Check(captured_quality.settings.contains(key) && captured_quality.settings[key] == value
                    && adapter::ReadSetting(key) == value,
                ("Quality capture and INI reload preserve field: " + key).c_str());
        }
        Check(adapter::Undo(&quality_error), "Undo four-field Quality batch after configuration reload.");
        for (const auto& [key, value] : quality_baseline.items()) {
          Check(adapter::ReadSetting(key) == value, ("Quality undo restores field: " + key).c_str());
        }
        Check(adapter::Apply(reloaded_quality, false, true, &quality_error), "Apply reloaded Quality preset without compatibility opt-in.");
        for (const auto& [key, value] : before_quality.settings.items()) {
          if (const auto* rule = presets::FindRule(key);
              rule->category == Category::COMPATIBILITY || rule->category == Category::GENERAL) {
            Check(!captured_quality.settings.contains(key) && adapter::ReadSetting(key) == value,
                  ("Quality file and apply isolate compatibility/general field: " + key).c_str());
          }
        }
        const auto isolated_before = capture();
        const auto isolated_writes = writes.size();
        const Json dedicated{{"NRPresentEncoding", 3u}, {"NRDepthMode", 2u}, {"NRMVecScaleX", -1.f},
                             {"NRPresentFgSource", 2u}, {"NRPresentUICorrection", false},
                             {"UiMode", "classic"}, {"NRToggleKey", 0x70u},
                             {"NRPasses", 4u}, {"NRPass4Preset", 2u}};
        const presets::Document isolated_request{.scope = presets::Scope::QUALITY, .settings = dedicated};
        const auto isolated_rows = adapter::Preview(isolated_request, true);
        for (const auto& [key, value] : dedicated.items()) {
          Check(std::any_of(isolated_rows.begin(), isolated_rows.end(), [&](const auto& row) {
            return row.key == key && !row.apply && std::string_view(row.status) == "Skipped (outside scope)";
          }), ("Quality preview rejects dedicated field even with opt-in: " + key).c_str());
        }
        Check(adapter::Apply(isolated_request, true, true, &quality_error)
                  && capture().settings == isolated_before.settings && writes.size() == isolated_writes,
              "Quality cannot import dedicated compatibility, UI, keys or multipass fields.");

        // Schema v1 predates the expanded Quality field set; absent keys stay local.
        Check(apply({{"NRPreset", 3u}, {"NRPasses", 2u}, {"NRPass2Preset", 2u}}), "Prepare legacy model requests.");
        presets::Document old_v1;
        Check(presets::Parse(presets::Serialize({.scope = selected_scope,
                    .settings = {{"NRStyle", 1u}, {"NRPreset", 3u}, {"NRPasses", 2u}, {"NRPass2Preset", 2u}}}), &old_v1),
              "Read schema v1 Quality preset without the four new fields.");
        const auto old_v1_rows = adapter::Preview(old_v1, false);
        for (const auto& [key, value] : quality_target.items()) {
          Check(!old_v1.settings.contains(key)
                    && std::any_of(old_v1_rows.begin(), old_v1_rows.end(), [&](const auto& row) {
                      return row.key == key && !row.apply && row.before == value;
                    }), ("Missing v1 field previews current value without applying: " + key).c_str());
        }
        const auto legacy_writes = writes.size();
        Check(adapter::Apply(old_v1, false, true, &quality_error), "Apply old v1 with missing Quality fields.");
        runtime::LoadConfiguration();
        for (const auto& [key, value] : quality_target.items()) {
          Check(adapter::ReadSetting(key) == value, ("Missing v1 Quality field stays unchanged: " + key).c_str());
        }
        Check(model::ResolvePreset(3, 0, true) == 1 && model::ResolvePreset(3, 2, false) == 1
                  && adapter::ReadSetting("NRPreset") == 3 && adapter::ReadSetting("NRPass2Preset") == 2
                  && Read("NRPreset") == "3" && Read("NRPass2Preset") == "2",
              "Old v1 preserves historical model and unsupported request instead of writing runtime fallback.");
        Check(std::none_of(writes.begin() + legacy_writes, writes.end(), [&](const auto& event) {
          return quality_target.contains(event.key) || event.key == "NRPreset" || event.key == "NRPass2Preset";
        }), "Missing v1 fields and unchanged raw model requests are never rewritten.");
      }
      Check(apply(quality_original.settings), "Restore configuration after four-field Quality regression.");
      const auto saved_passes = runtime::stack_passes.load();
      Check(apply({{"NRPasses", 1u}}), "Prepare multipass expansion preview.");
      const auto floating_passes = adapter::Preview({.scope = presets::Scope::QUALITY_MULTIPASS,
          .settings = {{"NRPasses", 4.0}, {"NRPass4Intensity", 0.93f}}}, true);
      Check(std::any_of(floating_passes.begin(), floating_passes.end(), [](const auto& row) {
        return row.key == "NRPass4Intensity" && row.apply;
      }), "Integral floating pass count uses imported count when previewing active pass fields.");
      Check(apply({{"NRPasses", saved_passes}}), "Restore original pass count.");
      Json excluded = Json::object();
      for (const auto& rule : presets::FieldRules()) {
        if (rule.category == Category::EXCLUDED) {
          excluded[rule.key] = "must remain inert";
        }
      }
      const auto excluded_ini = Bytes();
      const auto excluded_writes = writes.size();
      const auto excluded_runtime = capture();
      Check(apply(excluded) && Bytes() == excluded_ini && writes.size() == excluded_writes
                && capture().settings == excluded_runtime.settings, "Every excluded field is forbidden on import, including full scope.");
      const auto explicit_scope = Read(SCOPE_KEY);
      const auto explicit_scope_version = Read(VERSION_KEY);
      // A missing version keeps a real inherited, writable configuration; a
      // completely absent pair is initialized by the existing v8 load migration.
      WritePrivateProfileStringA(runtime::kConfigSection, VERSION_KEY, nullptr, ini_path.c_str());
      runtime::LoadConfiguration();
      const auto inherited_ini = Bytes();
      const auto inherited_writes = writes.size();
      Check(Configuration().inherited && !capture().settings.contains(SCOPE_KEY), "Full capture omits inherited restore scope.");
      Check(Bytes() == inherited_ini && writes.size() == inherited_writes, "Inherited scope capture does not stamp explicit keys.");
      std::string scope_undo_error;
      const auto inherited_configuration = Configuration();
      for (const uint32_t explicit_value : {0u, 1u}) {
        Check(apply({{SCOPE_KEY, explicit_value}}) && !Configuration().inherited,
              "Explicit scope import replaces inherited state even when its numeric value is unchanged.");
        if (explicit_value == 0) {
          const auto undo_failure_ini = Bytes();
          failed_key = VERSION_KEY;
          Check(!adapter::Undo(&scope_undo_error), "Scope Undo verifies failed deletion of its version key.");
          failed_key.clear();
          Check(Bytes() == undo_failure_ini && !Configuration().inherited
                    && Configuration().scope == scope::Scope::FINAL_PRESENT_ONLY && !adapter::undo.empty(),
                "Failed scope Undo restores disk and retains the runtime state and retry snapshot.");
        }
        Check(adapter::Undo(&scope_undo_error) && Configuration().inherited && Read(SCOPE_KEY) == explicit_scope
                  && Read(VERSION_KEY) == "<absent>" && Configuration().source == inherited_configuration.source
                  && Configuration().reason == inherited_configuration.reason,
              "Undo restores inherited scope, source and original incomplete key pair.");
        const auto inherited_decision = scope::Decide(Configuration(), scope::Domain::PRE_FG_HUDLESS);
        Check(inherited_decision.sdr_policy && !inherited_decision.hdr_policy,
              "Restored inherited scope retains Pre-FG SDR-only semantics.");
      }
      Check(apply({{SCOPE_KEY, 1u}}) && runtime::SavePresentRestoreScope(scope::Scope::FINAL_PRESENT_ONLY),
            "Manually change scope after an inherited-scope import.");
      Check(adapter::Undo(&scope_undo_error) && !Configuration().inherited && Read(SCOPE_KEY) == "0" && Read(VERSION_KEY) == "1",
            "Scope Undo preserves a later explicit manual choice.");
      WritePrivateProfileStringA(runtime::kConfigSection, SCOPE_KEY, explicit_scope.c_str(), ini_path.c_str());
      WritePrivateProfileStringA(runtime::kConfigSection, VERSION_KEY, explicit_scope_version.c_str(), ini_path.c_str());
      runtime::LoadConfiguration();
      const auto kept = capture();
      const auto skipped_sdr = adapter::ReadSetting("NRPresentSdrRestorePolicy");
      const auto skipped_debug = adapter::ReadSetting("NRDebugBypassSdrRestore");
      const auto unknown_before = Read("UnregisteredPersistentKnob");
      Check(apply({{"NRIntensity", 0.8}, {"FutureUnknownSetting", 17}, {"NRDebugBypassSdrRestore", false},
                   {"NRPresentSdrRestorePolicy", 4u}}, presets::Scope::QUALITY), "Apply quality with unknown/excluded/out-of-scope data.");
      Check(adapter::ReadSetting("NRPresentSdrRestorePolicy") == skipped_sdr
                && adapter::ReadSetting("NRDebugBypassSdrRestore") == skipped_debug
                && Read("FutureUnknownSetting") == "<absent>" && Read("UnregisteredPersistentKnob") == unknown_before,
            "Unknown, excluded and out-of-scope keys never write.");
      for (const auto& [key, value] : kept.settings.items()) {
        if (key != "NRIntensity") {
          Check(adapter::ReadSetting(key) == value, ("Missing key remains unchanged: " + key).c_str());
        }
      }
      auto rows = adapter::Preview({.scope = presets::Scope::FULL, .settings = {{"FutureUnknownSetting", true}}}, false);
      Check(std::any_of(rows.begin(), rows.end(), [](const auto& row) {
        return row.key == "FutureUnknownSetting" && !row.apply && std::string_view(row.status) == "Unsupported";
      }), "Preview explains unsupported setting.");
      const auto compatibility_before = adapter::ReadSetting("NRPresentWhiteNits");
      Check(apply({{"NRPresentWhiteNits", 321.f}}, presets::Scope::FULL, false)
                && adapter::ReadSetting("NRPresentWhiteNits") == compatibility_before, "Compatibility requires opt-in.");
      const auto saved = capture();
      const auto defaults_generation = runtime::configuration_generation.load();
      const auto defaults_epoch = runtime::history_reset_epoch.load();
      const auto defaults_bytes = Bytes();
      const auto defaults_writes = writes.size();
      const auto exported = capture();
      Check(exported.settings == saved.settings, "Export reads the actual current user snapshot.");
      Check(defaults_generation == runtime::configuration_generation.load() && defaults_epoch == runtime::history_reset_epoch.load()
                && defaults_bytes == Bytes() && defaults_writes == writes.size(), "Snapshot export has no write/recreate/history side effects.");
      std::string error;
      for (const uint32_t main_model : {1u, 2u}) {
        for (const uint32_t pass_model : {1u, 2u}) {
          for (uint32_t passes = 1; passes <= 4; ++passes) {
            Check(apply({{"NRPreset", main_model}, {"NRPasses", passes}, {"NRLookMode", true}, {"NRLookStrength", 1.25f},
                         {"NRDetailStability", 1u}, {"NRPass2Preset", pass_model}, {"NRPass3Preset", pass_model},
                         {"NRPass4Preset", pass_model}, {"NRPass2FollowPass1", true},
                         {"NRPass2LookFollowPass1", true}, {"NRPass3LookFollowPass1", true}, {"NRPass4LookFollowPass1", true}}),
                  "A/A A/B B/A B/B batch applies for passes 1-4.");
            const auto document = adapter::Capture(presets::Scope::QUALITY_MULTIPASS, {});
            const auto result = runtime::LookSettingsSnapshots(false);
            for (uint32_t pass = 1; pass < 4; ++pass) {
              const std::string prefix = "NRPass" + std::to_string(pass + 1);
              Check(document.settings.contains(prefix + "Preset") == (pass < passes), "Multipass captures only active passes.");
              Check(model::ResolvePreset(runtime::preset.load(), runtime::pass_preset[pass - 1].load(), true) == pass_model,
                    "Appearance inheritance leaves explicit model selection intact.");
              Check(result[pass].strength == result[0].strength && result[pass].strength == 1.25f,
                    "Result inheritance resolves the main snapshot.");
            }
          }
        }
      }
      for (unsigned pass = 2; pass <= 4; ++pass) {
        const auto prefix = "NRPass" + std::to_string(pass);
        const float custom = 0.1f * pass;
        Check(apply({{prefix + "LookFollowPass1", false}, {prefix + "LookMode", true},
                     {prefix + "LookStrength", custom}, {prefix + "DetailStability", 1u}}), "Apply independent pass result.");
        Check(runtime::LookSettingsSnapshots(false)[pass - 1].strength == custom, "Custom pass result resolves independently.");
        Check(apply({{prefix + "LookFollowPass1", true}})
                  && runtime::LookSettingsSnapshots(false)[pass - 1].strength == 1.25f
                  && adapter::ReadSetting(prefix + "LookStrength") == custom, "Result inheritance retains saved custom value.");
      }
      Check(apply({{"NRPasses", 1u}, {"NRPass4LookFollowPass1", false}, {"NRPass4LookStrength", 0.42f}}), "Prepare inactive custom pass 4.");
      const auto full = capture();
      Check(full.settings["NRPass4LookStrength"] == 0.42f && full.settings["NRPass4LookFollowPass1"] == false,
            "Full export retains inactive pass 4 customization.");
      presets::Document decoded;
      Check(presets::Parse(presets::Serialize(full), &decoded) && decoded.settings == full.settings, "Full preset codec round trip.");
      Check(apply({{"NRPreset", 3u}, {"NRPass2Preset", 2u}}), "Apply raw legacy model 3 and unsupported B.");
      const auto legacy = capture();
      Check(presets::Parse(presets::Serialize(legacy), &decoded) && decoded.settings["NRPreset"] == 3
                && decoded.settings["NRPass2Preset"] == 2, "Legacy 3 and B round-trip without runtime fallback rewriting.");
      Check(model::ResolvePreset(3, 0, true) == 1 && model::ResolvePreset(1, 2, false) == 1
                && Read("NRPreset") == "3" && Read("NRPass2Preset") == "2", "Unsupported runtime fallback preserves original disk choices.");
      auto generation = runtime::configuration_generation.load();
      auto history = runtime::history_reset_epoch.load();
      Check(apply({{"NRPreset", 1u}, {"NRPasses", 3u}, {"NRPass2Preset", 1u}, {"NRStyle", 1u}, {"NRIntensity", 0.9f}}),
            "Batch combines multiple recreate and history effects.");
      Check(runtime::configuration_generation.load() == generation + 1 && runtime::history_reset_epoch.load() == history + 1,
            "One batch increments recreate generation and merged history reset exactly once.");
      generation = runtime::configuration_generation.load();
      history = runtime::history_reset_epoch.load();
      Check(apply({{"NRIntensity", 0.7f}, {"NRStyle", 2u}, {"NRLocalTone", 0.8f}}), "History-only batch applies.");
      Check(runtime::configuration_generation.load() == generation && runtime::history_reset_epoch.load() == history + 1,
            "History-only changes merge without recreating features.");
      history = runtime::history_reset_epoch.load();
      Check(apply({{"UiTheme", ui::GetTheme(ui::ThemeId::FLUENT_LIGHT).slug}, {"UiMode", "classic"}, {"UiLanguage", "auto"}}),
            "Ordinary settings apply.");
      Check(runtime::configuration_generation.load() == generation && runtime::history_reset_epoch.load() == history,
            "Ordinary settings cause zero recreate/history requests.");
      const auto invalid_before = Bytes();
      const auto invalid_writes = writes.size();
      Check(!adapter::Apply({.scope = presets::Scope::FULL, .settings = {{"NRIntensity", 99}, {"NRPreset", 2u}}},
                            true, true, &error), "Invalid batch is rejected as a whole.");
      adapter::rows = adapter::Preview({.scope = presets::Scope::FULL, .settings = {{"NRIntensity", 0.2f}}}, true);
      adapter::have_preview = true;
      adapter::have_preview = false;
      adapter::rows.clear();
      Check(Bytes() == invalid_before && writes.size() == invalid_writes
                && runtime::configuration_generation.load() == generation, "Invalid batch and cancelled preview do not write or recreate.");
      const auto rollback_runtime = capture();
      const auto rollback_ini = Bytes();
      generation = runtime::configuration_generation.load();
      history = runtime::history_reset_epoch.load();
      failed_key = "NRIntensity";
      Check(!adapter::Apply({.scope = presets::Scope::FULL, .settings = {{"NRIntensity", 0.25f}, {"NRPreset", 2u}}},
                            true, true, &error), "Injected ReShade write failure rejects batch.");
      Check(WasWritten("NRIntensity") && error.find("restored") != std::string::npos, "Failure injection reached writer and verified rollback.");
      failed_key.clear();
      Check(Bytes() == rollback_ini && capture().settings == rollback_runtime.settings
                && runtime::configuration_generation.load() == generation && runtime::history_reset_epoch.load() == history,
            "Failed write restores disk and leaves runtime/effects unpublished.");
      for (const auto version_key : {"ConfigVersion", "NRPresentSdrRestoreConfigVersion",
                                     "NRPresentHdrPedestalConfigVersion", "NRPresentRestoreScopeConfigVersion"}) {
        const auto restore_bytes = Bytes();
        WritePrivateProfileStringA(runtime::kConfigSection, version_key, "99", ini_path.c_str());
        runtime::LoadConfiguration();
        const auto read_only_ini = Bytes();
        const auto read_only_runtime = capture();
        const auto read_only_writes = writes.size();
        Json settings{{"NRIntensity", 0.3f}};
        if (std::string_view(version_key) == "NRPresentSdrRestoreConfigVersion") {
          settings["NRPresentSdrRestorePolicy"] = (adapter::ReadSetting("NRPresentSdrRestorePolicy") == 0 ? 1u : 0u);
        }
        if (std::string_view(version_key) == "NRPresentHdrPedestalConfigVersion") {
          settings["NRPresentHdrPedestalPolicy"] = (adapter::ReadSetting("NRPresentHdrPedestalPolicy") == 0 ? 1u : 0u);
        }
        if (std::string_view(version_key) == "NRPresentRestoreScopeConfigVersion") {
          settings["NRPresentRestoreScope"] = (adapter::ReadSetting("NRPresentRestoreScope") == 0 ? 1u : 0u);
        }
        Check(!adapter::Apply({.scope = presets::Scope::FULL, .settings = settings}, true, true, &error),
              "Future global/SDR/HDR/scope schema prevents affected batch.");
        Check(Bytes() == read_only_ini && writes.size() == read_only_writes && capture().settings == read_only_runtime.settings,
              "Future schemas remain read-only on disk and in memory.");
        {
          std::ofstream restore(ini_path, std::ios::binary | std::ios::trunc);
          restore << restore_bytes;
        }
        runtime::LoadConfiguration();
      }
      Check(apply({{"NRIntensity", 0.6f}, {"NRStyle", 0u}}), "Prepare undo baseline.");
      Check(apply({{"NRIntensity", 1.1f}, {"NRStyle", 1u}}), "Remember imported changes for Undo.");
      runtime::kSetIntensity.Commit(0.95f);
      Check(adapter::Undo(&error), "Undo applies through production batch writer.");
      Check(runtime::intensity.load() == 0.95f && runtime::style.load() == 0u,
            "Undo restores import but preserves later manual edits.");
      std::map<std::string, std::optional<std::string>> startup_backup;
      for (const auto key : {"EnableHooks", "DX11Source", "ForeignNr"}) {
        startup_backup[key] = runtime::ReadPresentRestoreRaw(key);
        WritePrivateProfileStringA(runtime::kConfigSection, key, nullptr, ini_path.c_str());
      }
      runtime::LoadConfiguration();
      generation = runtime::configuration_generation.load();
      history = runtime::history_reset_epoch.load();
      const auto hooks_before = runtime::hooks_enabled.load();
      const auto streamline_before = runtime::streamline_hooks_enabled.load();
      const auto source_before = runtime::dx11_source.load();
      const auto foreign_before = runtime::foreign_nr_yield.load();
      const auto absent_capture = capture();
      Check(!absent_capture.settings.contains("EnableHooks") && !absent_capture.settings.contains("DX11Source")
                && !absent_capture.settings.contains("ForeignNr"), "Missing startup keys stay nullable in full capture.");
      Check(apply({{"EnableHooks", 0u}, {"DX11Source", "foreign"}, {"ForeignNr", "observe"}}),
            "Startup values import into a game without explicit keys.");
      Check(Read("EnableHooks") == "0" && Read("DX11Source") == "foreign" && Read("ForeignNr") == "observe",
            "Startup import persists explicit choices.");
      Check(runtime::hooks_enabled.load() == hooks_before && runtime::streamline_hooks_enabled.load() == streamline_before
                && runtime::dx11_source.load() == source_before && runtime::foreign_nr_yield.load() == foreign_before
                && runtime::configuration_generation.load() == generation && runtime::history_reset_epoch.load() == history,
            "Startup choices require restart and do not pretend to hot-switch runtime hooks/policy.");
      Check(adapter::Undo(&error) && Read("EnableHooks") == "<absent>" && Read("DX11Source") == "<absent>"
                && Read("ForeignNr") == "<absent>", "Undo restores missing startup keys without stamping auto/default.");
      runtime::LoadConfiguration();
      Check(!capture().settings.contains("EnableHooks") && !capture().settings.contains("DX11Source")
                && !capture().settings.contains("ForeignNr"), "Startup key absence survives reload and capture.");
      for (const auto& [key, value] : startup_backup) {
        WritePrivateProfileStringA(runtime::kConfigSection, key.c_str(), (value ? value->c_str() : nullptr), ini_path.c_str());
      }
      runtime::LoadConfiguration();
      const auto persisted = capture();
      const auto saved_local_size = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
      std::wstring saved_local(saved_local_size, L'\0');
      if (saved_local_size != 0) {
        const auto saved_length = GetEnvironmentVariableW(L"LOCALAPPDATA", saved_local.data(), saved_local_size);
        saved_local.resize(saved_length);
      }
      runtime::LoadConfiguration();
      Check(capture().settings == persisted.settings, "Imported and manually edited values survive reload.");
      const auto game_one_bytes = Bytes();
      const auto local = fixture_root / "local-appdata";
      Check(SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str()) != 0, "Controlled shared preset environment.");
      std::filesystem::path library_one;
      Check(presets::LibraryPath(&library_one, &error), "Resolve game one shared library.");
      Check(presets::WriteFile(library_one / L"shared.nrnexus.json", persisted, false, &error), "Save shared preset from game one.");
      base_directory = fixture_root / "game-two";
      std::filesystem::create_directory(base_directory);
      ini_path = (base_directory / "ReShade.ini").string();
      {
        std::ofstream fixture(ini_path, std::ios::binary);
        fixture << "[GENERAL]\r\nPresetPath=game-two.ini\r\n[OTHER.ADDON]\r\nPersistent=29\r\n"
                   "[RenoDX.DLSS5]\r\n" << current_global << policy_keys
                << "NRPresentRestoreScope=1\r\nNRPresentRestoreScopeConfigVersion=1\r\nNRIntensity=0.1\r\n";
      }
      runtime::LoadConfiguration();
      std::filesystem::path library_two;
      Check(presets::LibraryPath(&library_two, &error) && library_two == library_one, "Separate game INIs share one preset library.");
      Check(presets::ReadFile(library_two / L"shared.nrnexus.json", &decoded, &error)
                && adapter::Apply(decoded, true, true, &error), "Game two reads and applies game one's preset.");
      Check(runtime::intensity.load() == 0.95f && Read("NRIntensity") != "0.1", "Game two writes only its selected INI.");
      const auto game_two_bytes = Bytes();
      Check(game_two_bytes.find("Persistent=29") != std::string::npos && game_two_bytes.find("PresetPath=game-two.ini") != std::string::npos,
            "Game two unrelated sections remain intact.");
      base_directory = fixture_root;
      ini_path = fixture_ini;
      Check(Bytes() == game_one_bytes, "Game two import never writes game one's INI.");
      runtime::LoadConfiguration();
      Check(capture().settings == persisted.settings && Read("UnregisteredPersistentKnob") == "42",
            "Game one reload preserves its own values and unrelated add-on keys.");
      Check(Bytes().find("Persistent=17") != std::string::npos && Bytes().find("PresetPath=other.ini") != std::string::npos,
            "Game one other sections remain intact.");
      Check(non_global_calls == 0, "All preset writes use global ReShade boundary for the current game.");
      SetEnvironmentVariableW(L"LOCALAPPDATA", (saved_local_size == 0 ? nullptr : saved_local.c_str()));
      std::ofstream receipt("presets.write-events.tsv");
      for (const auto& event : writes) {
        receipt << event.key << '\t' << event.value << '\t' << event.stored << '\n';
      }
      std::cout << "case=presets reads=" << reads << " writes=" << writes.size() << '\n'
                << checks << " checks, " << failures << " failures\n";
      return (failures == 0 ? 0 : 1);
    }
    if (test_case.starts_with("nr-stage-")) {
      const bool explicit_stage = test_case == "nr-stage-explicit";
      Check(runtime::pass_stage[0] == (explicit_stage ? 2u : 0u)
                && runtime::pass_stage[1] == 0u && runtime::pass_stage[2] == (explicit_stage ? 3u : 0u),
            "Missing and invalid stages inherit; explicit stages load unchanged.");
      Check(runtime::hook_point == runtime::kHookRender && runtime::preset == 1
                && runtime::pass_preset[0] == 2 && runtime::pass_follow[0] && runtime::pass_look_result[0].follow,
            "Stage load preserves hook/model/Look/result choices.");
      for (uint32_t pass = 0; pass < 3; ++pass) {
        const auto generation = runtime::configuration_generation.load();
        const auto setting = runtime::kSetPassStage[pass];
        setting.Commit(1.f);
        Check(runtime::configuration_generation.load() == generation + 1, "Stage commit requests feature recreation.");
        Check(Read(setting.key) == "1", "Stage commit persists raw selection.");
        setting.Commit(0.f);
        Check(Read(setting.key) == "0", "Stage reset persists inheritance.");
      }
      runtime::PersistConfig();
    }
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
      if (test_case == "nr-model-default") {
        const auto old_declaration = runtime::native_fg_declaration.load();
        const bool old_off = runtime::native_fg_public_off.load();
        const auto old_options_generation = runtime::native_fg_options_generation.load();
        const auto old_options_ns = runtime::native_fg_options_changed_ns.load();
        const auto old_history = runtime::history_reset_epoch.load();
        runtime::native_fg_public_off = false;
        runtime::OnNativeFgOptions(true);
        Check(runtime::native_fg_declaration.load() == runtime::NativeFgDeclaration::kOn
                  && runtime::native_fg_options_generation.load() == old_options_generation
                  && runtime::history_reset_epoch.load() == old_history,
              "first successful On declaration is observable without changing scheduling or history");
        runtime::OnNativeFgOptions(false);
        Check(runtime::native_fg_declaration.load() == runtime::NativeFgDeclaration::kOff
                  && runtime::history_reset_epoch.load() == old_history,
              "public Off remains distinct from unobserved FG and adds no reset without an owner");
        runtime::native_fg_declaration = old_declaration;
        runtime::native_fg_public_off = old_off;
        runtime::native_fg_options_generation = old_options_generation;
        runtime::native_fg_options_changed_ns = old_options_ns;
        const float old_intensity = runtime::intensity.load();
        const float old_structure = runtime::local_structure_strength.load();
        const bool old_mask = runtime::use_auto_mask.load();
        const bool old_independent = runtime::skin_independent.load();
        const bool old_look = runtime::look_mode.load();
        const float old_strength = runtime::look_strength.load();
        auto slot = std::make_unique<runtime::NrFeatureSlot>();
        slot->parameters = &parameters;
        for (uint32_t preset : {1u, 2u}) {
          runtime::SetNrCreateParameters(&parameters, 640, 360, 640, 360, 0, false, preset);
          for (float value : {0.f, 0.5f, 1.f, 1.5f, 2.f}) {
            runtime::intensity = value;
            runtime::SetModelParameters(*slot, 0, runtime::history_reset_epoch.load(), 0, 0, false, 0);
            float sent = -1.f;
            Check(NVSDK_NGX_SUCCEED(parameters.Get("DLSSNR.Intensity", &sent)) && sent == value,
                  "A/B production evaluate block sends the full 0-2 model intensity without a clamp at 1");
          }
        }
        for (bool mask : {false, true}) {
          runtime::use_auto_mask = mask;
          for (bool independent : {false, true}) {
            runtime::skin_independent = independent;
            for (float value : {0.f, 1.f, 1.5f, 2.f}) {
              runtime::local_structure_strength = value;
              runtime::SetModelParameters(*slot, 0, runtime::history_reset_epoch.load(), 0, 0, false, 0);
              float sent = -1.f;
              Check(NVSDK_NGX_SUCCEED(parameters.Get("DLSSNR.LocalStructureStrength", &sent))
                        && sent == (mask && independent ? (std::min)(value, 1.f) : value),
                    "production structure clamp requires both the character mask and independent skin");
            }
          }
        }
        runtime::look_mode = true;
        const renodx::addons::dlss5::look::Frame frame{.nr_width = 640, .nr_height = 360, .full_width = 640, .full_height = 360};
        for (float value : {0.f, 0.5f, 1.f, 1.5f, 2.f}) {
          runtime::look_strength = value;
          const auto settings = runtime::LookSettingsSnapshot(false);
          Check(renodx::addons::dlss5::look::MakeConstants(settings, frame).strength == value,
                "enabled Look Stage preserves the five result-strength values in GPU constants");
        }
        runtime::look_mode = false;
        Check(runtime::LookSettingsSnapshot(false).strength == 1.f && runtime::look_strength.load() == 2.f,
              "disabled result shaping uses neutral strength and retains the saved value");
        runtime::intensity = old_intensity;
        runtime::local_structure_strength = old_structure;
        runtime::use_auto_mask = old_mask;
        runtime::skin_independent = old_independent;
        runtime::look_mode = old_look;
        runtime::look_strength = old_strength;
      }
    }
    if (test_case.starts_with("pass-result-")) {
      auto result = runtime::LookSettingsSnapshots(false);
      Check(result[0].strength == 1.4f && result[0].halo == 0.3f && result[0].stabilize == 1u,
            "main result controls retain the existing keys");
      if (test_case == "pass-result-default") {
        for (uint32_t pass = 1; pass < runtime::kMaxNrPasses; ++pass) {
          Check(runtime::pass_look_result[pass - 1].follow.load()
                    && result[pass].strength == result[0].strength && result[pass].halo == result[0].halo
                    && result[pass].stabilize == result[0].stabilize,
                "old configuration applies the shared result to every pass unchanged");
        }
      } else if (test_case == "pass-result-custom") {
        Check(result[1].strength == 0.4f && result[1].halo == 0.8f && result[1].stabilize == 2u
                  && result[1].stabilize_ms == 120.f, "pass 2 resolves independent gains, limits and temporal settings");
        Check(result[2].strength == 1.f && result[2].halo == 0.f && result[2].stabilize == 0u,
              "pass 3 can disable result shaping independently");
        Check(result[3].strength == result[0].strength, "pass 4 continues inheriting the main settings");
        const renodx::addons::dlss5::look::Frame frame{.nr_width = 640, .nr_height = 360, .full_width = 640, .full_height = 360};
        const auto first = renodx::addons::dlss5::look::MakeConstants(result[0], frame);
        const auto second = renodx::addons::dlss5::look::MakeConstants(result[1], frame);
        Check(first.strength == 1.4f && second.strength == 0.4f && first.halo != second.halo,
              "GPU constant generation uses each pass result snapshot");
        runtime::kSetPassLookFollow[0].Commit(1.f);
        result = runtime::LookSettingsSnapshots(false);
        Check(result[1].strength == 1.4f && runtime::pass_look_result[0].strength.load() == 0.4f,
              "enabling inheritance preserves independent stored values");
        runtime::kSetPassLookFollow[0].Commit(0.f);
        runtime::PersistConfig();
        Check(Read("NRPass2LookStrength") == "0.400000" || Read("NRPass2LookStrength") == "0.4",
              "independent result value persists");
        runtime::LoadConfiguration();
        Check(runtime::LookSettingsSnapshots(false)[1].strength == 0.4f,
              "independent result values survive a configuration reload");
          runtime::ResetSettings({runtime::kPassLookResultSettings});
        Check(runtime::pass_look_result[0].follow.load() && runtime::pass_look_result[0].strength.load() == 1.f,
              "result reset restores inheritance and stored defaults");
      } else {
        Check(result[1].strength == 1.f && result[1].halo == 1.f && result[1].detail_radius == 0.25f
                  && result[1].stabilize == 0u, "invalid result values use the established defaults");
        Check(runtime::pass_look_result[0].stabilize_ms.load() == 250.f
                  && runtime::pass_look_result[0].detail_stability.load() == runtime::defaults::kDetailStability,
              "invalid temporal settings are sanitized");
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
    } else if (test_case.starts_with("nr-model-") || test_case.starts_with("nr-stage-") || test_case.starts_with("pass-result-")
               || test_case.starts_with("nr-resolution-") || test_case == "profiles"
               || test_case == "native-prefg-resolution-config") {
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
