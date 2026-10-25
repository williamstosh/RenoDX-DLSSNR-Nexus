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
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
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
  TextDisabledV(fmt, args);
  va_end(args);
  rendered_text.push_back({.text = fmt, .rect = ImRect(GetItemRectMin(), GetItemRectMax()), .window = GetCurrentWindow()});
}
inline void ObservedTextWrapped(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  TextWrappedV(fmt, args);
  va_end(args);
  rendered_text.push_back({.text = fmt, .rect = ImRect(GetItemRectMin(), GetItemRectMax()), .window = GetCurrentWindow()});
}
}  // namespace ImGui
#define TextUnformatted ObservedTextUnformatted
#define Text            ObservedText
#define TextDisabled    ObservedTextDisabled
#define TextWrapped     ObservedTextWrapped
#define TextV           ObservedTextV
#undef IMGUI_VERSION_NUM
#include "dlssnr.hpp"
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

struct Item {
  ImGuiID id = 0;
  ImRect rect;
  std::string label;
  bool disabled = false;
  ImGuiWindow* window = nullptr;
};
std::map<ImGuiID, Item> items;

void ImGuiTestEngineHook_ItemAdd(ImGuiContext* context, ImGuiID id, const ImRect& rect, const ImGuiLastItemData*) {
  items[id] = {.id = id, .rect = rect, .disabled = (context->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0, .window = context->CurrentWindow};
  // BeginCombo does not emit ItemInfo in the pinned ImGui version.
  if (context->CurrentWindow != nullptr && !context->CurrentWindow->IDStack.empty()) {
    for (const char* label : {"##model", "##theme"}) {
      if (id == context->CurrentWindow->GetID(label)) {
        items[id].label = label;
      }
    }
  }
}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags) {
  items[id].label = label;
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
  // ReShade distinguishes a missing key from a present empty value. The
  // Win32 count alone is zero for both, so retain a missing-key sentinel.
  const DWORD count = GetPrivateProfileStringA(section, key, "\x01", buffer, sizeof(buffer), ".\\ReShade.ini");
  if (count == 1 && buffer[0] == '\x01') {
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
extern "C" __declspec(dllexport) void ReShadeSetConfigValue(
    void*, reshade::api::effect_runtime*, const char* section, const char* key, const char* value) {
  if (!WritePrivateProfileStringA(section, key, value, ".\\ReShade.ini")) {
    std::abort();
  }
}

int main() {
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
    unsigned char* atlas;
    int atlas_width, atlas_height;
    io.Fonts->GetTexDataAsRGBA32(&atlas, &atlas_width, &atlas_height);
    io.Fonts->SetTexID(1);
    runtime::ui_welcomed = true;
    runtime::ui_language_choice = 1;
    runtime::direct_runtime_requirements.file_version = "310.9.16.0";
    runtime::direct_runtime_requirements.dual_models = true;
    runtime::last_input_width = runtime::last_output_width = 3840;
    runtime::last_input_height = runtime::last_output_height = 2160;
    runtime::dlss_render_share = 4.f / 9.f;
    unsigned checks = 0;
    const auto check = [&](bool condition, const char* message) {
      ++checks;
      if (!condition) {
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
    load_theme_case((section + "; empty section\n").c_str(), ui::ThemeId::CYBER_NEON);
    load_theme_case((section + "NRIntensity=1.3\n").c_str(), ui::ThemeId::NEXUS_DARK);
    load_theme_case((section + "ConfigVersion=8\n").c_str(), ui::ThemeId::NEXUS_DARK);
    load_theme_case((section + "UiTheme=invalid-theme\nNRIntensity=1.3\n").c_str(), ui::ThemeId::CYBER_NEON);
    load_theme_case((section + "UiTheme=\n").c_str(), ui::ThemeId::CYBER_NEON);
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
    load_theme_case("", ui::ThemeId::CYBER_NEON);
    runtime::ui_welcomed = true;
    runtime::ui_language_choice = 1;
    float panel_width = 1000.f;
    float font_size = 18.f;
    double panel_total_ms = 0.0;
    unsigned panel_frames = 0;
    bool clipped_callback = false;
    const auto frame = [&](const ui::StatusCard* diagnostic_case = nullptr) {
      items.clear();
      rendered_text.clear();
      tooltip_text.clear();
      ImGui::GetStyle().FontSizeBase = font_size;
      ImGui::NewFrame();
      ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
      ImGui::SetNextWindowSize(ImVec2(panel_width, 1100.f));
      ImGui::Begin("NR Nexus menu fixture", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);
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
      if (diagnostic_case != nullptr) {
        ui::StyleScope theme;
        ui::PushPanelTheme(theme);
        runtime::DrawRuntimeStatus(*diagnostic_case);
      } else {
        runtime::OnOverlay(nullptr);
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
        if (window->Active && (window->Flags & ImGuiWindowFlags_ChildWindow) != 0
            && std::strstr(window->Name, "NR Nexus menu fixture") != nullptr) {
          const float overflow = window->DC.CursorMaxPos.x - (window->Pos.x + window->Size.x - window->WindowPadding.x - window->ScrollbarSizes.x);
          if (overflow > 1.f) {
            std::printf("overflow: window=%s width=%.0f font=%.0f page=%d classic=%d scroll_x=%.1f\n",
                        window->Name, panel_width, font_size, runtime::ui_menu_page,
                        runtime::ui_classic_layout.load(), overflow);
            for (const auto& [id, item] : items) {
              if (item.window == window && item.rect.Max.x > window->InnerRect.Max.x) {
                std::printf("  item=%s rect_max_x=%.1f right=%.1f language=%d\n", item.label.c_str(),
                            item.rect.Max.x, window->InnerRect.Max.x, runtime::ui_language_choice.load());
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
      throw std::runtime_error("menu item missing: " + label);
    };
    const auto click = [&](const Item& item) {
      ImVec2 center = item.rect.GetCenter();
      if (item.window != nullptr && (center.y < item.window->InnerRect.Min.y || center.y > item.window->InnerRect.Max.y)) {
        ImGui::SetScrollY(item.window, item.window->Scroll.y + center.y - item.window->InnerRect.GetCenter().y);
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
      if (center.y < item.window->InnerRect.Min.y || center.y > item.window->InnerRect.Max.y) {
        ImGui::SetScrollY(item.window, item.window->Scroll.y + center.y - item.window->InnerRect.GetCenter().y);
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
    const auto capture = [&](const char* name, const ui::StatusCard* diagnostic_case = nullptr, bool settle = true) {
      // Store the actual ImGui mesh and atlas; the optional CPU renderer uses
      // these without substituting HTML controls or a second menu definition.
      if (settle) {
        frame(diagnostic_case);
        frame(diagnostic_case);
        frame(diagnostic_case);
      }
      Json output{{"width", panel_width}, {"height", 1100}, {"theme", ui::CurrentTheme().slug},
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
      std::ofstream(std::string(name) + ".json") << output;
      std::ofstream texture(std::string(name) + ".rgba", std::ios::binary);
      texture.write(reinterpret_cast<char*>(atlas), static_cast<std::streamsize>(atlas_width) * atlas_height * 4);
    };

    frame();
    frame();
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
    check(runtime::ui_menu_page == 2, "status entry did not select Diagnostics");
    for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows) {
      check(!window->Active || (window->Flags & ImGuiWindowFlags_Popup) == 0, "status navigation opened a floating popup");
    }
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
    click(find("Built-in effect"));
    check(runtime::ui_defaults_view && runtime::kSetIntensity.Get() == runtime::kSetIntensity.fallback,
          "Built-in effect did not temporarily select stock values");
    click(find("Mine"));
    check(!runtime::ui_defaults_view && runtime::kSetIntensity.Get() == 1.3f,
          "Mine did not restore the player's values");
    check(runtime::ui_theme_choice.load() == ui::ThemeId::FLUENT_LIGHT,
          "quality preset comparison changed the selected theme");
    runtime::kSetIntensity.Store(own_intensity);
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
      click(find(label));
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
    check(fold_open(ui::text::kResolutionFine) && fold_open(ui::text::kResultLimits),
          "expand all did not reach nested Quality groups");
    click(find("collapse_all"));
    frame();
    check(!fold_open(ui::text::kResolutionFine) && !fold_open(ui::text::kResultLimits),
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

    click(find("Balanced"));
    hover(find("Balanced"), ui::text::kResBalancedTip, ui::text::kResQualityTip);
    check(!runtime::nr_follow_input_res.load() && std::abs(runtime::nr_resolution_scale.load() - 0.58f) < 0.001f,
          "resolution preset did not select its original parameter pair");
    click(find("Match game"));
    check(runtime::nr_follow_input_res.load(), "Match game did not select follow-input mode");
    runtime::nr_follow_input_res = false;
    runtime::nr_resolution_scale = 0.75f;
    frame();
    check(runtime::nr_resolution_scale.load() == 0.75f, "drawing custom resolution changed its saved scale");
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
    for (unsigned count = 1; count <= 4; ++count) {
      runtime::stack_passes = count;
      frame();
      frame();
      unsigned nodes = 0;
      for (const auto& [id, item] : items) {
        if (item.label.ends_with("###chain_pass")) {
          ++nodes;
          check(std::count(item.label.begin(), item.label.end(), '\n') == 2,
                "processing node must retain three stable lines");
          check(item.label.find(ui::text::kEditing) != std::string::npos
                    || item.label.find(ui::text::kConfigured) != std::string::npos,
                "processing nodes must describe configuration/editor state, not inferred execution");
        }
      }
      check(nodes == count, "processing chain must include the single base pass");
      for (unsigned pass = 1; pass <= count; ++pass) {
        click(find("Pass " + std::to_string(pass)));
        check(runtime::ui_selected_pass == pass - 1, "chain selects the wrong editor");
      }
    }
    runtime::stack_passes = 4;
    frame();
    frame();
    const std::string chain_tip = std::string("Pass 4\n") + ui::text::kRequest + ": " + ui::text::kInheritModel
        + "\n" + ui::text::kInheritModel
        + "\n" + (runtime::pass_follow[2].load() ? ui::text::kStyleFollows : ui::text::kStyleIndependent)
        + "\n" + (runtime::pass_look_result[2].follow.load() ? ui::text::kResultFollows : ui::text::kResultIndependent)
        + "\n" + ui::text::kRuntimeUnconfirmed + "\n" + ui::text::kChainMarkTip;
    hover(find("Pass 4"), chain_tip.c_str(), nullptr);
    click(find("Pass 2"));
    check(runtime::ui_selected_pass == 1, "pass navigation failed");
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
    click(find("Pass 2"));
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
    click(find(ui::Tr(ui::text::kRestoreAll) + std::string("###") + ui::text::kRestoreAll));
    click(find(ui::text::kRestoreAll));
    click(find("Restore"));
    check(runtime::stack_passes.load() == 1 && runtime::kSetPassPreset[0].Get() == 0.f,
          "confirmed restore defaults did not reset pass settings");
    check(runtime::ui_theme_choice.load() == ui::ThemeId::FLUENT_LIGHT,
          "global quality reset changed the selected theme");
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
                    if (window->Active && std::strstr(window->Name, "##nr_settings") != nullptr) {
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
                  check(std::abs(settings->Pos.y - anchor) <= 0.01f, "live refresh moved production settings scroller");
                }
              }
            }
          }
        }
      }
    }
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
    check(themed_captures == 80, "four themes must each capture four pages, both widths/fold states, faults, popup and tooltip");
    std::printf("all-theme mesh captures=%u; synthetic fixture, no game/GPU/visual acceptance inferred\n", themed_captures);
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
    std::printf("production menu checks=%u failures=0; category/pass/model/reset/persistence/classic/language/size matrix passed\n", checks);
    std::printf("production menu CPU mean=%.3f ms, frames=%u; includes synthetic test hooks and language changes, excludes GPU rendering\n",
                panel_total_ms / panel_frames, panel_frames);
    ImGui::DestroyContext();
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
