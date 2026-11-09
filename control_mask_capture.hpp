/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>
#include <nlohmann/json.hpp>

#include "gpu_timers.hpp"
#include "report_bundle.hpp"
#include "stage_plan.hpp"

namespace renodx::addons::dlss5::control_mask_capture {

struct Context {
  char runtime_version[32] = {};
  uint32_t requested_model = 0;
  uint32_t stage = 1;  // stage_plan::Stage::UPSCALED
  bool mask_enabled = false;
  uint32_t mask_mode = 0;
  float mask_blend = 0;
  float mask_r = 0;
  float mask_g = 0;
  float mask_b = 0;
  bool auto_mask = false;
  float style = 0;
  float intensity = 0;
  float local_tone = 0;
  float sent_structure = 0;
  float sent_skin = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  bool source_generation_available = false;
  uint64_t source_generation = 0;
  bool api_returned = false;
  bool ngx_accepted = false;
};

enum class ExportState : uint8_t { NONE, PENDING, MARKER_WRITTEN, WRITTEN, FAILED };

struct Summary {
  uint64_t capture_id = 0;
  uint32_t target = 0;
  uint32_t capacity = 0;
  uint32_t recorded = 0;
  uint32_t valid = 0;
  uint32_t rejected = 0;
  uint32_t timeout = 0;
  uint32_t dropped = 0;
  uint32_t overflow = 0;
  uint32_t pending_stop = 0;
  uint32_t in_flight = 0;
  double window_seconds = 0;
  double warmup_seconds = 0;
  int64_t started_ns = 0;
  int64_t ended_ns = 0;
  bool window_timed_out = false;
  bool active = false;
  bool marker_written = false;
  ExportState export_state = ExportState::NONE;
  char reason[96] = {};
  char error[96] = {};
};

// Assigned before capture configuration; callback lifetime belongs to the existing screenshot worker.
inline bool (*wake_worker)() = nullptr;

namespace internal {
enum class RowState : uint8_t { PENDING, VALID, REJECTED, DROPPED, PENDING_AT_STOP };
struct Row {
  Context context;
  uint64_t token = 0;
  uint64_t timer_sequence = 0;
  int64_t recorded_ns = 0;
  RowState state = RowState::PENDING;
  gpu_timers::SampleState gpu_state = gpu_timers::SampleState::PENDING;
  gpu_timers::Stats stats;
};
struct State {
  std::shared_mutex mutex;
  Summary summary;
  std::vector<Row> rows;
  uint64_t next_token = 0;
  uint64_t next_capture = 0;
  uint64_t max_sequence = 0;
  int64_t window_ns = 0;
  int64_t warmup_ns = 0;
  bool armed = false;
  bool worker_busy = false;
  bool marker_needed = false;
  bool terminal_needed = false;
  std::filesystem::path marker_path;
  std::filesystem::path terminal_path;
};
static State state;
static std::atomic_bool enabled{false};
static std::atomic_bool capture_pending{false};
static std::atomic_bool work_pending{false};

template <size_t N>
inline void CopyText(char (&destination)[N], const char* source) {
  std::snprintf(destination, N, "%s", (source == nullptr ? "unspecified" : source));
}

// Caller owns the leaf lock. No callback, filesystem access or outer lock is acquired here.
inline void Finish(const char* reason, int64_t now_ns = gpu_timers::NowNs()) {
  auto& s = state;
  if (!s.summary.active) return;
  s.summary.active = false;
  s.summary.ended_ns = now_ns;
  s.armed = false;
  enabled.store(false, std::memory_order_relaxed);
  CopyText(s.summary.reason, reason);
  for (auto& row : s.rows) {
    if (row.state != RowState::PENDING) continue;
    row.state = RowState::PENDING_AT_STOP;
    ++s.summary.pending_stop;
  }
  s.summary.in_flight = 0;
  s.terminal_needed = true;
  work_pending.store(true, std::memory_order_release);
}

inline void Fail(const char* error) {
  auto& s = state;
  Finish("export_failed");
  s.armed = false;
  enabled.store(false, std::memory_order_relaxed);
  s.summary.export_state = ExportState::FAILED;
  CopyText(s.summary.error, error);
  s.marker_needed = false;
  s.terminal_needed = false;
  work_pending.store(false, std::memory_order_release);
  capture_pending.store(false, std::memory_order_release);
}

inline void Wake() {
  if (wake_worker == nullptr || !wake_worker()) {
    std::unique_lock lock(state.mutex);
    Fail("worker_unavailable");
  }
}

// CREATE_NEW protects both old exports and partial writes, including across processes.
inline bool WriteExclusive(const std::filesystem::path& path, const std::string& text) {
  struct File {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~File() {
      if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
      }
    }
  } file{CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
  if (file.handle == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  if (text.size() > MAXDWORD
      || !WriteFile(file.handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr)
      || written != text.size() || !FlushFileBuffers(file.handle)) return false;
  const bool closed = CloseHandle(file.handle) != FALSE;
  file.handle = INVALID_HANDLE_VALUE;
  return closed;
}
}  // namespace internal

inline bool Enabled() { return internal::enabled.load(std::memory_order_relaxed); }
inline bool CapturePending() { return internal::capture_pending.load(std::memory_order_acquire); }
inline bool WorkPending() { return internal::work_pending.load(std::memory_order_acquire); }

inline Summary GetSummary() {
  std::shared_lock lock(internal::state.mutex);
  return internal::state.summary;
}

// Busy sessions (including a failed export) cannot be overwritten by reconfiguration.
inline bool Configure(uint32_t target = 0, double seconds = 30, double warmup = 0, uint32_t capacity = 8192) {
  auto& s = internal::state;
  std::unique_lock lock(s.mutex);
  if (s.armed || s.summary.active || s.worker_busy || s.marker_needed || s.terminal_needed
      || s.summary.export_state == ExportState::FAILED) return false;
  if (!std::isfinite(seconds) || !std::isfinite(warmup) || seconds <= 0 || seconds > 3600
      || warmup < 0 || warmup >= seconds || capacity == 0 || capacity > 8192) return false;
  s.summary = Summary{
      .target = (std::min)(target, 4096u), .capacity = capacity,
      .window_seconds = seconds, .warmup_seconds = warmup};
  s.rows.clear();
  s.max_sequence = 0;
  s.marker_path.clear();
  s.terminal_path.clear();
  s.window_ns = static_cast<int64_t>(seconds * 1e9);
  s.warmup_ns = static_cast<int64_t>(warmup * 1e9);
  s.armed = target != 0;
  internal::enabled.store(s.armed, std::memory_order_relaxed);
  return true;
}

inline void Stop(const char* reason = "stopped", bool notify_worker = true) {
  bool wake = false;
  {
    auto& s = internal::state;
    std::unique_lock lock(s.mutex);
    s.armed = false;
    internal::enabled.store(false, std::memory_order_relaxed);
    wake = s.summary.active;
    internal::Finish(reason);
  }
  if (wake && notify_worker) {
    internal::Wake();
  }
}

inline void Tick(int64_t now_ns) {
  if (!Enabled()) return;
  bool wake = false;
  {
    auto& s = internal::state;
    std::unique_lock lock(s.mutex);
    if (s.summary.active && now_ns >= s.summary.started_ns
        && static_cast<uint64_t>(now_ns) - static_cast<uint64_t>(s.summary.started_ns)
            >= static_cast<uint64_t>(s.window_ns)) {
      ++s.summary.timeout;
      s.summary.window_timed_out = true;
      internal::Finish("window_timeout", now_ns);
      wake = true;
    }
  }
  if (wake) {
    internal::Wake();
  }
}

inline uint64_t Reserve(const Context& context, uint64_t timer_sequence, int64_t now_ns) {
  if (!Enabled() || (context.stage != static_cast<uint32_t>(stage_plan::Stage::UPSCALED)
                     && context.stage != static_cast<uint32_t>(stage_plan::Stage::PRESENT))) return 0;
  Tick(now_ns);
  uint64_t token = 0;
  bool wake = false;
  {
    auto& s = internal::state;
    std::unique_lock lock(s.mutex);
    if (!s.armed) return 0;
    if (!s.summary.active) {
      try {
        s.rows.reserve(s.summary.capacity);
      } catch (...) {
        internal::Fail("allocation_failed");
        return 0;
      }
      s.summary.capture_id = ++s.next_capture;
      s.summary.started_ns = now_ns;
      s.summary.active = true;
      s.summary.export_state = ExportState::PENDING;
      s.marker_needed = true;
      internal::capture_pending.store(true, std::memory_order_release);
      internal::work_pending.store(true, std::memory_order_release);
      wake = true;
    }
    const bool warmed = now_ns >= s.summary.started_ns
        && static_cast<uint64_t>(now_ns) - static_cast<uint64_t>(s.summary.started_ns)
            >= static_cast<uint64_t>(s.warmup_ns);
    if (warmed && s.rows.size() >= s.summary.capacity) {
      ++s.summary.overflow;
      internal::Finish("buffer_full", now_ns);
      wake = true;
    } else if (warmed && s.summary.valid + s.summary.in_flight < s.summary.target
               && (timer_sequence == 0 || timer_sequence > s.max_sequence)) {
      token = ++s.next_token;
      auto frozen = context;
      frozen.runtime_version[sizeof(frozen.runtime_version) - 1] = '\0';
      s.rows.push_back(internal::Row{
          .context = frozen, .token = token, .timer_sequence = timer_sequence, .recorded_ns = now_ns});
      ++s.summary.recorded;
      ++s.summary.in_flight;
      if (timer_sequence != 0) {
        s.max_sequence = timer_sequence;
      }
    }
  }
  if (wake) {
    internal::Wake();
  }
  return token;
}

inline void Observe(const gpu_timers::Stats& stats, uint64_t token) {
  if (token == 0 || stats.state == gpu_timers::SampleState::PENDING) return;
  bool wake = false;
  {
    auto& s = internal::state;
    std::unique_lock lock(s.mutex);
    if (!s.summary.active) return;
    // Tokens are contiguous within the bounded row vector; old-session tokens cannot index it.
    if (s.rows.empty() || token < s.rows.front().token || token - s.rows.front().token >= s.rows.size()) return;
    auto& row = s.rows[static_cast<size_t>(token - s.rows.front().token)];
    if (row.token != token || row.state != internal::RowState::PENDING) return;
    row.gpu_state = stats.state;
    --s.summary.in_flight;
    if (row.timer_sequence == 0) {
      row.state = internal::RowState::DROPPED;
      ++s.summary.dropped;
    } else if (stats.valid && stats.state == gpu_timers::SampleState::VALID && stats.samples == 1
               && stats.sequence == row.timer_sequence && row.context.api_returned && row.context.ngx_accepted) {
      row.state = internal::RowState::VALID;
      row.stats = stats;
      row.stats.queue = nullptr;
      ++s.summary.valid;
    } else {
      row.state = internal::RowState::REJECTED;
      ++s.summary.rejected;
      if (stats.state == gpu_timers::SampleState::EXPIRED) {
        ++s.summary.timeout;
      }
    }
    if (s.summary.valid == s.summary.target) {
      internal::Finish("target_reached");
    } else if (s.rows.size() == s.summary.capacity && s.summary.in_flight == 0) {
      ++s.summary.overflow;
      internal::Finish("buffer_full");
    }
    // Results are retained in preallocated memory until the terminal job. No per-frame disk IO.
    wake = s.terminal_needed;
  }
  if (wake) {
    internal::Wake();
  }
}

// Existing screenshot worker owns module lifetime and calls this outside its job mutex.
// The optional redaction is for synthetic CPU tests; production uses SystemRedaction().
inline void ProcessPending(const std::filesystem::path& directory, const report::Redaction* redaction = nullptr) noexcept {
  using Json = nlohmann::ordered_json;
  auto& s = internal::state;
  Summary summary;
  std::vector<internal::Row> rows;
  std::filesystem::path marker_path;
  std::filesystem::path terminal_path;
  bool marker = false;
  bool terminal = false;
  bool claimed = false;
  const char* error = "export_exception";
  try {
    {
      std::unique_lock lock(s.mutex);
      if (s.worker_busy || !WorkPending()) return;
      s.worker_busy = true;
      claimed = true;
      summary = s.summary;
      marker = std::exchange(s.marker_needed, false);
      terminal = std::exchange(s.terminal_needed, false);
      if (terminal) {
        rows = std::move(s.rows);
      }
      marker_path = s.marker_path;
      terminal_path = s.terminal_path;
      internal::work_pending.store(false, std::memory_order_release);
    }
    const auto scrub = (redaction == nullptr ? report::SystemRedaction() : *redaction);
    if (marker) {
      const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
      const auto stem = "control-mask-" + std::to_string(GetCurrentProcessId()) + "-"
          + std::to_string(stamp) + "-" + std::to_string(summary.capture_id);
      marker_path = directory / (stem + ".incomplete.json");
      terminal_path = directory / (stem + ".json");
      std::string text;
      error = "marker_redaction_failed";
      if (!report::RedactJson(Json{{"schema", "control_mask_capture_v1"}, {"status", "incomplete"},
                                  {"capture_id", summary.capture_id}, {"target", summary.target},
                                  {"started_ns", summary.started_ns}}.dump(), scrub, &text)) throw 0;
      error = "marker_write_failed_or_exists";
      std::filesystem::create_directories(directory);
      if (!internal::WriteExclusive(marker_path, text)) throw 0;
      {
        std::unique_lock lock(s.mutex);
        s.marker_path = marker_path;
        s.terminal_path = terminal_path;
        s.summary.marker_written = true;
        s.summary.export_state = ExportState::MARKER_WRITTEN;
      }
    }
    if (terminal) {
      Json events = Json::array();
      for (const auto& row : rows) {
        const auto& c = row.context;
        const char* row_state = "pending_at_stop";
        switch (row.state) {
          case internal::RowState::VALID: row_state = "valid"; break;
          case internal::RowState::REJECTED: row_state = "rejected"; break;
          case internal::RowState::DROPPED: row_state = "dropped"; break;
          case internal::RowState::PENDING: row_state = "pending"; break;
          case internal::RowState::PENDING_AT_STOP: break;
        }
        Json event{{"token", row.token}, {"timer_sequence", row.timer_sequence}, {"recorded_ns", row.recorded_ns},
                   {"status", row_state}, {"gpu_state", gpu_timers::StateText(row.gpu_state)},
                   {"context", {{"runtime_version", c.runtime_version}, {"requested_model", c.requested_model},
                                {"stage", stage_plan::Name(static_cast<stage_plan::Stage>(c.stage))},
                                {"mask_enabled", c.mask_enabled}, {"mask_mode", c.mask_mode},
                                {"mask_blend", c.mask_blend}, {"mask_rgb", {c.mask_r, c.mask_g, c.mask_b}},
                                {"auto_mask", c.auto_mask}, {"style", c.style}, {"intensity", c.intensity},
                                {"local_tone", c.local_tone}, {"sent_structure", c.sent_structure}, {"sent_skin", c.sent_skin},
                                {"width", c.width}, {"height", c.height}, {"api_returned", c.api_returned},
                                {"ngx_accepted", c.ngx_accepted}, {"source_generation_available", c.source_generation_available}}}};
        if (c.source_generation_available) {
          event["context"]["source_generation"] = c.source_generation;
        }
        if (row.state == internal::RowState::VALID) {
          const auto& t = row.stats;
          event["timing_us"] = {{"total", t.total_us}, {"own", t.own_us}, {"eval", t.eval_us},
                                {"encode", t.encode_us}, {"resolve", t.resolve_us}, {"look", t.look_us},
                                {"linearize", t.linearize_us}, {"commit", t.commit_us}, {"copies", t.copies_us}};
          event["samples"] = t.samples;
          event["measured_ns"] = t.measured_ns;
          event["passes"] = Json::array();
          for (const auto& pass : t.passes) {
            if (pass.id == UINT32_MAX) continue;
            event["passes"].push_back({{"id", pass.id}, {"requested_model", pass.model},
                                       {"width", pass.width}, {"height", pass.height},
                                       {"output_width", pass.output_width}, {"output_height", pass.output_height},
                                       {"state", gpu_timers::StateText(pass.state)}, {"encode_us", pass.encode_us},
                                       {"eval_us", pass.eval_us}, {"resolve_us", pass.resolve_us},
                                       {"look_us", pass.look_us}, {"look_measured", pass.look_measured}, {"total_us", pass.total_us}});
          }
        }
        events.push_back(std::move(event));
      }
      Json document{{"schema", "control_mask_capture_v1"}, {"status", "terminal"}, {"capture_id", summary.capture_id},
                    {"time_basis", "started_ns/recorded_ns are CPU recording times; ended_ns/measured_ns are CPU stop/observation times on steady_clock, not absolute GPU timestamps"},
                    {"eval_scope", "mask_generation+NR_Evaluate; not an independent mask duration"},
                    {"count_scope", "Disabled, warmup, duplicate-reservation and target-drain evaluates are excluded from event counts. timeout counts EXPIRED rejections plus window expiry; window_timed_out identifies the latter. Terminal means export ended, not necessarily target reached."},
                    {"summary", {{"target", summary.target}, {"actual_valid", summary.valid}, {"rejected", summary.rejected},
                                 {"timeout", summary.timeout}, {"dropped", summary.dropped}, {"overflow", summary.overflow},
                                 {"pending_stop", summary.pending_stop}, {"recorded", summary.recorded},
                                 {"capacity", summary.capacity}, {"window_seconds", summary.window_seconds},
                                 {"warmup_seconds", summary.warmup_seconds}, {"reason", summary.reason},
                                 {"started_ns", summary.started_ns}, {"ended_ns", summary.ended_ns},
                                 {"window_timed_out", summary.window_timed_out}}},
                    {"events", std::move(events)}};
      std::string text;
      error = "terminal_redaction_failed";
      if (!report::RedactJson(document.dump(), scrub, &text)) throw 0;
      error = "terminal_write_failed_or_exists";
      if (terminal_path.empty() || !internal::WriteExclusive(terminal_path, text)) throw 0;
      // The start receipt remains as provenance. Only the separate terminal file proves a completed export.
      std::unique_lock lock(s.mutex);
      s.summary.export_state = ExportState::WRITTEN;
      internal::capture_pending.store(false, std::memory_order_release);
    }
    std::unique_lock lock(s.mutex);
    s.worker_busy = false;
  } catch (...) {
    if (!claimed) return;
    std::unique_lock lock(s.mutex);
    s.worker_busy = false;
    if (terminal) {
      s.rows = std::move(rows);
    }
    internal::Fail(error);
  }
}

}  // namespace renodx::addons::dlss5::control_mask_capture
