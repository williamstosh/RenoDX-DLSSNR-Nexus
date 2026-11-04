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

void RunInline(Gpu& gpu) {
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
  Check(DetourTransactionBegin() == NO_ERROR && DetourUpdateThread(GetCurrentThread()) == NO_ERROR,
        "ControlMask observer transaction");
  Check(DetourAttach(reinterpret_cast<PVOID*>(&real_dispatch), reinterpret_cast<PVOID>(ObserveDispatch)) == NO_ERROR
            && DetourAttach(reinterpret_cast<PVOID*>(&real_pipeline), reinterpret_cast<PVOID>(ObservePipeline)) == NO_ERROR
            && DetourAttach(reinterpret_cast<PVOID*>(&real_allocate), reinterpret_cast<PVOID>(Allocate)) == NO_ERROR
            && DetourTransactionCommit() == NO_ERROR, "ControlMask observers attached");
  const auto record = [&] {
    const auto before = model_calls;
    addon::ProcessInline(gpu.list.Get(), owner, &parameters);
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
  Check(addon::worksets.at({owner, output.Get(), false}).control_mask == nullptr,
        "default-off production path allocates no mask");
  addon::control_mask_enabled = true;
  bool saw_mask = false;
  model_parameter_observer = [&](const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter* block) {
    saw_mask = addon::GetD3D12Resource(block, "DLSSNR.ControlMask") != nullptr;
  };
  mature();
  Check(saw_mask && mask_dispatches > 0, "production Evaluate receives generated mask");
  auto& slot = addon::features.at(owner).slots[0];
  Check(!slot.control_mask_bound
            && addon::GetD3D12Resource(slot.parameters, "DLSSNR.ControlMask") == nullptr,
        "completed Evaluate leaves no stale mask in reusable NGX parameters");
  for (uint32_t mode : {addon::kControlMaskNeutral, addon::kControlMaskRect}) {
    addon::control_mask_mode = mode;
    const auto before = mask_dispatches;
    Check(record() == 1 && mask_dispatches == before + 1 && !slot.control_mask_bound,
          "mode switch has one mask dispatch, one Evaluate and no stale binding");
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
  slot.failed = false;
  mature();
  fail_call = model_calls + 1;
  const auto before_evaluate_failure = stage_execution_cases::Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  gpu.Submit();
  Check(record() == 1, "NGX failure has exactly one Evaluate attempt");
  const auto after_evaluate_failure = stage_execution_cases::Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  gpu.Submit();
  Check(before_evaluate_failure.Pixels() == after_evaluate_failure.Pixels()
            && !addon::features.at(owner).slots[0].control_mask_bound,
        "NGX failure preserves game output and clears the old mask");
  fail_call = 0;
  slot.failed = false;
  mature();
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
  Check(DetourTransactionBegin() == NO_ERROR && DetourUpdateThread(GetCurrentThread()) == NO_ERROR
            && DetourDetach(reinterpret_cast<PVOID*>(&real_dispatch), reinterpret_cast<PVOID>(ObserveDispatch)) == NO_ERROR
            && DetourDetach(reinterpret_cast<PVOID*>(&real_pipeline), reinterpret_cast<PVOID>(ObservePipeline)) == NO_ERROR
            && DetourDetach(reinterpret_cast<PVOID*>(&real_allocate), reinterpret_cast<PVOID>(Allocate)) == NO_ERROR
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
      8, 8, mode, rect_width, rect_height,
      std::bit_cast<uint32_t>(red), std::bit_cast<uint32_t>(green), std::bit_cast<uint32_t>(blue)};
  gpu.list->SetComputeRoot32BitConstants(2, static_cast<UINT>(constants.size()), constants.data(), 0);
  gpu.list->Dispatch(1, 1, 1);
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  barrier.UAV.pResource = texture;
  gpu.list->ResourceBarrier(1, &barrier);
}

int Run() {
  try {
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    Gpu gpu(false);
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
    Check(pixel(7, 7)[0] == 64 && pixel(7, 7)[1] == 128
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
    Check(pixel(6, 6)[0] == 255 && pixel(6, 6)[1] == 128
              && pixel(6, 6)[2] == 0 && pixel(6, 6)[3] == 255,
          "pixels outside test rectangle retain fixed RGB");

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
    RunInline(gpu);
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
    std::cout << "PASS ControlMask WARP shader and NGX parameter binding; NVIDIA runtime not exercised";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL ControlMask: " << error.what() << std::endl;
    return 1;
  }
}
}  // namespace control_mask_gpu_cases
