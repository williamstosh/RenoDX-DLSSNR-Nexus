/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

// Included after the existing native GPU/model and stage fixtures.
namespace present_chain_cases {
namespace addon = renodx::addons::dlss5;
namespace stages = stage_execution_cases;
inline native::PresentStream* current_stream = nullptr;
inline std::optional<stages::Snapshot> present_original;

NVSDK_NGX_Result NVSDK_CONV Evaluate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback) {
  const auto game_list = model_gpu->list;
  model_gpu->list = list;
  const auto restore = [&](int*) { model_gpu->list = game_list; };
  int key = 0;
  const std::unique_ptr<int, decltype(restore)> restore_list(&key, restore);
  if (current_stream != nullptr && list != game_list.Get()) {
    const auto entry = native::worksets.find({current_stream->Handle(), reinterpret_cast<const ID3D12Resource*>(current_stream)});
    if (entry != native::worksets.end()) {
      present_original = stages::Capture(*model_gpu, entry->second.original, entry->second.original_state);
    }
  }
  return stages::Evaluate(list, handle, parameters, callback);
}

int Run(bool hardware, DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM) {
  try {
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    Gpu gpu(hardware);
    model_gpu = &gpu;
    ComPtr<ID3D12InfoQueue> info;
    gpu.device.As(&info);
    const auto wait = [&](ID3D12Fence* fence, uint64_t value) {
      HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      Check(event != nullptr, "B1 completion event");
      const auto hr = fence->SetEventOnCompletion(value, event);
      const auto result = SUCCEEDED(hr) ? WaitForSingleObject(event, 10000) : WAIT_FAILED;
      CloseHandle(event);
      Check(result == WAIT_OBJECT_0, "B1 bounded GPU completion");
    };
    const auto finish = [&] {
      Check(SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial)), "B1 test fence");
      wait(gpu.fence.Get(), gpu.serial);
    };
    const auto submit = [&](bool complete = true, ID3D12CommandQueue* destination = nullptr) {
      Check(SUCCEEDED(gpu.list->Close()), "B1 game close");
      ID3D12CommandList* lists[]{gpu.list.Get()};
      (destination != nullptr ? destination : gpu.queue.Get())->ExecuteCommandLists(1, lists);
      if (complete) {
        if (destination != nullptr) {
          Check(SUCCEEDED(destination->Signal(gpu.fence.Get(), ++gpu.serial)), "B1 other queue test fence");
          wait(gpu.fence.Get(), gpu.serial);
        } else finish();
        Check(SUCCEEDED(gpu.allocator->Reset()), "B1 allocator reset");
        Check(SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)), "B1 game list reset");
        submission::OnCommandListReset(gpu.list.Get());
        submission::PruneCompletedGenerations(native::PreservePresentCaptureProof);
        gpu.uploads.clear();
      }
    };
    native::enabled.store(true);
    native::hooks_enabled.store(true);
    native::experimental_present_chain.store(true);
    native::hook_point.store(native::kHookUpscaled);
    native::stack_passes.store(2);
    native::pass_stage[0].store(3);
    native::preset.store(1);
    native::pass_preset[0].store(2);
    native::nr_follow_input_res.store(false);
    native::nr_resolution_scale.store(1.f);
    native::look_mode.store(false);
    native::codec_mode.store(0);
    native::transfer_strength.store(1.f);
    native::color_strength.store(1.f);
    native::pass_transfer_strength[0].store(.7f);
    native::pass_color_strength[0].store(.8f);
    native::dedupe_mode.store(addon::kDedupePerEvaluate);
    native::direct_api.module = GetModuleHandleW(nullptr);
    native::direct_api.create = stages::Create;
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
    Check(native::InstallQueueCompletionHooks(gpu.queue.Get()), "B1 queue hooks");
    Check(native::InstallCommandListStateHooks(gpu.list.Get()), "B1 list hooks");
    Check(native::EnsurePresentRing(gpu.device.Get()), "B1 production Present ring");
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = desc.Height = 8;
    desc.Format = format;
    const bool hdr = format != DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferCount = 2;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    HWND window = CreateWindowExW(0, L"STATIC", L"NR Nexus B1 GPU fixture", WS_OVERLAPPED,
        0, 0, 8, 8, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "B1 hidden DXGI host window");
    const std::unique_ptr<std::remove_pointer_t<HWND>, decltype(&DestroyWindow)> window_owner(window, &DestroyWindow);
    ComPtr<IDXGISwapChain1> swapchain;
    const HRESULT swapchain_result = gpu.factory->CreateSwapChainForHwnd(gpu.queue.Get(), window, &desc, nullptr, nullptr, &swapchain);
    if (FAILED(swapchain_result)) std::cerr << "B1 DXGI CreateSwapChainForHwnd HRESULT=" << std::hex << swapchain_result << std::dec << '\n';
    Check(SUCCEEDED(swapchain_result), "B1 actual DXGI swapchain");
    ComPtr<ID3D12Resource> back;
    Check(SUCCEEDED(swapchain->GetBuffer(0, IID_PPV_ARGS(&back))), "B1 actual backbuffer");
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    const D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV, .NumDescriptors = 1};
    Check(SUCCEEDED(gpu.device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap))), "B1 post-processing RTV");
    const auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    gpu.device->CreateRenderTargetView(back.Get(), nullptr, rtv);
    auto source_desc = back->GetDesc();
    source_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> output;
    const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &source_desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&output))), "B1 real SR output");
    const uint32_t guide_size = format == DXGI_FORMAT_R10G10B10A2_UNORM ? 4u : 8u;
    const auto guide_texture = [&](DXGI_FORMAT guide_format) {
      const D3D12_RESOURCE_DESC guide_desc{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
          .Width = guide_size, .Height = guide_size, .DepthOrArraySize = 1, .MipLevels = 1,
          .Format = guide_format, .SampleDesc = {1, 0}};
      ComPtr<ID3D12Resource> texture;
      Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &guide_desc,
          D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&texture))), "B1 typed source guide");
      return texture;
    };
    auto color = guide_texture(DXGI_FORMAT_R8G8B8A8_UNORM);
    auto depth = guide_texture(DXGI_FORMAT_R32_FLOAT);
    auto motion = guide_texture(DXGI_FORMAT_R16G16_FLOAT);
    gpu.Write(color.Get(), 64);
    gpu.Write(depth.Get(), 0);
    gpu.Write(motion.Get(), 0);
    for (auto* resource : {color.Get(), depth.Get(), motion.Get()}) {
      gpu.Transition(resource, D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
    }
    submit();
    auto& stream = native::present_streams[swapchain.Get()];
    current_stream = &stream;
    stream.mixed_queue = renodx::utils::directx::NativeIdentity(gpu.queue.Get());
    stream.mixed_submission_queue = gpu.queue.Get();
    stream.mixed_write_marker = reinterpret_cast<const void*>(uintptr_t{0xFFFD1000});
    stream.mixed_buffers = {renodx::utils::directx::NativeIdentity(back.Get())};
    stream.mixed_width = stream.mixed_height = 8;
    stream.mixed_format = desc.Format;
    stream.mixed_device = renodx::utils::directx::NativeIdentity(gpu.device.Get());
    addon::bridge::OwnedParameters parameters;
    parameters.Set(NVSDK_NGX_Parameter_Color, color.Get());
    parameters.Set(NVSDK_NGX_Parameter_Output, output.Get());
    parameters.Set(NVSDK_NGX_Parameter_Depth, depth.Get());
    parameters.Set(NVSDK_NGX_Parameter_MotionVectors, motion.Get());
    parameters.Set(NVSDK_NGX_Parameter_Width, guide_size);
    parameters.Set(NVSDK_NGX_Parameter_Height, guide_size);
    parameters.Set(NVSDK_NGX_Parameter_OutWidth, 8u);
    parameters.Set(NVSDK_NGX_Parameter_OutHeight, 8u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, guide_size);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, guide_size);
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
        (hdr ? static_cast<uint32_t>(NVSDK_NGX_DLSS_Feature_Flags_IsHDR) : 0u)
            | (guide_size < 8 ? static_cast<uint32_t>(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) : 0u));
    const auto* handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xB100});
    native::RegisterCreatedFeatureLocked(handle, native::kFeatureDlss, &parameters);
    native::PresentTarget target;
    target.capture_display = true;
    target.domain = addon::present_restore_scope::Domain::FINAL_PRESENT;
    native::SnapshotPresentRestoreConfiguration(&target);
    if (hdr) {
      target.hdr_mode = format == DXGI_FORMAT_R10G10B10A2_UNORM ? 2 : 1;
      target.units = target.hdr_mode == 2
          ? native::codec::SourceUnits{native::codec::Encoding::Pq, true, 10000.f, true}
          : native::codec::SourceUnits{native::codec::Encoding::Linear, true, 80.f, false};
      native::codec_mode.store(3);
      target.hdr_codec_mode = 3;
      target.hdr_white_nits = 80.f;
    }
    const auto begin = [&](bool copy = true, bool region = false, bool partial = false, bool post_process = true) {
      stages::evaluations.clear();
      present_original.reset();
      gpu.Write(output.Get(), 48);
      gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      {
        const addon::EvaluateChainScope chain;
        const addon::SourceStageScope source(handle, true);
        bool touched = false;
        Check(native::ProcessMixedPresentPredecessor(gpu.list.Get(), handle, &parameters, &touched), "B1 source owns mixed group");
        const auto count = model_calls;
        Check(native::ProcessMixedPresentPredecessor(gpu.list.Get(), handle, &parameters, &touched)
            && model_calls == count, "B1 nested source cannot run A twice");
      }
      if (!post_process) {
        gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        return;
      }
      gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      gpu.Transition(back.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
      if (copy) {
        if (region || partial) {
          const D3D12_TEXTURE_COPY_LOCATION from{.pResource = output.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
          const D3D12_TEXTURE_COPY_LOCATION to{.pResource = back.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
          const D3D12_BOX box{0, 0, 0, 4, 4, 1};
          gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, partial ? &box : nullptr);
        } else gpu.list->CopyResource(back.Get(), output.Get());
      }
      gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
      gpu.Transition(back.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
      // Stand in for the game's post-processing/HUD on the actual BB.
      gpu.Transition(back.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
      constexpr float post_color[] = {176.f / 255.f, 176.f / 255.f, 176.f / 255.f, 176.f / 255.f};
      gpu.list->ClearRenderTargetView(rtv, post_color, 0, nullptr);
      gpu.Transition(back.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
    };
    const auto tail = [&] {
      ++native::present_generation;
      return native::RunMixedPresent(gpu.queue.Get(), nullptr, stream, back.Get(), target);
    };
    uint32_t complete_frames = 0;
    for (uint32_t frame = 0; frame < 8; ++frame) {
      begin();
      const auto a = stages::evaluations.size();
      const auto post = stages::Capture(gpu, back.Get(), D3D12_RESOURCE_STATE_COMMON);
      submit();
      const auto before_b = model_calls;
      const bool applied = tail();
      std::cout << "B1 frame=" << frame << " status=" << native::mixed_present_status << " model_delta=" << model_calls - before_b << '\n';
      finish();
      Check(model_calls - before_b <= 1 && stages::evaluations.size() <= 2, "B1 no replay of A at Present");
      if (applied) {
        Check(a == 1 && stages::evaluations.size() == 2, "B1 A and B once each");
        const auto& eval_a = stages::evaluations[0];
        const auto& eval_b = stages::evaluations[1];
        Check(eval_a.handle != eval_b.handle && stages::presets[eval_a.handle] == 1
            && stages::presets[eval_b.handle] == 2, "B1 independent A/B features and model presets");
        const auto workset = native::worksets.find({stream.Handle(), reinterpret_cast<const ID3D12Resource*>(&stream)});
        Check(workset != native::worksets.end(), "B1 independent Present workset");
        const auto final = stages::Capture(gpu, back.Get(), D3D12_RESOURCE_STATE_COMMON);
        submit();
        Check(present_original.has_value() && present_original->Pixels() == post.Pixels(),
            "B1 reads post-processing/HUD pixels, not A scratch");
        Check(eval_a.input.Pixels() != eval_b.input.Pixels(), "B1 model sees distinct final image");
        if (!hdr) Check(final.Pixels() == post.Pixels(), "B1 identity model preserves post-processing output and alpha");
        else {
          const auto pixels = final.Pixels();
          std::array<float, 32> row;
          Check(addon::screenshot::internal::DecodeDiagnosticRow(pixels.data(), 8, format, row.data()), "B1 HDR decoder");
          for (const auto value : row) Check(std::isfinite(value) && value > 0.f, "B1 HDR final output remains finite and nonblack");
        }
        ++complete_frames;
      }
    }
    Check(complete_frames >= 3, "B1 reaches repeated stable complete frames");

    if (!hdr) {
    const auto rejected = [&](const char* expected) {
      const auto calls = model_calls;
      const bool applied = tail();
      if (applied || model_calls != calls || std::strcmp(native::mixed_present_status, expected) != 0) {
        std::cerr << "B1 rejection expected=" << expected << " actual=" << native::mixed_present_status << '\n';
      }
      Check(!applied && model_calls == calls && std::strcmp(native::mixed_present_status, expected) == 0,
          "B1 rejects before B without replaying A");
      finish();
    };
    const auto recover = [&] {
      bool applied = false;
      for (uint32_t i = 0; i < 4; ++i) { begin(); submit(); applied = tail(); finish(); }
      Check(applied, "B1 returns to a complete chain after rejection");
    };
    begin(true, true);
    submit();
    Check(tail(), "B1 actual full-mip native CopyTextureRegion handoff");
    finish();
    begin(false);
    submit();
    rejected("present_target_changed");
    recover();
    begin(true, true, true);
    submit();
    rejected("present_target_changed");
    recover();
    begin();
    submit();
    ++native::present_generation;
    rejected("source_sequence_or_view_unproven");
    recover();
    begin();
    submit();
    const auto* second_view = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xB101});
    native::RegisterCreatedFeatureLocked(second_view, native::kFeatureDlss, &parameters);
    {
      const addon::EvaluateChainScope chain;
      const addon::SourceStageScope source(second_view, true);
      bool touched = false;
      native::ProcessMixedPresentPredecessor(gpu.list.Get(), second_view, &parameters, &touched);
    }
    rejected("source_sequence_or_view_unproven");
    native::ForgetSourceHandleLocked(second_view);
    recover();
    begin();
    Check(SUCCEEDED(gpu.list->Close()), "B1 discard closes game recording");
    Check(SUCCEEDED(gpu.allocator->Reset()), "B1 discard resets allocator");
    Check(SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)), "B1 discard resets list");
    submission::OnCommandListReset(gpu.list.Get());
    submission::PruneCompletedGenerations(native::PreservePresentCaptureProof);
    gpu.uploads.clear();
    rejected("no_actual_submission_receipt");
    const auto after_discard = model_calls;
    begin();
    Check(model_calls == after_discard, "B1 discarded learning copy cannot admit A on the next source");
    submit();
    rejected("source_sequence_or_view_unproven");
    for (uint32_t i = 0; i < 4; ++i) { begin(); submit(); tail(); finish(); }
    begin();
    rejected("source_not_submitted");
    submit();
    recover();
    begin();
    rejected("source_not_submitted");
    Check(SUCCEEDED(gpu.list->Close()), "B1 late discarded source close");
    Check(SUCCEEDED(gpu.allocator->Reset()), "B1 late discarded allocator");
    Check(SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)), "B1 late discarded recording");
    submission::OnCommandListReset(gpu.list.Get());
    submission::PruneCompletedGenerations(native::PreservePresentCaptureProof);
    gpu.uploads.clear();
    Check(native::features.at(handle).mixed_recording_discarded, "B1 observes a source discarded after Present rejection");
    recover();
    begin();
    submit();
    ++native::configuration_generation;
    rejected("configuration_changed");
    recover();
    begin();
    submit();
    native::native_fg_owns.store(true);
    rejected("route_or_fg_unavailable");
    native::native_fg_owns.store(false);
    recover();
    begin();
    submit();
    adapter::owns_stream.store(true);
    rejected("route_or_fg_unavailable");
    adapter::owns_stream.store(false);

    recover();
    const auto previous_output = output;
    output.Reset();
    Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &source_desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&output))), "B1 rebuilt SR output");
    parameters.Set(NVSDK_NGX_Parameter_Output, output.Get());
    const auto before_rebuilt = model_calls;
    begin();
    Check(model_calls == before_rebuilt, "B1 new output resource cannot inherit a historical link");
    submit();
    rejected("source_sequence_or_view_unproven");
    recover();

    for (const bool partial : {false, true}) {
      begin();
      gpu.Transition(color.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COPY_SOURCE);
      gpu.Transition(back.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
      if (partial) {
        const D3D12_TEXTURE_COPY_LOCATION from{.pResource = color.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        const D3D12_TEXTURE_COPY_LOCATION to{.pResource = back.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        const D3D12_BOX box{0, 0, 0, 4, 4, 1};
        gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
      } else gpu.list->CopyResource(back.Get(), color.Get());
      gpu.Transition(color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, NATIVE_INPUT_STATE);
      gpu.Transition(back.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
      submit();
      rejected("present_target_changed");
      recover();
    }

    // Record A on one list, record and submit a competing copy on a second
    // list first, then submit A. Same queue alone must not admit this order.
    begin(false, false, false, false);
    const auto source_list = gpu.list;
    const auto source_allocator = gpu.allocator;
    const auto source_uploads = gpu.uploads;
    ComPtr<ID3D12GraphicsCommandList> copy_list;
    ComPtr<ID3D12CommandAllocator> copy_allocator;
    Check(SUCCEEDED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&copy_allocator))),
        "B1 copy allocator");
    Check(SUCCEEDED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, copy_allocator.Get(), nullptr,
        IID_PPV_ARGS(&copy_list))), "B1 copy list");
    gpu.list = copy_list;
    gpu.allocator = copy_allocator;
    gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    gpu.Transition(back.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    gpu.list->CopyResource(back.Get(), output.Get());
    gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    gpu.Transition(back.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    submit();
    gpu.list = source_list;
    gpu.allocator = source_allocator;
    submit();
    rejected("present_target_changed");
    recover();

    // A competing BB copy is recorded earlier, but submitted after the
    // source list. CPU observation order must not hide the late GPU writer.
    gpu.list = copy_list;
    gpu.allocator = copy_allocator;
    gpu.Transition(color.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    gpu.Transition(back.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    gpu.list->CopyResource(back.Get(), color.Get());
    gpu.Transition(color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, NATIVE_INPUT_STATE);
    gpu.Transition(back.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    gpu.list = source_list;
    gpu.allocator = source_allocator;
    begin();
    submit();
    gpu.list = copy_list;
    gpu.allocator = copy_allocator;
    submit();
    gpu.list = source_list;
    gpu.allocator = source_allocator;
    const auto before_late_writer = model_calls;
    Check(!tail() && model_calls == before_late_writer, "B1 rejects a competing copy submitted after the source");
    finish();
    recover();

    // Replay a learning-only recording twice without Reset. Its live
    // submission receipts must not authorize A in the next window.
    output.Reset();
    Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &source_desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&output))), "B1 replay learning resource");
    parameters.Set(NVSDK_NGX_Parameter_Output, output.Get());
    begin();
    Check(SUCCEEDED(gpu.list->Close()), "B1 learning replay close");
    ID3D12CommandList* replay[]{gpu.list.Get()};
    gpu.queue->ExecuteCommandLists(1, replay);
    gpu.queue->ExecuteCommandLists(1, replay);
    finish();
    Check(!tail(), "B1 learning replay cannot run B");
    gpu.list = copy_list;
    gpu.allocator = copy_allocator;
    const auto before_replay_learn = model_calls;
    begin();
    Check(model_calls == before_replay_learn, "B1 live repeated learning submissions cannot admit A");
    submit();
    Check(!tail(), "B1 repeated learning source cannot run B");
    finish();
    Check(SUCCEEDED(source_allocator->Reset()), "B1 replay recording allocator reset");
    Check(SUCCEEDED(source_list->Reset(source_allocator.Get(), nullptr)), "B1 replay recording reset");
    submission::OnCommandListReset(source_list.Get());
    submission::PruneCompletedGenerations(native::PreservePresentCaptureProof);
    gpu.list = source_list;
    gpu.allocator = source_allocator;
    recover();

    fail_call = model_calls + 1;
    begin();
    fail_call = 0;
    submit();
    rejected("source_sequence_or_view_unproven");
    recover();

    for (uint32_t i = 0; i < 4; ++i) { begin(); submit(); tail(); finish(); }
    begin();
    submit();
    native::ForgetSourceHandleLocked(handle);
    native::RegisterCreatedFeatureLocked(handle, native::kFeatureDlss, &parameters);
    rejected("source_feature_changed");
    recover();
    begin();
    submit();
    auto* slot = &native::present_ring.slots[(native::present_ring.submitted + 1) % native::kPresentRing];
    const auto saved_value = slot->value;
    slot->value = native::present_ring.submitted + 100;
    rejected("present_ring_busy_or_removed");
    slot->value = saved_value;

    for (uint32_t i = 0; i < 3; ++i) { begin(); submit(); tail(); finish(); }
    begin();
    fail_call = model_calls + 1;
    submit();
    Check(!tail(), "B1 failing B does not report a full chain");
    finish();
    fail_call = 0;
    const auto after_failure = stages::Capture(gpu, back.Get(), D3D12_RESOURCE_STATE_COMMON);
    submit();
    Check(after_failure.Pixels() == std::vector<uint8_t>(256, 176), "B1 failure retains game post-processing output");
    for (uint32_t i = 0; i < 4; ++i) { begin(); submit(); tail(); finish(); }

    // A source submitted to an actual different queue remains inadmissible
    // even after that queue completes: B1 never adds a cross-queue Wait.
    ComPtr<ID3D12CommandQueue> other_queue;
    const D3D12_COMMAND_QUEUE_DESC queue_desc{};
    Check(SUCCEEDED(gpu.device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&other_queue))), "B1 second real queue");
    begin(false, false, false, false);
    submit(true, other_queue.Get());
    rejected("present_target_changed");
    for (uint32_t i = 0; i < 3; ++i) { begin(); submit(); tail(); finish(); }

    // Same-queue ordered work need not be GPU-complete at admission. Hold
    // the real queue, submit both groups, then release the external test gate.
    ComPtr<ID3D12Fence> gate;
    Check(SUCCEEDED(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))), "B1 GPU gate");
    Check(SUCCEEDED(gpu.queue->Wait(gate.Get(), 1)), "B1 external fixture gate");
    bool ordered_tail = false;
    try {
      begin();
      submit(false);
      ordered_tail = tail();
      Check(native::present_ring.fence->GetCompletedValue() < native::present_ring.submitted,
          "B1 submitted does not claim GPU completion");
    } catch (...) {
      gate->Signal(1);
      throw;
    }
    Check(SUCCEEDED(gate->Signal(1)), "B1 release fixture gate");
    finish();
    Check(ordered_tail, "B1 same-queue pending source can order B without waiting on CPU");
    Check(SUCCEEDED(gpu.allocator->Reset()), "B1 pending allocator reset");
    Check(SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)), "B1 pending list reset");
    submission::OnCommandListReset(gpu.list.Get());
    submission::PruneCompletedGenerations(native::PreservePresentCaptureProof);
    gpu.uploads.clear();
    }

    native::ForgetSourceHandleLocked(handle);
    native::RetirePresentStream(swapchain.Get());
    native::ReleasePresentRing();
    submit();
    native::DrainPresentRetired();
    native::DrainRetiredResources(true);
    native::UnhookMixedPresentCopyHooks();
    current_stream = nullptr;
    if (info != nullptr) {
      for (UINT64 index = 0; index < info->GetNumStoredMessages(); ++index) {
        SIZE_T bytes = 0;
        info->GetMessage(index, nullptr, &bytes);
        std::vector<uint8_t> data(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(data.data());
        info->GetMessage(index, message, &bytes);
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
          std::cerr << message->pDescription << '\n';
          Check(false, "B1 D3D12 validation error");
        }
      }
    }
    std::cout << "PASS B1 " << (hardware ? "hardware" : "WARP") << " format=" << format
        << " source_input=" << guide_size << "x" << guide_size << " stable=" << complete_frames
        << " actual_DXGI_backbuffer production_A_and_Present_B; NVIDIA networks substituted, no game/display acceptance\n";
    return 0;
  } catch (const std::exception& error) {
    if (native::direct_device != nullptr) {
      std::cerr << "B1 device status=" << std::hex << native::direct_device->GetDeviceRemovedReason() << std::dec << '\n';
      ComPtr<ID3D12InfoQueue> messages;
      if (SUCCEEDED(native::direct_device->QueryInterface(IID_PPV_ARGS(&messages)))) {
        for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i) {
          SIZE_T bytes = 0;
          messages->GetMessage(i, nullptr, &bytes);
          std::vector<uint8_t> data(bytes);
          auto* message = reinterpret_cast<D3D12_MESSAGE*>(data.data());
          messages->GetMessage(i, message, &bytes);
          if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) std::cerr << message->pDescription << '\n';
        }
      }
    }
    std::cerr << "FAIL B1: " << error.what() << '\n';
    return 1;
  }
}
}  // namespace present_chain_cases
