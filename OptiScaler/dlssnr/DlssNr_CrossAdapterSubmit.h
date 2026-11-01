#pragma once

#include "DlssNr_CrossAdapterBuffer.h"
#include "DlssNr_CrossAdapterProtocol.h"
#include <mutex>

namespace DlssNr::MultiGpu
{
// Enqueues already-closed command lists in strict game -> secondary -> game order, with GPU-side
// waits. This does not close, split, or submit an existing game's open command list; all three lists
// must be independently owned and closed. The caller owns resource-state transitions and must keep
// every referenced resource, list and allocator alive until GPU work completes. A transfer must be
// used by only one coordinator. This class holds queues and the transfer alive, not the caller's lists.
class CrossAdapterSubmit
{
    using Queue = Microsoft::WRL::ComPtr<ID3D12CommandQueue>;
    using Device = Microsoft::WRL::ComPtr<ID3D12Device>;
    Queue primary_, secondary_;
    CrossAdapterBuffer transfer_;
    std::mutex mutex_;
    CrossAdapterProtocol protocol_;

    static bool SameAdapter(ID3D12Device* a, ID3D12Device* b)
    {
        const auto x = a->GetAdapterLuid(), y = b->GetAdapterLuid();
        return x.HighPart == y.HighPart && x.LowPart == y.LowPart;
    }
    static bool OwnedBy(ID3D12DeviceChild* object, ID3D12Device* expected)
    {
        Device owner;
        return object && SUCCEEDED(object->GetDevice(IID_PPV_ARGS(&owner))) && SameAdapter(owner.Get(), expected);
    }

  public:
    CrossAdapterSubmit(ID3D12CommandQueue* primary, ID3D12CommandQueue* secondary, const CrossAdapterBuffer& transfer)
        : primary_(primary), secondary_(secondary), transfer_(transfer)
    {
    }

    // Lists must already be closed and direct-type. If this returns Complete, all work is enqueued,
    // not necessarily finished; the caller must use its own retirement fence after the primary list.
    SubmissionResult Submit(ID3D12CommandList* primaryBefore, ID3D12CommandList* secondaryWork,
                            ID3D12CommandList* primaryAfter)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        SubmissionResult result { E_INVALIDARG, SubmissionPhase::Validate, 0 };
        if (protocol_.Poisoned())
        {
            result.result = E_ABORT;
            return result;
        }
        if (!primary_ || !secondary_ || !primaryBefore || !secondaryWork || !primaryAfter ||
            primaryBefore == primaryAfter || !transfer_.PrimaryBuffer() || !transfer_.SecondaryBuffer() ||
            !transfer_.PrimaryReady() || !transfer_.SecondaryReady() || !transfer_.SecondaryDone() ||
            !transfer_.PrimaryDone() || primary_->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
            secondary_->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
            primaryBefore->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT ||
            secondaryWork->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT ||
            primaryAfter->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
            return result;
        Device a, b;
        if (FAILED(primary_->GetDevice(IID_PPV_ARGS(&a))) || FAILED(secondary_->GetDevice(IID_PPV_ARGS(&b))) || !a ||
            !b || SameAdapter(a.Get(), b.Get()) || !OwnedBy(transfer_.PrimaryBuffer(), a.Get()) ||
            !OwnedBy(transfer_.SecondaryBuffer(), b.Get()) || !OwnedBy(primaryBefore, a.Get()) ||
            !OwnedBy(primaryAfter, a.Get()) || !OwnedBy(secondaryWork, b.Get()))
            return result;
        HRESULT hr = a->GetDeviceRemovedReason();
        if (SUCCEEDED(hr))
            hr = b->GetDeviceRemovedReason();
        if (FAILED(hr))
        {
            result.result = hr;
            return result;
        }
        struct Operations
        {
            ID3D12CommandQueue* primary;
            ID3D12CommandQueue* secondary;
            CrossAdapterBuffer& transfer;
            ID3D12CommandList* before;
            ID3D12CommandList* work;
            ID3D12CommandList* after;
            void SubmitPrimary() { primary->ExecuteCommandLists(1, &before); }
            HRESULT SignalPrimary(uint64_t value) { return primary->Signal(transfer.PrimaryReady(), value); }
            HRESULT WaitSecondary(uint64_t value) { return secondary->Wait(transfer.SecondaryReady(), value); }
            void SubmitSecondary() { secondary->ExecuteCommandLists(1, &work); }
            HRESULT SignalSecondary(uint64_t value) { return secondary->Signal(transfer.SecondaryDone(), value); }
            HRESULT WaitPrimary(uint64_t value) { return primary->Wait(transfer.PrimaryDone(), value); }
            void SubmitPrimaryAfter() { primary->ExecuteCommandLists(1, &after); }
        } ops { primary_.Get(), secondary_.Get(), transfer_, primaryBefore, secondaryWork, primaryAfter };
        return protocol_.Submit(ops);
    }
};
} // namespace DlssNr::MultiGpu
