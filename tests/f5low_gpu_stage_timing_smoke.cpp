// Host check of native/GpuStageTiming.h: per-stage GPU time from a frame's timestamps, averaged and maxed over a window.
// cl /std:c++20 /EHsc /W4 tests/f5low_gpu_stage_timing_smoke.cpp
#include "../OptiScaler/native/GpuStageTiming.h"

#include <cmath>
#include <cstdio>

using namespace native::gputiming;

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

int main()
{
    constexpr uint64_t kHz = 1'000'000; // one tick a microsecond

    // Nothing counted: no report
    {
        Stats s;
        CHECK(s.Frames() == 0);
        CHECK(s.Report().empty());
    }

    // Two frames: motion 0.5 and 0.7 ms, trust 0.2 and 0.2, consumer 0 and 1.0
    {
        Stats s;
        s.Add({ 1000, 1500, 1700, 1700 }, kHz, true);
        s.Add({ 5000, 5700, 5900, 6900 }, kHz, true);
        CHECK(s.Frames() == 2);

        const std::string r = s.Report();
        CHECK(r.find("over 2 frames") != std::string::npos);
        CHECK(r.find("motion 0.600 / 0.700 ms") != std::string::npos);
        CHECK(r.find("trust mask + guides 0.200 / 0.200 ms") != std::string::npos);
        CHECK(r.find("consumer 0.500 / 1.000 ms (in the 2 frames it ran)") != std::string::npos);
        CHECK(r.find("total 1.300 / 1.900 ms") != std::string::npos);

        // The report starts a new window
        CHECK(s.Frames() == 0);
        CHECK(s.Report().empty());
    }

    // The consumer is averaged over the frames it ran in only: a frame without it does not pull it down
    {
        Stats s;
        s.Add({ 0, 500, 600, 2600 }, kHz, true);  // consumer 2.0 ms
        s.Add({ 0, 500, 600, 600 }, kHz, false);  // no consumer
        const std::string r = s.Report();
        CHECK(r.find("over 2 frames") != std::string::npos);
        CHECK(r.find("consumer 2.000 / 2.000 ms (in the 1 frames it ran)") != std::string::npos);
        CHECK(r.find("motion 0.500 / 0.500 ms") != std::string::npos);

        Stats none;
        none.Add({ 0, 500, 600, 600 }, kHz, false);
        CHECK(none.Report().find("consumer did not run") != std::string::npos);
    }

    // Stamps going backwards, or no tick rate: not counted
    {
        Stats s;
        s.Add({ 1000, 900, 1100, 1200 }, kHz, true);
        s.Add({ 1000, 1100, 1200, 1300 }, 0, true);
        CHECK(s.Frames() == 0);
    }

    printf(fails == 0 ? "f5low gpu stage timing: ok\n" : "f5low gpu stage timing: %d FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
