#pragma once

// InputCommon's outputs on OptiScaler's terms: which low latency output runs, the sleep mode each one gets, and who
// keeps the frame cap while the game's own Reflex is the input. Before InputCommon ran them these decisions were spread
// over ReflexHooks' router (hooks/Reflex_Hooks.cpp) and fakenvapi's own context (nvapi/fakenvapi/low_latency_d3d.cpp);
// the policy is the same, native/XefgReflexRule.h and native/NativeLowLatencyRule.h are its rules. Pure, host tested
// (tests/ll_output_rule_smoke.cpp).

#include "../../native/XefgReflexRule.h"

#include <cstdint>

namespace lloutput
{
using native::lowlatency::XefgLatency;
using native::xefgreflex::SleepMode;

// The low latency methods InputCommon can run (LowLatencyMode's). On a Vulkan device Reflex is the driver's Vulkan
// Reflex (ReflexVkSurface) and Anti-Lag 2 is Anti-Lag Vk.
enum class Tech : uint8_t
{
    None,
    Reflex,      // the driver's (NVIDIA, real nvapi)
    AntiLag2,    // AMD
    XeLL,        // Intel's, also XeFG's on every GPU; D3D12 only
    LatencyFlex, // works anywhere
    FrameCap,    // only waits for OptiScaler's frame cap, at the Sleep or simulation start (ll_fps_limiter.h)
};

struct OutputInputs
{
    bool realNvapi = false;               // NVIDIA's nvapi on an NVIDIA GPU, not fakenvapi as the game's nvapi
    bool vulkan = false;                  // the input's device is a Vulkan one
    bool xefgOutput = false;              // XeFG is the frame generation output
    XefgLatency side = XefgLatency::XeLL; // with XeFG, what lowers latency (native::lowlatency::XefgSide, in force)
    bool forceLatencyFlex = false;        // fakenvapi's Force LatencyFlex
    bool frameCapOnly = false;            // [Framerate] FrameCapOnly, the Output "Frame cap only"
    bool multiFrame = false; // DLSS FG with more than one generated frame, or Dynamic: no Anti-Lag 2 or XeLL
    // The input sends XeLL's six markers with frame ids every frame (Intel R9, XL18): not the UE mod (simulation
    // markers only) or Anti-Lag 2 (no frame ids). XeLL fed half a frame does harm (InputCommon::incompatibility).
    bool inputFeedsXell = true;
};

// The output to start, the ones to try when it can't start, in order, and whether XeFG gets an XeLL context of its own
// next to it. Without XeFG fakenvapi tried Anti-Lag 2, then XeLL, then LatencyFlex (init fails on the wrong GPU).
struct OutputPlan
{
    Tech tries[3] = { Tech::None, Tech::None, Tech::None };
    bool xefgOwnXell = false; // XeFG paces with its own XeLL (low latency off), the output sleeps

    constexpr Tech first() const { return tries[0]; }
};

constexpr OutputPlan PlanOutput(const OutputInputs& in)
{
    if (in.xefgOutput)
    {
        switch (in.side)
        {
        case XefgLatency::Reflex:
            return { { Tech::Reflex, Tech::None, Tech::None }, true };
        case XefgLatency::AntiLag2:
            return { { Tech::AntiLag2, Tech::None, Tech::None }, true };
        default:
            // XeFG paces with the XeLL output's own context, when the input can feed it; else LatencyFlex lowers
            // latency and XeFG keeps its own XeLL with low latency off, as next to Reflex
            if (!in.inputFeedsXell)
                return { { Tech::LatencyFlex, Tech::None, Tech::None }, true };

            return { { Tech::XeLL, Tech::None, Tech::None }, false };
        }
    }

    if (in.realNvapi)
        return { { Tech::Reflex, Tech::None, Tech::None }, false };

    // The player's choice over Anti-Lag 2, XeLL and LatencyFlex: it waits where they do, without their latency work
    // (the menu says so). It counts the game's frames, more than one generated frame is fine.
    if (in.frameCapOnly)
        return { { Tech::FrameCap, Tech::None, Tech::None }, false };

    // fakenvapi as the game's nvapi: Anti-Lag 2 and XeLL fall apart with more than one generated frame
    if (in.forceLatencyFlex || in.multiFrame)
        return { { Tech::LatencyFlex, Tech::None, Tech::None }, false };

    // XeLL is D3D12's: a Vulkan device has Anti-Lag Vk, then LatencyFlex
    if (!in.inputFeedsXell || in.vulkan)
        return { { Tech::AntiLag2, Tech::LatencyFlex, Tech::None }, false };

    return { { Tech::AntiLag2, Tech::XeLL, Tech::LatencyFlex }, false };
}

// The driver's Reflex on a Vulkan device: VK_NV_low_latency2 (upstream's output, ll_reflex_vk.h) where the device has
// it and its swapchain has the latency mode, else NVIDIA's older NvAPI_Vulkan Reflex (ll_reflex_nvapi_vk.h), which
// works on the device alone (upstream ran LatencyFlex there)
enum class ReflexVkSurface : uint8_t
{
    None,
    LowLatency2,
    NvApi,
};

struct ReflexVkInputs
{
    bool lowLatency2Device = false; // made with VK_NV_low_latency2 and timeline semaphores (VulkanHooks::LowLatency2On)
    bool swapchainKnown = false;    // the device has a swapchain
    bool swapchainLatencyMode = false; // ... made with the latency mode (ours chained in, or the game's)
    bool nvapiVulkan = false;          // NVIDIA's nvapi with its Vulkan Reflex
    bool timelineSemaphores = false;   // NvAPI_Vulkan's sleep signals a timeline semaphore
    bool lowLatency2Failed = false;    // VK_NV_low_latency2's sleep never signalled on this device (ReflexVk gave up)
};

// Also asked of a running VK_NV_low_latency2 output: another answer makes it again (its swapchain was remade without
// the latency mode, or its sleep never signals)
constexpr ReflexVkSurface ChooseReflexVk(const ReflexVkInputs& in)
{
    // No swapchain yet: the device's next one gets the latency mode (VulkanHooks chains it in)
    if (in.lowLatency2Device && !in.lowLatency2Failed && (!in.swapchainKnown || in.swapchainLatencyMode))
        return ReflexVkSurface::LowLatency2;

    if (in.nvapiVulkan && in.timelineSemaphores)
        return ReflexVkSurface::NvApi;

    return ReflexVkSurface::None;
}

// Where a game's NvAPI_Vulkan_* Reflex call goes (InputReflex::Vulkan_*). A device with a sleep semaphore of ours (made
// at its InitLowLatencyDevice): InputCommon, our semaphore is signalled after the sleep. Else the game made the device
// a low latency one with the driver before our hooks, whose semaphore only the driver's Sleep signals: every call to
// the driver as it is (one sleeper, the limiter keeps the cap: the game's Reflex is no input). Neither (fakenvapi as
// the nvapi sets every device up through us, so a device it never saw): InputCommon, and a Sleep signals nothing.
enum class GameVkReflexRoute : uint8_t
{
    InputCommon,
    Driver,
    NoSemaphore,
};

constexpr GameVkReflexRoute RouteGameVkReflex(bool ourSemaphore, bool driverVulkanReflex)
{
    if (ourSemaphore)
        return GameVkReflexRoute::InputCommon;

    return driverVulkanReflex ? GameVkReflexRoute::Driver : GameVkReflexRoute::NoSemaphore;
}

// VK_NV_low_latency2's VkLatencyMarkerNV against NVAPI's marker types (MarkerType, NV_LATENCY_MARKER_TYPE): 0-7
// (simulation start .. trigger flash) are the same; NVAPI has PC_LATENCY_PING at 8, which Vulkan does not have, so
// Vulkan's out of band markers 8-11 are NVAPI's 9-12. False for a value that has no counterpart.
inline constexpr uint32_t kNvApiPcLatencyPing = 8;
inline constexpr uint32_t kNvApiLastMarker = 12; // OUT_OF_BAND_PRESENT_END

constexpr bool MarkerFromVulkanNV(uint32_t vkMarker, uint32_t* nvapiMarker)
{
    if (vkMarker > kNvApiLastMarker - 1)
        return false;

    *nvapiMarker = vkMarker < kNvApiPcLatencyPing ? vkMarker : vkMarker + 1;
    return true;
}

constexpr bool MarkerToVulkanNV(uint32_t nvapiMarker, uint32_t* vkMarker)
{
    if (nvapiMarker == kNvApiPcLatencyPing || nvapiMarker > kNvApiLastMarker)
        return false;

    *vkMarker = nvapiMarker < kNvApiPcLatencyPing ? nvapiMarker : nvapiMarker - 1;
    return true;
}

// Where InputCommon::update runs: once per frame, on one thread (its static locals rely on that). A D3D game, dxvk's
// too: its own DXGI present (dxvk's Vulkan present runs on dxvk's own thread). A native Vulkan game: its Vulkan
// present; its only DXGI swapchain is the present bridge's D3D12 one (frame generation's output). Told apart by what
// presents, not by the DXGI adapter: under Proton every DXGI adapter is dxvk's, a native Vulkan game's too.
enum class PresentSite : uint8_t
{
    Vulkan, // vkQueuePresentKHR
    Dxgi,   // the wrapped DXGI swapchain's present
};

// A Vulkan present this long after the game's own last DXGI present is a native Vulkan game's (a D3D game presents
// every frame)
inline constexpr double kD3dGamePresentGapMs = 250.0;

// `bridgeOutput`: a DXGI present of the Vulkan bridge's own D3D12 swapchain. `msSinceD3dGamePresent`: since the game's
// own last DXGI present, negative when it never made one.
constexpr bool RunsLowLatencyUpdate(PresentSite site, bool bridgeOutput, double msSinceD3dGamePresent)
{
    if (site == PresentSite::Dxgi)
        return !bridgeOutput;

    return msSinceD3dGamePresent < 0.0 || msSinceD3dGamePresent > kD3dGamePresentGapMs;
}

// The low latency inputs InputCommon chooses among (LowLatencyInput's)
enum class Input : uint8_t
{
    None,
    Reflex,     // the game's
    OptiScaler, // OptiScaler's own calls (native::lowlatency)
    UeLowLatency,
    XeLL,     // the game's (LOW_LATENCY_INPUTS builds)
    AntiLag2, // the game's (LOW_LATENCY_INPUTS builds)
};

struct InputChoice
{
    bool lowLatencyInputs = false;  // built with LOW_LATENCY_INPUTS: the game's XeLL and Anti-Lag 2 are inputs too
    bool reflex = false;            // the game made a Reflex call that runs low latency (not a query)
    bool optiscalerRunning = false; // native::lowlatency runs (InputCommon::set_optiscaler_running)
    bool ue = false;                // the UE Low Latency mod calls
    bool xell = false;              // the game's XeLL
    bool xellLowLatency = false;    // ... with low latency on
    bool xefgInput = false;         // the frame generation input is the game's XeFG
    bool antiLag2 = false;          // the game's Anti-Lag 2
    bool antiLag2ReportsFg = false; // ... reports its frame generation's frames
    bool antiLagVk = false;         // the game's Anti-Lag Vk (VK_AMD_anti_lag), in every build
};

// One sleeper per frame: the game's own Reflex, then OptiScaler's own low latency while it runs (Optical F5Low, for
// frame generation), then the UE Low Latency mod, each standing aside for the one before. With the low latency inputs
// the game's XeLL for its XeFG and the game's FSR-FG's Anti-Lag 2 come first, its other XeLL and Anti-Lag 2 last.
constexpr Input ChooseInput(const InputChoice& in)
{
    if (in.lowLatencyInputs)
    {
        // The game's XeFG paces with its XeLL; Reflex markers it also sends belong to its other paths
        if (in.xefgInput && in.xell && in.xellLowLatency)
            return Input::XeLL;

        // The game's FSR-FG only reports its generated frames through Anti-Lag 2
        if (in.antiLag2 && in.antiLag2ReportsFg)
            return Input::AntiLag2;
    }

    if (in.reflex)
        return Input::Reflex;

    if (in.optiscalerRunning)
        return Input::OptiScaler;

    if (in.ue)
        return Input::UeLowLatency;

    // A Vulkan game's own Anti-Lag: last, as the game's Anti-Lag 2 is
    if (in.antiLagVk)
        return Input::AntiLag2;

    if (in.lowLatencyInputs)
    {
        // A game can have XeLL without it being enabled in its settings, it does nothing until it is
        if (in.xell && in.xellLowLatency)
            return Input::XeLL;

        if (in.antiLag2)
            return Input::AntiLag2;

        if (in.xell)
            return Input::XeLL;
    }

    return Input::None;
}

// XeFG's Output side changed (XeLL <-> Reflex / Anti-Lag 2) and the output follows on the next call: a cap planned for
// the new side must not go to the old output (XeLL counts shown frames, the others rendered ones). The new output gets
// it when it starts. An input that can't feed XeLL has LatencyFlex on the XeLL side (PlanOutput).
constexpr bool OutputSwitchPending(bool xefgOutput, bool hasOutput, XefgLatency side, bool outputIsXell,
                                   bool inputFeedsXell = true)
{
    if (!xefgOutput || !hasOutput)
        return false;

    const bool sideIsXell = side == XefgLatency::XeLL && inputFeedsXell;
    return sideIsXell != outputIsXell;
}

// The interval the Streamline of OptiScaler's DLSS FG output carries in its own sleep mode. It sets the device's sleep
// mode itself every frame, also next to the game's Reflex output: it carries the cap the output keeps, or its next call
// takes the cap off the device (the driver keeps the last mode set). 0 when the output keeps no cap.
constexpr uint32_t StreamlineCapUs(bool streamlineSet, uint32_t outputIntervalUs)
{
    return streamlineSet ? outputIntervalUs : 0u;
}

// Which tech a sleep mode is for
enum class Role : uint8_t
{
    Output,   // the output: it sleeps and, while it keeps it, holds the cap
    XefgXell, // XeFG's own XeLL next to a Reflex or Anti-Lag 2 output: low latency off, it only keeps XeFG running
};

struct ShapeInputs
{
    bool xefgOutput = false;
    XefgLatency side = XefgLatency::XeLL;
    uint32_t capIntervalUs = 0; // OptiScaler's frame cap for the output, in place of the input's interval; 0: none
    bool forceDisabled = false; // fakenvapi's Force Disable
    bool blocked = false;       // the Framerate page's Input is Off (native::lowlatency::GameLowLatencyBlocked)
    bool inputSetMode = true;   // the input has set a sleep mode (a game may only mark frames)
    // The input's own cap (OptiScaler's calls carry theirs) was planned for XeFG's other side: XeLL counts shown
    // frames, the others rendered ones. No interval and no Boost until it sends one for this side.
    bool modeForOtherSide = false;
};

// The sleep mode a tech gets from the input's own: XeLL with XeFG has low latency on whatever the game says (only Force
// Disable turns it off), the split's other side the game's own with the cap, XeFG's own XeLL everything off; Input Off
// sends low latency off, no Boost and no interval everywhere (OptiScaler's limiter keeps the cap)
constexpr SleepMode SleepModeFor(const SleepMode& game, Role role, const ShapeInputs& in)
{
    using namespace native::xefgreflex;

    const bool split = in.xefgOutput && in.side != XefgLatency::XeLL;

    SleepMode input = game;

    if (in.modeForOtherSide)
    {
        input.boost = false;
        input.minimumIntervalUs = 0;
    }

    if (role == Role::XefgXell)
        return XellSleepModeInSplit(SleepModeForOutput(input, in.capIntervalUs, true, in.forceDisabled, in.blocked));

    if (split && !in.blocked)
    {
        // A game that only marks frames: Anti-Lag 2 still sleeps on its markers with low latency on, the driver's
        // Reflex has nothing to wait in
        if (!in.inputSetMode && in.side == XefgLatency::AntiLag2)
        {
            SleepMode markersOnly;
            markersOnly.lowLatency = true;
            markersOnly.minimumIntervalUs = in.capIntervalUs;
            return markersOnly;
        }

        return DriverSleepModeInSplit(input, in.capIntervalUs);
    }

    return SleepModeForOutput(input, in.capIntervalUs, in.xefgOutput && !split, in.forceDisabled, in.blocked);
}

// The game's own Reflex as the input: who keeps OptiScaler's frame cap (the output, through its sleep mode, or
// OptiScaler's limiter) and at what rate. ReflexHooks::update's rule for D3D games.
struct GameCapInputs
{
    float capFps = 0.0f;                  // FramerateLimit (shown frames); 0: none
    bool externalFrameGeneration = false; // another frame generation owns Reflex
    bool markersRecently = false;         // the game sent a marker in the last 20 presents
    bool gameSetSleepMode = false;        // it set a sleep mode on a device that is still there
    bool gameLowLatencyOn = false;        // ... with low latency on
    bool gameSendingMarkers = false;      // its markers keep up with the frames
    bool realNvapi = false;
    bool xefgOutput = false;
    XefgLatency side = XefgLatency::XeLL;
    bool splitLimiterKeepsCap = false; // native::lowlatency::SplitLimiterKeepsCap
    bool xellHoldsCap = false;         // native::xefgreflex::XellHoldsCap (Force Disable, idle XeLL)
    bool fsrFgOutput = false;          // FSR FG is the frame generation output
    // OptiScaler's frame generation generates frames (IFGFeature::IsGenerating): steady over a frame's presents. A
    // per-present value (generated or real) would hand the cap back and forth every frame.
    bool fgActive = false;
    bool blocked = false;              // Input Off
    uint32_t xefgShownPerRendered = 1; // native::lowlatency::XefgShownPerRendered
    bool xellCountsShown = true;       // XeFG's XeLL counts shown frames (XeFGProxy::XellCountsShownFrames)
    bool driverMultipliesCap = false;  // DLSS-G behind XeFG's API (XeFGProxy::DriverMultipliesReflexCap)
    uint8_t dlssgFramesToGenerate = 0; // the game's DLSS FG, seen from its async markers
    bool outputIsXell = false;         // the output runs XeLL (fakenvapi's choice)
    bool outputIsFrameCap = false;     // the output only waits for the cap (Frame cap only)
    bool forceDisabled = false;        // fakenvapi's Force Disable: Frame cap only stops waiting
    bool outputRunning = true;         // an output runs (none: not started yet, or it could not start)
};

struct GameCap
{
    bool outputHoldsCap = false;
    float fps = 0.0f;         // the rate the output gets when it keeps the cap
    bool showWarning = false; // FSR FG with real Reflex markers and Reflex on: OptiScaler warns about input latency
};

constexpr GameCap CapForGame(const GameCapInputs& in)
{
    GameCap out;

    if (in.externalFrameGeneration || !in.markersRecently)
        return out;

    const bool split = in.xefgOutput && in.side != XefgLatency::XeLL;

    if (in.xefgOutput && (in.gameSetSleepMode || in.gameSendingMarkers))
    {
        if (split)
        {
            // A game that marks frames and never sleeps: only Anti-Lag 2 (fakenvapi) acts on the markers alone
            out.outputHoldsCap =
                !in.splitLimiterKeepsCap && (!in.realNvapi || (in.gameSetSleepMode && in.gameLowLatencyOn));
        }
        else
        {
            out.outputHoldsCap = in.xellHoldsCap;
        }
    }
    else if (in.outputIsFrameCap)
    {
        // Waiting for the cap is all it does: it keeps it with the game's markers alone too (it waits at simulation
        // start without a Sleep), unless Force Disable stops it
        out.outputHoldsCap = !in.forceDisabled;
    }
    else if (in.gameSetSleepMode)
    {
        // Real Reflex markers with FSR FG and Reflex off: huge input latency, the limiter keeps it. Not for
        // OptiScaler's DLSS FG output: its Streamline sets low latency on itself and carries the cap (StreamlineCapUs).
        out.outputHoldsCap = !in.realNvapi || !in.fsrFgOutput || !in.fgActive || in.gameLowLatencyOn;
        out.showWarning = in.fsrFgOutput && in.realNvapi && in.fgActive && in.gameLowLatencyOn;
    }

    if (in.blocked)
    {
        out.outputHoldsCap = false;
        out.showWarning = false;
    }

    // Nothing would sleep with the cap: the limiter keeps it (one cap owner, never none)
    if (!in.outputRunning)
        out.outputHoldsCap = false;

    if (!out.outputHoldsCap)
        return out;

    float fps = in.capFps;

    if (in.fgActive && in.fsrFgOutput)
        fps /= 2;
    else if (split)
    {
        // Per rendered frame, except DLSS-G behind XeFG's API: the driver multiplies the interval itself (as for a
        // game's DLSS-G), so the shown cap stays and no later rule may divide it
        if (!(in.side == XefgLatency::Reflex && in.realNvapi && in.driverMultipliesCap))
            fps = native::lowlatency::CapPerRenderedFrame(fps, in.xefgShownPerRendered);
    }
    else if (in.xefgOutput && in.xellHoldsCap && !in.xellCountsShown) // XeFG's XeLL counts the game's own frames (XL23)
        fps = native::lowlatency::CapPerRenderedFrame(fps, in.xefgShownPerRendered);
    else if (in.dlssgFramesToGenerate > 0 && !in.realNvapi && !in.outputIsXell)
        fps /= (float) (in.dlssgFramesToGenerate + 1);

    out.fps = fps;
    return out;
}

// Whose async frame marker (NvAPI_D3D12_SetAsyncFrameMarker) it is. The frame generation library OptiScaler loaded (the
// own generator, Intel's) marks the presents it makes itself: that is neither the game's Reflex (the game did not call
// Reflex, so OptiScaler stands aside for nobody) nor a marker for the active input to take or drop. Judged by the
// module the call came from, never by a file name.
constexpr bool IsFgLibraryMarker(const void* callerModule, const void* fgLibraryModule)
{
    return fgLibraryModule != nullptr && callerModule == fgLibraryModule;
}

// The interval a sleep mode carries for `fps`, 0: no cap
constexpr uint32_t IntervalUs(float fps) { return fps > 0.0f ? static_cast<uint32_t>(1'000'000.0f / fps + 0.5f) : 0; }

// Whether one read of the report counts toward the shown input latency: the report's frame must be a new one since the
// last read (a loading screen or a pause presents nothing, the frame ID stays and its GPU mark keeps rising) and the
// sample must be sane (500 ms or less). The first read has no last frame to compare. Otherwise the average starts over.
constexpr bool InputLatencySampleCounts(bool haveLastFrame, uint64_t lastFrameId, uint64_t frameId, double ms)
{
    return ms <= 500.0 && (!haveLastFrame || frameId != lastFrameId);
}

// The input latency the FPS overlay shows, from one frame report's marks (all in the report's own unit, microseconds, 0
// = not set): from the simulation start to the GPU finishing the frame. It stops at the end of Present when the report
// has no usable GPU mark (XeLL gives none, DLSS-G's ends before the Present, the markers-only reports). Not the whole
// span of the report: its earliest and latest marks can lie well outside simulation start .. GPU done.
struct InputLatency
{
    bool valid = false;
    double ms = 0.0;
    bool reachesGpu = false; // true: simulation start to GPU done, false: simulation start to the end of Present
};

constexpr InputLatency InputLatencyFromReport(uint64_t simStart, uint64_t presentEnd, uint64_t gpuRenderEnd)
{
    if (simStart == 0)
        return {};

    if (gpuRenderEnd != 0 && gpuRenderEnd >= presentEnd && gpuRenderEnd >= simStart)
        return { true, static_cast<double>(gpuRenderEnd - simStart) / 1000.0, true };

    if (presentEnd != 0 && presentEnd > simStart)
        return { true, static_cast<double>(presentEnd - simStart) / 1000.0, false };

    return {};
}
} // namespace lloutput
