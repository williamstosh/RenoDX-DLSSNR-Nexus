/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Included in the real production-menu fixture after its profile switch tests.
{
  namespace profile = runtime::profile_ui;
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
  const auto official_copy = profile::data::Resolve(0, profile::state.slots[0], profile::defaults_snapshot, profile::Validate);
  click(custom_button(4));
  click(find("Copy from official profile"));
  click(find("NR Lite"));
  settle_profile_ux();
  check(profile::active == 4 && profile::state.slots[4].populated
            && profile::state.slots[4].snapshot == official_copy && profile::Snapshot() == official_copy,
        "empty slot can copy and apply an official profile directly");
  for (const float width : {1000.f, 570.f, 320.f}) {
    panel_width = width;
    io.DisplaySize = ImVec2(panel_width, panel_height);
    frame(); frame(); frame();
    check(find("##nr_enable").rect.Min.y < find("open_presets").rect.Min.y,
          "responsive layout keeps NR switch first");
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
  click(find("profile_actions"));
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
  click(find("profile_actions"));
  click(find("restore_profile"));
  click(find("confirm_restore"));
  settle_profile_ux();
  check(profile::state.slots[0].overrides.empty(), "confirmed restore affects current official slot");
  click(find("profile_actions"));
  click(find("undo_profile"));
  settle_profile_ux();
  check(profile::Snapshot() == before_restore && profile::state.slots[0].overrides == before_override,
        "profile actions preserve undo after restoring defaults");
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
