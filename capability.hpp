/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "stage_plan.hpp"

namespace renodx::addons::dlss5::capability {

enum class Status : uint8_t { SUPPORTED, UNSUPPORTED, NEEDS_PROOF, EXPERIMENTAL, UNAVAILABLE };
enum class Api : uint8_t { UNKNOWN, DX11, DX12, VULKAN, OTHER };
enum class Backend : uint8_t { UNKNOWN, NATIVE, DX11_BRIDGE, STREAMLINE_FALLBACK };
enum class Runtime : uint8_t { UNKNOWN, AVAILABLE, UNAVAILABLE };
enum class Fg : uint8_t { UNKNOWN, OFF, ON };
enum class Reason : uint8_t {
  NONE, ENVIRONMENT_UNKNOWN, RUNTIME_UNAVAILABLE, BACKEND_UNSUPPORTED, PLAN_REJECTED,
  INVALID_PLAN, INVALID_RESOLUTION, INDEPENDENT_MODE_UNSUPPORTED, GROUP_SIZE_MISMATCH,
  DIMENSIONS_UNKNOWN, FG_OFF_REQUIRED, FG_OWNS_INPUT, B1_FRAME_PROOF_REQUIRED,
  NATIVE_PREFG_BLOCKED, MODEL_UNCONFIRMED,
};

struct Resolution {
  uint32_t mode = 0;  // Pass 2-4: 0 inherits Pass 1, 1 follows Guide, 2 uses scale.
  float scale = 1.f;
};
struct EffectiveResolution {
  bool follow_input = true;
  float scale = 1.f;
};
struct Geometry {
  uint32_t reference_width = 0;
  uint32_t reference_height = 0;
  uint32_t guide_width = 0;
  uint32_t guide_height = 0;
};
struct Size {
  uint32_t width = 0;
  uint32_t height = 0;
  bool operator==(const Size&) const = default;
};

// The executor can use these two functions with its frozen EvaluateChain
// snapshot. Inheritance is from Pass 1, never the preceding pass or group.
inline EffectiveResolution ResolveResolution(
    uint32_t pass, EffectiveResolution main, const std::array<Resolution, stage_plan::kMaxPasses - 1>& passes) {
  if (pass == 0 || passes[pass - 1].mode == 0) return main;
  return {.follow_input = passes[pass - 1].mode == 1, .scale = passes[pass - 1].scale};
}

inline Size ResolveWorkingSize(EffectiveResolution settings, const Geometry& geometry) {
  if (geometry.reference_width == 0 || geometry.reference_height == 0) return {};
  if (settings.follow_input && geometry.guide_width != 0 && geometry.guide_height != 0) {
    return {.width = std::min(geometry.guide_width, geometry.reference_width),
            .height = std::min(geometry.guide_height, geometry.reference_height)};
  }
  if (!std::isfinite(settings.scale)) return {};
  const float scale = std::clamp(settings.scale, 0.33f, 1.f);
  return {.width = std::max(1u, static_cast<uint32_t>(std::llround(geometry.reference_width * double{scale}))),
          .height = std::max(1u, static_cast<uint32_t>(std::llround(geometry.reference_height * double{scale})))};
}

struct Input {
  Api api = Api::UNKNOWN;
  Backend backend = Backend::UNKNOWN;
  Runtime runtime = Runtime::UNKNOWN;
  Fg fg = Fg::UNKNOWN;
  bool fg_owns_input = false;
  bool xefg_enabled = false;
  bool auto_fg_fallback = false;
  bool native_prefg_resolution_experiment = false;
  stage_plan::Plan plan;
  EffectiveResolution main_resolution;
  std::array<Resolution, stage_plan::kMaxPasses - 1> resolutions{};
  // Indexed by effective Stage, not logical pass. Zero reference means unknown.
  std::array<Geometry, 3> geometry{};
  // Only current-frame, same-queue/source/configuration proofs from the B1
  // executor qualify. Historical counters and a successful Build do not.
  bool b1_frame_proven = false;
  // Caller must invalidate this when any configuration/environment/source
  // generation changes. This does not authorize execution or prove visual quality.
  bool current_execution_proven = false;
};
inline constexpr uint32_t kNoIndex = stage_plan::kMaxPasses;
struct Decision {
  Status status = Status::SUPPORTED;
  Reason reason = Reason::NONE;
  uint32_t pass = kNoIndex;  // Zero-based logical pass; kNoIndex means whole configuration.
  uint32_t group = kNoIndex;
  bool independent_sizes_allowed = false;
  bool CanSubmit() const { return status == Status::SUPPORTED; }
};
struct Result {
  Decision configuration;
  std::array<Decision, 3> groups{};
  std::array<Size, stage_plan::kMaxPasses> working_sizes{};
  uint32_t group_count = 0;
  stage_plan::Reason plan_reason = stage_plan::Reason::NONE;
  bool execution_proven = false;
  bool CanSubmit() const { return configuration.CanSubmit(); }
};

inline bool SupportsIndependentSizes(stage_plan::Stage stage, uint32_t group_count, bool present_route_allowed) {
  return group_count == 2 && (stage != stage_plan::Stage::PRESENT || present_route_allowed);
}

inline constexpr const char* ReasonText(Reason reason) {
  switch (reason) {
    case Reason::NONE: return "Configuration is eligible; execution is a separate proof";
    case Reason::ENVIRONMENT_UNKNOWN: return "API, backend or runtime has not been identified";
    case Reason::RUNTIME_UNAVAILABLE: return "NR runtime is unavailable";
    case Reason::BACKEND_UNSUPPORTED: return "This API/backend route is not supported by this capability contract";
    case Reason::PLAN_REJECTED: return "Stage planner rejected the requested chain; inspect plan_reason";
    case Reason::INVALID_PLAN: return "Plan does not describe contiguous effective stage groups";
    case Reason::INVALID_RESOLUTION: return "Active resolution mode or scale is invalid";
    case Reason::INDEPENDENT_MODE_UNSUPPORTED: return "Vulkan retains inherited resolution only";
    case Reason::GROUP_SIZE_MISMATCH: return "This stage group cannot execute different working sizes";
    case Reason::DIMENSIONS_UNKNOWN: return "Current stage dimensions are required to compare working sizes";
    case Reason::FG_OFF_REQUIRED: return "Ordinary Present independent sizes require an explicit successful FG Off declaration";
    case Reason::FG_OWNS_INPUT: return "FG ownership, XeFG or automatic fallback excludes this route";
    case Reason::B1_FRAME_PROOF_REQUIRED: return "B1 requires current-frame source, submission and queue proof";
    case Reason::NATIVE_PREFG_BLOCKED: return "Native Pre-FG independent sizes remain experimental and BLOCKED / NO-GO";
    case Reason::MODEL_UNCONFIRMED: return "Model B requires an identified compatible NR runtime";
  }
  return "Unknown capability reason";
}

// Shared policy boundary for ComputeNrGroupWorkingResolution and the dual-size
// execution branches. It grants configuration eligibility only, never resource,
// queue, producer identity, feature availability or successful GPU execution.
namespace internal {

// Query validates the plan and environment before entering this boundary.
inline Decision QueryGroup(const Input& input, uint32_t group_index) {
  const auto& group = input.plan.groups[group_index];
  const auto& geometry = input.geometry[static_cast<uint32_t>(group.stage)];
  const auto first = ResolveResolution(group.begin, input.main_resolution, input.resolutions);
  const auto known_size = [&](EffectiveResolution settings) {
    // A cached reference rectangle does not establish the current Guide size.
    // The executor may use a saved-scale fallback, but a UI query must not
    // misclassify an unobserved Follow-input combination as a proven mismatch.
    if (settings.follow_input && (geometry.guide_width == 0 || geometry.guide_height == 0)) return Size{};
    return ResolveWorkingSize(settings, geometry);
  };
  const auto first_size = known_size(first);
  bool differs = false;
  bool unknown = false;
  uint32_t different_pass = group.begin;
  for (uint32_t pass = group.begin + 1; pass < group.begin + group.count; ++pass) {
    const auto settings = ResolveResolution(pass, input.main_resolution, input.resolutions);
    const auto size = known_size(settings);
    const bool same_settings = settings.follow_input == first.follow_input && settings.scale == first.scale;
    if (size.width != 0 && first_size.width != 0) {
      if (size == first_size) continue;
      differs = true;
    } else if (same_settings) {
      continue;
    } else {
      unknown = true;
    }
    different_pass = pass;
  }
  Decision result{.pass = different_pass, .group = group_index};
  const bool native_pair = group.count == 2 && input.api != Api::VULKAN;
  result.independent_sizes_allowed = input.api != Api::VULKAN && SupportsIndependentSizes(group.stage, group.count,
      !input.plan.mixed && !input.native_prefg_resolution_experiment
          && input.fg == Fg::OFF && !input.fg_owns_input && !input.xefg_enabled && !input.auto_fg_fallback);
  if (!differs && !unknown) return result;
  if (result.independent_sizes_allowed) return result;
  if (native_pair && !input.plan.mixed) {
    if (input.native_prefg_resolution_experiment) {
      result.status = Status::EXPERIMENTAL;
      result.reason = Reason::NATIVE_PREFG_BLOCKED;
    } else if (input.fg_owns_input || input.xefg_enabled || input.auto_fg_fallback) {
      result.status = Status::UNSUPPORTED;
      result.reason = Reason::FG_OWNS_INPUT;
    } else if (input.fg != Fg::OFF) {
      result.status = (input.fg == Fg::UNKNOWN ? Status::NEEDS_PROOF : Status::UNSUPPORTED);
      result.reason = Reason::FG_OFF_REQUIRED;
    }
    return result;
  }
  result.status = (differs ? Status::UNSUPPORTED : Status::NEEDS_PROOF);
  result.reason = (differs ? Reason::GROUP_SIZE_MISMATCH : Reason::DIMENSIONS_UNKNOWN);
  return result;
}

}  // namespace internal

inline Result Query(const Input& input) {
  Result result{.plan_reason = input.plan.reason};
  const auto reject = [&](Status status, Reason reason, uint32_t pass = kNoIndex, uint32_t group = kNoIndex) {
    result.configuration = {.status = status, .reason = reason, .pass = pass, .group = group};
    return result;
  };
  if (!input.plan.valid) return reject(Status::UNSUPPORTED, Reason::PLAN_REJECTED);
  if (input.api == Api::UNKNOWN || input.backend == Backend::UNKNOWN || input.runtime == Runtime::UNKNOWN) {
    return reject(Status::UNAVAILABLE, Reason::ENVIRONMENT_UNKNOWN);
  }
  if (input.runtime == Runtime::UNAVAILABLE) return reject(Status::UNAVAILABLE, Reason::RUNTIME_UNAVAILABLE);
  const bool native_dx12 = input.api == Api::DX12 && input.backend == Backend::NATIVE;
  const bool bridge_dx11 = input.api == Api::DX11 && input.backend == Backend::DX11_BRIDGE;
  const bool native_vk = input.api == Api::VULKAN && input.backend == Backend::NATIVE;
  if (!native_dx12 && !bridge_dx11 && !native_vk) return reject(Status::UNSUPPORTED, Reason::BACKEND_UNSUPPORTED);
  const auto& plan = input.plan;
  if (plan.pass_count == 0 || plan.pass_count > stage_plan::kMaxPasses
      || plan.group_count == 0 || plan.group_count > plan.groups.size()) {
    return reject(Status::UNSUPPORTED, Reason::INVALID_PLAN);
  }
  uint32_t covered = 0;
  for (uint32_t index = 0; index < plan.group_count; ++index) {
    const auto& group = plan.groups[index];
    if (group.begin != covered || group.count == 0 || group.count > plan.pass_count - covered
        || static_cast<uint32_t>(group.stage) > static_cast<uint32_t>(stage_plan::Stage::PRESENT)
        || (index != 0 && group.stage <= plan.groups[index - 1].stage)) {
      return reject(Status::UNSUPPORTED, Reason::INVALID_PLAN, covered, index);
    }
    for (uint32_t pass = group.begin; pass < group.begin + group.count; ++pass) {
      if (plan.effective[pass] != group.stage) return reject(Status::UNSUPPORTED, Reason::INVALID_PLAN, pass, index);
    }
    covered += group.count;
  }
  if (covered != plan.pass_count) return reject(Status::UNSUPPORTED, Reason::INVALID_PLAN);
  const bool mixed = plan.group_count > 1;
  if (mixed != plan.mixed) {
    // RR may legitimately collapse a requested mixed chain into one group.
    if (!plan.mixed || plan.reason != stage_plan::Reason::RR_AFTER) return reject(Status::UNSUPPORTED, Reason::INVALID_PLAN);
  }
  if (plan.mixed && native_vk) return reject(Status::UNSUPPORTED, Reason::BACKEND_UNSUPPORTED);
  if (native_vk && plan.requested[0] != stage_plan::Stage::UPSCALED) {
    return reject(Status::UNSUPPORTED, Reason::BACKEND_UNSUPPORTED);
  }
  if (native_vk && (input.main_resolution.follow_input || std::abs(input.main_resolution.scale - 1.f) > 0.00001f)) {
    return reject(Status::UNSUPPORTED, Reason::INDEPENDENT_MODE_UNSUPPORTED);
  }
  if (plan.mixed && input.fg_owns_input) return reject(Status::UNSUPPORTED, Reason::FG_OWNS_INPUT);
  const auto present = plan.At(stage_plan::Stage::PRESENT);
  const bool b1 = mixed && present.count != 0;
  if (b1 && (!native_dx12 || plan.pass_count != 2 || plan.groups[0].stage != stage_plan::Stage::UPSCALED)) {
    return reject(Status::UNSUPPORTED, Reason::BACKEND_UNSUPPORTED);
  }
  for (uint32_t pass = 0; pass < plan.pass_count; ++pass) {
    if (pass != 0 && input.resolutions[pass - 1].mode > 2) return reject(Status::UNSUPPORTED, Reason::INVALID_RESOLUTION, pass);
    const auto settings = ResolveResolution(pass, input.main_resolution, input.resolutions);
    if (!std::isfinite(settings.scale)) return reject(Status::UNSUPPORTED, Reason::INVALID_RESOLUTION, pass);
    if (native_vk && pass != 0 && input.resolutions[pass - 1].mode != 0) {
      return reject(Status::UNSUPPORTED, Reason::INDEPENDENT_MODE_UNSUPPORTED, pass);
    }
    result.working_sizes[pass] = ResolveWorkingSize(settings, input.geometry[static_cast<uint32_t>(plan.effective[pass])]);
  }
  for (uint32_t index = 0; index < plan.group_count; ++index) {
    result.groups[index] = internal::QueryGroup(input, index);
    if (!result.groups[index].CanSubmit() && result.configuration.CanSubmit()) result.configuration = result.groups[index];
  }
  result.group_count = plan.group_count;
  if (b1) {
    Decision decision{.pass = present.begin, .group = plan.group_count - 1};
    if (input.fg_owns_input || input.xefg_enabled || input.auto_fg_fallback || input.fg == Fg::ON) {
      decision.status = Status::UNSUPPORTED;
      decision.reason = Reason::FG_OWNS_INPUT;
    } else if (!input.b1_frame_proven) {
      decision.status = Status::NEEDS_PROOF;
      decision.reason = Reason::B1_FRAME_PROOF_REQUIRED;
    }
    result.groups[decision.group] = decision;
    if (!decision.CanSubmit()) result.configuration = decision;
  }
  result.execution_proven = result.CanSubmit() && input.current_execution_proven;
  return result;
}

}  // namespace renodx::addons::dlss5::capability
