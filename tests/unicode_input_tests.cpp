/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#include <Windows.h>
#include <imm.h>

#include <cstddef>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef UNICODE_INPUT_TEST_MODULE
namespace ime_key_fixture {
HWND window = nullptr;
UINT key = VK_PROCESSKEY;
UINT WINAPI GetVirtualKey(HWND target) {
  return target == window ? key : ImmGetVirtualKey(target);
}
}  // namespace ime_key_fixture
#define ImmGetVirtualKey ime_key_fixture::GetVirtualKey
#endif
#include <unicode_input.hpp>
#ifndef UNICODE_INPUT_TEST_MODULE
#undef ImmGetVirtualKey
#endif

namespace text = renodx::addons::dlss5::unicode_input;
namespace native = text::internal;

#ifdef UNICODE_INPUT_TEST_MODULE

// Only Win32 handles/scalars cross CRT boundaries; the DLL owns its request.
extern "C" __declspec(dllexport) bool StartModuleEditor() {
  return native::StartEditor({.field = 7, .value = "模块原值", .title = "Synthetic DLL editor",
                              .label = "Text", .apply_label = "Apply", .cancel_label = "Cancel",
                              .invalid_label = "Invalid text"}, false);
}

extern "C" __declspec(dllexport) HANDLE DuplicateModuleWorker() {
  std::shared_lock lock(native::state.mutex);
  HANDLE duplicate = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), native::state.worker, GetCurrentProcess(),
                       &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) return nullptr;
  return duplicate;
}

#else

namespace {

std::size_t checks = 0;

void Require(bool condition, const char* message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}

text::Request Sample() {
  return {.field = 42, .value = "原值 日本語 한국어 café é 😀 𠮷 👩‍💻",
          .title = "合成测试 Unicode 😀", .label = "文本 日本語", .apply_label = "应用",
          .cancel_label = "取消", .invalid_label = "文本无效或超过 UTF-8 字节上限",
          .max_bytes = 128};
}

std::wstring ReadText(HWND window) {
  const int length = GetWindowTextLengthW(window);
  std::wstring result(static_cast<std::size_t>(length) + 1, L'\0');
  Require(GetWindowTextW(window, result.data(), length + 1) == length, "Read Unicode window text");
  result.resize(length);
  return result;
}

struct Editor {
  native::Dialog dialog;

  explicit Editor(const text::Request& request) {
    Require(native::PrepareDialog(request, &dialog), "Prepare production dialog");
    dialog.module = GetModuleHandleW(nullptr);
    Require(native::OpenEditor(&dialog) != nullptr, "Create real hidden production dialog");
  }

  ~Editor() {
    if (dialog.window != nullptr) {
      DestroyWindow(dialog.window);
    }
  }
};

BOOL CALLBACK FindEditor(HWND window, LPARAM parameter) {
  if (GetDlgItem(window, native::TEXT_ID) == nullptr) return TRUE;
  *reinterpret_cast<HWND*>(parameter) = window;
  return FALSE;
}

struct Worker {
  HANDLE handle = nullptr;

  ~Worker() {
    if (handle == nullptr) return;
    if (WaitForSingleObject(handle, 0) != WAIT_OBJECT_0) {
      // On any failed assertion, close only this test-owned worker's windows.
      PostThreadMessageW(GetThreadId(handle), WM_QUIT, 0, 0);
      WaitForSingleObject(handle, 5000);
    }
    CloseHandle(handle);
  }
};

HWND WaitForEditor(HANDLE worker) {
  const auto deadline = GetTickCount64() + 5000;
  while (GetTickCount64() < deadline) {
    HWND window = nullptr;
    EnumThreadWindows(GetThreadId(worker), FindEditor, reinterpret_cast<LPARAM>(&window));
    if (window != nullptr) return window;
    if (WaitForSingleObject(worker, 0) == WAIT_OBJECT_0) break;
    Sleep(1);
  }
  throw std::runtime_error("Hidden worker did not create its editor");
}

void TestStrictConversion() {
  const auto sample = Sample();
  std::wstring wide;
  std::string returned;
  Require(native::Utf8ToWide(sample.value, &wide), "Strict UTF-8 initialization");
  Require(native::WideToUtf8(wide, &returned) && returned == sample.value, "Mixed scripts/emoji bytes preserved");
  Require(native::Utf8ToWide("", &wide) && wide.empty(), "Empty UTF-8");
  Require(native::WideToUtf8(L"", &returned) && returned.empty(), "Empty UTF-16");
  for (const auto& invalid : std::vector<std::string>{"\x80", "\xc0\xaf", "\xe4\xb8", "\xed\xa0\x80", "\xf4\x90\x80\x80", std::string("a\0b", 3)}) {
    wide = L"unchanged";
    Require(!native::Utf8ToWide(invalid, &wide) && wide == L"unchanged", "Reject malformed UTF-8/NUL without replacement");
    auto request = sample;
    request.value = invalid;
    Require(!text::Start(request), "Malformed UTF-8 never launches a worker");
  }
  for (const auto& invalid : std::vector<std::wstring>{std::wstring(1, 0xd800), std::wstring(1, 0xdc00), std::wstring(L"a\0b", 3)}) {
    returned = "unchanged";
    Require(!native::WideToUtf8(invalid, &returned) && returned == "unchanged", "Reject malformed UTF-16/NUL without replacement");
  }
  Require(!native::Utf8ToWide("a", nullptr) && !native::WideToUtf8(L"a", nullptr), "Null conversion outputs rejected");
  auto request = sample;
  request.max_bytes = 4097;
  Require(!text::Start(request), "Reject byte limit above 4096");
  request.max_bytes = 128;
  request.value.assign(4097, 'x');
  Require(!text::Start(request), "Bound initial allocation");
  request = sample;
  request.title = "\xff";
  Require(!text::Start(request), "Validate localized labels before launching");
  request = sample;
  request.value = "one\ntwo";
  Require(!text::Start(request), "Single line initialization never truncates at newline");
  Require(!text::Busy() && !text::TakeResult(), "Invalid requests leave API idle");
}

void TestNativeCharacters() {
  const auto sample = Sample();
  Editor editor(sample);
  HWND window = editor.dialog.window;
  HWND edit = GetDlgItem(window, native::TEXT_ID);
  Require(IsWindowUnicode(window) && IsWindowUnicode(edit), "Real Unicode dialog and EDIT");
  Require(!IsWindowVisible(window), "Production OpenEditor stays hidden until explicit ShowWindow");
  Require(GetParent(window) == nullptr && GetWindow(window, GW_OWNER) == nullptr, "Top-level editor has no game parent or owner");
  Require(GetAncestor(edit, GA_ROOT) == window && GetParent(edit) == window, "EDIT routes only through unregistered dialog");
  Require(ReadText(window) == L"合成测试 Unicode 😀", "Localized Unicode title");
  Require(ReadText(GetDlgItem(window, native::LABEL_ID)) == L"文本 日本語", "Localized static label");
  Require(ReadText(GetDlgItem(window, native::APPLY_ID)) == L"应用" && ReadText(GetDlgItem(window, IDCANCEL)) == L"取消", "Localized native buttons");
  Require(ReadText(edit) == L"原值 日本語 한국어 café é 😀 𠮷 👩‍💻", "Multilingual/emoji native initialization");
  Require(SetWindowTextW(edit, L""), "Clear synthetic EDIT");
  // WM_CHAR carries UTF-16 code units for a Unicode EDIT. No global/user input.
  for (const wchar_t unit : std::wstring_view(L"中文😀")) {
    SendMessageW(edit, WM_CHAR, unit, 1);
  }
  Require(ReadText(edit) == L"中文😀", "Unicode WM_CHAR Chinese and surrogate pair");
  SendMessageW(window, WM_COMMAND, native::APPLY_ID, 0);
  Require(editor.dialog.window == nullptr && !IsWindow(window), "Apply destroys native window synchronously");
  Require(editor.dialog.request.accepted && !editor.dialog.request.failed, "Apply publishes accepted success");
  Require(editor.dialog.request.value == "中文😀" && editor.dialog.request.field == sample.field, "Apply returns exact UTF-8 and stable field ID");

  // Controlled fault injection of a byte-origin WM_CHAR cast, not a user trace.
  // Casting each UTF-8 byte to wchar_t models the host's code-unit interpretation;
  // it deliberately does not use ACP, codepage repair, or simulated IME typing.
  std::wstring byte_origin;
  for (const unsigned char byte : std::string("中文")) {
    byte_origin.push_back(static_cast<wchar_t>(byte));
  }
  std::string corrupted;
  std::string correct;
  Require(native::WideToUtf8(byte_origin, &corrupted) && corrupted != "中文", "Controlled byte-origin cast changes UTF-8");
  Require(native::WideToUtf8(L"中文", &correct) && correct == "中文", "Correct UTF-16 preserves Chinese");
  std::cout << "Controlled fault injection: byte-origin WM_CHAR cast differs; not an actual user trace.\n";
}

void TestLimitsAndInvalidText() {
  auto request = Sample();
  request.max_bytes = 6;
  Editor editor(request);
  HWND window = editor.dialog.window;
  HWND edit = GetDlgItem(window, native::TEXT_ID);
  Require(SendMessageW(edit, EM_GETLIMITTEXT, 0, 0) == native::MAX_EDIT_UNITS, "Bound native edit storage independently of byte budget");
  for (const auto& invalid : std::vector<std::wstring>{L"中文a", std::wstring(1, 0xd800), std::wstring(1, 0xdc00)}) {
    Require(SetWindowTextW(edit, invalid.c_str()), "Set controlled invalid native text");
    SendMessageW(window, WM_COMMAND, native::APPLY_ID, 0);
    Require(editor.dialog.window == window && IsWindow(window), "Invalid Apply keeps dialog open");
    Require(!editor.dialog.request.accepted && !editor.dialog.request.failed, "Validation is not a terminal Win32 failure");
    Require(editor.dialog.request.value == request.value, "Invalid Apply never mutates original value");
    Require(ReadText(GetDlgItem(window, native::ERROR_ID)) == L"文本无效或超过 UTF-8 字节上限", "Inline static localized error");
    Require(ReadText(edit) == invalid, "Invalid Apply never truncates edit text");
  }
  Require(SetWindowTextW(edit, L"中文"), "Set exact UTF-8 byte boundary");
  SendMessageW(window, WM_COMMAND, native::APPLY_ID, 0);
  Require(editor.dialog.request.accepted && editor.dialog.request.value == "中文", "UTF-8 byte limit accepts exact boundary");

  request.value = "";
  request.max_bytes = 0;
  Editor empty(request);
  SendMessageW(empty.dialog.window, WM_COMMAND, native::APPLY_ID, 0);
  Require(empty.dialog.request.accepted && empty.dialog.request.value.empty(), "Zero byte budget permits empty text");
  request.max_bytes = 4096;
  Editor bounded(request);
  edit = GetDlgItem(bounded.dialog.window, native::TEXT_ID);
  const std::wstring large(4096, L'x');
  Require(SetWindowTextW(edit, large.c_str()), "Initialize maximum ASCII budget");
  SendMessageW(bounded.dialog.window, WM_COMMAND, native::APPLY_ID, 0);
  Require(bounded.dialog.request.accepted && bounded.dialog.request.value.size() == 4096, "Maximum 4096 byte Apply");
}

void TestCancelAndMultiline() {
  auto request = Sample();
  request.multiline = true;
  request.value = "第一行\n日本語\r\nemoji 😀\r尾行";
  Editor editor(request);
  HWND edit = GetDlgItem(editor.dialog.window, native::TEXT_ID);
  Require(ReadText(edit) == L"第一行\r\n日本語\r\nemoji 😀\r\n尾行", "Native initialization normalizes CR/LF to CRLF");
  SendMessageW(editor.dialog.window, WM_COMMAND, native::APPLY_ID, 0);
  Require(editor.dialog.request.accepted && editor.dialog.request.value == "第一行\n日本語\nemoji 😀\n尾行", "Accepted multiline returns LF");
  Require(native::NormalizeLines(L"a\r\nb\rc\n", false) == L"a\nb\nc\n", "Normalization handles all newline forms");
  for (const int action : {IDCANCEL, WM_CLOSE, WM_KEYDOWN}) {
    Editor cancelled(request);
    HWND window = cancelled.dialog.window;
    edit = GetDlgItem(window, native::TEXT_ID);
    Require(SetWindowTextW(edit, L"discarded\r\n改动"), "Edit before cancellation");
    if (action == WM_KEYDOWN) {
      MSG message{.hwnd = edit, .message = WM_KEYDOWN, .wParam = VK_ESCAPE};
      native::ProcessMessage(&cancelled.dialog, &message);
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
    } else if (action == WM_CLOSE) {
      SendMessageW(window, WM_CLOSE, 0, 0);
    } else {
      SendMessageW(window, WM_COMMAND, IDCANCEL, 0);
    }
    Require(cancelled.dialog.window == nullptr && !IsWindow(window), "Cancel/close/Escape destroys window");
    Require(!cancelled.dialog.request.accepted && !cancelled.dialog.request.failed, "Cancellation is distinct from failure");
    Require(cancelled.dialog.request.value == request.value, "Cancel preserves original bytes and original line endings");
  }
  request.value = "";
  request.max_bytes = 4096;
  Editor truncated(request);
  edit = GetDlgItem(truncated.dialog.window, native::TEXT_ID);
  std::wstring lines;
  for (int index = 0; index < 5000; ++index) {
    lines += L"\r\n";
  }
  // EM_REPLACESEL exercises EDIT's actual storage bound, without the clipboard.
  SendMessageW(edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(lines.c_str()));
  Require(GetWindowTextLengthW(edit) <= native::MAX_EDIT_UNITS, "Native replacement remains bounded");
  SendMessageW(truncated.dialog.window, WM_COMMAND, native::APPLY_ID, 0);
  Require(truncated.dialog.window != nullptr && !truncated.dialog.request.accepted, "Native truncated paste cannot publish a valid prefix");
}

void TestWorkerAndResults() {
  Require(!text::Busy() && !text::TakeResult(), "No eager worker on module load");
  auto request = Sample();
  const auto original = request;
  request.accepted = true;
  request.failed = true;
  Require(native::StartEditor(request, false), "Start hidden production worker");
  Worker worker;
  {
    std::shared_lock lock(native::state.mutex);
    Require(DuplicateHandle(GetCurrentProcess(), native::state.worker, GetCurrentProcess(),
                             &worker.handle, 0, FALSE, DUPLICATE_SAME_ACCESS), "Retain test-owned thread handle");
  }
  HWND window = WaitForEditor(worker.handle);
  Require(!IsWindowVisible(window) && text::Busy(), "Hidden worker reports busy");
  Require(!text::Start(request), "Second public Start is rejected without spawning");
  std::optional<text::Request> result;
  Require(!(result = text::TakeResult()), "Cannot consume an active editor");
  request.value = "caller changed";
  request.title = "caller changed title";
  Require(ReadText(window) == L"合成测试 Unicode 😀", "Worker owns its copied request");
  DWORD_PTR ignored = 0;
  Require(SendMessageTimeoutW(window, WM_COMMAND, IDCANCEL, 0, SMTO_ABORTIFHUNG, 2000, &ignored), "Cancel hidden worker deterministically");
  Require(WaitForSingleObject(worker.handle, 5000) == WAIT_OBJECT_0, "Worker exits after HWND and C++ cleanup");
  Require(!IsWindow(window) && !text::Busy(), "No live native window after worker exit");
  Require(!native::StartEditor(request, false), "Pending result cannot be overwritten");
  Require((result = text::TakeResult()).has_value(), "Consume completed result");
  Require(result->field == original.field && result->value == original.value && result->title == original.title,
          "Copied result remains independent of caller mutation");
  Require(!result->accepted && !result->failed, "Input result flags are reset; cancellation does not fail");
  Require(!(result = text::TakeResult()), "Result is single-consumer");

  // Competing callers exercise the same gate while the first editor is active.
  Require(native::StartEditor(original, false), "Editor can restart after result consumption");
  Worker accepted;
  {
    std::shared_lock lock(native::state.mutex);
    Require(DuplicateHandle(GetCurrentProcess(), native::state.worker, GetCurrentProcess(),
                             &accepted.handle, 0, FALSE, DUPLICATE_SAME_ACCESS), "Retain second worker handle");
  }
  window = WaitForEditor(accepted.handle);
  bool first = true;
  bool second = true;
  std::thread one([&] { first = native::StartEditor(original, false); });
  std::thread two([&] { second = native::StartEditor(original, false); });
  one.join();
  two.join();
  Require(!first && !second, "Concurrent requests do not spawn extra windows");
  HWND edit = GetDlgItem(window, native::TEXT_ID);
  Require(SendMessageTimeoutW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"中文😀"),
                              SMTO_ABORTIFHUNG, 2000, &ignored), "Set hidden worker Unicode text");
  Require(SendMessageTimeoutW(window, WM_COMMAND, native::APPLY_ID, 0, SMTO_ABORTIFHUNG, 2000, &ignored), "Apply hidden worker");
  Require(WaitForSingleObject(accepted.handle, 5000) == WAIT_OBJECT_0, "Accepted worker fully exits");
  Require((result = text::TakeResult()) && result->accepted && !result->failed && result->value == "中文😀", "Worker publishes strict UTF-8 result");
  Require(!text::Busy(), "API returns to idle");
}

void TestModuleLifetime(const wchar_t* path) {
  HMODULE module = LoadLibraryW(path);
  Require(module != nullptr, "Load independent CRT test DLL");
  const auto start = reinterpret_cast<bool (*)()>(GetProcAddress(module, "StartModuleEditor"));
  const auto duplicate = reinterpret_cast<HANDLE (*)()>(GetProcAddress(module, "DuplicateModuleWorker"));
  if (start == nullptr || duplicate == nullptr) {
    FreeLibrary(module);
    throw std::runtime_error("Test module exports missing");
  }
  if (!start()) {
    FreeLibrary(module);
    throw std::runtime_error("DLL hidden worker did not start");
  }
  Worker worker{.handle = duplicate()};
  Require(worker.handle != nullptr, "Duplicate DLL worker handle");
  HWND window = WaitForEditor(worker.handle);
  Require(!IsWindowVisible(window), "DLL worker window remains hidden");
  Require(FreeLibrary(module), "Release all external DLL ownership while window is open");
  Require(GetModuleHandleW(path) != nullptr, "Worker's module reference protects dialog callback and RAII");
  DWORD_PTR ignored = 0;
  Require(SendMessageTimeoutW(window, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG, 2000, &ignored), "Close after external DLL unload request");
  Require(WaitForSingleObject(worker.handle, 5000) == WAIT_OBJECT_0, "DLL worker exits via FreeLibraryAndExitThread");
  Require(!IsWindow(window), "DLL window gone before module is released");
  Require(GetModuleHandleW(path) == nullptr, "No pinned DLL leak after callback and C++ cleanup");
}

void TestWin32Failure() {
  const auto request = Sample();
  Editor broken(request);
  HWND window = broken.dialog.window;
  // Controlled native failure, without intercepting APIs: remove the required
  // EDIT so Apply must fail rather than turn a failed read into an empty value.
  Require(DestroyWindow(GetDlgItem(window, native::TEXT_ID)), "Remove synthetic editor control");
  SendMessageW(window, WM_COMMAND, native::APPLY_ID, 0);
  Require(broken.dialog.window == nullptr && !IsWindow(window), "Win32 read failure closes dialog");
  Require(broken.dialog.request.failed && !broken.dialog.request.accepted
              && broken.dialog.request.value == request.value, "Win32 failure retains original and reports failed");

  Require(native::StartEditor(request, false), "Start worker for message-loop failure");
  Worker worker;
  {
    std::shared_lock lock(native::state.mutex);
    Require(DuplicateHandle(GetCurrentProcess(), native::state.worker, GetCurrentProcess(),
                             &worker.handle, 0, FALSE, DUPLICATE_SAME_ACCESS), "Retain failure worker handle");
  }
  window = WaitForEditor(worker.handle);
  Require(PostThreadMessageW(GetThreadId(worker.handle), WM_QUIT, 0, 0), "Stop synthetic message loop before cancellation");
  Require(WaitForSingleObject(worker.handle, 5000) == WAIT_OBJECT_0 && !IsWindow(window), "Failed message loop destroys native window and exits");
  std::optional<text::Request> result;
  Require((result = text::TakeResult()) && result->failed && !result->accepted && result->value == request.value,
          "Async Win32 failure is consumable and preserves original");
}

void TestImeRouting() {
  for (const bool multiline : {false, true}) {
    auto request = Sample();
    request.multiline = multiline;
    Editor editor(request);
    HWND window = editor.dialog.window;
    HWND edit = GetDlgItem(window, native::TEXT_ID);
    HIMC context = ImmGetContext(edit);
    Require(context != nullptr && context == editor.dialog.ime_context, "EDIT has an independent IMM context");
    ImmReleaseContext(edit, context);
    for (const WPARAM key : {WPARAM(VK_RETURN), WPARAM(VK_ESCAPE)}) {
      SendMessageW(edit, WM_IME_STARTCOMPOSITION, 0, 0);
      Require(native::CompositionActive(&editor.dialog), "Subclass tracks composition before keyboard translation");
      MSG message{.hwnd = edit, .message = WM_KEYDOWN, .wParam = key};
      native::ProcessMessage(&editor.dialog, &message);
      SendMessageW(window, WM_COMMAND, native::APPLY_ID, 0);
      SendMessageW(window, WM_COMMAND, IDCANCEL, 0);
      Require(IsWindow(window) && !editor.dialog.request.accepted, "Composition never applies or cancels the editor");
      SendMessageW(edit, WM_IME_ENDCOMPOSITION, 0, 0);
      Require(!editor.dialog.composing, "Composition end reaches EDIT subclass");
      // IME may end composition during key-down; protect its command/key-up tail.
      SendMessageW(window, WM_COMMAND, native::APPLY_ID, 0);
      SendMessageW(window, WM_COMMAND, IDCANCEL, 0);
      Require(IsWindow(window), "Candidate-commit key tail cannot become a dialog action");
      message.message = WM_KEYUP;
      native::ProcessMessage(&editor.dialog, &message);
      Require(editor.dialog.ime_key == 0, "Protected key released");
      message.message = WM_CHAR;
      native::ProcessMessage(&editor.dialog, &message);
      Require(IsWindow(window), "Trailing IME character never closes the editor");
    }
    editor.dialog.ime_key = VK_ESCAPE;
    SendMessageW(edit, WM_KILLFOCUS, 0, 0);
    Require(editor.dialog.ime_key == 0 && !editor.dialog.composing, "Focus loss cannot leave a key-up gate latched");
    Require(IsWindow(window) && !editor.dialog.request.accepted, "Focus loss never publishes or closes");
    // No default IDOK or ordinary Enter can publish text, even after composition.
    SendMessageW(window, WM_COMMAND, IDOK, 0);
    MSG enter{.hwnd = edit, .message = WM_KEYDOWN, .wParam = VK_RETURN};
    native::ProcessMessage(&editor.dialog, &enter);
    Require(IsWindow(window) && !editor.dialog.request.accepted, "Only explicit Apply publishes text");
    Require(SetWindowTextW(edit, L"中文😀"), "Set final committed text");
    SendMessageW(window, WM_COMMAND, native::APPLY_ID, 0);
    Require(editor.dialog.request.accepted && editor.dialog.request.value == "中文😀", "Explicit Apply works after IME key release");
    Require(editor.dialog.ime_context == nullptr, "Destroy releases private IMM context");
  }
}

void TestOwner() {
  HWND owner = CreateWindowExW(0, L"STATIC", L"Synthetic editor owner", WS_OVERLAPPED,
                               0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  Require(owner != nullptr, "Create test-only owner");
  auto request = Sample();
  request.return_window = owner;
  {
    Editor editor(request);
    Require(GetWindow(editor.dialog.window, GW_OWNER) == nullptr, "Return target never becomes native owner");
    Require(IsWindowEnabled(owner), "Modeless editor never disables owner");
    SendMessageW(editor.dialog.window, WM_CLOSE, 0, 0);
    Require(IsWindow(owner) && IsWindowEnabled(owner), "Closing editor preserves owner");
    Require(!editor.dialog.request.accepted && !editor.dialog.request.failed,
            "Owned dialog close is cancellation");
  }
  Require(DestroyWindow(owner), "Destroy only synthetic owner");
  Require(!text::Start(request), "Stale owner rejected before starting worker");
}

void TestCursorQueueIsolation() {
  HWND target = CreateWindowExW(0, L"STATIC", L"Synthetic hidden game", WS_OVERLAPPED,
                                0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  Require(target != nullptr, "Create hidden cursor isolation target");
  auto request = Sample();
  request.return_window = target;
  std::atomic<bool> finished = false;
  bool opened = false;
  int before = 0;
  int after = 0;
  ShowCursor(FALSE);
  std::thread worker([&]() {
    try {
      before = ShowCursor(TRUE) - 1;
      ShowCursor(FALSE);
      native::Dialog dialog;
      if (native::PrepareDialog(request, &dialog)) {
        dialog.module = GetModuleHandleW(nullptr);
        if (native::OpenEditor(&dialog) != nullptr) {
          opened = true;
          after = ShowCursor(TRUE) - 1;
          ShowCursor(FALSE);
          DestroyWindow(dialog.window);
        }
      }
    } catch (...) {
      opened = false;
    }
    finished.store(true);
  });
  while (!finished.load()) {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    Sleep(1);
  }
  worker.join();
  ShowCursor(TRUE);
  Require(DestroyWindow(target), "Destroy hidden cursor isolation target");
  Require(opened, "Open hidden production editor on separate thread");
  Require(before == after, "Return target must not merge hidden game cursor count into editor queue");
}

// Intercept only this test EDIT's clipboard commands. Selection/replacement are
// still handled by the real Windows EDIT; no OS clipboard API is called.
struct ClipboardFixture {
  HWND edit;
  std::wstring contents = L"粘贴😀";
  unsigned copies = 0;
  unsigned pastes = 0;
  bool failed = false;

  static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM value, LPARAM data,
                                   UINT_PTR, DWORD_PTR reference) {
    auto* fixture = reinterpret_cast<ClipboardFixture*>(reference);
    try {
      if (message == WM_COPY) {
        DWORD begin = 0, end = 0;
        SendMessageW(window, EM_GETSEL, reinterpret_cast<WPARAM>(&begin), reinterpret_cast<LPARAM>(&end));
        const int length = GetWindowTextLengthW(window);
        std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
        GetWindowTextW(window, text.data(), length + 1);
        fixture->contents = text.substr(begin, end - begin);
        ++fixture->copies;
        return 0;
      }
      if (message == WM_PASTE) {
        SendMessageW(window, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(fixture->contents.c_str()));
        ++fixture->pastes;
        return 0;
      }
    } catch (...) {
      fixture->failed = true;
      return 0;
    }
    return DefSubclassProc(window, message, value, data);
  }

  explicit ClipboardFixture(HWND window) : edit(window) {
    Require(SetWindowSubclass(edit, Procedure, 2, reinterpret_cast<DWORD_PTR>(this)), "Install private clipboard fixture");
  }
  ~ClipboardFixture() { RemoveWindowSubclass(edit, Procedure, 2); }
};

void TestCtrlShortcuts() {
  struct KeyboardState {
    BYTE previous[256]{};
    KeyboardState() {
      Require(GetKeyboardState(previous), "Read test-thread keyboard state");
      BYTE zero[256]{};
      Require(SetKeyboardState(zero), "Zero only test-thread consumer keyboard state");
    }
    ~KeyboardState() { SetKeyboardState(previous); }
  } keyboard_state;
  for (const bool multiline : {false, true}) {
    auto request = Sample();
    request.value = "base";
    request.multiline = multiline;
    Editor editor(request);
    HWND edit = GetDlgItem(editor.dialog.window, native::TEXT_ID);
    ClipboardFixture clipboard(edit);
    SetFocus(edit);
    Require(GetFocus() == edit, "Hidden EDIT owns focus on its test thread");
    const auto route = [&](UINT key, bool released = false, LPARAM flags = 0) {
      Require(PostMessageW(edit, released ? WM_KEYUP : WM_KEYDOWN, key, flags | 1), "Post actual shortcut MSG");
      MSG message{};
      while (PeekMessageW(&message, edit, 0, 0, PM_REMOVE)) {
        native::ProcessMessage(&editor.dialog, &message);
      }
    };
    SendMessageW(edit, EM_SETSEL, 0, -1);
    route(VK_CONTROL);
    BYTE observed[256]{};
    Require(GetKeyboardState(observed) && observed[VK_CONTROL] == 0 && GetKeyState(VK_CONTROL) == 0,
            "Consumer sees zero Ctrl state despite queued Ctrl key-down");
    route('V');
    if (clipboard.pastes == 0) {
      std::string actual;
      native::WideToUtf8(ReadText(edit), &actual);
      std::cout << "Zero-key-state reproduction: Ctrl+V reached real EDIT as [" << actual
                << "]; private WM_PASTE count=" << clipboard.pastes << "\n";
    }
    Require(clipboard.pastes == 1 && ReadText(edit) == L"粘贴😀", "Ctrl+V pastes Unicode exactly once with zero key state");
    SendMessageW(edit, EM_SETSEL, 0, 2);
    route('C');
    Require(clipboard.copies == 1 && clipboard.contents == L"粘贴" && ReadText(edit) == L"粘贴😀",
            "Ctrl+C copies selected Unicode without inserting a letter");
    route(VK_CONTROL, true);
    route('V');
    Require(clipboard.pastes == 1, "Released Ctrl no longer pastes");
    route(VK_CONTROL);
    SetFocus(GetDlgItem(editor.dialog.window, native::APPLY_ID));
    Require(GetFocus() != edit, "Move actual thread focus away from EDIT");
    SetFocus(edit);
    route('V');
    Require(clipboard.pastes == 1, "Focus loss clears shortcut modifier without needing key-up");

    route(VK_CONTROL);
    SendMessageW(edit, WM_IME_STARTCOMPOSITION, 0, 0);
    route('V');
    route('C');
    Require(clipboard.pastes == 1 && clipboard.copies == 1, "IME composition retains native C/V handling");
    route(VK_RETURN);
    SendMessageW(edit, WM_IME_ENDCOMPOSITION, 0, 0);
    route('V');
    Require(clipboard.pastes == 1, "IME candidate key tail cannot become a paste");
    route(VK_RETURN, true);
    route('V');
    Require(clipboard.pastes == 2, "Paste resumes after IME candidate key release");

    route(VK_MENU);
    route('V');
    Require(clipboard.pastes == 2, "Ctrl+Alt/AltGr is not claimed as Ctrl+V");
    route(VK_MENU, true);
    route('V');
    Require(clipboard.pastes == 3, "Alt release restores plain Ctrl+V");

    // Real Windows key messages use VK_CONTROL plus the extended-key bit for
    // right Ctrl. Releasing one side must not clear the other held modifier.
    route(VK_CONTROL, false, 1L << 24);
    route(VK_CONTROL, true);
    route('V');
    Require(clipboard.pastes == 4, "Right Ctrl survives left Ctrl release");
    route(VK_CONTROL, true, 1L << 24);
    route('V');
    Require(clipboard.pastes == 4, "Both Ctrl releases remove shortcut state");

    SetFocus(GetDlgItem(editor.dialog.window, native::APPLY_ID));
    route(VK_CONTROL);
    SetFocus(edit);
    route('V');
    Require(clipboard.pastes == 4, "Ctrl message without EDIT focus cannot arm a shortcut");

    SendMessageW(edit, WM_IME_KEYDOWN, VK_CONTROL, 1);
    SendMessageW(edit, EM_SETSEL, 0, -1);
    SendMessageW(edit, WM_IME_KEYDOWN, 'V', 1);
    Require(clipboard.pastes == 5, "IME-delivered Ctrl+V pastes once despite masked consumer key state");
    SendMessageW(edit, EM_SETSEL, 0, 2);
    SendMessageW(edit, WM_IME_KEYDOWN, 'C', 1);
    Require(clipboard.copies == 2 && clipboard.contents == L"粘贴",
            "IME-delivered Ctrl+C copies selected Unicode instead of typing c");
    SendMessageW(edit, WM_IME_KEYUP, VK_CONTROL, (1L << 30) | (1LL << 31) | 1);
    SendMessageW(edit, WM_IME_KEYDOWN, 'V', 1);
    Require(clipboard.pastes == 5, "IME-delivered Ctrl release cannot leave paste armed");

    SendMessageW(edit, WM_IME_KEYDOWN, VK_CONTROL, 1);
    SendMessageW(edit, WM_IME_STARTCOMPOSITION, 0, 0);
    SendMessageW(edit, WM_IME_KEYDOWN, 'V', 1);
    Require(clipboard.pastes == 5, "IME-delivered V during composition remains with the input method");
    SendMessageW(edit, WM_IME_ENDCOMPOSITION, 0, 0);
    SendMessageW(edit, WM_IME_KEYDOWN, VK_MENU, 1);
    SendMessageW(edit, WM_IME_KEYDOWN, 'V', 1);
    Require(clipboard.pastes == 5, "IME-delivered Ctrl+Alt is not treated as paste");
    SendMessageW(edit, WM_IME_KEYUP, VK_MENU, 1);
    SendMessageW(edit, WM_KILLFOCUS, 0, 0);
    SendMessageW(edit, WM_IME_KEYDOWN, 'V', 1);
    Require(clipboard.pastes == 5, "Focus loss clears modifiers learned through IME messages");

    ime_key_fixture::window = edit;
    MSG processed{.hwnd = edit, .message = WM_KEYDOWN, .wParam = VK_PROCESSKEY, .lParam = 1};
    ime_key_fixture::key = VK_CONTROL;
    native::ProcessMessage(&editor.dialog, &processed);
    SendMessageW(edit, EM_SETSEL, 0, -1);
    ime_key_fixture::key = 'V';
    native::ProcessMessage(&editor.dialog, &processed);
    Require(clipboard.pastes == 6 && processed.wParam == VK_PROCESSKEY,
            "IMM original key enables one paste without rewriting the source MSG");
    SendMessageW(edit, EM_SETSEL, 0, 2);
    ime_key_fixture::key = 'C';
    native::ProcessMessage(&editor.dialog, &processed);
    Require(clipboard.copies == 3 && clipboard.contents == L"粘贴", "IMM original C copies the current selection");
    ime_key_fixture::key = VK_PROCESSKEY;
    native::ProcessMessage(&editor.dialog, &processed);
    Require(clipboard.pastes == 6 && clipboard.copies == 3, "Unavailable IMM key cannot become a shortcut");
    ime_key_fixture::key = 'V';
    SendMessageW(edit, WM_IME_STARTCOMPOSITION, 0, 0);
    native::ProcessMessage(&editor.dialog, &processed);
    Require(clipboard.pastes == 6, "IMM recovered key does not override active composition");
    SendMessageW(edit, WM_IME_ENDCOMPOSITION, 0, 0);
    route(VK_CONTROL, true);
    const unsigned before_released_key = clipboard.pastes;
    Require(editor.dialog.control_keys == 0, "IME shortcut Ctrl release clears delivered modifier state");
    native::ProcessMessage(&editor.dialog, &processed);
    Require(clipboard.pastes == before_released_key, "IMM recovered key requires a delivered Ctrl modifier");
    ime_key_fixture::window = nullptr;
    SendMessageW(edit, WM_KILLFOCUS, 0, 0);
    Require(IsWindow(editor.dialog.window) && !editor.dialog.request.accepted,
            "Shortcut tests never Apply or close the editor");
    Require(!clipboard.failed, "Private clipboard fixture completed without errors");
  }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    if (argc == 2 && (std::wstring_view(argv[1]) == L"--manual"
                      || std::wstring_view(argv[1]) == L"--manual-single"
                      || std::wstring_view(argv[1]) == L"--interactive"
                      || std::wstring_view(argv[1]) == L"--interactive-single")) {
      auto request = Sample();
      request.title = "NR Nexus Unicode input — interactive IME test";
      request.label = "Test IME Enter/Escape, focus, multiline, Apply/Cancel";
      request.multiline = std::wstring_view(argv[1]) == L"--manual"
                           || std::wstring_view(argv[1]) == L"--interactive";
      request.max_bytes = 4096;
      Require(text::Start(request), "Start visible manual editor");
      std::optional<text::Request> result;
      while (!(result = text::TakeResult())) Sleep(20);
      std::cout << "accepted=" << result->accepted << " failed=" << result->failed
                << " bytes=" << result->value.size() << "\n" << result->value << "\n";
      return result->failed ? 1 : 0;
    }
    Require(argc == 2, "Pass test module path");
    TestStrictConversion();
    TestNativeCharacters();
    TestLimitsAndInvalidText();
    TestCancelAndMultiline();
    TestImeRouting();
    TestOwner();
    TestCursorQueueIsolation();
    TestWorkerAndResults();
    TestModuleLifetime(argv[1]);
    TestWin32Failure();
    TestCtrlShortcuts();
    HWND leftover = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), FindEditor, reinterpret_cast<LPARAM>(&leftover));
    Require(leftover == nullptr && !text::Busy(), "No hidden real editor remains at test completion");
    std::cout << "Unicode input: " << checks << " checks passed; all synthetic dialogs hidden and closed.\n"
              << "True OS IME composition/candidate UI still requires manual testing.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Unicode input failure: " << error.what() << '\n';
    return 1;
  }
}

#endif
