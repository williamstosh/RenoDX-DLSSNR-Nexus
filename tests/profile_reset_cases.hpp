/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Included inside RunProfileRuntimeCases; all files and host configuration are
// synthetic. Exercise the production worker, atomic store and Apply readback.
{
  namespace adapter = runtime::preset_ui;
  namespace presets = renodx::addons::dlss5::presets;
  const auto full_defaults = adapter::DefaultsDocument(3);
  Json page_keys = Json::object();
  for (int target = 0; target < 4; ++target) {
    const auto document = adapter::DefaultsDocument(target);
    auto& keys = page_keys[std::to_string(target)];
    keys = Json::array();
    for (const auto& [key, value] : document.settings.items()) keys.push_back(key);
  }
  {
    std::ofstream file(root / "page-defaults.json");
    file << page_keys.dump(2) << '\n';
    if (!file) throw std::runtime_error("Cannot write page-defaults.json.");
  }
  auto original = full_defaults;
  for (auto& [key, value] : original.settings.items()) value = adapter::ReadSetting(key);
  Check(adapter::Apply({.scope = presets::Scope::FULL,
                        .settings = {{"NRMonitorKey", 0}, {"NRMonitorModeKey", 0}}}, true, true, &error),
        "Unbound monitor shortcuts are valid in a full profile.");
  Check(adapter::Capture(presets::Scope::FULL, Json::object()).settings["NRMonitorKey"] == 0
            && adapter::Capture(presets::Scope::FULL, Json::object()).settings["NRMonitorModeKey"] == 0
            && adapter::Undo(&error), "Full profile capture and undo retain an unbound shortcut.");
  Check(adapter::Apply({.scope = presets::Scope::FULL,
                        .settings = {{"UiHudAnchor", 8}, {"UiHudOffsetX", -12.5f}, {"UiHudOffsetY", -7.25f}}},
                       true, false, &error), "Apply signed fractional notice offsets through the preset adapter.");
  Check(runtime::GetHudPosition().anchor == 8 && runtime::GetHudPosition().offset.x == -12.5f
            && runtime::GetHudPosition().offset.y == -7.25f, "Preset apply updates independent notice placement.");
  Check(adapter::Apply({.scope = presets::Scope::FULL, .settings = {{"UiHudAnchor", 8}, {"UiHudX", 41.f}}},
                       true, true, &error), "A new-format profile may keep its existing anchor.");
  Check(adapter::Undo(&error) && runtime::GetHudPosition().anchor == 8
            && runtime::GetHudPosition().offset.x == -12.5f,
        "Undo deltas never masquerade as old percentage-only profiles.");
  Check(adapter::Apply({.scope = presets::Scope::FULL, .settings = {{"UiHudX", 33.3f}, {"UiHudY", 73.f}}},
                       true, true, &error), "Old percentage profiles remain applicable with a current grid anchor.");
  Check(runtime::GetHudPosition().anchor == -1 && runtime::GetHudPosition().offset.x == 0.f
            && std::abs(runtime::GetHudPosition().legacy_percent.x - 33.3f) < .001f,
        "Legacy profiles restore their exact percentage placement rather than a new anchor.");
  Check(adapter::Undo(&error) && runtime::GetHudPosition().anchor == 8
            && runtime::GetHudPosition().offset.x == -12.5f, "Undo restores grid placement and fractional offsets.");
  Check(adapter::DefaultsDocument(2).settings["UiHudAnchor"] == -1
            && full_defaults.settings["UiHudOffsetX"] == 0.f
            && full_defaults.settings["UiHudOffsetY"] == 0.f,
        "Advanced and global defaults own the new placement fields.");
  Check(adapter::Apply(original, true, false, &error, false, nullptr, true),
        "Restore placement before the existing reset matrix.");
  const auto config_values = [&]() {
    Json result = Json::object();
    for (const auto& rule : presets::FieldRules()) result[rule.key] = adapter::ReadSetting(rule.key);
    return result;
  };
  const auto slot_bytes = [](const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  };
  const auto same_content = [&](data::State left, data::State right) {
    left.revision = right.revision = 0;
    return encoded(left) == encoded(right);
  };
  const auto wait_worker = [&]() {
    const auto deadline = GetTickCount64() + 10000;
    for (;;) {
      {
        std::shared_lock lock(profile::worker.mutex);
        if (!profile::worker.busy) return;
      }
      if (GetTickCount64() >= deadline) throw std::runtime_error("Reset worker timed out.");
      Sleep(1);
    }
  };
  const auto prepare = [&](int selected) {
    seed();
    profile::reset_notice.clear();
    Check(adapter::Apply({.scope = presets::Scope::FULL,
                          .settings = {{"NRIntensity", 0.375f}, {"NRPass4Intensity", 0.625f}, {"UiHudX", 25.f}}},
                         true, false, &error), "Seed distinct settings across reset pages.");
    for (int index = 0; index < 3; ++index) {
      profile::state.slots[index].snapshot = data::Template(index, profile::defaults_snapshot);
      profile::state.slots[index].overrides = {{"NRIntensity", 0.375f}};
      profile::state.slots[index].snapshot.update(profile::state.slots[index].overrides);
    }
    for (int index = 3; index < 6; ++index) {
      profile::Assign(index, profile::Snapshot());
      settle();
      profile::RenameSlot(index, "Synthetic custom " + std::to_string(index));
      settle();
    }
    select(selected);
    profile::reset_notice.clear();
  };
  const auto verify_reset = [&](int target, const data::State& before, const Json& running,
                                bool restore_official = false, bool clear_custom = false) {
    const auto document = adapter::DefaultsDocument(target);
    const auto current = config_values();
    for (const auto& [key, value] : running.items()) {
      const auto expected = (document.settings.contains(key) ? document.settings[key] : value);
      Check(current[key] == expected, "Reset applies exactly the page-owned defaults and keeps every other field.");
      if (!document.settings.contains(key)) continue;
      bool matches = false;
      if (expected.is_boolean()) {
        bool actual = !expected.get<bool>();
        matches = reshade::get_config_value(nullptr, runtime::kConfigSection, key.c_str(), actual)
                  && actual == expected.get<bool>();
      } else if (expected.is_string()) {
        matches = runtime::ReadPresentRestoreRaw(key.c_str()) == expected.get<std::string>();
      } else {
        float actual = NAN;
        matches = reshade::get_config_value(nullptr, runtime::kConfigSection, key.c_str(), actual)
                  && std::abs(actual - expected.get<float>()) < 0.000001f;
      }
      // Apply may leave an unchanged absent key absent; its fallback is already
      // verified above. Explicit writes must agree with runtime values.
      Check(matches || (!runtime::ReadPresentRestoreRaw(key.c_str()) && value == expected),
            "Reset INI values agree with runtime or an unchanged registered fallback.");
    }
    Check(profile::active == -1 && profile::state.selected.empty(), "Reset detaches every selected Slot.");
    Check(profile::state.last_applied == profile::Snapshot(), "Saved reset marker matches managed runtime settings.");
    for (int index = 0; index < 6; ++index) {
      auto expected = before;
      if (restore_official && index < 3) {
        expected.slots[index].overrides = Json::object();
        expected.slots[index].snapshot = data::Template(index, profile::defaults_snapshot);
        expected.slots[index].template_version = data::TEMPLATE_VERSION;
      }
      if (clear_custom && index >= 3) expected.slots[index] = {};
      Check(encoded(profile::state)["slots"][std::string(data::SLOT_IDS[index])]
                == encoded(expected)["slots"][std::string(data::SLOT_IDS[index])],
            "Page reset preserves all Slots; full reset changes only official template data.");
    }
    Check(encoded(disk()) == encoded(profile::state), "Reset memory and actual Slot file are identical.");
    Check(!profile::Resetting() && !profile::dirty && !profile::save_failed
              && profile::reset_notice.find("Defaults restored") != std::string::npos,
          "Success appears only after both Slot storage and configuration application succeed.");
  };

  for (int target = 0; target < 3; ++target) {
    const auto document = adapter::DefaultsDocument(target);
    Check(document.scope == presets::Scope::FULL && !document.settings.empty(), "Each page has a full-scope default document.");
    for (const auto& [key, value] : document.settings.items()) {
      Check(full_defaults.settings.contains(key) && full_defaults.settings[key] == value,
            "Page defaults are a subset of registered full defaults.");
      const auto* rule = presets::FindRule(key);
      Check(rule != nullptr && (target == 0 ? (rule->category == presets::FieldRule::QUALITY || key == "NeuralUplift")
                                  : target == 1 ? rule->category == presets::FieldRule::MULTIPASS
                                                : rule->category != presets::FieldRule::QUALITY
                                                      && rule->category != presets::FieldRule::MULTIPASS && key != "NeuralUplift"),
            "Page ownership follows the registry category contract.");
    }
    for (int selected = 0; selected < 6; ++selected) {
      prepare(selected);
      const auto before = profile::state;
      const auto running = config_values();
      profile::RequestReset(target);
      Check(profile::Resetting() && config_values() == running, "RequestReset is nonblocking and does not apply configuration.");
      profile::Request(profile::Action::SWITCH, (selected + 1) % 6);
      Check(profile::pending_action == profile::Action::NONE, "Profile actions cannot enter a pending reset transaction.");
      settle();
      verify_reset(target, before, running);
    }
  }

  Check(adapter::DefaultsDocument(0).settings.contains("NeuralUplift")
            && !adapter::DefaultsDocument(2).settings.contains("NeuralUplift"),
        "The Quality NR master switch is not owned by Advanced despite the legacy General category.");

  seed();
  Check(WritePrivateProfileStringA(runtime::kConfigSection, "ProfileGameId", nullptr, ini_path.c_str()) != FALSE,
        "Seed first reset without a persisted profile identity.");
  WritePrivateProfileStringA(nullptr, nullptr, nullptr, ini_path.c_str());
  const auto first_state = profile::state;
  const auto first_running = config_values();
  profile::RequestReset(0);
  settle();
  verify_reset(0, first_state, first_running);
  Check(runtime::ReadPresentRestoreRaw("ProfileGameId") == profile::game_id,
        "First reset accepts its own verified identity write without detecting an external edit.");

  // Full reset also preserves unrelated game and library files byte-for-byte.
  prepare(5);
  const auto other_path = root / "other-game.json";
  auto other = data::EmptyState(std::string(64, 'b'));
  Check(data::Write(other_path, &other, 0, profile::Validate, &error), "Seed independent other-game file.");
  const auto library_path = root / "library.json";
  { std::ofstream file(library_path); file << "synthetic library sentinel"; }
  const auto other_bytes = slot_bytes(other_path);
  const auto library_bytes = slot_bytes(library_path);
  const auto before_full = profile::state;
  const auto running_full = config_values();
  profile::RequestReset(3);
  profile::Tick();
  wait_worker();
  Check(profile::Resetting() && config_values() == running_full, "Configuration is unchanged until detached manifest save is consumed.");
  Check(disk().selected.empty(), "Detached manifest is atomically published before Apply.");
  settle();
  verify_reset(3, before_full, running_full);

  // The two optional profile actions are independent and share the same
  // protected reset transaction. Exercise all four checkbox combinations.
  for (const bool restore_official : {false, true}) {
    for (const bool clear_custom : {false, true}) {
      prepare(0);
      const auto before = profile::state;
      const auto running = config_values();
      profile::RequestReset(3, restore_official, clear_custom);
      settle();
      verify_reset(3, before, running, restore_official, clear_custom);
    }
  }
  Check(slot_bytes(other_path) == other_bytes && slot_bytes(library_path) == library_bytes,
        "Reset never writes another game or shared preset library.");

  prepare(0);
  commit(runtime::kSetIntensity, 0.5625f);
  profile::RequestReset(0);
  settle();
  Check(profile::state.slots[0].overrides["NRIntensity"] == 0.5625f
            && disk().slots[0].overrides["NRIntensity"] == 0.5625f,
        "Reset flushes pending old edits before detaching and never autosaves reset values into the Slot.");

  prepare(0);
  commit(runtime::kSetIntensity, 0.6875f);
  profile::Request(profile::Action::SWITCH, 1);
  const auto pending = profile::pending_action;
  profile::RequestReset(3);
  Check(!profile::Resetting() && profile::pending_action == pending,
        "Reset cannot replace an existing pending profile action.");
  settle();
  Check(profile::active == 1, "Rejected reset leaves the earlier pending action intact.");

  prepare(0);
  auto flush_lock = profile::storage_path;
  flush_lock += L".lock";
  const auto flush_bytes = slot_bytes(profile::storage_path);
  {
    presets::internal::Handle held(CreateFileW(flush_lock.c_str(), GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, 0, nullptr));
    if (held.value == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot lock pending flush destination.");
    commit(runtime::kSetIntensity, 0.8125f);
    const auto flush_state = encoded(profile::state);
    const auto flush_ini = Bytes();
    profile::RequestReset(3);
    settle();
    Check(profile::active == 0 && encoded(profile::state) == flush_state && profile::dirty
              && Bytes() == flush_ini && slot_bytes(profile::storage_path) == flush_bytes
              && profile::reset_notice.find("previous profile changes") != std::string::npos,
          "Failed old-edit flush refuses reset and retains unsaved edits without changing the stored Slot.");
  }
  profile::Schedule(true);
  settle();

  // The atomic writer must leave the original destination unchanged on failure.
  prepare(2);
  const auto before_store = profile::state;
  const auto store_ini = Bytes();
  const auto store_running = config_values();
  const auto store_bytes = slot_bytes(profile::storage_path);
  auto lock_path = profile::storage_path;
  lock_path += L".lock";
  {
    presets::internal::Handle held(CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, 0, nullptr));
    if (held.value == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot lock reset Slot file.");
    profile::RequestReset(3);
    settle();
    Check(encoded(profile::state) == encoded(before_store) && profile::active == 2
              && config_values() == store_running && Bytes() == store_ini
              && slot_bytes(profile::storage_path) == store_bytes,
          "Initial Slot write failure restores association and leaves memory, INI and manifest unchanged.");
    Check(profile::reset_notice.find("Reset failed") != std::string::npos, "Slot failure has an independent visible reset error.");
  }

  prepare(3);
  const auto before_apply = profile::state;
  const auto apply_running = config_values();
  const auto apply_ini = Bytes();
  failed_key = "NRIntensity";
  profile::RequestReset(0);
  settle();
  failed_key.clear();
  Check(same_content(profile::state, before_apply) && profile::active == 3
            && config_values() == apply_running && Bytes() == apply_ini
            && encoded(profile::state) == encoded(disk())
            && profile::state.revision == before_apply.revision + 2,
        "Apply failure rolls back INI/runtime and asynchronously restores Slots using the latest revision.");
  Check(profile::reset_notice.find("Previous profiles restored.") != std::string::npos,
        "Successful rollback reports reset failure, never success.");

  for (int protection = 0; protection < 5; ++protection) {
    prepare(1);
    const auto before = encoded(profile::state);
    const auto before_ini = Bytes();
    const auto before_bytes = slot_bytes(profile::storage_path);
    if (protection == 0) profile::writable = false;
    if (protection == 1) { std::unique_lock lock(runtime::present_restore_configuration.mutex); runtime::present_restore_configuration.global_writable = false; }
    if (protection == 2) { std::unique_lock lock(runtime::present_restore_configuration.mutex); runtime::present_restore_configuration.scope.writable = false; }
    if (protection == 3) runtime::present_sdr_restore_config_writable = false;
    if (protection == 4) { std::unique_lock lock(runtime::hdr_pedestal_policy_state.mutex); runtime::hdr_pedestal_policy_state.configuration.writable = false; }
    profile::RequestReset(3);
    Check(!profile::Resetting() && encoded(profile::state) == before && Bytes() == before_ini
              && slot_bytes(profile::storage_path) == before_bytes
              && profile::reset_notice.find("read-only") != std::string::npos,
          "Read-only profile/global/current-policy protection refuses reset before any write.");
    profile::writable = true;
    { std::unique_lock lock(runtime::present_restore_configuration.mutex); runtime::present_restore_configuration.global_writable = true; runtime::present_restore_configuration.scope.writable = true; }
    runtime::present_sdr_restore_config_writable = true;
    { std::unique_lock lock(runtime::hdr_pedestal_policy_state.mutex); runtime::hdr_pedestal_policy_state.configuration.writable = true; }
  }

  for (int change = 0; change < 5; ++change) {
    prepare(4);
    const auto before = profile::state;
    profile::RequestReset(0);
    profile::Tick();
    wait_worker();
    if (change == 0) runtime::profile_reload_epoch.fetch_add(1);
    if (change == 1) runtime::kSetIntensity.Commit(0.8125f);
    if (change == 2) {
      WritePrivateProfileStringA(runtime::kConfigSection, "NRIntensity", "0.8125", ini_path.c_str());
      WritePrivateProfileStringA(nullptr, nullptr, nullptr, ini_path.c_str());
      runtime::LoadConfiguration();
    }
    if (change == 3) {
      WritePrivateProfileStringA(runtime::kConfigSection, "ProfileGameId", std::string(32, 'b').c_str(), ini_path.c_str());
      WritePrivateProfileStringA(nullptr, nullptr, nullptr, ini_path.c_str());
    }
    if (change == 4) {
      std::unique_lock lock(runtime::present_restore_configuration.mutex);
      runtime::present_restore_configuration.global_writable = false;
    }
    const auto changed = config_values();
    const auto changed_ini = Bytes();
    settle();
    if (config_values() != changed || Bytes() != changed_ini || !same_content(profile::state, before)
        || encoded(disk()) != encoded(profile::state)) {
      std::cerr << "reset external case=" << change << " notice=" << profile::reset_notice
                << " detail=" << profile::reset_error << '\n';
    }
    Check(config_values() == changed && Bytes() == changed_ini && same_content(profile::state, before)
              && encoded(disk()) == encoded(profile::state),
          "Reload or external configuration change cancels reset and restores the Slot transaction without overwriting settings.");
    Check(profile::reset_notice.find("cancelled") != std::string::npos, "External edits have a visible cancellation result.");
    if (change == 4) {
      std::unique_lock lock(runtime::present_restore_configuration.mutex);
      runtime::present_restore_configuration.global_writable = true;
    }
  }

  // Force a rollback failure after the detached file has really been saved.
  prepare(0);
  const auto recovery_before = profile::state;
  profile::RequestReset(0);
  profile::Tick();
  wait_worker();
  const auto detached = disk();
  lock_path = profile::storage_path;
  lock_path += L".lock";
  {
    presets::internal::Handle held(CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, 0, nullptr));
    if (held.value == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot lock rollback destination.");
    failed_key = "NRIntensity";
    settle();
    failed_key.clear();
    Check(profile::reset.recovery_failed && !profile::Resetting() && profile::active == -1
              && encoded(profile::state) == encoded(detached) && encoded(disk()) == encoded(detached)
              && same_content(profile::reset.before, recovery_before),
          "Rollback failure retains proven detached memory/disk state and the original recovery snapshot.");
    Check(profile::reset_notice.find("rollback failed") != std::string::npos && !profile::dirty,
          "Rollback failure remains explicit and stops automatic retries.");
  }
  const auto recovery_bytes = slot_bytes(profile::storage_path);
  for (int iteration = 0; iteration < 5; ++iteration) profile::Tick();
  profile::Schedule(true);
  Check(!profile::Busy() && slot_bytes(profile::storage_path) == recovery_bytes,
        "Releasing failed rollback lock does not silently restart writes.");
  const auto recovery_ini = Bytes();
  const auto recovery_config = config_values();
  profile::RetryResetRecovery();
  Check(profile::Resetting(), "Manual recovery starts the existing asynchronous rollback path.");
  settle();
  Check(!profile::reset.recovery_failed && !profile::Resetting() && !profile::dirty
            && !profile::save_failed && profile::active == 0
            && same_content(profile::state, recovery_before) && encoded(disk()) == encoded(profile::state)
            && config_values() == recovery_config && Bytes() == recovery_ini,
        "Manual recovery restores the snapshot and association with latest revision, preserving INI and runtime.");
  const auto recovered_bytes = slot_bytes(profile::storage_path);
  profile::RetryResetRecovery();
  Check(!profile::Resetting() && !profile::Busy() && slot_bytes(profile::storage_path) == recovered_bytes,
        "Recovery retry is inert after successful restoration.");
  profile::active = -1;
  Check(adapter::Apply(original, true, false, &error, false, nullptr, true), "Restore all fixture settings after reset safety cases.");
  profile::observed_snapshot = profile::Snapshot();
  profile::observed_epoch = runtime::profile_commit_epoch.load();
  profile::observed_reload_epoch = runtime::profile_reload_epoch.load();
}
