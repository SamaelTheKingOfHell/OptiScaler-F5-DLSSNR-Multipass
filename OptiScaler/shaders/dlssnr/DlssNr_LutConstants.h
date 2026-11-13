#pragma once

// Constants of precompile/dlssnr_lut.hlsl (Story 2 of the LUT-apply epic,
// dlssnr-lut-apply). Its own 256-byte slot in DlssNr_Dx12's dedicated
// _lutConstantBuffers ring -- the LUT pass has its own root signature and descriptor table, not the shared
// one DlssNrConstants/DlssNrDetailReuseConstants go through, so this does not need to match either's layout.
// Still one ordered list of 4-byte scalars matching the HLSL Params cbuffer field for field, append only,
// both sides changed together (DEVELOPMENT.md rule 5, applied to this pass's own constants).

#include <cstddef>
#include <cstdint>

struct alignas(256) DlssNrLutConstants
{
    uint32_t Width;
    uint32_t Height;
    float Strength;
    float DomainMinR, DomainMinG, DomainMinB;
    float DomainMaxR, DomainMaxG, DomainMaxB;
    uint32_t LutSize;
    uint32_t InputEncoding;
    // Fixed (Review Pass, 2026-10-04): InputEncoding alone cannot say this -- DlssNrColourEncoding's
    // ShaderConversion() collapses scene-linear HDR and tone-mapped sRGB into the same "else" value
    // (DlssNr_Common.h's own comment on ColourIsLinearHdr says so), so the shader was saturating raw,
    // open-ended linear light straight to 0-1 as if it were already display-referred. This carries the
    // real signal (frame.ColourIsLinearHdr) and Trim below carries what to divide it by first.
    uint32_t ColourIsLinearHdr;
    // What to divide linear HDR light by before compressing it into 0-1: the white point the encode will use for this
    // frame (ResolveWhitePoint, or the Vulkan path's resolved white point). It is the whole divisor, not the Trim on top
    // of it -- a game that scales its frame by its exposure sits near paper white 1000x, and dividing that by a Trim of ~1
    // would pin every pixel at the top of the curve. For PQ it is the game's HDR paper white (1.0 at 203 nits after the
    // PQ decode), which the PQ branch divides by too. Unused for tone-mapped frames.
    float Trim;
    // The encode's live white point inputs (dlssnr_whitepoint.hlsli): when the encode recomputes its white point from a
    // live exposure texture, the LUT pass reads the same texture with the same Trim table, so it grades for the white the
    // model is shown this frame. Trim above stays the host's value, the fallback. CopyLiveWhitePoint fills these.
    float PreExposure;
    uint32_t UseGameExposure;
    uint32_t UseExposureWhitePoint;
    float ExposureBaseScale;
    float ExposureTrim;
    uint32_t ExposureTrimPreview;
    uint32_t ExposureTrimAnchorCount;
    float ExposureTrimAnchors[16]; // key, trim pairs, as DlssNrConstants' ExposureTrimAnchorExposure0..Trim7
    uint32_t LatticeCoord; // DlssNr::Lut3D::latticeCoord: 0 even over display 0..1, 1 log-spaced Linear HDR light
};
static_assert(sizeof(DlssNrLutConstants) == 256);
// The HLSL Params cbuffer reads these by position; the live white point block must sit where dlssnr_lut.hlsl puts it.
static_assert(offsetof(DlssNrLutConstants, Trim) == 48);
static_assert(offsetof(DlssNrLutConstants, ExposureTrimAnchors) == 80);
static_assert(offsetof(DlssNrLutConstants, LatticeCoord) == 144);

// The encode's white point fields into the LUT pass's constants. Templated on the encode's constants type
// (DlssNrConstants) so this header stays free of it; its anchor table is eight key/trim float pairs in order.
template <typename EncodeConstants>
inline void CopyLiveWhitePoint(DlssNrLutConstants& lut, const EncodeConstants& encode)
{
    lut.Trim = encode.WhitePoint;
    lut.PreExposure = encode.PreExposure;
    lut.UseGameExposure = encode.UseGameExposure;
    lut.UseExposureWhitePoint = encode.UseExposureWhitePoint;
    lut.ExposureBaseScale = encode.ExposureBaseScale;
    lut.ExposureTrim = encode.ExposureTrim;
    lut.ExposureTrimPreview = encode.ExposureTrimPreview;
    lut.ExposureTrimAnchorCount = encode.ExposureTrimAnchorCount;
    const float* pairs = &encode.ExposureTrimAnchorExposure0;
    for (int i = 0; i < 16; ++i)
        lut.ExposureTrimAnchors[i] = pairs[i];
}
