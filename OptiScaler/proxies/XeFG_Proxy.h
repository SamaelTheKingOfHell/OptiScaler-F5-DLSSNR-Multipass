#pragma once

#include "SysUtils.h"
#include "Util.h"
#include "Config.h"
#include "Logger.h"

#include <proxies/Ntdll_Proxy.h>
#include <proxies/KernelBase_Proxy.h>
#include <misc/XeModuleInfo.h>
#include <framegen/f5low/F5Low.h>

#include <xefg_swapchain.h>
#include <xefg_swapchain_d3d12.h>
#include <xefg_swapchain_debug.h>

#pragma comment(lib, "Version.lib")

// Common
typedef decltype(&xefgSwapChainGetVersion) PFN_xefgSwapChainGetVersion;
typedef decltype(&xefgSwapChainGetProperties) PFN_xefgSwapChainGetProperties;
typedef decltype(&xefgSwapChainTagFrameConstants) PFN_xefgSwapChainTagFrameConstants;
typedef decltype(&xefgSwapChainSetEnabled) PFN_xefgSwapChainSetEnabled;
typedef decltype(&xefgSwapChainSetPresentId) PFN_xefgSwapChainSetPresentId;
typedef decltype(&xefgSwapChainGetLastPresentStatus) PFN_xefgSwapChainGetLastPresentStatus;
typedef decltype(&xefgSwapChainSetLoggingCallback) PFN_xefgSwapChainSetLoggingCallback;
typedef decltype(&xefgSwapChainDestroy) PFN_xefgSwapChainDestroy;
typedef decltype(&xefgSwapChainSetLatencyReduction) PFN_xefgSwapChainSetLatencyReduction;
typedef decltype(&xefgSwapChainSetSceneChangeThreshold) PFN_xefgSwapChainSetSceneChangeThreshold;
typedef decltype(&xefgSwapChainGetPipelineBuildStatus) PFN_xefgSwapChainGetPipelineBuildStatus;
typedef decltype(&xefgSwapChainSetNumInterpolatedFrames) PFN_xefgSwapChainSetNumInterpolatedFrames;
typedef decltype(&xefgSwapChainSetUiCompositionState) PFN_xefgSwapChainSetUiCompositionState;

// Dx12
typedef decltype(&xefgSwapChainD3D12CreateContext) PFN_xefgSwapChainD3D12CreateContext;
typedef decltype(&xefgSwapChainD3D12BuildPipelines) PFN_xefgSwapChainD3D12BuildPipelines;
typedef decltype(&xefgSwapChainD3D12GetProperties) PFN_xefgSwapChainD3D12GetProperties;
typedef decltype(&xefgSwapChainD3D12InitFromSwapChain) PFN_xefgSwapChainD3D12InitFromSwapChain;
typedef decltype(&xefgSwapChainD3D12InitFromSwapChainDesc) PFN_xefgSwapChainD3D12InitFromSwapChainDesc;
typedef decltype(&xefgSwapChainD3D12GetSwapChainPtr) PFN_xefgSwapChainD3D12GetSwapChainPtr;
typedef decltype(&xefgSwapChainD3D12TagFrameResource) PFN_xefgSwapChainD3D12TagFrameResource;
typedef decltype(&xefgSwapChainD3D12SetDescriptorHeap) PFN_xefgSwapChainD3D12SetDescriptorHeap;
typedef decltype(&xefgSwapChainD3D12UpdateExternalHeapOnResize) PFN_xefgSwapChainD3D12UpdateExternalHeapOnResize;
typedef decltype(&xefgSwapChainD3D12GetInitializationParameters) PFN_xefgSwapChainD3D12GetInitializationParameters;

// Debug
typedef decltype(&xefgSwapChainEnableDebugFeature) PFN_xefgSwapChainEnableDebugFeature;

class XeFGProxy
{
  private:
    inline static HMODULE _dll = nullptr;
    // A libxess_fg.dll that was already loaded (the game's own) when OptiScaler loaded its copy
    inline static HMODULE _memModule = nullptr;
    inline static std::wstring _dllPath;

    inline static xefg_swapchain_version_t _xefgVersion {};

    // Common
    inline static PFN_xefgSwapChainGetVersion _xefgSwapChainGetVersion = nullptr;
    inline static PFN_xefgSwapChainGetProperties _xefgSwapChainGetProperties = nullptr;
    inline static PFN_xefgSwapChainTagFrameConstants _xefgSwapChainTagFrameConstants = nullptr;
    inline static PFN_xefgSwapChainSetEnabled _xefgSwapChainSetEnabled = nullptr;
    inline static PFN_xefgSwapChainSetPresentId _xefgSwapChainSetPresentId = nullptr;
    inline static PFN_xefgSwapChainGetLastPresentStatus _xefgSwapChainGetLastPresentStatus = nullptr;
    inline static PFN_xefgSwapChainSetLoggingCallback _xefgSwapChainSetLoggingCallback = nullptr;
    inline static PFN_xefgSwapChainDestroy _xefgSwapChainDestroy = nullptr;
    inline static PFN_xefgSwapChainSetLatencyReduction _xefgSwapChainSetLatencyReduction = nullptr;
    using PFN_XellCountsRenderedOnly = uint32_t (*)();
    inline static PFN_XellCountsRenderedOnly _xellCountsRenderedOnly = nullptr;
    using PFN_DriverMultipliesReflexCap = uint32_t (*)();
    inline static PFN_DriverMultipliesReflexCap _driverMultipliesReflexCap = nullptr;
    using PFN_HostRoutesMarkers = void (*)();
    inline static PFN_xefgSwapChainSetSceneChangeThreshold _xefgSwapChainSetSceneChangeThreshold = nullptr;
    inline static PFN_xefgSwapChainGetPipelineBuildStatus _xefgSwapChainGetPipelineBuildStatus = nullptr;
    inline static PFN_xefgSwapChainSetNumInterpolatedFrames _xefgSwapChainSetNumInterpolatedFrames = nullptr;
    inline static PFN_xefgSwapChainSetUiCompositionState _xefgSwapChainSetUiCompositionState = nullptr;

    // Dx12
    inline static PFN_xefgSwapChainD3D12CreateContext _xefgSwapChainD3D12CreateContext = nullptr;
    inline static PFN_xefgSwapChainD3D12BuildPipelines _xefgSwapChainD3D12BuildPipelines = nullptr;
    inline static PFN_xefgSwapChainD3D12GetProperties _xefgSwapChainD3D12GetProperties = nullptr;
    inline static PFN_xefgSwapChainD3D12InitFromSwapChain _xefgSwapChainD3D12InitFromSwapChain = nullptr;
    inline static PFN_xefgSwapChainD3D12InitFromSwapChainDesc _xefgSwapChainD3D12InitFromSwapChainDesc = nullptr;
    inline static PFN_xefgSwapChainD3D12GetSwapChainPtr _xefgSwapChainD3D12GetSwapChainPtr = nullptr;
    inline static PFN_xefgSwapChainD3D12TagFrameResource _xefgSwapChainD3D12TagFrameResource = nullptr;
    inline static PFN_xefgSwapChainD3D12SetDescriptorHeap _xefgSwapChainD3D12SetDescriptorHeap = nullptr;
    inline static PFN_xefgSwapChainD3D12UpdateExternalHeapOnResize _xefgSwapChainD3D12UpdateExternalHeapOnResize =
        nullptr;
    inline static PFN_xefgSwapChainD3D12GetInitializationParameters _xefgSwapChainD3D12GetInitializationParameters =
        nullptr;

    // Debug
    inline static PFN_xefgSwapChainEnableDebugFeature _xefgSwapChainEnableDebugFeature = nullptr;

    inline static xefg_swapchain_version_t GetDLLVersion(std::wstring dllPath)
    {
        xefg_swapchain_version_t xefgVersion {};
        version_t tempVersion {};
        auto result = Util::GetFileVersion(dllPath, &tempVersion);

        // Don't assume that the structs are identical
        if (result)
        {
            xefgVersion.major = tempVersion.major;
            xefgVersion.minor = tempVersion.minor;
            xefgVersion.patch = tempVersion.patch;
            xefgVersion.reserved = tempVersion.reserved;
        }

        return xefgVersion;
    }

    inline static std::filesystem::path DllPath(HMODULE module)
    {
        static std::filesystem::path dll;

        if (dll.empty())
        {
            wchar_t dllPath[MAX_PATH];
            GetModuleFileNameW(module, dllPath, MAX_PATH);
            dll = std::filesystem::path(dllPath);
        }

        return dll;
    }

  public:
    static HMODULE Module() { return _dll; }
    static HMODULE MemModule() { return _memModule; }
    static std::wstring Module_Path() { return _dllPath; }

    static bool InitXeFG()
    {
        if (_dll != nullptr)
            return true;

        HMODULE mainModule = nullptr;

        std::vector<std::wstring> dllNames = { L"libxess_fg.dll" };

        auto optiPath = Config::Instance()->MainDllPath.value();

        // FG Output F5Low: our f5low.dll from the plugins folder only, never Intel's library in its place
        if (State::Instance().activeFgOutput == FGOutput::F5Low)
        {
            // One try per session (the output does not change without a restart); a second caller waits for it
            static std::once_flag f5lowOnce;
            static HMODULE f5lowModule = nullptr;
            std::call_once(f5lowOnce, [] { f5lowModule = F5Low::LoadLibraryForOutput(); });

            mainModule = f5lowModule;
            if (mainModule == nullptr)
                return false;

            _dllPath = F5Low::LibraryPath();
            return HookXeFG(mainModule);
        }

        for (size_t i = 0; i < dllNames.size(); i++)
        {
            LOG_DEBUG("Trying to load {}", wstring_to_string(dllNames[i]));

            auto overridePath = Config::Instance()->XeFGLibrary.value_or(L"");

            HMODULE memModule = nullptr;
            Util::LoadProxyLibrary(dllNames[i], optiPath, overridePath, &memModule, &mainModule);

            if (mainModule != nullptr)
            {
                _memModule = memModule;
                break;
            }
        }

        if (mainModule != nullptr)
        {
            wchar_t modulePath[MAX_PATH];
            DWORD len = GetModuleFileNameW(mainModule, modulePath, MAX_PATH);
            _dllPath = std::wstring(modulePath);

            LOG_INFO("Loaded from {}", wstring_to_string(_dllPath));

            XeModuleInfo::CheckXeFGPath();
            return HookXeFG(mainModule);
        }

        XeModuleInfo::CheckXeFGPath();
        return false;
    }

    static bool HookXeFG(HMODULE libxefgModule)
    {
        // if dll already loaded
        if (_dll != nullptr && _xefgSwapChainGetVersion != nullptr)
            return true;

        spdlog::info("");

        if (libxefgModule == nullptr)
            return false;

        _dll = libxefgModule;

        {
            ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};

            if (_dll != nullptr)
            {
                // Common
                _xefgSwapChainGetVersion =
                    (PFN_xefgSwapChainGetVersion) KernelBaseProxy::GetProcAddress_()(_dll, "xefgSwapChainGetVersion");
                _xefgSwapChainGetProperties = (PFN_xefgSwapChainGetProperties) KernelBaseProxy::GetProcAddress_()(
                    _dll, "xefgSwapChainGetProperties");
                _xefgSwapChainTagFrameConstants =
                    (PFN_xefgSwapChainTagFrameConstants) KernelBaseProxy::GetProcAddress_()(
                        _dll, "xefgSwapChainTagFrameConstants");
                _xefgSwapChainSetEnabled =
                    (PFN_xefgSwapChainSetEnabled) KernelBaseProxy::GetProcAddress_()(_dll, "xefgSwapChainSetEnabled");
                _xefgSwapChainSetPresentId = (PFN_xefgSwapChainSetPresentId) KernelBaseProxy::GetProcAddress_()(
                    _dll, "xefgSwapChainSetPresentId");
                _xefgSwapChainGetLastPresentStatus =
                    (PFN_xefgSwapChainGetLastPresentStatus) KernelBaseProxy::GetProcAddress_()(
                        _dll, "xefgSwapChainGetLastPresentStatus");
                _xefgSwapChainSetLoggingCallback =
                    (PFN_xefgSwapChainSetLoggingCallback) KernelBaseProxy::GetProcAddress_()(
                        _dll, "xefgSwapChainSetLoggingCallback");
                _xefgSwapChainDestroy =
                    (PFN_xefgSwapChainDestroy) KernelBaseProxy::GetProcAddress_()(_dll, "xefgSwapChainDestroy");
                _xefgSwapChainSetLatencyReduction =
                    (PFN_xefgSwapChainSetLatencyReduction) KernelBaseProxy::GetProcAddress_()(
                        _dll, "xefgSwapChainSetLatencyReduction");
                // Not Intel's: a frame generation library behind XeFG's API that does not tell XeLL about its frames
                _xellCountsRenderedOnly = (PFN_XellCountsRenderedOnly) KernelBaseProxy::GetProcAddress_()(
                    _dll, "xefgCudaXellCountsRenderedOnly");
                if (_xellCountsRenderedOnly != nullptr)
                    LOG_INFO("XeFG library does not tell XeLL about its generated frames while it runs its own: a cap "
                             "XeLL keeps then goes per rendered frame");
                // Not Intel's: a library that runs NVIDIA's DLSS-G behind XeFG's API (xefg_cuda Generator=dlssg)
                _driverMultipliesReflexCap = (PFN_DriverMultipliesReflexCap) KernelBaseProxy::GetProcAddress_()(
                    _dll, "xefgCudaDriverMultipliesReflexCap");
                // Asked once now too: the library logs a warning when an OptiScaler never asks
                if (_driverMultipliesReflexCap != nullptr)
                    _driverMultipliesReflexCap();
                // Not Intel's: tells f5low.dll once that this OptiScaler hands the library's async frame markers on as
                // the library's own, not the game's Reflex calls (IsFgLibraryMarker), so its DriverPacing default can
                // be automatic. Before any context is made: the contexts are created through the procs looked up below,
                // and f5low reads the answer when it creates one.
                if (const auto routesMarkers =
                        (PFN_HostRoutesMarkers) KernelBaseProxy::GetProcAddress_()(_dll, "f5lowHostRoutesMarkers");
                    routesMarkers != nullptr)
                    routesMarkers();
                _xefgSwapChainSetSceneChangeThreshold =
                    (PFN_xefgSwapChainSetSceneChangeThreshold) KernelBaseProxy::GetProcAddress_()(
                        _dll, "xefgSwapChainSetSceneChangeThreshold");
                _xefgSwapChainGetPipelineBuildStatus =
                    (PFN_xefgSwapChainGetPipelineBuildStatus) KernelBaseProxy::GetProcAddress_()(
                        _dll, "xefgSwapChainGetPipelineBuildStatus");
                _xefgSwapChainSetNumInterpolatedFrames =
                    (PFN_xefgSwapChainSetNumInterpolatedFrames) KernelBaseProxy::GetProcAddress_()(
                        _dll, "xefgSwapChainSetNumInterpolatedFrames");
                _xefgSwapChainSetUiCompositionState =
                    (PFN_xefgSwapChainSetUiCompositionState) KernelBaseProxy::GetProcAddress_()(
                        _dll, "xefgSwapChainSetUiCompositionState");

                // Dx12
                _xefgSwapChainD3D12CreateContext =
                    (PFN_xefgSwapChainD3D12CreateContext) KernelBaseProxy::GetProcAddress_()(
                        _dll, "xefgSwapChainD3D12CreateContext");
                _xefgSwapChainD3D12BuildPipelines =
                    (PFN_xefgSwapChainD3D12BuildPipelines) KernelBaseProxy::GetProcAddress_()(
                        _dll, "xefgSwapChainD3D12BuildPipelines");
                _xefgSwapChainD3D12GetProperties =
                    (PFN_xefgSwapChainD3D12GetProperties) KernelBaseProxy::GetProcAddress_()(
                        _dll, "xefgSwapChainD3D12GetProperties");
                _xefgSwapChainD3D12InitFromSwapChain =
                    (PFN_xefgSwapChainD3D12InitFromSwapChain) KernelBaseProxy::GetProcAddress_()(
                        _dll, "xefgSwapChainD3D12InitFromSwapChain");
                _xefgSwapChainD3D12InitFromSwapChainDesc =
                    (PFN_xefgSwapChainD3D12InitFromSwapChainDesc) KernelBaseProxy::GetProcAddress_()(
                        _dll, "xefgSwapChainD3D12InitFromSwapChainDesc");
                _xefgSwapChainD3D12GetSwapChainPtr =
                    (PFN_xefgSwapChainD3D12GetSwapChainPtr) KernelBaseProxy::GetProcAddress_()(
                        _dll, "xefgSwapChainD3D12GetSwapChainPtr");
                _xefgSwapChainD3D12TagFrameResource =
                    (PFN_xefgSwapChainD3D12TagFrameResource) KernelBaseProxy::GetProcAddress_()(
                        _dll, "xefgSwapChainD3D12TagFrameResource");
                _xefgSwapChainD3D12SetDescriptorHeap =
                    (PFN_xefgSwapChainD3D12SetDescriptorHeap) KernelBaseProxy::GetProcAddress_()(
                        _dll, "xefgSwapChainD3D12SetDescriptorHeap");
                _xefgSwapChainD3D12UpdateExternalHeapOnResize =
                    (PFN_xefgSwapChainD3D12UpdateExternalHeapOnResize) KernelBaseProxy::GetProcAddress_()(
                        _dll, "xefgSwapChainD3D12UpdateExternalHeapOnResize");
                _xefgSwapChainD3D12GetInitializationParameters =
                    (PFN_xefgSwapChainD3D12GetInitializationParameters) KernelBaseProxy::GetProcAddress_()(
                        _dll, "xefgSwapChainD3D12GetInitializationParameters");

                // Debug
                _xefgSwapChainEnableDebugFeature =
                    (PFN_xefgSwapChainEnableDebugFeature) KernelBaseProxy::GetProcAddress_()(
                        _dll, "xefgSwapChainEnableDebugFeature");
            }
        }

        bool loadResult = _xefgSwapChainGetVersion != nullptr;
        LOG_INFO("LoadResult: {}", loadResult);
        return loadResult;
    }

    static xefg_swapchain_version_t Version()
    {
        if (_xefgVersion.major == 0 && _xefgSwapChainGetVersion != nullptr)
        {
            if (auto result = _xefgSwapChainGetVersion(&_xefgVersion); result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
            {
                LOG_INFO("XeFG Version: v{}.{}.{}", _xefgVersion.major, _xefgVersion.minor, _xefgVersion.patch);
            }
            else
            {
                LOG_ERROR("Can't get XeFG version: {}", (UINT) result);
            }
        }

        if (_xefgVersion.major == 0)
        {
            _xefgVersion.major = 1;
            _xefgVersion.minor = 0;
            _xefgVersion.patch = 0;
        }

        return _xefgVersion;
    }

    // Common
    static PFN_xefgSwapChainGetVersion GetVersion() { return _xefgSwapChainGetVersion; }
    static PFN_xefgSwapChainGetProperties GetProperties() { return _xefgSwapChainGetProperties; }
    static PFN_xefgSwapChainTagFrameConstants TagFrameConstants() { return _xefgSwapChainTagFrameConstants; }
    static PFN_xefgSwapChainSetEnabled SetEnabled() { return _xefgSwapChainSetEnabled; }
    static PFN_xefgSwapChainSetPresentId SetPresentId() { return _xefgSwapChainSetPresentId; }
    static PFN_xefgSwapChainGetLastPresentStatus GetLastPresentStatus() { return _xefgSwapChainGetLastPresentStatus; }
    static PFN_xefgSwapChainSetLoggingCallback SetLoggingCallback() { return _xefgSwapChainSetLoggingCallback; }
    static PFN_xefgSwapChainDestroy Destroy() { return _xefgSwapChainDestroy; }
    static PFN_xefgSwapChainSetLatencyReduction SetLatencyReduction() { return _xefgSwapChainSetLatencyReduction; }

    // Intel's library tells XeLL about the frames it generates, and XeLL's cap counts shown frames (Intel: "XeSS-FG
    // Support"). A replacement such as xefg_cuda that does not says so (xefgCudaXellCountsRenderedOnly, while its own
    // context runs): XeLL then caps the game's own frames.
    static bool XellCountsShownFrames() { return _xellCountsRenderedOnly == nullptr || _xellCountsRenderedOnly() == 0; }
    // A library that runs DLSS-G behind XeFG's API (xefgCudaDriverMultipliesReflexCap, while it does): the NVIDIA
    // driver multiplies a Reflex interval by DLSS-G's multiplier itself (NBA 2K27: 33333 us at 3x read back as 99999
    // us), so a shown-frame cap goes to real Reflex undivided, as for a game's own DLSS-G (CapForGame)
    static bool DriverMultipliesReflexCap()
    {
        return _driverMultipliesReflexCap != nullptr && _driverMultipliesReflexCap() != 0;
    }
    static PFN_xefgSwapChainSetSceneChangeThreshold SetSceneChangeThreshold()
    {
        return _xefgSwapChainSetSceneChangeThreshold;
    }
    static PFN_xefgSwapChainGetPipelineBuildStatus GetPipelineBuildStatus()
    {
        return _xefgSwapChainGetPipelineBuildStatus;
    }
    static PFN_xefgSwapChainSetNumInterpolatedFrames SetNumInterpolatedFrames()
    {
        return _xefgSwapChainSetNumInterpolatedFrames;
    }
    static PFN_xefgSwapChainSetUiCompositionState SetUiCompositionState()
    {
        return _xefgSwapChainSetUiCompositionState;
    }

    // Dx12
    static PFN_xefgSwapChainD3D12CreateContext D3D12CreateContext() { return _xefgSwapChainD3D12CreateContext; }
    static PFN_xefgSwapChainD3D12BuildPipelines D3D12BuildPipelines() { return _xefgSwapChainD3D12BuildPipelines; }
    static PFN_xefgSwapChainD3D12GetProperties D3D12GetProperties() { return _xefgSwapChainD3D12GetProperties; }
    static PFN_xefgSwapChainD3D12InitFromSwapChain D3D12InitFromSwapChain()
    {
        return _xefgSwapChainD3D12InitFromSwapChain;
    }
    static PFN_xefgSwapChainD3D12InitFromSwapChainDesc D3D12InitFromSwapChainDesc()
    {
        return _xefgSwapChainD3D12InitFromSwapChainDesc;
    }
    static PFN_xefgSwapChainD3D12GetSwapChainPtr D3D12GetSwapChainPtr() { return _xefgSwapChainD3D12GetSwapChainPtr; }
    static PFN_xefgSwapChainD3D12TagFrameResource D3D12TagFrameResource()
    {
        return _xefgSwapChainD3D12TagFrameResource;
    }
    static PFN_xefgSwapChainD3D12SetDescriptorHeap D3D12SetDescriptorHeap()
    {
        return _xefgSwapChainD3D12SetDescriptorHeap;
    }
    static PFN_xefgSwapChainD3D12UpdateExternalHeapOnResize D3D12UpdateExternalHeapOnResize()
    {
        return _xefgSwapChainD3D12UpdateExternalHeapOnResize;
    }
    static PFN_xefgSwapChainD3D12GetInitializationParameters D3D12GetInitializationParameters()
    {
        return _xefgSwapChainD3D12GetInitializationParameters;
    }

    // Debug
    static PFN_xefgSwapChainEnableDebugFeature EnableDebugFeature() { return _xefgSwapChainEnableDebugFeature; }
};
