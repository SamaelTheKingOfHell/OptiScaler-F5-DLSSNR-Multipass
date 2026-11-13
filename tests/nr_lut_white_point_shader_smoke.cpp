// Headless shader test: Windows D3D11 WARP runs dlssnr_lut.hlsl, the LUT grade NR's input goes through, and checks the
// white point it grades for: the encode's own, recomputed this frame from the live exposure texture.
//
//   1. Tone-mapped sRGB and gamma 2.2 frames get the exact grade (full float interpolation since 2026-11-01), PQ the
//      shader before the white point became shared (dlssnr_lut.hlsl at 7364a7f8) within its 8-bit filter weights,
//      with no live exposure. Linear HDR matches GradeLinearHdr (the CPU model: below the knee the inverse of the
//      handed display, above it the look's display-linear ratio on the input light, capped). It grades in the sRGB
//      display domain a .cube is made for (sRGB of Neutwo of light / white; fixed 2026-10-31, the reference graded
//      the Neutwo values as they were), checked against a CPU model of that grade at several white points, and
//      linear mid grey 0.18 lands on display 0.46.
//      With LatticeCoord 1 the lattice is looked up at the CPU's LatticeFromDisplay (DlssNr_LutFile.h) of that value.
//   2. With a live exposure texture (UseGameExposure, or UseExposureWhitePoint for Automatic's) the pass grades for
//      PreExposure / exposure x the Trim -- the encode's own rule (dlssnr_whitepoint.hlsli) -- and follows the texel
//      when it changes between dispatches, instead of the host value it was handed.
//   3. The Trim table applies at the live base white point, as it does in the encode.
//   4. An implausible texel (0, inf) falls back to the host value.
//   5. Forced PQ grades for the game's HDR paper white: white point 2 reads the decoded light at half, so the result
//      matches a frame of half the light graded at white 1.
//   6. The precompiled D3D11 shader F5LUT's D3D11 chain step ships (dlssnr_lut_Shader_Dx11.h) grades bit-identically to
//      this compile of dlssnr_lut.hlsl, with the chain step's constants (F5Lut_Common.h: no encoding, the HDR flag,
//      white 1.0 or PreExposure / the game's exposure), so a stale header shows here.
//   7. Linear HDR highlights a look sends to display white (AgX does) come back at hypot(input, white), not ~700x white
//      (2026-11-01, RDR2's silver faces: Neutwo's inverse at display 1).
//   8. A black-and-white look greys saturated highlights (no per-channel ratio keeping them coloured).
//   9. A brightening look keeps a grey ramp rising (no plateau) and matches the CPU model.
//  10. Strength 0 is the identity on Linear HDR.
//
// Build: cl /nologo /std:c++20 /EHsc /W3 tests\nr_lut_white_point_shader_smoke.cpp d3d11.lib d3dcompiler.lib
// Reference: git show 7364a7f8:OptiScaler/shaders/dlssnr/precompile/dlssnr_lut.hlsl > precompile\dlssnr_lut_ref.hlsl
//            (next to dlssnr_pq.hlsli and dlssnr_replace_curve.hlsli, which it includes)
// Run:   nr_lut_white_point_shader_smoke.exe <dlssnr_lut.hlsl> <its reference>
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include "../OptiScaler/dlssnr/DlssNr_LutFile.h"
#include "../OptiScaler/shaders/dlssnr/DlssNr_LutConstants.h"
#include "../OptiScaler/shaders/dlssnr/precompile/dlssnr_lut_Shader_Dx11.h"
using Microsoft::WRL::ComPtr;

struct Pixel { float r, g, b, a; };
static void check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D call failed"); }
static int fails = 0;
static void expect(bool ok, const std::string& label)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", label.c_str());
        ++fails;
    }
}

constexpr int N = 8;
constexpr int kLut = 4;

struct Gpu
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Texture2D> sourceTex, exposureTex, target, readback;
    ComPtr<ID3D11ShaderResourceView> sourceSrv, lutSrv, whiteLutSrv, bwLutSrv, brightLutSrv, exposureSrv;

    ComPtr<ID3D11ShaderResourceView> Cube(const D3D11_TEXTURE3D_DESC& desc, const std::vector<float>& texels)
    {
        D3D11_SUBRESOURCE_DATA data { texels.data(), kLut * 16, kLut * kLut * 16 };
        ComPtr<ID3D11Texture3D> tex;
        check(device->CreateTexture3D(&desc, &data, &tex));
        ComPtr<ID3D11ShaderResourceView> srv;
        check(device->CreateShaderResourceView(tex.Get(), nullptr, &srv));
        return srv;
    }
    ComPtr<ID3D11UnorderedAccessView> targetUav;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;

    Gpu()
    {
        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                nullptr, &ctx));
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = N; desc.Height = 1; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        check(device->CreateTexture2D(&desc, nullptr, &sourceTex));
        check(device->CreateShaderResourceView(sourceTex.Get(), nullptr, &sourceSrv));
        desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        check(device->CreateTexture2D(&desc, nullptr, &target));
        check(device->CreateUnorderedAccessView(target.Get(), nullptr, &targetUav));
        desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check(device->CreateTexture2D(&desc, nullptr, &readback));

        D3D11_TEXTURE2D_DESC one {};
        one.Width = 1; one.Height = 1; one.MipLevels = 1; one.ArraySize = 1; one.Format = DXGI_FORMAT_R32_FLOAT;
        one.SampleDesc.Count = 1; one.Usage = D3D11_USAGE_DEFAULT; one.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        check(device->CreateTexture2D(&one, nullptr, &exposureTex));
        check(device->CreateShaderResourceView(exposureTex.Get(), nullptr, &exposureSrv));

        // A graded cube: each lattice point (r, g, b) -> (g, b, r) with a lift. Trilinear between lattice points is
        // linear, so the grade is exact everywhere and depends on where the white point put each pixel.
        std::vector<float> lattice;
        for (int b = 0; b < kLut; ++b)
            for (int g = 0; g < kLut; ++g)
                for (int r = 0; r < kLut; ++r)
                {
                    const float R = r / float(kLut - 1), G = g / float(kLut - 1), B = b / float(kLut - 1);
                    lattice.insert(lattice.end(), { 0.9f * G + 0.05f, 0.9f * B + 0.05f, 0.9f * R + 0.05f, 1.0f });
                }
        D3D11_TEXTURE3D_DESC cube {};
        cube.Width = cube.Height = cube.Depth = kLut; cube.MipLevels = 1; cube.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        cube.Usage = D3D11_USAGE_DEFAULT; cube.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA cubeData { lattice.data(), kLut * 16, kLut * kLut * 16 };
        ComPtr<ID3D11Texture3D> cubeTex;
        check(device->CreateTexture3D(&cube, &cubeData, &cubeTex));
        check(device->CreateShaderResourceView(cubeTex.Get(), nullptr, &lutSrv));

        // A look that sends every colour to display white, as a view transform does with a bright enough highlight.
        std::vector<float> white(lattice.size(), 1.0f);
        whiteLutSrv = Cube(cube, white);

        // Black and white (display luma: linear in the lattice, so trilinear is exact) and a brightening look (display
        // x1.6, clipped at 1).
        std::vector<float> bw, bright;
        for (int b = 0; b < kLut; ++b)
            for (int g = 0; g < kLut; ++g)
                for (int r = 0; r < kLut; ++r)
                {
                    const float R = r / float(kLut - 1), G = g / float(kLut - 1), B = b / float(kLut - 1);
                    const float y = 0.2126f * R + 0.7152f * G + 0.0722f * B;
                    bw.insert(bw.end(), { y, y, y, 1.0f });
                    bright.insert(bright.end(), { std::min(1.6f * R, 1.0f), std::min(1.6f * G, 1.0f),
                                                  std::min(1.6f * B, 1.0f), 1.0f });
                }
        bwLutSrv = Cube(cube, bw);
        brightLutSrv = Cube(cube, bright);

        D3D11_BUFFER_DESC buffer {};
        buffer.ByteWidth = sizeof(DlssNrLutConstants);
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        check(device->CreateBuffer(&buffer, nullptr, &constants));
        D3D11_SAMPLER_DESC sampling {};
        sampling.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampling.AddressU = sampling.AddressV = sampling.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampling.MaxLOD = D3D11_FLOAT32_MAX;
        check(device->CreateSamplerState(&sampling, &sampler));
    }

    ComPtr<ID3D11ComputeShader> Compile(const wchar_t* path)
    {
        ComPtr<ID3DBlob> code, errors;
        HRESULT hr = D3DCompileFromFile(path, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "CSMain", "cs_5_0",
                                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (errors && FAILED(hr)) std::fprintf(stderr, "%s", (char*) errors->GetBufferPointer());
        check(hr);
        ComPtr<ID3D11ComputeShader> shader;
        check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader));
        return shader;
    }

    std::array<Pixel, N> Run(ID3D11ComputeShader* shader, const std::array<Pixel, N>& in, const DlssNrLutConstants& c,
                             float exposureTexel, ID3D11ShaderResourceView* lut = nullptr)
    {
        ctx->UpdateSubresource(sourceTex.Get(), 0, nullptr, in.data(), sizeof(in), 0);
        ctx->UpdateSubresource(exposureTex.Get(), 0, nullptr, &exposureTexel, sizeof(float), 0);
        ctx->UpdateSubresource(constants.Get(), 0, nullptr, &c, 0, 0);
        ID3D11ShaderResourceView* srvs[] = { sourceSrv.Get(), lut != nullptr ? lut : lutSrv.Get(), exposureSrv.Get() };
        ctx->CSSetShader(shader, nullptr, 0);
        ctx->CSSetShaderResources(0, 3, srvs);
        ctx->CSSetUnorderedAccessViews(0, 1, targetUav.GetAddressOf(), nullptr);
        ctx->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
        ctx->CSSetSamplers(0, 1, sampler.GetAddressOf());
        ctx->Dispatch(1, 1, 1);
        ctx->CopyResource(readback.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped {};
        check(ctx->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        std::array<Pixel, N> out;
        std::memcpy(out.data(), mapped.pData, sizeof(out));
        ctx->Unmap(readback.Get(), 0);
        return out;
    }
};

static bool SameBits(const std::array<Pixel, N>& a, const std::array<Pixel, N>& b)
{
    return std::memcmp(a.data(), b.data(), sizeof(a)) == 0;
}

static float Worst(const std::array<Pixel, N>& a, const std::array<Pixel, N>& b)
{
    float worst = 0.0f;
    for (int i = 0; i < N; ++i)
    {
        const float scale = std::max({ std::abs(b[i].r), std::abs(b[i].g), std::abs(b[i].b), 1e-3f });
        worst = std::max({ worst, std::abs(a[i].r - b[i].r) / scale, std::abs(a[i].g - b[i].g) / scale,
                           std::abs(a[i].b - b[i].b) / scale });
    }
    return worst;
}

// The shader's Linear HDR decode on the CPU: sRGB of Neutwo (peak channel, hue kept) of light / white.
static float Neutwo(float x) { return x / std::sqrt(x * x + 1.0f); }
static float SrgbOf(float v)
{
    v = std::clamp(v, 0.0f, 1.0f);
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}
static void ToDisplay(const float in[3], float white, float out[3])
{
    float v[3], m = 0.0f;
    for (int c = 0; c < 3; ++c)
        m = std::max(m, v[c] = std::max(in[c], 0.0f) / white);
    const float scale = m <= 1e-6f ? 1.0f : Neutwo(m) / m;
    for (int c = 0; c < 3; ++c)
        out[c] = SrgbOf(v[c] * scale);
}

static float LinearOf(float v)
{
    v = std::clamp(v, 0.0f, 1.0f);
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

// The pass's whole Linear HDR grade on the CPU: handed display, the look (mixed by `strength`), back to light through
// Neutwo's inverse, blended (peak display of the handed value or the result 0.80-0.95) to the result's display-linear
// value times the INPUT's own Neutwo scalar.
template <typename Look>
static void GradeLinearHdr(const float in[3], float white, const Look& look, float out[3], float strength = 1.0f)
{
    float d[3], g[3], y[3];
    ToDisplay(in, white, d);
    look(d, g);
    float ym = 0.0f, nearWhite = 0.0f, inPeak = 0.0f;
    for (int c = 0; c < 3; ++c)
    {
        g[c] = d[c] + (g[c] - d[c]) * strength;
        y[c] = LinearOf(g[c]);
        ym = std::max(ym, y[c]);
        nearWhite = std::max({ nearWhite, d[c], g[c] });
        inPeak = std::max(inPeak, std::max(in[c], 0.0f) / white);
    }
    ym = std::min(ym, 0.999999f);
    const float x = ym / std::sqrt(std::max(1.0f - ym * ym, 1e-8f));
    const float t = std::clamp((nearWhite - 0.80f) / 0.15f, 0.0f, 1.0f);
    const float knee = t * t * (3.0f - 2.0f * t);
    const float back = inPeak > 1e-6f ? std::sqrt(inPeak * inPeak + 1.0f) : 1.0f; // m / Neutwo(m)
    for (int c = 0; c < 3; ++c)
    {
        const float inverse = (ym > 1e-6f ? y[c] * (x / ym) : y[c]) * white;
        out[c] = inverse + (y[c] * back * white - inverse) * knee;
    }
}

static DlssNrLutConstants Base(uint32_t encoding, bool linearHdr, float white)
{
    DlssNrLutConstants c {};
    c.Width = N; c.Height = 1; c.Strength = 1.0f;
    c.DomainMinR = c.DomainMinG = c.DomainMinB = 0.0f;
    c.DomainMaxR = c.DomainMaxG = c.DomainMaxB = 1.0f;
    c.LutSize = kLut;
    c.InputEncoding = encoding;
    c.ColourIsLinearHdr = linearHdr ? 1u : 0u;
    c.Trim = white;
    c.ExposureTrim = 1.0f;
    return c;
}

int wmain(int argc, wchar_t** argv)
try
{
    if (argc != 3) throw std::runtime_error("Pass dlssnr_lut.hlsl and its reference");
    Gpu gpu;
    const auto shader = gpu.Compile(argv[1]);
    const auto reference = gpu.Compile(argv[2]);

    const std::array<Pixel, N> linear {{ {0.0f, 0.0f, 0.0f, 1}, {0.02f, 0.03f, 0.05f, 1}, {0.18f, 0.18f, 0.18f, 1},
                                         {0.6f, 0.4f, 0.2f, 1}, {1.0f, 1.0f, 1.0f, 1}, {3.0f, 1.0f, 0.2f, 1},
                                         {12.0f, 12.0f, 12.0f, 0.5f}, {250.0f, 300.0f, 120.0f, 1} }};
    const std::array<Pixel, N> display {{ {0.0f, 0.0f, 0.0f, 1}, {0.1f, 0.2f, 0.3f, 1}, {0.46f, 0.46f, 0.46f, 1},
                                          {0.8f, 0.3f, 0.1f, 1}, {1.0f, 1.0f, 1.0f, 1}, {0.05f, 0.6f, 0.9f, 1},
                                          {0.33f, 0.66f, 0.99f, 0.5f}, {0.7f, 0.7f, 0.2f, 1} }};
    const std::array<Pixel, N> pq {{ {0.0f, 0.0f, 0.0f, 1}, {0.3f, 0.3f, 0.3f, 1}, {0.58f, 0.58f, 0.58f, 1},
                                     {0.65f, 0.5f, 0.4f, 1}, {0.75f, 0.75f, 0.75f, 1}, {0.4f, 0.55f, 0.7f, 1},
                                     {0.5f, 0.62f, 0.3f, 1}, {0.2f, 0.25f, 0.45f, 1} }};

    // 1. Linear HDR grades in the sRGB display domain: compared where the LUT works (display 0-1), as the GPU's
    // trilinear weights are 8-bit; the rest is bit-identical to the reference with no live exposure.
    for (float white : { 0.01f, 1.0f, 2.5375f, 1700.0f })
    {
        const auto got = gpu.Run(shader.Get(), linear, Base(0, true, white), 0.0f);
        const auto lift = [](const float d[3], float g[3])
        {
            g[0] = 0.9f * d[1] + 0.05f;
            g[1] = 0.9f * d[2] + 0.05f;
            g[2] = 0.9f * d[0] + 0.05f;
        };
        float off = 0.0f;
        for (int i = 0; i < N; ++i)
        {
            const float in[3] = { linear[i].r, linear[i].g, linear[i].b };
            float want[3];
            GradeLinearHdr(in, white, lift, want);
            const float scale = std::max({ want[0], want[1], want[2], 1e-3f * white });
            off = std::max({ off, std::abs(got[i].r - want[0]) / scale, std::abs(got[i].g - want[1]) / scale,
                             std::abs(got[i].b - want[2]) / scale });
            expect(got[i].a == linear[i].a, "linear HDR alpha changed");
        }
        std::printf("linear HDR at white %g: worst error vs the CPU grade %.2e\n", white, off);
        expect(off < 1e-3f, "linear HDR does not grade as the CPU model at white " + std::to_string(white));
    }
    {
        // mid grey: the cube's red is 0.9 x the display green + 0.05, so display = (red - 0.05) / 0.9
        const auto got = gpu.Run(shader.Get(), linear, Base(0, true, 1.0f), 0.0f);
        const float out[3] = { got[2].r, got[2].g, got[2].b };
        float back[3];
        ToDisplay(out, 1.0f, back);
        const float grey = (back[0] - 0.05f) / 0.9f;
        std::printf("linear mid grey 0.18 graded at display %.3f\n", grey);
        expect(std::abs(grey - 0.46f) < 0.01f, "linear mid grey does not reach the LUT as display 0.46");
    }
    {
        // LatticeCoord 1 (a post-NR stack converting Linear HDR for a log look): the lattice is looked up at
        // DlssNr_LutFile.h's LatticeFromDisplay of the display value, the same function the bake lays it out by.
        auto c = Base(0, true, 2.0f);
        c.LatticeCoord = 1u;
        const auto got = gpu.Run(shader.Get(), linear, c, 0.0f);
        const auto logLift = [](const float d[3], float g[3])
        {
            float u[3];
            for (int ch = 0; ch < 3; ++ch)
                u[ch] = DlssNr::LatticeFromDisplay(d[ch]);
            g[0] = 0.9f * u[1] + 0.05f;
            g[1] = 0.9f * u[2] + 0.05f;
            g[2] = 0.9f * u[0] + 0.05f;
        };
        float off = 0.0f;
        for (int i = 0; i < N; ++i)
        {
            const float in[3] = { linear[i].r, linear[i].g, linear[i].b };
            float want[3];
            GradeLinearHdr(in, 2.0f, logLift, want);
            const float scale = std::max({ want[0], want[1], want[2], 2e-3f });
            off = std::max({ off, std::abs(got[i].r - want[0]) / scale, std::abs(got[i].g - want[1]) / scale,
                             std::abs(got[i].b - want[2]) / scale });
        }
        std::printf("log-spaced lattice (LatticeCoord 1): worst error vs the CPU grade %.2e\n", off);
        expect(off < 1e-2f, "the shader's LatticeFromDisplay does not match the CPU's");
    }
    // Display-referred frames: the exact grade (the cube is linear, so trilinear is exact), at full float precision since
    // the shader interpolates itself (2026-11-01; the sampler's 8-bit weights were ~1e-3 off). PQ against the reference
    // within those old 8-bit weights.
    for (uint32_t encoding : { 0u, 3u })
    {
        const auto got = gpu.Run(shader.Get(), display, Base(encoding, false, 1.0f), 0.0f);
        float off = 0.0f;
        for (int i = 0; i < N; ++i)
        {
            const float want[3] = { 0.9f * display[i].g + 0.05f, 0.9f * display[i].b + 0.05f,
                                    0.9f * display[i].r + 0.05f };
            off = std::max({ off, std::abs(got[i].r - want[0]), std::abs(got[i].g - want[1]),
                             std::abs(got[i].b - want[2]) });
            expect(got[i].a == display[i].a, "display alpha changed");
        }
        std::printf("%s: worst error vs the exact grade %.2e\n", encoding == 3u ? "gamma 2.2" : "tone-mapped sRGB", off);
        expect(off < 1e-5f, encoding == 3u ? "gamma 2.2 is not the exact grade" : "tone-mapped sRGB is not the exact grade");
    }
    {
        const float off = Worst(gpu.Run(shader.Get(), pq, Base(4, true, 1.0f), 0.0f),
                                gpu.Run(reference.Get(), pq, Base(4, true, 1.0f), 0.0f));
        std::printf("PQ at the default paper white vs the reference: %.2e\n", off);
        expect(off < 2e-3f, "PQ at the default paper white differs from the reference");
    }

    // 2. Live exposure: PreExposure / exposure x Trim, following the texel; the host value is ignored.
    {
        for (uint32_t automatic : { 0u, 1u })
            for (float exposure : { 0.5f, 0.01f, 0.004f })
            {
                auto live = Base(0, true, 77.0f); // a stale host value, never the answer here
                live.PreExposure = 2.0f;
                live.ExposureTrim = 1.5f;
                live.UseGameExposure = automatic ? 0u : 1u;
                live.UseExposureWhitePoint = automatic;
                const float want = 2.0f / exposure * 1.5f;
                const auto got = gpu.Run(shader.Get(), linear, live, exposure);
                const auto host = gpu.Run(shader.Get(), linear, Base(0, true, want), 0.0f);
                const float off = Worst(got, host);
                std::printf("live %s exposure %.3f: white %.1f, off %.2e\n", automatic ? "automatic" : "game",
                            exposure, want, off);
                expect(off < 1e-5f, "live white point is not PreExposure / exposure x Trim");
            }
        // Following the game: ExposureBaseScale multiplies the base.
        auto follow = Base(0, true, 77.0f);
        follow.PreExposure = 1.0f;
        follow.ExposureTrim = 1.0f;
        follow.UseExposureWhitePoint = 1u;
        follow.ExposureBaseScale = 4.0f;
        expect(Worst(gpu.Run(shader.Get(), linear, follow, 0.5f), gpu.Run(shader.Get(), linear, Base(0, true, 8.0f), 0.0f)) <
                   1e-5f,
               "ExposureBaseScale is not applied to the live base");
    }

    // 3. Trim table at the live base: two points (base 1 -> trim 1, base 100 -> trim 4); base 10 is halfway in log.
    {
        auto live = Base(0, true, 77.0f);
        live.PreExposure = 1.0f;
        live.UseGameExposure = 1u;
        live.ExposureTrimAnchorCount = 2;
        live.ExposureTrimAnchors[0] = 1.0f; live.ExposureTrimAnchors[1] = 1.0f;
        live.ExposureTrimAnchors[2] = 100.0f; live.ExposureTrimAnchors[3] = 4.0f;
        const float off = Worst(gpu.Run(shader.Get(), linear, live, 0.1f), gpu.Run(shader.Get(), linear, Base(0, true, 20.0f), 0.0f));
        std::printf("trim table at live base 10: off %.2e\n", off);
        expect(off < 1e-4f, "the Trim table is not applied at the live base white point");
    }

    // 4. Implausible texels fall back to the host value.
    for (float bad : { 0.0f, INFINITY, -1.0f })
    {
        auto live = Base(0, true, 3.0f);
        live.PreExposure = 1.0f;
        live.UseGameExposure = 1u;
        expect(SameBits(gpu.Run(shader.Get(), linear, live, bad), gpu.Run(shader.Get(), linear, Base(0, true, 3.0f), 0.0f)),
               "an implausible exposure texel did not fall back to the host value");
    }

    // 5. Forced PQ at paper white 2x the reference: grades the light at half, so it equals a frame with half the light
    // at white 1 (EncodePQ/DecodePQ of the same light; the round trip is compared in decoded light, to rounding).
    {
        const auto atTwo = gpu.Run(shader.Get(), pq, Base(4, true, 2.0f), 0.0f);
        const auto atOne = gpu.Run(shader.Get(), pq, Base(4, true, 1.0f), 0.0f);
        float moved = 0.0f;
        for (int i = 1; i < N; ++i)
            moved = std::max(moved, std::abs(atTwo[i].r - atOne[i].r));
        std::printf("PQ paper white 406 vs 203 nits: largest change %.3f of the signal\n", moved);
        expect(moved > 0.01f, "forced PQ ignores the paper white");
        for (int i = 0; i < N; ++i)
            expect(std::isfinite(atTwo[i].r) && std::isfinite(atTwo[i].g) && std::isfinite(atTwo[i].b),
                   "forced PQ at paper white 2 is not finite");
    }

    // 7. A look that sends everything to display white, on Linear HDR: the light handed back is the input's own Neutwo
    // scale, hypot(input peak, white) -- about the input for a bright pixel, about white for a dark one -- where
    // Neutwo's inverse at display 1 alone gives ~700x white (the reference shader shows it).
    for (float white : { 0.06f, 1.0f, 18.5f })
    {
        const auto got = gpu.Run(shader.Get(), linear, Base(0, true, white), 0.0f, gpu.whiteLutSrv.Get());
        const auto old = gpu.Run(reference.Get(), linear, Base(0, true, white), 0.0f, gpu.whiteLutSrv.Get());
        float worstGain = 0.0f, oldWorst = 0.0f;
        for (int i = 0; i < N; ++i)
        {
            const float in = std::max({ linear[i].r, linear[i].g, linear[i].b, 0.0f });
            const float bound = std::sqrt(in * in + white * white);
            worstGain = std::max(worstGain, std::max({ got[i].r, got[i].g, got[i].b }) / bound);
            oldWorst = std::max(oldWorst, std::max({ old[i].r, old[i].g, old[i].b }) / bound);
            expect(std::isfinite(got[i].r) && std::isfinite(got[i].g) && std::isfinite(got[i].b),
                   "a look to white is not finite");
        }
        std::printf("look to white at white %g: worst light / hypot(in, white) %.4f (reference shader %.1f)\n", white,
                    worstGain, oldWorst);
        expect(worstGain <= 1.0001f, "a look to white hands back more light than hypot(in, white) at white " +
                                         std::to_string(white));
    }

    // 8. A black-and-white look on saturated highlights (Review Pass 2026-11-01: a per-channel ratio left a red highlight
    // red): every channel comes back equal, and as bright as the model says.
    {
        const std::array<Pixel, N> red {{ {1.5f, 0, 0, 1}, {4.0f, 0, 0, 1}, {30.0f, 0, 0, 1}, {0, 6.0f, 0, 1},
                                          {0, 0, 12.0f, 1}, {8.0f, 0.1f, 0, 1}, {0.3f, 0, 0, 1}, {2.0f, 2.0f, 0, 1} }};
        const auto got = gpu.Run(shader.Get(), red, Base(0, true, 1.0f), 0.0f, gpu.bwLutSrv.Get());
        float spread = 0.0f;
        for (int i = 0; i < N; ++i)
        {
            const float hi = std::max({ got[i].r, got[i].g, got[i].b }), lo = std::min({ got[i].r, got[i].g, got[i].b });
            spread = std::max(spread, (hi - lo) / std::max(hi, 1e-6f));
            expect(hi > 0.0f, "a black-and-white look blacks out a saturated highlight");
        }
        std::printf("black-and-white on saturated highlights: worst channel spread %.2e\n", spread);
        expect(spread < 1e-4f, "a black-and-white look leaves a saturated highlight coloured");
    }

    // 9. A brightening look (display x1.6) on a grey ramp from 0.05x to 40x white: the output keeps rising (no plateau
    // where a cap or the look's clip at 1 would flatten it) and matches the model.
    {
        const std::array<Pixel, N> ramp {{ {0.05f, 0.05f, 0.05f, 1}, {0.2f, 0.2f, 0.2f, 1}, {0.45f, 0.45f, 0.45f, 1},
                                           {0.7f, 0.7f, 0.7f, 1}, {1.0f, 1.0f, 1.0f, 1}, {2.0f, 2.0f, 2.0f, 1},
                                           {6.0f, 6.0f, 6.0f, 1}, {40.0f, 40.0f, 40.0f, 1} }};
        const auto got = gpu.Run(shader.Get(), ramp, Base(0, true, 1.0f), 0.0f, gpu.brightLutSrv.Get());
        // Each channel depends on its own only, so the lattice's trilinear is a 1D interpolation of its kLut samples.
        const auto bright = [](const float d[3], float g[3])
        {
            for (int c = 0; c < 3; ++c)
            {
                const float p = std::clamp(d[c], 0.0f, 1.0f) * (kLut - 1);
                const int i0 = std::min((int) p, kLut - 2);
                const float f = p - (float) i0;
                const float a = std::min(1.6f * i0 / float(kLut - 1), 1.0f);
                const float b = std::min(1.6f * (i0 + 1) / float(kLut - 1), 1.0f);
                g[c] = a + (b - a) * f;
            }
        };
        bool rising = true;
        float off = 0.0f;
        for (int i = 0; i < N; ++i)
        {
            if (i > 0 && !(got[i].r > got[i - 1].r * 1.001f))
                rising = false;
            const float in[3] = { ramp[i].r, ramp[i].g, ramp[i].b };
            float want[3];
            GradeLinearHdr(in, 1.0f, bright, want);
            off = std::max(off, std::abs(got[i].r - want[0]) / std::max(want[0], 1e-3f));
        }
        std::printf("brightening look on a grey ramp: rising %s, worst error vs the CPU grade %.2e\n",
                    rising ? "yes" : "NO", off);
        expect(rising, "a brightening look flattens or reverses the ramp");
        expect(off < 1e-2f, "a brightening look does not grade as the CPU model");
    }

    // 10. Strength 0 is the identity on Linear HDR, below and above the knee and past Neutwo's old clamp.
    for (float white : { 0.06f, 1.0f, 18.5f })
    {
        auto c = Base(0, true, white);
        c.Strength = 0.0f;
        const auto got = gpu.Run(shader.Get(), linear, c, 0.0f);
        const float off = Worst(got, linear);
        std::printf("strength 0 at white %g: worst relative change %.2e\n", white, off);
        expect(off < 1e-4f, "strength 0 changes a Linear HDR picture at white " + std::to_string(white));
    }

    // 6. The shipped D3D11 bytecode against this compile, with the chain step's constants.
    {
        ComPtr<ID3D11ComputeShader> shipped;
        check(gpu.device->CreateComputeShader(dlssnr_lut_dx11_cso, sizeof(dlssnr_lut_dx11_cso), nullptr, &shipped));
        auto sdr = Base(0, false, 1.0f);
        auto hdr = Base(0, true, 1.0f);
        auto hdrExposure = Base(0, true, 1.0f);
        hdrExposure.PreExposure = 2.0f;
        hdrExposure.UseExposureWhitePoint = 1u;
        hdrExposure.ExposureBaseScale = 1.0f;
        auto hdrLog = Base(0, true, 1.0f);
        hdrLog.LatticeCoord = 1u;
        expect(SameBits(gpu.Run(shipped.Get(), display, sdr, 0.0f), gpu.Run(shader.Get(), display, sdr, 0.0f)),
               "shipped D3D11 shader differs on a tone-mapped frame");
        expect(SameBits(gpu.Run(shipped.Get(), linear, hdr, 0.0f), gpu.Run(shader.Get(), linear, hdr, 0.0f)),
               "shipped D3D11 shader differs on a linear HDR frame");
        expect(SameBits(gpu.Run(shipped.Get(), linear, hdrExposure, 0.5f),
                        gpu.Run(shader.Get(), linear, hdrExposure, 0.5f)),
               "shipped D3D11 shader differs with the game's exposure");
        expect(SameBits(gpu.Run(shipped.Get(), linear, hdrLog, 0.0f), gpu.Run(shader.Get(), linear, hdrLog, 0.0f)),
               "shipped D3D11 shader differs on the log-spaced lattice");
        // The chain's white point with the game's exposure: PreExposure 2 / exposure 0.5 = 4.
        expect(Worst(gpu.Run(shipped.Get(), linear, hdrExposure, 0.5f),
                     gpu.Run(shader.Get(), linear, Base(0, true, 4.0f), 0.0f)) < 1e-5f,
               "the chain step's white point is not PreExposure / exposure");
        std::puts("shipped D3D11 shader (F5LUT chain step) checked against the source");
    }

    std::puts(fails == 0 ? "PASS: the LUT grades for the encode's white point, live or host (WARP HLSL)"
                         : "FAILED");
    return fails == 0 ? 0 : 1;
}
catch (const std::exception& e)
{
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
}
