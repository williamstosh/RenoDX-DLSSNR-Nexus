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
  const auto profile_hover = [&](const char* label, const char* recipe) {
    const auto id = find(label).id;
    for (int i = 0; i < 40; ++i) {
      const auto center = items.at(id).rect.GetCenter();
      io.AddMousePosEvent(center.x, center.y);
      frame();
    }
    check(std::ranges::any_of(tooltip_text, [&](const auto& text) {
      return text.find(recipe) != std::string::npos;
    }), "official tooltip describes the actual template recipe");
    check(std::ranges::none_of(tooltip_text, [](const auto& text) {
      return text.find("Saved settings do not confirm NR execution.") != std::string::npos;
    }), "ordinary Slot tooltip leaves runtime evidence to the status area");
  };
  profile_hover("NR Lite", "Official template: 1 Render pass, model B, scale 0.67, Default style.");
  profile_hover("NR Balanced", "Official template: 1 Upscaled pass, model A, scale 0.67, Default style.");
  profile_hover("NR Max", "Official template: Render B (Cinematic), then Upscaled A (Default); both at scale 0.67.");
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
  const auto* clear_window = find("confirm_slot_operation").window;
  check(std::strstr(clear_window->Name, "###nr_slot_operation_clear") != nullptr
            && std::fabs(clear_window->Rect().GetCenter().x - io.DisplaySize.x * 0.5f) <= 1.f
            && std::fabs(clear_window->Rect().GetCenter().y - io.DisplaySize.y * 0.5f) <= 1.f,
        "dangerous clear confirmation has a separate centered modal identity");
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
    const auto manage_header = find("open_presets");
    check(manage_header.rect.GetWidth() > 0.f
              && std::none_of(items.begin(), items.end(), [](const auto& entry) {
                return entry.second.label.find("undo_profile") != std::string::npos;
              }), "management remains available without a misleading header Undo");
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
  profile::Request(profile::Action::UNDO);
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
  // Exercise actual mouse input with a movable host menu, rather than assigning
  // a popup's position from the fixture and masking production placement.
  {
    ImVec2 menu_position(20.f, 40.f);
    ImVec2 menu_size(650.f, 500.f);
    const auto placement_frame = [&]() {
      items.clear();
      rendered_text.clear();
      tooltip_text.clear();
      ImGui::GetStyle().FontSizeBase = font_size;
      ImGui::NewFrame();
      runtime::RefreshUiLanguage();
      ImGui::SetNextWindowPos(menu_position);
      ImGui::SetNextWindowSize(menu_size);
      ImGui::Begin("Slot placement fixture", nullptr, ImGuiWindowFlags_NoSavedSettings);
      profile::slot_menu_bounds = {.position = ImGui::GetWindowPos(), .size = ImGui::GetWindowSize()};
      {
        ui::StyleScope theme;
        ui::PushPanelTheme(theme);
        ImGui::BeginChild("##slot_settings", ImVec2(0.f, 0.f));
        manager::DrawEntry();
        ImGui::EndChild();
      }
      ImGui::End();
      ImGui::Render();
    };
    const auto placement_click = [&](ImVec2 position, int button = 0) {
      io.AddMousePosEvent(position.x, position.y); placement_frame();
      io.AddMouseButtonEvent(button, true); placement_frame();
      io.AddMouseButtonEvent(button, false); placement_frame(); placement_frame();
    };
    const auto bounded = [&](const ImGuiWindow* window) {
      check(window->Pos.x >= 0.f && window->Pos.y >= 0.f
                && window->Pos.x + window->Size.x <= io.DisplaySize.x + 1.f
                && window->Pos.y + window->Size.y <= io.DisplaySize.y + 1.f,
            "Slot menus and tasks stay within the display after auto-fit");
    };
    const auto open_context = [&](int slot = 3) {
      std::printf("Slot placement: language=%d menu=%.0f,%.0f display=%.0fx%.0f slot=%d\n",
                  runtime::ui_language_choice.load(), menu_position.x, menu_position.y,
                  io.DisplaySize.x, io.DisplaySize.y, slot);
      placement_click(custom_button(slot).rect.GetCenter(), 1);
      const auto* context = find("Rename slot").window;
      bounded(context);
      check(!context->Rect().Overlaps(ImRect(profile::slot_anchor.minimum, profile::slot_anchor.maximum)),
            "context placement leaves its source Slot visible");
      const float below = context->Pos.y - profile::slot_anchor.maximum.y;
      const float above = profile::slot_anchor.minimum.y - context->Pos.y - context->Size.y;
      const float left = profile::slot_anchor.minimum.x - context->Pos.x - context->Size.x;
      check((below >= 0.f && below <= font_size)
                || (above >= 0.f && above <= font_size)
                || (left >= 0.f && left <= font_size),
            "context menu remains adjacent to the clicked Slot");
    };
    const auto settings_before_placement = profile::Snapshot();
    const auto slots_before_placement = profile::data::internal::Encode(profile::state);
    const int active_before_placement = profile::active;
    const auto original_validator = profile::capability_validator;
    profile::capability_validator = [](const Json&, std::string* error) {
      if (error != nullptr) {
        *error = "Synthetic unavailable Slot reason";
      }
      return false;
    };
    io.DisplaySize = ImVec2(1800.f, 1100.f);
    runtime::ui_language_choice = 1;
    placement_frame(); placement_frame();
    check(find("NR Lite").disabled, "unavailable profile remains disabled");
    const auto lite_id = find("NR Lite").id;
    for (int i = 0; i < 40; ++i) {
      const auto center = items.at(lite_id).rect.GetCenter();
      io.AddMousePosEvent(center.x, center.y);
      placement_frame();
    }
    check(std::ranges::any_of(tooltip_text, [](const auto& text) {
      return text.find("Synthetic unavailable Slot reason") != std::string::npos;
    }), "disabled Slot tooltip preserves its compatibility reason");
    profile::capability_validator = original_validator;
    placement_frame(); placement_frame();
    for (const int language : {1, 4, 14}) {
      runtime::ui_language_choice = language;
      for (const auto position : {ImVec2(20.f, 40.f), ImVec2(1100.f, 40.f),
                                  ImVec2(20.f, 570.f), ImVec2(1100.f, 570.f)}) {
        io.DisplaySize = ImVec2(1800.f, 1100.f);
        menu_position = position;
        manager::slot_geometry[static_cast<int>(profile::SlotTask::RENAME)] = {};
        placement_frame(); placement_frame();
        open_context();
        placement_click(find("Rename slot").rect.GetCenter());
        auto* task_window = find("rename_slot").window;
        bounded(task_window);
        check((task_window->Flags & ImGuiWindowFlags_Modal) != 0
                  && std::strstr(task_window->Name, "###nr_slot_operation_rename") != nullptr,
              "rename retains the existing task lifecycle with its own stable window identity");
        check(!task_window->Rect().Overlaps(ImRect(profile::slot_anchor.minimum, profile::slot_anchor.maximum)),
              "first rename task stays near and does not cover its Slot when space permits");
        placement_click(find("close_slot_operation").rect.GetCenter());
      }
    }
    runtime::ui_language_choice = 1;
    menu_position = ImVec2(600.f, 40.f);
    placement_frame(); placement_frame();
    open_context(5);
    check(find("Rename slot").window->Pos.y > profile::slot_anchor.maximum.y,
          "the rightmost Slot uses its nearby lower edge when it fits");
    placement_click(ImVec2(menu_position.x + 20.f, menu_position.y + font_size * 0.5f));
    check(ImGui::GetCurrentContext()->OpenPopupStack.empty(), "clicking outside dismisses the context menu");
    menu_position = ImVec2(20.f, 40.f);
    placement_frame(); placement_frame();
    open_context();
    placement_click(find("Rename slot").rect.GetCenter());
    auto* rename_window = find("rename_slot").window;
    const auto before_drag = rename_window->Pos;
    const ImVec2 title(before_drag.x + rename_window->Size.x * 0.5f, before_drag.y + font_size * 0.5f);
    io.AddMousePosEvent(title.x, title.y); placement_frame();
    io.AddMouseButtonEvent(0, true); placement_frame();
    io.AddMousePosEvent(title.x + 35.f, title.y + 45.f); placement_frame();
    io.AddMouseButtonEvent(0, false); placement_frame(); placement_frame();
    const auto moved = rename_window->Pos;
    check(moved.x > before_drag.x + 20.f && moved.y > before_drag.y + 30.f,
          "Slot task supports a real mouse title drag");
    placement_click(find("close_slot_operation").rect.GetCenter());
    menu_position = ImVec2(1100.f, 40.f);
    placement_frame(); placement_frame();
    open_context();
    placement_click(find("Rename slot").rect.GetCenter());
    check(std::fabs(rename_window->Pos.x - moved.x) <= 1.f
              && std::fabs(rename_window->Pos.y - moved.y) <= 1.f,
          "reopening rename preserves the player's moved position");
    placement_click(find("close_slot_operation").rect.GetCenter());
    manager::slot_geometry[static_cast<int>(profile::SlotTask::EXPORT)] = {};
    open_context();
    placement_click(find("Export profile").rect.GetCenter());
    auto* export_window = find("choose_export").window;
    bounded(export_window);
    check(std::strstr(export_window->Name, "###nr_slot_operation_export") != nullptr
              && !export_window->Rect().Overlaps(ImRect(profile::slot_anchor.minimum, profile::slot_anchor.maximum)),
          "export has an independent first Slot anchor, unaffected by rename memory");
    placement_click(find("close_slot_operation").rect.GetCenter());
    for (const float scale : {18.f, 26.f}) {
      font_size = scale;
      io.DisplaySize = ImVec2(700.f, 720.f);
      menu_position = ImVec2(35.f, 180.f);
      placement_frame(); placement_frame();
      open_context();
      placement_click(find("Rename slot").rect.GetCenter());
      bounded(find("rename_slot").window);
      placement_click(find("close_slot_operation").rect.GetCenter());
    }
    check(profile::Snapshot() == settings_before_placement
              && profile::data::internal::Encode(profile::state) == slots_before_placement
              && profile::active == active_before_placement,
          "placement, dragging and task cancellation preserve Slot data and association");
    font_size = 18.f;
    profile::slot_menu_bounds = {};
  }
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
  std::puts("profile UX: Slot mouse/menu/task placement, languages/edges/drag memory, recipes/compatibility, data/undo/save-status cases passed");
}
