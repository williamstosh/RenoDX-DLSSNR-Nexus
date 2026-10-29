/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <limits>
#include <string_view>
#include <vector>

namespace renodx::addons::dlss5::presets {

struct FieldRule {
  const char* key;
  const char* label;
  const char* group;
  double minimum;
  double maximum;
  enum Category { QUALITY, MULTIPASS, COMPATIBILITY, GENERAL, EXCLUDED } category;
  unsigned pass = 0;
};

// Bounds describe LoadConfiguration's domain, not the sometimes narrower UI.
// Types and defaults come from the live Setting; non-Setting keys need the
// adapters documented in docs/PRESET_FORMAT.md. NaN bounds mean nonnumeric.
inline const std::vector<FieldRule>& FieldRules() {
  constexpr double NONNUMERIC = std::numeric_limits<double>::quiet_NaN();

// Literal expansion keeps keys stable for borrowed FindRule results and gives
// every additional pass exactly the same field set without owning strings.
#define RENODX_DLSS5_PRESET_PASS_RULES(PASS) \
    {.key = "NRPass" #PASS "Stage", .label = "Hook stage", .group = "Hook", .minimum = 0, .maximum = 3, .category = FieldRule::MULTIPASS, .pass = PASS}, \
    {.key = "NRPass" #PASS "ResolutionMode", .label = "Working resolution", .group = "Resolution", .minimum = 0, .maximum = 2, .category = FieldRule::MULTIPASS, .pass = PASS}, \
    {.key = "NRPass" #PASS "ResolutionScale", .label = "Resolution scale", .group = "Resolution", .minimum = 0.33f, .maximum = 1, .category = FieldRule::MULTIPASS, .pass = PASS}, \
    {.key = "NRPass" #PASS "Preset", .label = "NR model", .group = "Model", .minimum = 0, .maximum = 2, .category = FieldRule::MULTIPASS, .pass = PASS}, \
    {.key = "NRPass" #PASS "Intensity", .label = "Model strength", .group = "Model", .minimum = 0, .maximum = 2, .category = FieldRule::MULTIPASS, .pass = PASS}, \
    {.key = "NRPass" #PASS "Transfer", .label = "HDR transfer", .group = "Model", .minimum = 0, .maximum = 1, .category = FieldRule::MULTIPASS, .pass = PASS}, \
    {.key = "NRPass" #PASS "Color", .label = "Color", .group = "Model", .minimum = 0, .maximum = 1, .category = FieldRule::MULTIPASS, .pass = PASS}, \
    {.key = "NRPass" #PASS "FollowPass1", .label = "Same as pass 1", .group = "Steering", .minimum = 0, .maximum = 1, .category = FieldRule::MULTIPASS, .pass = PASS}, \
    {.key = "NRPass" #PASS "Style", .label = "Style", .group = "Steering", .minimum = 0, .maximum = 2, .category = FieldRule::MULTIPASS, .pass = PASS}, \
    {.key = "NRPass" #PASS "LocalTone", .label = "Local tone", .group = "Steering", .minimum = 0, .maximum = 2, .category = FieldRule::MULTIPASS, .pass = PASS}, \
    {.key = "NRPass" #PASS "Structure", .label = "Structure", .group = "Steering", .minimum = 0, .maximum = 2, .category = FieldRule::MULTIPASS, .pass = PASS}, \
    {.key = "NRPass" #PASS "Skin", .label = "Character / skin", .group = "Steering", .minimum = -1, .maximum = 1, .category = FieldRule::MULTIPASS, .pass = PASS}, \
    {.key = "NRPass" #PASS "AutoMask", .label = "Character mask", .group = "Steering", .minimum = 0, .maximum = 1, .category = FieldRule::MULTIPASS, .pass = PASS}, \
    {.key = "NRPass" #PASS "UICorrection", .label = "UI correction", .group = "Steering", .minimum = 0, .maximum = 1, .category = FieldRule::MULTIPASS, .pass = PASS}, \
    {.key = "NRPass" #PASS "LookFollowPass1", .label = "Inherit Pass 1 result settings", .group = "Result", .minimum = 0, .maximum = 1, .category = FieldRule::MULTIPASS, .pass = PASS}, \
    {.key = "NRPass" #PASS "LookMode", .label = "Adjust NR's look", .group = "Result", .minimum = 0, .maximum = 1, .category = FieldRule::MULTIPASS, .pass = PASS}, \
    {.key = "NRPass" #PASS "LookStrength", .label = "Result adjustment strength", .group = "Result", .minimum = 0, .maximum = 2, .category = FieldRule::MULTIPASS, .pass = PASS}, \
    {.key = "NRPass" #PASS "LookBrighten", .label = "Brightening", .group = "Result", .minimum = 0, .maximum = 2, .category = FieldRule::MULTIPASS, .pass = PASS}, \
    {.key = "NRPass" #PASS "LookDarken", .label = "Darkening", .group = "Result", .minimum = 0, .maximum = 2, .category = FieldRule::MULTIPASS, .pass = PASS}, \
    {.key = "NRPass" #PASS "LookMaxBrighten", .label = "Max brightening", .group = "Result", .minimum = 0, .maximum = 4, .category = FieldRule::MULTIPASS, .pass = PASS}, \
    {.key = "NRPass" #PASS "LookMaxDarken", .label = "Max darkening", .group = "Result", .minimum = 0, .maximum = 4, .category = FieldRule::MULTIPASS, .pass = PASS}, \
    {.key = "NRPass" #PASS "LookColour", .label = "Colour", .group = "Result", .minimum = 0, .maximum = 2, .category = FieldRule::MULTIPASS, .pass = PASS}, \
    {.key = "NRPass" #PASS "LookHue", .label = "Hue shift", .group = "Result", .minimum = 0, .maximum = 2, .category = FieldRule::MULTIPASS, .pass = PASS}, \
    {.key = "NRPass" #PASS "LookMaxColour", .label = "Max colour change", .group = "Result", .minimum = 0, .maximum = 2, .category = FieldRule::MULTIPASS, .pass = PASS}, \
    {.key = "NRPass" #PASS "LookShadows", .label = "Shadows", .group = "Result", .minimum = 0, .maximum = 2, .category = FieldRule::MULTIPASS, .pass = PASS}, \
    {.key = "NRPass" #PASS "LookMidtones", .label = "Midtones", .group = "Result", .minimum = 0, .maximum = 2, .category = FieldRule::MULTIPASS, .pass = PASS}, \
    {.key = "NRPass" #PASS "LookHighlights", .label = "Highlights", .group = "Result", .minimum = 0, .maximum = 2, .category = FieldRule::MULTIPASS, .pass = PASS}, \
    {.key = "NRPass" #PASS "LookTone", .label = "Large-scale tone", .group = "Result", .minimum = 0, .maximum = 2, .category = FieldRule::MULTIPASS, .pass = PASS}, \
    {.key = "NRPass" #PASS "LookDetail", .label = "Fine detail", .group = "Result", .minimum = 0, .maximum = 2, .category = FieldRule::MULTIPASS, .pass = PASS}, \
    {.key = "NRPass" #PASS "LookHalo", .label = "Halo suppression", .group = "Result", .minimum = 0, .maximum = 1, .category = FieldRule::MULTIPASS, .pass = PASS}, \
    {.key = "NRPass" #PASS "LookHaloBrightSide", .label = "Halo next to dark objects", .group = "Result", .minimum = 0, .maximum = 1, .category = FieldRule::MULTIPASS, .pass = PASS}, \
    {.key = "NRPass" #PASS "LookDetailRadius", .label = "Detail radius", .group = "Result", .minimum = 0.25, .maximum = 4, .category = FieldRule::MULTIPASS, .pass = PASS}, \
    {.key = "NRPass" #PASS "LookStabilize", .label = "Stabilize", .group = "Result", .minimum = 0, .maximum = 2, .category = FieldRule::MULTIPASS, .pass = PASS}, \
    {.key = "NRPass" #PASS "LookStabilizeMs", .label = "Stabilize time", .group = "Result", .minimum = 10, .maximum = 250, .category = FieldRule::MULTIPASS, .pass = PASS}, \
    {.key = "NRPass" #PASS "LookStabilizeDetail", .label = "Stabilize detail", .group = "Result", .minimum = 0, .maximum = 1, .category = FieldRule::MULTIPASS, .pass = PASS}, \
    {.key = "NRPass" #PASS "DetailStability", .label = "Detail stability", .group = "Result", .minimum = 0, .maximum = 2, .category = FieldRule::MULTIPASS, .pass = PASS}

  static const std::vector<FieldRule> rules = {
      {.key = "NRIntensity", .label = "Model strength", .group = "Model", .minimum = 0, .maximum = 2, .category = FieldRule::QUALITY, .pass = 1},
      {.key = "NRPreset", .label = "NR model", .group = "Model", .minimum = 0, .maximum = 3, .category = FieldRule::QUALITY, .pass = 1},
      {.key = "NRStyle", .label = "Style", .group = "Steering", .minimum = 0, .maximum = 2, .category = FieldRule::QUALITY, .pass = 1},
      {.key = "NRLocalTone", .label = "Local tone", .group = "Steering", .minimum = 0, .maximum = 2, .category = FieldRule::QUALITY, .pass = 1},
      {.key = "NRLocalStructure", .label = "Structure", .group = "Steering", .minimum = 0, .maximum = 2, .category = FieldRule::QUALITY, .pass = 1},
      {.key = "NRSkinStructure", .label = "Character / skin", .group = "Steering", .minimum = -1, .maximum = 1, .category = FieldRule::QUALITY, .pass = 1},
      {.key = "NRSkinIndependent", .label = "Independent skin", .group = "Steering", .minimum = 0, .maximum = 1, .category = FieldRule::QUALITY},
      {.key = "NRAutoMask", .label = "Character mask", .group = "Steering", .minimum = 0, .maximum = 1, .category = FieldRule::QUALITY, .pass = 1},
      {.key = "NRUICorrection", .label = "UI correction", .group = "Steering", .minimum = 0, .maximum = 1, .category = FieldRule::QUALITY, .pass = 1},
      {.key = "NRLookMode", .label = "Adjust NR's look", .group = "Result", .minimum = 0, .maximum = 1, .category = FieldRule::QUALITY, .pass = 1},
      {.key = "NRLookStrength", .label = "Result adjustment strength", .group = "Result", .minimum = 0, .maximum = 2, .category = FieldRule::QUALITY, .pass = 1},
      {.key = "NRLookBrighten", .label = "Brightening", .group = "Result", .minimum = 0, .maximum = 2, .category = FieldRule::QUALITY, .pass = 1},
      {.key = "NRLookDarken", .label = "Darkening", .group = "Result", .minimum = 0, .maximum = 2, .category = FieldRule::QUALITY, .pass = 1},
      {.key = "NRLookMaxBrighten", .label = "Max brightening", .group = "Result", .minimum = 0, .maximum = 4, .category = FieldRule::QUALITY, .pass = 1},
      {.key = "NRLookMaxDarken", .label = "Max darkening", .group = "Result", .minimum = 0, .maximum = 4, .category = FieldRule::QUALITY, .pass = 1},
      {.key = "NRLookColour", .label = "Colour", .group = "Result", .minimum = 0, .maximum = 2, .category = FieldRule::QUALITY, .pass = 1},
      {.key = "NRLookHue", .label = "Hue shift", .group = "Result", .minimum = 0, .maximum = 2, .category = FieldRule::QUALITY, .pass = 1},
      {.key = "NRLookMaxColour", .label = "Max colour change", .group = "Result", .minimum = 0, .maximum = 2, .category = FieldRule::QUALITY, .pass = 1},
      {.key = "NRLookShadows", .label = "Shadows", .group = "Result", .minimum = 0, .maximum = 2, .category = FieldRule::QUALITY, .pass = 1},
      {.key = "NRLookMidtones", .label = "Midtones", .group = "Result", .minimum = 0, .maximum = 2, .category = FieldRule::QUALITY, .pass = 1},
      {.key = "NRLookHighlights", .label = "Highlights", .group = "Result", .minimum = 0, .maximum = 2, .category = FieldRule::QUALITY, .pass = 1},
      {.key = "NRLookTone", .label = "Large-scale tone", .group = "Result", .minimum = 0, .maximum = 2, .category = FieldRule::QUALITY, .pass = 1},
      {.key = "NRLookDetail", .label = "Fine detail", .group = "Result", .minimum = 0, .maximum = 2, .category = FieldRule::QUALITY, .pass = 1},
      {.key = "NRLookHalo", .label = "Halo suppression", .group = "Result", .minimum = 0, .maximum = 1, .category = FieldRule::QUALITY, .pass = 1},
      {.key = "NRLookHaloBrightSide", .label = "Halo next to dark objects", .group = "Result", .minimum = 0, .maximum = 1, .category = FieldRule::QUALITY, .pass = 1},
      {.key = "NRLookDetailRadius", .label = "Detail radius", .group = "Result", .minimum = 0.25, .maximum = 4, .category = FieldRule::QUALITY, .pass = 1},
      {.key = "NRLookStabilize", .label = "Stabilize", .group = "Result", .minimum = 0, .maximum = 2, .category = FieldRule::QUALITY, .pass = 1},
      {.key = "NRLookStabilizeMs", .label = "Stabilize time", .group = "Result", .minimum = 10, .maximum = 250, .category = FieldRule::QUALITY, .pass = 1},
      {.key = "NRLookStabilizeDetail", .label = "Stabilize detail", .group = "Result", .minimum = 0, .maximum = 1, .category = FieldRule::QUALITY, .pass = 1},
      {.key = "NRDetailStability", .label = "Detail stability", .group = "Result", .minimum = 0, .maximum = 2, .category = FieldRule::QUALITY, .pass = 1},
      {.key = "NRPasses", .label = "Pass Count", .group = "Multi-Pass", .minimum = 1, .maximum = 4, .category = FieldRule::MULTIPASS},
      RENODX_DLSS5_PRESET_PASS_RULES(2),
      RENODX_DLSS5_PRESET_PASS_RULES(3),
      RENODX_DLSS5_PRESET_PASS_RULES(4),

      {.key = "NRHookPoint", .label = "Hook point", .group = "Hook", .minimum = 0, .maximum = 2, .category = FieldRule::QUALITY},
      {.key = "NRPresentGuides", .label = "Depth and motion", .group = "Hook", .minimum = 0, .maximum = 2, .category = FieldRule::COMPATIBILITY},
      {.key = "NRPresentEncoding", .label = "Frame encoding", .group = "HDR", .minimum = 0, .maximum = 3, .category = FieldRule::COMPATIBILITY},
      {.key = "NRPresentWhiteNits", .label = "HDR paper white", .group = "HDR", .minimum = 0, .maximum = 10000, .category = FieldRule::COMPATIBILITY},
      {.key = "NRPresentUICorrection", .label = "Keep the HUD", .group = "Hook", .minimum = 0, .maximum = 1, .category = FieldRule::COMPATIBILITY},
      {.key = "NRPresentFrames", .label = "Generated frames", .group = "Hook", .minimum = 0, .maximum = 1, .category = FieldRule::COMPATIBILITY},
      {.key = "NRPresentFgSource", .label = "Frame generation detection", .group = "Hook", .minimum = 0, .maximum = 2, .category = FieldRule::COMPATIBILITY},
      {.key = "NRPresentSdrRestorePolicy", .label = "Present SDR restore", .group = "HDR", .minimum = 0, .maximum = 4, .category = FieldRule::COMPATIBILITY},
      {.key = "NRFollowInputRes", .label = "Follow input resolution", .group = "Resolution", .minimum = 0, .maximum = 1, .category = FieldRule::QUALITY},
      {.key = "NRResolutionScale", .label = "Resolution scale", .group = "Resolution", .minimum = 0.33f, .maximum = 1, .category = FieldRule::QUALITY},
      {.key = "NRLookUpsample", .label = "Upsampling", .group = "Resolution", .minimum = 0, .maximum = 3, .category = FieldRule::QUALITY},
      {.key = "NRCodecMode", .label = "HDR mode", .group = "HDR", .minimum = 0, .maximum = 3, .category = FieldRule::COMPATIBILITY},
      {.key = "NRProxyAnchor", .label = "Proxy anchor (nits)", .group = "HDR", .minimum = 0.5, .maximum = 32, .category = FieldRule::COMPATIBILITY},
      {.key = "NRSourceEncoding", .label = "Encoding", .group = "HDR", .minimum = 0, .maximum = 3, .category = FieldRule::COMPATIBILITY},
      {.key = "NRLinearUnitNits", .label = "Linear unit (nits)", .group = "HDR", .minimum = 0, .maximum = 10000, .category = FieldRule::COMPATIBILITY},
      {.key = "NRSourcePrimaries", .label = "Primaries", .group = "HDR", .minimum = 0, .maximum = 3, .category = FieldRule::COMPATIBILITY},
      {.key = "NRPQCalibration", .label = "PQ calibration", .group = "HDR", .minimum = 0.005f, .maximum = 16, .category = FieldRule::COMPATIBILITY},
      {.key = "NRDiffuseWhiteNits", .label = "Paper white (nits)", .group = "HDR", .minimum = 80, .maximum = 1000, .category = FieldRule::COMPATIBILITY},
      {.key = "NRPaperWhiteScale", .label = "Scene white scale", .group = "HDR", .minimum = 0.005f, .maximum = 16, .category = FieldRule::COMPATIBILITY},
      {.key = "NRTransferStrength", .label = "HDR transfer", .group = "Model", .minimum = 0, .maximum = 1, .category = FieldRule::QUALITY, .pass = 1},
      {.key = "NRColorStrength", .label = "Color", .group = "Model", .minimum = 0, .maximum = 1, .category = FieldRule::QUALITY, .pass = 1},
      {.key = "NRChromaClamp", .label = "Chroma clamp", .group = "HDR", .minimum = 0.25, .maximum = 2, .category = FieldRule::COMPATIBILITY},
      {.key = "NRTransferMode", .label = "Neural transfer", .group = "HDR", .minimum = 0, .maximum = 1, .category = FieldRule::COMPATIBILITY},
      {.key = "NRStyleHighlightGuard", .label = "Style highlight guard (stops)", .group = "HDR", .minimum = 0, .maximum = 2, .category = FieldRule::COMPATIBILITY},
      {.key = "NRDisplayPedestal", .label = "Dark pedestal removal", .group = "HDR", .minimum = 0, .maximum = 2, .category = FieldRule::COMPATIBILITY},
      {.key = "NRNeuralFloorGuard", .label = "Near-black colour guard", .group = "HDR", .minimum = 0, .maximum = 1, .category = FieldRule::COMPATIBILITY},
      {.key = "NRFeedMode", .label = "Brightness source", .group = "HDR", .minimum = 0, .maximum = 2, .category = FieldRule::COMPATIBILITY},
      {.key = "NRNormGovernor", .label = "Normalization", .group = "HDR", .minimum = 0, .maximum = 2, .category = FieldRule::COMPATIBILITY},
      {.key = "NRNormAttackStops", .label = "Normalization attack (stops)", .group = "HDR", .minimum = 0.1f, .maximum = 64, .category = FieldRule::COMPATIBILITY},
      {.key = "NRNormReleaseStops", .label = "Normalization release (stops)", .group = "HDR", .minimum = 0.05f, .maximum = 16, .category = FieldRule::COMPATIBILITY},
      {.key = "NRNormCutAware", .label = "Hold through black, re-prime on cuts", .group = "HDR", .minimum = 0, .maximum = 1, .category = FieldRule::COMPATIBILITY},
      {.key = "NRNormSlewStops", .label = "Slew (stops/frame)", .group = "HDR", .minimum = 0, .maximum = 0.5, .category = FieldRule::COMPATIBILITY},
      {.key = "NRDepthMode", .label = "Depth", .group = "Depth and motion", .minimum = 0, .maximum = 2, .category = FieldRule::COMPATIBILITY},
      {.key = "NRMVecScaleX", .label = "Motion scale X", .group = "Depth and motion", .minimum = -2, .maximum = 2, .category = FieldRule::COMPATIBILITY},
      {.key = "NRMVecScaleY", .label = "Motion scale Y", .group = "Depth and motion", .minimum = -2, .maximum = 2, .category = FieldRule::COMPATIBILITY},
      {.key = "NRChainedHistory", .label = "Chained history", .group = "Depth and motion", .minimum = 0, .maximum = 1, .category = FieldRule::COMPATIBILITY},

      // Full-backup fields read outside kSettingSections; no defaults live here.
      {.key = "EnableHooks", .label = "Hook policy", .group = "API and runtime", .minimum = 0, .maximum = 2, .category = FieldRule::COMPATIBILITY},
      {.key = "DX11Source", .label = "Direct3D 11 source", .group = "API and runtime", .minimum = NONNUMERIC, .maximum = NONNUMERIC, .category = FieldRule::COMPATIBILITY},
      {.key = "ForeignNr", .label = "Foreign NR policy", .group = "API and runtime", .minimum = NONNUMERIC, .maximum = NONNUMERIC, .category = FieldRule::COMPATIBILITY},
      {.key = "DedupeMode", .label = "Deduplication", .group = "API and runtime", .minimum = 0, .maximum = 1, .category = FieldRule::COMPATIBILITY},
      {.key = "ListStateMode", .label = "Command list state", .group = "API and runtime", .minimum = 0, .maximum = 1, .category = FieldRule::COMPATIBILITY},
      {.key = "NRMaxWorksets", .label = "Maximum worksets", .group = "API and runtime", .minimum = 1, .maximum = 8, .category = FieldRule::COMPATIBILITY},
      {.key = "NRWorksetFreePolicy", .label = "Workset release policy", .group = "API and runtime", .minimum = 0, .maximum = 1, .category = FieldRule::COMPATIBILITY},
      {.key = "NRDisableRelease", .label = "Release memory while disabled", .group = "API and runtime", .minimum = 0, .maximum = 1, .category = FieldRule::COMPATIBILITY},
      {.key = "NRShutdownQuiesce", .label = "Shutdown quiesce", .group = "API and runtime", .minimum = 0, .maximum = 1, .category = FieldRule::COMPATIBILITY},
      {.key = "NRVkStateRestore", .label = "Vulkan state restore", .group = "API and runtime", .minimum = 0, .maximum = 1, .category = FieldRule::COMPATIBILITY},
      {.key = "NRPresentWhiteSource", .label = "Present white source", .group = "HDR", .minimum = 0, .maximum = 1, .category = FieldRule::COMPATIBILITY},
      {.key = "NRPresentPreFg", .label = "Pre-FG route", .group = "Hook", .minimum = 0, .maximum = 3, .category = FieldRule::COMPATIBILITY},
      {.key = "NRPresentPreFgContract", .label = "Pre-FG contract", .group = "Hook", .minimum = 0, .maximum = 1, .category = FieldRule::COMPATIBILITY},
      {.key = "NRPresentBlockHudless", .label = "Block HUD-less input", .group = "Hook", .minimum = 0, .maximum = 1, .category = FieldRule::COMPATIBILITY},
      {.key = "NRPresentMVecUnits", .label = "Present motion units", .group = "Depth and motion", .minimum = 0, .maximum = 2, .category = FieldRule::COMPATIBILITY},
      {.key = "NRAutoFgFallback", .label = "Automatic FG fallback", .group = "Hook", .minimum = 0, .maximum = 1, .category = FieldRule::COMPATIBILITY},
      {.key = "NRPresentHdrPedestalPolicy", .label = "Present HDR pedestal", .group = "HDR", .minimum = 0, .maximum = 2, .category = FieldRule::COMPATIBILITY},
      {.key = "NRPresentRestoreScope", .label = "Present restore scope", .group = "HDR", .minimum = 0, .maximum = 1, .category = FieldRule::COMPATIBILITY},
      {.key = "NROutputRect", .label = "Output rectangle", .group = "Resolution", .minimum = 0, .maximum = 2, .category = FieldRule::COMPATIBILITY},

      {.key = "NeuralUplift", .label = "Neural Rendering", .group = "General", .minimum = 0, .maximum = 1, .category = FieldRule::GENERAL},
      {.key = "NRToggleKey", .label = "Toggle NR", .group = "Hotkeys", .minimum = 0x08, .maximum = 0xFE, .category = FieldRule::GENERAL},
      {.key = "NRScreenshotKey", .label = "Screenshot", .group = "Hotkeys", .minimum = 0x08, .maximum = 0xFE, .category = FieldRule::GENERAL},
      {.key = "NRScreenshotSource", .label = "Capture source", .group = "Screenshots", .minimum = 0, .maximum = 2, .category = FieldRule::GENERAL},
      {.key = "NRScreenshotFormat", .label = "File format", .group = "Screenshots", .minimum = 0, .maximum = 2, .category = FieldRule::GENERAL},
      {.key = "NRScreenshotLayout", .label = "Comparison layout", .group = "Screenshots", .minimum = 0, .maximum = 1, .category = FieldRule::GENERAL},
      {.key = "NRScreenshotJpegQuality", .label = "JPEG quality", .group = "Screenshots", .minimum = 80, .maximum = 100, .category = FieldRule::GENERAL},
      {.key = "NRScreenshotMaxMB", .label = "JPEG size limit (MB)", .group = "Screenshots", .minimum = 0, .maximum = 500, .category = FieldRule::GENERAL},
      {.key = "NRGpuTimers", .label = "GPU timers", .group = "Diagnostics", .minimum = 0, .maximum = 1, .category = FieldRule::GENERAL},
      {.key = "NRCostMeter", .label = "NR cost", .group = "Diagnostics", .minimum = 0, .maximum = 1, .category = FieldRule::GENERAL},
      {.key = "NRCrashDump", .label = "Crash dumps", .group = "Diagnostics", .minimum = 0, .maximum = 1, .category = FieldRule::GENERAL},
      {.key = "UiMode", .label = "Developer view", .group = "UI", .minimum = NONNUMERIC, .maximum = NONNUMERIC, .category = FieldRule::GENERAL},
      {.key = "UiTheme", .label = "Interface theme", .group = "UI", .minimum = NONNUMERIC, .maximum = NONNUMERIC, .category = FieldRule::GENERAL},
      {.key = "UiLanguage", .label = "Language", .group = "UI", .minimum = NONNUMERIC, .maximum = NONNUMERIC, .category = FieldRule::GENERAL},
      {.key = "UiWelcomed", .label = "Welcome dismissed", .group = "UI", .minimum = 0, .maximum = 1, .category = FieldRule::GENERAL},
      {.key = "UiIssueNotice", .label = "On-screen notices", .group = "UI", .minimum = 0, .maximum = 1, .category = FieldRule::GENERAL},
      {.key = "UiHudX", .label = "Notice horizontal position", .group = "UI", .minimum = 0, .maximum = 100, .category = FieldRule::GENERAL},
      {.key = "UiHudY", .label = "Notice vertical position", .group = "UI", .minimum = 0, .maximum = 100, .category = FieldRule::GENERAL},

      // Debug overrides and capture/trace requests must not travel in presets.
      {.key = "NRDebugBypassSdrRestore", .label = "Bypass SDR restore", .group = "Session debug", .minimum = 0, .maximum = 1, .category = FieldRule::EXCLUDED},
      {.key = "NRDebugBypassHdrPedestal", .label = "Bypass HDR pedestal", .group = "Session debug", .minimum = 0, .maximum = 1, .category = FieldRule::EXCLUDED},
      {.key = "NRDebugHdrCaptureMode", .label = "HDR capture mode", .group = "Session debug", .minimum = 0, .maximum = 1, .category = FieldRule::EXCLUDED},
      {.key = "NRDebugBypassHostLook", .label = "Bypass host look", .group = "Session debug", .minimum = 0, .maximum = 1, .category = FieldRule::EXCLUDED},
      {.key = "NRPresentModelUI", .label = "Diagnostic model UI", .group = "Session debug", .minimum = 0, .maximum = 2, .category = FieldRule::EXCLUDED},
      {.key = "NRDebugGuideMode", .label = "Diagnostic guides", .group = "Session debug", .minimum = 0, .maximum = 2, .category = FieldRule::EXCLUDED},
      {.key = "NRDebugCapturePlanes", .label = "Capture stage planes", .group = "Session debug", .minimum = 0, .maximum = 1, .category = FieldRule::EXCLUDED},
      {.key = "NREditTrace", .label = "Edit trace", .group = "Session debug", .minimum = 0, .maximum = 1, .category = FieldRule::EXCLUDED},
      {.key = "NRNormTrace", .label = "Normalization trace", .group = "Session debug", .minimum = 0, .maximum = 1, .category = FieldRule::EXCLUDED},
      {.key = "NRScreenshotPlanes", .label = "Diagnostic screenshot planes", .group = "Session debug", .minimum = 0, .maximum = 1, .category = FieldRule::EXCLUDED},
      {.key = "NRTelemetrySeconds", .label = "Telemetry interval (s)", .group = "Session debug", .minimum = 0, .maximum = 3600, .category = FieldRule::EXCLUDED},
      {.key = "NRReportHost", .label = "Report upload hosts", .group = "Upload information", .minimum = NONNUMERIC, .maximum = NONNUMERIC, .category = FieldRule::EXCLUDED},
      {.key = "NRPreUpscale", .label = "Retired pre-upscale alias", .group = "Legacy", .minimum = 0, .maximum = 1, .category = FieldRule::EXCLUDED},
      {.key = "NRGlobalTone", .label = "Retired global tone", .group = "Legacy", .minimum = NONNUMERIC, .maximum = NONNUMERIC, .category = FieldRule::EXCLUDED},
      {.key = "NRSdrBlackRestore", .label = "Read-only legacy SDR restore", .group = "Legacy", .minimum = 0, .maximum = 2, .category = FieldRule::EXCLUDED},
      {.key = "ConfigVersion", .label = "Configuration schema", .group = "Schema", .minimum = NONNUMERIC, .maximum = NONNUMERIC, .category = FieldRule::EXCLUDED},
      {.key = "ProfileGameId", .label = "Private game profile identity", .group = "Profiles", .minimum = NONNUMERIC, .maximum = NONNUMERIC, .category = FieldRule::EXCLUDED},
      {.key = "NRPresentSdrRestoreConfigVersion", .label = "SDR restore schema", .group = "Schema", .minimum = NONNUMERIC, .maximum = NONNUMERIC, .category = FieldRule::EXCLUDED},
      {.key = "NRPresentHdrPedestalConfigVersion", .label = "HDR pedestal schema", .group = "Schema", .minimum = NONNUMERIC, .maximum = NONNUMERIC, .category = FieldRule::EXCLUDED},
      {.key = "NRPresentRestoreScopeConfigVersion", .label = "Restore scope schema", .group = "Schema", .minimum = NONNUMERIC, .maximum = NONNUMERIC, .category = FieldRule::EXCLUDED},
  };
#undef RENODX_DLSS5_PRESET_PASS_RULES

  return rules;
}

// The table is immutable; returned pointers remain valid for this module's life.
inline const FieldRule* FindRule(std::string_view key) {
  for (const auto& rule : FieldRules()) {
    if (key == rule.key) return &rule;
  }
  return nullptr;
}

}  // namespace renodx::addons::dlss5::presets
