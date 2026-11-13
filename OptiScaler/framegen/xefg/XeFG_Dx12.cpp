#include "pch.h"
#include "XeFG_Dx12.h"
#include <hudfix/Hudfix_Dx12.h>
#include <menu/menu_overlay_dx.h>
#include <misc/REFrameworkCompat.h>
#include <resource_tracking/ResTrack_dx12.h>

#include <hooks/Reflex_Hooks.h>
#include <native/NativeLowLatency.h>
#include <nvapi/fakenvapi.h>
#include <low_latency/input/input_common.h>
#include <misc/FrameLimit.h>
#include <framegen/mfgunlock/MfgUnlock.h>
#include <framegen/f5low/F5Low.h>
#include <imgui/ImGuiNotify.hpp>
#include <framegen/GameFsrFgRule.h>

#include <magic_enum.hpp>

struct XellFrameIdAndSource
{
    uint32_t id = 0;
    xefg::XellIdSource source = xefg::XellIdSource::None;
};

static XellFrameIdAndSource XellFrameId();

// 1: the game's window is in front, 0: minimised or another process has the focus, -1: not known right now
static int GameWindowFocus(HWND window)
{
    // Unknown window: nothing to tell, keep deciding
    if (window == nullptr || !IsWindow(window))
        return 1;

    if (IsIconic(GetAncestor(window, GA_ROOT)))
        return 0;

    const auto foreground = GetForegroundWindow();

    if (foreground == nullptr)
        return -1;

    if (GetAncestor(foreground, GA_ROOT) == GetAncestor(window, GA_ROOT))
        return 1;

    // Any window of the game's process counts, a game can present to one window while another has the focus
    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    return pid == GetCurrentProcessId() ? 1 : 0;
}

#include <DirectXMath.h>

using namespace DirectX;

void XeFG_Dx12::xefgLogCallback(const char* message, xefg_swapchain_logging_level_t level, void* userData)
{
    switch (level)
    {
    case XEFG_SWAPCHAIN_LOGGING_LEVEL_DEBUG:
        spdlog::debug("XeFG Log: {}", message);
        return;

    case XEFG_SWAPCHAIN_LOGGING_LEVEL_INFO:
        spdlog::info("XeFG Log: {}", message);
        return;

    case XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING:
        spdlog::warn("XeFG Log: {}", message);
        return;

    default:
        spdlog::error("XeFG Log: {}", message);
        return;
    }
}

bool XeFG_Dx12::CreateSwapchainContext(ID3D12Device* device)
{
    if (XeFGProxy::Module() == nullptr && !XeFGProxy::InitXeFG())
    {
        if (State::Instance().activeFgOutput == FGOutput::F5Low)
            F5Low::ReportFailure("OptiScaler\\plugins\\f5low.dll could not be loaded");
        else
            LOG_ERROR("XeFG proxy can't find libxess_fg.dll!");
        return false;
    }

    // F5Low runs our f5low.dll: the XeFG unlock patches Intel's library only
    const bool f5low = State::Instance().activeFgOutput == FGOutput::F5Low;
    if (f5low)
        F5Low::ApplyGenerator();
    else if (!MfgUnlock::PrepareXeFG())
    {
        LOG_ERROR("XeFG MFG unlock recovery failed; refusing to create a context with partially patched code");
        return false;
    }

    auto createResult = false;

#ifndef DONT_USE_XMX
    ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
#endif // !DONT_USE_XMX

    do
    {
        auto result = XeFGProxy::D3D12CreateContext()(device, &_swapChainContext);
        MfgUnlock::RefreshXeFGStatus();

        if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            LOG_ERROR("D3D12CreateContext error: {} ({})", magic_enum::enum_name(result), (UINT) result);

            if (f5low)
                F5Low::ReportFailure("f5low.dll could not create its frame generation context");
            return false;
        }

        LOG_INFO("XeFG context created");
        if (f5low)
            F5Low::LogStatus();
        result = XeFGProxy::SetLoggingCallback()(_swapChainContext, XEFG_SWAPCHAIN_LOGGING_LEVEL_DEBUG, xefgLogCallback,
                                                 nullptr);

        if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            LOG_ERROR("SetLoggingCallback error: {} ({})", magic_enum::enum_name(result), (UINT) result);
        }

        // XeFG paces with OptiScaler's XeLL (InputCommon): the low latency output itself while XeLL lowers latency,
        // else next to the Reflex or Anti-Lag 2 output with its low latency off. It stays until this context is
        // destroyed.
        if (auto xellContext = InputCommon::acquire_xefg_xell(device); xellContext != nullptr)
        {
            result = XeFGProxy::SetLatencyReduction()(_swapChainContext, xellContext);

            if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
            {
                LOG_ERROR("SetLatencyReduction error: {} ({})", magic_enum::enum_name(result), (UINT) result);

                // No XeFG paces with it: it must not stay the output
                InputCommon::release_xefg_xell();
                return false;
            }
        }
        else
        {
            LOG_ERROR("Couldn't create XeLL");
            return false;
        }

        createResult = true;

    } while (false);

    return createResult;
}

void XeFG_Dx12::ApplyMaxInterpolations(uint32_t reported)
{
    // F5Low: what f5low.dll reports (up to 6x), capped by [XeFG] MaxInterpolations
    if (State::Instance().activeFgOutput == FGOutput::F5Low)
    {
        _nativeMaxInterpolations = reported < 1 ? 1 : static_cast<int>(reported);
        const int configured = Config::Instance()->FGXeFGMaxInterpolations.value_or_default();
        _maxInterpolationCount = F5Low::MaxInterpolations(reported, configured);
        LOG_INFO("Max supported interpolations: {} (F5Low library reports {}, MaxInterpolations {})",
                 _maxInterpolationCount, reported, configured);
        F5Low::LogStatus();
        return;
    }

    // Without the unlock the reported value is used as is, as before the unlock existed.
    if (!MfgUnlock::IsXeFGActive())
    {
        _nativeMaxInterpolations = static_cast<int>(reported);
        _maxInterpolationCount = _nativeMaxInterpolations;
        LOG_INFO("Max supported interpolations: {}", reported);
        return;
    }

    const int reportedMax = reported < 1 ? 1 : static_cast<int>(reported);
    _nativeMaxInterpolations = std::min(reportedMax, MfgUnlock::kXeFGOriginalDefaultMaxInterpolations);
    const int configured = std::clamp(Config::Instance()->FGXeFGMaxInterpolations.value_or_default(), 1, 5);
    _maxInterpolationCount = std::max(_nativeMaxInterpolations, configured);

    if (_maxInterpolationCount > _nativeMaxInterpolations)
        LOG_INFO("Max supported interpolations: {} (XeFG native maximum {}, raised by MaxInterpolations)",
                 _maxInterpolationCount, _nativeMaxInterpolations);
    else
        LOG_INFO("Max supported interpolations: {}", _maxInterpolationCount);
}

void XeFG_Dx12::LogAcceptedInterpolations()
{
    if (!MfgUnlock::IsXeFGActive() || XeFGProxy::D3D12GetInitializationParameters() == nullptr)
        return;

    xefg_swapchain_d3d12_init_params_t used {};
    if (XeFGProxy::D3D12GetInitializationParameters()(_swapChainContext, &used) == XEFG_SWAPCHAIN_RESULT_SUCCESS)
        LOG_INFO("XeFG initialised for up to {} interpolated frames (native maximum {})",
                 used.maxInterpolatedFrames, _nativeMaxInterpolations);
}

const char* XeFG_Dx12::Name()
{
    static std::string nameBuffer;
    const char* base = State::Instance().activeFgOutput == FGOutput::F5Low ? "F5Low" : "XeFG";

    if (_maxInterpolationCount == 1 || _framesToInterpolate < 0)
    {
        nameBuffer = base;
    }
    else
    {
        auto count = _framesToInterpolate + 1;
        nameBuffer = std::string(base) + " " + std::to_string(count) + "x";
    }

    return nameBuffer.c_str();
}

feature_version XeFG_Dx12::Version()
{
    if (XeFGProxy::InitXeFG())
    {
        auto ver = XeFGProxy::Version();
        return feature_version(ver.major, ver.minor, ver.patch);
    }

    return { 0, 0, 0 };
}

HWND XeFG_Dx12::Hwnd() { return _hwnd; }

bool XeFG_Dx12::DestroySwapchainContext()
{
    LOG_DEBUG("");

    if (_swapChainContext != nullptr && !State::Instance().isShuttingDown)
    {
        auto context = _swapChainContext;
        _swapChainContext = nullptr;

        auto result = XeFGProxy::Destroy()(context);

        LOG_INFO("Destroy result: {} ({})", magic_enum::enum_name(result), (UINT) result);

        // Set it back because context is not destroyed
        if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            _swapChainContext = context;
        }
        else
        {
            State::Instance().currentFGSwapchain = nullptr;

            // Its XeLL context isn't read anymore
            InputCommon::release_xefg_xell();
        }
    }

    return true;
}

xefg_swapchain_d3d12_resource_data_t XeFG_Dx12::GetResourceData(FG_ResourceType type, int index)
{
    if (index < 0)
    {
        index = GetIndex();
        LOG_WARN("GetResourceData called with -1 index, using current index: {}", index);
    }

    xefg_swapchain_d3d12_resource_data_t resourceParam = {};

    if (!_frameResources[index].contains(type))
    {
        LOG_WARN("Resource type not found: {} for index: {}", magic_enum::enum_name(type), index);
        return resourceParam;
    }

    auto fResource = &_frameResources[index].at(type);

    resourceParam.validity = (fResource->validity == FG_ResourceValidity::ValidNow)
                                 ? XEFG_SWAPCHAIN_RV_ONLY_NOW
                                 : XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;

    resourceParam.resourceBase = { fResource->left, fResource->top };
    resourceParam.resourceSize = { static_cast<uint32_t>(fResource->width), fResource->height };
    resourceParam.pResource = fResource->GetResource();
    resourceParam.incomingState = fResource->state;

    switch (type)
    {
    case FG_ResourceType::Depth:
        resourceParam.type = XEFG_SWAPCHAIN_RES_DEPTH;
        break;

    case FG_ResourceType::HudlessColor:
        resourceParam.type = XEFG_SWAPCHAIN_RES_HUDLESS_COLOR;
        break;

    case FG_ResourceType::UIColor:
        resourceParam.type = XEFG_SWAPCHAIN_RES_UI;
        break;

    case FG_ResourceType::Velocity:
        resourceParam.type = XEFG_SWAPCHAIN_RES_MOTION_VECTOR;
        break;
    default:
        LOG_WARN("Unsupported resource type: {}", magic_enum::enum_name(type));
        return xefg_swapchain_d3d12_resource_data_t {};
    }

    return resourceParam;
}

bool XeFG_Dx12::CreateSwapchain(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, DXGI_SWAP_CHAIN_DESC* desc,
                                IDXGISwapChain** swapChain, bool readyToRelease)
{
    if (State::Instance().currentFGSwapchain != nullptr && _hwnd == desc->OutputWindow)
    {
        if (Config::Instance()->FGPreserveSwapChain.value_or_default())
        {
            LOG_WARN("FG swapchain already created for the same output window!");
            auto result = State::Instance().currentFGSwapchain->ResizeBuffers(
                              desc->BufferCount, desc->BufferDesc.Width, desc->BufferDesc.Height,
                              desc->BufferDesc.Format, desc->Flags) == S_OK;

            *swapChain = State::Instance().currentFGSwapchain;
            return result;
        }
        // Game is creating new swapchain without releasing old one,
        // we need to release it to avoid errors
        else if (readyToRelease)
        {
            LOG_INFO("Releasing old swapchain");
            ReleaseSwapchain(_hwnd);
        }
        else
        {
            LOG_WARN("FG swapchain already exists for the same output window and is not ready to release!");
            return false;
        }
    }

    if (_swapChainContext == nullptr)
    {
        LOG_DEBUG("Creating swapchain context for the first time");

        if (State::Instance().currentD3D12Device == nullptr)
            return false;

        CreateSwapchainContext(State::Instance().currentD3D12Device);

        if (_swapChainContext == nullptr)
            return false;

        _width = desc->BufferDesc.Width;
        _height = desc->BufferDesc.Height;

        xefg_swapchain_properties_t props {};
        auto result = XeFGProxy::GetProperties()(_swapChainContext, &props);
        if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
            ApplyMaxInterpolations(props.maxSupportedInterpolations);
        else
        {
            LOG_ERROR("Can't get swapchain properties: {} ({})", magic_enum::enum_name(result), (UINT) result);
        }
    }

    IDXGIFactory* realFactory = nullptr;
    ID3D12CommandQueue* realQueue = nullptr;

    if (!CheckForRealObject(__FUNCTION__, factory, (IUnknown**) &realFactory))
        realFactory = factory;

    if (!CheckForRealObject(__FUNCTION__, cmdQueue, (IUnknown**) &realQueue))
        realQueue = cmdQueue;

    IDXGIFactory2* factory12 = nullptr;
    if (realFactory->QueryInterface(IID_PPV_ARGS(&factory12)) != S_OK)
        return false;

    factory12->Release();

    HWND hwnd = desc->OutputWindow;
    DXGI_SWAP_CHAIN_DESC1 scDesc {};

    scDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE; // No info
    scDesc.BufferCount = desc->BufferCount;
    scDesc.BufferUsage = desc->BufferUsage;
    scDesc.Flags = desc->Flags;
    scDesc.Format = desc->BufferDesc.Format;
    scDesc.Height = desc->BufferDesc.Height;
    scDesc.SampleDesc = desc->SampleDesc;

    switch (desc->BufferDesc.Scaling)
    {
    // DXGI_SCALING_ASPECT_RATIO_STRETCH is only valid for CoreWindow/composition swapchains; on an HWND swapchain
    // CreateSwapChainForHwnd rejects it with DXGI_ERROR_INVALID_CALL (seen in Prey, which asks for a CENTERED mode).
    case DXGI_MODE_SCALING_CENTERED:
        scDesc.Scaling = DXGI_SCALING_STRETCH;
        break;

    case DXGI_MODE_SCALING_STRETCHED:
        scDesc.Scaling = DXGI_SCALING_STRETCH;
        break;

    case DXGI_MODE_SCALING_UNSPECIFIED:
        scDesc.Scaling = DXGI_SCALING_NONE;
        break;
    }

    scDesc.Stereo = false; // No info
    scDesc.SwapEffect = desc->SwapEffect;
    scDesc.Width = desc->BufferDesc.Width;

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsDesc {};
    fsDesc.RefreshRate = desc->BufferDesc.RefreshRate;
    fsDesc.Scaling = desc->BufferDesc.Scaling;
    fsDesc.ScanlineOrdering = desc->BufferDesc.ScanlineOrdering;
    fsDesc.Windowed = desc->Windowed;

    xefg_swapchain_d3d12_init_params_t params {};

    int intTarget = _maxInterpolationCount;

    // For old libxess_fg versions we use max to control interpolation count
    if (XeFGProxy::SetNumInterpolatedFrames() == nullptr)
        intTarget = Config::Instance()->FGInterpolationCount(
            Config::Instance()->FGXeFGInterpolationCount.value_or_default(), _maxInterpolationCount);

    if (intTarget < 1 || intTarget > _maxInterpolationCount)
    {
        LOG_WARN("Invalid XeFG interpolation count: {}, max count: {}", intTarget, _maxInterpolationCount);

        intTarget = 1;
    }

    if (_framesToInterpolate > intTarget)
        Config::Instance()->FGXeFGInterpolationCount.set_volatile_value(intTarget);

    params.maxInterpolatedFrames = intTarget;

    // Force XeLL generates nothing: one interpolated frame is enough (this used to be overwritten by the line above)
    if (Config::Instance()->ForceXeLL.value_or_default())
        params.maxInterpolatedFrames = 1;

    params.initFlags = XEFG_SWAPCHAIN_INIT_FLAG_NONE;

    if (Config::Instance()->FGXeFGDepthInverted.value_or_default())
        params.initFlags |= XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH;

    if (Config::Instance()->FGXeFGJitteredMV.value_or_default())
        params.initFlags |= XEFG_SWAPCHAIN_INIT_FLAG_JITTERED_MV;

    if (Config::Instance()->FGXeFGHighResMV.value_or_default())
        params.initFlags |= XEFG_SWAPCHAIN_INIT_FLAG_HIGH_RES_MV;

    if (!Config::Instance()->FGUIPremultipliedAlpha.value_or_default())
        params.initFlags |= XEFG_SWAPCHAIN_INIT_FLAG_UITEXTURE_NOT_PREMUL_ALPHA;

    LOG_DEBUG("Inverted Depth: {}", Config::Instance()->FGXeFGDepthInverted.value_or_default());
    LOG_DEBUG("Jittered Velocity: {}", Config::Instance()->FGXeFGJitteredMV.value_or_default());
    LOG_DEBUG("High Res MV: {}", Config::Instance()->FGXeFGHighResMV.value_or_default());

    if (Config::Instance()->FGXeFGDepthInverted.value_or_default())
        _constants.flags |= FG_Flags::InvertedDepth;

    if (Config::Instance()->FGXeFGJitteredMV.value_or_default())
        _constants.flags |= FG_Flags::JitteredMVs;

    if (Config::Instance()->FGXeFGHighResMV.value_or_default())
        _constants.flags |= FG_Flags::DisplayResolutionMVs;

#ifndef DONT_USE_XMX
    ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
#endif // !DONT_USE_XMX

    xefg_swapchain_result_t result;
    result = XeFGProxy::D3D12InitFromSwapChainDesc()(_swapChainContext, hwnd, &scDesc, &fsDesc, realQueue, factory12,
                                                     &params);

    if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS && MfgUnlock::IsXeFGActive() &&
        params.maxInterpolatedFrames > static_cast<uint32_t>(_nativeMaxInterpolations))
    {
        LOG_WARN("XeFG refused {} interpolated frames ({}), retrying with its own maximum {}",
                 params.maxInterpolatedFrames, magic_enum::enum_name(result), _nativeMaxInterpolations);
        _maxInterpolationCount = _nativeMaxInterpolations;
        params.maxInterpolatedFrames = static_cast<uint32_t>(_nativeMaxInterpolations);
        result = XeFGProxy::D3D12InitFromSwapChainDesc()(_swapChainContext, hwnd, &scDesc, &fsDesc, realQueue,
                                                         factory12, &params);
    }

    if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        LOG_ERROR("D3D12InitFromSwapChainDesc error: {} ({:X})", magic_enum::enum_name(result), (UINT) result);
        return false;
    }

    LOG_INFO("XeFG swapchain created");
    LogAcceptedInterpolations();
    result = XeFGProxy::D3D12GetSwapChainPtr()(_swapChainContext, IID_PPV_ARGS(swapChain));
    if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        LOG_ERROR("D3D12GetSwapChainPtr error: {} ({})", magic_enum::enum_name(result), (UINT) result);
        return false;
    }

    // When forcing XeLL, always tell XeFG that FG is active, even tho we don't send anything
    if (State::Instance().activeFgInput == FGInput::ForceXeLL)
    {
        XeFGProxy::SetEnabled()(_swapChainContext, true);
    }

    _gameCommandQueue = realQueue;
    _swapChain = *swapChain;
    _hwnd = hwnd;

    return true;
}

bool XeFG_Dx12::CreateSwapchain1(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, HWND hwnd,
                                 DXGI_SWAP_CHAIN_DESC1* desc, DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                 IDXGISwapChain1** swapChain, bool readyToRelease)
{
    if (State::Instance().currentFGSwapchain != nullptr && _hwnd == hwnd)
    {
        if (Config::Instance()->FGPreserveSwapChain.value_or_default())
        {
            LOG_WARN("FG swapchain already created for the same output window!");
            auto result = State::Instance().currentFGSwapchain->ResizeBuffers(
                              desc->BufferCount, desc->Width, desc->Height, desc->Format, desc->Flags) == S_OK;

            *swapChain = (IDXGISwapChain1*) State::Instance().currentFGSwapchain;
            return result;
        }
        // Game is creating new swapchain without releasing old one,
        // we need to release it to avoid errors
        else if (readyToRelease)
        {
            LOG_INFO("Releasing old swapchain");
            ReleaseSwapchain(_hwnd);
        }
        else
        {
            LOG_WARN("FG swapchain already exists for the same output window and is not ready to release!");
            return false;
        }
    }

    if (_swapChainContext == nullptr)
    {
        if (State::Instance().currentD3D12Device == nullptr)
            return false;

        CreateSwapchainContext(State::Instance().currentD3D12Device);

        if (_swapChainContext == nullptr)
            return false;

        _width = desc->Width;
        _height = desc->Height;

        xefg_swapchain_properties_t props {};
        auto result = XeFGProxy::GetProperties()(_swapChainContext, &props);
        if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
            ApplyMaxInterpolations(props.maxSupportedInterpolations);
        else
        {
            LOG_ERROR("Can't get swapchain properties: {} ({})", magic_enum::enum_name(result), (UINT) result);
        }
    }

    IDXGIFactory* realFactory = nullptr;
    ID3D12CommandQueue* realQueue = nullptr;

    if (!CheckForRealObject(__FUNCTION__, factory, (IUnknown**) &realFactory))
        realFactory = factory;

    if (!CheckForRealObject(__FUNCTION__, cmdQueue, (IUnknown**) &realQueue))
        realQueue = cmdQueue;

    IDXGIFactory2* factory12 = nullptr;
    if (realFactory->QueryInterface(IID_PPV_ARGS(&factory12)) != S_OK)
        return false;

    factory12->Release();

    xefg_swapchain_d3d12_init_params_t params {};

    int intTarget = _maxInterpolationCount;

    // For old libxess_fg versions we use max to control interpolation count
    if (XeFGProxy::SetNumInterpolatedFrames() == nullptr)
        intTarget = Config::Instance()->FGInterpolationCount(
            Config::Instance()->FGXeFGInterpolationCount.value_or_default(), _maxInterpolationCount);

    if (intTarget < 1 || intTarget > _maxInterpolationCount)
    {
        LOG_WARN("Invalid XeFG interpolation count: {}, max count: {}", intTarget, _maxInterpolationCount);

        intTarget = 1;
    }

    if (_framesToInterpolate > intTarget)
        Config::Instance()->FGXeFGInterpolationCount.set_volatile_value(intTarget);

    params.maxInterpolatedFrames = intTarget;

    // Force XeLL generates nothing: one interpolated frame is enough (this used to be overwritten by the line above)
    if (Config::Instance()->ForceXeLL.value_or_default())
        params.maxInterpolatedFrames = 1;

    params.initFlags = XEFG_SWAPCHAIN_INIT_FLAG_NONE;

    if (Config::Instance()->FGXeFGDepthInverted.value_or_default())
        params.initFlags |= XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH;

    if (Config::Instance()->FGXeFGJitteredMV.value_or_default())
        params.initFlags |= XEFG_SWAPCHAIN_INIT_FLAG_JITTERED_MV;

    if (Config::Instance()->FGXeFGHighResMV.value_or_default())
        params.initFlags |= XEFG_SWAPCHAIN_INIT_FLAG_HIGH_RES_MV;

    if (!Config::Instance()->FGUIPremultipliedAlpha.value_or_default())
        params.initFlags |= XEFG_SWAPCHAIN_INIT_FLAG_UITEXTURE_NOT_PREMUL_ALPHA;

    LOG_DEBUG("Inverted Depth: {}", Config::Instance()->FGXeFGDepthInverted.value_or_default());
    LOG_DEBUG("Jittered Velocity: {}", Config::Instance()->FGXeFGJitteredMV.value_or_default());
    LOG_DEBUG("High Res MV: {}", Config::Instance()->FGXeFGHighResMV.value_or_default());

    if (Config::Instance()->FGXeFGDepthInverted.value_or_default())
        _constants.flags |= FG_Flags::InvertedDepth;

    if (Config::Instance()->FGXeFGJitteredMV.value_or_default())
        _constants.flags |= FG_Flags::JitteredMVs;

    if (Config::Instance()->FGXeFGHighResMV.value_or_default())
        _constants.flags |= FG_Flags::DisplayResolutionMVs;

    xefg_swapchain_result_t result;

    {
#ifndef DONT_USE_XMX
        ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
#endif // !DONT_USE_XMX
        result = XeFGProxy::D3D12InitFromSwapChainDesc()(_swapChainContext, hwnd, desc, pFullscreenDesc, realQueue,
                                                         factory12, &params);

        if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS && MfgUnlock::IsXeFGActive() &&
            params.maxInterpolatedFrames > static_cast<uint32_t>(_nativeMaxInterpolations))
        {
            LOG_WARN("XeFG refused {} interpolated frames ({}), retrying with its own maximum {}",
                     params.maxInterpolatedFrames, magic_enum::enum_name(result), _nativeMaxInterpolations);
            _maxInterpolationCount = _nativeMaxInterpolations;
            params.maxInterpolatedFrames = static_cast<uint32_t>(_nativeMaxInterpolations);
            result = XeFGProxy::D3D12InitFromSwapChainDesc()(_swapChainContext, hwnd, desc, pFullscreenDesc,
                                                             realQueue, factory12, &params);
        }
    }

    if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        LOG_ERROR("D3D12InitFromSwapChainDesc error: {} ({})", magic_enum::enum_name(result), (UINT) result);
        return false;
    }

    LOG_INFO("XeFG swapchain created");
    LogAcceptedInterpolations();
    result = XeFGProxy::D3D12GetSwapChainPtr()(_swapChainContext, IID_PPV_ARGS(swapChain));
    if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        LOG_ERROR("D3D12GetSwapChainPtr error: {} ({})", magic_enum::enum_name(result), (UINT) result);
        return false;
    }

    // When forcing XeLL, always tell XeFG that FG is active, even tho we don't send anything
    if (State::Instance().activeFgInput == FGInput::ForceXeLL)
    {
        XeFGProxy::SetEnabled()(_swapChainContext, true);
    }

    _gameCommandQueue = realQueue;
    _swapChain = *swapChain;
    _hwnd = hwnd;

    return true;
}

void XeFG_Dx12::CreateContext(ID3D12Device* device, FG_Constants& fgConstants)
{
    LOG_DEBUG("");

    _device = device;
    CreateObjects(device);

    if (_fgContext == nullptr && _swapChainContext != nullptr)
    {
        _fgContext = _swapChainContext;
        _lastDispatchedFrame = 0;
    }

    if (_isActive)
    {
        LOG_INFO("FG context recreated while active, pausing");
        State::Instance().fgChanged = true;
        UpdateTarget();
        Deactivate();
    }
}

void XeFG_Dx12::Activate()
{
    LOG_DEBUG("");

    // Our swapchain sits under the game's FSR FG one (FG_Hooks RefuseUnderGameFsrFg): both generating crashes
    if (GameFsrFg::StandsAside(State::Instance().xefgUnderGameFsrFg, State::Instance().gameFsrFgOn))
    {
        static bool shown = false;

        if (!shown)
        {
            shown = true;
            LOG_WARN("The game's FSR Frame Generation is on: XeFG stands aside until it is off");
            ImGui::InsertNotification({ ImGuiToastType::Warning, 15000,
                                        "The game's FSR Frame Generation is on: XeFG can't run under it.\n"
                                        "Turn the game's FSR frame generation off, or set FG Input to FSR-FG." });
        }

        return;
    }

    auto currentFeature = State::Instance().currentFeature;
    bool nativeAA = false;
    if (State::Instance().activeFgInput == FGInput::Upscaler && currentFeature != nullptr)
        nativeAA = currentFeature->RenderWidth() == currentFeature->DisplayWidth();

    if (_swapChainContext != nullptr && _fgContext != nullptr && !_isActive &&
        (IsLowResMV() || nativeAA || (State::Instance().gameQuirks & GameQuirk::ForceFGRenderSizeMVs) ||
         Config::Instance()->FGXeFGIgnoreInitChecks.value_or_default()))
    {
        auto result = XeFGProxy::SetEnabled()(_swapChainContext, true);

        if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            _isActive = true;
            _lastDispatchedFrame = 0;
            _generationOff = false;

            // FG back on: Dynamic MFG starts its averages over, keeps what it learned
            _dynamicCount.Restart();
            _dynamicCap.Reset();
            _dynamicDrain = true;
        }

        LOG_INFO("SetEnabled: true, result: {} ({})", magic_enum::enum_name(result), (UINT) result);
    }
}

void XeFG_Dx12::Deactivate()
{
    LOG_DEBUG("");

    if (_isActive)
    {
        auto fIndex = GetIndex();
        if (_uiCommandListResetted[fIndex])
        {
            LOG_DEBUG("Executing _uiCommandList[fIndex][{}]: {:X}", fIndex, (size_t) _uiCommandList[fIndex]);
            auto closeResult = _uiCommandList[fIndex]->Close();

            if (closeResult == S_OK)
                _gameCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList**) &_uiCommandList[fIndex]);
            else
                LOG_ERROR("_uiCommandList[{}]->Close() error: {:X}", fIndex, (UINT) closeResult);

            _gameCommandQueue->Signal(_uiFence, _uiAllocatorFenceValues[fIndex]);

            _uiCommandListResetted[fIndex] = false;
        }

        xefg_swapchain_result_t result = XEFG_SWAPCHAIN_RESULT_SUCCESS;

        if (_swapChainContext != nullptr)
        {
            result = XeFGProxy::SetEnabled()(_swapChainContext, false);
            if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
                _isActive = false;
        }
        else
        {
            _isActive = false;
        }

        //_lastDispatchedFrame = 0;
        _waitingNewFrameData = false;

        // Activate switches frame generation back on
        _generationOff = false;

        LOG_INFO("SetEnabled: false, result: {} ({})", magic_enum::enum_name(result), (UINT) result);
    }
}

void XeFG_Dx12::DestroyFGContext()
{
    Deactivate();

    if (_fgContext != nullptr)
        _fgContext = nullptr;

    ReleaseObjects();
}

bool XeFG_Dx12::Shutdown()
{
    MenuOverlayDx::CleanupRenderTarget(true, NULL);

    if (_fgContext != nullptr)
        DestroyFGContext();

    ReleaseObjects();

    if (_swapChainContext != nullptr)
        DestroySwapchainContext();

    return true;
}

bool XeFG_Dx12::Dispatch()
{
    LOG_FUNC();

    UINT64 willDispatchFrame = 0;
    auto fIndex = GetDispatchIndex(willDispatchFrame);
    if (fIndex < 0)
        return false;

    if (!IsActive() || IsPaused())
        return false;

    LOG_DEBUG("_frameCount: {}, willDispatchFrame: {}, fIndex: {}", _frameCount, willDispatchFrame, fIndex);

    if (!_resourceReady[fIndex].contains(FG_ResourceType::Depth) ||
        !_resourceReady[fIndex].at(FG_ResourceType::Depth) ||
        !_resourceReady[fIndex].contains(FG_ResourceType::Velocity) ||
        !_resourceReady[fIndex].at(FG_ResourceType::Velocity))
    {
        LOG_WARN("Depth or Velocity is not ready, skipping");
        return false;
    }

    auto& state = State::Instance();

    if (XeFGProxy::SetUiCompositionState() != nullptr &&
        Config::Instance()->FGXeFGUIComposition.value_or_default() != _uiComposition && IsUsingHudless(fIndex))
    {
        // To prevent XeLL issues
        LOG_DEBUG("UI Composition state changed {}, skipping rendering for 10 frames", _uiComposition);
        state.WAR_xefgRequestFGToggle = true;

        _uiComposition = Config::Instance()->FGXeFGUIComposition.value_or_default();

        auto uiState =
            _uiComposition ? XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED : XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_DISABLED;

#ifndef DONT_USE_XMX
        ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
#endif // !DONT_USE_XMX

        auto uiResult = XeFGProxy::SetUiCompositionState()(_swapChainContext, uiState);

        if (uiResult != XEFG_SWAPCHAIN_RESULT_SUCCESS)
            LOG_ERROR("SetUiCompositionState error: {} ({})", magic_enum::enum_name(uiResult), (UINT) uiResult);
    }

    if (XeFGProxy::SetNumInterpolatedFrames() != nullptr)
    {
        if (Config::Instance()->FGXeFGInterpolationCount.value_or_default() > _maxInterpolationCount)
        {
            Config::Instance()->FGXeFGInterpolationCount = _maxInterpolationCount;
            LOG_WARN("Requested interpolation count is higher than max supported, setting to max: {}",
                     _maxInterpolationCount);
        }

        // The game's count while following its DLSS FG (FG Input DLSSG), clamped here and never saved; else the menu's
        int targetCount = Config::Instance()->FGInterpolationCount(
            Config::Instance()->FGXeFGInterpolationCount.value_or_default(), _maxInterpolationCount);

        // Dynamic MFG (not while following the game's count): the setting is the most, the count follows the frame rate
        const bool dynamic =
            Config::Instance()->FGXeFGDynamicMFG.value_or_default() && !Config::Instance()->FGFollowsGame();
        const bool dynamicChange = dynamic && _dynamicActive;

        if (dynamic)
            targetCount = DynamicCount(targetCount);
        else if (_dynamicActive)
            StopDynamic();

        // Every frame Dynamic doesn't run: a new XeFG (output switched, recreated) mustn't inherit an old target
        if (!dynamic)
        {
            state.xefgDynamicCapFps = 0.0f;
            _dynamicDrain = true;
        }

        _dynamicActive = dynamic;

        if (_framesToInterpolate != targetCount)
        {
            LOG_INFO("Interpolation count changed {} -> {}", _framesToInterpolate, targetCount);

            // XeFG 3.x changes the count at the next frame: Dynamic MFG's own changes skip the FG toggle
            if (!dynamicChange)
                state.WAR_xefgRequestFGToggle = true;

#ifndef DONT_USE_XMX
            ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
#endif // !DONT_USE_XMX

            auto intResult = XeFGProxy::SetNumInterpolatedFrames()(_swapChainContext, targetCount);

            _framesToInterpolate = targetCount;

            if (intResult != XEFG_SWAPCHAIN_RESULT_SUCCESS)
            {
                LOG_ERROR("SetNumInterpolatedFrames error: {} ({})", magic_enum::enum_name(intResult),
                          (UINT) intResult);
            }
        }
    }

    // Workaround for wrong frame limit
    if (state.WAR_xefgRequestFGToggle)
    {
        state.WAR_xefgRequestFGToggle = false;

        state.fgChanged = true;
        UpdateTarget();
        Deactivate();
    }

    if (!_haveHudless.has_value())
    {
        _haveHudless = IsUsingHudless(fIndex);
    }
    else
    {
        auto usingHudless = IsUsingHudless(fIndex);
        static auto version = Version();

        // SDK version 2.1.1 fixed this
        // https://github.com/intel/xess/issues/48
        if (version < feature_version { 1, 2, 2 } && _haveHudless.value() != usingHudless)
        {
            LOG_INFO("Hudless state changed {} -> {}, skipping rendering for 10 frames", _haveHudless.value(),
                     usingHudless);

            _haveHudless = usingHudless;
            state.fgChanged = true;
            UpdateTarget();
            Deactivate();

            return false;
        }
    }

    if (!_noHudless[fIndex])
    {
        auto res = &_frameResources[fIndex][FG_ResourceType::HudlessColor];
        if (res->validity != FG_ResourceValidity::ValidNow)
        {
            res->validity = FG_ResourceValidity::UntilPresentFromDispatch;
            res->frameIndex = fIndex;
            SetResource(res);
        }
    }

    if (!_noDistortionField[fIndex])
    {
        auto res = &_frameResources[fIndex][FG_ResourceType::Distortion];
        if (res->validity != FG_ResourceValidity::ValidNow)
        {
            res->validity = FG_ResourceValidity::UntilPresentFromDispatch;
            res->frameIndex = fIndex;
            SetResource(res);
        }
    }

    XeFGProxy::EnableDebugFeature()(_swapChainContext, XEFG_SWAPCHAIN_DEBUG_FEATURE_TAG_INTERPOLATED_FRAMES,
                                    Config::Instance()->FGXeFGDebugView.value_or_default(), nullptr);
    XeFGProxy::EnableDebugFeature()(_swapChainContext, XEFG_SWAPCHAIN_DEBUG_FEATURE_SHOW_ONLY_INTERPOLATION,
                                    state.fgOnlyGenerated, nullptr);

    xefg_swapchain_frame_constant_data_t constData = {};

    if (_cameraPosition[fIndex][0] != 0.0f || _cameraPosition[fIndex][1] != 0.0f || _cameraPosition[fIndex][2] != 0.0f)
    {
        XMVECTOR right = XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(_cameraRight[fIndex]));
        XMVECTOR up = XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(_cameraUp[fIndex]));
        XMVECTOR forward = XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(_cameraForward[fIndex]));
        XMVECTOR pos = XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(_cameraPosition[fIndex]));

        float x = -XMVectorGetX(XMVector3Dot(pos, right));
        float y = -XMVectorGetX(XMVector3Dot(pos, up));
        float z = -XMVectorGetX(XMVector3Dot(pos, forward));

        XMMATRIX view = { XMVectorSet(XMVectorGetX(right), XMVectorGetX(up), XMVectorGetX(forward), 0.0f),
                          XMVectorSet(XMVectorGetY(right), XMVectorGetY(up), XMVectorGetY(forward), 0.0f),
                          XMVectorSet(XMVectorGetZ(right), XMVectorGetZ(up), XMVectorGetZ(forward), 0.0f),
                          XMVectorSet(x, y, z, 1.0f) };

        memcpy(constData.viewMatrix, view.r, sizeof(view));
    }

    if (Config::Instance()->FGXeFGDepthInverted.value_or_default())
        std::swap(_cameraNear[fIndex], _cameraFar[fIndex]);

    if (_infiniteDepth && _cameraFar[fIndex] > _cameraNear[fIndex])
        _cameraFar[fIndex] = std::numeric_limits<float>::infinity();
    else if (_infiniteDepth && _cameraNear[fIndex] > _cameraFar[fIndex])
        _cameraNear[fIndex] = std::numeric_limits<float>::infinity();

    // Cyberpunk seems to be sending LH so do the same
    // it also sends some extra data in usually empty spots but no idea what that is
    if (_cameraNear[fIndex] > 0.f && _cameraFar[fIndex] > 0.f &&
        !XMScalarNearEqual(_cameraVFov[fIndex], 0.0f, 0.00001f) &&
        !XMScalarNearEqual(_cameraAspectRatio[fIndex], 0.0f, 0.00001f))
    {
        if (XMScalarNearEqual(_cameraNear[fIndex], _cameraFar[fIndex], 0.00001f))
            _cameraFar[fIndex]++;

        auto projectionMatrix = XMMatrixPerspectiveFovLH(_cameraVFov[fIndex], _cameraAspectRatio[fIndex],
                                                         _cameraNear[fIndex], _cameraFar[fIndex]);
        memcpy(constData.projectionMatrix, projectionMatrix.r, sizeof(projectionMatrix));
    }
    else
    {
        LOG_WARN("Can't calculate projectionMatrix");
    }

    constData.jitterOffsetX = _jitterX[fIndex];
    constData.jitterOffsetY = _jitterY[fIndex];
    constData.motionVectorScaleX = _mvScaleX[fIndex];
    constData.motionVectorScaleY = _mvScaleY[fIndex];

    // The frame's ID (FrameIdFor): Present Id and Present Start Id (XeLL's PRESENT_START) match while our XeLL is followed
    const auto framePick = FrameIdFor(willDispatchFrame);
    const auto frameId = framePick.id;

    // A followed XeLL ID that is not XeFG's last + 1 (a pause, or our XeLL restarted its IDs): no interpolation across it
    if (!Config::Instance()->FGSkipReset.value_or_default())
        constData.resetHistory = _reset[fIndex] || framePick.resetHistory;
    else
        constData.resetHistory = false;

    switch (Config::Instance()->FTInput.value_or_default())
    {
    case FrameTimeSource::Input:
        constData.frameRenderTime = (float) _ftDelta[fIndex];
        break;

    case FrameTimeSource::Opti:
        constData.frameRenderTime = static_cast<float>(state.lastFGFrameTime);
        break;

    case FrameTimeSource::Zero:
        constData.frameRenderTime = 0.0f;
        break;
    }

    LOG_DEBUG("Reset: {}, Opti FT: {}, Source FT: {}, Set FT: {}, Opti Id: {}, Present Id: {}, Reflex Id: {}, XeLL Id "
              "now: {}, Present Start Id: {}",
              constData.resetHistory, constData.frameRenderTime, _ftDelta[fIndex], constData.frameRenderTime,
              _frameCount, frameId, State::Instance().reflexFrameId, XellFrameId().id,
              ReflexHooks::lastMarkerFrameId(PRESENT_START));

    auto result = XeFGProxy::TagFrameConstants()(_swapChainContext, frameId, &constData);
    if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        LOG_ERROR("TagFrameConstants error: {} ({})", magic_enum::enum_name(result), (UINT) result);

        state.fgChanged = true;
        UpdateTarget();
        Deactivate();

        return false;
    }

    result = XeFGProxy::SetPresentId()(_swapChainContext, frameId);
    state.xefgPresentId.store(frameId, std::memory_order_relaxed);
    state.xefgPresentIdFollowsMarkers.store(framePick.followsXell, std::memory_order_relaxed);

    if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
        LOG_ERROR("SetPresentId error: {} ({})", magic_enum::enum_name(result), (UINT) result);

        state.fgChanged = true;
        UpdateTarget();
        Deactivate();

        return false;
    }

    // When using Hudfix, we always copy hudless as swapchain size
    if (state.activeFgInput != FGInput::Upscaler)
    {
        uint32_t left = 0;
        uint32_t top = 0;

        if (_interpolationWidth[fIndex] == 0 && _interpolationHeight[fIndex] == 0)
        {
            LOG_WARN("Interpolation size is 0, using swapchain size");
            _interpolationWidth[fIndex] = state.currentSwapchainDesc.BufferDesc.Width;
            _interpolationHeight[fIndex] = state.currentSwapchainDesc.BufferDesc.Height;
        }
        else
        {
            auto calculatedLeft =
                ((int) state.currentSwapchainDesc.BufferDesc.Width - (int) _interpolationWidth[fIndex]) / 2;
            if (calculatedLeft > 0)
                left = Config::Instance()->FGRectLeft.value_or(_interpolationLeft[fIndex].value_or(calculatedLeft));

            auto calculatedTop =
                ((int) state.currentSwapchainDesc.BufferDesc.Height - (int) _interpolationHeight[fIndex]) / 2;
            if (calculatedTop > 0)
                top = Config::Instance()->FGRectTop.value_or(_interpolationTop[fIndex].value_or(calculatedTop));
        }

        LOG_DEBUG("SwapChain Res: {}x{}, Interpolation Res: {}x{}", state.currentSwapchainDesc.BufferDesc.Width,
                  state.currentSwapchainDesc.BufferDesc.Height, _interpolationWidth[fIndex],
                  _interpolationHeight[fIndex]);

        xefg_swapchain_d3d12_resource_data_t backbuffer = {};
        backbuffer.type = XEFG_SWAPCHAIN_RES_BACKBUFFER;
        backbuffer.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
        backbuffer.resourceBase = { (UINT) Config::Instance()->FGRectLeft.value_or(left),
                                    (UINT) Config::Instance()->FGRectTop.value_or(top) };
        backbuffer.resourceSize = { static_cast<uint32_t>(
                                        Config::Instance()->FGRectWidth.value_or(_interpolationWidth[fIndex])),
                                    (UINT) Config::Instance()->FGRectHeight.value_or(_interpolationHeight[fIndex]) };

        result = XeFGProxy::D3D12TagFrameResource()(_swapChainContext, (ID3D12CommandList*) 1, frameId, &backbuffer);

        if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            LOG_ERROR("D3D12TagFrameResource Backbuffer error: {} ({})", magic_enum::enum_name(result), (UINT) result);

            state.fgChanged = true;
            UpdateTarget();
            Deactivate();
        }
    }

    LOG_DEBUG("Result: Ok");

    return true;
}

void* XeFG_Dx12::FrameGenerationContext() { return _fgContext; }

void* XeFG_Dx12::SwapchainContext() { return _swapChainContext; }

XeFG_Dx12::~XeFG_Dx12() { Shutdown(); }

bool XeFG_Dx12::SetInterpolatedFrameCount(UINT interpolatedFrameCount) { return true; }

int XeFG_Dx12::DynamicCount(int maxCount)
{
    auto& state = State::Instance();
    auto config = Config::Instance();

    const auto now = Util::MillisecondsNow();
    const auto target = DynamicTargetFps(now);
    const auto refresh = DisplayRefreshFps(now);

    // The count XeFG has (1X keeps it), within the setting
    const int kept = _framesToInterpolate >= 1 ? std::min(_framesToInterpolate, maxCount) : maxCount;
    const int current = _generationOff ? 0 : kept;

    // In the background the game is throttled: the count is kept, and it starts over once the game is back in front
    const int focus =
        GameWindowFocus(_hwnd != nullptr ? _hwnd : State::Instance().currentSwapchainDesc.OutputWindow);

    if (focus == 0 || (focus < 0 && _dynamicUnfocused))
    {
        if (!_dynamicUnfocused)
            LOG_INFO("Dynamic MFG: the game's window lost focus, keeping {}X", current + 1);

        _dynamicUnfocused = true;
        _dynamicDrain = true; // the waits piled up in the background are no evidence
        return current >= 1 ? current : kept;
    }

    if (_dynamicUnfocused)
    {
        LOG_INFO("Dynamic MFG: the game's window is back in front, starting over at {}X", current + 1);
        _dynamicUnfocused = false;
        _dynamicCount.Reset();
        _dynamicCap.Reset();
        _dynamicCapActive = false;
        _shownPerFrame.Rebase();
        _shownCount = -1;
    }

    // Shown per real frame, measured (the count asked for is not always what XeFG shows): started over at a new count
    if (current != _shownCount)
    {
        _shownPerFrame.Restart();
        _shownCount = current;
    }

    _shownPerFrame.Update(state.xefgShown.load());
    const double shownPerFrame = _shownPerFrame.Value();
    const double rawShownPerFrame = _shownPerFrame.Sample();

    // What the game waited in OptiScaler: its low latency sleep, the present and OptiScaler's limiter (inside the
    // present's time). The cap controller takes the sleeps alone, its probing covers a present held by V-Sync.
    double sleepMs = InputCommon::take_sleep_ms();
    double limiterMs = FrameLimit::TakeSleptMs();

    // Piled up while Dynamic or FG was off, or Dispatch was skipped
    if (_dynamicDrain)
    {
        sleepMs = 0.0;
        limiterMs = 0.0;
        _dynamicDrain = false;
    }

    const double waitMs = sleepMs + state.lastFGPresentMs.load();
    const double frameMs = state.lastFGFrameTime;
    const int rounding = std::clamp(config->FGXeFGRoundingDMFG.value_or_default(), 0, 2);
    // At 2X the only other step is 1X
    const int minCount = config->FGXeFGDynamicAllow1X.value_or_default() || maxCount == 1 ? 0 : 1;

    // Use target as cap: the target is also the FPS limit (FrameLimit::CapFps). It picks from 2X up, so it needs at
    // least 3X to choose from.
    const bool cap = config->FGXeFGTargetAsCapDMFG.value_or_default() && target > 0.0f && maxCount > 1;
    state.xefgDynamicCapFps = cap ? target : 0.0f;

    int count = current;

    if (cap)
    {
        if (!_dynamicCapActive)
            _dynamicCap.Reset();

        count = _dynamicCap.Update({ now, frameMs, sleepMs + limiterMs, target, maxCount, kept, shownPerFrame, rawShownPerFrame });

        if (count != current || now - _dynamicLogMs >= 5000.0)
        {
            static constexpr const char* reasons[] = { "", "up", "down", "try down", "undo" };
            const auto reason = _dynamicCap.LastReason();
            const auto line = std::format(
                "Dynamic MFG {}X{}{}: presented {:.1f} fps, target {:.1f} as cap, reachable {:.1f} fps, back-off "
                "{:.0f} s",
                current + 1, count != current ? std::format(" -> {}X", count + 1) : std::string(),
                reason != XeFG_DynamicCap::Reason::None ? std::format(" ({})", reasons[(int) reason]) : std::string(),
                _dynamicCap.PresentedFps(), target, _dynamicCap.ReachableFps(), _dynamicCap.BackoffMs() / 1000.0);

            // Changes at info level, the state every 5 s at debug level
            if (count != current)
                LOG_INFO("{}", line);
            else
                LOG_DEBUG("{}", line);

            _dynamicLogMs = now;
        }
    }
    else
    {
        // Back from the cap: the averages start over, what was learned stays
        if (_dynamicCapActive)
            _dynamicCount.Restart();

        count = _dynamicCount.Update({ now, frameMs, waitMs, target, refresh, config->FramerateLimit.value_or_default(),
                                       maxCount, current, rounding, minCount, shownPerFrame, rawShownPerFrame });

        if (count != current || now - _dynamicLogMs >= 5000.0)
        {
            static constexpr const char* reasons[] = { "", "up", "down", "try down", "undo", "no gain, back" };
            static constexpr const char* roundings[] = { "at most", "around", "at least" };
            const auto reason = _dynamicCount.LastReason();
            const auto line = std::format(
                "Dynamic MFG {}X{}{}: presented {:.1f} fps, target {:.1f} (set {:.1f}, {}, {}, refresh {:.0f}), one "
                "step less {:.1f}, waits {:.0f}%, frame cost {:.2f} ms, back-off {:.0f} s",
                current + 1, count != current ? std::format(" -> {}X", count + 1) : std::string(),
                reason != XeFG_DynamicCount::Reason::None ? std::format(" ({})", reasons[(int) reason]) : std::string(),
                _dynamicCount.PresentedFps(), _dynamicCount.TargetFps(), target, roundings[rounding],
                _dynamicCount.Uncapped() ? "no cap" : "capped", refresh, _dynamicCount.LowerFps(),
                _dynamicCount.WaitShare() * 100.0, _dynamicCount.FrameCostMs(), _dynamicCount.BackoffMs() / 1000.0);

            if (count != current)
                LOG_INFO("{}", line);
            else
                LOG_DEBUG("{}", line);

            _dynamicLogMs = now;
        }
    }

    _dynamicCapActive = cap;

    // 1X: frame generation off. XeFG keeps its count, so nothing is rebuilt; it turns back on at once, and its first
    // frame after that has no history and shows the real frame alone.
    if ((count == 0) != _generationOff)
        SetGenerationOff(count == 0);

    return count >= 1 ? count : kept;
}

void XeFG_Dx12::StopDynamic()
{
    _dynamicCount.Reset();
    _dynamicCap.Reset();
    _dynamicCapActive = false;
    _dynamicUnfocused = false;
    _shownPerFrame.Rebase();
    _shownCount = -1;
    State::Instance().xefgDynamicCapFps = 0.0f;

    if (_generationOff)
        SetGenerationOff(false);
}

void XeFG_Dx12::SetGenerationOff(bool off)
{
    if (_swapChainContext == nullptr || XeFGProxy::SetEnabled() == nullptr)
        return;

#ifndef DONT_USE_XMX
    ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
#endif // !DONT_USE_XMX

    // Only frame generation: OptiScaler's FG stays active and keeps dispatching, XeFG keeps its count
    const auto result = XeFGProxy::SetEnabled()(_swapChainContext, !off);

    if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
        _generationOff = off;

    LOG_INFO("Dynamic MFG 1X: frame generation {}, result: {} ({})", off ? "off" : "on", magic_enum::enum_name(result),
             (UINT) result);
}

float XeFG_Dx12::DisplayRefreshFps(double nowMs)
{
    // The window can move to another display: looked up again every 2 s
    const HWND hwnd = _hwnd != nullptr ? _hwnd : State::Instance().currentSwapchainDesc.OutputWindow;

    // Also when it failed: the lookup runs up to twice a frame with an Auto target
    if (hwnd != nullptr && (_refreshCheckedMs <= 0.0 || nowMs - _refreshCheckedMs >= 2000.0))
    {
        _refreshCheckedMs = nowMs;
        _refreshRate = Util::GetActiveRefreshRate(hwnd);
    }

    return (float) std::max(_refreshRate, 0);
}

float XeFG_Dx12::DynamicTargetFps(double nowMs)
{
    return xefg::DynamicTargetFps(Config::Instance()->FGXeFGFramerateTargetDMFG.value_or_default(),
                                  Config::Instance()->FramerateLimit.value_or_default(), DisplayRefreshFps(nowMs),
                                  State::Instance().fgPresentVsync.load(std::memory_order_relaxed));
}

void XeFG_Dx12::EvaluateState(ID3D12Device* device, FG_Constants& fgConstants)
{
    LOG_FUNC();

    OwnedLockGuard lock(Mutex, 555);

    auto& state = State::Instance();

    // If needed hooks are missing or XeFG proxy is not inited or FG swapchain is not created
    if (!XeFGProxy::InitXeFG() || state.currentFGSwapchain == nullptr)
        return;

    if (state.isShuttingDown)
    {
        DestroyFGContext();
        return;
    }

    _infiniteDepth = static_cast<bool>(fgConstants.flags & FG_Flags::InfiniteDepth);

    // If FG Enabled from menu
    if (Config::Instance()->IsFGEnabled())
    {
        // If FG context is nullptr
        if (_fgContext == nullptr)
        {
            // Create it again
            CreateContext(device, fgConstants);

            // Pause for 10 frames
            UpdateTarget();
        }
        // If there is a change deactivate it
        else if (state.fgChanged)
        {
            LOG_DEBUG("FGChanged");
            Deactivate();

            // Pause for 10 frames
            UpdateTarget();

            // Destroy if Swapchain has a change destroy FG Context too
            if (state.scChanged)
                DestroyFGContext();
        }

        if (_fgContext != nullptr && State::Instance().activeFgInput == FGInput::Upscaler && !IsPaused() && !IsActive())
            Activate();
    }
    else
    {
        LOG_DEBUG("!FGEnabled");
        Deactivate();

        state.clearCapturedHudlesses = true;
        Hudfix_Dx12::ResetCounters();
    }

    if (state.fgChanged)
    {
        LOG_DEBUG("FGchanged");

        state.fgChanged = false;

        Hudfix_Dx12::ResetCounters();

        // Pause for 10 frames
        UpdateTarget();

        // Release FG mutex
        if (Mutex.getOwner() == 2)
            Mutex.unlockThis(2);
    }

    state.scChanged = false;
}

void XeFG_Dx12::ReleaseObjects()
{
    for (size_t i = 0; i < BUFFER_COUNT; i++)
    {
        SAFE_RELEASE(_uiCommandAllocator[i]);
        SAFE_RELEASE(_uiCommandList[i]);
        SAFE_RELEASE(_scCommandAllocator[i]);
        SAFE_RELEASE(_scCommandList[i]);

        // Reset command list state
        _scCommandListResetted[i] = false;
        _scAllocatorFenceValues[i] = 0;

        _uiCommandListResetted[i] = false;
        _uiAllocatorFenceValues[i] = 0;
    }

    _renderUI.reset();
    _hudlessCompare.reset();
    _mvFlip.reset();
    _depthFlip.reset();
    _depthInvert.reset();
}

void XeFG_Dx12::CreateObjects(ID3D12Device* InDevice)
{
    _device = InDevice;

    if (_uiCommandAllocator[0] != nullptr)
        return;

    LOG_DEBUG("");

    do
    {
        HRESULT result;
        ID3D12CommandAllocator* allocator = nullptr;
        ID3D12GraphicsCommandList* cmdList = nullptr;
        ID3D12CommandQueue* cmdQueue = nullptr;

        // FG
        for (size_t i = 0; i < BUFFER_COUNT; i++)
        {
            // Reset command list state
            _scCommandListResetted[i] = false;
            _scAllocatorFenceValues[i] = 0;

            _uiCommandListResetted[i] = false;
            _uiAllocatorFenceValues[i] = 0;

            result =
                InDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_uiCommandAllocator[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandAllocators _uiCommandAllocator[{}]: {:X}", i, (unsigned long) result);
                break;
            }

            _uiCommandAllocator[i]->SetName(std::format(L"_uiCommandAllocator[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _uiCommandAllocator[i], (IUnknown**) &allocator))
                _uiCommandAllocator[i] = allocator;

            result = InDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _uiCommandAllocator[i], NULL,
                                                 IID_PPV_ARGS(&_uiCommandList[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandList _hudlessCommandList[{}]: {:X}", i, (unsigned long) result);
                break;
            }
            _uiCommandList[i]->SetName(std::format(L"_uiCommandList[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _uiCommandList[i], (IUnknown**) &cmdList))
                _uiCommandList[i] = cmdList;

            result = _uiCommandList[i]->Close();
            if (result != S_OK)
            {
                LOG_ERROR("_uiCommandList[{}]->Close: {:X}", i, (unsigned long) result);
                break;
            }

            if (_uiFence == nullptr)
            {
                result = InDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_uiFence));
                if (FAILED(result))
                {
                    LOG_ERROR("Create UI fence failed: {:X}", (UINT) result);
                    break;
                }
            }

            if (_uiFenceEvent == nullptr)
            {
                _uiFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                if (_uiFenceEvent == nullptr)
                {
                    LOG_ERROR("CreateEvent for UI fence failed");
                    break;
                }
            }

            result =
                InDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_scCommandAllocator[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandAllocators _scCommandAllocator[{}]: {:X}", i, (unsigned long) result);
                break;
            }

            _scCommandAllocator[i]->SetName(std::format(L"_scCommandAllocator[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _scCommandAllocator[i], (IUnknown**) &allocator))
                _scCommandAllocator[i] = allocator;

            result = InDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _scCommandAllocator[i], NULL,
                                                 IID_PPV_ARGS(&_scCommandList[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandList _hudlessCommandList[{}]: {:X}", i, (unsigned long) result);
                break;
            }
            _scCommandList[i]->SetName(std::format(L"_scCommandList[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _scCommandList[i], (IUnknown**) &cmdList))
                _scCommandList[i] = cmdList;

            result = _scCommandList[i]->Close();
            if (result != S_OK)
            {
                LOG_ERROR("_scCommandList[{}]->Close: {:X}", i, (unsigned long) result);
                break;
            }

            if (_scFence == nullptr)
            {
                result = InDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_scFence));
                if (FAILED(result))
                {
                    LOG_ERROR("Create SC fence failed: {:X}", (UINT) result);
                    break;
                }
            }

            if (_scFenceEvent == nullptr)
            {
                _scFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                if (_scFenceEvent == nullptr)
                {
                    LOG_ERROR("CreateEvent for SC fence failed");
                    break;
                }
            }
        }

    } while (false);
}

bool XeFG_Dx12::Present()
{
    auto fIndex = GetIndexWillBeDispatched();
    LOG_DEBUG("fIndex: {}", fIndex);

    if (Config::Instance()->FGDrawUIOverFG.value_or_default())
    {
        auto ui = GetResource(FG_ResourceType::UIColor, fIndex);
        if (ui && (ui->validity == FG_ResourceValidity::UntilPresent ||
                   ui->validity == FG_ResourceValidity::UntilPresentFromDispatch))
        {
            LOG_DEBUG("UI[{}] resource: {:X}, copy: {}", fIndex, (size_t) ui->resource, (size_t) ui->copy);
            if (_renderUI.get() == nullptr)
            {
                _renderUI = std::make_unique<RUI_Dx12>("RenderUI", _device,
                                                       Config::Instance()->FGUIPremultipliedAlpha.value_or_default());
            }
            else
            {
                if (Config::Instance()->FGUIPremultipliedAlpha.value_or_default() != _renderUI->IsPreMultipliedAlpha())
                {
                    LOG_INFO("UI premultiplied alpha changed, recreating RenderUI");
                    _renderUI = std::make_unique<RUI_Dx12>(
                        "RenderUI", _device, Config::Instance()->FGUIPremultipliedAlpha.value_or_default());
                }
                else if (_renderUI->IsInit())
                {
                    auto commandList = GetSCCommandList(fIndex);
                    _renderUI->Dispatch((IDXGISwapChain3*) _swapChain, commandList, ui->GetResource(), ui->state);
                }
            }
        }
        else if (!ui)
        {
            LOG_WARN("UI resource is nullptr");
        }
    }

    if (IsActive() && !IsPaused())
    {
        if (State::Instance().fgHudlessCompare)
        {
            auto hudless = GetResource(FG_ResourceType::HudlessColor, fIndex);
            if (hudless && (hudless->validity == FG_ResourceValidity::UntilPresent ||
                            hudless->validity == FG_ResourceValidity::UntilPresentFromDispatch))
            {
                LOG_DEBUG("Hudless[{}] resource: {:X}, copy: {}", fIndex, (size_t) hudless->resource,
                          (size_t) hudless->copy);
                if (_hudlessCompare.get() == nullptr)
                {
                    _hudlessCompare = std::make_unique<HC_Dx12>("HudlessCompare", _device);
                }
                else
                {
                    if (_hudlessCompare->IsInit())
                    {
                        auto commandList = GetSCCommandList(fIndex);
                        _hudlessCompare->Dispatch((IDXGISwapChain3*) _swapChain, commandList, hudless->GetResource(),
                                                  hudless->state);
                    }
                }
            }
            else if (!hudless)
            {
                LOG_WARN("Hudless resource is nullptr");
            }
        }
    }

    bool result = false;

    // if (IsActive() && !IsPaused())
    {
        if (_uiCommandListResetted[fIndex])
        {
            LOG_DEBUG("Executing _uiCommandList[{}]: {:X}", fIndex, (size_t) _uiCommandList[fIndex]);
            auto closeResult = _uiCommandList[fIndex]->Close();

            if (closeResult == S_OK)
                _gameCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList**) &_uiCommandList[fIndex]);
            else
                LOG_ERROR("_uiCommandList[{}]->Close() error: {:X}", fIndex, (UINT) closeResult);

            _gameCommandQueue->Signal(_uiFence, _uiAllocatorFenceValues[fIndex]);

            _uiCommandListResetted[fIndex] = false;
        }

        if (_scCommandListResetted[fIndex])
        {
            LOG_DEBUG("Executing _scCommandList[{}]: {:X}", fIndex, (size_t) _scCommandList[fIndex]);
            auto closeResult = _scCommandList[fIndex]->Close();

            if (closeResult == S_OK)
                _gameCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList**) &_scCommandList[fIndex]);
            else
                LOG_ERROR("_scCommandList[{}]->Close() error: {:X}", fIndex, (UINT) closeResult);

            _scCommandListResetted[fIndex] = false;
        }
    }

    if ((_fgFramePresentId - _lastFGFramePresentId) > 3 && IsActive() && !_waitingNewFrameData)
    {
        LOG_DEBUG("Pausing FG");
        Deactivate();
        _waitingNewFrameData = true;
        return false;
    }

    _fgFramePresentId++;

    return Dispatch();
}

// XeLL's frame ID of the frame being rendered and whose it is: ours while Optical F5Low's low latency feeds XeFG's XeLL,
// else the game's when its own Reflex feeds XeLL, else 0 (XeFG numbers frames itself)
static XellFrameIdAndSource XellFrameId()
{
    if (const auto ours = native::lowlatency::FrameIdForFrameGeneration(); ours != 0)
        return { ours, xefg::XellIdSource::Ours };

    if (const auto game = ReflexHooks::gameXellFrameId(); game != 0)
        return { game, xefg::XellIdSource::Game };

    return {};
}

// Picked at the frame's first use (its first tagged resource, else its Dispatch) and kept for all of the frame's tags
// and its present (framegen/xefg/XeFGFrameId.h)
xefg::FrameIdPick XeFG_Dx12::FrameIdFor(UINT64 fgFrame)
{
    std::scoped_lock lock(_frameIdMutex);

    const auto lastFollowed = _frameIds.LastFollowedXell();
    const auto lastId = _frameIds.LastId();
    const auto xell = XellFrameId();
    const bool canReset = !Config::Instance()->FGSkipReset.value_or_default();
    const auto pick = _frameIds.For(fgFrame, xell.id, xell.source, _reset[fgFrame % BUFFER_COUNT] != 0, canReset);

    if (!pick.picked)
        return pick;

    if (pick.followsXell)
    {
        if (_frameIdMode != FrameIdMode::FollowsOurs)
            LOG_INFO("XeFG frame IDs follow our XeLL's (from {})", pick.id);

        _frameIdMode = FrameIdMode::FollowsOurs;
    }
    else if (pick.ownReason == xefg::OwnReason::Jumped)
    {
        if (_frameIdMode != FrameIdMode::OwnAfterJump)
            LOG_INFO("XeFG frame IDs: own counter, XeLL ID jumped {} -> {} (XeFG last {})", lastFollowed, xell.id,
                     lastId);

        _frameIdMode = FrameIdMode::OwnAfterJump;
    }
    else if (pick.ownReason == xefg::OwnReason::Unusable)
    {
        if (_frameIdMode != FrameIdMode::OwnUnusable)
            LOG_INFO("XeFG frame IDs: own counter, our XeLL ID {} not usable (XeFG last {})", xell.id, lastId);

        _frameIdMode = FrameIdMode::OwnUnusable;
    }
    else if (xell.source == xefg::XellIdSource::Game)
    {
        if (_frameIdMode != FrameIdMode::OwnForGame)
            LOG_INFO("XeFG frame IDs: own counter (game Reflex IDs not followed)");

        _frameIdMode = FrameIdMode::OwnForGame;
    }
    else
    {
        if (_frameIdMode != FrameIdMode::Own)
            LOG_INFO("XeFG frame IDs: own counter (no XeLL IDs)");

        _frameIdMode = FrameIdMode::Own;
    }

    if (pick.resetHistory)
        LOG_INFO("XeFG frame IDs: XeLL's {} is not XeFG's last {} + 1, history reset", pick.id, lastId);

    return pick;
}

bool XeFG_Dx12::SetResource(Dx12Resource* inputResource)
{
    if (inputResource == nullptr || inputResource->resource == nullptr ||
        (inputResource->type != FG_ResourceType::UIColor && (!IsActive() || IsPaused())))
    {
        return false;
    }

    // For late sent SL resources
    // we use provided frame index
    auto fIndex = inputResource->frameIndex;
    if (fIndex < 0)
        fIndex = GetIndex();

    auto& type = inputResource->type;

    std::unique_lock<std::shared_mutex> lock(_resourceMutex[fIndex]);

    // This is mostly useful for cases where the user has manually set validity as ValidNow
    if (!inputResource->cmdList && inputResource->validity != FG_ResourceValidity::UntilPresent &&
        inputResource->validity != FG_ResourceValidity::UntilPresentFromDispatch)
    {
        LOG_WARN("XeFG needs cmdList for ValidNow resources, YOLOing");
        inputResource->validity = FG_ResourceValidity::UntilPresent;
    }

    if (type == FG_ResourceType::HudlessColor)
    {
        if (Config::Instance()->FGDisableHudless.value_or_default())
            return false;

        // Making a copy if it's just valid now to be able to use it later
        if (State::Instance().fgHudlessCompare && inputResource->validity == FG_ResourceValidity::ValidNow)
            inputResource->validity = FG_ResourceValidity::ValidButMakeCopy;

        if (!_noHudless[fIndex] && (_frameResources[fIndex][type].validity == FG_ResourceValidity::ValidNow))
        {
            return false;
        }

        if (!_noHudless[fIndex] && Config::Instance()->FGOnlyAcceptFirstHudless.value_or_default() &&
            inputResource->validity != FG_ResourceValidity::UntilPresentFromDispatch)
        {
            return false;
        }
    }

    if (type == FG_ResourceType::UIColor)
    {
        if (Config::Instance()->FGDisableUI.value_or_default())
            return false;

        // Making a copy if it's just valid now
        if (Config::Instance()->FGDrawUIOverFG.value_or_default() &&
            inputResource->validity == FG_ResourceValidity::ValidNow)
        {
            inputResource->validity = FG_ResourceValidity::ValidButMakeCopy;
        }

        if (!_noUi[fIndex] && (_frameResources[fIndex][type].validity == FG_ResourceValidity::ValidNow))
        {
            return false;
        }
    }

    if (type == FG_ResourceType::Distortion)
    {
        if (!_noDistortionField[fIndex] && (_frameResources[fIndex][type].validity == FG_ResourceValidity::ValidNow))
        {
            return false;
        }
    }

    if ((type == FG_ResourceType::Depth || type == FG_ResourceType::Velocity) && _frameResources[fIndex].contains(type))
    {
        return false;
    }

    if (inputResource->cmdList == nullptr && inputResource->validity == FG_ResourceValidity::ValidNow)
    {
        LOG_ERROR("{}, validity == ValidNow but cmdList is nullptr!", magic_enum::enum_name(type));
        return false;
    }

    if (type == FG_ResourceType::Distortion)
    {
        LOG_TRACE("Distortion field is not supported by XeFG");
        return false;
    }

    auto fResource = &_frameResources[fIndex][type];
    fResource->type = type;
    fResource->state = inputResource->state;
    fResource->validity = inputResource->validity;
    fResource->resource = inputResource->resource;
    fResource->top = inputResource->top;
    fResource->left = inputResource->left;
    fResource->width = inputResource->width;
    fResource->height = inputResource->height;
    fResource->cmdList = inputResource->cmdList;

    auto willFlip = State::Instance().activeFgInput == FGInput::Upscaler &&
                    Config::Instance()->FGResourceFlip.value_or_default() &&
                    (type == FG_ResourceType::Velocity || type == FG_ResourceType::Depth);

    // Resource flipping
    if (willFlip && _device != nullptr)
        FlipResource(fResource);

    // Depth Invert
    // https://github.com/intel/xess/issues/50
    static auto version = Version();

    // SDK version 2.1.1 fixed this
    if (version < feature_version { 1, 2, 2 } && _device != nullptr && type == FG_ResourceType::Depth &&
        !Config::Instance()->FGXeFGDepthInverted.value_or_default())
    {
        if (_depthInvert.get() == nullptr)
        {
            _depthInvert = std::make_unique<DI_Dx12>("DepthInvert", _device);
        }
        else if (_depthInvert->IsInit())
        {
            if (_depthInvert->CreateBufferResource(_device, fResource->GetResource(), fResource->width,
                                                   fResource->height, fResource->state) &&
                _depthInvert->Buffer() != nullptr)
            {
                auto cmdList = (fResource->cmdList != nullptr) ? fResource->cmdList : GetUICommandList(fIndex);

                _depthInvert->SetBufferState(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                if (_depthInvert->Dispatch(cmdList, fResource->GetResource(), _depthInvert->Buffer()))
                {
                    fResource->copy = _depthInvert->Buffer();
                }

                _depthInvert->SetBufferState(cmdList, fResource->state);
            }
        }
    }

    // We usually don't copy any resources for XeFG, the ones with this tag are the exception
    if (inputResource->cmdList != nullptr && fResource->validity == FG_ResourceValidity::ValidButMakeCopy)
    {
        LOG_DEBUG("Making a resource copy of: {}", magic_enum::enum_name(type));

        ID3D12Resource* copyOutput = nullptr;

        if (_resourceCopy[fIndex].contains(type))
            copyOutput = _resourceCopy[fIndex][type];

        if (!CopyResource(inputResource->cmdList, inputResource->resource, &copyOutput, inputResource->state))
        {
            LOG_ERROR("{}, CopyResource error!", magic_enum::enum_name(type));
            return false;
        }

        _resourceCopy[fIndex][type] = copyOutput;
        _resourceCopy[fIndex][type]->SetName(std::format(L"_resourceCopy[{}][{}]", fIndex, (UINT) type).c_str());
        fResource->copy = copyOutput;
        fResource->state = D3D12_RESOURCE_STATE_COPY_DEST;

        fResource->validity = FG_ResourceValidity::UntilPresent;
    }

    if (type == FG_ResourceType::UIColor)
        _noUi[fIndex] = false;
    else if (type == FG_ResourceType::Distortion)
        _noDistortionField[fIndex] = false;
    else if (type == FG_ResourceType::HudlessColor)
        _noHudless[fIndex] = false;

    if ((type == FG_ResourceType::Depth || type == FG_ResourceType::Velocity) ||
        fResource->validity != FG_ResourceValidity::UntilPresent)
    {
        fResource->validity = (fResource->validity != FG_ResourceValidity::ValidNow || willFlip)
                                  ? FG_ResourceValidity::UntilPresent
                                  : FG_ResourceValidity::ValidNow;

        if (type == FG_ResourceType::HudlessColor)
        {
            static DXGI_FORMAT lastFormat[BUFFER_COUNT] = {};
            auto desc = fResource->GetResource()->GetDesc();

            if (lastFormat[fIndex] != DXGI_FORMAT_UNKNOWN && lastFormat[fIndex] != desc.Format)
            {
                State::Instance().fgChanged = true;
                return false;
            }

            lastFormat[fIndex] = desc.Format;
        }

        xefg_swapchain_d3d12_resource_data_t resourceParam = GetResourceData(type, fIndex);

        // SDK version 2.1.1 fixes those issues
        if (version < feature_version { 1, 2, 2 })
        {
            // HACK: XeFG docs lie and cmd list is technically required as it checks for it
            // But it doesn't seem to use it when the validity is UNTIL_NEXT_PRESENT
            // https://github.com/intel/xess/issues/45
            if (fResource->cmdList == nullptr && resourceParam.validity == XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT)
                fResource->cmdList = (ID3D12GraphicsCommandList*) 1;

            // HACK: XeFG seems to crash if the resource is in COPY_SOURCE state
            // even though the docs say it's the preferred state
            // https://github.com/intel/xess/issues/47
            if (inputResource->state == D3D12_RESOURCE_STATE_COPY_SOURCE)
            {
                ResourceBarrier(inputResource->cmdList, inputResource->resource, inputResource->state,
                                D3D12_RESOURCE_STATE_COPY_DEST);

                resourceParam.incomingState = D3D12_RESOURCE_STATE_COPY_DEST;
            }
        }

        int indexDiff = GetIndex() - fIndex;
        if (indexDiff < 0)
            indexDiff += BUFFER_COUNT;

        // We will us UI color later with Render UI
        if (type != FG_ResourceType::UIColor ||
            (XeFGProxy::SetUiCompositionState() != nullptr || Config::Instance()->FGDrawUIOverFG.value_or_default()))
        {
            auto frameId = FrameIdFor(_frameCount - indexDiff).id;
            auto result =
                XeFGProxy::D3D12TagFrameResource()(_swapChainContext, fResource->cmdList, frameId, &resourceParam);
            LOG_DEBUG("D3D12TagFrameResource, frameId: {}, type: {} result: {} ({})", frameId,
                      magic_enum::enum_name(type), magic_enum::enum_name(result), (int32_t) result);

            if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
            {
                State::Instance().fgChanged = true;
                UpdateTarget();
                Deactivate();

                return false;
            }
        }

        // Potentially we don't need to restore but do it just to be safe
        if (inputResource->state == D3D12_RESOURCE_STATE_COPY_SOURCE)
        {
            ResourceBarrier(inputResource->cmdList, inputResource->resource, D3D12_RESOURCE_STATE_COPY_DEST,
                            inputResource->state);
        }

        SetResourceReady(type, fIndex);
    }

    LOG_TRACE("_frameResources[{}][{}]: {:X}", fIndex, magic_enum::enum_name(type), (size_t) fResource->GetResource());

    return true;
}

void XeFG_Dx12::SetCommandQueue(FG_ResourceType type, ID3D12CommandQueue* queue) { _gameCommandQueue = queue; }

bool XeFG_Dx12::ReleaseSwapchain(HWND hwnd)
{
    if (hwnd != _hwnd || _hwnd == NULL)
        return false;

    LOG_DEBUG("");

    // Every teardown of XeFG's swapchain comes through here (a new swapchain in CreateSwapchain/CreateSwapchain1,
    // FGHooks::hkFGRelease, the wrapped swapchain's Release): a REFramework drawing on it lets go first. Before frame
    // generation's lock: REFramework takes its own lock in there, and the fork's author saw the other order deadlock
    REFrameworkCompat::BeforeXeFGSwapchainRetire(State::Instance().currentFGSwapchain, _swapChainContext, hwnd);

    // Setting can be changed from the menu while we are in here
    bool mutexLocked = false;
    if (Config::Instance()->FGUseMutexForSwapchain.value_or_default())
    {
        if (Mutex.getOwner() == 1)
        {
            LOG_WARN("Skipping Mutex we are already in ReleaseSwapchain");
            return true;
        }

        LOG_TRACE("Waiting Mutex 1, current: {}", Mutex.getOwner());
        Mutex.lock(1);
        mutexLocked = true;
        LOG_TRACE("Accuired Mutex: {}", Mutex.getOwner());
    }

    MenuOverlayDx::CleanupRenderTarget(true, NULL);

    if (_fgContext != nullptr)
        DestroyFGContext();

    if (!State::Instance().isShuttingDown)
    {
        // First: destroying the context releases the swapchain, and that Release (FGHooks::hkFGRelease) must not
        // find it still current and come back in here
        State::Instance().currentFGSwapchain = nullptr;

        if (_swapChainContext != nullptr)
            DestroySwapchainContext();

        _swapChainContext = nullptr;
    }

    ReleaseObjects();

    if (mutexLocked)
    {
        LOG_TRACE("Releasing Mutex: {}", Mutex.getOwner());
        Mutex.unlockThis(1);
    }

    return true;
}
