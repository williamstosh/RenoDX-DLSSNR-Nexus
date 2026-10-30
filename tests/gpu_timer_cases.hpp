/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <utility>
#include <vector>

// Included after the native WARP host and GPU-copy model stand-ins.
namespace gpu_timer_cases {
namespace addon = renodx::addons::dlss5;
namespace timers = addon::gpu_timers;
using State = timers::SampleState;

int Run() {
  try {
    gpu_comparison_cases::Run();
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    Gpu gpu;
    timers::comparison_settings = native::comparison_ui::CaptureSettings;
    Check(!timers::ComparisonActive(), "normal timing identity does not require A/B capture");
    std::cout << "TIMER fixed CPU storage: segment=" << sizeof(timers::Segment)
              << " pass=" << sizeof(timers::PassStats) << " snapshot=" << sizeof(timers::Stats)
              << " ring=" << sizeof(timers::internal::segments)
              << " display_windows=" << sizeof(timers::internal::display_windows) << " bytes\n";
    model_gpu = &gpu;
    ComPtr<ID3D12InfoQueue> info;
    gpu.device.As(&info);
    Check(native::InstallQueueCompletionHooks(gpu.queue.Get()), "timer queue hooks");
    Check(native::InstallCommandListStateHooks(gpu.list.Get()), "timer list hooks");

    // Gpu::Submit prunes without preserving timer receipts. Use the production
    // queue hooks, wait on real GPU completion, then close the generation by Reset.
    bool keep_policy = false;
    const auto finish = [&](uint32_t submissions = 1, bool preserve = true, bool poll = true) {
      Check(SUCCEEDED(gpu.list->Close()), "timer close");
      ID3D12CommandList* lists[]{gpu.list.Get()};
      for (uint32_t i = 0; i < submissions; ++i) gpu.queue->ExecuteCommandLists(1, lists);
      Check(SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial)), "timer completion signal");
      HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      Check(event != nullptr, "timer completion event");
      const auto signal = gpu.fence->SetEventOnCompletion(gpu.serial, event);
      const auto wait = (SUCCEEDED(signal) ? WaitForSingleObject(event, 10000) : WAIT_FAILED);
      CloseHandle(event);
      Check(wait == WAIT_OBJECT_0, "timer bounded GPU completion");
      if (poll) {
        const auto before_close = timers::published_segments;
        timers::Poll(true, false);
        Check(timers::published_segments == before_close, "GPU complete but replayable generation cannot publish");
      }
      Check(SUCCEEDED(gpu.allocator->Reset()), "timer allocator reset");
      Check(SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)), "timer generation reset");
      if (preserve) {
        if (keep_policy) {
          native::RunLifecycleTick(0, false);
        } else {
          native::DrainRetiredResources(true);
        }
      } else {
        submission::PruneCompletedGenerations();
      }
      if (poll) timers::Poll(true, false);
      gpu.uploads.clear();
    };
    finish();

    Check(timers::Begin(gpu.list.Get(), gpu.device.Get(), "preSR", 16, 16), "start-only timer begin");
    const auto start_only = timers::internal::current.idx;
    timers::End(gpu.list.Get());
    Check(timers::internal::segments[start_only].recorded
              && !submission::ResourceReleasable(&timers::internal::segments[start_only]),
          "maturation exit retains its start-only query slot until the recording closes");
    finish();
    Check(!timers::stage_stats[0].valid && timers::stage_stats[0].state == State::INVALID_TIMESTAMPS,
          "start-only GPU recording retires safely without an invented span");

    const auto record = [&] {
      Check(timers::Begin(gpu.list.Get(), gpu.device.Get(), "postSR", 16, 16, 0, 2), "timer begin");
      timers::SetPass(0, 0, 1, 16, 16, 16, 16);
      for (const auto slot : {timers::PassEncodeStart(0), timers::PassEncodeEnd(0),
               timers::PassEvalStart(0), timers::PassEvalEnd(0), timers::PassResolveEnd(0),
               timers::kSlotCopyBackEnd}) timers::Mark(gpu.list.Get(), slot);
      timers::End(gpu.list.Get());
    };
    record();
    timers::Poll(true, false);
    Check(!timers::stats.valid && timers::stats.state == State::PENDING, "unsubmitted recording stays pending");
    finish(0);
    Check(!timers::stats.valid && timers::stats.state == State::DISCARDED, "Reset discards unsubmitted timer");
    record();
    const auto published = timers::published_segments;
    const auto recorded_identity = timers::internal::segments[timers::internal::current.idx].comparison.identity;
    Check(recorded_identity.configuration_generation == native::configuration_generation.load()
              && recorded_identity.settings_hash != 0 && !timers::ComparisonActive(),
          "ordinary timing freezes production settings identity without A/B capture");
    finish(1, true, false);
    Check(timers::published_segments == published, "CPU recording and GPU completion do not publish without Poll");
    timers::Poll(true, false);
    Check(timers::stats.valid && timers::stats.state == State::VALID
              && timers::published_segments == published + 1 && timers::stats.queue == gpu.queue.Get(),
          "closed single real queue submission publishes with its frequency");
    Check(timers::stats.identity == recorded_identity,
          "published ordinary GPU sample retains recording identity");
    Check(timers::stats.passes[1].state == State::NOT_EXECUTED && timers::stats.passes[1].total_us == 0
              && timers::stats.passes[2].total_us == 0 && timers::stats.passes[3].total_us == 0
              && !timers::stats.passes[0].look_measured,
          "unexecuted later passes have no invented GPU time");
    record();
    finish(2);
    Check(!timers::stats.valid && timers::stats.state == State::REPLAYED
              && timers::published_segments == published + 1,
          "two GPU executions of one recorded list reject replay without another model API call");
    record();
    finish(1, false);
    Check(!timers::stats.valid && timers::stats.state == State::PROOF_UNAVAILABLE,
          "completed generation without preserved receipt cannot publish");

    record();
    const auto parked = timers::internal::current.idx;
    const auto parked_sequence = timers::internal::segments[parked].sequence;
    timers::internal::head = parked;
    record();
    Check(timers::internal::current.idx != parked
              && timers::internal::segments[parked].sequence == parked_sequence,
          "bounded slot scan skips a parked recording and uses another free slot");
    finish(0);

    for (uint32_t i = 0; i < timers::kSegmentDepth; ++i) record();
    const auto occupied = timers::internal::head;
    const auto sequence = timers::internal::segments[occupied].sequence;
    for (uint64_t tick = 0; tick < timers::kStaleTicks; ++tick) timers::Poll(true, false);
    Check(timers::stats.state == State::EXPIRED && !timers::stats.valid,
          "unsubmitted samples expire after 64 lifecycle ticks");
    Check(!timers::Begin(gpu.list.Get(), gpu.device.Get(), "postSR", 16, 16)
              && timers::internal::segments[occupied].recorded
              && timers::internal::segments[occupied].sequence == sequence,
          "expired replayable recording still reserves query and readback slots");
    finish();
    Check(timers::published_segments == published + 1 && !timers::internal::segments[occupied].recorded,
          "late actual completion releases expired slots without publishing stale values");
    record();
    finish();
    Check(timers::stats.valid, "closed expired ring is reusable");
    native::enabled.store(true);
    native::hooks_enabled.store(true);
    native::workset_free_policy.store(1);
    keep_policy = true;
    for (uint32_t i = 0; i < timers::kSegmentDepth + 2; ++i) {
      record();
      finish();
      Check(timers::stage_stats[1].valid,
            "Keep policy lifecycle advances timer receipts without draining worksets or exhausting slots");
    }
    keep_policy = false;
    native::workset_free_policy.store(0);
    std::cout << "PASS timer recording proofs: pending, discard, single, replay, missing receipt, expiry and slot retention\n";

    native::enabled.store(true);
    native::hooks_enabled.store(true);
    native::dedupe_mode.store(addon::kDedupePerEvaluate);
    native::stack_passes.store(2);
    native::nr_follow_input_res.store(false);
    native::nr_resolution_scale.store(1.f);
    native::gpu_timers_enabled.store(true);
    native::codec_mode.store(0);
    native::feed_mode.store(0);
    native::look_mode.store(false);
    native::detail_stability.store(1);
    native::chained_temporal_history.store(true);
    native::transfer_strength.store(1.f);
    native::color_strength.store(1.f);
    native::intensity.store(.5f);
    native::preset.store(1);
    for (uint32_t pass = 0; pass < 3; ++pass) {
      native::pass_follow[pass].store(false);
      native::pass_intensity[pass].store(.75f);
      native::pass_preset[pass].store(2);
      native::pass_stage[pass].store(0);
      native::pass_transfer_strength[pass].store(1.f);
      native::pass_color_strength[pass].store(1.f);
      native::pass_look_result[pass].follow.store(true);
      native::pass_resolution_mode[pass].store(2);
      native::pass_resolution_scale[pass].store(.67f);
    }
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
    stage_execution_cases::capture_model_output = true;
    const auto texture = [&](DXGI_FORMAT format, bool output) {
      const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
      const D3D12_RESOURCE_DESC desc{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
          .Width = 16, .Height = 16, .DepthOrArraySize = 1, .MipLevels = 1, .Format = format,
          .SampleDesc = {1, 0}, .Flags = (output ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE)};
      ComPtr<ID3D12Resource> result;
      Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
          D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&result))), "timer fixture texture");
      gpu.Write(result.Get(), 96);
      gpu.Transition(result.Get(), D3D12_RESOURCE_STATE_COMMON,
          output ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : NATIVE_INPUT_STATE);
      return result;
    };
    auto source = texture(DXGI_FORMAT_R8G8B8A8_UNORM, false);
    auto output = texture(DXGI_FORMAT_R8G8B8A8_UNORM, true);
    auto depth = texture(DXGI_FORMAT_R32_FLOAT, false);
    auto motion = texture(DXGI_FORMAT_R16G16_FLOAT, false);
    finish();
    addon::bridge::OwnedParameters parameters;
    parameters.Set(NVSDK_NGX_Parameter_Color, source.Get());
    parameters.Set(NVSDK_NGX_Parameter_Output, output.Get());
    parameters.Set(NVSDK_NGX_Parameter_Depth, depth.Get());
    parameters.Set(NVSDK_NGX_Parameter_MotionVectors, motion.Get());
    parameters.Set(NVSDK_NGX_Parameter_Width, 16u);
    parameters.Set(NVSDK_NGX_Parameter_Height, 16u);
    parameters.Set(NVSDK_NGX_Parameter_OutWidth, 16u);
    parameters.Set(NVSDK_NGX_Parameter_OutHeight, 16u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 16u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 16u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, 0u);
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
    parameters.Set(NVSDK_NGX_Parameter_Reset, 0u);
    const auto* handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0x71000});
    native::RegisterCreatedFeatureLocked(handle, native::kFeatureDlss, &parameters);
    for (const bool mixed : {false, true}) {
      native::hook_point.store(mixed ? native::kHookRender : native::kHookUpscaled);
      native::pass_stage[0].store(mixed ? 2u : 0u);
      uint32_t steady = 0;
      for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 40 && steady < 2; ++attempt) {
        timers::comparison.Reset();
        timers::comparison_settings = native::comparison_ui::CaptureSettings;
        native::comparison_ui::CaptureSettings(&timers::comparison_capture_settings);
        const auto min_sequence = timers::internal::next_sequence + 1;
        Check(timers::comparison.Start(true, mixed ? 3 : 2, timers::NowNs(), min_sequence),
              "production A/B recording contract starts before actual NR commands");
        stage_execution_cases::evaluations.clear();
        const auto original = stage_execution_cases::Capture(gpu, source.Get(), NATIVE_INPUT_STATE);
        {
          const addon::EvaluateChainScope chain;
          if (mixed) {
            native::PreSrSwap swap;
            native::ProcessInlinePreSR(gpu.list.Get(), handle, &parameters, swap);
            auto* consumed = native::GetD3D12Resource(&parameters, NVSDK_NGX_Parameter_Color);
            const auto state = (swap.result != nullptr ? native::kPreSrReadable : NATIVE_INPUT_STATE);
            gpu.Transition(consumed, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            const D3D12_TEXTURE_COPY_LOCATION from{.pResource = consumed, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            const D3D12_TEXTURE_COPY_LOCATION to{.pResource = output.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            gpu.Transition(consumed, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
            gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            native::RestorePreSrColor(&parameters, swap);
          }
          native::ProcessInline(gpu.list.Get(), handle, &parameters);
        }
        const auto after = stage_execution_cases::Capture(gpu, source.Get(), NATIVE_INPUT_STATE);
        const auto delivered = stage_execution_cases::Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        for (auto& window : timers::internal::display_windows) window.updated_ns = 0;
        finish();
        Check(original.Pixels() == after.Pixels()
                  && native::GetD3D12Resource(&parameters, NVSDK_NGX_Parameter_Color) == source.Get(),
              "production path preserves source bytes and restores SR Color");
        const auto& calls = stage_execution_cases::evaluations;
        if (calls.size() != 2 || calls[0].input.width != 16 || calls[1].input.width != 11) continue;
        uint32_t bound_stages = 0;
        for (const auto& segment : timers::internal::segments) {
          const auto& captured = segment.comparison;
          if (segment.sequence < min_sequence || captured.sequence == 0) continue;
          Check(captured.identity.device_session != 0 && captured.identity.source_generation != 0
                    && captured.identity.configuration_generation == native::configuration_generation.load()
                    && captured.identity.sr_width == 16 && captured.identity.sr_height == 16
                    && captured.identity.out_width == 16 && captured.identity.out_height == 16
                    && captured.pass_count == (mixed ? 1u : 2u),
                "actual recording freezes source, device, configuration, SR/display and group identity");
          bound_stages |= 1u << captured.stage;
        }
        Check(bound_stages == (mixed ? 3u : 2u), "comparison identity preserves actual same/cross-stage grouping");
        for (uint32_t pass = 0; pass < 2; ++pass) {
          const auto& timing = timers::stage_stats[mixed && pass == 0 ? 0 : 1];
          const auto& layer = timing.passes[mixed ? 0 : pass];
          const auto& call = calls[pass];
          Check(timing.valid && layer.state == State::VALID && layer.id == pass
                    && layer.model == stage_execution_cases::presets.at(call.handle)
                    && layer.model == pass + 1 && layer.width == call.input.width
                    && layer.height == call.input.height && layer.output_width == call.output.width
                    && layer.output_height == call.output.height,
                "proven timer snapshot matches actual A1/B2 model parameters and 16/11 GPU extents");
          Check(call.input.Pixels() == call.output.Pixels(), "GPU identity model actually delivers its input");
          if (mixed) Check(timing.passes[1].state == State::NOT_EXECUTED && timing.passes[1].total_us == 0,
              "each separated stage uses local slot zero and keeps its global pass identity");
        }
        const auto pixels = delivered.Pixels();
        Check(delivered.width == 16 && delivered.height == 16
                  && std::any_of(pixels.begin(), pixels.end(), [](uint8_t value) { return value != 0; }),
              "full extent production image reaches GPU readback");
        ++steady;
      }
      Check(steady == 2, "production timer reaches two proven steady samples within bounded warmup");
      std::cout << "PASS production timer " << (mixed ? "Render -> Upscaled" : "Upscaled dual layer")
                << ": A1=16x16 B2=11x11, GPU copy model/readback\n";
    }
    timers::comparison.Reset();
    native::ForgetSourceHandleLocked(handle);
    finish();
    Check(native::worksets.empty() && !model_released_handles.empty(), "source retirement releases model worksets");
    timers::ReleaseAll();

    // Deterministic timestamp arithmetic uses the real publisher after all GPU
    // slots are retired. No synthetic array is presented as a GPU execution.
    std::array<uint64_t, timers::kSlotsPerEval> timestamps{};
    timers::internal::mapped = timestamps.data();
    timers::internal::frequency = 1'000'000;
    timers::Segment synthetic;
    std::strcpy(synthetic.path, "postSR");
    synthetic.width = synthetic.height = 16;
    synthetic.passes[0] = {.id = 0, .model = 1, .width = 16, .height = 16,
        .output_width = 16, .output_height = 16};
    const auto publish = [&](uint32_t scale) {
      const std::array<std::pair<uint32_t, uint64_t>, 8> values{{
          {timers::kSlotFrameStart, 100}, {timers::PassEncodeStart(0), 110},
          {timers::PassEncodeEnd(0), 120}, {timers::PassEvalStart(0), 120},
          {timers::PassEvalEnd(0), 140}, {timers::PassLookEnd(0), 145},
          {timers::PassResolveEnd(0), 150}, {timers::kSlotCopyBackEnd, 160}}};
      for (const auto& [slot, value] : values) {
        timestamps[slot] = value * scale;
        synthetic.written |= uint64_t{1} << slot;
      }
      synthetic.sequence = ++timers::internal::next_sequence;
      if (timers::ComparisonActive()) synthetic.comparison.sequence = synthetic.sequence;
      return timers::internal::Publish(synthetic, false, gpu.queue.Get());
    };
    Check(publish(1), "synthetic timestamp publisher");
    Check(timers::stats.total_us == 60 && timers::stats.passes[0].total_us == 40
              && timers::stats.passes[0].encode_us == 10 && timers::stats.passes[0].eval_us == 20
              && timers::stats.passes[0].resolve_us == 10 && timers::stats.passes[0].look_us == 5
              && timers::stats.passes[0].look_measured
              && timers::stats.own_us == 40, "look is included in resolve, never added twice");
    auto& window = timers::internal::display_windows[1];
    const auto first_display = timers::stage_stats[1].total_us;
    window.updated_ns = timers::NowNs() + timers::internal::kDisplayIntervalNs;
    Check(publish(2) && publish(3) && timers::stage_stats[1].total_us == first_display,
          "display remains stable before half-second refresh");
    window.updated_ns = 0;
    Check(publish(4) && timers::stage_stats[1].samples == 4 && timers::stage_stats[1].total_us == 150
              && timers::stage_stats[1].passes[0].total_us == 100,
          "half-second refresh averages exactly four samples including per-pass fields");
    synthetic.passes[0].model = 2;
    Check(publish(1) && timers::stage_stats[1].samples == 1 && timers::stage_stats[1].total_us == 60,
          "model identity change resets display averaging");
    synthetic.passes[0].width = 11;
    Check(publish(2) && timers::stage_stats[1].samples == 1, "working extent change resets averaging");
    synthetic.passes[0].id = 1;
    Check(publish(1) && timers::stage_stats[1].samples == 1, "global pass identity change resets averaging");
    synthetic.written &= ~(uint64_t{1} << timers::PassLookEnd(0));
    synthetic.sequence = ++timers::internal::next_sequence;
    Check(timers::internal::Publish(synthetic, false, gpu.queue.Get())
              && !timers::stats.passes[0].look_measured && timers::stats.passes[0].look_us == 0
              && timers::stage_stats[1].samples == 1,
          "absent look marker is not executed, and does not mix with measured look samples");
    Check(publish(1) && timers::stats.passes[0].look_measured, "recorded look marker restores measured identity");
    timestamps[timers::PassResolveEnd(0)] = timestamps[timers::PassEvalEnd(0)] - 1;
    synthetic.sequence = ++timers::internal::next_sequence;
    Check(!timers::internal::Publish(synthetic, false, gpu.queue.Get())
              && timers::stats.state == State::INVALID_TIMESTAMPS, "out-of-order timestamps cannot publish");
    Check(publish(1), "synthetic valid sample restored");
    for (const bool baseline : {true, false}) {
      const auto start = timers::NowNs() - 6'000'000'000;
      Check(timers::comparison.Start(baseline, 2, start, timers::internal::next_sequence + 1),
            "raw timer A/B capture starts");
      synthetic.comparison = {.stage = 1, .valid = true, .stable = true, .pass_count = 1};
      synthetic.passes[0].width = synthetic.passes[0].height = (baseline ? 16 : 11);
      synthetic.passes[0].output_width = synthetic.passes[0].output_height = (baseline ? 16 : 11);
      for (uint32_t i = 0; i < addon::gpu_comparison::SAMPLE_COUNT; ++i) {
        synthetic.comparison.recorded_ns = start + addon::gpu_comparison::STABILITY_NS
            + i * addon::gpu_comparison::INTERVAL_NS;
        Check(publish(baseline ? 2 : 1), "raw timer A/B sample publishes");
      }
    }
    Check(timers::comparison_result.reason == addon::gpu_comparison::Reason::NONE
              && timers::comparison_result.stages[1].passes[0].a.median_us == 40
              && timers::comparison_result.stages[1].passes[0].b.median_us == 20
              && timers::comparison_result.stages[1].total.delta_us == -60,
          "A/B uses individual completion data, not the four-sample display average");
    timers::comparison.Reset();
    synthetic.comparison = {};
    timers::stats.measured_ns = timers::NowNs() - timers::internal::kSampleAgeNs - 1;
    timers::stage_stats[1].measured_ns = timers::stats.measured_ns;
    timers::Poll(true, false);
    Check(!timers::stats.valid && !timers::stage_stats[1].valid
              && timers::stage_stats[1].state == State::EXPIRED, "display expires after two seconds");
    native::cost_chain_ms = 8;
    native::nr_cost_gpu_ms.store(8.f);
    native::nr_cost_present_ms.store(8.f);
    native::TickNrCost(timers::NowNs(), 16.0);
    Check(native::cost_chain_ms == 0 && native::nr_cost_gpu_ms.load() == 0.f
              && native::nr_cost_present_ms.load() == 0.f,
          "expired proof clears legacy cost instead of retaining a stale performance reading");
    timers::stage_stats[0].valid = timers::stage_stats[1].valid = true;
    timers::stage_stats[0].measured_ns = timers::stage_stats[1].measured_ns = timers::NowNs();
    native::cost_chain_ms = 8;
    native::nr_cost_gpu_ms.store(8.f);
    native::TickNrCost(timers::NowNs(), 16.0);
    Check(native::cost_chain_ms == 0 && native::nr_cost_gpu_ms.load() == 0.f,
          "multiple stage spans are not published as a frame latency estimate");
    timers::internal::mapped = nullptr;
    timers::ReleaseAll();
    if (info != nullptr) {
      for (UINT64 index = 0; index < info->GetNumStoredMessages(); ++index) {
        SIZE_T bytes = 0;
        info->GetMessage(index, nullptr, &bytes);
        std::vector<uint8_t> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        Check(SUCCEEDED(info->GetMessage(index, message, &bytes)), "timer debug message");
        Check(message->Severity != D3D12_MESSAGE_SEVERITY_ERROR
                  && message->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION, message->pDescription);
      }
    }
    std::cout << "PASS gpu-pass-diagnostics: WARP proof/lifecycle, production passes and deterministic display arithmetic; NVIDIA model substituted\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL gpu-pass-diagnostics: " << error.what() << '\n';
    return 1;
  }
}
}  // namespace gpu_timer_cases
