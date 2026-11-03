/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Composed in capability-ui: synthetic observations, production adapters and
// the existing disk-backed ReShade fixture. No GPU execution is performed.
{
  namespace xefg = renodx::addons::dlss5::fg::xefg::internal;
  namespace templates = renodx::addons::dlss5::profiles;
  const auto saved_xefg_owner = xefg::owns_stream.load();
  const auto saved_dx12 = runtime::d3d12_swapchain_seen.load();
  const auto saved_dx11 = runtime::d3d11_swapchain_seen.load();
  const auto saved_dx11_route = runtime::dx11_route.load();
  const auto saved_auto_fallback = runtime::auto_fg_fallback_active.load();
  const auto defaults = profile::Snapshot(true);
  const auto ultra = templates::Template(2, defaults);
  Check(ultra.at("NRHookPoint") == 2u && ultra.at("NRPasses") == 2u
            && ultra.at("NRPreset") == 1u && ultra.at("NRPass2Preset") == 2u
            && ultra.at("NRFollowInputRes") == false && ultra.at("NRResolutionScale") == 1.f
            && ultra.at("NRPass2Stage") == 3u && ultra.at("NRPass2ResolutionMode") == 2u
            && ultra.at("NRPass2ResolutionScale") == 1.f,
        "Official Ultra retains its explicit Present A100 / B100 recipe.");
  const auto trace = [&](const char* scenario, const Json& candidate, bool accepted, const std::string& reason) {
    const auto current = profile::Snapshot();
    auto full_candidate = current;
    full_candidate.update(candidate);
    const auto result = runtime::QueryConfigurationCapability(full_candidate);
    const auto current_plan = runtime::RequestedStagePlan();
    Json current_stages = Json::array();
    for (uint32_t pass = 0; pass < current_plan.pass_count; ++pass) {
      current_stages.push_back({{"requested", plan::Name(current_plan.requested[pass])},
                                {"effective", plan::Name(current_plan.effective[pass])}});
    }
    Json saved = Json::object();
    Json disk = Json::object();
    for (const auto* key : {"NRPasses", "NRHookPoint", "NRPreset", "NRPass2Preset",
                            "NRFollowInputRes", "NRResolutionScale", "NRPass2Stage",
                            "NRPass2ResolutionMode", "NRPass2ResolutionScale"}) {
      saved[key] = adapter::ReadSetting(key);
      disk[key] = Read(key);
    }
    std::cout << "present-resolution-edit: " << Json{
        {"scenario", scenario}, {"fg", static_cast<unsigned>(runtime::native_fg_declaration.load())},
        {"backend", runtime::d3d11_swapchain_seen.load() ? "DX11 bridge" : "DX12 native"},
        {"auto_fallback", runtime::auto_fg_fallback_active.load()},
        {"xefg", xefg::owns_stream.load()}, {"candidate_patch", candidate},
        {"full_candidate", full_candidate}, {"current", current},
        {"saved_settings", saved}, {"saved_ini", disk}, {"accepted", accepted}, {"reason", reason},
        {"stage_plan", {{"current_valid", current_plan.valid}, {"current_stages", current_stages},
                        {"current_reason", plan::ReasonText(current_plan.reason)},
                        {"candidate_root", full_candidate.at("NRHookPoint")},
                        {"candidate_pass_count", full_candidate.at("NRPasses")},
                        {"candidate_tail_stages", {full_candidate.at("NRPass2Stage"),
                                                   full_candidate.at("NRPass3Stage"),
                                                   full_candidate.at("NRPass4Stage")}},
                        {"candidate_reason", plan::ReasonText(result.plan_reason)},
                        {"candidate_group_count", result.group_count}}},
        {"capability_reason", cap::ReasonText(result.configuration.reason)},
        {"working_sizes", {{result.working_sizes[0].width, result.working_sizes[0].height},
                             {result.working_sizes[1].width, result.working_sizes[1].height}}},
        {"execution_proven", result.execution_proven}}.dump() << std::endl;
  };
  for (const bool bridge : {false, true}) {
    runtime::d3d12_swapchain_seen = !bridge;
    runtime::d3d11_swapchain_seen = bridge;
    runtime::dx11_route = runtime::kDx11RouteNative;
    runtime::auto_fg_fallback_active = false;
    for (unsigned environment = 0; environment < 3; ++environment) {
      runtime::native_fg_declaration = (environment == 0 ? runtime::NativeFgDeclaration::kOn
                                                        : runtime::NativeFgDeclaration::kUnknown);
      runtime::native_fg_public_off = false;
      xefg::owns_stream = environment == 1;
      for (const float scale : {.67f, .5f, .75f}) {
        auto inherited = pair;
        inherited.update({{"NRHookPoint", 2u}, {"NRResolutionScale", scale},
                          {"NRPass2Stage", 3u}, {"NRPass2ResolutionMode", 0u}});
        const auto before = profile::Snapshot();
        const auto bytes = Bytes();
        const auto write_count = writes.size();
        const auto result = runtime::QueryConfigurationCapability(inherited);
        const auto rows = adapter::Preview({.scope = presets::Scope::FULL, .settings = inherited}, true);
        Check(result.CanSubmit() && result.configuration.reason == cap::Reason::NONE
                  && result.group_count == 1 && result.working_sizes[0] == result.working_sizes[1]
                  && !result.execution_proven,
              "FG On / XeFG / Unknown permits inherited Present 67 / 50 / 75 pairs without GPU proof.");
        Check(std::none_of(rows.begin(), rows.end(), [](const auto& row) {
                return std::string_view(row.status) == "Unavailable in current environment";
              }) && profile::Snapshot() == before && Bytes() == bytes && writes.size() == write_count,
              "Legal resolution preview is read-only and does not report an unavailable candidate.");
        const bool accepted = apply(inherited);
        trace("inherited-pair", inherited, accepted, error);
        Check(accepted && !runtime::nr_follow_input_res.load() && runtime::nr_resolution_scale.load() == scale
                  && runtime::pass_resolution_mode[0].load() == 0u
                  && std::abs(std::stof(Read("NRResolutionScale")) - scale) < .00001f
                  && Read("NRPass2ResolutionMode") == "0",
              "Inherited pair application persists the chosen scale and inheritance mode.");
      }
      auto fixed = pair;
      fixed.update({{"NRHookPoint", 2u}, {"NRResolutionScale", .67f}, {"NRPass2Stage", 3u},
                    {"NRPass2ResolutionMode", 2u}, {"NRPass2ResolutionScale", .67f}});
      const auto equal = runtime::QueryConfigurationCapability(fixed);
      Check(equal.CanSubmit() && equal.working_sizes[0] == cap::Size{1286, 724}
                && equal.working_sizes[1] == equal.working_sizes[0] && !equal.execution_proven,
            "Independent fixed 67 / 67 remains legal under every FG declaration.");
      const bool fixed_accepted = apply(fixed);
      trace("independent-equal-67", fixed, fixed_accepted, error);
      Check(fixed_accepted && Read("NRPass2ResolutionMode") == "2"
                && std::abs(std::stof(Read("NRPass2ResolutionScale")) - .67f) < .00001f,
            "Equal fixed sizes retain and persist their independent mode.");
      auto mismatch = fixed;
      {
        auto rounded_pair = fixed;
        rounded_pair["NRPass2ResolutionScale"] = .6701f;
        const auto saved_snapshot = profile::Snapshot();
        const auto saved_ini = Bytes();
        const auto saved_writes = writes.size();
        const auto saved_generation = runtime::configuration_generation.load();
        const auto saved_history = runtime::history_reset_epoch.load();
        auto& geometry = runtime::stage_telemetry[static_cast<unsigned>(plan::Stage::PRESENT)];
        const auto saved_width = geometry.width;
        const auto saved_height = geometry.height;
        geometry.width = 100;
        geometry.height = 100;
        const auto rounded_equal = runtime::QueryConfigurationCapability(rounded_pair);
        Check(rounded_equal.CanSubmit() && rounded_equal.working_sizes[0] == cap::Size{67, 67}
                  && rounded_equal.working_sizes[1] == rounded_equal.working_sizes[0]
                  && !rounded_equal.execution_proven,
              "Different fixed scales are eligible when the current reference rounds both to 67 pixels.");
        trace("rounded-same-pixels", rounded_pair, rounded_equal.CanSubmit(), "");
        geometry.width = 1920;
        geometry.height = 1080;
        const auto resized_mismatch = runtime::QueryConfigurationCapability(rounded_pair);
        Check(!resized_mismatch.CanSubmit() && !resized_mismatch.execution_proven
                  && resized_mismatch.working_sizes[0] == cap::Size{1286, 724}
                  && resized_mismatch.working_sizes[1] == cap::Size{1287, 724}
                  && resized_mismatch.configuration.reason == (environment == 1 ? cap::Reason::FG_OWNS_INPUT
                                                                               : cap::Reason::FG_OFF_REQUIRED)
                  && profile::Snapshot() == saved_snapshot && Bytes() == saved_ini && writes.size() == saved_writes
                  && runtime::configuration_generation.load() == saved_generation
                  && runtime::history_reset_epoch.load() == saved_history,
              "Resize recomputes a real mismatch instead of reusing rounded-size eligibility, without configuration or epoch changes.");
        trace("rounded-pixels-resize-mismatch", rounded_pair, resized_mismatch.CanSubmit(),
              runtime::FormatConfigurationCapabilityError(rounded_pair, resized_mismatch));
        geometry.width = saved_width;
        geometry.height = saved_height;
      }
      mismatch["NRPass2ResolutionScale"] = 1.f;
      const auto rejected = runtime::QueryConfigurationCapability(mismatch);
      Check(!rejected.CanSubmit() && !rejected.execution_proven
                && rejected.configuration.reason == (environment == 1 ? cap::Reason::FG_OWNS_INPUT
                                                                       : cap::Reason::FG_OFF_REQUIRED),
            "Present 67 / 100 rejects with the actual FG ownership / Off-required reason.");
      reject_unchanged(mismatch);
      error.clear();
      const bool mismatch_valid = runtime::ValidateConfigurationCapability(mismatch, &error);
      trace("independent-mismatch-67-100", mismatch, mismatch_valid, error);
      Check(error == runtime::FormatConfigurationCapabilityError(mismatch, rejected)
                && error.find("67%") != std::string::npos && error.find("100%") != std::string::npos
                && error.find("Use matching resolutions.") != std::string::npos
                && error.find(cap::ReasonText(rejected.configuration.reason)) != std::string::npos,
            "Real fixed 67 / 100 conflict names both percentages, matching-size advice and the original FG reason.");
      for (unsigned boundary = 0; boundary < 5; ++boundary) {
        auto request = mismatch;
        const auto saved_prefg = runtime::experimental_native_prefg_resolution.load();
        const auto saved_mixed = runtime::experimental_present_chain.load();
        auto& present_geometry = runtime::stage_telemetry[static_cast<unsigned>(plan::Stage::PRESENT)];
        const auto saved_width = present_geometry.width;
        const auto saved_height = present_geometry.height;
        switch (boundary) {
          case 0: request["NRFollowInputRes"] = true; break;
          case 1: request["NRPass2ResolutionMode"] = 1u; break;
          case 2:
            present_geometry.width = 0;
            present_geometry.height = 0;
            break;
          case 3: runtime::experimental_native_prefg_resolution = true; break;
          case 4:
            runtime::experimental_present_chain = true;
            request["NRHookPoint"] = 0u;
            break;
        }
        const auto result = runtime::QueryConfigurationCapability(request);
        const auto message = runtime::FormatConfigurationCapabilityError(request, result);
        const auto* original_reason = (result.configuration.reason == cap::Reason::PLAN_REJECTED
            ? plan::ReasonText(result.plan_reason) : cap::ReasonText(result.configuration.reason));
        Check(!runtime::PresentResolutionMismatch(request, result)
                  && message == original_reason
                  && message.find("67%") == std::string::npos && message.find("100%") == std::string::npos
                  && message.find("Use matching resolutions.") == std::string::npos
                  && runtime::ProposedResolutionInheritance(request).empty(),
              "Unknown Guide / reference, PreFG and B1 retain their own reason without a fixed-size conflict or repair.");
        if (boundary == 3) {
          Check(result.configuration.reason == cap::Reason::NATIVE_PREFG_BLOCKED,
                "PreFG preserves the explicit experimental BLOCKED / NO-GO capability reason.");
        }
        trace("conflict-wording-boundary", request, result.CanSubmit(), message);
        runtime::experimental_native_prefg_resolution = saved_prefg;
        runtime::experimental_present_chain = saved_mixed;
        present_geometry.width = saved_width;
        present_geometry.height = saved_height;
      }
      for (const bool resolution_first : {true, false}) {
        auto start = fixed;
        start.update({{"NRPasses", 1u}, {"NRResolutionScale", 1.f}, {"NRPass2ResolutionMode", 0u}});
        Check(apply(start), "Editing-order fixture starts at one Present pass with inherited tail.");
        const Json resolution{{"NRFollowInputRes", false}, {"NRResolutionScale", .67f}};
        const Json count{{"NRPasses", 2u}};
        const auto& first = (resolution_first ? resolution : count);
        const auto& second = (resolution_first ? count : resolution);
        Check(runtime::CanChooseSetting(resolution_first ? runtime::kSetResolutionScale : runtime::kSetStackPasses,
                                        resolution_first ? .67f : 2.f, &error),
              "First editing step is selectable through the production setting gate.");
        const bool first_applied = apply(first);
        trace(resolution_first ? "resolution-first-step1" : "count-first-step1", first, first_applied, error);
        Check(first_applied, "First editing step applies through the production preset adapter.");
        Check(runtime::CanChooseSetting(resolution_first ? runtime::kSetStackPasses : runtime::kSetResolutionScale,
                                        resolution_first ? 2.f : .67f, &error),
              "Second editing step is selectable after re-evaluating the saved configuration.");
        const bool second_applied = apply(second);
        trace(resolution_first ? "resolution-first-step2" : "count-first-step2", second, second_applied, error);
        Check(second_applied && runtime::stack_passes.load() == 2u && runtime::nr_resolution_scale.load() == .67f
                  && runtime::pass_resolution_mode[0].load() == 0u
                  && Read("NRPasses") == "2" && Read("NRPass2ResolutionMode") == "0",
              "Both editing orders reach the same persisted inherited Present 67 / 67 pair.");
      }
      Check(apply(fixed), "Runtime geometry fixture restores fixed 67 / 67.");
      const auto before_resize = profile::Snapshot();
      const auto ini_before_resize = Bytes();
      const auto writes_before_resize = writes.size();
      auto& geometry = runtime::stage_telemetry[static_cast<unsigned>(plan::Stage::PRESENT)];
      geometry.width = 2560;
      geometry.height = 1440;
      const auto resized = runtime::QueryConfigurationCapability();
      Check(resized.CanSubmit() && resized.working_sizes[0] == cap::Size{1715, 965}
                && resized.working_sizes[1] == resized.working_sizes[0] && !resized.execution_proven
                && profile::Snapshot() == before_resize && Bytes() == ini_before_resize
                && writes.size() == writes_before_resize,
            "New runtime dimensions recompute working pixels without rewriting configuration or GPU proof.");
      trace("runtime-resize", Json::object(), resized.CanSubmit(), "");
      geometry.width = 1920;
      geometry.height = 1080;
      Check(runtime::QueryConfigurationCapability().working_sizes[0] == cap::Size{1286, 724},
            "Returning runtime geometry recomputes the original size instead of retaining cached pixels.");
      Check(apply(ultra), "Official Ultra remains eligible at its original equal full-size recipe.");
      const auto official_before = profile::Snapshot();
      const auto official_ini = Bytes();
      const Json quick_request{{"NRFollowInputRes", false}, {"NRResolutionScale", .67f}};
      error.clear();
      const bool quick_valid = runtime::ValidateConfigurationCapability(quick_request, &error);
      trace("repair-api-needed-ultra-to-67", quick_request, quick_valid, error);
      Check(!quick_valid && !error.empty(),
            "Baseline direct Ultra-to-67 request requires a repair candidate for its explicit fixed-100 tail.");
      reject_unchanged(quick_request);
      Check(profile::Snapshot() == official_before && Bytes() == official_ini
                && templates::Template(2, defaults) == ultra,
            "Rejected shortcut preserves official Ultra and never rewrites its recipe to inheritance.");
      auto single_ultra = ultra;
      single_ultra["NRPasses"] = 1u;
      Check(apply(single_ultra) && apply(quick_request),
            "Resolution-first edits one Present pass to 67 while retaining the hidden fixed-100 tail.");
      const Json add_pass{{"NRPasses", 2u}};
      error.clear();
      const bool add_valid = runtime::ValidateConfigurationCapability(add_pass, &error);
      trace("repair-api-needed-resolution-first-add-pass", add_pass, add_valid, error);
      Check(!add_valid && !error.empty()
                && !runtime::CanChooseSetting(runtime::kSetStackPasses, 2.f, &error),
            "Baseline resolution-first cannot add fixed-100 Pass 2 through the strict setting gate.");
      reject_unchanged(add_pass);
      Check(apply(single_ultra) && apply(add_pass)
                && !runtime::CanChooseSetting(runtime::kSetResolutionScale, .67f, &error),
            "Baseline count-first adds the equal full-size pass then strictly rejects the base-67 shortcut.");
      reject_unchanged(quick_request);
      {
        namespace edit = runtime::resolution_edit;
        for (const bool resolution_first : {true, false}) {
          edit::Cancel();
          edit::undo.clear();
          edit::redo.clear();
          Check(apply(single_ultra), "Repair transaction starts from the unchanged one-pass Ultra recipe.");
          const Json requested = (resolution_first ? add_pass : quick_request);
          Check(apply(resolution_first ? quick_request : add_pass),
                "First editing step preserves the explicit hidden / active fixed-100 tail.");
          const auto original = profile::Snapshot();
          const auto original_ini = Bytes();
          const auto original_writes = writes.size();
          const auto original_generation = runtime::configuration_generation.load();
          const auto original_history = runtime::history_reset_epoch.load();
          auto proposed = requested;
          proposed["NRPass2ResolutionMode"] = 0u;
          auto expected = original;
          expected.update(proposed);
          Check(!runtime::ValidateConfigurationCapability(requested, &error)
                    && runtime::CanRequestConfigurationEdit(requested, &error)
                    && runtime::ProposedResolutionInheritance(requested) == proposed,
                "Strict conflict stays invalid while the UI gate offers precisely the requested inheritance repair.");
          Check(edit::Prepare(requested) && edit::pending && edit::confirmation_required
                    && edit::before == original && edit::changes == proposed,
                "Prepare stages the requested patch and inheritance repair for either editing order.");
          trace(resolution_first ? "repair-resolution-first-preview" : "repair-count-first-preview",
                edit::changes, edit::pending, edit::error);
          Check(profile::Snapshot() == original && Bytes() == original_ini && writes.size() == original_writes
                    && runtime::configuration_generation.load() == original_generation
                    && runtime::history_reset_epoch.load() == original_history,
                "Repair preview never publishes settings, writes the INI or changes runtime epochs.");
          edit::Cancel();
          Check(!edit::pending && edit::before.empty() && edit::changes.empty() && edit::error.empty()
                    && !edit::Confirm() && profile::Snapshot() == original && Bytes() == original_ini
                    && writes.size() == original_writes,
                "Cancel discards repair preview and a later Confirm cannot commit it.");
          Check(edit::Prepare(requested), "Cancelled repair can be prepared again.");
          for (const auto& [key, value] : proposed.items()) {
            if (original.at(key) == value) continue;
            failed_key = key;
            const bool committed = edit::Confirm();
            failed_key.clear();
            Check(!committed && !edit::error.empty() && edit::pending
                      && profile::Snapshot() == original && Bytes() == original_ini
                      && runtime::configuration_generation.load() == original_generation
                      && runtime::history_reset_epoch.load() == original_history
                      && edit::undo.empty() && edit::redo.empty(),
                  "Failure of each changed repair key rolls back the whole batch without runtime publication or history.");
            trace("repair-write-failure", proposed, committed, edit::error);
          }
          Check(edit::Confirm() && !edit::pending && edit::changes.empty() && edit::before.empty()
                    && edit::error.empty() && profile::Snapshot() == expected && !edit::undo.empty()
                    && edit::redo.empty(),
                "Confirm retries a failed repair and atomically publishes only the requested fields plus inheritance.");
          Check(Read("NRPasses") == "2" && Read("NRPass2ResolutionMode") == "0"
                    && std::abs(std::stof(Read("NRResolutionScale")) - .67f) < .00001f
                    && Read("NRPass2ResolutionScale") == "1",
                "Repair persists the base-67 pair while preserving the unused fixed-100 value.");
          trace("repair-confirmed", proposed, true, edit::error);
          const auto persisted = Bytes();
          runtime::LoadConfiguration();
          Check(profile::Snapshot() == expected && Bytes() == persisted
                    && runtime::QueryConfigurationCapability().CanSubmit(),
                "Production reload retains the confirmed repair and its equal-size eligibility.");
          const auto intensity_before = runtime::intensity.load();
          runtime::kSetIntensity.Commit(.625f);
          auto undo_expected = original;
          undo_expected["NRIntensity"] = .625f;
          auto redo_expected = expected;
          redo_expected["NRIntensity"] = .625f;
          const auto before_failed_undo = profile::Snapshot();
          const auto before_failed_undo_ini = Bytes();
          const auto undo_size = edit::undo.size();
          failed_key = "NRPass2ResolutionMode";
          const bool failed_undo = edit::Undo();
          failed_key.clear();
          Check(!failed_undo && !edit::error.empty() && profile::Snapshot() == before_failed_undo
                    && Bytes() == before_failed_undo_ini && edit::undo.size() == undo_size && edit::redo.empty(),
                "Failed Undo persistence preserves the complete repaired state and retryable history.");
          Check(edit::Undo() && profile::Snapshot() == undo_expected && edit::undo.empty() && !edit::redo.empty(),
                "Undo restores the complete pre-repair batch and retains later unrelated intensity edits.");
          const auto before_failed_redo_ini = Bytes();
          const auto redo_size = edit::redo.size();
          failed_key = "NRPass2ResolutionMode";
          const bool failed_redo = edit::Redo();
          failed_key.clear();
          Check(!failed_redo && !edit::error.empty() && profile::Snapshot() == undo_expected
                    && Bytes() == before_failed_redo_ini && edit::redo.size() == redo_size && edit::undo.empty(),
                "Failed Redo persistence preserves the original batch and retryable redo history.");
          Check(edit::Redo() && profile::Snapshot() == redo_expected && !edit::undo.empty() && edit::redo.empty(),
                "Redo reapplies exactly the repaired batch and retains later unrelated edits.");
          const auto redo_ini = Bytes();
          runtime::LoadConfiguration();
          Check(profile::Snapshot() == redo_expected && Bytes() == redo_ini,
                "Undo / Redo results survive a production reload from the disk-backed fixture.");
          runtime::kSetIntensity.Commit(intensity_before);
          Check(templates::Template(2, defaults) == ultra,
                "Repair / persistence / Undo / Redo never changes the official Ultra template.");
        }

        edit::Cancel();
        edit::undo.clear();
        edit::redo.clear();
        Check(apply(ultra), "Repair input validation starts from the unmodified official Ultra recipe.");
        const auto invalid_before = profile::Snapshot();
        const auto invalid_ini = Bytes();
        const auto invalid_writes = writes.size();
        const auto invalid_generation = runtime::configuration_generation.load();
        const auto invalid_history = runtime::history_reset_epoch.load();
        for (const auto& invalid : std::vector<Json>{
                 {{"NRResolutionScale", std::numeric_limits<float>::quiet_NaN()}},
                 {{"NRResolutionScale", std::numeric_limits<float>::infinity()}},
                 {{"NRResolutionScale", -std::numeric_limits<float>::infinity()}},
                 {{"NRResolutionScale", 1.5f}}, {{"NRResolutionScale", .2f}},
                 {{"NRResolutionScale", -.5f}}, {{"NRResolutionScale", "0.67"}},
                 {{"NRResolutionScale", nullptr}}, {{"NRResolutionScale", true}},
                 {{"NRResolutionScale", .67f}, {"NRPasses", 0u}},
                 {{"NRResolutionScale", .67f}, {"NRPasses", 5u}},
                 {{"NRResolutionScale", .67f}, {"NRPasses", 2.5f}},
                 {{"NRResolutionScale", .67f}, {"NRFollowInputRes", "false"}},
                 {{"NRResolutionScale", .67f}, {"NRIntensity", .5f}},
                 Json::array(), Json::object()}) {
          const auto repair = runtime::ProposedResolutionInheritance(invalid);
          Check(repair.empty(), "Invalid / out-of-schema requests never produce a submit-capable inheritance repair.");
          Check(!edit::Prepare(invalid) && !edit::pending && !edit::confirmation_required
                    && edit::changes.empty() && edit::before.empty()
                    && profile::Snapshot() == invalid_before && Bytes() == invalid_ini
                    && writes.size() == invalid_writes
                    && runtime::configuration_generation.load() == invalid_generation
                    && runtime::history_reset_epoch.load() == invalid_history,
                "Invalid repair requests leave preview, persistence and runtime epochs untouched.");
        }
        edit::Cancel();
        Check(apply(ultra) && edit::Prepare(quick_request), "Stale-preview fixture prepares an Ultra resolution repair.");
        runtime::kSetIntensity.Commit(.375f);
        const auto stale_snapshot = profile::Snapshot();
        const auto stale_ini = Bytes();
        const auto stale_writes = writes.size();
        const auto stale_generation = runtime::configuration_generation.load();
        Check(!edit::Confirm() && !edit::error.empty() && profile::Snapshot() == stale_snapshot
                  && Bytes() == stale_ini && writes.size() == stale_writes
                  && runtime::configuration_generation.load() == stale_generation,
              "Changed snapshot rejects repair confirmation without overwriting the intervening edit.");
        trace("repair-stale-snapshot", edit::changes, false, edit::error);
        edit::Cancel();
        Check(apply(ultra) && edit::Prepare(quick_request), "Environment-change fixture opens a supported repair.");
        runtime::d3d12_swapchain_seen = false;
        runtime::d3d11_swapchain_seen = false;
        const auto environment_snapshot = profile::Snapshot();
        const auto environment_ini = Bytes();
        const auto environment_writes = writes.size();
        const auto environment_generation = runtime::configuration_generation.load();
        Check(!edit::Confirm() && !edit::error.empty() && profile::Snapshot() == environment_snapshot
                  && Bytes() == environment_ini && writes.size() == environment_writes
                  && runtime::configuration_generation.load() == environment_generation,
              "Losing the identified backend after Prepare rejects final confirmation without writes.");
        trace("repair-environment-changed", edit::changes, false, edit::error);
        runtime::d3d12_swapchain_seen = !bridge;
        runtime::d3d11_swapchain_seen = bridge;
        Check(edit::Confirm(), "An unchanged repair can retry after its supported backend returns.");
        Check(runtime::ProposedResolutionInheritance(quick_request).empty(),
              "Already matching inherited configuration does not propose a redundant inheritance repair.");
        edit::Cancel();
        auto inherited_half = ultra;
        inherited_half.update({{"NRResolutionScale", .5f}, {"NRPass2ResolutionMode", 0u},
                               {"NRPass2ResolutionScale", 1.f}});
        Check(apply(inherited_half), "Fixed-choice fixture starts with inherited 50 and unused fixed 100.");
        const auto inherited_before = profile::Snapshot();
        const auto inherited_ini = Bytes();
        const auto inherited_writes = writes.size();
        const Json fixed_half{{"NRPass2ResolutionMode", 2u}, {"NRPass2ResolutionScale", .5f}};
        Check(!runtime::CanChooseSetting(runtime::kSetPassResolutionMode[0], 2.f, &error)
                  && runtime::ValidateConfigurationCapability(fixed_half, &error),
              "Mode-only fixed-100 is rejected but explicit mode plus fixed-50 is a legal complete candidate.");
        Check(edit::Prepare(fixed_half) && edit::pending && !edit::confirmation_required
                  && edit::changes == fixed_half && edit::before == inherited_before
                  && profile::Snapshot() == inherited_before && Bytes() == inherited_ini
                  && writes.size() == inherited_writes,
              "Prepare accepts explicit fixed mode and scale as one batch without collateral confirmation or writes.");
        auto fixed_expected = inherited_before;
        fixed_expected.update(fixed_half);
        failed_key = "NRPass2ResolutionScale";
        const bool fixed_committed = edit::Confirm();
        failed_key.clear();
        Check(!fixed_committed && !edit::error.empty() && profile::Snapshot() == inherited_before
                  && Bytes() == inherited_ini,
              "Failed explicit fixed-scale batch restores the inherited mode and old saved scale together.");
        Check(edit::Confirm() && profile::Snapshot() == fixed_expected
                  && Read("NRPass2ResolutionMode") == "2" && Read("NRPass2ResolutionScale") == "0.5",
              "Inherited 50 becomes independent fixed 50 through one confirmed persistence batch.");
        trace("explicit-fixed-half", fixed_half, true, edit::error);
        const auto fixed_ini = Bytes();
        runtime::LoadConfiguration();
        Check(profile::Snapshot() == fixed_expected && Bytes() == fixed_ini
                  && runtime::QueryConfigurationCapability().CanSubmit(),
              "Explicit fixed 50 survives production reload and remains eligible with FG.");
        Check(edit::Undo() && profile::Snapshot() == inherited_before
                  && edit::Redo() && profile::Snapshot() == fixed_expected,
              "Undo / Redo restores both mode and scale for the explicit fixed-50 batch.");
        edit::Cancel();
        edit::undo.clear();
        edit::redo.clear();
      }
    }
  }
  xefg::owns_stream = saved_xefg_owner;
  runtime::d3d12_swapchain_seen = saved_dx12;
  runtime::d3d11_swapchain_seen = saved_dx11;
  runtime::dx11_route = saved_dx11_route;
  runtime::auto_fg_fallback_active = saved_auto_fallback;
  runtime::native_fg_declaration = runtime::NativeFgDeclaration::kOff;
  runtime::native_fg_public_off = true;
  Check(apply(pair), "Resolution editing cases restore the following capability fixture.");
}
