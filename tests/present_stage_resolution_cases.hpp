/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

// Reuse the native model, readback and submission fixtures; only NVIDIA NR is substituted.
namespace present_stage_resolution_cases {
namespace addon = renodx::addons::dlss5;
namespace pair = same_stage_resolution_cases;
using stage_execution_cases::Capture;
using stage_execution_cases::Snapshot;

inline bool fail_close = false;
inline bool fail_signal = false;
inline ID3D12GraphicsCommandList* fault_list = nullptr;
using CloseFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
using SignalFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
inline CloseFn real_close = nullptr;
inline SignalFn real_signal = nullptr;

HRESULT STDMETHODCALLTYPE Close(ID3D12GraphicsCommandList* list) {
  if (fail_close && list == fault_list) {
    fail_close = false;
    // Actually close the list so the test does not manufacture a D3D validation error.
    real_close(list);
    return E_FAIL;
  }
  return real_close(list);
}
HRESULT STDMETHODCALLTYPE Signal(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) {
  if (fail_signal && fence == native::present_ring.fence) {
    fail_signal = false;
    return E_FAIL;
  }
  return real_signal(queue, fence, value);
}

NVSDK_NGX_Result NVSDK_CONV Evaluate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback) {
  const auto saved = model_gpu->list;
  model_gpu->list = list;
  int key = 0;
  const auto restore = [&](int*) { model_gpu->list = saved; };
  const std::unique_ptr<int, decltype(restore)> restore_list(&key, restore);
  return pair::Evaluate(list, handle, parameters, callback);
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
    native::hook_point.store(native::kHookPresent);
    native::OnNativeFgOptions(false);
    native::present_fg_multiplier.store(1);
    native::dedupe_mode.store(addon::kDedupePerEvaluate);
    native::stack_passes.store(2);
    native::nr_follow_input_res.store(false);
    native::gpu_timers_enabled.store(true);
    native::feed_mode.store(0);
    native::look_mode.store(false);
    native::detail_stability.store(1);
    native::chained_temporal_history.store(true);
    native::transfer_strength.store(1.f);
    native::color_strength.store(1.f);
    native::intensity.store(.5f);
    native::pass_stage[0].store(0);
    native::pass_follow[0].store(false);
    native::pass_intensity[0].store(.75f);
    native::pass_transfer_strength[0].store(1.f);
    native::pass_color_strength[0].store(1.f);
    native::pass_look_result[0].follow.store(true);
    native::direct_api.module = GetModuleHandleW(nullptr);
    native::direct_api.create = stage_execution_cases::Create;
    native::direct_api.evaluate = Evaluate;
    native::direct_api.release = ReleaseModel;
    native::direct_load_state = native::DirectLoadState::Ready;
    native::direct_device = gpu.device.Get();
    native::direct_device->AddRef();
    native::direct_device_native = gpu.device.Get();
    native::direct_runtime_requirements.dual_models = true;
    native::parameter_runtime_ready = true;
    native::core_allocate_parameters = addon::bridge::AllocateOwnedParameters;
    native::core_destroy_parameters = addon::bridge::DestroyOwnedParameters;
    Check(native::InstallQueueCompletionHooks(gpu.queue.Get()), "Present pair queue hooks");
    Check(native::InstallCommandListStateHooks(gpu.list.Get()), "Present pair list hooks");
    Check(native::EnsurePresentRing(gpu.device.Get()), "Present pair production ring");
    real_close = reinterpret_cast<CloseFn>((*reinterpret_cast<void***>(gpu.list.Get()))[9]);
    real_signal = reinterpret_cast<SignalFn>((*reinterpret_cast<void***>(gpu.queue.Get()))[14]);
    Check(DetourTransactionBegin() == NO_ERROR && DetourUpdateThread(GetCurrentThread()) == NO_ERROR
        && DetourAttach(reinterpret_cast<PVOID*>(&real_close), reinterpret_cast<PVOID>(Close)) == NO_ERROR
        && DetourAttach(reinterpret_cast<PVOID*>(&real_signal), reinterpret_cast<PVOID>(Signal)) == NO_ERROR
        && DetourTransactionCommit() == NO_ERROR, "Present pair scoped fault detours");
    int hooks_key = 0;
    const auto unhook = [&](int*) {
      DetourTransactionBegin();
      DetourUpdateThread(GetCurrentThread());
      DetourDetach(reinterpret_cast<PVOID*>(&real_close), reinterpret_cast<PVOID>(Close));
      DetourDetach(reinterpret_cast<PVOID*>(&real_signal), reinterpret_cast<PVOID>(Signal));
      DetourTransactionCommit();
    };
    const std::unique_ptr<int, decltype(unhook)> fault_hooks(&hooks_key, unhook);
    const auto mean = [](const Snapshot& image) {
      const auto pixels = image.Pixels();
      std::vector<float> row(image.width * 4);
      double sum = 0.;
      for (uint32_t y = 0; y < image.height; ++y) {
        Check(addon::screenshot::internal::DecodeDiagnosticRow(pixels.data()
            + static_cast<size_t>(y) * image.width * image.pixel_bytes, image.width, image.format, row.data()),
            "Present pair decode real GPU pixels");
        for (uint32_t x = 0; x < image.width; ++x) {
          for (uint32_t c = 0; c < 3; ++c) {
            Check(std::isfinite(row[x * 4 + c]), "Present pair finite pixels");
            sum += row[x * 4 + c];
          }
        }
      }
      return static_cast<float>(sum / (image.width * image.height * 3));
    };
    const auto texture = [&](DXGI_FORMAT format) {
      const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
      const D3D12_RESOURCE_DESC desc{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
          .Width = 16, .Height = 16, .DepthOrArraySize = 1, .MipLevels = 1,
          .Format = format, .SampleDesc = {1, 0}};
      ComPtr<ID3D12Resource> result;
      Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
          &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&result))), "Present pair texture");
      return result;
    };
    auto motion = texture(DXGI_FORMAT_R16G16_FLOAT);
    auto depth = texture(DXGI_FORMAT_R32_FLOAT);
    gpu.Write(motion.Get(), 0);
    gpu.Write(depth.Get(), 0);
    gpu.Transition(motion.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
    gpu.Transition(depth.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
    gpu.Submit();
    pair::CopyObservation copy_observation(gpu);
    pair::production_stage_extent = 16;
    uint32_t cases = 0;
    for (const auto format : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT,
                             DXGI_FORMAT_R10G10B10A2_UNORM}) {
      auto frame = texture(format);
      auto& stream = native::present_streams[frame.Get()];
      native::FeatureState contract;
      contract.input_width = contract.input_height = 16;
      contract.has_motion_scale_x = contract.has_motion_scale_y = true;
      contract.motion_scale_x = contract.motion_scale_y = 1.f;
      native::PresentTarget target;
      target.domain = addon::present_restore_scope::Domain::FINAL_PRESENT;
      target.capture_display = true;
      target.output_state = D3D12_RESOURCE_STATE_COMMON;
      target.workset_key = reinterpret_cast<const ID3D12Resource*>(&stream);
      target.independent_resolution_pair = true;
      const bool hdr = format != DXGI_FORMAT_R8G8B8A8_UNORM;
      const auto write_frame = [&](uint8_t sdr) {
        if (!hdr) {
          gpu.Write(frame.Get(), sdr);
          return;
        }
        const auto desc = frame->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        uint64_t bytes = 0;
        gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        auto upload = gpu.Buffer(bytes, D3D12_HEAP_TYPE_UPLOAD);
        uint8_t* mapped = nullptr;
        Check(SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))), "Present HDR source upload");
        std::memset(mapped, 0, static_cast<size_t>(bytes));
        // About 0.8 nit in either encoding. With this fixture's fixed Display
        // divisor the proxy stays off the shoulder, where edits are observable.
        constexpr uint16_t scrgb[]{0x211f, 0x211f, 0x211f, 0x3c00};
        constexpr uint32_t pq = 148u | (148u << 10) | (148u << 20) | (3u << 30);
        const bool linear = format == DXGI_FORMAT_R16G16B16A16_FLOAT;
        for (uint32_t y = 0; y < 16; ++y) {
          for (uint32_t x = 0; x < 16; ++x) {
            std::memcpy(mapped + footprint.Offset + y * footprint.Footprint.RowPitch
                    + x * (linear ? sizeof(scrgb) : sizeof(pq)),
                linear ? static_cast<const void*>(scrgb) : static_cast<const void*>(&pq),
                linear ? sizeof(scrgb) : sizeof(pq));
          }
        }
        upload->Unmap(0, nullptr);
        gpu.Transition(frame.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        const D3D12_TEXTURE_COPY_LOCATION from{.pResource = upload.Get(),
            .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = footprint};
        const D3D12_TEXTURE_COPY_LOCATION to{.pResource = frame.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        gpu.Transition(frame.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        gpu.uploads.push_back(upload);
      };
      native::codec_mode.store(hdr ? 3u : 0u);
      if (hdr) {
        target.hdr_mode = (format == DXGI_FORMAT_R10G10B10A2_UNORM ? 2 : 1);
        target.units = (target.hdr_mode == 2
            ? native::codec::SourceUnits{native::codec::Encoding::Pq, true, 10000.f, true}
            : native::codec::SourceUnits{native::codec::Encoding::Linear, true, 80.f, false});
        target.hdr_codec_mode = 3;
        target.hdr_white_nits = 80.f;
      }
      // Freeze the same encoding/curve contract as RunPresentPath, after
      // selecting the actual swapchain units rather than the SDR defaults.
      native::SnapshotPresentRestoreConfiguration(&target);
      target.hdr_encoding = native::V6EncodingConstant(target.units);
      target.hdr_dark_gate = native::V6DarkGate(target.units);
      target.hdr_pedestal_cap = native::codec::BlackPedestalCap(target.units);
      Check(!hdr || (target.hdr_curve == 1
          && target.hdr_encoding == (target.hdr_mode == 2 ? 4u : 1u)),
          "HDR Present fixture freezes Display curve and scRGB/PQ BT.2020 encoding");
      Check(native::PresentResolutionPairAllowed(target), "explicit FG off permits ordinary Present pair");
      // Structure and live FG state are checked even if a caller incorrectly sets the flag.
      auto forbidden = target;
      forbidden.d3d11_bridge = true;
      Check(!native::PresentResolutionPairAllowed(forbidden), "DX11 cannot enable Present pair");
      forbidden = target;
      forbidden.source_color = frame.Get();
      Check(!native::PresentResolutionPairAllowed(forbidden), "borrowed PreFG source cannot enable pair");
      forbidden = target;
      forbidden.domain = addon::present_restore_scope::Domain::OTHER;
      Check(!native::PresentResolutionPairAllowed(forbidden), "nonfinal domain cannot enable pair");
      addon::stage_plan::Plan b1;
      forbidden = target;
      forbidden.stage_execution_plan = &b1;
      Check(!native::PresentResolutionPairAllowed(forbidden), "B1 cannot enable pair");
      native::native_fg_declaration.store(native::NativeFgDeclaration::kUnknown);
      Check(!native::PresentResolutionPairAllowed(target), "unknown FG is not off");
      native::OnNativeFgOptions(true);
      Check(!native::PresentResolutionPairAllowed(target), "FG on cannot enable pair");
      native::OnNativeFgOptions(false);
      native::native_fg_owns.store(true);
      Check(!native::PresentResolutionPairAllowed(target), "native FG owner cannot enable pair");
      native::native_fg_owns.store(false);
      addon::fg::xefg::internal::owns_stream.store(true);
      Check(!native::PresentResolutionPairAllowed(target), "enabled XeFG without requested ownership cannot enable pair");
      addon::fg::xefg::internal::requested.store(+[]() noexcept { return true; });
      Check(!native::PresentResolutionPairAllowed(target), "XeFG owned stream cannot enable pair");
      addon::fg::xefg::internal::owns_stream.store(false);
      addon::fg::xefg::internal::requested.store(nullptr);
      native::auto_fg_fallback_active.store(true);
      Check(!native::PresentResolutionPairAllowed(target), "active FG fallback cannot enable pair");
      native::auto_fg_fallback_active.store(false);
      native::hook_point.store(native::kHookUpscaled);
      Check(!native::PresentResolutionPairAllowed(target), "non-Present hook cannot enable pair");
      native::hook_point.store(native::kHookPresent);
      const uint32_t scenarios = (hdr ? 1u : 4u);
      for (uint32_t scenario = 0; scenario < scenarios; ++scenario) {
        native::preset.store(1);
        native::pass_preset[0].store(scenario == 2 ? 1u : 2u);
        native::pass_resolution_mode[0].store(scenario >= 2 ? 0u : 2u);
        constexpr std::array<std::array<float, 2>, 2> scales{{{1.f, .67f}, {1.f, .5f}}};
        const uint32_t phases = (scenario == 0 ? 2u : 1u);
        std::array<uint32_t, 2> generations{};
        uint64_t parent_id = 0, old_tail_id = 0;
        const void* old_proxy = nullptr;
        const native::WorksetKey key{stream.Handle(), target.workset_key};
        uint64_t frame_copies = 0;
        const auto record = [&](bool neutral = false, native::PresentGuideSet* guides = nullptr, bool prepare = true) {
          pair::evaluations.clear();
          if (prepare) {
            write_frame(96);
            gpu.Submit();
          }
          auto& block = native::FillPresentBlock(&stream, frame.Get(), motion.Get(), depth.Get(),
              contract, 16, 16, 1.f, 0, neutral);
          auto* slot = &native::present_ring.slots[(native::present_ring.submitted + 1) % native::kPresentRing];
          Check(native::present_ring.fence->GetCompletedValue() >= slot->value, "fixture selects completed slot");
          fault_list = slot->list;
          target.independent_resolution_pair = true;
          const auto saved_list = gpu.list;
          gpu.list = slot->list;
          int list_key = 0;
          const auto restore_list = [&](int*) { gpu.list = saved_list; };
          const std::unique_ptr<int, decltype(restore_list)> restored(&list_key, restore_list);
          const auto copies_before = pair::production_full_stage_copies;
          const bool submitted = native::SubmitPresentGroup(gpu.queue.Get(), nullptr, slot, stream, block, guides, target, neutral, {});
          frame_copies = pair::production_full_stage_copies - copies_before;
          return submitted;
        };
        for (uint32_t phase = 0; phase < phases; ++phase) {
          const auto scale = (scenario == 0 ? scales[phase]
              : (scenario == 1 ? std::array{.67f, 1.f} : std::array{1.f, 1.f}));
          native::nr_resolution_scale.store(scale[0]);
          native::pass_resolution_scale[0].store(scale[1]);
          const std::array<uint32_t, 2> sizes{static_cast<uint32_t>(std::lround(16 * scale[0])),
              static_cast<uint32_t>(std::lround(16 * scale[1]))};
          const auto timer_start = addon::gpu_timers::measured_total_us;
          uint32_t steady = 0;
          bool tail_reset_seen = false;
          for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 32 && steady < 2; ++attempt) {
            record();
            gpu.Submit();
            addon::gpu_timers::Poll(true, false);
            if (pair::evaluations.size() != 2 || pair::evaluations[0].input.width != sizes[0]
                || pair::evaluations[1].input.width != sizes[1]) continue;
            const auto& first = pair::evaluations[0];
            const auto& second = pair::evaluations[1];
            tail_reset_seen |= second.reset == 1;
            Check(first.input.height == sizes[0] && second.input.height == sizes[1], "Present actual working sizes");
            Check(first.preset == 1 && second.preset == (scenario == 2 ? 1u : 2u), "Present logical AA/AB models");
            if (phase == 1) {
              Check(first.generation == generations[0] && first.reset == 0, "Pass2 resize preserves upstream Feature/History");
              Check(second.generation != generations[1], "Pass2 resize rebuilds only downstream Feature");
            }
            if (first.reset == 0 && second.reset == 0) {
              Check(frame_copies == 2, "steady Present has original copy-in/out with zero extra layer handoff copies");
              ++steady;
            }
          }
          Check(steady == 2, "Present bounded warmup reaches steady dual pass");
          Check(tail_reset_seen, "new or resized Present tail resets History");
          // Keep the tail markers near the HDR10 proxy. Larger arbitrary
          // stamps can both hit the existing +2-stop Display resolve bound.
          pair::stamps = {0x3400, 0x3200};
          record(); gpu.Submit();
          const float low = mean(pair::evaluations[1].input);
          const auto low_input = pair::evaluations[1].input.Pixels();
          const auto low_final = Capture(gpu, frame.Get(), D3D12_RESOURCE_STATE_COMMON);
          gpu.Submit();
          const float low_result = mean(low_final);
          pair::stamps[0] = 0x3a00;
          record(); gpu.Submit();
          const float high = mean(pair::evaluations[1].input);
          std::cout << "HANDOFF format=" << format << " scenario=" << scenario << " phase=" << phase
              << " input2_low=" << low << " input2_high=" << high << '\n';
          // Display resolve damps edits according to the HDR shoulder. Its
          // signal need not exceed SDR's arbitrary absolute 0.05 threshold.
          Check(high > low && pair::evaluations[1].input.Pixels() != low_input,
              "Present Pass2 GPU input follows real Pass1 output with a positive pixel change");
          pair::stamps = {0x3400, 0x3400};
          record(); gpu.Submit();
          const auto high_final = Capture(gpu, frame.Get(), D3D12_RESOURCE_STATE_COMMON);
          gpu.Submit();
          const float high_result = mean(high_final);
          std::cout << "DELIVERY format=" << format << " scenario=" << scenario << " phase=" << phase
              << " final_low=" << low_result << " final_high=" << high_result << '\n';
          Check(high_result > low_result && high_final.Pixels() != low_final.Pixels(),
              "Present final GPU pixels follow Pass2 output with a positive encoded pixel change");
          pair::stamps = {0x3400, 0x3600};
          auto& work = native::worksets.at(key);
          if (sizes[0] != sizes[1]) {
            Check(work.resolution_tail != nullptr && work.resolution_tail->original == work.original
                && work.resolution_tail->descriptors != work.descriptors, "Present shared full-size root and private tail");
          } else Check(work.resolution_tail == nullptr, "legacy AA/AB has no tail");
          if (phase == 1) {
            Check(work.id == parent_id && work.resolution_tail->id != old_tail_id,
                "Pass2-only resize preserves full-size parent");
            // GPU completion alone does not invalidate replayable private ring recordings.
            Check(!submission::ResourceReleasable(old_proxy), "completed old tail remains protected by replayable ring lists");
            for (uint32_t turn = 0; turn < native::kPresentRing; ++turn) { record(); gpu.Submit(); }
            native::DrainRetiredResources(true);
            Check(submission::ResourceReleasable(old_proxy), "completed old Present tail releases");
          }
          generations = {pair::evaluations[0].generation, pair::evaluations[1].generation};
          parent_id = work.id;
          if (work.resolution_tail != nullptr) {
            old_tail_id = work.resolution_tail->id;
            old_proxy = work.resolution_tail->proxy;
          }
          const auto memory = addon::telemetry::QueryVideoMemory(gpu.device.Get());
          std::cout << "PASS present-stage-resolution format=" << format << " scenario=" << scenario << " phase=" << phase
              << " effective=" << sizes[0] << ',' << sizes[1] << " live_workset_bytes=" << work.allocated_bytes
              << " dxgi_local_usage_bytes=" << memory.local_usage << " dxgi_local_budget_bytes=" << memory.local_budget
              << " timer_total_us=" << addon::gpu_timers::measured_total_us - timer_start
              << " production_full_stage_copies=" << frame_copies
              << " timer_scope=warmup-readback-model-standin vendor_model_memory=excluded\n";
          ++cases;
        }
        if (scenario == 0) {
          namespace shot = addon::screenshot;
          write_frame(32); gpu.Submit();
          const auto before_capture = Capture(gpu, frame.Get(), D3D12_RESOURCE_STATE_COMMON);
          gpu.Submit();
          shot::source_mode.store(1);
          shot::stage_planes_enabled.store(false);
          Check(shot::RequestCapture() && shot::WantsFinalPresent(), "arm actual final Present screenshot");
          record(false, nullptr, false); gpu.Submit();
          Check(shot::internal::pending.has_value(), "production Present creates F5 GPU readbacks");
          auto& capture = *shot::internal::pending;
          Check(capture.pre_recorded && capture.ready && !capture.model_diagnostic
              && capture.width == 16 && capture.height == 16, "F5 records complete full-size pair across dual pass");
          Snapshot pre, post;
          pre.readback = capture.pre_nr_readback;
          post.readback = capture.nr_output_readback;
          for (auto* image : {&pre, &post}) {
            image->footprint = capture.footprint;
            image->width = capture.width;
            image->height = capture.height;
            image->format = capture.format;
            image->pixel_bytes = shot::internal::DiagnosticPixelBytes(capture.format);
          }
          const auto after_capture = Capture(gpu, frame.Get(), D3D12_RESOURCE_STATE_COMMON);
          gpu.Submit();
          Check(pre.Pixels() == before_capture.Pixels() && post.Pixels() == after_capture.Pixels(),
              "F5 GPU pre equals untouched full frame and GPU post equals final delivered full frame");
          Check(pre.Pixels() != post.Pixels(), "F5 pair contains distinct before and after images");
          Check(native::ReleasePresentRing(), "F5 destroys completed replayable ring before releasing captures");
          shot::internal::ReleaseReadbacks(capture);
          shot::internal::pending.reset();
          shot::internal::has_pending.store(false);
          shot::Disarm();
          Check(native::EnsurePresentRing(gpu.device.Get()), "F5 resumes ordinary Present ring");
          std::cout << "PASS Present F5 GPU full-frame pre-post exact format=" << format << " no-export\n";
        }
        if (format == DXGI_FORMAT_R8G8B8A8_UNORM && scenario == 0) {
          // Same real resources: cancelled recording must not become a GPU use.
          for (const bool neutral : {false, true}) {
            for (uint32_t warm = 0; warm < 5; ++warm) { record(neutral); gpu.Submit(); }
            const auto submitted = native::present_ring.submitted;
            fail_close = true;
            Check(!record(neutral) && !fail_close, "Close fault is reached and rejected");
            Check(native::present_ring.submitted == submitted, "Close failure does not claim submission");
            Check(!native::worksets.contains({stream.Handle(neutral), target.workset_key}),
                "Close failure retires the actual real/neutral handle workset");
            for (const auto& slot : native::features.at(stream.Handle(neutral)).slots) {
              Check(slot.handle == nullptr, "cancelled recording releases actual real/neutral model slots");
            }
            for (uint32_t warm = 0; warm < 6; ++warm) { record(neutral); gpu.Submit(); }
            Check(pair::evaluations.size() == 2, "real/neutral Close failure recovers dual pass");
          }
          // The Submit entry must withdraw stale authority after PreFG becomes active.
          for (auto* active : {&native::present_prefg_active, &native::present_prefg_upscaled_active}) {
            active->store(true);
            record(); gpu.Submit();
            Check(!target.independent_resolution_pair, "Submit refresh rejects live PreFG state");
            active->store(false);
          }
          for (uint32_t warm = 0; warm < 6; ++warm) { record(); gpu.Submit(); }
          const auto before_failure = Capture(gpu, frame.Get(), D3D12_RESOURCE_STATE_COMMON);
          gpu.Submit();
          fail_call = model_calls + 2;
          // Keep the frame unchanged so the oracle checks failed tail writeback.
          record(false, nullptr, false);
          Check(model_calls == fail_call, "failure injection reaches second model");
          fail_call = 0;
          gpu.Submit();
          const auto after_failure = Capture(gpu, frame.Get(), D3D12_RESOURCE_STATE_COMMON);
          gpu.Submit();
          Check(before_failure.Pixels() == after_failure.Pixels(), "tail failure preserves original frame bytes");
          for (uint32_t warm = 0; warm < 6; ++warm) { record(); gpu.Submit(); }

          auto& guides = native::present_guide_sets[0];
          guides.serial = 123;
          // Fence hold proves in-flight retention with an actual queued Present list.
          ComPtr<ID3D12Fence> gate;
          Check(SUCCEEDED(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))), "Present GPU gate");
          gpu.Write(frame.Get(), 96); gpu.Submit();
          Check(SUCCEEDED(gpu.queue->Wait(gate.Get(), 1)), "hold actual Present queue");
          bool signal_result = true;
          // The observer's readbacks are fixture-owned, so keep the pending
          // recording's buffers alive while the next record replaces its ledger.
          std::vector<pair::Evaluation> pending_signal_readbacks;
          try {
            fail_signal = true;
            signal_result = record(false, &guides, false);
            pending_signal_readbacks = std::move(pair::evaluations);
            Check(!signal_result && !fail_signal, "actual Present Signal failure reports false");
            Check(guides.read_value == native::present_ring.submitted && stream.consumed_serial == guides.serial,
                "Signal failure retains guide read occupancy and consumed serial");
            Check(native::NextPresentCaptureSet() != &guides, "capture selector cannot overwrite in-flight read guides");
            Check(!native::PresentRingComplete() && !native::ReleasePresentRing(), "in-flight ring cannot retire");
            const auto& live = native::worksets.at(key);
            Check(live.resolution_tail != nullptr && !submission::ResourceReleasable(live.resolution_tail->proxy),
                "actual queued tail remains pinned before completion");
            for (const auto& slot : native::features.at(stream.Handle()).slots) {
              if (slot.handle != nullptr) Check(slot.pending_reset, "Signal failure requests recovery History");
            }
          } catch (...) { gate->Signal(1); throw; }
          Check(SUCCEEDED(gate->Signal(1)), "release Present GPU gate");
          // A later successful same-queue ring Signal covers the earlier failed Signal.
          Check(record(false, &guides, false), "next completed slot remains usable after Signal failure");
          gpu.Submit();
          Check(native::PresentRingComplete() && native::present_ring.fence->GetCompletedValue() >= guides.read_value,
              "later queue Signal proves both Present submissions and guide reads complete");
          std::cout << "PASS Present faults real-neutral-Close cancel model-tail-failure PreFG-refresh"
                       " actual-inflight-tail Signal-guide-occupancy later-same-queue-signal\n";
        }
        native::ForgetSourceHandleLocked(stream.Handle());
        native::ForgetSourceHandleLocked(stream.Handle(true));
        Check(native::ReleasePresentRing(), "completed ring releases replayable recordings");
        native::DrainRetiredResources(true);
        Check(native::EnsurePresentRing(gpu.device.Get()), "rebuild fixture ring for next scenario");
      }
      native::RetirePresentStream(frame.Get());
      native::DrainRetiredResources(true);
    }
    Check(native::ReleasePresentRing(), "Present final ring cleanup");
    if (info != nullptr) {
      for (UINT64 index = 0; index < info->GetNumStoredMessages(); ++index) {
        SIZE_T bytes = 0;
        info->GetMessage(index, nullptr, &bytes);
        std::vector<uint8_t> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        info->GetMessage(index, message, &bytes);
        Check(message->Severity > D3D12_MESSAGE_SEVERITY_ERROR, message->pDescription);
      }
    }
    std::cout << "PASS present-stage-resolution cases=" << cases << " hardware=" << hardware
        << " actual-production-ring NVIDIA-NR=substituted no-game-display-performance-claim\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL present-stage-resolution: " << error.what() << '\n';
    return 1;
  }
}
}  // namespace present_stage_resolution_cases
