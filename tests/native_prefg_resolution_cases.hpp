/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <vector>

// Reuses the native Host, GPU stamps and readbacks. Only NR and the FG consumer
// are substitutes; scheduling, shaders, resource copies and queue fences are real.
namespace native_prefg_resolution_cases {
namespace addon = renodx::addons::dlss5;
namespace pair = same_stage_resolution_cases;
using stage_execution_cases::Capture;
using stage_execution_cases::Snapshot;

int Run(bool hardware) {
  try {
    const auto started = std::chrono::steady_clock::now();
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    Gpu gpu(hardware);
    model_gpu = &gpu;
    ComPtr<ID3D12InfoQueue> info;
    gpu.device.As(&info);
    UINT64 checked_messages = 0;
    const auto check_debug = [&] {
      if (info != nullptr) {
        while (checked_messages < info->GetNumStoredMessages()) {
          SIZE_T bytes = 0;
          info->GetMessage(checked_messages, nullptr, &bytes);
          std::vector<uint8_t> storage(bytes);
          auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
          Check(SUCCEEDED(info->GetMessage(checked_messages++, message, &bytes)), "PreFG debug message");
          if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
            std::cerr << "GPU ERROR fence=" << gpu.serial << ' ' << message->pDescription << '\n';
            Check(false, message->pDescription);
          }
        }
      }
      Check(SUCCEEDED(gpu.device->GetDeviceRemovedReason()), "PreFG GPU device remains available");
    };
    Check(!native::experimental_native_prefg_resolution.load(), "PreFG pair defaults off");
    native::enabled.store(true);
    native::hooks_enabled.store(true);
    native::hook_point.store(native::kHookPresent);
    native::present_pre_fg.store(1);
    native::present_guides.store(native::kPresentGuidesRequired);
    native::OnNativeFgOptions(true);
    native::present_fg_multiplier.store(2);
    native::dedupe_mode.store(addon::kDedupePerEvaluate);
    native::stack_passes.store(2);
    native::nr_follow_input_res.store(false);
    native::feed_mode.store(0);
    native::look_mode.store(false);
    native::detail_stability.store(1);
    native::gpu_timers_enabled.store(true);
    native::chained_temporal_history.store(true);
    native::transfer_strength.store(1.f);
    native::color_strength.store(1.f);
    native::intensity.store(.5f);
    native::preset.store(1);
    native::pass_stage[0].store(0);
    native::pass_follow[0].store(false);
    native::pass_intensity[0].store(.75f);
    native::pass_preset[0].store(2);
    native::pass_resolution_mode[0].store(2);
    native::pass_transfer_strength[0].store(1.f);
    native::pass_color_strength[0].store(1.f);
    native::pass_look_result[0].follow.store(true);
    native::direct_api.module = GetModuleHandleW(nullptr);
    native::direct_api.create = stage_execution_cases::Create;
    native::direct_api.evaluate = pair::Evaluate;
    native::direct_api.release = ReleaseModel;
    native::direct_load_state = native::DirectLoadState::Ready;
    native::direct_device = gpu.device.Get();
    native::direct_device->AddRef();
    native::direct_device_native = gpu.device.Get();
    native::direct_runtime_requirements.dual_models = true;
    native::parameter_runtime_ready = true;
    native::core_allocate_parameters = addon::bridge::AllocateOwnedParameters;
    native::core_destroy_parameters = addon::bridge::DestroyOwnedParameters;
    Check(native::InstallQueueCompletionHooks(gpu.queue.Get()), "PreFG actual Host queue hooks");
    Check(native::InstallCommandListStateHooks(gpu.list.Get()), "PreFG actual Host list hooks");

    // Deliberately bypass Gpu::Submit's synthetic submission bookkeeping.
    // Production Execute hooks must attach their own completion proof.
    const auto submit = [&](bool reset = true, bool close = true) {
      if (close) Check(SUCCEEDED(gpu.list->Close()), "PreFG Host Close");
      ID3D12CommandList* lists[]{gpu.list.Get()};
      gpu.queue->ExecuteCommandLists(1, lists);
      Check(SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial)), "PreFG Host Fence Signal");
      const auto event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      Check(event != nullptr, "PreFG Host completion event");
      const auto signal = gpu.fence->SetEventOnCompletion(gpu.serial, event);
      const auto wait = (SUCCEEDED(signal) ? WaitForSingleObject(event, 10000) : WAIT_FAILED);
      CloseHandle(event);
      Check(wait == WAIT_OBJECT_0 && gpu.fence->GetCompletedValue() >= gpu.serial,
          "PreFG actual GPU completion Fence");
      check_debug();
      if (reset) {
        Check(SUCCEEDED(gpu.allocator->Reset()), "PreFG Host allocator Reset");
        Check(SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)), "PreFG Host list Reset");
        gpu.uploads.clear();
      }
      native::DrainRetiredResources(true);
      addon::gpu_timers::Poll(true, false);
    };
    submit();
    const auto texture = [&](DXGI_FORMAT format, uint32_t extent = 16) {
      const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
      const D3D12_RESOURCE_DESC desc{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
          .Width = extent, .Height = extent, .DepthOrArraySize = 1, .MipLevels = 1,
          .Format = format, .SampleDesc = {1, 0}};
      ComPtr<ID3D12Resource> result;
      Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
          D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&result))), "PreFG real texture");
      return result;
    };
    auto frame = texture(DXGI_FORMAT_R8G8B8A8_UNORM);
    auto motion = texture(DXGI_FORMAT_R16G16_FLOAT);
    auto depth = texture(DXGI_FORMAT_R32_FLOAT);
    for (auto* resource : {frame.Get(), motion.Get(), depth.Get()}) {
      gpu.Write(resource, resource == frame.Get() ? 64 : 0);
      gpu.Transition(resource, D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
    }
    submit();
    auto& owner = native::present_streams[nullptr];
    owner.device_identity = renodx::utils::directx::NativeIdentity(gpu.device.Get());
    owner.backbuffer_width = owner.backbuffer_height = 16;
    owner.backbuffer_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    owner.prefg_target_valid = true;
    owner.prefg_target.diagnostic.bypass_host_look = true;
    owner.prefg_target.pass_transfer_strength[0] = 1.f;
    owner.prefg_target.pass_color_strength[0] = 1.f;
    native::codec_mode.store(0);
    addon::bridge::OwnedParameters parameters;
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_Backbuffer, frame.Get());
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_Depth, depth.Get());
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MVecs, motion.Get());
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MVecsSubrectWidth, 16u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MVecsSubrectHeight, 16u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MvecScaleX, 1.f);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MvecScaleY, 1.f);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 1u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, 1u);
    const auto* handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xF067});
    native::RegisterCreatedFeatureLocked(handle, NVSDK_NGX_Feature_FrameGeneration, &parameters);
    native::native_fg_owner = handle;
    native::native_fg_owns.store(true);
    pair::production_stage_extent = 16;
    pair::CopyObservation copies(gpu);
    unsigned long long frame_id = 1;
    Snapshot consumed;
    Snapshot original_frame;
    uint32_t consumers = 0;
    bool replaced = false;
    const auto record = [&](bool new_frame = true, bool fg_success = true) {
      if (new_frame) {
        ++frame_id;
        gpu.Transition(frame.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COMMON);
        if (frame->GetDesc().Format == DXGI_FORMAT_R8G8B8A8_UNORM) {
          gpu.Write(frame.Get(), 64);
        } else {
          const auto desc = frame->GetDesc();
          D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
          uint64_t bytes = 0;
          gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
          auto upload = gpu.Buffer(bytes, D3D12_HEAP_TYPE_UPLOAD);
          uint8_t* mapped = nullptr;
          Check(SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))), "scRGB source upload");
          std::memset(mapped, 0, static_cast<size_t>(bytes));
          // 0.8 nit: a finite nonblack Display proxy below the shoulder.
          constexpr uint16_t rgba[]{0x211f, 0x211f, 0x211f, 0x3c00};
          for (uint32_t y = 0; y < 16; ++y) {
            for (uint32_t x = 0; x < 16; ++x) {
              std::memcpy(mapped + footprint.Offset + y * footprint.Footprint.RowPitch + x * sizeof(rgba),
                  rgba, sizeof(rgba));
            }
          }
          upload->Unmap(0, nullptr);
          gpu.Transition(frame.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
          const D3D12_TEXTURE_COPY_LOCATION source{.pResource = upload.Get(),
              .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = footprint};
          const D3D12_TEXTURE_COPY_LOCATION destination{.pResource = frame.Get(),
              .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
          gpu.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
          gpu.Transition(frame.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
          gpu.uploads.push_back(upload);
        }
        gpu.Transition(frame.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
        original_frame = Capture(gpu, frame.Get(), NATIVE_INPUT_STATE);
      }
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, frame_id);
      pair::evaluations.clear();
      replaced = false;
      auto* original = native::RunNativePreFg(gpu.list.Get(), &parameters, handle, &replaced);
      Check(original == nullptr, "single Backbuffer contract never acquires HUDLess");
      // A real consumer copy follows RunNativePreFg on the actual Host list.
      // Record it even on decline to catch partial output leaking into FG.
      auto* input = native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_Backbuffer);
      Check(input == frame.Get() && input->GetDesc().Width == 16 && input->GetDesc().Height == 16,
          "FG consumes actual final full-size Backbuffer");
      consumed = Capture(gpu, input, NATIVE_INPUT_STATE);
      ++consumers;
      native::RestoreNativePreFg(gpu.list.Get(), &parameters, original, fg_success, handle, replaced);
      return replaced;
    };
    const auto mean = [](const Snapshot& image) {
      const auto pixels = image.Pixels();
      std::vector<float> row(image.width * 4);
      double total = 0.;
      for (uint32_t y = 0; y < image.height; ++y) {
        Check(addon::screenshot::internal::DecodeDiagnosticRow(pixels.data()
            + static_cast<size_t>(y) * image.width * image.pixel_bytes, image.width, image.format, row.data()),
            "PreFG real readback decode");
        for (uint32_t x = 0; x < image.width; ++x) {
          for (uint32_t c = 0; c < 3; ++c) {
            Check(std::isfinite(row[x * 4 + c]), "PreFG finite real pixels");
            total += row[x * 4 + c];
          }
        }
      }
      return static_cast<float>(total / (image.width * image.height * 3));
    };
    const auto raw = [&] {
      Check(consumed.Pixels() == original_frame.Pixels(),
          "declined/partial PreFG keeps every original Backbuffer byte for FG");
    };
    const auto report_resources = [&](const char* encoding) {
      const auto& stream = native::fg_service::streams.at({true, reinterpret_cast<uintptr_t>(&owner)});
      const auto& work = native::worksets.at({stream.roles[0].Handle(),
          reinterpret_cast<const ID3D12Resource*>(&stream.roles[0])});
      Check(work.resolution_tail != nullptr && stream.cached != nullptr,
          "mature heterogeneous sample owns an actual private tail and FG slot");
      std::array<uint64_t, 4> active_bytes{};
      uint64_t pool_bytes = 0;
      for (const auto& slot : stream.slots) {
        const std::array resources{slot.color[0], slot.color[1], slot.original_backbuffer, slot.original_hudless};
        for (size_t index = 0; index < resources.size(); ++index) {
          if (resources[index] == nullptr) continue;
          const auto desc = resources[index]->GetDesc();
          const auto allocation = gpu.device->GetResourceAllocationInfo(0, 1, &desc);
          Check(allocation.SizeInBytes != UINT64_MAX, "real FG slot allocation information");
          pool_bytes += allocation.SizeInBytes;
          if (&slot == stream.cached) active_bytes[index] = allocation.SizeInBytes;
        }
      }
      Check(active_bytes[0] != 0 && active_bytes[1] == 0 && active_bytes[2] != 0 && active_bytes[3] == 0
          && stream.cached->coupled_hudless == nullptr && stream.cached->coupled_ui == nullptr,
          "single-plane prototype allocates only private Backbuffer and original snapshot colors");
      std::cout << "RESOURCES encoding=" << encoding
          << " scale1=" << native::nr_resolution_scale.load() << " scale2=" << native::pass_resolution_scale[0].load()
          << " root_including_tail_bytes=" << work.allocated_bytes
          << " extra_resolution_tail_bytes=" << work.resolution_tail->allocated_bytes
          << " slot_color0_bytes=" << active_bytes[0] << " slot_color1_bytes=" << active_bytes[1]
          << " slot_original_backbuffer_bytes=" << active_bytes[2] << " slot_original_hudless_bytes=" << active_bytes[3]
          << " slot_pool_color_bytes=" << pool_bytes
          << "; actual allocation information; excludes model runtime and fixture allocations\n";
    };
    const auto foreign_cache_probe = [&] {
      const auto saved_list = gpu.list;
      const auto saved_allocator = gpu.allocator;
      const auto saved_consumer = consumed;
      const auto saved_replaced = replaced;
      const auto saved_index = native::GetUInt(&parameters, NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex);
      ComPtr<ID3D12CommandAllocator> allocator;
      ComPtr<ID3D12GraphicsCommandList> list;
      Check(SUCCEEDED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
          IID_PPV_ARGS(&allocator))), "foreign cache list allocator");
      Check(SUCCEEDED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
          allocator.Get(), nullptr, IID_PPV_ARGS(&list))), "foreign cache command list");
      Check(native::InstallCommandListStateHooks(list.Get()) && SUCCEEDED(list->Close())
          && SUCCEEDED(list->Reset(allocator.Get(), nullptr)), "foreign cache state envelope");
      const auto calls_before = model_calls;
      gpu.list = list;
      gpu.allocator = allocator;
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 2u);
      const auto before = std::chrono::steady_clock::now();
      const bool served = record(false);
      const auto duration = std::chrono::steady_clock::now() - before;
      // This consumer was never executed; drop its generation and restore the
      // producer fixture before asserting so failure cannot strand the Host.
      Check(SUCCEEDED(list->Close()) && SUCCEEDED(list->Reset(allocator.Get(), nullptr))
          && SUCCEEDED(list->Close()), "discard declined foreign consumer list");
      gpu.list = saved_list;
      gpu.allocator = saved_allocator;
      consumed = saved_consumer;
      replaced = saved_replaced;
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, saved_index);
      Check(!served && model_calls == calls_before, "different unproven list declines cache without recording NR");
      Check(duration < std::chrono::seconds(1), "different unproven list declines without CPU Fence wait");
    };
    native::nr_resolution_scale.store(1.f);
    native::pass_resolution_scale[0].store(.67f);
    record(); submit(); raw();
    Check(!replaced && pair::evaluations.empty(), "disabled prototype cannot execute heterogeneous PreFG NR");
    native::experimental_native_prefg_resolution.store(true);
    {
      const auto before = model_calls;
      bool unknown_replaced = false;
      const auto* unknown = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xF068});
      auto* original = native::RunNativePreFg(gpu.list.Get(), &parameters, unknown, &unknown_replaced);
      consumed = Capture(gpu, frame.Get(), NATIVE_INPUT_STATE);
      native::RestoreNativePreFg(gpu.list.Get(), &parameters, original, true, unknown, unknown_replaced);
      submit(); raw();
      Check(original == nullptr && !unknown_replaced && model_calls == before,
          "unknown FG handle never gains prototype authority by guessed parameter shape");
    }
    {
      addon::bridge::OwnedParameters missing_frame;
      missing_frame.Set(NVSDK_NGX_DLSSG_Parameter_Backbuffer, frame.Get());
      missing_frame.Set(NVSDK_NGX_DLSSG_Parameter_Depth, depth.Get());
      missing_frame.Set(NVSDK_NGX_DLSSG_Parameter_MVecs, motion.Get());
      missing_frame.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 1u);
      const auto before = model_calls;
      bool missing_replaced = false;
      auto* original = native::RunNativePreFg(gpu.list.Get(), &missing_frame, handle, &missing_replaced);
      consumed = Capture(gpu, frame.Get(), NATIVE_INPUT_STATE);
      native::RestoreNativePreFg(gpu.list.Get(), &missing_frame, original, true, handle, missing_replaced);
      submit(); raw();
      Check(original == nullptr && !missing_replaced && model_calls == before,
          "missing BackbufferFrameID cannot borrow SDK/present fallback");
    }
    for (const char* key : {NVSDK_NGX_DLSSG_Parameter_HUDLess, NVSDK_NGX_DLSSG_Parameter_UI,
                           NVSDK_NGX_DLSSG_Parameter_UIAlpha}) {
      parameters.Set(key, frame.Get());
      record(); submit(); raw();
      Check(!replaced && pair::evaluations.empty(), "even aliased HUDLess/UI excludes single-plane prototype");
      parameters.Set(key, static_cast<ID3D12Resource*>(nullptr));
    }
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_NotRenderingGameFrames, 1u);
    record(); submit(); raw();
    Check(!replaced && pair::evaluations.empty(), "non-game frame excludes prototype");
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_NotRenderingGameFrames, 0u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_Depth, static_cast<ID3D12Resource*>(nullptr));
    record(); submit(); raw();
    Check(!replaced && pair::evaluations.empty(), "missing real Depth never uses neutral guides");
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_Depth, depth.Get());
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MVecs, static_cast<ID3D12Resource*>(nullptr));
    record(); submit(); raw();
    Check(!replaced && pair::evaluations.empty(), "missing real motion never uses neutral guides");
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MVecs, motion.Get());
    for (const char* key : {"DLSSG.MVecsSubrectBaseX", "DLSSG.DepthSubrectBaseY"}) {
      parameters.Set(key, 16u);
      record(); submit(); raw();
      Check(!replaced && pair::evaluations.empty(), "real Guide subrect must fit actual GPU texture");
      parameters.Set(key, 0u);
    }
    const auto valid_id = frame_id;
    frame_id = 0;
    record(false); submit(); raw();
    Check(!replaced && pair::evaluations.empty(), "zero BackbufferFrameID cannot borrow SDK/present fallback");
    frame_id = valid_id;
    for (uint32_t index : {0u, 2u}) {
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, index);
      record(); submit(); raw();
      Check(!replaced && pair::evaluations.empty(), "invalid initial index cannot create application NR");
    }
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 1u);
    owner.d3d11 = true;
    record(); submit(); raw();
    Check(!replaced && pair::evaluations.empty(), "DX11 owner cannot enable native DX12 prototype");
    owner.d3d11 = false;
    addon::fg::xefg::internal::owns_stream.store(true);
    addon::fg::xefg::internal::requested.store(+[]() noexcept { return true; });
    record(); submit(); raw();
    Check(!replaced && pair::evaluations.empty(), "XeFG ownership cannot enter native DX12 prototype");
    addon::fg::xefg::internal::owns_stream.store(false);
    addon::fg::xefg::internal::requested.store(nullptr);
    native::pass_stage[0].store(2);
    record(); submit(); raw();
    Check(!replaced && pair::evaluations.empty(), "mixed Upscaled/Present plan cannot enter prototype");
    native::pass_stage[0].store(0);
    uint32_t successful = 0;
    const auto mature = [&] {
      uint32_t steady = 0;
      for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 40 && steady < 2; ++attempt) {
        record();
        submit();
        if (!replaced) {
          raw();
          const auto calls_before_retry = model_calls;
          record(false); submit(); raw();
          Check(!replaced && model_calls == calls_before_retry,
              "warming/unready FrameID gets at most one real-frame attempt");
          continue;
        }
        Check(pair::evaluations.size() == 2, "only complete pair reaches FG");
        const auto& first = pair::evaluations[0];
        const auto& second = pair::evaluations[1];
        Check(first.preset == 1 && second.preset == 2, "PreFG A then B model order");
        // Slider changes retain the previous valid dimensions during the
        // production debounce; only requested-size frames count as steady.
        if (first.input.width != static_cast<uint32_t>(std::lround(16 * native::nr_resolution_scale.load()))
            || second.input.width != static_cast<uint32_t>(std::lround(16 * native::pass_resolution_scale[0].load()))) continue;
        Check(first.input.height == first.input.width && second.input.height == second.input.width,
            "PreFG actual per-model GPU input dimensions");
        Check(first.motion != nullptr && first.depth != nullptr && second.motion != nullptr && second.depth != nullptr,
            "both PreFG layers receive actual guides");
        Check(mean(consumed) > 0.f, "FG real consumer output is nonblack");
        if (first.reset == 0 && second.reset == 0) ++steady;
        ++successful;
      }
      Check(steady == 2, "PreFG bounded maturity and temporal History continuation");
    };
    for (uint32_t discarded_models : {0u, 1u}) {
      ++native::configuration_generation;
      bool discarded = false;
      for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 12 && !discarded; ++attempt) {
        const auto submitted_before = native::native_fg_submitted_frames.load();
        record();
        if (!replaced && pair::evaluations.size() == discarded_models) {
          Check(SUCCEEDED(gpu.list->Close()) && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)),
              "discard feature-only/partial warming Host recording before Execute");
          native::DrainRetiredResources(true);
          Check(native::native_fg_submitted_frames.load() == submitted_before,
              "unsubmitted feature-only/partial warming cannot count a production frame");
          discarded = true;
        } else {
          submit();
          if (!replaced) raw();
        }
      }
      Check(discarded, "bounded fixture observes feature-only and partial warming discard");
      const auto handles_before_recovery = next_handle;
      mature();
      Check(next_handle > handles_before_recovery, "discarded warming ledger rebuilds NR feature before recovery");
      std::cout << "PASS warming-discard evaluated_models=" << discarded_models << '\n';
    }
    std::array<uint32_t, 2> generations{};
    uint32_t phase = 0;
    for (const auto scales : {std::array{1.f, .67f}, std::array{1.f, .5f}, std::array{.67f, 1.f}}) {
      native::nr_resolution_scale.store(scales[0]);
      native::pass_resolution_scale[0].store(scales[1]);
      mature();
      if (phase == 1) {
        Check(pair::evaluations[0].generation == generations[0] && pair::evaluations[0].reset == 0,
            "tail-only resize retains upstream model and History");
        Check(pair::evaluations[1].generation != generations[1], "tail-only resize rebuilds downstream model");
      }
      generations = {pair::evaluations[0].generation, pair::evaluations[1].generation};
      ++phase;
      pair::stamps = {0x3400, 0x3200};
      const auto sample_calls = model_calls;
      const auto sample_copies = pair::production_texture_copies;
      const auto sample_full_copies = pair::production_full_stage_copies;
      Check(record(), "PreFG low upstream stamp accepted");
      Check(model_calls - sample_calls == 2 && pair::evaluations.size() == 2,
          "one complete new producer records exactly two NR model calls");
      const auto copies_per_frame = pair::production_texture_copies - sample_copies;
      const auto full_copies_per_frame = pair::production_full_stage_copies - sample_full_copies;
      submit();
      report_resources("SDR");
      std::cout << "METRICS complete_frame encoding=SDR scale1=" << scales[0] << " scale2=" << scales[1]
          << " nr_api_calls=2 producer_submissions=1 production_texture_copies=" << copies_per_frame
          << " production_fullsize_copies=" << full_copies_per_frame
          << "; fixture readbacks/uploads and model stamp copies excluded\n";
      const auto low_input = pair::evaluations[1].input.Pixels();
      const float low = mean(pair::evaluations[1].input);
      const auto low_final = consumed.Pixels();
      const float final_low = mean(consumed);
      pair::stamps[0] = 0x3a00;
      Check(record(), "PreFG high upstream stamp accepted"); submit();
      const float high = mean(pair::evaluations[1].input);
      Check(high > low && pair::evaluations[1].input.Pixels() != low_input,
          "model2 real GPU input follows model1 GPU stamp");
      pair::stamps = {0x3400, 0x3400};
      Check(record(), "PreFG high tail stamp accepted"); submit();
      Check(mean(consumed) > final_low && consumed.Pixels() != low_final,
          "real FG consumer reads restored full-size output following model2 stamp");
      std::cout << "HANDOFF native-prefg scale1=" << scales[0] << " scale2=" << scales[1]
          << " input2_low=" << low << " input2_high=" << high
          << " fg_low=" << final_low << " fg_high=" << mean(consumed) << '\n';
      pair::stamps = {0x3400, 0x3600};
    }

    const auto zero_index_calls = model_calls;
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 0u);
    const auto attempt_before_zero = native::fg_service::streams.at({true,
        reinterpret_cast<uintptr_t>(&owner)}).native_pair_attempt_frame;
    record(); submit(); raw();
    Check(!replaced && model_calls == zero_index_calls
        && native::fg_service::streams.at({true, reinterpret_cast<uintptr_t>(&owner)}).native_pair_attempt_frame
            == attempt_before_zero, "index zero cannot run NR or consume the real-frame attempt");
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 1u);
    Check(record(false), "same new FrameID can start its first actual attempt at index one"); submit();
    Check(model_calls == zero_index_calls + 2, "index one records exactly one pair after declined index zero");
    std::cout << "PASS current-index index0=declined index1=one_pair\n";

    Check(record(), "disable boundary starts with complete producer");
    submit(false);
    Check(SUCCEEDED(gpu.allocator->Reset()) && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)),
        "Reset completed producer before disabling prototype");
    native::experimental_native_prefg_resolution.store(false);
    native::DrainRetiredResources(true);
    const auto& disabled_stream = native::fg_service::streams.at({true, reinterpret_cast<uintptr_t>(&owner)});
    Check(!disabled_stream.native_pair_completion.empty() && !disabled_stream.native_pair_discarded,
        "disable before prune preserves actual completed producer receipt");
    const auto disabled_calls = model_calls;
    Check(!record(false), "disable rolls original raw pixels back for the same legitimate source"); submit(); raw();
    native::stack_passes.store(1);
    Check(!record(false), "same FrameID remains attempted across pair to single transition"); submit(); raw();
    Check(model_calls == disabled_calls, "pair to single cannot retry NR in the same real frame");
    native::stack_passes.store(2);
    native::experimental_native_prefg_resolution.store(true);
    mature();
    std::cout << "PASS disable-before-prune raw-rollback same-frame-pair-to-single-no-retry\n";

    for (uint32_t replacement = 0; replacement < 3; ++replacement) {
      const auto format = (replacement == 2 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM);
      const auto extent = (replacement == 1 ? 8u : 16u);
      auto other = texture(format, extent);
      gpu.Write(other.Get(), 112);
      gpu.Transition(other.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
      submit();
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_Backbuffer, other.Get());
      owner.backbuffer_width = owner.backbuffer_height = extent;
      owner.backbuffer_format = format;
      ++native::configuration_generation;
      const auto before = Capture(gpu, other.Get(), NATIVE_INPUT_STATE);
      const auto calls_before = model_calls;
      bool other_replaced = false;
      auto* original = native::RunNativePreFg(gpu.list.Get(), &parameters, handle, &other_replaced);
      const auto after = Capture(gpu, other.Get(), NATIVE_INPUT_STATE);
      native::RestoreNativePreFg(gpu.list.Get(), &parameters, original, true, handle, other_replaced);
      submit();
      Check(original == nullptr && !other_replaced && model_calls == calls_before && before.Pixels() == after.Pixels(),
          "raw snapshot cannot overwrite a different Backbuffer identity/dimensions/format");
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_Backbuffer, frame.Get());
      owner.backbuffer_width = owner.backbuffer_height = 16;
      owner.backbuffer_format = DXGI_FORMAT_R8G8B8A8_UNORM;
      mature();
    }
    std::cout << "PASS raw-rollback rejects other identity/extent/format\n";

    // A duplicate in the very same live recording can reuse its complete pair.
    Check(record(), "same-generation duplicate starts with a complete pair");
    auto first_observations = std::move(pair::evaluations);
    const auto first_consumer = consumed;
    const auto duplicate_calls = model_calls;
    const auto duplicate_identity = submission::RecordingIdentity(gpu.list.Get());
    Check(record(false), "same-list same-generation same-frame cache reuse");
    Check(model_calls == duplicate_calls && submission::RecordingIdentity(gpu.list.Get()) == duplicate_identity,
        "same live recording duplicate adds no NR model call");
    foreign_cache_probe();
    submit();
    Check(consumed.Pixels() == first_consumer.Pixels(), "same-generation FG consumers read identical complete result");

    // MFG repeats must use completion-proven cache without running NR again.
    const auto calls = model_calls;
    for (uint32_t multiplier : {2u, 4u, 3u}) {
      native::present_fg_multiplier.store(multiplier);
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, multiplier - 1);
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 2u);
      Check(record(false), "same-frame MFG completion-proven cache"); submit();
      Check(model_calls == calls, "MFG multiplier/cache never adds NR evaluations");
    }
    std::cout << "PASS MFG multipliers=2,4,3 nr_api_delta=0; cached consumers do not replay producer list\n";
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 1u);
    const auto submitted_before_discard = native::native_fg_submitted_frames.load();
    Check(record(), "discard starts with a complete pair");
    Check(native::native_fg_submitted_frames.load() == submitted_before_discard,
        "record/Restore alone never commits application NR submission");
    Check(SUCCEEDED(gpu.list->Close()) && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)),
        "Host discards complete pair without GPU submission");
    Check(native::native_fg_submitted_frames.load() == submitted_before_discard,
        "discarded pair cannot count as submitted");
    // Keep fixture uploads alive until the next actual Fence; discarded copies
    // have not executed, and the raw source will be rewritten on the new frame.
    mature();

    ComPtr<ID3D12QueryHeap> replay_query;
    const D3D12_QUERY_HEAP_DESC query_desc{.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = 1};
    Check(SUCCEEDED(gpu.device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&replay_query))), "PreFG GPU replay query");
    auto replay_readback = gpu.Buffer(sizeof(uint64_t), D3D12_HEAP_TYPE_READBACK);
    const auto timestamp = [&] {
      uint64_t* mapped = nullptr;
      Check(SUCCEEDED(replay_readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped))), "PreFG GPU timestamp map");
      const auto value = *mapped;
      replay_readback->Unmap(0, nullptr);
      return value;
    };
    model_execution_query = replay_query.Get();
    model_execution_readback = replay_readback.Get();
    Check(record(), "replay starts with complete real NR recording");
    auto& resident = native::fg_service::streams.at({true, reinterpret_cast<uintptr_t>(&owner)});
    auto* replay_color = resident.cached->color[0];
    const auto recording = submission::RecordingIdentity(gpu.list.Get());
    const auto replay_calls = model_calls;
    submit(false);
    const auto first_execution = timestamp();
    const auto replay_submitted = native::native_fg_submitted_frames.load();
    Check(!submission::ResourceReleasable(replay_color), "completed replayable generation retains resources");
    submit(false, false);
    const auto replay_execution = timestamp();
    Check(first_execution != 0 && replay_execution > first_execution, "replay executes actual GPU model stamp commands again");
    Check(model_calls == replay_calls && native::native_fg_submitted_frames.load() == replay_submitted
        && submission::RecordingIdentity(gpu.list.Get()) == recording,
        "replay adds no NGX calls/application NR frame and preserves recording generation");
    std::vector<std::pair<addon::FenceRef, uint64_t>> receipts;
    Check(submission::UseSubmissionTokens(replay_color, &receipts) == submission::UseSubmission::kTokens
        && receipts.size() == 2, "replay has two real queue completion receipts");
    for (const auto& [fence, value] : receipts) {
      Check(fence->GetCompletedValue() != UINT64_MAX && fence->GetCompletedValue() >= value,
          "each replay receipt has completed Fence proof");
    }
    std::cout << "PASS producer-replay first_gpu_timestamp=" << first_execution
        << " replay_gpu_timestamp=" << replay_execution
        << " nr_api_delta=0 application_frame_delta=0 actual_submissions=2; each list execution reruns both GPU model stamps\n";
    Check(SUCCEEDED(gpu.allocator->Reset()) && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)),
        "Host retires replayable generation");
    model_execution_query = nullptr;
    model_execution_readback = nullptr;
    gpu.uploads.clear();
    const auto replay_pixels = consumed.Pixels();
    const auto calls_before_replayed_cache = model_calls;
    Check(!record(false), "replayed producer cannot authorize cache across list generations");
    submit();
    Check(model_calls == calls_before_replayed_cache && consumed.Pixels() == replay_pixels,
        "multiple producer receipts decline cache without NR calls or Backbuffer edits");
    mature();
    fail_call = model_calls + 2;
    record(); submit();
    Check(!replaced && pair::evaluations.size() == 2, "second model failure declines entire pair"); raw();
    const auto failed_calls = model_calls;
    record(false); submit(); raw();
    Check(!replaced && model_calls == failed_calls, "failed FrameID cannot retry NR before next actual frame");
    fail_call = 0;
    mature();
    const auto epoch = native::configuration_generation.fetch_add(1);
    record(); submit();
    if (!replaced) raw();
    Check(native::configuration_generation.load() == epoch + 1, "configuration generation preserved");
    mature();
    for (uint32_t mutation = 0; mutation < 5; ++mutation) {
      bool changed = false;
      model_ui_observer = [&] {
        if (changed) return;
        changed = true;
        switch (mutation) {
          case 0: ++native::configuration_generation; break;
          case 1: ++native::native_fg_options_generation; break;
          case 2: ++native::history_reset_epoch; break;
          case 3: ++native::nr_resolution_commit_epochs[1]; break;
          case 4: native::pass_resolution_scale[0].store(.5f); break;
        }
      };
      record();
      model_ui_observer = {};
      submit(); raw();
      Check(changed && !replaced, "mid-record config/options/history/resolution/epoch change stays private");
      mature();
    }
    Check(record(true, false), "failed FG consumer starts with complete pair");
    const auto failed_fg_consumer = consumed;
    consumed = Capture(gpu, frame.Get(), NATIVE_INPUT_STATE);
    submit(); raw();
    mature();

    // Hold actual queued work behind a GPU fence, then retire its owner.
    // Neither the model nor the tail scratch may be freed before execution.
    Check(record(), "in-flight retirement starts with complete pair");
    auto& live = native::fg_service::streams.at({true, reinterpret_cast<uintptr_t>(&owner)});
    const auto role_handle = live.roles[0].Handle();
    const auto role_key = reinterpret_cast<const ID3D12Resource*>(&live.roles[0]);
    const auto& work = native::worksets.at({role_handle, role_key});
    Check(work.resolution_tail != nullptr, "in-flight retirement owns different-size tail");
    auto* tail_proxy = work.resolution_tail->proxy;
    auto* tail_model = native::features.at(role_handle).slots[1].handle;
    Check(tail_model != nullptr, "in-flight retirement has downstream model");
    const auto releases_before_retirement = model_released_handles.size();
    ComPtr<ID3D12Fence> gate;
    Check(SUCCEEDED(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))), "PreFG retirement GPU gate");
    Check(SUCCEEDED(gpu.queue->Wait(gate.Get(), 1)), "hold actual Host queue before execution");
    bool queued = false;
    auto gated_observations = std::move(pair::evaluations);
    try {
      Check(SUCCEEDED(gpu.list->Close()), "PreFG gated Close");
      ID3D12CommandList* lists[]{gpu.list.Get()};
      gpu.queue->ExecuteCommandLists(1, lists);
      Check(SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial)), "PreFG gated Fence Signal");
      queued = true;
      Check(gpu.fence->GetCompletedValue() < gpu.serial, "actual Host work remains incomplete behind gate");
      foreign_cache_probe();
      native::OnNativePresentStreamRetired(&owner);
      native::DrainRetiredResources(true);
      Check(!submission::ResourceReleasable(tail_proxy) && !submission::ResourceReleasable(tail_model),
          "incomplete owner retirement retains tail resource and downstream model");
      Check(std::find(model_released_handles.begin() + releases_before_retirement, model_released_handles.end(), tail_model)
          == model_released_handles.end(), "in-flight downstream model has not been released");
    } catch (...) {
      gate->Signal(1);
      if (queued) {
        const auto event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event != nullptr) {
          if (SUCCEEDED(gpu.fence->SetEventOnCompletion(gpu.serial, event))) WaitForSingleObject(event, 10000);
          CloseHandle(event);
        }
      }
      throw;
    }
    Check(SUCCEEDED(gate->Signal(1)), "release actual Host GPU gate");
    const auto completed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Check(completed != nullptr, "retirement completion event");
    const auto signal = gpu.fence->SetEventOnCompletion(gpu.serial, completed);
    const auto waited = (SUCCEEDED(signal) ? WaitForSingleObject(completed, 10000) : WAIT_FAILED);
    CloseHandle(completed);
    Check(waited == WAIT_OBJECT_0, "retired owner GPU work completes");
    check_debug();
    Check(!submission::ResourceReleasable(tail_proxy), "completed Host list still protects replayable tail");
    Check(SUCCEEDED(gpu.allocator->Reset()) && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)),
        "Reset retires final Host generation");
    native::DrainRetiredResources(true);
    Check(submission::ResourceReleasable(tail_proxy) && submission::ResourceReleasable(tail_model),
        "completion plus Reset permits final retirement");

    frame = texture(DXGI_FORMAT_R16G16B16A16_FLOAT);
    gpu.Transition(frame.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
    owner.backbuffer_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    owner.prefg_target.hdr_mode = 1;
    owner.prefg_target.units = {native::codec::Encoding::Linear, true, 80.f, false};
    owner.prefg_target.hdr_white_nits = 80.f;
    native::codec_mode.store(3);
    native::SnapshotPresentRestoreConfiguration(&owner.prefg_target);
    owner.prefg_target.diagnostic.bypass_host_look = true;
    owner.prefg_target.hdr_encoding = native::V6EncodingConstant(owner.prefg_target.units);
    owner.prefg_target.hdr_dark_gate = native::V6DarkGate(owner.prefg_target.units);
    owner.prefg_target.hdr_pedestal_cap = native::codec::BlackPedestalCap(owner.prefg_target.units);
    Check(owner.prefg_target.hdr_curve == 1 && owner.prefg_target.hdr_encoding == 1,
        "scRGB freezes actual Display curve and encoding");
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_Backbuffer, frame.Get());
    native::RegisterCreatedFeatureLocked(handle, NVSDK_NGX_Feature_FrameGeneration, &parameters);
    native::native_fg_owner = handle;
    native::native_fg_owns.store(true);
    for (const auto scales : {std::array{1.f, .67f}, std::array{.67f, 1.f}}) {
      native::nr_resolution_scale.store(scales[0]);
      native::pass_resolution_scale[0].store(scales[1]);
      pair::stamps = {0x3400, 0x3200};
      mature();
      report_resources("scRGB");
      const auto low_input = pair::evaluations[1].input.Pixels();
      const auto low_final = consumed.Pixels();
      const float input_low = mean(pair::evaluations[1].input);
      const float output_low = mean(consumed);
      pair::stamps[0] = 0x3a00;
      const auto sample_calls = model_calls;
      const auto sample_copies = pair::production_texture_copies;
      const auto sample_full_copies = pair::production_full_stage_copies;
      Check(record(), "scRGB upstream stamp accepted");
      Check(model_calls - sample_calls == 2 && pair::evaluations.size() == 2,
          "one complete scRGB producer records exactly two NR model calls");
      const auto copies_per_frame = pair::production_texture_copies - sample_copies;
      const auto full_copies_per_frame = pair::production_full_stage_copies - sample_full_copies;
      submit();
      std::cout << "METRICS complete_frame encoding=scRGB scale1=" << scales[0] << " scale2=" << scales[1]
          << " nr_api_calls=2 producer_submissions=1 production_texture_copies=" << copies_per_frame
          << " production_fullsize_copies=" << full_copies_per_frame
          << "; fixture readbacks/uploads and model stamp copies excluded\n";
      Check(mean(pair::evaluations[1].input) > input_low && pair::evaluations[1].input.Pixels() != low_input,
          "scRGB model2 actual input changes positively with model1 GPU stamp");
      pair::stamps = {0x3400, 0x3400};
      Check(record(), "scRGB downstream stamp accepted"); submit();
      Check(mean(consumed) > output_low && consumed.Pixels() != low_final,
          "scRGB FG consumer changes positively without black/shoulder/gain-clamp false oracle");
      std::cout << "HANDOFF native-prefg scRGB scale1=" << scales[0] << " scale2=" << scales[1]
          << " input2_low=" << input_low << " fg_low=" << output_low << " fg_high=" << mean(consumed) << '\n';
    }
    native::OnNativePresentStreamRetired(&owner);
    native::DrainRetiredResources(true);
    check_debug();
    const auto memory = addon::telemetry::QueryVideoMemory(gpu.device.Get());
    std::cout << "PASS native-prefg-resolution hardware=" << hardware << " steady_pairs_observed=" << successful
        << " nr_api_calls_total=" << model_calls << " fg_consumer_copies_recorded=" << consumers << " host_fences=" << gpu.serial
        << " copy_calls_observed_total=" << recorded_copies << " elapsed_ms="
        << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()
        << " measured_gpu_us_total=" << addon::gpu_timers::measured_total_us
        << " dxgi_local_usage_bytes=" << memory.local_usage << " dxgi_local_budget_bytes=" << memory.local_budget
        << "; totals include warming/failure/discard/debounce; GPU body executions follow submissions/replay,"
           " not API-call totals; NVIDIA NR and FG algorithms substituted; no game or visual acceptance\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL native-prefg-resolution: " << error.what() << '\n';
    return 1;
  }
}
}  // namespace native_prefg_resolution_cases
