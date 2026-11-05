#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <winver.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <nvsdk_ngx.h>

namespace {
using Json = nlohmann::ordered_json;
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

struct Report {
  fs::path directory;
  Json value = {{"schema", "nr-nexus-offline-ngx-probe/1"},
                {"evidence", "STATIC"},
                {"not_evaluated", Json::array({"NATIVE_GPU", "NATIVE_NGX", "IN_GAME", "VISUAL"})},
                {"status", "blocked"}, {"created", false}, {"evaluated", false}, {"gpu_completed", false},
                {"feature", {{"id", 18}, {"sdk_name", "Reserved18"}, {"public_contract", "unknown"}}},
                {"adapter", {{"evidence", "STATIC"}, {"description", nullptr}, {"vendor_id", nullptr},
                             {"device_id", nullptr}, {"adapter_flags", nullptr},
                             {"adapter_luid", {{"high", nullptr}, {"low", nullptr}}},
                             {"driver", {{"source", "IDXGIAdapter::CheckInterfaceSupport(IDXGIDevice)"},
                                         {"status", "unknown"}, {"available", false}, {"hresult", nullptr},
                                         {"raw_version", nullptr}, {"version", nullptr}}}}},
                {"sdk_candidates", Json::array()}, {"runtime", Json::array()}, {"stages", Json::object()}};

  void Save() const {
    const auto pending = directory / L"ngx-probe-result.pending";
    std::ofstream stream(pending, std::ios::binary | std::ios::trunc);
    stream << value.dump(2) << '\n';
    stream.close();
    if (!stream || !MoveFileExW(pending.c_str(), (directory / L"ngx-probe-result.json").c_str(),
                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      throw std::runtime_error("result_write_failed");
    }
  }

  void Stage(const char* name, const char* status, const char* reason, Json detail = Json::object()) {
    const std::string stage = name;
    const bool static_check = std::string(status) != "running"
        && (stage == "inventory" || stage == "device" || stage == "exports");
    value["stages"][name] = {{"status", status}, {"reason", reason}, {"detail", std::move(detail)},
                             {"evidence", (static_check ? Json("STATIC") : Json())},
                             {"requested_evidence", (static_check ? Json() : Json(stage == "gpu_completion" ? "NATIVE_GPU" : "NATIVE_NGX"))},
                             {"network_evaluated", false}};
    Save();
  }
};

struct Module {
  HMODULE handle = nullptr;
  ~Module() { if (handle != nullptr) { FreeLibrary(handle); } }
};

struct Hash {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE handle = nullptr;
  ~Hash() {
    if (handle != nullptr) { BCryptDestroyHash(handle); }
    if (algorithm != nullptr) { BCryptCloseAlgorithmProvider(algorithm, 0); }
  }
};
}  // namespace

int wmain(int argc, wchar_t** argv) {
  fs::path runtime_dir;
  fs::path output;
  DWORD timeout_ms = 20000;
  bool worker = false;
  bool output_owned = false;
  Report report;
  try {
    for (int i = 1; i < argc; ++i) {
      const std::wstring argument = argv[i];
      if (argument == L"--worker") {
        if (worker) { throw std::runtime_error("duplicate_worker"); }
        worker = true;
      } else if ((argument == L"--runtime-dir" || argument == L"--output" || argument == L"--timeout-ms") && i + 1 < argc) {
        const std::wstring value = argv[++i];
        if (argument == L"--runtime-dir") {
          if (!runtime_dir.empty()) { throw std::runtime_error("duplicate_runtime_dir"); }
          runtime_dir = value;
        } else if (argument == L"--output") {
          if (!output.empty()) { throw std::runtime_error("duplicate_output"); }
          output = value;
        } else {
          if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos) {
            throw std::runtime_error("invalid_timeout");
          }
          const auto parsed = std::stoul(value);
          if (parsed < 100 || parsed > 60000) { throw std::runtime_error("timeout_out_of_bounds"); }
          timeout_ms = parsed;
        }
      } else {
        throw std::runtime_error("usage: ngx_probe --runtime-dir <directory> --output <new_empty_directory> [--timeout-ms 100..60000]");
      }
    }
    if (runtime_dir.empty() || output.empty()) { throw std::runtime_error("runtime_dir_and_output_required"); }
    runtime_dir = fs::absolute(runtime_dir).lexically_normal();
    output = fs::absolute(output).lexically_normal();
    // A trailing empty component otherwise makes lexically_relative compare directories as files.
    for (auto* directory : {&runtime_dir, &output}) {
      if (directory->filename().empty() && *directory != directory->root_path()) {
        *directory = directory->parent_path();
      }
    }
    if (!fs::is_directory(runtime_dir)) { throw std::runtime_error("runtime_directory_missing"); }
    // Do not allow logs or result files to become a Runtime directory entry.
    const auto canonical_output = fs::weakly_canonical(output);
    const auto canonical_runtime = fs::canonical(runtime_dir);
    const auto relative_output = canonical_output.lexically_relative(canonical_runtime);
    if (canonical_output == canonical_runtime || (!relative_output.empty() && *relative_output.begin() != L"..")) {
      throw std::runtime_error("output_must_be_outside_runtime_directory");
    }
    report.directory = output;
    if (!worker) {
      if (fs::exists(output)) {
        if (!fs::is_directory(output) || !fs::is_empty(output)) { throw std::runtime_error("output_not_empty"); }
      } else if (!fs::create_directories(output)) {
        throw std::runtime_error("output_create_failed");
      }
      output_owned = true;
      report.value["timeout_ms"] = timeout_ms;
      for (const char* stage : {"inventory", "device", "exports", "initialize", "capability", "create", "evaluate", "gpu_completion", "cleanup"}) {
        report.value["stages"][stage] = {{"status", "not_attempted"}, {"reason", "prerequisite_not_reached"}, {"detail", Json::object()},
                                         {"evidence", nullptr},
                                         {"requested_evidence", (std::string(stage) == "gpu_completion" ? "NATIVE_GPU" : "NATIVE_NGX")},
                                         {"network_evaluated", false}};
      }
      report.Save();
      std::array<wchar_t, 32768> executable{};
      const auto executable_size = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
      if (executable_size == 0 || executable_size >= executable.size()) { throw std::runtime_error("executable_path_failed"); }
      const auto quote_argument = [](const std::wstring& argument) {
        std::wstring quoted = L"\"";
        size_t backslashes = 0;
        for (const wchar_t character : argument) {
          if (character == L'\\') {
            ++backslashes;
          } else {
            quoted.append(character == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
            backslashes = 0;
            quoted += character;
          }
        }
        quoted.append(backslashes * 2, L'\\');
        quoted += L'"';
        return quoted;
      };
      std::wstring command = quote_argument(executable.data()) + L" --worker --runtime-dir " + quote_argument(runtime_dir.native())
                           + L" --output " + quote_argument(output.native());
      STARTUPINFOW startup{};
      startup.cb = sizeof(startup);
      PROCESS_INFORMATION process{};
      const auto worker_started = GetTickCount64();
      if (!CreateProcessW(executable.data(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                          nullptr, output.c_str(), &startup, &process)) {
        throw std::runtime_error("worker_start_failed");
      }
      CloseHandle(process.hThread);
      const auto wait_result = WaitForSingleObject(process.hProcess, timeout_ms);
      if (wait_result != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 124);
        if (WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0) {
          CloseHandle(process.hProcess);
          output_owned = false;
          throw std::runtime_error("worker_termination_not_confirmed");
        }
      }
      DWORD exit_code = 0;
      GetExitCodeProcess(process.hProcess, &exit_code);
      CloseHandle(process.hProcess);
      std::ifstream stream(output / L"ngx-probe-result.json");
      report.value = Json::parse(stream);
      stream.close();
      report.value["worker_elapsed_ms"] = GetTickCount64() - worker_started;
      report.value["worker_exit_code"] = exit_code;
      if (wait_result != WAIT_OBJECT_0 || (exit_code != 0 && exit_code != 2)) {
        report.value["status"] = "blocked";
        report.value["blocker"] = (wait_result == WAIT_TIMEOUT ? "worker_timeout" : "worker_failed");
        report.value["worker_forcibly_terminated"] = (wait_result != WAIT_OBJECT_0);
      }
      report.Save();
      std::cout << "ngx-probe-result.json: " << report.value["status"].get<std::string>() << '\n';
      return 2;
    }

    // The parent owns the output and the wall-clock bound, including DLL initialization and shutdown.
    std::ifstream initial(output / L"ngx-probe-result.json");
    report.value = Json::parse(initial);
    initial.close();
    output_owned = true;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);
    report.Stage("inventory", "running", "explicit_directory_only");
    for (const auto* name : {L"nvngx_dlssnr.dll", L"nvngx.dll", L"_nvngx.dll"}) {
      const auto file = runtime_dir / name;
      if (!fs::is_regular_file(file)) { continue; }
      Json identity = {{"basename", file.filename().string()}, {"bytes", fs::file_size(file)}};
      Hash hash;
      if (BCryptOpenAlgorithmProvider(&hash.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
          BCryptCreateHash(hash.algorithm, &hash.handle, nullptr, 0, nullptr, 0, 0) < 0) {
        throw std::runtime_error("sha256_initialization_failed");
      }
      std::ifstream bytes(file, std::ios::binary);
      std::array<char, 65536> buffer{};
      while (bytes.read(buffer.data(), buffer.size()) || bytes.gcount() > 0) {
        if (BCryptHashData(hash.handle, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(bytes.gcount()), 0) < 0) {
          throw std::runtime_error("sha256_update_failed");
        }
      }
      if (!bytes.eof()) { throw std::runtime_error("runtime_read_failed"); }
      std::array<unsigned char, 32> digest{};
      if (BCryptFinishHash(hash.handle, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
        throw std::runtime_error("sha256_finish_failed");
      }
      std::ostringstream hex;
      for (const auto value : digest) { hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(value); }
      identity["sha256"] = hex.str();
      DWORD unused = 0;
      std::vector<unsigned char> version(GetFileVersionInfoSizeW(file.c_str(), &unused));
      VS_FIXEDFILEINFO* fixed = nullptr;
      UINT fixed_size = 0;
      identity["version"] = nullptr;
      if (!version.empty() && GetFileVersionInfoW(file.c_str(), 0, static_cast<DWORD>(version.size()), version.data()) &&
          VerQueryValueW(version.data(), L"\\", reinterpret_cast<void**>(&fixed), &fixed_size) &&
          fixed_size >= sizeof(VS_FIXEDFILEINFO)) {
        identity["version"] = std::to_string(HIWORD(fixed->dwFileVersionMS)) + "." + std::to_string(LOWORD(fixed->dwFileVersionMS))
                            + "." + std::to_string(HIWORD(fixed->dwFileVersionLS)) + "." + std::to_string(LOWORD(fixed->dwFileVersionLS));
      }
      // This mapping is for named-export inspection only: no DllMain or export is executed.
      Module image{LoadLibraryExW(file.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES)};
      identity["image_mapped"] = (image.handle != nullptr);
      identity["code_executed"] = false;
      identity["mapping_win32_error"] = (image.handle != nullptr ? 0UL : GetLastError());
      identity["exports"] = Json::object();
      for (const char* symbol : {"NVSDK_NGX_D3D12_Init_with_ProjectID", "NVSDK_NGX_D3D12_Init_Ext",
                                 "NVSDK_NGX_D3D12_GetCapabilityParameters", "NVSDK_NGX_D3D12_AllocateParameters",
                                 "NVSDK_NGX_D3D12_DestroyParameters", "NVSDK_NGX_D3D12_CreateFeature",
                                 "NVSDK_NGX_D3D12_EvaluateFeature", "NVSDK_NGX_D3D12_ReleaseFeature",
                                 "NVSDK_NGX_D3D12_Shutdown1"}) {
        identity["exports"][symbol] = (image.handle != nullptr && GetProcAddress(image.handle, symbol) != nullptr);
      }
      report.value["runtime"].push_back(std::move(identity));
      report.Save();
    }
    report.Stage("inventory", "completed", "basename_hash_version_and_exports_only");
    report.Stage("device", "running", "nvidia_hardware_d3d12_no_game_no_window");
    ComPtr<IDXGIFactory6> factory;
    HRESULT result = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    ComPtr<ID3D12Device> device;
    if (SUCCEEDED(result)) {
      for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        result = factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
        if (result == DXGI_ERROR_NOT_FOUND) { break; }
        if (FAILED(result)) { break; }
        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description)) || description.VendorId != 0x10de || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) { continue; }
        result = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
        if (SUCCEEDED(result)) {
          const auto actual_luid = device->GetAdapterLuid();
          if (actual_luid.HighPart != description.AdapterLuid.HighPart || actual_luid.LowPart != description.AdapterLuid.LowPart) {
            throw std::runtime_error("device_adapter_identity_mismatch");
          }
          auto& identity = report.value["adapter"];
          identity["vendor_id"] = description.VendorId;
          identity["device_id"] = description.DeviceId;
          identity["adapter_flags"] = description.Flags;
          identity["adapter_luid"] = {{"high", actual_luid.HighPart}, {"low", actual_luid.LowPart}};
          const int description_bytes = WideCharToMultiByte(CP_UTF8, 0, description.Description, -1, nullptr, 0, nullptr, nullptr);
          if (description_bytes > 0) {
            std::string text(description_bytes, '\0');
            if (WideCharToMultiByte(CP_UTF8, 0, description.Description, -1, text.data(), description_bytes,
                                    nullptr, nullptr) == description_bytes) {
              text.pop_back();
              identity["description"] = std::move(text);
            }
          }
          LARGE_INTEGER driver{};
          const HRESULT driver_result = adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver);
          identity["driver"]["hresult"] = static_cast<int64_t>(driver_result);
          identity["driver"]["available"] = SUCCEEDED(driver_result);
          if (SUCCEEDED(driver_result)) {
            const auto version = static_cast<uint64_t>(driver.QuadPart);
            identity["driver"]["status"] = "available";
            identity["driver"]["raw_version"] = version;
            identity["driver"]["version"] = std::to_string((version >> 48) & 0xffff) + "."
                + std::to_string((version >> 32) & 0xffff) + "." + std::to_string((version >> 16) & 0xffff)
                + "." + std::to_string(version & 0xffff);
          }
          break;
        }
      }
    }
    report.Stage("device", device ? "completed" : "blocked", device ? "hardware_device_created" : "nvidia_d3d12_device_unavailable",
                 {{"hresult", static_cast<uint32_t>(result)}});
    for (const auto& identity : report.value["runtime"]) {
      // A matching export set is only a static candidate, never SDK/Core/Snippet ABI proof.
      if (identity["exports"]["NVSDK_NGX_D3D12_Init_with_ProjectID"] == true &&
          identity["exports"]["NVSDK_NGX_D3D12_GetCapabilityParameters"] == true &&
          identity["exports"]["NVSDK_NGX_D3D12_DestroyParameters"] == true &&
          identity["exports"]["NVSDK_NGX_D3D12_Shutdown1"] == true) {
        report.value["sdk_candidates"].push_back({{"basename", identity["basename"]}, {"evidence", "STATIC"},
            {"abi_verified", false}, {"initialize_attempted", false}});
      }
    }
    const bool candidate_found = !report.value["sdk_candidates"].empty();
    report.Stage("exports", candidate_found ? "completed" : "blocked",
                 candidate_found ? "public_sdk_export_combination_candidate_only" : "public_project_id_sdk_entrypoints_unavailable",
                 {{"candidate_count", report.value["sdk_candidates"].size()}, {"abi_verified", false}});
    report.Stage("initialize", "not_attempted", "sdk_provider_abi_unverified", {{"api_called", false}, {"abi_verified", false}});
    report.Stage("capability", "not_attempted", "sdk_provider_abi_unverified", {{"api_called", false}, {"feature18_available", nullptr}});
    report.Stage("cleanup", "not_attempted", "no_sdk_initialized", {{"api_called", false}});
    report.Stage("create", "blocked", "Reserved18_create_parameters_and_authorization_contract_unknown");
    report.Stage("evaluate", "blocked", "Reserved18_resource_states_formats_history_and_callbacks_unknown");
    report.Stage("gpu_completion", "not_attempted", "no_feature_commands_recorded_or_submitted_no_completion_claim");
    report.value["blocker"] = "public_feature18_contract_unavailable";
    report.Save();
    return 2;
  } catch (const std::exception&) {
    // Exception messages from filesystem and loader APIs can contain private absolute paths.
    if (output_owned) {
      try {
        report.value["status"] = "blocked";
        report.value["blocker"] = "probe_internal_error";
        report.Save();
      } catch (...) {}
    }
    std::cerr << "NGX probe failed; check arguments and stage evidence (private paths suppressed).\n";
    return 1;
  }
}
