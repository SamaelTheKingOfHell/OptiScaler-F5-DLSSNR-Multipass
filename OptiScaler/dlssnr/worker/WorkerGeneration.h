#pragma once

#include "WorkerFrameHandoff.h"
#include <algorithm>
#include <iterator>
#include <utility>

namespace DlssNr::Worker
{
// Owns one immutable worker/resource generation. The caller retains this object and its command
// chain until all submitted primary work retires. All calls must be serialized by the caller.
class WorkerGeneration
{
    enum class State
    {
        Empty,
        Initializing,
        Ready,
        PendingWorker,
        AwaitingCopy,
        CopyRecorded,
        AwaitingRetirement,
        Failed,
        Stopped
    };
    FrameTransfer transfer_;
    WorkerClient client_;
    Microsoft::WRL::ComPtr<ID3D12Fence> retirement_, orderedRetirement_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> submissionQueue_, retirementQueue_, requiredQueue_;
    uint64_t retirementValue_ = 0, nextFrame_ = 1;
    uint32_t passCount_ = 0;
    bool reset_ = true;
    State state_ = State::Empty;

    static bool SameQueue(ID3D12CommandQueue* a, ID3D12CommandQueue* b)
    {
        Microsoft::WRL::ComPtr<IUnknown> x, y;
        return a && b && SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&x))) &&
               SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&y))) && x.Get() == y.Get();
    }
    void Retired(bool queueOrdered)
    {
        if (queueOrdered)
        {
            requiredQueue_ = submissionQueue_;
            // Its signal may still be queued. Ready of the next worker frame
            // follows this signal, so hold it until that worker actually returns.
            orderedRetirement_ = retirement_;
        }
        retirement_.Reset();
        retirementQueue_.Reset();
        retirementValue_ = 0;
        ++nextFrame_;
        state_ = State::Ready;
    }

  public:
    explicit WorkerGeneration(FrameTransfer&& transfer) : transfer_(std::move(transfer)) {}
    WorkerGeneration(const WorkerGeneration&) = delete;
    WorkerGeneration& operator=(const WorkerGeneration&) = delete;

    HRESULT Start(ID3D12Device* primary, const wchar_t* executable, const wchar_t* core, const wchar_t* forwarder,
                  const wchar_t* model, const wchar_t* data, const wchar_t* log, const InitialFrameWire& settings)
    {
        if (state_ != State::Empty || !settings.passCount || settings.passCount > MaxWorkerPasses)
            return E_INVALIDARG;
        auto wire = transfer_.MakeWire(settings.passCount);
        wire.passFeedback = settings.passFeedback;
        wire.applicationId = settings.applicationId;
        wire.enableDebug = settings.enableDebug;
        std::copy(std::begin(settings.passes), std::end(settings.passes), std::begin(wire.passes));
        const HRESULT hr =
            client_.Start(primary, executable, core, forwarder, model, data, log, transfer_.Buffer(), wire);
        if (FAILED(hr))
        {
            state_ = State::Failed;
            return hr;
        }
        passCount_ = settings.passCount;
        state_ = State::Initializing;
        return S_OK;
    }
    // Poll with timeout 0 in a render path. A generation awaiting primary retirement is not reusable.
    HRESULT Poll(DWORD startupTimeoutMs = 0)
    {
        if (state_ == State::Initializing)
        {
            const HRESULT hr = client_.WaitForReady(startupTimeoutMs);
            if (hr == S_OK)
                state_ = State::Ready;
            else if (hr != E_PENDING)
                state_ = State::Failed;
            return hr;
        }
        if (state_ == State::AwaitingRetirement)
        {
            const HRESULT hr = client_.AcknowledgeReturn(nextFrame_, retirement_.Get(), retirementValue_);
            if (hr == S_OK)
            {
                Retired(false);
            }
            else if (hr != E_PENDING)
                state_ = State::Failed;
            return hr;
        }
        return state_ == State::Ready ? S_OK : state_ == State::Failed ? E_ABORT : E_PENDING;
    }
    // Queue ordering can protect shared-buffer reuse before CPU fence completion.
    // TrackRetirement's queue overload must have recorded the prior submitted signal.
    // ```cpp
    // if (generation.PollForQueue(realQueue) == S_OK) generation.SubmitAsync(chain, realQueue, frame, color, depth, motion);
    // ```
    HRESULT PollForQueue(ID3D12CommandQueue* queue)
    {
        if (!queue || !client_.Owns(queue) || (submissionQueue_ && !SameQueue(queue, submissionQueue_.Get())))
            return E_INVALIDARG;
        if (state_ != State::AwaitingRetirement || !retirementQueue_)
            return Poll();
        if (!SameQueue(queue, retirementQueue_.Get())) return E_INVALIDARG;
        const HRESULT hr = client_.AcknowledgeQueuedReturn(nextFrame_, retirement_.Get(), retirementValue_, queue);
        if (hr == S_OK) Retired(true);
        else state_ = State::Failed;
        return hr;
    }
    // A skipped game frame invalidates every secondary feature history on the next successful handoff.
    void MarkSkipped() { reset_ = true; }
    // Submit without waiting for secondary completion. Only one frame may be outstanding.
    // ```cpp
    // auto sent = generation.SubmitAsync(chain, queue, frame, color, depth, motion);
    // ```
    HandoffResult SubmitAsync(MultiGpu::OwnedCommandChain& chain, ID3D12CommandQueue* queue, FrameCommand frame,
                              ID3D12Resource* color, ID3D12Resource* depth, ID3D12Resource* motion)
    {
        if (state_ != State::Ready)
            return { E_PENDING, HandoffPhase::Validate };
        if (requiredQueue_ && !SameQueue(queue, requiredQueue_.Get()))
            return { E_INVALIDARG, HandoffPhase::Validate };
        frame.frameNumber = nextFrame_;
        if (reset_)
            frame.resetMask |= (1u << passCount_) - 1u;
        const auto result = SendFrame(transfer_, client_, chain, queue, frame, color, depth, motion);
        if (SUCCEEDED(result.result))
        {
            state_ = State::PendingWorker;
            submissionQueue_ = queue;
            requiredQueue_.Reset();
            reset_ = false;
        }
        else if (result.phase != HandoffPhase::Validate)
            state_ = State::Failed;
        return result;
    }
    // E_PENDING is normal and does not invalidate the outstanding frame.
    // ```cpp
    // if (generation.PollReturn() == S_OK) generation.RecordReturn(commands, output);
    // ```
    HRESULT PollReturn()
    {
        if (state_ == State::AwaitingCopy) return S_OK;
        if (state_ != State::PendingWorker) return state_ == State::Failed ? E_ABORT : E_INVALIDARG;
        const HRESULT hr = client_.PollReturn(nextFrame_);
        if (hr == S_OK)
        {
            state_ = State::AwaitingCopy;
            orderedRetirement_.Reset();
        }
        else if (hr != E_PENDING) state_ = State::Failed;
        return hr;
    }
    // A positive timeout fails closed; zero is the same non-destructive poll.
    // ```cpp
    // Check(generation.WaitReturn(10000), "Wait for the secondary frame");
    // ```
    HRESULT WaitReturn(DWORD timeoutMs)
    {
        if (timeoutMs == INFINITE) return E_INVALIDARG;
        if (timeoutMs == 0) return PollReturn();
        if (state_ == State::AwaitingCopy) return S_OK;
        if (state_ != State::PendingWorker) return state_ == State::Failed ? E_ABORT : E_INVALIDARG;
        const auto result = client_.WaitForReturn(nextFrame_, timeoutMs);
        state_ = result == MultiGpu::WorkerFenceWaitResult::Ready ? State::AwaitingCopy : State::Failed;
        if (result == MultiGpu::WorkerFenceWaitResult::Ready) orderedRetirement_.Reset();
        return result == MultiGpu::WorkerFenceWaitResult::Ready ? S_OK
             : result == MultiGpu::WorkerFenceWaitResult::Timeout ? HRESULT_FROM_WIN32(WAIT_TIMEOUT)
             : result == MultiGpu::WorkerFenceWaitResult::DeviceLost ? DXGI_ERROR_DEVICE_REMOVED : E_ABORT;
    }
    HandoffResult Submit(MultiGpu::OwnedCommandChain& chain, ID3D12CommandQueue* queue, FrameCommand frame,
                         ID3D12Resource* color, ID3D12Resource* depth, ID3D12Resource* motion, DWORD timeoutMs)
    {
        if (state_ != State::Ready) return { E_PENDING, HandoffPhase::Validate };
        if (timeoutMs == INFINITE) return { E_INVALIDARG, HandoffPhase::Validate };
        auto result = SubmitAsync(chain, queue, frame, color, depth, motion);
        if (FAILED(result.result)) return result;
        // Preserve the legacy zero-timeout failure behavior of the synchronous API.
        const auto waited = client_.WaitForReturn(nextFrame_, timeoutMs);
        state_ = waited == MultiGpu::WorkerFenceWaitResult::Ready ? State::AwaitingCopy : State::Failed;
        if (waited == MultiGpu::WorkerFenceWaitResult::Ready)
        {
            orderedRetirement_.Reset();
            result.phase = HandoffPhase::WorkerReturned;
        }
        else
            result.result = waited == MultiGpu::WorkerFenceWaitResult::Timeout
                                ? HRESULT_FROM_WIN32(WAIT_TIMEOUT) : E_ABORT;
        return result;
    }
    HRESULT RecordReturn(ID3D12GraphicsCommandList* commands, ID3D12Resource* output)
    {
        if (state_ != State::AwaitingCopy)
            return E_INVALIDARG;
        const HRESULT hr = transfer_.RecordInbound(commands, output);
        if (SUCCEEDED(hr))
            state_ = State::CopyRecorded;
        return hr;
    }
    // Fence and value must cover the submitted primary continuation containing RecordReturn.
    HRESULT TrackRetirement(ID3D12Fence* fence, uint64_t value)
    {
        if (state_ != State::CopyRecorded || !fence || !value)
            return E_INVALIDARG;
        retirement_ = fence;
        retirementQueue_.Reset();
        retirementValue_ = value;
        state_ = State::AwaitingRetirement;
        return S_OK;
    }
    // Call only after RecordReturn's list and this fence signal were successfully
    // enqueued on the exact real queue used by SubmitAsync. The next outbound is
    // then restricted to that queue; the worker cannot overwrite until its Ready signal.
    // ```cpp
    // queue->Signal(fence, value);
    // generation.TrackRetirement(fence, value, queue);
    // ```
    HRESULT TrackRetirement(ID3D12Fence* fence, uint64_t value, ID3D12CommandQueue* queue)
    {
        if (state_ != State::CopyRecorded || !fence || !value ||
            value == (std::numeric_limits<uint64_t>::max)() || !SameQueue(queue, submissionQueue_.Get()) ||
            !client_.Owns(fence) || !client_.Owns(queue))
            return E_INVALIDARG;
        if (fence->GetCompletedValue() == (std::numeric_limits<uint64_t>::max)())
            return DXGI_ERROR_DEVICE_REMOVED;
        Microsoft::WRL::ComPtr<ID3D12Device> owner;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&owner))) || !owner || FAILED(owner->GetDeviceRemovedReason()))
            return DXGI_ERROR_DEVICE_REMOVED;
        retirement_ = fence;
        retirementValue_ = value;
        retirementQueue_ = queue;
        state_ = State::AwaitingRetirement;
        return S_OK;
    }
    HRESULT Stop(DWORD timeoutMs)
    {
        if (state_ != State::Ready)
            return E_PENDING;
        const HRESULT hr = client_.Stop(timeoutMs);
        state_ = SUCCEEDED(hr) ? State::Stopped : State::Failed;
        return hr;
    }
    const FrameTransfer& Transfer() const { return transfer_; }
    HANDLE ProcessHandle() const { return client_.ProcessHandle(); }
#ifdef DLSSNR_WORKER_TESTING
    WorkerClient& TestingClient() { return client_; }
#endif
};
} // namespace DlssNr::Worker
