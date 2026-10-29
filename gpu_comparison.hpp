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

namespace renodx::addons::dlss5::gpu_comparison {

constexpr size_t STAGE_COUNT = 3, MAX_PASSES = 4, SAMPLE_COUNT = 16;
constexpr uint64_t INTERVAL_NS = 250'000'000, STABILITY_NS = 1'000'000'000, TIMEOUT_NS = 10'000'000'000;

struct Identity {
  uint64_t configuration_generation = 0, settings_epoch = 0, settings_hash = 0, resolution_hash = 0;
  uint64_t config_epoch = 0, stable_epoch = 0;
  uint64_t source_generation = 0, source_feature = 0, device_session = 0, queue = 0;
  uint64_t route = 0, fg = 0, hdr = 0, sr_mode = 0, processing_hash = 0, display_format = 0;
  uint32_t sr_width = 0, sr_height = 0, out_width = 0, out_height = 0;
  bool operator==(const Identity&) const = default;
};

enum class PassState : uint8_t { NOT_EXECUTED, VALID };
struct Pass {
  uint32_t id = UINT32_MAX, model = 0, width = 0, height = 0, output_width = 0, output_height = 0;
  double eval_us = 0;
  PassState state = PassState::NOT_EXECUTED;
  bool look_executed = false;
};

// valid is the adapter's exact, closed-against-replay GPU completion proof.
// Identity and pass metadata must be frozen at recording, never read at publication.
struct Sample {
  uint32_t stage = 0;
  uint64_t sequence = 0, recorded_ns = 0, completed_ns = 0, stable_after_ns = 0;
  bool valid = false, stable = false;
  Identity identity{};
  uint32_t pass_count = 0;
  std::array<Pass, MAX_PASSES> passes{};
  double total_us = 0;
};

enum class Status : uint8_t { IDLE, COLLECTING, COMPLETE, FAILED };
enum class Reason : uint8_t {
  NONE, MISSING_DATA, TIMEOUT, IDENTITY_CHANGED, INCOMPATIBLE_IDENTITY,
  PASS_MISMATCH, MULTIPLE_RESOLUTION_CHANGES, NO_RESOLUTION_CHANGE,
  AMBIGUOUS_RESOLUTION, INVALID_SAMPLE, ZERO_BASELINE, HIGH_VARIATION, INSUFFICIENT_DIFFERENCE,
};
enum class Advice : uint8_t { NONE, LOWER_RESOLUTION_LOWER_MEASURED_COST_CHECK_QUALITY };

struct RawSample {
  uint64_t sequence = 0, recorded_ns = 0, completed_ns = 0;
  std::array<double, MAX_PASSES> eval_us{};
  double total_us = 0;
};
struct StageWindow {
  Status status = Status::IDLE;
  Reason reason = Reason::NONE;
  size_t count = 0;
  uint64_t started_ns = 0, stable_after_ns = 0, min_sequence = 0, last_sequence = 0;
  bool has_identity = false;
  Identity identity{};
  uint32_t pass_count = 0;
  std::array<Pass, MAX_PASSES> passes{};
  std::array<RawSample, SAMPLE_COUNT> samples{};
};
struct Statistics {
  double median_us = 0, mad_us = 0, relative_mad = 0;
};
struct MetricComparison {
  Statistics a{}, b{};
  double delta_us = 0, percent = 0;
  bool percent_valid = false;
  Reason reason = Reason::MISSING_DATA;
  Advice advice = Advice::NONE;
};
struct StageComparison {
  bool selected = false;
  uint32_t pass_count = 0;
  MetricComparison total{};
  std::array<MetricComparison, MAX_PASSES> passes{};
};
struct Comparison {
  Reason reason = Reason::MISSING_DATA;
  std::array<StageComparison, STAGE_COUNT> stages{};
};

struct Engine {
  std::array<StageWindow, STAGE_COUNT> baseline{}, control{};
  uint32_t baseline_mask = 0, control_mask = 0;
  bool recording_baseline = true;

  // Restarting A invalidates B. Restarting B preserves the completed A evidence.
  bool Start(bool is_baseline, uint32_t stage_mask, uint64_t now_ns, uint64_t min_sequence) {
    if (stage_mask == 0 || (stage_mask >> STAGE_COUNT) != 0) return false;
    for (const auto* windows : {&baseline, &control}) {
      for (const auto& window : *windows) {
        if (window.status == Status::COLLECTING) return false;
      }
    }
    if (!is_baseline) {
      if (stage_mask != baseline_mask) return false;
      for (size_t stage = 0; stage < STAGE_COUNT; ++stage) {
        if ((stage_mask & (1u << stage)) != 0 && baseline[stage].status != Status::COMPLETE) return false;
      }
    }
    recording_baseline = is_baseline;
    if (is_baseline) {
      baseline = {};
      control = {};
      baseline_mask = stage_mask;
      control_mask = 0;
    } else {
      control = {};
      control_mask = stage_mask;
    }
    auto& windows = (is_baseline ? baseline : control);
    for (size_t stage = 0; stage < STAGE_COUNT; ++stage) {
      if ((stage_mask & (1u << stage)) == 0) continue;
      windows[stage] = StageWindow{
          .status = Status::COLLECTING, .started_ns = now_ns, .stable_after_ns = now_ns + STABILITY_NS,
          .min_sequence = min_sequence};
    }
    return true;
  }

  void Tick(uint64_t now_ns) {
    for (auto* windows : {&baseline, &control}) {
      for (auto& window : *windows) {
        if (window.status != Status::COLLECTING) continue;
        if (now_ns < window.started_ns || now_ns - window.started_ns >= TIMEOUT_NS) {
          window.status = Status::FAILED;
          window.reason = Reason::TIMEOUT;
        }
      }
    }
  }

  void Observe(const Sample& sample, uint64_t now_ns) {
    Tick(now_ns);
    if (sample.stage >= STAGE_COUNT || !sample.valid) return;
    auto& windows = (recording_baseline ? baseline : control);
    auto& window = windows[sample.stage];
    if (window.status != Status::COLLECTING
        || sample.sequence < window.min_sequence
        || (window.has_identity && sample.sequence <= window.last_sequence)
        || sample.recorded_ns < window.started_ns || sample.recorded_ns > now_ns
        || sample.completed_ns < sample.recorded_ns || sample.completed_ns > now_ns) return;
    window.last_sequence = sample.sequence;
    auto previous_identity = window.identity;
    if (window.has_identity && window.count == 0 && sample.identity.stable_epoch != previous_identity.stable_epoch) {
      previous_identity.stable_epoch = sample.identity.stable_epoch;
    }
    if (window.has_identity && sample.identity != previous_identity) {
      window.status = Status::FAILED;
      window.reason = Reason::IDENTITY_CHANGED;
      return;
    }
    if (!window.has_identity) {
      window.identity = sample.identity;
      window.has_identity = true;
    }
    window.identity.stable_epoch = sample.identity.stable_epoch;
    window.stable_after_ns = std::max(window.stable_after_ns, sample.stable_after_ns);
    if (!sample.stable) {
      // An upstream reset/warmup after collection starts cannot share a window.
      if (window.count != 0) {
        window.status = Status::FAILED;
        window.reason = Reason::IDENTITY_CHANGED;
      }
      return;
    }
    if (sample.recorded_ns < window.stable_after_ns) return;
    if (sample.pass_count == 0 || sample.pass_count > MAX_PASSES
        || !std::isfinite(sample.total_us) || sample.total_us < 0) return;
    for (size_t pass = 0; pass < sample.pass_count; ++pass) {
      const auto& layer = sample.passes[pass];
      if (layer.id >= MAX_PASSES || layer.state != PassState::VALID || layer.width == 0 || layer.height == 0
          || layer.output_width == 0 || layer.output_height == 0
          || !std::isfinite(layer.eval_us) || layer.eval_us < 0) return;
      for (size_t previous = 0; previous < pass; ++previous) {
        if (sample.passes[previous].id == layer.id) return;
      }
    }
    if (window.count != 0) {
      bool same_passes = sample.pass_count == window.pass_count;
      for (size_t pass = 0; pass < sample.pass_count && same_passes; ++pass) {
        const auto& recorded = sample.passes[pass];
        const auto& expected = window.passes[pass];
        same_passes = recorded.id == expected.id && recorded.model == expected.model
                      && recorded.width == expected.width && recorded.height == expected.height
                      && recorded.output_width == expected.output_width && recorded.output_height == expected.output_height
                      && recorded.state == expected.state && recorded.look_executed == expected.look_executed;
      }
      if (!same_passes) {
        window.status = Status::FAILED;
        window.reason = Reason::PASS_MISMATCH;
        return;
      }
      const auto previous_ns = window.samples[window.count - 1].recorded_ns;
      if (sample.recorded_ns < previous_ns || sample.recorded_ns - previous_ns < INTERVAL_NS) return;
    } else {
      window.pass_count = sample.pass_count;
      window.passes = sample.passes;
    }
    auto& raw = window.samples[window.count++];
    raw = RawSample{.sequence = sample.sequence, .recorded_ns = sample.recorded_ns, .completed_ns = sample.completed_ns,
                    .total_us = sample.total_us};
    for (size_t pass = 0; pass < sample.pass_count; ++pass) raw.eval_us[pass] = sample.passes[pass].eval_us;
    if (window.count == SAMPLE_COUNT) window.status = Status::COMPLETE;
  }

  void Reset() { *this = Engine{}; }

  // Stage totals are independent GPU intervals, never a sum or a frame latency.
  Comparison Results() const {
    Comparison result{};
    if (baseline_mask == 0 || baseline_mask != control_mask) return result;
    size_t changed_count = 0, changed_stage = 0, changed_pass = 0;
    bool b_is_lower = false;
    std::array<bool, MAX_PASSES> seen_ids{};
    for (size_t stage = 0; stage < STAGE_COUNT; ++stage) {
      if ((baseline_mask & (1u << stage)) == 0) continue;
      const auto& a = baseline[stage];
      const auto& b = control[stage];
      if (a.status == Status::FAILED || b.status == Status::FAILED) {
        result.reason = (a.status == Status::FAILED ? a.reason : b.reason);
        return result;
      }
      if (a.status != Status::COMPLETE || b.status != Status::COMPLETE) return result;
      auto a_identity = a.identity;
      auto b_identity = b.identity;
      a_identity.configuration_generation = b_identity.configuration_generation = 0;
      a_identity.settings_epoch = b_identity.settings_epoch = 0;
      a_identity.resolution_hash = b_identity.resolution_hash = 0;
      a_identity.config_epoch = b_identity.config_epoch = 0;
      a_identity.stable_epoch = b_identity.stable_epoch = 0;
      if (a_identity != b_identity) {
        result.reason = Reason::INCOMPATIBLE_IDENTITY;
        return result;
      }
      if (a.pass_count != b.pass_count) {
        result.reason = Reason::PASS_MISMATCH;
        return result;
      }
      for (size_t pass = 0; pass < a.pass_count; ++pass) {
        const auto& x = a.passes[pass];
        const auto& y = b.passes[pass];
        if (x.id != y.id || x.model != y.model || x.state != y.state || x.look_executed != y.look_executed || seen_ids[x.id]) {
          result.reason = Reason::PASS_MISMATCH;
          return result;
        }
        seen_ids[x.id] = true;
        if (x.width == y.width && x.height == y.height) {
          if (x.output_width != y.output_width || x.output_height != y.output_height) {
            result.reason = Reason::AMBIGUOUS_RESOLUTION;
            return result;
          }
          continue;
        }
        ++changed_count;
        changed_stage = stage;
        changed_pass = pass;
        const bool lower = y.width <= x.width && y.height <= x.height;
        const bool higher = y.width >= x.width && y.height >= x.height;
        const bool output_lower = y.output_width <= x.output_width && y.output_height <= x.output_height;
        const bool output_higher = y.output_width >= x.output_width && y.output_height >= x.output_height;
        if ((!lower && !higher) || (lower && !output_lower) || (higher && !output_higher)) {
          result.reason = Reason::AMBIGUOUS_RESOLUTION;
          return result;
        }
        b_is_lower = lower;
      }
    }
    if (changed_count != 1) {
      result.reason = (changed_count == 0 ? Reason::NO_RESOLUTION_CHANGE : Reason::MULTIPLE_RESOLUTION_CHANGES);
      return result;
    }
    result.reason = Reason::NONE;
    for (size_t stage = 0; stage < STAGE_COUNT; ++stage) {
      if ((baseline_mask & (1u << stage)) == 0) continue;
      const auto& a = baseline[stage];
      const auto& b = control[stage];
      auto& output = result.stages[stage];
      output.selected = true;
      output.pass_count = a.pass_count;
      for (size_t metric = 0; metric <= a.pass_count; ++metric) {
        auto& comparison = (metric == a.pass_count ? output.total : output.passes[metric]);
        for (size_t side = 0; side < 2; ++side) {
          const auto& window = (side == 0 ? a : b);
          auto& statistics = (side == 0 ? comparison.a : comparison.b);
          std::array<double, SAMPLE_COUNT> values{};
          for (size_t i = 0; i < SAMPLE_COUNT; ++i) {
            values[i] = (metric == a.pass_count ? window.samples[i].total_us : window.samples[i].eval_us[metric]);
          }
          std::sort(values.begin(), values.end());
          statistics.median_us = values[7] / 2 + values[8] / 2;
          for (auto& value : values) value = std::abs(value - statistics.median_us);
          std::sort(values.begin(), values.end());
          statistics.mad_us = values[7] / 2 + values[8] / 2;
          if (statistics.median_us > 0) statistics.relative_mad = statistics.mad_us / statistics.median_us;
        }
        comparison.delta_us = comparison.b.median_us - comparison.a.median_us;
        if (comparison.a.median_us <= 0 || comparison.b.median_us <= 0) {
          comparison.reason = Reason::ZERO_BASELINE;
          continue;
        }
        comparison.percent = (comparison.delta_us / comparison.a.median_us) * 100;
        comparison.percent_valid = std::isfinite(comparison.percent);
        if (comparison.a.relative_mad > 0.15 || comparison.b.relative_mad > 0.15) {
          comparison.reason = Reason::HIGH_VARIATION;
          continue;
        }
        const auto difference = std::abs(comparison.delta_us);
        if (difference < 2 * (comparison.a.mad_us + comparison.b.mad_us)
            || difference < std::max(1.0, comparison.a.median_us * 0.03)) {
          comparison.reason = Reason::INSUFFICIENT_DIFFERENCE;
          continue;
        }
        comparison.reason = Reason::NONE;
        if (stage == changed_stage && (metric == changed_pass || metric == a.pass_count)
            && (b_is_lower ? comparison.delta_us < 0 : comparison.delta_us > 0)) {
          comparison.advice = Advice::LOWER_RESOLUTION_LOWER_MEASURED_COST_CHECK_QUALITY;
        }
      }
    }
    return result;
  }
};

}  // namespace renodx::addons::dlss5::gpu_comparison
