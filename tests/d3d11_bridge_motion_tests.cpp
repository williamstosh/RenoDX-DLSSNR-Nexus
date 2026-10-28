#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <thread>
#include <tuple>
using Microsoft::WRL::ComPtr;
int ProbeFormats() {
  ComPtr<IDXGIFactory4> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 1;
  for (bool warp : {false, true}) {
    ComPtr<IDXGIAdapter1> adapter;
    HRESULT hr = warp ? factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)) : factory->EnumAdapters1(0, &adapter);
    if (FAILED(hr)) return 2;
    DXGI_ADAPTER_DESC1 info{};
    adapter->GetDesc1(&info);
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                         D3D11_SDK_VERSION, &device, nullptr, &context);
    std::printf("%s adapter=%ls D3D11=0x%08X\n", warp ? "WARP" : "hardware", info.Description, unsigned(hr));
    if (FAILED(hr)) return 3;
    ComPtr<ID3D12Device> device12;
    hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12));
    std::printf("same-adapter D3D12=0x%08X\n", unsigned(hr));
    if (FAILED(hr)) return 4;
    for (auto format : {DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT}) {
      for (UINT bind : {UINT(D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS), UINT(D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET), UINT(D3D11_BIND_SHADER_RESOURCE)}) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = 13; desc.Height = 9; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = format; desc.SampleDesc.Count = 1; desc.BindFlags = bind;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
        ComPtr<ID3D11Texture2D> texture;
        hr = device->CreateTexture2D(&desc, nullptr, &texture);
        std::printf("format=%u bind=0x%X create=0x%08X", unsigned(format), bind, unsigned(hr));
        if (SUCCEEDED(hr)) {
          ComPtr<IDXGIResource1> resource;
          hr = texture.As(&resource);
          HANDLE handle = nullptr;
          if (SUCCEEDED(hr)) hr = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle);
          std::printf(" handle=0x%08X", unsigned(hr));
          if (SUCCEEDED(hr)) {
            ComPtr<ID3D12Resource> opened;
            hr = device12->OpenSharedHandle(handle, IID_PPV_ARGS(&opened));
            std::printf(" open12=0x%08X", unsigned(hr));
            CloseHandle(handle);
          }
        }
        std::printf("\n");
      }
    }
  }
  return 0;
}
#ifdef MOTION_FORMAT_PROBE_ONLY
int main() { return ProbeFormats(); }
#endif

#ifndef MOTION_FORMAT_PROBE_ONLY
#define DEBUG_LEVEL_0
#include <d3dcompiler.h>
#include <dlssnr.hpp>
// Reuse the established host types and checks, without executing its matrix.
#define main UnusedXeFgMotionHostMain
#include "fg_xefg_tests.cpp"
#undef main
extern "C" __declspec(dllexport) void ReShadeLogMessage(void*, int, const char* message) {
  std::cout << message << '\n';
}
extern "C" __declspec(dllexport) bool ReShadeGetBasePath(char*, size_t* size) {
  *size = 0;
  return false;
}
namespace {
namespace native = renodx::addons::dlss5::internal;
namespace params = renodx::addons::dlss5::bridge;
std::vector<ComPtr<ID3D12Resource>> model_uploads;
uint32_t model_calls = 0;
std::vector<uint32_t> model_presets;
struct MotionModel {
  uint32_t preset;
  uint64_t id;
  uint32_t evaluations = 0;
};
uint64_t model_created = 0;
struct ModelObservation {
  int reset;
  float intensity;
  uint32_t style;
  float tone;
  uint64_t id;
  uint32_t evaluation;
};
std::vector<ModelObservation> model_observations;
uint32_t fail_model_call = 0;
NVSDK_NGX_Handle* source_create_result = nullptr;
NVSDK_NGX_Result NVSDK_CONV MotionCreateSource(ID3D11DeviceContext*, NVSDK_NGX_Feature,
    NVSDK_NGX_Parameter*, NVSDK_NGX_Handle** handle) {
  *handle = source_create_result;
  return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result NVSDK_CONV MotionReleaseSource(NVSDK_NGX_Handle*) {
  return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result NVSDK_CONV MotionCreateModel(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
    const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle) {
  *handle = reinterpret_cast<NVSDK_NGX_Handle*>(new MotionModel{
      .preset = native::GetUInt(parameters, "DLSSNR.Hint.Render.Preset"), .id = ++model_created});
  return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result NVSDK_CONV MotionReleaseModel(NVSDK_NGX_Handle* handle) {
  delete reinterpret_cast<MotionModel*>(handle);
  return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result NVSDK_CONV MotionEvaluateModel(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback) {
  ++model_calls;
  auto* model = reinterpret_cast<MotionModel*>(const_cast<NVSDK_NGX_Handle*>(handle));
  model_presets.push_back(model->preset);
  model_observations.push_back({native::GetInt(parameters, "DLSSNR.Reset"),
      native::GetFloat(parameters, "DLSSNR.Intensity"), native::GetUInt(parameters, "DLSSNR.Style"),
      native::GetFloat(parameters, "DLSSNR.LocalToneStrength"), model->id, ++model->evaluations});
  if (model_calls == fail_model_call) return NVSDK_NGX_Result_Fail;
  auto* output = native::GetD3D12Resource(parameters, "DLSSNR.Output");
  Check(output != nullptr, "model output is a real D3D12 resource");
  auto* device = native::bridge.device12;
  const auto desc = output->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  uint64_t bytes = 0;
  device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  D3D12_RESOURCE_DESC buffer{.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Width = bytes,
      .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1, .SampleDesc = {1, 0},
      .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
  D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_UPLOAD};
  ComPtr<ID3D12Resource> upload;
  Check(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))), "model upload");
  void* mapped = nullptr;
  Check(SUCCEEDED(upload->Map(0, nullptr, &mapped)), "model upload map");
  std::memset(mapped, 0, static_cast<size_t>(bytes));
  Check(desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT, "model output fixture format");
  // Distinct finite model results make both stages observable even at 1:1.
  const unsigned short model_pixel = (model_presets.back() == 2 ? 0x3800 : 0x3400);
  for (UINT y = 0; y < desc.Height; ++y) {
    auto* row = reinterpret_cast<unsigned short*>(static_cast<unsigned char*>(mapped) + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch);
    for (UINT x = 0; x < desc.Width * 4; ++x) row[x] = model_pixel;
  }
  upload->Unmap(0, nullptr);
  native::Transition(list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TEXTURE_COPY_LOCATION from{.pResource = upload.Get(),
      .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = footprint};
  D3D12_TEXTURE_COPY_LOCATION to{.pResource = output, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  native::Transition(list, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  model_uploads.push_back(upload);
  return NVSDK_NGX_Result_Success;
}
void WaitForGpu(ID3D12Fence* fence, uint64_t value) {
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  Check(event != nullptr, "GPU event");
  const HRESULT hr = fence->SetEventOnCompletion(value, event);
  const DWORD wait = SUCCEEDED(hr) ? WaitForSingleObject(event, 10000) : WAIT_FAILED;
  CloseHandle(event);
  Check(wait == WAIT_OBJECT_0 && fence->GetCompletedValue() != UINT64_MAX, "bounded real GPU completion");
}
std::vector<unsigned char> Read11(ID3D11Texture2D* texture, UINT pixel_bytes) {
  auto desc = D3D11_TEXTURE2D_DESC{};
  texture->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = desc.MiscFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> stage;
  Check(SUCCEEDED(native::bridge.device11->CreateTexture2D(&desc, nullptr, &stage)), "D3D11 readback stage");
  native::bridge.context11->CopyResource(stage.Get(), texture);
  D3D11_MAPPED_SUBRESOURCE mapped{};
  Check(SUCCEEDED(native::bridge.context11->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "D3D11 readback map");
  std::vector<unsigned char> result(size_t(desc.Width) * desc.Height * pixel_bytes);
  for (UINT y = 0; y < desc.Height; ++y) {
    std::memcpy(result.data() + size_t(y) * desc.Width * pixel_bytes,
        static_cast<const unsigned char*>(mapped.pData) + size_t(y) * mapped.RowPitch, size_t(desc.Width) * pixel_bytes);
  }
  native::bridge.context11->Unmap(stage.Get(), 0);
  return result;
}
std::vector<unsigned char> Read12(ID3D12Resource* texture, UINT pixel_bytes,
    D3D12_RESOURCE_STATES rest = D3D12_RESOURCE_STATE_COMMON) {
  auto& bridge = native::bridge;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  Check(SUCCEEDED(bridge.device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))), "readback allocator");
  Check(SUCCEEDED(bridge.device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
      allocator.Get(), nullptr, IID_PPV_ARGS(&list))), "readback list");
  const auto desc = texture->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  uint64_t bytes = 0;
  bridge.device12->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  D3D12_RESOURCE_DESC buffer{.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Width = bytes,
      .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1, .SampleDesc = {1, 0},
      .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
  D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_READBACK};
  ComPtr<ID3D12Resource> readback;
  Check(SUCCEEDED(bridge.device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))), "D3D12 readback buffer");
  native::Transition(list.Get(), texture, rest, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION from{.pResource = texture, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  D3D12_TEXTURE_COPY_LOCATION to{.pResource = readback.Get(),
      .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = footprint};
  list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  native::Transition(list.Get(), texture, D3D12_RESOURCE_STATE_COPY_SOURCE, rest);
  Check(SUCCEEDED(list->Close()), "readback close");
  ID3D12CommandList* lists[]{list.Get()};
  bridge.queue->ExecuteCommandLists(1, lists);
  ComPtr<ID3D12Fence> fence;
  Check(SUCCEEDED(bridge.device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))), "readback fence");
  Check(SUCCEEDED(bridge.queue->Signal(fence.Get(), 1)), "readback GPU signal");
  WaitForGpu(fence.Get(), 1);
  void* mapped = nullptr;
  Check(SUCCEEDED(readback->Map(0, nullptr, &mapped)), "D3D12 readback map");
  std::vector<unsigned char> result(size_t(desc.Width) * desc.Height * pixel_bytes);
  for (UINT y = 0; y < desc.Height; ++y) {
    std::memcpy(result.data() + size_t(y) * desc.Width * pixel_bytes,
        static_cast<const unsigned char*>(mapped) + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch,
        size_t(desc.Width) * pixel_bytes);
  }
  readback->Unmap(0, nullptr);
  return result;
}
}
int main(int argc, char** argv) {
  try {
    bool warp = false;
    bool dlaa_only = false;
    bool sr_only = false;
    for (int arg = 1; arg < argc; ++arg) {
      if (std::strcmp(argv[arg], "--warp") == 0) {
        warp = true;
      } else if (std::strcmp(argv[arg], "--dlaa") == 0) {
        dlaa_only = true;
      } else if (std::strcmp(argv[arg], "--sr") == 0) {
        sr_only = true;
      } else {
        Check(false, "known GPU fixture argument");
      }
    }
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "factory");
    Check(SUCCEEDED(warp ? factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)) : factory->EnumAdapters1(0, &adapter)), "adapter");
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Check(SUCCEEDED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, &context)), "D3D11 device");
    native::direct_api.module = GetModuleHandleW(nullptr);
    native::direct_api.create = MotionCreateModel;
    native::direct_api.evaluate = MotionEvaluateModel;
    native::direct_api.release = MotionReleaseModel;
    native::direct_load_state = native::DirectLoadState::Ready;
    // Avoid runtime-loader priming: the only substituted component is the NR provider.
    native::hooks_enabled.store(false);
    Check(native::EnsureBridgeUp(context.Get()) == native::kBridgeReady, "production bridge bring-up");
    DXGI_ADAPTER_DESC1 adapter_desc{};
    adapter->GetDesc1(&adapter_desc);
    const auto luid12 = native::bridge.device12->GetAdapterLuid();
    Check(luid12.HighPart == adapter_desc.AdapterLuid.HighPart && luid12.LowPart == adapter_desc.AdapterLuid.LowPart,
        "D3D11 and D3D12 adapter LUID must match");
    std::printf("GPU %s adapter=%ls LUID=%08X:%08X same-adapter=PASS\n", warp ? "WARP" : "hardware",
        adapter_desc.Description, unsigned(luid12.HighPart), luid12.LowPart);
    native::direct_device = native::bridge.device12;
    native::direct_device->AddRef();
    native::direct_device_native = native::bridge.device12;
    native::parameter_runtime_ready = true;
    native::core_allocate_parameters = params::AllocateOwnedParameters;
    native::core_destroy_parameters = params::DestroyOwnedParameters;
    native::enabled.store(true);
    native::stack_passes.store(1);
    native::codec_mode.store(0);
    native::transfer_strength.store(1.f);
    native::color_strength.store(1.f);
    native::debug_bypass_host_look.store(true);
    native::detail_stability.store(1);
    native::look_mode.store(false);
    const auto make_texture = [&](UINT width, UINT height, DXGI_FORMAT format, UINT bind,
                                  const void* data, UINT pitch) {
      D3D11_TEXTURE2D_DESC desc{.Width = width, .Height = height, .MipLevels = 1, .ArraySize = 1,
          .Format = format, .SampleDesc = {1, 0}, .Usage = D3D11_USAGE_DEFAULT, .BindFlags = bind};
      D3D11_SUBRESOURCE_DATA initial{.pSysMem = data, .SysMemPitch = pitch};
      ComPtr<ID3D11Texture2D> texture;
      Check(SUCCEEDED(device->CreateTexture2D(&desc, data ? &initial : nullptr, &texture)), "input texture");
      return texture;
    };
    // Non-multiples of eight exercise dispatch edges; selected values cannot survive a float16 detour.
    constexpr UINT width = 13, height = 9;
    const float values[]{0.f, -0.f, 0.1f, -0.375f, 65537.f, -131071.25f, 1.234567f, -8388607.f};
    std::vector<float> motion_pixels(width * height * 2);
    for (size_t i = 0; i < motion_pixels.size(); ++i) motion_pixels[i] = values[i % std::size(values)];
    std::vector<unsigned short> rg16_pixels(width * height * 2, 0x3400);
    std::vector<unsigned short> color_pixels(width * height * 4, 0x3800);
    auto color = make_texture(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, color_pixels.data(), width * 8);
    std::vector<unsigned short> output_pixels(17 * 11 * 4, 0x3800);
    auto output = make_texture(17, 11, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
        output_pixels.data(), 17 * 8);
    auto motion = make_texture(width, height, DXGI_FORMAT_R32G32_FLOAT, D3D11_BIND_SHADER_RESOURCE, motion_pixels.data(), width * 8);
    auto rg16 = make_texture(width, height, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE, rg16_pixels.data(), width * 4);
    std::vector<float> depth_pixels(width * height, 0.375f);
    auto depth_direct = make_texture(width, height, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, depth_pixels.data(), width * 4);
    auto depth_planar = make_texture(width, height, DXGI_FORMAT_R32G8X24_TYPELESS, D3D11_BIND_DEPTH_STENCIL, nullptr, 0);
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc{.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
        .ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D};
    ComPtr<ID3D11DepthStencilView> dsv;
    Check(SUCCEEDED(device->CreateDepthStencilView(depth_planar.Get(), &dsv_desc, &dsv)), "depth fixture view");
    context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.375f, 0x17);
    auto sentinel = make_texture(1, 1, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, nullptr, 0);
    ComPtr<ID3D11ShaderResourceView> saved_srv;
    Check(SUCCEEDED(device->CreateShaderResourceView(sentinel.Get(), nullptr, &saved_srv)), "sentinel SRV");
    D3D11_BUFFER_DESC counter_desc{.ByteWidth = 16, .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_UNORDERED_ACCESS, .MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
        .StructureByteStride = 4};
    ComPtr<ID3D11Buffer> counter_buffer;
    Check(SUCCEEDED(device->CreateBuffer(&counter_desc, nullptr, &counter_buffer)), "sentinel counter buffer");
    D3D11_UNORDERED_ACCESS_VIEW_DESC counter_view{.Format = DXGI_FORMAT_UNKNOWN,
        .ViewDimension = D3D11_UAV_DIMENSION_BUFFER, .Buffer = {0, 4, D3D11_BUFFER_UAV_FLAG_COUNTER}};
    ComPtr<ID3D11UnorderedAccessView> saved_uav;
    Check(SUCCEEDED(device->CreateUnorderedAccessView(counter_buffer.Get(), &counter_view, &saved_uav)), "sentinel counter UAV");
    const char sentinel_source[] =
        "interface IOperation { float Apply(float x); };"
        "class MotionSentinel : IOperation { float Apply(float x) { return x * 2; } };"
        "MotionSentinel sentinel; IOperation operation; RWStructuredBuffer<float> result : register(u0);"
        "[numthreads(1,1,1)] void main(uint3 id:SV_DispatchThreadID) { result[0]=operation.Apply(1); }";
    ComPtr<ID3DBlob> bytecode, errors;
    Check(SUCCEEDED(D3DCompile(sentinel_source, sizeof(sentinel_source) - 1, nullptr, nullptr, nullptr,
        "main", "cs_5_0", D3DCOMPILE_SKIP_OPTIMIZATION, 0, &bytecode, &errors)), "dynamic-linkage sentinel compile");
    ComPtr<ID3D11ClassLinkage> linkage;
    ComPtr<ID3D11ClassInstance> saved_instance;
    ComPtr<ID3D11ComputeShader> saved_shader;
    Check(SUCCEEDED(device->CreateClassLinkage(&linkage)), "sentinel linkage");
    Check(SUCCEEDED(device->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), linkage.Get(), &saved_shader)), "sentinel shader");
    Check(SUCCEEDED(linkage->CreateClassInstance("MotionSentinel", 0, 0, 0, 0, &saved_instance)), "sentinel class instance");
    for (bool pre : {false, true}) {
      if (dlaa_only) break;
      for (int variant = 0; variant < 3; ++variant) {
        const bool convert = variant != 2;
        const bool depth_convert = variant == 1;
        native::hook_point.store(pre ? native::kHookRender : native::kHookUpscaled);
        context->UpdateSubresource(color.Get(), 0, nullptr, color_pixels.data(), width * 8, 0);
        context->UpdateSubresource(output.Get(), 0, nullptr, output_pixels.data(), 17 * 8, 0);
        params::OwnedParameters parameters;
        parameters.Set(NVSDK_NGX_Parameter_Width, width);
        parameters.Set(NVSDK_NGX_Parameter_Height, height);
        parameters.Set(NVSDK_NGX_Parameter_OutWidth, 17u);
        parameters.Set(NVSDK_NGX_Parameter_OutHeight, 11u);
        parameters.Set(NVSDK_NGX_Parameter_Color, static_cast<ID3D11Resource*>(color.Get()));
        parameters.Set(NVSDK_NGX_Parameter_Output, static_cast<ID3D11Resource*>(output.Get()));
        parameters.Set(NVSDK_NGX_Parameter_MotionVectors, static_cast<ID3D11Resource*>(convert ? motion.Get() : rg16.Get()));
        parameters.Set(NVSDK_NGX_Parameter_Depth, static_cast<ID3D11Resource*>(depth_convert ? depth_planar.Get() : depth_direct.Get()));
        parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
        parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
        parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, int(NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes));
        const auto* handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t(0xD110 + variant + (pre ? 16 : 0)));
        const params::ParameterView create_view(&parameters, nullptr);
        native::RegisterCreatedFeatureLocked(handle, native::kFeatureDlss, &create_view);
        auto* srv = saved_srv.Get();
        auto* uav = saved_uav.Get();
        auto* instance = saved_instance.Get();
        context->CSSetShader(saved_shader.Get(), &instance, 1);
        context->CSSetShaderResources(0, 1, &srv);
        const UINT initial_counter = 7;
        context->CSSetUnorderedAccessViews(0, 1, &uav, &initial_counter);
        ComPtr<ID3D11ComputeShader> baseline_shader;
        ComPtr<ID3D11ClassInstance> baseline_instance;
        UINT baseline_count = 1;
        context->CSGetShader(&baseline_shader, baseline_instance.GetAddressOf(), &baseline_count);
        Check(baseline_shader == saved_shader && baseline_count == 1 && baseline_instance == saved_instance,
            "sentinel shader/class instance actually bound before RunBridge");
        const auto calls_before = model_calls;
        bool delivered = false;
        for (int attempt = 0; attempt < 4 && (!delivered || model_calls == calls_before); ++attempt) {
          ++native::present_generation;
          if (pre) {
            params::ScopedD3D11Redirect redirect;
            delivered = native::RunBridge(context.Get(), handle, &parameters, &redirect);
            if (delivered) Check(params::GetD3D11Resource(&parameters, NVSDK_NGX_Parameter_Color) == native::bridge_sets.at(handle)[1].color11,
                "Render redirects shared Color");
          } else {
            delivered = native::RunBridge(context.Get(), handle, &parameters, nullptr);
          }
          if (native::bridge_submitted_value.load()) WaitForGpu(native::bridge.fence_done12, native::bridge_submitted_value.load());
        }
        if (!delivered || model_calls == calls_before) {
          for (size_t i = 0; i < std::size(renodx::addons::dlss5::nr_decline_counts); ++i) {
            if (const auto count = renodx::addons::dlss5::nr_decline_counts[i].load(); count != 0) {
              std::cerr << renodx::addons::dlss5::kNrDeclineTags[i] << "=" << count << '\n';
            }
          }
        }
        Check(delivered && model_calls > calls_before, "real RunBridge delivers model GPU output");
        auto& set = native::bridge_sets.at(handle)[pre ? 1 : 0];
        Check(set.usable && set.motion_convert == convert && set.depth_convert == depth_convert, "selected conversion paths");
        Check(pre ? (set.color11 && !set.output11) : (!set.color11 && set.output11), "Render/Upscaled twin selection");
        Check(set.color12->GetDesc().Width == width && set.output12->GetDesc().Width == 17, "distinct input/output geometry");
        Check((set.motion_stage != nullptr) == convert && (set.motion_srv != nullptr) == convert && (set.motion_uav != nullptr) == convert,
            "fallback resources only on RG32");
        Check(set.motion12->GetDesc().Format == (convert ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16G16_FLOAT), "shared format");
        ComPtr<ID3D11ComputeShader> observed_shader;
        ComPtr<ID3D11ShaderResourceView> observed_srv;
        ComPtr<ID3D11UnorderedAccessView> observed_uav;
        UINT instances = 1;
        ComPtr<ID3D11ClassInstance> observed_instance;
        context->CSGetShader(&observed_shader, observed_instance.GetAddressOf(), &instances);
        context->CSGetShaderResources(0, 1, &observed_srv);
        context->CSGetUnorderedAccessViews(0, 1, &observed_uav);

        Check(observed_shader == saved_shader && observed_srv == saved_srv && observed_uav == saved_uav && instances == 1 && observed_instance == saved_instance, "D3D11 CS state restored");
        D3D11_BUFFER_DESC count_desc{.ByteWidth = 4, .Usage = D3D11_USAGE_DEFAULT};
        ComPtr<ID3D11Buffer> count_copy, count_stage;
        Check(SUCCEEDED(device->CreateBuffer(&count_desc, nullptr, &count_copy)), "counter copy target");
        count_desc.Usage = D3D11_USAGE_STAGING;
        count_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Check(SUCCEEDED(device->CreateBuffer(&count_desc, nullptr, &count_stage)), "counter readback");
        context->CopyStructureCount(count_copy.Get(), 0, saved_uav.Get());
        context->CopyResource(count_stage.Get(), count_copy.Get());
        D3D11_MAPPED_SUBRESOURCE count_mapped{};
        Check(SUCCEEDED(context->Map(count_stage.Get(), 0, D3D11_MAP_READ, 0, &count_mapped)), "counter map");
        const UINT observed_count = *static_cast<const UINT*>(count_mapped.pData);
        context->Unmap(count_stage.Get(), 0);
        Check(observed_count == initial_counter, "CS UAV hidden counter preserved");
        const auto delivered_pixels = Read11(pre ? set.color11 : output.Get(), 8);
        const UINT delivered_width = pre ? width : 17;
        const UINT delivered_height = pre ? height : 11;
        bool changed = false;
        for (size_t i = 0; i < size_t(delivered_width) * delivered_height; ++i) {
          unsigned short pixel[4]{};
          std::memcpy(pixel, delivered_pixels.data() + i * 8, sizeof(pixel));
          for (size_t channel = 0; channel < 3; ++channel) {
            Check((pixel[channel] & 0x7c00) != 0x7c00, "delivered RGB is finite");
            changed |= pixel[channel] != 0x3800;
          }
        }
        Check(changed, "model GPU output changes the delivered image");
        if (pre) {
          const auto game_output = Read11(output.Get(), 8);
          Check(std::memcmp(game_output.data(), output_pixels.data(), game_output.size()) == 0,
              "Render leaves game Output content untouched");
        } else {
          Check(Read11(set.output11, 8) == delivered_pixels, "Upscaled shared twin copied exactly to game Output");
        }
        const auto game_color = Read11(color.Get(), 8);
        Check(std::memcmp(game_color.data(), color_pixels.data(), game_color.size()) == 0, "game Color content untouched");
        const auto actual = Read12(set.motion12, convert ? 16 : 4);
        if (convert) {
          for (size_t i = 0; i < width * height; ++i) {
            Check(std::memcmp(actual.data() + i * 16, motion_pixels.data() + i * 2, 8) == 0,
                "float32 xy bit exact including edge texels");
          }
        } else {
          Check(actual.size() == rg16_pixels.size() * 2 && std::memcmp(actual.data(), rg16_pixels.data(), actual.size()) == 0, "RG16 direct copy pixels");
        }
        const auto depth_actual = Read12(set.depth12, 4);
        Check(depth_actual.size() == depth_pixels.size() * 4 && std::memcmp(depth_actual.data(), depth_pixels.data(), depth_actual.size()) == 0,
            "depth copied on motion-only and dual-convert paths");
        const auto original_motion = Read11(motion.Get(), 8);
        Check(std::memcmp(original_motion.data(), motion_pixels.data(), original_motion.size()) == 0, "original RG32 motion untouched");
        const auto original_rg16 = Read11(rg16.Get(), 4);
        Check(std::memcmp(original_rg16.data(), rg16_pixels.data(), original_rg16.size()) == 0, "original RG16 motion untouched");
        Check(params::GetD3D11Resource(&parameters, NVSDK_NGX_Parameter_Color) == color.Get(), "Render Color binding restored");
        // Hold one observer reference: after retirement only that reference may remain.
        ComPtr<ID3D11Texture2D> stage = set.motion_stage;
        ComPtr<ID3D11ShaderResourceView> motion_srv = set.motion_srv;
        ComPtr<ID3D11UnorderedAccessView> motion_uav = set.motion_uav;
        const auto last_value = set.last_value;
        const auto retired_before = native::bridge_retired.size();
        native::RetireBridgeSetLocked(handle);
        Check(!native::bridge_sets.contains(handle) && native::bridge_retired.size() == retired_before + 1, "set retired");
        native::DrainBridgeRetired(last_value - 1);
        Check(native::bridge_retired.size() == retired_before + 1, "D3D12 resources retained below completion value");
        native::DrainBridgeRetired(last_value);
        Check(native::bridge_retired.size() == retired_before, "D3D12 resources drained at completion value");
        context->ClearState();
        context->Flush();
        if (convert) {
          Check(motion_srv.Reset() == 0 && motion_uav.Reset() == 0 && stage.Reset() == 0,
              "motion SRV/UAV/stage released by RetireBridgeSet");
        }
        model_uploads.clear();
        std::printf("PASS mode=%s motion=%s depth=%s 13x9 float32/shared/readback/state/retire\n",
            pre ? "Render" : "Upscaled", convert ? "RG32 fallback" : "RG16 direct", depth_convert ? "converted" : "direct");
      }
    }
    for (uint32_t fixture = (dlaa_only ? 1u : 0u); fixture < (sr_only ? 1u : 3u); ++fixture) {
      const UINT output_width = (fixture == 0 ? 17u : width);
      const UINT output_height = (fixture == 0 ? 11u : height);
      const bool aliased = fixture == 2;
      const auto game_color = make_texture(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT,
          D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, color_pixels.data(), width * 8);
      const std::vector<unsigned short> game_output_pixels(output_width * output_height * 4, 0x3800);
      ComPtr<ID3D11Texture2D> game_output;
      if (aliased) {
        game_output = game_color;
      } else {
        game_output = make_texture(output_width, output_height, DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, game_output_pixels.data(), output_width * 8);
      }
      std::printf("CASE %s %ux%u->%ux%u aliased=%u\n", fixture == 0 ? "SR" : "DLAA",
          width, height, output_width, output_height, unsigned(aliased));
      const auto run_sr = [&](ID3D11Resource* input) {
        // Without a redirect an aliased identity SR needs no overlapping self-copy.
        if (input != game_output.Get()) {
          const D3D11_BOX region{0, 0, 0, width, height, 1};
          context->CopySubresourceRegion(game_output.Get(), 0, 0, 0, 0, input, 0, &region);
        }
        if (fixture != 0) {
          // Distinguish actual SR output from its Render input at equal dimensions.
          const unsigned short marker[]{0x3a00, 0x3800, 0x3400, 0x3c00};
          const D3D11_BOX pixel{width - 1, height - 1, 0, width, height, 1};
          context->UpdateSubresource(game_output.Get(), 0, &pixel, marker, sizeof(marker), 0);
        }
      };
      // Exercise the real D3D11 detour and both shared sets. Only SR/NR are stand-ins.
      native::d3d11_swapchain_seen.store(true);
      native::dx11_route.store(native::kDx11RouteNative);
      native::direct_runtime_requirements.dual_models = true;
      native::dedupe_mode.store(renodx::addons::dlss5::kDedupePerEvaluate);
      native::stack_passes.store(2);
      native::hook_point.store(native::kHookRender);
      native::preset.store(1);
      native::intensity.store(1.f);
      native::pass_preset[0].store(2);
      native::pass_stage[0].store(2);
      native::pass_transfer_strength[0].store(0.5f);
      native::pass_color_strength[0].store(0.5f);
      native::pass_look_result[0].follow.store(true);
      native::pass_follow[0].store(true);
      // Publish the fixture loader identity so the detour never loads a vendor DLL.
      native::ngx_loader_staging.nr_ready = true;
      native::ngx_loader_staging.core_ready = true;
      native::PublishNgxLoaderSymbols();
      params::OwnedParameters mixed_parameters;
      mixed_parameters.Set(NVSDK_NGX_Parameter_Width, width);
      mixed_parameters.Set(NVSDK_NGX_Parameter_Height, height);
      mixed_parameters.Set(NVSDK_NGX_Parameter_OutWidth, output_width);
      mixed_parameters.Set(NVSDK_NGX_Parameter_OutHeight, output_height);
      mixed_parameters.Set(NVSDK_NGX_Parameter_Color, static_cast<ID3D11Resource*>(game_color.Get()));
      mixed_parameters.Set(NVSDK_NGX_Parameter_Output, static_cast<ID3D11Resource*>(game_output.Get()));
      mixed_parameters.Set(NVSDK_NGX_Parameter_MotionVectors, static_cast<ID3D11Resource*>(rg16.Get()));
      mixed_parameters.Set(NVSDK_NGX_Parameter_Depth, static_cast<ID3D11Resource*>(depth_direct.Get()));
      mixed_parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
      mixed_parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
      mixed_parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
          int(NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes));
      const auto* mixed_handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t(0xD120 + fixture * 2));
      const params::ParameterView mixed_create_view(&mixed_parameters, nullptr);
      native::RegisterCreatedFeatureLocked(mixed_handle, native::kFeatureDlss, &mixed_create_view);
      bool mixed_delivered = false;
      uint64_t resident_builds = 0;
      bool stable_residency = false;
      for (int frame = 0; frame < 12; ++frame) {
        ++native::present_generation;
        context->UpdateSubresource(game_color.Get(), 0, nullptr, color_pixels.data(), width * 8, 0);
        if (!aliased) {
          context->UpdateSubresource(game_output.Get(), 0, nullptr, game_output_pixels.data(), output_width * 8, 0);
        }
        const auto calls_before = model_calls;
        bool redirected = false;
        std::vector<unsigned char> sr_input, sr_output;
        const auto result = native::Ngx11Evaluate(0, "mixed fixture", context.Get(), mixed_handle, &mixed_parameters, [&] {
          auto* input = params::GetD3D11Resource(&mixed_parameters, NVSDK_NGX_Parameter_Color);
          redirected = input != game_color.Get();
          ComPtr<ID3D11Texture2D> input_texture;
          Check(SUCCEEDED(input->QueryInterface(IID_PPV_ARGS(&input_texture))), "SR receives D3D11 Color texture");
          sr_input = Read11(input_texture.Get(), 8);
          run_sr(input);
          sr_output = Read11(game_output.Get(), 8);
          return NVSDK_NGX_Result_Success;
        });
        Check(NVSDK_NGX_SUCCEED(result), "mixed detour preserves successful SR result");
        Check(params::GetD3D11Resource(&mixed_parameters, NVSDK_NGX_Parameter_Color) == game_color.Get()
                  && params::GetD3D11Resource(&mixed_parameters, NVSDK_NGX_Parameter_Output) == game_output.Get(),
            "mixed detour restores the original Color/Output bindings");
        if (native::bridge_submitted_value.load()) {
          WaitForGpu(native::bridge.fence_done12, native::bridge_submitted_value.load());
        }
        if (!redirected || model_calls - calls_before != 2) continue;
        Check(model_presets[calls_before] == 1 && model_presets[calls_before + 1] == 2,
            "actual model requests execute A before SR and B after SR");
        auto& sets = native::bridge_sets.at(mixed_handle);
        Check(sets[1].usable && sets[0].usable && sets[1].color11 && sets[0].output11,
            "Render and Upscaled shared sets remain resident together");
        Check(sets[1].last_value < sets[0].last_value, "Render submits before Upscaled");
        Check(sr_input == Read11(sets[1].color11, 8), "SR reads exact Render shared twin pixels");
        Check(std::memcmp(sr_input.data(), color_pixels.data(), sr_input.size()) != 0,
            "Render result actually changes SR input");
        for (UINT y = 0; y < height; ++y) {
          const UINT copied_width = width - unsigned(fixture != 0 && y == height - 1);
          Check(std::memcmp(sr_input.data() + size_t(y) * width * 8,
              sr_output.data() + size_t(y) * output_width * 8, copied_width * 8) == 0,
              "SR output contains exact Render input pixels");
        }
        Check(fixture == 0 || sr_output != sr_input, "DLAA SR output differs from its Render input");
        const auto final_pixels = Read11(game_output.Get(), 8);
        Check(final_pixels == Read11(sets[0].output11, 8), "Upscaled result handed back exactly to game Output");
        Check(final_pixels != sr_output, "Upscaled B processes SR output after Render A");
        if (aliased) {
          Check(Read11(game_color.Get(), 8) == final_pixels, "aliased Color receives the final Output writeback");
        } else {
          Check(std::memcmp(Read11(game_color.Get(), 8).data(), color_pixels.data(), color_pixels.size() * 2) == 0,
              "distinct game Color remains untouched");
        }
        auto& render_work = native::worksets.at({mixed_handle, sets[1].color12, true});
        auto& upscaled_work = native::worksets.at({mixed_handle, sets[0].output12, false});
        Check(render_work.work0 != upscaled_work.work0 && render_work.descriptors != upscaled_work.descriptors,
            "equal dimensions and game aliases retain separate stage worksets");
        // In HDR original aliases the decoded result; work0 retains the source.
        // The fixture uses linear FP16, so linearization preserves its bytes.
        Check(Read12(render_work.work0, 8, render_work.work0_state)
                  == std::vector<unsigned char>(reinterpret_cast<const unsigned char*>(color_pixels.data()),
                      reinterpret_cast<const unsigned char*>(color_pixels.data()) + color_pixels.size() * 2),
            "Render snapshots the original game Color before SR can overwrite it");
        Check(Read12(upscaled_work.work0, 8, upscaled_work.work0_state) == sr_output,
            "Upscaled snapshots the actual SR output, not the old Color or Render input");
        const auto& feature = native::features.at(mixed_handle);
        Check(feature.render_session && feature.render_session->slots[0].handle
                  && feature.slots[0].handle && feature.render_session->slots[0].handle != feature.slots[0].handle,
            "Render and Upscaled retain distinct model/history instances");
        if (mixed_delivered) {
          Check(native::bridge_sets_built == resident_builds, "steady mixed frame does not rebuild either set");
          stable_residency = true;
          break;
        }
        mixed_delivered = true;
        resident_builds = native::bridge_sets_built;
      }
      Check(mixed_delivered && stable_residency, "AB reaches two complete frames with stable resident sets");
      {
        // Rebuild scratch while retaining the resident Render twin. Control the
        // tracker receipt separately from GPU timing so both retention gates are
        // deterministic; these extra lists record no GPU commands.
        const auto* owner = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t(0xD121 + fixture * 2));
        native::RegisterCreatedFeatureLocked(owner, native::kFeatureDlss, &mixed_create_view);
        auto* twin = native::bridge_sets.at(mixed_handle)[1].color12;
        const native::WorksetKey key{owner, twin, true};
        ComPtr<ID3D12CommandAllocator> allocator, new_allocator;
        ComPtr<ID3D12GraphicsCommandList> old_list, new_list;
        ComPtr<ID3D12Fence> receipt;
        auto* device12 = native::bridge.device12;
        Check(SUCCEEDED(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&allocator))), "retirement regression allocator");
        Check(SUCCEEDED(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&new_allocator))), "retirement new recording allocator");
        Check(SUCCEEDED(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            allocator.Get(), nullptr, IID_PPV_ARGS(&old_list))), "retirement old recording");
        Check(SUCCEEDED(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            new_allocator.Get(), nullptr, IID_PPV_ARGS(&new_list))), "retirement new recording");
        Check(SUCCEEDED(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(&receipt))), "retirement controlled submission receipt");
        auto* old = native::EnsureWorkset(device12, owner, twin, width, height, width, height,
            DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 1, 0, true);
        Check(old != nullptr, "retirement old Render workset");
        twin->AddRef();
        old->pre_sr_source = twin;
        const auto old_id = old->id;
        const auto* old_scratch = old->original;
        const auto* old_heap = old->descriptors;
        const auto* shared_norm = old->norm_commit;
        native::TrackWorksetRecording(old_list.Get(), *old);
        Check(SUCCEEDED(old_list->Close()), "retirement close old recording");
        ID3D12CommandList* lists[] = {old_list.Get()};
        const auto pending = submission::BeginSubmission(1, lists);
        Check(pending.size() == 1, "old recording has an exact submission reservation");
        submission::CompleteSubmission(pending, receipt.Get(), 1);
        auto* current = native::EnsureWorkset(device12, owner, twin, width, height, width, height,
            DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 2, 0, true);
        Check(current != nullptr && current->id != old_id, "stack change rebuilds Render scratch");
        twin->AddRef();
        current->pre_sr_source = twin;
        Check(current->original != old_scratch && current->descriptors != old_heap
                  && current->norm_commit == shared_norm,
            "rebuilt workset has independent scratch/heap and shares stream Norm");
        native::TrackWorksetRecording(new_list.Get(), *current);
        Check(SUCCEEDED(new_list->Close()), "retirement close new recording");
        const auto retained = [&] {
          for (const auto& entry : native::retired_final_resources) {
            if (entry.resources.id == old_id) {
              Check(entry.tracker_proof, "regression uses exact tracker retirement");
              return true;
            }
          }
          return false;
        };
        native::DrainRetiredResources(true);
        Check(retained(), "old pending submission retains its scratch");
        Check(SUCCEEDED(receipt->Signal(1)), "complete controlled old submission receipt");
        native::DrainRetiredResources(true);
        Check(retained(), "completed but replayable old recording retains its scratch");
        const auto replay = submission::BeginSubmission(1, lists);
        Check(replay.size() == 1, "old replay has an exact submission reservation");
        submission::CompleteSubmission(replay, receipt.Get(), 2);
        Check(SUCCEEDED(old_list->Reset(allocator.Get(), nullptr)), "discard old recording");
        submission::OnCommandListReset(old_list.Get());
        Check(SUCCEEDED(old_list->Close()), "close discarded old recording");
        native::DrainRetiredResources(true);
        Check(retained(), "discarded old recording with pending replay retains its scratch");
        Check(SUCCEEDED(receipt->Signal(2)), "complete controlled old replay receipt");
        native::DrainRetiredResources(true);
        Check(!submission::ResourceReleasable(current->original),
            "new workset recording remains live during old retirement");
        Check(submission::ResourceReleasable(old_scratch)
                  && submission::ResourceReleasable(old_heap),
            "old workset exclusive objects have no remaining recorded uses");
        Check(!retained(), "new Render workset sharing Color twin must not pin retired scratch");
        submission::OnCommandListDestroyed(old_list.Get());
        submission::OnCommandListDestroyed(new_list.Get());
        native::RetireWorkset(key);
        native::DrainRetiredResources(true);
        native::ForgetSourceHandleLocked(owner);
        std::printf("PASS retirement rebuilt Render/shared Color/Norm/pending submission/replayable recording\n");
      }
      {
        // Neither binding's lifetime depends on the map or on Set retaining COM
        // objects. A bridge retirement during the SR handoff keeps Color live.
        ComPtr<ID3D11Texture2D> replacement = native::bridge_sets.at(mixed_handle)[1].color11;
        params::ScopedD3D11Redirect redirect;
        redirect.Point(&mixed_parameters, NVSDK_NGX_Parameter_Color, replacement.Get());
        native::RetireBridgeSetLocked(mixed_handle);
        Check(Read11(replacement.Get(), 8).size() == width * height * 8,
            "redirect owns its shared Color through bridge retirement");
        redirect.Restore();
        Check(params::GetD3D11Resource(&mixed_parameters, NVSDK_NGX_Parameter_Color) == game_color.Get(),
            "explicit redirect restoration remains idempotent");
      }
      std::printf("PASS mixed AB %ux%u->%ux%u aliased=%u handoff/readback/feature/history/residency\n",
          width, height, output_width, output_height, unsigned(aliased));
      struct FrameResult {
        NVSDK_NGX_Result result;
        uint32_t calls_before;
        uint32_t calls_at_sr = 0;
        bool redirected = false;
        std::vector<unsigned char> sr_pixels;
      };
      struct SrAborted {
        std::vector<unsigned char> pixels;
      };
      const auto frame = [&](bool fail_sr = false, bool change_generation = false,
                             bool nested = false, bool drain = true, bool inspect = true,
                             bool abort_sr = false) {
        ++native::present_generation;
        context->UpdateSubresource(game_color.Get(), 0, nullptr, color_pixels.data(), width * 8, 0);
        if (!aliased) {
          context->UpdateSubresource(game_output.Get(), 0, nullptr, game_output_pixels.data(), output_width * 8, 0);
        }
        FrameResult observed{.calls_before = model_calls};
        const auto sr = [&] {
          observed.calls_at_sr = model_calls;
          auto* input = params::GetD3D11Resource(&mixed_parameters, NVSDK_NGX_Parameter_Color);
          observed.redirected = input != game_color.Get();
          run_sr(input);
          if (inspect) {
            observed.sr_pixels = Read11(game_output.Get(), 8);
          }
          if (change_generation) {
            ++native::configuration_generation;
          }
          if (abort_sr) {
            Check(observed.redirected, "exception occurs during the Render handoff");
            throw SrAborted{observed.sr_pixels};
          }
          return fail_sr ? NVSDK_NGX_Result_Fail : NVSDK_NGX_Result_Success;
        };
        observed.result = native::Ngx11Evaluate(0, "stage fixture", context.Get(), mixed_handle, &mixed_parameters, [&] {
          if (nested) {
            return native::Ngx11Evaluate(0, "nested C/C++ fixture", context.Get(), mixed_handle, &mixed_parameters, sr);
          }
          return sr();
        });
        Check(params::GetD3D11Resource(&mixed_parameters, NVSDK_NGX_Parameter_Color) == game_color.Get()
                  && params::GetD3D11Resource(&mixed_parameters, NVSDK_NGX_Parameter_Output) == game_output.Get(),
            "all detour exits restore Color/Output bindings");
        if (drain) {
          WaitForGpu(native::bridge.fence_done12, native::bridge_submitted_value.load());
          native::DrainRetiredResources(true);
          renodx::addons::dlss5::gpu_timers::Poll(native::queue_tracking_active.load(), false);
        }
        return observed;
      };
      const auto configure = [&](uint32_t count, uint32_t render_count, bool reverse) {
        native::stack_passes.store(count);
        native::hook_point.store(render_count != 0 ? native::kHookRender : native::kHookUpscaled);
        native::preset.store(reverse ? 2u : 1u);
        native::intensity.store(0.5f);
        native::style.store(0);
        native::local_tone_strength.store(0.25f);
        for (uint32_t pass = 1; pass < 4; ++pass) {
          native::pass_stage[pass - 1].store(pass < render_count ? 1u : 2u);
          native::pass_preset[pass - 1].store((pass + uint32_t(reverse)) % 2 + 1);
          native::pass_follow[pass - 1].store(false);
          native::pass_intensity[pass - 1].store(0.5f + 0.125f * pass);
          native::pass_style[pass - 1].store(pass % 3);
          native::pass_local_tone[pass - 1].store(0.25f + 0.125f * pass);
          native::pass_transfer_strength[pass - 1].store(0.5f);
          native::pass_color_strength[pass - 1].store(0.5f);
          native::pass_look_result[pass - 1].follow.store(false);
          native::pass_look_result[pass - 1].mode.store(true);
          native::pass_look_result[pass - 1].strength.store(0.5f + 0.125f * pass);
        }
        native::RecreateFeatures();
      };
      const auto mature = [&](uint32_t count, uint32_t render_count) {
        for (uint32_t attempt = 0; attempt < 16; ++attempt) {
          const auto observed = frame();
          Check(model_calls - observed.calls_before <= count, "no duplicate pass during maturation");
          if (model_calls - observed.calls_before == count
              && observed.calls_at_sr - observed.calls_before == render_count
              && (render_count == 0 || observed.redirected)) return;
        }
        Check(false, "stage configuration matures to a complete chain");
      };
      // Every legal Render/Upscaled split, plus AB/BA and 1->2->3->1 hot changes.
      const uint32_t configurations[][3] = {
          {1, 0, 0}, {2, 1, 0}, {2, 1, 1}, {3, 1, 0}, {3, 2, 1},
          {4, 1, 0}, {4, 2, 1}, {4, 3, 0}, {1, 1, 0}, {1, 0, 0}};
      for (const auto& config : configurations) {
        if (fixture != 0 && !(config[0] == 2 && config[1] == 1)) continue;
        const auto [count, render_count, reverse] = std::tuple{config[0], config[1], config[2]};
        configure(count, render_count, reverse != 0);
        mature(count, render_count);
        const auto built_before = native::bridge_sets_built;
        const auto observed = frame(false, false, true);
        Check(model_calls - observed.calls_before == count, "exact pass count across nested exports");
        Check(observed.calls_at_sr - observed.calls_before == render_count, "passes execute on the assigned side of SR");
        Check(native::bridge_sets_built == built_before, "hot model/stage changes retain compatible twins");
        for (uint32_t pass = 0; pass < count; ++pass) {
          const auto index = observed.calls_before + pass;
          Check(model_presets[index] == (pass + reverse) % 2 + 1, "logical model assignment survives local stage slots");
          const auto& model = model_observations[index];
          Check(model.intensity == 0.5f + 0.125f * pass && model.style == pass % 3
                    && model.tone == 0.25f + 0.125f * pass, "per-pass steering keeps logical pass identity");
          Check(model.reset == 0, "steady temporal history is not reset every frame");
        }
        mixed_parameters.Set(NVSDK_NGX_Parameter_Reset, 1);
        const auto reset_frame = frame();
        mixed_parameters.Set(NVSDK_NGX_Parameter_Reset, 0);
        for (uint32_t pass = 0; pass < count; ++pass) {
          Check(model_observations[reset_frame.calls_before + pass].reset == 1, "game Reset reaches every group");
        }
        std::printf("PASS stages passes=%u Render=%u Upscaled=%u reverse=%u\n", count, render_count, count - render_count, reverse);
      }
      for (const uint32_t render_count : {1u, 2u}) {
        configure(render_count + 1, render_count, false);
        mature(render_count + 1, render_count);
        fail_model_call = model_calls + render_count;
        const auto failed = frame();
        fail_model_call = 0;
        Check(NVSDK_NGX_SUCCEED(failed.result) && !failed.redirected, "failed/partial Render keeps native SR input");
        Check(model_calls - failed.calls_before == render_count, "failed/partial Render never executes Upscaled");
        Check(Read11(game_output.Get(), 8) == failed.sr_pixels, "Render failure preserves native SR result");
        Check(native::features.at(mixed_handle).slots[0].pending_reset, "failed predecessor resets following history");
      }
      configure(2, 1, false);
      mature(2, 1);
      const auto failed_sr = frame(true);
      Check(NVSDK_NGX_FAILED(failed_sr.result) && model_calls - failed_sr.calls_before == 1,
          "failed native SR never executes Upscaled");
      Check(Read11(game_output.Get(), 8) == failed_sr.sr_pixels, "failed SR output is not overwritten by NR");
      Check(native::features.at(mixed_handle).slots[0].pending_reset, "SR failure resets following history");
      const auto changed = frame(false, true);
      Check(model_calls - changed.calls_before == 1 && Read11(game_output.Get(), 8) == changed.sr_pixels,
          "configuration generation change blocks the dependent group");
      configure(2, 1, false);
      mature(2, 1);
      const auto calls_before_abort = model_calls;
      std::vector<unsigned char> aborted_pixels;
      try {
        frame(false, false, false, true, true, true);
      } catch (const SrAborted& aborted) {
        aborted_pixels = aborted.pixels;
      }
      Check(!aborted_pixels.empty() && model_calls - calls_before_abort == 1,
          "SR exception unwinds without executing the dependent group");
      Check(params::GetD3D11Resource(&mixed_parameters, NVSDK_NGX_Parameter_Color) == game_color.Get()
                && params::GetD3D11Resource(&mixed_parameters, NVSDK_NGX_Parameter_Output) == game_output.Get(),
          "SR exception restores Color and preserves the original Output binding, including aliases");
      WaitForGpu(native::bridge.fence_done12, native::bridge_submitted_value.load());
      Check(Read11(game_output.Get(), 8) == aborted_pixels,
          "exception exit leaves the SR write intact");
      configure(2, 1, false);
      mature(2, 1);
      auto& busy_slot = native::bridge.ring[(native::bridge_submitted_value.load() + 1) % native::kBridgeRing];
      const auto old_value = busy_slot.value;
      busy_slot.value = native::bridge_submitted_value.load() + native::kBridgeRing;
      const auto busy = frame();
      busy_slot.value = old_value;
      Check(model_calls == busy.calls_before && !busy.redirected && Read11(game_output.Get(), 8) == busy.sr_pixels,
          "ring without completion proof preserves SR and skips both groups");
      // Out may be rescued while done is blocked by a test-owned GPU fence.
      // The bounded event wait must fail safely, and even a mode change must
      // not overwrite the still-pending twins. No production busy wait is used.
      ComPtr<ID3D12Fence> stalled;
      Check(SUCCEEDED(native::bridge.device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&stalled))), "stall gate");
      Check(SUCCEEDED(native::bridge.queue->Wait(stalled.Get(), 1)), "stall private queue");
      std::jthread release_stall([stalled] { Sleep(1600); stalled->Signal(1); });
      const auto signals_before_stall = native::bridge_signals.load();
      const auto timed_out = frame(false, false, false, false);
      const auto pending = native::bridge_submitted_value.load();
      Check(!timed_out.redirected && model_calls - timed_out.calls_before == 1,
          "watchdog rescue is not a successful Render delivery");
      Check(native::bridge.fence_done12->GetCompletedValue() < pending
                && native::bridge_pending_delivery_value == pending, "real completion remains unproven after rescue");
      Check(Read11(game_output.Get(), 8) == timed_out.sr_pixels, "timeout preserves native SR result");
      const auto blocked = frame(false, false, false, false);
      native::hook_point.store(native::kHookUpscaled);
      native::stack_passes.store(1);
      const auto blocked_single = frame(false, false, false, false);
      Check(model_calls == blocked.calls_before && model_calls == blocked_single.calls_before
                && native::bridge_signals.load() == signals_before_stall + 1,
          "unproven mixed delivery blocks twin writes across a switch to single stage");
      native::RetireBridgeSetLocked(mixed_handle);
      const auto retained = native::bridge_retired.size();
      native::DrainBridgeRetired(native::bridge.fence_done12->GetCompletedValue());
      Check(!native::bridge_retired.empty() && native::bridge_retired.size() <= retained,
          "in-flight twins remain retired until actual done");
      release_stall.join();
      WaitForGpu(native::bridge.fence_done12, pending);
      native::DrainBridgeRetired(pending);
      mature(1, 0);
      Check(native::bridge_pending_delivery_value == 0, "recovery clears pending delivery after real completion");
      std::printf("PASS failures Render/partial/SR/exception/generation/ring/watchdog/retire/recovery aliased=%u\n",
          unsigned(aliased));
      // Existing GPU timers measure NR command spans; wall time and the bridge
      // completion ledger separately include the fixture's synchronization.
      native::gpu_timers_enabled.store(true);
      for (uint32_t mode = 0; fixture == 0 && !sr_only && mode < 3; ++mode) {
        const uint32_t count = (mode == 2 ? 2u : 1u);
        configure(count, mode == 0 ? 0u : 1u, false);
        mature(count, mode == 0 ? 0u : 1u);
        const auto cpu_start = native::SteadyNowNs();
        const auto wait_start = native::bridge_completion_wait_ns;
        const auto gpu_start = renodx::addons::dlss5::gpu_timers::measured_total_us;
        for (int sample = 0; sample < 20; ++sample) frame(false, false, false, true, false);
        std::printf("PERF synthetic 13x9->17x11 mode=%u wall_us/frame=%.3f completion_wait_us/frame=%.3f nr_gpu_us/frame=%.3f\n",
            mode, (native::SteadyNowNs() - cpu_start) / 20000.0,
            (native::bridge_completion_wait_ns - wait_start) / 20000.0,
            (renodx::addons::dlss5::gpu_timers::measured_total_us - gpu_start) / 20.0);
      }
      if (!aliased) {
        // Reuse one set of game textures across source handles inside a single
        // outer chain. Dimensions cannot identify the source or own its history.
        configure(2, 1, false);
        const auto saved_budget = native::max_worksets.exchange(8);
        const auto saved_create = native::ngx11_slot_real[0].create;
        const auto saved_release = native::ngx11_slot_real[0].release;
        native::ngx11_slot_real[0].create = reinterpret_cast<void*>(MotionCreateSource);
        native::ngx11_slot_real[0].release = reinterpret_cast<void*>(MotionReleaseSource);
        NVSDK_NGX_Handle* sources[]{
            reinterpret_cast<NVSDK_NGX_Handle*>(uintptr_t(0xD180 + fixture * 4)),
            reinterpret_cast<NVSDK_NGX_Handle*>(uintptr_t(0xD181 + fixture * 4)),
            reinterpret_cast<NVSDK_NGX_Handle*>(uintptr_t(0xD182 + fixture * 4))};
        const auto create_source = [&](uint32_t index) {
          source_create_result = sources[index];
          NVSDK_NGX_Handle* created = nullptr;
          Check(NVSDK_NGX_SUCCEED(native::HookedCreateFeature11Slot<0>(context.Get(),
                    index == 2 ? native::kFeatureDlssd : native::kFeatureDlss, &mixed_parameters, &created))
                    && created == sources[index], "DX11 source create wrapper records the returned handle");
        };
        const auto release_source = [&](uint32_t index) {
          Check(NVSDK_NGX_SUCCEED(native::HookedReleaseFeature11Slot<0>(sources[index])),
              "DX11 source release wrapper succeeds");
          Check(!native::features.contains(sources[index]) && !native::bridge_sets.contains(sources[index])
                    && !native::created_feature_ids.contains(sources[index]),
              "released source loses sessions, twins and feature identity");
          for (const auto& [key, workset] : native::worksets) {
            Check(key.handle != sources[index], "released source has no live stage workset");
          }
        };
        // Full transfer makes each model's deterministic GPU output observable.
        native::intensity.store(1.f);
        native::pass_intensity[0].store(1.f);
        native::pass_transfer_strength[0].store(1.f);
        native::pass_color_strength[0].store(1.f);
        native::look_mode.store(false);
        native::pass_look_result[0].follow.store(true);
        const std::vector<unsigned short> switch_pixels(width * height * 4, 0x3a00);
        context->UpdateSubresource(game_color.Get(), 0, nullptr, switch_pixels.data(), width * 8, 0);
        for (uint32_t index = 0; index < 3; ++index) create_source(index);
        UINT active_width = width, active_height = height;
        ComPtr<ID3D11Texture2D> active_color = game_color;
        const auto source_frame = [&](uint32_t index) {
          ++native::present_generation;
          context->UpdateSubresource(game_output.Get(), 0, nullptr, game_output_pixels.data(), output_width * 8, 0);
          FrameResult observed{.calls_before = model_calls};
          const auto result = native::Ngx11Evaluate(0, "source switch fixture", context.Get(), sources[index], &mixed_parameters, [&] {
            observed.calls_at_sr = model_calls;
            auto* input = params::GetD3D11Resource(&mixed_parameters, NVSDK_NGX_Parameter_Color);
            observed.redirected = input != active_color.Get();
            if (index == 2) {
              Check(!observed.redirected, "RR Render request never redirects Color");
            }
            const D3D11_BOX region{0, 0, 0, active_width, active_height, 1};
            context->CopySubresourceRegion(game_output.Get(), 0, 0, 0, 0, input, 0, &region);
            observed.sr_pixels = Read11(game_output.Get(), 8);
            return NVSDK_NGX_Result_Success;
          });
          Check(NVSDK_NGX_SUCCEED(result), "source switch preserves game evaluate result");
          Check(params::GetD3D11Resource(&mixed_parameters, NVSDK_NGX_Parameter_Color) == active_color.Get()
                    && params::GetD3D11Resource(&mixed_parameters, NVSDK_NGX_Parameter_Output) == game_output.Get(),
              "source switch restores game bindings");
          WaitForGpu(native::bridge.fence_done12, native::bridge_submitted_value.load());
          native::DrainRetiredResources(true);
          if (model_calls - observed.calls_before == 2) {
            Check(observed.calls_at_sr - observed.calls_before == (index == 2 ? 0u : 1u),
                "source-local feature type selects the actual side of SR/RR");
            Check(index == 2 || observed.redirected, "complete SR Render group delivers redirected Color");
            auto& sets = native::bridge_sets.at(sources[index]);
            Check(Read11(game_output.Get(), 8) == Read11(sets[0].output11, 8)
                      && Read11(game_output.Get(), 8) != observed.sr_pixels,
                "source switch delivers model GPU pixels through Upscaled writeback");
          }
          return observed;
        };
        const auto mature_source = [&](uint32_t index) {
          for (uint32_t attempt = 0; attempt < 16; ++attempt) {
            const auto observed = source_frame(index);
            Check(model_calls - observed.calls_before <= 2, "source maturation never duplicates passes");
            if (model_calls - observed.calls_before == 2) return observed;
          }
          Check(false, "source matures to two GPU passes");
          return FrameResult{};
        };
        std::array<std::array<ModelObservation, 2>, 3> previous{};
        const auto starts_with_reset = [&](uint64_t id) {
          for (const auto& observation : model_observations) {
            if (observation.id == id && observation.evaluation == 1) return observation.reset == 1;
          }
          return false;
        };
        for (uint32_t index = 0; index < 3; ++index) {
          mature_source(index);
          const auto steady = source_frame(index);
          Check(model_calls - steady.calls_before == 2, "source reaches steady state before interleaving");
          for (uint32_t pass = 0; pass < 2; ++pass) {
            previous[index][pass] = model_observations[steady.calls_before + pass];
            Check(starts_with_reset(previous[index][pass].id), "new source stage starts with independent Reset");
          }
        }
        for (uint32_t index = 0; index < 3; ++index) {
          const auto& state = native::features.at(sources[index]);
          Check(state.source_feature == (index == 2 ? native::kFeatureDlssd : native::kFeatureDlss),
              "same-size sources retain their own creation identity");
          Check(index == 2 ? !state.render_session : state.render_session != nullptr,
              "RR uses root slots while SR retains its Render session");
          for (uint32_t pass = 0; pass < 2; ++pass) {
            const auto* slot = (index != 2 && pass == 0 ? &state.render_session->slots[0]
                                                       : &state.slots[index == 2 ? pass : 0]);
            Check(slot->handle && reinterpret_cast<const MotionModel*>(slot->handle)->id == previous[index][pass].id,
                "model observations belong to the source-local stage slot");
            for (uint32_t other = 0; other < index; ++other) {
              for (const auto& model : previous[other]) {
                Check(previous[index][pass].id != model.id, "retained sources never share model instances");
              }
            }
          }
          Check(previous[index][0].id != previous[index][1].id, "each source owns two independent pass histories");
        }
        const auto check_continuity = [&](uint32_t index) {
          const auto observed = source_frame(index);
          Check(model_calls - observed.calls_before == 2, "every interleaved source executes both passes");
          for (uint32_t pass = 0; pass < 2; ++pass) {
            const auto& current = model_observations[observed.calls_before + pass];
            Check(current.id == previous[index][pass].id && current.evaluation == previous[index][pass].evaluation + 1
                      && current.reset == 0, "interleaving preserves source-local slot and temporal history");
            previous[index][pass] = current;
          }
        };
        for (uint32_t cycle = 0; cycle < 2; ++cycle) {
          const renodx::addons::dlss5::EvaluateChainScope outer_chain;
          for (const uint32_t index : {0u, 1u, 2u, 0u}) {
            check_continuity(index);
          }
        }
        const auto old_generation = native::features.at(sources[1]).source_generation;
        const auto old_models = previous[1];
        release_source(1);
        check_continuity(0);
        check_continuity(2);
        create_source(1);
        Check(native::features.at(sources[1]).source_generation != old_generation,
            "same-address recreate has a new source generation");
        const auto recreated = mature_source(1);
        for (uint32_t pass = 0; pass < 2; ++pass) {
          const auto& current = model_observations[recreated.calls_before + pass];
          Check(current.id != old_models[pass].id, "same-address recreate never inherits the old model");
          Check(starts_with_reset(current.id), "same-address recreate starts each stage with Reset");
          previous[1][pass] = current;
        }
        check_continuity(1);
        check_continuity(0);
        check_continuity(2);
        // Change only A's render size; the display target and other handles stay live.
        for (const bool smaller : {true, false}) {
          active_width = (smaller ? width - 2 : width);
          active_height = (smaller ? height - 2 : height);
          std::vector<unsigned short> pixels(active_width * active_height * 4, 0x3a00);
          active_color = make_texture(active_width, active_height, DXGI_FORMAT_R16G16B16A16_FLOAT,
              D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, pixels.data(), active_width * 8);
          const std::vector<uint32_t> zero_guides(active_width * active_height, 0);
          const auto active_motion = make_texture(active_width, active_height, DXGI_FORMAT_R16G16_FLOAT,
              D3D11_BIND_SHADER_RESOURCE, zero_guides.data(), active_width * 4);
          const auto active_depth = make_texture(active_width, active_height, DXGI_FORMAT_R32_FLOAT,
              D3D11_BIND_SHADER_RESOURCE, zero_guides.data(), active_width * 4);
          mixed_parameters.Set(NVSDK_NGX_Parameter_Color, static_cast<ID3D11Resource*>(active_color.Get()));
          mixed_parameters.Set(NVSDK_NGX_Parameter_MotionVectors, static_cast<ID3D11Resource*>(active_motion.Get()));
          mixed_parameters.Set(NVSDK_NGX_Parameter_Depth, static_cast<ID3D11Resource*>(active_depth.Get()));
          mixed_parameters.Set(NVSDK_NGX_Parameter_Width, active_width);
          mixed_parameters.Set(NVSDK_NGX_Parameter_Height, active_height);
          const auto before_resize = previous[0][0].id;
          FrameResult resized;
          for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 16; ++attempt) {
            const auto observed = source_frame(0);
            const auto& session = native::features.at(sources[0]).render_session;
            if (model_calls - observed.calls_before == 2 && session
                && session->input_width == active_width && session->input_height == active_height) {
              resized = observed;
              break;
            }
          }
          const auto& render = native::features.at(sources[0]).render_session;
          Check(render && render->input_width == active_width && render->input_height == active_height,
              "same source updates its Render working dimensions");
          Check(model_calls - resized.calls_before == 2, "resized source resumes both stages after contract debounce");
          Check(native::bridge_sets.at(sources[0])[1].color12->GetDesc().Width == active_width,
              "dynamic render size rebuilds the matching shared twin");
          for (uint32_t pass = 0; pass < 2; ++pass) {
            previous[0][pass] = model_observations[resized.calls_before + pass];
          }
          Check(previous[0][0].id != before_resize, "changed Render dimensions create an independent model");
          Check(starts_with_reset(previous[0][0].id), "resized Render model starts with Reset");
          check_continuity(0);
        }
        mixed_parameters.Set(NVSDK_NGX_Parameter_Color, static_cast<ID3D11Resource*>(game_color.Get()));
        mixed_parameters.Set(NVSDK_NGX_Parameter_MotionVectors, static_cast<ID3D11Resource*>(rg16.Get()));
        mixed_parameters.Set(NVSDK_NGX_Parameter_Depth, static_cast<ID3D11Resource*>(depth_direct.Get()));
        active_color = game_color;
        check_continuity(1);
        check_continuity(2);
        for (uint32_t index = 0; index < 3; ++index) release_source(index);
        native::ngx11_slot_real[0].create = saved_create;
        native::ngx11_slot_real[0].release = saved_release;
        source_create_result = nullptr;
        native::max_worksets.store(saved_budget);
        std::printf("PASS source switching SR A/B/RR C/A, retained history, same-address recreate, dynamic Render size %ux%u\n",
            output_width, output_height);
      }
      native::ForgetSourceHandleLocked(mixed_handle);
      native::RetireBridgeSetLocked(mixed_handle);
      Check(!native::features.contains(mixed_handle) && !native::bridge_sets.contains(mixed_handle),
          "source release removes both stage sessions and resident twins");
      for (const auto& [key, workset] : native::worksets) {
        Check(key.handle != mixed_handle, "source release retires both stage worksets");
      }
    }
    Check(native::DrainBridgeQueue(), "production bridge drained");
    native::ReleaseBridge();
    Check(native::bridge.motion_cs == nullptr && native::bridge_retired.empty()
              && native::bridge_pending_delivery_value == 0, "drained bridge releases shaders, twins and pending delivery");
    std::printf("PASS motion GPU regression model_calls=%u\n", model_calls);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
#endif
