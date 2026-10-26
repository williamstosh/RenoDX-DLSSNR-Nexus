/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#include <cstdio>
#include <cstring>
#include <string_view>

#include "brand.h"
#include "preset_fields.hpp"
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
      text::kInterfaceTheme, text::kRestoreTheme, text::kThemeTip, text::kEditing,
      text::kStyleFollows, text::kStyleIndependent, text::kResultFollows,
      text::kResultIndependent, text::kRuntimeUnconfirmed,
      "Precision cyan on deep blue.", "Classic teal and charcoal.",
      "Clear blue on bright surfaces.", "Pure black with restrained green.",
      text::kPresentGuideCountersTip,
      text::kPassCountTip, text::kSettingsViewTips[0], text::kSettingsViewTips[1],
      text::kModelStructure,
      text::kCadenceObservation,
      text::kOptionOff, text::kDetailAuto, text::kDetailAlways, text::kStabilizeStill, text::kStabilizeMotion, text::kUpsampleAuto, text::kUpsampleEdge, text::kUpsampleClassic, text::kUpsampleArea,
      text::kModelStrength, text::kResultStrength, text::kModelStrengthTip, text::kResultStrengthTip, text::kStructureTip, text::kStructureCapped, text::kGuideTip, text::kGuideOptional, text::kGuideRequired, text::kGuideNever, text::kHookUpscaledTip, text::kHookRenderTip, text::kHookPresentTip, text::kResFullTip, text::kResQualityTip, text::kResBalancedTip, text::kResPerformanceTip, text::kResFollowTip, text::kStyleDefaultTip, text::kStyleNaturalTip, text::kStyleCinematicTip, text::kFgEveryTip, text::kFgGameTip, text::kFgDetectAutoTip, text::kFgDetectStableTip, text::kFgDetectLegacyTip, text::kFgStatusUnknown, text::kFgDeclared, text::kFgBeforeUnknown, text::kFgBeforeShort, text::kFgEveryShort, text::kFgGameShort, text::kFgFallbackShort, text::kFgUpscaledShort, text::kFgMixedShort, text::kStackCost, text::kFgUnknownMultiplierTip,
      text::kResolutionLabels[1],
      text::kExpandAll, text::kCollapseAll, text::kChanged, text::kNrResolution, text::kResolutionFine, text::kProcessingLocation, text::kFgAdvanced, text::kFgFramePolicy, text::kFgFramePolicyTip, text::kResultBasic, text::kResultLimits, text::kResultDetail, text::kResultTemporal, text::kTemporalIndependent, text::kModelAutoTip, text::kModelATip, text::kModelBTip, text::kModelInheritTip, text::kChainMarkTip,
      text::kFrameGenUnconfirmed, text::kFrameGenFallback, text::kFgNoMultiplier, text::kFgUnknown, text::kFgOtherStage, text::kFgBefore, text::kFgEvery, text::kFgGame, text::kFgFallback, text::kFgUpscaled, text::kFgMixed, text::kFgDiagnostics,
      text::kSettingsViewTip, text::kStatusDetails, "Advanced", text::kPassOverview,
      "Multi-Pass", "Pass Count", text::kOpenPresentSettings, text::kAbout,
      text::kRuntimeStatus, text::kResources, text::kDetails, text::kResultShaping,
      text::kResultChanged, text::kPresentSettings, text::kPresentDisplay,
      text::kGeneral, text::kChainNote, text::kModelNote, text::kResourceNote,
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
  const char* preset_strings[] = {
      "All groups",
      "Author (optional)",
      "Cancel import",
      "Choose a different preset name.",
      "Clipboard is empty, unavailable or exceeds the preset size limit.",
      "Close",
      "Compatibility",
      "Configuration write failed; previous settings restored.",
      "Configuration write failed; runtime unchanged. Some saved values could not be restored.",
      "Confirm and apply preset",
      "Confirm deletion of selected file",
      "Confirm the full backup scope shown below",
      "Copy share text",
      "Could not start preset worker.",
      "Current",
      "Delete preset",
      "Description (optional)",
      "Duplicate preset",
      "Export file path",
      "Export preset file",
      "Export scope",
      "Exports use My settings, including while Built-in effects is previewed.",
      "File path",
      "Full backups are not guaranteed to work across games.",
      "Full configuration backup",
      "Game-dependent setting",
      "General",
      "Group",
      "Hook, HDR, depth, motion, resolution and FG settings depend on this game and API. Existing runtime gates remain active.",
      "Import a file by its full path. Opening it only prepares a preview.",
      "Import preview",
      "Import undone. Later edits were kept.",
      "Include game compatibility settings",
      "Invalid value",
      "Kept (not in preset)",
      "Local library",
      "Newer or invalid game configuration is read-only.",
      "Only confirmed valid fields in this preset will change. Missing fields keep their current values.",
      "Pass",
      "Preset",
      "Preset applied.",
      "Preset applied. Restart the game for hook policy changes.",
      "Preset deleted. Applied game settings were kept.",
      "Preset export failed. Check the metadata.",
      "Preset file exported.",
      "Preset file saved. Refresh the library to see changes.",
      "Preset JSON copied to clipboard.",
      "Preset name",
      "Preset operation failed. Check the file and directory permissions.",
      "Presets",
      "Preview export fields",
      "Preview selected preset",
      "Quality + Multi-Pass",
      "Quality preset",
      "Read clipboard text",
      "Read preset file",
      "Refresh library",
      "Rename preset",
      "Replace an existing file with this name",
      "Requires game restart",
      "Return to My settings before applying a preset.",
      "Save and share",
      "Save preset to library",
      "Save your settings to create a shared local preset.",
      "Search presets",
      "Select a local preset, read a file or paste share text to preview changes.",
      "Settings changed. Review the refreshed preview and confirm again.",
      "Show unchanged fields",
      "Skipped (compatibility)",
      "Skipped (outside scope)",
      "Source game (optional)",
      "Unchanged",
      "Undo last import",
      "Unsaved preset changes",
      "Unsupported",
      "Unsupported model requests are preserved; the runtime uses the existing fallback policy.",
      "Will change",
      "Working...",
      "Imported preset",
      "Current game settings",
      "Game settings and the shared library are independent.",
      "An absolute local drive path is required.",
      "Directory reparse points and non-directories are not allowed.",
      "Duplicate JSON key.",
      "Invalid preset JSON or UTF-8.",
      "Invalid preset scope.",
      "JSON key exceeds 4096 UTF-8 bytes.",
      "JSON nesting exceeds depth 8.",
      "JSON string exceeds 4096 UTF-8 bytes.",
      "Metadata field has an invalid type or length.",
      "Metadata must be an object.",
      "Metadata tags must be strings of at most 64 UTF-8 bytes.",
      "Metadata tags must contain at most 16 strings.",
      "Missing preset scope.",
      "Missing preset settings; no settings were reset.",
      "Only local disk paths are supported.",
      "Preset already exists; explicit overwrite is required.",
      "Preset exceeds 256 KiB.",
      "Preset files must be regular disk files without reparse points or hard links.",
      "Preset library exceeds the 1024-entry scan limit.",
      "Preset must be an object.",
      "Preset name exceeds 4096 UTF-8 bytes.",
      "Preset name must be valid UTF-8.",
      "Setting key exceeds 4096 UTF-8 bytes.",
      "Setting numbers must be finite.",
      "Setting values must be booleans, finite numbers or strings of at most 128 UTF-8 bytes.",
      "Settings must be an object with at most 512 entries.",
      "Unknown metadata field.",
      "Unknown preset field.",
      "Unknown preset scope.",
      "Unsafe import filename.",
      "Unsafe path component.",
      "Unsafe preset filename or extension.",
      "Unsupported preset format.",
      "Unsupported preset schema version.",
  };
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
      if (matches != 1 || (!model_identifier && std::string_view(translated) == english)) {
        std::fprintf(stderr, "Translation mismatch: language=%s matches=%d key=%s\n", kLanguages[language].code, matches, english);
        return 5;
      }
      if (FormatConversions(english) != FormatConversions(translated)) {
        std::fprintf(stderr, "Format mismatch: language=%s key=%s translation=%s\n", kLanguages[language].code, english, translated);
        return 6;
      }
    }
    const auto check_preset_translation = [&](const char* english) {
      int matches = 0;
      for (const Translation& entry : kLanguages[language].table) {
        if (std::string_view(entry.en) == english) ++matches;
      }
      const char* translated = Tr(english);
      // Labels such as French Style can legitimately equal their English key.
      if ((language == 0 && (matches != 0 || std::string_view(translated) != english))
          || (language != 0 && (matches != 1 || *translated == '\0'))
          || FormatConversions(english) != FormatConversions(translated)) {
        std::fprintf(stderr, "Preset translation mismatch: language=%s matches=%d key=%s\n",
                     kLanguages[language].code, matches, english);
        return false;
      }
      return true;
    };
    for (const char* english : preset_strings) {
      if (!check_preset_translation(english)) return 7;
    }
    for (const auto& rule : renodx::addons::dlss5::presets::FieldRules()) {
      if (!check_preset_translation(rule.label)) return 7;
    }
    std::printf("%s: %s | %s\n", kLanguages[language].code, welcome.data(), crash.data());
  }
  std::printf("%s: %s\n", NR_NEXUS_ADDON_NAME, footer);
  return 0;
}
