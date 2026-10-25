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
  const char* report_strings[] = {
      text::kSettingsViewTip, text::kStatusDetails, "Advanced", text::kPassOverview,
      "Multi-Pass", "Pass Count", text::kOpenPresentSettings, text::kAbout,
      text::kRuntimeStatus, text::kResources, text::kDetails, text::kResultShaping,
      text::kResultChanged, text::kPresentSettings, text::kPresentDisplay,
      text::kGeneral, text::kStoredValues, text::kChainNote, text::kModelNote, text::kResourceNote,
      text::kSharedResult, text::kInheritResult, text::kInheritResultTip, text::kInheritedResult,
      text::kRequest, text::kResolved, text::kNetwork, text::kNetworkUnavailable,
      text::kIndependentModel, text::kInheritModel, text::kInput, text::kOutput,
      text::kConfigured, "Runtime default", "Pass 1", "Built-in effect",
      "NR model", "A (Legacy)", "B (Small FP8)", "Inherit main model",
      "Model selection recreates every pass and resets temporal history. Appearance sharing does not change the model.",
      "Automatic selects B on DLSS NR 310.9.16; other runtimes keep their default. Explicit choices are preserved.",
      "B requires DLSS NR 310.9.16. Unsupported selections use A; saved choices are preserved.",
      "Model selection is unavailable until the NR runtime is identified.",
      text::kReportSteps, text::kReportCapture, text::kReportCollect,
      text::kReportWithoutDiagnostics, text::kReportFeature, text::kReportPrivacy,
      text::kReportPreviewLimit, text::kReportOpenPreview, text::kReportPreview,
      text::kReportUploadConsent,
      text::kReportPublicSummaryConsent, text::kReportPublicLinkConsent,
      text::kReportGitHub, text::kReportPlainIssue, text::kReportPublicNotice,
      text::kReportBrowserFailed, text::kReportClipboardFailed,
      text::kReportManualFallback, text::kReportDraftReady, text::kReportLinkReady,
      text::kReportLocalSaved, text::kReportCleaning, text::kReportFailed,
      text::kReportNativeFg, text::kReportColour, text::kReportVisual,
      text::kReportXefg, text::kReportCompatibility};
  for (int language = 0; language < kLanguageCount; ++language) {
    SelectLanguage(language);
    const std::string_view welcome = Tr(text::kWelcomeTitle);
    const std::string_view crash = Tr("Crash log (" NR_NEXUS_NAME ")");
    const std::string_view copies = Tr("Keep one renodx-dlss5.addon64 file in the game's folder, remove the others, and restart the game.");
    if (welcome.find(NR_NEXUS_NAME) == welcome.npos || crash.find(NR_NEXUS_NAME) == crash.npos
        || copies.find("renodx-dlss5.addon64") == copies.npos) return 2;
    if (language != 0 && (welcome == text::kWelcomeTitle || crash == "Crash log (" NR_NEXUS_NAME ")")) return 3;
    if (FormatConversions(text::kFooter) != "%s|%s|%s|") return 4;
    for (const char* english : report_strings) {
      int matches = 0;
      for (const Translation& entry : kLanguages[language].table) {
        if (std::string_view(entry.en) == english) ++matches;
      }
      const char* translated = Tr(english);
      if (language == 0) {
        if (matches != 0 || std::string_view(translated) != english) return 5;
        continue;
      }
      const bool model_identifier = std::string_view(english) == "A (Legacy)"
                                    || std::string_view(english) == "B (Small FP8)" || std::string_view(english) == "Multi-Pass";
      if (matches != 1 || (!model_identifier && std::string_view(translated) == english)) return 5;
      if (FormatConversions(english) != FormatConversions(translated)) return 6;
    }
    std::printf("%s: %s | %s\n", kLanguages[language].code, welcome.data(), crash.data());
  }
  std::printf("%s: %s\n", NR_NEXUS_ADDON_NAME, footer);
  return 0;
}
