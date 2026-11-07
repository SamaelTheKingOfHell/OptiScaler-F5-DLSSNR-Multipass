#pragma once
// Network split: one DLSS-NR evaluate shared between the game GPU and a helper GPU (docs/NR-network-trace.md).
//
// The owner (game GPU) evaluates twice per picture: kernels [0, cut] (Head), then kernel 0 and [resume, end) (End).
// The helper evaluates kernel 0 and (cut, resume) (Segment) on its own instance of the same model, whose workspace has
// the same layout. Tensors cross between the two workspaces as byte ranges (a Plan, calibrated offline by
// tests/nr_gpu/nr_split_sets.py): forward ranges leave the owner's workspace after kernel `cut` and enter the helper's
// before kernel cut + 1; backward ranges leave the helper's after kernel resume - 1 and enter the owner's before kernel
// `resume`. Kernel 0 clears the model's synchronization area, so every instance runs it; the area itself is part of
// both range sets.
//
// This header holds the parts both processes share: the plan file, its compact form for the worker handshake, and
// the per-evaluate launch filter. It knows nothing of NvAPI types: launching a kernel and recording a copy are the
// caller's callbacks.

#include <algorithm>
#include <cstdint>
#include <istream>
#include <sstream>
#include <string>
#include <vector>

namespace DlssNr::NetworkSplit
{
inline constexpr unsigned MaxRanges = 128;

struct Range
{
    uint64_t offset = 0, bytes = 0, packed = 0; // workspace offset, length, offset inside the packed transfer
};

struct Plan
{
    int cut = 0, resume = 0;
    uint64_t workspaceBytes = 0; // calibrated workspace size: a mismatch means another resolution or model
    uint64_t layout = 0;         // workspace layout fingerprint it was calibrated on (DlssNr_SplitCalibration.h), 0 unknown
    std::vector<Range> forward, backward;
    uint64_t forwardBytes = 0, backwardBytes = 0;

    bool Valid() const
    {
        return cut > 0 && resume > cut + 1 && !forward.empty() && !backward.empty() &&
               forward.size() + backward.size() <= MaxRanges && forwardBytes && backwardBytes;
    }
    // A range must lie inside the workspace and be a whole number of 16-byte elements (the copy kernel's unit).
    bool Fits(uint64_t workspace) const
    {
        if (workspaceBytes && workspaceBytes != workspace)
            return false;
        for (const auto* set : { &forward, &backward })
            for (const auto& r : *set)
                if (r.bytes == 0 || r.bytes % 16 || r.offset % 16 || r.offset + r.bytes > workspace)
                    return false;
        return true;
    }
};

// Plan file:  "C <cut> <resume>", optional "W <workspace bytes>" and "L <layout fingerprint, hex>", then "F <offset> <bytes>" / "B <offset> <bytes>".
// ```cpp
// Plan plan; std::string why;
// if (!ParsePlan(file, plan, why)) LOG_WARN("network split plan: {}", why);
// ```
inline bool ParsePlan(std::istream& in, Plan& plan, std::string& error)
{
    plan = {};
    std::string line;
    while (std::getline(in, line))
    {
        std::istringstream l(line);
        char kind = 0;
        if (!(l >> kind) || kind == '#')
            continue;
        if (kind == 'C')
            l >> plan.cut >> plan.resume;
        else if (kind == 'W')
            l >> plan.workspaceBytes;
        else if (kind == 'L')
            l >> std::hex >> plan.layout;
        else if (kind == 'F' || kind == 'B')
        {
            Range r;
            if (!(l >> r.offset >> r.bytes))
            {
                error = "malformed range line";
                return false;
            }
            auto& total = kind == 'F' ? plan.forwardBytes : plan.backwardBytes;
            r.packed = total;
            total += r.bytes;
            (kind == 'F' ? plan.forward : plan.backward).push_back(r);
        }
    }
    if (!plan.Valid())
    {
        error = "the plan needs a cut, a resume after it, and forward and backward ranges (at most 128)";
        return false;
    }
    return true;
}

// The compact form carried by the worker handshake: forward ranges first, then backward ones.
struct WirePlan
{
    int32_t cut = 0, resume = 0;
    uint32_t forwardCount = 0, backwardCount = 0;
    uint64_t workspaceBytes = 0;
    uint64_t ranges[2 * MaxRanges] {}; // offset, bytes pairs
};
inline WirePlan ToWire(const Plan& plan)
{
    WirePlan w;
    w.cut = plan.cut;
    w.resume = plan.resume;
    w.workspaceBytes = plan.workspaceBytes;
    w.forwardCount = static_cast<uint32_t>(plan.forward.size());
    w.backwardCount = static_cast<uint32_t>(plan.backward.size());
    size_t i = 0;
    for (const auto* set : { &plan.forward, &plan.backward })
        for (const auto& r : *set)
        {
            w.ranges[i++] = r.offset;
            w.ranges[i++] = r.bytes;
        }
    return w;
}
inline bool FromWire(const WirePlan& w, Plan& plan)
{
    plan = {};
    if (w.forwardCount + w.backwardCount > MaxRanges)
        return false;
    plan.cut = w.cut;
    plan.resume = w.resume;
    plan.workspaceBytes = w.workspaceBytes;
    size_t i = 0;
    for (uint32_t k = 0; k < w.forwardCount + w.backwardCount; ++k)
    {
        Range r { w.ranges[i], w.ranges[i + 1], 0 };
        i += 2;
        const bool forward = k < w.forwardCount;
        auto& total = forward ? plan.forwardBytes : plan.backwardBytes;
        r.packed = total;
        total += r.bytes;
        (forward ? plan.forward : plan.backward).push_back(r);
    }
    return plan.Valid();
}

enum class Role : uint8_t
{
    Off,     // every kernel goes out unchanged
    Head,    // owner, first evaluate: [0, cut], then forward ranges out to staging
    Segment, // helper: kernel 0 and (cut, resume); forward ranges in before cut + 1, backward out after resume - 1
    End      // owner, second evaluate: kernel 0 and [resume, end); backward ranges in before resume
};

// One evaluate's filter. Not thread-safe: the caller serializes evaluates (g_nrMutex / the worker loop).
// ```cpp
// filter.Begin(Role::Head, plan, workspaceVa, forwardStagingVa, backwardStagingVa);
// model.Evaluate(...);   // every kernel passes through filter.Launch(kernel, launch, copy, barrier)
// bool complete = filter.End();
// ```
class Filter
{
    Role role_ = Role::Off;
    const Plan* plan_ = nullptr;
    uint64_t workspace_ = 0, forward_ = 0, backward_ = 0;
    int index_ = 0;
    bool forwardDone_ = false, backwardDone_ = false;

    template<class Copy, class Barrier>
    int Transfer(const std::vector<Range>& set, uint64_t staging, bool intoWorkspace, Copy& copy, Barrier& barrier)
    {
        barrier();
        for (const auto& r : set)
        {
            const int status = intoWorkspace ? copy(workspace_ + r.offset, staging + r.packed, r.bytes)
                                             : copy(staging + r.packed, workspace_ + r.offset, r.bytes);
            if (status)
                return status;
        }
        barrier();
        return 0;
    }

  public:
    void Begin(Role role, const Plan* plan, uint64_t workspace, uint64_t forwardStaging, uint64_t backwardStaging)
    {
        role_ = plan && plan->Valid() && workspace ? role : Role::Off;
        plan_ = plan;
        workspace_ = workspace;
        forward_ = forwardStaging;
        backward_ = backwardStaging;
        index_ = 0;
        forwardDone_ = backwardDone_ = false;
    }
    Role Active() const { return role_; }
    // True when this evaluate recorded every transfer its role owes (a shorter kernel chain than calibrated is not).
    bool End()
    {
        const bool ok = role_ == Role::Off || (role_ == Role::Head && forwardDone_) ||
                        (role_ == Role::Segment && forwardDone_ && backwardDone_) ||
                        (role_ == Role::End && backwardDone_);
        role_ = Role::Off;
        return ok;
    }
    // Handles one kernel of the evaluate. `launch()` sends the kernel; `copy(dst, src, bytes)` records a copy;
    // `barrier()` records a UAV barrier. Returns the first nonzero status.
    template<class Launch, class Copy, class Barrier> int Kernel(Launch launch, Copy copy, Barrier barrier)
    {
        const int i = index_++;
        if (role_ == Role::Off)
            return launch();
        const int cut = plan_->cut, resume = plan_->resume;
        switch (role_)
        {
        case Role::Head:
            if (i <= cut)
                return launch();
            if (!forwardDone_)
            {
                forwardDone_ = true;
                return Transfer(plan_->forward, forward_, false, copy, barrier);
            }
            return 0;
        case Role::Segment:
        {
            if ((i > 0 && i <= cut) || i >= resume)
                return 0;
            if (i > cut && !forwardDone_)
            {
                forwardDone_ = true;
                if (int s = Transfer(plan_->forward, forward_, true, copy, barrier))
                    return s;
            }
            if (int s = launch())
                return s;
            if (i + 1 == resume)
            {
                backwardDone_ = true;
                return Transfer(plan_->backward, backward_, false, copy, barrier);
            }
            return 0;
        }
        case Role::End:
            if (i > 0 && i < resume)
                return 0;
            if (i >= resume && !backwardDone_)
            {
                backwardDone_ = true;
                if (int s = Transfer(plan_->backward, backward_, true, copy, barrier))
                    return s;
            }
            return launch();
        default:
            return launch();
        }
    }
};
} // namespace DlssNr::NetworkSplit
