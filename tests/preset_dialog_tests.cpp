/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#include <preset_dialog.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace {
namespace dialogs = renodx::addons::dlss5::presets::dialogs;
namespace fs = std::filesystem;

unsigned checks = 0;
unsigned failures = 0;
unsigned show_calls = 0;
unsigned shell_calls = 0;
dialogs::Kind expected_kind = dialogs::Kind::OPEN_FILE;
fs::path expected_directory;
fs::path expected_filename;
HRESULT show_status = HRESULT_FROM_WIN32(ERROR_CANCELLED);
INT_PTR shell_status = 33;

void Check(bool condition, const char* description) {
  ++checks;
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << description << '\n';
  }
}

HRESULT InspectDialog(IFileDialog* dialog, HWND owner) {
  ++show_calls;
  Check(owner == nullptr, "Dialog has no game owner.");
  APTTYPE apartment;
  APTTYPEQUALIFIER qualifier;
  Check(SUCCEEDED(CoGetApartmentType(&apartment, &qualifier))
            && (apartment == APTTYPE_STA || apartment == APTTYPE_MAINSTA), "Dialog runs in STA.");
  FILEOPENDIALOGOPTIONS options = 0;
  Check(SUCCEEDED(dialog->GetOptions(&options)), "Read native COM dialog options.");
  constexpr auto COMMON = FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT;
  Check((options & COMMON) == COMMON && (options & FOS_ALLOWMULTISELECT) == 0,
        "Filesystem-only single selection avoids changing CWD and recent items.");
  Microsoft::WRL::ComPtr<IFileSaveDialog> save;
  Check(SUCCEEDED(dialog->QueryInterface(IID_PPV_ARGS(save.GetAddressOf()))) == (expected_kind == dialogs::Kind::SAVE_FILE),
        "Native COM interface matches open/save kind.");
  Check(((options & FOS_PICKFOLDERS) != 0) == (expected_kind == dialogs::Kind::FOLDER), "Folder picker only for folders.");
  if (expected_kind == dialogs::Kind::SAVE_FILE) {
    Check((options & FOS_OVERWRITEPROMPT) != 0 && (options & FOS_NOTESTFILECREATE) != 0,
          "Save prompts before overwrite and never probes by creating a file.");
  } else if (expected_kind == dialogs::Kind::OPEN_FILE) {
    Check((options & FOS_FILEMUSTEXIST) != 0, "Open requires an existing file.");
  }
  if (expected_kind != dialogs::Kind::FOLDER) {
    UINT filter = 0;
    Check(SUCCEEDED(dialog->GetFileTypeIndex(&filter)) && filter == 1, "Preset filter selected by default.");
  }
  if (!expected_filename.empty()) {
    dialogs::internal::ShellPath filename;
    Check(SUCCEEDED(dialog->GetFileName(&filename.value)) && filename.value != nullptr
              && fs::path(filename.value) == expected_filename, "Native dialog receives only the suggested filename.");
  }
  Microsoft::WRL::ComPtr<IShellItem> folder;
  dialogs::internal::ShellPath folder_path;
  Check(SUCCEEDED(dialog->GetFolder(folder.GetAddressOf()))
            && SUCCEEDED(folder->GetDisplayName(SIGDN_FILESYSPATH, &folder_path.value)) && folder_path.value != nullptr
            && fs::equivalent(fs::path(folder_path.value), expected_directory), "Native dialog receives initial directory.");
  return show_status;
}

HINSTANCE WINAPI InspectShell(HWND owner, LPCWSTR operation, LPCWSTR file, LPCWSTR parameters,
                             LPCWSTR directory, INT show) {
  ++shell_calls;
  APTTYPE apartment;
  APTTYPEQUALIFIER qualifier;
  Check(SUCCEEDED(CoGetApartmentType(&apartment, &qualifier))
            && (apartment == APTTYPE_STA || apartment == APTTYPE_MAINSTA), "Folder action initializes STA for Shell extensions.");
  Check(owner == nullptr && parameters == nullptr && directory == nullptr, "Shell has no owner, command parameters or CWD.");
  Check(std::wstring(operation) == L"explore" && fs::path(file) == expected_directory && show == SW_SHOWNORMAL,
        "Shell explores the verified directory as one object.");
  return reinterpret_cast<HINSTANCE>(shell_status);
}

}  // namespace

int main() {
  // No native Show or ShellExecute call may escape the injected callbacks.
  struct Callbacks {
    decltype(dialogs::internal::show_dialog) show = dialogs::internal::show_dialog;
    decltype(dialogs::internal::shell_execute) shell = dialogs::internal::shell_execute;
    ~Callbacks() {
      dialogs::internal::show_dialog = show;
      dialogs::internal::shell_execute = shell;
    }
  } callbacks;
  dialogs::internal::show_dialog = InspectDialog;
  dialogs::internal::shell_execute = InspectShell;
  expected_directory = fs::current_path();
  const auto original_directory = fs::current_path();
  for (const auto kind : {dialogs::Kind::OPEN_FILE, dialogs::Kind::SAVE_FILE, dialogs::Kind::FOLDER}) {
    expected_kind = kind;
    expected_filename = (kind == dialogs::Kind::FOLDER ? fs::path{} : fs::path(L"synthetic.nrnexus.json"));
    const auto result = dialogs::Choose(kind, expected_directory, expected_filename);
    Check(result.cancelled && result.error.empty() && result.path.empty(), "Cancellation is distinct from failure and has no path.");
    Check(fs::current_path() == original_directory && !fs::exists(expected_directory / L"synthetic.nrnexus.json"),
          "Choosing creates no file and leaves working directory unchanged.");
    APTTYPE apartment;
    APTTYPEQUALIFIER qualifier;
    Check(CoGetApartmentType(&apartment, &qualifier) == CO_E_NOTINITIALIZED, "Choose balances its STA initialization.");
  }
  expected_kind = dialogs::Kind::OPEN_FILE;
  expected_filename.clear();
  show_status = E_ACCESSDENIED;
  auto result = dialogs::Choose(expected_kind, expected_directory);
  Check(!result.cancelled && result.path.empty() && !result.error.empty(), "Show failure is not cancellation.");
  show_status = S_OK;
  result = dialogs::Choose(expected_kind, expected_directory);
  Check(!result.cancelled && result.path.empty() && !result.error.empty(), "Missing COM result after injected Show success fails safely.");
  show_status = HRESULT_FROM_WIN32(ERROR_CANCELLED);
  {
    dialogs::internal::Apartment existing_sta;
    Check(SUCCEEDED(existing_sta.status), "Initialize caller-owned STA.");
    Check(dialogs::Choose(expected_kind, expected_directory).cancelled, "Choose supports an existing STA.");
    APTTYPE apartment;
    APTTYPEQUALIFIER qualifier;
    Check(SUCCEEDED(CoGetApartmentType(&apartment, &qualifier)), "Choose preserves caller-owned COM initialization.");
  }
  const auto invalid_calls = show_calls;
  for (const auto& directory : {fs::path(L"relative"), expected_directory / L"missing-directory", fs::path(std::wstring(L"C:\\bad\0path", 11))}) {
    result = dialogs::Choose(expected_kind, directory);
    Check(!result.cancelled && result.path.empty() && !result.error.empty(), "Invalid directory fails before Show.");
  }
  for (const auto& filename : {fs::path(L"../escape.json"), fs::path(L"C:\\escape.json"), fs::path(L"stream:payload.json"), fs::path(L"..")}) {
    result = dialogs::Choose(dialogs::Kind::SAVE_FILE, expected_directory, filename);
    Check(!result.cancelled && result.path.empty() && !result.error.empty(), "Unsafe suggested filename fails before Show.");
  }
  result = dialogs::Choose(static_cast<dialogs::Kind>(99), expected_directory);
  Check(!result.cancelled && !result.error.empty() && show_calls == invalid_calls, "Invalid inputs never display a dialog.");
  std::thread mta([&] {
    const auto status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Check(SUCCEEDED(status), "Initialize test worker MTA.");
    const auto rejected = dialogs::Choose(dialogs::Kind::OPEN_FILE, expected_directory);
    Check(!rejected.cancelled && rejected.path.empty() && !rejected.error.empty() && show_calls == invalid_calls,
          "Existing MTA fails without changing apartment mode or displaying a dialog.");
    std::string folder_error;
    Check(!dialogs::OpenFolder(expected_directory, &folder_error) && !folder_error.empty() && shell_calls == 0,
          "Existing MTA folder action fails before Shell.");
    if (SUCCEEDED(status)) {
      CoUninitialize();
    }
  });
  mta.join();
  std::string error = "stale error";
  Check(dialogs::OpenFolder(expected_directory, &error) && error.empty(), "Explicit folder action clears stale error on success.");
  shell_status = SE_ERR_ACCESSDENIED;
  Check(!dialogs::OpenFolder(expected_directory, &error) && !error.empty(), "Shell failure is reported.");
  APTTYPE apartment;
  APTTYPEQUALIFIER qualifier;
  Check(CoGetApartmentType(&apartment, &qualifier) == CO_E_NOTINITIALIZED, "Folder action balances its COM initialization.");
  const auto invalid_shell_calls = shell_calls;
  const auto file_path = expected_directory / L"synthetic-file.txt";
  {
    std::ofstream file(file_path);
    file << "synthetic";
  }
  for (const auto& path : {fs::path{}, fs::path(L"relative"), fs::path(L"https://example.invalid"), file_path}) {
    Check(!dialogs::OpenFolder(path, &error) && !error.empty(), "Folder action rejects empty, relative, URL and regular file inputs.");
  }
  Check(shell_calls == invalid_shell_calls, "Rejected folders never reach Shell.");
  Check(!dialogs::OpenFolder(file_path, nullptr), "Optional error pointer may be null.");
  fs::remove(file_path);
  std::cout << checks << " checks, " << failures << " failures; no dialog shown, no Explorer launched\n";
  return (failures == 0 ? 0 : 1);
}
