/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace renodx::addons::dlss5::adaptive_resolution {

constexpr int64_t SAMPLE_INTERVAL_NS = 250'000'000;
constexpr int64_t SAMPLE_MAX_AGE_NS = 2'000'000'000;
constexpr int64_t WARMUP_NS = 1'000'000'000;
constexpr size_t MAX_PASSES = 4, MAX_LEVELS = 16;

enum class Reason : uint8_t {
  DISABLED, UNSUPPORTED, OUTPUT_UNAVAILABLE, COLLECTING, WARMING, OBSERVING,
  LOWERING, RAISING, NO_BENEFIT, AT_MINIMUM, FROZEN, FAILED, PREPARING, BUDGET,
};

inline const char* ReasonText(Reason reason) {
  switch (reason) {
    case Reason::DISABLED: return "Off";
    case Reason::UNSUPPORTED: return "Automatic sizing is unavailable on this path";
    case Reason::OUTPUT_UNAVAILABLE: return "Waiting for a reliable output FPS estimate";
    case Reason::COLLECTING: return "Collecting stable samples";
    case Reason::WARMING: return "Warming up after a size change";
    case Reason::OBSERVING: return "Maintaining quality near the target";
    case Reason::LOWERING: return "Trying a lower working resolution";
    case Reason::RAISING: return "Trying higher quality";
    case Reason::NO_BENEFIT: return "Kept the previous working resolution";
    case Reason::AT_MINIMUM: return "Minimum resolution reached below the FPS target";
    case Reason::FROZEN: return "Paused while editing or comparing";
    case Reason::FAILED: return "Paused after an NR execution failure";
    case Reason::PREPARING: return "Preparing working resolutions";
    case Reason::BUDGET: return "Paused: insufficient workset capacity";
  }
  return "Collecting stable samples";
}

struct Size {
  uint32_t width = 0, height = 0;
  bool operator==(const Size&) const = default;
};
using Sizes = std::array<Size, MAX_PASSES>;
struct Settings {
  bool enabled = false;
  float target_fps = 60.f, minimum_scale = 0.67f;
  uint32_t prepared_levels = 0;
  bool operator==(const Settings&) const = default;
};
struct Output {
  bool available = false;
  uint64_t source = 0, generation = 0, configuration = 0, serial = 0;
  int64_t begin_ns = 0, end_ns = 0;
  double fps = 0., mad = 0.;
  double observed_multiplier = 0.;
  bool multiplier_variable = false;
};
struct Ladder {
  std::array<Sizes, MAX_LEVELS> sizes{};
  std::array<float, MAX_LEVELS> coefficients{};
  size_t count = 0;
};

inline Ladder BuildLadder(const Sizes& upper, const Sizes& stage_floor, float minimum_scale) {
  Ladder result;
  minimum_scale = (std::isfinite(minimum_scale) ? std::clamp(minimum_scale, 0.33f, 1.f) : 0.67f);
  for (size_t step = 0; step < MAX_LEVELS; ++step) {
    const float coefficient = std::max(minimum_scale, 1.f - static_cast<float>(step) * 0.05f);
    Sizes sizes;
    for (size_t pass = 0; pass < MAX_PASSES; ++pass) {
      if (upper[pass].width == 0 || upper[pass].height == 0) continue;
      sizes[pass] = {
          .width = std::min(upper[pass].width, std::max(stage_floor[pass].width,
              std::max(1u, static_cast<uint32_t>(std::llround(upper[pass].width * double{coefficient}))))),
          .height = std::min(upper[pass].height, std::max(stage_floor[pass].height,
              std::max(1u, static_cast<uint32_t>(std::llround(upper[pass].height * double{coefficient})))))};
    }
    if (result.count == 0 || result.sizes[result.count - 1] != sizes) {
      result.sizes[result.count] = sizes;
      result.coefficients[result.count++] = coefficient;
    }
    if (coefficient <= minimum_scale) break;
  }
  return result;
}

struct Status {
  Reason reason = Reason::DISABLED;
  float coefficient = 1.f;
  uint64_t generation = 0;
  Sizes upper{}, requested{}, applied{};
  double output_fps = 0., gpu_ms = 0.;
  uint32_t ready_levels = 0, prepared_levels = 0;
  double preparation_cpu_ms = 0.;
  uint64_t preparation_vram_delta_bytes = 0, peak_process_vram_bytes = 0;
};
struct GpuSample {
  uint64_t generation = 0, frame = 0, sequence = 0, context = 0;
  int64_t recorded_ns = 0, completed_ns = 0;
  double total_us = 0.;
  uint32_t role = 1, required_roles = 1;
  bool valid = false, stable = false;
};

struct Session {
  Status status;
  Settings settings;
  Ladder ladder;
  uint64_t identity = 0, output_source = 0, output_generation = 0, output_configuration = 0;
  uint64_t configuration_epoch = 0;
  uint64_t last_output_serial = 0, last_sequence = 0, gpu_context = 0;
  size_t level = 0, previous_level = 0;
  int64_t applied_ns = 0, stable_after_ns = 0, last_switch_ns = 0, at_target_since_ns = 0;
  int64_t next_measure_ns = 0;
  int64_t failed_raise_until_ns = 0;
  size_t failed_raise_level = MAX_LEVELS;
  bool trial = false, raising = false, extra_window = false, blocked = false, failed = false;
  double baseline_fps = 0., baseline_fps_mad = 0., baseline_gpu_us = 0., baseline_gpu_mad = 0.;
  double baseline_multiplier = 0.;
  double blocked_fps = 0., blocked_gpu_us = 0.;
  std::array<double, 5> gpu_us{};
  std::array<int64_t, 5> gpu_recorded_ns{};
  size_t gpu_count = 0;
  GpuSample pending;
  uint32_t pending_roles = 0;

  void ResetMeasurements() {
    gpu_count = 0;
    gpu_us = {};
    gpu_recorded_ns = {};
    pending = {};
    pending_roles = 0;
    last_output_serial = 0;
    at_target_since_ns = 0;
  }

  void Request(size_t candidate, int64_t now_ns) {
    level = candidate;
    ++status.generation;
    status.coefficient = ladder.coefficients[level];
    status.requested = ladder.sizes[level];
    applied_ns = 0;
    stable_after_ns = now_ns + WARMUP_NS;
    last_switch_ns = now_ns;
    ResetMeasurements();
  }

  void Applied(uint64_t generation, const Sizes& actual, int64_t now_ns) {
    if (generation != status.generation || actual != status.requested) return;
    status.applied = actual;
    if (applied_ns != 0) return;
    applied_ns = now_ns;
    stable_after_ns = now_ns + WARMUP_NS;
  }

  void Fail(uint64_t generation) {
    if (generation != status.generation) return;
    failed = true;
    trial = false;
    status.reason = Reason::FAILED;
    ResetMeasurements();
  }

  void ObserveGpu(const GpuSample& sample, int64_t now_ns) {
    if (sample.generation != status.generation
        || (sample.sequence <= last_sequence && (pending_roles == 0 || sample.frame != pending.frame))) return;
    last_sequence = std::max(last_sequence, sample.sequence);
    if (!sample.valid || !sample.stable || sample.recorded_ns < stable_after_ns || applied_ns == 0
        || sample.recorded_ns > sample.completed_ns || sample.completed_ns > now_ns
        || now_ns - sample.recorded_ns > SAMPLE_MAX_AGE_NS
        || now_ns - sample.completed_ns > SAMPLE_MAX_AGE_NS) {
      pending_roles = 0;
      return;
    }
    if (gpu_context != 0 && gpu_context != sample.context) {
      ResetMeasurements();
      trial = false;
    }
    gpu_context = sample.context;
    if (pending_roles == 0 || pending.frame != sample.frame || pending.generation != sample.generation
        || pending.required_roles != sample.required_roles || pending.context != sample.context) {
      pending = sample;
      pending.total_us = 0.;
      pending_roles = 0;
    }
    if ((pending_roles & sample.role) != 0) return;
    pending_roles |= sample.role;
    pending.total_us += sample.total_us;
    pending.recorded_ns = std::min(pending.recorded_ns, sample.recorded_ns);
    if (pending_roles != sample.required_roles) return;
    pending_roles = 0;
    if (gpu_count != 0 && pending.recorded_ns - gpu_recorded_ns[gpu_count - 1] < SAMPLE_INTERVAL_NS) return;
    if (gpu_count == gpu_us.size()) {
      std::move(gpu_us.begin() + 1, gpu_us.end(), gpu_us.begin());
      std::move(gpu_recorded_ns.begin() + 1, gpu_recorded_ns.end(), gpu_recorded_ns.begin());
      --gpu_count;
    }
    gpu_us[gpu_count] = pending.total_us;
    gpu_recorded_ns[gpu_count++] = pending.recorded_ns;
  }

  bool GpuStatistics(int64_t now_ns, double* median, double* mad) const {
    if (gpu_count != gpu_us.size() || gpu_recorded_ns.back() - gpu_recorded_ns.front() < WARMUP_NS
        || now_ns < gpu_recorded_ns.back() || now_ns - gpu_recorded_ns.front() > SAMPLE_MAX_AGE_NS) return false;
    auto sorted = gpu_us;
    std::sort(sorted.begin(), sorted.end());
    *median = sorted[2];
    for (size_t i = 0; i < sorted.size(); ++i) sorted[i] = std::abs(gpu_us[i] - *median);
    std::sort(sorted.begin(), sorted.end());
    *mad = sorted[2];
    return true;
  }

  void Prepare(const Settings& requested_settings, const Sizes& upper, const Sizes& floor,
               uint64_t source_identity, const Output& output, int64_t now_ns, bool supported, bool frozen) {
    Settings next = requested_settings;
    next.target_fps = (std::isfinite(next.target_fps) ? std::clamp(next.target_fps, 30.f, 360.f) : 60.f);
    next.minimum_scale = (std::isfinite(next.minimum_scale) ? std::clamp(next.minimum_scale, 0.33f, 1.f) : 0.67f);
    const bool changed = identity != source_identity || settings != next || status.upper != upper;
    if (changed) {
      const uint64_t generation = status.generation + 1;
      *this = Session{};
      status.generation = generation;
      settings = next;
      identity = source_identity;
      status.upper = upper;
      ladder = BuildLadder(upper, floor, next.minimum_scale);
      if (next.prepared_levels >= 2 && ladder.count > next.prepared_levels) {
        Ladder prepared;
        prepared.count = next.prepared_levels;
        for (size_t tier = 0; tier < prepared.count; ++tier) {
          const size_t index = tier * (ladder.count - 1) / (prepared.count - 1);
          prepared.sizes[tier] = ladder.sizes[index];
          prepared.coefficients[tier] = ladder.coefficients[index];
        }
        ladder = prepared;
      }
      Request(0, now_ns);
    }
    if (!settings.enabled || !supported) {
      if (level != 0) Request(0, now_ns);
      trial = false;
      status.reason = (settings.enabled ? Reason::UNSUPPORTED : Reason::DISABLED);
      return;
    }
    if (failed) {
      status.reason = Reason::FAILED;
      return;
    }
    if (frozen) {
      trial = false;
      ResetMeasurements();
      status.reason = Reason::FROZEN;
      return;
    }
    const bool output_changed = output_source != output.source || output_generation != output.generation
        || output_configuration != output.configuration;
    if (output_changed || !output.available) {
      if (trial) Request(previous_level, now_ns);
      trial = false;
      blocked = false;
      ResetMeasurements();
      output_source = output.source;
      output_generation = output.generation;
      output_configuration = output.configuration;
    }
    status.output_fps = (output.available ? output.fps : 0.);
    if (!output.available) {
      status.reason = Reason::OUTPUT_UNAVAILABLE;
      return;
    }
    if (output.multiplier_variable && (!std::isfinite(output.observed_multiplier) || output.observed_multiplier <= 0.)) {
      if (trial) Request(previous_level, now_ns);
      trial = false;
      ResetMeasurements();
      status.reason = Reason::COLLECTING;
      return;
    }
    if (applied_ns == 0 || now_ns < stable_after_ns) {
      status.reason = Reason::WARMING;
      return;
    }
    double gpu = 0., gpu_mad = 0.;
    if (output.begin_ns < stable_after_ns || output.end_ns > now_ns || now_ns - output.end_ns > SAMPLE_MAX_AGE_NS
        || !GpuStatistics(now_ns, &gpu, &gpu_mad)) {
      status.reason = Reason::COLLECTING;
      return;
    }
    status.gpu_ms = gpu / 1000.;
    if (last_output_serial == output.serial) return;
    last_output_serial = output.serial;
    if (trial) {
      const bool multiplier_changed = baseline_multiplier > 0. && output.observed_multiplier > 0.
          && std::abs(output.observed_multiplier / baseline_multiplier - 1.) > 0.10;
      if (multiplier_changed && !extra_window) {
        extra_window = true;
        stable_after_ns = output.end_ns;
        return;
      }
      const bool on_target = output.fps >= settings.target_fps * 0.95;
      const double fps_margin = std::max({1., baseline_fps * 0.02, 2. * output.mad, 2. * baseline_fps_mad});
      const bool cheaper = baseline_gpu_us - gpu > std::max({1., 2. * baseline_gpu_mad, 2. * gpu_mad});
      const bool improved = output.fps - baseline_fps > fps_margin;
      if (!multiplier_changed && ((raising && on_target) || (!raising && cheaper && (on_target || improved)))) {
        trial = false;
        status.reason = Reason::OBSERVING;
      } else {
        const bool uncertain = !multiplier_changed && !raising && cheaper && output.fps >= baseline_fps
            && output.fps - baseline_fps <= fps_margin && output.mad > 0.;
        if (uncertain && !extra_window) {
          extra_window = true;
          stable_after_ns = output.end_ns;
          return;
        }
        if (raising) {
          failed_raise_level = level;
          failed_raise_until_ns = now_ns + 30'000'000'000;
        } else {
          blocked = true;
          blocked_fps = (multiplier_changed ? output.fps : baseline_fps);
          blocked_gpu_us = baseline_gpu_us;
        }
        Request(previous_level, now_ns);
        trial = false;
        status.reason = Reason::NO_BENEFIT;
        return;
      }
    }
    if (blocked && (std::abs(output.fps - blocked_fps) > std::max(3., blocked_fps * 0.15)
        || std::abs(gpu - blocked_gpu_us) > std::max(100., blocked_gpu_us * 0.2))) blocked = false;
    if (output.fps >= settings.target_fps * 0.99) {
      if (at_target_since_ns == 0) at_target_since_ns = output.begin_ns;
    } else {
      at_target_since_ns = 0;
    }
    size_t candidate = level;
    if (output.fps < settings.target_fps * 0.95 && !blocked && now_ns - last_switch_ns >= 3'000'000'000) {
      candidate = std::min(level + (settings.prepared_levels != 0 ? 1 : 2), ladder.count - 1);
      raising = false;
    } else if (level != 0 && at_target_since_ns != 0 && now_ns - at_target_since_ns >= 6'000'000'000
        && now_ns - last_switch_ns >= 10'000'000'000
        && (level - 1 != failed_raise_level || now_ns >= failed_raise_until_ns)) {
      candidate = level - 1;
      raising = true;
    }
    if (candidate == level) {
      status.reason = (blocked ? Reason::NO_BENEFIT
          : (level + 1 == ladder.count && output.fps < settings.target_fps * 0.95 ? Reason::AT_MINIMUM : Reason::OBSERVING));
      return;
    }
    previous_level = level;
    baseline_fps = output.fps;
    baseline_fps_mad = output.mad;
    baseline_gpu_us = gpu;
    baseline_gpu_mad = gpu_mad;
    baseline_multiplier = output.observed_multiplier;
    extra_window = false;
    trial = true;
    Request(candidate, now_ns);
    status.reason = (raising ? Reason::RAISING : Reason::LOWERING);
  }
};

struct Frame {
  std::shared_ptr<Session> session;
  Sizes sizes{}, upper{};
  uint64_t generation = 0, application_frame = 0;
  uint64_t layout_identity = 0;
  uint32_t required_roles = 1, role = 1;
  uint32_t slot = 0;
  uint32_t output_count = 1;
  uintptr_t output_identity = 0;
  bool enabled = false, measure = false, preparing = false, immediate = true;
};

}  // namespace renodx::addons::dlss5::adaptive_resolution
