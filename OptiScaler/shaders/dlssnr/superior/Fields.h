#pragma once
#include <cstddef>
#include <cstdint>
#include "Constants.h"
#include "../DlssNr_Common.h"

// Superior passes reuse constant-buffer fields named for other modes (the v59 shader ABI and the shared resolve's
// cbuffer). Every such reuse goes through one of these named setters, and the offsets the shaders read are pinned
// below, so a layout change fails to compile instead of silently moving a value into another field.
namespace DlssNr::Superior::Fields
{
// Final and inter-pass frequency shaping: the high and low band gains travel in MvScaleX / MvScaleY
// (HostResolve.hlsli: gMvScaleX * edit + (gMvScaleY - gMvScaleX) * low). Both constant structs.
template <class C> inline void SetBandGains(C& c, float high, float low)
{
    c.MvScaleX = high;
    c.MvScaleY = low;
}
template <class C> inline float HighGain(const C& c) { return c.MvScaleX; }
template <class C> inline float LowGain(const C& c) { return c.MvScaleY; }

// Host resolve flags (HostResolve.hlsli): GuideWidth != 0 binds the 1/8 low field at t5 for shaping; GuideHeight != 0
// means the model texture holds a raw NR answer (non-temporal), 0 a decoded signed DLSS residual.
template <class C> inline void SetResolveFlags(C& c, bool lowField, bool rawAnswer)
{
    c.GuideWidth = lowField ? 1u : 0u;
    c.GuideHeight = rawAnswer ? 1u : 0u;
}

// Inter-pass shaping (v59 mode 36): GuideWidth 1 = a low field is bound.
inline void SetInterPassLowField(DlssNrConstants& c, bool lowField) { c.GuideWidth = lowField ? 1u : 0u; }
inline bool InterPassLowField(const DlssNrConstants& c) { return c.GuideWidth != 0u; }

// v59 guide resize / carrier modes (10, 39): the depth and motion regions travel in the comparison fields.
//   depth:  size GuideWidth/GuideHeight, origin DebugView/CompareMode
//   motion: size TransferStrength/ColourStrength (as floats), origin CompareSwap/Transfer
inline void SetDepthRegion(DlssNrConstants& c, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    c.GuideWidth = w;
    c.GuideHeight = h;
    c.DebugView = x;
    c.CompareMode = y;
}
inline void SetMotionRegion(DlssNrConstants& c, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    c.TransferStrength = float(w);
    c.ColourStrength = float(h);
    c.CompareSwap = x;
    c.Transfer = y;
}

// The fields above, at the offsets both cbuffers (superior/precompile/dlssnr.hlsl and precompile/dlssnr.hlsl,
// "cbuffer Params") declare them. Changing either struct or cbuffer requires changing these and rebuilding blobs.
#define DLSSNR_SUPERIOR_PIN(Struct)                                                                                    \
    static_assert(offsetof(Struct, Mode) == 0 && offsetof(Struct, Width) == 8 && offsetof(Struct, Height) == 12);      \
    static_assert(offsetof(Struct, TransferStrength) == 16 && offsetof(Struct, ColourStrength) == 20);                 \
    static_assert(offsetof(Struct, DebugView) == 24 && offsetof(Struct, Passthrough) == 32);                           \
    static_assert(offsetof(Struct, MvScaleX) == 36 && offsetof(Struct, MvScaleY) == 40);                               \
    static_assert(offsetof(Struct, GuideWidth) == 44 && offsetof(Struct, GuideHeight) == 48);                          \
    static_assert(offsetof(Struct, CompareMode) == 52 && offsetof(Struct, CompareSwap) == 64);                         \
    static_assert(offsetof(Struct, Transfer) == 68);
DLSSNR_SUPERIOR_PIN(DlssNr::Superior::DlssNrConstants)
DLSSNR_SUPERIOR_PIN(::DlssNrConstants)
// The Superior blobs read the first 256 bytes of whatever buffer they are given; the live struct is longer (the depth
// fade) and keeps the same first 256 bytes.
static_assert(sizeof(DlssNr::Superior::DlssNrConstants) == 256 && sizeof(::DlssNrConstants) >= 256);
#undef DLSSNR_SUPERIOR_PIN
} // namespace DlssNr::Superior::Fields
