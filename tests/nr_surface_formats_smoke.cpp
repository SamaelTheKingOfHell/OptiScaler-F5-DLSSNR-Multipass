// Host check of shaders/dlssnr/DlssNr_SurfaceFormats.h: when a format change makes NR's surfaces stale.
// cl /std:c++20 /EHsc /W4 tests/nr_surface_formats_smoke.cpp
#include "../OptiScaler/shaders/dlssnr/DlssNr_SurfaceFormats.h"

#include <cstdio>

int main()
{
    constexpr int kRgba8 = 28, kRgba16f = 10;
    int fails = 0;
    const auto check = [&](bool ok, const char* what) {
        if (!ok)
        {
            std::printf("FAIL: %s\n", what);
            ++fails;
        }
    };
    using DlssNr::SurfacesStale;

    check(!SurfacesStale(0, kRgba8, 0, kRgba8), "nothing made yet: nothing stale");
    check(!SurfacesStale(kRgba8, kRgba8, kRgba8, kRgba8), "same formats: kept");
    check(SurfacesStale(kRgba8, kRgba16f, kRgba8, kRgba8), "model format changed (compression switched on)");
    // The crash: compression on, so the model is RGBA16F before and after; only the game's colour moved.
    check(SurfacesStale(kRgba16f, kRgba16f, kRgba8, kRgba16f), "only the colour format changed, model format fixed");
    check(SurfacesStale(kRgba8, kRgba16f, kRgba8, kRgba16f), "both changed");
    check(!SurfacesStale(kRgba16f, kRgba16f, kRgba16f, kRgba16f), "compression on, nothing changed: kept");
    check(SurfacesStale(kRgba16f, kRgba8, kRgba8, kRgba8), "compression switched off");

    // Watch Dogs: Legion swaps its output RGBA8 <-> BGRA8 (27 <-> 90) between frames: one family, nothing stale.
    using DlssNr::CanonicalColourFormat;
    using DlssNr::SameColourFamily;
    constexpr int kRgba8Typeless = 27, kRgba8Srgb = 29, kBgra8 = 87, kBgra8Typeless = 90, kBgra8Srgb = 91;
    constexpr int kRgb10a2 = 24, kR11g11b10 = 26, kBgrx8 = 88;
    check(CanonicalColourFormat(kBgra8Typeless) == kRgba8Typeless, "BGRA8 typeless -> RGBA8 typeless");
    check(CanonicalColourFormat(kBgra8) == kRgba8, "BGRA8 UNORM -> RGBA8 UNORM");
    check(CanonicalColourFormat(kBgra8Srgb) == kRgba8Srgb, "BGRA8 sRGB -> RGBA8 sRGB");
    const int others[] = { kRgba8Typeless, kRgba8, kRgba8Srgb, kRgba16f, kRgb10a2, kR11g11b10, kBgrx8 };
    for (const int f : others)
        check(CanonicalColourFormat(f) == f, "everything else keeps its format");
    check(SameColourFamily(kRgba8Typeless, kBgra8Typeless), "27 and 90 are one family");
    check(SameColourFamily(kBgra8, kRgba8), "87 and 28 are one family");
    check(!SameColourFamily(kRgba8, kRgba8Srgb), "sRGB is its own family");
    check(!SameColourFamily(kBgra8Typeless, kRgba16f), "BGRA8 and RGBA16F are not");
    check(!SameColourFamily(kRgba8, kRgb10a2), "RGBA8 and RGB10A2 are not");
    const int was = CanonicalColourFormat(kRgba8Typeless), now = CanonicalColourFormat(kBgra8Typeless);
    check(!SurfacesStale(was, now, was, now), "the swap 27 -> 90: kept");
    check(SurfacesStale(was, kRgba16f, was, kRgba16f), "27 -> RGBA16F: rebuilt");

    // Neverness to Everness flips its pre-SR colour RGBA16F <-> R11G11B10F (10 <-> 26) during its loading splash.
    using DlssNr::IsFloatFlip;
    check(IsFloatFlip(kRgba16f, kR11g11b10) && IsFloatFlip(kR11g11b10, kRgba16f), "10 <-> 26 is a float flip");
    check(!IsFloatFlip(kR11g11b10, kR11g11b10), "no change is no flip");
    check(!IsFloatFlip(0, kR11g11b10), "the first frame is no flip");
    check(!IsFloatFlip(kRgba8, kRgba16f) && !IsFloatFlip(kRgb10a2, kR11g11b10), "other pairs are not");
    check(CanonicalColourFormat(kR11g11b10) == kR11g11b10, "before a flip R11G11B10F keeps its surfaces");
    check(CanonicalColourFormat(kR11g11b10, true) == kRgba16f, "after a flip R11G11B10F gets RGBA16F surfaces");
    check(CanonicalColourFormat(kRgba16f, true) == kRgba16f, "after a flip RGBA16F stays");
    check(CanonicalColourFormat(kBgra8, true) == kRgba8 && CanonicalColourFormat(kRgb10a2, true) == kRgb10a2,
          "the flag moves nothing else");
    {
        const int a = CanonicalColourFormat(kRgba16f, true), b = CanonicalColourFormat(kR11g11b10, true);
        check(!SurfacesStale(a, b, a, b) && !SurfacesStale(b, a, b, a), "after the flip, 10 <-> 26 keeps the model");
    }

    // RebuildGuard. Sim plays the game against a model the way DlssNr_Dx12::Dispatch does: a call whose set-up differs
    // from the model's rebuilds it, if the guard lets it run.
    using DlssNr::RebuildGuard;
    using DlssNr::Setup;
    struct Sim
    {
        RebuildGuard guard;
        Setup model {};
        bool haveModel = false;
        int ran = 0, rebuilt = 0;

        bool Call(const Setup& s)
        {
            const bool wouldRebuild = haveModel && !(s == model);
            const bool run = guard.Allow(s, wouldRebuild);
            if (run)
            {
                ++ran;
                rebuilt += wouldRebuild ? 1 : 0;
                model = s;
                haveModel = true;
            }
            return run;
        }
    };
    const auto size = [&](unsigned w) { return Setup { kRgba16f, kRgba16f, w, w, w, w, true, false }; };
    const Setup a = size(100), b = size(200), c = size(300), d = size(400);
    {
        Sim sim;
        // Two set-ups alternating on every call: two rebuilds, then three returns stand NR down.
        for (int i = 0; i < 200; ++i)
            sim.Call(i & 1 ? b : a);
        check(sim.guard.standing, "alternating: stood down");
        check(sim.rebuilt == 3, "alternating: three rebuilds before the stand-down");
        check(sim.ran == 4, "alternating: four calls ran");
        check(sim.model == b, "alternating: the model stays as last built");
        // The game settles on a new set-up: kSettle calls in a row, the last one runs and builds it once.
        int waited = 0;
        while (!sim.Call(c) && waited < 100)
            ++waited;
        check(waited == (int) RebuildGuard::kSettle - 1, "settled: runs on the kSettle-th steady call");
        check(!sim.guard.standing && sim.model == c, "settled: running, built for the new set-up");
        check(sim.Call(c) && sim.Call(c), "settled: steady calls run");
        // Wary after the stand-down: one return trips it at once.
        sim.Call(d);
        check(!sim.Call(c) && sim.guard.standing, "wary: the first return after a stand-down stands down again");
    }
    {
        Sim sim;
        for (int i = 0; i < 10; ++i)
            sim.Call(i & 1 ? b : a);
        // A change while standing starts the count again.
        int calls = 0;
        for (int i = 0; i < 20; ++i, ++calls)
            sim.Call(c);
        while (!sim.Call(d) && calls < 200)
            ++calls;
        check(calls == 20 + (int) RebuildGuard::kSettle - 1, "a new set-up while standing restarts the settle count");
    }
    {
        // The window: a switch every k calls puts the first and third return 2k calls apart.
        const auto standsAt = [&](int k)
        {
            Sim sim;
            for (int i = 0; i < 200; ++i)
                sim.Call((i / k) & 1 ? b : a);
            return sim.guard.standing;
        };
        check(standsAt(7), "returns 14 calls apart: stood down");
        check(!standsAt(8), "returns 16 calls apart: not stood down");
    }
    {
        Sim sim;
        // Dynamic resolution moving on (a new size every 3 calls, never going back) is never stood down.
        for (int i = 0; i < 3000; ++i)
            sim.Call(size(100 + (unsigned) i / 3));
        check(!sim.guard.standing && sim.ran == 3000, "dynamic resolution stepping on: never stood down");
    }
    {
        Sim sim;
        // Dynamic resolution swinging back and forth slowly (every 20 calls) is not stood down either.
        for (int i = 0; i < 2000; ++i)
            sim.Call((i / 20) & 1 ? b : a);
        check(!sim.guard.standing && sim.ran == 2000, "slow back-and-forth: never stood down");
    }
    {
        // Three set-ups in turn (three pictures of different sizes per frame): stood down too.
        Sim sim;
        for (int i = 0; i < 300; ++i)
            sim.Call(i % 3 == 0 ? a : i % 3 == 1 ? b : c);
        check(sim.guard.standing && sim.rebuilt <= 5, "three set-ups in turn: stood down after a few rebuilds");
    }
    {
        // Four in turn: still within the recent ones.
        Sim sim;
        for (int i = 0; i < 300; ++i)
            sim.Call(i % 4 == 0 ? a : i % 4 == 1 ? b : i % 4 == 2 ? c : d);
        check(sim.guard.standing, "four set-ups in turn: stood down");
    }
    {
        // A call the caller does not count as a rebuild (no model yet) still becomes the one built.
        RebuildGuard g;
        g.Allow(a, false);
        g.Allow(b, false);
        g.Allow(c, true);
        check(!g.Allow(b, true) || !g.standing, "the set-up taken without a rebuild is remembered");
        check(g.built == b, "built follows calls that needed no rebuild");
    }
    {
        Sim sim;
        for (int i = 0; i < 500; ++i)
            sim.Call(a);
        check(!sim.guard.standing && sim.ran == 500 && sim.rebuilt == 0, "steady game: never stood down");
    }

    // The reviewer's probe: the ordinary size moved 960x544 -> 976x544 while the packed one did not.
    check(DlssNr::SizeStale(960, 544, 976, 544), "ordinary size moved, packed size did not");
    check(DlssNr::SizeStale(976, 544, 976, 560), "height moved");
    check(!DlssNr::SizeStale(976, 544, 976, 544), "same ordinary size: kept");
    check(!DlssNr::SizeStale(0, 0, 976, 544), "no surface yet");

    if (fails == 0)
        std::puts("PASS: nr_surface_formats_smoke");
    return fails != 0;
}
