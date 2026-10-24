#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

#include <wrl/client.h>

#include <utils/directx.hpp>
#include <utils/vtable.hpp>

namespace {

namespace directx = renodx::utils::directx;
namespace vtable = renodx::utils::vtable;
using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;

static_assert(std::is_same_v<decltype(vtable::TransactionMutex()), std::mutex&>);
static_assert(std::is_same_v<decltype(vtable::transaction_contended), std::atomic<std::uint64_t>>);

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

struct __declspec(uuid("673442AB-40F7-470C-BF69-90AA51F88A81")) FirstFace : IUnknown {};
struct __declspec(uuid("EB22CBDE-FCD5-417F-83C0-93DBD65E2822")) SecondFace : IUnknown {};

// Stack lifetime makes reference accounting observable without hiding leaks
// behind destruction. Both distinct interface addresses share FirstFace identity.
struct FakeDevice final : FirstFace, SecondFace {
  ULONG references = 1;
  ULONG adds = 0;
  ULONG releases = 0;
  bool fail_identity = false;
  bool null_identity = false;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
    if (object == nullptr) return E_POINTER;
    *object = nullptr;
    if (iid == IID_IUnknown && fail_identity) return E_FAIL;
    if (iid == IID_IUnknown && null_identity) return S_OK;
    if (iid == IID_IUnknown || iid == __uuidof(FirstFace)) {
      *object = static_cast<FirstFace*>(this);
    } else if (iid == __uuidof(SecondFace)) {
      *object = static_cast<SecondFace*>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  ULONG STDMETHODCALLTYPE AddRef() override {
    ++adds;
    return ++references;
  }

  ULONG STDMETHODCALLTYPE Release() override {
    ++releases;
    return --references;
  }
};

constexpr GUID STREAMLINE_BASE_IID = {
    0xADEC44E2, 0x61F0, 0x45C3, {0xAD, 0x9F, 0x1B, 0x37, 0x37, 0x92, 0x84, 0xFF}};

struct FakeProxy final : IUnknown {
  IUnknown* base = nullptr;
  bool reshade = false;
  bool null_base = false;
  bool fail_base = false;
  ULONG references = 1;
  ULONG adds = 0;
  ULONG releases = 0;
  ULONG streamline_queries = 0;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
    if (object == nullptr) return E_POINTER;
    *object = nullptr;
    if (iid == IID_IUnknown) {
      *object = static_cast<IUnknown*>(this);
      AddRef();
      return S_OK;
    }
    if (iid == STREAMLINE_BASE_IID) {
      ++streamline_queries;
      *object = base;
      base->AddRef();
      return S_OK;
    }
    if (iid == __uuidof(directx::ReShadeRetrieveBaseInterface) && reshade) {
      if (fail_base) return E_FAIL;
      if (null_base) return S_OK;
      *object = base;
      base->AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override {
    ++adds;
    return ++references;
  }

  ULONG STDMETHODCALLTYPE Release() override {
    ++releases;
    return --references;
  }
};

void TestFakeIdentity() {
  FakeDevice device;
  FakeDevice other;
  auto* first = static_cast<FirstFace*>(&device);
  auto* second = static_cast<SecondFace*>(&device);
  Check(static_cast<IUnknown*>(first) != static_cast<IUnknown*>(second), "fake faces must differ");
  Check(directx::NativeIdentity(second) == static_cast<IUnknown*>(first), "canonical IID_IUnknown");
  Check(directx::SameNativeObject(first, second), "same object across interfaces");
  Check(!directx::SameNativeObject(first, static_cast<FirstFace*>(&other)), "different devices");

  FakeProxy reshade;
  reshade.base = second;
  reshade.reshade = true;
  Check(directx::NativeIdentity(&reshade) == static_cast<IUnknown*>(first), "ReShade base canonicalized");
  Check(directx::SameNativeObject(&reshade, first), "ReShade proxy matches native");
  Check(directx::SameNativeObject(first, &reshade), "ReShade match symmetric");

  FakeProxy streamline;
  streamline.base = first;
  Check(directx::NativeIdentity(&streamline) == &streamline, "non-ReShade face preserved");
  Check(!directx::SameNativeObject(&streamline, first), "Streamline face independent of base");
  reshade.base = &streamline;
  Check(directx::NativeIdentity(&reshade) == &streamline, "ReShade unwrap stops at Streamline face");
  Check(streamline.streamline_queries == 0, "Streamline private IID must never be queried");

  Check(directx::NativeIdentity(nullptr) == nullptr, "null identity");
  Check(!directx::SameNativeObject(nullptr, nullptr), "null-null is not a match");
  Check(!directx::SameNativeObject(first, nullptr), "null right is not a match");
  Check(!directx::SameNativeObject(nullptr, first), "null left is not a match");
  device.fail_identity = true;
  Check(directx::NativeIdentity(first) == nullptr, "failed canonical query");
  Check(!directx::SameNativeObject(first, first), "failed identities cannot match");
  reshade.base = first;
  Check(directx::NativeIdentity(&reshade) == nullptr, "failed native query after unwrap");
  device.fail_identity = false;
  device.null_identity = true;
  Check(directx::NativeIdentity(first) == nullptr, "successful null canonical query safe");
  device.null_identity = false;
  reshade.null_base = true;
  Check(directx::NativeIdentity(&reshade) == &reshade, "null private base preserves face");
  reshade.null_base = false;
  reshade.fail_base = true;
  Check(directx::NativeIdentity(&reshade) == &reshade, "failed private query preserves face");
  Check(!directx::NativeFromReShadeProxy<IUnknown>(nullptr), "null unwrap output safe");
  IUnknown* absent = nullptr;
  Check(!directx::NativeFromReShadeProxy(&absent), "null unwrap input safe");

  for (int iteration = 0; iteration < 1000; ++iteration) {
    Check(directx::SameNativeObject(first, second), "repeated canonical match");
  }
  Check(device.references == 1 && device.adds == device.releases && device.adds > 0, "native refs balanced");
  Check(other.references == 1 && other.adds == other.releases, "other device refs balanced");
  Check(reshade.references == 1 && reshade.adds == reshade.releases, "ReShade refs balanced");
  Check(streamline.references == 1 && streamline.adds == streamline.releases, "Streamline refs balanced");
  std::cout << "PASS fake COM identity, ReShade-only unwrap, failure/null and reference balance\n";
}

void TestWarpIdentity() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11Device> other;
  Check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                  D3D11_SDK_VERSION, &device, nullptr, nullptr)), "D3D11 WARP creation");
  Check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                  D3D11_SDK_VERSION, &other, nullptr, nullptr)), "second D3D11 WARP creation");
  ComPtr<IDXGIDevice> dxgi_device;
  ComPtr<IUnknown> canonical;
  Check(SUCCEEDED(device.As(&dxgi_device)) && SUCCEEDED(device.As(&canonical)), "D3D11 interfaces");
  Check(directx::NativeIdentity(dxgi_device.Get()) == canonical.Get(), "real D3D11 canonical identity");
  Check(directx::SameNativeObject(device.Get(), dxgi_device.Get()), "real D3D11 same interface identity");
  Check(!directx::SameNativeObject(device.Get(), other.Get()), "real D3D11 distinct WARP devices");
  std::cout << "PASS D3D11 WARP same-device interfaces and distinct devices\n";

  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter> adapter;
  ComPtr<ID3D12Device> device12;
  HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  if (SUCCEEDED(result)) result = factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
  if (SUCCEEDED(result)) result = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12));
  if (FAILED(result)) {
    std::cout << "SKIP D3D12 WARP unavailable: HRESULT=0x" << std::hex
              << static_cast<unsigned long>(result) << std::dec << '\n';
    return;
  }
  ComPtr<ID3D12Object> object12;
  ComPtr<IUnknown> canonical12;
  Check(SUCCEEDED(device12.As(&object12)) && SUCCEEDED(device12.As(&canonical12)), "D3D12 interfaces");
  Check(directx::NativeIdentity(object12.Get()) == canonical12.Get(), "real D3D12 canonical identity");
  Check(directx::SameNativeObject(device12.Get(), object12.Get()), "real D3D12 same interface identity");
  std::cout << "PASS D3D12 WARP same-device interfaces\n";
}

void TestForeignTransaction() {
  const auto initial_count = vtable::transaction_contended.load(std::memory_order_relaxed);
  std::promise<LONG> started;
  std::promise<void> release;
  auto started_future = started.get_future();
  auto release_future = release.get_future();
  LONG foreign_abort = ERROR_INVALID_OPERATION;
  bool holder_timed_out = false;
  std::thread holder([&] {
    const LONG result = DetourTransactionBegin();
    started.set_value(result);
    if (result != NO_ERROR) return;
    holder_timed_out = release_future.wait_for(10s) != std::future_status::ready;
    foreign_abort = DetourTransactionAbort();
  });
  const bool started_in_time = started_future.wait_for(5s) == std::future_status::ready;
  const LONG foreign_begin = (started_in_time ? started_future.get() : ERROR_TIMEOUT);
  bool collision_accepted = false;
  bool second_collision_accepted = false;
  auto elapsed = std::chrono::steady_clock::duration::zero();
  if (foreign_begin == NO_ERROR) {
    std::lock_guard<std::mutex> lock(vtable::TransactionMutex());
    const auto before = std::chrono::steady_clock::now();
    collision_accepted = vtable::BeginTransaction();
    second_collision_accepted = vtable::BeginTransaction();
    elapsed = std::chrono::steady_clock::now() - before;
  }
  const auto collision_count = vtable::transaction_contended.load(std::memory_order_relaxed);
  release.set_value();
  holder.join();
  Check(started_in_time && foreign_begin == NO_ERROR && !holder_timed_out, "foreign holder handshake");
  Check(!collision_accepted && !second_collision_accepted, "foreign collisions must fail");
  Check(collision_count == initial_count + 2, "each collision counted exactly once");
  Check(elapsed < 2s, "collision must return promptly without waiting for holder");
  Check(foreign_abort == NO_ERROR, "foreign transaction still owned and abortable");
  {
    std::lock_guard<std::mutex> lock(vtable::TransactionMutex());
    Check(vtable::BeginTransaction(), "begin recovers after foreign abort");
    const LONG update = DetourUpdateThread(GetCurrentThread());
    const LONG abort = DetourTransactionAbort();
    Check(update == NO_ERROR && abort == NO_ERROR, "caller update/abort works");
  }
  Check(vtable::transaction_contended.load(std::memory_order_relaxed) == collision_count,
        "successful begin does not increment contention");
  std::cout << "PASS real Detours foreign-thread collision, count +2, foreign abort and recovery\n";
}

void TestSerialTransactions() {
  const auto initial_count = vtable::transaction_contended.load(std::memory_order_relaxed);
  std::atomic<unsigned> failures{0};
  std::atomic<unsigned> completed{0};
  std::promise<void> start;
  auto gate = start.get_future().share();
  std::vector<std::thread> workers;
  for (unsigned worker = 0; worker < 8; ++worker) {
    workers.emplace_back([&] {
      gate.wait();
      for (unsigned iteration = 0; iteration < 100; ++iteration) {
        std::lock_guard<std::mutex> lock(vtable::TransactionMutex());
        if (!vtable::BeginTransaction()) {
          failures.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        const LONG update = DetourUpdateThread(GetCurrentThread());
        const LONG abort = DetourTransactionAbort();
        if (update != NO_ERROR || abort != NO_ERROR) {
          failures.fetch_add(1, std::memory_order_relaxed);
        } else {
          completed.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  start.set_value();
  for (auto& worker : workers) worker.join();
  Check(failures.load() == 0 && completed.load() == 800, "externally locked transactions serialize");
  Check(vtable::transaction_contended.load(std::memory_order_relaxed) == initial_count,
        "external mutex prevents internal collisions");
  {
    std::lock_guard<std::mutex> lock(vtable::TransactionMutex());
    Check(vtable::BeginTransaction(), "begin before empty commit");
    const LONG update = DetourUpdateThread(GetCurrentThread());
    const LONG commit = DetourTransactionCommit();
    Check(update == NO_ERROR && commit == NO_ERROR, "caller empty commit works without hooks");
  }
  std::cout << "PASS real Detours 8 threads x 100 externally locked begin/update/abort; empty commit\n";
}

}  // namespace

int main() {
  try {
    TestFakeIdentity();
    TestWarpIdentity();
    TestForeignTransaction();
    TestSerialTransactions();
    std::cout << "PASS all native contracts (compatibility candidate; no hooks installed)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
