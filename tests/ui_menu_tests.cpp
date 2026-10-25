/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#define DEBUG_LEVEL_0

#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
namespace ImGui {
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
#undef IMGUI_VERSION_NUM
#include "dlssnr.hpp"
#undef TextUnformatted
#undef Text
#undef TextDisabled
#undef TextWrapped
#define IMGUI_VERSION_NUM 19250
#include <deps/imgui/imgui_internal.h>

namespace runtime = renodx::addons::dlss5::internal;
namespace ui = renodx::addons::dlss5::ui;
using Json = nlohmann::json;

struct Item {
  ImRect rect;
  std::string label;
  bool disabled = false;
  ImGuiWindow* window = nullptr;
};
std::map<ImGuiID, Item> items;

void ImGuiTestEngineHook_ItemAdd(ImGuiContext* context, ImGuiID id, const ImRect& rect, const ImGuiLastItemData*) {
  items[id] = {.rect = rect, .disabled = (context->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0, .window = context->CurrentWindow};
  // BeginCombo does not emit ItemInfo in the pinned ImGui version.
  if (context->CurrentWindow != nullptr && !context->CurrentWindow->IDStack.empty()
      && id == context->CurrentWindow->GetID("##model")) {
    items[id].label = "##model";
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
    runtime::LoadConfiguration();
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
    float panel_width = 1000.f;
    float font_size = 18.f;
    double panel_total_ms = 0.0;
    unsigned panel_frames = 0;
    const auto frame = [&]() {
      items.clear();
      rendered_text.clear();
      ImGui::GetStyle().FontSizeBase = font_size;
      ImGui::NewFrame();
      ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
      ImGui::SetNextWindowSize(ImVec2(panel_width, 1100.f));
      ImGui::Begin("NR Nexus menu fixture", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);
      const auto panel_start = std::chrono::steady_clock::now();
      runtime::OnOverlay(nullptr);
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
        if (item.label == label || item.label.starts_with(label + "\n") || item.label.ends_with("###" + label)) {
          return item;
        }
      }
      throw std::runtime_error("menu item missing: " + label);
    };
    const auto click = [&](const Item& item) {
      const ImVec2 center = item.rect.GetCenter();
      io.AddMousePosEvent(center.x, center.y);
      frame();
      io.AddMouseButtonEvent(0, true);
      frame();
      io.AddMouseButtonEvent(0, false);
      frame();
      frame();
    };
    const auto capture = [&](const char* name) {
      // Store the actual ImGui mesh and atlas; the optional CPU renderer uses
      // these without substituting HTML controls or a second menu definition.
      frame();
      frame();
      frame();
      Json output{{"width", panel_width}, {"height", 1100}, {"lists", Json::array()}};
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
    runtime::kSetIntensity.Store(1.3f);
    click(find("Built-in effect"));
    check(runtime::ui_defaults_view && runtime::kSetIntensity.Get() == runtime::kSetIntensity.fallback,
          "Built-in effect did not temporarily select stock values");
    click(find("Mine"));
    check(!runtime::ui_defaults_view && runtime::kSetIntensity.Get() == 1.3f,
          "Mine did not restore the player's values");
    runtime::kSetIntensity.Store(own_intensity);

    // A diagnostic shortcut must select the real Present controls, not a popup.
    runtime::hook_point = runtime::kHookPresent;
    runtime::ui_menu_page = 2;
    frame();
    frame();
    click(find(ui::text::kOpenPresentSettings));
    check(runtime::ui_menu_page == 0 && !runtime::ui_open_present_settings,
          "Present shortcut did not reach the Quality group");
    const auto has_text = [&](const char* value) {
      for (const auto& text : rendered_text) {
        if (text.text == value) return true;
      }
      return false;
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
    check(!has_text("Edit strength"), "disabled result shaping still draws its sliders");
    click(find(ui::text::kStoredValues));
    check(has_text("Edit strength"), "stored result values cannot be inspected");
    check(runtime::look_mode.load() == false, "inspecting stored values enabled result shaping");
    click(find(ui::text::kStoredValues));
    runtime::look_mode = true;
    frame();
    check(has_text("Edit strength"), "enabled result shaping hides its parameters");

    click(find("Multi-Pass"));
    for (unsigned count = 1; count <= 4; ++count) {
      runtime::stack_passes = count;
      frame();
      frame();
      unsigned nodes = 0;
      for (const auto& [id, item] : items) {
        if (item.label.ends_with("###chain_pass")) ++nodes;
      }
      check(nodes == count, "chain node count differs from Pass Count including the base pass");
      if (count > 1) {
        click(find("Pass " + std::to_string(count)));
        check(runtime::ui_selected_pass == count - 1, "chain selects the wrong editor");
      }
    }
    runtime::stack_passes = 4;
    frame();
    frame();
    click(find("Pass 2"));
    check(runtime::ui_selected_pass == 1, "pass navigation failed");
    click(find(ui::text::kResultShaping));
    const auto switch_for_label = [&](const char* label) {
      for (const auto& text : rendered_text) {
        if (text.text != label) continue;
        for (const auto& [id, item] : items) {
          if (item.label == "##value" && std::abs(item.rect.GetCenter().y - text.rect.GetCenter().y) < 5.f) return item;
        }
      }
      throw std::runtime_error(std::string("switch missing for label: ") + label);
    };
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
    click(find(ui::text::kResultShaping));

    click(find("##model"));
    click(find("B (Small FP8)"));
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
    const Item model = find("##model");
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
    click(find("##model"));
    check(find("B (Small FP8)").disabled, "unsupported model B remains editable");
    io.AddKeyEvent(ImGuiKey_Escape, true);
    frame();
    io.AddKeyEvent(ImGuiKey_Escape, false);
    frame();
    runtime::direct_runtime_requirements.file_version.clear();
    frame();
    frame();
    check(find("##model").disabled, "unidentified runtime permits model changes");
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
    runtime::stack_passes = 4;

    click(find("developer_view"));
    check(runtime::ui_classic_layout.load(), "Developer view action did not select the classic layout");
    GetPrivateProfileStringA(runtime::kConfigSection, "UiMode", "missing", saved, sizeof(saved), ".\\ReShade.ini");
    check(std::string(saved) == "classic", "Developer view selection was not persisted");

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
