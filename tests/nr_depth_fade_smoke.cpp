// Host test for the depth fade's maths (shaders/dlssnr/precompile/dlssnr_depthfade.hlsli, the very file the shader
// includes, compiled here as C++) and the block the shader reads (shaders/dlssnr/DlssNr_DepthFade.h).
//
// Build: cl /nologo /std:c++20 /EHsc /W4 tests\nr_depth_fade_smoke.cpp
// Run:   nr_depth_fade_smoke.exe
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>

// What HLSL has built in and the shared file uses.
static float saturate(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }
static float lerp(float a, float b, float t) { return a + (b - a) * t; }
using std::log2;
using std::max;
#include "../OptiScaler/shaders/dlssnr/precompile/dlssnr_depthfade.hlsli"
#include "../OptiScaler/shaders/dlssnr/DlssNr_DepthFade.h"

static int fails = 0;
static void Check(bool ok, const std::string& label)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", label.c_str());
        ++fails;
    }
}
static bool Near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

// "rest" the way the shader derives it from a device depth value: 1 - depth for standard depth (far = 1), the value
// itself for reversed depth (far = 0). The far plane is 0 either way, the near plane 1.
static float RestOf(float depth, bool inverted) { return saturate(inverted ? depth : 1.0f - depth); }

// The distance a rest value stands for, undone: rest = 2^(-16 * d).
static float RestAt(float d) { return std::exp2(-16.0f * d); }

static float Keep(float rest, const DlssNrDepthFade::Settings& s)
{
    return DepthFadeFactor(rest, s.farStart, s.farEnd, s.farStrength, s.nearStart, s.nearEnd, s.nearStrength,
                           s.skyStrength);
}

int main()
{
    using DlssNrDepthFade::Settings;

    // Distance: 0 at the near plane, rising by 1/16 per doubling, 1 from 65536 times further and at the far plane.
    Check(Near(DepthFadeDistance(1.0f), 0.0f), "rest 1 (near plane) is distance 0");
    Check(Near(DepthFadeDistance(0.5f), 1.0f / 16.0f), "one doubling is 1/16");
    Check(Near(DepthFadeDistance(0.25f), 2.0f / 16.0f), "two doublings are 2/16");
    Check(Near(DepthFadeDistance(RestAt(0.5f)), 0.5f), "RestAt inverts the distance");
    Check(Near(DepthFadeDistance(kDepthFadeMinRest), 1.0f), "the floor is distance 1");
    Check(Near(DepthFadeDistance(0.0f), 1.0f), "the far plane is distance 1");
    Check(DepthFadeDistance(2.0f) == 0.0f, "rest above 1 is held at distance 0");
    {
        float last = -1.0f;
        bool rising = true;
        for (float rest = 1.0f; rest > 1e-6f; rest *= 0.9f)
        {
            const float d = DepthFadeDistance(rest);
            rising = rising && d >= last;
            last = d;
        }
        Check(rising, "distance never falls as the surface gets further");
    }

    // Ramp: endpoints and midpoint, and the two degenerate orders do not divide by zero.
    Check(DepthFadeRamp(0.2f, 0.6f, 0.2f) == 0.0f, "ramp is 0 at its start");
    Check(DepthFadeRamp(0.2f, 0.6f, 0.6f) == 1.0f, "ramp is 1 at its end");
    Check(Near(DepthFadeRamp(0.2f, 0.6f, 0.4f), 0.5f), "ramp is 0.5 halfway (smoothstep)");
    Check(DepthFadeRamp(0.2f, 0.6f, 0.0f) == 0.0f && DepthFadeRamp(0.2f, 0.6f, 1.0f) == 1.0f, "ramp holds outside");
    Check(DepthFadeRamp(0.5f, 0.5f, 0.49f) == 0.0f && DepthFadeRamp(0.5f, 0.5f, 0.51f) == 1.0f,
          "start == end is a step, not a NaN");
    Check(DepthFadeRamp(0.7f, 0.3f, 0.9f) == 1.0f && DepthFadeRamp(0.7f, 0.3f, 0.5f) == 0.0f,
          "end below start is a step at the start");
    {
        float last = -1.0f;
        bool soft = true;
        for (float x = 0.0f; x <= 1.0f; x += 0.01f)
        {
            const float r = DepthFadeRamp(0.2f, 0.6f, x);
            soft = soft && r >= last && r >= 0.0f && r <= 1.0f;
            last = r;
        }
        Check(soft, "ramp rises smoothly and stays in 0..1");
    }

    // Far fade only.
    {
        Settings s;
        s.farStart = 0.4f;
        s.farEnd = 0.8f;
        s.farStrength = 0.25f;
        Check(Keep(RestAt(0.0f), s) == 1.0f, "far: at distance 0 all of NR is kept");
        Check(Near(Keep(RestAt(0.4f), s), 1.0f), "far: all kept at the start");
        Check(Near(Keep(RestAt(0.6f), s), 0.625f), "far: halfway between 1 and the strength at the middle of the ramp");
        Check(Near(Keep(RestAt(0.8f), s), 0.25f), "far: the strength at the end");
        Check(Near(Keep(RestAt(0.95f), s), 0.25f), "far: the strength beyond the end");
    }

    // Near fade only, and near and far together.
    {
        Settings s;
        s.farStrength = 1.0f;
        s.nearStart = 0.1f;
        s.nearEnd = 0.3f;
        s.nearStrength = 0.2f;
        Check(Near(Keep(RestAt(0.0f), s), 0.2f), "near: the strength at the camera");
        Check(Near(Keep(RestAt(0.1f), s), 0.2f), "near: the strength up to the start");
        Check(Near(Keep(RestAt(0.2f), s), 0.6f), "near: halfway up the ramp");
        Check(Near(Keep(RestAt(0.3f), s), 1.0f) && Near(Keep(RestAt(0.5f), s), 1.0f), "near: all kept from the end");

        s.farStart = 0.5f;
        s.farEnd = 0.7f;
        s.farStrength = 0.5f;
        Check(Near(Keep(RestAt(0.05f), s), 0.2f), "near and far: near part only close up");
        Check(Near(Keep(RestAt(0.4f), s), 1.0f), "near and far: full NR in between");
        Check(Near(Keep(RestAt(0.9f), s), 0.5f), "near and far: far part only far away");
        // Overlapping ranges multiply
        s.nearStart = 0.5f;
        s.nearEnd = 1.0f;
        s.nearStrength = 0.5f;
        Check(Near(Keep(RestAt(0.6f), s), lerp(1.0f, 0.5f, DepthFadeRamp(0.5f, 0.7f, 0.6f)) *
                                              lerp(0.5f, 1.0f, DepthFadeRamp(0.5f, 1.0f, 0.6f))),
              "near and far: overlapping ramps multiply");
    }

    // Strengths 0 and 1.
    {
        Settings s;
        s.farStart = 0.0f;
        s.farEnd = 0.01f;
        s.farStrength = 0.0f;
        Check(Keep(RestAt(0.5f), s) == 0.0f, "far strength 0 keeps none of NR far away");
        s.farStrength = 1.0f;
        for (float d : { 0.0f, 0.2f, 0.5f, 0.9f, 1.0f })
            Check(Keep(RestAt(d), s) == 1.0f, "all strengths 1 keep everything at distance " + std::to_string(d));
        s.farStrength = 7.0f; // out of range: held to 1
        s.nearStrength = -3.0f; // out of range: held to 0
        s.nearStart = 0.0f;
        s.nearEnd = 0.2f;
        Check(Keep(RestAt(0.9f), s) == 1.0f && Keep(RestAt(0.0f), s) == 0.0f, "strengths outside 0..1 are held to it");
    }

    // Sky: the far plane takes the sky strength in place of the far fade, in either depth direction.
    {
        Settings s;
        s.farStart = 0.2f;
        s.farEnd = 0.4f;
        s.farStrength = 0.5f;
        s.skyStrength = 1.0f;
        Check(Keep(RestOf(1.0f, false), s) == 1.0f, "sky, standard depth, sky strength 1: all kept");
        Check(Keep(RestOf(0.0f, true), s) == 1.0f, "sky, reversed depth, sky strength 1: all kept");
        Check(Near(Keep(RestOf(0.9999f, false), s), 0.5f), "just short of the far plane is far, not sky");
        s.skyStrength = 0.0f;
        Check(Keep(RestOf(1.0f, false), s) == 0.0f, "sky, standard depth, sky strength 0: none kept");
        Check(Keep(RestOf(0.0f, true), s) == 0.0f, "sky, reversed depth, sky strength 0: none kept");
        Check(Keep(RestOf(0.5f, false), s) == Keep(RestOf(0.5f, true), s), "both directions agree on a surface");
        s.skyStrength = 0.3f;
        Check(Near(Keep(RestOf(1.0f - 2e-6f, false), s), 0.3f), "a value a hair short of the plane is still sky");
        Check(Near(Keep(0.0f, s), 0.3f), "rest 0 is sky");
    }

    // Reversed and standard depth give the same keep factor for the same surface.
    {
        Settings s;
        s.farStart = 0.3f;
        s.farEnd = 0.7f;
        s.farStrength = 0.1f;
        bool same = true;
        for (float rest : { 0.9f, 0.3f, 0.01f, 1e-3f, 1e-4f })
            same = same && Near(Keep(RestOf(1.0f - rest, false), s), Keep(RestOf(rest, true), s), 1e-3f);
        Check(same, "standard depth 1 - r and reversed depth r are the same surface");
    }

    // The block the shader reads.
    {
        Settings s;
        s.enabled = true;
        s.farStrength = 0.4f;
        s.nearStrength = 0.6f;
        s.skyStrength = 0.2f;
        const DlssNrDepthFade::DepthRegion region { 960, 540, 4, 8 };

        const auto on = DlssNrDepthFade::MakeBlock(s, true, false, region);
        Check(on.Flags == DlssNrDepthFade::kActive, "block: active, standard depth");
        Check(on.Width == 960 && on.Height == 540 && on.BaseX == 4 && on.BaseY == 8, "block: the depth region");
        Check(on.FarStrength == 0.4f && on.NearStrength == 0.6f && on.SkyStrength == 0.2f, "block: the strengths");

        const auto reversed = DlssNrDepthFade::MakeBlock(s, true, true, region);
        Check(reversed.Flags == (DlssNrDepthFade::kActive | DlssNrDepthFade::kInverted), "block: reversed depth flag");

        const auto noDepth = DlssNrDepthFade::MakeBlock(s, false, false, region);
        Check(noDepth.Flags == 0 && noDepth.Width == 0 && noDepth.FarStrength == 0.0f, "block: no depth is all zero (k = 1)");

        const auto emptyRegion = DlssNrDepthFade::MakeBlock(s, true, false, {});
        Check(emptyRegion.Flags == 0, "block: an empty depth region is no depth");

        Settings off = s;
        off.enabled = false;
        const auto disabled = DlssNrDepthFade::MakeBlock(off, true, false, region);
        Check(disabled.Flags == 0 && disabled.Width == 0, "block: off is all zero");

        Settings odd = s;
        odd.farStart = std::numeric_limits<float>::quiet_NaN();
        odd.farStrength = 5.0f;
        odd.nearStrength = -1.0f;
        const auto held = DlssNrDepthFade::MakeBlock(odd, true, false, region);
        Check(held.FarStart == 0.5f && held.FarStrength == 1.0f && held.NearStrength == 0.0f,
              "block: a NaN takes its default, out-of-range strengths are held to 0..1");
    }

    // The menu state.
    {
        Settings s;
        DlssNrDepthFade::Report(s, true);
        Check(DlssNrDepthFade::Current() == DlssNrDepthFade::State::Unknown, "state: off reports nothing");
        s.enabled = true;
        DlssNrDepthFade::Report(s, true);
        Check(DlssNrDepthFade::Current() == DlssNrDepthFade::State::Active, "state: on with depth");
        DlssNrDepthFade::Report(s, false);
        Check(DlssNrDepthFade::Current() == DlssNrDepthFade::State::NoDepth, "state: on without depth");
    }

    if (fails)
    {
        std::printf("%d check(s) failed\n", fails);
        return 1;
    }
    std::puts("PASS: depth fade maths (distance, ramps, near/far/sky, both depth directions, strengths 0 and 1, no depth)");
    return 0;
}
