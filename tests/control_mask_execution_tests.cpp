/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#include "d3d12_test_gpu.hpp"
#include <control_mask_execution.hpp>

#include <atomic>
#include <thread>
#include <string>

namespace {
namespace execution = renodx::addons::dlss5::control_mask_execution;
namespace submission = renodx::addons::dlss5::submission;
using d3d12_test::Check;
using Microsoft::WRL::ComPtr;

void Wait(ID3D12Fence* fence, uint64_t value) {
  if (fence->GetCompletedValue() >= value) return;
  const auto event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  Check(event != nullptr, "event");
  const auto hr = fence->SetEventOnCompletion(value, event);
  const auto waited = (SUCCEEDED(hr) ? WaitForSingleObject(event, 10000) : WAIT_FAILED);
  CloseHandle(event);
  Check(waited == WAIT_OBJECT_0 && fence->GetCompletedValue() != UINT64_MAX, "bounded GPU completion");
}

struct Slot {
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Resource> upload;
  ComPtr<ID3D12Resource> readback;
  ComPtr<ID3D12Fence> done;
  uint64_t serial = 0;
  uint32_t value = 0;
  uint32_t expected = 0;
  bool submitted = false;
};

// Called under ExecuteOrdered's lock: this records the actual CPU forward order,
// allowing both concurrent queue submissions and deliberately reversed replays.
std::vector<Slot*> observed_slots;
uint32_t history_value = 0;
uint64_t forward_calls = 0;
void STDMETHODCALLTYPE Forward(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
  ++forward_calls;
  for (UINT index = 0; index < count; ++index) {
    for (auto* slot : observed_slots) {
      if (slot->list.Get() == lists[index]) {
        slot->expected = history_value;
        history_value = slot->value;
      }
    }
  }
  queue->ExecuteCommandLists(count, lists);
}

struct Fixture {
  d3d12_test::Gpu gpu;
  ComPtr<ID3D12CommandQueue> other_queue;
  ComPtr<ID3D12Resource> history;
  std::array<Slot, 4> slots;
  std::shared_ptr<execution::Stream> stream;
  explicit Fixture(bool hardware = false) : gpu(hardware) {
    ComPtr<IDXGIAdapter1> adapter;
    const auto luid = gpu.device->GetAdapterLuid();
    Check(SUCCEEDED(gpu.factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))), "adapter identity");
    DXGI_ADAPTER_DESC1 adapter_desc{};
    adapter->GetDesc1(&adapter_desc);
    std::wcout << L"adapter=" << adapter_desc.Description << L" LUID=" << luid.HighPart << L':' << luid.LowPart
               << L" buffer_bytes=4 COPY_SOURCE/COPY_DEST independent_allocator_fence=1\n";
    const D3D12_COMMAND_QUEUE_DESC desc{};
    Check(SUCCEEDED(gpu.device->CreateCommandQueue(&desc, IID_PPV_ARGS(&other_queue))), "second queue");
    history = gpu.Buffer(4, D3D12_HEAP_TYPE_DEFAULT);
    auto zero = gpu.Buffer(4, D3D12_HEAP_TYPE_UPLOAD);
    uint32_t* mapped = nullptr;
    Check(SUCCEEDED(zero->Map(0, nullptr, reinterpret_cast<void**>(&mapped))), "zero map");
    *mapped = 0;
    zero->Unmap(0, nullptr);
    gpu.list->CopyBufferRegion(history.Get(), 0, zero.Get(), 0, 4);
    gpu.Transition(history.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    gpu.Submit();
    stream = execution::CreateStream(gpu.device.Get());
    Check(stream != nullptr, "stream fence creation");
    observed_slots.clear();
    history_value = 0;
    for (auto& slot : slots) {
      Check(SUCCEEDED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
          IID_PPV_ARGS(&slot.allocator))), "slot allocator");
      Check(SUCCEEDED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
          slot.allocator.Get(), nullptr, IID_PPV_ARGS(&slot.list))), "slot list");
      Check(SUCCEEDED(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&slot.done))), "slot fence");
      slot.upload = gpu.Buffer(4, D3D12_HEAP_TYPE_UPLOAD);
      slot.readback = gpu.Buffer(4, D3D12_HEAP_TYPE_READBACK);
      observed_slots.push_back(&slot);
    }
  }
  void Verify(Slot* slot) {
    if (!slot->submitted) return;
    Wait(slot->done.Get(), slot->serial);
    uint32_t* mapped = nullptr;
    Check(SUCCEEDED(slot->readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped))), "history read map");
    const auto actual = *mapped;
    slot->readback->Unmap(0, nullptr);
    Check(actual == slot->expected, "GPU shared history matches actual ordered predecessor");
  }
  void Record(Slot* slot, uint32_t value) {
    if (slot->submitted) {
      // Only wait for this allocator's reuse, never drain all four in the loop.
      Verify(slot);
      Check(SUCCEEDED(slot->allocator->Reset()), "slot allocator reuse");
      Check(SUCCEEDED(slot->list->Reset(slot->allocator.Get(), nullptr)), "slot reset");
      submission::OnCommandListReset(slot->list.Get());
      submission::PruneCompletedGenerations();
      execution::Prune();
      slot->submitted = false;
    }
    Check(execution::BeginRecording(stream, slot->list.Get()), "begin recording admitted");
    execution::BeginResult result{};
    Check(execution::BeginRecording(stream, slot->list.Get(), &result)
        && result == execution::BeginResult::SAME_RECORDING, "same generation evaluate reuse");
    slot->value = value;
    uint32_t* mapped = nullptr;
    Check(SUCCEEDED(slot->upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))), "slot upload map");
    *mapped = value;
    slot->upload->Unmap(0, nullptr);
    slot->list->CopyBufferRegion(slot->readback.Get(), 0, history.Get(), 0, 4);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {history.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST};
    slot->list->ResourceBarrier(1, &barrier);
    slot->list->CopyBufferRegion(history.Get(), 0, slot->upload.Get(), 0, 4);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    slot->list->ResourceBarrier(1, &barrier);
    Check(SUCCEEDED(slot->list->Close()), "close recording");
  }
  execution::ExecutionResult Submit(Slot* slot, ID3D12CommandQueue* queue) {
    ID3D12CommandList* lists[]{slot->list.Get()};
    const auto pending = submission::BeginSubmission(1, lists);
    const auto result = execution::ExecuteOrdered(queue, 1, lists, Forward);
    Check(SUCCEEDED(queue->Signal(slot->done.Get(), ++slot->serial)), "independent actual completion signal");
    submission::CompleteSubmission(pending, slot->done.Get(), slot->serial);
    slot->submitted = true;
    return result;
  }
  void Finish() {
    for (auto& slot : slots) {
      Verify(&slot);
      submission::OnCommandListDestroyed(slot.list.Get());
    }
    submission::PruneCompletedGenerations();
    Check(execution::ReleaseIfIdle(), "registry releases only after original tracker closure and completion");
    observed_slots.clear();
  }
};

// MOCK API failures are injected only after actual GPU work is independently
// complete. No invalid native Wait/Signal or device-removal experiment is used.
struct QueueMock final : ID3D12CommandQueue {
  ID3D12CommandQueue* real = nullptr;
  bool fail_wait = false;
  bool fail_signal = false;
  ID3D12Fence* failed_fence = nullptr;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override { return real->QueryInterface(iid, out); }
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* bytes, void* data) override {
    return real->GetPrivateData(guid, bytes, data);
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT bytes, const void* data) override {
    return real->SetPrivateData(guid, bytes, data);
  }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* data) override {
    return real->SetPrivateDataInterface(guid, data);
  }
  HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override { return real->SetName(name); }
  HRESULT STDMETHODCALLTYPE GetDevice(REFIID iid, void** out) override { return real->GetDevice(iid, out); }
  void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource*, UINT, const D3D12_TILED_RESOURCE_COORDINATE*,
      const D3D12_TILE_REGION_SIZE*, ID3D12Heap*, UINT, const D3D12_TILE_RANGE_FLAGS*,
      const UINT*, const UINT*, D3D12_TILE_MAPPING_FLAGS) override {}
  void STDMETHODCALLTYPE CopyTileMappings(ID3D12Resource*, const D3D12_TILED_RESOURCE_COORDINATE*,
      ID3D12Resource*, const D3D12_TILED_RESOURCE_COORDINATE*, const D3D12_TILE_REGION_SIZE*,
      D3D12_TILE_MAPPING_FLAGS) override {}
  void STDMETHODCALLTYPE ExecuteCommandLists(UINT count, ID3D12CommandList* const* lists) override {
    real->ExecuteCommandLists(count, lists);
  }
  void STDMETHODCALLTYPE SetMarker(UINT, const void*, UINT) override {}
  void STDMETHODCALLTYPE BeginEvent(UINT, const void*, UINT) override {}
  void STDMETHODCALLTYPE EndEvent() override {}
  HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence* fence, UINT64 value) override {
    return (fail_signal && fence == failed_fence ? E_FAIL : real->Signal(fence, value));
  }
  HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence* fence, UINT64 value) override {
    return (fail_wait ? E_FAIL : real->Wait(fence, value));
  }
  HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64* frequency) override { return real->GetTimestampFrequency(frequency); }
  HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64* gpu, UINT64* cpu) override { return real->GetClockCalibration(gpu, cpu); }
  D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc() override { return real->GetDesc(); }
};

void Ring(bool cross_queue, bool hardware) {
  Fixture fixture(hardware);
  uint64_t waits = 0;
  for (uint32_t index = 0; index < 64; ++index) {
    auto& slot = fixture.slots[index % 4];
    fixture.Record(&slot, index + 1);
    auto* queue = (cross_queue && index % 2 != 0 ? fixture.other_queue.Get() : fixture.gpu.queue.Get());
    const auto result = fixture.Submit(&slot, queue);
    Check(result.ordered_lists == 1 && result.unsafe_lists == 0 && result.signals == 1, "ring ordered");
    waits += result.waits;
    Check(execution::internal::active_recordings.load() <= 4, "closed completed registry generations do not accumulate");
  }
  Check(waits == (cross_queue ? 63 : 0), "actual same queue omits Wait; actual queue changes wait on published tail");
  Check(execution::GetStatus(fixture.stream).tail == 64, "one successful signal per execution");
  fixture.Finish();
  std::cout << (hardware ? "NATIVE_GPU" : "WARP") << " ring64 four allocators cross_queue=" << cross_queue
            << " waits=" << waits << " history_readback=PASS\n";
}

void ReplayBatchConcurrent() {
  Fixture fixture;
  for (uint32_t index = 0; index < 4; ++index) {
    fixture.Record(&fixture.slots[index], index + 1);
    Check(fixture.Submit(&fixture.slots[index], fixture.gpu.queue.Get()).ordered_lists == 1, "prime recording");
  }
  for (auto& slot : fixture.slots) fixture.Verify(&slot);
  Check(!execution::ReleaseIfIdle(), "completed open recordings retain stream for replay");
  ID3D12CommandList* lists[]{fixture.slots[0].list.Get(), fixture.slots[1].list.Get()};
  const auto pending = submission::BeginSubmission(2, lists);
  const auto batch = execution::ExecuteOrdered(fixture.other_queue.Get(), 2, lists, Forward);
  Check(batch.forward_calls == 2 && batch.ordered_lists == 2 && batch.signals == 2 && batch.replays == 2
      && batch.recording_order_deviations == 2 && batch.waits == 1, "batch splits at complete GPU boundaries");
  const auto status = execution::GetStatus(fixture.stream);
  execution::BeginResult reason{};
  Check(status.history_invalidated && !status.poisoned
      && !execution::BeginRecording(fixture.stream, fixture.slots[3].list.Get(), &reason)
      && reason == execution::BeginResult::HISTORY_INVALIDATED,
      "old replay invalidates CPU History admission without poisoning GPU ordering");
  auto& first = fixture.slots[0];
  Check(SUCCEEDED(fixture.other_queue->Signal(first.done.Get(), ++first.serial)), "batch actual fence");
  submission::CompleteSubmission(pending, first.done.Get(), first.serial);
  Wait(first.done.Get(), first.serial);
  for (auto& slot : fixture.slots) fixture.Verify(&slot);
  std::array<execution::ExecutionResult, 2> results{};
  std::thread left([&] { results[0] = fixture.Submit(&fixture.slots[2], fixture.gpu.queue.Get()); });
  std::thread right([&] { results[1] = fixture.Submit(&fixture.slots[3], fixture.other_queue.Get()); });
  left.join();
  right.join();
  Check(results[0].ordered_lists == 1 && results[1].ordered_lists == 1, "concurrent submission ordered");
  Check(execution::GetStatus(fixture.stream).tail == 8 && !execution::GetStatus(fixture.stream).poisoned,
      "history invalidation still sequences every existing replay");
  std::weak_ptr<execution::Stream> retained = fixture.stream;
  fixture.stream.reset();
  execution::Prune();
  Check(!retained.expired(), "registry owns stream while old recording remains replayable");
  fixture.Finish();
  Check(retained.expired(), "closed and tracker-complete recordings release final stream reference");
  std::cout << "WARP batch/reversed-old-replay/concurrent-submit history_readback=PASS; CPU History not tested\n";
}

void Admission() {
  Fixture fixture;
  auto& first = fixture.slots[0];
  auto& second = fixture.slots[1];
  Check(execution::BeginRecording(fixture.stream, first.list.Get()), "first recording");
  execution::BeginResult reason{};
  Check(!execution::BeginRecording(fixture.stream, second.list.Get(), &reason)
      && reason == execution::BeginResult::PREVIOUS_UNSUBMITTED, "unsubmitted recording rejected without CPU wait");
  Check(SUCCEEDED(first.list->Close()), "discard close");
  Check(SUCCEEDED(first.list->Reset(first.allocator.Get(), nullptr)), "discard reset");
  submission::OnCommandListReset(first.list.Get());
  Check(execution::BeginRecording(fixture.stream, second.list.Get(), &reason)
      && reason == execution::BeginResult::DISCARDED_PREVIOUS, "closed never submitted permits caller reconstruction");
  fixture.Finish();
  std::cout << "WARP admission/discard/same-generation contract=PASS; caller History reconstruction not implemented here\n";
}

void Failures(bool wait_failure) {
  Fixture fixture;
  auto& slot = fixture.slots[0];
  fixture.Record(&slot, 5);
  fixture.Submit(&slot, fixture.gpu.queue.Get());
  fixture.Verify(&slot);  // Failure injection starts only after actual completion.
  const auto tail = execution::GetStatus(fixture.stream).tail;
  QueueMock mock;
  mock.real = fixture.other_queue.Get();
  mock.fail_wait = wait_failure;
  mock.fail_signal = !wait_failure;
  mock.failed_fence = fixture.stream->fence.Get();
  ID3D12CommandList* lists[]{slot.list.Get()};
  const auto pending = submission::BeginSubmission(1, lists);
  const auto result = execution::ExecuteOrdered(&mock, 1, lists, Forward);
  Check(SUCCEEDED(mock.real->Signal(slot.done.Get(), ++slot.serial)), "failure independent real signal");
  Wait(slot.done.Get(), slot.serial);
  Check(result.forward_calls == 1 && result.forwarded_lists == 1 && result.unsafe_lists == 1
      && result.ordered_lists == 0 && FAILED(result.failure), "failure forwards once and reports unsafe");
  Check(execution::GetStatus(fixture.stream).tail == tail && execution::GetStatus(fixture.stream).poisoned,
      "failure never publishes success tail");
  Check(!execution::BeginRecording(fixture.stream, fixture.slots[1].list.Get()), "poison rejects new recording");
  fixture.Verify(&slot);  // Independent legacy actual fence, not the failed mock signal.
  const auto continued = fixture.Submit(&slot, fixture.gpu.queue.Get());
  Check(continued.unsafe_lists == 1 && continued.signals == 0 && continued.waits == 0
      && execution::GetStatus(fixture.stream).tail == tail, "poisoned continuation remains unsafe without fake recovery");
  fixture.Verify(&slot);
  Check(SUCCEEDED(slot.list->Reset(slot.allocator.Get(), nullptr)), "close replay capability by real reset");
  submission::OnCommandListReset(slot.list.Get());
  submission::PruneCompletedGenerations();
  Check(!execution::ReleaseIfIdle(), "closed poisoned registry retains refs without legacy completion proof");
  submission::CompleteSubmission(pending, slot.done.Get(), slot.serial);
  fixture.Finish();
  Check(execution::GetStatus(fixture.stream).poisoned, "legacy completion retires references without unpoisoning");
  std::cout << "MOCK " << (wait_failure ? "Wait" : "Signal")
            << " failure tail/poison/forward-once=PASS; WARP independent actual completion=PASS\n";
}

uint64_t passthrough_calls = 0;
UINT passthrough_count = 0;
ID3D12CommandList* const* passthrough_lists = nullptr;
void STDMETHODCALLTYPE Passthrough(ID3D12CommandQueue*, UINT count, ID3D12CommandList* const* lists) {
  ++passthrough_calls;
  passthrough_count = count;
  passthrough_lists = lists;
}

struct ForeignIdentity final : IUnknown {
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (iid == IID_IUnknown) {
      *out = this;
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
};

void IdleAndMismatch() {
  Check(execution::ReleaseIfIdle(), "start idle");
  const auto tracker_count = submission::tracked_lists.size();
  ID3D12CommandList* invalid[]{nullptr, nullptr};
  for (const auto count : {0u, 2u}) {
    const auto result = execution::ExecuteOrdered(nullptr, count, invalid, Passthrough);
    Check(result.forward_calls == 1 && result.signals == 0 && result.waits == 0
        && passthrough_count == count && passthrough_lists == invalid, "idle preserves batch exactly");
  }
  execution::ExecuteOrdered(nullptr, 2, nullptr, Passthrough);
  Check(passthrough_lists == nullptr && passthrough_calls == 3
      && submission::tracked_lists.size() == tracker_count, "null forwarded; idle creates no tracker state");
  Fixture fixture;
  // D3D12CreateDevice is singleton per adapter. Inject a distinct canonical
  // identity, rather than mislabel two WARP factory calls as different devices.
  ForeignIdentity foreign_identity;
  auto stream = execution::CreateStream(fixture.gpu.device.Get());
  stream->device_identity = &foreign_identity;
  execution::BeginResult reason{};
  Check(!execution::BeginRecording(stream, fixture.slots[0].list.Get(), &reason)
      && reason == execution::BeginResult::DEVICE_MISMATCH && execution::GetStatus(stream).poisoned,
      "record-time device mismatch strong rejection");
  fixture.Record(&fixture.slots[0], 7);
  ID3D12CommandList* foreign_lists[]{fixture.slots[1].list.Get(), fixture.slots[2].list.Get()};
  const auto active_tracker_count = submission::tracked_lists.size();
  const auto foreign_batch = execution::ExecuteOrdered(fixture.gpu.queue.Get(), 2, foreign_lists, Passthrough);
  Check(foreign_batch.forward_calls == 1 && foreign_batch.forwarded_lists == 2 && foreign_batch.signals == 0
      && foreign_batch.waits == 0 && submission::tracked_lists.size() == active_tracker_count,
      "foreign batch remains intact and lookup does not create tracker entries");
  const auto empty_batch = execution::ExecuteOrdered(fixture.gpu.queue.Get(), 0, foreign_lists, Passthrough);
  const auto null_batch = execution::ExecuteOrdered(fixture.gpu.queue.Get(), 2, nullptr, Passthrough);
  Check(empty_batch.signals == 0 && null_batch.signals == 0 && passthrough_lists == nullptr,
      "active registry still forwards zero/null unchanged");
  fixture.stream->device_identity = &foreign_identity;
  ID3D12CommandList* lists[]{fixture.slots[0].list.Get()};
  // Mismatch is MOCK-forwarded: never send a wrong-device list to native D3D12.
  const auto result = execution::ExecuteOrdered(fixture.other_queue.Get(), 1, lists, Passthrough);
  Check(result.unsafe_lists == 1 && result.device_mismatches == 1 && result.signals == 0
      && result.waits == 0 && execution::GetStatus(fixture.stream).tail == 0, "submit mismatch no tail");
  fixture.Finish();
  fixture.stream.reset();
  std::cout << "MOCK device mismatch/idle passthrough=PASS; no illegal native submission\n";
}
}  // namespace

int main(int argc, char** argv) {
  try {
    const bool hardware = argc == 2 && std::string(argv[1]) == "--hardware";
    Ring(false, hardware);
    if (hardware) return 0;
    Ring(true, false);
    ReplayBatchConcurrent();
    Admission();
    Failures(true);
    Failures(false);
    IdleAndMismatch();
    std::cout << "PASS all targeted execution tests; no NGX/game/visual claim\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
