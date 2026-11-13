#pragma once

// Optical F5Low's low latency: OptiScaler calls the Reflex API itself, because a game that makes no upscaler call makes
// no Reflex call either, and XeFG's XeLL needs those calls in any game that does not make them. One path for every vendor: through ReflexHooks' function table, so real Reflex on NVIDIA and
// fakenvapi (Anti-Lag 2, XeLL or LatencyFlex, whichever the fakenvapi settings pick) everywhere else. D3D11 and D3D12
// go through the D3D-flavoured NVAPI entry points (NvAPI_D3D_*); a native Vulkan game goes through the Vulkan-flavoured
// ones (NvAPI_Vulkan_*, OnPresentBeginVulkan/OnPresentEndVulkan below) -- different device handle, different call
// surface, but the same decision and pacing underneath.
//
// The calls are paced by the game's Present, since there are no engine hooks:
//   after Present returns: PRESENT_END, Sleep, SIMULATION_START
//   the frame's first queue submit (D3D12) / first draw (D3D11) / nothing earlier on Vulkan: SIMULATION_END,
//   RENDERSUBMIT_START
//   just before Present: RENDERSUBMIT_END, PRESENT_START
// The markers are approximate, so useMarkersToOptimize stays off. It runs only while native/NativeLowLatencyRule.h
// says so (an Optical F5Low mode or XeFG runs, the game makes no Reflex call itself, fakenvapi's Force Reflex and Force
// XeLL are not in the way) and stands aside the moment the game calls Reflex. Our own calls are not counted as the game's.
// Never writes a fakenvapi setting.

#include "NativeLowLatencyRule.h"

#include <Unknwn.h>
#include <vulkan/vulkan.h>

namespace native::lowlatency
{
// The game's Present: before the real Present (RENDERSUBMIT_END, PRESENT_START) and after it returns (PRESENT_END,
// Sleep, SIMULATION_START). `device` is the game's D3D12 or D3D11 device. SwapChain calls are ignored while
// FrameGeneration calls come in.
void OnPresentBegin(IUnknown* device, PresentSource source = PresentSource::SwapChain);
void OnPresentEnd(IUnknown* device, PresentSource source = PresentSource::SwapChain);

// Same pairing, for a native Vulkan game (native/NativeDriverVk.cpp, around the game's own vkQueuePresentKHR). There is
// no earlier queue-submit hook on this path either, so OnPresentBeginVulkan's own fallback (as OnPresentBegin's) sends
// SIMULATION_END/RENDERSUBMIT_START too, right before RENDERSUBMIT_END/PRESENT_START: no OnFirstSubmit equivalent
// needed. Calls the Vulkan-flavoured Reflex entry points (NvAPI_Vulkan_*), never the D3D ones, even when a D3D12 device
// happens to share this process (dxvk): the two share no state. While the Vulkan present bridge is up the game's present
// is paced on the bridge's private D3D12 device through OnPresentBegin/OnPresentEnd instead (hooks/Vulkan_Hooks.cpp,
// NativeLowLatencyRule.h PresentTarget), so XeFG's XeLL routing applies; each call carries its own API.
void OnPresentBeginVulkan(VkDevice device);
void OnPresentEndVulkan(VkDevice device);

// A D3D11 game on dxvk with the Vulkan present bridge presents twice a frame: the game's D3D11 Present on its own thread,
// and dxvk's vkQueuePresentKHR on dxvk's. Each side notes its thread (cheap, a relaxed exchange; nothing is logged per
// frame) and the one line saying whether the two are the same is logged when both are known, again if either changes.
void NoteDxvkGamePresentThread();
void NoteDxvkVulkanPresentThread();

// Where the pair runs for that game (NativeLowLatencyRule.h SleepSite). The game's Present decides, once per frame, and
// writes the decision into the frame's FrameLedger entry; dxvk's Vulkan present for that frame follows its own entry. So a
// frame is paired exactly once at one site, also across a change of the setting or of XeFG's Output.
//
// hkvkQueuePresentKHR, every present of the bridged swapchain under dxvk, before anything else is done with the frame:
// publishes the devices the game's Present may use (the bridge's D3D12 device and the VkDevice, as one unit), takes the
// frame's entry, and hands XeFG the frame's ID (FrameIdForFrameGeneration). Returns true when the Vulkan present runs the
// pair for this frame (the entry says so, or none was queued).
bool DxvkVulkanPresent(VkDevice vkDevice, IUnknown* device12, uint32_t generation);

// The game's Present with no bridge output up: what the ledger holds is for frames dxvk presents unbridged
void DxvkBridgeDown();

// What the game's Present began, to end it the same way
struct DxvkGameScope
{
    bool began = false;    // the pair's Begin ran here
    bool pushed = false;   // a ledger entry was pushed
    uint64_t seq = 0;      // the entry's
    Api api = Api::D3D;
    IUnknown* device = nullptr; // a D3D device, or the VkDevice as a token (Api::Vulkan)
};

// The wrapped D3D11 swap chain's Present under dxvk through the Vulkan bridge, for the swap chain on the bridge's window
// (the caller checked both): before the real Present, and after it returns with its result. `keyOn`: the ini key.
// `window`: the bridge's window, a different one than last time starts the ledger over; `generation`: the bridge output's
// (VkPresentBridge::OutputGeneration), a newer one does the same. The API is chosen here, from the
// Output now (PresentTarget), never from the last frame's.
DxvkGameScope OnDxvkGamePresentBegin(bool keyOn, HWND window, uint32_t generation);
void OnDxvkGamePresentEnd(const DxvkGameScope& scope, HRESULT presentResult);

// Held around each Begin/End call of either site while the bridge is up under dxvk. A frame is paired at one site, but
// the pair's state (the frame ID, the sleep mode) is written for one thread, and two frames can overlap right after the
// site changed: dxvk's thread still in the Vulkan site's pair of an older frame while the game's thread starts the next
// one. The lock serialises those; in steady state only one side calls, so it costs nothing, and at a change the other
// side may wait out one Sleep once. `take` false: no lock.
class DxvkSiteLock
{
  public:
    explicit DxvkSiteLock(bool take);
    ~DxvkSiteLock();
    DxvkSiteLock(const DxvkSiteLock&) = delete;
    DxvkSiteLock& operator=(const DxvkSiteLock&) = delete;

  private:
    bool _held;
};

// The frame's first queue submit (D3D12) or first draw (D3D11): any thread, one relaxed load when we do not run.
// `queue`: the D3D12 queue submitted to; only the game's queue counts. nullptr (D3D11) always counts.
void OnFirstSubmit(const void* queue = nullptr);

// The game's device is being released for good: nothing of ours may use it again.
void OnDeviceReleased(IUnknown* device);
void OnDeviceReleasedVulkan(VkDevice device);

struct Status
{
    Decision decision = Decision::NoF5Low; // as the menu shows it (ShownDecision), not always the one the code runs on
    const char* path = ""; // plain words for the method in use, empty unless decision is Run
    bool hasLatency = false;
    float latencyMs = 0.0f; // the game's simulation start to the end of the GPU work, from GetLatency
    bool latencyEstimated = false; // fakenvapi's report (no GPU times): latencyMs runs to the present's end instead
    CapOwner capOwner = CapOwner::NotOurs;
    bool xellIdle = false; // XeLL on an NVIDIA/AMD GPU with no XeFG generating frames: it does nothing
    RunsFor runsFor = RunsFor::F5Low; // what it runs for: Optical F5Low, or frame generation alone (XeFG, FSR FG)
    // The game's own Reflex calls are sent to XeFG's XeLL (ReflexHooks::gameReflexFeedsXell): DecisionText says so
    bool gameReflexFeedsXell = false;
    bool forceReflexDisabled = false; // fakenvapi's Force Disable: XeLL gets the game's calls with low latency off
    // With XeFG: what lowers latency (XefgSide). Reflex or Anti-Lag 2: XeLL only keeps XeFG running, so the game's calls do
    // not "feed XeLL" (gameReflexFeedsXell is false then)
    XefgLatency xefgSide = XefgLatency::XeLL;
    // What lowers latency here whoever makes the calls (plain words, never empty): the menu's "Active: <input> -> <output>"
    const char* output = "";
    bool offKeptForFrameGeneration = false; // Input is Off, but DLSS FG needs Reflex: the game's Reflex stays on
};

// Frame generation that runs Reflex itself (DLSS FG, an external owner): it cannot work with low latency off. Any thread.
bool FrameGenerationNeedsReflex();

// The Framerate page's Input is Off and nothing needs the game's Reflex on: its sleep mode (D3D and Vulkan NVAPI,
// VK_NV_low_latency2) goes out with low latency off, no Boost and no interval (hooks/Reflex_Hooks.cpp), and OptiScaler's
// limiter keeps the frame cap. Any thread, cheap.
bool GameLowLatencyBlocked();

// With XeFG as the frame generation output: what lowers latency right now (NativeLowLatencyRule.h XefgLatency). XeLL
// unless the player chose Reflex or Anti-Lag 2 ([XeFG] LowLatencyOutput) and the setup has it, and neither the
// Framerate page's Input Off nor fakenvapi's Force Disable is on (native/XefgReflexRule.h EffectiveXefgLatency). Reflex
// or Anti-Lag 2 means "split": XeLL stays in place for XeFG with its low latency off, and the other side gets the calls
// (InputCommon's output). Any thread, cheap.
XefgLatency XefgSide();

// What the Output combo works from: the setup and the choice made
OutputInputs CurrentOutputInputs();

// Frames XeFG shows per frame the game renders: 1 plus the ones it generates, 1 while it generates none. Any thread.
uint32_t XefgShownPerRendered();
// XeFG's XeLL counts shown frames (false with a library behind XeFG's API that does not tell XeLL about its frames)
bool XellCountsShown();
// XeFG frames from DLSS-G (xefg_cuda Generator=dlssg): the driver multiplies a Reflex interval itself
bool DriverMultipliesReflexCap();

// The menu's line. Cheap; GetLatency is asked at most twice a second.
Status GetStatus();

// The one reason both pages show after "[<StatusLabel>]": the method in use while we run (empty path: the first frame,
// before the first Reflex call went out), else why we do not
inline const char* ReasonText(const Status& status)
{
    if (status.decision == Decision::Run)
        return status.path[0] != '\0' ? status.path : "Starting";

    return DecisionText(status.decision, status.gameReflexFeedsXell, status.forceReflexDisabled, status.xefgSide);
}

// We run (markers and Sleep go out). Any thread, two relaxed loads.
bool Running();

// We run and our SetSleepMode carries the frame cap (NativeLowLatencyRule.h PlanCap): OptiScaler's own limiter
// (misc/FrameLimit) must not wait as well. Any thread, one relaxed load.
bool OwnsFrameCap();

// Our frame in flight's XeLL frame ID while XeFG runs on our XeLL, for XeFG to tag and present it with (Intel: one ID
// for both), else 0. Any thread.
uint32_t FrameIdForFrameGeneration();

// DLSS frame generation runs Reflex itself through Streamline (Decision::FrameGenerationOwnsReflex) and the player
// asked for Boost: framegen/dlssg sets Streamline's Reflex mode with Boost.
bool WantsBoostForFrameGeneration();

// XeFG is the output and generates frames: active, not paused, not in real exclusive full screen (Intel: it passes
// frames through there). Whether XeLL does anything on NVIDIA and AMD (NativeLowLatencyRule.h XellIdle). Any thread.
bool XefgGenerating();

// Intel's XeLL guide: all GPU work finished before xellSetSleepMode (R11). Drains the game's D3D12 queue (frame
// generation's game queue, else the current one; the bridges' D3D12 queue for D3D11 and Vulkan games), 250 ms at most.
// Called by fakenvapi's XeLL right before it changes the sleep mode, whoever asked (our low latency, the game's
// Reflex, the frame cap). Any thread.
void WaitForGameQueueIdle();
} // namespace native::lowlatency
