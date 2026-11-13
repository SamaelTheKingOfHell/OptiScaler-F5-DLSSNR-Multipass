#include "pch.h"
#include "dx11_with_dx12_sc.h"

#include <with_dx12/with_dx12.h>
#include <with_dx12/dx11_with_dx12_sync.h>

#include <hooks/FG_Hooks.h>
#include <menu/menu_overlay_dx.h>
#include <native/NativeDriverDx11.h>
#include <native/NativeLowLatency.h>

#include <Util.h>
#include <Config.h>

#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#pragma intrinsic(_ReturnAddress)

static int scCount = 0;

namespace
{
template <typename T> void SafeRelease(T*& value)
{
    if (value != nullptr)
    {
        value->Release();
        value = nullptr;
    }
}

void SafeCloseHandle(HANDLE& value)
{
    if (value != nullptr)
    {
        CloseHandle(value);
        value = nullptr;
    }
}

bool WaitForQueueIdle(ID3D12CommandQueue* queue, ID3D12Fence* fence, HANDLE fenceEvent, UINT64& fenceValue)
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

void TransitionResource(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource,
                        D3D12_RESOURCE_STATES beforeState, D3D12_RESOURCE_STATES afterState)
{
    if (commandList == nullptr || resource == nullptr || beforeState == afterState)
        return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = beforeState;
    barrier.Transition.StateAfter = afterState;

    commandList->ResourceBarrier(1, &barrier);
}

UINT ResolveBufferCount(IDXGISwapChain* swapchain, IDXGISwapChain1* swapchain1)
{
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (swapchain != nullptr && SUCCEEDED(swapchain->GetDesc(&desc)) && desc.BufferCount > 0)
        return desc.BufferCount;

    DXGI_SWAP_CHAIN_DESC1 desc1 = {};
    if (swapchain1 != nullptr && SUCCEEDED(swapchain1->GetDesc1(&desc1)) && desc1.BufferCount > 0)
        return desc1.BufferCount;

    return 2;
}

DXGI_FORMAT ResolveBufferFormat(IDXGISwapChain* swapchain, IDXGISwapChain1* swapchain1)
{
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (swapchain != nullptr && SUCCEEDED(swapchain->GetDesc(&desc)) && desc.BufferDesc.Format != DXGI_FORMAT_UNKNOWN)
        return desc.BufferDesc.Format;

    DXGI_SWAP_CHAIN_DESC1 desc1 = {};
    if (swapchain1 != nullptr && SUCCEEDED(swapchain1->GetDesc1(&desc1)))
        return desc1.Format;

    return DXGI_FORMAT_UNKNOWN;
}

bool IsSame(IDXGISwapChain* swapchain, UINT bufferCount, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
{
    if (swapchain == nullptr)
        return false;

    DXGI_SWAP_CHAIN_DESC desc {};
    if (FAILED(swapchain->GetDesc(&desc)))
        return false;

    const UINT resolvedBufferCount = bufferCount != 0 ? bufferCount : desc.BufferCount;
    const UINT resolvedWidth = width != 0 ? width : desc.BufferDesc.Width;
    const UINT resolvedHeight = height != 0 ? height : desc.BufferDesc.Height;
    const DXGI_FORMAT resolvedFormat = format != DXGI_FORMAT_UNKNOWN ? format : desc.BufferDesc.Format;

    return resolvedBufferCount == desc.BufferCount && resolvedWidth == desc.BufferDesc.Width &&
           resolvedHeight == desc.BufferDesc.Height && resolvedFormat == desc.BufferDesc.Format && flags == desc.Flags;
}

struct FgResizeArgs
{
    UINT bufferCount;
    DXGI_FORMAT format;
    UINT flags;
};

// What the game's resize becomes for the FG swapchain, which the bridge created as a flip one (DxgiFactory_Hooks'
// PrepareDx12InteropDesc): an SRGB format as its UNORM one, fewer than 2 buffers as the count it has, and the flags it
// was created with (creation drops some, such as GDI_COMPATIBLE, and a flip swapchain's tearing and waitable-object
// flags can't change in a resize). A flip swapchain rejects an SRGB format or 1 buffer (E_INVALIDARG); it then kept
// its old size while the bridge copied the new-size picture into it, and the GPU hung (MGS V and Mad Max resize with an
// SRGB format, MGS V with 1 buffer). A resize that only differs in flags is skipped. Formats the creation path refuses
// outright are not changed here.
FgResizeArgs ResizeArgsForFg(IDXGISwapChain* fgSwapChain, UINT bufferCount, DXGI_FORMAT format, UINT flags)
{
    FgResizeArgs args { bufferCount, format, flags };

    if (args.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
        args.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    else if (args.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
        args.format = DXGI_FORMAT_B8G8R8A8_UNORM;

    if (args.bufferCount == 1)
        args.bufferCount = 0;

    DXGI_SWAP_CHAIN_DESC desc {};
    if (fgSwapChain != nullptr && SUCCEEDED(fgSwapChain->GetDesc(&desc)))
        args.flags = desc.Flags;

    if (args.bufferCount != bufferCount || args.format != format || args.flags != flags)
    {
        LOG_INFO("FG swapchain resizes as created: count {} -> {}, format {} -> {}, flags {:X} -> {:X}", bufferCount,
                 args.bufferCount, (UINT) format, (UINT) args.format, flags, args.flags);
    }

    return args;
}

// The menu overlay's teardown in a bridge resize. A real size change can't hold the present barrier for the whole call:
// frame generation's own resize waits for its present thread, which takes the shared side (XeFG's presents only,
// wrapped_swapchain's LocalPresent). That thread draws the overlay, and tearing the overlay down under it freed
// ImGui's viewport data while it was in use (Mad Max, XeFG); so the barrier is held for the teardown only. Frame
// generation is switched off before, outside the barrier: the teardown would otherwise do it while holding it, and
// switching XeFG off can wait for that same present thread. The caller keeps a ScopedResize for the whole resize, so
// the present leaves the overlay alone until it is done.
void TearDownOverlayForResize(HWND handle, bool barrierHeld)
{
    auto fg = State::Instance().currentFG;
    if (fg != nullptr && fg->FrameGenerationContext() != nullptr && fg->IsActive())
    {
        State::Instance().fgChanged = true;
        fg->UpdateTarget();
        fg->Deactivate();
    }

    std::unique_lock<std::shared_mutex> overlayLock(Dx11wDx12Sync::PresentResizeMutex(), std::defer_lock);
    if (!barrierHeld && UsesXeFGApi(State::Instance().activeFgOutput))
        overlayLock.lock();

    MenuOverlayDx::CleanupRenderTarget(true, handle);
}
} // namespace

Dx11wDx12SC::Dx11wDx12SC(IDXGISwapChain* real, IDXGISwapChain4* fgSC, ID3D11Device* pDevice, HWND hWnd, UINT flags)
    : _real(real), _fgSwapChain(fgSC), _dx11Device(pDevice), _handle(hWnd), _refcount(1)
{
    _id = ++scCount;
    _lastFlags = flags;

    // Exclusive fullscreen on XeFG's swapchain behind this bridge leaves every following Present failing with
    // DXGI_ERROR_INVALID_CALL (seen in Prey and Blair Witch, both D3D11 through Dx11wDx12). Default to the existing
    // borderless override here, same as the per-game ForceBorderlessWhenUsingXeFG quirk does; an explicit
    // ForceBorderless in the ini still wins. FGHooks::hkSetFullscreenState reads this live, so it applies to the
    // game's first SetFullscreenState right after this swapchain is handed back.
    if (UsesXeFGApi(State::Instance().activeFgOutput) && !Config::Instance()->FGXeFGForceBorderless.has_value())
    {
        LOG_INFO("XeFG through Dx11wDx12: forcing borderless instead of exclusive fullscreen");
        Config::Instance()->FGXeFGForceBorderless.set_volatile_value(true);
    }

    if (_real != nullptr)
    {
        _real->AddRef();
        _real->QueryInterface(IID_PPV_ARGS(&_real1));
        _real->QueryInterface(IID_PPV_ARGS(&_real2));
        _real->QueryInterface(IID_PPV_ARGS(&_real3));
        _real->QueryInterface(IID_PPV_ARGS(&_real4));
    }

    if (_fgSwapChain != nullptr)
        _fgSwapChain->AddRef();

    if (_dx11Device != nullptr)
    {
        _dx11Device->AddRef();
        auto device5Result = _dx11Device->QueryInterface(IID_PPV_ARGS(&_dx11Device5));
        if (FAILED(device5Result))
            LOG_WARN("ID3D11Device5 unavailable: {:X}", (UINT) device5Result);

        _dx11Device->GetImmediateContext(&_dx11Context);
        if (_dx11Context != nullptr)
        {
            auto context4Result = _dx11Context->QueryInterface(IID_PPV_ARGS(&_dx11Context4));
            if (FAILED(context4Result))
                LOG_WARN("ID3D11DeviceContext4 unavailable: {:X}", (UINT) context4Result);
        }
    }

    if (WithDx12::PrepareD3D12ForD3D11(_dx11Device, D3D_FEATURE_LEVEL_11_0))
    {
        _dx12Device = WithDx12::GetD3D12Device();
        _dx12CommandQueue = WithDx12::GetD3D12CommandQueue();
    }
    else
    {
        LOG_ERROR("failed to resolve D3D12 device/queue");
    }

    State::Instance().swapchainInteropApi = SwapchainInteropApi::Dx11wDx12;

    _RefreshCachedSwapchainDesc();

    LOG_INFO("Dx11wDx12SC {} created, real: {:X}, fg: {:X}, dx11: {:X}, dx12: {:X}, queue: {:X}", _id, (UINT64) _real,
             (UINT64) _fgSwapChain, (UINT64) _dx11Device, (UINT64) _dx12Device, (UINT64) _dx12CommandQueue);
}

Dx11wDx12SC::~Dx11wDx12SC()
{
    MenuOverlayDx::CleanupRenderTarget(true, _handle);
    _ReleaseInteropObjects();

    SafeRelease(_real4);
    SafeRelease(_real3);
    SafeRelease(_real2);
    SafeRelease(_real1);
    SafeRelease(_fgSwapChain);
    SafeRelease(_real);
    SafeRelease(_dx11Context4);
    SafeRelease(_dx11Context);
    SafeRelease(_dx11Device5);
    SafeRelease(_dx11Device);
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::QueryInterface(REFIID riid, void** ppvObject)
{
    LOG_TRACE("Caller: {}", Util::WhoIsTheCaller(_ReturnAddress()));

    if (ppvObject == nullptr)
        return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDeviceSubObject) ||
        riid == __uuidof(IDXGISwapChain))
    {
        AddRef();
        *ppvObject = static_cast<IDXGISwapChain*>(this);
        return S_OK;
    }

    if (riid == __uuidof(IDXGISwapChain1))
    {
        if (_real1 == nullptr)
            return E_NOINTERFACE;

        AddRef();
        *ppvObject = static_cast<IDXGISwapChain1*>(this);
        return S_OK;
    }

    if (riid == __uuidof(IDXGISwapChain2))
    {
        if (_real2 == nullptr)
            return E_NOINTERFACE;

        AddRef();
        *ppvObject = static_cast<IDXGISwapChain2*>(this);
        return S_OK;
    }

    if (riid == __uuidof(IDXGISwapChain3))
    {
        if (_real3 == nullptr)
            return E_NOINTERFACE;

        AddRef();
        *ppvObject = static_cast<IDXGISwapChain3*>(this);
        return S_OK;
    }

    if (riid == __uuidof(IDXGISwapChain4))
    {
        if (_real4 == nullptr && _fgSwapChain == nullptr)
            return E_NOINTERFACE;

        AddRef();
        *ppvObject = static_cast<IDXGISwapChain4*>(this);
        return S_OK;
    }

    if (riid == __uuidof(Dx11wDx12SC))
    {
        AddRef();
        *ppvObject = this;
        return S_OK;
    }

    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE Dx11wDx12SC::AddRef()
{
    auto count = InterlockedIncrement(&_refcount);
    LOG_TRACE("Count: {}, caller: {}", count, Util::WhoIsTheCaller(_ReturnAddress()));
    return count;
}

ULONG STDMETHODCALLTYPE Dx11wDx12SC::Release()
{
    ULONG ret = InterlockedDecrement(&_refcount);

    LOG_TRACE("Count: {}, caller: {}", ret, Util::WhoIsTheCaller(_ReturnAddress()));

    if (ret == 0)
    {
        if (State::Instance().currentSwapchain == this)
            State::Instance().currentSwapchain = nullptr;

        if (State::Instance().currentWrappedSwapchain == this)
            State::Instance().currentWrappedSwapchain = nullptr;

        if (State::Instance().currentRealSwapchain == _real)
            State::Instance().currentRealSwapchain = nullptr;

        FGHooks::ClearDx12InteropPresentSC(_fgSwapChain);

        if (State::Instance().currentFGSwapchain == _fgSwapChain)
            State::Instance().currentFGSwapchain = nullptr;

        if (State::Instance().currentD3D11Device == _dx11Device)
            State::Instance().currentD3D11Device = nullptr;

        State::Instance().swapchainInteropApi = SwapchainInteropApi::None;

        auto fg = State::Instance().currentFG;
        const bool releaseFg = fg != nullptr && fg->Mutex.getOwner() != 1 && fg->SwapchainContext() != nullptr;
        const auto handle = _handle;

        if (releaseFg)
            fg->Deactivate();

        // Intel: XeFG's context is destroyed after every reference to its swapchain is gone (else
        // xefgSwapChainDestroy fails with POINTER_STILL_IN_USE). The destructor releases ours.
        delete this;

        if (releaseFg)
            fg->ReleaseSwapchain(handle);
    }

    return ret;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetPrivateData(REFGUID Name, UINT DataSize, const void* pData)
{
    return _real != nullptr ? _real->SetPrivateData(Name, DataSize, pData) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown)
{
    return _real != nullptr ? _real->SetPrivateDataInterface(Name, pUnknown) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData)
{
    return _real != nullptr ? _real->GetPrivateData(Name, pDataSize, pData) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetParent(REFIID riid, void** ppParent)
{
    return _real != nullptr ? _real->GetParent(riid, ppParent) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetDevice(REFIID riid, void** ppDevice)
{
    return _real != nullptr ? _real->GetDevice(riid, ppDevice) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::Present(UINT SyncInterval, UINT Flags)
{
    if (_real == nullptr || _fgSwapChain == nullptr)
        return DXGI_ERROR_DEVICE_REMOVED;

    if (_fg == nullptr)
        _fg = State::Instance().currentFG;

    if ((Flags & DXGI_PRESENT_TEST) != 0)
        return _real->Present(SyncInterval, Flags);

    // Optical F5Low's low latency (native/NativeLowLatency.h): Reflex markers around the game's present, on the game's
    // own D3D11 device. This is the game's only present here; the bridge's D3D12 presents belong to frame generation.
    struct LowLatencyScope
    {
        explicit LowLatencyScope(IUnknown* device) : _device(device)
        {
            if (_device != nullptr)
                native::lowlatency::OnPresentBegin(_device);
        }

        ~LowLatencyScope()
        {
            if (_device != nullptr)
                native::lowlatency::OnPresentEnd(_device);
        }

        IUnknown* _device;
    } lowLatencyScope(State::Instance().currentD3D11Device);

    if (!_InitInteropObjects())
        return DXGI_ERROR_DEVICE_REMOVED;

    auto dx11Index = _GetDx11BackBufferIndexForPresent();

    if (!_RequestSharedBackBuffer(dx11Index))
        return DXGI_ERROR_DEVICE_REMOVED;

    if (!_CopyDx11BackBufferToShared(dx11Index))
        return DXGI_ERROR_DEVICE_REMOVED;

    if (!_WaitDx11ThenDx12())
        return DXGI_ERROR_DEVICE_REMOVED;

    if (!_CopyDx11SharedToDx12FGBackBuffer(dx11Index))
        return DXGI_ERROR_DEVICE_REMOVED;

    // Overwrites the raw copy just made above with native input's processed picture, when that option is on; a no-op
    // otherwise. Runs here, before FG takes the frame: this is the only present this game gets once Dx11wDx12SC
    // exists, so it is also where the D3D11 depth finder's own frame close has to happen (NativeMotionDx11::
    // OnFGPresent does that), same placement rule Story F used on the D3D12 side.
    _ApplyNativeInputToFGBackBuffer();

    if (!_WaitForInteropCopyOnPresentQueue())
        return DXGI_ERROR_DEVICE_REMOVED;

    const bool fgHookedPresenter =
        State::Instance().currentFGSwapchain == _fgSwapChain && !FGHooks::IsDx12InteropPresentSC(_fgSwapChain);

    // The game-facing DX11 swapchain is never presented in this wrapper.
    // For a plain external DX12 presenter, draw Opti's overlay here.
    // For a real FG swapchain, FGHooks::FGPresent/LocalPresent owns overlay/present-side work;
    // drawing it here would double-enter the overlay path before the FG present hook.
    if (!fgHookedPresenter)
        MenuOverlayDx::Present(_fgSwapChain, SyncInterval, Flags, nullptr, _dx12CommandQueue, _handle, false);
    else
        LOG_TRACE("real FG presenter detected; skipping wrapper overlay path");

    LOG_DEBUG("frame {}, dx11Index {}, real3Index {}, fakeIndex {}, bufferCount {}", State::Instance().frameCount,
              dx11Index, _real3 != nullptr ? _real3->GetCurrentBackBufferIndex() : 0xFFFFFFFF, _currentFakeIndex,
              _bufferCount);

    if (_real != nullptr)
    {
        UINT realFlags = Flags;

        // Do not wait for it
        auto realPresentResult = _real->Present(0, realFlags);

        if (FAILED(realPresentResult))
            LOG_WARN("hidden real DX11 Present failed: {:X}", (UINT) realPresentResult);
    }

    auto result = _fgSwapChain->Present(SyncInterval, Flags);

    if (SUCCEEDED(result))
    {
        _AdvanceFakeBackBufferIndex();
    }
    else
    {
        LOG_ERROR("fg Present failed: {:X}", (UINT) result);

        // The reason (and the D3D12 DRED report, when on) is the bridge's D3D12 device's; the outer present only asks
        // the game's D3D11 one
        if ((result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET) && _dx12Device != nullptr)
            Util::GetDeviceRemovedReason(_dx12Device);
    }

    return result;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetBuffer(UINT Buffer, REFIID riid, void** ppSurface)
{
    return _real != nullptr ? _real->GetBuffer(Buffer, riid, ppSurface) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget)
{
    DXGI_OUTPUT_DESC targetDesc {};
    const bool targetDescribed = pTarget != nullptr && SUCCEEDED(pTarget->GetDesc(&targetDesc));

    LOG_DEBUG("Dx11wDx12SC SetFullscreenState: {}, target: {:X} ({}), caller: {}", Fullscreen, (size_t) pTarget,
              targetDescribed ? wstring_to_string(targetDesc.DeviceName) : std::string("-"),
              Util::WhoIsTheCaller(_ReturnAddress()));

    State::Instance().realExclusiveFullscreen = Fullscreen;

    if (_fgSwapChain != nullptr)
        return _fgSwapChain->SetFullscreenState(Fullscreen, pTarget);

    return _real != nullptr ? _real->SetFullscreenState(Fullscreen, pTarget) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->GetFullscreenState(pFullscreen, ppTarget);

    return _real != nullptr ? _real->GetFullscreenState(pFullscreen, ppTarget) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc)
{
    if (_real == nullptr)
        return DXGI_ERROR_DEVICE_REMOVED;

    auto result = _real->GetDesc(pDesc);
    if (SUCCEEDED(result) && pDesc != nullptr)
        pDesc->OutputWindow = _handle;

    return result;
}

// ResizeBuffers(0, 0) means "the window's size". With the window still hidden or minimised (a game leaving exclusive full screen
// before it sizes its window) that is 1x1, and the picture, XeFG and the upscaler all end up at 1x1 for good. Keep the size the
// swapchain has until the window is real: the game resizes again when it is.
static void KeepSizeWhileWindowTiny(IDXGISwapChain* swapChain, HWND window, UINT& width, UINT& height)
{
    if (width != 0 && height != 0)
        return;

    RECT client {};
    const bool haveClient = window != nullptr && GetClientRect(window, &client);

    LOG_INFO("ResizeBuffers 0x0: window {:X} client {}x{} (read {}), iconic {}, visible {}", (uintptr_t) window,
             client.right, client.bottom, haveClient, window != nullptr && IsIconic(window),
             window != nullptr && IsWindowVisible(window));

    if (!haveClient || swapChain == nullptr)
        return;

    // A real window: say its size ourselves. The swapchain behind the bridge came out 1x1 from a 0x0 here (Call to Arms leaving
    // exclusive full screen, window 2560x1440).
    if (client.right >= 16 && client.bottom >= 16)
    {
        width = (UINT) client.right;
        height = (UINT) client.bottom;
        return;
    }

    DXGI_SWAP_CHAIN_DESC current {};

    if (FAILED(swapChain->GetDesc(&current)))
        return;

    LOG_WARN("ResizeBuffers 0x0 with a {}x{} window: keeping {}x{}", client.right, client.bottom,
             current.BufferDesc.Width, current.BufferDesc.Height);
    width = current.BufferDesc.Width;
    height = current.BufferDesc.Height;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat,
                                                     UINT SwapChainFlags)
{
    KeepSizeWhileWindowTiny(_fgSwapChain, _handle, Width, Height);

    LOG_DEBUG("Dx11wDx12SC ResizeBuffers: count {}, size {}x{}, format {}, flags {:X}", BufferCount, Width, Height,
              (UINT) NewFormat, SwapChainFlags);

    const auto fgArgs = ResizeArgsForFg(_fgSwapChain, BufferCount, NewFormat, SwapChainFlags);
    const bool skipFgResize = IsSame(_fgSwapChain, fgArgs.bufferCount, Width, Height, fgArgs.format, fgArgs.flags);
    const bool synchronizeXeFGPresent = skipFgResize && UsesXeFGApi(State::Instance().activeFgOutput) &&
                                        State::Instance().swapchainInteropApi == SwapchainInteropApi::Dx11wDx12;

    std::unique_lock<std::shared_mutex> presentResizeLock(Dx11wDx12Sync::PresentResizeMutex(), std::defer_lock);
    if (synchronizeXeFGPresent)
    {
        auto fg = State::Instance().currentFG;
        if (fg != nullptr && fg->FrameGenerationContext() != nullptr && fg->IsActive())
        {
            State::Instance().fgChanged = true;
            fg->UpdateTarget();
            fg->Deactivate();
        }

        LOG_DEBUG("Waiting for XeFG native presents before equivalent ResizeBuffers");
        presentResizeLock.lock();
        LOG_DEBUG("XeFG native present barrier acquired");
    }

    if (!_WaitForCopyQueueIdle())
        LOG_WARN("continuing ResizeBuffers after copy fence wait failure");

    if (synchronizeXeFGPresent && !WaitForQueueIdle(_dx12CommandQueue, _copyFence, _copyFenceEvent, _copyFenceValue))
    {
        LOG_WARN("continuing ResizeBuffers after FG/present queue wait failure");
    }

    Dx11wDx12Sync::ScopedResize resizing {};
    TearDownOverlayForResize(_handle, presentResizeLock.owns_lock());

    _ReleaseInteropBackBuffers();

    HRESULT realResult = _real != nullptr ? _real->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags)
                                          : DXGI_ERROR_DEVICE_REMOVED;

    HRESULT fgResult = DXGI_ERROR_DEVICE_REMOVED;
    if (SUCCEEDED(realResult) && _fgSwapChain != nullptr)
    {
        if (skipFgResize)
        {
            LOG_DEBUG("Skipping FG ResizeBuffers");
            fgResult = S_OK;
        }
        else
        {
            fgResult = _fgSwapChain->ResizeBuffers(fgArgs.bufferCount, Width, Height, fgArgs.format, fgArgs.flags);
        }
    }

    LOG_DEBUG("Dx11wDx12SC ResizeBuffers results: real {:X}, fg {:X}", (UINT) realResult, (UINT) fgResult);

    if (_real != nullptr)
    {
        DXGI_SWAP_CHAIN_DESC after {};

        if (SUCCEEDED(_real->GetDesc(&after)))
            LOG_INFO("ResizeBuffers: the game's swapchain is now {}x{}", after.BufferDesc.Width, after.BufferDesc.Height);
    }

    if (SUCCEEDED(realResult) && SUCCEEDED(fgResult))
    {
        _RefreshCachedSwapchainDesc();

        if (Config::Instance()->IsFGEnabled())
        {
            State::Instance().fgResetCapturedResources = true;
            State::Instance().fgOnlyUseCapturedResources = false;
            State::Instance().fgChanged = true;
        }

        State::Instance().scChanged = true;
        State::Instance().SCAllowTearing = (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;

        if (State::Instance().currentFeature == nullptr)
        {
            State::Instance().screenWidth = static_cast<float>(Width);
            State::Instance().screenHeight = static_cast<float>(Height);
            State::Instance().lastMipBias = 100.0f;
            State::Instance().lastMipBiasMax = -100.0f;
        }
    }

    return FAILED(realResult) ? realResult : fgResult;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters)
{
    return _fgSwapChain != nullptr
               ? _fgSwapChain->ResizeTarget(pNewTargetParameters)
               : (_real != nullptr ? _real->ResizeTarget(pNewTargetParameters) : DXGI_ERROR_DEVICE_REMOVED);
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetContainingOutput(IDXGIOutput** ppOutput)
{
    const HRESULT result =
        _fgSwapChain != nullptr
            ? _fgSwapChain->GetContainingOutput(ppOutput)
            : (_real != nullptr ? _real->GetContainingOutput(ppOutput) : DXGI_ERROR_DEVICE_REMOVED);

    // Which display the game is told it is on (its resolution list follows it): the output, and where the window is.
    DXGI_OUTPUT_DESC desc {};
    RECT window {};
    const bool described = SUCCEEDED(result) && ppOutput != nullptr && *ppOutput != nullptr &&
                           SUCCEEDED((*ppOutput)->GetDesc(&desc));
    GetWindowRect(_handle, &window);

    LOG_DEBUG("GetContainingOutput {:X}: {} ({},{} {}x{}), window ({},{} {}x{})", (UINT) result,
              described ? wstring_to_string(desc.DeviceName) : std::string("-"), desc.DesktopCoordinates.left,
              desc.DesktopCoordinates.top, desc.DesktopCoordinates.right - desc.DesktopCoordinates.left,
              desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top, window.left, window.top,
              window.right - window.left, window.bottom - window.top);

    return result;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats)
{
    return _fgSwapChain != nullptr ? _fgSwapChain->GetFrameStatistics(pStats)
                                   : (_real != nullptr ? _real->GetFrameStatistics(pStats) : DXGI_ERROR_DEVICE_REMOVED);
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetLastPresentCount(UINT* pLastPresentCount)
{
    return _fgSwapChain != nullptr
               ? _fgSwapChain->GetLastPresentCount(pLastPresentCount)
               : (_real != nullptr ? _real->GetLastPresentCount(pLastPresentCount) : DXGI_ERROR_DEVICE_REMOVED);
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc)
{
    return _real1 != nullptr ? _real1->GetDesc1(pDesc) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->GetFullscreenDesc(pDesc);

    return _real1 != nullptr ? _real1->GetFullscreenDesc(pDesc) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetHwnd(HWND* pHwnd)
{
    if (pHwnd == nullptr)
        return DXGI_ERROR_INVALID_CALL;

    *pHwnd = _handle;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetCoreWindow(REFIID refiid, void** ppUnk)
{
    return _real1 != nullptr ? _real1->GetCoreWindow(refiid, ppUnk) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::Present1(UINT SyncInterval, UINT Flags,
                                                const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    UNREFERENCED_PARAMETER(pPresentParameters);
    return Present(SyncInterval, Flags);
}

BOOL STDMETHODCALLTYPE Dx11wDx12SC::IsTemporaryMonoSupported(void)
{
    return _real1 != nullptr ? _real1->IsTemporaryMonoSupported() : FALSE;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput)
{
    return _real1 != nullptr ? _real1->GetRestrictToOutput(ppRestrictToOutput) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetBackgroundColor(const DXGI_RGBA* pColor)
{
    return _real1 != nullptr ? _real1->SetBackgroundColor(pColor) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetBackgroundColor(DXGI_RGBA* pColor)
{
    return _real1 != nullptr ? _real1->GetBackgroundColor(pColor) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetRotation(DXGI_MODE_ROTATION Rotation)
{
    return _real1 != nullptr ? _real1->SetRotation(Rotation) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetRotation(DXGI_MODE_ROTATION* pRotation)
{
    return _real1 != nullptr ? _real1->GetRotation(pRotation) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetSourceSize(UINT Width, UINT Height)
{
    return _real2 != nullptr ? _real2->SetSourceSize(Width, Height) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetSourceSize(UINT* pWidth, UINT* pHeight)
{
    return _real2 != nullptr ? _real2->GetSourceSize(pWidth, pHeight) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetMaximumFrameLatency(UINT MaxLatency)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->SetMaximumFrameLatency(MaxLatency);

    return _real2 != nullptr ? _real2->SetMaximumFrameLatency(MaxLatency) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetMaximumFrameLatency(UINT* pMaxLatency)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->GetMaximumFrameLatency(pMaxLatency);

    return _real2 != nullptr ? _real2->GetMaximumFrameLatency(pMaxLatency) : DXGI_ERROR_DEVICE_REMOVED;
}

HANDLE STDMETHODCALLTYPE Dx11wDx12SC::GetFrameLatencyWaitableObject(void)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->GetFrameLatencyWaitableObject();

    return _real2 != nullptr ? _real2->GetFrameLatencyWaitableObject() : nullptr;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix)
{
    return _real2 != nullptr ? _real2->SetMatrixTransform(pMatrix) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix)
{
    return _real2 != nullptr ? _real2->GetMatrixTransform(pMatrix) : DXGI_ERROR_DEVICE_REMOVED;
}

UINT STDMETHODCALLTYPE Dx11wDx12SC::GetCurrentBackBufferIndex(void)
{
    if (_real3 != nullptr)
        return _real3->GetCurrentBackBufferIndex();

    return _currentFakeIndex;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                              UINT* pColorSpaceSupport)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport);

    return _real3 != nullptr ? _real3->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport)
                             : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->SetColorSpace1(ColorSpace);

    return _real3 != nullptr ? _real3->SetColorSpace1(ColorSpace) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format,
                                                      UINT SwapChainFlags, const UINT* pCreationNodeMask,
                                                      IUnknown* const* ppPresentQueue)
{
    KeepSizeWhileWindowTiny(_fgSwapChain, _handle, Width, Height);

    LOG_DEBUG("Dx11wDx12SC ResizeBuffers1: count {}, size {}x{}, format {}, flags {:X}", BufferCount, Width, Height,
              (UINT) Format, SwapChainFlags);

    if (_real3 == nullptr)
        return ResizeBuffers(BufferCount, Width, Height, Format, SwapChainFlags);

    const auto fgArgs = ResizeArgsForFg(_fgSwapChain, BufferCount, Format, SwapChainFlags);
    const bool skipFgResize = IsSame(_fgSwapChain, fgArgs.bufferCount, Width, Height, fgArgs.format, fgArgs.flags);
    const bool synchronizeXeFGPresent = skipFgResize && UsesXeFGApi(State::Instance().activeFgOutput) &&
                                        State::Instance().swapchainInteropApi == SwapchainInteropApi::Dx11wDx12;

    std::unique_lock<std::shared_mutex> presentResizeLock(Dx11wDx12Sync::PresentResizeMutex(), std::defer_lock);
    if (synchronizeXeFGPresent)
    {
        auto fg = State::Instance().currentFG;
        if (fg != nullptr && fg->FrameGenerationContext() != nullptr && fg->IsActive())
        {
            State::Instance().fgChanged = true;
            fg->UpdateTarget();
            fg->Deactivate();
        }

        LOG_DEBUG("Waiting for XeFG native presents before equivalent ResizeBuffers1");
        presentResizeLock.lock();
        LOG_DEBUG("XeFG native present barrier acquired");
    }

    if (!_WaitForCopyQueueIdle())
        LOG_WARN("continuing ResizeBuffers1 after copy fence wait failure");

    if (synchronizeXeFGPresent && !WaitForQueueIdle(_dx12CommandQueue, _copyFence, _copyFenceEvent, _copyFenceValue))
    {
        LOG_WARN("continuing ResizeBuffers1 after FG/present queue wait failure");
    }

    Dx11wDx12Sync::ScopedResize resizing {};
    TearDownOverlayForResize(_handle, presentResizeLock.owns_lock());

    _ReleaseInteropBackBuffers();

    HRESULT realResult =
        _real3->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask, ppPresentQueue);

    HRESULT fgResult = DXGI_ERROR_DEVICE_REMOVED;
    if (SUCCEEDED(realResult) && _fgSwapChain != nullptr)
    {
        // The game's ppPresentQueue is not valid for the DX12 FG swapchain. Use ResizeBuffers for phase 1.
        if (skipFgResize)
        {
            LOG_DEBUG("Skipping FG ResizeBuffers1");
            fgResult = S_OK;
        }
        else
        {
            fgResult = _fgSwapChain->ResizeBuffers(fgArgs.bufferCount, Width, Height, fgArgs.format, fgArgs.flags);
        }
    }

    LOG_DEBUG("Dx11wDx12SC ResizeBuffers1 results: real {:X}, fg {:X}", (UINT) realResult, (UINT) fgResult);

    if (SUCCEEDED(realResult) && SUCCEEDED(fgResult))
    {
        _RefreshCachedSwapchainDesc();

        if (Config::Instance()->IsFGEnabled())
        {
            State::Instance().fgResetCapturedResources = true;
            State::Instance().fgOnlyUseCapturedResources = false;
            State::Instance().fgChanged = true;
        }

        State::Instance().scChanged = true;
        State::Instance().SCAllowTearing = (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;
    }

    return FAILED(realResult) ? realResult : fgResult;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->SetHDRMetaData(Type, Size, pMetaData);

    return _real4 != nullptr ? _real4->SetHDRMetaData(Type, Size, pMetaData) : DXGI_ERROR_DEVICE_REMOVED;
}

bool Dx11wDx12SC::_InitInteropObjects()
{
    if (_interopInitialized)
    {
        _RefreshCachedSwapchainDesc();

        if (_bufferCount == 0)
        {
            LOG_ERROR("InitInteropObjects resolved zero backbuffers");
            return false;
        }

        if (_sharedDx11BackBufferCopies.size() != _bufferCount)
            _sharedDx11BackBufferCopies.resize(_bufferCount, nullptr);

        if (_openedDx11BackBuffers.size() != _bufferCount)
            _openedDx11BackBuffers.resize(_bufferCount, nullptr);

        if (_sharedBackBufferHandles.size() != _bufferCount)
            _sharedBackBufferHandles.resize(_bufferCount, nullptr);

        if (_openedDx11BackBufferStates.size() != _bufferCount)
            _openedDx11BackBufferStates.resize(_bufferCount, D3D12_RESOURCE_STATE_COMMON);

        return true;
    }

    if (_dx11Device == nullptr || _dx11Device5 == nullptr || _dx11Context4 == nullptr || _dx12Device == nullptr ||
        _dx12CommandQueue == nullptr)
    {
        LOG_ERROR("interop objects missing: dx11 {}, dx11-5 {}, ctx4 {}, dx12 {}, queue {}", (UINT64) _dx11Device,
                  (UINT64) _dx11Device5, (UINT64) _dx11Context4, (UINT64) _dx12Device, (UINT64) _dx12CommandQueue);
        return false;
    }

    HRESULT result = S_OK;

    const UINT copyAllocatorCount = std::max<UINT>(_bufferCount != 0 ? _bufferCount : 3, 3);

    if (_copyAllocators.size() != copyAllocatorCount)
    {
        for (auto& allocator : _copyAllocators)
            SafeRelease(allocator);

        _copyAllocators.assign(copyAllocatorCount, nullptr);
    }

    if (_copyAllocatorFenceValues.size() != copyAllocatorCount)
        _copyAllocatorFenceValues.assign(copyAllocatorCount, 0);

    if (_copyCommandLists.size() != copyAllocatorCount)
    {
        for (auto& commandList : _copyCommandLists)
            SafeRelease(commandList);

        _copyCommandLists.assign(copyAllocatorCount, nullptr);
    }

    for (UINT i = 0; i < copyAllocatorCount; ++i)
    {
        if (_copyAllocators[i] != nullptr)
            continue;

        result = _dx12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_copyAllocators[i]));
        if (FAILED(result))
        {
            LOG_ERROR("CreateCommandAllocator[{}] failed: {:X}", i, (UINT) result);
            return false;
        }
    }

    for (UINT i = 0; i < copyAllocatorCount; ++i)
    {
        if (_copyCommandLists[i] != nullptr)
            continue;

        result = _dx12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _copyAllocators[i], nullptr,
                                                IID_PPV_ARGS(&_copyCommandLists[i]));
        if (FAILED(result))
        {
            LOG_ERROR("CreateCommandList failed: {:X}", (UINT) result);
            return false;
        }

        _copyCommandLists[i]->SetName(L"D3D11 bridge copy");
        _copyCommandLists[i]->Close();
    }

    if (_copyFence == nullptr)
    {
        result = _dx12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_copyFence));
        if (FAILED(result))
        {
            LOG_ERROR("Create copy fence failed: {:X}", (UINT) result);
            return false;
        }
    }

    if (_copyFenceEvent == nullptr)
    {
        _copyFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (_copyFenceEvent == nullptr)
        {
            LOG_ERROR("CreateEvent for copy fence failed");
            return false;
        }
    }

    if (_dx11Fence == nullptr)
    {
        result = _dx11Device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&_dx11Fence));
        if (FAILED(result))
        {
            LOG_ERROR("D3D11 CreateFence failed: {:X}", (UINT) result);
            return false;
        }
    }

    if (_sharedFenceHandle == nullptr)
    {
        result = _dx11Fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &_sharedFenceHandle);
        if (FAILED(result))
        {
            LOG_ERROR("CreateSharedHandle for D3D11 fence failed: {:X}", (UINT) result);
            return false;
        }
    }

    if (_dx12SharedFence == nullptr)
    {
        result = _dx12Device->OpenSharedHandle(_sharedFenceHandle, IID_PPV_ARGS(&_dx12SharedFence));
        if (FAILED(result))
        {
            LOG_ERROR("OpenSharedHandle for D3D12 fence failed: {:X}", (UINT) result);
            return false;
        }
    }

    _RefreshCachedSwapchainDesc();

    if (_bufferCount == 0)
    {
        LOG_ERROR("InitInteropObjects resolved zero backbuffers");
        return false;
    }

    _sharedDx11BackBufferCopies.assign(_bufferCount, nullptr);
    _openedDx11BackBuffers.assign(_bufferCount, nullptr);
    _sharedBackBufferHandles.assign(_bufferCount, nullptr);
    _openedDx11BackBufferStates.assign(_bufferCount, D3D12_RESOURCE_STATE_COMMON);

    _interopInitialized = true;
    return true;
}

bool Dx11wDx12SC::_EnsureNativeInputCopyRing()
{
    // Lazy and separate from _InitInteropObjects: every D3D11+FG-interop user pays for that ring, but
    // this one is only worth creating -- and only worth the extra GPU objects and failure surface -- once
    // native input has actually produced a frame to composite.
    if (_dx12Device == nullptr || _bufferCount == 0)
        return false;

    const UINT copyAllocatorCount = std::max<UINT>(_bufferCount, 3);

    if (_nrCopyAllocators.size() != copyAllocatorCount)
    {
        for (auto& allocator : _nrCopyAllocators)
            SafeRelease(allocator);

        _nrCopyAllocators.assign(copyAllocatorCount, nullptr);
    }

    if (_nrCopyAllocatorFenceValues.size() != copyAllocatorCount)
        _nrCopyAllocatorFenceValues.assign(copyAllocatorCount, 0);

    if (_nrCopyCommandLists.size() != copyAllocatorCount)
    {
        for (auto& commandList : _nrCopyCommandLists)
            SafeRelease(commandList);

        _nrCopyCommandLists.assign(copyAllocatorCount, nullptr);
    }

    for (UINT i = 0; i < copyAllocatorCount; ++i)
    {
        if (_nrCopyAllocators[i] != nullptr)
            continue;

        auto result =
            _dx12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_nrCopyAllocators[i]));
        if (FAILED(result))
        {
            LOG_ERROR("CreateCommandAllocator (native input copy)[{}] failed: {:X}", i, (UINT) result);
            return false;
        }
    }

    for (UINT i = 0; i < copyAllocatorCount; ++i)
    {
        if (_nrCopyCommandLists[i] != nullptr)
            continue;

        auto result = _dx12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _nrCopyAllocators[i],
                                                      nullptr, IID_PPV_ARGS(&_nrCopyCommandLists[i]));
        if (FAILED(result))
        {
            LOG_ERROR("CreateCommandList (native input copy) failed: {:X}", (UINT) result);
            return false;
        }

        _nrCopyCommandLists[i]->SetName(L"D3D11 bridge native input copy");
        _nrCopyCommandLists[i]->Close();
    }

    return true;
}

bool Dx11wDx12SC::_RequestSharedBackBuffer(UINT index)
{
    if (_real == nullptr || _dx11Device == nullptr || _dx12Device == nullptr)
        return false;

    if (_bufferCount == 0)
        _RefreshCachedSwapchainDesc();

    if (index >= _bufferCount)
    {
        LOG_ERROR("backbuffer index {} out of range {}", index, _bufferCount);
        return false;
    }

    if (_sharedDx11BackBufferCopies.size() <= index)
        _sharedDx11BackBufferCopies.resize(_bufferCount, nullptr);

    if (_openedDx11BackBuffers.size() <= index)
        _openedDx11BackBuffers.resize(_bufferCount, nullptr);

    if (_sharedBackBufferHandles.size() <= index)
        _sharedBackBufferHandles.resize(_bufferCount, nullptr);

    if (_openedDx11BackBufferStates.size() <= index)
        _openedDx11BackBufferStates.resize(_bufferCount, D3D12_RESOURCE_STATE_COMMON);

    if (_openedDx11BackBuffers[_currentFakeIndex] != nullptr)
        return true;

    // Read Dx11 sc backbuffer
    ID3D11Texture2D* sourceTexture = nullptr;
    auto result = _real->GetBuffer(index, IID_PPV_ARGS(&sourceTexture));
    if (FAILED(result) || sourceTexture == nullptr)
    {
        LOG_ERROR("GetBuffer({}) failed: {:X}", index, (UINT) result);
        return false;
    }

    // Create shared copy
    IDXGIResource1* dxgiResource = nullptr;
    if (_sharedBackBufferHandles[_currentFakeIndex] == nullptr)
    {
        D3D11_TEXTURE2D_DESC desc = {};
        sourceTexture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

        // A game with MSAA on has a multisampled swap chain (_real keeps the samples it asked for); the D3D12 side cannot
        // open or present a multisampled texture, so the copy is single-sample and the samples are resolved into it
        // (_CopyDx11BackBufferToShared).
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;

        result = _dx11Device->CreateTexture2D(&desc, nullptr, &_sharedDx11BackBufferCopies[_currentFakeIndex]);
        if (FAILED(result) || _sharedDx11BackBufferCopies[_currentFakeIndex] == nullptr)
        {
            LOG_ERROR("CreateTexture2D shadow buffer {} failed: {:X}", _currentFakeIndex, (UINT) result);
            sourceTexture->Release();
            return false;
        }

        result = _sharedDx11BackBufferCopies[_currentFakeIndex]->QueryInterface(IID_PPV_ARGS(&dxgiResource));
        if (FAILED(result) || dxgiResource == nullptr)
        {
            LOG_ERROR("shadow IDXGIResource1 query {} failed: {:X}", _currentFakeIndex, (UINT) result);
            sourceTexture->Release();
            return false;
        }

        result = dxgiResource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr,
                                                  &_sharedBackBufferHandles[_currentFakeIndex]);
        dxgiResource->Release();

        if (FAILED(result) || _sharedBackBufferHandles[_currentFakeIndex] == nullptr)
        {
            LOG_ERROR("shadow CreateSharedHandle {} failed: {:X}", _currentFakeIndex, (UINT) result);
            sourceTexture->Release();
            return false;
        }
    }

    // Dx12 handle to Dx11 shared texture
    result = _dx12Device->OpenSharedHandle(_sharedBackBufferHandles[_currentFakeIndex],
                                           IID_PPV_ARGS(&_openedDx11BackBuffers[_currentFakeIndex]));
    sourceTexture->Release();

    if (FAILED(result) || _openedDx11BackBuffers[_currentFakeIndex] == nullptr)
    {
        // A device-removed result here never recovers by itself; log the actual reason once so it's diagnosable
        // without flooding the log on every subsequent frame (this failure repeats identically otherwise).
        if (!_deviceRemovedLogged)
        {
            _deviceRemovedLogged = true;
            LOG_ERROR("OpenSharedHandle for backbuffer {} failed: {:X}", _currentFakeIndex, (UINT) result);

            if (_dx12Device != nullptr)
                LOG_ERROR("dx12 device removed reason: {:X}", (UINT) _dx12Device->GetDeviceRemovedReason());

            if (_dx11Device != nullptr)
                LOG_ERROR("dx11 device removed reason: {:X}", (UINT) _dx11Device->GetDeviceRemovedReason());
        }

        return false;
    }

    _deviceRemovedLogged = false;

    _openedDx11BackBufferStates[_currentFakeIndex] = D3D12_RESOURCE_STATE_COMMON;
    return true;
}

bool Dx11wDx12SC::_CopyDx11BackBufferToShared(UINT index)
{
    if (_currentFakeIndex >= _sharedDx11BackBufferCopies.size() ||
        _sharedDx11BackBufferCopies[_currentFakeIndex] == nullptr)
    {
        return false;
    }

    if (_dx11Context == nullptr || _real == nullptr)
        return false;

    ID3D11Texture2D* sourceTexture = nullptr;
    auto result = _real->GetBuffer(index, IID_PPV_ARGS(&sourceTexture));
    if (FAILED(result) || sourceTexture == nullptr)
    {
        LOG_ERROR("GetBuffer for shadow copy {} failed: {:X}", index, (UINT) result);
        return false;
    }

    LOG_DEBUG("Copying DX11 backbuffer {} sourceTexture: {:X} to shadow copy {:X}", index, (size_t) sourceTexture,
              (size_t) _sharedDx11BackBufferCopies[_currentFakeIndex]);

    D3D11_TEXTURE2D_DESC sourceDesc {};
    sourceTexture->GetDesc(&sourceDesc);

    if (sourceDesc.SampleDesc.Count > 1)
    {
        // The back buffer is multisampled and the shared copy is not: a copy would be dropped by D3D11 without a word.
        ID3D11Texture2D* sharedCopy = _sharedDx11BackBufferCopies[_currentFakeIndex];

        if (const char* why = _msaaResolve.ResolveIn(_dx11Context, sourceTexture, sharedCopy))
        {
            LOG_ERROR("resolving the multisampled back buffer {} into the shadow copy failed: {}", index, why);
            sourceTexture->Release();
            return false;
        }
    }
    else
    {
        _dx11Context->CopyResource(_sharedDx11BackBufferCopies[_currentFakeIndex], sourceTexture);
    }

    sourceTexture->Release();
    return true;
}

bool Dx11wDx12SC::_WaitDx11ThenDx12()
{
    if (_dx11Context4 == nullptr || _dx11Fence == nullptr || _dx12CommandQueue == nullptr ||
        _dx12SharedFence == nullptr)
        return false;

    const auto waitValue = _sharedFenceValue++;

    auto result = _dx11Context4->Signal(_dx11Fence, waitValue);
    if (FAILED(result))
    {
        LOG_ERROR("DX11 Signal failed: {:X}", (UINT) result);
        return false;
    }

    _dx11Context4->Flush();

    // Important:
    // Do not block the FG/present queue on the D3D11 shared fence.
    // Isolate the cross-API wait on the interop copy queue.
    result = _dx12CommandQueue->Wait(_dx12SharedFence, waitValue);
    if (FAILED(result))
    {
        LOG_ERROR("interop copy queue Wait on D3D11 fence failed: {:X}", (UINT) result);
        return false;
    }

    return true;
}

bool Dx11wDx12SC::_WaitForCopyAllocator(UINT slot, std::vector<UINT64>& fenceValues)
{
    if (_copyFence == nullptr || _copyFenceEvent == nullptr)
        return true;

    if (slot >= fenceValues.size())
    {
        LOG_ERROR("copy allocator slot {} out of range {}", slot, fenceValues.size());
        return false;
    }

    const auto fenceValue = fenceValues[slot];

    if (fenceValue == 0)
        return true;

    const auto completedValue = _copyFence->GetCompletedValue();
    if (completedValue >= fenceValue)
        return true;

    auto result = _copyFence->SetEventOnCompletion(fenceValue, _copyFenceEvent);
    if (FAILED(result))
    {
        LOG_ERROR("copy allocator fence SetEventOnCompletion failed. slot {}, fence {}, completed {}, result {:X}",
                  slot, fenceValue, completedValue, (UINT) result);
        return false;
    }

    const auto waitResult = WaitForSingleObject(_copyFenceEvent, 5000);
    if (waitResult != WAIT_OBJECT_0)
    {
        LOG_ERROR("copy allocator fence wait failed. slot {}, fence {}, completed {}, waitResult {:X}", slot,
                  fenceValue, _copyFence->GetCompletedValue(), waitResult);
        return false;
    }

    return true;
}

bool Dx11wDx12SC::_CopyDx11SharedToDx12FGBackBuffer(UINT dx11Index)
{
    if (_copyAllocators.empty() || _copyCommandLists.empty() || _dx12CommandQueue == nullptr || _copyFence == nullptr ||
        _fgSwapChain == nullptr || _currentFakeIndex >= _openedDx11BackBuffers.size() ||
        _openedDx11BackBuffers[_currentFakeIndex] == nullptr)
    {
        return false;
    }

    const UINT copySlot = _currentFakeIndex;
    auto allocator = _copyAllocators[copySlot];

    if (allocator == nullptr)
        return false;

    if (!_WaitForCopyAllocator(copySlot, _copyAllocatorFenceValues))
        return false;

    auto result = allocator->Reset();
    if (FAILED(result))
    {
        LOG_ERROR("copy allocator[{}] reset failed: {:X}", copySlot, (UINT) result);
        return false;
    }

    result = _copyCommandLists[copySlot]->Reset(allocator, nullptr);
    if (FAILED(result))
    {
        LOG_ERROR("copy command list reset failed: {:X}", (UINT) result);
        return false;
    }

    UINT fgIndex = _fgSwapChain->GetCurrentBackBufferIndex();
    LOG_DEBUG("dx11Index {}, fgIndex {}", dx11Index, fgIndex);

    ID3D12Resource* fgBackBuffer = nullptr;
    result = _fgSwapChain->GetBuffer(fgIndex, IID_PPV_ARGS(&fgBackBuffer));
    if (FAILED(result) || fgBackBuffer == nullptr)
    {
        LOG_ERROR("FG GetBuffer({}) failed: {:X}", fgIndex, (UINT) result);
        _copyCommandLists[copySlot]->Close();
        return false;
    }

    auto sourceBefore = _openedDx11BackBufferStates[copySlot];

    TransitionResource(_copyCommandLists[copySlot], _openedDx11BackBuffers[copySlot], sourceBefore,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
    TransitionResource(_copyCommandLists[copySlot], fgBackBuffer, D3D12_RESOURCE_STATE_PRESENT,
                       D3D12_RESOURCE_STATE_COPY_DEST);

    _copyCommandLists[copySlot]->CopyResource(fgBackBuffer, _openedDx11BackBuffers[copySlot]);

    TransitionResource(_copyCommandLists[copySlot], fgBackBuffer, D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_PRESENT);
    TransitionResource(_copyCommandLists[copySlot], _openedDx11BackBuffers[copySlot], D3D12_RESOURCE_STATE_COPY_SOURCE,
                       sourceBefore);

    _openedDx11BackBufferStates[copySlot] = D3D12_RESOURCE_STATE_COMMON;

    fgBackBuffer->Release();

    result = _copyCommandLists[copySlot]->Close();
    if (FAILED(result))
    {
        LOG_ERROR("copy command list close failed: {:X}", (UINT) result);
        return false;
    }

    ID3D12CommandList* lists[] = { _copyCommandLists[copySlot] };
    _dx12CommandQueue->ExecuteCommandLists(1, lists);

    const auto signalValue = ++_copyFenceValue;

    result = _dx12CommandQueue->Signal(_copyFence, signalValue);
    if (FAILED(result))
    {
        LOG_ERROR("interop copy fence signal failed: {:X}", (UINT) result);
        return false;
    }

    _copyAllocatorFenceValues[copySlot] = signalValue;
    _lastInteropCopyFenceValue = signalValue;

    return true;
}

void Dx11wDx12SC::_ApplyNativeInputToFGBackBuffer()
{
    // Runs the D3D11 depth finder's frame close and the native input producer (NativeMotionDx11::OnFGPresent); null
    // when the option is off, the game is calling its own upscaler, or nothing was ready this frame -- all of those
    // are the ordinary case, not a failure.
    ID3D12Resource* processed = NativeMotionDx11::OnFGPresent(_real, _dx11Device);

    if (processed == nullptr)
        return;

    if (_nrCopyAllocators.empty() && !_EnsureNativeInputCopyRing())
    {
        LOG_WARN("native input ready but its copy ring could not be created: dropping this frame's result");
        return;
    }

    if (_nrCopyAllocators.empty() || _nrCopyCommandLists.empty() || _dx12CommandQueue == nullptr ||
        _copyFence == nullptr || _fgSwapChain == nullptr || _currentFakeIndex >= _nrCopyAllocators.size())
    {
        LOG_WARN("native input ready but the FG copy ring is not: dropping this frame's result");
        return;
    }

    const UINT slot = _currentFakeIndex;
    auto allocator = _nrCopyAllocators[slot];

    if (allocator == nullptr || !_WaitForCopyAllocator(slot, _nrCopyAllocatorFenceValues))
        return;

    auto result = allocator->Reset();
    if (FAILED(result))
    {
        LOG_ERROR("native input copy allocator[{}] reset failed: {:X}", slot, (UINT) result);
        return;
    }

    auto* list = _nrCopyCommandLists[slot];

    result = list->Reset(allocator, nullptr);
    if (FAILED(result))
    {
        LOG_ERROR("native input copy command list reset failed: {:X}", (UINT) result);
        return;
    }

    UINT fgIndex = _fgSwapChain->GetCurrentBackBufferIndex();
    ID3D12Resource* fgBackBuffer = nullptr;
    result = _fgSwapChain->GetBuffer(fgIndex, IID_PPV_ARGS(&fgBackBuffer));
    if (FAILED(result) || fgBackBuffer == nullptr)
    {
        LOG_ERROR("native input copy FG GetBuffer({}) failed: {:X}", fgIndex, (UINT) result);
        list->Close();
        return;
    }

    // `processed` rests in COMMON (native::NativeProducer's own contract); fgBackBuffer was already put back in
    // PRESENT by _CopyDx11SharedToDx12FGBackBuffer just before this runs.
    TransitionResource(list, processed, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    TransitionResource(list, fgBackBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);

    list->CopyResource(fgBackBuffer, processed);

    TransitionResource(list, fgBackBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    TransitionResource(list, processed, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);

    fgBackBuffer->Release();

    result = list->Close();
    if (FAILED(result))
    {
        LOG_ERROR("native input copy command list close failed: {:X}", (UINT) result);
        return;
    }

    ID3D12CommandList* lists[] = { list };
    _dx12CommandQueue->ExecuteCommandLists(1, lists);

    const auto signalValue = ++_copyFenceValue;

    result = _dx12CommandQueue->Signal(_copyFence, signalValue);
    if (FAILED(result))
    {
        LOG_ERROR("native input copy fence signal failed: {:X}", (UINT) result);
        return;
    }

    _nrCopyAllocatorFenceValues[slot] = signalValue;
    _lastInteropCopyFenceValue = signalValue;
}

bool Dx11wDx12SC::_WaitForInteropCopyOnPresentQueue()
{
    if (_copyFence == nullptr)
        return false;

    if (_lastInteropCopyFenceValue == 0)
        return true;

    // The interop copies are submitted on _dx12CommandQueue. Only an FG backend presenting from a different queue
    // needs a cross-queue wait; with no backend queue (no FG feature, or the backend's own swapchain creation failed
    // and DxgiFactoryHooks fell back to a plain DX12 swapchain on _dx12CommandQueue -- seen with XeFG in Prey, where
    // GetCommandQueue() then stays null for the whole session) the present is already ordered after the copy on
    // the same queue. Failing here instead turned every Present into DXGI_ERROR_DEVICE_REMOVED for the game.
    auto* queue = _fg != nullptr ? _fg->GetCommandQueue() : nullptr;
    if (queue == nullptr || queue == _dx12CommandQueue)
        return true;

    auto result = queue->Wait(_copyFence, _lastInteropCopyFenceValue);
    if (FAILED(result))
    {
        LOG_ERROR("present queue Wait on interop copy fence failed: {:X}", (UINT) result);
        return false;
    }

    return true;
}

bool Dx11wDx12SC::_WaitForCopyQueueIdle()
{
    if (_copyFence == nullptr || _copyFenceEvent == nullptr)
        return true;

    UINT64 waitValue = _lastInteropCopyFenceValue;

    for (const auto fenceValue : _copyAllocatorFenceValues)
        waitValue = std::max(waitValue, fenceValue);

    for (const auto fenceValue : _nrCopyAllocatorFenceValues)
        waitValue = std::max(waitValue, fenceValue);

    if (waitValue == 0)
        return true;

    const auto completedValue = _copyFence->GetCompletedValue();
    if (completedValue >= waitValue)
        return true;

    auto result = _copyFence->SetEventOnCompletion(waitValue, _copyFenceEvent);
    if (FAILED(result))
    {
        LOG_ERROR("copy queue idle SetEventOnCompletion failed. fence {}, completed {}, result {:X}", waitValue,
                  completedValue, (UINT) result);
        return false;
    }

    const auto waitResult = WaitForSingleObject(_copyFenceEvent, 5000);
    if (waitResult != WAIT_OBJECT_0)
    {
        LOG_ERROR("copy queue idle wait failed. fence {}, completed {}, waitResult {:X}", waitValue,
                  _copyFence->GetCompletedValue(), waitResult);
        return false;
    }

    for (auto& fenceValue : _copyAllocatorFenceValues)
        fenceValue = 0;

    for (auto& fenceValue : _nrCopyAllocatorFenceValues)
        fenceValue = 0;

    _lastInteropCopyFenceValue = 0;

    return true;
}

void Dx11wDx12SC::_ReleaseInteropBackBuffers()
{
    _interopInitialized = false;

    for (auto& resource : _openedDx11BackBuffers)
        SafeRelease(resource);

    for (auto& texture : _sharedDx11BackBufferCopies)
        SafeRelease(texture);

    for (auto& handle : _sharedBackBufferHandles)
        SafeCloseHandle(handle);

    _openedDx11BackBufferStates.clear();
    _openedDx11BackBuffers.clear();
    _sharedDx11BackBufferCopies.clear();
    _sharedBackBufferHandles.clear();
}

void Dx11wDx12SC::_ReleaseInteropObjects()
{
    _WaitForCopyQueueIdle();
    _ReleaseInteropBackBuffers();

    _lastInteropCopyFenceValue = 0;

    SafeRelease(_dx12SharedFence);
    SafeRelease(_dx11Fence);
    SafeCloseHandle(_dx11FenceEvent);
    SafeCloseHandle(_sharedFenceHandle);

    for (auto& cmdList : _copyCommandLists)
        SafeRelease(cmdList);

    _copyCommandLists.clear();

    for (auto& allocator : _copyAllocators)
        SafeRelease(allocator);

    _copyAllocators.clear();
    _copyAllocatorFenceValues.clear();

    for (auto& cmdList : _nrCopyCommandLists)
        SafeRelease(cmdList);

    _nrCopyCommandLists.clear();

    for (auto& allocator : _nrCopyAllocators)
        SafeRelease(allocator);

    _nrCopyAllocators.clear();
    _nrCopyAllocatorFenceValues.clear();

    SafeRelease(_copyFence);
    SafeCloseHandle(_copyFenceEvent);
    _copyFenceValue = 1;

    _interopInitialized = false;
}

void Dx11wDx12SC::_RefreshCachedSwapchainDesc()
{
    _bufferCount = ResolveBufferCount(_real, _real1);
    _bufferFormat = ResolveBufferFormat(_real, _real1);
    _currentFakeIndex = _bufferCount > 0 ? _currentFakeIndex : 0;
}

UINT Dx11wDx12SC::_GetDx11BackBufferIndexForPresent() const
{
    if (_real3 != nullptr)
        return _real3->GetCurrentBackBufferIndex();

    return _bufferCount > 0 ? _currentFakeIndex : 0;
}

void Dx11wDx12SC::_AdvanceFakeBackBufferIndex() { _currentFakeIndex = (_currentFakeIndex + 1) % _bufferCount; }
