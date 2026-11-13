// Host check of DlssNrNativeMode::MotionUsed (dlssnr/DlssNr_NativeMode.h): Optical F5Low's motion runs only for something
// that reads it. cl /std:c++20 /EHsc /W4 tests/f5low_motion_users_smoke.cpp
#include "../OptiScaler/dlssnr/DlssNr_NativeMode.h"

#include <cstdio>

using namespace DlssNrNativeMode;

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
    // Nothing on: no motion
    CHECK(!MotionUsed({}));

    // "NR + frame generation": frame generation takes the motion whether NR is on or off
    CHECK(MotionUsed({ .virtualUpscaler = true }));
    CHECK(MotionUsed({ .virtualUpscaler = true, .nrEnabled = true }));

    // "NR only": only while NR can run on it (Enable Neural Rendering and Finished Picture both on)
    CHECK(MotionUsed({ .nativeInput = true, .nrEnabled = true, .finishedPicture = true }));
    CHECK(!MotionUsed({ .nativeInput = true, .nrEnabled = false, .finishedPicture = true }));
    CHECK(!MotionUsed({ .nativeInput = true, .nrEnabled = true, .finishedPicture = false }));

    // NR on without native input (NR on the game's own upscaler call, or the motion-only keys): no motion
    CHECK(!MotionUsed({ .nrEnabled = true, .finishedPicture = true }));

    // The menu's pictures need it, NR or not
    CHECK(MotionUsed({ .debugView = true }));
    CHECK(MotionUsed({ .nativeInput = true, .debugView = true }));

    printf(fails == 0 ? "f5low motion users: ok\n" : "f5low motion users: %d FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
