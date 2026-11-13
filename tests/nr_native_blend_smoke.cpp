// Host check of dlssnr/DlssNr_NativeBlend.h: turning Native blend on and off, and what a change made while it is on does.
// No GPU, no game.
// cl /nologo /std:c++20 /EHsc /W4 tests\nr_native_blend_smoke.cpp
#include "../OptiScaler/dlssnr/DlssNr_NativeBlend.h"

#include <cstdio>
#include <string>

using namespace DlssNrNativeBlend;

static int fails = 0;
static void Check(bool ok, const std::string& label)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", label.c_str());
        ++fails;
    }
}

// What the config would resolve these to (the defaults of Config.h: false, 0, false, 0.5).
static Resolved Resolve(const Settings& s)
{
    return { s.finishedPicture.value_or(false), s.reversibleMode.value_or(0u), s.skinProtection.value_or(false),
             s.replaceDetail.value_or(0.5f) };
}

static void NativeValues()
{
    const Settings native = NativeSettings();
    Check(native.finishedPicture == true && native.reversibleMode == 2u && native.skinProtection == false &&
              native.replaceDetail == 0.0f,
          "Native blend writes Finished Picture, Replace (2), skin protection off, detail 0");
    Check(Matches(Resolve(native)), "its own settings match");
    Check(!Matches(Resolve(Settings {})), "the defaults do not match");
}

// On then off puts every setting back, whatever it was, including unset ones.
static void RestoresEverything()
{
    const std::optional<bool> bools[] = { std::nullopt, false, true };
    const std::optional<uint32_t> modes[] = { std::nullopt, 0u, 1u, 2u, 3u, 4u, 7u };
    const std::optional<float> details[] = { std::nullopt, 0.0f, 0.5f, 1.25f };

    int cases = 0;
    for (const auto& finished : bools)
        for (const auto& mode : modes)
            for (const auto& skin : bools)
                for (const auto& detail : details)
                {
                    const Settings before { finished, mode, skin, detail };
                    State state;
                    const Settings written = TurnOn(state, before);
                    Check(written == NativeSettings(), "on writes the native settings");
                    Check(Active(state, Resolve(written)), "active with the native settings in effect");

                    const auto back = TurnOff(state);
                    ++cases;
                    Check(back.has_value() && *back == before, "off restores exactly the earlier settings (" +
                                                                   std::to_string(cases) + ")");
                    Check(state == State {}, "off clears the remembered values");
                }
    Check(cases == 3 * 7 * 3 * 4, "every combination ran");
}

// An unset setting is put back unset, not as its default number.
static void AutoStaysAuto()
{
    const Settings before { std::nullopt, std::nullopt, std::nullopt, std::nullopt };
    State state;
    TurnOn(state, before);
    const auto back = TurnOff(state);
    Check(back.has_value() && !back->finishedPicture && !back->reversibleMode && !back->skinProtection &&
              !back->replaceDetail,
          "auto comes back as auto");
}

static void OnTwice()
{
    const Settings before { std::nullopt, 3u, true, 1.0f };
    State state;
    TurnOn(state, before);

    // The second click arrives with the native settings already in effect: they must not replace the remembered ones.
    const Settings second = TurnOn(state, NativeSettings());
    Check(second == NativeSettings(), "on twice still writes the native settings");
    Check(state.on && state.saved == before, "on twice keeps the first remembered values");

    const auto back = TurnOff(state);
    Check(back.has_value() && *back == before, "off after on twice restores the first values");
}

static void OffWhenNotOn()
{
    State state;
    Check(!TurnOff(state).has_value(), "off when it was not on does nothing");
}

// A change made while it is on ends it and keeps the change; nothing is put back later.
static void ChangeWhileOn()
{
    const Settings before { false, 0u, true, 0.5f };

    {
        State state;
        TurnOn(state, before);
        Settings now = NativeSettings();
        now.replaceDetail = 0.7f; // Restore Sharpness moved
        Check(Reconcile(state, Resolve(now)), "a changed Restore Sharpness ends Native blend");
        Check(state == State {}, "the remembered values are dropped");
        Check(!Active(state, Resolve(now)), "no longer shown as on");
        Check(!TurnOff(state).has_value(), "nothing is put back afterwards");
    }
    {
        State state;
        TurnOn(state, before);
        Settings now = NativeSettings();
        now.skinProtection = true;
        Check(Reconcile(state, Resolve(now)), "skin protection turned on ends it");
    }
    {
        State state;
        TurnOn(state, before);
        Settings now = NativeSettings();
        now.finishedPicture = false;
        Check(Reconcile(state, Resolve(now)), "another placement ends it");
    }
    {
        State state;
        TurnOn(state, before);
        Settings now = NativeSettings();
        now.reversibleMode = 1u; // composed
        Check(Reconcile(state, Resolve(now)), "a composed curve ends it");
    }
    {
        // Replace on the other curve is still Replace: it stays on, and off still restores the first values.
        State state;
        TurnOn(state, before);
        Settings now = NativeSettings();
        now.reversibleMode = 4u;
        Check(!Reconcile(state, Resolve(now)) && state.on, "the other Replace curve does not end it");
        const auto back = TurnOff(state);
        Check(back.has_value() && *back == before, "and off restores the first values");
    }
    {
        // Nothing changed: Reconcile leaves it alone.
        State state;
        TurnOn(state, before);
        Check(!Reconcile(state, Resolve(NativeSettings())) && state.on, "unchanged settings keep it on");
    }
    {
        // Not on: Reconcile has nothing to do.
        State state;
        Check(!Reconcile(state, Resolve(before)), "not on, nothing to end");
    }
}

// A saved "on" read at the next start (the ini) behaves the same as one made in the session.
static void AcrossRestart()
{
    const Settings before { std::nullopt, 1u, std::nullopt, 0.9f };
    State session;
    const Settings written = TurnOn(session, before);

    State restarted = session; // what the config holds, read back
    Check(!Reconcile(restarted, Resolve(written)) && Active(restarted, Resolve(written)), "still on after a restart");
    const auto back = TurnOff(restarted);
    Check(back.has_value() && *back == before, "off after a restart restores the earlier settings");

    // An ini edited while the game was closed so that the settings no longer match: it is not on any more.
    State edited = session;
    Settings changed = written;
    changed.skinProtection = true;
    Check(Reconcile(edited, Resolve(changed)), "an edited ini ends it at the next start");
}

// A grade layer counts with a file and a strength above 0.
static void Grades()
{
    Check(LayerGrades(true, 1.0f), "a file at full strength grades");
    Check(LayerGrades(true, 0.01f), "a file at low strength grades");
    Check(!LayerGrades(true, 0.0f), "strength 0 does not grade");
    Check(!LayerGrades(false, 1.0f), "no file does not grade");
}

// Paused only on the finished picture, on a native DirectX 12 game, while the game's frame generation blocks it.
static void Paused()
{
    Check(FinishedPicturePaused(true, true, true), "finished picture under game frame generation is paused");
    Check(!FinishedPicturePaused(false, true, true), "after SR is not paused by game frame generation");
    Check(!FinishedPicturePaused(true, false, true), "not native DirectX 12 is not reported as paused");
    Check(!FinishedPicturePaused(true, true, false), "no game frame generation is not paused");
}

int main()
{
    Grades();
    Paused();
    NativeValues();
    RestoresEverything();
    AutoStaysAuto();
    OnTwice();
    OffWhenNotOn();
    ChangeWhileOn();
    AcrossRestart();

    if (fails)
    {
        std::printf("%d check(s) failed\n", fails);
        return 1;
    }

    std::puts("PASS: Native blend on/off restores every setting, auto stays auto, on twice keeps the first values");
    return 0;
}
