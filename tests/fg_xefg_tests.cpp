#define RESHADE_ADDON 1
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <include/reshade.hpp>
#include <fg_xefg.hpp>
#include <fg_native_options.hpp>

namespace {
using Microsoft::WRL::ComPtr;
namespace fg = renodx::addons::dlss5::fg;
namespace xefg = fg::xefg;
namespace adapter = xefg::internal;
namespace submission = renodx::addons::dlss5::submission;
using Hooks = adapter::SdkHooks<0>;
constexpr int32_t HUDLESS = 0, DEPTH = 1, MOTION = 2, BACKBUFFER = 4;
constexpr int32_t UNTIL_NEXT_PRESENT = 0, ONLY_NOW = 1;
void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
sl::Result options_result = sl::Result::eOk;
uint32_t options_calls = 0;
uint32_t options_events = 0;
bool last_options_enabled = true;
sl::Result FakeOptions(const sl::ViewportHandle&, const sl::DLSSGOptions&) {
  ++options_calls;
  return options_result;
}
void OptionsChanged(bool enabled) noexcept {
  ++options_events;
  last_options_enabled = enabled;
}
struct Gpu {
  ComPtr<IDXGIFactory4> factory;
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  std::vector<ComPtr<ID3D12Resource>> uploads;
  uint64_t serial = 0;
  explicit Gpu(bool hardware = false) {
    ComPtr<IDXGIAdapter> warp;
    Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "DXGI factory");
    if (hardware) {
      for (UINT index = 0; ; ++index) {
        ComPtr<IDXGIAdapter1> candidate;
        if (factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 adapter_desc{};
        candidate->GetDesc1(&adapter_desc);
        if (adapter_desc.VendorId == 0x10DE && (adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
          candidate.As(&warp);
          std::wcout << L"GPU fixture: " << adapter_desc.Description << L'\n';
          break;
        }
      }
      Check(warp != nullptr, "NVIDIA hardware adapter");
    } else {
      Check(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))), "WARP adapter");
    }
    Check(SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))), "WARP device");
    D3D12_COMMAND_QUEUE_DESC desc{};
    Check(SUCCEEDED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))), "queue");
    Check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))), "allocator");
    Check(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                            IID_PPV_ARGS(&list))), "list");
    Check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))), "fence");
  }
  ComPtr<IDXGISwapChain3> Swapchain() {
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = 8;
    desc.Height = 8;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    ComPtr<IDXGISwapChain1> swapchain;
    Check(SUCCEEDED(factory->CreateSwapChainForComposition(queue.Get(), &desc, nullptr, &swapchain)), "composition swapchain");
    ComPtr<IDXGISwapChain3> result;
    Check(SUCCEEDED(swapchain.As(&result)), "swapchain3");
    return result;
  }
  ComPtr<ID3D12Resource> Texture() {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 8;
    desc.Height = 8;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    ComPtr<ID3D12Resource> result;
    Check(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&result))), "texture");
    return result;
  }
  ComPtr<ID3D12Resource> Buffer(uint64_t bytes, D3D12_HEAP_TYPE type) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{.Type = type};
    ComPtr<ID3D12Resource> result;
    Check(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        (type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST),
        nullptr, IID_PPV_ARGS(&result))), "buffer");
    return result;
  }
  void Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &barrier);
  }
  void Write(ID3D12Resource* resource, uint8_t value) {
    const auto desc = resource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    uint64_t bytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    auto upload = Buffer(bytes, D3D12_HEAP_TYPE_UPLOAD);
    void* mapped = nullptr;
    Check(SUCCEEDED(upload->Map(0, nullptr, &mapped)), "upload map");
    std::memset(mapped, value, static_cast<size_t>(bytes));
    upload->Unmap(0, nullptr);
    Transition(resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION source{.pResource = upload.Get(),
        .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = footprint};
    D3D12_TEXTURE_COPY_LOCATION destination{.pResource = resource,
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0};
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    Transition(resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    uploads.push_back(upload);
  }
  void Submit() {
    Check(SUCCEEDED(list->Close()), "close");
    ID3D12CommandList* lists[]{list.Get()};
    const auto pending = submission::BeginSubmission(1, lists);
    queue->ExecuteCommandLists(1, lists);
    Check(SUCCEEDED(queue->Signal(fence.Get(), ++serial)), "submit signal");
    submission::CompleteSubmission(pending, fence.Get(), serial);
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Check(event != nullptr, "completion event");
    const auto hr = fence->SetEventOnCompletion(serial, event);
    const auto wait = (SUCCEEDED(hr) ? WaitForSingleObject(event, 10000) : WAIT_FAILED);
    CloseHandle(event);
    Check(wait == WAIT_OBJECT_0, "bounded WARP completion");
    Check(SUCCEEDED(allocator->Reset()), "allocator reset");
    Check(SUCCEEDED(list->Reset(allocator.Get(), nullptr)), "list reset");
    submission::OnCommandListReset(list.Get());
    submission::PruneCompletedGenerations();
    uploads.clear();
  }
  void ExpectPixels(ID3D12Resource* resource, uint8_t value) {
    const auto desc = resource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    uint64_t bytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    auto readback = Buffer(bytes, D3D12_HEAP_TYPE_READBACK);
    Transition(resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source{.pResource = resource,
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0};
    D3D12_TEXTURE_COPY_LOCATION destination{.pResource = readback.Get(),
        .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = footprint};
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    Transition(resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    Submit();
    uint8_t* mapped = nullptr;
    Check(SUCCEEDED(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped))), "readback map");
    bool matches = true;
    for (UINT y = 0; y < desc.Height; ++y) {
      for (UINT x = 0; x < desc.Width * 4; ++x) {
        matches &= mapped[footprint.Offset + y * footprint.Footprint.RowPitch + x] == value;
      }
    }
    readback->Unmap(0, nullptr);
    Check(matches, "GPU pixels changed after ONLY_NOW source overwrite");
  }
};
struct SdkCall {
  void* context;
  ID3D12CommandList* list;
  uint32_t id;
  xefg::abi::Resource resource;
};
std::vector<SdkCall> sdk_calls;
std::vector<fg::FrameInput> service_calls;
int32_t sdk_status = 0;
bool policy = true;
bool service_observed_preparing = true;
fg::Result service_outcome = fg::Result::SUBMITTED;
std::vector<fg::Result> observed_display_results;
bool Requested() noexcept { return policy; }
bool Prepare(ID3D12GraphicsCommandList*) noexcept { return true; }
fg::Result Service(const fg::FrameInput& input) noexcept {
  service_observed_preparing &= xefg::CurrentResult() == fg::Result::PREPARED;
  observed_display_results.push_back(xefg::DisplayResult());
  service_calls.push_back(input);
  return (input.depth != nullptr && input.motion != nullptr && input.has_constants
      ? service_outcome : fg::Result::NOT_SERVED);
}
int32_t FakeInit(void*, ID3D12CommandQueue*, const xefg::abi::Init*) { return sdk_status; }
int32_t FakeResource(void* context, ID3D12CommandList* list, uint32_t id, const xefg::abi::Resource* resource) {
  if (resource != nullptr) sdk_calls.push_back({context, list, id, *resource});
  return sdk_status;
}
int32_t FakeConstants(void*, uint32_t, const xefg::abi::Constants*) { return sdk_status; }
int32_t FakeId(void*, uint32_t) { return sdk_status; }
uint32_t fake_present_calls = 0;
bool fake_present_owns_stream = false;
HRESULT STDMETHODCALLTYPE FakePresent(IDXGISwapChain*, UINT, UINT) {
  ++fake_present_calls;
  fake_present_owns_stream = xefg::OwnsStream();
  return S_OK;
}
HRESULT STDMETHODCALLTYPE FakePresent1(IDXGISwapChain1* swapchain, UINT interval, UINT flags,
                                      const DXGI_PRESENT_PARAMETERS*) {
  return adapter::ProxyHooks<0>::Present(swapchain, interval, flags);
}
struct Session {
  Gpu* gpu;
  ComPtr<IDXGISwapChain3> proxy;
  void* handle = this;
  std::shared_ptr<adapter::Context> context;
  explicit Session(Gpu* value) : gpu(value), proxy(value->Swapchain()) {
    xefg::abi::Init init{.application_swapchain = proxy.Get(), .flags = 17, .max_interpolated = 1};
    Check(Hooks::Init(handle, gpu->queue.Get(), &init) == 0, "SDK init");
    context = adapter::state.contexts.at(handle);
    context->proxy_identity = adapter::Identity(proxy.Get());
    context->enabled = true;
    context->ui_composition = true;
    adapter::RefreshOwnership();
  }
  ~Session() {
    // The fake SDK has no Destroy callback. Do not leave a dead proxy's
    // recycled COM identity eligible for another session's Present.
    context->enabled = false;
    context->proxy_identity = 0;
    adapter::RefreshOwnership();
  }
  xefg::abi::Resource Tag(uint32_t id, int32_t type, ID3D12Resource* resource, int32_t validity = UNTIL_NEXT_PRESENT) {
    const xefg::abi::Resource data{.type = type, .validity = validity, .base = {1, 2}, .size = {4, 3},
        .resource = resource, .state = D3D12_RESOURCE_STATE_COMMON};
    Check(Hooks::Resource(handle, gpu->list.Get(), id, &data) == sdk_status, "SDK resource result forwarding");
    Check(data.resource == resource && data.validity == validity, "caller tag mutated");
    return data;
  }
  void Guides(uint32_t id, ID3D12Resource* depth, ID3D12Resource* motion) {
    Tag(id, DEPTH, depth);
    Tag(id, MOTION, motion);
    const xefg::abi::Constants constants{.jitter_x = .25f, .jitter_y = -.5f,
        .motion_scale_x = 8.f, .motion_scale_y = -8.f, .frame_time_ms = 16.5f};
    Check(Hooks::Constants(handle, id, &constants) == sdk_status, "SDK constants result forwarding");
  }
  void Present(uint32_t id, UINT flags = 0) {
    Check(Hooks::PresentId(handle, id) == sdk_status, "SDK present id result forwarding");
    Check(SUCCEEDED(adapter::ProxyHooks<0>::Present(proxy.Get(), 0, flags)), "proxy present forwarding");
  }
};
}  // namespace
int main() {
  try {
    fg::native_options::internal::state.real = &FakeOptions;
    fg::native_options::Configure(&OptionsChanged);
    sl::DLSSGOptions options;
    const sl::ViewportHandle viewport(0);
    options_result = sl::Result::eErrorInvalidParameter;
    Check(fg::native_options::internal::HookedSetOptions(viewport, options) == options_result
        && options_events == 0, "rejected native Off changed ownership");
    options_result = sl::Result::eOk;
    Check(fg::native_options::internal::HookedSetOptions(viewport, options) == sl::Result::eOk
        && options_events == 1 && !last_options_enabled, "successful native Off not forwarded");
    options.mode = sl::DLSSGMode::eOn;
    fg::native_options::internal::HookedSetOptions(viewport, options);
    Check(options_events == 2 && last_options_enabled, "native enable request left Off latched");
    fg::native_options::Stop();
    options.mode = sl::DLSSGMode::eOff;
    fg::native_options::internal::HookedSetOptions(viewport, options);
    Check(options_events == 2 && options_calls == 4, "stopped native hook did not pass through");
    std::cout << "PASS native options result, explicit Off/On and stopped pass-through\n";
    adapter::modules[0].complete = true;
    adapter::modules[0].real[0] = reinterpret_cast<void*>(&FakeInit);
    adapter::modules[0].real[3] = reinterpret_cast<void*>(&FakeResource);
    adapter::modules[0].real[4] = reinterpret_cast<void*>(&FakeConstants);
    adapter::modules[0].real[5] = reinterpret_cast<void*>(&FakeId);
    adapter::proxies[0].real[0] = reinterpret_cast<void*>(&FakePresent);
    adapter::proxies[0].real[1] = reinterpret_cast<void*>(&FakePresent1);
    xefg::Configure(&Service, &Prepare, nullptr, &Requested);
    Gpu gpu;
    {
      Session nested(&gpu);
      auto depth = gpu.Texture();
      auto motion = gpu.Texture();
      bool nested_contracts_passed = true;
      for (uint32_t mode = 0; mode < 3; ++mode) {
        nested.context->enabled = mode != 0;
        policy = mode != 1;
        adapter::RefreshOwnership();
        nested.Guides(mode, depth.Get(), motion.Get());
        gpu.Submit();
        Check(Hooks::PresentId(nested.handle, mode) == 0, "nested SDK present id");
        const auto application_frame = nested.context->application_frame;
        const auto next_fence = nested.context->next_fence;
        const auto calls = service_calls.size();
        const auto presents = fake_present_calls;
        nested.context->interval_start = std::chrono::steady_clock::now();
        nested.context->interval_presents = 0;
        DXGI_PRESENT_PARAMETERS parameters{};
        Check(SUCCEEDED(adapter::ProxyHooks<0>::Present1(nested.proxy.Get(), 0, 0, &parameters)),
              "nested proxy Present1 forwarding");
        const auto frames_delta = nested.context->application_frame - application_frame;
        const auto fence_delta = nested.context->next_fence - next_fence;
        const auto service_delta = service_calls.size() - calls;
        const bool owned = mode == 2;
        const bool passed = frames_delta == 1 && fence_delta == 1 && nested.context->interval_presents == 1
            && service_delta == (owned ? 1u : 0u) && fake_present_calls == presents + 1
            && fake_present_owns_stream == owned && xefg::OwnsStream() == owned;
        nested_contracts_passed &= passed;
        std::cout << (passed ? "PASS" : "FAIL") << " nested Present1->Present "
                  << (mode == 0 ? "FGOff" : (mode == 1 ? "policyOff" : "FGOn"))
                  << " application_frame_delta=" << frames_delta << " next_fence_delta=" << fence_delta
                  << " interval_presents=" << nested.context->interval_presents
                  << " service_delta=" << service_delta << " owns_stream=" << fake_present_owns_stream << '\n';
      }
      Check(nested_contracts_passed, "nested proxy Present counted more than once or changed FG ownership/service");
      nested.context->enabled = false;
      adapter::RefreshOwnership();
      service_calls.clear();
      gpu.Submit();
    }
    Session first(&gpu), second(&gpu);
    auto color = gpu.Texture();
    auto depth = gpu.Texture();
    auto motion = gpu.Texture();
    auto decoy = gpu.Texture();
    gpu.Write(color.Get(), 0x31);
    gpu.Write(depth.Get(), 0x52);
    gpu.Write(motion.Get(), 0x73);
    const auto original = first.Tag(10, HUDLESS, color.Get(), ONLY_NOW);
    const auto forwarded = sdk_calls.back();
    Check(forwarded.resource.resource != color.Get() && forwarded.resource.validity == UNTIL_NEXT_PRESENT,
          "HUDLESS ONLY_NOW must forward owned UNTIL_NEXT_PRESENT texture");
    Check(forwarded.context == first.handle && forwarded.id == 10 && forwarded.list == gpu.list.Get()
        && forwarded.resource.type == original.type && forwarded.resource.base.x == original.base.x
        && forwarded.resource.base.y == original.base.y && forwarded.resource.size.x == original.size.x
        && forwarded.resource.size.y == original.size.y, "SDK routing/region parameters changed");
    first.Tag(10, DEPTH, depth.Get(), ONLY_NOW);
    auto captured_depth = first.context->frames.at(10)->resources[DEPTH].resource;
    Check(sdk_calls.back().resource.resource == depth.Get() && sdk_calls.back().resource.validity == ONLY_NOW,
          "non-HUDLESS SDK input changed");
    first.Tag(10, MOTION, motion.Get(), ONLY_NOW);
    auto captured_motion = first.context->frames.at(10)->resources[MOTION].resource;
    const xefg::abi::Constants constants{.motion_scale_x = 8.f, .motion_scale_y = -8.f, .frame_time_ms = 16.5f};
    Check(Hooks::Constants(first.handle, 10, &constants) == 0, "constants");
    first.Tag(10, BACKBUFFER, decoy.Get());
    gpu.Write(color.Get(), 0x91);
    gpu.Write(depth.Get(), 0xa2);
    gpu.Write(motion.Get(), 0xb3);
    gpu.Submit();
    gpu.ExpectPixels(forwarded.resource.resource, 0x31);
    gpu.ExpectPixels(captured_depth.Get(), 0x52);
    gpu.ExpectPixels(captured_motion.Get(), 0x73);
    gpu.ExpectPixels(color.Get(), 0x91);
    std::cout << "PASS ONLY_NOW GPU snapshots survive source rewrites; HUDLESS SDK forwarding\n";
    const auto submitted_before = xefg::SubmittedFrames();
    first.Present(10, DXGI_PRESENT_TEST);
    Check(service_calls.empty() && first.context->frames.contains(10), "TEST Present consumed frame");
    Check(xefg::SubmittedFrames() == submitted_before, "TEST Present inflated submitted frame count");
    first.Present(10);
    Check(service_calls.size() == 1 && xefg::CurrentResult() == fg::Result::SUBMITTED, "valid frame not served");
    Check(xefg::SubmittedFrames() == submitted_before + 1, "real service submission not counted once");
    Check(service_observed_preparing, "active recording is exposed as unserved to concurrent UI");
    Check(service_calls.back().hudless == forwarded.resource.resource
        && service_calls.back().depth == captured_depth.Get() && service_calls.back().motion == captured_motion.Get(),
          "service received original resources instead of ONLY_NOW snapshots");
    ComPtr<ID3D12Resource> real_backbuffer;
    Check(SUCCEEDED(first.proxy->GetBuffer(first.proxy->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&real_backbuffer))), "real BB");
    Check(service_calls.back().backbuffer == real_backbuffer.Get() && service_calls.back().backbuffer != decoy.Get(),
          "BACKBUFFER tag substituted for proxy GetBuffer");
    first.Present(10);
    Check(service_calls.size() == 1, "duplicate id served twice");
    Check(xefg::SubmittedFrames() == submitted_before + 1, "repeated ID inflated submitted frame count");
    Check(SUCCEEDED(adapter::ProxyHooks<0>::Present(first.proxy.Get(), 0, 0)), "repeat Present");
    Check(service_calls.size() == 2 && !service_calls.back().has_present_id
        && service_calls.back().hudless == nullptr && service_calls.back().depth == nullptr
        && service_calls.back().motion == nullptr && !service_calls.back().has_constants,
          "untagged application Present did not receive isolated neutral NR input");
    std::cout << "PASS TEST/repeated Present and real proxy backbuffer\n";
    first.Guides(20, depth.Get(), motion.Get());
    second.Tag(20, HUDLESS, color.Get());
    second.Present(20);
    Check(service_calls.back().context == reinterpret_cast<uintptr_t>(second.handle)
        && service_calls.back().depth == nullptr && service_calls.back().motion == nullptr,
          "guides leaked across contexts sharing presentId");
    Check(xefg::CurrentResult() == fg::Result::NOT_SERVED, "missing guides incorrectly successful");
    first.Present(20);
    Check(service_calls.back().context == reinterpret_cast<uintptr_t>(first.handle)
        && service_calls.back().depth == depth.Get(), "matching context frame lost");
    first.Tag(21, HUDLESS, color.Get());
    first.Present(21);
    Check(service_calls.back().depth == nullptr && service_calls.back().motion == nullptr
        && !service_calls.back().has_constants, "previous presentId guides reused");
    std::cout << "PASS context+presentId association and no stale guides\n";
    first.Guides(30, depth.Get(), motion.Get());
    first.Tag(30, HUDLESS, color.Get(), ONLY_NOW);
    second.Guides(31, depth.Get(), motion.Get());
    const auto calls_before_unrelated = service_calls.size();
    second.Present(31);
    Check(service_calls.size() == calls_before_unrelated + 1 && xefg::CurrentResult() == fg::Result::SUBMITTED,
          "unrelated unsubmitted capture blocked another context");
    const auto calls_before_unsubmitted = service_calls.size();
    first.Present(30);
    Check(service_calls.size() == calls_before_unsubmitted && xefg::CurrentResult() != fg::Result::SUBMITTED,
          "unsubmitted relevant copy served");
    gpu.Submit();
    std::cout << "PASS relevant unsubmitted copy blocks service; unrelated context remains serviceable\n";
    first.Guides(40, depth.Get(), motion.Get());
    sdk_status = -7;
    first.Tag(40, HUDLESS, color.Get(), ONLY_NOW);
    sdk_status = 0;
    gpu.Submit();
    const auto calls_before_rejection = service_calls.size();
    first.Present(40);
    Check(service_calls.size() == calls_before_rejection && xefg::CurrentResult() != fg::Result::SUBMITTED,
          "SDK rejected resource reported successful");
    first.Guides(41, depth.Get(), motion.Get());
    sdk_status = -9;
    Check(Hooks::Constants(first.handle, 41, &constants) == -9, "constants SDK rejection masked");
    sdk_status = 0;
    first.Present(41);
    Check(!service_calls.back().has_constants && xefg::CurrentResult() != fg::Result::SUBMITTED,
          "rejected constants reported successful");
    std::cout << "PASS SDK resource/constants rejection\n";
    Session consumer(&gpu);
    consumer.Guides(50, depth.Get(), motion.Get());
    consumer.Tag(50, HUDLESS, color.Get(), ONLY_NOW);
    const auto retained = consumer.context->frames.at(50)->resources[HUDLESS].copy;
    gpu.Submit();
    ComPtr<ID3D12Fence> gate;
    Check(SUCCEEDED(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))), "consumer gate");
    Check(SUCCEEDED(gpu.queue->Wait(gate.Get(), 1)), "consumer queue wait");
    consumer.Present(50);
    const bool consumer_pending = retained->consumer_fence != nullptr
        && retained->consumer_fence->GetCompletedValue() < retained->consumer_value;
    consumer.Tag(51, HUDLESS, color.Get(), ONLY_NOW);
    const auto next = consumer.context->frames.at(51)->resources[HUDLESS].copy;
    const bool distinct_while_pending = next != retained;
    // Unblock the real queue before an assertion can unwind the host.
    Check(SUCCEEDED(gate->Signal(1)), "release consumer gate");
    gpu.Submit();
    Check(consumer_pending && distinct_while_pending, "surface reused before proxy consumer fence completion");
    Check(retained->consumer_fence->GetCompletedValue() >= retained->consumer_value, "consumer never completed");
    consumer.Tag(52, HUDLESS, color.Get(), ONLY_NOW);
    Check(consumer.context->frames.at(52)->resources[HUDLESS].copy == retained,
          "completed surface not returned to pool");
    gpu.Submit();
    std::cout << "PASS normal Present consumer fence prevents early surface reuse\n";
    policy = false;
    consumer.Present(51);
    Check(!consumer.context->frames.contains(51) && !next->reserved,
          "policy-off Present failed to release previously substituted input");
    consumer.Present(52);
    policy = true;
    gpu.Submit();
    std::cout << "PASS policy-off Present cleanup\n";
    Session routing(&gpu);
    for (uint32_t mode = 0; mode <= 5; ++mode) {
      for (const bool composition : {false, true}) {
        routing.context->init.ui_mode = static_cast<int32_t>(mode);
        routing.context->ui_composition = composition;
        const uint32_t id = 60 + mode * 2 + static_cast<uint32_t>(composition);
        routing.Guides(id, depth.Get(), motion.Get());
        routing.Tag(id, HUDLESS, color.Get());
        const auto tagged = sdk_calls.back().resource.resource;
        routing.Present(id);
        const bool consumes_hudless = !composition || (mode != 1 && mode != 2);
        Check((service_calls.back().hudless != nullptr) == consumes_hudless,
              "NR colors do not follow XeFG UI composition consumer mode");
        Check(tagged == color.Get(), "consumer routing changed SDK HUDLess input");
        Check(service_calls.back().backbuffer != nullptr, "consumer routing lost actual backbuffer");
      }
    }
    gpu.Submit();
    submission::ResetAll();
    std::cout << "PASS public UI modes select NR color consumers without changing SDK tags\n";
    xefg::Configure(&Service, &Prepare, nullptr, &Requested, true);
    Session final_color(&gpu);
    final_color.context->ui_composition = false;
    final_color.Guides(80, depth.Get(), motion.Get());
    final_color.Tag(80, 3, decoy.Get());
    const auto before_optional = sdk_calls.size();
    final_color.Tag(80, HUDLESS, color.Get(), ONLY_NOW);
    Check(sdk_calls.size() == before_optional, "optional HUDLess still reached SDK in final-color mode");
    Check(final_color.context->frames.at(80)->captures.empty(), "blocked optional HUDLess still copied");
    final_color.Present(80);
    Check(service_calls.back().hudless == nullptr && service_calls.back().backbuffer != nullptr
        && service_calls.back().depth == depth.Get() && service_calls.back().motion == motion.Get()
        && service_calls.back().ui == decoy.Get(),
        "final-color mode changed guides or kept a second NR consumer");
    policy = false;
    final_color.Tag(81, HUDLESS, color.Get());
    Check(sdk_calls.size() == before_optional, "NR Off changed the selected FG color contract");
    final_color.Present(81);
    policy = true;
    final_color.context->ui_composition = true;
    final_color.Tag(82, HUDLESS, color.Get());
    Check(sdk_calls.size() == before_optional + 1, "active UI composition lost HUDLess");
    final_color.Present(82);
    final_color.context->ui_composition = false;
    final_color.context->enabled = false;
    final_color.Tag(83, HUDLESS, color.Get());
    Check(sdk_calls.size() == before_optional + 2, "FG Off did not pass HUDLess through");
    final_color.Present(83);
    final_color.context->enabled = true;
    sdk_status = -7;
    final_color.Tag(85, HUDLESS, color.Get(), 2);
    Check(sdk_calls.size() == before_optional + 3, "unsupported tag validity was hidden");
    const xefg::abi::Resource invalid_region{.type = HUDLESS, .validity = ONLY_NOW,
        .size = {0, 0}, .resource = color.Get()};
    Check(Hooks::Resource(final_color.handle, gpu.list.Get(), 86, &invalid_region) == -7,
          "invalid region error was hidden");
    const xefg::abi::Resource missing_list{.type = HUDLESS, .validity = ONLY_NOW,
        .size = {4, 3}, .resource = color.Get()};
    Check(Hooks::Resource(final_color.handle, nullptr, 87, &missing_list) == -7,
          "ONLY_NOW without command list error was hidden");
    sdk_status = 0;
    xefg::Configure(&Service, &Prepare, nullptr, &Requested);
    final_color.Tag(84, HUDLESS, color.Get());
    Check(sdk_calls.size() == before_optional + 6, "default color policy changed SDK tags");
    final_color.Present(84);
    gpu.Submit();
    submission::ResetAll();
    std::cout << "PASS optional HUDLess is omitted at SDK boundary only in explicit final-color mode\n";
    {
      Session continuous(&gpu);
      const auto submit = [&](uint32_t id, fg::Result outcome) {
        service_outcome = outcome;
        continuous.Guides(id, depth.Get(), motion.Get());
        continuous.Present(id);
      };
      submit(900, fg::Result::SUBMITTED);
      Check(observed_display_results.back() == fg::Result::PREPARED, "new context inherited active service");
      submit(901, fg::Result::SUBMITTED);
      Check(observed_display_results.back() == fg::Result::SUBMITTED
                && xefg::CurrentResult() == fg::Result::SUBMITTED,
            "adjacent successful frames flash waiting while recording");
      continuous.Present(902);
      Check(xefg::DisplayResult() == fg::Result::NOT_SERVED, "declined return retained an active display");
      submit(903, fg::Result::SUBMITTED);
      Check(observed_display_results.back() == fg::Result::PREPARED, "submission gap retained active recording");
      const auto count = xefg::SubmittedFrames();
      submit(904, fg::Result::PREPARED);
      Check(observed_display_results.back() == fg::Result::SUBMITTED
                && xefg::DisplayResult() == fg::Result::PREPARED && xefg::SubmittedFrames() == count,
            "completed preparation fabricated a successful submission");
      submit(905, fg::Result::FAILED);
      Check(observed_display_results.back() == fg::Result::PREPARED
                && xefg::DisplayResult() == fg::Result::FAILED,
            "actual failure was hidden by service activity");
      submit(906, fg::Result::SUBMITTED);
      adapter::BeforeResize(continuous.proxy.Get());
      submit(907, fg::Result::SUBMITTED);
      Check(observed_display_results.back() == fg::Result::PREPARED, "new epoch inherited active recording");
      policy = false;
      Check(xefg::DisplayResult() == fg::Result::NOT_SERVED, "NR Off retained active display");
      continuous.Present(908);
      policy = true;
      submit(909, fg::Result::SUBMITTED);
      Check(observed_display_results.back() == fg::Result::PREPARED, "NR Off gap inherited active recording");
      Check(service_observed_preparing, "display service state changed exact frame handoff truth");
      std::cout << "PASS continuous UI service with exact phase, gap, pending, failure, epoch and Off truth\n";
    }
    gpu.Submit();
    submission::ResetAll();
    std::cout << "All XeFG adapter WARP contracts passed (fake SDK slots; no SDK/game loaded).\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
