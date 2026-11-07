// The native harness has a nested main macro, so select the Unicode CRT entry
// explicitly while keeping its implementation available unchanged.
#include "fg_native_tests.cpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

#pragma comment(linker, "/ENTRY:wmainCRTStartup")

int wmain(int argc, wchar_t** argv) {
  using Json = nlohmann::json;
  namespace fs = std::filesystem;
  namespace addon = renodx::addons::dlss5::internal;
  fs::path fixture_path;
  fs::path output;
  std::string selected_adapter;
  bool contracts = false;
  bool benchmark = false;
  bool owns_output = false;
  int exit_code = 0;
  Json fixture;
  Json result{{"schema", "nr-nexus-offline-backend/1"},
              {"backend", "d3d12-control-mask"},
              {"execution_mode", "validation"},
              {"reference_policy", "d3d-float-to-unorm-0.6-ulp"},
              {"environment", Json::object()}, {"cases", Json::array()}};
  // D3D11 functional spec section 3.2.3.6, applied through the D3D12 SDK tolerance:
  // https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#FLOATtoUNORM
  // https://github.com/microsoft/DirectX-Headers/blob/main/include/directx/d3d12.idl
  const auto unorm_conforms = [](double ideal, uint8_t actual) {
    if (ideal == 0.0 || ideal == 255.0) return static_cast<double>(actual) == ideal;
    return std::abs(static_cast<double>(actual) - ideal)
        <= static_cast<double>(D3D12_FLOAT32_TO_INTEGER_TOLERANCE_IN_ULP);
  };
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
      Check(options.insert(option).second, "duplicate CLI option");
      if (option == L"--contracts") {
        contracts = true;
        continue;
      }
      if (option == L"--benchmark") {
        benchmark = true;
        continue;
      }
      Check(i + 1 < argc, "CLI option requires a value");
      const std::wstring value = argv[++i];
      if (option == L"--fixture") {
        fixture_path = value;
      } else if (option == L"--output") {
        output = value;
      } else if (option == L"--adapter") {
        Check(value == L"warp" || value == L"nvidia", "adapter must be warp or nvidia");
        selected_adapter = (value == L"warp" ? "warp" : "nvidia");
      } else {
        throw std::runtime_error("unknown CLI option");
      }
    }
    Check(!output.empty() && !selected_adapter.empty(), "--output and --adapter are required");
    Check(contracts != !fixture_path.empty(), "select either --fixture or --contracts");
    Check(!contracts || !benchmark, "--benchmark requires a fixture");
    Check(!contracts || selected_adapter == "warp", "--contracts reuses the WARP harness; select --adapter warp");
    result["adapter"] = selected_adapter;
    result["execution_mode"] = benchmark ? "performance" : "validation";
    output = fs::absolute(output);
    if (fs::exists(output)) {
      Check(fs::is_directory(output) && fs::is_empty(output), "refusing existing nonempty output");
    } else {
      Check(fs::create_directories(output), "cannot create output directory");
    }
    owns_output = true;

    if (!contracts) {
      std::ifstream input(fixture_path, std::ios::binary);
      Check(input.good(), "cannot open fixture");
      input >> fixture;
      Check(fixture.is_object() && fixture.at("schema") == "nr-nexus-offline-fixture/1"
                && fixture.at("fixture_id").is_string() && fixture.at("cases").is_array()
                && !fixture.at("cases").empty(), "invalid fixture envelope");
      result["fixture_id"] = fixture.at("fixture_id");
    }
    Check(unorm_conforms(127.5, 127) && unorm_conforms(127.5, 128)
              && !unorm_conforms(127.5, 126) && !unorm_conforms(127.5, 129)
              && unorm_conforms(63.75, 64) && !unorm_conforms(63.75, 63)
              && unorm_conforms(0.0, 0) && !unorm_conforms(0.0, 1)
              && unorm_conforms(255.0, 255) && !unorm_conforms(255.0, 254),
          "FLOAT-to-UNORM oracle boundary contracts");
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    ComPtr<ID3D12Debug> debug;
    const HRESULT debug_status = D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
    if (SUCCEEDED(debug_status)) {
      debug->EnableDebugLayer();
    }
    result["environment"]["debug_layer_available"] = debug != nullptr;
    result["environment"]["debug_layer_enabled"] = debug != nullptr;
    result["environment"]["debug_layer_hresult"] = static_cast<int64_t>(debug_status);
    if (contracts) {
      // Run enables the debug layer before creating its own WARP device.
      exit_code = control_mask_gpu_cases::Run();
      result["contracts"] = {{"status", (exit_code == 0 ? "passed" : "failed")},
                              {"evidence", Json::array({"MOCK", "WARP"})},
                              {"entry", "control_mask_gpu_cases::Run"},
                              {"network_evaluated", false}};
    } else {
      const auto device_init_start = std::chrono::steady_clock::now();
      Gpu gpu(selected_adapter == "nvidia");
      result["cpu_timing"] = {{"clock", "std::chrono::steady_clock"},
          {"device_queue_init_ms", std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - device_init_start).count()}};
      bool ngx_loaded = false;
      for (const auto* module : {L"nvngx.dll", L"_nvngx.dll", L"nvngx_dlssnr.dll"}) {
        ngx_loaded = ngx_loaded || GetModuleHandleW(module) != nullptr;
      }
      Check(!ngx_loaded, "shader-only backend must not have an NGX Runtime loaded");
      ComPtr<IDXGIAdapter1> actual_adapter;
      const LUID luid = gpu.device->GetAdapterLuid();
      Check(SUCCEEDED(gpu.factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&actual_adapter))), "actual device adapter lookup");
      DXGI_ADAPTER_DESC1 adapter_desc{};
      Check(SUCCEEDED(actual_adapter->GetDesc1(&adapter_desc)), "actual adapter description");
      Check(selected_adapter != "nvidia" || (adapter_desc.VendorId == 0x10DE
                && (adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0), "actual NVIDIA adapter identity");
      if (selected_adapter == "warp") {
        ComPtr<IDXGIAdapter1> warp;
        Check(SUCCEEDED(gpu.factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))), "WARP identity lookup");
        DXGI_ADAPTER_DESC1 warp_desc{};
        Check(SUCCEEDED(warp->GetDesc1(&warp_desc)) && warp_desc.AdapterLuid.HighPart == luid.HighPart
                  && warp_desc.AdapterLuid.LowPart == luid.LowPart, "actual WARP adapter identity");
      }
      const int description_bytes = WideCharToMultiByte(CP_UTF8, 0, adapter_desc.Description, -1, nullptr, 0, nullptr, nullptr);
      Check(description_bytes > 0, "adapter description UTF-8 size");
      std::string description(description_bytes, '\0');
      Check(WideCharToMultiByte(CP_UTF8, 0, adapter_desc.Description, -1, description.data(),
                description_bytes, nullptr, nullptr) == description_bytes, "adapter description UTF-8 conversion");
      description.pop_back();
      LARGE_INTEGER driver{};
      const HRESULT driver_status = actual_adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver);
      result["environment"].update(Json{
          {"description", description}, {"vendor_id", adapter_desc.VendorId},
          {"device_id", adapter_desc.DeviceId}, {"subsystem_id", adapter_desc.SubSysId},
          {"revision", adapter_desc.Revision}, {"adapter_flags", adapter_desc.Flags},
          {"adapter_luid", {{"high", luid.HighPart}, {"low", luid.LowPart}}},
          {"driver", {{"source", "IDXGIAdapter::CheckInterfaceSupport(IDXGIDevice)"},
                       {"available", SUCCEEDED(driver_status)}, {"hresult", static_cast<int64_t>(driver_status)}}},
          {"ngx_loaded", ngx_loaded}, {"network_evaluated", false}});
      if (SUCCEEDED(driver_status)) {
        const uint64_t version = static_cast<uint64_t>(driver.QuadPart);
        result["environment"]["driver"]["raw_version"] = version;
        result["environment"]["driver"]["version"] =
            std::to_string((version >> 48) & 0xffff) + "." + std::to_string((version >> 32) & 0xffff)
            + "." + std::to_string((version >> 16) & 0xffff) + "." + std::to_string(version & 0xffff);
      }
      Check(addon::InstallQueueCompletionHooks(gpu.queue.Get()), "production queue completion hooks");
      Check(addon::InstallCommandListStateHooks(gpu.list.Get()), "production list state hooks");
      const auto pipeline_init_start = std::chrono::steady_clock::now();
      Check(addon::EnsureControlMaskPipeline(gpu.device.Get()), "production ControlMask pipeline");
      result["cpu_timing"]["pipeline_init_ms"] = std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - pipeline_init_start).count();
      const auto frequency = d3d12_test::TimestampFrequency(gpu.queue.Get());
      result["environment"]["timestamp_frequency_hz"] = frequency;
      result["environment"]["timestamp_frequency_scope"] = "D3D12 command-queue timestamp ticks per second; not GPU core clock";
      const auto resources_init_start = std::chrono::steady_clock::now();
      ComPtr<ID3D12InfoQueue> info;
      gpu.device.As(&info);
      result["environment"]["debug_message_queue_available"] = info != nullptr;
      std::set<std::string> ids;
      bool stop_gpu = false;
      for (const auto& item : fixture.at("cases")) {
        Json case_result{{"id", nullptr}, {"width", nullptr}, {"height", nullptr}, {"status", "failed"},
                         {"reference_policy", "d3d-float-to-unorm-0.6-ulp"},
                         {"exact_reference_differences", nullptr}, {"max_quantization_error_ulp", nullptr},
                         {"evidence", (selected_adapter == "warp" ? "WARP" : "NATIVE_GPU")},
                         {"pixels_file", nullptr}, {"format", "R8G8B8A8_UNORM"},
                         {"pixel_checks", 0}, {"pixel_checks_unit", "bytes_across_all_frames"},
                         {"timing", {{"samples", Json::array()}, {"frequency_hz", frequency},
                                      {"scope", "control-mask-dispatch"},
                                      {"includes_uav_barrier", true},
                                      {"source", "D3D12_QUERY_TYPE_TIMESTAMP"}}},
                         {"parameter_contract", {{"evidence", "MOCK"}, {"status", "failed"},
                                                  {"network_evaluated", false}}},
                         {"completion", {{"completed", false}, {"gpu_completed", false},
                                          {"fence_value", nullptr}}}};
        try {
          Check(item.is_object() && item.at("id").is_string(), "case id must be a string");
          case_result["id"] = item.at("id");
          case_result["width"] = item.value("width", Json());
          case_result["height"] = item.value("height", Json());
          Check(!stop_gpu, "not run after previous GPU/case failure");
          const std::string id = item.at("id");
          Check(!id.empty() && id.size() <= 128 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
          }), "case id must be an ASCII file-safe identifier");
          std::string folded_id = id;
          std::transform(folded_id.begin(), folded_id.end(), folded_id.begin(), [](unsigned char c) {
            return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
          });
          Check(ids.insert(folded_id).second, "duplicate case id (case insensitive)");
          Check(folded_id != "con" && folded_id != "prn" && folded_id != "aux" && folded_id != "nul"
                    && !(folded_id.size() == 4 && (folded_id.starts_with("com") || folded_id.starts_with("lpt"))
                         && folded_id[3] >= '1' && folded_id[3] <= '9'), "reserved Windows case id");
          const auto read_uint = [](const Json& value, uint32_t minimum, uint32_t maximum) {
            Check(value.is_number_integer(), "integer fixture field required");
            Check(value >= minimum && value <= maximum, "integer fixture field out of range");
            return value.get<uint32_t>();
          };
          const uint32_t width = read_uint(item.at("width"), 1, D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
          const uint32_t height = read_uint(item.at("height"), 1, D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
          case_result["width"] = width;
          case_result["height"] = height;
          const uint32_t frames = read_uint(item.at("frames"), 1, 4096);
          const uint32_t warmup = read_uint(item.at("warmup"), 0, frames - 1);
          Check(item.at("auto_mask").is_boolean(), "auto_mask must be boolean");
          const bool auto_mask = item.at("auto_mask");
          const uint32_t blend = read_uint(item.at("blend_mode"), 0, 3);
          const std::string model = item.at("model").get<std::string>();
          Check(model == "A" || model == "B", "model must be A or B");
          const std::string mode_name = item.at("mode").get<std::string>();
          const std::map<std::string, uint32_t> modes{{"rgb", addon::kControlMaskRgb},
              {"neutral", addon::kControlMaskNeutral}, {"rect", addon::kControlMaskRect},
              {"green_rect", addon::kControlMaskGreenRect}, {"blue_rect", addon::kControlMaskBlueRect}};
          Check(modes.contains(mode_name), "unknown control mask mode");
          const uint32_t mode = modes.at(mode_name);
          Check(item.at("rect").is_array() && item.at("rect").size() == 2
                    && item.at("rgb").is_array() && item.at("rgb").size() == 3, "rect/rgb arrays required");
          const uint32_t rect_width = read_uint(item.at("rect")[0], 0, UINT32_MAX);
          const uint32_t rect_height = read_uint(item.at("rect")[1], 0, UINT32_MAX);
          std::array<float, 3> rgb{};
          std::array<double, 4> base_ideal{0.0, 0.0, 0.0, 255.0};
          for (size_t channel = 0; channel < rgb.size(); ++channel) {
            const auto& value = item.at("rgb")[channel];
            Check(value.is_number() && value >= 0 && value <= 1, "RGB must be finite numbers in [0,1]");
            rgb[channel] = value.get<float>();
            // The oracle starts from the exact float32 sent to the shader, then
            // computes its ideal continuous integer-side value without another float32 rounding.
            base_ideal[channel] = static_cast<double>(rgb[channel]) * 255.0;
          }
          const auto case_start = std::chrono::steady_clock::now();
          const auto resource_setup_start = case_start;
          const D3D12_HEAP_PROPERTIES properties{.Type = D3D12_HEAP_TYPE_DEFAULT};
          const D3D12_RESOURCE_DESC desc{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
              .Width = width, .Height = height, .DepthOrArraySize = 1, .MipLevels = 1,
              .Format = DXGI_FORMAT_R8G8B8A8_UNORM, .SampleDesc = {1, 0},
              .Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
          ComPtr<ID3D12Resource> texture;
          Check(SUCCEEDED(gpu.device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&texture))), "case RGBA8 texture");
          const auto texture_allocation = gpu.device->GetResourceAllocationInfo(0, 1, &desc);
          const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
              .NumDescriptors = addon::kDescriptorHeapSize, .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
          ComPtr<ID3D12DescriptorHeap> heap;
          Check(SUCCEEDED(gpu.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap))), "case descriptor heap");
          D3D12_SHADER_RESOURCE_VIEW_DESC null_depth_srv{.Format = DXGI_FORMAT_R32_FLOAT,
              .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
              .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
          null_depth_srv.Texture2D.MipLevels = 1;
          gpu.device->CreateShaderResourceView(nullptr, &null_depth_srv,
              addon::DescriptorHeapSlot(heap->GetCPUDescriptorHandleForHeapStart(),
                  addon::kDescriptorControlMaskDepthNullSrv,
                  gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)));
          const D3D12_UNORDERED_ACCESS_VIEW_DESC view{.Format = DXGI_FORMAT_R8G8B8A8_UNORM,
              .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D};
          auto descriptor = heap->GetCPUDescriptorHandleForHeapStart();
          descriptor.ptr += static_cast<SIZE_T>(addon::kDescriptorControlMaskUav)
              * gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
          gpu.device->CreateUnorderedAccessView(texture.Get(), nullptr, &view, descriptor);

          renodx::addons::dlss5::bridge::OwnedParameters parameters;
          addon::FeatureState feature{};
          addon::NrFeatureSlot slot{};
          addon::FinalResources resources{};
          addon::EvaluationContract frame{};
          slot.parameters = &parameters;
          resources.control_mask = texture.Get();
          const uint32_t preset = (model == "A" ? 1u : 2u);
          addon::SetNrCreateParameters(&parameters, width, height, width, height, 0, false, preset);
          Check(addon::GetUInt(&parameters, "DLSSNR.Hint.Render.Preset") == preset
                    && addon::GetUInt(&parameters, NVSDK_NGX_Parameter_Width) == width
                    && addon::GetUInt(&parameters, NVSDK_NGX_Parameter_Height) == height
                    && addon::GetUInt(&parameters, NVSDK_NGX_Parameter_OutWidth) == width
                    && addon::GetUInt(&parameters, NVSDK_NGX_Parameter_OutHeight) == height, "MOCK creation request contract");
          const bool previous_auto_mask = addon::use_auto_mask.exchange(auto_mask);
          addon::SetEvaluationParameters(feature, slot, 0, 0, resources, frame, width, height, width, height, true, blend);
          addon::use_auto_mask.store(previous_auto_mask);
          Check(addon::GetD3D12Resource(&parameters, "DLSSNR.ControlMask") == texture.Get()
                    && addon::GetUInt(&parameters, "DLSSNR.ControlMaskSubrectBaseX", UINT32_MAX) == 0
                    && addon::GetUInt(&parameters, "DLSSNR.ControlMaskSubrectBaseY", UINT32_MAX) == 0
                    && addon::GetUInt(&parameters, "DLSSNR.ControlMaskSubrectWidth") == width
                    && addon::GetUInt(&parameters, "DLSSNR.ControlMaskSubrectHeight") == height
                    && addon::GetUInt(&parameters, "DLSSNR.MaskBlendMode", UINT32_MAX) == blend
                    && addon::GetInt(&parameters, "DLSSNR.UseAutoMask", -1) == static_cast<int>(auto_mask),
                "MOCK evaluation parameter contract");
          addon::ClearControlMaskBinding(slot);
          Check(addon::GetD3D12Resource(&parameters, "DLSSNR.ControlMask") == nullptr && !slot.control_mask_bound,
                "MOCK parameter binding cleared");
          case_result["parameter_contract"].update(Json{{"status", "passed"}, {"model", model},
              {"model_request", model},
              {"requested_preset", preset}, {"auto_mask", auto_mask}, {"blend_mode", blend},
              {"mask_bound", true}, {"binding_cleared", true},
              {"scope", "SetNrCreateParameters + SetEvaluationParameters on OwnedParameters; no NGX create/evaluate"}});

          const auto timestamps = d3d12_test::CreateTimestampQueries(gpu, frames);
          case_result["timing"]["warmup_frames"] = warmup;
          case_result["timing"]["total_frames"] = frames;
          case_result["timing"]["execution_mode"] = benchmark ? "batched" : "per-frame-readback";
          case_result["timing"]["validation_scope"] = benchmark ? "final-frame" : "all-frames";
          case_result["allocation"] = {{"texture_bytes_estimate", texture_allocation.SizeInBytes},
              {"texture_alignment", texture_allocation.Alignment},
              {"descriptor_count", addon::kDescriptorHeapSize},
              {"descriptor_increment_bytes", gpu.device->GetDescriptorHandleIncrementSize(
                  D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)},
              {"timestamp_query_count", 2 * timestamps.interval_count},
              {"timestamp_readback_bytes", timestamps.readback_bytes}};
          case_result["cpu_timing"] = {{"resource_setup_ms", nullptr}, {"command_recording_ms", 0.0},
              {"case_wall_ms", nullptr}, {"clock", "std::chrono::steady_clock"}};
          case_result["cpu_timing"]["resource_setup_ms"] = std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - resource_setup_start).count();
          uint64_t checks = 0;
          uint64_t exact_differences = 0;
          double max_quantization_error = 0.0;
          bool pixels_match = true;
          for (uint32_t index = 0; index < frames; ++index) {
            const auto recording_start = std::chrono::steady_clock::now();
            timestamps.Begin(gpu.list.Get(), index);
            control_mask_gpu_cases::Dispatch(gpu, texture.Get(), heap.Get(), mode, rect_width, rect_height, rgb[0], rgb[1], rgb[2]);
            timestamps.EndAndResolve(gpu.list.Get(), index);
            case_result["cpu_timing"]["command_recording_ms"] = case_result["cpu_timing"]["command_recording_ms"].get<double>()
                + std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - recording_start).count();
            std::vector<uint8_t> pixels;
            uint64_t completed_serial = 0;
            if (!benchmark || index + 1 == frames) {
              pixels = control_mask_gpu_cases::ReadPixels(gpu, texture.Get());
              completed_serial = gpu.fence->GetCompletedValue();
              Check(completed_serial != UINT64_MAX && completed_serial >= gpu.serial, "real submission fence completed");
              case_result["completion"] = {{"completed", true}, {"gpu_completed", true},
                                            {"fence_value", completed_serial}, {"readback_serial", gpu.serial},
                                            {"submitted_serial", gpu.serial},
                                            {"completed_serial", completed_serial}, {"frames", frames},
                                            {"source", "Gpu::Submit + ID3D12Fence::GetCompletedValue"}};
              if (benchmark) {
                const auto samples = timestamps.ReadAll();
                for (uint32_t sample_index = 0; sample_index < frames; ++sample_index) {
                  const auto& sample = samples[sample_index];
                  case_result["timing"]["samples"].push_back({{"frame", sample_index}, {"begin_tick", sample.begin_tick},
                      {"end_tick", sample.end_tick}, {"ticks", sample.ticks}, {"warmup", sample_index < warmup},
                      {"gpu_ms", sample.gpu_ms},
                      {"completion_serial", completed_serial}});
                }
              } else {
                const auto sample = timestamps.Read(index);
                case_result["timing"]["samples"].push_back({{"frame", index}, {"begin_tick", sample.begin_tick},
                    {"end_tick", sample.end_tick}, {"ticks", sample.ticks}, {"warmup", index < warmup},
                    {"gpu_ms", sample.gpu_ms},
                    {"completion_serial", completed_serial}});
              }
            }
            if (benchmark && index + 1 != frames) continue;
            uint64_t frame_exact_differences = 0;
            double frame_max_error = 0.0;
            bool frame_conforms = true;
            for (uint32_t y = 0; y < height; ++y) {
              for (uint32_t x = 0; x < width; ++x) {
                auto ideal = base_ideal;
                if (mode == addon::kControlMaskNeutral) {
                  ideal = {255.0, 255.0, 255.0, 255.0};
                } else if (mode == addon::kControlMaskRect && x < rect_width && y < rect_height) {
                  ideal = {0.0, 0.0, 0.0, 255.0};
                } else if (mode == addon::kControlMaskGreenRect || mode == addon::kControlMaskBlueRect) {
                  ideal = {255.0, 255.0, 255.0, 255.0};
                  if (x < width / 2 && y < height / 2) {
                    ideal[mode == addon::kControlMaskGreenRect ? 1 : 2] = 0.0;
                  }
                }
                const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
                for (size_t channel = 0; channel < ideal.size(); ++channel) {
                  ++checks;
                  const uint8_t actual = pixels[offset + channel];
                  const double lower = std::floor(ideal[channel]);
                  const double fraction = ideal[channel] - lower;
                  const auto exact_reference = static_cast<uint8_t>(lower
                      + (fraction > .5 || (fraction == .5 && static_cast<int>(lower) % 2 != 0)));
                  frame_exact_differences += actual != exact_reference;
                  const double error_ulp = std::abs(static_cast<double>(actual) - ideal[channel]);
                  frame_max_error = std::max(frame_max_error, error_ulp);
                  if (!unorm_conforms(ideal[channel], actual)) {
                    if (pixels_match) {
                      case_result["first_mismatch"] = {{"frame", index}, {"x", x}, {"y", y}, {"channel", channel},
                                                       {"exact_reference", exact_reference}, {"ideal", ideal[channel]},
                                                       {"actual", actual}, {"quantization_error_ulp", error_ulp}};
                    }
                    pixels_match = false;
                    frame_conforms = false;
                  }
                }
              }
            }
            auto& sample = case_result["timing"]["samples"].back();
            sample["exact_reference_differences"] = frame_exact_differences;
            sample["max_quantization_error_ulp"] = frame_max_error;
            sample["format_conformant"] = frame_conforms;
            sample["bitwise_identical_to_reference"] = frame_exact_differences == 0;
            exact_differences += frame_exact_differences;
            max_quantization_error = std::max(max_quantization_error, frame_max_error);
            case_result["exact_reference_differences"] = exact_differences;
            case_result["max_quantization_error_ulp"] = max_quantization_error;
            case_result["format_conformant"] = pixels_match;
            case_result["bitwise_identical_to_reference"] = exact_differences == 0;
            case_result["pixel_checks"] = checks;
            case_result["validated_frames"] = benchmark ? 1 : index + 1;
            if (index + 1 == frames) {
              const std::string pixels_file = id + ".rgba8";
              write_new(output / pixels_file, pixels.data(), pixels.size());
              case_result["pixels_file"] = pixels_file;
            }
          }
          case_result["cpu_timing"]["case_wall_ms"] = std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - case_start).count();
          Check(pixels_match, "RGBA8 FLOAT-to-UNORM format conformance failed");
          if (info != nullptr) {
            for (UINT64 index = 0; index < info->GetNumStoredMessages(); ++index) {
              SIZE_T bytes = 0;
              Check(SUCCEEDED(info->GetMessage(index, nullptr, &bytes)), "debug message size");
              std::vector<uint8_t> buffer(bytes);
              auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
              Check(SUCCEEDED(info->GetMessage(index, message, &bytes)), "debug message read");
              if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                case_result["debug_error"] = message->pDescription;
                throw std::runtime_error("D3D12 debug layer validation error");
              }
            }
            info->ClearStoredMessages();
          }
          case_result["status"] = "passed";
        } catch (const std::exception& error) {
          case_result["error"] = error.what();
          exit_code = 1;
          // A format mismatch after a completed fence is safe to diagnose alongside
          // later cases. Unfinished work or debug validation errors stop recording.
          stop_gpu = !case_result["completion"].value("completed", false) || case_result.contains("debug_error");
        }
        result["cases"].push_back(std::move(case_result));
      }
      result["cpu_timing"]["all_case_resource_and_execution_ms"] = std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - resources_init_start).count();
    }
  } catch (const std::exception& error) {
    result["error"] = error.what();
    exit_code = 1;
    // Preserve case identities even if device/pipeline setup failed before the loop.
    if (fixture.is_object() && fixture.contains("cases") && fixture["cases"].is_array()) {
      for (size_t index = result["cases"].size(); index < fixture["cases"].size(); ++index) {
        const auto& item = fixture["cases"][index];
        result["cases"].push_back({
            {"id", (item.is_object() ? item.value("id", Json()) : Json())},
            {"width", (item.is_object() ? item.value("width", Json()) : Json())},
            {"height", (item.is_object() ? item.value("height", Json()) : Json())},
            {"status", "failed"}, {"error", error.what()},
            {"reference_policy", "d3d-float-to-unorm-0.6-ulp"},
            {"evidence", (selected_adapter == "warp" ? "WARP" : "NATIVE_GPU")},
            {"pixels_file", nullptr}, {"format", "R8G8B8A8_UNORM"}, {"pixel_checks", 0},
            {"timing", {{"samples", Json::array()}, {"frequency_hz", nullptr},
                         {"scope", "control-mask-dispatch"}}},
            {"parameter_contract", {{"evidence", "MOCK"}, {"status", "failed"}, {"network_evaluated", false}}},
            {"completion", {{"completed", false}, {"gpu_completed", false}, {"fence_value", nullptr}}}});
      }
    }
  }
  result["status"] = (exit_code == 0 ? "passed" : "failed");
  try {
    if (owns_output) {
      const std::string serialized = result.dump(2) + "\n";
      write_new(output / "backend-result.json", serialized.data(), serialized.size());
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << result.dump() << '\n';
  return exit_code;
}
