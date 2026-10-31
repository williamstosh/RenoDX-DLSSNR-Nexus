/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#include <cstdio>
#include <cstring>
#include <string_view>

#include "brand.h"
#include "preset_fields.hpp"
#include "stage_plan.hpp"
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
      text::kPassStage, text::kPreviousStage, text::kPassStageTip,
      text::kRequestedStage, text::kEffectiveStage, text::kStageBlocked, text::kStagePlanNote,
      text::kStagesUnavailable,
      text::kResolutionInherit, text::kResolutionFixed, text::kPassResolutionTip,
      text::kStageResolutionNote, text::kStageResolutionBlocked,
      text::kResolutionBackend, text::kObservedResolution,
      renodx::addons::dlss5::stage_plan::ReasonText(renodx::addons::dlss5::stage_plan::Reason::NONE),
      renodx::addons::dlss5::stage_plan::ReasonText(renodx::addons::dlss5::stage_plan::Reason::BACKWARD_STAGE),
      renodx::addons::dlss5::stage_plan::ReasonText(renodx::addons::dlss5::stage_plan::Reason::UNSUPPORTED_BACKEND),
      renodx::addons::dlss5::stage_plan::ReasonText(renodx::addons::dlss5::stage_plan::Reason::MIXED_PRESENT),
      renodx::addons::dlss5::stage_plan::ReasonText(renodx::addons::dlss5::stage_plan::Reason::FG_OWNS_INPUT),
      renodx::addons::dlss5::stage_plan::ReasonText(renodx::addons::dlss5::stage_plan::Reason::RR_AFTER),
      renodx::addons::dlss5::stage_plan::ReasonText(renodx::addons::dlss5::stage_plan::Reason::PRESENT_AFTER),
      renodx::addons::dlss5::stage_plan::ReasonText(renodx::addons::dlss5::stage_plan::Reason::INVALID_STAGE),
      text::kInterfaceTheme, text::kRestoreTheme, text::kThemeTip, text::kEditing,
      text::kStyleFollows, text::kStyleIndependent, text::kResultFollows,
      text::kResultIndependent, text::kRuntimeUnconfirmed,
      "Precision cyan on deep blue.", "Classic teal and charcoal.",
      "Clear blue on bright surfaces.", "Pure black with restrained green.",
      text::kPresentGuideCountersTip,
      text::kPassCountTip,
      text::kModelStructure,
      text::kCadenceObservation,
      text::kOptionOff, text::kDetailAuto, text::kDetailAlways, text::kStabilizeStill, text::kStabilizeMotion, text::kUpsampleAuto, text::kUpsampleEdge, text::kUpsampleClassic, text::kUpsampleArea,
      text::kModelStrength, text::kResultStrength, text::kModelStrengthTip, text::kResultStrengthTip, text::kStructureTip, text::kStructureCapped, text::kGuideTip, text::kGuideOptional, text::kGuideRequired, text::kGuideNever, text::kHookUpscaledTip, text::kHookRenderTip, text::kHookPresentTip, text::kResFullTip, text::kResQualityTip, text::kResBalancedTip, text::kResPerformanceTip, text::kResFollowTip, text::kStyleDefaultTip, text::kStyleNaturalTip, text::kStyleCinematicTip, text::kFgEveryTip, text::kFgGameTip, text::kFgDetectAutoTip, text::kFgDetectStableTip, text::kFgDetectLegacyTip, text::kFgStatusUnknown, text::kFgDeclared, text::kFgBeforeUnknown, text::kFgBeforeShort, text::kFgEveryShort, text::kFgGameShort, text::kFgFallbackShort, text::kFgUpscaledShort, text::kFgMixedShort, text::kStackCost, text::kFgUnknownMultiplierTip,
      text::kExpandAll, text::kCollapseAll, text::kChanged, text::kNrResolution, text::kResolutionFine, text::kProcessingLocation, text::kFgAdvanced, text::kFgFramePolicy, text::kFgFramePolicyTip, text::kResultBasic, text::kResultLimits, text::kResultDetail, text::kResultTemporal, text::kTemporalIndependent, text::kModelAutoTip, text::kModelATip, text::kModelBTip, text::kModelInheritTip, text::kChainMarkTip,
      text::kFrameGenUnconfirmed, text::kFrameGenFallback, text::kFgNoMultiplier, text::kFgUnknown, text::kFgOtherStage, text::kFgBefore, text::kFgEvery, text::kFgGame, text::kFgFallback, text::kFgUpscaled, text::kFgMixed, text::kFgDiagnostics,
      text::kStatusDetails, "Advanced", text::kPassOverview,
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
      "Monitor",
      "Position",
      text::kHotkeys,
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
  // Includes dynamic profile status, notices and fixed slot labels.
  const char* profile_strings[] = {
      "Private game profile identity",
      "Profiles",
      "This profile requires a newer template version.",
      " (empty)",
      "Assign NR fields to custom slot",
      "Browse folder",
      "Choose export file",
      "Choose preset file",
      "Clear only this official profile's overrides? This action can be undone.",
      "Clear slot",
      "Confirm replacing or clearing this slot",
      "Copy official profile to slot",
      "Could not create a game profile identity.",
      "Could not identify this game. Profiles were not loaded.",
      "Could not read game profiles. Current settings were kept.",
      "Could not save the game profile identity. Changes remain unsaved.",
      "Could not start profile storage. Changes remain unsaved.",
      "Could not start profile storage. Current settings were kept.",
      "Custom 1",
      "Custom 2",
      "Custom 3",
      "Custom slot",
      "Custom slot saved. Select it to apply its settings.",
      "Custom slots",
      "Development candidate",
      "Empty slot",
      "Empty slot. Save current settings or copy an official profile to create it.",
      "Export destination selected. Confirm export to write the file.",
      "Exporting current game settings.",
      "Exporting the saved custom slot snapshot.",
      "Game location changed. Profiles were copied; current settings were kept.",
      "Game settings changed elsewhere. The last selected profile was not reapplied.",
      "Import preset",
      "Loading profiles...",
      "Modified",
      "My presets",
      "My profiles",
      "NR Balanced",
      "NR Lite",
      "NR Max",
      "NR Profiles",
      "Official defaults",
      "Official profiles",
      "Open preset folder",
      "Preset file saved.",
      "Preset folder opened.",
      "Preset library",
      "Preset operation cancelled.",
      "Profile action undone. Later edits were kept.",
      "Profile applied. Runtime execution is pending confirmation.",
      "Profile contains unsupported settings. Current settings were kept.",
      "Profile storage unavailable",
      "Profiles changed after this action. Undo was not applied.",
      "Rename slot",
      "Restore official defaults",
      "Retry saving profiles",
      "Save current game settings to slot",
      "Save or share this slot",
      "Saved",
      "Saving...",
      "Slot cleared. Running game settings were kept.",
      "Slot display name",
      "The selected path is too long for this input field.",
      "Undo profile action",
      "Unsaved profile changes",
      "Use current game settings",
  };
  // Includes ternary labels, dynamic SaveStatus/Summary notices and popup copy.
  const char* profile_ux_strings[] = {
      "Restore Quality defaults?",
      "Restore Multi-Pass defaults?",
      "Restore Advanced defaults?",
      "Restore all defaults?",
      "Restore this game's Quality settings, including shared Pass 1 controls. Independent Pass 2-4 settings and other pages are kept.",
      "Restore pass count and independent Pass 2-4 settings. Shared Pass 1 model, stage, resolution, steering and result settings are kept.",
      "Restore compatibility, HDR/depth/motion, hotkeys, screenshots and interface preferences. Quality, independent passes and diagnostic controls are kept.",
      "Restore this game's NR settings, compatibility, hotkeys, interface and diagnostics. Clear personal changes in the three official profiles; keep all custom profiles, library files and other games. Attach-time support policies and unknown keys are kept.",
      "All six saved profiles and library files are kept. The current profile association will be detached before resetting.",
      "Confirm restore",
      "Restore page defaults",
      "Reset page",
      "Saving profiles and restoring defaults...",
      "Recovering previous profiles...",
      "Another profile action is pending. Reset was not started.",
      "Reset unavailable: profile storage or game configuration is read-only.",
      "Reset pending: saving previous profile changes.",
      "Game configuration changed during reset. Reset was cancelled.",
      "Reset failed: previous profile changes could not be saved.",
      "Reset failed.",
      "Profile rollback failed. Detached profiles and recovery snapshot were kept.",
      "Reset cancelled or failed. Previous profiles restored.",
      "Reset failed: profile storage was not changed.",
      "Reset failed: applied settings did not match the saved reset snapshot.",
      "Defaults restored; custom profiles were kept.",
      "Retry profile recovery",
      "Manage profiles",
      "Profile library",
      "Save current profile",
      "Import profile",
      "Back to profile library",
      "More options",
      "Enter file path",
      "Export file",
      "Paste share text",
      "Official defaults restored.",
      "Save current settings to this slot",
      "Copy from official profile",
      "This slot is no longer empty. Review it in Manage profiles.",
      "Profile actions",
      "Saved settings do not confirm NR execution. See Diagnostics for runtime status.",
      "This official profile has no personal changes.",
      "Restore only this official profile. Other profiles are kept.",
      "Restore official defaults?",
      "Personal changes to this official profile will be cleared. Other profiles are kept.",
      "Cancel",
      "Restore",
  };
  const char* runtime_strings[] = {
      "Checking NR",
      "Needs attention",
      "Checking Neural Rendering",
      "Waiting for a fresh GPU result for the current settings.",
      "Neural Rendering needs attention",
      "Check the warnings below while NR checks the current settings.",
      "Settings changed. Cancel and choose the stage again.",
      "Undo stage change",
      "Confirm execution chain change",
      "Review the complete chain. Only the stages shown below will change.",
      "Before",
      "After",
      "Inherited",
      "Confirm change",
      "Execution chain updated.",
      "Stage change undone. Later edits were kept.",
      "NR enabled",
      "NR disabled",
      "Checking runtime status",
      "NR running normally",
      "NR submitted before FG",
        "Locks movement and resizing.",
        "Detailed monitor",
        "Open the menu to edit the monitor.",
        "Show / hide runtime monitor",
        "Keep monitor visible after closing the menu",
        "Default F11 toggles the monitor. Open the menu to move, resize or edit it.",
        "Monitor key conflicts with another shortcut. Choose a different key.",
      text::kEditingPass,
      text::kResolutionFollow,
      text::kResolutionSavedScale,
      text::kResolutionSharedUpsample,
      text::kResolutionReferenceRender,
      text::kResolutionReferenceUpscaled,
      text::kResolutionReferencePresent,
      text::kResolutionGuideUnknown,
      text::kResolutionPresentGuides,
      text::kResolutionCurrentPending,
      text::kResolutionUnavailable,
      text::kResolutionEffective,
      text::kResolutionEffectiveUnknown,
      text::kResolutionCompletedSample,
      text::kResolutionTip,
      "How Neural Rendering's change reaches the screen when it works below the output resolution, including Follow input (no effect when it works at the output resolution).  Auto and Edge-aware keep the change on the edges it belongs to and the full-resolution detail under it, and sample the nearest SDR pixel for NR.  Edge-aware (area input) averages SDR pixels before NR; this can change NR's look.  Classic shows NR's own lower-resolution pixels, which look blocky.",
      "Export profile",
      "Open Diagnostics",
      "Details",
      "Resolution unavailable",
      "NR GPU %.1f ms",
      "NR cost unavailable",
      "Model names are resolved creation requests. The runtime's internal network cannot be read back.",
      "NR is off. Previous samples do not confirm current execution.",
      "GPU-complete group: %.3f ms | %u samples",
      "Pass %u | %s | %ux%u -> %ux%u | model request %s",
      "NGX %.3f ms | Encode %.3f ms | Resolve %.3f ms",
      "Pass %u: %s",
      "Execution stage pending confirmation",
      "Runtime details",
      "Fallback and compatibility",
      "Not measured",
      "Awaiting closed recording and GPU completion",
      "GPU complete: single submission",
      "Not executed",
      "Skipped: zero strength",
      "Discarded: recording not submitted",
      "Rejected: command list replayed",
      "Expired: no recent proven sample",
      "Unavailable: device removed",
      "Unavailable: exact submission or queue frequency missing",
      "Rejected: incomplete or invalid timestamps",
      "Render",
      "Upscaled",
      "Present",
      "Left-click to apply; right-click to manage.",
      "Left-click to create; right-click to manage.",
      "Change only the slot display name.",
      "Create this slot first.",
      "Export the actual saved slot settings, including personal changes.",
      "Running game settings and library files are kept.",
      "This slot is already empty.",
      "Slot renamed.",
      "Enter a nonempty slot name within the length limit.",
      "Copies the original official template, without personal changes.",
      "Personal changes have been automatically saved.",
      "Undo",
  };
  const char* stage_c_strings[] = {
      "Lock",
      "Unlock",
      "Compact",
      "Details",
      "Diagnostics",
      "Close",
      "NR GPU",
      "ReShade FPS",
      "FG",
      "Stage",
      "Execution confirmed",
      "Stage adjusted",
      "Requested stage differs from effective stage.",
      "Configured model",
      "Model request adjusted",
      "Working ratio",
      "Pass evidence",
      "Raw evidence",
      "Working resolution",
      "ReShade overlay cadence. This is not a separate measurement of game render FPS or generated output FPS.",
      "Observed frame generation route. FG GPU duration is unavailable.",
      "Estimated working resolution: %ux%u",
      "Estimated working resolution",
      "Actual working resolution",
      "Estimate uses the last observed reference. Actual dimensions require a current completed sample.",
      "Working resolution history",
      "On",
      "Off",
      "Unknown",
      "Official template: 1 Render pass, model B, scale 0.67, Default style.",
      "Official template: 1 Upscaled pass, model A, scale 0.67, Default style.",
      "Official template: Render B (Cinematic), then Upscaled A (Default); both at scale 0.67.",
      "Configured working ratio and current completed working dimensions when available.",
  };
  const char* monitor_v2_strings[] = {
      "Restore monitor visibility default",
      "Interface & notifications",
      "Game fixes & compatibility",
      "Development & maintenance",
      "Screen notifications",
      "Custom position: your saved notification placement is unchanged. Choose a grid position to adjust offsets.",
      "Reset notification position",
      "Preview notification position",
      "Notification preview",
      "ImGui fallback",
      "Frame rate unavailable",
      "Present callback cadence for this runtime and surface. Does not measure final frame-generated output FPS.",
      "Fallback: ImGui DeltaTime update cadence for this runtime. Does not measure final frame-generated output FPS.",
      "Frame rate unavailable: waiting for fresh samples for this runtime.",
      "NR: %.1f ms GPU per completed stage group.",
      "Measured GPU time for the current completed stage group. Compare configurations using GPU A/B; overall FPS also depends on the game and frame limits.",
      "FPS",
      "Recent valid NR GPU time. Shows — when no measurement is available.",
      "Fit to content",
      "Fit height to current content.",
      "Trends",
      "Show performance history",
      "Frame interval (ms)",
      "Pass 1 GPU",
      "Pass 2 GPU",
      "Pass 3 GPU",
      "Pass 4 GPU",
      "10 s",
      "30 s",
      "60 s",
      "FPS averages the latest 16 intervals; frame interval shows raw samples. GPU curves preserve completed sample peaks and leave gaps empty.",
      "Reference sum of recent stage GPU times, including each stage's shared work once. Not same-frame or FG end-to-end latency.",
      "Stage measurements use different queues or conditions. See per-pass GPU times.",
      "Some stages have no current GPU measurement. See per-pass GPU times.",
      "GPU timing unavailable",
      "Background opacity",
      "Only monitor backgrounds fade. Text and graphs keep their opacity.",
      "Quick position",
      "Drag the monitor with the menu open to keep a custom position.",
      "Custom position",
      "Top left",
      "Top center",
      "Top right",
      "Center left",
      "Center",
      "Center right",
      "Bottom left",
      "Bottom center",
      "Bottom right",
      "Position offset in UI pixels.",
      "Horizontal offset",
      "Vertical offset",
      "Reset monitor position and size",
  };
  if (FormatConversions("Estimated working resolution: %ux%u") != "%u|%u|") return 7;
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
      if (matches != 1 || *translated == '\0' || (!model_identifier && std::string_view(translated) == english)) {
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
    for (const char* english : profile_strings) {
      if (!check_preset_translation(english)) return 7;
      if (language != 0 && std::string_view(Tr(english)) == english) {
        std::fprintf(stderr, "Untranslated profile text: language=%s key=%s\n", kLanguages[language].code, english);
        return 7;
      }
    }
    for (const char* english : profile_ux_strings) {
      if (!check_preset_translation(english)) return 7;
      if (language != 0 && std::string_view(Tr(english)) == english) {
        std::fprintf(stderr, "Untranslated profile UX text: language=%s key=%s\n", kLanguages[language].code, english);
        return 7;
      }
    }
    for (const char* english : runtime_strings) {
      if (!check_preset_translation(english)) return 7;
      const bool technical_stage = std::string_view(english) == "Render"
                                   || std::string_view(english) == "Upscaled"
                                   || std::string_view(english) == "Present";
      if (language != 0 && !technical_stage && std::string_view(Tr(english)) == english) {
        std::fprintf(stderr, "Untranslated runtime text: language=%s key=%s\n", kLanguages[language].code, english);
        return 7;
      }
    }
    for (const char* english : stage_c_strings) {
      if (!check_preset_translation(english)) return 7;
      const bool technical_label = std::string_view(english) == "NR GPU";
      const bool shared_word = std::string_view(kLanguages[language].code) == "fr"
                               && std::string_view(english) == "Compact";
      if (language != 0 && !technical_label && !shared_word && std::string_view(Tr(english)) == english) {
        std::fprintf(stderr, "Untranslated stage C text: language=%s key=%s\n", kLanguages[language].code, english);
        return 7;
      }
    }
    for (const char* english : monitor_v2_strings) {
      if (!check_preset_translation(english)) return 7;
      const std::string_view key = english;
      const bool technical_label = key == "FPS" || key == "10 s" || key == "30 s" || key == "60 s"
                                   || key == "Pass 1 GPU" || key == "Pass 2 GPU"
                                   || key == "Pass 3 GPU" || key == "Pass 4 GPU";
      if (language != 0 && !technical_label && std::string_view(Tr(english)) == english) {
        std::fprintf(stderr, "Untranslated monitor 2.0 text: language=%s key=%s\n", kLanguages[language].code, english);
        return 7;
      }
    }
    const std::string_view code = kLanguages[language].code;
    if (code == "zh-Hans" || code == "zh-Hant") {
      const bool simplified = code == "zh-Hans";
      const struct {
        const char* english;
        const char* simplified;
        const char* traditional;
      } terms[] = {
          {"Render", "渲染", "渲染"},
          {"Upscaled", "超分后", "放大後"},
          {"Present", "呈现", "呈現"},
          {"FPS", "帧率", "幀率"},
          {"Working resolution", "工作分辨率", "工作分辨率"},
          {"Estimated working resolution", "预计工作分辨率", "預計工作分辨率"},
          {"Actual working resolution", "实际工作分辨率", "實際工作分辨率"},
          {"Working resolution history", "工作分辨率历史", "工作分辨率歷史"},
          {"Requested working size", "请求工作分辨率", "要求工作分辨率"},
          {"Observed working size", "观测工作分辨率", "觀測工作分辨率"},
          {"Recorded working sizes", "已记录工作分辨率", "已記錄工作分辨率"},
          {"Last recorded model work: %ux%u", "最近记录的模型工作分辨率：%ux%u", "最近記錄的模型工作分辨率：%ux%u"},
          {"Quality", "画质", "畫質"},
          {"Multi-Pass", "叠层", "疊層"},
          {"Advanced", "高级", "進階"},
          {"Manage profiles", "管理方案", "管理方案"},
          {"Profile library", "方案库", "方案庫"},
          {"Preset library", "方案库", "方案庫"},
          {"Save current profile", "保存方案", "儲存方案"},
          {"Import profile", "导入方案", "匯入方案"},
          {"My profiles", "我的方案", "我的方案"},
          {"My presets", "我的方案", "我的方案"},
          {"Custom slots", "我的方案", "我的方案"},
          {"Preset", "方案", "方案"},
          {"NR Lite", "轻量", "輕量"},
          {"NR Balanced", "均衡", "均衡"},
          {"NR Max", "极致", "極致"},
      };
      for (const auto& term : terms) {
        if (std::string_view(Tr(term.english)) != (simplified ? term.simplified : term.traditional)) {
          std::fprintf(stderr, "Profile terminology mismatch: language=%s key=%s\n", kLanguages[language].code, term.english);
          return 7;
        }
      }
    }
    for (const auto& rule : renodx::addons::dlss5::presets::FieldRules()) {
      if (!check_preset_translation(rule.label)) return 7;
    }
    std::printf("%s: %s | %s\n", kLanguages[language].code, welcome.data(), crash.data());
  }
  std::printf("%s: %s\n", NR_NEXUS_ADDON_NAME, footer);
  return 0;
}
