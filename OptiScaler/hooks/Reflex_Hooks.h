#pragma once
#include <d3d12.h>
#include <atomic>
#include <mutex>
#include <nvapi/NvApiTypes.h>

#include "Hook_Utils.h"
#include "low_latency/ll_util.h"
#include <native/XefgReflexRule.h>

namespace lloutput
{
enum class PresentSite : uint8_t; // low_latency/input/OutputRule.h
}

enum TimingType : uint32_t
{
    TimeRange, // in ns, value stored in length
    Simulation,
    RenderSubmit,
    Present,
    Driver,
    OsRenderQueue,
    GpuRender,

    TimingTypeCOUNT
};

class ReflexHooks
{
    inline static bool _inited = false;
    // The cap the Streamline of OptiScaler's DLSS FG output carries (streamlineCap): its later calls get it too
    inline static uint32_t _minimumIntervalUs = 0;
    // OptiScaler's Streamline's (DLSS FG output) sleep mode and device: its calls still go through the hooks
    inline static NV_SET_SLEEP_MODE_PARAMS _lastSleepParams {};
    inline static IUnknown* _lastSleepDev = nullptr;
    // The game's own D3D sleep mode and its device (set by SetSleepMode or Sleep): InputCommon delivers its calls
    inline static NV_SET_SLEEP_MODE_PARAMS _gameSleepParams {};
    inline static IUnknown* _gameSleepDev = nullptr;
    // A Vulkan game's own (NvAPI_Vulkan_* or VK_NV_low_latency2): its VkDevice and whether low latency is on
    inline static HANDLE _gameVkSleepDev = nullptr;
    inline static bool _gameVkLowLatency = false;
    // Optical F5Low's own Sleep device: only the timing graph reads it
    inline static IUnknown* _ownSleepDev = nullptr;
    inline static uint8_t _FgNumFramesToGenerate = 0;
    inline static uint64_t _lastAsyncMarkerFrameId = 0;
    inline static uint64_t _updatesWithoutMarker = 0;

    inline static uint64_t _lastMarkerFrame = 0;

    inline static std::thread::id _lastSetSleepThread {};

    // The marker bookkeeping every marker does: the frame IDs XeFG follows (gameXellFrameId) and the menu read.
    // `countsAsGame`: the game sending markers (gameIsSendingMarkers), not one of ours
    static void noteMarker(uint64_t frameId, uint32_t markerType, bool countsAsGame);

    inline static std::atomic<bool> _gameCalledReflex = false;
    inline static std::atomic<bool> _timingEstimated = false;
    // The overlay's input latency: a running average of the last reads (see ReflexHooks::updateTimingData)
    inline static std::atomic<double> _inputLatencyMs = 0.0;
    inline static std::atomic<bool> _inputLatencyValid = false;
    inline static std::atomic<bool> _inputLatencyReachesGpu = false;
    static void resetInputLatency();
    inline static uint64_t _lastLatencyFrameId = 0;
    inline static bool _haveLatencyFrameId = false;
    inline static std::atomic<bool> _gameCalledSetSleepMode = false;
    // The stored sleep modes and devices: the game's calls and ours never interleave, and a released device is dropped
    inline static std::recursive_mutex _sleepModeMutex;

    // D3D
    inline static decltype(&NvAPI_D3D_SetSleepMode) o_NvAPI_D3D_SetSleepMode = nullptr;
    inline static decltype(&NvAPI_D3D_GetSleepStatus) o_NvAPI_D3D_GetSleepStatus = nullptr;
    inline static decltype(&NvAPI_D3D_Sleep) o_NvAPI_D3D_Sleep = nullptr;
    inline static decltype(&NvAPI_D3D_GetLatency) o_NvAPI_D3D_GetLatency = nullptr;
    inline static decltype(&NvAPI_D3D_SetLatencyMarker) o_NvAPI_D3D_SetLatencyMarker = nullptr;
    inline static decltype(&NvAPI_D3D12_SetAsyncFrameMarker) o_NvAPI_D3D12_SetAsyncFrameMarker = nullptr;

    static NvAPI_Status hkNvAPI_D3D_SetSleepMode(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* pSetSleepModeParams);
    static NvAPI_Status hkNvAPI_D3D_Sleep(IUnknown* pDev);
    static NvAPI_Status hkNvAPI_D3D_GetLatency(IUnknown* pDev, NV_LATENCY_RESULT_PARAMS* pGetLatencyParams);
    static NvAPI_Status hkNvAPI_D3D_SetLatencyMarker(IUnknown* pDev, NV_LATENCY_MARKER_PARAMS* pSetLatencyMarkerParams);
    static NvAPI_Status hkNvAPI_D3D12_SetAsyncFrameMarker(ID3D12CommandQueue* pCommandQueue,
                                                          NV_ASYNC_FRAME_MARKER_PARAMS* pSetAsyncFrameMarkerParams);

    // Vulkan: the driver's, for InputCommon's NvAPI Vulkan Reflex output only (the game's calls are InputReflex's).
    // Optional: an nvapi without them still has the D3D table.
    inline static decltype(&NvAPI_Vulkan_SetLatencyMarker) o_NvAPI_Vulkan_SetLatencyMarker = nullptr;
    inline static decltype(&NvAPI_Vulkan_SetSleepMode) o_NvAPI_Vulkan_SetSleepMode = nullptr;
    inline static decltype(&NvAPI_Vulkan_GetSleepStatus) o_NvAPI_Vulkan_GetSleepStatus = nullptr;
    inline static decltype(&NvAPI_Vulkan_Sleep) o_NvAPI_Vulkan_Sleep = nullptr;
    inline static decltype(&NvAPI_Vulkan_GetLatency) o_NvAPI_Vulkan_GetLatency = nullptr;
    inline static decltype(&NvAPI_Vulkan_InitLowLatencyDevice) o_NvAPI_Vulkan_InitLowLatencyDevice = nullptr;
    inline static decltype(&NvAPI_Vulkan_DestroyLowLatencyDevice) o_NvAPI_Vulkan_DestroyLowLatencyDevice = nullptr;
    inline static decltype(&NvAPI_Vulkan_NotifyOutOfBandVkQueue) o_NvAPI_Vulkan_NotifyOutOfBandVkQueue = nullptr;
    // The table's Vulkan entries are the driver's, not OptiScaler's own fakenvapi (UseFakenvapi's fallback query): its
    // NvAPI_Vulkan_* hand the calls to InputCommon, which would call them again
    inline static bool _vulkanTableIsDriver = false;

    VALIDATE_MEMBER_HOOK(hkNvAPI_D3D_SetSleepMode, decltype(&NvAPI_D3D_SetSleepMode))
    VALIDATE_MEMBER_HOOK(hkNvAPI_D3D_Sleep, decltype(&NvAPI_D3D_Sleep))
    VALIDATE_MEMBER_HOOK(hkNvAPI_D3D_GetLatency, decltype(&NvAPI_D3D_GetLatency))
    VALIDATE_MEMBER_HOOK(hkNvAPI_D3D_SetLatencyMarker, decltype(&NvAPI_D3D_SetLatencyMarker))
    VALIDATE_MEMBER_HOOK(hkNvAPI_D3D12_SetAsyncFrameMarker, decltype(&NvAPI_D3D12_SetAsyncFrameMarker))

  public:
    static std::optional<TimingEntry> timingData[TimingType::TimingTypeCOUNT];

    static void hookReflex(PFN_NvApi_QueryInterface& queryInterface);
    static uint8_t dlssgFrameCountToGenerate();
    static void setDlssgFrameCount(uint8_t count);
    static bool isReflexHooked();
    static void* getHookedReflex(unsigned int InterfaceId);
    static bool updateTimingData();

    // ---- D3D Reflex: InputCommon delivers the game's calls and OptiScaler's own (low_latency/input) ----
    // What the hooks still do for each call before InputCommon delivers it: who called Reflex, the frame IDs XeFG
    // follows and the timings' devices, the game quirks (Hitman, RTSS) and DLSS frame generation's Streamline taking
    // the game's Sleep and markers. `game`: the game's call, else OptiScaler's own (Optical F5Low).
    enum class Pre
    {
        Deliver, // InputCommon delivers it
        Skip,    // nothing is delivered, the call answers NVAPI_OK (Hitman's markers off its sleep thread)
        Taken    // DLSS frame generation's Streamline took it, `*result` is its answer
    };
    static void preSetSleepMode(IUnknown* pDev, const NV_SET_SLEEP_MODE_PARAMS& params, bool game);
    static Pre preSleep(IUnknown* pDev, bool game, NvAPI_Status* result);
    // Delivers a marker (also the ones a game quirk adds)
    using MarkerSend = NvAPI_Status (*)(IUnknown* pDev, NV_LATENCY_MARKER_PARAMS* params, bool game);
    static Pre preMarker(IUnknown* pDev, NV_LATENCY_MARKER_PARAMS* params, bool game, MarkerSend send,
                         NvAPI_Status* result);
    static void preAsyncMarker(uint64_t frameId, bool game);

    // ---- Vulkan Reflex (the game's NvAPI_Vulkan_* and VK_NV_low_latency2 calls, InputReflex / InputReflexVk) ----
    // The same bookkeeping for a Vulkan game's call: who called Reflex, the frame IDs XeFG follows (a game on the
    // Vulkan present bridge), the game's sleep mode for the frame cap's rule. Streamline and the D3D game quirks are
    // D3D's. `markerType`: NVAPI's (NV_LATENCY_MARKER_TYPE ordinal).
    static void preVkSetSleepMode(HANDLE vkDevice, bool lowLatency, bool game);
    static void preVkSleep(HANDLE vkDevice, bool game);
    static void preVkMarker(HANDLE vkDevice, uint64_t frameId, uint32_t markerType, bool game);

    // The game's own sleep mode, for the frame cap's rule (InputCommon::update, OutputRule.h CapForGame): set on a
    // device that is still there (by SetSleepMode or Sleep), and with low latency on. A Vulkan game's when it has one.
    struct GameSleepMode
    {
        bool set = false;
        bool lowLatency = false;
    };
    static GameSleepMode gameSleepMode();
    // The game sent a marker within the last 20 presents
    static bool markersRecently();

    // OptiScaler's own Streamline (its DLSS frame generation output) goes to the nvapi through the hooks as before
    // (InputReflex hands its calls here). Its sleep mode keeps the frame cap when InputCommon::update says so.
    static NvAPI_Status routeSetSleepMode(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* pParams, bool game);
    static NvAPI_Status routeSleep(IUnknown* pDev, bool game);
    static NvAPI_Status routeGetLatency(IUnknown* pDev, NV_LATENCY_RESULT_PARAMS* pParams);
    static NvAPI_Status routeSetLatencyMarker(IUnknown* pDev, NV_LATENCY_MARKER_PARAMS* pParams, bool game);
    static NvAPI_Status routeSetAsyncFrameMarker(ID3D12CommandQueue* pQueue, NV_ASYNC_FRAME_MARKER_PARAMS* pParams,
                                                 bool game);
    static GameSleepMode streamlineSleepMode();
    static void streamlineCap(uint32_t intervalUs);

    // The nvapi's own D3D Reflex (the driver's on real nvapi), past every hook: InputCommon's Reflex output
    static NvAPI_Status nvapiSetSleepMode(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* pParams)
    {
        return o_NvAPI_D3D_SetSleepMode != nullptr ? o_NvAPI_D3D_SetSleepMode(pDev, pParams) : NVAPI_NO_IMPLEMENTATION;
    }
    static NvAPI_Status nvapiSleep(IUnknown* pDev)
    {
        return o_NvAPI_D3D_Sleep != nullptr ? o_NvAPI_D3D_Sleep(pDev) : NVAPI_NO_IMPLEMENTATION;
    }
    static NvAPI_Status nvapiSetLatencyMarker(IUnknown* pDev, NV_LATENCY_MARKER_PARAMS* pParams)
    {
        return o_NvAPI_D3D_SetLatencyMarker != nullptr ? o_NvAPI_D3D_SetLatencyMarker(pDev, pParams)
                                                       : NVAPI_NO_IMPLEMENTATION;
    }
    static NvAPI_Status nvapiGetLatency(IUnknown* pDev, NV_LATENCY_RESULT_PARAMS* pParams)
    {
        return o_NvAPI_D3D_GetLatency != nullptr ? o_NvAPI_D3D_GetLatency(pDev, pParams) : NVAPI_NO_IMPLEMENTATION;
    }

    // The driver's own Vulkan Reflex, past every hook: InputCommon's NvAPI Vulkan Reflex output
    // (low_latency_tech/ll_reflex_nvapi_vk.h), and a game's device the driver made a low latency one before our hooks.
    // Ready when every call it makes is there, and they are the driver's (never fakenvapi's).
    static bool nvapiVulkanReady()
    {
        return _vulkanTableIsDriver && o_NvAPI_Vulkan_SetSleepMode != nullptr && o_NvAPI_Vulkan_Sleep != nullptr &&
               o_NvAPI_Vulkan_SetLatencyMarker != nullptr && o_NvAPI_Vulkan_GetLatency != nullptr &&
               o_NvAPI_Vulkan_InitLowLatencyDevice != nullptr;
    }
    static NvAPI_Status nvapiVkInitLowLatencyDevice(HANDLE vkDevice, HANDLE* signalSemaphore)
    {
        return o_NvAPI_Vulkan_InitLowLatencyDevice != nullptr
                   ? o_NvAPI_Vulkan_InitLowLatencyDevice(vkDevice, signalSemaphore)
                   : NVAPI_NO_IMPLEMENTATION;
    }
    static NvAPI_Status nvapiVkDestroyLowLatencyDevice(HANDLE vkDevice)
    {
        return o_NvAPI_Vulkan_DestroyLowLatencyDevice != nullptr ? o_NvAPI_Vulkan_DestroyLowLatencyDevice(vkDevice)
                                                                 : NVAPI_NO_IMPLEMENTATION;
    }
    static NvAPI_Status nvapiVkNotifyOutOfBandVkQueue(HANDLE vkDevice, HANDLE queue,
                                                      NV_VULKAN_OUT_OF_BAND_QUEUE_TYPE queueType)
    {
        return o_NvAPI_Vulkan_NotifyOutOfBandVkQueue != nullptr
                   ? o_NvAPI_Vulkan_NotifyOutOfBandVkQueue(vkDevice, queue, queueType)
                   : NVAPI_NO_IMPLEMENTATION;
    }
    static NvAPI_Status nvapiVkSetSleepMode(HANDLE vkDevice, NV_VULKAN_SET_SLEEP_MODE_PARAMS* pParams)
    {
        return o_NvAPI_Vulkan_SetSleepMode != nullptr ? o_NvAPI_Vulkan_SetSleepMode(vkDevice, pParams)
                                                      : NVAPI_NO_IMPLEMENTATION;
    }
    static NvAPI_Status nvapiVkGetSleepStatus(HANDLE vkDevice, NV_VULKAN_GET_SLEEP_STATUS_PARAMS* pParams)
    {
        return o_NvAPI_Vulkan_GetSleepStatus != nullptr ? o_NvAPI_Vulkan_GetSleepStatus(vkDevice, pParams)
                                                        : NVAPI_NO_IMPLEMENTATION;
    }
    static NvAPI_Status nvapiVkSleep(HANDLE vkDevice, NvU64 signalValue)
    {
        return o_NvAPI_Vulkan_Sleep != nullptr ? o_NvAPI_Vulkan_Sleep(vkDevice, signalValue) : NVAPI_NO_IMPLEMENTATION;
    }
    static NvAPI_Status nvapiVkSetLatencyMarker(HANDLE vkDevice, NV_VULKAN_LATENCY_MARKER_PARAMS* pParams)
    {
        return o_NvAPI_Vulkan_SetLatencyMarker != nullptr ? o_NvAPI_Vulkan_SetLatencyMarker(vkDevice, pParams)
                                                          : NVAPI_NO_IMPLEMENTATION;
    }
    static NvAPI_Status nvapiVkGetLatency(HANDLE vkDevice, NV_VULKAN_LATENCY_RESULT_PARAMS* pParams)
    {
        return o_NvAPI_Vulkan_GetLatency != nullptr ? o_NvAPI_Vulkan_GetLatency(vkDevice, pParams)
                                                    : NVAPI_NO_IMPLEMENTATION;
    }

    // The last timings came from estimated reports (InputCommon's own markers for a method without a driver report):
    // no GPU, driver or queue times, only the markers' own
    static bool timingEstimated() { return _timingEstimated.load(std::memory_order_relaxed); }
    // The input latency of the last timing reads, smoothed: simulation start to GPU done (reachesGpu), else to the end
    // of Present. False while the timing data is empty. Not the mouse or the monitor.
    static bool inputLatency(double& ms, bool& reachesGpu)
    {
        if (!_inputLatencyValid.load(std::memory_order_relaxed))
            return false;

        ms = _inputLatencyMs.load(std::memory_order_relaxed);
        reachesGpu = _inputLatencyReachesGpu.load(std::memory_order_relaxed);
        return true;
    }
    static bool gameIsSendingMarkers();
    // The frame ID the last marker of this type carried (the game's, or ours while it does not call Reflex)
    static uint64_t lastMarkerFrameId(NV_LATENCY_MARKER_TYPE type);
    // While the game's own Reflex feeds XeLL: the XeLL frame ID of the frame being rendered, the one whose render
    // submit started last (its simulation may be a frame ahead), or its last simulation start's when it sends no render
    // submit markers. 0 when the game sends none.
    static uint32_t gameXellFrameId();

    // The nvapi's Reflex table, looked up once (Optical F5Low's low latency, InputCommon's Reflex output)
    static bool ensureTable(PFN_NvApi_QueryInterface queryInterface);
    // XeFG on the Vulkan present bridge: the bridge's D3D12 device XeFG's XeLL runs on, else null (dxvk unbridged, no
    // XeFG, no bridge). Any thread, lock-free, no side effects.
    static ID3D12Device* bridgedXellDevice();
    // The game's own Reflex calls reach XeFG's XeLL (a D3D game's, or a Vulkan game's on the bridge): the menu's status
    // and the low latency's log say so
    static bool gameReflexFeedsXell();

    // The device is gone: its stored sleep mode goes with it
    static void forgetSleepDevice(IUnknown* pDev);
    static void forgetSleepDeviceVulkan(HANDLE vkDevice);

    // A Reflex call made by the game: not from the Streamline OptiScaler loaded for its own DLSS frame generation
    // (`returnAddress`: the hooked function's caller)
    static bool isGameCall(void* returnAddress);

    // An async marker of the frame generation library's own (lloutput::IsFgLibraryMarker): straight to the driver, no
    // Reflex bookkeeping (not the game's call, no last marker frame, no frame generation mark)
    static NvAPI_Status deliverLibraryAsyncMarker(ID3D12CommandQueue* pQueue, NV_ASYNC_FRAME_MARKER_PARAMS* pParams);

    // The game itself has called Reflex (SetSleepMode, Sleep, a marker or an async marker), ever; and SetSleepMode
    // in particular (a game that only sends markers has Reflex off).
    static bool gameCalledReflex() { return _gameCalledReflex.load(std::memory_order_relaxed); }
    static bool gameCalledSetSleepMode() { return _gameCalledSetSleepMode.load(std::memory_order_relaxed); }

    // Every present of the game's (a DXGI one, `bridgeOutput`: the Vulkan bridge's own D3D12 swapchain's) and every
    // Vulkan present: once per frame, from the present OutputRule.h RunsLowLatencyUpdate picks, the markers' freshness
    // and InputCommon::update (Force State, who keeps the frame cap)
    static void presentUpdate(lloutput::PresentSite site, bool bridgeOutput);

  private:
    static void update();
};
