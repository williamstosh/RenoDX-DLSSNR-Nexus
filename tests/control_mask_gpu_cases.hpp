#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <vector>

namespace control_mask_gpu_cases {
namespace addon = renodx::addons::dlss5::internal;

using DispatchFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
inline DispatchFn real_dispatch = nullptr;
inline uint32_t mask_dispatches = 0;
inline ID3D12PipelineState* observed_pipeline = nullptr;
using PipelineFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
inline PipelineFn real_pipeline = nullptr;
using AllocateFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*,
    D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
    const D3D12_CLEAR_VALUE*, REFIID, void**);
inline AllocateFn real_allocate = nullptr;
inline bool fail_mask_allocation = false;
inline uint32_t allocation_failures = 0;
using RootFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
using PsoFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
using HeapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**);
using SignalFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
using CompletedFn = UINT64(STDMETHODCALLTYPE*)(ID3D12Fence*);
using RemovedFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*);
inline RootFn real_root = nullptr;
inline PsoFn real_pso = nullptr;
inline HeapFn real_heap = nullptr;
inline SignalFn real_signal = nullptr;
inline CompletedFn real_completed = nullptr;
inline RemovedFn real_removed = nullptr;
inline ID3D12Device* removed_device = nullptr;
inline uint32_t runtime_initializations = 0;
inline uint32_t runtime_shutdowns = 0;
inline uint32_t source_evaluate_calls = 0;
inline uint32_t source_evaluate_c_calls = 0;
enum class Fault { NONE, ROOT, PSO, HEAP };
inline Fault fault = Fault::NONE;
inline uint32_t injected_failures = 0;
inline ID3D12Fence* signal_fault_fence = nullptr;
inline ID3D12Fence* removed_fence = nullptr;
inline int reservation_fault_stage = -1;

HRESULT STDMETHODCALLTYPE CreateRoot(ID3D12Device* device, UINT node, const void* blob, SIZE_T bytes,
                                     REFIID iid, void** result) {
  if (fault == Fault::ROOT) {
    ++injected_failures;
    *result = nullptr;
    return E_OUTOFMEMORY;
  }
  return real_root(device, node, blob, bytes, iid, result);
}
HRESULT STDMETHODCALLTYPE CreatePso(ID3D12Device* device, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
                                    REFIID iid, void** result) {
  if (fault == Fault::PSO) {
    ++injected_failures;
    *result = nullptr;
    return E_OUTOFMEMORY;
  }
  return real_pso(device, desc, iid, result);
}
HRESULT STDMETHODCALLTYPE CreateHeap(ID3D12Device* device, const D3D12_DESCRIPTOR_HEAP_DESC* desc,
                                     REFIID iid, void** result) {
  if (fault == Fault::HEAP) {
    ++injected_failures;
    *result = nullptr;
    return E_OUTOFMEMORY;
  }
  return real_heap(device, desc, iid, result);
}
HRESULT STDMETHODCALLTYPE Signal(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) {
  if (fence == signal_fault_fence) {
    ++injected_failures;
    signal_fault_fence = nullptr;
    return E_FAIL;
  }
  return real_signal(queue, fence, value);
}
UINT64 STDMETHODCALLTYPE Completed(ID3D12Fence* fence) {
  if (removed_device != nullptr) {
    ComPtr<ID3D12Device> device;
    if (SUCCEEDED(fence->GetDevice(IID_PPV_ARGS(&device))) && device.Get() == removed_device) return UINT64_MAX;
  }
  return fence == removed_fence ? UINT64_MAX : real_completed(fence);
}
HRESULT STDMETHODCALLTYPE RemovedReason(ID3D12Device* device) {
  return device == removed_device ? DXGI_ERROR_DEVICE_REMOVED : real_removed(device);
}
NVSDK_NGX_Result NVSDK_CONV SourceEvaluate(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
    const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback) {
  ++source_evaluate_calls;
  return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result NVSDK_CONV SourceEvaluateC(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback_C) {
  ++source_evaluate_c_calls;
  return addon::HookedEvaluateFeatureSlot<0>(list, handle, parameters, nullptr);
}
NVSDK_NGX_Result NVSDK_CONV InitializeRuntime(unsigned long long, const wchar_t*, ID3D12Device*,
                                             NVSDK_NGX_Version, const NVSDK_NGX_Parameter*) {
  ++runtime_initializations;
  return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result NVSDK_CONV ShutdownRuntime(ID3D12Device*) {
  ++runtime_shutdowns;
  return NVSDK_NGX_Result_Success;
}
void ReservationFault(unsigned stage) {
  if (reservation_fault_stage == static_cast<int>(stage)) {
    reservation_fault_stage = -1;
    ++injected_failures;
    throw std::bad_alloc();
  }
}

inline bool wrapper_returned = false;
inline uint32_t wrapper_post_reads = 0;
inline bool wrapper_credential_seen = false;
inline bool wrapper_probe_adoption = false;
inline bool wrapper_adopted = false;
inline ID3D12GraphicsCommandList* wrapper_list = nullptr;
inline uint32_t failed_core_creates = 0;
inline uint32_t forbidden_fallback_creates = 0;

struct CredentialParameters : NVSDK_NGX_Parameter {
  renodx::addons::dlss5::bridge::OwnedParameters values;

  // Observe the production after-path through its ordinary Output query,
  // after the real-export substitute has returned Success.
#define NR_CREDENTIAL_PARAMETER(type) \
  void Set(const char* key, type value) override { values.Set(key, value); } \
  NVSDK_NGX_Result Get(const char* key, type* value) const override { \
    if (wrapper_returned && std::strcmp(key, NVSDK_NGX_Parameter_Output) == 0) { \
      ++wrapper_post_reads; \
      wrapper_credential_seen |= addon::successful_sr_device != nullptr; \
      if (wrapper_probe_adoption) { \
        wrapper_adopted |= addon::EnsureDirectRuntime(wrapper_list); \
      } \
    } \
    return values.Get(key, value); \
  }
  NR_CREDENTIAL_PARAMETER(unsigned long long)
  NR_CREDENTIAL_PARAMETER(float)
  NR_CREDENTIAL_PARAMETER(double)
  NR_CREDENTIAL_PARAMETER(unsigned int)
  NR_CREDENTIAL_PARAMETER(int)
  NR_CREDENTIAL_PARAMETER(ID3D11Resource*)
  NR_CREDENTIAL_PARAMETER(ID3D12Resource*)
  NR_CREDENTIAL_PARAMETER(void*)
#undef NR_CREDENTIAL_PARAMETER
  void Reset() override { values.Reset(); }
};

NVSDK_NGX_Result NVSDK_CONV SuccessfulHostEvaluate(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
    const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback) {
  wrapper_returned = true;
  return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result NVSDK_CONV SuccessfulHostEvaluateC(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
    const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback_C) {
  wrapper_returned = true;
  return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result NVSDK_CONV FailCoreCreate(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
    const NVSDK_NGX_Parameter*, NVSDK_NGX_Handle** handle) {
  ++failed_core_creates;
  *handle = nullptr;
  return NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
}
NVSDK_NGX_Result NVSDK_CONV ForbiddenFallbackCreate(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
    const NVSDK_NGX_Parameter*, NVSDK_NGX_Handle** handle) {
  ++forbidden_fallback_creates;
  *handle = nullptr;
  return NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
}

void RunBorrowedWrapperClassification(Gpu& gpu) {
  const auto slot = addon::ngx_slot_real[0];
  const bool enabled = addon::enabled.exchange(true);
  const bool device_seen = addon::d3d12_device_seen.exchange(true);
  const bool foreign_seen = addon::foreign_nr_seen.exchange(true);
  const bool foreign_yield = addon::foreign_nr_yield.exchange(true);
  addon::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(SuccessfulHostEvaluate);
  addon::ngx_slot_real[0].evaluate_c = reinterpret_cast<void*>(SuccessfulHostEvaluateC);
  wrapper_list = gpu.list.Get();
  CredentialParameters parameters;
  const auto* handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xC0BE001});
  for (const bool c_export : {false, true}) {
    // Unknown, guessed DLSS, guessed RR, observed non-SR, observed SR/RR.
    for (unsigned source = 0; source < 6; ++source) {
      if (source == 1 || source == 2) {
        addon::features[handle].source_feature = (source == 1 ? addon::kFeatureDlss : addon::kFeatureDlssd);
        addon::features[handle].source_type_confirmed = true;
      } else if (source >= 3) {
        addon::created_feature_ids[handle] = (source == 3 ? NVSDK_NGX_Feature_FrameGeneration
            : source == 4 ? addon::kFeatureDlss : addon::kFeatureDlssd);
      }
      wrapper_returned = wrapper_credential_seen = wrapper_adopted = false;
      wrapper_post_reads = 0;
      wrapper_probe_adoption = source < 3;
      addon::last_present_steady_ns = addon::SteadyNowNs();
      const auto result = (c_export
          ? addon::HookedEvaluateFeatureCSlot<0>(gpu.list.Get(), handle, &parameters, nullptr)
          : addon::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), handle, &parameters, nullptr));
      Check(NVSDK_NGX_SUCCEED(result) && wrapper_returned, "wrapper preserves successful unknown/known Evaluate");
      if (source < 3) {
        Check(wrapper_post_reads != 0 && !wrapper_credential_seen && !wrapper_adopted
                  && addon::direct_device == nullptr && addon::direct_load_state == addon::DirectLoadState::Unknown,
              "successful unknown or guessed SR Evaluate cannot lend Core through either wrapper");
      } else {
        Check(wrapper_credential_seen == (source >= 4), "only observed SR/RR Create can grant wrapper credential");
      }
      Check(addon::successful_sr_device == nullptr, "wrapper credential ends with callback");
      addon::ForgetSourceHandleLocked(handle);
    }
  }
  wrapper_list = nullptr;
  wrapper_probe_adoption = false;
  addon::ngx_slot_real[0] = slot;
  addon::enabled = enabled;
  addon::d3d12_device_seen = device_seen;
  addon::foreign_nr_seen = foreign_seen;
  addon::foreign_nr_yield = foreign_yield;
  std::cout << "PASS borrowed Core C++/C successful unknown/guessed/non-SR/observed SR classification evidence=MOCK+GPU\n";
}

void RunBorrowedCore(Gpu& gpu, bool hardware, bool second_adapter) {
  Check(!addon::control_mask_use_core_context.load(), "Core diagnostic defaults off");
  const auto* published = addon::ngx_loader_published.load();
  addon::NgxLoaderSymbols symbols;
  symbols.nr_ready = symbols.core_ready = true;
  symbols.nr_module = symbols.core = GetModuleHandleW(nullptr);
  symbols.init = InitializeRuntime;
  symbols.shutdown = ShutdownRuntime;
  symbols.core_allocate = renodx::addons::dlss5::bridge::AllocateOwnedParameters;
  symbols.core_destroy = renodx::addons::dlss5::bridge::DestroyOwnedParameters;
  symbols.core_create = CreateModel;
  symbols.core_evaluate = EvaluateModel;
  symbols.core_release = ReleaseModel;
  symbols.nr_requirements.file_version = "310.9.16.0";
  addon::ngx_loader_published.store(&symbols);
  addon::control_mask_use_core_context = true;
  addon::hook_point = addon::kHookUpscaled;
  addon::stack_passes = 1;
  addon::control_mask_enabled = false;
  RunBorrowedWrapperClassification(gpu);
  addon::last_result = NVSDK_NGX_Result_FAIL_InvalidParameter;
  const auto last = addon::last_result.load();
  Check(!addon::EnsureDirectRuntime(gpu.list.Get()) && !addon::LoadDirectApi()
            && addon::direct_load_state == addon::DirectLoadState::Unknown,
        "resolved exports without successful SR cannot adopt Core");
  {
    addon::SuccessfulSrCoreScope sr(gpu.list.Get());
    for (unsigned missing = 0; missing < 7; ++missing) {
      const auto complete = symbols;
      switch (missing) {
        case 0: symbols.core_allocate = nullptr; break;
        case 1: symbols.core_destroy = nullptr; break;
        case 2: symbols.core_create = nullptr; break;
        case 3: symbols.core_evaluate = nullptr; break;
        case 4: symbols.core_release = nullptr; break;
        case 5: symbols.core_ready = false; break;
        case 6: symbols.nr_requirements.file_version = "310.9.16.1"; break;
      }
      Check(!addon::EnsureDirectRuntime(gpu.list.Get())
                && addon::direct_load_state == addon::DirectLoadState::Unknown
                && addon::get_module_filename_iat == nullptr,
            "incomplete Core or wrong runtime cannot touch identity or adopt");
      symbols = complete;
    }
    Check(addon::EnsureDirectRuntime(gpu.list.Get()) && addon::direct_api.borrowed_core_context
              && addon::parameter_runtime_via_core && addon::direct_device == gpu.device.Get()
              && addon::direct_api.init == nullptr && addon::direct_api.shutdown == nullptr
              && addon::last_result.load() == last,
          "none baseline adopts exact SR device without fabricating Init result");
    {
      addon::SuccessfulSrCoreScope invalid(nullptr);
      Check(!addon::EnsureDirectRuntime(gpu.list.Get()), "nested empty credential shadows ready device");
    }
    Check(addon::successful_sr_device == gpu.device.Get()
              && addon::EnsureDirectRuntime(gpu.list.Get()), "nested credential restores outer SR");
    {
      renodx::addons::dlss5::DirectCallScope own;
      Check(!addon::EnsureDirectRuntime(gpu.list.Get()), "own NGX reentry cannot borrow inherited credential");
    }
    for (unsigned missing = 0; missing < 5; ++missing) {
      const auto complete = symbols;
      switch (missing) {
        case 0: symbols.core_allocate = nullptr; break;
        case 1: symbols.core_destroy = nullptr; break;
        case 2: symbols.core_create = nullptr; break;
        case 3: symbols.core_evaluate = nullptr; break;
        case 4: symbols.core_release = nullptr; break;
      }
      Check(!addon::EnsureDirectRuntime(gpu.list.Get()), "ready fast path still requires every Core export");
      symbols = complete;
    }
    addon::control_mask_use_core_context = false;
    Check(!addon::EnsureDirectRuntime(gpu.list.Get()) && !addon::LoadDirectApi(),
          "borrowed ready backend cannot hot-switch to legacy");
    addon::control_mask_use_core_context = true;
    addon::direct_api.borrowed_core_context = false;
    Check(!addon::EnsureDirectRuntime(gpu.list.Get()), "legacy ready backend cannot hot-switch to borrowed");
    addon::direct_api.borrowed_core_context = true;
    addon::hook_point = addon::kHookPresent;
    Check(!addon::EnsureDirectRuntime(gpu.list.Get()), "Present cannot borrow current SR context");
    addon::hook_point = addon::kHookUpscaled;
    addon::stack_passes = 2;
    Check(!addon::EnsureDirectRuntime(gpu.list.Get()), "stack cannot borrow P0 context");
    addon::stack_passes = 1;
    if (second_adapter) {
      Gpu other(!hardware);
      Check(other.device.Get() != gpu.device.Get(), "distinct adapter device");
      Check(!addon::EnsureDirectRuntime(other.list.Get()), "SR credential cannot lend another device");
      {
        addon::SuccessfulSrCoreScope nested(other.list.Get());
        Check(!addon::EnsureDirectRuntime(gpu.list.Get()), "nested SR cannot borrow outer device");
        Check(addon::EnsureDirectRuntime(other.list.Get()), "device change adopts only nested successful SR face");
      }
      Check(addon::EnsureDirectRuntime(gpu.list.Get()), "outer credential restored after device change");
    }
    addon::FeatureState state;
    addon::FinalResources resources;
    Check(addon::InstallQueueCompletionHooks(gpu.queue.Get()), "borrowed Core production queue tracking");
    Check(addon::EnsureNrFeature(gpu.list.Get(), state, 0, resources, 8, 8, 8, 8)
              && state.slots[0].via_core, "borrow creates plugin NR through Core");
    Check(!addon::Shutdown(false) && addon::direct_device == gpu.device.Get(),
          "borrowed teardown still retains unsubmitted feature recording");
    // Complete and Reset the real recording before releasing its mock feature.
    gpu.Submit();
    const auto releases = model_released_handles.size();
    addon::ReleaseNrSlot(state, 0, false);
    Check(state.slots[0].handle == nullptr && state.slots[0].parameters == nullptr
              && model_released_handles.size() == releases + 1, "borrow releases plugin feature and parameters");
    addon::core_create_feature = FailCoreCreate;
    addon::direct_api.create = ForbiddenFallbackCreate;
    addon::FeatureState rejected;
    Check(!addon::EnsureNrFeature(gpu.list.Get(), rejected, 0, resources, 8, 8, 8, 8)
              && rejected.slots[0].failed && rejected.slots[0].parameters == nullptr
              && failed_core_creates == 1 && forbidden_fallback_creates == 0
              && runtime_initializations == 0 && addon::get_module_filename_iat == nullptr,
          "borrowed Core Create failure releases parameters without fallback/identity/Init");
  }
  Check(!addon::EnsureDirectRuntime(gpu.list.Get()), "ready Core cannot serve unscoped fallback");
  Check(addon::Shutdown(false) && addon::direct_device == nullptr
            && runtime_initializations == 0 && runtime_shutdowns == 0
            && addon::get_module_filename_iat == nullptr,
        "full borrowed teardown makes no SDK Init/Shutdown or identity calls");
  addon::control_mask_use_core_context = false;
  addon::ngx_loader_published.store(published);
  std::cout << "PASS borrowed Core credential/exports/backend/teardown evidence=MOCK+GPU no real NGX\n";
}

void STDMETHODCALLTYPE ObservePipeline(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline) {
  observed_pipeline = pipeline;
  real_pipeline(list, pipeline);
}

void STDMETHODCALLTYPE ObserveDispatch(ID3D12GraphicsCommandList* list, UINT x, UINT y, UINT z) {
  if (addon::codec_pipeline.control_mask != nullptr && observed_pipeline == addon::codec_pipeline.control_mask) {
    ++mask_dispatches;
  }
  real_dispatch(list, x, y, z);
}

HRESULT STDMETHODCALLTYPE Allocate(ID3D12Device* device, const D3D12_HEAP_PROPERTIES* heap,
    D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
    const D3D12_CLEAR_VALUE* clear, REFIID iid, void** resource) {
  if (fail_mask_allocation && desc->Format == DXGI_FORMAT_R8G8B8A8_UNORM
      && state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS
      && (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0) {
    ++allocation_failures;
    *resource = nullptr;
    return E_OUTOFMEMORY;
  }
  return real_allocate(device, heap, flags, desc, state, clear, iid, resource);
}

void RunInline(Gpu& gpu, bool hardware, bool safety) {
  using renodx::addons::dlss5::NrDeclineReason;
  using renodx::addons::dlss5::kNrDeclineTags;
  Check(static_cast<size_t>(NrDeclineReason::kControlMaskSubmissionsUnproven)
                == static_cast<size_t>(NrDeclineReason::kStageResolutionBackend) + 1
            && std::strcmp(kNrDeclineTags[static_cast<size_t>(NrDeclineReason::kWorksetSetupFailed)], "setup_failed") == 0
            && std::strcmp(kNrDeclineTags[static_cast<size_t>(NrDeclineReason::kControlMaskSubmissionsUnproven)], "control_mask_unproven") == 0
            && std::strcmp(kNrDeclineTags[static_cast<size_t>(NrDeclineReason::kControlMaskReuseNotReady)], "control_mask_reuse_not_ready") == 0,
        "ControlMask decline tags append to the taxonomy and preserve setup_failed");
  model_gpu = &gpu;
  addon::enabled = true;
  addon::hooks_enabled = true;
  addon::dedupe_mode = renodx::addons::dlss5::kDedupePerEvaluate;
  addon::hook_point = addon::kHookUpscaled;
  addon::stack_passes = 1;
  addon::nr_follow_input_res = false;
  addon::nr_resolution_scale = 1.f;
  addon::feed_mode = 0;
  addon::look_mode = false;
  addon::detail_stability = 1;
  addon::transfer_strength = 1.f;
  addon::color_strength = 1.f;
  addon::intensity = .5f;
  addon::preset = 2;
  addon::codec_mode = 0;
  addon::direct_api.module = GetModuleHandleW(nullptr);
  addon::direct_api.create = CreateModel;
  addon::direct_api.evaluate = EvaluateModel;
  addon::direct_api.release = ReleaseModel;
  addon::direct_load_state = addon::DirectLoadState::Ready;
  addon::direct_device = gpu.device.Get();
  addon::direct_device->AddRef();
  addon::direct_device_native = gpu.device.Get();
  addon::direct_runtime_requirements.dual_models = true;
  addon::direct_runtime_requirements.file_version = "310.9.16.0";
  addon::parameter_runtime_ready = true;
  addon::core_allocate_parameters = renodx::addons::dlss5::bridge::AllocateOwnedParameters;
  addon::core_destroy_parameters = renodx::addons::dlss5::bridge::DestroyOwnedParameters;
  const auto texture = [&](DXGI_FORMAT format, bool uav) {
    const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_RESOURCE_DESC desc{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Width = 8, .Height = 8, .DepthOrArraySize = 1, .MipLevels = 1, .Format = format,
        .SampleDesc = {1, 0}, .Flags = (uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE)};
    ComPtr<ID3D12Resource> resource;
    Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource))), "ControlMask inline texture");
    return resource;
  };
  auto source = texture(DXGI_FORMAT_R8G8B8A8_UNORM, false);
  auto output = texture(DXGI_FORMAT_R8G8B8A8_UNORM, true);
  auto depth = texture(DXGI_FORMAT_R32_FLOAT, false);
  auto motion = texture(DXGI_FORMAT_R16G16_FLOAT, false);
  gpu.Write(source.Get(), 96);
  gpu.Write(output.Get(), 96);
  gpu.Transition(source.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
  gpu.Transition(depth.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
  gpu.Transition(motion.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
  gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  gpu.Submit();
  renodx::addons::dlss5::bridge::OwnedParameters parameters;
  parameters.Set(NVSDK_NGX_Parameter_Color, source.Get());
  parameters.Set(NVSDK_NGX_Parameter_Output, output.Get());
  parameters.Set(NVSDK_NGX_Parameter_Depth, depth.Get());
  parameters.Set(NVSDK_NGX_Parameter_MotionVectors, motion.Get());
  parameters.Set(NVSDK_NGX_Parameter_Width, 8u);
  parameters.Set(NVSDK_NGX_Parameter_Height, 8u);
  parameters.Set(NVSDK_NGX_Parameter_OutWidth, 8u);
  parameters.Set(NVSDK_NGX_Parameter_OutHeight, 8u);
  parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 8u);
  parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 8u);
  parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
      static_cast<uint32_t>(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes));
  parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
  parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
  const auto* owner = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xC04A6});
  addon::RegisterCreatedFeatureLocked(owner, addon::kFeatureDlss, &parameters);
  real_dispatch = reinterpret_cast<DispatchFn>((*reinterpret_cast<void***>(gpu.list.Get()))[14]);
  real_pipeline = reinterpret_cast<PipelineFn>((*reinterpret_cast<void***>(gpu.list.Get()))[25]);
  real_allocate = reinterpret_cast<AllocateFn>((*reinterpret_cast<void***>(gpu.device.Get()))[27]);
  real_root = reinterpret_cast<RootFn>((*reinterpret_cast<void***>(gpu.device.Get()))[16]);
  real_pso = reinterpret_cast<PsoFn>((*reinterpret_cast<void***>(gpu.device.Get()))[11]);
  real_heap = reinterpret_cast<HeapFn>((*reinterpret_cast<void***>(gpu.device.Get()))[14]);
  real_signal = reinterpret_cast<SignalFn>((*reinterpret_cast<void***>(gpu.queue.Get()))[14]);
  real_completed = reinterpret_cast<CompletedFn>((*reinterpret_cast<void***>(gpu.fence.Get()))[8]);
  real_removed = reinterpret_cast<RemovedFn>((*reinterpret_cast<void***>(gpu.device.Get()))[37]);
  Check(DetourTransactionBegin() == NO_ERROR && DetourUpdateThread(GetCurrentThread()) == NO_ERROR,
        "ControlMask observer transaction");
  Check(DetourAttach(reinterpret_cast<PVOID*>(&real_dispatch), reinterpret_cast<PVOID>(ObserveDispatch)) == NO_ERROR
            && DetourAttach(reinterpret_cast<PVOID*>(&real_pipeline), reinterpret_cast<PVOID>(ObservePipeline)) == NO_ERROR
            && DetourAttach(reinterpret_cast<PVOID*>(&real_allocate), reinterpret_cast<PVOID>(Allocate)) == NO_ERROR
            && DetourAttach(reinterpret_cast<PVOID*>(&real_root), reinterpret_cast<PVOID>(CreateRoot)) == NO_ERROR
            && DetourAttach(reinterpret_cast<PVOID*>(&real_pso), reinterpret_cast<PVOID>(CreatePso)) == NO_ERROR
            && DetourAttach(reinterpret_cast<PVOID*>(&real_heap), reinterpret_cast<PVOID>(CreateHeap)) == NO_ERROR
            && DetourAttach(reinterpret_cast<PVOID*>(&real_signal), reinterpret_cast<PVOID>(Signal)) == NO_ERROR
            && DetourAttach(reinterpret_cast<PVOID*>(&real_completed), reinterpret_cast<PVOID>(Completed)) == NO_ERROR
            && DetourAttach(reinterpret_cast<PVOID*>(&real_removed), reinterpret_cast<PVOID>(RemovedReason)) == NO_ERROR
            && DetourTransactionCommit() == NO_ERROR, "ControlMask observers attached");
  const auto record = [&] {
    const auto before = model_calls;
    const auto gate_before = addon::control_mask_reuse_stats;
    addon::ProcessInline(gpu.list.Get(), owner, &parameters);
    Check(addon::control_mask_reuse_stats.checks - gate_before.checks <= 1
              && addon::control_mask_reuse_stats.admitted - gate_before.admitted
                  <= addon::control_mask_reuse_stats.checks - gate_before.checks,
          "one ProcessInline contributes at most one coherent reuse check and admission");
    gpu.Submit();
    addon::DrainRetiredResources(true);
    Check(model_calls <= before + 1, "at most one NR Evaluate per frame, including failures");
    return model_calls - before;
  };
  const auto mature = [&] {
    for (uint32_t attempt = 0; attempt < addon::kContractDebounceFrames + 32; ++attempt) {
      if (record() == 1) return;
    }
    Check(false, "ControlMask production path matures within bounded attempts");
  };
  addon::control_mask_enabled = false;
  mature();
  Check(mask_dispatches == 0, "default-off ProcessInline records zero ControlMask dispatches");
  Check(addon::control_mask_reuse_stats.checks == 0 && addon::control_mask_reuse_stats.admitted == 0,
        "default-off without old mask objects contributes no reuse checks or admissions");
  Check(addon::worksets.at({owner, output.Get(), false}).control_mask == nullptr,
        "default-off production path allocates no mask");
  Check(addon::worksets.at({owner, output.Get(), false}).recording == nullptr
            && addon::features.at(owner).slots[0].recording == nullptr,
        "default-off NR allocates no ControlMask recording observations");
  addon::control_mask_enabled = true;
  bool saw_mask = false;
  model_parameter_observer = [&](const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter* block) {
    saw_mask = addon::GetD3D12Resource(block, "DLSSNR.ControlMask") != nullptr;
  };
  mature();
  Check(saw_mask && mask_dispatches > 0, "production Evaluate receives generated mask");
  const auto previous_evaluate = addon::ngx_slot_real[0].evaluate;
  const auto previous_evaluate_c = addon::ngx_slot_real[0].evaluate_c;
  addon::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(SourceEvaluate);
  addon::ngx_slot_real[0].evaluate_c = reinterpret_cast<void*>(SourceEvaluateC);
  const auto exercise_hook = [&](bool c_export) {
    const auto source_before = source_evaluate_calls;
    const auto source_c_before = source_evaluate_c_calls;
    const auto model_before = model_calls;
    const auto dispatch_before = mask_dispatches;
    gpu.list->ClearState(nullptr);
    const auto result = c_export
        ? addon::HookedEvaluateFeatureCSlot<0>(gpu.list.Get(), owner, &parameters, nullptr)
        : addon::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), owner, &parameters, nullptr);
    Check(NVSDK_NGX_SUCCEED(result), "production NGX evaluate wrapper preserves a successful source result");
    Check(source_evaluate_calls == source_before + 1
              && source_evaluate_c_calls == source_c_before + (c_export ? 1u : 0u),
          "production evaluate wrapper calls its source once, including C-to-C++ delegation");
    gpu.Submit();
    addon::DrainRetiredResources(true);
    Check(model_calls == model_before + 1 && mask_dispatches == dispatch_before + 1,
          "production evaluate wrapper records one real NR path and one ControlMask dispatch");
    const auto& wrapper_slot = addon::features.at(owner).slots[0];
    Check(!wrapper_slot.control_mask_bound
              && addon::GetD3D12Resource(wrapper_slot.parameters, "DLSSNR.ControlMask") == nullptr,
          "production evaluate wrapper clears the transient mask binding");
    std::cout << "CASE source=MOCK wrapper=" << (c_export ? "C+Ex/C++" : "Ex/C++")
              << " inner_ngx=MOCK completion=Fence" << std::endl;
  };
  exercise_hook(false);
  exercise_hook(true);
  addon::ngx_slot_real[0].evaluate = previous_evaluate;
  addon::ngx_slot_real[0].evaluate_c = previous_evaluate_c;
  auto& slot = addon::features.at(owner).slots[0];
  Check(!slot.control_mask_bound
            && addon::GetD3D12Resource(slot.parameters, "DLSSNR.ControlMask") == nullptr,
        "completed Evaluate leaves no stale mask in reusable NGX parameters");
  for (uint32_t mode : {addon::kControlMaskNeutral, addon::kControlMaskRect,
                       addon::kControlMaskGreenRect, addon::kControlMaskBlueRect}) {
    addon::control_mask_mode = mode;
    const auto before = mask_dispatches;
    Check(record() == 1 && mask_dispatches == before + 1 && !slot.control_mask_bound,
          "mode switch has one mask dispatch, one Evaluate and no stale binding");
    Check(addon::worksets.at({owner, output.Get(), false}).control_mask_state
                  == D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
          "mode switch restores mask UAV state after Evaluate");
  }
  const auto enabled_dispatches = mask_dispatches;
  addon::control_mask_enabled = false;
  mature();
  Check(!saw_mask && mask_dispatches == enabled_dispatches,
        "switching off clears binding and stops mask dispatches");
  addon::control_mask_enabled = true;
  addon::direct_runtime_requirements.file_version = "310.9.16.1";
  mature();
  Check(!saw_mask && mask_dispatches == enabled_dispatches,
        "nonexact runtime records no mask dispatch and passes no mask");
  addon::direct_runtime_requirements.file_version = "310.9.16.0";
  fail_mask_allocation = true;
  const auto before_failure = stage_execution_cases::Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  gpu.Submit();
  Check(record() == 0 && allocation_failures == 1, "mask allocation failure skips NR without retry");
  const auto after_failure = stage_execution_cases::Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  gpu.Submit();
  Check(before_failure.Pixels() == after_failure.Pixels() && slot.pending_reset && !slot.control_mask_bound,
        "allocation failure preserves game output, clears mask and arms history reset");
  fail_mask_allocation = false;
  mature();
  Check(model_resets.back() == 1, "allocation recovery resets History through production retry");
  std::cout << "CASE location=mask-allocation evidence=MOCK+GPU size=8x8 output=preserved binding=clear recovery=production-retry\n";
  for (const auto injected : {Fault::ROOT, Fault::PSO, Fault::HEAP}) {
    if (injected == Fault::ROOT) addon::ReleaseCodecPipeline();
    else if (injected == Fault::PSO) addon::ReleaseCom(addon::codec_pipeline.control_mask);
    else addon::RetireWorkset({owner, output.Get(), false});
    const auto before = injected_failures;
    fault = injected;
    Check(record() == 0 && injected_failures > before && slot.failed && !slot.control_mask_bound,
          "setup creation failure skips model and clears mask");
    fault = Fault::NONE;
    // Advance only the existing failure clock; never clear the production latch.
    slot.failed_ns -= addon::kFeatureRetryMaxNs;
    mature();
    Check(model_resets.back() == 1, "setup retry sends History reset");
    std::cout << "CASE location=create-" << static_cast<int>(injected)
              << " evidence=MOCK+GPU size=8x8 binding=clear recovery=expired-production-backoff\n";
  }
  fail_call = model_calls + 1;
  const auto before_evaluate_failure = stage_execution_cases::Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  gpu.Submit();
  const auto gate_before_failure = addon::control_mask_reuse_stats;
  Check(record() == 1, "NGX failure has exactly one Evaluate attempt");
  Check(addon::control_mask_reuse_stats.checks == gate_before_failure.checks + 1
            && addon::control_mask_reuse_stats.admitted == gate_before_failure.admitted + 1,
        "reuse gate admission is counted even when the subsequent NGX Evaluate fails");
  const auto after_evaluate_failure = stage_execution_cases::Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  gpu.Submit();
  Check(before_evaluate_failure.Pixels() == after_evaluate_failure.Pixels()
            && !addon::features.at(owner).slots[0].control_mask_bound,
        "NGX failure preserves game output and clears the old mask");
  fail_call = 0;
  mature();
  Check(model_resets.back() == 1, "NGX failure recovers with History reset");
  uint32_t exception_attempts = 0;
  model_parameter_observer = [&](const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*) {
    ++exception_attempts;
    throw std::runtime_error("injected ControlMask NGX exception");
  };
  const auto before_exception = stage_execution_cases::Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  gpu.Submit();
  record();
  const auto after_exception = stage_execution_cases::Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  gpu.Submit();
  Check(exception_attempts == 1 && before_exception.Pixels() == after_exception.Pixels()
            && !slot.control_mask_bound, "NGX exception clears mask without retry or output change");
  model_parameter_observer = {};
  mature();
  Check(model_resets.back() == 1, "NGX exception recovers with History reset");
  int unknown_object = 0;
  const auto& completed_workset = addon::worksets.at({owner, output.Get(), false});
  Check(submission::CanReuseRecordingObjects(
            addon::CollectWorksetGpuObjects(completed_workset),
            submission::RecordingIdentity(gpu.list.Get()), completed_workset.recording),
        "pruned healthy completed recording retains positive object provenance");
  Check(!submission::CanReuseRecordingObjects({&unknown_object},
            submission::RecordingIdentity(gpu.list.Get()), completed_workset.recording),
        "untracked object cannot borrow another object's completed receipt");
  std::cout << "CASE location=NGX-return-and-exception evidence=MOCK+GPU size=8x8 output=preserved binding=clear recovery=production-retry\n";

  // A second list cannot reuse state whose first recording has no submission.
  ComPtr<ID3D12CommandAllocator> other_allocator;
  ComPtr<ID3D12GraphicsCommandList> other_list;
  Check(SUCCEEDED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
      IID_PPV_ARGS(&other_allocator))), "second allocator");
  Check(SUCCEEDED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
      other_allocator.Get(), nullptr, IID_PPV_ARGS(&other_list))), "second list");
  const addon::WorksetKey key{owner, output.Get(), false};
  const auto assert_gate = [&](ID3D12GraphicsCommandList* list, bool admitted) {
    const auto before = addon::control_mask_reuse_stats;
    const auto declines = renodx::addons::dlss5::nr_decline_counts[static_cast<size_t>(
        renodx::addons::dlss5::NrDeclineReason::kControlMaskReuseNotReady)].load();
    addon::ProcessInline(list, owner, &parameters);
    Check(addon::control_mask_reuse_stats.checks == before.checks + 1
              && addon::control_mask_reuse_stats.admitted == before.admitted + (admitted ? 1 : 0)
              && renodx::addons::dlss5::nr_decline_counts[static_cast<size_t>(
                  renodx::addons::dlss5::NrDeclineReason::kControlMaskReuseNotReady)].load() == declines + (admitted ? 0 : 1),
          "actual reuse gate counts one check and exactly one admission or classified rejection");
  };
  const auto before_parked = model_calls;
  addon::ProcessInline(gpu.list.Get(), owner, &parameters);
  Check(model_calls == before_parked + 1, "first parked recording evaluates");
  addon::ProcessInline(gpu.list.Get(), owner, &parameters);
  Check(model_calls == before_parked + 2, "exclusive same recording permits ordered reuse");
  auto* parked_mask = addon::worksets.at(key).control_mask;
  Check(!addon::ProcessInline(other_list.Get(), owner, &parameters) && model_calls == before_parked + 2,
        "unsubmitted recording rejects second list reuse without another Evaluate");
  // Real Close/Reset discards work. Do not submit commands with stale before-states.
  Check(SUCCEEDED(gpu.list->Close()) && SUCCEEDED(gpu.allocator->Reset())
            && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)), "discard parked recording");
  submission::OnCommandListReset(gpu.list.Get());
  submission::PruneCompletedGenerations();
  const auto old_budget = addon::max_worksets.load();
  addon::max_worksets = 1;
  ++addon::present_generation;
  auto output_b = texture(DXGI_FORMAT_R8G8B8A8_UNORM, true);
  gpu.Transition(output_b.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  gpu.Submit();
  parameters.Set(NVSDK_NGX_Parameter_Output, output_b.Get());
  mature();
  Check(addon::worksets.at({owner, output_b.Get(), false}).control_mask != parked_mask
            && model_resets.back() == 1, "discarded A cannot re-key into B with stale state or History");
  parameters.Set(NVSDK_NGX_Parameter_Output, output.Get());
  // Evict A under another owner before A gets a chance to inspect its old key.
  addon::ProcessInline(gpu.list.Get(), owner, &parameters);
  Check(SUCCEEDED(gpu.list->Close()) && SUCCEEDED(gpu.allocator->Reset())
            && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)), "discard before owner eviction");
  submission::OnCommandListReset(gpu.list.Get());
  submission::PruneCompletedGenerations();
  const auto previous_handle = slot.handle;
  const auto* other_owner = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xC04A7});
  Check(addon::EnsureWorkset(gpu.device.Get(), other_owner, output_b.Get(), 8, 8, 8, 8,
      DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1, 0, false, nullptr, nullptr, true) != nullptr,
      "other owner evicts discarded owner A workset");
  addon::max_worksets = old_budget;
  mature();
  Check(slot.handle != previous_handle && model_resets.back() == 1,
        "owner History reset survives eviction of its discarded workset");
  std::cout << "CASE location=unsubmitted-and-reset-rekey evidence=MOCK+GPU size=8x8 resource=fresh binding=clear recovery=discard-and-recreate\n";

  // Hold one actual queue, then record the next frame without waiting for it.
  ComPtr<ID3D12Fence> gate;
  Check(SUCCEEDED(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))), "GPU delay gate");
  const auto wait_real = [&] {
    Check(SUCCEEDED(real_signal(gpu.queue.Get(), gpu.fence.Get(), ++gpu.serial)), "test-owned completion signal");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Check(event != nullptr, "test-owned completion event");
    const auto hr = gpu.fence->SetEventOnCompletion(gpu.serial, event);
    const auto wait = SUCCEEDED(hr) ? WaitForSingleObject(event, 10000) : WAIT_FAILED;
    CloseHandle(event);
    Check(wait == WAIT_OBJECT_0 && real_completed(gpu.fence.Get()) >= gpu.serial,
          "independent real fence finishes actual GPU work before cleanup or removal mock");
  };
  Check(SUCCEEDED(gpu.queue->Wait(gate.Get(), 1)), "queue waits on test gate");
  const auto queued_before = model_calls;
  addon::ProcessInline(gpu.list.Get(), owner, &parameters);
  Check(SUCCEEDED(gpu.list->Close()), "queued frame Close");
  ID3D12CommandList* queued[]{gpu.list.Get()};
  gpu.queue->ExecuteCommandLists(1, queued);
  auto* queued_mask = addon::worksets.at(key).control_mask;
  try {
    Check(!submission::HasUnprovenSubmissions(), "valid delayed Fence is not an unknown submission");
    const auto saved_list = gpu.list;
    gpu.list = other_list;
    assert_gate(gpu.list.Get(), false);
    gpu.list = saved_list;
    Check(model_calls == queued_before + 1 && addon::worksets.at(key).control_mask == queued_mask,
          "healthy pending submission rejects reuse without being classified unknown");
    parameters.Set(NVSDK_NGX_Parameter_Output, output_b.Get());
    assert_gate(other_list.Get(), false);
    parameters.Set(NVSDK_NGX_Parameter_Output, output.Get());
    Check(model_calls == queued_before + 1,
          "rotating output cannot bypass shared owner NR History completion gate");
    Check(SUCCEEDED(other_list->Close()), "next queued frame Close");
    ID3D12CommandList* next[]{other_list.Get()};
    gpu.queue->ExecuteCommandLists(1, next);
    Check(!submission::ResourceReleasable(queued_mask), "delayed work remains retained");
  } catch (...) { gate->Signal(1); wait_real(); throw; }
  Check(SUCCEEDED(gate->Signal(1)), "release healthy queue gate");
  wait_real();
  Check(SUCCEEDED(other_list->Reset(other_allocator.Get(), nullptr)), "open empty rejected list");
  const auto before_replayable = model_calls;
  assert_gate(other_list.Get(), false);
  Check(model_calls == before_replayable,
        "Fence completion without Reset still rejects replayable prior recording");
  Check(SUCCEEDED(other_list->Close()), "close replayability probe without addon work");
  Check(SUCCEEDED(gpu.allocator->Reset()) && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)), "reset completed frame");
  Check(SUCCEEDED(other_allocator->Reset()) && SUCCEEDED(other_list->Reset(other_allocator.Get(), nullptr)), "reset completed second frame");
  submission::OnCommandListReset(gpu.list.Get());
  submission::OnCommandListReset(other_list.Get());
  addon::DrainRetiredResources(true);
  Check(submission::ResourceReleasable(queued_mask), "real delayed fence then Reset releases use");
  const auto gate_before_reset_recovery = addon::control_mask_reuse_stats;
  Check(record() == 1 && addon::worksets.at(key).control_mask == queued_mask,
        "healthy completion plus Reset permits next recording without rebuilding");
  Check(addon::control_mask_reuse_stats.checks == gate_before_reset_recovery.checks + 1
            && addon::control_mask_reuse_stats.admitted == gate_before_reset_recovery.admitted + 1,
        "Reset recovery contributes exactly one admitted reuse check");
  std::cout << "CASE location=healthy-queued evidence=GPU+MOCK size=8x8 pending=declined replayable=declined complete-closed=reused resource=retained no_cpu_wait=1\n";

  // Exercise the actual detour's exception boundary, not manual null receipts.
  for (int stage : {0, 1, 2}) {
    const auto before = model_calls;
    addon::ProcessInline(gpu.list.Get(), owner, &parameters);
    Check(model_calls == before + 1, "fault case records actual mask and model");
    auto* mask = addon::worksets.at(key).control_mask;
    Check(SUCCEEDED(gpu.list->Close()), "fault recording Close");
    ComPtr<ID3D12Fence> delayed;
    Check(SUCCEEDED(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&delayed))), "fault GPU gate");
    Check(SUCCEEDED(gpu.queue->Wait(delayed.Get(), 1)), "delay actual fault submission");
    const auto failures_before = injected_failures;
    if (stage < 2) {
      reservation_fault_stage = stage;
      submission::reservation_fault = ReservationFault;
    } else {
      std::lock_guard lock(renodx::addons::dlss5::queue_completion_mutex);
      signal_fault_fence = renodx::addons::dlss5::queue_completions.at(gpu.queue.Get()).fence;
    }
    ID3D12CommandList* lists[]{gpu.list.Get()};
    gpu.queue->ExecuteCommandLists(1, lists);
    submission::reservation_fault = nullptr;
    try {
      Check(injected_failures == failures_before + 1, "production submit fault was reached");
      Check(SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)), "Reset in-flight list without resetting allocator");
      submission::OnCommandListReset(gpu.list.Get());
      submission::PruneCompletedGenerations();
      Check(submission::HasUnprovenSubmissions(), "allocation or Signal fault is a device admission barrier");
      Check(!submission::ResourceReleasable(mask), "failed bookkeeping or Signal retains actual in-flight mask");
      const auto failed_calls = model_calls;
      const auto gate_before_unknown = addon::control_mask_reuse_stats;
      const auto unknown_before = renodx::addons::dlss5::nr_decline_counts[static_cast<size_t>(
          renodx::addons::dlss5::NrDeclineReason::kControlMaskSubmissionsUnproven)].load();
      Check(!addon::ProcessInline(gpu.list.Get(), owner, &parameters) && model_calls == failed_calls,
            "unknown submit proof forbids reusing mask and model");
      Check(addon::control_mask_reuse_stats.checks == gate_before_unknown.checks
                && addon::control_mask_reuse_stats.admitted == gate_before_unknown.admitted
                && renodx::addons::dlss5::nr_decline_counts[static_cast<size_t>(
                    renodx::addons::dlss5::NrDeclineReason::kControlMaskSubmissionsUnproven)].load() == unknown_before + 1,
            "early unknown decline counts once and is excluded from the later reuse denominator");
      addon::hook_point = addon::kHookRender;
      addon::PreSrSwap swap;
      Check(!addon::ProcessInlinePreSR(gpu.list.Get(), owner, &parameters, swap)
                && addon::control_mask_reuse_stats.checks == gate_before_unknown.checks
                && addon::control_mask_reuse_stats.admitted == gate_before_unknown.admitted
                && renodx::addons::dlss5::nr_decline_counts[static_cast<size_t>(
                    renodx::addons::dlss5::NrDeclineReason::kControlMaskSubmissionsUnproven)].load() == unknown_before + 2,
            "PreSR unknown rejection shares the classification and never enters the Upscaled reuse denominator");
      addon::hook_point = addon::kHookUpscaled;
      addon::teardown_pending = true;
      addon::teardown_other_device_waiting = true;
      addon::teardown_pending_since_ns = addon::SteadyNowNs() - 60'000'000'000LL;
      Check(!addon::ServicePendingTeardown() && addon::worksets.contains(key),
            "actual pending teardown cannot release aged unfinished work");
    } catch (...) { delayed->Signal(1); wait_real(); throw; }
    delayed->Signal(1);
    wait_real();
    // Test fence proves physical safety only: the production receipt stays unknown.
    Check(!submission::ResourceReleasable(mask) && !addon::ServicePendingTeardown(),
          "test-owned GPU completion does not repair production unknown proof");
    addon::teardown_pending = false;
    addon::teardown_other_device_waiting = false;
    const auto* replacement_owner = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xC04A8});
    addon::RegisterCreatedFeatureLocked(replacement_owner, addon::kFeatureDlss, &parameters);
    const auto assert_bounded = [&](const NVSDK_NGX_Handle* candidate) {
      const auto creates = addon::worksets_created;
      const auto models = next_handle;
      const auto calls = model_calls;
      const auto dispatches = mask_dispatches;
      const auto active = addon::worksets.size();
      const auto retired = addon::retired_final_resources.size();
      const auto retired_models = addon::retired_nr_features.size();
      uint64_t retained_bytes = 0;
      for (const auto& entry : addon::retired_final_resources) retained_bytes += entry.resources.allocated_bytes;
      for (uint32_t attempt = 0; attempt != 16; ++attempt) {
        ++addon::present_generation;
        // Turning the option off cannot make the old unknown work disappear.
        addon::control_mask_enabled = (attempt % 2 == 0);
        parameters.Set(NVSDK_NGX_Parameter_Output, (attempt % 2 == 0 ? output.Get() : output_b.Get()));
        addon::ProcessInline(gpu.list.Get(), candidate, &parameters);
      }
      addon::control_mask_enabled = true;
      parameters.Set(NVSDK_NGX_Parameter_Output, output.Get());
      uint64_t final_bytes = 0;
      for (const auto& entry : addon::retired_final_resources) final_bytes += entry.resources.allocated_bytes;
      Check(addon::worksets_created == creates && next_handle == models && model_calls == calls
                && mask_dispatches == dispatches && addon::worksets.size() == active
                && addon::retired_final_resources.size() == retired
                && addon::retired_nr_features.size() == retired_models && final_bytes == retained_bytes,
            "unknown submission blocks device-wide Owner/output/option churn without new GPU resources");
    };
    assert_bounded(replacement_owner);
    addon::ForgetSourceHandleLocked(owner);
    addon::RegisterCreatedFeatureLocked(owner, addon::kFeatureDlss, &parameters);
    Check(!submission::ResourceReleasable(mask), "source release retains unknown mask");
    assert_bounded(owner);
    assert_bounded(replacement_owner);
    const auto retained_count = addon::retired_final_resources.size();
    Check(addon::EnsureWorkset(gpu.device.Get(), replacement_owner, output_b.Get(), 8, 8, 8, 8,
              DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1, 0, false, nullptr, nullptr, true) == nullptr
              && addon::retired_final_resources.size() == retained_count,
          "direct workset allocation cannot bypass retained unknown submission");
    std::cout << "CASE location=unknown-owner-churn stage=" << stage
              << " evidence=MOCK+GPU size=8x8 attempts=48 new_resources=0 resource=retained"
                 " recovery=requires-production-proof-or-terminal-device-lifecycle\n";
    // Explicit isolated fixture reset after real GPU completion, never a product recovery.
    submission::ResetAll();
    addon::RetireWorkset(key);
    addon::ReleaseNrSlot(addon::features.at(owner), 0, true);
    addon::DrainRetiredResources(true);
    renodx::addons::dlss5::ReleaseQueueCompletions();
    Check(SUCCEEDED(gpu.list->Close()) && SUCCEEDED(gpu.allocator->Reset())
              && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)), "safe fixture reset");
    mature();
    std::cout << "CASE location=" << (stage < 2 ? "submission-reservation-allocation" : "post-execute-signal")
              << " stage=" << stage << " evidence=MOCK+GPU size=8x8 resource=retained proof=unknown recovery=isolated-fixture-reset-after-independent-fence\n";
  }
  // Keep a real completed recording replayable: neither teardown entry nor
  // a new runtime may discard it just because another device is waiting.
  namespace timers = renodx::addons::dlss5::gpu_timers;
  Check(timers::Begin(gpu.list.Get(), gpu.device.Get(), "postSR", 8, 8, 0, 1, false), "removal completion-only sample");
  addon::ProcessInline(gpu.list.Get(), owner, &parameters);
  timers::End(gpu.list.Get());
  Check(SUCCEEDED(gpu.list->Close()), "replayable removal recording Close");
  ID3D12CommandList* removal_lists[]{gpu.list.Get()};
  gpu.queue->ExecuteCommandLists(1, removal_lists);
  wait_real();
  addon::teardown_pending = true;
  addon::teardown_other_device_waiting = true;
  addon::teardown_pending_since_ns = addon::SteadyNowNs() - 60'000'000'000LL;
  Check(!addon::ServicePendingTeardown() && !addon::Shutdown(false)
            && addon::worksets.contains(key), "all teardown entries retain completed replayable list");
  // D3D12CreateDevice may return the existing device on the same adapter.
  // Use the other already-supported adapter to obtain a distinct generation.
  std::unique_ptr<Gpu> recovery_gpu;
  if (safety) recovery_gpu = std::make_unique<Gpu>(!hardware);
  if (recovery_gpu != nullptr) {
  auto& recovered = *recovery_gpu;
  Check(recovered.device.Get() != gpu.device.Get(), "recovery uses a genuinely distinct device");
  addon::direct_api.init = InitializeRuntime;
  addon::direct_api.shutdown = ShutdownRuntime;
  Check(!addon::EnsureDirectRuntime(recovered.list.Get()) && runtime_initializations == 0,
        "runtime switch retains replayable old work despite new device");
  addon::teardown_pending = false;
  addon::teardown_other_device_waiting = false;
  std::cout << "CASE location=replayable-teardown-and-runtime-switch evidence=GPU+MOCK size=8x8 resource=retained recovery=close-recording\n";
  // Removal substitutes only API observations after independent real completion.
  {
    namespace leases = renodx::addons::dlss5;
    leases::GpuLease unknown;
    Check(leases::QueryGpuLease(unknown) == leases::GpuLeaseState::kPending, "unknown lease stays pending");
    leases::GpuLease removed;
    removed.coverage_complete = true;
    removed.waits.emplace_back(leases::FenceRef(gpu.fence.Get()), gpu.serial);
    auto* production_fence = leases::queue_completions.at(gpu.queue.Get()).fence;
    removed_fence = production_fence;
    removed_device = gpu.device.Get();
    removed.waits.clear();
    removed.waits.emplace_back(leases::FenceRef(production_fence), 1);
    Check(leases::QueryGpuLease(removed) == leases::GpuLeaseState::kDeviceRemoved
              && !leases::GpuLeaseCompleted(removed), "removed fence cannot prove successful execution");
    const auto calls = model_calls;
    Check(!addon::ProcessInline(gpu.list.Get(), owner, &parameters) && model_calls == calls,
          "removed device cannot evaluate again");
    addon::RetireWorkset(key);
    addon::ReleaseNrSlot(addon::features.at(owner), 0, true);
    addon::DrainRetiredResources(true);
    timers::Poll(true, false);
    Check(!timers::execution_stats[1].valid
              && timers::execution_stats[1].state == timers::SampleState::DEVICE_REMOVED,
          "production completion consumer rejects removed sample instead of successful readback");
    Check(addon::retired_final_resources.empty() && addon::retired_nr_features.empty()
              && addon::TeardownProof() == submission::RecordedWork::kComplete,
          "production removal retires dead-device resources without requiring old-list Reset");
    Check(addon::EnsureDirectRuntime(recovered.list.Get()) && addon::direct_device == recovered.device.Get()
              && runtime_initializations == 1 && runtime_shutdowns == 1,
          "production runtime switch initializes a distinct legal device generation");
    removed_fence = nullptr;
    removed_device = nullptr;
    auto* rebuilt = addon::EnsureWorkset(recovered.device.Get(), owner, output.Get(), 8, 8, 8, 8,
        DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1, 0, false, nullptr, nullptr, true);
    Check(rebuilt != nullptr && addon::EnsureControlMaskPipeline(recovered.device.Get()),
          "new device rebuilds actual mask resources and PSO");
    addon::TrackWorksetRecording(recovered.list.Get(), *rebuilt);
    ID3D12DescriptorHeap* heaps[]{rebuilt->descriptors};
    recovered.list->SetDescriptorHeaps(1, heaps);
    recovered.list->SetComputeRootSignature(addon::codec_pipeline.root_signature);
    recovered.list->SetPipelineState(addon::codec_pipeline.control_mask);
    recovered.list->SetComputeRootDescriptorTable(1, addon::DescriptorHeapSlot(
        rebuilt->descriptors->GetGPUDescriptorHandleForHeapStart(), addon::kDescriptorControlMaskUav, rebuilt->descriptor_size));
    const std::array<uint32_t, 8> constants{8, 8, addon::kControlMaskNeutral, 4, 4, 0, 0, 0};
    recovered.list->SetComputeRoot32BitConstants(2, 8, constants.data(), 0);
    recovered.list->Dispatch(1, 1, 1);
    recovered.Transition(rebuilt->control_mask, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    auto transfer = recovered.CreateTextureTransfer(rebuilt->control_mask);
    recovered.RecordTextureReadback(rebuilt->control_mask, transfer);
    recovered.Submit();
    const auto pixels = recovered.ReadTexture(transfer);
    Check(std::all_of(pixels.begin(), pixels.end(), [](uint8_t value) { return value == 255; }),
          "new device generation dispatch completes with neutral mask bytes");
  }
  std::cout << "CASE location=device-removal evidence=MOCK+GPU size=8x8 actual_gpu=already-completed old_device_evaluates=0 old_device_reset=0 recovery=distinct-adapter-device-new-PSO-resource-dispatch runtime=MOCK\n";
  } else {
    // The ordinary WARP case remains usable on machines without NVIDIA.
    Check(SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)), "close replay lifetime in WARP-only case");
    submission::OnCommandListReset(gpu.list.Get());
    addon::teardown_pending = false;
    addon::teardown_other_device_waiting = false;
    std::cout << "CASE location=device-removal evidence=NOT_RUN recovery=requires-explicit-safety-case-and-second-adapter\n";
  }
  Check(DetourTransactionBegin() == NO_ERROR && DetourUpdateThread(GetCurrentThread()) == NO_ERROR
            && DetourDetach(reinterpret_cast<PVOID*>(&real_dispatch), reinterpret_cast<PVOID>(ObserveDispatch)) == NO_ERROR
            && DetourDetach(reinterpret_cast<PVOID*>(&real_pipeline), reinterpret_cast<PVOID>(ObservePipeline)) == NO_ERROR
            && DetourDetach(reinterpret_cast<PVOID*>(&real_allocate), reinterpret_cast<PVOID>(Allocate)) == NO_ERROR
            && DetourDetach(reinterpret_cast<PVOID*>(&real_root), reinterpret_cast<PVOID>(CreateRoot)) == NO_ERROR
            && DetourDetach(reinterpret_cast<PVOID*>(&real_pso), reinterpret_cast<PVOID>(CreatePso)) == NO_ERROR
            && DetourDetach(reinterpret_cast<PVOID*>(&real_heap), reinterpret_cast<PVOID>(CreateHeap)) == NO_ERROR
            && DetourDetach(reinterpret_cast<PVOID*>(&real_signal), reinterpret_cast<PVOID>(Signal)) == NO_ERROR
            && DetourDetach(reinterpret_cast<PVOID*>(&real_completed), reinterpret_cast<PVOID>(Completed)) == NO_ERROR
            && DetourDetach(reinterpret_cast<PVOID*>(&real_removed), reinterpret_cast<PVOID>(RemovedReason)) == NO_ERROR
            && DetourTransactionCommit() == NO_ERROR, "ControlMask observers detached");
  std::cout << "PASS ControlMask ProcessInline default-off dispatch, allocation/NGX failure and no-retry contracts\n";
}

std::vector<uint8_t> ReadPixels(Gpu& gpu, ID3D12Resource* texture) {
  const auto desc = texture->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  uint64_t bytes = 0;
  gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  auto readback = gpu.Buffer(bytes, D3D12_HEAP_TYPE_READBACK);
  gpu.Transition(texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  const D3D12_TEXTURE_COPY_LOCATION source{.pResource = texture,
      .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  const D3D12_TEXTURE_COPY_LOCATION destination{.pResource = readback.Get(),
      .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = footprint};
  gpu.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
  gpu.Transition(texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  gpu.Submit();
  uint8_t* mapped = nullptr;
  Check(SUCCEEDED(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped))), "ControlMask readback map");
  std::vector<uint8_t> pixels(static_cast<size_t>(desc.Width) * desc.Height * 4);
  for (UINT y = 0; y < desc.Height; ++y) {
    std::memcpy(pixels.data() + static_cast<size_t>(y) * desc.Width * 4,
        mapped + footprint.Offset + static_cast<size_t>(y) * footprint.Footprint.RowPitch,
        static_cast<size_t>(desc.Width) * 4);
  }
  readback->Unmap(0, nullptr);
  return pixels;
}

void Dispatch(Gpu& gpu, ID3D12Resource* texture, ID3D12DescriptorHeap* heap,
              uint32_t mode, uint32_t rect_width, uint32_t rect_height,
              float red, float green, float blue) {
  ID3D12DescriptorHeap* heaps[]{heap};
  gpu.list->SetDescriptorHeaps(1, heaps);
  gpu.list->SetComputeRootSignature(addon::codec_pipeline.root_signature);
  gpu.list->SetPipelineState(addon::codec_pipeline.control_mask);
  auto uav = heap->GetGPUDescriptorHandleForHeapStart();
  uav.ptr += static_cast<UINT64>(addon::kDescriptorControlMaskUav)
      * gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  gpu.list->SetComputeRootDescriptorTable(1, uav);
  const std::array<uint32_t, 8> constants{
      static_cast<uint32_t>(texture->GetDesc().Width), texture->GetDesc().Height,
      mode, rect_width, rect_height,
      std::bit_cast<uint32_t>(red), std::bit_cast<uint32_t>(green), std::bit_cast<uint32_t>(blue)};
  gpu.list->SetComputeRoot32BitConstants(2, static_cast<UINT>(constants.size()), constants.data(), 0);
  gpu.list->Dispatch((static_cast<UINT>(texture->GetDesc().Width) + 15) / 16,
                    (texture->GetDesc().Height + 15) / 16, 1);
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  barrier.UAV.pResource = texture;
  gpu.list->ResourceBarrier(1, &barrier);
}

void RunJitterBoundaryContract() {
  renodx::addons::dlss5::bridge::OwnedParameters parameters;
  addon::FeatureState feature{};
  auto& slot = feature.slots[0];
  addon::FinalResources resources{};
  slot.parameters = &parameters;
  struct JitterCase {
    const char* name;
    bool source_x;
    bool source_y;
    float x;
    float y;
  };
  bool contract_ok = true;
  bool ever_present = false;
  // Reuse one NR block without Reset, as consecutive evaluations do.
  for (const auto& test : std::array{
           JitterCase{"never-present", false, false, 0.f, 0.f},
           JitterCase{"never-partial-x", true, false, 0.25f, 0.f},
           JitterCase{"never-partial-y", false, true, 0.f, -0.5f},
           JitterCase{"present", true, true, 0.375f, -0.25f},
           JitterCase{"absent", false, false, 0.f, 0.f},
           JitterCase{"absent-again", false, false, 0.f, 0.f},
           JitterCase{"partial-source-x", true, false, -0.125f, 0.f},
           JitterCase{"partial-source-x-again", true, false, 0.75f, 0.f},
           JitterCase{"partial-source-y", false, true, 0.f, 0.625f},
           JitterCase{"explicit-zero", true, true, 0.f, 0.f},
           JitterCase{"absent-after-zero", false, false, 0.f, 0.f},
           JitterCase{"present-again", true, true, -0.625f, 0.125f},
           JitterCase{"present-to-partial-x", true, false, 0.25f, 0.f},
           JitterCase{"present-before-partial-y", true, true, 0.375f, -0.5f},
           JitterCase{"present-to-partial-y", false, true, 0.f, -0.25f}}) {
    renodx::addons::dlss5::bridge::OwnedParameters source;
    if (test.source_x) {
      source.Set(NVSDK_NGX_Parameter_Jitter_Offset_X, test.x);
    }
    if (test.source_y) {
      source.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, test.y);
    }
    const auto current = addon::DeriveFeatureState(&source);
    addon::EvaluationContract frame{};
    frame.has_jitter = current.has_jitter;
    frame.jitter_x = current.jitter_x;
    frame.jitter_y = current.jitter_y;
    addon::SetEvaluationParameters(feature, slot, 0, 0, resources, frame, 8, 8, 8, 8, false, 0);
    float x = 0.f;
    float y = 0.f;
    const bool has_x = NVSDK_NGX_SUCCEED(parameters.Get("DLSSNR.JitterOffsetX", &x));
    const bool has_y = NVSDK_NGX_SUCCEED(parameters.Get("DLSSNR.JitterOffsetY", &y));
    bool stage_ok = current.has_jitter == (test.source_x && test.source_y)
        && slot.jitter_bound == current.has_jitter;
    if (current.has_jitter) {
      stage_ok &= has_x && has_y && x == test.x && y == test.y;
      ever_present = true;
    } else if (!ever_present) {
      stage_ok &= !has_x && !has_y;
    } else {
      stage_ok &= (!has_x || x == 0.f) && (!has_y || y == 0.f);
    }
    std::cout << "JITTER evidence=MOCK stage=" << test.name << " has_jitter=" << current.has_jitter
              << " has_x=" << has_x << " x=" << x << " has_y=" << has_y << " y=" << y
              << " contract=" << (stage_ok ? "PASS" : "FAIL") << '\n';
    contract_ok &= stage_ok;
  }
  // Mask binding and jitter belong to each parameter slot independently.
  renodx::addons::dlss5::bridge::OwnedParameters other_parameters;
  auto& other_slot = feature.slots[1];
  other_slot.parameters = &other_parameters;
  addon::EvaluationContract frame{};
  addon::SetEvaluationParameters(feature, other_slot, 1, 0, resources, frame, 8, 8, 8, 8, true, 0);
  float x = 0.f;
  float y = 0.f;
  const bool other_never_present = !NVSDK_NGX_SUCCEED(other_parameters.Get("DLSSNR.JitterOffsetX", &x))
      && !NVSDK_NGX_SUCCEED(other_parameters.Get("DLSSNR.JitterOffsetY", &y));
  frame.has_jitter = true;
  frame.jitter_x = 0.5f;
  frame.jitter_y = -0.75f;
  addon::SetEvaluationParameters(feature, other_slot, 1, 0, resources, frame, 8, 8, 8, 8, true, 0);
  frame.has_jitter = false;
  addon::SetEvaluationParameters(feature, slot, 0, 0, resources, frame, 8, 8, 8, 8, false, 0);
  const bool isolated = other_never_present && other_slot.control_mask_bound && !slot.control_mask_bound
      && other_slot.jitter_bound && !slot.jitter_bound
      && NVSDK_NGX_SUCCEED(other_parameters.Get("DLSSNR.JitterOffsetX", &x)) && x == 0.5f
      && NVSDK_NGX_SUCCEED(other_parameters.Get("DLSSNR.JitterOffsetY", &y)) && y == -0.75f;
  std::cout << "JITTER evidence=MOCK stage=mask-slot-isolation contract=" << (isolated ? "PASS" : "FAIL") << '\n';
  contract_ok &= isolated;
  // The ordinary release destroys its owned block. The empty release must
  // also discard binding state before a fresh block is attached to that slot.
  const auto saved_destroy = addon::core_destroy_parameters;
  addon::core_destroy_parameters = renodx::addons::dlss5::bridge::DestroyOwnedParameters;
  for (const bool empty_release : {false, true}) {
    auto& released_slot = feature.slots[2];
    Check(NVSDK_NGX_SUCCEED(renodx::addons::dlss5::bridge::AllocateOwnedParameters(&released_slot.parameters)),
          "allocate jitter release fixture");
    frame.has_jitter = true;
    addon::SetEvaluationParameters(feature, released_slot, 2, 0, resources, frame, 8, 8, 8, 8, false, 0);
    if (empty_release) {
      renodx::addons::dlss5::bridge::DestroyOwnedParameters(released_slot.parameters);
      released_slot.parameters = nullptr;
    }
    addon::ReleaseNrSlot(feature, 2, false);
    const bool released_clean = !released_slot.jitter_bound && released_slot.parameters == nullptr;
    Check(NVSDK_NGX_SUCCEED(renodx::addons::dlss5::bridge::AllocateOwnedParameters(&released_slot.parameters)),
          "allocate fresh jitter release fixture");
    frame.has_jitter = false;
    addon::SetEvaluationParameters(feature, released_slot, 2, 0, resources, frame, 8, 8, 8, 8, false, 0);
    const bool fresh_unset = released_clean && !released_slot.jitter_bound
        && !NVSDK_NGX_SUCCEED(released_slot.parameters->Get("DLSSNR.JitterOffsetX", &x))
        && !NVSDK_NGX_SUCCEED(released_slot.parameters->Get("DLSSNR.JitterOffsetY", &y));
    std::cout << "JITTER evidence=MOCK stage=" << (empty_release ? "empty-release" : "normal-release")
              << " contract=" << (fresh_unset ? "PASS" : "FAIL") << '\n';
    contract_ok &= fresh_unset;
    addon::ReleaseNrSlot(feature, 2, false);
  }
  addon::core_destroy_parameters = saved_destroy;
  other_slot.parameters = nullptr;
  Check(contract_ok, "reused NR parameter block must not retain nonzero jitter after source becomes absent or partial");
}

int RunSafety(bool hardware, bool safety = true) {
  try {
    RunJitterBoundaryContract();
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    Gpu gpu(hardware);
    RunBorrowedCore(gpu, hardware, safety);
    ComPtr<ID3D12InfoQueue> info;
    gpu.device.As(&info);
    std::cout << "ControlMask WARP debug_layer=" << (debug != nullptr) << '\n';
    Check(addon::InstallQueueCompletionHooks(gpu.queue.Get()), "ControlMask queue completion hooks");
    Check(addon::InstallCommandListStateHooks(gpu.list.Get()), "ControlMask command-list state hooks");
    Check(addon::EnsureControlMaskPipeline(gpu.device.Get()), "production ControlMask shader pipeline creates on WARP");
    const D3D12_HEAP_PROPERTIES properties{.Type = D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_RESOURCE_DESC texture_desc{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Width = 8, .Height = 8, .DepthOrArraySize = 1, .MipLevels = 1,
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM, .SampleDesc = {1, 0},
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    ComPtr<ID3D12Resource> texture;
    Check(SUCCEEDED(gpu.device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE,
        &texture_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&texture))), "ControlMask RGBA8 UAV allocation");
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = addon::kDescriptorHeapSize,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    ComPtr<ID3D12DescriptorHeap> heap;
    Check(SUCCEEDED(gpu.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap))), "ControlMask workset descriptor heap");
    D3D12_UNORDERED_ACCESS_VIEW_DESC view{.Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D};
    auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(addon::kDescriptorControlMaskUav)
        * gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    gpu.device->CreateUnorderedAccessView(texture.Get(), nullptr, &view, cpu);

    Dispatch(gpu, texture.Get(), heap.Get(), addon::kControlMaskRgb, 0, 0, 0.25f, 0.5f, 0.75f);
    auto pixels = ReadPixels(gpu, texture.Get());
    auto pixel = [&](uint32_t x, uint32_t y) { return pixels.data() + (y * 8 + x) * 4; };
    Check(pixel(7, 7)[0] == 64 && (pixel(7, 7)[1] == 128 || pixel(7, 7)[1] == 127)
              && pixel(7, 7)[2] == 191 && pixel(7, 7)[3] == 255,
          "fixed RGB mask writes expected RGBA8 texels");
    Dispatch(gpu, texture.Get(), heap.Get(), addon::kControlMaskNeutral, 0, 0, 0.f, 0.f, 0.f);
    pixels = ReadPixels(gpu, texture.Get());
    Check(pixel(0, 0)[0] == 255 && pixel(0, 0)[1] == 255
              && pixel(0, 0)[2] == 255 && pixel(0, 0)[3] == 255,
          "neutral mask is full one in all channels");
    Dispatch(gpu, texture.Get(), heap.Get(), addon::kControlMaskRect, 4, 4, 1.f, 0.5f, 0.f);
    pixels = ReadPixels(gpu, texture.Get());
    Check(pixel(1, 1)[0] == 0 && pixel(1, 1)[1] == 0
              && pixel(1, 1)[2] == 0 && pixel(1, 1)[3] == 255,
          "local test rectangle is zero RGB with opaque alpha");
    Check(pixel(6, 6)[0] == 255 && (pixel(6, 6)[1] == 128 || pixel(6, 6)[1] == 127)
              && pixel(6, 6)[2] == 0 && pixel(6, 6)[3] == 255,
          "pixels outside test rectangle retain fixed RGB");

    for (const auto& size : {std::array<uint32_t, 2>{8, 8}, {19, 17}, {1, 3}, {3, 1}}) {
      auto probe_desc = texture_desc;
      probe_desc.Width = size[0];
      probe_desc.Height = size[1];
      ComPtr<ID3D12Resource> probe;
      Check(SUCCEEDED(gpu.device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE,
          &probe_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
          IID_PPV_ARGS(&probe))), "ControlMask boundary probe allocation");
      gpu.device->CreateUnorderedAccessView(probe.Get(), nullptr, &view, cpu);
      for (uint32_t mode : {addon::kControlMaskRgb, addon::kControlMaskNeutral, addon::kControlMaskRect,
                            addon::kControlMaskGreenRect, addon::kControlMaskBlueRect}) {
        for (const auto& rgb : {std::array<float, 3>{0.f, 0.f, 0.f}, {0.25f, 0.5f, 0.75f}}) {
          // New probes derive their half-size region from the texture, ignoring legacy rectangle constants.
          Dispatch(gpu, probe.Get(), heap.Get(), mode, 1, 1, rgb[0], rgb[1], rgb[2]);
          const auto probe_pixels = ReadPixels(gpu, probe.Get());
          const std::array<uint8_t, 4> fixed_rgb = (rgb[0] == 0.f
              ? std::array<uint8_t, 4>{0, 0, 0, 255}
              : std::array<uint8_t, 4>{64, 128, 191, 255});
          for (uint32_t y = 0; y < size[1]; ++y) {
            for (uint32_t x = 0; x < size[0]; ++x) {
              auto expected = fixed_rgb;
              if (mode == addon::kControlMaskNeutral) {
                expected = {255, 255, 255, 255};
              } else if (mode == addon::kControlMaskRect && x == 0 && y == 0) {
                expected = {0, 0, 0, 255};
              } else if (mode == addon::kControlMaskGreenRect || mode == addon::kControlMaskBlueRect) {
                expected = {255, 255, 255, 255};
                if (x < size[0] / 2 && y < size[1] / 2) {
                  expected[mode == addon::kControlMaskGreenRect ? 1 : 2] = 0;
                }
              }
              const size_t offset = (static_cast<size_t>(y) * size[0] + x) * 4;
              bool matches = true;
              for (size_t channel = 0; channel < 4; ++channel) {
                const auto actual = probe_pixels[offset + channel];
                matches &= actual == expected[channel] || (expected[channel] == 128 && actual == 127);
              }
              Check(matches,
                    "all mask texels match channel, origin, half-open boundary, RGB independence and alpha contracts");
            }
          }
        }
      }
    }
    gpu.device->CreateUnorderedAccessView(texture.Get(), nullptr, &view, cpu);

    const auto* owner = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xC04A5});
    const auto* disabled_owner = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xC04A4});
    const addon::WorksetKey workset_key{owner, texture.Get(), false};
    const addon::WorksetKey disabled_key{disabled_owner, texture.Get(), false};
    auto* disabled = addon::EnsureWorkset(gpu.device.Get(), disabled_owner, texture.Get(), 8, 8, 8, 8,
        DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1, 0, false);
    Check(disabled != nullptr && disabled->control_mask == nullptr
              && !addon::control_mask_enabled.load(),
          "default-off workset allocates no ControlMask resource");
    addon::RetireWorkset(disabled_key);
    addon::DrainRetiredResources(true);
    auto* first = addon::EnsureWorkset(gpu.device.Get(), owner, texture.Get(), 8, 8, 8, 8,
        DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1, 0, false, nullptr, nullptr, true);
    Check(first != nullptr && first->control_mask != nullptr
              && first->allocated_bytes >= 8u * 8u * 4u,
          "enabled workset owns and accounts for its RGBA8 mask");
    auto* first_mask = first->control_mask;
    addon::TrackWorksetRecording(gpu.list.Get(), *first);
    auto* resized = addon::EnsureWorkset(gpu.device.Get(), owner, texture.Get(), 4, 4, 8, 8,
        DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1, 0, false, nullptr, nullptr, true);
    Check(resized != nullptr && resized->control_mask != nullptr
              && resized->control_mask != first_mask,
          "workset size change replaces the mask resource");
    addon::DrainRetiredResources(true);
    Check(std::any_of(addon::retired_final_resources.begin(), addon::retired_final_resources.end(),
              [&](const auto& retired) { return retired.resources.control_mask == first_mask; }),
          "recorded but unsubmitted old mask cannot retire early");
    gpu.Submit();
    addon::DrainRetiredResources(true);
    Check(std::none_of(addon::retired_final_resources.begin(), addon::retired_final_resources.end(),
              [&](const auto& retired) { return retired.resources.control_mask == first_mask; }),
          "old mask retires only after its recorded submission completes");
    addon::RetireWorkset(workset_key);
    addon::DrainRetiredResources(true);

    renodx::addons::dlss5::bridge::OwnedParameters parameters;
    addon::FeatureState feature{};
    addon::NrFeatureSlot slot{};
    addon::FinalResources resources{};
    addon::EvaluationContract frame{};
    resources.control_mask = texture.Get();
    const bool old_auto_mask = addon::use_auto_mask.exchange(true);
    slot.parameters = &parameters;
    for (uint32_t blend = 0; blend < 4; ++blend) {
      addon::SetEvaluationParameters(feature, slot, 0, 0, resources, frame,
                                     8, 8, 8, 8, true, blend);
      Check(addon::GetD3D12Resource(&parameters, "DLSSNR.ControlMask") == texture.Get()
                && addon::GetUInt(&parameters, "DLSSNR.ControlMaskSubrectBaseX") == 0
                && addon::GetUInt(&parameters, "DLSSNR.ControlMaskSubrectBaseY") == 0
                && addon::GetUInt(&parameters, "DLSSNR.ControlMaskSubrectWidth") == 8
                && addon::GetUInt(&parameters, "DLSSNR.ControlMaskSubrectHeight") == 8
                && addon::GetUInt(&parameters, "DLSSNR.MaskBlendMode") == blend
                && addon::GetInt(&parameters, "DLSSNR.UseAutoMask") == 1,
            "NGX block binds mask/subrect/blend mode and preserves AutoMask");
    }
    addon::SetEvaluationParameters(feature, slot, 0, 0, resources, frame, 8, 8, 8, 8, false, 0);
    Check(addon::GetD3D12Resource(&parameters, "DLSSNR.ControlMask") == nullptr
              && addon::GetUInt(&parameters, "DLSSNR.ControlMaskSubrectBaseX") == 0
              && addon::GetUInt(&parameters, "DLSSNR.ControlMaskSubrectBaseY") == 0
              && addon::GetUInt(&parameters, "DLSSNR.ControlMaskSubrectWidth") == 0
              && addon::GetUInt(&parameters, "DLSSNR.ControlMaskSubrectHeight") == 0
              && !slot.control_mask_bound,
          "previous mask binding is cleared when the feature is disabled");
    addon::use_auto_mask.store(old_auto_mask);
    feature.slots[1].control_mask_bound = true;
    addon::ReleaseNrSlot(feature, 1, false);
    Check(!feature.slots[1].control_mask_bound,
          "released parameter slot forgets its prior mask binding");
    RunInline(gpu, hardware, safety);
    if (info != nullptr) {
      for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
        SIZE_T bytes = 0;
        info->GetMessage(i, nullptr, &bytes);
        std::vector<uint8_t> buffer(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
        Check(SUCCEEDED(info->GetMessage(i, message, &bytes)), "ControlMask debug message");
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
          std::cerr << message->pDescription << '\n';
          Check(false, "ControlMask D3D12 validation error");
        }
      }
    }
    std::cout << "PASS ControlMask safety adapter=" << (hardware ? "NATIVE_GPU" : "WARP")
              << " model=MOCK; NVIDIA runtime not exercised\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL ControlMask: " << error.what() << std::endl;
    return 1;
  }
}

int Run() { return RunSafety(false, false); }
}  // namespace control_mask_gpu_cases
