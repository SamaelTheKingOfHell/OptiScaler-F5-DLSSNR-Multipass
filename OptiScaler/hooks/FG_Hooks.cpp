#include "pch.h"
#include <dlssnr/DlssNr.h>
#include "FG_Hooks.h"
#include <Config.h>
#include <output/SdrToHdr_Create.h>

#include <framegen/ffx/FSRFG_Dx12.h>
#include <framegen/xefg/XeFG_Dx12.h>
#include <framegen/xefg/XeFG_ShownRate.h>
#include <framegen/dlssg/DLSSG_Dx12.h>

#include <inputs/FG/FSR3_Dx12_FG.h>
#include <inputs/FG/FfxApi_Dx12_FG.h>

#include <hudfix/Hudfix_Dx12.h>
#include <resource_tracking/ResTrack_Dx12.h>

#include <misc/FrameLimit.h>

#include <misc/IdentifyGpu.h>
#include <misc/REFrameworkCompat.h>
#include <hooks/Reflex_Hooks.h>
#include <menu/menu_overlay_dx.h>
#include <native/NativeDriverDx12.h>
#include <native/NativeLowLatency.h>
#include <native/PresentStageTiming.h>
#include <native/VkPresentBridge.h>
#include <low_latency/input/input_common.h>
#include <proxies/XeFG_Proxy.h>
#include <proxies/KernelBase_Proxy.h>
#include <imgui/ImGuiNotify.hpp>
#include <framegen/GameFsrFgRule.h>
#include <framegen/FgWindowRule.h>

#include <magic_enum.hpp>

#include <d3d12.h>
#include <detours/detours.h>

#define XEFG_RESOURCE_REF_LIMIT 1

static ID3D12Fence* resizeFence = nullptr;
static UINT64 resizeFenceValue = 0;
static HANDLE resizeFenceEvent = nullptr;
static IUnknown* oldSwapChain = nullptr;
static ID3D12CommandQueue* currentCommandQueue = nullptr;
static bool _forcedHdrForXeFG = false;
static HANDLE _semaphore = nullptr;

#if (XEFG_RESOURCE_REF_LIMIT == 0)
inline static std::vector<void*> oldBackBuffers;
#endif

static bool WaitForQueueIdle(ID3D12CommandQueue* queue, ID3D12Fence* fence, HANDLE fenceEvent, UINT64& fenceValue)
{
    if (queue == nullptr || fence == nullptr || fenceEvent == nullptr)
        return true;

    const UINT64 waitValue = ++fenceValue;
    auto result = queue->Signal(fence, waitValue);
    if (FAILED(result))
    {
        LOG_ERROR("FG/present queue idle Signal failed: {:X}", (UINT) result);
        return false;
    }

    if (fence->GetCompletedValue() >= waitValue)
        return true;

    result = fence->SetEventOnCompletion(waitValue, fenceEvent);
    if (FAILED(result))
    {
        LOG_ERROR("FG/present queue idle SetEventOnCompletion failed. fence {}, completed {}, result {:X}", waitValue,
                  fence->GetCompletedValue(), (UINT) result);
        return false;
    }

    const auto waitResult = WaitForSingleObject(fenceEvent, 5000);
    if (waitResult != WAIT_OBJECT_0)
    {
        LOG_ERROR("FG/present queue idle wait failed. fence {}, completed {}, waitResult {:X}", waitValue,
                  fence->GetCompletedValue(), waitResult);
        return false;
    }

    return true;
}

// The game's own FSR Frame Generation creates this swapchain: XeFG under it crashes (Modern Warfare 4). With FG Input
// FSR-FG OptiScaler replaces that swapchain itself, so it never gets here.
static bool CreatedByGameFsrFg()
{
    const auto ffx = KernelBaseProxy::GetModuleHandleW_()(L"amd_fidelityfx_framegeneration_dx12.dll");

    if (ffx == nullptr)
        return false;

    void* frames[24] {};
    const auto count = CaptureStackBackTrace(1, (DWORD) std::size(frames), frames, nullptr);

    for (USHORT i = 0; i < count; i++)
    {
        HMODULE module = nullptr;

        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR) frames[i], &module) &&
            module == ffx)
        {
            return true;
        }
    }

    return false;
}

// Some games make their swapchain through AMD's FG library even while the player picked DLSS-G: with FG Input DLSSG
// XeFG takes it and only stands aside while the game's FSR FG is really on (framegen/GameFsrFgRule.h,
// XeFG_Dx12::Activate, NoteGameFsrFg). Other FG inputs can't tell the two apart, so they leave the swapchain to the
// game.
static bool RefuseUnderGameFsrFg(HWND hwnd)
{
    auto& state = State::Instance();

    const auto decision =
        GameFsrFg::Decide(UsesXeFGApi(state.activeFgOutput), UsesXeFGApi(state.activeFgOutput) && CreatedByGameFsrFg(),
                          state.activeFgInput == FGInput::DLSSG, state.gameFsrFgSeen, state.gameFsrFgOn);

    state.xefgUnderGameFsrFg = decision == GameFsrFg::Swapchain::Take;

    if (decision == GameFsrFg::Swapchain::NotUnder)
        return false;

    if (decision == GameFsrFg::Swapchain::Take)
    {
        LOG_INFO("The game creates its swapchain through its FSR Frame Generation library, its FSR FG is off: XeFG "
                 "takes it and stands aside while the game's FSR FG is on");
        return false;
    }

    LOG_WARN("The game creates its swapchain through its FSR Frame Generation library, leaving it to the game (FG "
             "Input {}, the game's FSR FG {}, its FFX calls {}seen)",
             magic_enum::enum_name(state.activeFgInput), state.gameFsrFgOn ? "on" : "off",
             state.gameFsrFgSeen ? "" : "not ");

    static bool shown = false;

    if (!shown)
    {
        shown = true;
        ImGui::InsertNotification({ ImGuiToastType::Warning, 15000,
                                    "The game makes its swapchain through its FSR Frame Generation: XeFG can't run "
                                    "under it now.\n"
                                    "Set FG Input to FSR-FG, or to DLSSG with the game's FSR frame generation off." });
    }

    // XeFG's swapchain from before (the game's first one, made before it went through its FSR FG library) still owns
    // this window: the game's new one would fail with E_ACCESSDENIED and the game crash (Cyberpunk 2077 started with
    // its FSR FG on). Released as XeFG releases a same-window swapchain when it makes a new one, also with
    // PreserveSwapChain on (the game's chain can't share the window). As there, none stays current, so the game's own
    // Release of the old one goes through to the end; State's game swapchain pointers follow with the game's new one.
    if (state.currentFGSwapchain != nullptr && state.currentFG != nullptr && state.currentFG->Hwnd() == hwnd)
    {
        if (state.currentCommandQueue != nullptr && resizeFence != nullptr && resizeFenceEvent != nullptr)
        {
            resizeFenceValue++;
            WaitForQueueIdle(state.currentCommandQueue, resizeFence, resizeFenceEvent, resizeFenceValue);
        }

        if (state.currentFG->ReleaseSwapchain(hwnd))
        {
            state.currentFGSwapchain = nullptr;
            oldSwapChain = nullptr;
            LOG_INFO("Released XeFG's swapchain on this window so the game's own can be made");
        }
    }

    return true;
}

static FgWindow::Window DescribeWindow(HWND hwnd)
{
    FgWindow::Window window { hwnd };

    if (hwnd == nullptr || !IsWindow(hwnd))
        return window;

    window.shown = IsWindowVisible(hwnd) != FALSE;

    const bool minimized = IsIconic(hwnd) != FALSE;

    RECT client {};
    if (!GetClientRect(hwnd, &client))
        client = {};

    // The restored size is the window's rect (frame included), a little more than its client area: errs towards
    // keeping frame generation where it is
    WINDOWPLACEMENT placement { sizeof(WINDOWPLACEMENT) };
    RECT restored {};
    if (minimized && GetWindowPlacement(hwnd, &placement))
        restored = placement.rcNormalPosition;

    window.area = FgWindow::WindowArea(minimized, client.right - client.left, client.bottom - client.top,
                                       restored.right - restored.left, restored.bottom - restored.top);

    return window;
}

// The window frame generation's live swapchain is on: SetFGSwapchain's, or the window of the D3D11 interop's plain
// D3D12 chain (SetDx12InteropPresentSC) when that one stands in as currentFGSwapchain. Null without one.
static HWND LiveFgWindow(HWND fgHwnd, IDXGISwapChain* interopSC, HWND interopHwnd)
{
    auto* live = State::Instance().currentFGSwapchain;

    if (live == nullptr)
        return nullptr;

    return interopSC != nullptr && live == interopSC ? interopHwnd : fgHwnd;
}

bool FGHooks::IsOtherWindowThanFG(HWND hwnd)
{
    // The Vulkan bridge's chain: its own call is not guarded, and the library's inner creates for it come through the
    // guarded hooks inside ScopedBridgeCreate
    if (_bridgeCreateDepth > 0)
        return false;

    const HWND fgHwnd = LiveFgWindow(_hwnd, _dx12InteropPresentSC, _dx12InteropPresentHwnd);

    if (fgHwnd == nullptr || hwnd == nullptr || hwnd == fgHwnd)
        return false;

    const auto fg = DescribeWindow(fgHwnd);
    const auto requested = DescribeWindow(hwnd);

    if (!FgWindow::IsOtherWindow(true, fg, requested))
        return false;

    LOG_WARN("Frame generation's swapchain lives on window {:X} ({} px), window {:X} ({} px) gets a plain swapchain "
             "and leaves frame generation's state alone",
             (size_t) fgHwnd, fg.area, (size_t) hwnd, requested.area);

    return true;
}

void FGHooks::NoteGameFsrFgSeen() { State::Instance().gameFsrFgSeen = true; }

void FGHooks::NoteGameFsrFg(bool on)
{
    auto& state = State::Instance();

    if (state.gameFsrFgOn == on)
        return;

    state.gameFsrFgOn = on;
    LOG_INFO("The game's FSR Frame Generation is {}", on ? "on" : "off");

    if (GameFsrFg::StandsAside(state.xefgUnderGameFsrFg, on) && state.currentFG != nullptr &&
        state.currentFG->IsActive())
    {
        state.currentFG->Deactivate();
        state.currentFG->ResetCounters();
    }
}

static bool CheckForFGStatus()
{
    // Need to check overlay menu parameter, goes to places it shouldn't go
    // if (!Config::Instance()->OverlayMenu.value_or_default())
    //    return false;

    if (State::Instance().activeFgInput == FGInput::NoFG || State::Instance().activeFgInput == FGInput::NvngxFG)
        return false;

    // Disable FG if amd dll is not found
    if (State::Instance().activeFgOutput == FGOutput::FSRFG)
    {
        FfxApiProxy::InitFfxDx12();
        if (!FfxApiProxy::IsFGReady())
        {
            ImGui::InsertNotification(
                { ImGuiToastType::Error, 20000, "Can't init FSR FG\nAre you missing the required DLLs?" });

            LOG_DEBUG("Can't init FfxApiProxy, disabling FGOutput");
            Config::Instance()->FGOutput.set_volatile_value(FGOutput::NoFG);
            State::Instance().activeFgOutput = Config::Instance()->FGOutput.value_or_default();
        }
    }
    else if (UsesXeFGApi(State::Instance().activeFgOutput) && !XeFGProxy::InitXeFG())
    {
        ImGui::InsertNotification(
            { ImGuiToastType::Error, 20000, "Can't init XeFG\nAre you missing the required DLLs?" });

        LOG_DEBUG("Can't init XeFGProxy, disabling FGOutput");
        Config::Instance()->FGOutput.set_volatile_value(FGOutput::NoFG);
        State::Instance().activeFgOutput = Config::Instance()->FGOutput.value_or_default();
    }
    else if (State::Instance().activeFgOutput == FGOutput::DLSSG && !StreamlineProxy::LoadStreamline())
    {
        ImGui::InsertNotification(
            { ImGuiToastType::Error, 20000, "Can't init DLSSG Output\nAre you missing the streamline folder?" });

        LOG_DEBUG("Can't init StreamlineProxy, disabling FGOutput");
        Config::Instance()->FGOutput.set_volatile_value(FGOutput::NoFG);
        State::Instance().activeFgOutput = Config::Instance()->FGOutput.value_or_default();

        Config::Instance()->FGNvngxReplacement.set_volatile_value(FGNvngxReplacement::None);
        State::Instance().activeFgNvngx = Config::Instance()->FGNvngxReplacement.value_or_default();
    }

    if (State::Instance().activeFgOutput == FGOutput::NoFG)
    {
        LOG_WARN("FGOutput doesn't have any FG active");
        return false;
    }

    return true;
}

HRESULT FGHooks::CreateSwapChain(IDXGIFactory* pFactory, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc,
                                 IDXGISwapChain** ppSwapChain)
{
    if (!CheckForFGStatus())
    {
        LOG_WARN("Can't init FG Feature or invalid FGOutput setting!");
        return E_NOINTERFACE;
    }

    if (RefuseUnderGameFsrFg(pDesc->OutputWindow))
        return E_NOINTERFACE;

    // Check if it's Dx12
    ID3D12CommandQueue* cq = nullptr;
    if (pDevice->QueryInterface(IID_PPV_ARGS(&cq)) != S_OK)
    {
        LOG_ERROR("FG Feature requires D3D12 Command Queue!");
        return E_INVALIDARG;
    }

    if (State::Instance().currentFG == nullptr)
    {
        // FG Init
        if (State::Instance().activeFgOutput == FGOutput::FSRFG)
        {
            State::Instance().currentFG = new FSRFG_Dx12();
        }
        else if (UsesXeFGApi(State::Instance().activeFgOutput))
        {
            State::Instance().currentFG = new XeFG_Dx12();
        }
        else if (State::Instance().activeFgOutput == FGOutput::DLSSG)
        {
            State::Instance().currentFG = new DLSSG_Dx12();
        }
    }

    // Create FG swapchain
    auto fg = State::Instance().currentFG;
    bool scResult = false;

    {
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};

        if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
            State::Instance().skipHeapCapture = true;

        if (UsesXeFGApi(State::Instance().activeFgOutput) && !pDesc->Windowed)
            LOG_WARN("Using exclusive fullscreen with XeFG!!!");

        // These effects are not supported in DX12
        if (pDesc->SwapEffect == DXGI_SWAP_EFFECT_SEQUENTIAL)
        {
            LOG_WARN("DXGI_SWAP_EFFECT_SEQUENTIAL is not supported in DX12, changing to FLIP_SEQUENTIAL");
            pDesc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        }
        else if (pDesc->SwapEffect == DXGI_SWAP_EFFECT_DISCARD)
        {
            LOG_WARN("DXGI_SWAP_EFFECT_DISCARD is not supported in DX12, changing to FLIP_DISCARD");
            pDesc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        }

        // Looks like game is creating new swapchain,
        // without releasing old one, be sure gpu is in idle state
        if (State::Instance().currentFGSwapchain != nullptr)
        {
            LOG_WARN("Looks like game is creating new swapchain, without releasing old one!");

            if (State::Instance().currentCommandQueue != nullptr && resizeFence != nullptr &&
                resizeFenceEvent != nullptr)
            {
                LOG_DEBUG("Waiting for GPU to finish");

                resizeFenceValue++;
                const auto waitResult = WaitForQueueIdle(State::Instance().currentCommandQueue, resizeFence,
                                                         resizeFenceEvent, resizeFenceValue);

                LOG_DEBUG("WaitForSingleObject result: {}", waitResult);
            }
        }

        scResult = fg->CreateSwapchain(pFactory, cq, pDesc, ppSwapChain, true);

        // Taken after the create. XeFG and FSR-FG release a same-window swapchain in there and leave none
        // current, so the game's own Release of it goes through. Still current: one on another window (its
        // Releases stay skipped, so it leaks, as before), a preserved one, or DLSSG's (which frees it itself).
        // Null otherwise, so a later swapchain at a freed one's address is not skipped
        oldSwapChain = State::Instance().currentFGSwapchain;

        if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
            State::Instance().skipHeapCapture = false;
    }

    if (scResult)
    {
        if (State::Instance().currentD3D12Device != nullptr)
        {
            SAFE_RELEASE(resizeFence);
            SAFE_CLOSE_HANDLE(resizeFenceEvent);

            State::Instance().currentD3D12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&resizeFence));
            resizeFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        }

        SdrToHdr::NoteGameFacingFrameGenChain();
        SetFGSwapchain(*ppSwapChain, pDesc->OutputWindow);

        return S_OK;
    }

    return E_INVALIDARG;
}

HRESULT FGHooks::CreateSwapChainForHwnd(IDXGIFactory* pFactory, IUnknown* pDevice, HWND hWnd,
                                        DXGI_SWAP_CHAIN_DESC1* pDesc, DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                        IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain)
{
    if (!CheckForFGStatus())
    {
        LOG_WARN("Can't init FG Feature or invalid FGOutput setting!");
        return E_NOINTERFACE;
    }

    if (RefuseUnderGameFsrFg(hWnd))
        return E_NOINTERFACE;

    // Check if it's Dx12
    ID3D12CommandQueue* cq = nullptr;
    if (pDevice->QueryInterface(IID_PPV_ARGS(&cq)) != S_OK)
    {
        LOG_ERROR("FG Feature requires D3D12 Command Queue!");
        return E_INVALIDARG;
    }

    if (State::Instance().currentFG == nullptr)
    {
        // FG Init
        if (State::Instance().activeFgOutput == FGOutput::FSRFG)
        {
            State::Instance().currentFG = new FSRFG_Dx12();
        }
        else if (UsesXeFGApi(State::Instance().activeFgOutput))
        {
            State::Instance().currentFG = new XeFG_Dx12();
        }
        else if (State::Instance().activeFgOutput == FGOutput::DLSSG)
        {
            State::Instance().currentFG = new DLSSG_Dx12();
        }
    }

    // Create FG swapchain
    auto fg = State::Instance().currentFG;
    bool scResult = false;

    {
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};

        if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
            State::Instance().skipHeapCapture = true;

        if (UsesXeFGApi(State::Instance().activeFgOutput) && pFullscreenDesc != nullptr && !pFullscreenDesc->Windowed)
            LOG_WARN("Using exclusive fullscreen with XeFG!!!");

        // These effects are not supported in DX12
        if (pDesc->SwapEffect == DXGI_SWAP_EFFECT_SEQUENTIAL)
        {
            LOG_WARN("DXGI_SWAP_EFFECT_SEQUENTIAL is not supported in DX12, changing to FLIP_SEQUENTIAL");
            pDesc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        }
        else if (pDesc->SwapEffect == DXGI_SWAP_EFFECT_DISCARD)
        {
            LOG_WARN("DXGI_SWAP_EFFECT_DISCARD is not supported in DX12, changing to FLIP_DISCARD");
            pDesc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        }

        // Looks like game is creating new swapchain,
        // without releasing old one, be sure gpu is in idle state
        if (State::Instance().currentFGSwapchain != nullptr)
        {
            LOG_WARN("Looks like game is creating new swapchain, without releasing old one!");

            if (State::Instance().currentCommandQueue != nullptr && resizeFence != nullptr &&
                resizeFenceEvent != nullptr)
            {
                LOG_DEBUG("Waiting for GPU to finish");

                resizeFenceValue++;
                const auto waitResult = WaitForQueueIdle(State::Instance().currentCommandQueue, resizeFence,
                                                         resizeFenceEvent, resizeFenceValue);

                LOG_DEBUG("WaitForSingleObject result: {}", waitResult);
            }
        }

        scResult = fg->CreateSwapchain1(pFactory, cq, hWnd, pDesc, pFullscreenDesc, ppSwapChain, true);

        // Taken after the create. XeFG and FSR-FG release a same-window swapchain in there and leave none
        // current, so the game's own Release of it goes through. Still current: one on another window (its
        // Releases stay skipped, so it leaks, as before), a preserved one, or DLSSG's (which frees it itself).
        // Null otherwise, so a later swapchain at a freed one's address is not skipped
        oldSwapChain = State::Instance().currentFGSwapchain;

        if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
            State::Instance().skipHeapCapture = false;
    }

    if (scResult)
    {
        if (State::Instance().currentD3D12Device != nullptr)
        {
            SAFE_RELEASE(resizeFence);
            SAFE_CLOSE_HANDLE(resizeFenceEvent);

            State::Instance().currentD3D12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&resizeFence));
            resizeFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        }

        SdrToHdr::NoteGameFacingFrameGenChain();
        SetFGSwapchain((IDXGISwapChain*) *ppSwapChain, hWnd);

        return S_OK;
    }

    return E_INVALIDARG;
}

void FGHooks::SetFGSwapchain(IDXGISwapChain* pSwapChain, HWND hWnd)
{
    if (pSwapChain == nullptr)
        return;

    _hwnd = hWnd;

    if (_dx12InteropPresentSC == pSwapChain)
    {
        _dx12InteropPresentSC = nullptr;
        _dx12InteropPresentHwnd = nullptr;
    }

    State::Instance().currentFGSwapchain = pSwapChain;
    State::Instance().currentSwapchain = pSwapChain;

    HookFGSwapchain(pSwapChain);
}

void FGHooks::SetDx12InteropPresentSC(IDXGISwapChain* pSwapChain, HWND hWnd)
{
    if (pSwapChain == nullptr)
        return;

    _dx12InteropPresentSC = pSwapChain;
    _dx12InteropPresentHwnd = hWnd;

    LOG_INFO("Set DX12 presenting swapchain: {:X}, hWnd: {:X}", (size_t) pSwapChain, (size_t) hWnd);
}

void FGHooks::ClearDx12InteropPresentSC(IUnknown* pSwapChain)
{
    if (pSwapChain == nullptr || pSwapChain != _dx12InteropPresentSC)
        return;

    LOG_INFO("Clearing DX12 presenting swapchain: {:X}", (size_t) pSwapChain);

    _dx12InteropPresentSC = nullptr;
    _dx12InteropPresentHwnd = nullptr;
}

bool FGHooks::IsDx12InteropPresentSC(IUnknown* pSwapChain)
{
    return pSwapChain != nullptr && pSwapChain == _dx12InteropPresentSC;
}

void FGHooks::HookFGSwapchain(IDXGISwapChain* pSwapChain)
{
    if (o_FGSCPresent != nullptr || pSwapChain == nullptr)
        return;

    void** pFactoryVTable = *reinterpret_cast<void***>(pSwapChain);

    o_FGRelease = (PFN_Release) pFactoryVTable[2];
    o_FGSCPresent = (PFN_Present) pFactoryVTable[8];
    o_FGSCSetFullscreenState = (PFN_SetFullscreenState) pFactoryVTable[10];
    o_FGSCGetFullscreenState = (PFN_GetFullscreenState) pFactoryVTable[11];
    o_FGSCResizeBuffers = (PFN_ResizeBuffers) pFactoryVTable[13];
    o_FGSCResizeTarget = (PFN_ResizeTarget) pFactoryVTable[14];
    o_FGSCGetFullscreenDesc = (PFN_GetFullscreenDesc) pFactoryVTable[19];
    o_FGSCPresent1 = (PFN_Present1) pFactoryVTable[22];
    o_FGSCGetFrameLatencyWaitableObject = (PFN_GetFrameLatencyWaitableObject) pFactoryVTable[33];
    o_FGSCResizeBuffers1 = (PFN_ResizeBuffers1) pFactoryVTable[39];

    if (o_FGSCPresent != nullptr)
    {
        LOG_INFO("Hooking FG SwapChain present");
        LOG_TRACE("FGRelease: {:X}", (size_t) o_FGRelease);
        LOG_TRACE("FGSCPresent: {:X}", (size_t) o_FGSCPresent);
        LOG_TRACE("FGSCSetFullscreenState: {:X}", (size_t) o_FGSCSetFullscreenState);
        LOG_TRACE("FGSCGetFullscreenState: {:X}", (size_t) o_FGSCGetFullscreenState);
        LOG_TRACE("FGSCResizeBuffers: {:X}", (size_t) o_FGSCResizeBuffers);
        LOG_TRACE("FGSCResizeTarget: {:X}", (size_t) o_FGSCResizeTarget);
        LOG_TRACE("FGSCGetFullscreenDesc: {:X}", (size_t) o_FGSCGetFullscreenDesc);
        LOG_TRACE("FGSCPresent1: {:X}", (size_t) o_FGSCPresent1);
        LOG_TRACE("FGSCResizeBuffers1: {:X}", (size_t) o_FGSCResizeBuffers1);
        LOG_TRACE("FGSCGetFrameLatencyWaitableObject: {:X}", (size_t) o_FGSCGetFrameLatencyWaitableObject);

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        DetourAttach(&(PVOID&) o_FGRelease, hkFGRelease);
        DetourAttach(&(PVOID&) o_FGSCPresent, hkFGPresent);
        DetourAttach(&(PVOID&) o_FGSCResizeTarget, hkResizeTarget);
        DetourAttach(&(PVOID&) o_FGSCResizeBuffers, hkResizeBuffers);
        DetourAttach(&(PVOID&) o_FGSCSetFullscreenState, hkSetFullscreenState);

        if (o_FGSCPresent1 != nullptr)
            DetourAttach(&(PVOID&) o_FGSCPresent1, hkFGPresent1);

        if (o_FGSCResizeBuffers1 != nullptr)
            DetourAttach(&(PVOID&) o_FGSCResizeBuffers1, hkResizeBuffers1);

        // What the game is told about full screen. hkGetFullscreenState / hkGetFullscreenDesc change nothing unless the
        // chain is kept borderless (FGXeFGForceBorderless, or HDR output's windowed chain, KeepBorderless);
        // FGXeFGForceBorderless thereby also governs what an FSR FG game is told, whose swap chain forwards both calls
        // to the real (windowed) chain
        if (FgAnswersFullscreenForGame(State::Instance().activeFgOutput))
        {
            if (o_FGSCGetFullscreenState != nullptr)
                DetourAttach(&(PVOID&) o_FGSCGetFullscreenState, hkGetFullscreenState);

            if (o_FGSCGetFullscreenDesc != nullptr)
                DetourAttach(&(PVOID&) o_FGSCGetFullscreenDesc, hkGetFullscreenDesc);
        }

        if (UsesXeFGApi(State::Instance().activeFgOutput))
        {
            if ((Config::Instance()->SimulateWaitableObject.value_or_default() ||
                 (State::Instance().gameEngine == GameEngineType::Unity &&
                  UsesXeFGApi(State::Instance().activeFgOutput))) &&
                o_FGSCGetFrameLatencyWaitableObject != nullptr)
            {
                DetourAttach(&(PVOID&) o_FGSCGetFrameLatencyWaitableObject, hkGetFrameLatencyWaitableObject);
            }
        }

        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to attach detour: {:X}", detourResult);
            o_FGRelease = nullptr;
            o_FGSCPresent = nullptr;
            o_FGSCSetFullscreenState = nullptr;
            o_FGSCGetFullscreenState = nullptr;
            o_FGSCResizeBuffers = nullptr;
            o_FGSCResizeTarget = nullptr;
            o_FGSCGetFullscreenDesc = nullptr;
            o_FGSCPresent1 = nullptr;
            o_FGSCResizeBuffers1 = nullptr;
            o_FGSCGetFrameLatencyWaitableObject = nullptr;
        }
    }
}

HRESULT FGHooks::hkSetFullscreenState(IDXGISwapChain* This, BOOL Fullscreen, IDXGIOutput* pTarget)
{
    IFGFeature* fg = State::Instance().currentFG;

    if (fg != nullptr && fg->IsActive())
    {
        State::Instance().fgChanged = true;
        fg->UpdateTarget();
        fg->Deactivate();
    }

    bool modeChanged = false;
    bool orgFS = Fullscreen;
    if (BorderlessForced())
    {
        if (Fullscreen)
        {
            Fullscreen = false;

            if (!State::Instance().SCExclusiveFullscreen)
            {
                State::Instance().SCExclusiveFullscreen = true;
                modeChanged = true;
            }
            else if (_hdrKeeper.Active())
            {
                // HDR output already keeps it borderless; the game asking again restyles the window once more (it may
                // have been restyled by the game in between)
                modeChanged = true;
            }

            LOG_DEBUG("Prevented exclusive fullscreen");
        }
        else
        {
            if (State::Instance().SCExclusiveFullscreen)
            {
                modeChanged = true;
                State::Instance().SCExclusiveFullscreen = false;
            }
        }
    }

    State::Instance().realExclusiveFullscreen = Fullscreen;

    if (UsesXeFGApi(State::Instance().activeFgOutput) && Fullscreen)
        LOG_WARN("Using exclusive fullscreen with XeFG!!!");

    auto result = S_OK;

    if (!BorderlessForced())
    {
        result = o_FGSCSetFullscreenState(This, Fullscreen, pTarget);
        LOG_DEBUG("Fullscreen: {}, pTarget: {:X}, Result: {:X}", Fullscreen, (size_t) pTarget, (UINT) result);
    }

    if (result == S_OK && modeChanged)
    {
        LOG_DEBUG("Mode changed");

        DXGI_SWAP_CHAIN_DESC scDesc {};
        This->GetDesc(&scDesc);

        if (State::Instance().SCExclusiveFullscreen)
        {
            Util::MonitorInfo info;

            if (pTarget != nullptr)
                info = Util::GetMonitorInfoForOutput(pTarget);
            else
                info = Util::GetMonitorInfoForWindow(_hwnd);

            LOG_DEBUG("Overriding window size: {}x{}, and pos: {}x{} at monitor: {}", info.width, info.height, info.x,
                      info.y, wstring_to_string(info.name));

            RestyleWindowLater({ _hwnd, true, info.x, info.y, info.width, info.height });
        }
        else
        {
            RestyleWindowLater(
                { _hwnd, false, 0, 0, (int) scDesc.BufferDesc.Width, (int) scDesc.BufferDesc.Height });
        }
    }

    return result;
}

bool FGHooks::BorderlessForced()
{
    return ::BorderlessForced(Config::Instance()->FGXeFGForceBorderless.value_or_default(), _hdrKeeper.Active());
}

void FGHooks::KeepBorderless(HWND hwnd, const void* owner)
{
    if (hwnd == nullptr || owner == nullptr)
        return;

    auto& state = State::Instance();

    // A separate state: the player's FGXeFGForceBorderless is never changed (a player who switched it off on purpose
    // does not get here, SdrToHdr::Inputs::fgWindowable). It is undone by ReleaseBorderless.
    _hdrKeeper.Keep(owner);

    // The game's full screen is the emulated one from here on: SCExclusiveFullscreen is what the game is told
    // (hkGetFullscreenState, hkGetFullscreenDesc), realExclusiveFullscreen what the chain is (tearing flags, the frame
    // limiter, low latency)
    state.SCExclusiveFullscreen = true;
    state.realExclusiveFullscreen = false;

    const Util::MonitorInfo info = Util::GetMonitorInfoForWindow(hwnd);

    if (info.width <= 0 || info.height <= 0)
    {
        LOG_WARN("Frame generation chain kept borderless, but the monitor of the window could not be read: window left "
                 "as it "
                 "is until the game's next full screen switch");
        return;
    }

    LOG_INFO("Frame generation chain kept borderless over the monitor: {}x{} at {}x{}", info.width, info.height, info.x,
             info.y);
    RestyleWindowLater({ hwnd, true, info.x, info.y, info.width, info.height });
}

void FGHooks::ReleaseBorderless(const void* owner)
{
    if (!_hdrKeeper.Release(owner))
        return;

    // What the game is told goes back to what the player's option says; the emulated state is dropped with it, so a
    // later chain or a stood-down one does not start from a stale "exclusive" answer
    if (!Config::Instance()->FGXeFGForceBorderless.value_or_default())
        State::Instance().SCExclusiveFullscreen = false;

    LOG_INFO("Frame generation chain no longer kept borderless by HDR output");
}

void FGHooks::ApplyRestyle(const WindowRestyle& restyle)
{
    // Already so: every successful ResizeBuffers asks again while exclusive fullscreen is refused, and restyling a window
    // that already matches still sends it a frame change, which a game can answer with yet another resize.
    const LONG_PTR style = restyle.popup ? (WS_POPUP | WS_VISIBLE) : (WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    const int x = restyle.popup ? restyle.x : 0;
    const int y = restyle.popup ? restyle.y : 0;
    const LONG_PTR currentStyle =
        GetWindowLongPtr(restyle.hwnd, GWL_STYLE) & ~static_cast<LONG_PTR>(WS_CLIPSIBLINGS | WS_CLIPCHILDREN);
    RECT rect {};

    if (currentStyle == style && GetWindowRect(restyle.hwnd, &rect) && rect.left == x && rect.top == y &&
        rect.right - rect.left == restyle.width && rect.bottom - rect.top == restyle.height)
    {
        LOG_DEBUG("Window already {} {}x{} at {}x{}", restyle.popup ? "popup" : "windowed", restyle.width,
                  restyle.height, x, y);
        return;
    }

    if (restyle.popup)
    {
        SetWindowLongPtr(restyle.hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowLongPtr(restyle.hwnd, GWL_EXSTYLE, WS_EX_APPWINDOW);
        SetWindowPos(restyle.hwnd, HWND_TOP, restyle.x, restyle.y, restyle.width, restyle.height,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    else
    {
        SetWindowLongPtr(restyle.hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowLongPtr(restyle.hwnd, GWL_EXSTYLE, WS_EX_OVERLAPPEDWINDOW);
        SetWindowPos(restyle.hwnd, nullptr, 0, 0, restyle.width, restyle.height,
                     SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

// Posts the restyle to the window and applies it when the window thread takes that message from its queue, through a
// WH_GETMESSAGE hook on that thread (no dependency on the menu's input hooks, which may not be set up yet at the game's
// first fullscreen switch). Only the latest request is kept; the message carries no data, so a foreign post of the same
// message can only apply the pending one early. Falls back to applying it now when it cannot be posted.
void FGHooks::RestyleWindowLater(const WindowRestyle& restyle)
{
    if (restyle.hwnd == nullptr || !IsWindow(restyle.hwnd))
        return;

    bool posted = false;
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(restyle.hwnd, &pid);

    if (tid != 0 && pid == GetCurrentProcessId())
    {
        std::scoped_lock lock(_restyleMutex);

        if (_restyleMessage.load() == 0)
            _restyleMessage.store(RegisterWindowMessageW(L"OptiScaler.FGHooks.Restyle"));

        if (_restyleHook == nullptr || _restyleHookThread != tid)
        {
            if (_restyleHook != nullptr)
                UnhookWindowsHookEx(_restyleHook);

            _restyleHook = SetWindowsHookExW(WH_GETMESSAGE, RestyleMessageHook, nullptr, tid);
            _restyleHookThread = _restyleHook != nullptr ? tid : 0;

            if (_restyleHook == nullptr)
                LOG_WARN("SetWindowsHookEx(WH_GETMESSAGE) failed: {}", GetLastError());
        }

        if (_restyleMessage.load() != 0 && _restyleHook != nullptr)
        {
            _pendingRestyle = restyle;
            _restylePending = PostMessageW(restyle.hwnd, _restyleMessage.load(), 0, 0) != FALSE;
            posted = _restylePending;
        }
    }

    if (posted)
    {
        LOG_DEBUG("Window restyle deferred to the message loop: {} {}x{} at {}x{}", restyle.popup ? "popup" : "windowed",
                  restyle.width, restyle.height, restyle.x, restyle.y);
        return;
    }

    LOG_WARN("Could not defer the window restyle, applying it now");
    ApplyRestyle(restyle);
}

void FGHooks::ReleaseRestyleHook()
{
    std::scoped_lock lock(_restyleMutex);

    // hMod is null for a thread hook in this process, so the hook does not keep this DLL loaded
    if (_restyleHook != nullptr)
        UnhookWindowsHookEx(_restyleHook);

    _restyleHook = nullptr;
    _restyleHookThread = 0;
    _restylePending = false;
}

LRESULT CALLBACK FGHooks::RestyleMessageHook(int code, WPARAM wParam, LPARAM lParam)
{
    auto msg = reinterpret_cast<MSG*>(lParam);
    const UINT restyleMessage = _restyleMessage.load();

    if (code == HC_ACTION && wParam == PM_REMOVE && msg != nullptr && restyleMessage != 0 &&
        msg->message == restyleMessage)
    {
        msg->message = WM_NULL;

        WindowRestyle restyle {};
        bool apply = false;

        {
            std::scoped_lock lock(_restyleMutex);
            apply = _restylePending;
            restyle = _pendingRestyle;
            _restylePending = false;
        }

        // Outside the lock: the messages this sends can bring the game back into SetFullscreenState.
        if (apply && IsWindow(restyle.hwnd))
        {
            LOG_DEBUG("Applying deferred window restyle: {} {}x{} at {}x{}", restyle.popup ? "popup" : "windowed",
                      restyle.width, restyle.height, restyle.x, restyle.y);
            ApplyRestyle(restyle);
        }
    }

    return CallNextHookEx(nullptr, code, wParam, lParam);
}

HANDLE FGHooks::hkGetFrameLatencyWaitableObject(IDXGISwapChain2* This)
{
    if (!UsesXeFGApi(State::Instance().activeFgOutput))
        return o_FGSCGetFrameLatencyWaitableObject(This);

    if (_semaphore == nullptr)
    {
        _semaphore = CreateSemaphore(nullptr, 16, 16, nullptr);

        if (_semaphore == nullptr)
            return nullptr;
    }

    HANDLE duplicatedHandle = nullptr;

    if (!DuplicateHandle(GetCurrentProcess(), _semaphore, GetCurrentProcess(), &duplicatedHandle, 0, FALSE,
                         DUPLICATE_SAME_ACCESS))
    {
        return nullptr;
    }

    return duplicatedHandle;
}

HRESULT FGHooks::hkGetFullscreenDesc(IDXGISwapChain1* This, DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc)
{
    auto result = o_FGSCGetFullscreenDesc(This, pDesc);

    if (result == S_OK && State::Instance().SCExclusiveFullscreen && BorderlessForced())
    {
        pDesc->Windowed = false;
    }

    return result;
}

HRESULT FGHooks::hkGetFullscreenState(IDXGISwapChain* This, BOOL* pFullscreen, IDXGIOutput** ppTarget)
{
    auto result = o_FGSCGetFullscreenState(This, pFullscreen, ppTarget);

    if (result == S_OK && State::Instance().SCExclusiveFullscreen && BorderlessForced())
    {
        *pFullscreen = true;
    }

    return result;
}

HRESULT FGHooks::hkResizeBuffers(IDXGISwapChain* This, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat,
                                 UINT SwapChainFlags)
{
    // Skip XeFG's internal call
    if (_skipResize)
    {
        LOG_DEBUG("XeFG call skipping");
        _skipResize = false;

        IDXGISwapChain* sc = nullptr;

        if (sc != nullptr)
        {
            auto result = sc->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
            LOG_DEBUG("XeFG internal ResizeBuffers result: {:X}", (UINT) result);
            return result;
        }

        return o_FGSCResizeBuffers(This, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }

    if (State::Instance().currentCommandQueue != nullptr && resizeFence != nullptr && resizeFenceEvent != nullptr)
    {
        LOG_DEBUG("Waiting for GPU to finish before resizing buffers");

        resizeFenceValue++;
        const auto waitResult =
            WaitForQueueIdle(State::Instance().currentCommandQueue, resizeFence, resizeFenceEvent, resizeFenceValue);

        LOG_DEBUG("WaitForSingleObject result: {}", waitResult);
    }

    if (UsesXeFGApi(State::Instance().activeFgOutput))
    {
        if (BorderlessForced())
        {
            SwapChainFlags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        }

        if (State::Instance().SCLastFlags != SwapChainFlags)
        {
            LOG_WARN("SwapChainFlags changed from {:X} to {:X}", State::Instance().SCLastFlags, SwapChainFlags);

            if (UsesXeFGApi(State::Instance().activeFgOutput))
            {
                LOG_WARN("Preventing flag change for XeFG!");
                SwapChainFlags = State::Instance().SCLastFlags;
            }
        }
    }

    LOG_DEBUG("BufferCount: {}, Width: {}, Height: {}, NewFormat:{}, SwapChainFlags: {:X}", BufferCount, Width, Height,
              (UINT) NewFormat, SwapChainFlags);

    auto fgDx12 = State::Instance().currentFG;
    IFGFeature* fg = fgDx12;

    if (!State::Instance().SCExclusiveFullscreen && Config::Instance()->FGSkipResizeBuffers.value_or_default())
    {
        DXGI_SWAP_CHAIN_DESC desc {};
        if (This->GetDesc(&desc) == S_OK)
        {
            LOG_DEBUG("SC BufferCount: {}, Width: {}, Height: {}, NewFormat:{}, SwapChainFlags: {:X}", desc.BufferCount,
                      desc.BufferDesc.Width, desc.BufferDesc.Height, (UINT) desc.BufferDesc.Format,
                      State::Instance().SCLastFlags);

            if (BufferCount == 0)
                BufferCount = desc.BufferCount;

            if ((desc.BufferDesc.Width == Width || Width == 0) && (desc.BufferDesc.Height == Height || Height == 0) &&
                (NewFormat == desc.BufferDesc.Format || NewFormat == 0) &&
                State::Instance().SCLastFlags == SwapChainFlags &&
                (BufferCount == desc.BufferCount || BufferCount == 0))
            {
                LOG_DEBUG("Skipping resize");

                if (Config::Instance()->FGModifyBufferState.value_or_default() ||
                    Config::Instance()->FGModifySCIndex.value_or_default())
                {
                    auto swapchain = ((IDXGISwapChain3*) This);
                    auto swapchainIndex = swapchain->GetCurrentBackBufferIndex();

                    if (fgDx12 != nullptr && Config::Instance()->FGModifyBufferState.value_or_default())
                    {
                        LOG_INFO("Trying to change backbuffer state to COMMON");

                        auto cmdList = fgDx12->GetUICommandList();

                        if (cmdList != nullptr)
                        {
                            for (size_t i = 0; i < desc.BufferCount; i++)
                            {
                                ID3D12Resource* backBuffer = nullptr;
                                if (swapchain->GetBuffer(swapchainIndex, IID_PPV_ARGS(&backBuffer)) == S_OK)
                                {
                                    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                                        backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);

                                    cmdList->ResourceBarrier(1, &barrier);

                                    backBuffer->Release();
                                }
                            }
                        }
                    }

                    if (swapchainIndex != 0 && Config ::Instance()->FGModifySCIndex.value_or_default())
                    {
                        auto presents = desc.BufferCount - swapchainIndex;

                        LOG_DEBUG("Trying to reset backbuffer index: {} with {} present calls", swapchainIndex,
                                  presents);

                        for (size_t i = 0; i < presents; i++)
                        {
                            swapchain->Present(0, 0);
                        }
                    }
                }

                return S_OK;
            }
        }

        // Docs say you cannot do that
        // SwapChainFlags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    State::Instance().SCAllowTearing = (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) > 0;

    State::Instance().SCLastFlags = SwapChainFlags;

    if (fg != nullptr && fg->IsActive())
    {
        State::Instance().fgChanged = true;
        fg->UpdateTarget();
        fg->Deactivate();

        // Let's try Dx11 like approach on Dx12
        std::shared_lock<std::shared_mutex> resizeLock(_resizeMutex, std::defer_lock);
        if (UsesXeFGApi(State::Instance().activeFgOutput) &&
            State::Instance().swapchainInteropApi == SwapchainInteropApi::None)
        {
            resizeLock.lock();
        }
    }

    _skipResize1 = true;

    // Release swapchain backbuffers to prevent errors when resizing
    //    if (State::Instance().activeFgOutput == FGOutput::XeFG)
    //    {
    //        for (UINT i = 0; i < 8; i++)
    //        {
    //            ID3D12Resource* backBuffer = nullptr;
    //            auto bbResult = This->GetBuffer(i, IID_PPV_ARGS(&backBuffer));
    //
    //            if (bbResult == S_OK)
    //            {
    //                LOG_DEBUG("Backbuffer {}: {:X}", i, (size_t) backBuffer);
    //                auto refCount = backBuffer->Release();
    //                while (refCount > XEFG_RESOURCE_REF_LIMIT)
    //                {
    //                    LOG_DEBUG("Releasing backbuffer {}: RefCount {}", i, refCount);
    //                    refCount = backBuffer->Release();
    //                }
    //
    // #if (XEFG_RESOURCE_REF_LIMIT == 0)
    //                oldBackBuffers.push_back(backBuffer);
    // #endif
    //            }
    //            else
    //            {
    //                LOG_DEBUG("GetBuffer failed for index {}: {:X}", i, (UINT) bbResult);
    //                break;
    //            }
    //        }
    //    }

    HRESULT result;
    {
        ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
        result = o_FGSCResizeBuffers(This, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }

    _skipResize1 = false;

    LOG_DEBUG("Result: {:X}, Caller: {}", (UINT) result, Util::WhoIsTheCaller(_ReturnAddress()));

    if (result == S_OK)
    {
        if (fg != nullptr)
        {
            State::Instance().fgChanged = true;
            fg->Deactivate();
            fg->UpdateTarget();
        }
    }

    // Resize window to cover the screen
    if (result == S_OK && BorderlessForced() && State::Instance().SCExclusiveFullscreen)
    {
        Util::MonitorInfo info;
        info = Util::GetMonitorInfoForWindow(_hwnd);

        LOG_DEBUG("Overriding window size: {}x{}, and pos: {}x{} at monitor: {}", info.width, info.height, info.x,
                  info.y, wstring_to_string(info.name));

        RestyleWindowLater({ _hwnd, true, info.x, info.y, info.width, info.height });
    }

    return result;
}

HRESULT FGHooks::hkResizeTarget(IDXGISwapChain* This, const DXGI_MODE_DESC* pNewTargetParameters)
{
    if (BorderlessForced())
    {
        // The display mode is not changed (borderless), but the game asked for a new size and expects its window to
        // follow: games on the blt model take the new size from the window (WM_SIZE) or call ResizeBuffers after it.
        // Dropping the call left the window, and so the picture, at the old size while the game drew at the new one.
        DXGI_SWAP_CHAIN_DESC desc {};
        HWND hwnd = (This != nullptr && SUCCEEDED(This->GetDesc(&desc))) ? desc.OutputWindow : nullptr;

        if (hwnd != nullptr && IsWindow(hwnd) && pNewTargetParameters != nullptr && pNewTargetParameters->Width > 0 &&
            pNewTargetParameters->Height > 0)
        {
            const Util::MonitorInfo info = Util::GetMonitorInfoForWindow(hwnd);
            const int width = info.width > 0 ? std::min<int>((int) pNewTargetParameters->Width, info.width)
                                             : (int) pNewTargetParameters->Width;
            const int height = info.height > 0 ? std::min<int>((int) pNewTargetParameters->Height, info.height)
                                               : (int) pNewTargetParameters->Height;

            RECT client {};
            GetClientRect(hwnd, &client);

            LOG_INFO("ResizeTarget {}x{} requested, borderless: window client {}x{} -> {}x{} on monitor {}x{}",
                     pNewTargetParameters->Width, pNewTargetParameters->Height, client.right - client.left,
                     client.bottom - client.top, width, height, info.width, info.height);

            // Synchronous: games read the window size right after ResizeTarget. Centred on the monitor; a size equal
            // to the monitor covers it.
            if (client.right - client.left != width || client.bottom - client.top != height)
            {
                ApplyRestyle({ hwnd, true, info.x + (info.width - width) / 2, info.y + (info.height - height) / 2,
                               width, height });
            }
        }
        else
        {
            LOG_DEBUG("Skipping resize target.");
        }

        return S_OK;
    }

    IFGFeature* fg = State::Instance().currentFG;

    if (fg != nullptr && fg->IsActive())
    {
        State::Instance().fgChanged = true;
        fg->UpdateTarget();
        fg->Deactivate();
    }

    return o_FGSCResizeTarget(This, pNewTargetParameters);
}

HRESULT FGHooks::hkResizeBuffers1(IDXGISwapChain3* This, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format,
                                  UINT SwapChainFlags, const UINT* pCreationNodeMask, IUnknown* const* ppPresentQueue)
{
    // Skip XeFG's internal call
    if (_skipResize1)
    {
        LOG_DEBUG("XeFG call skipping");
        _skipResize1 = false;

        IDXGISwapChain3* sc = nullptr;

        if (sc != nullptr)
        {
            auto result = sc->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask,
                                             ppPresentQueue);

            LOG_DEBUG("XeFG internal ResizeBuffers1 result: {:X}", (UINT) result);
            return result;
        }

        return o_FGSCResizeBuffers1(This, BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask,
                                    ppPresentQueue);
    }

    if (State::Instance().currentCommandQueue != nullptr && resizeFence != nullptr && resizeFenceEvent != nullptr)
    {
        LOG_DEBUG("Waiting for GPU to finish before resizing buffers");

        resizeFenceValue++;
        const auto waitResult =
            WaitForQueueIdle(State::Instance().currentCommandQueue, resizeFence, resizeFenceEvent, resizeFenceValue);

        LOG_DEBUG("WaitForSingleObject result: {}", waitResult);
    }

    if (UsesXeFGApi(State::Instance().activeFgOutput))
    {
        if (BorderlessForced())
        {
            SwapChainFlags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        }

        if (State::Instance().SCLastFlags != SwapChainFlags)
        {
            LOG_WARN("SwapChainFlags changed from {:X} to {:X}", State::Instance().SCLastFlags, SwapChainFlags);

            if (UsesXeFGApi(State::Instance().activeFgOutput))
            {
                LOG_WARN("Preventing flag change for XeFG!");
                SwapChainFlags = State::Instance().SCLastFlags;
            }
        }
    }

    LOG_DEBUG("BufferCount: {}, Width: {}, Height: {}, NewFormat:{}, SwapChainFlags: {:X}, Caller: {}", BufferCount,
              Width, Height, (UINT) Format, SwapChainFlags, Util::WhoIsTheCaller(_ReturnAddress()));

    auto fgDx12 = State::Instance().currentFG;
    IFGFeature* fg = fgDx12;

    if (!State::Instance().SCExclusiveFullscreen && Config::Instance()->FGSkipResizeBuffers.value_or_default())
    {
        DXGI_SWAP_CHAIN_DESC desc {};
        if (This->GetDesc(&desc) == S_OK)
        {
            LOG_DEBUG("SC BufferCount: {}, Width: {}, Height: {}, NewFormat:{}, SwapChainFlags: {:X}", desc.BufferCount,
                      desc.BufferDesc.Width, desc.BufferDesc.Height, (UINT) desc.BufferDesc.Format,
                      State::Instance().SCLastFlags);

            if (BufferCount == 0)
                BufferCount = desc.BufferCount;

            if ((desc.BufferDesc.Width == Width || Width == 0) && (desc.BufferDesc.Height == Height || Height == 0) &&
                (Format == desc.BufferDesc.Format || Format == 0) && State::Instance().SCLastFlags == SwapChainFlags &&
                (BufferCount == desc.BufferCount || BufferCount == 0))
            {
                LOG_DEBUG("Skipping resize");

                if (Config::Instance()->FGModifyBufferState.value_or_default() ||
                    Config::Instance()->FGModifySCIndex.value_or_default())
                {
                    auto swapchain = ((IDXGISwapChain3*) This);
                    auto swapchainIndex = swapchain->GetCurrentBackBufferIndex();

                    if (fgDx12 != nullptr && Config::Instance()->FGModifyBufferState.value_or_default())
                    {
                        LOG_INFO("Trying to change backbuffer state to COMMON");

                        auto cmdList = fgDx12->GetUICommandList();
                        if (cmdList != nullptr)
                        {
                            for (size_t i = 0; i < desc.BufferCount; i++)
                            {
                                ID3D12Resource* backBuffer = nullptr;
                                if (swapchain->GetBuffer(swapchainIndex, IID_PPV_ARGS(&backBuffer)) == S_OK)
                                {
                                    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                                        backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);

                                    cmdList->ResourceBarrier(1, &barrier);

                                    backBuffer->Release();
                                }
                            }
                        }
                    }

                    if (swapchainIndex != 0 && Config ::Instance()->FGModifySCIndex.value_or_default())
                    {
                        auto presents = desc.BufferCount - swapchainIndex;

                        LOG_DEBUG("Trying to reset backbuffer index: {} with {} present calls", swapchainIndex,
                                  presents);

                        for (size_t i = 0; i < presents; i++)
                        {
                            swapchain->Present(0, 0);
                        }
                    }
                }

                return S_OK;
            }
        }

        // SwapChainFlags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    State::Instance().SCAllowTearing = (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) > 0;

    State::Instance().SCLastFlags = SwapChainFlags;

    if (fg != nullptr && fg->IsActive())
    {
        State::Instance().fgChanged = true;
        fg->UpdateTarget();
        fg->Deactivate();

        // Let's try Dx11 like approach on Dx12
        std::shared_lock<std::shared_mutex> resizeLock(_resizeMutex, std::defer_lock);
        if (UsesXeFGApi(State::Instance().activeFgOutput) &&
            State::Instance().swapchainInteropApi == SwapchainInteropApi::None)
        {
            resizeLock.lock();
        }
    }

    // Release menu render targets
    if (Config::Instance()->OverlayMenu.value_or_default())
        MenuOverlayDx::CleanupRenderTarget(false, NULL);

    // Release swapchain backbuffers to prevent errors when resizing
    //    if (State::Instance().activeFgOutput == FGOutput::XeFG)
    //    {
    //        for (UINT i = 0; i < 8; i++)
    //        {
    //            ID3D12Resource* backBuffer = nullptr;
    //            auto bbResult = This->GetBuffer(i, IID_PPV_ARGS(&backBuffer));
    //
    //            if (bbResult == S_OK)
    //            {
    //                LOG_DEBUG("Backbuffer {}: {:X}", i, (size_t) backBuffer);
    //                auto refCount = backBuffer->Release();
    //                while (refCount > XEFG_RESOURCE_REF_LIMIT)
    //                {
    //                    LOG_DEBUG("Releasing backbuffer {}: RefCount {}", i, refCount);
    //                    refCount = backBuffer->Release();
    //                }
    //
    // #if (XEFG_RESOURCE_REF_LIMIT == 0)
    //                oldBackBuffers.push_back(backBuffer);
    // #endif
    //            }
    //            else
    //            {
    //                LOG_DEBUG("GetBuffer failed for index {}: {:X}", i, (UINT) bbResult);
    //                break;
    //            }
    //        }
    //    }

    HRESULT result;
    {
        ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
        _skipResize = true;

        result = o_FGSCResizeBuffers1(This, BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask,
                                      ppPresentQueue);

        _skipResize = false;
    }

    LOG_DEBUG("Result: {:X}, Caller: {}", (UINT) result, Util::WhoIsTheCaller(_ReturnAddress()));

    if (result == S_OK)
    {
        if (fg != nullptr)
        {
            State::Instance().fgChanged = true;
            fg->Deactivate();
            fg->UpdateTarget();
        }
    }

    // Resize window to cover the screen
    if (result == S_OK && BorderlessForced() && State::Instance().SCExclusiveFullscreen)
    {
        Util::MonitorInfo info;
        info = Util::GetMonitorInfoForWindow(_hwnd);

        LOG_DEBUG("Overriding window size: {}x{}, and pos: {}x{} at monitor: {}", info.width, info.height, info.x,
                  info.y, wstring_to_string(info.name));

        RestyleWindowLater({ _hwnd, true, info.x, info.y, info.width, info.height });
    }

    return result;
}

HRESULT FGHooks::hkFGPresent(IDXGISwapChain* This, UINT SyncInterval, UINT Flags)
{
    // Skip XeFG's internal call
    if (_skipPresent)
    {
        LOG_DEBUG("XeFG call skipping");

        IDXGISwapChain* sc = nullptr;

        HRESULT result;

        if (sc != nullptr)
        {
            result = sc->Present(SyncInterval, Flags);
            LOG_DEBUG("sc->Present result: {:X}", (UINT) result);
        }
        else
        {
            result = o_FGSCPresent(This, SyncInterval, Flags);
            LOG_DEBUG("o_FGSCPresent result: {:X}", (UINT) result);
        }

        return result;
    }

    LOG_DEBUG("SyncInterval: {}, Flags: {:X}", SyncInterval, Flags);

    _skipPresent1 = true;
    auto result = FGPresent(This, SyncInterval, Flags, nullptr);
    _skipPresent1 = false;

    return result;
}

HRESULT FGHooks::hkFGPresent1(IDXGISwapChain1* This, UINT SyncInterval, UINT Flags,
                              const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    // Skip XeFG's internal call
    if (_skipPresent1)
    {
        LOG_DEBUG("XeFG call skipping");

        IDXGISwapChain3* sc = nullptr;

        HRESULT result;

        if (sc != nullptr)
        {
            result = sc->Present1(SyncInterval, Flags, pPresentParameters);
            LOG_DEBUG("sc->Present result: {:X}", (UINT) result);
        }
        else
        {
            result = o_FGSCPresent1(This, SyncInterval, Flags, pPresentParameters);
            LOG_DEBUG("o_FGSCPresent result: {:X}", (UINT) result);
        }

        return result;
    }

    LOG_DEBUG("SyncInterval: {}, Flags: {:X}", SyncInterval, Flags);
    _skipPresent = true;
    auto result = FGPresent(This, SyncInterval, Flags, pPresentParameters);
    _skipPresent = false;

    return result;
}

HRESULT FGHooks::FGPresent(IDXGISwapChain* This, UINT SyncInterval, UINT Flags,
                           const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    _lastPresentFlags = Flags;

    auto& state = State::Instance();
    auto config = Config::Instance();

    if (state.isShuttingDown)
    {
        if (pPresentParameters == nullptr)
            return o_FGSCPresent(This, SyncInterval, Flags);
        else
            return o_FGSCPresent1((IDXGISwapChain1*) This, SyncInterval, Flags, pPresentParameters);
    }

    auto willPresent = (Flags & DXGI_PRESENT_TEST) == 0;

    if (willPresent)
    {
        // Let's try Dx11 like approach on Dx12
        std::shared_lock<std::shared_mutex> resizeLock(_resizeMutex, std::defer_lock);
        if (UsesXeFGApi(State::Instance().activeFgOutput) &&
            State::Instance().swapchainInteropApi == SwapchainInteropApi::None)
        {
            resizeLock.lock();
        }

        state.fgLastFrame++;

        double ftDelta = 0.0f;
        auto now = Util::MillisecondsNow();

        if (_lastFGFrameTime > 0.0)
            ftDelta = now - _lastFGFrameTime;

        _lastFGFrameTime = now;
        state.lastFGFrameTime = ftDelta;

        LOG_DEBUG("flags: {:X}, Frametime: {}", Flags, ftDelta);

#ifdef LOW_LATENCY_INPUTS
        IUnknown* device = state.currentD3D12Device ? state.currentD3D12Device : (IUnknown*) state.currentD3D11Device;
        InputCommon::mark_present_start(device);
#endif
    }

    // Work on the game's picture before frame generation takes it must go on the queue frame generation takes it on.
    ID3D12CommandQueue* gameQueue = state.currentCommandQueue;

    if (auto* fg12 = dynamic_cast<IFGFeature_Dx12*>(state.currentFG); fg12 != nullptr && fg12->GameCommandQueue() != nullptr)
    {
        static bool logged = false;

        if (!logged && fg12->GameCommandQueue() != state.currentCommandQueue)
        {
            logged = true;
            LOG_INFO("FGPresent: frame generation's game queue {:X} is not State::currentCommandQueue {:X}; native input "
                     "and the finished-picture pass use frame generation's",
                     (size_t) fg12->GameCommandQueue(), (size_t) state.currentCommandQueue);
        }

        gameQueue = fg12->GameCommandQueue();
    }

    // Which queue this is can change from frame to frame (frame generation's game queue comes and goes at a screen
    // change): native input's work keeps its own order across a change (native::NativeProducer), and the log says when.
    {
        static ID3D12CommandQueue* lastGameQueue = nullptr; // compared only
        static uint32_t changes = 0;

        if (gameQueue != lastGameQueue)
        {
            if (lastGameQueue != nullptr && changes++ < 8)
            {
                auto* fg12 = dynamic_cast<IFGFeature_Dx12*>(state.currentFG);
                LOG_INFO("FGPresent: the game's queue for work before frame generation changed {:X} -> {:X} ({})",
                         (size_t) lastGameQueue, (size_t) gameQueue,
                         fg12 == nullptr                       ? "no frame generation"
                         : fg12->GameCommandQueue() == nullptr ? "frame generation has no game queue right now"
                                                               : "frame generation's game queue");
            }

            lastGameQueue = gameQueue;
        }
    }

    // Native input runs here, on the game's picture and queue, ahead of frame generation and before its lock below: when it
    // presents to the virtual upscaler, that call feeds frame generation (UpscaleStart takes the same lock).
    //
    // Not under the Vulkan bridge's D3D12 swapchain: its picture belongs to the Vulkan driver (hooks/Vulkan_Hooks.cpp), which
    // runs the same step on the Vulkan game's presents. The interop state alone is not enough to tell: it is reset when a
    // device of the game's is destroyed, and this driver then ran on the bridge's own D3D12 device.
    if (willPresent && state.swapchainInteropApi == SwapchainInteropApi::None && gameQueue != nullptr &&
        state.currentD3D12Device != nullptr && !VkPresentBridge::OutputActive())
    {
        NativeMotionDx12::OnFGPresent(This, gameQueue, state.currentD3D12Device);
    }

    IFGFeature* fg = state.currentFG;

    if (fg != nullptr && willPresent && fg->IsActive() && !fg->IsPaused())
    {
        if (auto currentFeature = State::Instance().currentFeature; currentFeature != nullptr)
        {
            std::optional<double> upscalerTimeOpt {};

            // ReadUpscalerTime takes a void*: a plain D3D12 feature casts it to ID3D12CommandQueue*, so it must never get
            // the D3D11 context. Under Dx11wDx12 that feature is native::VirtualUpscalerDriver's, recorded on the queue
            // gameQueue names (WithDx12's paired one).
            const bool pureDx12Feature = currentFeature->Api() == API::DX12 && !currentFeature->IsWithDx12();

            if (pureDx12Feature && state.swapchainInteropApi != SwapchainInteropApi::None)
            {
                if (gameQueue != nullptr)
                {
                    if (upscalerTimeOpt = currentFeature->ReadUpscalerTime(gameQueue); upscalerTimeOpt.has_value())
                        currentFeature->ReadDetailedGpuTimes(gameQueue, state.detailedGpuTimes);
                }
            }
            else if (state.swapchainInteropApi == SwapchainInteropApi::Dx11wDx12 && state.currentD3D11Device != nullptr)
            {
                ID3D11DeviceContext* context = nullptr;
                state.currentD3D11Device->GetImmediateContext(&context);

                if (upscalerTimeOpt = currentFeature->ReadUpscalerTime(context); upscalerTimeOpt.has_value())
                    currentFeature->ReadDetailedGpuTimes(context, State::Instance().detailedGpuTimes);

                context->Release();
            }
            else if (state.swapchainInteropApi == SwapchainInteropApi::None && state.currentCommandQueue != nullptr)
            {
                if (upscalerTimeOpt = currentFeature->ReadUpscalerTime(state.currentCommandQueue);
                    upscalerTimeOpt.has_value())
                {
                    currentFeature->ReadDetailedGpuTimes(state.currentCommandQueue, state.detailedGpuTimes);
                }
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

    bool mutexUsed = false;
    native::presenttiming::NoteFrameGenerationMutexWait(0.0);
    if (willPresent && fg != nullptr && fg->IsActive() && !fg->IsPaused() &&
        config->FGUseMutexForSwapchain.value_or_default() && fg->Mutex.getOwner() != 2)
    {
        LOG_TRACE("Waiting FG->Mutex 2, current: {}", fg->Mutex.getOwner());
        const auto mutexWaitStartMs = Util::MillisecondsNow();
        fg->Mutex.lock(2);
        // For the stall timing: on the Vulkan game's present thread this is the wait inside the bridge's present
        native::presenttiming::NoteFrameGenerationMutexWait(Util::MillisecondsNow() - mutexWaitStartMs);
        mutexUsed = true;
        LOG_TRACE("Accuired FG->Mutex: {}", fg->Mutex.getOwner());
    }

    const bool fgFeatureActive = fg != nullptr && fg->IsActive() && !fg->IsPaused();

    sl::FrameToken* localToken = nullptr;
    sl::Result tokenResult = sl::Result::eErrorReflexAPI;
    if (willPresent && fg != nullptr && !fgFeatureActive)
        state.dlssgDetectedInterpolationCount = 0;

    if (willPresent && fgFeatureActive && state.activeFgOutput == FGOutput::DLSSG)
    {
        if ((!ReflexHooks::gameIsSendingMarkers() || !config->FGDLSSGUseGamesReflexMarkers.value_or_default()))
        {
            if (StreamlineProxy::PCLSetMarker() != nullptr)
            {
                ((IDXGISwapChain4*) This)->GetCurrentBackBufferIndex();
                const uint32_t frameId = (uint32_t) fg->FrameCount();
                tokenResult = StreamlineProxy::GetNewFrameToken()(localToken, &frameId);

                if (tokenResult == sl::Result::eOk)
                    StreamlineProxy::PCLSetMarker()(sl::PCLMarker::ePresentStart, *localToken);
            }
        }
    }

    if (willPresent && fgFeatureActive)
    {
        if (state.activeFgInput == FGInput::FSRFG)
            ffxPresentCallback();
        else if (state.activeFgInput == FGInput::FSRFG30)
            FSR3FG::ffxPresentCallback();

        // (`This` is the game-facing chain and hands out the game's SDR picture. HDR output converts later, in the
        // library's own real chain behind it: XeFG draws on that chain's buffers, which are HDR output's proxies, FSR
        // FG keeps replacement buffers of its own and composites them into the proxies. Either way this reads the
        // picture before the pass.)
        DlssNr::ApplyToFinishedPicture(This, gameQueue);
        fg->Present();
    }
    else if (willPresent && fg != nullptr)
    {
        LOG_TRACE("FGHooks::FGPresent: FG feature exists but is inactive/paused; pass-through present only");
    }

    if (willPresent && state.swapchainInteropApi == SwapchainInteropApi::None)
    {
        ResTrack_Dx12::ClearPossibleHudless();
        Hudfix_Dx12::PresentStart();
    }

    if (willPresent && config->ForceVsync.has_value())
    {
        LOG_DEBUG("ForceVsync: {}, VsyncInterval: {}, SCAllowTearing: {}, realExclusiveFullscreen: {}",
                  config->ForceVsync.value(), config->VsyncInterval.value_or_default(), state.SCAllowTearing,
                  state.realExclusiveFullscreen);

        if (!config->ForceVsync.value())
        {
            SyncInterval = 0;

            if (state.SCAllowTearing && !state.realExclusiveFullscreen)
            {
                LOG_DEBUG("Adding DXGI_PRESENT_ALLOW_TEARING");
                Flags |= DXGI_PRESENT_ALLOW_TEARING;
            }
        }
        else
        {
            SyncInterval = config->VsyncInterval.value_or_default();

            if (SyncInterval < 1)
                SyncInterval = 1;

            // XeFG runs with XeLL, and XeLL needs sync interval 1 with VSync on (Intel's XeLL guide)
            if (SyncInterval > 1 && UsesXeFGApi(state.activeFgOutput))
            {
                static bool logged = false;

                if (!logged)
                {
                    logged = true;
                    LOG_WARN("VsyncInterval {} with XeFG: using 1, XeLL needs sync interval 1", SyncInterval);
                }

                SyncInterval = 1;
            }

            LOG_DEBUG("Removing DXGI_PRESENT_ALLOW_TEARING");
            Flags &= ~DXGI_PRESENT_ALLOW_TEARING;
        }

        LOG_DEBUG("Final SyncInterval: {}", SyncInterval);
    }

    // Used at wrapped_swapchain LocalPresent to determine is frame is interpolated or not
    if (willPresent)
    {
        state.fgPresentIsCalled = true;
        state.fgPresentVsync.store(SyncInterval > 0, std::memory_order_relaxed);
    }

    // Optical F5Low's low latency (native/NativeLowLatency.h): this is the game's present; the wrapped swap chain's
    // presents behind it are frame generation's own. (A D3D11 game's is Dx11wDx12SC::Present.)
    IUnknown* lowLatencyDevice = nullptr;

    if (willPresent && state.swapchainInteropApi == SwapchainInteropApi::None)
    {
        lowLatencyDevice = state.currentD3D12Device;

        if (lowLatencyDevice != nullptr)
            native::lowlatency::OnPresentBegin(lowLatencyDevice, native::lowlatency::PresentSource::FrameGeneration);
    }

    // XeFG marks a batch of frames during the game's present (InputCommon::xefg_marker); its own count of the frames
    // it sends for it, read after, sets how many of its presents carry that batch's frame id
    void* xefgContext = nullptr;
    uint64_t xefgFrameId = 0;

    if (willPresent && state.currentFG != nullptr && UsesXeFGApi(state.activeFgOutput))
    {
        xefgContext = state.currentFG->SwapchainContext();
        xefgFrameId = InputCommon::xefg_marked_frame_id();
    }

    HRESULT result;
    const auto presentStartMs = Util::MillisecondsNow();

    if (pPresentParameters == nullptr)
        result = o_FGSCPresent(This, SyncInterval, Flags);
    else
        result = o_FGSCPresent1((IDXGISwapChain1*) This, SyncInterval, Flags, pPresentParameters);

    if (xefgContext != nullptr && XeFGProxy::GetLastPresentStatus() != nullptr)
    {
        xefg_swapchain_present_status_t status {};

        if (XeFGProxy::GetLastPresentStatus()((xefg_swapchain_handle_t) xefgContext, &status) ==
            XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            InputCommon::xefg_present_status(xefgFrameId, status.framesPresented);

            // A present that sent nothing counts too: leaving it out would hide a stall
            if (status.framesPresented != 0 || status.isFrameGenEnabled != 0)
                xefg::AddShownPresent(state.xefgShown, status.framesPresented);
        }
    }

    // Where a stall sits when the game's present is held up inside frame generation's own (the game's present thread
    // only; native/PresentStageTiming.h). Only for the Vulkan bridge: other games log nothing of this.
    if (willPresent &&
        native::presenttiming::FrameGenerationTimingWanted(state.swapchainInteropApi == SwapchainInteropApi::VkwDx12))
    {
        static native::presenttiming::StageTiming presentTiming;

        const auto now = Util::MillisecondsNow();
        presentTiming.Record(native::presenttiming::Stage::FrameGenerationPresent, now - presentStartMs);
        presentTiming.Record(native::presenttiming::Stage::FrameGenerationMutexWait,
                             native::presenttiming::PeekFrameGenerationMutexWait());
        const auto outcome = presentTiming.EndPresent(now);

        if (outcome.stall)
            LOG_WARN("Frame generation present stall: {}", presentTiming.StallLine());

        if (outcome.summary)
        {
            LOG_INFO("Frame generation present timing, {}", presentTiming.SummaryLine());
            presentTiming.Restart();
        }
    }

    if (result == S_OK)
    {
        LOG_DEBUG("Result: {:X}", result);
    }
    else
    {
        if (result == DXGI_ERROR_DEVICE_REMOVED && state.currentD3D12Device != nullptr)
            Util::GetDeviceRemovedReason(state.currentD3D12Device);
    }

    if (tokenResult == sl::Result::eOk && localToken != nullptr && fgFeatureActive &&
        (!ReflexHooks::gameIsSendingMarkers() || !config->FGDLSSGUseGamesReflexMarkers.value_or_default()) &&
        willPresent && state.activeFgOutput == FGOutput::DLSSG)
    {
        if (StreamlineProxy::PCLSetMarker() != nullptr)
            StreamlineProxy::PCLSetMarker()(sl::PCLMarker::ePresentEnd, *localToken);

        LOG_DEBUG("Calling ReflexSleep");
        StreamlineProxy::ReflexSleep()(*localToken);
    }

    if (state.swapchainInteropApi == SwapchainInteropApi::None)
        Hudfix_Dx12::PresentEnd();

    // A dxvk game's frame generation through the Vulkan present bridge (NativeDxvkVulkan) presents a real D3D12 swapchain
    // here, as a native Vulkan game's does: the limiter applies to it.
    if (willPresent && !state.reflexLimitsFps && !native::lowlatency::OwnsFrameCap() &&
        state.activeFgOutput != FGOutput::NoFG &&
        (!IdentifyGpu::getPrimaryGpu().usesDxvk || VkPresentBridge::DxvkThroughVulkan()) && !XellHooks::canLimit())
    {
        // Shown frames per rendered one only while it generates (native::lowlatency::ShownPerRendered)
        FrameLimit::sleep(native::lowlatency::ShownPerRendered(fg != nullptr && fg->IsGenerating(),
                                                               fg != nullptr ? fg->GetInterpolatedFrameCount() : 0));
    }

    if ((config->SimulateWaitableObject.value_or_default() ||
         (state.gameEngine == GameEngineType::Unity && UsesXeFGApi(state.activeFgOutput))) &&
        _semaphore != nullptr)
    {
        ReleaseSemaphore(_semaphore, 1, nullptr);
    }

    if (mutexUsed && fg != nullptr)
    {
        LOG_TRACE("Releasing FG->Mutex: {}", fg->Mutex.getOwner());
        fg->Mutex.unlockThis(2);
    }

    // What this present waited (the present and OptiScaler's limiter), for XeFG's Dynamic MFG; the low latency sleep
    // below counts apart (InputCommon::take_sleep_ms)
    if (willPresent)
        state.lastFGPresentMs = Util::MillisecondsNow() - presentStartMs;

    // After frame generation's lock is released: Sleep holds this thread until the next frame should start
    if (lowLatencyDevice != nullptr)
        native::lowlatency::OnPresentEnd(lowLatencyDevice, native::lowlatency::PresentSource::FrameGeneration);

    LOG_DEBUG("Present finished");

    return result;
}

ULONG FGHooks::hkFGRelease(IUnknown* This)
{
    // We already released this one, prevent crashes
    if (This == oldSwapChain)
    {
        LOG_DEBUG("Release called on old swapchain, skipping release and returning 0");
        return 0;
    }

#if (XEFG_RESOURCE_REF_LIMIT == 0)
    // find if this resource in oldBackBuffers, if it is, skip release and return 0
    if (oldBackBuffers.size() > 0)
    {
        for (auto it = oldBackBuffers.begin(); it != oldBackBuffers.end(); ++it)
        {
            if (*it == This)
            {
                LOG_DEBUG("Release called on old backbuffer, skipping release and returning 0");
                return 0;
            }
        }
    }
#endif // (XEFG_RESOURCE_REF_LIMIT == 0)

    static bool skipReleaseChecks = false;

    if (skipReleaseChecks || State::Instance().currentFGSwapchain != This || State::Instance().isShuttingDown)
        return o_FGRelease(This);

    This->AddRef();

    if (!Config::Instance()->FGPreserveSwapChain.value_or_default())
    {
        if (o_FGRelease(This) == 1)
        {
            LOG_DEBUG("");

            if (State::Instance().currentCommandQueue != nullptr && resizeFence != nullptr &&
                resizeFenceEvent != nullptr)
            {
                LOG_DEBUG("Waiting for GPU to finish");

                resizeFenceValue++;
                const auto waitResult = WaitForQueueIdle(State::Instance().currentCommandQueue, resizeFence,
                                                         resizeFenceEvent, resizeFenceValue);

                LOG_DEBUG("WaitForSingleObject result: {}", waitResult);
            }

            DXGI_SWAP_CHAIN_DESC scDesc {};
            ((IDXGISwapChain*) This)->GetDesc(&scDesc);

            // The loops below release references OptiScaler never took (a backbuffer down to one, the game's wrapped
            // swapchain down to none). Next to a REFramework drawing on XeFG's frames those are REFramework's own, and it
            // releases them again later: misc/REFrameworkCompat.h leaves both alone.
            const bool onlyOwnReferences = REFrameworkCompat::Active();

            if (onlyOwnReferences)
                LOG_INFO("REFramework compatibility: FG swapchain released, other components' references left alone");

            // Release swapchain backbuffers to prevent errors when releasing FG swapchain
            if (!onlyOwnReferences)
            {
                for (UINT i = 0; i < scDesc.BufferCount; i++)
                {
                    ID3D12Resource* backBuffer = nullptr;
                    auto bbResult = ((IDXGISwapChain*) This)->GetBuffer(i, IID_PPV_ARGS(&backBuffer));

                    if (bbResult == S_OK)
                    {
                        LOG_DEBUG("Backbuffer {}: {:X}", i, (size_t) backBuffer);
                        auto refCount = backBuffer->Release();
                        while (refCount > XEFG_RESOURCE_REF_LIMIT)
                        {
                            LOG_DEBUG("Releasing backbuffer {}: RefCount {}", i, refCount);
                            refCount = backBuffer->Release();
                        }

#if (XEFG_RESOURCE_REF_LIMIT == 0)
                        oldBackBuffers.push_back(backBuffer);
#endif
                    }
                    else
                    {
                        LOG_DEBUG("GetBuffer failed for index {}: {:X}", i, (UINT) bbResult);
                        break;
                    }
                }
            }

            // To prevent deadlock when FG release the swapchain
            skipReleaseChecks = true;

            if (State::Instance().currentFG != nullptr)
            {
                LOG_DEBUG("FG Swapchain released, release FG & swapchain context");
                State::Instance().currentFG->ReleaseSwapchain(_hwnd);
            }

            LOG_DEBUG("FG Swapchain released, clearing currentFGSwapchain");
            State::Instance().currentFGSwapchain = nullptr;

            // currentWrappedSwapchain is the game's swapchain, held by the game: kept, as it is still alive
            if (!onlyOwnReferences && State::Instance().currentWrappedSwapchain != nullptr &&
                State::Instance().currentSwapchainDesc.OutputWindow == _hwnd)
            {
                auto refCount = State::Instance().currentWrappedSwapchain->Release();

                while (refCount > 0 && refCount < 0xffffff00)
                {
                    refCount = State::Instance().currentWrappedSwapchain->Release();
                }

                State::Instance().currentWrappedSwapchain = nullptr;
            }

            skipReleaseChecks = false;

            return 0;
        }
    }
    else
    {
        if (o_FGRelease(This) == 1)
        {
            LOG_INFO("Preserving FG Swapchain from release");
            return 0;
        }
    }

    return o_FGRelease(This);
}
