#pragma once
#include "SysUtils.h"
#include <dxgi1_6.h>

#include "Hook_Utils.h"
#include <framegen/FGBorderless.h>

class FGHooks
{
  public:
    static HRESULT CreateSwapChain(IDXGIFactory* pFactory, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc,
                                   IDXGISwapChain** ppSwapChain);

    static HRESULT CreateSwapChainForHwnd(IDXGIFactory* This, IUnknown* pDevice, HWND hWnd,
                                          DXGI_SWAP_CHAIN_DESC1* pDesc,
                                          DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                          IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain);

    // Registers a real FG swapchain created through FGHooks::CreateSwapChain*().
    static void SetFGSwapchain(IDXGISwapChain* pSwapChain, HWND hWnd);

    // Registers a plain/external DX12 presenting swapchain used by DX11->DX12 interop fallback.
    // This must not mark State::currentFGSwapchain and must not install FG present hooks.
    static void SetDx12InteropPresentSC(IDXGISwapChain* pSwapChain, HWND hWnd);
    static void ClearDx12InteropPresentSC(IUnknown* pSwapChain);

    // Removes the window-thread hook the borderless restyle is applied through (when OptiScaler is unloaded).
    static void ReleaseRestyleHook();

    // The game's own FFX frame generation, seen by the FFX input hooks while another FG input feeds OptiScaler
    // (framegen/GameFsrFgRule.h): it made an FG or FG swapchain context (seen), switched its FSR FG on or off in
    // ffxConfigure, or destroyed its FG context (off). XeFG under that swapchain stands aside while it is on.
    static void NoteGameFsrFgSeen();
    static void NoteGameFsrFg(bool on);
    static bool IsDx12InteropPresentSC(IUnknown* pSwapChain);

    // A swapchain of another window while the frame generation swapchain lives (framegen/FgWindowRule.h): it gets no
    // frame generation and must not replace the State pointers frame generation reads. The game's own DXGI calls only,
    // the Vulkan bridge manages its windows itself.
    static bool IsOtherWindowThanFG(HWND hwnd);

    // While the Vulkan bridge makes its chain (DxgiFactoryHooks::CreateDx12BridgeSwapChain), the frame generation
    // library's inner creates on this thread are never another window's: the bridge manages its windows itself (RDR2
    // moves to a new one after start)
    class ScopedBridgeCreate
    {
      public:
        ScopedBridgeCreate() { _bridgeCreateDepth++; }
        ~ScopedBridgeCreate() { _bridgeCreateDepth--; }
        ScopedBridgeCreate(const ScopedBridgeCreate&) = delete;
        ScopedBridgeCreate& operator=(const ScopedBridgeCreate&) = delete;
    };

  private:
    using PFN_Present = rewrite_signature<decltype(&IDXGISwapChain::Present)>::type;
    using PFN_Present1 = rewrite_signature<decltype(&IDXGISwapChain1::Present1)>::type;
    using PFN_SetFullscreenState = rewrite_signature<decltype(&IDXGISwapChain::SetFullscreenState)>::type;
    using PFN_GetFullscreenState = rewrite_signature<decltype(&IDXGISwapChain::GetFullscreenState)>::type;
    using PFN_GetFullscreenDesc = rewrite_signature<decltype(&IDXGISwapChain1::GetFullscreenDesc)>::type;
    using PFN_ResizeBuffers = rewrite_signature<decltype(&IDXGISwapChain::ResizeBuffers)>::type;
    using PFN_ResizeBuffers1 = rewrite_signature<decltype(&IDXGISwapChain3::ResizeBuffers1)>::type;
    using PFN_ResizeTarget = rewrite_signature<decltype(&IDXGISwapChain::ResizeTarget)>::type;
    using PFN_GetFrameLatencyWaitableObject =
        rewrite_signature<decltype(&IDXGISwapChain2::GetFrameLatencyWaitableObject)>::type;
    using PFN_Release = rewrite_signature<decltype(&IUnknown::Release)>::type;

    inline static PFN_ResizeBuffers o_FGSCResizeBuffers = nullptr;
    inline static PFN_ResizeTarget o_FGSCResizeTarget = nullptr;
    inline static PFN_ResizeBuffers1 o_FGSCResizeBuffers1 = nullptr;
    inline static PFN_SetFullscreenState o_FGSCSetFullscreenState = nullptr;
    inline static PFN_GetFullscreenState o_FGSCGetFullscreenState = nullptr;
    inline static PFN_GetFullscreenDesc o_FGSCGetFullscreenDesc = nullptr;
    inline static PFN_Present o_FGSCPresent = nullptr;
    inline static PFN_Present1 o_FGSCPresent1 = nullptr;
    inline static PFN_Release o_FGRelease = nullptr;
    inline static PFN_GetFrameLatencyWaitableObject o_FGSCGetFrameLatencyWaitableObject = nullptr;
    inline static HWND _hwnd = nullptr;
    inline static thread_local int _bridgeCreateDepth = 0;
    inline static IDXGISwapChain* _dx12InteropPresentSC = nullptr;
    inline static HWND _dx12InteropPresentHwnd = nullptr;
    inline static bool _skipResize = false;
    inline static bool _skipResize1 = false;
    inline static bool _skipPresent = false;
    inline static bool _skipPresent1 = false;
    inline static UINT _lastPresentFlags = 0;
    inline static double _lastFGFrameTime = -1.0;
    inline static std::shared_mutex _resizeMutex;

    // The window style and size that stand in for exclusive fullscreen when FGXeFGForceBorderless is on. Applied from
    // the window thread's message loop instead of inside the game's SetFullscreenState / ResizeBuffers call: the window
    // messages it sends reach the game's window procedure at once, and Mad Max answers them by starting its fullscreen
    // switch again inside the one still running, which then reads memory it has already freed.
    struct WindowRestyle
    {
        HWND hwnd = nullptr;
        bool popup = true; // borderless popup covering (x, y, width, height); false: overlapped window at width x height
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
    };

    inline static std::mutex _restyleMutex;
    inline static WindowRestyle _pendingRestyle;
    inline static bool _restylePending = false;
    inline static HHOOK _restyleHook = nullptr;
    inline static DWORD _restyleHookThread = 0;
    inline static std::atomic<UINT> _restyleMessage = 0;

    static void RestyleWindowLater(const WindowRestyle& restyle);
    inline static HdrBorderlessKeeper _hdrKeeper;

  public:
    // FGXeFGForceBorderless, or HDR output keeping the window borderless for its windowed library chain
    // (KeepBorderless): the game is answered with full screen, the window is a borderless popup over the monitor, and
    // no exclusive switch is made.
    static bool BorderlessForced();

    // HDR output made the frame generation library's chain (`owner`) windowed where the game asked for exclusive full
    // screen: from here the game-facing chain is answered as for FGXeFGForceBorderless, whichever frame generation
    // output it is. The window is restyled from its own message loop. The player's option is not changed.
    // ReleaseBorderless undoes it, for the same owner, when that chain stands down or is released.
    static void KeepBorderless(HWND hwnd, const void* owner);
    static void ReleaseBorderless(const void* owner);

  private:
    static void ApplyRestyle(const WindowRestyle& restyle);
    static LRESULT CALLBACK RestyleMessageHook(int code, WPARAM wParam, LPARAM lParam);

    static void HookFGSwapchain(IDXGISwapChain* pSwapChain);

    static HRESULT hkSetFullscreenState(IDXGISwapChain* This, BOOL Fullscreen, IDXGIOutput* pTarget);
    static HRESULT hkGetFullscreenDesc(IDXGISwapChain1* This, DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc);
    static HRESULT hkGetFullscreenState(IDXGISwapChain* This, BOOL* pFullscreen, IDXGIOutput** ppTarget);
    static HRESULT hkResizeBuffers(IDXGISwapChain* This, UINT BufferCount, UINT Width, UINT Height,
                                   DXGI_FORMAT NewFormat, UINT SwapChainFlags);
    static HRESULT hkResizeTarget(IDXGISwapChain* This, const DXGI_MODE_DESC* pNewTargetParameters);
    static HRESULT hkResizeBuffers1(IDXGISwapChain3* This, UINT BufferCount, UINT Width, UINT Height,
                                    DXGI_FORMAT Format, UINT SwapChainFlags, const UINT* pCreationNodeMask,
                                    IUnknown* const* ppPresentQueue);
    static ULONG hkFGRelease(IUnknown* This);

    static HRESULT hkFGPresent(IDXGISwapChain* This, UINT SyncInterval, UINT Flags);
    static HRESULT hkFGPresent1(IDXGISwapChain1* This, UINT SyncInterval, UINT Flags,
                                const DXGI_PRESENT_PARAMETERS* pPresentParameters);
    static HRESULT FGPresent(IDXGISwapChain* This, UINT SyncInterval, UINT Flags,
                             const DXGI_PRESENT_PARAMETERS* pPresentParameters);

    static HANDLE hkGetFrameLatencyWaitableObject(IDXGISwapChain2* This);

    VALIDATE_MEMBER_HOOK(hkFGPresent, PFN_Present)
    VALIDATE_MEMBER_HOOK(hkFGPresent1, PFN_Present1)
    VALIDATE_MEMBER_HOOK(hkSetFullscreenState, PFN_SetFullscreenState)
    VALIDATE_MEMBER_HOOK(hkGetFullscreenState, PFN_GetFullscreenState)
    VALIDATE_MEMBER_HOOK(hkGetFullscreenDesc, PFN_GetFullscreenDesc)
    VALIDATE_MEMBER_HOOK(hkResizeBuffers, PFN_ResizeBuffers)
    VALIDATE_MEMBER_HOOK(hkResizeBuffers1, PFN_ResizeBuffers1)
    VALIDATE_MEMBER_HOOK(hkResizeTarget, PFN_ResizeTarget)
    VALIDATE_MEMBER_HOOK(hkGetFrameLatencyWaitableObject, PFN_GetFrameLatencyWaitableObject)
    VALIDATE_MEMBER_HOOK(hkFGRelease, PFN_Release)
};
