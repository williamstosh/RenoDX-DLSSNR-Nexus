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
    for (const int language : {1, 14}) {
      const std::string suffix = (language == 1 ? "-en" : "-zh");
      for (const char* action : {"a-stage-preview", "a-stage-cancel", "a-stage-confirm", "a-stage-direct", "a-stage-escape",
                                 "a-stage-return-render", "a-stage-return-upscaled",
                                 "a-f6-on", "a-f6-off", "a-checking", "a-active"}) {
        scenarios.push_back({.id = std::string(action) + suffix, .page = 1, .action = action,
                             .language = language, .width = 1100.f});
      }
    }
    scenarios.push_back({.id = "a-stage-narrow", .page = 1, .action = "a-stage-preview", .width = 600.f, .font = 24.f});
    for (const int language : {1, 14}) {
      const std::string suffix = language == 1 ? "-en" : "-zh";
      for (const char* action : {"b-hud-compact", "b-hud-detailed", "b-hud-edit", "b-hud-hidden", "b-hud-keep", "b-hud-menuclose"}) {
        scenarios.push_back({.id = std::string(action) + suffix, .action = action, .language = language, .width = 1500.f});
      }
    }
    scenarios.push_back({.id = "b-hud-narrow", .action = "b-hud-compact", .width = 620.f, .font = 24.f});
    scenarios.push_back({.id = "b-hud-dpi", .action = "b-hud-detailed", .width = 1800.f, .font = 30.f});
    for (const int language : {1, 14}) {
      const std::string suffix = language == 1 ? "-en" : "-zh";
      scenarios.push_back({.id = "c-hud-single" + suffix, .action = "c-hud-single", .language = language, .width = 1500.f});
      scenarios.push_back({.id = "c-hud-pair" + suffix, .action = "c-hud-pair", .language = language, .width = 1500.f});
      scenarios.push_back({.id = "c-quality-narrow" + suffix, .action = "viewport", .language = language, .width = 640.f, .font = 24.f});
    }
    for (const char* action : {"r3-normal", "r3-yellow", "r3-pending", "r3-recovered",
                               "r3-pass1", "r3-pass2", "r3-pass4", "r3-monitor-open", "r3-monitor-close"}) {
      scenarios.push_back({.id = action, .action = action, .width = 1600.f});
    }
    scenarios.push_back({.id = "r3-monitor-narrow", .action = "r3-monitor-narrow", .width = 700.f});
    scenarios.push_back({.id = "r3-invalid-combination", .page = 1, .action = "cross-stage-backward"});
    scenarios.push_back({.id = "r3-hotkey", .page = 3, .action = "r3-hotkey"});
    scenarios.push_back({.id = "r3-fixed-scale", .action = "r3-fixed-scale"});
    scenarios.push_back({.id = "r3-custom-scale", .page = 1, .action = "r3-custom-scale"});
    for (const auto& theme : ui::kThemes) {
      for (const char* state : {"r3-normal", "r3-yellow", "r3-pending"}) {
        scenarios.push_back({.id = std::string(state) + "-" + theme.slug, .action = state,
                             .theme = theme.id, .width = 1600.f});
      }
    }
    scenarios.push_back({.id = "r2-quality", .action = "r2-quality"});
    scenarios.push_back({.id = "r2-quality-wide-en", .action = "r2-quality", .language = 1, .width = 1400.f});
    scenarios.push_back({.id = "r2-quality-follow", .action = "r2-follow"});
    scenarios.push_back({.id = "r2-pass1", .page = 1, .action = "r2-pass1"});
    scenarios.push_back({.id = "r2-pass2", .page = 1, .action = "r2-pass2"});
    scenarios.push_back({.id = "r2-pass3", .page = 1, .action = "r2-pass3"});
    scenarios.push_back({.id = "r2-pass4-blocked", .page = 1, .action = "r2-pass4"});
    for (const auto& theme : ui::kThemes) {
      scenarios.push_back({.id = std::string("r2-") + theme.slug + "-narrow", .action = "r2-follow",
                           .theme = theme.id, .width = 450.f});
    }
    for (int page : {0, 1, 3}) {
      scenarios.push_back({.id = std::string("r2-reset-") + pages[page], .page = page, .action = "r2-reset-confirm"});
    }
    scenarios.push_back({.id = "restore-all-confirmation", .page = 3, .action = "restore-all-confirmation"});
    scenarios.push_back({.id = "restore-all-confirmation-en-narrow", .page = 3,
                         .action = "restore-all-confirmation", .language = 1, .width = 450.f});
    scenarios.push_back({.id = "quality-viewport", .action = "viewport"});
    scenarios.push_back({.id = "status-details", .action = "status-details"});
    for (const char* action : {"slot-context", "official-context", "slot-rename", "slot-copy", "slot-export", "slot-import", "slot-clear"}) {
      scenarios.push_back({.id = action, .action = action});
    }
    for (const int language : {1, 14}) {
      const std::string suffix = language == 1 ? "-en" : "-zh";
      scenarios.push_back({.id = "c-slot-context-narrow" + suffix, .action = "slot-context", .language = language, .width = 640.f, .font = 24.f});
      scenarios.push_back({.id = "c-slot-rename-wide" + suffix, .action = "slot-rename", .language = language, .width = 1600.f});
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
                                  "Reset confirmations are production modals; the report description wizard remains inline.",
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
    monitor_fixture = nullptr;
    hud_fixture = false;
    independent_hud_fixture = false;
    runtime::monitor_requested = false;
    runtime::monitor_snapshot = {};
    runtime::monitor_observation_epoch.fetch_add(1);
    runtime::stage_edit::pending = false;
    runtime::stage_edit::error.clear();
    runtime::stage_edit::notice.clear();
    runtime::stage_edit::undo.clear();
    synthetic_monitor_state = {};
    runtime::ui_runtime_monitor_state.open = false;
    if (!ImGui::GetCurrentContext()->OpenPopupStack.empty()) ImGui::ClosePopupToLevel(0, true);
    io.AddMousePosEvent(-100.f, -100.f);
    runtime::ui_theme_choice = scenario.theme;
    runtime::ui_language_choice = scenario.language;
    runtime::ui_menu_page = scenario.page;
    runtime::ui_selected_pass = ((scenario.action == "pass-first" || scenario.action == "r2-pass1") ? 0 : 1);
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
    if (scenario.action.starts_with("a-")) {
      runtime::d3d12_swapchain_seen = true;
      runtime::d3d11_swapchain_seen = false;
      runtime::vulkan_present_seen = false;
      runtime::stage_source_rr = false;
      runtime::native_fg_declaration = runtime::NativeFgDeclaration::kOff;
      runtime::native_fg_public_off = true;
      runtime::direct_runtime_requirements.dual_models = true;
      runtime::hook_point = runtime::kHookRender;
      runtime::stack_passes = 2;
      runtime::pass_stage[0] = 2;
      runtime::ui_selected_pass = 0;
      runtime::nr_follow_input_res = false;
      runtime::nr_resolution_scale = 1.f;
    }
    if (scenario.action.starts_with("r2-")) {
      runtime::nr_follow_input_res = scenario.action == "r2-follow";
      runtime::nr_resolution_scale = .67f;
      runtime::hook_point = runtime::kHookRender;
      for (auto& observed : runtime::stage_telemetry) observed = {};
      for (auto& timing : renodx::addons::dlss5::gpu_timers::stage_stats) timing = {};
      if (scenario.action == "r2-pass2" || scenario.action == "r2-pass4") {
        runtime::pass_stage[0] = 2;
        runtime::pass_resolution_mode[0] = 2;
        runtime::pass_resolution_scale[0] = .58f;
      }
      if (scenario.action == "r2-pass4") {
        runtime::ui_selected_pass = 3;
        auto& observed = runtime::stage_telemetry[1];
        observed.groups = 1;
        observed.pass_begin = 1;
        observed.pass_count = 3;
        observed.width = 3840;
        observed.height = 2160;
        observed.resolution_blocked = true;
        observed.pass_resolutions[3].requested_width = 2573;
        observed.pass_resolutions[3].requested_height = 1447;
      }
      if (scenario.action == "r2-pass3") {
        runtime::ui_selected_pass = 2;
      }
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
    ui::MonitorInputs window_evidence;
    const bool synthetic_window = scenario.action.starts_with("r3-")
        && scenario.action != "r3-hotkey" && scenario.action != "r3-fixed-scale" && scenario.action != "r3-custom-scale";
    if (synthetic_window) {
      window_evidence.card = {.tone = ui::CardTone::kGood, .glyph = ui::CardGlyph::kCheck,
                              .title = "Neural Rendering is on"};
      window_evidence.current_conclusion = "Synthetic clean window";
      window_evidence.recent = {.available = true, .ready = true, .eligible = 120, .injected = 120,
                                .last_sample_ns = 1'000'000'000LL};
      window_evidence.now_ns = 1'500'000'000LL;
      window_evidence.pass_count = (scenario.action == "r3-pass1" ? 1u : (scenario.action == "r3-pass4" ? 4u : 2u));
      runtime::hook_point = runtime::kHookRender;
      runtime::stack_passes = window_evidence.pass_count;
      runtime::preset = 2;
      runtime::nr_follow_input_res = false;
      runtime::nr_resolution_scale = .67f;
      for (unsigned index = 0; index < window_evidence.pass_count; ++index) {
        auto& pass = window_evidence.passes[index];
        const char* stage = (index == 0 ? "Render" : "Upscaled");
        const char* model = (index == 0 ? "B" : "A");
        if (index != 0) {
          runtime::pass_stage[index - 1] = 2;
          runtime::pass_preset[index - 1] = 1;
        }
        pass = {.requested_stage = stage, .effective_stage = stage,
                .execution_proof = "Synthetic completed submission", .requested_model = model,
                .created_model_request = model, .requested_size = "67%",
                .actual_size = "2560x1440", .gpu_cost = "1.2 ms", .gpu_state = ui::MonitorGpuState::CURRENT};
      }
      window_evidence.gpu_scope = "Synthetic completed submission; not a performance measurement";
      window_evidence.current_limit = "Synthetic window evidence only; no game or GPU inference";
      if (scenario.action == "r3-yellow") {
        window_evidence.card.tone = ui::CardTone::kWarn;
        window_evidence.card.glyph = ui::CardGlyph::kWarning;
        window_evidence.recent.partial = true;
        window_evidence.recent.injected = 80;
        window_evidence.recent.gaps = 40;
        window_evidence.current_conclusion = "Synthetic partial window";
      } else if (scenario.action == "r3-pending") {
        window_evidence.card.tone = ui::CardTone::kIdle;
        window_evidence.recent = {};
        window_evidence.current_conclusion = "Synthetic pending window";
        for (auto& pass : window_evidence.passes) {
          pass.effective_stage.clear();
          pass.execution_proof.clear();
          pass.created_model_request.clear();
          pass.actual_size.clear();
          pass.gpu_cost.clear();
          pass.gpu_state = ui::MonitorGpuState::UNAVAILABLE;
        }
      }
      monitor_fixture = &window_evidence;
      synthetic_monitor_state.open = true;
    } else if (scenario.action == "r3-fixed-scale") {
      runtime::nr_follow_input_res = false;
      runtime::nr_resolution_scale = .58f;
    } else if (scenario.action == "r3-custom-scale") {
      runtime::hook_point = runtime::kHookRender;
      runtime::stack_passes = 2;
      runtime::pass_stage[0] = 2;
      runtime::nr_follow_input_res = false;
      runtime::nr_resolution_scale = .67f;
      runtime::pass_resolution_mode[0] = 2;
      runtime::pass_resolution_scale[0] = .63f;
    }
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
    if (scenario.action != "doc-default" && !synthetic_window) {
      // The manager inherits the page's fold scope; keep its custom tools folded until explicitly clicked.
      click(find(collapsed ? "collapse_all" : "expand_all"));
    }
    settle();
    const bool long_page = scenario.action == "expanded" || scenario.action == "collapsed"
        || scenario.action == "pass-independent" || scenario.action == "pass-first" || scenario.action == "doc-default"
        || cross_stage || scenario.action == "gpu-pass-diagnostics" || scenario.action == "gpu-comparison"
        || (scenario.action.starts_with("r2-") && scenario.action != "r2-reset-confirm");
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
        // This technical disclosure intentionally does not follow parameter folds.
        if (item.label == "##Recorded working sizes") continue;
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
    ui::StatusCard phase_a_card;
    if (scenario.action.starts_with("a-stage-")) {
      const bool returning = scenario.action.starts_with("a-stage-return-");
      if (returning) {
        runtime::hook_point = runtime::kHookPresent;
        runtime::pass_stage[0] = 3u;
        frame();
      }
      const auto original = profile::Snapshot();
      const char* target = (scenario.action == "a-stage-return-render" ? "Render"
          : scenario.action == "a-stage-direct" || scenario.action == "a-stage-return-upscaled" ? "Upscaled" : "Present");
      const bool segmented = std::ranges::any_of(items, [&](const auto& entry) { return entry.second.label == ui::Tr(target); });
      if (!segmented) click(find("##stage_value"));
      check(!find(ui::Tr(target)).disabled, "stage preview must be reachable through the real control");
      click(find(ui::Tr(target)));
      if (scenario.action == "a-stage-direct") {
        auto expected = original;
        expected["NRHookPoint"] = 0u;
        check(!runtime::stage_edit::pending && profile::Snapshot() == expected,
              "real Stage selection directly commits only the chosen pass when no collateral stage changes");
        check(!ImGui::IsPopupOpen("###nr_stage_change", ImGuiPopupFlags_AnyPopupId),
              "direct stage edit does not open a confirmation modal");
      } else {
        check(runtime::stage_edit::pending && profile::Snapshot() == original, "stage selection opens preview without mutation");
        check(!find("cancel_stage_change").disabled && !find("confirm_stage_change").disabled, "stage preview provides cancel and confirm");
        if (scenario.action == "a-stage-cancel") {
          click(find("cancel_stage_change"));
          check(!runtime::stage_edit::pending && profile::Snapshot() == original, "real Cancel keeps original configuration");
        } else if (returning) {
          const uint32_t hook = (scenario.action == "a-stage-return-render" ? 1u : 0u);
          auto expected = original;
          expected.update({{"NRHookPoint", hook}, {"NRPass2Stage", 2u}});
          click(find("confirm_stage_change"));
          check(!runtime::stage_edit::pending && profile::Snapshot() == expected,
                "real Present pair controls reach and commit Render/Upscaled candidates without altering other fields");
        } else if (scenario.action == "a-stage-confirm") {
          click(find("confirm_stage_change"));
          check(!runtime::stage_edit::pending && runtime::hook_point.load() == 2
                    && runtime::pass_stage[0].load() == 3, "real Confirm publishes the complete candidate chain");
        } else if (scenario.action == "a-stage-escape") {
          io.AddKeyEvent(ImGuiKey_Escape, true);
          frame();
          io.AddKeyEvent(ImGuiKey_Escape, false);
          frame();
          check(!runtime::stage_edit::pending && runtime::stage_edit::changes.empty()
                    && profile::Snapshot() == original, "Escape cancels the staged transaction without publishing settings");
        }
      }
      interaction = "real Stage selection and transaction modal; synthetic configuration only";
    } else if (scenario.action.starts_with("b-hud-") || scenario.action.starts_with("c-hud-")) {
      if (scenario.action == "c-hud-single") { runtime::stack_passes = 1; }
      if (scenario.action == "c-hud-pair") { runtime::stack_passes = 2; }
      const bool edit = scenario.action == "b-hud-edit";
      const bool closing = scenario.action == "b-hud-keep" || scenario.action == "b-hud-menuclose";
      runtime::ui_runtime_monitor_state = {.detailed = scenario.action == "b-hud-detailed"};
      runtime::ui_monitor_saved_detailed = runtime::ui_runtime_monitor_state.detailed;
      runtime::monitor_hotkey = VK_F11;
      runtime::monitor_key_edge = {};
      runtime::hud_enabled = false;
      runtime::hud_overlay_open = edit || closing;
      runtime::PollMonitorKey(false, true, edit || closing);
      runtime::PollMonitorKey(false, true, edit || closing);
      runtime::PollMonitorKey(true, true, edit || closing);
      check(runtime::monitor_requested.load(), "F11 opens the independent monitor without opening Home");
      if (closing) {
        const auto flush = runtime::flush_profile_storage;
        runtime::flush_profile_storage = nullptr;
        runtime::monitor_keep_visible = scenario.action == "b-hud-keep";
        runtime::OnReshadeOpenOverlay(nullptr, false, reshade::api::input_source::keyboard);
        runtime::flush_profile_storage = flush;
        check(runtime::monitor_requested.load() == runtime::monitor_keep_visible.load(), "Home close obeys the keep-visible option");
      }
      if (scenario.action == "b-hud-hidden") {
        runtime::PollMonitorKey(false, true, false);
        runtime::PollMonitorKey(true, true, false);
        check(!runtime::monitor_requested.load(), "the next F11 edge hides the monitor");
      }
      independent_hud_fixture = !edit;
      frame(); frame();
      if (runtime::monitor_requested.load()) {
        const auto* window = ImGui::FindWindowByName("###nr_runtime_monitor");
        check(window != nullptr && window->Active, "independent HUD is drawn with Home closed");
        if (!edit) check((window->Flags & ImGuiWindowFlags_NoInputs) == ImGuiWindowFlags_NoInputs,
            "gameplay HUD accepts neither mouse nor keyboard input");
      }
      interaction = "production F11 edge / Home visibility / independent draw; synthetic runtime, no game input or performance claim";
    } else if (scenario.action == "a-f6-on" || scenario.action == "a-f6-off") {
      const bool on = scenario.action == "a-f6-on";
      runtime::enabled = !on;
      runtime::hud_enabled = true;
      runtime::ToggleNrFromHotkey(VK_F6, true);
      check(runtime::enabled.load() == on && runtime::profile_feedback_until_ns.load() > runtime::SteadyNowNs(),
            "F6 operation feedback is immediate without any execution evidence");
      runtime::hud_overlay_open = true;
      hud_fixture = true;
      interaction = "production F6 handler and existing HUD draw; synthetic input, no game focus claim";
    } else if (scenario.action == "a-checking" || scenario.action == "a-active") {
      auto verdict = renodx::addons::dlss5::ComputeNrVerdict(runtime::CollectFunnelInputs());
      verdict.state = renodx::addons::dlss5::NrState::kEngaged;
      ui::CardInputs evidence{.verdict = verdict, .active_features = 1};
      evidence.recent = ui::RecentHealth{.gpu_completed = scenario.action == "a-active"};
      phase_a_card = ui::ComputeStatusCard(evidence);
      diagnostic = &phase_a_card;
      interaction = "synthetic checking / completed GPU evidence card; no measured performance values";
    } else if (synthetic_window) {
      if (scenario.action == "r3-recovered") {
        ui::RecentHealthWindow recent;
        std::uint64_t eligible = 0, injected = 0, gaps = 0;
        std::int64_t now = 1'000'000'000LL;
        recent.Observe(1, eligible, injected, gaps, now);
        for (int tick = 0; tick < 12; ++tick) {
          now += 100'000'000LL;
          eligible += 10; injected += 5; gaps += 5;
          recent.Observe(1, eligible, injected, gaps, now);
        }
        window_evidence.card.tone = ui::CardTone::kWarn;
        window_evidence.current_conclusion = "Synthetic partial window";
        settle();
        ui::RecentHealth clean;
        for (int tick = 0; tick < 40; ++tick) {
          now += 100'000'000LL;
          eligible += 10; injected += 10;
          clean = recent.Observe(1, eligible, injected, gaps, now);
        }
        check(clean.ready && !clean.partial && clean.gaps == 0, "synthetic recent window recovers despite historical gaps");
        window_evidence.card.tone = ui::CardTone::kGood;
        window_evidence.current_conclusion = "Synthetic recovered window; historical gaps retained";
        window_evidence.recent = {.available = true, .ready = clean.ready, .partial = clean.partial,
                                  .eligible = clean.eligible, .injected = clean.injected, .gaps = clean.gaps,
                                  .last_sample_ns = clean.last_sample_ns};
        window_evidence.now_ns = now;
        settle();
      } else if (scenario.action == "r3-monitor-close") {
        click(find("status_close"));
        check(!synthetic_monitor_state.open, "monitor close fixture uses the real close button");
      } else if (scenario.action == "r3-monitor-narrow") {
        click(find("nr_monitor_inline"));
        check(!find("status_close").disabled, "narrow monitor disclosure retains close action");
      }
      interaction = "production summary/monitor widgets with explicitly synthetic window evidence; no game, network or GPU validation";
    } else if (scenario.action == "r3-fixed-scale") {
      check(!find("##resolution_scale").disabled && std::abs(runtime::nr_resolution_scale.load() - .58f) < .001f,
            "fixed-scale scene retains the saved editable value");
      interaction = "production fixed-scale editor; synthetic fixture configuration";
    } else if (scenario.action == "r3-custom-scale") {
      check(!find("##pass_resolution_scale").disabled && std::abs(runtime::pass_resolution_scale[0].load() - .63f) < .001f,
            "independent custom scale stays editable without changing stored encoding");
      check(std::ranges::any_of(rendered_text, [](const auto& text) { return text.text == ui::Tr("Custom"); }),
            "non-preset independent scale is labeled Custom");
      interaction = "production independent 100/67/58/50 shortcuts and Custom 63% slider; synthetic configuration";
    } else if (scenario.action == "r3-hotkey") {
      check(find("NRToggleKey").id != 0 && find("NRScreenshotKey").id != 0,
            "advanced hotkey controls are registered");
      interaction = "production hotkey controls; bindings are not polled or saved by the snapshot";
    } else if (scenario.action == "r2-reset-confirm") {
      click(find("reset_page"));
      check(!find("cancel_page_reset").disabled, "shared page reset confirmation is accessible");
      interaction = "real reset-page command; confirmation only, no persistence or reset";
    } else if (scenario.action == "status-details") {
      click(find("status_details"));
      if (!runtime::ui_runtime_monitor_state.detailed) click(find("monitor_details"));
      const auto* monitor = ImGui::FindWindowByName("###nr_runtime_monitor");
      check(monitor != nullptr && monitor->Active && (monitor->Flags & ImGuiWindowFlags_ChildWindow) == 0,
            "narrow runtime details remain an independent window");
      check(!find("status_close").disabled && !find("status_open_diagnostics").disabled,
            "runtime details must open without changing the selected page");
      for (const char* id : {"status_close", "status_open_diagnostics"}) {
        const Item action = find(id);
        check(action.rect.Min.y >= action.window->InnerRect.Min.y && action.rect.Max.y <= action.window->InnerRect.Max.y,
              "runtime detail navigation must remain visible outside the scrolling status body");
      }
      interaction = "click real independent monitor entry and detailed view; GPU-complete data is synthetic";
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
        // Measure the popup rendered with its locked legacy atlas. CalcTextSize
        // after Render() can bake a new size and invalidate that atlas between frames.
        check(first_action.window->ContentSize.x <= first_action.window->InnerRect.GetWidth() + 1.f,
              "slot menu width accommodates its translated action within the display");
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
      click(find("open_reset_all"));
      check(!find("cancel_page_reset").disabled, "restore-all protected confirmation did not open");
      interaction = "click restore-all action and open shared confirmation; no restore";
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
      const auto lite_center = find(ui::Tr("NR Lite")).rect.GetCenter();
      io.AddMousePosEvent(lite_center.x, lite_center.y); frame();
      io.AddMouseButtonEvent(1, true); frame();
      io.AddMouseButtonEvent(1, false); frame(); settle();
      click(find("restore_profile"));
      check(!find("confirm_restore").disabled, "restore confirmation did not open");
      interaction = "right-click official slot and restore_profile; no restore performed";
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
    if (scenario.action.find("tooltip") == std::string::npos) {
      io.AddMousePosEvent(-100.f, -100.f);
      frame(diagnostic);
      frame(diagnostic);
    }
    if (hud_fixture) {
      const auto* foreground = ImGui::GetForegroundDrawList(ImGui::GetMainViewport());
      const auto* data = ImGui::GetDrawData();
      check(foreground->VtxBuffer.Size != 0 && data->CmdListsCount != 0
                && data->CmdLists[data->CmdListsCount - 1] == foreground,
            "F6 notice must reach the foreground mesh above the menu, not only submit hidden text");
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
    monitor_fixture = nullptr;
    hud_fixture = false;
    independent_hud_fixture = false;
    runtime::monitor_requested = false;
    runtime::monitor_keep_visible = true;
    runtime::hud_enabled = true;
    runtime::hud_overlay_open = false;
    runtime::profile_feedback_until_ns = 0;
    runtime::stage_edit::pending = false;
    runtime::ui_runtime_monitor_state.open = false;
    if (scenario.action.starts_with("r2-") || scenario.action == "r3-fixed-scale") {
      runtime::nr_follow_input_res = false;
      runtime::nr_resolution_scale = 1.f;
    }
    if (scenario.action == "custom-slot-create") {
      click(find("cancel_create_slot"));
      check(!profile::state.slots[3].populated, "creation snapshot must not populate the slot");
    } else if (scenario.action == "restore-all-confirmation") {
      click(find("cancel_page_reset"));
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
