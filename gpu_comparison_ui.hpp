/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Composed inside dlssnr.hpp after the live Setting registry.
namespace comparison_ui {
namespace comparison = gpu_comparison;

inline void CaptureSettings(comparison::Identity* identity) {
  std::shared_lock config_lock(present_restore_configuration.mutex);
  identity->configuration_generation = configuration_generation.load();
  identity->config_epoch = present_restore_configuration.epoch;
  identity->route = static_cast<uint64_t>(bridge_force_snippet.load())
      | (static_cast<uint64_t>(auto_fg_fallback_active.load()) << 1)
      | (static_cast<uint64_t>(present_prefg_active.load()) << 2)
      | (static_cast<uint64_t>(present_prefg_upscaled_active.load()) << 3);
  identity->fg = static_cast<uint64_t>(native_fg_declaration.load())
      | (static_cast<uint64_t>(native_fg_owns.load()) << 4)
      | (static_cast<uint64_t>(fg::xefg::OwnsStream()) << 5)
      | (static_cast<uint64_t>(fg::xefg::Enabled()) << 6)
      | (static_cast<uint64_t>(present_fg_multiplier.load()) << 8);
  identity->settings_hash = 14695981039346656037ull;
  identity->resolution_hash = 14695981039346656037ull;
  const auto append = [](uint64_t* hash, uint32_t value) {
    *hash ^= value;
    *hash *= 1099511628211ull;
  };
  for (const auto& section : kSettingSections) {
    for (const auto* setting : section) {
      const std::string_view key(setting->key);
      if (!key.starts_with("NR") && key != "NeuralUplift") continue;
      if (key.starts_with("NRScreenshot") || key == "NRToggleKey" || key == "NRGpuTimers"
          || key == "NRCostMeter" || key == "NREditTrace") continue;
      const bool resolution = key == "NRFollowInputRes" || key == "NRResolutionScale"
          || key.ends_with("ResolutionMode") || key.ends_with("ResolutionScale");
      auto* hash = (resolution ? &identity->resolution_hash : &identity->settings_hash);
      append(hash, std::bit_cast<uint32_t>(setting->Get()));
    }
  }
  // Compatibility settings outside the menu registry also affect processing.
  for (const uint32_t value : std::array<uint32_t, 6>{present_pre_fg.load(), present_pre_fg_contract.load(),
           present_mvec_units.load(), nr_output_rect.load(), dedupe_mode.load(), static_cast<uint32_t>(list_state_mode.load())}) {
    append(&identity->settings_hash, value);
  }
  for (const uint32_t value : std::array<uint32_t, 4>{present_block_hudless.load(), auto_fg_fallback.load(),
           experimental_present_chain.load(), experimental_native_prefg_resolution.load()}) {
    append(&identity->settings_hash, value);
  }
}

inline const char* ReasonText(comparison::Reason reason) {
  using enum comparison::Reason;
  switch (reason) {
    case NONE: return "Comparable GPU windows";
    case TIMEOUT: return "Timed out: not enough stable GPU samples";
    case IDENTITY_CHANGED: return "Capture rejected: configuration, source or history changed";
    case INCOMPATIBLE_IDENTITY: return "A/B conditions differ: model settings, route, FG, HDR, SR, device or source";
    case PASS_MISMATCH: return "A/B pass, model request or processing differs";
    case MULTIPLE_RESOLUTION_CHANGES: return "Change the working resolution of only one pass";
    case NO_RESOLUTION_CHANGE: return "No actual working-resolution change measured";
    case AMBIGUOUS_RESOLUTION: return "Resolution axes changed in opposite directions";
    case HIGH_VARIATION: return "High variation: repeat the capture in a steadier scene";
    case INSUFFICIENT_DIFFERENCE: return "Difference too small for a resolution suggestion";
    case ZERO_BASELINE: return "Timing too small for a relative comparison";
    default: return "Capture A, change one pass resolution, then capture B";
  }
}

inline void Draw() {
  RuntimeLock lock(runtime_mutex);
  auto& engine = gpu_timers::comparison;
  const bool active = gpu_timers::ComparisonActive();
  const auto plan = RequestedStagePlan();
  uint32_t mask = 0;
  for (uint32_t group = 0; group < plan.group_count; ++group) {
    mask |= 1u << static_cast<uint32_t>(plan.groups[group].stage);
  }
  ImGui::BeginChild("gpu_comparison", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 24.f), true);
  ImGui::TextUnformatted(ui::Tr("Manual GPU comparison"));
  ImGui::BeginDisabled(active || !plan.valid || vulkan_present_seen.load() || stage_source_rr.load());
  const auto start = [&](bool baseline) {
    gpu_timers::comparison_settings = CaptureSettings;
    if (engine.Start(baseline, mask, SteadyNowNs(), gpu_timers::internal::next_sequence + 1)) {
      CaptureSettings(&gpu_timers::comparison_capture_settings);
      gpu_timers::comparison_result = {};
    }
  };
  if (ImGui::Button(ui::Tr("Capture baseline"))) start(true);
  ImGui::SameLine();
  bool baseline_ready = mask != 0 && engine.baseline_mask == mask;
  for (uint32_t stage = 0; stage < 3; ++stage) {
    if ((mask & (1u << stage)) != 0) baseline_ready &= engine.baseline[stage].status == comparison::Status::COMPLETE;
  }
  ImGui::BeginDisabled(!baseline_ready);
  if (ImGui::Button(ui::Tr("Capture comparison"))) start(false);
  ImGui::EndDisabled();
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button(ui::Tr("Clear comparison"))) {
    engine.Reset();
    gpu_timers::comparison_result = {};
  }
  ImGui::TextWrapped("%s", ui::Tr("16 samples per StageGroup; 250 ms cadence, 1 s settling, 10 s limit. Keep the same scene."));
  auto reason = gpu_timers::comparison_result.reason;
  for (const auto* windows : {&engine.baseline, &engine.control}) {
    for (const auto& window : *windows) {
      if (window.status == comparison::Status::FAILED) reason = window.reason;
    }
  }
  ImGui::TextWrapped("%s", ui::Tr(active ? "Collecting stable, GPU-complete samples..." : ReasonText(reason)));
  const bool comparable = gpu_timers::comparison_result.reason == comparison::Reason::NONE;
  if (ImGui::BeginTable("gpu_ab_results", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH)) {
    ImGui::TableSetupColumn(ui::Tr("GPU metric"), 0, 1.4f);
    ImGui::TableSetupColumn(ui::Tr("Baseline A"));
    ImGui::TableSetupColumn(ui::Tr("Comparison B"));
    ImGui::TableSetupColumn(ui::Tr("B - A"));
    ImGui::TableHeadersRow();
    const auto metric_cells = [&](const comparison::MetricComparison* metric,
                                  const comparison::Pass* const* dimensions = nullptr) {
      for (uint32_t side = 0; side < 2; ++side) {
        ImGui::TableNextColumn();
        if (metric != nullptr) {
          const auto& statistics = (side == 0 ? metric->a : metric->b);
          ImGui::Text("%.3f ms", statistics.median_us / 1000.0);
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("MAD %.3f ms (%.1f%%)", statistics.mad_us / 1000.0, statistics.relative_mad * 100);
          }
        } else ImGui::TextUnformatted("--");
        if (dimensions != nullptr) {
          if (dimensions[side] != nullptr) ImGui::Text("%ux%u", dimensions[side]->width, dimensions[side]->height);
          else ImGui::TextUnformatted("--");
        }
      }
      ImGui::TableNextColumn();
      if (metric != nullptr && metric->percent_valid) {
        ImGui::Text("%+.3f ms\n%+.1f%%", metric->delta_us / 1000.0, metric->percent);
      } else ImGui::TextUnformatted("--\n--");
    };
    for (uint32_t id = 0; id < comparison::MAX_PASSES; ++id) {
      const comparison::Pass* passes[2] = {};
      const comparison::MetricComparison* metric = nullptr;
      uint32_t actual_stage = 0;
      for (uint32_t stage = 0; stage < 3; ++stage) {
        for (uint32_t side = 0; side < 2; ++side) {
          const auto& window = (side == 0 ? engine.baseline[stage] : engine.control[stage]);
          for (uint32_t pass = 0; pass < window.pass_count; ++pass) {
            if (window.passes[pass].id != id) continue;
            passes[side] = &window.passes[pass];
            actual_stage = stage;
            if (comparable) metric = &gpu_timers::comparison_result.stages[stage].passes[pass];
          }
        }
      }
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("Pass %u / %s", id + 1, passes[0] != nullptr ? stage_plan::Name(static_cast<stage_plan::Stage>(actual_stage)) : "--");
      ImGui::Text("%s: %s", ui::Tr("Request"), passes[0] != nullptr ? nr_model::PresetName(passes[0]->model) : "--");
      metric_cells(metric, passes);
    }
    for (uint32_t stage = 0; stage < 3; ++stage) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("%s / StageGroup", stage_plan::Name(static_cast<stage_plan::Stage>(stage)));
      ImGui::Text("A %u/16 | B %u/16", static_cast<unsigned>(engine.baseline[stage].count), static_cast<unsigned>(engine.control[stage].count));
      metric_cells(comparable && gpu_timers::comparison_result.stages[stage].selected
          ? &gpu_timers::comparison_result.stages[stage].total : nullptr);
    }
    ImGui::EndTable();
  }
  bool advice = false;
  comparison::Reason metric_reason = comparison::Reason::NONE;
  double variation_a = 0, variation_b = 0;
  for (const auto& stage : gpu_timers::comparison_result.stages) {
    if (!stage.selected) continue;
    for (uint32_t pass = 0; pass <= stage.pass_count; ++pass) {
      const auto& metric = (pass == stage.pass_count ? stage.total : stage.passes[pass]);
      variation_a = std::max(variation_a, metric.a.relative_mad);
      variation_b = std::max(variation_b, metric.b.relative_mad);
      if (pass < stage.pass_count) advice |= metric.advice != comparison::Advice::NONE;
      if (metric.reason == comparison::Reason::HIGH_VARIATION) metric_reason = metric.reason;
      if (metric.reason == comparison::Reason::ZERO_BASELINE && metric_reason != comparison::Reason::HIGH_VARIATION) {
        metric_reason = metric.reason;
      }
    }
  }
  ImGui::Text(ui::Tr("Median; max MAD / median: A %.1f%% | B %.1f%%"), variation_a * 100, variation_b * 100);
  ImGui::TextWrapped("%s", ui::Tr(advice && metric_reason == comparison::Reason::NONE
      ? "The lower-resolution pass measured less NGX GPU time. Check whether its image quality is acceptable."
      : ReasonText(comparable ? (metric_reason == comparison::Reason::NONE ? comparison::Reason::INSUFFICIENT_DIFFERENCE : metric_reason) : reason)));
  ImGui::TextWrapped("%s", ui::Tr("Scene changes are unverified; model requests do not confirm the internal network. No FPS or image-quality guarantee. StageGroups are separate; Look is inside Resolve."));
  ImGui::EndChild();
}
}  // namespace comparison_ui
