// Host check of native/NativeLowLatencyRule.h (when Optical F5Low's own low latency runs and stands aside) and
// native/XefgReflexRule.h (a game's own Reflex with XeFG). No GPU, no game.
// cl /std:c++20 /EHsc /W4 /Iexternal\vulkan\include tests/nr_native_low_latency_smoke.cpp
#include "../OptiScaler/native/XefgReflexRule.h"
#include "../OptiScaler/native/NativeLowLatencyRule.h"
#include "../OptiScaler/native/NativeLowLatencyVkSync.h"
#include "../OptiScaler/framegen/xefg/XeFGFrameId.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

#include <vulkan/vulkan_core.h>

using namespace native::lowlatency;

static int fails = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                                                \
            ++fails;                                                                                                   \
        }                                                                                                              \
    } while (0)

// dxvk with the Vulkan bridge, as a sequence: the game's Present of frame f pushes its entry (id 100 + f, the site chosen
// then), dxvk presents frame f `lag` game frames later and pops. Each frame must be paired exactly once (at the game's
// Present for a GamePresent entry, at dxvk's for a VulkanPresent one or when nothing was queued) and a GamePresent frame
// must reach XeFG with its own ID.
struct SeqResult
{
    int pairs[64] = {};
    uint32_t idSeen[64] = {};
};

// `siteOf(f)`: the site the game's Present picks for frame f. `failed(f)`: its real Present did not return S_OK (undone).
template <class SiteFn, class FailFn> SeqResult RunSeq(int frames, int lag, SiteFn siteOf, FailFn failed)
{
    SeqResult r;
    FrameLedger led;
    auto dxvkPresent = [&](int frame)
    {
        FrameLedger::Entry e;
        const bool popped = led.Pop(&e);

        if (!popped || e.site == SleepSite::VulkanPresent)
            ++r.pairs[frame];

        r.idSeen[frame] = popped && e.site == SleepSite::GamePresent ? e.frameId : 0;
    };

    int next = 0; // the next frame dxvk presents (a failed frame is never presented)
    auto skipFailed = [&] { while (next < frames && failed(next)) ++next; };

    for (int f = 0; f < frames; ++f)
    {
        const auto site = siteOf(f);
        const auto seq = led.Push(100 + f, site);

        if (site == SleepSite::GamePresent)
            ++r.pairs[f];

        if (failed(f))
            CHECK(led.Undo(seq));

        // dxvk is `lag` frames behind
        skipFailed();

        if (f - lag >= 0 && next <= f - lag)
        {
            dxvkPresent(next++);
            skipFailed();
        }
    }

    while (true)
    {
        skipFailed();

        if (next >= frames)
            break;

        dxvkPresent(next++);
    }

    return r;
}

int main()
{
    Inputs in;
    in.f5lowRunning = true;

    // The default: on with an Optical F5Low mode
    CHECK(Decide(in) == Decision::Run);

    // Auto needs an Optical F5Low mode; On does not
    in.f5lowRunning = false;
    CHECK(Decide(in) == Decision::NoF5Low);
    in.setting = Setting::On;
    CHECK(Decide(in) == Decision::Run);

    // Off beats everything
    in.setting = Setting::Off;
    in.f5lowRunning = true;
    CHECK(Decide(in) == Decision::SettingOff);

    // XeFG alone: Auto runs us to feed its XeLL; Off still wins, and the game's own Reflex (which feeds XeLL itself) or
    // XeLL comes first
    {
        Inputs x;
        x.xefgRunning = true;
        CHECK(Decide(x) == Decision::Run);
        x.setting = Setting::Off;
        CHECK(Decide(x) == Decision::SettingOff);
        x.setting = Setting::Auto;
        x.gameCallsReflex = true;
        CHECK(Decide(x) == Decision::GameRunsReflex);
        x.gameCallsReflex = false;
        x.gameRunsXell = true;
        CHECK(Decide(x) == Decision::GameRunsXell);
        x.gameRunsXell = false;
        x.forceXell = true;
        CHECK(Decide(x) == Decision::ForceXell);
    }

    // FSR FG generating (D3D12, or D3D11 through the bridge), no Optical F5Low mode: Auto runs us; Off, the game's own
    // Reflex and fakenvapi's Force Disable still win
    {
        Inputs f;
        f.fsrFgRunning = true;
        CHECK(Decide(f) == Decision::Run);
        CHECK(RunReason(f) == RunsFor::FsrFg);
        f.setting = Setting::Off;
        CHECK(Decide(f) == Decision::SettingOff);
        f.setting = Setting::Auto;
        f.gameCallsReflex = true;
        CHECK(Decide(f) == Decision::GameRunsReflex);
        f.gameCallsReflex = false;
        f.forceReflexDisabled = true;
        CHECK(Decide(f) == Decision::ForceReflexDisabled);
        f.forceReflexDisabled = false;

        // FSR FG paused or off: Auto stops again
        f.fsrFgRunning = false;
        CHECK(Decide(f) == Decision::NoF5Low);

        // What it runs for: an Optical F5Low mode first, then XeFG, then FSR FG; On with nothing keeps the F5Low name
        Inputs r;
        r.fsrFgRunning = true;
        r.f5lowRunning = true;
        CHECK(RunReason(r) == RunsFor::F5Low);
        r.f5lowRunning = false;
        r.xefgRunning = true;
        CHECK(RunReason(r) == RunsFor::Xefg);
        Inputs on;
        on.setting = Setting::On;
        CHECK(RunReason(on) == RunsFor::F5Low);
    }

    // The game's own Reflex: ours never runs, in either setting
    for (auto setting : { Setting::Auto, Setting::On })
    {
        Inputs g;
        g.setting = setting;
        g.f5lowRunning = true;
        g.gameCallsReflex = true;
        CHECK(Decide(g) == Decision::GameRunsReflex);
    }

    // The game's own XeLL: ours stands aside (a second method would be a second wait); its Reflex still comes first
    {
        Inputs x;
        x.setting = Setting::On;
        x.gameRunsXell = true;
        CHECK(Decide(x) == Decision::GameRunsXell);
        x.gameCallsReflex = true;
        CHECK(Decide(x) == Decision::GameRunsReflex);
    }

    // fakenvapi's settings are respected, never overridden
    {
        Inputs f;
        f.f5lowRunning = true;
        f.forceReflexDisabled = true;
        CHECK(Decide(f) == Decision::ForceReflexDisabled);
        f.forceReflexDisabled = false;
        f.forceXell = true;
        CHECK(Decide(f) == Decision::ForceXell);
        f.forceXell = false;
        f.otherFrameGenerationOwner = true;
        CHECK(Decide(f) == Decision::FrameGenerationOwnsReflex);
        f.otherFrameGenerationOwner = false;
        f.apiAvailable = false;
        CHECK(Decide(f) == Decision::NoApi);
    }

    // The game beats the fakenvapi reasons (the more useful line)
    {
        Inputs f;
        f.f5lowRunning = true;
        f.gameCallsReflex = true;
        f.forceReflexDisabled = true;
        CHECK(Decide(f) == Decision::GameRunsReflex);
    }

    // Every reason has its plain text; Run has none
    for (auto d : { Decision::SettingOff, Decision::NoF5Low, Decision::GameRunsReflex, Decision::GameRunsXell,
                    Decision::ForceReflexDisabled, Decision::ForceXell, Decision::FrameGenerationOwnsReflex,
                    Decision::NoApi })
        CHECK(DecisionText(d)[0] != '\0');

    CHECK(DecisionText(Decision::Run)[0] == '\0');
    CHECK(strcmp(DecisionText(Decision::GameRunsReflex), "Use the game's own Reflex settings") == 0);

    // The status label: On when it runs, Standing aside when something else owns latency, Off otherwise
    CHECK(strcmp(StatusLabel(Decision::Run), "On") == 0);

    for (auto d : { Decision::GameRunsReflex, Decision::GameRunsXell, Decision::ForceReflexDisabled,
                    Decision::ForceXell, Decision::FrameGenerationOwnsReflex })
        CHECK(strcmp(StatusLabel(d), "Standing aside") == 0);

    for (auto d : { Decision::SettingOff, Decision::NoF5Low, Decision::NoApi })
        CHECK(strcmp(StatusLabel(d), "Off") == 0);

    // With frame generation's swap chain, only the game's present to it counts; the wrapped swap chain's presents
    // behind it (real and generated frames) are skipped, and count again once frame generation's presents stop
    CHECK(!SwapChainPresentIgnored(5000.0, 0.0));
    CHECK(SwapChainPresentIgnored(5000.0, 4990.0));
    CHECK(SwapChainPresentIgnored(5000.0, 5000.0));
    CHECK(!SwapChainPresentIgnored(5000.0, 5000.0 - kFrameGenerationPresentHoldMs));
    CHECK(!SwapChainPresentIgnored(9000.0, 4990.0));

    // Which Reflex surface a game's present goes through: decided per call from what the caller says, never remembered
    // from an earlier call (a D3D call after a Vulkan one is D3D; no D3D12 device pointer reaches the Vulkan calls).
    {
        int vkDevice = 0, bridgeDevice12 = 0, d3dDevice = 0;

        // A Vulkan game, not bridged: its VkDevice through the Vulkan surface
        auto t = PresentTarget(true, false, &vkDevice, &bridgeDevice12);
        CHECK(t.api == Api::Vulkan && t.device == &vkDevice);

        // Then a D3D game's present (the Vulkan one before it leaves nothing behind)
        t = PresentTarget(false, false, &d3dDevice, nullptr);
        CHECK(t.api == Api::D3D && t.device == &d3dDevice);

        // A bridged Vulkan game: the bridge's private D3D12 device through the D3D surface; no Vulkan call at all
        t = PresentTarget(true, true, &vkDevice, &bridgeDevice12);
        CHECK(t.api == Api::D3D && t.device == &bridgeDevice12);

        // The bridge's device is gone mid-present: nothing to call (and still not the VkDevice through the D3D calls)
        t = PresentTarget(true, true, &vkDevice, nullptr);
        CHECK(t.device == nullptr);

        // And back to the unbridged Vulkan game
        t = PresentTarget(true, false, &vkDevice, &bridgeDevice12);
        CHECK(t.api == Api::Vulkan && t.device == &vkDevice);
    }

    // The Vulkan Sleep pair (NativeLowLatencyVkSync.h): init once, strictly rising values from 1, a wait on each value
    {
        int initCalls = 0, sleepCalls = 0, waitCalls = 0;
        uint64_t lastSleep = 0, lastWait = 0;
        int semaphoreStorage = 0;
        bool timeline = true;
        int initStatus = 0;
        bool reached = true;
        void* semaphore = &semaphoreStorage;

        VkSleepCalls calls;
        calls.timelineSemaphoresOn = [&] { return timeline; };
        calls.init = [&](void** out)
        {
            ++initCalls;
            *out = semaphore;
            return initStatus;
        };
        calls.sleep = [&](uint64_t value)
        {
            ++sleepCalls;
            lastSleep = value;
            return 0;
        };
        calls.wait = [&](void* sem, uint64_t value, uint64_t timeoutNs)
        {
            ++waitCalls;
            lastWait = value;
            CHECK(sem == &semaphoreStorage);
            CHECK(timeoutNs == kVkSleepWaitNs && timeoutNs > 0 && timeoutNs < 5ull * 1000 * 1000 * 1000);
            return reached;
        };

        VkSleepSync sync;
        bool first = false;

        for (uint64_t frame = 1; frame <= 5; ++frame)
        {
            CHECK(VkSleepFrame(sync, calls, &first) == VkSleepResult::Slept);
            CHECK(lastSleep == frame && lastWait == frame);
        }

        CHECK(initCalls == 1 && sleepCalls == 5 && waitCalls == 5);

        // The wait never gets its value: one first-time flag, the value keeps rising
        reached = false;
        CHECK(VkSleepFrame(sync, calls, &first) == VkSleepResult::TimedOut && first && lastSleep == 6);
        CHECK(VkSleepFrame(sync, calls, &first) == VkSleepResult::TimedOut && !first && lastSleep == 7);

        // A wait that comes through again clears the count of timeouts in a row
        reached = true;
        CHECK(VkSleepFrame(sync, calls, &first) == VkSleepResult::Slept && lastSleep == 8);
        reached = false;
        CHECK(VkSleepFrame(sync, calls, &first) == VkSleepResult::TimedOut && !first && lastSleep == 9);
        CHECK(VkSleepFrame(sync, calls, &first) == VkSleepResult::TimedOut && lastSleep == 10);

        // Many timeouts in a row after the driver has signalled before: logged once, tried again, never given up
        CHECK(VkSleepFrame(sync, calls, &first) == VkSleepResult::TimedOut && lastSleep == 11);
        CHECK(VkSleepFrame(sync, calls, &first) == VkSleepResult::TimedOut && lastSleep == 12);
        CHECK(VkSleepFrame(sync, calls, &first) == VkSleepResult::TimedOut && !first && lastSleep == 13);
        CHECK(sync.state == VkSleepState::Ready);
        reached = true;
        CHECK(VkSleepFrame(sync, calls, &first) == VkSleepResult::Slept && lastSleep == 14);

        // A device whose semaphore never reaches a value (the driver does not signal): three in a row and it gives up,
        // no more NVAPI calls at all
        reached = false;
        VkSleepSync silent;
        CHECK(VkSleepFrame(silent, calls, &first) == VkSleepResult::TimedOut && first);
        CHECK(VkSleepFrame(silent, calls, &first) == VkSleepResult::TimedOut && !first);
        CHECK(silent.state == VkSleepState::Ready);
        CHECK(VkSleepFrame(silent, calls, &first) == VkSleepResult::TimedOut);
        CHECK(silent.state == VkSleepState::Unavailable && silent.why == VkSleepWhy::WaitTimedOut);
        const int sleepsBefore = sleepCalls;
        CHECK(VkSleepFrame(silent, calls, &first) == VkSleepResult::Unavailable && sleepCalls == sleepsBefore);

        // A device without the timeline feature: no NVAPI call of any kind
        initCalls = sleepCalls = waitCalls = 0;
        timeline = false;
        VkSleepSync noTimeline;
        CHECK(!VkSleepReady(noTimeline, calls));
        CHECK(VkSleepFrame(noTimeline, calls) == VkSleepResult::Unavailable);
        CHECK(noTimeline.why == VkSleepWhy::NoTimelineSemaphores);
        CHECK(initCalls == 0 && sleepCalls == 0 && waitCalls == 0);

        // A failed init: unavailable for good, the status kept for the log, never a Sleep (fakenvapi would signal a null
        // semaphore), never a second init
        timeline = true;
        initStatus = -5;
        VkSleepSync failed;
        CHECK(VkSleepFrame(failed, calls) == VkSleepResult::Unavailable);
        CHECK(failed.why == VkSleepWhy::InitFailed && failed.initStatus == -5);
        initStatus = 0;
        CHECK(VkSleepFrame(failed, calls) == VkSleepResult::Unavailable);
        CHECK(initCalls == 1 && sleepCalls == 0 && waitCalls == 0);

        // An init that returns OK and no semaphore is as bad
        semaphore = nullptr;
        initCalls = 0;
        VkSleepSync nullSemaphore;
        CHECK(VkSleepFrame(nullSemaphore, calls) == VkSleepResult::Unavailable);
        CHECK(nullSemaphore.why == VkSleepWhy::NoSemaphore);
        CHECK(sleepCalls == 0 && waitCalls == 0);

        // A failing Sleep call is not waited for
        semaphore = &semaphoreStorage;
        VkSleepSync sleepFails;
        calls.sleep = [&](uint64_t) { return -1; };
        CHECK(VkSleepFrame(sleepFails, calls) == VkSleepResult::SleepFailed);
        CHECK(waitCalls == 0);
    }

    // Frames shown per rendered one: 1 + generated only while frame generation generates (waiting for frame data or
    // paused it shows the game's frames alone), so XeFG 3x and 4x divide the cap by 3 and 4, not by a fixed 2
    CHECK(ShownPerRendered(true, 1) == 2);
    CHECK(ShownPerRendered(true, 2) == 3);
    CHECK(ShownPerRendered(true, 3) == 4);
    CHECK(ShownPerRendered(false, 1) == 1);
    CHECK(ShownPerRendered(false, 3) == 1);
    CHECK(CapPerRenderedFrame(120.0f, ShownPerRendered(true, 3)) == 30.0f);
    CHECK(CapPerRenderedFrame(50.0f, ShownPerRendered(false, 1)) == 50.0f);

    // The frame cap while we run (PlanCap): one waiter per frame
    {
        CapInputs cap;

        // Not running: not ours, whatever the cap
        cap.capFps = 60.0f;
        CHECK(PlanCap(cap).owner == CapOwner::NotOurs);
        CHECK(PlanCap(cap).minimumIntervalUs == 0);

        // Running with no cap: ours, nothing to limit (the limiter has nothing to do either)
        cap.running = true;
        cap.capFps = 0.0f;
        CHECK(PlanCap(cap).owner == CapOwner::LowLatency);
        CHECK(PlanCap(cap).minimumIntervalUs == 0);

        // Real Reflex, no frame generation: Reflex keeps 60 fps
        cap.capFps = 60.0f;
        CHECK(PlanCap(cap).owner == CapOwner::LowLatency);
        CHECK(PlanCap(cap).minimumIntervalUs == 16667);

        // Real Reflex + FSR FG where the limiter waits before our Sleep: the limiter keeps it
        cap.fsrFgActive = true;
        cap.limiterBeforeSleep = true;
        CHECK(PlanCap(cap).owner == CapOwner::Limiter);
        CHECK(PlanCap(cap).minimumIntervalUs == 0);

        // ... a cap of 0 does not change who would keep it
        cap.capFps = 0.0f;
        CHECK(PlanCap(cap).owner == CapOwner::Limiter);
        cap.capFps = 60.0f;

        // ... on the Vulkan bridge (limiter after our Sleep): Reflex keeps the game's frames, half of the shown 60
        cap.limiterBeforeSleep = false;
        CHECK(PlanCap(cap).owner == CapOwner::LowLatency);
        CHECK(PlanCap(cap).minimumIntervalUs == 33333);

        // fakenvapi with FSR FG: its method keeps the game's frames
        cap.method = Method::Fakenvapi;
        cap.limiterBeforeSleep = true;
        CHECK(PlanCap(cap).owner == CapOwner::LowLatency);
        CHECK(PlanCap(cap).minimumIntervalUs == 33333);

        // fakenvapi without frame generation
        cap.fsrFgActive = false;
        CHECK(PlanCap(cap).minimumIntervalUs == 16667);

        // XeFG's XeLL counts shown frames
        cap.method = Method::XellWithXefg;
        cap.xefgGenerating = true;
        cap.capFps = 120.0f;
        CHECK(PlanCap(cap).owner == CapOwner::LowLatency);
        CHECK(PlanCap(cap).minimumIntervalUs == 8333);

        // ... a library that does not tell XeLL about its frames (xefg_cuda, XL23): XeLL counts the game's own frames,
        // the cap goes per rendered frame (6x: 20 fps); generating none, the same interval
        {
            const uint32_t shown = cap.shownPerRendered;
            // Intel's library tells XeLL about its frames: the shown cap, generating or not (6x: still 8333)
            cap.shownPerRendered = 6;
            CHECK(PlanCap(cap).minimumIntervalUs == 8333);
            cap.xellCountsShown = false;
            cap.shownPerRendered = 6;
            CHECK(PlanCap(cap).owner == CapOwner::LowLatency);
            CHECK(PlanCap(cap).minimumIntervalUs == 50000);
            cap.shownPerRendered = 1;
            CHECK(PlanCap(cap).minimumIntervalUs == 8333);
            cap.xellCountsShown = true;
            cap.shownPerRendered = shown;
        }

        // XeFG paused or off on NVIDIA/AMD: XeLL does nothing, the limiter keeps the cap and XeLL gets no interval
        cap.xefgGenerating = false;
        cap.intelGpu = false;
        CHECK(XellIdle(cap));
        CHECK(PlanCap(cap).owner == CapOwner::Limiter);
        CHECK(PlanCap(cap).minimumIntervalUs == 0);

        // ... XeFG generating again: XeLL keeps it
        cap.xefgGenerating = true;
        CHECK(!XellIdle(cap));
        CHECK(PlanCap(cap).owner == CapOwner::LowLatency);
        CHECK(PlanCap(cap).minimumIntervalUs == 8333);

        // ... with V-Sync XeLL goes by the refresh rate: the limiter keeps the cap, XeLL still lowers latency
        cap.xefgVsync = true;
        CHECK(!XellIdle(cap));
        CHECK(PlanCap(cap).owner == CapOwner::Limiter);
        CHECK(PlanCap(cap).minimumIntervalUs == 0);

        // ... V-Sync is XeLL's alone: Reflex in the split keeps its cap
        cap.method = Method::RealReflexWithXefg;
        CHECK(PlanCap(cap).owner == CapOwner::LowLatency);
        CHECK(PlanCap(cap).minimumIntervalUs == 8333);

        // ... per rendered frame (3x: 40 fps), except DLSS-G behind XeFG's API (xefg_cuda Generator=dlssg): the driver
        // multiplies the interval itself, the shown cap goes in. XeLL is not the driver: still per rendered frame there
        {
            const uint32_t shown = cap.shownPerRendered;
            cap.shownPerRendered = 3;
            CHECK(PlanCap(cap).minimumIntervalUs == 25000);
            cap.driverMultipliesCap = true;
            CHECK(PlanCap(cap).owner == CapOwner::LowLatency);
            CHECK(PlanCap(cap).minimumIntervalUs == 8333);
            cap.driverMultipliesCap = false;
            cap.shownPerRendered = shown;
        }
        cap.method = Method::XellWithXefg;
        cap.xefgVsync = false;

        // ... driverMultipliesCap belongs to Reflex alone: XeLL (not counting shown frames) and Anti-Lag 2 still
        // divide (3x, cap 120: 40 fps per rendered frame)
        {
            const uint32_t shown = cap.shownPerRendered;
            const bool countsShown = cap.xellCountsShown;
            cap.shownPerRendered = 3;
            cap.xellCountsShown = false;
            cap.driverMultipliesCap = true;
            CHECK(PlanCap(cap).owner == CapOwner::LowLatency);
            CHECK(PlanCap(cap).minimumIntervalUs == 25000);
            cap.method = Method::FakenvapiWithXefg;
            CHECK(PlanCap(cap).owner == CapOwner::LowLatency);
            CHECK(PlanCap(cap).minimumIntervalUs == 25000);
            cap.method = Method::XellWithXefg;
            cap.driverMultipliesCap = false;
            cap.xellCountsShown = countsShown;
            cap.shownPerRendered = shown;
        }

        // ... on Intel XeLL works without XeFG generating
        cap.xefgGenerating = false;
        cap.intelGpu = true;
        CHECK(!XellIdle(cap));
        CHECK(PlanCap(cap).owner == CapOwner::LowLatency);

        // ... XeFG on Intel, paused: XeLL keeps the same shown-frame interval
        CHECK(PlanCap(cap).minimumIntervalUs == 8333);

        // ... with no cap: XeLL generating keeps it with no limit
        cap.xefgGenerating = true;
        cap.capFps = 0.0f;
        CHECK(PlanCap(cap).owner == CapOwner::LowLatency);
        CHECK(PlanCap(cap).minimumIntervalUs == 0);
        cap.capFps = 120.0f;
        cap.xefgGenerating = false;

        // Only XeLL depends on XeFG: real Reflex with XeFG off is never idle
        cap.intelGpu = false;
        cap.method = Method::RealReflex;
        CHECK(!XellIdle(cap));

        // ... nor is fakenvapi's Anti-Lag 2 or LatencyFlex
        cap.method = Method::Fakenvapi;
        CHECK(!XellIdle(cap));
        CHECK(PlanCap(cap).owner == CapOwner::LowLatency);

        // fakenvapi fell back to XeLL (no Anti-Lag 2) on NVIDIA/AMD: no XeFG with it, so it does nothing
        cap.fakenvapiXell = true;
        CHECK(XellIdle(cap));
        CHECK(PlanCap(cap).owner == CapOwner::Limiter);
        CHECK(PlanCap(cap).minimumIntervalUs == 0);

        // ... on Intel it works
        cap.intelGpu = true;
        CHECK(!XellIdle(cap));
        CHECK(PlanCap(cap).owner == CapOwner::LowLatency);
        CHECK(PlanCap(cap).minimumIntervalUs == 8333);
        cap.intelGpu = false;
        cap.fakenvapiXell = false;

        // A refused SetSleepMode: the limiter keeps the cap, whatever the method
        for (auto method : { Method::RealReflex, Method::Fakenvapi, Method::XellWithXefg })
        {
            CapInputs r;
            r.running = true;
            r.capFps = 60.0f;
            r.method = method;
            r.xefgGenerating = true;
            CHECK(PlanCap(r).owner == CapOwner::LowLatency);
            CHECK(OwnerAfterCall(PlanCap(r), true) == CapOwner::Limiter);
            CHECK(OwnerAfterCall(PlanCap(r), false) == CapOwner::LowLatency);
        }

        // ... not running stays not ours
        CHECK(OwnerAfterCall(CapPlan {}, true) == CapOwner::NotOurs);

        // A new plan holds for a few presents before it is taken (frame generation's short pauses)
        {
            const CapPlan xell { CapOwner::LowLatency, 8333 };
            const CapPlan limiter { CapOwner::Limiter, 0 };
            CapSettle settle;

            // The first plan at once
            CHECK(SettleCap(settle, xell) == xell);

            // Frame generation's own pause (IFGFeature::UpdateTarget, 10 frames) is shorter than the settle window
            static_assert(kCapSettlePresents > 10);

            // A pause shorter than kCapSettlePresents: nothing changes
            for (int i = 0; i < kCapSettlePresents - 1; ++i)
                CHECK(SettleCap(settle, limiter) == xell);
            CHECK(SettleCap(settle, xell) == xell);

            // ... and the count starts over after it
            for (int i = 0; i < kCapSettlePresents - 1; ++i)
                CHECK(SettleCap(settle, limiter) == xell);

            // A pause that lasts: taken on its kCapSettlePresents-th present
            CHECK(SettleCap(settle, limiter) == limiter);

            // A changed method takes its plan at once: Reflex's per-rendered interval is wrong for XeLL
            CapSettle byMethod;
            const CapPlan reflex { CapOwner::LowLatency, 25000 };
            const CapPlan xellNow { CapOwner::LowLatency, 12500 };
            CHECK(SettleCap(byMethod, reflex, Method::RealReflexWithXefg) == reflex);
            CHECK(SettleCap(byMethod, xellNow, Method::XellWithXefg) == xellNow);
            // ... the same method still settles
            CHECK(SettleCap(byMethod, reflex, Method::XellWithXefg) == xellNow);

            // XeFG's count stepping while it generates (Dynamic MFG) is taken at once on the split; a pause still
            // settles
            {
                const auto split = Method::RealReflexWithXefg;
                const CapPlan at4x { CapOwner::LowLatency, IntervalUs(CapPerRenderedFrame(120.0f, 4)) };
                const CapPlan at3x { CapOwner::LowLatency, IntervalUs(CapPerRenderedFrame(120.0f, 3)) };
                const CapPlan paused { CapOwner::LowLatency, IntervalUs(120.0f) };
                CapSettle counted;

                CHECK(SettleCapForCount(counted, at4x, split, 4) == at4x);
                // 4X -> 3X: at once
                CHECK(SettleCapForCount(counted, at3x, split, 3) == at3x);
                // A pause (one shown per rendered) keeps 3X's interval through it
                for (int i = 0; i < kCapSettlePresents - 1; ++i)
                    CHECK(SettleCapForCount(counted, paused, split, 1) == at3x);
                CHECK(SettleCapForCount(counted, at3x, split, 3) == at3x);
                // A count step that moves the cap to another owner still settles
                const CapPlan limiterNow { CapOwner::Limiter, 0 };
                CHECK(SettleCapForCount(counted, limiterNow, split, 4) == at3x);
                // (last call: 4 shown) a step to 3 on the same method is one, on another method not
                CHECK(CountStep(counted, at3x, split, 3));
                CHECK(!CountStep(counted, at3x, Method::XellWithXefg, 3));
            }
            CHECK(SettleCap(settle, limiter) == limiter);

            // A plan that keeps changing never settles on any of its passing values
            const CapPlan other { CapOwner::LowLatency, 16667 };
            for (int i = 0; i < 20; ++i)
                CHECK(SettleCap(settle, i % 2 ? xell : other) == limiter);

            // needed 1: at once
            CHECK(SettleCap(settle, other, 1) == other);
        }

        CHECK(IntervalUs(0.0f) == 0);
        CHECK(IntervalUs(-5.0f) == 0);
        // A tiny ini cap does not overflow
        CHECK(IntervalUs(0.0000001f) == 100000000u);
        CHECK(IntervalUs(0.01f) == 100000000u);
    }

    // When our SetSleepMode goes out again (NeedsSleepModeResend)
    {
        int device = 0;
        int otherDevice = 0;

        SleepModeCall last;
        last.device = &device;
        last.minimumIntervalUs = 16667;
        last.xellGeneration = 3;

        // The first one always goes out
        CHECK(NeedsSleepModeResend(nullptr, false, last, false));

        // Nothing changed: not again, retry due or not
        SleepModeCall now = last;
        CHECK(!NeedsSleepModeResend(&last, false, now, false));
        CHECK(!NeedsSleepModeResend(&last, false, now, true));

        // Each change sends it again
        now.minimumIntervalUs = 8333;
        CHECK(NeedsSleepModeResend(&last, false, now, false));
        now = last;
        now.boost = true;
        CHECK(NeedsSleepModeResend(&last, false, now, false));
        now = last;
        now.device = &otherDevice;
        CHECK(NeedsSleepModeResend(&last, false, now, false));
        now = last;
        now.api = Api::Vulkan;
        CHECK(NeedsSleepModeResend(&last, false, now, false));

        // XeFG made a new XeLL context (same address or not): the generation tells
        now = last;
        now.xellGeneration = 4;
        CHECK(NeedsSleepModeResend(&last, false, now, false));

        // A refused one is tried again only when the retry is due
        now = last;
        CHECK(!NeedsSleepModeResend(&last, true, now, false));
        CHECK(NeedsSleepModeResend(&last, true, now, true));
    }

    // XeFG's frame IDs advance by exactly 1 per frame; our own XeLL's are followed while they do the same
    // (framegen/xefg/XeFGFrameId.h)
    {
        using xefg::FrameIds;
        using xefg::XellIdSource;

        // No XeLL ID: XeFG numbers on its own
        FrameIds<4> ids;
        CHECK(ids.For(1, 0, XellIdSource::None).id == 1);
        CHECK(ids.For(2, 0, XellIdSource::None).id == 2);
        CHECK(!ids.For(3, 0, XellIdSource::None).followsXell);

        using xefg::OwnReason;

        // Our XeLL fed from the next frame on (XeFG numbered 3 frames): followed; 18 is not 3 + 1, history reset
        auto pick = ids.For(4, 18, XellIdSource::Ours);
        CHECK(pick.id == 18 && pick.followsXell && pick.resetHistory && pick.picked);
        pick = ids.For(5, 19, XellIdSource::Ours);
        CHECK(pick.id == 19 && pick.followsXell && !pick.resetHistory);

        // The frame's ID is kept for all of its uses (tags, constants, present), whatever XeLL says by then
        pick = ids.For(5, 20, XellIdSource::Ours);
        CHECK(pick.id == 19 && !pick.picked);

        // Nothing numbered before: the first ID is followed whatever it is, no reset
        FrameIds<4> fresh;
        pick = fresh.For(1, 700, XellIdSource::Ours);
        CHECK(pick.id == 700 && pick.followsXell && !pick.resetHistory);

        // Low latency started after XeFG numbered thousands of frames: followed downwards, with a history reset
        FrameIds<4> late;
        for (uint64_t f = 1; f <= 4420; ++f)
            late.For(f, 0, XellIdSource::None);
        pick = late.For(4421, 1, XellIdSource::Ours);
        CHECK(pick.id == 1 && pick.followsXell && pick.resetHistory);
        pick = late.For(4422, 2, XellIdSource::Ours);
        CHECK(pick.id == 2 && pick.followsXell && !pick.resetHistory);

        // The game's own Reflex IDs skip (a tester's Forza Motorsport log) and come from the latest marker, not the
        // frame's: never followed, XeFG's IDs rise by 1
        FrameIds<4> game;
        const uint32_t skipping[] = { 1674, 1676, 1677, 1680, 1681, 1685, 1686, 1687, 1690, 1692 };
        uint32_t previous = 0;
        for (uint64_t f = 0; f < sizeof(skipping) / sizeof(skipping[0]); ++f)
        {
            pick = game.For(f + 1, skipping[f], XellIdSource::Game);
            CHECK(!pick.followsXell && !pick.resetHistory && pick.ownReason == OwnReason::None);
            CHECK(previous == 0 || pick.id == previous + 1);
            previous = pick.id;
        }

        // Our own XeLL IDs rise by 1: followed, XeFG's IDs are XeLL's
        FrameIds<4> ours;
        for (uint32_t id = 300; id < 320; ++id)
        {
            pick = ours.For(id, id, XellIdSource::Ours);
            CHECK(pick.id == id && pick.followsXell && !pick.resetHistory && pick.ownReason == OwnReason::None);
        }

        // One jump in our XeLL IDs mid-stream: XeFG's IDs go on by 1, the jump is flagged, no re-sync without a reset
        FrameIds<4> jump;
        for (uint32_t id = 10; id <= 12; ++id)
            jump.For(id, id, XellIdSource::Ours);
        pick = jump.For(13, 20, XellIdSource::Ours);
        CHECK(pick.id == 13 && !pick.followsXell && pick.ownReason == OwnReason::Jumped);
        for (uint32_t id = 21; id < 30; ++id)
        {
            pick = jump.For(id - 7, id, XellIdSource::Ours);
            CHECK(pick.id == id - 7 && !pick.followsXell && !pick.resetHistory);
        }

        // Pause and resume: FG paused for 10 frames while our XeLL ran on (no For call); the first frame after is
        // followed again with a history reset, then without
        FrameIds<4> pause;
        for (uint32_t id = 100; id <= 105; ++id)
            pause.For(id, id, XellIdSource::Ours);
        pick = pause.For(116, 116, XellIdSource::Ours);
        CHECK(pick.id == 116 && pick.followsXell && pick.resetHistory);
        pick = pause.For(117, 117, XellIdSource::Ours);
        CHECK(pick.id == 117 && pick.followsXell && !pick.resetHistory);

        // A frame with a history reset (the game's reset flag) re-syncs too, not one without
        FrameIds<4> reset;
        for (uint32_t id = 50; id <= 52; ++id)
            reset.For(id, id, XellIdSource::Ours);
        pick = reset.For(53, 60, XellIdSource::Ours, false);
        CHECK(pick.id == 53 && !pick.followsXell && pick.ownReason == OwnReason::Jumped);
        pick = reset.For(54, 61, XellIdSource::Ours, true);
        CHECK(pick.id == 61 && pick.followsXell && pick.resetHistory);
        // With resets switched off (FGSkipReset) nothing is re-synced
        pick = reset.For(55, 70, XellIdSource::Ours, true, false);
        CHECK(pick.id == 62 && !pick.followsXell && !pick.resetHistory);

        // The game takes XeLL over from ours: XeFG's IDs go on by 1, and a later start of ours is followed again
        FrameIds<4> handover;
        handover.For(1, 5, XellIdSource::Ours);
        handover.For(2, 6, XellIdSource::Ours);
        pick = handover.For(3, 900, XellIdSource::Game);
        CHECK(pick.id == 7 && !pick.followsXell && pick.ownReason == OwnReason::None);
        pick = handover.For(4, 2, XellIdSource::Ours);
        CHECK(pick.id == 2 && pick.followsXell && pick.resetHistory);

        // Our XeLL restarts its IDs (NBA 2K27: 241 -> 2): followed, with a history reset
        FrameIds<4> restart;
        for (uint32_t id = 230; id <= 241; ++id)
            restart.For(id, id, XellIdSource::Ours);
        pick = restart.For(242, 2, XellIdSource::Ours);
        CHECK(pick.id == 2 && pick.followsXell && pick.resetHistory);
        pick = restart.For(243, 3, XellIdSource::Ours);
        CHECK(pick.id == 3 && pick.followsXell && !pick.resetHistory);

        // XeLL's ID does not advance (a frame started twice): XeFG's ID still rises, with a reason. Frame 3: XeLL's 51 is XeFG's
        // last, so it is not usable either (no silent follow)
        FrameIds<4> twice;
        CHECK(twice.For(1, 50, XellIdSource::Ours).id == 50);
        pick = twice.For(2, 50, XellIdSource::Ours);
        CHECK(pick.id == 51 && !pick.followsXell && !pick.resetHistory && pick.ownReason == OwnReason::Unusable);
        pick = twice.For(3, 51, XellIdSource::Ours);
        CHECK(pick.id == 52 && !pick.followsXell && !pick.resetHistory && pick.ownReason == OwnReason::Unusable);

        // An ID XeFG still holds after a gap (a pause): not followed, flagged
        FrameIds<4> held;
        for (uint32_t id = 40; id <= 43; ++id)
            held.For(id, id, XellIdSource::Ours);
        pick = held.For(50, 42, XellIdSource::Ours);
        CHECK(pick.id == 44 && !pick.followsXell && pick.ownReason == OwnReason::Unusable);

        // An XeLL ID that stays the same for a long time is never followed again (no reset every few frames)
        FrameIds<4> stale;
        stale.For(1, 70, XellIdSource::Ours);
        int resets = 0;
        int follows = 0;
        uint32_t last = 70;
        for (uint64_t f = 2; f < 40; ++f)
        {
            pick = stale.For(f, 70, XellIdSource::Ours);
            resets += pick.resetHistory ? 1 : 0;
            follows += pick.followsXell ? 1 : 0;
            CHECK(pick.id == last + 1 && pick.ownReason == OwnReason::Unusable);
            last = pick.id;
        }
        CHECK(resets == 0 && follows == 0);

        // XeFG's own counter wraps past 0xFFFFFFFF without handing out 0
        FrameIds<4> wrap;
        wrap.For(1, 0xFFFFFFFEu, XellIdSource::Ours);
        wrap.For(2, 0xFFFFFFFFu, XellIdSource::Ours);
        pick = wrap.For(3, 0, XellIdSource::None);
        CHECK(pick.id == 1 && !pick.followsXell);
        CHECK(wrap.For(4, 0, XellIdSource::None).id == 2);

        // The history holds the newest kSize IDs only
        xefg::FrameIdHistory history;
        for (uint32_t id = 1; id <= xefg::FrameIdHistory::kSize + 1; ++id)
            history.Add(id);
        CHECK(!history.Contains(1));
        CHECK(history.Contains(2));
        CHECK(history.Contains(xefg::FrameIdHistory::kSize + 1));
    }

    // XeFG on the Vulkan present bridge, and the sleep modes (native/XefgReflexRule.h)
    {
        using namespace native::xefgreflex;

        // XeFG output and the bridge's device; never dxvk without the Vulkan bridge
        CHECK(ShouldForward(true, true, false));
        CHECK(!ShouldForward(false, true, false)); // another output: the driver keeps the game's Reflex
        CHECK(!ShouldForward(true, false, false)); // no bridge device (plain Vulkan, bridge down or being let go of)
        CHECK(!ShouldForward(true, true, true));   // dxvk with no Vulkan bridge: the D3D hooks and dxvk own it
        CHECK(!ShouldForward(true, false, true));

        // The sleep mode: XeLL keeps the game's values (the cap replaces the interval when one is set)
        SleepMode game;
        game.lowLatency = true;
        game.boost = true;
        game.minimumIntervalUs = 16667;
        game.useMarkersToOptimize = true;

        auto xell = SleepModeForOutput(game, 0, true, false);
        CHECK(xell.lowLatency && xell.boost && xell.minimumIntervalUs == 16667 && xell.useMarkersToOptimize);
        xell = SleepModeForOutput(game, 8333, true, false);
        CHECK(xell.lowLatency && xell.minimumIntervalUs == 8333);

        // The rule the D3D hooks share: XeFG is the output -> low latency on whatever the game says, the cap in place
        // of the game's interval; only fakenvapi's Force Disable turns it off; no XeFG -> the game's mode as it is
        SleepMode gameOff;
        gameOff.minimumIntervalUs = 16667;
        auto out = SleepModeForOutput(gameOff, 0, true, false);
        CHECK(out.lowLatency && out.minimumIntervalUs == 16667);
        out = SleepModeForOutput(gameOff, 13333, true, false);
        CHECK(out.lowLatency && out.minimumIntervalUs == 13333);
        out = SleepModeForOutput(gameOff, 13333, true, true);
        CHECK(!out.lowLatency && out.minimumIntervalUs == 13333);
        out = SleepModeForOutput(gameOff, 0, false, false);
        CHECK(!out.lowLatency && out.minimumIntervalUs == 16667);
        SleepMode gameOn;
        gameOn.lowLatency = gameOn.boost = true;
        out = SleepModeForOutput(gameOn, 0, false, true);
        CHECK(out.lowLatency && out.boost); // Force Disable is fakenvapi's own business without XeFG

        // Who keeps the cap the game's Reflex hands XeLL: XeLL while it can, OptiScaler's limiter otherwise
        CHECK(XellHoldsCap(false, false, true, false));   // NVIDIA/AMD, XeFG generating
        CHECK(!XellHoldsCap(false, false, false, false)); // NVIDIA/AMD, XeFG paused or off: XeLL idle
        CHECK(XellHoldsCap(false, true, false, false));   // Intel: XeLL works on its own
        CHECK(XellHoldsCap(false, true, true, false));
        CHECK(!XellHoldsCap(true, true, true, false));  // Force Disable
        CHECK(!XellHoldsCap(true, false, true, false)); // Force Disable on NVIDIA/AMD while XeFG generates
        CHECK(!XellHoldsCap(true, true, false, false));
        CHECK(!XellHoldsCap(false, false, true, true)); // V-Sync: XeLL goes by the refresh rate
        CHECK(!XellHoldsCap(false, true, true, true));

        // Reflex off in the game's menu: XeLL still runs (its sleeps carry the cap) and gets the cap
        SleepMode off;
        xell = SleepModeForOutput(off, 8333, true, false);
        CHECK(xell.lowLatency && xell.minimumIntervalUs == 8333);

        // The menu: the game's Reflex stands us aside either way, and the text says where it goes
        CHECK(std::strcmp(DecisionText(Decision::GameRunsReflex), "Use the game's own Reflex settings") == 0);
        CHECK(std::strcmp(DecisionText(Decision::GameRunsReflex, true), "The game's Reflex feeds XeFG's XeLL") == 0);
        CHECK(std::strcmp(DecisionText(Decision::GameRunsReflex, true, true),
                          "The game's Reflex feeds XeFG's XeLL, low latency off by Force Disable") == 0);
        CHECK(std::strcmp(DecisionText(Decision::GameRunsReflex, false, true), "Use the game's own Reflex settings") ==
              0);
        CHECK(std::strcmp(DecisionText(Decision::SettingOff, true), DecisionText(Decision::SettingOff)) == 0);
        CHECK(std::strcmp(StatusLabel(Decision::GameRunsReflex), "Standing aside") == 0);

        // The decision itself is unchanged: the game calls Reflex, so we do not
        Inputs x;
        x.xefgRunning = true;
        x.gameCallsReflex = true;
        CHECK(Decide(x) == Decision::GameRunsReflex);
    }

    // Framerate page: the Input choice (Auto / Game / OptiScaler / Off) and what each means
    {
        // Mapping to the setting our own low latency uses, and whether the game's Reflex is sent low latency off
        CHECK(RuleForInput(Input::Auto).setting == Setting::Auto && !RuleForInput(Input::Auto).blockGameLowLatency);
        CHECK(RuleForInput(Input::Game).setting == Setting::Off && !RuleForInput(Input::Game).blockGameLowLatency);
        CHECK(RuleForInput(Input::OptiScaler).setting == Setting::On &&
              !RuleForInput(Input::OptiScaler).blockGameLowLatency);
        CHECK(RuleForInput(Input::Off).setting == Setting::Off && RuleForInput(Input::Off).blockGameLowLatency);

        // Off sends the game's Reflex low latency off, unless DLSS frame generation needs Reflex
        CHECK(BlocksGameLowLatency(Input::Off, false));
        CHECK(!BlocksGameLowLatency(Input::Off, true));
        CHECK(!BlocksGameLowLatency(Input::Game, false));
        CHECK(!BlocksGameLowLatency(Input::Auto, false));
        CHECK(!BlocksGameLowLatency(Input::OptiScaler, false));

        // Auto is Game with the virtual upscaler; a hand-picked Input stays what it is
        CHECK(EffectiveInput(Input::Auto, true) == Input::Game);
        CHECK(EffectiveInput(Input::Auto, false) == Input::Auto);
        for (auto input : { Input::Game, Input::OptiScaler, Input::Off })
            CHECK(EffectiveInput(input, true) == input && EffectiveInput(input, false) == input);

        // Ini words: the new key, and the old [DlssNr] NativeLowLatency one
        for (auto input : { Input::Auto, Input::Game, Input::OptiScaler, Input::Off })
        {
            Input back = Input::Auto;
            CHECK(InputFromIniText(InputIniText(input), &back) && back == input);
        }

        Input parsed = Input::Auto;
        CHECK(!InputFromIniText("", &parsed));
        CHECK(!InputFromIniText(nullptr, &parsed));
        CHECK(!InputFromIniText("true", &parsed));
        CHECK(!InputFromIniText("banana", &parsed));

        CHECK(InputFromLegacyText("auto", &parsed) && parsed == Input::Auto);
        CHECK(InputFromLegacyText("true", &parsed) && parsed == Input::OptiScaler);
        CHECK(InputFromLegacyText("false", &parsed) && parsed == Input::Game);
        CHECK(!InputFromLegacyText("game", &parsed));
        CHECK(!InputFromLegacyText(nullptr, &parsed));

        // The old key an older DLL reads: Off and Game both write false (neither makes calls of ours)
        CHECK(std::strcmp(LegacySettingText(Input::Auto), "auto") == 0);
        CHECK(std::strcmp(LegacySettingText(Input::OptiScaler), "true") == 0);
        CHECK(std::strcmp(LegacySettingText(Input::Game), "false") == 0);
        CHECK(std::strcmp(LegacySettingText(Input::Off), "false") == 0);

        // New key wins; a bad new key falls back to the old one; neither: Auto
        CHECK(InputFromIniValues(true, "off", "true") == Input::Off);
        CHECK(InputFromIniValues(true, "game", nullptr) == Input::Game);
        CHECK(InputFromIniValues(false, nullptr, "true") == Input::OptiScaler);
        CHECK(InputFromIniValues(false, nullptr, "false") == Input::Game);
        CHECK(InputFromIniValues(true, "banana", "false") == Input::Game);
        CHECK(InputFromIniValues(false, nullptr, nullptr) == Input::Auto);
        CHECK(InputFromIniValues(true, "", "") == Input::Auto);
        // An explicit auto is a choice: the old key does not override it
        CHECK(InputFromIniValues(true, "auto", "false") == Input::Auto);
        CHECK(InputFromIniValues(true, "auto", "true") == Input::Auto);
        // The key is there but holds nothing readable: the old key still counts
        CHECK(InputFromIniValues(true, nullptr, "false") == Input::Game);

        // Decide: Game stands us down with its own answer, Off with its own, both before anything else
        Inputs g;
        g.f5lowRunning = true;
        g.setting = RuleForInput(Input::Game).setting;
        g.blockGameLowLatency = RuleForInput(Input::Game).blockGameLowLatency;
        CHECK(Decide(g) == Decision::SettingOff);

        Inputs o;
        o.f5lowRunning = true;
        o.gameCallsReflex = true;
        o.setting = RuleForInput(Input::Off).setting;
        o.blockGameLowLatency = RuleForInput(Input::Off).blockGameLowLatency;
        CHECK(Decide(o) == Decision::LowLatencyOff);

        // Off kept back by DLSS frame generation: no block, the plain switched-off answer
        o.blockGameLowLatency = BlocksGameLowLatency(Input::Off, true);
        CHECK(Decide(o) == Decision::SettingOff);

        CHECK(std::strcmp(StatusLabel(Decision::LowLatencyOff), "Off") == 0);
        CHECK(DecisionText(Decision::LowLatencyOff)[0] != '\0');
        CHECK(std::strcmp(DecisionText(Decision::LowLatencyOff), DecisionText(Decision::SettingOff)) != 0);

        // OptiScaler runs in any game without Reflex; Auto needs something to run for
        Inputs w;
        w.setting = RuleForInput(Input::OptiScaler).setting;
        CHECK(Decide(w) == Decision::Run);
        w.setting = RuleForInput(Input::Auto).setting;
        CHECK(Decide(w) == Decision::NoF5Low);
    }

    // Framerate page: "Active: <input> -> <output>" names who makes the calls, from the same Decision the code uses
    {
        CHECK(std::strcmp(ActiveInputText(Decision::Run, false), "OptiScaler") == 0);
        CHECK(std::strcmp(ActiveInputText(Decision::LowLatencyOff, true), "Off") == 0);
        CHECK(std::strcmp(ActiveInputText(Decision::FrameGenerationOwnsReflex, true), "Frame generation") == 0);

        // The game's Reflex is what runs when it makes calls, whatever the decision says about us
        CHECK(std::strcmp(ActiveInputText(Decision::GameRunsReflex, true), "Game") == 0);
        CHECK(std::strcmp(ActiveInputText(Decision::NoF5Low, true), "Game") == 0);
        CHECK(std::strcmp(ActiveInputText(Decision::SettingOff, true), "Game") == 0);

        // Nobody makes calls
        CHECK(std::strcmp(ActiveInputText(Decision::NoF5Low, false), "None") == 0);
        CHECK(std::strcmp(ActiveInputText(Decision::SettingOff, false), "None") == 0);

        // The game runs XeLL of its own (no Reflex calls): that is who lowers latency, not nothing
        CHECK(std::strcmp(ActiveInputText(Decision::GameRunsXell, false, true, false), "Game") == 0);
        // DLSS FG runs Reflex whatever the Input says (Game, or Off kept for it)
        CHECK(std::strcmp(ActiveInputText(Decision::SettingOff, false, false, true), "Frame generation") == 0);
        CHECK(std::strcmp(ActiveInputText(Decision::NoF5Low, false, false, true), "Frame generation") == 0);
        // Input Off with nothing needing Reflex is still Off
        CHECK(std::strcmp(ActiveInputText(Decision::LowLatencyOff, false, false, false), "Off") == 0);
        // The game's own Reflex calls still count first
        CHECK(std::strcmp(ActiveInputText(Decision::SettingOff, true, false, true), "Game") == 0);

        // Input Game (or Auto acting as Game with NR + frame generation) and the game making its own Reflex calls: the
        // menu stands aside for it ("Active: Game -> XeLL" next to "[Off]" was read as two notices that disagree)
        CHECK(ShownDecision(Decision::SettingOff, Input::Game, true) == Decision::GameRunsReflex);
        CHECK(ShownDecision(Decision::SettingOff, Input::Auto, true) == Decision::GameRunsReflex);
        CHECK(std::strcmp(StatusLabel(ShownDecision(Decision::SettingOff, Input::Auto, true)), "Standing aside") == 0);
        CHECK(std::strcmp(DecisionText(ShownDecision(Decision::SettingOff, Input::Auto, true), true),
                          "The game's Reflex feeds XeFG's XeLL") == 0);
        // No game Reflex: still off, with the reason that says why
        CHECK(ShownDecision(Decision::SettingOff, Input::Game, false) == Decision::SettingOff);
        // Input Off keeps its own wording, also when DLSS FG keeps the game's Reflex on (SettingOff then)
        CHECK(ShownDecision(Decision::SettingOff, Input::Off, true) == Decision::SettingOff);
        CHECK(ShownDecision(Decision::LowLatencyOff, Input::Off, true) == Decision::LowLatencyOff);
        // Every other decision is shown as it is
        CHECK(ShownDecision(Decision::Run, Input::OptiScaler, true) == Decision::Run);
        CHECK(ShownDecision(Decision::NoF5Low, Input::Auto, true) == Decision::NoF5Low);
        CHECK(std::strcmp(ActiveInputText(ShownDecision(Decision::SettingOff, Input::Auto, true), true), "Game") == 0);

        // The reason for Input Game, or Off kept by DLSS FG, does not claim the Input is Game
        CHECK(std::strstr(DecisionText(Decision::SettingOff), "Input is Game:") == nullptr);
        CHECK(DecisionText(Decision::SettingOff)[0] != '\0');
    }

    // Framerate page: the Output choice
    {
        OutputInputs real; // NVIDIA, real nvapi, no XeFG
        real.nvidiaGpu = true;
        real.driverReflex = true; // as CurrentOutputInputs sets it
        CHECK(OutputAvailability(Output::Auto, real).available);
        CHECK(OutputAvailability(Output::XeLL, real).available);
        CHECK(!OutputAvailability(Output::LatencyFlex, real).available);
        CHECK(OutputAvailability(Output::LatencyFlex, real).why[0] != '\0');

        OutputInputs fake;
        fake.fakenvapiMain = true;
        CHECK(OutputAvailability(Output::LatencyFlex, fake).available);

        // XeFG always uses XeLL: Auto and LatencyFlex are out, XeLL is the locked answer
        OutputInputs xefg;
        xefg.fakenvapiMain = true;
        xefg.xefgOutput = true;
        CHECK(!OutputAvailability(Output::LatencyFlex, xefg).available);
        CHECK(!OutputAvailability(Output::Auto, xefg).available);
        CHECK(OutputAvailability(Output::XeLL, xefg).available);
        CHECK(OutputLocked(xefg));
        CHECK(!OutputLocked(real));
        CHECK(OutputShown(xefg, false, false) == Output::XeLL);

        // What the combo shows from the two fakenvapi flags
        CHECK(OutputShown(real, false, false) == Output::Auto);
        CHECK(OutputShown(real, true, false) == Output::XeLL);
        CHECK(OutputShown(fake, false, true) == Output::LatencyFlex);
        // LatencyFlex forced in the ini on the real nvapi (fakenvapi is not the nvapi): that entry is not offered, so Auto
        // shows, not a greyed LatencyFlex
        CHECK(OutputShown(real, false, true) == Output::Auto);
        CHECK(OutputShown(fake, true, true) == Output::XeLL);

        // Frame cap only: offered without XeFG wherever the driver's Reflex is not the output
        CHECK(!OutputAvailability(Output::FrameCapOnly, real).available);
        CHECK(OutputAvailability(Output::FrameCapOnly, real).why[0] != '\0');
        CHECK(OutputAvailability(Output::FrameCapOnly, fake).available);
        OutputInputs amdReal; // an AMD card on its own nvapi: InputCommon's Anti-Lag 2, or Frame cap only
        amdReal.amdGpu = true;
        CHECK(OutputAvailability(Output::FrameCapOnly, amdReal).available);
        CHECK(!OutputAvailability(Output::FrameCapOnly, xefg).available);
        CHECK(OutputShown(fake, false, false, true) == Output::FrameCapOnly);
        CHECK(OutputShown(fake, false, true, true) == Output::FrameCapOnly); // ahead of LatencyFlex, as PlanOutput
        CHECK(OutputShown(fake, true, false, true) == Output::XeLL);         // Force XeLL takes the slot
        CHECK(OutputShown(real, false, false, true) == Output::Auto);        // set in the ini on NVIDIA Reflex
        CHECK(OutputShown(xefg, false, false, true) == Output::XeLL);

        // NVIDIA with fakenvapi as the nvapi: offered; but where InputCommon runs the driver's Reflex anyway (a Vulkan
        // device with VK_NV_low_latency2) neither forced entry is, and Auto shows what runs
        OutputInputs nvFake;
        nvFake.nvidiaGpu = true;
        nvFake.fakenvapiMain = true;
        CHECK(OutputAvailability(Output::FrameCapOnly, nvFake).available);
        CHECK(OutputAvailability(Output::LatencyFlex, nvFake).available);
        nvFake.driverReflex = true;
        CHECK(!OutputAvailability(Output::FrameCapOnly, nvFake).available);
        CHECK(!OutputAvailability(Output::LatencyFlex, nvFake).available);
        CHECK(OutputShown(nvFake, false, false, true) == Output::Auto);
        CHECK(OutputShown(nvFake, false, true, false) == Output::Auto);

        // The menu hides entries whose reason holds for the whole session and greys only the passing ones. Where the
        // driver's Reflex runs, the Auto entry is that method by name, and it is what OutputShown selects there.
        CHECK(OutputAvailability(Output::LatencyFlex, real).hidden);
        CHECK(OutputAvailability(Output::FrameCapOnly, real).hidden);
        CHECK(OutputAvailability(Output::FrameCapOnly, nvFake).hidden);
        CHECK(!OutputAvailability(Output::XeLL, real).hidden && !OutputAvailability(Output::Auto, real).hidden);
        OutputInputs amdNoFake; // LatencyFlex needs fakenvapi, which the session does not get later
        amdNoFake.amdGpu = true;
        CHECK(OutputAvailability(Output::LatencyFlex, amdNoFake).hidden);
        CHECK(!OutputAvailability(Output::FrameCapOnly, amdNoFake).hidden);
        CHECK(AutoEntryName(real) != nullptr && std::strcmp(AutoEntryName(real), "NVIDIA Reflex") == 0);
        CHECK(AutoEntryName(nvFake) != nullptr);
        CHECK(AutoEntryName(fake) == nullptr && AutoEntryName(amdReal) == nullptr);
        for (const bool frameCapOnly : { false, true })
            for (const bool forceLatencyFlex : { false, true })
                CHECK(OutputShown(real, false, forceLatencyFlex, frameCapOnly) == Output::Auto);

        // Boost: only with real Reflex as the output, and only while OptiScaler makes the calls
        CHECK(BoostAvailable(Input::Auto, real, false));
        CHECK(BoostAvailable(Input::OptiScaler, real, false));
        CHECK(!BoostAvailable(Input::Game, real, false));
        CHECK(!BoostAvailable(Input::Off, real, false));
        CHECK(!BoostAvailable(Input::Auto, fake, false));
        CHECK(!BoostAvailable(Input::Auto, xefg, false));
        CHECK(!BoostAvailable(Input::Auto, real, true)); // Force XeLL
    }

    // Off: the game's sleep mode goes out with low latency off, no boost, no interval (OptiScaler's limiter keeps the cap)
    {
        using namespace native::xefgreflex;

        SleepMode game;
        game.lowLatency = true;
        game.boost = true;
        game.minimumIntervalUs = 8333;
        game.useMarkersToOptimize = true;

        for (const bool xefgOutput : { false, true })
        {
            const auto off = SleepModeForOutput(game, 16666, xefgOutput, false, true);
            CHECK(!off.lowLatency && !off.boost && off.minimumIntervalUs == 0);
            CHECK(off.useMarkersToOptimize); // not ours to change
        }

        // Not blocked: as before (cap in, XeFG keeps low latency on)
        const auto plain = SleepModeForOutput(game, 16666, false, false, false);
        CHECK(plain.lowLatency && plain.boost && plain.minimumIntervalUs == 16666);
        const auto xefgOn = SleepModeForOutput(game, 0, true, false, false);
        CHECK(xefgOn.lowLatency && xefgOn.minimumIntervalUs == 8333);

        // Off and Force Disable together: still off
        CHECK(!SleepModeForOutput(game, 0, true, true, true).lowLatency);
    }

    // XeFG with NVIDIA Reflex or Anti-Lag 2 as the low latency Output ("split"): XeLL keeps its context and the frame
    // markers with its low latency off, the driver's Reflex (or Anti-Lag 2 / Anti-Lag Vk through fakenvapi) lowers latency
    {
        using namespace native::xefgreflex;

        // [XeFG] LowLatencyOutput
        XefgLatency lat = XefgLatency::XeLL;
        CHECK(XefgLatencyFromIniText("reflex", &lat) && lat == XefgLatency::Reflex);
        CHECK(XefgLatencyFromIniText("antilag2", &lat) && lat == XefgLatency::AntiLag2);
        CHECK(XefgLatencyFromIniText("xell", &lat) && lat == XefgLatency::XeLL);
        lat = XefgLatency::AntiLag2;
        CHECK(!XefgLatencyFromIniText("bogus", &lat) && lat == XefgLatency::AntiLag2); // left alone
        CHECK(!XefgLatencyFromIniText(nullptr, &lat) && lat == XefgLatency::AntiLag2);
        CHECK(!XefgLatencyFromIniText("", &lat));
        for (const auto each : { XefgLatency::XeLL, XefgLatency::Reflex, XefgLatency::AntiLag2 })
        {
            XefgLatency back = XefgLatency::XeLL;
            CHECK(XefgLatencyFromIniText(XefgLatencyIniText(each), &back) && back == each);
        }

        // Which entries a setup has
        OutputInputs nvidia; // real nvapi, XeFG, Reflex asked for
        nvidia.nvidiaGpu = true;
        nvidia.xefgOutput = true;
        nvidia.xefgLatency = XefgLatency::Reflex;
        OutputInputs amd; // fakenvapi on an AMD card
        amd.xefgOutput = true;
        amd.fakenvapiMain = true;
        amd.amdGpu = true;
        amd.xefgLatency = XefgLatency::AntiLag2;
        OutputInputs intel; // fakenvapi, no AMD card: only XeLL
        intel.xefgOutput = true;
        intel.fakenvapiMain = true;
        intel.xefgLatency = XefgLatency::Reflex;

        CHECK(XefgLatencyAvailable(XefgLatency::XeLL, nvidia) && XefgLatencyAvailable(XefgLatency::XeLL, intel));
        CHECK(XefgLatencyAvailable(XefgLatency::Reflex, nvidia));
        CHECK(!XefgLatencyAvailable(XefgLatency::AntiLag2, nvidia));
        CHECK(!XefgLatencyAvailable(XefgLatency::Reflex, amd));
        CHECK(XefgLatencyAvailable(XefgLatency::AntiLag2, amd));
        CHECK(!XefgLatencyAvailable(XefgLatency::Reflex, intel) && !XefgLatencyAvailable(XefgLatency::AntiLag2, intel));

        CHECK(OutputAvailability(Output::XeLL, nvidia).available);
        CHECK(OutputAvailability(Output::Reflex, nvidia).available);
        CHECK(!OutputAvailability(Output::AntiLag2, nvidia).available);
        CHECK(OutputAvailability(Output::AntiLag2, nvidia).why[0] != '\0');
        CHECK(OutputAvailability(Output::AntiLag2, amd).available);
        CHECK(!OutputAvailability(Output::Reflex, amd).available && OutputAvailability(Output::Reflex, amd).why[0] != '\0');
        CHECK(!OutputAvailability(Output::Reflex, intel).available && !OutputAvailability(Output::AntiLag2, intel).available);
        CHECK(!OutputAvailability(Output::Auto, nvidia).available && !OutputAvailability(Output::LatencyFlex, nvidia).available);

        // Reflex is the driver's and needs an NVIDIA card: real nvapi on an AMD or Intel card (no fakenvapi) has none
        for (const bool amdCard : { true, false })
        {
            OutputInputs other; // real nvapi file present, but the card is AMD or Intel
            other.xefgOutput = true;
            other.amdGpu = amdCard;
            other.xefgLatency = XefgLatency::Reflex;
            CHECK(!XefgLatencyAvailable(XefgLatency::Reflex, other));
            CHECK(!OutputAvailability(Output::Reflex, other).available);
            CHECK(OutputShown(other, false, false) == Output::XeLL);
            CHECK(EffectiveXefgLatency(other, false, false) == XefgLatency::XeLL);
            CHECK(!BoostAvailable(Input::Auto, other, false));
            CHECK(OutputLocked(other)); // no Reflex, and no fakenvapi for Anti-Lag 2: XeLL is the one choice
            other.xefgOutput = false; // without XeFG the same card still has no Reflex boost
            CHECK(!BoostAvailable(Input::Auto, other, false));
        }

        // Anti-Lag 2 that failed to start (a second context fakenvapi could not make): not available, with its own reason
        {
            OutputInputs failed = amd;
            failed.antiLag2Failed = true;
            CHECK(!XefgLatencyAvailable(XefgLatency::AntiLag2, failed));
            CHECK(!OutputAvailability(Output::AntiLag2, failed).available);
            CHECK(std::strstr(OutputAvailability(Output::AntiLag2, failed).why, "could not start") != nullptr);
            CHECK(!OutputAvailability(Output::AntiLag2, failed).hidden); // can pass: greyed, not hidden
            CHECK(OutputShown(failed, false, false) == Output::XeLL);
            CHECK(EffectiveXefgLatency(failed, false, false) == XefgLatency::XeLL);
            CHECK(OutputLocked(failed));
            // ... the other reasons keep their own words
            CHECK(std::strstr(OutputAvailability(Output::AntiLag2, nvidia).why, "could not start") == nullptr);
        }

        // Reflex that failed to start (a Vulkan device with neither VK_NV_low_latency2 nor NVIDIA's NvAPI Vulkan
        // Reflex): the same, XeFG goes back to XeLL
        {
            OutputInputs failed = nvidia;
            failed.xefgLatency = XefgLatency::Reflex;
            failed.reflexFailed = true;
            CHECK(!XefgLatencyAvailable(XefgLatency::Reflex, failed));
            CHECK(!OutputAvailability(Output::Reflex, failed).available);
            CHECK(std::strstr(OutputAvailability(Output::Reflex, failed).why, "could not start") != nullptr);
            CHECK(!OutputAvailability(Output::Reflex, failed).hidden); // can pass: greyed, not hidden
            CHECK(OutputShown(failed, false, false) == Output::XeLL);
            CHECK(EffectiveXefgLatency(failed, false, false) == XefgLatency::XeLL);
            CHECK(OutputLocked(failed));
            CHECK(std::strstr(OutputAvailability(Output::Reflex, amd).why, "could not start") == nullptr);
        }

        // Without XeFG the two are not offered
        OutputInputs plain;
        CHECK(!OutputAvailability(Output::Reflex, plain).available && OutputAvailability(Output::Reflex, plain).why[0] != '\0');
        CHECK(!OutputAvailability(Output::AntiLag2, plain).available);

        // Locked only when XeLL is the one choice
        CHECK(!OutputLocked(nvidia) && !OutputLocked(amd));
        CHECK(OutputLocked(intel));

        // What the combo shows
        CHECK(OutputShown(nvidia, false, false) == Output::Reflex);
        CHECK(OutputShown(amd, false, false) == Output::AntiLag2);
        CHECK(OutputShown(intel, false, false) == Output::XeLL); // asked for Reflex, not available
        nvidia.xefgLatency = XefgLatency::XeLL;
        CHECK(OutputShown(nvidia, false, false) == Output::XeLL);
        nvidia.xefgLatency = XefgLatency::AntiLag2; // not available on NVIDIA
        CHECK(OutputShown(nvidia, false, false) == Output::XeLL);
        nvidia.xefgLatency = XefgLatency::Reflex;

        // Boost is Reflex's: with XeFG only when Reflex is the output
        CHECK(BoostAvailable(Input::Auto, nvidia, false));
        CHECK(BoostAvailable(Input::OptiScaler, nvidia, false));
        CHECK(!BoostAvailable(Input::Game, nvidia, false));
        CHECK(!BoostAvailable(Input::Off, nvidia, false));
        CHECK(!BoostAvailable(Input::Auto, amd, false));
        CHECK(!BoostAvailable(Input::Auto, intel, false));
        nvidia.xefgLatency = XefgLatency::XeLL;
        CHECK(!BoostAvailable(Input::Auto, nvidia, false));
        nvidia.xefgLatency = XefgLatency::Reflex;
        // Force Reflex set in the fakenvapi settings keeps XeLL (nothing sends Boost then), whatever the choice shows
        CHECK(!BoostAvailable(Input::Auto, nvidia, false, true));
        CHECK(BoostAvailable(Input::Auto, nvidia, false, false));

        // The side in force: the choice if the setup has it, XeLL for everything else. Input Off and Force Disable win.
        CHECK(EffectiveXefgLatency(nvidia, false, false) == XefgLatency::Reflex);
        CHECK(EffectiveXefgLatency(amd, false, false) == XefgLatency::AntiLag2);
        CHECK(EffectiveXefgLatency(intel, false, false) == XefgLatency::XeLL);
        CHECK(EffectiveXefgLatency(nvidia, true, false) == XefgLatency::XeLL); // Input Off
        CHECK(EffectiveXefgLatency(nvidia, false, true) == XefgLatency::XeLL); // Force Disable
        CHECK(EffectiveXefgLatency(amd, true, true) == XefgLatency::XeLL);
        OutputInputs notXefg = nvidia;
        notXefg.xefgOutput = false;
        CHECK(EffectiveXefgLatency(notXefg, false, false) == XefgLatency::XeLL);
        OutputInputs xellChosen = nvidia;
        xellChosen.xefgLatency = XefgLatency::XeLL;
        CHECK(EffectiveXefgLatency(xellChosen, false, false) == XefgLatency::XeLL);

        // The two sleep modes
        SleepMode game;
        game.lowLatency = true;
        game.boost = true;
        game.minimumIntervalUs = 8333;
        game.useMarkersToOptimize = true;

        const auto xellMode = XellSleepModeInSplit(game);
        CHECK(!xellMode.lowLatency && !xellMode.boost && xellMode.minimumIntervalUs == 0);
        CHECK(xellMode.useMarkersToOptimize);

        // The driver's: the game's own (Reflex off in its menu stays off), the cap in place of its interval
        auto driverMode = DriverSleepModeInSplit(game, 0);
        CHECK(driverMode.lowLatency && driverMode.boost && driverMode.minimumIntervalUs == 8333);
        driverMode = DriverSleepModeInSplit(game, 16667);
        CHECK(driverMode.lowLatency && driverMode.minimumIntervalUs == 16667);
        SleepMode gameOff;
        driverMode = DriverSleepModeInSplit(gameOff, 0);
        CHECK(!driverMode.lowLatency && driverMode.minimumIntervalUs == 0);

        // Methods: the three that carry XeFG's frame IDs, the two that leave the sleeping to the driver or Anti-Lag 2
        CHECK(MethodWithXefg(XefgLatency::XeLL) == Method::XellWithXefg);
        CHECK(MethodWithXefg(XefgLatency::Reflex) == Method::RealReflexWithXefg);
        CHECK(MethodWithXefg(XefgLatency::AntiLag2) == Method::FakenvapiWithXefg);
        CHECK(CarriesXefgFrameIds(Method::XellWithXefg) && CarriesXefgFrameIds(Method::RealReflexWithXefg) &&
              CarriesXefgFrameIds(Method::FakenvapiWithXefg));
        CHECK(!CarriesXefgFrameIds(Method::RealReflex) && !CarriesXefgFrameIds(Method::Fakenvapi));
        CHECK(!LatencySplitFromXell(Method::XellWithXefg) && LatencySplitFromXell(Method::RealReflexWithXefg) &&
              LatencySplitFromXell(Method::FakenvapiWithXefg));
        CHECK(!LatencySplitFromXell(Method::RealReflex));

        // The frame cap: the driver (or Anti-Lag 2) paces the game's own frames, so the shown-frame cap is divided by the
        // frames XeFG shows per rendered one. XeLL being idle (XeFG paused) does not hand the cap away: it never held it.
        for (const auto method : { Method::RealReflexWithXefg, Method::FakenvapiWithXefg })
        {
            CapInputs cap;
            cap.running = true;
            cap.method = method;
            cap.capFps = 120.0f;
            cap.xefgGenerating = true;
            cap.shownPerRendered = 2;
            CHECK(PlanCap(cap).owner == CapOwner::LowLatency && PlanCap(cap).minimumIntervalUs == 16667);
            cap.shownPerRendered = 4;
            CHECK(PlanCap(cap).minimumIntervalUs == 33333);
            cap.shownPerRendered = 0; // never divides by nothing
            CHECK(PlanCap(cap).minimumIntervalUs == 8333);
            cap.xefgGenerating = false; // XeFG paused: shown frames are rendered ones
            cap.shownPerRendered = 1;
            CHECK(PlanCap(cap).owner == CapOwner::LowLatency && PlanCap(cap).minimumIntervalUs == 8333);
            CHECK(!XellIdle(cap));
            cap.capFps = 0.0f;
            CHECK(PlanCap(cap).owner == CapOwner::LowLatency && PlanCap(cap).minimumIntervalUs == 0);
            cap.running = false;
            CHECK(PlanCap(cap).owner == CapOwner::NotOurs);
        }

        // NVIDIA Reflex beside XeFG on the D3D11-to-D3D12 bridge: the limiter keeps the cap, the driver did not hold the
        // interval it accepted (Assetto Corsa). On D3D12 (NBA 2K27) and the Vulkan bridge (RDR2) the driver holds it, with
        // the cap divided by the frames XeFG shows per rendered one.
        {
            CapInputs cap;
            cap.running = true;
            cap.method = Method::RealReflexWithXefg;
            cap.capFps = 60.0f;
            cap.xefgGenerating = true;
            cap.shownPerRendered = 2;
            cap.limiterBeforeSleep = true; // D3D12 and the D3D11 bridge alike: not what decides it
            cap.d3d11Bridge = true;
            CHECK(PlanCap(cap).owner == CapOwner::Limiter && PlanCap(cap).minimumIntervalUs == 0);
            cap.d3d11Bridge = false;
            CHECK(PlanCap(cap).owner == CapOwner::LowLatency && PlanCap(cap).minimumIntervalUs == 33333);
            cap.limiterBeforeSleep = false; // the Vulkan bridge
            CHECK(PlanCap(cap).owner == CapOwner::LowLatency && PlanCap(cap).minimumIntervalUs == 33333);
            // Anti-Lag 2 keeps its own cap everywhere
            cap.method = Method::FakenvapiWithXefg;
            cap.d3d11Bridge = true;
            CHECK(PlanCap(cap).owner == CapOwner::LowLatency && PlanCap(cap).minimumIntervalUs == 33333);
            cap.d3d11Bridge = false;
            CHECK(PlanCap(cap).owner == CapOwner::LowLatency && PlanCap(cap).minimumIntervalUs == 33333);
        }

        // The one rule both the plan for our calls and ReflexHooks::update (the game's own Reflex) take
        CHECK(SplitLimiterKeepsCap(XefgLatency::Reflex, true));
        CHECK(!SplitLimiterKeepsCap(XefgLatency::Reflex, false));
        CHECK(!SplitLimiterKeepsCap(XefgLatency::AntiLag2, true));
        CHECK(!SplitLimiterKeepsCap(XefgLatency::AntiLag2, false));
        CHECK(!SplitLimiterKeepsCap(XefgLatency::XeLL, true));

        // XeLL as the output: as before
        CapInputs xellCap;
        xellCap.running = true;
        xellCap.method = Method::XellWithXefg;
        xellCap.capFps = 120.0f;
        xellCap.xefgGenerating = true;
        xellCap.shownPerRendered = 2;
        CHECK(PlanCap(xellCap).owner == CapOwner::LowLatency && PlanCap(xellCap).minimumIntervalUs == 8333);
        xellCap.xefgGenerating = false;
        CHECK(PlanCap(xellCap).owner == CapOwner::Limiter);

        // A bridged Vulkan game with the split: our calls go to the game's VkDevice through the Vulkan surface (the driver's
        // Reflex, or Anti-Lag Vk, lives there); XeLL on the bridge's device gets the markers through the Vulkan hooks
        {
            int vkDevice = 0, bridgeDevice12 = 0, d3dDevice = 0;
            auto t = PresentTarget(true, true, &vkDevice, &bridgeDevice12, true);
            CHECK(t.api == Api::Vulkan && t.device == &vkDevice);
            t = PresentTarget(true, true, &vkDevice, &bridgeDevice12, false);
            CHECK(t.api == Api::D3D && t.device == &bridgeDevice12);
            t = PresentTarget(true, false, &vkDevice, &bridgeDevice12, true);
            CHECK(t.api == Api::Vulkan && t.device == &vkDevice);
            t = PresentTarget(false, true, &d3dDevice, &bridgeDevice12, true);
            CHECK(t.api == Api::D3D && t.device == &d3dDevice);
        }

        // The status reason when the game's Reflex runs: names who lowers latency in the split, as before otherwise
        CHECK(std::strstr(DecisionText(Decision::GameRunsReflex, true, false, XefgLatency::Reflex), "own Reflex lowers") !=
              nullptr);
        CHECK(std::strstr(DecisionText(Decision::GameRunsReflex, true, false, XefgLatency::AntiLag2), "Anti-Lag 2") !=
              nullptr);
        CHECK(std::strcmp(DecisionText(Decision::GameRunsReflex, true, false, XefgLatency::XeLL),
                          "The game's Reflex feeds XeFG's XeLL") == 0);
        CHECK(std::strcmp(DecisionText(Decision::GameRunsReflex, false, false, XefgLatency::XeLL),
                          "Use the game's own Reflex settings") == 0);
        CHECK(std::strcmp(DecisionText(Decision::SettingOff, true, false, XefgLatency::Reflex),
                          DecisionText(Decision::SettingOff)) == 0);

        // Our SetSleepMode goes out again when XeFG's Output changes (the other side has to be told, the old one let go)
        {
            SleepModeCall last;
            last.api = Api::D3D;
            int device = 0;
            last.device = &device;
            last.minimumIntervalUs = 16667;
            last.xellGeneration = 3;
            last.side = (uint32_t) XefgLatency::XeLL;

            auto now = last;
            CHECK(!NeedsSleepModeResend(&last, false, now, false));
            now.side = (uint32_t) XefgLatency::Reflex;
            CHECK(NeedsSleepModeResend(&last, false, now, false));
            now.side = (uint32_t) XefgLatency::AntiLag2;
            CHECK(NeedsSleepModeResend(&last, false, now, false));
        }

        // The game's own Reflex cap: per rendered frame
        CHECK(CapPerRenderedFrame(120.0f, 2) == 60.0f);
        CHECK(CapPerRenderedFrame(120.0f, 4) == 30.0f);
        CHECK(CapPerRenderedFrame(120.0f, 1) == 120.0f);
        CHECK(CapPerRenderedFrame(120.0f, 0) == 120.0f);
        CHECK(CapPerRenderedFrame(0.0f, 4) == 0.0f);
    }

    // dxvk through the Vulkan bridge: the pair runs at the game's Present only with the key on; every other game keeps the
    // Vulkan (or its own D3D) site whatever the key says
    CHECK(SleepSiteFor(true, true) == SleepSite::GamePresent);
    CHECK(SleepSiteFor(true, false) == SleepSite::VulkanPresent);
    CHECK(SleepSiteFor(false, true) == SleepSite::VulkanPresent);
    CHECK(SleepSiteFor(false, false) == SleepSite::VulkanPresent);

    // The ledger of a dxvk game: a few frames in flight while the site changes, a failed Present, a doubled present, a
    // minimised game, the bridge going away. Every frame is paired exactly once and a game-site frame keeps its own ID.
    {
        constexpr auto none = [](int) { return false; };
        constexpr auto allVulkan = [](int) { return SleepSite::VulkanPresent; };
        constexpr auto allGame = [](int) { return SleepSite::GamePresent; };

        for (int lag = 0; lag <= 3; ++lag)
        {
            // The site never changes
            for (const auto r : { RunSeq(20, lag, allVulkan, none), RunSeq(20, lag, allGame, none) })
            {
                for (int f = 0; f < 20; ++f)
                    CHECK(r.pairs[f] == 1);
            }

            // Vulkan -> game at frame 8, and game -> Vulkan at frame 8: no frame loses its pair or gets two
            for (const bool toGame : { true, false })
            {
                const auto r = RunSeq(
                    20, lag, [&](int f) { return (f >= 8) == toGame ? SleepSite::GamePresent : SleepSite::VulkanPresent; },
                    none);

                for (int f = 0; f < 20; ++f)
                {
                    CHECK(r.pairs[f] == 1);
                    const bool gameFrame = (f >= 8) == toGame;
                    CHECK(r.idSeen[f] == (gameFrame ? 100u + f : 0u));
                }
            }

            // Back and forth every frame
            {
                const auto r = RunSeq(
                    24, lag, [](int f) { return f % 2 ? SleepSite::GamePresent : SleepSite::VulkanPresent; }, none);

                for (int f = 0; f < 24; ++f)
                {
                    CHECK(r.pairs[f] == 1);
                    CHECK(r.idSeen[f] == (f % 2 ? 100u + f : 0u));
                }
            }

            // Presents that did not go through (occluded, failed): their entries are undone, the rest stay in step
            {
                const auto r = RunSeq(20, lag, allGame, [](int f) { return f == 5 || f == 6 || f == 13; });

                for (int f = 0; f < 20; ++f)
                {
                    if (f == 5 || f == 6 || f == 13)
                        continue; // paired at the game's Present, but never presented by dxvk
                    CHECK(r.pairs[f] == 1);
                    CHECK(r.idSeen[f] == 100u + f);
                }
            }
        }
    }

    // Edges of the ledger itself
    {
        FrameLedger q;
        FrameLedger::Entry e;

        // Empty before any pop worked is just the start: no resync, and the caller pairs at the Vulkan present
        CHECK(!q.Pop(&e));
        CHECK(!q.TakeResync());

        // Undo of an entry dxvk already popped finds nothing
        const auto a = q.Push(1, SleepSite::GamePresent);
        const auto b = q.Push(2, SleepSite::VulkanPresent);
        CHECK(q.Pop(&e) && e.frameId == 1 && e.site == SleepSite::GamePresent);
        CHECK(!q.Undo(a));
        CHECK(q.Undo(b));
        CHECK(!q.Pop(&e));
        CHECK(q.TakeResync()); // a pop found nothing after pops had worked

        // A doubled dxvk present has nothing to pop for its second half: unpaired entries are not invented
        q.Push(3, SleepSite::GamePresent);
        CHECK(q.Pop(&e) && e.frameId == 3);
        CHECK(!q.Pop(&e));

        // Minimised: many game presents, no dxvk presents. The oldest are dropped, the newest kSize stay, and it is flagged
        q.Clear();
        q.TakeResync();

        for (uint32_t id = 1; id <= 20; ++id)
            q.Push(id, SleepSite::GamePresent);

        CHECK(q.count == FrameLedger::kSize);
        CHECK(q.TakeResync());
        CHECK(!q.TakeResync());
        CHECK(q.Pop(&e) && e.frameId == 20 - FrameLedger::kSize + 1);

        // The bridge goes away: Clear leaves nothing, and a new bridge's first pop is "the start" again
        q.Clear();
        CHECK(!q.Pop(&e));
        CHECK(!q.TakeResync());
        q.Push(9, SleepSite::GamePresent);
        CHECK(q.Pop(&e) && e.frameId == 9);
    }

    // The game minimised: it presents 20 frames (S_OK) that dxvk never presents, and goes on three seconds later. The
    // entries left over are too old to be for dxvk's next presents and are dropped, so the ledger is back in step and every
    // frame dxvk presents after it has its own ID. (Without the age rule the up-to-kSize left over would stay in front for
    // good.)
    {
        FrameLedger led;
        uint32_t idSeen[40] = {};

        for (int f = 0; f < 20; ++f)
            led.Push(100 + f, SleepSite::GamePresent, 0, 1000.0 + f * 16.0);

        for (int f = 20; f < 40; ++f)
        {
            const double now = 4000.0 + (f - 20) * 16.0;
            led.Push(100 + f, SleepSite::GamePresent, 0, now);

            if (f - 2 >= 20) // dxvk presents two frames behind the game
            {
                FrameLedger::Entry e;
                idSeen[f - 2] = led.Pop(&e, 0, now) ? e.frameId : 0;
            }
        }

        for (int p = 20; p < 38; ++p)
            CHECK(idSeen[p] == 100u + p);

        CHECK(led.TakeResync());
    }

    // Frame generation off and on again in place, on the same window: entries pushed for frames dxvk presented unbridged
    // are left over. They are dropped when the output is made again (a newer generation), and the frames presented after
    // it get their own IDs.
    {
        FrameLedger led;
        uint32_t idSeen[40] = {};
        const int lag = 2;

        for (int f = 0; f < 30; ++f)
        {
            const uint32_t generation = f < 10 ? 1 : 2;
            led.Push(100 + f, SleepSite::GamePresent, generation);

            const int p = f - lag; // dxvk presents frame p

            if (p >= 10) // frames before the output was made again are presented unbridged: no pop
            {
                FrameLedger::Entry e;
                idSeen[p] = led.Pop(&e, 2) ? e.frameId : 0;
            }
        }

        for (int p = 10; p < 28; ++p)
            CHECK(idSeen[p] == 100u + p);

        CHECK(led.TakeResync());
    }

    // An entry's ID is read with the site lock held: the other site's End moves the frame ID on while the game's Present
    // waits for the lock, and the entry must carry the ID the markers do
    {
        FrameLedger led;
        uint32_t frameId = 7;
        uint32_t markerId = 0;
        uint64_t seq = BeginGameSiteFrame(
            [&]
            {
                frameId = 8; // dxvk's thread finished the Vulkan site's End for the older frame first
                return 0;
            },
            [&] { return frameId; }, [&](uint32_t id) { return led.Push(id, SleepSite::GamePresent); },
            [&] { markerId = frameId; });

        FrameLedger::Entry e;
        CHECK(seq != 0 && led.Pop(&e));
        CHECK(e.frameId == markerId);
        CHECK(e.frameId == 8);
    }

    // The pair's End never comes (the device went away between Begin and End): not left half way, the frame ID moves on
    {
        bool started = true;
        bool submitted = true;
        uint64_t frameId = 5;
        AbandonPair(started, submitted, frameId);
        CHECK(!started && !submitted && frameId == 6);
    }

    // What the old design did (the Vulkan present reading the site the game's latest Present chose) is not enough: with the
    // site changing while frames are in flight, a frame is left unpaired or paired twice. Kept as a check of the check.
    {
        const int lag = 2;
        int pairs[20] = {};

        for (int f = 0; f < 20; ++f) // the game's Present f pairs at the game's Present if its latch is GamePresent
        {
            const bool game = f >= 8;

            if (game)
                ++pairs[f];

            // dxvk presents frame f - lag now and reads the latch of the game's latest Present (f)
            if (f - lag >= 0 && !game)
                ++pairs[f - lag];
        }

        int wrong = 0;

        for (int f = 0; f < 18; ++f)
            wrong += pairs[f] != 1;

        CHECK(wrong > 0);
    }

    printf(fails == 0 ? "nr_native_low_latency_smoke: PASS\n" : "nr_native_low_latency_smoke: %d FAIL\n", fails);
    return fails == 0 ? 0 : 1;
}
