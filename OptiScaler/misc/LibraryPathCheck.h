#pragma once

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <string>

// Did a library come from the path the ini names for it? Users most often get this wrong (XeFGPath = .\OptiScaler\...
// is read from the game process's working folder, which is not always the folder with OptiScaler.ini), and the loader
// then falls back to another copy without a word. Pure std, so it is unit tested on the host.
namespace LibraryPathCheck
{
enum class Result
{
    NotSet,   // the ini has no path (empty or "auto"): nothing to check
    Used,     // the loaded file is the configured one, or sits in the configured folder
    NotFound, // the configured file does not exist
    NotUsed,  // it exists, but another file is loaded (or none)
};

struct Verdict
{
    Result result = Result::NotSet;
    std::string message; // for the toast and the log; empty unless NotFound or NotUsed
};

inline std::string Utf8(const std::wstring& text)
{
    const auto u8 = std::filesystem::path(text).u8string();
    return std::string(u8.begin(), u8.end());
}

inline bool IsAuto(const std::wstring& text)
{
    std::wstring lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return (wchar_t) std::towlower(c); });
    return lower.empty() || lower == L"auto";
}

// what: "XeFG"; fileName: the library's own file name ("libxess_fg.dll"), used when the ini names a folder;
// configured: the ini value; loadedPath: the file that was loaded, empty when none.
inline Verdict Check(const std::string& what, const std::wstring& fileName, const std::wstring& configured,
                     const std::wstring& loadedPath)
{
    Verdict verdict;

    if (IsAuto(configured))
        return verdict;

    std::error_code error;
    std::filesystem::path wanted(configured);
    const bool relative = wanted.is_relative();

    if (std::filesystem::is_directory(wanted, error))
        wanted /= fileName;

    const std::string shown = Utf8(configured);
    const std::string loaded = loadedPath.empty() ? std::string() : Utf8(loadedPath);
    const std::string instead =
        loaded.empty() ? what + " could not be loaded at all." : "Using " + loaded + " instead.";

    if (!std::filesystem::exists(wanted, error))
    {
        verdict.result = Result::NotFound;
        verdict.message = what + "Path is set to \"" + shown + "\", but there is no file there. " + instead;

        if (relative)
            verdict.message += " A relative path is read from the game's working folder: use the full path.";

        return verdict;
    }

    if (!loadedPath.empty() && std::filesystem::equivalent(wanted, loadedPath, error))
    {
        verdict.result = Result::Used;
        return verdict;
    }

    verdict.result = Result::NotUsed;
    verdict.message = what + "Path points at \"" + shown + "\", but that file was not loaded. " + instead;
    return verdict;
}
} // namespace LibraryPathCheck
