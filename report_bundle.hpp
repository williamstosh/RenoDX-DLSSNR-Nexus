/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// The issue report's bundle (rc11): redaction and the zip the report ships
// as.  No addon state here - test/dlss5 runs all of it without a device.
//
// Callers must pass every text through RedactText, or RedactJson for JSON,
// before adding it to the ZIP. Entry names are the bundle's own. Redaction
// removes recognizable credentials and sensitive structured fields, plus:
//   - the Windows profile folder (C:\Users\Example and its 8.3 short form
//     C:\Users\EXAMPL~1 that GetTempPath and old APIs hand out) becomes
//     %USERPROFILE%, whatever the separators (a log may escape '\' as "\\"
//     or write '/') and whatever the case;
//   - the user name anywhere else it stands as a word (D:\<name>\Games, a
//     "user=<name>" field) becomes <user>.
// The F5 PNGs are pixels the addon wrote and carry no path.
//
// The zip is store-only, written by the code below: AGENTS.md rules out a new
// vendored library, and the logs are small.  Entries are streamed from disk
// with the local header patched after the data, so a 4K PNG never sits in
// memory.  Windows' own reader (Expand-Archive) reads it back in test/dlss5.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

namespace renodx::addons::dlss5::report {

inline std::string Utf8(std::wstring_view wide) {
  if (wide.empty()) return {};
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                        nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>((std::max)(bytes, 0)), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), bytes,
                      nullptr, nullptr);
  return out;
}

// ---------------------------------------------------------------------------
// Redaction
// ---------------------------------------------------------------------------

struct Redaction {
  std::string profile;        // "C:\Users\Example UTF-8
  std::string profile_short;  // "C:\Users\EXAMPL~1 or empty when it has none
  std::string user;           // "Name"
};

// The running user's, from the process environment (USERPROFILE and
// USERNAME, what every path the game and ReShade log is built from) and the
// profile's short form.  Environment reads only: safe on any thread.
inline Redaction SystemRedaction() {
  const auto variable = [](const wchar_t* name) {
    std::wstring value(32768, L'\0');
    const DWORD length = GetEnvironmentVariableW(name, value.data(),
                                                 static_cast<DWORD>(value.size()));
    value.resize(length < value.size() ? length : 0);
    return value;
  };
  std::wstring profile = variable(L"USERPROFILE");
  while (!profile.empty() && (profile.back() == L'\\' || profile.back() == L'/')) {
    profile.pop_back();
  }
  Redaction redaction{Utf8(profile), {}, Utf8(variable(L"USERNAME"))};
  if (!profile.empty()) {
    std::wstring shortened(32768, L'\0');
    const DWORD length = GetShortPathNameW(profile.c_str(), shortened.data(),
                                           static_cast<DWORD>(shortened.size()));
    if (length != 0 && length < shortened.size()) {
      shortened.resize(length);
      if (_wcsicmp(shortened.c_str(), profile.c_str()) != 0) {
        redaction.profile_short = Utf8(shortened);
      }
    }
  }
  return redaction;
}

inline char AsciiLower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

inline bool IsPathSeparator(char c) { return c == '\\' || c == '/'; }

// A character that continues a name: a boundary is anything else.  Non-ASCII
// UTF-8 bytes continue it, so a name never ends inside an accented letter.
inline bool IsNameChar(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'
         || static_cast<unsigned char>(c) >= 0x80;
}

// The length of `path` matched at text[at], 0 when it does not match: ASCII
// case folded, and a run of separators in the text standing for each
// separator of the path.  The match must end at a name boundary, so the
// profile C:\Users\Example never matches C:\Users\ExamplePlus
inline size_t MatchPath(std::string_view text, size_t at, std::string_view path) {
  if (path.empty()) return 0;
  size_t t = at;
  for (size_t p = 0; p < path.size(); ++p) {
    if (t >= text.size()) return 0;
    if (IsPathSeparator(path[p])) {
      if (!IsPathSeparator(text[t])) return 0;
      while (t < text.size() && IsPathSeparator(text[t])) ++t;
      while (p + 1 < path.size() && IsPathSeparator(path[p + 1])) ++p;
      continue;
    }
    if (AsciiLower(text[t]) != AsciiLower(path[p])) return 0;
    ++t;
  }
  return t < text.size() && IsNameChar(text[t]) ? 0 : t - at;
}

// Shared by structured fields and log/config assignments. Normalize spelling,
// not substrings: diagnostic fields such as token_count must remain useful.
inline bool IsSensitiveReportKey(std::string_view key) {
  std::string normalized;
  for (char c : key) {
    if (c != '_' && c != '-' && c != ' ') normalized += AsciiLower(c);
  }
  constexpr std::string_view keys[] = {
      "authorization", "proxyauthorization", "cookie", "setcookie", "password", "passwd", "pwd",
      "secret", "clientsecret", "token", "accesstoken", "refreshtoken", "idtoken", "apikey", "xapikey",
      "credential", "credentials", "awsaccesskeyid", "awssecretaccesskey", "awssessiontoken",
      "githubtoken", "apitoken", "authtoken", "sessiontoken", "personalaccesstoken",
      "accesskeyid", "secretaccesskey", "secretkey", "privatekey",
      "username", "userdomain", "computername"};
  return std::find(std::begin(keys), std::end(keys), normalized) != std::end(keys);
}

inline std::string RedactText(std::string_view text, const Redaction& redaction) {
  std::string out;
  out.reserve(text.size());
  for (size_t at = 0; at < text.size();) {
    // Export runs before preview and again after optional fields are added.
    // Do not reinterpret our markers as real usernames (e.g. Windows "User").
    bool marker = false;
    for (const std::string_view replacement : {"%USERPROFILE%", "<user>", "<redacted>", "<report-link>"}) {
      if (text.substr(at).starts_with(replacement)) {
        out.append(replacement);
        at += replacement.size();
        marker = true;
        break;
      }
    }
    if (marker) continue;
    const auto equal_at = [&](std::string_view value) {
      return at + value.size() <= text.size()
             && std::equal(value.begin(), value.end(), text.begin() + at,
                           [](char a, char b) { return AsciiLower(a) == AsciiLower(b); });
    };
    const bool boundary = at == 0 || !IsNameChar(text[at - 1]);
    if (boundary) {
      // Legacy logs may contain a download capability in the path alone.
      // Match exact upload hosts; unrelated documentation URLs stay useful.
      if (equal_at("https://") || equal_at("http://")) {
        const size_t authority_at = at + (equal_at("https://") ? 8 : 7);
        size_t url_end = authority_at;
        while (url_end < text.size()) {
          const char c = text[url_end];
          if (static_cast<unsigned char>(c) <= ' ' || c == '"' || c == '\'' || c == '<' || c == '>' || c == '\\') break;
          ++url_end;
        }
        const size_t authority_end = (std::min)(text.find_first_of("/?#", authority_at), url_end);
        const auto authority = text.substr(authority_at, authority_end - authority_at);
        const size_t userinfo = authority.rfind('@');
        const size_t host_at = authority_at + (userinfo == std::string_view::npos ? 0 : userinfo + 1);
        auto host = text.substr(host_at, authority_end - host_at);
        host = host.substr(0, host.find(':'));
        bool report_host = false;
        for (const std::string_view known : {"litterbox.catbox.moe", "litter.catbox.moe", "files.catbox.moe", "temp.sh"}) {
          if (host.size() == known.size()
              && std::equal(host.begin(), host.end(), known.begin(),
                            [](char a, char b) { return AsciiLower(a) == b; })) {
            report_host = true;
            break;
          }
        }
        if (report_host && authority_end < url_end) {
          out += "<report-link>";
          at = url_end;
          continue;
        }
        if (userinfo != std::string_view::npos) {
          out.append(text.substr(at, authority_at - at));
          out += "<redacted>@";
          at = host_at;
          continue;
        }
      }
      size_t key_end = at;
      while (key_end < text.size() && (IsNameChar(text[key_end]) || text[key_end] == '-')) ++key_end;
      size_t value_at = key_end;
      if (value_at < text.size() && (text[value_at] == '\'' || text[value_at] == '"')) ++value_at;
      while (value_at < text.size() && (text[value_at] == ' ' || text[value_at] == '\t')) ++value_at;
      const bool assignment = value_at < text.size() && (text[value_at] == '=' || text[value_at] == ':');
      const bool bearer = equal_at("Bearer ") || equal_at("Basic ")
                          || equal_at("Bearer\t") || equal_at("Basic\t");
      if ((assignment && IsSensitiveReportKey(text.substr(at, key_end - at))) || bearer) {
        value_at = (bearer ? key_end : value_at + 1);
        while (value_at < text.size() && (text[value_at] == ' ' || text[value_at] == '\t')) ++value_at;
        char quote = '\0';
        if (value_at < text.size() && (text[value_at] == '\'' || text[value_at] == '"')) {
          quote = text[value_at++];
        }
        size_t end = value_at;
        const bool header = equal_at("authorization") || equal_at("proxy-authorization")
                            || equal_at("cookie") || equal_at("set-cookie");
        if (quote == '\0' && end < text.size() && (text[end] == '{' || text[end] == '[')) {
          // A credential may itself be an object/array embedded in a log or
          // report description. Delimit its complete JSON value before hiding
          // it; punctuation inside escaped strings must not end the match.
          std::string closing;
          bool in_string = false;
          bool complete = false;
          for (; end < text.size(); ++end) {
            const char c = text[end];
            if (in_string) {
              if (c == '\\' && end + 1 < text.size()) {
                ++end;
              } else if (c == '"') {
                in_string = false;
              }
              continue;
            }
            if (c == '"') {
              in_string = true;
            } else if (c == '{' || c == '[') {
              closing.push_back(c == '{' ? '}' : ']');
            } else if (c == '}' || c == ']') {
              if (closing.empty() || c != closing.back()) break;
              closing.pop_back();
              if (closing.empty()) {
                ++end;
                complete = nlohmann::json::accept(text.begin() + value_at, text.begin() + end);
                break;
              }
            }
          }
          if (!complete) end = (std::min)(text.find_first_of("\r\n", value_at), text.size());
        } else {
          while (end < text.size()) {
            const char c = text[end];
            if (c == '\r' || c == '\n' || (quote != '\0' && c == quote)) break;
            if (quote == '\0' && !header
                && (c == '&' || c == '#' || c == ',' || c == ';' || c == '"' || c == '\''
                    || c == '}' || c == ']' || c == ' ' || c == '\t')) break;
            if (quote != '\0' && c == '\\' && end + 1 < text.size()) ++end;
            ++end;
          }
        }
        out.append(text.substr(at, value_at - at));
        out += "<redacted>";
        at = end;
        continue;
      }

      const bool known_token = equal_at("github_pat_") || equal_at("ghp_") || equal_at("gho_")
                               || equal_at("ghu_") || equal_at("ghs_") || equal_at("ghr_")
                               || equal_at("AKIA") || equal_at("ASIA") || equal_at("sk-")
                               || equal_at("xoxb-") || equal_at("xoxp-") || equal_at("xoxs-");
      size_t token_end = at;
      if (known_token || equal_at("eyJ")) {
        while (token_end < text.size()
               && (IsNameChar(text[token_end]) || text[token_end] == '-' || text[token_end] == '.')) ++token_end;
        const auto token = text.substr(at, token_end - at);
        const bool jwt = token.starts_with("eyJ") && std::count(token.begin(), token.end(), '.') >= 2;
        if (known_token || jwt) {
          out += "<redacted>";
          at = token_end;
          continue;
        }
      }
    }
    size_t matched = MatchPath(text, at, redaction.profile);
    if (matched == 0) matched = MatchPath(text, at, redaction.profile_short);
    if (matched != 0) {
      out += "%USERPROFILE%";
      at += matched;
      continue;
    }
    const std::string_view user = redaction.user;
    if (!user.empty() && (at == 0 || !IsNameChar(text[at - 1]))
        && at + user.size() <= text.size()
        && (at + user.size() == text.size() || !IsNameChar(text[at + user.size()]))
        && std::equal(user.begin(), user.end(), text.begin() + at,
                      [](char a, char b) { return AsciiLower(a) == AsciiLower(b); })) {
      out += "<user>";
      at += user.size();
      continue;
    }
    out.push_back(text[at++]);
  }
  return out;
}

// A file's digest as upper-case hex (BCrypt: BCRYPT_MD5_ALGORITHM for the
// addon's MD5, the key artifacts/dlss5/index.json maps binaries by); empty
// when the file or the provider cannot be opened.
inline std::string FileDigestHex(const std::filesystem::path& path, const wchar_t* algorithm) {
  std::ifstream file(path, std::ios::binary);
  BCRYPT_ALG_HANDLE provider = nullptr;
  if (!file || !BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&provider, algorithm, nullptr, 0))) {
    return {};
  }
  BCRYPT_HASH_HANDLE hash = nullptr;
  DWORD digest_bytes = 0;
  ULONG written = 0;
  std::string out;
  if (BCRYPT_SUCCESS(BCryptGetProperty(provider, BCRYPT_HASH_LENGTH,
                                       reinterpret_cast<PUCHAR>(&digest_bytes),
                                       sizeof(digest_bytes), &written, 0))
      && BCRYPT_SUCCESS(BCryptCreateHash(provider, &hash, nullptr, 0, nullptr, 0, 0))) {
    std::vector<char> chunk(1 << 16);
    while (file) {
      file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
      if (file.gcount() <= 0) break;
      BCryptHashData(hash, reinterpret_cast<PUCHAR>(chunk.data()),
                     static_cast<ULONG>(file.gcount()), 0);
    }
    std::vector<uint8_t> digest(digest_bytes);
    if (BCRYPT_SUCCESS(BCryptFinishHash(hash, digest.data(), digest_bytes, 0))) {
      for (const uint8_t byte : digest) {
        out.push_back("0123456789ABCDEF"[byte >> 4]);
        out.push_back("0123456789ABCDEF"[byte & 15]);
      }
    }
    BCryptDestroyHash(hash);
  }
  BCryptCloseAlgorithmProvider(provider, 0);
  return out;
}

// ---------------------------------------------------------------------------
// report.json
// ---------------------------------------------------------------------------

inline std::string JsonString(std::string_view text) {
  std::string out = "\"";
  for (const char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char escaped[8];
          std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(c));
          out += escaped;
        } else {
          out.push_back(c);
        }
    }
  }
  return out + "\"";
}

// Parse before redacting so escaped separators, Unicode escapes and quotes are
// treated as data. On invalid JSON clear the output; callers must abort export
// on false rather than fall back to the unredacted source. Keys and duplicate
// fields are processed by the parser callback before the DOM can discard them.
// Uses the host's existing nlohmann JSON dependency; no new library is vendored.
inline bool RedactJson(std::string_view text, const Redaction& redaction, std::string* output) {
  if (output == nullptr) return false;
  using Json = nlohmann::ordered_json;
  const auto callback = [&](int, Json::parse_event_t event, Json& value) {
    if (event == Json::parse_event_t::key) {
      const auto key = value.get<std::string>();
      if (!IsSensitiveReportKey(key)) value = RedactText(key, redaction);
    } else if (event == Json::parse_event_t::value && value.is_string()) {
      value = RedactText(value.get_ref<const std::string&>(), redaction);
    }
    return true;
  };
  try {
    auto document = Json::parse(text.begin(), text.end(), callback);
    const auto redact_fields = [&](auto&& self, Json* node) -> void {
      if (node->is_object()) {
        for (auto it = node->begin(); it != node->end(); ++it) {
          if (IsSensitiveReportKey(it.key())) {
            it.value() = "<redacted>";
          } else {
            self(self, &it.value());
          }
        }
      } else if (node->is_array()) {
        for (auto& value : *node) self(self, &value);
      }
    };
    redact_fields(redact_fields, &document);
    *output = document.dump(2);
    return true;
  } catch (const Json::exception&) {
    output->clear();
    return false;
  }
}

// ---------------------------------------------------------------------------
// Zip (PKWARE APPNOTE 6.3: store method, UTF-8 names, no zip64)
// ---------------------------------------------------------------------------

inline const std::array<uint32_t, 256>& Crc32Table() {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> out{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t crc = i;
      for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
      out[i] = crc;
    }
    return out;
  }();
  return table;
}

// Streaming CRC-32: start from 0, feed chunks in order.
inline uint32_t Crc32(uint32_t crc, const void* data, size_t size) {
  const auto& table = Crc32Table();
  crc = ~crc;
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) crc = table[(crc ^ bytes[i]) & 0xFFu] ^ (crc >> 8);
  return ~crc;
}

class ZipWriter {
 public:
  // Store-only zips cap every size at 4 GiB - 1; a bundle is far below.
  static constexpr uint64_t kMaxBytes = 0xFFFFFFFEull;

  explicit ZipWriter(const std::filesystem::path& path)
      : out_(path, std::ios::binary | std::ios::trunc) {
    ok_ = out_.good();
  }

  bool Ok() const { return ok_; }
  uint64_t Bytes() const { return offset_; }

  bool AddBytes(std::string_view name, const void* data, size_t size) {
    if (size > kMaxBytes || offset_ + size + 30 + name.size() > kMaxBytes
        || (size != 0 && data == nullptr)) return ok_ = false;
    if (!Begin(name)) return false;
    Entry& entry = entries_.back();
    entry.crc = Crc32(0, data, size);
    entry.size = static_cast<uint32_t>(size);
    out_.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return End(size);
  }

  bool AddFile(std::string_view name, const std::filesystem::path& source) {
    // A selected attachment must be present and completely read. Otherwise
    // Finish must fail even when a caller overlooks this return value.
    std::ifstream in(source, std::ios::binary);
    if (!in) return ok_ = false;
    if (!Begin(name)) return false;
    std::vector<char> chunk(1 << 20);
    uint64_t size = 0;
    uint32_t crc = 0;
    while (in) {
      in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
      const std::streamsize read = in.gcount();
      if (read <= 0) break;
      if (size + static_cast<uint64_t>(read) > kMaxBytes
          || offset_ + size + static_cast<uint64_t>(read) > kMaxBytes) return ok_ = false;
      crc = Crc32(crc, chunk.data(), static_cast<size_t>(read));
      out_.write(chunk.data(), read);
      size += static_cast<uint64_t>(read);
    }
    if (in.bad() || (in.fail() && !in.eof())) return ok_ = false;
    Entry& entry = entries_.back();
    entry.crc = crc;
    entry.size = static_cast<uint32_t>(size);
    return End(size);
  }

  // The central directory.  The zip is complete only after this returns true.
  bool Finish() {
    if (!ok_) return false;
    const uint64_t directory_offset = offset_;
    std::string directory;
    for (const Entry& entry : entries_) {
      AppendLe32(&directory, 0x02014B50u);
      AppendLe16(&directory, 20);  // made by: 2.0, MS-DOS attributes
      AppendLe16(&directory, 20);  // needed: 2.0
      AppendLe16(&directory, kUtf8Flag);
      AppendLe16(&directory, 0);  // stored
      AppendLe16(&directory, entry.dos_time);
      AppendLe16(&directory, entry.dos_date);
      AppendLe32(&directory, entry.crc);
      AppendLe32(&directory, entry.size);
      AppendLe32(&directory, entry.size);
      AppendLe16(&directory, static_cast<uint16_t>(entry.name.size()));
      AppendLe16(&directory, 0);  // extra
      AppendLe16(&directory, 0);  // comment
      AppendLe16(&directory, 0);  // disk
      AppendLe16(&directory, 0);  // internal attributes
      AppendLe32(&directory, 0);  // external attributes
      AppendLe32(&directory, entry.offset);
      directory += entry.name;
    }
    std::string end;
    AppendLe32(&end, 0x06054B50u);
    AppendLe16(&end, 0);
    AppendLe16(&end, 0);
    AppendLe16(&end, static_cast<uint16_t>(entries_.size()));
    AppendLe16(&end, static_cast<uint16_t>(entries_.size()));
    AppendLe32(&end, static_cast<uint32_t>(directory.size()));
    AppendLe32(&end, static_cast<uint32_t>(directory_offset));
    AppendLe16(&end, 0);
    out_.write(directory.data(), static_cast<std::streamsize>(directory.size()));
    out_.write(end.data(), static_cast<std::streamsize>(end.size()));
    offset_ += directory.size() + end.size();
    out_.flush();
    ok_ = out_.good() && offset_ <= kMaxBytes && entries_.size() < 0xFFFF;
    out_.close();
    return ok_;
  }

 private:
  static constexpr uint16_t kUtf8Flag = 0x0800;

  struct Entry {
    std::string name;
    uint32_t offset = 0;
    uint32_t crc = 0;
    uint32_t size = 0;
    uint16_t dos_time = 0;
    uint16_t dos_date = 0;
  };

  static void AppendLe16(std::string* out, uint16_t value) {
    out->push_back(static_cast<char>(value & 0xFF));
    out->push_back(static_cast<char>(value >> 8));
  }
  static void AppendLe32(std::string* out, uint32_t value) {
    AppendLe16(out, static_cast<uint16_t>(value & 0xFFFF));
    AppendLe16(out, static_cast<uint16_t>(value >> 16));
  }

  // Writes the local header with the CRC and sizes zeroed; End patches them.
  bool Begin(std::string_view name) {
    if (!ok_ || name.empty() || name.size() > 0xFFFF || offset_ > kMaxBytes) return ok_ = false;
    if (std::any_of(entries_.begin(), entries_.end(), [&](const Entry& entry) { return entry.name == name; })) {
      return ok_ = false;
    }
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    Entry entry;
    entry.name = name;
    entry.offset = static_cast<uint32_t>(offset_);
    entry.dos_time = static_cast<uint16_t>((local.tm_hour << 11) | (local.tm_min << 5)
                                           | (local.tm_sec / 2));
    entry.dos_date = static_cast<uint16_t>(((std::max)(local.tm_year - 80, 0) << 9)
                                           | ((local.tm_mon + 1) << 5) | local.tm_mday);
    std::string header;
    AppendLe32(&header, 0x04034B50u);
    AppendLe16(&header, 20);
    AppendLe16(&header, kUtf8Flag);
    AppendLe16(&header, 0);
    AppendLe16(&header, entry.dos_time);
    AppendLe16(&header, entry.dos_date);
    AppendLe32(&header, 0);  // crc, patched
    AppendLe32(&header, 0);  // compressed size, patched
    AppendLe32(&header, 0);  // size, patched
    AppendLe16(&header, static_cast<uint16_t>(name.size()));
    AppendLe16(&header, 0);
    header += name;
    out_.write(header.data(), static_cast<std::streamsize>(header.size()));
    offset_ += header.size();
    entries_.push_back(std::move(entry));
    return ok_ = out_.good();
  }

  bool End(uint64_t size) {
    const Entry& entry = entries_.back();
    if (size > kMaxBytes || offset_ + size > kMaxBytes) return ok_ = false;
    std::string patch;
    AppendLe32(&patch, entry.crc);
    AppendLe32(&patch, entry.size);
    AppendLe32(&patch, entry.size);
    const std::streamoff resume = out_.tellp();
    out_.seekp(static_cast<std::streamoff>(entry.offset) + 14);
    out_.write(patch.data(), static_cast<std::streamsize>(patch.size()));
    out_.seekp(resume);
    offset_ += size;
    return ok_ = out_.good();
  }

  std::ofstream out_;
  std::vector<Entry> entries_;
  uint64_t offset_ = 0;
  bool ok_ = false;
};

}  // namespace renodx::addons::dlss5::report
