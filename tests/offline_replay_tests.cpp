#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "d3d12_test_gpu.hpp"

using d3d12_test::Check;
using d3d12_test::Gpu;
using Microsoft::WRL::ComPtr;
#pragma comment(linker, "/ENTRY:wmainCRTStartup")

int wmain(int argc, wchar_t** argv) {
  using Json = nlohmann::json;
  namespace fs = std::filesystem;
  fs::path sequence;
  fs::path output;
  std::string selected_adapter;
  bool owns_output = false;
  int exit_code = 0;
  Json result{{"schema", "nr-nexus-offline-replay-backend/1"}, {"status", "failed"},
              {"adapter", nullptr}, {"evidence", nullptr},
              {"not_evaluated", Json::array({"NATIVE_NGX", "IN_GAME", "VISUAL"})},
              {"environment", {{"ngx_loaded", false}, {"network_evaluated", false}}},
              {"history_rebuilt", false}, {"input_sha_validation", "external_runner_required"},
              {"frames", Json::array()}};
  const auto write_new = [](const fs::path& path, const void* data, size_t bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "exclusive output creation failed");
    bool written = true;
    const auto* cursor = static_cast<const uint8_t*>(data);
    while (bytes != 0 && written) {
      const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes, MAXDWORD));
      DWORD count = 0;
      written = WriteFile(file, cursor, chunk, &count, nullptr) != FALSE && count == chunk;
      if (written) {
        cursor += count;
        bytes -= count;
      }
    }
    const bool closed = CloseHandle(file) != FALSE;
    Check(written && closed, "output write failed");
  };
  try {
    std::set<std::wstring> options;
    for (int i = 1; i < argc; ++i) {
      const std::wstring option = argv[i];
      Check(options.insert(option).second && i + 1 < argc, "duplicate option or missing option value");
      const std::wstring value = argv[++i];
      if (option == L"--sequence") {
        sequence = value;
      } else if (option == L"--output") {
        output = value;
      } else if (option == L"--adapter") {
        Check(value == L"warp" || value == L"nvidia", "adapter must be warp or nvidia");
        selected_adapter = (value == L"warp" ? "warp" : "nvidia");
      } else {
        throw std::runtime_error("unknown CLI option");
      }
    }
    Check(!sequence.empty() && !output.empty() && !selected_adapter.empty(), "--sequence --output --adapter are required");
    result["adapter"] = selected_adapter;
    sequence = fs::canonical(sequence);
    output = fs::weakly_canonical(fs::absolute(output));
    const auto relative_output = output.lexically_relative(sequence);
    Check(output != sequence && (relative_output.empty() || *relative_output.begin() == L".."),
          "output must be outside sequence directory");
    if (fs::exists(output)) {
      Check(fs::is_directory(output) && fs::is_empty(output), "refusing existing nonempty output");
    } else {
      Check(fs::create_directories(output), "cannot create output directory");
    }
    owns_output = true;
    const auto manifest_path = sequence / "sequence.json";
    Check(fs::file_size(manifest_path) <= 16 * 1024 * 1024, "sequence manifest exceeds 16 MiB");
    std::ifstream manifest_input(manifest_path, std::ios::binary);
    Check(manifest_input.good(), "cannot open sequence manifest");
    Json manifest;
    manifest_input >> manifest;
    Check(manifest.is_object() && (manifest.at("schema") == "nr-nexus-offline-sequence/1"
              || manifest.at("schema") == "nr-nexus-offline-sequence/2")
              && manifest.at("byte_order") == "little", "unsupported sequence envelope");
    const bool per_frame_geometry = manifest.at("schema") == "nr-nexus-offline-sequence/2";
    const auto read_uint = [](const Json& value, uint32_t minimum, uint32_t maximum) {
      Check(value.is_number_integer() && value >= minimum && value <= maximum, "integer sequence field out of range");
      return value.get<uint32_t>();
    };
    const uint32_t width = read_uint(manifest.at("width"), 1, 8192);
    const uint32_t height = read_uint(manifest.at("height"), 1, 8192);
    Check(manifest.at("frames").is_array() && !manifest.at("frames").empty()
              && manifest.at("frames").size() <= 4096, "sequence requires 1..4096 frames");
    uint64_t total_bytes = 0;
    for (const auto& item : manifest.at("frames")) {
      const auto frame_width = per_frame_geometry ? read_uint(item.at("width"), 1, 8192) : width;
      const auto frame_height = per_frame_geometry ? read_uint(item.at("height"), 1, 8192) : height;
      Check(frame_width <= 8192 && frame_height <= 8192, "frame geometry out of range");
      total_bytes += static_cast<uint64_t>(frame_width) * frame_height * 28;
      Check(total_bytes <= 256 * 1024 * 1024, "sequence exceeds 256 MiB");
    }
    Check(manifest.at("sequence_sha256").is_string(), "sequence identity required");
    result["sequence_sha256"] = manifest.at("sequence_sha256");
    result["width"] = width;
    result["height"] = height;
    ComPtr<ID3D12Debug> debug;
    const HRESULT debug_status = D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
    if (SUCCEEDED(debug_status)) { debug->EnableDebugLayer(); }
    result["environment"]["debug_layer_enabled"] = debug != nullptr;
    result["environment"]["debug_layer_hresult"] = static_cast<int64_t>(debug_status);
    Gpu gpu(selected_adapter == "nvidia");
    bool ngx_loaded = false;
    for (const wchar_t* module : {L"nvngx.dll", L"_nvngx.dll", L"nvngx_dlss.dll", L"nvngx_dlssd.dll", L"nvngx_dlssnr.dll"}) {
      ngx_loaded = ngx_loaded || GetModuleHandleW(module) != nullptr;
    }
    result["environment"]["ngx_loaded"] = ngx_loaded;
    Check(!ngx_loaded, "NGX module unexpectedly loaded in replay process");
    ComPtr<IDXGIAdapter1> actual_adapter;
    const LUID luid = gpu.device->GetAdapterLuid();
    Check(SUCCEEDED(gpu.factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&actual_adapter))), "actual adapter lookup");
    DXGI_ADAPTER_DESC1 description{};
    Check(SUCCEEDED(actual_adapter->GetDesc1(&description)), "actual adapter description");
    const bool software = (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    if (selected_adapter == "nvidia") {
      Check(description.VendorId == 0x10de && !software, "actual NVIDIA hardware identity");
    } else {
      ComPtr<IDXGIAdapter1> warp;
      DXGI_ADAPTER_DESC1 warp_description{};
      Check(SUCCEEDED(gpu.factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) && SUCCEEDED(warp->GetDesc1(&warp_description))
                && warp_description.AdapterLuid.HighPart == luid.HighPart && warp_description.AdapterLuid.LowPart == luid.LowPart,
            "actual WARP adapter identity");
    }
    const int description_bytes = WideCharToMultiByte(CP_UTF8, 0, description.Description, -1, nullptr, 0, nullptr, nullptr);
    Check(description_bytes > 0, "adapter description UTF-8 size");
    std::string description_text(description_bytes, '\0');
    Check(WideCharToMultiByte(CP_UTF8, 0, description.Description, -1, description_text.data(), description_bytes,
                             nullptr, nullptr) == description_bytes, "adapter description UTF-8 conversion");
    description_text.pop_back();
    LARGE_INTEGER driver{};
    const HRESULT driver_status = actual_adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver);
    result["environment"].update(Json{{"description", description_text}, {"vendor_id", description.VendorId},
        {"device_id", description.DeviceId}, {"adapter_flags", description.Flags}, {"actual_gpu", !software},
        {"software_adapter", software}, {"adapter_luid", {{"high", luid.HighPart}, {"low", luid.LowPart}}},
        {"driver", {{"source", "IDXGIAdapter::CheckInterfaceSupport(IDXGIDevice)"}, {"available", SUCCEEDED(driver_status)},
                    {"hresult", static_cast<int64_t>(driver_status)}}}});
    if (SUCCEEDED(driver_status)) {
      const auto version = static_cast<uint64_t>(driver.QuadPart);
      result["environment"]["driver"]["raw_version"] = version;
      result["environment"]["driver"]["version"] = std::to_string((version >> 48) & 0xffff) + "."
          + std::to_string((version >> 32) & 0xffff) + "." + std::to_string((version >> 16) & 0xffff) + "." + std::to_string(version & 0xffff);
    }
    ComPtr<ID3D12InfoQueue> info;
    gpu.device.As(&info);
    struct ResourceCopy {
      const char* name;
      const char* format_name;
      DXGI_FORMAT format;
      uint32_t stride;
      std::vector<uint8_t> input;
      ComPtr<ID3D12Resource> source;
      ComPtr<ID3D12Resource> destination;
      d3d12_test::TextureTransfer transfer;
    };
    for (size_t index = 0; index < manifest.at("frames").size(); ++index) {
      const auto& frame = manifest.at("frames")[index];
      const uint32_t frame_width = per_frame_geometry ? read_uint(frame.at("width"), 1, 8192) : width;
      const uint32_t frame_height = per_frame_geometry ? read_uint(frame.at("height"), 1, 8192) : height;
      result["frames"].push_back({{"index", index}, {"status", "failed"}, {"evidence", nullptr},
          {"width", frame_width}, {"height", frame_height},
          {"requested_evidence", (selected_adapter == "warp" ? "WARP" : "NATIVE_GPU")},
          {"resources", Json::object()}, {"controls", {{"evidence", "STATIC"}, {"history_evaluated", false}}},
          {"network_evaluated", false}, {"completion", {{"gpu_completed", false}, {"fence_value", nullptr}}}});
      auto& frame_result = result["frames"].back();
      Check(frame.is_object() && frame.at("index") == index && frame.at("reset").is_boolean(), "frame index/reset mismatch");
      for (const char* field : {"jitter", "motion_scale"}) {
        Check(frame.at(field).is_array() && frame.at(field).size() == 2, "two-component metadata required");
        for (const auto& value : frame.at(field)) {
          Check(value.is_number() && std::isfinite(value.get<double>()), "finite frame metadata required");
        }
        frame_result["controls"][field] = frame.at(field);
      }
      frame_result["controls"]["reset"] = frame.at("reset");
      Check(frame.at("subrect").is_array() && frame.at("subrect").size() == 4,
            "four-component sequence subrect required");
      const auto rect_x = read_uint(frame.at("subrect")[0], 0, frame_width);
      const auto rect_y = read_uint(frame.at("subrect")[1], 0, frame_height);
      const auto rect_width = read_uint(frame.at("subrect")[2], 0, frame_width);
      const auto rect_height = read_uint(frame.at("subrect")[3], 0, frame_height);
      Check(rect_x + rect_width <= frame_width && rect_y + rect_height <= frame_height,
            "sequence subrect exceeds input geometry");
      frame_result["controls"]["subrect"] = frame.at("subrect");
      Check(frame.at("resources").is_object() && frame.at("resources").size() == 3, "three sequence resources required");
      std::array<ResourceCopy, 3> copies{{
          {.name = "color", .format_name = "R32G32B32A32_FLOAT", .format = DXGI_FORMAT_R32G32B32A32_FLOAT, .stride = 16},
          {.name = "depth", .format_name = "R32_FLOAT", .format = DXGI_FORMAT_R32_FLOAT, .stride = 4},
          {.name = "motion", .format_name = "R32G32_FLOAT", .format = DXGI_FORMAT_R32G32_FLOAT, .stride = 8}}};
      for (auto& copy : copies) {
        const auto& resource = frame.at("resources").at(copy.name);
        const auto bytes = static_cast<size_t>(frame_width) * frame_height * copy.stride;
        Check(resource.at("format") == copy.format_name && resource.at("bytes").is_number_integer()
                  && resource.at("bytes") == bytes, "resource format/length mismatch");
        const auto name = resource.at("file").get<std::string>();
        Check(!name.empty() && name.find_first_of("/\\:") == std::string::npos && name != "." && name != "..",
              "resource must name a sequence-local file");
        const auto path = sequence / fs::path(std::u8string(name.begin(), name.end()));
        Check((GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) == 0, "resource reparse point rejected");
        Check(fs::is_regular_file(path) && fs::file_size(path) == bytes, "resource file size mismatch");
        copy.input.resize(bytes);
        std::ifstream input(path, std::ios::binary);
        Check(input.good() && static_cast<bool>(input.read(reinterpret_cast<char*>(copy.input.data()), bytes)), "resource read failed");
        copy.source = gpu.Texture(frame_width, frame_height, copy.format, D3D12_RESOURCE_STATE_COPY_DEST);
        copy.destination = gpu.Texture(frame_width, frame_height, copy.format, D3D12_RESOURCE_STATE_COPY_DEST);
        copy.transfer = gpu.CreateTextureTransfer(copy.source.Get());
        Check(copy.transfer.rows == frame_height && copy.transfer.row_bytes == static_cast<uint64_t>(frame_width) * copy.stride
                  && copy.transfer.footprint.Footprint.RowPitch >= copy.transfer.row_bytes,
              "copy footprint shape mismatch");
        gpu.UploadTexture(copy.source.Get(), copy.transfer, copy.input.data(), copy.input.size(), copy.transfer.row_bytes);
        gpu.Transition(copy.source.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.list->CopyResource(copy.destination.Get(), copy.source.Get());
        gpu.Transition(copy.destination.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.RecordTextureReadback(copy.destination.Get(), copy.transfer);
        frame_result["resources"][copy.name] = {{"output_file", nullptr}, {"format", copy.format_name}, {"bytes", bytes},
            {"bitwise_equal", false}, {"row_pitch", copy.transfer.footprint.Footprint.RowPitch},
            {"row_bytes", copy.transfer.row_bytes}, {"footprint_bytes", copy.transfer.allocation_bytes}};
      }
      gpu.Submit();
      const auto completed = gpu.fence->GetCompletedValue();
      Check(completed != UINT64_MAX && completed >= gpu.serial && SUCCEEDED(gpu.device->GetDeviceRemovedReason()),
            "GPU copy completion not proven");
      frame_result["completion"] = {{"gpu_completed", true}, {"fence_value", completed}, {"submitted_fence_value", gpu.serial},
          {"source", "Gpu::Submit + ID3D12Fence::GetCompletedValue"}};
      frame_result["evidence"] = (selected_adapter == "warp" ? "WARP" : "NATIVE_GPU");
      if (info != nullptr) {
        for (UINT64 message_index = 0; message_index < info->GetNumStoredMessages(); ++message_index) {
          SIZE_T bytes = 0;
          Check(SUCCEEDED(info->GetMessage(message_index, nullptr, &bytes)), "debug message size");
          std::vector<uint8_t> message_buffer(bytes);
          auto* message = reinterpret_cast<D3D12_MESSAGE*>(message_buffer.data());
          Check(SUCCEEDED(info->GetMessage(message_index, message, &bytes)), "debug message read");
          Check(message->Severity > D3D12_MESSAGE_SEVERITY_ERROR, "D3D12 debug layer error");
        }
        info->ClearStoredMessages();
      }
      for (auto& copy : copies) {
        auto pixels = gpu.ReadTexture(copy.transfer);
        const auto filename = std::to_string(index) + "-" + copy.name + ".f32";
        write_new(output / filename, pixels.data(), pixels.size());
        auto& resource_result = frame_result["resources"][copy.name];
        resource_result["output_file"] = filename;
        resource_result["bitwise_equal"] = (pixels == copy.input);
        Check(pixels == copy.input, "GPU copy readback differs from uploaded input");
      }
      frame_result["status"] = "passed";
      result["evidence"] = (selected_adapter == "warp" ? "WARP" : "NATIVE_GPU");
    }
    result["status"] = "passed";
  } catch (const std::exception& error) {
    // Filesystem diagnostics can disclose private paths; keep their category only.
    result["error"] = (dynamic_cast<const fs::filesystem_error*>(&error) != nullptr ? "filesystem_error" : error.what());
    exit_code = 1;
  }
  try {
    if (owns_output) {
      const auto serialized = result.dump(2) + "\n";
      write_new(output / "replay-backend-result.json", serialized.data(), serialized.size());
    }
  } catch (const std::exception&) {
    std::cerr << "Replay result write failed\n";
    return 1;
  }
  std::cout << result.dump() << '\n';
  return exit_code;
}
