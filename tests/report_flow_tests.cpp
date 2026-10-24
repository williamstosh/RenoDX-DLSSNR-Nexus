/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#define DEBUG_LEVEL_0

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <dlssnr.hpp>

namespace runtime = renodx::addons::dlss5::internal;
namespace flow = runtime::issue_report;
namespace capture = renodx::addons::dlss5::screenshot;
namespace report = renodx::addons::dlss5::report;
using Json = nlohmann::json;

namespace {
std::filesystem::path base_directory;
unsigned checks = 0;
bool copy_result = true;
INT_PTR browser_result = 33;
unsigned copy_calls = 0;
unsigned browser_calls = 0;
std::string copied_text;
std::wstring browser_url;

void Require(bool condition, const char* message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}

void Write(const std::filesystem::path& path, std::string_view text) {
  std::ofstream out(path, std::ios::binary);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  Require(out.good(), "fixture write");
}

std::string Read(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  Require(in.good(), "fixture read");
  return {std::istreambuf_iterator<char>(in), {}};
}

void CheckPrivacy(std::string_view text) {
  for (const auto* secret : {"SYNTHETIC123", "query-value", "opaque-secret", "private-header"}) {
    Require(text.find(secret) == std::string_view::npos, "export contains synthetic secret");
  }
  const auto privacy = report::SystemRedaction();
  if (!privacy.profile.empty()) {
    Require(text.find(privacy.profile) == std::string_view::npos, "export contains profile path");
  }
}

bool FakeCopyDraft(std::string_view text) {
  ++copy_calls;
  copied_text = text;
  return copy_result;
}

HINSTANCE WINAPI FakeOpenBrowser(HWND window, LPCWSTR operation, LPCWSTR file,
                                LPCWSTR parameters, LPCWSTR directory, INT show) {
  ++browser_calls;
  Require(window == nullptr && parameters == nullptr && directory == nullptr, "browser receives URL without shell arguments");
  Require(operation != nullptr && std::wstring_view(operation) == L"open" && show == SW_SHOWNORMAL,
          "browser open boundary contract");
  Require(file != nullptr, "browser has URL");
  browser_url = file;
  return reinterpret_cast<HINSTANCE>(browser_result);
}

void Begin() {
  Require(runtime::BeginIssueReport("fixture"), "begin report");
  flow::test_reason = "fixture";  // Disable Explorer/browser; no automatic present callback is registered.
  flow::test_timeout_ms = 200;
  flow::host_setting = "none";
  flow::description = "中文 issue ghp_SYNTHETIC123";
  flow::reproduction = "First step\nAuthorization: Bearer private-header";
  flow::ticks = (1u << 9) - 1;
}

void Collect(const std::string& mode) {
  flow::capture_requested = mode == "written" || mode == "timeout" || mode == "no-nr-frame" || mode == "cancel-capture";
  flow::step = flow::Step::kCapturing;
  const auto request_before = capture::internal::request_id.load();
  std::thread publisher;
  if (mode == "timeout" || mode == "written") {
    publisher = std::thread([mode] {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (!capture::IsArmed() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      if (!capture::IsArmed()) return;
      const auto serial = capture::internal::capture_serial.fetch_add(1) + 1;
      if (mode == "written") {
        capture::internal::PublishFinished({
            .serial = serial,
            .outcome = "written",
            .pre_image = base_directory / "Saved" / "pre.png",
            .post_image = base_directory / "Saved" / "post.png",
            .meta = base_directory / "Saved" / "capture.meta.txt",
        });
      }
    });
  }
  if (mode == "cancel-capture") {
    flow::cancel = true;
  }
  try {
    runtime::RunReportCaptureJob();
  } catch (...) {
    if (publisher.joinable()) publisher.join();
    throw;
  }
  if (publisher.joinable()) publisher.join();
  capture::Disarm();
  if (mode == "cancel-capture") {
    Require(flow::step == flow::Step::kClosed, "capture cancelled");
    return;
  }
  Require(flow::step == flow::Step::kReview, "collection enters review");
  Require(flow::stage.parent_path().filename() == ".cache", "staging isolated in .cache");
  const std::string expected = (mode == "written" ? "written" : mode == "timeout" ? "timeout"
                                               : mode == "no-nr-frame" ? "no_nr_frame" : "skipped");
  Require(flow::capture_result == expected, "capture outcome");
  if (!flow::capture_requested) {
    Require(capture::internal::request_id == request_before, "no screenshot option never arms capture");
  }
  const auto summary = Json::parse(flow::summary);
  Require(summary["diagnostics"]["extension_version"] == 2, "diagnostics extension version 2");
  for (const auto* field : {"nr", "native_fg", "xefg", "colour", "performance", "permissions"}) {
    Require(summary["diagnostics"].contains(field), "diagnostic extension field");
  }
  CheckPrivacy(flow::summary);
  for (const auto& item : flow::items) {
    if (!item.file.empty() && !item.image) {
      CheckPrivacy(Read(item.file));
    }
  }
}

void Save(const std::string& mode) {
  std::vector<std::filesystem::path> old_zips;
  const auto reports = base_directory / "DLSS5 Reports";
  if (mode == "retention") {
    for (int index = 0; index < 6; ++index) {
      const auto path = reports / ("renodx-dlss5-report-20000101-00000" + std::to_string(index) + ".zip");
      Write(path, "user saved archive");
      old_zips.push_back(path);
    }
  }
  Json excluded = Json::array();
  Json included = Json::array({"report.json"});
  for (auto& item : flow::items) {
    if (mode == "written" && item.image && item.entry.find("NR_ON") != std::string::npos) {
      item.include = true;
    }
    if (mode == "staged-edit" && !item.file.empty() && !item.image) {
      Write(item.file, "Edited after review: ghp_SYNTHETIC123\ntoken=opaque-secret\n"
                       "Authorization: Bearer private-header\n?token=query-value&fps=60\n"
                       "{\"credentials\":{\"value\":\"opaque-secret\"}}\n");
    }
    if (mode == "selection" && item.entry == "logs/ReShade.ini") {
      item.include = false;
    }
    if (!item.include) {
      excluded.push_back(item.entry);
    } else if (!item.required && item.include) {
      included.push_back(item.entry);
    }
  }
  if (mode == "selection") {
    Require(!excluded.empty(), "selection fixture has optional settings attachment");
  }
  if (mode == "json-privacy") {
    auto summary = Json::parse(flow::summary);
    summary["future_extension"] = {
        {"access_token", "opaque-secret"},
        {"message", "ghp_SYNTHETIC123 ?token=query-value&width=1920"},
        {"profile_path", report::SystemRedaction().profile + "\\test.txt"},
        {"nested", Json::array({true, 42, "中文"})},
    };
    flow::summary = summary.dump();
  }
  if (mode == "pack-failure") {
    flow::items.push_back({.label = "Missing fixture", .entry = "missing.txt",
                           .file = base_directory / "absent.txt", .bytes = 7});
  }
  flow::upload = false;
  flow::step = flow::Step::kPacking;
  const auto staging = flow::stage;
  runtime::RunReportSendJob();
  Require(flow::step == flow::Step::kDone, "send job completes");
  Require(!std::filesystem::exists(staging), "private staging removed");
  Require(flow::link.empty(), "local save has no uploaded link");
  Require(Read(base_directory / "Saved" / "pre.png") == "synthetic-original-pre", "original Saved capture retained");
  if (mode == "pack-failure") {
    Require(flow::zip.empty() && !flow::error.empty(), "packing failure surfaced without saved ZIP");
    return;
  }
  Require(!flow::zip.empty() && std::filesystem::is_regular_file(flow::zip), "local ZIP created without upload");
  for (const auto& path : old_zips) {
    Require(Read(path) == "user saved archive", "more than three old ZIPs retained byte for byte");
  }
  Write(base_directory / "archive-manifest.json", Json({
      {"archive", report::Utf8(flow::zip.wstring())}, {"included", included}, {"excluded", excluded},
      {"future_extension", mode == "json-privacy"},
  }).dump(2));
  runtime::CloseIssueReport();
  Require(std::filesystem::exists(reports), "closing done retains reports");
}

void TestIssueJob(const std::string& mode) {
  flow::test_reason.clear();
  std::strcpy(flow::typed, "中文 title ghp_SYNTHETIC123");
  std::strcpy(flow::typed_steps, "step one\ntoken=opaque-secret");
  flow::typed_capture = false;
  const auto request_before = capture::internal::request_id.load();
  runtime::ContinueIssueReport(false);
  Require(flow::step == flow::Step::kDone && !flow::worker_active, "skip diagnostics synchronous transition");
  Require(flow::summary.empty() && flow::public_summary.empty(), "skip diagnostics has no report data");
  Require(flow::stage.empty() && flow::zip.empty(), "skip diagnostics creates no files");
  Require(!flow::capture_requested && capture::internal::request_id == request_before, "skip diagnostics does not capture");
  Require(!flow::public_summary_consent && !flow::public_link_consent, "public consent defaults off");
  flow::test_reason = "fixture";
  flow::public_summary = "PRIVATE_DIAGNOSTIC_SENTINEL";
  flow::link = "https://example.invalid/PRIVATE_LINK_SENTINEL";
  flow::feature_request = mode == "issue-feature";
  flow::force_issue_fallback = mode == "issue-fallback";
  flow::public_summary_consent = mode == "issue-consent";
  flow::public_link_consent = mode == "issue-consent";
  flow::step = flow::Step::kOpeningIssue;
  runtime::RunReportIssueJob();
  Require(flow::step == flow::Step::kDone, "issue worker completes");
  Require(flow::issue_notice == "test_browser_suppressed", "test suppresses clipboard/browser");
  Require(!flow::issue_body.empty(), "issue worker prepares Markdown");
  CheckPrivacy(flow::issue_body);
  const bool consent = mode == "issue-consent";
  Require((flow::issue_body.find("PRIVATE") != std::string::npos) == consent, "public content requires separate consent");
  Require(flow::zip.empty() && flow::stage.empty(), "issue-only draft needs no ZIP or capture");
  runtime::CloseIssueReport();
}

void TestIssueBoundaries(const std::string& mode) {
  // Keep an actual production-written ZIP while injecting only the two OS
  // boundaries. The test reason continues to suppress all default OS actions.
  Save(mode);
  const auto zip = flow::zip;
  const auto zip_bytes = Read(zip);
  Require(flow::copy_draft == runtime::CopyReportText && flow::open_browser == ShellExecuteW,
          "production boundaries default to real platform functions");
  copy_result = mode != "boundary-clipboard-failure" && mode != "boundary-both-failure";
  browser_result = (mode == "boundary-browser-failure" || mode == "boundary-both-failure" ? 31 : 33);
  flow::force_issue_fallback = mode == "boundary-manual-fallback";
  flow::copy_draft = FakeCopyDraft;
  flow::open_browser = FakeOpenBrowser;
  flow::step = flow::Step::kOpeningIssue;
  runtime::RunReportIssueJob();
  flow::copy_draft = runtime::CopyReportText;
  flow::open_browser = ShellExecuteW;
  Require(copy_calls == 1 && browser_calls == 1, "both injected boundaries called once");
  Require(flow::step == flow::Step::kDone, "platform failure completes with manual recovery");
  const std::string expected = (browser_result <= 32 ? "browser_unavailable" : !copy_result ? "clipboard_unavailable"
      : flow::force_issue_fallback ? "manual_fallback" : "draft_opened");
  Require(flow::issue_notice == expected, "platform result selects visible notice");
  Require(!copied_text.empty() && flow::issue_body == copied_text, "full Markdown retained even when clipboard fails");
  CheckPrivacy(copied_text);
  Require(browser_url.starts_with(L"https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/issues/new"),
          "browser failure still targets official repository");
  Require(browser_url.size() <= report::issue::MAX_URL_BYTES, "browser URL respects shell limit");
  if (flow::force_issue_fallback) {
    Require(report::Utf8(browser_url) == report::issue::NEW_ISSUE_URL, "manual fallback opens ordinary new issue");
  }
  Require(flow::zip == zip && Read(zip) == zip_bytes, "all platform outcomes preserve saved ZIP byte for byte");
  runtime::CloseIssueReport();
  Require(Read(zip) == zip_bytes, "closing platform error retains saved ZIP");
}
}  // namespace

extern "C" __declspec(dllexport) void ReShadeLogMessage(void*, int, const char* message) {
  std::cout << "production: " << message << '\n';
}

// Only the ReShade host boundary is replaced; collectors, state machine,
// redaction, ZIP writer and diagnostics execute their production implementation.
extern "C" __declspec(dllexport) bool ReShadeGetBasePath(char* path, size_t* size) {
  const auto base = base_directory.string();
  if (!path) {
    *size = base.size() + 1;
  } else if (*size != 0) {
    const auto copied = std::min(*size - 1, base.size());
    std::memcpy(path, base.data(), copied);
    path[copied] = '\0';
    *size = copied;
  }
  return true;
}

extern "C" __declspec(dllexport) bool ReShadeGetConfigValue(
    void*, reshade::api::effect_runtime*, const char*, const char*, char*, size_t* size) {
  *size = 0;
  return false;
}

extern "C" __declspec(dllexport) void ReShadeSetConfigValue(
    void*, reshade::api::effect_runtime*, const char*, const char*, const char*) {}

int main(int argc, char** argv) {
  try {
    Require(argc == 2, "usage: report_flow_tests CASE (private, empty cwd required)");
    const std::string mode = argv[1];
    base_directory = std::filesystem::current_path();
    Require(std::filesystem::is_empty(base_directory), "flow fixture requires empty private cwd");
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    std::filesystem::create_directory(base_directory / "Saved");
    Write(base_directory / "Saved" / "pre.png", "synthetic-original-pre");
    Write(base_directory / "Saved" / "post.png", "synthetic-original-post");
    Write(base_directory / "Saved" / "capture.meta.txt", "token=opaque-secret\nwidth=1920");
    Write(base_directory / "ReShade.log", "ReShade version 'fixture'\nAuthorization: Bearer private-header\nghp_SYNTHETIC123");
    Write(base_directory / "ReShade.ini", "[Synthetic]\naccess_token=opaque-secret\nUrl=https://example.invalid/?token=query-value\n");
    Begin();
    if (mode == "cancel-describe") {
      runtime::CloseIssueReport();
      Require(flow::step == flow::Step::kClosed, "cancel description");
    } else if (mode == "repeat") {
      Require(!runtime::BeginIssueReport("duplicate"), "repeat begin refused");
      flow::worker_active = true;
      runtime::CloseIssueReport();
      Require(flow::step == flow::Step::kDescribe, "close cannot race active worker");
      flow::step = flow::Step::kReview;
      runtime::SendIssueReport(false);
      Require(flow::step == flow::Step::kReview, "repeat send refused while worker active");
      flow::worker_active = false;
      flow::step = flow::Step::kDescribe;
      runtime::CloseIssueReport();
      Require(runtime::BeginIssueReport("again"), "begin after close");
      runtime::CloseIssueReport();
    } else if (mode == "test-environment") {
      Require(_putenv_s("RENODX_NR_TEST_REPORT", "fixture") == 0, "set test reason environment");
      for (const auto* url : {"https://example.invalid/upload", "http://127.0.0.1:1@evil.invalid/upload",
                              "http://localhost:1@evil.invalid/upload", "http://127.0.0.1.evil.invalid:1/upload", ""}) {
        Require(_putenv_s("RENODX_NR_TEST_REPORT_URL", url) == 0, "set synthetic test URL");
        Require(runtime::InitIssueReportTest(), "test environment recognized");
        report::ParsedReportUrl parsed;
        Require(report::ParseReportUrl(flow::test_url, &parsed) && parsed.loopback,
                "remote or absent test URL replaced by exact loopback refusal");
        Require(!flow::test_reason.empty(), "test browser suppression remains active");
      }
      Require(_putenv_s("RENODX_NR_TEST_REPORT", "") == 0, "remove test reason");
      Require(_putenv_s("RENODX_NR_TEST_REPORT_URL", "") == 0, "remove test URL");
      runtime::CloseIssueReport();
    } else if (mode.starts_with("issue-")) {
      TestIssueJob(mode);
    } else if (mode == "collect-failure") {
      Write(base_directory / "DLSS5 Reports", "block directory creation");
      flow::step = flow::Step::kCollecting;
      bool threw = false;
      try {
        runtime::RunReportCaptureJob();
      } catch (const std::exception&) {
        threw = true;
      }
      Require(threw && flow::step != flow::Step::kReview, "collection error propagated");
    } else {
      Collect(mode);
      if (mode == "cancel-capture") {
        Require(flow::zip.empty(), "cancel creates no archive");
      } else if (mode.starts_with("boundary-")) {
        TestIssueBoundaries(mode);
      } else if (mode == "cancel-review") {
        const auto staging = flow::stage;
        runtime::CloseIssueReport();
        // Review cancellation uses the production cleanup thread. ReShade is
        // the only substituted boundary; module references use real Win32 APIs.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (flow::worker_active && std::chrono::steady_clock::now() < deadline) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Require(!flow::worker_active, "cleanup worker completes within bound");
        Require(flow::step == flow::Step::kClosed && !std::filesystem::exists(staging), "cancel review removes staging");
        Require(std::filesystem::exists(base_directory / "Saved" / "pre.png"), "cancel retains source capture");
      } else if (mode == "upload-consent") {
        Require(!flow::upload_consent, "upload consent defaults off");
        flow::test_reason.clear();
        runtime::SendIssueReport(true);
        Require(flow::step == flow::Step::kReview && !flow::worker_active && !flow::upload,
                "upload without consent refused before worker/network");
        flow::test_reason = "fixture";
        flow::step = flow::Step::kDone;
        runtime::CloseIssueReport();
      } else if (mode == "test-empty-url") {
        flow::test_url.clear();
        // Keep configured hosts disabled even if a future regression removes
        // the production test-mode override; this fixture must stay local.
        flow::host_setting = "none";
        runtime::SendIssueReport(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (flow::worker_active && std::chrono::steady_clock::now() < deadline) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Require(!flow::worker_active && flow::step == flow::Step::kDone, "test upload worker completed");
        report::ParsedReportUrl parsed;
        Require(report::ParseReportUrl(flow::test_url, &parsed) && parsed.loopback,
                "empty test override replaced by loopback refusal, never configured host");
        Require(flow::link.empty() && !flow::error.empty(), "refused upload has no link and visible failure");
        Require(!flow::zip.empty() && std::filesystem::exists(flow::zip), "failed test upload retains ZIP");
        runtime::CloseIssueReport();
      } else {
        Save(mode);
      }
    }
    std::cout << checks << " checks, 0 failures\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
