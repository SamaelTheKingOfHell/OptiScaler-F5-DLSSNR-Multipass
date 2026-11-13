// Host check of OutputRule.h: InputCommon's outputs on OptiScaler's terms. No GPU and no game needed.
// cl /std:c++20 /EHsc /W4 /I external/vulkan/include tests/ll_output_rule_smoke.cpp
#include "../OptiScaler/low_latency/input/OutputRule.h"
#include "../OptiScaler/low_latency/input/VkLatencyMode.h"

#include <cstdio>

#include <vulkan/vulkan_core.h>

using namespace lloutput;

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

static bool Same(const SleepMode& a, const SleepMode& b)
{
    return a.lowLatency == b.lowLatency && a.boost == b.boost && a.minimumIntervalUs == b.minimumIntervalUs &&
           a.useMarkersToOptimize == b.useMarkersToOptimize;
}

static SleepMode Mode(bool lowLatency, bool boost, uint32_t intervalUs, bool markers = true)
{
    SleepMode m;
    m.lowLatency = lowLatency;
    m.boost = boost;
    m.minimumIntervalUs = intervalUs;
    m.useMarkersToOptimize = markers;
    return m;
}

int main()
{
    // ---- PlanOutput ----
    {
        OutputInputs in;

        // NVIDIA, real nvapi: the driver's Reflex
        in.realNvapi = true;
        auto p = PlanOutput(in);
        CHECK(p.first() == Tech::Reflex && p.tries[1] == Tech::None && !p.xefgOwnXell);

        // fakenvapi as the nvapi: Anti-Lag 2, then XeLL, then LatencyFlex
        in.realNvapi = false;
        p = PlanOutput(in);
        CHECK(p.tries[0] == Tech::AntiLag2 && p.tries[1] == Tech::XeLL && p.tries[2] == Tech::LatencyFlex);

        // Force LatencyFlex, or more than one generated DLSS FG frame: LatencyFlex only
        in.forceLatencyFlex = true;
        CHECK(PlanOutput(in).first() == Tech::LatencyFlex && PlanOutput(in).tries[1] == Tech::None);
        in.forceLatencyFlex = false;
        in.multiFrame = true;
        CHECK(PlanOutput(in).first() == Tech::LatencyFlex);
        in.multiFrame = false;

        // Frame cap only: ahead of Force LatencyFlex and multi frame (it counts the game's frames), alone, no XeFG XeLL
        in.frameCapOnly = true;
        p = PlanOutput(in);
        CHECK(p.first() == Tech::FrameCap && p.tries[1] == Tech::None && !p.xefgOwnXell);
        in.forceLatencyFlex = true;
        in.multiFrame = true;
        CHECK(PlanOutput(in).first() == Tech::FrameCap);
        in.forceLatencyFlex = false;
        in.multiFrame = false;

        // ... also on a Vulkan device and for an input that can't feed XeLL (Anti-Lag Vk / LatencyFlex otherwise)
        in.vulkan = true;
        CHECK(PlanOutput(in).first() == Tech::FrameCap);
        in.vulkan = false;
        in.inputFeedsXell = false;
        CHECK(PlanOutput(in).first() == Tech::FrameCap);
        in.inputFeedsXell = true;

        // ... never on the driver's Reflex, never with XeFG (it needs XeLL)
        in.realNvapi = true;
        CHECK(PlanOutput(in).first() == Tech::Reflex);
        in.realNvapi = false;
        in.xefgOutput = true;
        CHECK(PlanOutput(in).first() == Tech::XeLL);
        in.xefgOutput = false;
        in.frameCapOnly = false;

        // XeFG: XeLL is the output, XeFG paces with its context
        in.xefgOutput = true;
        p = PlanOutput(in);
        CHECK(p.first() == Tech::XeLL && !p.xefgOwnXell);

        // ... also with Force LatencyFlex or multi frame (XeFG decides)
        in.forceLatencyFlex = true;
        CHECK(PlanOutput(in).first() == Tech::XeLL);
        in.forceLatencyFlex = false;

        // XeFG with Reflex or Anti-Lag 2 lowering latency: that output, XeFG gets its own XeLL
        in.side = XefgLatency::Reflex;
        p = PlanOutput(in);
        CHECK(p.first() == Tech::Reflex && p.xefgOwnXell);
        in.side = XefgLatency::AntiLag2;
        p = PlanOutput(in);
        CHECK(p.first() == Tech::AntiLag2 && p.xefgOwnXell);

        // An input that can't feed XeLL (the UE mod, Anti-Lag 2: XL18): never XeLL as the output
        in.inputFeedsXell = false;
        CHECK(PlanOutput(in).first() == Tech::AntiLag2); // the split is not XeLL's anyway
        in.side = XefgLatency::XeLL;
        p = PlanOutput(in);
        CHECK(p.first() == Tech::LatencyFlex && p.tries[1] == Tech::None && p.xefgOwnXell); // XeFG keeps its own
        in.xefgOutput = false;
        p = PlanOutput(in);
        CHECK(p.tries[0] == Tech::AntiLag2 && p.tries[1] == Tech::LatencyFlex && p.tries[2] == Tech::None);
        in.realNvapi = true;
        CHECK(PlanOutput(in).first() == Tech::Reflex); // Reflex takes any input

        // A Vulkan device: no XeLL without XeFG (fakenvapi: Anti-Lag Vk, then LatencyFlex); Reflex the driver's Vulkan
        // one
        OutputInputs vk;
        vk.vulkan = true;
        p = PlanOutput(vk);
        CHECK(p.tries[0] == Tech::AntiLag2 && p.tries[1] == Tech::LatencyFlex && p.tries[2] == Tech::None);
        vk.realNvapi = true;
        CHECK(PlanOutput(vk).first() == Tech::Reflex && !PlanOutput(vk).xefgOwnXell);
        vk.realNvapi = false;
        vk.forceLatencyFlex = true;
        CHECK(PlanOutput(vk).first() == Tech::LatencyFlex);
        vk.forceLatencyFlex = false;

        // ... XeFG on the Vulkan present bridge: its XeLL (on the bridge's D3D12 device) as on D3D
        vk.xefgOutput = true;
        p = PlanOutput(vk);
        CHECK(p.first() == Tech::XeLL && !p.xefgOwnXell);
        vk.side = XefgLatency::Reflex;
        p = PlanOutput(vk);
        CHECK(p.first() == Tech::Reflex && p.xefgOwnXell);
        vk.side = XefgLatency::AntiLag2;
        CHECK(PlanOutput(vk).first() == Tech::AntiLag2 && PlanOutput(vk).xefgOwnXell);
    }

    // ---- ChooseReflexVk ----
    {
        ReflexVkInputs in;
        CHECK(ChooseReflexVk(in) == ReflexVkSurface::None);

        // VK_NV_low_latency2 on the device: before its first swapchain (ours get the latency mode) and on one that has
        // it
        in.lowLatency2Device = true;
        CHECK(ChooseReflexVk(in) == ReflexVkSurface::LowLatency2);
        in.swapchainKnown = in.swapchainLatencyMode = true;
        CHECK(ChooseReflexVk(in) == ReflexVkSurface::LowLatency2);

        // A swapchain without it (made before our hooks, or the game said no): NVIDIA's NvAPI Vulkan Reflex
        in.swapchainLatencyMode = false;
        CHECK(ChooseReflexVk(in) == ReflexVkSurface::None);
        in.nvapiVulkan = true;
        CHECK(ChooseReflexVk(in) == ReflexVkSurface::None); // its sleep signals a timeline semaphore
        in.timelineSemaphores = true;
        CHECK(ChooseReflexVk(in) == ReflexVkSurface::NvApi);

        // A device without the extension (dxvk's for a D3D game, one made before our hooks)
        ReflexVkInputs nvapi;
        nvapi.nvapiVulkan = nvapi.timelineSemaphores = true;
        CHECK(ChooseReflexVk(nvapi) == ReflexVkSurface::NvApi);
        nvapi.lowLatency2Device = true;
        CHECK(ChooseReflexVk(nvapi) == ReflexVkSurface::LowLatency2); // VK_NV_low_latency2 first

        // ... unless its sleep never signalled there: NVIDIA's NvAPI Vulkan Reflex, else none
        nvapi.lowLatency2Failed = true;
        CHECK(ChooseReflexVk(nvapi) == ReflexVkSurface::NvApi);
        nvapi.nvapiVulkan = false;
        CHECK(ChooseReflexVk(nvapi) == ReflexVkSurface::None);

        // A running VK_NV_low_latency2 output asks it again: its swapchain made again without the latency mode
        ReflexVkInputs running;
        running.lowLatency2Device = running.swapchainKnown = running.swapchainLatencyMode = true;
        CHECK(ChooseReflexVk(running) == ReflexVkSurface::LowLatency2);
        running.swapchainLatencyMode = false;
        CHECK(ChooseReflexVk(running) != ReflexVkSurface::LowLatency2);
    }

    // ---- RouteGameVkReflex ----
    {
        // A device with our semaphore: InputCommon, whatever the driver has
        CHECK(RouteGameVkReflex(true, true) == GameVkReflexRoute::InputCommon);
        CHECK(RouteGameVkReflex(true, false) == GameVkReflexRoute::InputCommon);

        // Set up with the driver before our hooks: the driver as it is; no driver either (fakenvapi): nothing to signal
        CHECK(RouteGameVkReflex(false, true) == GameVkReflexRoute::Driver);
        CHECK(RouteGameVkReflex(false, false) == GameVkReflexRoute::NoSemaphore);
    }

    // ---- ChainLatencyMode ----
    {
        VkSwapchainCreateInfoKHR info {};
        info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        VkSwapchainLatencyCreateInfoNV ours {};

        // A device without VK_NV_low_latency2: untouched
        CHECK(!ChainLatencyMode(false, info, ours) && info.pNext == nullptr);

        // With it: ours at the head of the chain, the rest behind it
        VkImageFormatListCreateInfo other {};
        other.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
        info.pNext = &other;
        CHECK(ChainLatencyMode(true, info, ours));
        CHECK(info.pNext == &ours && ours.pNext == &other && ours.latencyModeEnable == VK_TRUE);
        CHECK(ours.sType == VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV);

        // Again on the bridge's fallback path, from the game's own info: chained once more, not twice
        VkSwapchainCreateInfoKHR again {};
        again.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        again.pNext = &other;
        CHECK(ChainLatencyMode(true, again, ours) && again.pNext == &ours && ours.pNext == &other);

        // The game's own choice stays, on or off, and nothing is added
        VkSwapchainLatencyCreateInfoNV game {};
        game.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV;
        game.pNext = &other;
        game.latencyModeEnable = VK_FALSE;
        VkSwapchainCreateInfoKHR gameInfo {};
        gameInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        gameInfo.pNext = &game;
        VkSwapchainLatencyCreateInfoNV unused {};
        CHECK(!ChainLatencyMode(true, gameInfo, unused) && gameInfo.pNext == &game);
        game.latencyModeEnable = VK_TRUE;
        CHECK(ChainLatencyMode(true, gameInfo, unused) && gameInfo.pNext == &game);
    }

    // ---- RunsLowLatencyUpdate ----
    {
        // A D3D game's own DXGI present, dxvk's too; never the Vulkan bridge's D3D12 output
        CHECK(RunsLowLatencyUpdate(PresentSite::Dxgi, false, -1.0));
        CHECK(!RunsLowLatencyUpdate(PresentSite::Dxgi, true, -1.0));

        // A native Vulkan game (also under Proton, where every DXGI adapter is dxvk's): no game DXGI present ever
        CHECK(RunsLowLatencyUpdate(PresentSite::Vulkan, false, -1.0));

        // dxvk's Vulkan presents of a D3D game's frames: its DXGI present ran the update just before
        CHECK(!RunsLowLatencyUpdate(PresentSite::Vulkan, false, 5.0));
        CHECK(!RunsLowLatencyUpdate(PresentSite::Vulkan, false, kD3dGamePresentGapMs));

        // A game that stopped presenting through DXGI (a launcher's or a video's swapchain): its Vulkan present again
        CHECK(RunsLowLatencyUpdate(PresentSite::Vulkan, false, kD3dGamePresentGapMs + 1.0));
    }

    // ---- MarkerFromVulkanNV / MarkerToVulkanNV ----
    {
        // VkLatencyMarkerNV against NVAPI's NV_LATENCY_MARKER_TYPE (NVAPI has PC_LATENCY_PING at 8)
        static_assert(VK_LATENCY_MARKER_SIMULATION_START_NV == 0 && VK_LATENCY_MARKER_TRIGGER_FLASH_NV == 7);
        static_assert(VK_LATENCY_MARKER_OUT_OF_BAND_RENDERSUBMIT_START_NV == 8 &&
                      VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_END_NV == 11);
        uint32_t nv = 99;
        uint32_t back = 99;

        for (uint32_t vk = 0; vk <= 7; ++vk)
        {
            CHECK(MarkerFromVulkanNV(vk, &nv) && nv == vk);
            CHECK(MarkerToVulkanNV(nv, &back) && back == vk);
        }

        CHECK(MarkerFromVulkanNV(VK_LATENCY_MARKER_OUT_OF_BAND_RENDERSUBMIT_START_NV, &nv) && nv == 9);
        CHECK(MarkerFromVulkanNV(VK_LATENCY_MARKER_OUT_OF_BAND_RENDERSUBMIT_END_NV, &nv) && nv == 10);
        CHECK(MarkerFromVulkanNV(VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_START_NV, &nv) && nv == 11);
        CHECK(MarkerFromVulkanNV(VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_END_NV, &nv) && nv == kNvApiLastMarker);
        CHECK(!MarkerFromVulkanNV(12, &nv));
        CHECK(!MarkerFromVulkanNV(0x7FFFFFFF, &nv));

        for (uint32_t marker = 9; marker <= kNvApiLastMarker; ++marker)
            CHECK(MarkerToVulkanNV(marker, &back) && back == marker - 1);

        CHECK(!MarkerToVulkanNV(kNvApiPcLatencyPing, &back)); // Vulkan has no PC latency ping
        CHECK(!MarkerToVulkanNV(kNvApiLastMarker + 1, &back));
    }

    // ---- ChooseInput ----
    {
        InputChoice in;
        CHECK(ChooseInput(in) == Input::None);

        // The UE mod, then OptiScaler's own while it runs, then the game's Reflex: each stands aside for the next
        in.ue = true;
        CHECK(ChooseInput(in) == Input::UeLowLatency);
        in.optiscalerRunning = true;
        CHECK(ChooseInput(in) == Input::OptiScaler);
        in.reflex = true;
        CHECK(ChooseInput(in) == Input::Reflex);

        // The game's XeLL and Anti-Lag 2 are inputs only with LOW_LATENCY_INPUTS
        InputChoice xell;
        xell.xell = xell.xellLowLatency = xell.xefgInput = true;
        xell.reflex = true;
        CHECK(ChooseInput(xell) == Input::Reflex);
        xell.lowLatencyInputs = true;
        CHECK(ChooseInput(xell) == Input::XeLL); // its XeFG paces with its XeLL
        xell.xefgInput = false;
        CHECK(ChooseInput(xell) == Input::Reflex);
        xell.reflex = false;
        CHECK(ChooseInput(xell) == Input::XeLL);
        xell.xellLowLatency = false;
        xell.antiLag2 = true;
        CHECK(ChooseInput(xell) == Input::AntiLag2);
        xell.antiLag2 = false;
        CHECK(ChooseInput(xell) == Input::XeLL); // low latency off: last, it does nothing until enabled

        InputChoice fsrFg;
        fsrFg.lowLatencyInputs = fsrFg.antiLag2 = fsrFg.antiLag2ReportsFg = fsrFg.reflex = true;
        CHECK(ChooseInput(fsrFg) == Input::AntiLag2); // the game's FSR-FG reports its frames only through it

        // A Vulkan game's own Anti-Lag Vk, in every build: last, after the UE mod
        InputChoice antiLagVk;
        antiLagVk.antiLagVk = true;
        CHECK(ChooseInput(antiLagVk) == Input::AntiLag2);
        antiLagVk.ue = true;
        CHECK(ChooseInput(antiLagVk) == Input::UeLowLatency);
        antiLagVk.optiscalerRunning = true;
        CHECK(ChooseInput(antiLagVk) == Input::OptiScaler);
    }

    // ---- OutputSwitchPending ----
    {
        // Not XeFG, or no output yet: nothing pending
        CHECK(!OutputSwitchPending(false, true, XefgLatency::Reflex, true));
        CHECK(!OutputSwitchPending(true, false, XefgLatency::Reflex, true));

        // The side and the output agree
        CHECK(!OutputSwitchPending(true, true, XefgLatency::XeLL, true));
        CHECK(!OutputSwitchPending(true, true, XefgLatency::Reflex, false));

        // The side moved, the output not yet
        CHECK(OutputSwitchPending(true, true, XefgLatency::Reflex, true));
        CHECK(OutputSwitchPending(true, true, XefgLatency::XeLL, false));

        // The UE mod on the XeLL side runs LatencyFlex: nothing pending
        CHECK(!OutputSwitchPending(true, true, XefgLatency::XeLL, false, false));
    }

    // ---- StreamlineCapUs ----
    {
        // Streamline of OptiScaler's DLSS FG output sets the device's mode too: it carries the output's cap
        CHECK(StreamlineCapUs(true, 16667) == 16667);
        CHECK(StreamlineCapUs(true, 0) == 0);
        CHECK(StreamlineCapUs(false, 16667) == 0);
    }

    // ---- SleepModeFor ----
    {
        const auto game = Mode(true, true, 5000);
        ShapeInputs in;

        // No XeFG: the game's own mode, the cap in place of its interval
        CHECK(Same(SleepModeFor(game, Role::Output, in), game));
        in.capIntervalUs = 13333;
        CHECK(Same(SleepModeFor(game, Role::Output, in), Mode(true, true, 13333)));

        // Force Disable without XeFG: the tech applies it, the mode stays
        in.forceDisabled = true;
        CHECK(Same(SleepModeFor(game, Role::Output, in), Mode(true, true, 13333)));
        in.forceDisabled = false;

        // A game with Reflex off: stays off without XeFG
        const auto gameOff = Mode(false, false, 0);
        CHECK(Same(SleepModeFor(gameOff, Role::Output, in), Mode(false, false, 13333)));

        // Input Off: low latency off, no Boost, no interval
        in.blocked = true;
        CHECK(Same(SleepModeFor(game, Role::Output, in), Mode(false, false, 0)));
        in.blocked = false;

        // XeFG with XeLL: low latency on whatever the game says, the cap in
        in.xefgOutput = true;
        CHECK(Same(SleepModeFor(gameOff, Role::Output, in), Mode(true, false, 13333)));

        // ... unless Force Disable
        in.forceDisabled = true;
        CHECK(Same(SleepModeFor(game, Role::Output, in), Mode(false, true, 13333)));
        in.forceDisabled = false;

        // ... Input Off wins
        in.blocked = true;
        CHECK(Same(SleepModeFor(game, Role::Output, in), Mode(false, false, 0)));
        in.blocked = false;

        // The split (Reflex side): the output gets the game's own with the cap, XeFG's XeLL everything off
        in.side = XefgLatency::Reflex;
        CHECK(Same(SleepModeFor(game, Role::Output, in), Mode(true, true, 13333)));
        CHECK(Same(SleepModeFor(gameOff, Role::Output, in), Mode(false, false, 13333)));
        CHECK(Same(SleepModeFor(game, Role::XefgXell, in), Mode(false, false, 0)));

        // Without a cap the game's interval stays for the side
        in.capIntervalUs = 0;
        CHECK(Same(SleepModeFor(game, Role::Output, in), game));

        // A game that only marks frames: Anti-Lag 2 sleeps on them with low latency on and the cap, Reflex gets its
        // (empty) mode
        in.capIntervalUs = 13333;
        in.inputSetMode = false;
        CHECK(Same(SleepModeFor(Mode(false, false, 0, false), Role::Output, in), Mode(false, false, 13333, false)));
        in.side = XefgLatency::AntiLag2;
        CHECK(Same(SleepModeFor(Mode(false, false, 0, false), Role::Output, in), Mode(true, false, 13333, false)));
        CHECK(Same(SleepModeFor(Mode(false, false, 0, false), Role::XefgXell, in), Mode(false, false, 0, false)));

        // OptiScaler's own mode planned for XeFG's other side (its cap counts that side's frames): low latency stays,
        // no interval and no Boost until it sends one for this side
        ShapeInputs own;
        own.xefgOutput = true;
        own.modeForOtherSide = true;
        const auto ownMode = Mode(true, true, 34483);
        CHECK(Same(SleepModeFor(ownMode, Role::Output, own), Mode(true, false, 0)));
        own.side = XefgLatency::Reflex;
        CHECK(Same(SleepModeFor(ownMode, Role::Output, own), Mode(true, false, 0)));
        CHECK(Same(SleepModeFor(ownMode, Role::XefgXell, own), Mode(false, false, 0)));
        own.modeForOtherSide = false;
        CHECK(Same(SleepModeFor(ownMode, Role::Output, own), ownMode));
    }

    // ---- CapForGame: Frame cap only ----
    {
        GameCapInputs in;
        in.capFps = 60.0f;
        in.markersRecently = true;
        in.outputIsFrameCap = true;

        // It keeps the cap with the game's markers alone (no sleep mode set), at the game's frame rate
        auto c = CapForGame(in);
        CHECK(c.outputHoldsCap && c.fps == 60.0f && !c.showWarning);

        // No cap set: it holds "none" (interval 0), as Reflex does; the limiter has nothing to keep either
        GameCapInputs none = in;
        none.capFps = 0.0f;
        c = CapForGame(none);
        CHECK(c.outputHoldsCap && c.fps == 0.0f && IntervalUs(c.fps) == 0u);

        // FSR FG generating halves it, the game's DLSS FG (fakenvapi) divides by its frames
        in.fsrFgOutput = true;
        in.fgActive = true;
        CHECK(CapForGame(in).fps == 30.0f);
        in.fsrFgOutput = false;
        in.fgActive = false;
        in.dlssgFramesToGenerate = 3;
        CHECK(CapForGame(in).fps == 15.0f);
        in.dlssgFramesToGenerate = 0;

        // Force Disable stops its waits, Input Off and no output running: the limiter (one owner, never none)
        GameCapInputs off = in;
        off.forceDisabled = true;
        CHECK(!CapForGame(off).outputHoldsCap);
        off = in;
        off.blocked = true;
        CHECK(!CapForGame(off).outputHoldsCap);
        off = in;
        off.outputRunning = false;
        CHECK(!CapForGame(off).outputHoldsCap);

        // No marker lately, another frame generation's Reflex: the limiter
        off = in;
        off.markersRecently = false;
        CHECK(!CapForGame(off).outputHoldsCap);
        off = in;
        off.externalFrameGeneration = true;
        CHECK(!CapForGame(off).outputHoldsCap);
    }

    // ---- CapForGame ----
    {
        GameCapInputs in;
        in.capFps = 75.0f;
        in.markersRecently = true;
        in.realNvapi = true;

        // No marker in 20 presents, or another frame generation owns Reflex: the limiter
        GameCapInputs quiet = in;
        quiet.markersRecently = false;
        quiet.gameSetSleepMode = true;
        CHECK(!CapForGame(quiet).outputHoldsCap);

        // Real Reflex, game set low latency on: Reflex keeps it at 75
        in.gameSetSleepMode = true;
        in.gameLowLatencyOn = true;
        auto c = CapForGame(in);
        CHECK(c.outputHoldsCap && c.fps == 75.0f && !c.showWarning);

        // FSR FG generating: halved, and the warning about real Reflex markers
        in.fsrFgOutput = true;
        in.fgActive = true;
        c = CapForGame(in);
        CHECK(c.outputHoldsCap && c.fps == 37.5f && c.showWarning);

        // ... with the game's Reflex off: the limiter keeps it
        in.gameLowLatencyOn = false;
        c = CapForGame(in);
        CHECK(!c.outputHoldsCap && !c.showWarning);

        // ... but OptiScaler's DLSS FG output generating next to the game's Reflex off: Reflex keeps it (its Streamline
        // carries the cap), not handed to the limiter
        in.fsrFgOutput = false;
        c = CapForGame(in);
        CHECK(c.outputHoldsCap && c.fps == 75.0f && !c.showWarning);
        in.fgActive = false;
        CHECK(CapForGame(in).outputHoldsCap); // the same answer on every present

        // A game that never set a sleep mode, no XeFG: the limiter
        GameCapInputs markersOnly = in;
        markersOnly.gameSetSleepMode = false;
        markersOnly.gameSendingMarkers = true;
        CHECK(!CapForGame(markersOnly).outputHoldsCap);

        // XeFG with XeLL: XeLL keeps it while it holds caps (not idle, no Force Disable)
        in.xefgOutput = true;
        in.gameLowLatencyOn = true;
        in.xellHoldsCap = true;
        c = CapForGame(in);
        CHECK(c.outputHoldsCap && c.fps == 75.0f);

        // Intel's library tells XeLL about its frames: the shown cap, generating or not (3x: still 75)
        in.xefgShownPerRendered = 3;
        CHECK(CapForGame(in).fps == 75.0f);

        // ... a library that does not tell XeLL about its frames (xefg_cuda, XL23): per rendered frame (3x: 25)
        in.xellCountsShown = false;
        in.xefgShownPerRendered = 3;
        CHECK(CapForGame(in).fps == 25.0f);
        in.xefgShownPerRendered = 1;
        CHECK(CapForGame(in).fps == 75.0f);
        in.xellCountsShown = true;
        in.xellHoldsCap = false;
        CHECK(!CapForGame(in).outputHoldsCap);

        // ... a game that only marks frames counts too
        markersOnly.xefgOutput = true;
        markersOnly.xellHoldsCap = true;
        CHECK(CapForGame(markersOnly).outputHoldsCap);

        // The split with NVIDIA Reflex: the game's own mode with low latency on, per rendered frame (2x: 37.5)
        in.side = XefgLatency::Reflex;
        in.xefgShownPerRendered = 2;
        c = CapForGame(in);
        CHECK(c.outputHoldsCap && c.fps == 37.5f);
        in.xefgShownPerRendered = 3;
        CHECK(CapForGame(in).fps == 25.0f);

        // ... DLSS-G behind XeFG's API (xefg_cuda Generator=dlssg): the driver multiplies the interval by DLSS-G's
        // multiplier itself, so the shown cap goes in; fakenvapi does not multiply
        {
            const bool real = in.realNvapi;
            in.driverMultipliesCap = true;
            in.realNvapi = true;
            c = CapForGame(in);
            CHECK(c.outputHoldsCap && c.fps == 75.0f);
            // ... no later rule divides it, XeLL's cap-per-rendered rule included
            const bool xellCounts = in.xellCountsShown;
            const bool xellHolds = in.xellHoldsCap;
            in.xellCountsShown = false;
            in.xellHoldsCap = true;
            CHECK(CapForGame(in).fps == 75.0f);
            in.xellCountsShown = xellCounts;
            in.xellHoldsCap = xellHolds;
            in.realNvapi = false;
            CHECK(CapForGame(in).fps == 25.0f);
            in.realNvapi = real;
            // ... Anti-Lag 2 is not Reflex: it still divides
            const auto side = in.side;
            in.side = XefgLatency::AntiLag2;
            in.realNvapi = true;
            CHECK(CapForGame(in).fps == 25.0f);
            in.realNvapi = real;
            in.side = side;
            in.driverMultipliesCap = false;
        }

        // ... not when the game's Reflex is off, nor for a game that only marks frames
        in.gameLowLatencyOn = false;
        CHECK(!CapForGame(in).outputHoldsCap);
        markersOnly.side = XefgLatency::Reflex;
        CHECK(!CapForGame(markersOnly).outputHoldsCap);

        // ... not on the D3D11 bridge (the driver did not hold it there)
        in.gameLowLatencyOn = true;
        in.splitLimiterKeepsCap = true;
        CHECK(!CapForGame(in).outputHoldsCap);
        in.splitLimiterKeepsCap = false;

        // Anti-Lag 2 (fakenvapi) acts on the markers alone
        markersOnly.realNvapi = false;
        markersOnly.side = XefgLatency::AntiLag2;
        CHECK(CapForGame(markersOnly).outputHoldsCap);

        // Input Off: never
        in.blocked = true;
        CHECK(!CapForGame(in).outputHoldsCap);
        in.blocked = false;

        // fakenvapi without XeFG and the game's DLSS FG at 3x: per rendered frame, except with XeLL
        GameCapInputs fake;
        fake.capFps = 120.0f;
        fake.markersRecently = true;
        fake.gameSetSleepMode = true;
        fake.dlssgFramesToGenerate = 2;
        c = CapForGame(fake);
        CHECK(c.outputHoldsCap && c.fps == 40.0f);
        fake.outputIsXell = true;
        CHECK(CapForGame(fake).fps == 120.0f);

        // fakenvapi keeps the cap even with the game's Reflex off
        fake.gameLowLatencyOn = false;
        fake.fgActive = true;
        CHECK(CapForGame(fake).outputHoldsCap);

        // No output running (not started yet, or it could not start): nothing would sleep with it, the limiter keeps it
        fake.outputRunning = false;
        CHECK(!CapForGame(fake).outputHoldsCap);
        in.side = XefgLatency::Reflex;
        in.gameLowLatencyOn = true;
        in.xefgShownPerRendered = 2;
        CHECK(CapForGame(in).outputHoldsCap);
        in.outputRunning = false;
        CHECK(!CapForGame(in).outputHoldsCap);
    }

    // Async markers: the frame generation library's own calls are recognised by module, and only when a library is loaded
    {
        int libraryModule = 0, gameModule = 0;
        CHECK(IsFgLibraryMarker(&libraryModule, &libraryModule));
        CHECK(!IsFgLibraryMarker(&gameModule, &libraryModule));
        CHECK(!IsFgLibraryMarker(nullptr, nullptr));
        CHECK(!IsFgLibraryMarker(&gameModule, nullptr));
    }

    // Input latency: simulation start to GPU done, else to the end of Present, else none
    {
        auto none = InputLatencyFromReport(0, 5000, 9000);
        CHECK(!none.valid);
        auto gpu = InputLatencyFromReport(1000, 30000, 52000); // 51 ms, not the report's span
        CHECK(gpu.valid && gpu.reachesGpu && gpu.ms > 50.99 && gpu.ms < 51.01);
        auto gpuSameAsPresent = InputLatencyFromReport(1000, 4000, 4000);
        CHECK(gpuSameAsPresent.valid && gpuSameAsPresent.reachesGpu && gpuSameAsPresent.ms > 2.99 && gpuSameAsPresent.ms < 3.01);
        auto gpuBeforePresent = InputLatencyFromReport(1000, 30000, 20000);
        CHECK(gpuBeforePresent.valid && !gpuBeforePresent.reachesGpu && gpuBeforePresent.ms > 28.99 && gpuBeforePresent.ms < 29.01);
        auto noGpu = InputLatencyFromReport(1000, 12000, 0);
        CHECK(noGpu.valid && !noGpu.reachesGpu && noGpu.ms > 10.99 && noGpu.ms < 11.01);
        CHECK(!InputLatencyFromReport(5000, 5000, 0).valid);
        CHECK(!InputLatencyFromReport(5000, 0, 0).valid);
        CHECK(!InputLatencyFromReport(5000, 3000, 0).valid);
        CHECK(!InputLatencyFromReport(5000, 3000, 2000).valid);
    }

    // Which reads count toward the shown input latency: a new frame and 500 ms or less; the first read counts
    {
        CHECK(InputLatencySampleCounts(false, 0, 100, 40.0));
        CHECK(InputLatencySampleCounts(true, 100, 101, 40.0));
        CHECK(!InputLatencySampleCounts(true, 100, 100, 40.0));   // the frame did not change (loading screen)
        CHECK(!InputLatencySampleCounts(true, 100, 101, 22233.0)); // too long
        CHECK(!InputLatencySampleCounts(false, 0, 100, 500.5));
        CHECK(InputLatencySampleCounts(true, 100, 101, 500.0));
    }

    static_assert(IntervalUs(75.0f) == 13333);
    static_assert(IntervalUs(37.5f) == 26667);
    static_assert(IntervalUs(0.0f) == 0);
    static_assert(PlanOutput(OutputInputs { true }).first() == Tech::Reflex);

    if (fails == 0)
        printf("ll_output_rule_smoke: PASS\n");

    return fails == 0 ? 0 : 1;
}
