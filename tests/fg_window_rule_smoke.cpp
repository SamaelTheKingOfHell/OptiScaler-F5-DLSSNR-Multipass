// Host check of FgWindowRule.h: a swapchain on another window while frame generation's swapchain lives.
// No GPU and no game needed.
// cl /std:c++20 /EHsc tests/fg_window_rule_smoke.cpp
#include "../OptiScaler/framegen/FgWindowRule.h"

#include <cstdio>

using FgWindow::IsOtherWindow;
using FgWindow::Window;

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
    int a = 0, b = 0;
    const void* gameHwnd = &a;
    const void* otherHwnd = &b;

    constexpr int64_t fullHd = 1920LL * 1080;
    constexpr int64_t splash = 800LL * 450;

    const Window game { gameHwnd, true, fullHd };

    // No frame generation swapchain: nothing changes
    CHECK(!IsOtherWindow(false, game, { otherHwnd, true, splash }));
    CHECK(!IsOtherWindow(false, Window {}, { otherHwnd, true, splash }));

    // Same window (the game re-creates its swapchain): not another window, the existing paths handle it
    CHECK(!IsOtherWindow(true, game, { gameHwnd, true, fullHd }));

    // A smaller helper / launcher / overlay window while the game's is shown: plain swapchain, state left alone
    CHECK(IsOtherWindow(true, game, { otherHwnd, true, splash }));
    CHECK(IsOtherWindow(true, game, { otherHwnd, false, splash })); // not shown yet
    CHECK(IsOtherWindow(true, game, { otherHwnd, false, 0 }));      // zero-size helper
    CHECK(IsOtherWindow(true, game, { otherHwnd, true, fullHd }));  // same size, e.g. a second monitor copy

    // The game moved on: its old window is gone or hidden, the new one takes frame generation as before
    CHECK(!IsOtherWindow(true, { gameHwnd, false, fullHd }, { otherHwnd, true, splash }));
    CHECK(!IsOtherWindow(true, { gameHwnd, false, 0 }, { otherHwnd, true, fullHd })); // destroyed (no area either)

    // Splash first, game window after while the splash is still up: the larger game window takes frame generation
    const Window splashFg { otherHwnd, true, splash };
    CHECK(!IsOtherWindow(true, splashFg, { gameHwnd, true, fullHd }));
    CHECK(!IsOtherWindow(true, splashFg, { gameHwnd, false, fullHd })); // created before ShowWindow, sized already

    // Unknown handles never refuse (CoreWindow / composition have no HWND, frame generation's window not known)
    CHECK(!IsOtherWindow(true, game, { nullptr, true, splash }));
    CHECK(!IsOtherWindow(true, { nullptr, true, fullHd }, { otherHwnd, true, splash }));

    // A minimized window counts its restored size, not its empty client area
    CHECK(FgWindow::WindowArea(false, 1920, 1080, 0, 0) == fullHd);
    CHECK(FgWindow::WindowArea(true, 0, 0, 1920, 1080) == fullHd);
    CHECK(FgWindow::WindowArea(true, 1920, 1080, 0, 0) == 0); // no restored size known
    CHECK(FgWindow::WindowArea(false, -5, 1080, 0, 0) == 0);  // never negative

    // Game minimized by Alt+Tab (still shown): a crash reporter or overlay popup does not take frame generation
    const Window minimizedGame { gameHwnd, true, FgWindow::WindowArea(true, 0, 0, 1920, 1080) };
    CHECK(IsOtherWindow(true, minimizedGame, { otherHwnd, true, splash }));
    // ... with its client area alone it would have (the bug this guards)
    CHECK(!IsOtherWindow(true, { gameHwnd, true, FgWindow::WindowArea(false, 0, 0, 1920, 1080) },
                         { otherHwnd, true, splash }));

    // Compile-time too
    static_assert(IsOtherWindow(true, Window { &a, true, 100 }, Window { &b, true, 50 }));
    static_assert(!IsOtherWindow(true, Window { &a, false, 100 }, Window { &b, true, 50 }));

    if (fails == 0)
        printf("fg_window_rule_smoke: PASS\n");

    return fails == 0 ? 0 : 1;
}
