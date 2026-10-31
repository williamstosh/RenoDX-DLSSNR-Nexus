/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "brand.h"

namespace renodx::addons::dlss5::presets {

using Json = nlohmann::json;

enum class Scope { QUALITY, QUALITY_MULTIPASS, FULL };

struct Document {
  Json metadata = Json::object();
  Scope scope = Scope::QUALITY;
  Json settings = Json::object();
};

inline constexpr std::size_t MAX_FILE_BYTES = 256 * 1024;
inline constexpr std::size_t MAX_DEPTH = 8;
inline constexpr std::size_t MAX_SETTINGS = 512;
inline constexpr std::size_t MAX_LIBRARY_ENTRIES = 1024;

namespace internal {

inline bool Fail(std::string* error, std::string_view message) {
  if (error != nullptr) {
    error->assign(message);
  }
  return false;
}

inline bool WindowsError(std::string* error, std::string_view operation) {
  const DWORD code = GetLastError();
  return Fail(error, std::string(operation) + " (Windows error " + std::to_string(code) + ").");
}

inline bool ValidateDocument(const Document& document, std::string* error) {
  switch (document.scope) {
    case Scope::QUALITY:
    case Scope::QUALITY_MULTIPASS:
    case Scope::FULL: break;
    default: return Fail(error, "Invalid preset scope.");
  }
  if (!document.metadata.is_object()) return Fail(error, "Metadata must be an object.");
  for (const auto& [key, value] : document.metadata.items()) {
    if (key == "tags") {
      if (!value.is_array() || value.size() > 16) return Fail(error, "Metadata tags must contain at most 16 strings.");
      for (const auto& tag : value) {
        if (!tag.is_string() || tag.get_ref<const std::string&>().size() > 64) {
          return Fail(error, "Metadata tags must be strings of at most 64 UTF-8 bytes.");
        }
      }
      continue;
    }
    std::size_t limit = 4096;
    if (key == "name" || key == "author" || key == "source_game") {
      limit = 128;
    } else if (key == "description") {
      limit = 2048;
    } else if (key != "created_with") {
      return Fail(error, "Unknown metadata field.");
    }
    if (!value.is_string() || value.get_ref<const std::string&>().size() > limit) {
      return Fail(error, "Metadata field has an invalid type or length.");
    }
  }
  if (!document.settings.is_object() || document.settings.size() > MAX_SETTINGS) {
    return Fail(error, "Settings must be an object with at most 512 entries.");
  }
  // Unknown setting keys remain inert data for the caller's preview and allowlist.
  for (const auto& [key, value] : document.settings.items()) {
    if (key.size() > 4096) return Fail(error, "Setting key exceeds 4096 UTF-8 bytes.");
    if (value.is_boolean()) continue;
    if (value.is_number()) {
      if (!value.is_number_float() || std::isfinite(value.get<double>())) continue;
      return Fail(error, "Setting numbers must be finite.");
    }
    if (value.is_string() && value.get_ref<const std::string&>().size() <= 128) continue;
    return Fail(error, "Setting values must be booleans, finite numbers or strings of at most 128 UTF-8 bytes.");
  }
  return true;
}

struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;

  Handle() = default;
  explicit Handle(HANDLE native) : value(native) {}
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
  Handle& operator=(Handle&& other) noexcept {
    if (this != &other) {
      if (value != INVALID_HANDLE_VALUE) {
        CloseHandle(value);
      }
      value = std::exchange(other.value, INVALID_HANDLE_VALUE);
    }
    return *this;
  }
  ~Handle() {
    if (value != INVALID_HANDLE_VALUE) {
      CloseHandle(value);
    }
  }
};

inline bool IsComponent(std::wstring_view component) {
  if (component.empty() || component == L"." || component == L".."
      || component.back() == L'.' || component.back() == L' ') return false;
  for (const wchar_t character : component) {
    if (character < 32 || std::wstring_view(L"<>:\"/\\|?*").find(character) != std::wstring_view::npos) return false;
  }
  std::wstring base(component.substr(0, component.find(L'.')));
  while (!base.empty() && base.back() == L' ') {
    base.pop_back();
  }
  for (auto& character : base) {
    if (character >= L'a' && character <= L'z') {
      character -= L'a' - L'A';
    }
  }
  if (base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL"
      || base == L"CONIN$" || base == L"CONOUT$") return false;
  if (base.size() == 4 && (base.starts_with(L"COM") || base.starts_with(L"LPT"))) {
    const wchar_t digit = base.back();
    if ((digit >= L'1' && digit <= L'9') || digit == L'\u00b9' || digit == L'\u00b2' || digit == L'\u00b3') return false;
  }
  return true;
}

inline bool IsFilename(const std::filesystem::path& filename) {
  const auto& text = filename.native();
  constexpr std::wstring_view SUFFIX = L".nrnexus.json";
  return IsComponent(text) && text.size() > SUFFIX.size() && text.size() <= 128 + SUFFIX.size()
         && text.ends_with(SUFFIX);
}

// Hold every ancestor without write/delete sharing until the operation finishes.
// OPEN_REPARSE_POINT alone protects only the final component of each CreateFile.
inline bool OpenDirectory(const std::filesystem::path& path, bool create,
                          std::vector<Handle>* ancestors, std::string* error) {
  const auto root = path.root_name().native();
  if (!path.is_absolute() || root.size() != 2 || root[1] != L':'
      || !((root[0] >= L'A' && root[0] <= L'Z') || (root[0] >= L'a' && root[0] <= L'z'))) {
    return Fail(error, "An absolute local drive path is required.");
  }
  const auto drive_type = GetDriveTypeW(path.root_path().c_str());
  if (drive_type != DRIVE_FIXED && drive_type != DRIVE_REMOVABLE && drive_type != DRIVE_RAMDISK) {
    return Fail(error, "Only local disk paths are supported.");
  }
  // Validate before creating any directory, including components beyond '..'.
  for (const auto& component : path.relative_path()) {
    if (!IsComponent(component.native())) return Fail(error, "Unsafe path component.");
  }
  auto current = path.root_path();
  const auto open = [&]() {
    Handle directory(CreateFileW(current.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (directory.value == INVALID_HANDLE_VALUE) return WindowsError(error, "Cannot open preset directory");
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(directory.value, &information)) return WindowsError(error, "Cannot inspect preset directory");
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
        || (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0
        || GetFileType(directory.value) != FILE_TYPE_DISK) {
      return Fail(error, "Directory reparse points and non-directories are not allowed.");
    }
    ancestors->push_back(std::move(directory));
    return true;
  };
  if (!open()) return false;
  for (const auto& component : path.relative_path()) {
    current /= component;
    if (create && !CreateDirectoryW(current.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
      return WindowsError(error, "Cannot create preset directory");
    }
    if (!open()) return false;
  }
  return true;
}

inline bool OpenRegularFile(const std::filesystem::path& path, DWORD access, DWORD sharing,
                            Handle* file, std::string* error) {
  Handle opened(CreateFileW(path.c_str(), access, sharing, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  if (opened.value == INVALID_HANDLE_VALUE) return WindowsError(error, "Cannot open preset file");
  BY_HANDLE_FILE_INFORMATION information{};
  if (!GetFileInformationByHandle(opened.value, &information)) return WindowsError(error, "Cannot inspect preset file");
  if ((information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0
      || GetFileType(opened.value) != FILE_TYPE_DISK || information.nNumberOfLinks != 1) {
    return Fail(error, "Preset files must be regular disk files without reparse points or hard links.");
  }
  *file = std::move(opened);
  return true;
}

inline bool ReadContents(HANDLE file, std::string* contents, std::string* error) {
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size)) return WindowsError(error, "Cannot measure preset file");
  if (size.QuadPart < 0 || size.QuadPart > static_cast<LONGLONG>(MAX_FILE_BYTES)) {
    return Fail(error, "Preset exceeds 256 KiB.");
  }
  std::string text(static_cast<std::size_t>(size.QuadPart), '\0');
  DWORD count = 0;
  if (!::ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &count, nullptr)) {
    return WindowsError(error, "Cannot read preset file");
  }
  if (count != text.size()) return Fail(error, "Incomplete preset file read.");
  *contents = std::move(text);
  return true;
}

struct TemporaryFile {
  Handle file;
  bool published = false;

  ~TemporaryFile() {
    if (!published && file.value != INVALID_HANDLE_VALUE) {
      FILE_DISPOSITION_INFO disposition{.DeleteFile = TRUE};
      SetFileInformationByHandle(file.value, FileDispositionInfo, &disposition, sizeof(disposition));
    }
  }
};

}  // namespace internal

// Limits on JSON strings are UTF-8 byte limits. Failure preserves output.
inline bool ParseJson(std::string_view text, Json* output, std::string* error = nullptr) {
  if (output == nullptr) return internal::Fail(error, "Missing document output.");
  if (text.size() > MAX_FILE_BYTES) return internal::Fail(error, "Preset exceeds 256 KiB.");
  try {
    std::vector<std::unordered_set<std::string>> keys;
    const auto callback = [&](int depth, Json::parse_event_t event, Json& value) {
      if (event == Json::parse_event_t::object_start || event == Json::parse_event_t::array_start) {
        if (depth >= static_cast<int>(MAX_DEPTH)) throw std::invalid_argument("JSON nesting exceeds depth 8.");
        keys.emplace_back();
      } else if (event == Json::parse_event_t::key) {
        const auto& key = value.get_ref<const std::string&>();
        if (key.size() > 4096) throw std::invalid_argument("JSON key exceeds 4096 UTF-8 bytes.");
        if (!keys.back().insert(key).second) throw std::invalid_argument("Duplicate JSON key.");
      } else if (event == Json::parse_event_t::object_end || event == Json::parse_event_t::array_end) {
        keys.pop_back();
      } else if (event == Json::parse_event_t::value && value.is_string()
                 && value.get_ref<const std::string&>().size() > 4096) {
        throw std::invalid_argument("JSON string exceeds 4096 UTF-8 bytes.");
      }
      return true;
    };
    auto root = Json::parse(text.begin(), text.end(), callback);
    *output = std::move(root);
    if (error != nullptr) {
      error->clear();
    }
    return true;
  } catch (const std::invalid_argument& exception) {
    return internal::Fail(error, exception.what());
  } catch (const std::exception&) {
    return internal::Fail(error, "Invalid JSON or UTF-8.");
  }
}

inline bool Parse(std::string_view text, Document* output, std::string* error = nullptr) {
  if (output == nullptr) return internal::Fail(error, "Missing document output.");
  try {
    Json root;
    if (!ParseJson(text, &root, error)) return false;
    if (!root.is_object()) return internal::Fail(error, "Preset must be an object.");
    for (const auto& [key, value] : root.items()) {
      if (key != "format" && key != "schema_version" && key != "metadata" && key != "scope" && key != "settings") {
        return internal::Fail(error, "Unknown preset field.");
      }
    }
    if (!root.contains("format") || root["format"] != "nr-nexus-preset") {
      return internal::Fail(error, "Unsupported preset format.");
    }
    if (!root.contains("schema_version") || !root["schema_version"].is_number_integer() || root["schema_version"] != 1) {
      return internal::Fail(error, "Unsupported preset schema version.");
    }
    if (!root.contains("scope") || !root["scope"].is_string()) return internal::Fail(error, "Missing preset scope.");
    if (!root.contains("settings")) return internal::Fail(error, "Missing preset settings; no settings were reset.");
    Document document{.metadata = root.value("metadata", Json::object()), .settings = root["settings"]};
    const auto& scope = root["scope"].get_ref<const std::string&>();
    if (scope == "quality") {
      document.scope = Scope::QUALITY;
    } else if (scope == "quality-multipass") {
      document.scope = Scope::QUALITY_MULTIPASS;
    } else if (scope == "full") {
      document.scope = Scope::FULL;
    } else {
      return internal::Fail(error, "Unknown preset scope.");
    }
    if (!internal::ValidateDocument(document, error)) return false;
    *output = std::move(document);
    if (error != nullptr) {
      error->clear();
    }
    return true;
  } catch (const Json::exception&) {
    return internal::Fail(error, "Invalid preset JSON or UTF-8.");
  } catch (const std::invalid_argument& exception) {
    return internal::Fail(error, exception.what());
  } catch (const std::exception&) {
    return internal::Fail(error, "Cannot parse preset.");
  }
}

// dump() preserves binary64 round trips; never narrow settings to float.
inline std::string Serialize(Document document) {
  std::string error;
  if (!internal::ValidateDocument(document, &error)) throw std::invalid_argument(error);
  std::string_view scope;
  switch (document.scope) {
    case Scope::QUALITY: scope = "quality"; break;
    case Scope::QUALITY_MULTIPASS: scope = "quality-multipass"; break;
    case Scope::FULL: scope = "full"; break;
    default: throw std::invalid_argument("Invalid preset scope.");
  }
  const Json root{{"format", "nr-nexus-preset"}, {"schema_version", 1}, {"metadata", std::move(document.metadata)},
                  {"scope", scope}, {"settings", std::move(document.settings)}};
  auto text = root.dump(2) + "\n";
  if (text.size() > MAX_FILE_BYTES) throw std::invalid_argument("Preset exceeds 256 KiB.");
  return text;
}

inline bool MakeFilename(std::string_view name, std::filesystem::path* filename, std::string* error = nullptr) {
  if (filename == nullptr) return internal::Fail(error, "Missing filename output.");
  if (name.size() > 4096) return internal::Fail(error, "Preset name exceeds 4096 UTF-8 bytes.");
  try {
    std::wstring stem;
    if (!name.empty()) {
      const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), nullptr, 0);
      if (count == 0) return internal::Fail(error, "Preset name must be valid UTF-8.");
      stem.resize(count);
      if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), stem.data(), count) != count) {
        return internal::WindowsError(error, "Cannot convert preset name");
      }
    }
    for (auto& character : stem) {
      if (character < 32 || std::wstring_view(L"<>:\"/\\|?*.").find(character) != std::wstring_view::npos) {
        character = L'-';
      }
    }
    if (stem.size() > 120) {
      stem.resize(120);
      if (stem.back() >= 0xd800 && stem.back() <= 0xdbff) {
        stem.pop_back();
      }
    }
    while (!stem.empty() && stem.back() == L' ') {
      stem.pop_back();
    }
    if (stem.empty()) {
      stem = L"preset";
    }
    if (!internal::IsComponent(stem)) {
      stem.insert(0, L"preset-");
    }
    *filename = std::filesystem::path(stem + L".nrnexus.json");
    if (error != nullptr) {
      error->clear();
    }
    return true;
  } catch (const std::exception&) {
    return internal::Fail(error, "Cannot generate preset filename.");
  }
}

// Resolves LOCALAPPDATA at each call so independently loaded add-ons share disk data,
// without retaining cross-module pointers or caching a game-specific directory.
inline bool LibraryPath(std::filesystem::path* path, std::string* error = nullptr) {
  if (path == nullptr) return internal::Fail(error, "Missing library path output.");
  try {
    const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (required == 0 || required > 32768) return internal::Fail(error, "LOCALAPPDATA is unavailable or invalid.");
    std::wstring local(required, L'\0');
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local.data(), required);
    if (length == 0 || length >= required) return internal::Fail(error, "LOCALAPPDATA changed or is invalid.");
    local.resize(length);
    auto directory = std::filesystem::path(local) / NR_NEXUS_NAME / L"Presets";
    std::vector<internal::Handle> ancestors;
    if (!internal::OpenDirectory(directory, true, &ancestors, error)) return false;
    *path = std::move(directory);
    if (error != nullptr) {
      error->clear();
    }
    return true;
  } catch (const std::exception&) {
    return internal::Fail(error, "Cannot resolve preset library.");
  }
}

// External paths must come from an explicit UI choice, never from preset JSON.
namespace internal {

inline bool ReadTextFile(const std::filesystem::path& absolute_path, std::string* output, std::string* error) {
  if (output == nullptr) return internal::Fail(error, "Missing document output.");
  try {
    if (!internal::IsComponent(absolute_path.filename().native())) return internal::Fail(error, "Unsafe import filename.");
    std::vector<internal::Handle> ancestors;
    if (!internal::OpenDirectory(absolute_path.parent_path(), false, &ancestors, error)) return false;
    internal::Handle file;
    if (!internal::OpenRegularFile(absolute_path, GENERIC_READ, FILE_SHARE_READ, &file, error)) return false;
    return internal::ReadContents(file.value, output, error);
  } catch (const std::exception&) {
    return internal::Fail(error, "Cannot read preset.");
  }
}

}  // namespace internal

inline bool ReadJsonFile(const std::filesystem::path& absolute_path, Json* output, std::string* error = nullptr) {
  if (output == nullptr) return internal::Fail(error, "Missing JSON output.");
  std::string text;
  if (!internal::ReadTextFile(absolute_path, &text, error)) return false;
  return ParseJson(text, output, error);
}

inline bool ReadFile(const std::filesystem::path& absolute_path, Document* output, std::string* error = nullptr) {
  if (output == nullptr) return internal::Fail(error, "Missing document output.");
  std::string text;
  if (!internal::ReadTextFile(absolute_path, &text, error)) return false;
  return Parse(text, output, error);
}

// Same-directory CREATE_NEW + read-back verification + rename gives readers either
// the complete old document or the complete new document. Conflicts never auto-suffix.
template <typename Validate>
inline bool WriteJsonFile(const std::filesystem::path& absolute_path, std::string_view text,
                          bool overwrite, Validate validate, std::string* error = nullptr) {
  try {
    if (!internal::IsComponent(absolute_path.filename().native())) return internal::Fail(error, "Unsafe JSON filename.");
    Json checked;
    if (!ParseJson(text, &checked, error) || !validate(checked, error)) return false;
    std::vector<internal::Handle> ancestors;
    if (!internal::OpenDirectory(absolute_path.parent_path(), false, &ancestors, error)) return false;
    internal::Handle existing;
    const DWORD attributes = GetFileAttributesW(absolute_path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
      if (!internal::OpenRegularFile(absolute_path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                     &existing, error)) return false;
      if (!overwrite) return internal::Fail(error, "Preset already exists; explicit overwrite is required.");
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND) {
      return internal::WindowsError(error, "Cannot inspect preset destination");
    }
    internal::TemporaryFile temporary;
    // Per-module counters only choose names; CREATE_NEW arbitrates across modules.
    static std::atomic<std::uint64_t> sequence = 0;
    for (int attempt = 0; attempt < 32; ++attempt) {
      auto candidate = absolute_path.parent_path()
                       / (L".nrnexus-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())
                          + L"-" + std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed)) + L".tmp");
      internal::Handle file(CreateFileW(candidate.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
                                       0, nullptr, CREATE_NEW,
                                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
      if (file.value != INVALID_HANDLE_VALUE) {
        temporary.file = std::move(file);
        break;
      }
      if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) {
        return internal::WindowsError(error, "Cannot create temporary preset");
      }
    }
    if (temporary.file.value == INVALID_HANDLE_VALUE) return internal::Fail(error, "Temporary preset name conflicts.");
    DWORD count = 0;
    if (!::WriteFile(temporary.file.value, text.data(), static_cast<DWORD>(text.size()), &count, nullptr)) {
      return internal::WindowsError(error, "Cannot write temporary preset");
    }
    if (count != text.size()) return internal::Fail(error, "Incomplete temporary preset write.");
    if (!FlushFileBuffers(temporary.file.value)) return internal::WindowsError(error, "Cannot flush temporary preset");
    if (!SetFilePointerEx(temporary.file.value, {}, nullptr, FILE_BEGIN)) {
      return internal::WindowsError(error, "Cannot rewind temporary preset");
    }
    std::string verified;
    if (!internal::ReadContents(temporary.file.value, &verified, error)) return false;
    Json parsed;
    if (verified != text) return internal::Fail(error, "Temporary preset read-back mismatch.");
    if (!ParseJson(verified, &parsed, error) || !validate(parsed, error)) return false;
    const auto& destination = absolute_path.native();
    const auto rename_bytes = sizeof(FILE_RENAME_INFO) + (destination.size() + 1) * sizeof(wchar_t);
    std::vector<std::uint64_t> storage((rename_bytes + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t), 0);
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    rename->ReplaceIfExists = overwrite;
    rename->FileNameLength = static_cast<DWORD>(destination.size() * sizeof(wchar_t));
    std::memcpy(rename->FileName, destination.c_str(), (destination.size() + 1) * sizeof(wchar_t));
    existing = internal::Handle();
    // Publish the verified object, never a second lookup of its temporary name.
    // Ancestors remain pinned; this same-directory rename cannot copy across volumes.
    if (!SetFileInformationByHandle(temporary.file.value, FileRenameInfo, rename, static_cast<DWORD>(rename_bytes))) {
      return internal::WindowsError(error, "Cannot atomically publish preset");
    }
    temporary.published = true;
    if (error != nullptr) {
      error->clear();
    }
    return true;
  } catch (const std::invalid_argument& exception) {
    return internal::Fail(error, exception.what());
  } catch (const std::exception&) {
    return internal::Fail(error, "Cannot write preset.");
  }
}

inline bool WriteFile(const std::filesystem::path& absolute_path, const Document& document,
                      bool overwrite = false, std::string* error = nullptr) {
  try {
    if (!internal::IsFilename(absolute_path.filename())) return internal::Fail(error, "Unsafe preset filename or extension.");
    return WriteJsonFile(absolute_path, Serialize(document), overwrite,
                         [](const Json& root, std::string* message) {
                           Document parsed;
                           return Parse(root.dump(), &parsed, message);
                         }, error);
  } catch (const std::exception& exception) {
    return internal::Fail(error, exception.what());
  }
}

// Call on user refresh/workers, never every frame. The cap counts all directory
// entries, including unrelated files; a truncated scan is reported as failure.
inline bool List(std::vector<std::filesystem::path>* filenames, std::string* error = nullptr) {
  if (filenames == nullptr) return internal::Fail(error, "Missing filename list output.");
  try {
    std::filesystem::path library;
    if (!LibraryPath(&library, error)) return false;
    std::vector<internal::Handle> ancestors;
    if (!internal::OpenDirectory(library, false, &ancestors, error)) return false;
    WIN32_FIND_DATAW entry{};
    const HANDLE search = FindFirstFileW((library / L"*").c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) return internal::WindowsError(error, "Cannot scan preset library");
    struct Search {
      HANDLE value;
      ~Search() { FindClose(value); }
    } guard{.value = search};
    std::vector<std::filesystem::path> result;
    std::size_t count = 0;
    do {
      if (std::wstring_view(entry.cFileName) == L"." || std::wstring_view(entry.cFileName) == L"..") continue;
      if (++count > MAX_LIBRARY_ENTRIES) return internal::Fail(error, "Preset library exceeds the 1024-entry scan limit.");
      if ((entry.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) continue;
      if (internal::IsFilename(entry.cFileName)) {
        result.emplace_back(entry.cFileName);
      }
    } while (FindNextFileW(search, &entry));
    if (GetLastError() != ERROR_NO_MORE_FILES) return internal::WindowsError(error, "Cannot finish preset library scan");
    std::sort(result.begin(), result.end());
    *filenames = std::move(result);
    if (error != nullptr) {
      error->clear();
    }
    return true;
  } catch (const std::exception&) {
    return internal::Fail(error, "Cannot list presets.");
  }
}

inline bool Delete(const std::filesystem::path& filename, std::string* error = nullptr) {
  try {
    if (!internal::IsFilename(filename)) return internal::Fail(error, "Delete requires one safe preset filename.");
    std::filesystem::path library;
    if (!LibraryPath(&library, error)) return false;
    std::vector<internal::Handle> ancestors;
    if (!internal::OpenDirectory(library, false, &ancestors, error)) return false;
    internal::Handle file;
    if (!internal::OpenRegularFile(library / filename, DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, &file, error)) return false;
    FILE_DISPOSITION_INFO disposition{.DeleteFile = TRUE};
    // Delete the inspected object by handle, avoiding a second pathname lookup.
    if (!SetFileInformationByHandle(file.value, FileDispositionInfo, &disposition, sizeof(disposition))) {
      return internal::WindowsError(error, "Cannot delete preset");
    }
    if (error != nullptr) {
      error->clear();
    }
    return true;
  } catch (const std::exception&) {
    return internal::Fail(error, "Cannot delete preset.");
  }
}

}  // namespace renodx::addons::dlss5::presets
