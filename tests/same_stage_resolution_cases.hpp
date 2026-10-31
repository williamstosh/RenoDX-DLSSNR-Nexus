/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <vector>

// Included after stage_execution_cases.hpp and the native GPU/model fixture.
namespace same_stage_resolution_cases {
namespace addon = renodx::addons::dlss5;
using stage_execution_cases::Capture;
using stage_execution_cases::Snapshot;

struct Evaluation {
  const NVSDK_NGX_Handle* handle = nullptr;
  uint32_t generation = 0;
  int reset = -1;
  uint32_t preset = 0;
  float intensity = 0.f;
  ID3D12Resource* motion = nullptr;
  ID3D12Resource* depth = nullptr;
  float motion_scale_x = 0.f;
  float motion_scale_y = 0.f;
  float jitter_x = 0.f;
  float jitter_y = 0.f;
  Snapshot input;
  Snapshot output;
  std::vector<float> look_strengths;
  std::vector<std::pair<float, float>> result_strengths;
};
inline std::vector<Evaluation> evaluations;
inline std::array<uint16_t, 2> stamps{0x3400, 0x3600};  // FP16 .25 / .375, in the model's encoded domain.
inline bool inside_model = false;
inline uint64_t production_texture_copies = 0;
inline uint64_t production_full_stage_copies = 0;
inline uint32_t production_stage_extent = 16;

void STDMETHODCALLTYPE ObserveProductionCopies(ID3D12GraphicsCommandList* list,
    const D3D12_TEXTURE_COPY_LOCATION* destination, UINT x, UINT y, UINT z,
    const D3D12_TEXTURE_COPY_LOCATION* source, const D3D12_BOX* box) {
  if (!inside_model && model_gpu != nullptr && list == model_gpu->list.Get()) {
    const auto from = source->pResource->GetDesc();
    const auto to = destination->pResource->GetDesc();
    if (from.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && to.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
      ++production_texture_copies;
      if (from.Width == production_stage_extent && from.Height == production_stage_extent
          && to.Width == production_stage_extent && to.Height == production_stage_extent) {
        ++production_full_stage_copies;
      }
    }
  }
  ObserveCopies(list, destination, x, y, z, source, box);
}

struct ModelObservationScope {
  ModelObservationScope() { inside_model = true; }
  ~ModelObservationScope() { inside_model = false; }
};

void STDMETHODCALLTYPE ObservePairConstants(ID3D12GraphicsCommandList* list, UINT index, UINT count,
                                             const void* data, UINT offset) {
  if (list == model_gpu->list.Get() && index == 2 && offset == 0 && !evaluations.empty()) {
    if (count == sizeof(addon::look::Constants) / 4) {
      addon::look::Constants constants;
      std::memcpy(&constants, data, sizeof(constants));
      evaluations.back().look_strengths.push_back(constants.strength);
    } else if (count == sizeof(native::V6CodecConstants) / 4) {
      native::V6CodecConstants constants;
      std::memcpy(&constants, data, sizeof(constants));
      evaluations.back().result_strengths.emplace_back(constants.transfer_strength, constants.color_strength);
    }
  }
  stage_execution_cases::real_constants(list, index, count, data, offset);
}

struct PairConstantsObservation {
  bool active = false;
  PairConstantsObservation(Gpu& gpu, bool enabled) : active(enabled) {
    if (!active) return;
    stage_execution_cases::real_constants = reinterpret_cast<stage_execution_cases::ConstantsFn>(
        (*reinterpret_cast<void***>(gpu.list.Get()))[35]);
    Check(DetourTransactionBegin() == NO_ERROR && DetourUpdateThread(GetCurrentThread()) == NO_ERROR,
          "model-pair constants observer transaction");
    Check(DetourAttach(reinterpret_cast<PVOID*>(&stage_execution_cases::real_constants),
              reinterpret_cast<PVOID>(ObservePairConstants)) == NO_ERROR
              && DetourTransactionCommit() == NO_ERROR, "model-pair constants observer attached");
  }
  ~PairConstantsObservation() {
    if (!active) return;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(reinterpret_cast<PVOID*>(&stage_execution_cases::real_constants),
                 reinterpret_cast<PVOID>(ObservePairConstants));
    DetourTransactionCommit();
  }
};

struct CopyObservation {
  explicit CopyObservation(Gpu& gpu) {
    real_copy = reinterpret_cast<CopyFn>((*reinterpret_cast<void***>(gpu.list.Get()))[16]);
    Check(DetourTransactionBegin() == NO_ERROR && DetourUpdateThread(GetCurrentThread()) == NO_ERROR,
          "same-stage copy observer transaction");
    Check(DetourAttach(reinterpret_cast<PVOID*>(&real_copy), reinterpret_cast<PVOID>(ObserveProductionCopies)) == NO_ERROR
              && DetourTransactionCommit() == NO_ERROR, "same-stage copy observer attached");
  }
  ~CopyObservation() {
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(reinterpret_cast<PVOID*>(&real_copy), reinterpret_cast<PVOID>(ObserveProductionCopies));
    DetourTransactionCommit();
  }
};

NVSDK_NGX_Result NVSDK_CONV Evaluate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
                                   const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback) {
  ModelObservationScope model_observation;
  auto* input = native::GetD3D12Resource(parameters, "DLSSNR.Color");
  Check(input != nullptr && evaluations.size() < 2, "same-stage model receives at most two real inputs");
  const auto pass = evaluations.size();
  evaluations.push_back({.handle = handle, .generation = *reinterpret_cast<const uint32_t*>(handle),
      .reset = native::GetInt(parameters, "DLSSNR.Reset", -1),
      .preset = stage_execution_cases::presets.at(handle),
      .intensity = native::GetFloat(parameters, "DLSSNR.Intensity"),
      .motion = native::GetD3D12Resource(parameters, "DLSSNR.MVec"),
      .depth = native::GetD3D12Resource(parameters, "DLSSNR.Depth"),
      .motion_scale_x = native::GetFloat(parameters, "DLSSNR.MVecScaleX"),
      .motion_scale_y = native::GetFloat(parameters, "DLSSNR.MVecScaleY"),
      .jitter_x = native::GetFloat(parameters, "DLSSNR.JitterOffsetX"),
      .jitter_y = native::GetFloat(parameters, "DLSSNR.JitterOffsetY"),
      .input = Capture(*model_gpu, input, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)});
  const auto result = EvaluateModel(list, handle, parameters, callback);
  if (NVSDK_NGX_FAILED(result)) return result;
  auto* output = native::GetD3D12Resource(parameters, "DLSSNR.Output");
  const auto desc = output->GetDesc();
  Check(desc.Width == input->GetDesc().Width && desc.Height == input->GetDesc().Height
            && desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT,
        "same-stage model has an actual 1:1 FP16 working contract");
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  uint64_t bytes = 0;
  model_gpu->device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  auto upload = model_gpu->Buffer(bytes, D3D12_HEAP_TYPE_UPLOAD);
  uint8_t* mapped = nullptr;
  Check(SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))), "same-stage stamp upload");
  std::memset(mapped, 0, static_cast<size_t>(bytes));
  const uint16_t rgba[]{stamps[pass], stamps[pass], stamps[pass], 0x3c00};
  for (uint32_t y = 0; y < desc.Height; ++y) {
    for (uint32_t x = 0; x < desc.Width; ++x) {
      std::memcpy(mapped + footprint.Offset + y * footprint.Footprint.RowPitch + x * sizeof(rgba), rgba, sizeof(rgba));
    }
  }
  upload->Unmap(0, nullptr);
  model_gpu->Transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
  const D3D12_TEXTURE_COPY_LOCATION from{.pResource = upload.Get(),
      .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = footprint};
  const D3D12_TEXTURE_COPY_LOCATION to{.pResource = output, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  model_gpu->Transition(output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  model_gpu->uploads.push_back(upload);
  evaluations.back().output = Capture(*model_gpu, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  return result;
}

int Run(bool hardware, bool model_pair_only = false, bool render_stage = false) {
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
    native::hook_point.store(render_stage ? native::kHookRender : native::kHookUpscaled);
    production_stage_extent = 16;
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
    for (uint32_t pass = 0; pass < 3; ++pass) {
      native::pass_stage[pass].store(0);
      native::pass_follow[pass].store(false);
      native::pass_intensity[pass].store(.75f);
      native::pass_transfer_strength[pass].store(1.f);
      native::pass_color_strength[pass].store(1.f);
      native::pass_look_result[pass].follow.store(true);
      native::pass_resolution_mode[pass].store(0);
      native::pass_resolution_scale[pass].store(1.f);
    }
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
    Check(native::InstallQueueCompletionHooks(gpu.queue.Get()), "same-stage queue hooks");
    Check(native::InstallCommandListStateHooks(gpu.list.Get()), "same-stage command-list hooks");
    gpu.Submit();
    CopyObservation copy_observation(gpu);
    PairConstantsObservation pair_constants_observation(gpu, model_pair_only);

    const auto texture = [&](uint32_t size, DXGI_FORMAT format, D3D12_RESOURCE_STATES state, bool uav) {
      const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
      const D3D12_RESOURCE_DESC desc{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
          .Width = size, .Height = size, .DepthOrArraySize = 1, .MipLevels = 1, .Format = format,
          .SampleDesc = {1, 0}, .Flags = (uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE)};
      ComPtr<ID3D12Resource> resource;
      Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
          state, nullptr, IID_PPV_ARGS(&resource))), "same-stage texture");
      return resource;
    };
    const auto upload_color = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES state, bool hdr) {
      const auto desc = resource->GetDesc();
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
      uint64_t bytes = 0;
      gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
      auto upload = gpu.Buffer(bytes, D3D12_HEAP_TYPE_UPLOAD);
      uint8_t* mapped = nullptr;
      Check(SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))), "same-stage input upload");
      std::memset(mapped, 0, static_cast<size_t>(bytes));
      constexpr uint16_t rgba[]{0x3800, 0x3800, 0x3800, 0x3c00};
      for (uint32_t y = 0; y < desc.Height; ++y) {
        for (uint32_t x = 0; x < desc.Width; ++x) {
          auto* pixel = mapped + footprint.Offset + y * footprint.Footprint.RowPitch + x * (hdr ? 8u : 4u);
          if (hdr) {
            std::memcpy(pixel, rgba, sizeof(rgba));
          } else {
            std::memset(pixel, 96, 3);
            pixel[3] = 255;
          }
        }
      }
      upload->Unmap(0, nullptr);
      gpu.Transition(resource, state, D3D12_RESOURCE_STATE_COPY_DEST);
      const D3D12_TEXTURE_COPY_LOCATION from{.pResource = upload.Get(),
          .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = footprint};
      const D3D12_TEXTURE_COPY_LOCATION to{.pResource = resource, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
      gpu.Transition(resource, D3D12_RESOURCE_STATE_COPY_DEST, state);
      gpu.uploads.push_back(upload);
    };
    const auto mean_rgb = [](const Snapshot& snapshot) {
      const auto pixels = snapshot.Pixels();
      std::vector<float> row(snapshot.width * 4);
      double sum = 0.;
      for (uint32_t y = 0; y < snapshot.height; ++y) {
        Check(addon::screenshot::internal::DecodeDiagnosticRow(
            pixels.data() + static_cast<size_t>(y) * snapshot.width * snapshot.pixel_bytes,
            snapshot.width, snapshot.format, row.data()), "same-stage readback decodes");
        for (uint32_t x = 0; x < snapshot.width; ++x) {
          for (uint32_t c = 0; c < 3; ++c) {
            Check(std::isfinite(row[x * 4 + c]), "same-stage every GPU pixel is finite");
            sum += row[x * 4 + c];
          }
        }
      }
      return static_cast<float>(sum / (snapshot.width * snapshot.height * 3));
    };
    const uint32_t guide_size = (render_stage ? 16u : 8u);
    auto depth = texture(guide_size, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_STATE_COMMON, false);
    auto motion = texture(guide_size, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_STATE_COMMON, false);
    gpu.Write(depth.Get(), 0);
    gpu.Write(motion.Get(), 0);
    gpu.Transition(depth.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
    gpu.Transition(motion.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
    gpu.Submit();
    uint32_t completed = 0;
    for (const bool hdr : {false, true}) {
      if (model_pair_only && !hdr) continue;
      // Two split directions and the unchanged inherited AA/AB execution path.
      for (uint32_t scenario = 0; scenario < 4; ++scenario) {
        const auto format = (hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM);
        const uint32_t render_size = (render_stage ? 16u : 8u);
        const uint32_t output_size = (render_stage ? 32u : 16u);
        auto source = texture(render_size, format, NATIVE_INPUT_STATE, false);
        auto output = texture(output_size, format, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        native::codec_mode.store(hdr ? 1u : 0u);
        native::preset.store(1);
        native::pass_preset[0].store(scenario == 2 ? 1u : 2u);
        native::pass_resolution_mode[0].store(scenario >= 2 ? 0u : 2u);
        if (model_pair_only) {
          native::look_mode.store(true);
          native::look_strength.store(.5f);
          native::look_stabilize.store(0);
          native::pass_look_result[0].follow.store(false);
          native::pass_look_result[0].mode.store(true);
          native::pass_look_result[0].strength.store(.75f);
          native::pass_look_result[0].stabilize.store(0);
          native::pass_look_result[0].detail_stability.store(1);
          native::transfer_strength.store(.25f);
          native::color_strength.store(.5f);
          native::pass_transfer_strength[0].store(.75f);
          native::pass_color_strength[0].store(.625f);
        }
        addon::bridge::OwnedParameters parameters;
        parameters.Set(NVSDK_NGX_Parameter_Color, source.Get());
        parameters.Set(NVSDK_NGX_Parameter_Output, output.Get());
        parameters.Set(NVSDK_NGX_Parameter_Depth, depth.Get());
        parameters.Set(NVSDK_NGX_Parameter_MotionVectors, motion.Get());
        parameters.Set(NVSDK_NGX_Parameter_Width, render_size);
        parameters.Set(NVSDK_NGX_Parameter_Height, render_size);
        parameters.Set(NVSDK_NGX_Parameter_OutWidth, output_size);
        parameters.Set(NVSDK_NGX_Parameter_OutHeight, output_size);
        parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, render_size);
        parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, render_size);
        parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
            static_cast<uint32_t>(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes
                | (hdr ? NVSDK_NGX_DLSS_Feature_Flags_IsHDR : 0)));
        parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
        parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
        parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_X, .25f);
        parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, -.5f);
        parameters.Set(NVSDK_NGX_Parameter_Reset, 0u);
        const auto* handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0x5F000} + completed++);
        native::RegisterCreatedFeatureLocked(handle, native::kFeatureDlss, &parameters);
        const native::WorksetKey workset_key{handle, (render_stage ? source.Get() : output.Get()), render_stage};
        std::array<uint32_t, 2> resident{};
        std::array<uint32_t, 2> previous_sizes{};
        constexpr std::array<std::array<float, 2>, 7> transitions{{
            {1.f, .67f}, {1.f, .5f}, {1.f, 1.f}, {1.f, .67f}, {.67f, .67f}, {.5f, .67f}, {1.f, .67f}}};
        const uint32_t phases = (scenario == 0 ? static_cast<uint32_t>(transitions.size()) : 1u);
        for (uint32_t phase = 0; phase < phases; ++phase) {
          const bool check_live_parent_retirement = scenario == 0 && phase == 1;
          uint32_t old_tail_id = 0;
          uint32_t parent_id = 0;
          const void* old_tail_heap = nullptr;
          const void* old_tail_proxy = nullptr;
          ID3D12Resource* shared_original = nullptr;
          if (check_live_parent_retirement) {
            const auto work = native::worksets.find(workset_key);
            Check(work != native::worksets.end() && work->second.resolution_tail != nullptr,
                  "Pass 2-only retirement counterexample starts with a live tail");
            parent_id = work->second.id;
            old_tail_id = work->second.resolution_tail->id;
            old_tail_heap = work->second.resolution_tail->descriptors;
            old_tail_proxy = work->second.resolution_tail->proxy;
            shared_original = work->second.original;
          }
          const auto scales = (scenario == 0 ? transitions[phase]
              : (scenario == 1 ? std::array{.67f, 1.f} : std::array{1.f, 1.f}));
          native::nr_resolution_scale.store(scales[0]);
          native::pass_resolution_scale[0].store(scales[1]);
          const std::array<uint32_t, 2> sizes{
              static_cast<uint32_t>(std::lround(production_stage_extent * scales[0])),
              static_cast<uint32_t>(std::lround(production_stage_extent * scales[1]))};
          const auto timer_start = addon::gpu_timers::measured_total_us;
          const auto samples_start = addon::gpu_timers::published_segments;
          const auto copies_start = recorded_copies;
          bool contract_seen = false;
          std::array<bool, 2> reset_seen{};
          uint32_t steady = 0;
          uint64_t frame_texture_copies = 0;
          uint64_t frame_full_stage_copies = 0;
          Snapshot original_before, original_after, sr_input, sr_output;
          bool sr_swapped = false;
          const auto validate_delivery = [&] {
            Check(original_before.Pixels() == original_after.Pixels(),
                  "PreSR never changes the original game Color bytes");
            if (!sr_swapped) return;
            Check(sr_input.width == render_size && sr_input.height == render_size
                      && sr_output.width == output_size && sr_output.height == output_size,
                  "PreSR delivers the full Render extent without changing the game's SR dimensions");
            const auto input_pixels = sr_input.Pixels();
            const auto output_pixels = sr_output.Pixels();
            for (uint32_t y = 0; y < output_size; ++y) {
              for (uint32_t x = 0; x < output_size; ++x) {
                Check(std::memcmp(output_pixels.data() + (y * output_size + x) * sr_output.pixel_bytes,
                          input_pixels.data() + ((y % render_size) * render_size + x % render_size) * sr_input.pixel_bytes,
                          sr_input.pixel_bytes) == 0, "GPU SR surrogate consumes the actual swapped Color pixel-for-pixel");
              }
            }
          };
          const auto record = [&] {
            evaluations.clear();
            upload_color(source.Get(), NATIVE_INPUT_STATE, hdr);
            upload_color(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, hdr);
            if (render_stage) original_before = Capture(gpu, source.Get(), NATIVE_INPUT_STATE);
            const auto texture_start = production_texture_copies;
            const auto full_stage_start = production_full_stage_copies;
            const addon::EvaluateChainScope chain;
            if (render_stage) {
              native::PreSrSwap swap;
              native::ProcessInlinePreSR(gpu.list.Get(), handle, &parameters, swap);
              frame_texture_copies = production_texture_copies - texture_start;
              frame_full_stage_copies = production_full_stage_copies - full_stage_start;
              auto* consumed = native::GetD3D12Resource(&parameters, NVSDK_NGX_Parameter_Color);
              sr_swapped = swap.result != nullptr;
              const auto input_state = (sr_swapped ? native::kPreSrReadable : NATIVE_INPUT_STATE);
              if (sr_swapped) {
                Check(consumed == swap.result && consumed != source.Get() && swap.color == source.Get(),
                      "SR surrogate uses the parameter block's actual replacement Color");
                sr_input = Capture(gpu, consumed, input_state);
              }
              gpu.Transition(consumed, input_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
              gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
              const D3D12_TEXTURE_COPY_LOCATION from{.pResource = consumed,
                  .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
              const D3D12_TEXTURE_COPY_LOCATION to{.pResource = output.Get(),
                  .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
              // Tiled SR surrogate consumes every Render pixel; a copy does not resample.
              for (uint32_t y = 0; y < output_size; y += render_size) {
                for (uint32_t x = 0; x < output_size; x += render_size) {
                  gpu.list->CopyTextureRegion(&to, x, y, 0, &from, nullptr);
                }
              }
              gpu.Transition(consumed, D3D12_RESOURCE_STATE_COPY_SOURCE, input_state);
              gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
              native::RestorePreSrColor(&parameters, swap);
              Check(native::GetD3D12Resource(&parameters, NVSDK_NGX_Parameter_Color) == source.Get()
                        && swap.color == nullptr, "RestorePreSrColor restores the original parameter pointer");
              original_after = Capture(gpu, source.Get(), NATIVE_INPUT_STATE);
              sr_output = Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
              return sr_output;
            }
            native::ProcessInline(gpu.list.Get(), handle, &parameters);
            frame_texture_copies = production_texture_copies - texture_start;
            frame_full_stage_copies = production_full_stage_copies - full_stage_start;
            return Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          };
          for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 32 && steady < 2; ++attempt) {
            stamps = {0x3400, 0x3600};
            const auto final = record();
            gpu.Submit();
            if (render_stage) validate_delivery();
            addon::gpu_timers::Poll(true, false);
            // Keep the retired child available until the counterexample pins
            // the live parent; draining it earlier would make this test vacuous.
            if (!check_live_parent_retirement) native::DrainRetiredResources(true);
            // Pass 1 can mature and consume its Reset before Pass 2 is ready.
            for (uint32_t pass = 0; pass < evaluations.size(); ++pass) {
              if (evaluations[pass].input.width == sizes[pass]) {
                reset_seen[pass] |= evaluations[pass].reset == 1;
              }
            }
            if (evaluations.size() != 2 || evaluations[0].input.width != sizes[0]
                || evaluations[1].input.width != sizes[1]) continue;
            for (uint32_t pass = 0; pass < 2; ++pass) {
              const auto& call = evaluations[pass];
              Check(call.input.height == sizes[pass] && call.output.width == sizes[pass]
                        && call.output.height == sizes[pass], "each same-stage model really works at its configured size");
              const auto& observed = native::stage_telemetry[render_stage ? 0 : 1].pass_resolutions[pass];
              Check(observed.requested_width == sizes[pass] && observed.requested_height == sizes[pass]
                        && observed.effective_width == call.input.width && observed.effective_height == call.input.height
                        && observed.model_recorded,
                    "logical pass telemetry agrees with requested settings and the actual recorded GPU contract");
              Check(call.preset == (pass == 0 || scenario == 2 ? 1u : 2u)
                        && call.intensity == (pass == 0 ? .5f : .75f), "model and settings use logical pass ID");
              Check(call.motion == motion.Get() && call.depth == depth.Get()
                        && call.motion_scale_x == 1.f && call.motion_scale_y == 1.f
                        && call.jitter_x == .25f && call.jitter_y == -.5f,
                    "working-size changes preserve original guide resources, motion scale and jitter");
              Check(std::abs(mean_rgb(call.output) - (pass == 0 ? .25f : .375f)) < .001f,
                    "GPU model output contains this pass's distinct marker");
              reset_seen[pass] |= call.reset == 1;
              if (!contract_seen && phase > 0) {
                if (sizes[pass] == previous_sizes[pass]) {
                  Check(call.generation == resident[pass], "unchanged pass size preserves its Feature generation");
                } else {
                  Check(call.generation != resident[pass], "only changed pass size rebuilds its Feature");
                }
                if (pass == 0 && sizes[0] == previous_sizes[0]) {
                  Check(call.reset == 0, "Pass 2 resize preserves upstream History");
                }
              }
              if (contract_seen) Check(call.generation == resident[pass], "steady same-stage Feature remains resident");
              resident[pass] = call.generation;
            }
            Check(mean_rgb(final) > 0.f, "same-stage final image is nonblack");
            contract_seen = true;
            if (evaluations[0].reset == 0 && evaluations[1].reset == 0) {
              const uint64_t expected_copies = (render_stage ? 1u : 2u);
              Check(frame_texture_copies == expected_copies && frame_full_stage_copies == expected_copies,
                    "steady dual pass records only existing boundary copies with no tail handoff copy");
              ++steady;
            }
          }
          Check(steady == 2, "same-stage chain reaches two steady frames within bounded warmup/debounce");
          if (model_pair_only) {
            namespace shot = addon::screenshot;
            shot::source_mode.store(2);
            shot::stage_planes_enabled.store(false);
            Check(shot::RequestCapture() && shot::WantsEvaluatedBuffer(), "arm real ModelPair capture request");
            const auto final = record();
            const auto work = native::worksets.find(workset_key);
            Check(work != native::worksets.end() && work->second.resolution_tail != nullptr,
                  "armed ModelPair has distinct full-size root and reduced tail");
            Check(shot::internal::pending.has_value(), "production CaptureModelPair created pending readbacks");
            auto& capture = *shot::internal::pending;
            Check(capture.model_diagnostic && capture.width == 11 && capture.height == 11
                      && capture.full_width == 11 && capture.full_height == 11 && capture.ready
                      && capture.model_recording_list == gpu.list.Get()
                      && capture.meta.find("stack_pass=2\n") != std::string::npos,
                  "armed ModelPair captures tail's 11x11 proxy/output and logical last pass, not root's 16x16");
            Snapshot captured_input, captured_output;
            captured_input.readback = capture.pre_nr_readback;
            captured_output.readback = capture.nr_output_readback;
            for (auto* snapshot : {&captured_input, &captured_output}) {
              snapshot->footprint = capture.footprint;
              snapshot->width = capture.width;
              snapshot->height = capture.height;
              snapshot->format = capture.format;
              snapshot->pixel_bytes = addon::screenshot::internal::DiagnosticPixelBytes(capture.format);
            }
            gpu.Submit();
            Check(evaluations.size() == 2 && captured_input.Pixels() == evaluations[1].input.Pixels()
                      && captured_output.Pixels() == evaluations[1].output.Pixels(),
                  "real ModelPair GPU readbacks equal actual second-pass proxy and distinct neural output");
            Check(mean_rgb(final) > 0.f, "HDR independent Look/Result produces finite nonblack final output");
            for (uint32_t pass = 0; pass < 2; ++pass) {
              const auto& call = evaluations[pass];
              Check(std::find(call.look_strengths.begin(), call.look_strengths.end(), (pass == 0 ? .5f : .75f))
                        != call.look_strengths.end(), "armed HDR model-pair uses independent actual Look constants");
              Check(std::find(call.result_strengths.begin(), call.result_strengths.end(),
                        (pass == 0 ? std::pair{.25f, .5f} : std::pair{.75f, .625f})) != call.result_strengths.end(),
                    "armed HDR model-pair uses independent actual Result constants");
            }
            shot::internal::ReleaseReadbacks(capture);
            shot::internal::pending.reset();
            shot::internal::has_pending.store(false);
            shot::Disarm();
            native::ForgetSourceHandleLocked(handle);
            gpu.Submit();
            native::DrainRetiredResources(true);
            // Seed stale executed statuses, then exercise the production refusal
            // resolver directly; no GPU matrix or third model is required.
            native::pass_resolution_mode[1].store(2);
            native::pass_resolution_scale[1].store(.5f);
            for (auto& observed : native::stage_telemetry[1].pass_resolutions) {
              observed.effective_width = observed.effective_height = 16;
              observed.model_recorded = true;
            }
            uint32_t declined_width = 0, declined_height = 0;
            Check(!native::ComputeNrGroupWorkingResolution(
                      {.stage = addon::stage_plan::Stage::UPSCALED, .begin = 0, .count = 3},
                      16, 16, 8, 8, &declined_width, &declined_height)
                      && native::stage_telemetry[1].resolution_blocked,
                  "unsupported three-pass different-size group explicitly blocks");
            constexpr std::array<uint32_t, 3> declined_sizes{16, 11, 8};
            for (uint32_t pass = 0; pass < 3; ++pass) {
              const auto& observed = native::stage_telemetry[render_stage ? 0 : 1].pass_resolutions[pass];
              Check(observed.requested_width == declined_sizes[pass] && observed.requested_height == declined_sizes[pass]
                        && observed.effective_width == 0 && observed.effective_height == 0 && !observed.model_recorded,
                    "declined group's every logical pass clears stale effective dimensions and recorded status");
            }
            if (info != nullptr) {
              for (UINT64 index = 0; index < info->GetNumStoredMessages(); ++index) {
                SIZE_T bytes = 0;
                info->GetMessage(index, nullptr, &bytes);
                std::vector<uint8_t> storage(bytes);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                Check(SUCCEEDED(info->GetMessage(index, message, &bytes)), "model-pair debug message");
                Check(message->Severity != D3D12_MESSAGE_SEVERITY_ERROR
                          && message->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION, message->pDescription);
              }
            }
            std::cout << "PASS same-stage-model-pair HDR A100->B67 root=16x16 captured=11x11"
                         " input=tail-proxy-GPU-equal output=tail-neural-GPU-equal"
                         " Look=0.5,0.75 Result=0.25/0.5,0.75/0.625 hardware=" << hardware
                      << " capture_buffers=released-after-GPU-completion unsupported-group3-telemetry=cleared-CPU no-export\n";
            return 0;
          }
          if (check_live_parent_retirement) {
            const auto work = native::worksets.find(workset_key);
            Check(work != native::worksets.end() && work->second.id == parent_id
                      && work->second.original == shared_original && work->second.resolution_tail != nullptr
                      && work->second.resolution_tail->id != old_tail_id,
                  "Pass 2-only resize replaces the child while preserving the parent full-stage images");
            const auto old_tail_retained = [&] {
              return std::any_of(native::retired_final_resources.begin(), native::retired_final_resources.end(),
                  [&](const auto& retired) { return retired.resources.id == old_tail_id; });
            };
            Check(old_tail_retained(), "old tail is still queued before the live-parent retirement probe");
            native::TrackWorksetRecording(gpu.list.Get(), work->second);
            Check(!submission::ResourceReleasable(shared_original)
                      && submission::ResourceReleasable(old_tail_heap)
                      && submission::ResourceReleasable(old_tail_proxy),
                  "unsubmitted parent recording pins shared images while old child heap and proxy receipts are complete");
            native::DrainRetiredResources(true);
            Check(!old_tail_retained() && !submission::ResourceReleasable(shared_original),
                  "old tail drains even while the live parent's shared full-stage images remain pinned");
            gpu.Submit();
            Check(submission::ResourceReleasable(shared_original),
                  "tracker-only parent recording releases after submission completion and Reset");
            std::cout << "PASS same-stage-resolution live-parent-retirement hdr=" << hdr
                      << " old_tail_id=" << old_tail_id << " parent_id=" << parent_id
                      << " shared_images_pinned=1 old_tail_drained=1 tracker_recording_reset=1\n";
          }
          for (uint32_t pass = 0; pass < 2; ++pass) {
            if (phase == 0 || sizes[pass] != previous_sizes[pass] || (pass == 1 && sizes[0] != previous_sizes[0])) {
              Check(reset_seen[pass], "new/changed Feature and affected downstream History reset");
            }
          }
          // Keep game input fixed, vary only the preceding model's actual GPU result.
          // A stale proxy or reread of the game image cannot satisfy this paired oracle.
          stamps = {0x3400, 0x3600};
          const auto low_final = record();
          gpu.Submit();
          if (render_stage) validate_delivery();
          Check(evaluations.size() == 2, "low marker executes both models");
          const float low_input = mean_rgb(evaluations[1].input);
          const float low_result = mean_rgb(low_final);
          stamps[0] = 0x3a00;  // .75
          const auto upstream_changed = record();
          gpu.Submit();
          if (render_stage) validate_delivery();
          Check(evaluations.size() == 2 && mean_rgb(evaluations[1].input) > low_input + .05f,
                "Pass 2 GPU input follows Pass 1's modified output across the full-stage handoff");
          Check(mean_rgb(upstream_changed) > 0.f, "upstream-change final GPU output remains valid");
          stamps = {0x3400, 0x3900};  // same first result, second result .625
          const auto downstream_changed = record();
          gpu.Submit();
          if (render_stage) validate_delivery();
          Check(evaluations.size() == 2, "downstream marker executes both models");
          const uint64_t expected_copies = (render_stage ? 1u : 2u);
          Check(frame_texture_copies == expected_copies && frame_full_stage_copies == expected_copies,
                "distinct-pass output delivery adds no production texture copy between passes");
          Check(mean_rgb(downstream_changed) > low_result + .01f,
                "final GPU output follows Pass 2's distinct result rather than a stale first-pass image");
          const auto work = native::worksets.find(workset_key);
          Check(work != native::worksets.end(), "same-stage root workset exists");
          const auto& resources = work->second;
          if (sizes[0] != sizes[1]) {
            const auto* tail = resources.resolution_tail;
            Check(tail != nullptr && tail->shared_stage_surfaces,
                  "different working sizes have a dedicated tail with shared full-stage images");
            Check(tail->proxy != resources.proxy && tail->nr_output != resources.nr_output
                      && tail->descriptors != resources.descriptors
                      && tail->input_width == sizes[1] && tail->input_height == sizes[1],
                  "tail working images and immutable descriptors do not alias the first pass");
            Check(tail->original == resources.original && tail->decoded == resources.decoded
                      && tail->decoded_alt == resources.decoded_alt && tail->work0 == resources.work0
                      && tail->work_a == resources.work_a && tail->work_b == resources.work_b
                      && (!render_stage || tail->pre_sr_color == resources.pre_sr_color),
                  "both passes use the root full-stage ping-pong and one HDR linearization");
          } else {
            Check(resources.resolution_tail == nullptr, "equal-size legacy chain needs no tail allocation");
          }
          const auto vram = addon::telemetry::QueryVideoMemory(native::direct_device);
          std::cout << "PASS same-stage-resolution hdr=" << hdr << " scenario=" << scenario << " phase=" << phase
                    << " stage=" << (render_stage ? "Render" : "Upscaled")
                    << " requested=" << sizes[0] << ',' << sizes[1]
                    << " effective=" << evaluations[0].input.width << ',' << evaluations[1].input.width
                    << " live_workset_bytes=" << resources.allocated_bytes << " dxgi_valid=" << vram.valid
                    << " dxgi_local_usage_bytes=" << vram.local_usage << " dxgi_local_budget_bytes=" << vram.local_budget
                    << " dxgi_nonlocal_usage_bytes=" << vram.nonlocal_usage
                    << " timer_segments=" << addon::gpu_timers::published_segments - samples_start
                    << " timer_total_us=" << addon::gpu_timers::measured_total_us - timer_start
                    << " timer_scope=warmup-readback-and-model-upload-standins"
                    << " copy_calls=" << recorded_copies - copies_start
                    << " copy_scope=warmup-input-upload-model-stamp-SR-surrogate-and-readbacks"
                    << " production_full_stage_copies=" << frame_full_stage_copies
                    << " production_texture_copies=" << frame_texture_copies
                    << " production_copy_scope=one-steady-frame-excluding-model-upload-readbacks"
                    << " vendor_model_memory=excluded linearize_dispatch_count=unavailable\n";
          previous_sizes = sizes;
        }

        // Use the real root/tail resources in a controlled tracker receipt;
        // no fabricated uncompleted work is submitted to the GPU queue.
        const auto work = native::worksets.find(workset_key);
        Check(work != native::worksets.end(), "same-stage retirement has a live workset");
        std::vector<const void*> protected_objects;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12Fence> receipt;
        Check(SUCCEEDED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))
                  && SUCCEEDED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                      nullptr, IID_PPV_ARGS(&list)))
                  && SUCCEEDED(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&receipt))),
              "same-stage controlled retirement fixture");
        const auto protect = [&](const native::FinalResources& resources) {
          native::TrackWorksetRecording(list.Get(), resources);
          protected_objects.push_back(resources.proxy);
          protected_objects.push_back(resources.nr_output);
          protected_objects.push_back(resources.descriptors);
        };
        protect(work->second);
        if (work->second.resolution_tail != nullptr) {
          protect(*work->second.resolution_tail);
        }
        const auto& feature = native::features.at(handle);
        const auto& session = (render_stage && feature.render_session ? *feature.render_session : feature);
        for (const auto& slot : session.slots) {
          if (slot.handle != nullptr) {
            submission::TrackUse(list.Get(), slot.handle);
            protected_objects.push_back(slot.handle);
          }
        }
        Check(SUCCEEDED(list->Close()), "same-stage close controlled recording");
        ID3D12CommandList* lists[]{list.Get()};
        const auto pending = submission::BeginSubmission(1, lists);
        Check(pending.size() == 1, "same-stage controlled submission reserves a generation");
        submission::CompleteSubmission(pending, receipt.Get(), 1);

        // An actual second-model failure must not commit the partial first result.
        evaluations.clear();
        upload_color(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, hdr);
        const auto before_failure = Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const auto color_before_failure = Capture(gpu, source.Get(), NATIVE_INPUT_STATE);
        fail_call = model_calls + 2;
        {
          const addon::EvaluateChainScope chain;
          if (render_stage) {
            native::PreSrSwap swap;
            Check(!native::ProcessInlinePreSR(gpu.list.Get(), handle, &parameters, swap)
                      && swap.result == nullptr && swap.color == nullptr,
                  "failed Render second model never swaps a partial result into SR Color");
            native::RestorePreSrColor(&parameters, swap);
            Check(native::GetD3D12Resource(&parameters, NVSDK_NGX_Parameter_Color) == source.Get(),
                  "failed PreSR keeps the original Color parameter");
          } else {
            native::ProcessInline(gpu.list.Get(), handle, &parameters);
          }
        }
        Check(model_calls == fail_call, "same-stage failure injected at the second model");
        fail_call = 0;
        const auto after_failure = Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const auto color_after_failure = Capture(gpu, source.Get(), NATIVE_INPUT_STATE);
        gpu.Submit();
        Check(color_before_failure.Pixels() == color_after_failure.Pixels(), "model failure protects original Color bytes");
        Check(before_failure.Pixels() == after_failure.Pixels(), "failed second pass preserves the game output byte-for-byte");
        native::ForgetSourceHandleLocked(handle);
        const auto expect_protected = [&] {
          native::DrainRetiredResources(true);
          std::vector<const void*> retained_objects;
          for (const auto& retired : native::retired_final_resources) {
            const auto& objects = native::CollectWorksetGpuObjects(retired.resources);
            retained_objects.insert(retained_objects.end(), objects.begin(), objects.end());
          }
          for (const auto& retired : native::retired_nr_features) {
            retained_objects.push_back(retired.handle);
          }
          for (const auto object : protected_objects) {
            Check(!submission::ResourceReleasable(object), "both pass work/descriptor/model objects retain outstanding proof");
            Check(std::find(retained_objects.begin(), retained_objects.end(), object) != retained_objects.end(),
                  "production retirement queues actually retain protected work/descriptor/model objects");
          }
        };
        expect_protected();
        Check(SUCCEEDED(receipt->Signal(1)), "same-stage complete controlled receipt");
        expect_protected();  // A completed but replayable recording still owns resources.
        const auto replay = submission::BeginSubmission(1, lists);
        Check(replay.size() == 1, "same-stage controlled replay reserves a generation");
        submission::CompleteSubmission(replay, receipt.Get(), 2);
        Check(SUCCEEDED(list->Reset(allocator.Get(), nullptr)), "same-stage discard controlled recording");
        submission::OnCommandListReset(list.Get());
        Check(SUCCEEDED(list->Close()), "same-stage close discarded recording");
        expect_protected();  // Reset cannot release an unfinished replay.
        Check(SUCCEEDED(receipt->Signal(2)), "same-stage complete controlled replay");
        native::DrainRetiredResources(true);
        for (const auto object : protected_objects) {
          Check(submission::ResourceReleasable(object), "same-stage resources become releasable after receipt and recording close");
        }
        submission::OnCommandListDestroyed(list.Get());
        std::cout << "PASS same-stage-resolution failure-and-retirement hdr=" << hdr << " scenario=" << scenario
                  << " protected_objects=" << protected_objects.size() << " proof=controlled-recording-receipt-and-replay\n";
      }
    }
    if (info != nullptr) {
      for (UINT64 index = 0; index < info->GetNumStoredMessages(); ++index) {
        SIZE_T bytes = 0;
        info->GetMessage(index, nullptr, &bytes);
        std::vector<uint8_t> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        Check(SUCCEEDED(info->GetMessage(index, message, &bytes)), "same-stage debug message");
        Check(message->Severity != D3D12_MESSAGE_SEVERITY_ERROR
                  && message->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION, message->pDescription);
      }
    }
    std::cout << "PASS same-stage-resolution stage=" << (render_stage ? "Render" : "Upscaled")
              << " cases=" << completed << " hardware=" << hardware
              << " NVIDIA-NR=substituted no-game-or-performance-claim\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL same-stage-resolution: " << error.what() << '\n';
    return 1;
  }
}
}  // namespace same_stage_resolution_cases
