#define RESHADE_ADDON 1
#include <windows.h>
#include <include/reshade.hpp>

#include <fg_xefg.hpp>

extern "C" __declspec(dllexport) void Install() {
  renodx::addons::dlss5::fg::xefg::Poll();
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_DETACH) {
    renodx::addons::dlss5::fg::xefg::Stop();
  }
  return TRUE;
}
