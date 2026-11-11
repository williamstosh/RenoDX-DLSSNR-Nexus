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
  summary_fields.resolution_ratio = "67%";
  if (std::getenv("NR_NEXUS_UX_REPRO") != nullptr) {
    const float saved_height = panel_height;
    panel_height = 240.f;
    for (const auto& [language, suffix] : {std::pair{0, "en"}, std::pair{13, "zh"}}) {
      ui::SelectLanguage(language);
      runtime::ui_language_choice = language + 1;
      for (const int width : {1000, 340}) {
        panel_width = static_cast<float>(width);
        capture((std::string("adaptive-summary-filled-") + suffix + "-" + std::to_string(width)).c_str());
      }
    }
    panel_height = saved_height;
    runtime::ui_language_choice = 1;
    ui::SelectLanguage(0);
  }
  for (const auto width : {1000.f, 340.f, 220.f}) {
    panel_width = width;
    frame(); frame();
    const auto locate = [&](const char* label) {
      const auto found = std::ranges::find_if(rendered_text, [label](const auto& item) { return item.text == label; });
      check(found != rendered_text.end(), "all four compact metric labels and values remain visible");
      return found->rect;
    };
    const auto fps = locate("FPS");
    const auto gpu = locate("NR GPU");
    const auto fg = locate("FG");
    const auto resolution = locate("NR resolution");
    check(locate("153").Min.y > fps.Min.y && locate("6.6ms").Min.y > gpu.Min.y
              && locate("4x").Min.y > fg.Min.y && locate("67%").Min.y > resolution.Min.y,
          "each compact metric places its value on the second line");
    check(std::abs(locate("153").GetCenter().x - fps.GetCenter().x) < 1.f
              && std::abs(locate("6.6ms").GetCenter().x - gpu.GetCenter().x) < 1.f
              && std::abs(locate("4x").GetCenter().x - fg.GetCenter().x) < 1.f
              && std::abs(locate("67%").GetCenter().x - resolution.GetCenter().x) < 1.f,
          "metric labels and values share each column's center");
    if (width >= 340.f) {
      check(fps.Min.x < gpu.Min.x && gpu.Min.x < fg.Min.x && fg.Min.x < resolution.Min.x
                && std::abs(fps.Min.y - resolution.Min.y) < 1.f,
            "wide and game-sized 340 px status fit four ordered metric columns");
      const auto first = std::ranges::find_if(rendered_text, [](const auto& item) { return item.text == "FPS"; });
      const auto& content = first->window->WorkRect;
      check(std::abs((fps.Min.x - content.Min.x) - (content.Max.x - resolution.Max.x)) < 1.f,
            "four metric columns leave symmetric text padding at the content edges");
    } else {
      check(gpu.Min.y > fps.Min.y && fg.Min.y > gpu.Min.y && resolution.Min.y > fg.Min.y,
            "narrow status stacks complete metric labels without clipping");
    }
  }
  for (int language = 0; language < ui::kLanguageCount; ++language) {
    ui::SelectLanguage(language);
    for (const float width : {1000.f, 340.f, 220.f}) {
      panel_width = width;
      frame(); frame();
      for (const char* key : {"FPS", "NR GPU", "FG", "NR resolution", "67%"}) {
        check(std::ranges::any_of(rendered_text, [&](const auto& item) { return item.text == ui::Tr(key); }),
              "four summary fields retain their complete labels and values in every language and width");
      }
      for (const auto& item : rendered_text) {
        if (std::strstr(item.window->Name, "##nr_status_summary") == nullptr) continue;
        check(item.rect.Max.x <= item.window->InnerClipRect.Max.x + 1.f
                  && item.rect.Max.y <= item.window->InnerClipRect.Max.y + 1.f,
              "wrapped summary fields stay inside the compact card");
      }
    }
  }
  ui::SelectLanguage(0);
  check(ui::SummaryMetricColumns(summary_fields, 1000.f) == 3,
        "the runtime monitor retains its existing three-column metric layout");
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
  summary_fixture = false;
  panel_width = 1000.f;
  hook_editor_fixture = true;
  runtime::nr_adaptive_enabled = true;
  runtime::nr_follow_input_res = false;
  runtime::nr_resolution_scale = 1.f;
  runtime::preset = 2;
  namespace xefg = renodx::addons::dlss5::fg::xefg;
  const auto saved_requested = xefg::internal::requested.load();
  const bool saved_owns = xefg::internal::owns_stream.load();
  xefg::internal::requested = runtime::fg_service::Requested;
  xefg::internal::owns_stream = true;
  runtime::hooks_enabled = true;
  runtime::enabled = true;
  for (const uint32_t passes : {1u, 2u}) {
    runtime::stack_passes = passes;
    runtime::pass_resolution_mode[0] = 2;
    runtime::pass_resolution_scale[0] = 1.f;
    runtime::pass_stage[0] = 1;
    for (const auto& [label, stored] : {std::pair{"Upscaled", 0u}, {"Render", 1u}}) {
      runtime::hook_point = runtime::kHookPresent;
      frame(); frame();
      check(!find(label).disabled, "adaptive Present permits a supported return to inline execution");
      click(find(label));
      if (runtime::stage_edit::pending) click(find("confirm_stage_change"));
      check(runtime::hook_point.load() == stored, "adaptive Hook click applies the requested inline stage");
    }
  }
  hook_editor_fixture = false;
  runtime::stack_passes = 1;
  runtime::ui_menu_page = 0;
  runtime::ui_classic_layout = false;
  for (const bool minimum_reached : {false, true}) {
    for (const auto& [label, stored] : {std::pair{"Upscaled", 0u}, {"Render", 1u}}) {
      runtime::hook_point = runtime::kHookPresent;
      namespace adaptive = renodx::addons::dlss5::adaptive_resolution;
      adaptive::Session session;
      adaptive::Sizes upper{}, floor{};
      upper[0] = {.width = 3840, .height = 2160};
      floor[0] = {.width = 1267, .height = 713};
      const adaptive::Settings settings{.enabled = true, .target_fps = 116.f, .minimum_scale = .33f, .prepared_levels = 4};
      const auto now = runtime::SteadyNowNs();
      const adaptive::Output output{.available = true, .source = 1, .generation = 1, .configuration = 1,
          .serial = 1, .begin_ns = now - 500'000'000, .end_ns = now, .fps = (minimum_reached ? 50. : 120.)};
      session.Prepare(settings, upper, floor, 42, output, now - 10'000'000'000, true, false);
      session.Request(minimum_reached ? session.ladder.count - 1 : 0, now - 10'000'000'000);
      session.Applied(session.status.generation, session.status.requested, now - 10'000'000'000);
      for (uint64_t sample = 0; sample < 5; ++sample) {
        session.ObserveGpu({.generation = session.status.generation, .frame = sample + 1, .sequence = sample + 1,
            .recorded_ns = now - 1'500'000'000 + static_cast<int64_t>(sample) * 250'000'000,
            .completed_ns = now - 500'000'000, .total_us = 1000., .valid = true, .stable = true}, now);
      }
      session.Prepare(settings, upper, floor, 42, output, now, true, false);
      check(session.status.reason == (minimum_reached ? adaptive::Reason::AT_MINIMUM : adaptive::Reason::OBSERVING),
          "the held-click fixture starts from normal or minimum-reached controller status");
      runtime::PublishAdaptiveStatus(session.status);
      frame(); frame();
      check(find("##actual_resolution_scale").disabled && !find(label).disabled,
            "the complete Quality page restores interaction after the adaptive read-only control");
      auto button = find(label);
      for (int attempt = 0; attempt < 4 && !button.window->InnerClipRect.Contains(button.rect.GetCenter()); ++attempt) {
        ImGui::SetScrollY(button.window, button.window->Scroll.y + button.rect.GetCenter().y
            - button.window->InnerClipRect.GetCenter().y);
        frame(); frame();
        button = find(label);
      }
      const auto pressed_at = button.rect.GetCenter();
      io.AddMousePosEvent(pressed_at.x, pressed_at.y);
      frame();
      io.AddMouseButtonEvent(0, true);
      frame();
      check(!runtime::nr_adaptive_editing.load(), "holding a Stage button never masquerades as an unpublished slider edit");
      session.Prepare(settings, upper, floor, 42, output, now, true, runtime::nr_adaptive_editing.load());
      runtime::PublishAdaptiveStatus(session.status);
      frame();
      check(std::abs(find(label).rect.Min.y - button.rect.Min.y) < .5f,
          "temporary adaptive pause cannot move the pressed Stage button before mouse release");
      io.AddMouseButtonEvent(0, false);
      frame(); frame();
      if (runtime::stage_edit::pending) click(find("confirm_stage_change"));
      check(runtime::hook_point.load() == stored,
            "the complete Quality page applies adaptive Present to inline Hook changes");
    }
  }
  auto minimum_slider = find("##adaptive_minimum");
  for (int attempt = 0; attempt < 4 && !minimum_slider.window->InnerClipRect.Contains(minimum_slider.rect.GetCenter()); ++attempt) {
    ImGui::SetScrollY(minimum_slider.window, minimum_slider.window->Scroll.y + minimum_slider.rect.GetCenter().y
        - minimum_slider.window->InnerClipRect.GetCenter().y);
    frame(); frame();
    minimum_slider = find("##adaptive_minimum");
  }
  const auto minimum_position = minimum_slider.rect.GetCenter();
  io.AddMousePosEvent(minimum_position.x, minimum_position.y);
  frame();
  io.AddMouseButtonEvent(0, true);
  frame();
  io.AddMousePosEvent(minimum_position.x + 10.f, minimum_position.y);
  frame();
  check(runtime::slider_pending == &runtime::kSetAdaptiveMinScale && runtime::nr_adaptive_editing.load(),
      "an actual minimum-scale drag retains the production adaptive pause");
  std::shared_ptr<renodx::addons::dlss5::adaptive_resolution::Session> editing_session;
  const auto editing_plan = runtime::RequestedStagePlan();
  runtime::FreezeAdaptiveFrame(&editing_session, editing_plan, editing_plan.At(editing_plan.effective[0]),
      3840, 2160, 1920, 1080, 0xADA57A6E, true);
  check(editing_session->status.reason == renodx::addons::dlss5::adaptive_resolution::Reason::FROZEN,
      "real slider edits still freeze evaluation while Stage-button holds do not");
  io.AddMouseButtonEvent(0, false);
  frame(); frame();
  check(runtime::slider_pending == nullptr && !runtime::nr_adaptive_editing.load(),
      "releasing the actual slider settles the edit and resumes adaptive evaluation");
  xefg::internal::requested = saved_requested;
  xefg::internal::owns_stream = saved_owns;
  hook_editor_fixture = false;
  {
    namespace addon = renodx::addons::dlss5;
    namespace timers = addon::gpu_timers;
    runtime::hook_point = runtime::kHookUpscaled;
    runtime::stack_passes = 1;
    runtime::active_features = 1;
    runtime::nr_adaptive_enabled = false;
    runtime::card_observation.window = {};
    addon::NrVerdict verdict{.state = addon::NrState::kEngaged, .api = "d3d12", .path = "inline"};
    verdict.eligible = verdict.injected = 100;
    const auto started = runtime::SteadyNowNs();
    auto& measured = timers::stage_stats[1];
    measured = {};
    timers::execution_stats[1] = {};
    timers::internal::next_sequence = 10;
    runtime::CollectCardInputs(verdict, started);
    measured.valid = true;
    measured.state = timers::SampleState::VALID;
    measured.sequence = ++timers::internal::next_sequence;
    measured.recorded_ns = measured.measured_ns = started + 100'000'000LL;
    runtime::comparison_ui::CaptureSettings(&measured.identity);
    measured.identity.device_session = timers::device_session;
    measured.passes[0] = {.id = 0, .state = timers::SampleState::VALID};
    verdict.eligible = verdict.injected = 110;
    check(runtime::CollectCardInputs(verdict, started + 100'000'000LL).recent->gpu_completed,
          "a current GPU completion establishes card execution evidence");
    const auto completed = measured;
    measured.state = timers::SampleState::REPLAYED;
    measured.sequence = ++timers::internal::next_sequence;
    verdict.eligible = verdict.injected = 120;
    check(runtime::CollectCardInputs(verdict, started + 200'000'000LL).recent->gpu_completed,
          "a rejected newer recording cannot erase a still-current completed execution");
    auto& observed = runtime::stage_telemetry[1].last_evaluate;
    observed.observed_ns = started + 250'000'000LL;
    observed.api_returned = true;
    observed.result = NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
    check(!runtime::CollectCardInputs(verdict, started + 250'000'000LL).recent->gpu_completed,
          "a real Evaluate failure immediately invalidates retained execution evidence");
    observed = {};
    measured = completed;
    runtime::CollectCardInputs(verdict, started + 300'000'000LL);
    check(!runtime::CollectCardInputs(verdict, started + ui::RecentHealthWindow::kStaleNs + 100'000'001LL)
               .recent->gpu_completed,
          "retained execution evidence expires at the existing two-second bound");
    measured = completed;
    measured.sequence = ++timers::internal::next_sequence;
    measured.recorded_ns = measured.measured_ns = started + 300'000'000LL;
    ++runtime::configuration_generation;
    check(!runtime::CollectCardInputs(verdict, started + 300'000'000LL).recent->gpu_completed,
          "a settings change rejects both latest and retained old-configuration completions");
    runtime::nr_resolution_scale = .42f;
    for (const auto hook : {runtime::kHookRender, runtime::kHookUpscaled, runtime::kHookPresent}) {
      runtime::hook_point = hook;
      const size_t stage = (hook == runtime::kHookRender ? 0 : hook == runtime::kHookUpscaled ? 1 : 2);
      auto& sample = timers::stage_stats[stage];
      sample = completed;
      sample.recorded_ns = sample.measured_ns = runtime::SteadyNowNs();
      sample.width = (hook == runtime::kHookRender ? 2560 : 3840);
      sample.height = (hook == runtime::kHookRender ? 1440 : 2160);
      sample.passes[0].width = (hook == runtime::kHookRender ? 1715 : 2573);
      sample.passes[0].height = (hook == runtime::kHookRender ? 965 : 1447);
      std::strcpy(sample.path, (stage == 0 ? "preSR" : stage == 1 ? "upscaled" : "present"));
      runtime::comparison_ui::CaptureSettings(&sample.identity);
      sample.identity.device_session = timers::device_session;
      runtime::monitor_snapshot.attempted_ns = 0;
      const auto actual_snapshot = runtime::RefreshMonitorSnapshot(true);
      const auto reference = std::to_string(sample.width) + " x " + std::to_string(sample.height);
      const auto actual_size = std::to_string(sample.passes[0].width) + " x " + std::to_string(sample.passes[0].height);
      check(actual_snapshot.summary.resolution_ratio == "67%"
                && actual_snapshot.monitor.passes[0].actual_scale == static_cast<float>(sample.passes[0].width) / sample.width
                && actual_snapshot.summary.resolution_tip.find(reference) != std::string::npos
                && actual_snapshot.summary.resolution_tip.find(actual_size) != std::string::npos,
            "the fourth field shares Pass 1 completed working dimensions and the selected Stage reference");
      sample.state = timers::SampleState::EXPIRED;
      runtime::card_observation.completions[stage].recorded_ns = runtime::SteadyNowNs()
          - timers::internal::kSampleAgeNs - 1;
      runtime::monitor_snapshot.attempted_ns = 0;
      check(runtime::RefreshMonitorSnapshot(true).summary.resolution_ratio.empty(),
            "an unknown actual ratio never falls back to the saved manual percentage");
    }
    const auto saved_policy = xefg::internal::requested.load();
    const bool saved_stream_owned = xefg::internal::owns_stream.load();
    xefg::internal::requested = runtime::fg_service::Requested;
    for (const bool private_fg : {false, true}) {
      runtime::hook_point = (private_fg ? runtime::kHookPresent : runtime::kHookUpscaled);
      xefg::internal::owns_stream = private_fg;
      const size_t stage = (private_fg ? 2 : 1);
      auto& sample = timers::stage_stats[stage];
      auto& proof = timers::execution_stats[stage];
      const auto seed = [&] {
        runtime::stage_telemetry[stage].last_evaluate = {.source_generation = 99};
        proof = {};
        sample = completed;
        sample.sequence = ++timers::internal::next_sequence;
        sample.recorded_ns = sample.measured_ns = runtime::SteadyNowNs();
        sample.width = 3840;
        sample.height = 2160;
        sample.total_us = 6600;
        sample.passes[0] = {.id = 0, .model = runtime::preset.load(), .width = 2573, .height = 1447,
            .output_width = 2573, .output_height = 1447, .total_us = 6600, .state = timers::SampleState::VALID};
        std::strcpy(sample.path, (private_fg ? "present" : "upscaled"));
        runtime::comparison_ui::CaptureSettings(&sample.identity);
        sample.identity.source_generation = 99;
        sample.identity.stable_epoch = runtime::comparison_stable_epochs[stage];
        sample.identity.device_session = timers::device_session;
        runtime::monitor_snapshot.attempted_ns = 0;
        const auto snapshot = runtime::RefreshMonitorSnapshot(true);
        check(snapshot.summary.resolution_ratio == "67%" && snapshot.summary.gpu == "6.6ms",
              "inline and private FG share the same current completed size and measured GPU source");
        sample.state = timers::SampleState::REPLAYED;
        sample.sequence = ++timers::internal::next_sequence;
      };
      seed();
      runtime::monitor_snapshot.attempted_ns = 0;
      const auto rejected = runtime::RefreshMonitorSnapshot(true);
      check(rejected.summary.resolution_ratio == "67%" && rejected.summary.gpu.empty()
                && rejected.monitor.passes[0].execution_confirmed,
            "a rejected latest recording retains bounded actual size while leaving GPU cost unavailable");
      proof = runtime::card_observation.completions[stage];
      proof.sequence = ++timers::internal::next_sequence;
      proof.recorded_ns = proof.measured_ns = runtime::SteadyNowNs();
      proof.passes[0].width = 1920;
      proof.passes[0].height = 1080;
      runtime::monitor_snapshot.attempted_ns = 0;
      check(runtime::RefreshMonitorSnapshot(true).summary.resolution_ratio == "50%",
            "a new completed working size takes priority over retained actual dimensions");
      for (int rejection = 0; rejection < 8; ++rejection) {
        seed();
        auto& observed = runtime::stage_telemetry[stage].last_evaluate;
        switch (rejection) {
          case 0:
            runtime::card_observation.completions[stage].recorded_ns = runtime::SteadyNowNs()
                - timers::internal::kSampleAgeNs - 1;
            break;
          case 1: ++runtime::configuration_generation; break;
          case 2: ++timers::device_session; break;
          case 3: observed.source_released = true; break;
          case 4: ++observed.source_generation; break;
          case 5: ++runtime::comparison_stable_epochs[stage]; break;
          case 6:
          case 7:
            observed.observed_ns = runtime::SteadyNowNs();
            observed.api_returned = rejection == 7;
            observed.result = NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
            break;
        }
        runtime::monitor_snapshot.attempted_ns = 0;
        const auto invalid = runtime::RefreshMonitorSnapshot(true);
        check(invalid.summary.resolution_ratio.empty() && invalid.summary.gpu.empty()
                  && !invalid.monitor.passes[0].execution_confirmed,
              "expired, changed, released or truly failed execution cannot retain actual dimensions");
      }
    }
    xefg::internal::requested = saved_policy;
    xefg::internal::owns_stream = saved_stream_owned;
  }
}
