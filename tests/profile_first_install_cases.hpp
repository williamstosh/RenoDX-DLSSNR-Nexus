/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Included inside RunProfileRuntimeCases; only synthetic INI and profile paths.
{
  using Evidence = profile::FirstInstallEvidence;
  const auto supported = [](const Json&, std::string*) { return true; };
  const auto prepare = [&]() {
    seed();
    profile::fresh_storage = true;
    for (const auto* key : {"ProfileGameId", "ProfileBalancedInitialized"}) {
      Check(WritePrivateProfileStringA(runtime::kConfigSection, key, nullptr, ini_path.c_str()) != FALSE,
            "Clear only synthetic initialization metadata.");
    }
    WritePrivateProfileStringA(nullptr, nullptr, nullptr, ini_path.c_str());
  };
  prepare();
  Check(profile::ApplySnapshot({{"NRResolutionScale", 1.f}}, &error), "Set distinct initial scale for real rollback test.");
  const auto original = profile::Snapshot();
  const auto ini_before = Bytes();
  const auto state_before = encoded(profile::state);
  for (const auto evidence : {Evidence::UNKNOWN, Evidence::EXISTING}) {
    Check(!profile::TryInitializeBalanced(evidence, supported), "Unknown or existing NR configuration never initializes.");
  }
  Check(!profile::TryInitializeBalanced(Evidence::PROVEN_EMPTY, {}), "Missing capability callback blocks initialization.");
  Check(!profile::TryInitializeBalanced(Evidence::PROVEN_EMPTY, [](const Json&, std::string*) { return false; }),
        "Unsupported capability blocks initialization.");
  Check(Bytes() == ini_before && profile::Snapshot() == original && encoded(profile::state) == state_before,
        "Rejected evidence/capability preserves settings, INI and profile state.");
  profile::fresh_storage = false;
  Check(!profile::TryInitializeBalanced(Evidence::PROVEN_EMPTY, supported), "Existing, relocated or missing old storage cannot initialize.");
  profile::fresh_storage = true;
  for (const auto* key : {"ProfileGameId", "ProfileBalancedInitialized"}) {
    const auto saved_ini = Bytes();
    const auto saved_state = profile::state;
    const auto saved_evidence = runtime::first_install_evidence;
    // Real ReShade returns false for a zero-element value. Presence evidence
    // comes from LoadConfiguration's disk scan, not a fabricated raw getter.
    {
      std::ofstream fixture(ini_path, std::ios::binary | std::ios::trunc);
      fixture << "[OTHER.ADDON]\r\nPersistent=17\r\n[" << runtime::kConfigSection << "]\r\n" << key << "=\r\n";
      if (!fixture) throw std::runtime_error("Cannot seed empty metadata INI.");
    }
    WritePrivateProfileStringA(nullptr, nullptr, nullptr, ini_path.c_str());
    runtime::LoadConfiguration();
    Check(!runtime::ReadPresentRestoreRaw(key).has_value(), "Empty metadata follows the real ReShade raw-value contract.");
    Check(runtime::first_install_evidence == Evidence::EXISTING,
          "LoadConfiguration identifies an otherwise empty metadata-only INI as existing.");
    const auto after_load = profile::Snapshot();
    const auto after_load_ini = Bytes();
    Check(!profile::TryInitializeBalanced(runtime::first_install_evidence, supported)
              && !profile::first_install.pending && profile::Snapshot() == after_load
              && Bytes() == after_load_ini && encoded(profile::state) == encoded(saved_state),
          "Actual loaded empty identity/marker evidence blocks initialization without side effects.");
    {
      std::ofstream fixture(ini_path, std::ios::binary | std::ios::trunc);
      fixture << saved_ini;
      if (!fixture) throw std::runtime_error("Cannot restore empty metadata fixture INI.");
    }
    WritePrivateProfileStringA(nullptr, nullptr, nullptr, ini_path.c_str());
    runtime::LoadConfiguration();
    Check(profile::ApplySnapshot(original, &error) && profile::Snapshot() == original,
          "Empty metadata case restores original running settings before marker failure injection.");
    profile::state = saved_state;
    runtime::first_install_evidence = saved_evidence;
    profile::observed_snapshot = profile::Snapshot();
    profile::observed_epoch = runtime::profile_commit_epoch.load();
    profile::observed_reload_epoch = runtime::profile_reload_epoch.load();
  }
  profile::state.revision = 1;
  Check(!profile::TryInitializeBalanced(Evidence::PROVEN_EMPTY, supported), "Old manifest, including detached selection, blocks initialization.");
  profile::state.revision = 0;
  profile::state.slots[0].overrides["NRIntensity"] = 0.5f;
  Check(!profile::TryInitializeBalanced(Evidence::PROVEN_EMPTY, supported), "Explicit old override blocks initialization.");
  profile::state.slots[0].overrides.clear();
  profile::writable = false;
  Check(!profile::TryInitializeBalanced(Evidence::PROVEN_EMPTY, supported), "Unavailable or corrupt storage blocks initialization.");
  profile::writable = true;
  failed_key = "ProfileBalancedInitialized";
  Check(!profile::TryInitializeBalanced(Evidence::PROVEN_EMPTY, supported), "Marker write failure prevents any application.");
  Check(profile::Snapshot() == original && !profile::first_install.pending, "Marker failure keeps original configuration.");
  failed_key.clear();

  prepare();
  const auto expected = data::Template(1, profile::defaults_snapshot);
  Check(profile::TryInitializeBalanced(Evidence::PROVEN_EMPTY, supported), "Proven clean first install begins durable preparation.");
  Check(profile::active == -1 && profile::Snapshot() == original, "No association or real apply before storage completion.");
  profile::Request(profile::Action::SWITCH, 0);
  Check(profile::pending_action == profile::Action::NONE, "User action cannot interleave with first-install preparation.");
  settle();
  Check(profile::active == 1 && profile::Snapshot() == expected && profile::state.last_applied == expected
            && disk().selected == "official_balanced" && disk().last_applied == expected,
        "First install really applies Balanced and persists a matching association.");
  Check(profile::undo.available && profile::undo.configuration, "First install retains existing transactional Undo.");
  Check(!profile::TryInitializeBalanced(Evidence::PROVEN_EMPTY, supported), "Successful initialization never repeats.");
  profile::Request(profile::Action::UNDO);
  settle();
  Check(profile::Snapshot() == original && profile::active == -1 && profile::state.selected.empty(),
        "Undo restores original running values and detaches Balanced.");
  Check(!profile::TryInitializeBalanced(Evidence::PROVEN_EMPTY, supported)
            && runtime::ReadPresentRestoreRaw("ProfileBalancedInitialized") == "1", "Undo/detach retains persistent no-repeat marker.");

  prepare();
  failed_key = "ProfileGameId";
  Check(profile::TryInitializeBalanced(Evidence::PROVEN_EMPTY, supported), "Identity failure injection starts initialization.");
  settle();
  Check(profile::Snapshot() == original && profile::active == -1 && profile::state.selected.empty()
            && !profile::first_install.pending && profile::save_failed, "Identity persistence failure leaves original settings and association.");
  failed_key.clear();
  settle();

  prepare();
  const auto safe_path = profile::storage_path;
  profile::storage_path = root / "absent-parent" / "slots.json";
  Check(profile::TryInitializeBalanced(Evidence::PROVEN_EMPTY, supported), "Store failure injection starts initialization.");
  settle();
  Check(profile::Snapshot() == original && profile::active == -1 && profile::state.selected.empty()
            && profile::save_failed, "Store failure cannot apply Balanced or select it.");
  profile::storage_path = safe_path;
  settle();

  prepare();
  failed_key = "NRResolutionScale";
  Check(profile::TryInitializeBalanced(Evidence::PROVEN_EMPTY, supported), "Apply readback failure injection starts initialization.");
  settle();
  Check(profile::Snapshot() == original && profile::active == -1 && profile::state.selected.empty()
            && disk().selected.empty(), "Batch Apply rollback keeps original values and clears prepared disk association.");
  failed_key.clear();

  prepare();
  unsigned capability_calls = 0;
  Check(profile::TryInitializeBalanced(Evidence::PROVEN_EMPTY, [&](const Json&, std::string*) { return ++capability_calls == 1; }),
        "Capability is initially available.");
  settle();
  Check(capability_calls == 2 && profile::Snapshot() == original && profile::active == -1 && disk().selected.empty(),
        "Capability loss during storage prevents application and clears prepared association.");

  prepare();
  Check(profile::TryInitializeBalanced(Evidence::PROVEN_EMPTY, supported), "Concurrent-edit cancellation starts initialization.");
  runtime::kSetIntensity.Commit(0.8125f);
  const auto edited = profile::Snapshot();
  settle();
  Check(profile::Snapshot() == edited && profile::active == -1 && disk().selected.empty(),
        "Concurrent user edit cancels initialization without overwriting it.");
  Check(profile::ApplySnapshot(original, &error), "Restore initialization fixture.");

  // A saved identity whose file is missing is not evidence of a new install.
  prepare();
  const auto missing_id = profile::game_id;
  Check(WritePrivateProfileStringA(runtime::kConfigSection, "ProfileGameId", missing_id.c_str(), ini_path.c_str()) != FALSE,
        "Seed identity with absent old manifest.");
  profile::loaded = false;
  profile::Initialize();
  settle(false);
  Check(profile::loaded && !profile::fresh_storage && profile::game_id == missing_id
            && !profile::TryInitializeBalanced(Evidence::PROVEN_EMPTY, supported), "Real load of missing old manifest never becomes fresh storage.");

  prepare();
  {
    std::ofstream file(profile::storage_path);
    file << "broken manifest";
  }
  Check(WritePrivateProfileStringA(runtime::kConfigSection, "ProfileGameId", profile::game_id.c_str(), ini_path.c_str()) != FALSE,
        "Seed identity with corrupt manifest.");
  profile::loaded = false;
  profile::Initialize();
  settle(false);
  Check(!profile::loaded && !profile::writable && !profile::fresh_storage
            && profile::Snapshot() == original
            && !profile::TryInitializeBalanced(Evidence::PROVEN_EMPTY, supported), "Real corrupt manifest load keeps configuration and blocks initialization.");
  seed();
}
