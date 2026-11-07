#pragma once
// Owner side of a network split (DlssNr_NetworkSplit.h): the helper worker process, the cross-adapter buffer, a copy
// queue for the PCIe transfers, and the fences that order them. Per picture:
//
//   direct queue:  Head evaluate (forward ranges -> forward staging) ........ wait(xfer 2N) -> End evaluate
//                       | signal xfer 2N-1                                   ^
//   copy queue:    wait(xfer 2N-1) -> staging -> shared -> signal ready N    | wait(done N) -> shared -> staging
//                                                  |                         |      -> signal xfer 2N
//   helper GPU:                                    +--> segment ------> done N
//
// No queue waits on the CPU and no GPU spins: every wait is a queue-level fence wait. A WorkerWatch guards the helper's
// done fence: if the worker exits or stalls, it signals that fence from the CPU so the copy queue (and the direct queue
// behind it) continue, and Failed() reports it; that picture keeps stale helper rows.
//
// The caller serializes all calls, keeps this object alive until every submission that references it has retired
// (e.g. through a finished-picture slot fence), and records the four state transitions around the two evaluates.

#include "../DlssNr_CrossAdapterBuffer.h"
#include "../DlssNr_NetworkSplit.h"
#include "../DlssNr_OwnedCommandChain.h"
#include "WorkerClient.h"
#include "WorkerWatch.h"
#include <array>
#include <memory>

namespace DlssNr::Worker
{
class SplitGeneration
{
    using Plan = NetworkSplit::Plan;
    template<class T> using Com = Microsoft::WRL::ComPtr<T>;
    struct CopySlot
    {
        Com<ID3D12CommandAllocator> allocator;
        Com<ID3D12GraphicsCommandList> list;
        uint64_t retired = 0; // copyFence value after which this slot's last list is done
    };
    Plan plan_;
    MultiGpu::CrossAdapterBuffer transfer_;
    WorkerClient client_;
    Com<ID3D12Device> device_;
    Com<ID3D12CommandQueue> copyQueue_;
    Com<ID3D12Fence> xfer_, copyFence_;
    Com<ID3D12Resource> forward_, backward_;
    std::array<CopySlot, 4> slots_ {};
    size_t nextSlot_ = 0;
    // Small direct lists that put a timestamp on the game queue right before its wait for the helper, so the caller
    // can measure how long that wait held the queue (retired through stampFence_; a busy one is skipped, never waited).
    std::array<CopySlot, 4> stamps_ {};
    size_t nextStamp_ = 0;
    Com<ID3D12Fence> stampFence_;
    uint64_t stampValue_ = 0;
    uint64_t copyValue_ = 0, frame_ = 0;
    bool started_ = false, failed_ = false, pending_ = false;
    WorkerWatch watch_; // last: its thread stops before the client's process handle closes

    static void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES a,
                           D3D12_RESOURCE_STATES b)
    {
        D3D12_RESOURCE_BARRIER x {};
        x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        x.Transition.pResource = r;
        x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        x.Transition.StateBefore = a;
        x.Transition.StateAfter = b;
        list->ResourceBarrier(1, &x);
    }
    HRESULT Buffer(uint64_t bytes, Com<ID3D12Resource>& out)
    {
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = (std::max<uint64_t>)(bytes, 256);
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                IID_PPV_ARGS(&out));
    }
    // A copy list whose previous use has finished (normally immediately; bounded wait otherwise).
    HRESULT NextCopyList(ID3D12GraphicsCommandList*& list)
    {
        auto& slot = slots_[nextSlot_];
        nextSlot_ = (nextSlot_ + 1) % slots_.size();
        if (copyFence_->GetCompletedValue() < slot.retired)
        {
            HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            const bool ok = event && SUCCEEDED(copyFence_->SetEventOnCompletion(slot.retired, event)) &&
                            WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
            if (event)
                CloseHandle(event);
            if (!ok)
                return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        }
        HRESULT hr = slot.allocator->Reset();
        if (SUCCEEDED(hr))
            hr = slot.list->Reset(slot.allocator.Get(), nullptr);
        list = slot.list.Get();
        slot.retired = copyValue_ + 1;
        return hr;
    }
    HRESULT SubmitCopy(ID3D12GraphicsCommandList* list)
    {
        HRESULT hr = list->Close();
        if (FAILED(hr))
            return hr;
        ID3D12CommandList* lists[] = { list };
        copyQueue_->ExecuteCommandLists(1, lists);
        return copyQueue_->Signal(copyFence_.Get(), ++copyValue_);
    }

  public:
    SplitGeneration() = default;
    SplitGeneration(const SplitGeneration&) = delete;
    SplitGeneration& operator=(const SplitGeneration&) = delete;

    // Creates the shared buffer, staging buffers, copy queue and fences. `secondary` is the helper adapter's device.
    HRESULT Create(ID3D12Device* primary, ID3D12Device* secondary, const Plan& plan)
    {
        if (!primary || !secondary || !plan.Valid() || device_)
            return E_INVALIDARG;
        plan_ = plan;
        device_ = primary;
        HRESULT hr = MultiGpu::CrossAdapterBuffer::Create(primary, secondary, plan.forwardBytes + plan.backwardBytes,
                                                          transfer_);
        if (FAILED(hr))
            return hr;
        D3D12_COMMAND_QUEUE_DESC qd {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        if (FAILED(hr = primary->CreateCommandQueue(&qd, IID_PPV_ARGS(&copyQueue_))) ||
            FAILED(hr = primary->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&xfer_))) ||
            FAILED(hr = primary->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copyFence_))) ||
            FAILED(hr = Buffer(plan.forwardBytes, forward_)) || FAILED(hr = Buffer(plan.backwardBytes, backward_)))
            return hr;
        for (auto& slot : slots_)
        {
            if (FAILED(hr = primary->CreateCommandAllocator(qd.Type, IID_PPV_ARGS(&slot.allocator))) ||
                FAILED(hr = primary->CreateCommandList(0, qd.Type, slot.allocator.Get(), nullptr,
                                                       IID_PPV_ARGS(&slot.list))) ||
                FAILED(hr = slot.list->Close()))
                return hr;
        }
        return S_OK;
    }
    // Starts the helper. `wire` carries the model size, applicationId and passes[0] tuning; the rest is filled here.
    HRESULT Start(const wchar_t* executable, const wchar_t* core, const wchar_t* forwarder, const wchar_t* model,
                  const wchar_t* data, const wchar_t* log, InitialFrameWire wire)
    {
        if (!device_ || started_)
            return E_INVALIDARG;
        Com<ID3D12Device> secondary;
        if (FAILED(transfer_.SecondaryBuffer()->GetDevice(IID_PPV_ARGS(&secondary))) || !secondary)
            return E_FAIL;
        wire.frameCount = 0;
        wire.passCount = 1;
        wire.bytes = transfer_.Size();
        wire.primary = device_->GetAdapterLuid();
        wire.secondary = secondary->GetAdapterLuid();
        wire.split = NetworkSplit::ToWire(plan_);
        HRESULT hr = client_.Start(device_.Get(), executable, core, forwarder, model, data, log, transfer_, wire);
        // No picture goes out unguarded: without the watchdog a stalled helper would hold the game's queue.
        if (SUCCEEDED(hr) && !watch_.Start(transfer_.PrimaryReady(), transfer_.PrimaryDone(), client_.ProcessHandle()))
            hr = E_FAIL;
        started_ = SUCCEEDED(hr);
        failed_ = !started_;
        return hr;
    }
    // S_OK when the helper is ready for a picture, E_PENDING while it loads or a picture is outstanding.
    HRESULT Poll()
    {
        if (failed_ || watch_.Failed())
        {
            failed_ = true;
            return E_ABORT;
        }
        if (!started_)
            return E_INVALIDARG;
        if (pending_)
            return E_PENDING;
        const HRESULT hr = client_.WaitForReady(0);
        if (FAILED(hr) && hr != E_PENDING)
            failed_ = true;
        return hr;
    }
    bool Failed() const { return failed_ || watch_.Failed(); }
    // True when the helper has already finished the picture in flight (its done fence passed it).
    bool HelperDone() const { return pending_ && transfer_.PrimaryDone()->GetCompletedValue() >= frame_; }
    const Plan& GetPlan() const { return plan_; }
    uint64_t ForwardStaging() const { return forward_->GetGPUVirtualAddress(); }
    uint64_t BackwardStaging() const { return backward_->GetGPUVirtualAddress(); }

    // Around the Head evaluate on the direct list: the staging buffer is written by the copy kernel (UAV), then handed
    // to the copy queue in COMMON.
    void BeforeHead(ID3D12GraphicsCommandList* list)
    {
        Transition(list, forward_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    void AfterHead(ID3D12GraphicsCommandList* list)
    {
        Transition(list, forward_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    }
    // After AfterHead: submits the head (splits the chain), sends the forward data and the frame to the helper.
    HRESULT Submit(MultiGpu::OwnedCommandChain& chain, ID3D12CommandQueue* queue, FrameCommand command)
    {
        if (Poll() != S_OK)
            return E_PENDING;
        const uint64_t n = ++frame_;
        HRESULT hr = chain.Split();
        if (SUCCEEDED(hr))
            hr = queue->Signal(xfer_.Get(), 2 * n - 1);
        ID3D12GraphicsCommandList* list = nullptr;
        if (SUCCEEDED(hr))
            hr = NextCopyList(list);
        if (SUCCEEDED(hr))
        {
            list->CopyBufferRegion(transfer_.PrimaryBuffer(), 0, forward_.Get(), 0, plan_.forwardBytes);
            hr = copyQueue_->Wait(xfer_.Get(), 2 * n - 1);
        }
        if (SUCCEEDED(hr))
            hr = SubmitCopy(list);
        if (SUCCEEDED(hr))
            hr = copyQueue_->Signal(transfer_.PrimaryReady(), n);
        if (SUCCEEDED(hr))
        {
            command.frameNumber = n;
            hr = client_.SendFrame(command);
        }
        if (SUCCEEDED(hr))
            watch_.Arm(n);
        if (FAILED(hr))
            failed_ = true;
        pending_ = SUCCEEDED(hr);
        return hr;
    }
    // Orders the backward data behind the helper on the copy queue and the direct queue behind that. The direct list
    // recorded after this call (End evaluate) runs only once the backward data has landed.
    // Call right before AwaitReturn: `record` fills a small list that `queue` executes just ahead of the wait for the
    // helper, e.g. a timestamp, so the wait's hold on the queue can be measured.
    template<class Record> void StampBeforeWait(ID3D12CommandQueue* queue, const Record& record)
    {
        if (!stampFence_ && FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&stampFence_))))
            return;
        auto& slot = stamps_[nextStamp_];
        if (!slot.list &&
            (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator))) ||
             FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr,
                                               IID_PPV_ARGS(&slot.list))) ||
             FAILED(slot.list->Close())))
        {
            slot.list.Reset();
            return;
        }
        if (stampFence_->GetCompletedValue() < slot.retired)
            return; // still on the GPU: no stamp this frame
        if (FAILED(slot.allocator->Reset()) || FAILED(slot.list->Reset(slot.allocator.Get(), nullptr)))
            return;
        record(slot.list.Get());
        if (FAILED(slot.list->Close()))
            return;
        ID3D12CommandList* lists[] = { slot.list.Get() };
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(stampFence_.Get(), ++stampValue_);
        slot.retired = stampValue_;
        nextStamp_ = (nextStamp_ + 1) % stamps_.size();
    }
    HRESULT AwaitReturn(ID3D12CommandQueue* queue)
    {
        if (!pending_)
            return E_INVALIDARG;
        ID3D12GraphicsCommandList* list = nullptr;
        HRESULT hr = NextCopyList(list);
        if (SUCCEEDED(hr))
        {
            list->CopyBufferRegion(backward_.Get(), 0, transfer_.PrimaryBuffer(), plan_.forwardBytes,
                                   plan_.backwardBytes);
            hr = copyQueue_->Wait(transfer_.PrimaryDone(), frame_);
        }
        if (SUCCEEDED(hr))
            hr = SubmitCopy(list);
        if (SUCCEEDED(hr))
            hr = copyQueue_->Signal(xfer_.Get(), 2 * frame_);
        if (SUCCEEDED(hr))
            hr = queue->Wait(xfer_.Get(), 2 * frame_);
        // Queue order now protects the shared buffer: the next forward copy follows this one on the copy queue, and
        // the worker reads it only after the next ready signal.
        if (SUCCEEDED(hr))
            hr = client_.MarkQueuedWait(frame_);
        if (SUCCEEDED(hr))
            hr = client_.AcknowledgeQueuedReturn(frame_, xfer_.Get(), 2 * frame_, queue);
        if (FAILED(hr))
            failed_ = true;
        pending_ = false;
        return hr;
    }
    void BeforeEnd(ID3D12GraphicsCommandList* list)
    {
        Transition(list, backward_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    void AfterEnd(ID3D12GraphicsCommandList* list)
    {
        Transition(list, backward_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    }
    HANDLE ProcessHandle() const { return client_.ProcessHandle(); }
    HRESULT Stop(DWORD timeoutMs) { return pending_ ? E_PENDING : client_.Stop(timeoutMs); }
};
} // namespace DlssNr::Worker
