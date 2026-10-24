/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdint>
#include <atomic>

namespace renodx::addons::dlss5::fg {

inline std::atomic_bool runtime_callback_pinned{false};
inline bool PinRuntimeCallback() noexcept {
  if (runtime_callback_pinned.load(std::memory_order_acquire)) return true;
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
          reinterpret_cast<LPCWSTR>(&runtime_callback_pinned), &module)) return false;
  runtime_callback_pinned.store(true, std::memory_order_release);
  return true;
}

enum class Result : uint8_t { NOT_SERVED, PREPARED, SUBMITTED, FAILED };

// Borrowed for one application proxy Present. Early resources are private
// copies ordered on queue; late resources retain their public tag lifetime.
// No adapter lock is held while the NR callback runs.
struct FrameInput {
  ID3D12CommandQueue* queue = nullptr;
  IDXGISwapChain3* swapchain = nullptr;
  ID3D12Resource* backbuffer = nullptr;
  ID3D12Resource* hudless = nullptr;
  ID3D12Resource* depth = nullptr;
  ID3D12Resource* motion = nullptr;
  ID3D12Resource* ui = nullptr;
  D3D12_RESOURCE_STATES backbuffer_state = D3D12_RESOURCE_STATE_PRESENT;
  D3D12_RESOURCE_STATES hudless_state = D3D12_RESOURCE_STATE_COMMON;
  D3D12_RESOURCE_STATES depth_state = D3D12_RESOURCE_STATE_COMMON;
  D3D12_RESOURCE_STATES motion_state = D3D12_RESOURCE_STATE_COMMON;
  D3D12_RESOURCE_STATES ui_state = D3D12_RESOURCE_STATE_COMMON;
  uint32_t color_x = 0;
  uint32_t color_y = 0;
  uint32_t color_width = 0;
  uint32_t color_height = 0;
  uint32_t depth_x = 0;
  uint32_t depth_y = 0;
  uint32_t motion_x = 0;
  uint32_t motion_y = 0;
  uint32_t guide_width = 0;
  uint32_t guide_height = 0;
  float motion_scale_x = 1.f;
  float motion_scale_y = 1.f;
  float jitter_x = 0.f;
  float jitter_y = 0.f;
  float frame_time_ms = 0.f;
  uintptr_t context = 0;
  uint64_t epoch = 0;
  uint64_t application_frame = 0;
  uint32_t present_id = 0;
  uint32_t init_flags = 0;
  uint32_t ui_mode = 0;
  DXGI_COLOR_SPACE_TYPE color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
  bool has_present_id = false;
  bool has_constants = false;
  bool reset = false;
  bool fg_enabled = false;
  bool ui_composition = false;
};

using Service = Result (*)(const FrameInput&) noexcept;
using PrepareCapture = bool (*)(ID3D12GraphicsCommandList*) noexcept;
using Retire = void (*)() noexcept;

}  // namespace renodx::addons::dlss5::fg
