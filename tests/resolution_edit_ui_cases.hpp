/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Real production widgets with synthetic FG evidence; no game or GPU execution.
{
  namespace profile = runtime::profile_ui;
  namespace edit = runtime::resolution_edit;
  const auto saved = profile::Snapshot();
  const auto saved_page = runtime::ui_menu_page;
  const auto saved_pass = runtime::ui_selected_pass;
  const auto saved_width = panel_width;
  const auto saved_height = panel_height;
  const auto saved_fg = runtime::native_fg_declaration.load();
  const auto saved_off = runtime::native_fg_public_off.load();
  const auto saved_owner = runtime::native_fg_owns.load();
  const auto saved_geometry = runtime::stage_telemetry[2];
  const auto ultra = renodx::addons::dlss5::profiles::Template(2, profile::Snapshot(true));
  runtime::native_fg_declaration = runtime::NativeFgDeclaration::kOn;
  runtime::native_fg_public_off = false;
  runtime::native_fg_owns = true;
  runtime::stage_telemetry[2].width = 1920;
  runtime::stage_telemetry[2].height = 1080;
  runtime::ui_menu_page = 0;
  runtime::ui_selected_pass = 0;
  panel_width = 1000.f;
  panel_height = 2400.f;
  std::string error;
  check(profile::ApplySnapshot(ultra, &error), "FG menu fixture applies the unchanged official Max recipe");
  frame(); frame();
  const auto original = profile::Snapshot();
  const auto write_count = configuration_writes;
  check(!find("nr_resolution_quality").disabled, "67 percent offers explicit repair instead of an unreachable disabled control");
  click(find("nr_resolution_quality"));
  check(edit::pending && edit::confirmation_required && profile::Snapshot() == original
            && configuration_writes == write_count,
        "real base-resolution click stages a complete candidate without publishing or writing");
  click(find("cancel_resolution_change"));
  check(!edit::pending && profile::Snapshot() == original && configuration_writes == write_count,
        "real Cancel keeps all models, appearance, results and saved resolution values");
  click(find("nr_resolution_quality"));
  click(find("confirm_resolution_change"));
  auto repaired = original;
  repaired.update({{"NRResolutionScale", .67f}, {"NRPass2ResolutionMode", 0u}});
  check(!edit::pending && profile::Snapshot() == repaired && runtime::QueryConfigurationCapability().CanSubmit(),
        "one explicit confirmation commits Present FG matching 67 percent passes");
  click(find("undo_resolution_change"));
  check(profile::Snapshot() == original, "real Undo restores the complete pre-repair configuration");
  click(find("redo_resolution_change"));
  check(profile::Snapshot() == repaired, "real Redo restores the same resolution batch");

  check(profile::ApplySnapshot(ultra, &error), "pass-count sequence restarts from unchanged Max");
  runtime::kSetStackPasses.Commit(1.f);
  frame(); frame();
  click(find("nr_resolution_quality"));
  check(!edit::pending && runtime::nr_resolution_scale.load() == .67f
            && runtime::pass_resolution_mode[0].load() == 2
            && runtime::pass_resolution_scale[0].load() == 1.f,
        "single-pass edit preserves the dormant fixed-100 second pass");
  runtime::ui_menu_page = 1;
  frame(); frame();
  const auto single = profile::Snapshot();
  click(find("2"));
  check(edit::pending && profile::Snapshot() == single, "real two-pass click offers inheritance before changing the pass count");
  click(find("cancel_resolution_change"));
  check(profile::Snapshot() == single, "cancelling two-pass repair preserves the single-pass snapshot");
  click(find("2"));
  click(find("confirm_resolution_change"));
  check(runtime::stack_passes.load() == 2 && runtime::pass_resolution_mode[0].load() == 0
            && runtime::nr_resolution_scale.load() == .67f, "real pass-count repair applies count and inheritance together");
  runtime::ui_selected_pass = 0;
  frame(); frame();
  click(find("nr_resolution_performance"));
  runtime::ui_selected_pass = 1;
  frame(); frame();
  click(find("50%"));
  check(!edit::pending && runtime::pass_resolution_mode[0].load() == 2
            && runtime::pass_resolution_scale[0].load() == .5f
            && runtime::QueryConfigurationCapability().CanSubmit(),
        "explicit second-pass fixed 50 sets mode and scale in one valid batch without another confirmation");
  edit::Cancel();
  edit::undo.clear();
  edit::redo.clear();
  for (const auto& [key, value] : saved.items()) {
    if (const auto* setting = runtime::preset_ui::FindSetting(key)) {
      setting->Store(value.is_boolean() ? (value.get<bool>() ? 1.f : 0.f) : value.get<float>());
    }
  }
  runtime::native_fg_declaration = saved_fg;
  runtime::native_fg_public_off = saved_off;
  runtime::native_fg_owns = saved_owner;
  runtime::stage_telemetry[2] = saved_geometry;
  runtime::ui_menu_page = saved_page;
  runtime::ui_selected_pass = saved_pass;
  panel_width = saved_width;
  panel_height = saved_height;
  profile::observed_snapshot = profile::Snapshot();
  frame(); frame();
}
