#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <atomic>
#include <cstdint>

namespace {
std::atomic_uint32_t calls{0};
}

// Opaque public SDK structures are never dereferenced in this boundary fixture.
// Atomic increments keep each export long enough for real x64 Detours relocation.
extern "C" __declspec(dllexport) uint32_t FixtureCalls() { return calls.load(); }
extern "C" __declspec(dllexport) __declspec(noinline) int32_t xefgSwapChainD3D12InitFromSwapChain(
    void*, ID3D12CommandQueue*, const void*) {
  ++calls;
  return 77;
}
extern "C" __declspec(dllexport) __declspec(noinline) int32_t xefgSwapChainD3D12InitFromSwapChainDesc(
    void*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
    ID3D12CommandQueue*, IDXGIFactory2*, const void*) {
  ++calls;
  return 77;
}
extern "C" __declspec(dllexport) __declspec(noinline) int32_t xefgSwapChainD3D12GetSwapChainPtr(
    void*, REFIID, void**) {
  ++calls;
  return 77;
}
extern "C" __declspec(dllexport) __declspec(noinline) int32_t xefgSwapChainD3D12TagFrameResource(
    void*, ID3D12CommandList*, uint32_t, const void*) {
  ++calls;
  return 77;
}
extern "C" __declspec(dllexport) __declspec(noinline) int32_t xefgSwapChainTagFrameConstants(
    void*, uint32_t, const void*) {
  ++calls;
  return 77;
}
extern "C" __declspec(dllexport) __declspec(noinline) int32_t xefgSwapChainSetPresentId(void*, uint32_t) {
  ++calls;
  return 77;
}
extern "C" __declspec(dllexport) __declspec(noinline) int32_t xefgSwapChainSetEnabled(void*, uint32_t) {
  ++calls;
  return 77;
}
extern "C" __declspec(dllexport) __declspec(noinline) int32_t xefgSwapChainDestroy(void*) {
  ++calls;
  return 77;
}
