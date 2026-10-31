/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cmath>
#include <memory>

// Included after the existing D11 bridge GPU/model fixture.
namespace d3d11_present_stage_resolution_cases {
namespace addon = renodx::addons::dlss5;

struct InputCapture {
  ComPtr<ID3D12Resource> readback;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<unsigned char> Pixels() const {
    void* mapped = nullptr;
    Check(SUCCEEDED(readback->Map(0, nullptr, &mapped)), "Present model input readback map");
    std::vector<unsigned char> pixels(size_t(width) * height * 8);
    for (uint32_t y = 0; y < height; ++y) {
      std::memcpy(pixels.data() + size_t(y) * width * 8,
          static_cast<unsigned char*>(mapped) + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch,
          size_t(width) * 8);
    }
    readback->Unmap(0, nullptr);
    return pixels;
  }
};
std::vector<InputCapture> inputs;
std::array<unsigned short, 2> stamps{0x3400, 0x3600};
using CloseFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
CloseFn real_close = nullptr;
ID3D12GraphicsCommandList* failed_list = nullptr;
HRESULT STDMETHODCALLTYPE Close(ID3D12GraphicsCommandList* list) {
  const auto result = real_close(list);
  if (list == failed_list) {
    failed_list = nullptr;
    return E_FAIL;
  }
  return result;
}
NVSDK_NGX_Result NVSDK_CONV Evaluate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback) {
  auto* input = native::GetD3D12Resource(parameters, "DLSSNR.Color");
  const auto desc = input->GetDesc();
  InputCapture capture{.width = static_cast<uint32_t>(desc.Width), .height = desc.Height};
  uint64_t bytes = 0;
  native::bridge.device12->GetCopyableFootprints(&desc, 0, 1, 0, &capture.footprint, nullptr, nullptr, &bytes);
  const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_READBACK};
  const D3D12_RESOURCE_DESC buffer{.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Width = bytes,
      .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1, .SampleDesc = {1, 0},
      .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
  Check(SUCCEEDED(native::bridge.device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&capture.readback))), "Present model input capture");
  native::Transition(list, input, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
  const D3D12_TEXTURE_COPY_LOCATION from{.pResource = input, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  const D3D12_TEXTURE_COPY_LOCATION to{.pResource = capture.readback.Get(),
      .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = capture.footprint};
  list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  native::Transition(list, input, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  const auto pass = inputs.size();
  inputs.push_back(std::move(capture));
  Check(pass < 2, "Present records exactly two model calls at most");
  const auto saved_stamps = model_stamps;
  model_stamps = {stamps[pass], stamps[pass]};
  const auto result = MotionEvaluateModel(list, handle, parameters, callback);
  model_stamps = saved_stamps;
  return result;
}

template <typename TextureFactory>
int Run(ID3D11DeviceContext* context, const TextureFactory& texture) {
  native::d3d11_swapchain_seen.store(true);
  native::dx11_route.store(native::kDx11RouteNative);
  native::direct_runtime_requirements.dual_models = true;
  native::dedupe_mode.store(addon::kDedupePerEvaluate);
  native::hook_point.store(native::kHookPresent);
  native::stack_passes.store(2);
  native::nr_follow_input_res.store(false);
  native::gpu_timers_enabled.store(true);
  native::telemetry_seconds.store(1);
  native::chained_temporal_history.store(true);
  native::pass_stage[0].store(0);
  native::pass_follow[0].store(true);
  native::pass_transfer_strength[0].store(1.f);
  native::pass_color_strength[0].store(1.f);
  native::pass_look_result[0].follow.store(true);
  native::direct_api.evaluate = Evaluate;
  real_close = reinterpret_cast<CloseFn>((*reinterpret_cast<void***>(native::bridge.ring[0].list))[9]);
  Check(DetourTransactionBegin() == NO_ERROR && DetourUpdateThread(GetCurrentThread()) == NO_ERROR
      && DetourAttach(reinterpret_cast<PVOID*>(&real_close), reinterpret_cast<PVOID>(Close)) == NO_ERROR
      && DetourTransactionCommit() == NO_ERROR, "Present bridge scoped Close fault detour");
  int hooks_key = 0;
  const auto unhook = [](int*) {
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(reinterpret_cast<PVOID*>(&real_close), reinterpret_cast<PVOID>(Close));
    DetourTransactionCommit();
  };
  const std::unique_ptr<int, decltype(unhook)> fault_hooks(&hooks_key, unhook);
  fg::native_options::Configure(native::OnNativeFgOptions, native::OnNativePresentObserved);
  fg::native_options::internal::state.real = &FakeOptions;
  const auto set_options = [&](sl::DLSSGMode mode, sl::Result result = sl::Result::eOk) {
    options_result = result;
    sl::DLSSGOptions options;
    options.mode = mode;
    Check(fg::native_options::internal::HookedSetOptions(sl::ViewportHandle(7), options) == result,
        "FG mock forwards actual SetOptions result");
  };
  const auto mean = [](const std::vector<unsigned char>& pixels, uint32_t width, uint32_t height,
                       DXGI_FORMAT format) {
    const uint32_t stride = (format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u);
    std::vector<float> row(width * 4);
    double sum = 0;
    for (uint32_t y = 0; y < height; ++y) {
      Check(addon::screenshot::internal::DecodeDiagnosticRow(pixels.data() + size_t(y) * width * stride,
          width, format, row.data()), "Present GPU pixel decoding");
      for (uint32_t x = 0; x < width; ++x) {
        for (uint32_t c = 0; c < 3; ++c) {
          Check(std::isfinite(row[x * 4 + c]), "Present finite GPU result");
          sum += row[x * 4 + c];
        }
      }
    }
    return sum / (width * height * 3);
  };
  constexpr uint32_t extent = 16;
  std::vector<uint16_t> zeros(extent * extent * 2, 0);
  auto motion = texture(extent, extent, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE,
      zeros.data(), extent * 4);
  auto depth = texture(extent, extent, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE,
      zeros.data(), extent * 4);
  for (uint32_t scenario = 0; scenario < 6; ++scenario) {
    const DXGI_FORMAT format = (scenario == 4 ? DXGI_FORMAT_R16G16B16A16_FLOAT
        : (scenario == 5 ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM));
    const bool hdr = scenario >= 4;
    const uint32_t stride = (scenario == 4 ? 8u : 4u);
    std::vector<unsigned char> source(extent * extent * stride, 96);
    if (hdr) {
      constexpr uint16_t scrgb[]{0x211f, 0x211f, 0x211f, 0x3c00};
      constexpr uint32_t pq = 148u | (148u << 10) | (148u << 20) | (3u << 30);
      for (uint32_t pixel = 0; pixel < extent * extent; ++pixel) {
        std::memcpy(source.data() + pixel * stride,
            scenario == 4 ? static_cast<const void*>(scrgb) : static_cast<const void*>(&pq), stride);
      }
    }
    auto frame = texture(extent, extent, format, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
        source.data(), extent * stride);
    auto& stream = native::present_streams[frame.Get()];
    const auto* handle = stream.Handle();
    native::FeatureState contract;
    contract.input_width = contract.input_height = extent;
    contract.has_motion_scale_x = contract.has_motion_scale_y = true;
    contract.motion_scale_x = contract.motion_scale_y = 1.f;
    auto& block = native::FillPresentBlock(&stream, frame.Get(), motion.Get(), depth.Get(),
        contract, extent, extent, 1.f, 0);
    native::PresentTarget target;
    target.d3d11_bridge = true;
    target.independent_resolution_pair = true;
    target.domain = addon::present_restore_scope::Domain::FINAL_PRESENT;
    target.capture_display = true;
    target.workset_key = reinterpret_cast<const ID3D12Resource*>(&stream);
    native::codec_mode.store(hdr ? 3u : 0u);
    if (hdr) {
      target.hdr_mode = (scenario == 5 ? 2 : 1);
      target.units = (scenario == 5
          ? native::codec::SourceUnits{native::codec::Encoding::Pq, true, 10000.f, true}
          : native::codec::SourceUnits{native::codec::Encoding::Linear, true, 80.f, false});
      target.hdr_codec_mode = 3;
      target.hdr_white_nits = 80.f;
    }
    native::SnapshotPresentRestoreConfiguration(&target);
    target.hdr_encoding = native::V6EncodingConstant(target.units);
    target.hdr_dark_gate = native::V6DarkGate(target.units);
    target.hdr_pedestal_cap = native::codec::BlackPedestalCap(target.units);
    Check(!hdr || (target.hdr_curve == 1 && target.hdr_encoding == (scenario == 5 ? 4u : 1u)),
        "Present HDR freezes actual source units before restore configuration");
    native::preset.store(1);
    native::pass_preset[0].store(scenario == 2 ? 1u : 2u);
    native::nr_resolution_scale.store(scenario == 1 ? .67f : 1.f);
    native::pass_resolution_mode[0].store(2);
    native::pass_resolution_scale[0].store(scenario == 1 || scenario == 2 || scenario == 3 ? 1.f : .67f);
    const native::WorksetKey key{handle, target.workset_key};
    const auto frame_run = [&](bool wait = true) {
      if (wait && native::bridge_submitted_value.load()) {
        WaitForGpu(native::bridge.fence_done12, native::bridge_submitted_value.load());
      }
      inputs.clear();
      model_observations.clear();
      context->UpdateSubresource(frame.Get(), 0, nullptr, source.data(), extent * stride, 0);
      ++native::present_generation;
      const addon::EvaluateChainScope scope;
      const bool delivered = native::RunBridge(context, handle, &block, nullptr, &target);
      if (wait && native::bridge_submitted_value.load()) {
        WaitForGpu(native::bridge.fence_done12, native::bridge_submitted_value.load());
      }
      Check(params::GetD3D11Resource(&block, NVSDK_NGX_Parameter_Color) == frame.Get()
          && params::GetD3D11Resource(&block, NVSDK_NGX_Parameter_Output) == frame.Get(),
          "Present true alias remains bound to original D11 surface");
      return delivered;
    };
    if (scenario == 0) {
      Check(!native::native_fg_public_off.load() && !native::PresentResolutionPairAllowed(target),
          "fresh unknown FG cannot enable Present pair");
      Check(!frame_run() && model_observations.empty(), "unknown FG entry rejects pair before model calls");
      set_options(sl::DLSSGMode::eOff, sl::Result::eErrorInvalidParameter);
      Check(!native::native_fg_public_off.load() && !frame_run() && model_observations.empty(),
          "failed Off result cannot provide production FG evidence");
      set_options(sl::DLSSGMode::eOff);
      Check(native::native_fg_public_off.load() && native::PresentResolutionPairAllowed(target),
          "successful public Off callback enables native D11 conditional pair");
      auto forbidden = target;
      forbidden.independent_resolution_pair = false;
      Check(!native::PresentResolutionPairAllowed(forbidden), "missing ordinary Present flag rejects pair");
      native::dx11_route.store(native::kDx11RouteUndecided);
      Check(!native::PresentResolutionPairAllowed(target), "non-native DX11 route rejects pair");
      native::dx11_route.store(native::kDx11RouteNative);
    }
    const auto timer_start = addon::gpu_timers::measured_total_us;
    const auto samples_start = addon::gpu_timers::published_segments;
    const auto signals_start = native::bridge_signals.load();
    const auto submits_start = native::bridge_submits.load();
    const auto output_start = native::bridge_waits11.load();
    const auto completion_start = native::bridge_completion_waits;
    const auto completion_ns_start = native::bridge_completion_wait_ns;
    const auto cpu_start = native::bridge_cpu_timing[0];
    std::array<uint64_t, 2> generations{};
    uint64_t root_id = 0;
    for (uint32_t phase = 0; phase < (scenario == 0 ? 2u : 1u); ++phase) {
      if (phase) native::pass_resolution_scale[0].store(.75f);
      const uint32_t first_size = (scenario == 1 ? 11u : 16u);
      const uint32_t last_size = (phase ? 12u : (scenario == 1 || scenario == 2 || scenario == 3 ? 16u : 11u));
      uint32_t steady = 0;
      bool reset_seen = false;
      stamps = {0x3400, 0x3600};
      for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 32 && steady < 2; ++attempt) {
        const bool delivered = frame_run();
        if (model_observations.size() != 2 || model_observations[0].width != first_size
            || model_observations[1].width != last_size) continue;
        const auto& first = model_observations[0];
        const auto& second = model_observations[1];
        Check(first.height == first_size && second.height == last_size, "Present actual GPU input/output extents");
        reset_seen |= second.reset != 0;
        if (phase) {
          Check(first.id == generations[0] && first.reset == 0 && second.id != generations[1],
              "tail-only resize preserves upstream Feature and History");
          Check(native::worksets.at(key).id == root_id, "tail-only resize preserves full-size root");
        }
        if (delivered && first.reset == 0 && second.reset == 0) ++steady;
      }
      Check(steady == 2 && reset_seen, "Present bounded warmup reaches steady pair with tail reset");
      generations = {model_observations[0].id, model_observations[1].id};
      auto& work = native::worksets.at(key);
      root_id = work.id;
      Check((first_size == last_size) == (work.resolution_tail == nullptr), "Present tail allocation matches actual sizes");
      stamps = {0x3400, 0x3200};
      Check(frame_run(), "Present low markers deliver");
      const auto low_input = inputs.at(1).Pixels();
      const auto low_final = Read11(frame.Get(), stride);
      stamps[0] = 0x3a00;
      Check(frame_run(), "Present high first marker delivers");
      const auto high_input = inputs.at(1).Pixels();
      const auto low_mean = mean(low_input, last_size, last_size, DXGI_FORMAT_R16G16B16A16_FLOAT);
      const auto high_mean = mean(high_input, last_size, last_size, DXGI_FORMAT_R16G16B16A16_FLOAT);
      Check(high_input != low_input && high_mean > low_mean, "GPU Pass2 input follows actual Pass1 marker");
      stamps = {0x3400, 0x3400};
      Check(frame_run(), "Present high tail marker delivers");
      const auto high_final = Read11(frame.Get(), stride);
      Check(high_final != low_final && mean(high_final, extent, extent, format) > mean(low_final, extent, extent, format),
          "full-size alias GPU output follows actual tail marker");
      Check(Read11(native::bridge_sets.at(handle)[0].output11, stride) == high_final,
          "Present D11 alias receives exact completed bridge output");
      std::printf("HANDOFF present-bridge scenario=%u phase=%u input2_low=%.8f input2_high=%.8f extent=%u,%u\n",
          scenario, phase, low_mean, high_mean, first_size, last_size);
    }
    addon::gpu_timers::Poll(native::queue_tracking_active.load(), false);
    Check(native::bridge_signals.load() - signals_start == native::bridge_submits.load() - submits_start
        && native::bridge_completion_waits - completion_start == native::bridge_submits.load() - submits_start,
        "single Bridge input submission and strict completion per pair");
    std::printf("PASS bridge-present-stage-resolution scenario=%u live_workset_bytes=%llu timer_segments=%llu timer_total_us=%llu singlebridge_input=%llu singlebridge_submit=%llu singlebridge_output=%llu completion_waits=%llu GPU=standins scope=warmup-model-upload-readbacks\n",
        scenario, static_cast<unsigned long long>(native::worksets.at(key).allocated_bytes),
        static_cast<unsigned long long>(addon::gpu_timers::published_segments - samples_start),
        static_cast<unsigned long long>(addon::gpu_timers::measured_total_us - timer_start),
        static_cast<unsigned long long>(native::bridge_signals.load() - signals_start),
        static_cast<unsigned long long>(native::bridge_submits.load() - submits_start),
        static_cast<unsigned long long>(native::bridge_waits11.load() - output_start),
        static_cast<unsigned long long>(native::bridge_completion_waits - completion_start));
    const auto& cpu = native::bridge_cpu_timing[0];
    std::printf("COST present-bridge scenario=%u shared_bytes=%llu completion_wait_ns=%llu cpu_samples=%llu cpu_input_ns=%llu cpu_submit_ns=%llu cpu_completion_ns=%llu cpu_return_ns=%llu counters=scenario-deltas completion=CPU-blocked-wait not-GPU-NR-benefit\n",
        scenario, static_cast<unsigned long long>(native::bridge_sets.at(handle)[0].bytes),
        static_cast<unsigned long long>(native::bridge_completion_wait_ns - completion_ns_start),
        static_cast<unsigned long long>(cpu.samples - cpu_start.samples),
        static_cast<unsigned long long>(cpu.input_ns - cpu_start.input_ns),
        static_cast<unsigned long long>(cpu.submit_ns - cpu_start.submit_ns),
        static_cast<unsigned long long>(cpu.completion_ns - cpu_start.completion_ns),
        static_cast<unsigned long long>(cpu.return_ns - cpu_start.return_ns));
    fail_model_call = model_calls + 2;
    Check(!frame_run(), "failed Present tail cannot deliver partial result");
    fail_model_call = 0;
    Check(Read11(frame.Get(), stride) == source, "failed tail preserves original alias pixels");
    for (uint32_t pass = 0; pass < 2; ++pass) {
      const auto& slot = native::features.at(handle).slots[pass];
      Check(slot.pending_reset && (slot.pending_reset_reasons & native::kResetRecovery) && !slot.look_history.valid,
          "failed Present pair marks both model slots Recovery and invalidates Look history");
    }
    // Recovery must recreate both slots after a partially recorded pair.
    uint32_t recovered = 0;
    for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 32 && recovered < 2; ++attempt) {
      if (frame_run()) ++recovered;
    }
    Check(recovered == 2, "Present pair recovers after tail failure");
    if (scenario == 0) {
      const auto submitted = native::bridge_submitted_value.load();
      failed_list = native::bridge.ring[(submitted + 1) % native::kBridgeRing].list;
      Check(!frame_run() && failed_list == nullptr, "actual Bridge Close failure cancels Present recording");
      Check(native::bridge_submitted_value.load() == submitted && !native::worksets.contains(key),
          "cancel retires actual Present stream workset key without claiming GPU submission");
      for (const auto& slot : native::features.at(handle).slots) {
        Check(slot.handle == nullptr, "cancel releases actual Present stream model slots");
      }
      recovered = 0;
      for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 32 && recovered < 2; ++attempt) {
        if (frame_run()) ++recovered;
      }
      Check(recovered == 2, "Present pair recovers after cancelled recording");
      ComPtr<ID3D12Fence> revoked;
      Check(SUCCEEDED(native::bridge.device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&revoked))),
          "FG revocation test gate");
      Check(SUCCEEDED(native::bridge.queue->Wait(revoked.Get(), 1)), "hold done while FG authority changes");
      std::jthread revoke_off([&] {
        Sleep(200);
        set_options(sl::DLSSGMode::eOn);
        revoked->Signal(1);
      });
      Check(!frame_run(false), "successful On during done wait revokes Present copyback");
      revoke_off.join();
      WaitForGpu(native::bridge.fence_done12, native::bridge_submitted_value.load());
      Check(Read11(frame.Get(), stride) == source, "completed pair cannot copy after FG Off revocation");
      set_options(sl::DLSSGMode::eOff);
      recovered = 0;
      for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 32 && recovered < 2; ++attempt) {
        if (frame_run()) ++recovered;
      }
      Check(recovered == 2, "successful Off callback restores conditional pair after revocation");
      // A test-owned gate delays real GPU completion beyond the watchdog rescue.
      ComPtr<ID3D12Fence> stalled;
      Check(SUCCEEDED(native::bridge.device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&stalled))),
          "Present test completion gate");
      Check(SUCCEEDED(native::bridge.queue->Wait(stalled.Get(), 1)), "Present test stalls private queue");
      std::jthread release_stall([stalled] { Sleep(1600); stalled->Signal(1); });
      Check(!frame_run(false), "Watchdog rescue cannot deliver unfinished Present pair");
      const auto pending = native::bridge_submitted_value.load();
      Check(native::bridge.fence_done12->GetCompletedValue() < pending
          && native::bridge_pending_delivery_value == pending, "Present real done remains unproven after rescue");
      Check(Read11(frame.Get(), stride) == source, "Watchdog never copies unfinished pair to alias");
      const auto calls = model_calls;
      Check(!native::RunBridge(context, handle, &block, nullptr, &target) && model_calls == calls,
          "pending Present completion prevents twin reuse");
      native::RetireBridgeSetLocked(handle);
      const auto retained = native::bridge_retired.size();
      native::DrainBridgeRetired(native::bridge.fence_done12->GetCompletedValue());
      Check(retained != 0 && !native::bridge_retired.empty(), "in-flight Present twins cannot retire early");
      release_stall.join();
      WaitForGpu(native::bridge.fence_done12, pending);
      native::DrainBridgeRetired(pending);
      Check(native::bridge_retired.empty(), "Present twins retire after actual done");
      set_options(sl::DLSSGMode::eOn);
      Check(!native::PresentResolutionPairAllowed(target), "successful On revokes pair evidence");
      set_options(sl::DLSSGMode::eOff, sl::Result::eErrorInvalidParameter);
      Check(!native::PresentResolutionPairAllowed(target), "failed Off cannot overwrite successful On");
      set_options(sl::DLSSGMode::eOff);
    }
    native::ForgetSourceHandleLocked(handle);
    native::RetireBridgeSetLocked(handle);
    Check(native::DrainBridgeQueue(), "Present scenario final drain");
    native::present_streams.erase(frame.Get());
  }
  Check(native::DrainBridgeQueue(), "Present pair final queue drain");
  native::ReleaseBridge();
  Check(native::bridge_retired.empty(), "Present completed resources safely retired");
  fg::native_options::Stop();
  std::puts("PASS bridge-present-stage-resolution all six scenarios; NR stand-ins, no game/real-NR performance claim");
  return 0;
}
}  // namespace d3d11_present_stage_resolution_cases
