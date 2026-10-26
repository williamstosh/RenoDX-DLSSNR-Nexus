/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#include <Windows.h>

#include <atomic>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef PRESET_STORE_TEST_MODULE
namespace {
bool inspect_publication = false;
bool exclusive_source = true;
unsigned inspected_publications = 0;

BOOL WINAPI InspectSetFileInformationByHandle(HANDLE file, FILE_INFO_BY_HANDLE_CLASS kind,
                                             LPVOID information, DWORD size) {
  if (inspect_publication && kind == FileRenameInfo) {
    ++inspected_publications;
    std::wstring source(32768, L'\0');
    const DWORD length = GetFinalPathNameByHandleW(file, source.data(), static_cast<DWORD>(source.size()), 0);
    if (length == 0 || length >= source.size()) {
      exclusive_source = false;
    } else {
      source.resize(length);
      const auto displaced = source + L".swapped";
      if (MoveFileExW(source.c_str(), displaced.c_str(), 0)) {
        exclusive_source = false;
        MoveFileExW(displaced.c_str(), source.c_str(), 0);
      }
      const HANDLE writer = CreateFileW(source.c_str(), GENERIC_WRITE | DELETE,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                         nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (writer != INVALID_HANDLE_VALUE) {
        exclusive_source = false;
        CloseHandle(writer);
      }
    }
  }
  return ::SetFileInformationByHandle(file, kind, information, size);
}
}  // namespace

#define SetFileInformationByHandle InspectSetFileInformationByHandle
#endif
#include <preset_store.hpp>
#ifndef PRESET_STORE_TEST_MODULE
#undef SetFileInformationByHandle
#endif

namespace presets = renodx::addons::dlss5::presets;
using Json = presets::Json;
namespace fs = std::filesystem;

#ifdef PRESET_STORE_TEST_MODULE

// The DLL owns its CRT, JSON values and header statics. Only a boolean crosses
// the module boundary; disk storage is the shared contract.
extern "C" __declspec(dllexport) bool PresetModuleRoundTrip() {
  fs::path library;
  presets::Document document;
  if (!presets::LibraryPath(&library) || !presets::ReadFile(library / L"from-exe.nrnexus.json", &document)) return false;
  if (document.settings.value("precision", 0.0) != 1.2345678901234567) return false;
  document.metadata["name"] = "From module";
  document.settings["module"] = true;
  return presets::WriteFile(library / L"from-dll.nrnexus.json", document);
}

#else

namespace {

std::size_t checks = 0;

void Require(bool condition, const std::string& message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}

presets::Document Sample() {
  return {.metadata = {{"name", "Synthetic preset"}, {"author", "Synthetic author"},
                       {"description", "Synthetic data only"}, {"source_game", "Synthetic game"},
                       {"created_with", "0.1.0"}, {"tags", {"test", "quality"}}},
          .scope = presets::Scope::QUALITY_MULTIPASS,
          .settings = {{"precision", 1.2345678901234567}, {"future-setting", true},
                       {"UiTheme", "cyber-neon"}, {"Language", "zh-CN"}}};
}

std::string Bytes(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("Cannot read test artifact.");
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void Put(const fs::path& path, std::string_view text) {
  std::ofstream stream(path, std::ios::binary);
  stream.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!stream) throw std::runtime_error("Cannot write test artifact.");
}

void Reject(std::string_view text) {
  auto output = Sample();
  const auto before = presets::Serialize(output);
  std::string error;
  Require(!presets::Parse(text, &output, &error), "Malicious/invalid preset was accepted.");
  Require(!error.empty(), "Parse failure must explain its cause.");
  Require(presets::Serialize(output) == before, "Parse failure changed existing settings.");
}

void CodecTests() {
  std::string error = "stale error";
  auto document = Sample();
  presets::Document parsed;
  const auto text = presets::Serialize(document);
  Require(presets::Parse(text, &parsed, &error) && error.empty(), "Valid schema round trip.");
  Require(parsed.metadata == document.metadata && parsed.settings == document.settings
              && parsed.scope == document.scope, "Round trip preserves all fields and unknown settings.");
  const auto root = Json::parse(text);
  Require(root["format"] == "nr-nexus-preset" && root["schema_version"] == 1
              && root["scope"] == "quality-multipass", "Wire format identifiers.");
  for (const auto scope : {presets::Scope::QUALITY, presets::Scope::QUALITY_MULTIPASS, presets::Scope::FULL}) {
    document.scope = scope;
    Require(presets::Parse(presets::Serialize(document), &parsed) && parsed.scope == scope, "Scope round trip.");
  }
  for (const auto value : {0.0, -0.0, 0.1, 1.2345678901234567, std::numeric_limits<double>::denorm_min(),
                           std::numeric_limits<double>::min(), std::numeric_limits<double>::max(),
                           -std::numeric_limits<double>::max(), 9007199254740991.0}) {
    document.settings = {{"float", value}};
    Require(presets::Parse(presets::Serialize(document), &parsed), "Binary64 parses after serialization.");
    Require(std::bit_cast<std::uint64_t>(value)
                == std::bit_cast<std::uint64_t>(parsed.settings["float"].get<double>()), "Binary64 precision/sign preserved.");
  }
  document.settings = {{"unsigned", std::numeric_limits<std::uint64_t>::max()},
                       {"signed", std::numeric_limits<std::int64_t>::min()}};
  Require(presets::Parse(presets::Serialize(document), &parsed) && parsed.settings == document.settings,
          "64-bit integers are not narrowed.");
  for (const auto invalid : {"", "null", "[]", "{}", "true", "{", "{}{}", "/*comment*/{}"}) {
    Reject(invalid);
  }
  for (const auto field : {"format", "schema_version", "scope", "settings"}) {
    auto altered = root;
    altered.erase(field);
    Reject(altered.dump());
  }
  for (const auto version : {Json(0), Json(2), Json(-1), Json(1.0), Json("1"), Json(true), Json(nullptr),
                              Json(std::numeric_limits<std::uint64_t>::max())}) {
    auto altered = root;
    altered["schema_version"] = version;
    Reject(altered.dump());
  }
  for (const auto field : {"format", "scope"}) {
    for (const auto value : {Json("future"), Json(1), Json(nullptr)}) {
      auto altered = root;
      altered[field] = value;
      Reject(altered.dump());
    }
  }
  auto altered = root;
  altered["path"] = "C:/synthetic/private";
  Reject(altered.dump());
  altered = root;
  altered["metadata"]["private_path"] = "synthetic";
  Reject(altered.dump());
  altered = root;
  altered.erase("metadata");
  Require(presets::Parse(altered.dump(), &parsed) && parsed.metadata == Json::object(), "Metadata is optional.");
  for (const auto bad : {Json(nullptr), Json::array(), Json("text"), Json(1)}) {
    altered = root;
    altered["metadata"] = bad;
    Reject(altered.dump());
    altered = root;
    altered["settings"] = bad;
    Reject(altered.dump());
  }
  for (const auto& [key, limit] : std::vector<std::pair<std::string, std::size_t>>{
           {"name", 128}, {"author", 128}, {"description", 2048}, {"source_game", 128}, {"created_with", 4096}}) {
    altered = root;
    altered["metadata"][key] = std::string(limit, 'x');
    Require(presets::Parse(altered.dump(), &parsed), "Metadata boundary is allowed.");
    altered["metadata"][key] = std::string(limit + 1, 'x');
    Reject(altered.dump());
    altered["metadata"][key] = true;
    Reject(altered.dump());
  }
  altered = root;
  altered["metadata"]["tags"] = std::vector<std::string>(16, std::string(64, 'x'));
  Require(presets::Parse(altered.dump(), &parsed), "16 tags of 64 bytes are allowed.");
  altered["metadata"]["tags"].push_back("x");
  Reject(altered.dump());
  altered["metadata"]["tags"] = {std::string(65, 'x')};
  Reject(altered.dump());
  altered["metadata"]["tags"] = {true};
  Reject(altered.dump());
  altered["metadata"]["tags"] = "tag";
  Reject(altered.dump());
  altered = root;
  altered["settings"] = Json::object();
  Require(presets::Parse(altered.dump(), &parsed) && parsed.settings.empty(), "Explicit empty settings remains empty.");
  for (std::size_t index = 0; index < presets::MAX_SETTINGS; ++index) {
    altered["settings"]["unknown-" + std::to_string(index)] = true;
  }
  Require(presets::Parse(altered.dump(), &parsed) && parsed.settings.size() == 512, "512 unknown settings are retained.");
  altered["settings"]["one-too-many"] = false;
  Reject(altered.dump());
  altered = root;
  altered["settings"] = {{std::string(4096, 'k'), std::string(128, 'v')}};
  Require(presets::Parse(altered.dump(), &parsed), "Key/value string boundaries.");
  altered["settings"] = {{std::string(4097, 'k'), true}};
  Reject(altered.dump());
  for (const auto value : {Json(nullptr), Json::object(), Json::array(), Json(std::string(129, 'x'))}) {
    altered = root;
    altered["settings"]["future-key"] = value;
    Reject(altered.dump());
  }
  for (const auto duplicate : {
           R"({"format":"nr-nexus-preset","schema_version":1,"scope":"quality","scope":"full","settings":{}})",
           R"({"format":"nr-nexus-preset","schema_version":1,"scope":"quality","settings":{"x":1,"x":2}})",
           R"({"format":"nr-nexus-preset","schema_version":1,"scope":"quality","settings":{"x":1,"\u0078":2}})",
           R"({"format":"nr-nexus-preset","schema_version":1,"scope":"quality","metadata":{"name":"a","name":"b"},"settings":{}})",
           R"({"x":{"nested":1,"nested":2}})"}) {
    Reject(duplicate);
  }
  Reject(std::string(10000, '[') + std::string(10000, ']'));
  Reject(std::string(8, '[') + "{}" + std::string(8, ']'));
  for (const auto number : {"NaN", "Infinity", "-Infinity", "1e400", "-1e400"}) {
    Reject(std::string(R"({"format":"nr-nexus-preset","schema_version":1,"scope":"quality","settings":{"n":)")
           + number + "}}");
  }
  auto invalid_utf8 = text;
  invalid_utf8.insert(invalid_utf8.find("Synthetic"), 1, '\xff');
  Reject(invalid_utf8);
  Reject(std::string("\0", 1) + text);
  std::string padded = text;
  padded.resize(presets::MAX_FILE_BYTES, ' ');
  Require(presets::Parse(padded, &parsed), "Exactly 256 KiB is accepted.");
  padded.push_back(' ');
  Reject(padded);
  Require(!presets::Parse(text, nullptr, &error) && !error.empty(), "Null Parse output is rejected.");
  for (const auto number : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                            -std::numeric_limits<double>::infinity()}) {
    document = Sample();
    document.settings["bad"] = number;
    bool threw = false;
    try {
      presets::Serialize(document);
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "Serialize rejects nonfinite values before JSON can convert them to null.");
  }
  for (int index = 0; index < 4; ++index) {
    document = Sample();
    if (index == 0) {
      document.metadata["private"] = "synthetic";
    }
    if (index == 1) {
      document.scope = static_cast<presets::Scope>(99);
    }
    if (index == 2) {
      document.settings["bad"] = Json::array();
    }
    if (index == 3) {
      document.settings = {{std::string(4096, 'k'), true}};
    }
    if (index == 3) {
      for (int key = 0; key < 70; ++key) {
        document.settings[std::to_string(key) + std::string(4000, 'k')] = true;
      }
    }
    bool threw = false;
    try {
      presets::Serialize(document);
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "Serialize enforces schema and output size.");
  }
}

void FileTests(const fs::path& directory, const fs::path& module) {
  const auto local = directory / L"local-appdata";
  Require(SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str()) != 0, "Set controlled LOCALAPPDATA.");
  fs::path library;
  std::string error;
  Require(presets::LibraryPath(&library, &error), "Create controlled library: " + error);
  Require(library == local / L"NR Nexus" / L"Presets", "Library location is independent of game directory.");
  fs::path filename;
  for (const auto name : {"ordinary", "../escape", "C:\\escape:stream", "CON", "NUL", "COM1", "LPT9", "", "...", "with space "}) {
    Require(presets::MakeFilename(name, &filename, &error) && error.empty(), "Generate safe filename.");
    Require(filename == filename.filename() && filename.extension() == L".json", "Generated filename is a single component.");
    Require(presets::WriteFile(library / filename, Sample(), false, &error), "Generated name writes safely: " + error);
    Require(presets::Delete(filename, &error), "Generated name deletes safely: " + error);
  }
  Require(presets::MakeFilename("画质预设😀", &filename) && filename.native().find(L"画质预设") != std::wstring::npos,
          "Safe names retain Unicode.");
  Require(presets::MakeFilename(std::string(4096, 'x'), &filename) && filename.native().size() <= 140, "Long names are bounded.");
  const auto old_filename = filename;
  Require(!presets::MakeFilename(std::string(1, '\xff'), &filename, &error) && filename == old_filename,
          "Invalid name UTF-8 preserves output.");
  Require(!presets::MakeFilename("x", nullptr, &error), "Null filename output.");
  Require(!presets::LibraryPath(nullptr, &error), "Null library output.");
  Require(!presets::List(nullptr, &error), "Null List output.");
  std::vector<fs::path> files{L"sentinel"};
  Require(presets::List(&files, &error) && files.empty(), "Empty library scan.");
  auto document = Sample();
  const auto target = library / L"from-exe.nrnexus.json";
  inspect_publication = true;
  Require(presets::WriteFile(target, document, false, &error) && error.empty(), "Initial atomic write: " + error);
  inspect_publication = false;
  Require(inspected_publications == 1 && exclusive_source, "Publish uses verified handle and denies temporary rename/write replacement.");
  const auto original = Bytes(target);
  auto changed = document;
  changed.settings["revision"] = 2;
  Require(!presets::WriteFile(target, changed, false, &error) && !error.empty(), "Conflict requires explicit overwrite.");
  Require(Bytes(target) == original, "Conflict preserves existing bytes.");
  presets::Document parsed;
  Require(presets::ReadFile(target, &parsed, &error) && error.empty() && parsed.settings == document.settings, "Read local preset.");
  const auto dll = LoadLibraryW(module.c_str());
  Require(dll != nullptr, "Load separately compiled test module.");
  const auto round_trip = reinterpret_cast<bool (*)()>(GetProcAddress(dll, "PresetModuleRoundTrip"));
  const bool module_passed = round_trip != nullptr && round_trip();
  FreeLibrary(dll);
  Require(module_passed, "Independent DLL reads EXE preset and writes to same controlled library.");
  Require(presets::ReadFile(library / L"from-dll.nrnexus.json", &parsed, &error)
              && parsed.settings["module"] == true, "EXE reads preset from independent module.");
  const auto external = directory / L"external";
  fs::create_directory(external);
  Require(presets::WriteFile(external / L"export.nrnexus.json", changed, false, &error), "Explicit external absolute export.");
  Put(external / L"import.json", original);
  Require(presets::ReadFile(external / L"import.json", &parsed, &error), "Explicit external absolute import.");
  const auto before_read = presets::Serialize(parsed);
  Put(external / L"invalid.json", "{}");
  Require(!presets::ReadFile(external / L"invalid.json", &parsed, &error) && !error.empty()
              && presets::Serialize(parsed) == before_read, "Invalid file leaves settings unchanged.");
  Put(external / L"oversized.json", std::string(presets::MAX_FILE_BYTES + 1, ' '));
  Require(!presets::ReadFile(external / L"oversized.json", &parsed, &error), "Oversized file rejected before parse.");
  Require(!presets::ReadFile(target, nullptr, &error), "Null ReadFile output.");
  for (const auto& unsafe : std::vector<fs::path>{L"relative.nrnexus.json", library / L".." / L"escape.nrnexus.json",
           library / L"bad.nrnexus.json:stream", library / L"CON.nrnexus.json", library / L"bad.nrnexus.json.",
           library / L"bad.nrnexus.json ", L"\\\\?\\C:\\bad.nrnexus.json", L"C:relative.nrnexus.json"}) {
    Require(!presets::WriteFile(unsafe, document, false, &error) && !error.empty(), "Unsafe write path is rejected.");
    Require(!presets::ReadFile(unsafe, &parsed, &error) && !error.empty(), "Unsafe read path is rejected.");
  }
  for (const auto& unsafe : std::vector<fs::path>{target, L"..\\escape.nrnexus.json", L"sub/name.nrnexus.json",
                                                L"CON.nrnexus.json", L"file.json", L"x.nrnexus.json:stream"}) {
    Require(!presets::Delete(unsafe, &error) && !error.empty(), "Delete accepts only safe library filename.");
  }
  Require(!presets::WriteFile(library / L"wrong.json", document, false, &error), "Export requires preset extension.");
  Require(!presets::WriteFile(directory / L"missing" / L"x.nrnexus.json", document, false, &error), "Export does not create arbitrary directories.");
  const auto previous_file = Bytes(target);
  changed.settings["bad"] = std::numeric_limits<double>::infinity();
  Require(!presets::WriteFile(target, changed, true, &error) && Bytes(target) == previous_file,
          "Invalid document cannot damage existing file.");
  changed.settings.erase("bad");
  const bool overwritten = presets::WriteFile(target, changed, true, &error);
  Require(overwritten, "Explicit overwrite: " + error);
  Require(presets::ReadFile(target, &parsed)
              && parsed.settings == changed.settings, "Explicit overwrite publishes complete document.");
  Put(library / L"unrelated.txt", "synthetic");
  fs::create_directory(library / L"directory.nrnexus.json");
  Require(presets::List(&files, &error) && files == std::vector<fs::path>{L"from-dll.nrnexus.json", L"from-exe.nrnexus.json"},
          "List returns sorted filenames, excludes unrelated files and directories.");

  // Hold the actual destination without delete sharing to force rename failure
  // after the temporary file has been written, flushed and validated.
  HANDLE pinned = CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  Require(pinned != INVALID_HANDLE_VALUE, "Pin destination for failure test.");
  const bool blocked = !presets::WriteFile(target, document, true, &error);
  CloseHandle(pinned);
  Require(blocked && Bytes(target) == presets::Serialize(changed), "Failed atomic rename preserves old content.");
  for (const auto& entry : fs::directory_iterator(library)) {
    Require(entry.path().extension() != L".tmp", "Failed write cleans its temporary file.");
  }
  const auto cleanup_original = external / L"cleanup.tmp";
  const auto cleanup_moved = external / L"cleanup-moved.tmp";
  {
    presets::internal::TemporaryFile temporary;
    temporary.file = presets::internal::Handle(CreateFileW(cleanup_original.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    Require(temporary.file.value != INVALID_HANDLE_VALUE, "Create synthetic movable cleanup handle.");
    Require(MoveFileExW(cleanup_original.c_str(), cleanup_moved.c_str(), 0) != 0, "Relocate cleanup fixture by its old path.");
    Put(cleanup_original, "replacement sentinel");
  }
  Require(!fs::exists(cleanup_moved) && Bytes(cleanup_original) == "replacement sentinel",
          "Cleanup deletes held object and preserves replacement at its former pathname.");

  std::atomic<bool> running = true;
  std::atomic<std::size_t> valid_reads = 0;
  std::atomic<bool> partial = false;
  std::thread reader([&]() {
    while (running.load()) {
      presets::Document observed;
      std::string read_error;
      if (presets::ReadFile(target, &observed, &read_error)) {
        if (observed.settings != document.settings && observed.settings != changed.settings) {
          partial.store(true);
        }
        valid_reads.fetch_add(1);
      } else if (read_error.find("Cannot open preset file") == std::string::npos) {
        partial.store(true);
      }
    }
  });
  std::size_t writes = 0;
  for (int iteration = 0; iteration < 100; ++iteration) {
    if (presets::WriteFile(target, (iteration % 2 == 0 ? document : changed), true, &error)) {
      ++writes;
    }
    std::this_thread::yield();
  }
  running.store(false);
  reader.join();
  Require(writes > 0 && valid_reads.load() > 0 && !partial.load(), "Concurrent readers see only complete documents.");
  const auto contested = library / L"contested.nrnexus.json";
  std::atomic<int> winners = 0;
  const auto compete = [&]() {
    if (presets::WriteFile(contested, document)) {
      winners.fetch_add(1);
    }
  };
  std::thread first(compete);
  std::thread second(compete);
  first.join();
  second.join();
  Require(winners.load() == 1 && presets::ReadFile(contested, &parsed), "Concurrent non-overwrite has exactly one winner.");
  Require(presets::Delete(L"contested.nrnexus.json", &error) && !fs::exists(contested), "Delete removes selected library file.");
  Require(!presets::Delete(L"contested.nrnexus.json", &error) && !error.empty(), "Missing delete is an explicit failure.");

  const auto directory_link = directory / L"ancestor-link";
  const auto file_link = library / L"linked.nrnexus.json";
  Require(fs::is_directory(directory_link) && (GetFileAttributesW(directory_link.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0,
          "Runner prepared a real junction fixture.");
  Require(!presets::ReadFile(directory_link / L"outside.nrnexus.json", &parsed, &error), "Import rejects ancestor junction.");
  Require(!presets::WriteFile(directory_link / L"new.nrnexus.json", document, false, &error), "Export rejects ancestor junction.");
  Require(!presets::ReadFile(file_link, &parsed, &error), "Import rejects final reparse point.");
  Require(!presets::WriteFile(file_link, document, true, &error), "Overwrite rejects final reparse point.");
  Require(!presets::Delete(file_link.filename(), &error), "Delete rejects final reparse point.");
  Require(presets::List(&files, &error)
              && std::find(files.begin(), files.end(), file_link.filename()) == files.end(), "Scan never follows or returns reparse points.");
  const auto hard_link = library / L"hard-linked.nrnexus.json";
  Require(CreateHardLinkW(hard_link.c_str(), (external / L"export.nrnexus.json").c_str(), nullptr) != 0, "Create hardlink fixture.");
  Require(!presets::ReadFile(hard_link, &parsed, &error) && !presets::WriteFile(hard_link, document, true, &error)
              && !presets::Delete(hard_link.filename(), &error), "Hard-linked files cannot alias external data.");
  Require(DeleteFileW(hard_link.c_str()) != 0, "Remove this test's hardlink only.");
  Require(Bytes(directory / L"outside" / L"outside.nrnexus.json") == "synthetic outside sentinel", "Outside fixture remains intact.");
  Require(!fs::exists(directory / L"outside" / L"new.nrnexus.json"), "Rejected junction export created nothing outside.");
  auto old_path = library;
  Require(SetEnvironmentVariableW(L"LOCALAPPDATA", directory_link.c_str()) != 0, "Set junction library root.");
  Require(!presets::LibraryPath(&old_path, &error) && old_path == library, "Library rejects ancestor reparse without changing output.");
  files = {L"sentinel"};
  Require(!presets::List(&files, &error) && files == std::vector<fs::path>{L"sentinel"}, "Failed scan preserves output.");
  Require(!presets::Delete(L"outside.nrnexus.json", &error), "Delete rejects junction library root.");
  for (const auto invalid : {L"relative", L"C:\\synthetic\\..\\private", L"\\\\server\\share", L"C:\\synthetic.\\private"}) {
    SetEnvironmentVariableW(L"LOCALAPPDATA", invalid);
    Require(!presets::LibraryPath(&old_path, &error) && !error.empty(), "Invalid environment root is rejected.");
  }
  SetEnvironmentVariableW(L"LOCALAPPDATA", nullptr);
  Require(!presets::LibraryPath(&old_path, &error), "Absent LOCALAPPDATA is rejected.");
  SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
  for (std::size_t index = 0; index < presets::MAX_LIBRARY_ENTRIES; ++index) {
    Put(library / ("scan-" + std::to_string(index) + ".txt"), "");
  }
  files = {L"sentinel"};
  Require(!presets::List(&files, &error) && !error.empty() && files == std::vector<fs::path>{L"sentinel"},
          "Scan cap counts unrelated entries and never returns a partial list.");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    Require(argc == 3, "Pass fixture directory and independent module path.");
    const fs::path directory(argv[1]);
    Require(directory.is_absolute() && fs::is_directory(directory), "Use an existing isolated absolute fixture directory.");
    CodecTests();
    FileTests(directory, fs::path(argv[2]));
    std::cout << "Preset codec/store: " << checks << " checks passed; EXE/DLL sharing and reparse rejection passed.\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "Preset test failed after " << checks << " checks: " << exception.what() << '\n';
    return 1;
  }
}

#endif
