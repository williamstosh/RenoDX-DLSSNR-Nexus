/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

namespace renodx::addons::dlss5::presets::dialogs {

enum class Kind { OPEN_FILE, SAVE_FILE, FOLDER };

struct Result {
  std::filesystem::path path;
  bool cancelled = false;
  std::string error;
};

namespace internal {

struct Apartment {
  HRESULT status = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  Apartment() = default;
  Apartment(const Apartment&) = delete;
  Apartment& operator=(const Apartment&) = delete;

  ~Apartment() {
    if (SUCCEEDED(status)) {
      CoUninitialize();
    }
  }
};

struct ShellPath {
  PWSTR value = nullptr;

  ShellPath() = default;
  ShellPath(const ShellPath&) = delete;
  ShellPath& operator=(const ShellPath&) = delete;

  ~ShellPath() { CoTaskMemFree(value); }
};

inline std::string StatusError(const char* operation, HRESULT status) {
  return std::string(operation) + " (HRESULT " + std::to_string(static_cast<std::uint32_t>(status)) + ").";
}

inline bool IsDirectory(const std::filesystem::path& path, std::string* error) {
  if (path.empty() || !path.is_absolute() || path.native().find(L'\0') != std::wstring::npos) {
    if (error != nullptr) {
      *error = "Select an absolute filesystem directory.";
    }
    return false;
  }
  std::error_code status;
  if (!std::filesystem::is_directory(path, status) || status) {
    if (error != nullptr) {
      *error = "The selected directory is unavailable.";
    }
    return false;
  }
  return true;
}

// Only tests replace these callbacks, before starting any worker. Production
// keeps the native calls; no hook or process-wide COM configuration is installed.
static auto show_dialog = +[](IFileDialog* dialog, HWND owner) -> HRESULT { return dialog->Show(owner); };
static decltype(&ShellExecuteW) shell_execute = ShellExecuteW;

}  // namespace internal

// Call on a module-pinned worker, never the render thread. Selection is UI data:
// preset_store still owns path validation, I/O and explicit overwrite consent.
inline Result Choose(Kind kind, const std::filesystem::path& initial_directory,
                     const std::filesystem::path& suggested_filename = {}) {
  Result result;
  if (kind != Kind::OPEN_FILE && kind != Kind::SAVE_FILE && kind != Kind::FOLDER) {
    result.error = "Unsupported file dialog kind.";
    return result;
  }
  if (!initial_directory.empty() && !internal::IsDirectory(initial_directory, &result.error)) {
    return result;
  }
  if (!suggested_filename.empty()
      && (kind == Kind::FOLDER || suggested_filename.has_parent_path()
          || suggested_filename.native().find_first_of(L"<>:\"/\\|?*") != std::wstring::npos
          || suggested_filename.native().find(L'\0') != std::wstring::npos
          || suggested_filename == L"." || suggested_filename == L"..")) {
    result.error = "The suggested filename must be a single filename.";
    return result;
  }
  internal::Apartment apartment;
  if (FAILED(apartment.status)) {
    result.error = internal::StatusError("Cannot initialize file dialog STA", apartment.status);
    return result;
  }
  Microsoft::WRL::ComPtr<IFileDialog> dialog;
  HRESULT status = CoCreateInstance((kind == Kind::SAVE_FILE ? CLSID_FileSaveDialog : CLSID_FileOpenDialog),
                                    nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.GetAddressOf()));
  if (FAILED(status)) {
    result.error = internal::StatusError("Cannot create file dialog", status);
    return result;
  }
  FILEOPENDIALOGOPTIONS options = 0;
  status = dialog->GetOptions(&options);
  if (SUCCEEDED(status)) {
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT;
    options &= ~FOS_ALLOWMULTISELECT;
    switch (kind) {
      case Kind::OPEN_FILE: options |= FOS_FILEMUSTEXIST; break;
      case Kind::SAVE_FILE: options |= FOS_OVERWRITEPROMPT | FOS_NOTESTFILECREATE; break;
      case Kind::FOLDER: options |= FOS_PICKFOLDERS; break;
    }
    status = dialog->SetOptions(options);
  }
  if (FAILED(status)) {
    result.error = internal::StatusError("Cannot configure file dialog options", status);
    return result;
  }
  if (kind != Kind::FOLDER) {
    const COMDLG_FILTERSPEC filters[] = {{L"Presets (*.nrnexus.json)", L"*.nrnexus.json"},
                                       {L"All JSON (*.json)", L"*.json"}};
    status = dialog->SetFileTypes(2, filters);
    if (SUCCEEDED(status)) {
      status = dialog->SetFileTypeIndex(1);
    }
    if (SUCCEEDED(status) && kind == Kind::SAVE_FILE) {
      status = dialog->SetDefaultExtension(L"nrnexus.json");
    }
    if (FAILED(status)) {
      result.error = internal::StatusError("Cannot configure preset file types", status);
      return result;
    }
  }
  if (!initial_directory.empty()) {
    Microsoft::WRL::ComPtr<IShellItem> folder;
    status = SHCreateItemFromParsingName(initial_directory.c_str(), nullptr, IID_PPV_ARGS(folder.GetAddressOf()));
    if (SUCCEEDED(status)) {
      status = dialog->SetFolder(folder.Get());
    }
    if (FAILED(status)) {
      result.error = internal::StatusError("Cannot set file dialog directory", status);
      return result;
    }
  }
  if (!suggested_filename.empty()) {
    status = dialog->SetFileName(suggested_filename.c_str());
    if (FAILED(status)) {
      result.error = internal::StatusError("Cannot set suggested filename", status);
      return result;
    }
  }
  status = internal::show_dialog(dialog.Get(), nullptr);
  if (status == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
    result.cancelled = true;
    return result;
  }
  if (FAILED(status)) {
    result.error = internal::StatusError("Cannot show file dialog", status);
    return result;
  }
  Microsoft::WRL::ComPtr<IShellItem> selected;
  internal::ShellPath selected_path;
  status = dialog->GetResult(selected.GetAddressOf());
  if (SUCCEEDED(status)) {
    status = selected->GetDisplayName(SIGDN_FILESYSPATH, &selected_path.value);
  }
  if (FAILED(status)) {
    result.error = internal::StatusError("Cannot read selected filesystem path", status);
    return result;
  }
  if (selected_path.value == nullptr || selected_path.value[0] == L'\0') {
    result.error = "The file dialog returned an empty path.";
    return result;
  }
  result.path = selected_path.value;
  return result;
}

// Only an explicit UI action may call this on the pinned worker.
inline bool OpenFolder(const std::filesystem::path& path, std::string* error) {
  if (error != nullptr) {
    error->clear();
  }
  if (!internal::IsDirectory(path, error)) {
    return false;
  }
  internal::Apartment apartment;
  if (FAILED(apartment.status)) {
    if (error != nullptr) {
      *error = internal::StatusError("Cannot initialize folder action STA", apartment.status);
    }
    return false;
  }
  // A verified directory is the Shell object, never executable/command text.
  // No parameters, owner window, working-directory changes or cmd.exe are used.
  const auto status = reinterpret_cast<INT_PTR>(internal::shell_execute(
      nullptr, L"explore", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  if (status <= 32) {
    if (error != nullptr) {
      *error = "Cannot open the selected folder (Shell error " + std::to_string(status) + ").";
    }
    return false;
  }
  return true;
}

}  // namespace renodx::addons::dlss5::presets::dialogs
