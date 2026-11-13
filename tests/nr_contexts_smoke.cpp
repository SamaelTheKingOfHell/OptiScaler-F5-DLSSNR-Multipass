// Host check of shaders/dlssnr/DlssNr_Contexts.h: what NR does with each of the game's DLSS features.
// cl /std:c++17 /EHsc /W4 tests/nr_contexts_smoke.cpp
#include "../OptiScaler/shaders/dlssnr/DlssNr_Contexts.h"

#include <cstdio>
#include <initializer_list>

int main()
{
    int fails = 0;
    const auto check = [&](bool ok, const char* what) {
        if (!ok)
        {
            std::printf("FAIL: %s\n", what);
            ++fails;
        }
    };
    using namespace DlssNr::Contexts;
    constexpr uint32_t world = 1000000, preview = 1000001, third = 1000002, fourth = 1000003, fifth = 1000004;
    // Neverness to Everness before SR: the world in R11G11B10F, the preview in RGBA16F, same size
    const Picture worldPic { 0x1000, 1705, 960, 26 }, previewPic { 0x2000, 1705, 960, 10 };
    const auto other = [](uint64_t res) { return Picture { res, 800, 600, 10 }; };

    {
        // One feature (most games): always main, said once.
        Map m;
        const Role r1 = m.Acquire(world, Mode::StandAside, worldPic);
        const Role r2 = m.Acquire(world, Mode::StandAside, worldPic);
        check(r1.kind == Kind::Main && r1.first && !r1.tookMain, "the first feature is main");
        check(r2.kind == Kind::Main && !r2.first, "and stays main");
        check(m.Acquire(0, Mode::StandAside, previewPic).kind == Kind::Main, "an unknown feature (native path) is main");
    }
    {
        // Neverness to Everness: the preview next to the world, both every frame.
        Map m;
        m.Acquire(world, Mode::StandAside, worldPic);
        const Role a = m.Acquire(preview, Mode::StandAside, previewPic);
        const Role b = m.Acquire(preview, Mode::StandAside, previewPic);
        check(a.kind == Kind::StandAside && a.first, "stand aside: the preview passes untouched, said once");
        check(b.kind == Kind::StandAside && !b.first, "said once only");
        for (int i = 0; i < 2000; ++i)
        {
            m.Acquire(world, Mode::StandAside, worldPic);
            m.Acquire(preview, Mode::StandAside, previewPic);
        }
        check(m.Acquire(world, Mode::StandAside, worldPic).kind == Kind::Main, "both every frame: the world stays main");
        check(m.AsideCount() == 1, "one feature standing aside");
        m.Release(preview);
        check(m.AsideCount() == 0, "released: forgotten");
        check(m.Acquire(preview, Mode::StandAside, previewPic).first, "made again: said again");
    }
    {
        // Assetto Corsa EVO: one feature per screen and one for the combined picture, all in one colour allocation.
        Map m;
        const Picture rig { 0x3000, 5120, 960, 10 };
        m.Acquire(world, Mode::StandAside, rig);
        bool allMain = true;
        for (uint32_t h : { preview, third, fourth })
            allMain = m.Acquire(h, Mode::StandAside, rig).kind == Kind::Main && allMain;
        check(allMain && m.AsideCount() == 0, "features on main's colour allocation are views of main: main");
        // Same size and format in another resource: another picture
        check(m.Acquire(fifth, Mode::StandAside, Picture { 0x4000, 5120, 960, 10 }).kind == Kind::StandAside,
              "same size and format, another resource: another picture");
        // No resource said: size and format decide
        check(m.Acquire(fifth + 1, Mode::StandAside, Picture { 0, 5120, 960, 10 }).kind == Kind::Main,
              "no resource: same size and format count as main's picture");
    }
    {
        // Own model: a slot each, up to kOwnSlots, then stand aside.
        Map m;
        m.Acquire(world, Mode::OwnModel, worldPic);
        const Role p = m.Acquire(preview, Mode::OwnModel, previewPic);
        const Role p2 = m.Acquire(preview, Mode::OwnModel, previewPic);
        check(p.kind == Kind::Own && p.slot == 0 && p.first, "own model: the preview gets slot 0");
        check(p2.kind == Kind::Own && p2.slot == 0 && !p2.first, "and keeps it");
        check(m.Acquire(third, Mode::OwnModel, other(1)).slot == 1, "a third feature gets slot 1");
        check(m.Acquire(fourth, Mode::OwnModel, other(2)).slot == 2, "a fourth gets slot 2");
        check(m.Acquire(fifth, Mode::OwnModel, other(3)).kind == Kind::StandAside, "past the slots: stand aside");
        check(m.OwnCount() == 3 && m.AsideCount() == 1, "three own models, one aside");
        const Released r = m.Release(third);
        check(r.own && r.slot == 1, "released: its slot is to be parked");
        check(m.Acquire(fifth, Mode::OwnModel, other(3)).slot == 1, "the freed slot is taken by the next feature");
        // The setting moved off Own model: the slot is given back
        const Role off = m.Acquire(preview, Mode::StandAside, previewPic);
        check(off.kind == Kind::StandAside && off.freedSlot == 0, "own model switched off: the slot is freed");
        check(m.OwnCount() == 2, "two own models left");
    }
    {
        // Share: the old behaviour, every feature through the main state.
        Map m;
        m.Acquire(world, Mode::Share, worldPic);
        check(m.Acquire(preview, Mode::Share, previewPic).kind == Kind::Main, "share: everything is main");
    }
    {
        // Switched from Own model to Share: each feature gives its slot back on its next call.
        Map m;
        m.Acquire(world, Mode::OwnModel, worldPic);
        m.Acquire(preview, Mode::OwnModel, previewPic);
        const Role r = m.Acquire(preview, Mode::Share, previewPic);
        check(r.kind == Kind::Main && r.freedSlot == 0 && m.OwnCount() == 0, "share: the own slot is freed");
    }
    {
        // A feature that stood aside and turns into a view of main (main moved to its allocation) is forgotten.
        Map m;
        const Picture rig { 0x3000, 5120, 960, 10 }, combined { 0x3100, 7680, 1440, 10 };
        m.Acquire(world, Mode::OwnModel, combined);
        m.Acquire(preview, Mode::OwnModel, rig);
        check(m.OwnCount() == 1, "a per-screen feature with its own model while the combined view is main");
        // The combined view goes quiet; the left screen takes main, the other screen is a view of it
        Role r;
        for (int i = 0; i < 300; ++i)
            r = m.Acquire(third, Mode::OwnModel, rig);
        const Role v = m.Acquire(preview, Mode::OwnModel, rig);
        check(v.kind == Kind::Main && v.freedSlot == 0 && m.OwnCount() == 0 && m.AsideCount() == 0,
              "a former other picture that is a view of main gives its slot back, no phantom");
    }
    {
        // Main released and made again (a resolution change), nothing else alive: the new one is main, state kept.
        Map m;
        m.Acquire(world, Mode::StandAside, worldPic);
        m.Release(world);
        const Role r = m.Acquire(fifth, Mode::StandAside, Picture { 0x5000, 2560, 1440, 26 });
        check(r.kind == Kind::Main && !r.tookMain, "main made again: the new feature is main, history kept");
    }
    {
        // Main released while the preview stays open: the preview waits, the new world feature takes main.
        Map m;
        m.Acquire(world, Mode::StandAside, worldPic);
        for (int i = 0; i < 1000; ++i)
            m.Acquire(preview, Mode::StandAside, previewPic), m.Acquire(world, Mode::StandAside, worldPic);
        m.Release(world);
        check(m.Acquire(preview, Mode::StandAside, previewPic).kind == Kind::StandAside,
              "after main's release the preview (another picture) does not take main");
        const Role r = m.Acquire(fifth, Mode::StandAside, Picture { 0x6000, 2560, 1440, 26 });
        check(r.kind == Kind::Main && !r.tookMain, "the new world feature takes main");
        check(m.Acquire(preview, Mode::StandAside, previewPic).kind == Kind::StandAside, "the preview stays aside");
    }
    {
        // Made before the old one was released (a resolution change): the new feature succeeds main.
        Map m;
        m.Acquire(world, Mode::StandAside, worldPic);
        for (int i = 0; i < 1000; ++i)
            m.Acquire(world, Mode::StandAside, worldPic);
        const Picture smaller { 0x7000, 1280, 720, 26 };
        check(m.Acquire(fifth, Mode::StandAside, smaller).kind == Kind::StandAside, "made first: aside for now");
        m.Release(world);
        const Role r = m.Acquire(fifth, Mode::StandAside, smaller);
        check(r.kind == Kind::Main && r.tookMain, "the old one released: the successor takes main");
        check(m.AsideCount() == 0, "and is no longer counted aside (no phantom feature in the menu)");
    }
    {
        // Main released and only the preview goes on calling: after a while it takes main.
        Map m;
        m.Acquire(world, Mode::StandAside, worldPic);
        for (int i = 0; i < 1000; ++i)
            m.Acquire(preview, Mode::StandAside, previewPic), m.Acquire(world, Mode::StandAside, worldPic);
        m.Release(world);
        Role r;
        unsigned long long waited = 0;
        while ((r = m.Acquire(preview, Mode::StandAside, previewPic)).kind != Kind::Main && waited < 10000)
            ++waited;
        check(r.tookMain && waited == kSoloCalls - 1, "the preview calling alone takes main after kSoloCalls, reset");
    }
    {
        // Neverness to Everness resized on the Character page: both features made again, the new preview first.
        Map m;
        m.Acquire(world, Mode::StandAside, worldPic);
        for (int i = 0; i < 1000; ++i)
            m.Acquire(preview, Mode::StandAside, previewPic), m.Acquire(world, Mode::StandAside, worldPic);
        m.Release(world);
        m.Release(preview);
        const Picture newWorld { 0xA000, 2560, 1440, 26 }, newPreview { 0xB000, 2560, 1440, 10 };
        check(m.Acquire(fourth, Mode::StandAside, newPreview).kind == Kind::StandAside,
              "resize: a new feature in another format than main's does not take main");
        check(m.Acquire(fifth, Mode::StandAside, newWorld).kind == Kind::Main,
              "the new world feature (main's format) does");
        check(m.Acquire(fourth, Mode::StandAside, newPreview).kind == Kind::StandAside, "the new preview stays aside");
    }
    {
        // Main made again in another format, nothing else calling (HDR switched on): main after kSoloCalls calls.
        Map m;
        m.Acquire(world, Mode::StandAside, worldPic);
        m.Release(world);
        Role r;
        unsigned long long waited = 0;
        while ((r = m.Acquire(fifth, Mode::StandAside, Picture { 0xC000, 1705, 960, 10 })).kind != Kind::Main &&
               waited < 10000)
            ++waited;
        check(waited == kSoloCalls - 1, "another format, calling alone: main after kSoloCalls calls");
    }
    {
        // Leaving Share mode does not find main quiet.
        Map m;
        for (int i = 0; i < 1000; ++i)
            m.Acquire(world, Mode::Share, worldPic), m.Acquire(preview, Mode::Share, previewPic);
        check(m.Acquire(preview, Mode::StandAside, previewPic).kind == Kind::StandAside,
              "after Share, the preview calling first does not take main");
        check(m.Acquire(world, Mode::StandAside, worldPic).kind == Kind::Main, "the world is still main");
    }
    {
        // A larger picture takes main (a menu scene's small feature made first).
        Map m;
        const Picture small { 0x8000, 960, 540, 10 }, big { 0x9000, 1920, 1080, 10 };
        m.Acquire(world, Mode::StandAside, small);
        const Role r = m.Acquire(preview, Mode::StandAside, big);
        check(r.kind == Kind::Main && r.tookMain, "a clearly larger picture takes main");
        check(m.Acquire(world, Mode::StandAside, small).kind == Kind::StandAside, "the small one stands aside");
        check(m.Acquire(world, Mode::StandAside, Picture { 0x8000, 1700, 960, 10 }).kind == Kind::StandAside,
              "less than 25% more area: no takeover");
    }
    {
        // A first feature that goes quiet (a menu scene never released) gives main away.
        Map m;
        m.Acquire(world, Mode::StandAside, worldPic);
        Role r;
        unsigned long long calls = 0;
        while ((r = m.Acquire(preview, Mode::StandAside, previewPic)).kind != Kind::Main && calls < 10000)
            ++calls;
        check(r.tookMain && calls == kQuietCalls, "main quiet for kQuietCalls calls: the other takes it");
    }
    {
        // Own model: a feature with a slot that takes main gives the slot back to be parked.
        Map m;
        m.Acquire(world, Mode::OwnModel, worldPic);
        m.Acquire(preview, Mode::OwnModel, previewPic);
        Role r;
        unsigned long long calls = 0;
        while ((r = m.Acquire(preview, Mode::OwnModel, previewPic)).kind != Kind::Main && calls < 10000)
            ++calls;
        check(r.tookMain && r.freedSlot == 0 && m.OwnCount() == 0, "taking main frees the slot for parking");
    }
    {
        // Its own model failed: the feature stands aside for good, the slot is parked.
        Map m;
        m.Acquire(world, Mode::OwnModel, worldPic);
        m.Acquire(preview, Mode::OwnModel, previewPic);
        check(m.Failed(preview) == 0, "failed: its slot is to be parked");
        check(m.Acquire(preview, Mode::OwnModel, previewPic).kind == Kind::StandAside, "and it stands aside after");
        check(m.Failed(world) == -1, "main is not in the map: nothing to park");
    }
    {
        // The output size, not the allocation, decides "larger": a big render-target pool is no bigger picture.
        Map m;
        m.Acquire(world, Mode::StandAside, Picture { 0x1, 1920, 1080, 26, 1920, 1080 });
        check(m.Acquire(preview, Mode::StandAside, Picture { 0x2, 3840, 2160, 10, 960, 540 }).kind == Kind::StandAside,
              "a small render in a large allocation does not take main");
    }
    {
        Map m;
        m.Acquire(world, Mode::OwnModel, worldPic);
        m.Acquire(preview, Mode::OwnModel, previewPic);
        m.Reset();
        check(m.OwnCount() == 0 && m.Acquire(preview, Mode::OwnModel, previewPic).kind == Kind::Main,
              "reset: a fresh map");
    }

    if (fails == 0)
        std::puts("PASS: nr_contexts_smoke");
    return fails != 0;
}
