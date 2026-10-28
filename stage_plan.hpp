/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <cstdint>

namespace renodx::addons::dlss5::stage_plan {

inline constexpr uint32_t kMaxPasses = 4;
enum class Stage : uint8_t { RENDER, UPSCALED, PRESENT };
enum class Reason : uint8_t {
  NONE, BACKWARD_STAGE, UNSUPPORTED_BACKEND, MIXED_PRESENT,
  FG_OWNS_INPUT, RR_AFTER, PRESENT_AFTER, INVALID_STAGE,
};

struct Configuration {
  uint32_t pass_count = 1;
  uint32_t hook_point = 0;  // Existing NRHookPoint: Upscaled, Render, Present.
  std::array<uint32_t, kMaxPasses - 1> stages{};
};
struct Capabilities {
  bool mixed_dx12 = true;
  bool mixed_dx11 = false;
  bool ray_reconstruction = false;
  bool present_available = true;
  bool fg_owns_input = false;
  bool mixed_present_dx12 = false;
};
struct Group {
  Stage stage = Stage::UPSCALED;
  uint32_t begin = 0;
  uint32_t count = 0;
};
struct Plan {
  uint32_t pass_count = 1;
  std::array<Stage, kMaxPasses> requested{};
  std::array<Stage, kMaxPasses> effective{};
  std::array<Group, 3> groups{};
  uint32_t group_count = 0;
  bool valid = true;
  Reason reason = Reason::NONE;
  bool mixed = false;

  Group At(Stage stage) const {
    for (uint32_t i = 0; i < group_count; ++i) {
      if (groups[i].stage == stage) return groups[i];
    }
    return {.stage = stage};
  }
};

inline constexpr const char* Name(Stage stage) {
  switch (stage) {
    case Stage::RENDER: return "Render";
    case Stage::UPSCALED: return "Upscaled";
    case Stage::PRESENT: return "Present";
  }
  return "Unknown";
}
inline constexpr const char* ReasonText(Reason reason) {
  switch (reason) {
    case Reason::NONE: return "Requested stages available";
    case Reason::BACKWARD_STAGE: return "A later pass cannot run at an earlier stage";
    case Reason::UNSUPPORTED_BACKEND: return "Cross-stage passes require native DX11/DX12 DLSS SR/DLAA";
    case Reason::MIXED_PRESENT: return "Cross-stage Present execution is not validated";
    case Reason::FG_OWNS_INPUT: return "Frame generation owns the NR input path";
    case Reason::RR_AFTER: return "Ray Reconstruction runs Render passes after upscaling";
    case Reason::PRESENT_AFTER: return "Present is unavailable; using the existing Upscaled path";
    case Reason::INVALID_STAGE: return "Invalid stage selection";
  }
  return "Unknown";
}

// No resource ownership or model policy here. Effective groups preserve the
// logical pass order; an unproven mixed path is rejected as a whole.
inline Plan Build(const Configuration& config, const Capabilities& caps = {}) {
  Plan plan;
  plan.pass_count = (config.pass_count < 1 ? 1 : (config.pass_count > kMaxPasses ? kMaxPasses : config.pass_count));
  plan.requested[0] = (config.hook_point == 1 ? Stage::RENDER
                        : (config.hook_point == 2 ? Stage::PRESENT : Stage::UPSCALED));
  for (uint32_t pass = 1; pass < plan.pass_count; ++pass) {
    const uint32_t choice = config.stages[pass - 1];
    plan.requested[pass] = (choice == 0 ? plan.requested[pass - 1]
                            : (choice == 1 ? Stage::RENDER
                               : (choice == 2 ? Stage::UPSCALED : Stage::PRESENT)));
    plan.mixed |= plan.requested[pass] != plan.requested[pass - 1];
    if (choice > 3) {
      plan.valid = false;
      plan.reason = Reason::INVALID_STAGE;
    } else if (plan.requested[pass] < plan.requested[pass - 1]) {
      plan.valid = false;
      plan.reason = Reason::BACKWARD_STAGE;
    }
  }
  plan.effective = plan.requested;
  if (!plan.valid) return plan;
  if (plan.mixed) {
    if (!caps.mixed_dx12 && !caps.mixed_dx11) {
      plan.reason = Reason::UNSUPPORTED_BACKEND;
    } else if (plan.requested[plan.pass_count - 1] == Stage::PRESENT) {
      // B1 is an opt-in two-pass SR/DLAA route. Per-frame source and queue
      // proofs belong to the executor, not to this configuration planner.
      const bool b1 = caps.mixed_present_dx12 && caps.mixed_dx12
          && caps.present_available && !caps.ray_reconstruction
          && plan.pass_count == 2 && plan.requested[0] == Stage::UPSCALED;
      if (b1) {
        if (caps.fg_owns_input) {
          plan.reason = Reason::FG_OWNS_INPUT;
        }
      } else {
        plan.reason = Reason::MIXED_PRESENT;
      }
    } else if (caps.fg_owns_input) {
      plan.reason = Reason::FG_OWNS_INPUT;
    }
    if (plan.reason != Reason::NONE) {
      plan.valid = false;
      return plan;
    }
  }
  for (uint32_t pass = 0; pass < plan.pass_count; ++pass) {
    if (caps.ray_reconstruction && plan.effective[pass] == Stage::RENDER) {
      plan.effective[pass] = Stage::UPSCALED;
      plan.reason = Reason::RR_AFTER;
    } else if (!caps.present_available && plan.effective[pass] == Stage::PRESENT) {
      plan.effective[pass] = Stage::UPSCALED;
      plan.reason = Reason::PRESENT_AFTER;
    }
    if (plan.group_count == 0 || plan.groups[plan.group_count - 1].stage != plan.effective[pass]) {
      plan.groups[plan.group_count++] = {.stage = plan.effective[pass], .begin = pass};
    }
    ++plan.groups[plan.group_count - 1].count;
  }
  return plan;
}

}  // namespace renodx::addons::dlss5::stage_plan
