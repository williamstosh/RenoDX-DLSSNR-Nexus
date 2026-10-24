/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include "../report_issue.hpp"

namespace issue = renodx::addons::dlss5::report::issue;

namespace {
int failures = 0;

void Check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void TestEncoding() {
  Check(issue::PercentEncodeUtf8("AZaz09-._~") == "AZaz09-._~", "unreserved ASCII");
  Check(issue::PercentEncodeUtf8(" &=?#%+/:\"\r\n") == "%20%26%3D%3F%23%25%2B%2F%3A%22%0D%0A", "reserved bytes");
  Check(issue::PercentEncodeUtf8("中文🎮") == "%E4%B8%AD%E6%96%87%F0%9F%8E%AE", "UTF-8 bytes");
  Check(issue::PercentEncodeUtf8(std::string("\0\x7f", 2)) == "%00%7F", "NUL and DEL encoding");
}

void TestFormsAndCategories() {
  issue::Input input{
      .title = "中文 & #?",
      .categories = {"nr_not_working", "native_fg", "xefg", "compatibility"},
      .game = "Game & More",
      .reproduction = "First\nSecond",
      .diagnostics = "GPU summary",
  };
  const auto draft = issue::BuildDraft(input);
  Check(!draft.fallback && draft.reason.empty(), "normal draft");
  Check(draft.url.starts_with(std::string(issue::NEW_ISSUE_URL) + "?template=bug_report.yml&title="), "official bug form");
  Check(draft.url.find("title=%E4%B8%AD%E6%96%87%20%26%20%23%3F") != std::string::npos, "encoded title");
  Check(draft.url.find("&category=NR%20not%20working%3B%20Native%20FG%2FMFG%20issue%3B%20XeFG%20issue%3B%20Game%20compatibility") != std::string::npos, "multiple known categories");
  for (std::string_view key : {"&game=", "&version=", "&reproduction=", "&diagnostics="}) {
    Check(draft.url.find(key) != std::string::npos, "form prefill field");
  }
  Check(draft.url.find(issue::PercentEncodeUtf8(NR_NEXUS_VERSION)) != std::string::npos, "version source");
  Check(draft.body.starts_with("# " NR_NEXUS_NAME "\n"), "brand source");
  auto plain_body = draft.body;
  for (auto offset = plain_body.find('\\'); offset != std::string::npos; offset = plain_body.find('\\', offset)) {
    plain_body.erase(offset, 1);
  }
  Check(plain_body.find(NR_NEXUS_VERSION) != std::string::npos, "Markdown version");
  for (const auto& category : issue::CATEGORIES) {
    input.categories = {std::string(category.slug)};
    Check(issue::BuildDraft(input).url.find("&category=" + issue::PercentEncodeUtf8(category.label)) != std::string::npos, "all nine slugs");
  }
  input.kind = issue::Kind::FEATURE;
  Check(issue::BuildDraft(input).url.find("?template=feature_request.yml") != std::string::npos, "feature form");
  input.categories = {"unknown\n&title=injected"};
  Check(issue::BuildDraft(input).url.find("category=Something%20else") != std::string::npos, "unknown category safe default");
}

void TestTextSafety() {
  const auto draft = issue::BuildDraft({
      .title = std::string("title\r\n&labels=evil\0x", 22),
      .game = "Game\n## Spoofed",
      .reproduction = "text\n\n## Spoofed\n<script>alert(1)</script>\n```",
      .diagnostics = "safe\x01\x7f",
  });
  Check(draft.url.find("\r") == std::string::npos && draft.url.find("\n") == std::string::npos, "URL no raw CRLF");
  Check(draft.url.find("&labels=") == std::string::npos, "no injected query key");
  Check(draft.url.find("%00") == std::string::npos, "NUL removed");
  Check(draft.body.find("\n## Spoofed") == std::string::npos, "no injected Markdown heading");
  Check(draft.body.find("<script>") == std::string::npos, "HTML escaped");
  Check(draft.body.find("\n> \\#\\# Spoofed") != std::string::npos, "multiline text remains quoted");
  Check(draft.body.find('\x01') == std::string::npos && draft.body.find('\x7f') == std::string::npos, "controls removed");
}

void TestLinkConsentAndEmptyReport() {
  issue::Input input{.report_link = "https://example.test/report?secret=a&b=2"};
  const auto private_draft = issue::BuildDraft(input);
  Check(private_draft.url.find("example.test") == std::string::npos
            && private_draft.body.find("example.test") == std::string::npos, "default no public link consent");
  input.include_report_link = true;
  const auto public_draft = issue::BuildDraft(input);
  Check(public_draft.url.find("&reportlink=https%3A%2F%2Fexample.test%2Freport%3Fsecret%3Da%26b%3D2") != std::string::npos, "explicit link consent");
  Check(public_draft.body.find("example\\.test") != std::string::npos, "consented link in clipboard body");
  for (const auto* invalid : {"file:///private.zip", "javascript:alert(1)", "https://example.test/\nsecret", "https://"}) {
    input.report_link = invalid;
    Check(issue::BuildDraft(input).url.find("&reportlink=") == std::string::npos, "reject non-link/control scheme");
  }
  const auto empty = issue::BuildDraft({});
  Check(!empty.fallback && !empty.url.empty() && !empty.body.empty(), "empty report needs no files/upload");
  Check(empty.url.find("reportlink") == std::string::npos, "no empty link field");
}

void TestUrlBound() {
  issue::Input input;
  const auto overhead = issue::BuildDraft(input).url.size();
  input.reproduction.assign(issue::MAX_URL_BYTES - overhead, 'a');
  const auto exact = issue::BuildDraft(input);
  Check(!exact.fallback && exact.url.size() == issue::MAX_URL_BYTES, "exact bound accepted");
  input.reproduction += 'a';
  const auto overflow = issue::BuildDraft(input);
  Check(overflow.fallback && overflow.reason == "encoded_url_too_long", "one byte over bound");
  Check(overflow.url == issue::NEW_ISSUE_URL, "ordinary official new issue fallback");
  Check(overflow.body.find(input.reproduction) != std::string::npos, "fallback preserves full body");
  input.reproduction.clear();
  for (int i = 0; i < 500; ++i) {
    input.reproduction += "中文🎮";
  }
  Check(issue::BuildDraft(input).fallback, "bound uses encoded UTF-8 size");
  input.reproduction.assign(20000, 'z');
  Check(issue::BuildDraft(input).url.size() <= issue::MAX_URL_BYTES, "very large input stays bounded");
}
}  // namespace

int main() {
  TestEncoding();
  TestFormsAndCategories();
  TestTextSafety();
  TestLinkConsentAndEmptyReport();
  TestUrlBound();
  if (failures != 0) return EXIT_FAILURE;
  std::cout << "report_issue_tests: all checks passed\n";
  return EXIT_SUCCESS;
}
