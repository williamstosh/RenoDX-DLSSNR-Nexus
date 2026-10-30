/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Included in the real production-menu fixture after its profile switch tests.
{
  namespace profile = runtime::profile_ui;
  namespace manager = runtime::preset_ui;
  namespace presets = renodx::addons::dlss5::presets;
  const Json original_settings = profile::Snapshot();
  const bool original_enabled = runtime::enabled.load();
  const auto settle_profile_ux = [&]() {
    const auto deadline = GetTickCount64() + 10000;
    for (;;) {
      profile::Tick();
      if (!profile::Busy() && profile::pending_action == profile::Action::NONE && !profile::dirty) break;
      check(!profile::save_failed, "profile UX persistence failed");
      check(GetTickCount64() < deadline, "profile UX persistence timed out");
      Sleep(1);
    }
    frame(); frame();
  };
  const auto custom_button = [&](int index) {
    const auto* window = find("open_presets").window;
    const ImGuiID scope = ImHashStr("nr_profiles", 0, ImHashStr("presets", 0, window->ID));
    const ImGuiID id = ImHashStr("##custom_slot", 0, ImHashData(&index, sizeof(index), scope));
    check(items.contains(id), "custom profile retains its stable index ID");
    return items.at(id);
  };
  runtime::ui_menu_page = 0;
  runtime::ui_language_choice = 1;
  panel_width = 1000.f;
  panel_height = 1400.f;
  io.DisplaySize = ImVec2(panel_width, panel_height);
  font_size = 18.f;
  frame(); frame();
  check(find("##nr_enable").rect.Min.y < find("open_presets").rect.Min.y,
        "NR main switch precedes the entire profile area");
  const auto before_empty = profile::Snapshot();
  click(custom_button(3));
  capture("ux-create-custom-slot");
  check(!profile::state.slots[3].populated && profile::Snapshot() == before_empty,
        "opening empty-slot creation does not clear or apply configuration");
  click(find("cancel_create_slot"));
  check(!profile::state.slots[3].populated, "cancel leaves an empty slot empty");
  click(custom_button(3));
  click(find("create_slot_current"));
  settle_profile_ux();
  check(profile::state.slots[3].populated && profile::active == 3
            && profile::state.slots[3].snapshot == before_empty && profile::Snapshot() == before_empty,
        "empty slot can capture current settings directly without entering management");
  profile::state.slots[3].name = "A deliberately long custom profile ## identity remains separate ### 123";
  profile::Changed(true);
  settle_profile_ux();
  const auto official_copy = profile::data::Template(0, profile::defaults_snapshot);
  click(custom_button(4));
  click(find("Copy from official profile"));
  click(find("NR Lite"));
  settle_profile_ux();
  check(profile::active == 4 && profile::state.slots[4].populated
            && profile::state.slots[4].snapshot == official_copy && profile::Snapshot() == official_copy,
        "empty slot can copy and apply an official profile directly");
  // The import preview must describe the target slot, not the running game.
  const auto previous_preview = manager::preview;
  manager::preview = {.metadata = {{"name", "Synthetic slot preview"}},
                      .scope = presets::Scope::QUALITY_MULTIPASS,
                      .settings = {{"NRIntensity", manager::ReadSetting("NRIntensity")}}};
  const auto preview_slot_before = profile::state.slots[3].snapshot;
  profile::state.slots[3].snapshot["NRIntensity"] = 0.375;
  const auto slot_preview = manager::SlotPreview(3);
  const auto intensity_row = std::find_if(slot_preview.begin(), slot_preview.end(), [](const auto& row) { return row.key == "NRIntensity"; });
  check(intensity_row != slot_preview.end() && intensity_row->before == Json(0.375)
            && intensity_row->after == manager::ReadSetting("NRIntensity") && intensity_row->apply,
        "slot import preview uses the actual target baseline even when the import equals running settings");
  profile::state.slots[3].snapshot = preview_slot_before;
  manager::preview.settings["NRIntensity"] = -100;
  const auto before_invalid_import = profile::state.slots[3].snapshot;
  check(!manager::AssignPreviewToSlot(3) && profile::state.slots[3].snapshot == before_invalid_import,
        "invalid compact import preserves existing slot contents");
  manager::preview = previous_preview;
  const auto settle_preset_worker = [&]() {
    const auto deadline = GetTickCount64() + 10000;
    for (;;) {
      frame();
      bool busy;
      {
        std::shared_lock lock(manager::worker.mutex);
        busy = manager::worker.busy || manager::worker.ready;
      }
      if (!busy) break;
      check(GetTickCount64() < deadline, "compact preset worker timed out");
      Sleep(1);
    }
    frame(); frame();
  };
  const auto right_click_slot = [&](int index) {
    const auto item = custom_button(index);
    io.AddMousePosEvent(item.rect.GetCenter().x, item.rect.GetCenter().y);
    frame();
    io.AddMouseButtonEvent(1, true);
    frame();
    io.AddMouseButtonEvent(1, false);
    frame();
    frame();
  };
  const auto right_click_lite = [&]() {
    const auto* window = find("open_presets").window;
    const ImGuiID scope = ImHashStr("##official_profiles", 0,
        ImHashStr("nr_profiles", 0, ImHashStr("presets", 0, window->ID)));
    const auto item = items.at(ImHashStr(ui::Tr("NR Lite"), 0, scope));
    io.AddMousePosEvent(item.rect.GetCenter().x, item.rect.GetCenter().y);
    frame();
    io.AddMouseButtonEvent(1, true); frame();
    io.AddMouseButtonEvent(1, false); frame(); frame();
  };
  const auto running_before_manage = profile::Snapshot();
  const int active_before_manage = profile::active;
  right_click_slot(3);
  check(profile::active == active_before_manage &&
            profile::Snapshot() == running_before_manage,
        "right-click management does not select or apply a slot");
  click(find("Rename slot"));
  frame();
  frame();
  check(find("rename_slot").window != nullptr,
        "rename opens a compact operation window");
  std::strcpy(profile::rename, "Renamed quick slot");
  click(find("rename_slot"));
  settle_profile_ux();
  check(profile::Name(3) == "Renamed quick slot" &&
            profile::active == active_before_manage,
        "compact rename updates display without changing stable slot identity "
        "or association");
  click(find("close_slot_operation"));
  frame();
  frame();
  click(custom_button(3));
  settle_profile_ux();
  check(profile::active == 3 &&
            profile::Snapshot() == profile::state.slots[3].snapshot,
        "populated custom left-click directly applies its settings");
  right_click_slot(3);
  click(find("Copy from official profile"));
  frame();
  frame();
  check(find("confirm_slot_operation").disabled,
        "copying over a populated slot requires confirmation");
  click(find("close_slot_operation"));
  frame();
  frame();
  const auto before_clear = profile::state.slots[3];
  const auto running_before_clear = profile::Snapshot();
  right_click_slot(3);
  click(find("Clear slot"));
  frame();
  frame();
  check(find("confirm_slot_operation").disabled,
        "clearing requires explicit confirmation");
  click(find("Confirm replacing or clearing this slot"));
  click(find("confirm_slot_operation"));
  settle_profile_ux();
  check(!profile::state.slots[3].populated && profile::active == -1 &&
            profile::state.selected.empty() &&
            profile::Snapshot() == running_before_clear &&
            profile::Name(3) == ui::Tr("Custom 1"),
        "clearing detaches the active slot, resets its name and keeps running "
        "settings");
  runtime::kSetIntensity.Commit(0.75f);
  profile::Observe();
  check(!profile::state.slots[3].populated,
        "Observe cannot repopulate a cleared slot");
  click(find("undo_slot_operation"));
  settle_profile_ux();
  check(profile::state.slots[3].populated &&
            profile::state.slots[3].name == before_clear.name &&
            profile::state.slots[3].snapshot == before_clear.snapshot &&
            profile::active == -1,
        "clear Undo restores only the slot and keeps later running edits "
        "detached");
  click(find("close_slot_operation"));
  frame();
  frame();
  right_click_slot(3);
  click(find("Import profile"));
  settle_preset_worker();
  check(!find("choose_import").disabled && !find("read_preset").disabled,
        "compact import exposes existing native chooser and manual path input");
  click(find("close_slot_operation"));
  frame();
  frame();
  right_click_slot(3);
  click(find("Export profile"));
  settle_preset_worker();
  check(!find("choose_export").disabled && !find("choose_folder").disabled,
        "compact export exposes existing file and folder dialogs");
  check(manager::path[0] != '\0' && std::filesystem::u8path(manager::path).parent_path() == manager::library_path,
        "asynchronous library discovery fills a default export filename in the shared library");
  click(find("close_slot_operation"));
  frame();
  frame();
  for (const float width : {1000.f, 570.f, 320.f}) {
    panel_width = width;
    io.DisplaySize = ImVec2(panel_width, panel_height);
    frame(); frame(); frame();
    check(find("##nr_enable").rect.Min.y < find("open_presets").rect.Min.y,
          "responsive layout keeps NR switch first");
    const auto undo_header = find("undo_profile");
    const auto manage_header = find("open_presets");
    check(std::abs(undo_header.rect.Min.y - manage_header.rect.Min.y) < 1.f
              && undo_header.rect.Max.x <= manage_header.rect.Min.x,
          "Undo and management share one header row without overlap");
    check(std::none_of(items.begin(), items.end(), [](const auto& entry) {
      return entry.second.label == "###profile_actions";
    }), "redundant profile actions entry is removed");
    const float content_width = find("open_presets").window->WorkRect.GetWidth();
    if (content_width < font_size * 18.f) {
      const auto* window = find("open_presets").window;
      const ImGuiID scope = ImHashStr("nr_profiles", 0, ImHashStr("presets", 0, window->ID));
      const ImGuiID id = ImHashStr("##custom_profiles", 0, scope);
      check(items.contains(id), "narrow custom chooser is present under its stable profile scope");
      const auto combo = items.at(id);
      click(combo);
      unsigned choices = 0;
      for (const auto& [id, item] : items) {
        if (item.label == "##custom_profile_choice") ++choices;
      }
      check(choices == 3, "narrow dropdown keeps all three fixed custom identities accessible");
      io.AddKeyEvent(ImGuiKey_Escape, true); frame();
      io.AddKeyEvent(ImGuiKey_Escape, false); frame();
    } else {
      const auto first = custom_button(3);
      check(first.rect.GetWidth() >= font_size * 4.f, "parallel slot buttons retain a readable text budget");
    }
    capture(("ux-profiles-width-" + std::to_string(static_cast<int>(width))).c_str());
  }
  panel_width = 650.f;
  io.DisplaySize = ImVec2(panel_width, panel_height);
  runtime::ui_language_choice = 14;
  profile::Request(profile::Action::SWITCH, 0);
  settle_profile_ux();
  runtime::kSetIntensity.Commit(0.625f);
  profile::Observe();
  profile::Schedule(true);
  settle_profile_ux();
  const auto before_restore = profile::Snapshot();
  const auto before_override = profile::state.slots[0].overrides;
  right_click_lite();
  click(find("restore_profile"));
  frame(); frame();
  const auto* confirm = ImGui::FindWindowByName("恢复官方默认？###restore_profile_confirm");
  // Match stable suffix as well, since fonts/translations may change the title.
  if (confirm == nullptr) {
    for (const auto* window : ImGui::GetCurrentContext()->Windows) {
      if (window->Active && std::strstr(window->Name, "###restore_profile_confirm") != nullptr) confirm = window;
    }
  }
  check(confirm != nullptr && confirm->Active && confirm->Size.x >= font_size * 14.f
            && confirm->Size.y < panel_height * 0.6f, "official restore uses a horizontal bounded confirmation");
  capture("ux-restore-confirmation-zh");
  click(find("cancel_restore"));
  check(profile::Snapshot() == before_restore && profile::state.slots[0].overrides == before_override,
        "restore cancellation preserves personal values");
  right_click_lite();
  click(find("restore_profile"));
  click(find("confirm_restore"));
  settle_profile_ux();
  check(profile::state.slots[0].overrides.empty(), "confirmed restore affects current official slot");
  check(std::string_view(profile::undo.action_label) == "Restore official defaults",
        "header Undo describes the actual restore action");
  click(find("undo_profile"));
  settle_profile_ux();
  check(profile::Snapshot() == before_restore && profile::state.slots[0].overrides == before_override,
        "profile actions preserve undo after restoring defaults");
  profile::Request(profile::Action::SWITCH, 1);
  settle_profile_ux();
  const auto before_inactive_restore = profile::Snapshot();
  const auto inactive_override = profile::state.slots[0].overrides;
  profile::Request(profile::Action::RESTORE, 0);
  settle_profile_ux();
  check(profile::active == 1 && profile::Snapshot() == before_inactive_restore
            && profile::state.slots[0].overrides.empty(),
        "restoring an inactive official slot does not switch or apply it");
  profile::Request(profile::Action::UNDO);
  settle_profile_ux();
  check(profile::active == 1 && profile::Snapshot() == before_inactive_restore
            && profile::state.slots[0].overrides == inactive_override,
        "inactive official restoration is reversible without changing the active slot");
  runtime::enabled = false;
  frame(); frame();
  check(std::none_of(rendered_text.begin(), rendered_text.end(), [](const auto& text) {
    return text.text == ui::Tr("Profile applied. Runtime execution is pending confirmation.");
  }), "NR off does not show a persistent profile execution success message");
  profile::save_failed = true;
  check(std::string(profile::SaveStatus()) != "Saved", "saving failures cannot be presented as saved");
  profile::save_failed = false;
  runtime::enabled = original_enabled;
  profile::active = -1;
  profile::state.selected.clear();
  std::string error;
  check(profile::ApplySnapshot(original_settings, &error), "restore production configuration after profile UX cases");
  profile::observed_snapshot = original_settings;
  profile::observed_epoch = runtime::profile_commit_epoch.load();
  profile::undo.available = false;
  profile::notice.clear();
  runtime::ui_language_choice = 1;
  panel_width = 1000.f;
  panel_height = 1100.f;
  io.DisplaySize = ImVec2(panel_width, panel_height);
  frame(); frame();
  std::puts("profile UX: switch order, local empty creation/cancel, responsive identities, restore/cancel/undo and save-status cases passed");
}
