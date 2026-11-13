/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iostream>
#include <vector>

namespace adaptive_fg_service_cases {
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
    native::enabled.store(true);
    native::hooks_enabled.store(true);
    native::hook_point.store(native::kHookPresent);
    native::present_pre_fg.store(1);
    native::present_guides.store(native::kPresentGuidesRequired);
    native::OnNativeFgOptions(false);
    native::stack_passes.store(2);
    native::pass_stage[0].store(0);
    native::nr_follow_input_res.store(false);
    native::nr_resolution_scale.store(1.f);
    native::pass_resolution_mode[0].store(0);
    native::pass_follow[0].store(false);
    native::pass_preset[0].store(2);
    native::codec_mode.store(0);
    native::feed_mode.store(0);
    native::look_mode.store(false);
    native::detail_stability.store(0);
    native::gpu_timers_enabled.store(true);
    native::chained_temporal_history.store(true);
    native::transfer_strength.store(1.f);
    native::color_strength.store(1.f);
    native::intensity.store(.5f);
    native::pass_intensity[0].store(.75f);
    native::pass_transfer_strength[0].store(1.f);
    native::pass_color_strength[0].store(1.f);
    native::control_mask_enabled.store(true);
    native::nr_adaptive_min_scale.store(.33f);
    native::nr_adaptive_enabled.store(false);
    native::max_worksets.store(4);
    native::adaptive_configuration_settings = native::comparison_ui::CaptureAdaptiveSettings;
    native::direct_api.module = GetModuleHandleW(nullptr);
    native::direct_api.create = CreateModel;
    native::direct_api.evaluate = EvaluateModel;
    native::direct_api.release = ReleaseModel;
    native::direct_load_state = native::DirectLoadState::Ready;
    native::direct_device = gpu.device.Get();
    native::direct_device->AddRef();
    native::direct_device_native = gpu.device.Get();
    native::direct_runtime_requirements.dual_models = true;
    native::direct_runtime_requirements.file_version = "310.9.16.0";
    native::parameter_runtime_ready = true;
    native::core_allocate_parameters = addon::bridge::AllocateOwnedParameters;
    native::core_destroy_parameters = addon::bridge::DestroyOwnedParameters;
    uint32_t masked_calls = 0;
    uint32_t active_successes = 0;
    uint32_t active_failures = 0;
    uint32_t preparation_calls = 0;
    uint32_t preparation_failures = 0;
    model_parameter_observer = [&](const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter* parameters) {
      Check(native::GetD3D12Resource(parameters, "DLSSNR.ControlMask") != nullptr,
          "every model call receives the production GPU ControlMask");
      ++masked_calls;
      const bool failing = model_calls + 1 == fail_call;
      if (native::preparing_adaptive_frame != nullptr) {
        ++preparation_calls;
        preparation_failures += failing;
      } else if (failing) {
        ++active_failures;
      } else {
        ++active_successes;
      }
    };
    std::cout << "ADAPTIVE_SERVICE evidence=WARP+MOCK Runtime_contract=mock_310.9.16.0 native_NGX=not_run\n";
    Check(native::InstallQueueCompletionHooks(gpu.queue.Get()), "adaptive service real queue hooks");
    Check(native::InstallCommandListStateHooks(gpu.list.Get()), "adaptive service real list hooks");
    const auto texture = [&](DXGI_FORMAT format) {
      const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
      const D3D12_RESOURCE_DESC desc{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
          .Width = 16, .Height = 16, .DepthOrArraySize = 1, .MipLevels = 1,
          .Format = format, .SampleDesc = {1, 0}};
      ComPtr<ID3D12Resource> resource;
      Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
          &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource))), "adaptive service texture");
      gpu.Write(resource.Get(), 0);
      gpu.Transition(resource.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
      return resource;
    };
    auto color = texture(DXGI_FORMAT_R8G8B8A8_UNORM);
    auto motion = texture(DXGI_FORMAT_R16G16_FLOAT);
    auto depth = texture(DXGI_FORMAT_R32_FLOAT);
    gpu.Submit();
    fg::FrameInput input;
    input.context = 0xADAF6;
    input.epoch = 1;
    input.queue = gpu.queue.Get();
    input.backbuffer = color.Get();
    input.backbuffer_state = NATIVE_INPUT_STATE;
    input.depth = depth.Get();
    input.depth_state = NATIVE_INPUT_STATE;
    input.motion = motion.Get();
    input.motion_state = NATIVE_INPUT_STATE;
    input.color_width = input.color_height = input.guide_width = input.guide_height = 16;
    input.motion_scale_x = input.motion_scale_y = 16.f;
    input.has_constants = true;
    input.fg_enabled = true;
    input.color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    const auto service = [&] {
      ++input.application_frame;
      const auto before_source = native::SourceSuccessfulEvaluations();
      const auto before_active = active_successes;
      const auto before_failed = active_failures;
      const auto result = native::fg_service::Service(input);
      const uint64_t completed_active = (active_successes > before_active && active_failures == before_failed ? 1u : 0u);
      Check(native::SourceSuccessfulEvaluations() == before_source + completed_active,
          "Service source successes count only the active output, excluding every standby Evaluate");
      gpu.Submit();
      native::DrainRetiredResources(true);
      auto& stream = native::fg_service::streams.at({false, input.context});
      std::cout << "ADAPTIVE_SERVICE frame=" << input.application_frame << " result=" << static_cast<int>(result)
          << " calls=" << model_calls << " failed=" << (stream.adaptive_session && stream.adaptive_session->failed)
          << " source_successes=" << native::SourceSuccessfulEvaluations() << " preparation_calls=" << preparation_calls
          << " reason=" << (stream.adaptive_session ? addon::adaptive_resolution::ReasonText(stream.adaptive_session->status.reason) : "Off")
          << '\n';
      return result;
    };
    for (uint32_t frame = 0; frame < 8; ++frame) service();
    Check(service() == fg::Result::SUBMITTED, "manual two-Pass single Backbuffer matures");
    const auto* occupied_owner = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xADAF7});
    Check(native::EnsureWorkset(gpu.device.Get(), occupied_owner, color.Get(), 16, 16, 16, 16,
        DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1.f, 0, false) != nullptr,
        "an unrelated stream can temporarily occupy one real workset");
    const auto before_occupied_features = native::nr_features_created;
    const auto before_occupied_worksets = native::worksets_created;
    native::nr_adaptive_enabled.store(true);
    service();
    auto& stream = native::fg_service::streams.at({false, input.context});
    auto cache = native::features.at(stream.roles[0].Handle()).adaptive_cache;
    Check(cache != nullptr && cache->count == 4 && !cache->budget_blocked
        && native::nr_features_created == before_occupied_features
        && native::worksets_created == before_occupied_worksets,
        "temporary capacity pressure defers preparation without creating or permanently blocking a tier");
    native::RetireWorkset({occupied_owner, color.Get()});
    native::DrainRetiredResources(true);
    service();
    Check(native::nr_features_created > before_occupied_features
        && native::features.at(stream.roles[0].Handle()).adaptive_cache == cache,
        "preparation resumes on the next frame after the occupying workset retires");
    for (uint32_t frame = 0; frame < 16; ++frame) service();
    Check(stream.adaptive_session != nullptr && !stream.adaptive_session->failed,
        "turning adaptive On after manual two-Pass does not pause a healthy service");
    Check(masked_calls == model_calls && masked_calls != 0,
        "both Passes and standby first-evaluates use the actual GPU mask path");
    Check(preparation_calls != 0 && model_calls > native::SourceSuccessfulEvaluations() * 2,
        "additional standby model calls really execute while source successes remain active-frame only");
    native::PollAdaptiveCache(cache.get());
    for (size_t tier = 0; tier < cache->count; ++tier) {
      Check(cache->tiers[tier].ready,
          "every tier matures through complete two-Pass recording and actual WARP submission");
    }
    Check(stream.adaptive_session->status.applied[0].width == 16
        && stream.adaptive_session->status.applied[1].width == 16,
        "the active two-Pass Service still reports both full working sizes after preparation");
    native::nr_adaptive_enabled.store(false);
    Check(service() == fg::Result::SUBMITTED && stream.adaptive_session->status.applied[0].width == 16,
        "adaptive Off returns the same Service stream to its saved manual upper size");
    native::nr_adaptive_enabled.store(true);
    for (uint32_t frame = 0; frame < 16; ++frame) service();
    cache = native::features.at(stream.roles[0].Handle()).adaptive_cache;
    native::PollAdaptiveCache(cache.get());
    Check(!cache->failed && !stream.adaptive_session->failed
        && std::all_of(cache->tiers.begin(), cache->tiers.begin() + cache->count,
            [](const auto& tier) { return tier.ready; }),
        "Off then On rebuilds and completes the same stream's standby tiers");
    auto& standby = cache->tiers[1];
    Check(standby.state.control_mask_stream != nullptr,
        "standby tier has its own ordered ControlMask producer");
    standby.output_completions.clear();
    standby.ready = false;
    Check(addon::control_mask_execution::BeginRecording(standby.state.control_mask_stream, gpu.list.Get()),
        "standby preparation can be deferred by an earlier unsubmitted producer");
    service();
    Check(!stream.adaptive_session->failed && !cache->failed,
        "standby admission rejection after a successful active frame never latches NR failure");
    for (uint32_t frame = 0; frame < 8; ++frame) service();
    native::PollAdaptiveCache(cache.get());
    Check(standby.ready, "standby preparation resumes after its earlier producer submits");
    auto mask_stream = native::features.at(stream.roles[0].Handle()).control_mask_stream;
    Check(mask_stream != nullptr, "the mocked 310.9.16 contract enables the real ControlMask producer");
    Check(addon::control_mask_execution::BeginRecording(mask_stream, gpu.list.Get()),
        "a previous application recording can retain the History ordering turn before submission");
    const auto before_deferred = model_calls;
    service();
    Check(model_calls == before_deferred && !stream.adaptive_session->failed,
        "a temporary unsubmitted ControlMask producer neither invokes NR nor latches an execution failure");
    for (uint32_t frame = 0; frame < 16; ++frame) service();
    Check(service() == fg::Result::SUBMITTED && !stream.adaptive_session->failed,
        "the service resumes after the pending producer is submitted");
    auto failure_cache = native::features.at(stream.roles[0].Handle()).adaptive_cache;
    Check(failure_cache != nullptr && failure_cache->count > 1,
        "active-stream recovery exposes the current standby cache for failure injection");
    auto& failure_standby = failure_cache->tiers[(failure_cache->active + 1) % failure_cache->count];
    failure_standby.output_completions.clear();
    failure_standby.ready = false;
    const auto before_failure = model_calls;
    fail_call = model_calls + 3;  // Both active Passes succeed, then standby P1 fails.
    service();
    Check(model_calls == before_failure + 3 && stream.adaptive_session->failed && failure_cache->failed,
        "an actual standby Evaluate failure after both active Passes pauses the adaptive session");
    Check(preparation_failures == 1 && active_failures == 0,
        "the failed standby API call remains classified as a real preparation failure");
    fail_call = 0;
    const auto before_recreate = native::configuration_generation.load();
    const auto before_rebuild = native::nr_features_created;
    native::RecreateFeatures();
    Check(native::configuration_generation.load() == before_recreate + 1
        && native::features.at(stream.roles[0].Handle()).adaptive_cache == nullptr,
        "the production clear-failure action retires the failed adaptive cache");
    for (uint32_t frame = 0; frame < 24; ++frame) service();
    auto recovered_cache = native::features.at(stream.roles[0].Handle()).adaptive_cache;
    native::PollAdaptiveCache(recovered_cache.get());
    Check(service() == fg::Result::SUBMITTED && recovered_cache != failure_cache
        && !stream.adaptive_session->failed && !recovered_cache->failed
        && native::nr_features_created > before_rebuild
        && stream.adaptive_session->status.applied[0].width == 16
        && std::all_of(recovered_cache->tiers.begin(), recovered_cache->tiers.begin() + recovered_cache->count,
            [](const auto& tier) { return tier.ready; }),
        "explicit clear-failure rebuilds real Features and successfully resumes the existing Service");
    model_parameter_observer = {};
    native::fg_service::Retire();
    gpu.Submit();
    native::DrainRetiredResources(true);
    if (info != nullptr) {
      for (UINT64 index = 0; index < info->GetNumStoredMessages(); ++index) {
        SIZE_T bytes = 0;
        info->GetMessage(index, nullptr, &bytes);
        std::vector<uint8_t> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        Check(SUCCEEDED(info->GetMessage(index, message, &bytes)), "adaptive service debug message");
        Check(message->Severity > D3D12_MESSAGE_SEVERITY_ERROR, message->pDescription);
      }
    }
    std::cout << "PASS adaptive single Backbuffer two-Pass Service evidence=WARP+MOCK frames=" << input.application_frame
        << " source_successes=" << native::SourceSuccessfulEvaluations() << " model_calls=" << model_calls
        << " preparation_calls=" << preparation_calls << " preparation_failures=" << preparation_failures << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "adaptive-fg-service: " << error.what() << '\n';
    return 1;
  }
}
}  // namespace adaptive_fg_service_cases
