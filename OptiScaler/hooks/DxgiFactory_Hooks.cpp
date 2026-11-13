#include "pch.h"
#include "DxgiFactory_Hooks.h"
#include "SwapchainCreateRule.h"
#include "DxgiSwapchainSizing.h"
#include "DxvkOutputs.h"

#include "D3D11_Hooks.h"
#include "D3D12_Hooks.h"

#include <Config.h>

#include <misc/IdentifyGpu.h>
#include <spoofing/Dxgi_Spoofing.h>

#include <misc/HiddenWindow.h>
#include <native/VkPresentBridge.h>
#include <output/SdrToHdr_Create.h>
#include <with_dx12/with_dx12.h>
#include <wrapped/wrapped_swapchain.h>
#include <with_dx12/dx11_with_dx12_sc.h>

#include <d3d11.h>
#include <magic_enum.hpp>
#include <detours/detours.h>

// #define DETAILED_SC_LOGS

#ifdef DETAILED_SC_LOGS
#include <magic_enum.hpp>
#endif

// NativeDxvkVulkan: a dxvk game's frame generation is the Vulkan present bridge (native/VkPresentBridge.h) on dxvk's own
// Vulkan swapchain, not the D3D11 bridge (Dx11wDx12SC). That one cannot work on dxvk: dxvk's shared fence and texture
// handles do not open on a native D3D12 device (OpenSharedHandle E_HANDLE, then a crash in Batman: Arkham Knight).
static bool DxvkFrameGenerationViaVulkan() { return VkPresentBridge::DxvkThroughVulkan(); }

// A plain (wrapped) game swapchain has no interop. Except a dxvk game's D3D11 one while the Vulkan present bridge carries
// its frames: the bridge's VkwDx12 stays (a dxvk that makes its Vulkan swapchain inside CreateSwapChain sets it first).
static void SetPlainSwapchainInterop()
{
    if (VkPresentBridge::DxvkThroughVulkan() && VkPresentBridge::IsUp())
        return;

    State::Instance().swapchainInteropApi = SwapchainInteropApi::None;
}

static bool IsTearingSupported(IDXGIFactory* factory)
{
    if (factory == nullptr)
        return false;

    IDXGIFactory5* factory5 = nullptr;

    if (FAILED(factory->QueryInterface(IID_PPV_ARGS(&factory5))))
        return false;

    BOOL supported = FALSE;

    const HRESULT hr = factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &supported, sizeof(supported));

    factory5->Release();

    return SUCCEEDED(hr) && supported == TRUE;
}

static bool PrepareDx12FlipFormat(DXGI_FORMAT& format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        LOG_WARN("Dx11wDx12 converting R8G8B8A8_UNORM_SRGB to "
                 "R8G8B8A8_UNORM for DX12 flip swapchain");
        format = DXGI_FORMAT_R8G8B8A8_UNORM;
        return true;

    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        LOG_WARN("Dx11wDx12 converting B8G8R8A8_UNORM_SRGB to "
                 "B8G8R8A8_UNORM for DX12 flip swapchain");
        format = DXGI_FORMAT_B8G8R8A8_UNORM;
        return true;

    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return true;

    default:
        LOG_ERROR("Unsupported texture format for DX12 flip swapchain: {}", (UINT) format);
        return false;
    }
}

static bool PrepareDx12InteropDesc(DXGI_SWAP_CHAIN_DESC& desc, bool tearingSupported)
{
    // D3D12 swapchain backbuffers cannot be multisampled: this desc is the D3D12 side's, forced to one sample below. The
    // game's own D3D11 swap chain keeps the samples it asked for, and the bridge resolves them into single-sample copies.
    if (desc.SampleDesc.Count > 1)
    {
        LOG_INFO("Dx11wDx12 interop: the game's swapchain is multisampled ({} samples); the D3D12 side is single-sample",
                 desc.SampleDesc.Count);
    }

    // Flip-model swapchains support a limited set of formats.
    if (!PrepareDx12FlipFormat(desc.BufferDesc.Format))
    {
        LOG_WARN("Dx11wDx12 interop unsupported flip-model format: {}", (UINT) desc.BufferDesc.Format);
        return false;
    }

    // Flip-model requires 2-16 buffers.
    if (desc.BufferCount < 2)
        desc.BufferCount = 2;

    if (desc.BufferCount > 16)
    {
        LOG_WARN("Dx11wDx12 interop invalid BufferCount: {}", desc.BufferCount);
        return false;
    }

    // D3D12 supports flip-model swap effects only.
    switch (desc.SwapEffect)
    {
    case DXGI_SWAP_EFFECT_DISCARD:
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        break;

    case DXGI_SWAP_EFFECT_SEQUENTIAL:
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        break;

    case DXGI_SWAP_EFFECT_FLIP_DISCARD:
    case DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL:
        break;

    default:
        LOG_WARN("Dx11wDx12 interop unsupported SwapEffect: {}", (UINT) desc.SwapEffect);
        return false;
    }

    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;

    // D3D12 swapchain backbuffers cannot expose UAV usage.
    if (desc.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS)
    {
        LOG_DEBUG("Dx11wDx12 removing DXGI_USAGE_UNORDERED_ACCESS from DX12 swapchain");
        desc.BufferUsage &= ~DXGI_USAGE_UNORDERED_ACCESS;
    }

    // GDI-compatible swapchains are not applicable to the D3D12 interop path.
    if (desc.Flags & DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE)
    {
        LOG_DEBUG("Dx11wDx12 removing DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE");
        desc.Flags &= ~DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE;
    }

    // Keep the game's tearing intent when the system supports it.
    if (!tearingSupported && (desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING))
    {
        LOG_DEBUG("Dx11wDx12 removing unsupported DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING");
        desc.Flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    LOG_DEBUG("Dx11wDx12 DX12 desc: {}x{}, Format: {}, Count: {}, "
              "Sample: {}/{}, Usage: {:X}, SwapEffect: {}, Flags: {:X}, "
              "Windowed: {}, Refresh: {}/{}, Scaling: {}, Scanline: {}",
              desc.BufferDesc.Width, desc.BufferDesc.Height, (UINT) desc.BufferDesc.Format, desc.BufferCount,
              desc.SampleDesc.Count, desc.SampleDesc.Quality, desc.BufferUsage, (UINT) desc.SwapEffect, desc.Flags,
              desc.Windowed, desc.BufferDesc.RefreshRate.Numerator, desc.BufferDesc.RefreshRate.Denominator,
              (UINT) desc.BufferDesc.Scaling, (UINT) desc.BufferDesc.ScanlineOrdering);

    return true;
}

static bool PrepareDx12InteropDesc1(DXGI_SWAP_CHAIN_DESC1& desc, bool tearingSupported)
{
    // D3D12 swapchain backbuffers cannot be multisampled: this desc is the D3D12 side's, forced to one sample below (see the
    // other overload).
    if (desc.SampleDesc.Count > 1)
    {
        LOG_INFO("Dx11wDx12 interop: the game's swapchain is multisampled ({} samples); the D3D12 side is single-sample",
                 desc.SampleDesc.Count);
    }

    if (!PrepareDx12FlipFormat(desc.Format))
    {
        LOG_WARN("Dx11wDx12 interop unsupported flip-model format: {}", (UINT) desc.Format);
        return false;
    }

    if (desc.BufferCount < 2)
        desc.BufferCount = 2;

    if (desc.BufferCount > 16)
    {
        LOG_WARN("Dx11wDx12 interop invalid BufferCount: {}", desc.BufferCount);
        return false;
    }

    // Current interop wrapper does not explicitly handle stereo swapchains.
    if (desc.Stereo)
    {
        LOG_WARN("Dx11wDx12 interop does not support stereo swapchains!");
        return false;
    }

    switch (desc.SwapEffect)
    {
    case DXGI_SWAP_EFFECT_DISCARD:
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        break;

    case DXGI_SWAP_EFFECT_SEQUENTIAL:
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        break;

    case DXGI_SWAP_EFFECT_FLIP_DISCARD:
    case DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL:
        break;

    default:
        LOG_WARN("Dx11wDx12 interop unsupported SwapEffect: {}", (UINT) desc.SwapEffect);
        return false;
    }

    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;

    if (desc.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS)
    {
        LOG_DEBUG("Dx11wDx12 removing DXGI_USAGE_UNORDERED_ACCESS from DX12 swapchain");
        desc.BufferUsage &= ~DXGI_USAGE_UNORDERED_ACCESS;
    }

    if (desc.Flags & DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE)
    {
        LOG_DEBUG("Dx11wDx12 removing DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE");
        desc.Flags &= ~DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE;
    }

    if (!tearingSupported && (desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING))
    {
        LOG_DEBUG("Dx11wDx12 removing unsupported DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING");
        desc.Flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    LOG_DEBUG("Dx11wDx12 DX12 desc1: {}x{}, Format: {}, Count: {}, "
              "Sample: {}/{}, Usage: {:X}, SwapEffect: {}, Flags: {:X}, "
              "Scaling: {}, AlphaMode: {}, Stereo: {}",
              desc.Width, desc.Height, (UINT) desc.Format, desc.BufferCount, desc.SampleDesc.Count,
              desc.SampleDesc.Quality, desc.BufferUsage, (UINT) desc.SwapEffect, desc.Flags, (UINT) desc.Scaling,
              (UINT) desc.AlphaMode, desc.Stereo);

    return true;
}

void DxgiFactoryHooks::HookToFactory(IDXGIFactory* pFactory)
{
    if (pFactory == nullptr || o_EnumAdapters != nullptr)
        return;

    LOG_FUNC();

    IDXGIFactory* real = nullptr;
    if (!Util::CheckForRealObject(__FUNCTION__, pFactory, (IUnknown**) &real))
        real = pFactory;

    void** pFactoryVTable = *reinterpret_cast<void***>(real);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_EnumAdapters == nullptr)
    {
        o_EnumAdapters = (PFN_EnumAdapters) pFactoryVTable[7];

        if (o_EnumAdapters != nullptr)
            DetourAttach(&(PVOID&) o_EnumAdapters, DxgiFactoryHooks::EnumAdapters);
    }

    if (o_CreateSwapChain == nullptr)
    {
        o_CreateSwapChain = (PFN_CreateSwapChain) pFactoryVTable[10];

        if (o_CreateSwapChain != nullptr)
            DetourAttach(&(PVOID&) o_CreateSwapChain, DxgiFactoryHooks::CreateSwapChain);
    }

    IDXGIFactory1* factory1 = nullptr;
    if (pFactory->QueryInterface(IID_PPV_ARGS(&factory1)) == S_OK)
    {
        factory1->Release();

        if (o_EnumAdapters1 == nullptr)
        {
            o_EnumAdapters1 = (PFN_EnumAdapters1) pFactoryVTable[12];

            if (o_EnumAdapters1 != nullptr)
                DetourAttach(&(PVOID&) o_EnumAdapters1, DxgiFactoryHooks::EnumAdapters1);
        }
    }

    IDXGIFactory2* factory2 = nullptr;
    if (pFactory->QueryInterface(IID_PPV_ARGS(&factory2)) == S_OK)
    {
        void** factory2VTable = *reinterpret_cast<void***>(factory2);
        factory2->Release();

        if (o_CreateSwapChainForHwnd == nullptr)
        {
            o_CreateSwapChainForHwnd = (PFN_CreateSwapChainForHwnd) pFactoryVTable[15];

            if (o_CreateSwapChainForHwnd != nullptr)
                DetourAttach(&(PVOID&) o_CreateSwapChainForHwnd, DxgiFactoryHooks::CreateSwapChainForHwnd);
        }

        if (o_CreateSwapChainForCoreWindow == nullptr)
        {
            o_CreateSwapChainForCoreWindow = (PFN_CreateSwapChainForCoreWindow) pFactoryVTable[16];

            if (o_CreateSwapChainForCoreWindow != nullptr)
                DetourAttach(&(PVOID&) o_CreateSwapChainForCoreWindow, DxgiFactoryHooks::CreateSwapChainForCoreWindow);
        }

        if (o_CreateSwapChainForComposition == nullptr)
        {
            o_CreateSwapChainForComposition = (PFN_CreateSwapChainForComposition) factory2VTable[24];
            if (o_CreateSwapChainForComposition != nullptr)
                DetourAttach(&(PVOID&) o_CreateSwapChainForComposition, DxgiFactoryHooks::CreateSwapChainForComposition);
        }
    }

    IDXGIFactory4* factory4 = nullptr;
    if (pFactory->QueryInterface(IID_PPV_ARGS(&factory4)) == S_OK)
    {
        factory4->Release();

        if (o_EnumAdapterByLuid == nullptr)
        {
            o_EnumAdapterByLuid = (PFN_EnumAdapterByLuid) pFactoryVTable[26];

            if (o_EnumAdapterByLuid != nullptr)
                DetourAttach(&(PVOID&) o_EnumAdapterByLuid, DxgiFactoryHooks::EnumAdapterByLuid);
        }
    }

    IDXGIFactory6* factory6 = nullptr;
    if (pFactory->QueryInterface(IID_PPV_ARGS(&factory6)) == S_OK)
    {
        factory6->Release();

        if (o_EnumAdapterByGpuPreference == nullptr)
        {
            o_EnumAdapterByGpuPreference = (PFN_EnumAdapterByGpuPreference) pFactoryVTable[29];

            if (o_EnumAdapterByGpuPreference != nullptr)
                DetourAttach(&(PVOID&) o_EnumAdapterByGpuPreference, DxgiFactoryHooks::EnumAdapterByGpuPreference);
        }
    }

    auto detourResult = DetourTransactionCommit();
    if (detourResult != NO_ERROR)
    {
        LOG_ERROR("Failed to hook IDXGIFactory: {:X}", detourResult);
        o_EnumAdapters = nullptr;
        o_CreateSwapChain = nullptr;
        o_EnumAdapters1 = nullptr;
        o_CreateSwapChainForHwnd = nullptr;
        o_CreateSwapChainForCoreWindow = nullptr;
        o_CreateSwapChainForComposition = nullptr;
        o_EnumAdapterByLuid = nullptr;
        o_EnumAdapterByGpuPreference = nullptr;
    }

    // dxvk's factory: its adapters list the outputs in monitor order, not with the primary display first as Windows'
    // DXGI does (hooks/DxvkOutputs.h); put right before the game enumerates them.
    IDXGIVkInteropFactory* dxvkFactory = nullptr;

    if (o_EnumAdapters != nullptr &&
        SUCCEEDED(real->QueryInterface(__uuidof(IDXGIVkInteropFactory), (void**) &dxvkFactory)) && dxvkFactory != nullptr)
    {
        dxvkFactory->Release();
        IDXGIAdapter* adapter = nullptr;

        if (o_EnumAdapters(real, 0, &adapter) == S_OK && adapter != nullptr)
        {
            DxvkOutputs::Hook(adapter);
            adapter->Release();
        }
    }
}

void DxgiFactoryHooks::NoteCreateStatus(HRESULT result, IDXGISwapChain* swapChain)
{
    if (result == S_OK || swapChain == nullptr)
        return;

    BOOL fullscreen = FALSE;

    if (FAILED(swapChain->GetFullscreenState(&fullscreen, nullptr)))
        fullscreen = FALSE;

    State::Instance().realExclusiveFullscreen = fullscreen != FALSE;

    LOG_INFO("Swapchain made with status {:X}, exclusive full screen: {}", (UINT) result, fullscreen != FALSE);
}

HRESULT DxgiFactoryHooks::CreateDx12BridgeSwapChain(IDXGIFactory* factory, ID3D12CommandQueue* queue,
                                                    DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain4** out, bool* realFG,
                                                    bool systemFactory)
{
    *out = nullptr;
    *realFG = false;

    if (factory == nullptr || queue == nullptr || desc == nullptr)
        return E_INVALIDARG;

    // A Vulkan game never made a DXGI factory of its own: frame generation's swapchain is made through this hooked one.
    // Not Windows' own beside dxvk's: the hooks' originals (o_*) are dxvk's methods, which must not be called on it.
    if (!systemFactory)
        HookToFactory(factory);

    if (!PrepareDx12InteropDesc(*desc, IsTearingSupported(factory)))
        return E_INVALIDARG;

    IDXGISwapChain* swapChain = nullptr;
    HRESULT result = E_FAIL;

    // HDR output is not available for a Vulkan game: the library's chain made here stands aside
    SdrToHdr::ScopedVulkanBridgeCreate vulkanBridge {};
    FGHooks::ScopedBridgeCreate bridgeCreate {};

    {
        ScopedSkipFGSCCreation skipFGSCCreation {};
        result = FGHooks::CreateSwapChain(factory, queue, desc, &swapChain);
        *realFG = SUCCEEDED(result) && swapChain != nullptr;
    }

    if (FAILED(result) || swapChain == nullptr)
    {
        *realFG = false;
        LOG_WARN("Vulkan bridge FG swapchain creation failed: {:X}; creating plain DX12 swapchain", (UINT) result);

        ScopedSkipParentWrapping skipParentWrapping {};
        result = systemFactory ? factory->CreateSwapChain(queue, desc, &swapChain)
                               : o_CreateSwapChain(factory, queue, desc, &swapChain);
    }

    if (SUCCEEDED(result) && swapChain != nullptr)
    {
        result = swapChain->QueryInterface(IID_PPV_ARGS(out));
        swapChain->Release();
    }

    // The game owns full screen (Vulkan, or dxvk's own DXGI): DXGI's Alt+Enter on the window must not put this swapchain
    // into exclusive full screen behind its back. The bridge lets the swapchain go on every remake, and releasing one in
    // exclusive full screen raises DXGI_ERROR_INVALID_CALL (Batman: Arkham Knight on dxvk, Alt+Enter).
    if (SUCCEEDED(result) && *out != nullptr && desc->OutputWindow != nullptr)
        factory->MakeWindowAssociation(desc->OutputWindow, DXGI_MWA_NO_ALT_ENTER);

    return result;
}

void DxgiFactoryHooks::HookToDLSSGFactory(IDXGIFactory* pFactory)
{
    if (pFactory == nullptr || o_DLSSGCreateSwapChain != nullptr)
        return;

    IDXGIFactory* real = nullptr;
    if (!Util::CheckForRealObject(__FUNCTION__, pFactory, (IUnknown**) &real))
        return;

    real->Release();

    LOG_FUNC();

    void** pFactoryVTable = *reinterpret_cast<void***>(pFactory);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_DLSSGCreateSwapChain == nullptr)
    {
        o_DLSSGCreateSwapChain = (PFN_CreateSwapChain) pFactoryVTable[10];

        if (o_DLSSGCreateSwapChain != nullptr)
            DetourAttach(&(PVOID&) o_DLSSGCreateSwapChain, DxgiFactoryHooks::DLSSGCreateSwapChain);
    }

    IDXGIFactory2* factory2 = nullptr;
    if (pFactory->QueryInterface(IID_PPV_ARGS(&factory2)) == S_OK)
    {
        factory2->Release();

        if (o_DLSSGCreateSwapChainForHwnd == nullptr)
        {
            o_DLSSGCreateSwapChainForHwnd = (PFN_CreateSwapChainForHwnd) pFactoryVTable[15];

            if (o_DLSSGCreateSwapChainForHwnd != nullptr)
                DetourAttach(&(PVOID&) o_DLSSGCreateSwapChainForHwnd, DxgiFactoryHooks::DLSSGCreateSwapChainForHwnd);
        }

        if (o_DLSSGCreateSwapChainForCoreWindow == nullptr)
        {
            o_DLSSGCreateSwapChainForCoreWindow = (PFN_CreateSwapChainForCoreWindow) pFactoryVTable[16];

            if (o_DLSSGCreateSwapChainForCoreWindow != nullptr)
                DetourAttach(&(PVOID&) o_DLSSGCreateSwapChainForCoreWindow,
                             DxgiFactoryHooks::DLSSGCreateSwapChainForCoreWindow);
        }
    }

    DetourTransactionCommit();
}

HRESULT DxgiFactoryHooks::CreateSwapChain(IDXGIFactory* realFactory, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc,
                                          IDXGISwapChain** ppSwapChain)
{
    *ppSwapChain = nullptr;

    if (State::Instance().vulkanCreatingSC)
    {
        LOG_WARN("Vulkan is creating swapchain!");

        if (pDesc != nullptr)
            LOG_DEBUG("Width: {}, Height: {}, Format: {}, Count: {}, Hwnd: {:X}, Windowed: {}, SkipWrapping: {}",
                      pDesc->BufferDesc.Width, pDesc->BufferDesc.Height, (UINT) pDesc->BufferDesc.Format,
                      pDesc->BufferCount, (SIZE_T) pDesc->OutputWindow, pDesc->Windowed, _skipFGSwapChainCreation);

        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        ScopedSkipParentWrapping skipParentWrapping {};

        auto res = o_CreateSwapChain(realFactory, pDevice, pDesc, ppSwapChain);
        return res;
    }

    if (pDevice == nullptr || pDesc == nullptr)
    {
        LOG_WARN("pDevice or pDesc is nullptr!");

        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        ScopedSkipParentWrapping skipParentWrapping {};

        auto res = o_CreateSwapChain(realFactory, pDevice, pDesc, ppSwapChain);
        return res;
    }

    DXGI_SWAP_CHAIN_DESC localDesc = *pDesc;
    const bool sizeToWindow = ResolveWindowSizedSwapchain(localDesc);

    if (localDesc.BufferDesc.Height < 100 || localDesc.BufferDesc.Width < 100)
    {
        LOG_WARN("Overlay call! Width: {}, Height: {}, Format: {}, Count: {}, Hwnd: {:X}, Windowed: {}",
                 pDesc->BufferDesc.Width, pDesc->BufferDesc.Height, (UINT) pDesc->BufferDesc.Format,
                 pDesc->BufferCount, (SIZE_T) pDesc->OutputWindow, pDesc->Windowed);

        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        ScopedSkipParentWrapping skipParentWrapping {};

        auto res = o_CreateSwapChain(realFactory, pDevice, pDesc, ppSwapChain);
        return res;
    }

    if (sizeToWindow)
        LOG_INFO("CreateSwapChain: 0x0 size-to-window descriptor, Hwnd: {:X}, Count: {}, wrapping normally",
                 (SIZE_T) pDesc->OutputWindow, pDesc->BufferCount);

    LOG_DEBUG("Width: {}, Height: {}, Format: {}, Count: {}, Flags: {:X}, Hwnd: {:X}, Windowed: {}, SkipWrapping: {}",
              localDesc.BufferDesc.Width, localDesc.BufferDesc.Height, (UINT) localDesc.BufferDesc.Format,
              localDesc.BufferCount, localDesc.Flags, (SIZE_T) localDesc.OutputWindow, localDesc.Windowed,
              _skipFGSwapChainCreation);

    if (UsesXeFGApi(State::Instance().activeFgOutput) && FGHooks::BorderlessForced())
    {
        if (!localDesc.Windowed)
        {
            State::Instance().SCExclusiveFullscreen = true;
            localDesc.Windowed = true;
        }

        localDesc.Flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        localDesc.BufferDesc.Scaling = DXGI_MODE_SCALING_STRETCHED;
    }

    // For vsync override
    if (!localDesc.Windowed)
    {
        LOG_INFO("Game is creating fullscreen swapchain, disabled V-Sync overrides");
        Config::Instance()->OverrideVsync.set_volatile_value(false);
    }

    if (Config::Instance()->OverrideVsync.value_or_default())
    {
        localDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        localDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

        if (localDesc.BufferCount < 2)
            localDesc.BufferCount = 2;
    }

    State::Instance().SCAllowTearing = (localDesc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) > 0;
    State::Instance().SCLastFlags = localDesc.Flags;
    State::Instance().realExclusiveFullscreen = !localDesc.Windowed;

#ifdef DETAILED_SC_LOGS
    LOG_TRACE("localDesc.BufferCount: {}", localDesc.BufferCount);
    LOG_TRACE("localDesc.BufferDesc.Format: {}", magic_enum::enum_name(localDesc.BufferDesc.Format));
    LOG_TRACE("localDesc.BufferDesc.Height: {}", localDesc.BufferDesc.Height);
    LOG_TRACE("localDesc.BufferDesc.RefreshRate.Denominator: {}", localDesc.BufferDesc.RefreshRate.Denominator);
    LOG_TRACE("localDesc.BufferDesc.RefreshRate.Numerator: {}", localDesc.BufferDesc.RefreshRate.Numerator);
    LOG_TRACE("localDesc.BufferDesc.Scaling: {}", magic_enum::enum_name(localDesc.BufferDesc.Scaling));
    LOG_TRACE("localDesc.BufferDesc.ScanlineOrdering: {}",
              magic_enum::enum_name(localDesc.BufferDesc.ScanlineOrdering));
    LOG_TRACE("localDesc.BufferDesc.Width: {}", localDesc.BufferDesc.Width);
    LOG_TRACE("localDesc.BufferUsage: {}", localDesc.BufferUsage);
    LOG_TRACE("localDesc.Flags: {}", localDesc.Flags);
    LOG_TRACE("localDesc.OutputWindow: {}", (UINT64) localDesc.OutputWindow);
    LOG_TRACE("localDesc.SampleDesc.Count: {}", localDesc.SampleDesc.Count);
    LOG_TRACE("localDesc.SampleDesc.Quality: {}", localDesc.SampleDesc.Quality);
    LOG_TRACE("localDesc.SwapEffect: {}", magic_enum::enum_name(localDesc.SwapEffect));
    LOG_TRACE("localDesc.Windowed: {}", localDesc.Windowed);
#endif //

    // Check for SL proxy, get real queue
    ID3D12CommandQueue* cq = nullptr;
    IUnknown* real = nullptr;
    HRESULT FGSCResult = E_NOTIMPL;

    // Another window while frame generation's swapchain lives: a plain swapchain, frame generation's
    // state stays as it is (framegen/FgWindowRule.h)
    const bool otherWindow = FGHooks::IsOtherWindowThanFG(localDesc.OutputWindow);

    if (pDevice->QueryInterface(IID_PPV_ARGS(&cq)) == S_OK)
    {
        if (State::Instance().currentD3D12Device == nullptr)
        {
            ID3D12Device* device = nullptr;
            if (cq->GetDevice(IID_PPV_ARGS(&device)) == S_OK)
            {
                if (device != nullptr)
                {
                    // Update current D3D12 device
                    if (State::Instance().currentD3D12Device != device)
                    {
                        State::Instance().currentD3D12Device = device;
                    }

                    LOG_INFO("Captured D3D12 device from command queue: {:X}", (UINT64) device);
                    D3D12Hooks::HookDevice(State::Instance().currentD3D12Device);
                }
            }
        }

        if (!Util::CheckForRealObject(__FUNCTION__, cq, &real))
            real = cq;

        if (!otherWindow)
            State::Instance().currentCommandQueue = (ID3D12CommandQueue*) real;

        if (!otherWindow && State::Instance().currentD3D12Device != nullptr)
        {
            WithDx12::SetD3D12Objects(State::Instance().currentD3D12Device, State::Instance().currentCommandQueue,
                                      D3D12_COMMAND_LIST_TYPE_DIRECT);
        }

        // Create FG SwapChain
        if (!_skipFGSwapChainCreation && !otherWindow)
        {
            ScopedSkipFGSCCreation skipFGSCCreation {};
            FGSCResult = FGHooks::CreateSwapChain(realFactory, real, &localDesc, ppSwapChain);

            if (FGSCResult == S_OK)
            {
                State::Instance().currentSwapchainDesc = localDesc;
                return FGSCResult;
            }
        }
    }
    else
    {
        LOG_INFO("Failed to get ID3D12CommandQueue from pDevice, creating Dx11 swapchain!");

        ID3D11Device* device = nullptr;

        if (pDevice->QueryInterface(IID_PPV_ARGS(&device)) == S_OK)
        {
            D3D11Hooks::HookToDevice(device);
            if (!otherWindow)
                State::Instance().currentD3D11Device = device;

            if (!_skipFGSwapChainCreation && !otherWindow && State::Instance().activeFgInput == FGInput::Upscaler &&
                State::Instance().activeFgOutput != FGOutput::NoFG &&
                State::Instance().activeFgInput != FGInput::NvngxFG && !DxvkFrameGenerationViaVulkan())
            {
                auto hiddenHwnd = CreateHiddenSwapchainWindow();

                ID3D12Device* dx12Device = nullptr;
                ID3D12CommandQueue* dx12Queue = nullptr;

                if (WithDx12::PrepareD3D12ForD3D11(device, D3D_FEATURE_LEVEL_11_0))
                {
                    dx12Device = WithDx12::GetD3D12Device();
                    dx12Queue = WithDx12::GetD3D12CommandQueue();
                }

                if (hiddenHwnd != nullptr && dx12Device != nullptr && dx12Queue != nullptr)
                {
                    DXGI_SWAP_CHAIN_DESC realDesc = localDesc;
                    realDesc.OutputWindow = hiddenHwnd;
                    realDesc.Windowed = TRUE;

                    IDXGISwapChain* realDx11SwapChain = nullptr;
                    HRESULT realScResult = E_FAIL;
                    {
                        ScopedSkipParentWrapping skipParentWrapping {};
                        realScResult = o_CreateSwapChain(realFactory, pDevice, &realDesc, &realDx11SwapChain);
                    }

                    DXGI_SWAP_CHAIN_DESC fgDesc = localDesc;
                    HRESULT fgScResult = E_FAIL;
                    IDXGISwapChain* fgSwapChain = nullptr;
                    IDXGISwapChain4* fgSwapChain4 = nullptr;
                    bool fgSwapChainIsRealFG = false;
                    const bool tearingSupported = IsTearingSupported(realFactory);

                    if (SUCCEEDED(realScResult) && PrepareDx12InteropDesc(fgDesc, tearingSupported))
                    {
                        {
                            ScopedSkipFGSCCreation skipFGSCCreation {};
                            fgScResult = FGHooks::CreateSwapChain(realFactory, dx12Queue, &fgDesc, &fgSwapChain);
                            fgSwapChainIsRealFG = SUCCEEDED(fgScResult) && fgSwapChain != nullptr;
                        }

                        if (FAILED(fgScResult) || fgSwapChain == nullptr)
                        {
                            fgSwapChainIsRealFG = false;

                            LOG_WARN("Dx11wDx12 FG swapchain creation failed: {:X}; creating plain DX12 swapchain",
                                     (UINT) fgScResult);

                            ScopedSkipParentWrapping skipParentWrapping {};
                            fgScResult = o_CreateSwapChain(realFactory, dx12Queue, &fgDesc, &fgSwapChain);
                        }

                        if (SUCCEEDED(fgScResult) && fgSwapChain != nullptr)
                            fgSwapChain->QueryInterface(IID_PPV_ARGS(&fgSwapChain4));
                    }

                    if (SUCCEEDED(realScResult) && realDx11SwapChain != nullptr && fgSwapChain4 != nullptr)
                    {
                        State::Instance().currentSwapchainDesc = fgDesc;
                        State::Instance().currentRealSwapchain = realDx11SwapChain;
                        State::Instance().currentFGSwapchain = fgSwapChain4;
                        if (!otherWindow)
                            State::Instance().currentD3D11Device = device;
                        State::Instance().currentD3D12Device = WithDx12::GetD3D12Device();
                        State::Instance().currentCommandQueue = WithDx12::GetD3D12CommandQueue();
                        State::Instance().swapchainInteropApi = SwapchainInteropApi::Dx11wDx12;

                        if (!fgSwapChainIsRealFG)
                            FGHooks::SetDx12InteropPresentSC(fgSwapChain4, localDesc.OutputWindow);

                        *ppSwapChain = new Dx11wDx12SC(realDx11SwapChain, fgSwapChain4, device, localDesc.OutputWindow,
                                                       localDesc.Flags);

                        State::Instance().currentSwapchain = *ppSwapChain;
                        State::Instance().currentWrappedSwapchain = *ppSwapChain;

                        LOG_INFO("Created Dx11wDx12SC: wrapper {:X}, real11 {:X}, fg12 {:X}", (size_t) *ppSwapChain,
                                 (size_t) realDx11SwapChain, (size_t) fgSwapChain4);

                        realDx11SwapChain->Release();
                        fgSwapChain4->Release();
                        if (fgSwapChain != nullptr)
                            fgSwapChain->Release();
                        device->Release();
                        return S_OK;
                    }

                    LOG_WARN("Dx11wDx12 swapchain creation failed: real {:X}, fg {:X}", (UINT) realScResult,
                             (UINT) fgScResult);

                    if (realDx11SwapChain != nullptr)
                        realDx11SwapChain->Release();
                    if (fgSwapChain4 != nullptr)
                        fgSwapChain4->Release();
                    if (fgSwapChain != nullptr)
                        fgSwapChain->Release();
                }
            }

            device->Release();
        }
    }

    HRESULT result = E_FAIL;

    // If FG is disabled or call is coming from FG library
    // Create the DXGI SwapChain and wrap it
    if (_skipFGSwapChainCreation || FGSCResult != S_OK)
    {
        // HDR output (output/SdrToHdr_*), on the final desc: a frame generation swap chain returned above, so this is the
        // plain chain (or a frame generation library's own inner one)
        SdrToHdr::CreateRequest hdrRequest;
        hdrRequest.device = pDevice;
        hdrRequest.format = localDesc.BufferDesc.Format;
        hdrRequest.windowed = localDesc.Windowed;
        hdrRequest.swapEffect = localDesc.SwapEffect;
        hdrRequest.bufferCount = localDesc.BufferCount;
        hdrRequest.flags = localDesc.Flags;
        hdrRequest.sampleCount = localDesc.SampleDesc.Count;
        hdrRequest.hasOriginal = true; // the game's own desc, before the V-Sync override and the rest
        hdrRequest.originalSwapEffect = pDesc->SwapEffect;
        hdrRequest.originalBufferCount = pDesc->BufferCount;
        hdrRequest.originalFlags = pDesc->Flags;
        hdrRequest.flipModel = localDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ||
                               localDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        hdrRequest.hwnd = localDesc.OutputWindow;
        hdrRequest.factory = realFactory;
        // Another window: HDR output leaves its chain alone
        SdrToHdr::Plan hdrPlan =
            otherWindow ? SdrToHdr::Plan {} : SdrToHdr::PrepareCreate(hdrRequest, localDesc.BufferDesc.Format);
        SdrToHdr::ScopedProxiedCreate hdrScope(hdrPlan.proxied);
        // The frame generation library asked for exclusive full screen: its HDR chain is made windowed
        const BOOL hdrGameWindowed = localDesc.Windowed;
        SdrToHdr::ApplyWindowed(hdrPlan, localDesc.Windowed);
        // An older (bitblt) chain is made flip model for HDR (the wrapper keeps telling the game what it asked for)
        SdrToHdr::ApplySwapDesc(hdrPlan, localDesc.SwapEffect, localDesc.BufferCount, localDesc.Flags);

        // !_skipFGSwapChainCreation for preventing early enablement flags
        if (!_skipFGSwapChainCreation)
        {
            State::Instance().skipDxgiLoadChecks = true;

            if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
                State::Instance().skipHeapCapture = true;
        }

        {
            ScopedSkipParentWrapping skipParentWrapping {};
            result = o_CreateSwapChain(realFactory, pDevice, &localDesc, ppSwapChain);

            if (FAILED(result) && hdrPlan.proxied)
            {
                SdrToHdr::RestoreSwapDesc(hdrPlan, localDesc.SwapEffect, localDesc.BufferCount, localDesc.Flags);
                SdrToHdr::Unproxy(hdrPlan, localDesc.BufferDesc.Format, result);
                localDesc.Windowed = hdrGameWindowed;
                result = o_CreateSwapChain(realFactory, pDevice, &localDesc, ppSwapChain);
            }
        }

        if (!_skipFGSwapChainCreation)
        {
            if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
                State::Instance().skipHeapCapture = false;

            State::Instance().skipDxgiLoadChecks = false;
        }

        if (SwapchainCreate::Created(result, ppSwapChain))
        {
            if (sizeToWindow)
            {
                // Keep the dimensions actually created by DXGI authoritative for menu sizing.
                DXGI_SWAP_CHAIN_DESC resolvedDesc {};
                if ((*ppSwapChain)->GetDesc(&resolvedDesc) == S_OK)
                {
                    LOG_INFO("CreateSwapChain: resolved size-to-window swapchain to {}x{}",
                             resolvedDesc.BufferDesc.Width, resolvedDesc.BufferDesc.Height);
                    localDesc.BufferDesc.Width = resolvedDesc.BufferDesc.Width;
                    localDesc.BufferDesc.Height = resolvedDesc.BufferDesc.Height;
                }
            }

            if (!otherWindow)
                State::Instance().currentSwapchainDesc = localDesc;

            if (!otherWindow && hdrPlan.proxied)
                State::Instance().currentSwapchainDesc.BufferDesc.Format = hdrPlan.gameFormat; // the game's, not the HDR one

            if (!otherWindow)
                SetPlainSwapchainInterop();

            // Check for SL proxy
            IDXGISwapChain* realSC = nullptr;
            if (!Util::CheckForRealObject(__FUNCTION__, *ppSwapChain, (IUnknown**) &realSC))
                realSC = *ppSwapChain;

            if (!otherWindow)
                State::Instance().currentRealSwapchain = realSC;

            IUnknown* realDevice = nullptr;
            if (!Util::CheckForRealObject(__FUNCTION__, pDevice, (IUnknown**) &realDevice))
                realDevice = pDevice;

            if (!otherWindow && Util::GetProcessWindow() == localDesc.OutputWindow)
            {
                State::Instance().screenWidth = static_cast<float>(localDesc.BufferDesc.Width);
                State::Instance().screenHeight = static_cast<float>(localDesc.BufferDesc.Height);
            }

            if (!otherWindow)
                DxgiFactoryHooks::NoteCreateStatus(result, realSC);

            LOG_DEBUG("Created new swapchain: {0:X}, hWnd: {1:X}", (UINT64) *ppSwapChain,
                      (UINT64) localDesc.OutputWindow);

            WrappedIDXGISwapChain4* wrapped;
            if ((*ppSwapChain)->QueryInterface(IID_PPV_ARGS(&wrapped)) != S_OK)
            {
                *ppSwapChain =
                    new WrappedIDXGISwapChain4(realSC, realDevice, localDesc.OutputWindow, localDesc.Flags, false, false,
                                           &hdrPlan);
                if (otherWindow)
                    static_cast<WrappedIDXGISwapChain4*>(*ppSwapChain)->MarkOtherWindow();

                // Set as currentSwapchain is FG is disabled
                if (!_skipFGSwapChainCreation && !otherWindow)
                    State::Instance().currentSwapchain = *ppSwapChain;

                if (!otherWindow)
                    State::Instance().currentWrappedSwapchain = *ppSwapChain;

                LOG_DEBUG("Created new WrappedIDXGISwapChain4: {:X}, pDevice: {:X}", (size_t) *ppSwapChain,
                          (size_t) pDevice);
            }
            else
            {
                // (also a plan that stood aside: it marks the library chain)
                wrapped->AdoptHdrPlan(hdrPlan, localDesc.Flags);

                wrapped->Release();
            }
        }
    }
    else
    {
        LOG_ERROR("CreateSwapChain failed: {:X}", (UINT) result);
    }

    return result;
}

HRESULT DxgiFactoryHooks::CreateSwapChainForHwnd(IDXGIFactory2* realFactory, IUnknown* pDevice, HWND hWnd,
                                                 const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                 const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                                 IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain)
{
    *ppSwapChain = nullptr;

    static bool firstCall = static_cast<bool>(State::Instance().gameQuirks & GameQuirk::NoFSRFGFirstSwapchain);
    if (firstCall)
    {
        LOG_DEBUG("Skipping FG swapchain creation");
        _skipFGSwapChainCreation = true;
    }

    if (State::Instance().vulkanCreatingSC)
    {
        LOG_WARN("Vulkan is creating swapchain!");
        HRESULT result;

        {
            ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
            ScopedSkipParentWrapping skipParentWrapping {};

            result = o_CreateSwapChainForHwnd(realFactory, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput,
                                              ppSwapChain);
        }

        if (firstCall)
            _skipFGSwapChainCreation = false;

        return result;
    }

    if (pDevice == nullptr || pDesc == nullptr)
    {
        LOG_WARN("pDevice or pDesc is nullptr!");
        HRESULT result;

        {
            ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
            ScopedSkipParentWrapping skipParentWrapping {};
            result = o_CreateSwapChainForHwnd(realFactory, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput,
                                              ppSwapChain);
        }

        if (firstCall)
            _skipFGSwapChainCreation = false;

        return result;
    }

    if (pDesc->Height < 100 || pDesc->Width < 100)
    {
        LOG_WARN("Overlay call! Width: {}, Height: {}", pDesc->Width, pDesc->Height);
        HRESULT result;

        {
            ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
            ScopedSkipParentWrapping skipParentWrapping {};
            result = o_CreateSwapChainForHwnd(realFactory, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput,
                                              ppSwapChain);
        }

        if (firstCall)
            _skipFGSwapChainCreation = false;

        return result;
    }

    DXGI_SWAP_CHAIN_DESC1 localDesc {};
    memcpy(&localDesc, pDesc, sizeof(DXGI_SWAP_CHAIN_DESC1));

    LOG_DEBUG("Width: {}, Height: {}, Format: {}, Count: {}, Flags: {:X}, Hwnd: {:X}, SkipWrapping: {}",
              localDesc.Width, localDesc.Height, (UINT) localDesc.Format, localDesc.BufferCount, localDesc.Flags,
              (size_t) hWnd, _skipFGSwapChainCreation);

    if (pFullscreenDesc != nullptr)
        State::Instance().realExclusiveFullscreen = !pFullscreenDesc->Windowed;

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC localFullscreenDesc {};

    if (pFullscreenDesc != nullptr)
        memcpy(&localFullscreenDesc, pFullscreenDesc, sizeof(DXGI_SWAP_CHAIN_FULLSCREEN_DESC));

    if (UsesXeFGApi(State::Instance().activeFgOutput) && FGHooks::BorderlessForced())
    {
        if (pFullscreenDesc != nullptr && !localFullscreenDesc.Windowed)
        {

            State::Instance().SCExclusiveFullscreen = true;
            localFullscreenDesc.Windowed = true;
        }

        localDesc.Flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        localDesc.Scaling = DXGI_SCALING_STRETCH;
    }

    // For vsync override
    if (pFullscreenDesc != nullptr && !localFullscreenDesc.Windowed)
    {
        LOG_INFO("Game is creating fullscreen swapchain, disabled V-Sync overrides");
        Config::Instance()->OverrideVsync.set_volatile_value(false);
    }

    if (Config::Instance()->OverrideVsync.value_or_default())
    {
        localDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        localDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

        if (localDesc.BufferCount < 2)
            localDesc.BufferCount = 2;
    }

    State::Instance().SCAllowTearing = (localDesc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) > 0;
    State::Instance().SCLastFlags = localDesc.Flags;
    State::Instance().realExclusiveFullscreen = pFullscreenDesc != nullptr && !localFullscreenDesc.Windowed;

#ifdef VER_PRE_RELEASE
    LOG_TRACE("localDesc.AlphaMode : {}", magic_enum::enum_name(localDesc.AlphaMode));
    LOG_TRACE("localDesc.BufferCount : {}", localDesc.BufferCount);
    LOG_TRACE("localDesc.BufferUsage : {}", localDesc.BufferUsage);
    LOG_TRACE("localDesc.Flags : {}", localDesc.Flags);
    LOG_TRACE("localDesc.Format : {}", magic_enum::enum_name(localDesc.Format));
    LOG_TRACE("localDesc.Height : {}", localDesc.Height);
    LOG_TRACE("localDesc.SampleDesc.Count : {}", localDesc.SampleDesc.Count);
    LOG_TRACE("localDesc.SampleDesc.Quality : {}", localDesc.SampleDesc.Quality);
    LOG_TRACE("localDesc.Scaling : {}", magic_enum::enum_name(localDesc.Scaling));
    LOG_TRACE("localDesc.Stereo : {}", localDesc.Stereo);

    if (pFullscreenDesc != nullptr)
    {
        LOG_TRACE("localFullscreenDesc.RefreshRate.Denominator : {}", localFullscreenDesc.RefreshRate.Denominator);
        LOG_TRACE("localFullscreenDesc.RefreshRate.Numerator : {}", localFullscreenDesc.RefreshRate.Numerator);
        LOG_TRACE("localFullscreenDesc.Scaling : {}", magic_enum::enum_name(localFullscreenDesc.Scaling));
        LOG_TRACE("localFullscreenDesc.ScanlineOrdering : {}",
                  magic_enum::enum_name(localFullscreenDesc.ScanlineOrdering));
        LOG_TRACE("localFullscreenDesc.Windowed : {}", localFullscreenDesc.Windowed);
    }
#endif

    // Check for SL proxy, get real queue
    ID3D12CommandQueue* cq = nullptr;
    IUnknown* real = nullptr;
    HRESULT FGSCResult = E_NOTIMPL;

    // Another window while frame generation's swapchain lives: a plain swapchain, frame generation's
    // state stays as it is (framegen/FgWindowRule.h)
    const bool otherWindow = FGHooks::IsOtherWindowThanFG(hWnd);

    if (pDevice->QueryInterface(IID_PPV_ARGS(&cq)) == S_OK)
    {
        if (State::Instance().currentD3D12Device == nullptr)
        {
            ID3D12Device* device = nullptr;
            if (cq->GetDevice(IID_PPV_ARGS(&device)) == S_OK)
            {
                if (device != nullptr)
                {
                    // Update current D3D12 device
                    if (State::Instance().currentD3D12Device != device)
                    {
                        State::Instance().currentD3D12Device = device;
                    }

                    LOG_INFO("Captured D3D12 device from command queue: {:X}", (UINT64) device);
                    D3D12Hooks::HookDevice(State::Instance().currentD3D12Device);
                }
            }
        }

        if (!Util::CheckForRealObject(__FUNCTION__, cq, &real))
            real = cq;

        if (!otherWindow)
            State::Instance().currentCommandQueue = (ID3D12CommandQueue*) real;

        if (!otherWindow && State::Instance().currentD3D12Device != nullptr)
        {
            WithDx12::SetD3D12Objects(State::Instance().currentD3D12Device, State::Instance().currentCommandQueue,
                                      D3D12_COMMAND_LIST_TYPE_DIRECT);
        }

        // Create FG SwapChain
        if (!_skipFGSwapChainCreation && !otherWindow)
        {
            ScopedSkipFGSCCreation skipFGSCCreation {};
            FGSCResult = FGHooks::CreateSwapChainForHwnd(realFactory, real, hWnd, &localDesc,
                                                         pFullscreenDesc != nullptr ? &localFullscreenDesc : nullptr,
                                                         pRestrictToOutput, ppSwapChain);

            if (FGSCResult == S_OK)
            {
                ((IDXGISwapChain*) *ppSwapChain)->GetDesc(&State::Instance().currentSwapchainDesc);
                return FGSCResult;
            }
        }
    }
    else
    {
        LOG_INFO("Failed to get ID3D12CommandQueue from pDevice, creating Dx11 swapchain!");

        ID3D11Device* device = nullptr;

        if (pDevice->QueryInterface(IID_PPV_ARGS(&device)) == S_OK)
        {
            D3D11Hooks::HookToDevice(device);
            if (!otherWindow)
                State::Instance().currentD3D11Device = device;

            if (!_skipFGSwapChainCreation && !otherWindow && State::Instance().activeFgInput == FGInput::Upscaler &&
                State::Instance().activeFgOutput != FGOutput::NoFG &&
                State::Instance().activeFgInput != FGInput::NvngxFG && !DxvkFrameGenerationViaVulkan())
            {
                // For dx11 swapchain
                auto hiddenHwnd = CreateHiddenSwapchainWindow();

                ID3D12Device* dx12Device = nullptr;
                ID3D12CommandQueue* dx12Queue = nullptr;

                if (WithDx12::PrepareD3D12ForD3D11(device, D3D_FEATURE_LEVEL_11_0))
                {
                    dx12Device = WithDx12::GetD3D12Device();
                    dx12Queue = WithDx12::GetD3D12CommandQueue();
                }

                if (hiddenHwnd != nullptr && dx12Device != nullptr && dx12Queue != nullptr)
                {
                    DXGI_SWAP_CHAIN_DESC1 realDesc = localDesc;
                    IDXGISwapChain1* realDx11SwapChain1 = nullptr;
                    HRESULT realScResult = E_FAIL;
                    {
                        ScopedSkipParentWrapping skipParentWrapping {};
                        realScResult = o_CreateSwapChainForHwnd(realFactory, pDevice, hiddenHwnd, &realDesc, nullptr,
                                                                pRestrictToOutput, &realDx11SwapChain1);
                    }

                    DXGI_SWAP_CHAIN_DESC1 fgDesc = localDesc;
                    HRESULT fgScResult = E_FAIL;
                    IDXGISwapChain1* fgSwapChain1 = nullptr;
                    IDXGISwapChain4* fgSwapChain4 = nullptr;
                    bool fgSwapChainIsRealFG = false;
                    const bool tearingSupported = IsTearingSupported(realFactory);

                    if (realScResult == S_OK && PrepareDx12InteropDesc1(fgDesc, tearingSupported))
                    {
                        {
                            ScopedSkipFGSCCreation skipFGSCCreation {};
                            fgScResult = FGHooks::CreateSwapChainForHwnd(
                                realFactory, dx12Queue, hWnd, &fgDesc,
                                pFullscreenDesc != nullptr ? &localFullscreenDesc : nullptr, pRestrictToOutput,
                                &fgSwapChain1);

                            fgSwapChainIsRealFG = fgScResult == S_OK && fgSwapChain1 != nullptr;
                        }

                        if (fgScResult != S_OK || fgSwapChain1 == nullptr)
                        {
                            fgSwapChainIsRealFG = false;

                            LOG_WARN("Dx11wDx12 FG swapchain creation failed: {:X}; creating plain DX12 swapchain",
                                     (UINT) fgScResult);

                            ScopedSkipParentWrapping skipParentWrapping {};
                            fgScResult =
                                o_CreateSwapChainForHwnd(realFactory, dx12Queue, hWnd, &fgDesc,
                                                         pFullscreenDesc != nullptr ? &localFullscreenDesc : nullptr,
                                                         pRestrictToOutput, &fgSwapChain1);
                        }

                        if (fgScResult == S_OK && fgSwapChain1 != nullptr)
                            fgSwapChain1->QueryInterface(IID_PPV_ARGS(&fgSwapChain4));
                    }

                    if (realScResult == S_OK && realDx11SwapChain1 != nullptr && fgSwapChain4 != nullptr)
                    {
                        ((IDXGISwapChain*) fgSwapChain4)->GetDesc(&State::Instance().currentSwapchainDesc);
                        State::Instance().currentSwapchainDesc.OutputWindow = hWnd;
                        State::Instance().currentRealSwapchain = realDx11SwapChain1;
                        State::Instance().currentFGSwapchain = fgSwapChain4;
                        if (!otherWindow)
                            State::Instance().currentD3D11Device = device;
                        State::Instance().currentD3D12Device = WithDx12::GetD3D12Device();
                        State::Instance().currentCommandQueue = WithDx12::GetD3D12CommandQueue();
                        State::Instance().swapchainInteropApi = SwapchainInteropApi::Dx11wDx12;

                        if (!fgSwapChainIsRealFG)
                            FGHooks::SetDx12InteropPresentSC((IDXGISwapChain*) fgSwapChain4, hWnd);

                        *ppSwapChain = (IDXGISwapChain1*) new Dx11wDx12SC(realDx11SwapChain1, fgSwapChain4, device,
                                                                          hWnd, localDesc.Flags);

                        State::Instance().currentSwapchain = *ppSwapChain;
                        State::Instance().currentWrappedSwapchain = *ppSwapChain;

                        LOG_INFO("Created Dx11wDx12SC HWND: wrapper {:X}, real11 {:X}, fg12 {:X}",
                                 (size_t) *ppSwapChain, (size_t) realDx11SwapChain1, (size_t) fgSwapChain4);

                        realDx11SwapChain1->Release();
                        fgSwapChain4->Release();

                        if (fgSwapChain1 != nullptr)
                            fgSwapChain1->Release();

                        device->Release();
                        return S_OK;
                    }

                    LOG_WARN("Dx11wDx12 HWND swapchain creation failed: real {:X}, fg {:X}", (UINT) realScResult,
                             (UINT) fgScResult);

                    if (realDx11SwapChain1 != nullptr)
                        realDx11SwapChain1->Release();
                    if (fgSwapChain4 != nullptr)
                        fgSwapChain4->Release();
                    if (fgSwapChain1 != nullptr)
                        fgSwapChain1->Release();
                }
            }

            // Legacy DX11 FG path intentionally removed.
            // DX11 FG must now go through Dx11wDx12SC; if interop creation failed, fall back to the normal wrapper path
            // below.

            device->Release();
        }
    }

    HRESULT result = E_FAIL;

    // If FG is disabled or call is coming from FG library
    // Create the DXGI SwapChain and wrap it
    if (_skipFGSwapChainCreation || FGSCResult != S_OK)
    {
        // HDR output, as in CreateSwapChain
        SdrToHdr::CreateRequest hdrRequest;
        hdrRequest.device = pDevice;
        hdrRequest.format = localDesc.Format;
        hdrRequest.windowed = pFullscreenDesc == nullptr || localFullscreenDesc.Windowed;
        hdrRequest.swapEffect = localDesc.SwapEffect;
        hdrRequest.bufferCount = localDesc.BufferCount;
        hdrRequest.flags = localDesc.Flags;
        hdrRequest.sampleCount = localDesc.SampleDesc.Count;
        hdrRequest.hasOriginal = true; // the game's own desc, before the V-Sync override and the rest
        hdrRequest.originalSwapEffect = pDesc->SwapEffect;
        hdrRequest.originalBufferCount = pDesc->BufferCount;
        hdrRequest.originalFlags = pDesc->Flags;
        hdrRequest.flipModel = localDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ||
                               localDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        hdrRequest.hwnd = hWnd;
        hdrRequest.factory = realFactory;
        // Another window: HDR output leaves its chain alone
        SdrToHdr::Plan hdrPlan =
            otherWindow ? SdrToHdr::Plan {} : SdrToHdr::PrepareCreate(hdrRequest, localDesc.Format);
        SdrToHdr::ScopedProxiedCreate hdrScope(hdrPlan.proxied);
        // An older (bitblt) chain is made flip model for HDR (the wrapper keeps telling the game what it asked for)
        SdrToHdr::ApplySwapDesc(hdrPlan, localDesc.SwapEffect, localDesc.BufferCount, localDesc.Flags);

        // !_skipFGSwapChainCreation for preventing early enablement flags
        if (!_skipFGSwapChainCreation)
        {
            State::Instance().skipDxgiLoadChecks = true;

            if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
                State::Instance().skipHeapCapture = true;
        }

        {
            ScopedSkipParentWrapping skipParentWrapping {};
            result = o_CreateSwapChainForHwnd(
                realFactory, pDevice, hWnd, &localDesc,
                SdrToHdr::KeepFullscreenDesc(hdrPlan, pFullscreenDesc != nullptr) ? &localFullscreenDesc : nullptr,
                pRestrictToOutput, ppSwapChain);

            if (FAILED(result) && hdrPlan.proxied)
            {
                SdrToHdr::RestoreSwapDesc(hdrPlan, localDesc.SwapEffect, localDesc.BufferCount, localDesc.Flags);
                SdrToHdr::Unproxy(hdrPlan, localDesc.Format, result);
                result = o_CreateSwapChainForHwnd(
                    realFactory, pDevice, hWnd, &localDesc,
                    SdrToHdr::KeepFullscreenDesc(hdrPlan, pFullscreenDesc != nullptr) ? &localFullscreenDesc : nullptr,
                    pRestrictToOutput, ppSwapChain);
            }
        }

        if (!_skipFGSwapChainCreation)
        {
            if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
                State::Instance().skipHeapCapture = false;

            State::Instance().skipDxgiLoadChecks = false;
        }

        if (SwapchainCreate::Created(result, ppSwapChain))
        {
            if (!otherWindow)
                SetPlainSwapchainInterop();

            // check for SL proxy
            IDXGISwapChain1* realSC = nullptr;
            if (!Util::CheckForRealObject(__FUNCTION__, *ppSwapChain, (IUnknown**) &realSC))
                realSC = *ppSwapChain;

            if (!otherWindow)
                State::Instance().currentRealSwapchain = realSC;

            IUnknown* readDevice = nullptr;
            if (!Util::CheckForRealObject(__FUNCTION__, pDevice, (IUnknown**) &readDevice))
                readDevice = pDevice;

            if (!otherWindow && Util::GetProcessWindow() == hWnd)
            {
                State::Instance().screenWidth = static_cast<float>(localDesc.Width);
                State::Instance().screenHeight = static_cast<float>(localDesc.Height);
            }

            if (!otherWindow)
                realSC->GetDesc(&State::Instance().currentSwapchainDesc);

            if (!otherWindow && hdrPlan.proxied)
                State::Instance().currentSwapchainDesc.BufferDesc.Format = hdrPlan.gameFormat; // the game's, not the HDR one

            if (!otherWindow)
                DxgiFactoryHooks::NoteCreateStatus(result, realSC);

            LOG_DEBUG("Created new swapchain: {0:X}, hWnd: {1:X}", (uintptr_t) *ppSwapChain, (uintptr_t) hWnd);

            WrappedIDXGISwapChain4* wrapped;
            if ((*ppSwapChain)->QueryInterface(IID_PPV_ARGS(&wrapped)) != S_OK)
            {
                *ppSwapChain =
                    new WrappedIDXGISwapChain4(realSC, readDevice, hWnd, localDesc.Flags, false, false, &hdrPlan);
                if (otherWindow)
                    static_cast<WrappedIDXGISwapChain4*>(*ppSwapChain)->MarkOtherWindow();

                LOG_DEBUG("Created new WrappedIDXGISwapChain4: {0:X}, pDevice: {1:X}", (uintptr_t) *ppSwapChain,
                          (uintptr_t) pDevice);

                if (!_skipFGSwapChainCreation && !otherWindow)
                    State::Instance().currentSwapchain = *ppSwapChain;

                if (!otherWindow)
                    State::Instance().currentWrappedSwapchain = *ppSwapChain;
            }
            else
            {
                // (also a plan that stood aside: it marks the library chain)
                wrapped->AdoptHdrPlan(hdrPlan, localDesc.Flags);

                wrapped->Release();
            }
        }
        else
        {
            LOG_ERROR("CreateSwapChainForHwnd failed: {:X}", (UINT) result);
        }
    }

    if (firstCall)
    {
        LOG_DEBUG("Unsetting skip FG swapchain creation");
        _skipFGSwapChainCreation = false;
        firstCall = false;
    }

    return result;
}

HRESULT DxgiFactoryHooks::CreateSwapChainForCoreWindow(IDXGIFactory2* realFactory, IUnknown* pDevice, IUnknown* pWindow,
                                                       const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                       IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain)
{
    if (State::Instance().vulkanCreatingSC)
    {
        LOG_WARN("Vulkan is creating swapchain!");

        if (pDesc != nullptr)
            LOG_DEBUG("Width: {}, Height: {}, Format: {}, Flags: {:X}, Count: {}, SkipWrapping: {}", pDesc->Width,
                      pDesc->Height, (UINT) pDesc->Format, pDesc->Flags, pDesc->BufferCount, _skipFGSwapChainCreation);

        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        return realFactory->CreateSwapChainForCoreWindow(pDevice, pWindow, pDesc, pRestrictToOutput, ppSwapChain);
    }

    if (pDevice == nullptr || pDesc == nullptr)
    {
        LOG_WARN("pDevice or pDesc is nullptr!");
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        return realFactory->CreateSwapChainForCoreWindow(pDevice, pWindow, pDesc, pRestrictToOutput, ppSwapChain);
    }

    if (pDesc->Height < 100 || pDesc->Width < 100)
    {
        LOG_WARN("Overlay call! Width: {}, Height: {}", pDesc->Width, pDesc->Height);

        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        return realFactory->CreateSwapChainForCoreWindow(pDevice, pWindow, pDesc, pRestrictToOutput, ppSwapChain);
    }

    DXGI_SWAP_CHAIN_DESC1 localDesc {};
    memcpy(&localDesc, pDesc, sizeof(DXGI_SWAP_CHAIN_DESC1));

    LOG_DEBUG("Width: {}, Height: {}, Format: {}, Count: {}, SkipWrapping: {}", localDesc.Width, localDesc.Height,
              (UINT) localDesc.Format, localDesc.BufferCount, _skipFGSwapChainCreation);

    // For vsync override
    if (Config::Instance()->OverrideVsync.value_or_default())
    {
        localDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        localDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

        if (localDesc.BufferCount < 2)
            localDesc.BufferCount = 2;
    }

    State::Instance().SCAllowTearing = (localDesc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) > 0;
    State::Instance().SCLastFlags = localDesc.Flags;
    State::Instance().realExclusiveFullscreen = false;

    ID3D12CommandQueue* cq = nullptr;
    IUnknown* real = nullptr;
    if (pDevice->QueryInterface(IID_PPV_ARGS(&cq)) == S_OK)
    {
        if (!Util::CheckForRealObject(__FUNCTION__, cq, &real))
            real = cq;

        State::Instance().currentCommandQueue = (ID3D12CommandQueue*) real;

        if (State::Instance().currentD3D12Device != nullptr)
            WithDx12::SetD3D12Objects(State::Instance().currentD3D12Device, State::Instance().currentCommandQueue,
                                      D3D12_COMMAND_LIST_TYPE_DIRECT);
    }

    {
        // HDR output: CoreWindow (UWP) swap chains are not supported; this logs the stand-down and the chain is not touched
        SdrToHdr::CreateRequest hdrRequest;
        hdrRequest.device = pDevice;
        hdrRequest.coreWindow = true;
        hdrRequest.format = localDesc.Format;
        hdrRequest.flipModel = localDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ||
                               localDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        DXGI_FORMAT hdrFormat = localDesc.Format;
        SdrToHdr::PrepareCreate(hdrRequest, hdrFormat);
    }

    HRESULT result = E_FAIL;
    {
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        result =
            o_CreateSwapChainForCoreWindow(realFactory, pDevice, pWindow, &localDesc, pRestrictToOutput, ppSwapChain);
    }

    if (SwapchainCreate::Created(result, ppSwapChain))
    {
        // check for SL proxy
        IDXGISwapChain* realSC = nullptr;
        if (!Util::CheckForRealObject(__FUNCTION__, *ppSwapChain, (IUnknown**) &realSC))
            realSC = *ppSwapChain;

        State::Instance().currentRealSwapchain = realSC;

        IUnknown* readDevice = nullptr;
        if (!Util::CheckForRealObject(__FUNCTION__, pDevice, (IUnknown**) &readDevice))
            readDevice = pDevice;

        realSC->GetDesc(&State::Instance().currentSwapchainDesc);

        State::Instance().screenWidth = static_cast<float>(localDesc.Width);
        State::Instance().screenHeight = static_cast<float>(localDesc.Height);

        LOG_DEBUG("Created new swapchain: {0:X}, hWnd: {1:X}", (UINT64) *ppSwapChain, (UINT64) pWindow);

        WrappedIDXGISwapChain4* wrapped;
        if ((*ppSwapChain)->QueryInterface(IID_PPV_ARGS(&wrapped)) != S_OK)
        {
            *ppSwapChain = new WrappedIDXGISwapChain4(realSC, readDevice, (HWND) pWindow, localDesc.Flags, true);

            if (!_skipFGSwapChainCreation)
                State::Instance().currentSwapchain = *ppSwapChain;

            State::Instance().currentWrappedSwapchain = *ppSwapChain;

            LOG_DEBUG("Created new WrappedIDXGISwapChain4: {0:X}, pDevice: {1:X}", (UINT64) *ppSwapChain,
                      (UINT64) pDevice);
        }
        else
        {
            wrapped->Release();
        }
    }

    return result;
}


HRESULT DxgiFactoryHooks::CreateSwapChainForComposition(IDXGIFactory2* realFactory, IUnknown* pDevice,
                                                        const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                        IDXGIOutput* pRestrictToOutput,
                                                        IDXGISwapChain1** ppSwapChain)
{
    // Always call the trampoline, including pass-through/error cases. Calling the detoured virtual
    // method here re-enters this hook. Keep the composition descriptor intact: notably, a desktop
    // VSync override must not turn its FLIP_SEQUENTIAL swap effect into FLIP_DISCARD.
    const bool passThrough = State::Instance().vulkanCreatingSC || _skipFGSwapChainCreation ||
                             pDevice == nullptr || pDesc == nullptr || ppSwapChain == nullptr ||
                             pDesc->Width < 100 || pDesc->Height < 100;
    if (pDesc != nullptr && (pDesc->Width < 100 || pDesc->Height < 100))
        LOG_WARN("Composition overlay/helper call! Width: {}, Height: {}", pDesc->Width, pDesc->Height);

    HRESULT result;
    {
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        ScopedSkipParentWrapping skipParentWrapping {};
        result = o_CreateSwapChainForComposition(realFactory, pDevice, pDesc, pRestrictToOutput, ppSwapChain);
    }
    if (passThrough || FAILED(result) || *ppSwapChain == nullptr)
        return result;

    {
        // HDR output: composition swap chains are presented through the compositor and are not touched
        SdrToHdr::CreateRequest hdrRequest;
        hdrRequest.device = pDevice;
        hdrRequest.format = pDesc->Format;
        hdrRequest.flipModel = true;
        hdrRequest.composition = true;
        DXGI_FORMAT hdrFormat = pDesc->Format;
        SdrToHdr::PrepareCreate(hdrRequest, hdrFormat);
    }

    WrappedIDXGISwapChain4* existing = nullptr;
    if ((*ppSwapChain)->QueryInterface(IID_PPV_ARGS(&existing)) == S_OK)
    {
        existing->Release();
        return result;
    }

    DXGI_SWAP_CHAIN_DESC1 resolved {};
    if (FAILED((*ppSwapChain)->GetDesc1(&resolved)))
        return result; // Do not install a wrapper with unknown dimensions/flags.

    // A composition chain has no native HWND. Use only an eligible window in this process, and
    // defer overlay initialization to Present if no unambiguous window exists yet. Do not borrow
    // another application's foreground window. HWND association remains a best-effort fallback.
    const HWND window = FindCompositionWindow();
    IDXGISwapChain1* const created = *ppSwapChain;
    auto* wrapped = new WrappedIDXGISwapChain4(created, pDevice, window, resolved.Flags, false, true);
    *ppSwapChain = wrapped;

    // Preserve the returned COM/proxy chain: the wrapper owns its original reference. Unwrapping it
    // here can leak an outer proxy and bypass another provider's presentation path.
    State::Instance().currentRealSwapchain = created;
    State::Instance().currentSwapchain = wrapped;
    State::Instance().currentWrappedSwapchain = wrapped;
    SetPlainSwapchainInterop();
    State::Instance().SCAllowTearing = (resolved.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;
    State::Instance().SCLastFlags = resolved.Flags;
    State::Instance().realExclusiveFullscreen = false;
    State::Instance().screenWidth = (float) resolved.Width;
    State::Instance().screenHeight = (float) resolved.Height;
    DXGI_SWAP_CHAIN_DESC legacy {};
    if (SUCCEEDED(created->GetDesc(&legacy)))
        State::Instance().currentSwapchainDesc = legacy;
    LOG_INFO("Wrapped composition swapchain {}x{}, window {:X}; plain presentation, no FG swapchain replacement",
             resolved.Width, resolved.Height, (SIZE_T) window);
    return result;
}

HRESULT DxgiFactoryHooks::DLSSGCreateSwapChain(IDXGIFactory* realFactory, IUnknown* pDevice,
                                               DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain)
{
    *ppSwapChain = nullptr;

    if (State::Instance().vulkanCreatingSC)
    {
        LOG_WARN("Vulkan is creating swapchain!");

        if (pDesc != nullptr)
            LOG_DEBUG("Width: {}, Height: {}, Format: {}, Count: {}, Hwnd: {:X}, Windowed: {}, SkipWrapping: {}",
                      pDesc->BufferDesc.Width, pDesc->BufferDesc.Height, (UINT) pDesc->BufferDesc.Format,
                      pDesc->BufferCount, (SIZE_T) pDesc->OutputWindow, pDesc->Windowed, _skipFGSwapChainCreation);

        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        ScopedSkipParentWrapping skipParentWrapping {};

        auto res = o_DLSSGCreateSwapChain(realFactory, pDevice, pDesc, ppSwapChain);
        return res;
    }

    if (pDevice == nullptr || pDesc == nullptr)
    {
        LOG_WARN("pDevice or pDesc is nullptr!");

        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        ScopedSkipParentWrapping skipParentWrapping {};

        auto res = o_DLSSGCreateSwapChain(realFactory, pDevice, pDesc, ppSwapChain);
        return res;
    }

    DXGI_SWAP_CHAIN_DESC localDesc = *pDesc;
    ResolveWindowSizedSwapchain(localDesc);

    if (localDesc.BufferDesc.Height < 100 || localDesc.BufferDesc.Width < 100)
    {
        LOG_WARN("Overlay call! Width: {}, Height: {}", pDesc->BufferDesc.Width, pDesc->BufferDesc.Height);

        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        ScopedSkipParentWrapping skipParentWrapping {};

        auto res = o_DLSSGCreateSwapChain(realFactory, pDevice, pDesc, ppSwapChain);
        return res;
    }

    LOG_DEBUG("Width: {}, Height: {}, Format: {}, Count: {}, Flags: {:X}, Hwnd: {:X}, Windowed: {}, SkipWrapping: {}",
              localDesc.BufferDesc.Width, localDesc.BufferDesc.Height, (UINT) localDesc.BufferDesc.Format,
              localDesc.BufferCount, localDesc.Flags, (SIZE_T) localDesc.OutputWindow, localDesc.Windowed,
              _skipFGSwapChainCreation);

    if (UsesXeFGApi(State::Instance().activeFgOutput) && FGHooks::BorderlessForced())
    {
        if (!localDesc.Windowed)
        {
            State::Instance().SCExclusiveFullscreen = true;
            localDesc.Windowed = true;
        }

        localDesc.Flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        localDesc.BufferDesc.Scaling = DXGI_MODE_SCALING_STRETCHED;
    }

    // For vsync override
    if (!localDesc.Windowed)
    {
        LOG_INFO("Game is creating fullscreen swapchain, disabled V-Sync overrides");
        Config::Instance()->OverrideVsync.set_volatile_value(false);
    }

    if (Config::Instance()->OverrideVsync.value_or_default())
    {
        localDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        localDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

        if (localDesc.BufferCount < 2)
            localDesc.BufferCount = 2;
    }

    State::Instance().SCAllowTearing = (localDesc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) > 0;
    State::Instance().SCLastFlags = localDesc.Flags;
    State::Instance().realExclusiveFullscreen = !localDesc.Windowed;

#ifdef DETAILED_SC_LOGS
    LOG_TRACE("localDesc.BufferCount: {}", localDesc.BufferCount);
    LOG_TRACE("localDesc.BufferDesc.Format: {}", magic_enum::enum_name(localDesc.BufferDesc.Format));
    LOG_TRACE("localDesc.BufferDesc.Height: {}", localDesc.BufferDesc.Height);
    LOG_TRACE("localDesc.BufferDesc.RefreshRate.Denominator: {}", localDesc.BufferDesc.RefreshRate.Denominator);
    LOG_TRACE("localDesc.BufferDesc.RefreshRate.Numerator: {}", localDesc.BufferDesc.RefreshRate.Numerator);
    LOG_TRACE("localDesc.BufferDesc.Scaling: {}", magic_enum::enum_name(localDesc.BufferDesc.Scaling));
    LOG_TRACE("localDesc.BufferDesc.ScanlineOrdering: {}",
              magic_enum::enum_name(localDesc.BufferDesc.ScanlineOrdering));
    LOG_TRACE("localDesc.BufferDesc.Width: {}", localDesc.BufferDesc.Width);
    LOG_TRACE("localDesc.BufferUsage: {}", localDesc.BufferUsage);
    LOG_TRACE("localDesc.Flags: {}", localDesc.Flags);
    LOG_TRACE("localDesc.OutputWindow: {}", (UINT64) localDesc.OutputWindow);
    LOG_TRACE("localDesc.SampleDesc.Count: {}", localDesc.SampleDesc.Count);
    LOG_TRACE("localDesc.SampleDesc.Quality: {}", localDesc.SampleDesc.Quality);
    LOG_TRACE("localDesc.SwapEffect: {}", magic_enum::enum_name(localDesc.SwapEffect));
    LOG_TRACE("localDesc.Windowed: {}", localDesc.Windowed);
#endif //

    // Check for SL proxy, get real queue
    ID3D12CommandQueue* cq = nullptr;
    IUnknown* real = nullptr;
    HRESULT FGSCResult = E_NOTIMPL;

    // Another window while frame generation's swapchain lives: a plain swapchain, frame generation's
    // state stays as it is (framegen/FgWindowRule.h)
    const bool otherWindow = FGHooks::IsOtherWindowThanFG(localDesc.OutputWindow);

    if (pDevice->QueryInterface(IID_PPV_ARGS(&cq)) == S_OK)
    {
        if (State::Instance().currentD3D12Device == nullptr)
        {
            ID3D12Device* device = nullptr;
            if (cq->GetDevice(IID_PPV_ARGS(&device)) == S_OK)
            {
                if (device != nullptr)
                {
                    // Update current D3D12 device and adapter
                    if (State::Instance().currentD3D12Device != device)
                    {
                        State::Instance().currentD3D12Device = device;

                        IDXGIDevice* dxgiDevice = nullptr;
                        if (device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)) == S_OK)
                        {
                            IDXGIAdapter* adapter = nullptr;
                            if (dxgiDevice->GetAdapter(&adapter) == S_OK)
                                adapter->Release();

                            dxgiDevice->Release();
                        }
                    }

                    LOG_INFO("Captured D3D12 device from command queue: {:X}", (UINT64) device);
                    D3D12Hooks::HookDevice(State::Instance().currentD3D12Device);
                }
            }
        }

        if (!Util::CheckForRealObject(__FUNCTION__, cq, &real))
            real = cq;

        if (!otherWindow)
            State::Instance().currentCommandQueue = (ID3D12CommandQueue*) real;

        if (!otherWindow && State::Instance().currentD3D12Device != nullptr)
            WithDx12::SetD3D12Objects(State::Instance().currentD3D12Device, State::Instance().currentCommandQueue,
                                      D3D12_COMMAND_LIST_TYPE_DIRECT);

        // Create FG SwapChain
        if (!_skipFGSwapChainCreation && !otherWindow)
        {
            ScopedSkipFGSCCreation skipFGSCCreation {};
            FGSCResult = FGHooks::CreateSwapChain(realFactory, real, &localDesc, ppSwapChain);

            if (FGSCResult == S_OK)
            {
                State::Instance().currentSwapchainDesc = localDesc;
                return FGSCResult;
            }
        }
    }
    else
    {
        LOG_INFO("Failed to get ID3D12CommandQueue from pDevice, creating Dx11 swapchain!");

        ID3D11Device* device = nullptr;

        if (pDevice->QueryInterface(IID_PPV_ARGS(&device)) == S_OK)
        {
            D3D11Hooks::HookToDevice(device);
            device->Release();

            // Update current D3D11 device and adapter
            if (State::Instance().currentD3D11Device != device)
            {
                if (!otherWindow)
                    State::Instance().currentD3D11Device = device;

                IDXGIDevice* dxgiDevice = nullptr;
                if (device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)) == S_OK)
                {
                    IDXGIAdapter* adapter = nullptr;
                    if (dxgiDevice->GetAdapter(&adapter) == S_OK)
                        adapter->Release();

                    dxgiDevice->Release();
                }
            }

            // Legacy DX11 FG path intentionally removed.
            // DX11 FG must now go through Dx11wDx12SC; if interop creation failed, fall back to the normal wrapper path
            // below.
        }
    }

    HRESULT result = E_FAIL;

    // If FG is disabled or call is coming from FG library
    // Create the DXGI SwapChain and wrap it
    if (_skipFGSwapChainCreation || FGSCResult != S_OK)
    {
        SdrToHdr::NoteTwinStandDown();

        // !_skipFGSwapChainCreation for preventing early enablement flags
        if (!_skipFGSwapChainCreation)
        {
            State::Instance().skipDxgiLoadChecks = true;

            if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
                State::Instance().skipHeapCapture = true;
        }

        {
            ScopedSkipParentWrapping skipParentWrapping {};
            result = o_DLSSGCreateSwapChain(realFactory, pDevice, &localDesc, ppSwapChain);
        }

        if (!_skipFGSwapChainCreation)
        {
            if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
                State::Instance().skipHeapCapture = false;

            State::Instance().skipDxgiLoadChecks = false;
        }

        if (SwapchainCreate::Created(result, ppSwapChain))
        {
            if (!otherWindow)
                State::Instance().currentSwapchainDesc = localDesc;

            // Check for SL proxy
            IDXGISwapChain* realSC = nullptr;
            if (!Util::CheckForRealObject(__FUNCTION__, *ppSwapChain, (IUnknown**) &realSC))
                realSC = *ppSwapChain;

            if (!otherWindow)
                State::Instance().currentRealSwapchain = realSC;

            IUnknown* realDevice = nullptr;
            if (!Util::CheckForRealObject(__FUNCTION__, pDevice, (IUnknown**) &realDevice))
                realDevice = pDevice;

            if (!otherWindow && Util::GetProcessWindow() == localDesc.OutputWindow)
            {
                State::Instance().screenWidth = static_cast<float>(localDesc.BufferDesc.Width);
                State::Instance().screenHeight = static_cast<float>(localDesc.BufferDesc.Height);
            }

            if (!otherWindow)
                DxgiFactoryHooks::NoteCreateStatus(result, realSC);

            LOG_DEBUG("Created new swapchain: {0:X}, hWnd: {1:X}", (UINT64) *ppSwapChain,
                      (UINT64) localDesc.OutputWindow);

            WrappedIDXGISwapChain4* wrapped;
            if ((*ppSwapChain)->QueryInterface(IID_PPV_ARGS(&wrapped)) != S_OK)
            {
                *ppSwapChain =
                    new WrappedIDXGISwapChain4(realSC, realDevice, localDesc.OutputWindow, localDesc.Flags, false);
                if (otherWindow)
                    static_cast<WrappedIDXGISwapChain4*>(*ppSwapChain)->MarkOtherWindow();

                // Set as currentSwapchain is FG is disabled
                if (!_skipFGSwapChainCreation && !otherWindow)
                    State::Instance().currentSwapchain = *ppSwapChain;

                if (!otherWindow)
                    State::Instance().currentWrappedSwapchain = *ppSwapChain;

                LOG_DEBUG("Created new WrappedIDXGISwapChain4: {:X}, pDevice: {:X}", (size_t) *ppSwapChain,
                          (size_t) pDevice);
            }
            else
            {
                wrapped->Release();
            }
        }
    }
    else
    {
        LOG_ERROR("CreateSwapChain failed: {:X}", (UINT) result);
    }

    return result;
}

HRESULT DxgiFactoryHooks::DLSSGCreateSwapChainForHwnd(IDXGIFactory2* realFactory, IUnknown* pDevice, HWND hWnd,
                                                      const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                                      IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain)
{
    *ppSwapChain = nullptr;

    static bool firstCall = static_cast<bool>(State::Instance().gameQuirks & GameQuirk::NoFSRFGFirstSwapchain);
    if (firstCall)
    {
        LOG_DEBUG("Skipping FG swapchain creation");
        _skipFGSwapChainCreation = true;
    }

    if (State::Instance().vulkanCreatingSC)
    {
        LOG_WARN("Vulkan is creating swapchain!");
        HRESULT result;

        {
            ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
            ScopedSkipParentWrapping skipParentWrapping {};

            result = o_DLSSGCreateSwapChainForHwnd(realFactory, pDevice, hWnd, pDesc, pFullscreenDesc,
                                                   pRestrictToOutput, ppSwapChain);
        }

        if (firstCall)
            _skipFGSwapChainCreation = false;

        return result;
    }

    if (pDevice == nullptr || pDesc == nullptr)
    {
        LOG_WARN("pDevice or pDesc is nullptr!");
        HRESULT result;

        {
            ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
            ScopedSkipParentWrapping skipParentWrapping {};
            result = o_DLSSGCreateSwapChainForHwnd(realFactory, pDevice, hWnd, pDesc, pFullscreenDesc,
                                                   pRestrictToOutput, ppSwapChain);
        }

        if (firstCall)
            _skipFGSwapChainCreation = false;

        return result;
    }

    if (pDesc->Height < 100 || pDesc->Width < 100)
    {
        LOG_WARN("Overlay call! Width: {}, Height: {}", pDesc->Width, pDesc->Height);
        HRESULT result;

        {
            ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
            ScopedSkipParentWrapping skipParentWrapping {};
            result = o_DLSSGCreateSwapChainForHwnd(realFactory, pDevice, hWnd, pDesc, pFullscreenDesc,
                                                   pRestrictToOutput, ppSwapChain);
        }

        if (firstCall)
            _skipFGSwapChainCreation = false;

        return result;
    }

    DXGI_SWAP_CHAIN_DESC1 localDesc {};
    memcpy(&localDesc, pDesc, sizeof(DXGI_SWAP_CHAIN_DESC1));

    LOG_DEBUG("Width: {}, Height: {}, Format: {}, Count: {}, Flags: {:X}, Hwnd: {:X}, SkipWrapping: {}",
              localDesc.Width, localDesc.Height, (UINT) localDesc.Format, localDesc.BufferCount, localDesc.Flags,
              (size_t) hWnd, _skipFGSwapChainCreation);

    if (pFullscreenDesc != nullptr)
        State::Instance().realExclusiveFullscreen = !pFullscreenDesc->Windowed;

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC localFullscreenDesc {};

    if (pFullscreenDesc != nullptr)
        memcpy(&localFullscreenDesc, pFullscreenDesc, sizeof(DXGI_SWAP_CHAIN_FULLSCREEN_DESC));

    if (UsesXeFGApi(State::Instance().activeFgOutput) && FGHooks::BorderlessForced())
    {
        if (pFullscreenDesc != nullptr && !localFullscreenDesc.Windowed)
        {

            State::Instance().SCExclusiveFullscreen = true;
            localFullscreenDesc.Windowed = true;
        }

        localDesc.Flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        localDesc.Scaling = DXGI_SCALING_STRETCH;
    }

    // For vsync override
    if (pFullscreenDesc != nullptr && !localFullscreenDesc.Windowed)
    {
        LOG_INFO("Game is creating fullscreen swapchain, disabled V-Sync overrides");
        Config::Instance()->OverrideVsync.set_volatile_value(false);
    }

    if (Config::Instance()->OverrideVsync.value_or_default())
    {
        localDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        localDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

        if (localDesc.BufferCount < 2)
            localDesc.BufferCount = 2;
    }

    State::Instance().SCAllowTearing = (localDesc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) > 0;
    State::Instance().SCLastFlags = localDesc.Flags;
    State::Instance().realExclusiveFullscreen = pFullscreenDesc != nullptr && !localFullscreenDesc.Windowed;

#ifdef VER_PRE_RELEASE
    LOG_TRACE("localDesc.AlphaMode : {}", magic_enum::enum_name(localDesc.AlphaMode));
    LOG_TRACE("localDesc.BufferCount : {}", localDesc.BufferCount);
    LOG_TRACE("localDesc.BufferUsage : {}", localDesc.BufferUsage);
    LOG_TRACE("localDesc.Flags : {}", localDesc.Flags);
    LOG_TRACE("localDesc.Format : {}", magic_enum::enum_name(localDesc.Format));
    LOG_TRACE("localDesc.Height : {}", localDesc.Height);
    LOG_TRACE("localDesc.SampleDesc.Count : {}", localDesc.SampleDesc.Count);
    LOG_TRACE("localDesc.SampleDesc.Quality : {}", localDesc.SampleDesc.Quality);
    LOG_TRACE("localDesc.Scaling : {}", magic_enum::enum_name(localDesc.Scaling));
    LOG_TRACE("localDesc.Stereo : {}", localDesc.Stereo);

    if (pFullscreenDesc != nullptr)
    {
        LOG_TRACE("localFullscreenDesc.RefreshRate.Denominator : {}", localFullscreenDesc.RefreshRate.Denominator);
        LOG_TRACE("localFullscreenDesc.RefreshRate.Numerator : {}", localFullscreenDesc.RefreshRate.Numerator);
        LOG_TRACE("localFullscreenDesc.Scaling : {}", magic_enum::enum_name(localFullscreenDesc.Scaling));
        LOG_TRACE("localFullscreenDesc.ScanlineOrdering : {}",
                  magic_enum::enum_name(localFullscreenDesc.ScanlineOrdering));
        LOG_TRACE("localFullscreenDesc.Windowed : {}", localFullscreenDesc.Windowed);
    }
#endif

    // Check for SL proxy, get real queue
    ID3D12CommandQueue* cq = nullptr;
    IUnknown* real = nullptr;
    HRESULT FGSCResult = E_NOTIMPL;

    // Another window while frame generation's swapchain lives: a plain swapchain, frame generation's
    // state stays as it is (framegen/FgWindowRule.h)
    const bool otherWindow = FGHooks::IsOtherWindowThanFG(hWnd);

    if (pDevice->QueryInterface(IID_PPV_ARGS(&cq)) == S_OK)
    {
        if (State::Instance().currentD3D12Device == nullptr)
        {
            ID3D12Device* device = nullptr;
            if (cq->GetDevice(IID_PPV_ARGS(&device)) == S_OK)
            {
                if (device != nullptr)
                {
                    // Update current D3D12 device and adapter
                    if (State::Instance().currentD3D12Device != device)
                    {
                        State::Instance().currentD3D12Device = device;

                        IDXGIDevice* dxgiDevice = nullptr;
                        if (device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)) == S_OK)
                        {
                            IDXGIAdapter* adapter = nullptr;
                            if (dxgiDevice->GetAdapter(&adapter) == S_OK)
                                adapter->Release();

                            dxgiDevice->Release();
                        }
                    }

                    LOG_INFO("Captured D3D12 device from command queue: {:X}", (UINT64) device);
                    D3D12Hooks::HookDevice(State::Instance().currentD3D12Device);
                }
            }
        }

        if (!Util::CheckForRealObject(__FUNCTION__, cq, &real))
            real = cq;

        if (!otherWindow)
            State::Instance().currentCommandQueue = (ID3D12CommandQueue*) real;

        if (!otherWindow && State::Instance().currentD3D12Device != nullptr)
            WithDx12::SetD3D12Objects(State::Instance().currentD3D12Device, State::Instance().currentCommandQueue,
                                      D3D12_COMMAND_LIST_TYPE_DIRECT);

        // Create FG SwapChain
        if (!_skipFGSwapChainCreation && !otherWindow)
        {
            ScopedSkipFGSCCreation skipFGSCCreation {};
            FGSCResult = FGHooks::CreateSwapChainForHwnd(realFactory, real, hWnd, &localDesc,
                                                         pFullscreenDesc != nullptr ? &localFullscreenDesc : nullptr,
                                                         pRestrictToOutput, ppSwapChain);

            if (FGSCResult == S_OK)
            {
                ((IDXGISwapChain*) *ppSwapChain)->GetDesc(&State::Instance().currentSwapchainDesc);
                return FGSCResult;
            }
        }
    }
    else
    {
        LOG_INFO("Failed to get ID3D12CommandQueue from pDevice, creating Dx11 swapchain!");

        ID3D11Device* device = nullptr;

        if (pDevice->QueryInterface(IID_PPV_ARGS(&device)) == S_OK)
        {
            D3D11Hooks::HookToDevice(device);

            device->Release();

            // Update current D3D11 device and adapter
            if (State::Instance().currentD3D11Device != device)
            {
                if (!otherWindow)
                    State::Instance().currentD3D11Device = device;

                IDXGIDevice* dxgiDevice = nullptr;
                if (device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)) == S_OK)
                {
                    IDXGIAdapter* adapter = nullptr;
                    if (dxgiDevice->GetAdapter(&adapter) == S_OK)
                        adapter->Release();

                    dxgiDevice->Release();
                }
            }

            // Legacy DX11 FG path intentionally removed.
            // DX11 FG must now go through Dx11wDx12SC; if interop creation failed, fall back to the normal wrapper path
            // below.
        }
    }

    HRESULT result = E_FAIL;

    // If FG is disabled or call is coming from FG library
    // Create the DXGI SwapChain and wrap it
    if (_skipFGSwapChainCreation || FGSCResult != S_OK)
    {
        SdrToHdr::NoteTwinStandDown();

        // !_skipFGSwapChainCreation for preventing early enablement flags
        if (!_skipFGSwapChainCreation)
        {
            State::Instance().skipDxgiLoadChecks = true;

            if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
                State::Instance().skipHeapCapture = true;
        }

        {
            ScopedSkipParentWrapping skipParentWrapping {};
            result = o_DLSSGCreateSwapChainForHwnd(realFactory, pDevice, hWnd, &localDesc,
                                                   pFullscreenDesc != nullptr ? &localFullscreenDesc : nullptr,
                                                   pRestrictToOutput, ppSwapChain);
        }

        if (!_skipFGSwapChainCreation)
        {
            if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
                State::Instance().skipHeapCapture = false;

            State::Instance().skipDxgiLoadChecks = false;
        }

        if (SwapchainCreate::Created(result, ppSwapChain))
        {
            // check for SL proxy
            IDXGISwapChain1* realSC = nullptr;
            if (!Util::CheckForRealObject(__FUNCTION__, *ppSwapChain, (IUnknown**) &realSC))
                realSC = *ppSwapChain;

            if (!otherWindow)
                State::Instance().currentRealSwapchain = realSC;

            IUnknown* readDevice = nullptr;
            if (!Util::CheckForRealObject(__FUNCTION__, pDevice, (IUnknown**) &readDevice))
                readDevice = pDevice;

            if (!otherWindow && Util::GetProcessWindow() == hWnd)
            {
                State::Instance().screenWidth = static_cast<float>(localDesc.Width);
                State::Instance().screenHeight = static_cast<float>(localDesc.Height);
            }

            if (!otherWindow)
                realSC->GetDesc(&State::Instance().currentSwapchainDesc);

            if (!otherWindow)
                DxgiFactoryHooks::NoteCreateStatus(result, realSC);

            LOG_DEBUG("Created new swapchain: {0:X}, hWnd: {1:X}", (uintptr_t) *ppSwapChain, (uintptr_t) hWnd);

            WrappedIDXGISwapChain4* wrapped;
            if ((*ppSwapChain)->QueryInterface(IID_PPV_ARGS(&wrapped)) != S_OK)
            {
                *ppSwapChain = new WrappedIDXGISwapChain4(realSC, readDevice, hWnd, localDesc.Flags, false);
                if (otherWindow)
                    static_cast<WrappedIDXGISwapChain4*>(*ppSwapChain)->MarkOtherWindow();
                LOG_DEBUG("Created new WrappedIDXGISwapChain4: {0:X}, pDevice: {1:X}", (uintptr_t) *ppSwapChain,
                          (uintptr_t) pDevice);

                if (!_skipFGSwapChainCreation && !otherWindow)
                    State::Instance().currentSwapchain = *ppSwapChain;

                if (!otherWindow)
                    State::Instance().currentWrappedSwapchain = *ppSwapChain;
            }
            else
            {
                wrapped->Release();
            }
        }
        else
        {
            LOG_ERROR("CreateSwapChainForHwnd failed: {:X}", (UINT) result);
        }
    }

    if (firstCall)
    {
        LOG_DEBUG("Unsetting skip FG swapchain creation");
        _skipFGSwapChainCreation = false;
        firstCall = false;
    }

    return result;
}

HRESULT DxgiFactoryHooks::DLSSGCreateSwapChainForCoreWindow(IDXGIFactory2* realFactory, IUnknown* pDevice,
                                                            IUnknown* pWindow, const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                            IDXGIOutput* pRestrictToOutput,
                                                            IDXGISwapChain1** ppSwapChain)
{
    if (State::Instance().vulkanCreatingSC)
    {
        LOG_WARN("Vulkan is creating swapchain!");

        if (pDesc != nullptr)
            LOG_DEBUG("Width: {}, Height: {}, Format: {}, Flags: {:X}, Count: {}, SkipWrapping: {}", pDesc->Width,
                      pDesc->Height, (UINT) pDesc->Format, pDesc->Flags, pDesc->BufferCount, _skipFGSwapChainCreation);

        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        return realFactory->CreateSwapChainForCoreWindow(pDevice, pWindow, pDesc, pRestrictToOutput, ppSwapChain);
    }

    if (pDevice == nullptr || pDesc == nullptr)
    {
        LOG_WARN("pDevice or pDesc is nullptr!");
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        return realFactory->CreateSwapChainForCoreWindow(pDevice, pWindow, pDesc, pRestrictToOutput, ppSwapChain);
    }

    if (pDesc->Height < 100 || pDesc->Width < 100)
    {
        LOG_WARN("Overlay call! Width: {}, Height: {}", pDesc->Width, pDesc->Height);

        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        return realFactory->CreateSwapChainForCoreWindow(pDevice, pWindow, pDesc, pRestrictToOutput, ppSwapChain);
    }

    DXGI_SWAP_CHAIN_DESC1 localDesc {};
    memcpy(&localDesc, pDesc, sizeof(DXGI_SWAP_CHAIN_DESC1));

    LOG_DEBUG("Width: {}, Height: {}, Format: {}, Count: {}, SkipWrapping: {}", localDesc.Width, localDesc.Height,
              (UINT) localDesc.Format, localDesc.BufferCount, _skipFGSwapChainCreation);

    // For vsync override
    if (Config::Instance()->OverrideVsync.value_or_default())
    {
        localDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        localDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

        if (localDesc.BufferCount < 2)
            localDesc.BufferCount = 2;
    }

    State::Instance().SCAllowTearing = (localDesc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) > 0;
    State::Instance().SCLastFlags = localDesc.Flags;
    State::Instance().realExclusiveFullscreen = false;

    ID3D12CommandQueue* cq = nullptr;
    IUnknown* real = nullptr;
    if (pDevice->QueryInterface(IID_PPV_ARGS(&cq)) == S_OK)
    {
        if (!Util::CheckForRealObject(__FUNCTION__, cq, &real))
            real = cq;

        State::Instance().currentCommandQueue = (ID3D12CommandQueue*) real;

        if (State::Instance().currentD3D12Device != nullptr)
            WithDx12::SetD3D12Objects(State::Instance().currentD3D12Device, State::Instance().currentCommandQueue,
                                      D3D12_COMMAND_LIST_TYPE_DIRECT);
    }

    SdrToHdr::NoteTwinStandDown();

    HRESULT result = E_FAIL;
    {
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        auto result = o_DLSSGCreateSwapChainForCoreWindow(realFactory, pDevice, pWindow, &localDesc, pRestrictToOutput,
                                                          ppSwapChain);
    }

    if (SwapchainCreate::Created(result, ppSwapChain))
    {
        // check for SL proxy
        IDXGISwapChain* realSC = nullptr;
        if (!Util::CheckForRealObject(__FUNCTION__, *ppSwapChain, (IUnknown**) &realSC))
            realSC = *ppSwapChain;

        State::Instance().currentRealSwapchain = realSC;

        IUnknown* readDevice = nullptr;
        if (!Util::CheckForRealObject(__FUNCTION__, pDevice, (IUnknown**) &readDevice))
            readDevice = pDevice;

        realSC->GetDesc(&State::Instance().currentSwapchainDesc);

        State::Instance().screenWidth = static_cast<float>(localDesc.Width);
        State::Instance().screenHeight = static_cast<float>(localDesc.Height);

        LOG_DEBUG("Created new swapchain: {0:X}, hWnd: {1:X}", (UINT64) *ppSwapChain, (UINT64) pWindow);

        WrappedIDXGISwapChain4* wrapped;
        if ((*ppSwapChain)->QueryInterface(IID_PPV_ARGS(&wrapped)) != S_OK)
        {
            *ppSwapChain = new WrappedIDXGISwapChain4(realSC, readDevice, (HWND) pWindow, localDesc.Flags, true);

            if (!_skipFGSwapChainCreation)
                State::Instance().currentSwapchain = *ppSwapChain;

            State::Instance().currentWrappedSwapchain = *ppSwapChain;

            LOG_DEBUG("Created new WrappedIDXGISwapChain4: {0:X}, pDevice: {1:X}", (UINT64) *ppSwapChain,
                      (UINT64) pDevice);
        }
        else
        {
            wrapped->Release();
        }
    }

    return result;
}

HRESULT DxgiFactoryHooks::EnumAdapters(IDXGIFactory* realFactory, UINT Adapter, IDXGIAdapter** ppAdapter)
{
    HRESULT result = S_FALSE;

    if (State::Instance().skipDxgiLoadChecks)
        return o_EnumAdapters(realFactory, Adapter, ppAdapter);

    if (Config::Instance()->PreferFirstDedicatedGpu.value_or_default() && Adapter > 0)
    {
        LOG_DEBUG("{}, returning not found", Adapter);
        return DXGI_ERROR_NOT_FOUND;
    }

    IDXGIFactory6* factory6 = nullptr;
    if (realFactory->QueryInterface(IID_PPV_ARGS(&factory6)) == S_OK && factory6 != nullptr)
    {
        auto allGpus = IdentifyGpu::getAllGpus();
        if (Adapter < allGpus.size())
        {
            LOG_DEBUG("Trying to select: {}", allGpus[Adapter].name);

            auto gpuLuid = allGpus[Adapter].luid;

            ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
            result = o_EnumAdapterByLuid(factory6, gpuLuid, __uuidof(IDXGIAdapter), (void**) ppAdapter);
        }
        else
            result = DXGI_ERROR_NOT_FOUND;

        factory6->Release();
    }

    if (result != S_OK && result != DXGI_ERROR_NOT_FOUND)
    {
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        result = o_EnumAdapters(realFactory, Adapter, ppAdapter);
    }

    if (result == S_OK)
        DxgiSpoofing::AttachToAdapter(*ppAdapter);

#if _DEBUG
    LOG_TRACE("result: {:X}, Adapter: {}, pAdapter: {:X}", (UINT) result, Adapter, (uintptr_t) *ppAdapter);
#endif

    return result;
}

HRESULT DxgiFactoryHooks::EnumAdapters1(IDXGIFactory1* realFactory, UINT Adapter, IDXGIAdapter1** ppAdapter)
{
    HRESULT result = S_FALSE;

    if (State::Instance().skipDxgiLoadChecks)
        return o_EnumAdapters1(realFactory, Adapter, ppAdapter);

    if (Config::Instance()->PreferFirstDedicatedGpu.value_or_default() && Adapter > 0)
    {
        LOG_DEBUG("{}, returning not found", Adapter);
        return DXGI_ERROR_NOT_FOUND;
    }

    IDXGIFactory6* factory6 = nullptr;
    if (realFactory->QueryInterface(IID_PPV_ARGS(&factory6)) == S_OK && factory6 != nullptr)
    {
        auto allGpus = IdentifyGpu::getAllGpus();
        if (Adapter < allGpus.size())
        {
            LOG_DEBUG("Trying to select: {}", allGpus[Adapter].name);

            auto gpuLuid = allGpus[Adapter].luid;

            ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
            result = o_EnumAdapterByLuid(factory6, gpuLuid, __uuidof(IDXGIAdapter), (void**) ppAdapter);
        }
        else
            result = DXGI_ERROR_NOT_FOUND;

        factory6->Release();
    }

    if (result != S_OK && result != DXGI_ERROR_NOT_FOUND)
    {
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        result = o_EnumAdapters1(realFactory, Adapter, ppAdapter);
    }

    if (result == S_OK)
        DxgiSpoofing::AttachToAdapter(*ppAdapter);

#if _DEBUG
    LOG_TRACE("result: {:X}, Adapter: {}, pAdapter: {:X}", (UINT) result, Adapter, (uintptr_t) *ppAdapter);
#endif

    return result;
}

HRESULT DxgiFactoryHooks::EnumAdapterByLuid(IDXGIFactory4* realFactory, LUID AdapterLuid, REFIID riid,
                                            void** ppvAdapter)
{
    HRESULT result;
    {
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        result = o_EnumAdapterByLuid(realFactory, AdapterLuid, riid, ppvAdapter);
    }

    if (result == S_OK)
        DxgiSpoofing::AttachToAdapter((IUnknown*) *ppvAdapter);

#if _DEBUG
    LOG_TRACE("result: {:X}, pAdapter: {:X}", (UINT) result, (uintptr_t) *ppvAdapter);
#endif

    return result;
}

HRESULT DxgiFactoryHooks::EnumAdapterByGpuPreference(IDXGIFactory6* realFactory, UINT Adapter,
                                                     DXGI_GPU_PREFERENCE GpuPreference, REFIID riid, void** ppvAdapter)
{
    HRESULT result;

    {
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        result = o_EnumAdapterByGpuPreference(realFactory, Adapter, GpuPreference, riid, ppvAdapter);
    }

    // Log because that's not something we usually expect
    if (GpuPreference & DXGI_GPU_PREFERENCE_MINIMUM_POWER)
        LOG_WARN("Game asked for minimum power GPU");

    if (result == S_OK)
        DxgiSpoofing::AttachToAdapter((IUnknown*) *ppvAdapter);

#if _DEBUG
    LOG_TRACE("result: {:X}, Adapter: {}, pAdapter: {:X}", (UINT) result, Adapter, (uintptr_t) *ppvAdapter);
#endif

    return result;
}
