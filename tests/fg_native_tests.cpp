#define DEBUG_LEVEL_0
#include <windows.h>
#include <d3d12sdklayers.h>
#include <array>
#include <cmath>
#include <limits>
#include <tuple>
#include <type_traits>
#include <dlssnr.hpp>

// Reuse the WARP/list/submission host, without running the adapter matrix.
#define main UnusedXeFgTestMain
#include "fg_xefg_tests.cpp"
#undef main

static std::vector<std::pair<int, std::string>> captured_logs;

extern "C" __declspec(dllexport) void ReShadeLogMessage(void*, int level, const char* message) {
  captured_logs.emplace_back(level, message);
  std::cout << message << '\n';
}
extern "C" __declspec(dllexport) bool ReShadeGetBasePath(char*, size_t* size) {
  *size = 0;
  return false;
}

#include "fg_funnel_accounting_tests.hpp"

namespace {
namespace native = renodx::addons::dlss5::internal;
Gpu* model_gpu = nullptr;
uint32_t model_calls = 0;
std::vector<int> model_resets;
std::vector<const NVSDK_NGX_Handle*> model_evaluated_handles;
std::vector<const NVSDK_NGX_Handle*> model_released_handles;
uint32_t fail_call = 0;
uint32_t next_handle = 0;
std::map<const NVSDK_NGX_Handle*, uint32_t> model_create_flags;
std::function<void()> model_ui_observer;
constexpr auto NATIVE_INPUT_STATE = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
using BarrierFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
BarrierFn real_barrier = nullptr;
std::map<ID3D12Resource*, D3D12_RESOURCE_STATES> observed_states;
ID3D12Resource* game_inputs[2]{};
uint32_t state_errors = 0;
uint32_t observed_barriers = 0;
using CopyFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*,
    UINT, UINT, UINT, const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
CopyFn real_copy = nullptr;
uint32_t hudless_snapshot_copies = 0;
uint32_t staged_input_copies = 0;
uint32_t recorded_copies = 0;
uint32_t recorded_barriers = 0;

void STDMETHODCALLTYPE ObserveCopies(ID3D12GraphicsCommandList* list,
    const D3D12_TEXTURE_COPY_LOCATION* destination, UINT x, UINT y, UINT z,
    const D3D12_TEXTURE_COPY_LOCATION* source, const D3D12_BOX* box) {
  if (model_gpu != nullptr && list == model_gpu->list.Get()) {
    ++recorded_copies;
    for (const auto& [key, stream] : native::fg_service::streams) {
      for (const auto& slot : stream.slots) {
        if (destination->pResource == slot.original_hudless) ++hudless_snapshot_copies;
        if ((destination->pResource == slot.color[0] && source->pResource == game_inputs[0])
            || (destination->pResource == slot.color[1] && source->pResource == game_inputs[1])) {
          ++staged_input_copies;
        }
      }
    }
  }
  real_copy(list, destination, x, y, z, source, box);
}

// Observe the actual COM calls and always forward their original arguments.
// Only game inputs have independently established initial states; scratch
// texture initial states are deliberately not inferred by this observer.
void STDMETHODCALLTYPE ObserveBarriers(ID3D12GraphicsCommandList* list, UINT count,
                                       const D3D12_RESOURCE_BARRIER* barriers) {
  if (model_gpu != nullptr && list == model_gpu->list.Get()) {
    recorded_barriers += count;
    for (UINT i = 0; i < count; ++i) {
      if (barriers[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) continue;
      const auto& transition = barriers[i].Transition;
      const auto previous = observed_states.find(transition.pResource);
      for (size_t role = 0; role < 2; ++role) {
        if (transition.pResource != game_inputs[role]) continue;
        ++observed_barriers;
        if (previous != observed_states.end() && previous->second != transition.StateBefore) {
          ++state_errors;
          std::cerr << "STATE ERROR input=" << (role == 0 ? "backbuffer" : "HUDLess")
                    << " actual_previous=" << previous->second << " declared_before=" << transition.StateBefore
                    << " declared_after=" << transition.StateAfter << '\n';
        }
      }
      observed_states[transition.pResource] = transition.StateAfter;
    }
  }
  real_barrier(list, count, barriers);
}

NVSDK_NGX_Result NVSDK_CONV CreateModel(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature feature,
                                      const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle) {
  Check(feature == native::kFeatureDlssNr, "only NR model entry replaced");
  *handle = reinterpret_cast<NVSDK_NGX_Handle*>(new uint32_t(++next_handle));
  model_create_flags[*handle] = native::GetUInt(parameters, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags);
  return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result NVSDK_CONV ReleaseModel(NVSDK_NGX_Handle* handle) {
  model_released_handles.push_back(handle);
  delete reinterpret_cast<uint32_t*>(handle);
  return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result NVSDK_CONV EvaluateModel(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
                                        const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback) {
  if (model_ui_observer) model_ui_observer();
  model_resets.push_back(native::GetInt(parameters, "DLSSNR.Reset", -1));
  model_evaluated_handles.push_back(handle);
  if (++model_calls == fail_call) return NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
  auto* output = native::GetD3D12Resource(parameters, "DLSSNR.Output");
  Check(output != nullptr && list == model_gpu->list.Get(), "model gets real output and game list");
  const auto desc = output->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  uint64_t bytes = 0;
  model_gpu->device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  auto upload = model_gpu->Buffer(bytes, D3D12_HEAP_TYPE_UPLOAD);
  void* mapped = nullptr;
  Check(SUCCEEDED(upload->Map(0, nullptr, &mapped)), "model upload map");
  std::memset(mapped, 0, static_cast<size_t>(bytes));
  upload->Unmap(0, nullptr);
  native::Transition(list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TEXTURE_COPY_LOCATION from{.pResource = upload.Get(),
      .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = footprint};
  D3D12_TEXTURE_COPY_LOCATION to{.pResource = output,
      .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0};
  list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  native::Transition(list, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  model_gpu->uploads.push_back(upload);
  return NVSDK_NGX_Result_Success;
}
}  // namespace

// The isolated idle test is defined after the existing scenarios.
int RunNativeOwnerIdleTest();
int RunNativeOptionsResumeTest(bool delayed_options = false);
int RunNativeGuideContractTest();
int RunPresentGuideModelsTest();
int RunNativePresentDecisionTest();

int RunNativeRequiredMenuTest() {
  try {
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    Gpu gpu;
    model_gpu = &gpu;
    {
      namespace shot = renodx::addons::dlss5::screenshot;
      Check(!shot::internal::pending, "planar depth fixture starts without pending capture");
      shot::internal::pending.emplace();
      shot::internal::pending->stage_planes = true;
      for (const auto format : {DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_D32_FLOAT_S8X24_UINT}) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = 8;
        desc.Height = 8;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
        ComPtr<ID3D12Resource> depth;
        ComPtr<ID3D12Resource> other_depth;
        Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                  D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&depth)))
                  && SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                      D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&other_depth))),
              "planar depth-stencil resources");
        const D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV, .NumDescriptors = 1};
        ComPtr<ID3D12DescriptorHeap> dsv_heap;
        Check(SUCCEEDED(gpu.device->CreateDescriptorHeap(&dsv_heap_desc, IID_PPV_ARGS(&dsv_heap))), "planar DSV heap");
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
        dsv_desc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        const auto dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
        gpu.device->CreateDepthStencilView(depth.Get(), &dsv_desc, dsv);
        gpu.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.375f, 0x17, 0, nullptr);
        const D3D12_RECT stencil_rect{4, 0, 8, 8};
        gpu.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_STENCIL, 0.f, 0xe3, 1, &stencil_rect);

        const std::string name = "planar_depth_" + std::to_string(format);
        Check(shot::PrepareDiagnosticPlane(gpu.device.Get(), name.c_str(), depth.Get(), 8, 8),
              "prepare planar depth plane zero");
        auto& plane = shot::internal::pending->planes.back();
        Check(plane.source_format == format && plane.format == plane.footprint.Footprint.Format
                  && shot::internal::DiagnosticPixelBytes(plane.format) == 4
                  && shot::internal::DiagnosticFootprintValid(plane), "planar depth uses four-byte plane footprint");
        const auto export_format = plane.format;
        plane.format = DXGI_FORMAT_R16_FLOAT;
        Check(!shot::internal::DiagnosticFootprintValid(plane), "planar footprint format mismatch remains rejected");
        plane.format = export_format;
        shot::RecordDiagnosticPlaneCopy(gpu.list.Get(), name.c_str(), other_depth.Get());
        Check(!plane.recorded && plane.error == "record_source_mismatch", "planar source identity mismatch rejected");
        plane.error.clear();
        plane.source_format = DXGI_FORMAT_R32_FLOAT;
        shot::RecordDiagnosticPlaneCopy(gpu.list.Get(), name.c_str(), depth.Get());
        Check(!plane.recorded && plane.error == "record_source_mismatch", "planar source format mismatch rejected");
        plane.error.clear();
        plane.source_format = format;
        gpu.Transition(depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        shot::RecordDiagnosticPlaneCopy(gpu.list.Get(), name.c_str(), depth.Get());
        Check(plane.recorded && plane.error.empty(), "record planar depth copy");

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT stencil_footprint{};
        uint64_t stencil_bytes = 0;
        gpu.device->GetCopyableFootprints(&desc, 1, 1, 0, &stencil_footprint, nullptr, nullptr, &stencil_bytes);
        auto stencil_readback = gpu.Buffer(stencil_bytes, D3D12_HEAP_TYPE_READBACK);
        const D3D12_TEXTURE_COPY_LOCATION stencil_source{.pResource = depth.Get(),
            .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 1};
        const D3D12_TEXTURE_COPY_LOCATION stencil_destination{.pResource = stencil_readback.Get(),
            .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = stencil_footprint};
        gpu.list->CopyTextureRegion(&stencil_destination, 0, 0, 0, &stencil_source, nullptr);
        gpu.Submit();
        uint8_t* mapped_depth = nullptr;
        uint8_t* mapped_stencil = nullptr;
        Check(SUCCEEDED(plane.readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped_depth)))
                  && SUCCEEDED(stencil_readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped_stencil))),
              "completed planar readbacks map");
        float rgba[32]{};
        for (uint32_t y = 0; y < 8; ++y) {
          Check(shot::internal::DecodeDiagnosticRow(mapped_depth + plane.footprint.Offset
                      + y * plane.footprint.Footprint.RowPitch, 8, plane.format, rgba), "decode planar depth row");
          for (uint32_t x = 0; x < 8; ++x) {
            Check(rgba[x * 4] == 0.375f && rgba[x * 4 + 1] == 0.f
                      && rgba[x * 4 + 2] == 0.f && rgba[x * 4 + 3] == 1.f,
                  "planar depth every pixel is clear float without interleaved stencil");
            Check(mapped_stencil[stencil_footprint.Offset + y * stencil_footprint.Footprint.RowPitch + x]
                      == (x < 4 ? 0x17 : 0xe3), "independent stencil plane retains mixed clear values");
          }
        }
        plane.readback->Unmap(0, nullptr);
        stencil_readback->Unmap(0, nullptr);
        std::cout << "PASS planar depth source_format=" << format << " footprint_format=" << plane.format
                  << " depth=0.375 stencil=23/227 all 64 pixels verified\n";
      }
      auto regular = gpu.Texture();
      Check(shot::PrepareDiagnosticPlane(gpu.device.Get(), "regular_format", regular.Get(), 8, 8),
            "prepare regular diagnostic plane");
      auto& regular_plane = shot::internal::pending->planes.back();
      Check(regular_plane.source_format == regular_plane.format
                && shot::internal::DiagnosticFootprintValid(regular_plane), "regular format contract retained");
      regular_plane.source_format = DXGI_FORMAT_UNKNOWN;
      gpu.Transition(regular.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
      shot::RecordDiagnosticPlaneCopy(gpu.list.Get(), "regular_format", regular.Get());
      Check(regular_plane.recorded && regular_plane.error.empty(), "legacy manual plane source format fallback retained");
      gpu.Submit();
      shot::internal::ReleaseReadbacks(*shot::internal::pending);
      shot::internal::pending.reset();
    }
    ComPtr<ID3D12InfoQueue> info;
    if (debug != nullptr) Check(SUCCEEDED(gpu.device.As(&info)), "Required debug queue");
    std::cout << "NATIVE_REQUIRED_MENU isolated WARP; debug=" << (debug != nullptr) << '\n';
    real_barrier = reinterpret_cast<BarrierFn>((*reinterpret_cast<void***>(gpu.list.Get()))[26]);
    real_copy = reinterpret_cast<CopyFn>((*reinterpret_cast<void***>(gpu.list.Get()))[16]);
    Check(DetourTransactionBegin() == NO_ERROR, "Required observer transaction");
    Check(DetourUpdateThread(GetCurrentThread()) == NO_ERROR, "Required observer thread");
    Check(DetourAttach(reinterpret_cast<PVOID*>(&real_barrier), reinterpret_cast<PVOID>(ObserveBarriers)) == NO_ERROR,
          "Required barrier observer attach");
    Check(DetourAttach(reinterpret_cast<PVOID*>(&real_copy), reinterpret_cast<PVOID>(ObserveCopies)) == NO_ERROR,
          "Required copy observer attach");
    Check(DetourTransactionCommit() == NO_ERROR, "Required observer commit");
    native::enabled.store(true);
    native::hooks_enabled.store(true);
    native::hook_point.store(native::kHookPresent);
    native::present_pre_fg.store(1);
    native::present_guides.store(native::kPresentGuidesOptional);
    native::stack_passes.store(1);
    native::nr_follow_input_res.store(false);
    native::nr_resolution_scale.store(1.f);
    native::codec_mode.store(0);
    native::transfer_strength.store(1.f);
    native::color_strength.store(1.f);
    native::direct_api.module = GetModuleHandleW(nullptr);
    native::direct_api.create = CreateModel;
    native::direct_api.evaluate = EvaluateModel;
    native::direct_api.release = ReleaseModel;
    native::direct_load_state = native::DirectLoadState::Ready;
    native::direct_device = gpu.device.Get();
    native::direct_device->AddRef();
    native::direct_device_native = gpu.device.Get();
    native::parameter_runtime_ready = true;
    native::core_allocate_parameters = renodx::addons::dlss5::bridge::AllocateOwnedParameters;
    native::core_destroy_parameters = renodx::addons::dlss5::bridge::DestroyOwnedParameters;
    Check(native::InstallQueueCompletionHooks(gpu.queue.Get()), "Required real queue hooks");
    Check(native::InstallCommandListStateHooks(gpu.list.Get()), "Required real list hooks");
    const auto submit = [&] {
      Check(SUCCEEDED(gpu.list->Close()), "Required close");
      ID3D12CommandList* lists[]{gpu.list.Get()};
      gpu.queue->ExecuteCommandLists(1, lists);
      Check(SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial)), "Required signal");
      HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      Check(event != nullptr, "Required completion event");
      const auto signal = gpu.fence->SetEventOnCompletion(gpu.serial, event);
      const auto wait = SUCCEEDED(signal) ? WaitForSingleObject(event, 10000) : WAIT_FAILED;
      CloseHandle(event);
      Check(wait == WAIT_OBJECT_0, "Required bounded GPU completion");
      Check(SUCCEEDED(gpu.allocator->Reset()), "Required allocator reset");
      Check(SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)), "Required list reset");
      submission::PruneCompletedGenerations();
      gpu.uploads.clear();
    };
    submit();
    auto backbuffer = gpu.Texture();
    auto hudless = gpu.Texture();
    auto ui = gpu.Texture();
    for (auto* resource : {backbuffer.Get(), hudless.Get(), ui.Get()}) {
      gpu.Transition(resource, D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
    }
    const auto write_inputs = [&](uint8_t back, uint8_t scene) {
      for (auto [resource, value] : {std::pair{backbuffer.Get(), back},
                                    std::pair{hudless.Get(), scene}, std::pair{ui.Get(), uint8_t{255}}}) {
        gpu.Transition(resource, NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COMMON);
        gpu.Write(resource, value);
        gpu.Transition(resource, D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
      }
      submit();
    };
    const auto pixels = [&](ID3D12Resource* resource) {
      const auto desc = resource->GetDesc();
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
      uint64_t bytes = 0;
      gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
      auto readback = gpu.Buffer(bytes, D3D12_HEAP_TYPE_READBACK);
      gpu.Transition(resource, NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COPY_SOURCE);
      const D3D12_TEXTURE_COPY_LOCATION source{.pResource = resource,
          .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      const D3D12_TEXTURE_COPY_LOCATION destination{.pResource = readback.Get(),
          .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = footprint};
      gpu.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
      gpu.Transition(resource, D3D12_RESOURCE_STATE_COPY_SOURCE, NATIVE_INPUT_STATE);
      submit();
      uint8_t* mapped = nullptr;
      Check(SUCCEEDED(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped))), "Required readback map");
      std::vector<uint8_t> result(256);
      for (uint32_t y = 0; y < 8; ++y) {
        std::memcpy(result.data() + y * 32, mapped + footprint.Offset + y * footprint.Footprint.RowPitch, 32);
      }
      readback->Unmap(0, nullptr);
      return result;
    };
    ComPtr<ID3D12Resource> motion, depth;
    for (auto [resource, format] : {std::pair{motion.GetAddressOf(), DXGI_FORMAT_R16G16_FLOAT},
                                  std::pair{depth.GetAddressOf(), DXGI_FORMAT_R32_FLOAT}}) {
      const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
      const D3D12_RESOURCE_DESC desc{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
          .Width = 8, .Height = 8, .DepthOrArraySize = 1, .MipLevels = 1, .Format = format,
          .SampleDesc = {1, 0}};
      Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                NATIVE_INPUT_STATE, nullptr, IID_PPV_ARGS(resource))), "Required actual guide resource");
    }
    // Both guide formats have four bytes per pixel. Preserve exact float/half
    // bits so the fixture never relies on default-heap initialization.
    const auto write_words = [&](ID3D12Resource* resource, const std::array<uint32_t, 64>& words) {
      const auto desc = resource->GetDesc();
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
      uint64_t bytes = 0;
      gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
      auto upload = gpu.Buffer(bytes, D3D12_HEAP_TYPE_UPLOAD);
      uint8_t* mapped = nullptr;
      Check(SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))), "guide pattern upload map");
      std::memset(mapped, 0, static_cast<size_t>(bytes));
      for (uint32_t y = 0; y < 8; ++y) {
        std::memcpy(mapped + footprint.Offset + y * footprint.Footprint.RowPitch, words.data() + y * 8, 32);
      }
      upload->Unmap(0, nullptr);
      gpu.Transition(resource, NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COPY_DEST);
      const D3D12_TEXTURE_COPY_LOCATION from{.pResource = upload.Get(),
          .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = footprint};
      const D3D12_TEXTURE_COPY_LOCATION to{.pResource = resource,
          .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
      gpu.Transition(resource, D3D12_RESOURCE_STATE_COPY_DEST, NATIVE_INPUT_STATE);
      gpu.uploads.push_back(upload);
    };
    std::array<uint32_t, 64> depth_words{}, motion_words{};
    depth_words.fill(0x3f000000u);  // Finite 0.5 depth, distinct from either far plane.
    write_words(depth.Get(), depth_words);
    write_words(motion.Get(), motion_words);
    submit();
    auto& owner = native::present_streams[nullptr];
    owner.device_identity = renodx::utils::directx::NativeIdentity(gpu.device.Get());
    owner.backbuffer_width = owner.backbuffer_height = 8;
    owner.backbuffer_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    owner.prefg_target_valid = true;
    owner.prefg_target.diagnostic.bypass_host_look = true;
    owner.prefg_target.pass_transfer_strength[0] = 1.f;
    owner.prefg_target.pass_color_strength[0] = 1.f;
    renodx::addons::dlss5::bridge::OwnedParameters parameters;
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_Backbuffer, backbuffer.Get());
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_HUDLess, hudless.Get());
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_UI, ui.Get());
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_UIAlpha, ui.Get());
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_DepthInverted, 1u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, 3u);
    const auto* handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xF018});
    native::RegisterCreatedFeatureLocked(handle, NVSDK_NGX_Feature_FrameGeneration, &parameters);
    unsigned failures = 0;
    const auto expect = [&](bool passed, const std::string& message) {
      if (!passed) ++failures;
      std::cout << (passed ? "PASS " : "FAIL ") << message << '\n';
    };
    unsigned long long frame_id = 0;
    const auto record_base = [&](bool warmup) {
      write_inputs(64, 96);
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 1u);
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, ++frame_id);
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_NotRenderingGameFrames, 0u);
      const auto before_calls = model_calls;
      const auto before_submitted = native::native_fg_submitted_frames.load();
      ID3D12Resource* original = nullptr;
      bool replaced = false;
      for (uint32_t attempt = 0; attempt < (warmup ? 4u : 1u) && original == nullptr; ++attempt) {
        original = native::RunNativePreFg(gpu.list.Get(), &parameters, handle, &replaced);
        if (original == nullptr) submit();
      }
      Check(original == hudless.Get() && replaced
                && (warmup ? model_calls >= before_calls + 2 : model_calls == before_calls + 2),
            "Required base reaches actual dual NR; recovery needs only the next base evaluate");
      const auto enhanced = pixels(backbuffer.Get());
      const auto enhanced_scene = pixels(native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_HUDLess));
      for (size_t i = 0; i < enhanced.size(); ++i) {
        Check(enhanced[i] == (i % 4 == 3 ? 64 : 0), "Required B output keeps NR with supplied resources");
        Check(enhanced_scene[i] == (i % 4 == 3 ? 96 : 0), "Required H output keeps NR with supplied resources");
      }
      native::RestoreNativePreFg(gpu.list.Get(), &parameters, original, true, handle, replaced);
      submit();
      Check(native::native_fg_submitted_frames.load() == before_submitted + 1,
            "Required base counts exactly one actual NR application submission");
    };
    const auto reject_mfg = [&](const std::string& phase, uint32_t index = 2u) {
      // Distinct current pixels expose an old cached-color write even if it
      // does not invoke another model or increment the application counter.
      write_inputs(160, 192);
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, index);
      const auto calls = model_calls;
      const auto creates = next_handle;
      const auto copies = recorded_copies;
      const auto barriers = recorded_barriers;
      const auto serial = native::native_fg_recording_serial.load();
      const auto submitted = native::native_fg_submitted_frames.load();
      bool replaced = true;
      auto* original = native::RunNativePreFg(gpu.list.Get(), &parameters, handle, &replaced);
      expect(original == nullptr && !replaced, phase + " declines without replacement proof");
      expect(native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_HUDLess) == hudless.Get()
                 && native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_UI) == ui.Get(),
             phase + " preserves HUDLess/UI bindings");
      expect(model_calls == calls && next_handle == creates && recorded_copies == copies
                 && recorded_barriers == barriers, phase + " records no model, copy or barrier");
      auto& stream = native::fg_service::streams.at({true, reinterpret_cast<uintptr_t>(&owner)});
      expect(stream.cached == nullptr && stream.native_serial == 0
                 && native::native_fg_marker.load() == nullptr && !native::native_fg_continuing_submission.load()
                 && native::native_fg_result.load() == fg::Result::NOT_SERVED,
             phase + " clears reusable colors and current NR submission proof");
      const auto back_pixels = pixels(backbuffer.Get());
      const auto scene_pixels = pixels(hudless.Get());
      const auto ui_pixels = pixels(ui.Get());
      expect(back_pixels == std::vector<uint8_t>(256, 160) && scene_pixels == std::vector<uint8_t>(256, 192)
                 && ui_pixels == std::vector<uint8_t>(256, 255), phase + " preserves actual GPU input pixels");
      expect(native::native_fg_recording_serial.load() == serial
                 && native::native_fg_submitted_frames.load() == submitted,
             phase + " adds no NR recording or queue-proven submission");
      if (replaced) {
        native::RestoreNativePreFg(gpu.list.Get(), &parameters, original, true, handle, replaced);
      }
      submit();
    };
    record_base(true);
    auto& stream = native::fg_service::streams.at({true, reinterpret_cast<uintptr_t>(&owner)});
    Check(stream.cached != nullptr && !stream.roles[0].real_guides, "Optional base really used neutral guides");
    const auto configuration = native::configuration_generation.load();
    const auto history = native::history_reset_epoch.load();
    native::present_guides.store(native::kPresentGuidesRequired);
    native::ApplySettingEffect(native::SettingEffect::kHistory);
    Check(native::configuration_generation.load() == configuration && native::history_reset_epoch.load() == history + 1,
          "actual Required setting effect advances history without recreating the native stream");
    reject_mfg("Optional-neutral -> Required MFG");
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_Depth, depth.Get());
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MVecs, motion.Get());
    record_base(false);
    Check(stream.roles[0].real_guides && stream.roles[1].real_guides, "Required recovery consumes real guides");
    std::cout << "PASS next base recovers after neutral-cache rejection\n";
    const auto calls = model_calls;
    const auto submitted = native::native_fg_submitted_frames.load();
    const auto serial = native::native_fg_recording_serial.load();
    auto* cached = stream.cached;
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 2u);
    bool replaced = false;
    auto* original = native::RunNativePreFg(gpu.list.Get(), &parameters, handle, &replaced);
    Check(original == hudless.Get() && replaced && stream.cached == cached && model_calls == calls,
          "Required real-guide MFG reuses the actual cached result without another model");
    const auto enhanced = pixels(backbuffer.Get());
    for (size_t i = 0; i < enhanced.size(); ++i) {
      Check(enhanced[i] == (i % 4 == 3 ? 64 : 0), "Required real-guide MFG GPU output");
    }
    native::RestoreNativePreFg(gpu.list.Get(), &parameters, original, true, handle, replaced);
    submit();
    Check(native::native_fg_recording_serial.load() == serial
              && native::native_fg_submitted_frames.load() == submitted,
          "Required valid MFG neither reevaluates nor counts another application submission");
    std::cout << "PASS Required real-guide normal MFG reuse\n";
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_Depth, static_cast<ID3D12Resource*>(nullptr));
    reject_mfg("Real -> missing current depth MFG");
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_Depth, depth.Get());
    record_base(false);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MVecs, static_cast<ID3D12Resource*>(nullptr));
    reject_mfg("Real -> missing current motion repeated index1", 1u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MVecs, motion.Get());
    record_base(false);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MVecsSubrectWidth, 0u);
    reject_mfg("Real -> empty current guide extent MFG");
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MVecsSubrectWidth, 8u);
    record_base(false);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_NotRenderingGameFrames, 1u);
    reject_mfg("Real -> NotRenderingGameFrames MFG");
    record_base(false);
    std::cout << "PASS next base recovers after NotRenderingGameFrames rejection\n";
    // Use the real SDK callbacks and WARP inputs. This host has no ReShade
    // swapchain facade: Final's RunPresentPath gate is not exercised here.
    {
      using Phase = renodx::addons::dlss5::fg::native_options::PresentPhase;
      HWND window = CreateWindowExW(0, L"STATIC", L"NR guide scope", WS_OVERLAPPED,
          0, 0, 8, 8, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
      Check(window != nullptr, "guide scope hidden window");
      const std::unique_ptr<std::remove_pointer_t<HWND>, decltype(&DestroyWindow)> window_owner(window, &DestroyWindow);
      ComPtr<IDXGIFactory4> factory;
      Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "guide scope factory");
      DXGI_SWAP_CHAIN_DESC1 desc{};
      desc.Width = desc.Height = 8;
      desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      desc.SampleDesc.Count = 1;
      desc.BufferCount = 2;
      desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
      ComPtr<IDXGISwapChain1> output;
      Check(SUCCEEDED(factory->CreateSwapChainForHwnd(gpu.queue.Get(), window, &desc, nullptr, nullptr, &output)),
            "guide scope real WARP output");
      const void* identity = renodx::utils::directx::NativeIdentity(output.Get());
      int other_output = 0;
      owner.native_output_identity = identity;
      owner.native_route_id = 77;
      const auto begin = [&] {
        native::OnNativePresentObserved(output.Get(), 0, Phase::SDK_BEGIN);
        expect(native::native_sdk_present_input.decision_id != 0
                   && !native::native_sdk_present_input.guides_unavailable
                   && native::native_sdk_present_input.owner == nullptr,
               "SDK_BEGIN clears previous negative scope without proving availability");
      };
      // Each cause must survive either SDK outcome, including failed Restore.
      for (uint32_t cause = 0; cause < 4; ++cause) {
        for (const bool succeeded : {true, false}) {
          record_base(false);
          begin();
          const auto decision = native::native_sdk_present_input.decision_id;
          if (cause == 0) {
            parameters.Set(NVSDK_NGX_DLSSG_Parameter_Depth, static_cast<ID3D12Resource*>(nullptr));
          }
          if (cause == 1) {
            parameters.Set(NVSDK_NGX_DLSSG_Parameter_MVecs, static_cast<ID3D12Resource*>(nullptr));
          }
          if (cause == 2) {
            parameters.Set(NVSDK_NGX_DLSSG_Parameter_MVecsSubrectHeight, 0u);
          }
          if (cause == 3) {
            parameters.Set(NVSDK_NGX_DLSSG_Parameter_NotRenderingGameFrames, 1u);
          }
          reject_mfg("SDK scoped missing guides cause=" + std::to_string(cause));
          native::RestoreNativePreFg(gpu.list.Get(), &parameters, nullptr, succeeded, handle, false);
          const auto input = native::native_sdk_present_input;
          expect(input.guides_unavailable && input.identity == identity && input.decision_id == decision
                     && input.owner == &owner && input.route_id == 77 && input.backbuffer == backbuffer.Get()
                     && input.marker == nullptr && input.serial == 0,
                 succeeded ? "successful Restore preserves exact negative declaration"
                           : "failed Restore preserves exact negative declaration without NR proof");
          native::OnNativePresentObserved(output.Get(), 0, Phase::SDK_FORWARD);
          native::NativePresentInput forwarded;
          expect(!native::TakeNativePassthrough(&other_output, nullptr, false, &forwarded),
                 "other output cannot consume missing-guide declaration");
          expect(native::TakeNativePassthrough(identity, nullptr, false, &forwarded)
                     && forwarded.guides_unavailable && forwarded.owner == &owner
                     && forwarded.route_id == 77 && forwarded.decision_id == decision
                     && forwarded.backbuffer == backbuffer.Get() && forwarded.marker == nullptr && forwarded.serial == 0,
                 "SDK_FORWARD delivers current negative declaration to matching output");
          expect(native::native_sdk_present_input.decision_id == 0
                     && !native::TakeNativePassthrough(identity, nullptr), "negative ticket is consumed once");
          parameters.Set(NVSDK_NGX_DLSSG_Parameter_Depth, depth.Get());
          parameters.Set(NVSDK_NGX_DLSSG_Parameter_MVecs, motion.Get());
          parameters.Set(NVSDK_NGX_DLSSG_Parameter_MVecsSubrectHeight, 8u);
          parameters.Set(NVSDK_NGX_DLSSG_Parameter_NotRenderingGameFrames, 0u);
          begin();
          // A failed SDK evaluate invalidates the models; allow their normal
          // warmup instead of requiring mature-model recovery after failure.
          record_base(!succeeded);
          expect(!native::native_sdk_present_input.guides_unavailable
                     && native::native_sdk_present_input.marker != nullptr,
                 "valid guides recover NR in a fresh decision without stale negative state");
        }
      }
      begin();
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_Depth, static_cast<ID3D12Resource*>(nullptr));
      reject_mfg("same decision withdraws guides");
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_Depth, depth.Get());
      record_base(false);
      expect(!native::native_sdk_present_input.guides_unavailable,
             "later valid current guides clear negative declaration in the same decision");
      begin();
      native::native_sdk_present_input.identity = &other_output;
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_Depth, static_cast<ID3D12Resource*>(nullptr));
      reject_mfg("unmatched SDK output withdraws guides");
      expect(native::native_sdk_present_input.identity == &other_output
                 && !native::native_sdk_present_input.guides_unavailable
                 && native::native_sdk_present_input.owner == nullptr,
             "FG owner cannot write a negative declaration into another output scope");
      begin();
      native::present_guides.store(native::kPresentGuidesOptional);
      record_base(false);
      expect(native::native_sdk_present_input.guides_unavailable
                 && native::native_sdk_present_input.marker != nullptr,
             "Optional still runs actual NR while preserving missing-guide declaration");
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_Depth, depth.Get());
      native::present_guides.store(native::kPresentGuidesRequired);
      begin();
      record_base(false);
      std::cout << "PASS negative guide SDK scope on real GPU inputs; Final RunPresentPath not exercised\n";
      native::native_sdk_present_input = {};
      native::native_passthrough_input = {};
      native::native_passthrough_identity = nullptr;
    }
    depth_words.fill(0);
    motion_words.fill(0);
    write_words(depth.Get(), depth_words);
    write_words(motion.Get(), motion_words);
    std::cout << "BEGIN Required supplied-clear guides must retain NR\n";
    record_base(false);
    std::cout << "PASS Required supplied-clear depth/MV reach both NR consumers; no content scan\n";
    native::RetirePresentStream(nullptr);
    native::ForgetSourceHandleLocked(handle);
    if (info != nullptr) {
      for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
        SIZE_T bytes = 0;
        info->GetMessage(i, nullptr, &bytes);
        std::vector<uint8_t> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        Check(SUCCEEDED(info->GetMessage(i, message, &bytes)), "Required debug message");
        Check(message->Severity != D3D12_MESSAGE_SEVERITY_ERROR
                  && message->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION, message->pDescription);
      }
    }
    Check(DetourTransactionBegin() == NO_ERROR, "Required observer detach transaction");
    Check(DetourUpdateThread(GetCurrentThread()) == NO_ERROR, "Required observer detach thread");
    Check(DetourDetach(reinterpret_cast<PVOID*>(&real_barrier), reinterpret_cast<PVOID>(ObserveBarriers)) == NO_ERROR,
          "Required barrier observer detach");
    Check(DetourDetach(reinterpret_cast<PVOID*>(&real_copy), reinterpret_cast<PVOID>(ObserveCopies)) == NO_ERROR,
          "Required copy observer detach");
    Check(DetourTransactionCommit() == NO_ERROR, "Required observer detach commit");
    model_gpu = nullptr;
    std::cout << "NATIVE_REQUIRED_MENU failures=" << failures << '\n';
    Check(failures == 0, "Required MFG policy regressions");
    std::cout << "PASS native Required MFG policy; real NGX model not exercised\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL native Required menu: " << error.what() << '\n';
    return 1;
  }
}

#include "stage_execution_cases.hpp"

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--stage-chain") == 0) return stage_execution_cases::Run();
  if (argc == 2 && std::strcmp(argv[1], "--native-required-menu") == 0) return RunNativeRequiredMenuTest();
  if (argc == 2 && std::strcmp(argv[1], "--native-owner-idle") == 0) return RunNativeOwnerIdleTest();
  if (argc == 2 && std::strcmp(argv[1], "--native-options-resume") == 0) return RunNativeOptionsResumeTest();
  if (argc == 2 && std::strcmp(argv[1], "--native-options-delayed") == 0) return RunNativeOptionsResumeTest(true);
  if (argc == 2 && std::strcmp(argv[1], "--native-guide-contract") == 0) return RunNativeGuideContractTest();
  if (argc == 2 && std::strcmp(argv[1], "--present-guide-models") == 0) return RunPresentGuideModelsTest();
  if (argc == 2 && std::strcmp(argv[1], "--native-present-decision") == 0) return RunNativePresentDecisionTest();
  const bool trace_metadata_only = argc == 2 && std::strcmp(argv[1], "--trace-metadata") == 0;
  try {
    {
      const uint8_t bgra[] = {17, 32, 255, 128};
      float rgba[4]{};
      namespace capture = renodx::addons::dlss5::screenshot::internal;
      Check(capture::DiagnosticPixelBytes(DXGI_FORMAT_B8G8R8A8_TYPELESS) == 4
                && capture::DecodeDiagnosticRow(bgra, 1, DXGI_FORMAT_B8G8R8A8_TYPELESS, rgba),
            "native typeless BGRA UI readback supported");
      Check(rgba[0] == 1.f && rgba[1] == 32.f / 255.f && rgba[2] == 17.f / 255.f
                && rgba[3] == 128.f / 255.f, "native typeless UI stored codes and alpha preserved");

      const uint16_t half_motion[] = {0x3e00, 0xc100, 0x0000, 0x8000, 0x7e00, 0xfe00};
      float decoded[12]{};
      Check(capture::DiagnosticPixelBytes(DXGI_FORMAT_R16G16_FLOAT) == 4
                && capture::DecodeDiagnosticRow(reinterpret_cast<const uint8_t*>(half_motion), 3,
                                                DXGI_FORMAT_R16G16_FLOAT, decoded),
            "native half RG motion readback supported");
      Check(decoded[0] == 1.5f && decoded[1] == -2.5f
                && decoded[4] == 0.f && !std::signbit(decoded[4])
                && decoded[5] == 0.f && std::signbit(decoded[5])
                && std::isnan(decoded[8]) && std::isnan(decoded[9]),
            "native half RG preserves signed values, zero and NaN");
      for (size_t pixel = 0; pixel < 3; ++pixel) {
        Check(decoded[pixel * 4 + 2] == 0.f && decoded[pixel * 4 + 3] == 1.f,
              "native half RG pads blue zero and alpha one");
      }

      const float motion[] = {1.5f, -2.5f, 0.f, -0.f,
                              std::numeric_limits<float>::quiet_NaN(), -std::numeric_limits<float>::quiet_NaN()};
      Check(capture::DiagnosticPixelBytes(DXGI_FORMAT_R32G32_FLOAT) == 8
                && capture::DecodeDiagnosticRow(reinterpret_cast<const uint8_t*>(motion), 3,
                                                DXGI_FORMAT_R32G32_FLOAT, decoded),
            "native float RG motion readback supported");
      for (size_t pixel = 0; pixel < 3; ++pixel) {
        Check(std::memcmp(decoded + pixel * 4, motion + pixel * 2, sizeof(float) * 2) == 0
                  && decoded[pixel * 4 + 2] == 0.f && decoded[pixel * 4 + 3] == 1.f,
              "native float RG preserves value, signed zero and NaN bits with padding");
      }

      const float depth[] = {0.25f, -2.5f, 0.f, -0.f, std::numeric_limits<float>::quiet_NaN()};
      float decoded_depth[20]{};
      for (const auto format : {DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT}) {
        Check(capture::DiagnosticPixelBytes(format) == 4
                  && capture::DecodeDiagnosticRow(reinterpret_cast<const uint8_t*>(depth), 5, format, decoded_depth),
              "native typeless and D32 float depth readback supported");
        for (size_t pixel = 0; pixel < 5; ++pixel) {
          Check(std::memcmp(decoded_depth + pixel * 4, depth + pixel, sizeof(float)) == 0
                    && decoded_depth[pixel * 4 + 1] == 0.f && decoded_depth[pixel * 4 + 2] == 0.f
                    && decoded_depth[pixel * 4 + 3] == 1.f,
                "native depth preserves float bits without color transform");
        }
      }
    }
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    ComPtr<ID3D12Debug> debug;
    const bool debug_available = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
    if (debug_available) debug->EnableDebugLayer();
    std::cout << "D3D12 debug layer: " << (debug_available ? "enabled" : "unavailable") << '\n';
    Gpu gpu;
    model_gpu = &gpu;
    ComPtr<ID3D12InfoQueue> info;
    if (debug_available) Check(SUCCEEDED(gpu.device.As(&info)), "debug info queue");
    // ID3D12GraphicsCommandList::ResourceBarrier, SDK vtable slot 26.
    real_barrier = reinterpret_cast<BarrierFn>((*reinterpret_cast<void***>(gpu.list.Get()))[26]);
    real_copy = reinterpret_cast<CopyFn>((*reinterpret_cast<void***>(gpu.list.Get()))[16]);
    Check(DetourTransactionBegin() == NO_ERROR, "barrier observer transaction");
    Check(DetourUpdateThread(GetCurrentThread()) == NO_ERROR, "barrier observer thread");
    Check(DetourAttach(reinterpret_cast<PVOID*>(&real_barrier), reinterpret_cast<PVOID>(ObserveBarriers)) == NO_ERROR,
          "barrier observer attach");
    Check(DetourAttach(reinterpret_cast<PVOID*>(&real_copy), reinterpret_cast<PVOID>(ObserveCopies)) == NO_ERROR,
          "copy observer attach");
    Check(DetourTransactionCommit() == NO_ERROR, "barrier observer commit");
    native::enabled.store(true);
    native::hooks_enabled.store(true);
    native::hook_point.store(native::kHookPresent);
    native::present_pre_fg.store(1);
    native::present_guides.store(native::kPresentGuidesNever);
    native::stack_passes.store(1);
    native::codec_mode.store(0);
    native::transfer_strength.store(1.f);
    native::color_strength.store(1.f);
    // Adopt a model provider through the existing NGX ABI, using production
    // owned parameters. No Record, writeback, restoration or tracker substitute.
    native::direct_api.module = GetModuleHandleW(nullptr);
    native::direct_api.create = CreateModel;
    native::direct_api.evaluate = EvaluateModel;
    native::direct_api.release = ReleaseModel;
    native::direct_load_state = native::DirectLoadState::Ready;
    native::direct_device = gpu.device.Get();
    native::direct_device->AddRef();
    native::direct_device_native = gpu.device.Get();
    native::parameter_runtime_ready = true;
    native::core_allocate_parameters = renodx::addons::dlss5::bridge::AllocateOwnedParameters;
    native::core_destroy_parameters = renodx::addons::dlss5::bridge::DestroyOwnedParameters;
    Check(native::InstallQueueCompletionHooks(gpu.queue.Get()), "real queue hooks");
    Check(native::InstallCommandListStateHooks(gpu.list.Get()), "real list hooks");
    gpu.Submit();  // Hook-observed Reset seeds the real compute-state envelope.
    if (!trace_metadata_only) funnel_accounting_tests::Run(&gpu);

    {
      namespace trace = renodx::addons::dlss5::norm_trace;
      const auto make_trace_texture = [&] {
        const D3D12_RESOURCE_DESC desc{
            .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D, .Width = 1, .Height = 1,
            .DepthOrArraySize = 1, .MipLevels = 1, .Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
            .SampleDesc = {1, 0}, .Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
        const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
        ComPtr<ID3D12Resource> texture;
        Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                  &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture))),
              "trace texture fixture");
        auto upload = gpu.Buffer(256, D3D12_HEAP_TYPE_UPLOAD);
        float* pixels = nullptr;
        Check(SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&pixels))), "trace upload");
        pixels[0] = 0.25f;
        pixels[1] = 0.125f;
        pixels[2] = pixels[3] = 0.f;
        upload->Unmap(0, nullptr);
        const D3D12_TEXTURE_COPY_LOCATION source{
            .pResource = upload.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
            .PlacedFootprint = {.Footprint = {.Format = desc.Format, .Width = 1, .Height = 1,
                                            .Depth = 1, .RowPitch = 256}}};
        const D3D12_TEXTURE_COPY_LOCATION destination{
            .pResource = texture.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        gpu.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        gpu.Transition(texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        gpu.uploads.push_back(upload);
        return texture;
      };
      auto scale = make_trace_texture();
      auto blocks = make_trace_texture();
      trace::Configure("normtrace-ring.csv");
      const auto before = trace::Published();
      for (uint32_t i = 0; i < trace::kSlots; ++i) {
        trace::Record(gpu.list.Get(), gpu.device.Get(), scale.Get(),
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, blocks.Get(), 1, 1,
                      {.generation = i + 1, .time_ns = static_cast<int64_t>(i + 1) * 1000000,
                       .workset = 77});
      }
      trace::Poll(true);
      Check(trace::Published() == before, "trace must not publish a pending GPU batch");
      const auto dropped_before = trace::Dropped();
      trace::Record(gpu.list.Get(), gpu.device.Get(), scale.Get(),
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, blocks.Get(), 1, 1,
                    {.generation = 999, .time_ns = 999000000, .workset = 999});
      Check(trace::Dropped() == dropped_before + 1, "full trace ring skips the new sample");
      gpu.Submit();
      trace::Poll(true);
      std::cout << "Trace full-ring evidence: published=" << trace::Published() - before
                << " expected=" << trace::kSlots << '\n';
      Check(trace::Published() - before == trace::kSlots,
            "trace must drain a full ring after exact GPU completion");
      trace::Record(gpu.list.Get(), gpu.device.Get(), scale.Get(),
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, blocks.Get(), 1, 1,
                    {.generation = 17, .time_ns = 9007199254740993LL, .workset = 77,
                     .present_domain = renodx::addons::dlss5::present_restore_scope::Domain::PRE_FG_BACKBUFFER,
                     .chain_reset_epoch = 9007199254740995ULL, .last_model_pass_reset = true});
      gpu.Submit();
      trace::Poll(true);
      Check(trace::Published() - before == trace::kSlots + 1,
            "trace resumes after the full batch drains");
      trace::Poll(true);
      Check(trace::Published() - before == trace::kSlots + 1,
            "empty trace ring does not publish the completed batch twice");
      trace::ReleaseAll();
      std::cout << "PASS normalization trace drains full completed GPU ring\n";
      if (trace_metadata_only) {
        namespace edit = renodx::addons::dlss5::edit_trace;
        const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_RESOURCE_DESC desc{
            .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Width = edit::kHistogramBytes,
            .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1, .SampleDesc = {1, 0},
            .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR, .Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
        ComPtr<ID3D12Resource> histogram;
        Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                  &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&histogram))),
              "edit histogram GPU fixture");
        auto upload = gpu.Buffer(edit::kHistogramBytes, D3D12_HEAP_TYPE_UPLOAD);
        uint32_t* words = nullptr;
        Check(SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&words))), "edit histogram upload");
        std::fill(words, words + renodx::addons::dlss5::look::kTraceWords, 0u);
        words[64] = words[edit::kWordPixels] = 1;
        upload->Unmap(0, nullptr);
        gpu.list->CopyBufferRegion(histogram.Get(), 0, upload.Get(), 0, edit::kHistogramBytes);
        gpu.Transition(histogram.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        gpu.uploads.push_back(upload);
        edit::Configure("edittrace-ring.csv");
        const auto published = edit::Published();
        for (uint32_t i = 0; i < edit::kSlots; ++i) {
          edit::Record(gpu.list.Get(), gpu.device.Get(), histogram.Get(), blocks.Get(),
                       {.generation = i + 1, .time_ns = 9007199254740993LL + i,
                        .workset = 78, .pass = 1, .nr_width = 1, .nr_height = 1});
        }
        const auto oldest = edit::internal::slots[edit::internal::tail].meta.generation;
        const auto dropped = edit::Dropped();
        edit::Record(gpu.list.Get(), gpu.device.Get(), histogram.Get(), blocks.Get(),
                     {.generation = 999, .time_ns = 9007199254741993LL,
                      .workset = 999, .pass = 1, .nr_width = 1, .nr_height = 1});
        Check(edit::Dropped() == dropped + 1
                  && edit::internal::slots[edit::internal::tail].meta.generation == oldest,
              "full edit ring must preserve GPU-owned sample instead of overwriting it");
        edit::Poll(true);
        Check(edit::Published() == published, "edit trace cannot publish pending GPU work");
        gpu.Submit();
        edit::Poll(true);
        Check(edit::Published() == published + edit::kSlots, "full edit ring drains completed GPU batch");
        edit::Poll(true);
        Check(edit::Published() == published + edit::kSlots, "edit batch publishes only once");
        edit::ReleaseAll();
        std::ifstream csv("edittrace-ring.csv");
        std::string header, row, last;
        Check(static_cast<bool>(std::getline(csv, header)) && header.ends_with(",time_ns"), "edit CSV time column");
        while (std::getline(csv, row)) if (!row.empty()) last = row;
        Check(std::count(last.begin(), last.end(), ',') == 16
                  && last.ends_with("," + std::to_string(9007199254740993LL + edit::kSlots - 1)),
              "edit CSV exact absolute time after GPU completion");
        std::cout << "PASS edit trace full ring preserves pending copies and serializes exact time\n";
      }
    }

    if (trace_metadata_only) {
      std::ifstream csv("normtrace-ring.csv");
      std::string header, row, last_row;
      Check(static_cast<bool>(std::getline(csv, header)), "trace CSV header exists");
      Check(header.ends_with(
                ",time_ns,present_domain,chain_reset_epoch,last_model_pass_reset"),
            "trace CSV appends alignment fields");
      while (std::getline(csv, row)) if (!row.empty()) last_row = row;
      Check(std::count(last_row.begin(), last_row.end(), ',') == 18,
            "trace CSV field count matches header");
      Check(last_row.ends_with(
                ",9007199254740993,pre_fg_backbuffer,9007199254740995,1"),
            "trace CSV preserves exact nanoseconds, domain, reset epoch and actual reset");
      std::cout << "PASS GPU-completed trace serializes exact alignment metadata\n";
      return 0;
    }

    auto backbuffer = gpu.Texture();
    auto hudless = gpu.Texture();
    game_inputs[0] = backbuffer.Get();
    game_inputs[1] = hudless.Get();
    observed_states[backbuffer.Get()] = observed_states[hudless.Get()] = D3D12_RESOURCE_STATE_COMMON;
    const auto expect_native_inputs = [&](ID3D12Resource* bound_hudless) {
      for (auto* input : {backbuffer.Get(), hudless.Get(), bound_hudless}) {
        if (input == nullptr) continue;
        const auto state = observed_states.find(input);
        if (state == observed_states.end() || state->second != NATIVE_INPUT_STATE) {
          ++state_errors;
          std::cerr << "STATE ERROR FG boundary input=" << input << " expected=NPS(64) observed="
                    << (state == observed_states.end() ? -1 : static_cast<int>(state->second)) << '\n';
        }
      }
    };
    renodx::addons::dlss5::bridge::OwnedParameters parameters;
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_Backbuffer, backbuffer.Get());
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_HUDLess, hudless.Get());
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 1u);
    auto& owner = native::present_streams[{}];
    owner.device_identity = renodx::utils::directx::NativeIdentity(gpu.device.Get());
    owner.backbuffer_width = owner.backbuffer_height = 8;
    owner.backbuffer_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    owner.prefg_target_valid = true;
    owner.prefg_target.diagnostic.bypass_host_look = true;
    owner.prefg_target.ui_correction = false;
    owner.prefg_target.pass_transfer_strength[0] = 1.f;
    owner.prefg_target.pass_color_strength[0] = 1.f;

    const auto seed = [&] {
      gpu.Write(backbuffer.Get(), 64);
      gpu.Write(hudless.Get(), 96);
      gpu.Transition(backbuffer.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
      gpu.Transition(hudless.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
    };
    const auto pixels = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES state) {
      const auto desc = resource->GetDesc();
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
      uint64_t bytes = 0;
      gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
      auto readback = gpu.Buffer(bytes, D3D12_HEAP_TYPE_READBACK);
      gpu.Transition(resource, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION from{.pResource = resource,
          .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0};
      D3D12_TEXTURE_COPY_LOCATION to{.pResource = readback.Get(),
          .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = footprint};
      gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
      gpu.Transition(resource, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
      gpu.Submit();
      uint8_t* mapped = nullptr;
      Check(SUCCEEDED(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped))), "native readback map");
      std::vector<uint8_t> result;
      for (uint32_t y = 0; y < 8; ++y) {
        const auto* row = mapped + footprint.Offset + y * footprint.Footprint.RowPitch;
        result.insert(result.end(), row, row + 32);
      }
      readback->Unmap(0, nullptr);
      return result;
    };

    seed();
    const auto submitted_before_warmup = native::native_fg_submitted_frames.load();
    Check(native::RunNativePreFg(gpu.list.Get(), &parameters) == nullptr, "first model creation has no output");
    auto& stream = native::fg_service::streams.at({true, reinterpret_cast<uintptr_t>(&parameters)});
    Check(hudless_snapshot_copies == 0, "no HUDLess snapshot without a UI composition consumer");
    Check(stream.result == fg::Result::PREPARED && model_calls == 0, "warmup prepared without model evaluation");
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 2u);
    Check(native::RunNativePreFg(gpu.list.Get(), &parameters) == nullptr
              && native::native_fg_result.load() == fg::Result::PREPARED && model_calls == 0,
          "MFG during base-frame warmup waits for an application result without a false failure");
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 1u);
    Check(native::native_fg_result.load() == fg::Result::PREPARED,
          "create-only warmup publishes PREPARED rather than NOT_SERVED");
    Check(native::native_fg_submitted_frames.load() == submitted_before_warmup
              && native::native_fg_marker.load() == nullptr && stream.native_serial == 0,
          "create-only warmup fabricates neither submitted frames nor a submission marker");
    {
      namespace ui = renodx::addons::dlss5::ui;
      const auto previous_owns = native::native_fg_owns.exchange(true);
      const auto previous_creates = native::intercepted_creates.exchange(1);
      const auto previous_present = native::d3d12_present_seen.exchange(true);
      const auto inputs = native::CollectFunnelInputs();
      const auto verdict = renodx::addons::dlss5::ComputeNrVerdict(inputs);
      const auto card = ui::ComputeStatusCard(
          native::CollectCardInputs(verdict, native::SteadyNowNs()));
      native::native_fg_owns.store(previous_owns);
      native::intercepted_creates.store(previous_creates);
      native::d3d12_present_seen.store(previous_present);
      Check(verdict.IsFgPreparing() && inputs.injected == 0 && inputs.submitted_ok == 0,
            "create-only warmup funnel reports preparation without submission proof");
      Check(card.tone == ui::CardTone::kIdle && card.glyph == ui::CardGlyph::kWaiting
                && !ui::ShowsOnHud(card),
            "create-only warmup card is neutral rather than a service failure");
    }
    std::vector<std::pair<renodx::addons::dlss5::FenceRef, uint64_t>> tokens;
    Check(submission::UseSubmissionTokens(stream.slots[0].color[0], &tokens)
              == submission::UseSubmission::kUnsubmitted,
          "PREPARED recording has no submission proof");
    Check(pixels(backbuffer.Get(), NATIVE_INPUT_STATE) == std::vector<uint8_t>(256, 64),
          "warmup leaves game backbuffer intact after actual execution");
    Check(pixels(hudless.Get(), NATIVE_INPUT_STATE) == std::vector<uint8_t>(256, 96),
          "warmup leaves game HUDLess intact");
    std::cout << "PASS PREPARED is not SUBMITTED; real execute/readback preserves warmup inputs\n";

    // The HUDLess feature matures first; the next application frame creates
    // the backbuffer feature. Neither warmup is allowed to commit one side.
    native::RunNativePreFg(gpu.list.Get(), &parameters);
    gpu.Submit();
    fail_call = model_calls + 2;
    Check(native::RunNativePreFg(gpu.list.Get(), &parameters) == nullptr, "second role model fails");
    Check(model_calls == fail_call, "failure occurs after successful HUDLess evaluation");
    Check(stream.result == fg::Result::FAILED && native::native_fg_result.load() == fg::Result::FAILED,
          "actual model failure publishes FAILED rather than preparation or NOT_SERVED");
    Check(native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_HUDLess) == hudless.Get(),
          "partial failure keeps original HUDLess binding");
    Check(pixels(backbuffer.Get(), NATIVE_INPUT_STATE) == std::vector<uint8_t>(256, 64),
          "partial failure does not commit backbuffer pixels");
    Check(pixels(hudless.Get(), NATIVE_INPUT_STATE) == std::vector<uint8_t>(256, 96),
          "partial failure does not commit successful side pixels");
    std::cout << "PASS partial model failure preserves both game surfaces and HUDLess binding\n";

    fail_call = 0;
    ID3D12Resource* original = nullptr;
    for (uint32_t frame = 0; frame < 4 && original == nullptr; ++frame) {
      original = native::RunNativePreFg(gpu.list.Get(), &parameters);
      if (original == nullptr) gpu.Submit();
    }
    Check(original == hudless.Get(), "mature native transaction succeeds");
    // Ownership normally comes from the NGX dispatch. Keep the real native
    // recording/queue hooks as the only source of PREPARED/SUBMITTED here.
    native::native_fg_owns.store(true);
    native::intercepted_creates.store(1);
    native::d3d12_present_seen.store(true);
    native::emitted_any_verdict.store(false);
    const auto submit_native = [&] {
      // Gpu::Submit also reserves synthetic host proofs before Execute.
      // This lane lets the installed production queue hook own all proofs.
      Check(SUCCEEDED(gpu.list->Close()), "native close");
      ID3D12CommandList* lists[]{gpu.list.Get()};
      gpu.queue->ExecuteCommandLists(1, lists);
      Check(SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial)), "native signal");
      HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      Check(event != nullptr, "native completion event");
      const auto hr = gpu.fence->SetEventOnCompletion(gpu.serial, event);
      const auto wait = (SUCCEEDED(hr) ? WaitForSingleObject(event, 10000) : WAIT_FAILED);
      CloseHandle(event);
      Check(wait == WAIT_OBJECT_0, "native bounded completion");
      Check(SUCCEEDED(gpu.allocator->Reset()), "native allocator reset");
      Check(SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)), "native list reset");
      submission::PruneCompletedGenerations();
      gpu.uploads.clear();
    };
    const auto verdict_lines = [] {
      return std::count_if(captured_logs.begin(), captured_logs.end(), [](const auto& entry) {
        return entry.second.find("NR-VERDICT ") != std::string::npos;
      });
    };
    const auto check_preparing = [&] {
      const auto verdict = native::MaybeEmitNrVerdict();
      Check(verdict.state == renodx::addons::dlss5::NrState::kNotEngaged
                && std::strcmp(verdict.ReasonName(), "fg_preparing") == 0
                && verdict.injected == 0,
            "PREPARED cannot claim engagement or submitted work");
      const auto card = renodx::addons::dlss5::ui::ComputeStatusCard(
          {.verdict = verdict, .active_features = 1, .present = true});
      Check(card.tone == renodx::addons::dlss5::ui::CardTone::kIdle
                && card.glyph == renodx::addons::dlss5::ui::CardGlyph::kWaiting
                && std::strcmp(card.title, "Preparing Neural Rendering") == 0,
            "PREPARED must show neutral waiting-for-submission card");
    };
    captured_logs.clear();
    check_preparing();
    const auto pending_ui_verdict = renodx::addons::dlss5::ComputeNrVerdict(native::CollectFunnelInputs());
    Check(verdict_lines() == 1, "initial PREPARED verdict remains visible");
    Check(captured_logs.back().first == static_cast<int>(reshade::log::level::info),
          "PREPARED is informational, not a warning");
    native::RestoreNativePreFg(gpu.list.Get(), &parameters, original, true);
    submit_native();
    Check(native::native_fg_result.load() == fg::Result::SUBMITTED,
          "real queue execution is required for SUBMITTED");
    Check(native::MaybeEmitNrVerdict().state == renodx::addons::dlss5::NrState::kEngaged,
          "submission proof enables ENGAGED");
    {
      funnel_accounting_tests::Restore restore;
      restore.Atomic(&native::native_fg_owns);
      restore.Atomic(&native::synthetic_tick_active);
      restore.Atomic(&native::present_unpresented_evaluates);
      restore.Atomic(&native::present_unusable);
      restore.Atomic(&native::present_prefg_upscaled_active);
      restore.Atomic(&native::present_starved_fallbacks);
      restore.Atomic(&native::present_format_fallbacks);
      restore.Value(&native::present_inline_fallback_pending);
      restore.Value(&native::present_fallback_episode);
      restore.Value(&captured_logs);
      native::native_fg_owns.store(true);
      native::synthetic_tick_active.store(true);
      native::present_unpresented_evaluates.store(native::kPresentStarvedEvaluates);
      native::present_inline_fallback_pending = false;
      Check(native::PresentTakesEvaluate() && !native::present_inline_fallback_pending,
            "native FG ownership must keep starved SR evaluates out of Upscaled NR fallback");
      native::present_unusable.store(true);
      Check(native::PresentTakesEvaluate() && !native::present_inline_fallback_pending,
            "native FG ownership must not depend on final-Present backbuffer usability");
      native::present_unusable.store(false);
      native::native_fg_owns.store(false);
      Check(!native::PresentTakesEvaluate() && native::present_inline_fallback_pending,
            "ordinary starved final-Present integration retains its existing fallback");
      std::cout << "PASS native FG ownership prevents alternate Upscaled NR during Present starvation\n";
    }
    Check(verdict_lines() == 2 && captured_logs.back().second.find("state=ENGAGED") != std::string::npos,
          "first proven submission remains immediately visible before any interval snapshot");
    native::EmitNrVerdict("interval");
    Check(captured_logs.back().second.find("state=ENGAGED") != std::string::npos,
          "periodic snapshot reports current submission proof");
    const auto normal_lines = verdict_lines();
    unsigned ui_recording_samples = 0;
    bool ui_recording_active = true;
    model_ui_observer = [&] {
      namespace ui = renodx::addons::dlss5::ui;
      const auto fresh_verdict = renodx::addons::dlss5::ComputeNrVerdict(native::CollectFunnelInputs());
      Check(fresh_verdict.IsFgPreparing() && fresh_verdict.injected == 0,
            "fresh verdict during mature model recording is preparing, never transient fg_unserved");
      // The overlay reads its verdict before the queue submits, then builds
      // the card while the next model is recording on another game thread.
      const auto inputs = native::CollectCardInputs(pending_ui_verdict, native::SteadyNowNs());
      ui_recording_active &= inputs.fg_continuing_submission
          && ui::ComputeStatusCard(inputs).id == ui::CardId::kActive;
      ++ui_recording_samples;
    };
    for (uint32_t frame = 0; frame < 4; ++frame) {
      Check(native::RunNativePreFg(gpu.list.Get(), &parameters) == hudless.Get(), "next native recording");
      check_preparing();
      {
        namespace ui = renodx::addons::dlss5::ui;
        const auto verdict = renodx::addons::dlss5::ComputeNrVerdict(native::CollectFunnelInputs());
        const auto inputs = native::CollectCardInputs(verdict, native::SteadyNowNs());
        Check(inputs.fg_continuing_submission && ui::ComputeStatusCard(inputs).id == ui::CardId::kActive,
              "native production card stays active while the adjacent proven stream records");
        const auto concurrent = native::CollectCardInputs(
            verdict, native::native_fg_last_submission_ns.load() - 1);
        Check(concurrent.fg_continuing_submission
                  && ui::ComputeStatusCard(concurrent).id == ui::CardId::kActive,
              "a queue publication newer than the UI tick cannot flash a pending card");
        const auto stalled = native::CollectCardInputs(verdict, native::SteadyNowNs() + 2'000'000'000LL);
        Check(!stalled.fg_continuing_submission
                  && ui::ComputeStatusCard(stalled).id == ui::CardId::kFgPreparing,
              "native card expires continuity when the next recording never submits");
        const auto epoch = native::native_fg_display_epoch.fetch_add(1);
        const auto recreated = native::CollectCardInputs(verdict, native::SteadyNowNs());
        Check(!recreated.fg_continuing_submission
                  && ui::ComputeStatusCard(recreated).id == ui::CardId::kFgPreparing,
              "native card cannot carry service continuity across a history epoch");
        native::native_fg_display_epoch.store(epoch);
      }
      native::RestoreNativePreFg(gpu.list.Get(), &parameters, original, true);
      submit_native();
      Check(native::MaybeEmitNrVerdict().state == renodx::addons::dlss5::NrState::kEngaged,
            "each real submission engages");
    }
    model_ui_observer = {};
    Check(ui_recording_samples == 8, "Home samples cover both model roles during four real recordings");
    Check(ui_recording_active,
          "concurrent Home read during model recording must keep the proven service active");
    Check(verdict_lines() == normal_lines, "normal PREPARED/SUBMITTED cycles do not emit change lines");
    {
      const auto legacy_gaps = native::present_gaps.exchange(6864);
      const auto observed = native::CollectFunnelInputs();
      const auto inputs = native::CollectCardInputs(
          renodx::addons::dlss5::ComputeNrVerdict(observed), native::SteadyNowNs());
      Check(inputs.present_gaps == 0 && inputs.present_before_fg,
            "native pre-FG card must not report final-Present bypasses as unenhanced display gaps");
      Check(inputs.fg_submitted_frames.has_value() && *inputs.fg_submitted_frames >= 5
                && observed.injected == 1,
            "native card needs cumulative proven frame submissions, not the current 0/1 handoff");
      native::native_fg_owns.store(false);
      Check(native::CollectCardInputs(
                renodx::addons::dlss5::ComputeNrVerdict(native::CollectFunnelInputs()), native::SteadyNowNs())
                .present_gaps == 6864,
            "ordinary final-Present gap evidence must remain visible on its own route");
      native::native_fg_owns.store(true);
      native::present_gaps.store(legacy_gaps);
    }
    // Drive the production card/HUD tick through a healthy cycle and a real
    // failure. A neutral recording must neither re-arm nor lose recovery.
    native::hud_pill.store(false);
    native::hud_toast_until_ns.store(0);
    auto hud_now = native::SteadyNowNs();
    {
      Check(native::RunNativePreFg(gpu.list.Get(), &parameters) == hudless.Get(),
            "initial toast regression starts during mature native recording");
      const auto recording_verdict = native::MaybeEmitNrVerdict();
      Check(recording_verdict.IsFgPreparing()
                && native::CollectCardInputs(recording_verdict, hud_now).fg_continuing_submission,
            "initial toast regression has a current-stream submission proof");
      native::MaybeEmitStatusCard(recording_verdict, hud_now);
      Check(native::hud_toast_until_ns.load() == hud_now + native::kHudToastNs,
            "first proven native service toasts even when the UI samples the next recording");
      native::RestoreNativePreFg(gpu.list.Get(), &parameters, original, true);
      submit_native();
      native::hud_toast_until_ns.store(0);
    }
    auto hud_verdict = native::MaybeEmitNrVerdict();
    native::MaybeEmitStatusCard(hud_verdict, hud_now);
    const auto first_toast = native::hud_toast_until_ns.load();
    Check(first_toast == hud_now + native::kHudToastNs, "first real engagement toasts once");
    Check(native::RunNativePreFg(gpu.list.Get(), &parameters) == hudless.Get(), "HUD pending recording");
    hud_verdict = native::MaybeEmitNrVerdict();
    native::MaybeEmitStatusCard(hud_verdict, ++hud_now);
    Check(!native::hud_pill.load() && native::hud_toast_until_ns.load() == first_toast,
          "preparing is neutral and does not restart the active toast");
    native::RestoreNativePreFg(gpu.list.Get(), &parameters, original, true);
    submit_native();
    hud_verdict = native::MaybeEmitNrVerdict();
    native::MaybeEmitStatusCard(hud_verdict, ++hud_now);
    Check(native::hud_toast_until_ns.load() == first_toast, "normal handoff does not repeat toast");
    auto hud_failure = hud_verdict;
    hud_failure.state = renodx::addons::dlss5::NrState::kNotEngaged;
    hud_failure.stage = renodx::addons::dlss5::FunnelStage::kInjected;
    hud_failure.reason = "fg_handoff_failed";
    native::MaybeEmitStatusCard(hud_failure, ++hud_now);
    Check(native::hud_pill.load(), "actual service failure remains visible on HUD");
    // Production invalidates continuity on a failed native handoff. The
    // following neutral preparation has no current-stream submission proof.
    native::native_fg_continuing_submission.store(false);
    auto hud_preparing = hud_failure;
    hud_preparing.reason = "fg_preparing";
    native::MaybeEmitStatusCard(hud_preparing, ++hud_now);
    Check(!native::hud_pill.load(), "preparation after failure is neutral");
    Check(native::hud_recovery_pending && native::hud_toast_until_ns.load() == first_toast,
          "a pending card cannot consume failure recovery before current submission");
    Check(native::RunNativePreFg(gpu.list.Get(), &parameters) == hudless.Get(),
          "failure recovery records a new native handoff before toasting");
    native::RestoreNativePreFg(gpu.list.Get(), &parameters, original, true);
    submit_native();
    hud_verdict = native::MaybeEmitNrVerdict();
    native::MaybeEmitStatusCard(hud_verdict, ++hud_now);
    Check(native::hud_toast_until_ns.load() == hud_now + native::kHudToastNs,
          "real failure recovery survives intermediate preparation and toasts once");
    std::cout << "PASS production HUD healthy handoff and failure/preparing/recovery toast lifecycle\n";
    {
      auto off_verdict = hud_verdict;
      off_verdict.state = renodx::addons::dlss5::NrState::kOff;
      native::MaybeEmitStatusCard(off_verdict, ++hud_now);
      Check(native::hud_toast_until_ns.load() == 0 && !native::hud_recovery_pending,
            "turning NR off clears the previous activation toast");
      native::native_fg_continuing_submission.store(false);
      native::MaybeEmitStatusCard(hud_preparing, ++hud_now);
      Check(native::hud_toast_until_ns.load() == 0 && !native::hud_pill.load(),
            "re-enabling NR before its first submission shows neither success nor a false warning");
      Check(native::RunNativePreFg(gpu.list.Get(), &parameters) == hudless.Get(),
            "NR re-enable toast records a fresh handoff");
      native::RestoreNativePreFg(gpu.list.Get(), &parameters, original, true);
      submit_native();
      Check(native::RunNativePreFg(gpu.list.Get(), &parameters) == hudless.Get(),
            "NR re-enable toast samples the following native recording");
      native::MaybeEmitStatusCard(native::MaybeEmitNrVerdict(), ++hud_now);
      Check(native::hud_toast_until_ns.load() == hud_now + native::kHudToastNs,
            "NR re-enable toasts from fresh submission proof during the following recording");
      native::RestoreNativePreFg(gpu.list.Get(), &parameters, original, true);
      submit_native();
      hud_verdict = native::MaybeEmitNrVerdict();
      std::cout << "PASS native activation toast at Present sampling and NR off/on\n";
    }
    {
      namespace ui = renodx::addons::dlss5::ui;
      native::hud_enabled.store(false);
      native::MaybeEmitStatusCard(hud_verdict, ++hud_now);
      Check(!native::hud_pill.load() && native::hud_toast_until_ns.load() == 0,
            "disabled status HUD clears the active green toast and arms no pill");
      Check(ui::ComputeStatusCard(native::CollectCardInputs(hud_verdict, hud_now)).tone
                == ui::CardTone::kGood,
            "disabled status HUD keeps the healthy Home card truthful");
      // A callback already queued before disabling must return without ImGui,
      // even with stale HUD work still armed. This host has no ImGui context.
      Check(!native::hud_overlay_open.load(), "HUD switch regression runs with Home closed");
      native::hud_pill.store(true);
      native::hud_toast_until_ns.store(native::SteadyNowNs() + native::kHudToastNs);
      native::OnHudOverlay(nullptr);
      native::MaybeEmitStatusCard(hud_failure, ++hud_now);
      Check(!native::hud_pill.load() && native::hud_toast_until_ns.load() == 0,
            "disabled status HUD arms neither failure pill nor green toast");
      const auto disabled_failure_card =
          ui::ComputeStatusCard(native::CollectCardInputs(hud_failure, hud_now));
      Check(ui::ShowsOnHud(disabled_failure_card),
            "disabled status HUD keeps the actual failed Home card warning-visible");
      native::OnHudOverlay(nullptr);

      native::hud_enabled.store(true);
      native::MaybeEmitStatusCard(hud_failure, ++hud_now);
      Check(native::hud_pill.load(),
            "re-enabled status HUD shows the actual service failure warning");
      Check(native::RunNativePreFg(gpu.list.Get(), &parameters) == hudless.Get(),
            "re-enabled status HUD recovery records real NR work");
      native::RestoreNativePreFg(gpu.list.Get(), &parameters, original, true);
      submit_native();
      hud_verdict = native::MaybeEmitNrVerdict();
      Check(hud_verdict.state == renodx::addons::dlss5::NrState::kEngaged,
            "re-enabled status HUD recovery has a real submitted verdict");
      native::MaybeEmitStatusCard(hud_verdict, ++hud_now);
      Check(!native::hud_pill.load()
                && native::hud_toast_until_ns.load() == hud_now + native::kHudToastNs,
            "re-enabled status HUD toasts again after real failure recovery");
      std::cout << "PASS status HUD enable switch suppresses all HUD output and permits recovery\n";
    }
    {
      namespace ui = renodx::addons::dlss5::ui;
      char active_detail[256];
      char pending_detail[256];
      const auto submitted_card = ui::ComputeStatusCard(
          {.verdict = hud_verdict, .active_features = 1, .present = true, .fg_submitted_frames = 1234});
      const auto pending_card = ui::ComputeStatusCard(
          {.verdict = hud_preparing, .active_features = 1, .present = true, .fg_submitted_frames = 1234});
      ui::FormatCardDetail(active_detail, sizeof(active_detail), submitted_card);
      ui::FormatCardDetail(pending_detail, sizeof(pending_detail), pending_card);
      Check(std::strcmp(active_detail, "1234 NR frames submitted before FG") == 0
                && std::strcmp(active_detail, pending_detail) == 0,
            "current 0/1 verdict cannot replace cumulative submissions or remove pending detail row");
      Check(pending_card.tone == ui::CardTone::kIdle && !ui::ShowsOnHud(pending_card),
            "historical submissions do not make current preparation successful");
      const auto continuing_card = ui::ComputeStatusCard(
          {.verdict = hud_preparing, .active_features = 1, .present = true,
              .fg_submitted_frames = 1234, .fg_continuing_submission = true});
      Check(continuing_card.id == submitted_card.id
                && continuing_card.tone == submitted_card.tone
                && std::strcmp(continuing_card.title, submitted_card.title) == 0
                && std::strcmp(continuing_card.body, submitted_card.body) == 0,
            "continuous recording must not flash a waiting card between submitted frames");
      const auto failed_card = ui::ComputeStatusCard(
          {.verdict = hud_failure, .starting = true, .fg_continuing_submission = true});
      Check(ui::ShowsOnHud(failed_card), "startup grace cannot hide explicit service failure");
      auto unserved_verdict = hud_failure;
      unserved_verdict.reason = "fg_unserved";
      Check(ui::ShowsOnHud(ui::ComputeStatusCard(
                {.verdict = unserved_verdict, .starting = true, .fg_continuing_submission = true})),
            "startup grace cannot hide final NOT_SERVED");
      auto off = hud_preparing;
      off.state = renodx::addons::dlss5::NrState::kOff;
      const auto off_card = ui::ComputeStatusCard(
          {.verdict = off, .fg_submitted_frames = 1234, .fg_continuing_submission = true});
      Check(off_card.id == ui::CardId::kOffByUser, "historical submissions cannot hide Off");
      Check(native::BuildSupportReport().find("fg_submitted=") != std::string::npos,
            "support report distinguishes real FG submissions from model evaluations");
      // Select the real XeFG observation path, without fabricating a service
      // submission: observing phases must never manufacture session counts.
      xefg::Configure(nullptr, nullptr, nullptr, &Requested);
      xefg::internal::owns_stream.store(true);
      const auto submitted_count = xefg::SubmittedFrames();
      for (auto phase : {fg::Result::PREPARED, fg::Result::SUBMITTED, fg::Result::FAILED, fg::Result::NOT_SERVED}) {
        xefg::internal::result.store(phase);
        const auto observed = native::CollectFunnelInputs();
        const auto verdict = renodx::addons::dlss5::ComputeNrVerdict(observed);
        const auto inputs = native::CollectCardInputs(verdict, hud_now);
        Check(observed.injected == (phase == fg::Result::SUBMITTED ? 1u : 0u)
                  && inputs.fg_submitted_frames == submitted_count,
              "real XeFG UI source keeps current result separate from submitted session count");
      }
      xefg::internal::owns_stream.store(false);
      xefg::Configure(nullptr, nullptr, nullptr, nullptr);
      std::cout << "PASS FG cumulative submission label, stable detail row, pending/failure/Off truth\n";
    }
    Check(std::none_of(captured_logs.begin(), captured_logs.end(), [](const auto& entry) {
      return entry.second.find("NR-VERDICT ") != std::string::npos
          && entry.first == static_cast<int>(reshade::log::level::warning);
    }), "normal handoff phases never emit warning verdicts");
    // Repeated reads of an unsubmitted recording must stay pending, even
    // after successful prior frames and a periodic snapshot.
    Check(native::RunNativePreFg(gpu.list.Get(), &parameters) == hudless.Get(), "pending recording");
    check_preparing();
    native::EmitNrVerdict("interval");
    Check(captured_logs.back().second.find("reason=fg_preparing") != std::string::npos,
          "periodic snapshot reports actual pending state");
    native::nr_enabled_since_ns = native::SteadyNowNs() - native::kVerdictSettleNs - 1;
    native::emitted_settled_verdict = false;
    check_preparing();
    Check(captured_logs.back().second.find("trigger=settled") != std::string::npos,
          "settled snapshot remains visible during pending recording");
    for (uint32_t poll = 0; poll < 4; ++poll) check_preparing();
    native::RestoreNativePreFg(gpu.list.Get(), &parameters, original, false);
    auto before_failure = verdict_lines();
    auto failed = native::MaybeEmitNrVerdict();
    Check(std::strcmp(failed.ReasonName(), "fg_handoff_failed") == 0
              && failed.state == renodx::addons::dlss5::NrState::kNotEngaged
              && verdict_lines() == before_failure + 1
              && captured_logs.back().first == static_cast<int>(reshade::log::level::warning),
          "failed handoff is visible immediately as a warning");
    native::enabled.store(false);
    Check(native::MaybeEmitNrVerdict().state == renodx::addons::dlss5::NrState::kOff
              && verdict_lines() == before_failure + 2,
          "Off remains immediately visible");
    native::enabled.store(true);
    submit_native();
    const auto submitted_before_recovery_warmup = native::native_fg_submitted_frames.load();
    Check(native::RunNativePreFg(gpu.list.Get(), &parameters) == nullptr, "failed stream restarts with warmup");
    Check(native::native_fg_result.load() == fg::Result::PREPARED
              && native::native_fg_submitted_frames.load() == submitted_before_recovery_warmup,
          "re-enable warmup is preparation without a new native submission");
    Check(native::MaybeEmitNrVerdict().IsFgPreparing()
              && verdict_lines() == before_failure + 3,
          "re-enable recovery waits neutrally for its own first submission");
    Check(native::RunNativePreFg(nullptr, &parameters) == nullptr
              && native::native_fg_result.load() == fg::Result::NOT_SERVED,
          "an actual rejected recording remains NOT_SERVED during recovery");
    auto unserved = native::MaybeEmitNrVerdict();
    Check(std::strcmp(unserved.ReasonName(), "fg_unserved") == 0
              && unserved.state == renodx::addons::dlss5::NrState::kNotEngaged
              && verdict_lines() == before_failure + 4,
          "NOT_SERVED remains immediately visible after earlier submissions");
    submit_native();
    original = nullptr;
    for (uint32_t frame = 0; frame < 4 && original == nullptr; ++frame) {
      original = native::RunNativePreFg(gpu.list.Get(), &parameters);
      if (original == nullptr) submit_native();
    }
    Check(original == hudless.Get(), "native transaction recovers after diagnostic checks");
    std::cout << "PASS native verdict/card pending, real submission, cycle dedupe, interval/settled, failure/Off/unserved\n";
    auto* replacement = native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_HUDLess);
    Check(replacement != nullptr && replacement != hudless.Get(), "real parameter getter sees private HUDLess");
    expect_native_inputs(replacement);
    native::RestoreNativePreFg(gpu.list.Get(), &parameters, original, true);
    expect_native_inputs(replacement);
    Check(native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_HUDLess) == hudless.Get(),
          "restore returns original binding");
    const auto enhanced = pixels(backbuffer.Get(), NATIVE_INPUT_STATE);
    Check(enhanced != std::vector<uint8_t>(256, 64), "successful model changes game pixels");
    for (size_t i = 0; i < enhanced.size(); ++i) {
      Check(enhanced[i] == (i % 4 == 3 ? 64 : 0), "deterministic black model output with original alpha");
    }
    Check(pixels(hudless.Get(), NATIVE_INPUT_STATE) == std::vector<uint8_t>(256, 96),
          "HUDLess source pixels preserved after successful restore");
    const auto evaluations = model_calls;
    const auto submitted_before_mfg = native::native_fg_submitted_frames.load();
    const auto serial_before_mfg = native::native_fg_recording_serial.load();
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 2u);
    Check(native::RunNativePreFg(gpu.list.Get(), &parameters) == hudless.Get(), "MFG reuses application frame");
    Check(model_calls == evaluations, "MFG does not evaluate models again");
    Check(native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_HUDLess) == replacement,
          "MFG rebinds same private result");
    expect_native_inputs(replacement);
    native::RestoreNativePreFg(gpu.list.Get(), &parameters, hudless.Get(), true);
    expect_native_inputs(replacement);
    Check(pixels(backbuffer.Get(), NATIVE_INPUT_STATE) == enhanced, "MFG real copy repeats cached pixels");
    submission::PruneCompletedGenerations();
    Check(native::native_fg_recording_serial.load() == serial_before_mfg
              && native::native_fg_submitted_frames.load() == submitted_before_mfg,
          "MFG copies and retirement proofs must not count the same application NR frame again");
    std::cout << "PASS HUDLess substitution/restoration and same-frame MFG GPU reuse\n";

    // GPU completion does not close a game's replayable command list. A
    // buffered host may retain more lists than the private XeFG ring size.
    {
      auto explicit_ui = gpu.Texture();
      gpu.Write(explicit_ui.Get(), 255);
      gpu.Transition(explicit_ui.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
      gpu.Submit();
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_UI, explicit_ui.Get());
      const auto saved_list = gpu.list;
      const auto saved_allocator = gpu.allocator;
      std::array<ComPtr<ID3D12GraphicsCommandList>, 8> lists;
      std::array<ComPtr<ID3D12CommandAllocator>, 8> allocators;
      std::array<native::fg_service::Slot*, 8> retained_slots{};
      const auto submitted_before = native::native_fg_submitted_frames.load();
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 1u);
      const auto execute_retained = [&] {
        ID3D12CommandList* commands[]{gpu.list.Get()};
        gpu.queue->ExecuteCommandLists(1, commands);
        Check(SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial)), "buffered host fence");
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Check(event != nullptr, "buffered host event");
        const auto signal = gpu.fence->SetEventOnCompletion(gpu.serial, event);
        const auto wait = SUCCEEDED(signal) ? WaitForSingleObject(event, 10000) : WAIT_FAILED;
        CloseHandle(event);
        Check(wait == WAIT_OBJECT_0, "buffered host completion without Reset");
      };
      for (uint32_t round = 0; round < 2; ++round) {
        for (size_t index = 0; index < lists.size(); ++index) {
          if (round == 0) {
            Check(SUCCEEDED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&allocators[index]))), "buffered host allocator");
            Check(SUCCEEDED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                allocators[index].Get(), nullptr, IID_PPV_ARGS(&lists[index]))), "buffered host list");
            Check(native::InstallCommandListStateHooks(lists[index].Get()), "buffered host state hooks");
            Check(SUCCEEDED(lists[index]->Close()), "buffered host initial close");
          }
          Check(SUCCEEDED(allocators[index]->Reset()), "buffered host allocator reuse");
          Check(SUCCEEDED(lists[index]->Reset(allocators[index].Get(), nullptr)), "buffered host list reuse");
          submission::PruneCompletedGenerations();
          gpu.list = lists[index];
          gpu.allocator = allocators[index];
          const auto before = model_calls;
          Check(native::RunNativePreFg(gpu.list.Get(), &parameters) == hudless.Get()
                    && model_calls == before + 1 && stream.cached->color_inputs_prepared,
                "every buffered host frame must run NR even beyond three replayable lists");
          if (round == 0) retained_slots[index] = stream.cached;
          auto* cached_ui = native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_UI);
          native::RestoreNativePreFg(gpu.list.Get(), &parameters, hudless.Get(), true);
          parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 2u);
          Check(native::RunNativePreFg(gpu.list.Get(), &parameters) == hudless.Get()
                    && model_calls == before + 1
                    && native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_UI) == cached_ui,
                "MFG keeps the same enhanced UI without another model on buffered host lists");
          native::RestoreNativePreFg(gpu.list.Get(), &parameters, hudless.Get(), true);
          parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 1u);
          Check(SUCCEEDED(gpu.list->Close()), "buffered host recording close");
          execute_retained();
          Check(!submission::ResourceReleasable(stream.cached->color[0])
                    && !submission::ResourceReleasable(stream.cached->coupled_ui)
                    && !submission::ResourceReleasable(stream.cached->coupled_hudless)
                    && !submission::ResourceReleasable(stream.cached->color_heap),
                "GPU completion alone cannot permit overwriting a replayable NR slot");
          for (size_t earlier = 0; round == 0 && earlier < index; ++earlier) {
            Check(retained_slots[earlier] != stream.cached && retained_slots[earlier]->color_inputs_prepared
                      && !submission::ResourceReleasable(retained_slots[earlier]->color_heap),
                  "buffered host frames retain distinct private NR outputs until Reset");
          }
        }
        if (round == 0) {
          gpu.list = lists[0];
          execute_retained();
          Check(native::native_fg_submitted_frames.load() == submitted_before + lists.size(),
                "replaying an older host list cannot count a new application NR frame");
        }
      }
      Check(std::size(stream.slots) == lists.size(), "native slots stop growing when the host rotates its lists");
      Check(native::native_fg_submitted_frames.load() == submitted_before + 2 * lists.size(),
            "each buffered application frame has exactly one proven NR submission");
      for (size_t index = 0; index < lists.size(); ++index) {
        Check(SUCCEEDED(allocators[index]->Reset()), "buffered host cleanup allocator");
        Check(SUCCEEDED(lists[index]->Reset(allocators[index].Get(), nullptr)), "buffered host cleanup Reset");
        Check(SUCCEEDED(lists[index]->Close()), "buffered host cleanup close");
      }
      submission::PruneCompletedGenerations();
      gpu.list = saved_list;
      gpu.allocator = saved_allocator;
      gpu.Submit();
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_UI, static_cast<void*>(nullptr));
      Check(pixels(explicit_ui.Get(), NATIVE_INPUT_STATE) == std::vector<uint8_t>(256, 255),
            "buffered host composition never overwrites the original explicit UI pixels");
      std::cout << "PASS native eight-list host: 16 NR frames, coherent UI/MFG, replay protected, stable slot reuse\n";
    }

    // FG's declared scale already converts raw vectors to resource pixels.
    // Exercise the real native entry and Record with actual guide resources;
    // inspect the NGX parameter interface consumed by ProcessInline, not a
    // reconstructed FrameInput or a mirrored FeatureState field.
    constexpr uint32_t GUIDE_WIDTH = 4, GUIDE_HEIGHT = 4;
    ComPtr<ID3D12Resource> motion, depth;
    Check(native::CreateScratchTexture(gpu.device.Get(), GUIDE_WIDTH, GUIDE_HEIGHT, DXGI_FORMAT_R16G16_FLOAT,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, &motion, L"FG test motion"), "real motion texture");
    Check(native::CreateScratchTexture(gpu.device.Get(), GUIDE_WIDTH, GUIDE_HEIGHT, DXGI_FORMAT_R32_FLOAT,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, &depth, L"FG test depth"), "real depth texture");
    gpu.Write(motion.Get(), 0);
    gpu.Write(depth.Get(), 0);
    gpu.Transition(motion.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
    gpu.Transition(depth.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
    gpu.Submit();
    native::present_guides.store(native::kPresentGuidesOptional);
    renodx::addons::dlss5::bridge::OwnedParameters guide_parameters;
    guide_parameters.Set(NVSDK_NGX_DLSSG_Parameter_Backbuffer, backbuffer.Get());
    guide_parameters.Set(NVSDK_NGX_DLSSG_Parameter_HUDLess, hudless.Get());
    guide_parameters.Set(NVSDK_NGX_DLSSG_Parameter_MVecs, motion.Get());
    guide_parameters.Set(NVSDK_NGX_DLSSG_Parameter_Depth, depth.Get());
    guide_parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 1u);
    uint32_t scale_errors = 0;
    for (const uint32_t units : {0u, 1u}) {
      native::present_mvec_units.store(units);
      for (const auto declared : {std::pair{1.f, -1.f},
                                 std::pair{static_cast<float>(GUIDE_WIDTH), -static_cast<float>(GUIDE_HEIGHT)}}) {
        guide_parameters.Set(NVSDK_NGX_DLSSG_Parameter_MvecScaleX, declared.first);
        guide_parameters.Set(NVSDK_NGX_DLSSG_Parameter_MvecScaleY, declared.second);
        const auto calls_before = model_calls;
        ID3D12Resource* guide_original = nullptr;
        for (uint32_t frame = 0; frame < 4 && guide_original == nullptr; ++frame) {
          guide_original = native::RunNativePreFg(gpu.list.Get(), &guide_parameters);
          if (guide_original == nullptr) gpu.Submit();
        }
        Check(guide_original == hudless.Get() && model_calls > calls_before,
              "real-guides transaction reaches model evaluation and native writeback");
        auto& guide_stream = native::fg_service::streams.at({true, reinterpret_cast<uintptr_t>(&guide_parameters)});
        const float expected_x = declared.first * (units == 1 ? GUIDE_WIDTH : 1u);
        const float expected_y = declared.second * (units == 1 ? GUIDE_HEIGHT : 1u);
        for (uint32_t role = 0; role < 2; ++role) {
          const auto& block = guide_stream.roles[role].parameters;
          Check(guide_stream.roles[role].real_guides
                    && native::GetD3D12Resource(&block, NVSDK_NGX_Parameter_MotionVectors) == motion.Get()
                    && native::GetD3D12Resource(&block, NVSDK_NGX_Parameter_Depth) == depth.Get(),
                "both model roles consume the actual FG guides");
          const float actual_x = native::GetFloat(&block, NVSDK_NGX_Parameter_MV_Scale_X);
          const float actual_y = native::GetFloat(&block, NVSDK_NGX_Parameter_MV_Scale_Y);
          const bool matches = actual_x == expected_x && actual_y == expected_y;
          scale_errors += !matches;
          std::cout << (matches ? "PASS" : "MV SCALE ERROR") << " mode=" << (units == 0 ? "Auto" : "Normalized")
                    << " role=" << role << " declared=" << declared.first << ',' << declared.second
                    << " expected=" << expected_x << ',' << expected_y << " actual=" << actual_x << ',' << actual_y << '\n';
        }
        expect_native_inputs(native::PreFgResource(&guide_parameters, NVSDK_NGX_DLSSG_Parameter_HUDLess));
        native::RestoreNativePreFg(gpu.list.Get(), &guide_parameters, guide_original, true);
        expect_native_inputs(nullptr);
        gpu.Submit();
      }
    }
    Check(hudless_snapshot_copies == 0, "dual NR and MFG retain no unused HUDLess snapshot copies");
    // The shared Record path receives null composition parameters for XeFG
    // disabled/AUTO; a valid native UI consumer must still retain its input.
    auto ui = gpu.Texture();
    gpu.Write(ui.Get(), 0);
    gpu.Transition(ui.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
    gpu.Submit();
    native::stack_passes.store(1);
    native::present_pre_fg.store(native::kPresentPreFgTwoPass);
    native::present_guides.store(native::kPresentGuidesNever);
    fg::FrameInput composition_input;
    composition_input.backbuffer = backbuffer.Get();
    composition_input.hudless = hudless.Get();
    composition_input.ui = ui.Get();
    composition_input.backbuffer_state = composition_input.hudless_state = composition_input.ui_state = NATIVE_INPUT_STATE;
    composition_input.color_width = composition_input.color_height = 8;
    auto& composition_slot = stream.slots[0];
    Check(native::fg_service::PrepareSurfaces(&stream, &composition_slot, composition_input), "composition surfaces");
    bool composition_ready = false;
    std::array<native::NrRecording, 2> composition_recordings;
    native::fg_service::Record(&stream, &composition_slot, gpu.list.Get(), composition_input,
        owner.prefg_target, &composition_ready, &composition_recordings, &parameters);
    Check(hudless_snapshot_copies == 1, "possible UI consumer retains one original HUDLess snapshot");
    Check(pixels(composition_slot.original_hudless, NATIVE_INPUT_STATE) == std::vector<uint8_t>(256, 96),
          "UI snapshot preserves original pixels before HUDLess NR");
    const auto before_null_consumer = hudless_snapshot_copies;
    const auto before_direct_input = staged_input_copies;
    const auto original_backbuffer_pixels = pixels(backbuffer.Get(), NATIVE_INPUT_STATE);
    const auto original_hudless_pixels = pixels(hudless.Get(), NATIVE_INPUT_STATE);
    for (uint32_t i = 0; i < 2; ++i) {
      gpu.Transition(composition_slot.color[i], NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COMMON);
      gpu.Write(composition_slot.color[i], static_cast<uint8_t>(160 + i));
      gpu.Transition(composition_slot.color[i], D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
    }
    gpu.Submit();
    native::fg_service::Record(&stream, &composition_slot, gpu.list.Get(), composition_input,
        owner.prefg_target, &composition_ready, &composition_recordings);
    Check(composition_ready && composition_recordings[0].model_invoked && composition_recordings[1].model_invoked,
          "direct input must actually prepare both NR outputs");
    Check(hudless_snapshot_copies == before_null_consumer, "XeFG null composition parameters skip snapshot with UI present");
    Check(staged_input_copies == before_direct_input, "full-frame XeFG still copies host input through output staging");
    for (uint32_t i = 0; i < 2; ++i) {
      const auto key = native::WorksetKey{stream.roles[i].Handle(), reinterpret_cast<const ID3D12Resource*>(&stream.roles[i])};
      const auto& workset = native::worksets.at(key);
      Check(pixels(workset.original, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                == (i == 0 ? original_backbuffer_pixels : original_hudless_pixels),
            "NR copy-in read poisoned output staging instead of the original host color");
    }
    Check(pixels(backbuffer.Get(), NATIVE_INPUT_STATE) == original_backbuffer_pixels
        && pixels(hudless.Get(), NATIVE_INPUT_STATE) == original_hudless_pixels,
        "direct input recording published before the commit boundary");
    // Motion describes the immediately preceding application frame. After a
    // skipped application frame, neither role may reuse an older NR history.
    for (const uint64_t frame : {100u, 101u, 103u, 104u}) {
      composition_input.application_frame = frame;
      const auto calls_before = model_calls;
      native::fg_service::Record(&stream, &composition_slot, gpu.list.Get(), composition_input,
          owner.prefg_target, &composition_ready, &composition_recordings);
      Check(composition_ready && model_calls == calls_before + 2, "history continuity reaches both model roles");
      if (frame != 100) {
        const int expected_reset = (frame == 103 ? 1 : 0);
        Check(model_resets[model_resets.size() - 2] == expected_reset
                  && model_resets.back() == expected_reset,
              "skipped application frame must reset both actual model histories exactly once");
      }
      gpu.Submit();
    }
    composition_input.application_frame = 105;
    composition_input.hudless = nullptr;
    const auto before_single_color = model_calls;
    native::fg_service::Record(&stream, &composition_slot, gpu.list.Get(), composition_input,
        owner.prefg_target, &composition_ready, &composition_recordings);
    Check(composition_ready && model_calls == before_single_color + 1 && model_resets.back() == 0,
          "continuous backbuffer must retain its history when HUDLess is absent");
    gpu.Submit();
    composition_input.application_frame = 106;
    composition_input.hudless = hudless.Get();
    native::fg_service::Record(&stream, &composition_slot, gpu.list.Get(), composition_input,
        owner.prefg_target, &composition_ready, &composition_recordings);
    Check(composition_ready && model_resets[model_resets.size() - 2] == 1 && model_resets.back() == 0,
          "returning HUDLess alone must reset; continuous backbuffer must not reset");
    gpu.Submit();
    std::cout << "PASS application-frame gaps reset affected model histories once; continuous roles retain history\n";
    composition_input.application_frame = 0;
    fail_call = model_calls + 2;
    Check(native::fg_service::Record(&stream, &composition_slot, gpu.list.Get(), composition_input,
              owner.prefg_target, &composition_ready, &composition_recordings) == fg::Result::FAILED
              && !composition_ready, "direct input second-face failure must reject the transaction");
    Check(pixels(backbuffer.Get(), NATIVE_INPUT_STATE) == original_backbuffer_pixels
        && pixels(hudless.Get(), NATIVE_INPUT_STATE) == original_hudless_pixels,
        "direct input partial failure changed external game colors");
    fail_call = 0;
    {
      namespace shot = renodx::addons::dlss5::screenshot;
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_UIAlpha, ui.Get());
      const auto record_composition = [&] {
        Check(native::fg_service::Record(&stream, &composition_slot, gpu.list.Get(), composition_input,
                  owner.prefg_target, &composition_ready, &composition_recordings, &parameters) == fg::Result::PREPARED
                  && composition_ready, "capture composition reaches accepted output");
      };
      // Mature the same workset before comparing normal and diagnostic GPU work.
      for (uint32_t retry = 0; retry < 4; ++retry) {
        native::fg_service::Record(&stream, &composition_slot, gpu.list.Get(), composition_input,
            owner.prefg_target, &composition_ready, &composition_recordings, &parameters);
        gpu.Submit();
        if (composition_ready) break;
      }
      Check(composition_ready, "composition workset matures after preceding failure fixture");
      const auto normal_copies_before = recorded_copies;
      const auto normal_barriers_before = recorded_barriers;
      record_composition();
      const auto normal_copies = recorded_copies - normal_copies_before;
      const auto normal_barriers = recorded_barriers - normal_barriers_before;
      Check(!shot::HasPending(), "normal composition creates no pending capture");
      gpu.Submit();
      shot::stage_planes_enabled.store(true);
      shot::diagnostic_planes_enabled.store(true);
      shot::source_mode.store(2);
      Check(shot::RequestCapture(), "arm exact model capture");
      record_composition();
      const uint64_t serial = shot::PendingModelCaptureSerial(gpu.list.Get());
      Check(serial != 0 && shot::PendingModelCaptureSerial(nullptr) == 0,
            "model pending is bound to its recording list");
      ComPtr<ID3D12CommandAllocator> other_allocator;
      ComPtr<ID3D12GraphicsCommandList> other_list;
      Check(SUCCEEDED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&other_allocator)))
                && SUCCEEDED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    other_allocator.Get(), nullptr, IID_PPV_ARGS(&other_list))), "other list fixture");
      Check(shot::PendingModelCaptureSerial(other_list.Get()) == 0, "foreign recording list rejected");
      {
        std::scoped_lock lock(shot::internal::state_mutex);
        const auto& capture = *shot::internal::pending;
        Check(capture.required_planes_complete, "all five same-frame composition planes complete");
        for (const auto& [name, resource] : {
                 std::pair{"native_ui_alpha", ui.Get()},
                 std::pair{"native_original_hudless", composition_slot.original_hudless},
                 std::pair{"native_enhanced_hdrhl", composition_slot.color[1]},
                 std::pair{"native_pre_compose", composition_slot.color[0]},
                 std::pair{"native_post_compose", composition_slot.color[0]}}) {
          const auto found = std::find_if(capture.planes.begin(), capture.planes.end(),
              [&](const auto& plane) { return plane.name == name; });
          Check(found != capture.planes.end() && found->recorded && found->required
                    && found->source_identity == reinterpret_cast<uintptr_t>(resource), "exact plane source and copy");
          Check(observed_states.at(resource) == NATIVE_INPUT_STATE, "diagnostic barrier restores each source");
        }
      }
      const auto copies_before_reject = recorded_copies;
      Check(!shot::PrepareDiagnosticPlane(gpu.device.Get(), "wrong_serial", ui.Get(), 8, 8,
                std::nullopt, true, serial + 1), "wrong serial cannot attach a plane");
      Check(shot::PrepareDiagnosticPlane(gpu.device.Get(), "ownership_probe", ui.Get(), 8, 8,
                std::nullopt, true, serial), "prepare ownership copy probe");
      shot::RecordDiagnosticPlaneCopy(gpu.list.Get(), "ownership_probe", ui.Get(), serial + 1);
      shot::RecordDiagnosticPlaneCopy(other_list.Get(), "ownership_probe", ui.Get(), serial);
      Check(recorded_copies == copies_before_reject, "serial and list mismatch issue no copies");
      {
        std::scoped_lock lock(shot::internal::state_mutex);
        Check(!shot::internal::pending->planes.back().recorded, "mismatched copies leave plane unrecorded");
        // Simulate OnPresent handoff and a new pending request on the same list.
        ++shot::internal::pending->serial;
      }
      Check(!shot::PrepareDiagnosticPlane(gpu.device.Get(), "stale_serial", ui.Get(), 8, 8,
                std::nullopt, true, serial), "old serial cannot attach to replacement pending");
      shot::RecordDiagnosticPlaneCopy(gpu.list.Get(), "ownership_probe", ui.Get(), serial);
      Check(recorded_copies == copies_before_reject, "old serial cannot copy into replacement pending");
      gpu.Submit();
      {
        std::scoped_lock lock(shot::internal::state_mutex);
        shot::internal::ReleaseReadbacks(*shot::internal::pending);
        shot::internal::pending.reset();
        shot::internal::has_pending.store(false);
        shot::internal::stage_busy.store(false);
      }
      shot::stage_planes_enabled.store(false);
      shot::diagnostic_planes_enabled.store(false);
      shot::source_mode.store(1);
      const auto copies_after_capture = recorded_copies;
      const auto barriers_after_capture = recorded_barriers;
      record_composition();
      Check(recorded_copies - copies_after_capture == normal_copies
                && recorded_barriers - barriers_after_capture == normal_barriers,
            "unrequested normal path retains identical GPU copy and barrier counts");
      gpu.Submit();
      gpu.Transition(ui.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COMMON);
      gpu.Write(ui.Get(), 255);
      gpu.Transition(ui.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
      gpu.Submit();
      const auto before_legacy_ui = model_calls;
      native::fg_service::Record(&stream, &composition_slot, gpu.list.Get(), composition_input,
          owner.prefg_target, &composition_ready, &composition_recordings, &parameters, false);
      Check(composition_ready && model_calls == before_legacy_ui + 2
                && composition_recordings[0].model_recorded && composition_recordings[1].model_recorded,
            "legacy FG with opaque UI must enhance both consumed colors instead of alpha-composing to the original");
      gpu.Submit();
      Check(native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_UIAlpha) == ui.Get(),
            "legacy consumer resources remain bound; no HUDLess/UI tags are removed");
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_UIAlpha, static_cast<ID3D12Resource*>(nullptr));
      const auto* uir_handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xF011});
      native::RegisterCreatedFeatureLocked(uir_handle, NVSDK_NGX_Feature_FrameGeneration, &parameters);
      Check(!native::native_fg_ui_recomposition.at(uir_handle), "missing create UIR is recorded disabled");
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_UserInterfaceRecompositionEnabled, 1u);
      Check(!native::native_fg_ui_recomposition.at(uir_handle), "evaluate block mutation cannot enable create-time UIR");
      native::ForgetSourceHandleLocked(uir_handle);
      Check(!native::native_fg_ui_recomposition.contains(uir_handle), "FG release removes its UIR contract");
      native::RegisterCreatedFeatureLocked(uir_handle, NVSDK_NGX_Feature_FrameGeneration, &parameters);
      Check(native::native_fg_ui_recomposition.at(uir_handle), "enabled create UIR retains exact alpha path eligibility");
      native::ForgetSourceHandleLocked(uir_handle);
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_UserInterfaceRecompositionEnabled, 0u);
      std::cout << "PASS exact model serial/list ownership, five planes, barrier restoration and unchanged normal GPU work\n";
    }
    auto oversized_desc = backbuffer->GetDesc();
    oversized_desc.Width = 16;
    const D3D12_HEAP_PROPERTIES oversized_heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    ComPtr<ID3D12Resource> oversized;
    Check(SUCCEEDED(gpu.device->CreateCommittedResource(&oversized_heap, D3D12_HEAP_FLAG_NONE,
              &oversized_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&oversized))),
          "oversized borrowed color fixture");
    native::PresentTarget invalid_source = owner.prefg_target;
    invalid_source.output_state = NATIVE_INPUT_STATE;
    invalid_source.source_color = oversized.Get();
    bool invalid_touched = false;
    native::NrRecording invalid_recording;
    const auto calls_before_invalid = model_calls;
    native::last_result = NVSDK_NGX_Result_Success;
    Check(!native::ProcessInline(gpu.list.Get(), stream.roles[0].Handle(), &stream.roles[0].parameters,
              &invalid_touched, &invalid_source, &invalid_recording)
              && !invalid_touched && model_calls == calls_before_invalid
              && native::last_result == NVSDK_NGX_Result_FAIL_InvalidParameter,
          "incompatible borrowed input must reject before GPU work");
    invalid_source.source_color = backbuffer.Get();
    stream.roles[0].parameters.Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 1);
    stream.roles[0].parameters.Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 1u);
    native::last_result = NVSDK_NGX_Result_Success;
    Check(!native::ProcessInline(gpu.list.Get(), stream.roles[0].Handle(), &stream.roles[0].parameters,
              &invalid_touched, &invalid_source, &invalid_recording)
              && !invalid_touched && model_calls == calls_before_invalid
              && native::last_result == NVSDK_NGX_Result_FAIL_InvalidParameter,
          "borrowed input subrect must reject before GPU work");
    stream.roles[0].parameters.Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);
    stream.roles[0].parameters.Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0u);
    gpu.Submit();
    std::cout << "PASS unused HUDLess snapshot omitted; possible UI consumer preserves original pixels\n";
    {
      // Explicit opaque UI reduces the SDK's color relation to B.rgb=U.rgb,
      // independently of the feature's UIR creation option.
      auto opaque_ui = gpu.Texture();
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
      uint64_t bytes = 0;
      const auto ui_desc = opaque_ui->GetDesc();
      gpu.device->GetCopyableFootprints(&ui_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
      auto upload = gpu.Buffer(bytes, D3D12_HEAP_TYPE_UPLOAD);
      uint8_t* mapped = nullptr;
      Check(SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))), "opaque UI upload map");
      std::memset(mapped, 0, static_cast<size_t>(bytes));
      for (uint32_t y = 0; y < 8; ++y) {
        for (uint32_t x = 0; x < 8; ++x) {
          auto* pixel = mapped + footprint.Offset + y * footprint.Footprint.RowPitch + x * 4;
          pixel[0] = pixel[1] = pixel[2] = 64;
          pixel[3] = 255;
        }
      }
      upload->Unmap(0, nullptr);
      const D3D12_TEXTURE_COPY_LOCATION ui_source{.pResource = upload.Get(),
          .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = footprint};
      const D3D12_TEXTURE_COPY_LOCATION ui_destination{.pResource = opaque_ui.Get(),
          .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      gpu.Transition(opaque_ui.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
      gpu.list->CopyTextureRegion(&ui_destination, 0, 0, 0, &ui_source, nullptr);
      gpu.Transition(opaque_ui.Get(), D3D12_RESOURCE_STATE_COPY_DEST, NATIVE_INPUT_STATE);
      gpu.uploads.push_back(upload);
      gpu.Transition(backbuffer.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COMMON);
      gpu.Transition(hudless.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COMMON);
      seed();
      gpu.Submit();
      renodx::addons::dlss5::bridge::OwnedParameters explicit_ui;
      explicit_ui.Set(NVSDK_NGX_DLSSG_Parameter_Backbuffer, backbuffer.Get());
      explicit_ui.Set(NVSDK_NGX_DLSSG_Parameter_HUDLess, hudless.Get());
      explicit_ui.Set(NVSDK_NGX_DLSSG_Parameter_UI, opaque_ui.Get());
      explicit_ui.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 1u);
      const auto* ui_handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xF012});
      native::RegisterCreatedFeatureLocked(ui_handle, NVSDK_NGX_Feature_FrameGeneration, &explicit_ui);
      ID3D12Resource* original = nullptr;
      const auto models_before_ui = model_calls;
      for (uint32_t attempt = 0; attempt < 4 && original == nullptr; ++attempt) {
        original = native::RunNativePreFg(gpu.list.Get(), &explicit_ui, ui_handle);
        if (original == nullptr) submit_native();
      }
      Check(original == hudless.Get(), "explicit UI regression reaches a real prepared NR output");
      ComPtr<ID3D12Resource> served_ui = native::PreFgResource(&explicit_ui, NVSDK_NGX_DLSSG_Parameter_UI);
      native::RestoreNativePreFg(gpu.list.Get(), &explicit_ui, original, true, ui_handle);
      const auto served_back = pixels(backbuffer.Get(), NATIVE_INPUT_STATE);
      Check(served_back[0] != 64, "opaque UI fixture must actually enhance backbuffer color");
      // The current bug leaves the old explicit UI resource bound even
      // though the real image changed. This assertion needs no assumption
      // about the private UI replacement's eventual format.
      Check(served_ui.Get() != opaque_ui.Get(),
            "non-UIR NR must update the explicit UI color contract, not submit the original opaque scene");
      Check(native::PreFgResource(&explicit_ui, NVSDK_NGX_DLSSG_Parameter_UI) == opaque_ui.Get(),
            "native FG restores the original explicit UI binding after the consumer returns");
      Check(pixels(opaque_ui.Get(), NATIVE_INPUT_STATE)[0] == 64,
            "native FG must preserve the original UI pixels");
      Check(model_calls == models_before_ui + 1, "one NR result supplies real color and both auxiliary FG colors");
      auto& ui_stream = native::fg_service::streams.at({true, reinterpret_cast<uintptr_t>(&owner)});
      auto* ui_slot = ui_stream.cached;
      Check(ui_slot != nullptr, "native color fixture retains its completed recording slot");
      explicit_ui.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 2u);
      Check(native::RunNativePreFg(gpu.list.Get(), &explicit_ui, ui_handle) == hudless.Get()
                && native::PreFgResource(&explicit_ui, NVSDK_NGX_DLSSG_Parameter_UI) == served_ui.Get()
                && model_calls == models_before_ui + 1,
            "MFG reuses the same coupled UI and NR output without another model evaluation");
      native::RestoreNativePreFg(gpu.list.Get(), &explicit_ui, hudless.Get(), true, ui_handle);
      Check(native::PreFgResource(&explicit_ui, NVSDK_NGX_DLSSG_Parameter_UI) == opaque_ui.Get(),
            "MFG restores the explicit UI binding");
      gpu.Submit();
      explicit_ui.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 1u);
      const uint8_t alphas[8]{0, 1, 64, 128, 192, 254, 255, 255};
      auto alpha_upload = gpu.Buffer(bytes, D3D12_HEAP_TYPE_UPLOAD);
      Check(SUCCEEDED(alpha_upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))), "alpha fixture map");
      std::memset(mapped, 0, static_cast<size_t>(bytes));
      for (uint32_t y = 0; y < 8; ++y) {
        for (uint32_t x = 0; x < 8; ++x) {
          auto* pixel = mapped + footprint.Offset + y * footprint.Footprint.RowPitch + x * 4;
          pixel[0] = pixel[1] = pixel[2] = 64;
          pixel[3] = alphas[x];
        }
      }
      alpha_upload->Unmap(0, nullptr);
      auto alpha_source = ui_source;
      alpha_source.pResource = alpha_upload.Get();
      gpu.Transition(opaque_ui.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COPY_DEST);
      gpu.list->CopyTextureRegion(&ui_destination, 0, 0, 0, &alpha_source, nullptr);
      gpu.Transition(opaque_ui.Get(), D3D12_RESOURCE_STATE_COPY_DEST, NATIVE_INPUT_STATE);
      gpu.uploads.push_back(alpha_upload);
      for (auto [resource, value] : {std::pair{backbuffer.Get(), uint8_t{64}},
                                   std::pair{ui_slot->color[0], uint8_t{128}}}) {
        gpu.Transition(resource, NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COMMON);
        gpu.Write(resource, value);
        gpu.Transition(resource, D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
      }
      gpu.Submit();
      fg::FrameInput color_input;
      color_input.backbuffer = backbuffer.Get();
      color_input.hudless = hudless.Get();
      color_input.ui = opaque_ui.Get();
      color_input.color_width = color_input.color_height = 8;
      Check(native::fg_service::PrepareNativeColorInputs(&ui_stream, ui_slot, color_input),
            "direct color contract fixture prepares real GPU resources");
      native::fg_service::RecordNativeColorInputs(&ui_stream, ui_slot, gpu.list.Get(), color_input);
      gpu.Submit();
      const auto read_unorm16 = [&](ID3D12Resource* texture) {
        Check(texture->GetDesc().Format == DXGI_FORMAT_R16G16B16A16_UNORM,
              "FG auxiliary colors retain sixteen-bit channels");
        const auto desc = texture->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        uint64_t length = 0;
        gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &length);
        auto readback = gpu.Buffer(length, D3D12_HEAP_TYPE_READBACK);
        const D3D12_TEXTURE_COPY_LOCATION from{.pResource = texture,
            .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        const D3D12_TEXTURE_COPY_LOCATION to{.pResource = readback.Get(),
            .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = fp};
        gpu.Transition(texture, NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        gpu.Transition(texture, D3D12_RESOURCE_STATE_COPY_SOURCE, NATIVE_INPUT_STATE);
        gpu.Submit();
        uint8_t* data = nullptr;
        Check(SUCCEEDED(readback->Map(0, nullptr, reinterpret_cast<void**>(&data))), "color contract readback");
        std::vector<uint16_t> result(8 * 8 * 4);
        for (uint32_t y = 0; y < 8; ++y) {
          std::memcpy(result.data() + y * 32, data + fp.Offset + y * fp.Footprint.RowPitch, 64);
        }
        readback->Unmap(0, nullptr);
        return result;
      };
      const auto h_codes = read_unorm16(ui_slot->coupled_hudless);
      const auto u_codes = read_unorm16(ui_slot->coupled_ui);
      for (uint32_t pixel = 0; pixel < 64; ++pixel) {
        const auto alpha = static_cast<uint16_t>(alphas[pixel % 8] * 257u);
        Check(u_codes[pixel * 4 + 3] == alpha, "all original alpha codes survive exactly in UNORM16");
        for (uint32_t channel = 0; channel < 3; ++channel) {
          const float u = u_codes[pixel * 4 + channel] / 65535.f;
          const float h = h_codes[pixel * 4 + channel] / 65535.f;
          Check(u_codes[pixel * 4 + channel] <= alpha,
                "derived UI respects premultiplication at zero, partial and full opacity");
          Check(std::abs(u + (1.f - alpha / 65535.f) * h - 128.f / 255.f) <= 2.f / 65535.f,
                "actual GPU colors recompose to the unchanged NR image within auxiliary quantization");
        }
      }
      Check(pixels(ui_slot->color[0], NATIVE_INPUT_STATE) == std::vector<uint8_t>(256, 128)
                && pixels(backbuffer.Get(), NATIVE_INPUT_STATE) == std::vector<uint8_t>(256, 64)
                && pixels(hudless.Get(), NATIVE_INPUT_STATE) == std::vector<uint8_t>(256, 96),
            "color coupling leaves the NR result and original game color surfaces unchanged");
      {
        ComPtr<ID3D12Resource> original10, hudless10, enhanced10;
        for (auto* target : {original10.GetAddressOf(), hudless10.GetAddressOf(), enhanced10.GetAddressOf()}) {
          Check(native::CreateScratchTexture(gpu.device.Get(), 8, 8, DXGI_FORMAT_R10G10B10A2_UNORM,
                    D3D12_RESOURCE_FLAG_NONE, NATIVE_INPUT_STATE, target, L"native RGB10 color fixture"),
                "RGB10 fixture texture");
        }
        const uint32_t codes[8]{0, 1, 2, 511, 512, 1021, 1022, 1023};
        const auto write10 = [&](ID3D12Resource* texture, uint32_t rotation) {
          D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
          uint64_t length = 0;
          const auto desc = texture->GetDesc();
          gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &length);
          auto data = gpu.Buffer(length, D3D12_HEAP_TYPE_UPLOAD);
          uint8_t* mapped_data = nullptr;
          Check(SUCCEEDED(data->Map(0, nullptr, reinterpret_cast<void**>(&mapped_data))), "RGB10 upload");
          std::memset(mapped_data, 0, static_cast<size_t>(length));
          for (uint32_t y = 0; y < 8; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(mapped_data + fp.Offset + y * fp.Footprint.RowPitch);
            for (uint32_t x = 0; x < 8; ++x) {
              row[x] = 0xc0000000u;
              for (uint32_t c = 0; c < 3; ++c) {
                row[x] |= codes[(x + y + c * 3 + rotation) % 8] << (c * 10);
              }
            }
          }
          data->Unmap(0, nullptr);
          const D3D12_TEXTURE_COPY_LOCATION from{.pResource = data.Get(),
              .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = fp};
          const D3D12_TEXTURE_COPY_LOCATION to{.pResource = texture,
              .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
          gpu.Transition(texture, NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COPY_DEST);
          gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
          gpu.Transition(texture, D3D12_RESOURCE_STATE_COPY_DEST, NATIVE_INPUT_STATE);
          gpu.uploads.push_back(data);
        };
        write10(original10.Get(), 4);
        write10(hudless10.Get(), 2);
        write10(enhanced10.Get(), 0);
        gpu.Submit();
        const auto unchanged10 = pixels(enhanced10.Get(), NATIVE_INPUT_STATE);
        auto rgb10_input = color_input;
        rgb10_input.backbuffer = original10.Get();
        rgb10_input.hudless = hudless10.Get();
        auto* previous_color = ui_slot->color[0];
        ui_slot->color[0] = enhanced10.Get();
        Check(native::fg_service::PrepareNativeColorInputs(&ui_stream, ui_slot, rgb10_input),
              "RGB10 boundary fixture prepares without changing output format");
        native::fg_service::RecordNativeColorInputs(&ui_stream, ui_slot, gpu.list.Get(), rgb10_input);
        gpu.Submit();
        const auto h10 = read_unorm16(ui_slot->coupled_hudless);
        const auto u10 = read_unorm16(ui_slot->coupled_ui);
        for (uint32_t pixel = 0; pixel < 64; ++pixel) {
          const auto a = alphas[pixel % 8] * 257u;
          Check(u10[pixel * 4 + 3] == a, "RGB10 preserves the original UI alpha codes");
          for (uint32_t c = 0; c < 3; ++c) {
            const float expected = codes[(pixel % 8 + pixel / 8 + c * 3) % 8] / 1023.f;
            const float actual = (u10[pixel * 4 + c]
                + (1.f - a / 65535.f) * h10[pixel * 4 + c]) / 65535.f;
            Check(u10[pixel * 4 + c] <= a && std::abs(actual - expected) <= 2.f / 65535.f,
                  "RGB10 endpoints, unequal channels and both edit directions preserve legal color composition");
          }
        }
        Check(pixels(enhanced10.Get(), NATIVE_INPUT_STATE) == unchanged10,
              "all ten-bit NR output codes survive coupling bit for bit");
        ui_slot->color[0] = previous_color;
      }
      {
        // Coupled UI has needed only the backbuffer model so far. Losing
        // that UI must expose the known HUDLess role without a cold frame.
        gpu.Transition(backbuffer.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COMMON);
        gpu.Transition(hudless.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COMMON);
        seed();
        explicit_ui.Set(NVSDK_NGX_DLSSG_Parameter_UI, static_cast<ID3D12Resource*>(nullptr));
        explicit_ui.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 1u);
        const auto models_before_ui_loss = model_calls;
        const auto creates_before_ui_loss = next_handle;
        const auto submitted_before_ui_loss = native::native_fg_submitted_frames.load();
        auto* ui_loss_original = native::RunNativePreFg(gpu.list.Get(), &explicit_ui, ui_handle);
        Check(ui_loss_original == hudless.Get(),
              "losing explicit UI immediately records dual NR without HUDLess warmup");
        Check(model_calls == models_before_ui_loss + 2 && next_handle == creates_before_ui_loss,
              "known HUDLess model is resident before UI disappears; both roles evaluate without creation");
        auto* ui_loss_replacement = native::PreFgResource(&explicit_ui, NVSDK_NGX_DLSSG_Parameter_HUDLess);
        Check(ui_loss_replacement != nullptr && ui_loss_replacement != hudless.Get(),
              "UI disappearance binds a private enhanced HUDLess output");
        expect_native_inputs(ui_loss_replacement);
        native::RestoreNativePreFg(gpu.list.Get(), &explicit_ui, ui_loss_original, true, ui_handle);
        Check(native::PreFgResource(&explicit_ui, NVSDK_NGX_DLSSG_Parameter_Backbuffer) == backbuffer.Get()
                  && native::PreFgResource(&explicit_ui, NVSDK_NGX_DLSSG_Parameter_HUDLess) == hudless.Get()
                  && native::PreFgResource(&explicit_ui, NVSDK_NGX_DLSSG_Parameter_UI) == nullptr,
              "UI disappearance restores original input bindings and preserves the absent UI binding");
        Check(native::native_fg_submitted_frames.load() == submitted_before_ui_loss,
              "dual NR recording after UI loss does not fabricate a submission");
        submit_native();
        Check(native::native_fg_result.load() == fg::Result::SUBMITTED
                  && native::native_fg_submitted_frames.load() == submitted_before_ui_loss + 1,
              "first dual NR frame after UI loss receives real queue submission proof");
        Check(pixels(backbuffer.Get(), NATIVE_INPUT_STATE) != std::vector<uint8_t>(256, 64)
                  && pixels(hudless.Get(), NATIVE_INPUT_STATE) == std::vector<uint8_t>(256, 96),
              "UI disappearance enhances the backbuffer while preserving original HUDLess pixels");
        explicit_ui.Set(NVSDK_NGX_DLSSG_Parameter_UI, opaque_ui.Get());
        std::cout << "PASS coupled UI disappearance immediately exposes resident HUDLess NR without a cold frame\n";
      }
      {
        funnel_accounting_tests::Restore substitutions;
        substitutions.Value(&native::ngx_slot_real[0]);
        substitutions.Value(&funnel_accounting_tests::result);
        substitutions.Value(&funnel_accounting_tests::delegate);
        substitutions.Atomic(&native::ngx_loader_published);
        substitutions.Atomic(&native::last_present_steady_ns);
        substitutions.Atomic(&native::lifecycle_tick_owed_ns);
        native::NgxLoaderSymbols loader;
        loader.nr_ready = loader.core_ready = true;
        native::ngx_loader_published.store(&loader);
        native::last_present_steady_ns.store(native::SteadyNowNs());
        native::lifecycle_tick_owed_ns.store(0);
        native::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(funnel_accounting_tests::Evaluate);
        native::ngx_slot_real[0].evaluate_c = reinterpret_cast<void*>(funnel_accounting_tests::EvaluateC);
        funnel_accounting_tests::delegate = true;
        funnel_accounting_tests::result = NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
        Check(NVSDK_NGX_FAILED(native::HookedEvaluateFeatureCSlot<0>(
                  gpu.list.Get(), ui_handle, &explicit_ui, nullptr)),
              "delegated FG consumer really returns a failure");
        Check(native::PreFgResource(&explicit_ui, NVSDK_NGX_DLSSG_Parameter_HUDLess) == hudless.Get()
                  && native::PreFgResource(&explicit_ui, NVSDK_NGX_DLSSG_Parameter_UI) == opaque_ui.Get(),
              "outer handoff owner restores both bindings before nested failure invalidates the cache");
        Check(ui_stream.cached == nullptr && native::native_fg_result.load() == fg::Result::FAILED,
              "failed delegated consumer invalidates the output and preserves a real failure verdict");
        gpu.Submit();
        explicit_ui.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 2u);
        Check(native::RunNativePreFg(gpu.list.Get(), &explicit_ui, ui_handle) == nullptr
                  && native::PreFgResource(&explicit_ui, NVSDK_NGX_DLSSG_Parameter_UI) == opaque_ui.Get(),
              "MFG cannot reuse a failed consumer's coupled colors");
      }
      native::ForgetSourceHandleLocked(ui_handle);
      std::cout << "PASS opaque explicit UI follows NR while source pixels and bindings remain protected\n";
    }
    {
      // One live FG feature may receive a different parameter block on its
      // next application frame without changing its image/history contract.
      auto frame_a = std::make_unique<renodx::addons::dlss5::bridge::OwnedParameters>();
      auto frame_b = std::make_unique<renodx::addons::dlss5::bridge::OwnedParameters>();
      for (auto* frame : {frame_a.get(), frame_b.get()}) {
        frame->Set(NVSDK_NGX_DLSSG_Parameter_Backbuffer, backbuffer.Get());
        frame->Set(NVSDK_NGX_DLSSG_Parameter_HUDLess, hudless.Get());
        frame->Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 1u);
      }
      const auto* stable_handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xF013});
      native::RegisterCreatedFeatureLocked(stable_handle, NVSDK_NGX_Feature_FrameGeneration, frame_a.get());
      const auto write_frame = [&](uint8_t back_value, uint8_t hudless_value) {
        gpu.Transition(backbuffer.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COMMON);
        gpu.Transition(hudless.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COMMON);
        gpu.Write(backbuffer.Get(), back_value);
        gpu.Write(hudless.Get(), hudless_value);
        gpu.Transition(backbuffer.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
        gpu.Transition(hudless.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
      };
      write_frame(64, 96);
      ID3D12Resource* frame_a_original = nullptr;
      for (uint32_t attempt = 0; attempt < 4 && frame_a_original == nullptr; ++attempt) {
        frame_a_original = native::RunNativePreFg(gpu.list.Get(), frame_a.get(), stable_handle);
        if (frame_a_original == nullptr) submit_native();
      }
      Check(frame_a_original == hudless.Get(), "stable FG handle parameter A reaches mature NR output");
      expect_native_inputs(native::PreFgResource(frame_a.get(), NVSDK_NGX_DLSSG_Parameter_HUDLess));
      native::RestoreNativePreFg(gpu.list.Get(), frame_a.get(), frame_a_original, true, stable_handle);
      Check(native::PreFgResource(frame_a.get(), NVSDK_NGX_DLSSG_Parameter_HUDLess) == hudless.Get(),
            "parameter A restores its original HUDLess binding");
      const auto submitted_before_a = native::native_fg_submitted_frames.load();
      submit_native();
      Check(native::native_fg_result.load() == fg::Result::SUBMITTED
                && native::native_fg_submitted_frames.load() == submitted_before_a + 1,
            "parameter A matures through a real queue-proven native submission");
      const auto frame_a_pixels = pixels(backbuffer.Get(), NATIVE_INPUT_STATE);
      Check(frame_a_pixels != std::vector<uint8_t>(256, 64)
                && pixels(hudless.Get(), NATIVE_INPUT_STATE) == std::vector<uint8_t>(256, 96),
            "parameter A enhances the backbuffer and preserves original HUDLess pixels");

      // The deterministic model outputs black RGB, preserving current alpha.
      // A different alpha makes an old-frame cache distinguishable from B.
      write_frame(160, 192);
      const auto models_before_b = model_calls;
      const auto creates_before_b = next_handle;
      const auto serial_before_b = native::native_fg_recording_serial.load();
      const auto submitted_before_b = native::native_fg_submitted_frames.load();
      auto* frame_b_original = native::RunNativePreFg(gpu.list.Get(), frame_b.get(), stable_handle);
      Check(frame_b_original == hudless.Get(),
            "same live FG handle accepts parameter B immediately without create-only warmup");
      Check(model_calls == models_before_b + 2 && next_handle == creates_before_b,
            "parameter pointer change evaluates both existing NR models without recreating them");
      Check(native::native_fg_result.load() == fg::Result::PREPARED
                && native::native_fg_recording_serial.load() == serial_before_b + 1
                && native::native_fg_submitted_frames.load() == submitted_before_b,
            "parameter B records a new application image without fabricating submission proof");
      auto* frame_b_replacement = native::PreFgResource(frame_b.get(), NVSDK_NGX_DLSSG_Parameter_HUDLess);
      Check(frame_b_replacement != nullptr && frame_b_replacement != hudless.Get(),
            "parameter B binds its private enhanced HUDLess input");
      expect_native_inputs(frame_b_replacement);
      native::RestoreNativePreFg(gpu.list.Get(), frame_b.get(), frame_b_original, true, stable_handle);
      Check(native::PreFgResource(frame_b.get(), NVSDK_NGX_DLSSG_Parameter_Backbuffer) == backbuffer.Get()
                && native::PreFgResource(frame_b.get(), NVSDK_NGX_DLSSG_Parameter_HUDLess) == hudless.Get()
                && native::PreFgResource(frame_a.get(), NVSDK_NGX_DLSSG_Parameter_HUDLess) == hudless.Get(),
            "switching parameter objects preserves and restores original game bindings");
      submit_native();
      Check(native::native_fg_result.load() == fg::Result::SUBMITTED
                && native::native_fg_submitted_frames.load() == submitted_before_b + 1,
            "parameter B gains exactly one submission through the real queue hook");
      const auto frame_b_pixels = pixels(backbuffer.Get(), NATIVE_INPUT_STATE);
      Check(frame_b_pixels != frame_a_pixels, "parameter B cannot reuse the previous application image");
      for (size_t i = 0; i < frame_b_pixels.size(); ++i) {
        Check(frame_b_pixels[i] == (i % 4 == 3 ? 160 : 0),
              "parameter B output contains freshly evaluated RGB and current-frame original alpha");
      }
      Check(pixels(hudless.Get(), NATIVE_INPUT_STATE) == std::vector<uint8_t>(256, 192),
            "parameter B leaves original HUDLess source pixels untouched");
      frame_a->Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 2u);
      const auto models_before_pointer_mfg = model_calls;
      const auto submitted_before_pointer_mfg = native::native_fg_submitted_frames.load();
      auto* pointer_mfg_original = native::RunNativePreFg(gpu.list.Get(), frame_a.get(), stable_handle);
      Check(pointer_mfg_original == hudless.Get() && model_calls == models_before_pointer_mfg,
            "MFG may switch back to parameter A while reusing B's latest application result");
      native::RestoreNativePreFg(gpu.list.Get(), frame_a.get(), pointer_mfg_original, true, stable_handle);
      submit_native();
      Check(native::native_fg_submitted_frames.load() == submitted_before_pointer_mfg
                && pixels(backbuffer.Get(), NATIVE_INPUT_STATE) == frame_b_pixels
                && native::PreFgResource(frame_a.get(), NVSDK_NGX_DLSSG_Parameter_HUDLess) == hudless.Get(),
            "pointer-changing MFG preserves current-frame pixels, original binding and application count");
      const auto owner_key = std::pair{true, reinterpret_cast<uintptr_t>(&owner)};
      auto& resident = native::fg_service::streams.at(owner_key);
      Check(native::fg_service::native_bindings.at(stable_handle) == owner_key.second,
            "live FG handle binds restoration to its matched output owner");
      std::array<NVSDK_NGX_Handle*, 2> resident_models{};
      for (size_t i = 0; i < resident_models.size(); ++i) {
        resident_models[i] = native::features.at(resident.roles[i].Handle()).slots[0].handle;
        Check(resident_models[i] != nullptr, "output owner retains both mature model handles");
      }
      native::ForgetSourceHandleLocked(stable_handle);
      Check(native::fg_service::streams.contains(owner_key) && resident.cached == nullptr
                && resident.native_serial == 0
                && !native::fg_service::native_bindings.contains(stable_handle),
            "FG release retains the owner model pool but discards cached application RGB");
      for (size_t i = 0; i < resident_models.size(); ++i) {
        const auto& slot = native::features.at(resident.roles[i].Handle()).slots[0];
        Check(slot.handle == resident_models[i] && slot.pending_reset,
              "FG release preserves resident models and requires fresh temporal history");
      }

      const auto* recreated_handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xF014});
      native::RegisterCreatedFeatureLocked(recreated_handle, NVSDK_NGX_Feature_FrameGeneration, frame_b.get());
      write_frame(208, 224);
      const auto models_before_recreate = model_calls;
      const auto creates_before_recreate = next_handle;
      const auto resets_before_recreate = model_resets.size();
      const auto submitted_before_recreate = native::native_fg_submitted_frames.load();
      auto* recreated_original = native::RunNativePreFg(gpu.list.Get(), frame_b.get(), recreated_handle);
      Check(recreated_original == hudless.Get() && model_calls == models_before_recreate + 2
                && next_handle == creates_before_recreate,
            "new FG handle on the same output owner immediately evaluates resident NR without warmup");
      Check(native::fg_service::native_bindings.at(recreated_handle) == owner_key.second,
            "new FG handle restores through the same resident output owner");
      Check(model_resets.size() == resets_before_recreate + 2
                && model_resets[resets_before_recreate] == 1
                && model_resets[resets_before_recreate + 1] == 1,
            "first application under the new FG handle resets both retained model histories");
      Check(native::native_fg_submitted_frames.load() == submitted_before_recreate,
            "recreated FG recording does not fabricate a native submission");
      expect_native_inputs(native::PreFgResource(frame_b.get(), NVSDK_NGX_DLSSG_Parameter_HUDLess));
      native::RestoreNativePreFg(gpu.list.Get(), frame_b.get(), recreated_original, true, recreated_handle);
      Check(native::PreFgResource(frame_b.get(), NVSDK_NGX_DLSSG_Parameter_Backbuffer) == backbuffer.Get()
                && native::PreFgResource(frame_b.get(), NVSDK_NGX_DLSSG_Parameter_HUDLess) == hudless.Get(),
            "recreated FG restores the current game's original resource bindings");
      submit_native();
      Check(native::native_fg_result.load() == fg::Result::SUBMITTED
                && native::native_fg_submitted_frames.load() == submitted_before_recreate + 1,
            "new FG handle's first application gains its own real submission proof");
      const auto recreated_pixels = pixels(backbuffer.Get(), NATIVE_INPUT_STATE);
      Check(recreated_pixels != frame_a_pixels && recreated_pixels != frame_b_pixels,
            "new FG handle cannot serve either cached image from the released feature");
      for (size_t i = 0; i < recreated_pixels.size(); ++i) {
        Check(recreated_pixels[i] == (i % 4 == 3 ? 208 : 0),
              "new FG handle enhances the fresh application RGB while preserving its current alpha");
      }
      Check(pixels(hudless.Get(), NATIVE_INPUT_STATE) == std::vector<uint8_t>(256, 224),
            "recreated FG preserves the new application's original HUDLess pixels");
      native::native_fg_owns.store(true);
      native::native_fg_owner = recreated_handle;
      native::enabled.store(false);
      Check(renodx::addons::dlss5::ComputeNrVerdict(native::CollectFunnelInputs()).state
                == renodx::addons::dlss5::NrState::kOff,
            "mid-MFG re-enable starts from a truthful Off verdict");
      native::enabled.store(true);
      native::RecreateFeatures();
      Check(renodx::addons::dlss5::ComputeNrVerdict(native::CollectFunnelInputs()).IsFgPreparing(),
            "NR reconfiguration publishes preparation before the first new FG evaluate");
      const auto reenable_models = model_calls;
      const auto reenable_submissions = native::native_fg_submitted_frames.load();
      frame_b->Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 2u);
      Check(native::RunNativePreFg(gpu.list.Get(), frame_b.get(), recreated_handle) == nullptr
                && native::native_fg_result.load() == fg::Result::PREPARED
                && model_calls == reenable_models
                && native::native_fg_submitted_frames.load() == reenable_submissions,
            "reenabling mid-MFG waits for the next base frame without warning or cached RGB");
      frame_b->Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 1u);
      // Normal output-owner retirement is the lifetime boundary for the pool.
      // This is the final scenario; no later test uses this PresentStream.
      native::RetirePresentStream(nullptr);
      Check(!native::present_streams.contains(nullptr) && !native::fg_service::streams.contains(owner_key)
                && !native::fg_service::native_bindings.contains(recreated_handle),
            "normal PresentStream retirement removes its resident native model pool");
      for (auto* model : resident_models) {
        Check(std::none_of(native::features.begin(), native::features.end(), [&](const auto& entry) {
          return std::any_of(std::begin(entry.second.slots), std::end(entry.second.slots),
                             [&](const auto& slot) { return slot.handle == model; });
        }), "retired output owner leaves no active resident model handle");
      }
      native::ForgetSourceHandleLocked(recreated_handle);
      std::cout << "PASS live FG handle retains NR across parameter objects with fresh output and real submissions\n";
      std::cout << "PASS output owner retains reset NR models across FG recreation and releases them on retirement\n";
    }
    if (info != nullptr) {
      for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
        SIZE_T bytes = 0;
        info->GetMessage(i, nullptr, &bytes);
        std::vector<uint8_t> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        Check(SUCCEEDED(info->GetMessage(i, message, &bytes)), "debug message read");
        if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR || message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
          ++state_errors;
          std::cerr << "D3D12 ERROR id=" << message->ID << ' ' << message->pDescription << '\n';
        }
      }
    }
    std::cout << "Native state evidence: game barriers=" << observed_barriers << " errors=" << state_errors << '\n';
    Check(observed_barriers != 0, "passthrough observer saw real game-input barriers");
    Check(state_errors == 0, "native FG inputs and final restoration must satisfy public NPS contract");
    std::cout << "PASS public NPS input state and final restoration\n";
    Check(scale_errors == 0, "Auto must preserve declared FG pixel scale; explicit Normalized retains dimension scaling");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}

int RunNativeOwnerIdleTest() {
  try {
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    Gpu gpu;
    model_gpu = &gpu;
    ComPtr<ID3D12InfoQueue> info;
    if (debug != nullptr) Check(SUCCEEDED(gpu.device.As(&info)), "idle test debug queue");
    native::enabled.store(true);
    native::hooks_enabled.store(true);
    native::hook_point.store(native::kHookPresent);
    native::present_pre_fg.store(1);
    native::present_guides.store(native::kPresentGuidesNever);
    native::stack_passes.store(1);
    native::codec_mode.store(0);
    native::transfer_strength.store(1.f);
    native::color_strength.store(1.f);
    native::direct_api.module = GetModuleHandleW(nullptr);
    native::direct_api.create = CreateModel;
    native::direct_api.evaluate = EvaluateModel;
    native::direct_api.release = ReleaseModel;
    native::direct_load_state = native::DirectLoadState::Ready;
    native::direct_device = gpu.device.Get();
    native::direct_device->AddRef();
    native::direct_device_native = gpu.device.Get();
    native::parameter_runtime_ready = true;
    native::core_allocate_parameters = renodx::addons::dlss5::bridge::AllocateOwnedParameters;
    native::core_destroy_parameters = renodx::addons::dlss5::bridge::DestroyOwnedParameters;
    Check(native::InstallQueueCompletionHooks(gpu.queue.Get()), "idle test real queue hooks");
    Check(native::InstallCommandListStateHooks(gpu.list.Get()), "idle test real list hooks");
    const auto submit = [&] {
      Check(SUCCEEDED(gpu.list->Close()), "idle test close");
      ID3D12CommandList* lists[]{gpu.list.Get()};
      gpu.queue->ExecuteCommandLists(1, lists);
      Check(SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial)), "idle test signal");
      HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      Check(event != nullptr, "idle test event");
      const auto signal = gpu.fence->SetEventOnCompletion(gpu.serial, event);
      const auto wait = SUCCEEDED(signal) ? WaitForSingleObject(event, 10000) : WAIT_FAILED;
      CloseHandle(event);
      Check(wait == WAIT_OBJECT_0, "idle test bounded GPU completion");
      Check(SUCCEEDED(gpu.allocator->Reset()), "idle test allocator reset");
      Check(SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)), "idle test list reset");
      submission::PruneCompletedGenerations();
      gpu.uploads.clear();
    };
    submit();
    auto backbuffer = gpu.Texture();
    auto hudless = gpu.Texture();
    gpu.Write(backbuffer.Get(), 64);
    gpu.Write(hudless.Get(), 96);
    gpu.Transition(backbuffer.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
    gpu.Transition(hudless.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
    auto& owner = native::present_streams[nullptr];
    owner.device_identity = renodx::utils::directx::NativeIdentity(gpu.device.Get());
    owner.backbuffer_width = owner.backbuffer_height = 8;
    owner.backbuffer_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    owner.prefg_target_valid = true;
    owner.prefg_target.diagnostic.bypass_host_look = true;
    owner.prefg_target.pass_transfer_strength[0] = 1.f;
    owner.prefg_target.pass_color_strength[0] = 1.f;
    renodx::addons::dlss5::bridge::OwnedParameters parameters;
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_Backbuffer, backbuffer.Get());
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_HUDLess, hudless.Get());
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 1u);
    const auto* handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xF015});
    native::RegisterCreatedFeatureLocked(handle, NVSDK_NGX_Feature_FrameGeneration, &parameters);
    ID3D12Resource* original = nullptr;
    for (uint32_t attempt = 0; attempt < 4 && original == nullptr; ++attempt) {
      original = native::RunNativePreFg(gpu.list.Get(), &parameters, handle);
      if (original == nullptr) submit();
    }
    Check(original == hudless.Get(), "idle test matures both native roles");
    native::RestoreNativePreFg(gpu.list.Get(), &parameters, original, true, handle);
    submit();
    Check(native::native_fg_result.load() == fg::Result::SUBMITTED, "idle test starts with real native submission");
    auto& resident = native::fg_service::streams.at({true, reinterpret_cast<uintptr_t>(&owner)});
    std::array<const NVSDK_NGX_Handle*, 2> role_handles{resident.roles[0].Handle(), resident.roles[1].Handle()};
    std::array<NVSDK_NGX_Handle*, 2> model_handles{};
    for (size_t i = 0; i < role_handles.size(); ++i) {
      model_handles[i] = native::features.at(role_handles[i]).slots[0].handle;
      Check(model_handles[i] != nullptr, "idle test native models are resident before waiting");
    }

    // Ordinary streams are controls: the dormant one must still lose its
    // model, while the recently rendered one makes idle retirement eligible.
    native::PresentStream dormant, active;
    auto dormant_color = gpu.Texture();
    auto active_color = gpu.Texture();
    auto final_color = gpu.Texture();
    native::max_worksets.store(6);
    for (auto* color : {dormant_color.Get(), active_color.Get(), final_color.Get()}) {
      gpu.Write(color, 80);
      gpu.Transition(color, D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
    }
    native::FeatureState contract;
    contract.input_width = contract.input_height = 8;
    contract.create_flags = NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    contract.has_motion_scale_x = contract.has_motion_scale_y = true;
    contract.motion_scale_x = contract.motion_scale_y = 8.f;
    const auto record_control = [&](native::PresentStream* stream, ID3D12Resource* color, bool neutral = false) {
      native::FillPresentBlock(stream, color, resident.roles[0].neutral_motion,
          resident.roles[0].neutral_depth, contract, 8, 8, 1.f, 0, neutral);
      auto target = owner.prefg_target;
      target.workset_key = reinterpret_cast<const ID3D12Resource*>(stream);
      target.output_state = NATIVE_INPUT_STATE;
      target.domain = (stream == &owner ? renodx::addons::dlss5::present_restore_scope::Domain::FINAL_PRESENT
                                        : renodx::addons::dlss5::present_restore_scope::Domain::OTHER);
      native::NrRecording recording;
      Check(native::ProcessInline(gpu.list.Get(), stream->Handle(neutral), &stream->parameters, nullptr, &target, &recording),
            "idle test ordinary control records through ProcessInline");
      return recording;
    };
    record_control(&dormant, dormant_color.Get());
    record_control(&active, active_color.Get());
    record_control(&owner, final_color.Get());
    record_control(&owner, final_color.Get(), true);
    submit();
    const auto* final_handle = owner.Handle();
    auto* final_model = native::features.at(final_handle).slots[0].handle;
    Check(final_model != nullptr, "matched owner already has a Final Present model before idle");
    const auto* neutral_final_handle = owner.Handle(true);
    auto* neutral_final_model = native::features.at(neutral_final_handle).slots[0].handle;
    Check(neutral_final_model != nullptr && neutral_final_model != final_model,
          "matched owner has a distinct neutral Final Present model before idle");
    Check(native::worksets.size() == 6, "idle test has two native, two owner Final Present and two ordinary worksets");
    const auto wait_started = native::SteadyNowNs();
    Sleep(static_cast<DWORD>(native::kWorksetIdleRetireNs / 1'000'000 + 100));
    Check(native::SteadyNowNs() - wait_started >= native::kWorksetIdleRetireNs,
          "idle test waits actual policy time without altering workset timestamps");
    record_control(&active, active_color.Get());
    submit();
    native::RetireSupersededWorksets(native::SteadyNowNs());
    for (size_t i = 0; i < role_handles.size(); ++i) {
      Check(!native::worksets.contains({role_handles[i], reinterpret_cast<const ID3D12Resource*>(&resident.roles[i])}),
            "idle native scratch is still retired");
      Check(native::features.at(role_handles[i]).slots[0].handle == model_handles[i],
            "live native output owner must retain its model after actual idle retirement");
    }
    Check(native::features.at(dormant.Handle()).slots[0].handle == nullptr,
          "ordinary dormant stream still releases its model");
    Check(native::features.at(active.Handle()).slots[0].handle != nullptr,
          "recent ordinary stream keeps its model");
    Check(!native::worksets.contains({final_handle, reinterpret_cast<const ID3D12Resource*>(&owner)})
              && native::features.at(final_handle).slots[0].handle == final_model,
          "matched owner Final Present scratch retires while its existing model stays resident");
    Check(!native::worksets.contains({neutral_final_handle, reinterpret_cast<const ID3D12Resource*>(&owner)})
              && native::features.at(neutral_final_handle).slots[0].handle == neutral_final_model,
          "matched owner neutral Final Present scratch retires while its model stays resident");
    const auto creates_before_final_resume = next_handle;
    const auto models_before_final_resume = model_calls;
    const auto resets_before_final_resume = model_resets.size();
    const auto final_resume = record_control(&owner, final_color.Get());
    Check(final_resume.output_ready && next_handle == creates_before_final_resume && model_calls == models_before_final_resume + 1
              && model_resets.size() == resets_before_final_resume + 1 && model_resets.back() == 1,
          "same owner Final Present immediately evaluates its retained model with Reset instead of a cold frame");
    submit();
    const auto creates_before_neutral_resume = next_handle;
    const auto calls_before_neutral_resume = model_calls;
    const auto neutral_resume = record_control(&owner, final_color.Get(), true);
    Check(neutral_resume.output_ready && next_handle == creates_before_neutral_resume
              && model_calls == calls_before_neutral_resume + 1 && model_resets.back() == 1
              && model_evaluated_handles.back() == neutral_final_model,
          "neutral Final Present immediately evaluates its retained reset model after actual idle retirement");
    submit();
    const auto creates_before_resume = next_handle;
    const auto models_before_resume = model_calls;
    const auto resets_before_resume = model_resets.size();
    gpu.Transition(backbuffer.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COMMON);
    gpu.Transition(hudless.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COMMON);
    gpu.Write(backbuffer.Get(), 160);
    gpu.Write(hudless.Get(), 192);
    gpu.Transition(backbuffer.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
    gpu.Transition(hudless.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
    original = native::RunNativePreFg(gpu.list.Get(), &parameters, handle);
    Check(original == hudless.Get() && next_handle == creates_before_resume && model_calls == models_before_resume + 2,
          "native resume recreates scratch and evaluates both resident models without a cold frame");
    Check(model_resets.size() == resets_before_resume + 2
              && model_resets[resets_before_resume] == 1 && model_resets[resets_before_resume + 1] == 1,
          "scratch retirement resets retained model histories before resume");
    native::RestoreNativePreFg(gpu.list.Get(), &parameters, original, true, handle);
    submit();
    for (auto* resource : {backbuffer.Get(), hudless.Get()}) {
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
      uint64_t bytes = 0;
      const auto desc = resource->GetDesc();
      gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
      auto readback = gpu.Buffer(bytes, D3D12_HEAP_TYPE_READBACK);
      gpu.Transition(resource, NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COPY_SOURCE);
      const D3D12_TEXTURE_COPY_LOCATION source{.pResource = resource,
          .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      const D3D12_TEXTURE_COPY_LOCATION destination{.pResource = readback.Get(),
          .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = footprint};
      gpu.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
      gpu.Transition(resource, D3D12_RESOURCE_STATE_COPY_SOURCE, NATIVE_INPUT_STATE);
      submit();
      uint8_t* mapped = nullptr;
      Check(SUCCEEDED(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped))), "idle resume readback");
      for (uint32_t y = 0; y < 8; ++y) {
        for (uint32_t x = 0; x < 32; ++x) {
          const auto expected = resource == hudless.Get() ? 192 : (x % 4 == 3 ? 160 : 0);
          Check(mapped[footprint.Offset + y * footprint.Footprint.RowPitch + x] == expected,
                "idle resume produces fresh NR pixels and preserves the current original HUDLess input");
        }
      }
      readback->Unmap(0, nullptr);
    }
    ++native::configuration_generation;
    const auto creates_before_contract = next_handle;
    Check(native::RunNativePreFg(gpu.list.Get(), &parameters, handle) == nullptr
              && next_handle > creates_before_contract,
          "incompatible configuration still rebuilds resident native models");
    submit();
    const auto owner_context = reinterpret_cast<uintptr_t>(&owner);
    native::RetirePresentStream(nullptr);
    Check(!native::fg_service::streams.contains({true, owner_context}),
          "actual output owner retirement removes native pool");
    for (auto* role_handle : role_handles) {
      Check(!native::features.contains(role_handle), "actual owner retirement removes native model states");
    }
    if (info != nullptr) {
      for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
        SIZE_T bytes = 0;
        info->GetMessage(i, nullptr, &bytes);
        std::vector<uint8_t> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        Check(SUCCEEDED(info->GetMessage(i, message, &bytes)), "idle test debug message");
        Check(message->Severity != D3D12_MESSAGE_SEVERITY_ERROR
                  && message->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION, message->pDescription);
      }
    }
    std::cout << "PASS native owner idle: actual 5 s retirement, scratch freed, models retained, immediate resume, "
                 "both same-owner Final Present handles resume, ordinary retirement, incompatible reconfigure and owner teardown\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL native owner idle: " << error.what() << '\n';
    return 1;
  }
}

int RunNativeOptionsResumeTest(bool delayed_options) {
  try {
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    Gpu gpu;
    model_gpu = &gpu;
    ComPtr<ID3D12InfoQueue> info;
    if (debug != nullptr) Check(SUCCEEDED(gpu.device.As(&info)), "options test debug queue");
    native::enabled.store(true);
    native::hooks_enabled.store(true);
    native::hook_point.store(native::kHookPresent);
    native::present_pre_fg.store(1);
    native::present_guides.store(native::kPresentGuidesNever);
    native::stack_passes.store(1);
    native::codec_mode.store(0);
    native::transfer_strength.store(1.f);
    native::color_strength.store(1.f);
    native::direct_api.module = GetModuleHandleW(nullptr);
    native::direct_api.create = CreateModel;
    native::direct_api.evaluate = EvaluateModel;
    native::direct_api.release = ReleaseModel;
    native::direct_load_state = native::DirectLoadState::Ready;
    native::direct_device = gpu.device.Get();
    native::direct_device->AddRef();
    native::direct_device_native = gpu.device.Get();
    native::parameter_runtime_ready = true;
    native::core_allocate_parameters = renodx::addons::dlss5::bridge::AllocateOwnedParameters;
    native::core_destroy_parameters = renodx::addons::dlss5::bridge::DestroyOwnedParameters;
    Check(native::InstallQueueCompletionHooks(gpu.queue.Get()), "options test real queue hooks");
    Check(native::InstallCommandListStateHooks(gpu.list.Get()), "options test real list hooks");
    const auto submit = [&] {
      Check(SUCCEEDED(gpu.list->Close()), "options test close");
      ID3D12CommandList* lists[]{gpu.list.Get()};
      gpu.queue->ExecuteCommandLists(1, lists);
      Check(SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial)), "options test signal");
      HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      Check(event != nullptr, "options test event");
      const auto signal = gpu.fence->SetEventOnCompletion(gpu.serial, event);
      const auto wait = SUCCEEDED(signal) ? WaitForSingleObject(event, 10000) : WAIT_FAILED;
      CloseHandle(event);
      Check(wait == WAIT_OBJECT_0, "options test bounded GPU completion");
      Check(SUCCEEDED(gpu.allocator->Reset()), "options test allocator reset");
      Check(SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)), "options test list reset");
      submission::PruneCompletedGenerations();
      gpu.uploads.clear();
    };
    submit();
    auto backbuffer = gpu.Texture();
    auto hudless = gpu.Texture();
    gpu.Write(backbuffer.Get(), 64);
    gpu.Write(hudless.Get(), 96);
    gpu.Transition(backbuffer.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
    gpu.Transition(hudless.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
    auto& owner = native::present_streams[nullptr];
    owner.device_identity = renodx::utils::directx::NativeIdentity(gpu.device.Get());
    owner.backbuffer_width = owner.backbuffer_height = 8;
    owner.backbuffer_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    owner.prefg_target_valid = true;
    owner.prefg_target.diagnostic.bypass_host_look = true;
    owner.prefg_target.pass_transfer_strength[0] = 1.f;
    owner.prefg_target.pass_color_strength[0] = 1.f;
    renodx::addons::dlss5::bridge::OwnedParameters parameters;
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_Backbuffer, backbuffer.Get());
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_HUDLess, hudless.Get());
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, 3u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 1u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, 1ull);
    const auto* handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xF016});
    native::RegisterCreatedFeatureLocked(handle, NVSDK_NGX_Feature_FrameGeneration, &parameters);
    native::native_fg_owner = handle;
    native::native_fg_owns.store(true);
    ID3D12Resource* original = nullptr;
    for (uint32_t attempt = 0; attempt < 4 && original == nullptr; ++attempt) {
      original = native::RunNativePreFg(gpu.list.Get(), &parameters, handle);
      if (original == nullptr) submit();
    }
    Check(original == hudless.Get(), "options test begins with mature dual NR output");
    native::RestoreNativePreFg(gpu.list.Get(), &parameters, original, true, handle);
    submit();
    const auto submitted_before_off = native::native_fg_submitted_frames.load();
    Check(native::native_fg_result.load() == fg::Result::SUBMITTED && submitted_before_off != 0,
          "options test starts with a real submitted application image");
    const auto read_pixels = [&](ID3D12Resource* resource, uint8_t rgb, uint8_t alpha, bool required = true) {
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
      uint64_t bytes = 0;
      const auto desc = resource->GetDesc();
      gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
      auto readback = gpu.Buffer(bytes, D3D12_HEAP_TYPE_READBACK);
      gpu.Transition(resource, NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COPY_SOURCE);
      const D3D12_TEXTURE_COPY_LOCATION source{.pResource = resource,
          .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      const D3D12_TEXTURE_COPY_LOCATION destination{.pResource = readback.Get(),
          .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = footprint};
      gpu.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
      gpu.Transition(resource, D3D12_RESOURCE_STATE_COPY_SOURCE, NATIVE_INPUT_STATE);
      submit();
      uint8_t* mapped = nullptr;
      Check(SUCCEEDED(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped))), "options resume readback");
      bool matches = true;
      for (uint32_t y = 0; y < 8; ++y) {
        for (uint32_t x = 0; x < 32; ++x) {
          matches &= mapped[footprint.Offset + y * footprint.Footprint.RowPitch + x] == (x % 4 == 3 ? alpha : rgb);
        }
      }
      if (!required) {
        std::cout << "DELAYED GPU rgb/alpha=" << unsigned(mapped[footprint.Offset]) << '/'
                  << unsigned(mapped[footprint.Offset + 3]) << " expected=" << unsigned(rgb) << '/'
                  << unsigned(alpha) << '\n';
      }
      readback->Unmap(0, nullptr);
      if (required) Check(matches, "options resume uses current pixels and never republishes pre-Off RGB");
      return matches;
    };
    unsigned delayed_failures = 0;
    if (delayed_options) {
      // SetOptions is actioned by the following application Present. A valid
      // old-mode NGX evaluate may therefore run after the request returns.
      namespace options = renodx::addons::dlss5::fg::native_options;
      funnel_accounting_tests::Restore substitutions;
      substitutions.Value(&native::ngx_slot_real[0]);
      substitutions.Atomic(&native::ngx_loader_published);
      substitutions.Atomic(&native::last_present_steady_ns);
      substitutions.Atomic(&native::lifecycle_tick_owed_ns);
      substitutions.Value(&options::internal::state.real);
      substitutions.Atomic(&options::internal::options_changed);
      substitutions.Atomic(&options::internal::decisions_ready);
      substitutions.Atomic(&xefg::internal::requested);
      substitutions.Atomic(&xefg::internal::owns_stream);
      substitutions.Value(&native::native_sdk_present_input);
      substitutions.Value(&native::native_passthrough_input);
      substitutions.Value(&native::native_passthrough_identity);
      int opaque_output = 0;
      int other_output = 0;
      uint64_t sdk_decision_id = 1000;
      owner.native_output_identity = &opaque_output;
      owner.native_route_id = 71;
      native::NgxLoaderSymbols loader;
      loader.nr_ready = loader.core_ready = true;
      native::ngx_loader_published.store(&loader);
      native::last_present_steady_ns.store(native::SteadyNowNs());
      native::lifecycle_tick_owed_ns.store(0);
      static std::function<void(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
          const NVSDK_NGX_Parameter*)> consume;
      static NVSDK_NGX_Result consumer_result = NVSDK_NGX_Result_Success;
      uint32_t consumer_calls = 0;
      std::vector<ComPtr<ID3D12Resource>> consumed_hudless;
      std::vector<ComPtr<ID3D12Resource>> consumed_backbuffers;
      std::vector<ComPtr<ID3D12Resource>> consumed_ui;
      consume = [&](ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* feature,
                    const NVSDK_NGX_Parameter* input) {
        Check(list != nullptr && list == gpu.list.Get() && feature == handle && input == &parameters,
              "delayed options real FG consumer receives the registered handle and valid non-null list");
        Check(native::PreFgResource(input, NVSDK_NGX_DLSSG_Parameter_Backbuffer) == backbuffer.Get(),
              "delayed options real FG consumer retains the original backbuffer binding");
        ++consumer_calls;
        consumed_hudless.emplace_back(native::PreFgResource(input, NVSDK_NGX_DLSSG_Parameter_HUDLess));
        consumed_ui.emplace_back(native::PreFgResource(input, NVSDK_NGX_DLSSG_Parameter_UI));
        auto snapshot = gpu.Texture();
        gpu.Transition(snapshot.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
        native::fg_service::CopyColor(list, snapshot.Get(), NATIVE_INPUT_STATE, backbuffer.Get(), NATIVE_INPUT_STATE);
        consumed_backbuffers.push_back(std::move(snapshot));
      };
      native::ngx_slot_real[0].evaluate_c = reinterpret_cast<void*>(+[](
          ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* feature,
          const NVSDK_NGX_Parameter* input, PFN_NVSDK_NGX_ProgressCallback_C) -> NVSDK_NGX_Result {
        consume(list, feature, input);
        return consumer_result;
      });
      native::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(+[](
          ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* feature,
          const NVSDK_NGX_Parameter* input, PFN_NVSDK_NGX_ProgressCallback) -> NVSDK_NGX_Result {
        consume(list, feature, input);
        return consumer_result;
      });
      options::internal::state.real = +[](const sl::ViewportHandle&, const sl::DLSSGOptions&) {
        return sl::Result::eOk;
      };
      options::Configure(native::OnNativeFgOptions);
      options::internal::decisions_ready.store(true);
      const auto check = [&](bool ok, const char* message) {
        std::cout << "DELAYED " << (ok ? "PASS " : "FAIL ") << message << '\n';
        if (!ok) ++delayed_failures;
      };
      ComPtr<ID3D12Resource> separate_real;
      Check(native::CreateScratchTexture(gpu.device.Get(), 8, 8, DXGI_FORMAT_R8G8B8A8_UNORM,
          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
          &separate_real, L"FG test independent real output"), "separate FG real output");
      const auto late_copy = [&] {
        // Streamline may record its real-frame copy after NGX evaluate has
        // returned. This consumer is outside both production wrappers.
        auto snapshot = gpu.Texture();
        gpu.Transition(snapshot.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
        native::fg_service::CopyColor(gpu.list.Get(), snapshot.Get(), NATIVE_INPUT_STATE,
                                     backbuffer.Get(), NATIVE_INPUT_STATE);
        return snapshot;
      };
      const auto check_scope = [&](const void* identity, bool captured, bool failed_without_guides = false) {
        const auto& input = native::native_sdk_present_input;
        check(input.decision_id == sdk_decision_id && input.decision_id != 0,
              "wrapper preserves the current nonzero SDK decision ID");
        if (captured) {
          const auto& stream = native::fg_service::streams.at({true, reinterpret_cast<uintptr_t>(&owner)});
          check(input.identity == identity && input.owner == &owner && input.route_id == owner.native_route_id
                    && input.backbuffer == backbuffer.Get() && stream.cached != nullptr
                    && input.marker == stream.cached->color[0] && input.serial != 0 && input.serial == stream.native_serial,
                "successful wrapper associates SDK scope with this owner, output, B, marker and NR serial");
        } else if (failed_without_guides) {
          check(input.identity == identity && input.owner == &owner && input.route_id == owner.native_route_id
                    && input.backbuffer == backbuffer.Get() && input.guides_unavailable
                    && input.marker == nullptr && input.serial == 0,
                "failed wrapper retains known missing-guide scope without capturing NR input or submission proof");
        } else {
          check(input.identity == identity && input.owner == nullptr && input.route_id == 0
                    && input.backbuffer == nullptr && input.marker == nullptr && input.serial == 0
                    && !input.guides_unavailable,
                "failed or different-output wrapper preserves scope identity without capturing NR input");
        }
        check(native::native_passthrough_identity == nullptr,
              "evaluate alone does not fabricate an SDK direct-Present permission");
      };
      for (const bool c_wrapper : {true, false}) {
        std::cout << "DELAYED wrapper=" << (c_wrapper ? "C" : "Ex/C++") << '\n';
        const auto evaluate = [&](const void* scope_identity = nullptr) {
          native::native_sdk_present_input = {.identity = (scope_identity != nullptr ? scope_identity : &opaque_output),
              .decision_id = sdk_decision_id};
          return c_wrapper
              ? native::HookedEvaluateFeatureCSlot<0>(gpu.list.Get(), handle, &parameters, nullptr)
              : native::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), handle, &parameters, nullptr);
        };
        sl::DLSSGOptions off;
        off.mode = sl::DLSSGMode::eOff;
        for (const bool has_output_real : {false, true}) {
          ++sdk_decision_id;
          std::cout << "DELAYED OutputReal=" << (has_output_real ? "separate" : "null") << '\n';
          parameters.Set(NVSDK_NGX_DLSSG_Parameter_OutputReal,
                         has_output_real ? separate_real.Get() : static_cast<ID3D12Resource*>(nullptr));
          parameters.Set(NVSDK_NGX_DLSSG_Parameter_OutputInterpolated, static_cast<ID3D12Resource*>(nullptr));
          native::OnNativeFgOptions(true);
          consumer_calls = 0;
          consumed_hudless.clear();
          consumed_backbuffers.clear();
          consumed_ui.clear();
          const auto submitted_before_pending_off = native::native_fg_submitted_frames.load();
          parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 1u);
          gpu.Transition(backbuffer.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COMMON);
          gpu.Transition(hudless.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COMMON);
          gpu.Write(backbuffer.Get(), 160);
          gpu.Write(hudless.Get(), 192);
          gpu.Transition(backbuffer.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
          gpu.Transition(hudless.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
          parameters.Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID,
                         (c_wrapper ? 10ull : 20ull) + (has_output_real ? 3ull : 0ull));
          const auto generation_before_off = native::native_fg_options_generation.load();
          Check(options::internal::HookedSetOptions(sl::ViewportHandle{0}, off) == sl::Result::eOk,
                "delayed options Off request returns successfully before the effective application Present");
          check(!native::native_fg_owns.load() && native::native_fg_marker.load() == nullptr
                    && native::native_fg_options_generation.load() == generation_before_off + 1,
                "changed Off relinquishes native ownership and advances the options generation");
          const auto models_before = model_calls;
          const auto creates_before = next_handle;
          Check(NVSDK_NGX_SUCCEED(evaluate()),
                "delayed options valid old-mode base evaluate reaches the real FG consumer");
          check_scope(&opaque_output, true);
          auto late_base = late_copy();
          check(consumer_calls == 1, "non-null real FG consumer was called for the pending-Off base");
          check(model_calls == models_before + 2 && next_handle == creates_before,
                "pending-Off base evaluates both mature models without creating a model");
          check(consumed_hudless.back().Get() != hudless.Get(), "pending-Off FG receives enhanced current HUDLess");
          check(native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_HUDLess) == hudless.Get(),
                "wrapper restores the original HUDLess binding");
          check(native::native_fg_submitted_frames.load() == submitted_before_pending_off,
                "recording pending-Off base does not invent submission proof");
          submit();
          check(native::native_fg_submitted_frames.load() == submitted_before_pending_off + 1,
                "pending-Off base receives real queue submission proof");
          const auto generation_before_repeat = native::native_fg_options_generation.load();
          const auto changed_before_repeat = native::native_fg_options_changed_ns.load();
          const auto reset_before_repeat = native::history_reset_epoch.load();
          const auto marker_before_repeat = native::native_fg_marker.load();
          check(native::native_fg_owns.load() && native::native_fg_owner == handle
                    && marker_before_repeat != nullptr,
                "valid pending-Off FG reacquires ownership and a submitted marker");
          Check(options::internal::HookedSetOptions(sl::ViewportHandle{0}, off) == sl::Result::eOk,
                "repeated Off request succeeds after FG reacquires ownership");
          check(native::native_fg_owns.load() && native::native_fg_owner == handle
                    && native::native_fg_marker.load() == marker_before_repeat
                    && native::native_fg_result.load() == fg::Result::SUBMITTED
                    && native::native_fg_options_generation.load() == generation_before_repeat
                    && native::native_fg_options_changed_ns.load() == changed_before_repeat
                    && native::history_reset_epoch.load() == reset_before_repeat,
                "same-mode Off preserves reacquired owner, submission proof and cache interval");
          check(read_pixels(consumed_backbuffers.back().Get(), 0, 160, false), "FG consumed current NR backbuffer pixels");
          check(read_pixels(late_base.Get(), 0, 160, false), "post-evaluate SDK consumer receives the same current NR pixels");
          check(read_pixels(backbuffer.Get(), 0, 160, false), "successful FG retains NR pixels for later real-frame consumers");
          check(read_pixels(consumed_hudless.back().Get(), 0, 192, false), "FG consumed current enhanced HUDLess pixels");
          check(read_pixels(hudless.Get(), 192, 192, false), "pending-Off original HUDLess pixels remain protected");
          const auto serial_before_duplicate = native::native_fg_recording_serial.load();
          const auto submitted_before_duplicate = native::native_fg_submitted_frames.load();
          Check(NVSDK_NGX_SUCCEED(evaluate()), "same-frame index1 evaluate reaches the FG consumer again");
          check_scope(&opaque_output, true);
          auto late_duplicate = late_copy();
          check(consumer_calls == 2 && model_calls == models_before + 2 && next_handle == creates_before
                    && native::native_fg_recording_serial.load() == serial_before_duplicate,
                "same nonzero FrameID index1 reuses NR without another model, creation or recording serial");
          check(consumed_hudless.back().Get() == consumed_hudless.front().Get(),
                "same-frame index1 keeps the cached enhanced HUDLess");
          submit();
          check(read_pixels(consumed_backbuffers.back().Get(), 0, 160, false)
                    && read_pixels(late_duplicate.Get(), 0, 160, false)
                    && read_pixels(backbuffer.Get(), 0, 160, false),
                "same-frame index1 FG and post-evaluate consumers both retain NR pixels");
          check(native::native_fg_submitted_frames.load() == submitted_before_duplicate,
                "same-frame index1 does not count another application NR submission");
          gpu.Transition(backbuffer.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COMMON);
          gpu.Write(backbuffer.Get(), 160);
          gpu.Transition(backbuffer.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
          parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 2u);
          Check(NVSDK_NGX_SUCCEED(evaluate()),
                "delayed options current-frame MFG evaluate reaches the real FG consumer");
          check_scope(&opaque_output, true);
          auto late_mfg = late_copy();
          check(consumer_calls == 3 && model_calls == models_before + 2 && next_handle == creates_before,
                "MFG index2 consumes the current base without another evaluation or model creation");
          check(consumed_hudless.back().Get() != hudless.Get()
                    && consumed_hudless.back().Get() == consumed_hudless.front().Get(),
                "MFG index2 retains the pending-Off base HUDLess replacement");
          submit();
          check(read_pixels(consumed_backbuffers.back().Get(), 0, 160, false), "MFG index2 consumes current NR base");
          check(read_pixels(late_mfg.Get(), 0, 160, false), "post-evaluate MFG consumer receives the same NR base");
          check(read_pixels(backbuffer.Get(), 0, 160, false), "successful MFG retains NR for its later consumer");
          check(read_pixels(consumed_hudless.back().Get(), 0, 192, false), "MFG index2 uses current enhanced HUDLess pixels");
          check(native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_HUDLess) == hudless.Get(),
                "MFG wrapper restores the original HUDLess binding");
          check(native::native_fg_submitted_frames.load() == submitted_before_duplicate,
                "same-frame MFG does not count another application NR submission");
          Check(NVSDK_NGX_SUCCEED(evaluate(&other_output)), "same wrapper forwards a different SDK output scope");
          check_scope(&other_output, false);
          check(model_calls == models_before + 2,
                "different SDK output identity cannot trigger another model for the same application image");
          submit();
        }
        ++sdk_decision_id;
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, 0ull);
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 1u);
        gpu.Transition(backbuffer.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COMMON);
        gpu.Write(backbuffer.Get(), 144);
        gpu.Transition(backbuffer.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
        const auto models_before_zero = model_calls;
        Check(NVSDK_NGX_SUCCEED(evaluate()), "zero FrameID base reaches the real FG wrapper");
        check_scope(&opaque_output, true);
        check(model_calls == models_before_zero + 2, "new zero FrameID scope evaluates both current NR models");
        submit();
        const auto serial_before_zero_repeat = native::native_fg_recording_serial.load();
        Check(NVSDK_NGX_SUCCEED(evaluate()), "zero FrameID index1 repeats in the same SDK scope");
        auto late_zero_repeat = late_copy();
        check_scope(&opaque_output, true);
        check(model_calls == models_before_zero + 2
                  && native::native_fg_recording_serial.load() == serial_before_zero_repeat,
              "zero FrameID repeated index1 reuses cached NR within the exact SDK scope");
        submit();
        check(read_pixels(consumed_backbuffers.back().Get(), 0, 144, false)
                  && read_pixels(late_zero_repeat.Get(), 0, 144, false),
              "zero FrameID same-scope FG and late consumers retain the same NR pixels");
        ++sdk_decision_id;
        gpu.Transition(backbuffer.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COMMON);
        gpu.Write(backbuffer.Get(), 176);
        gpu.Transition(backbuffer.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
        const auto models_before_new_scope = model_calls;
        Check(NVSDK_NGX_SUCCEED(evaluate()), "zero FrameID next SDK decision reaches the real FG wrapper");
        auto late_zero_new_scope = late_copy();
        check_scope(&opaque_output, true);
        check(model_calls == models_before_new_scope + 2,
              "zero FrameID with a new SDK decision evaluates fresh NR instead of reusing the previous scope");
        submit();
        check(read_pixels(consumed_backbuffers.back().Get(), 0, 176, false)
                  && read_pixels(late_zero_new_scope.Get(), 0, 176, false),
              "zero FrameID next-scope consumers use newly written raw B as the current NR source");
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 2u);
        gpu.Transition(backbuffer.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COMMON);
        gpu.Write(backbuffer.Get(), 160);
        gpu.Transition(backbuffer.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, 999ull);
        const auto models_before_stale = model_calls;
        Check(NVSDK_NGX_SUCCEED(evaluate()), "different-frame MFG still forwards its FG consumer");
        submit();
        check(model_calls == models_before_stale && consumed_hudless.back().Get() == hudless.Get()
                  && read_pixels(backbuffer.Get(), 160, 160, false),
              "different FrameID cannot reuse NR or restore a previous application's raw snapshot");
        // Exercise the remaining admission gates with fresh base IDs and raw
        // pixels, so a cached MFG output cannot disguise an unwanted NR call.
        for (const bool xefg_owner : {false, true}) {
          native::OnNativeFgOptions(true);
          Check(options::internal::HookedSetOptions(sl::ViewportHandle{0}, off) == sl::Result::eOk,
                "boundary Off relinquishes the prior native owner");
          native::enabled.store(xefg_owner);
          xefg::internal::requested.store(&Requested);
          xefg::internal::owns_stream.store(xefg_owner);
          Check(xefg::OwnsStream() == xefg_owner, "boundary fixture establishes effective XeFG ownership");
          parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 1u);
          parameters.Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID,
                         (c_wrapper ? 3ull : 6ull) + (xefg_owner ? 1ull : 0ull));
          gpu.Transition(backbuffer.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COMMON);
          gpu.Write(backbuffer.Get(), 160);
          gpu.Transition(backbuffer.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
          const auto models_before_skip = model_calls;
          const auto creates_before_skip = next_handle;
          const auto consumers_before_skip = consumer_calls;
          const auto submitted_before_skip = native::native_fg_submitted_frames.load();
          Check(NVSDK_NGX_SUCCEED(evaluate()), "admission gate still forwards the FG consumer");
          check(consumer_calls == consumers_before_skip + 1 && model_calls == models_before_skip
                    && next_handle == creates_before_skip && consumed_hudless.back().Get() == hudless.Get(),
                xefg_owner ? "XeFG ownership prevents native NR and passes original HUDLess"
                           : "NR disabled prevents NR and passes original HUDLess");
          check(!native::native_fg_owns.load() && native::native_fg_marker.load() == nullptr
                    && xefg::internal::owns_stream.load() == xefg_owner,
                "skipped native evaluate does not acquire ownership or steal the XeFG stream");
          submit();
          check(native::native_fg_submitted_frames.load() == submitted_before_skip,
                "skipped native evaluate cannot claim an NR submission");
          check(read_pixels(backbuffer.Get(), 160, 160, false)
                    && read_pixels(hudless.Get(), 192, 192, false),
                "admission gate leaves original backbuffer and HUDLess pixels intact");
          xefg::internal::owns_stream.store(false);
          native::enabled.store(true);
        }
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_OutputReal, backbuffer.Get());
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 1u);
        const auto models_before_alias = model_calls;
        Check(NVSDK_NGX_SUCCEED(evaluate()), "unsupported alias forwards untouched FG parameters");
        submit();
        check(model_calls == models_before_alias && consumed_hudless.back().Get() == hudless.Get()
                  && read_pixels(backbuffer.Get(), 160, 160, false),
              "input/output alias cannot be enhanced and then overwritten by input restoration");
        parameters.Set(NVSDK_NGX_DLSSG_Parameter_OutputReal, static_cast<void*>(nullptr));
      }
      auto failure_ui = gpu.Texture();
      gpu.Write(failure_ui.Get(), 255);
      gpu.Transition(failure_ui.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
      unsigned long long failure_frame_id = 100;
      for (const bool c_wrapper : {true, false}) {
        for (const bool has_output_real : {false, true}) {
          std::cout << "DELAYED failure wrapper=" << (c_wrapper ? "C" : "Ex/C++")
                    << " OutputReal=" << (has_output_real ? "separate" : "null") << '\n';
          const auto evaluate = [&] {
            native::native_sdk_present_input = {.identity = &opaque_output, .decision_id = sdk_decision_id};
            return c_wrapper
                ? native::HookedEvaluateFeatureCSlot<0>(gpu.list.Get(), handle, &parameters, nullptr)
                : native::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), handle, &parameters, nullptr);
          };
          native::OnNativeFgOptions(true);
          consumer_result = NVSDK_NGX_Result_Success;
          parameters.Set(NVSDK_NGX_DLSSG_Parameter_OutputReal,
                         has_output_real ? separate_real.Get() : static_cast<ID3D12Resource*>(nullptr));
          parameters.Set(NVSDK_NGX_DLSSG_Parameter_OutputInterpolated, static_cast<ID3D12Resource*>(nullptr));
          parameters.Set(NVSDK_NGX_DLSSG_Parameter_UI, static_cast<ID3D12Resource*>(nullptr));
          parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 1u);
          // A failed real evaluate retires its NR models. Warm the next case
          // through the same real wrappers before exercising failure again.
          bool ready = false;
          for (uint32_t attempt = 0; attempt < 4 && !ready; ++attempt) {
            ++sdk_decision_id;
            parameters.Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, ++failure_frame_id);
            Check(NVSDK_NGX_SUCCEED(evaluate()), "failure fixture prepares mature NR through the real wrapper");
            const auto& stream = native::fg_service::streams.at({true, reinterpret_cast<uintptr_t>(&owner)});
            ready = stream.cached != nullptr;
            submit();
          }
          Check(ready, "failure fixture has mature NR before real FG failure");
          for (auto [resource, value] : {std::pair{backbuffer.Get(), uint8_t{208}},
                                        std::pair{hudless.Get(), uint8_t{224}}}) {
            gpu.Transition(resource, NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COMMON);
            gpu.Write(resource, value);
            gpu.Transition(resource, D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
          }
          parameters.Set(NVSDK_NGX_DLSSG_Parameter_UI, failure_ui.Get());
          parameters.Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, ++failure_frame_id);
          ++sdk_decision_id;
          const auto calls_before_failure = consumer_calls;
          const auto models_before_failure = model_calls;
          const auto submitted_before_failure = native::native_fg_submitted_frames.load();
          consumer_result = NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
          Check(native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_Depth) == nullptr
                    || native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_MVecs) == nullptr,
                "failure fixture explicitly lacks current guides");
          Check(evaluate() == consumer_result, "wrapper preserves the real FG failure result");
          auto late_failed = late_copy();
          check_scope(&opaque_output, false, true);
          check(consumer_calls == calls_before_failure + 1 && model_calls > models_before_failure
                    && consumed_hudless.back().Get() != hudless.Get() && consumed_ui.back().Get() != failure_ui.Get(),
                "failing FG actually consumed enhanced B and substituted HUDLess/UI before rollback");
          check(native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_HUDLess) == hudless.Get()
                    && native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_UI) == failure_ui.Get(),
                "failed FG restores original HUDLess and UI bindings");
          check(native::native_fg_marker.load() == nullptr && native::native_fg_result.load() == fg::Result::FAILED,
                "failed FG clears current NR submission proof");
          submit();
          check(read_pixels(consumed_backbuffers.back().Get(), 0, 208, false),
                "FG-internal failure snapshot retains the NR pixels consumed before the error");
          check(read_pixels(late_failed.Get(), 208, 208, false) && read_pixels(backbuffer.Get(), 208, 208, false),
                "failed FG rolls back current raw B before any post-evaluate consumer");
          check(read_pixels(hudless.Get(), 224, 224, false) && read_pixels(failure_ui.Get(), 255, 255, false),
                "failed FG preserves original HUDLess and UI GPU pixels");
          check(native::native_fg_submitted_frames.load() == submitted_before_failure,
                "failed FG does not publish a successful NR application submission");
          consumer_result = NVSDK_NGX_Result_Success;
          parameters.Set(NVSDK_NGX_DLSSG_Parameter_UI, static_cast<ID3D12Resource*>(nullptr));
        }
      }
      consume = {};
    } else {
      native::ObservePresentFrameGeneration(&parameters);
      Check(native::present_fg_declaration.multiplier == 4, "options test has a real pre-Off 4x declaration");
      const auto options_before_off = native::native_fg_options_generation.load();
      const auto pre_off_declaration_ns = native::present_fg_declaration.observed_ns;
      native::OnNativeFgOptions(false);
      Check(!native::native_fg_owns.load() && native::native_fg_marker.load() == nullptr,
            "public Off relinquishes ownership and current submission marker");
      Check(native::native_fg_options_generation.load() == options_before_off + 1
                && native::native_fg_options_changed_ns.load() >= pre_off_declaration_ns,
            "public Off advances the cache interval and fences out its earlier 4x declaration");
      gpu.Transition(backbuffer.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COMMON);
      gpu.Transition(hudless.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COMMON);
      gpu.Write(backbuffer.Get(), 160);
      gpu.Write(hudless.Get(), 192);
      gpu.Transition(backbuffer.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
      gpu.Transition(hudless.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, 2ull);
      submit();
      native::OnNativeFgOptions(true);
      Check(!native::native_fg_owns.load(), "public On does not manufacture native ownership");
      Check(native::native_fg_options_generation.load() == options_before_off + 2
                && pre_off_declaration_ns <= native::native_fg_options_changed_ns.load(),
            "public On starts a new declaration interval rather than reviving pre-Off pacing");
      native::OnNativeFgOptions(true);
      Check(native::native_fg_options_generation.load() == options_before_off + 2,
            "repeated enabled options do not invalidate a current application frame");
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 2u);
      const auto creates_before_resume = next_handle;
      const auto models_before_resume = model_calls;
      auto* mfg_original = native::RunNativePreFg(gpu.list.Get(), &parameters, handle);
      Check(mfg_original == nullptr && model_calls == models_before_resume,
            "MFG resume after public Off cannot reuse the old application's cached RGB");
      Check(native::native_fg_marker.load() == nullptr && native::native_fg_submitted_frames.load() == submitted_before_off
                && native::native_fg_result.load() != fg::Result::SUBMITTED,
            "MFG resume without a new base cannot claim current submission proof");
      submit();
      read_pixels(backbuffer.Get(), 160, 160);
      read_pixels(hudless.Get(), 192, 192);
      native::ObservePresentFrameGeneration(&parameters);
      Check(native::present_fg_declaration.multiplier == 4
                && native::present_fg_declaration.observed_ns > native::native_fg_options_changed_ns.load(),
            "fresh FG observation after On carries a declaration from the current interval");
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 1u);
      const auto resets_before_base = model_resets.size();
      original = native::RunNativePreFg(gpu.list.Get(), &parameters, handle);
      Check(original == hudless.Get() && model_calls == models_before_resume + 2 && next_handle == creates_before_resume,
            "new base after public On immediately evaluates existing mature models");
      Check(model_resets.size() == resets_before_base + 2 && model_resets[resets_before_base] == 1
                && model_resets[resets_before_base + 1] == 1,
            "new base resets temporal histories across the public options boundary");
      native::RestoreNativePreFg(gpu.list.Get(), &parameters, original, true, handle);
      Check(native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_HUDLess) == hudless.Get(),
            "options resume restores the current original HUDLess binding");
      submit();
      Check(native::native_fg_submitted_frames.load() == submitted_before_off + 1
                && native::native_fg_result.load() == fg::Result::SUBMITTED,
            "only the new base gains its own real submission");
      read_pixels(backbuffer.Get(), 0, 160);
      read_pixels(hudless.Get(), 192, 192);
    }
    native::RetirePresentStream(nullptr);
    native::ForgetSourceHandleLocked(handle);
    if (info != nullptr) {
      for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
        SIZE_T bytes = 0;
        info->GetMessage(i, nullptr, &bytes);
        std::vector<uint8_t> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        Check(SUCCEEDED(info->GetMessage(i, message, &bytes)), "options test debug message");
        Check(message->Severity != D3D12_MESSAGE_SEVERITY_ERROR
                  && message->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION, message->pDescription);
      }
    }
    Check(delayed_failures == 0, "native delayed FG consumer lifetime, rollback and SDK scope contracts");
    if (delayed_options) {
      std::cout << "PASS native delayed options: C/Ex and null/separate OutputReal preserve late consumers, duplicate index1/MFG, "
                   "failure rollback and SDK scope association; admission gates respected; WARP/fake NR and FG only, not game proof\n";
    } else {
      std::cout << "PASS native public options resume: MFG cache invalidated, current pixels protected, "
                   "new base evaluates mature reset models and owns real submission proof\n";
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << (delayed_options ? "FAIL native delayed options: " : "FAIL native public options resume: ")
              << error.what() << '\n';
    return 1;
  }
}

int RunPresentGuideModelsTest() {
  try {
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    Gpu gpu;
    model_gpu = &gpu;
    ComPtr<ID3D12InfoQueue> info;
    if (debug != nullptr) Check(SUCCEEDED(gpu.device.As(&info)), "Present models debug queue");
    native::enabled.store(true);
    native::hooks_enabled.store(true);
    native::hook_point.store(native::kHookPresent);
    native::present_pre_fg.store(1);
    native::present_guides.store(native::kPresentGuidesOptional);
    native::stack_passes.store(1);
    native::nr_follow_input_res.store(false);
    native::nr_resolution_scale.store(1.f);
    native::codec_mode.store(0);
    native::transfer_strength.store(1.f);
    native::color_strength.store(1.f);
    native::direct_api.module = GetModuleHandleW(nullptr);
    native::direct_api.create = CreateModel;
    native::direct_api.evaluate = EvaluateModel;
    native::direct_api.release = ReleaseModel;
    native::direct_load_state = native::DirectLoadState::Ready;
    native::direct_device = gpu.device.Get();
    native::direct_device->AddRef();
    native::direct_device_native = gpu.device.Get();
    native::parameter_runtime_ready = true;
    native::core_allocate_parameters = renodx::addons::dlss5::bridge::AllocateOwnedParameters;
    native::core_destroy_parameters = renodx::addons::dlss5::bridge::DestroyOwnedParameters;
    Check(native::InstallQueueCompletionHooks(gpu.queue.Get()), "Present models real queue hooks");
    Check(native::InstallCommandListStateHooks(gpu.list.Get()), "Present models real list hooks");
    const auto submit = [&] {
      Check(SUCCEEDED(gpu.list->Close()), "Present models close");
      ID3D12CommandList* lists[]{gpu.list.Get()};
      gpu.queue->ExecuteCommandLists(1, lists);
      Check(SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial)), "Present models signal");
      HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      Check(event != nullptr, "Present models event");
      const auto signal = gpu.fence->SetEventOnCompletion(gpu.serial, event);
      const auto wait = (SUCCEEDED(signal) ? WaitForSingleObject(event, 10000) : WAIT_FAILED);
      CloseHandle(event);
      Check(wait == WAIT_OBJECT_0, "Present models bounded GPU completion");
      Check(SUCCEEDED(gpu.allocator->Reset()), "Present models allocator reset");
      Check(SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)), "Present models list reset");
      submission::PruneCompletedGenerations();
      gpu.uploads.clear();
    };
    submit();
    auto color = gpu.Texture();
    auto hudless = gpu.Texture();
    gpu.Write(color.Get(), 64);
    gpu.Write(hudless.Get(), 96);
    gpu.Transition(color.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
    gpu.Transition(hudless.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
    auto& owner = native::present_streams[nullptr];
    owner.device_identity = renodx::utils::directx::NativeIdentity(gpu.device.Get());
    owner.backbuffer_width = owner.backbuffer_height = 8;
    owner.backbuffer_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    owner.prefg_target_valid = true;
    owner.prefg_target.diagnostic.bypass_host_look = true;
    owner.prefg_target.pass_transfer_strength[0] = 1.f;
    owner.prefg_target.pass_color_strength[0] = 1.f;
    renodx::addons::dlss5::bridge::OwnedParameters fg_parameters;
    fg_parameters.Set(NVSDK_NGX_DLSSG_Parameter_Backbuffer, color.Get());
    fg_parameters.Set(NVSDK_NGX_DLSSG_Parameter_HUDLess, hudless.Get());
    fg_parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 1u);
    const auto* fg_handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xF018});
    native::RegisterCreatedFeatureLocked(fg_handle, NVSDK_NGX_Feature_FrameGeneration, &fg_parameters);
    // Exercise the real output-owner registration rather than seeding its resident set.
    auto* original = native::RunNativePreFg(gpu.list.Get(), &fg_parameters, fg_handle);
    if (original != nullptr) {
      native::RestoreNativePreFg(gpu.list.Get(), &fg_parameters, original, true, fg_handle);
    }
    submit();

    std::array<ComPtr<ID3D12Resource>, 4> guides;
    for (size_t i = 0; i < guides.size(); ++i) {
      const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
      const uint32_t size = (i < 2 ? 4u : 8u);
      const D3D12_RESOURCE_DESC desc{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
          .Width = size, .Height = size, .DepthOrArraySize = 1, .MipLevels = 1,
          .Format = (i % 2 == 0 ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R32_FLOAT), .SampleDesc = {1, 0}};
      Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&guides[i]))), "Present models guide texture");
      gpu.Write(guides[i].Get(), 0);
      gpu.Transition(guides[i].Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
    }
    std::array<native::FeatureState, 2> contracts;
    for (size_t i = 0; i < contracts.size(); ++i) {
      auto& contract = contracts[i];
      contract.input_width = contract.input_height = (i == 0 ? 4u : 8u);
      contract.create_flags = NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
      if (i == 0) {
        contract.create_flags |= NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
      }
      contract.has_motion_scale_x = contract.has_motion_scale_y = true;
      contract.motion_scale_x = contract.motion_scale_y = static_cast<float>(contract.input_width);
    }
    auto target = owner.prefg_target;
    target.workset_key = reinterpret_cast<const ID3D12Resource*>(&owner);
    target.output_state = NATIVE_INPUT_STATE;
    target.domain = renodx::addons::dlss5::present_restore_scope::Domain::FINAL_PRESENT;
    const auto record = [&](bool neutral, int reset) {
      const size_t index = (neutral ? 1 : 0);
      native::FillPresentBlock(&owner, color.Get(), guides[index * 2].Get(), guides[index * 2 + 1].Get(),
          contracts[index], 8, 8, 1.f, reset, neutral);
      native::NrRecording recording;
      Check(native::ProcessInline(gpu.list.Get(), owner.Handle(neutral), &owner.parameters,
                nullptr, &target, &recording), "Present models actual ProcessInline records");
      return recording;
    };
    const auto expect_pixels = [&](uint8_t alpha) {
      const auto desc = color->GetDesc();
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
      uint64_t bytes = 0;
      gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
      auto readback = gpu.Buffer(bytes, D3D12_HEAP_TYPE_READBACK);
      gpu.Transition(color.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COPY_SOURCE);
      const D3D12_TEXTURE_COPY_LOCATION from{.pResource = color.Get(),
          .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      const D3D12_TEXTURE_COPY_LOCATION to{.pResource = readback.Get(),
          .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = footprint};
      gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
      gpu.Transition(color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, NATIVE_INPUT_STATE);
      submit();
      uint8_t* mapped = nullptr;
      Check(SUCCEEDED(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped))), "Present models readback");
      bool matches = true;
      for (uint32_t y = 0; y < 8; ++y) {
        for (uint32_t x = 0; x < 32; ++x) {
          matches &= mapped[footprint.Offset + y * footprint.Footprint.RowPitch + x] == (x % 4 == 3 ? alpha : 0);
        }
      }
      readback->Unmap(0, nullptr);
      Check(matches, "Present models fresh GPU NR RGB and original alpha, without reducing strength");
    };
    for (bool neutral : {false, true}) {
      native::NrRecording recording;
      for (uint32_t attempt = 0; attempt < 4 && !recording.output_ready; ++attempt) {
        recording = record(neutral, 0);
        submit();
      }
      Check(recording.output_ready, "Present models independently warm both contracts");
    }
    const auto creates_after_warmup = next_handle;
    // Run this before identity assertions so the API-compatible single-handle
    // control fails on the observed cold output / model recreation regression.
    auto calls_before = model_calls;
    const auto switched = record(false, 0);
    std::cout << "PRESENT_GUIDE_MODELS first_switch ready=" << switched.output_ready
              << " creates_before=" << creates_after_warmup << " creates_after=" << next_handle
              << " evaluations=" << model_calls - calls_before << '\n';
    Check(switched.output_ready && model_calls == calls_before + 1 && next_handle == creates_after_warmup,
          "Present guide switch must evaluate immediately: unexpected warmup or model rebuild");
    submit();
    const std::array handles{owner.Handle(), owner.Handle(true)};
    const std::array models{native::features.at(handles[0]).slots[0].handle,
                            native::features.at(handles[1]).slots[0].handle};
    Check(handles[0] != handles[1] && models[0] != models[1], "Present models have distinct real feature handles");
    Check(model_create_flags.at(models[0]) == 0xa && model_create_flags.at(models[1]) == 0x8,
          "Present models preserve actual NR create ABI flags for each guide contract");
    for (const auto* handle : handles) {
      Check(native::owner_resident_nr_handles.contains(handle), "real FG owner retains both Final Present models");
    }
    for (uint32_t round = 0; round < 6; ++round) {
      native::ResetPresentStreamHistory(&owner);
      for (bool neutral : {true, false}) {
        const auto index = (neutral ? 1u : 0u);
        for (int repeat = 0; repeat < 2; ++repeat) {
          const auto value = static_cast<uint8_t>(112 + round * 8 + index * 2 + repeat);
          gpu.Transition(color.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COMMON);
          gpu.Write(color.Get(), value);
          gpu.Transition(color.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
          calls_before = model_calls;
          const auto recording = record(neutral, 0);
          Check(recording.output_ready && recording.model_recorded && model_calls == calls_before + 1
                    && next_handle == creates_after_warmup, "Present repeated switching has no cold frames or new creates");
          Check(model_evaluated_handles.back() == models[index] && model_resets.back() == (repeat == 0 ? 1 : 0),
                "Present history reset reaches each actual model once without consuming the other's reset");
          Check(native::transfer_strength.load() == 1.f && native::color_strength.load() == 1.f
                    && target.pass_transfer_strength[0] == 1.f && target.pass_color_strength[0] == 1.f,
                "Present switching preserves requested input strengths");
          expect_pixels(value);
        }
      }
    }
    // Leave both models referenced by a real, unsubmitted list at retirement.
    record(false, 0);
    record(true, 0);
    for (size_t i = 0; i < handles.size(); ++i) {
      Check(native::worksets.contains({handles[i], target.workset_key}) && !submission::ResourceReleasable(models[i]),
            "both Final Present worksets and recorded model uses exist before retirement");
    }
    const auto owner_context = reinterpret_cast<uintptr_t>(&owner);
    native::RetirePresentStream(nullptr);
    Check(!native::present_streams.contains(nullptr) && !native::fg_service::streams.contains({true, owner_context}),
          "actual PresentStream retirement removes its output owner and native pool");
    native::DrainRetiredResources(true);
    for (size_t i = 0; i < handles.size(); ++i) {
      Check(!native::features.contains(handles[i]) && !native::worksets.contains({handles[i], target.workset_key})
                && !native::owner_resident_nr_handles.contains(handles[i]), "retirement removes both features, worksets and residency");
      Check(std::find(model_released_handles.begin(), model_released_handles.end(), models[i]) == model_released_handles.end(),
            "retirement cannot release an NR model still referenced by the unsubmitted GPU list");
      Check(std::any_of(native::retired_nr_features.begin(), native::retired_nr_features.end(), [&](const auto& entry) {
        return entry.handle == models[i] && entry.tracker_proof;
      }), "both models await exact tracker retirement");
    }
    Check(std::count_if(native::retired_final_resources.begin(), native::retired_final_resources.end(), [&](const auto& entry) {
      return entry.tracker_proof && !native::WorksetRecordingsComplete(entry.resources);
    }) >= 2, "both retired worksets retain their pending GPU recordings");
    submit();
    native::DrainRetiredResources(true);
    for (const auto* model : models) {
      Check(std::count(model_released_handles.begin(), model_released_handles.end(), model) == 1,
            "exact GPU completion releases each retired model once");
    }
    Check(native::retired_nr_features.empty() && native::retired_final_resources.empty(),
          "exact GPU completion drains retired model and workset resources");
    native::ForgetSourceHandleLocked(fg_handle);
    if (info != nullptr) {
      for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
        SIZE_T bytes = 0;
        info->GetMessage(i, nullptr, &bytes);
        std::vector<uint8_t> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        Check(SUCCEEDED(info->GetMessage(i, message, &bytes)), "Present models debug message");
        Check(message->Severity != D3D12_MESSAGE_SEVERITY_ERROR
                  && message->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION, message->pDescription);
      }
    }
    std::cout << "PASS Present guide models: warm real/neutral contracts, 24 fresh GPU outputs, stable creates, "
                 "independent Reset and exact tracker retirement\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL Present guide models: " << error.what() << '\n';
    return 1;
  }
}

int RunNativeGuideContractTest() {
  try {
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    Gpu gpu;
    model_gpu = &gpu;
    ComPtr<ID3D12InfoQueue> info;
    if (debug != nullptr) Check(SUCCEEDED(gpu.device.As(&info)), "guide contract debug queue");
    native::enabled.store(true);
    native::hooks_enabled.store(true);
    native::hook_point.store(native::kHookPresent);
    native::present_pre_fg.store(1);
    native::present_guides.store(native::kPresentGuidesOptional);
    native::stack_passes.store(1);
    native::nr_follow_input_res.store(false);
    native::nr_resolution_scale.store(1.f);
    native::codec_mode.store(0);
    native::transfer_strength.store(1.f);
    native::color_strength.store(1.f);
    native::direct_api.module = GetModuleHandleW(nullptr);
    native::direct_api.create = CreateModel;
    native::direct_api.evaluate = EvaluateModel;
    native::direct_api.release = ReleaseModel;
    native::direct_load_state = native::DirectLoadState::Ready;
    native::direct_device = gpu.device.Get();
    native::direct_device->AddRef();
    native::direct_device_native = gpu.device.Get();
    native::parameter_runtime_ready = true;
    native::core_allocate_parameters = renodx::addons::dlss5::bridge::AllocateOwnedParameters;
    native::core_destroy_parameters = renodx::addons::dlss5::bridge::DestroyOwnedParameters;
    Check(native::InstallQueueCompletionHooks(gpu.queue.Get()), "guide contract real queue hooks");
    Check(native::InstallCommandListStateHooks(gpu.list.Get()), "guide contract real list hooks");
    const auto submit = [&] {
      Check(SUCCEEDED(gpu.list->Close()), "guide contract close");
      ID3D12CommandList* lists[]{gpu.list.Get()};
      gpu.queue->ExecuteCommandLists(1, lists);
      Check(SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial)), "guide contract signal");
      HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      Check(event != nullptr, "guide contract event");
      const auto signal = gpu.fence->SetEventOnCompletion(gpu.serial, event);
      const auto wait = SUCCEEDED(signal) ? WaitForSingleObject(event, 10000) : WAIT_FAILED;
      CloseHandle(event);
      Check(wait == WAIT_OBJECT_0, "guide contract bounded GPU completion");
      Check(SUCCEEDED(gpu.allocator->Reset()), "guide contract allocator reset");
      Check(SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)), "guide contract list reset");
      submission::PruneCompletedGenerations();
      gpu.uploads.clear();
    };
    submit();
    auto backbuffer = gpu.Texture();
    auto hudless = gpu.Texture();
    gpu.Write(backbuffer.Get(), 64);
    gpu.Write(hudless.Get(), 96);
    gpu.Transition(backbuffer.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
    gpu.Transition(hudless.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
    ComPtr<ID3D12Resource> motion, depth;
    for (auto [resource, format] : {std::pair{motion.GetAddressOf(), DXGI_FORMAT_R16G16_FLOAT},
                                  std::pair{depth.GetAddressOf(), DXGI_FORMAT_R32_FLOAT}}) {
      const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
      const D3D12_RESOURCE_DESC desc{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
          .Width = 4, .Height = 4, .DepthOrArraySize = 1, .MipLevels = 1, .Format = format,
          .SampleDesc = {1, 0}};
      Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                NATIVE_INPUT_STATE, nullptr, IID_PPV_ARGS(resource))),
            "guide contract actual 4x4 guide resource");
    }
    auto& owner = native::present_streams[nullptr];
    owner.device_identity = renodx::utils::directx::NativeIdentity(gpu.device.Get());
    owner.backbuffer_width = owner.backbuffer_height = 8;
    owner.backbuffer_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    owner.prefg_target_valid = true;
    owner.prefg_target.diagnostic.bypass_host_look = true;
    owner.prefg_target.pass_transfer_strength[0] = 1.f;
    owner.prefg_target.pass_color_strength[0] = 1.f;
    renodx::addons::dlss5::bridge::OwnedParameters parameters;
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_Backbuffer, backbuffer.Get());
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_HUDLess, hudless.Get());
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 1u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_DepthInverted, 1u);
    const auto* handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xF017});
    native::RegisterCreatedFeatureLocked(handle, NVSDK_NGX_Feature_FrameGeneration, &parameters);
    const auto record_frame = [&] {
      ID3D12Resource* original = nullptr;
      for (uint32_t attempt = 0; attempt < 4 && original == nullptr; ++attempt) {
        original = native::RunNativePreFg(gpu.list.Get(), &parameters, handle);
        if (original == nullptr) submit();
      }
      Check(original == hudless.Get(), "guide contract reaches real mature dual NR output");
      native::RestoreNativePreFg(gpu.list.Get(), &parameters, original, true, handle);
      submit();
      Check(native::native_fg_result.load() == fg::Result::SUBMITTED,
            "guide contract frame has a real native submission");
    };
    record_frame();
    auto& stream = native::fg_service::streams.at({true, reinterpret_cast<uintptr_t>(&owner)});
    const auto models = [&] {
      return std::array{native::features.at(stream.roles[0].Handle()).slots[0].handle,
                        native::features.at(stream.roles[1].Handle()).slots[0].handle};
    };
    const auto matches_flags = [&](const char* phase, uint32_t expected) {
      bool matches = true;
      for (uint32_t role = 0; role < 2; ++role) {
        const auto& feature = native::features.at(stream.roles[role].Handle());
        const auto& slot = feature.slots[0];
        const auto actual = model_create_flags.at(slot.handle);
        Check(feature.input_width == 8 && feature.input_height == 8
                  && feature.output_width == 8 && feature.output_height == 8 && slot.nr_hdr_mode == 0,
              "guide contract changes neither native working/output dimensions nor HDR");
        std::cout << "GUIDE_CONTRACT phase=" << phase << " role=" << role << " model=" << slot.handle
                  << " ABI_create_flags=0x" << std::hex << actual << " current_flags=0x" << feature.create_flags
                  << " expected=0x" << expected << std::dec << '\n';
        matches &= actual == expected;
      }
      return matches;
    };
    Check(matches_flags("neutral_initial", 0x8), "neutral models are really created with ABI flags 0x8");
    const auto neutral_models = models();
    auto creates_before_same = next_handle;
    record_frame();
    Check(models() == neutral_models && next_handle == creates_before_same,
          "identical neutral effective flags do not rebuild models");

    parameters.Set(NVSDK_NGX_DLSSG_Parameter_Depth, depth.Get());
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MVecs, motion.Get());
    parameters.Set("DLSSG.MVecsSubrectWidth", 4u);
    parameters.Set("DLSSG.MVecsSubrectHeight", 4u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MvecScaleX, 1.f);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MvecScaleY, 1.f);
    record_frame();
    const auto real_models = models();
    const bool forward_flags = matches_flags("neutral_to_real", 0xa);
    const bool forward_recreated = real_models[0] != neutral_models[0] && real_models[1] != neutral_models[1];
    creates_before_same = next_handle;
    record_frame();
    Check(models() == real_models && next_handle == creates_before_same,
          "unchanged real guide effective flags do not rebuild models");

    // Diagnostic control only: F6's existing rebuild must create from the
    // current real-guide contract, without changing the guides or settings.
    native::RecreateFeatures();
    record_frame();
    Check(matches_flags("recreate_control", 0xa), "RecreateFeatures rebuilds both models with current ABI flags 0xa");
    const auto recreated_models = models();
    Check(recreated_models[0] != real_models[0] && recreated_models[1] != real_models[1],
          "RecreateFeatures control obtains new model handles");
    creates_before_same = next_handle;
    record_frame();
    Check(models() == recreated_models && next_handle == creates_before_same,
          "same effective flags after RecreateFeatures remain stable");
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_Depth, static_cast<ID3D12Resource*>(nullptr));
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MVecs, static_cast<ID3D12Resource*>(nullptr));
    record_frame();
    const auto fallback_models = models();
    const bool reverse_flags = matches_flags("real_to_neutral", 0x8);
    const bool reverse_recreated = fallback_models[0] != recreated_models[0] && fallback_models[1] != recreated_models[1];
    creates_before_same = next_handle;
    record_frame();
    Check(models() == fallback_models && next_handle == creates_before_same,
          "unchanged fallback effective flags do not rebuild models");

    // FillPresentBlock deliberately masks the raw HDR bits. Exercise the
    // common consumer directly to distinguish raw flags from baked flags.
    auto& hdr_role = stream.roles[0];
    auto hdr_target = owner.prefg_target;
    hdr_target.hdr_mode = 2;
    hdr_target.output_state = NATIVE_INPUT_STATE;
    hdr_target.workset_key = reinterpret_cast<const ID3D12Resource*>(&hdr_role);
    hdr_target.domain = renodx::addons::dlss5::present_restore_scope::Domain::PRE_FG_BACKBUFFER;
    const auto record_hdr = [&](uint32_t raw_flags) {
      hdr_role.parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, raw_flags);
      native::NrRecording recording;
      for (uint32_t attempt = 0; attempt < 3 && !recording.output_ready; ++attempt) {
        Check(native::ProcessInline(gpu.list.Get(), hdr_role.Handle(), &hdr_role.parameters,
                  nullptr, &hdr_target, &recording), "HDR guide contract records through common consumer");
        submit();
      }
      Check(recording.output_ready, "HDR guide contract reaches a real evaluated output");
    };
    record_hdr(0x8);
    auto* hdr_model = native::features.at(hdr_role.Handle()).slots[0].handle;
    Check(model_create_flags.at(hdr_model) == 0x49,
          "HDR bakes IsHDR and AutoExposure into raw neutral flags 0x8");
    const auto creates_before_equivalent_hdr = next_handle;
    for (uint32_t raw_flags : {0x49u, 0x8u, 0x49u}) {
      record_hdr(raw_flags);
      const auto& feature = native::features.at(hdr_role.Handle());
      Check(feature.slots[0].handle == hdr_model && next_handle == creates_before_equivalent_hdr
                && model_create_flags.at(hdr_model) == 0x49,
            "raw HDR/AutoExposure changes with identical baked flags must not recreate the model");
    }
    std::cout << "GUIDE_CONTRACT HDR_equivalent_raw_flags=0x8,0x49 baked_flags=0x49 model_retained=1\n";
    native::RetirePresentStream(nullptr);
    native::ForgetSourceHandleLocked(handle);
    if (info != nullptr) {
      for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
        SIZE_T bytes = 0;
        info->GetMessage(i, nullptr, &bytes);
        std::vector<uint8_t> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        Check(SUCCEEDED(info->GetMessage(i, message, &bytes)), "guide contract debug message");
        Check(message->Severity != D3D12_MESSAGE_SEVERITY_ERROR
                  && message->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION, message->pDescription);
      }
    }
    std::cout << "GUIDE_CONTRACT forward_flags=" << forward_flags << " forward_recreated=" << forward_recreated
              << " reverse_flags=" << reverse_flags << " reverse_recreated=" << reverse_recreated << '\n';
    Check(forward_flags && forward_recreated && reverse_flags && reverse_recreated,
          "guide create-contract changes must rebuild models in both directions; historical ABI flags cannot silently persist");
    std::cout << "PASS native guide contracts rebuild incompatible models and preserve identical effective flags\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL native guide contract: " << error.what() << '\n';
    return 1;
  }
}

int RunNativePresentDecisionTest() {
  try {
    namespace options = renodx::addons::dlss5::fg::native_options;
    using Phase = options::PresentPhase;
    Gpu gpu;
    HWND window = CreateWindowExW(0, L"STATIC", L"NR WARP routing", WS_OVERLAPPED,
        0, 0, 8, 8, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "routing hidden test window");
    const std::unique_ptr<std::remove_pointer_t<HWND>, decltype(&DestroyWindow)> window_owner(window, &DestroyWindow);
    ComPtr<IDXGIFactory4> factory;
    Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "routing DXGI factory");
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = desc.Height = 8;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferCount = 2;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> swapchain;
    Check(SUCCEEDED(factory->CreateSwapChainForHwnd(gpu.queue.Get(), window, &desc, nullptr, nullptr, &swapchain)),
          "routing real WARP swapchain");
    const void* identity = renodx::utils::directx::NativeIdentity(swapchain.Get());
    int other_output = 0;
    funnel_accounting_tests::Restore restore;
    restore.Atomic(&options::internal::present_observed);
    restore.Atomic(&options::internal::stopped);
    restore.Value(&options::internal::state.present);
    restore.Value(&options::internal::state.present1);
    options::internal::stopped.store(false);
    static bool handled = false;
    static HRESULT sdk_result = S_OK;
    static std::vector<Phase> phases;
    phases.clear();
    options::internal::present_observed.store(+[](IDXGISwapChain* output, UINT flags, Phase phase) noexcept {
      phases.push_back(phase);
      native::OnNativePresentObserved(output, flags, phase);
    });
    options::internal::state.present = +[](IDXGISwapChain*, UINT, UINT, bool& skip) {
      skip = handled;
      return sdk_result;
    };
    options::internal::state.present1 = +[](IDXGISwapChain*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*, bool& skip) {
      skip = handled;
      return sdk_result;
    };
    const auto submitted = native::native_fg_submitted_frames.load();
    for (const bool present1 : {false, true}) {
      const auto invoke = [&](UINT flags = 0u) {
        bool skip = false;
        return present1 ? options::internal::HookedPresent1(swapchain.Get(), 0, flags, nullptr, skip)
                        : options::internal::HookedPresent(swapchain.Get(), 0, flags, skip);
      };
      handled = false;
      sdk_result = S_FALSE;
      Check(invoke() == S_FALSE && phases.back() == Phase::SDK_FORWARD,
            "SDK pass-through preserves HRESULT and reports routing before COM Present");
      Check(!native::TakeNativePassthrough(&other_output, nullptr), "unrelated output cannot consume a raw ticket");
      Check(!native::TakeNativePassthrough(&other_output, identity),
            "outer proxy cannot consume a previous disabled frame's ticket before the new SDK decision");
      bool other_thread = true;
      std::thread thread([&] { other_thread = native::TakeNativePassthrough(identity, nullptr); });
      thread.join();
      Check(!other_thread && native::TakeNativePassthrough(identity, nullptr)
                && !native::TakeNativePassthrough(identity, nullptr),
            "raw ticket belongs to one output on one thread and is consumed once");
      Check(invoke() == S_FALSE && native::TakeNativePassthrough(&other_output, identity, true),
            "current deferred SDK decision can authorize the matching proxy's base output");
      handled = true;
      sdk_result = S_OK;
      Check(invoke() == S_OK && phases.back() == Phase::SDK_HANDLED
                && !native::TakeNativePassthrough(identity, nullptr), "SDK-owned output cannot enter Final NR");
      handled = false;
      sdk_result = E_FAIL;
      Check(invoke() == E_FAIL && phases.back() == Phase::SDK_FAILED
                && !native::TakeNativePassthrough(identity, nullptr), "SDK failure does not invent a raw output");
      sdk_result = S_OK;
      Check(invoke() == S_OK && native::native_passthrough_identity == identity, "raw ticket created");
      const auto before_test = phases.size();
      Check(invoke(DXGI_PRESENT_TEST) == S_OK && phases.size() == before_test,
            "DXGI test Present does not enter or complete application routing");
      handled = true;
      Check(invoke() == S_OK && !native::TakeNativePassthrough(identity, nullptr),
            "next SDK decision clears an unused previous raw ticket");
      native::native_deferred_present = {.swapchain = reinterpret_cast<reshade::api::swapchain*>(&other_output),
          .identity = identity, .route_id = 77};
      handled = false;
      Check(invoke() == S_OK && native::native_deferred_present.swapchain == nullptr
                && !native::TakeNativePassthrough(identity, nullptr), "retired output token never invokes a stale API object");
    }
    Check(native::native_fg_submitted_frames.load() == submitted,
          "SDK routing decisions are not GPU submissions or display completion");
    native::PresentStream owner, other;
    const auto* handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xF017});
    native::fg_service::native_bindings[handle] = reinterpret_cast<uintptr_t>(&owner);
    native::native_fg_owner = handle;
    native::native_fg_owns.store(true);
    const auto history = native::history_reset_epoch.load();
    native::OnNativePresentPassthrough(&other);
    Check(native::native_fg_owns.load(), "another output cannot relinquish the active FG owner");
    native::OnNativePresentPassthrough(&owner);
    Check(!native::native_fg_owns.load() && native::native_fg_owner == nullptr
              && native::history_reset_epoch.load() == history,
          "matched pass-through relinquishes only its owner without global NR history reset");
    native::fg_service::native_bindings.erase(handle);
    std::cout << "PASS native Present decisions: output/thread binding, one-time raw routing, preserved failures and no completion claims\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL native Present decisions: " << error.what() << '\n';
    return 1;
  }
}
