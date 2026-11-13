#pragma once

#include "SysUtils.h"
#include <OwnedMutex.h>
#include <Config.h>

#include "dxgi1_6.h"
#include "d3d12.h"

#include <output/SdrToHdr_Create.h>
#include <output/SdrToHdr_Fullscreen.h>
#include <output/SdrToHdr_ProxyBase.h>
#include <output/SdrToHdr_ProxyDx11.h>
#include <output/SdrToHdr_ProxyDx12.h>

#include <atomic>
#include <memory>
#include <mutex>

using Microsoft::WRL::ComPtr;

#define USE_LOCAL_MUTEX

class DECLSPEC_UUID("3af622a3-82d0-49cd-994f-cce05122c222") WrappedIDXGISwapChain4 final : public IDXGISwapChain4
{
  public:
    // `plan` is HDR output's decision for this chain (output/SdrToHdr_Create.h); null: the chain is not touched by it.
    WrappedIDXGISwapChain4(IDXGISwapChain* real, IUnknown* pDevice, HWND hWnd, UINT flags, bool isUWP,
                          bool isComposition = false, const SdrToHdr::Plan* plan = nullptr);
    virtual ~WrappedIDXGISwapChain4();

    // Made on another window while frame generation's swapchain lived (FGHooks::IsOtherWindowThanFG): while that one
    // lives, this chain only presents, resizes and switches full screen (no State, frame generation, low latency or
    // menu work), and its release never tears frame generation down
    void MarkOtherWindow() { _otherWindow = true; }

    // implement IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // implement IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override;

    // implement IDXGIDeviceSubObject
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppDevice) override;

    // implement IDXGISwapChain
    HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags) override;
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, REFIID riid, void** ppSurface) override;
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget) override;
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget) override;
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat,
                                            UINT SwapChainFlags) override;
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters) override;
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** ppOutput) override;
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats) override;
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* pLastPresentCount) override;

    // implement IDXGISwapChain1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc) override;
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* pHwnd) override;
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID refiid, void** ppUnk) override;
    HRESULT STDMETHODCALLTYPE Present1(UINT SyncInterval, UINT PresentFlags,
                                       const DXGI_PRESENT_PARAMETERS* pPresentParameters) override;
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported(void) override;
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput) override;
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* pColor) override;
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* pColor) override;
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION Rotation) override;
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* pRotation) override;

    // implement IDXGISwapChain2
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT Width, UINT Height) override;
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* pWidth, UINT* pHeight) override;
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override;
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* pMaxLatency) override;
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject(void) override;
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix) override;
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix) override;

    // implement IDXGISwapChain3
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex(void) override;
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                     UINT* pColorSpaceSupport) override;
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace) override;
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format,
                                             UINT SwapChainFlags, const UINT* pCreationNodeMask,
                                             IUnknown* const* ppPresentQueue) override;

    // implement IDXGISwapChain4
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData) override;

    // HDR output: a creation hook that found this wrapper already made for the chain it just created (a nested hook
    // wrapped it first) hands over its plan here, so the chain it made HDR gets its proxies. Safe to call with a plan
    // that is not proxied (nothing happens).
    void AdoptHdrPlan(const SdrToHdr::Plan& plan, UINT flags);

  private:
    IDXGISwapChain* _real = nullptr;
    IDXGISwapChain1* _real1 = nullptr;
    IDXGISwapChain2* _real2 = nullptr;
    IDXGISwapChain3* _real3 = nullptr;
    IDXGISwapChain4* _real4 = nullptr;

    int _id = 0;
    bool _uwp = false;
    bool _composition = false;
    LONG _refcount;
    // The final Release is running (misc/REFrameworkCompat.h: a nested one returns at once)
    std::atomic_bool _finalReleaseInProgress { false };
    UINT _lastFlags = 0;

    IUnknown* _device = nullptr;

    HWND _handle = nullptr;

    bool _otherWindow = false;
    // Presented through LocalPresent (frame generation was away): the menu may have drawn on this chain. Written by the
    // presenting thread, read by Release's
    std::atomic<bool> _presentedAsMain { false };
    bool PassesThrough() const;
    // The menu's buffers go with this chain (MenuOverlayDx::CleanupRenderTarget is global: it deactivates frame
    // generation and frees the menu's objects whatever the window): never for another window's chain while frame
    // generation lives, nor for one the menu never drew on or has since left
    bool OwnsMenuCleanup() const;

    // HDR output (output/SdrToHdr_*): null until the chain is first proxied, then set once and never reset or replaced
    // (GetBuffer / GetDesc read it on any thread). While it is Active the game's GetBuffer returns the proxies and the real
    // chain is HDR; when HDR output ends it stays, inactive.
    std::unique_ptr<SdrToHdr::ProxyChainBase> _proxyOwner; // a Direct3D 12 or a Direct3D 11 chain, by the game's device
    std::atomic<SdrToHdr::ProxyChainBase*> _proxyPtr { nullptr };
    IUnknown* _creationQueue = nullptr; // identity of the queue the chain was made with (not owned)
    DXGI_COLOR_SPACE_TYPE _gameColourSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709; // what the game last asked for
    SdrToHdr::DisplayInfo _display;
    bool _displayKnown = false;
    bool _hdrColourSpaceLogged = false;

    // An older (bitblt) chain that was made flip model for HDR output (SdrToHdr::Plan::upgraded). It stays flip for good (a chain
    // cannot change its model), whether or not HDR output is still converting it. The game keeps being told what it asked for.
    bool _upgraded = false;
    DXGI_SWAP_EFFECT _gameSwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    std::atomic<UINT> _gameBufferCount { 0 };
    std::atomic<bool> _presentParamsLogged { false };
    void NoteUpgrade(const SdrToHdr::Plan& plan);

    // This is the frame generation library's own real chain (SdrToHdr::Plan::fgLibraryChain), also when HDR output
    // stood aside at creation: XeFG and FSR FG make it through our factory hooks and draw / present through it. Decided
    // again at every resize.
    bool _fgLibraryChain = false;
    // The library asked for exclusive full screen and its HDR chain was made windowed (Plan::exclusiveToWindowed). The
    // window is kept borderless by FGHooks::KeepBorderless (the game-facing chain's own mechanism); here DXGI's
    // Alt+Enter is switched off, so it cannot put this real flip chain into exclusive full screen behind everyone's
    // back
    bool _libraryWindowed = false;
    void NoteLibraryWindowed(const SdrToHdr::Plan& plan);
    void DisableAltEnter();

    // Exclusive full screen is never entered by an upgraded chain (it would be invalid until a resize, which a bitblt-era game
    // never does): the game's SetFullscreenState(TRUE) is answered with a borderless window (SdrToHdr_Fullscreen.h), and DXGI's
    // own Alt+Enter is switched off for the window. Alt+Enter itself is therefore not handled for such a game; its own menu is.
    // The window itself is changed by the thread that calls SetFullscreenState (BorderlessFullscreen guards the transition and
    // answers Active from an atomic); the output the game is told about is read from other threads, so it has its own small mutex
    // (not _localMutex: Present holds that).
    SdrToHdr::BorderlessFullscreen _borderless;
    std::mutex _borderlessMutex;
    ComPtr<IDXGIOutput> _borderlessTarget;
    std::atomic<bool> _exclusiveRecoveryLogged { false };
    std::atomic<bool> _borderlessRefusedLogged { false };
    HRESULT EmulateFullscreen(BOOL fullscreen, IDXGIOutput* target);
    void RecoverFromExclusive(HRESULT presentResult);

    void StartHdrPlan(const SdrToHdr::Plan& plan, UINT flags);
    SdrToHdr::ProxyChainBase* Proxy() const { return _proxyPtr.load(std::memory_order_acquire); }
    SdrToHdr::ProxyChainBase* EnsureProxy(bool d3d11)
    {
        if (auto* existing = Proxy())
            return existing;

        // Built first, published by compare-exchange: of two threads that get here at once one chain wins, the other is
        // dropped and its caller uses the winner. Ownership moves to the wrapper only for the winner.
        std::unique_ptr<SdrToHdr::ProxyChainBase> fresh;

        if (d3d11)
            fresh = std::make_unique<SdrToHdr::ProxyChainDx11>();
        else
            fresh = std::make_unique<SdrToHdr::ProxyChain>();

        SdrToHdr::ProxyChainBase* expected = nullptr;

        if (_proxyPtr.compare_exchange_strong(expected, fresh.get(), std::memory_order_acq_rel))
        {
            _proxyOwner = std::move(fresh);
            return _proxyOwner.get();
        }

        return expected;
    }
    bool Proxied() const
    {
        auto* proxy = Proxy();
        return proxy != nullptr && proxy->Active();
    }
    // The game's device: the Direct3D 12 device behind the queue the chain was made with, or the Direct3D 11 device (d3d11).
    // Null when it is neither.
    IUnknown* ApiDevice(bool& d3d11) const;
    HRESULT ResizeHdr(bool v1, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format, UINT SwapChainFlags,
                      const UINT* pCreationNodeMask, IUnknown* const* ppPresentQueue, bool& handled);

#ifdef USE_LOCAL_MUTEX
    OwnedMutex _localMutex;
#endif

    void CheckForHdrOutput()
    {
        ComPtr<IDXGIOutput> output;

        HRESULT hr = GetContainingOutput(&output);
        if (SUCCEEDED(hr) && output)
        {
            ComPtr<IDXGIOutput6> output6;

            if (SUCCEEDED(output.As(&output6)))
            {
                DXGI_OUTPUT_DESC1 desc {};
                if (SUCCEEDED(output6->GetDesc1(&desc)))
                {
                    State::Instance().hdrOutputActive = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;

                    // HDR output also wants the SDR white level and the peak: only read while it is on
                    if (Config::Instance()->HdrOutputEnabled.value_or_default() || Proxy() != nullptr)
                    {
                        _display = SdrToHdr::QueryDisplay(output.Get());
                        _displayKnown = true;
                    }
                }
            }
        }
    }
};
