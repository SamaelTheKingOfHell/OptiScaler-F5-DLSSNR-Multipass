#pragma once

#include <windows.h>
#include <cstdint>
#include <limits>

namespace DlssNr::MultiGpu
{
// A failed operation after PrimarySubmitted may leave GPU work in flight. The owner must retain
// the resources until submitted work retires, or abandon them on device loss.
enum class SubmissionPhase
{
    Validate,
    PrimarySubmitted,
    OutboundSignalled,
    SecondaryWaiting,
    SecondarySubmitted,
    ReturnSignalled,
    PrimaryWaiting,
    Complete
};
struct SubmissionResult
{
    HRESULT result = E_FAIL;
    SubmissionPhase phase = SubmissionPhase::Validate;
    uint64_t ticket = 0;
};

// Queue-independent sequencing. A failure after submission permanently poisons this instance so
// later frames cannot overwrite resources that may still be in flight. Callers serialize access.
class CrossAdapterProtocol
{
    uint64_t nextTicket_ = 1;
    bool poisoned_ = false;

  public:
    bool Poisoned() const { return poisoned_; }

    template <typename Operations> SubmissionResult Submit(Operations& ops)
    {
        SubmissionResult result { E_INVALIDARG, SubmissionPhase::Validate, 0 };
        if (poisoned_)
        {
            result.result = E_ABORT;
            return result;
        }
        if (nextTicket_ == (std::numeric_limits<uint64_t>::max)())
            return result;
        result.ticket = nextTicket_++;
        ops.SubmitPrimary();
        result.phase = SubmissionPhase::PrimarySubmitted;
        HRESULT hr = ops.SignalPrimary(result.ticket);
        if (FAILED(hr))
            return Fail(result, hr);
        result.phase = SubmissionPhase::OutboundSignalled;
        hr = ops.WaitSecondary(result.ticket);
        if (FAILED(hr))
            return Fail(result, hr);
        result.phase = SubmissionPhase::SecondaryWaiting;
        ops.SubmitSecondary();
        result.phase = SubmissionPhase::SecondarySubmitted;
        hr = ops.SignalSecondary(result.ticket);
        if (FAILED(hr))
            return Fail(result, hr);
        result.phase = SubmissionPhase::ReturnSignalled;
        hr = ops.WaitPrimary(result.ticket);
        if (FAILED(hr))
            return Fail(result, hr);
        result.phase = SubmissionPhase::PrimaryWaiting;
        ops.SubmitPrimaryAfter();
        result.result = S_OK;
        result.phase = SubmissionPhase::Complete;
        return result;
    }

  private:
    SubmissionResult Fail(SubmissionResult result, HRESULT hr)
    {
        poisoned_ = true;
        result.result = hr;
        return result;
    }
};
} // namespace DlssNr::MultiGpu
