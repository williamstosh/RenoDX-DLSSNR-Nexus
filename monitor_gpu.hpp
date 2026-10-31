/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "gpu_timers.hpp"
#include "stage_plan.hpp"

namespace renodx::addons::dlss5 {

struct MonitorGpuTotal {
  bool valid = false;
  bool reference = false;
  bool incompatible = false;
  uint32_t missing_stages = 0;
  float milliseconds = 0.f;
};

// The caller validates recording/completion age, live configuration and source
// epochs. This layer verifies complete group coverage and comparable contracts.
inline MonitorGpuTotal SummarizeMonitorGpu(const stage_plan::Plan& plan,
    const std::array<gpu_timers::Stats, 3>& samples, const std::array<bool, 3>& current,
    const std::array<uint32_t, 4>& resolved_models) {
  MonitorGpuTotal result;
  if (!plan.valid || plan.group_count == 0 || plan.group_count > 3) return result;
  const gpu_timers::Stats* first = nullptr;
  uint64_t total_us = 0;
  uint32_t covered = 0;
  for (uint32_t group_index = 0; group_index < plan.group_count; ++group_index) {
    const auto& group = plan.groups[group_index];
    const size_t stage = static_cast<size_t>(group.stage);
    if (stage >= samples.size() || group.count == 0 || group.begin + group.count > plan.pass_count) return result;
    const auto& sample = samples[stage];
    bool complete = current[stage] && sample.valid && sample.state == gpu_timers::SampleState::VALID
        && gpu_timers::StageIndex(sample.path) == stage;
    for (uint32_t local = 0; local < gpu_timers::kTimerMaxPasses; ++local) {
      const auto& pass = sample.passes[local];
      if (local < group.count) {
        const auto id = group.begin + local;
        complete &= pass.id == id && pass.state == gpu_timers::SampleState::VALID
            && pass.model == resolved_models[id] && pass.width != 0 && pass.height != 0
            && pass.output_width != 0 && pass.output_height != 0;
        if ((covered & (1u << id)) != 0) complete = false;
        covered |= 1u << id;
      } else {
        complete &= pass.state != gpu_timers::SampleState::VALID;
      }
    }
    if (!complete) result.missing_stages |= 1u << stage;
    if (first != nullptr) {
      const auto& a = first->identity;
      const auto& b = sample.identity;
      // Durations are already converted with each exact submission's queue
      // frequency. This first reference metric deliberately requires one queue.
      // Stage-local work sizes, processing flags and rebuild epochs may differ;
      // their own current-contract checks must pass before reaching this layer.
      result.incompatible |= first->queue == nullptr || sample.queue != first->queue
          || a.source_generation == 0 || a.source_generation != b.source_generation
          || a.configuration_generation != b.configuration_generation || a.config_epoch != b.config_epoch
          || a.settings_hash != b.settings_hash || a.resolution_hash != b.resolution_hash
          || a.device_session != b.device_session || a.route != b.route || a.fg != b.fg
          || a.source_feature != b.source_feature || a.hdr != b.hdr || a.sr_mode != b.sr_mode
          || a.sr_width == 0 || a.sr_height == 0 || a.out_width == 0 || a.out_height == 0
          || a.sr_width != b.sr_width || a.sr_height != b.sr_height
          || a.out_width != b.out_width || a.out_height != b.out_height
          || a.display_format != b.display_format;
    } else {
      first = &sample;
    }
    // Each total includes that group's copies/linearize/commit once. Pass
    // totals and Look (already inside Resolve) must never be added again.
    total_us += sample.total_us;
  }
  result.valid = result.missing_stages == 0 && !result.incompatible
      && covered == (1u << plan.pass_count) - 1u;
  result.reference = result.valid && plan.group_count > 1;
  if (result.valid) result.milliseconds = static_cast<float>(total_us) / 1000.f;
  return result;
}

}  // namespace renodx::addons::dlss5
