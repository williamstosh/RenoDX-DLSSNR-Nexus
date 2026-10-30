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
  edit::pending = false;
  Check(profile::Snapshot() == before && Bytes() == ini_before, "Cancel preserves exact original state.");
  Check(edit::Prepare(runtime::kSetHookPoint, 0.f) && edit::changes.size() == 1
            && !edit::changes.contains("NRPass2Stage"), "Compatible explicit P2 is not rewritten.");
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
  edit::pending = false;
  edit::error.clear();
  edit::notice.clear();
  edit::undo.clear();
  Check(apply(pair), "Stage transaction tests restore the following capability fixture.");
}
