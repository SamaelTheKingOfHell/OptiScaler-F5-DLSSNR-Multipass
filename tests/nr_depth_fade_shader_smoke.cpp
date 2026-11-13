// Headless shader test: Windows D3D11 WARP runs dlssnr.hlsl's real resolve for the depth fade
// (dlssnr/precompile/dlssnr_depthfade.hlsli, DlssNr_DepthFade.h). No game and no NVIDIA model: the answer, the frame
// and the depth are textures the test fills.
//
//   1. Fade off is the resolve it always was: the same shader source as the one before the fade existed (the second
//      argument, a copy of the committed dlssnr.hlsl from before this change) and this one give the same bits, for
//      Replace and Composed, SDR and HDR, skin protection on and off, with a depth texture bound and with a block that
//      carries numbers but is not switched on. A fade that keeps everything (all strengths 1) is the same bits too.
//   2. Fade on: the pixel is lerp(frame as it arrived, the result with the fade off, k), k from the falloff maths
//      (the same file the shader includes), with the nearest of the 3x3 depth neighbourhood deciding, in a depth
//      texture bigger than its valid region (base offset), for standard and reversed depth, and sky.
//   3. Debug view 4 writes k as grey. No depth (block not active) is k = 1. The skin mask preview is left alone.
//
// Build: cl /nologo /std:c++20 /EHsc /W3 tests\nr_depth_fade_shader_smoke.cpp /link d3d11.lib d3dcompiler.lib
// Run:   nr_depth_fade_shader_smoke.exe OptiScaler\shaders\dlssnr\precompile\dlssnr.hlsl <old dlssnr.hlsl>
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

// What HLSL has built in and the shared falloff file uses.
static float saturate(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }
static float lerp(float a, float b, float t) { return a + (b - a) * t; }
using std::log2;
using std::max;
#include "../OptiScaler/shaders/dlssnr/precompile/dlssnr_depthfade.hlsli"
#include "../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
using Microsoft::WRL::ComPtr;

struct Pixel { float r, g, b, a; };
constexpr int N = 6;
using Row = std::array<Pixel, N>;

static void check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D call failed"); }
static int fails = 0;
static void expect(bool ok, const std::string& label) { if (!ok) { std::printf("FAIL: %s\n", label.c_str()); ++fails; } }

// The depth texture is larger than the region the frame maps onto, so the base offset matters.
constexpr int DEPTH_W = 10, DEPTH_H = 3, REGION_X = 3, REGION_Y = 1;

struct Gpu
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;

    Gpu()
    {
        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &ctx));
        D3D11_BUFFER_DESC buffer {};
        buffer.ByteWidth = sizeof(DlssNrConstants);
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        check(device->CreateBuffer(&buffer, nullptr, &constants));
        D3D11_SAMPLER_DESC sampling {};
        sampling.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sampling.AddressU = sampling.AddressV = sampling.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampling.MaxLOD = D3D11_FLOAT32_MAX;
        check(device->CreateSamplerState(&sampling, &sampler));
    }

    ComPtr<ID3D11ComputeShader> Compile(const wchar_t* path)
    {
        ComPtr<ID3DBlob> code, errors;
        // WARP crashes on the percentile meter's branch; this test does not use the meter.
        const D3D_SHADER_MACRO noMeter[] = { { "DLSSNR_NO_PERCENTILE_METER", "1" }, { nullptr, nullptr } };
        HRESULT hr = D3DCompileFromFile(path, noMeter, D3D_COMPILE_STANDARD_FILE_INCLUDE, "CSMain", "cs_5_0",
                                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (errors && FAILED(hr)) std::fprintf(stderr, "%s", (char*) errors->GetBufferPointer());
        check(hr);
        ComPtr<ID3D11ComputeShader> shader;
        check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader));
        return shader;
    }

    ComPtr<ID3D11Texture2D> Texture(const Row* data, bool uav)
    {
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = N; desc.Height = 1; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0);
        D3D11_SUBRESOURCE_DATA init {};
        if (data)
            init = {data->data(), sizeof(Row), 0};
        ComPtr<ID3D11Texture2D> texture;
        check(device->CreateTexture2D(&desc, data ? &init : nullptr, &texture));
        return texture;
    }

    // R32_FLOAT depth, DEPTH_W x DEPTH_H, filled with `outside` everywhere but the region, which holds `inside`.
    ComPtr<ID3D11Texture2D> DepthTexture(const std::array<float, N>& inside, float outside)
    {
        std::vector<float> data(DEPTH_W * DEPTH_H, outside);
        for (int x = 0; x < N; ++x)
            data[REGION_Y * DEPTH_W + REGION_X + x] = inside[x];
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = DEPTH_W; desc.Height = DEPTH_H; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32_FLOAT; desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init { data.data(), DEPTH_W * sizeof(float), 0 };
        ComPtr<ID3D11Texture2D> texture;
        check(device->CreateTexture2D(&desc, &init, &texture));
        return texture;
    }

    Row Read(ID3D11Texture2D* texture)
    {
        D3D11_TEXTURE2D_DESC desc {};
        texture->GetDesc(&desc);
        desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        check(device->CreateTexture2D(&desc, nullptr, &staging));
        ctx->CopyResource(staging.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped {};
        check(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        Row row {};
        std::memcpy(row.data(), mapped.pData, sizeof(Row));
        ctx->Unmap(staging.Get(), 0);
        return row;
    }

    // One dispatch: t0 proxy, t1 model, t2 original, t5 depth, u0 target, u1 keep.
    void Run(ID3D11ComputeShader* shader, const DlssNrConstants& settings, ID3D11Texture2D* t0, ID3D11Texture2D* t1,
             ID3D11Texture2D* t2, ID3D11Texture2D* depth, ID3D11Texture2D* u0, ID3D11Texture2D* u1)
    {
        ComPtr<ID3D11ShaderResourceView> s0, s1, s2, sd;
        check(device->CreateShaderResourceView(t0, nullptr, &s0));
        check(device->CreateShaderResourceView(t1, nullptr, &s1));
        check(device->CreateShaderResourceView(t2, nullptr, &s2));
        check(device->CreateShaderResourceView(depth, nullptr, &sd));
        ComPtr<ID3D11UnorderedAccessView> v0, v1;
        check(device->CreateUnorderedAccessView(u0, nullptr, &v0));
        check(device->CreateUnorderedAccessView(u1, nullptr, &v1));
        ID3D11ShaderResourceView* srvs[] = {s0.Get(), s1.Get(), s2.Get(), s0.Get(), s0.Get(), sd.Get()};
        ID3D11UnorderedAccessView* uavs[] = {v0.Get(), v1.Get()};
        ctx->UpdateSubresource(constants.Get(), 0, nullptr, &settings, 0, 0);
        ctx->CSSetShader(shader, nullptr, 0);
        ctx->CSSetShaderResources(0, 6, srvs);
        ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        ctx->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
        ctx->CSSetSamplers(0, 1, sampler.GetAddressOf());
        ctx->Dispatch(1, 1, 1);
        ID3D11ShaderResourceView* noSrvs[6] = {};
        ID3D11UnorderedAccessView* noUavs[2] = {};
        ctx->CSSetShaderResources(0, 6, noSrvs);
        ctx->CSSetUnorderedAccessViews(0, 2, noUavs, nullptr);
    }
};

struct Case
{
    uint32_t passthrough = 1;
    uint32_t curve = 2; // ReversibleMode
    float whitePoint = 1.0f;
    bool skinProtection = false;
    bool showSkinMask = false;
    uint32_t debugView = 0;
};

static DlssNrConstants Constants(const Case& c)
{
    DlssNrConstants k {};
    k.Mode = DlssNrMode_Resolve;
    k.Width = N; k.Height = 1;
    k.WhitePoint = c.whitePoint;
    k.TransferStrength = 1.0f; k.ColourStrength = 1.0f;
    k.MaxRatio = 8.0f;
    k.Passthrough = c.passthrough;
    k.ApplyModel = 1;
    k.ReversibleMode = c.curve;
    k.DebugScale = 1.0f;
    k.DebugView = c.debugView;
    k.ModelWorkScale = 1.0f;
    k.PassFeedback = 1.0f;
    k.SkinProtection = c.skinProtection ? 1u : 0u;
    k.ShowSkinMask = c.showSkinMask ? 1u : 0u;
    k.SkinDetail = 0.5f; k.SkinColour = 0.5f; k.EnvironmentDetail = 0.5f; k.EnvironmentColour = 0.5f;
    return k;
}

struct Fade
{
    bool enabled = true;
    bool inverted = false;
    float farStart = 0.3f, farEnd = 0.6f, farStrength = 0.2f;
    float nearStart = 0.0f, nearEnd = 0.1f, nearStrength = 0.5f;
    float skyStrength = 0.0f;
};

static DlssNrDepthFade::Block BlockOf(const Fade& f)
{
    DlssNrDepthFade::Block b {};
    if (!f.enabled)
        return b;
    b.Flags = DlssNrDepthFade::kActive | (f.inverted ? DlssNrDepthFade::kInverted : 0u);
    b.Width = N; b.Height = 1; b.BaseX = REGION_X; b.BaseY = REGION_Y;
    b.FarStart = f.farStart; b.FarEnd = f.farEnd; b.FarStrength = f.farStrength;
    b.NearStart = f.nearStart; b.NearEnd = f.nearEnd; b.NearStrength = f.nearStrength;
    b.SkyStrength = f.skyStrength;
    return b;
}

struct Frames
{
    Row proxy, model, original;
};

static Row Resolve(Gpu& gpu, ID3D11ComputeShader* shader, const Frames& fr, const DlssNrConstants& k,
                   ID3D11Texture2D* depth)
{
    auto p = gpu.Texture(&fr.proxy, false);
    auto m = gpu.Texture(&fr.model, false);
    auto o = gpu.Texture(&fr.original, false);
    auto target = gpu.Texture(nullptr, true);
    auto spare = gpu.Texture(nullptr, true);
    gpu.Run(shader, k, p.Get(), m.Get(), o.Get(), depth, target.Get(), spare.Get());
    return gpu.Read(target.Get());
}

static bool SameBits(const Row& a, const Row& b) { return std::memcmp(a.data(), b.data(), sizeof(Row)) == 0; }

// The keep factor of pixel x, worked out the long way round from the depth values: the nearest of the pixel and its
// neighbours inside the region decides.
static float ExpectedKeep(const std::array<float, N>& depth, int x, const Fade& f)
{
    float nearest = -1.0f;
    for (int dx = -1; dx <= 1; ++dx)
    {
        const int tx = std::clamp(x + dx, 0, N - 1);
        const float z = depth[tx];
        nearest = std::max(nearest, saturate(f.inverted ? z : 1.0f - z));
    }
    return DepthFadeFactor(nearest, f.farStart, f.farEnd, f.farStrength, f.nearStart, f.nearEnd, f.nearStrength,
                           f.skyStrength);
}

int wmain(int argc, wchar_t** argv) try
{
    if (argc != 3) throw std::runtime_error("Pass <dlssnr.hlsl> <the dlssnr.hlsl from before the depth fade>");
    static_assert(sizeof(DlssNrConstants) == 512);
    Gpu gpu;
    auto shader = gpu.Compile(argv[1]);
    auto before = gpu.Compile(argv[2]);

    // A frame that runs well past 1 in places, a different proxy and a model answer that differs from it everywhere.
    Frames fr;
    fr.model = {{ {0.02f, 0.03f, 0.05f, 1}, {0.2f, 0.25f, 0.3f, 1}, {0.5f, 0.45f, 0.4f, 1},
                  {0.9f, 0.85f, 0.8f, 1}, {0.8f, 0.3f, 0.1f, 1}, {0.1f, 0.4f, 0.7f, 1} }};
    fr.proxy = {{ {0.03f, 0.03f, 0.04f, 1}, {0.22f, 0.22f, 0.22f, 1}, {0.45f, 0.45f, 0.45f, 1},
                  {0.85f, 0.85f, 0.85f, 1}, {0.7f, 0.35f, 0.15f, 1}, {0.15f, 0.35f, 0.6f, 1} }};
    fr.original = {{ {0.05f, 0.04f, 0.04f, 1}, {0.6f, 0.5f, 0.4f, 1}, {1.5f, 1.2f, 0.9f, 1},
                     {3.0f, 2.5f, 2.0f, 1}, {2.0f, 0.5f, 0.2f, 1}, {0.4f, 1.1f, 2.2f, 1} }};

    // Standard depth (far = 1): a surface at the camera, one a doubling out, a mid one, a far one, the plane (sky), a
    // far one again. The garbage around the region is a value that would be a very near surface, so a read outside the
    // region shows.
    const std::array<float, N> standard { 0.0f, 0.5f, 0.99f, 0.9999f, 1.0f, 0.999f };
    const float garbage = 0.0f;
    auto depthStandard = gpu.DepthTexture(standard, garbage);

    // 1. Off is the old resolve, bit for bit.
    {
        struct Config { Case c; const char* what; };
        const Config configs[] = {
            { Case { 1, 2, 1.0f, false }, "SDR Replace" },
            { Case { 1, 4, 4.0f, false }, "SDR Replace hybrid, white point 4" },
            { Case { 1, 1, 1.0f, false }, "SDR Composed" },
            { Case { 0, 1, 4.0f, false }, "HDR Composed, white point 4" },
            { Case { 0, 2, 4.0f, false }, "HDR Replace, white point 4" },
            { Case { 0, 0, 2.0f, false }, "HDR soft knee" },
            { Case { 1, 2, 1.0f, true }, "SDR Replace with skin protection" },
            { Case { 0, 1, 2.0f, true }, "HDR Composed with skin protection" },
            { Case { 1, 1, 1.0f, true, true }, "skin mask preview" },
            { Case { 1, 2, 1.0f, false, false, 3 }, "debug view 3" },
        };
        for (const auto& cfg : configs)
        {
            DlssNrConstants off = Constants(cfg.c);
            const Row reference = Resolve(gpu, before.Get(), fr, off, depthStandard.Get());
            expect(SameBits(Resolve(gpu, shader.Get(), fr, off, depthStandard.Get()), reference),
                   std::string("fade off differs from the resolve before the fade: ") + cfg.what);

            // Numbers in the block but not switched on: still off.
            Fade numbersOnly;
            numbersOnly.enabled = true;
            DlssNrConstants carrying = Constants(cfg.c);
            carrying.DepthFade = BlockOf(numbersOnly);
            carrying.DepthFade.Flags = 0;
            expect(SameBits(Resolve(gpu, shader.Get(), fr, carrying, depthStandard.Get()), reference),
                   std::string("a block that is not switched on changes the picture: ") + cfg.what);

            // On, but every strength keeps all of NR: no change either.
            Fade keepAll;
            keepAll.farStrength = 1.0f; keepAll.nearStrength = 1.0f; keepAll.skyStrength = 1.0f;
            DlssNrConstants keeping = Constants(cfg.c);
            keeping.DepthFade = BlockOf(keepAll);
            expect(SameBits(Resolve(gpu, shader.Get(), fr, keeping, depthStandard.Get()), reference),
                   std::string("a fade that keeps everything changes the picture: ") + cfg.what);
        }
    }

    // 2. Fade on: lerp(frame as it arrived, result with the fade off, k).
    {
        const Case c {};
        const Row off = Resolve(gpu, shader.Get(), fr, Constants(c), depthStandard.Get());
        // Replace on an SDR frame writes the model's answer, which differs from the original, so the fade has
        // something to act on.
        bool differs = false;
        for (int i = 0; i < N; ++i) differs = differs || off[i].r != fr.original[i].r;
        expect(differs, "setup: the fade has no edit to act on");

        for (int direction = 0; direction < 2; ++direction)
            for (int variant = 0; variant < 3; ++variant)
            {
                Fade f;
                f.inverted = direction == 1;
                if (variant == 1) { f.farStrength = 0.0f; f.nearStrength = 1.0f; f.skyStrength = 1.0f; }
                if (variant == 2) { f.farStrength = 1.0f; f.nearStrength = 0.0f; f.nearEnd = 0.3f; f.skyStrength = 0.25f; }

                // The same surfaces, as reversed depth stores them.
                std::array<float, N> depth = standard;
                if (f.inverted)
                    for (auto& z : depth) z = 1.0f - z;
                auto tex = gpu.DepthTexture(depth, f.inverted ? 1.0f : garbage);
                DlssNrConstants k = Constants(c);
                k.DepthFade = BlockOf(f);
                const Row got = Resolve(gpu, shader.Get(), fr, k, tex.Get());

                for (int x = 0; x < N; ++x)
                {
                    const float keep = ExpectedKeep(depth, x, f);
                    const float want[3] = { lerp(fr.original[x].r, off[x].r, keep), lerp(fr.original[x].g, off[x].g, keep),
                                            lerp(fr.original[x].b, off[x].b, keep) };
                    const float have[3] = { got[x].r, got[x].g, got[x].b };
                    for (int ch = 0; ch < 3; ++ch)
                        expect(std::fabs(have[ch] - want[ch]) <= 1e-5f * std::max(1.0f, std::fabs(want[ch])),
                               "pixel " + std::to_string(x) + " channel " + std::to_string(ch) + " (inverted " +
                                   std::to_string(direction) + ", variant " + std::to_string(variant) + "): " +
                                   std::to_string(have[ch]) + " vs " + std::to_string(want[ch]) + " at keep " +
                                   std::to_string(keep));
                    expect(got[x].a == fr.original[x].a, "alpha is the original's");
                }
            }

        // Sensitivity: the fade does change the picture (the checks above can fail).
        Fade f;
        DlssNrConstants k = Constants(c);
        k.DepthFade = BlockOf(f);
        expect(!SameBits(Resolve(gpu, shader.Get(), fr, k, depthStandard.Get()), off), "sensitivity: the fade acts");

        // The silhouette rule: pixel 4 is exactly the far plane, but its neighbour is a far surface, so it is not sky.
        {
            Fade s;
            s.farStrength = 1.0f; s.nearStrength = 1.0f; s.skyStrength = 0.0f;
            std::array<float, N> alone { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
            auto allSky = gpu.DepthTexture(alone, garbage);
            DlssNrConstants ks = Constants(c);
            ks.DepthFade = BlockOf(s);
            const Row sky = Resolve(gpu, shader.Get(), fr, ks, allSky.Get());
            bool allOriginal = true;
            for (int x = 0; x < N; ++x) allOriginal = allOriginal && sky[x].r == fr.original[x].r;
            expect(allOriginal, "all sky with sky strength 0 gives the frame as it arrived");
            expect(sky[4].r == fr.original[4].r && ExpectedKeep(standard, 4, s) > 0.0f,
                   "setup: a sky pixel beside a surface is not sky");
        }
    }

    // 3. Debug view 4, no depth, the skin mask preview.
    {
        Case view;
        view.debugView = 4;
        Fade f;
        DlssNrConstants k = Constants(view);
        k.DepthFade = BlockOf(f);
        const Row got = Resolve(gpu, shader.Get(), fr, k, depthStandard.Get());
        for (int x = 0; x < N; ++x)
        {
            const float keep = ExpectedKeep(standard, x, f);
            for (float v : { got[x].r, got[x].g, got[x].b })
                expect(std::fabs(v - keep) <= 1e-5f, "debug view 4 pixel " + std::to_string(x) + " is " + std::to_string(v) +
                                                         ", expected " + std::to_string(keep));
        }

        // No depth for the frame: the view is white and the picture is the one without the fade.
        DlssNrConstants none = Constants(view);
        none.DepthFade = BlockOf(f);
        none.DepthFade.Flags = 0;
        const Row white = Resolve(gpu, shader.Get(), fr, none, depthStandard.Get());
        bool allWhite = true;
        for (int x = 0; x < N; ++x) allWhite = allWhite && white[x].r == 1.0f && white[x].g == 1.0f && white[x].b == 1.0f;
        expect(allWhite, "debug view 4 without depth is white");

        Case mask;
        mask.showSkinMask = true;
        mask.skinProtection = true;
        mask.curve = 1;
        DlssNrConstants withMask = Constants(mask);
        const Row maskOff = Resolve(gpu, shader.Get(), fr, withMask, depthStandard.Get());
        withMask.DepthFade = BlockOf(f);
        expect(SameBits(Resolve(gpu, shader.Get(), fr, withMask, depthStandard.Get()), maskOff),
               "the skin mask preview is changed by the fade");
    }

    if (fails)
    {
        std::printf("%d check(s) failed\n", fails);
        return 1;
    }
    std::puts("PASS: depth fade off is bit for bit the earlier resolve; on it is lerp(frame, result, k) for standard and "
              "reversed depth, sky and the nearest-of-nine rule; debug view 4 shows k (WARP HLSL)");
    return 0;
}
catch (const std::exception& e)
{
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
}
