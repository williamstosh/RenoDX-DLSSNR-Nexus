/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Composed after preset/profile adapters; only the overlay thread owns previews.
namespace resolution_edit {
using Json = presets::Json;
inline bool pending = false;
inline bool confirmation_required = false;
inline Json before = Json::object();
inline Json changes = Json::object();
inline std::string error;
inline std::vector<preset_ui::Difference> undo;
inline std::vector<preset_ui::Difference> redo;
inline std::string conflict_reason;

inline void Cancel() {
  pending = false;
  confirmation_required = false;
  before = Json::object();
  changes = Json::object();
  error.clear();
  conflict_reason.clear();
}

inline bool Prepare(const Json& requested) {
  Cancel();
  if (!requested.is_object() || requested.empty()) return false;
  for (const auto& [key, value] : requested.items()) {
    const bool allowed = key == "NRPasses" || key == "NRFollowInputRes" || key == "NRResolutionScale"
        || key == "NRPass2ResolutionMode" || key == "NRPass2ResolutionScale"
        || key == "NRPass3ResolutionMode" || key == "NRPass3ResolutionScale"
        || key == "NRPass4ResolutionMode" || key == "NRPass4ResolutionScale";
    const auto* rule = presets::FindRule(key);
    if (!allowed || rule == nullptr || !preset_ui::ValidateValue(*rule, value)) {
      error = ui::Tr("Invalid value");
      return false;
    }
  }
  SettleSlider(true);
  before = profile_ui::Snapshot();
  auto original = before;
  original.update(requested);
  if (ValidateConfigurationCapability(original, nullptr)) {
    changes = requested;
    pending = true;
    return true;
  }
  changes = ProposedResolutionInheritance(requested);
  if (changes.empty()) {
    before = Json::object();
    return false;
  }
  auto candidate = before;
  candidate.update(changes);
  if (!ValidateConfigurationCapability(candidate, &error)) {
    before = Json::object();
    changes = Json::object();
    return false;
  }
  conflict_reason = FormatConfigurationCapabilityError(original, QueryConfigurationCapability(original));
  confirmation_required = true;
  pending = true;
  return true;
}

inline bool Confirm() {
  if (!pending) return false;
  error.clear();
  if (profile_ui::Snapshot() != before) {
    error = ui::Tr("Settings changed. Cancel and choose the stage again.");
    return false;
  }
  auto candidate = before;
  candidate.update(changes);
  const presets::Document document{.metadata = Json::object(), .scope = presets::Scope::FULL, .settings = candidate};
  auto differences = preset_ui::Preview(document, false);
  if (!preset_ui::Apply(document, false, false, &error, false, nullptr, false, &before)) return false;
  undo.clear();
  for (auto& difference : differences) {
    if (difference.apply) undo.push_back(std::move(difference));
  }
  redo.clear();
  Cancel();
  return true;
}

inline bool Restore(std::vector<preset_ui::Difference>* source, std::vector<preset_ui::Difference>* destination) {
  if (pending || source->empty()) return false;
  error.clear();
  std::vector<preset_ui::Difference> reverse;
  for (const auto& difference : *source) {
    // Match Undo's later-edit filter so Redo owns only this restored batch.
    if (preset_ui::ReadSetting(difference.key) != difference.after) continue;
    auto reversed = difference;
    std::swap(reversed.before, reversed.after);
    reverse.push_back(std::move(reversed));
  }
  if (!preset_ui::Undo(&error, source)) return false;
  *destination = std::move(reverse);
  return true;
}

inline bool Undo() {
  return Restore(&undo, &redo);
}

inline bool Redo() {
  return Restore(&redo, &undo);
}

inline void Draw() {
  if (!resolution_edit_request.empty()) {
    const auto requested = std::move(resolution_edit_request);
    resolution_edit_request = Json::object();
    if (Prepare(requested)) {
      if (confirmation_required) {
        ImGui::OpenPopup("###nr_resolution_change");
      } else if (!Confirm()) {
        pending = false;
        before = Json::object();
        changes = Json::object();
      }
    }
  }
  if (pending && confirmation_required) {
    const auto display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x * .5f, display.y * .5f), ImGuiCond_Appearing, ImVec2(.5f, .5f));
    const float width = (std::max)(1.f, (std::min)(ImGui::GetFontSize() * 34.f, display.x - 24.f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.f), ImVec2(width, display.y - 24.f));
    bool open = true;
    if (ImGui::BeginPopupModal((std::string(ui::Tr("Use matching working resolutions?")) + "###nr_resolution_change").c_str(),
            &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
      ImGui::TextWrapped("%s", conflict_reason.c_str());
      auto candidate = before;
      candidate.update(changes);
      std::string limitation;
      const bool eligible = ValidateConfigurationCapability(candidate, &limitation);
      if (!eligible) {
        ImGui::TextWrapped("%s", limitation.c_str());
      }
      if (!error.empty()) ImGui::TextWrapped("%s", ui::Tr(error.c_str()));
      if (ui::ActionButton(ui::Tr("Cancel"), "cancel_resolution_change")
          || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        Cancel();
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
      }
      ui::SameLineIfFits(ImGui::CalcTextSize(ui::Tr("Let other passes inherit base resolution")).x
          + ImGui::GetStyle().FramePadding.x * 2.f);
      ImGui::BeginDisabled(!eligible);
      if (ui::ActionButton(ui::Tr("Let other passes inherit base resolution"), "confirm_resolution_change") && Confirm()) {
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndDisabled();
      ImGui::EndPopup();
    }
    if (!open) Cancel();
  }
}
}  // namespace resolution_edit
