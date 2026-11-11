/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <array>
#include <exception>
#include <iostream>
#include <thread>
#include <vector>

namespace adaptive_inline_cases {
namespace addon = renodx::addons::dlss5;

int Run() {
  try {
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    Gpu gpu(false, 16, 16);
    model_gpu = &gpu;
    ComPtr<ID3D12InfoQueue> info;
    gpu.device.As(&info);
    native::enabled = true;
    native::hooks_enabled = true;
    native::dedupe_mode = addon::kDedupePerEvaluate;
    native::stack_passes = 1;
    native::nr_follow_input_res = false;
    native::nr_resolution_scale = 1.f;
    native::codec_mode = 0;
    native::feed_mode = 0;
    native::look_mode = false;
    native::detail_stability = 0;
    native::chained_temporal_history = true;
    native::transfer_strength = 1.f;
    native::color_strength = 1.f;
    native::control_mask_enabled = true;
    native::nr_adaptive_min_scale = .33f;
    native::nr_adaptive_target_fps = 116.f;
    native::max_worksets = 4;
    native::OnNativeFgOptions(false);
    native::adaptive_configuration_settings = native::comparison_ui::CaptureAdaptiveSettings;
    addon::gpu_timers::comparison_settings = native::comparison_ui::CaptureSettings;
    native::direct_api.module = GetModuleHandleW(nullptr);
    native::direct_api.create = CreateModel;
    native::direct_api.evaluate = EvaluateModel;
    native::direct_api.release = ReleaseModel;
    native::direct_load_state = native::DirectLoadState::Ready;
    native::direct_device = gpu.device.Get();
    native::direct_device->AddRef();
    native::direct_device_native = gpu.device.Get();
    native::successful_sr_device = gpu.device.Get();
    native::direct_runtime_requirements.dual_models = true;
    native::direct_runtime_requirements.file_version = "310.9.16.0";
    native::parameter_runtime_ready = true;
    native::core_allocate_parameters = addon::bridge::AllocateOwnedParameters;
    native::core_destroy_parameters = addon::bridge::DestroyOwnedParameters;
    Check(native::InstallQueueCompletionHooks(gpu.queue.Get()), "inline adaptive real queue hooks");
    Check(native::InstallCommandListStateHooks(gpu.list.Get()), "inline adaptive real list hooks");
    const auto texture = [&](DXGI_FORMAT format, bool writable) {
      const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
      const D3D12_RESOURCE_DESC desc{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
          .Width = 16, .Height = 16, .DepthOrArraySize = 1, .MipLevels = 1,
          .Format = format, .SampleDesc = {1, 0},
          .Flags = (writable ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE)};
      ComPtr<ID3D12Resource> resource;
      Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
          &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource))), "inline adaptive texture");
      gpu.Write(resource.Get(), 0);
      gpu.Transition(resource.Get(), D3D12_RESOURCE_STATE_COMMON,
          writable ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : NATIVE_INPUT_STATE);
      return resource;
    };
    std::array colors{texture(DXGI_FORMAT_R8G8B8A8_UNORM, false), texture(DXGI_FORMAT_R8G8B8A8_UNORM, false)};
    std::array outputs{texture(DXGI_FORMAT_R8G8B8A8_UNORM, true), texture(DXGI_FORMAT_R8G8B8A8_UNORM, true)};
    auto motion = texture(DXGI_FORMAT_R16G16_FLOAT, false);
    auto depth = texture(DXGI_FORMAT_R32_FLOAT, false);
    gpu.Submit();
    const auto submit = [&] {
      Check(SUCCEEDED(gpu.list->Close()), "inline adaptive recording close");
      ID3D12CommandList* lists[]{gpu.list.Get()};
      gpu.queue->ExecuteCommandLists(1, lists);
      Check(SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial)), "inline adaptive Fence signal");
      HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      Check(event != nullptr, "inline adaptive Fence event");
      const auto signal = gpu.fence->SetEventOnCompletion(gpu.serial, event);
      const auto wait = (SUCCEEDED(signal) ? WaitForSingleObject(event, 10000) : WAIT_FAILED);
      CloseHandle(event);
      Check(wait == WAIT_OBJECT_0, "inline adaptive bounded GPU completion");
      Check(SUCCEEDED(gpu.allocator->Reset()) && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)),
          "inline adaptive completed recording reset");
      native::DrainRetiredResources(true);
      addon::gpu_timers::Poll(true, false);
      gpu.uploads.clear();
    };
    addon::bridge::OwnedParameters parameters;
    parameters.Set(NVSDK_NGX_Parameter_Depth, depth.Get());
    parameters.Set(NVSDK_NGX_Parameter_MotionVectors, motion.Get());
    parameters.Set(NVSDK_NGX_Parameter_Width, 16u);
    parameters.Set(NVSDK_NGX_Parameter_Height, 16u);
    parameters.Set(NVSDK_NGX_Parameter_OutWidth, 16u);
    parameters.Set(NVSDK_NGX_Parameter_OutHeight, 16u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 16u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 16u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
        static_cast<uint32_t>(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes));
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
    parameters.Set(NVSDK_NGX_Parameter_Reset, 0u);
    for (const auto hook : {native::kHookUpscaled, native::kHookRender}) {
      native::hook_point = hook;
      native::nr_adaptive_enabled = true;
      const auto* handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xADA100} + hook);
      native::RegisterCreatedFeatureLocked(handle, native::kFeatureDlss, &parameters);
      uint32_t frame = 0;
      const auto record = [&] {
        const uint32_t output = frame++ % 2;
        parameters.Set(NVSDK_NGX_Parameter_Color, colors[output].Get());
        parameters.Set(NVSDK_NGX_Parameter_Output, outputs[output].Get());
        bool accepted = false;
        {
          const addon::EvaluateChainScope chain;
          const addon::SourceStageScope source(handle, true);
          if (hook == native::kHookRender) {
            native::PreSrSwap swap;
            accepted = native::ProcessInlinePreSR(gpu.list.Get(), handle, &parameters, swap);
            native::RestorePreSrColor(&parameters, swap);
          } else {
            accepted = native::ProcessInline(gpu.list.Get(), handle, &parameters);
          }
        }
        submit();
        return accepted;
      };
      for (uint32_t warmup = 0; warmup < 32; ++warmup) record();
      auto& feature = native::features.at(handle);
      auto cache = feature.adaptive_cache;
      std::cout << "ADAPTIVE_INLINE hook=" << hook << " outputs=" << (cache ? cache->output_count : 0)
          << " tiers=" << (cache ? cache->count : 0) << " failed=" << feature.adaptive_session->failed
          << " reason=" << addon::adaptive_resolution::ReasonText(feature.adaptive_session->status.reason) << '\n';
      Check(cache != nullptr && cache->count == 2 && cache->output_count == 2,
          "inline A/B rotation shares the original four-workset budget across two tiers");
      native::PollAdaptiveCache(cache.get());
      Check(!cache->failed && !feature.adaptive_session->failed && cache->tiers[0].ready && cache->tiers[1].ready,
          "Render/Upscaled standby tiers complete both output recordings without permanent pause");
      const auto creates = native::nr_features_created;
      feature.adaptive_session->Request(1, native::SteadyNowNs());
      Check(record() && record(), "both rotating outputs accept their prepared lower tier");
      Check(cache->active == 1 && feature.adaptive_session->status.applied[0].width == 5
          && native::nr_features_created == creates,
          "inline lower tier actually applies its requested size without cold Create");
      feature.adaptive_session->Request(0, native::SteadyNowNs());
      Check(record() && record() && cache->active == 0 && native::nr_features_created == creates,
          "inline prepared upper tier restores without cold Create");
      const auto deadline = native::SteadyNowNs() + 5'000'000'000;
      uint64_t serial = 0;
      while (feature.adaptive_session->level == 0 && native::SteadyNowNs() < deadline) {
        const auto now = native::SteadyNowNs();
        {
          std::unique_lock lock(addon::output_framerate::internal::state.mutex);
          auto& estimate = addon::output_framerate::internal::state.stream;
          estimate.previous_ns = now;
          estimate.snapshot = {.available = true, .source = addon::output_framerate::Source::PRESENT,
              .source_generation = 1, .configuration_generation = native::configuration_generation.load(),
              .window_serial = ++serial, .window_begin_ns = now - 500'000'000, .window_end_ns = now,
              .fps = 50., .fg_off = true};
        }
        record();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      std::cout << "ADAPTIVE_INLINE controller hook=" << hook << " level=" << feature.adaptive_session->level
          << " gpu_samples=" << feature.adaptive_session->gpu_count
          << " applied_width=" << feature.adaptive_session->status.applied[0].width
          << " reason=" << addon::adaptive_resolution::ReasonText(feature.adaptive_session->status.reason) << '\n';
      Check(feature.adaptive_session->level == 1 && cache->active == 1
          && feature.adaptive_session->status.applied[0].width == 5 && native::nr_features_created == creates,
          "real GPU samples and mocked low output FPS drive the production inline controller into its ready lower tier");
      {
        std::unique_lock lock(addon::output_framerate::internal::state.mutex);
        addon::output_framerate::internal::state.stream = {};
      }
      native::ForgetSourceHandleLocked(handle);
      submit();
    }
    native::successful_sr_device = nullptr;
    if (info != nullptr) {
      for (UINT64 index = 0; index < info->GetNumStoredMessages(); ++index) {
        SIZE_T bytes = 0;
        info->GetMessage(index, nullptr, &bytes);
        std::vector<uint8_t> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        Check(SUCCEEDED(info->GetMessage(index, message, &bytes)), "inline adaptive debug message");
        Check(message->Severity > D3D12_MESSAGE_SEVERITY_ERROR, message->pDescription);
      }
    }
    std::cout << "PASS adaptive Render/Upscaled two-output preparation and ready-tier switching evidence=WARP+MOCK\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "adaptive-inline: " << error.what() << '\n';
    return 1;
  }
}
}  // namespace adaptive_inline_cases
