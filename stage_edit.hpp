/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Composed after preset/profile adapters; only the overlay thread owns previews.
namespace stage_edit {
using Json = presets::Json;
inline Json before = Json::object();
inline Json changes = Json::object();
inline std::vector<preset_ui::Difference> undo;
inline std::string error;
inline std::string notice;
inline bool pending = false;

inline bool Prepare(const Setting& setting, float value) {
  SettleSlider(true);
  before = profile_ui::Snapshot();
  changes = ProposedStageValues(setting, value);
  error.clear();
  pending = !changes.empty() && ValidateConfigurationCapability(changes, &error);
  return pending;
}

inline bool Confirm() {
  if (!pending) return false;
  SettleSlider(true);
  if (profile_ui::Snapshot() != before) {
    error = ui::Tr("Settings changed. Cancel and choose the stage again.");
    return false;
  }
  const presets::Document document{.metadata = Json::object(), .scope = presets::Scope::FULL, .settings = changes};
  auto differences = preset_ui::Preview(document, false);
  if (!preset_ui::Apply(document, false, false, &error, false, nullptr, false, &before)) return false;
  undo.clear();
  for (auto& difference : differences) {
    if (difference.apply) undo.push_back(std::move(difference));
  }
  pending = false;
  notice = "Execution chain updated.";
  return true;
}

inline void Draw() {
  if (stage_edit_request.setting != nullptr) {
    const auto request = stage_edit_request;
    stage_edit_request = {};
    if (Prepare(*request.setting, request.value)) ImGui::OpenPopup("###nr_stage_change");
  }
  const auto display = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos(ImVec2(display.x * .5f, display.y * .5f), ImGuiCond_Appearing, ImVec2(.5f, .5f));
  ImGui::SetNextWindowSize(ImVec2((std::min)(ImGui::GetFontSize() * 32.f, display.x - 24.f), 0.f), ImGuiCond_Appearing);
  bool open = true;
  if (ImGui::BeginPopupModal((std::string(ui::Tr("Confirm execution chain change")) + "###nr_stage_change").c_str(),
          &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::TextWrapped("%s", ui::Tr("Review the complete chain. Only the stages shown below will change."));
    auto after = before;
    after.update(changes);
    const auto plan_from = [](const Json& values) {
      stage_plan::Configuration config{.pass_count = values.at("NRPasses").get<uint32_t>(),
          .hook_point = values.at("NRHookPoint").get<uint32_t>()};
      for (uint32_t pass = 1; pass < kMaxNrPasses; ++pass) {
        config.stages[pass - 1] = values.at(kSetPassStage[pass - 1].key).get<uint32_t>();
      }
      return stage_plan::Build(config);
    };
    const auto old_plan = plan_from(before);
    const auto new_plan = plan_from(after);
    if (ImGui::BeginTable("##stage_preview", 3, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
      ImGui::TableSetupColumn(ui::Tr("Pass"));
      ImGui::TableSetupColumn(ui::Tr("Before"));
      ImGui::TableSetupColumn(ui::Tr("After"));
      ImGui::TableHeadersRow();
      for (uint32_t pass = 0; pass < new_plan.pass_count; ++pass) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("P%u", pass + 1);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(stage_plan::Name(old_plan.requested[pass]));
        ImGui::TableNextColumn();
        if (old_plan.requested[pass] != new_plan.requested[pass]) ImGui::PushStyleColor(ImGuiCol_Text, ui::CurrentTheme().accent);
        ImGui::TextUnformatted(stage_plan::Name(new_plan.requested[pass]));
        if (old_plan.requested[pass] != new_plan.requested[pass]) ImGui::PopStyleColor();
        if (pass != 0 && after.at(kSetPassStage[pass - 1].key) == 0u) {
          ImGui::TextDisabled("%s", ui::Tr("Inherited"));
        }
      }
      ImGui::EndTable();
    }
    std::string limitation;
    const bool eligible = ValidateConfigurationCapability(changes, &limitation);
    if (!eligible) ImGui::TextWrapped("%s", limitation.c_str());
    if (!error.empty()) ImGui::TextWrapped("%s", ui::Tr(error.c_str()));
    if (ui::ActionButton(ui::Tr("Cancel"), "cancel_stage_change")
        || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      pending = false;
      error.clear();
      ImGui::CloseCurrentPopup();
    }
    ui::SameLineIfFits(ImGui::CalcTextSize(ui::Tr("Confirm change")).x + ImGui::GetStyle().FramePadding.x * 2.f);
    ImGui::BeginDisabled(!eligible);
    if (ui::ActionButton(ui::Tr("Confirm change"), "confirm_stage_change") && Confirm()) ImGui::CloseCurrentPopup();
    ImGui::EndDisabled();
    ImGui::EndPopup();
  }
  if (!open) {
    pending = false;
    error.clear();
  }
}
}  // namespace stage_edit
