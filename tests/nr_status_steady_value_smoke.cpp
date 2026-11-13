// Host check of dlssnr/DlssNr_SteadyValue.h: the menu's elapsed time changes a few times a second, as an average, not
// every frame. cl /std:c++20 /EHsc /W4 tests/nr_status_steady_value_smoke.cpp
#include "../OptiScaler/dlssnr/DlssNr_SteadyValue.h"

#include <cmath>
#include <cstdio>

using DlssNr::SteadyValue;

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

static bool Near(std::optional<double> v, double want) { return v.has_value() && std::fabs(*v - want) < 1e-9; }

int main()
{
    // The first sample shows at once
    {
        SteadyValue s;
        CHECK(Near(s.Update(8.85, 0.0), 8.85));
    }

    // Samples every frame (60 fps) that alternate: the shown value holds between refreshes, then is their average
    {
        SteadyValue s;
        s.Update(10.0, 0.0);
        int changes = 0;
        std::optional<double> last = 10.0;

        for (int i = 1; i <= 60; ++i) // one second
        {
            const auto shown = s.Update(i % 2 ? 8.0 : 12.0, i * 1000.0 / 60.0);
            changes += shown != last ? 1 : 0;
            last = shown;
        }

        CHECK(changes <= 4);           // a few times a second, not 60
        // the average of the window's 8s and 12s (15 frames a window: one more of one of them)
        CHECK(last.has_value() && std::fabs(*last - 10.0) < 0.3);
    }

    // Nothing measured: nothing shown, and the next sample shows at once again
    {
        SteadyValue s;
        s.Update(5.0, 0.0);
        CHECK(!s.Update(std::nullopt, 16.0).has_value());
        CHECK(Near(s.Update(7.0, 32.0), 7.0));
    }

    printf(fails == 0 ? "nr status steady value: ok\n" : "nr status steady value: %d FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
