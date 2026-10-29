/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

// Included inside dlss5::internal after present_path.hpp's resource helpers.
namespace fg_service {

struct Slot {
  ID3D12CommandAllocator* allocator = nullptr;
  ID3D12GraphicsCommandList* list = nullptr;
  ID3D12Resource* color[2] = {};
  ID3D12Resource* original_backbuffer = nullptr;
  ID3D12Resource* original_hudless = nullptr;
  ID3D12Resource* coupled_hudless = nullptr;
  ID3D12Resource* coupled_ui = nullptr;
  ID3D12DescriptorHeap* color_heap = nullptr;
  bool color_inputs_prepared = false;
  uint64_t fence_value = 0;
  bool quarantined = false;
};

struct Stream {
  ID3D12Device* device = nullptr;
  ID3D12CommandQueue* queue = nullptr;
  ID3D12Fence* fence = nullptr;
  uint64_t submitted = 0;
  uint64_t epoch = 0;
  uint64_t application_frame = 0;
  uint64_t native_serial = 0;
  uint64_t native_display_epoch = 0;
  uint64_t native_configuration = 0;
  uint64_t native_options_generation = 0;
  bool native_resolution_pair = false;
  uint64_t native_pair_attempt_frame = 0;
  std::pair<uint64_t, uint64_t> native_pair_recording{};
  const void* native_pair_producer_marker = nullptr;
  std::array<PassResolutionSettings, 2> native_pair_resolutions{};
  FenceRef native_pair_completion;
  uint64_t native_pair_completion_value = 0;
  bool native_pair_discarded = false;
  bool native_pair_replayed = false;
  const NVSDK_NGX_Handle* native_handle = nullptr;
  bool continuing_submission = false;
  uint64_t last_model_frame[2] = {};
  bool has_frame = false;
  fg::Result result = fg::Result::NOT_SERVED;
  PresentStream roles[2];
  // Native inputs live as long as the game's replayable list generations,
  // which need not fit the private XeFG queue's three-slot ring. Appending
  // must preserve cached MFG slot addresses.
  std::deque<Slot> slots{3};
  Slot* cached = nullptr;
  ID3D12Resource* source_backbuffer = nullptr;  // identities only
  ID3D12Resource* source_hudless = nullptr;
  ID3D12Resource* source_ui = nullptr;
  uint64_t source_frame_id = 0;
  uint64_t source_sdk_decision = 0;
  ID3D12PipelineState* color_pipeline = nullptr;
  bool native = false;
};

// All access is under runtime_mutex; nodes also key NR worksets/history.
static std::map<std::pair<bool, uintptr_t>, Stream> streams;
static std::map<const NVSDK_NGX_Handle*, uintptr_t> native_bindings;

inline void ResetNativeFrame(Stream* stream) {
  stream->cached = nullptr;
  stream->native_pair_producer_marker = nullptr;
  stream->native_pair_completion = {};
  stream->native_serial = 0;
  stream->continuing_submission = false;
  stream->result = fg::Result::PREPARED;
  for (auto& role : stream->roles) {
    ResetPresentStreamHistory(&role);
  }
}

inline bool Requested() noexcept {
  return enabled.load(std::memory_order_relaxed)
      && hooks_enabled.load(std::memory_order_acquire)
      && !shutting_down.load(std::memory_order_acquire)
      && !NrYieldsToForeign() && hook_point.load(std::memory_order_relaxed) == kHookPresent
      && present_pre_fg.load(std::memory_order_relaxed) != kPresentPreFgOff;
}

inline bool PrepareCapture(ID3D12GraphicsCommandList* list) noexcept {
  try {
    if (list == nullptr || !IsSupportedInjectionCommandList(list)) return false;
    EnsureForeignEntryInstalls(list);
    if (!queue_tracking_active.load(std::memory_order_acquire)) {
      ID3D12Device* device = nullptr;
      ID3D12CommandQueue* queue = nullptr;
      if (SUCCEEDED(list->GetDevice(IID_PPV_ARGS(&device)))) {
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = list->GetType();
        if (SUCCEEDED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)))) {
          ID3D12CommandQueue* native_queue = queue;
          renodx::utils::directx::NativeFromReShadeProxy(&native_queue);
          InstallQueueCompletionHooks(native_queue);
        }
      }
      ReleaseCom(queue);
      ReleaseCom(device);
    }
    return queue_tracking_active.load(std::memory_order_acquire)
        && real_queue_execute_command_lists != nullptr && InstallCommandListStateHooks(list);
  } catch (...) {
    return false;
  }
}

// An unsubmitted recording can have created handles and advanced NGX CPU
// history. Rebuild the feature and workset, including their GPU state ledger.
inline void Invalidate(Stream* stream) {
  stream->cached = nullptr;
  stream->native_pair_producer_marker = nullptr;
  stream->native_pair_completion = {};
  stream->native_serial = 0;
  stream->continuing_submission = false;
  stream->last_model_frame[0] = stream->last_model_frame[1] = 0;
  stream->result = fg::Result::NOT_SERVED;
  for (auto& role : stream->roles) {
    if (const auto it = features.find(role.Handle()); it != features.end()) {
      ReleaseAllNrSlots(it->second, true);
      features.erase(it);
    }
    RetireWorkset({role.Handle(), reinterpret_cast<const ID3D12Resource*>(&role)});
  }
}

inline void Release(Stream* stream) {
  for (auto& role : stream->roles) {
    owner_resident_nr_handles.erase(role.Handle());
  }
  for (auto& slot : stream->slots) {
    if (slot.list != nullptr && !slot.quarantined) {
      // This closes replay eligibility only. Exact submission proofs still
      // retain the objects until the GPU finishes their last submission.
      submission::OnCommandListReset(slot.list);
    }
    for (auto& color : slot.color) RetirePresentResource(&color);
    RetirePresentResource(&slot.original_backbuffer);
    RetirePresentResource(&slot.original_hudless);
    RetirePresentResource(&slot.coupled_hudless);
    RetirePresentResource(&slot.coupled_ui);
    RetirePresentResource(&slot.color_heap);
    // Lists and allocators are referenced by the same exact submission as
    // their resources, rather than released when the callback returns.
    if (slot.list != nullptr) {
      RetirePresentResource(&slot.list);
    }
    if (slot.allocator != nullptr) {
      RetirePresentResource(&slot.allocator);
    }
  }
  Invalidate(stream);
  RetirePresentResource(&stream->color_pipeline);
  for (auto& role : stream->roles) {
    RetirePresentResource(&role.neutral_motion);
    RetirePresentResource(&role.neutral_depth);
  }
  ReleaseCom(stream->fence);
  ReleaseCom(stream->queue);
  ReleaseCom(stream->device);
}

inline void Retire() noexcept {
  try {
    const ngx_serial::PresentTurn turn(true);
    if (!turn.owns()) return;
    RuntimeLock lock(runtime_mutex);
    for (auto it = streams.begin(); it != streams.end();) {
      if (it->first.first) {
        ++it;
        continue;
      }
      Release(&it->second);
      it = streams.erase(it);
    }
  } catch (...) {
  }
}

inline void CopyColor(ID3D12GraphicsCommandList* list, ID3D12Resource* destination,
                      D3D12_RESOURCE_STATES destination_state, ID3D12Resource* source,
                      D3D12_RESOURCE_STATES source_state, uint32_t source_x = 0,
                      uint32_t source_y = 0, uint32_t destination_x = 0, uint32_t destination_y = 0,
                      uint32_t width = 0, uint32_t height = 0) {
  submission::TrackUse(list, source);
  submission::TrackUse(list, destination);
  Transition(list, source, source_state, D3D12_RESOURCE_STATE_COPY_SOURCE, 0u);
  Transition(list, destination, destination_state, D3D12_RESOURCE_STATE_COPY_DEST, 0u);
  D3D12_TEXTURE_COPY_LOCATION from{};
  from.pResource = source;
  from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION to{};
  to.pResource = destination;
  to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  if (width == 0) width = static_cast<uint32_t>(destination->GetDesc().Width);
  if (height == 0) height = destination->GetDesc().Height;
  const D3D12_BOX box{source_x, source_y, 0, source_x + width, source_y + height, 1};
  list->CopyTextureRegion(&to, destination_x, destination_y, 0, &from, &box);
  Transition(list, destination, D3D12_RESOURCE_STATE_COPY_DEST, destination_state, 0u);
  Transition(list, source, D3D12_RESOURCE_STATE_COPY_SOURCE, source_state, 0u);
}

inline bool NeedsNativeColorCoupling(const fg::FrameInput& input,
                                     const NVSDK_NGX_Parameter* parameters, bool ui_recomposition) {
  return parameters != nullptr && !ui_recomposition && input.hudless != nullptr
      && input.hudless != input.backbuffer && input.ui != nullptr
      && PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_UIAlpha) == nullptr
      && PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_UI) == input.ui;
}

inline bool PrepareNativeColorInputs(Stream* stream, Slot* slot, const fg::FrameInput& input) {
  if (input.color_x != 0 || input.color_y != 0) return false;
  for (auto* source : {input.backbuffer, input.hudless, input.ui}) {
    if (source == nullptr) return false;
    const auto desc = source->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D
        || desc.Width != input.color_width || desc.Height != input.color_height
        || desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1
        || (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0) return false;
  }
  // These FG colors are normalized code values, including HDR10/PQ. Float
  // scene-linear inputs require a different range contract, not a clamp.
  for (auto* source : {input.backbuffer, input.hudless}) {
    switch (CodecViewFormat(ConcreteResourceFormat(source->GetDesc().Format))) {
      case DXGI_FORMAT_R8G8B8A8_UNORM:
      case DXGI_FORMAT_B8G8R8A8_UNORM:
      case DXGI_FORMAT_R10G10B10A2_UNORM:
      case DXGI_FORMAT_R16G16B16A16_UNORM: break;
      default: return false;
    }
  }
  switch (CodecViewFormat(ConcreteResourceFormat(input.ui->GetDesc().Format))) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UNORM: break;
    default: return false;
  }
  constexpr auto kFormat = DXGI_FORMAT_R16G16B16A16_UNORM;
  if (!SupportsCodecFormat(stream->device, kFormat)
      || !EnsureCodecRootSignature(stream->device)
      || !CreateCodecPipelineState(stream->device, __native_fg_color, &stream->color_pipeline,
                                  L"DLSS5 native FG color inputs", "native FG color inputs")) return false;
  for (auto** resource : {&slot->coupled_hudless, &slot->coupled_ui}) {
    if (*resource != nullptr && ((*resource)->GetDesc().Width != input.color_width
        || (*resource)->GetDesc().Height != input.color_height)) {
      RetirePresentResource(resource);
    }
    if (*resource == nullptr && !CreateScratchTexture(stream->device, input.color_width, input.color_height,
            kFormat, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, resource, L"DLSS5 coupled FG color")) return false;
  }
  if (slot->color_heap == nullptr) {
    const D3D12_DESCRIPTOR_HEAP_DESC desc{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = 6, .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    if (FAILED(stream->device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&slot->color_heap)))) return false;
  }
  return true;
}

inline void RecordNativeColorInputs(Stream* stream, Slot* slot, ID3D12GraphicsCommandList* list,
                                    const fg::FrameInput& input) {
  constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  const auto cpu = slot->color_heap->GetCPUDescriptorHandleForHeapStart();
  const auto stride = stream->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  uint32_t index = 0;
  for (auto* resource : {slot->color[0], input.backbuffer, input.hudless, input.ui}) {
    const D3D12_SHADER_RESOURCE_VIEW_DESC desc{
        .Format = CodecViewFormat(ConcreteResourceFormat(resource->GetDesc().Format)),
        .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, .Texture2D = {.MipLevels = 1}};
    stream->device->CreateShaderResourceView(resource, &desc, DescriptorHeapSlot(cpu, index++, stride));
    submission::TrackUse(list, resource);
  }
  for (auto* resource : {slot->coupled_hudless, slot->coupled_ui}) {
    const D3D12_UNORDERED_ACCESS_VIEW_DESC desc{
        .Format = DXGI_FORMAT_R16G16B16A16_UNORM, .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D};
    stream->device->CreateUnorderedAccessView(resource, nullptr, &desc, DescriptorHeapSlot(cpu, index++, stride));
    submission::TrackUse(list, resource);
    Transition(list, resource, kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  submission::TrackUse(list, slot->color_heap);
  submission::TrackUse(list, stream->color_pipeline);
  submission::TrackUse(list, codec_pipeline.root_signature);
  list->SetDescriptorHeaps(1, &slot->color_heap);
  list->SetComputeRootSignature(codec_pipeline.root_signature);
  list->SetPipelineState(stream->color_pipeline);
  const auto gpu = slot->color_heap->GetGPUDescriptorHandleForHeapStart();
  list->SetComputeRootDescriptorTable(0, gpu);
  list->SetComputeRootDescriptorTable(1, {gpu.ptr + static_cast<uint64_t>(4) * stride});
  const uint32_t size[4]{input.color_width, input.color_height};
  list->SetComputeRoot32BitConstants(2, 4, size, 0);
  list->Dispatch((input.color_width + 15) / 16, (input.color_height + 15) / 16, 1);
  for (auto* resource : {slot->coupled_hudless, slot->coupled_ui}) {
    Transition(list, resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
  }
  slot->color_inputs_prepared = true;
}

inline bool PrepareSurfaces(Stream* stream, Slot* slot, const fg::FrameInput& input,
                             bool coupled_ui = false) {
  const uint32_t width = input.color_width;
  const uint32_t height = input.color_height;
  if (input.hudless != nullptr && input.hudless != input.backbuffer) {
    const auto hudless = input.hudless->GetDesc();
    const auto back = input.backbuffer->GetDesc();
    if (input.color_x != 0 || input.color_y != 0 || back.Width != width || back.Height != height
        || hudless.Width != width || hudless.Height != height) return false;
  }
  ID3D12Resource** outputs[]{&slot->color[0], &slot->color[1],
                            &slot->original_hudless, &slot->original_backbuffer};
  for (uint32_t i = 0; i < 4; ++i) {
    if (i == 3 && !stream->native) continue;
    if (coupled_ui && (i == 1 || i == 2)) continue;
    auto* source = (i == 0 || i == 3 ? input.backbuffer : input.hudless);
    if (source == nullptr || ((i == 1 || i == 2) && source == input.backbuffer)) continue;
    auto** output = outputs[i];
    const auto desc = source->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1
        || desc.SampleDesc.Count != 1 || input.color_x > desc.Width || input.color_y > desc.Height
        || width > desc.Width - input.color_x || height > desc.Height - input.color_y) return false;
    if (*output != nullptr) {
      const auto have = (*output)->GetDesc();
      if (have.Width != width || have.Height != height || have.Format != desc.Format) {
        RetirePresentResource(output);
      }
    }
    if (*output == nullptr
        && !CreateScratchTexture(stream->device, width, height, desc.Format,
                                D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                output, L"DLSS5 FG private color")) return false;
  }
  return true;
}

inline fg::Result Record(Stream* stream, Slot* slot, ID3D12GraphicsCommandList* list,
                         const fg::FrameInput& input, const PresentTarget& base,
                         bool* output_ready, std::array<NrRecording, 2>* recordings,
                         const NVSDK_NGX_Parameter* native_parameters = nullptr,
                         bool ui_recomposition = true) {
  *output_ready = false;
  *recordings = {};
  slot->color_inputs_prepared = false;
  constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  const bool has_hudless = input.hudless != nullptr && input.hudless != input.backbuffer;
  const bool coupled_ui = NeedsNativeColorCoupling(input, native_parameters, ui_recomposition);
  if (coupled_ui && !PrepareNativeColorInputs(stream, slot, input)) return fg::Result::NOT_SERVED;
  const bool may_compose = ui_recomposition && has_hudless && native_parameters != nullptr && input.ui != nullptr
      && input.color_x == 0 && input.color_y == 0;
  const uint32_t guide_mode = diagnostics::GuideMode(
      base.diagnostic.guides, present_guides.load(std::memory_order_relaxed));
  const bool real_guides = input.depth != nullptr && input.motion != nullptr
      && input.guide_width != 0 && input.guide_height != 0 && guide_mode != kPresentGuidesNever;
  if (!real_guides && guide_mode == kPresentGuidesRequired) return fg::Result::NOT_SERVED;
  FeatureState contract;
  contract.input_width = (real_guides ? input.guide_width : input.color_width);
  contract.input_height = (real_guides ? input.guide_height : input.color_height);
  contract.motion_x = (real_guides ? input.motion_x : 0u);
  contract.motion_y = (real_guides ? input.motion_y : 0u);
  contract.depth_x = (real_guides ? input.depth_x : 0u);
  contract.depth_y = (real_guides ? input.depth_y : 0u);
  contract.has_motion_scale_x = true;
  contract.has_motion_scale_y = true;
  contract.motion_scale_x = (real_guides ? input.motion_scale_x : static_cast<float>(input.color_width));
  contract.motion_scale_y = (real_guides ? input.motion_scale_y : static_cast<float>(input.color_height));
  contract.create_flags = (real_guides ? input.init_flags : NVSDK_NGX_DLSS_Feature_Flags_DepthInverted);
  if (!real_guides) {
    auto& role = stream->roles[0];
    for (auto [surface, format] : {std::pair{&role.neutral_motion, DXGI_FORMAT_R16G16_FLOAT},
                                 std::pair{&role.neutral_depth, DXGI_FORMAT_R32_FLOAT}}) {
      if (*surface != nullptr && ((*surface)->GetDesc().Width != input.color_width
          || (*surface)->GetDesc().Height != input.color_height)) RetirePresentResource(surface);
      if (*surface == nullptr && !CreateScratchTexture(stream->device, input.color_width, input.color_height,
          format, D3D12_RESOURCE_FLAG_NONE, kRead, surface, L"DLSS5 FG neutral NR guides")) {
        return fg::Result::FAILED;
      }
    }
  }
  auto* motion = (real_guides ? input.motion : stream->roles[0].neutral_motion);
  auto* depth = (real_guides ? input.depth : stream->roles[0].neutral_depth);
  const auto motion_state = (real_guides ? input.motion_state : kRead);
  const auto depth_state = (real_guides ? input.depth_state : kRead);
  if (motion == depth || motion == input.backbuffer || depth == input.backbuffer
      || motion == input.hudless || depth == input.hudless) return fg::Result::NOT_SERVED;
  submission::TrackUse(list, motion);
  submission::TrackUse(list, depth);
  Transition(list, motion, motion_state, kRead, 0u);
  Transition(list, depth, depth_state, kRead, 0u);
  const auto restore_guides = [&] {
    Transition(list, depth, kRead, depth_state, 0u);
    Transition(list, motion, kRead, motion_state, 0u);
  };
  // XeFG full-frame inputs can feed the core's original copy directly while
  // output stays private. Composition needs the unmodified staged backbuffer.
  const auto backbuffer_desc = input.backbuffer->GetDesc();
  const bool borrow_color_inputs = native_parameters == nullptr
      && input.color_x == 0 && input.color_y == 0
      && backbuffer_desc.Width == input.color_width && backbuffer_desc.Height == input.color_height;
  if (!borrow_color_inputs) {
    CopyColor(list, slot->color[0], kRead, input.backbuffer, input.backbuffer_state,
              input.color_x, input.color_y);
    if (has_hudless && !coupled_ui) {
      CopyColor(list, slot->color[1], kRead, input.hudless, input.hudless_state,
                input.color_x, input.color_y);
    }
  }
  if (has_hudless) {
    // Only UI composition reads the pre-NR snapshot; dual NR keeps its own inputs.
    if (may_compose) CopyColor(list, slot->original_hudless, kRead, slot->color[1], kRead);
  }
  gate_ever_opened.store(true, std::memory_order_relaxed);
  bool prepared = false;
  for (const uint32_t i : {1u, 0u}) {
    if (i == 1 && (!has_hudless || coupled_ui)) continue;
    auto& role = stream->roles[i];
    // Public motion relates adjacent application frames, not the last frame
    // for which a busy service could obtain a recording slot or NGX turn.
    if (input.reset || role.real_guides != real_guides || stream->epoch != input.epoch
        || (input.application_frame != 0 && stream->last_model_frame[i] != 0
            && input.application_frame != stream->last_model_frame[i] + 1)) {
      ResetPresentStreamHistory(&role);
    }
    role.real_guides = real_guides;
    FillPresentBlock(&role, slot->color[i], motion, depth, contract,
                     input.color_width, input.color_height, 1.f, input.reset ? 1 : 0);
    role.parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_X, input.jitter_x);
    role.parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, input.jitter_y);
    PresentTarget target = base;
    target.output_state = kRead;
    target.source_color = (borrow_color_inputs ? (i == 0 ? input.backbuffer : input.hudless) : nullptr);
    target.source_color_state = (i == 0 ? input.backbuffer_state : input.hudless_state);
    target.workset_key = reinterpret_cast<const ID3D12Resource*>(&role);
    target.capture_display = false;
    target.ui_correction = i == 0 && base.ui_correction;
    target.guide_serial = (real_guides ? input.application_frame + 1 : 0u);
    target.domain = (i == 0 ? present_restore_scope::Domain::PRE_FG_BACKBUFFER
                            : present_restore_scope::Domain::PRE_FG_HUDLESS);
    target.original_identity = reinterpret_cast<uintptr_t>(i == 0 ? input.backbuffer : input.hudless);
    auto& recording = (*recordings)[i];
    bool touched = false;
    const EvaluateInFlightScope attempt(true);
    const uint64_t previous_capture_serial = screenshot::PendingModelCaptureSerial(list);
    const bool accepted = ProcessInline(list, role.Handle(), &role.parameters, &touched, &target, &recording);
    prepared |= recording.prepared;
    const bool complete_pair = !base.native_prefg_resolution_pair
        || (NativePreFgResolutionPairAllowed(target)
            && recording.group_complete && recording.model_pass_mask == 3u);
    if (!accepted || !recording.output_ready || !complete_pair) {
      restore_guides();
      return (accepted && prepared ? fg::Result::PREPARED : fg::Result::FAILED);
    }
    {
      const auto capture_serial = screenshot::PendingModelCaptureSerial(list);
      if (capture_serial != 0 && capture_serial != previous_capture_serial) {
        const auto capture_guide = [&](const char* name, ID3D12Resource* resource) {
          const auto desc = resource->GetDesc();
          if (!screenshot::PrepareDiagnosticPlane(stream->device, name, resource,
                  static_cast<uint32_t>(desc.Width), desc.Height, std::nullopt, true, capture_serial)) return;
          Transition(list, resource, kRead, D3D12_RESOURCE_STATE_COPY_SOURCE, 0u);
          screenshot::RecordDiagnosticPlaneCopy(list, name, resource, capture_serial);
          Transition(list, resource, D3D12_RESOURCE_STATE_COPY_SOURCE, kRead, 0u);
        };
        capture_guide("guide_depth", depth);
        capture_guide("guide_motion", motion);
        uint32_t no_game = 0;
        const bool declared = GetUIntIfPresent(native_parameters,
            NVSDK_NGX_DLSSG_Parameter_NotRenderingGameFrames, &no_game);
        std::scoped_lock lock(screenshot::internal::state_mutex);
        if (screenshot::internal::pending && screenshot::internal::pending->serial == capture_serial) {
          screenshot::internal::pending->meta += "guide_source=native_fg\nguide_real="
              + std::to_string(real_guides) + "\nno_game_declared=" + std::to_string(declared)
              + "\nno_game_value=" + std::to_string(no_game) + "\n";
        }
      }
    }
    // Exact public UI alpha permits one-model composition. Without alpha
    // each consumed color has its own NR history and result.
    if (i == 1 && may_compose) {
      const uint64_t pending_capture_serial = screenshot::PendingModelCaptureSerial(list);
      const uint64_t capture_serial = (pending_capture_serial != previous_capture_serial ? pending_capture_serial : 0);
      bool capture_was_complete = false;
      if (capture_serial != 0) {
        std::scoped_lock lock(screenshot::internal::state_mutex);
        if (screenshot::internal::pending && screenshot::internal::pending->serial == capture_serial) {
          capture_was_complete = screenshot::internal::pending->required_planes_complete;
          // A present may hand off this capture before all five copies are recorded.
          screenshot::internal::pending->required_planes_complete = false;
        }
      }
      const auto capture_plane = [&](const char* name, ID3D12Resource* source, D3D12_RESOURCE_STATES state) {
        if (capture_serial == 0 || source == nullptr) return;
        const auto desc = source->GetDesc();
        if (!screenshot::PrepareDiagnosticPlane(stream->device, name, source,
                static_cast<uint32_t>(desc.Width), desc.Height, std::nullopt, true, capture_serial)) return;
        submission::TrackUse(list, source);
        Transition(list, source, state, D3D12_RESOURCE_STATE_COPY_SOURCE, 0u);
        screenshot::RecordDiagnosticPlaneCopy(list, name, source, capture_serial);
        Transition(list, source, D3D12_RESOURCE_STATE_COPY_SOURCE, state, 0u);
      };
      const char* ui_plane = (input.ui == PreFgResource(native_parameters, NVSDK_NGX_DLSSG_Parameter_UIAlpha)
                                  ? "native_ui_alpha" : "native_ui_color");
      capture_plane(ui_plane, input.ui, input.ui_state);
      capture_plane("native_original_hudless", slot->original_hudless, kRead);
      capture_plane("native_enhanced_hdrhl", slot->color[1], kRead);
      capture_plane("native_pre_compose", slot->color[0], kRead);
      Transition(list, input.ui, input.ui_state, kRead, 0u);
      submission::TrackUse(list, input.ui);
      const uint32_t composed = ComposePresentPreFg(list, native_parameters, slot->color[0],
          slot->original_hudless, slot->color[0]->GetDesc(), kRead, slot->color[1]);
      Transition(list, input.ui, kRead, input.ui_state, 0u);
      capture_plane("native_post_compose", slot->color[0], kRead);
      if (capture_serial != 0) {
        std::scoped_lock lock(screenshot::internal::state_mutex);
        if (screenshot::internal::pending && screenshot::internal::pending->serial == capture_serial) {
          size_t recorded_planes = 0;
          for (const auto& plane : screenshot::internal::pending->planes) {
            if ((plane.name == ui_plane || plane.name == "native_original_hudless"
                 || plane.name == "native_enhanced_hdrhl" || plane.name == "native_pre_compose"
                 || plane.name == "native_post_compose") && plane.recorded && plane.error.empty()) {
              ++recorded_planes;
            }
          }
          screenshot::internal::pending->required_planes_complete = capture_was_complete && recorded_planes == 5;
        }
      }
      if (composed == 0) break;
    }
  }
  if (coupled_ui) {
    RecordNativeColorInputs(stream, slot, list, input);
    auto& role = stream->roles[1];
    const auto primary = features.find(stream->roles[0].Handle());
    const auto auxiliary = features.find(role.Handle());
    bool models_ready = primary != features.end() && auxiliary != features.end()
        && primary->second.input_width == auxiliary->second.input_width
        && primary->second.input_height == auxiliary->second.input_height;
    if (models_ready) {
      for (uint32_t pass = 0; pass < std::clamp(stack_passes.load(), 1u, kMaxNrPasses); ++pass) {
        const auto& model = auxiliary->second.slots[pass];
        models_ready &= model.handle != nullptr && model.nr_hdr_mode == base.hdr_mode
            && model.nr_create_flags == NrCreateFlags(contract.create_flags, base.hdr_mode != 0)
            && model.configuration_generation == configuration_generation.load();
      }
    }
    if (!models_ready) {
      FillPresentBlock(&role, input.hudless, motion, depth, contract,
                       input.color_width, input.color_height, 1.f, 1);
      PresentTarget target = base;
      target.source_color = nullptr;
      target.output_state = kRead;
      target.workset_key = reinterpret_cast<const ID3D12Resource*>(&role);
      target.capture_display = false;
      target.ui_correction = false;
      target.domain = present_restore_scope::Domain::PRE_FG_HUDLESS;
      target.original_identity = reinterpret_cast<uintptr_t>(input.hudless);
      ProcessInline(list, role.Handle(), &role.parameters, nullptr, &target, nullptr, true);
    }
  }
  restore_guides();
  if (input.application_frame != 0) {
    for (uint32_t i = 0; i < recordings->size(); ++i) {
      if ((*recordings)[i].model_recorded) {
        stream->last_model_frame[i] = input.application_frame;
      }
    }
  }
  *output_ready = true;
  return fg::Result::PREPARED;
}

inline fg::Result Service(const fg::FrameInput& input) noexcept {
  try {
    if (!Requested() || input.queue == nullptr || input.backbuffer == nullptr
        || input.color_width == 0 || input.color_height == 0 || !input.fg_enabled) return fg::Result::NOT_SERVED;
    ID3D12CommandQueue* queue = input.queue;
    renodx::utils::directx::NativeFromReShadeProxy(&queue);
    if (!InstallQueueCompletionHooks(queue)) return fg::Result::NOT_SERVED;
    ID3D12Device* device = nullptr;
    if (FAILED(input.queue->GetDevice(IID_PPV_ARGS(&device)))) return fg::Result::FAILED;
    PrimeNgxLoaderSymbols(device);
    auto device_release = std::unique_ptr<ID3D12Device, void (*)(ID3D12Device*)>(device,
        [](ID3D12Device* value) { value->Release(); });
    const ngx_serial::PresentTurn turn(true);
    if (!turn.owns()) return fg::Result::NOT_SERVED;
    RuntimeLock lock(runtime_mutex);
    auto& stream = streams[{false, input.context}];
    if (stream.device != nullptr && (stream.device != device || stream.queue != queue || stream.epoch != input.epoch)) {
      Release(&stream);
      stream = {};
    }
    if (stream.device == nullptr) {
      stream.device = device;
      device->AddRef();
      stream.queue = queue;
      queue->AddRef();
      stream.epoch = input.epoch;
      if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&stream.fence)))) {
        Release(&stream);
        stream = {};
        return fg::Result::FAILED;
      }
    }
    if (stream.has_frame && stream.application_frame == input.application_frame) return stream.result;
    stream.has_frame = true;
    stream.application_frame = input.application_frame;
    stream.result = fg::Result::NOT_SERVED;
    Slot* slot = nullptr;
    for (auto& candidate : stream.slots) {
      if (!candidate.quarantined && (candidate.fence_value == 0
          || stream.fence->GetCompletedValue() >= candidate.fence_value)) {
        slot = &candidate;
        break;
      }
    }
    if (slot == nullptr) return stream.result;
    const auto type = queue->GetDesc().Type;
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE) return stream.result;
    if (slot->allocator == nullptr
        && FAILED(device->CreateCommandAllocator(type, IID_PPV_ARGS(&slot->allocator)))) {
      stream.result = fg::Result::FAILED;
      return stream.result;
    }
    if (slot->list == nullptr) {
      if (FAILED(device->CreateCommandList(0, type, slot->allocator, nullptr, IID_PPV_ARGS(&slot->list)))) {
        stream.result = fg::Result::FAILED;
        return stream.result;
      }
      slot->list->Close();
    }
    if (FAILED(slot->allocator->Reset()) || FAILED(slot->list->Reset(slot->allocator, nullptr))) {
      stream.result = fg::Result::FAILED;
      return stream.result;
    }
    submission::OnCommandListReset(slot->list);
    bool submitted = false;
    const auto cancel_recording = [&](Slot*) noexcept {
      if (submitted) return;
      try {
        slot->list->Close();
        submission::OnCommandListReset(slot->list);
        Invalidate(&stream);
      } catch (...) {
      }
    };
    const std::unique_ptr<Slot, decltype(cancel_recording)> cancellation(slot, cancel_recording);
    fg::FrameInput nr_input = input;
    if (input.ui_composition && input.ui_mode >= 3 && nr_input.hudless == nullptr) {
      slot->list->Close();
      return stream.result;
    }
    if (!PrepareSurfaces(&stream, slot, nr_input)) {
      slot->list->Close();
      stream.result = fg::Result::FAILED;
      return stream.result;
    }
    PresentTarget target;
    SnapshotPresentRestoreConfiguration(&target);
    target.swapchain_identity = reinterpret_cast<uintptr_t>(input.swapchain);
    target.swapchain_format = input.backbuffer->GetDesc().Format;
    if (input.color_space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
      target.hdr_mode = 2;
      target.units = {codec::Encoding::Pq, true, 10000.f, true};
    } else if (input.color_space == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709) {
      target.hdr_mode = 1;
      target.units = {codec::Encoding::Linear, true, 80.f, false};
    } else if (input.color_space != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) {
      slot->list->Close();
      return stream.result;
    }
    const float mod_white = (present_white_source.load() == 0u ? present_mod_white.load() : 0.f);
    const float white = (present_white_nits.load() > 0.f ? present_white_nits.load()
        : (mod_white > 0.f ? mod_white : (present_sdr_white.load() > 0.f
            ? present_sdr_white.load() : diffuse_white_nits.load())));
    const uint32_t mode = codec_mode.load();
    target.hdr_codec_mode = mode;
    target.hdr_curve = (FrameDisplayCodec(target, mode) ? 1u : (neural_floor_guard.load() ? 2u : 0u));
    target.hdr_white_nits = white;
    target.hdr_white_source = "configured_present_white";
    target.hdr_encoding = V6EncodingConstant(target.units);
    target.hdr_dark_gate = V6DarkGate(target.units);
    target.hdr_pedestal_cap = codec::BlackPedestalCap(target.units);
    target.hdr_pedestal = present_hdr_pedestal::Decide(target.hdr_pedestal.configuration,
        {.actual_present = true, .hdr_mode = target.hdr_mode, .v6_available = mode != 0u,
         .encoding_valid = target.hdr_mode == 2 || target.hdr_mode == 1,
         .absolute_units = target.units.absolute, .unit_nits = target.units.unit_nits,
         .display_codec = target.hdr_curve == 1u, .legacy_mode = target.hdr_pedestal.legacy_mode,
         .debug_bypass = target.diagnostic.bypass_hdr_pedestal});
    target.divisor = (target.hdr_mode != 0 && mode != 1u && mode != 3u
        ? codec::PresentDivisor(target.units, white, paper_white_scale.load(), pq_calibration.load()) : 0.f);
    target.ui_correction = present_ui_correction.load();
    const EvaluateChainScope chain;
    // XeFG init flags are a different ABI from NGX feature flags. Motion
    // scale is already supplied in pixels by XeFG's public constants.
    nr_input.init_flags = ((input.init_flags & 1u) != 0 ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0)
        | ((input.init_flags & (1u << 4)) != 0 ? NVSDK_NGX_DLSS_Feature_Flags_MVJittered : 0)
        | (input.guide_width < input.color_width ? NVSDK_NGX_DLSS_Feature_Flags_MVLowRes : 0);
    if ((input.init_flags & (1u << 3)) != 0) {
      nr_input.motion_scale_x *= static_cast<float>(input.guide_width) * 0.5f;
      nr_input.motion_scale_y *= -static_cast<float>(input.guide_height) * 0.5f;
    }
    bridge::OwnedParameters composition;
    const bool exact_ui = nr_input.hudless != nullptr && input.ui_composition && input.ui_mode == 3
        && input.ui != nullptr
        && input.color_x == 0 && input.color_y == 0
        && input.ui->GetDesc().Width == input.color_width && input.ui->GetDesc().Height == input.color_height;
    if (exact_ui) composition.Set(NVSDK_NGX_DLSSG_Parameter_UI, input.ui);
    bool output_ready = false;
    std::array<NrRecording, 2> recordings;
    const auto recorded = Record(&stream, slot, slot->list, nr_input, target,
                                 &output_ready, &recordings, exact_ui ? &composition : nullptr);
    if (!output_ready) {
      const bool closed = SUCCEEDED(slot->list->Close());
      if (recorded == fg::Result::PREPARED && closed) {
        // Feature creation/maturation is real GPU work, but no color is
        // committed and the application frame is explicitly not served.
        for (uint32_t i = 0; i < recordings.size(); ++i) {
          if (recordings[i].model_invoked) ResetPresentStreamHistory(&stream.roles[i]);
        }
        ID3D12CommandList* list = slot->list;
        renodx::utils::directx::NativeFromReShadeProxy(&list);
        submission::TrackUse(slot->list, slot->allocator);
        submission::TrackUse(slot->list, slot->list);
        queue->ExecuteCommandLists(1, &list);
        submitted = true;
        slot->fence_value = ++stream.submitted;
        if (FAILED(queue->Signal(stream.fence, slot->fence_value))) slot->quarantined = true;
      } else {
        submission::OnCommandListReset(slot->list);
        Invalidate(&stream);
      }
      stream.result = (slot->quarantined ? fg::Result::FAILED : recorded);
      return stream.result;
    }
    CopyColor(slot->list, input.backbuffer, input.backbuffer_state, slot->color[0],
              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, 0, 0, input.color_x, input.color_y,
              input.color_width, input.color_height);
    if (nr_input.hudless != nullptr && nr_input.hudless != input.backbuffer) {
      CopyColor(slot->list, input.hudless, input.hudless_state, slot->color[1],
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, 0, 0, input.color_x, input.color_y,
                input.color_width, input.color_height);
    }
    if (FAILED(slot->list->Close())) {
      submission::OnCommandListReset(slot->list);
      Invalidate(&stream);
      stream.result = fg::Result::FAILED;
      return stream.result;
    }
    submission::TrackUse(slot->list, slot->allocator);
    submission::TrackUse(slot->list, slot->list);
    ID3D12CommandList* list = slot->list;
    renodx::utils::directx::NativeFromReShadeProxy(&list);
    queue->ExecuteCommandLists(1, &list);
    submitted = true;
    slot->fence_value = ++stream.submitted;
    if (FAILED(queue->Signal(stream.fence, slot->fence_value))) {
      slot->quarantined = true;
      Invalidate(&stream);
      stream.result = fg::Result::FAILED;
      return stream.result;
    }
    stream.result = fg::Result::SUBMITTED;
    PublishPresentRestoreOwner(present_restore_scope::Domain::PRE_FG_BACKBUFFER,
        target.restore_decision.reason, target.restore_contract_compatible,
        input.application_frame, target.restore_config_epoch, "fg_service_submitted");
    return stream.result;
  } catch (...) {
    return fg::Result::FAILED;
  }
}

}  // namespace fg_service

inline void PreserveNativePreFgProof(
    const void* marker, const std::vector<submission::GenerationSubmission>& submissions) {
  if (screenshot::WantsFinalPresent() || experimental_present_chain.load()) {
    PreservePresentCaptureProof(marker, submissions);
  }
  // An opt-out must still retain the previous producer's exact receipt so
  // a same-frame rollback never treats a pruned recording as unproven work.
  for (auto& [key, stream] : fg_service::streams) {
    if (!stream.native_resolution_pair || marker != stream.native_pair_producer_marker) continue;
    if (submissions.empty()) {
      stream.native_pair_discarded = true;
      continue;
    }
    for (const auto& proof : submissions) {
      if (std::pair{proof.list_identity, proof.recording_generation} != stream.native_pair_recording) continue;
      if (submissions.size() != 1 || proof.pending || proof.fence.empty()) {
        stream.native_pair_replayed = true;
        continue;
      }
      stream.native_pair_completion = proof.fence;
      stream.native_pair_completion_value = proof.value;
    }
  }
}

inline void OnNativePresentPassthrough(const PresentStream* owner) {
  const auto binding = fg_service::native_bindings.find(native_fg_owner);
  if (binding == fg_service::native_bindings.end()
      || binding->second != reinterpret_cast<uintptr_t>(owner)) return;
  native_fg_owner = nullptr;
  native_fg_owns.store(false, std::memory_order_release);
  native_fg_marker.store(nullptr, std::memory_order_release);
  native_fg_continuing_submission.store(false, std::memory_order_release);
  native_fg_result.store(fg::Result::NOT_SERVED, std::memory_order_release);
}

inline void OnNativeFgFeatureReleased(const NVSDK_NGX_Handle* handle) {
  if (handle == nullptr) return;
  if (native_fg_owner == handle) {
    native_fg_owner = nullptr;
    native_fg_owns.store(false, std::memory_order_release);
    native_fg_marker.store(nullptr, std::memory_order_release);
    native_fg_continuing_submission.store(false, std::memory_order_release);
    native_fg_result.store(fg::Result::NOT_SERVED, std::memory_order_release);
  }
  const auto binding = fg_service::native_bindings.find(handle);
  if (binding == fg_service::native_bindings.end()) return;
  if (const auto entry = fg_service::streams.find({true, binding->second});
      entry != fg_service::streams.end() && entry->second.native_handle == handle) {
    fg_service::ResetNativeFrame(&entry->second);
    entry->second.native_handle = nullptr;
  }
  fg_service::native_bindings.erase(binding);
}

inline void OnNativePresentStreamRetired(const void* owner) {
  const auto context = reinterpret_cast<uintptr_t>(owner);
  for (auto binding = fg_service::native_bindings.begin(); binding != fg_service::native_bindings.end();) {
    if (binding->second == context) {
      if (native_fg_owner == binding->first) {
        native_fg_owner = nullptr;
        native_fg_owns.store(false, std::memory_order_release);
        native_fg_marker.store(nullptr, std::memory_order_release);
        native_fg_continuing_submission.store(false, std::memory_order_release);
        native_fg_result.store(fg::Result::NOT_SERVED, std::memory_order_release);
      }
      binding = fg_service::native_bindings.erase(binding);
    } else {
      ++binding;
    }
  }
  if (const auto entry = fg_service::streams.find({true, context}); entry != fg_service::streams.end()) {
    fg_service::Release(&entry->second);
    fg_service::streams.erase(entry);
  }
}

inline ID3D12Resource* RunNativePreFg(ID3D12GraphicsCommandList* list,
                                     const NVSDK_NGX_Parameter* parameters,
                                     const NVSDK_NGX_Handle* handle, bool* backbuffer_replaced) {
  // Observation only. Log the first unmet condition, rather than attributing
  // a downstream NOT_SERVED verdict to an input that was never inspected.
  struct ExperimentalDiagnostic {
    bool enabled;
    ID3D12GraphicsCommandList* list;
    const NVSDK_NGX_Parameter* parameters;
    const NVSDK_NGX_Handle* handle;
    const char* reason = "entry_contract";
    const PresentStream* owner = nullptr;
    fg_service::Stream* stream = nullptr;
    std::array<NrRecording, 2> recordings{};
    ~ExperimentalDiagnostic() noexcept {
      if (!enabled) return;
      try {
        static std::string last_signature;
        static ULONGLONG last_ms = 0;
        static uint32_t emitted = 0;
        const auto now = GetTickCount64();
        if (emitted >= 64 || (emitted != 0 && now - last_ms < 1000)) return;
        // Throttle observation too: an unchanged signature must not cause
        // resource/parameter queries on every later frame.
        last_ms = now;
        std::ostringstream line;
        line << "native-prefg-experiment reason=" << reason;
        const auto source = created_feature_ids.find(handle);
        line << " feature=" << (source == created_feature_ids.end() ? -1 : static_cast<int>(source->second));
        const auto uir = native_fg_ui_recomposition.find(handle);
        line << " uir=" << (uir == native_fg_ui_recomposition.end() ? -1 : static_cast<int>(uir->second));
        unsigned long long frame = 0;
        const auto frame_result = (parameters != nullptr ? parameters->Get(
            NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, &frame) : NVSDK_NGX_Result_FAIL_InvalidParameter);
        static uint32_t identity_probes = 0;
        if (parameters != nullptr && source != created_feature_ids.end()
            && source->second == NVSDK_NGX_Feature_FrameGeneration
            && GetUInt(parameters, NVSDK_NGX_DLSSG_Parameter_NotRenderingGameFrames) == 0
            && identity_probes < 3) {
          // The public interface has no key/type enumeration. Compare all
          // documented Get overloads with an unset control, without writing
          // either key or treating a readable value as a proven frame token.
          bool alternate_readable = false;
          const auto probe = [&]<typename T>(const char* key, const char* type, T value) {
            const auto result = parameters->Get(key, &value);
            line << ' ' << type << "=[result=0x" << std::hex << static_cast<uint32_t>(result) << std::dec;
            if (NVSDK_NGX_SUCCEED(result)) {
              if constexpr (std::is_pointer_v<T>) {
                line << " nonnull=" << (value != nullptr);
              } else {
                line << " value=" << value;
              }
            }
            line << ']';
            return NVSDK_NGX_SUCCEED(result);
          };
          line << " identity_probe=" << ++identity_probes << "/3 frame_id_getters={";
          for (const char* key : {NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, "NRNexus.AbsentFrameIdentityControl"}) {
            line << " key=" << key;
            const bool readable_ull = probe(key, "ull", 0ull);
            const bool readable_ui = probe(key, "uint", 0u);
            const bool readable_i = probe(key, "int", 0);
            const bool readable_f = probe(key, "float", 0.f);
            const bool readable_d = probe(key, "double", 0.);
            const bool readable_p = probe(key, "voidp", static_cast<void*>(nullptr));
            const bool readable_11 = probe(key, "d3d11", static_cast<ID3D11Resource*>(nullptr));
            const bool readable_12 = probe(key, "d3d12", static_cast<ID3D12Resource*>(nullptr));
            if (std::string_view(key) == NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID) {
              alternate_readable = readable_ull || readable_ui || readable_i || readable_f
                  || readable_d || readable_p || readable_11 || readable_12;
            }
          }
          line << " } frame_id_class="
               << (NVSDK_NGX_SUCCEED(frame_result) ? "uint64_readable_semantics_unverified"
                   : (alternate_readable ? "alternate_getter_readable_semantics_unverified"
                                         : "unreadable_absence_or_type_unproven"));
          line << " known_backbuffer_control={";
          probe(NVSDK_NGX_DLSSG_Parameter_Backbuffer, "d3d12", static_cast<ID3D12Resource*>(nullptr));
          probe(NVSDK_NGX_DLSSG_Parameter_Backbuffer, "voidp", static_cast<void*>(nullptr));
          line << " }";
        }
        uint32_t index = 1;
        const bool index_available = GetUIntIfPresent(parameters, NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, &index);
        const auto plan = RequestedStagePlan();
        line << " plan_valid=" << plan.valid << " mixed=" << plan.mixed << " passes=" << plan.pass_count
             << " present_passes=" << plan.At(stage_plan::Stage::PRESENT).count
             << " frame_id_available=" << NVSDK_NGX_SUCCEED(frame_result)
             << " frame_id_nonzero=" << (frame != 0)
             << " index_available=" << index_available << " index=" << index
             << " no_game=" << GetUInt(parameters, NVSDK_NGX_DLSSG_Parameter_NotRenderingGameFrames);
        auto* back = PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_Backbuffer);
        auto* hudless = PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_HUDLess);
        for (const auto& [name, key] : {std::pair{"back", NVSDK_NGX_DLSSG_Parameter_Backbuffer},
                 {"hudless", NVSDK_NGX_DLSSG_Parameter_HUDLess}, {"ui", NVSDK_NGX_DLSSG_Parameter_UI},
                 {"alpha", NVSDK_NGX_DLSSG_Parameter_UIAlpha}, {"depth", NVSDK_NGX_DLSSG_Parameter_Depth},
                 {"motion", NVSDK_NGX_DLSSG_Parameter_MVecs}}) {
          auto* resource = PreFgResource(parameters, key);
          const auto desc = (resource != nullptr ? resource->GetDesc() : D3D12_RESOURCE_DESC{});
          line << ' ' << name << "=[present=" << (resource != nullptr) << " size=" << desc.Width << 'x' << desc.Height
               << " format=" << static_cast<uint32_t>(desc.Format) << " back_alias="
               << (resource != nullptr && back != nullptr && renodx::utils::directx::SameNativeObject(resource, back))
               << " hudless_alias=" << (resource != nullptr && hudless != nullptr
                   && renodx::utils::directx::SameNativeObject(resource, hudless)) << ']';
        }
        for (uint32_t pass = 0; pass < 2; ++pass) {
          const auto resolution = CurrentPassResolution(pass);
          line << " pass" << pass + 1 << "=[follow=" << resolution.follow_input << " scale=" << resolution.scale << ']';
        }
        for (const char* key : {"DLSSG.MVecsSubrectBaseX", "DLSSG.MVecsSubrectBaseY",
                 "DLSSG.MVecsSubrectWidth", "DLSSG.MVecsSubrectHeight",
                 "DLSSG.DepthSubrectBaseX", "DLSSG.DepthSubrectBaseY"}) {
          uint32_t value = 0;
          const bool available = GetUIntIfPresent(parameters, key, &value);
          line << ' ' << key << "=[available=" << available << " value=" << value << ']';
        }
        for (uint32_t role = 0; role < recordings.size(); ++role) {
          const auto& record = recordings[role];
          line << " plane" << role << "=[prepared=" << record.prepared << " output_ready=" << record.output_ready
               << " model_invoked=" << record.model_invoked << " model_recorded=" << record.model_recorded
               << " group_complete=" << record.group_complete << " model_pass_mask=" << record.model_pass_mask << ']';
        }
        line << " fg_handle=" << handle << " owner=" << owner
             << " frame_id_get_result=" << static_cast<uint32_t>(frame_result)
             << " frame_id_type=uint64"
             << " guide_mode=" << present_guides.load()
             << " queue_tracking=" << queue_tracking_active.load();
        if (stream != nullptr && stream->native_pair_producer_marker != nullptr) {
          std::vector<std::pair<FenceRef, uint64_t>> proofs;
          const auto state = submission::UseSubmissionTokens(stream->native_pair_producer_marker, &proofs);
          bool complete = !proofs.empty();
          for (const auto& [fence, value] : proofs) {
            const auto done = fence.fence->GetCompletedValue();
            complete &= done != UINT64_MAX && done >= value;
          }
          line << " producer_state=" << static_cast<uint32_t>(state) << " receipts=" << proofs.size()
               << " receipts_complete=" << complete << " cached=" << (stream->cached != nullptr)
               << " replayed=" << stream->native_pair_replayed << " discarded=" << stream->native_pair_discarded;
        } else {
          line << " producer=unavailable";
        }
        // Frame IDs and recording generations advance every frame. Keep
        // identity/contract/proof changes in the signature, but not counters.
        const auto signature = line.str();
        if (signature == last_signature) return;
        last_signature = signature;
        std::pair<uint64_t, uint64_t> recording{};
        {
          // Never create or advance a recording merely to explain a rejection.
          std::lock_guard lock(submission::tracker_mutex);
          const auto identity = submission::identity_by_pointer.find(list);
          if (identity != submission::identity_by_pointer.end()) {
            const auto state = submission::tracked_lists.find(identity->second);
            if (state != submission::tracked_lists.end()) recording = {identity->second, state->second.current};
          }
        }
        line << " frame_id=" << frame << " recording=" << recording.first << ':' << recording.second;
        line << " gpu_execution=unverified log_budget=" << ++emitted << "/64";
        LogPresent(reshade::log::level::info, line.str());
      } catch (...) {
        // Diagnostic allocation/logging must not change FG execution or unwind.
      }
    }
  } diagnostic{.enabled = experimental_native_prefg_resolution.load(), .list = list,
               .parameters = parameters, .handle = handle};
  if (backbuffer_replaced != nullptr) {
    *backbuffer_replaced = false;
  }
  // The overlay reads atomics without runtime_mutex. Publish a declined
  // recording on exit, not a transient outage before the model is called.
  struct DisplayRecording {
    bool prepared = false;
    fg::Result result = fg::Result::NOT_SERVED;
    ~DisplayRecording() {
      if (!prepared) {
        native_fg_continuing_submission.store(false, std::memory_order_release);
        native_fg_result.store(result, std::memory_order_release);
      }
    }
  } display_recording;
  diagnostic.reason = "xefg_owns_stream";
  if (fg::xefg::OwnsStream()) return nullptr;
  diagnostic.reason = "prefg_not_requested";
  if (!fg_service::Requested()) return nullptr;
  diagnostic.reason = "command_list_missing";
  if (list == nullptr) return nullptr;
  diagnostic.reason = "public_fg_off_without_present_decisions";
  if (native_fg_public_off.load(std::memory_order_acquire) && !fg::native_options::ObservesPresentDecisions()) return nullptr;
  diagnostic.reason = "queue_tracking_unavailable";
  if (!queue_tracking_active.load(std::memory_order_acquire)) return nullptr;
  auto* backbuffer = PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_Backbuffer);
  auto* hudless = PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_HUDLess);
  auto* ui = PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_UI);
  diagnostic.reason = "backbuffer_missing";
  if (backbuffer == nullptr) return nullptr;
  // FG inputs are NPS and outputs are UAV. Restoring an aliased input would
  // also overwrite FG's output, so reject the unsupported contract up front.
  for (const char* key : {NVSDK_NGX_DLSSG_Parameter_OutputReal,
                          NVSDK_NGX_DLSSG_Parameter_OutputInterpolated}) {
    diagnostic.reason = "fg_output_alias";
    if (auto* output = PreFgResource(parameters, key); output != nullptr
        && (output == backbuffer || renodx::utils::directx::SameNativeObject(output, backbuffer))) return nullptr;
  }
  const auto desc = backbuffer->GetDesc();
  ID3D12Device* device = nullptr;
  diagnostic.reason = "list_device";
  if (FAILED(list->GetDevice(IID_PPV_ARGS(&device)))) return nullptr;
  auto device_release = std::unique_ptr<ID3D12Device, void (*)(ID3D12Device*)>(device,
      [](ID3D12Device* value) { value->Release(); });
  const auto identity = renodx::utils::directx::NativeIdentity(device);
  const PresentStream* owner = nullptr;
  for (const auto& [key, candidate] : present_streams) {
    if (!candidate.d3d11 && candidate.prefg_target_valid
        && (candidate.device_identity == identity || candidate.streamline_identity == identity)
        && candidate.backbuffer_width == desc.Width && candidate.backbuffer_height == desc.Height
        && ConcreteResourceFormat(candidate.backbuffer_format) == ConcreteResourceFormat(desc.Format)) {
      diagnostic.reason = "owner_ambiguous";
      if (owner != nullptr) return nullptr;
      owner = &candidate;
    }
  }
  diagnostic.reason = "owner_missing";
  if (owner == nullptr) return nullptr;
  diagnostic.owner = owner;
  auto* current_depth = PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_Depth);
  auto* current_motion = PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_MVecs);
  const auto motion_desc = (current_motion != nullptr ? current_motion->GetDesc() : D3D12_RESOURCE_DESC{});
  const bool current_guides = current_depth != nullptr && current_motion != nullptr
      && GetUInt(parameters, NVSDK_NGX_DLSSG_Parameter_MVecsSubrectWidth,
                 static_cast<uint32_t>(motion_desc.Width)) != 0
      && GetUInt(parameters, NVSDK_NGX_DLSSG_Parameter_MVecsSubrectHeight, motion_desc.Height) != 0
      && GetUInt(parameters, NVSDK_NGX_DLSSG_Parameter_NotRenderingGameFrames) == 0;
  const auto pair_plan = RequestedStagePlan();
  const bool resolution_pair = experimental_native_prefg_resolution.load()
      && pair_plan.valid && !pair_plan.mixed && pair_plan.pass_count == 2
      && pair_plan.At(stage_plan::Stage::PRESENT).count == 2;
  const uint32_t index = GetUInt(parameters, NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 1);
  unsigned long long frame_id = 0;
  if (NVSDK_NGX_FAILED(parameters->Get(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, &frame_id))) frame_id = 0;
  if (resolution_pair) {
    const auto source = created_feature_ids.find(handle);
    diagnostic.reason = "fg_feature_unrecognized";
    if (handle == nullptr || source == created_feature_ids.end()
        || source->second != NVSDK_NGX_Feature_FrameGeneration) return nullptr;
    diagnostic.reason = "frame_id_uint64_unreadable_or_zero";
    if (frame_id == 0) return nullptr;
    diagnostic.reason = "single_backbuffer_requires_no_hudless_ui";
    if (hudless != nullptr || ui != nullptr
        || PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_UIAlpha) != nullptr) return nullptr;
    diagnostic.reason = "real_guides_unavailable";
    if (!current_guides) return nullptr;
    diagnostic.reason = "xefg_enabled";
    if (fg::xefg::Enabled()) return nullptr;
    diagnostic.reason = "guide_mode_never";
    if (diagnostics::GuideMode(owner->prefg_target.diagnostic.guides,
                             present_guides.load()) == kPresentGuidesNever) return nullptr;
    const uint32_t guide_width = GetUInt(parameters, NVSDK_NGX_DLSSG_Parameter_MVecsSubrectWidth,
                                        static_cast<uint32_t>(motion_desc.Width));
    const uint32_t guide_height = GetUInt(parameters, NVSDK_NGX_DLSSG_Parameter_MVecsSubrectHeight,
                                         motion_desc.Height);
    diagnostic.reason = "guide_subrect_invalid";
    if (!SubrectFits(GetUInt(parameters, "DLSSG.MVecsSubrectBaseX"),
                    GetUInt(parameters, "DLSSG.MVecsSubrectBaseY"), guide_width, guide_height, motion_desc)
        || !SubrectFits(GetUInt(parameters, "DLSSG.DepthSubrectBaseX"),
                       GetUInt(parameters, "DLSSG.DepthSubrectBaseY"), guide_width, guide_height,
                       current_depth->GetDesc())) return nullptr;
  }
  if (native_sdk_present_input.decision_id != 0
      && native_sdk_present_input.identity == owner->native_output_identity) {
    if (!current_guides) {
      native_sdk_present_input = {.identity = native_sdk_present_input.identity,
          .decision_id = native_sdk_present_input.decision_id, .owner = owner,
          .route_id = owner->native_route_id, .backbuffer = backbuffer, .guides_unavailable = true};
    } else {
      native_sdk_present_input.guides_unavailable = false;
    }
  }
  // Temporary FG stops can return this output to its existing Final Present
  // model. Retain that model, without inventing a new guide contract here.
  owner_resident_nr_handles.insert(owner->Handle());
  owner_resident_nr_handles.insert(owner->Handle(true));
  ComputeStateEnvelope envelope(list);
  envelope.StopCapture();
  diagnostic.reason = "command_state_unproven";
  if (!envelope.CanInject()) return nullptr;
  // NR models belong to the matched output. A game FG feature may restart
  // on focus changes without changing the NR image contract.
  const auto context = (handle != nullptr ? reinterpret_cast<uintptr_t>(owner) : reinterpret_cast<uintptr_t>(parameters));
  auto& stream = fg_service::streams[{true, context}];
  diagnostic.stream = &stream;
  if (stream.result == fg::Result::FAILED) {
    fg_service::Invalidate(&stream);
    stream.result = fg::Result::FAILED;
  }
  if (stream.device != nullptr && stream.device != device) {
    fg_service::Release(&stream);
    stream = {};
  }
  if (stream.device == nullptr) {
    stream.device = device;
    device->AddRef();
    stream.native = true;
    if (handle != nullptr) {
      for (auto& role : stream.roles) {
        owner_resident_nr_handles.insert(role.Handle());
      }
    }
  }
  if (handle != nullptr) {
    fg_service::native_bindings[handle] = context;
  }
  const auto configuration = configuration_generation.load(std::memory_order_relaxed);
  const auto options_generation = native_fg_options_generation.load(std::memory_order_acquire);
  const std::array pair_resolutions{CurrentPassResolution(0), CurrentPassResolution(1)};
  const auto same_resolutions = [](const auto& first, const auto& second) {
    for (size_t i = 0; i < first.size(); ++i) {
      if (first[i].follow_input != second[i].follow_input || first[i].scale != second[i].scale
          || first[i].commit_source != second[i].commit_source || first[i].commit_epoch != second[i].commit_epoch) return false;
    }
    return true;
  };
  if (stream.native_resolution_pair && stream.native_pair_producer_marker != nullptr) {
    const bool same_recording = stream.native_pair_recording == submission::RecordingIdentity(list);
    std::vector<std::pair<FenceRef, uint64_t>> proofs;
    const auto proof = submission::UseSubmissionTokens(stream.native_pair_producer_marker, &proofs);
    bool completed_producer = proof == submission::UseSubmission::kTokens && proofs.size() == 1;
    if (proof == submission::UseSubmission::kComplete && !stream.native_pair_completion.empty()
        && !stream.native_pair_discarded && !stream.native_pair_replayed) {
      const auto completed = stream.native_pair_completion.fence->GetCompletedValue();
      completed_producer = completed != UINT64_MAX && completed >= stream.native_pair_completion_value;
    }
    for (const auto& [fence, value] : proofs) {
      const auto completed = fence.fence->GetCompletedValue();
      completed_producer &= completed != UINT64_MAX && completed >= value;
    }
    // Another list has no known queue ordering here. Poll a single genuine
    // producer completion; an empty/discarded recording is never production.
    if (stream.native_pair_replayed || stream.native_pair_discarded || proofs.size() > 1
        || (!same_recording && !completed_producer)) {
      diagnostic.reason = "producer_discarded_replayed_or_unfinished";
      if (frame_id == stream.source_frame_id || (!proofs.empty() && !completed_producer)) return nullptr;
      fg_service::Invalidate(&stream);
    } else if (stream.cached != nullptr && frame_id == stream.source_frame_id
               && (!resolution_pair || stream.native_configuration != configuration
                   || stream.native_handle != handle || stream.native_options_generation != options_generation
                   || stream.native_display_epoch != history_reset_epoch.load()
                   || !same_resolutions(stream.native_pair_resolutions, pair_resolutions))) {
      const auto original_desc = stream.cached->original_backbuffer->GetDesc();
      if (backbuffer == stream.source_backbuffer && desc.Width == original_desc.Width
          && desc.Height == original_desc.Height && desc.Format == original_desc.Format
          && desc.SampleDesc.Count == original_desc.SampleDesc.Count) {
        fg_service::CopyColor(list, backbuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                              stream.cached->original_backbuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      }
      fg_service::ResetNativeFrame(&stream);
      diagnostic.reason = "cached_configuration_changed";
      return nullptr;
    }
  }
  if (stream.native_resolution_pair != resolution_pair) fg_service::ResetNativeFrame(&stream);
  if (stream.native_configuration != configuration || stream.native_handle != handle
      || stream.native_options_generation != options_generation) {
    fg_service::ResetNativeFrame(&stream);
    stream.native_configuration = configuration;
    stream.native_options_generation = options_generation;
    stream.native_handle = handle;
    native_fg_marker.store(nullptr, std::memory_order_release);
    native_fg_continuing_submission.store(false, std::memory_order_release);
    native_fg_result.store(fg::Result::PREPARED, std::memory_order_release);
  }
  // NGX DLSS-FG requires NPS for every input on both direct and compute lists.
  constexpr auto input_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  const bool same_sdk_frame = frame_id == 0 && stream.source_frame_id == 0
      && native_sdk_present_input.decision_id != 0
      && native_sdk_present_input.identity == owner->native_output_identity
      && stream.source_sdk_decision == native_sdk_present_input.decision_id;
  const bool repeated_frame = ((frame_id != 0 && frame_id == stream.source_frame_id) || same_sdk_frame)
      && stream.cached != nullptr && stream.result == fg::Result::PREPARED
      && stream.source_backbuffer == backbuffer && stream.source_hudless == hudless && stream.source_ui == ui;
  if (index > 1 || repeated_frame) {
    diagnostic.reason = "mfg_cache_unavailable";
    if (stream.cached == nullptr && stream.result == fg::Result::PREPARED) {
      display_recording.result = fg::Result::PREPARED;
      return nullptr;
    }
    if (stream.result == fg::Result::FAILED) {
      display_recording.result = fg::Result::FAILED;
    }
    if (stream.cached == nullptr || stream.result != fg::Result::PREPARED
        || stream.source_backbuffer != backbuffer || stream.source_hudless != hudless
        || stream.source_ui != ui
        || (frame_id != 0 && frame_id != stream.source_frame_id)) return nullptr;
    // Reuse bypasses Record. A real-guide cache does not authorize NR after
    // this invocation withdraws its guides or declares no game frame.
    if (diagnostics::GuideMode(owner->prefg_target.diagnostic.guides,
                              present_guides.load(std::memory_order_relaxed)) == kPresentGuidesRequired
        && (!stream.roles[0].real_guides || !current_guides)) {
      fg_service::ResetNativeFrame(&stream);
      stream.result = fg::Result::NOT_SERVED;
      native_fg_marker.store(nullptr, std::memory_order_release);
      diagnostic.reason = "mfg_guides_withdrawn";
      return nullptr;
    }
  } else {
    // A failed or invalidated pair cannot become a second attempt on the same
    // game frame by changing its pass count or disabling the experiment.
    diagnostic.reason = "frame_already_attempted_or_nonmonotonic";
    if (stream.native_pair_attempt_frame != 0 && frame_id <= stream.native_pair_attempt_frame) return nullptr;
    if (resolution_pair) {
      diagnostic.reason = "first_index_not_one";
      if (index != 1) return nullptr;
      stream.native_pair_attempt_frame = frame_id;
    }
    // Only the same stream's adjacent, queue-proven recording can keep the
    // service active while this list records. A skipped recording breaks it.
    const auto display_epoch = history_reset_epoch.load(std::memory_order_relaxed);
    stream.continuing_submission = stream.native_serial != 0
        && stream.native_serial == native_fg_recording_serial.load(std::memory_order_acquire)
        && stream.native_serial == native_fg_counted_serial.load(std::memory_order_relaxed)
        && stream.native_display_epoch == display_epoch;
    stream.native_serial = 0;
    native_fg_marker.store(nullptr, std::memory_order_release);
    native_fg_result.store(fg::Result::PREPARED, std::memory_order_release);
    stream.cached = nullptr;
    stream.result = fg::Result::NOT_SERVED;
    fg_service::Slot* slot = nullptr;
    for (auto& candidate : stream.slots) {
      if (submission::ResourceReleasable(candidate.color[0])
          && submission::ResourceReleasable(candidate.color[1])
          && submission::ResourceReleasable(candidate.original_backbuffer)
          && submission::ResourceReleasable(candidate.original_hudless)
          && submission::ResourceReleasable(candidate.coupled_hudless)
          && submission::ResourceReleasable(candidate.coupled_ui)
          && submission::ResourceReleasable(candidate.color_heap)) {
        slot = &candidate;
        break;
      }
    }
    if (slot == nullptr) {
      stream.slots.emplace_back();
      slot = &stream.slots.back();
    }
    fg::FrameInput input;
    input.backbuffer = backbuffer;
    input.hudless = hudless;
    input.backbuffer_state = input_state;
    input.hudless_state = input_state;
    input.depth_state = input_state;
    input.motion_state = input_state;
    input.ui_state = input_state;
    input.depth = PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_Depth);
    input.motion = PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_MVecs);
    if (GetUInt(parameters, NVSDK_NGX_DLSSG_Parameter_NotRenderingGameFrames) != 0) {
      input.depth = nullptr;
      input.motion = nullptr;
    }
    input.ui = PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_UIAlpha);
    if (input.ui == nullptr) input.ui = PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_UI);
    input.color_width = static_cast<uint32_t>(desc.Width);
    input.color_height = desc.Height;
    if (input.motion != nullptr && input.depth != nullptr) {
      input.guide_width = GetUInt(parameters, "DLSSG.MVecsSubrectWidth", static_cast<uint32_t>(input.motion->GetDesc().Width));
      input.guide_height = GetUInt(parameters, "DLSSG.MVecsSubrectHeight", input.motion->GetDesc().Height);
    }
    input.motion_x = GetUInt(parameters, "DLSSG.MVecsSubrectBaseX");
    input.motion_y = GetUInt(parameters, "DLSSG.MVecsSubrectBaseY");
    input.depth_x = GetUInt(parameters, "DLSSG.DepthSubrectBaseX");
    input.depth_y = GetUInt(parameters, "DLSSG.DepthSubrectBaseY");
    // FG's declared factors already convert motion to pixels. Only the
    // explicit normalized override applies another resource-size factor.
    const bool normalized_override = present_mvec_units.load() == 1;
    input.motion_scale_x = GetFloat(parameters, NVSDK_NGX_DLSSG_Parameter_MvecScaleX, 1.f)
        * (normalized_override ? static_cast<float>(input.guide_width) : 1.f);
    input.motion_scale_y = GetFloat(parameters, NVSDK_NGX_DLSSG_Parameter_MvecScaleY, 1.f)
        * (normalized_override ? static_cast<float>(input.guide_height) : 1.f);
    input.init_flags = (GetUInt(parameters, NVSDK_NGX_DLSSG_Parameter_DepthInverted) != 0
        ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0)
        | (GetUInt(parameters, NVSDK_NGX_DLSSG_Parameter_MvecJittered) != 0
            ? NVSDK_NGX_DLSS_Feature_Flags_MVJittered : 0)
        | (input.guide_width < input.color_width ? NVSDK_NGX_DLSS_Feature_Flags_MVLowRes : 0);
    input.reset = GetUInt(parameters, NVSDK_NGX_DLSSG_Parameter_Reset) != 0;
    input.jitter_x = GetFloat(parameters, "DLSSG.JitterOffsetX");
    input.jitter_y = GetFloat(parameters, "DLSSG.JitterOffsetY");
    input.application_frame = ++stream.application_frame;
    // A non-UIR integration can supply an opaque scene as its UI texture;
    // the texture's presence alone does not justify one-model composition.
    const auto uir = native_fg_ui_recomposition.find(handle);
    const bool ui_recomposition = uir != native_fg_ui_recomposition.end() && uir->second;
    diagnostic.reason = "private_surfaces_unavailable";
    if (!fg_service::PrepareSurfaces(&stream, slot, input,
            fg_service::NeedsNativeColorCoupling(input, parameters, ui_recomposition))) return nullptr;
    if (resolution_pair) {
      // Cover feature-only and partial warm-up recordings as well as a final
      // image. Discarding any such recording invalidates its GPU state ledger.
      stream.native_resolution_pair = true;
      stream.native_pair_producer_marker = slot->original_backbuffer;
      stream.native_pair_recording = submission::RecordingIdentity(list);
      stream.native_pair_completion = {};
      stream.native_pair_completion_value = 0;
      stream.native_pair_discarded = false;
      stream.native_pair_replayed = false;
      submission::TrackUse(list, stream.native_pair_producer_marker);
    }
    bool output_ready = false;
    std::array<NrRecording, 2> recordings;
    PresentTarget target = owner->prefg_target;
    target.native_prefg_resolution_pair = resolution_pair;
    const auto result = fg_service::Record(&stream, slot, list, input, target,
                                          &output_ready, &recordings, parameters, ui_recomposition);
    diagnostic.recordings = recordings;
    // These commands belong to the game's list and cannot be rolled back.
    // Private colors preserve both game surfaces on any partial failure;
    // discontinuous model history is rebuilt before the next application frame.
    envelope.MarkInjectionCommandsRecorded();
    if (resolution_pair && (configuration != configuration_generation.load()
        || options_generation != native_fg_options_generation.load()
        || display_epoch != history_reset_epoch.load()
        || !experimental_native_prefg_resolution.load()
        || !same_resolutions(pair_resolutions, std::array{CurrentPassResolution(0), CurrentPassResolution(1)}))) {
      fg_service::Invalidate(&stream);
      diagnostic.reason = "configuration_changed_during_record";
      return nullptr;
    }
    if (!output_ready) {
      diagnostic.reason = (result == fg::Result::PREPARED ? "nr_group_warming_or_incomplete" : "nr_group_failed");
      if (result != fg::Result::PREPARED) fg_service::Invalidate(&stream);
      for (uint32_t i = 0; i < recordings.size(); ++i) {
        if (recordings[i].model_invoked) ResetPresentStreamHistory(&stream.roles[i]);
      }
      stream.result = result;
      display_recording.result = result;
      return nullptr;
    }
    // MFG lists share this application image read-only. Save raw B once so
    // their recordings cannot overwrite a snapshot used by another queue.
    fg_service::CopyColor(list, slot->original_backbuffer, input_state, backbuffer, input_state);
    stream.cached = slot;
    stream.native_resolution_pair = resolution_pair;
    stream.native_pair_recording = submission::RecordingIdentity(list);
    stream.native_pair_resolutions = pair_resolutions;
    stream.native_pair_completion = {};
    stream.native_pair_completion_value = 0;
    stream.native_pair_discarded = false;
    stream.native_pair_replayed = false;
    stream.source_backbuffer = backbuffer;
    stream.source_hudless = hudless;
    stream.source_ui = ui;
    stream.source_frame_id = frame_id;
    stream.source_sdk_decision = (native_sdk_present_input.identity == owner->native_output_identity
        ? native_sdk_present_input.decision_id : 0);
    stream.result = fg::Result::PREPARED;
    stream.native_serial = native_fg_recording_serial.fetch_add(1, std::memory_order_acq_rel) + 1;
    stream.native_display_epoch = display_epoch;
    native_fg_marker.store(slot->color[0], std::memory_order_release);
  }
  native_fg_display_epoch.store(stream.native_display_epoch, std::memory_order_relaxed);
  native_fg_continuing_submission.store(stream.continuing_submission, std::memory_order_release);
  native_fg_result.store(fg::Result::PREPARED, std::memory_order_release);
  fg_service::CopyColor(list, backbuffer, input_state, stream.cached->color[0],
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  if (backbuffer_replaced != nullptr) {
    *backbuffer_replaced = true;
  }
  envelope.MarkInjectionCommandsRecorded();
  if (hudless != nullptr && hudless != backbuffer) {
    auto* replacement = (stream.cached->color_inputs_prepared
        ? stream.cached->coupled_hudless : stream.cached->color[1]);
    submission::TrackUse(list, replacement);
    Transition(list, replacement, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, input_state);
    NVSDK_NGX_Parameter_SetVoidPointer(const_cast<NVSDK_NGX_Parameter*>(parameters),
        NVSDK_NGX_DLSSG_Parameter_HUDLess, replacement);
  }
  if (stream.cached->color_inputs_prepared) {
    submission::TrackUse(list, stream.cached->coupled_ui);
    NVSDK_NGX_Parameter_SetVoidPointer(const_cast<NVSDK_NGX_Parameter*>(parameters),
                                      NVSDK_NGX_DLSSG_Parameter_UI, stream.cached->coupled_ui);
  }
  display_recording.prepared = true;
  diagnostic.reason = (repeated_frame || index > 1 ? "mfg_cached_output_recorded" : "group_output_recorded");
  return (hudless != backbuffer ? hudless : nullptr);
}

inline void RestoreNativePreFg(ID3D12GraphicsCommandList* list,
                               const NVSDK_NGX_Parameter* parameters, ID3D12Resource* hudless, bool succeeded,
                               const NVSDK_NGX_Handle* handle, bool backbuffer_replaced) {
  if (InsideDirectCall() || parameters == nullptr) return;
  RuntimeLock lock(runtime_mutex);
  auto context = reinterpret_cast<uintptr_t>(parameters);
  if (handle != nullptr) {
    const auto binding = fg_service::native_bindings.find(handle);
    if (binding == fg_service::native_bindings.end()) return;
    context = binding->second;
  }
  const auto it = fg_service::streams.find({true, context});
  if (it == fg_service::streams.end()) return;
  auto& stream = it->second;
  // Evaluate returning does not end the SDK's real-frame consumption of B.
  // Keep successful NR pixels; rollback only this invocation's failed work.
  if (!succeeded && backbuffer_replaced && list != nullptr && stream.cached != nullptr
      && stream.cached->original_backbuffer != nullptr && stream.source_backbuffer != nullptr) {
    constexpr auto input_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    fg_service::CopyColor(list, stream.source_backbuffer, input_state,
                          stream.cached->original_backbuffer, input_state);
  }
  if (succeeded && backbuffer_replaced && stream.cached != nullptr && handle != nullptr) {
    const auto* owner = reinterpret_cast<const PresentStream*>(context);
    if (native_sdk_present_input.identity != nullptr
        && native_sdk_present_input.identity == owner->native_output_identity) {
      native_sdk_present_input = {.identity = native_sdk_present_input.identity,
          .decision_id = native_sdk_present_input.decision_id,
          .owner = owner, .route_id = owner->native_route_id,
          .backbuffer = stream.source_backbuffer, .marker = stream.cached->color[0], .serial = stream.native_serial,
          .guides_unavailable = native_sdk_present_input.guides_unavailable};
    }
  }
  if (hudless != nullptr && stream.cached != nullptr) {
    NVSDK_NGX_Parameter_SetVoidPointer(const_cast<NVSDK_NGX_Parameter*>(parameters),
                                      NVSDK_NGX_DLSSG_Parameter_HUDLess, hudless);
    if (stream.cached->color_inputs_prepared) {
      NVSDK_NGX_Parameter_SetVoidPointer(const_cast<NVSDK_NGX_Parameter*>(parameters),
                                        NVSDK_NGX_DLSSG_Parameter_UI, stream.source_ui);
    }
  }
  if (!succeeded) {
    if (native_sdk_present_input.owner == reinterpret_cast<const PresentStream*>(context)) {
      const auto input = native_sdk_present_input;
      native_sdk_present_input = {.identity = input.identity, .decision_id = input.decision_id,
          .owner = (input.guides_unavailable ? input.owner : nullptr),
          .route_id = (input.guides_unavailable ? input.route_id : 0),
          .backbuffer = (input.guides_unavailable ? input.backbuffer : nullptr),
          .guides_unavailable = input.guides_unavailable};
    }
    native_fg_continuing_submission.store(false, std::memory_order_release);
    fg_service::Invalidate(&stream);
    stream.result = fg::Result::FAILED;
    native_fg_marker.store(nullptr, std::memory_order_release);
    native_fg_result.store(fg::Result::FAILED, std::memory_order_release);
  }
}
