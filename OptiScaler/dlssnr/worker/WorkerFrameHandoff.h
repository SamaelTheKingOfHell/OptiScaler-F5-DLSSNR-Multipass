#pragma once

#include "../DlssNr_OwnedCommandChain.h"
#include "WorkerClient.h"
#include "WorkerFrameTransfer.h"

namespace DlssNr::Worker
{
enum class HandoffPhase
{
    Validate,
    OutboundRecorded,
    ControlSent,
    PrimarySubmitted,
    OutboundSignalled,
    WorkerReturned
};
struct HandoffResult
{
    HRESULT result = E_FAIL;
    HandoffPhase phase = HandoffPhase::Validate;
};

// This only operates on a caller-owned direct list. Color/depth/motion must already be in
// COPY_SOURCE. On failure after PrimarySubmitted, the caller must retain all referenced resources
// and allocators until its own primary queue retirement fence completes. The caller must stop the
// worker on failure, and must not consume or reuse the shared buffer until that cleanup is safe.
// Success means outbound submission only; poll or wait for worker return before recording inbound reads.
// ```cpp
// auto sent = SendFrame(transfer, worker, chain, queue, frame, color, depth, motion);
// ```
inline HandoffResult SendFrame(FrameTransfer& transfer, WorkerClient& worker, MultiGpu::OwnedCommandChain& chain,
                               ID3D12CommandQueue* queue, const FrameCommand& frame, ID3D12Resource* color,
                               ID3D12Resource* depth, ID3D12Resource* motion)
{
    HandoffResult result { E_INVALIDARG, HandoffPhase::Validate };
    auto* commands = chain.Current();
    if (!commands || !queue || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
        !transfer.Buffer().PrimaryBuffer() || !transfer.Buffer().PrimaryReady() || !chain.UsesQueue(queue) ||
        !worker.UsesTransfer(transfer.Buffer()))
        return result;
    Microsoft::WRL::ComPtr<ID3D12Device> fromQueue, fromBuffer;
    Microsoft::WRL::ComPtr<IUnknown> a, b;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&fromQueue))) ||
        FAILED(transfer.Buffer().PrimaryBuffer()->GetDevice(IID_PPV_ARGS(&fromBuffer))) || !fromQueue || !fromBuffer ||
        FAILED(fromQueue.As(&a)) || FAILED(fromBuffer.As(&b)) || a.Get() != b.Get())
        return result;
    HRESULT hr = transfer.RecordOutbound(commands, color, depth, motion, frame.rows ? frame.rowOffset : 0, frame.rows);
    if (FAILED(hr))
    {
        result.result = hr;
        return result;
    }
    result.phase = HandoffPhase::OutboundRecorded;
    hr = worker.SendFrame(frame);
    if (FAILED(hr))
    {
        result.result = hr;
        return result;
    }
    result.phase = HandoffPhase::ControlSent;
    const unsigned submittedBefore = chain.SubmittedCount();
    hr = chain.Split();
    if (chain.SubmittedCount() > submittedBefore)
        result.phase = HandoffPhase::PrimarySubmitted;
    if (FAILED(hr))
    {
        result.result = hr;
        return result;
    }
    result.phase = HandoffPhase::PrimarySubmitted;
    hr = queue->Signal(transfer.Buffer().PrimaryReady(), frame.frameNumber);
    if (FAILED(hr))
    {
        result.result = hr;
        return result;
    }
    result.phase = HandoffPhase::OutboundSignalled;
    result.result = S_OK;
    return result;
}

// Compatibility entry point: dispatch the same frame, then wait a finite time.
// ```cpp
// auto result = SendFrameAndWait(transfer, worker, chain, queue, frame, color, depth, motion, 10000);
// ```
inline HandoffResult SendFrameAndWait(FrameTransfer& transfer, WorkerClient& worker, MultiGpu::OwnedCommandChain& chain,
                                      ID3D12CommandQueue* queue, const FrameCommand& frame, ID3D12Resource* color,
                                      ID3D12Resource* depth, ID3D12Resource* motion, DWORD timeoutMs)
{
    if (timeoutMs == INFINITE)
        return { E_INVALIDARG, HandoffPhase::Validate };
    auto result = SendFrame(transfer, worker, chain, queue, frame, color, depth, motion);
    if (FAILED(result.result)) return result;
    const auto waited = worker.WaitForReturn(frame.frameNumber, timeoutMs);
    if (waited != MultiGpu::WorkerFenceWaitResult::Ready)
    {
        result.result = waited == MultiGpu::WorkerFenceWaitResult::Timeout ? HRESULT_FROM_WIN32(WAIT_TIMEOUT) : E_ABORT;
        return result;
    }
    result.phase = HandoffPhase::WorkerReturned;
    result.result = S_OK;
    return result;
}
} // namespace DlssNr::Worker
