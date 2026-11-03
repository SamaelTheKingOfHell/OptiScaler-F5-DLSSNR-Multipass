// Scheduling-policy test vectors. These intervals are not GPU/game benchmarks.
#include "../OptiScaler/dlssnr/DlssNr_AdaptiveBalance.h"
#include <array>
#include <cassert>
#include <iostream>
#include <limits>

using DlssNr::AdaptiveBalance::Controller;
using DlssNr::AdaptiveBalance::Phase;

struct Run
{
    Controller controller;
    double now = 0, lastChange = -30;
    uint64_t picture = 0;
    unsigned changes = 0;
    void Start(unsigned total, unsigned manual) { controller.Begin(total, manual, 1, now); }
    void Step(double ms, bool ready = true, bool failed = false)
    {
        now += 0.1;
        const auto before = controller.PrimaryPasses();
        controller.Observe(1, ++picture, now, ms, ready, failed);
        if (before != controller.PrimaryPasses())
        {
            assert(now - lastChange >= 29.999);
            lastChange = now; ++changes;
        }
    }
    void Feed(const std::array<double, 4>& costs, unsigned frames)
    { for (unsigned i = 0; i < frames; ++i) Step(costs[controller.PrimaryPasses()]); }
};

int main()
{
    // Both mixed placement and primary-only can win; switching is bounded.
    for (unsigned winner : { 0u, 1u, 2u, 3u })
    {
        Run run; run.Start(3, 2);
        std::array<double, 4> costs { 30, 30, 30, 30 }; costs[winner] = 12;
        run.Feed(costs, 1800);
        assert(run.controller.CurrentPhase() == Phase::Settled);
        assert(run.controller.PrimaryPasses() == winner);
        const auto changes = run.changes;
        run.Feed(costs, 1000);
        assert(run.changes == changes);
    }
    // A single pass is indivisible; choose the faster device without dropping it.
    Run single; single.Start(1, 0); single.Feed({ 25, 10, 0, 0 }, 900);
    assert(single.controller.PrimaryPasses() == 1 && single.controller.CurrentPhase() == Phase::Settled);

    // Worker startup, duplicate IDs, stale contexts and invalid timestamps cannot steer placement.
    Run waiting; waiting.Start(3, 2);
    for (unsigned i = 0; i < 1000; ++i) waiting.Step(2, false);
    assert(waiting.controller.PrimaryPasses() == 2 && waiting.changes == 0);
    waiting.controller.Observe(2, ++waiting.picture, waiting.now + 500, 1, true, false);
    waiting.controller.Observe(1, waiting.picture - 1, waiting.now + 500, 1, true, false);
    waiting.controller.Observe(1, ++waiting.picture, std::numeric_limits<double>::quiet_NaN(), 1, true, false);
    assert(waiting.changes == 0 && waiting.controller.PrimaryPasses() == 2);
    waiting.Feed({ 30, 30, 20, 10 }, 1800);
    assert(waiting.controller.PrimaryPasses() == 3);

    // Improvements below 1 ms / 8% are not enough to disturb the initial route.
    Run close; close.Start(3, 2); close.Feed({ 19.5, 19.3, 20, 19.2 }, 1800);
    assert(close.controller.PrimaryPasses() == 2);

    // A faster median with a worse p95 is rejected.
    Run tails; tails.Start(1, 1);
    for (unsigned i = 0; i < 900; ++i)
        tails.Step(tails.controller.PrimaryPasses() == 1 ? 20 : (i % 10 == 0 ? 100 : 10));
    assert(tails.controller.PrimaryPasses() == 1);

    // An unavailable worker is not scored as fast fallback on the primary GPU.
    Run failure; failure.Start(1, 0);
    for (unsigned i = 0; i < 900; ++i)
        failure.Step(10, failure.controller.PrimaryPasses() == 1, failure.controller.PrimaryPasses() == 0);
    assert(failure.controller.PrimaryPasses() == 1 && failure.controller.CurrentPhase() == Phase::Settled);

    // Sustained slowdown reopens calibration; a workload change invalidates old scores/tokens.
    Run changing; changing.Start(1, 1); changing.Feed({ 30, 10, 0, 0 }, 1000);
    const auto revision = changing.controller.Revision();
    changing.Feed({ 10, 30, 0, 0 }, 1500);
    assert(changing.controller.Revision() > revision && changing.controller.PrimaryPasses() == 0);
    changing.controller.Begin(3, 2, 77, changing.now);
    assert(changing.controller.CurrentPhase() == Phase::Warming && changing.controller.PrimaryPasses() == 2);
    changing.controller.Observe(1, ++changing.picture, changing.now + 500, 1, true, false);
    assert(changing.controller.PrimaryPasses() == 2 && changing.controller.MedianMs() == 0);
    changing.controller.Begin(4, 0, 78, changing.now);
    assert(changing.controller.CurrentPhase() == Phase::Off);
    std::cout << "Adaptive scheduling policy passed (test vectors, not a performance benchmark).\n";
}
