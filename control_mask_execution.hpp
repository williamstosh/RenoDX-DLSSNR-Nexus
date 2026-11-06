/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <iterator>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include "submission_tracker.hpp"

namespace renodx::addons::dlss5::control_mask_execution {
using Microsoft::WRL::ComPtr;
using Forward = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

enum class BeginResult {
  READY,
  SAME_RECORDING,
  DISCARDED_PREVIOUS,
  PREVIOUS_UNSUBMITTED,
  INVALID_IDENTITY,
  DEVICE_MISMATCH,
  POISONED,
  HISTORY_INVALIDATED,
};

struct ExecutionResult {
  uint64_t forwarded_lists = 0;
  uint64_t forward_calls = 0;
  uint64_t protected_lists = 0;
  uint64_t ordered_lists = 0;
  uint64_t waits = 0;
  uint64_t signals = 0;
  uint64_t replays = 0;
  uint64_t recording_order_deviations = 0;
  uint64_t unsafe_lists = 0;
  uint64_t device_mismatches = 0;
  HRESULT failure = S_OK;
};

// Access these fields only through the API (execution_mutex owns all mutable state).
struct Stream {
  ComPtr<IUnknown> device_identity;
  ComPtr<ID3D12Fence> fence;
  ComPtr<IUnknown> tail_queue;
  uint64_t tail = 0;
  uint64_t next_signal = 0;
  bool poisoned = false;
  bool history_invalidated = false;
  std::shared_ptr<submission::RecordingStatus> recording;
  bool recording_submitted = false;
};

struct StreamStatus {
  bool poisoned = false;
  bool history_invalidated = false;
  uint64_t tail = 0;
  bool recording_submitted = false;
};

namespace internal {
struct Binding {
  std::shared_ptr<Stream> stream;
};
struct Recording {
  std::shared_ptr<submission::RecordingStatus> status;
  std::vector<Binding> bindings;
  uint64_t executions = 0;
};
// Lock order: execution_mutex -> tracker_mutex. Never acquire this lock from a
// tracker prune callback. Forward must be the real, non-reentrant queue entry.
inline std::shared_mutex execution_mutex;
inline std::vector<Recording> recordings;
inline std::atomic_size_t active_recordings{0};

inline ComPtr<IUnknown> DeviceIdentity(ID3D12DeviceChild* child) {
  ComPtr<ID3D12Device> device;
  ComPtr<IUnknown> identity;
  if (child != nullptr && SUCCEEDED(child->GetDevice(IID_PPV_ARGS(&device)))) {
    identity = renodx::utils::directx::NativeIdentity(device.Get());
  }
  return identity;
}

inline uint64_t StrictIdentity(ID3D12CommandList* list, bool create) {
  if (list == nullptr) return 0;
  uint64_t identity = 0;
  UINT bytes = sizeof(identity);
  if (create) {
    identity = submission::ListIdentity(list, true);
    if ((identity >> 63) != 0) return 0;
  }
  // Read back even after creation: native/proxy private data is the only join.
  if (FAILED(list->GetPrivateData(submission::kListIdentityGuid, &bytes, &identity))
      || bytes != sizeof(identity) || identity == 0 || (identity >> 63) != 0) return 0;
  return identity;
}
}  // namespace internal

inline std::shared_ptr<Stream> CreateStream(ID3D12Device* device) {
  if (device == nullptr) return nullptr;
  auto stream = std::make_shared<Stream>();
  stream->device_identity = renodx::utils::directx::NativeIdentity(device);
  if (stream->device_identity == nullptr
      || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&stream->fence)))) return nullptr;
  return stream;
}

// Caller owns one NRFeature/History per Stream and serializes CPU evaluation.
// GPU ordering cannot repair CPU ping-pong state or stale recorded Reset values.
// DISCARDED_PREVIOUS requires caller's existing History reconstruction path.
inline bool BeginRecording(const std::shared_ptr<Stream>& stream, ID3D12GraphicsCommandList* list,
                           BeginResult* result = nullptr) {
  auto finish = [result](BeginResult value, bool accepted) {
    if (result != nullptr) *result = value;
    return accepted;
  };
  if (stream == nullptr || list == nullptr) return finish(BeginResult::INVALID_IDENTITY, false);
  std::unique_lock lock(internal::execution_mutex);
  if (stream->poisoned) return finish(BeginResult::POISONED, false);
  if (stream->history_invalidated) return finish(BeginResult::HISTORY_INVALIDATED, false);
  const auto device = internal::DeviceIdentity(list);
  if (device == nullptr || device.Get() != stream->device_identity.Get()) {
    stream->poisoned = true;
    return finish(BeginResult::DEVICE_MISMATCH, false);
  }
  const auto identity = internal::StrictIdentity(list, true);
  if (identity == 0) {
    stream->poisoned = true;
    return finish(BeginResult::INVALID_IDENTITY, false);
  }
  const auto status = submission::ObserveRecording(list);
  if (status->list != identity || status->closed.load()) {
    stream->poisoned = true;
    return finish(BeginResult::INVALID_IDENTITY, false);
  }
  if (stream->recording == status) return finish(BeginResult::SAME_RECORDING, true);
  BeginResult accepted = BeginResult::READY;
  if (stream->recording != nullptr && !stream->recording_submitted) {
    if (stream->recording->submitted.load()) {
      stream->poisoned = true;
      return finish(BeginResult::POISONED, false);
    }
    if (!stream->recording->closed.load()) return finish(BeginResult::PREVIOUS_UNSUBMITTED, false);
    accepted = BeginResult::DISCARDED_PREVIOUS;
  }
  auto recording = std::ranges::find_if(internal::recordings, [&status](const auto& entry) {
    return entry.status == status;
  });
  if (recording == internal::recordings.end()) {
    internal::recordings.push_back({.status = status});
    internal::active_recordings.store(internal::recordings.size(), std::memory_order_release);
    recording = std::prev(internal::recordings.end());
  }
  // Register the owning reference before the opaque marker can reach the tracker.
  recording->bindings.push_back({.stream = stream});
  submission::TrackUse(list, stream.get());
  // A per-generation marker lets the original tracker retire this registry
  // entry while newer recordings still retain the whole Stream marker.
  submission::TrackUse(list, status.get());
  stream->recording = status;
  stream->recording_submitted = false;
  return finish(accepted, true);
}

inline StreamStatus GetStatus(const std::shared_ptr<Stream>& stream) {
  std::shared_lock lock(internal::execution_mutex);
  if (stream == nullptr) return {.poisoned = true};
  return {.poisoned = stream->poisoned, .history_invalidated = stream->history_invalidated, .tail = stream->tail,
          .recording_submitted = stream->recording_submitted};
}

// The global narrow lock makes read-tail/Wait/real Execute/Signal/publish atomic
// across queues. It never waits on the CPU or invents a future submission value.
inline ExecutionResult ExecuteOrdered(ID3D12CommandQueue* queue, UINT count,
                                      ID3D12CommandList* const* lists, Forward forward) {
  ExecutionResult result;
  if (count == 0 || lists == nullptr || internal::active_recordings.load(std::memory_order_acquire) == 0) {
    forward(queue, count, lists);
    result.forward_calls = 1;
    result.forwarded_lists = count;
    return result;
  }
  std::unique_lock lock(internal::execution_mutex);
  auto lookup = [](ID3D12CommandList* list) -> internal::Recording* {
    const auto identity = internal::StrictIdentity(list, false);
    if (identity == 0) return nullptr;
    std::lock_guard tracker_lock(submission::tracker_mutex);
    const auto tracked = submission::tracked_lists.find(identity);
    if (tracked == submission::tracked_lists.end()) return nullptr;
    for (auto& entry : internal::recordings) {
      if (entry.status->list == identity && entry.status->generation == tracked->second.current) return &entry;
    }
    return nullptr;
  };
  bool protected_batch = false;
  for (UINT index = 0; index < count; ++index) {
    if (lookup(lists[index]) != nullptr) {
      protected_batch = true;
      break;
    }
  }
  if (!protected_batch) {
    forward(queue, count, lists);
    result.forward_calls = 1;
    result.forwarded_lists = count;
    return result;
  }
  const auto device = internal::DeviceIdentity(queue);
  ComPtr<IUnknown> queue_identity;
  if (queue != nullptr) queue_identity = renodx::utils::directx::NativeIdentity(queue);
  for (UINT index = 0; index < count; ++index) {
    auto* recording = lookup(lists[index]);
    if (recording == nullptr) {
      forward(queue, 1, lists + index);
      ++result.forward_calls;
      ++result.forwarded_lists;
      continue;
    }
    ++result.protected_lists;
    if (recording->executions != 0) ++result.replays;
    bool healthy = true;
    for (auto& binding : recording->bindings) {
      auto& stream = *binding.stream;
      if (stream.recording != recording->status) {
        ++result.recording_order_deviations;
        stream.history_invalidated = true;
      }
      if (device == nullptr || device.Get() != stream.device_identity.Get() || queue_identity == nullptr
          || internal::DeviceIdentity(lists[index]).Get() != stream.device_identity.Get()) {
        healthy = false;
        ++result.device_mismatches;
        result.failure = E_INVALIDARG;
      }
      if (stream.poisoned || stream.next_signal == std::numeric_limits<uint64_t>::max()) {
        healthy = false;
        result.failure = E_FAIL;
      }
    }
    if (healthy) {
      for (auto& binding : recording->bindings) {
        auto& stream = *binding.stream;
        if (stream.tail != 0 && stream.tail_queue.Get() != queue_identity.Get()) {
          ++result.waits;
          const auto hr = queue->Wait(stream.fence.Get(), stream.tail);
          if (FAILED(hr)) {
            healthy = false;
            result.failure = hr;
            break;
          }
        }
      }
    }
    // Failure still forwards every external host list exactly once. Subsequent
    // execution is unsafe/unproven; no fallback can establish missing GPU order.
    forward(queue, 1, lists + index);
    ++result.forward_calls;
    ++result.forwarded_lists;
    ++recording->executions;
    if (healthy) {
      for (auto& binding : recording->bindings) {
        auto& stream = *binding.stream;
        ++result.signals;
        const auto hr = queue->Signal(stream.fence.Get(), ++stream.next_signal);
        if (FAILED(hr)) {
          healthy = false;
          result.failure = hr;
          break;
        }
      }
    }
    if (healthy) {
      for (auto& binding : recording->bindings) {
        auto& stream = *binding.stream;
        stream.tail = stream.next_signal;
        stream.tail_queue = queue_identity;
        if (stream.recording == recording->status) stream.recording_submitted = true;
      }
      ++result.ordered_lists;
    } else {
      for (auto& binding : recording->bindings) binding.stream->poisoned = true;
      ++result.unsafe_lists;
    }
  }
  return result;
}

inline size_t Prune() {
  std::unique_lock lock(internal::execution_mutex);
  const auto erased = std::erase_if(internal::recordings, [](const internal::Recording& recording) {
    if (!recording.status->closed.load()) return false;
    return submission::ResourceReleasable(recording.status.get());
  });
  internal::active_recordings.store(internal::recordings.size(), std::memory_order_release);
  return erased;
}

// Original tracker proof may retire a poisoned stream, but never unpoisons it.
// Does not reset the tracker or wait for GPU.
inline bool ReleaseIfIdle() {
  Prune();
  std::shared_lock lock(internal::execution_mutex);
  return internal::recordings.empty();
}
}  // namespace renodx::addons::dlss5::control_mask_execution
