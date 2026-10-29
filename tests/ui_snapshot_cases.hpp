/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Included inside main after the shared production frame/click/mesh helpers.
// Preview invokes this before the matrix; the legacy doc opt-in invokes it after regression.
if (std::getenv("NR_NEXUS_UI_SNAPSHOTS") != nullptr || std::getenv("NR_NEXUS_DOC_CAPTURES") != nullptr) {
  namespace profile = runtime::profile_ui;
  namespace manager = runtime::preset_ui;
  const char* mode_environment = std::getenv("NR_NEXUS_UI_SNAPSHOTS");
  const std::string snapshot_mode = (mode_environment == nullptr ? "Quick" : mode_environment);
  check(snapshot_mode == "Quick" || snapshot_mode == "Full", "unknown UI snapshot mode");
  const bool docs_only = mode_environment == nullptr;
  const bool include_docs = docs_only || std::getenv("NR_NEXUS_UI_DOCS") != nullptr;
  struct Scenario {
    std::string id;
    int page = 0;
    std::string action = "expanded";
    ui::ThemeId theme = ui::ThemeId::CYBER_NEON;
    int language = 14;
    float width = 827.f;
    float font = 18.f;
    std::string doc_role;
  };
  const char* pages[] = {"quality", "multipass", "diagnostics", "advanced"};
  std::vector<Scenario> scenarios;
  if (!docs_only) {
    scenarios.push_back({.id = "quality-viewport", .action = "viewport"});
    scenarios.push_back({.id = "status-details", .action = "status-details"});
    for (const char* action : {"slot-context", "official-context", "slot-rename", "slot-copy", "slot-export", "slot-import", "slot-clear"}) {
      scenarios.push_back({.id = action, .action = action});
    }
    for (int page = 0; page < 4; ++page) {
      for (const char* state : {"expanded", "collapsed"}) {
        scenarios.push_back({.id = std::string(pages[page]) + "-" + state, .page = page, .action = state});
      }
    }
    for (const auto& [id, page] : {std::pair{"model-dropdown", 0}, {"model-tooltip", 0},
                                  {"preset-library", 0}, {"preset-save", 0}, {"preset-import", 0},
                                  {"preset-export", 0}, {"preset-import-preview", 0},
                                  {"auxiliary-context", 0}, {"auxiliary-tooltip", 0},
                                  {"restore-confirmation", 0}, {"custom-slot-create", 0},
                                  {"pass-independent", 1}, {"pass-first", 1},
                                  {"diagnostics-normal", 2}, {"diagnostics-warning", 2}, {"diagnostics-error", 2}}) {
      scenarios.push_back({.id = id, .page = page, .action = id});
    }
    scenarios.push_back({.id = "pass-render-a-upscaled-b", .page = 1,
                         .action = "cross-stage", .language = 1});
    scenarios.push_back({.id = "pass-render-a-upscaled-b-zh", .page = 1,
                         .action = "cross-stage", .language = 14});
    scenarios.push_back({.id = "pass-resolution-67", .page = 1,
                         .action = "cross-stage-resolution", .language = 1});
    scenarios.push_back({.id = "pass-resolution-67-zh", .page = 1,
                         .action = "cross-stage-resolution", .language = 14});
    scenarios.push_back({.id = "same-stage-resolution", .page = 1,
                         .action = "same-stage-resolution", .language = 1});
    scenarios.push_back({.id = "same-stage-resolution-zh", .page = 1,
                         .action = "same-stage-resolution", .language = 14});
    scenarios.push_back({.id = "render-stage-resolution", .page = 1,
                         .action = "render-stage-resolution", .language = 1});
    scenarios.push_back({.id = "render-stage-resolution-zh", .page = 1,
                         .action = "render-stage-resolution", .language = 14});
    scenarios.push_back({.id = "present-stage-resolution", .page = 1,
                         .action = "present-stage-resolution", .language = 1});
    scenarios.push_back({.id = "present-stage-resolution-zh", .page = 1,
                         .action = "present-stage-resolution", .language = 14});
    scenarios.push_back({.id = "gpu-pass-diagnostics", .page = 2,
                         .action = "gpu-pass-diagnostics", .language = 1});
    scenarios.push_back({.id = "gpu-pass-diagnostics-zh", .page = 2,
                         .action = "gpu-pass-diagnostics", .language = 14});
    scenarios.push_back({.id = "gpu-comparison", .page = 2,
                         .action = "gpu-comparison", .language = 1, .doc_role = "gpu-comparison"});
    scenarios.push_back({.id = "gpu-comparison-zh", .page = 2,
                         .action = "gpu-comparison", .language = 14, .doc_role = "gpu-comparison"});
    if (snapshot_mode == "Full") {
      scenarios.push_back({.id = "status-details-narrow", .action = "status-details", .width = 450.f});
      scenarios.push_back({.id = "pass-cross-stage-collapsed-zh", .page = 1, .action = "cross-stage-collapsed"});
      scenarios.push_back({.id = "pass-render2-upscaled2-zh", .page = 1, .action = "cross-stage-four"});
      scenarios.push_back({.id = "pass-backward-stage-zh", .page = 1, .action = "cross-stage-backward"});
      scenarios.push_back({.id = "pass-mixed-present-zh", .page = 1, .action = "cross-stage-present"});
      for (const char* id : {"preset-selected", "preset-rename", "preset-delete-confirmation",
                             "preset-custom-slots", "preset-import-scope", "preset-import-scope-confirmed",
                             "preset-export-actions"}) {
        scenarios.push_back({.id = id, .action = id});
      }
      scenarios.push_back({.id = "restore-all-confirmation", .page = 3, .action = "restore-all-confirmation"});
      scenarios.push_back({.id = "issue-report-description", .page = 2, .action = "issue-report-description"});
      // Orthogonal additions reuse the same runner instead of multiplying every axis.
      for (const auto& theme : ui::kThemes) {
        scenarios.push_back({.id = std::string(theme.slug) + "-slot-hover", .action = "slot-hover", .theme = theme.id});
        for (int page = 0; page < 4; ++page) {
          scenarios.push_back({.id = std::string(theme.slug) + "-" + pages[page], .page = page, .theme = theme.id});
        }
      }
      for (int language : {1, 4}) {
        for (int page = 0; page < 4; ++page) {
          scenarios.push_back({.id = std::string(ui::kLanguages[language - 1].code) + "-" + pages[page],
                               .page = page, .language = language});
        }
      }
      for (int page = 0; page < 4; ++page) {
        scenarios.push_back({.id = std::string(pages[page]) + "-narrow", .page = page, .width = 450.f});
        scenarios.push_back({.id = std::string(pages[page]) + "-scaled", .page = page, .width = 1200.f, .font = 26.f});
        scenarios.push_back({.id = std::string(pages[page]) + "-viewport-narrow", .page = page,
                             .action = "viewport", .width = 450.f});
      }
    }
  }
  if (include_docs) {
    for (int language : {1, 14}) {
      const std::string suffix = (language == 14 ? "-zh" : "");
      for (const auto& [role, page] : {std::pair{"quality", 0}, {"multipass", 1}, {"results", 0},
                                     {"diagnostics", 2}, {"advanced", 3}, {"presets", 0}}) {
        scenarios.push_back({.id = std::string("doc-") + role + suffix, .page = page,
                             .action = (std::string_view(role) == "presets" ? "preset-save"
                                        : std::string_view(role) == "results" ? "expanded" : "doc-default"),
                             .language = language, .width = (page == 1 ? 1400.f : 827.f), .doc_role = role});
      }
      for (const auto& theme : ui::kThemes) {
        scenarios.push_back({.id = std::string("doc-theme-") + theme.slug + suffix,
                             .action = "doc-default", .theme = theme.id, .language = language,
                             .doc_role = std::string("theme-") + theme.slug});
      }
    }
  }
  std::vector<std::string> filters;
  if (const char* filter = std::getenv("NR_NEXUS_UI_SCENARIOS")) {
    std::string remaining(filter);
    while (!remaining.empty()) {
      const auto comma = remaining.find(',');
      std::string token = remaining.substr(0, comma);
      const auto first = token.find_first_not_of(" \t");
      if (first != std::string::npos) {
        token = token.substr(first, token.find_last_not_of(" \t") - first + 1);
        check(std::ranges::any_of(scenarios, [&](const auto& scenario) {
          return scenario.id == token || pages[scenario.page] == token;
        }), "unknown UI snapshot scenario filter");
        filters.push_back(token);
      }
      if (comma == std::string::npos) break;
      remaining.erase(0, comma + 1);
    }
  }
  Json registry{{"schema", "nr-nexus-ui-captures/1"}, {"mode", snapshot_mode},
                {"synthetic", true}, {"expected", Json::array()}, {"captures", Json::array()},
                {"limitations", {"Synthetic fixture, not a game runtime or GPU validation.",
                                  "Fixture system fonts do not include the ReShade icon font; icon glyph fidelity is not certified.",
                                  "Native file dialogs, compatibility editor and IME candidates require their existing native tests.",
                                  "Rename is an inline editor and delete uses an inline consent checkbox; neither is a modal in the production UI.",
                                  "Snapshot scenarios do not execute file rename/delete, preset apply, or custom-slot writes.",
                                  "Restore-all confirmation and the report description wizard are inline production UI, not modal windows.",
                                  "Report collection, screenshot capture, attachments, ZIP saving, upload and browser submission are not executed or captured."}}};
  for (const auto& scenario : scenarios) {
    if (filters.empty() || std::ranges::any_of(filters, [&](const auto& filter) {
      return filter == scenario.id || filter == pages[scenario.page];
    })) registry["expected"].push_back(scenario.id);
  }
  check(!registry["expected"].empty(), "UI snapshot selection is empty");
  runtime::ui_classic_layout = false;
  ui::expand_all_sections = false;
  runtime::hook_point = runtime::kHookPresent;
  runtime::stack_passes = 4;
  runtime::pass_follow[0] = false;
  runtime::pass_look_result[0].follow = false;
  runtime::pass_look_result[0].mode = true;
  runtime::look_mode = true;
  const auto drain_workers = [&]() {
    const auto deadline = GetTickCount64() + 10000;
    for (;;) {
      profile::Tick();
      manager::Pump();
      bool preset_busy;
      {
        std::shared_lock lock(manager::worker.mutex);
        preset_busy = manager::worker.busy;
      }
      if (!profile::Busy() && !preset_busy) break;
      check(GetTickCount64() < deadline, "snapshot fixture workers did not finish");
      Sleep(1);
    }
    check(!renodx::addons::dlss5::unicode_input::Busy(), "snapshot must not launch native input worker");
  };
  const auto settle = [&]() { frame(); frame(); frame(); };
  const auto custom_slot_button = [&](int slot) {
    const auto* window = find("open_presets").window;
    const auto scope = ImHashStr("nr_profiles", 0, ImHashStr("presets", 0, window->ID));
    const auto id = ImHashStr("##custom_slot", 0, ImHashData(&slot, sizeof(slot), scope));
    check(items.contains(id), "snapshot requires the actual fixed-ID custom slot");
    return Item(items.at(id));
  };
  for (const auto& scenario : scenarios) {
    if (std::find(registry["expected"].begin(), registry["expected"].end(), scenario.id)
        == registry["expected"].end()) continue;
    const auto profile_state_before = profile::state;
    if (!ImGui::GetCurrentContext()->OpenPopupStack.empty()) ImGui::ClosePopupToLevel(0, true);
    io.AddMousePosEvent(-100.f, -100.f);
    runtime::ui_theme_choice = scenario.theme;
    runtime::ui_language_choice = scenario.language;
    runtime::ui_menu_page = scenario.page;
    runtime::ui_selected_pass = (scenario.action == "pass-first" ? 0 : 1);
    runtime::hook_point = runtime::kHookPresent;
    runtime::stack_passes = 4;
    runtime::preset = 0;
    renodx::addons::dlss5::gpu_timers::comparison.Reset();
    renodx::addons::dlss5::gpu_timers::comparison_result = {};
    if (scenario.action == "gpu-comparison") {
      runtime::hook_point = runtime::kHookUpscaled;
      runtime::stack_passes = 2;
      runtime::vulkan_present_seen = false;
      runtime::stage_source_rr = false;
    }
    if (scenario.action == "gpu-pass-diagnostics" || scenario.action == "status-details") {
      namespace timers = renodx::addons::dlss5::gpu_timers;
      for (auto& timing : timers::stage_stats) timing = {};
      auto& sample = timers::stage_stats[1];
      sample.total_us = 1500;
      sample.eval_us = 1000;
      sample.own_us = 500;
      sample.width = 3840;
      sample.height = 2160;
      sample.valid = true;
      sample.state = timers::SampleState::VALID;
      sample.samples = 4;
      sample.measured_ns = timers::NowNs();
      sample.passes[0] = {.id = 0, .model = 1, .width = 3840, .height = 2160,
          .output_width = 3840, .output_height = 2160, .encode_us = 60, .eval_us = 750,
          .resolve_us = 100, .look_us = 40, .total_us = 930, .look_measured = true,
          .state = timers::SampleState::VALID};
      sample.passes[1] = {.id = 1, .model = 2, .width = 2573, .height = 1447,
          .output_width = 2573, .output_height = 1447, .encode_us = 30, .eval_us = 250,
          .resolve_us = 80, .total_us = 380, .state = timers::SampleState::VALID};
      sample.passes[2].id = 2;  // Planned but not executed: never draw a zero timing.
      timers::stage_stats[2].state = timers::SampleState::REPLAYED;
      timers::stats = {};
    }
    for (uint32_t pass = 0; pass < 3; ++pass) {
      runtime::pass_stage[pass] = 0;
      runtime::pass_preset[pass] = 0;
      runtime::pass_resolution_mode[pass] = 0;
      runtime::pass_resolution_scale[pass] = 1.f;
    }
    const bool render_stage_resolution = scenario.action == "render-stage-resolution";
    const bool present_stage_resolution = scenario.action == "present-stage-resolution";
    const bool same_stage_resolution = scenario.action == "same-stage-resolution"
        || render_stage_resolution || present_stage_resolution;
    const bool cross_stage = scenario.action.starts_with("cross-stage") || same_stage_resolution;
    const bool collapsed = scenario.action == "collapsed" || scenario.action.ends_with("-collapsed")
        || scenario.action.starts_with("preset-") || scenario.action.starts_with("auxiliary-");
    if (cross_stage) {
      runtime::hook_point = runtime::kHookRender;
      runtime::stack_passes = 2;
      runtime::preset = 1;
      runtime::pass_stage[0] = 2;
      runtime::pass_preset[0] = 2;
      if (scenario.action == "cross-stage-resolution" || same_stage_resolution) {
        runtime::pass_resolution_mode[0] = 2;
        runtime::pass_resolution_scale[0] = .67f;
      }
      if (same_stage_resolution) {
        runtime::hook_point = (render_stage_resolution ? runtime::kHookRender : runtime::kHookUpscaled);
        if (present_stage_resolution) runtime::hook_point = runtime::kHookPresent;
        runtime::pass_stage[0] = 0;
        runtime::nr_follow_input_res = false;
        runtime::nr_resolution_scale = 1.f;
      }
      if (scenario.action == "cross-stage-four") {
        runtime::stack_passes = 4;
        runtime::pass_stage[0] = 0;
        runtime::pass_stage[1] = 2;
        runtime::pass_preset[1] = 1;
        runtime::pass_preset[2] = 2;
        runtime::ui_selected_pass = 2;
      } else if (scenario.action == "cross-stage-backward") {
        runtime::hook_point = runtime::kHookUpscaled;
        runtime::pass_stage[0] = 1;
      } else if (scenario.action == "cross-stage-present") {
        runtime::hook_point = runtime::kHookUpscaled;
        runtime::pass_stage[0] = 3;
      }
      const auto plan = runtime::RequestedStagePlan();
      namespace stages = renodx::addons::dlss5::stage_plan;
      if (scenario.action == "cross-stage-backward" || scenario.action == "cross-stage-present") {
        check(!plan.valid && plan.reason == (scenario.action == "cross-stage-backward"
                  ? stages::Reason::BACKWARD_STAGE : stages::Reason::MIXED_PRESENT),
              "unsupported snapshot plan must preserve and explain the requested configuration");
      } else if (same_stage_resolution) {
        check(plan.valid && !plan.mixed && plan.group_count == 1 && plan.groups[0].count == 2,
              "same-stage resolution snapshot keeps one group and two logical passes");
        const auto expected_stage = (present_stage_resolution ? stages::Stage::PRESENT
            : (render_stage_resolution ? stages::Stage::RENDER : stages::Stage::UPSCALED));
        check(plan.groups[0].stage == expected_stage,
              "same-stage resolution editor preserves the requested stage");
      } else {
        check(plan.valid && plan.mixed && plan.group_count == 2,
              "representative cross-stage snapshot requires a valid two-stage plan");
        if (scenario.action == "cross-stage-four") {
          check(plan.groups[0].count == 2 && plan.groups[1].count == 2,
                "four-pass snapshot must show two Render and two Upscaled passes");
        }
      }
    }
    profile::active = -1;
    panel_width = scenario.width;
    panel_height = (scenario.action == "viewport" ? 900.f : 1100.f);
    font_size = scenario.font;
    io.DisplaySize = ImVec2(panel_width, panel_height);
    for (auto* window : ImGui::GetCurrentContext()->Windows) {
      window->StateStorage.Clear();
      ImGui::SetScrollY(window, 0.f);
    }
    for (auto& scope : ui::fold_scopes) scope = {};
    settle();
    drain_workers();
    if (scenario.action != "doc-default") {
      // The manager inherits the page's fold scope; keep its custom tools folded until explicitly clicked.
      click(find(collapsed ? "collapse_all" : "expand_all"));
    }
    settle();
    const bool long_page = scenario.action == "expanded" || scenario.action == "collapsed"
        || scenario.action == "pass-independent" || scenario.action == "pass-first" || scenario.action == "doc-default"
        || cross_stage || scenario.action == "gpu-pass-diagnostics" || scenario.action == "gpu-comparison";
    if (scenario.action == "cross-stage-resolution" || same_stage_resolution) {
      check(items.contains(find("##pass_resolution_scale").id), "independent pass resolution slider is drawn");
      check(runtime::pass_resolution_mode[0].load() == 2
                && std::abs(runtime::pass_resolution_scale[0].load() - .67f) < .0001f,
            "drawing the resolution editor preserves its independent mode and scale");
      check(runtime::pass_stage[0].load() == (same_stage_resolution ? 0u : 2u) && runtime::pass_preset[0].load() == 2,
            "resolution editor preserves Stage and Model selection");
    }
    ImGuiWindow* settings = nullptr;
    if (long_page) {
      bool fitted = false;
      for (int attempt = 0; attempt < 12; ++attempt) {
        settle();
        for (auto* window : ImGui::GetCurrentContext()->Windows) {
          if (window->Active && std::strstr(window->Name, "/##nr_settings_") != nullptr
              && std::strstr(window->Name, "/gpu_comparison_") == nullptr) settings = window;
        }
        check(settings != nullptr, "snapshot production settings child missing");
        // Measure the host's complete chrome budget, including spacing below BeginChild.
        const float desired = std::ceil(panel_height - settings->InnerRect.GetHeight()
                                       + settings->ContentSize.y + settings->WindowPadding.y * 2.f + font_size);
        check(desired < 24000.f, "snapshot content exceeds bounded virtual viewport");
        if (std::abs(panel_height - desired) < 2.f && settings->ScrollMax.y <= 1.f) {
          fitted = true;
          break;
        }
        panel_height = desired;
        io.DisplaySize = ImVec2(panel_width, panel_height);
        ImGui::SetScrollY(settings, 0.f);
      }
      check(fitted && settings->Scroll.y <= 1.f, "full-height snapshot did not settle without scrolling");
      check(settings->DC.CursorMaxPos.y <= settings->InnerRect.Max.y + 1.f, "snapshot bottom controls are clipped");
      unsigned fold_count = 0;
      for (const auto& [id, item] : items) {
        if ((item.status & ImGuiItemStatusFlags_Openable) == 0) continue;
        ++fold_count;
        if (scenario.action != "doc-default") {
          check(((item.status & ImGuiItemStatusFlags_Opened) != 0) == !collapsed,
                "snapshot tree/header disagrees with real expand/collapse command");
        }
      }
      const int scope = scenario.page * 8 + (scenario.page == 0 ? static_cast<int>(runtime::hook_point.load())
                                           : scenario.page == 1 ? runtime::ui_selected_pass : 0);
      if (scenario.action != "doc-default") {
        check(ui::fold_scopes[scope].target == !collapsed, "snapshot fold scope target mismatch");
      }
      std::printf("snapshot %s height=%.0f fold_nodes=%u\n", scenario.id.c_str(), panel_height, fold_count);
    }
    if (scenario.action == "viewport") {
      bool scroller = false;
      for (auto* window : ImGui::GetCurrentContext()->Windows) {
        if (window->Active && std::strstr(window->Name, "/##nr_settings_") != nullptr
            && std::strstr(window->Name, "/gpu_comparison_") == nullptr) {
          scroller = window->ScrollbarY && window->ScrollMax.y > 0.f;
        }
      }
      check(scroller, "fixed viewport must expose the production vertical scroller");
    }
    std::string interaction = (scenario.action == "doc-default" ? "production default folds" : "real expand_all/collapse_all button");
    const ui::StatusCard* diagnostic = nullptr;
    const ui::StatusCard card{
        .tone = (scenario.action == "diagnostics-error" ? ui::CardTone::kBad
                 : scenario.action == "diagnostics-warning" ? ui::CardTone::kWarn : ui::CardTone::kGood),
        .title = "Synthetic diagnostic fixture",
        .body = "Deterministic test state; not a game measurement or runtime failure.",
        .fix = {"Synthetic repair instruction"}, .fix_count = 1};
    if (scenario.action == "status-details") {
      click(find("status_details"));
      check(!find("status_close").disabled && !find("status_open_diagnostics").disabled,
            "runtime details must open without changing the selected page");
      for (const char* id : {"status_close", "status_open_diagnostics"}) {
        const Item action = find(id);
        check(action.rect.Min.y >= action.window->InnerRect.Min.y && action.rect.Max.y <= action.window->InnerRect.Max.y,
              "runtime detail navigation must remain visible outside the scrolling status body");
      }
      interaction = "click real runtime details; GPU-complete data is synthetic";
    } else if (scenario.action.starts_with("diagnostics-")) {
      diagnostic = &card;
      frame(diagnostic); frame(diagnostic); frame(diagnostic);
      interaction = "production DrawRuntimeStatus with explicitly synthetic card";
    } else if (scenario.action.starts_with("slot-") || scenario.action == "official-context") {
      const int slot = 3;
      profile::state.slots[slot].populated = true;
      profile::state.slots[slot].name = (scenario.language == 14 ? "我的夜景方案（自定义）" : "My night profile");
      profile::state.slots[slot].snapshot = profile::Snapshot();
      profile::active = slot;
      profile::observed_snapshot = profile::Snapshot();
      profile::state.selected = std::string(profile::data::SLOT_IDS[slot]);
      settle();
      Item target = (scenario.action == "official-context" ? find(ui::Tr("NR Lite")) : custom_slot_button(slot));
      const auto center = target.rect.GetCenter();
      const int before = profile::active;
      io.AddMousePosEvent(center.x, center.y);
      if (scenario.action == "slot-hover") {
        for (int tick = 0; tick < 45; ++tick) frame();
      } else {
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, true); frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, false); frame(); settle();
        check(profile::active == before, "right-click snapshot must never switch the selected slot");
        const Item first_action = find(scenario.action == "official-context" ? "restore_profile" : "Rename slot");
        check(first_action.window->Size.x >= (std::min)(font_size * 12.f, io.DisplaySize.x - font_size * 2.f),
              "wrapped slot menus require a readable width rather than an auto-fit feedback collapse");
        const char* action = nullptr;
        if (scenario.action == "slot-rename") action = "Rename slot";
        if (scenario.action == "slot-copy") action = "Copy from official profile";
        if (scenario.action == "slot-export") action = "Export profile";
        if (scenario.action == "slot-import") action = "Import profile";
        if (scenario.action == "slot-clear") action = "Clear slot";
        if (action != nullptr) {
          click(find(action));
          drain_workers(); settle();
          check(!find("close_slot_operation").disabled, "compact slot task must open without the library");
        }
      }
      interaction = "synthetic populated slot; real right-click/hover and compact task; no file or slot write";
    } else if (scenario.action == "custom-slot-create") {
      const int slot = 3;
      check(!profile::state.slots[slot].populated, "creation snapshot requires an empty synthetic slot");
      click(custom_slot_button(slot));
      check(!find("create_slot_current").disabled && !find("cancel_create_slot").disabled,
            "empty custom slot creation popup did not open");
      interaction = "click real empty Custom 1 slot; no capture/copy/save action";
    } else if (scenario.action == "restore-all-confirmation") {
      for (auto* window : ImGui::GetCurrentContext()->Windows) {
        if (window->Active && std::strstr(window->Name, "/##nr_settings_") != nullptr
            && std::strstr(window->Name, "/gpu_comparison_") == nullptr) {
          ImGui::SetScrollY(window, window->ScrollMax.y);
        }
      }
      settle();
      click(find(ui::Tr(ui::text::kRestoreAll)));
      check(!find(ui::Tr("Restore")).disabled && !find(ui::Tr("Cancel")).disabled,
            "restore-all inline confirmation did not open");
      interaction = "click restore-all action inside expanded section; no restore";
    } else if (scenario.action == "issue-report-description") {
      click(find(ui::Tr("I'm having issues...")));
      check(runtime::issue_report::step.load() == runtime::issue_report::Step::kDescribe
                && !runtime::issue_report::worker_active.load(), "report description started an unexpected worker");
      click(find("##issue_description"));
      auto* report_window = find("##issue_description").window;
      auto* parent = report_window->ParentWindow;
      check(parent != nullptr, "report description parent scroller missing");
      ImGui::SetScrollY(parent, parent->Scroll.y + report_window->Pos.y - parent->InnerRect.Min.y);
      settle();
      check(items.contains(find("##issue_steps").id), "report description steps field missing");
      interaction = "click report entry and description field; first step only, no collection or network";
    } else if (scenario.action == "restore-confirmation") {
      profile::active = 0;
      profile::state.slots[0].overrides["NRIntensity"] = 0.625f;
      settle();
      click(find("profile_actions"));
      click(find("restore_profile"));
      check(!find("confirm_restore").disabled, "restore confirmation did not open");
      interaction = "click profile_actions, restore_profile; no restore performed";
    } else if (scenario.action.starts_with("preset-") || scenario.action.starts_with("auxiliary-")) {
      manager::Navigate(0);
      manager::save_options = manager::export_options = manager::import_path_options = false;
      manager::have_preview = false;
      manager::search[0] = '\0';
      std::strcpy(manager::name, "Synthetic snapshot profile");
      click(find("open_presets"));
      drain_workers();
      // These fields only describe the fixture. No file action uses this display-only synthetic destination.
      manager::library_path = L"X:/NR Nexus synthetic library";
      std::strcpy(manager::path, "X:/NR Nexus synthetic library/Snapshot example.nrnexus.json");
      if (scenario.doc_role == "presets") {
        std::strcpy(manager::name, (scenario.language == 14 ? "中文电影画质" : "Snapshot example"));
      }
      settle();
      interaction = "click open_presets";
      if (scenario.action == "preset-selected" || scenario.action == "preset-rename"
          || scenario.action == "preset-delete-confirmation") {
        manager::files = {L"Synthetic snapshot A.nrnexus.json", L"Synthetic snapshot B.nrnexus.json"};
        manager::selected.clear();
        settle();
        click(find("##preset_file"));
        check(!manager::selected.empty(), "synthetic library file selection missing");
        interaction += ", select synthetic library row";
        if (scenario.action == "preset-rename") {
          click(find(std::string("##") + ui::Tr("Preset name")));
          io.AddKeyEvent(ImGuiMod_Ctrl, true);
          io.AddKeyEvent(ImGuiKey_A, true); frame();
          io.AddKeyEvent(ImGuiKey_A, false);
          io.AddKeyEvent(ImGuiMod_Ctrl, false); frame();
          io.AddInputCharactersUTF8("Synthetic renamed profile");
          settle();
          check(std::string(manager::name) == "Synthetic renamed profile", "inline rename draft missing");
          check(!find("rename_preset").disabled, "inline rename action unavailable");
          interaction += ", keyboard edit inline rename draft; no file operation";
        } else if (scenario.action == "preset-delete-confirmation") {
          check(find("delete_preset").disabled, "delete must start unconfirmed");
          click(find(ui::Tr("Confirm deletion of selected file")));
          check(manager::confirm_delete && !find("delete_preset").disabled, "delete consent did not enable action");
          interaction += ", click delete consent checkbox; no delete";
        }
      } else if (scenario.action == "preset-custom-slots") {
        click(find("custom_slot_tools"));
        check(!find("rename_slot").disabled, "custom slot management did not expand");
        interaction += ", click custom_slot_tools; no slot write";
      } else if (scenario.action != "preset-library") {
        const bool importing = scenario.action.starts_with("preset-import");
        click(find(importing ? "preset_import_task" : "preset_save_task"));
        interaction += (importing ? ", preset_import_task" : ", preset_save_task");
        if (scenario.action == "preset-import-preview" || scenario.action.starts_with("preset-import-scope")) {
          const bool full_scope = scenario.action.starts_with("preset-import-scope");
          const renodx::addons::dlss5::presets::Document document{
              .metadata = {{"name", "Synthetic snapshot import"}},
              .scope = (full_scope ? renodx::addons::dlss5::presets::Scope::FULL
                                  : renodx::addons::dlss5::presets::Scope::QUALITY_MULTIPASS),
              .settings = {{"NRIntensity", 1.25}}};
          check(runtime::CopyReportText(renodx::addons::dlss5::presets::Serialize(document)),
                "seed isolated synthetic clipboard");
          click(find("paste_preset"));
          drain_workers();
          settle();
          check(manager::have_preview, "synthetic pasted preset preview missing");
          interaction += ", paste_preset from test-only clipboard; no apply";
          if (full_scope) {
            check(find("apply_preset").disabled && !manager::confirm_full, "full import must start scope-gated");
            if (scenario.action == "preset-import-scope-confirmed") {
              click(find(ui::Tr("Confirm the full backup scope shown below")));
              check(manager::confirm_full && !find("apply_preset").disabled, "full import consent did not enable apply");
              interaction += ", click full backup scope consent; no apply";
            }
          }
        } else if (scenario.action == "preset-export" || scenario.action == "preset-export-actions") {
          click(find(ui::Tr("More options")));
          click(find("preview_export"));
          click(find(ui::Tr("Export file")));
          interaction += ", More options, preview_export, Export file (no write)";
          if (scenario.action == "preset-export-actions") {
            const auto action = find("export_preset");
            ImGui::SetScrollY(action.window, action.window->Scroll.y + action.rect.Max.y
                                              - action.window->InnerRect.Max.y + font_size);
            settle();
            const auto visible = find("export_preset");
            check(visible.window->InnerRect.Contains(visible.rect), "export action viewport clips the final button");
            interaction += ", scroll real manager content to reveal final export action";
          }
        } else if (scenario.action.starts_with("auxiliary-")) {
          const auto field = find(std::string("##") + ui::Tr("Preset name"));
          if (scenario.action == "auxiliary-context") {
            click(field);
            const auto center = items.at(field.id).rect.GetCenter();
            io.AddMousePosEvent(center.x, center.y);
            io.AddMouseButtonEvent(1, true); frame();
            io.AddMouseButtonEvent(1, false); settle();
            check(!find(ui::Tr("Assisted input...")).disabled, "assisted input context did not open");
            interaction += ", right click name field; native editor excluded";
          } else {
            const auto icon = ImHashStr("###assisted_input", 0, field.id);
            check(items.contains(icon), "assisted input icon missing");
            const auto center = items.at(icon).rect.GetCenter();
            io.AddMousePosEvent(center.x, center.y);
            for (int tick = 0; tick < 40; ++tick) frame();
            check(!tooltip_text.empty(), "assisted input tooltip missing");
            interaction += ", hover associated compatibility input icon";
          }
        }
      }
    } else if (scenario.action == "model-dropdown") {
      // The responsive production combo is used at its real narrow breakpoint.
      panel_width = 450.f;
      io.DisplaySize = ImVec2(panel_width, panel_height);
      settle();
      click(find("##model"));
      check(std::ranges::any_of(items, [](const auto& entry) { return entry.second.label == "B"; }),
            "model dropdown choices missing");
      interaction = "click responsive production model combo at 450px viewport";
    } else if (scenario.action == "model-tooltip") {
      panel_width = 1400.f;
      io.DisplaySize = ImVec2(panel_width, panel_height);
      settle();
      const auto target = find("A");
      io.AddMousePosEvent(target.rect.GetCenter().x, target.rect.GetCenter().y);
      for (int tick = 0; tick < 40; ++tick) frame();
      check(!tooltip_text.empty(), "model tooltip missing");
      interaction = "hover real model A control at segmented-control breakpoint";
    }
    if (scenario.action == "gpu-comparison") {
      namespace cmp = renodx::addons::dlss5::gpu_comparison;
      namespace timers = renodx::addons::dlss5::gpu_timers;
      const auto configuration_epoch = runtime::present_restore_configuration.epoch;
      const float comparison_height = find(ui::Tr("Capture baseline")).window->Size.y;
      check(find(ui::Tr("Capture comparison")).disabled, "B capture requires completed A");
      click(find(ui::Tr("Capture baseline")));
      check(timers::ComparisonActive() && !find(ui::Tr("Clear comparison")).disabled,
            "real baseline action starts a cancellable bounded capture");
      check(find(ui::Tr("Capture baseline")).disabled && find(ui::Tr("Capture comparison")).disabled,
            "capture actions cannot overlap");
      const auto fill = [&](bool baseline) {
        const uint64_t started = (baseline ? timers::comparison.baseline[1] : timers::comparison.control[1]).started_ns;
        for (uint32_t i = 0; i < cmp::SAMPLE_COUNT; ++i) {
          cmp::Sample sample{.stage = 1, .sequence = 1000u + i,
              .recorded_ns = started + cmp::STABILITY_NS + i * cmp::INTERVAL_NS,
              .valid = true, .stable = true,
              .identity = {.source_generation = 1, .device_session = 1, .queue = 1,
                  .sr_width = 2560, .sr_height = 1440, .out_width = 3840, .out_height = 2160},
              .pass_count = 2, .total_us = (baseline ? 1500.0 : 1300.0)};
          sample.completed_ns = sample.recorded_ns + 1;
          sample.passes[0] = {.id = 0, .model = 1, .width = 3840, .height = 2160,
              .output_width = 3840, .output_height = 2160, .eval_us = 700, .state = cmp::PassState::VALID};
          sample.passes[1] = {.id = 1, .model = 2, .width = (baseline ? 3840u : 2573u),
              .height = (baseline ? 2160u : 1447u), .output_width = (baseline ? 3840u : 2573u),
              .output_height = (baseline ? 2160u : 1447u), .eval_us = (baseline ? 400.0 : 200.0),
              .state = cmp::PassState::VALID};
          timers::comparison.Observe(sample, sample.completed_ns);
        }
        timers::comparison_result = timers::comparison.Results();
        settle();
      };
      fill(true);
      check(!find(ui::Tr("Capture comparison")).disabled, "completed A enables real B action");
      click(find(ui::Tr("Capture comparison")));
      fill(false);
      check(timers::comparison_result.reason == cmp::Reason::NONE
                && timers::comparison_result.stages[1].passes[1].delta_us == -200,
            "production comparison UI displays synthetic 100 to 67 percent GPU evidence");
      check(runtime::present_restore_configuration.epoch == configuration_epoch,
            "comparison actions never change processing or save presets");
      check(find(ui::Tr("Capture baseline")).window->Size.y == comparison_height,
            "comparison progress and result keep the same fixed height");
      for (const auto& timing : rendered_text) {
        if (timing.text != "0.700 ms" && timing.text != "0.400 ms" && timing.text != "0.200 ms") continue;
        for (const auto& size : rendered_text) {
          if (size.text != "3840x2160" && size.text != "2573x1447") continue;
          check(!timing.rect.Overlaps(size.rect), "GPU times and working dimensions must not overlap");
        }
      }
      interaction = "real A/B buttons; 16 synthetic completed samples per side; no game or performance claim";
    }
    capture(scenario.id.c_str(), diagnostic, false);
    Json regions = Json::object();
    if (scenario.action == "gpu-pass-diagnostics") {
      ImRect metrics(ImVec2(panel_width, panel_height), ImVec2(0.f, 0.f));
      bool inside = false;
      bool skipped = false;
      bool replayed = false;
      for (const auto& text : rendered_text) {
        if (text.text.starts_with("Pre-FG independent-resolution pair:")) inside = true;
        if (!inside) continue;
        metrics.Add(text.rect);
        skipped |= text.text.find("Pass 3: Not executed") != std::string::npos;
        replayed |= text.text.find("Rejected: command list replayed") != std::string::npos;
        if (replayed) break;
      }
      check(inside && skipped && replayed, "GPU diagnostics must draw valid, unexecuted and rejected samples");
      regions["gpu_pass_metrics"] = {metrics.Min.x - 4.f, metrics.Min.y - 4.f,
                                     (std::min)(panel_width, metrics.Max.x + 4.f), metrics.Max.y + 4.f};
    }
    for (auto* window : ImGui::GetCurrentContext()->Windows) {
      if (!window->Active) continue;
      if (scenario.action == "gpu-comparison" && std::strstr(window->Name, "/gpu_comparison_") != nullptr) {
        regions["gpu_comparison"] = {window->Pos.x, window->Pos.y,
            window->Pos.x + window->Size.x, window->Pos.y + window->Size.y};
      }
      std::string name;
      if (std::strstr(window->Name, "/##nr_settings_") != nullptr
          && std::strstr(window->Name, "/gpu_comparison_") == nullptr) name = "page_body";
      if ((window->Flags & ImGuiWindowFlags_Popup) != 0
          && std::strstr(window->Name, "nr_preset_manager") != nullptr
          && (window->Flags & ImGuiWindowFlags_ChildWindow) == 0) name = "library";
      if (!name.empty()) {
        const float bottom = (name == "page_body" && long_page
            ? (std::min)(window->InnerRect.Max.y, window->DC.CursorMaxPos.y + window->WindowPadding.y)
            : window->Pos.y + window->Size.y);
        regions[name] = {window->Pos.x, window->Pos.y, window->Pos.x + window->Size.x, bottom};
      }
    }
    for (const auto& [name, rect] : snapshot_regions) {
      regions[name] = {(std::max)(0.f, rect.Min.x - 4.f), (std::max)(0.f, rect.Min.y - 4.f),
                       (std::min)(panel_width, rect.Max.x + 4.f), (std::min)(panel_height, rect.Max.y + 4.f)};
    }
    if (!scenario.doc_role.empty()) {
      const std::string region = (scenario.doc_role == "gpu-comparison" ? "gpu_comparison"
                                  : scenario.doc_role == "results" ? "result_controls"
                                  : scenario.doc_role == "presets" ? "library" : "page_body");
      check(regions.contains(region), "documentation semantic crop region missing");
      regions["doc"] = regions[region];
      if (scenario.doc_role == "quality") {
        bool appearance_boundary = false;
        for (const auto& text : rendered_text) {
          if (text.window == settings && text.text == ui::Tr(ui::text::kLook)) {
            regions["doc"][3] = text.rect.Min.y - font_size * 0.25f;
            appearance_boundary = true;
            break;
          }
        }
        check(appearance_boundary, "quality crop requires the real appearance section boundary");
      } else if (scenario.doc_role.starts_with("theme-")) {
        check(regions.contains("model_primary"), "documentation model control region missing");
        regions["doc"][3] = regions["model_primary"][3];
      } else if (scenario.doc_role == "multipass") {
        check(regions.contains("chain") && regions.contains("pass_editor"), "documentation chain/editor region missing");
        ImRect feature_bounds(ImVec2(panel_width, panel_height), ImVec2(0.f, 0.f));
        for (const char* name : {"chain", "pass_editor", "result_controls"}) {
          check(regions.contains(name), "documentation pass feature region missing");
          feature_bounds.Add(ImVec2(regions[name][0].get<float>(), regions[name][1].get<float>()));
          feature_bounds.Add(ImVec2(regions[name][2].get<float>(), regions[name][3].get<float>()));
        }
        bool heading_found = false;
        for (const auto& text : rendered_text) {
          if (text.window == settings && text.text == ui::Tr(ui::text::kMenuPages[1])) {
            feature_bounds.Add(ImVec2(text.rect.Min.x, text.rect.Min.y - 4.f));
            heading_found = true;
            break;
          }
        }
        check(heading_found, "multipass crop requires its section heading above pass count");
        regions["doc"] = {feature_bounds.Min.x, feature_bounds.Min.y, feature_bounds.Max.x, feature_bounds.Max.y};
      }
      check(regions["doc"][2].get<float>() > regions["doc"][0].get<float>()
                && regions["doc"][3].get<float>() > regions["doc"][1].get<float>(), "documentation crop is empty");
      check(regions["doc"][0].get<float>() >= 0.f && regions["doc"][1].get<float>() >= 0.f
                && regions["doc"][2].get<float>() <= panel_width && regions["doc"][3].get<float>() <= panel_height,
            "documentation semantic crop extends outside the capture");
    }
    Json entry{{"id", scenario.id}, {"file", scenario.id + ".json"}, {"page", scenario.page},
               {"page_slug", pages[scenario.page]},
               {"category", (long_page ? "pages" : "interactions")}, {"state", scenario.action},
               {"theme", ui::CurrentTheme().slug}, {"language", scenario.language},
               {"language_code", ui::kLanguages[scenario.language - 1].code},
               {"font_size", font_size}, {"width", static_cast<int>(panel_width)}, {"height", static_cast<int>(panel_height)},
               {"kind", (long_page ? "full-page" : "viewport")}, {"interaction", interaction},
               {"regions", regions}, {"checks", {{"synthetic", true}, {"capture_bounds", true}}}};
    if (long_page) entry["checks"]["zero_scroll"] = true;
    if (cross_stage) {
      const auto plan = runtime::RequestedStagePlan();
      entry["plan"] = {{"requested", Json::array()}, {"models", Json::array()},
                        {"valid", plan.valid}, {"reason", renodx::addons::dlss5::stage_plan::ReasonText(plan.reason)},
                        {"execution_confirmed", false}};
      for (uint32_t pass = 0; pass < plan.pass_count; ++pass) {
        entry["plan"]["requested"].push_back(renodx::addons::dlss5::stage_plan::Name(plan.requested[pass]));
        entry["plan"]["models"].push_back(pass == 0 ? runtime::preset.load() : runtime::pass_preset[pass - 1].load());
      }
    }
    if (!scenario.doc_role.empty()) entry["doc_role"] = scenario.doc_role;
    if (scenario.action == "preset-export") {
      entry["limitations"] = {"Metadata/export options viewport; use Full preset-export-actions for the scrolled final action."};
    }
    std::ifstream input(scenario.id + ".json");
    Json mesh;
    input >> mesh;
    check(input.good() || input.eof(), "snapshot mesh metadata read failed");
    mesh["regions"] = regions;
    mesh["width"] = static_cast<int>(panel_width);
    mesh["height"] = static_cast<int>(panel_height);
    mesh["snapshot"] = entry;
    if (!scenario.doc_role.empty()) mesh["doc_role"] = scenario.doc_role;
    std::ofstream output(scenario.id + ".json", std::ios::trunc);
    output << mesh;
    output.close();
    check(static_cast<bool>(output), "snapshot mesh metadata write failed");
    registry["captures"].push_back(entry);
    if (scenario.action.starts_with("slot-") || scenario.action == "official-context") {
      profile::state = profile_state_before;
      profile::active = -1;
      profile::observed_snapshot = profile::Snapshot();
    }
    if (scenario.action == "custom-slot-create") {
      click(find("cancel_create_slot"));
      check(!profile::state.slots[3].populated, "creation snapshot must not populate the slot");
    } else if (scenario.action == "restore-all-confirmation") {
      click(find(ui::Tr("Cancel")));
    } else if (scenario.action == "issue-report-description") {
      runtime::CloseIssueReport();
      check(runtime::issue_report::step.load() == runtime::issue_report::Step::kClosed
                && !runtime::issue_report::worker_active.load(), "report description did not close without a worker");
    }
    if (!ImGui::GetCurrentContext()->OpenPopupStack.empty()) ImGui::ClosePopupToLevel(0, true);
  }
  drain_workers();
  clipboard_fixture::Empty();
  check(registry["captures"].size() == registry["expected"].size(), "snapshot registry incomplete");
  if (std::getenv("NR_NEXUS_TEST_CAPTURE_FAILURE") != nullptr) check(false, "controlled snapshot capture failure");
  registry["complete"] = true;
  std::ofstream manifest("captures.json");
  manifest << registry.dump(2) << '\n';
  manifest.close();
  check(static_cast<bool>(manifest), "snapshot registry write failed");
  std::printf("UI snapshots=%zu checks=%u shared_atlases=%u; synthetic real-production ImGui fixture\n",
              registry["captures"].size(), checks, atlas_count);
  ImGui::DestroyContext();
  return 0;
}
