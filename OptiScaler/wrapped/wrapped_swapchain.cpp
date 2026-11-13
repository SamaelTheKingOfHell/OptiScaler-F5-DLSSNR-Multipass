#include "pch.h"
#include "wrapped_swapchain.h"
#include <dlssnr/DlssNr.h>
#include <hooks/DxgiSwapchainSizing.h>

#include <Util.h>
#include <Logger.h>
#include <Config.h>

#include <nvapi/fakenvapi.h>
#include <hooks/Reflex_Hooks.h>
#include <hooks/D3D12_Hooks.h>
#include <with_dx12/dx11_with_dx12_sync.h>
#include <with_dx12/with_dx12.h>

#include <menu/menu_overlay_dx.h>
#include <menu/menu_overlay_base.h>
#include <output/SdrToHdr_Dx11Present.h>
#include <native/NativeDriverDx12.h>
#include <native/NativeLowLatency.h>
#include <native/VkPresentBridge.h>
#include <low_latency/input/input_common.h>

#include <misc/FrameLimit.h>

#include <d3d11.h>
#include <d3d12.h>
#include <misc/IdentifyGpu.h>
#include <misc/REFrameworkCompat.h>
#include <hooks/Xell_Hooks.h>
#include <hooks/FG_Hooks.h>

#include <magic_enum.hpp>

#ifdef LOW_LATENCY_INPUTS
#include <low_latency/input/input_antilag2.h>
#endif

#ifdef DXGI_DEBUG_ENABLED
#include <magic_enum.hpp>
#include <dxgidebug.h>

#pragma comment(lib, "dxguid.lib")

#ifdef ENABLE_DEBUG_LAYER_DX12
#include <d3d12sdklayers.h>
#endif
#endif

#pragma intrinsic(_ReturnAddress)

// Used RenderDoc's wrapped object as referance
// https://github.com/baldurk/renderdoc/blob/v1.x/renderdoc/driver/dxgi/dxgi_wrapped.cpp

static int scCount = 0;
static UINT64 _frameCounter = 0;
static double _lastFrameTime = 0;
static bool _dx11Device = false;
static bool _dx12Device = false;

const GUID IID_IUnwrappedDXGISwapChain = {
    0xe8a33b4a, 0x1405, 0x424c, { 0xae, 0x88, 0xd, 0x3e, 0x9d, 0x46, 0xc9, 0x14 }
};

// The identity of a COM object, to compare queues that may be reached through different interfaces
static IUnknown* IdentityOfObject(IUnknown* object)
{
    IUnknown* identity = nullptr;

    if (object == nullptr || FAILED(object->QueryInterface(IID_PPV_ARGS(&identity))) || identity == nullptr)
        return nullptr;

    identity->Release();
    return identity;
}

// The identity of a queue with Streamline's proxy taken away (the chain's creation queue was unwrapped the same way, so a
// present queue the game passes through Streamline compares equal to it)
static IUnknown* UnwrappedQueueIdentity(IUnknown* queue)
{
    IUnknown* real = nullptr;

    if (queue != nullptr && Util::CheckForRealObject("UnwrappedQueueIdentity", queue, &real) && real != nullptr)
        queue = real;

    return IdentityOfObject(queue);
}

static ID3D12Fence* resizeFence = nullptr;
static UINT64 resizeFenceValue = 0;
static HANDLE resizeFenceEvent = nullptr;

static void UpdateOutputColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace)
{
    auto& state = State::Instance();

    OutputColorSpace info {};
    info.dxgiColorSpace = colorSpace;

    switch (colorSpace)
    {
        // ------------------------------------------------------------
        // SDR / Rec.709
        // ------------------------------------------------------------

    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
        info.transfer = ColorTransfer::SRGB;
        info.primaries = ColorPrimaries::Rec709;
        info.range = ColorRange::Full;
        info.model = ColorModel::RGB;
        info.valid = true;

        break;

    case DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709:
        info.transfer = ColorTransfer::SRGB;
        info.primaries = ColorPrimaries::Rec709;
        info.range = ColorRange::Studio;
        info.model = ColorModel::RGB;
        info.valid = true;

        break;

        // ------------------------------------------------------------
        // scRGB / linear Rec.709
        // ------------------------------------------------------------

    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
        info.transfer = ColorTransfer::Linear;
        info.primaries = ColorPrimaries::Rec709;
        info.range = ColorRange::Full;
        info.model = ColorModel::RGB;
        info.valid = true;

        break;

        // ------------------------------------------------------------
        // HDR10 / PQ / Rec.2020
        // ------------------------------------------------------------

    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
        info.transfer = ColorTransfer::PQ;
        info.primaries = ColorPrimaries::Rec2020;
        info.range = ColorRange::Full;
        info.model = ColorModel::RGB;
        info.valid = true;

        break;

    case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020:
        info.transfer = ColorTransfer::PQ;
        info.primaries = ColorPrimaries::Rec2020;
        info.range = ColorRange::Studio;
        info.model = ColorModel::RGB;
        info.valid = true;

        break;

    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020:
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020:
        info.transfer = ColorTransfer::PQ;
        info.primaries = ColorPrimaries::Rec2020;
        info.range = ColorRange::Studio;
        info.model = ColorModel::YCbCr;
        info.valid = true;

        break;

        // ------------------------------------------------------------
        // HLG / Rec.2020
        // ------------------------------------------------------------

    case DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020:
        info.transfer = ColorTransfer::HLG;
        info.primaries = ColorPrimaries::Rec2020;
        info.range = ColorRange::Full;
        info.model = ColorModel::YCbCr;
        info.valid = true;

        break;

    case DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020:
        info.transfer = ColorTransfer::HLG;
        info.primaries = ColorPrimaries::Rec2020;
        info.range = ColorRange::Studio;
        info.model = ColorModel::YCbCr;
        info.valid = true;

        break;

        // ------------------------------------------------------------
        // Unknown / unsupported
        // ------------------------------------------------------------

    default:
        info.transfer = ColorTransfer::Unknown;
        info.primaries = ColorPrimaries::Unknown;
        info.range = ColorRange::Unknown;
        info.model = ColorModel::Unknown;
        info.valid = false;

        break;
    }

    state.outputColorSpace = info;

    LOG_INFO("Output color space updated: DXGI: {}, Transfer: {}, Primaries: {}, Range: {}, Model: {}, Valid: {}",
             magic_enum::enum_name(info.dxgiColorSpace), magic_enum::enum_name(info.transfer),
             magic_enum::enum_name(info.primaries), magic_enum::enum_name(info.range),
             magic_enum::enum_name(info.model), info.valid);
}

static void WaitForGPUIdle(IUnknown* object)
{
    if (State::Instance().currentD3D12Device == nullptr || object == nullptr)
        return;

    ID3D12CommandQueue* queue = nullptr;

    if (object->QueryInterface(IID_PPV_ARGS(&queue)) == S_OK)
    {
        LOG_DEBUG("Command queue obtained for GPU idle wait");
        queue->Release();
    }

    if (queue != nullptr && resizeFence != nullptr && resizeFenceEvent != nullptr)
    {
        if (State::Instance().currentD3D12Device != nullptr)
        {
            if (resizeFence != nullptr)
            {
                resizeFence->Release();
                resizeFence = nullptr;
            }

            if (resizeFenceEvent != nullptr)
            {
                CloseHandle(resizeFenceEvent);
                resizeFenceEvent = nullptr;
            }

            State::Instance().currentD3D12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&resizeFence));
            resizeFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        }

        LOG_DEBUG("Waiting for GPU to finish before resizing buffers");

        resizeFenceValue++;
        queue->Signal(resizeFence, resizeFenceValue);

        if (resizeFence->GetCompletedValue() < resizeFenceValue)
        {
            resizeFence->SetEventOnCompletion(resizeFenceValue, resizeFenceEvent);
            // Max 5 sec
            auto waitResult = WaitForSingleObject(resizeFenceEvent, 5000);
            LOG_DEBUG("WaitForSingleObject result: {:X}", waitResult);
        }
    }
}

#ifdef DXGI_DEBUG_ENABLED
void ReportDXGILiveObjects()
{
    IDXGIDebug1* dxgiDebug = nullptr;

    if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiDebug))))
    {
        dxgiDebug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_ALL);
        dxgiDebug->Release();
    }
}

void ReadDxgiInfoQueue()
{
    IDXGIInfoQueue* dxgiInfoQueue = nullptr;
    if (DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiInfoQueue)) == S_OK)
    {
        UINT64 msgCount = dxgiInfoQueue->GetNumStoredMessages(DXGI_DEBUG_ALL);
        for (UINT64 i = 0; i < msgCount; ++i)
        {
            SIZE_T msgLen = 0;
            dxgiInfoQueue->GetMessage(DXGI_DEBUG_ALL, i, nullptr, &msgLen);
            std::vector<char> buf(msgLen);
            auto* msg = reinterpret_cast<DXGI_INFO_QUEUE_MESSAGE*>(buf.data());
            dxgiInfoQueue->GetMessage(DXGI_DEBUG_ALL, i, msg, &msgLen);

            auto description = std::string(msg->pDescription, msg->DescriptionByteLength);
            LOG_DEBUG("DXGI Debug Message: Category: {}, Severity: {}, ID: {}, Description: {}",
                      magic_enum::enum_name(msg->Category), magic_enum::enum_name(msg->Severity), msg->ID, description);
        }
    }
}

#ifdef ENABLE_DEBUG_LAYER_DX12
void ReportD3D12LiveObjects(ID3D12Device* device)
{
    ID3D12DebugDevice* debugDevice = nullptr;

    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&debugDevice))))
    {
        debugDevice->ReportLiveDeviceObjects(D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL);
        debugDevice->Release();
    }
}
#endif
#endif

// `proxy` and `display` are HDR output's (output/SdrToHdr_*): null for a chain it is not on
static HRESULT LocalPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                            const DXGI_PRESENT_PARAMETERS* pPresentParameters, IUnknown* pDevice, HWND hWnd, bool isUWP,
                            SdrToHdr::ProxyChainBase* proxy = nullptr, const SdrToHdr::DisplayInfo* display = nullptr,
                            bool upgraded = false)
{
    if (State::Instance().isShuttingDown)
    {
        if (pPresentParameters == nullptr)
            return pSwapChain->Present(SyncInterval, Flags);
        else
            return ((IDXGISwapChain1*) pSwapChain)->Present1(SyncInterval, Flags, pPresentParameters);
    }

    // This lock will delay/prevent release of buffers while present is ongoing. Not for a dxvk game on the Vulkan bridge:
    // the swapchain here is then the game's own (frame generation's comes from the system DXGI, unwrapped), and dxvk's
    // present remakes its Vulkan swapchain after a resize, which takes the exclusive side on this same thread
    // (VkPresentBridge BeginResize): it would wait for itself. A native Vulkan game's present takes none either.
    std::shared_lock<std::shared_mutex> dx11wDx12PresentLock(Dx11wDx12Sync::PresentResizeMutex(), std::defer_lock);
    if (State::Instance().swapchainInteropApi != SwapchainInteropApi::None &&
        UsesXeFGApi(State::Instance().activeFgOutput) && !VkPresentBridge::DxvkThroughVulkan())
    {
        dx11wDx12PresentLock.lock();
    }

    LOG_DEBUG("{}", _frameCounter);
    NoteDroppedLogLinesOnPresent();

    HRESULT presentResult;

    auto willPresent = (Flags & DXGI_PRESENT_TEST) == 0;

    if (willPresent)
    {
        double ftDelta = 0.0;

        auto now = Util::MillisecondsNow();

        if (_lastFrameTime != 0)
            ftDelta = now - _lastFrameTime;

        _lastFrameTime = now;
        State::Instance().presentFrameTime = ftDelta;

        if (State::Instance().currentFG == nullptr)
            State::Instance().lastFGFrameTime = ftDelta;

        LOG_DEBUG("SyncInterval: {}, Flags: {:X}, Frametime: {:0.3f} ms", SyncInterval, Flags, ftDelta);

        // Update swapchain info evey frame
        if (pSwapChain->GetDesc(&State::Instance().currentSwapchainDesc) != S_OK)
            LOG_WARN("Can't get swapchain desc!");
        else if (proxy != nullptr && proxy->Active())
            State::Instance().currentSwapchainDesc.BufferDesc.Format = proxy->GameFormat(); // the game's, not the HDR one
    }

    ID3D11Device* device = nullptr;
    ID3D12Device* device12 = nullptr;
    ID3D12CommandQueue* cq = nullptr;

    bool isD3D11 = false;

    // try to obtain directx objects and find the path
    if (pDevice->QueryInterface(IID_PPV_ARGS(&device)) == S_OK)
    {
        isD3D11 = true;
        device->Release();

        if (!_dx11Device)
            LOG_DEBUG("D3D11Device captured");

        _dx11Device = true;
        State::Instance().swapchainApi = DX11;
        State::Instance().currentD3D11Device = device;
    }
    else if (pDevice->QueryInterface(IID_PPV_ARGS(&cq)) == S_OK)
    {
        cq->Release();

        if (!_dx12Device)
            LOG_DEBUG("D3D12CommandQueue captured");

        ID3D12CommandQueue* realQueue = nullptr;
        if (Util::CheckForRealObject(__FUNCTION__, cq, (IUnknown**) &realQueue))
            cq = realQueue;

        // Not on the Vulkan bridge (native/VkPresentBridge.h): this is XeFG's D3D12 swapchain for a Vulkan game, whose own
        // present sets swapchainApi to Vulkan. Writing D3D12 here made the menu flip between the two. The D3D12 queue and
        // device below are still captured: frame generation needs them.
        if (State::Instance().swapchainInteropApi != SwapchainInteropApi::VkwDx12)
            State::Instance().swapchainApi = DX12;

        // Not while a Vulkan bridge's output is up or being let go of: this is XeFG's present on the bridge's private
        // queue, and the state's pointer to it is cleared on purpose when the output goes (native/VkPresentBridge.cpp)
        if (State::Instance().currentCommandQueue == nullptr && !VkPresentBridge::OutputActive())
            State::Instance().currentCommandQueue = cq;

        if (cq->GetDevice(IID_PPV_ARGS(&device12)) == S_OK)
        {
            device12->Release();

            if (!_dx12Device)
                LOG_DEBUG("D3D12Device captured");

            _dx12Device = true;

            State::Instance().currentD3D12Device = device12;
            D3D12Hooks::HookDevice(device12);
        }
    }

    // The game's own present runs the frame's low latency update (OutputRule.h RunsLowLatencyUpdate), a dxvk game's on
    // the game's thread. On the Vulkan bridge (native/VkPresentBridge.h) a D3D12 present here is frame generation's for
    // a Vulkan game, whose own Vulkan present runs it.
    const bool bridgeOutput = State::Instance().swapchainInteropApi == SwapchainInteropApi::VkwDx12 && !isD3D11;
    auto fg = State::Instance().currentFG;

    ReflexHooks::presentUpdate(lloutput::PresentSite::Dxgi, bridgeOutput);

    XellHooks::update();

    // Upscaler GPU time computation
    if (willPresent && (fg == nullptr || !fg->IsActive() || fg->IsPaused()))
    {
        if (auto currentFeature = State::Instance().currentFeature; currentFeature != nullptr)
        {
            std::optional<double> upscalerTimeOpt {};

            // ReadUpscalerTime takes a void*: a plain D3D12 feature casts it to ID3D12CommandQueue*, so it must never be
            // handed a D3D11 context. A D3D11 game can own one (native::VirtualUpscalerDriver, run on the paired D3D12
            // queue) with no D3D12 queue on this swap chain; its timers live on that paired queue.
            const bool pureDx12Feature = currentFeature->Api() == API::DX12 && !currentFeature->IsWithDx12();

            if (pureDx12Feature)
            {
                auto timingQueue = cq != nullptr ? cq : WithDx12::GetD3D12CommandQueue();

                if (timingQueue != nullptr)
                {
                    if (upscalerTimeOpt = currentFeature->ReadUpscalerTime(timingQueue); upscalerTimeOpt.has_value())
                        currentFeature->ReadDetailedGpuTimes(timingQueue, State::Instance().detailedGpuTimes);
                }
            }
            else if (device != nullptr)
            {
                ID3D11DeviceContext* context = nullptr;
                device->GetImmediateContext(&context);

                if (upscalerTimeOpt = currentFeature->ReadUpscalerTime(context); upscalerTimeOpt.has_value())
                    currentFeature->ReadDetailedGpuTimes(context, State::Instance().detailedGpuTimes);

                context->Release();
            }
            if (!pureDx12Feature && State::Instance().swapchainInteropApi == SwapchainInteropApi::Dx11wDx12 &&
                State::Instance().currentD3D11Device != nullptr)
            {
                ID3D11DeviceContext* context = nullptr;
                State::Instance().currentD3D11Device->GetImmediateContext(&context);

                if (upscalerTimeOpt = currentFeature->ReadUpscalerTime(context); upscalerTimeOpt.has_value())
                    currentFeature->ReadDetailedGpuTimes(context, State::Instance().detailedGpuTimes);

                context->Release();
            }

            if (upscalerTimeOpt.has_value())
            {
                auto upscalerTime = upscalerTimeOpt.value();
                // filter out possibly wrong measured high values
                if (upscalerTime < 100.0)
                {
                    State::Instance().frameTimeMutex.lock();
                    State::Instance().upscaleTimes.push_back(upscalerTime);
                    State::Instance().upscaleTimes.pop_front();
                    State::Instance().frameTimeMutex.unlock();
                }
            }
        }
    }

    // Fallback when FGPresent is not hooked for V-sync
    if (willPresent && Config::Instance()->ForceVsync.has_value())
    {
        LOG_DEBUG("ForceVsync: {}, VsyncInterval: {}, SCAllowTearing: {}, realExclusiveFullscreen: {}",
                  Config::Instance()->ForceVsync.value(), Config::Instance()->VsyncInterval.value_or_default(),
                  State::Instance().SCAllowTearing, State::Instance().realExclusiveFullscreen);

        if (!Config::Instance()->ForceVsync.value())
        {
            SyncInterval = 0;

            if (State::Instance().SCAllowTearing && !State::Instance().realExclusiveFullscreen)
            {
                LOG_DEBUG("Adding DXGI_PRESENT_ALLOW_TEARING");
                Flags |= DXGI_PRESENT_ALLOW_TEARING;
            }
        }
        else
        {
            // Remove allow tearing
            SyncInterval = Config::Instance()->VsyncInterval.value_or_default();

            if (SyncInterval < 1)
                SyncInterval = 1;

            LOG_DEBUG("Removing DXGI_PRESENT_ALLOW_TEARING");
            Flags &= ~DXGI_PRESENT_ALLOW_TEARING;
        }

        LOG_DEBUG("Final SyncInterval: {}", SyncInterval);
    }

    // DXVK check, it's here because of upscaler time calculations
    if (IdentifyGpu::getPrimaryGpu().usesDxvk)
    {
        // dxvk with the Vulkan bridge: Optical F5Low's low latency (Reflex markers and the Sleep) runs here, on the game's
        // thread, as it does for a plain D3D11 game (Dx11wDx12SC::Present). The Vulkan present (hkvkQueuePresentKHR) runs on
        // dxvk's thread and leaves the frames this took to it (native/NativeLowLatency.h DxvkGameScope). Only for the swap
        // chain on the bridge's window: another one (a launcher, a video) has no Vulkan present of the bridge's to pair
        // with. Same willPresent rule as the non-dxvk path below: a DXGI_PRESENT_TEST is not a frame.
        native::lowlatency::DxvkGameScope lowLatencyScope;
        const auto bridgeWindow = VkPresentBridge::OutputWindow();

        if (willPresent && VkPresentBridge::DxvkThroughVulkan() && bridgeWindow != nullptr && bridgeWindow == hWnd)
        {
            lowLatencyScope = native::lowlatency::OnDxvkGamePresentBegin(
                Config::Instance()->DlssNrNativeLowLatencyDxvkGameThread.value_or_default(), bridgeWindow,
                VkPresentBridge::OutputGeneration());
        }
        else if (willPresent && VkPresentBridge::DxvkThroughVulkan() && bridgeWindow == nullptr)
        {
            native::lowlatency::DxvkBridgeDown();
        }

        if (pPresentParameters == nullptr)
            presentResult = pSwapChain->Present(SyncInterval, Flags);
        else
            presentResult = ((IDXGISwapChain1*) pSwapChain)->Present1(SyncInterval, Flags, pPresentParameters);

        native::lowlatency::OnDxvkGamePresentEnd(lowLatencyScope, presentResult);

        if (presentResult == S_OK)
        {
            // DXVK returns below before the native path advances the NR epoch.
            // Count a completed Present; retain the deferred NR evaluation guard.
            if (willPresent)
            {
                _frameCounter++;
                State::Instance().frameCount = _frameCounter;
            }

            LOG_TRACE("3 {}", (UINT) presentResult);
        }
        else if (presentResult == DXGI_ERROR_DEVICE_REMOVED)
        {
            if (isD3D11)
            {
                if (State::Instance().currentD3D11Device != nullptr)
                    Util::GetDeviceRemovedReason(State::Instance().currentD3D11Device);
            }
            else
            {
                if (State::Instance().currentD3D12Device != nullptr)
                    Util::GetDeviceRemovedReason(State::Instance().currentD3D12Device);
            }
        }
        else
        {
            LOG_ERROR("3 {:X}", (UINT) presentResult);
        }

        return presentResult;
    }

    if (willPresent)
    {
        // Tick feature to let it know if it's frozen
        if (auto currentFeature = State::Instance().currentFeature; currentFeature != nullptr)
        {
            if (auto currentFg = State::Instance().currentFG; currentFg != nullptr)
                currentFeature->TickFrozenCheck(currentFg->GetInterpolatedFrameCount());
            else
                currentFeature->TickFrozenCheck();
        }

        // Before the finished-picture pass, which composes from what the virtual upscaler's call captured this frame.
        if (cq && device12)
            NativeMotionDx12::OnPresent(pSwapChain, cq, device12);

        if (cq && (fg == nullptr || !fg->IsActive() || fg->IsPaused()))
            DlssNr::ApplyToFinishedPicture(pSwapChain, cq);

        // HDR output: the game drew into its proxy back buffer. Turn it into the real HDR one, after the finished picture
        // (which reads the proxy) and before the menu (which draws on the real buffer).
        if (proxy != nullptr && (cq != nullptr || isD3D11) && proxy->Active())
        {
            Microsoft::WRL::ComPtr<IDXGISwapChain3> chain3;

            if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&chain3))))
            {
                SdrToHdr::PassConstants k;
                k.convert = Config::Instance()->HdrOutputConvert.value_or_default() ? 1u : 0u;
                k.decode = (uint32_t) Config::Instance()->HdrOutputDecode.value_or_default();
                k.gameNits = SdrToHdr::ClampGameNits(Config::Instance()->HdrOutputGameNits.value_or_default());
                k.peakNits = SdrToHdr::ResolvePeakNits(Config::Instance()->HdrOutputPeakNits.value_or_default(),
                                                       display != nullptr ? display->maxLuminance : 0.0f);
                k.sdrWhiteNits = display != nullptr ? display->sdrWhiteNits : 80.0f;

                // The game may have turned on its own DLSS-G since the chain was proxied: noted, decided at the next resize
                SdrToHdr::CheckLateStandDown(*proxy, upgraded);

                if (isD3D11)
                {
                    // Direct3D 11: the pass runs inside MenuOverlayDx::Present, right after the motion step that reads the
                    // proxy and before the menu draws (SdrToHdr_Dx11Present.h); armed here, run there or just below
                    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
                    device->GetImmediateContext(&context);

                    if (context != nullptr)
                        SdrToHdr::ArmDx11Pass(proxy, chain3.Get(), context.Get(), k);
                }
                else
                {
                    const char* why = proxy->Present(cq, chain3.Get(), k);

                    if (why != nullptr && proxy->NoteFailureLogged())
                    {
                        LOG_ERROR("HDR output: {}; the real back buffer keeps the last picture", why);
                    }
                }
            }
        }

        // Draw overlay, not while the D3D11 bridge resizes (it tears the overlay down on the game's thread)
        if (Dx11wDx12Sync::ResizeInProgress().load() == 0)
            MenuOverlayDx::Present(pSwapChain, SyncInterval, Flags, pPresentParameters, pDevice, hWnd, isUWP);

        // Direct3D 11 HDR output: the menu overlay runs the armed pass after its motion step; when it did not get that far
        // the pass runs now (nothing was drawn on the real buffer yet)
        SdrToHdr::RunPendingDx11Pass();

#ifdef LOW_LATENCY_INPUTS
        if (State::Instance().activeFgOutput == FGOutput::FSRFG)
        {
            auto fgIsActive = fg != nullptr && fg->IsGenerating(); // not while it waits for frame data
            InputAntiLag2::injectAl2Context(pSwapChain, fgIsActive);
        }
#else
        if (State::Instance().activeFgOutput == FGOutput::FSRFG || UsesXeFGApi(State::Instance().activeFgOutput))
        {
            LOG_DEBUG("Calling fakenvapi");

            static UINT64 fgPresentFrame = 0;
            auto fgIsActive = fg != nullptr && fg->IsGenerating(); // not while it waits for frame data

            if (State::Instance().fgPresentIsCalled)
            {
                State::Instance().fgPresentIsCalled = false;
                fgPresentFrame = _frameCounter;
            }

            auto isInterpolated = fgIsActive && (_frameCounter - fgPresentFrame) > 0;

            fakenvapi::reportFGPresent(pSwapChain, fgIsActive, isInterpolated);
        }
#endif

        _frameCounter++;
        State::Instance().frameCount = _frameCounter;
    }

    LOG_DEBUG("Calling original present");

    // Optical F5Low's low latency (native/NativeLowLatency.h): Reflex markers around the game's present
    IUnknown* lowLatencyDevice = nullptr;

    // (A D3D11 game behind OptiScaler's D3D12 bridge is covered in Dx11wDx12SC::Present, the game's own present.)
    if (willPresent && State::Instance().swapchainInteropApi == SwapchainInteropApi::None)
    {
        if (isD3D11)
            lowLatencyDevice = device;
        else
            lowLatencyDevice = device12;

        if (lowLatencyDevice != nullptr)
            native::lowlatency::OnPresentBegin(lowLatencyDevice);
    }

    // XeFG with Reflex or Anti-Lag 2 lowering latency next to its XeLL: each of its presents, generated or real, as
    // DLSSG reports them, with the frame id of the batch XeFG marked (out of band present markers; Anti-Lag 2 takes the
    // frame type from them). InputCommon::fg_output_present does nothing while XeLL is the output.
    const bool xefgPresent = willPresent && cq != nullptr && fg != nullptr &&
                             UsesXeFGApi(State::Instance().activeFgOutput) && fg->IsGenerating();

    if (xefgPresent)
        InputCommon::fg_output_present(cq, false);

    // swapchain present
    if (pPresentParameters == nullptr)
        presentResult = pSwapChain->Present(SyncInterval, Flags);
    else
        presentResult = ((IDXGISwapChain1*) pSwapChain)->Present1(SyncInterval, Flags, pPresentParameters);

    if (xefgPresent)
        InputCommon::fg_output_present(cq, true);

    if (lowLatencyDevice != nullptr)
        native::lowlatency::OnPresentEnd(lowLatencyDevice);

    if (presentResult == S_OK)
    {
        LOG_DEBUG("Original present result: {:X}", (UINT) presentResult);
    }
    else
    {
        LOG_ERROR("Original present result: {:X}", (UINT) presentResult);

        if (presentResult == DXGI_ERROR_DEVICE_REMOVED && State::Instance().currentD3D12Device != nullptr)
            Util::GetDeviceRemovedReason(State::Instance().currentD3D12Device);
    }

    return presentResult;
}

WrappedIDXGISwapChain4::WrappedIDXGISwapChain4(IDXGISwapChain* real, IUnknown* pDevice, HWND hWnd, UINT flags,
                                               bool isUWP, bool isComposition, const SdrToHdr::Plan* plan)
    : _real(real), _device(pDevice), _handle(hWnd), _refcount(1), _uwp(isUWP), _composition(isComposition)
{
    _id = ++scCount;
    _lastFlags = flags;

    _real->QueryInterface(IID_PPV_ARGS(&_real1));
    if (_real1 != nullptr)
        _real1->Release();

    _real->QueryInterface(IID_PPV_ARGS(&_real2));
    if (_real2 != nullptr)
        _real2->Release();

    _real->QueryInterface(IID_PPV_ARGS(&_real3));
    if (_real3 != nullptr)
        _real3->Release();

    _real->QueryInterface(IID_PPV_ARGS(&_real4));
    if (_real4 != nullptr)
        _real4->Release();

    _real->AddRef();
    auto refCount = _real->Release();

    CheckForHdrOutput();

    // identity only; the device keeps the object alive
    _creationQueue = UnwrappedQueueIdentity(pDevice);

    // HDR output decided to make this chain HDR (the real chain already is): the game gets proxies in its own format
    if (plan != nullptr)
    {
        _fgLibraryChain = plan->fgLibraryChain;
        NoteUpgrade(*plan);
        StartHdrPlan(*plan, flags);
        NoteLibraryWindowed(*plan);
    }

    LOG_INFO("{} created, real: {:X}, refCount: {}", _id, (UINT64) real, refCount);
}

void WrappedIDXGISwapChain4::StartHdrPlan(const SdrToHdr::Plan& plan, UINT flags)
{
    if (plan.proxied)
    {
        _display = plan.display;
        _displayKnown = plan.displayKnown;

        const char* why = "the swap chain has no IDXGISwapChain3 or no Direct3D device";
        bool d3d11 = false;
        auto* device = ApiDevice(d3d11);
        auto* proxy = EnsureProxy(d3d11);

        if (_real3 != nullptr && device != nullptr)
        {
            ScopedSkipHeapCapture skipHeapCapture {};
            why = proxy->Start(device, _real3, plan.gameFormat, plan.hdr10);
        }

        if (why != nullptr)
        {
            if (_upgraded)
            {
                // A chain made flip model for HDR output is never handed to the game as a plain flip buffer (unbound at every
                // Present: a bitblt-era game would draw into nothing). It stays a broken proxy chain: restart-required.
                LOG_ERROR("HDR output: {}; this game's swap chain was changed for HDR and cannot be put back: restart the game",
                          why);
                proxy->HoldEngaged(plan.gameFormat);
                SdrToHdr::RecordStatus(SdrToHdr::Reason::StuckHdr, false, plan.hdr10, true, plan.display.maxLuminance);
                return;
            }

            // Never leave the game with an HDR chain and no pass
            LOG_WARN("HDR output: {}; putting the swap chain back to the game's format", why);
            HRESULT revert = E_POINTER;

            if (_real3 != nullptr)
                revert = proxy->Revert(_real3, plan.gameFormat, flags, _gameColourSpace);

            if (FAILED(revert))
                LOG_ERROR("HDR output: the swap chain could not be put back to the game's format ({:X}); it stays HDR "
                          "with nothing converting it",
                          (UINT) revert);

            SdrToHdr::RecordStatus(FAILED(revert) ? SdrToHdr::Reason::StuckHdr : SdrToHdr::Reason::StartFailed, false,
                                   plan.hdr10, true, plan.display.maxLuminance);
        }
        else
        {
            UpdateOutputColorSpace(plan.hdr10 ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                                               : DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
            DlssNr::FinishedPictureColorSpace(_real3, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709); // the finished picture is SDR
            MenuOverlayDx::ApplyThemeStyle();
        }
    }
}

void WrappedIDXGISwapChain4::NoteUpgrade(const SdrToHdr::Plan& plan)
{
    if (!plan.upgraded)
        return;

    _upgraded = true;
    _gameSwapEffect = plan.gameSwapEffect;
    _gameBufferCount = plan.gameBufferCount;

    DisableAltEnter();
}

void WrappedIDXGISwapChain4::NoteLibraryWindowed(const SdrToHdr::Plan& plan)
{
    // Only a chain that is proxied now (the start can fail and put the chain back)
    if (!plan.proxied || !plan.exclusiveToWindowed || _libraryWindowed || !Proxied())
        return;

    _libraryWindowed = true;
    DisableAltEnter();
    FGHooks::KeepBorderless(_handle, this);
}

void WrappedIDXGISwapChain4::DisableAltEnter()
{
    // DXGI's Alt+Enter on this window would put the real flip chain into exclusive full screen behind everyone's back, and
    // Present then fails for good (Batman: Arkham Knight). Same call, same reason as hooks/DxgiFactory_Hooks.cpp's for the Vulkan
    // bridge's swap chain.
    ComPtr<IDXGIFactory> factory;

    if (_real != nullptr && _handle != nullptr && SUCCEEDED(_real->GetParent(IID_PPV_ARGS(&factory))) && factory != nullptr)
    {
        const HRESULT association = factory->MakeWindowAssociation(_handle, DXGI_MWA_NO_ALT_ENTER);

        if (SUCCEEDED(association))
            LOG_INFO("HDR output: Alt+Enter is switched off for this game's swap chain (it was changed for HDR; exclusive full "
                     "screen is kept borderless)");
        else
            LOG_WARN("HDR output: could not switch Alt+Enter off for this game's swap chain ({:X}); the safety net takes the chain "
                     "out of exclusive full screen if it ends up there",
                     (UINT) association);
    }
}

HRESULT WrappedIDXGISwapChain4::EmulateFullscreen(BOOL fullscreen, IDXGIOutput* target)
{
    if (fullscreen)
    {
        if (_borderless.Active())
            return S_OK;

        const Util::MonitorInfo info = target != nullptr ? Util::GetMonitorInfoForOutput(target) : Util::GetMonitorInfoForWindow(_handle);

        // An empty monitor rectangle (the monitor query failed) or a window that is gone: nothing is changed
        if (!_borderless.Enter(_handle, info.monitorRect))
        {
            if (!_borderlessRefusedLogged.exchange(true))
                LOG_WARN("HDR output: borderless full screen refused for this game's swap chain (monitor rectangle empty or window "
                         "gone)");

            return DXGI_ERROR_INVALID_CALL;
        }

        ComPtr<IDXGIOutput> output;

        if (target != nullptr)
            output = target;
        else
            GetContainingOutput(&output);

        {
            std::lock_guard<std::mutex> lock(_borderlessMutex);
            _borderlessTarget = output;
        }

        LOG_INFO("HDR output: exclusive full screen kept borderless for this game's swap chain");
        return S_OK;
    }

    _borderless.Leave(_handle);

    {
        std::lock_guard<std::mutex> lock(_borderlessMutex);
        _borderlessTarget.Reset();
    }

    return S_OK;
}

// A real flip chain that ended up in exclusive full screen anyway (the game re-enabled Alt+Enter, say) fails every Present until
// it is resized; leaving full screen makes it valid again (read from DXGI, tests/sdr_to_hdr_dx11_gpu.cpp)
void WrappedIDXGISwapChain4::RecoverFromExclusive(HRESULT presentResult)
{
    if (!_upgraded || presentResult != DXGI_ERROR_INVALID_CALL)
        return;

    BOOL fullscreen = FALSE;

    if (SUCCEEDED(_real->GetFullscreenState(&fullscreen, nullptr)) && fullscreen)
    {
        _real->SetFullscreenState(FALSE, nullptr);

        // Recovery runs on every such event; only the first is logged
        if (!_exclusiveRecoveryLogged.exchange(true))
            LOG_WARN("HDR output: this game's swap chain was in exclusive full screen; took it out again (every later time too, "
                     "logged this once)");
    }
}

void WrappedIDXGISwapChain4::AdoptHdrPlan(const SdrToHdr::Plan& plan, UINT flags)
{
    _fgLibraryChain = _fgLibraryChain || plan.fgLibraryChain;

    if (plan.proxied && !Proxied())
    {
        NoteUpgrade(plan);
        LOG_INFO("HDR output: this swap chain was already wrapped by a nested hook, giving it the plan");
        StartHdrPlan(plan, flags);
        NoteLibraryWindowed(plan);
    }
}

IUnknown* WrappedIDXGISwapChain4::ApiDevice(bool& d3d11) const
{
    d3d11 = false;

    if (_device == nullptr)
        return nullptr;

    ID3D12CommandQueue* queue = nullptr;

    if (SUCCEEDED(_device->QueryInterface(IID_PPV_ARGS(&queue))) && queue != nullptr)
    {
        ID3D12Device* device = nullptr;
        queue->GetDevice(IID_PPV_ARGS(&device));
        queue->Release();

        if (device != nullptr)
            device->Release(); // the queue keeps it alive

        return device;
    }

    ID3D11Device* device11 = nullptr;

    if (SUCCEEDED(_device->QueryInterface(IID_PPV_ARGS(&device11))) && device11 != nullptr)
    {
        device11->Release(); // _device keeps it alive
        d3d11 = true;
        return device11;
    }

    return nullptr;
}

WrappedIDXGISwapChain4::~WrappedIDXGISwapChain4() {}

bool WrappedIDXGISwapChain4::PassesThrough() const
{
    return _otherWindow && State::Instance().currentFGSwapchain != nullptr;
}

bool WrappedIDXGISwapChain4::OwnsMenuCleanup() const
{
    return !_otherWindow || (_presentedAsMain && !PassesThrough() && MenuOverlayBase::Handle() == _handle);
}

//
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::QueryInterface(REFIID riid, void** ppvObject)
{
    LOG_TRACE("Caller: {}", Util::WhoIsTheCaller(_ReturnAddress()));

    if (riid == __uuidof(IDXGISwapChain))
    {
        AddRef();
        *ppvObject = (IDXGISwapChain*) this;
        return S_OK;
    }
    else if (riid == __uuidof(IDXGISwapChain1))
    {
        if (_real1)
        {
            AddRef();
            *ppvObject = (IDXGISwapChain1*) this;
            return S_OK;
        }
        else
        {
            return E_NOINTERFACE;
        }
    }
    else if (riid == __uuidof(IDXGISwapChain2))
    {
        if (_real2)
        {
            AddRef();
            *ppvObject = (IDXGISwapChain2*) this;
            return S_OK;
        }
        else
        {
            return E_NOINTERFACE;
        }
    }
    else if (riid == __uuidof(IDXGISwapChain3))
    {
        if (_real3)
        {
            AddRef();
            *ppvObject = (IDXGISwapChain3*) this;
            return S_OK;
        }
        else
        {
            return E_NOINTERFACE;
        }
    }
    else if (riid == __uuidof(IDXGISwapChain4))
    {
        if (_real4)
        {
            AddRef();
            *ppvObject = (IDXGISwapChain4*) this;
            return S_OK;
        }
        else
        {
            return E_NOINTERFACE;
        }
    }
    else if (riid == __uuidof(WrappedIDXGISwapChain4))
    {
        AddRef();
        *ppvObject = this;
        return S_OK;
    }
    else if (riid == __uuidof(IUnknown))
    {
        AddRef();
        *ppvObject = (IUnknown*) this;
        return S_OK;
    }
    else if (riid == __uuidof(IDXGIObject))
    {
        AddRef();
        *ppvObject = (IDXGIObject*) this;
        return S_OK;
    }
    else if (riid == __uuidof(IDXGIDeviceSubObject))
    {
        AddRef();
        *ppvObject = (IDXGIDeviceSubObject*) this;
        return S_OK;
    }

    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE WrappedIDXGISwapChain4::AddRef()
{
    InterlockedIncrement(&_refcount);
    LOG_TRACE("Count: {}, caller: {}", _refcount, Util::WhoIsTheCaller(_ReturnAddress()));
    return _refcount;
}

ULONG STDMETHODCALLTYPE WrappedIDXGISwapChain4::Release()
{
    ULONG ret = InterlockedDecrement(&_refcount);

    LOG_TRACE("Count: {}, caller: {}", _refcount, Util::WhoIsTheCaller(_ReturnAddress()));

    if (ret == 0)
    {
        // The teardown below asks REFramework to let go of frame generation's swapchain, and it can take and drop a
        // reference to this one while it does: that second final release would tear down and delete this twice
        const bool nested = _finalReleaseInProgress.exchange(true);

        if (nested && REFrameworkCompat::Active())
        {
            LOG_DEBUG("Nested final release while this swapchain is being released, left to the first one");
            return 0;
        }

#ifdef USE_LOCAL_MUTEX
        OwnedLockGuard lock(_localMutex, 999);
#endif

        // A dxvk game whose frames the Vulkan present bridge carries (VkPresentBridge::DxvkThroughVulkan): the menu and frame
        // generation are the bridge's, on this same window, and it lets go of them itself when dxvk's Vulkan swapchain goes
        // (in _real->Release() below). Tearing them down here would pull XeFG out from under its queued presents.
        const bool bridgeOwns = VkPresentBridge::DxvkThroughVulkan();

        // HDR output no longer keeps the window borderless for this chain
        if (_libraryWindowed)
            FGHooks::ReleaseBorderless(this);

        if (!bridgeOwns && OwnsMenuCleanup())
            MenuOverlayDx::CleanupRenderTarget(true, _handle);

        if (State::Instance().currentSwapchain == this)
            State::Instance().currentSwapchain = nullptr;

        if (State::Instance().currentRealSwapchain == this)
            State::Instance().currentRealSwapchain = nullptr;

        // Not for a chain on another window (MarkOtherWindow): frame generation lives on its own window's chain
        auto fg = State::Instance().currentFG;
        if (!bridgeOwns && !_otherWindow && fg != nullptr && fg->Mutex.getOwner() != 1 &&
            fg->SwapchainContext() != nullptr)
        {
            fg->Deactivate();
            fg->ReleaseSwapchain(_handle);

            if (State::Instance().currentFGSwapchain != nullptr)
                State::Instance().currentFGSwapchain = nullptr;
        }

        // Emulated full screen: the window gets its style and placement back before the chain goes
        if (_borderless.Active())
            _borderless.Leave(_handle);

        // HDR output: the proxies and the pass go before the chain they convert into
        if (auto* proxy = Proxy())
            proxy->Release();

        auto refCount = _real->Release();

        // Disabled for now, cause issues with some games
        /*
        IDXGISwapChain* skSC = nullptr;
        if (_real->QueryInterface(IID_IUnwrappedDXGISwapChain, (void**) &skSC) == S_OK && skSC != nullptr)
        {
            skSC->Release();
            LOG_DEBUG("Found SK swapchain, skip releasing of main swapchain");
        }
        else
        {
            // Release real swapchain, otherwise it can cause issues when re-creating swapchain with same handle
            while (refCount > 0)
            {
                LOG_DEBUG("Waiting for real swapchain to be released, refCount: {}", refCount);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                refCount = _real->Release();
            }
        }
        */

        LOG_DEBUG("Real swapchain released, refCount: {}", refCount);

        delete this;
    }

    return ret;
}

//
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetPrivateData(REFGUID Name, UINT DataSize, const void* pData)
{
    return _real->SetPrivateData(Name, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown)
{
    return _real->SetPrivateDataInterface(Name, pUnknown);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData)
{
    return _real->GetPrivateData(Name, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetParent(REFIID riid, void** ppParent)
{
    return _real->GetParent(riid, ppParent);
}

//
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetDevice(REFIID riid, void** ppDevice)
{
    return _real->GetDevice(riid, ppDevice);
}

//
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::Present(UINT SyncInterval, UINT Flags)
{
    if (_real == nullptr)
        return DXGI_ERROR_DEVICE_REMOVED;

#ifdef USE_LOCAL_MUTEX
    OwnedLockGuard lock(_localMutex, 4);
#endif

    if (_composition && !IsCompositionWindow(_handle))
        _handle = FindCompositionWindow();
    if (_composition && _handle == nullptr)
        return _real->Present(SyncInterval, Flags);

    if (PassesThrough())
        return _real->Present(SyncInterval, Flags);

    _presentedAsMain = true;

    HRESULT result;

    if ((Flags & DXGI_PRESENT_TEST) == 0)
    {
        result = LocalPresent(_real, SyncInterval, Flags, nullptr, _device, _handle, _uwp, Proxy(), &_display, _upgraded);
        RecoverFromExclusive(result);

        // When Reflex can't be used to limit (and Optical F5Low's low latency does not keep the cap), sleep in present
        if (!State::Instance().reflexLimitsFps && !native::lowlatency::OwnsFrameCap() &&
            State::Instance().activeFgOutput == FGOutput::NoFG &&
            !IdentifyGpu::getPrimaryGpu().usesDxvk && !XellHooks::canLimit())
            FrameLimit::sleep();
    }
    else
    {
        result = _real->Present(SyncInterval, Flags);
    }

    return result;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetBuffer(UINT Buffer, REFIID riid, void** ppSurface)
{
    // HDR output: the game gets its proxy back buffer, in its own format (any thread)
    // (also inside a resize, where it says INVALID_CALL rather than hand out the real HDR buffer)
    if (auto* proxy = Proxy(); proxy != nullptr && proxy->Engaged())
        return proxy->GetBuffer(Buffer, riid, ppSurface);

    auto result = _real->GetBuffer(Buffer, riid, ppSurface);
    return result;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget)
{
    LOG_DEBUG("Fullscreen: {}, pTarget: {:X}, Caller: {}", Fullscreen, (size_t) pTarget,
              Util::WhoIsTheCaller(_ReturnAddress()));

    // A chain made flip model for HDR output never reaches the real SetFullscreenState (see EmulateFullscreen). It
    // deliberately skips the FG mutex and the realExclusiveFullscreen bookkeeping below: the emulation only changes the
    // window, never the swap chain, so there is nothing for the mutex to protect from the frame generation present, and
    // the chain stays what it is. (The frame generation library's chain is not here: the game's calls reach the
    // game-facing chain, whose hook answers them.)
    if (_upgraded)
        return EmulateFullscreen(Fullscreen, pTarget);

    // Another window's chain (MarkOtherWindow): neither frame generation's mutex nor State's full-screen flag are its
    if (PassesThrough())
        return _real->SetFullscreenState(Fullscreen, pTarget);

    HRESULT result = S_OK;

    bool ffxLock = false;

    {
#ifdef USE_LOCAL_MUTEX
        // dlssg calls this from present it seems
        // don't try to get a mutex when present owns it while dlssg mod is enabled
        if (!(_localMutex.getOwner() == 4 && State::Instance().activeFgNvngx != FGNvngxReplacement::None))
        {
            OwnedLockGuard lock(_localMutex, 3);
        }
#endif
        if (Config::Instance()->FGUseMutexForSwapchain.value_or_default())
        {

            if (State::Instance().currentFG != nullptr && State::Instance().currentFG->IsActive() &&
                State::Instance().currentFG->Mutex.getOwner() != 3)
            {
                LOG_TRACE("Waiting ffxMutex 3, current: {}", State::Instance().currentFG->Mutex.getOwner());
                State::Instance().currentFG->Mutex.lock(3);
                ffxLock = true;
                LOG_TRACE("Accuired ffxMutex: {}", State::Instance().currentFG->Mutex.getOwner());
            }
            else
            {
                LOG_TRACE("Skipping ffxMutex, owner is already 3");
            }
        }

        State::Instance().realExclusiveFullscreen = Fullscreen;

        result = _real->SetFullscreenState(Fullscreen, pTarget);

        if (result != S_OK)
            LOG_ERROR("result: {:X}", (UINT) result);
        else
            LOG_DEBUG("result: {:X}", result);
    }

    if (ffxLock)
    {
        LOG_TRACE("Releasing ffxMutex: {}", State::Instance().currentFG->Mutex.getOwner());
        State::Instance().currentFG->Mutex.unlockThis(3);
    }

    return result;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget)
{
    // The game believes it is full screen
    if (_borderless.Active())
    {
        if (pFullscreen != nullptr)
            *pFullscreen = TRUE;

        if (ppTarget != nullptr)
        {
            // AddRef under the lock, so a Leave on another thread cannot free the output in between
            std::lock_guard<std::mutex> lock(_borderlessMutex);
            *ppTarget = _borderlessTarget.Get();

            if (*ppTarget != nullptr)
                (*ppTarget)->AddRef();
        }

        return S_OK;
    }

    return _real->GetFullscreenState(pFullscreen, ppTarget);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc)
{
    const HRESULT result = _real->GetDesc(pDesc);

    // HDR output: the game made, and is told about, its own format
    if (auto* proxy = Proxy(); SUCCEEDED(result) && pDesc != nullptr && proxy != nullptr && proxy->Engaged())
        pDesc->BufferDesc.Format = proxy->ReportedFormat();

    // The chain was made flip model for HDR output: the game is told the swap effect and buffer count it asked for
    if (_upgraded && SUCCEEDED(result) && pDesc != nullptr)
    {
        pDesc->SwapEffect = _gameSwapEffect;
        pDesc->BufferCount = _gameBufferCount;

        if (_borderless.Active())
            pDesc->Windowed = FALSE;
    }

    return result;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height,
                                                                DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    // Another window's chain (MarkOtherWindow): no HDR proxy, no frame generation, menu or State work (the menu's
    // cleanup is global, see OwnsMenuCleanup)
    if (PassesThrough())
        return _real->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);

    // HDR output: a resize inside a resize on this thread (a window message the game answers with its own ResizeBuffers) is
    // refused; one from another thread waits for the running one (SdrToHdr::ProxyChainBase::Resize)
    if (auto* proxy = Proxy(); proxy != nullptr && proxy->InResizeOnThisThread())
        return DXGI_ERROR_INVALID_CALL;

    if (!DlssNr::WaitForFinishedPicture())
        return DXGI_ERROR_WAS_STILL_DRAWING;
    LOG_DEBUG("");

    // A chain made flip model for HDR output needs two buffers at least: the game's count is remembered for GetDesc, the real
    // call gets the raised one (0 keeps the count). Flip refuses the GDI-compatible flag, so the resize goes without it (GetDesc
    // therefore does not show that flag: known limit). Done after the early return above, which must leave the count alone.
    const UINT previousGameCount = _gameBufferCount;

    if (_upgraded)
    {
        SwapChainFlags = SdrToHdr::UpgradedResizeFlags(SwapChainFlags);

        if (BufferCount != 0)
        {
            _gameBufferCount = BufferCount;
            BufferCount = SdrToHdr::UpgradedResizeCount(BufferCount);
        }
    }

#ifdef USE_LOCAL_MUTEX
    // dlssg calls this from present it seems
    // don't try to get a mutex when present owns it while dlssg mod is enabled
    if (!(_localMutex.getOwner() == 4 && State::Instance().activeFgNvngx != FGNvngxReplacement::None))
    {
        OwnedLockGuard lock(_localMutex, 1);
    }
#endif

    // Setting can be changed from the menu during the resize, unlock what we locked
    IFGFeature_Dx12* lockedFg = nullptr;
    if (State::Instance().currentFG != nullptr && Config::Instance()->FGUseMutexForSwapchain.value_or_default())
    {
        lockedFg = State::Instance().currentFG;
        LOG_TRACE("Waiting ffxMutex 3, current: {}", lockedFg->Mutex.getOwner());
        lockedFg->Mutex.lock(3);
        LOG_TRACE("Accuired ffxMutex: {}", lockedFg->Mutex.getOwner());
    }

    HRESULT result;
    DXGI_SWAP_CHAIN_DESC desc {};
    _real->GetDesc(&desc);

    if (Config::Instance()->IsFGEnabled())
    {
        State::Instance().fgResetCapturedResources = true;
        State::Instance().fgOnlyUseCapturedResources = false;
        State::Instance().fgChanged = true;
    }

    // Not on a dxvk game whose menu the Vulkan present bridge draws (see Release).
    if (!VkPresentBridge::DxvkThroughVulkan())
        MenuOverlayDx::CleanupRenderTarget(true, _handle);

    State::Instance().scChanged = true;

    if (!_composition && Config::Instance()->OverrideVsync.value_or_default() && !State::Instance().SCExclusiveFullscreen &&
        State::Instance().currentFG == nullptr)
    {
        LOG_DEBUG("Overriding flags");
        SwapChainFlags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

        if (BufferCount < 2)
            BufferCount = 2;
    }

    State::Instance().SCAllowTearing = (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) > 0;

    LOG_DEBUG("BufferCount: {0}, Width: {1}, Height: {2}, NewFormat: {3}, SwapChainFlags: {4:X}", BufferCount, Width,
              Height, (UINT) NewFormat, SwapChainFlags);

    WaitForGPUIdle(_device);

    // Release swapchain backbuffers to prevent errors when resizing
    /*

    const bool outputRequiresRelease =
        State::Instance().activeFgOutput == FGOutput::FSRFG || UsesXeFGApi(State::Instance().activeFgOutput);

    if (outputRequiresRelease && State::Instance().currentFG != nullptr)
    {
        IDXGISwapChain* skSC = nullptr;
        if (_real->QueryInterface(IID_IUnwrappedDXGISwapChain, (void**) &skSC) == S_OK && skSC != nullptr)
        {
            skSC->Release();
            LOG_DEBUG("Found SK swapchain, skip releasing backbuffers of main swapchain");
        }
        else
        {
            LOG_DEBUG("Releasing backbuffers, count: {}", desc.BufferCount);

            for (UINT i = 0; i < desc.BufferCount; i++)
            {
                ID3D12Resource* backBuffer = nullptr;
                auto bbResult = _real->GetBuffer(i, IID_PPV_ARGS(&backBuffer));

                if (bbResult == S_OK)
                {
                    backBuffer->AddRef();

                    auto refCount = backBuffer->Release();
                    refCount = backBuffer->Release();
                    LOG_DEBUG("Current backbuffer {}, RefCount {}", i, refCount);

                    while (refCount > 1 && refCount < 4294967200ul)
                    {
                        refCount = backBuffer->Release();
                        LOG_DEBUG("Releasing backbuffer {}, RefCount {}", i, refCount);
                    }

                    LOG_DEBUG("Backbuffer {}, RefCount {}", i, refCount);
                }
                else
                {
                    LOG_DEBUG("GetBuffer failed for index {}: {:X}", i, (UINT) bbResult);
                    break;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    */

#ifdef DXGI_DEBUG_ENABLED
    ReportDXGILiveObjects();

#ifdef ENABLE_DEBUG_LAYER_DX12
    ReportD3D12LiveObjects(State::Instance().currentD3D12Device);
#endif
#endif

    // HDR output (on, or still on this chain): decides again for the new state and does the resize itself when the chain is
    // or becomes proxied. Off, and never proxied: skipped, the resize below is what it always was.
    bool hdrHandled = false;

    if (Config::Instance()->HdrOutputEnabled.value_or_default() || Proxy() != nullptr)
    {
        _lastFlags = SwapChainFlags;
        result = ResizeHdr(false, BufferCount, Width, Height, NewFormat, SwapChainFlags, nullptr, nullptr, hdrHandled);
    }

    if (hdrHandled)
    {
        // done by HDR output
    }
    else if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
    {
        ScopedSkipHeapCapture skipHeapCapture {};

        _lastFlags = SwapChainFlags;
        result = _real->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }
    else
    {
        _lastFlags = SwapChainFlags;
        result = _real->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }

    if (result == DXGI_ERROR_DEVICE_REMOVED && State::Instance().currentD3D12Device != nullptr)
        Util::GetDeviceRemovedReason(State::Instance().currentD3D12Device);

#ifdef DXGI_DEBUG_ENABLED
    if (result != S_OK)
        ReadDxgiInfoQueue();
#endif

    if (result == S_OK && State::Instance().currentFeature == nullptr)
    {
        State::Instance().screenWidth = static_cast<float>(Width);
        State::Instance().screenHeight = static_cast<float>(Height);
        State::Instance().lastMipBias = 100.0f;
        State::Instance().lastMipBiasMax = -100.0f;
    }

    if (_upgraded && FAILED(result))
        _gameBufferCount = previousGameCount; // DXGI keeps the old count when a resize fails

    LOG_DEBUG("result: {0:X}", (UINT) result);

    if (lockedFg != nullptr)
    {
        LOG_TRACE("Releasing ffxMutex: {}", lockedFg->Mutex.getOwner());
        lockedFg->Mutex.unlockThis(3);
    }

    CheckForHdrOutput();

    return result;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters)
{
    // Emulated full screen keeps the popup over the monitor: a smaller mode would shrink it to a corner. (A ResizeTarget made
    // before SetFullscreenState(TRUE) goes through; Enter then covers the monitor anyway.)
    if (_borderless.Active())
        return S_OK;

    return _real->ResizeTarget(pNewTargetParameters);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetContainingOutput(IDXGIOutput** ppOutput)
{
    return _real->GetContainingOutput(ppOutput);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats)
{
    return _real->GetFrameStatistics(pStats);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetLastPresentCount(UINT* pLastPresentCount)
{
    return _real->GetLastPresentCount(pLastPresentCount);
}

//
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc)
{
    const HRESULT result = _real1->GetDesc1(pDesc);

    if (auto* proxy = Proxy(); SUCCEEDED(result) && pDesc != nullptr && proxy != nullptr && proxy->Engaged())
        pDesc->Format = proxy->ReportedFormat();

    if (_upgraded && SUCCEEDED(result) && pDesc != nullptr)
    {
        pDesc->SwapEffect = _gameSwapEffect;
        pDesc->BufferCount = _gameBufferCount;
    }

    return result;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc)
{
    const HRESULT result = _real1->GetFullscreenDesc(pDesc);

    if (SUCCEEDED(result) && pDesc != nullptr && _borderless.Active())
        pDesc->Windowed = FALSE;

    return result;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetHwnd(HWND* pHwnd) { return _real1->GetHwnd(pHwnd); }

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetCoreWindow(REFIID refiid, void** ppUnk)
{
    return _real1->GetCoreWindow(refiid, ppUnk);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::Present1(UINT SyncInterval, UINT Flags,
                                                           const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    if (_real1 == nullptr)
        return DXGI_ERROR_DEVICE_REMOVED;

#ifdef USE_LOCAL_MUTEX
    OwnedLockGuard lock(_localMutex, 5);
#endif

    if (_composition && !IsCompositionWindow(_handle))
        _handle = FindCompositionWindow();
    if (_composition && _handle == nullptr)
        return _real1->Present1(SyncInterval, Flags, pPresentParameters);

    if (PassesThrough())
        return _real1->Present1(SyncInterval, Flags, pPresentParameters);

    _presentedAsMain = true;

    HRESULT result;

    // A flip model chain refuses dirty rectangles (FLIP_DISCARD) and a scroll rectangle (both flip effects) with
    // DXGI_ERROR_INVALID_CALL, which a bitblt chain took (read from DXGI on WARP and hardware). They are hints for a game that
    // asked for bitblt: a chain made flip model for HDR output presents without them.
    if (_upgraded && pPresentParameters != nullptr)
    {
        if (pPresentParameters->DirtyRectsCount != 0 || pPresentParameters->pScrollRect != nullptr)
        {
            if (!_presentParamsLogged.exchange(true))
                LOG_INFO("HDR output: Present1 dirty / scroll rectangles are left out on a chain made flip model");
        }

        pPresentParameters = nullptr;
    }

    if ((Flags & DXGI_PRESENT_TEST) == 0)
    {
        result = LocalPresent(_real1, SyncInterval, Flags, pPresentParameters, _device, _handle, _uwp, Proxy(),
                              &_display, _upgraded);
        RecoverFromExclusive(result);

        // When Reflex can't be used to limit (and Optical F5Low's low latency does not keep the cap), sleep in present
        if (!State::Instance().reflexLimitsFps && !native::lowlatency::OwnsFrameCap() &&
            State::Instance().activeFgOutput == FGOutput::NoFG &&
            !IdentifyGpu::getPrimaryGpu().usesDxvk && !XellHooks::canLimit())
            FrameLimit::sleep();
    }
    else
    {
        // (Present1 does not take a null pointer: with the rectangles left out it is a plain Present)
        result = pPresentParameters != nullptr ? _real1->Present1(SyncInterval, Flags, pPresentParameters)
                                               : _real->Present(SyncInterval, Flags);
    }

    return result;
}

BOOL STDMETHODCALLTYPE WrappedIDXGISwapChain4::IsTemporaryMonoSupported(void)
{
    return _real1->IsTemporaryMonoSupported();
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput)
{
    return _real1->GetRestrictToOutput(ppRestrictToOutput);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetBackgroundColor(const DXGI_RGBA* pColor)
{
    return _real1->SetBackgroundColor(pColor);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetBackgroundColor(DXGI_RGBA* pColor)
{
    return _real1->GetBackgroundColor(pColor);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetRotation(DXGI_MODE_ROTATION Rotation)
{
    return _real1->SetRotation(Rotation);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetRotation(DXGI_MODE_ROTATION* pRotation)
{
    return _real1->GetRotation(pRotation);
}

//
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetSourceSize(UINT Width, UINT Height)
{
    return _real2->SetSourceSize(Width, Height);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetSourceSize(UINT* pWidth, UINT* pHeight)
{
    return _real2->GetSourceSize(pWidth, pHeight);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetMaximumFrameLatency(UINT MaxLatency)
{
    return _real2->SetMaximumFrameLatency(MaxLatency);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetMaximumFrameLatency(UINT* pMaxLatency)
{
    return _real2->GetMaximumFrameLatency(pMaxLatency);
}

HANDLE STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetFrameLatencyWaitableObject(void)
{
    return _real2->GetFrameLatencyWaitableObject();
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix)
{
    return _real2->SetMatrixTransform(pMatrix);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix)
{
    return _real2->GetMatrixTransform(pMatrix);
}

UINT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetCurrentBackBufferIndex(void)
{
    auto index = _real3->GetCurrentBackBufferIndex();
    // LOG_TRACE("index: {}", index);
    return index;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                                         UINT* pColorSpaceSupport)
{
    // HDR output: the real chain is FP16 and would answer scRGB only. The game has an 8-bit chain, so it gets the answer an
    // 8-bit flip chain gives (SdrToHdr::EightBitChainTakesColourSpace, read from DXGI itself)
    if (Proxied())
    {
        if (pColorSpaceSupport == nullptr)
            return E_INVALIDARG;

        *pColorSpaceSupport = SdrToHdr::EightBitChainTakesColourSpace(ColorSpace)
                                  ? (UINT) DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT
                                  : 0u;
        return S_OK;
    }

    return _real3->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace)
{
    // HDR output is converting this chain: the real chain's colour space is ours. Record the game's, do not forward it.
    if (Proxied())
    {
        // DXGI refuses a space the chain's format cannot take with E_INVALIDARG (8-bit chain, scRGB: 80070057)
        if (!SdrToHdr::EightBitChainTakesColourSpace(ColorSpace))
            return E_INVALIDARG;

        _gameColourSpace = ColorSpace;

        if (ColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 && ColorSpace != DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709 &&
            !_hdrColourSpaceLogged)
        {
            _hdrColourSpaceLogged = true;
            LOG_INFO("HDR output: the game asked for colour space {} while its picture is being converted; recorded, "
                     "not forwarded",
                     (int) ColorSpace);
        }

        return S_OK;
    }

    auto result = _real3->SetColorSpace1(ColorSpace);

    if (SUCCEEDED(result))
    {
        _gameColourSpace = ColorSpace;
        UpdateOutputColorSpace(ColorSpace);

        CheckForHdrOutput();

        LOG_INFO("Output HDR Active: {}", State::Instance().hdrOutputActive);

        MenuOverlayDx::ApplyThemeStyle();
    }

    // What one unit of the buffer means, which is the question the white point is really asking.
    //
    // Two of these encodings are absolute. PQ (ST.2084) puts 1.0 at 10,000 nits by definition, and
    // scRGB -- linear, Rec.709 primaries -- puts 1.0 at 80 nits. In either the divisor this pass
    // wants is arithmetic rather than a guess or a reading: paper white in nits over the unit. The
    // rest are relative and say nothing about scale.
    //
    // Logged rather than used, for now. Whether a game that reports one of these actually honours it
    // is the thing worth knowing before anything is built on it.
    const char* meaning = "relative -- no scale to be had";
    const char* name = "other";

    switch (ColorSpace)
    {
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
        name = "PQ / ST.2084 (HDR10)";
        meaning = "absolute: 1.0 = 10000 nits, so 203-nit paper white = 0.0203";
        break;
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
        name = "scRGB (linear, Rec.709)";
        meaning = "absolute: 1.0 = 80 nits, so 203-nit paper white = 2.5375";
        break;
    case DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020:
        name = "HLG";
        break;
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020:
        name = "Rec.2020, gamma 2.2";
        break;
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
        name = "sRGB (SDR)";
        break;
    default:
        break;
    }

    LOG_INFO("DLSS-NR: swapchain colour space {} -- {} ({})", (int) ColorSpace, name, meaning);

    if (SUCCEEDED(result))
        DlssNr::FinishedPictureColorSpace(_real3, ColorSpace);

    return result;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height,
                                                                 DXGI_FORMAT Format, UINT SwapChainFlags,
                                                                 const UINT* pCreationNodeMask,
                                                                 IUnknown* const* ppPresentQueue)
{
    // As in ResizeBuffers
    if (PassesThrough())
        return _real3->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask,
                                      ppPresentQueue);

    // HDR output: a resize inside a resize on this thread (a window message the game answers with its own ResizeBuffers) is
    // refused; one from another thread waits for the running one (SdrToHdr::ProxyChainBase::Resize)
    if (auto* proxy = Proxy(); proxy != nullptr && proxy->InResizeOnThisThread())
        return DXGI_ERROR_INVALID_CALL;

    if (!DlssNr::WaitForFinishedPicture())
        return DXGI_ERROR_WAS_STILL_DRAWING;
    LOG_DEBUG("");

    // As in ResizeBuffers: two buffers at least, no GDI-compatible flag, after the early return above
    const UINT previousGameCount = _gameBufferCount;
    const UINT askedCount = BufferCount;
    std::vector<UINT> upgradedMasks;
    std::vector<IUnknown*> upgradedQueues;

    if (_upgraded)
    {
        SwapChainFlags = SdrToHdr::UpgradedResizeFlags(SwapChainFlags);

        if (BufferCount != 0)
        {
            _gameBufferCount = BufferCount;
            BufferCount = SdrToHdr::UpgradedResizeCount(BufferCount);
        }

        // The node mask and present queue arrays have the GAME's count of entries; the real call needs one per real buffer.
        // The missing entries repeat the last one (a chain that is proxied has one node and one queue, see MultiNode /
        // OtherPresentQueue), so no loop below or in DXGI reads past an array.
        if (pCreationNodeMask != nullptr || ppPresentQueue != nullptr)
        {
            const UINT gameCount = askedCount != 0 ? askedCount : previousGameCount;
            UINT realCount = BufferCount;

            if (realCount == 0)
            {
                DXGI_SWAP_CHAIN_DESC now {};

                if (SUCCEEDED(_real->GetDesc(&now)))
                    realCount = now.BufferCount;
            }

            if (gameCount != 0 && realCount > gameCount)
            {
                if (pCreationNodeMask != nullptr)
                {
                    upgradedMasks.assign(pCreationNodeMask, pCreationNodeMask + gameCount);
                    upgradedMasks.resize(realCount, upgradedMasks.back());
                    pCreationNodeMask = upgradedMasks.data();
                }

                if (ppPresentQueue != nullptr)
                {
                    upgradedQueues.assign(ppPresentQueue, ppPresentQueue + gameCount);
                    upgradedQueues.resize(realCount, upgradedQueues.back());
                    ppPresentQueue = upgradedQueues.data();
                }
            }
        }
    }

#ifdef USE_LOCAL_MUTEX
    // dlssg calls this from present it seems
    // don't try to get a mutex when present owns it while dlssg mod is enabled
    if (!(_localMutex.getOwner() == 4 && State::Instance().activeFgNvngx != FGNvngxReplacement::None))
    {
        OwnedLockGuard lock(_localMutex, 2);
    }
#endif

    if (ppPresentQueue != nullptr && *ppPresentQueue != nullptr)
    {
        auto state = &State::Instance();
        state->currentCommandQueue = (ID3D12CommandQueue*) *ppPresentQueue;
        _device = state->currentCommandQueue;
    }

    // currentFG can still be null here
    IFGFeature_Dx12* lockedFg = nullptr;
    if (State::Instance().activeFgOutput == FGOutput::FSRFG && State::Instance().currentFG != nullptr &&
        Config::Instance()->FGUseMutexForSwapchain.value_or_default())
    {
        lockedFg = State::Instance().currentFG;
        LOG_TRACE("Waiting ffxMutex 3, current: {}", lockedFg->Mutex.getOwner());
        lockedFg->Mutex.lock(3);
        LOG_TRACE("Accuired ffxMutex: {}", lockedFg->Mutex.getOwner());
    }

    HRESULT result = E_FAIL;
    DXGI_SWAP_CHAIN_DESC desc {};
    _real->GetDesc(&desc);

    if (Config::Instance()->IsFGEnabled())
    {
        State::Instance().fgResetCapturedResources = true;
        State::Instance().fgOnlyUseCapturedResources = false;
        State::Instance().fgChanged = true;
    }

    // Not on a dxvk game whose menu the Vulkan present bridge draws (see Release).
    if (!VkPresentBridge::DxvkThroughVulkan())
        MenuOverlayDx::CleanupRenderTarget(true, _handle);

    State::Instance().scChanged = true;

    if (!_composition && Config::Instance()->OverrideVsync.value_or_default() && !State::Instance().SCExclusiveFullscreen &&
        State::Instance().currentFG == nullptr)
    {
        LOG_DEBUG("Overriding flags");
        SwapChainFlags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

        if (BufferCount < 2)
            BufferCount = 2;
    }

    State::Instance().SCAllowTearing = (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) > 0;

    LOG_DEBUG("BufferCount: {}, Width: {}, Height: {}, NewFormat: {}, SwapChainFlags: {:X}", BufferCount, Width, Height,
              (UINT) Format, SwapChainFlags);

    WaitForGPUIdle(_device);

    // Release swapchain backbuffers to prevent errors when resizing
    const bool isUsingOptiFgFeature =
        State::Instance().activeFgOutput == FGOutput::FSRFG || UsesXeFGApi(State::Instance().activeFgOutput);

    if (isUsingOptiFgFeature && State::Instance().currentFG != nullptr)
    {
        IDXGISwapChain* skSC = nullptr;
        if (_real->QueryInterface(IID_IUnwrappedDXGISwapChain, (void**) &skSC) == S_OK && skSC != nullptr)
        {
            skSC->Release();
            LOG_DEBUG(
                "Found SK swapchain, skip releasing backbuffersand using ResizeBuffers instead of ResizeBuffers1");

#ifdef DXGI_DEBUG_ENABLED
            ReportDXGILiveObjects();

#ifdef ENABLE_DEBUG_LAYER_DX12
            ReportD3D12LiveObjects(State::Instance().currentD3D12Device);
#endif
#endif

            if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
            {
                ScopedSkipHeapCapture skipHeapCapture {};

                _lastFlags = SwapChainFlags;
                result = _real3->ResizeBuffers(BufferCount, Width, Height, Format, SwapChainFlags);
            }
            else
            {
                _lastFlags = SwapChainFlags;
                result = _real3->ResizeBuffers(BufferCount, Width, Height, Format, SwapChainFlags);
            }
        }
        /*
        else
        {
            LOG_DEBUG("Releasing backbuffers, count: {}", desc.BufferCount);

            for (UINT i = 0; i < desc.BufferCount; i++)
            {
                ID3D12Resource* backBuffer = nullptr;
                auto bbResult = _real->GetBuffer(i, IID_PPV_ARGS(&backBuffer));

                if (bbResult == S_OK)
                {
                    backBuffer->AddRef();

                    auto refCount = backBuffer->Release();
                    refCount = backBuffer->Release();
                    LOG_DEBUG("Current backbuffer {}, RefCount {}", i, refCount);

                    while (refCount > 1 && refCount < 4294967200ul)
                    {
                        refCount = backBuffer->Release();
                        LOG_DEBUG("Releasing backbuffer {}, RefCount {}", i, refCount);
                    }

                    LOG_DEBUG("Backbuffer {}, RefCount {}", i, refCount);
                }
                else
                {
                    LOG_DEBUG("GetBuffer failed for index {}: {:X}", i, (UINT) bbResult);
                    break;
                }
            }
        }
        */

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

#ifdef DXGI_DEBUG_ENABLED
    ReportDXGILiveObjects();

#ifdef ENABLE_DEBUG_LAYER_DX12
    ReportD3D12LiveObjects(State::Instance().currentD3D12Device);
#endif
#endif

    // HDR output, as in ResizeBuffers
    bool hdrHandled = false;

    if (Config::Instance()->HdrOutputEnabled.value_or_default() || Proxy() != nullptr)
    {
        _lastFlags = SwapChainFlags;
        result = ResizeHdr(true, BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask, ppPresentQueue,
                           hdrHandled);
    }

    if (hdrHandled)
    {
        // done by HDR output
    }
    else if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
    {
        ScopedSkipHeapCapture skipHeapCapture {};

        _lastFlags = SwapChainFlags;
        result = _real3->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask,
                                        ppPresentQueue);
    }
    else
    {
        _lastFlags = SwapChainFlags;
        result = _real3->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask,
                                        ppPresentQueue);
    }

    if (result == DXGI_ERROR_DEVICE_REMOVED && State::Instance().currentD3D12Device != nullptr)
        Util::GetDeviceRemovedReason(State::Instance().currentD3D12Device);

#ifdef DXGI_DEBUG_ENABLED
    if (result != S_OK)
        ReadDxgiInfoQueue();
#endif

    if (result == S_OK && State::Instance().currentFeature == nullptr)
    {
        State::Instance().screenWidth = static_cast<float>(Width);
        State::Instance().screenHeight = static_cast<float>(Height);
        State::Instance().lastMipBias = 100.0f;
        State::Instance().lastMipBiasMax = -100.0f;
    }

    if (_upgraded && FAILED(result))
        _gameBufferCount = previousGameCount; // DXGI keeps the old count when a resize fails

    LOG_DEBUG("result: {0:X}", (UINT) result);

    if (lockedFg != nullptr)
    {
        LOG_TRACE("Releasing ffxMutex: {}", lockedFg->Mutex.getOwner());
        lockedFg->Mutex.unlockThis(3);
    }

    CheckForHdrOutput();

    return result;
}

// HDR output's part of a resize (see SdrToHdr::ProxyChain::Resize). `handled` is false when it left the resize to the caller:
// the chain is not, and does not become, proxied.
HRESULT WrappedIDXGISwapChain4::ResizeHdr(bool v1, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format,
                                          UINT SwapChainFlags, const UINT* pCreationNodeMask,
                                          IUnknown* const* ppPresentQueue, bool& handled)
{
    handled = false;
    CheckForHdrOutput(); // the display the window is on now, with its SDR white level and peak

    DXGI_SWAP_CHAIN_DESC current {};
    _real->GetDesc(&current);

    auto* proxy = Proxy();
    const bool wasProxied = proxy != nullptr && proxy->Active();
    DXGI_FORMAT gameFormat = wasProxied ? proxy->ReportedFormat() : current.BufferDesc.Format;

    if (Format != DXGI_FORMAT_UNKNOWN)
        gameFormat = Format;

    bool d3d11 = false;
    auto* device = ApiDevice(d3d11);

    SdrToHdr::Inputs in;
    in.enabled = Config::Instance()->HdrOutputEnabled.value_or_default();
    in.gameFormat = gameFormat;
    in.flipModel = _real3 != nullptr && (current.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ||
                                         current.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL);
    in.displayKnown = _displayKnown;
    in.displayHdr = _display.hdrOn;
    in.multisampled = current.SampleDesc.Count > 1;
    in.windowed = current.Windowed != FALSE; // (a chain that exists cannot be upgraded: canUpgradeBitBlt stays false)
    in.composition = _composition;
    in.coreWindow = _uwp && !_composition;
    in.fgLibraryChain = _fgLibraryChain;

    const UINT count = BufferCount != 0 ? BufferCount : current.BufferCount;

    if (pCreationNodeMask != nullptr)
    {
        for (UINT i = 1; i < count; ++i)
            in.multiNode = in.multiNode || pCreationNodeMask[i] != pCreationNodeMask[0];
    }

    // The pass runs on the queue the chain was made with: a chain that presents on another one is left alone
    if (ppPresentQueue != nullptr)
    {
        for (UINT i = 0; i < count; ++i)
            in.presentQueueDiffers = in.presentQueueDiffers || ppPresentQueue[i] == nullptr ||
                                     UnwrappedQueueIdentity(ppPresentQueue[i]) != _creationQueue;
    }

    SdrToHdr::FillEnvironment(in);
    const SdrToHdr::Decision decided = SdrToHdr::Decide(in);

    // A chain made flip model for HDR output keeps its proxy through a resize that would end HDR output (see
    // SdrToHdr::KeepUpgradedProxy): the game would otherwise get a real flip buffer, unbound at every Present
    // (also a chain whose proxy failed after the upgrade and is held engaged: it must not fall back to the real buffer)
    const bool upgradedLive = _upgraded && (wasProxied || (proxy != nullptr && proxy->Engaged()));
    const SdrToHdr::Decision decision = SdrToHdr::KeepUpgradedProxy(decided, upgradedLive);
    const bool kept = decision.use && !decided.use;

    if (in.enabled || wasProxied)
    {
        if (kept)
            LOG_INFO("{} (kept: this game's swap chain was changed for HDR; it ends at the next start)",
                     SdrToHdr::ReasonText(decided.reason));
        else
            LOG_INFO("{}", SdrToHdr::ReasonText(decision.reason));
    }

    // Neither a Direct3D 12 queue nor a Direct3D 11 device: nothing to make proxies on
    if (decision.use && device == nullptr)
    {
        SdrToHdr::RecordStatus(SdrToHdr::Reason::StartFailed, false, Config::Instance()->UseHDR10.value_or_default(),
                               in.enabled, _display.maxLuminance);
        return S_OK;
    }

    if (!decision.use && !wasProxied)
    {
        SdrToHdr::RecordStatus(decision.reason, false, Config::Instance()->UseHDR10.value_or_default(), in.enabled,
                               _display.maxLuminance);
        return S_OK;
    }

    handled = true;
    proxy = EnsureProxy(d3d11);

    const bool hdr10 = Config::Instance()->UseHDR10.value_or_default();

    SdrToHdr::ResizeRequest request;
    request.useV1 = v1;
    request.count = BufferCount;
    request.width = Width;
    request.height = Height;
    request.rawFormat = Format;
    request.resolvedFormat = gameFormat;
    request.flags = SwapChainFlags;
    request.nodeMask = pCreationNodeMask;
    request.queues = ppPresentQueue;
    request.wantProxy = decision.use;
    request.hdr10 = hdr10;
    request.gameColourSpace = _gameColourSpace;
    request.noRevert = _upgraded;

    SdrToHdr::ResizeResult result;

    {
        ScopedSkipHeapCapture skipHeapCapture {}; // the proxies are ours: not a game's resources to capture
        result = proxy->Resize(device, _real3, request);
    }

    if (result.note != nullptr)
        LOG_WARN("HDR output: {}", result.note);

    if (result.refused)
        return result.hr; // nothing was changed

    if (result.reverted)
        LOG_WARN("HDR output: the swap chain was put back to the game's format");

    if (result.stuckHdr)
        LOG_ERROR("HDR output: the swap chain could not be put back to the game's format; it stays HDR with nothing "
                  "converting it");

    const bool failedStart = result.reverted || result.stuckHdr;
    // (a chain kept through a resize that would have ended HDR output still did HDR output: recorded as enabled, so turning
    // it back on does not read as a change waiting for a restart)
    SdrToHdr::RecordStatus(result.stuckHdr ? SdrToHdr::Reason::StuckHdr
                           : failedStart   ? SdrToHdr::Reason::StartFailed
                                           : decision.reason,
                           result.proxied, hdr10, in.enabled || kept, _display.maxLuminance);

    // The library's HDR chain stood down (HDR output off, Windows HDR off, a start that failed): the window is no
    // longer kept borderless for it
    if (_libraryWindowed && !result.proxied)
    {
        _libraryWindowed = false;
        FGHooks::ReleaseBorderless(this);
    }

    // A resize kept the proxy of a chain that was made flip model: what it was kept through is said over the active line
    if (kept && result.proxied)
    {
        if (const auto note = SdrToHdr::KeptNote(decided.reason); note != SdrToHdr::Reason::Off)
            SdrToHdr::RecordNote(note);
    }

    if (result.stuckHdr)
        return result.hr; // the colour space is still the HDR one: do not claim otherwise

    // The menu and the finished picture follow the colour space the real chain ended up with
    if (result.proxied)
    {
        UpdateOutputColorSpace(hdr10 ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
        DlssNr::FinishedPictureColorSpace(_real3, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    }
    else if (wasProxied)
    {
        UpdateOutputColorSpace(_gameColourSpace);
        DlssNr::FinishedPictureColorSpace(_real3, _gameColourSpace);
    }

    if (result.proxied || wasProxied)
        MenuOverlayDx::ApplyThemeStyle();

    return result.hr;
}

//
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size,
                                                                 void* pMetaData)
{
    return _real4->SetHDRMetaData(Type, Size, pMetaData);
}
