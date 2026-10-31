/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#define DEBUG_LEVEL_0

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The fixture links the pinned ImGui implementation directly. Suppress only
// ReShade's forwarding wrappers while retaining its real header-only host boundary.
#include <deps/imgui/imgui.h>
#include <deps/imgui/imgui_internal.h>
struct TextItem {
  std::string text;
  ImRect rect;
  ImGuiWindow* window;
};
std::vector<TextItem> rendered_text;
std::vector<std::string> tooltip_text;
// Test-local Windows message probe: Unicode hooks see WM_CHAR before an ANSI
// retrieval converts it. No hook is installed in the add-on or other threads.
HWND message_probe_window = nullptr;
std::wstring message_probe_unicode;
LRESULT CALLBACK ObserveUnicodeMessage(int code, WPARAM removed, LPARAM value) {
  if (code >= 0 && removed == PM_REMOVE) {
    const auto* message = reinterpret_cast<const MSG*>(value);
    if (message->hwnd == message_probe_window && message->message == WM_CHAR) {
      message_probe_unicode += static_cast<wchar_t>(message->wParam);
    }
  }
  return CallNextHookEx(nullptr, code, removed, value);
}
LRESULT CALLBACK MessageProbeProcedure(HWND window, UINT message, WPARAM value, LPARAM data) {
  if (message == WM_CHAR) return 0;
  return DefWindowProcA(window, message, value, data);
}
// Substitute only the OS clipboard, retaining the production CF_UNICODETEXT
// allocation/conversion paths. Tests never read or replace the user's clipboard.
namespace clipboard_fixture {
HGLOBAL contents = nullptr;
BOOL WINAPI Open(HWND) { return TRUE; }
BOOL WINAPI Close() { return TRUE; }
HANDLE WINAPI Get(UINT format) { return format == CF_UNICODETEXT ? contents : nullptr; }
BOOL WINAPI Empty() {
  if (contents != nullptr) GlobalFree(contents);
  contents = nullptr;
  return TRUE;
}
HANDLE WINAPI Set(UINT format, HANDLE memory) {
  if (format != CF_UNICODETEXT) return nullptr;
  contents = memory;
  return memory;
}
}  // namespace clipboard_fixture
namespace ImGui {
inline void ObservedTextV(const char* fmt, va_list args) {
  va_list copy;
  va_copy(copy, args);
  char text[4096];
  std::vsnprintf(text, sizeof(text), fmt, copy);
  va_end(copy);
  TextV(fmt, args);
  if ((GetCurrentWindow()->Flags & ImGuiWindowFlags_Tooltip) != 0) tooltip_text.emplace_back(text);
}
inline void ObservedTextUnformatted(const char* begin, const char* end = nullptr) {
  TextUnformatted(begin, end);
  rendered_text.push_back({.text = (end == nullptr ? std::string(begin) : std::string(begin, end)),
                           .rect = ImRect(GetItemRectMin(), GetItemRectMax()),
                           .window = GetCurrentWindow()});
}
inline void ObservedText(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  TextV(fmt, args);
  va_end(args);
  rendered_text.push_back({.text = fmt, .rect = ImRect(GetItemRectMin(), GetItemRectMax()), .window = GetCurrentWindow()});
}
inline void ObservedTextDisabled(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  va_list copy;
  va_copy(copy, args);
  char text[4096];
  std::vsnprintf(text, sizeof(text), fmt, copy);
  va_end(copy);
  TextDisabledV(fmt, args);
  va_end(args);
  rendered_text.push_back({.text = text, .rect = ImRect(GetItemRectMin(), GetItemRectMax()), .window = GetCurrentWindow()});
}
inline void ObservedTextWrapped(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  va_list copy;
  va_copy(copy, args);
  char text[4096];
  std::vsnprintf(text, sizeof(text), fmt, copy);
  va_end(copy);
  TextWrappedV(fmt, args);
  va_end(args);
  rendered_text.push_back({.text = text, .rect = ImRect(GetItemRectMin(), GetItemRectMax()), .window = GetCurrentWindow()});
}
}  // namespace ImGui
namespace native_window_fixture {
inline BOOL WINAPI Show(HWND window, int command) {
  return ShowWindow(window, GetDlgItem(window, 1001) != nullptr ? SW_HIDE : command);
}
inline BOOL WINAPI Foreground(HWND window) {
  if (GetDlgItem(window, 1001) != nullptr) return TRUE;
  return SetForegroundWindow(window);
}
}  // namespace native_window_fixture
#define TextUnformatted ObservedTextUnformatted
#define Text            ObservedText
#define TextDisabled    ObservedTextDisabled
#define TextWrapped     ObservedTextWrapped
#define TextV           ObservedTextV
#define OpenClipboard clipboard_fixture::Open
#define CloseClipboard clipboard_fixture::Close
#define GetClipboardData clipboard_fixture::Get
#define EmptyClipboard clipboard_fixture::Empty
#define SetClipboardData clipboard_fixture::Set
#define ShowWindow native_window_fixture::Show
#define SetForegroundWindow native_window_fixture::Foreground
#undef IMGUI_VERSION_NUM
#include "ui/runtime_monitor.hpp"
#include "dlssnr.hpp"
#undef ShowWindow
#undef SetForegroundWindow
#undef OpenClipboard
#undef CloseClipboard
#undef GetClipboardData
#undef EmptyClipboard
#undef SetClipboardData
#undef TextUnformatted
#undef Text
#undef TextDisabled
#undef TextWrapped
#undef TextV
#define IMGUI_VERSION_NUM 19250
#include <deps/imgui/imgui_internal.h>

namespace runtime = renodx::addons::dlss5::internal;
namespace ui = renodx::addons::dlss5::ui;
using Json = nlohmann::json;
#include "monitor_performance_cases.hpp"
#include "monitor_session_cases.hpp"

struct Item {
  ImGuiID id = 0;
  ImRect rect;
  std::string label;
  bool disabled = false;
  ImGuiWindow* window = nullptr;
  ImGuiItemStatusFlags status = 0;
};
std::map<ImGuiID, Item> items;
std::map<std::string, ImRect> snapshot_regions;

void ImGuiTestEngineHook_ItemAdd(ImGuiContext* context, ImGuiID id, const ImRect& rect, const ImGuiLastItemData*) {
  items[id] = {.id = id, .rect = rect, .disabled = (context->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0, .window = context->CurrentWindow};
  if (std::getenv("NR_NEXUS_UI_SNAPSHOTS") != nullptr || std::getenv("NR_NEXUS_DOC_CAPTURES") != nullptr) {
    const auto* window = context->CurrentWindow;
    for (int index = 1; window != nullptr && index < window->IDStack.Size; ++index) {
      for (const auto& [scope, name] : {std::pair{"result_settings", "result_controls"},
                                       {"##pass_chain", "chain"}, {"##pass_editor", "pass_editor"},
                                       {"##quality_columns", "model_controls"}, {"NRPreset", "model_primary"},
                                       {"NRIntensity", "model_primary"}, {"NRTransferStrength", "model_primary"},
                                       {"NRColorStrength", "model_primary"}}) {
        if (window->IDStack[index] != ImHashStr(scope, 0, window->IDStack[index - 1])) continue;
        auto [region, inserted] = snapshot_regions.try_emplace(name, rect);
        if (!inserted) region->second.Add(rect);
      }
    }
  }
  // BeginCombo does not emit ItemInfo in the pinned ImGui version.
  if (context->CurrentWindow != nullptr && !context->CurrentWindow->IDStack.empty()) {
    for (const char* label : {"##model", "##theme", "##resolution"}) {
      if (id == context->CurrentWindow->GetID(label)) {
        items[id].label = label;
      }
    }
  }
}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags status) {
  items[id].label = label;
  items[id].status = status;
}
void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) { return "test item"; }

extern "C" __declspec(dllexport) void ReShadeLogMessage(void*, int, const char*) {}
extern "C" __declspec(dllexport) bool ReShadeGetBasePath(char* path, size_t* size) {
  const std::string base = std::filesystem::current_path().string();
  if (path == nullptr) {
    *size = base.size() + 1;
  } else {
    const size_t length = (std::min)(*size - 1, base.size());
    std::memcpy(path, base.c_str(), length);
    path[length] = '\0';
    *size = length;
  }
  return true;
}
extern "C" __declspec(dllexport) bool ReShadeGetConfigValue(
    void*, reshade::api::effect_runtime*, const char* section, const char* key, char* value, size_t* size) {
  char buffer[32768]{};
  // The pinned ReShade stores an empty INI value as an empty vector and its
  // Add-on getter returns false. Initial-install classification reads the
  // section before any automatic writes, independently of this value query.
  const DWORD count = GetPrivateProfileStringA(section, key, "", buffer, sizeof(buffer), ".\\ReShade.ini");
  if (count == 0) {
    *size = 0;
    return false;
  }
  if (value == nullptr) {
    *size = count + 2;
  } else {
    const size_t copied = (std::min)(*size - 1, static_cast<size_t>(count) + 1);
    std::memcpy(value, buffer, copied);
    value[copied] = '\0';
    *size = copied;
  }
  return true;
}
unsigned configuration_writes = 0;
extern "C" __declspec(dllexport) void ReShadeSetConfigValue(
    void*, reshade::api::effect_runtime*, const char* section, const char* key, const char* value) {
  ++configuration_writes;
  if (!WritePrivateProfileStringA(section, key, value, ".\\ReShade.ini")) {
    std::abort();
  }
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::puts("production menu phase=initialize");
  const auto profile_fixture_root = std::filesystem::current_path() / L"profile-fixture-local";
  std::filesystem::create_directories(profile_fixture_root);
  SetEnvironmentVariableW(L"LOCALAPPDATA", profile_fixture_root.c_str());
  try {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    ImGui::GetCurrentContext()->TestEngineHookItems = true;
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1800.f, 1300.f);
    io.DeltaTime = 1.f / 60.f;
    ImFontGlyphRangesBuilder ranges_builder;
    ranges_builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
    for (const auto& language : ui::kLanguages) {
      for (const auto& entry : language.table) {
        ranges_builder.AddText(entry.en);
        ranges_builder.AddText(entry.text);
      }
    }
    ranges_builder.AddText("\xef\x80\x8c\xef\x81\xb1\xef\x80\x8d\xef\x84\x8c\xef\x83\xa2");
    ranges_builder.AddText("中文电影画质_日本語_한국어_é_作者_Élodie_日本_巫师3_Witcher_中文描述，混合 café／한글／e\xcc\x81");
    ranges_builder.AddText("我的夜景方案（自定义）");
    ImVector<ImWchar> ranges;
    ranges_builder.BuildRanges(&ranges);
    const char* font_path = "C:/Windows/Fonts/msyh.ttc";
    if (!std::filesystem::exists(font_path)) {
      font_path = "C:/Windows/Fonts/segoeui.ttf";
    }
    io.Fonts->AddFontFromFileTTF(font_path, 18.f, nullptr, ranges.Data);
    // Use real glyph widths for the supported scripts even on a Chinese
    // Windows install. These are fixture-only fonts, never shipped assets.
    ImFontConfig fallback;
    fallback.MergeMode = true;
    for (const char* path : {"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/malgun.ttf",
                             "C:/Windows/Fonts/leelawui.ttf"}) {
      if (std::filesystem::exists(path)) {
        io.Fonts->AddFontFromFileTTF(path, 18.f, &fallback, ranges.Data);
      }
    }
    ImFont* latin_only_font = io.Fonts->AddFontDefault();
    unsigned char* atlas;
    int atlas_width, atlas_height;
    io.Fonts->GetTexDataAsRGBA32(&atlas, &atlas_width, &atlas_height);
    io.Fonts->SetTexID(1);
    runtime::ui_welcomed = true;
    renodx::addons::dlss5::gpu_timers::comparison_settings = runtime::comparison_ui::CaptureSettings;
    runtime::ui_language_choice = 1;
    runtime::direct_runtime_requirements.file_version = "310.9.16.0";
    runtime::direct_runtime_requirements.dual_models = true;
    // Synthetic capability evidence for menu interaction, never a GPU claim.
    runtime::d3d12_present_seen = true;
    runtime::direct_load_state = runtime::DirectLoadState::Ready;
    runtime::native_fg_declaration = runtime::NativeFgDeclaration::kOff;
    runtime::native_fg_public_off = true;
    runtime::last_input_width = runtime::last_output_width = 3840;
    runtime::last_input_height = runtime::last_output_height = 2160;
    unsigned checks = 0;
    std::function<void()> capture_failure;
    const auto check = [&](bool condition, const char* message) {
      ++checks;
      if (!condition) {
        std::fprintf(stderr, "Assertion failed: %s\n", message);
        try {
          if (capture_failure) capture_failure();
        } catch (const std::exception& diagnostic_error) {
          std::fprintf(stderr, "Capture unavailable: %s\n", diagnostic_error.what());
        }
        throw std::runtime_error(message);
      }
    };
    // Exercise the production loader before it has a chance to migrate or
    // stamp defaults into a fresh section. All writes stay in the fixture's INI.
    const auto load_theme_case = [&](const char* content, ui::ThemeId expected) {
      WritePrivateProfileStringA(nullptr, nullptr, nullptr, ".\\ReShade.ini");
      if (content != nullptr) {
        std::ofstream("ReShade.ini", std::ios::trunc | std::ios::binary) << content;
      } else {
        std::filesystem::remove("ReShade.ini");
      }
      WritePrivateProfileStringA(nullptr, nullptr, nullptr, ".\\ReShade.ini");
      runtime::ui_theme_choice = ui::ThemeId::MIDNIGHT_OLED;
      runtime::LoadConfiguration();
      if (runtime::ui_theme_choice.load() != expected) {
        std::printf("theme load case: expected=%s actual=%s ini=%s\n", ui::GetTheme(expected).slug,
                    ui::GetTheme(runtime::ui_theme_choice.load()).slug, (content != nullptr ? content : "<missing file>"));
      }
      check(runtime::ui_theme_choice.load() == expected, "production loader selected the wrong theme for this INI");
    };
    load_theme_case(nullptr, ui::ThemeId::CYBER_NEON);
    load_theme_case("", ui::ThemeId::CYBER_NEON);
    load_theme_case("[OTHER]\nValue=1\n", ui::ThemeId::CYBER_NEON);
    const std::string section = std::string("[") + runtime::kConfigSection + "]\n";
    const auto load_monitor_session = [&](const std::string& values) {
      runtime::monitor_visibility_initialized = false;
      load_theme_case((section + "UiTheme=cyber_neon\n" + values).c_str(), ui::ThemeId::CYBER_NEON);
    };
    load_monitor_session("");
    check(!runtime::monitor_requested.load() && !runtime::monitor_visible_preference.load(),
          "first installation keeps the monitor hidden without opening the host overlay");
    load_monitor_session("UiMonitorVisible=1\nUiMonitorKeepVisible=1\n");
    check(runtime::monitor_requested.load() && runtime::monitor_visible_preference.load(),
          "a new game session restores an explicitly visible monitor");
    runtime::monitor_requested = false;
    runtime::LoadConfiguration();
    check(!runtime::monitor_requested.load(), "repeated configuration initialization does not reopen a closed monitor");
    runtime::monitor_requested = true;
    runtime::ToggleMonitor();
    auto* monitor_config_context = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(nullptr);
    runtime::SaveMonitorVisibility();
    ImGui::SetCurrentContext(monitor_config_context);
    bool saved_monitor_visible = true;
    reshade::get_config_value(nullptr, runtime::kConfigSection, "UiMonitorVisible", saved_monitor_visible);
    check(!saved_monitor_visible && !runtime::monitor_visibility_dirty.load(),
          "F11 close persists immediately so normal and abnormal exits retain the choice");
    runtime::ToggleMonitor();
    runtime::SaveMonitorSettings();
    reshade::get_config_value(nullptr, runtime::kConfigSection, "UiMonitorVisible", saved_monitor_visible);
    check(saved_monitor_visible, "F11 open persists without relying on game shutdown");
    runtime::monitor_keep_visible = false;
    runtime::OnReshadeOpenOverlay(nullptr, false, reshade::api::input_source::keyboard);
    runtime::SaveMonitorSettings();
    reshade::get_config_value(nullptr, runtime::kConfigSection, "UiMonitorVisible", saved_monitor_visible);
    check(!runtime::monitor_requested.load() && saved_monitor_visible,
          "Home temporary hiding preserves the player's saved display intent");
    load_monitor_session("UiMonitorVisible=1\nUiMonitorKeepVisible=0\n");
    check(!runtime::monitor_requested.load() && runtime::monitor_visible_preference.load(),
          "restoration respects disabled keep-visible without capturing gameplay input");
    runtime::OnReshadeOpenOverlay(nullptr, true, reshade::api::input_source::keyboard);
    check(runtime::monitor_requested.load(), "the saved intent becomes visible when the player opens Home");
    runtime::OnReshadeOpenOverlay(nullptr, false, reshade::api::input_source::keyboard);
    load_monitor_session("UiMonitorVisible=0\nNRMonitorKey=0\n");
    check(!runtime::monitor_requested.load() && runtime::monitor_hotkey.load() == 0,
          "an explicit hidden choice and an unbound shortcut survive initialization");
    load_monitor_session("");
    check(!runtime::monitor_requested.load(), "a different game's empty configuration cannot inherit visibility");
    load_theme_case((section + "; empty section\n").c_str(), ui::ThemeId::CYBER_NEON);
    load_theme_case((section + "NRIntensity=1.3\n").c_str(), ui::ThemeId::NEXUS_DARK);
    load_theme_case((section + "ConfigVersion=8\n").c_str(), ui::ThemeId::NEXUS_DARK);
    load_theme_case((section + "UiTheme=invalid-theme\nNRIntensity=1.3\n").c_str(), ui::ThemeId::CYBER_NEON);
    load_theme_case((section + "UiTheme=\n").c_str(), ui::ThemeId::NEXUS_DARK);
    const std::string future_ini = section + "ConfigVersion=4294967295\nNRIntensity=1.3\n";
    load_theme_case(future_ini.c_str(), ui::ThemeId::NEXUS_DARK);
    {
      std::ifstream input("ReShade.ini", std::ios::binary);
      const std::string actual((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
      check(actual == future_ini, "future schema theme fallback rewrote the configuration");
    }
    for (const auto& theme : ui::kThemes) {
      load_theme_case((section + "UiTheme=" + theme.slug + "\nNRIntensity=1.3\n").c_str(), theme.id);
      runtime::StoreUiTheme(theme.id);
      char stored[64]{};
      GetPrivateProfileStringA(runtime::kConfigSection, "UiTheme", "missing", stored, sizeof(stored), ".\\ReShade.ini");
      check(std::string(stored) == theme.slug, "StoreUiTheme did not persist the public slug");
      runtime::ui_theme_choice = ui::ThemeId::CYBER_NEON;
      runtime::LoadConfiguration();
      check(runtime::ui_theme_choice.load() == theme.id, "theme preference did not survive production reload");
    }
    for (const auto opacity : {0.f, .8f, 1.f}) {
      load_theme_case((section + "UiTheme=cyber_neon\nUiMonitorOpacity=" + std::to_string(opacity)
          + "\nUiMonitorTrends=1\nUiMonitorHistorySeconds=60\nUiMonitorAnchor=8\nUiMonitorOffsetX=-12\nUiMonitorOffsetY=-9\n").c_str(), ui::ThemeId::CYBER_NEON);
      const auto& monitor = runtime::ui_runtime_monitor_state;
      check(std::abs(monitor.background_opacity - opacity) < .001f && monitor.trends
                && monitor.history_seconds == 60 && monitor.anchor == 8 && monitor.apply_geometry
                && monitor.offset.x == -12.f && monitor.offset.y == -9.f,
            "monitor opacity endpoints, history and anchored position survive production configuration loading");
    }
    load_theme_case((section + "UiTheme=cyber_neon\nUiMonitorOpacity=nan\nUiMonitorHistorySeconds=7\n"
        "UiMonitorAnchor=99\nUiMonitorOffsetX=inf\nUiMonitorOffsetY=nan\nUiMonitorTrendMask=4294967295\n").c_str(), ui::ThemeId::CYBER_NEON);
    check(runtime::ui_runtime_monitor_state.background_opacity == .8f
              && runtime::ui_runtime_monitor_state.history_seconds == 30
              && runtime::ui_runtime_monitor_state.anchor == 8
              && runtime::ui_runtime_monitor_state.offset.x == 0.f
              && runtime::ui_runtime_monitor_state.offset.y == 0.f
              && runtime::ui_runtime_monitor_state.trend_mask == 0x7f,
          "malformed persisted monitor settings are sanitized without disabling the UI");
    load_theme_case("", ui::ThemeId::CYBER_NEON);
    check(runtime::ui_runtime_monitor_state.background_opacity == .8f
              && !runtime::ui_runtime_monitor_state.trends && runtime::ui_runtime_monitor_state.anchor == -1,
          "absent monitor settings retain safe defaults and the existing custom geometry model");
    runtime::ui_welcomed = true;
    runtime::ui_language_choice = 1;
    float panel_width = 1000.f;
    float panel_height = 1100.f;
    float font_size = 18.f;
    double panel_total_ms = 0.0;
    unsigned panel_frames = 0;
    bool clipped_callback = false;
    bool preset_fixture = false;
    bool preset_latin_font = false;
    const ui::MonitorInputs* monitor_fixture = nullptr;
    const ui::MonitorInputs* resolution_observed_fixture = nullptr;
    bool hud_fixture = false;
    bool independent_hud_fixture = false;
    ui::MonitorState synthetic_monitor_state;
    const auto frame = [&](const ui::StatusCard* diagnostic_case = nullptr) {
      items.clear();
      snapshot_regions.clear();
      rendered_text.clear();
      tooltip_text.clear();
      ImGui::GetStyle().FontSizeBase = font_size;
      ImGui::NewFrame();
      ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
      ImGui::SetNextWindowSize(ImVec2(panel_width, panel_height));
      ImGui::Begin("NR Nexus menu fixture", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar
          | (independent_hud_fixture ? ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav : 0));
      auto& context = *ImGui::GetCurrentContext();
      const ImGuiStyle style_before = ImGui::GetStyle();
      const int color_depth = context.ColorStack.Size;
      const int var_depth = context.StyleVarStack.Size;
      const int font_depth = context.FontStack.Size;
      const int disabled_depth = context.DisabledStackSize;
      const int window_depth = context.CurrentWindowStack.Size;
      if (clipped_callback) {
        ImGui::PushClipRect(ImVec2(-10.f, -10.f), ImVec2(-1.f, -1.f), true);
      }
      const auto panel_start = std::chrono::steady_clock::now();
      if (resolution_observed_fixture != nullptr) {
        const auto* saved_view = runtime::ui_monitor_view;
        runtime::ui_monitor_view = resolution_observed_fixture;
        runtime::DrawResolutionObserved(ui::kModernText);
        runtime::ui_monitor_view = saved_view;
      } else if (independent_hud_fixture) {
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0.f, 0.f), io.DisplaySize, IM_COL32(6, 14, 25, 255));
        // Synthetic scene marker makes a deliberately hidden HUD distinguishable
        // from a missing or failed capture without fabricating a game image.
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(20.f, io.DisplaySize.y - 52.f),
            ImVec2(280.f, io.DisplaySize.y - 20.f), IM_COL32(13, 34, 48, 255));
        ImGui::GetBackgroundDrawList()->AddText(ImVec2(28.f, io.DisplaySize.y - 46.f),
            IM_COL32(120, 155, 180, 255), "Synthetic host frame");
        runtime::OnHudOverlay(nullptr);
      } else if (preset_fixture) {
        if (preset_latin_font) ImGui::PushFont(latin_only_font, font_size);
        runtime::RefreshUiLanguage();
        ui::SetTheme(runtime::ui_theme_choice.load());
        ui::StyleScope theme;
        ui::PushPanelTheme(theme);
        ImGui::OpenPopup("###nr_preset_manager");
        if (auto* popup = ImGui::FindWindowByName("###nr_preset_manager")) {
          const ImVec2 size((std::min)(panel_width * 0.94f, font_size * 43.f),
                            (std::min)(panel_height * 0.90f, font_size * 39.f));
          ImGui::SetWindowSize(popup, size);
          ImGui::SetWindowPos(popup, ImVec2((panel_width - size.x) * 0.5f, (panel_height - size.y) * 0.5f));
        }
        runtime::preset_ui::DrawManager();
        if (preset_latin_font) ImGui::PopFont();
      } else if (monitor_fixture != nullptr) {
        runtime::RefreshUiLanguage();
        ui::SetTheme(runtime::ui_theme_choice.load());
        ui::StyleScope theme;
        ui::PushPanelTheme(theme);
        ImGui::BeginChild("##r3_window_evidence", ImVec2(io.DisplaySize.x >= 1200.f ? 650.f : 0.f, 0.f));
        const ui::StatusSummaryFields fields{
            .resolution = monitor_fixture->passes[0].actual_size,
            .stage = monitor_fixture->passes[0].execution_proof,
            .gpu = monitor_fixture->passes[0].gpu_cost};
        if (ui::DrawStatusSummary(monitor_fixture->card, monitor_fixture->recent.partial ? 1u : 0u, fields)) {
          synthetic_monitor_state.open = !synthetic_monitor_state.open;
        }
        if (io.DisplaySize.x >= 1200.f && synthetic_monitor_state.open && !synthetic_monitor_state.geometry.valid) {
          ImGui::SetNextWindowFocus();
        }
        ui::DrawRuntimeMonitor(*monitor_fixture, &synthetic_monitor_state);
        ImGui::TextWrapped("Synthetic window evidence fixture; no game execution, internal network or GPU performance claim.");
        runtime::DrawModernLayout(nullptr, renodx::addons::dlss5::ComputeNrVerdict(runtime::CollectFunnelInputs()), monitor_fixture->card);
        ImGui::EndChild();
      } else if (diagnostic_case != nullptr) {
        ui::StyleScope theme;
        ui::PushPanelTheme(theme);
        runtime::DrawRuntimeStatus(*diagnostic_case);
      } else {
        runtime::OnOverlay(nullptr);
        if (hud_fixture) {
          // Font/mesh preparation can exceed a real Toast's lifetime. Capture
          // its initial visible phase without changing production timing.
          runtime::profile_feedback_until_ns = runtime::SteadyNowNs() + runtime::kHudToastNs;
          const auto* focused = ImGui::GetCurrentContext()->NavWindow;
          runtime::OnHudOverlay(nullptr);
          check(ImGui::GetCurrentContext()->NavWindow == focused, "operation HUD must not take navigation focus");
        }
      }
      if (clipped_callback) {
        ImGui::PopClipRect();
      }
      check(std::memcmp(&style_before, &ImGui::GetStyle(), sizeof(style_before)) == 0,
            "production callback did not restore the full host ImGuiStyle");
      check(context.ColorStack.Size == color_depth && context.StyleVarStack.Size == var_depth
                && context.FontStack.Size == font_depth && context.DisabledStackSize == disabled_depth
                && context.CurrentWindowStack.Size == window_depth,
            "production callback leaked an ImGui stack");
      panel_total_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - panel_start).count();
      ++panel_frames;
      ImGui::End();
      ImGui::Render();
      check(ui::style_depth == 0, "production menu leaks a style scope");
      for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows) {
        // Native multiline editors own an internal text scroller. The outer
        // input's item bounds, rather than that private cursor, define layout.
        if (std::strstr(window->Name, "##preset_description_") != nullptr) continue;
        if (std::strstr(window->Name, "##issue_steps_") != nullptr) {
          // InputTextMultiline reserves cursor scrolling space beyond its
          // text viewport. Validate the editor bounds, not that private cursor.
          if (window->Active && window->ParentWindow != nullptr) {
            check(window->Pos.x >= window->ParentWindow->InnerRect.Min.x - 1.f
                      && window->Pos.x + window->Size.x <= window->ParentWindow->InnerRect.Max.x + 1.f,
                  "report multiline editor extends outside its parent");
          }
          continue;
        }
        if (window->Active && (window->Flags & ImGuiWindowFlags_ChildWindow) != 0
            && (std::strstr(window->Name, "NR Nexus menu fixture") != nullptr
                || std::strstr(window->Name, "nr_preset_manager") != nullptr)) {
          const float overflow = window->DC.CursorMaxPos.x - (window->Pos.x + window->Size.x - window->WindowPadding.x - window->ScrollbarSizes.x);
          if (overflow > 1.f) {
            std::printf("overflow: window=%s width=%.0f font=%.0f page=%d classic=%d scroll_x=%.1f\n",
                        window->Name, panel_width, font_size, runtime::ui_menu_page,
                        runtime::ui_classic_layout.load(), overflow);
            std::printf("  cursor_max=%.1f pos=%.1f size=%.1f padding=%.1f scrollbar=%.1f available=%.1f\n",
                        window->DC.CursorMaxPos.x, window->Pos.x, window->Size.x, window->WindowPadding.x,
                        window->ScrollbarSizes.x, window->WorkRect.GetWidth());
            for (const auto& [id, item] : items) {
              if (item.window == window && item.rect.Max.x > window->InnerRect.Max.x) {
                std::printf("  item=%s rect_min_x=%.1f rect_max_x=%.1f right=%.1f language=%d\n", item.label.c_str(),
                            item.rect.Min.x, item.rect.Max.x, window->InnerRect.Max.x, runtime::ui_language_choice.load());
              }
            }
            for (const TextItem& text : rendered_text) {
              if (text.window == window && text.rect.Max.x > window->InnerRect.Max.x) {
                std::printf("  text=%s rect_max_x=%.1f\n", text.text.c_str(), text.rect.Max.x);
              }
            }
          }
          check(overflow <= 1.f, "production control layout creates horizontal overflow");
        }
      }
    };
    const auto find = [&](const std::string& label) {
      for (const auto& [id, item] : items) {
        if (item.label == label || item.label.starts_with(label + "\n") || item.label.starts_with(label + " (") || item.label.ends_with("###" + label)) {
          return item;
        }
      }
      // Clipped native buttons emit ItemAdd before their visible label info.
      for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows) {
        if (!window->Active || window->IDStack.empty()) continue;
        if (const auto id = window->GetID(label.c_str()); items.contains(id)) return items.at(id);
        const auto id = window->GetID((label.starts_with("##") ? label : "###" + label).c_str());
        if (items.contains(id)) return items.at(id);
      }
      throw std::runtime_error("menu item missing: " + label);
    };
    const auto click = [&](const Item& item) {
      ImVec2 center = item.rect.GetCenter();
      for (int attempt = 0; item.window != nullptr && attempt < 4
           && (center.y < item.window->InnerClipRect.Min.y + 1.f || center.y >= item.window->InnerClipRect.Max.y - 1.f); ++attempt) {
        ImGui::SetScrollY(item.window, item.window->Scroll.y + center.y - item.window->InnerClipRect.GetCenter().y);
        frame();
        frame();
        check(items.contains(item.id), "scrolling lost the target control");
        center = items.at(item.id).rect.GetCenter();
      }
      io.AddMousePosEvent(center.x, center.y);
      frame();
      io.AddMouseButtonEvent(0, true);
      frame();
      io.AddMouseButtonEvent(0, false);
      frame();
      frame();
    };
    const auto hover = [&](const Item& item, const char* expected, const char* excluded) {
      ImVec2 center = item.rect.GetCenter();
      if (center.y < item.window->InnerClipRect.Min.y + 1.f || center.y >= item.window->InnerClipRect.Max.y - 1.f) {
        ImGui::SetScrollY(item.window, item.window->Scroll.y + center.y - item.window->InnerClipRect.GetCenter().y);
        frame();
        frame();
        center = items.at(item.id).rect.GetCenter();
      }
      for (int i = 0; i < 40; ++i) {
        center = items.at(item.id).rect.GetCenter();
        io.AddMousePosEvent(center.x, center.y);
        frame();
      }
      if (std::ranges::find(tooltip_text, expected) == tooltip_text.end()) {
        std::printf("Tooltip target=%s id=%08x hovered=%08x at=%.1f,%.1f expected=%s\n", item.label.c_str(), item.id,
                    ImGui::GetCurrentContext()->HoveredId, center.x, center.y, expected);
        const auto found = items.find(item.id);
        if (found != items.end()) std::printf("  current_rect=%.1f,%.1f..%.1f,%.1f disabled=%d window=%s mouse=%.1f,%.1f\n",
                                              found->second.rect.Min.x, found->second.rect.Min.y, found->second.rect.Max.x, found->second.rect.Max.y,
                                              found->second.disabled, found->second.window->Name, io.MousePos.x, io.MousePos.y);
        for (const auto& value : tooltip_text) std::printf("  actual=%s\n", value.c_str());
      }
      check(std::ranges::find(tooltip_text, expected) != tooltip_text.end(), "candidate tooltip does not describe the hovered choice");
      if (excluded != nullptr) check(std::ranges::find(tooltip_text, excluded) == tooltip_text.end(), "candidate tooltip repeats a different choice");
      for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows) {
        if (window->Active && (window->Flags & ImGuiWindowFlags_Tooltip) != 0)
          check(window->Size.x < io.DisplaySize.x * 0.7f, "candidate tooltip is wider than its viewport budget");
      }
      io.AddMousePosEvent(-100.f, -100.f);
      frame();
    };
    const char* retention_environment = std::getenv("NR_NEXUS_TEST_RETENTION");
    const std::string retention = (retention_environment == nullptr ? "Standard" : retention_environment);
    if (retention != "Standard" && retention != "Full" && retention != "Acceptance") {
      throw std::runtime_error("Unknown NR_NEXUS_TEST_RETENTION mode");
    }
    std::vector<unsigned char> saved_atlas;
    std::string atlas_file;
    unsigned atlas_count = 0;
    unsigned capture_count = 0;
    const auto capture = [&](const char* name, const ui::StatusCard* diagnostic_case = nullptr, bool settle = true) {
      // Store the actual ImGui mesh and atlas; the optional CPU renderer uses
      // these without substituting HTML controls or a second menu definition.
      const std::string_view requested_capture(name);
      if (requested_capture.find("tooltip") == std::string_view::npos && !ImGui::IsMouseDown(0) && !ImGui::IsMouseDown(1)) {
        io.AddMousePosEvent(-100.f, -100.f);
      }
      if (settle) {
        frame(diagnostic_case);
        frame(diagnostic_case);
        frame(diagnostic_case);
      }
      // Settling remains part of the fixture in every mode. Only persistence is gated.
      const std::string_view capture_name(name);
      if (retention == "Standard" && capture_name != "quality-wide" && capture_name != "ux-library-home"
          && capture_name != "ux-restore-confirmation-zh" && capture_name != "presets-narrow-zh"
          && capture_name != "presets-4k-zh" && capture_name != "failure"
          && std::getenv("NR_NEXUS_UX_REPRO") == nullptr && std::getenv("NR_NEXUS_DOC_CAPTURES") == nullptr
          && std::getenv("NR_NEXUS_UI_SNAPSHOTS") == nullptr) {
        return;
      }
      Json output{{"width", panel_width}, {"height", panel_height}, {"theme", ui::CurrentTheme().slug},
                  {"page", runtime::ui_menu_page}, {"language", runtime::ui_language_choice.load()},
                  {"font_size", font_size}, {"expanded", ui::expand_all_sections}, {"lists", Json::array()}};
      const ImDrawData* data = ImGui::GetDrawData();
      for (const ImDrawList* list : data->CmdLists) {
        Json vertices = Json::array(), indices = Json::array(), commands = Json::array();
        for (const ImDrawVert& vertex : list->VtxBuffer) {
          vertices.push_back({vertex.pos.x, vertex.pos.y, vertex.uv.x, vertex.uv.y, vertex.col});
        }
        for (const ImDrawCmd& command : list->CmdBuffer) {
          if (command.UserCallback == nullptr) {
            commands.push_back({command.IdxOffset, command.ElemCount, command.VtxOffset,
                                command.ClipRect.x, command.ClipRect.y, command.ClipRect.z, command.ClipRect.w});
          }
        }
        for (ImDrawIdx index : list->IdxBuffer) {
          indices.push_back(index);
        }
        output["lists"].push_back({{"vertices", vertices}, {"indices", indices}, {"commands", commands}});
      }
      io.Fonts->GetTexDataAsRGBA32(&atlas, &atlas_width, &atlas_height);
      output["atlas_width"] = atlas_width;
      output["atlas_height"] = atlas_height;
      const size_t atlas_bytes = static_cast<size_t>(atlas_width) * atlas_height * 4;
      if (saved_atlas.size() != atlas_bytes || std::memcmp(saved_atlas.data(), atlas, atlas_bytes) != 0) {
        atlas_file = "atlas-" + std::to_string(atlas_count++) + ".rgba";
        std::ofstream texture(atlas_file, std::ios::binary);
        texture.write(reinterpret_cast<char*>(atlas), static_cast<std::streamsize>(atlas_bytes));
        texture.close();
        if (!texture) throw std::runtime_error("Unable to persist capture atlas");
        saved_atlas.assign(atlas, atlas + atlas_bytes);
      }
      output["atlas_file"] = atlas_file;
      std::ofstream mesh(std::string(name) + ".json");
      mesh << output;
      mesh.close();
      if (!mesh) throw std::runtime_error("Unable to persist capture mesh");
      ++capture_count;
    };
    capture_failure = [&]() {
      // A failing assertion must remain diagnosable even in the lightweight mode.
      if (!ImGui::GetCurrentContext()->WithinFrameScope && ImGui::GetDrawData() != nullptr) {
        capture("failure", nullptr, false);
      } else {
        std::fputs("Capture unavailable during active frame; existing captures and assertion trace retained\n", stderr);
      }
    };

    if (std::getenv("NR_NEXUS_UI_SNAPSHOTS") != nullptr) {
#include "ui_snapshot_cases.hpp"
    }
    if (std::getenv("NR_NEXUS_MONITOR_PERFORMANCE") != nullptr) {
      RunMonitorPerformanceCases(check);
      ImGui::DestroyContext();
      return 0;
    }

    frame();
    frame();
    if (std::getenv("NR_NEXUS_PROFILE_MENU_COST") != nullptr) {
      namespace profile = runtime::profile_ui;
      const auto deadline = GetTickCount64() + 10000;
      while (profile::Busy()) {
        profile::Tick();
        check(GetTickCount64() < deadline, "profile cost fixture initialization timed out");
        Sleep(1);
      }
      const auto measure = [&](const char* label, const auto& action) {
        for (int warmup = 0; warmup < 8; ++warmup) action();
        const auto start = std::chrono::steady_clock::now();
        for (int sample = 0; sample < 120; ++sample) action();
        std::printf("profile menu CPU %s mean_us=%.3f; synthetic, excludes game/GPU\n", label,
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / 120.0);
      };
      runtime::monitor_requested = false;
      measure("Home_FG_off", [&] { frame(); });
      runtime::native_fg_declaration = runtime::NativeFgDeclaration::kOn;
      runtime::native_fg_public_off = false;
      runtime::native_fg_owns = true;
      measure("Home_FG_on", [&] { frame(); });
      measure("six_slot_validation", [&] {
        for (int index = 0; index < 6; ++index) {
          std::string error;
          profile::SlotAvailable(index, &error);
        }
      });
      runtime::monitor_requested = true;
      runtime::monitor_keep_visible = true;
      runtime::hud_overlay_open = false;
      independent_hud_fixture = true;
      measure("F11_only", [&] { frame(); });
      std::printf("PASS profile menu CPU investigation; checks=%u\n", checks);
      ImGui::DestroyContext();
      return 0;
    }
    RunMonitorSessionCases(check, [&](const ui::MonitorInputs& inputs) {
      const auto* saved_fixture = monitor_fixture;
      const auto saved_state = synthetic_monitor_state;
      const auto saved_flush = runtime::flush_profile_storage;
      monitor_fixture = &inputs;
      synthetic_monitor_state.open = true;
      synthetic_monitor_state.detailed = true;
      try {
        frame();
      } catch (...) {
        monitor_fixture = saved_fixture;
        synthetic_monitor_state = saved_state;
        runtime::flush_profile_storage = saved_flush;
        throw;
      }
      monitor_fixture = saved_fixture;
      synthetic_monitor_state = saved_state;
      runtime::flush_profile_storage = saved_flush;
    });
    if (std::getenv("NR_NEXUS_MONITOR_SESSION_ONLY") != nullptr) {
      namespace timers = renodx::addons::dlss5::gpu_timers;
      const auto saved_resolution_language = ui::CurrentLanguage();
      ui::SelectLanguage(13);
      const auto saved_hook = runtime::hook_point.load();
      const auto saved_count = runtime::stack_passes.load();
      const auto saved_enabled = runtime::enabled.load();
      const auto saved_follow = runtime::nr_follow_input_res.load();
      const auto saved_scale = runtime::nr_resolution_scale.load();
      const auto saved_rr = runtime::stage_source_rr.load();
      const auto saved_timer = timers::stage_stats[0];
      const auto saved_proof = timers::execution_stats[0];
      const auto saved_observed = runtime::stage_telemetry[0].last_evaluate;
      const auto saved_input_width = runtime::last_input_width.load();
      const auto saved_input_height = runtime::last_input_height.load();
      runtime::hook_point = runtime::kHookRender;
      runtime::stack_passes = 1;
      runtime::enabled = true;
      runtime::stage_source_rr = false;
      runtime::nr_follow_input_res = false;
      runtime::nr_resolution_scale = .67f;
      runtime::last_input_width = 1715;
      runtime::last_input_height = 965;
      auto& sample = timers::stage_stats[0];
      sample = {};
      std::strcpy(sample.path, "preSR");
      sample.width = 2560;
      sample.height = 1440;
      sample.valid = true;
      sample.state = timers::SampleState::VALID;
      sample.sequence = 10;
      sample.recorded_ns = sample.measured_ns = timers::NowNs();
      sample.identity.device_session = timers::device_session;
      runtime::comparison_ui::CaptureSettings(&sample.identity);
      sample.identity.source_generation = 42;
      sample.identity.stable_epoch = runtime::comparison_stable_epochs[0];
      runtime::stage_telemetry[0].last_evaluate.source_generation = 42;
      runtime::stage_telemetry[0].last_evaluate.source_released = false;
      sample.passes[0] = {.id = 0, .width = 1715, .height = 965, .state = timers::SampleState::VALID};
      timers::execution_stats[0] = {};
      check(runtime::CurrentPassWorkingEstimate(0, timers::NowNs())
                == renodx::addons::dlss5::capability::Size{1715, 965},
            "Render estimate ignores last_input=1715x965 and scales its bound 2560x1440 reference once");
      ui::MonitorInputs resolution_view;
      resolution_view.pass_count = 1;
      resolution_view.passes[0].execution_confirmed = true;
      resolution_view.passes[0].actual_size = "1715 x 965";
      resolution_observed_fixture = &resolution_view;
      io.AddMousePosEvent(-100.f, -100.f);
      frame(); frame();
      check(rendered_text.size() == 1 && rendered_text[0].text.starts_with("P1 | ")
                && rendered_text[0].text.find("1715 x 965") != std::string::npos,
            "matching actual and estimated Render dimensions form one explicit P1 line");
      capture("resolution-observed-matching-zh");
      const auto matching_target = rendered_text[0].rect.GetCenter();
      io.AddMousePosEvent(matching_target.x, matching_target.y);
      for (int hover_frame = 0; hover_frame < 45; ++hover_frame) frame();
      check(std::ranges::any_of(rendered_text, [](const TextItem& item) {
              return item.text.find("1715x965") != std::string::npos;
            }) && std::ranges::any_of(rendered_text, [](const TextItem& item) {
              return item.text.find("2560x1440") != std::string::npos;
            }), "merged matching-size line retains an explanatory estimate and Stage reference tooltip");
      capture("resolution-observed-matching-tooltip-zh", nullptr, false);
      io.AddMousePosEvent(-100.f, -100.f);
      resolution_view.passes[0].actual_size = "1600 x 900";
      frame(); frame();
      check(rendered_text.size() == 2 && rendered_text[0].text.find("1600 x 900") != std::string::npos
                && rendered_text[1].text.find("1715x965") != std::string::npos,
            "different working dimensions show actual and estimate on separate lines");
      const auto resolution_target = rendered_text[0].rect.GetCenter();
      io.AddMousePosEvent(resolution_target.x, resolution_target.y);
      for (int hover_frame = 0; hover_frame < 45; ++hover_frame) frame();
      check(std::ranges::any_of(rendered_text, [](const TextItem& item) {
              return item.text.find("1715x965") != std::string::npos;
            }) && std::ranges::any_of(rendered_text, [](const TextItem& item) {
              return item.text.find("2560x1440") != std::string::npos;
            }), "different-size tooltip exposes the singly scaled estimate and unscaled Stage reference");
      capture("resolution-observed-difference-tooltip-zh", nullptr, false);
      const auto resolution_language = ui::CurrentLanguage();
      ui::SelectLanguage(0);
      io.AddMousePosEvent(-100.f, -100.f);
      frame(); frame();
      capture("resolution-observed-difference-en");
      ui::SelectLanguage(resolution_language);
      sample.recorded_ns -= timers::internal::kSampleAgeNs + 1;
      check(runtime::CurrentPassWorkingEstimate(0, timers::NowNs()) == renodx::addons::dlss5::capability::Size{},
            "expired reference cannot be replaced with last_input dimensions");
      sample.recorded_ns = sample.measured_ns;
      ++sample.identity.source_generation;
      check(runtime::CurrentPassWorkingEstimate(0, timers::NowNs()) == renodx::addons::dlss5::capability::Size{},
            "another source cannot supply the current Render estimate");
      resolution_observed_fixture = nullptr;
      runtime::hook_point = saved_hook;
      runtime::stack_passes = saved_count;
      runtime::enabled = saved_enabled;
      runtime::nr_follow_input_res = saved_follow;
      runtime::nr_resolution_scale = saved_scale;
      runtime::stage_source_rr = saved_rr;
      runtime::last_input_width = saved_input_width;
      runtime::last_input_height = saved_input_height;
      timers::stage_stats[0] = saved_timer;
      timers::execution_stats[0] = saved_proof;
      runtime::stage_telemetry[0].last_evaluate = saved_observed;
      ui::SelectLanguage(saved_resolution_language);
      std::printf("PASS production monitor session evidence cases; checks=%u\n", checks);
      ImGui::DestroyContext();
      return 0;
    }
    if (std::getenv("NR_NEXUS_UX_REPRO") != nullptr) {
      namespace profile = runtime::profile_ui;
      const auto deadline = GetTickCount64() + 10000;
      while (profile::Busy()) {
        profile::Tick();
        check(GetTickCount64() < deadline, "profile initialization timed out in UX repro");
        Sleep(1);
      }
      runtime::ui_language_choice = 14;
      runtime::ui_menu_page = 0;
      panel_width = 650.f;
      panel_height = 1400.f;
      io.DisplaySize = ImVec2(panel_width, panel_height);
      profile::active = 0;
      profile::state.slots[0].overrides["NRIntensity"] = 0.625f;
      frame(); frame();
      const auto lite_center = find(ui::Tr("NR Lite")).rect.GetCenter();
      io.AddMousePosEvent(lite_center.x, lite_center.y); frame();
      io.AddMouseButtonEvent(1, true); frame();
      io.AddMouseButtonEvent(1, false); frame(); frame();
      click(find("restore_profile"));
      capture("ux-restore-repro");
      bool found = false;
      for (const auto* window : ImGui::GetCurrentContext()->Windows) {
        if (window->Active && (window->Flags & ImGuiWindowFlags_Popup) != 0
            && (window->Flags & ImGuiWindowFlags_Tooltip) == 0) {
          found = true;
          std::printf("restore confirmation width=%.1f height=%.1f font=%.1f\n", window->Size.x, window->Size.y, font_size);
          check(window->Size.x >= font_size * 14.f && window->Size.y < panel_height * 0.6f,
                "restore confirmation collapses into a vertical text strip");
        }
      }
      check(found, "restore confirmation was not opened");
      ImGui::DestroyContext();
      return 0;
    }
    for (const auto& theme : ui::kThemes) {
      const auto previous = ui::CurrentThemeId();
      runtime::StoreUiTheme(theme.id);
      check(ui::CurrentThemeId() == previous, "theme persistence changed visual state outside the overlay frame");
      frame();
      check(ui::CurrentThemeId() == theme.id, "overlay did not apply the saved theme on the next frame");
      clipped_callback = true;
      frame();
      clipped_callback = false;
    }
    runtime::StoreUiTheme(ui::ThemeId::CYBER_NEON);
    frame();
    runtime::ui_menu_page = 3;
    panel_width = 1400.f;
    frame();
    frame();
    click(find("Interface & notifications"));
    for (const auto& theme : ui::kThemes) {
      click(find(theme.name));
      check(runtime::ui_theme_choice.load() == theme.id && ui::CurrentThemeId() == theme.id,
            "clicking the visible theme name did not apply its palette");
      char stored[64]{};
      GetPrivateProfileStringA(runtime::kConfigSection, "UiTheme", "missing", stored, sizeof(stored), ".\\ReShade.ini");
      check(std::string(stored) == theme.slug, "theme selection click did not save its slug");
    }
    click(find(ui::text::kRestoreTheme));
    check(runtime::ui_theme_choice.load() == ui::ThemeId::CYBER_NEON
              && ui::CurrentThemeId() == ui::ThemeId::CYBER_NEON,
          "theme reset did not restore Cyber Neon");
    runtime::ui_menu_page = 0;
    panel_width = 1000.f;
    frame();
    const ui::StatusCard normal_card{.tone = ui::CardTone::kGood, .title = "Normal synthetic state", .body = "Normal synthetic detail"};
    frame(&normal_card);
    for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows) {
      check(!window->Active || std::strstr(window->Name, "##nr_status_card") == nullptr,
            "normal Diagnostics still duplicates the large status card");
    }
    const ui::StatusCard warning_card{.tone = ui::CardTone::kWarn, .title = "Synthetic fault", .body = "Synthetic fault details retained", .fix = {"Synthetic repair step"}, .fix_count = 1};
    frame(&warning_card);
    check(std::ranges::any_of(rendered_text, [](const TextItem& item) { return item.text == "Synthetic fault details retained"; }),
          "diagnostic deduplication discarded fault details");
    frame();
    frame();
    for (int page = 0; page < 4; ++page) {
      click(find(ui::text::kMenuPages[page]));
      check(runtime::ui_menu_page == page, "category navigation failed");
      const Item expand = find("expand_all");
      const Item collapse = find("collapse_all");
      check(std::abs(expand.rect.Min.y - collapse.rect.Min.y) <= 0.5f,
            "page tools share one row");
      Item last = collapse;
      if (page != 2) {
        last = find("reset_page");
        check(std::abs(last.rect.Min.y - collapse.rect.Min.y) <= 0.5f, "page reset shares the fold toolbar row");
        click(last);
        check(!find("cancel_page_reset").disabled, "three pages share an accessible reset confirmation");
        io.AddKeyEvent(ImGuiKey_Escape, true);
        frame();
        io.AddKeyEvent(ImGuiKey_Escape, false);
        frame();
        check(!runtime::profile_ui::Resetting(), "Escape started a page reset");
        click(find("reset_page"));
        click(find("cancel_page_reset"));
      } else {
        check(std::ranges::none_of(items, [](const auto& entry) { return entry.second.label.ends_with("###reset_page"); }),
              "Diagnostics has no page reset command");
      }
      const auto* window = last.window;
      check(std::abs(last.rect.Max.x - (window->WorkRect.Max.x - ImGui::GetStyle().ScrollbarSize)) <= 1.f,
            "all page tools align to the right scroller content edge");
    }
    runtime::ui_menu_page = 0;
    frame();
    Item status_entry;
    bool status_found = false;
    for (const auto& [id, item] : items) {
      if (item.label.find("###status_details") != std::string::npos) {
        status_entry = item;
        status_found = true;
      }
    }
    check(status_found, "status navigation entry missing");
    click(status_entry);
    check(runtime::ui_menu_page == 0 && !find("status_open_diagnostics").disabled,
          "nonmodal monitor preserves the page and exposes diagnostics");
    {
      const auto* monitor = ImGui::FindWindowByName("###nr_runtime_monitor");
      check(monitor != nullptr && monitor->Active
            && (monitor->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Modal)) == 0,
            "runtime monitor is an independent nonmodal window");
      const Item enable_control = find("##nr_enable");
      check(!enable_control.disabled, "opening monitor keeps main controls enabled");
      const bool previous_enabled = runtime::enabled.load();
      click(enable_control);
      check(runtime::enabled.load() != previous_enabled && monitor->Active,
            "main NR switch remains interactive while monitor stays open");
      click(find("##nr_enable"));
      check(runtime::enabled.load() == previous_enabled && monitor->Active,
            "main switch can be restored without closing the monitor");
      click(find("status_close"));
      frame();
      check(!monitor->Active, "explicit close stops monitor submission");
      click(find("status_details"));
      check(monitor->Active, "summary entry reopens the nonmodal monitor");
    }
    {
      namespace timers = renodx::addons::dlss5::gpu_timers;
      if (!runtime::ui_runtime_monitor_state.detailed) click(find("monitor_details"));
      const auto original = timers::stage_stats[1];
      const bool was_enabled = runtime::enabled.load();
      runtime::enabled = true;
      auto& sample = timers::stage_stats[1];
      sample = {};
      sample.valid = true;
      sample.state = timers::SampleState::VALID;
      sample.measured_ns = timers::NowNs();
      sample.recorded_ns = sample.measured_ns;
      sample.identity.device_session = timers::device_session;
      runtime::comparison_ui::CaptureSettings(&sample.identity);
      sample.samples = 1;
      sample.passes[0] = {.id = 0, .model = 1, .width = 640, .height = 360,
          .output_width = 640, .output_height = 360, .eval_us = 100,
          .state = timers::SampleState::VALID};
      runtime::monitor_snapshot.attempted_ns = 0;
      frame(); frame();
      const auto measured_pass = [](const TextItem& item) {
        return item.text.find("640x360") != std::string::npos
            || item.text.find("640 x 360") != std::string::npos;
      };
      check(std::ranges::any_of(rendered_text, measured_pass), "details expose dimensions from a GPU-complete pass sample");
      sample.measured_ns -= 3'000'000'000;
      runtime::monitor_snapshot.attempted_ns = 0;
      frame(); frame();
      check(std::ranges::none_of(rendered_text, measured_pass), "details reject stale pass dimensions instead of showing configured values as actual");
      sample = original;
      runtime::enabled = was_enabled;
    }
    click(find("status_open_diagnostics"));
    check(runtime::ui_menu_page == 2, "status details did not navigate to Diagnostics");
    if (runtime::ui_runtime_monitor_state.open) click(find("status_close"));
    click(find("Quality"));
    const bool enabled_before = runtime::enabled.load();
    click(find("##nr_enable"));
    check(runtime::enabled.load() != enabled_before, "NR switch failed");
    click(find("##nr_enable"));
    check(runtime::enabled.load() == enabled_before, "NR switch restore failed");
    const float own_intensity = runtime::kSetIntensity.Get();
    runtime::StoreUiTheme(ui::ThemeId::FLUENT_LIGHT);
    frame();
    runtime::kSetIntensity.Store(1.3f);
    check(std::none_of(rendered_text.begin(), rendered_text.end(), [](const auto& item) {
      return item.text == "Built-in effect" || item.text == "Mine";
    }), "legacy temporary effect switch remains visible");
    runtime::kSetIntensity.Store(own_intensity);
    {
      namespace profile = runtime::profile_ui;
      const auto original = profile::Snapshot();
      const auto deadline = GetTickCount64() + 10000;
      const auto settle_profiles = [&]() {
        while (profile::Busy() || profile::dirty || profile::pending_action != profile::Action::NONE) {
          profile::Tick();
          check(GetTickCount64() < deadline, "profile UI worker did not settle");
          check(!profile::save_failed, "profile UI save failed");
          Sleep(1);
        }
        frame(); frame();
      };
      settle_profiles();
      check(profile::loaded && profile::active == -1, "first profile UI load changed game association");
      click(find("NR Lite"));
      settle_profiles();
      check(profile::active == 0, "official profile button did not select the fixed Lite slot");
      runtime::kSetIntensity.Commit(0.625f);
      profile::Observe();
      profile::Schedule(true);
      settle_profiles();
      click(find("NR Balanced"));
      settle_profiles();
      click(find("NR Lite"));
      settle_profiles();
      check(profile::Snapshot()["NRIntensity"] == 0.625f, "official quick buttons lost autosaved override");
      profile::active = -1;
      profile::state.selected.clear();
      std::string error;
      check(profile::ApplySnapshot(original, &error), "restore game fixture after profile UI test");
      profile::observed_snapshot = original;
      profile::undo.available = false;
      frame(); frame();
    }
#include "profile_ux_cases.hpp"
#include "profile_menu_cases.hpp"
    const uint32_t old_hook = runtime::hook_point.load();
    const uint32_t old_multiplier = runtime::present_fg_multiplier.load();
    runtime::hook_point = runtime::kHookPresent;
    runtime::present_fg_multiplier = 4;
    runtime::native_fg_owns = true;
    const auto native_inputs = runtime::CollectCardInputs(ComputeNrVerdict(runtime::CollectFunnelInputs()), runtime::SteadyNowNs());
    check(native_inputs.frame_gen_multiplier == 0, "native FG owner reused a stale legacy Present multiplier");
    runtime::native_fg_owns = false;
    runtime::hook_point = old_hook;
    runtime::present_fg_multiplier = old_multiplier;
    hover(find("A"), ui::text::kModelATip, ui::text::kModelBTip);
    hover(find("B"), ui::text::kModelBTip, ui::text::kModelATip);

    // Main zero means Auto; changing the UI must retain legacy stored values.
    for (const auto& [label, value] : {std::pair{"A", 1.f}, {"B", 2.f}, {"Auto", 0.f}}) {
      const auto model_row = find("A").rect.Min.y;
      for (const auto& [id, item] : items) {
        if (item.label == label && std::abs(item.rect.Min.y - model_row) < 1.f) {
          const Item choice = item;
          click(choice);
          break;
        }
      }
      check(runtime::kSetPreset.Get() == value, "main segmented model selection failed");
    }
    runtime::kSetPreset.Store(3.f);
    frame();
    check(runtime::kSetPreset.Get() == 3.f, "legacy model request was rewritten by drawing");
    runtime::kSetPreset.Store(0.f);
    panel_width = 320.f;
    frame();
    frame();
    click(find("##model"));
    hover(find("B"), ui::text::kModelBTip, ui::text::kModelAutoTip);
    click(find("B"));
    check(runtime::kSetPreset.Get() == 2.f, "narrow model dropdown does not preserve B semantics");
    click(find("##model"));
    click(find("Auto"));
    check(runtime::kSetPreset.Get() == 0.f, "narrow main-model zero is not Auto");
    panel_width = 1000.f;
    frame();
    frame();

    const auto fold_open = [&](const char* label) {
      const Item item = find(label);
      return item.window->StateStorage.GetInt(item.id, 0) != 0;
    };
    runtime::kLookResult[0].max_brighten.Store(0.6f);
    click(find("expand_all"));
    check(fold_open(ui::text::kResultLimits),
          "expand all did not reach nested Quality groups");
    click(find("collapse_all"));
    frame();
    check(!fold_open(ui::text::kResultLimits),
          "changed parameters reopened a manually collapsed group");
    click(find(ui::text::kResultLimits));
    check(fold_open(ui::text::kResultLimits), "one-shot collapse prevents later manual opening");
    runtime::ui_language_choice = 3;
    frame();
    check(fold_open(ui::Tr(ui::text::kResultLimits)), "language change lost stable fold state");
    runtime::ui_language_choice = 1;
    runtime::ui_menu_page = 3;
    frame();
    click(find("collapse_all"));
    click(find("expand_all"));
    check(fold_open(ui::text::kHotkeys), "SectionHeader ignores the bulk command");
    click(find("collapse_all"));
    runtime::ui_menu_page = 0;
    frame();
    check(fold_open(ui::text::kResultLimits), "another page changed Quality fold state");
    check(runtime::kLookResult[0].max_brighten.Get() == 0.6f, "fold controls changed a parameter");
    runtime::kLookResult[0].max_brighten.Store(0.f);
    click(find("collapse_all"));

    click(find("nr_resolution_balanced"));
    hover(find("nr_resolution_balanced"), ui::text::kResBalancedTip, ui::text::kResQualityTip);
    check(!runtime::nr_follow_input_res.load() && std::abs(runtime::nr_resolution_scale.load() - 0.58f) < 0.001f,
          "resolution preset did not select its original parameter pair");
    click(find("nr_resolution_follow"));
    check(runtime::nr_follow_input_res.load() && runtime::nr_resolution_scale.load() == .58f,
          "Follow input did not retain the saved scale");
    check(find("##resolution_scale").disabled, "Follow input leaves the saved scale editable");
    for (const auto& theme : ui::kThemes) {
      runtime::StoreUiTheme(theme.id);
      panel_width = 450.f;
      frame(); frame();
      for (const auto& text : rendered_text) {
        if (text.text != ui::Tr(ui::text::kResolutionSavedScale)) continue;
        for (const auto& [id, item] : items) {
          if (item.window == text.window && item.label == ui::icons::kUndo) {
            check(!item.rect.Overlaps(text.rect), "resolution reset overlaps the wrapped follow-input explanation");
          }
        }
      }
    }
    panel_width = 1000.f;
    frame(); frame();
    click(find("##resolution_scale"));
    check(runtime::nr_resolution_scale.load() == .58f, "disabled follow slider changed the saved scale");
    runtime::nr_follow_input_res = false;
    runtime::nr_resolution_scale = 0.75f;
    frame();
    check(runtime::nr_resolution_scale.load() == 0.75f, "drawing custom resolution changed its saved scale");
    check(!find("##resolution_scale").disabled, "fixed scale slider remains disabled");
    // Both editors must use the same storage without a navigation-triggered commit.
    runtime::stack_passes = 2;
    runtime::ui_selected_pass = 0;
    runtime::ui_menu_page = 1;
    const auto navigation_epoch = runtime::profile_commit_epoch.load();
    frame();
    frame();
    check(runtime::nr_resolution_scale.load() == .75f && runtime::profile_commit_epoch.load() == navigation_epoch,
          "Pass 1 navigation rewrites the shared resolution");
    check(!find("##resolution_scale").disabled, "Pass 1 does not expose the shared scale");
    runtime::nr_resolution_scale = .6705f;
    frame();
    click(find("nr_resolution_quality"));
    check(runtime::nr_resolution_scale.load() == .67f, "Pass 1 preset is not shared with Quality");
    runtime::ui_selected_pass = 1;
    runtime::pass_resolution_mode[0] = 2;
    runtime::pass_resolution_scale[0] = .58f;
    frame();
    check(find("##pass_resolution_scale").id != 0 && runtime::nr_resolution_scale.load() == .67f,
          "independent Pass 2 scale changes the base scale");
    for (int pass = 1; pass < 4; ++pass) {
      runtime::stack_passes = 4;
      runtime::ui_selected_pass = pass;
      runtime::pass_resolution_mode[pass - 1] = 2;
      frame(); frame();
      for (const auto& [label, value] : {std::pair{"100%", 1.f}, {"67%", .67f}, {"58%", .58f}, {"50%", .5f}}) {
        const Item choice = find(label);
        const float previous = runtime::pass_resolution_scale[pass - 1].load();
        click(choice);
        check(std::abs(runtime::pass_resolution_scale[pass - 1].load() - (choice.disabled ? previous : value)) < .0001f,
              "Pass 2-4 presets apply supported values and preserve blocked combinations");
      }
      runtime::pass_resolution_mode[pass - 1] = 0;
    }
    runtime::ui_selected_pass = 1;
    runtime::ui_menu_page = 0;
    frame();
    check(runtime::nr_resolution_scale.load() == .67f, "Quality does not retain the Pass 1 edit");
    const Item render_choice = find("Render");
    const Item upscaled_choice = find("Upscaled");
    const Item present_choice = find("Present");
    check(render_choice.rect.Min.x < upscaled_choice.rect.Min.x && upscaled_choice.rect.Min.x < present_choice.rect.Min.x,
          "Hook Point display order is not Render, Upscaled, Present");
    for (const auto& [label, stored] : {std::pair{"Render", 1u}, {"Upscaled", 0u}, {"Present", 2u}}) {
      click(find(label));
      if (runtime::stage_edit::pending) click(find("confirm_stage_change"));
      check(runtime::hook_point.load() == stored, "Hook Point UI changed historical storage encoding");
      click(find("nr_resolution_follow"));
      check(runtime::nr_follow_input_res.load(), "Follow input is unavailable at a supported Hook Point");
    }
    runtime::pass_resolution_mode[0] = 0;
    runtime::pass_resolution_scale[0] = 1.f;
    runtime::stack_passes = 1;
    runtime::nr_follow_input_res = false;
    runtime::nr_resolution_scale = 1.f;

    // A diagnostic shortcut must select the real Present controls, not a popup.
    runtime::hook_point = runtime::kHookPresent;
    runtime::ui_menu_page = 2;
    frame();
    frame();
    click(find(ui::text::kOpenPresentSettings));
    check(runtime::ui_menu_page == 0 && !runtime::ui_open_present_settings,
          "Present shortcut did not reach the Quality group");
    hover(find("Optional"), ui::text::kGuideOptional, ui::text::kGuideRequired);
    hover(find("Required"), ui::text::kGuideRequired, ui::text::kGuideNever);
    hover(find("Never"), ui::text::kGuideNever, ui::text::kGuideOptional);
    hover(find("Present"), ui::text::kHookPresentTip, ui::text::kHookRenderTip);
    hover(find("Natural"), ui::text::kStyleNaturalTip, ui::text::kStyleCinematicTip);
    click(find(ui::text::kFgAdvanced));
    hover(find("Every frame"), ui::text::kFgEveryTip, ui::text::kFgGameTip);
    hover(find("Game frames"), ui::text::kFgGameTip, ui::text::kFgEveryTip);
    hover(find("Stable timing"), ui::text::kFgDetectStableTip, ui::text::kFgDetectAutoTip);
    const auto has_text = [&](const char* value) {
      for (const auto& text : rendered_text) {
        if (text.text == value) return true;
      }
      return false;
    };
    const auto switch_for_label = [&](const char* label) {
      for (const auto& text : rendered_text) {
        if (text.text != label) continue;
        for (const auto& [id, item] : items) {
          if (item.label == "##value" && std::abs(item.rect.GetCenter().y - text.rect.GetCenter().y) < 5.f) return item;
        }
      }
      throw std::runtime_error(std::string("switch missing for label: ") + label);
    };
    check(!has_text(ui::text::kNetwork), "Quality still shows unavailable runtime-network readback");
    runtime::ui_menu_page = 2;
    ui::expand_all_sections = true;
    frame();
    frame();
    check(has_text(ui::text::kNetwork), "Detailed diagnostics lost the runtime-network limitation");
    runtime::ui_menu_page = 1;
    ui::expand_all_sections = false;
    frame();
    frame();
    check(!has_text(ui::text::kNetwork) && has_text(ui::text::kResolved),
          "Multi-Pass must retain resolved configuration without runtime-network readback");
    runtime::ui_menu_page = 0;
    frame();
    frame();
    runtime::look_mode = false;
    frame();
    click(find(ui::text::kResultBasic));
    check(has_text(ui::text::kResultStrength), "disabled result parameters cannot be inspected");
    check(!runtime::look_mode.load(), "inspecting result groups enabled result shaping");
    click(find(ui::text::kResultTemporal));
    check(has_text("Detail stability"), "independent detail stability is missing while look is off");
    runtime::look_mode = true;
    frame();
    check(has_text(ui::text::kResultStrength), "enabling result shaping lost its parameters");

    click(find("Multi-Pass"));
    const auto chain_nodes = [&]() {
      std::vector<Item> nodes;
      for (const auto& [id, item] : items) {
        if (item.label.ends_with("###chain_pass")) nodes.push_back(item);
      }
      std::sort(nodes.begin(), nodes.end(), [](const auto& left, const auto& right) {
        if (std::abs(left.rect.Min.y - right.rect.Min.y) > 1.f) return left.rect.Min.y < right.rect.Min.y;
        return left.rect.Min.x < right.rect.Min.x;
      });
      return nodes;
    };
#include "quality_editor_cases.hpp"
    for (unsigned count = 1; count <= 4; ++count) {
      runtime::stack_passes = count;
      frame();
      frame();
      unsigned nodes = 0;
      for (const auto& [id, item] : items) {
        if (item.label.ends_with("###chain_pass")) {
          ++nodes;
          check(item.rect.GetHeight() >= ImGui::GetTextLineHeight() * 3.f,
                "processing card reserves space for title, model and configuration state");
        }
      }
      check(nodes == count, "processing chain must include the single base pass");
      for (unsigned pass = 1; pass <= count; ++pass) {
        click(chain_nodes().at(pass - 1));
        check(runtime::ui_selected_pass == pass - 1, "chain selects the wrong editor");
      }
    }
    runtime::stack_passes = 4;
    frame();
    frame();
    const std::string chain_tip = std::string("Pass 4\n") + ui::text::kChainNote + "\n" + ui::text::kRequest + ": " + ui::text::kInheritModel
        + "\n" + (runtime::pass_follow[2].load() ? ui::text::kStyleFollows : ui::text::kStyleIndependent)
        + "\n" + (runtime::pass_look_result[2].follow.load() ? ui::text::kResultFollows : ui::text::kResultIndependent)
        + "\n" + ui::text::kModelNote + "\n" + ui::text::kChainMarkTip
        + "\n" + ui::text::kEffectiveStage + ": "
        + (runtime::RequestedStagePlan().valid
               ? renodx::addons::dlss5::stage_plan::Name(runtime::RequestedStagePlan().effective[3]) : ui::text::kStageBlocked);
    hover(chain_nodes().at(3), chain_tip.c_str(), nullptr);
    click(chain_nodes().at(1));
    check(runtime::ui_selected_pass == 1, "pass navigation failed");
    check(has_text(ui::text::kPassStage) && std::ranges::any_of(rendered_text, [](const auto& text) {
      return text.text.starts_with(std::string("P2 | ") + ui::Tr("Actual working resolution"));
    }),
          "pass editor identifies its Pass beside the current working-size or pending-confirmation line");
    check(!has_text("Recorded working sizes"), "ordinary pass editor does not repeat historical resolution evidence");
    runtime::ui_menu_page = 2;
    frame(); frame();
    click(find("##Recorded working sizes"));
    {
      const auto historical_hook = runtime::hook_point.load();
      runtime::hook_point = runtime::kHookUpscaled;
      auto& observed = runtime::stage_telemetry[1];
      const auto old_observed = observed;
      auto& sample = renodx::addons::dlss5::gpu_timers::stage_stats[1];
      const auto old_sample = sample;
      observed = {};
      observed.groups = 1;
      observed.pass_begin = 0;
      observed.pass_count = 4;
      observed.width = 1920;
      observed.height = 1080;
      observed.resolved_requests[1] = 2;
      observed.pass_resolutions[1] = {.requested_width = 1286, .requested_height = 724,
                                    .effective_width = 1286, .effective_height = 724, .model_recorded = true};
      sample = {};
      sample.valid = true;
      sample.measured_ns = runtime::SteadyNowNs();
      sample.passes[1] = {.id = 1, .model = 2, .width = 1286, .height = 724,
                         .state = renodx::addons::dlss5::gpu_timers::SampleState::VALID};
      frame();
      const char* historical = "Recent completed GPU sample: 1286x724 (historical; current settings unconfirmed).";
      check(has_text(historical) && has_text(ui::text::kResolutionCurrentPending),
            "recent GPU completion is not distinguished from current configuration");
      runtime::kSetPassResolutionScale[0].Commit(.73456f);
      frame();
      check(has_text(historical) && has_text(ui::text::kResolutionCurrentPending),
            "editing settings falsely certifies a recent historical GPU sample");
      sample.measured_ns = runtime::SteadyNowNs() - 3000000000ll;
      frame();
      check(!has_text(historical), "expired GPU sample is still shown as recent");
      observed.resolution_blocked = true;
      frame();
      check(has_text(ui::text::kStageResolutionBlocked), "same-stage size rejection is hidden");
      observed = old_observed;
      sample = old_sample;
      runtime::hook_point = historical_hook;
    }
    runtime::ui_menu_page = 1;
    frame(); frame();
    const auto saved_hook = runtime::hook_point.load();
    runtime::hook_point = runtime::kHookPresent;
    runtime::kSetPassStage[0].Commit(1.f);
    frame();
    frame();
    const std::string rejected_stage = std::string(ui::text::kStageBlocked) + ": "
        + renodx::addons::dlss5::stage_plan::ReasonText(runtime::RequestedStagePlan().reason);
    check(has_text(rejected_stage.c_str()),
          "invalid backward stage must display plan rejection and reason");
    check(runtime::kSetPassStage[0].Get() == 1.f, "rejected stage request must remain editable and saved");
    runtime::kSetPassStage[0].Commit(0.f);
    runtime::hook_point = saved_hook;
    frame();
    click(switch_for_label(ui::text::kInheritResult));
    check(!runtime::pass_look_result[0].follow.load(), "result inheritance switch failed");
    click(switch_for_label("Adjust NR's look"));
    check(runtime::pass_look_result[0].mode.load() && runtime::look_mode.load(),
          "pass result switch unexpectedly changed the main settings");
    runtime::kLookResult[1].halo.Commit(0.7f);
    click(switch_for_label(ui::text::kInheritResult));
    check(runtime::pass_look_result[0].follow.load() && runtime::pass_look_result[0].halo.load() == 0.7f,
          "result inheritance erased the independent limits");
    click(switch_for_label(ui::text::kInheritResult));
    check(runtime::LookSettingsSnapshots(false)[1].halo == 0.7f,
          "custom results were not restored after disabling inheritance");
    bool model_combo = false;
    for (const auto& [id, item] : items)
      if (item.label == "##model") model_combo = true;
    if (model_combo) click(find("##model"));
    click(find("B"));
    check(runtime::kSetPassPreset[0].Get() == 2.f, "pass model change failed");
    char saved[32]{};
    GetPrivateProfileStringA(runtime::kConfigSection, "NRPass2Preset", "missing", saved, sizeof(saved), ".\\ReShade.ini");
    check(std::string(saved) == "2", "pass model change was not persisted");
    runtime::stack_passes = 1;
    frame();
    check(runtime::ui_selected_pass == 0 && runtime::kSetPassPreset[0].Get() == 2.f,
          "reducing pass count cleared hidden settings or left an invalid selection");
    runtime::stack_passes = 4;
    frame();
    click(chain_nodes().at(1));
    const Item model = model_combo ? find("##model") : find("B");
    bool reset_found = false;
    Item model_reset;
    for (const auto& [id, item] : items) {
      if (item.label == ui::icons::kUndo && std::abs(item.rect.Min.y - model.rect.Min.y) < 1.f) {
        model_reset = item;
        reset_found = true;
        break;
      }
    }
    if (reset_found) {
      click(model_reset);
    }
    check(reset_found && runtime::kSetPassPreset[0].Get() == 0.f, "model reset button failed");
    runtime::direct_runtime_requirements.dual_models = false;
    runtime::kSetPassPreset[0].Store(2.f);
    capture("model-fallback");
    if (model_combo) click(find("##model"));
    check(find("B").disabled, "unsupported model B remains editable");
    hover(find("B"), "B requires DLSS NR 310.9.16. Unsupported selections use A; saved choices are preserved.", ui::text::kModelBTip);
    io.AddKeyEvent(ImGuiKey_Escape, true);
    frame();
    io.AddKeyEvent(ImGuiKey_Escape, false);
    frame();
    runtime::direct_runtime_requirements.file_version.clear();
    frame();
    frame();
    if (model_combo) click(find("##model"));
    check(find("A").disabled && find("B").disabled, "unidentified runtime permits model changes");
    io.AddKeyEvent(ImGuiKey_Escape, true);
    frame();
    io.AddKeyEvent(ImGuiKey_Escape, false);
    frame();
    check(runtime::kSetPassPreset[0].Get() == 2.f,
          "unidentified runtime rewrote the saved request");
    runtime::direct_runtime_requirements.file_version = "310.9.16.0";
    runtime::direct_runtime_requirements.dual_models = true;
    runtime::ui_open_diagnostics = true;
    frame();
    check(runtime::ui_menu_page == 2 && !runtime::ui_open_diagnostics, "status action cannot reach Diagnostics");
    click(find("Advanced"));
    click(find("Development & maintenance"));
    click(find("open_reset_all"));
    click(find("confirm_page_reset"));
    {
      const auto deadline = GetTickCount64() + 10000;
      while (runtime::profile_ui::Resetting() || runtime::profile_ui::Busy()) {
        runtime::profile_ui::Tick();
        if (GetTickCount64() > deadline) throw std::runtime_error("UI reset did not settle.");
        Sleep(1);
      }
      check(!runtime::profile_ui::save_failed, "UI reset must finish its protected persistence");
      frame(); frame();
    }
    check(runtime::stack_passes.load() == 1 && runtime::kSetPassPreset[0].Get() == 0.f,
          "confirmed restore defaults did not reset pass settings");
    check(runtime::ui_theme_choice.load() == ui::ThemeId::CYBER_NEON,
          "restore all resets interface preferences to the built-in theme");
    runtime::stack_passes = 4;

    click(find("developer_view"));
    check(runtime::ui_classic_layout.load(), "Developer view action did not select the classic layout");
    GetPrivateProfileStringA(runtime::kConfigSection, "UiMode", "missing", saved, sizeof(saved), ".\\ReShade.ini");
    check(std::string(saved) == "classic", "Developer view selection was not persisted");

    const float matrix_structure = runtime::local_structure_strength.load();
    const bool matrix_mask = runtime::use_auto_mask.load();
    const bool matrix_skin = runtime::skin_independent.load();
    runtime::local_structure_strength = 2.f;
    runtime::use_auto_mask = true;
    runtime::skin_independent = true;
    runtime::StoreUiTheme(ui::ThemeId::CYBER_NEON);
    std::printf("production menu phase=responsive-matrix start checks=%u frames=%u\n", checks, panel_frames);
    for (bool classic : {false, true}) {
      runtime::ui_classic_layout = classic;
      for (float font : {13.f, 26.f, 39.f}) {
        font_size = font;
        for (float width : {320.f, 450.f, 900.f, 1600.f}) {
          panel_width = width;
          for (int language = 1; language <= ui::kLanguageCount; ++language) {
            runtime::ui_language_choice = language;
            for (int page = 0; page < 4; ++page) {
              runtime::ui_menu_page = page;
              ui::expand_all_sections = (page == 3 || page == 1);
              runtime::pass_look_result[0].follow = false;
              runtime::pass_look_result[0].mode = true;
              for (unsigned hook = 0; hook < 3; ++hook) {
                runtime::hook_point = hook;
                runtime::present_fg_multiplier = hook + 2;
                runtime::nr_cost_gpu_ms = hook * 7.f;
                runtime::streamline_hooks_enabled = (hook == 2);
                runtime::hooks_enabled = true;
                runtime::ui_selected_pass = page;
                runtime::pass_follow[0] = (hook == 0);
                frame();
                frame();
                if (!classic) {
                  ImGuiWindow* settings = nullptr;
                  for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows) {
                    const auto* settings_name = std::strstr(window->Name, "/##nr_settings_");
                    if (window->Active && settings_name != nullptr && std::strchr(settings_name + 1, '/') == nullptr) {
                      settings = window;
                    }
                  }
                  check(settings != nullptr, "settings scroller missing");
                  const float anchor = settings->Pos.y;
                  runtime::enabled = !runtime::enabled.load();
                  runtime::streamline_hooks_enabled = !runtime::streamline_hooks_enabled.load();
                  runtime::present_fg_multiplier = 0;
                  runtime::nr_cost_gpu_ms = 25.f;
                  runtime::health_active_mask = 0;
                  frame();
                  if (std::abs(settings->Pos.y - anchor) > 0.01f) {
                    std::printf("scroller drift: %s page=%d width=%.0f font=%.0f language=%d anchor=%.1f now=%.1f\n",
                                settings->Name, page, width, font, language, anchor, settings->Pos.y);
                  }
                  check(std::abs(settings->Pos.y - anchor) <= 0.01f, "live refresh moved production settings scroller");
                }
              }
            }
          }
        }
      }
    }
    std::printf("production menu phase=responsive-matrix passed checks=%u frames=%u\n", checks, panel_frames);
    runtime::ui_classic_layout = false;
    runtime::ui_menu_page = 2;
    check(runtime::BeginIssueReport("ui-fixture"), "report entry cannot start the description step");
    for (int language = 1; language <= ui::kLanguageCount; ++language) {
      runtime::ui_language_choice = language;
      panel_width = 320.f;
      font_size = 26.f;
      frame();
      frame();
    }
    runtime::CloseIssueReport();
    check(runtime::issue_report::step.load() == runtime::issue_report::Step::kClosed,
          "report description cannot be cancelled");
    runtime::ui_language_choice = 14;
    runtime::local_structure_strength = matrix_structure;
    runtime::use_auto_mask = matrix_mask;
    runtime::skin_independent = matrix_skin;
    runtime::ui_classic_layout = false;
    runtime::ui_language_choice = 1;
    runtime::hook_point = runtime::kHookPresent;
    runtime::stack_passes = 4;
    runtime::ui_selected_pass = 1;
    runtime::pass_look_result[0].follow = false;
    runtime::pass_look_result[0].mode = true;
    io.AddMousePosEvent(-100.f, -100.f);
    unsigned themed_captures = 0;
    for (const auto& theme : ui::kThemes) {
      runtime::StoreUiTheme(theme.id);
      for (const auto& size : {ImVec2(320.f, 13.f), ImVec2(450.f, 26.f), ImVec2(1600.f, 39.f)}) {
        panel_width = size.x;
        font_size = size.y;
        for (int language : {1, 4, 14}) {
          runtime::ui_language_choice = language;
          for (int page = 0; page < 4; ++page) {
            runtime::ui_menu_page = page;
            frame();
            frame();
          }
        }
      }
      runtime::ui_language_choice = 1;
      font_size = 18.f;
      for (float width : {450.f, 1400.f}) {
        panel_width = width;
        for (int page = 0; page < 4; ++page) {
          runtime::ui_menu_page = page;
          for (bool expanded : {false, true}) {
            // Remove prior interaction/matrix fold state to capture genuine
            // default-open groups rather than a previous page's manual edits.
            for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows) {
              window->StateStorage.Clear();
              window->Scroll = ImVec2(0.f, 0.f);
            }
            for (auto& scope : ui::fold_scopes) {
              scope = {};
            }
            ui::expand_all_sections = expanded;
            const std::string name = std::string("theme-") + theme.slug + "-page-" + std::to_string(page)
                + (width < 500.f ? "-narrow" : "-wide") + (expanded ? "-expanded" : "-default");
            capture(name.c_str());
            ++themed_captures;
          }
        }
      }
      panel_width = 450.f;
      runtime::ui_menu_page = 2;
      for (ui::CardTone tone : {ui::CardTone::kWarn, ui::CardTone::kBad}) {
        const ui::StatusCard fault{.tone = tone, .title = "Synthetic diagnostic fault",
                                   .body = "Fixture-only warning/error; no runtime failure inferred.",
                                   .fix = {"Synthetic repair instruction"}, .fix_count = 1};
        const std::string name = std::string("theme-") + theme.slug
            + (tone == ui::CardTone::kWarn ? "-warning" : "-error");
        capture(name.c_str(), &fault);
        ++themed_captures;
      }
      runtime::ui_menu_page = 0;
      ui::expand_all_sections = false;
      frame();
      frame();
      click(find("##model"));
      check(std::ranges::any_of(items, [](const auto& entry) { return entry.second.label == "B"; }),
            "all-theme narrow model popup did not expose its real choices");
      capture((std::string("theme-") + theme.slug + "-popup").c_str(), nullptr, false);
      ++themed_captures;
      io.AddKeyEvent(ImGuiKey_Escape, true);
      frame();
      io.AddKeyEvent(ImGuiKey_Escape, false);
      frame();
      panel_width = 1400.f;
      frame();
      frame();
      const Item model_a = find("A");
      for (int tick = 0; tick < 40; ++tick) {
        const ImVec2 center = items.at(model_a.id).rect.GetCenter();
        io.AddMousePosEvent(center.x, center.y);
        frame();
      }
      check(std::ranges::find(tooltip_text, ui::text::kModelATip) != tooltip_text.end(),
            "all-theme tooltip did not describe the hovered model");
      capture((std::string("theme-") + theme.slug + "-tooltip").c_str(), nullptr, false);
      ++themed_captures;
      io.AddMousePosEvent(-100.f, -100.f);
    }
    check(themed_captures == 140, "seven themes must each capture four pages, both widths/fold states, faults, popup and tooltip");
    std::printf("all-theme capture scenarios=%u; persistence follows retention mode; synthetic fixture, no game/GPU/visual acceptance inferred\n", themed_captures);
    runtime::StoreUiTheme(ui::ThemeId::CYBER_NEON);
    runtime::ui_language_choice = 14;
    ui::expand_all_sections = false;
    panel_width = 1400.f;
    font_size = 18.f;
    runtime::ui_menu_page = 0;
    capture("quality-wide");
    panel_width = 450.f;
    capture("quality-narrow");
    panel_width = 1400.f;
    runtime::ui_menu_page = 1;
    runtime::ui_selected_pass = 1;
    ui::expand_all_sections = true;
    runtime::pass_look_result[0].follow = false;
    runtime::pass_look_result[0].mode = true;
    capture("performance-wide");
    panel_width = 450.f;
    capture("multipass-narrow");
    panel_width = 1400.f;
    runtime::ui_menu_page = 2;
    capture("diagnostics");
    runtime::ui_menu_page = 3;
    capture("advanced");
    // The preset modal is the production implementation. No personal library
    // or clipboard is read: seed only synthetic worker results and documents.
    preset_fixture = true;
    namespace presets = renodx::addons::dlss5::presets;
    namespace manager = runtime::preset_ui;
    // Trace the production input -> snapshot -> worker -> file -> scan ->
    // reload flow with exact UTF-8 bytes, independently of rendered glyphs.
    const auto unicode_root = std::filesystem::current_path() / L"unicode-runtime";
    const auto unicode_local = unicode_root / L"中文用户资料";
    std::filesystem::create_directories(unicode_local);
    check(SetEnvironmentVariableW(L"LOCALAPPDATA", unicode_local.c_str()) != 0, "set isolated Unicode preset library");
    manager::mode = 1;
    manager::save_options = manager::export_options = true;
    runtime::ui_language_choice = 1;
    panel_width = 1400.f;
    panel_height = 1600.f;
    io.DisplaySize = ImVec2(panel_width, panel_height);
    font_size = 18.f;
    const auto input_text = [&](const char* label, const char* text, const char* buffer) {
      frame(); frame();
      click(find(label));
      io.AddKeyEvent(ImGuiMod_Ctrl, true);
      io.AddKeyEvent(ImGuiKey_A, true);
      frame();
      io.AddKeyEvent(ImGuiKey_A, false);
      io.AddKeyEvent(ImGuiMod_Ctrl, false);
      frame();
      io.AddInputCharactersUTF8(text);
      frame(); frame();
      check(std::string(buffer) == text, "ImGui text input changed Unicode bytes");
    };
    const auto finish_job = [&]() {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      for (;;) {
        bool busy;
        {
          std::shared_lock lock(manager::worker.mutex);
          busy = manager::worker.busy;
        }
        if (!busy) break;
        check(std::chrono::steady_clock::now() < deadline, "preset Unicode worker timed out");
        Sleep(1);
      }
      manager::Pump();
      check(manager::worker.error.empty(), "preset Unicode worker failed");
    };
    const auto assisted_entry = [&](const char* label, char* buffer, bool context_menu) {
      namespace native = renodx::addons::dlss5::unicode_input;
      const auto original = std::string(buffer);
      frame(); frame();
      const auto field = find(label);
      if (context_menu) {
        click(field);
        const ImVec2 center = items.at(field.id).rect.GetCenter();
        io.AddMousePosEvent(center.x, center.y);
        io.AddMouseButtonEvent(1, true); frame();
        io.AddMouseButtonEvent(1, false); frame(); frame();
        click(find("Assisted input..."));
      } else {
        const auto button = ImHashStr("###assisted_input", 0, field.id);
        check(items.contains(button), "each input has a unique associated assistance button");
        click(Item(items.at(button)));
      }
      check(native::Busy(), "button and context menu start the existing native worker");
      check(ui::text_input::pending.field == field.id, "both entry points retain the original field ID");
      HWND editor = nullptr;
      DWORD thread = 0;
      {
        std::shared_lock lock(native::internal::state.mutex);
        thread = GetThreadId(native::internal::state.worker);
      }
      for (int attempt = 0; attempt < 1000; ++attempt) {
        EnumThreadWindows(thread, [](HWND window, LPARAM out) -> BOOL {
          if (GetDlgItem(window, 1001) == nullptr) return TRUE;
          *reinterpret_cast<HWND*>(out) = window;
          return FALSE;
        }, reinterpret_cast<LPARAM>(&editor));
        HWND edit = (editor == nullptr ? nullptr : GetDlgItem(editor, native::internal::TEXT_ID));
        if (edit != nullptr && SendMessageW(edit, EM_GETLIMITTEXT, 0, 0) == native::internal::MAX_EDIT_UNITS) break;
        editor = nullptr;
        Sleep(1);
      }
      check(editor != nullptr && IsWindowUnicode(editor) && !IsWindowVisible(editor),
            "entry uses the real Unicode editor while the test keeps it hidden");
      const std::string value = "辅助中文 English123";
      check(SetWindowTextW(GetDlgItem(editor, native::internal::TEXT_ID), L"辅助中文 English123"),
            "set only synthetic native editor text");
      SendMessageW(editor, WM_COMMAND, context_menu ? IDCANCEL : native::internal::APPLY_ID, 0);
      for (int attempt = 0; attempt < 1000 && native::Busy(); ++attempt) { frame(); Sleep(1); }
      frame(); frame();
      check(!native::Busy(), "assisted editor result is consumed without leaving a worker");
      check(std::string(buffer) == (context_menu ? original : value),
            "button Apply returns text and context-menu Cancel preserves the original");
    };
    frame(); frame();
    const auto form_window = find("##Preset name").window;
    const auto hint_before = std::find_if(rendered_text.begin(), rendered_text.end(), [&](const auto& text) {
      return text.window == form_window;
    });
    check(hint_before != rendered_text.end() && std::next(hint_before) != rendered_text.end(), "top instruction exposes its noninteractive text/icon row");
    const auto& hint_after = *std::next(hint_before);
    click({.rect = ImRect(ImVec2(hint_before->rect.Max.x, hint_before->rect.Min.y),
                          ImVec2(hint_after.rect.Min.x, hint_after.rect.Max.y)), .window = form_window});
    check(!renodx::addons::dlss5::unicode_input::Busy() && ui::text_input::pending.field == 0,
          "clicking the explanatory keyboard graphic never starts an editor");
    const auto icon_id = ImHashStr("###assisted_input", 0, find("##Preset name").id);
    hover(Item(items.at(icon_id)), "Assisted input: Open the compatibility text editor for Chinese and other text.", nullptr);
    capture("assisted-keyboard-idle-en", nullptr, false);
    const ImVec2 icon_center = items.at(icon_id).rect.GetCenter();
    io.AddMousePosEvent(icon_center.x, icon_center.y);
    for (int attempt = 0; attempt < 40; ++attempt) frame();
    capture("assisted-keyboard-hover-en", nullptr, false);
    io.AddMousePosEvent(-100.f, -100.f);
    frame();
    for (const auto& [label, buffer] : {std::pair{"##Preset name", manager::name},
                                      std::pair{"##Author (optional)", manager::author},
                                      std::pair{"##Source game (optional)", manager::source_game},
                                      std::pair{"##preset_description", manager::description}}) {
      assisted_entry(label, buffer, false);
      assisted_entry(label, buffer, true);
    }
    input_text("##Preset name", "电影画质_日本語_한국어_é", manager::name);
    input_text("##Author (optional)", "作者_Élodie_日本", manager::author);
    input_text("##Source game (optional)", "巫师3_Witcher_日本語", manager::source_game);
    input_text("##preset_description", "中文描述，混合 café／한글／e\xcc\x81", manager::description);
    // ReShade feeds WM_CHAR through AddInputCharacterUTF16. Exercise that
    // boundary too, rather than relying only on the UTF-8 injection helper.
    click(find("##Preset name"));
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_A, true);
    frame();
    io.AddKeyEvent(ImGuiKey_A, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    frame();
    for (const wchar_t character : std::wstring_view(L"中文电影_日本語_한국어_é")) io.AddInputCharacterUTF16(character);
    frame(); frame();
    check(std::string(manager::name) == "中文电影_日本語_한국어_é", "ReShade UTF-16 input boundary changed Chinese bytes");
    WNDCLASSA message_probe_class{};
    message_probe_class.lpfnWndProc = MessageProbeProcedure;
    message_probe_class.hInstance = GetModuleHandleW(nullptr);
    message_probe_class.lpszClassName = "NR-Nexus-Unicode-Input-Test";
    check(RegisterClassA(&message_probe_class) != 0, "register isolated ANSI input window");
    message_probe_window = CreateWindowExA(0, message_probe_class.lpszClassName, "Unicode fixture", 0,
                                          0, 0, 100, 100, nullptr, nullptr, message_probe_class.hInstance, nullptr);
    check(message_probe_window != nullptr && !IsWindowUnicode(message_probe_window), "create hidden ANSI input window");
    const auto message_hook = SetWindowsHookExW(WH_GETMESSAGE, ObserveUnicodeMessage, nullptr, GetCurrentThreadId());
    check(message_hook != nullptr, "install current-test-thread Unicode message observer");
    std::wstring ansi_message_units;
    for (const wchar_t unit : std::wstring_view(L"中文")) {
      check(PostMessageW(message_probe_window, WM_CHAR, unit, 1) != FALSE, "post Unicode character to ANSI window");
    }
    MSG message{};
    while (PeekMessageA(&message, message_probe_window, 0, 0, PM_REMOVE)) {
      if (message.message == WM_CHAR) ansi_message_units += static_cast<wchar_t>(message.wParam);
    }
    check(message_probe_unicode == L"中文" && ansi_message_units != message_probe_unicode,
          "Windows ANSI retrieval must reproduce loss before ReShade's UTF-16 cast");
    check(UnhookWindowsHookEx(message_hook) != FALSE, "remove test Unicode message observer");
    check(DestroyWindow(message_probe_window) != FALSE, "destroy hidden message probe");
    message_probe_window = nullptr;
    check(UnregisterClassA(message_probe_class.lpszClassName, message_probe_class.hInstance) != FALSE,
          "unregister message probe class");
    const auto replace_name_from_utf16 = [&](std::wstring_view value) {
      click(find("##Preset name"));
      io.AddKeyEvent(ImGuiMod_Ctrl, true);
      io.AddKeyEvent(ImGuiKey_A, true);
      frame();
      io.AddKeyEvent(ImGuiKey_A, false);
      io.AddKeyEvent(ImGuiMod_Ctrl, false);
      frame();
      for (const wchar_t unit : value) io.AddInputCharacterUTF16(unit);
      frame(); frame();
    };
    replace_name_from_utf16(ansi_message_units);
    check(std::string(manager::name) != "中文", "ANSI bytes interpreted as UTF-16 must reproduce visible mojibake");
    const std::string ansi_mojibake(manager::name);
    replace_name_from_utf16(message_probe_unicode);
    check(std::string(manager::name) == "中文", "Unicode captured before ANSI conversion must preserve inline input");
    replace_name_from_utf16(L"中文电影_日本語_한국어_é");
    // Exercise the real native worker -> production widget return path. Keep
    // test windows hidden, and never touch the user's clipboard or IME state.
    const auto accept_native_text = [&](const char* value, bool change_original) {
      namespace native = renodx::addons::dlss5::unicode_input;
      namespace input = renodx::addons::dlss5::ui::text_input;
      const auto item = find("##Preset name");
      const auto field = item.window->GetID("##Preset name");
      input::pending = {.field = field, .context = reinterpret_cast<std::uintptr_t>(&io),
                        .initial = manager::name};
      const std::string title = "NR Nexus hidden input integration " + std::to_string(GetCurrentProcessId());
      check(native::internal::StartEditor({.field = field, .value = manager::name, .title = title,
                .label = "Synthetic name", .apply_label = "Apply", .cancel_label = "Cancel",
                .invalid_label = "Invalid", .max_bytes = sizeof(manager::name) - 1}, false),
            "start hidden Unicode editor from production input transaction");
      frame();  // Retire ImGui's active edit state while native editing is busy.
      std::wstring wide_title;
      check(native::internal::Utf8ToWide(title, &wide_title), "native integration title conversion");
      HWND editor = nullptr;
      for (int attempt = 0; attempt < 1000; ++attempt) {
        editor = FindWindowW(nullptr, wide_title.c_str());
        // FindWindow can see the dialog before WM_INITDIALOG has finished.
        // The production text limit is set after its initial text/subclass.
        HWND edit = (editor == nullptr ? nullptr : GetDlgItem(editor, native::internal::TEXT_ID));
        if (edit != nullptr && SendMessageW(edit, EM_GETLIMITTEXT, 0, 0) == native::internal::MAX_EDIT_UNITS) break;
        editor = nullptr;
        Sleep(1);
      }
      check(editor != nullptr, "hidden editor created on its worker thread");
      if (editor != nullptr) {
        std::wstring wide_value;
        check(native::internal::Utf8ToWide(value, &wide_value), "native integration value conversion");
        check(SetDlgItemTextW(editor, native::internal::TEXT_ID, wide_value.c_str()) != FALSE,
              "set synthetic native text");
        if (change_original) std::strcpy(manager::name, "changed meanwhile");
        SendMessageW(editor, WM_COMMAND, native::internal::APPLY_ID, 0);
      }
      for (int attempt = 0; attempt < 1000 && native::Busy(); ++attempt) Sleep(1);
      check(!native::Busy(), "native worker completed before polling its result");
      frame(); frame();
      check(std::string(manager::name) == (change_original ? "changed meanwhile" : value),
            "native return must update its field once and preserve intervening edits");
      check(input::pending.field == 0, "native widget transaction consumed exactly once");
      click(find("##Preset name"));
      frame();
      check(std::string(manager::name) == (change_original ? "changed meanwhile" : value),
            "refocusing inline input must not revive its previous ImGui edit state");
    };
    accept_native_text("中文测试", false);
    accept_native_text("你好世界", true);
    replace_name_from_utf16(L"中文电影_日本語_한국어_é");
    const Json unicode_metadata{{"name", manager::name}, {"author", manager::author},
                                {"description", manager::description}, {"source_game", manager::source_game},
                                {"created_with", NR_NEXUS_NAME " " NR_NEXUS_VERSION}};
    const auto unicode_document = manager::Capture(presets::Scope::QUALITY, unicode_metadata);
    check(unicode_document.metadata == unicode_metadata, "preset capture changed UTF-8 metadata");
    presets::Document unicode_parsed;
    std::string unicode_error;
    const auto unicode_json = presets::Serialize(unicode_document);
    check(presets::Parse(unicode_json, &unicode_parsed, &unicode_error) && unicode_parsed.metadata == unicode_metadata,
          "preset JSON Unicode round trip changed metadata");
    check(runtime::CopyReportText(unicode_json), "Unicode share text clipboard conversion failed");
    check(clipboard_fixture::contents != nullptr, "Unicode share text did not reach CF_UNICODETEXT");
    const auto* unicode_clipboard = static_cast<const wchar_t*>(GlobalLock(clipboard_fixture::contents));
    check(unicode_clipboard != nullptr, "Unicode clipboard could not be inspected");
    const int unicode_clipboard_bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, unicode_clipboard, -1, nullptr, 0, nullptr, nullptr);
    std::string unicode_clipboard_json(unicode_clipboard_bytes, '\0');
    check(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, unicode_clipboard, -1, unicode_clipboard_json.data(), unicode_clipboard_bytes, nullptr, nullptr)
              == unicode_clipboard_bytes, "share text UTF-16 encoding is invalid");
    GlobalUnlock(clipboard_fixture::contents);
    unicode_clipboard_json.pop_back();
    check(presets::Parse(unicode_clipboard_json, &unicode_parsed, &unicode_error) && unicode_parsed.metadata == unicode_metadata,
          "share clipboard changed Unicode metadata");
    click(find("save_preset"));
    finish_job();
    check(manager::active_name == unicode_metadata["name"].get<std::string>(), "saved active name changed Unicode bytes");
    manager::Start(manager::Job::REFRESH);
    finish_job();
    check(manager::files.size() == 1 && manager::files[0].native() == L"中文电影_日本語_한국어_é.nrnexus.json",
          "Unicode filename changed between save and scan");
    const auto unicode_filename = manager::files[0].u8string();
    check(std::string(reinterpret_cast<const char*>(unicode_filename.data()), unicode_filename.size())
              == unicode_metadata["name"].get<std::string>() + ".nrnexus.json", "library label changed Unicode bytes");
    manager::Start(manager::Job::READ, manager::files[0]);
    finish_job();
    check(manager::preview.metadata == unicode_metadata, "reload changed Unicode metadata");
    manager::mode = 1;
    const auto unicode_export_directory = unicode_root / L"中文导出路径_日本語";
    std::filesystem::create_directory(unicode_export_directory);
    const auto unicode_export_path = unicode_export_directory / L"混合_é_한글.nrnexus.json";
    const auto unicode_path_bytes = unicode_export_path.u8string();
    const std::string unicode_path_text(reinterpret_cast<const char*>(unicode_path_bytes.data()), unicode_path_bytes.size());
    frame(); frame();
    for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows) {
      if (std::strstr(window->Name, "nr_preset_manager/##preset_content_") != nullptr) ImGui::SetScrollY(window, window->ScrollMax.y);
    }
    frame(); frame();
    input_text("##Export file path", unicode_path_text.c_str(), manager::path);
    click(find("export_preset"));
    finish_job();
    check(presets::ReadFile(unicode_export_path, &unicode_parsed, &unicode_error) && unicode_parsed.metadata == unicode_metadata,
          "Unicode export path input or worker changed the filename/metadata");
    const auto show_library_import_actions = [&]() {
      manager::mode = 2;
      frame(); frame();
      if (!manager::import_path_options) click(find("Enter file path"));
      for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows) {
        if (std::strstr(window->Name, "nr_preset_manager/##preset_content_") != nullptr) {
          ImGui::SetScrollY(window, 0.f);
        }
      }
      frame(); frame();
    };
    show_library_import_actions();
    click(find("read_preset"));
    finish_job();
    check(manager::preview.metadata == unicode_metadata, "explicit Unicode path import changed metadata");
    show_library_import_actions();
    click(find("paste_preset"));
    finish_job();
    check(manager::preview.metadata == unicode_metadata, "native clipboard import changed Unicode metadata");
    clipboard_fixture::Empty();
    check(io.Fonts->Fonts[0]->IsGlyphInFont(0x4E2D) && !latin_only_font->IsGlyphInFont(0x4E2D),
          "font fixture must distinguish Chinese coverage from missing-glyph display");
    manager::mode = 2;
    frame(); frame();
    for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows) {
      if (std::strstr(window->Name, "nr_preset_manager/##preset_content_") != nullptr) ImGui::SetScrollY(window, 0.f);
    }
    capture("unicode-covered-font");
    preset_latin_font = true;
    capture("unicode-missing-font");
    check(manager::preview.metadata == unicode_metadata, "font fallback must not rewrite preset data");
    preset_latin_font = false;
    std::ofstream("unicode-stages.json", std::ios::binary)
        << Json{{"input", unicode_metadata}, {"json", unicode_parsed.metadata}, {"reloaded", manager::preview.metadata},
                {"filename", std::string(reinterpret_cast<const char*>(unicode_filename.data()), unicode_filename.size())},
                {"covered_font_has_chinese", true}, {"latin_font_has_chinese", false},
                {"ansi_retrieval_as_utf16", ansi_mojibake}, {"windows_ansi_codepage", GetACP()},
                {"real_os_ime_composition", "not-tested"},
                {"imgui_codepoint_max", IM_UNICODE_CODEPOINT_MAX}}.dump(2);
    manager::files.clear();
    manager::active_name.clear();
    manager::active_settings.clear();
    manager::name[0] = manager::author[0] = manager::description[0] = manager::source_game[0] = '\0';
    std::puts("Unicode production trace passed: ImGui input, metadata, JSON, save, scan, UTF-8 label, reload");
    runtime::ui_classic_layout = false;
    manager::files = {L"Cinematic A+B.nrnexus.json", L"Long cinematic neural rendering sample name.nrnexus.json"};
    manager::selected = manager::files.front();
    manager::preview = {.metadata = {{"name", "Cinematic A+B"}, {"description", "Independent models and result settings"}},
                        .scope = presets::Scope::QUALITY_MULTIPASS,
                        .settings = {{"NRPreset", 1}, {"NRPasses", 2}, {"NRPass2Preset", 2},
                                     {"NRIntensity", 1.25}, {"NRPass2FollowPass1", false},
                                     {"NRPass2LookFollowPass1", false}, {"NRPass2LookColour", 0.75},
                                     {"FutureSetting", true}}};
    manager::have_preview = true;
    manager::rows = manager::Preview(manager::preview, false);
    std::strcpy(manager::name, "Cinematic A+B");
    std::strcpy(manager::author, "Example");
    std::strcpy(manager::description, "Independent models and result settings");
    runtime::profile_ui::state.slots[3].name = "A long custom profile name with literal ## and ### markers";
    ui::expand_all_sections = true;
    unsigned assisted_layout_cases = 0;
    const auto check_assisted_layout = [&]() {
      for (const char* title : {"Preset name", "Author (optional)", "Source game (optional)", "Description (optional)"}) {
        const std::string label = (std::string(title) == "Description (optional)"
                                      ? "##preset_description" : std::string("##") + ui::Tr(title));
        const auto field = find(label);
        const auto id = ImHashStr("###assisted_input", 0, field.id);
        check(items.contains(id), "localized assistance button stays associated with its field");
        const auto& button = items.at(id);
        check(button.window == field.window, "header button remains in the field's form");
        check(button.rect.Min.x >= field.rect.Min.x - 1.f && button.rect.Max.x <= field.rect.Max.x + 2.f,
              "assistance button fits inside the field width");
        const auto heading = std::find_if(rendered_text.begin(), rendered_text.end(), [&](const auto& text) {
          return text.window == button.window && std::fabs(text.rect.Min.y - button.rect.Min.y) <= 0.5f
                 && text.rect.Max.x <= button.rect.Min.x;
        });
        check(heading != rendered_text.end() && button.rect.Min.x - heading->rect.Max.x <= font_size * 0.5f + 1.f,
              "keyboard button stays immediately beside the title");
        check(button.rect.GetWidth() <= font_size * 1.3f + 0.5f, "assistance entry keeps compact icon dimensions");
        // Vertically clipped buttons have bounds but no ItemInfo label.
        check(button.label.empty() || button.label == "###assisted_input", "assistance button has no visible label text");
        check(button.rect.Max.y <= field.rect.Min.y && field.rect.Min.y - button.rect.Min.y <= button.rect.GetHeight() * 1.5f + 2.f,
              "assistance button shares the title row without adding a second row");
        check(button.rect.GetHeight() <= font_size + 0.5f, "assistance button uses a compact single text row");
      }
      const std::string hint = ui::Tr("For Chinese and other text, click %s beside the field.");
      const auto marker = hint.find("%s");
      check(marker != std::string::npos && hint.find("%s", marker + 2) == std::string::npos,
            "each translated single-line hint has one graphical icon placeholder");
      const auto form = find(std::string("##") + ui::Tr("Preset name")).window;
      unsigned count = 0;
      auto before = rendered_text.end();
      for (auto text = rendered_text.begin(); text != rendered_text.end() && std::next(text) != rendered_text.end(); ++text) {
        const auto& next = *std::next(text);
        if (text->window == form && next.window == form
            && std::fabs(text->rect.Min.y - next.rect.Min.y) <= 0.5f
            && std::fabs(next.rect.Min.x - text->rect.Max.x - font_size * 1.25f) <= 1.f) {
          ++count;
          before = text;
        }
      }
      check(count == 1, "the editing area shows the assistance hint exactly once");
      check(before != rendered_text.end() && std::next(before) != rendered_text.end(), "single-line hint has text on both sides of its static icon");
      const auto& after = *std::next(before);
      std::string before_text = before->text;
      std::string after_text = after.text;
      if (before_text.ends_with("...")) before_text.resize(before_text.size() - 3);
      if (after_text.ends_with("...")) after_text.resize(after_text.size() - 3);
      check(hint.substr(0, marker).starts_with(before_text) && hint.substr(marker + 2).starts_with(after_text),
            "hint preserves localized sentence order around the graphical placeholder");
      check(std::fabs(before->rect.Min.y - after.rect.Min.y) <= 0.5f, "hint remains on one line at every tested width and language");
      check(std::fabs(after.rect.Min.x - before->rect.Max.x - font_size * 1.25f) <= 1.f,
            "hint reserves the same keyboard geometry width as the field button");
      ++assisted_layout_cases;
    };
    for (const auto& theme : ui::kThemes) {
      runtime::ui_theme_choice = theme.id;
      for (const int language : {1, 14}) {
        runtime::ui_language_choice = language;
        for (int view = 0; view < 3; ++view) {
          manager::mode = view;
          panel_width = 1100.f;
          panel_height = 1250.f;
          io.DisplaySize = ImVec2(panel_width, panel_height);
          font_size = 18.f;
          capture((std::string("presets-") + theme.slug + "-" + (language == 14 ? "zh-" : "en-") + std::to_string(view)).c_str());
          if (view == 1) check_assisted_layout();
          check(ImGui::GetCurrentContext()->OpenPopupStack.Size == 1, "preset manager did not open as a modal");
        }
      }
    }
    for (int language = 1; language <= ui::kLanguageCount; ++language) {
      runtime::ui_language_choice = language;
      panel_width = 450.f;
      panel_height = 1300.f;
      io.DisplaySize = ImVec2(panel_width, panel_height);
      font_size = 24.f;
      for (int view = 0; view < 3; ++view) {
        manager::mode = view;
        frame(); frame(); frame();
        if (view == 1) check_assisted_layout();
      }
    }
    panel_width = 320.f;
    font_size = 32.f;
    io.DisplaySize = ImVec2(panel_width, panel_height);
    manager::mode = 1;
    for (int language = 1; language <= ui::kLanguageCount; ++language) {
      runtime::ui_language_choice = language;
      frame(); frame(); frame();
      check_assisted_layout();
    }
    runtime::ui_language_choice = 14;
    capture("assisted-presets-narrow-zh");
    panel_width = 450.f;
    font_size = 24.f;
    io.DisplaySize = ImVec2(panel_width, panel_height);
    runtime::ui_language_choice = 14;
    manager::mode = 2;
    capture("presets-narrow-zh");
    panel_width = 3840.f;
    panel_height = 2160.f;
    io.DisplaySize = ImVec2(panel_width, panel_height);
    font_size = 32.f;
    frame(); frame();
    capture("presets-4k-zh");
    std::printf("preset modal: 7 themes, 15 languages, library/export/preview, narrow/4K captures; synthetic environment\n");
    std::printf("assisted input: eight real hidden entry/Apply/Cancel flows; %u localized header layout cases passed\n", assisted_layout_cases);
    std::printf("production menu checks=%u failures=0; category/pass/model/reset/persistence/classic/language/size matrix passed\n", checks);
    std::printf("production menu CPU mean=%.3f ms, frames=%u; includes synthetic test hooks and language changes, excludes GPU rendering\n",
                panel_total_ms / panel_frames, panel_frames);
#include "preset_ux_cases.hpp"
    std::printf("capture retention=%s meshes=%u atlases=%u\n", retention.c_str(), capture_count, atlas_count);
    if (std::getenv("NR_NEXUS_TEST_CAPTURE_FAILURE") != nullptr) check(false, "controlled capture retention failure");
    if (std::getenv("NR_NEXUS_DOC_CAPTURES") != nullptr) {
      // Keep the legacy opt-in's regression coverage. Only the preview entry
      // takes the fast path; both callers share the same production scenes.
      preset_fixture = false;
      preset_latin_font = false;
      clipped_callback = false;
#include "ui_snapshot_cases.hpp"
    }
    ImGui::DestroyContext();
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
