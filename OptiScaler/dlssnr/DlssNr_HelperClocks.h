#pragma once
// [DlssNr] HelperFullClocks: keeps the second GPU at full clocks while it helps (worker/WorkerClockProfile.h).
// The driver setting is written by `dlssnr_worker.exe --clock-profile on|off` in its own process, once per change,
// before a helper starts (the driver reads it when the helper creates its device).
//
// ```cpp
// DlssNr::HelperClocks::Ensure(worker, cfg.DlssNrHelperFullClocks.value_or_default()); // before starting a helper
// ```

#include <windows.h>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace DlssNr::HelperClocks
{
inline std::mutex lock;
inline std::optional<bool> applied; // what the driver profile was last set to by this process
inline std::string status = "Not applied yet.";

// The worker beside OptiScaler or the game, as the helpers find it.
inline std::optional<std::filesystem::path> FindWorker(const std::vector<std::filesystem::path>& roots)
{
    for (const auto& root : roots)
        for (const auto& candidate : { root / L"OptiScaler" / L"NR-Worker" / L"dlssnr_worker.exe",
                                       root / L"NR-Worker" / L"dlssnr_worker.exe" })
        {
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error) && !error)
                return candidate;
        }
    return std::nullopt;
}

// Sets the driver profile to `on` unless this process already did. Waits for the worker (about 0.1 to 0.5 s).
inline bool Ensure(const std::filesystem::path& worker, bool on)
{
    std::lock_guard<std::mutex> guard(lock);
    if (applied == on)
        return true;
    if (!on && !applied)
        return true; // never switched on by this process: nothing to remove
    std::wstring command = L"\"" + worker.wstring() + L"\" --clock-profile " + (on ? L"on" : L"off");
    STARTUPINFOW startup { sizeof(startup) };
    PROCESS_INFORMATION process {};
    if (!CreateProcessW(worker.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        worker.parent_path().c_str(), &startup, &process))
    {
        status = "The worker could not be started to change the driver setting.";
        return false;
    }
    DWORD code = STILL_ACTIVE;
    if (WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0)
        GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (code == 0)
    {
        applied = on;
        status = on ? "Driver profile set: the second GPU's worker prefers maximum performance."
                    : "Driver profile removed.";
        return true;
    }
    status = code == 3 ? "NVIDIA's driver interface is unavailable." : "The driver refused the setting.";
    return false;
}
} // namespace DlssNr::HelperClocks
