#pragma once
// `dlssnr_worker.exe --clock-profile on|off`: keeps the helper GPU at full clocks while it helps.
//
// The helper works a few milliseconds per frame. NVIDIA's driver reads that as a light load and lowers its clocks within
// seconds (an RTX 3060 measured 1900 MHz falling to 400 to 1200 MHz), which doubles its time and makes it late. The
// driver's per-program "Power management mode: Prefer maximum performance" keeps it at full clocks. This sets that one
// setting for dlssnr_worker.exe only, in a driver profile of its own ("OptiScaler NR helper"). If dlssnr_worker.exe
// already belongs to another profile (one the user made), the setting is written there and that profile is left in
// place when switched off. Run in its own process: OptiScaler hooks NvAPI inside the game.
//
// ```cpp
// return DlssNr::Worker::ClockProfile::Apply(true); // 0 ok, 3 NvAPI unavailable, 4 a driver call failed
// ```

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iostream>
#pragma warning(push, 0)
#include "../../../external/nvapi/nvapi.h"
#include "../../../external/nvapi/NvApiDriverSettings.h"
#pragma warning(pop)

namespace DlssNr::Worker::ClockProfile
{
inline constexpr wchar_t ProfileName[] = L"OptiScaler NR helper";
inline constexpr wchar_t AppName[] = L"dlssnr_worker.exe";

inline void Copy(NvAPI_UnicodeString& to, const wchar_t* from)
{
    std::memset(to, 0, sizeof(to));
    for (size_t i = 0; from[i] && i + 1 < NVAPI_UNICODE_STRING_MAX; ++i)
        to[i] = static_cast<NvU16>(from[i]);
}

inline int Apply(bool on)
{
    HMODULE nvapi = LoadLibraryW(L"nvapi64.dll");
    if (!nvapi)
        return 3;
    using Query = void*(__cdecl*)(unsigned);
    auto query = reinterpret_cast<Query>(GetProcAddress(nvapi, "nvapi_QueryInterface"));
    if (!query)
        return 3;
    auto initialize = reinterpret_cast<NvAPI_Status(__cdecl*)()>(query(0x0150e828));
    auto createSession = reinterpret_cast<NvAPI_Status(__cdecl*)(NvDRSSessionHandle*)>(query(0x0694d52e));
    auto destroySession = reinterpret_cast<NvAPI_Status(__cdecl*)(NvDRSSessionHandle)>(query(0xdad9cff8));
    auto loadSettings = reinterpret_cast<NvAPI_Status(__cdecl*)(NvDRSSessionHandle)>(query(0x375dbd6b));
    auto saveSettings = reinterpret_cast<NvAPI_Status(__cdecl*)(NvDRSSessionHandle)>(query(0xfcbc7e14));
    auto findProfile = reinterpret_cast<NvAPI_Status(__cdecl*)(NvDRSSessionHandle, NvAPI_UnicodeString,
                                                               NvDRSProfileHandle*)>(query(0x7e4a9a0b));
    auto createProfile = reinterpret_cast<NvAPI_Status(__cdecl*)(NvDRSSessionHandle, NVDRS_PROFILE*,
                                                                 NvDRSProfileHandle*)>(query(0xcc176068));
    auto deleteProfile =
        reinterpret_cast<NvAPI_Status(__cdecl*)(NvDRSSessionHandle, NvDRSProfileHandle)>(query(0x17093206));
    auto findApplication = reinterpret_cast<NvAPI_Status(__cdecl*)(NvDRSSessionHandle, NvAPI_UnicodeString,
                                                                   NvDRSProfileHandle*, NVDRS_APPLICATION*)>(
        query(0xeee566b2));
    auto createApplication = reinterpret_cast<NvAPI_Status(__cdecl*)(NvDRSSessionHandle, NvDRSProfileHandle,
                                                                     NVDRS_APPLICATION*)>(query(0x4347a9de));
    auto setSetting = reinterpret_cast<NvAPI_Status(__cdecl*)(NvDRSSessionHandle, NvDRSProfileHandle,
                                                              NVDRS_SETTING*)>(query(0x577dd202));
    if (!initialize || !createSession || !destroySession || !loadSettings || !saveSettings || !findProfile ||
        !createProfile || !deleteProfile || !findApplication || !createApplication || !setSetting ||
        initialize() != NVAPI_OK)
        return 3;

    NvDRSSessionHandle session {};
    if (createSession(&session) != NVAPI_OK)
        return 4;
    int result = 4;
    if (loadSettings(session) == NVAPI_OK)
    {
        NvAPI_UnicodeString profileName {}, appName {};
        Copy(profileName, ProfileName);
        Copy(appName, AppName);
        NvDRSProfileHandle ours {}, owner {};
        const bool haveOurs = findProfile(session, profileName, &ours) == NVAPI_OK;
        NVDRS_APPLICATION app {};
        app.version = NVDRS_APPLICATION_VER;
        const bool listed = findApplication(session, appName, &owner, &app) == NVAPI_OK;
        if (on)
        {
            NvDRSProfileHandle target = listed ? owner : ours;
            NvAPI_Status status = NVAPI_OK;
            if (!target)
            {
                NVDRS_PROFILE profile {};
                profile.version = NVDRS_PROFILE_VER;
                Copy(profile.profileName, ProfileName);
                status = createProfile(session, &profile, &target);
            }
            if (status == NVAPI_OK && !listed)
            {
                NVDRS_APPLICATION add {};
                add.version = NVDRS_APPLICATION_VER;
                Copy(add.appName, AppName);
                Copy(add.userFriendlyName, L"OptiScaler NR helper GPU worker");
                status = createApplication(session, target, &add);
            }
            if (status == NVAPI_OK)
            {
                NVDRS_SETTING setting {};
                setting.version = NVDRS_SETTING_VER;
                setting.settingId = PREFERRED_PSTATE_ID;
                setting.settingType = NVDRS_DWORD_TYPE;
                setting.u32CurrentValue = PREFERRED_PSTATE_PREFER_MAX;
                status = setSetting(session, target, &setting);
            }
            if (status == NVAPI_OK)
                status = saveSettings(session);
            std::cerr << "Clock profile on: " << static_cast<int>(status)
                      << (listed && owner != ours ? " (in the existing profile of dlssnr_worker.exe)" : "") << "\n";
            result = status == NVAPI_OK ? 0 : 4;
        }
        else
        {
            NvAPI_Status status = NVAPI_OK;
            if (haveOurs)
            {
                status = deleteProfile(session, ours);
                if (status == NVAPI_OK)
                    status = saveSettings(session);
            }
            std::cerr << "Clock profile off: " << static_cast<int>(status) << "\n";
            result = status == NVAPI_OK ? 0 : 4;
        }
    }
    destroySession(session);
    return result;
}
} // namespace DlssNr::Worker::ClockProfile
