/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

#include "../report_bundle.hpp"

namespace report = renodx::addons::dlss5::report;
using Json = nlohmann::ordered_json;

void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

int main(int argc, char** argv) {
  try {
    Require(argc == 2, "Pass an empty test output directory");
    const std::filesystem::path directory(argv[1]);
    Require(std::filesystem::is_empty(directory), "Test output directory must be empty");
    const report::Redaction privacy{.profile = R"(C:\Users\Example)",
                                    .profile_short = R"(C:\Users\EXAMPL~1)", .user = "Example"};
    const std::string paths = R"(c:/USERS/EXAMPLE/log C:\\Users\\EXAMPL~1\\log D:\Example\Games user=EXAMPLE ExamplePlus)";
    const auto cleaned_paths = report::RedactText(paths, privacy);
    Require(cleaned_paths == R"(%USERPROFILE%/log %USERPROFILE%\\log D:\<user>\Games user=<user> ExamplePlus)",
            "Path normalization and user boundaries");

    constexpr std::string_view secrets[] = {
        "ghp_SYNTHETIC123", "github_pat_SYNTHETIC123", "AKIA1234567890123456", "ASIA1234567890123456",
        "eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjMifQ.signature", "sk-synthetic-key", "xoxb-synthetic-token"};
    for (const auto secret : secrets) {
      const auto clean = report::RedactText(std::string("value ") + std::string(secret) + " done", privacy);
      Require(clean == "value <redacted> done", "Recognizable credential");
    }
    const std::string assignments =
        "Authorization: Bearer private-value\nProxy-Authorization: Basic dXNlcjpwYXNz\n"
        "Authorization: Digest username=\"private\", response=\"private\"\n"
        "Cookie: one=private; two=also-private\n"
        "password=\"a secret with spaces\" api_key='another secret'\n"
        "https://example.invalid/?token=query-value&width=1920&client_secret=hidden#fragment\n"
        "Bearer standalone-value\nBasic\tdXNlcjpwYXNz\nAWS_SECRET_ACCESS_KEY=opaque-value\n";
    const auto cleaned = report::RedactText(assignments, privacy);
    for (const auto secret : {"private", "dXNlcjpwYXNz", "a secret", "another secret", "query-value", "hidden", "opaque-value", "standalone-value"}) {
      Require(cleaned.find(secret) == std::string::npos, "Assignment/header credential");
    }
    Require(cleaned.find("width=1920") != std::string::npos, "Preserve query diagnostic");
    Require(report::RedactText("fps=60 token_count=42 shader=abc123", privacy)
                == "fps=60 token_count=42 shader=abc123", "Preserve text diagnostics");
    Require(report::RedactText("token=opaque", {}) == "token=<redacted>", "No environment required for credentials");
    for (const auto nested : {R"({"credentials":{"value":"opaque-secret"},"fps":60})",
                              R"({"credentials":[{"value":"opaque-secret"},["opaque-secret",{"other":true}]],"fps":60})",
                              R"({"credentials":{"value":"opaque-secret \"quoted\" } ] \\ tail"},"fps":60})",
                              "credentials={\n\"value\":[\"opaque-secret\"]\n} fps=60"}) {
      const auto clean = report::RedactText(nested, {});
      Require(clean.find("opaque-secret") == std::string::npos && clean.find("fps") != std::string::npos,
              "Sensitive JSON containers in text removed completely");
      Require(report::RedactText(clean, {}) == clean, "Container text redaction is idempotent");
      const std::string envelope = Json{{"description", nested}, {"fps", 60}}.dump();
      std::string clean_json;
      Require(report::RedactJson(envelope, {}, &clean_json) && clean_json.find("opaque-secret") == std::string::npos
                  && Json::parse(clean_json)["fps"] == 60, "Nested credentials in report string removed");
    }
    for (const auto malformed : {R"(credentials={"value":"opaque-secret" fps=60)",
                                 R"(credentials=[{"value":"opaque-secret"} trailing-secret)",
                                 R"(credentials={"value":["opaque-secret"} trailing-secret)",
                                 R"(credentials={"value":"opaque-secret",bad:true} trailing-secret)"}) {
      const std::string input_line = std::string(malformed) + "\nfps=60";
      const auto clean = report::RedactText(input_line, {});
      Require(clean.find("opaque-secret") == std::string::npos && clean.ends_with("\nfps=60"),
              "Malformed container preserves next diagnostic line");
      Require(clean == "credentials=<redacted>\nfps=60", "Malformed container redacted through end of line");
    }
    for (const auto link : {"https://litterbox.catbox.moe/synthetic.zip", "https://litter.catbox.moe/synthetic.zip",
                            "https://files.catbox.moe/synthetic.zip",
                            "https://temp.sh/synthetic/report.zip", "HTTP://TEMP.SH:80/synthetic?download=1#fragment",
                            "https://oauth:synthetic-password@temp.sh/synthetic.zip"}) {
      const auto first = report::RedactText(std::string("Uploaded: ") + link + " done", {});
      Require(first == "Uploaded: <report-link> done", "Temporary report link removed in full");
      Require(report::RedactText(first, {.user = "report"}) == first, "Report link marker is idempotent");
    }
    const std::string technical = "https://learn.microsoft.com/windows/api https://temp.sh.evil.invalid/docs "
                                  "https://example.invalid/temp.sh/path https://files.catbox.moe.evil.invalid/docs";
    Require(report::RedactText(technical, {}) == technical, "General technical URLs and hostname boundaries preserved");
    for (const auto link : {"https://oauth:synthetic-password@example.invalid/docs", "http://synthetic-user@example.invalid/docs",
                            "https://encoded%40user:p%3Ass@example.invalid/docs"}) {
      const auto first = report::RedactText(link, {});
      Require(first == (std::string(link).starts_with("https") ? "https://<redacted>@example.invalid/docs"
                                                               : "http://<redacted>@example.invalid/docs"), "URL userinfo removed");
      Require(report::RedactText(first, {}) == first, "URL userinfo redaction is idempotent");
    }
    Require(report::RedactText("https://u:p@example.invalid/docs?token=opaque&fps=60", {})
                == "https://<redacted>@example.invalid/docs?token=<redacted>&fps=60", "Userinfo does not bypass query redaction");
    std::string url_json;
    Require(report::RedactJson(R"({"log":"Uploaded https:\/\/TEMP.SH\/synthetic.zip", "oauth":"https://u:p@example.invalid/docs", "docs":"https://example.invalid/docs"})",
                                {}, &url_json), "Escaped JSON URL accepted");
    const auto urls = Json::parse(url_json);
    Require(urls["log"] == "Uploaded <report-link>" && urls["oauth"] == "https://<redacted>@example.invalid/docs"
                && urls["docs"] == "https://example.invalid/docs", "Unified JSON URL redaction");
    std::string url_json_again;
    Require(report::RedactJson(url_json, {}, &url_json_again) && url_json == url_json_again, "JSON URL redaction is idempotent");
    for (const auto name : {"User", "redacted", "USERPROFILE"}) {
      const report::Redaction marker_user{.profile = std::string(R"(C:\Users\)") + name, .user = name};
      const auto first = report::RedactText(marker_user.profile + " " + name + " token=opaque", marker_user);
      Require(report::RedactText(first, marker_user) == first, "Markers stay stable for matching usernames");
    }

    const std::string input = R"({
      "path":"C:\\Users\\Example\\logs\\report.txt",
      "escaped":"c:\u005cUsers\u005cEXAMPLE\u005cfile.txt",
      "quoted":"line\n\"C:\\Users\\EXAMPL~1\\a\"\tEXAMPLE",
      "auth":{"access_token":"opaque value", "password":123, "client-secret":{"nested":"private"}},
      "username":"Other User", "computer_name":"SyntheticPC",
      "logs":["Bearer private", "?api_key=private&fps=60", "ghp_SYNTHETIC123"],
      "gpu":"Synthetic GPU", "width":1920, "active":true, "missing":null,
      "token_count":42, "ratio":1.25, "unicode":"\u4e2d\u6587"
    })";
    std::string output;
    Require(report::RedactJson(input, privacy, &output), "Valid JSON accepted");
    const auto document = Json::parse(output);
    Require(document["path"] == R"(%USERPROFILE%\logs\report.txt)", "Escaped Windows path");
    Require(document["escaped"] == R"(%USERPROFILE%\file.txt)", "Unicode escaped separators");
    Require(document["quoted"] == "line\n\"%USERPROFILE%\\a\"\t<user>", "JSON escapes preserved");
    Require(document["auth"]["access_token"] == "<redacted>" && document["auth"]["password"] == "<redacted>"
                && document["auth"]["client-secret"] == "<redacted>", "Structured sensitive values of any type");
    Require(document["username"] == "<redacted>" && document["computer_name"] == "<redacted>", "Windows identity fields");
    Require(output.find("private") == std::string::npos && output.find("SYNTHETIC123") == std::string::npos,
            "Nested JSON secrets absent");
    Require(document["gpu"] == "Synthetic GPU" && document["width"] == 1920 && document["active"] == true
                && document["missing"].is_null() && document["token_count"] == 42 && document["ratio"] == 1.25
                && document["unicode"] == "\xE4\xB8\xAD\xE6\x96\x87", "Diagnostic types and Unicode preserved");
    std::string repeated;
    Require(report::RedactJson(output, privacy, &repeated) && Json::parse(repeated) == document, "Repeat export is stable");
    Require(!report::RedactJson("{bad", privacy, &repeated) && repeated.empty(), "Malformed JSON fails closed");
    Require(!report::RedactJson("{}", privacy, nullptr), "Null output rejected");
    Require(report::RedactJson(R"({"\u0074oken":"private","token":"another-private"})", privacy, &repeated)
                && Json::parse(repeated)["token"] == "<redacted>", "Escaped and duplicate sensitive keys");

    const auto zip_path = directory / "report.zip";
    report::ZipWriter zip(zip_path);
    Require(zip.AddBytes("report.json", output.data(), output.size()), "ZIP JSON entry");
    const auto text_path = directory / "synthetic.txt";
    { std::ofstream text(text_path, std::ios::binary); text << cleaned; }
    Require(zip.AddFile("logs/synthetic.txt", text_path) && zip.Finish(), "ZIP file entry and finish");
    std::ifstream zip_file(zip_path, std::ios::binary);
    const std::string zip_bytes((std::istreambuf_iterator<char>(zip_file)), {});
    Require(zip_bytes.starts_with("PK\x03\x04") && zip_bytes.find("PK\x01\x02") != std::string::npos
                && zip_bytes.find("PK\x05\x06") != std::string::npos, "ZIP header structure");
    Require(zip_bytes.find("report.json") != std::string::npos && zip_bytes.find("logs/synthetic.txt") != std::string::npos
                && zip_bytes.find(output) != std::string::npos && zip_bytes.find(cleaned) != std::string::npos, "ZIP names and contents");
    report::ZipWriter missing(directory / "missing.zip");
    Require(!missing.AddFile("missing.txt", directory / "absent") && !missing.Ok() && !missing.Finish(), "Missing file poisons ZIP");
    report::ZipWriter read_failure(directory / "read-failure.zip");
    Require(!read_failure.AddFile("directory.txt", directory) && !read_failure.Finish(), "Unreadable source poisons ZIP");
    report::ZipWriter duplicate(directory / "duplicate.zip");
    Require(duplicate.AddBytes("report.json", "{}", 2) && !duplicate.AddBytes("report.json", "{}", 2)
                && !duplicate.Finish(), "Duplicate entry poisons ZIP");
    report::ZipWriter invalid(directory / "invalid.zip");
    Require(!invalid.AddBytes("bad.txt", nullptr, 1) && !invalid.Finish(), "Invalid data poisons ZIP");
    report::ZipWriter oversized(directory / "oversized.zip");
    Require(!oversized.AddBytes("bad.txt", "x", report::ZipWriter::kMaxBytes + 1) && !oversized.Finish(),
            "Oversized data rejected before dereference");
    report::ZipWriter unavailable(directory / "absent-directory" / "report.zip");
    Require(!unavailable.Ok() && !unavailable.AddBytes("report.json", "{}", 2) && !unavailable.Finish(),
            "Output open failure propagated");
    std::cout << "Report privacy and ZIP tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
