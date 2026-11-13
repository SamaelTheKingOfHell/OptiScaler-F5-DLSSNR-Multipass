// Headless shader comparison: D3D11 runs dlssnr.hlsl's encode, downsample and resolve to compare the three ways a
// model that ran below the frame's size comes back ([DlssNr] Transfer): 0 Classic (the answer enlarged bilinearly),
// 1 Matched residual (the RGB change enlarged and added), 2 NVIDIA residual (the change in OkLab -- brightness as a
// ratio, colour as a difference -- enlarged with weights guided by the full-size frame).
//
// Each scene is a row: hard edges, fine stripes, saturated colour, a shallow and a steep smooth ramp, fine texture. A known edit is made to it at full size
// (brighten the highlights, deepen the shadows, local contrast, +10%, a colour shift), standing in for the model.
// The reference is the resolve with the model at full size (Transfer 0, work scale 1). Each mode then gets only
// the reduced pair -- the frame's proxy and the edited proxy, area-downsampled by the shader to 50% and to 65.6%
// (42 of 64 pixels) -- and is scored against that reference on the resolved frame:
//
//   err    mean |Y - Y_ref| / max(Y_ref, 0.005), all pixels
//   halo   the largest brightening past the reference on dark pixels (frame Y < 0.1), relative, and `at`, the pixel
//          where it occurs
//   carried  how much of the edit arrives: sum |Y - Y_frame| / sum |Y_ref - Y_frame| (1 = all of it)
//   hue    the largest change of chromaticity (rgb / Y) past the reference, on pixels the edit touched
//   edge   err on the pixels within 2 of an edge of the frame (neighbours differing by more than 20%); flat, the rest.
//          The summary pools these pixels over all cases, so a scene with one flat pixel does not weigh like a flat one.
//
// Curves: soft knee, HLG and a passthrough (already tone-mapped) frame. It fails when a mode's output is not finite,
// when NVIDIA residual's mean edge err is not below kEdgeLimit of the plain bilinear enlarge, or when its flat err
// grows past kFlatLimit of that. The bilinear numbers come from the reference shader when one is given (pass the
// shader from before the guided enlarge, e.g. 317f11f6), else from kBilinear. None of the scenes reaches below OkLab
// L 0.1, so the near-black fix (nr_proxy_curves_shader_smoke.cpp section 7) does not show here.
//
// With a reference dlssnr.hlsl, it also requires bit-identical output for Classic and Matched residual at both sizes,
// every mode with the model at full size, and NVIDIA residual after SGSR1 (the answer at the frame's size), over all
// eight proxy curves and passthrough. That is D3D11 with fxc; the Vulkan (dxc) build is not compared.
//
// Runs on the hardware GPU. --warp asks for WARP, which has crashed on this shader since 507c9404 (the percentile
// exposure meter's groupshared histogram); the game never runs there.
//
// Build: cl /nologo /std:c++20 /EHsc /W3 tests\nr_residual_compare_shader_smoke.cpp /link d3d11.lib d3dcompiler.lib
// Run:   nr_residual_compare_shader_smoke.exe <dlssnr.hlsl> [reference dlssnr.hlsl] [--warp]
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>
#include "../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
using Microsoft::WRL::ComPtr;

struct Pixel { float r, g, b, a; };
using Row = std::vector<Pixel>;

static void check(HRESULT hr, int line = __builtin_LINE())
{
    if (FAILED(hr)) throw std::runtime_error("D3D call failed, line " + std::to_string(line));
}
static int fails = 0;
static void expect(bool ok, const std::string& label) { if (!ok) { std::printf("FAIL: %s\n", label.c_str()); ++fails; } }

struct Gpu
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> linearSampler;

    explicit Gpu(bool warp)
    {
        check(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                D3D11_SDK_VERSION, &device, nullptr, &ctx));
        D3D11_BUFFER_DESC buffer {};
        buffer.ByteWidth = sizeof(DlssNrConstants);
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        check(device->CreateBuffer(&buffer, nullptr, &constants));
        // gLinear, as the game binds it (DlssNr_Dx12.cpp: MIN_MAG_MIP_LINEAR, clamped).
        D3D11_SAMPLER_DESC sampling {};
        sampling.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampling.AddressU = sampling.AddressV = sampling.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampling.MaxLOD = D3D11_FLOAT32_MAX;
        check(device->CreateSamplerState(&sampling, &linearSampler));
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

    ComPtr<ID3D11Texture2D> Texture(UINT width, const Row* data)
    {
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = width; desc.Height = 1; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        D3D11_SUBRESOURCE_DATA init {data ? data->data() : nullptr, (UINT) (width * sizeof(Pixel)), 0};
        ComPtr<ID3D11Texture2D> texture;
        check(device->CreateTexture2D(&desc, data ? &init : nullptr, &texture));
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
        Row row(desc.Width);
        std::memcpy(row.data(), mapped.pData, desc.Width * sizeof(Pixel));
        ctx->Unmap(staging.Get(), 0);
        return row;
    }

    // One dispatch: t0..t2 as given (t3/t4 get t0, unread on these paths), u0 target, u1 keep.
    void Run(ID3D11ComputeShader* shader, const DlssNrConstants& settings, ID3D11Texture2D* t0, ID3D11Texture2D* t1,
             ID3D11Texture2D* t2, ID3D11Texture2D* u0, ID3D11Texture2D* u1)
    {
        ComPtr<ID3D11ShaderResourceView> s0, s1, s2;
        check(device->CreateShaderResourceView(t0, nullptr, &s0));
        check(device->CreateShaderResourceView(t1 ? t1 : t0, nullptr, &s1));
        check(device->CreateShaderResourceView(t2 ? t2 : t0, nullptr, &s2));
        ComPtr<ID3D11UnorderedAccessView> v0, v1;
        check(device->CreateUnorderedAccessView(u0, nullptr, &v0));
        check(device->CreateUnorderedAccessView(u1, nullptr, &v1));
        ID3D11ShaderResourceView* srvs[] = {s0.Get(), s1.Get(), s2.Get(), s0.Get(), s0.Get()};
        ID3D11UnorderedAccessView* uavs[] = {v0.Get(), v1.Get()};
        ctx->UpdateSubresource(constants.Get(), 0, nullptr, &settings, 0, 0);
        ctx->CSSetShader(shader, nullptr, 0);
        ctx->CSSetShaderResources(0, 5, srvs);
        ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        ctx->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
        ctx->CSSetSamplers(0, 1, linearSampler.GetAddressOf());
        ctx->Dispatch((settings.Width + 7) / 8, 1, 1); // 8x8 groups
        ID3D11ShaderResourceView* noSrvs[5] = {};
        ID3D11UnorderedAccessView* noUavs[2] = {};
        ctx->CSSetShaderResources(0, 5, noSrvs);
        ctx->CSSetUnorderedAccessViews(0, 2, noUavs, nullptr);
    }
};

static DlssNrConstants Base(uint32_t mode, UINT width, uint32_t reversible, uint32_t passthrough)
{
    DlssNrConstants k {};
    k.Mode = mode;
    k.Width = width; k.Height = 1;
    k.WhitePoint = 1.0f;
    k.TransferStrength = 1.0f; k.ColourStrength = 1.0f;
    k.MaxRatio = 8.0f;
    k.ApplyModel = 1;
    k.ReversibleMode = reversible;
    k.Passthrough = passthrough;
    k.DebugScale = 1.0f;
    k.ModelWorkScale = 1.0f;
    k.PassFeedback = 1.0f;
    return k;
}

// The encode's proxy and kept copy, and the proxy area-downsampled to `reduced` pixels, left on the GPU.
struct Encoded { ComPtr<ID3D11Texture2D> proxy, keep, reduced; };

static Encoded Encode(Gpu& gpu, ID3D11ComputeShader* shader, const Row& input, uint32_t reversible, uint32_t passthrough,
                      UINT reduced)
{
    const UINT n = (UINT) input.size();
    auto source = gpu.Texture(n, &input);
    Encoded e {gpu.Texture(n, nullptr), gpu.Texture(n, nullptr), gpu.Texture(reduced, nullptr)};
    auto spare = gpu.Texture(n, nullptr);
    gpu.Run(shader, Base(DlssNrMode_Encode, n, reversible, passthrough), source.Get(), nullptr, nullptr, e.proxy.Get(), e.keep.Get());
    gpu.Run(shader, Base(DlssNrMode_Downsample, reduced, reversible, passthrough), e.proxy.Get(), nullptr, nullptr, e.reduced.Get(), spare.Get());
    return e;
}

static bool SameBits(const Row& a, const Row& b)
{
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(Pixel)) == 0;
}

// Resolve only: t0 the proxy the model saw, t1 its answer, t2 the kept frame.
static Row Resolve(Gpu& gpu, ID3D11ComputeShader* shader, ID3D11Texture2D* proxy, ID3D11Texture2D* model, ID3D11Texture2D* keep,
                   UINT n, uint32_t reversible, uint32_t passthrough, uint32_t transfer, float workScale)
{
    auto frame = gpu.Texture(n, nullptr);
    auto spare = gpu.Texture(n, nullptr);
    DlssNrConstants k = Base(DlssNrMode_Resolve, n, reversible, passthrough);
    k.Transfer = transfer;
    k.ModelWorkScale = workScale;
    gpu.Run(shader, k, proxy, model, keep, frame.Get(), spare.Get());
    return gpu.Read(frame.Get());
}

static float Luma(const Pixel& p) { return 0.2126f * p.r + 0.7152f * p.g + 0.0722f * p.b; }

static bool Finite(const Row& row)
{
    for (const Pixel& p : row)
        if (!std::isfinite(p.r) || !std::isfinite(p.g) || !std::isfinite(p.b))
            return false;
    return true;
}

// Scenes, 64 pixels, edges on odd pixels so a half-size texel straddles them.
static Row EdgeScene()
{
    Row row;
    for (int i = 0; i < 64; ++i)
    {
        const bool bright = (i >= 13 && i < 29) || (i >= 45 && i < 55);
        const float v = bright ? 0.8f : 0.02f;
        row.push_back({v, v, v, 1});
    }
    return row;
}
static Row StripeScene()
{
    Row row;
    for (int i = 0; i < 64; ++i)
    {
        const float v = (i / 3) % 2 ? 0.5f : 0.04f; // stripes three pixels wide: not a multiple of the 2:1 grid
        row.push_back({v, v, v, 1});
    }
    return row;
}
static Row ColourScene()
{
    Row row;
    for (int i = 0; i < 64; ++i)
    {
        const int band = ((i + 3) / 8) % 4;
        const Pixel colours[4] = {{0.7f, 0.06f, 0.03f, 1}, {0.02f, 0.03f, 0.12f, 1}, {0.1f, 0.5f, 0.08f, 1}, {0.9f, 0.8f, 0.6f, 1}};
        row.push_back(colours[band]);
    }
    return row;
}

// A smooth ramp (a lit wall, a sky): no edges, so a guided enlarge must not fall back to blocks here.
static Row RampScene()
{
    Row row;
    for (int i = 0; i < 64; ++i)
    {
        const float t = (float) i / 63.0f;
        row.push_back({0.03f + 0.8f * t, 0.03f + 0.6f * t, 0.05f + 0.3f * t, 1});
    }
    return row;
}

// A steep but smooth gradient (soft shadow, falloff): up and down over 20 pixels, about 0.04 per pixel.
static Row SteepRampScene()
{
    Row row;
    for (int i = 0; i < 64; ++i)
    {
        const float t = (float) std::abs(i % 40 - 20) / 20.0f;
        row.push_back({0.06f + 0.8f * t, 0.05f + 0.7f * t, 0.04f + 0.5f * t, 1});
    }
    return row;
}
// Fine mid-amplitude texture (grain, gravel): +-0.03 around 0.4, fixed pseudo-random.
static Row TextureScene()
{
    Row row;
    uint32_t seed = 12345u;
    for (int i = 0; i < 64; ++i)
    {
        seed = seed * 1664525u + 1013904223u;
        const float v = 0.4f + 0.06f * ((float) (seed >> 8 & 1023u) / 1023.0f - 0.5f);
        row.push_back({v, v * 0.95f, v * 0.85f, 1});
    }
    return row;
}

using Edit = std::function<Row(const Row&)>;

static Row EachPixel(const Row& row, const std::function<Pixel(const Pixel&)>& f)
{
    Row out;
    for (const Pixel& p : row)
        out.push_back(f(p));
    return out;
}

// The pixels within 2 of an edge of the frame: neighbours whose luminance differs by more than 20% (of at least 0.05,
// so a dark gradient's small steps are not edges).
static std::vector<bool> EdgeBand(const Row& frame)
{
    const int n = (int) frame.size();
    std::vector<bool> band(n, false);
    for (int i = 0; i + 1 < n; ++i)
    {
        const float a = Luma(frame[i]), b = Luma(frame[i + 1]);
        if (std::abs(a - b) / std::max({a, b, 0.05f}) > 0.2f)
            for (int j = std::max(i - 1, 0); j <= std::min(i + 2, n - 1); ++j)
                band[j] = true;
    }
    return band;
}

// NVIDIA residual with the plain bilinear enlarge, before the guided one: mean edge / flat err per pixel, by curve
// (soft knee, HLG, passthrough) and size (50%, 65.6%). Measured with this harness on 317f11f6 (RTX 5070 Ti); used only
// without a reference shader, which otherwise supplies its own (the reference's Transfer 2 on the same inputs).
static const double kBilinear[3][2][2] = {{{0.06029, 0.00160}, {0.05156, 0.00153}},
                                           {{0.06353, 0.00180}, {0.05431, 0.00174}},
                                           {{0.06004, 0.00167}, {0.05204, 0.00163}}};
static constexpr double kEdgeLimit = 0.85; // the guided enlarge has to take at least 15% off the edge err
static constexpr double kFlatLimit = 1.05; // and may not make flat areas more than 5% worse

int wmain(int argc, wchar_t** argv) try
{
    const wchar_t* shaderPath = nullptr;
    const wchar_t* referencePath = nullptr;
    bool warp = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::wcscmp(argv[i], L"--warp") == 0)
            warp = true;
        else if (!shaderPath)
            shaderPath = argv[i];
        else if (!referencePath)
            referencePath = argv[i];
        else
            throw std::runtime_error("Too many arguments");
    }
    if (!shaderPath)
        throw std::runtime_error("Pass the dlssnr.hlsl path");
    Gpu gpu(warp);
    auto shader = gpu.Compile(shaderPath);
    ComPtr<ID3D11ComputeShader> reference;
    if (referencePath)
        reference = gpu.Compile(referencePath);

    struct Scene { const char* name; Row row; };
    const Scene scenes[] = {{"edges", EdgeScene()},   {"stripes", StripeScene()},    {"colour", ColourScene()},
                            {"ramp", RampScene()},    {"steep", SteepRampScene()},   {"texture", TextureScene()}};

    struct NamedEdit { const char* name; Edit edit; };
    const NamedEdit edits[] = {
        {"brighten highlights x1.2",
         [](const Row& r) { return EachPixel(r, [](const Pixel& p) { const float s = Luma(p) > 0.3f ? 1.2f : 1.0f; return Pixel {p.r * s, p.g * s, p.b * s, 1}; }); }},
        {"deepen shadows x0.7",
         [](const Row& r) { return EachPixel(r, [](const Pixel& p) { const float s = Luma(p) < 0.1f ? 0.7f : 1.0f; return Pixel {p.r * s, p.g * s, p.b * s, 1}; }); }},
        {"local contrast",
         [](const Row& r)
         {
             Row out;
             for (size_t i = 0; i < r.size(); ++i)
             {
                 const Pixel& a = r[i ? i - 1 : 0];
                 const Pixel& b = r[i];
                 const Pixel& c = r[std::min(i + 1, r.size() - 1)];
                 auto boost = [](float x, float y, float z) { return std::max(y + 0.5f * (y - (x + y + z) / 3.0f), 0.0f); };
                 out.push_back({boost(a.r, b.r, c.r), boost(a.g, b.g, c.g), boost(a.b, b.b, c.b), 1});
             }
             return out;
         }},
        {"everything +10%", [](const Row& r) { return EachPixel(r, [](const Pixel& p) { return Pixel {p.r * 1.1f, p.g * 1.1f, p.b * 1.1f, 1}; }); }},
        {"colour shift (green x0.8)",
         [](const Row& r) { return EachPixel(r, [](const Pixel& p) { return Pixel {p.r, p.g * 0.8f, p.b, 1}; }); }},
    };

    struct Curve { uint32_t mode; uint32_t passthrough; const char* name; };
    const Curve curves[] = {{0, 0, "soft knee"}, {5, 0, "HLG"}, {0, 1, "passthrough"}};
    struct Size { UINT pixels; const char* name; };
    const Size sizes[] = {{32, "50%"}, {42, "65.6%"}};
    const char* modeNames[3] = {"Classic", "Matched", "NVIDIA"};

    // Totals per curve, size and mode, over every scene and edit; edge/flat pooled per pixel. baseEdge/baseFlat: the
    // reference's NVIDIA residual on the same inputs.
    double totalErr[3][2][3] = {}, worstHalo[3][2][3] = {}, totalCarried[3][2][3] = {}, totalEdge[3][2][3] = {},
           totalFlat[3][2][3] = {}, baseEdge[3][2] = {}, baseFlat[3][2] = {};
    int edgePixels[3][2] = {}, flatPixels[3][2] = {};
    int cases = 0, identical = 0;

    for (int ci = 0; ci < 3; ++ci)
        for (int si = 0; si < 2; ++si)
        {
            const Curve& curve = curves[ci];
            const Size& size = sizes[si];
            std::printf("\n== %s (ReversibleMode %u, Passthrough %u), model at %s\n", curve.name, curve.mode, curve.passthrough,
                        size.name);
            std::printf("%-8s %-27s %-8s %8s %8s %4s %8s %8s %8s %8s\n", "scene", "edit", "mode", "err", "halo", "at", "carried",
                        "hue", "edge", "flat");
            for (const Scene& scene : scenes)
                for (const NamedEdit& edit : edits)
                {
                    const Row& frame = scene.row;
                    const Row answer = edit.edit(frame);
                    const UINT n = (UINT) frame.size();
                    const float workScale = (float) size.pixels / (float) n;
                    const std::vector<bool> band = EdgeBand(frame);
                    const Encoded f = Encode(gpu, shader.Get(), frame, curve.mode, curve.passthrough, size.pixels);
                    const Encoded a = Encode(gpu, shader.Get(), answer, curve.mode, curve.passthrough, size.pixels);
                    const Row full = Resolve(gpu, shader.Get(), f.proxy.Get(), a.proxy.Get(), f.keep.Get(), n, curve.mode,
                                             curve.passthrough, 0, 1.0f);
                    expect(Finite(full), std::string("reference not finite: ") + scene.name + ", " + edit.name);
                    Encoded rf, ra;
                    if (reference)
                    {
                        rf = Encode(gpu, reference.Get(), frame, curve.mode, curve.passthrough, size.pixels);
                        ra = Encode(gpu, reference.Get(), answer, curve.mode, curve.passthrough, size.pixels);
                    }

                    // Mean |Y - Y_full| / max(Y_full, 0.005) over the edge band and the rest, as sums for pooling.
                    auto bands = [&](const Row& got, double& edgeSum, double& flatSum)
                    {
                        edgeSum = flatSum = 0.0;
                        for (UINT i = 0; i < n; ++i)
                        {
                            const double e = std::abs(Luma(got[i]) - Luma(full[i])) / std::max(Luma(full[i]), 0.005f);
                            (band[i] ? edgeSum : flatSum) += e;
                        }
                    };

                    for (uint32_t transfer = 0; transfer < 3; ++transfer)
                    {
                        const Row got = Resolve(gpu, shader.Get(), f.reduced.Get(), a.reduced.Get(), f.keep.Get(), n, curve.mode,
                                                curve.passthrough, transfer, workScale);
                        const std::string label = std::string(scene.name) + ", " + edit.name + ", " + modeNames[transfer] + ", " +
                                                  curve.name + ", " + size.name;
                        expect(Finite(got), "not finite: " + label);

                        if (reference && transfer == 2)
                        {
                            double e, fl;
                            bands(Resolve(gpu, reference.Get(), rf.reduced.Get(), ra.reduced.Get(), rf.keep.Get(), n, curve.mode,
                                          curve.passthrough, 2, workScale),
                                  e, fl);
                            baseEdge[ci][si] += e;
                            baseFlat[ci][si] += fl;
                        }

                        double err = 0.0, moved = 0.0, wanted = 0.0, edgeErr = 0.0, flatErr = 0.0;
                        int edgeCount = 0, flatCount = 0;
                        float halo = 0.0f, hue = 0.0f;
                        int haloAt = -1;
                        bands(got, edgeErr, flatErr);
                        for (UINT i = 0; i < n; ++i)
                        {
                            const float y = Luma(got[i]), yRef = Luma(full[i]), yFrame = Luma(frame[i]);
                            const float scale = std::max(yRef, 0.005f);
                            err += std::abs(y - yRef) / scale;
                            ++(band[i] ? edgeCount : flatCount);
                            moved += std::abs(y - yFrame);
                            wanted += std::abs(yRef - yFrame);
                            if (yFrame < 0.1f && (y - yRef) / scale > halo)
                            {
                                halo = (y - yRef) / scale;
                                haloAt = (int) i;
                            }
                            if (std::abs(yRef - yFrame) > 1e-4f || std::abs(Luma(answer[i]) - yFrame) > 1e-4f ||
                                std::abs(answer[i].g - frame[i].g) > 1e-4f)
                            {
                                const float yy = std::max(y, 1e-4f), yr = std::max(yRef, 1e-4f);
                                hue = std::max({hue, std::abs(got[i].r / yy - full[i].r / yr), std::abs(got[i].g / yy - full[i].g / yr),
                                                std::abs(got[i].b / yy - full[i].b / yr)});
                            }
                        }
                        err /= n;
                        totalEdge[ci][si][transfer] += edgeErr;
                        totalFlat[ci][si][transfer] += flatErr;
                        if (transfer == 0)
                        {
                            edgePixels[ci][si] += edgeCount;
                            flatPixels[ci][si] += flatCount;
                        }
                        const double carried = wanted > 1e-6 ? moved / wanted : 1.0;
                        std::printf("%-8s %-27s %-8s %8.4f %8.4f %4d %8.3f %8.4f %8.4f %8.4f\n", scene.name, edit.name, modeNames[transfer],
                                    err, halo, haloAt, carried, hue, edgeCount ? edgeErr / edgeCount : 0.0,
                                    flatCount ? flatErr / flatCount : 0.0);
                        totalErr[ci][si][transfer] += err;
                        worstHalo[ci][si][transfer] = std::max(worstHalo[ci][si][transfer], (double) halo);
                        totalCarried[ci][si][transfer] += std::abs(carried - 1.0);
                    }
                    if (ci == 0 && si == 0)
                        ++cases;
                }
        }

    std::printf("\n== Summary over %d scene x edit cases (lower is better)\n", cases);
    std::printf("%-11s %-6s %-8s %10s %10s %14s %10s %10s\n", "curve", "size", "mode", "mean err", "worst halo", "mean |carried-1|",
                "edge", "flat");
    for (int ci = 0; ci < 3; ++ci)
        for (int si = 0; si < 2; ++si)
            for (int t = 0; t < 3; ++t)
                std::printf("%-11s %-6s %-8s %10.4f %10.4f %14.4f %10.5f %10.5f\n", curves[ci].name, sizes[si].name, modeNames[t],
                            totalErr[ci][si][t] / cases, worstHalo[ci][si][t], totalCarried[ci][si][t] / cases,
                            totalEdge[ci][si][t] / edgePixels[ci][si], totalFlat[ci][si][t] / flatPixels[ci][si]);

    std::printf("\n== NVIDIA residual against the %s bilinear baseline (edge must be < %.2f, flat <= %.2f)\n",
                reference ? "reference shader's" : "recorded", kEdgeLimit, kFlatLimit);
    for (int ci = 0; ci < 3; ++ci)
        for (int si = 0; si < 2; ++si)
        {
            const double edge = totalEdge[ci][si][2] / edgePixels[ci][si], flat = totalFlat[ci][si][2] / flatPixels[ci][si];
            const double bEdge = reference ? baseEdge[ci][si] / edgePixels[ci][si] : kBilinear[ci][si][0];
            const double bFlat = reference ? baseFlat[ci][si] / flatPixels[ci][si] : kBilinear[ci][si][1];
            const std::string where = std::string(curves[ci].name) + ", " + sizes[si].name;
            std::printf("%-11s %-6s edge %.5f / %.5f = %.3f   flat %.5f / %.5f = %.3f\n", curves[ci].name, sizes[si].name, edge, bEdge,
                        edge / bEdge, flat, bFlat, flat / bFlat);
            expect(edge < bEdge * kEdgeLimit, "NVIDIA residual edge err not below the limit: " + where);
            expect(flat <= bFlat * kFlatLimit + 1e-6, "NVIDIA residual flat err above the limit: " + where);
        }

    // Bit-identity with the reference where the change must not reach: Classic and Matched below full size, every mode
    // at full size, and NVIDIA residual after SGSR1 (the answer already at the frame's size, the proxy reduced) -- over
    // every proxy curve and passthrough, on the first and the last scene.
    if (reference)
    {
        for (uint32_t pass = 0; pass < 2; ++pass)
            for (uint32_t mode = 0; mode < (pass ? 1u : 8u); ++mode)
                for (const Size& size : sizes)
                    for (const Scene* scene : {&scenes[0], &scenes[5]})
                        for (const NamedEdit& edit : edits)
                        {
                            const Row& frame = scene->row;
                            const Row answer = edit.edit(frame);
                            const UINT n = (UINT) frame.size();
                            const float workScale = (float) size.pixels / (float) n;
                            const Encoded f = Encode(gpu, shader.Get(), frame, mode, pass, size.pixels);
                            const Encoded a = Encode(gpu, shader.Get(), answer, mode, pass, size.pixels);
                            const Encoded rf = Encode(gpu, reference.Get(), frame, mode, pass, size.pixels);
                            const Encoded ra = Encode(gpu, reference.Get(), answer, mode, pass, size.pixels);
                            const std::string label = std::string(scene->name) + ", " + edit.name + ", ReversibleMode " +
                                                      std::to_string(mode) + ", Passthrough " + std::to_string(pass) + ", " + size.name;
                            auto same = [&](ID3D11Texture2D* p, ID3D11Texture2D* m, ID3D11Texture2D* rp, ID3D11Texture2D* rm,
                                            uint32_t transfer, float scale, const char* what)
                            {
                                expect(SameBits(Resolve(gpu, shader.Get(), p, m, f.keep.Get(), n, mode, pass, transfer, scale),
                                                Resolve(gpu, reference.Get(), rp, rm, rf.keep.Get(), n, mode, pass, transfer, scale)),
                                       std::string("not bit-identical to the reference (") + what + "): " + label);
                                ++identical;
                            };
                            same(f.reduced.Get(), a.reduced.Get(), rf.reduced.Get(), ra.reduced.Get(), 0, workScale, "Classic");
                            same(f.reduced.Get(), a.reduced.Get(), rf.reduced.Get(), ra.reduced.Get(), 1, workScale, "Matched");
                            same(f.reduced.Get(), a.proxy.Get(), rf.reduced.Get(), ra.proxy.Get(), 2, workScale, "NVIDIA after SGSR1");
                            for (uint32_t transfer = 0; transfer < 3; ++transfer)
                                same(f.proxy.Get(), a.proxy.Get(), rf.proxy.Get(), ra.proxy.Get(), transfer, 1.0f, "full size");
                        }
    }

    if (fails)
    {
        std::printf("%d check(s) failed\n", fails);
        return 1;
    }
    std::printf("PASS: every mode finite; NVIDIA residual edges below %.2f x bilinear, flat within %.2f x", kEdgeLimit, kFlatLimit);
    if (reference)
        std::printf("; %d resolves bit-identical to the reference", identical);
    std::printf(" (%s)\n", warp ? "WARP" : "hardware GPU");
    return 0;
}
catch (const std::exception& e)
{
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
}
