// Host check of DlssNr_LutPack.h: the half-float packing of a LUT lattice that both the D3D12 and the Vulkan upload
// share. No GPU and no game.
// cl /std:c++20 /EHsc /W4 tests/nr_lut_pack_smoke.cpp
#include "../OptiScaler/dlssnr/DlssNr_LutPack.h"

#include <cstdio>
#include <vector>

static int fails = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                                                \
            ++fails;                                                                                                   \
        }                                                                                                              \
    } while (0)

int main()
{
    using namespace DlssNrLutPack;

    // The half encodings the shader relies on.
    CHECK(FloatToHalf(0.0f) == 0x0000);
    CHECK(FloatToHalf(1.0f) == 0x3C00);
    CHECK(FloatToHalf(0.5f) == 0x3800);
    CHECK(FloatToHalf(2.0f) == 0x4000);
    CHECK(FloatToHalf(-1.0f) == 0xBC00);
    CHECK(FloatToHalf(1e-9f) == 0x0000); // subnormal range flushes to zero
    CHECK(FloatToHalf(1e9f) == 0x7C00);  // overflow goes to infinity

    // A 2x2x2 lattice whose every value is distinct and exactly representable: red fastest, then green, then blue.
    constexpr int size = 2;
    std::vector<float> rgb;
    for (int z = 0; z < size; ++z)
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
            {
                const float index = (float) ((z * size + y) * size + x);
                rgb.push_back(index);
                rgb.push_back(index + 0.5f);
                rgb.push_back(index + 0.25f);
            }

    // Tightly packed (the Vulkan staging buffer).
    {
        std::vector<uint8_t> out(TightBytes(size), 0xCD);
        CHECK(out.size() == 2 * 2 * 2 * 8);
        PackHalf4(rgb.data(), size, out.data(), size * 8, size * 8 * size);

        const uint16_t* texel = reinterpret_cast<const uint16_t*>(out.data());
        for (int i = 0; i < size * size * size; ++i)
        {
            CHECK(texel[i * 4 + 0] == FloatToHalf(rgb[i * 3 + 0]));
            CHECK(texel[i * 4 + 1] == FloatToHalf(rgb[i * 3 + 1]));
            CHECK(texel[i * 4 + 2] == FloatToHalf(rgb[i * 3 + 2]));
            CHECK(texel[i * 4 + 3] == 0x3C00); // w = 1.0
        }
    }

    // A padded destination (D3D12's 256-byte row pitch): the padding is left alone and each row lands at its pitch.
    {
        constexpr size_t rowPitch = 256;
        constexpr size_t slicePitch = rowPitch * size;
        std::vector<uint8_t> out(slicePitch * size, 0xCD);
        PackHalf4(rgb.data(), size, out.data(), rowPitch, slicePitch);

        for (int z = 0; z < size; ++z)
            for (int y = 0; y < size; ++y)
            {
                const uint16_t* row = reinterpret_cast<const uint16_t*>(out.data() + z * slicePitch + y * rowPitch);
                for (int x = 0; x < size; ++x)
                {
                    const int i = (z * size + y) * size + x;
                    CHECK(row[x * 4 + 0] == FloatToHalf(rgb[i * 3 + 0]));
                    CHECK(row[x * 4 + 3] == 0x3C00);
                }

                CHECK(out[z * slicePitch + y * rowPitch + size * 8] == 0xCD);
            }
    }

    // Float texels (what every backend uploads since 2026-11-01), padded as D3D12 pads them.
    {
        CHECK(TightBytesFloat4(size) == 2 * 2 * 2 * 16);
        constexpr size_t rowPitch = 256;
        constexpr size_t slicePitch = rowPitch * size;
        std::vector<uint8_t> out(slicePitch * size, 0xCD);
        PackFloat4(rgb.data(), size, out.data(), rowPitch, slicePitch);

        for (int z = 0; z < size; ++z)
            for (int y = 0; y < size; ++y)
            {
                const float* row = reinterpret_cast<const float*>(out.data() + z * slicePitch + y * rowPitch);
                for (int x = 0; x < size; ++x)
                {
                    const int i = (z * size + y) * size + x;
                    CHECK(row[x * 4 + 0] == rgb[i * 3 + 0] && row[x * 4 + 1] == rgb[i * 3 + 1] &&
                          row[x * 4 + 2] == rgb[i * 3 + 2] && row[x * 4 + 3] == 1.0f);
                }

                CHECK(out[z * slicePitch + y * rowPitch + size * 16] == 0xCD);
            }
    }

    if (fails == 0)
        printf("nr_lut_pack_smoke: ok\n");

    return fails == 0 ? 0 : 1;
}
