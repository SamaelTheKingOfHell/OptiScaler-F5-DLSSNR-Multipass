// Host test for dlssnr/DlssNr_Profiles.h: which [DlssNr] keys an NR profile keeps, profile names, and that every key
// Config::SaveIni writes is sorted into kept or left out (so a new setting cannot slip past the choice).
//
// Build: cl /nologo /std:c++20 /EHsc /W4 tests\nr_profiles_smoke.cpp
// Run:   nr_profiles_smoke.exe OptiScaler\Config.cpp
#include <cstdio>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include "../OptiScaler/dlssnr/DlssNr_Profiles.h"

using namespace DlssNrProfiles;

static int fails = 0;
static void Check(bool ok, const std::string& label)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", label.c_str());
        ++fails;
    }
}

static void Names()
{
    Check(SanitiseName("NBA sharp-2") == std::optional<std::string>("NBA sharp-2"), "plain name kept");
    Check(SanitiseName("  Soft v1.2  ") == std::optional<std::string>("Soft v1.2"), "outer spaces trimmed");
    Check(SanitiseName("a_b") == std::optional<std::string>("a_b"), "underscore kept");
    Check(SanitiseName(std::string(kMaxNameLength, 'x')).has_value(), "longest name kept");

    for (const char* bad :
         { "",        "   ",       "..\\x", "a/b", "a\\b",    "c:x",  "a*b",      "a?b", "a\"b", "a<b", "a|b",
           ".hidden", "trailing.", "CON",   "con", "nul.txt", "Com1", "LPT9.ini", "AUX", "PRN",  "a\tb" })
        Check(!SanitiseName(bad).has_value(), std::string("name rejected: '") + bad + "'");

    Check(!SanitiseName(std::string(kMaxNameLength + 1, 'x')).has_value(), "too long rejected");
    Check(SanitiseName("CONSOLE").has_value(), "a name that only starts with a device name is kept");
    Check(SanitiseName("com10").has_value(), "COM10 is not a device name");

    const std::filesystem::path folder = std::filesystem::path("C:\\Games\\X\\OptiScaler") / kFolderName;
    const auto path = ProfilePath(folder, "NBA sharp");
    Check(path.has_value() && path->filename() == "NBA sharp.ini" && path->parent_path() == folder,
          "profile path is <folder>\\<name>.ini");
    Check(!ProfilePath(folder, "..\\..\\evil").has_value(), "a path name gives no path");

    const auto name = NameFromPath(std::filesystem::path("C:\\x\\NRProfiles\\Soft v1.2.ini"));
    Check(name == std::optional<std::string>("Soft v1.2"), "name read back from a file name");
    Check(!NameFromPath(std::filesystem::path("C:\\x\\NRProfiles\\notes.txt")).has_value(), "non-ini file ignored");

    // A shared file named outside the code page: skipped, never thrown on (path::string() would throw on MSVC)
    for (const wchar_t* wide : { L"C:\\x\\NRProfiles\\\u041f\u0440\u043e\u0444\u0438\u043b\u044c.ini",
                                 L"C:\\x\\NRProfiles\\\u65e5\u672c sharp.ini", L"C:\\x\\NRProfiles\\soft.\u0456ni" })
    {
        bool threw = false;
        std::optional<std::string> got;
        try
        {
            got = NameFromPath(std::filesystem::path(wide));
        }
        catch (...)
        {
            threw = true;
        }
        Check(!threw && !got.has_value(), "a non-ASCII file name is skipped without throwing");
    }
    Check(AsciiName(L"Soft v1") == std::optional<std::string>("Soft v1") && !AsciiName(L"caf\u00e9").has_value(),
          "AsciiName keeps ASCII only");
}

static void Keys()
{
    Check(IsKept("Passes") && IsKept("passes") && IsKept("PASSES"), "keys match case-insensitively (as the ini does)");
    Check(IsKept("Pass4Style") && IsKept("Pass30AutoMask") && IsKept("Pass17LocalTone"), "passes 4-30 kept");
    Check(!IsKept("Pass31Style") && !IsKept("Pass3x") && !IsKept("Pass4") && !IsKept("Pass04Style"), "not a pass key");
    Check(IsKept("WhitePointSource") && IsKept("NativeDepthWidth") && IsKept("NativeMotionProfile") &&
              IsKept("ColourEncoding") && IsKept("FinishedPicture") && IsKept("SpatialCenterX"),
          "the game's own settings kept (a profile is shared for one game)");
    for (const char* mode :
         { "NativeDepthFinder", "NativeMotion", "NativeInput", "NativeUpscaler", "NativeFrameGenerationOnly" })
        Check(!IsKept(mode) && IsLeftOut(mode), std::string("Optical F5Low mode key left out: ") + mode);
    Check(Filter({ { "NativeMotion", "false" }, { "NativeUpscaler", "false" }, { "Passes", "2" } }).size() == 1,
          "an older profile's mode keys are not read");

    const Section other { { "Passes", "3" }, { "NativeDepthWidth", "1280" }, { "NativeMotionProfile", "anime" } };
    const Section here { { "Passes", "1" }, { "NativeDepthWidth", "auto" }, { "NativeDepthOffsetX", "4" },
                         { "NativeMotion", "true" } };
    const Section mixed = KeepNative(other, here);
    Check(mixed.at("Passes") == "3" && mixed.at("NativeDepthWidth") == "auto" && mixed.at("NativeDepthOffsetX") == "4" &&
              !mixed.count("NativeMotionProfile") && !mixed.count("NativeMotion"),
          "another game's profile: this game's Optical F5Low tuning, the profile's look");
    Check(!IsKept("Enabled") && !IsKept("MultiViewScreens") && !IsKept("ToggleKey") && !IsKept("DisplayPaperWhiteNits") && !IsKept("HoldFrame") &&
              !IsKept("NativeDebugView") && !IsKept("Compare"),
          "the player's own and session keys left out");
    Check(!IsKept("Unknown"), "unknown key left out");

    Check(SameGame("NBA2K27.exe", "nba2k27.EXE") && SameGame("", "x.exe") && !SameGame("Cyberpunk2077.exe", "x.exe"),
          "same game by exe name, a file without one counts as this game's");
    Check(NativeChanged({ { "NativeDepthWidth", "1280" } }, {}) &&
              !NativeChanged({ { "NativeDepthWidth", "auto" } }, {}) && !NativeChanged({ { "NativeMotion", "true" } }, {}) &&
              !NativeChanged({ { "Passes", "3" } }, { { "Passes", "2" } }),
          "only a Native key change counts as an Optical F5Low change");

    for (auto key : kKeptKeys)
        Check(!IsLeftOut(key), "key in both lists: " + std::string(key));

    Section s { { "Passes", "3" }, { "Enabled", "true" }, { "LutFile", "a.cube" }, { "Bogus", "1" } };
    const Section f = Filter(s);
    Check(f.size() == 2 && f.count("Passes") && f.count("LutFile"), "filter keeps only kept keys");

    Check(Same({ { "Passes", "3" } }, { { "Passes", "3" }, { "Enabled", "false" } }), "left-out keys do not count");
    Check(Same({ { "Passes", "auto" } }, {}), "missing = auto");
    Check(!Same({ { "Passes", "3" } }, { { "Passes", "2" } }), "different value differs");
    Check(Same({ { "passes", "3" } }, { { "Passes", "3" } }), "key case does not count");
    Check(Same({ { "LutFile", "warm.cube" } }, { { "LutFile", "C:\\Games\\X\\OptiScaler\\LUTs\\warm.cube" } }) &&
              !Same({ { "LutFile", "warm.cube" } }, { { "LutFile", "C:\\LUTs\\cold.cube" } }),
          "the LUT counts by file name");

    // The post-NR LUT stack (memory/plans/2026-10-31-nr-lut-chain.md): its files and strengths are kept, by file name.
    for (int i = 1; i <= 4; ++i)
    {
        const std::string file = "PostLutFile" + std::to_string(i);
        const std::string strength = "PostLutStrength" + std::to_string(i);
        const std::string enabled = "PostLutEnabled" + std::to_string(i);
        Check(IsLutFileKey(file) && IsLutFileKey("postlutfile" + std::to_string(i)), "post LUT file key: " + file);
        Check(!IsLeftOut(file) && !IsLeftOut(strength) && !IsLeftOut(enabled), "post LUT keys are not left out: " + file);
        Check(Filter({ { enabled, "false" } }).count(enabled) == 1 && !IsLutFileKey(enabled),
              "a profile keeps " + enabled);
        const Section kept = Filter({ { file, "a.cube" }, { strength, "0.5" }, { "Bogus", "1" } });
        Check(kept.size() == 2 && kept.count(file) && kept.count(strength), "filter keeps " + file + " and its strength");
    }
    Check(!IsLeftOut("PostLutStackEnabled") && Filter({ { "PostLutStackEnabled", "false" } }).size() == 1,
          "a profile keeps the stack's on/off");
    Check(!IsLutFileKey("PostLutStrength1") && !IsLutFileKey("LutStrength") && !IsLutFileKey("PostLutFile5"),
          "only file keys are LUT file keys");
    Check(Same({ { "PostLutFile2", "warm.cube" } }, { { "PostLutFile2", "C:\\Games\\X\\OptiScaler\\LUTs\\warm.cube" } }) &&
              !Same({ { "PostLutFile2", "warm.cube" } }, { { "PostLutFile2", "cold.cube" } }),
          "a post LUT layer counts by file name");
}

// Every key SaveIni writes into [DlssNr] is kept or left out. The Pass{}... keys written in a loop are covered by the
// pass rule.
static void Coverage(const char* configCpp)
{
    std::ifstream in(configCpp);
    Check(in.good(), std::string("open ") + configCpp);
    if (!in.good())
        return;

    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string text = buffer.str();

    const std::regex written(R"re(SetValue\("DlssNr",\s*"([A-Za-z0-9]+)")re");
    std::set<std::string> keys;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), written); it != std::sregex_iterator(); ++it)
        keys.insert((*it)[1].str());

    Check(keys.size() > 100, "found the [DlssNr] keys SaveIni writes (" + std::to_string(keys.size()) + ")");

    for (const auto& key : keys)
        Check(IsKept(key) != IsLeftOut(key), "SaveIni key sorted into exactly one list: " + key);

    for (auto key : kKeptKeys)
        Check(keys.count(std::string(key)) == 1, "kept key is one SaveIni writes: " + std::string(key));

    // Config.cpp clears each kept field before a profile is read (set_from_config only fills an empty one): the clear
    // table must name every kept key, and only those.
    const std::regex cleared(R"re(NR_PROFILE_FIELD\("([A-Za-z0-9]+)")re");
    std::set<std::string> clearedKeys;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), cleared); it != std::sregex_iterator(); ++it)
        clearedKeys.insert((*it)[1].str());

    for (auto key : kKeptKeys)
        Check(clearedKeys.count(std::string(key)) == 1, "kept key cleared before a load: " + std::string(key));
    for (const auto& key : clearedKeys)
        Check(IsKept(key), "cleared key is a kept key: " + key);

    // Each clear-table key is paired with the field ReadDlssNr fills from that key, and ReadDlssNr reads every kept
    // key: a mis-paired row would clear one setting and leave another to keep its old value.
    const size_t readBegin = text.find("void Config::ReadDlssNr()");
    const size_t readEnd = text.find("static void WriteDlssNr", readBegin);
    Check(readBegin != std::string::npos && readEnd != std::string::npos, "found ReadDlssNr");
    if (readBegin == std::string::npos || readEnd == std::string::npos)
        return;
    const std::string readBody = text.substr(readBegin, readEnd - readBegin);

    // Read through a local first, then set (the input style card) or through readEnum
    const std::set<std::string> viaLocal = { "InputFlatten", "InputFlattenStrength", "InputStructureHint",
                                             "InputStructureStrength", "ScalingDownscaler" };

    const std::regex pair(R"re(NR_PROFILE_FIELD\("([A-Za-z0-9]+)",\s*([A-Za-z0-9_]+)\))re");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), pair); it != std::sregex_iterator(); ++it)
    {
        const std::string key = (*it)[1].str();
        const std::string field = (*it)[2].str();
        const std::string readsKey = R"(read\w*(<\w+>)?\("DlssNr",\s*")" + key + "\"";

        Check(std::regex_search(readBody, std::regex(readsKey)), "ReadDlssNr reads kept key " + key);

        const bool paired =
            viaLocal.count(key)
                ? readBody.find(field + ".set_from_config(") != std::string::npos
                : std::regex_search(readBody, std::regex(field + R"(\.set_from_config\(\s*)" + readsKey));
        Check(paired, "clear-table row " + key + " -> " + field + " is the field ReadDlssNr fills from it");
    }
}

int main(int argc, char** argv)
{
    Names();
    Keys();

    if (argc > 1)
        Coverage(argv[1]);
    else
        Check(false, "pass OptiScaler\\Config.cpp as the first argument");

    if (fails == 0)
        std::printf("nr_profiles_smoke: PASS\n");
    return fails == 0 ? 0 : 1;
}
