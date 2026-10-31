/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <Windows.h>
#include <imm.h>
#include <commctrl.h>

#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "comctl32.lib")

#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>

namespace renodx::addons::dlss5::unicode_input {

struct Request {
  unsigned field = 0;
  std::string value;
  std::string title;
  std::string label;
  std::string apply_label;
  std::string cancel_label;
  std::string invalid_label;
  std::size_t max_bytes = 128;
  bool multiline = false;
  bool accepted = false;
  bool failed = false;
  HWND return_window = nullptr;
};

namespace internal {

inline constexpr std::size_t MAX_BYTES = 4096;
// EDIT may omit a final half CRLF/surrogate pair at its storage limit. Even then,
// a truncated native paste must exceed MAX_BYTES after CRLF becomes LF.
inline constexpr int MAX_EDIT_UNITS = static_cast<int>(MAX_BYTES * 2 + 2);
inline constexpr int APPLY_ID = 1004;
inline constexpr int TEXT_ID = 1001;
inline constexpr int LABEL_ID = 1002;
inline constexpr int ERROR_ID = 1003;

inline bool Utf8ToWide(std::string_view value, std::wstring* output) {
  if (output == nullptr || value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())
      || value.find('\0') != std::string_view::npos) return false;
  if (value.empty()) {
    output->clear();
    return true;
  }
  const auto length = static_cast<int>(value.size());
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, nullptr, 0);
  if (count == 0) return false;
  std::wstring converted(count, L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, converted.data(), count) != count) return false;
  *output = std::move(converted);
  return true;
}

inline bool WideToUtf8(std::wstring_view value, std::string* output) {
  if (output == nullptr || value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())
      || value.find(L'\0') != std::wstring_view::npos) return false;
  if (value.empty()) {
    output->clear();
    return true;
  }
  const auto length = static_cast<int>(value.size());
  const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length, nullptr, 0, nullptr, nullptr);
  if (count == 0) return false;
  std::string converted(count, '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length,
                        converted.data(), count, nullptr, nullptr) != count) return false;
  *output = std::move(converted);
  return true;
}

// Native EDIT uses CRLF. Accepted multiline results use LF, including lone CR.
inline std::wstring NormalizeLines(std::wstring_view value, bool native) {
  std::wstring normalized;
  normalized.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    const wchar_t character = value[index];
    if (character != L'\r' && character != L'\n') {
      normalized.push_back(character);
      continue;
    }
    if (character == L'\r' && index + 1 < value.size() && value[index + 1] == L'\n') {
      ++index;
    }
    if (native) {
      normalized.push_back(L'\r');
    }
    normalized.push_back(L'\n');
  }
  return normalized;
}

struct Dialog {
  Request request;
  HMODULE module = nullptr;
  HWND window = nullptr;
  std::wstring value;
  std::wstring title;
  std::wstring label;
  std::wstring apply_label;
  std::wstring cancel_label;
  std::wstring invalid_label;
  HIMC ime_context = nullptr;
  HIMC previous_ime_context = nullptr;
  bool composing = false;
  UINT ime_key = 0;
  unsigned control_keys = 0;
  bool alt_down = false;
  bool initialized = false;
  bool visible = true;
  bool worker_thread = false;
  bool return_focus = false;
};

inline bool PrepareDialog(const Request& request, Dialog* dialog) {
  if (dialog == nullptr || request.max_bytes > MAX_BYTES || request.value.size() > MAX_BYTES) return false;
  if (request.return_window != nullptr) {
    DWORD process = 0;
    if (!IsWindow(request.return_window) || GetWindowThreadProcessId(request.return_window, &process) == 0
        || process != GetCurrentProcessId()) return false;
  }
  const std::string* inputs[] = {&request.value, &request.title, &request.label,
                                &request.apply_label, &request.cancel_label, &request.invalid_label};
  std::wstring* outputs[] = {&dialog->value, &dialog->title, &dialog->label,
                            &dialog->apply_label, &dialog->cancel_label, &dialog->invalid_label};
  for (std::size_t index = 0; index < 6; ++index) {
    if (inputs[index]->size() > MAX_BYTES || !Utf8ToWide(*inputs[index], outputs[index])) return false;
  }
  if (request.multiline) {
    dialog->value = NormalizeLines(dialog->value, true);
  } else if (dialog->value.find_first_of(L"\r\n") != std::wstring::npos) {
    return false;
  }
  dialog->request = request;
  dialog->request.accepted = false;
  dialog->request.failed = false;
  return true;
}

inline bool CompositionActive(const Dialog* dialog) {
  if (dialog->composing) return true;
  HWND edit = GetDlgItem(dialog->window, TEXT_ID);
  HIMC context = ImmGetContext(edit);
  if (context == nullptr) return false;
  const LONG length = ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0);
  ImmReleaseContext(edit, context);
  // NODATA means no composition; GENERAL is an unknown state, so defer actions.
  return length > 0 || length == IMM_ERROR_GENERAL;
}

// IME key notifications can arrive synchronously at EDIT, bypassing the outer
// GetMessage loop. Both routes share modifier state and consume shortcuts before
// their default processing can generate another key/character message.
inline bool HandleShortcut(Dialog* dialog, HWND window, UINT message, WPARAM key, LPARAM flags) {
  HWND edit = GetDlgItem(dialog->window, TEXT_ID);
  const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN || message == WM_IME_KEYDOWN;
  const bool up = message == WM_KEYUP || message == WM_SYSKEYUP || message == WM_IME_KEYUP;
  if (window != edit || GetFocus() != edit || (!down && !up)) return false;
  if (key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL) {
    const unsigned side = (key == VK_RCONTROL || (key == VK_CONTROL && (flags & (1L << 24)) != 0)) ? 2u : 1u;
    if (down) {
      dialog->control_keys |= side;
    } else {
      dialog->control_keys &= ~side;
    }
  }
  if (key == VK_MENU || key == VK_LMENU || key == VK_RMENU) dialog->alt_down = down;
  if ((message == WM_KEYDOWN || message == WM_IME_KEYDOWN) && dialog->control_keys != 0
      && !dialog->alt_down && dialog->ime_key == 0 && (key == 'C' || key == 'V')
      && !CompositionActive(dialog)) {
    SendMessageW(edit, key == 'C' ? WM_COPY : WM_PASTE, 0, 0);
    return true;
  }
  return false;
}

static LRESULT CALLBACK EditProc(HWND window, UINT message, WPARAM value, LPARAM data,
                                UINT_PTR subclass, DWORD_PTR reference) {
  auto* dialog = reinterpret_cast<Dialog*>(reference);
  if (HandleShortcut(dialog, window, message, value, data)) return 0;
  if (message == WM_IME_STARTCOMPOSITION) dialog->composing = true;
  if (message == WM_IME_ENDCOMPOSITION) dialog->composing = false;
  if (message == WM_CHAR && value == VK_ESCAPE) return 0;
  if (message == WM_NCDESTROY) {
    RemoveWindowSubclass(window, EditProc, subclass);
    ImmAssociateContext(window, dialog->previous_ime_context);
    ImmDestroyContext(dialog->ime_context);
    dialog->ime_context = nullptr;
  }
  const LRESULT result = DefSubclassProc(window, message, value, data);
  if (message == WM_KILLFOCUS) {
    // A key-up can go to another application after Alt-Tab. Do not leave the
    // action gate latched; IMM remains the authority if composition is still live.
    dialog->ime_key = 0;
    dialog->composing = false;
    dialog->control_keys = 0;
    dialog->alt_down = false;
  }
  return result;
}

// Query IMM before dialog translation: IsDialogMessage would otherwise turn a
// candidate-selection Enter/Escape into a dialog command before EDIT sees it.
inline void ProcessMessage(Dialog* dialog, MSG* message) {
  const bool ime_active = CompositionActive(dialog);
  HWND edit = GetDlgItem(dialog->window, TEXT_ID);
  WPARAM key_code = message->wParam;
  if (message->hwnd == edit && GetFocus() == edit && message->message == WM_KEYDOWN
      && key_code == VK_PROCESSKEY) {
    // IMM exposes the original key only before this message is translated.
    // Keep MSG unchanged when the key is not one of our focused shortcuts.
    key_code = ImmGetVirtualKey(edit);
  }
  if (HandleShortcut(dialog, message->hwnd, message->message, key_code, message->lParam)) return;
  const bool key = message->wParam == VK_RETURN || message->wParam == VK_ESCAPE;
  if (message->message == WM_KEYDOWN && key && ime_active) {
    dialog->ime_key = static_cast<UINT>(message->wParam);
  }
  const bool protected_key = key && (ime_active || dialog->ime_key == message->wParam);
  if (protected_key || message->message == WM_IME_STARTCOMPOSITION
      || message->message == WM_IME_COMPOSITION || message->message == WM_IME_ENDCOMPOSITION) {
    TranslateMessage(message);
    DispatchMessageW(message);
  } else if (message->message == WM_KEYDOWN && message->wParam == VK_RETURN
             && message->hwnd == GetDlgItem(dialog->window, TEXT_ID)) {
    // Enter in a multiline EDIT inserts a line; single-line Enter never applies.
    TranslateMessage(message);
    DispatchMessageW(message);
  } else if (!IsDialogMessageW(dialog->window, message)) {
    TranslateMessage(message);
    DispatchMessageW(message);
  }
  if (message->message == WM_KEYUP && dialog->ime_key == message->wParam) dialog->ime_key = 0;
}

static INT_PTR CALLBACK DialogProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
  auto* dialog = reinterpret_cast<Dialog*>(GetWindowLongPtrW(window, DWLP_USER));
  try {
    if (message == WM_INITDIALOG) {
      dialog = reinterpret_cast<Dialog*>(l_param);
      if (dialog == nullptr) return FALSE;
      dialog->window = window;
      SetWindowLongPtrW(window, DWLP_USER, reinterpret_cast<LONG_PTR>(dialog));
      if (!SetWindowTextW(window, dialog->title.c_str())) {
        dialog->request.failed = true;
        DestroyWindow(window);
        return FALSE;
      }
      const int edit_height = (dialog->request.multiline ? 100 : 16);
      const int button_y = 24 + edit_height + 30;
      const auto font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
      const auto create_control = [&](DWORD extended_style, LPCWSTR class_name, LPCWSTR caption,
                                      DWORD style, int id, RECT bounds) {
        if (!MapDialogRect(window, &bounds)) return static_cast<HWND>(nullptr);
        HWND control = CreateWindowExW(extended_style, class_name, caption, WS_CHILD | WS_VISIBLE | style,
                                       bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
                                       window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), dialog->module, nullptr);
        if (control != nullptr) {
          SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
        }
        return control;
      };
      const DWORD edit_style = WS_TABSTOP | ES_LEFT
                               | (dialog->request.multiline
                                      ? (ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL)
                                      : ES_AUTOHSCROLL);
      HWND label = create_control(0, L"STATIC", dialog->label.c_str(), SS_LEFT, LABEL_ID, {8, 8, 312, 22});
      HWND edit = create_control(WS_EX_CLIENTEDGE, L"EDIT", L"", edit_style, TEXT_ID,
                                 {8, 24, 312, 24 + edit_height});
      HWND error = create_control(0, L"STATIC", L"", SS_LEFT, ERROR_ID,
                                  {8, 28 + edit_height, 312, button_y - 2});
      HWND apply = create_control(0, L"BUTTON", dialog->apply_label.c_str(), WS_TABSTOP | BS_PUSHBUTTON,
                                  APPLY_ID, {170, button_y, 239, button_y + 16});
      HWND cancel = create_control(0, L"BUTTON", dialog->cancel_label.c_str(), WS_TABSTOP | BS_PUSHBUTTON,
                                   IDCANCEL, {243, button_y, 312, button_y + 16});
      if (label == nullptr || edit == nullptr || error == nullptr || apply == nullptr || cancel == nullptr
          || !IsWindowUnicode(edit) || !SetWindowTextW(edit, dialog->value.c_str())) {
        dialog->request.failed = true;
        DestroyWindow(window);
        return FALSE;
      }
      dialog->ime_context = ImmCreateContext();
      if (dialog->ime_context == nullptr) {
        dialog->request.failed = true;
        DestroyWindow(window);
        return FALSE;
      }
      dialog->previous_ime_context = ImmAssociateContext(edit, dialog->ime_context);
      if (!SetWindowSubclass(edit, EditProc, 1, reinterpret_cast<DWORD_PTR>(dialog))) {
        ImmAssociateContext(edit, dialog->previous_ime_context);
        ImmDestroyContext(dialog->ime_context);
        dialog->ime_context = nullptr;
        dialog->request.failed = true;
        DestroyWindow(window);
        return FALSE;
      }
      SendMessageW(edit, EM_SETLIMITTEXT, MAX_EDIT_UNITS, 0);
      SendMessageW(edit, EM_SETSEL, 0, -1);
      SetFocus(edit);
      dialog->initialized = true;
      return FALSE;
    }
    if (dialog == nullptr) return FALSE;
    switch (message) {
      case WM_COMMAND:
        switch (LOWORD(w_param)) {
          case IDOK:
            return TRUE;  // Enter is never an implicit Apply.
          case APPLY_ID: {
            HWND edit = GetDlgItem(window, TEXT_ID);
            if (CompositionActive(dialog) || dialog->ime_key != 0) return TRUE;
            SetLastError(ERROR_SUCCESS);
            const int length = GetWindowTextLengthW(edit);
            if (edit == nullptr || (length == 0 && GetLastError() != ERROR_SUCCESS)) {
              dialog->request.failed = true;
              DestroyWindow(window);
              return TRUE;
            }
            bool valid = length <= MAX_EDIT_UNITS;
            std::string value;
            if (valid) {
              std::wstring native(static_cast<std::size_t>(length) + 1, L'\0');
              SetLastError(ERROR_SUCCESS);
              const int copied = GetWindowTextW(edit, native.data(), length + 1);
              if (copied != length || (copied == 0 && GetLastError() != ERROR_SUCCESS)) {
                dialog->request.failed = true;
                DestroyWindow(window);
                return TRUE;
              }
              native.resize(length);
              if (dialog->request.multiline) {
                native = NormalizeLines(native, false);
              } else if (native.find_first_of(L"\r\n") != std::wstring::npos) {
                valid = false;
              }
              valid = valid && WideToUtf8(native, &value) && value.size() <= dialog->request.max_bytes;
            }
            if (!valid) {
              if (!SetDlgItemTextW(window, ERROR_ID, dialog->invalid_label.c_str())) {
                dialog->request.failed = true;
                DestroyWindow(window);
              } else {
                SetFocus(edit);
              }
              return TRUE;
            }
            dialog->request.value = std::move(value);
            dialog->request.accepted = true;
            dialog->return_focus = GetForegroundWindow() == window;
            DestroyWindow(window);
            return TRUE;
          }
          case IDCANCEL:
            if (CompositionActive(dialog) || dialog->ime_key != 0) return TRUE;
            dialog->return_focus = GetForegroundWindow() == window;
            DestroyWindow(window);
            return TRUE;
        }
        break;
      case WM_CLOSE:
        dialog->return_focus = GetForegroundWindow() == window;
        DestroyWindow(window);
        return TRUE;
      case WM_NCDESTROY:
        dialog->window = nullptr;
        SetWindowLongPtrW(window, DWLP_USER, 0);
        // Cross-thread SendMessage can destroy the window inside GetMessage's
        // sent-message dispatch. Wake that wait even if no posted input remains.
        if (dialog->worker_thread) {
          PostQuitMessage(0);
        }
        break;
    }
  } catch (...) {
    if (dialog != nullptr) {
      dialog->request.failed = true;
      DestroyWindow(window);
    }
    return TRUE;
  }
  return FALSE;
}

// Hidden by default so tests exercise the same dialog without taking user input.
// A cross-thread native owner would implicitly attach input queues, including
// the game's hidden cursor count. The return target is used only after closing.
inline HWND OpenEditor(Dialog* dialog) {
  struct alignas(DWORD) Template {
    DLGTEMPLATE dialog;
    WORD menu = 0;
    WORD window_class = 0;
    WORD title = 0;
    WORD font_size = 9;
    wchar_t font[9] = L"Segoe UI";
  } resource{
      .dialog = {.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT | DS_CENTER,
                 .dwExtendedStyle = WS_EX_APPWINDOW,
                 .cdit = 0,
                 .x = 0, .y = 0, .cx = 320,
                 .cy = static_cast<short>(dialog->request.multiline ? 178 : 94)},
  };
  HWND window = CreateDialogIndirectParamW(dialog->module, &resource.dialog, nullptr, DialogProc,
                                          reinterpret_cast<LPARAM>(dialog));
  if (window == nullptr || dialog->window == nullptr || !dialog->initialized) {
    dialog->request.failed = true;
    if (dialog->window != nullptr) {
      DestroyWindow(dialog->window);
    }
    return nullptr;
  }
  return window;
}

struct State {
  std::shared_mutex mutex;
  HANDLE worker = nullptr;
  std::optional<Request> result;

  ~State() {
    if (worker != nullptr) {
      CloseHandle(worker);
    }
  }
};

static State state;

static DWORD WINAPI WorkerMain(void* parameter) {
  HMODULE module = nullptr;
  {
    std::unique_ptr<Dialog> dialog(static_cast<Dialog*>(parameter));
    module = dialog->module;
    dialog->worker_thread = true;
    try {
      if (HWND window = OpenEditor(dialog.get())) {
        if (dialog->visible) {
          ShowWindow(window, SW_SHOW);
          SetForegroundWindow(window);
          SetFocus(GetDlgItem(window, TEXT_ID));
        }
        MSG message{};
        while (dialog->window != nullptr) {
          const BOOL received = GetMessageW(&message, nullptr, 0, 0);
          if (received <= 0) {
            if (dialog->window != nullptr) {
              dialog->request.failed = true;
              DestroyWindow(dialog->window);
            }
            break;
          }
          // A sent WM_CLOSE may destroy the HWND while GetMessage dispatches it.
          if (dialog->window == nullptr) break;
          ProcessMessage(dialog.get(), &message);
        }
      }
    } catch (...) {
      dialog->request.failed = true;
      if (dialog->window != nullptr) {
        DestroyWindow(dialog->window);
      }
    }
    if (dialog->visible && dialog->return_focus && IsWindow(dialog->request.return_window)) {
      // Foreground policy may deny activation. Never steal focus after Alt-Tab,
      // and never attach the game and editor input queues to force activation.
      SetForegroundWindow(dialog->request.return_window);
    }
    std::unique_lock lock(state.mutex);
    state.result.emplace(std::move(dialog->request));
  }
  // Release every C++ owner/lock before this call, which does not unwind.
  FreeLibraryAndExitThread(module, 0);
}

// The visible policy is internal: production Start always opens a visible editor;
// native regression tests can run the identical worker without showing a window.
inline bool StartEditor(const Request& request, bool visible) {
  try {
    std::unique_lock lock(internal::state.mutex);
    if (internal::state.worker != nullptr || internal::state.result.has_value()) return false;
    auto dialog = std::make_unique<internal::Dialog>();
    if (!internal::PrepareDialog(request, dialog.get())) return false;
    dialog->visible = visible;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCWSTR>(&internal::WorkerMain), &dialog->module)) return false;
    HANDLE worker = CreateThread(nullptr, 0, internal::WorkerMain, dialog.get(), 0, nullptr);
    if (worker == nullptr) {
      FreeLibrary(dialog->module);
      return false;
    }
    internal::state.worker = worker;
    dialog.release();
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace internal

// Call only for an explicit Edit text click. No thread/window is created at load.
// Reject malformed UTF-8/NUL, input strings over 4096 bytes, max_bytes > 4096,
// or a second request while editing or while a result is still unconsumed.
inline bool Start(Request request) {
  return internal::StartEditor(request, true);
}

// Remains busy through native window destruction, C++ cleanup and module release.
inline bool Busy() {
  std::shared_lock lock(internal::state.mutex);
  return internal::state.worker != nullptr
         && WaitForSingleObject(internal::state.worker, 0) != WAIT_OBJECT_0;
}

// Poll without blocking; consumes once, only after the worker has exited.
// Cancel/close preserves the original value with accepted=false, failed=false.
// Async Win32 failure returns the original value with accepted=false, failed=true.
inline std::optional<Request> TakeResult() {
  std::unique_lock lock(internal::state.mutex);
  if (!internal::state.result.has_value() || internal::state.worker == nullptr
      || WaitForSingleObject(internal::state.worker, 0) != WAIT_OBJECT_0) return std::nullopt;
  auto result = std::move(internal::state.result);
  internal::state.result.reset();
  CloseHandle(internal::state.worker);
  internal::state.worker = nullptr;
  return result;
}
}  // namespace renodx::addons::dlss5::unicode_input
