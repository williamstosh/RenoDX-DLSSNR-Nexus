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
#include <capability.hpp>

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

#include "stage_change_cases.hpp"

  {
    namespace cap = renodx::addons::dlss5::capability;
    const cap::EffectiveResolution scaled{.follow_input = false, .scale = .67f};
    const auto render = cap::ResolveWorkingSize(scaled,
        {.reference_width = 1715, .reference_height = 965, .guide_width = 1715, .guide_height = 965});
    const auto upscaled = cap::ResolveWorkingSize(scaled,
        {.reference_width = 2560, .reference_height = 1440, .guide_width = 1715, .guide_height = 965});
    check(render == cap::Size{1149, 647} && upscaled == cap::Size{1715, 965},
          "67 percent uses each Stage reference once: Render input versus Upscaled output");
    check(cap::ResolveWorkingSize({.follow_input = true},
              {.reference_width = 2560, .reference_height = 1440, .guide_width = 1715, .guide_height = 965})
              == cap::Size{1715, 965},
          "following Guide does not apply the configured percentage a second time");
  }

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

  context = "B1 default capability";
  check(!planner::Capabilities{}.mixed_present_dx12, "mixed Present DX12 execution requires explicit opt-in");
  struct B1Case {
    std::string_view name;
    planner::Configuration config;
    std::array<Stage, 4> requested;
  };
  const std::array b1_cases = {
      B1Case{.name = "Upscaled->Present",
             .config = {.pass_count = 2, .hook_point = 0, .stages = {3}},
             .requested = {Stage::UPSCALED, Stage::PRESENT}},
      B1Case{.name = "Render->Present",
             .config = {.pass_count = 2, .hook_point = 1, .stages = {3}},
             .requested = {Stage::RENDER, Stage::PRESENT}},
      B1Case{.name = "Render->Upscaled->Present",
             .config = {.pass_count = 3, .hook_point = 1, .stages = {2, 3}},
             .requested = {Stage::RENDER, Stage::UPSCALED, Stage::PRESENT}},
      B1Case{.name = "Upscaled->Upscaled->Present",
             .config = {.pass_count = 3, .hook_point = 0, .stages = {0, 3}},
             .requested = {Stage::UPSCALED, Stage::UPSCALED, Stage::PRESENT}},
      B1Case{.name = "Upscaled->Present->Present",
             .config = {.pass_count = 3, .hook_point = 0, .stages = {3, 0}},
             .requested = {Stage::UPSCALED, Stage::PRESENT, Stage::PRESENT}},
      B1Case{.name = "Upscaled->Present->Present->Present",
             .config = {.pass_count = 4, .hook_point = 0, .stages = {3, 0, 0}},
             .requested = {Stage::UPSCALED, Stage::PRESENT, Stage::PRESENT, Stage::PRESENT}},
  };
  for (const auto& test_case : b1_cases) {
    for (uint32_t flags = 0; flags < 64; ++flags) {
      context = "B1 " + std::string(test_case.name) + " caps=" + std::to_string(flags);
      const auto saved_config = test_case.config;
      const planner::Capabilities caps{
          .mixed_dx12 = (flags & 1) != 0,
          .mixed_dx11 = (flags & 2) != 0,
          .ray_reconstruction = (flags & 4) != 0,
          .present_available = (flags & 8) != 0,
          .fg_owns_input = (flags & 16) != 0,
          .mixed_present_dx12 = (flags & 32) != 0};
      const auto saved_caps = caps;
      const bool b1_route = test_case.config.pass_count == 2
                            && test_case.requested[0] == Stage::UPSCALED
                            && test_case.requested[1] == Stage::PRESENT;
      const bool supported = b1_route && caps.mixed_present_dx12 && caps.mixed_dx12
                             && caps.present_available && !caps.ray_reconstruction;
      const bool accepted = supported && !caps.fg_owns_input;
      const auto plan = planner::Build(test_case.config, caps);
      check(plan.valid == accepted, "B1 admits only the opted-in two-pass DX12 SR/DLAA route without FG");
      check(plan.pass_count == test_case.config.pass_count && plan.mixed
                && plan.requested == test_case.requested && plan.effective == test_case.requested,
            "B1 preserves requested stages and never moves the Present tail back to Upscaled");
      check(test_case.config.pass_count == saved_config.pass_count
                && test_case.config.hook_point == saved_config.hook_point && test_case.config.stages == saved_config.stages,
            "B1 admission and rejection preserve saved configuration");
      check(caps.mixed_dx12 == saved_caps.mixed_dx12 && caps.mixed_dx11 == saved_caps.mixed_dx11
                && caps.ray_reconstruction == saved_caps.ray_reconstruction
                && caps.present_available == saved_caps.present_available && caps.fg_owns_input == saved_caps.fg_owns_input
                && caps.mixed_present_dx12 == saved_caps.mixed_present_dx12,
            "B1 planning preserves all capability inputs");
      if (!accepted) {
        const auto reason = (!caps.mixed_dx12 && !caps.mixed_dx11 ? Reason::UNSUPPORTED_BACKEND
                             : (supported ? Reason::FG_OWNS_INPUT : Reason::MIXED_PRESENT));
        check(plan.reason == reason && plan.group_count == 0,
              "rejected B1 chains explain the failure and expose no partial executable groups");
        continue;
      }
      check(plan.reason == Reason::NONE && plan.group_count == 2,
            "accepted B1 retains two executable groups without fallback");
      check(plan.groups[0].stage == Stage::UPSCALED && plan.groups[0].begin == 0 && plan.groups[0].count == 1
                && plan.groups[1].stage == Stage::PRESENT && plan.groups[1].begin == 1 && plan.groups[1].count == 1,
            "accepted B1 executes each pass exactly once in Upscaled then Present order");
      check(plan.At(Stage::RENDER).count == 0
                && plan.At(Stage::UPSCALED).begin == 0 && plan.At(Stage::UPSCALED).count == 1
                && plan.At(Stage::PRESENT).begin == 1 && plan.At(Stage::PRESENT).count == 1,
            "B1 stage lookup preserves the distinct Present tail");
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
  namespace capability = renodx::addons::dlss5::capability;
  using capability::Status;
  using CapabilityReason = capability::Reason;
  capability::Input capability_input{
      .api = capability::Api::DX12,
      .backend = capability::Backend::NATIVE,
      .runtime = capability::Runtime::AVAILABLE,
      .fg = capability::Fg::OFF,
      .plan = planner::Build({.pass_count = 2}),
      .main_resolution = {.follow_input = false, .scale = 1.f},
      .resolutions = {{{.mode = 2, .scale = 0.67f}}},
      .geometry = {{{.reference_width = 1920, .reference_height = 1080, .guide_width = 1280, .guide_height = 720},
                    {.reference_width = 3840, .reference_height = 2160, .guide_width = 1920, .guide_height = 1080},
                    {.reference_width = 3840, .reference_height = 2160, .guide_width = 1920, .guide_height = 1080}}}};
  const auto base_input = capability_input;
  context = "capability group-count matrix";
  // Enumerate every ordered Render/Upscaled split, including four logical
  // passes arranged as 2+2. Each physical group, not total pass count, owns
  // the two-size limit. Repeat with both scale directions and FG On/Unknown.
  for (const bool bridge : {false, true}) {
    for (uint32_t count = 1; count <= 4; ++count) {
      for (uint32_t render_count = 0; render_count <= count; ++render_count) {
        for (uint32_t sizes = 0; sizes < (1u << count); ++sizes) {
          for (const auto fg : {capability::Fg::OFF, capability::Fg::ON, capability::Fg::UNKNOWN}) {
            capability_input = base_input;
            capability_input.api = (bridge ? capability::Api::DX11 : capability::Api::DX12);
            capability_input.backend = (bridge ? capability::Backend::DX11_BRIDGE : capability::Backend::NATIVE);
            capability_input.fg = fg;
            planner::Configuration config{.pass_count = count, .hook_point = (render_count == 0 ? 0u : 1u)};
            if (render_count > 0 && render_count < count) config.stages[render_count - 1] = 2;
            capability_input.plan = planner::Build(config, {.mixed_dx12 = !bridge, .mixed_dx11 = bridge});
            capability_input.main_resolution.scale = ((sizes & 1) != 0 ? 0.67f : 1.f);
            for (uint32_t pass = 1; pass < count; ++pass) {
              capability_input.resolutions[pass - 1] = {.mode = 2, .scale = ((sizes & (1u << pass)) != 0 ? 0.67f : 1.f)};
            }
            bool expected = true;
            uint32_t failed_group = capability::kNoIndex;
            for (uint32_t index = 0; index < capability_input.plan.group_count; ++index) {
              const auto group = capability_input.plan.groups[index];
              if (group.count <= 2) continue;
              for (uint32_t pass = group.begin + 1; pass < group.begin + group.count; ++pass) {
                if (((sizes >> pass) & 1u) != ((sizes >> group.begin) & 1u)) {
                  expected = false;
                  failed_group = index;
                }
              }
            }
            const auto queried = capability::Query(capability_input);
            check(queried.CanSubmit() == expected, "native size eligibility follows each effective group count");
            check(!queried.execution_proven, "eligible configurations do not imply execution proof");
            for (uint32_t index = 0; index < queried.group_count; ++index) {
              check(queried.groups[index].independent_sizes_allowed == (capability_input.plan.groups[index].count == 2),
                    "executor pair selection can reuse the queried group policy directly");
            }
            if (!expected) {
              check(queried.configuration.reason == CapabilityReason::GROUP_SIZE_MISMATCH
                        && queried.configuration.group == failed_group
                        && queried.configuration.pass < count,
                    "group-size rejection identifies the failing logical pass and group");
            }
          }
        }
      }
    }
  }
  context = "Present pair FG declaration and ownership";
  for (const bool bridge : {false, true}) {
    for (const auto fg : {capability::Fg::OFF, capability::Fg::ON, capability::Fg::UNKNOWN}) {
      for (uint32_t owners = 0; owners < 8; ++owners) {
        capability_input = base_input;
        capability_input.api = (bridge ? capability::Api::DX11 : capability::Api::DX12);
        capability_input.backend = (bridge ? capability::Backend::DX11_BRIDGE : capability::Backend::NATIVE);
        capability_input.plan = planner::Build({.pass_count = 2, .hook_point = 2});
        capability_input.fg = fg;
        capability_input.fg_owns_input = (owners & 1) != 0;
        capability_input.xefg_enabled = (owners & 2) != 0;
        capability_input.auto_fg_fallback = (owners & 4) != 0;
        const auto queried = capability::Query(capability_input);
        check(queried.CanSubmit() == (fg == capability::Fg::OFF && owners == 0),
              "Present different sizes require explicit Off and no active competing route");
        if (owners == 0 && fg == capability::Fg::UNKNOWN) {
          check(queried.configuration.status == Status::NEEDS_PROOF
                    && queried.configuration.reason == CapabilityReason::FG_OFF_REQUIRED,
                "unknown FG is never silently treated as Off");
        }
        capability_input.resolutions[0].mode = 0;
        const auto same_size = capability::Query(capability_input);
        check(same_size.CanSubmit(), "legacy same-size Present is not given a new FG Off requirement");
        check(same_size.groups[0].independent_sizes_allowed == (fg == capability::Fg::OFF && owners == 0),
              "same-size eligibility does not grant the independent Present executor branch");
      }
    }
  }
  context = "B1 per-frame proof";
  capability_input = base_input;
  capability_input.plan = planner::Build({.pass_count = 2, .stages = {3}}, {.mixed_present_dx12 = true});
  auto queried = capability::Query(capability_input);
  check(!queried.CanSubmit() && queried.configuration.status == Status::NEEDS_PROOF
            && queried.configuration.reason == CapabilityReason::B1_FRAME_PROOF_REQUIRED
            && queried.configuration.pass == 1 && queried.configuration.group == 1,
        "Build success alone never grants B1 frame admission");
  capability_input.b1_frame_proven = true;
  check(capability::Query(capability_input).CanSubmit() && !capability::Query(capability_input).execution_proven,
        "B1 source proof grants eligibility, not successful NR execution");
  capability_input.current_execution_proven = true;
  check(capability::Query(capability_input).execution_proven, "current execution proof is reported separately");
  capability_input.fg = capability::Fg::ON;
  check(!capability::Query(capability_input).CanSubmit() && !capability::Query(capability_input).execution_proven,
        "dynamic FG change overrides historical B1 and execution proofs");
  capability_input = base_input;
  capability_input.plan = planner::Build({.pass_count = 2, .hook_point = 2});
  capability_input.native_prefg_resolution_experiment = true;
  capability_input.current_execution_proven = true;
  queried = capability::Query(capability_input);
  check(queried.configuration.status == Status::EXPERIMENTAL && !queried.CanSubmit() && !queried.execution_proven
            && queried.configuration.reason == CapabilityReason::NATIVE_PREFG_BLOCKED,
        "retained native Pre-FG prototype never becomes supported even with claimed execution evidence");

  context = "capability unknown environment and legacy value retention";
  for (const auto api : {capability::Api::UNKNOWN, capability::Api::DX11, capability::Api::DX12,
                         capability::Api::VULKAN, capability::Api::OTHER}) {
    for (const auto backend : {capability::Backend::UNKNOWN, capability::Backend::NATIVE,
                               capability::Backend::DX11_BRIDGE, capability::Backend::STREAMLINE_FALLBACK}) {
      for (const auto runtime : {capability::Runtime::UNKNOWN, capability::Runtime::AVAILABLE, capability::Runtime::UNAVAILABLE}) {
        capability_input = base_input;
        capability_input.api = api;
        capability_input.backend = backend;
        capability_input.runtime = runtime;
        const auto saved = capability_input;
        queried = capability::Query(capability_input);
        const bool supported = runtime == capability::Runtime::AVAILABLE
            && ((api == capability::Api::DX12 && backend == capability::Backend::NATIVE)
                || (api == capability::Api::DX11 && backend == capability::Backend::DX11_BRIDGE));
        check(queried.CanSubmit() == supported, "unknown/unavailable/mismatched backend cannot admit new independent modes");
        check(capability_input.api == saved.api && capability_input.backend == saved.backend
                  && capability_input.runtime == saved.runtime && capability_input.plan.requested == saved.plan.requested
                  && capability_input.resolutions[0].mode == saved.resolutions[0].mode
                  && capability_input.resolutions[0].scale == saved.resolutions[0].scale,
              "capability query leaves saved stage, mode and scale choices intact in every environment");
      }
    }
  }
  capability_input = base_input;
  capability_input.api = capability::Api::VULKAN;
  capability_input.resolutions[0].mode = 0;
  check(capability::Query(capability_input).CanSubmit(), "Vulkan legacy inherited single-stage path is preserved");
  capability_input.main_resolution.scale = .67f;
  check(!capability::Query(capability_input).CanSubmit(), "Vulkan cannot advertise unimplemented base working-size scaling");
  capability_input.main_resolution.scale = 1.f;
  capability_input.plan = planner::Build({.hook_point = 1});
  check(!capability::Query(capability_input).CanSubmit(), "Vulkan cannot advertise an unimplemented Render insertion point");
  capability_input.plan = planner::Build({.pass_count = 2, .hook_point = 1, .stages = {2}});
  check(!capability::Query(capability_input).CanSubmit(), "Vulkan mixed stages remain blocked regardless of a DX12-built plan");
  capability_input = base_input;
  capability_input.plan = planner::Build({.pass_count = 2, .hook_point = 1, .stages = {2}}, {.ray_reconstruction = true});
  queried = capability::Query(capability_input);
  check(queried.CanSubmit() && queried.plan_reason == Reason::RR_AFTER
            && capability_input.plan.At(Stage::RENDER).count == 0,
        "RR eligibility exposes the existing Upscaled fallback without claiming real PreSR support");
  capability_input.plan = planner::Build({.pass_count = 2, .stages = {3}});
  queried = capability::Query(capability_input);
  check(!queried.CanSubmit() && queried.plan_reason == Reason::MIXED_PRESENT,
        "query cannot revive a planner-rejected route");

  context = "resolved dimensions, inheritance and dynamic changes";
  capability_input = base_input;
  capability_input.plan = planner::Build({.pass_count = 3});
  capability_input.main_resolution = {.follow_input = true, .scale = 1.f};
  capability_input.resolutions[0] = {.mode = 2, .scale = 0.5f};
  queried = capability::Query(capability_input);
  check(queried.CanSubmit() && queried.working_sizes[0] == queried.working_sizes[1],
        "different controls resolving to equal actual size remain legal in a three-pass group");
  capability_input.geometry[1].guide_width = 1280;
  queried = capability::Query(capability_input);
  check(!queried.CanSubmit() && queried.configuration.reason == CapabilityReason::GROUP_SIZE_MISMATCH,
        "dynamic Guide changes recompute actual size and reject newly incompatible groups");
  capability_input.geometry[1] = {};
  queried = capability::Query(capability_input);
  check(!queried.CanSubmit() && queried.configuration.status == Status::NEEDS_PROOF
            && queried.configuration.reason == CapabilityReason::DIMENSIONS_UNKNOWN,
        "missing geometry cannot prove three-pass size equivalence");
  capability_input.geometry[1] = {.reference_width = 3840, .reference_height = 2160};
  queried = capability::Query(capability_input);
  check(!queried.CanSubmit() && queried.configuration.status == Status::NEEDS_PROOF
            && queried.configuration.reason == CapabilityReason::DIMENSIONS_UNKNOWN,
        "a reference rectangle without current Guide evidence cannot prove a Follow-input mismatch");
  capability_input.resolutions[0].mode = 0;
  check(capability::Query(capability_input).CanSubmit(), "identical settings establish same-size eligibility without observed dimensions");
  const std::array<capability::Resolution, 3> inheritance = {{{.mode = 2, .scale = 0.67f}, {.mode = 0}, {.mode = 1}}};
  check(capability::ResolveResolution(2, {.follow_input = false, .scale = 0.8f}, inheritance).scale == 0.8f,
        "inheritance uses Pass 1 rather than the preceding independent pass");
  check(capability::ResolveWorkingSize({.follow_input = true}, {.reference_width = 100, .reference_height = 50,
          .guide_width = 200, .guide_height = 100}) == capability::Size{100, 50}, "Guide dimensions clamp to stage reference");
  check(capability::ResolveWorkingSize({.follow_input = true, .scale = 0.67f}, {.reference_width = 101, .reference_height = 51})
            == capability::Size{68, 34}, "missing Guide uses the executor's rounded scale fallback");
  capability_input = base_input;
  capability_input.resolutions[0].scale = std::numeric_limits<float>::quiet_NaN();
  check(capability::Query(capability_input).configuration.reason == CapabilityReason::INVALID_RESOLUTION,
        "non-finite active scales cannot reach integer dimension conversion");
  capability_input.resolutions[0] = {.mode = 3};
  check(capability::Query(capability_input).configuration.reason == CapabilityReason::INVALID_RESOLUTION,
        "invalid active mode is rejected without sanitizing the saved value");
  capability_input.plan = planner::Build({.pass_count = 1});
  check(capability::Query(capability_input).CanSubmit(), "invalid inactive future pass does not block the active chain");
  capability_input = base_input;
  capability_input.plan.groups[0].count = 5;
  check(capability::Query(capability_input).configuration.reason == CapabilityReason::INVALID_PLAN,
        "malformed plan cannot index past resolution inputs");
  for (uint32_t reason = 0; reason <= static_cast<uint32_t>(CapabilityReason::NATIVE_PREFG_BLOCKED); ++reason) {
    check(std::string_view(capability::ReasonText(static_cast<CapabilityReason>(reason))) != "Unknown capability reason",
          "every capability reason has a diagnostic explanation");
  }
  context = "Present three/four-pass groups";
  for (uint32_t count : {3u, 4u}) {
    capability_input = base_input;
    capability_input.plan = planner::Build({.pass_count = count, .hook_point = 2});
    queried = capability::Query(capability_input);
    check(!queried.CanSubmit() && queried.configuration.reason == CapabilityReason::GROUP_SIZE_MISMATCH,
          "three/four-pass Present rejects different sizes even with FG explicitly Off");
    capability_input.resolutions[0].mode = 0;
    check(capability::Query(capability_input).CanSubmit(), "legacy same-size three/four-pass Present remains eligible");
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
      nr::evaluate_chain.MarkInjected(reinterpret_cast<void*>(uintptr_t{3}), 1);
      {
        const nr::SourceStageScope mirror(reinterpret_cast<void*>(uintptr_t{1}), true);
        check(nr::evaluate_chain.game_wrappers == 2 && nr::evaluate_chain.render_attempted,
              "same-source nested wrappers share one stage state");
        check(nr::evaluate_chain.Injected(reinterpret_cast<void*>(uintptr_t{3}), 1),
              "same-source nested wrappers retain their output dedupe mark");
      }
      {
        const nr::SourceStageScope different(reinterpret_cast<void*>(uintptr_t{2}), true);
        check(nr::evaluate_chain.game_wrappers == 1 && !nr::evaluate_chain.render_attempted
                  && !nr::mixed_stage_terminal.active,
              "another nested source starts from independent stage state and terminal");
        check(!nr::evaluate_chain.Injected(reinterpret_cast<void*>(uintptr_t{3}), 1),
              "another source may evaluate the same output without inheriting a mirror mark");
        nr::evaluate_chain.MarkInjected(reinterpret_cast<void*>(uintptr_t{4}), 0);
        nr::mixed_stage_terminal.active = true;
        nr::CountNrDecline(nr::NrDeclineReason::kGameEvaluateFailed);
      }
      check(nr::mixed_stage_terminal.injected && nr::evaluate_chain.render_attempted
                && nr::evaluate_chain.configuration_generation == 73,
            "a failing child source restores the parent's plan and success");
      check(nr::evaluate_chain.Injected(reinterpret_cast<void*>(uintptr_t{3}), 1)
                && !nr::evaluate_chain.Injected(reinterpret_cast<void*>(uintptr_t{4}), 0),
            "a child source restores the parent's output marks without leaking its own");
    }
    {
      const nr::SourceStageScope next(reinterpret_cast<void*>(uintptr_t{1}), true);
      check(!nr::evaluate_chain.render_attempted && nr::evaluate_chain.game_wrappers == 1,
            "a sequential source call does not inherit an earlier Render admission");
      check(!nr::evaluate_chain.Injected(reinterpret_cast<void*>(uintptr_t{3}), 1),
            "a later real evaluate of the same source starts with clean output marks");
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
