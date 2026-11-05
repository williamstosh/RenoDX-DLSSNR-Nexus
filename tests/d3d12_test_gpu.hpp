#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include <submission_tracker.hpp>

namespace d3d12_test {
using Microsoft::WRL::ComPtr;
namespace submission = renodx::addons::dlss5::submission;

struct TextureTransfer {
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT rows = 0;
  UINT64 row_bytes = 0;
  UINT64 allocation_bytes = 0;
  ComPtr<ID3D12Resource> upload;
  ComPtr<ID3D12Resource> readback;
};

inline void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
struct Gpu {
  ComPtr<IDXGIFactory4> factory;
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  std::vector<ComPtr<ID3D12Resource>> retained_resources;
  uint64_t serial = 0;
  bool submission_pending = false;
  explicit Gpu(bool hardware = false) {
    ComPtr<IDXGIAdapter> adapter;
    Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "DXGI factory");
    if (hardware) {
      for (UINT index = 0; ; ++index) {
        ComPtr<IDXGIAdapter1> candidate;
        if (factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 adapter_desc{};
        candidate->GetDesc1(&adapter_desc);
        if (adapter_desc.VendorId == 0x10DE && (adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
          candidate.As(&adapter);
          std::wcout << L"GPU fixture: " << adapter_desc.Description << L'\n';
          break;
        }
      }
      Check(adapter != nullptr, "NVIDIA hardware adapter");
    } else {
      Check(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))), "WARP adapter");
    }
    Check(SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))), "D3D12 device");
    D3D12_COMMAND_QUEUE_DESC desc{};
    Check(SUCCEEDED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))), "queue");
    Check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))), "allocator");
    Check(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                            IID_PPV_ARGS(&list))), "list");
    Check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))), "fence");
  }
  ~Gpu() {
    if (submission_pending && fence->GetCompletedValue() < serial
        && device->GetDeviceRemovedReason() != DXGI_ERROR_DEVICE_REMOVED) {
      // A failed isolated test process must not release resources still referenced by queued work.
      for (auto& resource : retained_resources) {
        resource.Detach();
      }
    }
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
  ComPtr<ID3D12Resource> Texture(UINT width = 8, UINT height = 8,
                                 DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM,
                                 D3D12_RESOURCE_STATES initial_state = D3D12_RESOURCE_STATE_COMMON) {
    Check(width > 0 && height > 0, "texture dimensions");
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    ComPtr<ID3D12Resource> result;
    Check(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        initial_state, nullptr, IID_PPV_ARGS(&result))), "texture");
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
  TextureTransfer CreateTextureTransfer(ID3D12Resource* texture) {
    Check(texture != nullptr, "texture transfer resource");
    const auto desc = texture->GetDesc();
    Check(desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.DepthOrArraySize == 1
              && desc.MipLevels == 1 && desc.SampleDesc.Count == 1,
          "texture transfer requires a single-sample 2D subresource");
    TextureTransfer transfer{};
    device->GetCopyableFootprints(&desc, 0, 1, 0, &transfer.footprint, &transfer.rows,
                                  &transfer.row_bytes, &transfer.allocation_bytes);
    Check(transfer.rows == desc.Height && transfer.row_bytes > 0 && transfer.allocation_bytes > 0
              && transfer.row_bytes <= std::numeric_limits<size_t>::max() / transfer.rows
              && transfer.allocation_bytes <= std::numeric_limits<size_t>::max(),
          "texture copy footprint");
    transfer.upload = Buffer(transfer.allocation_bytes, D3D12_HEAP_TYPE_UPLOAD);
    transfer.readback = Buffer(transfer.allocation_bytes, D3D12_HEAP_TYPE_READBACK);
    return transfer;
  }
  void UploadTexture(ID3D12Resource* texture, TextureTransfer& transfer,
                     const uint8_t* bytes, size_t byte_count, UINT64 tight_row_bytes) {
    Check(texture != nullptr && bytes != nullptr && transfer.upload != nullptr, "texture upload arguments");
    Check(tight_row_bytes == transfer.row_bytes && byte_count == tight_row_bytes * transfer.rows,
          "texture upload shape");
    uint8_t* mapped = nullptr;
    const D3D12_RANGE no_reads{0, 0};
    Check(SUCCEEDED(transfer.upload->Map(0, &no_reads, reinterpret_cast<void**>(&mapped))), "upload map");
    std::memset(mapped, 0, static_cast<size_t>(transfer.allocation_bytes));
    for (UINT y = 0; y < transfer.rows; ++y) {
      std::memcpy(mapped + transfer.footprint.Offset + static_cast<size_t>(y) * transfer.footprint.Footprint.RowPitch,
                  bytes + static_cast<size_t>(y) * static_cast<size_t>(tight_row_bytes),
                  static_cast<size_t>(tight_row_bytes));
    }
    const D3D12_RANGE written{0, static_cast<SIZE_T>(transfer.allocation_bytes)};
    transfer.upload->Unmap(0, &written);
    const D3D12_TEXTURE_COPY_LOCATION source{.pResource = transfer.upload.Get(),
        .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = transfer.footprint};
    const D3D12_TEXTURE_COPY_LOCATION destination{.pResource = texture,
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0};
    Retain(transfer.upload.Get());
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
  }
  void RecordTextureReadback(ID3D12Resource* texture, const TextureTransfer& transfer) {
    Check(texture != nullptr && transfer.readback != nullptr, "texture readback arguments");
    Retain(transfer.readback.Get());
    const D3D12_TEXTURE_COPY_LOCATION source{.pResource = texture,
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0};
    const D3D12_TEXTURE_COPY_LOCATION destination{.pResource = transfer.readback.Get(),
        .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = transfer.footprint};
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
  }
  std::vector<uint8_t> ReadTexture(const TextureTransfer& transfer) {
    Check(transfer.readback != nullptr && transfer.row_bytes > 0 && transfer.rows > 0,
          "texture readback buffer");
    const auto byte_count = static_cast<size_t>(transfer.row_bytes) * transfer.rows;
    std::vector<uint8_t> result(byte_count);
    uint8_t* mapped = nullptr;
    const D3D12_RANGE read_range{0, static_cast<SIZE_T>(transfer.allocation_bytes)};
    Check(SUCCEEDED(transfer.readback->Map(0, &read_range, reinterpret_cast<void**>(&mapped))), "readback map");
    for (UINT y = 0; y < transfer.rows; ++y) {
      std::memcpy(result.data() + static_cast<size_t>(y) * static_cast<size_t>(transfer.row_bytes),
                  mapped + transfer.footprint.Offset + static_cast<size_t>(y) * transfer.footprint.Footprint.RowPitch,
                  static_cast<size_t>(transfer.row_bytes));
    }
    const D3D12_RANGE no_writes{0, 0};
    transfer.readback->Unmap(0, &no_writes);
    return result;
  }
  void Retain(ID3D12Resource* resource) {
    Check(resource != nullptr, "retained resource");
    ComPtr<ID3D12Resource> retained;
    retained = resource;
    retained_resources.push_back(std::move(retained));
  }
  void Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    Retain(resource);
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
    Retain(upload.Get());
  }
  void Submit() {
    Check(SUCCEEDED(list->Close()), "close");
    ID3D12CommandList* lists[]{list.Get()};
    const auto pending = submission::BeginSubmission(1, lists);
    queue->ExecuteCommandLists(1, lists);
    submission_pending = true;
    Check(SUCCEEDED(queue->Signal(fence.Get(), ++serial)), "submit signal");
    submission::CompleteSubmission(pending, fence.Get(), serial);
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Check(event != nullptr, "completion event");
    const auto hr = fence->SetEventOnCompletion(serial, event);
    const auto wait = (SUCCEEDED(hr) ? WaitForSingleObject(event, 10000) : WAIT_FAILED);
    CloseHandle(event);
    Check(wait == WAIT_OBJECT_0, "bounded GPU completion");
    submission_pending = false;
    Check(SUCCEEDED(allocator->Reset()), "allocator reset");
    Check(SUCCEEDED(list->Reset(allocator.Get(), nullptr)), "list reset");
    submission::OnCommandListReset(list.Get());
    submission::PruneCompletedGenerations();
    retained_resources.clear();
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

struct TimestampSample {
  uint64_t begin_tick = 0;
  uint64_t end_tick = 0;
  uint64_t ticks = 0;
  double gpu_ms = 0.0;
};

struct TimestampQueries {
  ComPtr<ID3D12QueryHeap> heap;
  ComPtr<ID3D12Resource> readback;
  uint64_t frequency_hz = 0;
  uint64_t readback_bytes = 0;
  UINT interval_count = 0;

  void Begin(ID3D12GraphicsCommandList* list, UINT interval) const {
    Check(list != nullptr && heap != nullptr && interval < interval_count, "timestamp begin arguments");
    list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * interval);
  }

  void EndAndResolve(ID3D12GraphicsCommandList* list, UINT interval) const {
    Check(list != nullptr && heap != nullptr && readback != nullptr && interval < interval_count,
          "timestamp end arguments");
    const UINT query = 2 * interval;
    list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query + 1);
    list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query, 2,
                           readback.Get(), static_cast<uint64_t>(query) * sizeof(uint64_t));
  }

  TimestampSample Read(UINT interval) const {
    Check(readback != nullptr && interval < interval_count, "timestamp read arguments");
    const auto offset = static_cast<SIZE_T>(2ull * interval * sizeof(uint64_t));
    const D3D12_RANGE range{offset, offset + 2 * sizeof(uint64_t)};
    uint64_t* values = nullptr;
    Check(SUCCEEDED(readback->Map(0, &range, reinterpret_cast<void**>(&values))), "timestamp map");
    const auto begin_tick = values[2 * interval];
    const auto end_tick = values[2 * interval + 1];
    const D3D12_RANGE no_writes{0, 0};
    readback->Unmap(0, &no_writes);
    return MakeSample(begin_tick, end_tick);
  }

  std::vector<TimestampSample> ReadAll() const {
    Check(readback != nullptr && interval_count > 0, "timestamp readback buffer");
    const D3D12_RANGE range{0, static_cast<SIZE_T>(readback_bytes)};
    uint64_t* values = nullptr;
    Check(SUCCEEDED(readback->Map(0, &range, reinterpret_cast<void**>(&values))), "timestamp map");
    std::vector<std::array<uint64_t, 2>> ticks;
    ticks.reserve(interval_count);
    for (UINT interval = 0; interval < interval_count; ++interval) {
      ticks.push_back({values[2 * interval], values[2 * interval + 1]});
    }
    const D3D12_RANGE no_writes{0, 0};
    readback->Unmap(0, &no_writes);
    std::vector<TimestampSample> samples;
    samples.reserve(interval_count);
    for (const auto& interval_ticks : ticks) {
      samples.push_back(MakeSample(interval_ticks[0], interval_ticks[1]));
    }
    return samples;
  }

 private:
  TimestampSample MakeSample(uint64_t begin_tick, uint64_t end_tick) const {
    Check(end_tick >= begin_tick && frequency_hz != 0, "timestamp order or frequency");
    const auto ticks = end_tick - begin_tick;
    return {begin_tick, end_tick, ticks,
            static_cast<double>(ticks) * 1000.0 / static_cast<double>(frequency_hz)};
  }
};

inline uint64_t TimestampFrequency(ID3D12CommandQueue* queue) {
  UINT64 frequency = 0;
  Check(queue != nullptr && SUCCEEDED(queue->GetTimestampFrequency(&frequency)) && frequency != 0,
        "GPU timestamp frequency");
  return frequency;
}

inline TimestampQueries CreateTimestampQueries(Gpu& gpu, UINT interval_count) {
  Check(interval_count > 0 && interval_count <= std::numeric_limits<UINT>::max() / 2,
        "timestamp interval count");
  TimestampQueries result{};
  result.interval_count = interval_count;
  result.frequency_hz = TimestampFrequency(gpu.queue.Get());
  const D3D12_QUERY_HEAP_DESC query_desc{.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP,
                                          .Count = 2 * interval_count};
  Check(SUCCEEDED(gpu.device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&result.heap))), "timestamp query heap");
  result.readback_bytes = 2ull * interval_count * sizeof(uint64_t);
  Check(result.readback_bytes <= std::numeric_limits<SIZE_T>::max(), "timestamp readback size");
  result.readback = gpu.Buffer(result.readback_bytes, D3D12_HEAP_TYPE_READBACK);
  return result;
}
}  // namespace d3d12_test
