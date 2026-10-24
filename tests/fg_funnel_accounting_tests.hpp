#pragma once

#include <functional>

namespace funnel_accounting_tests {
namespace nr = renodx::addons::dlss5::internal;
namespace dlss5 = renodx::addons::dlss5;
static NVSDK_NGX_Result result = NVSDK_NGX_Result_Success;
static bool delegate = false;

NVSDK_NGX_Result NVSDK_CONV Evaluate(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
    const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback) {
  return result;
}
NVSDK_NGX_Result NVSDK_CONV EvaluateC(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback_C) {
  if (delegate) return nr::HookedEvaluateFeatureSlot<0>(list, handle, parameters, nullptr);
  return result;
}

// Restore test-owned substitutions and observations before the native FG matrix.
struct Restore {
  std::vector<std::function<void()>> actions;
  template <typename T> void Atomic(std::atomic<T>* value) {
    const T saved = value->load();
    actions.emplace_back([value, saved] { value->store(saved); });
  }
  template <typename T> void Value(T* value) {
    const T saved = *value;
    actions.emplace_back([value, saved] { *value = saved; });
  }
  ~Restore() {
    for (auto it = actions.rbegin(); it != actions.rend(); ++it) (*it)();
  }
};

void Run(Gpu* gpu) {
  Restore restore;
  restore.Value(&result);
  restore.Value(&delegate);
  restore.Value(&nr::ngx_slot_real[0]);
  restore.Value(&nr::created_feature_ids);
  restore.Atomic(&nr::logged_skip_ids);
  restore.Value(&renodx::addons::dlss5::evaluate_chain);
  restore.Value(&nr::inside_game_evaluate_wrapper);
  restore.Value(&nr::telemetry_window_nr_calls);
  restore.Value(&nr::telemetry_window_nr_ns);
  restore.Value(&nr::telemetry_window_nr_max_ns);
  restore.Value(&captured_logs);
  restore.Atomic(&nr::ngx_loader_published);
  restore.Atomic(&nr::enabled);
  restore.Atomic(&nr::hook_point);
  restore.Atomic(&nr::d3d12_device_seen);
  restore.Atomic(&nr::cmd_hooks_ever_installed);
  restore.Atomic(&nr::ngx_ever_hooked);
  restore.Atomic(&nr::last_present_steady_ns);
  restore.Atomic(&nr::lifecycle_tick_owed_ns);
  restore.Atomic(&nr::evaluates_since_present);
  restore.Atomic(&nr::intercepted_evaluations);
  restore.Atomic(&nr::own_evaluations);
  restore.Atomic(&nr::evaluations_in_flight);
  restore.Atomic(&nr::successful_evaluations);
  restore.Atomic(&nr::successful_pre_sr_evaluations);
  restore.Atomic(&nr::present_frames_seen);
  restore.Atomic(&nr::present_prefg_attempts);
  restore.Atomic(&nr::native_fg_owns);
  restore.Atomic(&nr::ngx_entry_evaluate[0]);
  restore.Atomic(&nr::ngx_entry_evaluate_c[0]);
  restore.Atomic(&nr::logged_first_evaluate);
  restore.Atomic(&nr::ngx_dlss_evaluate_seen);
  restore.Atomic(&nr::gate_ever_opened);
  restore.Atomic(&nr::logged_state_target_armed);
  restore.Atomic(&nr::motion_checked_evaluates);
  restore.Atomic(&nr::motion_missing_evaluates);
  for (auto& count : dlss5::nr_decline_counts) restore.Atomic(&count);
  nr::NgxLoaderSymbols loader;
  loader.nr_ready = true;
  loader.core_ready = true;
  nr::ngx_loader_published.store(&loader);
  nr::last_present_steady_ns.store(nr::SteadyNowNs());
  nr::lifecycle_tick_owed_ns.store(0);
  nr::d3d12_device_seen.store(true);
  nr::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(Evaluate);
  nr::ngx_slot_real[0].evaluate_c = reinterpret_cast<void*>(EvaluateC);
  uint32_t sr_key = 0, fg_key = 0;
  const auto* sr = reinterpret_cast<const NVSDK_NGX_Handle*>(&sr_key);
  const auto* fg = reinterpret_cast<const NVSDK_NGX_Handle*>(&fg_key);
  nr::created_feature_ids[sr] = nr::kFeatureDlss;
  nr::created_feature_ids[fg] = NVSDK_NGX_Feature_FrameGeneration;
  renodx::addons::dlss5::bridge::OwnedParameters parameters;
  auto color = gpu->Texture();
  parameters.Set(NVSDK_NGX_Parameter_Color, color.Get());
  parameters.Set(NVSDK_NGX_Parameter_Output, color.Get());
  parameters.Set(NVSDK_NGX_Parameter_MotionVectors, color.Get());
  parameters.Set(NVSDK_NGX_Parameter_Depth, color.Get());
  unsigned failures = 0;
  const auto check = [&](bool ok, const char* message) {
    std::cout << "FUNNEL " << (ok ? "PASS " : "FAIL ") << message << '\n';
    if (!ok) ++failures;
  };
  for (const auto hook : {nr::kHookUpscaled, nr::kHookRender}) {
    nr::hook_point.store(hook);
    for (int entry = 0; entry < 3; ++entry) {
      delegate = entry == 2;
      std::cout << "FUNNEL hook=" << hook << " entry=" << entry << '\n';
      const auto call = [&](const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* block) {
        if (entry == 0) return nr::HookedEvaluateFeatureSlot<0>(gpu->list.Get(), handle, block, nullptr);
        return nr::HookedEvaluateFeatureCSlot<0>(gpu->list.Get(), handle, block, nullptr);
      };
      const auto excluded = [&](const char* name, const NVSDK_NGX_Handle* handle,
                                const NVSDK_NGX_Parameter* block, dlss5::NrDeclineReason reason, bool own) {
        const auto before = nr::CollectFunnelInputs();
        const auto count = dlss5::nr_decline_counts[static_cast<size_t>(reason)].load();
        const auto own_count = nr::own_evaluations.load();
        NVSDK_NGX_Result returned;
        if (own) {
          dlss5::DirectCallScope scope;
          returned = call(handle, block);
        } else {
          returned = call(handle, block);
        }
        const auto after = nr::CollectFunnelInputs();
        check(returned == result && after.seen == before.seen + 1
                  && after.eligible == before.eligible && after.injected == before.injected
                  && after.unaccounted == before.unaccounted
                  && nr::own_evaluations.load() == own_count + (own ? 1 : 0)
                  && dlss5::nr_decline_counts[static_cast<size_t>(reason)].load() == count + (own ? 0 : 1), name);
      };
      nr::enabled.store(true);
      result = NVSDK_NGX_Result_Success;
      excluded("FG excluded exactly once", fg, &parameters, dlss5::NrDeclineReason::kNgxNotDlssEvaluation, false);
      excluded("own excluded exactly once", fg, &parameters, dlss5::NrDeclineReason::kNgxNotDlssEvaluation, true);
      // A known FG handle avoids any pre-SR injection before the real failure/off terminal.
      result = NVSDK_NGX_Result_FAIL_InvalidParameter;
      excluded("game_failed excluded", fg, &parameters, dlss5::NrDeclineReason::kGameEvaluateFailed, false);
      result = NVSDK_NGX_Result_Success;
      nr::enabled.store(false);
      excluded("nr_off excluded", fg, &parameters, dlss5::NrDeclineReason::kNrDisabledEvaluation, false);
      nr::enabled.store(true);
      excluded("null handle malformed excluded", nullptr, &parameters, dlss5::NrDeclineReason::kMalformedEvaluate, false);
      excluded("null parameters malformed excluded", sr, nullptr, dlss5::NrDeclineReason::kMalformedEvaluate, false);
      if (hook != nr::kHookRender) continue;
      for (const char* missing : {NVSDK_NGX_Parameter_Depth, NVSDK_NGX_Parameter_MotionVectors}) {
        parameters.Set(missing, static_cast<ID3D12Resource*>(nullptr));
        const auto before = nr::CollectFunnelInputs();
        const auto count = dlss5::nr_decline_counts[static_cast<size_t>(dlss5::NrDeclineReason::kNgxMissingGuides)].load();
        call(sr, &parameters);
        const auto after = nr::CollectFunnelInputs();
        check(after.eligible == before.eligible + 1 && after.injected == before.injected
                  && after.unaccounted == before.unaccounted
                  && dlss5::nr_decline_counts[static_cast<size_t>(dlss5::NrDeclineReason::kNgxMissingGuides)].load() == count + 1,
              "missing guide remains eligible with one reason");
        parameters.Set(missing, color.Get());
      }
    }
  }
  for (const auto hook : {nr::kHookUpscaled, nr::kHookRender}) {
    for (auto& count : dlss5::nr_decline_counts) count.store(0);
    nr::intercepted_evaluations.store(400);
    nr::own_evaluations.store(0);
    nr::evaluations_in_flight.store(0);
    nr::successful_evaluations.store(400);
    nr::successful_pre_sr_evaluations.store(0);
    nr::present_frames_seen.store(0);
    nr::present_prefg_attempts.store(0);
    nr::native_fg_owns.store(false);
    nr::hook_point.store(hook);
    const auto card = [&] {
      auto in = nr::CollectFunnelInputs();
      in.hooks_enabled = in.nr_enabled = in.runtime_available = true;
      in.ngx_hooked = in.list_hooks_live = in.dlss_feature_seen = true;
      in.nr_feature_ready = in.gate_ever_opened = true;
      return dlss5::ui::ComputeStatusCard(
          {.verdict = dlss5::ComputeNrVerdict(in), .active_features = 1,
              .pre_sr = hook == nr::kHookRender});
    };
    check(card().id == dlss5::ui::CardId::kActive, "inline mode initially active");
    nr::hook_point.store(nr::kHookPresent);
    nr::native_fg_owns.store(true);
    bool yielded = true;
    for (uint32_t present = 0; present < 500; ++present) {
      nr::present_frames_seen.fetch_add(1);
      yielded &= nr::YieldPresentToNativeFg();
    }
    check(yielded && dlss5::nr_decline_counts[
              static_cast<size_t>(dlss5::NrDeclineReason::kPresentPreFg)].load() == 500,
          "native FG normal Present handoff names one ineligible terminal");
    nr::hook_point.store(hook);
    nr::intercepted_evaluations.fetch_add(100);
    nr::successful_evaluations.fetch_add(100);
    const auto after = nr::CollectFunnelInputs();
    check(!nr::YieldPresentToNativeFg() && after.seen == 1000 && after.eligible == 500
              && after.injected == 500 && after.unaccounted == 0
              && card().id == dlss5::ui::CardId::kActive,
          "inline to native Present and back remains active without resetting history");
    for (uint32_t skipped = 0; skipped < 50; ++skipped) {
      nr::intercepted_evaluations.fetch_add(1);
      dlss5::CountNrDecline(dlss5::NrDeclineReason::kNgxMissingGuides);
    }
    check(card().id == dlss5::ui::CardId::kPartlyActive,
          "real eligible skips still report partial activity after mode switching");
  }
  Check(failures == 0, "funnel wrapper accounting contracts");
}
}  // namespace funnel_accounting_tests
