#pragma once

// NR profiles: the Neural Rendering settings saved under a name in OptiScaler\NRProfiles\<name>.ini and loaded back.
// Plain data and rules, no config and no ImGui, so tests/nr_profiles_smoke.cpp runs them; Config.cpp writes and reads
// the files, dlssnr/DlssNr_Menu.cpp draws the NR Profiles section.
//
// A profile is made to be shared for one game: it keeps how NR looks and what it costs (what the preset rows set,
// passes, colour, output, input style, reuse, LUT) and the game's own settings (exposure, colour encoding, Finished
// Picture, Optical F5Low tuning, edge compression). It records the game it was saved in, and the menu warns when it is
// loaded in another; there the Optical F5Low tuning stays as it is (KeepNative). It leaves out what is the player's own
// or only for the session: NR's own on/off, the toggle key, the display's paper white, the Optical F5Low mode, debug
// and compare views, diagnostics. Every key Config::SaveIni writes is in exactly one of the two lists; the test fails
// on a new key that is in neither.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace DlssNrProfiles
{
inline constexpr const wchar_t* kFolderName = L"NRProfiles"; // inside the OptiScaler folder, beside LUTs
inline constexpr std::size_t kMaxNameLength = 48;
inline constexpr int kFileVersion = 1;                     // [Profile] Version
inline constexpr std::uintmax_t kMaxFileBytes = 64 * 1024; // a real profile is ~5 KB; anything larger is not read

// [DlssNr] keys a profile keeps. Pass4..Pass30 keys (written in a loop) are kept too, see IsKept.
inline constexpr std::string_view kKeptKeys[] = {
    // placement and model
    "RunBeforeSR",
    "DeferredDLSS",
    "FinishedPicture",
    "Precision",
    "Preset",
    "WorkingScale",
    "ModelResolutionAuto",
    "ReducedUpscaleMethod",
    "Transfer",
    "SuperiorScalingPreset",
    "SuperiorScalingDisableIsHDR",
    "ScalingDownscaler",
    // reuse
    "VitEvery",
    "VitEveryPlain",
    "DetailReuse",
    "DetailReuseSteady",
    "DetailReuseFill",
    "DetailReuseInterval",
    "DetailReuseSpread",
    "DetailReuseDepthTolerance",
    "DetailReuseClipGamma",
    "DetailReuseClipFalloff",
    "DetailReuseSigmaFloor",
    "DetailReuseWithFG",
    "DetailReuseMinFps",
    "DetailReuseMaxDropped",
    "SceneCut",
    "EditCache",
    "EditCacheInterval",
    "EditCacheCrossfade",
    // look
    "TransferStrength",
    "ColourStrength",
    "ReplaceDetailStrength",
    "MaxRatio",
    "ReversibleMode",
    "Passes",
    "MultiGpu",
    "MultiGpuPipelined",
    "MultiGpuAutoBalance",
    "MultiGpuHelperShare",
    "MultiGpuQueueWait",
    "NetworkSplit",
    "NetworkSplitPass",
    "NetworkSplitPlan",
    "PrimaryPasses",
    "SecondaryPassMask",
    "SecondaryModel",
    "PassFeedback",
    "UnlockPasses",
    "Style",
    "Intensity",
    "LocalStructure",
    "LocalTone",
    "SkinStructure",
    "AutoMask",
    "Pass2Preset",
    "Pass2Style",
    "Pass2Intensity",
    "Pass2LocalStructure",
    "Pass2LocalTone",
    "Pass2SkinStructure",
    "Pass2AutoMask",
    "Pass3Preset",
    "Pass3Style",
    "Pass3Intensity",
    "Pass3LocalStructure",
    "Pass3LocalTone",
    "Pass3SkinStructure",
    "Pass3AutoMask",
    "SkinProtection",
    "SkinToneEnabled",
    "SkinDetail",
    "SkinColour",
    "EnvironmentDetail",
    "EnvironmentColour",
    "DepthFade",
    "DepthFadeFarStart",
    "DepthFadeFarEnd",
    "DepthFadeFarStrength",
    "DepthFadeNearStart",
    "DepthFadeNearEnd",
    "DepthFadeNearStrength",
    "DepthFadeSkyStrength",
    // input style and grade
    "InputFlatten",
    "InputFlattenStrength",
    "InputStructureHint",
    "InputStructureStrength",
    "InputStyleFacesOnly",
    "LutFile",
    "LutStrength",
    "PostLutFile1",
    "PostLutStrength1",
    "PostLutFile2",
    "PostLutStrength2",
    "PostLutFile3",
    "PostLutStrength3",
    "PostLutFile4",
    "PostLutStrength4",
    "PostLutInput1",
    "PostLutInput2",
    "PostLutInput3",
    "PostLutInput4",
    "PostLutEnabled1",
    "PostLutEnabled2",
    "PostLutEnabled3",
    "PostLutEnabled4",
    "PostLutStackEnabled",
    // the game's: exposure and colour encoding
    "WhitePointSource",
    "GameExposureScale",
    "WhitePointTrim",
    "WhitePointScale",
    "AutoExposureTrim",
    "AutoExposureShadowProtection",
    "AutoExposureMeter",
    "AutoExposureMeterLowPercent",
    "AutoExposureMeterHighPercent",
    "AutoExposureAdaptBrighterSeconds",
    "AutoExposureAdaptDarkerSeconds",
    "AutoExposureFollowGame",
    "GameExposureTrimAnchors",
    "AutoExposureTrimAnchors",
    "ScanTrim",
    "ScanAnchorValue",
    "ScanAnchorWhitePoint",
    "ScanAnchors",
    "ScanInverted",
    "ScanMeter",
    "ColourEncoding",
    // the game's: Optical F5Low tuning, the native path and the Vulkan route (a change may need a restart: the pages
    // say which). The mode itself is left out.
    "NativeDepthVkCopyUsage",
    "NativeDepthResolveInList",
    "NativeDxvkVulkan",
    "NativeDepthWarmupFrames",
    "NativeDepthWidth",
    "NativeDepthHeight",
    "NativeDepthScaleX",
    "NativeDepthScaleY",
    "NativeDepthOffsetX",
    "NativeDepthOffsetY",
    "NativeMotionProfile",
    "NativeMotionProfileCustom",
    "NativeUpscalerPass",
    "NativeLowLatencyBoost",
    "NativeLowLatencyDxvkGameThread",
    // the game's: edge compression, laid out for its picture
    "SpatialCompression",
    "SpatialCenterX",
    "SpatialCenterY",
    "SpatialWorkX",
    "SpatialWorkY",
    "SpatialOffsetX",
    "SpatialOffsetY",
    "SpatialShiftX",
    "SpatialShiftY",
};

// [DlssNr] keys a profile leaves out.
inline constexpr std::string_view kLeftOutKeys[] = {
    // the player's own
    "Enabled",
    "ToggleKey",
    "DisplayPaperWhiteNits",
    // the rig's, not the look: which view of a multi-monitor layout NR works on
    "MultiViewScreens",
    "SecondaryFeatures",
    // Native blend and what it replaced (DlssNr_NativeBlend.h): the player's own switch with its put-back
    // values. A profile loaded while it is on ends it (the profile's own placement, curve and skin settings no longer
    // match), so these must not travel with a profile.
    "NativeBlend",
    "NativeBlendWasFinishedPicture",
    "NativeBlendWasReversibleMode",
    "NativeBlendWasSkinProtection",
    "NativeBlendWasReplaceDetail",
    // how OptiScaler is hooked into the game, not the look: the Optical F5Low mode (Off / NR only / NR + frame
    // generation) and the game's-upscaler route. A profile saved with the mode off would otherwise turn off the
    // upscaler and frame generation the mode feeds; one from an older build still has them, and they are not read.
    "NativeDepthFinder",
    "NativeMotion",
    "NativeInput",
    "NativeUpscaler",
    "NativeFrameGenerationOnly",
    // old keys kept for older builds: NativeLowLatency is [Framerate] LowLatencyInput's, InputStyle and
    // InputStyleStrength are written from the input style card (whose keys are kept), WhitePointFromExposure became
    // WhitePointSource
    "NativeLowLatency",
    "InputStyle",
    "InputStyleStrength",
    "WhitePointFromExposure",
    // views, diagnostics, developer overrides, testing
    "DebugView",
    "Compare",
    "CompareSplit",
    "CompareZoom",
    "CompareSwap",
    "CompareTags",
    "TagScale",
    "ShowSkinMask",
    "NativeDepthOverlay",
    "NativeDebugView",
    "NativeDepthLinearize",
    "NativeDepthMultiplier",
    "NativeDepthFarPlane",
    "DetailReuseDebug",
    "EditCacheDebug",
    "ProbeD3D11",
    "FrameStats",
    "KernelProfile",
    "AutoCapture",
    "ScanExposure",
    "RecordExposureScan",
    "UseProxy",
    "ProxyProbe",
    "ApplyModel",
    "HoldFrame",
};

inline bool EqualNoCase(std::string_view a, std::string_view b)
{
    return a.size() == b.size() &&
           std::equal(
               a.begin(), a.end(), b.begin(), [](char x, char y)
               { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
}

// Pass4Style .. Pass30AutoMask (Config::DlssNrExtraPasses)
inline bool IsExtraPassKey(std::string_view key)
{
    if (key.size() < 5 || !EqualNoCase(key.substr(0, 4), "Pass") || key[4] < '1' || key[4] > '9')
        return false;

    std::size_t i = 4;
    int pass = 0;
    while (i < key.size() && i < 6 && std::isdigit(static_cast<unsigned char>(key[i])))
        pass = pass * 10 + (key[i++] - '0');

    if (pass < 4 || pass > 30)
        return false;

    const std::string_view rest = key.substr(i);
    for (std::string_view suffix : { "Style", "Intensity", "LocalStructure", "LocalTone", "SkinStructure", "AutoMask" })
    {
        if (EqualNoCase(rest, suffix))
            return true;
    }
    return false;
}

inline bool IsKept(std::string_view key)
{
    if (IsExtraPassKey(key))
        return true;
    return std::any_of(std::begin(kKeptKeys), std::end(kKeptKeys),
                       [&](std::string_view k) { return EqualNoCase(k, key); });
}

inline bool IsLeftOut(std::string_view key)
{
    return std::any_of(std::begin(kLeftOutKeys), std::end(kLeftOutKeys),
                       [&](std::string_view k) { return EqualNoCase(k, key); });
}

// A profile name as typed: outer spaces trimmed; letters, digits, space, - _ . only; at most kMaxNameLength; not
// starting with '.', not ending with '.'; not a Windows device name (CON, NUL, COM1 ...) with or without an
// extension. Anything else is no name, so a name can never be a path.
inline std::optional<std::string> SanitiseName(std::string_view typed)
{
    const auto first = typed.find_first_not_of(' ');
    if (first == std::string_view::npos)
        return std::nullopt;
    const std::string name(typed.substr(first, typed.find_last_not_of(' ') - first + 1));

    if (name.size() > kMaxNameLength || name.front() == '.' || name.back() == '.')
        return std::nullopt;

    for (const char c : name)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != ' ' && c != '-' && c != '_' && c != '.')
            return std::nullopt;
    }

    std::string stem = name.substr(0, name.find('.'));
    while (!stem.empty() && stem.back() == ' ')
        stem.pop_back();
    for (std::string_view device : { "CON", "PRN", "AUX", "NUL" })
    {
        if (EqualNoCase(stem, device))
            return std::nullopt;
    }
    if (stem.size() == 4 && (EqualNoCase(stem.substr(0, 3), "COM") || EqualNoCase(stem.substr(0, 3), "LPT")) &&
        stem[3] >= '1' && stem[3] <= '9')
        return std::nullopt;

    return name;
}

// <folder>\<name>.ini, only for a name SanitiseName keeps unchanged
inline std::optional<std::filesystem::path> ProfilePath(const std::filesystem::path& folder, std::string_view name)
{
    const auto clean = SanitiseName(name);
    if (!clean || *clean != name)
        return std::nullopt;

    auto path = folder / std::filesystem::path(*clean + ".ini");
    if (path.parent_path() != folder)
        return std::nullopt;
    return path;
}

// A wide name as plain ASCII, or nothing when it has any other character. Profile names are ASCII (SanitiseName), and
// path::string() would throw on a name the code page cannot hold (a shared "Профиль.ini"), so names are read wide.
inline std::optional<std::string> AsciiName(const std::wstring& wide)
{
    std::string narrow;
    for (const wchar_t c : wide)
    {
        if (c <= 0 || c > 127)
            return std::nullopt;
        narrow.push_back(static_cast<char>(c));
    }
    return narrow;
}

// The profile name of a file in the folder: its stem, for a .ini whose stem is a valid name
inline std::optional<std::string> NameFromPath(const std::filesystem::path& path)
{
    const auto extension = AsciiName(path.extension().wstring());
    if (!extension || !EqualNoCase(*extension, ".ini"))
        return std::nullopt;

    const auto stem = AsciiName(path.stem().wstring());
    if (!stem)
        return std::nullopt;

    const auto clean = SanitiseName(*stem);
    if (!clean || *clean != *stem)
        return std::nullopt;
    return clean;
}

// Whether a path is a link (symlink or, on Windows, a junction), which could send a save or a read somewhere else.
// Missing or unreadable counts as not a link.
inline bool IsLink(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto type = std::filesystem::symlink_status(path, ec).type();
    if (ec)
        return false;
#ifdef _MSC_VER
    if (type == std::filesystem::file_type::junction)
        return true;
#endif
    return type == std::filesystem::file_type::symlink;
}

// A [DlssNr] section as key -> value text, the way SaveIni writes it ("auto" for a default)
struct NoCaseLess
{
    bool operator()(const std::string& a, const std::string& b) const
    {
        return std::lexicographical_compare(
            a.begin(), a.end(), b.begin(), b.end(), [](char x, char y)
            { return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y)); });
    }
};
using Section = std::map<std::string, std::string, NoCaseLess>;

// A profile file: its kept [DlssNr] settings and the game it was saved in ([Profile] SavedFrom, the exe's file name)
struct File
{
    Section settings;
    std::string savedFrom;
};

// Whether a profile was saved in this game. A file with no SavedFrom (made by hand) counts as this game's.
inline bool SameGame(std::string_view savedFrom, std::string_view exeName)
{
    return savedFrom.empty() || EqualNoCase(savedFrom, exeName);
}

inline Section Filter(const Section& section)
{
    Section kept;
    for (const auto& [key, value] : section)
    {
        if (IsKept(key))
            kept.emplace(key, value);
    }
    return kept;
}

// The keys that name a LUT file (the Input LUT and the post-NR stack's layers): a profile keeps only the file name.
inline bool IsLutFileKey(const std::string& key)
{
    if (EqualNoCase(key, "LutFile"))
        return true;

    static const char* post[] = { "PostLutFile1", "PostLutFile2", "PostLutFile3", "PostLutFile4" };
    return std::any_of(std::begin(post), std::end(post), [&](const char* name) { return EqualNoCase(key, name); });
}

// Whether two sections hold the same kept settings. A missing key reads as "auto", as the ini reader has it. The LUT
// counts by file name: a profile keeps only the name, the config the path it was found at.
inline bool Same(const Section& a, const Section& b)
{
    const auto valueIn = [](const Section& s, const std::string& key) -> std::string
    {
        const auto it = s.find(key);
        if (it == s.end())
            return "auto";
        return IsLutFileKey(key) ? std::filesystem::path(it->second).filename().string() : it->second;
    };

    for (const Section* s : { &a, &b })
    {
        for (const auto& [key, value] : *s)
        {
            if (IsKept(key) && !EqualNoCase(valueIn(a, key), valueIn(b, key)))
                return false;
        }
    }
    return true;
}

// Whether a load changed an Optical F5Low (Native) setting, which may need a restart
inline bool NativeChanged(const Section& before, const Section& after)
{
    const auto native = [](const Section& s)
    {
        Section only;
        for (const auto& [key, value] : s)
        {
            if (EqualNoCase(std::string_view(key).substr(0, 6), "Native"))
                only.emplace(key, value);
        }
        return only;
    };

    return !Same(native(before), native(after));
}

inline bool IsNativeKey(std::string_view key) { return key.size() >= 6 && EqualNoCase(key.substr(0, 6), "Native"); }

// A profile saved in another game, with this game's Optical F5Low tuning in place of its own: depth size, offsets and
// motion profile are laid out for the game they were found in.
inline Section KeepNative(const Section& profile, const Section& current)
{
    Section out;
    for (const auto& [key, value] : profile)
    {
        if (!IsNativeKey(key))
            out.emplace(key, value);
    }
    for (const auto& [key, value] : current)
    {
        if (IsNativeKey(key) && IsKept(key))
            out.emplace(key, value);
    }
    return out;
}
} // namespace DlssNrProfiles
