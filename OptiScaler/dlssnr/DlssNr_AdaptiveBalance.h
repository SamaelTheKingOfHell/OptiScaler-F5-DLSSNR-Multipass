#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace DlssNr::AdaptiveBalance
{
enum class Phase : uint32_t { Off, Waiting, Warming, Measuring, Settled, Cooldown };

// Selects placement from supplied picture intervals, never from guessed GPU load.
// The host supplies one matching completed picture at a time and owns history resets.
// ```cpp
// controller.Begin(3, 2, workloadId, seconds);
// controller.Observe(workloadId, pictureId, seconds, intervalMs, routeReady, routeFailed);
// unsigned primaryPasses = controller.PrimaryPasses();
// ```
class Controller
{
    struct Score { double median = 0, p95 = 0; bool valid = false; };
    std::array<Score, 4> scores_ {};
    std::array<bool, 4> tested_ {};
    std::array<double, 240> samples_ {};
    unsigned total_ = 0, current_ = 0, best_ = 0, warmFrames_ = 0, count_ = 0, cursor_ = 0;
    uint64_t context_ = 0, picture_ = 0, revision_ = 0;
    double switched_ = 0, phaseStarted_ = 0, cycleStarted_ = 0, lastObserved_ = 0;
    Phase phase_ = Phase::Off;
    bool measured_ = false, finishedSearch_ = false;
    Score published_ {};

    void ClearSamples(double now)
    { count_ = cursor_ = 0; phaseStarted_ = now; }

    void Select(unsigned split, double now)
    {
        if (split != current_) ++revision_;
        current_ = split;
        switched_ = now;
        warmFrames_ = 0;
        measured_ = false;
        phase_ = Phase::Warming;
        ClearSamples(now);
    }

    Score Summarize() const
    {
        if (!count_) return {};
        auto sorted = samples_;
        std::sort(sorted.begin(), sorted.begin() + count_);
        return { sorted[count_ / 2], sorted[(count_ * 95 - 1) / 100], true };
    }

    static bool Better(const Score& candidate, const Score& incumbent)
    {
        return candidate.valid && (!incumbent.valid ||
            (incumbent.median - candidate.median >= std::max(1.0, incumbent.median * 0.08) &&
             candidate.p95 <= incumbent.p95 * 1.10));
    }

    void Advance(double now)
    {
        if (now - switched_ < 30.0) { phase_ = Phase::Cooldown; return; }
        unsigned next = total_ + 1;
        if (!tested_[total_]) next = total_; // establish the primary-only alternative early
        else for (unsigned i = 0; i <= total_; ++i) if (!tested_[i]) { next = i; break; }
        if (next <= total_) { Select(next, now); return; }
        finishedSearch_ = true;
        if (current_ != best_) { Select(best_, now); return; }
        phase_ = Phase::Settled;
        ClearSamples(now);
    }

  public:
    // A different workload starts a fresh comparison, without changing saved settings.
    // ```cpp
    // controller.Begin(passCount, manualPrefix, configurationHash, now);
    // ```
    void Begin(unsigned total, unsigned manual, uint64_t context, double now)
    {
        if (!total || total > 3 || manual > total || !std::isfinite(now)) { Stop(); return; }
        if (total_ == total && context_ == context && phase_ != Phase::Off) return;
        total_ = total; context_ = context; best_ = current_ = manual;
        scores_ = {}; tested_ = {}; published_ = {};
        picture_ = 0; lastObserved_ = 0; finishedSearch_ = false;
        cycleStarted_ = now; ++revision_; Select(manual, now);
    }

    // Invalid, duplicate, stale, paused or unready pictures never enter timing scores.
    // Failed routes are rejected after the same switch cooldown as measured routes.
    // ```cpp
    // controller.Observe(workload, serial, now, elapsedMs, ready, failed);
    // ```
    void Observe(uint64_t context, uint64_t picture, double now, double intervalMs, bool ready, bool failed)
    {
        if (phase_ == Phase::Off || context != context_ || !picture || picture <= picture_ ||
            !std::isfinite(now) || now < switched_ || now <= lastObserved_) return;
        picture_ = picture; lastObserved_ = now;
        if (failed)
        {
            tested_[current_] = true; scores_[current_] = {}; measured_ = true;
            Advance(now); return;
        }
        if (!ready)
        {
            warmFrames_ = 0; measured_ = false; ClearSamples(now); phase_ = Phase::Waiting;
            return;
        }
        if (measured_ && !finishedSearch_) { Advance(now); return; }
        if (!std::isfinite(intervalMs) || intervalMs < 0.1 || intervalMs > 250.0)
        {
            warmFrames_ = 0; measured_ = false; ClearSamples(now); phase_ = Phase::Warming;
            return;
        }
        if (phase_ == Phase::Waiting) { phase_ = Phase::Warming; ClearSamples(now); }
        if (phase_ == Phase::Warming)
        {
            if (++warmFrames_ < 30 || now - phaseStarted_ < 2.0) return;
            phase_ = finishedSearch_ ? Phase::Settled : Phase::Measuring;
            ClearSamples(now); return;
        }
        samples_[cursor_] = intervalMs;
        cursor_ = (cursor_ + 1) % static_cast<unsigned>(samples_.size());
        count_ = std::min(count_ + 1, static_cast<unsigned>(samples_.size()));
        if (count_ < 60 || now - phaseStarted_ < 3.0) return;
        published_ = Summarize();
        if (phase_ == Phase::Settled)
        {
            const auto& baseline = scores_[best_];
            const bool degraded = baseline.valid && published_.median > baseline.median * 1.20 &&
                published_.median - baseline.median > 2.0;
            if (degraded && now - cycleStarted_ >= 60.0 && now - switched_ >= 30.0)
            {
                scores_ = {}; tested_ = {}; best_ = current_; finishedSearch_ = false;
                cycleStarted_ = now; ++revision_; Select(current_, now);
            }
            else ClearSamples(now);
            return;
        }
        scores_[current_] = published_; tested_[current_] = true; measured_ = true;
        if (Better(published_, scores_[best_])) best_ = current_;
        Advance(now);
    }

    // Disables calibration and invalidates outstanding feedback tokens.
    // ```cpp
    // controller.Stop();
    // ```
    void Stop() { if (phase_ != Phase::Off) ++revision_; phase_ = Phase::Off; total_ = 0; }
    unsigned PrimaryPasses() const { return current_; }
    uint64_t Context() const { return context_; }
    uint64_t Revision() const { return revision_; }
    Phase CurrentPhase() const { return phase_; }
    double MedianMs() const { return published_.median; }
    double P95Ms() const { return published_.p95; }
};
} // namespace DlssNr::AdaptiveBalance
