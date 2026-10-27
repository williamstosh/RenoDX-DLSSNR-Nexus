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
#include <map>
#include <vector>

// Included by fg_native_tests.cpp after its WARP host and model stand-ins.
// This case uses production NR scheduling/shaders and a GPU copy SR consumer;
// neither the model stand-in nor that consumer implements NVIDIA SR/NR.
namespace stage_execution_cases {
namespace addon = renodx::addons::dlss5;

struct Snapshot {
  ComPtr<ID3D12Resource> readback;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t pixel_bytes = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

  std::vector<uint8_t> Pixels() const {
    uint8_t* mapped = nullptr;
    Check(SUCCEEDED(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped))), "stage readback map");
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * pixel_bytes);
    for (uint32_t y = 0; y < height; ++y) {
      std::memcpy(pixels.data() + static_cast<size_t>(y) * width * pixel_bytes,
                  mapped + footprint.Offset + static_cast<size_t>(y) * footprint.Footprint.RowPitch,
                  static_cast<size_t>(width) * pixel_bytes);
    }
    readback->Unmap(0, nullptr);
    return pixels;
  }
};

Snapshot Capture(Gpu& gpu, ID3D12Resource* texture, D3D12_RESOURCE_STATES resting_state) {
  const auto desc = texture->GetDesc();
  Snapshot snapshot;
  snapshot.width = static_cast<uint32_t>(desc.Width);
  snapshot.height = desc.Height;
  snapshot.pixel_bytes = addon::screenshot::internal::DiagnosticPixelBytes(desc.Format);
  snapshot.format = desc.Format;
  Check(snapshot.pixel_bytes != 0, "stage capture has a supported pixel format");
  uint64_t bytes = 0;
  gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &snapshot.footprint, nullptr, nullptr, &bytes);
  snapshot.readback = gpu.Buffer(bytes, D3D12_HEAP_TYPE_READBACK);
  gpu.Transition(texture, resting_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  const D3D12_TEXTURE_COPY_LOCATION from{.pResource = texture, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  const D3D12_TEXTURE_COPY_LOCATION to{.pResource = snapshot.readback.Get(),
      .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = snapshot.footprint};
  gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  gpu.Transition(texture, D3D12_RESOURCE_STATE_COPY_SOURCE, resting_state);
  return snapshot;
}

struct Evaluation {
  const NVSDK_NGX_Handle* handle = nullptr;
  int reset = -1;
  Snapshot input;
  float intensity = 0.f;
  uint32_t style = 0;
  float tone = 0.f;
  std::vector<float> look_strengths;
  std::vector<std::pair<float, float>> result_strengths;
};
inline std::vector<Evaluation> evaluations;
inline std::map<const NVSDK_NGX_Handle*, uint32_t> presets;
using ConstantsFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, const void*, UINT);
inline ConstantsFn real_constants = nullptr;
inline bool observe_constants = false;

void STDMETHODCALLTYPE ObserveConstants(ID3D12GraphicsCommandList* list, UINT index, UINT count,
                                        const void* data, UINT offset) {
  if (observe_constants && list == model_gpu->list.Get() && index == 2 && offset == 0 && !evaluations.empty()) {
    if (count == sizeof(addon::look::Constants) / 4) {
      addon::look::Constants constants;
      std::memcpy(&constants, data, sizeof(constants));
      evaluations.back().look_strengths.push_back(constants.strength);
    } else if (count == 24) {
      // BindCodec's legacy layout: transfer and colour are dwords 11/12.
      std::array<float, 24> words;
      std::memcpy(words.data(), data, sizeof(words));
      evaluations.back().result_strengths.emplace_back(words[11], words[12]);
    }
  }
  real_constants(list, index, count, data, offset);
}

struct ConstantsObserver {
  explicit ConstantsObserver(Gpu& gpu) {
    real_constants = reinterpret_cast<ConstantsFn>((*reinterpret_cast<void***>(gpu.list.Get()))[35]);
    Check(DetourTransactionBegin() == NO_ERROR && DetourUpdateThread(GetCurrentThread()) == NO_ERROR,
          "stage constants observer transaction");
    Check(DetourAttach(reinterpret_cast<PVOID*>(&real_constants), reinterpret_cast<PVOID>(ObserveConstants)) == NO_ERROR
              && DetourTransactionCommit() == NO_ERROR, "stage constants observer attached");
    observe_constants = true;
  }
  ~ConstantsObserver() {
    observe_constants = false;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(reinterpret_cast<PVOID*>(&real_constants), reinterpret_cast<PVOID>(ObserveConstants));
    DetourTransactionCommit();
  }
};

NVSDK_NGX_Result NVSDK_CONV Create(ID3D12GraphicsCommandList* list, NVSDK_NGX_Feature feature,
                                 const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle) {
  const auto result = CreateModel(list, feature, parameters, handle);
  if (NVSDK_NGX_SUCCEED(result)) {
    presets[*handle] = native::GetUInt(parameters, "DLSSNR.Hint.Render.Preset");
  }
  return result;
}

NVSDK_NGX_Result NVSDK_CONV Evaluate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
                                   const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback) {
  auto* input = native::GetD3D12Resource(parameters, "DLSSNR.Color");
  Check(input != nullptr, "stage model receives production encoded input");
  evaluations.push_back({.handle = handle, .reset = native::GetInt(parameters, "DLSSNR.Reset", -1),
      .input = Capture(*model_gpu, input, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
      .intensity = native::GetFloat(parameters, "DLSSNR.Intensity"),
      .style = native::GetUInt(parameters, "DLSSNR.Style"),
      .tone = native::GetFloat(parameters, "DLSSNR.LocalToneStrength")});
  const auto result = EvaluateModel(list, handle, parameters, callback);
  if (NVSDK_NGX_FAILED(result)) return result;
  auto* output = native::GetD3D12Resource(parameters, "DLSSNR.Output");
  const auto input_desc = input->GetDesc();
  const auto output_desc = output->GetDesc();
  Check(input_desc.Width == output_desc.Width && input_desc.Height == output_desc.Height
            && input_desc.Format == output_desc.Format, "identity model has an equal-size encoded input/output contract");
  // Keep the existing stand-in's call/reset accounting, then replace its zero
  // output with an identity model using real GPU data rather than a CPU oracle.
  model_gpu->Transition(input, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
  model_gpu->Transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
  const D3D12_TEXTURE_COPY_LOCATION from{.pResource = input, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  const D3D12_TEXTURE_COPY_LOCATION to{.pResource = output, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  model_gpu->Transition(input, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  model_gpu->Transition(output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  return result;
}

int Run() {
  try {
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    Gpu gpu;
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
    native::detail_stability.store(1);
    native::chained_temporal_history.store(true);
    native::transfer_strength.store(1.f);
    native::color_strength.store(1.f);
    for (uint32_t pass = 0; pass < 3; ++pass) {
      native::pass_transfer_strength[pass].store(1.f);
      native::pass_color_strength[pass].store(1.f);
      native::pass_look_result[pass].follow.store(true);
    }
    native::direct_api.module = GetModuleHandleW(nullptr);
    native::direct_api.create = Create;
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
    Check(native::InstallQueueCompletionHooks(gpu.queue.Get()), "stage queue completion hooks");
    Check(native::InstallCommandListStateHooks(gpu.list.Get()), "stage command-list state hooks");
    gpu.Submit();

    const auto texture = [&](uint32_t size, DXGI_FORMAT format, bool output) {
      const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
      const D3D12_RESOURCE_DESC desc{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
          .Width = size, .Height = size, .DepthOrArraySize = 1, .MipLevels = 1, .Format = format,
          .SampleDesc = {1, 0}, .Flags = (output ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE)};
      ComPtr<ID3D12Resource> result;
      Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
          D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&result))), "stage fixture texture");
      return result;
    };
    auto color = texture(8, DXGI_FORMAT_R8G8B8A8_UNORM, false);
    auto depth = texture(8, DXGI_FORMAT_R32_FLOAT, false);
    auto motion = texture(8, DXGI_FORMAT_R16G16_FLOAT, false);
    gpu.Write(depth.Get(), 0);
    gpu.Write(motion.Get(), 0);
    for (auto* resource : {color.Get(), depth.Get(), motion.Get()}) {
      gpu.Transition(resource, D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
    }
    gpu.Submit();
    uint32_t cases = 0;
    // Dedicated boundary cases keep the SDR matrix's byte oracle unchanged.
    // First is DLAA with Color == Output; second is managed-exposure HDR.
    ConstantsObserver constants_observer(gpu);
    uint32_t boundary_cases = 0;
    uint32_t partial_warmups = 0;
    uint32_t failed_predecessors = 0;
    for (const bool hdr : {false, true}) {
      const uint32_t count = (hdr ? 2u : 4u);
      const uint32_t render_count = (hdr ? 1u : 2u);
      const auto format = (hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM);
      auto source = texture(8, format, true);
      auto output = (hdr ? texture(8, format, true) : source);
      auto exposure = texture(8, DXGI_FORMAT_R32_FLOAT, false);
      gpu.Transition(source.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
      if (hdr) gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      // Upload actual FP16 0.5 RGB / 1 alpha, or R32 exposure 0.5;
      // byte-pattern Gpu::Write is used only for the UNORM fixture.
      const auto write_float = [&](ID3D12Resource* resource, bool half) {
        const auto desc = resource->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        uint64_t bytes = 0;
        gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        auto upload = gpu.Buffer(bytes, D3D12_HEAP_TYPE_UPLOAD);
        uint8_t* mapped = nullptr;
        Check(SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))), "boundary float upload map");
        std::memset(mapped, 0, static_cast<size_t>(bytes));
        for (uint32_t y = 0; y < 8; ++y) {
          for (uint32_t x = 0; x < 8; ++x) {
            if (half) {
              constexpr uint16_t rgba[]{0x3800, 0x3800, 0x3800, 0x3c00};
              std::memcpy(mapped + footprint.Offset + y * footprint.Footprint.RowPitch + x * 8, rgba, sizeof(rgba));
            } else {
              constexpr float value = 0.5f;
              std::memcpy(mapped + footprint.Offset + y * footprint.Footprint.RowPitch + x * 4, &value, sizeof(value));
            }
          }
        }
        upload->Unmap(0, nullptr);
        gpu.Transition(resource, NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COPY_DEST);
        const D3D12_TEXTURE_COPY_LOCATION from{.pResource = upload.Get(),
            .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = footprint};
        const D3D12_TEXTURE_COPY_LOCATION to{.pResource = resource, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        gpu.Transition(resource, D3D12_RESOURCE_STATE_COPY_DEST, NATIVE_INPUT_STATE);
        gpu.uploads.push_back(upload);
      };
      gpu.Transition(exposure.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
      write_float(exposure.Get(), false);
      native::hook_point.store(native::kHookRender);
      native::stack_passes.store(count);
      native::preset.store(1);
      native::codec_mode.store(hdr ? 2u : 0u);
      native::feed_mode.store(hdr ? 2u : 0u);
      native::intensity.store(0.5f);
      native::style.store(0);
      native::local_tone_strength.store(0.125f);
      native::look_mode.store(true);
      native::look_strength.store(0.5f);
      native::look_stabilize.store(0);
      native::transfer_strength.store(0.25f);
      native::color_strength.store(0.25f);
      for (uint32_t pass = 1; pass < 4; ++pass) {
        native::pass_stage[pass - 1].store(pass == render_count ? 2u : 0u);
        native::pass_preset[pass - 1].store((pass & 1u) + 1);
        native::pass_follow[pass - 1].store(false);
        native::pass_intensity[pass - 1].store(0.5f + 0.25f * pass);
        native::pass_style[pass - 1].store(pass % 3);
        native::pass_local_tone[pass - 1].store(0.125f * (pass + 1));
        native::pass_look_result[pass - 1].follow.store(false);
        native::pass_look_result[pass - 1].mode.store(true);
        native::pass_look_result[pass - 1].strength.store(0.5f + 0.125f * pass);
        native::pass_look_result[pass - 1].stabilize.store(0);
        native::pass_look_result[pass - 1].detail_stability.store(1);
        native::pass_transfer_strength[pass - 1].store(0.25f * (pass + 1));
        native::pass_color_strength[pass - 1].store(0.25f * (pass + 1));
      }
      addon::bridge::OwnedParameters parameters;
      parameters.Set(NVSDK_NGX_Parameter_Color, source.Get());
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
          static_cast<uint32_t>(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | (hdr ? NVSDK_NGX_DLSS_Feature_Flags_IsHDR : 0)));
      parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
      parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
      parameters.Set(NVSDK_NGX_Parameter_Reset, 0u);
      if (hdr) {
        parameters.Set(NVSDK_NGX_Parameter_ExposureTexture, exposure.Get());
        parameters.Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.f);
        parameters.Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.f);
      }
      const auto* handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0x5D000} + boundary_cases);
      native::RegisterCreatedFeatureLocked(handle, native::kFeatureDlss, &parameters);
      std::array<const NVSDK_NGX_Handle*, 4> resident{};
      ID3D12Resource* render_commit = nullptr;
      ID3D12Resource* root_commit = nullptr;
      uint32_t steady = 0;
      const auto creates_before = next_handle;
      for (uint32_t attempt = 0; attempt < 20 && steady < 2; ++attempt) {
        evaluations.clear();
        if (hdr) {
          write_float(source.Get(), true);
        } else {
          gpu.Transition(source.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COMMON);
          gpu.Write(source.Get(), 96);
          gpu.Transition(source.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
        }
        {
          const addon::EvaluateChainScope outer;
          native::PreSrSwap swap;
          native::ProcessInlinePreSR(gpu.list.Get(), handle, &parameters, swap);
          const auto render_calls = evaluations.size();
          const bool complete = addon::evaluate_chain.render_complete;
          if (!hdr) gpu.Transition(output.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          if (swap.result != nullptr) {
            Check(native::GetD3D12Resource(&parameters, NVSDK_NGX_Parameter_Color) == swap.result,
                  "boundary SR consumes the swapped Color even with Color == Output");
            gpu.Transition(swap.result, native::kPreSrReadable, D3D12_RESOURCE_STATE_COPY_SOURCE);
            gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            const D3D12_TEXTURE_COPY_LOCATION from{.pResource = swap.result, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            const D3D12_TEXTURE_COPY_LOCATION to{.pResource = output.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            gpu.Transition(swap.result, D3D12_RESOURCE_STATE_COPY_SOURCE, native::kPreSrReadable);
            gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          }
          native::RestorePreSrColor(&parameters, swap);
          const auto before_post = model_calls;
          native::ProcessInline(gpu.list.Get(), handle, &parameters);
          if (!complete) {
            Check(model_calls == before_post, "partial Render warmup never evaluates an Upscaled model");
            for (const auto& slot : native::features.at(handle).slots) {
              Check(slot.pending_reset, "partial predecessor arms root history reset");
            }
            if (render_calls > 0 && render_calls < render_count) ++partial_warmups;
          }
          const auto before_mirror = model_calls;
          {
            const addon::EvaluateChainScope nested;
            native::ProcessInline(gpu.list.Get(), handle, &parameters);
          }
          Check(model_calls == before_mirror, "aliased DLAA stages each run once despite nested Evaluate");
        }
        const auto output_pixels = Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!hdr) gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NATIVE_INPUT_STATE);
        gpu.Submit();
        if (evaluations.size() != count) continue;
        auto& root = native::features.at(handle);
        Check(root.render_session != nullptr, "boundary has an independent Render session");
        bool reset = false;
        for (uint32_t pass = 0; pass < count; ++pass) {
          const auto& call = evaluations[pass];
          const auto& session = (pass < render_count ? *root.render_session : root);
          const uint32_t slot = (pass < render_count ? pass : pass - render_count);
          Check(call.handle == session.slots[slot].handle && presets.at(call.handle) == ((pass & 1u) + 1),
                "boundary model order and logical preset remain correct");
          Check(call.intensity == 0.5f + 0.25f * pass && call.style == pass % 3
                    && call.tone == 0.125f * (pass + 1), "production model parameters use logical pass, not local slot");
          Check(std::find(call.look_strengths.begin(), call.look_strengths.end(), 0.5f + 0.125f * pass)
                    != call.look_strengths.end(), "actual Look shader constants use independent logical pass strength");
          if (!hdr) {
            Check(std::find(call.result_strengths.begin(), call.result_strengths.end(),
                           std::pair{0.25f * (pass + 1), 0.25f * (pass + 1)}) != call.result_strengths.end(),
                  "actual Result shader constants use logical pass transfer and colour strengths");
          }
          if (steady != 0) Check(call.handle == resident[pass], "boundary retains every stage's resident model");
          resident[pass] = call.handle;
          reset |= call.reset != 0;
        }
        if (hdr) {
          Check(root.norm.commit != nullptr && root.render_session->norm.commit != nullptr
                    && root.norm.commit != root.render_session->norm.commit,
                "managed HDR stages own distinct nonempty normalization commit texels");
          Check(root.norm.feed == native::FeedSource::kTexture
                    && root.render_session->norm.feed == native::FeedSource::kTexture,
                "both HDR stages consume the managed exposure texture contract");
          Check(root.input_width == 8 && root.render_session->input_width == 8,
                "HDR stage dimensions remain independent and valid");
          if (steady != 0) {
            Check(root.norm.commit == root_commit && root.render_session->norm.commit == render_commit,
                  "HDR normalization texels retain identity across steady frames");
          }
          root_commit = root.norm.commit;
          render_commit = root.render_session->norm.commit;
          const auto pixels = output_pixels.Pixels();
          std::array<float, 32> row{};
          bool nonblack = false;
          for (uint32_t y = 0; y < 8; ++y) {
            Check(addon::screenshot::internal::DecodeDiagnosticRow(pixels.data() + y * 64, 8, format, row.data()),
                  "HDR boundary readback decodes");
            for (uint32_t x = 0; x < 8; ++x) {
              for (uint32_t channel = 0; channel < 3; ++channel) {
                Check(std::isfinite(row[x * 4 + channel]), "HDR output is finite without imposing an SDR equality oracle");
                nonblack |= row[x * 4 + channel] > 0.f;
              }
            }
          }
          Check(nonblack, "HDR boundary produces a nonblack GPU image");
        }
        if (!reset) ++steady;
      }
      Check(steady == 2 && next_handle == creates_before + count,
            "boundary reaches two stable full model sequences without unexpected recreation");
      if (!hdr) {
        evaluations.clear();
        fail_call = model_calls + 2;
        {
          const addon::EvaluateChainScope outer;
          native::PreSrSwap swap;
          Check(!native::ProcessInlinePreSR(gpu.list.Get(), handle, &parameters, swap)
                    && !addon::evaluate_chain.render_complete && evaluations.size() == 2,
                "second Render model failure leaves the predecessor incomplete");
          gpu.Transition(output.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          const auto before_post = model_calls;
          Check(!native::ProcessInline(gpu.list.Get(), handle, &parameters) && model_calls == before_post,
                "failed Render predecessor never runs Upscaled");
          for (const auto& slot : native::features.at(handle).slots) {
            Check(slot.pending_reset, "failed predecessor resets all following stage histories");
          }
          native::RestorePreSrColor(&parameters, swap);
        }
        fail_call = 0;
        gpu.Submit();
        ++failed_predecessors;
      }
      native::ForgetSourceHandleLocked(handle);
      gpu.Submit();
      ++boundary_cases;
      std::cout << "PASS STAGE_BOUNDARY " << (hdr ? "managed-HDR" : "Color=Output-DLAA")
                << " passes=" << count << " resident_creates=" << next_handle - creates_before
                << " model_parameters=logical-pass look_constants=logical-pass"
                << (hdr ? " norm_commits=distinct-and-retained output=finite-nonblack"
                        : " result_constants=logical-pass") << '\n';
    }
    Check(partial_warmups > 0 && failed_predecessors == 1,
          "partial warmup and model failure predecessor gates were both exercised");
    std::cout << "PASS STAGE_BOUNDARY predecessor_gates partial_warmups=" << partial_warmups
              << " failed_predecessors=" << failed_predecessors << '\n';
    native::codec_mode.store(0);
    native::feed_mode.store(0);
    native::look_mode.store(false);
    native::intensity.store(1.f);
    native::style.store(0);
    native::local_tone_strength.store(1.f);
    native::transfer_strength.store(1.f);
    native::color_strength.store(1.f);
    for (uint32_t pass = 0; pass < 3; ++pass) {
      native::pass_follow[pass].store(true);
      native::pass_look_result[pass].follow.store(true);
      native::pass_transfer_strength[pass].store(1.f);
      native::pass_color_strength[pass].store(1.f);
    }
    // Every legal two-stage grouping is one boundary: zero through count
    // Render passes followed by the remaining Upscaled passes.
    for (uint32_t count = 1; count <= 4; ++count) {
      for (uint32_t render_count = 0; render_count <= count; ++render_count) {
        for (uint32_t models = 0; models < 4; ++models) {
          for (const uint32_t output_size : {8u, 16u}) {
            // One distinct-size representative; the full matrix stays at 8x8.
            if (output_size == 16 && !(count == 2 && render_count == 1 && models == 1)) continue;
            std::cout << "STAGE_CASE passes=" << count << " render=" << render_count
                      << " models=" << models << " output=" << output_size << '\n';
            auto output = texture(output_size, DXGI_FORMAT_R8G8B8A8_UNORM, true);
            gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            native::hook_point.store(render_count == 0 ? native::kHookUpscaled : native::kHookRender);
            native::stack_passes.store(count);
            native::preset.store((models & 1u) + 1);
            for (uint32_t pass = 1; pass < 4; ++pass) {
              native::pass_stage[pass - 1].store(pass < render_count ? 0u : (pass == render_count ? 2u : 0u));
              native::pass_preset[pass - 1].store(((models >> (pass & 1u)) & 1u) + 1);
            }
            addon::bridge::OwnedParameters parameters;
            parameters.Set(NVSDK_NGX_Parameter_Color, color.Get());
            parameters.Set(NVSDK_NGX_Parameter_Output, output.Get());
            parameters.Set(NVSDK_NGX_Parameter_Depth, depth.Get());
            parameters.Set(NVSDK_NGX_Parameter_MotionVectors, motion.Get());
            parameters.Set(NVSDK_NGX_Parameter_Width, 8u);
            parameters.Set(NVSDK_NGX_Parameter_Height, 8u);
            parameters.Set(NVSDK_NGX_Parameter_OutWidth, output_size);
            parameters.Set(NVSDK_NGX_Parameter_OutHeight, output_size);
            parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 8u);
            parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 8u);
            parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
                           static_cast<uint32_t>(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes));
            parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
            parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
            parameters.Set(NVSDK_NGX_Parameter_Reset, 0u);
            const auto* handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0x5C000} + cases++);
            native::RegisterCreatedFeatureLocked(handle, native::kFeatureDlss, &parameters);
            std::array<const NVSDK_NGX_Handle*, 4> stable_handles{};
            uint32_t stable_frames = 0;
            bool isolate_render_reset = false;
            bool isolated_reset_checked = false;
            for (uint32_t attempt = 0; attempt < 20 && stable_frames < 2; ++attempt) {
              evaluations.clear();
              gpu.Transition(color.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COMMON);
              gpu.Write(color.Get(), 96);
              gpu.Transition(color.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
              Snapshot before_sr, after_sr;
              bool swapped = false;
              {
                const addon::EvaluateChainScope outer;
                native::PreSrSwap swap;
                if (render_count != 0) native::ProcessInlinePreSR(gpu.list.Get(), handle, &parameters, swap);
                auto* consumed = native::GetD3D12Resource(&parameters, NVSDK_NGX_Parameter_Color);
                swapped = swap.result != nullptr;
                if (swapped) {
                  Check(consumed == swap.result && consumed != color.Get() && swap.color == color.Get(),
                        "SR mock consumes the parameter block's actual PreSR stand-in");
                  before_sr = Capture(gpu, consumed, native::kPreSrReadable);
                  const auto before_duplicate = model_calls;
                  {
                    const addon::EvaluateChainScope inner;
                    native::PreSrSwap duplicate;
                    Check(!native::ProcessInlinePreSR(gpu.list.Get(), handle, &parameters, duplicate)
                              && duplicate.result == nullptr && model_calls == before_duplicate,
                          "nested PreSR mirror cannot evaluate the Render group twice");
                  }
                }
                const auto input_state = (swapped ? native::kPreSrReadable : NATIVE_INPUT_STATE);
                gpu.Transition(consumed, input_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
                gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
                const D3D12_TEXTURE_COPY_LOCATION from{.pResource = consumed,
                    .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
                const D3D12_TEXTURE_COPY_LOCATION to{.pResource = output.Get(),
                    .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
                // A deterministic tiled SR surrogate: each copy has equal source
                // and destination extents; no invalid CopyResource resize occurs.
                for (uint32_t y = 0; y < output_size; y += 8) {
                  for (uint32_t x = 0; x < output_size; x += 8) {
                    gpu.list->CopyTextureRegion(&to, x, y, 0, &from, nullptr);
                  }
                }
                gpu.Transition(consumed, D3D12_RESOURCE_STATE_COPY_SOURCE, input_state);
                gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                after_sr = Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                native::RestorePreSrColor(&parameters, swap);
                Check(native::GetD3D12Resource(&parameters, NVSDK_NGX_Parameter_Color) == color.Get() && swap.color == nullptr,
                      "RestorePreSrColor restores the original game's pointer");
                // Make the original Color disagree with the SR output. The next
                // model must encode SR output, not re-read this old input.
                gpu.Transition(color.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COMMON);
                gpu.Write(color.Get(), 208);
                gpu.Transition(color.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
                if (render_count < count) {
                  native::ProcessInline(gpu.list.Get(), handle, &parameters);
                  const auto before_duplicate = model_calls;
                  {
                    const addon::EvaluateChainScope inner;
                    native::ProcessInline(gpu.list.Get(), handle, &parameters);
                  }
                  Check(model_calls == before_duplicate, "nested Upscaled mirror cannot run the chain again");
                }
              }
              const auto final_output = Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
              gpu.Submit();
              Check(addon::evaluate_chain.depth == 0 && addon::evaluate_chain.output_count == 0,
                    "outer scope releases all dedupe marks for the next frame");
              Check(evaluations.size() <= count, "one evaluate chain cannot execute more than its logical pass count");
              if (evaluations.size() != count) continue;
              if (render_count != 0 && !swapped) continue;
              const auto sr_pixels = after_sr.Pixels();
              Check(final_output.Pixels() == sr_pixels,
                    "identity model preserves actual SR pixels through every Upscaled pass");
              Check(sr_pixels.front() == 96 && sr_pixels.back() == 96,
                    "SR GPU output retains the consumed input rather than the overwritten original Color");
              if (swapped) {
                const auto pre_pixels = before_sr.Pixels();
                for (uint32_t y = 0; y < output_size; ++y) {
                  for (uint32_t x = 0; x < output_size; ++x) {
                    for (uint32_t channel = 0; channel < 4; ++channel) {
                      Check(sr_pixels[(y * output_size + x) * 4 + channel]
                                == pre_pixels[((y % 8) * 8 + x % 8) * 4 + channel],
                            "GPU readback proves SR mock copied the swapped Color at each pixel");
                    }
                  }
                }
              }
              auto& root = native::features.at(handle);
              const bool mixed = render_count > 0 && render_count < count;
              Check(static_cast<bool>(root.render_session) == mixed,
                    "only mixed chains allocate an independent Render session");
              bool all_steady = true;
              const auto reference_input = evaluations.front().input.Pixels();
              for (uint32_t pass = 0; pass < count; ++pass) {
                const auto& call = evaluations[pass];
                const auto& session = (mixed && pass < render_count ? *root.render_session : root);
                const uint32_t slot = (mixed && pass >= render_count ? pass - render_count : pass);
                Check(call.handle == session.slots[slot].handle
                          && presets.at(call.handle) == (((models >> (pass & 1u)) & 1u) + 1),
                      "logical pass executes once in order with its independent requested A/B model");
                const uint32_t expected_size = (pass < render_count ? 8u : output_size);
                Check(call.input.width == expected_size && call.input.height == expected_size,
                      "model input uses its own stage dimensions");
                const auto pixels = call.input.Pixels();
                Check(call.input.pixel_bytes == evaluations.front().input.pixel_bytes,
                      "stage codec inputs use the same explicit pixel contract");
                // Identity NR on uniform SDR must encode the same GPU colour
                // despite the original overwrite after the SR copy.
                for (size_t byte = 0; byte < pixels.size(); ++byte) {
                  Check(pixels[byte] == reference_input[byte % reference_input.size()],
                        "Upscaled encoded GPU pixels come from SR output, not stale original Color");
                }
                std::vector<float> row(static_cast<size_t>(call.input.width) * 4);
                for (uint32_t y = 0; y < call.input.height; ++y) {
                  Check(addon::screenshot::internal::DecodeDiagnosticRow(
                            pixels.data() + static_cast<size_t>(y) * call.input.width * call.input.pixel_bytes,
                            call.input.width, call.input.format, row.data()), "stage model input pixels decode");
                  for (uint32_t x = 0; x < call.input.width; ++x) {
                    for (uint32_t channel = 0; channel < 3; ++channel) {
                      Check(std::abs(row[x * 4 + channel] - 96.f / 255.f) < 0.002f,
                            "encoded model input matches the SR GPU colour, including Upscaled-only controls");
                    }
                  }
                }
                if (stable_frames != 0) {
                  Check(call.handle == stable_handles[pass], "steady stage contracts retain independent model handles");
                }
                stable_handles[pass] = call.handle;
                all_steady &= call.reset == 0;
                if (isolate_render_reset) {
                  Check(call.reset == (pass == 0 ? 1 : 0),
                        "resetting one Render slot leaves Upscaled and other pass histories intact");
                }
              }
              if (mixed) {
                Check(root.render_session.get() != &root && root.render_session->pass_offset == 0
                          && root.pass_offset == render_count,
                      "stage sessions retain independent logical pass offsets");
                Check(root.render_session->input_width == 8 && root.input_width == output_size,
                      "Render and root dimensions never overwrite one another");
                Check(root.render_session->output_width == 8 && root.output_width == output_size,
                      "Render and root retain their own output dimensions");
                Check(root.render_session->slots[0].handle != root.slots[0].handle
                          && root.render_session->slots[0].parameters != root.slots[0].parameters,
                      "stage sessions never share an NR feature/history handle or parameter block");
              }
              if (isolate_render_reset) {
                isolated_reset_checked = true;
                isolate_render_reset = false;
              }
              if (all_steady) {
                ++stable_frames;
                if (mixed && !isolated_reset_checked) {
                  root.render_session->slots[0].pending_reset = true;
                  isolate_render_reset = true;
                }
              }
            }
            Check(stable_frames == 2, "complete chain reaches two steady submitted frames within bounded warmup");
            Check(render_count == 0 || render_count == count || isolated_reset_checked,
                  "mixed history isolation was exercised after full model warmup");
            native::ForgetSourceHandleLocked(handle);
            gpu.Submit();
          }
        }
      }
    }
    if (info != nullptr) {
      for (UINT64 index = 0; index < info->GetNumStoredMessages(); ++index) {
        SIZE_T bytes = 0;
        info->GetMessage(index, nullptr, &bytes);
        std::vector<uint8_t> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        Check(SUCCEEDED(info->GetMessage(index, message, &bytes)), "stage debug message read");
        Check(message->Severity != D3D12_MESSAGE_SEVERITY_ERROR
                  && message->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION, message->pDescription);
      }
    }
    const auto groups_before = native::successful_evaluations.load() + native::successful_pre_sr_evaluations.load();
    const auto sources_before = native::SourceSuccessfulEvaluations();
    native::cost_seen_published = addon::gpu_timers::published_segments;
    native::cost_seen_total_us = addon::gpu_timers::measured_total_us;
    native::cost_window = {.start_ns = 1, .start_evals = groups_before, .start_source_evals = sources_before,
        .presents = 60, .frame_ms = 1200, .chain_ms = 14, .samples = 4};
    native::successful_evaluations.fetch_add(2);
    native::successful_pre_sr_evaluations.fetch_add(2);
    addon::mixed_stage_group_successes.fetch_add(4);
    addon::mixed_stage_source_successes.fetch_add(2);
    native::TickNrCost(1 + native::kCostWindowNs, 0);
    Check(std::abs(native::nr_cost_gpu_ms.load() - 7.f) < .001f,
          "cost panel reports both 3.5ms stage groups as 7ms per source SR, not half the NR cost");
    Check(std::abs(native::nr_cost_present_ms.load() - (14.f / 60.f)) < .001f,
          "per-present cost counts the four stage groups once without multiplying source totals again");
    std::cout << "PASS stage-chain cases=" << cases << " boundaries=" << boundary_cases
              << " partial_warmups=" << partial_warmups << " failed_predecessors=" << failed_predecessors
              << " production PreSR/Upscaled, WARP copy-consumer/readback; NVIDIA SR/NR substituted\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL stage-chain: " << error.what() << '\n';
    return 1;
  }
}
}  // namespace stage_execution_cases
