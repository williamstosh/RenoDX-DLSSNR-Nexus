/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Composed inside dlssnr.hpp, after preset_ui's verified batch application.
namespace profile_ui {
using Json = presets::Json;
namespace data = profiles;

inline bool Managed(std::string_view key) {
  const auto* rule = presets::FindRule(key);
  return rule != nullptr && (rule->category == presets::FieldRule::QUALITY
                             || rule->category == presets::FieldRule::MULTIPASS
                             || key == "NRPresentGuides");
}

inline bool Validate(std::string_view key, const Json& value) {
  const auto* rule = presets::FindRule(key);
  return Managed(key) && preset_ui::ValidateValue(*rule, value);
}

inline Json Snapshot(bool defaults = false) {
  Json result = Json::object();
  RuntimeLock lock(runtime_mutex);
  for (const auto& rule : presets::FieldRules()) {
    if (!Managed(rule.key)) continue;
    const auto* setting = preset_ui::FindSetting(rule.key);
    if (setting == nullptr) continue;
    if (!defaults) {
      result[rule.key] = preset_ui::ReadSetting(rule.key);
    } else if (setting->b != nullptr) {
      result[rule.key] = setting->fallback != 0.f;
    } else if (setting->u != nullptr) {
      result[rule.key] = static_cast<uint32_t>(setting->fallback);
    } else {
      result[rule.key] = setting->fallback;
    }
  }
  return result;
}

enum class Action { NONE, SWITCH, RESTORE, UNDO };
struct UndoState {
  data::State before;
  data::State after;
  Json running_before = Json::object();
  Json running_after = Json::object();
  int previous_active = -1;
  bool available = false;
  bool configuration = false;
  const char* action_label = "NR Profiles";
  int action_index = -1;
};
inline data::State state;
inline Json defaults_snapshot = Json::object();
inline Json observed_snapshot = Json::object();
inline UndoState undo;
inline int active = -1;
inline bool loaded = false;
inline bool dirty = false;
inline bool writable = true;
inline bool save_failed = false;
inline std::string game_id;
inline std::filesystem::path storage_path;
inline std::string notice;
inline std::string reset_notice;
inline std::string reset_error;
inline uint64_t serial = 0;
inline uint64_t observed_epoch = 0;
inline uint64_t observed_reload_epoch = 0;
inline Action pending_action = Action::NONE;
inline int pending_index = -1;
inline int edit_index = 3;
inline int copy_official = 0;
inline char rename[129] = {};
inline bool confirm_replace = false;
inline std::optional<presets::Document> share_request;
enum class SlotTask { NONE, RENAME, COPY, IMPORT, EXPORT, CLEAR, RESTORE };
inline SlotTask slot_task = SlotTask::NONE;
inline int task_index = -1;
struct SlotAnchor {
  ImVec2 minimum;
  ImVec2 maximum;
  ImVec2 menu_minimum;
  ImVec2 menu_maximum;
  bool valid = false;
};
inline SlotAnchor slot_anchor;
struct SlotMenuBounds {
  ImVec2 position;
  ImVec2 size;
};
// Supplied by the overlay before entering its scrolling children.
inline SlotMenuBounds slot_menu_bounds;

inline ImVec2 SlotWindowPosition(const SlotAnchor& anchor, ImVec2 size) {
  // The pinned ReShade ImGui table exposes one overlay surface, not viewport getters.
  const float gap = ImGui::GetStyle().ItemSpacing.x;
  const ImVec2 minimum(0.f, 0.f);
  const auto maximum = ImGui::GetIO().DisplaySize;
  ImVec2 position(anchor.minimum.x, anchor.maximum.y + gap);
  if (position.y + size.y > maximum.y) {
    if (anchor.minimum.y - gap - size.y >= minimum.y) {
      position.y = anchor.minimum.y - gap - size.y;
    } else if (anchor.minimum.x - gap - size.x >= minimum.x) {
      position = ImVec2(anchor.minimum.x - gap - size.x, anchor.minimum.y);
    }
  }
  position.x = std::clamp(position.x, minimum.x, (std::max)(minimum.x, maximum.x - size.x));
  position.y = std::clamp(position.y, minimum.y, (std::max)(minimum.y, maximum.y - size.y));
  return position;
}

enum class ResetPhase { NONE, FLUSH, STORE, ROLLBACK };
struct ResetTransaction {
  ResetPhase phase = ResetPhase::NONE;
  int target = -1;
  data::State before;
  data::State detached;
  UndoState previous_undo;
  int previous_active = -1;
  presets::Document document;
  Json configuration = Json::object();
  uint64_t reload_epoch = 0;
  uint64_t commit_epoch = 0;
  std::string error;
  bool recovery_failed = false;
  bool restore_official = false;
  bool clear_custom = false;
};
inline ResetTransaction reset;

// Supplied before LoadConfiguration/migrations write any NR defaults. Unknown,
// unreadable INI, legacy keys and an explicit detach all count as EXISTING.
enum class FirstInstallEvidence { UNKNOWN, EXISTING, PROVEN_EMPTY };
struct FirstInstallTransaction {
  bool pending = false;
  data::State before;
  Json configuration = Json::object();
  Json target = Json::object();
  uint64_t commit_epoch = 0;
  uint64_t reload_epoch = 0;
  std::function<bool(const Json&, std::string*)> supported;
};
inline FirstInstallTransaction first_install;
inline bool fresh_storage = false;

// Host callbacks run on the profile/UI thread, without the storage mutex.
// Validate the complete proposed configuration; an empty callback preserves the
// existing field/preview validation. The host owns import/dialog exclusion.
inline std::function<bool(const Json&, std::string*)> capability_validator;
inline std::function<bool()> interaction_busy;

inline bool Resetting() {
  return reset.phase != ResetPhase::NONE;
}

// Read through the host cache, never the filesystem on the overlay thread.
// Include excluded diagnostics and raw key presence to detect external edits.
inline Json ResetConfiguration() {
  RuntimeLock lock(runtime_mutex);
  Json result = Json::object();
  for (const auto& rule : presets::FieldRules()) {
    const auto raw = ReadPresentRestoreRaw(rule.key);
    result[rule.key] = {{"value", preset_ui::ReadSetting(rule.key)},
                        {"raw", (raw ? Json(*raw) : Json(nullptr))}};
  }
  for (const auto* key : {"ProfileGameId", "ProfileBalancedInitialized", "ConfigVersion", "NRPresentRestoreScopeConfigVersion",
                          "NRPresentSdrRestoreConfigVersion", "NRPresentHdrPedestalConfigVersion"}) {
    const auto raw = ReadPresentRestoreRaw(key);
    result[key] = (raw ? Json(*raw) : Json(nullptr));
  }
  return result;
}

inline bool ResetWritable() {
  std::shared_lock config_lock(present_restore_configuration.mutex);
  std::shared_lock policy_lock(hdr_pedestal_policy_state.mutex);
  return loaded && writable && present_restore_configuration.global_writable
         && present_restore_configuration.scope.writable
         && present_sdr_restore_config_writable.load()
         && hdr_pedestal_policy_state.configuration.writable;
}

inline void OpenSlotTask(SlotTask task, int index) {
  if (Resetting() || reset.recovery_failed) return;
  if (index < 0 || index >= 6)
    return;
  slot_task = task;
  task_index = index;
}

struct Worker {
  std::shared_mutex mutex;
  bool busy = false;
  bool ready = false;
  bool loading = false;
  bool urgent = false;
  uint64_t serial = 0;
  uint64_t saved_serial = 0;
  ULONGLONG due = 0;
  presets::internal::Handle wake;
  data::State pending;
  data::State result;
  std::filesystem::path path;
  std::string id;
  std::string error;
  bool relocated = false;
  bool fresh = false;
};
inline Worker worker;

// One bounded worker exists only during load or pending edits. It owns a module
// reference and flushes the latest committed snapshot even after the overlay closes.
inline DWORD WINAPI WorkerMain(LPVOID module) {
  {
    bool loading;
    std::string id;
    {
      std::shared_lock lock(worker.mutex);
      loading = worker.loading;
      id = worker.id;
    }
    std::string error;
    if (loading) {
      std::wstring executable(32768, L'\0');
      const DWORD count = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
      std::string owner;
      if (count == 0 || count >= executable.size()) {
        error = "Could not identify this game. Profiles were not loaded.";
      } else {
        executable.resize(count);
        for (auto& unit : executable) unit = static_cast<wchar_t>(std::towlower(unit));
        // The private binding contains only a digest, never the installation path.
        unsigned char digest[32] = {};
        if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                       reinterpret_cast<PUCHAR>(executable.data()), static_cast<ULONG>(executable.size() * sizeof(wchar_t)),
                       digest, sizeof(digest)) < 0) {
          error = "Could not identify this game. Profiles were not loaded.";
        } else {
          constexpr char HEX[] = "0123456789abcdef";
          for (const auto byte : digest) {
            owner += HEX[byte >> 4];
            owner += HEX[byte & 15];
          }
        }
      }
      const auto new_id = [&]() {
        unsigned char bytes[16] = {};
        if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return std::string{};
        constexpr char HEX[] = "0123456789abcdef";
        std::string value;
        for (const auto byte : bytes) {
          value += HEX[byte >> 4];
          value += HEX[byte & 15];
        }
        return value;
      };
      const bool new_identity = id.empty();
      bool fresh = false;
      data::State result = data::EmptyState(owner);
      std::filesystem::path path;
      bool relocated = false;
      if (error.empty()) {
        if (id.empty()) id = new_id();
        if (id.empty()) {
          error = "Could not create a game profile identity.";
        } else if (data::StoragePath(id, &path, &error)) {
          const DWORD attributes = GetFileAttributesW(path.c_str());
          if (attributes != INVALID_FILE_ATTRIBUTES) {
            if (data::Read(path, Validate, &result, &error) && result.owner != owner) {
              // A copied INI or moved installation forks its slot data. It must
              // never start writing another game's existing profile document.
              relocated = true;
              id = new_id();
              result.owner = owner;
              result.revision = 0;
              if (id.empty()) error = "Could not create a game profile identity.";
              else data::StoragePath(id, &path, &error);
            }
          } else if (GetLastError() != ERROR_FILE_NOT_FOUND) {
            error = "Could not read game profiles. Current settings were kept.";
          } else {
            fresh = new_identity;
            if (fresh) {
              // An absent INI binding cannot prove first use when this game's
              // older Slot file survives. Inspect only the bounded profile root
              // on the storage worker; unknown/unreadable data is not freshness.
              std::error_code scan_error;
              std::filesystem::directory_iterator entries(path.parent_path().parent_path(), scan_error);
              uint32_t inspected = 0;
              if (scan_error) {
                fresh = false;
              }
              const std::filesystem::directory_iterator end;
              for (; entries != end && !scan_error; entries.increment(scan_error)) {
                const auto& entry = *entries;
                if (++inspected > presets::MAX_LIBRARY_ENTRIES) {
                  fresh = false;
                  break;
                }
                if (entry.path() == path.parent_path()) continue;
                const DWORD flags = GetFileAttributesW(entry.path().c_str());
                if (flags == INVALID_FILE_ATTRIBUTES || (flags & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                  fresh = false;
                  break;
                }
                if ((flags & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
                data::State existing;
                std::string read_error;
                if (!data::Read(entry.path() / L"slots.json", Validate, &existing, &read_error) || existing.owner == owner) {
                  fresh = false;
                  break;
                }
              }
              if (scan_error) {
                fresh = false;
              }
            }
          }
        }
      }
      std::unique_lock lock(worker.mutex);
      worker.result = std::move(result);
      worker.id = std::move(id);
      worker.path = std::move(path);
      worker.relocated = relocated;
      worker.fresh = fresh && error.empty();
      worker.error = std::move(error);
      worker.ready = true;
      worker.busy = false;
    } else {
      for (;;) {
        ULONGLONG due;
        bool urgent;
        {
          std::shared_lock lock(worker.mutex);
          due = worker.due;
          urgent = worker.urgent;
        }
        const ULONGLONG now = GetTickCount64();
        if (!urgent && due > now) {
          WaitForSingleObject(worker.wake.value, static_cast<DWORD>((std::min)(due - now, 500ull)));
          continue;
        }
        data::State saving;
        std::filesystem::path path;
        uint64_t saving_serial;
        {
          std::shared_lock lock(worker.mutex);
          saving = worker.pending;
          path = worker.path;
          saving_serial = worker.serial;
        }
        const bool saved = data::Write(path, &saving, saving.revision, Validate, &error);
        std::unique_lock lock(worker.mutex);
        if (saved) {
          worker.pending.revision = saving.revision;
          worker.result = saving;
          worker.saved_serial = saving_serial;
          if (saving_serial != worker.serial) continue;
        }
        worker.error = std::move(error);
        worker.ready = true;
        worker.busy = false;
        break;
      }
    }
  }
  FreeLibraryAndExitThread(static_cast<HMODULE>(module), 0);
}

inline bool Busy() {
  std::shared_lock lock(worker.mutex);
  return worker.busy || worker.ready;
}

inline void Schedule(bool urgent = false) {
  if (reset.recovery_failed) return;
  if (!loaded || !dirty) return;
  if (!writable && reset.phase != ResetPhase::ROLLBACK) return;
  {
    std::shared_lock lock(worker.mutex);
    if (worker.ready || worker.loading || (Resetting() && reset.phase != ResetPhase::FLUSH && worker.busy)) return;
  }
  if (reset.phase != ResetPhase::ROLLBACK) {
    std::shared_lock lock(present_restore_configuration.mutex);
    if (!present_restore_configuration.global_writable) {
      notice = "Newer or invalid game configuration is read-only.";
      save_failed = true;
      return;
    }
  }
  // Persist the private identity only on an explicit profile edit/selection.
  if (reset.phase != ResetPhase::ROLLBACK) {
    reshade::set_config_value(nullptr, kConfigSection, "ProfileGameId", game_id.c_str());
    if (ReadPresentRestoreRaw("ProfileGameId") != game_id) {
      notice = "Could not save the game profile identity. Changes remain unsaved.";
      save_failed = true;
      return;
    }
  }
  std::unique_lock lock(worker.mutex);
  if (worker.ready || worker.loading) return;
  const uint64_t revision = (worker.busy ? worker.pending.revision : state.revision);
  worker.pending = state;
  worker.pending.revision = revision;
  worker.serial = serial;
  worker.due = GetTickCount64() + (urgent ? 0 : 500);
  worker.urgent = urgent;
  worker.path = storage_path;
  if (worker.busy) {
    SetEvent(worker.wake.value);
    return;
  }
  if (worker.wake.value == INVALID_HANDLE_VALUE || worker.wake.value == nullptr) worker.wake = presets::internal::Handle(CreateEventW(nullptr, FALSE, FALSE, nullptr));
  HMODULE module = nullptr;
  if (worker.wake.value == nullptr || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
          reinterpret_cast<LPCWSTR>(WorkerMain), &module)) {
    notice = "Could not start profile storage. Changes remain unsaved.";
    save_failed = true;
    return;
  }
  worker.busy = true;
  save_failed = false;
  worker.error.clear();
  if (HANDLE thread = CreateThread(nullptr, 0, WorkerMain, module, 0, nullptr)) {
    CloseHandle(thread);
  } else {
    worker.busy = false;
    FreeLibrary(module);
    notice = "Could not start profile storage. Changes remain unsaved.";
    save_failed = true;
  }
}

inline void Changed(bool urgent = false) {
  dirty = true;
  ++serial;
  Schedule(urgent);
}

inline void Observe() {
  if (Resetting() || reset.recovery_failed || first_install.pending) return;
  const auto reloaded = profile_reload_epoch.load(std::memory_order_relaxed);
  if (loaded && reloaded != observed_reload_epoch) {
    observed_reload_epoch = reloaded;
    const auto current = Snapshot();
    if (active >= 0 && current != observed_snapshot) {
      active = -1;
      state.selected.clear();
      observed_snapshot = current;
      observed_epoch = profile_commit_epoch.load();
      notice = "Game settings changed elsewhere. The last selected profile was not reapplied.";
      Changed();
      return;
    }
  }
  const auto epoch = profile_commit_epoch.load(std::memory_order_relaxed);
  if (!loaded || epoch == observed_epoch) return;
  observed_epoch = epoch;
  const Json current = Snapshot();
  if (active < 0) {
    observed_snapshot = current;
    return;
  }
  if (current == observed_snapshot) return;
  auto& slot = state.slots[active];
  if (active < 3) {
    const auto base = data::Template(active, defaults_snapshot);
    for (const auto& [key, value] : current.items()) {
      if (observed_snapshot.contains(key) && observed_snapshot[key] == value) continue;
      if (base.contains(key) && base[key] == value) slot.overrides.erase(key);
      else slot.overrides[key] = value;
    }
  }
  slot.snapshot = current;
  state.last_applied = current;
  observed_snapshot = current;
  Changed();
}

inline bool ValidateSnapshotFields(const Json& settings, std::string* error) {
  if (!settings.is_object()) {
    *error = "Profile contains unsupported settings. Current settings were kept.";
    return false;
  }
  for (const auto& [key, value] : settings.items()) {
    if (!Validate(key, value) || preset_ui::FindSetting(key) == nullptr) {
      *error = "Profile contains unsupported settings. Current settings were kept.";
      return false;
    }
  }
  return true;
}

inline bool ValidateSnapshotCapability(const Json& settings, std::string* error) {
  if (settings.value("NRHookPoint", UINT32_MAX) == kHookPresent
      && (vulkan_present_seen.load() || (D3D11OnlySession() && !Dx11NativeRoute()) || present_unusable.load())) {
    *error = "Profile contains unsupported settings. Current settings were kept.";
    return false;
  }
  if (capability_validator) {
    // A resolved profile is a complete managed target. Validate it as an
    // independent candidate so a stale current Stage or pass count cannot
    // reject a direct atomic switch. Partial callers retain the current
    // values for the field-level validation API.
    bool complete = true;
    for (const auto& rule : presets::FieldRules()) {
      if (Managed(rule.key) && !settings.contains(rule.key)) {
        complete = false;
        break;
      }
    }
    auto proposed = (complete ? settings : Snapshot());
    if (!complete) proposed.update(settings);
    try {
      if (!capability_validator(proposed, error)) {
        if (error->empty()) {
          *error = "Profile contains unsupported settings. Current settings were kept.";
        }
        return false;
      }
    } catch (const std::exception& exception) {
      *error = exception.what();
      return false;
    }
  }
  return true;
}

inline bool ValidateSnapshot(const Json& settings, std::string* error) {
  return ValidateSnapshotFields(settings, error) && ValidateSnapshotCapability(settings, error);
}

// Official Slots are templates plus field-level personal overrides. When the
// recipe version advances, refresh only the derived snapshot and provenance;
// never discard overrides or touch custom snapshots.
inline void MigrateOfficialTemplates() {
  if (!loaded) return;
  bool changed = false;
  for (std::size_t index = 0; index < 3; ++index) {
    auto& slot = state.slots[index];
    if (!slot.populated || slot.template_version >= data::TEMPLATE_VERSION) continue;
    if (slot.snapshot.empty() && slot.overrides.empty()) {
      // Unused templates have no saved recipe to migrate. Keep first-use
      // storage empty so Balanced initialization can own its durable commit.
      slot.template_version = data::TEMPLATE_VERSION;
      continue;
    }
    try {
      const auto migrated = data::Resolve(index, slot, defaults_snapshot, Validate);
      slot.snapshot = migrated;
      slot.template_version = data::TEMPLATE_VERSION;
      changed = true;
    } catch (const std::exception&) {
      // Keep the old manifest intact when a host cannot resolve the new
      // template yet. Resolve remains backward-compatible for a later retry.
    }
  }
  if (changed) Changed(true);
}

inline bool ApplySnapshot(const Json& settings, std::string* error) {
  if (!ValidateSnapshot(settings, error)) return false;
  return preset_ui::Apply(
      {.metadata = Json::object(), .scope = presets::Scope::FULL, .settings = settings}, true, false, error);
}

inline void ExecuteAction(bool persist = true) {
  const auto action = pending_action;
  const int index = pending_index;
  pending_action = Action::NONE;
  pending_index = -1;
  const auto before = state;
  const int previous_active = active;
  const Json running_before = Snapshot();
  Json target;
  std::string error;
  if (action == Action::UNDO) {
    if (!undo.available) return;
    if (undo.configuration) {
      target = Json::object();
      for (const auto& [key, value] : undo.running_after.items()) {
        if (running_before.contains(key) && running_before[key] == value && undo.running_before.contains(key)) {
          target[key] = undo.running_before[key];
        }
      }
      if (!ApplySnapshot(target, &error)) {
        notice = error;
        return;
      }
      const Json restored = Snapshot();
      active = (running_before == undo.running_after ? undo.previous_active : -1);
      state.selected = (active >= 0 ? std::string(data::SLOT_IDS[active]) : "");
      state.last_applied = restored;
      observed_snapshot = restored;
      if (active >= 0) {
        // Undoing a restore also restores the old personal overrides. Later
        // edits detach the association instead of overwriting another slot.
        state.slots[active] = undo.before.slots[active];
        state.slots[active].snapshot = restored;
      }
    } else {
      for (size_t i = 0; i < state.slots.size(); ++i) {
        const auto& saved = undo.after.slots[i];
        const auto& now = state.slots[i];
        if (now.name != saved.name || now.snapshot != saved.snapshot || now.overrides != saved.overrides
            || now.populated != saved.populated) {
          notice = "Profiles changed after this action. Undo was not applied.";
          return;
        }
      }
      const auto revision = state.revision;
      state = undo.before;
      state.revision = revision;
      active = (Snapshot() == state.last_applied ? undo.previous_active : -1);
    }
    undo.available = false;
    observed_epoch = profile_commit_epoch.load();
    Changed(true);
    notice = "Profile action undone. Later edits were kept.";
    return;
  }
  if (index < 0 || index >= 6 || !state.slots[index].populated) {
    notice = "Empty slot. Save current settings or copy an official profile to create it.";
    return;
  }
  auto slot = state.slots[index];
  if (action == Action::RESTORE) slot.overrides = Json::object();
  try {
    target = data::Resolve(index, slot, defaults_snapshot, Validate);
  } catch (const std::exception& exception) {
    notice = exception.what();
    return;
  }
  // Managing an inactive official slot must not select or apply it.
  if (action == Action::RESTORE && active != index) {
    state.slots[index].overrides = slot.overrides;
    state.slots[index].snapshot = target;
    state.slots[index].template_version = data::TEMPLATE_VERSION;
    undo = {.before = before, .after = state, .previous_active = previous_active, .available = true,
            .action_label = "Restore official defaults", .action_index = index};
    Changed(true);
    notice = "Official defaults restored.";
    return;
  }
  if (!ApplySnapshot(target, &error)) {
    notice = error;
    return;
  }
  active = index;
  state.selected = std::string(data::SLOT_IDS[index]);
  state.last_applied = Snapshot();
  state.slots[index].snapshot = state.last_applied;
  state.slots[index].overrides = slot.overrides;
  state.slots[index].template_version = data::TEMPLATE_VERSION;
  observed_snapshot = state.last_applied;
  observed_epoch = profile_commit_epoch.load();
  undo = {.before = before, .after = state, .running_before = running_before,
          .running_after = state.last_applied, .previous_active = previous_active,
          .available = true, .configuration = true,
          .action_label = (action == Action::RESTORE ? "Restore official defaults" : "Apply"),
          .action_index = index};
  if (persist) {
    Changed(true);
  }
  notice = (action == Action::RESTORE ? "Official defaults restored." : "Profile applied. Runtime execution is pending confirmation.");
}

// Host entry point: invoke only after runtime capability discovery. The callback
// must verify the requested Stage/resolution/model contract, without applying it.
// Persist the proposed association first, so IO failures cannot change settings.
inline bool TryInitializeBalanced(FirstInstallEvidence evidence,
                                 const std::function<bool(const Json&, std::string*)>& supported) {
  if (evidence != FirstInstallEvidence::PROVEN_EMPTY || !supported || !fresh_storage
      || !loaded || !ResetWritable() || dirty || Busy() || save_failed || active >= 0
      || Resetting() || reset.recovery_failed || first_install.pending
      || slot_task != SlotTask::NONE || (interaction_busy && interaction_busy())
      || pending_action != Action::NONE || state.revision != 0 || !state.selected.empty()
      || !state.last_applied.empty() || ReadPresentRestoreRaw("ProfileGameId").has_value()
      || ReadPresentRestoreRaw("ProfileBalancedInitialized").has_value()) return false;
  for (std::size_t index = 0; index < state.slots.size(); ++index) {
    const auto& slot = state.slots[index];
    if (!slot.overrides.empty() || !slot.snapshot.empty() || (index >= 3 && slot.populated)) return false;
  }
  Json target;
  std::string error;
  try {
    target = data::Resolve(1, state.slots[1], defaults_snapshot, Validate);
    if (!ValidateSnapshot(target, &error) || !supported(target, &error)) return false;
  } catch (const std::exception& exception) {
    notice = exception.what();
    return false;
  }
  reshade::set_config_value(nullptr, kConfigSection, "ProfileBalancedInitialized", "1");
  if (ReadPresentRestoreRaw("ProfileBalancedInitialized") != "1") {
    notice = "Could not save first-install marker. Current settings were kept.";
    return false;
  }
  fresh_storage = false;
  first_install = {.pending = true, .before = state, .target = target,
                   .commit_epoch = profile_commit_epoch.load(), .reload_epoch = profile_reload_epoch.load(),
                   .supported = supported};
  state.selected = std::string(data::SLOT_IDS[1]);
  state.last_applied = target;
  state.slots[1].snapshot = target;
  state.slots[1].template_version = data::TEMPLATE_VERSION;
  Changed(true);
  first_install.configuration = ResetConfiguration();
  return true;
}

inline void Request(Action action, int index = -1) {
  if (first_install.pending) return;
  if (Resetting() || reset.recovery_failed) return;
  if (interaction_busy && interaction_busy()) return;
  if (!loaded || !writable || pending_action != Action::NONE) return;
  SettleSlider(true);
  Observe();
  if (action == Action::SWITCH && (index < 0 || index >= 6 || !state.slots[index].populated)) {
    if (index >= 3 && index < 6) edit_index = index;
    notice = "Empty slot. Save current settings or copy an official profile to create it.";
    return;
  }
  pending_action = action;
  pending_index = index;
  if (dirty || Busy()) {
    Schedule(true);
    if (save_failed) pending_action = Action::NONE;
  } else ExecuteAction();
}

inline bool SlotAvailable(int index, std::string* error) {
  if (index < 0 || index >= 6 || !state.slots[index].populated) return false;
  try {
    const auto settings = data::Resolve(index, state.slots[index], defaults_snapshot, Validate);
    return ValidateSnapshot(settings, error);
  } catch (const std::exception& exception) {
    if (error != nullptr) {
      *error = exception.what();
    }
    return false;
  }
}

struct SlotDisplayCache {
  data::Slot source;
  Json defaults = Json::object();
  Json settings = Json::object();
  std::string error;
  bool ready = false;
  bool valid = false;
  uint64_t rebuilds = 0;
};
inline std::array<SlotDisplayCache, 6> slot_display_cache;

// Cache only the saved payload and its field validation. Environment eligibility
// is checked on every draw; switching still resolves and validates a fresh target.
inline bool SlotDisplayAvailable(int index, std::string* error) {
  if (index < 0 || index >= 6 || !state.slots[index].populated) return false;
  const auto& slot = state.slots[index];
  auto& cached = slot_display_cache[index];
  if (!cached.ready || cached.source.populated != slot.populated
      || cached.source.template_version != slot.template_version
      || cached.source.overrides != slot.overrides || cached.source.snapshot != slot.snapshot
      || cached.defaults != defaults_snapshot) {
    cached.ready = true;
    cached.source = slot;
    cached.defaults = defaults_snapshot;
    cached.error.clear();
    ++cached.rebuilds;
    try {
      cached.settings = data::Resolve(index, slot, defaults_snapshot, Validate);
      cached.valid = ValidateSnapshotFields(cached.settings, &cached.error);
    } catch (const std::exception& exception) {
      cached.valid = false;
      cached.error = exception.what();
    }
  }
  if (!cached.valid) {
    *error = cached.error;
    return false;
  }
  return ValidateSnapshotCapability(cached.settings, error);
}

// Frame/hotkey entry point. Candidate selection is read-only; Request retains
// slider settling, dirty flush, transactional Apply and the existing Undo.
inline bool Cycle(bool custom) {
  if (!loaded || !ResetWritable() || Busy() || save_failed || first_install.pending
      || Resetting() || reset.recovery_failed || pending_action != Action::NONE
      || slot_task != SlotTask::NONE || (interaction_busy && interaction_busy())) return false;
  SettleSlider(true);
  Observe();
  const int begin = (custom ? 3 : 0);
  const int current = (active >= begin && active < begin + 3 ? active - begin : -1);
  bool populated = false;
  std::string error;
  for (int offset = 1; offset <= (custom ? 3 : 1); ++offset) {
    const int index = begin + (current + offset) % 3;
    if (!state.slots[index].populated || (custom && state.slots[index].snapshot.empty())) continue;
    populated = true;
    if (!SlotAvailable(index, &error)) continue;
    Request(Action::SWITCH, index);
    return pending_action == Action::SWITCH
           || notice == "Profile applied. Runtime execution is pending confirmation.";
  }
  notice = (populated ? error : "Empty slot. Save current settings or copy an official profile to create it.");
  return false;
}

inline void RequestReset(int target, bool restore_official = false, bool clear_custom = false) {
  if (first_install.pending) return;
  if (reset.recovery_failed) return;
  if (Resetting() || pending_action != Action::NONE) {
    reset_notice = "Another profile action is pending. Reset was not started.";
    return;
  }
  if (target < 0 || target > 3 || !ResetWritable()) {
    reset_error.clear();
    reset_notice = "Reset unavailable: profile storage or game configuration is read-only.";
    return;
  }
  SettleSlider(true);
  Observe();
  reset = {.phase = ResetPhase::FLUSH, .target = target,
           .restore_official = restore_official, .clear_custom = clear_custom};
  reset_error.clear();
  reset_notice = "Reset pending: saving previous profile changes.";
  if (dirty || Busy()) {
    Schedule(true);
  }
  reset.configuration = ResetConfiguration();
  reset.reload_epoch = profile_reload_epoch.load();
  reset.commit_epoch = profile_commit_epoch.load();
}

inline void RetryResetRecovery() {
  if (!reset.recovery_failed || Resetting() || Busy() || !loaded) return;
  const auto revision = state.revision;
  state = reset.before;
  state.revision = revision;
  reset.recovery_failed = false;
  reset.phase = ResetPhase::ROLLBACK;
  reset_notice = "Recovering previous profiles...";
  reset_error.clear();
  save_failed = false;
  Changed(true);
}

inline void Tick() {
  bool profile_loaded = false;
  {
    std::unique_lock lock(worker.mutex);
    if (worker.ready) {
      worker.ready = false;
      if (!worker.error.empty()) {
        notice = worker.error;
        save_failed = true;
        if (!worker.loading) state.revision = worker.pending.revision;
        if (worker.loading) writable = false;
        pending_action = Action::NONE;
      } else if (worker.loading) {
        state = std::move(worker.result);
        game_id = std::move(worker.id);
        storage_path = std::move(worker.path);
        const auto& display_name = state.slots[edit_index].name;
        std::memcpy(rename, display_name.c_str(), display_name.size() + 1);
        loaded = true;
        profile_loaded = true;
        fresh_storage = worker.fresh;
        observed_snapshot = Snapshot();
        observed_epoch = profile_commit_epoch.load();
        observed_reload_epoch = profile_reload_epoch.load();
        active = -1;
        if (!worker.relocated && observed_snapshot == state.last_applied) {
          for (int i = 0; i < 6; ++i) {
            if (state.selected == data::SLOT_IDS[i] && state.slots[i].populated) active = i;
          }
        }
        if (worker.relocated) notice = "Game location changed. Profiles were copied; current settings were kept.";
        else if (!state.selected.empty() && active < 0) notice = "Game settings changed elsewhere. The last selected profile was not reapplied.";
      } else {
        state.revision = worker.result.revision;
        dirty = serial != worker.saved_serial;
        save_failed = false;
      }
      worker.loading = false;
      worker.wake = presets::internal::Handle();
    }
  }
  if (profile_loaded) MigrateOfficialTemplates();
  if (first_install.pending) {
    if (Busy()) return;
    const auto before = first_install.before;
    const auto revision = state.revision;
    const bool unchanged = profile_commit_epoch.load() == first_install.commit_epoch
                           && profile_reload_epoch.load() == first_install.reload_epoch
                           && ResetConfiguration() == first_install.configuration && ResetWritable();
    std::string capability_error;
    bool supported = false;
    if (!save_failed && !dirty && unchanged) {
      try {
        supported = first_install.supported(first_install.target, &capability_error);
      } catch (const std::exception& exception) {
        capability_error = exception.what();
      }
    }
    if (supported) {
      pending_action = Action::SWITCH;
      pending_index = 1;
      ExecuteAction(false);
      if (active == 1 && Snapshot() == first_install.target) {
        undo.before = before;
        first_install = {};
        return;
      }
    }
    // ApplySnapshot owns the existing batch rollback on failure. No setting has
    // been touched if the durable preparation or concurrent-edit check failed.
    state = before;
    state.revision = revision;
    active = -1;
    first_install = {};
    observed_snapshot = Snapshot();
    observed_epoch = profile_commit_epoch.load();
    notice = "First-install profile was not applied. Current settings were kept.";
    // Clear a prepared disk association after application failure/cancellation.
    // The marker and nonzero revision prevent retry even if this cleanup fails.
    if (!save_failed) {
      Changed(true);
    }
    return;
  }
  if (Resetting()) {
    if (reset.phase != ResetPhase::ROLLBACK && reset.error.empty()
        && (profile_reload_epoch.load() != reset.reload_epoch
            || profile_commit_epoch.load() != reset.commit_epoch
            || ResetConfiguration() != reset.configuration || !ResetWritable())) {
      reset.error = "Game configuration changed during reset. Reset was cancelled.";
    }
    if (Busy()) return;
    const auto finish = [&](bool restored) {
      if (restored) {
        const auto revision = state.revision;
        state = reset.before;
        state.revision = revision;
        active = (Snapshot() == state.last_applied ? reset.previous_active : -1);
        undo = reset.previous_undo;
        dirty = false;
      }
      observed_snapshot = Snapshot();
      observed_epoch = profile_commit_epoch.load();
      observed_reload_epoch = profile_reload_epoch.load();
      reset.phase = ResetPhase::NONE;
    };
    if (reset.phase == ResetPhase::FLUSH) {
      if (save_failed || !reset.error.empty()) {
        reset_notice = (reset.error.empty() ? "Reset failed: previous profile changes could not be saved." : reset.error);
        if (save_failed) {
          reset_error = notice;
        }
        if (!reset.error.empty()) {
          active = -1;
          observed_snapshot = Snapshot();
          observed_epoch = profile_commit_epoch.load();
          observed_reload_epoch = profile_reload_epoch.load();
        }
        reset.phase = ResetPhase::NONE;
        return;
      }
      if (dirty) {
        Schedule(true);
        return;
      }
      reset.before = state;
      reset.previous_active = active;
      reset.previous_undo = undo;
      try {
        reset.document = preset_ui::DefaultsDocument(reset.target);
        reset.detached = state;
        reset.detached.selected.clear();
        reset.detached.last_applied = Snapshot();
        for (const auto& [key, value] : reset.document.settings.items()) {
          if (Managed(key)) {
            reset.detached.last_applied[key] = value;
          }
        }
        if (reset.restore_official) {
          for (int index = 0; index < 3; ++index) {
            auto& slot = reset.detached.slots[index];
            slot.overrides = Json::object();
            slot.snapshot = data::Template(index, defaults_snapshot);
            slot.template_version = data::TEMPLATE_VERSION;
          }
        }
        if (reset.clear_custom) {
          for (int index = 3; index < 6; ++index) reset.detached.slots[index] = {};
        }
      } catch (const std::exception& exception) {
        reset_notice = "Reset failed.";
        reset_error = exception.what();
        reset.phase = ResetPhase::NONE;
        return;
      }
      active = -1;
      undo.available = false;
      state = reset.detached;
      reset.phase = ResetPhase::STORE;
      Changed(true);
      // Schedule owns and verifies this private binding. Its first successful
      // write is transaction metadata, not an external configuration edit.
      if (ReadPresentRestoreRaw("ProfileGameId") == game_id) {
        reset.configuration["ProfileGameId"] = game_id;
      }
      return;
    }
    if (reset.phase == ResetPhase::ROLLBACK) {
      if (save_failed) {
        // Retain both snapshots for recovery; do not let autosave overwrite the
        // proven detached document with an unconfirmed restoration.
        state = reset.detached;
        active = -1;
        dirty = false;
        reset.recovery_failed = true;
        reset_notice = "Profile rollback failed. Detached profiles and recovery snapshot were kept.";
        if (!reset_error.empty()) {
          reset_error += "\n";
        }
        reset_error += notice;
        finish(false);
      } else if (!dirty) {
        reset_notice = "Reset cancelled or failed. Previous profiles restored.";
        finish(true);
      }
      return;
    }
    if (save_failed) {
      reset_notice = "Reset failed: profile storage was not changed.";
      reset_error = notice;
      finish(true);
      return;
    }
    if (dirty) return;
    reset.detached = state;
    if (reset.error.empty()) {
      std::string error;
      if (!preset_ui::Apply(reset.document, true, false, &error, false, nullptr, true)) {
        reset.error = "Reset failed.";
        reset_error = error;
      } else if (Snapshot() != state.last_applied) {
        reset.error = "Reset failed: applied settings did not match the saved reset snapshot.";
      } else {
        reset_notice = (reset.clear_custom ? "Defaults restored." : "Defaults restored; custom profiles were kept.");
        finish(false);
        return;
      }
    }
    if (reset_error.empty()) {
      reset_error = reset.error;
    }
    const auto revision = state.revision;
    state = reset.before;
    state.revision = revision;
    reset.phase = ResetPhase::ROLLBACK;
    save_failed = false;
    Changed(true);
    return;
  }
  if (reset.recovery_failed) return;
  Observe();
  if (pending_action != Action::NONE && !dirty && !Busy()) ExecuteAction();
  if (dirty && writable && !save_failed && !Busy()) Schedule();
}

inline void Initialize() {
  flush_profile_storage = []() {
    SettleSlider(true);
    Observe();
    Schedule(true);
  };
  Tick();
  if (loaded || !writable || Busy()) return;
  defaults_snapshot = Snapshot(true);
  fresh_storage = false;
  const auto saved_id = ReadPresentRestoreRaw("ProfileGameId");
  std::unique_lock lock(worker.mutex);
  worker.id = saved_id.value_or("");
  worker.loading = true;
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(WorkerMain), &module)) {
    writable = false;
    notice = "Could not start profile storage. Current settings were kept.";
    return;
  }
  worker.busy = true;
  if (HANDLE thread = CreateThread(nullptr, 0, WorkerMain, module, 0, nullptr)) CloseHandle(thread);
  else {
    worker.busy = worker.loading = false;
    writable = false;
    FreeLibrary(module);
    notice = "Could not start profile storage. Current settings were kept.";
  }
}

inline std::string Name(int index) {
  constexpr const char* LABELS[] = {"NR Lite", "NR Balanced", "NR Max", "Custom 1", "Custom 2", "Custom 3"};
  if (index < 0 || index >= 6) return ui::Tr("Current game settings");
  return (index >= 3 && state.slots[index].populated && !state.slots[index].name.empty() ? state.slots[index].name : ui::Tr(LABELS[index]));
}

inline void Assign(int index, const Json& settings, bool select = false) {
  if (first_install.pending) return;
  if (Resetting() || reset.recovery_failed) return;
  if (!loaded || !writable || index < 3 || index >= 6) return;
  for (const auto& [key, value] : settings.items()) {
    if (!Validate(key, value)) {
      notice = "Profile contains unsupported settings. Current settings were kept.";
      return;
    }
  }
  Observe();
  const auto before = state;
  const int previous_active = active;
  state.slots[index].snapshot = settings;
  state.slots[index].populated = true;
  if (select) {
    active = index;
    state.selected = std::string(data::SLOT_IDS[index]);
    state.last_applied = Snapshot();
    observed_snapshot = state.last_applied;
  } else if (active == index) {
    active = -1;
    state.selected.clear();
  }
  undo = {.before = before, .after = state, .previous_active = previous_active, .available = true,
          .action_label = "Save current settings to this slot", .action_index = index};
  Changed(true);
  notice = "Custom slot saved. Select it to apply its settings.";
}

inline const char* SaveStatus() {
  if (!writable) return "Profile storage unavailable";
  if (!loaded) return "Loading profiles...";
  if (save_failed) return "Unsaved profile changes";
  if (Busy()) return "Saving...";
  if (dirty) return "Unsaved profile changes";
  return "Saved";
}

inline std::string Summary() {
  std::string summary = Name(active);
  if (active < 0 && loaded && writable && !dirty && !Busy()) return summary;
  const char* status = SaveStatus();
  if (active >= 0 && active < 3 && !dirty && !Busy() && writable && !save_failed) {
    status = (state.slots[active].overrides.empty() ? "Official defaults" : "Modified");
    if (notice == "Official defaults restored." && state.slots[active].overrides.empty()) status = "Official defaults restored.";
  }
  return summary + " · " + ui::Tr(status);
}

inline void DrawSlotMenu(int index) {
  const bool populated = state.slots[index].populated;
  ImGui::BeginDisabled(!loaded || !writable || pending_action != Action::NONE);
  const auto choose = [&](SlotTask task, const char* label, const char* id) {
    if (ui::ActionButton(ui::Tr(label), id)) {
      OpenSlotTask(task, index);
      ImGui::CloseCurrentPopup();
    }
    ui::ItemTooltip("%s", ui::Tr(label));
  };
  if (index < 3) {
    ImGui::BeginDisabled(state.slots[index].overrides.empty());
    choose(SlotTask::RESTORE, "Restore official defaults", "restore_profile");
    ImGui::EndDisabled();
    ui::ItemTooltip("%s", ui::Tr(state.slots[index].overrides.empty()
        ? "This official profile has no personal changes." : "Restore only this official profile. Other profiles are kept."));
  } else {
    ImGui::BeginDisabled(!populated);
    choose(SlotTask::RENAME, "Rename slot", "Rename slot");
    ImGui::EndDisabled();
    ui::ItemTooltip("%s", ui::Tr(populated ? "Change only the slot display name." : "Create this slot first."));
    choose(SlotTask::COPY, "Copy from official profile", "Copy from official profile");
  }
  ImGui::BeginDisabled(!populated || dirty || Busy() || save_failed);
  choose(SlotTask::EXPORT, "Export profile", "Export profile");
  ImGui::EndDisabled();
  ui::ItemTooltip("%s", ui::Tr(!populated ? "Create this slot first."
      : (dirty || Busy() || save_failed) ? SaveStatus() : "Export the actual saved slot settings, including personal changes."));
  if (index >= 3) {
    choose(SlotTask::IMPORT, "Import profile", "Import profile");
    ImGui::BeginDisabled(!populated);
    choose(SlotTask::CLEAR, "Clear slot", "Clear slot");
    ImGui::EndDisabled();
    ui::ItemTooltip("%s", ui::Tr(populated ? "Running game settings and library files are kept." : "This slot is already empty."));
  }
  ImGui::EndDisabled();
}

inline void RenameSlot(int index, const std::string& value) {
  if (first_install.pending) return;
  if (Resetting() || reset.recovery_failed) return;
  if (!loaded || !writable || index < 3 || index >= 6 ||
      !state.slots[index].populated)
    return;
  if (value.empty() || value.size() >= sizeof(rename) ||
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), nullptr, 0) == 0) {
    notice = "Enter a nonempty slot name within the length limit.";
    return;
  }
  const auto before = state;
  state.slots[index].name = value;
  undo = {.before = before,
          .after = state,
          .previous_active = active,
          .available = true,
          .action_label = "Rename slot", .action_index = index};
  Changed(true);
  notice = "Slot renamed.";
}

inline void ClearSlot(int index) {
  if (first_install.pending) return;
  if (Resetting() || reset.recovery_failed) return;
  if (!loaded || !writable || index < 3 || index >= 6 ||
      !state.slots[index].populated)
    return;
  SettleSlider(true);
  Observe();
  const auto before = state;
  const int previous_active = active;
  state.slots[index] = {};
  if (active == index) {
    active = -1;
    state.selected.clear();
    observed_snapshot = Snapshot();
    observed_epoch = profile_commit_epoch.load();
  }
  undo = {.before = before,
          .after = state,
          .previous_active = previous_active,
          .available = true,
          .action_label = "Clear slot", .action_index = index};
  Changed(true);
  notice = "Slot cleared. Running game settings were kept.";
}

inline std::string SlotLabel(int index, bool compact = false) {
  const auto name = Name(index);
  const char* status = nullptr;
  if (state.slots[index].populated &&
      (dirty || Busy() || save_failed || !writable)) {
    status = SaveStatus();
  } else if (index < 3 && !state.slots[index].overrides.empty()) {
    status = "Modified";
  } else if (!state.slots[index].populated) {
    status = "Empty slot";
  }
  if (status == nullptr)
    return name;
  if (compact) {
    if (save_failed)
      return name + " !";
    if (dirty || Busy())
      return name + " ...";
    if (std::string_view(status) == "Modified")
      return name + " *";
  }
  return name + " (" + ui::Tr(status) + ")";
}

inline void DrawSlotContext(int index, const std::string& unavailable) {
  const bool right_click = ImGui::IsItemClicked(ImGuiMouseButton_Right);
  if (right_click) {
    const auto menu = (slot_menu_bounds.size.x > 0.f ? slot_menu_bounds.position : ImGui::GetWindowPos());
    const auto size = (slot_menu_bounds.size.x > 0.f ? slot_menu_bounds.size : ImGui::GetWindowSize());
    slot_anchor = {.minimum = ImGui::GetItemRectMin(),
                   .maximum = ImGui::GetItemRectMax(),
                   .menu_minimum = menu,
                   .menu_maximum = ImVec2(menu.x + size.x, menu.y + size.y),
                   .valid = true};
  }
  if (!unavailable.empty()) {
    ui::ItemTooltip("%s\n%s", ui::Tr("Unavailable in current environment"), unavailable.c_str());
  } else {
    const char* recipes[] = {
      "Official template: 1 Render pass, model B, scale 0.75, Default style.",
      "Official template: 1 Present pass, model B, scale 0.67, Default style.",
      "Official template: Present A at 100% with Cinematic style, then Present B at 100% with Default style; character mask 0.80 / 0.50."};
    ui::ItemTooltip("%s\n%s\n%s%s%s%s%s", SlotLabel(index).c_str(),
                  ui::Tr(state.slots[index].populated
                             ? "Left-click to apply; right-click to manage."
                             : "Left-click to create; right-click to manage."),
                  ui::Tr(index < 3 && !state.slots[index].overrides.empty() &&
                                 !dirty && !Busy() && !save_failed
                             ? "Personal changes have been automatically saved."
                             : SaveStatus()),
                  (index < 3 ? "\n" : ""),
                  (index < 3 ? ui::Tr(recipes[index]) : ""),
                  (index < 3 && data::DEVELOPMENT_CANDIDATE ? "\n" : ""),
                  (index < 3 && data::DEVELOPMENT_CANDIDATE ? ui::Tr("Development candidate") : ""));
  }
  if (right_click) {
    ImGui::OpenPopup("slot_context");
  }
  const auto display = ImGui::GetIO().DisplaySize;
  const float margin = ImGui::GetFontSize();
  const char* official_labels[] = {"Restore official defaults", "Export profile"};
  const char* custom_labels[] = {"Rename slot", "Copy from official profile", "Export profile",
                                 "Import profile", "Clear slot"};
  float content_width = 0.f;
  const auto measure = [&](const char* label) {
    content_width = (std::max)(content_width, ImGui::CalcTextSize(ui::Tr(label)).x);
  };
  if (index < 3) {
    for (const char* label : official_labels) measure(label);
  } else {
    for (const char* label : custom_labels) measure(label);
  }
  const float padding = ImGui::GetStyle().WindowPadding.x * 2.f + ImGui::GetStyle().FramePadding.x * 2.f;
  const float width = (std::max)(1.f, (std::min)(content_width + padding, display.x - margin * 2.f));
  // Wrapped action buttons need an initial content width; auto-fit cannot grow
  // a popup whose labels were already wrapped to its provisional narrow width.
  ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.f),
      ImVec2(width, (std::max)(1.f, display.y - margin * 2.f)));
  if (ImGui::BeginPopup("slot_context")) {
    ImGui::SetWindowPos(SlotWindowPosition(slot_anchor, ImGui::GetWindowSize()));
    DrawSlotMenu(index);
    ImGui::EndPopup();
  }
}

inline bool DrawQuick() {
  Initialize();
  ImGui::PushID("nr_profiles");
  const float font = ImGui::GetFontSize();
  const float gap = ImGui::GetStyle().ItemSpacing.x;
  const float available = ImGui::GetContentRegionAvail().x;
  const float start = ImGui::GetCursorPosX();
  const float manage_width = (std::min)(
      available * 0.40f, ImGui::CalcTextSize(ui::Tr("Manage profiles")).x +
                             ImGui::GetStyle().FramePadding.x * 2.f);
  {
    ui::StyleScope title;
    title.FontSize(ui::ModuleFontSize());
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(
        ui::Ellipsize(ui::Tr("NR Profiles"),
                      (std::max)(1.f, available - manage_width - gap))
            .c_str());
  }
  ImGui::SameLine(start + available - manage_width);
  const auto manage_label = ui::Ellipsize(ui::Tr("Manage profiles"),
      (std::max)(1.f, manage_width - ImGui::GetStyle().FramePadding.x * 2.f)) + "###open_presets";
  const bool manage = ImGui::Button(manage_label.c_str(), ImVec2(manage_width, 0.f));
  ui::ItemTooltip("%s", ui::Tr("Manage profiles"));
  ImGui::BeginDisabled(!loaded || !writable || pending_action != Action::NONE);
  const char* labels[] = {"NR Lite", "NR Balanced", "NR Max"};
  int selected = -1;
  bool blocked[6]{};
  std::string unavailable[6];
  for (int i = 0; i < 6; ++i) {
    blocked[i] = loaded && state.slots[i].populated && !SlotDisplayAvailable(i, &unavailable[i]);
  }
  if (available < font * 18.f) {
    ImGui::SetNextItemWidth(-1.f);
    const std::string preview = (active >= 0 && active < 3 ? SlotLabel(active, true) : ui::Tr("Official profiles"));
    if (ImGui::BeginCombo("##official_profiles",
                          ui::Ellipsize(preview.c_str(), available - font * 3.f).c_str())) {
      for (int i = 0; i < 3; ++i) {
        ImGui::PushID(i);
        ImGui::BeginDisabled(blocked[i]);
        if (ImGui::Selectable(SlotLabel(i).c_str(), active == i)) {
          selected = i;
        }
        ImGui::EndDisabled();
        DrawSlotContext(i, unavailable[i]);
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
  } else {
    ImGui::PushID("##official_profiles");
    const float width = (available - gap * 2.f) / 3.f;
    for (int i = 0; i < 3; ++i) {
      if (i != 0) {
        ImGui::SameLine();
      }
      const auto label = SlotLabel(i, available < font * 35.f);
      ImGui::BeginDisabled(blocked[i]);
      if (ui::ProfileSlotButton(ui::Tr(labels[i]), label.c_str(),
                                ImVec2(width, 0.f), active == i)) {
        selected = i;
      }
      ImGui::EndDisabled();
      // Each official context popup belongs to its stable segment identity.
      ImGui::PushID(labels[i]);
      DrawSlotContext(i, unavailable[i]);
      ImGui::PopID();
    }
    ImGui::PopID();
  }
  if (selected >= 0) {
    Request(Action::SWITCH, selected);
  }
  const char* short_names[] = {"Custom 1", "Custom 2", "Custom 3"};
  int chosen_custom = -1;
  if (available < font * 18.f) {
    const std::string preview = (active >= 3 ? SlotLabel(active, true) : ui::Tr("My profiles"));
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::BeginCombo(
            "##custom_profiles",
            ui::Ellipsize(preview.c_str(), available - font * 3.f).c_str())) {
      for (int i = 3; i < 6; ++i) {
        ImGui::PushID(i);
        const auto name = Name(i);
        const std::string label =
            std::string(ui::Tr(short_names[i - 3])) + ": " + name +
            (state.slots[i].populated
                 ? ""
                 : std::string(" · ") + ui::Tr("Empty slot"));
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::BeginDisabled(blocked[i]);
        if (ImGui::Selectable("##custom_profile_choice", active == i, 0,
                              ImVec2(0.f, ImGui::GetTextLineHeight()))) {
          chosen_custom = i;
        }
        ImGui::GetWindowDrawList()->AddText(
            at, ImGui::GetColorU32(ImGuiCol_Text),
            ui::Ellipsize(label.c_str(), ImGui::GetContentRegionAvail().x)
                .c_str());
        ImGui::EndDisabled();
        DrawSlotContext(i, unavailable[i]);
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
  } else {
    const float width = (available - gap * 2.f) / 3.f;
    for (int i = 3; i < 6; ++i) {
      if (i != 3) {
        ImGui::SameLine();
      }
      ImGui::PushID(i);
      const auto label = SlotLabel(i, available < font * 35.f);
      ImGui::BeginDisabled(blocked[i]);
      if (ui::ProfileSlotButton("##custom_slot", label.c_str(),
                                ImVec2(width, 0.f), active == i)) {
        chosen_custom = i;
      }
      ImGui::EndDisabled();
      DrawSlotContext(i, unavailable[i]);
      ImGui::PopID();
    }
  }
  static int creating = -1;
  if (chosen_custom >= 3) {
    if (state.slots[chosen_custom].populated) {
      Request(Action::SWITCH, chosen_custom);
    } else {
      creating = edit_index = chosen_custom;
      ImGui::OpenPopup("create_custom_slot");
    }
  }
  ImGui::EndDisabled();
  const auto popup_width = [&]() {
    const float width = (std::max)(
        1.f, (std::min)(font * 27.f, ImGui::GetIO().DisplaySize.x -
                                         ui::tokens::kCardPadding * 2.f));
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(width, 0.f), ImVec2(width, ImGui::GetIO().DisplaySize.y * 0.9f));
  };
  popup_width();
  if (ImGui::BeginPopup("create_custom_slot")) {
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextWrapped("%s", Name(creating).c_str());
    const bool empty =
        creating >= 3 && creating < 6 && !state.slots[creating].populated;
    ImGui::BeginDisabled(!empty || !loaded || !writable ||
                         pending_action != Action::NONE);
    if (ui::ActionButton(ui::Tr("Save current settings to this slot"),
                         "create_slot_current")) {
      SettleSlider(true);
      Assign(creating, Snapshot(), true);
      ImGui::CloseCurrentPopup();
    }
    if (ImGui::BeginMenu(ui::Tr("Copy from official profile"))) {
      ImGui::TextWrapped("%s", ui::Tr("Copies the original official template, "
                                      "without personal changes."));
      for (int i = 0; i < 3; ++i) {
        if (ImGui::MenuItem(ui::Tr(labels[i]))) {
          try {
            Assign(creating, data::Template(i, defaults_snapshot));
            if (!save_failed && state.slots[creating].populated) {
              Request(Action::SWITCH, creating);
            }
          } catch (const std::exception& exception) {
            notice = exception.what();
          }
          ImGui::CloseCurrentPopup();
        }
      }
      ImGui::EndMenu();
    }
    ImGui::EndDisabled();
    if (!empty) {
      ImGui::TextWrapped(
          "%s",
          ui::Tr(
              "This slot is no longer empty. Review it in Manage profiles."));
    }
    if (ui::ActionButton(ui::Tr("Cancel"), "cancel_create_slot")) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::PopTextWrapPos();
    ImGui::EndPopup();
  }
  if (active < 0 && loaded) {
    ui::SameLineIfFits(ImGui::CalcTextSize(ui::Tr("Current game settings")).x);
    ImGui::TextDisabled("%s", ui::Tr("Current game settings"));
  }
  const bool restore = slot_task == SlotTask::RESTORE;
  if (restore) {
    slot_task = SlotTask::NONE;
  }
  static int restoring = -1;
  if (restore) {
    restoring = task_index;
    ImGui::OpenPopup("###restore_profile_confirm");
  }
  popup_width();
  bool restore_open = true;
  if (ImGui::BeginPopupModal(
          (std::string(ui::Tr("Restore official defaults?")) +
           "###restore_profile_confirm")
              .c_str(),
          &restore_open,
          ImGuiWindowFlags_AlwaysAutoResize |
              ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextWrapped("%s",
                       ui::Tr("Personal changes to this official profile will "
                              "be cleared. Other profiles are kept."));
    if (ui::ActionButton(ui::Tr("Cancel"), "cancel_restore") ||
        ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      ImGui::CloseCurrentPopup();
    }
    ui::SameLineIfFits(ImGui::CalcTextSize(ui::Tr("Restore")).x +
                       ImGui::GetStyle().FramePadding.x * 2.f);
    if (ui::ActionButton(ui::Tr("Restore"), "confirm_restore")) {
      Request(Action::RESTORE, restoring);
      ImGui::CloseCurrentPopup();
    }
    ImGui::PopTextWrapPos();
    ImGui::EndPopup();
  }
  const bool important =
      !notice.empty() &&
      notice != "Profile applied. Runtime execution is pending confirmation." &&
      notice != "Slot renamed." && notice != "Official defaults restored." &&
      notice != "Custom slot saved. Select it to apply its settings." &&
      notice != "Profile action undone. Later edits were kept." &&
      notice != "Slot cleared. Running game settings were kept.";
  bool fallback = false;
  if (active >= 0) {
    RuntimeLock lock(runtime_mutex);
    if (!direct_runtime_requirements.dual_models) {
      const auto& saved = state.slots[active].snapshot;
      for (const auto key :
           {"NRPreset", "NRPass2Preset", "NRPass3Preset", "NRPass4Preset"}) {
        fallback |= saved.contains(key) && saved[key] == 2;
      }
    }
  }
  if (fallback) {
    const char* message =
        ui::Tr("Unsupported model requests are preserved; the runtime uses the "
               "existing fallback policy.");
    ImGui::TextUnformatted(ui::Ellipsize(message, available).c_str());
    ui::ItemTooltip("%s", message);
  }
  if (important) {
    ui::StyleScope warning;
    warning.Color(ImGuiCol_Text, ui::CurrentTheme().warn);
    ImGui::TextUnformatted(
        ui::Ellipsize(ui::Tr(notice.c_str()), available).c_str());
    ui::ItemTooltip("%s", ui::Tr(notice.c_str()));
  }
  if (dirty && !Busy() &&
      ui::ActionButton(ui::Tr("Retry saving profiles"), "retry_profiles")) {
    Schedule(true);
  }
  ImGui::PopID();
  return manage;
}

inline void DrawCustomTools() {
  Initialize();
  ImGui::BeginDisabled(!loaded || !writable || pending_action != Action::NONE);
  const char* targets[] = {"Custom 1", "Custom 2", "Custom 3"};
  ImGui::SetNextItemWidth(-1.f);
  const int chosen = ui::Segmented("##profile_target", targets, 3, edit_index - 3, nullptr);
  if (chosen >= 0) {
    edit_index = chosen + 3;
    confirm_replace = false;
    const auto& value = state.slots[edit_index].name;
    std::memcpy(rename, value.c_str(), (std::min)(sizeof(rename), value.size() + 1));
  }
  ImGui::TextWrapped("%s: %s", ui::Tr("Custom slot"), Name(edit_index).c_str());
  if (!state.slots[edit_index].populated) ImGui::TextWrapped("%s", ui::Tr("Empty slot"));
  ImGui::SetNextItemWidth(-1.f);
  ImGui::PushID(edit_index);
  ui::text_input::DrawLabeled(ui::Tr("Slot display name"), rename, sizeof(rename));
  ImGui::PopID();
  if (ui::ActionButton(ui::Tr("Rename slot"), "rename_slot")) {
    RenameSlot(edit_index, rename);
  }
  ui::Checkbox(ui::Tr("Confirm replacing or clearing this slot"), &confirm_replace);
  ImGui::BeginDisabled(state.slots[edit_index].populated && !confirm_replace);
  if (ui::ActionButton(ui::Tr("Save current game settings to slot"), "save_current_slot")) {
    SettleSlider(true);
    Assign(edit_index, Snapshot(), true);
  }
  const char* sources[] = {"NR Lite", "NR Balanced", "NR Max"};
  ImGui::SetNextItemWidth(-1.f);
  const int source = ui::Segmented("##copy_official", sources, 3, copy_official, nullptr);
  if (source >= 0) copy_official = source;
  if (ui::ActionButton(ui::Tr("Copy official profile to slot"), "copy_official_slot")) {
    try {
      Assign(edit_index, data::Template(copy_official, defaults_snapshot));
    } catch (const std::exception& exception) {
      notice = exception.what();
    }
  }
  if (ui::ActionButton(ui::Tr("Clear slot"), "clear_slot")) {
    ClearSlot(edit_index);
  }
  ImGui::EndDisabled();
  ImGui::BeginDisabled(!state.slots[edit_index].populated);
  if (ui::ActionButton(ui::Tr("Save or share this slot"), "share_slot")) {
    share_request = presets::Document{.metadata = {{"name", Name(edit_index)}},
                                      .scope = presets::Scope::QUALITY_MULTIPASS,
                                      .settings = state.slots[edit_index].snapshot};
  }
  ImGui::EndDisabled();
  ImGui::EndDisabled();
  ImGui::TextWrapped("%s", ui::Tr(SaveStatus()));
  if (!notice.empty()) ImGui::TextWrapped("%s", ui::Tr(notice.c_str()));
}
}  // namespace profile_ui
