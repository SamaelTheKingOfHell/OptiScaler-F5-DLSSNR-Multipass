// Host check of native/FpsLog.h: one frame-rate line after each change of what the player set (once it settled), then
// one now and then, with the game's and the shown frame rate; what runs at the moment never restarts it.
// cl /std:c++20 /EHsc /W4 tests/f5low_fps_log_smoke.cpp
#include "../OptiScaler/native/FpsLog.h"

#include <cstdio>
#include <functional>
#include <limits>
#include <vector>

using namespace native::fpslog;

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

static bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

struct Run
{
    Tracker t;
    double ms = 0.0;
    uint64_t shown = 0;
    uint64_t mask = ~0ull;
    std::vector<std::string> lines;

    // `seconds` of presents at `fps`. `live(i)` gives what runs on present i; `perPresent` frames are added to XeFG's
    // total each present when it has one.
    void Play(const Settings& s, double fps, double seconds, uint32_t perPresent, bool xefg,
              const std::function<Live(int)>& live = {})
    {
        const int count = (int) (fps * seconds);

        for (int i = 0; i < count; ++i)
        {
            ms += 1000.0 / fps;
            Live l = live ? live(i) : Live { s.fgEnabled, s.fgEnabled ? s.fgCount : 1u };
            shown = (shown + perPresent) & mask;

            if (xefg)
                l.shownTotal = shown;

            l.shownMask = mask;

            if (auto line = t.OnPresent(ms, s, l))
                lines.push_back(*line);
        }
    }

    int Measured() const
    {
        int n = 0;
        for (const auto& l : lines)
            n += Has(l, "FPS: game") ? 1 : 0;
        return n;
    }
};

int main()
{
    Settings nrOn { .nr = true, .modelScale = 1.5f, .upscalerPass = true, .mode = "NR + frame generation",
                    .fgOutput = "F5Low", .fgEnabled = true, .fgCount = 3, .capFps = 75.0f };
    Settings nrOff = nrOn;
    nrOff.nr = false;
    Settings off { .mode = "Off", .fgOutput = "F5Low", .fgEnabled = false, .capFps = 75.0f };

    // First settings: no "changed" line; one measurement after settle + 3 s: game 25, shown 75
    {
        Run r;
        r.Play(nrOn, 25.0, 5.0, 3, true);
        CHECK(r.lines.size() == 1);
        if (!r.lines.empty())
        {
            CHECK(Has(r.lines[0], "game 25.0, shown 75.0"));
            CHECK(Has(r.lines[0], "NR on, model 1.50, upscaler pass on"));
            CHECK(Has(r.lines[0], "frame generation F5Low x3, cap 75 fps"));
            CHECK(Has(r.lines[0], "frame generation running x3"));
        }

        // Nothing changes: the next line only after 30 s
        r.Play(nrOn, 25.0, 20.0, 3, true);
        CHECK(r.lines.size() == 1);
        r.Play(nrOn, 25.0, 12.0, 3, true);
        CHECK(r.lines.size() == 2);

        // A change: one "changed" line at once, then the new setting's own measurement
        r.Play(nrOff, 25.0, 0.5, 3, true);
        CHECK(r.lines.size() == 3 && Has(r.lines[2], "settings changed to (NR off"));
        r.Play(nrOff, 25.0, 5.0, 3, true);
        CHECK(r.lines.size() == 4 && Has(r.lines[3], "NR off") && Has(r.lines[3], "game 25.0"));

        // Mode Off, no frame generation and no XeFG total: the shown rate is the game's
        r.Play(off, 75.0, 5.0, 0, false);
        CHECK(r.lines.size() == 6);
        if (r.lines.size() == 6)
        {
            CHECK(Has(r.lines[5], "game 75.0, shown 75.0"));
            CHECK(Has(r.lines[5], "Optical F5Low Off, frame generation F5Low off, cap 75 fps"));
            CHECK(Has(r.lines[5], "frame generation not running"));
        }
    }

    // A change right after another: only the last settings are measured
    {
        Run r;
        r.Play(nrOn, 60.0, 0.2, 3, true);
        r.Play(nrOff, 60.0, 0.2, 3, true);
        r.Play(off, 60.0, 5.0, 0, false);
        CHECK(r.Measured() == 1);
        CHECK(Has(r.lines.back(), "Optical F5Low Off"));
    }

    // A step of 5 s, as a player clicks through the menu: each setting gets its own line
    {
        Run r;
        r.Play(nrOn, 60.0, 5.0, 3, true);
        r.Play(nrOff, 60.0, 5.0, 3, true);
        r.Play(off, 60.0, 5.0, 0, false);
        CHECK(r.Measured() == 3);
    }

    // Frame generation pausing and Dynamic MFG changing its count by itself restart nothing: one measurement, and the
    // line says it was on and off
    {
        Run r;
        Settings dyn = nrOn;
        dyn.dynamicMfg = true;
        dyn.fgCount = 4;
        r.Play(dyn, 40.0, 5.0, 2, true,
               [](int i) { return Live { i % 50 >= 5, (uint32_t) (2 + i % 3) }; });
        CHECK(r.lines.size() == 1);
        if (!r.lines.empty())
        {
            CHECK(Has(r.lines[0], "frame generation F5Low up to x4 (Dynamic MFG)"));
            CHECK(Has(r.lines[0], "frame generation on and off"));
        }
    }

    // XeFG sends nothing for the whole window: shown is 0, not unknown and not the game's rate
    {
        Run r;
        r.Play(nrOn, 30.0, 5.0, 0, true);
        CHECK(r.lines.size() == 1 && Has(r.lines[0], "shown 0.0"));
    }

    // XeFG's total wraps: the difference is still right
    {
        Run r;
        r.mask = 0xFF;
        r.shown = 0xF0;
        r.Play(nrOn, 25.0, 5.0, 3, true);
        CHECK(r.lines.size() == 1 && Has(r.lines[0], "shown 75.0"));
    }

    // Frame generation on but no XeFG total (FSR FG, DLSS-G): shown is unknown
    {
        Run r;
        Settings fsr = nrOn;
        fsr.fgOutput = "FSR FG";
        r.Play(fsr, 30.0, 5.0, 0, false);
        CHECK(r.lines.size() == 1 && Has(r.lines[0], "shown ?"));
    }

    // A NaN from the ini is not a change on every present
    {
        Run r;
        Settings bad = nrOn;
        bad.capFps = std::numeric_limits<float>::quiet_NaN();
        bad.modelScale = std::numeric_limits<float>::quiet_NaN();
        r.Play(bad, 60.0, 5.0, 3, true);
        CHECK(r.lines.size() == 1 && Has(r.lines[0], "cap none"));
    }

    // Auto model resolution: the setting says auto, the line the resolution applied
    {
        Run r;
        Settings autoModel = nrOn;
        autoModel.modelAuto = true;
        r.Play(autoModel, 30.0, 5.0, 3, true, [](int) { return Live { true, 3, {}, ~0ull, 77 }; });
        CHECK(r.lines.size() == 1 && Has(r.lines[0], "model auto") && Has(r.lines[0], "model applied 77%"));
    }

    printf(fails == 0 ? "f5low fps log: ok\n" : "f5low fps log: %d FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
