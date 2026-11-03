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
  {
    const auto saved_hook = runtime::hook_point.load();
    const auto saved_count = runtime::stack_passes.load();
    const auto saved_stage = runtime::pass_stage[0].load();
    const auto saved_mode = runtime::pass_resolution_mode[0].load();
    const auto saved_source = runtime::last_source_plan;
    const auto saved_rr = runtime::stage_source_rr.load();
    const auto* game_source = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xCA9102});
    runtime::PresentStream internal_present;
    runtime::features.emplace(internal_present.Handle(), runtime::FeatureState{});
    runtime::pass_stage[0] = 0u;
    runtime::pass_resolution_mode[0] = 0u;
    for (const unsigned count : {1u, 2u}) {
      runtime::hook_point = runtime::kHookPresent;
      runtime::stack_passes = count;
      // Each game observation is followed by an internal SR-shaped Present
      // plan. Repeated RR frames must not oscillate the Home controls.
      for (const int source : {1, 1, 0, 0, 1, 0, -1}) {
        {
          renodx::addons::dlss5::RuntimeLock lock(runtime::runtime_mutex);
          if (source < 0) {
            runtime::ForgetSourceHandleLocked(game_source);
          } else {
            runtime::created_feature_ids[game_source] = (source == 1 ? runtime::kFeatureDlssd : runtime::kFeatureDlss);
            runtime::EvaluationStagePlan(game_source);
          }
          runtime::EvaluationStagePlan(internal_present.Handle(), false);
        }
        frame();
        const bool render_available = source == 0;
        check(find(ui::StageDisplayName(renodx::addons::dlss5::stage_plan::Stage::RENDER)).disabled == !render_available,
            "Home Render button follows game RR / SR / released evidence on every frame");
        check(find("NR Lite").disabled == !render_available && !find("NR Balanced").disabled && !find("NR Max").disabled,
            "Home Lite follows Render eligibility while Balanced and Max retain Present eligibility");
        std::vector<Item> custom_buttons;
        for (const auto& [id, item] : items) {
          if (item.label == "##custom_slot") custom_buttons.push_back(item);
        }
        std::sort(custom_buttons.begin(), custom_buttons.end(), [](const auto& a, const auto& b) {
          return a.rect.Min.x < b.rect.Min.x;
        });
        check(custom_buttons.size() == 3, "Home exposes all three custom Slot buttons");
        if (custom_buttons.size() == 3) {
          check(custom_buttons[0].disabled == !render_available && !custom_buttons[1].disabled && !custom_buttons[2].disabled,
              "Custom Render / Present targets use the same per-frame source gate as official recipes");
        }
        compare();
        for (int index = 0; index < 6; ++index) {
          std::string reason;
          check(profile::SlotDisplayAvailable(index, &reason) == (index % 3 != 0 || render_available),
              "Every Slot has an explicit expected capability, independent of fresh/cache agreement");
          check(profile::slot_display_cache[index].rebuilds == builds[index],
              "Game source changes refresh eligibility without rebuilding unchanged Slot payloads");
        }
      }
    }
    {
      renodx::addons::dlss5::RuntimeLock lock(runtime::runtime_mutex);
      runtime::ForgetSourceHandleLocked(game_source);
      runtime::ForgetSourceHandleLocked(internal_present.Handle());
      runtime::last_source_plan = saved_source;
      runtime::stage_source_rr = saved_rr;
    }
    runtime::hook_point = saved_hook;
    runtime::stack_passes = saved_count;
    runtime::pass_stage[0] = saved_stage;
    runtime::pass_resolution_mode[0] = saved_mode;
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
