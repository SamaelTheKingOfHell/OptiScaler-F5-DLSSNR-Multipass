// Story 2 of the LUT-apply epic (dlssnr-lut-apply): grades the NR input image
// through a loaded 3D LUT (DlssNr_LutFile.h's Lut3D, uploaded by DlssNr_Dx12's DispatchLut) before the model
// ever sees it.
//
// Its own tiny root signature and descriptor table -- not dlssnr.hlsl's shared t0..t4/u0..u1 table -- because
// it is the first pass in this family to sample a 3D texture, and keeping it separate means every other
// pass's dispatch call sites and compiled bytecode stay untouched (DlssNr_Dx12.h's DispatchLut sits beside
// DispatchExposureAdapt/DispatchDetailReuse, with its own PSO and heap ring, not the shared one).
//
// t0 the pass's input (the real NGX colour resource, before Encode ever sees it), t1 the LUT as a 3D
// texture, t2 the live 1x1 exposure the encode reads (a stand-in when there is none), s0 a static linear-clamp
// sampler, u0 the graded output, b0 these constants.
//
// Decode is deliberately NOT dlssnr.hlsl's DecodeGameColour/EncodeGameColour: those land in the model's own
// working domain (tone-mapped sRGB OR linear light, depending on gInputEncoding there), not in the
// gamma-encoded display-referred 0-1 a .cube LUT is defined over. This shader decodes straight to display
// 0-1 with the same low-level curves dlssnr.hlsl itself is built from (LinearToSrgb/SrgbToLinear and
// NeutwoEncode/NeutwoDecode from dlssnr_replace_curve.hlsli, DecodePQ/EncodePQ from dlssnr_pq.hlsli), so a
// gamma 2.2 (and passthrough/tone-mapped-sRGB) frame is already there with nothing to convert, a PQ frame
// is tone-mapped through the same ITU-R BT.2408 reference-white convention Finished Picture/
// PqToReferenceLinear use before the sRGB encode, and open-ended scene-linear light (colourIsLinearHdr) is
// divided by the encode's white point and compressed with Neutwo -- the same reversible proxy curve the model's own encode
// pass uses, not a new one invented for this pass -- before the sRGB encode.
//
// Fixed (Review Pass, 2026-10-04): this used to fall through scene-linear HDR to the gamma-2.2 branch
// (`saturate(c)`), because inputEncoding alone cannot tell "already display-referred" apart from "wide open
// linear light" -- DlssNrColourEncoding::ShaderConversion collapses both to the same value. A LinearHDR
// frame (the normal outcome for most HDR games) was being clipped straight to 0-1 with no tonemap at all.

#include "dlssnr_pq.hlsli"          // DecodePQ/EncodePQ, kBt2020To709/kBt709To2020
#include "dlssnr_replace_curve.hlsli" // LinearToSrgb/SrgbToLinear

#ifdef VK_MODE
[[vk::binding(0, 0)]]
#endif
cbuffer Params : register(b0)
{
    uint width;
    uint height;
    float strength; // 0..1 (clamped on the host): how much of the graded result reaches the output
    float domainMinR, domainMinG, domainMinB;
    float domainMaxR, domainMaxG, domainMaxB;
    uint lutSize;       // the cube's side length; coordinates are clamped to the domain before sampling
    uint inputEncoding; // DlssNrConstants::InputEncoding's values: 3 gamma 2.2, 4 PQ, else passthrough/sRGB
    // Fixed (Review Pass, 2026-10-04): inputEncoding's "else" bucket covers BOTH tone-mapped sRGB (already
    // display 0-1, nothing to do) AND open-ended scene-linear HDR (needs compressing first) -- it cannot
    // tell them apart (DlssNrColourEncoding::ShaderConversion collapses both to the same value). This
    // carries the real signal (frame.ColourIsLinearHdr) so the two are no longer treated as one.
    uint colourIsLinearHdr;
    // What to divide linear light (and PQ's decoded light) by before compressing it: the white point the encode uses
    // this frame, as the host resolved it. Named as dlssnr.hlsl names it, for dlssnr_whitepoint.hlsli. When the
    // encode recomputes its white point from a live exposure texture, so does this pass (the fields below, copied from
    // the encode's constants), so the LUT grades for exactly the white the model is shown, not a value 3-4 frames old.
    float gWhitePoint;
    float gPreExposure;
    uint  gUseGameExposure;
    uint  gUseExposureWhitePoint;
    float gExposureBaseScale;
    float gExposureTrim;
    uint  gExposureTrimPreview;
    uint  gExposureTrimAnchorCount;
    float gExposureTrimAnchorExposure0;
    float gExposureTrimAnchorTrim0;
    float gExposureTrimAnchorExposure1;
    float gExposureTrimAnchorTrim1;
    float gExposureTrimAnchorExposure2;
    float gExposureTrimAnchorTrim2;
    float gExposureTrimAnchorExposure3;
    float gExposureTrimAnchorTrim3;
    float gExposureTrimAnchorExposure4;
    float gExposureTrimAnchorTrim4;
    float gExposureTrimAnchorExposure5;
    float gExposureTrimAnchorTrim5;
    float gExposureTrimAnchorExposure6;
    float gExposureTrimAnchorTrim6;
    float gExposureTrimAnchorExposure7;
    float gExposureTrimAnchorTrim7;
    // How the lattice is laid out over the display value (DlssNr_LutFile.h LatticeCoord): 0 evenly, 1 log-spaced Linear
    // HDR light, for a post-NR stack baked with a look-input conversion (highlights up to the Neutwo clamp get lattice
    // points of their own instead of sharing the top one).
    uint latticeCoord;
};

#ifdef VK_MODE
[[vk::binding(1, 0)]]
#endif
Texture2D<float4> gSource : register(t0);
#ifdef VK_MODE
[[vk::binding(2, 0)]]
#endif
Texture3D<float4> gLut : register(t1);
#ifdef VK_MODE
[[vk::binding(3, 0)]]
#endif
SamplerState gLutSampler : register(s0); // unused since LutTrilinear; kept so every backend's binding layout stays
#ifdef VK_MODE
// LUT_ANY_FORMAT_TARGET: the post-NR stack grades the game's own image in place, whatever its format, so that
// variant declares the format unknown (the device must allow storage writes without one). Left out, dxc would declare
// rgba32f from the float4, which must then match the image's own format.
#ifdef LUT_ANY_FORMAT_TARGET
[[vk::binding(4, 0)]] [[vk::image_format("unknown")]]
#else
[[vk::binding(4, 0)]] [[vk::image_format("rgba16f")]]
#endif
#endif
RWTexture2D<float4> gTarget : register(u0);
#ifdef VK_MODE
[[vk::binding(5, 0)]]
#endif
Texture2D<float4> gExposureLive : register(t2);

// The live exposure texel, for dlssnr_whitepoint.hlsli; read only when gUseGameExposure or gUseExposureWhitePoint is set.
float ExposureSample() { return gExposureLive.Load(int3(0, 0, 0)).r; }

#include "dlssnr_whitepoint.hlsli"

static const uint kInputGamma22 = 3u;
static const uint kInputPq = 4u;
static const float kScrgbPerReferenceWhite = 203.0 / 80.0; // mirrors dlssnr.hlsl's own constant of the same name

// PQ -> this pass's reference-linear light, exactly as dlssnr.hlsl's PqToReferenceLinear reads it (ST 2084,
// BT.2020 -> BT.709, ITU-R BT.2408 reference white at 1.0), put on the game's HDR paper white (the encode's white point
// for forced PQ, 1.0 at the default 203 nits), then LinearToSrgb lands it in display 0-1.
float3 DecodeToDisplay01(float3 c, float white)
{
    if (inputEncoding == kInputPq)
        return LinearToSrgb(mul(kBt2020To709, DecodePQ(c)) / kScrgbPerReferenceWhite / white);

    if (colourIsLinearHdr != 0)
    {
        // Open-ended scene-linear light: divide by the encode's white point to land near 1.0, then Neutwo
        // compresses [0, inf) into [0, 1) with no hard clip -- the same reversible proxy curve
        // dlssnr_replace_curve.hlsli's own Encode pass uses, so a bright highlight fades toward white instead of
        // being clipped flat there -- and LinearToSrgb puts it in the gamma-encoded display 0-1 a .cube is made for.
        // Fixed (2026-10-31): the sRGB encode was missing, so a .cube read linear light as display values (mid grey
        // 0.18 instead of 0.46): creative LUTs graded a far too dark picture and their sRGB result was read back as
        // linear light.
        return LinearToSrgb(NeutwoEncode(max(c, 0.0) / white));
    }

    // Gamma 2.2, passthrough and tone-mapped-sRGB frames are already display-referred: a .cube LUT is
    // defined over exactly this domain, so there is nothing to convert, only to clamp before the sample.
    return saturate(c);
}

float3 EncodeFromDisplay01(float3 c, float white)
{
    if (inputEncoding == kInputPq)
        return EncodePQ(mul(kBt709To2020, SrgbToLinear(c) * white * kScrgbPerReferenceWhite));

    if (colourIsLinearHdr != 0)
        return NeutwoDecode(SrgbToLinear(c)) * white;

    return c;
}

// LatticeCoord 1, exactly as DlssNr_LutFile.h's LatticeFromDisplay: per channel, the Linear HDR light this display value
// stands for (sRGB decode, Neutwo inverse, clamped as NeutwoDecode), log2-spaced from kLatticeFloor to the clamp.
static const float kLatticeFloor = 1.0 / 1024.0;
static const float kLatticeMaxLight = 707.10614; // 0.999999 / sqrt(1 - 0.999999^2)

float3 LatticeFromDisplay(float3 d)
{
    const float3 m = min(SrgbToLinear(d), 0.999999);
    const float3 light = m * rsqrt(max(1.0 - m * m, 1e-8));
    const float lo = log2(kLatticeFloor);
    return saturate((log2(light + kLatticeFloor) - lo) / (log2(kLatticeMaxLight + kLatticeFloor) - lo));
}

// Trilinear between the 8 lattice points around uvw (0..1 over the lattice), with full float weights. The sampler's own
// filtering weighs with 8-bit fractions on most GPUs (and the lattice used to be half floats): fine for a display
// result, but on Linear HDR the result goes back through Neutwo's inverse, which near display 1 turns those steps into
// visible bands in bright gradients (RDR2 skin, 2026-11-01). Same result as DlssNr_LutFile.h SampleLut.
float3 LutTrilinear(float3 uvw)
{
    const int last = (int) max(lutSize, 2u) - 1;
    const float3 p = saturate(uvw) * (float) last;
    const int3 i0 = min((int3) floor(p), last - 1);
    const int3 i1 = i0 + 1;
    const float3 f = p - (float3) i0;

    const float3 c000 = gLut.Load(int4(i0.x, i0.y, i0.z, 0)).rgb;
    const float3 c100 = gLut.Load(int4(i1.x, i0.y, i0.z, 0)).rgb;
    const float3 c010 = gLut.Load(int4(i0.x, i1.y, i0.z, 0)).rgb;
    const float3 c110 = gLut.Load(int4(i1.x, i1.y, i0.z, 0)).rgb;
    const float3 c001 = gLut.Load(int4(i0.x, i0.y, i1.z, 0)).rgb;
    const float3 c101 = gLut.Load(int4(i1.x, i0.y, i1.z, 0)).rgb;
    const float3 c011 = gLut.Load(int4(i0.x, i1.y, i1.z, 0)).rgb;
    const float3 c111 = gLut.Load(int4(i1.x, i1.y, i1.z, 0)).rgb;

    const float3 c00 = lerp(c000, c100, f.x);
    const float3 c10 = lerp(c010, c110, f.x);
    const float3 c01 = lerp(c001, c101, f.x);
    const float3 c11 = lerp(c011, c111, f.x);
    return lerp(lerp(c00, c10, f.y), lerp(c01, c11, f.y), f.z);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;

    // Uniform across the dispatch: the host's value, or recomputed from the live exposure as the encode does.
    // WhitePoint() floors the host value at 1e-4, as this pass's own max(trim, 1e-4) did.
    const float white = WhitePoint();

    const float4 raw = gSource.Load(int3(id.xy, 0));
    const float3 display = DecodeToDisplay01(raw.rgb, white);

    const float3 domainMin = float3(domainMinR, domainMinG, domainMinB);
    const float3 domainMax = float3(domainMaxR, domainMaxG, domainMaxB);
    const float3 clamped = clamp(display, domainMin, domainMax);
    const float3 span = max(domainMax - domainMin, 1e-6);
    const float3 even = (clamped - domainMin) / span;
    const float3 uvw = latticeCoord == 1u ? LatticeFromDisplay(even) : even;

    const float3 graded = LutTrilinear(uvw);

    const float3 result = lerp(display, graded, saturate(strength));
    float3 encoded = EncodeFromDisplay01(result, white);

    // Linear HDR: Neutwo's inverse is unbounded and very steep near display 1, so a look that rolls a highlight off
    // toward white (AgX and other view transforms do, on purpose) came back first squeezed, then surging (3x over half a
    // stop), then up to ~700x the white point: contour bands and flat silver patches (RDR2 faces in a dark room,
    // 2026-11-01). Where the handed value or the look's result is near white (peak display 0.80-0.95, blended in), the
    // result goes back to light with the INPUT's own Neutwo scalar instead of its own: the encode's one scalar for all
    // channels, so the look's colour holds (a black-and-white look greys a red highlight) and the light stays bounded --
    // about the input's for a bright pixel, about the white point for a dark one -- with no clip. Exactly the input at
    // strength 0; midtones and shadows go through the inverse as before. The same holds for NR's Finished Picture stage
    // on an scRGB swapchain, where the picture is already tonemapped: the light handed back stays near the input's, so
    // nothing there is pushed past what the game showed.
    if (inputEncoding != kInputPq && colourIsLinearHdr != 0)
    {
        const float3 light = max(raw.rgb, 0.0) / white;
        const float m = max(light.r, max(light.g, light.b));
        const float back = m > 1e-6 ? m / Neutwo(m) : 1.0;
        const float nearWhite = max(max(display.r, max(display.g, display.b)), max(result.r, max(result.g, result.b)));
        encoded = lerp(encoded, SrgbToLinear(result) * (back * white), smoothstep(0.80, 0.95, nearWhite));
    }

    gTarget[id.xy] = float4(encoded, raw.a);
}
