/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Included after the production menu's frame, click, fold and chain probes.
{
  namespace profile = runtime::profile_ui;
  std::vector<std::pair<const runtime::Setting*, float>> saved_settings;
  for (const auto group : runtime::kSettingSections) {
    for (const auto* setting : group) saved_settings.emplace_back(setting, setting->Get());
  }
  const auto saved_folds = ui::fold_scopes;
  const auto saved_width = panel_width;
  const auto saved_height = panel_height;
  const auto saved_font = font_size;
  const auto saved_display = io.DisplaySize;
  const auto saved_page = runtime::ui_menu_page;
  const auto saved_pass = runtime::ui_selected_pass;
  const auto saved_language = runtime::ui_language_choice.load();
  const auto saved_active = profile::active;
  profile::active = -1;
  runtime::ui_language_choice = 1;
  runtime::ui_menu_page = 0;
  runtime::ui_selected_pass = 0;
  runtime::hook_point = runtime::kHookRender;
  runtime::stack_passes = 4;
  runtime::preset = 1;
  runtime::nr_follow_input_res = false;
  runtime::nr_resolution_scale = .67f;
  runtime::look_mode = false;
  runtime::kLookResult[0].detail_stability.Store(0.f);
  panel_height = 900.f;
  font_size = 18.f;
  for (unsigned pass = 0; pass < 3; ++pass) {
    runtime::pass_stage[pass] = 0;
    runtime::pass_preset[pass] = 0;
    runtime::pass_resolution_mode[pass] = 0;
    runtime::pass_follow[pass] = false;
    runtime::pass_look_result[pass].follow = false;
    runtime::pass_look_result[pass].mode = false;
  }
  const auto text_item = [&](const char* label) {
    for (const auto& text : rendered_text) {
      if (text.text == ui::Tr(label)) return text;
    }
    throw std::runtime_error(std::string("r7 missing production text: ") + label);
  };
  const auto settings_window = [&]() {
    for (auto* window : ImGui::GetCurrentContext()->Windows) {
      if (window->Active && std::strstr(window->Name, "/##nr_settings_") != nullptr
          && std::strstr(window->Name, "/gpu_comparison_") == nullptr) return window;
    }
    throw std::runtime_error("r7 production settings scroller missing");
  };
  for (float width : {450.f, 600.f}) {
    panel_width = width;
    frame(); frame();
    click(find("collapse_all"));
    ImGui::SetScrollY(settings_window(), 0.f);
    frame(); frame();
    const auto core = text_item("Core quality");
    const auto processing = text_item("Processing & compatibility");
    const auto appearance = text_item("Appearance & result");
    check(core.rect.Max.y < processing.rect.Min.y && processing.rect.Max.y < appearance.rect.Min.y,
          "r7 quality has three ordered always-open groups instead of flat peer captions");
    const auto model = text_item("NR model");
    const auto strength = text_item(ui::text::kModelStrength);
    const auto resolution = text_item(ui::text::kResolution);
    check(model.rect.Min.y > core.rect.Max.y && model.rect.Max.y < processing.rect.Min.y
              && strength.rect.Min.y > core.rect.Max.y && strength.rect.Max.y < processing.rect.Min.y
              && resolution.rect.Min.y > core.rect.Max.y && resolution.rect.Max.y < processing.rect.Min.y,
          "r7 model, strength and working resolution belong to core quality");
    check(std::ranges::count_if(rendered_text, [](const auto& text) {
      return text.text == ui::Tr(ui::text::kResolution);
    }) == 1, "r7 working resolution has one label instead of a repeated section title");
    for (const char* label : {"Local tone", ui::text::kModelStructure, "Character / skin", "Character mask",
                              "Independent skin", "UI correction", "Adjust NR's look", ui::text::kResultStrength}) {
      check(text_item(label).rect.Min.y > appearance.rect.Max.y,
            "r7 common appearance and result controls remain available when parameter folds are closed");
    }
    const bool resolution_dropdown = std::ranges::any_of(items, [](const auto& entry) {
      return entry.second.label == "##resolution";
    });
    if (resolution_dropdown) click(find("##resolution"));
    for (const char* id : {"nr_resolution_full", "nr_resolution_quality", "nr_resolution_balanced",
                           "nr_resolution_performance", "nr_resolution_follow"}) {
      check(find(id).id != 0, "r7 narrow quality retains all common resolution controls");
    }
    if (resolution_dropdown) {
      click(Item{.rect = core.rect, .window = core.window});
    }
    check(find("##resolution_scale").id != 0, "r7 narrow quality retains the custom resolution slider");
    check(settings_window()->ScrollbarY && settings_window()->ScrollMax.y > 0.f,
          "r7 narrow common quality exposes a real vertical scroller");
    const auto unchanged = profile::Snapshot();
    click(find("expand_all"));
    check(fold_open(ui::text::kResultLimits) && fold_open(ui::text::kResultTemporal),
          "r7 expand reaches result limits and temporal controls below all three quality groups");
    click(find("collapse_all"));
    check(!fold_open(ui::text::kResultLimits) && !fold_open(ui::text::kResultTemporal)
              && profile::Snapshot() == unchanged,
          "r7 collapse changes disclosure state without changing quality settings");
    check(text_item("Core quality").rect.Min.y < text_item("Appearance & result").rect.Min.y,
          "r7 bulk collapse keeps quality hierarchy visible");
  }

  panel_width = 1000.f;
  panel_height = 1300.f;
  io.DisplaySize = ImVec2(1400.f, 1600.f);
  runtime::ui_menu_page = 1;
  for (unsigned pass = 0; pass < 4; ++pass) {
    ui::fold_scopes[8 + pass] = {};
    ui::fold_scopes[8 + pass].epoch = 10 + pass;
    ui::fold_scopes[8 + pass].target = pass % 2 == 0;
  }
  frame(); frame();
  const auto select_pass = [&](int pass) {
    ImGui::SetScrollY(settings_window(), 0.f);
    frame(); frame();
    const auto node = chain_nodes().at(pass);
    const auto source_pass = runtime::ui_selected_pass;
    const auto configuration = profile::Snapshot();
    const auto epoch = runtime::profile_commit_epoch.load();
    const auto writes = configuration_writes;
    const auto source_nodes = ui::fold_scopes[8 + source_pass].nodes;
    io.AddMousePosEvent(node.rect.GetCenter().x, node.rect.GetCenter().y);
    frame();
    io.AddMouseButtonEvent(0, true);
    frame();
    io.AddMouseButtonEvent(0, false);
    frame();
    // No settling frame: the releasing click must transfer fold ownership now.
    check(runtime::ui_selected_pass == pass, "r7 real chain click selects the requested pass in its release frame");
    const auto basic = find(ui::text::kResultBasic);
    check(ui::fold_scopes[8 + pass].nodes.contains(basic.id),
          "r7 newly selected editor registers folds in its own scope during the same frame");
    check(fold_open(ui::text::kResultBasic) == ui::fold_scopes[8 + pass].target,
          "r7 destination applies its pending fold command before drawing the selected editor");
    bool source_unchanged = ui::fold_scopes[8 + source_pass].nodes.size() == source_nodes.size();
    for (const auto& [id, state] : source_nodes) {
      const auto found = ui::fold_scopes[8 + source_pass].nodes.find(id);
      source_unchanged &= found != ui::fold_scopes[8 + source_pass].nodes.end()
          && found->second.epoch == state.epoch && found->second.seen == state.seen;
    }
    check(source_unchanged, "r7 pass switch does not register destination folds in the previous pass scope");
    check(profile::Snapshot() == configuration && runtime::profile_commit_epoch.load() == epoch
              && configuration_writes == writes,
          "r7 real pass navigation changes no configuration, commit epoch or persisted value");
    char heading[64];
    std::snprintf(heading, sizeof(heading), "Editing Pass %u", static_cast<unsigned>(pass + 1));
    const auto title = text_item(heading);
    const auto run = text_item("Run settings");
    const auto appearance = text_item("Appearance & inheritance");
    const auto result_count = std::size(runtime::kPassLookResultSettings) / 3;
    const bool result_changed = (pass == 0 ? runtime::AnyModified({runtime::kLookResultSettings})
        : runtime::AnyModified({runtime::SettingGroup(runtime::kPassLookResultSettings)
            .subspan((pass - 1) * result_count, result_count)}));
    const auto result = text_item(result_changed ? ui::text::kResultChanged : ui::text::kResultShaping);
    check(title.rect.Max.y < run.rect.Min.y && run.rect.Max.y < appearance.rect.Min.y
              && appearance.rect.Max.y < result.rect.Min.y,
          "r7 selected pass has a heading and three ordered always-open editor groups");
    check(!has_text(ui::text::kInput) && !has_text(ui::text::kOutput),
          "r7 configured chain does not repeat input and output captions");
  };
  for (int pass : {1, 2, 3, 0, 1}) select_pass(pass);
  click(find("expand_all"));
  check(fold_open(ui::text::kResultLimits), "r7 selected pass expand reaches result folds");
  click(find("collapse_all"));
  check(!fold_open(ui::text::kResultLimits), "r7 selected pass collapse reaches result folds");
  select_pass(2);
  check(fold_open(ui::text::kResultLimits), "r7 another pass retains its own expanded disclosure state");
  select_pass(1);
  check(!fold_open(ui::text::kResultLimits), "r7 returning to a pass retains its collapsed disclosure state");

  runtime::kModelSteering[1].local_tone.Store(.625f);
  runtime::kLookResult[1].halo.Store(.7f);
  runtime::kLookResult[1].detail_stability.Store(2.f);
  frame();
  click(switch_for_label("Same as pass 1"));
  check(runtime::pass_follow[0].load() && !has_text("Local tone") && has_text(ui::text::kStyleFollows),
        "r7 appearance inheritance hides independent steering and shows its Pass 1 summary");
  const auto inherited = profile::Snapshot();
  check(runtime::pass_preset[0].load() == 0 && runtime::pass_resolution_mode[0].load() == 0
            && text_item("Run settings").rect.Min.y < text_item("Appearance & inheritance").rect.Min.y,
        "r7 appearance inheritance preserves main-model and base-resolution controls in Run settings");
  click(switch_for_label("Same as pass 1"));
  check(!runtime::pass_follow[0].load() && has_text("Local tone")
            && runtime::kModelSteering[1].local_tone.Get() == .625f,
        "r7 independent appearance restores stored steering values");
  auto independent = inherited;
  independent["NRPass2FollowPass1"] = false;
  check(profile::Snapshot() == independent, "r7 appearance inheritance only changes its own setting");
  click(switch_for_label(ui::text::kInheritResult));
  check(runtime::pass_look_result[0].follow.load() && has_text(ui::text::kInheritedResult)
            && !has_text("Adjust NR's look") && !has_text("Detail stability"),
        "r7 result inheritance shows its own Pass 1 summary and hides independent result controls");
  check(!runtime::pass_follow[0].load() && runtime::kLookResult[1].halo.Get() == .7f
            && runtime::kLookResult[1].detail_stability.Get() == 2.f,
        "r7 result inheritance preserves independent appearance and stored result values");
  click(switch_for_label(ui::text::kInheritResult));
  click(find("expand_all"));
  ImGui::SetScrollY(settings_window(), settings_window()->ScrollMax.y);
  frame(); frame();
  check(!runtime::pass_look_result[0].follow.load() && !runtime::pass_look_result[0].mode.load()
            && has_text("Detail stability"),
        "r7 detail stability remains inspectable while result adjustment is off");
  check(!find("Always").disabled, "r7 independent detail stability remains editable with result shaping off");
  const auto generation = runtime::configuration_generation.load();
  click(find("Always"));
  check(runtime::kLookResult[1].detail_stability.Get() == 2.f && runtime::kLookResult[0].detail_stability.Get() != 2.f,
        "r7 result stability choice belongs to the selected independent pass");
  Item stability_off;
  for (const auto& [id, item] : items) {
    if (item.label == "Off" && std::abs(item.rect.Min.y - find("Always").rect.Min.y) < 1.f) {
      stability_off = item;
      break;
    }
  }
  check(stability_off.id != 0, "r7 stability Off is resolved in its own row instead of the Stabilize row");
  click(stability_off);
  check(runtime::kLookResult[1].detail_stability.Get() == 1.f
            && runtime::configuration_generation.load() == generation && !runtime::pass_look_result[0].mode.load(),
        "r7 independent detail stability does not enable result shaping or recreate the NR configuration");

  runtime::stack_passes = 2;
  runtime::ui_selected_pass = 0;
  runtime::pass_stage[0] = 2;
  runtime::hook_point = runtime::kHookRender;
  frame(); frame();
  ImGui::SetScrollY(settings_window(), 0.f);
  frame(); frame();
  const auto original = profile::Snapshot();
  const auto preview_writes = configuration_writes;
  const auto preview_epoch = runtime::profile_commit_epoch.load();
  click(find("Present"));
  check(runtime::stage_edit::pending && runtime::stage_edit::confirmation_required
            && runtime::stage_edit::changes.at("NRPass2Stage") == 3u,
        "r7 P1 Present selection previews the required P2 compatibility adjustment");
  check(profile::Snapshot() == original && configuration_writes == preview_writes
            && runtime::profile_commit_epoch.load() == preview_epoch,
        "r7 Stage preview publishes and persists no candidate setting");
  click(find("cancel_stage_change"));
  check(!runtime::stage_edit::pending && runtime::stage_edit::changes.empty()
            && profile::Snapshot() == original && configuration_writes == preview_writes,
        "r7 real Stage cancel discards compatibility changes and preserves exact configuration");
  click(find("Upscaled"));
  auto compatible = original;
  compatible["NRHookPoint"] = 0u;
  check(!runtime::stage_edit::pending && profile::Snapshot() == compatible,
        "r7 compatible P1 adjustment commits only P1 and preserves explicit P2 stage and inheritance");

  for (const auto& [setting, value] : saved_settings) setting->Store(value);
  panel_width = saved_width;
  panel_height = saved_height;
  font_size = saved_font;
  io.DisplaySize = saved_display;
  runtime::ui_menu_page = saved_page;
  runtime::ui_selected_pass = saved_pass;
  runtime::ui_language_choice = saved_language;
  ui::fold_scopes = saved_folds;
  profile::active = saved_active;
  profile::observed_snapshot = profile::Snapshot();
  io.AddMousePosEvent(-100.f, -100.f);
  frame(); frame();
}
