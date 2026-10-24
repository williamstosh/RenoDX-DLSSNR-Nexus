/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <windows.h>
#include <dxgi1_4.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <shared_mutex>

#include <sl_dlss_g.h>

#include "detour_guard.hpp"
#include "fg_frame.hpp"
#include "lastgasp.hpp"

namespace renodx::addons::dlss5::fg::native_options {

// These phases describe the SDK's routing decision, not COM completion.
enum class PresentPhase { SDK_BEGIN, SDK_FORWARD, SDK_HANDLED, SDK_FAILED };

namespace internal {
using OptionsChanged = void (*)(bool enabled) noexcept;
using PresentObserved = void (*)(IDXGISwapChain*, UINT, PresentPhase) noexcept;
using Present = HRESULT (*)(IDXGISwapChain*, UINT, UINT, bool&);
using Present1 = HRESULT (*)(IDXGISwapChain*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*, bool&);
static std::atomic<OptionsChanged> options_changed{nullptr};
static std::atomic<PresentObserved> present_observed{nullptr};
static std::atomic_bool stopped{false};
static std::atomic_bool decisions_ready{false};
static thread_local uint32_t present_depth = 0;

struct State {
  std::shared_mutex mutex;
  PFun_slDLSSGSetOptions* real = nullptr;
  detour_guard::Record record;
  Present present = nullptr;
  Present1 present1 = nullptr;
  detour_guard::Record present_record;
  detour_guard::Record present1_record;
  bool installed = false;
};
static State state;

struct PresentDecisionScope {
  IDXGISwapChain* swapchain;
  UINT flags;
  PresentObserved callback = nullptr;

  PresentDecisionScope(IDXGISwapChain* output, UINT present_flags) : swapchain(output), flags(present_flags) {
    if (present_depth++ == 0 && !stopped.load(std::memory_order_acquire) && (flags & DXGI_PRESENT_TEST) == 0) {
      callback = present_observed.load(std::memory_order_acquire);
      if (callback != nullptr) {
        callback(swapchain, flags, PresentPhase::SDK_BEGIN);
      }
    }
  }
  void Publish(HRESULT result, bool skip) const {
    if (callback != nullptr) {
      callback(swapchain, flags, FAILED(result) ? PresentPhase::SDK_FAILED
          : (skip ? PresentPhase::SDK_HANDLED : PresentPhase::SDK_FORWARD));
    }
  }
  ~PresentDecisionScope() { --present_depth; }
};

static sl::Result HookedSetOptions(const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options) {
  if (!stopped.load(std::memory_order_acquire)) PinRuntimeCallback();
  const auto result = state.real(viewport, options);
  // Enabled modes clear an explicit Off request; actual NGX evaluation
  // remains the evidence required to acquire native processing ownership.
  if (result == sl::Result::eOk && !stopped.load(std::memory_order_acquire)) {
    if (const auto callback = options_changed.load(std::memory_order_acquire); callback != nullptr) {
      callback(options.mode != sl::DLSSGMode::eOff);
    }
  }
  return result;
}

static HRESULT HookedPresent(IDXGISwapChain* swapchain, UINT interval, UINT flags, bool& skip) {
  if (!stopped.load(std::memory_order_acquire)) PinRuntimeCallback();
  const PresentDecisionScope scope(swapchain, flags);
  const HRESULT result = state.present(swapchain, interval, flags, skip);
  scope.Publish(result, skip);
  return result;
}

static HRESULT HookedPresent1(IDXGISwapChain* swapchain, UINT interval, UINT flags,
                              const DXGI_PRESENT_PARAMETERS* parameters, bool& skip) {
  if (!stopped.load(std::memory_order_acquire)) PinRuntimeCallback();
  const PresentDecisionScope scope(swapchain, flags);
  const HRESULT result = state.present1(swapchain, interval, flags, parameters, skip);
  scope.Publish(result, skip);
  return result;
}
}  // namespace internal

inline void Configure(void (*callback)(bool enabled) noexcept,
    void (*present)(IDXGISwapChain*, UINT, PresentPhase) noexcept = nullptr) {
  internal::options_changed.store(callback, std::memory_order_release);
  internal::present_observed.store(present, std::memory_order_release);
}

inline bool ObservesPresentDecisions() noexcept {
  return internal::decisions_ready.load(std::memory_order_acquire)
      && !internal::stopped.load(std::memory_order_acquire);
}

// Terminal logical disable; it does not drain an already executing callback.
// Both images remain pinned and the installed hook continues to pass through.
inline void Stop() noexcept {
  internal::stopped.store(true, std::memory_order_release);
  internal::options_changed.store(nullptr, std::memory_order_release);
  internal::present_observed.store(nullptr, std::memory_order_release);
  internal::decisions_ready.store(false, std::memory_order_release);
  if (internal::state.installed) {
    detour_guard::Neutralize(reinterpret_cast<void* const*>(&internal::state.real),
        reinterpret_cast<void*>(&internal::HookedSetOptions), internal::state.record);
    if (internal::state.present != nullptr) {
      detour_guard::Neutralize(reinterpret_cast<void* const*>(&internal::state.present),
          reinterpret_cast<void*>(&internal::HookedPresent), internal::state.present_record);
    }
    if (internal::state.present1 != nullptr) {
      detour_guard::Neutralize(reinterpret_cast<void* const*>(&internal::state.present1),
          reinterpret_cast<void*>(&internal::HookedPresent1), internal::state.present1_record);
    }
  }
}

// Call outside DllMain and without the host's runtime lock: discovery takes
// the loader lock, and AttachAll owns the existing Detours transaction lock.
inline void Poll() {
  if (internal::stopped.load(std::memory_order_acquire)
      || internal::options_changed.load(std::memory_order_acquire) == nullptr) return;
  std::unique_lock lock(internal::state.mutex, std::try_to_lock);
  if (!lock.owns_lock() || internal::state.installed
      || internal::stopped.load(std::memory_order_acquire)) return;

  HMODULE plugin = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"sl.dlss_g.dll", &plugin)) return;
  const auto image_size = detour_guard::ImageIdentity(plugin) & 0xFFFFFFFFu;
  if (image_size == 0) return;
  using Gateway = void* (*)(const char*);
  const auto gateway = reinterpret_cast<Gateway>(GetProcAddress(plugin, "slGetPluginFunction"));
  if (gateway == nullptr
      || reinterpret_cast<uintptr_t>(gateway) - reinterpret_cast<uintptr_t>(plugin) >= image_size) return;

  // Keep SEH in a scope without C++ objects requiring unwinding (MSVC C2712).
  const auto resolve = [](Gateway entry, const char* name) -> void* {
    ++lastgasp::expected_fault_depth;
    __try {
      __try {
        return entry(name);
      } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                      ? EXCEPTION_EXECUTE_HANDLER
                      : EXCEPTION_CONTINUE_SEARCH) {
        return nullptr;
      }
    } __finally {
      --lastgasp::expected_fault_depth;
    }
  };
  void* const target = resolve(gateway, "slDLSSGSetOptions");
  if (target == nullptr
      || reinterpret_cast<uintptr_t>(target) - reinterpret_cast<uintptr_t>(plugin) >= image_size) return;

  if (internal::stopped.load(std::memory_order_acquire)) return;

  internal::state.real = reinterpret_cast<PFun_slDLSSGSetOptions*>(target);
  void* const present = resolve(gateway, "slHookPresent");
  void* const present1 = resolve(gateway, "slHookPresent1");
  const bool has_decisions = present != nullptr && present1 != nullptr
      && reinterpret_cast<uintptr_t>(present) - reinterpret_cast<uintptr_t>(plugin) < image_size
      && reinterpret_cast<uintptr_t>(present1) - reinterpret_cast<uintptr_t>(plugin) < image_size;
  if (has_decisions) {
    internal::state.present = reinterpret_cast<internal::Present>(present);
    internal::state.present1 = reinterpret_cast<internal::Present1>(present1);
  }
  const detour_guard::Hook hooks[] = {{
      .name = "slDLSSGSetOptions",
      .real = reinterpret_cast<void**>(&internal::state.real),
      .detour = reinterpret_cast<void*>(&internal::HookedSetOptions),
      .record = &internal::state.record,
  }};
  size_t failed = 0;
  internal::state.installed = detour_guard::AttachAll(hooks, &failed) == 1;
  if (internal::state.installed && has_decisions) {
    const detour_guard::Hook present_hooks[] = {
        {"slHookPresent", reinterpret_cast<void**>(&internal::state.present),
            reinterpret_cast<void*>(&internal::HookedPresent), &internal::state.present_record},
        {"slHookPresent1", reinterpret_cast<void**>(&internal::state.present1),
            reinterpret_cast<void*>(&internal::HookedPresent1), &internal::state.present1_record},
    };
    internal::decisions_ready.store(detour_guard::AttachAll(present_hooks, &failed) == 2,
                                    std::memory_order_release);
  }
}

}  // namespace renodx::addons::dlss5::fg::native_options
