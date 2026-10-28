/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

#include <nr_model.hpp>
#include <evaluate_chain.hpp>
#include <stage_plan.hpp>

int main() {
  namespace planner = renodx::addons::dlss5::stage_plan;
  namespace model = renodx::addons::dlss5::nr_model;
  using planner::Reason;
  using planner::Stage;

  uint32_t checks = 0;
  uint32_t failures = 0;
  uint32_t configurations = 0;
  uint32_t capability_cases = 0;
  uint32_t model_cases = 0;
  std::string context;
  const auto check = [&](bool condition, std::string_view description) {
    ++checks;
    if (!condition) {
      ++failures;
      std::cerr << "FAIL: " << description << " [" << context << "]\n";
    }
  };
  constexpr std::array hook_stages = {Stage::UPSCALED, Stage::RENDER, Stage::PRESENT};
  constexpr std::array explicit_stages = {Stage::RENDER, Stage::UPSCALED, Stage::PRESENT};
  constexpr std::array all_stages = {Stage::RENDER, Stage::UPSCALED, Stage::PRESENT};

  for (uint32_t count = 1, combinations = 1; count <= 4; ++count, combinations *= 4) {
    for (uint32_t hook = 0; hook < hook_stages.size(); ++hook) {
      context = "legacy count=" + std::to_string(count) + " hook=" + std::to_string(hook);
      const planner::Configuration legacy{.pass_count = count, .hook_point = hook};
      const auto legacy_plan = planner::Build(legacy);
      check(legacy_plan.valid && !legacy_plan.mixed && legacy_plan.reason == Reason::NONE,
            "absent stage fields retain the old single-stage chain");
      check(legacy_plan.group_count == 1 && legacy_plan.groups[0].stage == hook_stages[hook]
                && legacy_plan.groups[0].begin == 0 && legacy_plan.groups[0].count == count,
            "old HookPoint values and pass count remain unchanged");

      for (uint32_t code = 0; code < combinations; ++code) {
        planner::Configuration config{.pass_count = count, .hook_point = hook};
        std::array<Stage, 4> requested{};
        requested[0] = hook_stages[hook];
        uint32_t remaining = code;
        for (uint32_t pass = 1; pass < count; ++pass) {
          const uint32_t choice = remaining % 4;
          remaining /= 4;
          config.stages[pass - 1] = choice;
          requested[pass] = (choice == 0 ? requested[pass - 1] : explicit_stages[choice - 1]);
        }
        ++configurations;
        const auto saved_config = config;
        bool ordered = true;
        bool mixed = false;
        bool has_present = false;
        for (uint32_t pass = 0; pass < count; ++pass) {
          has_present |= requested[pass] == Stage::PRESENT;
          if (pass > 0) {
            ordered &= requested[pass - 1] <= requested[pass];
            mixed |= requested[pass - 1] != requested[pass];
          }
        }

        for (uint32_t flags = 0; flags < 32; ++flags) {
          context = "count=" + std::to_string(count) + " hook=" + std::to_string(hook)
                    + " stages=" + std::to_string(code) + " caps=" + std::to_string(flags);
          const planner::Capabilities caps{
              .mixed_dx12 = (flags & 1) != 0,
              .mixed_dx11 = (flags & 16) != 0,
              .ray_reconstruction = (flags & 2) != 0,
              .present_available = (flags & 4) != 0,
              .fg_owns_input = (flags & 8) != 0};
          const auto saved_caps = caps;
          const auto plan = planner::Build(config, caps);
          ++capability_cases;
          const bool accepted = ordered && (!mixed || ((caps.mixed_dx12 || caps.mixed_dx11) && !has_present && !caps.fg_owns_input));
          check(plan.pass_count == count && plan.requested == requested && plan.mixed == mixed,
                "requested plan preserves explicit choices and previous-pass inheritance");
          check(plan.valid == accepted, "only ordered, supported stage combinations are executable");
          check(config.pass_count == saved_config.pass_count && config.hook_point == saved_config.hook_point
                    && config.stages == saved_config.stages,
                "planning and fallback never overwrite saved configuration");
          check(caps.mixed_dx12 == saved_caps.mixed_dx12 && caps.mixed_dx11 == saved_caps.mixed_dx11
                    && caps.ray_reconstruction == saved_caps.ray_reconstruction
                    && caps.present_available == saved_caps.present_available && caps.fg_owns_input == saved_caps.fg_owns_input,
                "capability inputs remain unchanged");
          if (!accepted) {
            check(plan.group_count == 0 && plan.effective == requested,
                  "rejected chains expose their request without executable or partial groups");
            if (!ordered) {
              check(plan.reason == Reason::BACKWARD_STAGE, "backward passes are rejected before capability fallback");
            } else if (!caps.mixed_dx12 && !caps.mixed_dx11) {
              check(plan.reason == Reason::UNSUPPORTED_BACKEND, "unsupported backend explains mixed-chain rejection");
            } else if (has_present) {
              check(plan.reason == Reason::MIXED_PRESENT, "mixed Present remains unvalidated even when Present is unavailable");
            } else {
              check(plan.reason == Reason::FG_OWNS_INPUT, "FG input ownership prevents mixed scheduling");
            }
            continue;
          }

          auto effective = requested;
          bool rr_fallback = false;
          bool present_fallback = false;
          for (uint32_t pass = 0; pass < count; ++pass) {
            rr_fallback |= caps.ray_reconstruction && requested[pass] == Stage::RENDER;
            present_fallback |= !caps.present_available && requested[pass] == Stage::PRESENT;
            if ((caps.ray_reconstruction && requested[pass] == Stage::RENDER)
                || (!caps.present_available && requested[pass] == Stage::PRESENT)) {
              effective[pass] = Stage::UPSCALED;
            }
          }
          check(plan.effective == effective, "safe RR and legacy Present fallback preserve logical pass order");
          check(plan.reason == (rr_fallback ? Reason::RR_AFTER : (present_fallback ? Reason::PRESENT_AFTER : Reason::NONE)),
                "fallback explains the effective stage without changing the requested stage");

          uint32_t covered = 0;
          uint32_t expected_groups = 1;
          for (uint32_t pass = 1; pass < count; ++pass) {
            expected_groups += effective[pass] != effective[pass - 1];
          }
          check(plan.group_count == expected_groups, "equal adjacent effective stages form maximal groups");
          for (uint32_t group_index = 0; group_index < plan.group_count; ++group_index) {
            const auto group = plan.groups[group_index];
            check(group.begin == covered && group.count > 0 && group.begin + group.count <= count,
                  "group ranges have no gaps, overlap or out-of-bounds pass");
            for (uint32_t pass = group.begin; pass < group.begin + group.count && pass < count; ++pass) {
              check(group.stage == effective[pass], "each logical pass runs in its effective stage group exactly once");
            }
            covered += group.count;
          }
          check(covered == count, "groups cover the entire chain exactly once");
          for (const auto stage : all_stages) {
            uint32_t expected_begin = 0;
            uint32_t expected_count = 0;
            for (uint32_t pass = 0; pass < count; ++pass) {
              if (effective[pass] == stage) {
                if (expected_count == 0) expected_begin = pass;
                ++expected_count;
              }
            }
            const auto group = plan.At(stage);
            check(group.stage == stage && group.begin == expected_begin && group.count == expected_count,
                  "stage lookup returns exactly its logical pass range, or an empty group");
          }
          if (rr_fallback) {
            check(plan.group_count == 1 && plan.At(Stage::UPSCALED).count == count && plan.At(Stage::RENDER).count == 0,
                  "RR safely merges Render and Upscaled passes into one ordered Upscaled group");
          }

          // Model choices are an independent contract: enumerate all A/B assignments,
          // including A->B, B->A, A->A and B->B at every legal stage split.
          for (uint32_t models = 0; models < (1u << count); ++models) {
            for (uint32_t pass = 0; pass < count; ++pass) {
              const uint32_t selected = ((models >> pass) & 1u) + 1;
              check(model::ResolvePreset(1, selected, true) == selected,
                    "stage and fallback never restrict an explicit A/B model selection");
            }
            const auto repeated = planner::Build(config, caps);
            check(repeated.requested == plan.requested && repeated.effective == plan.effective
                      && repeated.group_count == plan.group_count && repeated.reason == plan.reason,
                  "resolving model choices leaves the stage plan deterministic");
            ++model_cases;
          }
        }
      }
    }
  }

  context = "model compatibility";
  check(model::SupportsSmallModel(310, 9, 16, 0) && !model::SupportsSmallModel(310, 8, 0, 0),
        "dual-model support uses the identified runtime");
  for (const bool dual_models : {false, true}) {
    check(model::ResolvePreset(0, 0, dual_models) == (dual_models ? 2u : 0u),
          "Auto defaults to B only on the supported dual-model runtime");
    check(model::ResolvePreset(1, 0, dual_models) == 1,
          "missing per-pass model inherits main A independently of stage inheritance");
    check(model::ResolvePreset(2, 0, dual_models) == (dual_models ? 2u : 1u),
          "inherited B retains the existing safe runtime fallback");
    check(model::ResolvePreset(1, 2, dual_models) == (dual_models ? 2u : 1u),
          "explicit B overrides main A without changing saved selections");
  }

  for (uint32_t count = 1; count <= 4; ++count) {
    for (uint32_t invalid_pass = 1; invalid_pass < count; ++invalid_pass) {
      for (const uint32_t bad : {4u, 255u, std::numeric_limits<uint32_t>::max()}) {
        context = "invalid stage pass=" + std::to_string(invalid_pass) + " value=" + std::to_string(bad);
        planner::Configuration config{.pass_count = count, .hook_point = 1};
        config.stages[invalid_pass - 1] = bad;
        const auto plan = planner::Build(config);
        check(!plan.valid && plan.reason == Reason::INVALID_STAGE && plan.group_count == 0,
              "invalid active stage choices are rejected with an explicit reason");
        config.pass_count = invalid_pass;
        const auto inactive = planner::Build(config);
        check(inactive.valid && inactive.group_count == 1 && inactive.groups[0].count == invalid_pass,
              "inactive future-pass fields do not disturb the active chain");
      }
    }
  }
  context = "pass count boundaries";
  for (const uint32_t count : {0u, 5u, std::numeric_limits<uint32_t>::max()}) {
    const auto plan = planner::Build({.pass_count = count});
    check(plan.valid && plan.group_count == 1 && plan.pass_count == (count == 0 ? 1u : 4u)
              && plan.groups[0].count == plan.pass_count,
          "out-of-range pass counts retain the one-to-four-pass limit");
  }
  context = "mixed source terminal accounting";
  namespace nr = renodx::addons::dlss5;
  const auto declines = [](nr::NrDeclineReason reason) {
    return nr::nr_decline_counts[static_cast<size_t>(reason)].load();
  };
  const auto warmup_before = declines(nr::NrDeclineReason::kSlotWarmupMaturation);
  const auto predecessor_before = declines(nr::NrDeclineReason::kStagePredecessorIncomplete);
  const auto mirror_before = declines(nr::NrDeclineReason::kNestedMirror);
  const auto injected_before = nr::mixed_stage_source_successes.load();
  {
    const nr::EvaluateChainScope outer;
    nr::evaluate_chain.game_wrappers = 2;
    nr::mixed_stage_terminal.active = true;
    nr::CountNrDecline(nr::NrDeclineReason::kSlotWarmupMaturation);
    {
      const nr::EvaluateChainScope inner;
      nr::CountNrDecline(nr::NrDeclineReason::kNestedMirror);
      nr::CountNrDecline(nr::NrDeclineReason::kStagePredecessorIncomplete);
      nr::evaluate_chain.MarkInjected(reinterpret_cast<void*>(uintptr_t{1}), 1);
      check(nr::evaluate_chain.Injected(reinterpret_cast<void*>(uintptr_t{1}), 1)
                && !nr::evaluate_chain.Injected(reinterpret_cast<void*>(uintptr_t{1}), 0),
            "same resource admits different stages but not a mirror of the same stage");
    }
    check(declines(nr::NrDeclineReason::kSlotWarmupMaturation) == warmup_before,
          "group declines wait for the source call terminal");
  }
  check(declines(nr::NrDeclineReason::kSlotWarmupMaturation) == warmup_before + 1
            && declines(nr::NrDeclineReason::kStagePredecessorIncomplete) == predecessor_before
            && declines(nr::NrDeclineReason::kNestedMirror) == mirror_before + 1,
        "two skipped groups publish one original cause and one nested wrapper mirror");
  {
    const nr::EvaluateChainScope outer;
    nr::evaluate_chain.game_wrappers = 1;
    nr::mixed_stage_terminal = {.active = true, .injected = true};
    nr::CountNrDecline(nr::NrDeclineReason::kSlotWarmupMaturation);
  }
  check(nr::mixed_stage_source_successes.load() == injected_before + 1
            && declines(nr::NrDeclineReason::kSlotWarmupMaturation) == warmup_before + 1,
        "a partial injected chain counts once without adding a second decline terminal");
  const auto failed_before = declines(nr::NrDeclineReason::kGameEvaluateFailed);
  {
    const nr::EvaluateChainScope outer;
    nr::mixed_stage_terminal = {.active = true, .injected = true};
    nr::CountNrDecline(nr::NrDeclineReason::kSlotWarmupMaturation);
    nr::CountNrDecline(nr::NrDeclineReason::kGameEvaluateFailed);
  }
  check(nr::mixed_stage_source_successes.load() == injected_before + 1
            && declines(nr::NrDeclineReason::kGameEvaluateFailed) == failed_before + 1
            && !nr::mixed_stage_terminal.active && nr::evaluate_chain.depth == 0,
        "failed game SR overrides pre-SR work and the following source call starts clean");
  context = "several source SR calls under one Streamline evaluate";
  const auto multi_success_before = nr::mixed_stage_source_successes.load();
  const auto multi_failed_before = declines(nr::NrDeclineReason::kGameEvaluateFailed);
  const auto multi_mirror_before = declines(nr::NrDeclineReason::kNestedMirror);
  {
    const nr::EvaluateChainScope streamline;
    {
      const nr::SourceStageScope first(reinterpret_cast<void*>(uintptr_t{1}), true);
      nr::mixed_stage_terminal = {.active = true, .injected = true};
      nr::evaluate_chain.render_attempted = true;
      nr::evaluate_chain.configuration_generation = 73;
      {
        const nr::SourceStageScope mirror(reinterpret_cast<void*>(uintptr_t{1}), true);
        check(nr::evaluate_chain.game_wrappers == 2 && nr::evaluate_chain.render_attempted,
              "same-source nested wrappers share one stage state");
      }
      {
        const nr::SourceStageScope different(reinterpret_cast<void*>(uintptr_t{2}), true);
        check(nr::evaluate_chain.game_wrappers == 1 && !nr::evaluate_chain.render_attempted
                  && !nr::mixed_stage_terminal.active,
              "another nested source starts from independent stage state and terminal");
        nr::mixed_stage_terminal.active = true;
        nr::CountNrDecline(nr::NrDeclineReason::kGameEvaluateFailed);
      }
      check(nr::mixed_stage_terminal.injected && nr::evaluate_chain.render_attempted
                && nr::evaluate_chain.configuration_generation == 73,
            "a failing child source restores the parent's plan and success");
    }
    {
      const nr::SourceStageScope next(reinterpret_cast<void*>(uintptr_t{1}), true);
      check(!nr::evaluate_chain.render_attempted && nr::evaluate_chain.game_wrappers == 1,
            "a sequential source call does not inherit an earlier Render admission");
      nr::mixed_stage_terminal = {.active = true, .injected = true};
    }
    check(nr::mixed_stage_source_successes.load() == multi_success_before + 2
              && declines(nr::NrDeclineReason::kGameEvaluateFailed) == multi_failed_before + 1
              && declines(nr::NrDeclineReason::kNestedMirror) == multi_mirror_before + 1,
          "distinct sources publish independent terminals; only a same-source wrapper is a mirror");
  }
  std::cout << configurations << " configurations, " << capability_cases << " capability cases, "
            << model_cases << " model assignments; " << checks << " checks, " << failures << " failures\n";
  return (failures == 0 ? 0 : 1);
}
