/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "brand.h"
#include "version.h"

namespace renodx::addons::dlss5::report::issue {

inline constexpr std::string_view NEW_ISSUE_URL =
    "https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/issues/new";
// Bound the fully encoded ASCII URL before handing it to the Windows shell.
inline constexpr std::size_t MAX_URL_BYTES = 2000;

struct Category {
  std::string_view slug;
  std::string_view label;
};

inline constexpr std::array<Category, 9> CATEGORIES = {{
    {"flicker", "Flicker, ghosting or distortion"},
    {"colours", "HDR/SDR or wrong colours"},
    {"crash", "Crash or freeze"},
    {"fps", "Low fps"},
    {"nr_not_working", "NR not working"},
    {"other", "Something else"},
    {"native_fg", "Native FG/MFG issue"},
    {"xefg", "XeFG issue"},
    {"compatibility", "Game compatibility"},
}};

enum class Kind { BUG, FEATURE };

struct Input {
  Kind kind = Kind::BUG;
  std::string title;
  std::vector<std::string> categories;
  std::string game;
  std::string reproduction;
  // Caller supplies reviewed, non-sensitive text, never raw logs or file paths.
  std::string diagnostics;
  std::string report_link;
  // Separate from consent to upload: this authorizes putting the link in a public draft.
  bool include_report_link = false;
};

struct Draft {
  std::string url;
  std::string body;
  bool fallback = false;
  std::string reason;
};

// Input strings are UTF-8; encode their bytes, not locale-dependent characters.
inline std::string PercentEncodeUtf8(std::string_view text) {
  constexpr char HEX[] = "0123456789ABCDEF";
  std::string encoded;
  for (unsigned char byte : text) {
    if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z')
        || (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' || byte == '~') {
      encoded += static_cast<char>(byte);
    } else {
      encoded += '%';
      encoded += HEX[byte >> 4];
      encoded += HEX[byte & 15];
    }
  }
  return encoded;
}

// No clipboard, filesystem, network or browser side effects. On fallback the
// caller copies body before opening url; a clipboard failure must remain visible.
inline Draft BuildDraft(const Input& input) {
  const auto clean = [](std::string_view value, bool multiline) {
    std::string text;
    for (unsigned char byte : value) {
      if (byte == '\r') {
        continue;
      }
      if (byte < 32 || byte == 127) {
        text += (multiline && byte == '\n' ? '\n' : ' ');
      } else {
        text += static_cast<char>(byte);
      }
    }
    return text;
  };

  std::string category;
  for (const auto& slug : input.categories) {
    for (const auto& known : CATEGORIES) {
      if (slug != known.slug) continue;
      if (!category.empty()) {
        category += "; ";
      }
      category += known.label;
      break;
    }
  }
  if (category.empty()) {
    category = "Something else";
  }
  const auto title = clean(input.title, false);
  const auto game = clean(input.game, false);
  const auto reproduction = clean(input.reproduction, true);
  const auto diagnostics = clean(input.diagnostics, true);
  std::string report_link;
  if (input.include_report_link) {
    // A report link is optional, and must be a single HTTPS URL, never a local path or shell scheme.
    if (input.report_link.starts_with("https://") && input.report_link.size() > 8
        && input.report_link.find_first_of(" \t\r\n") == std::string::npos) {
      bool has_control = false;
      for (unsigned char byte : input.report_link) {
        if (byte < 32 || byte == 127) {
          has_control = true;
          break;
        }
      }
      if (!has_control) {
        report_link = input.report_link;
      }
    }
  }

  Draft draft;
  draft.body = "# " NR_NEXUS_NAME "\n\n";
  // Quote and escape user text so embedded headings/HTML cannot masquerade as fields.
  const auto append_field = [&draft](std::string_view label, std::string_view value) {
    draft.body += "## ";
    draft.body += label;
    draft.body += "\n\n> ";
    for (char byte : value) {
      if (byte == '\n') {
        draft.body += "\n> ";
      } else {
        if (std::string_view("\\`*_{}[]<>()#+.!|~&").find(byte) != std::string_view::npos) {
          draft.body += '\\';
        }
        draft.body += byte;
      }
    }
    draft.body += "\n\n";
  };
  append_field("Title / 标题", title);
  append_field("Category / 类别", category);
  append_field("Game / 游戏", game);
  append_field("Version / 版本", NR_NEXUS_VERSION);
  append_field("Reproduction or proposal / 复现步骤或建议", reproduction);
  append_field("Non-sensitive diagnostics / 非敏感诊断", diagnostics);
  if (!report_link.empty()) {
    append_field("Report link / 报告链接", report_link);
  }

  draft.url = NEW_ISSUE_URL;
  draft.url += "?template=";
  draft.url += (input.kind == Kind::FEATURE ? "feature_request.yml" : "bug_report.yml");
  const auto append_query = [&draft](std::string_view key, std::string_view value) {
    draft.url += '&';
    draft.url += key;
    draft.url += '=';
    draft.url += PercentEncodeUtf8(value);
  };
  append_query("title", title);
  append_query("category", category);
  append_query("game", game);
  append_query("version", NR_NEXUS_VERSION);
  append_query("reproduction", reproduction);
  append_query("diagnostics", diagnostics);
  if (!report_link.empty()) {
    append_query("reportlink", report_link);
  }
  if (draft.url.size() > MAX_URL_BYTES) {
    draft.url = NEW_ISSUE_URL;
    draft.fallback = true;
    draft.reason = "encoded_url_too_long";
  }
  return draft;
}

}  // namespace renodx::addons::dlss5::report::issue
