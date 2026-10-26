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
                             || rule->category == presets::FieldRule::MULTIPASS);
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
          }
        }
      }
      std::unique_lock lock(worker.mutex);
      worker.result = std::move(result);
      worker.id = std::move(id);
      worker.path = std::move(path);
      worker.relocated = relocated;
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
  if (!loaded || !writable || !dirty) return;
  {
    std::shared_lock lock(present_restore_configuration.mutex);
    if (!present_restore_configuration.global_writable) {
      notice = "Newer or invalid game configuration is read-only.";
      save_failed = true;
      return;
    }
  }
  // Persist the private identity only on an explicit profile edit/selection.
  reshade::set_config_value(nullptr, kConfigSection, "ProfileGameId", game_id.c_str());
  if (ReadPresentRestoreRaw("ProfileGameId") != game_id) {
    notice = "Could not save the game profile identity. Changes remain unsaved.";
    save_failed = true;
    return;
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

inline bool ApplySnapshot(const Json& settings, std::string* error) {
  for (const auto& [key, value] : settings.items()) {
    if (!Validate(key, value)) {
      *error = "Profile contains unsupported settings. Current settings were kept.";
      return false;
    }
  }
  const presets::Document document{.metadata = Json::object(), .scope = presets::Scope::FULL, .settings = settings};
  for (const auto& row : preset_ui::Preview(document, false)) {
    if (settings.contains(row.key) && std::string_view(row.status) == "Unsupported") {
      *error = "Profile contains unsupported settings. Current settings were kept.";
      return false;
    }
  }
  return preset_ui::Apply(document, false, false, error);
}

inline void ExecuteAction() {
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
          .available = true, .configuration = true};
  Changed(true);
  notice = "Profile applied. Runtime execution is pending confirmation.";
}

inline void Request(Action action, int index = -1) {
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

inline void Tick() {
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
  return (index >= 3 && !state.slots[index].name.empty() ? state.slots[index].name : ui::Tr(LABELS[index]));
}

inline void Assign(int index, const Json& settings, bool select = false) {
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
  undo = {.before = before, .after = state, .previous_active = previous_active, .available = true};
  Changed(true);
  notice = "Custom slot saved. Select it to apply its settings.";
}

inline void DrawQuick() {
  Initialize();
  ImGui::PushID("nr_profiles");
  ImGui::TextUnformatted(ui::Tr("NR Profiles"));
  ImGui::TextWrapped("%s · %s", ui::Tr("Official profiles"), ui::Tr("Development candidate"));
  ImGui::BeginDisabled(!loaded || !writable || pending_action != Action::NONE);
  const char* labels[] = {"NR Lite", "NR Balanced", "NR Max"};
  ImGui::SetNextItemWidth(-1.f);
  const int selected = ui::Segmented("##official_profiles", labels, 3, active < 3 ? active : -1, nullptr);
  if (selected >= 0) Request(Action::SWITCH, selected);
  ImGui::TextWrapped("%s", ui::Tr("My profiles"));
  // IDs use fixed slot indices. Display names are literal UTF-8 and never
  // participate in ImGui's ##/### label parsing or translation lookup.
  const float width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.f) / 3.f;
  for (int i = 3; i < 6; ++i) {
    if (i != 3) ImGui::SameLine();
    ImGui::PushID(i);
    ui::StyleScope style;
    if (active == i) {
      style.Color(ImGuiCol_Button, ui::CurrentTheme().accent)
          .Color(ImGuiCol_Text, ui::CurrentTheme().on_accent);
    }
    const auto name = Name(i);
    const auto label = name + (state.slots[i].populated ? "" : std::string(" · ") + ui::Tr("Empty slot"));
    const auto clipped = ui::Ellipsize(label.c_str(), (std::max)(1.f, width - ImGui::GetStyle().FramePadding.x * 2.f));
    const ImVec2 position = ImGui::GetCursorScreenPos();
    if (ImGui::Button("##custom_slot", ImVec2(width, 0.f))) Request(Action::SWITCH, i);
    ImGui::GetWindowDrawList()->AddText(ImVec2(position.x + ImGui::GetStyle().FramePadding.x,
                                              position.y + ImGui::GetStyle().FramePadding.y),
                                       ImGui::GetColorU32(ImGuiCol_Text), clipped.c_str());
    ui::ItemTooltip("%s", label.c_str());
    ImGui::PopID();
  }
  ImGui::EndDisabled();
  const auto name = Name(active);
  const char* status = (!writable ? "Profile storage unavailable" : !loaded ? "Loading profiles..." : dirty ? "Unsaved profile changes" : Busy() ? "Saving..." : "Saved");
  ImGui::TextWrapped("%s · %s", name.c_str(), ui::Tr(status));
  if (active >= 0 && active < 3) {
    ImGui::TextWrapped("%s · %s", ui::Tr("Development candidate"),
                        ui::Tr(state.slots[active].overrides.empty() ? "Official defaults" : "Modified"));
  }
  if (active >= 0) {
    bool fallback = false;
    {
      RuntimeLock lock(runtime_mutex);
      if (!direct_runtime_requirements.dual_models) {
        const auto& saved = state.slots[active].snapshot;
        for (const auto key : {"NRPreset", "NRPass2Preset", "NRPass3Preset", "NRPass4Preset"}) {
          fallback |= saved.contains(key) && saved[key] == 2;
        }
      }
    }
    if (fallback) ImGui::TextWrapped("%s", ui::Tr("Unsupported model requests are preserved; the runtime uses the existing fallback policy."));
  }
  if (!notice.empty()) ImGui::TextWrapped("%s", ui::Tr(notice.c_str()));
  if (dirty && !Busy() && ui::ActionButton(ui::Tr("Retry saving profiles"), "retry_profiles")) Schedule(true);
  ImGui::BeginDisabled(!undo.available || !loaded || !writable);
  if (ui::ActionButton(ui::Tr("Undo profile action"), "undo_profile")) Request(Action::UNDO);
  ImGui::EndDisabled();
  if (active >= 0 && active < 3) {
    ui::SameLineIfFits(ImGui::CalcTextSize(ui::Tr("Restore official defaults")).x + ImGui::GetStyle().FramePadding.x * 2.f);
    if (ui::ActionButton(ui::Tr("Restore official defaults"), "restore_profile")) ImGui::OpenPopup("restore_profile_confirm");
  }
  if (ImGui::BeginPopup("restore_profile_confirm")) {
    ImGui::TextWrapped("%s", ui::Tr("Clear only this official profile's overrides? This action can be undone."));
    if (ui::ActionButton(ui::Tr("Restore official defaults"), "confirm_restore")) {
      Request(Action::RESTORE, active);
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  ImGui::PopID();
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
  ImGui::InputText(ui::ControlLabel(ui::Tr("Slot display name")).c_str(), rename, sizeof(rename));
  if (ui::ActionButton(ui::Tr("Rename slot"), "rename_slot")) {
    const auto before = state;
    state.slots[edit_index].name = rename;
    undo = {.before = before, .after = state, .previous_active = active, .available = true};
    Changed(true);
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
      Assign(edit_index, data::Resolve(copy_official, state.slots[copy_official], defaults_snapshot, Validate));
    } catch (const std::exception& exception) {
      notice = exception.what();
    }
  }
  if (ui::ActionButton(ui::Tr("Clear slot"), "clear_slot")) {
    Observe();
    const auto before = state;
    const int previous_active = active;
    state.slots[edit_index] = {};
    if (active == edit_index) {
      active = -1;
      state.selected.clear();
    }
    undo = {.before = before, .after = state, .previous_active = previous_active, .available = true};
    Changed(true);
    notice = "Slot cleared. Running game settings were kept.";
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
  ImGui::TextWrapped("%s", ui::Tr(dirty ? "Unsaved profile changes" : Busy() ? "Saving..." : "Saved"));
  if (!notice.empty()) ImGui::TextWrapped("%s", ui::Tr(notice.c_str()));
}
}  // namespace profile_ui
