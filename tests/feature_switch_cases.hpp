/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <vector>

// Included after stage_execution_cases.hpp; reuses its GPU model and readback.
namespace feature_switch_cases {
namespace addon = renodx::addons::dlss5;

int RunLifecycleBoundaries() {
  try {
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    addon::RuntimeLock lock(native::runtime_mutex);
    addon::bridge::OwnedParameters parameters;
    parameters.Set(NVSDK_NGX_Parameter_Width, 8u);
    parameters.Set(NVSDK_NGX_Parameter_Height, 8u);
    parameters.Set(NVSDK_NGX_Parameter_OutWidth, 16u);
    parameters.Set(NVSDK_NGX_Parameter_OutHeight, 16u);
    native::hook_point.store(native::kHookUpscaled);
    native::stack_passes.store(1);
    native::chained_temporal_history.store(true);
    const auto* a = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0x5F001});
    const auto* b = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0x5F002});
    native::RegisterCreatedFeatureLocked(a, native::kFeatureDlss, &parameters);
    native::RegisterCreatedFeatureLocked(b, native::kFeatureDlssd, &parameters);
    const auto generation = native::features.at(a).source_generation;
    const auto plan = native::EvaluationStagePlan(a);
    Check(plan.valid && plan.At(addon::stage_plan::Stage::UPSCALED).count == 1,
          "retained source starts on its requested single-stage plan");
    const auto epoch = native::history_reset_epoch.load();
    // CPU policy fixture: no NR model, resource, queue or API-success record is
    // synthesized. The first-use pending flag models an already created slot.
    native::features.at(a).slots[0].pending_reset = true;
    const auto prepare_reset = [&](const NVSDK_NGX_Handle* handle, int game_reset) {
      auto source = native::FindOrRegisterFeature(handle, &parameters, "lifecycle CPU ");
      auto& slot = source->second.slots[0];
      addon::bridge::OwnedParameters model_parameters;
      slot.parameters = &model_parameters;
      native::SetModelParameters(slot, 0, native::history_reset_epoch.load(), 0, game_reset, false);
      const bool reset = native::GetInt(&model_parameters, "DLSSNR.Reset", -1) == 1;
      slot.parameters = nullptr;
      return reset;
    };
    Check(prepare_reset(a, 0) && !prepare_reset(a, 0), "first-use Reset is followed by continuous history");
    constexpr int64_t anchor = 10'000'000'000'000;
    native::worksets[{a, nullptr}].last_used_ns = anchor;
    native::worksets[{b, nullptr}].last_used_ns = anchor;
    const auto source_sequence = native::last_source_plan.sequence;
    const auto actual_sequence = native::stage_telemetry[1].last_evaluate.sequence;
    const auto accepted = native::stage_telemetry[1].accepted;
    for (const int64_t gap : {100'000'000ll, native::kWorksetIdleRetireNs + 1, 3'600'000'000'000ll}) {
      native::RetireSupersededWorksets(anchor + gap);
      Check(native::worksets.size() == 2 && native::features.at(a).source_generation == generation,
            "all-source inactivity keeps retained feature identity and scratch policy, including a one-hour gap");
      Check(native::last_source_plan.sequence == source_sequence
                && native::stage_telemetry[1].last_evaluate.sequence == actual_sequence
                && native::stage_telemetry[1].accepted == accepted
                && native::history_reset_epoch.load() == epoch,
            "no Evaluate cannot invent a source switch, actual NR invocation or global Reset");
    }
    const auto resumed = native::EvaluationStagePlan(a);
    Check(resumed.valid && resumed.requested == plan.requested && resumed.effective == plan.effective
              && native::last_source_plan.source_generation == generation && !prepare_reset(a, 0),
          "same retained feature resumes its original plan without an unconditional Reset");
    Check(prepare_reset(a, 1) && !prepare_reset(a, 0), "a game-requested Reset is sent once when normal evaluation resumes");
    const auto now = anchor + 3'600'000'000'000ll;
    native::worksets[{b, nullptr}].last_used_ns = now;
    native::RetireSupersededWorksets(now);
    Check(!native::worksets.contains({a, nullptr}) && native::worksets.contains({b, nullptr})
              && native::features.at(a).source_generation == generation && native::create_contracts.contains(a),
          "another active source retires only stale scratch while preserving the game's retained create contract");
    Check(native::FindOrRegisterFeature(a, &parameters, "lifecycle CPU ")->second.source_generation == generation,
          "retained source reuses its identity after idle scratch retirement");
    native::EvaluationStagePlan(a);
    native::ForgetSourceHandleLocked(a);
    Check(!native::features.contains(a) && !native::create_contracts.contains(a)
              && native::last_source_plan.source_released, "explicit DLSS release removes the old contract and marks its observation released");
    const auto released_sequence = native::last_source_plan.sequence;
    native::RetireSupersededWorksets(now + 3'600'000'000'000ll);
    Check(native::last_source_plan.sequence == released_sequence && native::last_source_plan.source_released,
          "no Evaluate after release cannot resurrect the released source");
    native::RegisterCreatedFeatureLocked(a, native::kFeatureDlss, &parameters);
    Check(native::features.at(a).source_generation > generation
              && native::last_source_plan.source_released && native::last_source_plan.sequence == released_sequence,
          "re-enabling DLSS creates a new generation without claiming it has evaluated");
    native::EvaluationStagePlan(a);
    native::features.at(a).slots[0].pending_reset = true;
    Check(!native::last_source_plan.source_released
              && native::last_source_plan.source_generation == native::features.at(a).source_generation
              && prepare_reset(a, 0) && !prepare_reset(a, 0),
          "recreated feature becomes the observed source on evaluation and starts fresh history once");
    Check(native::stage_telemetry[1].last_evaluate.sequence == actual_sequence
              && native::stage_telemetry[1].accepted == accepted,
          "CPU plan and Reset preparation never impersonate a real NR API call");
    native::ForgetSourceHandleLocked(a);
    native::ForgetSourceHandleLocked(b);
    Check(native::features.empty() && native::worksets.empty(), "CPU lifecycle fixture leaves no source state");
    std::cout << "PASS source-lifecycle: retained/no-Evaluate/menu/one-hour-policy-gap,"
                 " idle-other-source, release/recreate, stage and Reset preparation;"
                 " CPU only, no GPU or model execution\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL source-lifecycle: " << error.what() << '\n';
    return 1;
  }
}

int Run(bool hardware) {
  try {
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    Gpu gpu(hardware);
    model_gpu = &gpu;
    ComPtr<ID3D12InfoQueue> info;
    gpu.device.As(&info);
    native::enabled.store(true);
    native::hooks_enabled.store(true);
    native::dedupe_mode.store(addon::kDedupePerEvaluate);
    native::nr_follow_input_res.store(false);
    native::nr_resolution_scale.store(1.f);
    native::codec_mode.store(0);
    native::look_mode.store(false);
    native::chained_temporal_history.store(true);
    native::max_worksets.store(8);
    native::hook_point.store(native::kHookRender);
    native::stack_passes.store(2);
    native::preset.store(1);
    native::pass_preset[0].store(2);
    native::pass_stage[0].store(2);
    native::transfer_strength.store(1.f);
    native::color_strength.store(1.f);
    native::pass_transfer_strength[0].store(1.f);
    native::pass_color_strength[0].store(1.f);
    native::direct_api.module = GetModuleHandleW(nullptr);
    native::direct_api.create = stage_execution_cases::Create;
    native::direct_api.evaluate = stage_execution_cases::Evaluate;
    native::direct_api.release = ReleaseModel;
    native::direct_load_state = native::DirectLoadState::Ready;
    native::direct_device = gpu.device.Get();
    native::direct_device->AddRef();
    native::direct_device_native = gpu.device.Get();
    native::direct_runtime_requirements.dual_models = true;
    native::parameter_runtime_ready = true;
    native::core_allocate_parameters = addon::bridge::AllocateOwnedParameters;
    native::core_destroy_parameters = addon::bridge::DestroyOwnedParameters;
    Check(native::InstallQueueCompletionHooks(gpu.queue.Get()), "switch queue hooks");
    Check(native::InstallCommandListStateHooks(gpu.list.Get()), "switch list hooks");
    gpu.Submit();

    const auto texture = [&](DXGI_FORMAT format, bool writable, uint32_t size = 8) {
      const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
      const D3D12_RESOURCE_DESC desc{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
          .Width = size, .Height = size, .DepthOrArraySize = 1, .MipLevels = 1, .Format = format,
          .SampleDesc = {1, 0}, .Flags = (writable ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE)};
      ComPtr<ID3D12Resource> resource;
      Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource))), "switch texture");
      return resource;
    };
    auto color = texture(DXGI_FORMAT_R8G8B8A8_UNORM, false);
    auto output = texture(DXGI_FORMAT_R8G8B8A8_UNORM, true);
    auto depth = texture(DXGI_FORMAT_R32_FLOAT, false);
    auto motion = texture(DXGI_FORMAT_R16G16_FLOAT, false);
    for (auto* resource : {color.Get(), depth.Get(), motion.Get()}) {
      gpu.Write(resource, 0);
      gpu.Transition(resource, D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
    }
    gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    gpu.Submit();
    addon::bridge::OwnedParameters parameters;
    parameters.Set(NVSDK_NGX_Parameter_Color, color.Get());
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
    parameters.Set(NVSDK_NGX_Parameter_Reset, 0u);
    const auto* a = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0x5E001});
    const auto* b = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0x5E002});
    const auto* rr = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0x5E003});
    const auto* unused = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0x5E004});
    for (const auto* handle : {a, b, rr, unused}) {
      native::RegisterCreatedFeatureLocked(handle, handle == rr ? native::kFeatureDlssd : native::kFeatureDlss, &parameters);
      native::FeatureState captured;
      Check(native::CompletePresentContract(handle, 8, 8, &captured)
                && captured.source_generation == native::features.at(handle).source_generation
                && captured.source_generation != 0,
            "Present captures carry the current source's create generation");
    }
    const auto evaluate = [&](const NVSDK_NGX_Handle* handle, uint8_t value, bool steady) {
      stage_execution_cases::evaluations.clear();
      const auto accepted_before = native::stage_telemetry[0].accepted + native::stage_telemetry[1].accepted;
      gpu.Transition(color.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COMMON);
      gpu.Write(color.Get(), value);
      gpu.Transition(color.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
      bool complete = false;
      stage_execution_cases::Snapshot pixels;
      {
        const addon::EvaluateChainScope ngx;
        const addon::SourceStageScope source(handle, true);
        const auto plan = native::EvaluationStagePlan(handle);
        Check(native::last_source_plan.sequence != 0 && !native::last_source_plan.source_released
                  && native::last_source_plan.source_generation == native::features.at(handle).source_generation
                  && native::last_source_plan.source_feature == (handle == rr ? native::kFeatureDlssd : native::kFeatureDlss),
              "source plan identifies the evaluated source generation and feature type");
        if (handle == rr) Check(plan.requested[0] == addon::stage_plan::Stage::RENDER
                  && plan.effective[0] == addon::stage_plan::Stage::UPSCALED
                  && plan.reason == addon::stage_plan::Reason::RR_AFTER, "RR Render request retains its Planner fallback");
        Check(plan.valid && plan.At(addon::stage_plan::Stage::RENDER).count == (handle == rr ? 0u : 1u)
                  && plan.At(addon::stage_plan::Stage::UPSCALED).count == (handle == rr ? 2u : 1u),
              "current evaluated SR/RR handle decides actual stages at identical output size");
        native::PreSrSwap swap;
        if (native::PreSrTakes(handle)) native::ProcessInlinePreSR(gpu.list.Get(), handle, &parameters, swap);
        if (handle == rr) Check(swap.result == nullptr, "RR retains safe after-upscaling fallback");
        auto* consumed = native::GetD3D12Resource(&parameters, NVSDK_NGX_Parameter_Color);
        const auto input_state = (swap.result != nullptr ? native::kPreSrReadable : NATIVE_INPUT_STATE);
        gpu.Transition(consumed, input_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        const UINT size = static_cast<UINT>(consumed->GetDesc().Width);
        const D3D12_TEXTURE_COPY_LOCATION from{.pResource = consumed, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        const D3D12_TEXTURE_COPY_LOCATION to{.pResource = output.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        const D3D12_BOX box{0, 0, 0, size, size, 1};
        for (UINT y = 0; y < 8; y += size) {
          for (UINT x = 0; x < 8; x += size) {
            gpu.list->CopyTextureRegion(&to, x, y, 0, &from, &box);
          }
        }
        gpu.Transition(consumed, D3D12_RESOURCE_STATE_COPY_SOURCE, input_state);
        gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        native::RestorePreSrColor(&parameters, swap);
        Check(native::GetD3D12Resource(&parameters, NVSDK_NGX_Parameter_Color) == color.Get(),
              "source switch restores the game's Color binding");
        complete = native::ProcessInline(gpu.list.Get(), handle, &parameters);
        if (steady) {
          const auto& after = native::stage_telemetry[static_cast<size_t>(addon::stage_plan::Stage::UPSCALED)];
          const auto& render = native::stage_telemetry[static_cast<size_t>(addon::stage_plan::Stage::RENDER)];
          Check(after.accepted + render.accepted == accepted_before + 2, "each real interleaved source accepts both logical passes");
          Check(after.last_evaluate.source_generation == native::features.at(handle).source_generation
                    && after.last_evaluate.source_feature == (handle == rr ? native::kFeatureDlssd : native::kFeatureDlss)
                    && after.last_evaluate.pass == 1 && after.last_evaluate.requested == plan.requested[1]
                    && after.last_evaluate.planned == addon::stage_plan::Stage::UPSCALED
                    && after.last_evaluate.plan_reason == plan.reason && after.last_evaluate.observed_ns > 0
                    && NVSDK_NGX_SUCCEED(after.last_evaluate.result), "actual Upscaled observation matches this source and logical pass");
          if (handle != rr) Check(render.last_evaluate.source_generation == native::features.at(handle).source_generation
                    && render.last_evaluate.pass == 0 && render.last_evaluate.planned == addon::stage_plan::Stage::RENDER
                    && NVSDK_NGX_SUCCEED(render.last_evaluate.result), "interleaved SR source also has an accepted Render observation");
          Check(complete && stage_execution_cases::evaluations.size() == 2,
                "each real source call runs both logical passes, including reused outputs in one outer chain");
          const auto before_mirror = model_calls;
          const auto accepted_before_mirror = after.accepted + render.accepted;
          const auto observed_before_mirror = after.last_evaluate.sequence;
          const addon::EvaluateChainScope nested;
          const addon::SourceStageScope mirror(handle, true);
          Check(!native::ProcessInline(gpu.list.Get(), handle, &parameters) && model_calls == before_mirror,
                "a nested wrapper of the same call still cannot evaluate twice");
          Check(after.accepted + render.accepted == accepted_before_mirror && after.last_evaluate.sequence == observed_before_mirror,
                "same-source mirror advances neither acceptance nor actual observation");
        }
        pixels = stage_execution_cases::Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      }
      gpu.Submit();
      for (const auto byte : pixels.Pixels()) {
        Check(std::abs(static_cast<int>(byte) - value) <= 1, "switch output reads current source pixels");
      }
      if (complete) {
        for (const auto& call : stage_execution_cases::evaluations) {
          if (steady) Check(call.reset == 0, "interleaved sources keep independent continuous histories");
        }
      }
      return complete;
    };
    for (const auto* handle : {a, b, rr}) {
      uint32_t complete_frames = 0;
      for (uint32_t attempt = 0; attempt < 16 && complete_frames < 3; ++attempt) {
        if (evaluate(handle, 64, false)) ++complete_frames;
      }
      Check(complete_frames == 3, "each used source reaches bounded model warmup");
    }
    const auto creates = next_handle;
    const auto a_render = native::features.at(a).render_session->slots[0].handle;
    const auto a_after = native::features.at(a).slots[0].handle;
    Check(a_render != native::features.at(b).render_session->slots[0].handle
              && a_after != native::features.at(b).slots[0].handle
              && native::features.at(rr).render_session == nullptr
              && native::features.at(unused).slots[0].handle == nullptr,
          "same-size sources own separate stage histories; a created unused source never runs NR");
    for (uint32_t frame = 0; frame < 2; ++frame) {
      const addon::EvaluateChainScope streamline;
      evaluate(a, 48, true);
      evaluate(b, 80, true);
      evaluate(rr, 112, true);
      evaluate(a, 144, true);
    }
    Check(next_handle == creates && native::features.at(a).slots[0].handle == a_after
              && native::features.at(a).render_session->slots[0].handle == a_render,
          "switching actual source does not rebuild or mix another source's model state");
    const auto actual_before_create = native::stage_telemetry[1].last_evaluate.sequence;
    const auto plan_before_create = native::last_source_plan.sequence;
    native::RegisterCreatedFeatureLocked(unused, native::kFeatureDlss, &parameters);
    Check(native::stage_telemetry[1].last_evaluate.sequence == actual_before_create
              && native::last_source_plan.sequence == plan_before_create, "unused creation cannot impersonate an evaluated source");
    const auto original_color = color;
    for (const uint32_t size : {4u, 8u}) {
      color = (size == 8 ? original_color : texture(DXGI_FORMAT_R8G8B8A8_UNORM, false, size));
      if (size != 8) gpu.Transition(color.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
      parameters.Set(NVSDK_NGX_Parameter_Color, color.Get());
      parameters.Set(NVSDK_NGX_Parameter_Width, size);
      parameters.Set(NVSDK_NGX_Parameter_Height, size);
      parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, size);
      parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, size);
      bool recovered = false;
      bool reset_seen = false;
      bool dimensions_reason_seen = false;
      for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 16 && !recovered; ++attempt) {
        const bool complete = evaluate(a, 72, false);
        for (const auto& call : stage_execution_cases::evaluations) reset_seen |= call.reset == 1;
        const auto& observed = native::stage_telemetry[0].last_evaluate;
        dimensions_reason_seen |= observed.reset_sent && (observed.reset_reasons & native::kResetDimensions) != 0;
        recovered = complete && native::features.at(a).render_session->input_width == size;
      }
      Check(recovered && reset_seen && native::features.at(a).output_width == 8,
            "dynamic render size resets the affected stage and recovers at unchanged output size");
      Check(dimensions_reason_seen, "dynamic render size records Dimensions as an actual Reset reason");
      evaluate(a, 72, true);
    }
    evaluate(b, 80, true);
    evaluate(rr, 112, true);
    evaluate(b, 80, true);
    const auto generation = native::features.at(b).source_generation;
    native::ForgetSourceHandleLocked(b);
    Check(native::last_source_plan.source_released && native::stage_telemetry[1].last_evaluate.source_released
              && native::stage_telemetry[1].last_evaluate.source_generation == generation,
          "release marks retained source observations without losing their generation");
    Check(native::features.count(b) == 0 && std::ranges::none_of(native::worksets,
              [b](const auto& entry) { return entry.first.handle == b; }),
          "released source retires both stages and all its worksets");
    evaluate(a, 176, true);
    native::RegisterCreatedFeatureLocked(b, native::kFeatureDlss, &parameters);
    Check(native::features.at(b).source_generation > generation, "same-address recreate gets a new source generation");
    bool reset_seen = false;
    uint32_t resumed = 0;
    for (uint32_t attempt = 0; attempt < 16 && resumed < 3; ++attempt) {
      if (evaluate(b, 96, false)) ++resumed;
      for (const auto& call : stage_execution_cases::evaluations) reset_seen |= call.reset == 1;
    }
    Check(resumed == 3 && reset_seen, "recreated source starts fresh History and resumes");
    evaluate(b, 96, true);
    Check(!native::last_source_plan.source_released && !native::stage_telemetry[1].last_evaluate.source_released
              && native::stage_telemetry[1].last_evaluate.source_generation > generation,
          "same-address recreation publishes a fresh live actual generation");
    const auto reset_accepted_before_failure = native::stage_telemetry[0].reset_accepted;
    const auto accepted_before_failure = native::stage_telemetry[0].accepted;
    parameters.Set(NVSDK_NGX_Parameter_Reset, 1u);
    fail_call = model_calls + 1;
    Check(!evaluate(b, 96, false), "existing model failure injection rejects the source chain");
    fail_call = 0;
    parameters.Set(NVSDK_NGX_Parameter_Reset, 0u);
    Check(native::stage_telemetry[0].last_evaluate.reset_sent && NVSDK_NGX_FAILED(native::stage_telemetry[0].last_evaluate.result)
              && native::stage_telemetry[0].reset_accepted == reset_accepted_before_failure
              && native::stage_telemetry[0].accepted == accepted_before_failure,
          "failed NGX Reset invocation publishes failure without increasing acceptance");
    const auto accepted_before_exception = native::stage_telemetry[0].accepted;
    const auto sequence_before_exception = native::stage_telemetry[0].last_evaluate.sequence;
    model_ui_observer = [] { throw std::runtime_error("injected NR observation exception"); };
    const bool exception_complete = evaluate(a, 96, false);
    model_ui_observer = {};
    Check(!exception_complete && !native::stage_telemetry[0].last_evaluate.api_returned
              && native::stage_telemetry[0].last_evaluate.sequence > sequence_before_exception
              && native::stage_telemetry[0].accepted == accepted_before_exception,
          "NR exception publishes an unavailable API result without accepting the invocation");
    const auto actual_before_configuration = native::stage_telemetry;
    addon::bridge::OwnedParameters configuration_parameters;
    native::NrFeatureSlot configuration_slot;
    configuration_slot.parameters = &configuration_parameters;
    configuration_slot.acknowledged_reset_epoch = native::history_reset_epoch.load();
    native::RequestHistoryReset(native::HistoryResetSource::kSetting);
    native::SetModelParameters(configuration_slot, 0, native::history_reset_epoch.load(), 0, 0, false);
    Check(configuration_slot.model_reset && configuration_slot.model_reset_reasons == native::kResetConfiguration
              && native::GetInt(&configuration_parameters, "DLSSNR.Reset", -1) == 1,
          "real setting Reset request attributes the new epoch to Configuration");
    for (size_t stage = 0; stage < native::stage_telemetry.size(); ++stage) {
      Check(native::stage_telemetry[stage].last_evaluate.sequence == actual_before_configuration[stage].last_evaluate.sequence
                && native::stage_telemetry[stage].accepted == actual_before_configuration[stage].accepted
                && native::stage_telemetry[stage].reset_accepted == actual_before_configuration[stage].reset_accepted,
            "setting Reset parameter preparation alone cannot publish or accept an actual NR evaluation");
    }
    std::string diagnostic_telemetry;
    {
      addon::RuntimeLock lock(native::runtime_mutex);
      const auto* excluded = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0x5E0FF});
      const auto source_plan_sequence = native::last_source_plan.sequence;
      native::created_feature_ids[excluded] = NVSDK_NGX_Feature_FrameGeneration;
      native::ObserveSourcePlan(excluded, native::RequestedStagePlan(false));
      native::created_feature_ids.erase(excluded);
      Check(native::last_source_plan.sequence == source_plan_sequence,
            "known non-SR/RR evaluates cannot replace the observed upscaler source plan");
      const auto saved_hook = native::hook_point.exchange(native::kHookUpscaled);
      const auto saved_stage = native::pass_stage[0].exchange(1);
      const auto rejected_plan = native::EvaluationStagePlan(b);
      native::hook_point.store(saved_hook);
      native::pass_stage[0].store(saved_stage);
      Check(!native::StagePlanAdmitted(rejected_plan), "backward Stage plan is rejected without invoking NR");
      for (size_t stage = 0; stage < native::stage_telemetry.size(); ++stage) {
        Check(native::stage_telemetry[stage].last_evaluate.sequence == actual_before_configuration[stage].last_evaluate.sequence
                  && native::stage_telemetry[stage].accepted == actual_before_configuration[stage].accepted,
              "plan rejection cannot advance actual evaluation or acceptance");
      }
      const auto saved_interval = native::telemetry_seconds.exchange(1);
      const auto now = native::SteadyNowNs();
      native::EmitTelemetry(now);
      native::EmitTelemetry(now + 1'000'000'000, true);
      native::telemetry_seconds.store(saved_interval);
      std::lock_guard telemetry_lock(native::last_telemetry_mutex);
      diagnostic_telemetry = native::last_telemetry_line;
    }
    Check(diagnostic_telemetry.find(" source_plan[") != std::string::npos
              && diagnostic_telemetry.find(" last_nr_evaluate[") != std::string::npos
              && diagnostic_telemetry.find(" local_reset_request[") != std::string::npos
              && diagnostic_telemetry.find("ngx_result=Unavailable(exception)") != std::string::npos
              && diagnostic_telemetry.find("gpu_execution=Unverified") != std::string::npos,
          "periodic telemetry includes source plan, actual evaluation, local Reset request and unverified GPU execution");
    Check(native::BuildSupportReport().find("telemetry: " + diagnostic_telemetry) != std::string::npos,
          "support report reuses the published diagnostic telemetry text");
    {
      addon::RuntimeLock lock(native::runtime_mutex);
      const auto saved_stats = native::stage_telemetry[0];
      const auto saved_sequence = native::source_diagnostic_sequence;
      auto& stats = native::stage_telemetry[0];
      stats = {};
      stats.diagnostic_logged_ns = native::SteadyNowNs();
      const auto plan = native::RequestedStagePlan(false);
      const auto& feature = native::features.at(a);
      native::ObserveNrEvaluation(addon::stage_plan::Stage::RENDER, a, feature, configuration_slot, 0, NVSDK_NGX_Result_Success, plan);
      const bool deferred = stats.diagnostic_log_pending && stats.diagnostic_log_suppressed == 1;
      stats.diagnostic_logged_ns = native::SteadyNowNs() - 1'000'000'001;
      native::ObserveNrEvaluation(addon::stage_plan::Stage::RENDER, a, feature, configuration_slot, 0, NVSDK_NGX_Result_Success, plan);
      const bool flushed = !stats.diagnostic_log_pending && stats.diagnostic_log_suppressed == 1
          && stats.diagnostic_logged_ns == stats.last_evaluate.observed_ns;
      stats = saved_stats;
      native::source_diagnostic_sequence = saved_sequence;
      Check(deferred && flushed, "rate-limited change is logged by the next unchanged observation after the interval");
    }
    for (const auto* handle : {a, b, rr, unused}) native::ForgetSourceHandleLocked(handle);
    gpu.Submit();
    native::DrainRetiredResources(true);
    Check(native::features.empty() && native::worksets.empty() && native::retired_nr_features.empty()
              && native::retired_final_resources.empty(), "switch source cleanup drains after real GPU completion");
    if (info != nullptr) {
      for (UINT64 index = 0; index < info->GetNumStoredMessages(); ++index) {
        SIZE_T bytes = 0;
        info->GetMessage(index, nullptr, &bytes);
        std::vector<uint8_t> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        Check(SUCCEEDED(info->GetMessage(index, message, &bytes)), "switch debug message read");
        Check(message->Severity != D3D12_MESSAGE_SEVERITY_ERROR
                  && message->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION, message->pDescription);
      }
    }
    std::cout << "PASS feature-switch SR A/B, same-size SR/RR, retained/unused/recreated sources,"
                 " interleaved same-output calls, stage-local History, mirror dedupe and GPU readback;"
                 " NVIDIA SR/NR substituted\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL feature-switch: " << error.what() << '\n';
    return 1;
  }
}
}  // namespace feature_switch_cases
