/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <windows.h>
#include <psapi.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <type_traits>
#include <utility>
#include <vector>

#include "detour_guard.hpp"
#include "fg_frame.hpp"
#include "submission_tracker.hpp"

namespace renodx::addons::dlss5::fg::xefg {

// Public x64 XeSS-FG ABI, fixed Intel 8fe81bdb/de0fb9c1 headers. No SDK
// object layout is inspected. BACKBUFFER tags describe a region only.
namespace abi {
struct Extent { uint32_t x; uint32_t y; };
struct Resource {
  int32_t type;
  int32_t validity;
  Extent base;
  Extent size;
  ID3D12Resource* resource;
  D3D12_RESOURCE_STATES state;
};
struct Constants {
  float view[16];
  float projection[16];
  float jitter_x;
  float jitter_y;
  float motion_scale_x;
  float motion_scale_y;
  uint32_t reset;
  float frame_time_ms;
};
struct Init {
  IDXGISwapChain* application_swapchain;
  uint32_t flags;
  uint32_t max_interpolated;
  uint32_t creation_mask;
  uint32_t visible_mask;
  ID3D12Heap* buffer_heap;
  uint64_t buffer_offset;
  ID3D12Heap* texture_heap;
  uint64_t texture_offset;
  ID3D12PipelineLibrary* library;
  int32_t ui_mode;
};
struct PresentStatus {
  uint32_t frames_presented;
  int32_t frame_gen_result;
  uint32_t enabled;
};
static_assert(sizeof(Resource) == 40 && offsetof(Resource, resource) == 24);
static_assert(sizeof(Constants) == 152 && offsetof(Constants, reset) == 144);
static_assert(sizeof(Init) == 72 && offsetof(Init, ui_mode) == 64);
}  // namespace abi

namespace internal {
using Microsoft::WRL::ComPtr;
inline std::atomic<Service> service{nullptr};
inline std::atomic<PrepareCapture> prepare_capture{nullptr};
inline std::atomic<Retire> retire{nullptr};
inline std::atomic<bool (*)() noexcept> requested{nullptr};
inline std::atomic_bool block_optional_hudless{false};
inline std::atomic_bool stopped{false};
inline std::atomic_bool owns_stream{false};
inline std::atomic<Result> result{Result::NOT_SERVED};
inline std::atomic<Result> display_result{Result::NOT_SERVED};
inline std::atomic_uint64_t submitted_frames{0};
inline void PublishResult(Result value) noexcept {
  display_result.store(value, std::memory_order_release);
  result.store(value, std::memory_order_release);
}
inline thread_local uint32_t proxy_depth = 0;
// SDK Present1 may call Present even when FG does not own the stream.
inline thread_local uint32_t proxy_entry_depth = 0;

struct Surface {
  ComPtr<ID3D12Resource> texture;
  ComPtr<ID3D12Fence> consumer_fence;
  uint64_t consumer_value = 0;
  bool reserved = true;
  bool unproven = false;
};
struct TaggedResource {
  abi::Resource data{};
  ComPtr<ID3D12Resource> resource;
  std::shared_ptr<Surface> copy;
};
struct Frame {
  std::array<TaggedResource, 4> resources;
  std::vector<std::shared_ptr<Surface>> captures;
  abi::Constants constants{};
  abi::Resource region{};
  bool has_constants = false;
  bool has_region = false;
  bool rejected = false;
  bool presenting = false;
};
struct Context {
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12Fence> fence;
  uintptr_t proxy_identity = 0;
  abi::Init init{};
  DXGI_COLOR_SPACE_TYPE color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
  std::map<uint32_t, std::shared_ptr<Frame>> frames;
  std::vector<std::shared_ptr<Surface>> surfaces;
  uint64_t epoch = 1;
  uint64_t application_frame = 0;
  uint64_t last_submission_frame = 0;
  uint64_t next_fence = 0;
  uint32_t present_id = 0;
  uint32_t last_id = 0;
  bool fresh_id = false;
  bool last_id_known = false;
  bool enabled = false;
  bool ui_composition = false;
  int32_t (*get_status)(void*, abi::PresentStatus*) = nullptr;
  std::chrono::steady_clock::time_point interval_start = std::chrono::steady_clock::now();
  uint64_t interval_presents = 0;
  uint64_t interval_displayed = 0;
  uint64_t interval_submitted = 0;
  uint64_t interval_status_samples = 0;
};
struct State {
  std::shared_mutex mutex;
  std::map<void*, std::shared_ptr<Context>> contexts;
  std::vector<std::shared_ptr<Context>> retired_contexts;
};
inline State state;
inline std::shared_mutex install_mutex;
inline std::atomic<DWORD> module_bytes{0};

inline bool Requested() noexcept {
  const auto policy = requested.load(std::memory_order_acquire);
  return !stopped.load(std::memory_order_acquire) && policy != nullptr && policy();
}

inline void RefreshOwnership() {
  bool active = false;
  {
    std::shared_lock lock(state.mutex);
    for (const auto& [handle, context] : state.contexts) {
      active |= context->enabled && context->queue != nullptr && context->proxy_identity != 0;
    }
  }
  owns_stream.store(active, std::memory_order_release);
}

inline uintptr_t Identity(IUnknown* object) {
  ComPtr<IUnknown> identity;
  if (object == nullptr || FAILED(object->QueryInterface(IID_PPV_ARGS(&identity)))) return 0;
  return reinterpret_cast<uintptr_t>(identity.Get());
}

inline std::shared_ptr<Frame> GetFrame(Context* context, uint32_t id) {
  if (const auto it = context->frames.find(id); it != context->frames.end()) {
    return it->second->presenting ? nullptr : it->second;
  }
  // A caller may tag ahead. Keep the admission bounded without discarding a
  // frame whose SDK still holds one of our substituted resources.
  if (context->frames.size() >= 16) return nullptr;
  auto frame = std::make_shared<Frame>();
  context->frames.emplace(id, frame);
  return frame;
}

inline void ReleaseFrames(Context* context) {
  for (const auto& [id, frame] : context->frames) {
    for (const auto& resource : frame->resources) {
      if (resource.copy != nullptr) resource.copy->unproven = true;
    }
  }
  context->frames.clear();
  context->fresh_id = false;
  context->last_id_known = false;
  context->last_submission_frame = 0;
  ++context->epoch;
}

inline std::shared_ptr<Surface> AcquireSurface(const std::shared_ptr<Context>& context,
                                              const D3D12_RESOURCE_DESC& description) {
  std::shared_ptr<Surface> surface;
  {
    std::unique_lock lock(state.mutex);
    for (const auto& candidate : context->surfaces) {
      if (candidate->reserved || candidate->unproven
          || !submission::ResourceReleasable(candidate->texture.Get())) continue;
      if (candidate->consumer_fence != nullptr
          && candidate->consumer_fence->GetCompletedValue() < candidate->consumer_value) continue;
      const auto have = candidate->texture->GetDesc();
      if (have.Width == description.Width && have.Height == description.Height
          && have.Format == description.Format) {
        surface = candidate;
        surface->reserved = true;
        break;
      }
    }
    if (surface == nullptr && context->surfaces.size() >= 64) return nullptr;
  }
  if (surface != nullptr) return surface;
  D3D12_RESOURCE_DESC copy = description;
  copy.Alignment = 0;
  copy.MipLevels = 1;
  copy.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  copy.Flags &= ~(D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  surface = std::make_shared<Surface>();
  if (FAILED(context->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &copy,
          D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&surface->texture)))) return nullptr;
  surface->texture->SetName(L"DLSS5 FG tagged input");
  {
    std::unique_lock lock(state.mutex);
    context->surfaces.push_back(surface);
  }
  return surface;
}

inline bool OrderCopies(const std::shared_ptr<Frame>& frame, ID3D12CommandQueue* queue) {
  if (frame == nullptr) return true;
  std::vector<std::pair<FenceRef, uint64_t>> waits;
  bool ordered = true;
  for (const auto& resource : frame->resources) {
    if (resource.copy == nullptr) continue;
    if (submission::UseSubmissionTokens(resource.resource.Get(), &waits)
        == submission::UseSubmission::kUnsubmitted) {
      ordered = false;
      continue;
    }
    for (const auto& [fence, value] : waits) {
      if (FAILED(queue->Wait(fence.fence, value))) ordered = false;
    }
  }
  return ordered;
}

inline void Initialize(void* handle, ID3D12CommandQueue* queue, const abi::Init* parameters,
                        HMODULE sdk_module = nullptr) {
  if (handle == nullptr || queue == nullptr || parameters == nullptr) return;
  auto context = std::make_shared<Context>();
  context->queue = queue;
  context->init = *parameters;
  if (sdk_module != nullptr) {
    context->get_status = reinterpret_cast<int32_t (*)(void*, abi::PresentStatus*)>(
        GetProcAddress(sdk_module, "xefgSwapChainGetLastPresentStatus"));
  }
  if (FAILED(queue->GetDevice(IID_PPV_ARGS(&context->device)))
      || FAILED(context->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&context->fence)))) return;
  std::unique_lock lock(state.mutex);
  if (const auto it = state.contexts.find(handle); it != state.contexts.end()) {
    state.retired_contexts.push_back(it->second);
  }
  state.contexts[handle] = std::move(context);
}

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using Resize1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT,
                                              const UINT*, IUnknown* const*);
using ColorFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE);
struct ProxySlot {
  std::array<void*, 5> real{};
  std::array<detour_guard::Record, 5> records;
  bool installed = false;
  bool complete = false;
};
inline std::array<ProxySlot, 4> proxies;

struct PresentFrame {
  std::shared_ptr<Context> context;
  std::shared_ptr<Frame> frame;
  std::shared_ptr<Frame> pending_frame;
  ComPtr<IDXGISwapChain3> swapchain;
  ComPtr<ID3D12Resource> backbuffer;
  FrameInput input;
  bool service_owned = false;
  bool continuing_submission = false;
  Result service_result = Result::NOT_SERVED;
};

inline PresentFrame BeginPresent(IDXGISwapChain* swapchain, UINT flags) {
  PresentFrame present;
  if ((flags & DXGI_PRESENT_TEST) != 0 || proxy_depth != 0) return present;
  const bool service_requested = Requested();
  const auto identity = Identity(swapchain);
  {
    std::unique_lock lock(state.mutex);
    for (const auto& [handle, context] : state.contexts) {
      if (context->proxy_identity != identity) continue;
      present.context = context;
      present.service_owned = service_requested && context->enabled;
      present.input = {.queue = context->queue.Get(), .context = reinterpret_cast<uintptr_t>(handle),
          .epoch = context->epoch, .application_frame = ++context->application_frame,
          .present_id = context->present_id, .init_flags = context->init.flags,
          .ui_mode = static_cast<uint32_t>(context->init.ui_mode), .color_space = context->color_space,
          .has_present_id = context->fresh_id, .fg_enabled = context->enabled,
          .ui_composition = context->ui_composition};
      present.continuing_submission = context->last_submission_frame != 0
          && context->last_submission_frame + 1 == present.input.application_frame;
      if (const auto it = context->frames.find(context->present_id); context->fresh_id && it != context->frames.end()) {
        if (!it->second->presenting) {
          it->second->presenting = true;
          present.pending_frame = it->second;
          present.frame = std::make_shared<Frame>(*it->second);
        }
      }
      context->fresh_id = false;
      break;
    }
  }
  if (present.context == nullptr) return present;
  // A previously substituted HUDLess remains an SDK input even if NR was
  // switched off or another input declined after its tag.
  const bool copies_ordered = OrderCopies(present.frame, present.context->queue.Get());
  if (!present.service_owned) return present;
  // A frame phase is not a service outage. Keep an adjacent proven service
  // active only while its next frame records, never after a declined return.
  display_result.store(present.continuing_submission ? Result::SUBMITTED : Result::PREPARED,
                       std::memory_order_release);
  result.store(Result::PREPARED, std::memory_order_release);
  const auto publish_result = [&](PresentFrame*) noexcept {
    PublishResult(present.service_result);
  };
  const std::unique_ptr<PresentFrame, decltype(publish_result)> completion(&present, publish_result);
  if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&present.swapchain)))
      || FAILED(present.swapchain->GetBuffer(present.swapchain->GetCurrentBackBufferIndex(),
                                            IID_PPV_ARGS(&present.backbuffer)))) return present;
  present.input.swapchain = present.swapchain.Get();
  present.input.backbuffer = present.backbuffer.Get();
  const auto description = present.backbuffer->GetDesc();
  present.input.color_width = static_cast<uint32_t>(description.Width);
  present.input.color_height = description.Height;
  if (present.frame != nullptr) {
    const auto& frame = *present.frame;
    if (frame.rejected) return present;
    if (frame.has_region) {
      present.input.color_x = frame.region.base.x;
      present.input.color_y = frame.region.base.y;
      present.input.color_width = frame.region.size.x;
      present.input.color_height = frame.region.size.y;
    }
    const auto& hudless = frame.resources[0];
    const auto& depth = frame.resources[1];
    const auto& motion = frame.resources[2];
    const auto& ui = frame.resources[3];
    // Only explicitly active NONE/BACKBUFFER_UITEXTURE modes document that
    // HUDLess is not used. With composition disabled, optional HUDLess tags
    // can still affect interpolation; preserve consistent NR inputs there.
    present.input.hudless = (present.input.ui_composition
        && (present.input.ui_mode == 1 || present.input.ui_mode == 2) ? nullptr : hudless.resource.Get());
    present.input.hudless_state = hudless.data.state;
    present.input.ui = ui.resource.Get();
    present.input.ui_state = ui.data.state;
    if (present.input.has_present_id && depth.resource && motion.resource && depth.data.size.x == motion.data.size.x
        && depth.data.size.y == motion.data.size.y && frame.has_constants) {
      present.input.depth = depth.resource.Get();
      present.input.depth_state = depth.data.state;
      present.input.motion = motion.resource.Get();
      present.input.motion_state = motion.data.state;
      present.input.depth_x = depth.data.base.x;
      present.input.depth_y = depth.data.base.y;
      present.input.motion_x = motion.data.base.x;
      present.input.motion_y = motion.data.base.y;
      present.input.guide_width = motion.data.size.x;
      present.input.guide_height = motion.data.size.y;
    }
    present.input.has_constants = frame.has_constants;
    present.input.motion_scale_x = frame.constants.motion_scale_x;
    present.input.motion_scale_y = frame.constants.motion_scale_y;
    present.input.jitter_x = frame.constants.jitter_x;
    present.input.jitter_y = frame.constants.jitter_y;
    present.input.frame_time_ms = frame.constants.frame_time_ms;
    present.input.reset = frame.constants.reset != 0;
  }
  bool repeated = false;
  {
    std::unique_lock lock(state.mutex);
    repeated = present.input.has_present_id && present.context->last_id_known
        && present.context->last_id == present.input.present_id;
    if (present.input.has_present_id && !repeated) {
      present.context->last_id = present.input.present_id;
      present.context->last_id_known = true;
    }
  }
  const auto callback = service.load(std::memory_order_acquire);
  // The outer proxy call identifies a new application Present independently
  // of the optional FG input ID. Untagged menus/CG use only the current BB;
  // no previous frame's guides, constants or HUDLess are borrowed.
  if (!repeated && callback != nullptr && copies_ordered) {
    present.service_result = callback(present.input);
  }
  return present;
}

inline void EndPresent(PresentFrame* present) {
  if (present->context == nullptr) return;
  uint64_t value = 0;
  {
    std::unique_lock lock(state.mutex);
    value = ++present->context->next_fence;
  }
  // Intel permits tagged input reuse after matching proxy Present. This
  // marker orders our copies behind that public consumption boundary.
  const bool signaled = SUCCEEDED(present->context->queue->Signal(present->context->fence.Get(), value));
  abi::PresentStatus status{};
  const bool has_status = present->context->get_status != nullptr
      && present->context->get_status(reinterpret_cast<void*>(present->input.context), &status) == 0;
  std::unique_lock lock(state.mutex);
  if (present->frame != nullptr) {
    for (const auto& copy : present->frame->captures) {
      copy->consumer_fence = present->context->fence;
      copy->consumer_value = value;
      copy->unproven = !signaled;
      copy->reserved = false;
    }
    if (const auto it = present->context->frames.find(present->input.present_id);
        it != present->context->frames.end() && it->second == present->pending_frame) {
      present->context->frames.erase(it);
    }
  }
  auto& context = *present->context;
  ++context.interval_presents;
  context.interval_submitted += present->service_result == Result::SUBMITTED;
  if (present->service_result == Result::SUBMITTED) {
    submitted_frames.fetch_add(1, std::memory_order_relaxed);
    if (context.epoch == present->input.epoch) context.last_submission_frame = present->input.application_frame;
  }
  if (has_status) {
    context.interval_displayed += status.frames_presented;
    ++context.interval_status_samples;
  }
  const auto now = std::chrono::steady_clock::now();
  const double seconds = std::chrono::duration<double>(now - context.interval_start).count();
  if (seconds >= 5.0) {
    char message[512];
    std::snprintf(message, sizeof(message),
        "DLSS5 FG interval context=%llx seconds=%.3f application_presents=%llu nr_submitted=%llu "
        "status_samples=%llu displayed_frames=%llu application_fps=%.2f displayed_fps=%.2f last_result=%u; "
        "ui_mode=%u ui_composition=%u nr_colors=%u sdk_fg_enabled=%u fresh_present_id=%u; "
        "public_SDK_status_not_visual_acceptance",
        static_cast<unsigned long long>(present->input.context), seconds,
        static_cast<unsigned long long>(context.interval_presents),
        static_cast<unsigned long long>(context.interval_submitted),
        static_cast<unsigned long long>(context.interval_status_samples),
        static_cast<unsigned long long>(context.interval_displayed), context.interval_presents / seconds,
        context.interval_displayed / seconds, static_cast<unsigned>(present->service_result), present->input.ui_mode,
        static_cast<unsigned>(present->input.ui_composition), present->input.hudless != nullptr ? 2u : 1u,
        has_status ? status.enabled : 0u, static_cast<unsigned>(present->input.has_present_id));
    context.interval_start = now;
    context.interval_presents = context.interval_displayed = context.interval_submitted = context.interval_status_samples = 0;
    lock.unlock();
    reshade::log::message(reshade::log::level::info, message);
  }
}

inline void BeforeResize(IDXGISwapChain* swapchain) {
  const auto identity = Identity(swapchain);
  const auto callback = retire.load(std::memory_order_acquire);
  if (callback != nullptr) callback();
  std::unique_lock lock(state.mutex);
  for (const auto& [handle, context] : state.contexts) {
    if (context->proxy_identity == identity) ReleaseFrames(context.get());
  }
  PublishResult(Result::NOT_SERVED);
}

template <size_t Index>
struct ProxyHooks {
  static HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain* swapchain, UINT interval, UINT flags) {
    if (proxy_entry_depth != 0) {
      return reinterpret_cast<PresentFn>(proxies[Index].real[0])(swapchain, interval, flags);
    }
    ++proxy_entry_depth;
    PresentFrame present;
    try { present = BeginPresent(swapchain, flags); } catch (...) { PublishResult(Result::FAILED); }
    if (present.service_owned) ++proxy_depth;
    const HRESULT status = reinterpret_cast<PresentFn>(proxies[Index].real[0])(swapchain, interval, flags);
    if (present.service_owned) --proxy_depth;
    try { EndPresent(&present); } catch (...) { PublishResult(Result::FAILED); }
    --proxy_entry_depth;
    return status;
  }
  static HRESULT STDMETHODCALLTYPE Present1(IDXGISwapChain1* swapchain, UINT interval, UINT flags,
                                            const DXGI_PRESENT_PARAMETERS* parameters) {
    if (proxy_entry_depth != 0) {
      return reinterpret_cast<Present1Fn>(proxies[Index].real[1])(swapchain, interval, flags, parameters);
    }
    ++proxy_entry_depth;
    PresentFrame present;
    try { present = BeginPresent(swapchain, flags); } catch (...) { PublishResult(Result::FAILED); }
    if (present.service_owned) ++proxy_depth;
    const HRESULT status = reinterpret_cast<Present1Fn>(proxies[Index].real[1])(swapchain, interval, flags, parameters);
    if (present.service_owned) --proxy_depth;
    try { EndPresent(&present); } catch (...) { PublishResult(Result::FAILED); }
    --proxy_entry_depth;
    return status;
  }
  static HRESULT STDMETHODCALLTYPE Resize(IDXGISwapChain* swapchain, UINT count, UINT width, UINT height,
                                          DXGI_FORMAT format, UINT flags) {
    try { BeforeResize(swapchain); } catch (...) { PublishResult(Result::FAILED); }
    return reinterpret_cast<ResizeFn>(proxies[Index].real[2])(swapchain, count, width, height, format, flags);
  }
  static HRESULT STDMETHODCALLTYPE Resize1(IDXGISwapChain3* swapchain, UINT count, UINT width, UINT height,
                                           DXGI_FORMAT format, UINT flags, const UINT* masks, IUnknown* const* queues) {
    try { BeforeResize(swapchain); } catch (...) { PublishResult(Result::FAILED); }
    return reinterpret_cast<Resize1Fn>(proxies[Index].real[3])(swapchain, count, width, height, format, flags, masks, queues);
  }
  static HRESULT STDMETHODCALLTYPE Color(IDXGISwapChain3* swapchain, DXGI_COLOR_SPACE_TYPE color) {
    const auto status = reinterpret_cast<ColorFn>(proxies[Index].real[4])(swapchain, color);
    if (SUCCEEDED(status)) {
      const auto identity = Identity(swapchain);
      std::unique_lock lock(state.mutex);
      for (const auto& [handle, context] : state.contexts) {
        if (context->proxy_identity == identity) {
          if (context->color_space != color) ReleaseFrames(context.get());
          context->color_space = color;
        }
      }
    }
    return status;
  }
};

template <size_t Index>
inline std::array<void*, 5> ProxyDetours() {
  return {reinterpret_cast<void*>(&ProxyHooks<Index>::Present), reinterpret_cast<void*>(&ProxyHooks<Index>::Present1),
      reinterpret_cast<void*>(&ProxyHooks<Index>::Resize), reinterpret_cast<void*>(&ProxyHooks<Index>::Resize1),
      reinterpret_cast<void*>(&ProxyHooks<Index>::Color)};
}

inline bool InstallProxy(IUnknown* object, HMODULE sdk_module) {
  ComPtr<IDXGISwapChain3> proxy;
  if (object == nullptr || FAILED(object->QueryInterface(IID_PPV_ARGS(&proxy)))) return false;
  auto** table = *reinterpret_cast<void***>(proxy.Get());
  const std::array<void*, 5> targets{table[8], table[22], table[13], table[39], table[38]};
  std::unique_lock install(install_mutex);
  for (size_t index = 0; index < proxies.size(); ++index) {
    auto& slot = proxies[index];
    if (slot.installed) {
      if (slot.records[0].target == targets[0]) return slot.complete;
      continue;
    }
    // Install inside OptiScaler's later FGPresent detour. Attaching for the
    // first time over an existing detour would run before its final submits.
    // Public COM method code may have a linker thunk within the SDK image;
    // a jump outside that image cannot establish this boundary.
    for (size_t method : {size_t{0}, size_t{1}}) {
      auto* code = static_cast<uint8_t*>(targets[method]);
      for (size_t hop = 0; hop < 4; ++hop) {
        uint8_t bytes[6] = {};
        if (!detour_guard::ReadCode(code, bytes, sizeof(bytes))) return false;
        HMODULE owner = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(code), &owner) || owner != sdk_module) return false;
        if (bytes[0] == 0xe9) {
          int32_t displacement = 0;
          std::memcpy(&displacement, bytes + 1, sizeof(displacement));
          code += 5 + displacement;
        } else if (bytes[0] == 0xeb) {
          code += 2 + static_cast<int8_t>(bytes[1]);
        } else if (bytes[0] == 0xff && bytes[1] == 0x25) {
          return false;
        } else {
          break;
        }
        if (hop == 3) return false;
      }
    }
    slot.real = targets;
    const auto detours = (index == 0 ? ProxyDetours<0>() : index == 1 ? ProxyDetours<1>()
        : index == 2 ? ProxyDetours<2>() : ProxyDetours<3>());
    std::array<detour_guard::Hook, 5> hooks;
    for (size_t i = 0; i < hooks.size(); ++i) hooks[i] = {
        .name = "XeFG proxy", .real = &slot.real[i], .detour = detours[i], .record = &slot.records[i]};
    size_t failed = 0;
    const auto attached = detour_guard::AttachAll(hooks, &failed);
    slot.installed = attached != 0;
    slot.complete = attached == hooks.size();
    return slot.complete;
  }
  return false;
}

inline constexpr std::array<const char*, 9> EXPORTS{
    "xefgSwapChainD3D12InitFromSwapChain", "xefgSwapChainD3D12InitFromSwapChainDesc",
    "xefgSwapChainD3D12GetSwapChainPtr", "xefgSwapChainD3D12TagFrameResource", "xefgSwapChainTagFrameConstants",
    "xefgSwapChainSetPresentId", "xefgSwapChainSetEnabled", "xefgSwapChainSetUiCompositionState", "xefgSwapChainDestroy"};
struct SdkSlot {
  HMODULE module = nullptr;
  std::array<void*, EXPORTS.size()> real{};
  std::array<detour_guard::Record, EXPORTS.size()> records;
  bool installed = false;
  bool complete = false;
};
inline std::array<SdkSlot, 2> modules;

template <size_t Index>
struct SdkHooks {
  static int32_t Init(void* handle, ID3D12CommandQueue* queue, const abi::Init* parameters) {
    if (!stopped.load()) PinRuntimeCallback();
    const auto status = reinterpret_cast<int32_t (*)(void*, ID3D12CommandQueue*, const abi::Init*)>(modules[Index].real[0])(
        handle, queue, parameters);
    if (status == 0 && modules[Index].complete && !stopped.load()) {
      try {
        Initialize(handle, queue, parameters, modules[Index].module);
      } catch (...) { PublishResult(Result::FAILED); }
    }
    return status;
  }
  static int32_t InitDesc(void* handle, HWND window, const DXGI_SWAP_CHAIN_DESC1* description,
                          const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, ID3D12CommandQueue* queue,
                          IDXGIFactory2* factory, const abi::Init* parameters) {
    if (!stopped.load()) PinRuntimeCallback();
    const auto status = reinterpret_cast<int32_t (*)(void*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, ID3D12CommandQueue*, IDXGIFactory2*, const abi::Init*)>(modules[Index].real[1])(
        handle, window, description, fullscreen, queue, factory, parameters);
    if (status == 0 && modules[Index].complete && !stopped.load()) {
      try {
        Initialize(handle, queue, parameters, modules[Index].module);
      } catch (...) { PublishResult(Result::FAILED); }
    }
    return status;
  }
  static int32_t GetSwapchain(void* handle, REFIID iid, void** output) {
    if (!stopped.load()) PinRuntimeCallback();
    const auto status = reinterpret_cast<int32_t (*)(void*, REFIID, void**)>(modules[Index].real[2])(handle, iid, output);
    if (status == 0 && output != nullptr && *output != nullptr && !stopped.load()) {
      try {
        if (!InstallProxy(static_cast<IUnknown*>(*output), modules[Index].module)) {
          reshade::log::message(reshade::log::level::warning,
              "DLSS5 FG: SDK proxy boundary unavailable; frame remains unserved");
          return status;
        }
        const auto identity = Identity(static_cast<IUnknown*>(*output));
        {
          std::unique_lock lock(state.mutex);
          if (const auto it = state.contexts.find(handle); it != state.contexts.end()) it->second->proxy_identity = identity;
        }
        RefreshOwnership();
      } catch (...) { PublishResult(Result::FAILED); }
    }
    return status;
  }
  static int32_t Resource(void* handle, ID3D12CommandList* list, uint32_t id, const abi::Resource* data) {
    if (!stopped.load()) PinRuntimeCallback();
    std::shared_ptr<Context> context;
    std::shared_ptr<Frame> frame;
    TaggedResource tagged;
    abi::Resource replacement{};
    const abi::Resource* forwarded = data;
    try {
      // This explicit policy omits the optional input at the SDK boundary;
      // merely skipping its NR would leave FG consuming inconsistent colors.
      // Keep the selected FG contract while NR is Off so a hotkey cannot
      // introduce a second, unprocessed color within the same frame.
      if (!stopped.load(std::memory_order_acquire)
          && block_optional_hudless.load(std::memory_order_acquire)
          && data != nullptr && data->type == 0 && data->resource != nullptr
          && (data->validity == 0 || (data->validity == 1 && list != nullptr))) {
        std::shared_lock lock(state.mutex);
        if (const auto it = state.contexts.find(handle); it != state.contexts.end()
            && it->second->enabled && !it->second->ui_composition) {
          const auto description = data->resource->GetDesc();
          const auto tagged_frame = it->second->frames.find(id);
          const bool already_forwarded = tagged_frame != it->second->frames.end()
              && tagged_frame->second->resources[0].resource != nullptr;
          if (!already_forwarded && description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D
              && description.DepthOrArraySize == 1 && description.SampleDesc.Count == 1
              && data->size.x != 0 && data->size.y != 0
              && uint64_t{data->base.x} + data->size.x <= description.Width
              && uint64_t{data->base.y} + data->size.y <= description.Height) return 0;
        }
      }
      if (Requested() && data != nullptr) {
        {
          std::unique_lock lock(state.mutex);
          if (const auto it = state.contexts.find(handle); it != state.contexts.end()) {
            context = it->second;
            frame = GetFrame(context.get(), id);
          }
        }
        if (frame != nullptr && data->type >= 0 && data->type < 4 && data->resource != nullptr
            && (data->validity == 0 || data->validity == 1)) {
          tagged.data = *data;
          tagged.resource = data->resource;
          const auto description = data->resource->GetDesc();
          if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || description.DepthOrArraySize != 1
              || description.SampleDesc.Count != 1 || data->size.x == 0 || data->size.y == 0
              || uint64_t{data->base.x} + data->size.x > description.Width
              || uint64_t{data->base.y} + data->size.y > description.Height) {
            tagged.resource.Reset();
          } else if (data->validity == 1) {
            ComPtr<ID3D12GraphicsCommandList> graphics;
            const auto prepare = prepare_capture.load(std::memory_order_acquire);
            if (list != nullptr && SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&graphics)))
                && prepare != nullptr && prepare(graphics.Get())) {
              tagged.copy = AcquireSurface(context, description);
            }
            if (tagged.copy != nullptr) {
              {
                std::unique_lock lock(state.mutex);
                frame->captures.push_back(tagged.copy);
              }
              // Retention is registered before recording. If a later SDK tag
              // fails, these GPU operands still belong to the original list.
              submission::TrackUse(graphics.Get(), tagged.copy->texture.Get());
              std::array<D3D12_RESOURCE_BARRIER, 2> barriers{};
              for (auto& barrier : barriers) {
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.Subresource = 0;
              }
              barriers[0].Transition.pResource = data->resource;
              barriers[0].Transition.StateBefore = data->state;
              barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
              barriers[1].Transition.pResource = tagged.copy->texture.Get();
              barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
              barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
              if (data->state != D3D12_RESOURCE_STATE_COPY_SOURCE) graphics->ResourceBarrier(1, &barriers[0]);
              graphics->ResourceBarrier(1, &barriers[1]);
              D3D12_TEXTURE_COPY_LOCATION source{};
              source.pResource = data->resource;
              source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
              D3D12_TEXTURE_COPY_LOCATION destination{};
              destination.pResource = tagged.copy->texture.Get();
              destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
              graphics->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
              for (auto& barrier : barriers) std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
              if (data->state != D3D12_RESOURCE_STATE_COPY_SOURCE) graphics->ResourceBarrier(1, &barriers[0]);
              graphics->ResourceBarrier(1, &barriers[1]);
              tagged.resource = tagged.copy->texture;
              tagged.data.resource = tagged.resource.Get();
              tagged.data.state = D3D12_RESOURCE_STATE_COMMON;
              tagged.data.validity = 0;
              if (data->type == 0) {
                // The SDK must consume the owned color after late NR, rather
                // than its earlier private ONLY_NOW copy of unprocessed color.
                replacement = tagged.data;
                forwarded = &replacement;
              }
            } else {
              tagged.resource.Reset();
            }
          }
        }
      }
    } catch (...) { tagged.resource.Reset(); PublishResult(Result::FAILED); }
    const auto status = reinterpret_cast<int32_t (*)(void*, ID3D12CommandList*, uint32_t, const abi::Resource*)>(
        modules[Index].real[3])(handle, list, id, forwarded);
    try {
      if (frame != nullptr && data != nullptr) {
        std::unique_lock lock(state.mutex);
        if (data->type == 4 && status == 0) {
          frame->region = *data;
          frame->has_region = true;
        } else if (data->type >= 0 && data->type < 4) {
          if (status == 0 && tagged.resource != nullptr) {
            frame->resources[data->type] = std::move(tagged);
          } else {
            frame->rejected = true;
          }
        }
        if (tagged.copy != nullptr) tagged.copy->reserved = false;
      }
    } catch (...) { PublishResult(Result::FAILED); }
    return status;
  }
  static int32_t Constants(void* handle, uint32_t id, const abi::Constants* data) {
    if (!stopped.load()) PinRuntimeCallback();
    const auto status = reinterpret_cast<int32_t (*)(void*, uint32_t, const abi::Constants*)>(modules[Index].real[4])(
        handle, id, data);
    if (Requested() && data != nullptr) {
      try {
        std::unique_lock lock(state.mutex);
        if (const auto it = state.contexts.find(handle); it != state.contexts.end()) {
          if (const auto frame = GetFrame(it->second.get(), id)) {
            frame->has_constants = status == 0;
            if (status == 0) frame->constants = *data;
          }
        }
      } catch (...) { PublishResult(Result::FAILED); }
    }
    return status;
  }
  static int32_t PresentId(void* handle, uint32_t id) {
    if (!stopped.load()) PinRuntimeCallback();
    const auto status = reinterpret_cast<int32_t (*)(void*, uint32_t)>(modules[Index].real[5])(handle, id);
    if (status == 0) {
      std::unique_lock lock(state.mutex);
      if (const auto it = state.contexts.find(handle); it != state.contexts.end()) {
        it->second->present_id = id;
        it->second->fresh_id = true;
      }
    }
    return status;
  }
  static int32_t Enable(void* handle, uint32_t enabled) {
    if (!stopped.load()) PinRuntimeCallback();
    const auto status = reinterpret_cast<int32_t (*)(void*, uint32_t)>(modules[Index].real[6])(handle, enabled);
    if (status == 0) {
      {
        std::unique_lock lock(state.mutex);
        if (const auto it = state.contexts.find(handle); it != state.contexts.end()) {
          if (it->second->enabled != (enabled != 0)) ReleaseFrames(it->second.get());
          it->second->enabled = enabled != 0;
        }
      }
      RefreshOwnership();
    }
    return status;
  }
  static int32_t UiState(void* handle, int32_t composition) {
    if (!stopped.load()) PinRuntimeCallback();
    const auto status = reinterpret_cast<int32_t (*)(void*, int32_t)>(modules[Index].real[7])(handle, composition);
    if (status == 0) {
      std::unique_lock lock(state.mutex);
      if (const auto it = state.contexts.find(handle); it != state.contexts.end()) {
        if (it->second->ui_composition != (composition != 0)) ReleaseFrames(it->second.get());
        it->second->ui_composition = composition != 0;
      }
    }
    return status;
  }
  static int32_t Destroy(void* handle) {
    if (!stopped.load()) PinRuntimeCallback();
    const auto status = reinterpret_cast<int32_t (*)(void*)>(modules[Index].real[8])(handle);
    if (status == 0) {
      std::unique_lock lock(state.mutex);
      if (const auto it = state.contexts.find(handle); it != state.contexts.end()) {
        ReleaseFrames(it->second.get());
        // Keep recordings that can still replay alive; submission owns only
        // tokens, never COM references to their operands.
        for (auto& surface : it->second->surfaces) surface->unproven |= !submission::ResourceReleasable(surface->texture.Get());
        it->second->enabled = false;
        it->second->proxy_identity = 0;
        state.retired_contexts.push_back(it->second);
        state.contexts.erase(it);
      }
      lock.unlock();
      RefreshOwnership();
    }
    return status;
  }
};

template <size_t Index>
inline std::array<void*, EXPORTS.size()> SdkDetours() {
  return {reinterpret_cast<void*>(&SdkHooks<Index>::Init), reinterpret_cast<void*>(&SdkHooks<Index>::InitDesc),
      reinterpret_cast<void*>(&SdkHooks<Index>::GetSwapchain), reinterpret_cast<void*>(&SdkHooks<Index>::Resource),
      reinterpret_cast<void*>(&SdkHooks<Index>::Constants), reinterpret_cast<void*>(&SdkHooks<Index>::PresentId),
      reinterpret_cast<void*>(&SdkHooks<Index>::Enable), reinterpret_cast<void*>(&SdkHooks<Index>::UiState),
      reinterpret_cast<void*>(&SdkHooks<Index>::Destroy)};
}
}  // namespace internal

inline void Configure(Service service, PrepareCapture capture, Retire retire, bool (*requested)() noexcept,
                      bool block_optional_hudless = false) {
  internal::block_optional_hudless.store(block_optional_hudless, std::memory_order_release);
  internal::service.store(service, std::memory_order_release);
  internal::prepare_capture.store(capture, std::memory_order_release);
  internal::retire.store(retire, std::memory_order_release);
  internal::requested.store(requested, std::memory_order_release);
}

// Public SDK enable state, independent of whether NR requested ownership.
inline bool Enabled() noexcept {
  return internal::owns_stream.load(std::memory_order_acquire);
}

inline bool OwnsStream() noexcept {
  return internal::Requested() && (internal::proxy_depth != 0 || internal::owns_stream.load(std::memory_order_acquire));
}

inline Result CurrentResult() noexcept {
  return OwnsStream() ? internal::result.load(std::memory_order_acquire) : Result::NOT_SERVED;
}

// UI service state only: CurrentResult remains the exact current handoff.
inline Result DisplayResult() noexcept {
  return OwnsStream() ? internal::display_result.load(std::memory_order_acquire) : Result::NOT_SERVED;
}

inline uint64_t SubmittedFrames() noexcept {
  return internal::submitted_frames.load(std::memory_order_relaxed);
}

inline void Stop() noexcept {
  internal::stopped.store(true, std::memory_order_release);
  for (size_t index = 0; index < internal::modules.size(); ++index) {
    const auto& slot = internal::modules[index];
    if (!slot.installed) continue;
    const auto detours = (index == 0 ? internal::SdkDetours<0>() : internal::SdkDetours<1>());
    for (size_t i = 0; i < detours.size(); ++i) {
      if (slot.real[i] != nullptr) detour_guard::Neutralize(&slot.real[i], detours[i], slot.records[i]);
    }
  }
}

inline void Poll(bool force_discovery = true) {
  if (internal::stopped.load(std::memory_order_acquire)) return;
  {
    std::unique_lock lock(internal::state.mutex);
    std::erase_if(internal::state.retired_contexts, [](const auto& context) {
      for (const auto& surface : context->surfaces) {
        if (!submission::ResourceReleasable(surface->texture.Get())) return false;
        if (surface->consumer_fence != nullptr
            && surface->consumer_fence->GetCompletedValue() < surface->consumer_value) return false;
      }
      // Destroy succeeded: Intel requires the caller to finish tag recordings
      // before that boundary. Remaining plugin recordings still gate release.
      return true;
    });
  }
  HMODULE first = nullptr;
  DWORD needed = 0;
  if (!EnumProcessModules(GetCurrentProcess(), &first, sizeof(first), &needed)) return;
  if (!force_discovery && internal::module_bytes.load(std::memory_order_acquire) == needed) return;
  std::vector<HMODULE> loaded(needed / sizeof(HMODULE) + 16);
  if (!EnumProcessModules(GetCurrentProcess(), loaded.data(), static_cast<DWORD>(loaded.size() * sizeof(HMODULE)), &needed)
      || needed > loaded.size() * sizeof(HMODULE)) return;
  loaded.resize(needed / sizeof(HMODULE));
  bool retry = false;
  for (HMODULE borrowed : loaded) {
    wchar_t path[32768] = {};
    if (GetModuleFileNameW(borrowed, path, static_cast<DWORD>(std::size(path))) == 0) continue;
    const wchar_t* name = std::wcsrchr(path, L'\\');
    name = name == nullptr ? path : name + 1;
    if (_wcsicmp(name, L"libxess_fg.dll") != 0 && _wcsicmp(name, L"libxess_fg_dx12.dll") != 0) continue;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(borrowed), &module)) continue;
    const std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype(&FreeLibrary)> module_reference(module, FreeLibrary);
    std::unique_lock install(internal::install_mutex, std::try_to_lock);
    if (!install.owns_lock()) return;
    for (size_t index = 0; index < internal::modules.size(); ++index) {
      auto& slot = internal::modules[index];
      if (slot.installed) {
        if (slot.module == module) break;
        continue;
      }
      std::array<void*, internal::EXPORTS.size()> exports{};
      bool complete = true;
      size_t available = 0;
      for (size_t i = 0; i < exports.size(); ++i) {
        exports[i] = reinterpret_cast<void*>(GetProcAddress(module, internal::EXPORTS[i]));
        available += exports[i] != nullptr;
        // Older public runtimes always interpolate UI and have no dynamic
        // composition setter. Absence keeps the documented disabled default.
        if (i != 7) complete &= exports[i] != nullptr;
      }
      if (!complete) break;
      HMODULE pinned = nullptr;
      if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
              reinterpret_cast<LPCWSTR>(module), &pinned)) break;
      slot.module = module;
      slot.real = exports;
      const auto detours = (index == 0 ? internal::SdkDetours<0>() : internal::SdkDetours<1>());
      std::array<detour_guard::Hook, internal::EXPORTS.size()> hooks;
      for (size_t i = 0; i < hooks.size(); ++i) hooks[i] = {
          .name = internal::EXPORTS[i], .real = &slot.real[i], .detour = detours[i], .record = &slot.records[i]};
      size_t failed = 0;
      const auto attached = detour_guard::AttachAll(hooks, &failed);
      slot.installed = attached != 0;
      slot.complete = attached == available;
      retry |= attached == 0;
      break;
    }
  }
  if (!retry) internal::module_bytes.store(needed, std::memory_order_release);
}

}  // namespace renodx::addons::dlss5::fg::xefg
