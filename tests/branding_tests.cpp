/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#include <cstdio>
#include <cstring>
#include <string_view>

#include "brand.h"
#include "ui/strings.hpp"
#include "version.h"

int main() {
  using namespace renodx::addons::dlss5::ui;
  static_assert(std::string_view(NR_NEXUS_DEBUG_NAME) == std::string_view(NR_NEXUS_NAME " (Debug)"));
  static_assert(std::string_view(NR_NEXUS_DESCRIPTION) == "DLSS Neural Rendering Add-on for RenoDX");
#if defined(RENODX_DLSS5_DEBUG)
  static_assert(std::string_view(NR_NEXUS_ADDON_NAME) == NR_NEXUS_DEBUG_NAME);
#else
  static_assert(std::string_view(NR_NEXUS_ADDON_NAME) == NR_NEXUS_NAME);
#endif
  char footer[256];
  std::snprintf(footer, sizeof(footer), text::kFooter, "v" NR_NEXUS_VERSION, "test-runtime", "test.exe");
  if (std::string_view(footer) != NR_NEXUS_NAME " v" NR_NEXUS_VERSION " | DLSSNR test-runtime | test.exe") return 1;
  for (int language = 0; language < kLanguageCount; ++language) {
    SelectLanguage(language);
    const std::string_view welcome = Tr(text::kWelcomeTitle);
    const std::string_view crash = Tr("Crash log (" NR_NEXUS_NAME ")");
    const std::string_view copies = Tr("Keep one renodx-dlss5.addon64 file in the game's folder, remove the others, and restart the game.");
    if (welcome.find(NR_NEXUS_NAME) == welcome.npos || crash.find(NR_NEXUS_NAME) == crash.npos
        || copies.find("renodx-dlss5.addon64") == copies.npos) return 2;
    if (language != 0 && (welcome == text::kWelcomeTitle || crash == "Crash log (" NR_NEXUS_NAME ")")) return 3;
    if (FormatConversions(text::kFooter) != "%s|%s|%s|") return 4;
    std::printf("%s: %s | %s\n", kLanguages[language].code, welcome.data(), crash.data());
  }
  std::printf("%s: %s\n", NR_NEXUS_ADDON_NAME, footer);
  return 0;
}
