/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// The panel's display strings (PLAN_UI_V7.md 5, U2).
//
// Overlay text is free to change; LOG text is a contract and never lives
// here.  The two layouts share every section function, and a section reads
// the labels that differ between them from the table of the layout that
// called it: kClassicText is the alpha44 wording verbatim (Developer view
// promises today's layout), kModernText applies the rename table.  A label
// only one section draws is spelled at its control as
// text.Pick(classic, modern), so the two wordings sit side by side.  Strings
// only the modern layout draws follow the tables.  U5 moves the rest of the
// panel's text in here.
//
// Every string here is English and is translated where it is drawn
// (i18n.hpp): the modern layout's text goes through Tr, the Developer view's
// through PanelText::Tr, which leaves it English.  Both layouts draw the
// status card, the Streamline warning and the Developer view toggle
// translated: they are how a player finds their way in either layout.

#pragma once

#include "../brand.h"
#include "i18n.hpp"

namespace renodx::addons::dlss5::ui {

struct PanelText {
  // Which layout the table serves.  The modern one also draws label-left
  // rows with a reset button and a switch where the classic one draws
  // ImGui's label-right controls and a checkbox.
  bool modern;
  const char* nr_enable;
  const char* intensity;
  const char* hook_point;
  const char* stack_passes;
  const char* codec;
  const char* diffuse_white;
  const char* governor_attack;
  const char* governor_release;
  const char* guide_overrides_note;  // nullptr: the section header says it

  constexpr const char* Pick(const char* classic, const char* modern_label) const {
    return modern ? modern_label : classic;
  }

  // `english` as this layout draws it: translated in the modern layout,
  // English in the Developer view.
  const char* Tr(const char* english) const {
    return modern ? ::renodx::addons::dlss5::ui::Tr(english) : english;
  }
};

inline constexpr PanelText kClassicText = {
    false,
    "Enable DLSS Neural Rendering",
    "Overall Intensity",
    "NR Hook Point",
    "Pass Count",
    "NR Codec",
    "Diffuse White (nits)",
    "Governor attack (stops/s)",
    "Governor release (stops/s)",
    "Guide overrides (leave at defaults unless diagnostics require them)",
};

// i18n: begin - display strings (ui/lang tables)
inline constexpr PanelText kModernText = {
    true,
    "Neural Rendering",
    "Model strength",
    "Hook point",
    "Pass Count",
    "HDR mode",
    "Paper white (nits)",
    "Brighten speed",
    "Darken speed",
    nullptr,
};

namespace text {

inline constexpr const char* kModelStructure = "Model structure";
inline constexpr const char* kModelStrength = "Model strength";
inline constexpr const char* kResultStrength = "Result adjustment strength";
inline constexpr const char* kModelStrengthTip = "Controls the NR model before result adjustment. Values 0-2 are sent unchanged; 1 is the default. Response above 1 depends on the model/runtime and is unverified. Changes reset NR history.";
inline constexpr const char* kResultStrengthTip = "Scales the NR edit after the model: 0 removes the edit, 1 keeps its scale, 2 doubles its scale before limits. Requires result adjustment. Changes apply live without restarting NR history.";
inline constexpr const char* kStructureTip = "Controls structure synthesized by the NR model; Detail reshapes the output afterward. With Character mask and Independent skin enabled, the value sent to the model is capped at 1. Changes reset NR history.";
inline constexpr const char* kStructureCapped = "Character mask and Independent skin cap the sent structure at 1.00; the stored value is preserved.";
inline constexpr const char* kGuideTip = "Selects a resource policy for Present NR, not proof that depth and motion were captured. Other frame, resource and compatibility checks still apply.";
inline constexpr const char* kGuideOptional = "Prefer matching game depth and motion; use neutral data when unavailable. Recommended for most games, including scenes without usable DLSS resources. Other checks may still skip NR.";
inline constexpr const char* kGuideRequired = "Run Present NR only with matching depth and motion; otherwise skip this frame. Avoids neutral guides. This does not directly detect whether DLSS is enabled.";
inline constexpr const char* kGuideNever = "Always use neutral depth and motion, ignoring captured game guides. Useful for unreliable or incompatible resources, including games with DLSS. Motion stability may be affected.";
inline constexpr const char* kHookUpscaledTip = "Process the game output after upscaling. NR uses the existing Upscaled path and its resource contract.";
inline constexpr const char* kHookRenderTip = "Process before upscaling, using game depth and motion at render resolution. Ray Reconstruction keeps the compatible Upscaled path.";
inline constexpr const char* kHookPresentTip = "Process the presented image, including its HUD. Present depth/motion and FG policies apply; the observed route may differ from the selected position.";
inline constexpr const char* kResFullTip = "NR works at 100 percent of the reference resolution. Does not select DLAA or change game DLSS settings.";
inline constexpr const char* kResQualityTip = "NR works at 67 percent of the reference resolution. Changes NR working resolution only, not the game DLSS mode.";
inline constexpr const char* kResBalancedTip = "NR works at 58 percent of the reference resolution. Changes NR working resolution only.";
inline constexpr const char* kResPerformanceTip = "NR works at 50 percent of the reference resolution. Changes NR working resolution only.";
inline constexpr const char* kResFollowTip = "Follow the game input/render resolution instead of the saved scale. Does not change game resolution or its DLSS mode.";
inline constexpr const char* kStyleDefaultTip = "Let the NR runtime choose its default style.";
inline constexpr const char* kStyleNaturalTip = "Request the NR model Natural style. Changes restart NR history.";
inline constexpr const char* kStyleCinematicTip = "Request the NR model Cinematic style. Changes restart NR history.";
inline constexpr const char* kFgEveryTip = "On the ordinary Present path, run NR on every presented frame, including generated frames. Successful Pre-FG takes precedence. This does not enable FG.";
inline constexpr const char* kFgGameTip = "On the ordinary Present path, process only identified game frames; generated frames may look different. Successful Pre-FG takes precedence. This does not disable FG.";
inline constexpr const char* kFgDetectAutoTip = "Use a matching declared multiplier when available; otherwise use stable cadence detection on the ordinary Present path.";
inline constexpr const char* kFgDetectStableTip = "Use stable cadence detection on the ordinary Present path; ignore multiplier declarations.";
inline constexpr const char* kFgDetectLegacyTip = "Use the earlier Present cadence rule for compatibility.";
inline constexpr const char* kFgStatusUnknown = "FG activity unconfirmed";
inline constexpr const char* kFgDeclared = "Game declares FG enabled; activity unconfirmed";
inline constexpr const char* kFgBeforeUnknown = "Pre-FG NR observed; multiplier unknown";
inline constexpr const char* kFgBeforeShort = "Pre-FG NR confirmed";
inline constexpr const char* kFgEveryShort = "NR on every presented frame";
inline constexpr const char* kFgGameShort = "NR on game frames only";
inline constexpr const char* kFgFallbackShort = "Pre-FG fallback observed";
inline constexpr const char* kFgUpscaledShort = "NR fallback to Upscaled";
inline constexpr const char* kFgMixedShort = "Mixed Pre-FG and fallback";
inline constexpr const char* kStackCost = "Additional passes increase NR GPU work.";
inline constexpr const char* kFgUnknownMultiplierTip = "FG activity and multiplier are separate observations. Native FG/XeFG can bypass Present cadence sampling; an unknown multiplier does not mean FG is off.";

inline constexpr const char* kHookTips[] = {kHookUpscaledTip, kHookRenderTip, kHookPresentTip};
inline constexpr const char* kGuideTips[] = {kGuideOptional, kGuideRequired, kGuideNever};
inline constexpr const char* kResolutionTips[] = {kResFullTip, kResQualityTip, kResBalancedTip, kResPerformanceTip, kResFollowTip};
inline constexpr const char* kFgPolicyTips[] = {kFgEveryTip, kFgGameTip};
inline constexpr const char* kFgDetectionTips[] = {kFgDetectAutoTip, kFgDetectStableTip, kFgDetectLegacyTip};

inline constexpr const char* kOptionOff = "Disable this processing; keep saved parameter values.";
inline constexpr const char* kDetailAuto = "Apply detail stability on the compatible post-upscale path, even with result adjustment off.";
inline constexpr const char* kDetailAlways = "Request detail stability before or after upscaling. Resource and compatibility checks still apply.";
inline constexpr const char* kStabilizeStill = "Average the NR edit over time with less following of motion. Requires result adjustment.";
inline constexpr const char* kStabilizeMotion = "Average the NR edit while following game motion vectors. Requires result adjustment and usable motion.";
inline constexpr const char* kUpsampleAuto = "Choose the existing automatic edge-aware edit reconstruction. Independent of result adjustment.";
inline constexpr const char* kUpsampleEdge = "Reconstruct the NR edit using edges and full-resolution detail.";
inline constexpr const char* kUpsampleClassic = "Use the NR lower-resolution pixels directly; they may appear blocky.";
inline constexpr const char* kUpsampleArea = "Average input pixels before NR, then reconstruct the edit using edges. Can change the model output and resets NR history.";
inline constexpr const char* kDetailStabilityTips[] = {kDetailAuto, kOptionOff, kDetailAlways};
inline constexpr const char* kStabilizeTips[] = {kOptionOff, kStabilizeStill, kStabilizeMotion};
inline constexpr const char* kUpsampleTips[] = {kUpsampleAuto, kUpsampleEdge, kUpsampleClassic, kUpsampleArea};
inline constexpr const char* kCadenceObservation = "Present cadence: %.2f (observation only; not an FG verdict)";
inline constexpr const char* kPassCountTip = "Runs %u NR passes in total, including Pass 1.";
inline constexpr const char* kSettingsViewTips[] = {"Temporarily preview the built-in NR effect. Your saved settings are kept.", "Return to your saved NR settings for editing."};
inline constexpr const char* kPresentGuideCountersTip = "These are legacy Present guide counters. Native FG/XeFG Pre-FG guide use is not included; do not treat them as whole-chain resource availability.";
inline constexpr const char* kStatusDetails = "Status & warnings (%u)";
inline constexpr const char* kMenuPages[] = {"Quality", "Multi-Pass", "Diagnostics", "Advanced"};
inline constexpr const char* kPassOverview = "Pass Overview";
inline constexpr const char* kOpenPresentSettings = "Open Present settings";
inline constexpr const char* kAbout = "Version & about";
inline constexpr const char* kRuntimeStatus = "Runtime status";
inline constexpr const char* kResources = "Resources & compatibility";
inline constexpr const char* kDetails = "Detailed diagnostics";
inline constexpr const char* kResultShaping =
    "NR result adjustment";
inline constexpr const char* kResultChanged =
    "NR result adjustment (changed)";
inline constexpr const char* kPresentSettings = "Present settings";
inline constexpr const char* kPresentDisplay = "HDR & compatibility";
inline constexpr const char* kGeneral = "Language & notifications";

inline constexpr const char* kChainNote = "Configured chain; execution is reported in Diagnostics.";
inline constexpr const char* kModelNote =
    "This build does not read the runtime's actual network. Resolved models describe configuration only.";
inline constexpr const char* kResourceNote = "Observed this session; the selected resource policy is not proof of availability.";
inline constexpr const char* kSharedResult =
    "Result settings apply to Pass 1 and every pass inheriting them. Customize other passes in Multi-Pass.";
inline constexpr const char* kInheritResult =
    "Inherit Pass 1 result settings";
inline constexpr const char* kInheritResultTip =
    "Result shaping and stability only. Model choice and model styling have separate inheritance. Stored custom values are preserved.";
inline constexpr const char* kInheritedResult =
    "Uses Pass 1 result settings. Turn inheritance off to edit this pass.";
inline constexpr const char* kFgNoMultiplier =
    "No active frame generation multiplier is confirmed. Additional passes still increase NR GPU work.";
inline constexpr const char* kFgUnknown =
    "Recent NR execution path is unconfirmed. Open Diagnostics for observed counters; configuration alone does not confirm Pre-FG.";
inline constexpr const char* kFgOtherStage =
    "NR runs at Render or Upscaled. Extra passes increase NR GPU cost; this is not per-present processing.";
inline constexpr const char* kFgBefore =
    "Pre-FG processing observed. Extra passes still increase NR GPU cost, but generated frames normally do not require a full NR chain each. HUD handling may add NR work.";
inline constexpr const char* kFgEvery =
    "Present processing observed with the every-present policy, including generated frames. Extra passes can greatly increase GPU cost. Prefer a compatible Pre-FG path.";
inline constexpr const char* kFgGame =
    "Present processing observed with the game-frames-only policy. Generated frames may not receive the same NR effect, so appearance can vary between frames.";
inline constexpr const char* kFgFallback =
    "Pre-FG fallback and Present processing observed. The Present frame policy applies; review the fallback reason in Diagnostics.";
inline constexpr const char* kFgUpscaled =
    "Fallback to Upscaled observed. NR is running through the upscaler path instead of ordinary Present. Extra passes still increase GPU cost.";
inline constexpr const char* kFgMixed =
    "Recent Pre-FG successes and fallbacks were both observed. The route is mixed; review Diagnostics before judging NR cost or frame consistency.";
inline constexpr const char* kFgDiagnostics =
    "Inspect processing path";
inline constexpr const char* kExpandAll =
    "Expand all";
inline constexpr const char* kCollapseAll =
    "Collapse all";
inline constexpr const char* kChanged =
    "changed";
inline constexpr const char* kNrResolution =
    "NR working resolution";
inline constexpr const char* kResolutionFine =
    "Resolution fine tuning";
inline constexpr const char* kProcessingLocation =
    "Processing location (Hook Point)";
inline constexpr const char* kFgAdvanced =
    "FG advanced settings";
inline constexpr const char* kFgFramePolicy =
    "FG frame processing policy";
inline constexpr const char* kFgFramePolicyTip =
    "Controls NRPresentFrames only, not whether FG is enabled. It selects every-present or game-frame-only processing when ordinary Present is used. Successful Pre-FG processing takes precedence; fallback uses the applicable path.";
inline constexpr const char* kResultBasic =
    "Basic result adjustments";
inline constexpr const char* kResultLimits =
    "Brightness and colour limits";
inline constexpr const char* kResultDetail =
    "Detail and halos";
inline constexpr const char* kResultTemporal =
    "Temporal stability";
inline constexpr const char* kTemporalIndependent =
    "Detail stability can work while result adjustment is off. The Stabilize controls below require result adjustment to be on.";
inline constexpr const char* kModelAutoTip =
    "Automatically follows runtime compatibility rules. The identified DLSS NR 310.9.16 selects B; other versions keep their compatible default.";
inline constexpr const char* kModelATip =
    "Legacy NR model. GPU cost is often higher. Detail, structure and colour can differ from B; compare the actual scene rather than assuming higher quality.";
inline constexpr const char* kModelBTip =
    "Small FP8 model, often with lower GPU cost. Its appearance can differ from A; neither model is best in every scene. Requires the identified DLSS NR 310.9.16 runtime.";
inline constexpr const char* kModelInheritTip =
    "Inherit the main model, including its automatic resolution or fallback. This is separate from model-styling and result-setting inheritance.";
inline constexpr const char* kChainMarkTip =
    "* means stored settings differ from defaults. These nodes show requested models and inheritance, not runtime-confirmed networks or per-pass GPU timing.";
inline constexpr const char* kRequest = "Requested model";
inline constexpr const char* kResolved = "Resolved model";
inline constexpr const char* kNetwork = "Runtime network";
inline constexpr const char* kNetworkUnavailable = "Readback unavailable";
inline constexpr const char* kIndependentModel = "Independent Model";
inline constexpr const char* kInheritModel = "Inherit Main Model";
inline constexpr const char* kInput = "Input";
inline constexpr const char* kOutput = "Output";
inline constexpr const char* kConfigured = "Configured";

inline constexpr const char* kReportSteps = "Reproduction steps (optional)";
inline constexpr const char* kReportCapture = "Include an optional screenshot comparison";
inline constexpr const char* kReportCollect = "Generate local diagnostics";
inline constexpr const char* kReportWithoutDiagnostics = "Continue without diagnostics";
inline constexpr const char* kReportFeature = "Feature request";
inline constexpr const char* kReportPrivacy = "Review every attachment. Screenshots may contain personal information; they are excluded by default. Redaction may not detect every secret.";
inline constexpr const char* kReportPreviewLimit = "Text preview shows the first 64 KiB. Open the staged file to review the full text or image.";
inline constexpr const char* kReportOpenPreview = "Open staged file";
inline constexpr const char* kReportPreview = "Preview text";
inline constexpr const char* kReportUploadConsent = "I authorize uploading the selected files to the temporary hosts shown above.";
inline constexpr const char* kReportPublicSummaryConsent = "Include the reviewed diagnostic summary in the GitHub draft";
inline constexpr const char* kReportPublicLinkConsent = "Include the temporary report link in the public GitHub draft";
inline constexpr const char* kReportGitHub = "Prepare GitHub Issue";
inline constexpr const char* kReportPlainIssue = "Copy Markdown and open a blank Issue";
inline constexpr const char* kReportPublicNotice = "GitHub Issues are public. Review the draft and submit it yourself in the browser. This does not upload files.";
inline constexpr const char* kReportBrowserFailed = "The browser could not be opened. Copy the Markdown below and open the repository Issues page manually.";
inline constexpr const char* kReportClipboardFailed = "The clipboard was unavailable. Copy the Markdown below manually.";
inline constexpr const char* kReportManualFallback = "The full Markdown is available below. Paste it into a blank Issue if prefill fails or GitHub is offline.";
inline constexpr const char* kReportDraftReady = "The browser request was sent. No Issue has been submitted.";
inline constexpr const char* kReportLinkReady = "Temporary report link ready";
inline constexpr const char* kReportLocalSaved = "The ZIP stays on this PC until you explicitly upload or share it.";
inline constexpr const char* kReportCleaning = "Cleaning temporary report files...";
inline constexpr const char* kReportFailed = "Report operation failed: %s";
inline constexpr const char* kReportNativeFg = "Native FG/MFG issue";
inline constexpr const char* kReportColour = "HDR/SDR or wrong colours";
inline constexpr const char* kReportVisual = "Flicker, ghosting or distortion";
inline constexpr const char* kReportXefg = "XeFG issue";
inline constexpr const char* kReportCompatibility = "Game compatibility";

inline constexpr const char* kNrEnableTip =
    "Turns Neural Rendering on and off.  The key shown next to the switch"
    " does the same in game.";
// v8.0.3: the tooltips say what changes on screen, not "the neural pass".
inline constexpr const char* kIntensityTip =
    "How strongly Neural Rendering reworks the image: 0 leaves the game's"
    " image untouched, 1 is the default, 2 the strongest.  A change restarts"
    " NR's history for a moment.";
// Shared by pass 1's HDR rows and every stacked pass's.
inline constexpr const char* kTransferTip =
    "Float-HDR games: how much of the dark lift the model adds near black is"
    " removed (Dark pedestal removal).  Other HDR games: how much of Neural"
    " Rendering's change reaches the image, from 0 (none) to 1 (all).";
inline constexpr const char* kTransferInertTip =
    "Nothing to scale in this game: its HDR brightness is relative, so Dark"
    " pedestal removal (Auto) leaves the model's dark lift alone.  Set Dark"
    " pedestal removal to Always to use this slider.";
inline constexpr const char* kTransferSdrTip =
    "Nothing to scale here: the frame is SDR, so Neural Rendering's change"
    " reaches it in full.";
inline constexpr const char* kColorTip =
    "On float-HDR games, how much of Neural Rendering's change reaches the"
    " image: 0 keeps the game's image, 1.00 shows all of it.";
inline constexpr const char* kPaperWhiteTip =
    "How bright white surfaces glow, in nits (203 is the HDR reference).  PQ"
    " HDR games only; other HDR games use Scene white scale.";
inline constexpr const char* kSceneWhiteTip =
    "The same for float-HDR and classic sources (2.5375 is 203-nit paper"
    " white).  PQ HDR games use Paper white and PQ calibration instead.";
inline constexpr const char* kRebindTip =
    "Click, then press the new key; Esc cancels.";
inline constexpr const char* kNoneYet = "none yet";
// i18n: end
// Both layouts' status line: version, the loaded runtime's file version (through
// v8.0.2 a fixed "v310.8.0", whatever build was loaded), the game's file name.
inline constexpr const char* kFooter = NR_NEXUS_NAME " %s | DLSSNR %s | %s";
// i18n: begin - display strings (ui/lang tables)

// The Defaults view (alpha46): segment 0 runs the built-in look, segment 1
// the player's own.
inline constexpr const char* kSettingsView = "Settings";
inline constexpr const char* kSettingsViewLabels[] = {"Built-in effect", "Mine"};
inline constexpr const char* kSettingsViewTip =
    "Compare in game: Built-in effect temporarily uses stock Strength, Look and Multi-Pass settings. Mine restores your values. Nothing is saved while Built-in effect is selected.";
inline constexpr const char* kDefaultsViewNote =
    "Showing the built-in look.  Your settings are kept; pick Mine to return.";

inline constexpr const char* kResolution = "Resolution";
inline constexpr const char* kLook = "LOOK";
inline constexpr const char* kResetLook = "Reset look";
// The LOOK's two groups (v7.1, PLAN_NR_LOOK_V71.md): the model's own
// controls, which restart its history, and the look stage's, which reshape
// what it returned, live.  "\xc2\xb7" is U+00B7, a middle dot.
inline constexpr const char* kLookModel = "Model \xc2\xb7 restarts NR history";
inline constexpr const char* kLookResult = "Result \xc2\xb7 live, reshapes NR's output";

inline constexpr const char* kPerformance = "Performance";
// The hook point left this section in rc3: the hint no longer names pre-SR.
inline constexpr const char* kPerformanceHint = "stacking, resolution, upsampling";
inline constexpr const char* kResetPerformance =
    "Reset Multi-Pass";
inline constexpr const char* kHotkeys = "Hotkeys & screenshots";
// The Hotkeys header's hint: the toggle key, then the screenshot key.
inline constexpr const char* kHotkeysHint = "%s toggle | %s screenshot";
inline constexpr const char* kResetHotkeys = "Reset hotkeys";
inline constexpr const char* kDiagnostics = "Diagnostics";
inline constexpr const char* kDiagnosticsHint = "counters, support report";
// Diagnostics holds three settings (GPU stage timers, the NR cost meter, the
// edit trace); through v8.0.2 its header never showed one had changed, and
// the cost meter off silently removes the card's cost line and Faster.
inline constexpr const char* kResetDiagnostics = "Reset diagnostics";
inline constexpr const char* kFixes = "Fixes";
inline constexpr const char* kFixesHint = "only change if you know what you are doing";
inline constexpr const char* kResetFixes = "Reset fixes";
inline constexpr const char* kFixesWarning =
    "These correct games that report their HDR or motion data wrongly.  The"
    " defaults are right for nearly every game, and a wrong value here breaks"
    " the image.  Reset fixes puts every one of them back.";
inline constexpr const char* kFixesHdr = "HDR";
inline constexpr const char* kFixesGuides = "Depth and motion";
inline constexpr const char* kAdaptationSpeed = "HDR adaptation speed, in stops per second";
inline constexpr const char* kRestoreAll = "Restore all defaults";
inline constexpr const char* kRestoreConfirm =
    "Every setting for this game goes back to its default, hotkeys included.";
inline constexpr const char* kDeveloperView = "Developer view";
inline constexpr const char* kDeveloperViewTip =
    "The classic flat layout: every control in one list, as before v7.";

// The panel's language (UiLanguage).
inline constexpr const char* kLanguage = "Language";
inline constexpr const char* kLanguageAuto = "Auto (ReShade's language)";  // i18n: skip (never translated, see DrawSectionLanguage)
inline constexpr const char* kLanguageTip =
    "Auto follows ReShade's own language (ReShade's Settings tab), which"
    " follows Windows unless you picked one there.  ReShade loads the font"
    " for its own language only, so a language written in another script"
    " can show as boxes: pick it in ReShade's settings too.";

// The resolution presets, in segment order.  Quality, Balanced and
// Performance are the working-resolution fractions of DLSS's own modes;
// "Match game" follows the game's render resolution (NRFollowInputRes).
inline constexpr const char* kResolutionLabels[] = {
    "Full", "Quality###nr_resolution_quality", "Balanced", "Performance", "Match game"};
inline constexpr const char* kResolutionTip =
    "The resolution Neural Rendering works at.  Full is the sharpest; the"
    " lower presets cost less GPU time.  Match game follows the game's own"
    " render resolution.";
inline constexpr const char* kCustomResolution = "Custom (%.0f%%)";

// NR's cost line (FormatNrCost) and the Faster options beside it (rc11).
// Each option shows NR's estimated GPU time per frame with it.
inline constexpr const char* kNrCostTip =
    "Estimated from NR's measured GPU time and the time between frames.  It"
    " holds while the GPU limits the frame rate; a game held back by the CPU"
    " or a frame cap gains less without NR.";
inline constexpr const char* kNrCost = "NR cost";
inline constexpr const char* kFaster = "Faster:";
inline constexpr const char* kFasterOptions[] = {
    "Before upscaling ~%.1f ms", "Resolution 50%% ~%.1f ms", "Both ~%.1f ms"};
inline constexpr const char* kFasterTips[] = {
    "Runs NR on the game's image before DLSS upscales it (Hook point:"
    " Render), so NR works on fewer pixels.",
    "Runs NR at half the output resolution per side (Resolution:"
    " Performance).  NR then steadies shimmering detail less than at Full.",
    "Both of the above."};
inline constexpr const char* kFasterUndo = "Undo";

// Diagnostics, translated (U3).  Raw details keeps the alpha44 counters.
inline constexpr const char* kLastFrame = "Last frame";
inline constexpr const char* kFramesThisSession = "Frames this session";
inline constexpr const char* kWorkingResolution = "Working resolution";
inline constexpr const char* kBackend = "Backend";
inline constexpr const char* kRawDetails = "Raw details";
inline constexpr const char* kCopySupportReport = "Copy support report";
inline constexpr const char* kOpenLogFolder = "Open log folder";
inline constexpr const char* kReportCopied =
    "Copied. Paste it into your report together with ReShade.log.";
inline constexpr const char* kReportTip =
    "Copies a short text with the version, the verdict, the card and the"
    " counters.  It holds the game's file name and no folder paths.";

// Why frames kept the game's image: the eligible declines, most frequent
// first (the "partly active" card's fix names this row; through v8.0.2 no
// such row existed).  The reasons themselves are the log's English.
inline constexpr const char* kSkippedBecause = "Skipped because";
inline constexpr const char* kSkippedBecauseTip =
    "Why Neural Rendering left frames alone this session, most frequent"
    " first, with the number of frames.  Each of those frames shows the"
    " game's own image.";

// The Present hook point's state (present[...] on the telemetry line), in
// Diagnostics while it serves, and frame generation on the status card.
inline constexpr const char* kAtPresent = "At the present";
inline constexpr const char* kAtPresentTip =
    "What the Present hook point did this session.  A present without NR"
    " shows the game's image for that frame, which can read as a flicker.";
// %u: the Present path's whole-number multiplier (present_fg_multiplier).
inline constexpr const char* kFrameGenEvery = "Frame generation: %ux, NR on every present";
inline constexpr const char* kFrameGenUnconfirmed = "Frame generation: %ux, NR path unconfirmed";
inline constexpr const char* kFrameGenFallback = "Frame generation: %ux, NR fallback to Upscaled";
inline constexpr const char* kFrameGenGameFrames =
    "Frame generation: %ux, NR on game frames only";
inline constexpr const char* kFrameGenBeforeFg =
    "Frame generation: %ux, Pre-FG observed; HUD may add NR work";
// The card's flicker line (present_gaps) and its reason's phrase.
inline constexpr const char* kPresentGapsCard =
    "%llu presents reached the screen without NR between NR frames, which"
    " reads as flicker.  Most often: %s";
inline constexpr const char* kPresentStability =
    "%llu presents without NR between NR frames | %llu slot waits, %llu timed"
    " out | %llu history restarts | %llu frame generation on/off switches";
inline constexpr const char* kNoFrameGen = "No frame generation (%.1f presents per game frame)";
inline constexpr const char* kPresentRuns =
    "NR on %llu presents: %llu with the game's depth and motion, %llu neutral";
inline constexpr const char* kPresentSkips =
    "Without NR: %llu busy, %llu repeated game frames, %llu without depth and motion";
inline constexpr const char* kPresentInFlight =
    "%llu presents took an older capture: the newest was still being written"
    " on another queue";

// Under the hook point, when the chosen one does not run in this session.
inline constexpr const char* kHookRenderRr =
    "This game upscales with Ray Reconstruction, so NR runs on its output"
    " (Upscaled).";
inline constexpr const char* kHookPresentUnusable =
    "This game's swapchain cannot be served at the present, so NR runs on the"
    " DLSS output (Upscaled).";
inline constexpr const char* kHookPresentVulkan =
    "In a Vulkan game NR does not run at the present yet; it runs on the DLSS"
    " output (Upscaled).";
inline constexpr const char* kHookPresentDx11Foreign =
    "In this Direct3D 11 game Present needs DX11Source=native in ReShade.ini;"
    " NR runs on the DLSS output (Upscaled).";

// Under Stack passes, both layouts (through v8.0.2 in Raw details only).

// The HUD pill's second line (U4): where the details are, and the one key
// that silences it.  %s: the overlay tab's name, then the toggle key.
inline constexpr const char* kHudHint =
    "Details: the %s tab in ReShade's overlay.  %s turns Neural Rendering off.";

// The issue notice (rc11): the HUD's corner note when a health ledger
// warning starts, under the warning's own title.  %u: the other active
// warnings; %s: the overlay tab's name.
inline constexpr const char* kIssueNoticeMore = "and %u more";
inline constexpr const char* kIssueNoticeHint =
    "Details and fixes: the %s tab in ReShade's overlay.";
inline constexpr const char* kIssueNotices = "On-screen notices";
inline constexpr const char* kHudPositionX = "Notice horizontal position";
inline constexpr const char* kHudPositionY = "Notice vertical position";
inline constexpr const char* kHudPositionTip =
    "Position of status notices on the game screen. Horizontal: 0% left, 100% right. "
    "Vertical: 0% top, 100% bottom. Default: top center (50%, 0%).";
inline constexpr const char* kIssueNoticesTip =
    "Shows NR activation, warning and issue notices on the game screen. "
    "Turning this off hides all on-screen notices; Home status and diagnostics remain available.";

// The first-run welcome (U5), shown under the status card until dismissed.
inline constexpr const char* kWelcomeTitle = "Welcome to " NR_NEXUS_NAME;
inline constexpr const char* kWelcomeBody =
    "Neural Rendering is NVIDIA's DLSS enhancer.  It works on the image the"
    " game's own DLSS makes, so DLSS or DLAA has to be on in the game's"
    " settings.";
// The welcome at the Present hook point, by NRPresentGuides (Optional,
// Required, Never): there DLSS is needed only for Required.  Through v8.0.2
// every hook point read kWelcomeBody, which says DLSS has to be on.
inline constexpr const char* kWelcomeBodyPresent[] = {
    "Neural Rendering is NVIDIA's DLSS enhancer.  At the Present hook point it"
    " works on the finished frame the game shows, so DLSS is not required: with"
    " DLSS or DLAA on it uses the game's depth and motion, without it neutral"
    " ones.",
    "Neural Rendering is NVIDIA's DLSS enhancer.  At the Present hook point it"
    " works on the finished frame the game shows, and Depth and motion:"
    " Required needs DLSS or DLAA on in the game's settings.",
    "Neural Rendering is NVIDIA's DLSS enhancer.  At the Present hook point"
    " with Depth and motion: Never it works on the finished frame the game"
    " shows with neutral depth and motion, so DLSS is not needed.",
};
inline constexpr const char* kWelcomeSteps[] = {
    "The card above says whether it is working.  When it is not, a note in"
    " the game's top-left corner says so, even with this overlay closed.",
    "The switch below turns it on and off; in game, the key shown beside it"
    " does the same.",
    "Strength and the Look section shape the image; a row you changed gets a"
    " reset arrow.",
};

// i18n: end
}  // namespace text

}  // namespace renodx::addons::dlss5::ui
