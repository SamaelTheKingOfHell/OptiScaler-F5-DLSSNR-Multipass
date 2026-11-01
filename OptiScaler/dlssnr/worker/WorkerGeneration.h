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
        AwaitingCopy,
        CopyRecorded,
        AwaitingRetirement,
        Failed,
        Stopped
    };
    FrameTransfer transfer_;
    WorkerClient client_;
    Microsoft::WRL::ComPtr<ID3D12Fence> retirement_;
    uint64_t retirementValue_ = 0, nextFrame_ = 1;
    uint32_t passCount_ = 0;
    bool reset_ = true;
    State state_ = State::Empty;

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
                retirement_.Reset();
                retirementValue_ = 0;
                ++nextFrame_;
                state_ = State::Ready;
            }
            else if (hr != E_PENDING)
                state_ = State::Failed;
            return hr;
        }
        return state_ == State::Ready ? S_OK : state_ == State::Failed ? E_ABORT : E_PENDING;
    }
    // A skipped game frame invalidates every secondary feature history on the next successful handoff.
    void MarkSkipped() { reset_ = true; }
    HandoffResult Submit(MultiGpu::OwnedCommandChain& chain, ID3D12CommandQueue* queue, FrameCommand frame,
                         ID3D12Resource* color, ID3D12Resource* depth, ID3D12Resource* motion, DWORD timeoutMs)
    {
        if (state_ != State::Ready)
            return { E_PENDING, HandoffPhase::Validate };
        frame.frameNumber = nextFrame_;
        if (reset_)
            frame.resetMask |= (1u << passCount_) - 1u;
        const auto result = SendFrameAndWait(transfer_, client_, chain, queue, frame, color, depth, motion, timeoutMs);
        if (SUCCEEDED(result.result))
        {
            state_ = State::AwaitingCopy;
            reset_ = false;
        }
        else if (result.phase != HandoffPhase::Validate)
            state_ = State::Failed;
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
        retirementValue_ = value;
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
