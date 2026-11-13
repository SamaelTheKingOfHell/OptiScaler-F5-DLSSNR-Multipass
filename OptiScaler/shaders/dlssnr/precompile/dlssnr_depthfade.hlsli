// The depth fade's maths: how much of Neural Rendering's result a pixel keeps, from how far away the surface is.
// Scalar functions only, no resources, so dlssnr.hlsl and the host test (tests/nr_depth_fade_smoke.cpp, which defines
// saturate / lerp and includes this file as C++) run the very same code.
//
// The game's depth buffer does not say how far a surface is in any unit: its near and far planes are not known here.
// What it does say is how close the value sits to the far plane, and for the usual perspective depth that quantity
// ("rest": 1 minus the far-is-one depth) is the near plane's distance divided by the surface's distance. Taking its
// base-2 logarithm turns it into "how many doublings of distance past the near plane", which is the scale a person
// reads distance on (a metre away and two metres away differ as much as 100 and 200 do). The distance used by the
// controls is that number over 16 doublings, held to 0..1: 0 at the near plane, 1 at 65536 times its distance or
// further. Linear depth buffers are read on the same scale, only less evenly.
#ifndef DLSSNR_DEPTHFADE_HLSLI
#define DLSSNR_DEPTHFADE_HLSLI

// How close to the far plane a value has to be to count as sky (nothing is drawn there): the plane itself, with room
// for a 16-bit buffer's steps.
static const float kDepthFadeSkyRest = 1e-5f;
// The floor the logarithm is taken at: 2^-16, so the distance reaches 1 there and sky does not run off the scale.
static const float kDepthFadeMinRest = 1.52587890625e-5f;
static const float kDepthFadeOctaves = 16.0f;

// 0..1 distance for a "rest" value (1 at the camera's near plane, 0 at the far plane).
float DepthFadeDistance(float rest)
{
    return saturate(-log2(max(rest, kDepthFadeMinRest)) / kDepthFadeOctaves);
}

// A soft step from 0 at start to 1 at end. An end at or below the start is a hard step at the start (no division by
// zero, no inverted ramp).
float DepthFadeRamp(float start, float end, float x)
{
    const float t = saturate((x - start) / max(end - start, 1e-4f));
    return t * t * (3.0f - 2.0f * t);
}

// How much of the model's result to keep at a pixel whose nearest surface has this rest value:
//   far:   1 up to farStart, falling smoothly to farStrength at farEnd and beyond
//   near:  nearStrength up to nearStart, rising smoothly to 1 at nearEnd (1 everywhere when nearStrength is 1)
//   sky:   a pixel at the far plane takes skyStrength on its own, in place of the far fade
// Every strength is held to 0..1. With the defaults of the strengths (all 1) the answer is exactly 1 everywhere.
float DepthFadeFactor(float rest, float farStart, float farEnd, float farStrength, float nearStart, float nearEnd,
                      float nearStrength, float skyStrength)
{
    if (rest <= kDepthFadeSkyRest)
        return saturate(skyStrength);

    const float d = DepthFadeDistance(rest);
    const float keepFar = lerp(1.0f, saturate(farStrength), DepthFadeRamp(farStart, farEnd, d));
    const float keepNear = lerp(saturate(nearStrength), 1.0f, DepthFadeRamp(nearStart, nearEnd, d));
    return saturate(keepFar * keepNear);
}

#endif
