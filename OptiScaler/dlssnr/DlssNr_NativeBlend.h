#pragma once

// "Native blend" preset on the Neural Rendering Status & Presets page ("Native Presets" card): run NR on the finished
// picture and write the model's answer as it is, leaving all blending to NR's own Intensity. It sets four settings and
// remembers what they were, so turning it off puts every one back (an unset one stays unset). Plain data and rules, no
// config and no ImGui, so
// tests/nr_native_blend_smoke.cpp runs them; dlssnr/DlssNr_Menu.cpp draws the control and Config.cpp keeps the
// remembered values in [DlssNr] (NativeBlend and NativeBlendWas*).
//
// What the settings do on a tone-mapped (SDR) frame, where nothing else of ours touches the picture:
//   FinishedPicture      NR runs on the finished picture
//   ReversibleMode 2     Replace: the model's answer is the picture, with no composition and no highlight guard
//   SkinProtection off   no skin / environment attenuation of the final edit
//   ReplaceDetailStrength 0   no native detail put back below 100% model resolution
// On an HDR frame the answer still goes through a curve, so the menu says it is not a true native match.

#include <cstdint>
#include <optional>

namespace DlssNrNativeBlend
{
// The four settings as the config holds them: nullopt is "auto", which the ini keeps apart from a value that merely
// equals the default.
struct Settings
{
    std::optional<bool> finishedPicture;
    std::optional<uint32_t> reversibleMode;
    std::optional<bool> skinProtection;
    std::optional<float> replaceDetail;

    bool operator==(const Settings&) const = default;
};

// The same four with the config's defaults applied, for asking what is in effect.
struct Resolved
{
    bool finishedPicture = false;
    uint32_t reversibleMode = 0;
    bool skinProtection = false;
    float replaceDetail = 0.0f;
};

struct State
{
    bool on = false;
    Settings saved; // meaningful only while on

    bool operator==(const State&) const = default;
};

// What Native blend writes.
inline Settings NativeSettings() { return { true, 2u, false, 0.0f }; }

// Replace modes: Reversible curve + replace (2) and Balanced curve + replace (4). Both take the model's answer as it
// is on a tone-mapped frame, so moving between them does not end Native blend.
inline bool IsReplaceMode(uint32_t mode) { return mode == 2u || mode == 4u; }

// Whether the settings in effect are Native blend's.
inline bool Matches(const Resolved& now)
{
    return now.finishedPicture && IsReplaceMode(now.reversibleMode) && !now.skinProtection && now.replaceDetail == 0.0f;
}

// Turning it on. The first time the settings in effect are remembered; on again keeps what was remembered, so the
// values the player had are never replaced by Native blend's own.
inline Settings TurnOn(State& state, const Settings& current)
{
    if (!state.on)
    {
        state.on = true;
        state.saved = current;
    }

    return NativeSettings();
}

// Turning it off: what to put back, nothing when it was not on.
inline std::optional<Settings> TurnOff(State& state)
{
    if (!state.on)
        return std::nullopt;

    const Settings saved = state.saved;
    state = State {};
    return saved;
}

// Called before the control is drawn and the state read. A setting changed while it was on (the placement, the curve,
// skin protection or Restore Sharpness in their own controls, a look, a profile, an edited ini) ends Native blend and
// the player's change stands: nothing is put back, because there is no longer one state to go back from. Returns true
// when it ended.
inline bool Reconcile(State& state, const Resolved& now)
{
    if (!state.on || Matches(now))
        return false;

    state = State {};
    return true;
}

// A colour grade layer (the input LUT or one of the post-NR layers) changes the picture when it has a file and a
// strength above 0. Native blend leaves the player's grade alone and the menu says so.
inline bool LayerGrades(bool hasFile, float strength) { return hasFile && strength > 0.0f; }

// What the checkbox shows.
inline bool Active(const State& state, const Resolved& now) { return state.on && Matches(now); }

// NR on the finished picture pauses while the game's own frame generation runs without a picture handoff
// (StreamlinePicture::PauseEvaluation). The menu then says Paused instead of Running.
inline bool FinishedPicturePaused(bool finishedPictureSetting, bool nativeDx12, bool pauseEvaluation)
{
    return finishedPictureSetting && nativeDx12 && pauseEvaluation;
}
} // namespace DlssNrNativeBlend
