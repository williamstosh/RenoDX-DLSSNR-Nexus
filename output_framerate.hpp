/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <shared_mutex>

namespace renodx::addons::dlss5::output_framerate {

enum class Source { NONE, PRESENT, XEFG_STATUS, NATIVE_STATE, NATIVE_FIXED };
enum class Reason { kWARMUP, kREADY, kSTALE, kFAILED, kTEST_PRESENT, kDUPLICATE, kUNKNOWN_FG, kAMBIGUOUS };

inline const char* SourceText(Source source) {
  switch (source) {
    case Source::PRESENT: return "Present cadence estimate";
    case Source::XEFG_STATUS: return "XeFG SDK output estimate";
    case Source::NATIVE_STATE: return "DLSS FG SDK output estimate";
    case Source::NATIVE_FIXED: return "Fixed FG multiplier estimate";
    default: return "Output FPS unavailable";
  }
}

inline const char* ReasonText(Reason reason) {
  switch (reason) {
    case Reason::kREADY: return "Ready";
    case Reason::kSTALE: return "Waiting for fresh output samples";
    case Reason::kFAILED: return "Output observation failed";
    case Reason::kTEST_PRESENT: return "Waiting after a test Present";
    case Reason::kDUPLICATE: return "Waiting after a repeated application frame";
    case Reason::kUNKNOWN_FG: return "FG output rate is unknown";
    case Reason::kAMBIGUOUS: return "Output identity is ambiguous";
    default: return "Collecting output frame rate";
  }
}

struct Snapshot {
  bool available = false;
  Reason reason = Reason::kWARMUP;
  Source source = Source::NONE;
  uint64_t source_generation = 0;
  uint64_t configuration_generation = 0;
  uint64_t window_serial = 0;
  int64_t window_begin_ns = 0;
  int64_t window_end_ns = 0;
  double fps = 0;
  double fps_mad = 0;
  uint32_t requested_multiplier = 0;
  double observed_multiplier = 0;
  bool dynamic = false;
  uint32_t maximum_multiplier = 0;
  uint32_t declared_multiplier = 0;
  bool fg_off = false;
  bool multiplier_variable = false;
};

inline int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Four complete cadence bins retain no per-frame history. SDK counts include
// zero: a successful interval with no queued frames is still a measurement.
struct Stream {
  static constexpr int64_t BIN_NS = 500'000'000;
  static constexpr int64_t STALE_NS = 1'000'000'000;
  static constexpr int64_t NATIVE_STATE_STALE_NS = 2'000'000'000;
  static constexpr int64_t MaxAgeNs(Source source) {
    return (source == Source::NATIVE_STATE ? NATIVE_STATE_STALE_NS : STALE_NS);
  }
  struct Bin {
    int64_t begin_ns = 0, end_ns = 0;
    double frames = 0;
    uint64_t applications = 0;
    bool applications_known = true;
  };
  Snapshot snapshot;
  std::array<Bin, 4> bins{};
  Bin pending;
  size_t count = 0;
  size_t next = 0;
  int64_t previous_ns = 0;
  uint64_t identity = 0;

  void Reset(Reason reason = Reason::kWARMUP) {
    snapshot.available = false;
    snapshot.reason = reason;
    snapshot.window_begin_ns = snapshot.window_end_ns = 0;
    snapshot.fps = snapshot.fps_mad = 0;
    snapshot.observed_multiplier = 0;
    snapshot.requested_multiplier = snapshot.maximum_multiplier = snapshot.declared_multiplier = 0;
    snapshot.dynamic = snapshot.fg_off = false;
    snapshot.multiplier_variable = false;
    ++snapshot.source_generation;
    bins = {};
    pending = {};
    count = next = 0;
    previous_ns = 0;
  }

  void Observe(Source source, uint64_t next_identity, uint64_t configuration,
               int64_t now_ns, double frames, uint32_t requested = 0,
               uint32_t = 0, bool dynamic = false, uint32_t maximum = 0, bool fg_off = false,
               std::optional<uint64_t> applications = std::nullopt) {
    if (snapshot.source != source || identity != next_identity
        || snapshot.configuration_generation != configuration
        || snapshot.requested_multiplier != requested || snapshot.dynamic != dynamic
        || snapshot.fg_off != fg_off) {
      Reset();
    }
    snapshot.source = source;
    snapshot.configuration_generation = configuration;
    snapshot.requested_multiplier = requested;
    snapshot.dynamic = dynamic;
    snapshot.maximum_multiplier = maximum;
    snapshot.fg_off = fg_off;
    identity = next_identity;
    if (now_ns <= 0 || !std::isfinite(frames) || frames < 0) {
      Reset(Reason::kFAILED);
      return;
    }
    if (previous_ns != 0 && (now_ns <= previous_ns || now_ns - previous_ns > MaxAgeNs(source))) {
      Reset(Reason::kSTALE);
    }
    if (previous_ns == 0) {
      previous_ns = pending.begin_ns = now_ns;
      return;
    }
    previous_ns = pending.end_ns = now_ns;
    pending.frames += frames;
    if (source == Source::XEFG_STATUS) {
      ++pending.applications;
    } else if (source == Source::NATIVE_STATE && applications.has_value()) {
      pending.applications += *applications;
    } else {
      pending.applications_known = false;
    }
    if (pending.end_ns - pending.begin_ns < BIN_NS) return;
    bins[next] = pending;
    next = (next + 1) % bins.size();
    count = std::min(count + 1, bins.size());
    pending = {.begin_ns = now_ns};
    if (count != bins.size()) return;
    double total_frames = 0;
    uint64_t total_applications = 0;
    bool applications_known = true;
    int64_t total_ns = 0;
    std::array<double, 4> rates{};
    for (size_t index = 0; index < bins.size(); ++index) {
      const auto& bin = bins[index];
      total_frames += bin.frames;
      total_applications += bin.applications;
      applications_known &= bin.applications_known;
      total_ns += bin.end_ns - bin.begin_ns;
      rates[index] = bin.frames * 1e9 / static_cast<double>(bin.end_ns - bin.begin_ns);
    }
    std::sort(rates.begin(), rates.end());
    const double median = (rates[1] + rates[2]) * 0.5;
    for (auto& rate : rates) rate = std::abs(rate - median);
    std::sort(rates.begin(), rates.end());
    snapshot.available = true;
    snapshot.reason = Reason::kREADY;
    snapshot.window_begin_ns = bins[next].begin_ns;
    snapshot.window_end_ns = now_ns;
    snapshot.fps = total_frames * 1e9 / static_cast<double>(total_ns);
    snapshot.fps_mad = (rates[1] + rates[2]) * 0.5;
    snapshot.observed_multiplier = ((source == Source::XEFG_STATUS || source == Source::NATIVE_STATE)
        && applications_known && total_applications != 0
        ? total_frames / static_cast<double>(total_applications) : 0);
    ++snapshot.window_serial;
  }

  Snapshot Read(int64_t now_ns) const {
    auto result = snapshot;
    if (previous_ns != 0 && (now_ns < previous_ns || now_ns - previous_ns > MaxAgeNs(snapshot.source))) {
      result.available = false;
      result.reason = Reason::kSTALE;
      result.requested_multiplier = 0;
      result.maximum_multiplier = result.declared_multiplier = 0;
      result.dynamic = result.fg_off = false;
    }
    if (!result.available) result.observed_multiplier = 0;
    return result;
  }
};

namespace internal {
struct State {
  std::shared_mutex mutex;
  Stream stream;
  uint64_t native_viewport = 0;
  uint64_t native_configuration = 0;
  uint64_t native_version = 0;
  uint32_t native_multiplier = 0;
  uint32_t native_mode = 0;
  bool native_options_known = false;
  bool native_enabled = false;
  bool native_dynamic = false;
  int64_t native_evaluation_ns = 0;
  bool native_evaluation_success = false;
  uint32_t native_declared_multiplier = 0;
  int64_t native_state_ns = 0;
  uint64_t native_application_identity = 0;
  uint64_t native_applications = 0, native_state_applications = 0;
  int64_t native_application_ns = 0;
  bool native_state_application_coverage = false;
  int64_t xefg_ns = 0;
  uint64_t present_identity = 0;
  int64_t present_ns = 0;
};
static State state;
}  // namespace internal

inline Snapshot Read(int64_t now_ns) {
  std::shared_lock lock(internal::state.mutex);
  auto snapshot = internal::state.stream.Read(now_ns);
  const auto& state = internal::state;
  snapshot.multiplier_variable = snapshot.source == Source::NATIVE_STATE && state.native_mode != 1;
  if (state.native_evaluation_success && now_ns >= state.native_evaluation_ns
      && now_ns - state.native_evaluation_ns <= Stream::STALE_NS
      && snapshot.source != Source::XEFG_STATUS) {
    snapshot.declared_multiplier = state.native_declared_multiplier;
  }
  return snapshot;
}

inline void Invalidate(Reason reason = Reason::kFAILED) {
  std::unique_lock lock(internal::state.mutex);
  internal::state.stream.Reset(reason);
}

inline void ReleaseNative() {
  std::unique_lock lock(internal::state.mutex);
  auto& state = internal::state;
  state.native_options_known = state.native_enabled = false;
  state.native_evaluation_success = false;
  state.native_evaluation_ns = state.native_state_ns = 0;
  state.native_declared_multiplier = 0;
  state.native_application_identity = state.native_applications = state.native_state_applications = 0;
  state.native_application_ns = 0;
  state.native_state_application_coverage = false;
  ++state.native_configuration;
  if (state.stream.snapshot.source != Source::XEFG_STATUS) state.stream.Reset();
}

inline void ObserveNativeOptions(uint64_t viewport, uint64_t version, uint32_t mode,
                                 uint32_t multiplier) {
  std::unique_lock lock(internal::state.mutex);
  auto& state = internal::state;
  const bool enabled = mode != 0;
  const bool dynamic = mode == 3;
  const uint32_t effective_request = (enabled ? multiplier : 1);
  if (!state.native_options_known || state.native_viewport != viewport
      || state.native_version != version
      || state.native_mode != mode
      || state.native_enabled != enabled || state.native_dynamic != dynamic
      || state.native_multiplier != effective_request) {
    ++state.native_configuration;
    state.stream.Reset();
    state.stream.snapshot.source = Source::NONE;
    state.stream.identity = 0;
    state.native_state_ns = 0;
    state.native_application_identity = state.native_applications = state.native_state_applications = 0;
    state.native_application_ns = 0;
    state.native_state_application_coverage = false;
  }
  state.native_options_known = version >= 1;
  state.native_viewport = viewport;
  state.native_version = version;
  state.native_enabled = enabled;
  state.native_dynamic = dynamic;
  state.native_mode = mode;
  state.native_multiplier = effective_request;
  state.stream.snapshot.requested_multiplier = (enabled ? multiplier : 0);
  state.stream.snapshot.dynamic = dynamic;
  state.stream.snapshot.fg_off = !enabled;
  state.stream.snapshot.maximum_multiplier = 0;
}

inline void ObserveNativeEvaluation(uint32_t multiplier, bool success, int64_t now_ns) {
  std::unique_lock lock(internal::state.mutex);
  internal::state.native_evaluation_ns = now_ns;
  internal::state.native_evaluation_success = success;
  internal::state.native_declared_multiplier = (success ? multiplier : 0);
  if (!success) internal::state.stream.Reset(Reason::kFAILED);
}

inline void ObserveNativeState(uint64_t viewport, bool success, uint32_t frames, int64_t now_ns) {
  std::unique_lock lock(internal::state.mutex);
  auto& state = internal::state;
  if (!state.native_enabled || !state.native_options_known) return;
  if (!success || viewport != state.native_viewport) {
    state.stream.Reset(success ? Reason::kAMBIGUOUS : Reason::kFAILED);
    state.native_state_ns = 0;
    state.native_state_application_coverage = false;
    return;
  }
  const bool application_coverage = state.native_application_identity != 0
      && now_ns >= state.native_application_ns
      && now_ns - state.native_application_ns <= Stream::STALE_NS;
  std::optional<uint64_t> applications;
  if (state.native_state_ns != 0 && state.native_state_application_coverage && application_coverage) {
    applications = state.native_applications - state.native_state_applications;
  }
  state.native_state_ns = now_ns;
  state.native_state_applications = state.native_applications;
  state.native_state_application_coverage = application_coverage;
  state.stream.Observe(Source::NATIVE_STATE, viewport, state.native_configuration,
      now_ns, frames, state.native_multiplier, 0, state.native_dynamic, 0, false, applications);
}

inline void ObserveNativePresent(uint64_t identity, bool success, int64_t now_ns) {
  std::unique_lock lock(internal::state.mutex);
  auto& state = internal::state;
  if (!state.native_enabled || !state.native_options_known) return;
  if (!success || identity == 0 || now_ns <= 0
      || (state.native_application_ns != 0 && now_ns <= state.native_application_ns)) {
    state.stream.Reset(Reason::kFAILED);
    state.native_state_ns = 0;
    state.native_state_application_coverage = false;
    return;
  }
  if (state.native_application_identity != 0 && state.native_application_identity != identity) {
    state.stream.Reset(Reason::kAMBIGUOUS);
    state.native_state_ns = 0;
    state.native_applications = state.native_state_applications = 0;
    state.native_state_application_coverage = false;
  }
  state.native_application_identity = identity;
  ++state.native_applications;
  state.native_application_ns = now_ns;
  if (state.native_state_ns != 0 && now_ns >= state.native_state_ns
      && now_ns - state.native_state_ns <= Stream::MaxAgeNs(Source::NATIVE_STATE)) return;
  if (!success || state.native_mode != 1 || state.native_multiplier < 2
      || !state.native_evaluation_success || state.native_evaluation_ns == 0
      || now_ns - state.native_evaluation_ns > Stream::STALE_NS
      || (state.native_declared_multiplier != 0
          && state.native_declared_multiplier != state.native_multiplier)) {
    state.stream.Reset(success ? Reason::kUNKNOWN_FG : Reason::kFAILED);
    return;
  }
  // This is the outer SDK application Present, never ReShade's FG callbacks.
  state.stream.Observe(Source::NATIVE_FIXED, identity, state.native_configuration,
      now_ns, state.native_multiplier, state.native_multiplier, state.native_multiplier);
}

inline void ObserveXeFG(uint64_t identity, uint64_t configuration, int64_t now_ns,
                        bool present_ok, bool status_ok, bool duplicate, uint32_t frames,
                        uint32_t requested, uint32_t observed, uint32_t maximum = 0, bool fg_off = false) {
  std::unique_lock lock(internal::state.mutex);
  auto& state = internal::state;
  state.xefg_ns = now_ns;
  if (!present_ok || !status_ok || duplicate) {
    state.stream.Reset(duplicate ? Reason::kDUPLICATE : Reason::kFAILED);
    return;
  }
  state.stream.Observe(Source::XEFG_STATUS, identity, configuration, now_ns,
      frames, requested, observed, false, maximum, fg_off);
}

inline void ObserveXeFGMetadata(uint64_t identity, uint32_t maximum, uint32_t requested = 0,
                                bool fg_off = false) {
  std::unique_lock lock(internal::state.mutex);
  auto& stream = internal::state.stream;
  if (stream.identity != 0 && stream.identity != identity) return;
  stream.snapshot.source = Source::XEFG_STATUS;
  stream.identity = identity;
  stream.snapshot.maximum_multiplier = maximum;
  stream.snapshot.requested_multiplier = requested;
  stream.snapshot.fg_off = fg_off;
}

inline void ObservePresent(uint64_t identity, uint64_t configuration, bool fg_active, int64_t now_ns) {
  std::unique_lock lock(internal::state.mutex);
  auto& state = internal::state;
  if (state.native_enabled || (state.native_evaluation_ns != 0
          && now_ns - state.native_evaluation_ns <= Stream::STALE_NS)
      || (state.xefg_ns != 0 && now_ns - state.xefg_ns <= Stream::STALE_NS) || fg_active) {
    if (state.stream.snapshot.source == Source::PRESENT || state.stream.snapshot.source == Source::NONE) {
      state.stream.Reset(Reason::kUNKNOWN_FG);
    }
    return;
  }
  if (state.present_identity != 0 && state.present_identity != identity
      && now_ns - state.present_ns <= Stream::STALE_NS) {
    state.stream.Reset(Reason::kAMBIGUOUS);
    return;
  }
  state.present_identity = identity;
  state.present_ns = now_ns;
  state.stream.Observe(Source::PRESENT, identity, configuration, now_ns, 1, 0, 0,
      false, 0, state.native_options_known && !state.native_enabled);
}

}  // namespace renodx::addons::dlss5::output_framerate
