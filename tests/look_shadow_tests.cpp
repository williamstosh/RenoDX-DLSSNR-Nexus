/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#include <DirectXPackedVector.h>
#include <d3dcompiler.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "d3d12_test_gpu.hpp"
#include "look_stage.hpp"

namespace {
using d3d12_test::Check;
using d3d12_test::ComPtr;
using nlohmann::json;
namespace look = renodx::addons::dlss5::look;
using DirectX::PackedVector::XMConvertFloatToHalf;
using DirectX::PackedVector::XMConvertHalfToFloat;
}

int wmain(int argc, wchar_t** argv) {
  try {
    Check(argc == 5, "usage: look_shadow_tests <source-root> <fixture.json> <result.json> warp|nvidia");
    const std::wstring adapter = argv[4];
    Check(adapter == L"warp" || adapter == L"nvidia", "explicit supported adapter required");
    std::ifstream input{std::filesystem::path(argv[2])};
    Check(input.good(), "fixture open");
    const auto fixture = json::parse(input);
    const auto& samples = fixture.at("samples");
    Check(samples.is_array() && !samples.empty() && samples.size() <= 16384, "bounded fixture width");
    const auto width = static_cast<UINT>(samples.size());
    const auto shader_path = std::filesystem::path(argv[1]) / L"shaders/look_compose.cs_5_1.hlsl";
    ComPtr<ID3DBlob> shader;
    ComPtr<ID3DBlob> errors;
    const auto compile = D3DCompileFromFile(shader_path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                           "main", "cs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors);
    if (errors) std::cerr.write(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
    Check(SUCCEEDED(compile), "current production look shader compilation");

    d3d12_test::Gpu gpu(adapter == L"nvidia");
    ComPtr<IDXGIAdapter1> actual_adapter;
    Check(SUCCEEDED(gpu.factory->EnumAdapterByLuid(gpu.device->GetAdapterLuid(), IID_PPV_ARGS(&actual_adapter))),
          "actual device adapter");
    DXGI_ADAPTER_DESC1 adapter_desc{};
    Check(SUCCEEDED(actual_adapter->GetDesc1(&adapter_desc)), "adapter description");
    LARGE_INTEGER driver{};
    Check(SUCCEEDED(actual_adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver)), "driver identity");
    ComPtr<ID3DBlob> signature_blob;
    Check(SUCCEEDED(look::SerializeRootSignature(D3D12SerializeRootSignature, &signature_blob, &errors)), "look signature");
    ComPtr<ID3D12RootSignature> signature;
    Check(SUCCEEDED(gpu.device->CreateRootSignature(0, signature_blob->GetBufferPointer(), signature_blob->GetBufferSize(),
                                                  IID_PPV_ARGS(&signature))), "look root signature");
    const D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{
        .pRootSignature = signature.Get(), .CS = {shader->GetBufferPointer(), shader->GetBufferSize()}};
    ComPtr<ID3D12PipelineState> program;
    Check(SUCCEEDED(gpu.device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&program))), "look pipeline");

    std::array<std::vector<uint16_t>, 2> pixels;
    for (size_t source = 0; source < pixels.size(); ++source) {
      for (const auto& sample : samples) {
        const auto& rgb = sample.at(source == 0 ? "proxy" : "neural");
        Check(rgb.is_array() && rgb.size() == 3, "fixture RGB shape");
        for (const auto& channel : rgb) {
          const float value = channel.get<float>();
          Check(std::isfinite(value) && value >= 0.f && value <= 1.f, "finite encoded fixture RGB");
          pixels[source].push_back(XMConvertFloatToHalf(value));
        }
        pixels[source].push_back(XMConvertFloatToHalf(1.f));
      }
    }
    auto proxy = gpu.Texture(width, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST);
    auto neural = gpu.Texture(width, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST);
    auto proxy_transfer = gpu.CreateTextureTransfer(proxy.Get());
    auto neural_transfer = gpu.CreateTextureTransfer(neural.Get());
    gpu.UploadTexture(proxy.Get(), proxy_transfer, reinterpret_cast<const uint8_t*>(pixels[0].data()), width * 8, width * 8);
    gpu.UploadTexture(neural.Get(), neural_transfer, reinterpret_cast<const uint8_t*>(pixels[1].data()), width * 8, width * 8);
    gpu.Transition(proxy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    gpu.Transition(neural.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto output_desc = proxy->GetDesc();
    output_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const D3D12_HEAP_PROPERTIES default_heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    ComPtr<ID3D12Resource> output;
    Check(SUCCEEDED(gpu.device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &output_desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&output))), "look output");
    auto transfer = gpu.CreateTextureTransfer(output.Get());
    D3D12_RESOURCE_DESC history_desc{.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Width = 256,
        .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1, .SampleDesc = {1, 0},
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR, .Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    ComPtr<ID3D12Resource> history;
    Check(SUCCEEDED(gpu.device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &history_desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&history))), "unused history binding");

    const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = 14, .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    ComPtr<ID3D12DescriptorHeap> heap;
    Check(SUCCEEDED(gpu.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap))), "look descriptors");
    const auto stride = gpu.device->GetDescriptorHandleIncrementSize(heap_desc.Type);
    const auto cpu_start = heap->GetCPUDescriptorHandleForHeapStart();
    const auto gpu_start = heap->GetGPUDescriptorHandleForHeapStart();
    const auto cpu_at = [&](UINT slot) { return D3D12_CPU_DESCRIPTOR_HANDLE{cpu_start.ptr + slot * stride}; };
    const auto gpu_at = [&](UINT slot) { return D3D12_GPU_DESCRIPTOR_HANDLE{gpu_start.ptr + slot * stride}; };
    const D3D12_SHADER_RESOURCE_VIEW_DESC srv{.Format = output_desc.Format, .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, .Texture2D = {.MipLevels = 1}};
    for (UINT slot = 0; slot < 4; ++slot) {
      gpu.device->CreateShaderResourceView(slot == 2 ? neural.Get() : proxy.Get(), &srv, cpu_at(slot));
    }
    gpu.device->CreateShaderResourceView(nullptr, &srv, cpu_at(13));
    for (UINT slot = 0; slot < look::kUavCount; ++slot) {
      D3D12_UNORDERED_ACCESS_VIEW_DESC uav{.Format = output_desc.Format, .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D};
      if (slot == look::kUavBandA || slot == look::kUavBandB || slot == look::kUavTraceBlocks) {
        uav.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
      } else if (slot == look::kUavBandMaxA || slot == look::kUavBandMaxB) {
        uav.Format = DXGI_FORMAT_R32G32_FLOAT;
      } else if (slot == look::kUavTraceHistogram) {
        uav.Format = DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer = {.NumElements = 64, .Flags = D3D12_BUFFER_UAV_FLAG_RAW};
      }
      gpu.device->CreateUnorderedAccessView(slot == 0 ? output.Get() : nullptr, nullptr, &uav, cpu_at(4 + slot));
    }
    const look::Pipeline pipeline{.root_signature = signature.Get(), .compose = program.Get()};
    const look::Bindings bindings{.heap = heap.Get(), .codec_set = gpu_at(0), .look_set = gpu_at(0),
        .uav_table = gpu_at(4), .motion = gpu_at(13), .history_in = history->GetGPUVirtualAddress(),
        .history_out = history->GetGPUVirtualAddress()};
    const look::Frame frame{.nr_width = width, .nr_height = 1, .full_width = width, .full_height = 1};
    json rows = json::array();
    for (float shadows : {1.f, 0.5f, 0.f}) {
      for (auto* resource : std::array<IUnknown*, 7>{proxy.Get(), neural.Get(), output.Get(), history.Get(),
                                                   heap.Get(), signature.Get(), program.Get()}) {
        gpu.Retain(resource);
      }
      const look::Settings settings{.shadows = shadows};
      const auto constants = look::MakeConstants(settings, frame);
      Check(constants.flags == 0, "bounded basic-gains experiment without temporal/bands/transport");
      // Production bypasses neutral gains at 1:1. Copy the network input for that baseline.
      const bool dispatched = look::Dispatched(settings, false);
      if (dispatched) {
        look::RecordCompose(gpu.list.Get(), pipeline, bindings, constants);
        gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.RecordTextureReadback(output.Get(), transfer);
        gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      } else {
        gpu.Transition(neural.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.RecordTextureReadback(neural.Get(), transfer);
        gpu.Transition(neural.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      }
      gpu.Submit();
      Check(gpu.fence->GetCompletedValue() >= gpu.serial, "readback completion");
      const auto bytes = gpu.ReadTexture(transfer);
      std::vector<uint16_t> words(bytes.size() / sizeof(uint16_t));
      std::memcpy(words.data(), bytes.data(), bytes.size());
      for (size_t index = 0; index < samples.size(); ++index) {
        std::array<float, 3> rgb{};
        for (size_t channel = 0; channel < rgb.size(); ++channel) {
          rgb[channel] = XMConvertHalfToFloat(words[index * 4 + channel]);
          Check(std::isfinite(rgb[channel]), "finite shader output");
        }
        rows.push_back({{"id", samples[index].at("id")}, {"shadows", shadows}, {"output", rgb},
                        {"dispatched", dispatched}, {"completed_fence", gpu.serial}});
      }
    }
    const json result{{"backend", adapter == L"nvidia" ? "NATIVE_GPU" : "WARP"}, {"format", "R16G16B16A16_FLOAT"},
        {"vendor_id", adapter_desc.VendorId}, {"device_id", adapter_desc.DeviceId},
        {"adapter_luid", {adapter_desc.AdapterLuid.LowPart, adapter_desc.AdapterLuid.HighPart}},
        {"driver_version", {HIWORD(driver.HighPart), LOWORD(driver.HighPart), HIWORD(driver.LowPart), LOWORD(driver.LowPart)}},
        {"completed_fence", gpu.fence->GetCompletedValue()}, {"ngx", "not_run"}, {"rows", rows}};
    std::ofstream report{std::filesystem::path(argv[3])};
    report << result.dump(2) << '\n';
    Check(report.good(), "result write");
    std::cout << samples.size() << " samples x 3 shadow settings; completed production Look output\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
