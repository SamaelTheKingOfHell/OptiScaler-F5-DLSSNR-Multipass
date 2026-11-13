#pragma once

// The depth fade: Neural Rendering's result is kept less where the surface is far away (or very near, or sky). The
// maths is shared with the shader (precompile/dlssnr_depthfade.hlsli); this header is the settings, the block of
// constants the shader reads and the state the menu shows. No dependencies, so tests/nr_depth_fade_smoke.cpp can use
// it.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace DlssNrDepthFade
{
// The user's choices, as the ini and the menu hold them.
struct Settings
{
    bool enabled = false;
    float farStart = 0.5f;
    float farEnd = 0.85f;
    float farStrength = 0.5f;
    float nearStart = 0.0f;
    float nearEnd = 0.1f;
    float nearStrength = 1.0f;
    float skyStrength = 1.0f;
};

// The settings from the config (any object with the DlssNrDepthFade* members; kept a template so this header does not
// depend on Config.h).
template <class Cfg> Settings SettingsFrom(const Cfg& cfg)
{
    Settings s;
    s.enabled = cfg.DlssNrDepthFade.value_or_default();
    s.farStart = cfg.DlssNrDepthFadeFarStart.value_or_default();
    s.farEnd = cfg.DlssNrDepthFadeFarEnd.value_or_default();
    s.farStrength = cfg.DlssNrDepthFadeFarStrength.value_or_default();
    s.nearStart = cfg.DlssNrDepthFadeNearStart.value_or_default();
    s.nearEnd = cfg.DlssNrDepthFadeNearEnd.value_or_default();
    s.nearStrength = cfg.DlssNrDepthFadeNearStrength.value_or_default();
    s.skyStrength = cfg.DlssNrDepthFadeSkyStrength.value_or_default();
    return s;
}

// Flags in Block::Flags.
constexpr uint32_t kActive = 1u;   // the fade runs: it is on and the game gave depth
constexpr uint32_t kInverted = 2u; // depth runs the other way round (the far plane is 0)

// What the shader's constant buffer carries, twelve 4-byte scalars in this order (dlssnr.hlsl's gDepthFade* fields).
struct Block
{
    uint32_t Flags;
    uint32_t Width; // the depth's valid region, in texels
    uint32_t Height;
    uint32_t BaseX; // where it starts in the texture
    uint32_t BaseY;
    float FarStart;
    float FarEnd;
    float FarStrength;
    float NearStart;
    float NearEnd;
    float NearStrength;
    float SkyStrength;
};
static_assert(sizeof(Block) == 12 * 4);

struct DepthRegion
{
    uint32_t width = 0, height = 0, baseX = 0, baseY = 0;
};

inline float Finite01(float v, float fallback) { return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : fallback; }

// The shader block for this frame. Without depth (haveDepth false or an empty region) the block is all zero: the
// fade is off for the frame, which the shader reads as a factor of 1 everywhere, and nothing touches the depth slot.
inline Block MakeBlock(const Settings& s, bool haveDepth, bool inverted, const DepthRegion& region)
{
    Block b {};

    if (!s.enabled || !haveDepth || region.width == 0 || region.height == 0)
        return b;

    b.Flags = kActive | (inverted ? kInverted : 0u);
    b.Width = region.width;
    b.Height = region.height;
    b.BaseX = region.baseX;
    b.BaseY = region.baseY;
    b.FarStart = Finite01(s.farStart, 0.5f);
    b.FarEnd = Finite01(s.farEnd, 0.85f);
    b.FarStrength = Finite01(s.farStrength, 1.0f);
    b.NearStart = Finite01(s.nearStart, 0.0f);
    b.NearEnd = Finite01(s.nearEnd, 0.1f);
    b.NearStrength = Finite01(s.nearStrength, 1.0f);
    b.SkyStrength = Finite01(s.skyStrength, 1.0f);
    return b;
}

// What the menu says about the fade. Written by the passes each frame they run, read by the menu.
enum class State : int
{
    Unknown = 0, // off, or NR has not run since it was switched on
    Active = 1,  // on and reading the game's depth
    NoDepth = 2, // on, but the game gave no depth this frame: the fade does nothing
};

inline std::atomic<int> g_state { (int) State::Unknown };

inline void Report(const Settings& s, bool depthUsable)
{
    g_state.store((int) (!s.enabled    ? State::Unknown
                         : depthUsable ? State::Active
                                       : State::NoDepth),
                  std::memory_order_relaxed);
}

inline State Current() { return (State) g_state.load(std::memory_order_relaxed); }
} // namespace DlssNrDepthFade
