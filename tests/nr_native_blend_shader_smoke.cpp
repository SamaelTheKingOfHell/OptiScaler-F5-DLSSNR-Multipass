// Headless shader test: Windows D3D11 WARP runs dlssnr.hlsl's resolve for what Native blend relies on (the Neural
// Rendering page's "Native blend", dlssnr/DlssNr_NativeBlend.h). On a tone-mapped (SDR) frame with Replace, skin
// protection off and Replace detail 0, the frame the pass writes is the model's answer bit for bit: the white point
// divide and multiply-back (normScale) is exactly 1.0 there, so a white point that would rescale an HDR frame changes
// nothing. No game and no NVIDIA model: the answer is a texture the test fills.
//
//   1. SDR passthrough, Replace (curves 2 and 4), skin protection off, detail 0: output == model, at white points 1 and
//      4, for every transfer the shared shader knows, with an original that runs well past 1.
//   2. The same with Replace detail on but no reduced model (work scale 1): still the model's answer.
//   3. Sensitivity: each thing Native blend turns off does change the picture when on, and so does an HDR frame
//      (not passthrough) with a white point, so the checks above could have failed.
//
// Build: cl /nologo /std:c++20 /EHsc /W3 tests\nr_native_blend_shader_smoke.cpp /link d3d11.lib d3dcompiler.lib
// Run:   nr_native_blend_shader_smoke.exe OptiScaler\shaders\dlssnr\precompile\dlssnr.hlsl
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
#include "../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
using Microsoft::WRL::ComPtr;

struct Pixel { float r, g, b, a; };
constexpr int N = 6;
using Row = std::array<Pixel, N>;

static void check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D call failed"); }
static int fails = 0;
static void expect(bool ok, const std::string& label) { if (!ok) { std::printf("FAIL: %s\n", label.c_str()); ++fails; } }

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
        // WARP crashes on the percentile meter's branch (since 507c9404); this test does not use the meter.
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

    // One dispatch: t0 proxy, t1 model, t2 original, u0 target, u1 keep.
    void Run(ID3D11ComputeShader* shader, const DlssNrConstants& settings, ID3D11Texture2D* t0, ID3D11Texture2D* t1,
             ID3D11Texture2D* t2, ID3D11Texture2D* u0, ID3D11Texture2D* u1)
    {
        ComPtr<ID3D11ShaderResourceView> s0, s1, s2;
        check(device->CreateShaderResourceView(t0, nullptr, &s0));
        check(device->CreateShaderResourceView(t1, nullptr, &s1));
        check(device->CreateShaderResourceView(t2, nullptr, &s2));
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
        ctx->CSSetSamplers(0, 1, sampler.GetAddressOf());
        ctx->Dispatch(1, 1, 1);
        ID3D11ShaderResourceView* noSrvs[5] = {};
        ID3D11UnorderedAccessView* noUavs[2] = {};
        ctx->CSSetShaderResources(0, 5, noSrvs);
        ctx->CSSetUnorderedAccessViews(0, 2, noUavs, nullptr);
    }
};

struct Case
{
    uint32_t passthrough = 1;
    uint32_t curve = 2;          // ReversibleMode
    uint32_t transfer = 0;
    float whitePoint = 1.0f;
    bool skinProtection = false;
    float replaceDetail = 0.0f;
    float workScale = 1.0f;
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
    k.Transfer = c.transfer;
    k.DebugScale = 1.0f;
    k.ModelWorkScale = c.workScale;
    k.PassFeedback = 1.0f;
    k.SkinProtection = c.skinProtection ? 1u : 0u;
    k.SkinDetail = 0.5f; k.SkinColour = 0.5f; k.EnvironmentDetail = 0.5f; k.EnvironmentColour = 0.5f;
    k.ReplaceDetailStrength = c.replaceDetail;
    return k;
}

// The frame the resolve writes, for this model answer and original.
static Row Resolve(Gpu& gpu, ID3D11ComputeShader* shader, const Row& proxy, const Row& model, const Row& original, const Case& c)
{
    auto p = gpu.Texture(&proxy, false);
    auto m = gpu.Texture(&model, false);
    auto o = gpu.Texture(&original, false);
    auto target = gpu.Texture(nullptr, true);
    auto spare = gpu.Texture(nullptr, true);
    gpu.Run(shader, Constants(c), p.Get(), m.Get(), o.Get(), target.Get(), spare.Get());
    return gpu.Read(target.Get());
}

static bool SameBits(const Row& a, const Row& b) { return std::memcmp(a.data(), b.data(), sizeof(Row)) == 0; }

// The colour channels only: the pass writes the original's alpha.
static bool SameColour(const Row& a, const Row& b)
{
    for (int i = 0; i < N; ++i)
        if (std::memcmp(&a[i], &b[i], 3 * sizeof(float)) != 0)
            return false;
    return true;
}

int wmain(int argc, wchar_t** argv) try
{
    if (argc != 2) throw std::runtime_error("Pass <dlssnr.hlsl>");
    Gpu gpu;
    auto shader = gpu.Compile(argv[1]);

    // A model answer in 0..1 (an SDR picture), a different proxy, and an original that runs well past 1 and differs
    // from the answer everywhere, so a stray scale, ratio or blend toward the original cannot hide.
    const Row model {{ {0.02f, 0.03f, 0.05f, 1}, {0.2f, 0.25f, 0.3f, 1}, {0.5f, 0.45f, 0.4f, 1},
                       {0.9f, 0.85f, 0.8f, 1}, {0.8f, 0.3f, 0.1f, 1}, {0.1f, 0.4f, 0.7f, 1} }};
    const Row proxy {{ {0.03f, 0.03f, 0.04f, 1}, {0.22f, 0.22f, 0.22f, 1}, {0.45f, 0.45f, 0.45f, 1},
                       {0.85f, 0.85f, 0.85f, 1}, {0.7f, 0.35f, 0.15f, 1}, {0.15f, 0.35f, 0.6f, 1} }};
    const Row original {{ {0.05f, 0.04f, 0.04f, 1}, {0.6f, 0.5f, 0.4f, 1}, {1.5f, 1.2f, 0.9f, 1},
                          {3.0f, 2.5f, 2.0f, 1}, {2.0f, 0.5f, 0.2f, 1}, {0.4f, 1.1f, 2.2f, 1} }};

    // 1. SDR passthrough with Replace, nothing else of ours: the model's answer, exactly.
    for (uint32_t curve : {2u, 4u})
        for (uint32_t transfer : {0u, 1u, 2u})
            for (float whitePoint : {1.0f, 4.0f})
            {
                Case c;
                c.curve = curve;
                c.transfer = transfer;
                c.whitePoint = whitePoint;
                const Row got = Resolve(gpu, shader.Get(), proxy, model, original, c);
                const std::string what = " (curve " + std::to_string(curve) + ", transfer " + std::to_string(transfer) +
                                         ", white point " + std::to_string((int) whitePoint) + ")";
                expect(SameColour(got, model), "output is not the model's answer bit for bit" + what);
                expect(got[0].a == original[0].a, "alpha is not the original's" + what);
            }

    // 2. Replace detail on, but the model did not run reduced: nothing is put back, so still the answer.
    {
        Case c;
        c.replaceDetail = 0.5f;
        c.workScale = 1.0f;
        expect(SameColour(Resolve(gpu, shader.Get(), proxy, model, original, c), model),
               "Replace detail changes the picture although the model ran at full size");
    }

    // 3. Sensitivity: what Native blend turns off does act when on, so the checks above can fail.
    {
        Case reduced;
        reduced.replaceDetail = 0.5f;
        reduced.workScale = 0.5f;
        expect(!SameColour(Resolve(gpu, shader.Get(), proxy, model, original, reduced), model),
               "sensitivity: Replace detail with a reduced model should change the picture");

        Case skin;
        skin.skinProtection = true;
        expect(!SameColour(Resolve(gpu, shader.Get(), proxy, model, original, skin), model),
               "sensitivity: skin protection should change the picture");

        Case composed;
        composed.curve = 1;
        expect(!SameColour(Resolve(gpu, shader.Get(), proxy, model, original, composed), model),
               "sensitivity: a composed curve should change the picture");

        // An HDR frame (not passthrough) is divided by the white point, run through the curve and multiplied back, so it
        // is not the model's answer: Native blend is not a true native match there.
        Case hdr;
        hdr.passthrough = 0;
        hdr.whitePoint = 4.0f;
        expect(!SameColour(Resolve(gpu, shader.Get(), proxy, model, original, hdr), model),
               "sensitivity: an HDR frame should not give the model's answer");
    }

    if (fails)
    {
        std::printf("%d check(s) failed\n", fails);
        return 1;
    }
    std::puts("PASS: SDR passthrough + Replace + skin protection off + detail 0 writes the model's answer bit for bit "
              "(WARP HLSL)");
    return 0;
}
catch (const std::exception& e)
{
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
}
