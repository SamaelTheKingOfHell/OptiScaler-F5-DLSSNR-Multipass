#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <limits>
#include <mutex>

namespace DlssNr::MultiGpu
{
enum class WorkerFenceWaitResult { Ready, PeerExited, Timeout, DeviceLost, Failed };

// A timed-out or interrupted D3D12 event registration may still be pending.
// Keep its event and fence alive until the event actually fires. A bounded pool
// prevents repeated failures from accumulating unbounded handles; when full,
// the caller falls back to the original bounded polling wait.
class FenceEventPool
{
    struct Slot
    {
        HANDLE pending = nullptr;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        bool reserved = false;
    };
    std::array<Slot, 16> slots_ {};
    std::mutex mutex_;

  public:
    int Reserve()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& slot : slots_)
            if (slot.pending && WaitForSingleObject(slot.pending, 0) == WAIT_OBJECT_0)
            {
                CloseHandle(slot.pending);
                slot.pending = nullptr;
                slot.fence.Reset();
            }
        for (size_t i = 0; i < slots_.size(); ++i)
            if (!slots_[i].pending && !slots_[i].reserved)
            {
                slots_[i].reserved = true;
                return static_cast<int>(i);
            }
        return -1;
    }
    void Finish(int index, HANDLE event, ID3D12Fence* fence, bool delivered)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& slot = slots_[static_cast<size_t>(index)];
        if (event && !delivered && WaitForSingleObject(event, 0) != WAIT_OBJECT_0)
        {
            slot.pending = event;
            slot.fence = fence;
        }
        else if (event)
            CloseHandle(event);
        slot.reserved = false;
    }
};

inline FenceEventPool& FenceEvents()
{
    // Avoid a DLL-unload destructor closing events whose registrations may still be pending.
    static auto* pool = new FenceEventPool;
    return *pool;
}

inline WorkerFenceWaitResult PollForFenceOrPeer(ID3D12Fence* fence, uint64_t value, HANDLE peer, DWORD timeoutMs)
{
    const ULONGLONG started = GetTickCount64();
    for (;;)
    {
        const DWORD peerStatus = WaitForSingleObject(peer, 0);
        if (peerStatus == WAIT_OBJECT_0) return WorkerFenceWaitResult::PeerExited;
        if (peerStatus != WAIT_TIMEOUT) return WorkerFenceWaitResult::Failed;
        const uint64_t completed = fence->GetCompletedValue();
        if (completed == (std::numeric_limits<uint64_t>::max)()) return WorkerFenceWaitResult::DeviceLost;
        if (completed >= value) return WorkerFenceWaitResult::Ready;
        const ULONGLONG elapsed = GetTickCount64() - started;
        if (elapsed >= timeoutMs) return WorkerFenceWaitResult::Timeout;
        const DWORD remaining = timeoutMs - static_cast<DWORD>(elapsed);
        const DWORD waited = WaitForSingleObject(peer, remaining < 2 ? remaining : 2);
        if (waited == WAIT_OBJECT_0) return WorkerFenceWaitResult::PeerExited;
        if (waited != WAIT_TIMEOUT) return WorkerFenceWaitResult::Failed;
    }
}

// CPU-side bounded wait. Never enqueue a GPU wait on a fence controlled by a
// peer process that may die. Completion events avoid timer-resolution polling
// delays while the peer process handle remains part of the same bounded wait.
inline WorkerFenceWaitResult WaitForFenceOrPeer(ID3D12Fence* fence, uint64_t value, HANDLE peer, DWORD timeoutMs)
{
    if (!fence || !peer || peer == INVALID_HANDLE_VALUE || value == 0 || timeoutMs == INFINITE ||
        value == (std::numeric_limits<uint64_t>::max)())
        return WorkerFenceWaitResult::Failed;
    const DWORD peerStatus = WaitForSingleObject(peer, 0);
    if (peerStatus == WAIT_OBJECT_0) return WorkerFenceWaitResult::PeerExited;
    if (peerStatus != WAIT_TIMEOUT) return WorkerFenceWaitResult::Failed;
    const uint64_t completed = fence->GetCompletedValue();
    if (completed == (std::numeric_limits<uint64_t>::max)()) return WorkerFenceWaitResult::DeviceLost;
    if (completed >= value) return WorkerFenceWaitResult::Ready;
    if (timeoutMs == 0) return WorkerFenceWaitResult::Timeout;

    auto& pool = FenceEvents();
    const int slot = pool.Reserve();
    if (slot < 0) return PollForFenceOrPeer(fence, value, peer, timeoutMs);
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event || FAILED(fence->SetEventOnCompletion(value, event)))
    {
        pool.Finish(slot, event, fence, true); // no registration exists on failure
        return PollForFenceOrPeer(fence, value, peer, timeoutMs);
    }
    HANDLE handles[] = { peer, event };
    const DWORD waited = WaitForMultipleObjects(2, handles, FALSE, timeoutMs);
    pool.Finish(slot, event, fence, waited == WAIT_OBJECT_0 + 1);
    if (waited == WAIT_OBJECT_0) return WorkerFenceWaitResult::PeerExited;
    if (waited == WAIT_TIMEOUT) return WorkerFenceWaitResult::Timeout;
    if (waited != WAIT_OBJECT_0 + 1) return WorkerFenceWaitResult::Failed;
    const DWORD afterPeer = WaitForSingleObject(peer, 0);
    if (afterPeer == WAIT_OBJECT_0) return WorkerFenceWaitResult::PeerExited;
    if (afterPeer != WAIT_TIMEOUT) return WorkerFenceWaitResult::Failed;
    const uint64_t afterFence = fence->GetCompletedValue();
    if (afterFence == (std::numeric_limits<uint64_t>::max)()) return WorkerFenceWaitResult::DeviceLost;
    return afterFence >= value ? WorkerFenceWaitResult::Ready : WorkerFenceWaitResult::Failed;
}
} // namespace DlssNr::MultiGpu
