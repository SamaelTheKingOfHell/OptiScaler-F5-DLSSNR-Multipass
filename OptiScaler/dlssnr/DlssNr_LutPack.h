#pragma once

// The LUT-apply epic's GPU-side texel packing, shared by the D3D12 and Vulkan uploads so the two cannot drift: a
// parsed lattice (DlssNr_LutFile.h's Lut3D::rgb, red fastest, then green, then blue) becomes RGBA16F texels with
// w = 1.0, in exactly a 3D texture's own x-fastest-then-y-then-z order. Only the row and slice pitch of the
// destination differ between the backends, so those are parameters. No D3D or Vulkan types here, so a host test
// can run it.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace DlssNrLutPack
{

// Round-to-zero float -> half, no dependency on DirectXPackedVector: LUT colour data never needs anything
// beyond half's mantissa, and a truncated rather than rounded mantissa is one bit at most, far under what
// the format can represent of an 8-or-more-bit source table. Flushes subnormal/overflow results to
// signed-zero/infinity rather than reproducing their bit patterns -- never relevant for 0..a few LUT values.
inline uint16_t FloatToHalf(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));

    const uint32_t sign = (bits >> 16) & 0x8000u;
    const int32_t exponent = (int32_t) ((bits >> 23) & 0xFFu) - 127 + 15;
    const uint32_t mantissa = bits & 0x7FFFFFu;

    if (exponent <= 0)
        return (uint16_t) sign;
    if (exponent >= 0x1F)
        return (uint16_t) (sign | 0x7C00u);
    return (uint16_t) (sign | ((uint32_t) exponent << 10) | (mantissa >> 13));
}

// Bytes of a tightly packed size^3 RGBA16F lattice (what a Vulkan staging buffer holds).
inline size_t TightBytes(int size) { return (size_t) size * (size_t) size * (size_t) size * 4u * sizeof(uint16_t); }

// Writes the lattice into `dst`: row y of slice z starts at z * slicePitch + y * rowPitch (both in bytes, rowPitch
// at least size * 8). `rgb` holds size^3 triples.
inline void PackHalf4(const float* rgb, int size, uint8_t* dst, size_t rowPitch, size_t slicePitch)
{
    const size_t sizeT = (size_t) size;

    for (int z = 0; z < size; ++z)
    {
        uint8_t* const slice = dst + slicePitch * (size_t) z;
        for (int y = 0; y < size; ++y)
        {
            uint16_t* const row = reinterpret_cast<uint16_t*>(slice + rowPitch * (size_t) y);
            for (int x = 0; x < size; ++x)
            {
                const size_t i = ((size_t) z * sizeT + (size_t) y) * sizeT + (size_t) x;
                row[x * 4 + 0] = FloatToHalf(rgb[i * 3 + 0]);
                row[x * 4 + 1] = FloatToHalf(rgb[i * 3 + 1]);
                row[x * 4 + 2] = FloatToHalf(rgb[i * 3 + 2]);
                row[x * 4 + 3] = FloatToHalf(1.0f);
            }
        }
    }
}

// The lattice as RGBA32F texels (w = 1), the format the GPU pass reads since 2026-11-01: on Linear HDR its result is
// turned back into light through Neutwo's inverse, which near display 1 magnifies half's 1/2048 steps into visible
// bands. Same order and pitches as PackHalf4, 16 bytes a texel.
inline size_t TightBytesFloat4(int size) { return (size_t) size * (size_t) size * (size_t) size * 4u * sizeof(float); }

inline void PackFloat4(const float* rgb, int size, uint8_t* dst, size_t rowPitch, size_t slicePitch)
{
    const size_t sizeT = (size_t) size;

    for (int z = 0; z < size; ++z)
    {
        uint8_t* const slice = dst + slicePitch * (size_t) z;
        for (int y = 0; y < size; ++y)
        {
            float* const row = reinterpret_cast<float*>(slice + rowPitch * (size_t) y);
            for (int x = 0; x < size; ++x)
            {
                const size_t i = ((size_t) z * sizeT + (size_t) y) * sizeT + (size_t) x;
                row[x * 4 + 0] = rgb[i * 3 + 0];
                row[x * 4 + 1] = rgb[i * 3 + 1];
                row[x * 4 + 2] = rgb[i * 3 + 2];
                row[x * 4 + 3] = 1.0f;
            }
        }
    }
}

} // namespace DlssNrLutPack
