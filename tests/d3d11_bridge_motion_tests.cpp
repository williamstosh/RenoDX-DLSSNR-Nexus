#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <initializer_list>
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
NVSDK_NGX_Result NVSDK_CONV MotionCreateModel(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
    const NVSDK_NGX_Parameter*, NVSDK_NGX_Handle** handle) {
  *handle = reinterpret_cast<NVSDK_NGX_Handle*>(new uint32_t(1));
  return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result NVSDK_CONV MotionReleaseModel(NVSDK_NGX_Handle* handle) {
  delete reinterpret_cast<uint32_t*>(handle);
  return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result NVSDK_CONV MotionEvaluateModel(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle*,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback) {
  ++model_calls;
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
  // A positive finite model result exercises real decode and writeback.
  for (UINT y = 0; y < desc.Height; ++y) {
    auto* row = reinterpret_cast<unsigned short*>(static_cast<unsigned char*>(mapped) + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch);
    for (UINT x = 0; x < desc.Width * 4; ++x) row[x] = 0x3400;
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
std::vector<unsigned char> Read12(ID3D12Resource* texture, UINT pixel_bytes) {
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
  native::Transition(list.Get(), texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION from{.pResource = texture, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  D3D12_TEXTURE_COPY_LOCATION to{.pResource = readback.Get(),
      .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = footprint};
  list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  native::Transition(list.Get(), texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
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
    const bool warp = argc > 1 && std::strcmp(argv[1], "--warp") == 0;
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
            if (delivered) Check(params::GetD3D11Resource(&parameters, NVSDK_NGX_Parameter_Color) == native::bridge_sets.at(handle).color11,
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
        auto& set = native::bridge_sets.at(handle);
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
    Check(native::DrainBridgeQueue(), "production bridge drained");
    native::ReleaseBridge();
    Check(native::bridge.motion_cs == nullptr, "bridge motion shader released");
    std::printf("PASS motion GPU regression model_calls=%u\n", model_calls);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
#endif
