// GPU test for native::NativeProducer (OptiScaler/native/NativeProducer.cpp), no game and no model: a fake frame source
// hands it pictures and depth copies through the FrameContract, and a stand-in for DLSS-NR is passed in as a function.
// It checks, through the contract only:
//   - the first frame has no flow, later frames do; the trust mask runs with or without depth,
//   - the stand-in for NR is called with the guides (depth null when there was none) and the trust mask, and not called
//     when apply is off,
//   - the picture is left exactly as it was (bit for bit) when NR does nothing, whatever state it came in,
//   - the output's fence point completes, and a hard cut is reported and makes the next NR call a reset,
//   - a cut hint from the adapter restarts the flow,
//   - the debug views (PR #232): nothing is made for them until one is asked for; each of the seven draws a picture;
//     the depth view's look (multiplier, linearization) leaves the depth the consumer gets alone, and the depth
//     alignment moves that depth the way the menu says (DepthAlign_Dx12.h).
//
//   vcvars64, then from the repo root:
//   cl /std:c++20 /EHsc /O2 tests\nr_native_producer_gpu.cpp OptiScaler\native\NativeProducer.cpp
//   OptiScaler\motion\OpticalFlow_Dx12.cpp OptiScaler\motion\SceneCut_Dx12.cpp OptiScaler\motion\TrustMask_Dx12.cpp
//   d3d12.lib dxgi.lib d3dcompiler.lib

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "../OptiScaler/native/NativeProducer.h"

using Microsoft::WRL::ComPtr;

namespace
{

constexpr uint32_t kWidth = 1280;
constexpr uint32_t kHeight = 720;

float Hash(int x, int y, int seed)
{
    uint32_t h = (uint32_t) x * 374761393u + (uint32_t) y * 668265263u + (uint32_t) seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (h & 0xFFFFFF) / 16777215.0f;
}

float ValueNoise(float x, float y, float scale, int seed)
{
    const float fx = x / scale, fy = y / scale;
    const int ix = (int) std::floor(fx), iy = (int) std::floor(fy);
    const float tx = fx - ix, ty = fy - iy;
    const float sx = tx * tx * (3 - 2 * tx), sy = ty * ty * (3 - 2 * ty);
    const float a = Hash(ix, iy, seed), b = Hash(ix + 1, iy, seed), c = Hash(ix, iy + 1, seed),
                d = Hash(ix + 1, iy + 1, seed);
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
}

float Texture(float x, float y, int channel, int seed)
{
    return 0.45f * ValueNoise(x, y, 64, seed + channel) + 0.25f * ValueNoise(x, y, 24, seed + 20 + channel) +
           0.18f * ValueNoise(x, y, 9, seed + 40 + channel) + 0.12f * ValueNoise(x, y, 4, seed + 60 + channel);
}

struct Gpu
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    UINT64 fenceValue = 0;

    bool Init()
    {
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            return false;

        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
                SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
            {
                wprintf(L"adapter: %s\n", desc.Description);
                break;
            }
        }

        if (!device)
            return false;

        D3D12_COMMAND_QUEUE_DESC qd {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&list))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
            return false;

        event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        return true;
    }

    void Submit()
    {
        list->Close();
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence.Get(), ++fenceValue);
        fence->SetEventOnCompletion(fenceValue, event);
        WaitForSingleObject(event, INFINITE);
        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
    }

    ComPtr<ID3D12Resource> Buffer(UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state)
    {
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = type;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r;
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&r));
        return r;
    }

    // A texture filled by fill(x, y, pixel), left in the non-pixel shader resource state.
    ComPtr<ID3D12Resource> Upload(DXGI_FORMAT format, uint32_t bytesPerPixel,
                                  const std::function<void(uint32_t, uint32_t, uint8_t*)>& fill)
    {
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        ComPtr<ID3D12Resource> tex;
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(&tex));

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
        UINT64 total = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
        auto upload = Buffer(total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

        uint8_t* data = nullptr;
        upload->Map(0, nullptr, (void**) &data);
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
                fill(x, y, data + (size_t) y * fp.Footprint.RowPitch + (size_t) x * bytesPerPixel);
        upload->Unmap(0, nullptr);

        D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
        dst.pResource = tex.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = fp;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &b);
        Submit();
        return tex;
    }

    // The mask (R8) as bytes, tightly packed.
    std::vector<uint8_t> ReadMask(ID3D12Resource* tex)
    {
        const auto desc = tex->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
        UINT64 total = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
        auto readback = Buffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &b);

        D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        src.pResource = tex;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        list->ResourceBarrier(1, &b);
        Submit();

        std::vector<uint8_t> out((size_t) desc.Width * desc.Height);
        uint8_t* data = nullptr;
        readback->Map(0, nullptr, (void**) &data);
        for (uint32_t y = 0; y < desc.Height; ++y)
            memcpy(&out[(size_t) y * desc.Width], data + (size_t) y * fp.Footprint.RowPitch, desc.Width);
        readback->Unmap(0, nullptr);
        return out;
    }
};

// A scene: a still background and a square at x, with a depth map where near is 1 (reversed-Z, near plane 0.1).
struct Scene
{
    int seed;
    float squareX;       // the square's left edge; negative: none
    float backgroundZ;
    float squareZ;
    float depthShift = 0; // the depth map's square is this many pixels off the picture's (a jittered depth buffer)
    bool darkTop = false; // the top half is dark: another layout of light, which the flow's cut detector can tell
};

bool InSquare(const Scene& s, uint32_t x, uint32_t y, float shift = 0)
{
    return s.squareX >= 0 && x >= s.squareX + shift && x < s.squareX + shift + 200 && y >= 260 && y < 460;
}

ComPtr<ID3D12Resource> Colour(Gpu& gpu, const Scene& s)
{
    return gpu.Upload(DXGI_FORMAT_R8G8B8A8_UNORM, 4,
                      [&](uint32_t x, uint32_t y, uint8_t* px)
                      {
                          const bool in = InSquare(s, x, y);
                          for (int c = 0; c < 3; ++c)
                          {
                              const float v = (in ? Texture(x - s.squareX, (float) y, c, s.seed + 500)
                                                  : Texture((float) x, (float) y, c, s.seed)) *
                                              (s.darkTop && y < kHeight / 2 ? 0.15f : 1.0f);
                              px[c] = (uint8_t) std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f);
                          }
                          px[3] = 255;
                      });
}

ComPtr<ID3D12Resource> Depth(Gpu& gpu, const Scene& s, int mode = 0)
{
    return gpu.Upload(DXGI_FORMAT_R32_FLOAT, 4,
                      [&](uint32_t x, uint32_t y, uint8_t* px)
                      {
                          const bool square = InSquare(s, x, y, s.depthShift);
                          float d = 0.1f / (square ? s.squareZ : s.backgroundZ);

                          // A copy holding only part of the scene has nothing (far, 0 when reversed) elsewhere.
                          if ((mode == 1 && !square) || (mode == 2 && square))
                              d = 0.0f;

                          memcpy(px, &d, 4);
                      });
}


void Change(Gpu& gpu, ID3D12Resource* tex, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    gpu.list->ResourceBarrier(1, &b);
}

// An RGBA8 texture in the given state as bytes, tightly packed.
std::vector<uint8_t> ReadRgba(Gpu& gpu, ID3D12Resource* tex, D3D12_RESOURCE_STATES state)
{
    const auto desc = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
    UINT64 total = 0;
    gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
    auto readback = gpu.Buffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

    Change(gpu, tex, state, D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    src.pResource = tex;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    Change(gpu, tex, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    gpu.Submit();

    std::vector<uint8_t> out((size_t) desc.Width * desc.Height * 4);
    uint8_t* data = nullptr;
    readback->Map(0, nullptr, (void**) &data);
    for (uint32_t y = 0; y < desc.Height; ++y)
        memcpy(&out[(size_t) y * desc.Width * 4], data + (size_t) y * fp.Footprint.RowPitch, (size_t) desc.Width * 4);
    readback->Unmap(0, nullptr);
    return out;
}

// An R8 texture in the given state as bytes, tightly packed.
std::vector<uint8_t> ReadR8(Gpu& gpu, ID3D12Resource* tex, D3D12_RESOURCE_STATES state)
{
    const auto desc = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
    UINT64 total = 0;
    gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
    auto readback = gpu.Buffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

    Change(gpu, tex, state, D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    src.pResource = tex;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    Change(gpu, tex, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    gpu.Submit();

    std::vector<uint8_t> out((size_t) desc.Width * desc.Height);
    uint8_t* data = nullptr;
    readback->Map(0, nullptr, (void**) &data);
    for (uint32_t y = 0; y < desc.Height; ++y)
        memcpy(&out[(size_t) y * desc.Width], data + (size_t) y * fp.Footprint.RowPitch, desc.Width);
    readback->Unmap(0, nullptr);
    return out;
}

// An R32_FLOAT texture in the given state as floats, tightly packed.
std::vector<float> ReadFloats(Gpu& gpu, ID3D12Resource* tex, D3D12_RESOURCE_STATES state)
{
    const auto desc = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
    UINT64 total = 0;
    gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
    auto readback = gpu.Buffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

    Change(gpu, tex, state, D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    src.pResource = tex;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    Change(gpu, tex, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    gpu.Submit();

    std::vector<float> out((size_t) desc.Width * desc.Height);
    uint8_t* data = nullptr;
    readback->Map(0, nullptr, (void**) &data);
    for (uint32_t y = 0; y < desc.Height; ++y)
        memcpy(&out[(size_t) y * desc.Width], data + (size_t) y * fp.Footprint.RowPitch, (size_t) desc.Width * 4);
    readback->Unmap(0, nullptr);
    return out;
}

bool Check(const char* what, bool value)
{
    printf("  %-80s %s\n", what, value ? "ok" : "FAIL");
    return value;
}

// What the stand-in for DLSS-NR saw.
struct NrCalls
{
    int count = 0;
    bool reversed = false;
    bool reset = false;
    native::ColorSpace space = native::ColorSpace::Srgb;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool guidesSeen = false;       // depth and motion both present
    bool motionOnlyGuides = false; // motion present, depth null (no depth this frame)
    bool distrustIsMask = false;   // the history distrust handed over is this frame's trust mask
};

} // namespace

int main()
{
    Gpu gpu;

    if (!gpu.Init())
    {
        printf("no D3D12 device\n");
        return 2;
    }

    native::NativeProducer producer;

    if (!producer.Init(gpu.device.Get()))
    {
        printf("init failed: %s\n", producer.Error().c_str());
        return 1;
    }

    NrCalls nr;
    constexpr D3D12_RESOURCE_STATES kReadState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const native::NativeProducer::ApplyFn applyNr = [&](ID3D12GraphicsCommandList*, const native::NativeFrame& f)
    {
        ++nr.count;
        nr.reversed = f.depthReversed;
        nr.reset = f.reset;
        nr.space = f.space;
        nr.state = f.pictureState;
        nr.format = f.colorFormat;
        nr.guidesSeen = f.depth != nullptr && f.motion != nullptr;
        nr.motionOnlyGuides = f.depth == nullptr && f.motion != nullptr;
        nr.distrustIsMask = f.historyDistrust != nullptr && f.historyDistrust == producer.Trust()->Mask();
        return true;
    };

    bool ok = true;
    int extraCopies = 0; // more copies of the depth than the one: a game that splits the scene over several lists

    // One frame through the contract: a picture of the scene (in the state given), with or without depth.
    native::NativeProducer::Options extra; // the debug view and the depth alignment, for the frames below
    std::vector<float> guideDepth;          // the consumer's depth guide of the last frame, when asked for
    bool readGuideDepth = false;
    ID3D12CommandQueue* runQueue = nullptr; // the queue a frame runs on (null: gpu.queue)
    bool waitDone = true;                   // wait for the frame's work before returning
    std::vector<ComPtr<ID3D12Resource>> keep; // a frame's textures, alive while its work may still run

    auto frame = [&](const Scene& s, D3D12_RESOURCE_STATES state, bool withDepth, bool applyOn, bool cutHint,
                     native::FrameOutput& output, std::vector<uint8_t>* before, std::vector<uint8_t>* after)
    {
        auto picture = Colour(gpu, s);
        auto depth = Depth(gpu, s);

        if (state != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
        {
            Change(gpu, picture.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, state);
            gpu.Submit();
        }

        if (before)
            *before = ReadRgba(gpu, picture.Get(), state);

        native::FrameInput input;
        input.api = native::Api::D3D12;
        input.picture = picture.Get();
        input.pictureFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        input.pictureState = state;
        input.colorSpace = native::ColorSpace::Srgb;
        input.width = kWidth;
        input.height = kHeight;
        input.cutHint = cutHint;

        if (withDepth)
        {
            input.depth[0] = depth.Get();
            input.depthCount = 1 + extraCopies;
            for (int i = 1; i < input.depthCount; ++i)
                input.depth[i] = depth.Get();
            input.depthView = DXGI_FORMAT_R32_FLOAT;
            input.depthWidth = kWidth;
            input.depthHeight = kHeight;
            input.depthReversed = true;
        }

        native::NativeProducer::Options options = extra;
        options.apply = applyOn;

        const auto result =
            producer.Run(runQueue != nullptr ? runQueue : gpu.queue.Get(), input, options, applyNr, output);

        if (!waitDone)
        {
            keep.push_back(picture);
            keep.push_back(depth);
            return result;
        }

        // Wait for the producer's work the way an adapter would before presenting.
        if (output.done.fence != nullptr)
        {
            output.done.fence->SetEventOnCompletion(output.done.value, gpu.event);
            WaitForSingleObject(gpu.event, 5000);
        }

        if (after)
            *after = ReadRgba(gpu, picture.Get(), state);

        if (readGuideDepth)
            guideDepth = producer.Trust()->GuideDepth() != nullptr
                             ? ReadFloats(gpu, producer.Trust()->GuideDepth(), kReadState)
                             : std::vector<float> {};

        return result;
    };

    const D3D12_RESOURCE_STATES kRead = kReadState;
    const D3D12_RESOURCE_STATES kPresent = D3D12_RESOURCE_STATE_PRESENT;
    native::FrameOutput output;

    printf("frames through the contract, picture in a read state\n");
    {
        auto r = frame(Scene { 7, 300, 20.0f, 5.0f }, kRead, true, true, false, output, nullptr, nullptr);
        ok &= Check("first frame: submitted, no flow yet, NR not called", r.submitted && !r.flowValid && nr.count == 0);

        std::vector<uint8_t> before, after;

        for (int k = 0; k < 6; ++k)
            r = frame(Scene { 7, 300.0f + 12.0f * (k + 1), 20.0f, 5.0f }, kRead, true, true, false, output, &before, &after);

        ok &= Check("later frame: flow valid and the trust mask ran", r.flowValid && r.trustRan);
        ok &= Check("NR called on it with the guides", r.nativeRan && nr.count >= 1 && nr.guidesSeen);
        ok &= Check("and with the trust mask as the history distrust", nr.distrustIsMask);
        ok &= Check("... with reversed-Z, sRGB and the picture's state", nr.reversed && nr.space == native::ColorSpace::Srgb &&
                                                                     nr.state == kRead);
        ok &= Check("... and the picture's typed format", nr.format == DXGI_FORMAT_R8G8B8A8_UNORM);
        ok &= Check("the output reports it was processed", output.processed && output.done.fence != nullptr);
        ok &= Check("the picture is bit-identical after (NR did nothing)", before == after);
        ok &= Check("no debug view asked for: no picture made for one",
                    producer.Flow()->Preview() == nullptr && !producer.PreviewReady());
    }

    printf("picture in the present state (a back buffer)\n");
    {
        std::vector<uint8_t> before, after;
        auto r = frame(Scene { 7, 560, 20.0f, 5.0f }, kPresent, true, true, false, output, &before, &after);
        ok &= Check("flow valid, NR called with the PRESENT state", r.flowValid && r.nativeRan && nr.state == kPresent);
        ok &= Check("the picture is bit-identical after", before == after);
    }

    printf("no depth, or NR off\n");
    {
        // No depth this frame (the generic depth finder found nothing qualifying, say): the whole frame used to be
        // dropped (trustRan/nativeRan both false). It now still runs on motion alone -- a flow-only guide -- and NR
        // is still called, just without a depth guide.
        const int calls = nr.count;
        auto r = frame(Scene { 7, 572, 20.0f, 5.0f }, kRead, false, true, false, output, nullptr, nullptr);
        ok &= Check("no depth: flow runs, the mask still runs, NR still called with motion only",
                    r.flowValid && r.trustRan && r.nativeRan && nr.count == calls + 1);
        ok &= Check("... with a motion guide and no depth guide", nr.motionOnlyGuides);

        r = frame(Scene { 7, 584, 20.0f, 5.0f }, kRead, true, false, false, output, nullptr, nullptr);
        ok &= Check("NR off: mask runs, NR not called",
                    r.flowValid && r.trustRan && !r.nativeRan && nr.count == calls + 1);
    }

    printf("GPU timing\n");
    {
        // The frames above have finished (each later frame waited for its slot): their stages are timed
        const uint32_t timed = producer.GpuStats().Frames();
        const std::string report = producer.GpuStats().Report();
        printf("    %s\n", report.c_str());
        ok &= Check("finished frames are timed, by stage", timed > 0 && report.find("motion") != std::string::npos &&
                                                               report.find("consumer") != std::string::npos);
        ok &= Check("the report starts a new window", producer.GpuStats().Frames() == 0);
    }

    printf("depth for the flow\n");
    {
        // The flow matches with depth only when it is one copy of the whole scene: one of several copies holds part of it.
        frame(Scene { 7, 300, 20.0f, 5.0f }, kRead, true, true, false, output, nullptr, nullptr);
        ok &= Check("one depth copy: the flow matches with it", producer.Flow()->UsedDepth());

        extraCopies = 1;
        frame(Scene { 7, 312, 20.0f, 5.0f }, kRead, true, true, false, output, nullptr, nullptr);
        extraCopies = 0;
        ok &= Check("two copies: the flow matches without depth", !producer.Flow()->UsedDepth());

        frame(Scene { 7, 324, 20.0f, 5.0f }, kRead, false, true, false, output, nullptr, nullptr);
        ok &= Check("no depth: the flow matches without depth", !producer.Flow()->UsedDepth());
    }

    printf("a hard cut, found by the flow's own detector\n");
    {
        // Settle on a still scene, then change to another one entirely.
        for (int k = 0; k < 4; ++k)
            frame(Scene { 7, 300, 20.0f, 5.0f }, kRead, true, true, false, output, nullptr, nullptr);

        int reportedAt = -1, resets = 0, nrSkipped = 0, cuts = 0;

        for (int k = 0; k < 10; ++k)
        {
            Scene next { 91, -1, 4.0f, 4.0f };
            next.darkTop = true; // another layout of light: the detector sees it on the cut frame

            const int before = nr.count;
            const auto r = frame(next, kRead, true, true, false, output, nullptr, nullptr);
            nrSkipped += nr.count == before ? 1 : 0;
            resets += nr.count != before && nr.reset ? 1 : 0;
            cuts += r.sceneCut ? 1 : 0;

            if (r.sceneCut && reportedAt < 0)
                reportedAt = k;
        }

        ok &= Check("the cut is reported on the frame after it", reportedAt == 1);
        ok &= Check("one report and one reset of NR for the cut", cuts == 1 && resets == 1);
        ok &= Check("NR runs on every frame through the cut (no frame without it)", nrSkipped == 0);
    }

    printf("a hard cut the detector cannot see (the same layout of light)\n");
    {
        for (int k = 0; k < 8; ++k)
            frame(Scene { 7, 300, 20.0f, 5.0f }, kRead, true, true, false, output, nullptr, nullptr);

        int cuts = 0, resets = 0, nrSkipped = 0;

        for (int k = 0; k < 10; ++k)
        {
            const int before = nr.count;
            cuts += frame(Scene { 91, -1, 4.0f, 4.0f }, kRead, true, true, false, output, nullptr, nullptr).sceneCut;
            nrSkipped += nr.count == before ? 1 : 0;
            resets += nr.count != before && nr.reset ? 1 : 0;
        }

        ok &= Check("the trust mask's count reports it, once, and NR is reset", cuts == 1 && resets == 1);
        ok &= Check("NR runs on every frame through it", nrSkipped == 0);
    }

    printf("a hard cut, without the detector (the trust mask's count only)\n");
    {
        producer.Flow()->Tuning().sceneCutDetector = false;

        // Long enough for the change back to this scene (itself a cut) to be counted and over.
        for (int k = 0; k < 8; ++k)
            frame(Scene { 7, 300, 20.0f, 5.0f }, kRead, true, true, false, output, nullptr, nullptr);

        bool cut = false;

        for (int k = 0; k < 8 && !cut; ++k)
            cut = frame(Scene { 91, -1, 4.0f, 4.0f }, kRead, true, true, false, output, nullptr, nullptr).sceneCut;

        ok &= Check("the cut is reported within a few frames", cut);

        // The frame after the report starts over.
        const auto r = frame(Scene { 91, -1, 4.0f, 4.0f }, kRead, true, true, false, output, nullptr, nullptr);
        ok &= Check("the frame after it has no flow yet (the histories were reset)", !r.flowValid);

        // The next frame NR runs on is a reset (it was skipped on the frames the reset was asked on).
        const int before = nr.count;
        frame(Scene { 91, -1, 4.0f, 4.0f }, kRead, true, true, false, output, nullptr, nullptr);
        ok &= Check("the next NR call is a reset", nr.count != before && nr.reset);
        producer.Flow()->Tuning().sceneCutDetector = true;
    }

    printf("a cut hint from the adapter\n");
    {
        for (int k = 0; k < 3; ++k)
            frame(Scene { 7, 300.0f + 5.0f * k, 20.0f, 5.0f }, kRead, true, true, false, output, nullptr, nullptr);

        const auto r = frame(Scene { 7, 330, 20.0f, 5.0f }, kRead, true, true, true, output, nullptr, nullptr);
        ok &= Check("the flow restarts: no flow on that frame", !r.flowValid);
    }

    printf("the debug views\n");
    {
        static const char* names[] = { "arrows", "normals / depth", "final flow", "confidence", "channels",
                                       "history rejection", "still age" };
        constexpr D3D12_RESOURCE_STATES kShown = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        constexpr D3D12_RESOURCE_STATES kMaskState =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        std::vector<std::vector<uint8_t>> pictures(7);
        extra.flowPreview = true;

        // A square moving 8 pixels a frame, from before the first view on (a still scene would leave the motion views
        // black).
        for (int k = 0; k < 3; ++k)
            frame(Scene { 7, 300.0f + 8.0f * k, 20.0f, 5.0f }, kRead, true, true, false, output, nullptr, nullptr);

        for (int view = 0; view < 7; ++view)
        {
            extra.previewView = view;
            // The views of the guide motion (0, 2) with the consumer off: they build the guides themselves.
            const bool consumer = view != 0 && view != 2;
            const auto r =
                frame(Scene { 7, 324.0f + 8.0f * view, 20.0f, 5.0f }, kRead, true, consumer, false, output, nullptr,
                      nullptr);
            ID3D12Resource* picture = producer.Flow()->Preview();
            bool drawn = r.previewDrawn && picture != nullptr;
            int spread = 0;

            if (drawn)
            {
                const auto desc = picture->GetDesc();
                const bool full = desc.Width == kWidth && desc.Height == kHeight;
                drawn = view == 1 ? full : (desc.Width == (kWidth + 1) / 2 && desc.Height == (kHeight + 1) / 2);
                pictures[view] = ReadRgba(gpu, picture, kShown);
                int lo = 765, hi = 0;

                // Below the key's two lines: the view itself.
                for (size_t i = (size_t) desc.Width * 40 * 4; i + 3 < pictures[view].size(); i += 4 * 7)
                {
                    const int value = (int) pictures[view][i] + pictures[view][i + 1] + pictures[view][i + 2];
                    lo = (std::min)(lo, value);
                    hi = (std::max)(hi, value);
                }

                spread = hi - lo;
            }

            char line[128];
            snprintf(line, sizeof(line), "view %d (%s): drawn this frame at its size, not flat (spread %d)", view,
                     names[view], spread);
            ok &= Check(line, drawn && spread >= 20);

            // History rejection is the trust mask itself, grey for grey (the same size, sampled at its texel centres).
            if (view == 5 && drawn)
            {
                const auto mask = ReadR8(gpu, producer.Trust()->Mask(), kMaskState);
                const auto desc = picture->GetDesc();
                int worst = 0;

                for (uint32_t y = 40; y < desc.Height; ++y)
                    for (uint32_t x = 0; x < desc.Width; ++x)
                        worst = (std::max)(worst, std::abs((int) pictures[5][((size_t) y * desc.Width + x) * 4] -
                                                           (int) mask[(size_t) y * desc.Width + x]));

                snprintf(line, sizeof(line), "history rejection shows the trust mask (largest difference %d)", worst);
                ok &= Check(line, mask.size() == (size_t) desc.Width * desc.Height && worst <= 1);
            }
        }

        // Each view draws its own picture (one drawn in place of another, or without its input, would match).
        int same = 0;

        for (int i = 0; i < 7; ++i)
            for (int j = i + 1; j < 7; ++j)
                same += !pictures[i].empty() && pictures[i] == pictures[j] ? 1 : 0;

        ok &= Check("no two views draw the same picture", same == 0);
        extra.flowPreview = false;
    }

    printf("the depth view's look and the depth alignment\n");
    {
        readGuideDepth = true;
        Scene shifted { 7, 300, 20.0f, 5.0f };
        shifted.depthShift = 40.0f; // the depth's square is 40 pixels right of the picture's

        // The look of the depth view is the view's alone: it changes the view, not the consumer's depth.
        extra.flowPreview = true;
        extra.previewView = 1;
        frame(shifted, kRead, true, true, false, output, nullptr, nullptr);
        const auto plain = guideDepth;
        const auto plainView = ReadRgba(gpu, producer.Flow()->Preview(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        auto& look = producer.Flow()->DebugTuning();
        look.depthMultiplier = 50.0f;
        look.depthLinearize = false;
        frame(shifted, kRead, true, true, false, output, nullptr, nullptr);
        const auto lookView = ReadRgba(gpu, producer.Flow()->Preview(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ok &= Check("the depth view's multiplier and linearization change the view",
                    !plainView.empty() && lookView != plainView);
        ok &= Check("... and leave the consumer's depth as it was", !plain.empty() && guideDepth == plain);
        look = OpticalFlowDx12::DebugSettings {};
        extra.flowPreview = false;

        // Inside the picture's square but left of the depth's, and right of the picture's square but inside the
        // depth's: without the alignment the first is background and the second the square; moved 40 pixels back,
        // the other way round.
        const size_t inPicture = (size_t) 360 * kWidth + 300 + 20;
        const size_t pastPicture = (size_t) 360 * kWidth + 300 + 220;
        ok &= Check("without the alignment the depth's square is off the picture's",
                    !plain.empty() && plain[pastPicture] > plain[inPicture]);

        // A new alignment starts the trust mask over, not the flow: the frame keeps its motion and the consumer.
        extra.depthTransform[2] = -40.0f; // read the depth 40 pixels to the right: it moves left
        auto r = frame(shifted, kRead, true, true, false, output, nullptr, nullptr);
        ok &= Check("an alignment change keeps the flow and the consumer running", r.flowValid && r.nativeRan);
        ok &= Check("with an X offset of -40 the consumer's depth lines up with the picture",
                    !guideDepth.empty() && guideDepth[inPicture] > guideDepth[pastPicture]);

        // Y the other way up: an offset of +30 reads the depth 30 rows lower (uv + 30 / height), so it moves up.
        extra.depthTransform[2] = 0.0f;
        extra.depthTransform[3] = 30.0f;
        frame(shifted, kRead, true, true, false, output, nullptr, nullptr);
        const size_t row240 = (size_t) 240 * kWidth + 520, row270 = (size_t) 270 * kWidth + 520; // the depth's square
        ok &= Check("with a Y offset of +30 the depth moves up 30 rows",
                    !guideDepth.empty() && guideDepth[row240] == plain[row270] && guideDepth[row240] != plain[row240]);

        extra.depthTransform[3] = 0.0f;
        frame(shifted, kRead, true, true, false, output, nullptr, nullptr);
        ok &= Check("back to no alignment: the depth as the game gave it", guideDepth == plain);
        readGuideDepth = false;
    }

    // The caller's queue changes between frames (NBA 2K27: frame generation's game queue comes and goes at a screen
    // change). Every list uses the same textures, so a list on another queue must not run while the last one still
    // does: the frame is skipped (a CPU check, no GPU wait across queues) until it has finished.
    printf("the caller's queue changes between frames\n");
    {
        ComPtr<ID3D12CommandQueue> second;
        ComPtr<ID3D12Fence> gate;
        D3D12_COMMAND_QUEUE_DESC qd {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        const bool made = SUCCEEDED(gpu.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&second))) &&
                          SUCCEEDED(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
        ok &= Check("a second direct queue", made);
        const Scene a { 7, 300, 20.0f, 5.0f }, b { 7, 300, 24.0f, 5.0f };

        if (made)
        {
            auto r = frame(a, kRead, true, true, false, output, nullptr, nullptr);
            ok &= Check("a frame on the first queue runs", r.submitted && !r.queueChanged);

            // The last list done: a frame on the second queue runs, and says the queue changed.
            runQueue = second.Get();
            r = frame(b, kRead, true, true, false, output, nullptr, nullptr);
            ok &= Check("the last list done: a frame on another queue runs", r.submitted && r.queueChanged);

            // The second queue held back by a gate, so its list stays unfinished; then a frame on the first queue.
            second->Wait(gate.Get(), 1);
            waitDone = false;
            r = frame(a, kRead, true, true, false, output, nullptr, nullptr);
            ok &= Check("a frame behind the gate is submitted on its queue", r.submitted && !r.queueChanged);
            const native::SyncPoint held = output.done;

            runQueue = nullptr;
            r = frame(b, kRead, true, true, false, output, nullptr, nullptr);
            ok &= Check("the last list unfinished: a frame on another queue is skipped, nothing submitted",
                        !r.submitted && r.queueChanged && r.stoppedAt != nullptr &&
                            std::string(r.stoppedAt).find("another queue") != std::string::npos);

            // The gate opens: the held list finishes, and the next frame on the first queue runs with a reset of NR.
            gate->Signal(1);
            held.fence->SetEventOnCompletion(held.value, gpu.event);
            ok &= Check("the held list finishes once the gate opens", WaitForSingleObject(gpu.event, 5000) == WAIT_OBJECT_0);
            waitDone = true;
            const auto resetsBefore = nr.count;
            r = frame(a, kRead, true, true, false, output, nullptr, nullptr);
            ok &= Check("then a frame on the first queue runs", r.submitted && r.queueChanged);
            ok &= Check("... and NR's history restarts after the skipped frame", nr.count != resetsBefore && nr.reset);
            keep.clear();
        }
    }

    printf(ok ? "all passed\n" : "FAILED\n");
    return ok ? 0 : 1;
}
