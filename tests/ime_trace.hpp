/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

// Test builds only. These observers never change MSG, WndProc, IME state, or
// ImGui input. The explicit synthetic-field gate excludes other UI and games.
#include <windows.h>
#include <imm.h>

#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#pragma comment(lib, "imm32.lib")

namespace nr_ime_trace {
struct Entry {
  unsigned source = 0;  // 1: Unicode GetMessage hook, 2: WndProc hook, 3: ImGui, 4: buffer
  UINT message = 0;
  unsigned value = 0;
  unsigned flags = 0;
  bool composing = false;
  bool masked = false;
};
struct State {
  std::shared_mutex mutex;
  HWND window = nullptr;
  HHOOK get_message = nullptr;
  HHOOK call_window = nullptr;
  bool active = false;
  bool composing = false;
  unsigned dropped = 0;
  std::vector<Entry> entries;
};
static State state;

inline void Observe(unsigned source, HWND window, UINT message, WPARAM value, LPARAM flags) {
  if (message == WM_INPUT) {
    RAWINPUTHEADER header{};
    UINT bytes = sizeof(header);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(flags), RID_HEADER, &header, &bytes,
                        sizeof(RAWINPUTHEADER)) == UINT(-1) || header.dwType != RIM_TYPEKEYBOARD) return;
  }
  std::unique_lock lock(state.mutex);
  if (!state.active || window != state.window) return;
  switch (message) {
    case WM_IME_STARTCOMPOSITION: state.composing = true; break;
    case WM_IME_ENDCOMPOSITION: state.composing = false; break;
    case WM_IME_COMPOSITION:
    case WM_IME_CHAR:
    case WM_CHAR:
    case WM_UNICHAR:
    case WM_INPUT:
    case WM_INPUTLANGCHANGE: break;
    default: return;
  }
  // Only the fixed public test corpus can appear as code units. Unknown input
  // is masked before entering the trace; no text, hashes, or paths are logged.
  constexpr std::wstring_view corpus = L"中文测试你好世界ASCIIEnglish123zhongwenceshinihaoshijie \x00D6\x00D0\x00CE\x00C4\x00B2\x00E2\x00CA\x00D4\r\n\b";
  const bool is_character = message == WM_CHAR || message == WM_IME_CHAR || message == WM_UNICHAR;
  const bool masked = is_character && corpus.find(static_cast<wchar_t>(value)) == corpus.npos;
  if (state.entries.size() == 2048) { ++state.dropped; return; }
  state.entries.push_back({.source = source, .message = message,
      .value = (is_character && !masked ? static_cast<unsigned>(value) : 0),
      .flags = (message == WM_IME_COMPOSITION ? static_cast<unsigned>(flags) : 0),
      .composing = state.composing, .masked = masked});
}
static LRESULT CALLBACK GetMessage(int code, WPARAM remove, LPARAM data) {
  if (code >= 0 && remove == PM_REMOVE) {
    const auto& message = *reinterpret_cast<const MSG*>(data);
    Observe(1, message.hwnd, message.message, message.wParam, message.lParam);
  }
  return CallNextHookEx(nullptr, code, remove, data);
}
static LRESULT CALLBACK CallWindow(int code, WPARAM sent, LPARAM data) {
  if (code >= 0) {
    const auto& message = *reinterpret_cast<const CWPSTRUCT*>(data);
    Observe(2, message.hwnd, message.message, message.wParam, message.lParam);
    bool active;
    { std::shared_lock lock(state.mutex); active = state.active && state.window == message.hwnd; }
    if (active && message.message == WM_IME_COMPOSITION && (message.lParam & GCS_RESULTSTR) != 0) {
      if (HIMC context = ImmGetContext(message.hwnd)) {
        wchar_t units[256]{};
        const LONG bytes = ImmGetCompositionStringW(context, GCS_RESULTSTR, units, sizeof(units));
        ImmReleaseContext(message.hwnd, context);
        if (bytes > 0 && bytes <= sizeof(units) && bytes % sizeof(wchar_t) == 0) {
          for (LONG index = 0; index < bytes / sizeof(wchar_t); ++index) {
            Observe(6, message.hwnd, WM_CHAR, units[index], 0);
          }
        }
      }
    }
  }
  return CallNextHookEx(nullptr, code, sent, data);
}
inline void Stop() {
  HHOOK get_message;
  HHOOK call_window;
  {
    std::unique_lock lock(state.mutex);
    state.active = false;
    get_message = std::exchange(state.get_message, nullptr);
    call_window = std::exchange(state.call_window, nullptr);
  }
  if (get_message != nullptr) UnhookWindowsHookEx(get_message);
  if (call_window != nullptr) UnhookWindowsHookEx(call_window);
}
inline void Draw(reshade::api::effect_runtime* runtime) {
  static char buffer[256] = {};
  static bool running = false;
  static bool pinned = false;
  static bool focus_requested = false;
  ImGui::TextWrapped("Synthetic IME probe: type only Chinese test phrases or ASCIIEnglish123. Start pins this test build until process exit.");
  if (!running && ImGui::Button("Start synthetic IME trace")) {
    HWND window = reinterpret_cast<HWND>(runtime->get_hwnd());
    DWORD process_id = 0;
    const DWORD thread = GetWindowThreadProcessId(window, &process_id);
    HMODULE module = nullptr;
    // Observers execute on the game's message thread. Test builds deliberately
    // pin themselves until exit, so an in-flight callback cannot be unloaded.
    if (process_id == GetCurrentProcessId() && thread != 0
        && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                             reinterpret_cast<LPCWSTR>(&GetMessage), &module)) {
      const auto get_message = SetWindowsHookExW(WH_GETMESSAGE, GetMessage, module, thread);
      const auto call_window = SetWindowsHookExW(WH_CALLWNDPROC, CallWindow, module, thread);
      if (get_message != nullptr && call_window != nullptr) {
        std::unique_lock lock(state.mutex);
        state.window = window;
        state.get_message = get_message;
        state.call_window = call_window;
        state.entries.clear();
        state.entries.reserve(2048);
        state.dropped = 0;
        state.composing = false;
        UINT count = 0;
        if (GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE)) == 0 && count < 64) {
          std::array<RAWINPUTDEVICE, 64> devices{};
          if (GetRegisteredRawInputDevices(devices.data(), &count, sizeof(RAWINPUTDEVICE)) != UINT(-1)) {
            for (UINT index = 0; index < count; ++index) {
              if (devices[index].usUsagePage == 1 && devices[index].usUsage == 6) {
                state.entries.push_back({.source = 5, .value = devices[index].dwFlags});
              }
            }
          }
        }
        running = true;
        pinned = true;
        focus_requested = true;
      } else {
        if (get_message != nullptr) UnhookWindowsHookEx(get_message);
        if (call_window != nullptr) UnhookWindowsHookEx(call_window);
      }
    }
  }
  if (running && ImGui::Button("Stop synthetic IME trace")) { Stop(); running = false; }
  {
    std::shared_lock lock(state.mutex);
    ImGui::Text("Window Unicode=%d; ACP=%u; ImGui=%s; running=%d; dropped=%u",
        IsWindowUnicode(reinterpret_cast<HWND>(runtime->get_hwnd())), GetACP(), ImGui::GetVersion(), running, state.dropped);
  }
  // Snapshot the host queue before InputText consumes it. Only record when this
  // synthetic field held focus in the previous frame, matching the hook gate.
  if (running) {
    std::unique_lock lock(state.mutex);
    if (state.active) {
      constexpr std::wstring_view corpus = L"中文测试你好世界ASCIIEnglish123zhongwenceshinihaoshijie \x00D6\x00D0\x00CE\x00C4\x00B2\x00E2\x00CA\x00D4\r\n\b";
      for (const auto unit : ImGui::GetIO().InputQueueCharacters) {
        if (state.entries.size() == 2048) { ++state.dropped; break; }
        const bool masked = corpus.find(static_cast<wchar_t>(unit)) == corpus.npos;
        state.entries.push_back({.source = 3, .message = WM_CHAR, .value = (masked ? 0u : unit),
            .composing = state.composing, .masked = masked});
      }
    }
  }
  if (focus_requested) {
    ImGui::SetKeyboardFocusHere();
    focus_requested = false;
  }
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
  const bool changed = ImGui::InputText("##synthetic_input_only", buffer, sizeof(buffer));
  const bool focused = ImGui::IsItemActive();
  ImGui::Text("Synthetic field active=%d; bytes=%u", focused, static_cast<unsigned>(std::strlen(buffer)));
  if (running) {
    std::unique_lock lock(state.mutex);
    state.active = focused;
    if (changed && state.entries.size() < 2048) {
      unsigned fixture = 0;
      const std::string_view text(buffer);
      const std::string_view fixtures[] = {"", "中文测试", "你好世界", "中文测试你好世界", "ASCII中文English123", "ÖÐÎÄ²âÊÔ"};
      for (unsigned index = 1; index < std::size(fixtures); ++index) if (text == fixtures[index]) fixture = index;
      state.entries.push_back({.source = 4, .value = fixture, .flags = static_cast<unsigned>(text.size()),
          .composing = state.composing, .masked = fixture == 0 && !text.empty()});
    }
  }
  if (ImGui::Button("Write synthetic trace to ReShade log") && pinned) {
    auto* entries = new std::vector<Entry>;
    { std::shared_lock lock(state.mutex); *entries = state.entries; }
    HANDLE worker = CreateThread(nullptr, 0, [](void* data) -> DWORD {
      std::unique_ptr<std::vector<Entry>> entries(static_cast<std::vector<Entry>*>(data));
      for (const auto& entry : *entries) {
        std::ostringstream row;
        row << "[IME-PROBE] source=" << entry.source << " message=" << entry.message
            << " value=" << entry.value << " flags=" << entry.flags
            << " composing=" << entry.composing << " masked=" << entry.masked;
        reshade::log::message(reshade::log::level::info, row.str().c_str());
      }
      return 0;
    }, entries, 0, nullptr);
    if (worker != nullptr) CloseHandle(worker); else delete entries;
  }
}
}  // namespace nr_ime_trace
