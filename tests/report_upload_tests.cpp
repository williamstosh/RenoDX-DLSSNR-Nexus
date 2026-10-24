/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include "../report_upload.hpp"

namespace report = renodx::addons::dlss5::report;

void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

int wmain(int argc, wchar_t** argv) {
  try {
    Require(argc >= 2, "Usage: --unit OR --upload <loopback-url> <zip-path> <success|failure|cancelled> [status]");
    const std::wstring mode(argv[1]);
    if (mode == L"--unit") {
      Require(argc == 2, "Unexpected unit arguments");
      for (const auto url : {L"https://example.invalid/upload?mode=synthetic", L"http://127.0.0.1:12345/upload",
                             L"http://LOCALHOST:12345/upload", L"http://[::1]:12345/upload"}) {
        report::ParsedReportUrl parsed;
        Require(report::ParseReportUrl(url, &parsed), "Valid upload URL");
      }
      report::ParsedReportUrl parsed;
      Require(report::ParseReportUrl(L"https://example.invalid?mode=synthetic", &parsed)
                  && parsed.path == L"/?mode=synthetic" && !parsed.loopback && parsed.secure, "Preserve query path");
      for (const auto url : {L"http://example.invalid/upload", L"http://localhost.evil.invalid/upload",
                             L"http://127.0.0.1.evil.invalid/upload", L"http://127.0.0.2/upload",
                             L"http://127.1/upload", L"http://2130706433/upload", L"http://0177.0.0.1/upload",
                             L"http://localhost./upload", L"http://%6cocalhost/upload", L"ftp://localhost/upload",
                             L"https://user:secret@example.invalid/upload", L"https://@example.invalid/upload",
                             L"http://127.0.0.1:12345@evil.invalid/upload", L"http://localhost:0/upload",
                             L"https:///upload", L"https://example.invalid\\@evil.invalid/upload",
                             L"https://example.invalid/upload#secret", L"https://example.invalid/\r\nheader"}) {
        Require(!report::ParseReportUrl(url, &parsed), "Unsafe upload URL rejected");
        report::UploadProgress progress;
        const auto result = report::Upload(report::kHosts[0], url, L"absent-synthetic.zip", L"NR-test", &progress);
        Require(!result.ok && result.error == "the host URL is malformed or insecure", "Reject URL before file/network");
      }
      Require(!report::ParseReportUrl(std::wstring_view(L"https://example.invalid/\0hidden", 31), &parsed), "Embedded NUL rejected");
      Require(report::IsLink("https://example.invalid/a?secret=value")
                  && report::IsLink("https://example.invalid?secret=value"), "HTTPS result link accepted");
      for (const auto link : {"https://user:secret@example.invalid/a", "https://@example.invalid/a",
                              "https:///missing-host", "http://localhost/a", "https://example.invalid/a\nextra",
                              "https://example.invalid/a\r\nother text", "https://example.invalid/a\nhttps://example.invalid/b",
                              "https://example.invalid/a b", "https://example.invalid/a#secret"}) {
        Require(!report::IsLink(link), "Invalid result link rejected");
      }
      for (const auto link : {"https://example.invalid/path?secret=value", "https://example.invalid?secret=value",
                              "https://user:secret@example.invalid?secret=value", "https://example.invalid#secret",
                              "https://user:secret@example.invalid:443/path"}) {
        Require(report::LinkForLog(link) == "https://example.invalid/<redacted>", "Log only scheme and hostname");
      }
      Require(report::LinkForLog("arbitrary private text") == "<redacted>", "Malformed log link suppressed");
      report::UploadProgress cancelled;
      cancelled.cancel = true;
      const auto result = report::Upload(report::kHosts[0], L"https://example.invalid/upload",
                                          L"absent-synthetic.zip", L"NR-test", &cancelled);
      Require(result.cancelled && !result.ok && result.status == 0 && cancelled.sent == 0 && cancelled.total == 0,
              "Pre-cancel before file, session or network");
      Require(!report::Upload(report::kHosts[0], L"https://example.invalid/upload",
                               L"absent-synthetic.zip", L"NR-test", nullptr).ok, "Null progress rejected");
      std::cout << "Upload unit tests passed\n";
      return 0;
    }
    Require(mode == L"--upload" && (argc == 5 || argc == 6), "Invalid upload CLI");
    report::ParsedReportUrl parsed;
    Require(report::ParseReportUrl(argv[2], &parsed) && parsed.loopback, "Test client only permits exact loopback");
    const std::wstring expected(argv[4]);
    Require(expected == L"success" || expected == L"failure" || expected == L"cancelled", "Invalid expected outcome");
    report::UploadProgress progress;
    progress.cancel = expected == L"cancelled";
    const auto result = report::Upload(report::kHosts[0], argv[2], std::filesystem::path(argv[3]),
                                        L"NR-Nexus-Synthetic-Upload-Test", &progress);
    Require(result.ok == (expected == L"success"), "Unexpected upload success state");
    Require(result.cancelled == (expected == L"cancelled"), "Unexpected upload cancel state");
    if (argc == 6) Require(result.status == std::stoul(argv[5]), "Unexpected HTTP status");
    if (result.ok) {
      Require(report::IsLink(result.link) && result.error.empty(), "Successful link result");
      Require(progress.sent == progress.total && progress.sent != 0, "Multipart upload fully sent");
    } else {
      Require(result.link.empty() && !result.error.empty(), "Failure has no link");
      if (result.status != 0) {
        Require(result.error == "the host answered HTTP " + std::to_string(result.status)
                                    + " without a valid report link", "Generic status error, no body echoed");
      }
    }
    // Never print server-controlled links or error bodies, even on failures.
    std::cout << "Upload expectation passed; status=" << result.status
              << " sent=" << progress.sent.load() << " total=" << progress.total.load() << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
