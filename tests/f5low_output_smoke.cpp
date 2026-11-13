// Host check of FG Output F5Low: FGOutput.h's UsesXeFGApi and F5LowRules.h. No GPU needed.
// cl /std:c++20 /EHsc /W4 tests/f5low_output_smoke.cpp
#include "../OptiScaler/framegen/FGOutput.h"
#include "../OptiScaler/framegen/f5low/F5LowRules.h"

#include <cstdio>
#include <cstring>
#include <cwchar>

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

// The menu's option list is indexed by the enum: F5Low comes last
static_assert(static_cast<unsigned>(FGOutput::F5Low) == 4);

int main()
{
    // F5Low shares every XeFG path; the others do not
    CHECK(UsesXeFGApi(FGOutput::XeFG));
    CHECK(UsesXeFGApi(FGOutput::F5Low));
    CHECK(!UsesXeFGApi(FGOutput::NoFG));
    CHECK(!UsesXeFGApi(FGOutput::FSRFG));
    CHECK(!UsesXeFGApi(FGOutput::DLSSG));
    CHECK(FgAnswersFullscreenForGame(FGOutput::F5Low));
    CHECK(FgAnswersFullscreenForGame(FGOutput::FSRFG));
    CHECK(!FgAnswersFullscreenForGame(FGOutput::DLSSG));

    // Frame Generator parse / save
    bool valid = false;
    CHECK(!F5Low::ParseGenerator("auto", valid).has_value() && valid);
    CHECK(!F5Low::ParseGenerator("", valid).has_value() && valid);
    CHECK(F5Low::ParseGenerator("xefg_tensor", valid) == F5Low::Generator::XefgTensor && valid);
    CHECK(F5Low::ParseGenerator("XeFG_Tensor", valid) == F5Low::Generator::XefgTensor && valid);
    CHECK(F5Low::ParseGenerator("dlssg", valid) == F5Low::Generator::Dlssg && valid);
    CHECK(F5Low::ParseGenerator("DLSSG", valid) == F5Low::Generator::Dlssg && valid);
    CHECK(F5Low::ParseGenerator("intel", valid) == F5Low::Generator::Intel && valid);
    CHECK(F5Low::ParseGenerator("Intel", valid) == F5Low::Generator::Intel && valid);
    CHECK(!F5Low::ParseGenerator("libxess", valid).has_value() && !valid); // not a name
    CHECK(!F5Low::ParseGenerator("junk", valid).has_value() && !valid);
    CHECK(strcmp(F5Low::GeneratorText(std::nullopt), "auto") == 0);
    CHECK(strcmp(F5Low::GeneratorText(F5Low::Generator::XefgTensor), "xefg_tensor") == 0);
    {
        // Round trip
        bool ok = false;
        CHECK(F5Low::ParseGenerator(F5Low::GeneratorText(F5Low::Generator::XefgTensor), ok) ==
                  F5Low::Generator::XefgTensor &&
              ok);
    }
    CHECK(strcmp(F5Low::GeneratorText(F5Low::Generator::Dlssg), "dlssg") == 0);
    {
        bool ok = false;
        CHECK(F5Low::ParseGenerator(F5Low::GeneratorText(F5Low::Generator::Dlssg), ok) == F5Low::Generator::Dlssg &&
              ok);
    }
    CHECK(strcmp(F5Low::GeneratorLabel(F5Low::Generator::Dlssg), "DLSS-G (NVIDIA)") == 0);
    CHECK(wcscmp(F5Low::GeneratorEnvValue(F5Low::Generator::Dlssg), L"dlssg") == 0);
    CHECK(strcmp(F5Low::GeneratorText(F5Low::Generator::Intel), "intel") == 0);
    {
        bool ok = false;
        CHECK(F5Low::ParseGenerator(F5Low::GeneratorText(F5Low::Generator::Intel), ok) == F5Low::Generator::Intel &&
              ok);
    }
    CHECK(strcmp(F5Low::GeneratorLabel(F5Low::Generator::Intel), "XeFG (Intel library)") == 0);
    CHECK(wcscmp(F5Low::GeneratorEnvValue(F5Low::Generator::Intel), L"intel") == 0);
    CHECK(strcmp(F5Low::IntelFilesMissing(true, true), "") == 0);
    CHECK(strstr(F5Low::IntelFilesMissing(false, true), "libxess_fg.dll") != nullptr);
    CHECK(strstr(F5Low::IntelFilesMissing(true, false), "libxell.dll") != nullptr);
    CHECK(strstr(F5Low::IntelFilesMissing(false, false), "libxell.dll") != nullptr);
    CHECK(strcmp(F5Low::GeneratorLabel(F5Low::Generator::XefgTensor), "XeFG Tensor") == 0);
    CHECK(wcscmp(F5Low::GeneratorEnvValue(F5Low::Generator::XefgTensor), L"own") == 0);

    // Max interpolations: the library's report, capped by the ini, never above the library
    CHECK(F5Low::MaxInterpolations(5, 5) == 5);
    CHECK(F5Low::MaxInterpolations(5, 2) == 2);
    CHECK(F5Low::MaxInterpolations(3, 5) == 3); // library handed over to Intel: no more than it reports
    CHECK(F5Low::MaxInterpolations(0, 5) == 1); // nothing reported reads as 1
    CHECK(F5Low::MaxInterpolations(5, 9) == 5); // ini out of range is clamped
    CHECK(F5Low::MaxInterpolations(5, 0) == 1);

    // DLSS-G needs Streamline and nvngx_dlssg.dll in OptiScaler's streamline folder
    CHECK(strcmp(F5Low::DlssgFilesMissing(true, true), "") == 0);
    CHECK(strstr(F5Low::DlssgFilesMissing(false, true), "Streamline") != nullptr);
    CHECK(strstr(F5Low::DlssgFilesMissing(true, false), "nvngx_dlssg.dll") != nullptr);

    // Availability
    CHECK(F5Low::CheckAvailable(true, true).available);
    CHECK(!F5Low::CheckAvailable(false, true).available);
    CHECK(!F5Low::CheckAvailable(true, false).available);
    CHECK(!F5Low::CheckAvailable(false, false).available);
    CHECK(strstr(F5Low::CheckAvailable(false, true).reason, "NVIDIA") != nullptr);
    CHECK(strstr(F5Low::CheckAvailable(true, false).reason, "plugins") != nullptr);
    CHECK(!F5Low::CheckAvailable(true, true, false).available);
    CHECK(strstr(F5Low::CheckAvailable(true, true, false).reason, "libxess_fg.dll") != nullptr);
    CHECK(F5Low::CheckAvailable(true, true, true).available);
    // DLSS-G does not use libxess_fg.dll
    CHECK(F5Low::CheckAvailable(true, true, false, false).available);
    CHECK(!F5Low::CheckAvailable(true, false, false, false).available);
    CHECK(!F5Low::CheckAvailable(false, true, false, false).available);
    CHECK(F5Low::CheckAvailable(true, true, false, false).reason[0] == 0);

    // f5low.dll's status export
    CHECK(F5Low::RunStateFromCode(0) == F5Low::RunState::Undecided);
    CHECK(F5Low::RunStateFromCode(1) == F5Low::RunState::Own);
    CHECK(F5Low::RunStateFromCode(2) == F5Low::RunState::HandedOver);
    CHECK(F5Low::RunStateFromCode(3) == F5Low::RunState::Unavailable);
    CHECK(F5Low::RunStateFromCode(4) == F5Low::RunState::Unknown);
    CHECK(F5Low::RunStateFromCode(-1) == F5Low::RunState::Unknown);
    CHECK(strstr(F5Low::RunStateLabel(F5Low::RunState::Unknown), "older") != nullptr);
    CHECK(strstr(F5Low::RunStateLabel(F5Low::RunState::NotLoaded), "not loaded") != nullptr);
    CHECK(strstr(F5Low::RunStateLabel(F5Low::RunState::HandedOver), "Intel") != nullptr);
    CHECK(strstr(F5Low::RunStateLabel(F5Low::RunState::Own), "own frame generator") != nullptr);
    CHECK(strstr(F5Low::RunStateLabel(F5Low::RunState::Own, F5Low::Generator::Dlssg), "DLSS-G") != nullptr);
    CHECK(strstr(F5Low::RunStateLabel(F5Low::RunState::Unavailable, F5Low::Generator::Dlssg), "DLSS-G") != nullptr);
    CHECK(strstr(F5Low::RunStateLabel(F5Low::RunState::Undecided, F5Low::Generator::Dlssg), "starting") != nullptr);
    CHECK(strstr(F5Low::RunStateLabel(F5Low::RunState::Own, F5Low::Generator::Intel), "XeFG (Intel library) running") !=
          nullptr);
    CHECK(strstr(F5Low::RunStateLabel(F5Low::RunState::HandedOver, F5Low::Generator::Intel),
                 "XeFG (Intel library) running") != nullptr);
    CHECK(strstr(F5Low::RunStateLabel(F5Low::RunState::Unavailable, F5Low::Generator::Intel), "unavailable") !=
          nullptr);
    CHECK(strstr(F5Low::RunStateLabel(F5Low::RunState::Undecided, F5Low::Generator::Intel), "starting") != nullptr);

    if (fails == 0)
        printf("f5low_output_smoke: PASS\n");
    return fails == 0 ? 0 : 1;
}
