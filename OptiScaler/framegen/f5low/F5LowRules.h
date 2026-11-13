#pragma once

#include <optional>
#include <string>
#include <string_view>

// FG Output F5Low: our f5low.dll from the plugins folder (it implements XeFG's API, so OptiScaler drives it
// through the XeFG output code). The Frame Generator choice says who makes the frames inside it. Pure rules,
// host-tested in tests/f5low_output_smoke.cpp; F5Low.cpp feeds them the ini, the GPU and the folder.
//
// In plugins: f5low.dll and f5low.ini. Intel's libxess_fg.dll (what f5low.dll hands over to where it cannot
// run, and the "libxess" generator later) is the one in OptiScaler's folder, passed in XEFG_CUDA_INTEL_LIBRARY. It was
// built as xefg_cuda.dll before 1.0.0; F5Low loads only f5low.dll. The release zip carries f5low.dll
// (package_release.ps1 -F5LowKit) and libxess_fg.dll 1.3.4.95; f5low.dll builds its frame generator from the player's
// libxess_fg.dll on every start, in memory and in the background (about half a second); nothing is written to disk.
// Without f5low.dll F5Low is not available.
namespace F5Low
{
inline constexpr wchar_t kLibraryFile[] = L"f5low.dll";

// f5low.dll reads this with GetEnvironmentVariable when it creates each context; it wins over its own ini.
inline constexpr wchar_t kGeneratorEnv[] = L"XEFG_CUDA_GENERATOR";

// The same choice under f5low.dll's current name, which it reads first; and the Streamline folder for the DLSS-G
// generator.
inline constexpr wchar_t kGeneratorEnvNew[] = L"F5LOW_GENERATOR";
inline constexpr wchar_t kStreamlineDirEnv[] = L"F5LOW_STREAMLINE_DIR";

// Set to 1 when the game's own DLSS-G is meant to stay loaded next to f5low's (KeepGameDlssg, or FG Input DLSSG);
// f5low.dll refuses DLSS-G when it sees a foreign nvngx_dlssg otherwise.
inline constexpr wchar_t kAllowForeignEnv[] = L"F5LOW_DLSSG_ALLOW_FOREIGN";

// The full path of Intel's libxess_fg.dll that f5low.dll hands over to (and later builds on).
inline constexpr wchar_t kIntelLibraryEnv[] = L"XEFG_CUDA_INTEL_LIBRARY";

// [F5Low] FrameGenerator: XeFG Tensor, NVIDIA's DLSS-G and Intel's own XeFG library.
enum class Generator
{
    XefgTensor, // f5low.dll's own generator on the tensor cores (Generator=own)
    Dlssg,      // NVIDIA DLSS-G through f5low.dll's private Streamline copy (Generator=dlssg)
    Intel,      // Intel's own libxess_fg.dll + libxell.dll from OptiScaler's folder, paced by f5low (Generator=intel)
};

inline constexpr Generator kDefaultGenerator = Generator::XefgTensor;

// nullopt: the default. valid turns false for text that is not a generator.
inline std::optional<Generator> ParseGenerator(std::string_view text, bool& valid)
{
    const auto equals = [text](std::string_view word)
    {
        if (text.size() != word.size())
            return false;
        for (size_t index = 0; index < text.size(); ++index)
        {
            const char c =
                text[index] >= 'A' && text[index] <= 'Z' ? static_cast<char>(text[index] - 'A' + 'a') : text[index];
            if (c != word[index])
                return false;
        }
        return true;
    };

    valid = true;
    if (text.empty() || equals("auto"))
        return std::nullopt;
    if (equals("xefg_tensor"))
        return Generator::XefgTensor;
    if (equals("dlssg"))
        return Generator::Dlssg;
    if (equals("intel"))
        return Generator::Intel;
    valid = false;
    return std::nullopt;
}

// The text saved for a value: nullopt (the default) is saved as auto.
inline const char* GeneratorText(std::optional<Generator> generator)
{
    if (!generator.has_value())
        return "auto";
    switch (generator.value())
    {
    case Generator::XefgTensor:
        return "xefg_tensor";
    case Generator::Dlssg:
        return "dlssg";
    case Generator::Intel:
        return "intel";
    }
    return "auto";
}

// What the menu shows.
inline const char* GeneratorLabel(Generator generator)
{
    switch (generator)
    {
    case Generator::XefgTensor:
        return "XeFG Tensor";
    case Generator::Dlssg:
        return "DLSS-G (NVIDIA)";
    case Generator::Intel:
        return "XeFG (Intel library)";
    }
    return "?";
}

// The XEFG_CUDA_GENERATOR value for a generator.
inline const wchar_t* GeneratorEnvValue(Generator generator)
{
    switch (generator)
    {
    case Generator::XefgTensor:
        return L"own";
    case Generator::Dlssg:
        return L"dlssg";
    case Generator::Intel:
        return L"intel";
    }
    return L"own";
}

// DLSS-G runs through f5low.dll's own Streamline copy, made from the files in OptiScaler's streamline folder
// (sl.interposer.dll and the sl.* plugins, plus nvngx_dlssg.dll). Empty when they are there, else why not.
inline const char* DlssgFilesMissing(bool interposerFound, bool dlssgFound)
{
    if (!interposerFound)
        return "DLSS-G needs Streamline (sl.interposer.dll and the sl.* plugins) in OptiScaler's streamline folder";
    if (!dlssgFound)
        return "DLSS-G needs nvngx_dlssg.dll in OptiScaler's streamline folder";
    return "";
}

// The Intel generator runs Intel's own libxess_fg.dll and libxell.dll from OptiScaler's folder (any GPU vendor runs
// them; f5low unlocks up to 6x in memory and paces the frames). Empty when both are there, else why not.
inline const char* IntelFilesMissing(bool libxessFound, bool libxellFound)
{
    if (!libxessFound && !libxellFound)
        return "XeFG (Intel library) needs libxess_fg.dll and libxell.dll in OptiScaler's folder";
    if (!libxessFound)
        return "XeFG (Intel library) needs libxess_fg.dll in OptiScaler's folder";
    if (!libxellFound)
        return "XeFG (Intel library) needs libxell.dll in OptiScaler's folder";
    return "";
}

// Whether the game's own DLSS-G is forced off in slDLSSGSetOptions. F5Low's DLSS-G generator hands the game's
// Streamline F5Low's nvngx_dlssg (redirect); on RTX 20-40 f5low patches that module only when its own context starts,
// so the game must not turn its DLSS-G on. Not with FG Input DLSSG (it needs the game's options), nor with
// KeepGameDlssg, and never for the other generators.
// present: f5low.dll is in the plugins folder; canRun: F5Low can run here with DLSS-G (NVIDIA GPU, Streamline and
// nvngx_dlssg.dll found), so the game never loses its DLSS-G when F5Low cannot take over.
inline bool ForceGameDlssgOff(bool f5lowOutput, Generator generator, bool fgInputDlssg, bool keepGameDlssg,
                              bool present, bool canRun)
{
    return f5lowOutput && generator == Generator::Dlssg && !fgInputDlssg && !keepGameDlssg && present && canRun;
}

// The most interpolated frames F5Low offers: what its library reports (at least 1), capped by [XeFG]
// MaxInterpolations (1-5, 5 = up to 6x). Unlike the XeFG unlock it never goes above what the library reports.
inline int MaxInterpolations(unsigned reported, int configured)
{
    const int library = reported < 1 ? 1 : static_cast<int>(reported);
    const int cap = configured < 1 ? 1 : (configured > 5 ? 5 : configured);
    return library < cap ? library : cap;
}

// Whether F5Low can be offered, and why not (menu tooltip, log). Both generators need an NVIDIA GPU (CUDA, DLSS-G).
struct Availability
{
    bool available = false;
    const char* reason = "";
};

// intelLibraryFound: libxess_fg.dll next to OptiScaler.dll, at XeFGPath or next to f5low.dll. f5low.dll builds its
// frame generator from it on first use and hands over to it where its own generator cannot run, so without it F5Low has
// nothing to run on. The DLSS-G generator does not use it (needsIntelLibrary false).
inline Availability CheckAvailable(bool nvidia, bool libraryFound, bool intelLibraryFound = true,
                                   bool needsIntelLibrary = true)
{
    if (!nvidia)
        return { false, "F5Low frame generation needs an NVIDIA GPU (XeFG Tensor runs on CUDA, DLSS-G on RTX)" };
    if (!libraryFound)
        return { false, "f5low.dll is not in the plugins folder" };
    if (needsIntelLibrary && !intelLibraryFound)
        return { false, "libxess_fg.dll is missing (F5Low builds its frame generator from it)" };
    return { true, "" };
}

// What f5low.dll says it is doing (its export f5lowGetStatus, optional in older builds).
enum class RunState
{
    NotLoaded = -2, // f5low.dll is not loaded (yet)
    Unknown = -1,   // loaded, but without the export (an older f5low.dll)
    Undecided = 0,  // no context made yet
    Own = 1,        // its own generator runs
    HandedOver = 2, // Intel's libxess_fg.dll runs (up to 4x)
    Unavailable = 3 // nothing runs
};

inline RunState RunStateFromCode(int code)
{
    return code >= 0 && code <= 3 ? static_cast<RunState>(code) : RunState::Unknown;
}

// The FG page line for a state.
inline const char* RunStateLabel(RunState state, Generator generator = Generator::XefgTensor)
{
    if (generator == Generator::Dlssg)
    {
        if (state == RunState::Own)
            return "DLSS-G running";
        if (state == RunState::Unavailable)
            return "DLSS-G unavailable, no frames generated";
    }

    if (generator == Generator::Intel)
    {
        if (state == RunState::Own || state == RunState::HandedOver)
            return "XeFG (Intel library) running";
        if (state == RunState::Unavailable)
            return "XeFG (Intel library) unavailable, no frames generated";
    }

    switch (state)
    {
    case RunState::NotLoaded:
        return "f5low.dll not loaded yet";
    case RunState::Undecided:
        return "starting";
    case RunState::Own:
        return "own frame generator running";
    case RunState::HandedOver:
        return "running on Intel's XeFG (up to 4x)";
    case RunState::Unavailable:
        return "unavailable, no frames generated";
    default:
        return "state unknown (older f5low.dll)";
    }
}

// F5Low with the DLSS-G generator runs a Streamline of its own, in plugins\f5low_streamline. A game's own
// nvngx_dlssg.dll loaded next to it makes two DLSS-G instances, and the GPU faulted at level load in a game that did
// so. So that file (any folder but f5low_streamline), and the driver-downloaded DLSS-G (...\models\dlssg\), are kept
// from loading, unless the game's DLSS-G is the FG Input.
inline constexpr wchar_t kPrivateStreamlineFolder[] = L"f5low_streamline";

enum class GameDlssgLoad
{
    Allow,
    Block,
    Redirect, // hand the load our private copy, which is already loaded
    AllowWarn // allowed, but two DLSS-G copies may fault: the game's DLSS-G is the FG Input
};

namespace detail
{
// Lower case, one kind of separator
inline std::wstring Normalised(std::wstring_view text)
{
    std::wstring out(text);
    for (auto& c : out)
    {
        if (c >= L'A' && c <= L'Z')
            c = static_cast<wchar_t>(c - L'A' + L'a');
        else if (c == L'/')
            c = L'\\';
    }
    return out;
}
} // namespace detail

// Whether a path is a file inside a folder (any depth).
inline bool IsInsideFolder(std::wstring_view path, std::wstring_view folderPath)
{
    const std::wstring file = detail::Normalised(path);
    std::wstring folder = detail::Normalised(folderPath);
    while (!folder.empty() && folder.back() == L'\\')
        folder.pop_back();
    if (folder.empty())
        return false;
    folder += L'\\';
    return file.size() > folder.size() && file.compare(0, folder.size(), folder) == 0;
}

// modulePath: what the load asked for (a bare name or a full path). privateFolder: the full path of f5low_streamline,
// empty when unknown (then every copy counts as the game's). privateCopyLoaded: a module named nvngx_dlssg.dll is
// already loaded from privateFolder: a bare-name load then gets that one from the loader (intended: it is let
// through), and a load of the game's copy by path is redirected to it instead of failing. privateFileExists: our file
// is there but not loaded yet: the redirect loads it. A hard block would make NGX mark DLSS-G unavailable for
// every Streamline in the process, f5low's included, so Block is only for when we have no file to hand out.
inline GameDlssgLoad GameDlssgLoadRule(bool f5lowOutput, Generator generator, bool keepGameDlssg, bool fgInputDlssg,
                                       std::wstring_view modulePath, std::wstring_view privateFolder,
                                       bool privateCopyLoaded = false, bool privateFileExists = false)
{
    if (!f5lowOutput || generator != Generator::Dlssg || keepGameDlssg)
        return GameDlssgLoad::Allow;

    const std::wstring path = detail::Normalised(modulePath);
    const size_t slash = path.find_last_of(L'\\');
    const std::wstring_view file =
        slash == std::wstring::npos ? std::wstring_view(path) : std::wstring_view(path).substr(slash + 1);
    const bool dlssgFile = file == L"nvngx_dlssg.dll" || file == L"nvngx_dlssg";
    const bool driverCopy = path.find(L"\\models\\dlssg\\") != std::wstring::npos; // the driver's OTA download
    if (!dlssgFile && !driverCopy)
        return GameDlssgLoad::Allow;

    if (IsInsideFolder(path, privateFolder))
        return GameDlssgLoad::Allow; // f5low.dll's own copy

    if (dlssgFile && slash == std::wstring::npos && privateCopyLoaded)
        return GameDlssgLoad::Allow; // a bare name: the loader hands out the loaded private copy

    if (fgInputDlssg)
        return GameDlssgLoad::AllowWarn;

    return privateCopyLoaded || privateFileExists ? GameDlssgLoad::Redirect : GameDlssgLoad::Block;
}
} // namespace F5Low
