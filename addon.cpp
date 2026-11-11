/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#if defined(RENODX_DLSS5_DEBUG)
// Debug variant: enable ReShade-side utils debug logging alongside the
// addon's own trace log + crash dumps (see debug.hpp).
#define DEBUG_LEVEL_2
#else
#define DEBUG_LEVEL_0
#endif

#include <windows.h>

#include "brand.h"
#include "dlssnr.hpp"

#include "lastgasp.hpp"
#include "output_framerate.hpp"

namespace {
void ObserveOutputPresent(reshade::api::command_queue* queue, reshade::api::swapchain* swapchain,
    const reshade::api::rect*, const reshade::api::rect*, uint32_t, const reshade::api::rect*) {
  if (queue == nullptr || swapchain == nullptr || swapchain->get_back_buffer_count() == 0) return;
  namespace nr = renodx::addons::dlss5;
  const bool fg_active = nr::internal::present_fg_multiplier.load(std::memory_order_relaxed) > 1
      || nr::internal::native_fg_declaration.load(std::memory_order_relaxed)
          == nr::internal::NativeFgDeclaration::kOn;
  nr::output_framerate::ObservePresent(reinterpret_cast<uintptr_t>(swapchain),
      nr::internal::configuration_generation.load(std::memory_order_relaxed),
      fg_active, nr::output_framerate::NowNs());
}

void ObserveOutputDestroy(reshade::api::swapchain*, bool) {
  renodx::addons::dlss5::output_framerate::Invalidate();
}
}  // namespace

#if defined(NR_NEXUS_IME_DIAGNOSTICS)
#include "tests/ime_trace.hpp"
#endif

extern "C" __declspec(dllexport) constexpr const char* NAME = NR_NEXUS_ADDON_NAME;
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION = NR_NEXUS_DESCRIPTION;

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH) {
    // Before register_addon: the last-gasp reporter covers even a death in
    // the registration path.  The CRT and static initializers are already up.
    renodx::addons::dlss5::lastgasp::Install(
        module, renodx::addons::dlss5::internal::kAddonVersion);
    if (!reshade::register_addon(module)) {
      // No ReShade to register with: the loader unmaps this image next, and
      // the reporter's handlers must not outlive it.
      renodx::addons::dlss5::lastgasp::Exit(false);
      renodx::addons::dlss5::lastgasp::Uninstall();
      return FALSE;
    }
    reshade::register_event<reshade::addon_event::present>(ObserveOutputPresent);
    reshade::register_event<reshade::addon_event::destroy_swapchain>(ObserveOutputDestroy);
  }

  renodx::addons::dlss5::Use(reason, module, reserved);

#if defined(NR_NEXUS_IME_DIAGNOSTICS)
  if (reason == DLL_PROCESS_ATTACH) {
    reshade::register_overlay(NR_NEXUS_NAME " IME diagnostics", nr_ime_trace::Draw);
  } else if (reason == DLL_PROCESS_DETACH) {
    nr_ime_trace::Stop();
    reshade::unregister_overlay(NR_NEXUS_NAME " IME diagnostics", nr_ime_trace::Draw);
  }
#endif

  if (reason == DLL_PROCESS_DETACH) {
    reshade::unregister_event<reshade::addon_event::present>(ObserveOutputPresent);
    reshade::unregister_event<reshade::addon_event::destroy_swapchain>(ObserveOutputDestroy);
    reshade::unregister_addon(module);
    // `reserved` is null for FreeLibrary and non-null at process exit.
    // [exit] (lastgasp.hpp): the session ended here, not in a silent death.
    renodx::addons::dlss5::lastgasp::Exit(reserved != nullptr);
    if (reserved == nullptr) renodx::addons::dlss5::lastgasp::Uninstall();
  }
  return TRUE;
}
