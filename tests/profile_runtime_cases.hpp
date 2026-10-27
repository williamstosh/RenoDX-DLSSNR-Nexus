/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Include inside configuration-test main after LoadConfiguration and host fixture
// initialization. No namespace-scope includes are needed. This declares a local
// lambda, reusing runtime, Check, failed_key, ini_path and Bytes. Call explicitly
// with RunProfileRuntimeCases(std::filesystem::current_path() / "profile-runtime").
const auto RunProfileRuntimeCases = [&](const std::filesystem::path& outputdir) {
  namespace profile = runtime::profile_ui;
  namespace data = renodx::addons::dlss5::profiles;
  namespace fs = std::filesystem;
  using Json = data::Json;

  const auto root = fs::absolute(outputdir);
  fs::create_directories(root);
  const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
  std::wstring previous_appdata(required, L'\0');
  if (required != 0) {
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", previous_appdata.data(), required);
    if (length >= required) throw std::runtime_error("LOCALAPPDATA changed during test setup.");
    previous_appdata.resize(length);
  }
  struct EnvironmentGuard {
    std::wstring previous;
    ~EnvironmentGuard() {
      SetEnvironmentVariableW(L"LOCALAPPDATA", previous.empty() ? nullptr : previous.c_str());
    }
  } environment_guard{previous_appdata};
  if (!SetEnvironmentVariableW(L"LOCALAPPDATA", (root / L"appdata").c_str())) {
    throw std::runtime_error("Cannot isolate profile appdata.");
  }
  if (profile::Busy()) throw std::runtime_error("Profile worker must be idle before these cases.");
  const auto original_configuration = profile::Snapshot();
  const auto original_failed_key = failed_key;
  unsigned sequence = 0;
  std::string error;

  // Make pending debounce urgent once, then return as soon as the real worker
  // completes. A failure stops this suite instead of resetting an active worker.
  const auto settle = [&](bool urgent = true) {
    if (urgent && profile::dirty && !profile::save_failed) profile::Schedule(true);
    const auto deadline = GetTickCount64() + 10000;
    for (;;) {
      profile::Tick();
      if (!profile::Busy() && profile::pending_action == profile::Action::NONE
          && (!profile::dirty || profile::save_failed)) return;
      if (GetTickCount64() >= deadline) throw std::runtime_error("Profile worker did not settle within 10 seconds.");
      Sleep(1);
    }
  };
  const auto disk = [&]() {
    data::State result;
    const bool read = data::Read(profile::storage_path, profile::Validate, &result, &error);
    Check(read, "Profile worker publishes a readable manifest.");
    if (!read) throw std::runtime_error(error);
    return result;
  };
  const auto encoded = [](const data::State& state) { return data::internal::Encode(state); };
  const auto seed = [&]() {
    settle();
    failed_key.clear();
    profile::loaded = true;
    profile::writable = true;
    profile::dirty = profile::save_failed = false;
    profile::active = -1;
    profile::undo = {};
    profile::pending_action = profile::Action::NONE;
    profile::pending_index = -1;
    profile::serial = 0;
    profile::notice.clear();
    profile::defaults_snapshot = profile::Snapshot(true);
    profile::observed_snapshot = profile::Snapshot();
    profile::observed_epoch = runtime::profile_commit_epoch.load();
    profile::observed_reload_epoch = runtime::profile_reload_epoch.load();
    profile::state = data::EmptyState(std::string(64, 'a'));
    profile::game_id = std::string(24, '0') + std::to_string(10000000u + ++sequence);
    if (!data::StoragePath(profile::game_id, &profile::storage_path, &error)) throw std::runtime_error(error);
    if (fs::exists(profile::storage_path)) throw std::runtime_error("Profile test directory must be fresh.");
  };
  const auto commit = [&](const runtime::Setting& setting, float value) {
    // Commit owns RuntimeLock for model/recreate paths; never acquire the
    // nonrecursive runtime mutex around this public operation.
    setting.Commit(value);
    // Production Commit increments the epoch; Observe must consume it.
    profile::Observe();
    profile::Tick();
  };
  const auto select = [&](int index) {
    profile::Request(profile::Action::SWITCH, index);
    settle();
    Check(profile::active == index, "Requested populated Slot becomes active.");
  };

  seed();
  const auto initial = profile::Snapshot();
  const auto initial_ini = Bytes();
  const auto initial_writes = writes.size();
  profile::Observe();
  profile::Tick();
  Check(profile::Snapshot() == initial && Bytes() == initial_ini && writes.size() == initial_writes,
        "First empty profile state leaves configuration untouched.");
  Check(!fs::exists(profile::storage_path) && profile::active == -1 && !profile::dirty,
        "First empty state does not manufacture a selected profile or disk write.");
  // Run the production startup worker, including executable SHA256, generated
  // identity and real local file reads. No fixture worker result is substituted.
  profile::loaded = false;
  profile::Initialize();
  settle(false);
  Check(profile::loaded && profile::state.owner.size() == 64 && profile::game_id.size() == 32,
        "Initialize worker derives executable owner and random game identity.");
  Check(profile::Snapshot() == initial && Bytes() == initial_ini && writes.size() == initial_writes,
        "Real first startup does not write configuration or apply a profile.");
  const auto actual_owner = profile::state.owner;
  const auto actual_id = profile::game_id;
  const auto actual_path = profile::storage_path;
  profile::Assign(3, initial, true);
  settle();
  const auto saved_startup = disk();
  profile::loaded = false;
  profile::Initialize();
  settle(false);
  Check(profile::state.owner == actual_owner && profile::game_id == actual_id && profile::storage_path == actual_path
            && profile::state.revision == saved_startup.revision && profile::active == 3,
        "Real restart reads ProfileGameId and restores matching disk association.");
  seed();
  const auto empty_state = encoded(profile::state);
  profile::Request(profile::Action::SWITCH, 3);
  settle();
  Check(encoded(profile::state) == empty_state && profile::Snapshot() == initial,
        "Selecting empty custom Slot never clears running settings.");

  for (int index = 0; index < 3; ++index) {
    select(index);
    commit(runtime::kSetIntensity, 0.25f + static_cast<float>(index) * 0.125f);
    commit(runtime::kSetPassStage[2], static_cast<float>(index + 1));
    settle();
  }
  commit(runtime::kSetIntensity, 0.5625f);
  settle(false);
  Check(!profile::dirty && disk().slots[2].overrides["NRIntensity"] == 0.5625f,
        "Automatic debounce saves edits through real worker without urgent scheduling.");
  commit(runtime::kSetIntensity, 0.5f);
  settle();
  for (int index = 0; index < 3; ++index) {
    select(index);
    Check(profile::Snapshot()["NRIntensity"] == 0.25f + static_cast<float>(index) * 0.125f,
          "Official switches preserve each Slot's independent override.");
    Check(profile::Snapshot()["NRPass4Stage"] == index + 1,
          "Each official Slot preserves its independent inactive pass stage.");
  }
  commit(runtime::kSetStackPasses, 1.f);
  commit(runtime::kSetPassPreset[2], 2.f);
  commit(runtime::kSetPassStage[2], 3.f);
  commit(runtime::kSetPassIntensity[2], 0.375f);
  settle();
  const auto captured = profile::Snapshot();
  Check(captured["NRPasses"] == 1 && captured["NRPass4Preset"] == 2 && captured["NRPass4Intensity"] == 0.375f
            && captured["NRPass4Stage"] == 3,
        "Snapshot retains settings for disabled Pass 4.");
  for (int index = 3; index < 6; ++index) {
    profile::Assign(index, captured, true);
    settle();
    Check(profile::state.slots[index].populated && profile::state.slots[index].snapshot == captured,
          "Custom capture stores complete configuration under fixed ID.");
    // The production rename button has no callable data action; reproduce only
    // its state mutation and Changed call, without claiming UI interaction.
    const auto before = profile::state;
    profile::state.slots[index].name = "Name 123";
    profile::undo = {.before = before, .after = profile::state, .previous_active = profile::active, .available = true};
    profile::Changed(true);
    settle();
    Check(profile::state.slots[index].snapshot == captured
              && profile::state.selected == data::SLOT_IDS[index], "Rename preserves custom identity and snapshot.");
  }
  for (int index = 0; index < 6; ++index) {
    select(index);
    Check(profile::Snapshot()["NRPass4Stage"] == (index < 3 ? index + 1 : 3),
          "All six Slots restore their persisted stage selection.");
  }
  Check(disk().slots[5].snapshot["NRPass4Preset"] == 2, "Real worker persists disabled pass selection.");

  seed();
  select(0);
  commit(runtime::kSetIntensity, 0.375f);
  profile::Request(profile::Action::SWITCH, 1);
  settle();
  Check(profile::active == 1 && disk().slots[0].overrides["NRIntensity"] == 0.375f,
        "Switch waits for pending edits to save before applying next official Slot.");

  seed();
  select(0);
  commit(runtime::kSetIntensity, 0.25f);
  settle();
  select(2);
  commit(runtime::kSetIntensity, 0.625f);
  settle();
  profile::Request(profile::Action::UNDO);
  settle();
  Check(profile::Snapshot()["NRIntensity"] == 0.625f, "Undo selection preserves later Setting commit.");
  Check(profile::active == -1 && profile::state.selected.empty(), "Undo with later edit detaches association.");
  Check(profile::state.slots[2].overrides["NRIntensity"] == 0.625f, "Undo preserves edited target Slot data.");

  seed();
  select(1);
  commit(runtime::kSetIntensity, 0.375f);
  settle();
  select(0);
  commit(runtime::kSetIntensity, 0.25f);
  settle();
  const auto before_restore = profile::state;
  profile::Request(profile::Action::RESTORE, 0);
  settle();
  Check(profile::state.slots[0].overrides.empty(), "Restore clears current official overrides.");
  Check(profile::state.slots[1].overrides == before_restore.slots[1].overrides,
        "Restore leaves another official Slot's override untouched.");
  profile::Request(profile::Action::UNDO);
  settle();
  Check(profile::state.slots[0].overrides == before_restore.slots[0].overrides
            && profile::Snapshot()["NRIntensity"] == 0.25f, "Undo restore recovers personal override and running value.");

  seed();
  select(0);
  const auto before_failure = encoded(profile::state);
  const auto running_before_failure = profile::Snapshot();
  profile::state.slots[1].overrides["NRIntensity"] = 0.125f;
  const auto target_before_failure = encoded(profile::state);
  failed_key = "NRIntensity";
  profile::Request(profile::Action::SWITCH, 1);
  Check(WasWritten("NRIntensity"), "Apply failure injection reaches real configuration writer.");
  failed_key.clear();
  settle();
  Check(encoded(profile::state) == target_before_failure && profile::active == 0,
        "Failed Apply does not publish target Slot state or selection.");
  Check(profile::Snapshot() == running_before_failure, "Failed Apply preserves runtime snapshot.");
  Check(encoded(disk()) == before_failure, "Failed Apply does not publish manifest.");

  seed();
  select(0);
  const auto before_save_failure = disk();
  const auto failed_path = profile::storage_path;
  auto lock_path = failed_path;
  lock_path += L".lock";
  {
    renodx::addons::dlss5::presets::internal::Handle held(CreateFileW(
        lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
    if (held.value == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot acquire failure-injection storage lock.");
    commit(runtime::kSetIntensity, 0.625f);
    profile::Request(profile::Action::SWITCH, 2);
    settle();
    Check(profile::save_failed && profile::dirty && profile::active == 0,
          "Pending save failure blocks queued profile switch.");
    Check(profile::pending_action == profile::Action::NONE, "Failed save cancels queued switch.");
    Check(encoded(disk()) == encoded(before_save_failure), "Failed save preserves disk state.");
    for (int iteration = 0; iteration < 3; ++iteration) profile::Tick();
    Check(profile::active == 0 && !profile::Busy(), "Tick never resumes failed switch or silently retries.");
  }
  profile::Schedule(true);
  settle();
  Check(!profile::save_failed && !profile::dirty && profile::active == 0, "Explicit retry saves edits without reviving cancelled switch.");

  // A coalescing worker may publish an earlier revision before its next write
  // fails. Completion must retain that proven disk revision for a safe retry.
  {
    auto persisted = disk();
    const auto previous_revision = profile::state.revision;
    Check(data::Write(profile::storage_path, &persisted, persisted.revision, profile::Validate, &error),
          "Prepare a successfully published intermediate revision.");
    profile::dirty = true;
    ++profile::serial;
    {
      std::unique_lock lock(profile::worker.mutex);
      profile::worker.pending = profile::state;
      profile::worker.pending.revision = persisted.revision;
      profile::worker.loading = false;
      profile::worker.error = "Synthetic second coalesced write failed.";
      profile::worker.ready = true;
    }
    profile::Tick();
    Check(profile::state.revision == previous_revision + 1 && profile::dirty && profile::save_failed,
          "Failed later write keeps intermediate disk revision and unsaved status.");
    profile::Schedule(true);
    settle();
    Check(!profile::save_failed && !profile::dirty && disk().revision == persisted.revision + 1,
          "Retry after a partially successful coalesced save uses the proven revision.");
  }

  // Synchronous identity-write failure has no worker completion to cancel the
  // action. Keep this regression separate from the asynchronous disk failure.
  {
    const auto saved_identity = Read("ProfileGameId");
    Check(WritePrivateProfileStringA(runtime::kConfigSection, "ProfileGameId", nullptr, ini_path.c_str()) != FALSE,
          "Prepare missing private identity for failure injection.");
    WritePrivateProfileStringA(nullptr, nullptr, nullptr, ini_path.c_str());
    failed_key = "ProfileGameId";
    commit(runtime::kSetIntensity, 0.5f);
    profile::Request(profile::Action::SWITCH, 1);
    profile::Tick();
    Check(profile::save_failed && profile::active == 0, "Identity save error blocks profile switch.");
    Check(profile::pending_action == profile::Action::NONE, "Identity save error cancels queued switch without worker completion.");
    failed_key.clear();
    Check(WritePrivateProfileStringA(runtime::kConfigSection, "ProfileGameId", saved_identity.c_str(), ini_path.c_str()) != FALSE,
          "Restore synthetic identity after failure injection.");
    WritePrivateProfileStringA(nullptr, nullptr, nullptr, ini_path.c_str());
    profile::Schedule(true);
    settle();
  }

  // Exercise the production load-completion branch with a real disk-read result.
  // Avoid Initialize's executable identity hashing so all paths/IDs remain synthetic.
  const auto restart_state = disk();
  const auto restart_running = profile::Snapshot();
  const auto load_result = [&](const data::State& saved) {
    if (profile::Busy()) throw std::runtime_error("Load completion requires idle worker.");
    profile::loaded = false;
    {
      std::unique_lock lock(profile::worker.mutex);
      profile::worker.result = saved;
      profile::worker.id = profile::game_id;
      profile::worker.path = profile::storage_path;
      profile::worker.loading = true;
      profile::worker.relocated = false;
      profile::worker.error.clear();
      profile::worker.ready = true;
    }
    profile::Tick();
  };
  load_result(restart_state);
  Check(profile::active == 0 && profile::Snapshot() == restart_running, "Restart matching snapshot associates without applying.");
  commit(runtime::kSetIntensity, 0.75f);
  settle();
  const auto changed_running = profile::Snapshot();
  const auto before_restart_ini = Bytes();
  const auto before_restart_writes = writes.size();
  load_result(restart_state);
  Check(profile::active == -1 && profile::Snapshot() == changed_running && Bytes() == before_restart_ini
            && writes.size() == before_restart_writes, "Restart mismatched snapshot never reapplies saved settings.");

  seed();
  select(0);
  commit(runtime::kSetIntensity, 0.25f);
  settle();
  const auto before_upgrade = profile::state.slots[0].overrides;
  profile::state.slots[0].template_version = 1;
  profile::defaults_snapshot["NRIntensity"] = 0.875f;
  profile::defaults_snapshot["NRPass4Intensity"] = 0.5f;
  select(0);
  Check(profile::state.slots[0].overrides == before_upgrade && profile::Snapshot()["NRIntensity"] == 0.25f
            && profile::Snapshot()["NRPass4Intensity"] == 0.5f,
        "Template/default refresh preserves explicit override and inherits untouched fields.");
  const auto before_unknown = profile::Snapshot();
  const auto before_future = encoded(profile::state);
  profile::state.slots[1].template_version = data::TEMPLATE_VERSION + 1;
  const auto future_state = encoded(profile::state);
  profile::Request(profile::Action::SWITCH, 1);
  settle();
  Check(encoded(profile::state) == future_state && profile::Snapshot() == before_unknown && profile::active == 0,
        "Future template version rejects Resolve without publishing target state.");
  profile::state.slots[1].template_version = before_future["slots"]["official_balanced"]["template_version"].get<uint32_t>();
  profile::state.slots[1].overrides["UnknownProfileField"] = 1;
  const auto unknown_state = encoded(profile::state);
  profile::Request(profile::Action::SWITCH, 1);
  settle();
  Check(encoded(profile::state) == unknown_state && profile::Snapshot() == before_unknown && profile::active == 0,
        "Unknown official setting is rejected without dropping data or applying.");
  profile::state.slots[1].overrides.erase("UnknownProfileField");
  const auto custom_before = encoded(profile::state);
  profile::Assign(4, {{"UnknownProfileField", 1}}, true);
  Check(encoded(profile::state) == custom_before, "Unknown custom capture is rejected.");

  const bool previous_unusable = runtime::present_unusable.exchange(true);
  const Json present_settings{{"NRHookPoint", runtime::kHookPresent}};
  const auto before_present = profile::Snapshot();
  const auto before_present_ini = Bytes();
  const auto before_present_writes = writes.size();
  const auto present_rows = runtime::preset_ui::Preview(
      {.scope = renodx::addons::dlss5::presets::Scope::FULL, .settings = present_settings}, false);
  Check(std::any_of(present_rows.begin(), present_rows.end(), [](const auto& row) {
          return row.key == "NRHookPoint" && std::string_view(row.status) == "Unsupported" && !row.apply;
        }), "Preview reports unsupported Present request.");
  Check(!profile::ApplySnapshot(present_settings, &error), "ApplySnapshot rejects unsupported Present request.");
  Check(profile::Snapshot() == before_present && Bytes() == before_present_ini && writes.size() == before_present_writes,
        "Unsupported Present request preserves configuration and runtime.");
  runtime::present_unusable.store(previous_unusable);

  seed();
  select(0);
  const auto before_external = profile::state.slots[0].overrides;
  Check(WritePrivateProfileStringA(runtime::kConfigSection, "NRIntensity", "0.8125", ini_path.c_str()) != FALSE,
        "External configuration fixture write succeeds.");
  WritePrivateProfileStringA(nullptr, nullptr, nullptr, ini_path.c_str());
  runtime::LoadConfiguration();
  // Force observation even if LoadConfiguration does not increment the epoch.
  runtime::profile_commit_epoch.fetch_add(1, std::memory_order_relaxed);
  profile::Observe();
  profile::Tick();
  Check(profile::active == -1 && profile::state.selected.empty(), "External configuration reload detaches active profile.");
  Check(profile::state.slots[0].overrides == before_external, "External reload does not rewrite official overrides.");
  settle();

  // Finish all real writes before restoring fixture configuration/environment.
  profile::active = -1;
  profile::state.selected.clear();
  profile::loaded = false;
  failed_key.clear();
  Check(profile::ApplySnapshot(original_configuration, &error), "Restore configuration after profile integration cases.");
  failed_key = original_failed_key;
};
