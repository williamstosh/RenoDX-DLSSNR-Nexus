/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Include inside ui_menu_tests.cpp main after the preset fixture cases.
// A separate frame is necessary: the original fixture forces popup geometry
// every frame and cannot reproduce a user resize followed by a reopen.
{
  runtime::ui_language_choice = 1;
  runtime::RefreshUiLanguage();
  ui::SetTheme(ui::ThemeId::CYBER_NEON);
  ui::expand_all_sections = false;
  ui::fold_scope = -1;
  font_size = 18.f;
  io.DisplaySize = ImVec2(1200.f, 1000.f);
  manager::mode = 0;
  manager::save_options = manager::export_options = manager::import_path_options = false;
  manager::files.clear();
  manager::selected.clear();
  manager::search[0] = '\0';
  manager::have_preview = false;
  ImGui::ClosePopupToLevel(0, true);
  for (auto* window : ImGui::GetCurrentContext()->Windows) {
    if (std::strstr(window->Name, "nr_preset_manager") != nullptr) {
      window->StateStorage.Clear();
      window->Scroll = ImVec2(0.f, 0.f);
    }
  }
  bool ux_open = true;
  const auto ux_frame = [&]() {
    items.clear();
    rendered_text.clear();
    ImGui::GetStyle().FontSizeBase = font_size;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
    ImGui::SetNextWindowSize(ImVec2(1.f, 1.f));
    ImGui::Begin("Preset UX fixture", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDecoration
        | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs);
    {
      ui::StyleScope theme;
      ui::PushPanelTheme(theme);
      if (ux_open) {
        ImGui::OpenPopup("###nr_preset_manager");
        ux_open = false;
      }
      manager::DrawManager();
    }
    ImGui::End();
    ImGui::Render();
  };
  const auto ux_mouse_click = [&](ImVec2 center) {
    io.AddMousePosEvent(center.x, center.y); ux_frame();
    io.AddMouseButtonEvent(0, true); ux_frame();
    io.AddMouseButtonEvent(0, false); ux_frame(); ux_frame();
  };
  const auto ux_capture = [&](const char* name) {
    panel_width = io.DisplaySize.x;
    panel_height = io.DisplaySize.y;
    capture(name, nullptr, false);
  };
  const auto ux_click = [&](const char* label) {
    const auto item = find(label);
    if (!item.window->InnerRect.Contains(item.rect.GetCenter())) {
      ImGui::SetScrollY(item.window, item.window->Scroll.y + item.rect.GetCenter().y - item.window->InnerRect.GetCenter().y);
      ux_frame(); ux_frame();
    }
    ux_mouse_click(items.at(item.id).rect.GetCenter());
  };
  const auto ux_absent = [&](const char* label) {
    try {
      find(label);
      return false;
    } catch (const std::runtime_error&) {
      return true;
    }
  };
  const auto ux_escape = [&]() {
    io.AddKeyEvent(ImGuiKey_Escape, true); ux_frame();
    io.AddKeyEvent(ImGuiKey_Escape, false); ux_frame(); ux_frame();
  };
  ux_frame(); ux_frame();
  auto* ux_window = ImGui::FindWindowByName("###nr_preset_manager");
  check(ux_window != nullptr && ux_window->Active, "profile manager opens");
  ux_window->StateStorage.Clear();
  ImGui::SetWindowSize(ux_window, ImVec2(620.f, 610.f));
  ImGui::SetWindowPos(ux_window, ImVec2(130.f, 90.f));
  ux_frame(); ux_frame();
  const ImVec2 ux_size = ux_window->Size;
  const ImVec2 ux_position = ux_window->Pos;
  // Close exactly the production modal, then reopen on a later frame.
  ImGui::ClosePopupToLevel(0, true);
  ux_frame();
  ux_open = true;
  ux_frame(); ux_frame();
  check(std::fabs(ux_window->Size.x - ux_size.x) <= 1.f
            && std::fabs(ux_window->Size.y - ux_size.y) <= 1.f,
        "regression: popup reopen must preserve a user resize (old Appearing default resets it)");
  check(std::fabs(ux_window->Pos.x - ux_position.x) <= 1.f
            && std::fabs(ux_window->Pos.y - ux_position.y) <= 1.f,
        "popup reopen preserves a user move");

  check(ux_absent("##preset_modes"), "home has no three parallel tabs");
  check(ux_absent("preview_selected") && ux_absent("delete_preset"), "empty library hides file detail actions");
  check(ux_absent("close_presets"), "title X replaces duplicate footer Close");
  check(find("preset_save_task").disabled == false && find("preset_import_task").disabled == false,
        "home exposes save and import tasks");
  check(ux_absent("rename_slot"), "custom management starts folded");
  ux_capture("ux-library-home");
  const float undo_intensity = runtime::kSetIntensity.Get();
  manager::undo = {{.key = "NRIntensity", .before = undo_intensity, .after = undo_intensity}};
  ux_frame(); ux_frame();
  ux_click("undo_preset");
  check(manager::undo.empty() && runtime::kSetIntensity.Get() == undo_intensity,
        "shared manager home retains the real import undo for classic and modern callers");
  ux_click("custom_slot_tools");
  for (const char* label : {"rename_slot", "save_current_slot", "copy_official_slot", "clear_slot", "share_slot"}) {
    check(!ux_absent(label), "folded custom management retains all original tools");
  }
  ux_click("custom_slot_tools");
  manager::notice = "Synthetic error from another task";
  ux_click("preset_save_task");
  check(manager::mode == 1 && manager::notice.empty(), "save navigation clears stale UI notice");
  check(manager::scope_choice == 1, "new saves default to Quality plus Multi-Pass");
  check(ux_absent("##Author (optional)") && ux_absent("preview_export") && ux_absent("choose_export"),
        "save starts with only name and primary actions");
  ux_capture("ux-save-simple");
  std::strcpy(manager::name, "Synthetic UX profile"); ux_frame(); ux_frame();
  check(!find("save_preset").disabled && !find("copy_preset").disabled,
        "save library and copy share remain available");
  ux_click("More options");
  check(!ux_absent("##Author (optional)") && !ux_absent("##Source game (optional)")
            && !ux_absent("##preset_description") && !ux_absent("preview_export"),
        "More options exposes original metadata and preview");
  ux_click("preview_export");
  check(manager::export_scope == manager::scope_choice && !manager::export_rows.empty(), "export preview still works");
  ux_click("Export file");
  for (const char* label : {"choose_export", "choose_folder", "open_preset_folder", "##Export file path", "export_preset"}) {
    check(!ux_absent(label), "export options retain destination, browsing and explicit file export");
  }
  ux_click("Replace an existing file with this name");
  ux_click("More options");
  ux_click("Export file");
  check(manager::overwrite && !ux_absent("Replace an existing file with this name"),
        "an enabled overwrite confirmation never becomes hidden when options collapse");
  ux_click("Replace an existing file with this name");
  ux_click("More options");
  ux_click("Export file");

  // Escape first belongs to the active editor; it must not close its parent.
  ux_click("##Preset name");
  ux_escape();
  check(ux_window->Active, "Escape in a text editor keeps the manager open");
  ux_click("preset_home");
  ux_click("preset_import_task");
  check(manager::mode == 2 && !manager::have_preview, "import opens its empty state");
  check(!ux_absent("choose_import") && !ux_absent("paste_preset") && ux_absent("read_preset")
            && ux_absent("apply_preset") && ux_absent("import_to_slot"),
        "empty import has choose/paste, with no apply or slot controls");
  check(ux_window->Size.y <= font_size * 15.f, "unread import shrinks its unused content area");
  ux_capture("ux-import-empty");
  ux_click("Enter file path");
  check(!ux_absent("##File path") && !ux_absent("read_preset"), "manual path remains reachable");

  // Seed only synthetic worker completion; no OS dialog, library, or private IO.
  const presets::Document ux_document{
      .metadata = {{"name", "Synthetic full backup"}},
      .scope = presets::Scope::FULL,
      .settings = {{"NRIntensity", 1.25}}};
  for (const auto job : {manager::Job::READ, manager::Job::PARSE, manager::Job::CHOOSE_IMPORT}) {
    {
      std::unique_lock lock(manager::worker.mutex);
      manager::worker.job = job;
      manager::worker.document = ux_document;
      manager::worker.cancelled = false;
      manager::worker.error.clear();
      manager::worker.ready = true;
    }
    ux_frame(); ux_frame();
    check(manager::have_preview && manager::mode == 2 && !manager::confirm_full,
          "file, paste and chooser completions route to an unconfirmed preview");
    check(find("apply_preset").disabled && find("import_to_slot").disabled,
          "full backups require confirmation before apply or slot assignment");
  }
  ux_click("Confirm the full backup scope shown below");
  check(!find("apply_preset").disabled, "full backup confirmation enables apply");
  const auto* target_window = find("import_to_slot").window;
  const auto first_target = ImHashStr("Custom 1", 0, ImHashStr("##import_slot_target", 0, target_window->ID));
  check(items.contains(first_target) && !ux_absent("import_to_slot"), "slot assignment path remains present");
  const auto ux_pending = manager::preview.settings;
  const auto ux_pending_rows = manager::rows.size();
  manager::notice = "Synthetic import error";
  ux_click("preset_home");
  check(manager::notice.empty() && manager::have_preview && manager::preview.settings == ux_pending
            && manager::rows.size() == ux_pending_rows,
        "navigation clears notice while retaining pending apply data");
  check(std::string(manager::name) == "Synthetic UX profile", "navigation preserves typed save draft");

  manager::files = {L"Synthetic A.nrnexus.json", L"Synthetic B.nrnexus.json"};
  ux_frame(); ux_frame();
  ux_click("##preset_file");
  check(!manager::selected.empty(), "mouse selects a nonempty library row");
  std::strcpy(manager::search, "No matching synthetic file"); ux_frame(); ux_frame();
  check(ux_absent("##preset_file"), "library search filters nonmatching rows");
  manager::search[0] = '\0'; ux_frame(); ux_frame();
  for (const char* label : {"preview_selected", "rename_preset", "duplicate_preset", "delete_preset"}) {
    check(!ux_absent(label), "selected file exposes all original library operations");
  }
  check(find("delete_preset").disabled, "file deletion starts unconfirmed");
  ux_click("Confirm deletion of selected file");
  check(!find("delete_preset").disabled, "explicit deletion confirmation enables Delete");
  const auto ux_selected = manager::selected;
  ux_click("preset_save_task");
  check(manager::save_options && manager::export_options, "same-session task expansions are remembered");
  ux_click("preset_home");
  check(manager::selected == ux_selected && !manager::confirm_delete, "task navigation keeps selection and resets deletion consent");

  // Actual title drag and resize grip input exercise the production window.
  const ImVec2 ux_before_drag = ux_window->Pos;
  const ImVec2 ux_title(ux_window->Pos.x + ux_window->Size.x * 0.5f, ux_window->Pos.y + font_size * 0.5f);
  io.AddMousePosEvent(ux_title.x, ux_title.y); ux_frame();
  io.AddMouseButtonEvent(0, true); ux_frame();
  io.AddMousePosEvent(ux_title.x + 25.f, ux_title.y + 20.f); ux_frame(); ux_frame();
  io.AddMouseButtonEvent(0, false); ux_frame(); ux_frame();
  check(ux_window->Pos.x > ux_before_drag.x + 10.f, "title bar supports mouse move");
  const ImVec2 ux_before_resize = ux_window->Size;
  const ImVec2 ux_grip(ux_window->Pos.x + ux_window->Size.x - 3.f, ux_window->Pos.y + ux_window->Size.y - 3.f);
  io.AddMousePosEvent(ux_grip.x, ux_grip.y); ux_frame();
  io.AddMouseButtonEvent(0, true); ux_frame();
  io.AddMousePosEvent(ux_grip.x + 45.f, ux_grip.y + 35.f); ux_frame(); ux_frame();
  io.AddMouseButtonEvent(0, false); ux_frame(); ux_frame();
  check(ux_window->Size.x > ux_before_resize.x + 10.f, "resize grip supports mouse resize");

  // CloseButton ID is test-only ImGui inspection, never used in production.
  ux_mouse_click(items.at(ux_window->GetID("#CLOSE")).rect.GetCenter());
  check(!ux_window->Active && manager::mode == 0, "title X closes and returns the next open to home");
  ux_open = true; ux_frame(); ux_frame();
  check(manager::selected == ux_selected && manager::have_preview, "reopen retains selection and pending preview");
  io.AddMousePosEvent(-100.f, -100.f); ux_frame();
  ux_escape();
  check(!ux_window->Active, "idle Escape closes the manager");
  ux_open = true; ux_frame(); ux_frame();

  for (const auto display : {ImVec2(320.f, 400.f), ImVec2(1920.f, 1080.f), ImVec2(3840.f, 2160.f)}) {
    io.DisplaySize = display;
    font_size = (display.x > 2000.f ? 32.f : 18.f);
    for (const int task : {0, 1, 2}) {
      manager::mode = task;
      ux_frame(); ux_frame();
      check(ux_window->Pos.x >= 0.f && ux_window->Pos.y >= 0.f
                && ux_window->Pos.x + ux_window->Size.x <= display.x + 1.f
                && ux_window->Pos.y + ux_window->Size.y <= display.y + 1.f,
            "display/font changes clamp every task inside the screen");
      if (display.x == 320.f) ux_capture(("ux-manager-narrow-" + std::to_string(task)).c_str());
      for (const auto& [id, item] : items) {
        // Check content controls, not the native window, scrollbar or group bounds.
        if (id != 0 && item.window != nullptr && (item.window->Flags & ImGuiWindowFlags_ChildWindow) != 0
            && id != item.window->ID
            && id != ImGui::GetWindowScrollbarID(item.window, ImGuiAxis_X)
            && id != ImGui::GetWindowScrollbarID(item.window, ImGuiAxis_Y)
            && std::strstr(item.window->Name, "##preset_description_") == nullptr
            && std::strstr(item.window->Name, "nr_preset_manager") != nullptr) {
          if (item.rect.Max.x > item.window->InnerRect.Max.x + 1.f) {
            std::printf("manager overflow: display=%.0fx%.0f font=%.0f task=%d item=%s right=%.1f inner=%.1f window=%s\n",
                        display.x, display.y, font_size, task, item.label.c_str(), item.rect.Max.x,
                        item.window->InnerRect.Max.x, item.window->Name);
          }
          check(item.rect.Max.x <= item.window->InnerRect.Max.x + 1.f, "narrow manager controls stay inside child width");
        }
      }
    }
  }
  ImGui::ClosePopupToLevel(0, true); ux_frame();
  std::puts("preset UX: production mouse/navigation/geometry and synthetic worker routing cases; no game/visual acceptance");
}
