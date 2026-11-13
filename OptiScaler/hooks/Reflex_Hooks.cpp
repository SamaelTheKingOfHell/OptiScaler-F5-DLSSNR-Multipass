#include "pch.h"
#include "Reflex_Hooks.h"
#include <Config.h>
#include <Util.h>

#include <nvapi/fakenvapi.h>

#include <native/VkPresentBridge.h>
#include <native/NativeLowLatency.h>
#include <misc/IdentifyGpu.h>
#include <proxies/Streamline_Proxy.h>

#include <magic_enum.hpp>
#include <low_latency/input/input_common.h>

#include <math.h>
#include <imgui/ImGuiNotify.hpp>

#include <intrin.h>
#include <unordered_map>

// Written on the marker's thread, read by XeFG on the threads that tag its frames
static inline std::atomic<uint64_t> _lastFrameId[20] = {};

// #define LOG_REFLEX_CALLS

// DLSS frame generation's Streamline is calling Reflex from inside the game's Sleep / marker it took (preSleep,
// preMarker): its nested call goes to the nvapi
static bool s_streamlineSleeping = false;
static bool s_streamlineMarking[20] = {};

std::optional<TimingEntry> ReflexHooks::timingData[TimingType::TimingTypeCOUNT] {};

void ReflexHooks::noteMarker(uint64_t frameId, uint32_t markerType, bool countsAsGame)
{
    // TODO: reflexFrameId gets constantly changed, up and down depending on the marker
    State::Instance().reflexFrameId = frameId;

    // if (markerType == PRESENT_END)
    if (markerType < std::size(_lastFrameId))
        _lastFrameId[markerType] = frameId;

    // Our own markers (Optical F5Low's low latency) are not the game sending markers
    if (markerType == SIMULATION_START && countsAsGame)
        _lastMarkerFrame = State::Instance().fgLastFrame;
}

// RTSS's Reflex injection marks its markers' frame IDs in the high bits (its Vulkan ones too)
static void NoteRtssMarker(uint64_t frameId)
{
    if ((frameId >> 32) == 0)
        return;

    if (!State::Instance().rtssReflexInjection && UsesXeFGApi(State::Instance().activeFgOutput))
    {
        ImGuiToast notification { ImGuiToastType::Warning, 10000 };
        notification.setTitle("RTSS + XeFG detected");
        notification.setContent(
            "RTSS Reflex Injection is known to cause issues.\nEspecially when using XeFG.\nPlease disable it.");
        ImGui::InsertNotification(notification);
    }

    State::Instance().rtssReflexInjection = true;
}

ID3D12Device* ReflexHooks::bridgedXellDevice()
{
    const bool xefgOutput = UsesXeFGApi(State::Instance().activeFgOutput);

    // Lock-free (XeFG's present thread asks while the bridge's teardown holds its lock and waits for that thread)
    ID3D12Device* device = xefgOutput ? VkPresentBridge::Device() : nullptr;
    const bool dxvkWithoutVulkanBridge = VkPresentBridge::DxvkGame() && !VkPresentBridge::DxvkThroughVulkan();
    const bool forward =
        native::xefgreflex::ShouldForward(xefgOutput, device != nullptr, dxvkWithoutVulkanBridge);

    return forward ? device : nullptr;
}

bool ReflexHooks::gameReflexFeedsXell()
{
    // XeFG is a Vulkan game's output only on the bridge, where the game's calls are InputCommon's too
    return UsesXeFGApi(State::Instance().activeFgOutput) && gameCalledReflex();
}

namespace
{
native::xefgreflex::SleepMode ToMode(const NV_SET_SLEEP_MODE_PARAMS& params)
{
    native::xefgreflex::SleepMode mode;
    mode.lowLatency = params.bLowLatencyMode != 0;
    mode.boost = params.bLowLatencyBoost != 0;
    mode.minimumIntervalUs = params.minimumIntervalUs;
    mode.useMarkersToOptimize = params.bUseMarkersToOptimize != 0;
    return mode;
}

// `params` with the mode's fields in it (the rest of the call is kept as it was)
NV_SET_SLEEP_MODE_PARAMS WithMode(const NV_SET_SLEEP_MODE_PARAMS& params, const native::xefgreflex::SleepMode& mode)
{
    NV_SET_SLEEP_MODE_PARAMS out = params;
    out.bLowLatencyMode = mode.lowLatency;
    out.bLowLatencyBoost = mode.boost;
    out.minimumIntervalUs = mode.minimumIntervalUs;
    out.bUseMarkersToOptimize = mode.useMarkersToOptimize;
    return out;
}

bool ForceDisabled() { return Config::Instance()->FN_ForceReflex.value_or_default() == ForceReflex::ForceDisable; }

bool XefgOutput() { return UsesXeFGApi(State::Instance().activeFgOutput); }

// The Framerate page's Input is Off: the sleep mode goes out with low latency off (XefgReflexRule.h)
bool Blocked() { return native::lowlatency::GameLowLatencyBlocked(); }

// The Streamline of OptiScaler's DLSS FG output: its sleep mode as XefgReflexRule.h SleepModeForOutput shapes it (Input
// Off, Force Disable), the cap already in
NV_SET_SLEEP_MODE_PARAMS ForOutput(const NV_SET_SLEEP_MODE_PARAMS& params)
{
    return WithMode(
        params, native::xefgreflex::SleepModeForOutput(ToMode(params), 0, XefgOutput(), ForceDisabled(), Blocked()));
}
} // namespace

// The D3D calls that still come here are those of the Streamline OptiScaler loads for its DLSS frame generation output
// (InputReflex hands them over, route*): they go to the nvapi as before, and its sleep mode is kept for the frame cap
// (InputCommon::update, streamlineCap). The game's own and OptiScaler's own calls go to InputCommon (pre*).
NvAPI_Status ReflexHooks::hkNvAPI_D3D_SetSleepMode(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* pSetSleepModeParams)
{
#ifdef LOG_REFLEX_CALLS
    LOG_FUNC();
#endif
    std::scoped_lock lock(_sleepModeMutex);

    // Store for later so we can adjust the fps whenever we want
    memcpy(&_lastSleepParams, pSetSleepModeParams, sizeof(NV_SET_SLEEP_MODE_PARAMS));
    _lastSleepDev = pDev;

    if (_minimumIntervalUs != 0)
        pSetSleepModeParams->minimumIntervalUs = _minimumIntervalUs;

    // The Framerate page's Input is Off: the mode goes out with low latency off
    if (Blocked())
    {
        auto off = ForOutput(*pSetSleepModeParams);
        return o_NvAPI_D3D_SetSleepMode(pDev, &off);
    }

    return o_NvAPI_D3D_SetSleepMode(pDev, pSetSleepModeParams);
}

NvAPI_Status ReflexHooks::hkNvAPI_D3D_Sleep(IUnknown* pDev)
{
#ifdef LOG_REFLEX_CALLS
    LOG_FUNC();
#endif

    bool& skip = s_streamlineSleeping;
    if (State::Instance().activeFgOutput == FGOutput::DLSSG &&
        Config::Instance()->FGDLSSGUseGamesReflexMarkers.value_or_default() && State::Instance().currentFG &&
        State::Instance().currentFG->IsActive() && !State::Instance().currentFG->IsPaused())
    {
        if (!skip)
        {
            uint32_t frameCount = (uint32_t) _lastFrameId[SIMULATION_START] + 1;

            sl::FrameToken* frameToken;
            StreamlineProxy::GetNewFrameToken()(frameToken, &frameCount);

            LOG_TRACE("Sleep for frame {}", frameCount);

            skip = true;
            StreamlineProxy::ReflexSleep()(*frameToken);
            skip = false;

            return NVAPI_OK;
        }
    }

    {
        std::scoped_lock lock(_sleepModeMutex);
        _lastSleepDev = pDev;
    }

    return o_NvAPI_D3D_Sleep(pDev);
}

NvAPI_Status ReflexHooks::hkNvAPI_D3D_GetLatency(IUnknown* pDev, NV_LATENCY_RESULT_PARAMS* pGetLatencyParams)
{
#ifdef LOG_REFLEX_CALLS
    LOG_FUNC();
#endif

    return o_NvAPI_D3D_GetLatency(pDev, pGetLatencyParams);
}

NvAPI_Status ReflexHooks::hkNvAPI_D3D_SetLatencyMarker(IUnknown* pDev,
                                                       NV_LATENCY_MARKER_PARAMS* pSetLatencyMarkerParams)
{
#ifdef LOG_REFLEX_CALLS
    LOG_FUNC();
#endif

    _updatesWithoutMarker = 0;

    // LOG_DEBUG("frameID: {}, markerType: {}", pSetLatencyMarkerParams->frameID,
    //           magic_enum::enum_name(pSetLatencyMarkerParams->markerType));

    // Some games just stop sending any async markers when DLSSG is disabled, so a reset is needed
    if (_lastAsyncMarkerFrameId + 10 < pSetLatencyMarkerParams->frameID)
    {
        _FgNumFramesToGenerate = 0;
    }

    NoteRtssMarker(pSetLatencyMarkerParams->frameID);

    noteMarker(pSetLatencyMarkerParams->frameID, pSetLatencyMarkerParams->markerType, true);

    auto& skip = s_streamlineMarking;

    if (State::Instance().activeFgOutput == FGOutput::DLSSG && StreamlineProxy::IsD3D12Inited() &&
        Config::Instance()->FGDLSSGUseGamesReflexMarkers.value_or_default() && State::Instance().currentFG &&
        State::Instance().currentFG->IsActive() && !State::Instance().currentFG->IsPaused())
    {
        sl::PCLMarker marker {};
        bool noMarker = false;

        switch (pSetLatencyMarkerParams->markerType)
        {
        case SIMULATION_START:
            marker = sl::PCLMarker::eSimulationStart;
            break;

        case SIMULATION_END:
            marker = sl::PCLMarker::eSimulationEnd;
            break;

        case RENDERSUBMIT_START:
            marker = sl::PCLMarker::eRenderSubmitStart;
            break;

        case RENDERSUBMIT_END:
            marker = sl::PCLMarker::eRenderSubmitEnd;
            break;

        case PRESENT_START:
            marker = sl::PCLMarker::ePresentStart;
            break;

        case PRESENT_END:
            marker = sl::PCLMarker::ePresentEnd;
            break;

        case INPUT_SAMPLE:
            marker = sl::PCLMarker::eControllerInputSample;
            break;

        case TRIGGER_FLASH:
            marker = sl::PCLMarker::eTriggerFlash;
            break;

        case PC_LATENCY_PING:
            marker = sl::PCLMarker::eDeltaTCalculation;
            break;

        case OUT_OF_BAND_RENDERSUBMIT_START:
            marker = sl::PCLMarker::eOutOfBandRenderSubmitStart;
            break;

        case OUT_OF_BAND_RENDERSUBMIT_END:
            marker = sl::PCLMarker::eOutOfBandRenderSubmitEnd;
            break;

        case OUT_OF_BAND_PRESENT_START:
            marker = sl::PCLMarker::eOutOfBandPresentStart;
            break;

        case OUT_OF_BAND_PRESENT_END:
            marker = sl::PCLMarker::eOutOfBandPresentEnd;
            break;

        default:
            noMarker = true;
        }

        auto index = (UINT) marker;

        if (!noMarker && !skip[index])
        {
            if (pSetLatencyMarkerParams->markerType == PRESENT_START && State::Instance().currentFG != nullptr)
            {
                auto frameCount = State::Instance().currentFG->FrameCount();
                if (pSetLatencyMarkerParams->frameID - frameCount > 1)
                    LOG_WARN("FrameId Moved too much??? {} -> {}", frameCount, pSetLatencyMarkerParams->frameID);

                if (pSetLatencyMarkerParams->frameID != frameCount)
                    State::Instance().currentFG->SetFrameCount(pSetLatencyMarkerParams->frameID);

                State::Instance().reflexFrameId = pSetLatencyMarkerParams->frameID;
            }

            sl::FrameToken* frameToken;
            uint32_t frameCount = (uint32_t) pSetLatencyMarkerParams->frameID;
            StreamlineProxy::GetNewFrameToken()(frameToken, &frameCount);

            LOG_TRACE("{} for frame {}", magic_enum::enum_name(marker), frameCount);

            skip[index] = true;
            StreamlineProxy::PCLSetMarker()(marker, *frameToken);
            skip[index] = false;

            return NvAPI_Status::NVAPI_OK;
        }
        else
        {
            if (noMarker)
                return NVAPI_OK;
            else
                return o_NvAPI_D3D_SetLatencyMarker(pDev, pSetLatencyMarkerParams);
        }
    }

    if (State::Instance().gameQuirks & GameQuirk::HitmanReflexHacks)
    {
        if ((pSetLatencyMarkerParams->markerType != PRESENT_START &&
             pSetLatencyMarkerParams->markerType != PRESENT_END) &&
            _lastSetSleepThread != std::this_thread::get_id())
        {
            LOG_TRACE("Skipping marker for a hack");
            return NVAPI_OK;
        }

        if (pSetLatencyMarkerParams->markerType == SIMULATION_END)
        {
            NV_LATENCY_MARKER_PARAMS newParams = *pSetLatencyMarkerParams;
            auto previousThread = _lastSetSleepThread;
            _lastSetSleepThread = std::this_thread::get_id();

            newParams.markerType = RENDERSUBMIT_START;
            hkNvAPI_D3D_SetLatencyMarker(pDev, &newParams);

            newParams.markerType = RENDERSUBMIT_END;
            hkNvAPI_D3D_SetLatencyMarker(pDev, &newParams);

            _lastSetSleepThread = previousThread;
        }
    }

    return o_NvAPI_D3D_SetLatencyMarker(pDev, pSetLatencyMarkerParams);
}

NvAPI_Status ReflexHooks::hkNvAPI_D3D12_SetAsyncFrameMarker(ID3D12CommandQueue* pCommandQueue,
                                                            NV_ASYNC_FRAME_MARKER_PARAMS* pSetAsyncFrameMarkerParams)
{
#ifdef LOG_REFLEX_CALLS
    LOG_FUNC();
#endif

    _lastAsyncMarkerFrameId = pSetAsyncFrameMarkerParams->frameID;

    // if (pSetAsyncFrameMarkerParams->markerType == OUT_OF_BAND_PRESENT_START)
    //{
    //     constexpr size_t history_size = 20;
    //     static size_t counter = 0;
    //     static NvU64 previous_frame_ids[history_size] = {};

    //    previous_frame_ids[counter % history_size] = pSetAsyncFrameMarkerParams->frameID;
    //    counter++;

    //    size_t valid_count = (counter < history_size) ? counter : history_size;

    //    size_t max_run_length = 1;
    //    size_t current_run = 1;

    //    for (size_t i = 1; i < valid_count; i++)
    //    {
    //        if (previous_frame_ids[i] == previous_frame_ids[i - 1])
    //        {
    //            current_run++;
    //        }
    //        else
    //        {
    //            max_run_length = std::max(max_run_length, current_run);
    //            current_run = 1;
    //        }
    //    }

    //    max_run_length = std::max(max_run_length, current_run);

    //    int detected_mode = 0;
    //    if (max_run_length >= 4)
    //        detected_mode = 3;
    //    else if (max_run_length == 3)
    //        detected_mode = 2;
    //    else if (max_run_length == 2)
    //        detected_mode = 1;

    //    // --- Stability filter ---
    //    static int candidate_mode = 0; // No FG at start
    //    static int candidate_count = 0;
    //    constexpr int stability_threshold = 4;

    //    if (detected_mode == candidate_mode)
    //    {
    //        candidate_count++;
    //    }
    //    else
    //    {
    //        candidate_mode = detected_mode;
    //        candidate_count = 1;
    //    }

    //    // Redundant
    //    // max_run_length = std::max(max_run_length, current_run);

    //    // Only commit after stable detection
    //    if (candidate_count >= stability_threshold)
    //    {
    //        if (candidate_mode == 0 && _FgNumFramesToGenerate > 0)
    //        {
    //            _FgNumFramesToGenerate = 0;
    //            LOG_DEBUG("DLSS FG no longer detected");
    //        }
    //        else if (_FgNumFramesToGenerate != candidate_mode)
    //        {
    //            _FgNumFramesToGenerate = candidate_mode;

    //            LOG_DEBUG("DLSS FG detected: {}x mode", candidate_mode + 1);
    //        }
    //    }
    //}

    return o_NvAPI_D3D12_SetAsyncFrameMarker(pCommandQueue, pSetAsyncFrameMarkerParams);
}

void ReflexHooks::hookReflex(PFN_NvApi_QueryInterface& queryInterface)
{
    if (!_inited)
    {
#ifdef _DEBUG
        LOG_FUNC();
#endif

        o_NvAPI_D3D_SetSleepMode = GET_INTERFACE(NvAPI_D3D_SetSleepMode, queryInterface);
        o_NvAPI_D3D_Sleep = GET_INTERFACE(NvAPI_D3D_Sleep, queryInterface);
        o_NvAPI_D3D_GetLatency = GET_INTERFACE(NvAPI_D3D_GetLatency, queryInterface);
        o_NvAPI_D3D_SetLatencyMarker = GET_INTERFACE(NvAPI_D3D_SetLatencyMarker, queryInterface);
        o_NvAPI_D3D12_SetAsyncFrameMarker = GET_INTERFACE(NvAPI_D3D12_SetAsyncFrameMarker, queryInterface);
        // The Vulkan ones only for InputCommon's NvAPI Vulkan Reflex output (nvapiVulkanReady): optional
        o_NvAPI_Vulkan_SetLatencyMarker = GET_INTERFACE(NvAPI_Vulkan_SetLatencyMarker, queryInterface);
        o_NvAPI_Vulkan_SetSleepMode = GET_INTERFACE(NvAPI_Vulkan_SetSleepMode, queryInterface);
        o_NvAPI_Vulkan_GetSleepStatus = GET_INTERFACE(NvAPI_Vulkan_GetSleepStatus, queryInterface);
        o_NvAPI_Vulkan_Sleep = GET_INTERFACE(NvAPI_Vulkan_Sleep, queryInterface);
        o_NvAPI_Vulkan_GetLatency = GET_INTERFACE(NvAPI_Vulkan_GetLatency, queryInterface);
        o_NvAPI_Vulkan_InitLowLatencyDevice = GET_INTERFACE(NvAPI_Vulkan_InitLowLatencyDevice, queryInterface);
        o_NvAPI_Vulkan_DestroyLowLatencyDevice = GET_INTERFACE(NvAPI_Vulkan_DestroyLowLatencyDevice, queryInterface);
        o_NvAPI_Vulkan_NotifyOutOfBandVkQueue = GET_INTERFACE(NvAPI_Vulkan_NotifyOutOfBandVkQueue, queryInterface);

        // OptiScaler's own fakenvapi lives in this module (UseFakenvapi's fallback hands its query here without
        // making it the main nvapi): its Vulkan entries are InputCommon's inputs, never an output
        _vulkanTableIsDriver = o_NvAPI_Vulkan_Sleep != nullptr && !fakenvapi::isUsingAsMainNvapi() &&
                               Util::GetCallerModule((void*) o_NvAPI_Vulkan_Sleep) != dllModule;

        _inited = o_NvAPI_D3D_SetSleepMode && o_NvAPI_D3D_Sleep && o_NvAPI_D3D_GetLatency &&
                  o_NvAPI_D3D_SetLatencyMarker && o_NvAPI_D3D12_SetAsyncFrameMarker;

        if (_inited)
            LOG_DEBUG("Inited Reflex hooks");
    }
}

uint8_t ReflexHooks::dlssgFrameCountToGenerate() { return _FgNumFramesToGenerate; }

void ReflexHooks::setDlssgFrameCount(uint8_t count) { _FgNumFramesToGenerate = count; }

bool ReflexHooks::isReflexHooked() { return _inited; }

void* ReflexHooks::getHookedReflex(unsigned int InterfaceId)
{
    if (InterfaceId == GET_ID(NvAPI_D3D_SetSleepMode) && o_NvAPI_D3D_SetSleepMode)
    {
        return &hkNvAPI_D3D_SetSleepMode;
    }
    if (InterfaceId == GET_ID(NvAPI_D3D_Sleep) && o_NvAPI_D3D_Sleep)
    {
        return &hkNvAPI_D3D_Sleep;
    }
    if (InterfaceId == GET_ID(NvAPI_D3D_GetLatency) && o_NvAPI_D3D_GetLatency)
    {
        return &hkNvAPI_D3D_GetLatency;
    }
    if (InterfaceId == GET_ID(NvAPI_D3D_SetLatencyMarker) && o_NvAPI_D3D_SetLatencyMarker)
    {
        return &hkNvAPI_D3D_SetLatencyMarker;
    }
    if (InterfaceId == GET_ID(NvAPI_D3D12_SetAsyncFrameMarker) && o_NvAPI_D3D12_SetAsyncFrameMarker)
    {
        return &hkNvAPI_D3D12_SetAsyncFrameMarker;
    }
    return nullptr;
}

#define UPDATE_TIMING_ENTRY(name, type)                                                                                \
    if (frameReport.name##EndTime >= frameReport.name##StartTime)                                                      \
    {                                                                                                                  \
        double name##Pos = (double) (frameReport.name##StartTime - start) / rangeNs;                                   \
        double name##Length = (double) (frameReport.name##EndTime - frameReport.name##StartTime) / rangeNs;            \
        timingData[TimingType::type] = TimingEntry { .position = name##Pos, .length = name##Length };                  \
    }                                                                                                                  \
    else                                                                                                               \
    {                                                                                                                  \
        timingData[TimingType::type].reset();                                                                          \
    }

void ReflexHooks::resetInputLatency() { _inputLatencyValid.store(false, std::memory_order_relaxed); }

bool ReflexHooks::updateTimingData()
{
    // Why the Reflex timings are empty, once per reason (the menu's timing graph and the FPS overlay's Reflex timings)
    static const char* lastWhy = nullptr;
    auto noData = [&](const char* why)
    {
        if (lastWhy != why)
        {
            lastWhy = why;
            LOG_INFO("Reflex timings: no data, {}", why);
        }

        resetInputLatency();
        return false;
    };

    if (!_inited)
        return noData("the Reflex hooks are not up (NvAPI Reflex functions missing)");

    auto processFrameReport = [&](const auto& frameReport) -> bool
    {
        uint64_t start = UINT64_MAX;
        uint64_t end = 0;

        // Please don't look, just thought it would be least work
        auto pTimes = (const uint64_t*) &frameReport.simStartTime;
        for (auto i = 0; i < 11; i++)
        {
            auto& time = pTimes[i];
            if (time == 0)
                continue;

            if (time < start)
                start = time;

            if (time > end)
                end = time;
        }

        if (end < start)
            return noData("the driver's frame report is empty (no markers reached it on this device)");

        double rangeNs = static_cast<double>(end - start);

        // Input latency: the report's own unit is microseconds. An estimated report (fakenvapi) has made-up GPU marks:
        // it reaches no further than Present.
        {
            const auto latency = lloutput::InputLatencyFromReport(
                frameReport.simStartTime, frameReport.presentEndTime,
                _timingEstimated.load(std::memory_order_relaxed) ? 0 : frameReport.gpuRenderEndTime);

            // Only a new frame of sane length counts; a stalled report (loading screen) starts the average over
            const bool counts =
                latency.valid && lloutput::InputLatencySampleCounts(_haveLatencyFrameId, _lastLatencyFrameId,
                                                                    frameReport.frameID, latency.ms);
            _lastLatencyFrameId = frameReport.frameID;
            _haveLatencyFrameId = true;

            if (!counts)
            {
                resetInputLatency();
            }
            else
            {
                // An average of about the last 8 reads, about 4 s at the overlay's 500 ms poll (an exponential one);
                // starts again when the data was empty or changes from reaching the GPU to not (or back)
                constexpr double alpha = 2.0 / (8.0 + 1.0);
                double value = latency.ms;

                if (_inputLatencyValid.load(std::memory_order_relaxed) &&
                    _inputLatencyReachesGpu.load(std::memory_order_relaxed) == latency.reachesGpu)
                {
                    const double previous = _inputLatencyMs.load(std::memory_order_relaxed);
                    value = previous + alpha * (latency.ms - previous);
                }

                _inputLatencyMs.store(value, std::memory_order_relaxed);
                _inputLatencyReachesGpu.store(latency.reachesGpu, std::memory_order_relaxed);
                _inputLatencyValid.store(true, std::memory_order_relaxed);
            }
        }

        timingData[TimingType::TimeRange] = TimingEntry { .position = 0, .length = rangeNs };
        UPDATE_TIMING_ENTRY(sim, Simulation)
        UPDATE_TIMING_ENTRY(renderSubmit, RenderSubmit)
        UPDATE_TIMING_ENTRY(present, Present)
        UPDATE_TIMING_ENTRY(driver, Driver)
        UPDATE_TIMING_ENTRY(osRenderQueue, OsRenderQueue)
        UPDATE_TIMING_ENTRY(gpuRender, GpuRender)

        if (frameReport.frameID != 0)
            State::Instance().reflexFrameId = frameReport.frameID;

        return true;
    };

    // The report of InputCommon's output (the driver's for Reflex, D3D or Vulkan; its own markers' for the others)
    {
        // Not calling free on this but it's static so hopefully fine
        static NV_LATENCY_RESULT_PARAMS* results = new NV_LATENCY_RESULT_PARAMS();
        *results = {};
        results->version = NV_LATENCY_RESULT_PARAMS_VER;

        bool estimated = false;

        if (InputCommon::output_latency(results, &estimated) == NVAPI_OK)
        {
            _timingEstimated = estimated;

            static bool logged = false;
            if (!logged)
            {
                logged = true;
                LOG_INFO("Reflex timings: reading the report of the low latency output");
            }

            // 64th element has the latest data
            return processFrameReport(results->frameReport[63]);
        }
    }

    std::scoped_lock lock(_sleepModeMutex);

    // The Streamline of OptiScaler's DLSS FG output
    if (_lastSleepDev != nullptr && o_NvAPI_D3D_GetLatency)
    {
        _timingEstimated = fakenvapi::isUsingAsMainNvapi();

        static NV_LATENCY_RESULT_PARAMS* results = new NV_LATENCY_RESULT_PARAMS();
        results->version = NV_LATENCY_RESULT_PARAMS_VER;

        if (auto result = o_NvAPI_D3D_GetLatency(_lastSleepDev, results); result != NVAPI_OK)
        {
            LOG_WARN("NvAPI_D3D_GetLatency failed: {}", magic_enum::enum_name(result));
            resetInputLatency();
            return false;
        }

        // 64th element has the latest data
        return processFrameReport(results->frameReport[63]);
    }

    return noData("no Reflex device: no low latency output, no Streamline sleep device");
}

void ReflexHooks::presentUpdate(lloutput::PresentSite site, bool bridgeOutput)
{
    // The game's own last DXGI present (a D3D game's, dxvk's too): a Vulkan present soon after it is dxvk's
    static std::atomic<double> lastD3dGamePresentMs = -1.0;
    const double now = Util::MillisecondsNow();

    if (site == lloutput::PresentSite::Dxgi && !bridgeOutput)
        lastD3dGamePresentMs.store(now, std::memory_order_relaxed);

    const double last = lastD3dGamePresentMs.load(std::memory_order_relaxed);

    if (lloutput::RunsLowLatencyUpdate(site, bridgeOutput, last < 0.0 ? -1.0 : now - last))
        update();
}

void ReflexHooks::update()
{
    // We can still use just the markers to limit the fps with Reflex disabled
    // But need to fallback in case a game stops sending them for some reason
    _updatesWithoutMarker++;

    // InputCommon delivers every game's Reflex and ours, D3D and Vulkan, and keeps the frame cap's rule (OutputRule.h
    // CapForGame, which hands no cap to an external frame generation's Reflex), the Streamline of OptiScaler's DLSS FG
    // output included; Force State reaches its output whatever owns frame generation
    InputCommon::update();
}

uint64_t ReflexHooks::lastMarkerFrameId(NV_LATENCY_MARKER_TYPE type)
{
    return (size_t) type < std::size(_lastFrameId) ? _lastFrameId[type].load(std::memory_order_relaxed) : 0;
}

uint32_t ReflexHooks::gameXellFrameId()
{
    if (!gameCalledReflex() || !gameIsSendingMarkers())
        return 0;

    if (const auto renderSubmit = lastMarkerFrameId(RENDERSUBMIT_START); renderSubmit != 0)
        return (uint32_t) renderSubmit;

    return (uint32_t) lastMarkerFrameId(SIMULATION_START);
}

bool ReflexHooks::gameIsSendingMarkers()
{
    return _lastMarkerFrame != 0 && ((State::Instance().fgLastFrame < _lastMarkerFrame) ||
                                     (State::Instance().fgLastFrame - _lastMarkerFrame) < 5);
}

bool ReflexHooks::ensureTable(PFN_NvApi_QueryInterface queryInterface)
{
    if (!_inited && queryInterface != nullptr)
        hookReflex(queryInterface);

    return _inited;
}

// Streamline's own modules (sl.interposer, sl.reflex, sl.pcl, sl.dlss_g ...) and the DLSS frame generation module
static bool IsStreamlineModule(HMODULE module)
{
    if (module == nullptr)
        return false;

    static std::mutex cacheMutex;
    static std::unordered_map<HMODULE, bool> cache;

    std::scoped_lock lock(cacheMutex);

    if (auto it = cache.find(module); it != cache.end())
        return it->second;

    wchar_t path[MAX_PATH] {};
    GetModuleFileNameW(module, path, MAX_PATH);

    auto name = std::filesystem::path(path).filename().wstring();
    std::transform(name.begin(), name.end(), name.begin(), ::towlower);

    const bool streamline = name.starts_with(L"sl.") || name == L"nvngx_dlssg.dll";
    cache[module] = streamline;
    return streamline;
}

bool ReflexHooks::isGameCall(void* returnAddress)
{
    // A game that started Streamline itself may run its own Reflex through it (NBA 2K27): Streamline's calls are its
    if (State::Instance().gameCalledSlInit)
        return true;

    // Otherwise Streamline is the one OptiScaler loaded for its own DLSS frame generation output: its Reflex calls are
    // OptiScaler's, not the game's
    return !IsStreamlineModule(Util::GetCallerModule(returnAddress));
}

void ReflexHooks::forgetSleepDevice(IUnknown* pDev)
{
    std::scoped_lock lock(_sleepModeMutex);

    if (pDev != nullptr && _lastSleepDev == pDev)
        _lastSleepDev = nullptr;

    if (pDev != nullptr && _gameSleepDev == pDev)
        _gameSleepDev = nullptr;

    if (pDev != nullptr && _ownSleepDev == pDev)
        _ownSleepDev = nullptr;
}

// ---- D3D Reflex through InputCommon: what the hooks do before it delivers a call ----

namespace
{
// DLSS frame generation's Streamline takes the game's Sleep and markers (FGDLSSGUseGamesReflexMarkers)
bool DlssgTakesGameReflex()
{
    auto& state = State::Instance();
    return state.activeFgOutput == FGOutput::DLSSG &&
           Config::Instance()->FGDLSSGUseGamesReflexMarkers.value_or_default() && state.currentFG != nullptr &&
           state.currentFG->IsActive() && !state.currentFG->IsPaused();
}

bool PclMarker(NV_LATENCY_MARKER_TYPE type, sl::PCLMarker* marker)
{
    switch (type)
    {
    case SIMULATION_START:
        *marker = sl::PCLMarker::eSimulationStart;
        return true;
    case SIMULATION_END:
        *marker = sl::PCLMarker::eSimulationEnd;
        return true;
    case RENDERSUBMIT_START:
        *marker = sl::PCLMarker::eRenderSubmitStart;
        return true;
    case RENDERSUBMIT_END:
        *marker = sl::PCLMarker::eRenderSubmitEnd;
        return true;
    case PRESENT_START:
        *marker = sl::PCLMarker::ePresentStart;
        return true;
    case PRESENT_END:
        *marker = sl::PCLMarker::ePresentEnd;
        return true;
    case INPUT_SAMPLE:
        *marker = sl::PCLMarker::eControllerInputSample;
        return true;
    case TRIGGER_FLASH:
        *marker = sl::PCLMarker::eTriggerFlash;
        return true;
    case PC_LATENCY_PING:
        *marker = sl::PCLMarker::eDeltaTCalculation;
        return true;
    case OUT_OF_BAND_RENDERSUBMIT_START:
        *marker = sl::PCLMarker::eOutOfBandRenderSubmitStart;
        return true;
    case OUT_OF_BAND_RENDERSUBMIT_END:
        *marker = sl::PCLMarker::eOutOfBandRenderSubmitEnd;
        return true;
    case OUT_OF_BAND_PRESENT_START:
        *marker = sl::PCLMarker::eOutOfBandPresentStart;
        return true;
    case OUT_OF_BAND_PRESENT_END:
        *marker = sl::PCLMarker::eOutOfBandPresentEnd;
        return true;
    default:
        return false;
    }
}
} // namespace

void ReflexHooks::preSetSleepMode(IUnknown* pDev, const NV_SET_SLEEP_MODE_PARAMS& params, bool game)
{
    if (!game)
        return;

    std::scoped_lock lock(_sleepModeMutex);

    _gameCalledReflex = true;
    _gameCalledSetSleepMode = true;

    // The game's own mode, before the frame cap goes in: the cap's rule and the timings read it
    _gameSleepParams = params;
    _gameSleepDev = pDev;

    if (State::Instance().gameQuirks & GameQuirk::HitmanReflexHacks)
        _lastSetSleepThread = std::this_thread::get_id();
}

ReflexHooks::Pre ReflexHooks::preSleep(IUnknown* pDev, bool game, NvAPI_Status* result)
{
    {
        std::scoped_lock lock(_sleepModeMutex);

        // Our own Sleep's device is only read by the menu's timing graph when nothing else has one
        if (game)
            _gameSleepDev = pDev;
        else
            _ownSleepDev = pDev;
    }

    if (!game)
        return Pre::Deliver;

    _gameCalledReflex = true;

    if (!DlssgTakesGameReflex())
        return Pre::Deliver;

    // Streamline's own Sleep from inside the one below, counted as the game's (the game started Streamline itself)
    if (s_streamlineSleeping)
    {
        *result = o_NvAPI_D3D_Sleep(pDev);
        return Pre::Taken;
    }

    uint32_t frameCount = (uint32_t) _lastFrameId[SIMULATION_START] + 1;

    sl::FrameToken* frameToken;
    StreamlineProxy::GetNewFrameToken()(frameToken, &frameCount);

    LOG_TRACE("Sleep for frame {}", frameCount);

    s_streamlineSleeping = true;
    StreamlineProxy::ReflexSleep()(*frameToken);
    s_streamlineSleeping = false;

    *result = NVAPI_OK;
    return Pre::Taken;
}

ReflexHooks::Pre ReflexHooks::preMarker(IUnknown* pDev, NV_LATENCY_MARKER_PARAMS* params, bool game, MarkerSend send,
                                        NvAPI_Status* result)
{
    _updatesWithoutMarker = 0;

    if (game)
        _gameCalledReflex = true;

    // Some games just stop sending any async markers when DLSSG is disabled, so a reset is needed
    if (_lastAsyncMarkerFrameId + 10 < params->frameID)
        _FgNumFramesToGenerate = 0;

    NoteRtssMarker(params->frameID);

    noteMarker(params->frameID, params->markerType, game);

    *result = NVAPI_OK;

    if (game && StreamlineProxy::IsD3D12Inited() && DlssgTakesGameReflex())
    {
        sl::PCLMarker marker {};

        if (!PclMarker(params->markerType, &marker))
            return Pre::Taken;

        auto index = (UINT) marker;

        // Streamline's own marker from inside the one below, counted as the game's (the game started Streamline)
        if (s_streamlineMarking[index])
        {
            *result = o_NvAPI_D3D_SetLatencyMarker(pDev, params);
            return Pre::Taken;
        }

        auto& state = State::Instance();

        if (params->markerType == PRESENT_START && state.currentFG != nullptr)
        {
            auto frameCount = state.currentFG->FrameCount();
            if (params->frameID - frameCount > 1)
                LOG_WARN("FrameId Moved too much??? {} -> {}", frameCount, params->frameID);

            if (params->frameID != frameCount)
                state.currentFG->SetFrameCount(params->frameID);

            state.reflexFrameId = params->frameID;
        }

        sl::FrameToken* frameToken;
        uint32_t frameCount = (uint32_t) params->frameID;
        StreamlineProxy::GetNewFrameToken()(frameToken, &frameCount);

        LOG_TRACE("{} for frame {}", magic_enum::enum_name(marker), frameCount);

        s_streamlineMarking[index] = true;
        StreamlineProxy::PCLSetMarker()(marker, *frameToken);
        s_streamlineMarking[index] = false;

        return Pre::Taken;
    }

    if (game && (State::Instance().gameQuirks & GameQuirk::HitmanReflexHacks))
    {
        if ((params->markerType != PRESENT_START && params->markerType != PRESENT_END) &&
            _lastSetSleepThread != std::this_thread::get_id())
        {
            LOG_TRACE("Skipping marker for a hack");
            return Pre::Skip;
        }

        if (params->markerType == SIMULATION_END)
        {
            NV_LATENCY_MARKER_PARAMS newParams = *params;
            auto previousThread = _lastSetSleepThread;
            _lastSetSleepThread = std::this_thread::get_id();

            newParams.markerType = RENDERSUBMIT_START;
            send(pDev, &newParams, game);

            newParams.markerType = RENDERSUBMIT_END;
            send(pDev, &newParams, game);

            _lastSetSleepThread = previousThread;
        }
    }

    return Pre::Deliver;
}

void ReflexHooks::preAsyncMarker(uint64_t frameId, bool game)
{
    if (game)
        _gameCalledReflex = true;

    _lastAsyncMarkerFrameId = frameId;
}

ReflexHooks::GameSleepMode ReflexHooks::gameSleepMode()
{
    std::scoped_lock lock(_sleepModeMutex);

    if (_gameVkSleepDev != nullptr)
        return { true, _gameVkLowLatency };

    return { _gameSleepDev != nullptr, _gameSleepDev != nullptr && _gameSleepParams.bLowLatencyMode != 0 };
}

bool ReflexHooks::markersRecently() { return _inited && _updatesWithoutMarker <= 20; }

ReflexHooks::GameSleepMode ReflexHooks::streamlineSleepMode()
{
    std::scoped_lock lock(_sleepModeMutex);
    return { _lastSleepDev != nullptr, _lastSleepDev != nullptr && _lastSleepParams.bLowLatencyMode != 0 };
}

void ReflexHooks::streamlineCap(uint32_t intervalUs)
{
    std::scoped_lock lock(_sleepModeMutex);

    // Its later calls get it too (hkNvAPI_D3D_SetSleepMode)
    _minimumIntervalUs = intervalUs;

    if (_lastSleepDev == nullptr || o_NvAPI_D3D_SetSleepMode == nullptr)
        return;

    LOG_INFO("Frame cap in the Streamline Reflex of the DLSS frame generation output: {} us", intervalUs);

    NV_SET_SLEEP_MODE_PARAMS temp = _lastSleepParams;
    temp.minimumIntervalUs = intervalUs;
    temp = ForOutput(temp); // unchanged unless the Framerate page's Input is Off
    o_NvAPI_D3D_SetSleepMode(_lastSleepDev, &temp);
}

NvAPI_Status ReflexHooks::routeSetSleepMode(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* pParams, bool)
{
    return hkNvAPI_D3D_SetSleepMode(pDev, pParams);
}

NvAPI_Status ReflexHooks::routeSleep(IUnknown* pDev, bool) { return hkNvAPI_D3D_Sleep(pDev); }

NvAPI_Status ReflexHooks::routeGetLatency(IUnknown* pDev, NV_LATENCY_RESULT_PARAMS* pParams)
{
    return hkNvAPI_D3D_GetLatency(pDev, pParams);
}

NvAPI_Status ReflexHooks::routeSetLatencyMarker(IUnknown* pDev, NV_LATENCY_MARKER_PARAMS* pParams, bool)
{
    return hkNvAPI_D3D_SetLatencyMarker(pDev, pParams);
}

NvAPI_Status ReflexHooks::deliverLibraryAsyncMarker(ID3D12CommandQueue* pQueue, NV_ASYNC_FRAME_MARKER_PARAMS* pParams)
{
    if (o_NvAPI_D3D12_SetAsyncFrameMarker == nullptr)
        return NVAPI_NO_IMPLEMENTATION;

    return o_NvAPI_D3D12_SetAsyncFrameMarker(pQueue, pParams);
}

NvAPI_Status ReflexHooks::routeSetAsyncFrameMarker(ID3D12CommandQueue* pQueue, NV_ASYNC_FRAME_MARKER_PARAMS* pParams,
                                                   bool)
{
    return hkNvAPI_D3D12_SetAsyncFrameMarker(pQueue, pParams);
}

void ReflexHooks::forgetSleepDeviceVulkan(HANDLE vkDevice)
{
    std::scoped_lock lock(_sleepModeMutex);

    if (vkDevice != nullptr && _gameVkSleepDev == vkDevice)
        _gameVkSleepDev = nullptr;
}

// ---- Vulkan Reflex through InputCommon: what the hooks do before it delivers a call ----

void ReflexHooks::preVkSetSleepMode(HANDLE vkDevice, bool lowLatency, bool game)
{
    if (!game)
        return;

    std::scoped_lock lock(_sleepModeMutex);

    _gameCalledReflex = true;
    _gameCalledSetSleepMode = true;

    // The game's own mode, before the frame cap goes in: the cap's rule reads it
    _gameVkSleepDev = vkDevice;
    _gameVkLowLatency = lowLatency;
}

void ReflexHooks::preVkSleep(HANDLE vkDevice, bool game)
{
    if (!game)
        return;

    _gameCalledReflex = true;

    std::scoped_lock lock(_sleepModeMutex);

    // A game that sleeps without a mode of its own has it off
    if (_gameVkSleepDev == nullptr)
    {
        _gameVkSleepDev = vkDevice;
        _gameVkLowLatency = false;
    }
}

void ReflexHooks::preVkMarker(HANDLE vkDevice, uint64_t frameId, uint32_t markerType, bool game)
{
    _updatesWithoutMarker = 0;

    if (game)
        _gameCalledReflex = true;

    NoteRtssMarker(frameId);
    noteMarker(frameId, markerType, game);
}
