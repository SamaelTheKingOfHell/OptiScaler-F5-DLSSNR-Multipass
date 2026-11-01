#pragma once

#include <d3d12.h>
#include <cstdint>
#include <limits>

namespace DlssNr::MultiGpu
{
enum class WorkerFenceWaitResult
{
    Ready,
    PeerExited,
    Timeout,
    DeviceLost,
    Failed
};

// CPU-side bounded wait. Never enqueue a wait on a fence controlled by a process that may die;
// otherwise an unsignalled fence can permanently stall the consumer's GPU queue. The peer process
// handle must allow SYNCHRONIZE and remain valid for the duration of this call.
inline WorkerFenceWaitResult WaitForFenceOrPeer(ID3D12Fence* fence, uint64_t value, HANDLE peer, DWORD timeoutMs)
{
    if (!fence || !peer || peer == INVALID_HANDLE_VALUE || value == 0 || timeoutMs == INFINITE ||
        value == (std::numeric_limits<uint64_t>::max)())
        return WorkerFenceWaitResult::Failed;
    // Polling avoids leaving a fence-event registration referencing an event that was closed after
    // a timeout or peer exit. Blocking on the process between checks avoids a busy spin.
    const ULONGLONG started = GetTickCount64();
    for (;;)
    {
        const DWORD peerStatus = WaitForSingleObject(peer, 0);
        if (peerStatus == WAIT_OBJECT_0)
            return WorkerFenceWaitResult::PeerExited;
        if (peerStatus != WAIT_TIMEOUT)
            return WorkerFenceWaitResult::Failed;
        const uint64_t completed = fence->GetCompletedValue();
        if (completed == (std::numeric_limits<uint64_t>::max)())
            return WorkerFenceWaitResult::DeviceLost;
        if (completed >= value)
            return WorkerFenceWaitResult::Ready;
        const ULONGLONG elapsed = GetTickCount64() - started;
        if (elapsed >= timeoutMs)
            return WorkerFenceWaitResult::Timeout;
        const DWORD remaining = timeoutMs - static_cast<DWORD>(elapsed);
        const DWORD waited = WaitForSingleObject(peer, remaining < 2 ? remaining : 2);
        if (waited == WAIT_OBJECT_0)
            return WorkerFenceWaitResult::PeerExited;
        if (waited != WAIT_TIMEOUT)
            return WorkerFenceWaitResult::Failed;
    }
}
} // namespace DlssNr::MultiGpu
