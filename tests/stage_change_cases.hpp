/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Composed in stage_plan_tests main; proposals do not grant execution support.
{
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
            } else if (index > pass && before.stages[index - 1] == 0) {
              check(after.stages[index - 1] == 0, "proposal preserves downstream inheritance");
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
  check(!planner::Build(mixed).valid && planner::Build(mixed).reason == Reason::MIXED_PRESENT,
        "stage proposal retains unsupported mixed Present constraints");
  check(planner::Build(present, {.present_available = false}).reason == Reason::PRESENT_AFTER,
        "unavailable Present remains distinguishable from configuration success");
}
