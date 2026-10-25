/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// The issue report (rc11): "I'm having issues..." in the panel.
//
// Included by dlssnr.hpp INSIDE namespace internal, just before
// DrawHealthIssues, so it sees the support report, the verdict, the health
// ledger and the F5 capture.  It is not a standalone header.
//
// What the player sees, in the panel:
//   1. a description, reproduction steps and compatible category slugs;
//   2. optional capture while the game keeps running, with a progress bar
//      and a timeout - a report never waits forever on a capture: it bundles
//      what exists and states why the capture is missing;
//   3. a review of every file with its size and a checkbox, and the consent
//      statement (which host, that anyone with the link can download it, when
//      it is deleted); [Upload & copy link] [Save zip only] [Cancel];
//   4. optional upload, followed by a separately reviewed public GitHub draft.
// Nothing leaves the PC without the Upload click.
//
// Threads: the panel only reads and writes the flow's state under
// issue_report::mutex, a leaf lock never taken with runtime_mutex or
// screenshot's state_mutex.  Capture waits, file IO, the zip and the upload
// run on a worker thread that holds a reference on this module and ends
// through FreeLibraryAndExitThread (OpenLogFolderMain's pattern), so an
// addon unload cannot pull the code out from under it.
//
// Every step states one line: NR-REPORT v1 begin|capture|review|attempt|
// outcome ..., one grammar for support to grep.  No line ever carries the
// link's path: that is the secret (report::LinkForLog).
//
// The panel's strings use the existing i18n regions and ui/lang tables,
// and drawn through ui::Tr since v8.5.0-rc10; through rc9 they were English
// in every language.  Slugs and zip entry names on those lines are tagged
// "i18n: skip", and CaptureNote's result codes are comparisons.
//
// RENODX_NR_TEST_REPORT=<reason> (test only) clicks through the flow by
// itself: begin at present 60, continue, and upload from the review step.
// RENODX_NR_TEST_REPORT_URL replaces every host with a loopback URL (any
// other host is refused), and RENODX_NR_TEST_REPORT_TIMEOUT_MS shortens the
// capture timeout.  Unset, which is every shipped session, none of this is
// registered.

#pragma comment(lib, "advapi32.lib")

namespace issue_report {

enum class Step : uint8_t {
  kClosed = 0,
  kDescribe,    // step 1, in the panel
  kCapturing,   // worker: the F5 capture
  kCollecting,  // worker: logs, redaction, report.json
  kReview,      // step 3, in the panel
  kPacking,     // worker: the zip
  kUploading,   // worker: the host
  kOpeningIssue,
  kCleaning,
  kDone,        // step 4, in the panel
};

inline constexpr int64_t kCaptureTimeoutMs = 20'000;
// A log longer than this is sent as its last kTextTailBytes.
inline constexpr size_t kTextTailBytes = 16u << 20;

struct Tick {
  const char* slug;
  const char* label;
};
// i18n: begin - display strings (tools/i18n/ui_strings.py catalog)
inline constexpr Tick kTicks[] = {
    {"flicker",  // i18n: skip
     ui::text::kReportVisual},
    {"colours",  // i18n: skip
     ui::text::kReportColour},
    {"crash",  // i18n: skip
     "Crash or freeze"},
    {"fps",  // i18n: skip
     "Low fps"},
    {"nr_not_working",  // i18n: skip
     "NR not working"},
    {"other",  // i18n: skip
     "Something else"},
    {"native_fg", ui::text::kReportNativeFg},
    {"xefg", ui::text::kReportXefg},
    {"compatibility", ui::text::kReportCompatibility},
};
// i18n: end

struct Item {
  const char* label;
  std::string entry;           // the name inside the zip
  std::filesystem::path file;  // staged (redacted) copy, or the capture's own PNG
  uint64_t bytes = 0;
  bool include = true;
  bool required = false;
  std::string preview;
  bool image = false;
};

inline std::mutex mutex;
// Under `mutex`.
inline std::string reason = "button";
inline std::string description;
inline uint32_t ticks = 0;  // one bit per kTicks entry
inline std::string capture_result;
inline std::vector<Item> items;
inline std::string summary;  // valid redacted JSON; send appends left_out structurally
inline std::filesystem::path stage;
inline std::filesystem::path zip;
inline std::filesystem::path preview_file;
inline std::string link;  // the secret: the panel and the clipboard only
inline std::string error;
inline std::string host_display;
inline std::string reproduction;
inline std::string public_summary;
inline std::string issue_body;
inline std::string issue_notice;
inline bool upload_consent = false;
inline bool public_link_consent = false;
inline bool public_summary_consent = false;
inline bool link_copied = false;
inline bool capture_requested = false;
inline bool feature_request = false;
inline bool force_issue_fallback = false;
inline std::atomic_bool worker_active{false};
inline std::string host_setting = report::kDefaultHosts;  // NRReportHost

inline std::atomic<Step> step{Step::kClosed};
inline std::atomic_bool cancel{false};
inline std::atomic_bool skip_capture{false};
inline std::atomic_bool upload{false};
inline std::atomic_int64_t capture_started_ns{0};
inline report::UploadProgress progress;

// The panel's own copy while the player types; present thread only.
inline char typed[256] = {};
inline char typed_steps[2048] = {};
inline bool typed_capture = false;
inline bool typed_feature = false;
inline uint32_t typed_ticks = 0;

// Test only; set at attach, then read-only.
inline std::string test_reason;
inline std::wstring test_url;
inline int64_t test_timeout_ms = 0;
inline uint32_t test_stall_from = 0;

}  // namespace issue_report

// Opens the report's first step, as if the player had pressed "I'm having
// issues...".  reason_slug names what asked: "button", a health ledger row's
// slug, "test".  Safe from any thread (the flow's leaf lock only); false
// while a report is already under way, which the panel then shows.
inline bool BeginIssueReport(const char* reason_slug) {
  using issue_report::Step;
  if (issue_report::worker_active.load()) return false;
  Step expected = Step::kClosed;
  if (!issue_report::step.compare_exchange_strong(expected, Step::kDescribe)) {
    expected = Step::kDone;
    if (!issue_report::step.compare_exchange_strong(expected, Step::kDescribe)) return false;
  }
  issue_report::cancel = false;
  issue_report::skip_capture = false;
  issue_report::upload = false;
  std::string reason = reason_slug != nullptr && reason_slug[0] != '\0' ? reason_slug : "button";
  {
    std::lock_guard lock(issue_report::mutex);
    issue_report::reason = reason;
    issue_report::items.clear();
    issue_report::capture_result.clear();
    issue_report::link.clear();
    issue_report::error.clear();
    issue_report::zip.clear();
    issue_report::stage.clear();
    issue_report::summary.clear();
    issue_report::public_summary.clear();
    issue_report::issue_body.clear();
    issue_report::issue_notice.clear();
    issue_report::description.clear();
    issue_report::reproduction.clear();
    issue_report::upload_consent = false;
    issue_report::public_link_consent = false;
    issue_report::public_summary_consent = false;
    issue_report::link_copied = false;
    issue_report::capture_requested = false;
    issue_report::feature_request = false;
    issue_report::typed_capture = false;
    issue_report::typed_feature = false;
  }
  Log(reshade::log::level::info, "NR-REPORT v1 begin reason=" + reason);
  return true;
}

// Shows the zip in Explorer (selected), from a thread that is not the
// game's: ShellExecute loads shell extensions.  Never in a test session.
inline void ShowReportZip() {
  std::filesystem::path zip;
  {
    std::lock_guard lock(issue_report::mutex);
    zip = issue_report::zip;
  }
  if (zip.empty() || !issue_report::test_reason.empty()) return;
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  const std::wstring select = L"/select,\"" + zip.wstring() + L"\"";
  ShellExecuteW(nullptr, L"open", L"explorer.exe", select.c_str(), nullptr, SW_SHOWNORMAL);
  if (SUCCEEDED(com)) CoUninitialize();
}

inline DWORD WINAPI ShowReportZipMain(void* module) {
  ShowReportZip();
  issue_report::worker_active = false;
  FreeLibraryAndExitThread(static_cast<HMODULE>(module), 0);
}

inline bool CopyReportText(std::string_view text) {
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                        static_cast<int>(text.size()), nullptr, 0);
  if (length <= 0) return false;
  const HWND owner = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
                                     HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (owner == nullptr) return false;
  if (!OpenClipboard(owner)) { DestroyWindow(owner); return false; }
  bool copied = false;
  if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (static_cast<size_t>(length) + 1) * sizeof(wchar_t))) {
    if (auto* wide = static_cast<wchar_t*>(GlobalLock(memory))) {
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide, length);
      wide[length] = L'\0';
      GlobalUnlock(memory);
      if (EmptyClipboard()) copied = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
    }
    if (!copied) GlobalFree(memory);
  }
  CloseClipboard();
  DestroyWindow(owner);
  return copied;
}

namespace issue_report {
// Only the test fixture substitutes these OS boundaries; default calls remain on the worker.
inline bool (*copy_draft)(std::string_view) = CopyReportText;
inline decltype(&ShellExecuteW) open_browser = ShellExecuteW;
}  // namespace issue_report

inline std::string FormatBytes(uint64_t bytes) {
  char text[32];
  if (bytes >= (1ull << 20)) {
    std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1 << 20));
  } else {
    std::snprintf(text, sizeof(text), "%.0f KB",
                  std::ceil(static_cast<double>(bytes) / 1024.0));
  }
  return text;
}

// The worker's first job: the F5 capture, then everything the review lists.
inline void RunReportCaptureJob() {
  using issue_report::Step;
  const int64_t started_ns = SteadyNowNs();
  issue_report::capture_started_ns = started_ns;
  screenshot::internal::FinishedCapture capture;
  std::string result = "skipped";
  if (issue_report::capture_requested) {
    const uint64_t armed_at = screenshot::internal::capture_serial.load();
    screenshot::RequestCapture();
    const int64_t timeout_ms = issue_report::test_timeout_ms > 0 ? issue_report::test_timeout_ms
                                                                  : issue_report::kCaptureTimeoutMs;
    result.clear();
    while (result.empty()) {
      const bool prepared = screenshot::internal::capture_serial.load() != armed_at;
      if (screenshot::internal::finished_serial.load(std::memory_order_acquire) > armed_at) {
        std::lock_guard lock(screenshot::internal::finished_mutex);
        capture = screenshot::internal::finished;
        result = capture.outcome;
      } else if (issue_report::cancel || issue_report::skip_capture) {
        if (!prepared) screenshot::Disarm();
        result = issue_report::cancel ? "cancelled" : "skipped";
      } else if (!prepared && !screenshot::IsArmed()) {
        result = "no_nr_frame";  // the arm ran out: no NR evaluation for ~10 s
      } else if (SteadyNowNs() - started_ns >= timeout_ms * 1'000'000) {
        if (!prepared) screenshot::Disarm();
        result = prepared ? "timeout" : "no_nr_frame";
      } else {
        Sleep(50);
      }
    }
  }
  const int64_t capture_ms = (SteadyNowNs() - started_ns) / 1'000'000;
  Log(reshade::log::level::info,
      "NR-REPORT v1 capture result=" + result + " ms=" + std::to_string(capture_ms)
          + " serial=" + std::to_string(capture.serial));
  if (issue_report::cancel) {
    issue_report::step = Step::kClosed;
    Log(reshade::log::level::info, "NR-REPORT v1 outcome=cancelled step=capture");
    return;
  }
  issue_report::step = Step::kCollecting;

  // Everything below is file IO into <ReShade folder>\DLSS5 Reports.
  const report::Redaction redaction = report::SystemRedaction();
  std::filesystem::path base = renodx::utils::path::GetReShadeBasePath();
  if (base.empty()) base = AddonDirectory();
  SYSTEMTIME now{};
  GetLocalTime(&now);
  char stamp[32];
  std::snprintf(stamp, sizeof(stamp), "%04u%02u%02u-%02u%02u%02u", now.wYear, now.wMonth,
                now.wDay, now.wHour, now.wMinute, now.wSecond);
  const std::filesystem::path stage = base / L"DLSS5 Reports" / L".cache"
      / (std::string("report-") + stamp + "-" + std::to_string(GetCurrentProcessId())
         + "-" + std::to_string(GetTickCount64()));
  std::error_code ignored;
  std::filesystem::create_directories(stage);
  {
    std::lock_guard lock(issue_report::mutex);
    issue_report::stage = stage;
  }

  std::vector<issue_report::Item> items;
  const auto read_text = [](const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return std::string();
    const auto size = static_cast<uint64_t>((std::max)(in.tellg(), std::streampos(0)));
    const uint64_t skipped = size > issue_report::kTextTailBytes ? size - issue_report::kTextTailBytes : 0;
    std::string text(static_cast<size_t>(size - skipped), '\0');
    in.seekg(static_cast<std::streamoff>(skipped));
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<size_t>((std::max)(in.gcount(), std::streamsize(0))));
    return skipped == 0 ? text
                        : "[the first " + std::to_string(skipped) + " bytes are left out]\n" + text;
  };
  const auto add_text = [&](const char* label, const std::string& entry, std::string_view text) {
    if (text.empty()) return;
    const std::string redacted = report::RedactText(text, redaction);
    const std::filesystem::path file = stage / std::filesystem::path(entry).filename();
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(redacted.data(), static_cast<std::streamsize>(redacted.size()));
    if (!out.good()) throw std::runtime_error("A report text file could not be written.");
    items.push_back({label, entry, file, redacted.size(), true, false, redacted.substr(0, 65536)});
  };
  const std::string support = BuildSupportReport();
  // i18n: begin - the review step's file labels, drawn through ui::Tr
  add_text("Support report",
           "support-report.txt", support);  // i18n: skip
  // i18n: end
  // ReShade writes ReShade.log beside ReShade.ini, or ReShade.log1..9 when
  // another process holds it: the newest is this session's.
  std::filesystem::path reshade_log;
  std::filesystem::file_time_type newest{};
  for (int suffix = 0; suffix <= 9; ++suffix) {
    const std::filesystem::path candidate =
        base / (suffix == 0 ? std::string("ReShade.log") : "ReShade.log" + std::to_string(suffix));
    const auto written = std::filesystem::last_write_time(candidate, ignored);
    if (!ignored && (reshade_log.empty() || written > newest)) {
      reshade_log = candidate;
      newest = written;
    }
    ignored.clear();
  }
  const std::string log_text = reshade_log.empty() ? std::string() : read_text(reshade_log);
  // i18n: begin - the review step's file labels, drawn through ui::Tr
  add_text("ReShade log",
           "logs/ReShade.log", log_text);  // i18n: skip
  add_text("ReShade settings (ReShade.ini)",
           "logs/ReShade.ini", read_text(base / "ReShade.ini"));  // i18n: skip
  // The last-gasp crash reporter's log (lastgasp.hpp), appended sessions and
  // all.  Skipped when the addon never opened it, so a clean session ships
  // without the entry.
  if (!lastgasp::crash_log_path.empty()) {
    add_text("Crash log (" NR_NEXUS_NAME ")",
             "logs/RenoDX-DLSS5-crash.log",  // i18n: skip
             read_text(std::filesystem::path(lastgasp::crash_log_path)));
  }
  // The capture's PNG when it wrote one, else its JPEG; the entry keeps the
  // extension (ours: .png or .jpg, ASCII).
  const auto add_image = [&](const char* label, const char* entry, const std::filesystem::path& file) {
    const uint64_t bytes = file.empty() ? 0 : std::filesystem::file_size(file, ignored);
    if (!file.empty() && !ignored) {
      items.push_back({label, entry + file.extension().string(), file, bytes, false, false, {}, true});
    }
    ignored.clear();
  };
  add_image(capture.model_diagnostic ? "Model output diagnostic" : "Screenshot, NR on",
            capture.model_diagnostic ? "capture/MODEL_OUTPUT" : "capture/NR_ON",  // i18n: skip
            capture.post_image);
  add_image(capture.model_diagnostic ? "Model input diagnostic" : "Screenshot, NR off",
            capture.model_diagnostic ? "capture/MODEL_INPUT" : "capture/NR_OFF",  // i18n: skip
            capture.pre_image);
  if (!capture.meta.empty()) {
    add_text("Capture details",
             "capture/capture.meta.txt", read_text(capture.meta));  // i18n: skip
  }
  // i18n: end

  // report.json: what support needs first, as data.  Closed by the send
  // job, which appends what the player left out.
  const NrVerdict verdict = ComputeNrVerdict(CollectFunnelInputs());
  const ui::StatusCard card = ui::ComputeStatusCard(CollectCardInputs(verdict, SteadyNowNs()));
  std::string json = "{\n  \"schema\": \"renodx-dlss5-report/1\"";
  const auto field = [&json](const char* key, const std::string& value) {
    json += ",\n  " + report::JsonString(key) + ": " + value;
  };
  std::string ticks = "[";
  std::string description;
  {
    std::lock_guard lock(issue_report::mutex);
    field("reason", report::JsonString(issue_report::reason));
    description = issue_report::description;
    field("reproduction", report::JsonString(issue_report::reproduction));
    for (size_t i = 0; i < std::size(issue_report::kTicks); ++i) {
      if ((issue_report::ticks >> i) & 1u) {
        ticks += (ticks.size() > 1 ? ", " : "") + report::JsonString(issue_report::kTicks[i].slug);
      }
    }
  }
  field("description", report::JsonString(report::RedactText(description, redaction)));
  field("ticks", ticks + "]");
  wchar_t addon_path[MAX_PATH * 4] = {};
  GetModuleFileNameW(addon_module, addon_path, static_cast<DWORD>(std::size(addon_path)));
  field("brand", report::JsonString(NR_NEXUS_NAME));
  field("addon", "{\"version\": " + report::JsonString(kAddonVersion) + ", \"build\": "
                     + report::JsonString(__DATE__ " " __TIME__) + ", \"md5\": "
                     + report::JsonString(report::FileDigestHex(addon_path, BCRYPT_MD5_ALGORITHM))
                     + "}");
  field("game", "{\"exe\": " + report::JsonString(GameExeName()) + ", \"api\": "
                    + report::JsonString(verdict.api) + ", \"path\": "
                    + report::JsonString(verdict.path) + "}");
  field("verdict", "{\"state\": " + report::JsonString(NrStateName(verdict.state))
                       + ", \"stage\": " + report::JsonString(verdict.StageName())
                       + ", \"reason\": " + report::JsonString(verdict.ReasonName())
                       + ", \"injected\": " + std::to_string(verdict.injected)
                       + ", \"eligible\": " + std::to_string(verdict.eligible) + "}");
  field("card", "{\"id\": " + report::JsonString(ui::CardSlug(card.id)) + ", \"tone\": "
                    + report::JsonString(ui::CardToneName(card.tone)) + "}");
  // The health ledger's active rows, log-tone ones included: the panel
  // hides those, support wants them.
  std::string ledger = "[";
  const uint64_t mask = health_active_mask.load(std::memory_order_relaxed);
  for (size_t i = 0; i < health::kRowCount; ++i) {
    if (((mask >> i) & 1u) == 0) continue;
    const health::Row& row = health::kRows[i];
    char value[64];
    std::snprintf(value, sizeof(value), "%.6g",
                  health_row_values[i].load(std::memory_order_relaxed) * row.measure_scale);
    ledger += std::string(ledger.size() > 1 ? ", " : "") + "{\"slug\": " + report::JsonString(row.slug)
              + ", \"tone\": " + report::JsonString(health::ToneName(row.tone))
              + ", \"value\": " + value + "}";
  }
  field("ledger", ledger + "]");
  // The support report's settings/counters lines as objects, its telemetry
  // line as text.
  for (const char* key : {"settings", "counters", "telemetry"}) {
    const std::string prefix = std::string(key) + ": ";
    const size_t at = support.find("\n" + prefix);
    if (at == std::string::npos) continue;
    const size_t begin = at + 1 + prefix.size();
    const std::string line = support.substr(begin, support.find('\n', begin) - begin);
    if (std::strcmp(key, "telemetry") == 0) {
      field(key, report::JsonString(line));
      continue;
    }
    std::string object = "{";
    for (size_t token = 0; token < line.size();) {
      const size_t end = (std::min)(line.find(' ', token), line.size());
      const std::string pair = line.substr(token, end - token);
      const size_t equals = pair.find('=');
      if (equals != std::string::npos) {
        object += (object.size() > 1 ? ", " : "") + report::JsonString(pair.substr(0, equals))
                  + ": " + report::JsonString(pair.substr(equals + 1));
      }
      token = end + 1;
    }
    field(key, object + "}");
  }
  // The machine: first hardware adapter, its driver, the OS build, ReShade.
  std::string gpu;
  std::string driver;
  if (renodx::utils::directx::Initialize() && renodx::utils::directx::pCreateDXGIFactory1 != nullptr) {
    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(renodx::utils::directx::pCreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
      IDXGIAdapter1* adapter = nullptr;
      for (UINT index = 0; gpu.empty() && factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND;
           ++index) {
        DXGI_ADAPTER_DESC1 desc{};
        LARGE_INTEGER umd{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
          gpu = report::Utf8(desc.Description);
          if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
            char text[64];
            const auto high = static_cast<uint32_t>(umd.HighPart);
            const auto low = static_cast<uint32_t>(umd.LowPart);
            // NVIDIA's public number is the last five digits: 32.0.15.8129 is 581.29.
            if (desc.VendorId == 0x10DE) {
              const uint32_t nvidia = (HIWORD(low) % 10) * 10000 + LOWORD(low);
              std::snprintf(text, sizeof(text), "%u.%02u (%u.%u.%u.%u)", nvidia / 100, nvidia % 100,
                            HIWORD(high), LOWORD(high), HIWORD(low), LOWORD(low));
            } else {
              std::snprintf(text, sizeof(text), "%u.%u.%u.%u", HIWORD(high), LOWORD(high),
                            HIWORD(low), LOWORD(low));
            }
            driver = text;
          }
        }
        adapter->Release();
      }
      factory->Release();
    }
  }
  std::string os;
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  if (const auto get_version = reinterpret_cast<RtlGetVersionFn>(
          GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) {
    OSVERSIONINFOW version{sizeof(version)};
    DWORD revision = 0;
    DWORD revision_bytes = sizeof(revision);
    const bool revision_known = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR",
                 RRF_RT_REG_DWORD, nullptr, &revision, &revision_bytes) == ERROR_SUCCESS;
    if (get_version(&version) == 0) {
      os = std::to_string(version.dwMajorVersion) + "." + std::to_string(version.dwMinorVersion)
           + "." + std::to_string(version.dwBuildNumber) + (revision_known ? "." + std::to_string(revision) : "");
    }
  }
  std::string reshade_version;
  if (const size_t at = log_text.find("ReShade version '"); at != std::string::npos) {
    const size_t begin = at + std::strlen("ReShade version '");
    reshade_version = log_text.substr(begin, log_text.find('\'', begin) - begin);
  }
  field("system", "{\"gpu\": " + report::JsonString(gpu) + ", \"driver\": "
                      + report::JsonString(driver) + ", \"os\": " + report::JsonString(os)
                      + ", \"reshade\": " + report::JsonString(reshade_version) + "}");
  const auto& loader = NgxLoaderView();
  field("nr_runtime", "{\"sha256\": " + report::JsonString(loader.nr_sha256)
                          + ", \"reference_match\": "
                          + (loader.nr_sha256 == kExpectedNrSha256 ? "true" : "false")
                          + ", \"file_version\": "
                          + report::JsonString(loader.nr_requirements.file_version) + "}");
  field("capture", "{\"result\": " + report::JsonString(result) + ", \"ms\": "
                       + std::to_string(capture_ms) + ", \"comparison\": "
                       + report::JsonString(result != "written" ? "none"
                             : capture.model_diagnostic ? "model-pass-input-output"
                                                        : "final-present-same-frame")
                       + "}");

  nlohmann::json diagnostics;
  diagnostics["extension_version"] = 2;
  diagnostics["availability"] = {{"gpu", gpu.empty() ? "unavailable" : "collected"},
      {"driver", driver.empty() ? "unavailable" : "collected"}, {"os", os.empty() ? "unavailable" : "collected"},
      {"reshade", reshade_version.empty() ? "unavailable" : "collected"}};
  diagnostics["gpu_source"] = "first_hardware_adapter_not_verified_render_device";
  diagnostics["snapshot"] = "on_demand_independent_counters_not_a_frame_capture";
  diagnostics["nr"] = {{"status", NrStateName(verdict.state)}, {"hook", verdict.path},
      {"passes", stack_passes.load()}, {"input_width", last_input_width.load()},
      {"input_height", last_input_height.load()}, {"output_width", last_output_width.load()},
      {"output_height", last_output_height.load()}, {"resource_details", "unavailable_not_traced"}};
  {
    std::unique_lock lock(runtime_mutex, std::try_to_lock);
    if (lock.owns_lock()) {
      diagnostics["nr"]["resource_details"] = {{"status", "collected"},
          {"live_worksets", worksets.size()}, {"retired_worksets", retired_final_resources.size()},
          {"basis", "metadata_only_no_GPU_reads"}};
    } else {
      diagnostics["nr"]["resource_details"] = {{"status", "unavailable_busy"}};
    }
  }
  const bool native_observed = native_fg_options_generation.load() != 0 || native_fg_submitted_frames.load() != 0;
  diagnostics["native_fg"] = {{"status", "collected_public_state"}, {"owns_stream", native_fg_owns.load()},
      {"declared_off", native_fg_public_off.load()}, {"result_code", static_cast<int>(native_fg_result.load())},
      {"submitted_frames", native_fg_submitted_frames.load()},
      {"mfg_multiplier", {{"status", "unavailable_not_measured"}}}};
  if (!native_observed) {
    diagnostics["native_fg"] = {{"status", "unavailable_not_observed"}};
  }
  diagnostics["xefg"] = {{"status", "collected_public_state"}, {"owns_stream", fg::xefg::OwnsStream()},
      {"submitted_frames", fg::xefg::SubmittedFrames()}};
  {
    std::shared_lock lock(fg::xefg::internal::state.mutex);
    nlohmann::json contexts = nlohmann::json::array();
    for (const auto& [handle, context] : fg::xefg::internal::state.contexts) {
      contexts.push_back({{"enabled", context->enabled}, {"ui_composition", context->ui_composition},
          {"application_frames_in_current_interval", context->interval_presents},
          {"sdk_displayed_frames_in_current_interval", context->interval_displayed},
          {"sdk_status_samples_in_current_interval", context->interval_status_samples}});
    }
    diagnostics["xefg"]["contexts"] = std::move(contexts);
    if (fg::xefg::internal::state.contexts.empty()) {
      diagnostics["xefg"]["status"] = "unavailable_not_observed";
    }
  }
  diagnostics["colour"] = {{"codec_mode", codec_mode.load()}, {"paper_white_scale", paper_white_scale.load()},
      {"transfer_strength", transfer_strength.load()}, {"sdr_restore", sdr_black_restore.load()},
      {"hdr_pedestal", display_pedestal.load()}};
  diagnostics["colour"]["basis"] = "configured_values_effective_state_separate";
  diagnostics["colour"]["sdr_policy"] = present_sdr_restore_policy.load();
  diagnostics["colour"]["sdr_active"] = present_sdr_restore_active.load();
  diagnostics["colour"]["hdr_active"] = present_hdr_pedestal_active.load();
  {
    std::shared_lock lock(present_restore_configuration.mutex);
    diagnostics["colour"]["restore_scope"] = static_cast<uint32_t>(present_restore_configuration.scope.scope);
  }
  {
    std::shared_lock lock(present_restore_effective.mutex);
    diagnostics["colour"]["effective_domain"] = static_cast<uint32_t>(present_restore_effective.domain);
    diagnostics["colour"]["effective_status"] = present_restore_effective.status;
  }
  const float gpu_ms = nr_cost_gpu_ms.load();
  diagnostics["performance"] = {{"gpu_chain_ms", {{"status", gpu_ms > 0 ? "collected" : "unavailable"},
      {"value", gpu_ms > 0 ? nlohmann::json(gpu_ms) : nlohmann::json(nullptr)},
      {"basis", "existing_sampled_NR_chain_mean_not_whole_game_GPU_time"}}},
      {"displayed_fps", {{"status", "unavailable_not_measured"}}},
      {"counters", "cumulative_session_counts_not_rates"}};
  diagnostics["permissions"] = {{"screenshots", issue_report::capture_requested ? "authorized" : "not_authorized"},
      {"hardware_diagnostics", "authorized_local_collection"}, {"network_upload", "not_authorized"},
      {"public_report_link", "not_authorized"}};
  field("diagnostics", diagnostics.dump());
  json += "\n}\n";
  std::string redacted_json;
  if (!report::RedactJson(json, redaction, &redacted_json)) {
    throw std::runtime_error("Report JSON could not be sanitized.");
  }
  json = std::move(redacted_json);
  {
    std::ofstream preview(stage / "report.json", std::ios::binary | std::ios::trunc);
    preview.write(json.data(), static_cast<std::streamsize>(json.size()));
    if (!preview.good()) throw std::runtime_error("The report summary could not be written.");
  }
  const std::string public_summary = std::string(NR_NEXUS_NAME " ") + kAddonVersion
      + " | API=" + verdict.api + " | NR=" + NrStateName(verdict.state) + " | path=" + verdict.path
      + " | DLSSNR=" + (NgxLoaderView().nr_requirements.file_version.empty()
          ? std::string("unavailable") : NgxLoaderView().nr_requirements.file_version)
      + " | Native FG owner=" + (native_fg_owns.load() ? "yes" : "no")
      + " | XeFG owner=" + (fg::xefg::OwnsStream() ? "yes" : "no")
      + " | GPU=" + (gpu.empty() ? "unavailable" : gpu)
      + " | driver=" + (driver.empty() ? "unavailable" : driver)
      + " | OS=" + (os.empty() ? "unavailable" : os)
      + " | ReShade=" + (reshade_version.empty() ? "unavailable" : reshade_version);
  if (issue_report::cancel) {
    std::filesystem::remove_all(stage);
    issue_report::step = Step::kClosed;
    return;
  }

  uint64_t total = json.size();
  for (const issue_report::Item& item : items) total += item.bytes;
  // i18n: begin - the review step's file labels, drawn through ui::Tr
  items.insert(items.begin(),
               issue_report::Item{"Summary: version, status, settings, GPU and driver",
                                  "report.json", stage / "report.json", json.size(), true, true, json});  // i18n: skip
  // i18n: end
  const size_t files = items.size();
  std::string hosts = issue_report::test_url.empty() ? std::string() : "test";
  {
    std::lock_guard lock(issue_report::mutex);
    std::string unknown;
    for (const report::Host* host : report::ParseHosts(issue_report::host_setting, &unknown)) {
      if (issue_report::test_url.empty()) hosts += (hosts.empty() ? "" : ",") + std::string(host->id);
    }
    issue_report::capture_result = result;
    issue_report::items = std::move(items);
    issue_report::summary = std::move(json);
    issue_report::public_summary = report::RedactText(public_summary, redaction);
    issue_report::stage = stage;
  }
  Log(reshade::log::level::info,
      "NR-REPORT v1 review files=" + std::to_string(files) + " bytes="
          + std::to_string(total) + " capture=" + result
          + " hosts=" + (hosts.empty() ? "none" : hosts));
  issue_report::step = Step::kReview;
}

// The worker's second job: the zip, then the upload when the player asked
// for one.
inline void RunReportSendJob() {
  using issue_report::Step;
  const int64_t started_ns = SteadyNowNs();
  std::vector<issue_report::Item> items;
  std::string json;
  std::filesystem::path stage;
  std::string host_setting;
  {
    std::lock_guard lock(issue_report::mutex);
    items = issue_report::items;
    json = issue_report::summary;
    stage = issue_report::stage;
    host_setting = issue_report::host_setting;
  }
  std::string left_out = "[";
  for (const issue_report::Item& item : items) {
    if (!item.include) left_out += (left_out.size() > 1 ? ", " : "") + report::JsonString(item.entry);
  }
  auto document = nlohmann::json::parse(json);
  document["left_out"] = nlohmann::json::parse(left_out + "]");
  document["diagnostics"]["permissions"]["network_upload"] = issue_report::upload.load() ? "authorized" : "not_authorized";
  if (!report::RedactJson(document.dump(2), report::SystemRedaction(), &json)) {
    throw std::runtime_error("Report JSON could not be sanitized.");
  }
  const std::filesystem::path reports = stage.parent_path().parent_path();
  const std::filesystem::path zip = reports / ("renodx-dlss5-" + stage.filename().string() + ".zip");
  uint64_t zip_bytes = 0;
  bool packed = false;
  {
    report::ZipWriter writer(stage / "archive.zip");
    bool attachments_valid = true;
    writer.AddBytes("report.json", json.data(), json.size());
    for (const issue_report::Item& item : items) {
      if (issue_report::cancel) break;
      if (!item.include || item.required) continue;
      if (item.image) {
        if (!writer.AddFile(item.entry, item.file)) break;
      } else {
        // A staged file opened for review can be edited. Re-sanitize at the final export boundary.
        std::ifstream source(item.file, std::ios::binary | std::ios::ate);
        if (!source || source.tellg() < 0 || source.tellg() > static_cast<std::streamoff>(issue_report::kTextTailBytes + 1024)) {
          attachments_valid = false;
          break;
        }
        std::string text(static_cast<size_t>(source.tellg()), '\0');
        source.seekg(0);
        source.read(text.data(), static_cast<std::streamsize>(text.size()));
        if (source.gcount() != static_cast<std::streamsize>(text.size())) {
          attachments_valid = false;
          break;
        }
        const std::string redacted = report::RedactText(text, report::SystemRedaction());
        if (!writer.AddBytes(item.entry, redacted.data(), redacted.size())) break;
      }
    }
    packed = writer.Finish() && attachments_valid && !issue_report::cancel.load();
    zip_bytes = writer.Bytes();
  }
  std::error_code ignored;
  if (packed) {
    std::filesystem::rename(stage / "archive.zip", zip, ignored);
    packed = !ignored;
  }
  std::filesystem::remove_all(stage, ignored);
  // Explicitly saved ZIPs, including legacy reports, are never pruned automatically.
  {
    std::lock_guard lock(issue_report::mutex);
    issue_report::zip = packed ? zip : std::filesystem::path();
  }
  const std::string zip_name = zip.filename().string();
  const auto finish = [&](const char* outcome, const std::string& detail) {
    Log(std::strcmp(outcome, "uploaded") == 0 || std::strcmp(outcome, "saved") == 0
            ? reshade::log::level::info
            : reshade::log::level::warning,
        std::string("NR-REPORT v1 outcome=") + outcome + " bytes=" + std::to_string(zip_bytes)
            + " ms=" + std::to_string((SteadyNowNs() - started_ns) / 1'000'000) + " zip="
            + (packed ? zip_name : "-") + detail);
    issue_report::step = Step::kDone;
  };
  if (!packed) {
    if (issue_report::cancel) { finish("cancelled", ""); return; }
    {
      std::lock_guard lock(issue_report::mutex);
      issue_report::error = "the zip could not be written in " + report::Utf8(reports.wstring());
    }
    finish("pack_failed", "");
    return;
  }
  if (issue_report::cancel) { finish("cancelled", ""); return; }
  // A test posts litterbox's form to its loopback server.
  std::vector<const report::Host*> hosts;
  const bool test = !issue_report::test_reason.empty() || !issue_report::test_url.empty();
  if (test && issue_report::test_url.empty()) issue_report::test_url = L"http://127.0.0.1:1/unset";
  if (issue_report::upload && (test || issue_report::upload_consent)) {
    hosts = issue_report::test_url.empty() ? report::ParseHosts(host_setting, nullptr)
                                           : std::vector<const report::Host*>{&report::kHosts[0]};
  }
  if (hosts.empty()) {
    ShowReportZip();
    finish("saved", issue_report::upload ? " hosts=none" : "");
    return;
  }
  issue_report::step = Step::kUploading;
  const std::wstring agent = L"RenoDX-DLSS5/" + std::wstring(kAddonVersion, kAddonVersion + std::strlen(kAddonVersion));
  report::UploadResult result;
  const report::Host* answered = nullptr;
  for (const report::Host* host : hosts) {
    const char* id = issue_report::test_url.empty() ? host->id : "test";
    {
      std::lock_guard lock(issue_report::mutex);
      issue_report::host_display = issue_report::test_url.empty() ? host->display : "loopback test server";
    }
    Log(reshade::log::level::info,
        std::string("NR-REPORT v1 attempt host=") + id + " bytes=" + std::to_string(zip_bytes));
    result = report::Upload(*host, issue_report::test_url.empty() ? host->url : issue_report::test_url,
                            zip, agent, &issue_report::progress);
    if (result.ok || result.cancelled) {
      answered = host;
      break;
    }
    Log(reshade::log::level::warning,
        std::string("NR-REPORT v1 attempt host=") + id + " failed: " + result.error);
  }
  if (result.cancelled) {
    finish("cancelled", "");
    return;
  }
  if (!result.ok) {
    {
      std::lock_guard lock(issue_report::mutex);
      issue_report::error = result.error;
    }
    ShowReportZip();
    finish("upload_failed", " error=\"" + result.error + "\"");
    return;
  }
  // The link to the clipboard now, so it is there whether or not the
  // overlay is open when the upload ends.  A test session leaves the
  // developer's clipboard alone.
  const bool copied = !test && CopyReportText(result.link);
  {
    std::lock_guard lock(issue_report::mutex);
    issue_report::link = result.link;
    issue_report::link_copied = copied;
  }
  finish("uploaded", std::string(" host=")
                         + (issue_report::test_url.empty() ? answered->id : "test")
                         + " clipboard=" + (copied ? "copied" : issue_report::test_url.empty() ? "failed" : "skipped_test"));
}

inline void RunReportIssueJob() {
  report::issue::Input input;
  bool force_fallback = false;
  {
    std::lock_guard lock(issue_report::mutex);
    input.kind = issue_report::feature_request ? report::issue::Kind::FEATURE : report::issue::Kind::BUG;
    input.title = issue_report::description.empty() ? std::string(NR_NEXUS_NAME " - ") + GameExeName()
                                                   : issue_report::description;
    input.categories.clear();
    for (size_t i = 0; i < std::size(issue_report::kTicks); ++i) {
      if ((issue_report::ticks >> i) & 1u) input.categories.emplace_back(issue_report::kTicks[i].slug);
    }
    input.game = GameExeName();
    input.reproduction = issue_report::description + "\n\n" + issue_report::reproduction;
    if (issue_report::public_summary_consent) input.diagnostics = issue_report::public_summary;
    input.include_report_link = issue_report::public_link_consent;
    input.report_link = issue_report::link;
    force_fallback = issue_report::force_issue_fallback;
  }
  const auto redaction = report::SystemRedaction();
  input.title = report::RedactText(input.title, redaction);
  input.game = report::RedactText(input.game, redaction);
  input.reproduction = report::RedactText(input.reproduction, redaction);
  input.diagnostics = report::RedactText(input.diagnostics, redaction);
  auto draft = report::issue::BuildDraft(input);
  if (force_fallback) { draft.url = report::issue::NEW_ISSUE_URL; draft.fallback = true; }
  bool copied = false;
  bool opened = false;
  const bool test = !issue_report::test_reason.empty();
  const bool substituted = issue_report::copy_draft != CopyReportText || issue_report::open_browser != ShellExecuteW;
  if (!test || issue_report::copy_draft != CopyReportText) {
    copied = issue_report::copy_draft(draft.body);
  }
  if (!test || issue_report::open_browser != ShellExecuteW) {
    // Keep a full manual fallback even if GitHub ignores a form prefill or the browser is offline.
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, draft.url.data(),
                                          static_cast<int>(draft.url.size()), nullptr, 0);
    if (length > 0) {
      std::wstring url(length, L'\0');
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, draft.url.data(),
                          static_cast<int>(draft.url.size()), url.data(), length);
      const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
      opened = reinterpret_cast<INT_PTR>(issue_report::open_browser(
          nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
      if (SUCCEEDED(com)) CoUninitialize();
    }
  }
  {
    std::lock_guard lock(issue_report::mutex);
    issue_report::issue_body = std::move(draft.body);
    issue_report::issue_notice = test && !substituted ? "test_browser_suppressed"
        : !opened ? "browser_unavailable" : !copied ? "clipboard_unavailable"
        : draft.fallback ? "manual_fallback" : "draft_opened";
  }
  // Never log a URL, title, clipboard text or temporary report link.
  Log(reshade::log::level::info, "NR-REPORT v1 issue action=prepare result=ready");
  issue_report::step = issue_report::Step::kDone;
}

inline DWORD WINAPI ReportPreviewMain(void* module) {
  std::filesystem::path file;
  { std::lock_guard lock(issue_report::mutex); file = issue_report::preview_file; }
  if (!file.empty() && issue_report::test_reason.empty()) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ShellExecuteW(nullptr, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (SUCCEEDED(com)) CoUninitialize();
  }
  issue_report::worker_active = false;
  FreeLibraryAndExitThread(static_cast<HMODULE>(module), 0);
}

inline DWORD WINAPI IssueReportMain(void* module) {
  try {
    if (issue_report::step.load() == issue_report::Step::kOpeningIssue) {
      RunReportIssueJob();
    } else if (issue_report::step.load() == issue_report::Step::kCleaning) {
      std::filesystem::path stage;
      { std::lock_guard lock(issue_report::mutex); stage = issue_report::stage; }
      if (!stage.empty()) std::filesystem::remove_all(stage);
      issue_report::typed[0] = '\0';
      issue_report::typed_steps[0] = '\0';
      issue_report::typed_ticks = 0;
      issue_report::typed_capture = false;
      issue_report::step = issue_report::Step::kClosed;
    } else if (issue_report::step.load() == issue_report::Step::kCapturing) {
      RunReportCaptureJob();
    } else {
      RunReportSendJob();
    }
  } catch (const std::exception&) {
    std::filesystem::path stage;
    {
      std::lock_guard lock(issue_report::mutex);
      stage = issue_report::stage;
      issue_report::error = "Report generation failed; local files were kept when possible.";
    }
    std::error_code ignored;
    if (!stage.empty()) std::filesystem::remove_all(stage, ignored);
    Log(reshade::log::level::warning,
        "NR-REPORT v1 outcome=failed reason=worker_exception");
    issue_report::step = issue_report::Step::kDone;
  }
  issue_report::worker_active = false;
  FreeLibraryAndExitThread(static_cast<HMODULE>(module), 0);
}

// Runs `main` on a thread of its own that holds a reference on this module.
inline bool StartReportThread(LPTHREAD_START_ROUTINE main) {
  bool expected = false;
  if (!issue_report::worker_active.compare_exchange_strong(expected, true)) return false;
  HMODULE self = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(main),
                          &self)) {
    issue_report::worker_active = false;
    return false;
  }
  if (HANDLE thread = CreateThread(nullptr, 0, main, self, 0, nullptr)) {
    CloseHandle(thread);
    return true;
  }
  FreeLibrary(self);
  issue_report::worker_active = false;
  return false;
}

// Step 1 -> 2: keeps what the player typed and starts the capture.
inline void ContinueIssueReport(bool collect_diagnostics = true) {
  if (issue_report::step.load() != issue_report::Step::kDescribe || issue_report::worker_active.load()) return;
  {
    std::lock_guard lock(issue_report::mutex);
    issue_report::description = issue_report::typed;
    issue_report::ticks = issue_report::typed_ticks;
    issue_report::reproduction = issue_report::typed_steps;
    issue_report::capture_requested = issue_report::typed_capture || !issue_report::test_reason.empty();
    issue_report::feature_request = issue_report::typed_feature;
  }
  if (!collect_diagnostics) {
    issue_report::capture_requested = false;
    issue_report::step = issue_report::Step::kDone;
    return;
  }
  issue_report::step = issue_report::Step::kCapturing;
  if (!StartReportThread(IssueReportMain)) {
    Log(reshade::log::level::warning, "NR-REPORT v1 outcome=failed error=\"no worker thread\"");
    issue_report::step = issue_report::Step::kClosed;
  }
}

// Step 3 -> 4: the zip, and the upload when `with_upload`.
inline void SendIssueReport(bool with_upload) {
  if (issue_report::step.load() != issue_report::Step::kReview || issue_report::worker_active.load()) return;
  if (with_upload && issue_report::test_reason.empty() && !issue_report::upload_consent) return;
  issue_report::upload = with_upload;
  issue_report::progress.cancel = false;
  issue_report::step = issue_report::Step::kPacking;
  if (!StartReportThread(IssueReportMain)) {
    Log(reshade::log::level::warning, "NR-REPORT v1 outcome=failed error=\"no worker thread\"");
    issue_report::step = issue_report::Step::kReview;
  }
}

// Cancel in step 1 or 3, Close in step 4: back to the button.
inline void CloseIssueReport() {
  const issue_report::Step at = issue_report::step.load();
  if (issue_report::worker_active.load()) return;
  if (at == issue_report::Step::kReview) {
    issue_report::step = issue_report::Step::kCleaning;
    if (!StartReportThread(IssueReportMain)) issue_report::step = at;
    return;
  }
  issue_report::typed[0] = '\0';
  issue_report::typed_ticks = 0;
  issue_report::typed_steps[0] = '\0';
  issue_report::typed_capture = false;
  issue_report::step = issue_report::Step::kClosed;
}

// What the panel says about a capture result (English; drawn through Tr).
// i18n: begin - display strings (tools/i18n/ui_strings.py catalog)
inline const char* CaptureNote(const std::string& result) {
  if (result == "written") return "Capture taken.";
  if (result == "unsupported_present") return "No capture: this API or presentation path does not support an exact final-frame comparison yet.";
  if (result == "unsupported_diagnostic") return "No capture: model-buffer readback is unavailable on this path; use the game's screenshot function for the displayed image.";
  if (result == "unproven_baseline") return "No capture: exact comparison unavailable with this frame-generation route; disable frame generation for the capture.";
  if (result == "nr_disabled") return "No capture: enable NR before taking a comparison.";
  if (result == "foreign_nr") return "No capture: another NR implementation owns this frame.";
  if (result == "unexecuted") return "No capture: the command list carrying its copies did not execute.";
  if (result == "no_nr_frame") {
    return "No capture: NR did not run a frame (NR off, a menu or a loading screen).";
  }
  if (result == "timeout") return "No capture: it did not finish in time.";
  if (result == "gpu_unconfirmed") return "No capture: the GPU did not confirm the copies in time.";
  if (result == "aborted" || result == "not_ready") {
    return "No capture: the frame's NR pass did not complete.";
  }
  if (result == "skipped") return "No capture: skipped.";
  return "No capture: the files could not be written.";
}

// The report, drawn under the status card and the health ledger's issue
// list: the button while closed, the current step otherwise.  The capture is
// named by the key that takes it (NRScreenshotKey), not a fixed F5.
inline void DrawIssueReport() {
  using issue_report::Step;
  const Step at = issue_report::step.load();
  const std::string capture_key = VirtualKeyName(screenshot_hotkey.load());
  if (at == Step::kClosed) {
    // With a ledger warning active the button names it, and the report
    // carries the row that asked.
    const uint64_t mask = health_active_mask.load(std::memory_order_relaxed);
    const char* row_slug = nullptr;
    for (size_t i = 0; i < health::kRowCount && row_slug == nullptr; ++i) {
      if (((mask >> i) & 1u) != 0 && health::kRows[i].tone == health::Tone::kWarn) {
        row_slug = health::kRows[i].slug;
      }
    }
    if (ui::ActionButton(ui::Tr(row_slug != nullptr ? "Send a report about these issues"
                                                    : "I'm having issues..."))) {
      BeginIssueReport(row_slug != nullptr ? row_slug : "button");  // i18n: skip
    }
    ui::ItemTooltip("%s", ui::Tr(ui::text::kReportPrivacy));
    return;
  }

  ui::StyleScope frame;
  frame.Color(ImGuiCol_ChildBg, ui::tokens::kBgCard)
      .Var(ImGuiStyleVar_WindowPadding, ImVec2(ui::tokens::kCardPadding, ui::tokens::kCardPadding));
  if (ImGui::BeginChild("##nr_issue_report", ImVec2(0.f, 0.f),
                        ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding)) {
    ImGui::PushTextWrapPos(0.f);
    ui::CardHeading({.tone = ui::CardTone::kIdle, .glyph = ui::CardGlyph::kInfo,
                     .title = "Report an issue"});
    const auto progress_bar = [](float fraction) {
      ImGui::ProgressBar(std::clamp(fraction, 0.f, 1.f), ImVec2(-FLT_MIN, 0.f), "");
    };
    switch (at) {
      case Step::kDescribe: {
        ImGui::TextUnformatted(ui::Tr("Step 1 of 3: what went wrong?"));
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##issue_description",
                                 ui::Tr("One line: what you saw, and where in the game (optional)"),
                                 issue_report::typed, sizeof(issue_report::typed));
        ImGui::TextUnformatted(ui::Tr(ui::text::kReportSteps));
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextMultiline("##issue_steps", issue_report::typed_steps, sizeof(issue_report::typed_steps),
                                  ImVec2(-FLT_MIN, ImGui::GetTextLineHeightWithSpacing() * 3.f));
        ui::Checkbox(ui::Tr(ui::text::kReportFeature), &issue_report::typed_feature);
        for (size_t i = 0; i < std::size(issue_report::kTicks); ++i) {
          bool ticked = ((issue_report::typed_ticks >> i) & 1u) != 0;
          ImGui::PushID(static_cast<int>(i));
          const char* label = ui::Tr(issue_report::kTicks[i].label);
          if (i % 2 == 1) ui::SameLineIfFits(ImGui::CalcTextSize(label).x
                                             + ImGui::GetFrameHeight() * 2.f);
          if (ui::Checkbox(label, &ticked)) {
            issue_report::typed_ticks ^= 1u << i;
          }
          ImGui::PopID();
        }
        ImGui::TextDisabled("%s", ui::Tr(ui::text::kReportLocalSaved));
        ui::Checkbox(ui::Tr(ui::text::kReportCapture), &issue_report::typed_capture);
        if (ui::ActionButton(ui::Tr(ui::text::kReportCollect))) ContinueIssueReport();
        if (ui::ActionButton(ui::Tr(ui::text::kReportWithoutDiagnostics))) ContinueIssueReport(false);
        ui::SameLineIfFits(ImGui::GetFontSize() * 12.f);
        if (ui::ActionButton(ui::Tr("Cancel"))) CloseIssueReport();
        break;
      }
      case Step::kCapturing: {
        ImGui::TextUnformatted(ui::Tr("Step 2 of 3: capturing"));
        ImGui::TextDisabled(
            ui::Tr("Taking a capture (%s). Keep playing: a menu or loading screen has no NR"
                   " frame to capture."),
            capture_key.c_str());
        const int64_t timeout_ms = issue_report::test_timeout_ms > 0
                                       ? issue_report::test_timeout_ms
                                       : issue_report::kCaptureTimeoutMs;
        progress_bar(static_cast<float>(SteadyNowNs() - issue_report::capture_started_ns.load())
                     / (static_cast<float>(timeout_ms) * 1e6f));
        if (ui::ActionButton(ui::Tr("Continue without a capture"))) issue_report::skip_capture = true;
        ui::SameLineIfFits(ImGui::GetFontSize() * 12.f);
        if (ui::ActionButton(ui::Tr("Cancel"))) issue_report::cancel = true;
        break;
      }
      case Step::kCollecting:
        ImGui::TextUnformatted(ui::Tr("Collecting the logs..."));
        if (ui::ActionButton(ui::Tr("Cancel"))) issue_report::cancel = true;
        break;
      case Step::kReview: {
        ImGui::TextUnformatted(ui::Tr("Step 3 of 3: review what is sent"));
        bool send = false;
        bool save = false;
        bool close = false;
        bool preview = false;
        {
        std::lock_guard lock(issue_report::mutex);
        ImGui::TextDisabled("%s", ui::Tr(CaptureNote(issue_report::capture_result)));
        ImGui::TextUnformatted(ui::Tr(ui::text::kReportPrivacy));
        ImGui::TextDisabled("%s", ui::Tr(ui::text::kReportPreviewLimit));
        uint64_t total = 0;
        for (size_t i = 0; i < issue_report::items.size(); ++i) {
          issue_report::Item& item = issue_report::items[i];
          ImGui::PushID(static_cast<int>(i));
          ImGui::BeginDisabled(item.required);
          ui::Checkbox(ui::Tr(item.label), &item.include);
          ImGui::EndDisabled();
          ImGui::PopID();
          ui::SameLineIfFits(ImGui::GetFontSize() * 12.f);
          ImGui::TextDisabled("%s, %s", item.entry.c_str(), FormatBytes(item.bytes).c_str());
          if (!item.file.empty()) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::BeginDisabled(issue_report::worker_active.load());
            if (ImGui::SmallButton(ui::Tr(ui::text::kReportOpenPreview))) {
              issue_report::preview_file = item.file;
              preview = true;
            }
            ImGui::EndDisabled();
            ImGui::PopID();
          }
          if (!item.preview.empty() && ImGui::TreeNode((std::string(ui::Tr(ui::text::kReportPreview))
                  + "##" + std::to_string(i)).c_str())) {
            if (ImGui::BeginChild(("##attachment_preview" + std::to_string(i)).c_str(),
                ImVec2(0.f, ImGui::GetTextLineHeightWithSpacing() * 8.f), ImGuiChildFlags_Borders)) {
              ImGui::TextUnformatted(item.preview.c_str());
            }
            ImGui::EndChild();
            ImGui::TreePop();
          }
          if (item.include) total += item.bytes;
        }
        ImGui::Text(ui::Tr("Total: %s"), FormatBytes(total).c_str());
        const std::vector<const report::Host*> hosts =
            report::ParseHosts(issue_report::host_setting, nullptr);
        const bool can_upload = !hosts.empty() || !issue_report::test_url.empty();
        if (!issue_report::test_url.empty()) {
          ImGui::TextUnformatted("Upload sends the zip to the loopback test server.");  // i18n: skip (test only)
        } else if (can_upload) {
          ImGui::Text(
              ui::Tr("Upload sends the zip to %s. Anyone with the link can download it; it is"
                     " deleted after %u hours."),
              hosts.front()->display, hosts.front()->retention_hours);
          if (hosts.size() > 1) {
            ImGui::TextDisabled(ui::Tr("If it does not answer: %s, deleted after %u hours."),
                                hosts[1]->display, hosts[1]->retention_hours);
          }
        } else {
          ImGui::TextUnformatted(
              ui::Tr("Uploading is off (NRReportHost=none): the zip is saved on this PC."));
        }
    ImGui::TextDisabled("%s", ui::Tr(ui::text::kReportPrivacy));
        if (can_upload) {
          ui::Checkbox(ui::Tr(ui::text::kReportUploadConsent), &issue_report::upload_consent);
          ImGui::BeginDisabled(!issue_report::upload_consent);
          send = ui::ActionButton(ui::Tr("Upload & copy link"));
          ImGui::EndDisabled();
          ui::SameLineIfFits(ImGui::GetFontSize() * 12.f);
        }
        save = ui::ActionButton(ui::Tr("Save zip only"));
        ui::SameLineIfFits(ImGui::GetFontSize() * 12.f);
        close = ui::ActionButton(ui::Tr("Cancel"));
        }
        // Out of the lock: both take it.
        if (preview) StartReportThread(ReportPreviewMain);
        if (send || save) SendIssueReport(send);
        if (close) CloseIssueReport();
        break;
      }
      case Step::kPacking:
        ImGui::TextUnformatted(ui::Tr("Packing the zip..."));
        if (ui::ActionButton(ui::Tr("Cancel"))) issue_report::cancel = true;
        break;
      case Step::kCleaning:
        ImGui::TextUnformatted(ui::Tr(ui::text::kReportCleaning));
        break;
      case Step::kOpeningIssue:
        ImGui::TextUnformatted("GitHub...");
        break;
      case Step::kUploading: {
        std::string host;
        {
          std::lock_guard lock(issue_report::mutex);
          host = issue_report::host_display;
        }
        ImGui::Text(ui::Tr("Uploading to %s..."), host.c_str());
        const uint64_t total = issue_report::progress.total.load();
        progress_bar(total == 0 ? 0.f
                                : static_cast<float>(issue_report::progress.sent.load())
                                      / static_cast<float>(total));
        if (ui::ActionButton(ui::Tr("Cancel"))) issue_report::progress.cancel = true;
        break;
      }
      case Step::kDone: {
        std::string link;
        std::string error;
        std::filesystem::path zip;
        std::string body, notice, public_summary;
        {
          std::lock_guard lock(issue_report::mutex);
          body = issue_report::issue_body;
          notice = issue_report::issue_notice;
          public_summary = issue_report::public_summary;
          link = issue_report::link;
          error = issue_report::error;
          zip = issue_report::zip;
        }
        if (!link.empty()) {
          ImGui::TextColored(ui::tokens::kGood, "%s %s", ui::icons::kCheck,
                             ui::Tr(ui::text::kReportLinkReady));
          ImGui::SetNextItemWidth(-FLT_MIN);
          ImGui::InputText("##issue_link", link.data(), link.size() + 1, ImGuiInputTextFlags_ReadOnly);
          ImGui::TextUnformatted(ui::Tr(ui::text::kReportPublicNotice));
          if (ui::ActionButton(ui::Tr("Copy again"))) ImGui::SetClipboardText(link.c_str());
          ui::SameLineIfFits(ImGui::GetFontSize() * 12.f);
        } else {
          if (!error.empty()) {
            ImGui::TextColored(ui::tokens::kWarn, ui::Tr(ui::text::kReportFailed), error.c_str());
          }
          if (!zip.empty()) {
            ImGui::Text(ui::Tr("Saved: %s"), report::Utf8(zip.filename().wstring()).c_str());
            ImGui::TextUnformatted(ui::Tr(
                ui::text::kReportLocalSaved));
            if (ui::ActionButton(ui::Tr("Open folder"))) StartReportThread(ShowReportZipMain);
            ui::SameLineIfFits(ImGui::GetFontSize() * 12.f);
          }
        }
        ImGui::TextUnformatted(ui::Tr(ui::text::kReportPublicNotice));
        if (!public_summary.empty()) {
          ui::Checkbox(ui::Tr(ui::text::kReportPublicSummaryConsent), &issue_report::public_summary_consent);
          ImGui::TextUnformatted(public_summary.c_str());
        }
        if (!link.empty()) {
          ui::Checkbox(ui::Tr(ui::text::kReportPublicLinkConsent), &issue_report::public_link_consent);
        }
        ImGui::BeginDisabled(issue_report::worker_active.load());
        const bool open_issue = ui::ActionButton(ui::Tr(ui::text::kReportGitHub));
        const bool plain_issue = ui::ActionButton(ui::Tr(ui::text::kReportPlainIssue));
        if (open_issue || plain_issue) {
          issue_report::force_issue_fallback = plain_issue;
          issue_report::step = Step::kOpeningIssue;
          if (!StartReportThread(IssueReportMain)) issue_report::step = Step::kDone;
        }
        if (!notice.empty()) {
          ImGui::TextUnformatted(ui::Tr(notice == "browser_unavailable" ? ui::text::kReportBrowserFailed
              : notice == "clipboard_unavailable" ? ui::text::kReportClipboardFailed
              : notice == "manual_fallback" ? ui::text::kReportManualFallback : ui::text::kReportDraftReady));
        }
        if (!body.empty() && ImGui::TreeNode(ui::Tr(ui::text::kReportPreview))) {
          ImGui::InputTextMultiline("##public_markdown", body.data(), body.size() + 1,
              ImVec2(-FLT_MIN, ImGui::GetTextLineHeightWithSpacing() * 8.f), ImGuiInputTextFlags_ReadOnly);
          if (ui::ActionButton(ui::Tr("Copy again"))) ImGui::SetClipboardText(body.c_str());
          ImGui::TreePop();
        }
        if (ui::ActionButton(ui::Tr("Close"))) CloseIssueReport();
        ImGui::EndDisabled();
        break;
      }
      case Step::kClosed:
        break;
    }
    ImGui::PopTextWrapPos();
  }
  ImGui::EndChild();
}
// i18n: end

// ---------------------------------------------------------------------------
// The test driver (RENODX_NR_TEST_REPORT*)
// ---------------------------------------------------------------------------

inline bool InitIssueReportTest() {
  char buffer[512] = {};
  size_t length = 0;
  if (getenv_s(&length, buffer, sizeof(buffer), "RENODX_NR_TEST_REPORT") != 0 || length <= 1) {
    return false;
  }
  issue_report::test_reason = buffer;
  length = 0;
  if (getenv_s(&length, buffer, sizeof(buffer), "RENODX_NR_TEST_REPORT_URL") == 0 && length > 1) {
    // Loopback only: a test never reaches a real host.
    const std::string url = buffer;
    const std::wstring wide(url.begin(), url.end());
    report::ParsedReportUrl parsed;
    if (report::ParseReportUrl(wide, &parsed) && parsed.loopback && !parsed.secure) {
      issue_report::test_url = wide;
    } else {
      Log(reshade::log::level::warning,
          "RENODX_NR_TEST_REPORT_URL is not a loopback URL; the test report will not upload");
      issue_report::test_url = L"http://127.0.0.1:1/refused";
    }
  } else {
    issue_report::test_url = L"http://127.0.0.1:1/unset";
  }
  length = 0;
  if (getenv_s(&length, buffer, sizeof(buffer), "RENODX_NR_TEST_REPORT_TIMEOUT_MS") == 0
      && length > 1) {
    issue_report::test_timeout_ms = std::strtoll(buffer, nullptr, 10);
  }
  // Presents are held (not the capture's: it needs them) from 30 before
  // the host's last, so a session outlives its report's worker.
  length = 0;
  const uint32_t frames = getenv_s(&length, buffer, sizeof(buffer), "RENODX_E2E_FRAMES") == 0
                                  && length > 1
                              ? static_cast<uint32_t>(std::strtoul(buffer, nullptr, 10))
                              : 240u;
  issue_report::test_stall_from = frames > 90 ? frames - 30 : 60;
  return true;
}

inline void OnIssueReportTestPresent(reshade::api::effect_runtime* /*runtime*/) {
  using issue_report::Step;
  static uint32_t presents = 0;
  static bool sent = false;
  static uint32_t stalled_ms = 0;
  ++presents;
  const Step at = issue_report::step.load();
  if (presents == 60 && at == Step::kClosed && BeginIssueReport(issue_report::test_reason.c_str())) {
    // The player's own words carry their profile path: the bundle must not.
    char profile[MAX_PATH] = {};
    GetEnvironmentVariableA("USERPROFILE", profile, MAX_PATH);
    std::snprintf(issue_report::typed, sizeof(issue_report::typed),
                  "test report, saved under %s\\Documents", profile);
    issue_report::typed_ticks = 0b10001;
    ContinueIssueReport();
  }
  if (at == Step::kReview && !sent) {
    sent = true;
    SendIssueReport(true);
  }
  if (presents < issue_report::test_stall_from) return;
  // The host's last presents: hold this one until the worker is done (at
  // most two minutes), so the process outlives its report.
  for (Step now = at; (now == Step::kCapturing || now == Step::kCollecting || now == Step::kPacking
                       || now == Step::kUploading || (now == Step::kReview && !sent))
                      && stalled_ms < 120'000;
       now = issue_report::step.load()) {
    if (now == Step::kReview && !sent) {
      sent = true;
      SendIssueReport(true);
    }
    Sleep(20);
    stalled_ms += 20;
  }
}
