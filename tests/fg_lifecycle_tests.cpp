#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
constexpr std::array<const char*, 8> EXPORTS{
    "xefgSwapChainD3D12InitFromSwapChain", "xefgSwapChainD3D12InitFromSwapChainDesc",
    "xefgSwapChainD3D12GetSwapChainPtr", "xefgSwapChainD3D12TagFrameResource",
    "xefgSwapChainTagFrameConstants", "xefgSwapChainSetPresentId", "xefgSwapChainSetEnabled",
    "xefgSwapChainDestroy"};
uint32_t checks = 0;
void Check(bool passed, const char* message) {
  ++checks;
  std::printf("%s: %s (win32=%lu)\n", passed ? "PASS" : "FAIL", message, GetLastError());
  if (!passed) throw std::runtime_error(message);
}
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  try {
    auto sdk = LoadLibraryW(L".\\libxess_fg.dll");
    Check(sdk != nullptr, "load real fake-SDK DLL");
    std::array<FARPROC, EXPORTS.size()> exports{};
    std::array<std::array<uint8_t, 5>, EXPORTS.size()> pristine{};
    for (size_t i = 0; i < exports.size(); ++i) {
      exports[i] = GetProcAddress(sdk, EXPORTS[i]);
      Check(exports[i] != nullptr, EXPORTS[i]);
      std::memcpy(pristine[i].data(), reinterpret_cast<const void*>(exports[i]), pristine[i].size());
    }
    auto calls = reinterpret_cast<uint32_t (*)()>(GetProcAddress(sdk, "FixtureCalls"));
    Check(calls != nullptr, "resolve original-call counter");
    Check(GetProcAddress(sdk, "xefgSwapChainSetUiCompositionState") == nullptr, "optional UI setter absent");
    auto invoke_all = [&]() {
      Check(reinterpret_cast<int32_t (*)(void*, ID3D12CommandQueue*, const void*)>(exports[0])(
                nullptr, nullptr, nullptr) == 77, "Init original returns 77");
      Check(reinterpret_cast<int32_t (*)(void*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, ID3D12CommandQueue*, IDXGIFactory2*, const void*)>(exports[1])(
                nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) == 77, "InitDesc original returns 77");
      Check(reinterpret_cast<int32_t (*)(void*, REFIID, void**)>(exports[2])(
                nullptr, IID_IUnknown, nullptr) == 77, "GetSwapchain original returns 77");
      Check(reinterpret_cast<int32_t (*)(void*, ID3D12CommandList*, uint32_t, const void*)>(exports[3])(
                nullptr, nullptr, 1, nullptr) == 77, "Resource original returns 77");
      Check(reinterpret_cast<int32_t (*)(void*, uint32_t, const void*)>(exports[4])(
                nullptr, 1, nullptr) == 77, "Constants original returns 77");
      Check(reinterpret_cast<int32_t (*)(void*, uint32_t)>(exports[5])(nullptr, 1) == 77, "PresentId original returns 77");
      Check(reinterpret_cast<int32_t (*)(void*, uint32_t)>(exports[6])(nullptr, 1) == 77, "Enable original returns 77");
      Check(reinterpret_cast<int32_t (*)(void*)>(exports[7])(nullptr) == 77, "Destroy original returns 77");
    };
    auto fixture = LoadLibraryW(L".\\fg_lifecycle_fixture.dll");
    Check(fixture != nullptr, "load adapter DLL for temporary-device lifetime");
    auto install = reinterpret_cast<void (*)()>(GetProcAddress(fixture, "Install"));
    Check(install != nullptr, "resolve Install");
    install();
    for (size_t i = 0; i < exports.size(); ++i) {
      Check(std::memcmp(pristine[i].data(), reinterpret_cast<const void*>(exports[i]), 5) != 0,
            "real Detours changed SDK export entry");
    }
    Check(calls() == 0, "Poll invoked no public SDK callbacks");
    Check(FreeLibrary(fixture) != FALSE, "FreeLibrary temporary adapter");
    Check(GetModuleHandleW(L"fg_lifecycle_fixture.dll") == nullptr,
          "temporary adapter really unloaded; no premature runtime PIN");
    invoke_all();
    Check(calls() == 8, "all eight SDK exports survive Neutralize and adapter unload");

    fixture = LoadLibraryW(L".\\fg_lifecycle_fixture.dll");
    Check(fixture != nullptr, "reload adapter DLL for active lifetime");
    install = reinterpret_cast<void (*)()>(GetProcAddress(fixture, "Install"));
    Check(install != nullptr, "resolve reloaded Install");
    std::array<std::array<uint8_t, 5>, EXPORTS.size()> neutralized{};
    for (size_t i = 0; i < exports.size(); ++i) {
      std::memcpy(neutralized[i].data(), reinterpret_cast<const void*>(exports[i]), 5);
    }
    install();
    for (size_t i = 0; i < exports.size(); ++i) {
      Check(std::memcmp(neutralized[i].data(), reinterpret_cast<const void*>(exports[i]), 5) != 0,
            "reloaded adapter installed a fresh real detour");
    }
    Check(calls() == 8, "reloaded Poll invoked no public SDK callbacks");
    Check(reinterpret_cast<int32_t (*)(void*, uint32_t, const void*)>(exports[4])(
              nullptr, 1, nullptr) == 77, "actual Constants callback forwards original 77");
    Check(calls() == 9, "actual Constants callback reached SDK once");
    Check(FreeLibrary(fixture) != FALSE, "FreeLibrary active adapter");
    Check(GetModuleHandleW(L"fg_lifecycle_fixture.dll") == fixture,
          "active adapter remains loaded by runtime callback PIN");
    invoke_all();
    Check(calls() == 17, "SDK callbacks remain valid after active FreeLibrary");
    std::printf("%u checks, 0 failures; process exit naturally reclaims pinned DLLs\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL after %u checks: %s\n", checks, error.what());
    return 1;
  }
}
