/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Included in the production-menu fixture with initialized, isolated storage.
{
  namespace profile = runtime::profile_ui;
  const auto saved_slots = profile::state.slots;
  const auto saved_defaults = profile::defaults_snapshot;
  const auto saved_fg = runtime::native_fg_declaration.load();
  const auto saved_fg_off = runtime::native_fg_public_off.load();
  const auto saved_fg_owner = runtime::native_fg_owns.load();
  const auto saved_present = runtime::present_unusable.load();
  const auto saved_models = runtime::direct_runtime_requirements.dual_models;
  for (int index = 0; index < 6; ++index) {
    profile::state.slots[index] = {.populated = true, .template_version = profile::data::TEMPLATE_VERSION,
        .snapshot = profile::data::Template(index % 3, profile::defaults_snapshot)};
  }
  const auto compare = [&] {
    for (int index = 0; index < 6; ++index) {
      std::string expected_error, display_error;
      check(profile::SlotAvailable(index, &expected_error) == profile::SlotDisplayAvailable(index, &display_error)
                && expected_error == display_error,
            "cached Slot display matches fresh target validation and failure reason");
    }
  };
  compare();
  std::array<uint64_t, 6> builds{};
  for (int index = 0; index < 6; ++index) builds[index] = profile::slot_display_cache[index].rebuilds;
  for (int repeat = 0; repeat < 60; ++repeat) frame();
  for (int index = 0; index < 6; ++index) {
    check(builds[index] == profile::slot_display_cache[index].rebuilds,
        "settled Home frames do not resolve, serialize or revalidate unchanged Slot payloads");
  }
  runtime::native_fg_declaration = runtime::NativeFgDeclaration::kOn;
  runtime::native_fg_public_off = false;
  runtime::native_fg_owns = true;
  compare();
  runtime::present_unusable = true;
  compare();
  std::string error;
  check(!profile::SlotDisplayAvailable(2, &error), "cached Max never masks unavailable Present");
  runtime::present_unusable = saved_present;
  runtime::native_fg_declaration = saved_fg;
  runtime::native_fg_public_off = saved_fg_off;
  runtime::native_fg_owns = saved_fg_owner;
  runtime::direct_runtime_requirements.dual_models = false;
  compare();
  runtime::direct_runtime_requirements.dual_models = saved_models;
  profile::state.slots[0].overrides["NRIntensity"] = 0.73f;
  compare();
  check(profile::slot_display_cache[0].rebuilds == builds[0] + 1
            && profile::slot_display_cache[0].settings["NRIntensity"] == 0.73f,
        "personal overrides invalidate only their Slot payload");
  profile::state.slots[3].snapshot["NRPasses"] = 0u;
  compare();
  check(!profile::SlotDisplayAvailable(3, &error), "invalid imported custom payload is rejected after a cache hit");
  profile::state.slots[3].snapshot = profile::data::Template(0, profile::defaults_snapshot);
  profile::state.slots[3].template_version = profile::data::TEMPLATE_VERSION + 1;
  compare();
  profile::state.slots[3].template_version = profile::data::TEMPLATE_VERSION;
  profile::defaults_snapshot["NRIntensity"] = 0.89f;
  compare();
  check(profile::slot_display_cache[1].settings["NRIntensity"] == 0.89f,
      "new registered defaults invalidate cached official recipes");
  profile::state.slots[4].populated = false;
  compare();
  profile::state.slots = saved_slots;
  profile::defaults_snapshot = saved_defaults;
  frame(); frame();
  std::puts("profile menu: settled-frame reuse, six Slots, FG/Present/model changes and payload/default/version invalidation passed");
}
