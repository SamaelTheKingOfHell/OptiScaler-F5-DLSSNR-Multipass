#pragma once

#include "F5LowRules.h"

#include <Windows.h>

#include <string>

// FG Output F5Low at run time: where its library is, whether it can run here, and which generator it uses.
namespace F5Low
{
// The startup nvapi check's answer; DXGI cannot be asked inside DLL_PROCESS_ATTACH, where XeFG is first loaded.
void SetStartupNvidia(bool nvidia);

// <plugins folder>\f5low.dll
std::wstring LibraryPath();

// The Intel libxess_fg.dll OptiScaler's XeFG output would use (XeFGPath, else OptiScaler's folder), for f5low.dll;
// empty when there is none
std::wstring IntelLibraryPath();

// What f5low.dll reports (f5lowGetStatus) with its one-line reason. Asks the module LoadLibraryForOutput loaded;
// NotLoaded before that, Unknown when that f5low.dll has no such export.
struct Status
{
    RunState state = RunState::NotLoaded;
    std::string reason;
};
Status QueryStatus();

// Logs QueryStatus once per change (call after a context was made).
void LogStatus();

// A once-per-session toast for an F5Low that cannot generate frames, plus the log line.
void ReportFailure(const char* what);

// Whether F5Low can run on this PC (NVIDIA GPU, f5low.dll and libxess_fg.dll present), with the reason when not.
Availability Available();

// Loads f5low.dll from the plugins folder (XeFGPath is the XeFG output's, not used here). Null when F5Low is not
// available or the load failed; the reason is logged.
HMODULE LoadLibraryForOutput();

// Reads FGOutput, the Frame Generator and KeepGameDlssg once and keeps that copy for the session (done at startup, or
// on first use). ApplyGenerator and KeepsGameDlssgOut both use it; a menu change of the Frame Generator therefore takes
// effect after a restart.
void LatchSettings();

// The Frame Generator this session runs (what LatchSettings kept), not what the menu shows now.
Generator LatchedGenerator();

// With the DLSS-G generator: asks the loaded f5low.dll (f5lowPrepareDlssg) to make its private copy and load its
// nvngx_dlssg before the game's Streamline starts, so the redirect always has our module. Older f5low.dll without the
// export: logged, nothing else.
void PrepareDlssg(HMODULE f5low);

// Tells f5low.dll which generator to run (F5LOW_GENERATOR and XEFG_CUDA_GENERATOR, so a leftover user variable cannot
// disagree) and where OptiScaler's streamline folder is (F5LOW_STREAMLINE_DIR), before XeFG's API creates an F5Low
// context. Only F5Low sets it: the XeFG output with a hand-set XeFGPath to f5low.dll keeps following its own f5low.ini.
void ApplyGenerator();

// Why the DLSS-G generator cannot run (OptiScaler's streamline folder lacks Streamline or nvngx_dlssg.dll); empty when
// it can.
std::string DlssgFilesMissing();

// Why the Intel generator cannot run (libxess_fg.dll or libxell.dll missing in OptiScaler's folder); empty when both
// are there. Does not look at the GPU vendor.
std::string IntelFilesMissing();

// Whether slDLSSGSetOptions forces the game's own DLSS-G off (F5Low + DLSS-G generator, FG Input not DLSSG,
// KeepGameDlssg off), from the settings latched at startup.
bool ForcesGameDlssgOff();

// What a load of this module (a bare name or a full path) gets while F5Low runs DLSS-G: nullptr to load it as asked,
// kBlockedLoad when it is the game's own (or the driver's) nvngx_dlssg that must stay out, or our private copy's handle
// (a reference taken) when that one is already loaded: the game then shares it, as a bare-name load of nvngx_dlssg.dll
// does through the loader (intended). Decided from the ini values latched at startup, so it holds before the game
// loads Streamline. Never for dataOnly loads (AS_DATAFILE and the like), when F5Low cannot run here (NVIDIA GPU,
// f5low.dll, Streamline and nvngx_dlssg.dll in OptiScaler's streamline folder), or while that is not known yet.
// Logs once per path.
HMODULE FilterGameDlssg(const std::wstring& modulePath, bool dataOnly);

// LoadLibraryCheckW's answer for FilterGameDlssg's block. Not the 1337 of the older blocks (overlays and so on): only
// this one fails the load on the kernel32 path too.
inline const HMODULE kBlockedLoad = reinterpret_cast<HMODULE>(static_cast<uintptr_t>(1338));
} // namespace F5Low
