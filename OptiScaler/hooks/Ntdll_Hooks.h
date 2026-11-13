#pragma once
#include "SysUtils.h"
#include <Util.h>
#include <State.h>
#include <Config.h>
#include <DllNames.h>
#include <framegen/mfgunlock/MfgUnlock.h>
#include <framegen/f5low/F5Low.h>

#include "LibraryLoad_Hooks.h"
#include "Hook_Utils.h"

#include <cwctype>

#pragma intrinsic(_ReturnAddress)

class NtdllHooks
{
  private:
    inline static std::mutex hookMutex;

    inline static NtdllProxy::PFN_RtlGetVersion o_RtlGetVersion = nullptr;
    inline static NtdllProxy::PFN_NtLoadDll o_NtLoadDll = nullptr;
    inline static NtdllProxy::PFN_LdrLoadDll o_LdrLoadDll = nullptr;
    inline static NtdllProxy::PFN_LdrUnloadDll o_LdrUnloadDll = nullptr;
    inline static std::atomic<bool> unloadHookForMfgOnly { false };

    static NTSTATUS ObserveXeFGLoadResult(NTSTATUS result, std::wstring_view name, PHANDLE moduleHandle)
    {
        if (result >= 0 && moduleHandle != nullptr && *moduleHandle != nullptr && MfgUnlock::IsProviderModuleName(name))
            MfgUnlock::ObserveProviderModule(static_cast<HMODULE>(*moduleHandle));
        return result;
    }

    static NTSTATUS NTAPI hkRtlGetVersion(PRTL_OSVERSIONINFOW lpVersionInformation)
    {
        if (lpVersionInformation == nullptr)
            return STATUS_INVALID_PARAMETER;

        auto result = o_RtlGetVersion(lpVersionInformation);

        if (lpVersionInformation->dwMajorVersion <= 10 && lpVersionInformation->dwBuildNumber < 21996 &&
            State::Instance().activeFgOutput == FGOutput::FSRFG && State::Instance().isRunningOnLinux &&
            Util::GetCallerModule(_ReturnAddress()) ==
                KernelBaseProxy::GetModuleHandleW_()(L"amd_fidelityfx_framegeneration_dx12.dll"))
        {
            lpVersionInformation->dwMajorVersion = 10;
            lpVersionInformation->dwBuildNumber = 21996;
        }

        return result;
    }

    static NTSTATUS NTAPI hkLdrLoadDll(PWSTR PathToFile, PULONG Flags, PUNICODE_STRING ModuleFileName,
                                       PHANDLE ModuleHandle)
    {
        if (ModuleHandle == nullptr)
            return STATUS_INVALID_PARAMETER;

        if (ModuleFileName == nullptr || ModuleFileName->Length == 0)
            return o_LdrLoadDll(PathToFile, Flags, ModuleFileName, ModuleHandle);

        std::wstring_view name(ModuleFileName->Buffer, ModuleFileName->Length / sizeof(wchar_t));

#ifdef _DEBUG
        // LOG_DEBUG("{}, caller: {}", wstring_to_string(name.data()), Util::WhoIsTheCaller(_ReturnAddress()));
#endif

        if (State::Instance().isShuttingDown)
            return o_LdrLoadDll(PathToFile, Flags, ModuleFileName, ModuleHandle);

        if (LibraryLoadHooks::IsApiSetName(name))
            return o_LdrLoadDll(PathToFile, Flags, ModuleFileName, ModuleHandle);

        if (State::SkipDllChecks())
        {
            const std::wstring skip = string_to_wstring(State::SkipDllName());

            if (skip.empty() || LibraryLoadHooks::EndsWithInsensitive(name, std::wstring_view(skip)) ||
                LibraryLoadHooks::EndsWithInsensitive(name, std::wstring(skip + L".dll")))
            {
                LOG_TRACE("Skip checks for: {}", wstring_to_string(name.data()));
                return ObserveXeFGLoadResult(o_LdrLoadDll(PathToFile, Flags, ModuleFileName, ModuleHandle), name,
                                             ModuleHandle);
            }
        }

        // if (originalModule != Util::GetCallerModule(_ReturnAddress()))
        {
            auto moduleHandle = LibraryLoadHooks::LoadLibraryCheckW(name.data(), name.data());

            // skip loading of dll
            if (moduleHandle == (HMODULE) 1337 || moduleHandle == F5Low::kBlockedLoad)
            {
                return STATUS_DLL_NOT_FOUND;
            }

            if (moduleHandle != nullptr)
            {
                LOG_TRACE("{}, caller: {}", wstring_to_string(name.data()), Util::WhoIsTheCaller(_ReturnAddress()));
                *ModuleHandle = (HANDLE) moduleHandle;
                return ObserveXeFGLoadResult((NTSTATUS) 0x00000000L, name, ModuleHandle);
            }
        }

        return ObserveXeFGLoadResult(o_LdrLoadDll(PathToFile, Flags, ModuleFileName, ModuleHandle), name,
                                     ModuleHandle);
    }

    static NTSTATUS NTAPI hkNtLoadDll(PUNICODE_STRING PathToFile, PULONG Flags, PUNICODE_STRING ModuleFileName,
                                      PHANDLE ModuleHandle)
    {
        if (ModuleHandle == nullptr)
            return STATUS_INVALID_PARAMETER;

        if (ModuleFileName == nullptr || ModuleFileName->Length == 0)
            return o_NtLoadDll(PathToFile, Flags, ModuleFileName, ModuleHandle);

        std::wstring_view name(ModuleFileName->Buffer, ModuleFileName->Length / sizeof(wchar_t));

#ifdef _DEBUG
        // LOG_DEBUG("{}, caller: {}", wstring_to_string(name.data()), Util::WhoIsTheCaller(_ReturnAddress()));
#endif

        if (State::Instance().isShuttingDown)
            return o_NtLoadDll(PathToFile, Flags, ModuleFileName, ModuleHandle);

        if (LibraryLoadHooks::IsApiSetName(name))
            return o_NtLoadDll(PathToFile, Flags, ModuleFileName, ModuleHandle);

        std::wstring_view path;
        if (PathToFile == nullptr || PathToFile->Length == 0)
            path = name;
        else
            path = std::wstring_view(PathToFile->Buffer, PathToFile->Length / sizeof(wchar_t));

        if (State::SkipDllChecks())
        {
            const std::wstring skip = string_to_wstring(State::SkipDllName());

            if (skip.empty() || LibraryLoadHooks::EndsWithInsensitive(name, std::wstring_view(skip)) ||
                LibraryLoadHooks::EndsWithInsensitive(name, std::wstring(skip + L".dll")))
            {
                LOG_TRACE("Skip checks for: {}", wstring_to_string(name.data()));
                return o_NtLoadDll(PathToFile, Flags, ModuleFileName, ModuleHandle);
            }
        }

        // Do not interfere with original modules calls
        if (originalModule != Util::GetCallerModule(_ReturnAddress()))
        {
            auto moduleHandle = LibraryLoadHooks::LoadLibraryCheckW(name.data(), path.data());

            // skip loading of dll
            if (moduleHandle == (HMODULE) 1337 || moduleHandle == F5Low::kBlockedLoad)
            {
                return STATUS_DLL_NOT_FOUND;
            }

            if (moduleHandle != nullptr)
            {
                LOG_TRACE("{}, caller: {}", wstring_to_string(name.data()), Util::WhoIsTheCaller(_ReturnAddress()));
                *ModuleHandle = (HANDLE) moduleHandle;
                return (NTSTATUS) 0x00000000L;
            }
        }

        return o_NtLoadDll(PathToFile, Flags, ModuleFileName, ModuleHandle);
    }

    static NTSTATUS NTAPI hkLdrUnloadDll(PVOID lpLibrary)
    {
        if (lpLibrary == nullptr)
            return STATUS_INVALID_PARAMETER;

#ifdef _DEBUG
        // LOG_TRACE("{:X}", (size_t) lpLibrary);
#endif

        if (!State::Instance().isShuttingDown)
        {
            // Hooked only for the MFG unlock (UseNtdllHooks off): direct LdrUnloadDll callers get nothing else.
            if (!unloadHookForMfgOnly)
            {
                auto result = LibraryLoadHooks::FreeLibrary(lpLibrary);

                if (result.has_value())
                    return result.value();
            }

            const HMODULE module = static_cast<HMODULE>(lpLibrary);
            const bool trackedProvider = MfgUnlock::IsTrackedProviderModule(module);
            if (trackedProvider && !MfgUnlock::BeforeModuleUnload(module))
                return static_cast<NTSTATUS>(0xC0000001L);

            const NTSTATUS unloadResult = o_LdrUnloadDll(lpLibrary);
            if (trackedProvider)
                MfgUnlock::AfterModuleUnload(module);
            return unloadResult;
        }

        return o_LdrUnloadDll(lpLibrary);
    }

    VALIDATE_MEMBER_HOOK(hkRtlGetVersion, NtdllProxy::PFN_RtlGetVersion)
    VALIDATE_MEMBER_HOOK(hkNtLoadDll, NtdllProxy::PFN_NtLoadDll)
    VALIDATE_MEMBER_HOOK(hkLdrLoadDll, NtdllProxy::PFN_LdrLoadDll)
    VALIDATE_MEMBER_HOOK(hkLdrUnloadDll, NtdllProxy::PFN_LdrUnloadDll)

  public:
    static void Hook()
    {
        std::lock_guard lock(hookMutex);

        LOG_FUNC();

        if (NtdllProxy::Module() == nullptr)
            return;

        if (!Config::Instance()->UseNtdllHooks.value_or_default())
        {
            // Only the MFG unlock needs the unload notification; without it this stays unhooked as before.
            if (o_LdrUnloadDll == nullptr && MfgUnlock::AnyFamilyEnabled())
            {
                unloadHookForMfgOnly = true;
                o_LdrUnloadDll = NtdllProxy::Hook_LdrUnloadDll(hkLdrUnloadDll);
            }
            return;
        }

        if (o_RtlGetVersion == nullptr)
            o_RtlGetVersion = NtdllProxy::Hook_RtlGetVersion(hkRtlGetVersion);

        // if (o_NtLoadDll == nullptr)
        //     o_NtLoadDll = NtdllProxy::Hook_NtLoadDll(hkNtLoadDll);

        if (o_LdrLoadDll == nullptr)
            o_LdrLoadDll = NtdllProxy::Hook_LdrLoadDll(hkLdrLoadDll);

        if (o_LdrUnloadDll == nullptr)
            o_LdrUnloadDll = NtdllProxy::Hook_LdrUnloadDll(hkLdrUnloadDll);
    }
};
