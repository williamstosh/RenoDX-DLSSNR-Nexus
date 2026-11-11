{
  runtime::ui_language_choice = 1;
  ui::SelectLanguage(0);
  panel_height = 1600.f;
  panel_width = 1000.f;
  runtime::nr_follow_input_res = false;
  runtime::nr_resolution_scale = .75f;
  runtime::nr_adaptive_enabled = true;
  ui::MonitorInputs actual;
  actual.pass_count = 2;
  actual.passes[0].execution_confirmed = true;
  actual.passes[0].actual_size = "1600 x 900 (63%)";
  actual.passes[0].actual_scale = .625f;
  actual.passes[1].execution_confirmed = true;
  actual.passes[1].actual_size = "1280 x 720 (50%)";
  actual.passes[1].actual_scale = .5f;
  resolution_observed_fixture = &actual;
  adaptive_editor_fixture = true;
  frame(); frame();
  check(find("##actual_resolution_scale").disabled, "adaptive actual scale cannot be edited");
  check(std::none_of(items.begin(), items.end(), [](const auto& entry) {
    return entry.second.label == "##resolution_scale" || entry.second.label.ends_with("###nr_resolution_quality")
        || entry.second.label == "resolution_reset" || entry.second.label == "resolution_scale_reset";
  }), "adaptive mode exposes no manual resolution preset, slider or reset");
  const auto epoch = runtime::profile_commit_epoch.load();
  click(find("##actual_resolution_scale"));
  check(runtime::nr_resolution_scale.load() == .75f && runtime::profile_commit_epoch.load() == epoch,
        "clicking the actual scale preserves the saved manual upper limit");
  click(find("##adaptive_target_exact"));
  io.AddKeyEvent(ImGuiMod_Ctrl, true);
  io.AddKeyEvent(ImGuiKey_A, true);
  frame();
  io.AddKeyEvent(ImGuiKey_A, false);
  io.AddKeyEvent(ImGuiMod_Ctrl, false);
  frame();
  io.AddInputCharactersUTF8("116");
  frame(); frame();
  io.AddKeyEvent(ImGuiKey_Enter, true);
  frame();
  io.AddKeyEvent(ImGuiKey_Enter, false);
  frame(); frame();
  check(runtime::nr_adaptive_target_fps.load() == 116.f, "visible numeric input commits an exact 116 FPS target");
  float stored = 0.f;
  reshade::get_config_value(nullptr, runtime::kConfigSection, runtime::kSetAdaptiveTargetFps.key, stored);
  check(stored == 116.f, "exact numeric target persists through the production Setting boundary");
  actual.passes[0].execution_confirmed = false;
  frame(); frame();
  check(find("##actual_resolution_scale").disabled && std::ranges::any_of(rendered_text, [](const auto& item) {
    return item.text.find("Pending confirmation") != std::string::npos;
  }), "unknown actual size is pending while the saved manual value remains hidden");
  runtime::nr_adaptive_enabled = false;
  frame(); frame();
  check(!find("##resolution_scale").disabled && runtime::nr_resolution_scale.load() == .75f,
        "disabling adaptive restores the editable original manual upper limit");
  runtime::nr_adaptive_enabled = true;
  runtime::stack_passes = 2;
  runtime::ui_selected_pass = 1;
  runtime::pass_resolution_mode[0] = 2;
  runtime::pass_resolution_scale[0] = .58f;
  adaptive_pass_fixture = true;
  frame(); frame();
  check(find("##actual_resolution_scale").disabled && runtime::pass_resolution_scale[0].load() == .58f,
        "Pass 2 shares the read-only actual editor and preserves its independent upper limit");
  check(std::none_of(items.begin(), items.end(), [](const auto& entry) {
    return entry.second.label == "##pass_resolution_scale" || entry.second.label == "##pass_resolution_presets";
  }), "adaptive Pass 2 hides every manual resolution editing control");
  runtime::nr_adaptive_enabled = false;
  frame(); frame();
  check(!find("##pass_resolution_scale").disabled && runtime::pass_resolution_scale[0].load() == .58f,
        "disabling adaptive restores the independent Pass 2 editor");
  adaptive_pass_fixture = false;
  adaptive_editor_fixture = false;
  resolution_observed_fixture = nullptr;
  summary_fixture = true;
  summary_fields.fps = "153";
  summary_fields.gpu = "6.6ms";
  summary_fields.fg = "4x";
  for (const auto width : {1000.f, 340.f, 220.f}) {
    panel_width = width;
    frame(); frame();
    const auto locate = [&](const char* label) {
      const auto found = std::ranges::find_if(rendered_text, [label](const auto& item) { return item.text == label; });
      check(found != rendered_text.end(), "all three compact metric labels and values remain visible");
      return found->rect;
    };
    const auto fps = locate("FPS");
    const auto gpu = locate("NR GPU");
    const auto fg = locate("FG");
    check(locate("153").Min.y > fps.Min.y && locate("6.6ms").Min.y > gpu.Min.y
              && locate("4x").Min.y > fg.Min.y,
          "each compact metric places its value on the second line");
    check(std::abs(locate("153").GetCenter().x - fps.GetCenter().x) < 1.f
              && std::abs(locate("6.6ms").GetCenter().x - gpu.GetCenter().x) < 1.f
              && std::abs(locate("4x").GetCenter().x - fg.GetCenter().x) < 1.f,
          "metric labels and values share each column's center");
    if (width >= 340.f) {
      check(fps.Min.x < gpu.Min.x && gpu.Min.x < fg.Min.x && std::abs(fps.Min.y - fg.Min.y) < 1.f,
            "wide and game-sized 340 px status fit three ordered metric columns");
      const auto first = std::ranges::find_if(rendered_text, [](const auto& item) { return item.text == "FPS"; });
      const auto& content = first->window->WorkRect;
      check(std::abs((fps.GetCenter().x - content.Min.x) - (content.Max.x - fg.GetCenter().x)) < 1.f,
            "three metric columns leave symmetric space at the content edges");
    } else {
      check(fps.Min.x < gpu.Min.x && std::abs(fps.GetCenter().x - fg.GetCenter().x) < 1.f && fg.Min.y > fps.Min.y,
            "narrow status wraps the third metric onto a second row");
    }
  }
  runtime::MonitorCadence cadence;
  ui::frame_metrics::Sample raw{.source = ui::frame_metrics::Source::PRESENT, .raw_interval_ms = 28.f,
      .interval_ms = 1000.f / 35.f, .raw_valid = true, .valid = true};
  namespace rate = renodx::addons::dlss5::output_framerate;
  rate::Snapshot output{.available = true, .source = rate::Source::XEFG_STATUS, .fps = 153.4};
  cadence.FormatRate(output, raw);
  check(cadence.text == "153" && std::string(cadence.fps_tip) == rate::SourceText(output.source)
            && raw.raw_interval_ms == 28.f,
        "shared FPS formatting prefers output without manufacturing a frame interval");
  output.available = false;
  cadence.FormatRate(output, raw);
  check(cadence.text == "35" && std::string(cadence.fps_tip) == ui::frame_metrics::SourceTip(raw.source),
        "unknown output falls back to accurately labelled current cadence");
  rate::Stream saved_stream;
  {
    std::unique_lock lock(rate::internal::state.mutex);
    saved_stream = rate::internal::state.stream;
    rate::internal::state.stream = {};
  }
  const auto summary_for = [&](rate::Snapshot snapshot) {
    {
      std::unique_lock lock(rate::internal::state.mutex);
      rate::internal::state.stream.snapshot = snapshot;
      rate::internal::state.stream.previous_ns = runtime::SteadyNowNs();
    }
    runtime::monitor_snapshot.attempted_ns = 0;
    return runtime::RefreshMonitorSnapshot(true).summary;
  };
  check(summary_for({}).fg == "—", "unknown FG is not labelled disabled");
  check(summary_for({.fg_off = true}).fg == ui::Tr("Not enabled"), "confirmed disabled FG has a distinct value");
  const auto observed = summary_for({.available = true, .source = rate::Source::NATIVE_STATE, .fps = 153.,
      .observed_multiplier = 3.99});
  check(observed.fg == "4x" && observed.fps == "153", "observed near-integer FG and output FPS share concise values");
  check(observed.fg_tip.find("Observed") != std::string::npos, "FG estimation and source details remain discoverable in its tooltip");
  const auto saved_route = runtime::ui_fg_route;
  const auto saved_hook = runtime::hook_point.load();
  const bool saved_vulkan = runtime::vulkan_present_seen.load();
  const bool saved_unusable = runtime::present_unusable.load();
  runtime::hook_point = runtime::kHookPresent;
  runtime::vulkan_present_seen = false;
  runtime::present_unusable = false;
  runtime::ui_fg_route.route = ui::FgRoute::kBeforeFg;
  runtime::ui_fg_route.previous.multiplier = 1;
  fg_route_fixture = true;
  frame(); frame();
  check(rendered_text.empty(), "normal Pre-FG route adds no duplicate status row beside the shared SDK FG value");
  fg_route_detailed = true;
  frame(); frame();
  check(std::ranges::any_of(rendered_text, [](const auto& item) { return item.text == ui::Tr(ui::text::kFgBefore); })
            && std::ranges::none_of(rendered_text, [](const auto& item) {
              return item.text == ui::Tr(ui::text::kFgBeforeUnknown) || item.text == ui::Tr(ui::text::kFgUnknownMultiplierTip);
            }),
        "detailed Pre-FG diagnostics retain the route without contradicting reliable SDK multiplier evidence");
  runtime::ui_fg_route.route = ui::FgRoute::kFallbackPresent;
  fg_route_detailed = false;
  frame(); frame();
  check(std::ranges::any_of(rendered_text, [](const auto& item) { return item.text == ui::Tr(ui::text::kFgFallbackShort); }),
        "compact Pre-FG fallback warnings remain visible");
  fg_route_fixture = false;
  runtime::ui_fg_route = saved_route;
  runtime::hook_point = saved_hook;
  runtime::vulkan_present_seen = saved_vulkan;
  runtime::present_unusable = saved_unusable;
  {
    std::unique_lock lock(rate::internal::state.mutex);
    rate::internal::state.stream = saved_stream;
  }
}
