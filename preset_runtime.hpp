/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Composed inside dlssnr.hpp's internal namespace, after the configuration
// registry and overlay helpers. File/JSON code lives in preset_store.hpp.

namespace preset_ui {
using Json = presets::Json;

inline const Setting kExtraSettings[] = {
    {"NRPresentWhiteSource", &present_white_source, defaults::kPresentWhiteSource, SettingEffect::kHistory},
    {"NRPresentPreFg", &present_pre_fg, defaults::kPresentPreFg, SettingEffect::kRecreate},
    {"NRPresentPreFgContract", &present_pre_fg_contract, defaults::kPresentPreFgContract, SettingEffect::kRecreate},
    {"NRPresentBlockHudless", &present_block_hudless, false, SettingEffect::kHistory},
    {"NRPresentMVecUnits", &present_mvec_units, defaults::kPresentMvecUnits, SettingEffect::kHistory},
    {"NRAutoFgFallback", &auto_fg_fallback, defaults::kAutoFgFallback, SettingEffect::kRecreate},
    {"NRVkStateRestore", &vk::state_restore, defaults::kVkStateRestore, SettingEffect::kNone},
    {"NROutputRect", &nr_output_rect, defaults::kOutputRect, SettingEffect::kRecreate},
    {"NRSdrBlackRestore", &sdr_black_restore, defaults::kSdrBlackRestore, SettingEffect::kHostHistory},
    {"NRMaxWorksets", &max_worksets, kDefaultMaxWorksets, SettingEffect::kRecreate},
    {"NRWorksetFreePolicy", &workset_free_policy, kDefaultWorksetFreePolicy, SettingEffect::kNone},
    {"NRDisableRelease", &disable_release_mode, kDefaultDisableRelease, SettingEffect::kNone},
    {"NRShutdownQuiesce", &shutdown_quiesce_enabled, true, SettingEffect::kNone},
    {"UiIssueNotice", &hud_enabled, true, SettingEffect::kNone},
    {"UiWelcomed", &ui_welcomed, false, SettingEffect::kNone},
    {"NRCrashDump", &lastgasp::dumps_on, defaults::kCrashDump, SettingEffect::kNone},
    {"UiHudX", &hud_position_x, 50.f, SettingEffect::kNone},
    {"UiHudY", &hud_position_y, 0.f, SettingEffect::kNone},
};

inline const Setting* FindSetting(std::string_view key) {
  for (const auto& group : kSettingSections) {
    for (const auto* setting : group) {
      if (key == setting->key) {
        return setting;
      }
    }
  }
  for (const auto& setting : kExtraSettings) {
    if (key == setting.key) {
      return &setting;
    }
  }
  return nullptr;
}

inline Json ReadSetting(std::string_view key) {
  if (const auto* setting = FindSetting(key)) {
    float value = setting->Get();
    if (setting->b != nullptr) {
      return value != 0.f;
    }
    if (setting->u != nullptr) {
      return static_cast<uint32_t>(value);
    }
    return value;
  }
  if (key == "UiTheme") {
    return ui::GetTheme(ui_theme_choice.load()).slug;
  }
  if (key == "UiLanguage") {
    const int choice = ui_language_choice.load();
    return (choice == 0 ? "auto" : ui::kLanguages[choice - 1].code);
  }
  if (key == "UiMode") {
    return (ui_classic_layout.load() ? "classic" : "modern");
  }
  if (key == "DedupeMode") {
    return dedupe_mode.load();
  }
  if (key == "ListStateMode") {
    return list_state_mode.load();
  }
  if (key == "NRPresentHdrPedestalPolicy") {
    std::shared_lock lock(hdr_pedestal_policy_state.mutex);
    return static_cast<uint32_t>(hdr_pedestal_policy_state.configuration.policy);
  }
  if (key == "NRPresentRestoreScope") {
    std::shared_lock lock(present_restore_configuration.mutex);
    return static_cast<uint32_t>(present_restore_configuration.scope.scope);
  }
  // Attach-time policies are backed up only when explicitly stored. Their
  // absence carries auto-detection semantics and must not become a choice.
  if (key == "EnableHooks" || key == "DX11Source" || key == "ForeignNr") {
    const auto raw = ReadPresentRestoreRaw(std::string(key).c_str());
    if (!raw) {
      return nullptr;
    }
    if (key != "EnableHooks") {
      std::string normalized = *raw;
      for (char& character : normalized) {
        if (character >= 'A' && character <= 'Z') {
          character += 'a' - 'A';
        }
      }
      return normalized;
    }
    uint32_t value = 0;
    const auto parsed = std::from_chars(raw->data(), raw->data() + raw->size(), value);
    if (parsed.ec == std::errc{} && parsed.ptr == raw->data() + raw->size()) {
      return value;
    }
  }
  return nullptr;
}

inline bool InScope(const presets::FieldRule& rule, presets::Scope scope, unsigned passes) {
  using Category = presets::FieldRule::Category;
  if (rule.category == Category::EXCLUDED) {
    return false;
  }
  if (scope == presets::Scope::FULL) {
    return true;
  }
  if (rule.category == Category::QUALITY) {
    return true;
  }
  return scope == presets::Scope::QUALITY_MULTIPASS && rule.category == Category::MULTIPASS
         && (rule.pass == 0 || rule.pass <= passes);
}

inline bool ValidateValue(const presets::FieldRule& rule, const Json& value) {
  const std::string_view key(rule.key);
  if (key == "UiTheme") {
    if (!value.is_string()) {
      return false;
    }
    for (const auto& theme : ui::kThemes) {
      if (value.get_ref<const std::string&>() == theme.slug) {
        return true;
      }
    }
    return false;
  }
  if (key == "UiLanguage") {
    if (!value.is_string()) {
      return false;
    }
    if (value == "auto") {
      return true;
    }
    for (const auto& language : ui::kLanguages) {
      if (value == language.code) {
        return true;
      }
    }
    return false;
  }
  if (key == "UiMode") {
    return value == "modern" || value == "classic";
  }
  if (key == "DX11Source") {
    return value == "auto" || value == "native" || value == "foreign" || value == "off";
  }
  if (key == "ForeignNr") {
    return value == "yield" || value == "observe";
  }
  if (const auto* setting = FindSetting(key); setting != nullptr && setting->b != nullptr) {
    return value.is_boolean();
  }
  if (!value.is_number() || value.is_boolean()) {
    return false;
  }
  const double number = value.get<double>();
  if (!std::isfinite(number) || number < rule.minimum || number > rule.maximum) {
    return false;
  }
  const auto* setting = FindSetting(key);
  return (setting != nullptr && setting->f != nullptr) || std::floor(number) == number;
}

inline presets::Document Capture(presets::Scope scope, const Json& metadata) {
  RuntimeLock lock(runtime_mutex);
  presets::Document document{.metadata = metadata, .scope = scope, .settings = Json::object()};
  const unsigned passes = ReadSetting("NRPasses").get<unsigned>();
  for (const auto& rule : presets::FieldRules()) {
    if (!InScope(rule, scope, passes)) {
      continue;
    }
    if (std::string_view(rule.key) == "NRPresentRestoreScope") {
      std::shared_lock config_lock(present_restore_configuration.mutex);
      if (present_restore_configuration.scope.inherited) {
        continue;
      }
    }
    const Json value = ReadSetting(rule.key);
    if (!value.is_null() && ValidateValue(rule, value)) {
      document.settings[rule.key] = value;
    }
  }
  return document;
}

struct ScopeSnapshot {
  present_restore_scope::Configuration configuration;
  std::optional<std::string> raw_scope;
  std::optional<std::string> raw_version;
};

struct Difference {
  std::string key;
  Json before;
  Json after;
  const presets::FieldRule* rule = nullptr;
  const char* status = "Unchanged";
  bool apply = false;
  std::optional<ScopeSnapshot> scope_before;
};

inline std::vector<Difference> Preview(const presets::Document& document, bool compatibility) {
  RuntimeLock lock(runtime_mutex);
  std::vector<Difference> differences;
  unsigned passes = stack_passes.load();
  if (const auto selected = document.settings.find("NRPasses"); selected != document.settings.end()) {
    if (const auto* rule = presets::FindRule("NRPasses"); rule != nullptr && ValidateValue(*rule, *selected)) {
      passes = selected->get<unsigned>();
    }
  }
  for (const auto& [key, value] : document.settings.items()) {
    const auto* rule = presets::FindRule(key);
    Difference row{.key = key, .before = ReadSetting(key), .after = value, .rule = rule};
    bool inherited_scope = false;
    if (key == "NRPresentRestoreScope") {
      std::shared_lock config_lock(present_restore_configuration.mutex);
      inherited_scope = present_restore_configuration.scope.inherited;
    }
    const bool startup = key == "EnableHooks" || key == "DX11Source" || key == "ForeignNr";
    if (rule == nullptr || (row.before.is_null() && !startup)
        || (startup && !row.before.is_null() && !ValidateValue(*rule, row.before))) {
      row.status = "Unsupported";
    } else if (!InScope(*rule, document.scope, passes)) {
      row.status = "Skipped (outside scope)";
    } else if (!ValidateValue(*rule, value)) {
      row.status = "Invalid value";
    } else if (key == "NRHookPoint" && value == kHookPresent
               && (vulkan_present_seen.load() || (D3D11OnlySession() && !Dx11NativeRoute()) || present_unusable.load())) {
      row.status = "Unsupported";
    } else if (rule->category == presets::FieldRule::Category::COMPATIBILITY && !compatibility) {
      row.status = "Skipped (compatibility)";
    } else if (row.before != row.after || inherited_scope) {
      row.status = "Will change";
      row.apply = true;
    }
    if (rule != nullptr && ValidateValue(*rule, value)) {
      if (const auto* setting = FindSetting(key); setting != nullptr && setting->f != nullptr) {
        row.after = value.get<float>();
        if (row.apply && row.before == row.after) {
          row.apply = false;
          row.status = "Unchanged";
        }
      }
    }
    differences.push_back(std::move(row));
  }
  for (const auto& rule : presets::FieldRules()) {
    if (rule.category == presets::FieldRule::Category::EXCLUDED || document.settings.contains(rule.key)) {
      continue;
    }
    const Json current = ReadSetting(rule.key);
    if (current.is_null()) {
      continue;
    }
    differences.push_back({.key = rule.key, .before = current, .after = current, .rule = &rule, .status = "Kept (not in preset)"});
  }
  return differences;
}

inline std::vector<Difference> undo;
inline Json active_settings = Json::object();
inline std::string active_name;
inline std::string notice;

// ReShade's setters update its configuration cache. Verify them before
// publishing any runtime values; this is not a disk/GPU atomic transaction.
inline bool Apply(const presets::Document& document, bool compatibility, bool remember, std::string* error,
                  bool restore_missing = false, const ScopeSnapshot* restore_scope = nullptr) {
  SettleSlider(true);
  auto differences = Preview(document, compatibility);
  if (restore_missing) {
    for (auto& row : differences) {
      const bool scope_restore = row.key == "NRPresentRestoreScope" && restore_scope != nullptr;
      if (row.after.is_null() && (row.key == "EnableHooks" || row.key == "DX11Source" || row.key == "ForeignNr" || scope_restore)) {
        row.status = "Will change";
        row.apply = scope_restore || !row.before.is_null();
      }
    }
  }
  for (const auto& row : differences) {
    if (std::string_view(row.status) == "Invalid value") {
      *error = "Invalid value";
      return false;
    }
  }
  std::vector<Difference> changes;
  for (const auto& row : differences) {
    if (row.apply) {
      changes.push_back(row);
    }
  }
  RuntimeLock runtime_lock(runtime_mutex);
  std::unique_lock config_lock(present_restore_configuration.mutex);
  std::unique_lock policy_lock(hdr_pedestal_policy_state.mutex);
  if (!present_restore_configuration.global_writable) {
    *error = "Newer or invalid game configuration is read-only.";
    return false;
  }
  Json writes = Json::object();
  SettingEffect effect = SettingEffect::kNone;
  bool restart = false;
  for (auto& row : changes) {
    const auto* setting = FindSetting(row.key);
    writes[row.key] = row.after;
    if (setting != nullptr) {
      effect = (std::max)(effect, setting->effect);
    }
    if (row.key == "NRPresentSdrRestorePolicy") {
      if (!present_sdr_restore_config_writable.load()) {
        *error = "Newer or invalid game configuration is read-only.";
        return false;
      }
      writes["NRPresentSdrRestoreConfigVersion"] = present_sdr_restore::CONFIG_VERSION;
    }
    if (row.key == "NRPresentHdrPedestalPolicy") {
      if (!hdr_pedestal_policy_state.configuration.writable) {
        *error = "Newer or invalid game configuration is read-only.";
        return false;
      }
      writes["NRPresentHdrPedestalConfigVersion"] = present_hdr_pedestal::CONFIG_VERSION;
      effect = (std::max)(effect, SettingEffect::kHostHistory);
    }
    if (row.key == "NRPresentRestoreScope") {
      if (!present_restore_configuration.scope.writable) {
        *error = "Newer or invalid game configuration is read-only.";
        return false;
      }
      if (restore_scope != nullptr) {
        writes[row.key] = (restore_scope->raw_scope ? Json(*restore_scope->raw_scope) : Json(nullptr));
        writes["NRPresentRestoreScopeConfigVersion"] = (restore_scope->raw_version ? Json(*restore_scope->raw_version) : Json(nullptr));
      } else {
        writes["NRPresentRestoreScopeConfigVersion"] = present_restore_scope::CONFIG_VERSION;
        if (remember && present_restore_configuration.scope.inherited) {
          // Inherited and explicit scope 0 differ for Pre-FG; retain the source
          // and exact key presence instead of reducing the undo state to a number.
          row.scope_before = ScopeSnapshot{.configuration = present_restore_configuration.scope,
                                           .raw_scope = ReadPresentRestoreRaw("NRPresentRestoreScope"),
                                           .raw_version = ReadPresentRestoreRaw("NRPresentRestoreScopeConfigVersion")};
        }
      }
      effect = (std::max)(effect, SettingEffect::kHostHistory);
    }
    restart |= row.key == "EnableHooks" || row.key == "DX11Source" || row.key == "ForeignNr";
  }
  std::map<std::string, std::optional<std::string>> backup;
  for (const auto& [key, value] : writes.items()) {
    backup[key] = ReadPresentRestoreRaw(key.c_str());
  }
  const auto write = [](const std::string& key, const Json& value) {
    if (value.is_null()) {
      reshade::set_config_value(nullptr, kConfigSection, key.c_str(), static_cast<const char*>(nullptr));
    } else if (value.is_string()) {
      reshade::set_config_value(nullptr, kConfigSection, key.c_str(), value.get_ref<const std::string&>().c_str());
    } else if (value.is_boolean()) {
      reshade::set_config_value(nullptr, kConfigSection, key.c_str(), value.get<bool>());
    } else if (const auto* setting = FindSetting(key); setting != nullptr && setting->f != nullptr) {
      reshade::set_config_value(nullptr, kConfigSection, key.c_str(), value.get<float>());
    } else {
      reshade::set_config_value(nullptr, kConfigSection, key.c_str(), value.get<uint32_t>());
    }
  };
  const auto matches = [](const std::string& key, const Json& value) {
    if (value.is_null()) {
      return !ReadPresentRestoreRaw(key.c_str()).has_value();
    }
    if (value.is_string()) {
      return ReadPresentRestoreRaw(key.c_str()) == value.get<std::string>();
    }
    if (value.is_boolean()) {
      bool actual = !value.get<bool>();
      return reshade::get_config_value(nullptr, kConfigSection, key.c_str(), actual) && actual == value.get<bool>();
    }
    if (const auto* setting = FindSetting(key); setting != nullptr && setting->f != nullptr) {
      float actual = NAN;
      return reshade::get_config_value(nullptr, kConfigSection, key.c_str(), actual)
             && std::isfinite(actual) && std::abs(actual - value.get<float>()) <= 0.000001f;
    }
    uint32_t actual = UINT32_MAX;
    return reshade::get_config_value(nullptr, kConfigSection, key.c_str(), actual) && actual == value.get<uint32_t>();
  };
  for (const auto& [key, value] : writes.items()) {
    write(key, value);
  }
  bool verified = true;
  for (const auto& [key, value] : writes.items()) {
    verified &= matches(key, value);
  }
  if (!verified) {
    bool restored = true;
    for (const auto& [key, value] : backup) {
      reshade::set_config_value(nullptr, kConfigSection, key.c_str(), value ? value->c_str() : nullptr);
    }
    for (const auto& [key, value] : backup) {
      restored &= ReadPresentRestoreRaw(key.c_str()) == value;
    }
    *error = (restored ? "Configuration write failed; previous settings restored."
                       : "Configuration write failed; runtime unchanged. Some saved values could not be restored.");
    return false;
  }
  for (const auto& row : changes) {
    if (row.key == "NRFollowInputRes" || row.key == "NRResolutionScale") {
      ++nr_resolution_commit_epochs[0];
    }
    for (uint32_t pass = 1; pass < kMaxNrPasses; ++pass) {
      if (row.key == kSetPassResolutionScale[pass - 1].key) {
        ++nr_resolution_commit_epochs[pass];
      }
    }
    if (const auto* setting = FindSetting(row.key)) {
      setting->StoreLocked(row.after.is_boolean() ? (row.after.get<bool>() ? 1.f : 0.f) : row.after.get<float>());
    } else if (row.key == "UiTheme") {
      ui_theme_choice = ui::ParseTheme(row.after.get<std::string>());
    } else if (row.key == "UiLanguage") {
      ui_language_choice = 0;
      for (int i = 0; i < ui::kLanguageCount; ++i) {
        if (row.after == ui::kLanguages[i].code) {
          ui_language_choice = i + 1;
        }
      }
    } else if (row.key == "UiMode") {
      ui_classic_layout = row.after == "classic";
    } else if (row.key == "DedupeMode") {
      dedupe_mode = row.after.get<uint32_t>();
    } else if (row.key == "ListStateMode") {
      list_state_mode = row.after.get<int>();
    } else if (row.key == "NRPresentHdrPedestalPolicy") {
      hdr_pedestal_policy_state.configuration = present_hdr_pedestal::LoadPolicy(row.after.get<uint32_t>(), present_hdr_pedestal::CONFIG_VERSION);
    } else if (row.key == "NRPresentRestoreScope") {
      if (restore_scope != nullptr) {
        present_restore_configuration.scope = restore_scope->configuration;
      } else {
        present_restore_configuration.scope = present_restore_scope::Load({.section_known = true,
                                                                           .section_has_settings = true,
                                                                           .scope_present = true,
                                                                           .scope_value = row.after.get<uint32_t>(),
                                                                           .version_present = true});
      }
    }
  }
  if (!changes.empty()) {
    ++present_restore_configuration.epoch;
  }
  policy_lock.unlock();
  config_lock.unlock();
  if (effect == SettingEffect::kRecreate || (enabled.load() && std::any_of(changes.begin(), changes.end(), [](const auto& row) { return row.key == "NeuralUplift"; }))) {
    RecreateFeaturesLocked();
  } else if (effect == SettingEffect::kHistory) {
    RequestHistoryReset();
  } else if (effect == SettingEffect::kHostHistory) {
    for (auto& [handle, state] : features) {
      for (auto& slot : state.slots) {
        slot.look_history.valid = false;
      }
    }
  }
  if (remember) {
    undo = changes;
    active_settings = Json::object();
    for (const auto& row : differences) {
      if (row.rule != nullptr && (row.apply || std::string_view(row.status) == "Unchanged")) {
        active_settings[row.key] = row.after;
      }
    }
    active_name = document.metadata.value("name", "");
    if (active_name.empty()) {
      active_name = ui::Tr("Imported preset");
    }
  }
  notice = (restart ? "Preset applied. Restart the game for hook policy changes." : "Preset applied.");
  if (!changes.empty()) profile_commit_epoch.fetch_add(1, std::memory_order_relaxed);
  return true;
}

inline bool Undo(std::string* error) {
  presets::Document document{.metadata = Json::object(), .scope = presets::Scope::FULL, .settings = Json::object()};
  std::optional<ScopeSnapshot> restore_scope;
  for (const auto& row : undo) {
    if (ReadSetting(row.key) == row.after) {
      if (row.scope_before) {
        std::shared_lock config_lock(present_restore_configuration.mutex);
        if (present_restore_configuration.scope.inherited) {
          continue;
        }
        restore_scope = row.scope_before;
        document.settings[row.key] = nullptr;
      } else {
        document.settings[row.key] = row.before;
      }
    }
  }
  if (!Apply(document, true, false, error, true, (restore_scope ? &*restore_scope : nullptr))) {
    return false;
  }
  undo.clear();
  active_name.clear();
  active_settings.clear();
  notice = "Import undone. Later edits were kept.";
  return true;
}

}  // namespace preset_ui

#include "profile_runtime.hpp"

namespace preset_ui {
enum class Job { REFRESH,
                 READ,
                 SAVE,
                 EXPORT,
                 COPY,
                 RENAME,
                 REMOVE,
                 PARSE,
                 CHOOSE_IMPORT,
                 CHOOSE_EXPORT,
                 CHOOSE_FOLDER,
                 OPEN_FOLDER };
struct Worker {
  std::shared_mutex mutex;
  bool busy = false;
  bool ready = false;
  Job job = Job::REFRESH;
  std::filesystem::path path;
  std::filesystem::path destination;
  bool overwrite = false;
  std::string text;
  std::string error;
  presets::Document document;
  std::vector<std::filesystem::path> files;
  std::filesystem::path library;
  bool cancelled = false;
};
inline Worker worker;
inline std::vector<std::filesystem::path> files;
inline std::filesystem::path selected;
inline presets::Document preview;
inline std::vector<Difference> rows;
inline std::vector<Difference> export_rows;
inline int export_scope = -1;
inline bool have_preview = false;
inline bool compatibility = false;
inline bool confirm_full = false;
inline int mode = 0;
inline int scope_choice = 1;
inline int group_choice = 0;
inline bool overwrite = false;
inline bool confirm_delete = false;
inline bool show_kept = false;
inline bool save_options = false;
inline bool export_options = false;
inline bool import_path_options = false;
inline bool reset_scroll = false;
struct ManagerGeometry {
  bool valid = false;
  bool was_open = false;
  ImVec2 size_em = ImVec2(43.f, 30.f);
  ImVec2 relative_position = ImVec2(0.5f, 0.5f);
  ImVec2 display = ImVec2(0.f, 0.f);
  float font_size = 0.f;
  float compact_height_em = 0.f;
  bool task_resize = false;
  ImVec2 full_size_em = ImVec2(43.f, 30.f);
  ImVec2 last_size = ImVec2(0.f, 0.f);
};
inline ManagerGeometry manager_geometry;

inline void Navigate(int next) {
  mode = next;
  notice.clear();
  confirm_delete = false;
  overwrite = false;
  reset_scroll = true;
}
inline char name[129] = {};
inline char author[129] = {};
inline char description[2049] = {};
inline char source_game[129] = {};
inline char path[1024] = {};
inline char search[129] = {};
inline std::filesystem::path library_path;
inline std::optional<presets::Document> export_document;

inline presets::Document ExportSnapshot(presets::Scope scope, const Json& metadata) {
  if (!export_document) return Capture(scope, metadata);
  auto document = *export_document;
  document.metadata = metadata;
  document.scope = scope;
  const unsigned passes = document.settings.value("NRPasses", 1u);
  for (auto item = document.settings.begin(); item != document.settings.end();) {
    const auto* rule = presets::FindRule(item.key());
    if (rule == nullptr || !InScope(*rule, scope, passes)) item = document.settings.erase(item);
    else ++item;
  }
  return document;
}

inline DWORD WINAPI WorkerMain(LPVOID module) {
  {
    Job job;
    std::filesystem::path from, to;
    std::string input;
    presets::Document document;
    bool replace;
    {
      std::shared_lock lock(worker.mutex);
      job = worker.job;
      from = worker.path;
      to = worker.destination;
      input = worker.text;
      document = worker.document;
      replace = worker.overwrite;
    }
    std::string error;
    std::vector<std::filesystem::path> entries;
    std::filesystem::path library;
    bool cancelled = false;
    try {
      if (job == Job::REFRESH) {
        if (presets::LibraryPath(&library, &error)) presets::List(&entries, &error);
      } else if (job == Job::CHOOSE_EXPORT || job == Job::CHOOSE_FOLDER || job == Job::OPEN_FOLDER) {
        if (presets::LibraryPath(&library, &error)) {
          if (job == Job::OPEN_FOLDER) {
            presets::dialogs::OpenFolder(library, &error);
          } else {
            const auto result = presets::dialogs::Choose(job == Job::CHOOSE_EXPORT ? presets::dialogs::Kind::SAVE_FILE : presets::dialogs::Kind::FOLDER,
                                                         library, from.filename());
            to = result.path;
            cancelled = result.cancelled;
            error = result.error;
          }
        }
      } else if (job == Job::READ || job == Job::CHOOSE_IMPORT) {
        if (job == Job::CHOOSE_IMPORT && presets::LibraryPath(&library, &error)) {
          const auto result = presets::dialogs::Choose(presets::dialogs::Kind::OPEN_FILE, library);
          from = result.path;
          cancelled = result.cancelled;
          error = result.error;
        }
        if (from.is_relative()) {
          std::filesystem::path library;
          if (presets::LibraryPath(&library, &error)) {
            from = library / from;
          }
        }
        if (error.empty() && !cancelled) {
          presets::ReadFile(from, &document, &error);
        }
      } else if (job == Job::PARSE) {
        presets::Parse(input, &document, &error);
      } else if (job == Job::REMOVE) {
        presets::Delete(from, &error);
      } else {
        if (job == Job::COPY || job == Job::RENAME) {
          std::filesystem::path library;
          if (presets::LibraryPath(&library, &error)) {
            presets::ReadFile(library / from, &document, &error);
          }
          document.metadata["name"] = input;
        }
        if (error.empty()) {
          if (to.empty() || job == Job::COPY || job == Job::RENAME) {
            std::filesystem::path filename, library;
            if (presets::MakeFilename(document.metadata.value("name", ""), &filename, &error)
                && presets::LibraryPath(&library, &error)) {
              to = library / filename;
            }
          }
          if (error.empty() && job == Job::RENAME && from == to.filename()) {
            error = "Choose a different preset name.";
          }
          if (error.empty()) {
            presets::WriteFile(to, document, replace && (job == Job::SAVE || job == Job::EXPORT), &error);
          }
          if (error.empty() && job == Job::RENAME) {
            presets::Delete(from, &error);
          }
        }
      }
      if (error.empty() && (job == Job::SAVE || job == Job::COPY || job == Job::RENAME || job == Job::REMOVE)) {
        if (presets::LibraryPath(&library, &error)) presets::List(&entries, &error);
      }
    } catch (...) {
      error = "Preset operation failed. Check the file and directory permissions.";
    }
    {
      std::unique_lock lock(worker.mutex);
      worker.error = std::move(error);
      worker.document = std::move(document);
      worker.files = std::move(entries);
      worker.library = std::move(library);
      worker.destination = std::move(to);
      worker.cancelled = cancelled;
      worker.ready = true;
      worker.busy = false;
    }
  }
  FreeLibraryAndExitThread(static_cast<HMODULE>(module), 0);
}

inline void Start(Job job, const std::filesystem::path& from = {}, const std::filesystem::path& to = {},
                  const std::string& text = {}, const presets::Document& document = {}) {
  std::unique_lock lock(worker.mutex);
  if (worker.busy || worker.ready) {
    return;
  }
  worker.job = job;
  worker.path = from;
  worker.destination = to;
  worker.text = text;
  worker.document = document;
  worker.overwrite = overwrite;
  worker.error.clear();
  worker.cancelled = false;
  notice.clear();
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                          reinterpret_cast<LPCWSTR>(WorkerMain), &module)) {
    notice = "Could not start preset worker.";
    return;
  }
  worker.busy = true;
  if (HANDLE thread = CreateThread(nullptr, 0, WorkerMain, module, 0, nullptr)) {
    CloseHandle(thread);
  } else {
    worker.busy = false;
    FreeLibrary(module);
    notice = "Could not start preset worker.";
  }
}

inline void Pump() {
  std::unique_lock lock(worker.mutex);
  if (!worker.ready) {
    return;
  }
  worker.ready = false;
  if (!worker.library.empty()) library_path = worker.library;
  if (worker.cancelled) {
    notice = "Preset operation cancelled.";
    return;
  }
  if (!worker.error.empty()) {
    notice = worker.error;
    return;
  }
  if (worker.job == Job::REFRESH) {
    files = std::move(worker.files);
    if (std::find(files.begin(), files.end(), selected) == files.end()) {
      selected.clear();
      confirm_delete = false;
    }
  } else if (worker.job == Job::CHOOSE_EXPORT || worker.job == Job::CHOOSE_FOLDER) {
    auto destination = worker.destination;
    if (worker.job == Job::CHOOSE_FOLDER) {
      std::filesystem::path filename;
      std::string error;
      if (!presets::MakeFilename(name, &filename, &error)) {
        notice = error;
        return;
      }
      destination /= filename;
    }
    const auto text = destination.u8string();
    if (text.size() >= sizeof(path)) {
      notice = "The selected path is too long for this input field.";
      return;
    }
    std::memcpy(path, text.c_str(), text.size() + 1);
    notice = "Export destination selected. Confirm export to write the file.";
  } else if (worker.job == Job::OPEN_FOLDER) {
    notice = "Preset folder opened.";
  } else if (worker.job == Job::READ || worker.job == Job::PARSE || worker.job == Job::CHOOSE_IMPORT) {
    preview = std::move(worker.document);
    have_preview = true;
    compatibility = confirm_full = false;
    show_kept = false;
    rows = Preview(preview, compatibility);
    Navigate(2);
  } else {
    notice = "Preset file saved.";
    if (worker.job == Job::EXPORT) {
      notice = "Preset file exported.";
    }
    if (worker.job == Job::REMOVE) {
      notice = "Preset deleted. Applied game settings were kept.";
    }
    if (worker.job == Job::SAVE) {
      active_name = worker.document.metadata.value("name", "");
      active_settings = worker.document.settings;
    }
    if (worker.job != Job::EXPORT) {
      files = std::move(worker.files);
      if (std::find(files.begin(), files.end(), selected) == files.end()) {
        selected.clear();
        confirm_delete = false;
      }
    }
  }
  if (*path == '\0' && !library_path.empty()) {
    std::filesystem::path filename;
    std::string error;
    if (presets::MakeFilename(name, &filename, &error)) {
      const auto text = (library_path / filename).u8string();
      if (text.size() < sizeof(path)) std::memcpy(path, text.c_str(), text.size() + 1);
    }
  }
}

inline bool Dirty() {
  for (const auto& [key, value] : active_settings.items()) {
    if (ReadSetting(key) != value) {
      return true;
    }
  }
  return false;
}

inline void DrawRows(const std::vector<Difference>& differences) {
  const char* groups[] = {"All groups", "Quality", "Multi-Pass", "Compatibility", "General"};
  if (ImGui::BeginCombo(ui::ControlLabel(ui::Tr("Group")).c_str(), ui::Tr(groups[group_choice]))) {
    for (int i = 0; i < 5; ++i) {
      if (ImGui::Selectable(ui::Tr(groups[i]), i == group_choice)) {
        group_choice = i;
      }
    }
    ImGui::EndCombo();
  }
  ui::Checkbox(ui::Tr("Show unchanged fields"), &show_kept);
  if (ImGui::BeginChild("##preset_differences", ImVec2(0.f, ImGui::GetFontSize() * 13.f), ImGuiChildFlags_Borders)) {
    for (const auto& row : differences) {
      if (!show_kept && (std::string_view(row.status) == "Unchanged" || std::string_view(row.status) == "Kept (not in preset)")) {
        continue;
      }
      if (group_choice != 0 && (row.rule == nullptr || static_cast<int>(row.rule->category) != group_choice - 1)) {
        continue;
      }
      ImGui::PushID(row.key.c_str());
      if (row.rule != nullptr && row.rule->pass != 0) {
        ImGui::Text("%s %u", ui::Tr("Pass"), row.rule->pass);
      }
      ImGui::TextWrapped("%s", row.rule != nullptr ? ui::Tr(row.rule->label) : row.key.c_str());
      ImGui::TextColored(row.apply ? ui::CurrentTheme().accent : ui::CurrentTheme().text_secondary,
                         "%s", ui::Tr(row.status));
      if (row.key == "EnableHooks" || row.key == "DX11Source" || row.key == "ForeignNr") {
        ImGui::TextWrapped("%s", ui::Tr("Requires game restart"));
      } else if (row.rule != nullptr && row.rule->category == presets::FieldRule::Category::COMPATIBILITY) {
        ImGui::TextColored(ui::CurrentTheme().warn, "%s", ui::Tr("Game-dependent setting"));
      }
      const bool automatic = row.key == "EnableHooks" || row.key == "DX11Source" || row.key == "ForeignNr";
      ImGui::TextWrapped("%s: %s", ui::Tr("Current"), row.before.is_null() ? (automatic ? ui::Tr("Auto") : "-") : row.before.dump().c_str());
      ImGui::TextWrapped("%s: %s", ui::Tr("Preset"), row.after.dump().c_str());
      ImGui::Separator();
      ImGui::PopID();
    }
  }
  ImGui::EndChild();
}

inline void DrawManager() {
  Pump();
  profile_ui::Initialize();
  if (profile_ui::share_request) {
    export_document = std::move(profile_ui::share_request);
    profile_ui::share_request.reset();
    const auto value = export_document->metadata.value("name", "");
    std::memcpy(name, value.c_str(), (std::min)(sizeof(name), value.size() + 1));
    Navigate(1);
    scope_choice = 1;
  }
  if (!ImGui::IsPopupOpen("###nr_preset_manager")) {
    manager_geometry.was_open = false;
    return;
  }
  const auto display = ImGui::GetIO().DisplaySize;
  const float em = (std::max)(ImGui::GetFontSize(), 1.f);
  const float margin = (std::min)(em, (std::min)(display.x, display.y) * 0.02f);
  const ImVec2 maximum((std::max)(1.f, display.x - margin * 2.f),
                       (std::max)(1.f, display.y - margin * 2.f));
  ImGui::SetNextWindowSizeConstraints(ImVec2((std::min)(em * 22.f, maximum.x),
                                            (std::min)(em * (mode == 2 && !have_preview ? 12.f : 18.f), maximum.y)), maximum);
  const bool environment_changed = display.x != manager_geometry.display.x
      || display.y != manager_geometry.display.y || em != manager_geometry.font_size;
  const float compact_height = (mode == 2 && !have_preview ? 14.f
      : mode == 1 && !save_options && !export_options ? 20.f : 0.f);
  if (compact_height != manager_geometry.compact_height_em) {
    if (compact_height != 0.f) {
      if (manager_geometry.compact_height_em == 0.f) manager_geometry.full_size_em = manager_geometry.size_em;
      manager_geometry.size_em.y = compact_height;
    } else {
      manager_geometry.size_em = manager_geometry.full_size_em;
    }
    manager_geometry.compact_height_em = compact_height;
    manager_geometry.task_resize = true;
  }
  if (!manager_geometry.was_open || environment_changed || manager_geometry.task_resize) {
    const ImVec2 size((std::min)(manager_geometry.size_em.x * em, maximum.x),
                      (std::min)(manager_geometry.size_em.y * em, maximum.y));
    const ImGuiCond condition = (manager_geometry.valid ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(size, condition);
    ImGui::SetNextWindowPos(ImVec2(margin + (maximum.x - size.x) * manager_geometry.relative_position.x,
                                  margin + (maximum.y - size.y) * manager_geometry.relative_position.y), condition);
  }
  const std::string title = std::string(ui::Tr("Profile library")) + "###nr_preset_manager";
  bool open = true;
  if (!ImGui::BeginPopupModal(title.c_str(), &open, ImGuiWindowFlags_NoSavedSettings)) {
    manager_geometry.was_open = false;
    if (!open) {
      Navigate(0);
    }
    return;
  }
  // Modal navigation owns Escape only while neither an ImGui editor nor the
  // native assisted editor is using it. A nested popup owns its own dismissal.
  if (!unicode_input::Busy() && !ui::text_input::pending.result
      && !ImGui::IsAnyItemActive() && !ImGui::GetIO().WantTextInput
      && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
      && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    open = false;
  }
  const auto size = ImGui::GetWindowSize();
  const auto position = ImGui::GetWindowPos();
  manager_geometry.size_em = ImVec2(size.x / em, size.y / em);
  if (manager_geometry.compact_height_em != 0.f && !environment_changed && !manager_geometry.task_resize
      && manager_geometry.was_open && (std::abs(size.x - manager_geometry.last_size.x) > 1.f
                                       || std::abs(size.y - manager_geometry.last_size.y) > 1.f)) {
    manager_geometry.full_size_em = manager_geometry.size_em;
  }
  manager_geometry.last_size = size;
  manager_geometry.task_resize = false;
  manager_geometry.relative_position = ImVec2(
      std::clamp((position.x - margin) / (std::max)(1.f, maximum.x - size.x), 0.f, 1.f),
      std::clamp((position.y - margin) / (std::max)(1.f, maximum.y - size.y), 0.f, 1.f));
  manager_geometry.display = display;
  manager_geometry.font_size = em;
  manager_geometry.valid = true;
  manager_geometry.was_open = open;
  if (!open) {
    Navigate(0);
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }
  ImGui::PushTextWrapPos(0.f);
  bool busy;
  {
    std::shared_lock lock(worker.mutex);
    busy = worker.busy;
  }
  ImGui::BeginDisabled(busy);
  if (mode != 0) {
    if (ui::ActionButton(ui::Tr("Back to profile library"), "preset_home")) {
      Navigate(0);
    }
    ImGui::TextUnformatted(ui::Tr(mode == 1 ? "Save current profile" : "Import profile"));
  }
  const float footer = (busy || !notice.empty() ? ImGui::GetFontSize() * 3.5f : 0.f);
  if (ImGui::BeginChild("##preset_content", ImVec2(0.f, -footer), ImGuiChildFlags_None,
                        ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
    if (reset_scroll) {
      ImGui::SetScrollY(0.f);
      reset_scroll = false;
    }
    ui::text_input::Hint();
    if (mode == 0) {
      ImGui::TextWrapped("%s: %s", ui::Tr("Current"), profile_ui::Summary().c_str());
      if (ui::ActionButton(ui::Tr("Save current profile"), "preset_save_task")) {
        export_document.reset();
        Navigate(1);
      }
      ui::SameLineIfFits(ImGui::CalcTextSize(ui::Tr("Import profile")).x + ImGui::GetStyle().FramePadding.x * 2.f);
      if (ui::ActionButton(ui::Tr("Import profile"), "preset_import_task")) {
        Navigate(2);
      }
      if (!undo.empty() && ui::ActionButton(ui::Tr("Undo last import"), "undo_preset")) {
        std::string error;
        if (!Undo(&error)) notice = error;
      }
      if (ui::TreeNode("custom_slot_tools", ui::Tr("Custom slots"), 0, false)) {
        profile_ui::DrawCustomTools();
        ImGui::TreePop();
      }
      ui::text_input::DrawLabeled(ui::Tr("Search presets"), search, sizeof(search));
      if (ui::ActionButton(ui::Tr("Refresh library"), "refresh_presets")) {
        Start(Job::REFRESH);
      }
      if (files.empty()) {
        ImGui::TextWrapped("%s", ui::Tr("Save your settings to create a shared local preset."));
      }
      if (ImGui::BeginListBox("##preset_files", ImVec2(-1.f, ImGui::GetFontSize() * 8.f))) {
        for (const auto& file : files) {
          const auto utf8 = file.filename().u8string();
          const std::string label(reinterpret_cast<const char*>(utf8.data()), utf8.size());
          if (*search != '\0' && label.find(search) == std::string::npos) {
            continue;
          }
          ImGui::PushID(label.c_str());
          const ImVec2 at = ImGui::GetCursorScreenPos();
          if (ImGui::Selectable("##preset_file", selected == file, 0, ImVec2(0.f, ImGui::GetTextLineHeight()))) {
            if (selected != file) {
              selected = file;
              confirm_delete = false;
              notice.clear();
            }
          }
          ImGui::GetWindowDrawList()->AddText(at, ImGui::GetColorU32(ImGuiCol_Text),
                                              ui::Ellipsize(label.c_str(), ImGui::GetContentRegionAvail().x).c_str());
          ui::ItemTooltip("%s", label.c_str());
          ImGui::PopID();
        }
        ImGui::EndListBox();
      }
      if (!selected.empty()) {
        const auto selected_bytes = selected.filename().u8string();
        ImGui::TextWrapped("%s", reinterpret_cast<const char*>(selected_bytes.c_str()));
        if (ui::ActionButton(ui::Tr("Preview selected preset"), "preview_selected")) {
          Start(Job::READ, selected);
        }
        ImGui::SetNextItemWidth(-1.f);
        ui::text_input::DrawLabeled(ui::Tr("Preset name"), name, sizeof(name));
        ImGui::BeginDisabled(*name == '\0');
        if (ui::ActionButton(ui::Tr("Rename preset"), "rename_preset")) {
          Start(Job::RENAME, selected, {}, name);
        }
        ui::SameLineIfFits(ImGui::CalcTextSize(ui::Tr("Duplicate preset")).x + ImGui::GetStyle().FramePadding.x * 2.f);
        if (ui::ActionButton(ui::Tr("Duplicate preset"), "duplicate_preset")) {
          Start(Job::COPY, selected, {}, name);
        }
        ImGui::EndDisabled();
        ui::Checkbox(ui::Tr("Confirm deletion of selected file"), &confirm_delete);
        ImGui::BeginDisabled(!confirm_delete);
        if (ui::ActionButton(ui::Tr("Delete preset"), "delete_preset")) {
          Start(Job::REMOVE, selected);
          confirm_delete = false;
        }
        ImGui::EndDisabled();
      }
      ImGui::Separator();
    }
    if (mode == 2) {
      if (ui::ActionButton(ui::Tr("Choose preset file"), "choose_import")) Start(Job::CHOOSE_IMPORT);
      if (ui::ActionButton(ui::Tr("Paste share text"), "paste_preset")) {
        std::string text;
        if (OpenClipboard(nullptr)) {
          if (HANDLE handle = GetClipboardData(CF_UNICODETEXT)) {
            const SIZE_T bytes = GlobalSize(handle);
            if (bytes > 0 && bytes <= presets::MAX_FILE_BYTES * sizeof(wchar_t)) {
              if (const auto* data = static_cast<const wchar_t*>(GlobalLock(handle))) {
                const size_t length = wcsnlen_s(data, bytes / sizeof(wchar_t));
                if (length < bytes / sizeof(wchar_t)) {
                  const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, data, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
                  if (size > 0 && size <= static_cast<int>(presets::MAX_FILE_BYTES)) {
                    text.resize(size);
                    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, data, static_cast<int>(length), text.data(), size, nullptr, nullptr);
                  }
                }
                GlobalUnlock(handle);
              }
            }
          }
          CloseClipboard();
        }
        if (text.empty()) {
          notice = "Clipboard is empty, unavailable or exceeds the preset size limit.";
        } else {
          Start(Job::PARSE, {}, {}, text);
        }
      }
      ui::Checkbox(ui::Tr("Enter file path"), &import_path_options);
      if (import_path_options) {
        ImGui::TextWrapped("%s", ui::Tr("Import a file by its full path. Opening it only prepares a preview."));
        ImGui::SetNextItemWidth(-1.f);
        ui::text_input::DrawLabeled(ui::Tr("File path"), path, sizeof(path));
        if (ui::ActionButton(ui::Tr("Read preset file"), "read_preset")) {
          Start(Job::READ, std::filesystem::u8path(path));
        }
      }
    } else if (mode == 1) {
      ImGui::TextWrapped("%s", ui::Tr(export_document ? "Exporting the saved custom slot snapshot." : "Exporting current game settings."));
      if (export_document && ui::ActionButton(ui::Tr("Use current game settings"), "export_current")) export_document.reset();
      ui::text_input::DrawLabeled(ui::Tr("Preset name"), name, sizeof(name));
      ui::Checkbox(ui::Tr("More options"), &save_options);
      if (save_options) {
        const char* scopes[] = {"Quality preset", "Quality + Multi-Pass", "Full configuration backup"};
        if (ImGui::BeginCombo(ui::ControlLabel(ui::Tr("Export scope")).c_str(), ui::Tr(scopes[scope_choice]))) {
          for (int i = 0; i < 3; ++i) {
            ImGui::BeginDisabled(export_document.has_value() && i == 2);
            if (ImGui::Selectable(ui::Tr(scopes[i]), i == scope_choice)) {
              scope_choice = i;
            }
            ImGui::EndDisabled();
          }
          ImGui::EndCombo();
        }
        if (scope_choice == 2) {
          ImGui::TextColored(ui::CurrentTheme().warn, "%s", ui::Tr("Full backups are not guaranteed to work across games."));
        }
        ui::text_input::DrawLabeled(ui::Tr("Author (optional)"), author, sizeof(author));
        ui::text_input::DrawLabeled(ui::Tr("Source game (optional)"), source_game, sizeof(source_game));
        ui::text_input::Draw("##preset_description", description, sizeof(description), true,
                             ImVec2(-1.f, ImGui::GetFontSize() * 3.f), ui::Tr("Description (optional)"));
      }
      const auto scope = static_cast<presets::Scope>(scope_choice);
      const Json metadata = {{"name", name}, {"author", author}, {"description", description}, {"source_game", source_game}, {"created_with", NR_NEXUS_NAME " " NR_NEXUS_VERSION}};
      ImGui::BeginDisabled(*name == '\0');
      if (save_options && ui::ActionButton(ui::Tr("Preview export fields"), "preview_export")) {
        export_rows = Preview(ExportSnapshot(scope, metadata), true);
        export_rows.erase(std::remove_if(export_rows.begin(), export_rows.end(), [](const auto& row) { return std::string_view(row.status) == "Kept (not in preset)"; }), export_rows.end());
        export_scope = scope_choice;
        show_kept = true;
      }
      if (ui::ActionButton(ui::Tr("Save preset to library"), "save_preset")) {
        Start(Job::SAVE, {}, {}, {}, ExportSnapshot(scope, metadata));
      }
      ui::Checkbox(ui::Tr("Export file"), &export_options);
      if (save_options || export_options || overwrite) {
        ui::Checkbox(ui::Tr("Replace an existing file with this name"), &overwrite);
      }
      if (export_options) {
        if (!library_path.empty()) {
          const auto bytes = library_path.u8string();
          const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
          ImGui::TextWrapped("%s: %s", ui::Tr("Preset library"), text.c_str());
        }
        if (ui::ActionButton(ui::Tr("Open preset folder"), "open_preset_folder")) Start(Job::OPEN_FOLDER);
        if (ui::ActionButton(ui::Tr("Choose export file"), "choose_export")) {
          std::filesystem::path filename;
          std::string error;
          if (presets::MakeFilename(name, &filename, &error)) Start(Job::CHOOSE_EXPORT, filename);
          else notice = error;
        }
        ui::SameLineIfFits(ImGui::CalcTextSize(ui::Tr("Browse folder")).x + ImGui::GetStyle().FramePadding.x * 2.f);
        if (ui::ActionButton(ui::Tr("Browse folder"), "choose_folder")) Start(Job::CHOOSE_FOLDER);
        ImGui::SetNextItemWidth(-1.f);
        ui::text_input::DrawLabeled(ui::Tr("Export file path"), path, sizeof(path));
        ImGui::BeginDisabled(*path == '\0');
        if (ui::ActionButton(ui::Tr("Export preset file"), "export_preset")) {
          Start(Job::EXPORT, {}, std::filesystem::u8path(path), {}, ExportSnapshot(scope, metadata));
        }
        ImGui::EndDisabled();
      }
      if (ui::ActionButton(ui::Tr("Copy share text"), "copy_preset")) {
        try {
          notice = (CopyReportText(presets::Serialize(ExportSnapshot(scope, metadata)))
                        ? "Preset JSON copied to clipboard."
                        : "Clipboard is empty, unavailable or exceeds the preset size limit.");
        } catch (...) {
          notice = "Preset export failed. Check the metadata.";
        }
      }
      ImGui::EndDisabled();
      if (save_options && export_scope == scope_choice && !export_rows.empty()) {
        DrawRows(export_rows);
      }
    }
    if (mode == 2 && have_preview) {
      ImGui::TextWrapped("%s", preview.metadata.value("name", "").c_str());
      ImGui::TextWrapped("%s", preview.metadata.value("description", "").c_str());
      if (preview.metadata.contains("source_game")) {
        ImGui::TextWrapped("%s: %s", ui::Tr("Source game (optional)"), preview.metadata.value("source_game", "").c_str());
      }
      const char* scope_names[] = {"Quality preset", "Quality + Multi-Pass", "Full configuration backup"};
      ImGui::TextWrapped("%s: %s", ui::Tr("Export scope"), ui::Tr(scope_names[static_cast<int>(preview.scope)]));
      ImGui::TextWrapped("%s", ui::Tr("Only confirmed valid fields in this preset will change. Missing fields keep their current values."));
      if (preview.scope == presets::Scope::FULL) {
        ImGui::TextColored(ui::CurrentTheme().warn, "%s", ui::Tr("Full backups are not guaranteed to work across games."));
        ui::Checkbox(ui::Tr("Confirm the full backup scope shown below"), &confirm_full);
      }
      if (ui::Checkbox(ui::Tr("Include game compatibility settings"), &compatibility)) {
        rows = Preview(preview, compatibility);
      }
      ImGui::TextWrapped("%s", ui::Tr("Hook, HDR, depth, motion, resolution and FG settings depend on this game and API. Existing runtime gates remain active."));
      bool dual_models;
      {
        RuntimeLock lock(runtime_mutex);
        dual_models = direct_runtime_requirements.dual_models;
      }
      if (!dual_models) {
        ImGui::TextColored(ui::CurrentTheme().warn, "%s", ui::Tr("Unsupported model requests are preserved; the runtime uses the existing fallback policy."));
      }
      DrawRows(rows);
      ImGui::BeginDisabled(preview.scope == presets::Scope::FULL && !confirm_full);
      if (ui::ActionButton(ui::Tr("Confirm and apply preset"), "apply_preset")) {
        const auto refreshed = Preview(preview, compatibility);
        const bool changed = rows.size() != refreshed.size() || !std::equal(rows.begin(), rows.end(), refreshed.begin(), [](const auto& left, const auto& right) { return left.key == right.key && left.before == right.before; });
        if (changed) {
          notice = "Settings changed. Review the refreshed preview and confirm again.";
        } else {
          std::string error;
          if (!Apply(preview, compatibility, true, &error)) {
            notice = error;
          }
        }
        rows = Preview(preview, compatibility);
      }
      ImGui::EndDisabled();
      const char* target_labels[] = {"Custom 1", "Custom 2", "Custom 3"};
      ImGui::SetNextItemWidth(-1.f);
      const int target = ui::Segmented("##import_slot_target", target_labels, 3, profile_ui::edit_index - 3, nullptr);
      if (target >= 0) {
        profile_ui::edit_index = target + 3;
        profile_ui::confirm_replace = false;
      }
      ui::Checkbox(ui::Tr("Confirm replacing or clearing this slot"), &profile_ui::confirm_replace);
      ImGui::BeginDisabled((preview.scope == presets::Scope::FULL && !confirm_full)
                           || !profile_ui::loaded || !profile_ui::writable
                           || (profile_ui::state.slots[profile_ui::edit_index].populated && !profile_ui::confirm_replace));
      if (ui::ActionButton(ui::Tr("Assign NR fields to custom slot"), "import_to_slot")) {
        auto settings = (profile_ui::state.slots[profile_ui::edit_index].populated
                             ? profile_ui::state.slots[profile_ui::edit_index].snapshot : profile_ui::Snapshot());
        const auto differences = Preview(preview, false);
        bool invalid = false;
        for (const auto& row : differences) {
          invalid |= profile_ui::Managed(row.key) && std::string_view(row.status) == "Invalid value";
          if (profile_ui::Managed(row.key) && (row.apply || std::string_view(row.status) == "Unchanged")) settings[row.key] = row.after;
        }
        if (invalid) profile_ui::notice = "Profile contains unsupported settings. Current settings were kept.";
        else profile_ui::Assign(profile_ui::edit_index, settings);
        notice = profile_ui::notice;
      }
      ImGui::EndDisabled();
      ui::SameLineIfFits(ImGui::CalcTextSize(ui::Tr("Cancel import")).x + ImGui::GetStyle().FramePadding.x * 2.f);
      if (ui::ActionButton(ui::Tr("Cancel import"), "cancel_import")) {
        have_preview = false;
        rows.clear();
        Navigate(0);
      }
    } else if (mode == 2) {
      ImGui::TextWrapped("%s", ui::Tr("Select a local preset, read a file or paste share text to preview changes."));
    }
  }
  ImGui::EndChild();
  ImGui::EndDisabled();
  if (busy || !notice.empty()) {
    if (ImGui::BeginChild("##preset_notice", ImVec2(0.f, ImGui::GetFontSize() * 3.f))) {
      if (busy) {
        ImGui::TextDisabled("%s", ui::Tr("Working..."));
      } else if (!notice.empty()) {
        ImGui::TextWrapped("%s", ui::Tr(notice.c_str()));
      }
    }
    ImGui::EndChild();
  }
  ImGui::PopTextWrapPos();
  ImGui::EndPopup();
}

inline void DrawEntry(bool multipass = false, bool management_only = false) {
  ImGui::PushID("presets");
  bool open = false;
  if (!multipass && !management_only) {
    open = profile_ui::DrawQuick();
  }
  if (multipass && profile_ui::active >= 0) {
    ImGui::TextWrapped("%s: %s", ui::Tr("NR Profiles"), profile_ui::Name(profile_ui::active).c_str());
  }
  if (!multipass && management_only) {
    open = ui::ActionButton(ui::Tr("Manage profiles"), "open_presets");
  }
  if (open) {
    Navigate(0);
    Start(Job::REFRESH);
    ImGui::OpenPopup("###nr_preset_manager");
  }
  if (!multipass) {
    if (management_only) {
      ImGui::BeginDisabled(undo.empty());
      ui::SameLineIfFits(ImGui::CalcTextSize(ui::Tr("Undo last import")).x + ImGui::GetStyle().FramePadding.x * 2.f);
      if (ui::ActionButton(ui::Tr("Undo last import"), "undo_preset")) {
        std::string error;
        if (!Undo(&error)) {
          notice = error;
        }
      }
      ImGui::EndDisabled();
    }
    DrawManager();
  }
  ImGui::PopID();
}

}  // namespace preset_ui
