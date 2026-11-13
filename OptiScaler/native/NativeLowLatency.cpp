#include "pch.h"

#include "NativeLowLatency.h"
#include "DepthFinderCore.h"
#include "NativeDriver.h"

#include <Config.h>
#include <State.h>

#include <dlssnr/DlssNr_NativeMode.h>
#include <framegen/IFGFeature_Dx12.h>
#include <hooks/LibraryLoad_Hooks.h>
#include <hooks/Reflex_Hooks.h>
#include <hooks/Vulkan_Hooks.h>
#include <hooks/Xell_Hooks.h>
#include <low_latency/input/input_common.h>
#include <misc/FrameLimit.h>
#include <misc/IdentifyGpu.h>
#include <nvapi/NvApiHooks.h>
#include <nvapi/fakenvapi.h>
#include <proxies/XeFG_Proxy.h>

#include <magic_enum.hpp>

#include <atomic>
#include <mutex>
#include <optional>

namespace native::lowlatency
{

namespace
{
enum class Table
{
    Unknown,
    Found,
    Missing
};

std::atomic<bool> g_running = false;    // markers are being sent
std::atomic<bool> g_submitSent = false; // this frame's SIMULATION_END / RENDERSUBMIT_START went out
// `device` is a D3D11/D3D12 device (the game's, or the Vulkan bridge's private D3D12 device) or -- Api::Vulkan -- the
// game's VkDevice reinterpreted as the same pointer-sized token: never dereferenced as a COM interface here, only
// passed back to the Reflex calls of its own API. Every call carries its Api; nothing is remembered between calls but
// the Api each stored device was seen with (for the callers that bring no device: the first submit, the status line).
// The two go together under one lock: those callers run on other threads, and a VkDevice read with Api::D3D would be
// asked for COM interfaces.
struct DeviceRef
{
    Api api = Api::D3D;
    IUnknown* device = nullptr;
};

std::mutex g_deviceMutex;
DeviceRef g_deviceRef;

void SetDevice(Api api, IUnknown* device)
{
    std::lock_guard lock(g_deviceMutex);
    g_deviceRef = { api, device };
}

DeviceRef CurrentDevice()
{
    std::lock_guard lock(g_deviceMutex);
    return g_deviceRef;
}

void ForgetDevice(IUnknown* device)
{
    std::lock_guard lock(g_deviceMutex);

    if (g_deviceRef.device == device)
        g_deviceRef.device = nullptr;
}

std::atomic<Api> g_sleepModeApi = Api::D3D;
std::atomic<IUnknown*> g_sleepModeDevice = nullptr;   // the device our Reflex mode was set on
std::atomic<const void*> g_gameQueue = nullptr;       // the queue the game's frame is submitted to (D3D12)
std::atomic<double> g_frameGenerationPresentAt = 0.0; // last PresentSource::FrameGeneration call, ms
std::atomic<Decision> g_decision = Decision::NoF5Low;
std::atomic<CapOwner> g_capOwner = CapOwner::NotOurs; // PlanCap's answer on the last present we ran on
std::atomic<bool> g_xellIdle = false;                 // XellIdle on the last present we ran on
std::string g_capLogged;                              // the cap line PresentEnd last logged; cleared to say it again
std::atomic<RunsFor> g_runsFor = RunsFor::F5Low;      // what Auto runs us for (RunReason): log and menu name it
// XeLL and XeFG share one frame ID (framegen/xefg/XeFGFrameId.h): the ID of our frame in flight while XeFG runs on our
// XeLL (0: not)
std::atomic<uint32_t> g_xellFrameIdForFrameGeneration = 0;

// Only the game's present thread touches these.
Table g_table = Table::Unknown;
uint64_t g_frameId = 0;
bool g_presentStarted = false; // PRESENT_START of the frame in flight went out, PRESENT_END is owed
bool g_sleepModeSent = false;  // we turned Reflex on
// What our last SetSleepMode carried (NeedsSleepModeResend), whether the method refused it, and when it went out
std::optional<SleepModeCall> g_sent;
bool g_sentRefused = false;
double g_sentAt = 0.0;
CapSettle g_capSettle; // the cap plan in force (SettleCap)

// GetStatus
double g_latencyAskedAt = 0.0;
bool g_hasLatency = false;
float g_latencyMs = 0.0f;
bool g_latencyEstimated = false;

// Our calls are InputCommon's OptiScaler input (low_latency/input): its output lowers latency on OptiScaler's terms,
// also with XeFG. Not a lasting input like the game's (InputCommon::set_optiscaler_running). On a native Vulkan game's
// VkDevice (or dxvk's, in XeFG's split) the same input with the device as a token: the output is the driver's Vulkan
// Reflex, Anti-Lag Vk or LatencyFlex.
const InputContext kOwnContext { .caller = LowLatencyInput::OptiScaler,
                                 .localContext = true,
                                 .noFrameId = false,
                                 .markerMode = InputMarkerMode::FullMarkers };

const InputContext kOwnContextVk { .caller = LowLatencyInput::OptiScaler,
                                   .localContext = true,
                                   .noFrameId = false,
                                   .markerMode = InputMarkerMode::FullMarkers,
                                   .api = API::Vulkan };

const InputContext& OwnContext(Api api) { return api == Api::Vulkan ? kOwnContextVk : kOwnContext; }

void Marker(Api api, IUnknown* device, NV_LATENCY_MARKER_TYPE type)
{
    // The hooks' bookkeeping (the frame IDs, the timings), as for the game's: not counted as the game's
    if (api == Api::Vulkan)
    {
        ReflexHooks::preVkMarker(reinterpret_cast<HANDLE>(device), g_frameId, (uint32_t) type, false);
    }
    else
    {
        NV_LATENCY_MARKER_PARAMS params {};
        params.version = NV_LATENCY_MARKER_PARAMS_VER;
        params.frameID = g_frameId;
        params.markerType = type;

        NvAPI_Status ignored = NVAPI_OK;
        ReflexHooks::preMarker(device, &params, false, nullptr, &ignored);
    }

    MarkerParams marker {};
    marker.frame_id = g_frameId;
    marker.marker_type = (MarkerType) type; // the enums match
    InputCommon::set_marker(OwnContext(api), device, marker);
}

// `minimumIntervalUs`: the frame cap when we keep it (PlanCap), else 0 and OptiScaler's own limiter keeps it.
// ReflexHooks never puts its stored cap into our calls.
NvAPI_Status SetSleepMode(Api api, IUnknown* device, bool on, uint32_t minimumIntervalUs = 0, bool boost = false)
{
    SleepMode mode {};
    mode.low_latency_enabled = on;
    mode.low_latency_boost = on && boost;
    mode.minimum_interval_us = on ? minimumIntervalUs : 0;
    // The markers are approximate (no engine hooks), so the driver must not tune itself to them
    mode.use_markers_to_optimize = false;
    mode.use_min_queue_time = false;

    // Refused: no output could start (the limiter keeps the cap then), or the game's Reflex is the input now
    return InputCommon::set_sleep_mode(OwnContext(api), device, &mode) == InputResult::Ok ? NVAPI_OK : NVAPI_ERROR;
}

// The Reflex function table, from the nvapi the game would have loaded: Hook loads it like the game's LoadLibrary
// would (the real nvapi64.dll, or OptiScaler's own fakenvapi when there is no NVIDIA card or no real nvapi) and hooks
// its query function, so a game that loads nvapi later finds it already done. NvAPI_Initialize is counted, so ours is
// harmless next to the game's.
bool FindTable()
{
    if (ReflexHooks::isReflexHooked())
        return true;

    if (NvApiHooks::o_NvAPI_QueryInterface == nullptr)
        LibraryLoadHooks::LoadNvApi();

    auto queryInterface = NvApiHooks::o_NvAPI_QueryInterface;

    if (queryInterface == nullptr)
        return false;

    if (auto init = GET_INTERFACE(NvAPI_Initialize, queryInterface))
        init();

    return ReflexHooks::ensureTable(queryInterface);
}

bool ForceReflexDisabled()
{
    return Config::Instance()->FN_ForceReflex.value_or_default() == ForceReflex::ForceDisable;
}

Inputs GatherInputs()
{
    auto config = Config::Instance();
    auto& state = State::Instance();

    Inputs in;
    const auto shown = DlssNrNativeMode::FromKeys(
        { config->DlssNrNativeDepthFinder.value_or_default(), config->DlssNrNativeMotion.value_or_default(),
          config->DlssNrNativeInput.value_or_default(), config->DlssNrNativeUpscaler.value_or_default() });
    const bool gameUpscaler = GameUpscalerCalledRecently();
    const bool virtualUpscaler = shown == DlssNrNativeMode::Shown::NrAndFrameGeneration && !gameUpscaler;
    const auto input = EffectiveInput(config->FramerateLowLatencyInput.value_or_default(), virtualUpscaler);
    const auto rule = RuleForInput(input);
    in.setting = rule.setting;
    in.blockGameLowLatency = rule.blockGameLowLatency && !FrameGenerationNeedsReflex();

    // Optical F5Low's modes only: the Vulkan route (frame generation from the game's upscaler) is not Optical F5Low,
    // and with no upscaler call (menus) it has nothing running to pace.
    // Idle (nothing uses the motion: NativeDriver runs none of it) is not running either.
    in.f5lowRunning = shown != DlssNrNativeMode::Shown::Off && !gameUpscaler && NativeDriver::MotionUsed();
    in.xefgRunning = UsesXeFGApi(state.activeFgOutput) && state.currentFG != nullptr &&
                     state.currentFG->SwapchainContext() != nullptr;
    // While it is on. Its short pauses (toggles, a changed interpolation count, swap chain remakes, about ten frames) do
    // not count: stopping and starting again for them would change the sleep mode twice and hand the cap back and forth
    // (XeFG's check does not look at the pause either). The Vulkan present bridge is left out: FSR FG there is untested
    // with us.
    in.fsrFgRunning = state.activeFgOutput == FGOutput::FSRFG && state.currentFG != nullptr &&
                      state.currentFG->IsActive() &&
                      (state.swapchainInteropApi == SwapchainInteropApi::None ||
                       state.swapchainInteropApi == SwapchainInteropApi::Dx11wDx12);

    in.gameCallsReflex = ReflexHooks::gameCalledReflex();
    in.gameRunsXell = XellHooks::canLimit();
    in.forceReflexDisabled = ForceReflexDisabled();
    in.forceXell = config->ForceXeLL.value_or_default() || state.activeFgInput == FGInput::ForceXeLL;
    in.otherFrameGenerationOwner = FrameGenerationNeedsReflex();
    in.apiAvailable = g_table != Table::Missing;
    return in;
}

Decision Evaluate(Api api, IUnknown* device)
{
    const auto inputs = GatherInputs();
    auto decision = Decide(inputs);
    // Run for frame generation alone (no Optical F5Low mode): the menu and log say so
    g_runsFor = RunReason(inputs);

    if (decision == Decision::Run && g_table == Table::Unknown)
    {
        g_table = FindTable() ? Table::Found : Table::Missing;

        if (g_table == Table::Missing)
            LOG_WARN("Optical F5Low low latency: no Reflex or fakenvapi interface found, it stays off");

        decision = Decide(GatherInputs());
    }

    return decision;
}

// Our Reflex mode is turned off again, unless the game has set its own (its call is the later word)
void Stop(Api api, IUnknown* device)
{
    g_running = false;
    g_xellFrameIdForFrameGeneration = 0;
    g_capOwner = CapOwner::NotOurs;
    g_xellIdle = false;
    g_capLogged.clear();
    g_sent.reset();
    g_sentRefused = false;
    g_capSettle = {};

    if (g_presentStarted && device != nullptr && !ReflexHooks::gameCalledReflex())
        Marker(api, device, PRESENT_END);

    g_presentStarted = false;

    if (auto sleepModeDevice = g_sleepModeDevice.exchange(nullptr);
        g_sleepModeSent && sleepModeDevice != nullptr && !ReflexHooks::gameCalledSetSleepMode())
        SetSleepMode(g_sleepModeApi.load(), sleepModeDevice, false);

    g_sleepModeSent = false;

    // After the "off": InputCommon's output goes on with the input whose turn it is now
    InputCommon::set_optiscaler_running(false);
}

// With frame generation's swap chain in place, its presents are the game's and the wrapped swap chain's are frame
// generation's own (real and generated frames, on its thread)
bool Ignored(PresentSource source)
{
    const auto now = Util::MillisecondsNow();

    if (source == PresentSource::FrameGeneration)
    {
        g_frameGenerationPresentAt = now;
        return false;
    }

    return SwapChainPresentIgnored(now, g_frameGenerationPresentAt.load(std::memory_order_relaxed));
}

const void* GameQueue()
{
    auto& state = State::Instance();

    // Frame generation's swap chain can leave State::currentCommandQueue on its own queue; it knows the game's
    if (auto* fg12 = dynamic_cast<IFGFeature_Dx12*>(state.currentFG);
        fg12 != nullptr && fg12->GameCommandQueue() != nullptr)
        return fg12->GameCommandQueue();

    return state.currentCommandQueue;
}

// XeFG paces with its XeLL on a D3D12 device: a Vulkan game has XeFG only on the present bridge (bridgedXellDevice).
// With Reflex or Anti-Lag 2 as XeFG's Output (XefgSide) XeLL still carries the frame IDs, and a bridged Vulkan game's
// calls go to its VkDevice (PresentTarget): the Vulkan ones count then too.
Method CurrentMethod(Api api)
{
    if (UsesXeFGApi(State::Instance().activeFgOutput) &&
        (api == Api::D3D || ReflexHooks::bridgedXellDevice() != nullptr))
        return MethodWithXefg(XefgSide());

    return fakenvapi::isUsingAsMainNvapi() ? Method::Fakenvapi : Method::RealReflex;
}

CapInputs FrameCapInputs(Api api)
{
    auto& state = State::Instance();
    // Generating, not merely on: FSR FG waiting for frame data (FG only with no upscaler call) shows the game's frames
    // alone, and the cap must not be halved for frames it does not make (IFGFeature::IsGenerating).
    const bool fgGenerating = state.currentFG != nullptr && state.currentFG->IsGenerating();

    CapInputs in;
    in.running = true;
    in.capFps = FrameLimit::CapFps();
    in.method = CurrentMethod(api);
    in.fsrFgActive = state.activeFgOutput == FGOutput::FSRFG && fgGenerating;
    in.xefgGenerating = XefgGenerating();
    in.shownPerRendered = XefgShownPerRendered();
    in.xellCountsShown = XellCountsShown();
    in.driverMultipliesCap = DriverMultipliesReflexCap();
    in.xefgVsync = state.fgPresentVsync.load(std::memory_order_relaxed);
    in.fakenvapiXell = in.method == Method::Fakenvapi && InputCommon::current_mode() == LowLatencyMode::XeLL;

    // Once: getPrimaryGpu copies the GPU list, and the primary GPU does not change while the game runs
    static const bool intelGpu = IdentifyGpu::getPrimaryGpu().vendorId == VendorId::Intel;
    in.intelGpu = intelGpu;
    // FGHooks::FGPresent's limiter waits before our OnPresentEnd there; the Vulkan bridge's D3D12 present (and its
    // limiter) comes after the game's present, our Sleep included. Read with FSR FG on real Reflex; d3d11Bridge is the
    // separate fact that decides who keeps the cap beside NVIDIA Reflex and XeFG (SplitLimiterKeepsCap).
    in.limiterBeforeSleep = state.swapchainInteropApi != SwapchainInteropApi::VkwDx12;
    in.d3d11Bridge = state.swapchainInteropApi == SwapchainInteropApi::Dx11wDx12;
    return in;
}

// Run for frame generation alone (no Optical F5Low mode), the log does not name Optical F5Low
const char* LogName()
{
    switch (g_runsFor.load())
    {
    case RunsFor::Xefg:
        // In the split XeLL only stays for XeFG; Reflex or Anti-Lag 2 is fed
        return XefgSide() == XefgLatency::XeLL ? "Low latency (feeding XeFG's XeLL)" : "Low latency (for XeFG)";
    case RunsFor::FsrFg:
        return "Low latency (for FSR frame generation)";
    default:
        return "Optical F5Low low latency";
    }
}

std::string CapText(float capFps)
{
    return capFps > 0.0f ? std::format("frame cap {:.0f} fps", capFps) : std::string("no frame cap");
}

const char* PathText(Api api)
{
    const auto method = CurrentMethod(api);
    // Anti-Lag 2's Vulkan form is Anti-Lag Vk
    const bool vulkan = api == Api::Vulkan;

    // XeFG's XeLL is not the method in the split: it only keeps XeFG running
    if (method == Method::RealReflexWithXefg)
        return "NVIDIA Reflex";

    if (method == Method::FakenvapiWithXefg)
        return vulkan ? "Anti-Lag Vk" : "Anti-Lag 2";

    if (method == Method::RealReflex)
        return "NVIDIA Reflex";

    switch (InputCommon::current_mode())
    {
    case LowLatencyMode::AntiLag2:
        return vulkan ? "Anti-Lag Vk" : "Anti-Lag 2";
    case LowLatencyMode::LatencyFlex:
        return "LatencyFlex";
    case LowLatencyMode::XeLL:
        return "XeLL";
    case LowLatencyMode::AntiLagVk:
        return "Anti-Lag Vk";
    case LowLatencyMode::Reflex:
        return "NVIDIA Reflex";
    case LowLatencyMode::FpsLimiter:
        return "Frame cap only";
    default:
        return fakenvapi::isUsingAsMainNvapi() ? "no method active yet" : "NVIDIA Reflex";
    }
}

// What lowers latency here, whoever makes the calls (the menu's "Active: <input> -> <output>"). PathText names the method
// our own calls reach; XeFG, Force XeLL and a game's own XeLL come first because they decide it.
const char* OutputText(Api api)
{
    // With XeFG and Reflex or Anti-Lag 2 as its Output: they lower latency, XeLL stays for XeFG
    if (UsesXeFGApi(State::Instance().activeFgOutput))
    {
        switch (XefgSide())
        {
        case XefgLatency::Reflex:
            return "NVIDIA Reflex (XeLL stays for XeFG)";
        case XefgLatency::AntiLag2:
            return "Anti-Lag 2 (XeLL stays for XeFG)";
        default:
            return "XeLL";
        }
    }

    if (State::Instance().activeFgInput == FGInput::ForceXeLL)
        return "XeLL";

    if (XellHooks::canLimit())
        return "XeLL (the game's)";

    return PathText(api);
}

void PresentBegin(Api api, IUnknown* device, PresentSource source)
{
    // Ignored first: a FrameGeneration call must be noted even before we run, ahead of frame generation's own presents
    if (Ignored(source) || !g_running.load(std::memory_order_relaxed) || device == nullptr ||
        ReflexHooks::gameCalledReflex())
    {
        return;
    }

    SetDevice(api, device);

    // No submit hook fired (D3D12 sees it only where the queue hook is installed): the frame's work is all behind us
    if (!g_submitSent.exchange(true))
    {
        Marker(api, device, SIMULATION_END);
        Marker(api, device, RENDERSUBMIT_START);
    }

    Marker(api, device, RENDERSUBMIT_END);
    Marker(api, device, PRESENT_START);
    g_presentStarted = true;
}

void PresentEnd(Api api, IUnknown* device, PresentSource source)
{
    if (device == nullptr || State::Instance().isShuttingDown || Ignored(source))
        return;

    const auto decision = Evaluate(api, device);
    const auto previousDecision = g_decision.exchange(decision);

    if (decision != Decision::Run)
    {
        const auto reason = [decision]()
        {
            const auto side = XefgSide();
            return DecisionText(decision, ReflexHooks::gameReflexFeedsXell() && side == XefgLatency::XeLL,
                                ForceReflexDisabled(), side);
        };

        // Once whenever it changes (a running one logs its stop below)
        if (decision != previousDecision && !g_running.load(std::memory_order_relaxed))
            LOG_INFO("{}: not running ({})", LogName(), reason());

        if (g_running.load(std::memory_order_relaxed))
        {
            LOG_INFO("{}: stopped ({})", LogName(), reason());

            // XeLL's frame IDs go on from the game's own now, which can be below ours (Intel: they rise)
            if (decision == Decision::GameRunsReflex && g_xellFrameIdForFrameGeneration.load() != 0)
                LOG_WARN("{}: the game's own Reflex now feeds XeFG's XeLL; its frame IDs may restart below ours",
                         LogName());

            Stop(api, device);
        }

        return;
    }

    // Our calls are InputCommon's OptiScaler input from the first one on. Under dxvk they move to its Vulkan device
    // when XeFG's Output becomes Reflex or Anti-Lag 2 (PresentTarget): InputCommon restarts its output on that device's
    // API.
    InputCommon::set_optiscaler_running(true);

    // A refusal belongs to the device it came from: a new device (or the old one released) starts over
    if (g_sent && (g_sent->device != device || g_sent->api != api || g_sleepModeDevice.load() != device))
    {
        g_sent.reset();
        g_sentRefused = false;
    }

    auto capInputs = FrameCapInputs(api);
    const auto method = capInputs.method;

    // The plan in force: a new one once it has held for a few presents (frame generation's short pauses), XeFG's
    // count steps at once
    const auto planned = PlanCap(capInputs);
    const auto cap = SettleCapForCount(g_capSettle, planned, method, capInputs.shownPerRendered);

    SleepModeCall call;
    call.api = api;
    call.device = device;
    call.minimumIntervalUs = cap.minimumIntervalUs;
    // Only NVIDIA's Reflex has Boost; sent to XeLL it would only be a needless sleep mode change
    call.boost = (method == Method::RealReflex || method == Method::RealReflexWithXefg) &&
                 Config::Instance()->DlssNrNativeLowLatencyBoost.value_or_default();
    // XeFG makes a new XeLL context (and sets its sleep mode, with no limit) when its swap chain is made again; in the
    // split that one has to be told low latency off again
    call.xellGeneration = CarriesXefgFrameIds(method) ? InputCommon::xefg_xell_generation() : 0;
    // A changed Output sends the mode again, to the new side: InputCommon gives the new side no interval and no Boost
    // until it comes (OutputRule.h ShapeInputs::modeForOtherSide)
    call.side = (uint32_t) XefgSide();

    const auto now = Util::MillisecondsNow();

    if (NeedsSleepModeResend(g_sent ? &*g_sent : nullptr, g_sentRefused, call, now - g_sentAt >= kSleepModeRetryMs))
    {
        const bool wasRefused = g_sentRefused;
        const bool changed =
            !g_sent || g_sent->minimumIntervalUs != call.minimumIntervalUs || g_sent->boost != call.boost;
        const bool newXellContext = g_sent && g_sent->xellGeneration != call.xellGeneration;
        const auto result = SetSleepMode(api, device, true, call.minimumIntervalUs, call.boost);

        g_sleepModeSent = true;
        g_sleepModeApi = api;
        g_sleepModeDevice = device;
        g_sent = call;
        g_sentAt = now;
        g_sentRefused = result != NVAPI_OK;

        if (!g_running.load(std::memory_order_relaxed))
            LOG_INFO("{}: started ({})", LogName(), PathText(api));

        // Refused: the latency calls still go out, but the method never took the cap, so the limiter keeps it
        if (g_sentRefused && !wasRefused)
        {
            LOG_WARN("{}: SetSleepMode refused ({}), OptiScaler's limiter keeps the frame cap", LogName(),
                     magic_enum::enum_name(result));
        }
        else if (!g_sentRefused && newXellContext && !changed)
        {
            LOG_INFO("{}: {} sent again, XeFG made a new XeLL context", LogName(), CapText(capInputs.capFps));
        }
        else if (!g_sentRefused && (changed || wasRefused))
        {
            g_capLogged.clear(); // said below
        }
    }

    // Who keeps the cap, whenever it or the cap changes: the limiter's changes reach no sleep mode
    if (!g_sentRefused)
    {
        const bool xellVsync = capInputs.method == Method::XellWithXefg && !XellKeepsCap(capInputs.xefgVsync);
        auto text = std::format("{} in {}{}, Boost {}", CapText(capInputs.capFps),
                                cap.owner == CapOwner::Limiter ? "OptiScaler's limiter" : PathText(api),
                                cap.owner == CapOwner::Limiter && xellVsync && capInputs.capFps > 0.0f
                                    ? " (XeLL goes by the refresh rate with V-Sync)"
                                    : "",
                                call.boost ? "on" : "off");

        if (text != g_capLogged)
        {
            LOG_INFO("{}: {}", LogName(), text);
            g_capLogged = std::move(text);
        }
    }

    // XeLL doing nothing cannot hold the cap for the settle window either: the limiter takes it at once (XeLL's interval
    // waits for the settled plan, it does nothing meanwhile anyway)
    g_capOwner = OwnerAfterCall(XellIdle(capInputs) ? planned : cap, g_sentRefused);
    g_xellIdle = XellIdle(capInputs);

    if (g_presentStarted)
        Marker(api, device, PRESENT_END);

    g_presentStarted = false;
    SetDevice(api, device);
    g_gameQueue = GameQueue();

    if (api != Api::Vulkan)
    {
        NvAPI_Status ignored = NVAPI_OK;
        ReflexHooks::preSleep(device, false, &ignored);
    }

    InputCommon::sleep(OwnContext(api), device);

    // XeFG takes this ID for the frame it tags next (FrameIdForFrameGeneration)
    ++g_frameId;
    g_xellFrameIdForFrameGeneration = CarriesXefgFrameIds(method) ? (uint32_t) g_frameId : 0;
    g_submitSent = false;
    g_running = true;
    Marker(api, device, SIMULATION_START);
}
} // namespace

void OnPresentBegin(IUnknown* device, PresentSource source)
{
    PresentBegin(Api::D3D, device, source);
}

void OnPresentEnd(IUnknown* device, PresentSource source)
{
    PresentEnd(Api::D3D, device, source);
}

void OnPresentBeginVulkan(VkDevice device)
{
    PresentBegin(Api::Vulkan, reinterpret_cast<IUnknown*>(device), PresentSource::SwapChain);
}

void OnPresentEndVulkan(VkDevice device)
{
    PresentEnd(Api::Vulkan, reinterpret_cast<IUnknown*>(device), PresentSource::SwapChain);
}

namespace
{
std::atomic<DWORD> g_dxvkGameThread = 0;   // the thread of the game's D3D11 Present under dxvk (Vulkan bridge)
std::atomic<DWORD> g_dxvkVulkanThread = 0; // the thread of dxvk's vkQueuePresentKHR

void LogDxvkThreads()
{
    const auto game = g_dxvkGameThread.load();
    const auto vulkan = g_dxvkVulkanThread.load();

    if (game == 0 || vulkan == 0)
        return;

    if (game == vulkan)
        LOG_INFO("Low latency (dxvk): the game and dxvk present to Vulkan on the same thread ({})", game);
    else
        LOG_INFO("Low latency (dxvk): the game presents on thread {}, dxvk presents to Vulkan on thread {}", game,
                 vulkan);
}
} // namespace

void NoteDxvkGamePresentThread()
{
    const auto thread = GetCurrentThreadId();

    if (g_dxvkGameThread.exchange(thread, std::memory_order_relaxed) != thread)
        LogDxvkThreads();
}

void NoteDxvkVulkanPresentThread()
{
    const auto thread = GetCurrentThreadId();

    if (g_dxvkVulkanThread.exchange(thread, std::memory_order_relaxed) != thread)
        LogDxvkThreads();
}

namespace
{
// The devices the Vulkan present published, as ONE unit: the bridge's D3D12 device and the game's VkDevice (as a token).
// Which one the game's Present uses is chosen at the game's Present from the Output of that moment, so a change of
// XeFG's Output never uses the last frame's choice, and no call goes to the wrong API's device.
struct DxvkDevices
{
    IUnknown* device12 = nullptr;
    IUnknown* vk = nullptr;
};

std::mutex g_dxvkDevicesMutex;
DxvkDevices g_dxvkDevices;

std::mutex g_dxvkLedgerMutex;
FrameLedger g_dxvkLedger;
HWND g_dxvkLedgerWindow = nullptr;

// The XeLL frame ID of the frame the Vulkan present in flight is presenting, when the game's Present paired it (else 0)
std::atomic<uint32_t> g_dxvkBridgeFrameId = 0;

// -1 not decided yet, 0 the Vulkan present runs the pair, 1 the game's Present does: only to log a change
std::atomic<int> g_dxvkSite = -1;

std::mutex g_dxvkSiteMutex;

DxvkDevices DxvkDevicesNow()
{
    std::lock_guard lock(g_dxvkDevicesMutex);
    return g_dxvkDevices;
}
} // namespace

DxvkSiteLock::DxvkSiteLock(bool take) : _held(take)
{
    if (_held)
        g_dxvkSiteMutex.lock();
}

DxvkSiteLock::~DxvkSiteLock()
{
    if (_held)
        g_dxvkSiteMutex.unlock();
}

bool DxvkVulkanPresent(VkDevice vkDevice, IUnknown* device12, uint32_t generation)
{
    NoteDxvkVulkanPresentThread();

    {
        std::lock_guard lock(g_dxvkDevicesMutex);
        g_dxvkDevices = { device12, reinterpret_cast<IUnknown*>(vkDevice) };
    }

    FrameLedger::Entry entry;
    bool popped = false;
    bool resync = false;

    {
        std::lock_guard lock(g_dxvkLedgerMutex);
        popped = g_dxvkLedger.Pop(&entry, generation, Util::MillisecondsNow());
        resync = g_dxvkLedger.TakeResync();
    }

    static std::atomic<bool> resyncLogged = false;

    if (resync && !resyncLogged.exchange(true))
    {
        LOG_WARN("Low latency (dxvk): the game's presents and dxvk's Vulkan presents got out of step (a present without its "
                 "pair, or the bridge made again); the frame list was trimmed and goes on from here");
    }

    // Nothing queued: the Vulkan present runs the pair, as it did before the game's Present could
    g_dxvkBridgeFrameId = popped && entry.site == SleepSite::GamePresent ? entry.frameId : 0;
    return !popped || entry.site == SleepSite::VulkanPresent;
}

void DxvkBridgeDown()
{
    // The game presents with no output up: what is queued is for frames that dxvk presents unbridged, with no pop
    std::lock_guard lock(g_dxvkLedgerMutex);
    g_dxvkLedger.Clear();
    g_dxvkLedgerWindow = nullptr;
    g_dxvkBridgeFrameId = 0;
}

DxvkGameScope OnDxvkGamePresentBegin(bool keyOn, HWND window, uint32_t generation)
{
    NoteDxvkGamePresentThread();

    DxvkGameScope scope;

    {
        // The bridge went away or came back (another window): what is queued belongs to the old one
        std::lock_guard lock(g_dxvkLedgerMutex);

        if (g_dxvkLedgerWindow != window)
        {
            g_dxvkLedger.Clear();
            g_dxvkLedgerWindow = window;
            g_dxvkBridgeFrameId = 0;
        }
    }

    // The API from the Output now, the same rule as the Vulkan present's (PresentTarget)
    const auto devices = DxvkDevicesNow();
    const bool splitOnVulkan = XefgSide() != XefgLatency::XeLL;
    const auto target = PresentTarget(true, true, devices.vk, devices.device12, splitOnVulkan);
    const bool gamePresent = SleepSiteFor(true, keyOn) == SleepSite::GamePresent && target.device != nullptr;

    if (g_dxvkSite.exchange(gamePresent ? 1 : 0) != (gamePresent ? 1 : 0))
    {
        LOG_INFO("Low latency (dxvk): the Reflex calls run at {}",
                 gamePresent ? "the game's Present, on the game's thread" : "dxvk's Vulkan present, on dxvk's thread");
    }

    if (!gamePresent)
    {
        std::lock_guard lock(g_dxvkLedgerMutex);
        scope.seq = g_dxvkLedger.Push(0, SleepSite::VulkanPresent, generation, Util::MillisecondsNow());
        scope.pushed = true;
        return scope;
    }

    scope.began = true;
    scope.api = target.api;
    scope.device = const_cast<IUnknown*>(static_cast<const IUnknown*>(target.device));
    scope.pushed = true;

    // The entry's ID is the one the markers carry (0 when nobody feeds XeLL): XeFG takes it when the bridge presents the
    // frame. Read under the site lock (NativeLowLatencyRule.h BeginGameSiteFrame).
    scope.seq = BeginGameSiteFrame([] { return DxvkSiteLock(true); }, [] { return g_xellFrameIdForFrameGeneration.load(); },
                                   [&](uint32_t id)
                                   {
                                       std::lock_guard lock(g_dxvkLedgerMutex);
                                       return g_dxvkLedger.Push(id, SleepSite::GamePresent, generation, Util::MillisecondsNow());
                                   },
                                   [&] { PresentBegin(scope.api, scope.device, PresentSource::SwapChain); });
    return scope;
}

void OnDxvkGamePresentEnd(const DxvkGameScope& scope, HRESULT presentResult)
{
    if (scope.began)
    {
        // dxvk can remake its Vulkan swapchain inside this very Present, and the bridge then lets go of its D3D12 device
        // (OnDeviceReleased): ending the pair on it would store a device that is gone
        const auto devices = DxvkDevicesNow();
        DxvkSiteLock lock(true);

        if (scope.device == devices.device12 || scope.device == devices.vk)
        {
            PresentEnd(scope.api, scope.device, PresentSource::SwapChain);
        }
        else
        {
            // No call to the gone device, but the pair is not left half way and the frame ID moves on (AbandonPair)
            AbandonPair(g_presentStarted, g_submitSent, g_frameId);

            if (g_xellFrameIdForFrameGeneration.load() != 0)
                g_xellFrameIdForFrameGeneration = static_cast<uint32_t>(g_frameId);
        }
    }

    // Occluded or failed: dxvk presents nothing for this frame
    if (scope.pushed && presentResult != S_OK)
    {
        std::lock_guard lock(g_dxvkLedgerMutex);
        g_dxvkLedger.Undo(scope.seq);
    }
}

void OnFirstSubmit(const void* queue)
{
    if (!g_running.load(std::memory_order_relaxed) || g_submitSent.load(std::memory_order_relaxed))
        return;

    const auto current = CurrentDevice();

    // A D3D12 queue submit or D3D11 draw says nothing about a frame being paced on the Vulkan surface
    if (current.api != Api::D3D)
        return;

    // OptiScaler's own queues and frame generation's do not start the game's frame
    if (queue != nullptr)
    {
        if (auto gameQueue = g_gameQueue.load(std::memory_order_relaxed); gameQueue != nullptr && queue != gameQueue)
            return;
    }

    if (g_submitSent.exchange(true))
        return;

    auto device = current.device;

    if (device == nullptr || ReflexHooks::gameCalledReflex())
        return;

    Marker(Api::D3D, device, SIMULATION_END);
    Marker(Api::D3D, device, RENDERSUBMIT_START);
}

void OnDeviceReleased(IUnknown* device)
{
    if (device == nullptr)
        return;

    ForgetDevice(device);

    // The next present on a new device sets our Reflex mode again (OnPresentEnd)
    auto expected = device;
    g_sleepModeDevice.compare_exchange_strong(expected, nullptr);

    // dxvk: the game's Present must not run the pair on it either
    {
        std::lock_guard lock(g_dxvkDevicesMutex);

        if (g_dxvkDevices.device12 == device)
            g_dxvkDevices.device12 = nullptr;
    }

    ReflexHooks::forgetSleepDevice(device);
    InputCommon::device_released(device);
}

void OnDeviceReleasedVulkan(VkDevice device)
{
    if (device == nullptr)
        return;

    auto* token = reinterpret_cast<IUnknown*>(device);
    ForgetDevice(token);

    auto expected = token;
    g_sleepModeDevice.compare_exchange_strong(expected, nullptr);

    {
        std::lock_guard lock(g_dxvkDevicesMutex);

        if (g_dxvkDevices.vk == token)
            g_dxvkDevices.vk = nullptr;
    }

    ReflexHooks::forgetSleepDeviceVulkan(device);
}

bool OwnsFrameCap()
{
    return g_running.load(std::memory_order_relaxed) &&
           g_capOwner.load(std::memory_order_relaxed) == CapOwner::LowLatency;
}

bool Running()
{
    return g_decision.load(std::memory_order_relaxed) == Decision::Run && g_running.load(std::memory_order_relaxed);
}

uint32_t FrameIdForFrameGeneration()
{
    // dxvk with the pair at the game's Present: the bridge present on dxvk's thread presents a frame the game's thread has
    // long moved past, and takes the ID that frame's markers carried (its ledger entry, taken before the frame is touched).
    // The game's own thread (XeFG tags the frame's resources while it renders) and everyone else are in the newest frame,
    // as before; so is everyone when dxvk presents on the game's own thread.
    if (const auto bridge = g_dxvkBridgeFrameId.load(); bridge != 0)
    {
        const auto thread = GetCurrentThreadId();

        if (thread == g_dxvkVulkanThread.load() && thread != g_dxvkGameThread.load() && Running())
            return bridge;
    }

    if (const auto ours = g_xellFrameIdForFrameGeneration.load(); ours != 0 && Running())
        return ours;

    return 0;
}

bool WantsBoostForFrameGeneration()
{
    return g_decision.load(std::memory_order_relaxed) == Decision::FrameGenerationOwnsReflex &&
           Config::Instance()->DlssNrNativeLowLatencyBoost.value_or_default();
}

bool XefgGenerating()
{
    auto& state = State::Instance();
    // Not while it waits for the game's next frame data (FG only with no upscaler call feeding it): it shows only the
    // game's frames then, and a cap counted in shown frames would halve the frame rate
    const bool fgGenerating = state.currentFG != nullptr && state.currentFG->IsGenerating();

    // XeFG passes frames through in exclusive full screen (Intel); realExclusiveFullscreen is set after
    // FGXeFGForceBorderless had its say, so it means the real thing
    return UsesXeFGApi(state.activeFgOutput) && fgGenerating && !state.realExclusiveFullscreen;
}

void WaitForGameQueueIdle()
{
    static std::mutex mutex;
    static HANDLE event = nullptr; // reused; one a timed-out wait may still signal is let go, never closed

    std::scoped_lock lock(mutex);

    auto* queue = static_cast<ID3D12CommandQueue*>(const_cast<void*>(GameQueue()));

    if (queue == nullptr)
        return;

    if (event == nullptr)
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    ID3D12Device* device = nullptr;

    if (event == nullptr || FAILED(queue->GetDevice(IID_PPV_ARGS(&device))))
        return;

    // A fence of its own each time: held across calls it would keep a released device alive. The calls are rare.
    ID3D12Fence* fence = nullptr;

    if (SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
    {
        if (SUCCEEDED(queue->Signal(fence, 1)) && fence->GetCompletedValue() < 1 &&
            SUCCEEDED(fence->SetEventOnCompletion(1, event)) && WaitForSingleObject(event, 250) == WAIT_TIMEOUT)
        {
            LOG_WARN("Low latency: the game's queue did not go idle within 250 ms before a sleep mode change");

            // The fence may still signal it: a new event next time
            event = nullptr;
        }

        fence->Release();
    }

    device->Release();
}

bool FrameGenerationNeedsReflex()
{
    return State::Instance().externalFrameGeneration || State::Instance().activeFgOutput == FGOutput::DLSSG;
}

bool GameLowLatencyBlocked()
{
    return BlocksGameLowLatency(Config::Instance()->FramerateLowLatencyInput.value_or_default(),
                                FrameGenerationNeedsReflex());
}

OutputInputs CurrentOutputInputs()
{
    // Once: getPrimaryGpu copies the GPU list, and the primary GPU does not change while the game runs
    static const auto vendor = IdentifyGpu::getPrimaryGpu().vendorId;

    OutputInputs in;
    in.fakenvapiMain = fakenvapi::isUsingAsMainNvapi();
    in.xefgOutput = UsesXeFGApi(State::Instance().activeFgOutput);
    in.amdGpu = vendor == VendorId::AMD;
    in.nvidiaGpu = vendor == VendorId::Nvidia;
    in.driverReflex = (in.nvidiaGpu && !in.fakenvapiMain) || InputCommon::active_output() == LowLatencyMode::Reflex;
    in.antiLag2Failed = InputCommon::antilag2_could_not_start();
    in.reflexFailed = InputCommon::reflex_could_not_start();
    in.xefgLatency = Config::Instance()->FGXeFGLowLatencyOutput.value_or_default();
    return in;
}

XefgLatency XefgSide()
{
    // fakenvapi's Force Reflex, either way: it overrides what XeLL does, so it stays the one that lowers latency
    const bool forceReflexSet = Config::Instance()->FN_ForceReflex.value_or_default() != ForceReflex::InGame;
    return xefgreflex::EffectiveXefgLatency(CurrentOutputInputs(), GameLowLatencyBlocked(), forceReflexSet);
}

bool XellCountsShown() { return XeFGProxy::XellCountsShownFrames(); }

bool DriverMultipliesReflexCap() { return XeFGProxy::DriverMultipliesReflexCap(); }

uint32_t XefgShownPerRendered()
{
    const auto fg = State::Instance().currentFG;
    return ShownPerRendered(XefgGenerating() && fg != nullptr, fg != nullptr ? fg->GetInterpolatedFrameCount() : 0);
}

Status GetStatus()
{
    Status status;
    // As the menu shows it: standing aside for the game's own Reflex rather than off (ShownDecision)
    status.decision = ShownDecision(g_decision.load(), Config::Instance()->FramerateLowLatencyInput.value_or_default(),
                                    ReflexHooks::gameCalledReflex());
    status.xefgSide = XefgSide();
    // In the split XeLL only keeps XeFG running: the game's Reflex lowers latency on the other side
    status.gameReflexFeedsXell = ReflexHooks::gameReflexFeedsXell() && status.xefgSide == XefgLatency::XeLL;
    status.forceReflexDisabled = ForceReflexDisabled();
    const auto current = CurrentDevice();
    status.output = OutputText(current.api);
    status.offKeptForFrameGeneration =
        Config::Instance()->FramerateLowLatencyInput.value_or_default() == Input::Off && FrameGenerationNeedsReflex();

    if (status.decision != Decision::Run || !g_running.load(std::memory_order_relaxed))
        return status;

    status.capOwner = g_capOwner.load();
    status.runsFor = g_runsFor.load();
    status.xellIdle = g_xellIdle.load();

    const auto api = current.api;
    status.path = PathText(api);

    auto device = current.device;

    if (device == nullptr)
        return status;

    if (const auto now = Util::MillisecondsNow(); now - g_latencyAskedAt >= 500.0)
    {
        g_latencyAskedAt = now;
        g_hasLatency = false;

        // fakenvapi's report has the markers' times only; its GPU end is the last marker's plus 0.1 ms
        const auto latencyMethod = CurrentMethod(api);
        const bool estimated = latencyMethod != Method::RealReflex && latencyMethod != Method::RealReflexWithXefg;
        const auto ReadLatency = [](const auto& report, bool estimated)
        {
            const uint64_t end = estimated ? report.presentEndTime : report.gpuRenderEndTime;

            if (report.frameID != 0 && report.simStartTime != 0 && end > report.simStartTime)
            {
                g_latencyMs = (float) (end - report.simStartTime) / 1000.0f;
                g_latencyEstimated = estimated;
                g_hasLatency = true;
            }
        };

        static NV_LATENCY_RESULT_PARAMS results {};
        results.version = NV_LATENCY_RESULT_PARAMS_VER;

        if (InputCommon::get_latency(OwnContext(api), device, &results) == InputResult::Ok)
        {
            // The newest report is the last; times are in microseconds
            const auto& report = results.frameReport[63];

            ReadLatency(report, estimated);
        }
    }

    status.hasLatency = g_hasLatency;
    status.latencyMs = g_latencyMs;
    status.latencyEstimated = g_latencyEstimated;
    return status;
}

} // namespace native::lowlatency
