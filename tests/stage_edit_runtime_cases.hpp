/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
{
  namespace edit = runtime::stage_edit;
  auto original = pair;
  original["NRHookPoint"] = 1u;
  original["NRPass2Stage"] = 2u;
  Check(apply(original), "Stage transaction starts from supported Render to Upscaled.");
  const auto before = profile::Snapshot();
  const auto ini_before = Bytes();
  const auto write_count = writes.size();
  Check(runtime::CanChooseSetting(runtime::kSetHookPoint, 2.f, &error),
        "P1 Present is selectable after evaluating the entire repaired candidate.");
  Check(edit::Prepare(runtime::kSetHookPoint, 2.f) && edit::changes.size() == 2
            && edit::changes.at("NRPass2Stage") == 3u,
        "Present preview proposes both stages in one transaction.");
  Check(profile::Snapshot() == before && Bytes() == ini_before && writes.size() == write_count,
        "Preview does not publish values or persist any configuration.");
  Check(edit::confirmation_required, "Explicit collateral stages require a modal confirmation.");
  edit::Cancel();
  Check(!edit::pending && edit::changes.empty() && edit::before.empty() && edit::error.empty(),
        "Cancel discards the staged transaction without touching committed Undo.");
  Check(profile::Snapshot() == before && Bytes() == ini_before, "Cancel preserves exact original state.");
  Check(edit::Prepare(runtime::kSetHookPoint, 0.f) && edit::changes.size() == 1
            && !edit::changes.contains("NRPass2Stage"), "Compatible explicit P2 is not rewritten.");
  Check(!edit::confirmation_required, "Only the selected pass changes; no modal confirmation is needed.");
  Check(edit::Confirm(), "Supported minimal stage change commits successfully.");
  Check(adapter::Undo(&error, &edit::undo) && profile::Snapshot() == before, "Existing Undo restores stage changes.");
  Check(edit::Prepare(runtime::kSetHookPoint, 2.f), "Present candidate can be prepared again.");
  const auto generation = runtime::configuration_generation.load();
  failed_key = "NRPass2Stage";
  Check(!edit::Confirm(), "Stage transaction propagates a failed configuration write.");
  failed_key.clear();
  Check(profile::Snapshot() == before && Bytes() == ini_before
            && runtime::configuration_generation.load() == generation,
        "Failed batch restores saved values and never publishes partial runtime stages.");
  Check(edit::Confirm(), "Failed transaction can be retried after persistence becomes writable.");
  Check(edit::error.empty() && edit::changes.empty() && edit::before.empty(),
        "Successful retry clears stale failure and preview state.");
  Check(runtime::hook_point.load() == 2 && runtime::pass_stage[0].load() == 3
            && runtime::configuration_generation.load() == generation + 1,
        "Both stages become visible together with one feature recreation.");
  auto expected = before;
  expected.update({{"NRHookPoint", 2u}, {"NRPass2Stage", 3u}});
  Check(profile::Snapshot() == expected, "Models, independent sizes, appearance and result fields are preserved.");
  runtime::kSetIntensity.Commit(.625f);
  Check(adapter::Undo(&error, &edit::undo) && runtime::intensity.load() == .625f
            && runtime::hook_point.load() == 1 && runtime::pass_stage[0].load() == 2,
        "Undo restores stages while retaining later unrelated edits.");
  Check(edit::Prepare(runtime::kSetHookPoint, 2.f), "Revalidation fixture opens a preview.");
  runtime::native_fg_declaration = runtime::NativeFgDeclaration::kOn;
  runtime::native_fg_public_off = false;
  const auto on_snapshot = profile::Snapshot();
  const auto on_ini = Bytes();
  Check(!edit::Confirm() && profile::Snapshot() == on_snapshot && Bytes() == on_ini,
        "FG environment changes between preview and confirm cannot bypass capability checks.");
  runtime::native_fg_declaration = runtime::NativeFgDeclaration::kOff;
  runtime::native_fg_public_off = true;
  Check(edit::Prepare(runtime::kSetHookPoint, 2.f), "Changed-settings fixture opens a preview.");
  runtime::kSetPassPreset[0].Commit(2.f);
  const auto edited = profile::Snapshot();
  const auto edited_ini = Bytes();
  Check(!edit::Confirm() && profile::Snapshot() == edited && Bytes() == edited_ini,
        "A stale preview never overwrites settings changed after it opened.");
  edit::Cancel();
  for (const bool bridge : {false, true}) {
    runtime::d3d12_swapchain_seen = !bridge;
    runtime::d3d11_swapchain_seen = bridge;
    runtime::dx11_route = runtime::kDx11RouteNative;
    for (uint32_t mode = 0; mode < 3; ++mode) {
      runtime::auto_fg_fallback_active = false;
      for (uint32_t count = 2; count <= 4; ++count) {
        runtime::experimental_present_chain = false;
        runtime::native_fg_owns = false;
        auto request = pair;
        request.update({{"NRPasses", count}, {"NRHookPoint", 2u}, {"NRPass2Stage", 3u},
                        {"NRPass3Stage", 3u}, {"NRPass4Stage", 3u}, {"NRPass2ResolutionMode", 0u}});
        Check(apply(request), "Present chain recovery fixture applies on both native backends.");
        // RR's Render-to-Upscaled remap is rejected by the editing capability
        // gate: search must not replace the user's requested root to hide it.
        runtime::stage_source_rr = mode == 2;
        runtime::experimental_present_chain = mode == 1;
        runtime::native_fg_owns = mode == 1;
        const auto saved = profile::Snapshot();
        const auto ini = Bytes();
        for (const float hook : {1.f, 0.f}) {
          const bool available = mode != 2 || hook == 0.f;
          Check(runtime::CanChooseSetting(runtime::kSetHookPoint, hook, &error) == available,
                "Actual backend and RR capability decide whether the selected root is reachable.");
          Check(edit::Prepare(runtime::kSetHookPoint, hook) == available,
                "Candidate search reaches a legal complete chain exactly when the selected root is supported.");
          Check(profile::Snapshot() == saved && Bytes() == ini, "Search never publishes or persists candidates.");
          if (available) {
            auto expected = saved;
            expected.update(edit::changes);
            Check(edit::confirmation_required && runtime::ValidateConfigurationCapability(edit::changes, &error),
                  "Searched collateral stages are eligible and require confirmation.");
            const uint32_t tail = (hook == 1.f && mode == 1 ? 1u : 2u);
            for (uint32_t pass = 1; pass < count; ++pass) {
              Check(expected.at(runtime::kSetPassStage[pass - 1].key) == tail,
                    "Search prefers Render plus Upscaled, falls back to Render only, or selects Upscaled only.");
            }
            Check(edit::Confirm() && profile::Snapshot() == expected,
                  "Searched candidate changes only the proposed Stage fields.");
            Check(adapter::Undo(&error, &edit::undo) && profile::Snapshot() == saved,
                  "Undo restores the complete Present chain after candidate search.");
          }
          edit::Cancel();
        }
        runtime::stage_source_rr = false;
        runtime::native_fg_owns = false;
        runtime::experimental_present_chain = false;
      }
    }
  }
  runtime::d3d12_swapchain_seen = true;
  runtime::d3d11_swapchain_seen = false;
  for (uint32_t count = 1; count <= 4; ++count) {
    auto request = pair;
    request.update({{"NRPasses", count}, {"NRHookPoint", 1u}, {"NRPass2Stage", 0u},
                    {"NRPass2ResolutionMode", 0u}});
    Check(apply(request), "Inherited one to four pass fixture is supported.");
    const auto saved = profile::Snapshot();
    const auto ini = Bytes();
    Check(edit::Prepare(runtime::kSetHookPoint, 0.f) && edit::confirmation_required == (count > 1),
          "Inherited tail changes require confirmation only when other active passes are affected.");
    Check(edit::changes.size() == 1 && profile::Snapshot() == saved && Bytes() == ini,
          "Inherited stage preview preserves all explicit settings and persistence.");
    edit::Cancel();
    Check(!edit::Confirm() && profile::Snapshot() == saved && Bytes() == ini,
          "Cancelled transactions cannot later commit.");
    const auto* setting = (count == 1 ? &runtime::kSetHookPoint : &runtime::kSetPassStage[count - 2]);
    const float choice = (count == 1 ? 0.f : 2.f);
    Check(edit::Prepare(*setting, choice) && !edit::confirmation_required,
          "Editing the final active pass has no collateral confirmation.");
    const auto epoch = runtime::configuration_generation.load();
    failed_key = setting->key;
    Check(!edit::Confirm() && profile::Snapshot() == saved && Bytes() == ini
              && runtime::configuration_generation.load() == epoch,
          "Direct stage edits also roll back failed writes before runtime publication.");
    failed_key.clear();
    Check(edit::Confirm() && edit::error.empty(), "Direct stage edits can retry after failed persistence.");
    Check(adapter::Undo(&error, &edit::undo) && profile::Snapshot() == saved,
          "Undo restores final-pass edits for every active pass count.");
    Check(!edit::Prepare(*setting, setting->Get()) && !edit::pending && edit::changes.empty(),
          "Reselecting the current Stage produces no transaction.");
    Check(!runtime::CanChooseSetting(*setting, -1.f) && !edit::Prepare(*setting, 1.5f),
          "Invalid Stage requests never truncate into valid choices.");
  }
  edit::pending = false;
  edit::error.clear();
  edit::notice.clear();
  edit::undo.clear();
  Check(apply(pair), "Stage transaction tests restore the following capability fixture.");
}
