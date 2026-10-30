/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Composed inside main: calls the production adapters with synthetic runtime
// observations and the disk-backed ReShade fixture. This is not GPU execution.
namespace cap = renodx::addons::dlss5::capability;
namespace plan = renodx::addons::dlss5::stage_plan;
namespace presets = renodx::addons::dlss5::presets;
namespace adapter = runtime::preset_ui;
namespace profile = runtime::profile_ui;
using Json = presets::Json;
std::string error;
renodx::addons::dlss5::gpu_timers::comparison_settings = runtime::comparison_ui::CaptureSettings;
std::cout << std::unitbuf;
std::cerr << std::unitbuf;
std::cout << "capability-ui: synthetic production adapter checks; no GPU execution\n";
runtime::d3d12_swapchain_seen = true;
runtime::direct_runtime_requirements.dual_models = true;
runtime::enabled = true;
runtime::native_fg_declaration = runtime::NativeFgDeclaration::kOff;
runtime::native_fg_public_off = true;
for (auto& observed : runtime::stage_telemetry) {
  observed.width = 1920;
  observed.height = 1080;
}
const Json pair{{"NRPasses", 2u}, {"NRHookPoint", 0u}, {"NRPreset", 1u},
                {"NRFollowInputRes", false}, {"NRResolutionScale", 1.f},
                {"NRPass2Stage", 0u}, {"NRPass2Preset", 1u},
                {"NRPass2ResolutionMode", 2u}, {"NRPass2ResolutionScale", .67f},
                {"NRPass3Stage", 0u}, {"NRPass3ResolutionMode", 0u},
                {"NRPass4Stage", 0u}, {"NRPass4ResolutionMode", 0u}};
const auto apply = [&](const Json& settings) {
  error.clear();
  return adapter::Apply({.scope = presets::Scope::FULL, .settings = settings}, true, false, &error);
};
const auto reject_unchanged = [&](const Json& settings) {
  const auto snapshot = profile::Snapshot();
  const auto bytes = Bytes();
  const auto write_count = writes.size();
  const auto generation = runtime::configuration_generation.load();
  const auto history = runtime::history_reset_epoch.load();
  Check(!apply(settings) && !error.empty(), "Ineligible preset is rejected with a reason.");
  Check(profile::Snapshot() == snapshot && Bytes() == bytes && writes.size() == write_count
            && runtime::configuration_generation.load() == generation && runtime::history_reset_epoch.load() == history,
        "Rejected preset preserves INI, current settings, generation and history without writes.");
  error.clear();
  Check(!profile::ApplySnapshot(settings, &error) && !error.empty(), "Profile application uses the production capability gate.");
  Check(profile::Snapshot() == snapshot && Bytes() == bytes && writes.size() == write_count,
        "Rejected profile preserves INI and current settings without writes.");
};

#include "stage_edit_runtime_cases.hpp"
#include "stage_gpu_runtime_cases.hpp"

// A stale native FG owner must follow the existing planner's route boundary:
// only experimental Present chains treat it as an owner of a mixed NR chain.
runtime::native_fg_owns = true;
auto owner_request = pair;
owner_request["NRHookPoint"] = 1u;
owner_request["NRPass2Stage"] = 2u;
runtime::hook_point = 1u;
runtime::stack_passes = 2u;
runtime::pass_stage[0] = 2u;
const auto existing_plan = runtime::RequestedStagePlan();
const auto owner_result = runtime::QueryConfigurationCapability(owner_request);
std::cout << "native-owner/nonexperimental: planner_valid=" << existing_plan.valid
          << " planner_reason=" << static_cast<unsigned>(existing_plan.reason)
          << " adapter_submit=" << owner_result.CanSubmit()
          << " adapter_reason=" << static_cast<unsigned>(owner_result.configuration.reason) << '\n';
Check(existing_plan.valid && owner_result.CanSubmit(),
      "Nonexperimental Render to Upscaled retains the existing FG-compatible route with native owner state.");
runtime::native_fg_owns = false;
runtime::pass_stage[0] = 0u;
runtime::stack_passes = 1u;
runtime::hook_point = 0u;

for (const bool bridge : {false, true}) {
  runtime::d3d12_swapchain_seen = !bridge;
  runtime::d3d11_swapchain_seen = bridge;
  runtime::dx11_route = runtime::kDx11RouteNative;
  for (const unsigned hook : {1u, 0u}) {
    auto request = pair;
    request["NRHookPoint"] = hook;
    const auto result = runtime::QueryConfigurationCapability(request);
    Check(result.CanSubmit() && !result.execution_proven && result.group_count == 1
              && result.working_sizes[0] == cap::Size{1920, 1080}
              && result.working_sizes[1] == cap::Size{1286, 724},
          "Native DX12 / DX11 bridge Render / Upscaled pair retains supported unequal sizes, without execution proof.");
    std::cout << "capability-ui: applying supported pair bridge=" << bridge << " hook=" << hook << '\n';
    Check(apply(request), "Production preset applies a supported same-stage unequal-size pair.");
    Check(runtime::CanChooseSetting(runtime::kSetPassResolutionScale[0], .75f, &error),
          "UI allows changing the supported pair's scale.");
    for (const unsigned count : {3u, 4u}) {
      auto incompatible = request;
      incompatible["NRPasses"] = count;
      const auto rejected = runtime::QueryConfigurationCapability(incompatible);
      Check(!rejected.CanSubmit() && rejected.configuration.reason == cap::Reason::GROUP_SIZE_MISMATCH,
            "Three / four passes in one group reject unequal actual sizes.");
      Check(!runtime::CanChooseSetting(runtime::kSetStackPasses, static_cast<float>(count), &error),
            "UI rejects expanding an unequal-size pair to three / four same-stage passes.");
      reject_unchanged(incompatible);
    }
  }
  for (const unsigned count : {2u, 4u}) {
    auto request = pair;
    request["NRHookPoint"] = 1u;
    request["NRPasses"] = count;
    request["NRPass2Stage"] = (count == 2 ? 2u : 0u);
    request["NRPass3Stage"] = 2u;
    request["NRPass4ResolutionMode"] = 2u;
    request["NRPass4ResolutionScale"] = .5f;
    const auto result = runtime::QueryConfigurationCapability(request);
    Check(result.CanSubmit() && result.group_count == 2 && !result.execution_proven,
          "Native DX12 / DX11 bridge Render to Upscaled 1+1 / 2+2 remains eligible.");
    Check(apply(request), "Preset application preserves supported Render to Upscaled chains.");
  }
}

runtime::d3d12_swapchain_seen = true;
runtime::d3d11_swapchain_seen = false;
auto present = pair;
present["NRHookPoint"] = 2u;
Check(apply(present), "Ordinary native DX12 Present unequal-size pair is eligible with explicit FG Off.");
for (const auto declaration : {runtime::NativeFgDeclaration::kOn, runtime::NativeFgDeclaration::kUnknown}) {
  runtime::native_fg_declaration = declaration;
  runtime::native_fg_public_off = false;
  const auto result = runtime::QueryConfigurationCapability(present);
  Check(!result.CanSubmit() && result.configuration.reason == cap::Reason::FG_OFF_REQUIRED,
        "Ordinary Present pair requires explicit FG Off; On / Unknown cannot submit.");
  auto change = present;
  change["NRPass2ResolutionScale"] = .5f;
  reject_unchanged(change);
}
runtime::native_fg_declaration = runtime::NativeFgDeclaration::kOff;
runtime::native_fg_public_off = true;
auto change = present;
change["NRPass2ResolutionScale"] = .5f;
Check(!adapter::Preview({.scope = presets::Scope::FULL, .settings = change}, true).empty(),
      "Eligible Present change can be previewed.");
runtime::native_fg_declaration = runtime::NativeFgDeclaration::kOn;
reject_unchanged(change);
runtime::native_fg_declaration = runtime::NativeFgDeclaration::kOff;
Check(apply(change), "Same request is revalidated and accepted after FG returns to Off.");

runtime::experimental_present_chain = true;
auto b1 = pair;
b1["NRPass2Stage"] = 3u;
const auto b1_result = runtime::QueryConfigurationCapability(b1);
Check(b1_result.configuration.status == cap::Status::NEEDS_PROOF
          && b1_result.configuration.reason == cap::Reason::B1_FRAME_PROOF_REQUIRED && !b1_result.execution_proven,
      "B1 configuration cannot fabricate current-frame execution proof.");
reject_unchanged(b1);
runtime::experimental_present_chain = false;

runtime::stage_source_rr = true;
auto rr = pair;
rr["NRHookPoint"] = 1u;
rr["NRPass2Stage"] = 2u;
const auto rr_result = runtime::QueryConfigurationCapability(rr);
Check(rr_result.plan_reason == plan::Reason::RR_AFTER && rr_result.group_count == 1,
      "RR retains requested Render but reports effective Upscaled grouping.");
reject_unchanged(rr);
runtime::stage_source_rr = false;

runtime::d3d12_swapchain_seen = false;
runtime::vulkan_present_seen = true;
auto vk = pair;
const auto vk_result = runtime::QueryConfigurationCapability(vk);
Check(!vk_result.CanSubmit() && vk_result.configuration.reason == cap::Reason::INDEPENDENT_MODE_UNSUPPORTED,
      "Vulkan rejects explicit independent resolution modes.");
reject_unchanged(vk);
vk["NRPass2ResolutionMode"] = 0u;
vk["NRResolutionScale"] = 1.f;
vk["NRFollowInputRes"] = false;
Check(runtime::QueryConfigurationCapability(vk).CanSubmit(), "Vulkan inherited same-stage sizes remain eligible.");
auto vk_scaled = vk;
vk_scaled["NRResolutionScale"] = .67f;
Check(!runtime::QueryConfigurationCapability(vk_scaled).CanSubmit(), "Vulkan cannot accept a base working size the executor does not consume.");
reject_unchanged(vk_scaled);
const Json unavailable_present{{"NRHookPoint", 2u}, {"NRIntensity", .375f}};
const auto unavailable_rows = adapter::Preview({.scope = presets::Scope::FULL, .settings = unavailable_present}, true);
Check(std::any_of(unavailable_rows.begin(), unavailable_rows.end(), [](const auto& row) {
        return row.key == "NRHookPoint" && std::string_view(row.status) == "Unsupported";
      }), "Vulkan explicit Present request is visibly unsupported in production preview.");
reject_unchanged(unavailable_present);
runtime::vulkan_present_seen = false;

// Loading and inspecting unsupported old saved requests must not normalize them.
Check(WritePrivateProfileStringA("RenoDX.DLSS5", "NRHookPoint", "2", ini_path.c_str()) != 0,
      "Seed unsupported saved Present request.");
Check(WritePrivateProfileStringA("RenoDX.DLSS5", "NRPass2ResolutionScale", "0.67", ini_path.c_str()) != 0,
      "Seed saved independent scale.");
runtime::LoadConfiguration();
const auto saved_snapshot = profile::Snapshot();
const auto saved_bytes = Bytes();
runtime::QueryConfigurationCapability();
runtime::CanChooseSetting(runtime::kSetHookPoint, static_cast<float>(runtime::hook_point.load()), &error);
Check(profile::Snapshot() == saved_snapshot && Bytes() == saved_bytes
          && runtime::hook_point.load() == 2 && std::abs(runtime::pass_resolution_scale[0].load() - .67f) < .0001f,
      "Load and capability inspection preserve unsupported old saved values.");

runtime::enabled = true;
runtime::d3d12_swapchain_seen = true;
renodx::addons::dlss5::gpu_timers::Stats sample;
const int64_t now = runtime::SteadyNowNs();
sample.valid = true;
sample.measured_ns = now;
runtime::comparison_ui::CaptureSettings(&sample.identity);
Check(runtime::CurrentGpuSample(sample, now), "Fresh timing with matching configuration identity is current.");
for (auto member : {&renodx::addons::dlss5::gpu_comparison::Identity::configuration_generation,
                    &renodx::addons::dlss5::gpu_comparison::Identity::settings_hash,
                    &renodx::addons::dlss5::gpu_comparison::Identity::resolution_hash,
                    &renodx::addons::dlss5::gpu_comparison::Identity::route,
                    &renodx::addons::dlss5::gpu_comparison::Identity::fg}) {
  auto old = sample;
  ++(old.identity.*member);
  Check(!runtime::CurrentGpuSample(old, now), "Old generation / settings / resolution / route / FG identity cannot be current.");
}
Check(!runtime::CurrentGpuSample(sample, now - 1)
          && !runtime::CurrentGpuSample(sample, now + renodx::addons::dlss5::gpu_timers::internal::kSampleAgeNs + 1),
      "Future and expired timing samples cannot be current.");
sample.valid = false;
Check(!runtime::CurrentGpuSample(sample, now), "Invalid timing cannot be current.");
sample.valid = true;
runtime::enabled = false;
Check(!runtime::CurrentGpuSample(sample, now), "NR Off never reports current GPU timing.");

// Exercise the real storage worker in independent AppData roots. The owner is
// obtained from a clean worker load, never guessed from the test's executable.
{
  namespace data = renodx::addons::dlss5::profiles;
  const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
  std::wstring previous_appdata(required, L'\0');
  if (required != 0) {
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", previous_appdata.data(), required);
    if (length >= required) throw std::runtime_error("LOCALAPPDATA changed during owner fixture setup.");
    previous_appdata.resize(length);
  }
  struct AppdataGuard {
    std::wstring previous;
    ~AppdataGuard() {
      SetEnvironmentVariableW(L"LOCALAPPDATA", previous.empty() ? nullptr : previous.c_str());
    }
  } appdata_guard{previous_appdata};
  const auto load_without_binding = [&](const std::filesystem::path& appdata) {
    if (profile::Busy()) throw std::runtime_error("Owner fixture requires an idle profile worker.");
    if (!SetEnvironmentVariableW(L"LOCALAPPDATA", appdata.c_str())) {
      throw std::runtime_error("Cannot isolate owner fixture AppData.");
    }
    for (const auto* key : {"ProfileGameId", "ProfileBalancedInitialized"}) {
      if (!WritePrivateProfileStringA(runtime::kConfigSection, key, nullptr, ini_path.c_str())) {
        throw std::runtime_error("Cannot clear synthetic profile binding.");
      }
    }
    profile::loaded = false;
    profile::writable = true;
    profile::dirty = profile::save_failed = false;
    profile::active = -1;
    profile::pending_action = profile::Action::NONE;
    profile::first_install = {};
    profile::reset = {};
    profile::slot_task = profile::SlotTask::NONE;
    profile::capability_validator = {};
    profile::interaction_busy = {};
    profile::Initialize();
    const auto deadline = GetTickCount64() + 10000;
    while (profile::Busy()) {
      profile::Tick();
      if (GetTickCount64() >= deadline) throw std::runtime_error("Owner fixture load timed out.");
      Sleep(1);
    }
    profile::Tick();
    Check(profile::loaded && profile::writable && !profile::save_failed,
          "Real profile storage worker completes isolated no-binding load.");
  };
  const auto fixture_root = base_directory / "owner-no-binding";
  load_without_binding(fixture_root / "clean");
  const auto owner = profile::state.owner;
  Check(!owner.empty() && profile::fresh_storage, "Empty isolated profile root is genuinely fresh.");
  for (const bool same_owner : {true, false}) {
    const auto appdata = fixture_root / (same_owner ? "same-owner" : "other-game");
    if (!SetEnvironmentVariableW(L"LOCALAPPDATA", appdata.c_str())) {
      throw std::runtime_error("Cannot isolate existing owner fixture.");
    }
    std::filesystem::path old_path;
    if (!data::StoragePath(std::string(32, 'a'), &old_path, &error)) throw std::runtime_error(error);
    auto old_state = data::EmptyState(same_owner ? owner : std::string(64, 'f'));
    old_state.slots[3].populated = true;
    old_state.slots[3].snapshot = {{"NRIntensity", .625f}};
    if (!data::Write(old_path, &old_state, 0, profile::Validate, &error)) throw std::runtime_error(error);
    std::ifstream old_file(old_path, std::ios::binary);
    const std::string old_bytes{std::istreambuf_iterator<char>(old_file), std::istreambuf_iterator<char>()};
    old_file.close();
    load_without_binding(appdata);
    Check(profile::fresh_storage == !same_owner,
          "Surviving same-owner Slot file blocks freshness; another game's valid owner does not.");
    const auto before = profile::Snapshot();
    const auto ini_before = Bytes();
    if (same_owner) {
      Check(!profile::TryInitializeBalanced(profile::FirstInstallEvidence::PROVEN_EMPTY,
                                            [](const Json&, std::string*) { return true; })
                && !profile::first_install.pending && profile::Snapshot() == before && Bytes() == ini_before,
            "Old owner without INI binding cannot auto-initialize or change current settings / INI.");
    }
    std::ifstream unchanged_file(old_path, std::ios::binary);
    Check(std::string(std::istreambuf_iterator<char>(unchanged_file), std::istreambuf_iterator<char>()) == old_bytes,
          "No-binding discovery preserves the surviving Slot document byte-for-byte.");
  }
  profile::loaded = false;
}
