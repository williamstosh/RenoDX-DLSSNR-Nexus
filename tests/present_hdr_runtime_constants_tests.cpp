/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#define DEBUG_LEVEL_0

#include <cstdint>
#include <iostream>
#include <windows.h>

#include <dlssnr.hpp>

namespace {
namespace runtime = renodx::addons::dlss5::internal;
namespace hdr = renodx::addons::dlss5::present_hdr_pedestal;
namespace codec = renodx::dlss5::codec;

uint32_t checks = 0;
uint32_t failures = 0;

void Check(bool condition, const char* description, uint8_t hdr_mode, bool absolute,
           uint32_t mode, bool floor_guard, uint32_t policy_value = 0, bool bypass = false) {
  ++checks;
  if (condition) return;
  ++failures;
  std::cerr << "FAIL: " << description << " hdr=" << static_cast<uint32_t>(hdr_mode)
            << " absolute=" << absolute << " codec=" << mode << " floor=" << floor_guard
            << " policy=" << policy_value << " bypass=" << bypass << '\n';
}
}  // namespace

int main() {
  uint32_t combinations = 0;
  uint32_t hdr_floor_curve_checks = 0;
  runtime::display_pedestal.store(0);
  runtime::transfer_strength.store(1.f);
  runtime::color_strength.store(0.5f);

  for (uint8_t hdr_mode = 0; hdr_mode <= 2; ++hdr_mode) {
    for (const bool absolute : {false, true}) {
      for (uint32_t mode = 0; mode <= 3; ++mode) {
        for (const bool floor_guard : {false, true}) {
          ++combinations;
          runtime::codec_mode.store(mode);
          runtime::neural_floor_guard.store(floor_guard);
          runtime::PresentTarget target;
          target.hdr_mode = hdr_mode;
          target.units = {.encoding = (hdr_mode == 2 ? codec::Encoding::Pq : codec::Encoding::Linear),
                          .absolute = absolute, .unit_nits = (hdr_mode == 2 ? 10000.f : 80.f)};
          target.hdr_codec_mode = mode;
          const bool display = runtime::FrameDisplayCodec(target, mode);
          // Reproduce only the Present call-site's snapshot composition; the codec predicate and
          // comparison builder are actual production function calls, not local implementations.
          target.hdr_curve = (display ? 1u : (floor_guard ? 2u : 0u));
          target.pass_transfer_strength[0] = 1.f;
          target.pass_color_strength[0] = 0.5f;

          const auto old_constants = runtime::MakeV6Constants(
              target, 64, 64, 64, 64, 0, 0, 64, 64, 64, 64, 1.f, 0.01f,
              1.f, 0.5f, static_cast<uint32_t>(target.units.encoding), 0.005f, 2.f);
          Check(runtime::FrameDisplayCodec(target) == display,
                "default and explicit snapshot mode use the same production predicate",
                hdr_mode, absolute, mode, floor_guard);
          Check(old_constants.curve == target.hdr_curve,
                "old MakeV6Constants curve equals frozen Present curve",
                hdr_mode, absolute, mode, floor_guard);
          if (hdr_mode != 0 && floor_guard && !display) {
            ++hdr_floor_curve_checks;
            Check(old_constants.curve == 2u && target.hdr_curve == 2u,
                  "HDR non-Display floor guard retains curve 2",
                  hdr_mode, absolute, mode, floor_guard);
          }

          for (const uint32_t policy_value : {0u, 1u}) {
            for (const bool bypass : {false, true}) {
              target.hdr_pedestal = hdr::Decide(
                  hdr::LoadPolicy(policy_value, hdr::CONFIG_VERSION),
                  {.actual_present = true, .hdr_mode = hdr_mode, .v6_available = mode != 0u,
                   .encoding_valid = true, .absolute_units = target.units.absolute,
                   .unit_nits = target.units.unit_nits, .display_codec = display,
                   .legacy_mode = 0u, .debug_bypass = bypass});
              Check(runtime::ChainActive(target, 1, &target),
                    "Legacy/bypass/Off keep NR active with transfer 1 and positive color",
                    hdr_mode, absolute, mode, floor_guard, policy_value, bypass);
              if (target.hdr_pedestal.active && (policy_value == 1u || bypass)) {
                Check(target.hdr_pedestal.effective_gate == 0.f,
                      "active Off/bypass supplies zero application gate",
                      hdr_mode, absolute, mode, floor_guard, policy_value, bypass);
              }

              if (hdr_mode != 0) {
                target.pass_transfer_strength[0] = 0.f;
                Check(runtime::ChainActive(target, 1, &target) == display,
                      "color alone keeps Display active; other codecs still need transfer",
                      hdr_mode, absolute, mode, floor_guard, policy_value, bypass);
                target.pass_color_strength[0] = 0.f;
                Check(!runtime::ChainActive(target, 1, &target),
                      "zero transfer and color remain inactive",
                      hdr_mode, absolute, mode, floor_guard, policy_value, bypass);
                target.pass_transfer_strength[0] = 1.f;
                target.pass_color_strength[0] = 0.5f;
              }
              Check(target.hdr_curve == old_constants.curve,
                    "formal policy and bypass leave frozen curve unchanged",
                    hdr_mode, absolute, mode, floor_guard, policy_value, bypass);
            }
          }

          // The explicit overload and frozen HDR ChainActive must survive a later global mode change.
          runtime::codec_mode.store((mode + 1u) % 4u);
          Check(runtime::FrameDisplayCodec(target, target.hdr_codec_mode) == display,
                "explicit frame mode remains stable after the global mode changes",
                hdr_mode, absolute, mode, floor_guard);
          if (hdr_mode != 0) {
            target.pass_transfer_strength[0] = 0.f;
            Check(runtime::ChainActive(target, 1, &target) == display,
                  "frozen HDR ChainActive uses the captured curve after global mode changes",
                  hdr_mode, absolute, mode, floor_guard);
          }
        }
      }
    }
  }

  std::cout << "matrix_combinations=" << combinations << '\n'
            << "hdr_floor_curve_2_checks=" << hdr_floor_curve_checks << '\n'
            << checks << " checks, " << failures << " failures\n";
  return (failures == 0 ? 0 : 1);
}
