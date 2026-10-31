/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Composed in stage_plan_tests main; proposals do not grant execution support.
{
  const planner::Configuration present_pair{.pass_count = 2, .hook_point = 2, .stages = {3, 1, 2}};
  const auto render_recovery = planner::ProposeStageChange(present_pair, 0, 1);
  const auto upscaled_recovery = planner::ProposeStageChange(present_pair, 0, 0);
  check(planner::Build(render_recovery).valid && render_recovery.stages[0] == 2,
        "Present pair can reach Render plus Upscaled with the closest legal tail");
  check(planner::Build(upscaled_recovery).valid && upscaled_recovery.stages[0] == 2,
        "Present pair can reach double Upscaled without retaining illegal mixed Present");
  const planner::Configuration original{.pass_count = 2, .hook_point = 1, .stages = {2, 1, 3}};
  const auto present = planner::ProposeStageChange(original, 0, 2);
  check(present.hook_point == 2 && present.stages == std::array<uint32_t, 3>{3, 1, 3},
        "P1 Present promotes only the active incompatible tail");
  const auto upscaled = planner::ProposeStageChange(original, 0, 0);
  check(upscaled.stages == original.stages && original.hook_point == 1,
        "P1 Upscaled preserves compatible P2 and leaves original unchanged");
  for (uint32_t count = 1; count <= 4; ++count) {
    for (uint32_t mask = 0; mask < 64; ++mask) {
      planner::Configuration before{.pass_count = count, .hook_point = 1,
          .stages = {mask % 4, (mask / 4) % 4, (mask / 16) % 4}};
      if (!planner::Build(before).valid) continue;
      for (uint32_t pass = 0; pass < count; ++pass) {
        for (uint32_t choice = 0; choice <= (pass == 0 ? 2u : 3u); ++choice) {
          const auto after = planner::ProposeStageChange(before, pass, choice);
          for (uint32_t index = 1; index < 4; ++index) {
            if (index < pass || index >= count) {
              check(after.stages[index - 1] == before.stages[index - 1],
                    "proposal preserves preceding and inactive stored stages");
            }
          }
          const auto proposed = planner::Build(after);
          if (pass == 0) {
            check(proposed.reason != Reason::BACKWARD_STAGE, "root edit repairs the complete active tail");
          }
        }
      }
    }
  }
  const auto mixed = planner::ProposeStageChange(
      {.pass_count = 2, .hook_point = 1, .stages = {3, 0, 0}}, 0, 0);
  check(planner::Build(mixed).valid && mixed.stages[0] == 2,
        "stage search replaces an unsupported mixed Present tail with eligible Upscaled");
  check(planner::Build(present, {.present_available = false}).reason == Reason::PRESENT_AFTER,
        "unavailable Present remains distinguishable from configuration success");
  check(planner::RequiresConfirmation(original, present, 0), "explicit tail repair requires confirmation");
  check(!planner::RequiresConfirmation(original, upscaled, 0), "compatible explicit tail needs no confirmation");
  for (uint32_t count = 1; count <= 4; ++count) {
    for (uint32_t hook = 0; hook <= 2; ++hook) {
      for (uint32_t code = 0; code < 64; ++code) {
        const planner::Configuration before{.pass_count = count, .hook_point = hook,
            .stages = {code % 4, (code / 4) % 4, (code / 16) % 4}};
        const auto old_plan = planner::Build(before);
        if (old_plan.reason == Reason::BACKWARD_STAGE) continue;
        for (uint32_t pass = 0; pass < count; ++pass) {
          for (uint32_t choice = 0; choice <= (pass == 0 ? 2u : 3u); ++choice) {
            const auto after = planner::ProposeStageChange(before, pass, choice);
            const auto candidate = planner::Build(after);
            bool collateral = false;
            for (uint32_t index = 0; index < count; ++index) {
              if (index == pass) continue;
              collateral |= old_plan.requested[index] != candidate.requested[index]
                  || (index != 0 && before.stages[index - 1] != after.stages[index - 1]);
            }
            check(planner::RequiresConfirmation(before, after, pass) == collateral,
                  "one to four passes confirm exactly the collateral requested or stored stages");
            for (uint32_t flags = 0; flags < 64; ++flags) {
              const planner::Capabilities caps{.mixed_dx12 = (flags & 1) != 0, .mixed_dx11 = (flags & 2) != 0,
                  .ray_reconstruction = (flags & 4) != 0, .present_available = (flags & 8) != 0,
                  .fg_owns_input = (flags & 16) != 0, .mixed_present_dx12 = (flags & 32) != 0};
              const auto plan = planner::Build(after, caps);
              const bool b1 = caps.mixed_dx12 && caps.mixed_present_dx12 && caps.present_available
                  && !caps.ray_reconstruction && count == 2 && candidate.requested[0] == Stage::UPSCALED;
              const bool ordered = candidate.reason != Reason::BACKWARD_STAGE;
              const bool eligible = ordered && (!candidate.mixed || ((caps.mixed_dx12 || caps.mixed_dx11)
                  && !caps.fg_owns_input && (candidate.requested[count - 1] != Stage::PRESENT || b1)));
              check(plan.valid == eligible, "edited complete chains retain all capability combinations");
            }
          }
        }
      }
    }
  }
  const planner::Configuration inherited{.pass_count = 4, .hook_point = 1};
  for (uint32_t count = 1; count <= 4; ++count) {
    const planner::Configuration source{.pass_count = count, .hook_point = 2, .stages = {3, 3, 3}};
    for (uint32_t flags = 0; flags < 64; ++flags) {
      const planner::Capabilities caps{.mixed_dx12 = (flags & 1) != 0, .mixed_dx11 = (flags & 2) != 0,
          .ray_reconstruction = (flags & 4) != 0, .present_available = (flags & 8) != 0,
          .fg_owns_input = (flags & 16) != 0, .mixed_present_dx12 = (flags & 32) != 0};
      const auto eligible = [&](const planner::Configuration& candidate) {
        const auto plan = planner::Build(candidate, caps);
        return plan.valid && plan.reason == Reason::NONE;
      };
      for (uint32_t pass = 0; pass < count; ++pass) {
        for (uint32_t choice = 0; choice <= (pass == 0 ? 2u : 3u); ++choice) {
          if ((pass == 0 ? source.hook_point : source.stages[pass - 1]) == choice) continue;
          bool reachable = false;
          uint32_t minimum_changes = UINT32_MAX;
          for (uint32_t hook = 0; hook < 3; ++hook) {
            if (pass != 0 && hook != source.hook_point) continue;
            for (uint32_t code = 0; code < (1u << (2 * (count - 1))); ++code) {
              auto candidate = source;
              candidate.hook_point = hook;
              uint32_t rest = code;
              uint32_t changes = (pass != 0 && hook != source.hook_point ? 1u : 0u);
              for (uint32_t index = 1; index < count; ++index) {
                candidate.stages[index - 1] = rest % 4;
                rest /= 4;
                changes += index != pass && candidate.stages[index - 1] != source.stages[index - 1];
              }
              bool preceding_kept = true;
              for (uint32_t index = 1; index < pass; ++index) {
                preceding_kept &= candidate.stages[index - 1] == source.stages[index - 1];
              }
              if (!preceding_kept || (pass == 0 ? hook : candidate.stages[pass - 1]) != choice || !eligible(candidate)) continue;
              reachable = true;
              minimum_changes = (changes < minimum_changes ? changes : minimum_changes);
            }
          }
          const auto proposed = planner::ProposeStageChange(source, pass, choice, eligible);
          check(eligible(proposed) == reachable, "candidate search reaches every available selection across capability combinations");
          uint32_t changed = (pass != 0 && proposed.hook_point != source.hook_point ? 1u : 0u);
          for (uint32_t index = 1; index < count; ++index) {
            changed += index != pass && proposed.stages[index - 1] != source.stages[index - 1];
          }
          check(!reachable || changed == minimum_changes, "reachable proposals minimize collateral stored Stage changes");
          check((pass == 0 ? proposed.hook_point : proposed.stages[pass - 1]) == choice,
                "search never substitutes the user's selected Stage");
          for (uint32_t index = count; index < 4; ++index) {
            check(proposed.stages[index - 1] == source.stages[index - 1], "search preserves inactive Stage settings");
          }
        }
      }
    }
  }
  check(planner::RequiresConfirmation(inherited, planner::ProposeStageChange(inherited, 0, 0), 0),
        "inherited effective tail changes require confirmation despite a single stored write");
  const auto invalid_root = planner::Build({.hook_point = 3});
  check(!invalid_root.valid && invalid_root.reason == Reason::INVALID_STAGE && invalid_root.group_count == 0,
        "invalid root selection cannot masquerade as Upscaled");
  const planner::Configuration backward{.pass_count = 3, .hook_point = 2, .stages = {1, 1}};
  check(planner::ProposeStageChange(backward, 0, 2).stages == backward.stages,
        "reselecting the same value does not silently repair stored stages");
}
