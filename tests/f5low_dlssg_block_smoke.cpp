// Host check of the rule that keeps a game's own nvngx_dlssg.dll out while FG Output F5Low runs the DLSS-G generator
// (F5LowRules.h GameDlssgLoadRule). No GPU needed.
// cl /std:c++20 /EHsc /W4 tests/f5low_dlssg_block_smoke.cpp
#include "../OptiScaler/framegen/f5low/F5LowRules.h"

#include <cstdio>

static int fails = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                                                \
            ++fails;                                                                                                   \
        }                                                                                                              \
    } while (0)

using F5Low::Generator;
using F5Low::GameDlssgLoad;

static const wchar_t* kPrivate = L"C:\\Games\\Game\\OptiScaler\\plugins\\f5low_streamline";
static const wchar_t* kGamePath = L"C:\\Games\\Game\\bin\\nvngx_dlssg.dll";
static const wchar_t* kPrivateDll = L"C:\\Games\\Game\\OptiScaler\\plugins\\f5low_streamline\\nvngx_dlssg.dll";
static const wchar_t* kOtaPath =
    L"C:\\ProgramData\\NVIDIA\\NGX\\models\\dlssg\\versions\\310.9.0\\files\\160_abc.bin";

// f5lowOutput, generator, keep, fgInputDlssg
static GameDlssgLoad Rule(bool f5low, Generator generator, bool keep, bool inputDlssg, const wchar_t* module,
                          const wchar_t* folder = kPrivate, bool privateLoaded = false, bool fileExists = false)
{
    return F5Low::GameDlssgLoadRule(f5low, generator, keep, inputDlssg, module, folder, privateLoaded, fileExists);
}

int main()
{
    // The generator is our own: nothing is kept out
    CHECK(Rule(true, Generator::XefgTensor, false, false, kGamePath) == GameDlssgLoad::Allow);

    // DLSS-G generator: the game's copy is blocked, f5low's private copy is not
    CHECK(Rule(true, Generator::Dlssg, false, false, kGamePath) == GameDlssgLoad::Block);
    CHECK(Rule(true, Generator::Dlssg, false, false, L"nvngx_dlssg.dll") == GameDlssgLoad::Block);
    CHECK(Rule(true, Generator::Dlssg, false, false, L"nvngx_dlssg") == GameDlssgLoad::Block);
    CHECK(Rule(true, Generator::Dlssg, false, false,
               kPrivateDll) == GameDlssgLoad::Allow);

    // A folder that only starts with the same letters is not the private one
    CHECK(Rule(true, Generator::Dlssg, false, false,
               L"C:\\Games\\Game\\OptiScaler\\plugins\\f5low_streamline_old\\nvngx_dlssg.dll") == GameDlssgLoad::Block);
    // The folder itself is not a module named nvngx_dlssg: nothing to keep out
    CHECK(Rule(true, Generator::Dlssg, false, false, kPrivate) == GameDlssgLoad::Allow);
    // OptiScaler's own streamline folder holds the source copy: not the private one
    CHECK(Rule(true, Generator::Dlssg, false, false, L"C:\\Games\\Game\\OptiScaler\\streamline\\nvngx_dlssg.dll") ==
          GameDlssgLoad::Block);

    // Other modules are never touched
    CHECK(Rule(true, Generator::Dlssg, false, false, L"C:\\Games\\Game\\sl.dlss_g.dll") == GameDlssgLoad::Allow);
    CHECK(Rule(true, Generator::Dlssg, false, false, L"nvngx_dlss.dll") == GameDlssgLoad::Allow);
    CHECK(Rule(true, Generator::Dlssg, false, false, L"C:\\x\\not_nvngx_dlssg.dll") == GameDlssgLoad::Allow);
    CHECK(Rule(true, Generator::Dlssg, false, false, L"") == GameDlssgLoad::Allow);

    // Another FG Output: never
    CHECK(Rule(false, Generator::Dlssg, false, false, kGamePath) == GameDlssgLoad::Allow);

    // KeepGameDlssg turns the block off
    CHECK(Rule(true, Generator::Dlssg, true, false, kGamePath) == GameDlssgLoad::Allow);

    // FG Input DLSSG needs the game's DLSS-G: allowed, with a warning (also the bare name)
    CHECK(Rule(true, Generator::Dlssg, false, true, kGamePath) == GameDlssgLoad::AllowWarn);

    CHECK(Rule(true, Generator::Dlssg, false, true, L"nvngx_dlssg.dll") == GameDlssgLoad::AllowWarn);
    CHECK(Rule(true, Generator::Dlssg, false, true,
               kPrivateDll) == GameDlssgLoad::Allow);
    CHECK(Rule(true, Generator::Dlssg, true, true, kGamePath) == GameDlssgLoad::Allow);
    CHECK(Rule(true, Generator::XefgTensor, false, true, kGamePath) == GameDlssgLoad::Allow);

    // The Intel generator never blocks or redirects the game's nvngx_dlssg
    CHECK(Rule(true, Generator::Intel, false, false, kGamePath) == GameDlssgLoad::Allow);
    CHECK(Rule(true, Generator::Intel, false, false, L"nvngx_dlssg.dll") == GameDlssgLoad::Allow);
    CHECK(Rule(true, Generator::Intel, false, true, kGamePath) == GameDlssgLoad::Allow);

    // The game's own DLSS-G is forced off only with F5Low + DLSS-G generator, FG Input not DLSSG, KeepGameDlssg off,
    // and only when F5Low can run (f5low.dll present, DLSS-G can run)
    using F5Low::ForceGameDlssgOff;
    CHECK(ForceGameDlssgOff(true, Generator::Dlssg, false, false, true, true));
    CHECK(!ForceGameDlssgOff(true, Generator::Dlssg, true, false, true, true));  // FG Input DLSSG needs its options
    CHECK(!ForceGameDlssgOff(true, Generator::Dlssg, false, true, true, true));  // KeepGameDlssg
    CHECK(!ForceGameDlssgOff(false, Generator::Dlssg, false, false, true, true)); // FG Output not F5Low
    CHECK(!ForceGameDlssgOff(true, Generator::Intel, false, false, true, true));
    CHECK(!ForceGameDlssgOff(true, Generator::XefgTensor, false, false, true, true));
    CHECK(!ForceGameDlssgOff(true, Generator::Dlssg, false, false, false, true)); // f5low.dll missing
    CHECK(!ForceGameDlssgOff(true, Generator::Dlssg, false, false, true, false)); // F5Low cannot run

    // Case and separators do not matter, for the name and the folder
    CHECK(Rule(true, Generator::Dlssg, false, false, L"C:\\GAMES\\Game\\bin\\NVNGX_DLSSG.DLL") == GameDlssgLoad::Block);
    CHECK(Rule(true, Generator::Dlssg, false, false, L"C:/Games/Game/bin/Nvngx_Dlssg.dll") == GameDlssgLoad::Block);
    CHECK(Rule(true, Generator::Dlssg, false, false,
               L"c:/games/game/optiscaler/PLUGINS/F5LOW_STREAMLINE/NVNGX_DLSSG.dll") == GameDlssgLoad::Allow);
    CHECK(Rule(true, Generator::Dlssg, false, false,
               kPrivateDll, L"c:/games/game/optiscaler/plugins/f5low_streamline/") ==
          GameDlssgLoad::Allow);

    // A bare name is let through only when the private copy is already loaded (the loader hands that one out)
    CHECK(Rule(true, Generator::Dlssg, false, false, L"nvngx_dlssg.dll", kPrivate, true) == GameDlssgLoad::Allow);
    CHECK(Rule(true, Generator::Dlssg, false, false, L"NVNGX_DLSSG", kPrivate, true) == GameDlssgLoad::Allow);
    CHECK(Rule(true, Generator::Dlssg, false, false, L"nvngx_dlssg.dll", kPrivate, false) == GameDlssgLoad::Block);
    // ... a full path to the game's copy is blocked even then
    CHECK(Rule(true, Generator::Dlssg, false, false, kGamePath, kPrivate, true) == GameDlssgLoad::Redirect);
    CHECK(Rule(true, Generator::Dlssg, false, false, kGamePath, kPrivate, false) == GameDlssgLoad::Block);
    CHECK(Rule(true, Generator::Dlssg, false, false, L"c:/games/game/bin/NVNGX_DLSSG.DLL", kPrivate, true) ==
          GameDlssgLoad::Redirect);
    // ... FG Input DLSSG, KeepGameDlssg and another generator never redirect
    CHECK(Rule(true, Generator::Dlssg, false, true, kGamePath, kPrivate, true) == GameDlssgLoad::AllowWarn);
    CHECK(Rule(true, Generator::Dlssg, true, false, kGamePath, kPrivate, true) == GameDlssgLoad::Allow);
    CHECK(Rule(true, Generator::XefgTensor, false, false, kGamePath, kPrivate, true) == GameDlssgLoad::Allow);
    CHECK(Rule(false, Generator::Dlssg, false, false, kGamePath, kPrivate, true) == GameDlssgLoad::Allow);
    // ... a load of the private copy by path is plainly allowed
    CHECK(Rule(true, Generator::Dlssg, false, false, kPrivateDll, kPrivate, true) == GameDlssgLoad::Allow);
    // ... and with FG Input DLSSG a bare name warns when ours is not loaded
    CHECK(Rule(true, Generator::Dlssg, false, true, L"nvngx_dlssg.dll", kPrivate, false) == GameDlssgLoad::AllowWarn);

    // The driver's downloaded DLSS-G (any file under models\dlssg): blocked like the game's copy
    CHECK(Rule(true, Generator::Dlssg, false, false, kOtaPath) == GameDlssgLoad::Block);
    CHECK(Rule(true, Generator::Dlssg, false, false, L"c:/programdata/nvidia/ngx/models/DLSSG/x.dll") ==
          GameDlssgLoad::Block);
    CHECK(Rule(true, Generator::Dlssg, false, false, kOtaPath, kPrivate, true) == GameDlssgLoad::Redirect);
    CHECK(Rule(true, Generator::XefgTensor, false, false, kOtaPath) == GameDlssgLoad::Allow);
    CHECK(Rule(true, Generator::Dlssg, true, false, kOtaPath) == GameDlssgLoad::Allow);
    CHECK(Rule(true, Generator::Dlssg, false, true, kOtaPath) == GameDlssgLoad::AllowWarn);
    CHECK(Rule(false, Generator::Dlssg, false, false, kOtaPath) == GameDlssgLoad::Allow);
    // ... the other DLSS models are not DLSS-G
    CHECK(Rule(true, Generator::Dlssg, false, false,
               L"C:\\ProgramData\\NVIDIA\\NGX\\models\\dlss\\versions\\1\\files\\a.bin") == GameDlssgLoad::Allow);

    // Our file exists but is not loaded yet: the load is redirected (it gets loaded); Block only when neither
    CHECK(Rule(true, Generator::Dlssg, false, false, kGamePath, kPrivate, false, true) == GameDlssgLoad::Redirect);
    CHECK(Rule(true, Generator::Dlssg, false, false, kOtaPath, kPrivate, false, true) == GameDlssgLoad::Redirect);
    CHECK(Rule(true, Generator::Dlssg, false, false,
               L"C:\\Windows\\System32\\DriverStore\\FileRepository\\nvmdi.inf_x\\nvngx_dlssg.dll", kPrivate,
               false, true) == GameDlssgLoad::Redirect);
    CHECK(Rule(true, Generator::Dlssg, false, false, kGamePath, kPrivate, false, false) == GameDlssgLoad::Block);
    CHECK(Rule(true, Generator::Dlssg, false, true, kGamePath, kPrivate, false, true) == GameDlssgLoad::AllowWarn);
    CHECK(Rule(true, Generator::Dlssg, false, false, kPrivateDll, kPrivate, false, true) == GameDlssgLoad::Allow);
    CHECK(Rule(true, Generator::Dlssg, true, false, kGamePath, kPrivate, false, true) == GameDlssgLoad::Allow);

    // Path helper
    CHECK(F5Low::IsInsideFolder(kPrivateDll, kPrivate));
    CHECK(F5Low::IsInsideFolder(kPrivateDll, L"C:/Games/Game/OptiScaler/plugins/F5LOW_STREAMLINE/"));
    CHECK(!F5Low::IsInsideFolder(kGamePath, kPrivate));
    CHECK(!F5Low::IsInsideFolder(kPrivate, kPrivate));
    CHECK(!F5Low::IsInsideFolder(kPrivateDll, L""));

    // Unknown private folder: every copy counts as the game's
    CHECK(Rule(true, Generator::Dlssg, false, false,
               kPrivateDll, L"") == GameDlssgLoad::Block);

    if (fails == 0)
        printf("f5low_dlssg_block_smoke: PASS\n");
    return fails == 0 ? 0 : 1;
}
