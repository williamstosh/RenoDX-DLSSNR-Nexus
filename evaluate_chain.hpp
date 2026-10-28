/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// One NR injection per real DLSS evaluate, decided from the CALL, not from
// the present clock (hardening plan step 3, v6.5.2).
//
// A single game evaluate reaches the addon several times: NGX module copies
// forward into each other (plugin -> nvngx -> core, every copy hooked), the
// C export delegates to the C++ one, and Streamline's slEvaluateFeature runs
// the NGX evaluate inside itself.  All of those are NESTED calls on one
// thread, so the innermost hook that injects marks the output here and every
// enclosing hook sees the mark when the real call returns: a mirror.
//
// The per-present map this replaces asked "was this output processed since
// the last present event".  That is a different question: evaluates are
// recorded on worker threads while Present runs elsewhere, so the next
// frame's evaluate can arrive before the previous frame's present tick and
// be declined as a duplicate (a frame without NR), and a tick that lands
// between an inner and an outer hook clears the map and lets the outer hook
// inject a second time.  The map stays as DedupeMode 0.
//
// Dependency-free so test/dlss5 can include it.

#pragma once

#include <cstdint>

#include "declines.hpp"
#include "stage_plan.hpp"

namespace renodx::addons::dlss5 {

inline constexpr uint32_t kDedupePerPresent = 0;
inline constexpr uint32_t kDedupePerEvaluate = 1;

struct StageSourceState {
  static constexpr uint32_t kMaxOutputs = 8;
  const void* plan_handle = nullptr;
  stage_plan::Plan plan;
  uint64_t configuration_generation = 0;
  bool render_attempted = false;
  bool render_complete = false;
  bool present_predecessor_attempted = false;
  uint32_t game_wrappers = 0;
  // Mirrors share marks only within one real source call. Streamline may
  // enclose several calls that legitimately reuse the same output resource.
  uint32_t output_count = 0;
  const void* outputs[kMaxOutputs] = {};
  uint32_t output_stages[kMaxOutputs] = {};
};

struct EvaluateChain : StageSourceState {
  uint32_t depth = 0;
  // A hooked NGX evaluate inside this chain got past the structural gates
  // (a DLSS block with guides and usable geometry): the NGX path owns the
  // stream, whatever this frame's outcome.  The Streamline fallback only
  // engages when no nested NGX evaluate could.
  bool ngx_owned = false;
  // A hooked evaluate in this chain ran pre-FG (RunPresentPreFg) on a DLSS-G
  // evaluate: a nested one - a plugin copy delegating to the core, both
  // hooked - must not run it again on the stand-in the first put in the
  // block.  v8.5.0-rc1 Resonance: prefg_fallbacks equal to mirror (807/807).
  // Not a depth test: Streamline's evaluate hook encloses its NGX evaluates.
  bool prefg_ran = false;

  bool Injected(const void* output, uint32_t stage = 0) const noexcept {
    for (uint32_t i = 0; i < output_count; ++i) {
      if (outputs[i] == output && output_stages[i] == stage) return true;
    }
    return false;
  }
  // Past the capacity the mark is dropped: the enclosing hook then injects
  // again rather than silently skipping a pass it cannot prove ran.
  void MarkInjected(const void* output, uint32_t stage = 0) noexcept {
    if (depth == 0 || Injected(output, stage) || output_count == kMaxOutputs) return;
    output_stages[output_count] = stage;
    outputs[output_count++] = output;
  }
};

inline thread_local EvaluateChain evaluate_chain;

inline void FinishMixedStageTerminal(uint32_t game_wrappers) {
  if (!mixed_stage_terminal.active) return;
  const auto terminal = mixed_stage_terminal;
  mixed_stage_terminal = {};
  if (game_wrappers > 1) {
    nr_decline_counts[static_cast<size_t>(NrDeclineReason::kNestedMirror)].fetch_add(
        game_wrappers - 1, std::memory_order_relaxed);
  }
  if (terminal.injected && terminal.decline != NrDeclineReason::kGameEvaluateFailed) {
    mixed_stage_source_successes.fetch_add(1, std::memory_order_relaxed);
  } else if (terminal.decline != NrDeclineReason::kCount) {
    CountNrDecline(terminal.decline);
  }
}

struct StageSourceBinding {
  const void* handle = nullptr;
  bool active = false;
};
inline thread_local StageSourceBinding stage_source_binding;

// Streamline may enclose several distinct SR calls. Only nested wrappers of
// the same source share a plan/terminal; another source gets its own state.
struct SourceStageScope {
  StageSourceState previous_state;
  MixedStageTerminal previous_terminal;
  StageSourceBinding previous_binding;
  bool owner = false;

  SourceStageScope(const void* handle, bool counted) noexcept {
    if (!counted) return;
    if (stage_source_binding.active && stage_source_binding.handle == handle) {
      ++evaluate_chain.game_wrappers;
      return;
    }
    owner = true;
    previous_state = static_cast<const StageSourceState&>(evaluate_chain);
    previous_terminal = mixed_stage_terminal;
    previous_binding = stage_source_binding;
    static_cast<StageSourceState&>(evaluate_chain) = {.game_wrappers = 1};
    mixed_stage_terminal = {};
    stage_source_binding = {.handle = handle, .active = true};
  }
  ~SourceStageScope() noexcept {
    if (!owner) return;
    FinishMixedStageTerminal(evaluate_chain.game_wrappers);
    static_cast<StageSourceState&>(evaluate_chain) = previous_state;
    mixed_stage_terminal = previous_terminal;
    stage_source_binding = previous_binding;
  }
  SourceStageScope(const SourceStageScope&) = delete;
  SourceStageScope& operator=(const SourceStageScope&) = delete;
};

// Opened by every evaluate hook around its real call AND its own injection.
// The chain's marks live exactly as long as the outermost hook.
struct EvaluateChainScope {
  EvaluateChainScope() noexcept { ++evaluate_chain.depth; }
  ~EvaluateChainScope() noexcept {
    if (--evaluate_chain.depth != 0) return;
    FinishMixedStageTerminal(evaluate_chain.game_wrappers);
    evaluate_chain = EvaluateChain{};
  }
  EvaluateChainScope(const EvaluateChainScope&) = delete;
  EvaluateChainScope& operator=(const EvaluateChainScope&) = delete;
};

// The same nesting for the calls that carry no output to mark: a plugin
// copy's create forwarding into the core copy re-enters the detour on the
// same thread, and only the outermost entry may record the call.  Recorded
// twice, a D3D12 create found its own handle "still on record" from the
// inner entry, warned that a release had been missed and counted
// `over_live=1` in every multi-copy title (Code Vein II, KCD2, Forza
// Horizon 6: `_nvngx.dll` + `nvngx_dlss.dll`, both detoured).
struct NgxNesting {
  explicit NgxNesting(uint32_t* depth) noexcept
      : outermost((*depth)++ == 0), depth_(depth) {}
  ~NgxNesting() noexcept { --*depth_; }
  NgxNesting(const NgxNesting&) = delete;
  NgxNesting& operator=(const NgxNesting&) = delete;
  const bool outermost;

 private:
  uint32_t* depth_;
};

}  // namespace renodx::addons::dlss5
