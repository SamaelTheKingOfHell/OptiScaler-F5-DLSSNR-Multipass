#pragma once
// Shared pieces of the NvAPI kernel-chain research tools (nr_cuda_trace.cpp, nr_split_splice.cpp): the CUDA-kernel
// launch types NVIDIA's DLSS-NR model uses through NvAPI, a Detours hook of nvapi_QueryInterface, synthetic D3D12
// surfaces, and a model session loaded through the forwarder. Test code only.
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <detours/detours.h>
#include "../../external/nvngx_dlss_sdk/nvsdk_ngx.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace NrHarness
{
using Microsoft::WRL::ComPtr;

// ---- NvAPI cubin launch types (external/nvapi/nvapi.h) ----
using NvHandle = void*;
struct Dim3
{
    uint32_t x, y, z;
};
struct CuLaunch
{
    NvHandle hFunction;
    Dim3 gridDim;
    Dim3 blockDim;
    uint32_t dynSharedMemBytes;
    const void* pParams;
    uint32_t paramSize;
};
using PFN_QueryInterface = void*(__cdecl*)(unsigned);
using PFN_CreateCuModule = int(__cdecl*)(ID3D12Device*, const void*, uint32_t, NvHandle*);
using PFN_CreateCuFunction = int(__cdecl*)(ID3D12Device*, NvHandle, const char*, NvHandle*);
using PFN_LaunchChain = int(__cdecl*)(ID3D12GraphicsCommandList*, const CuLaunch*, uint32_t);
constexpr unsigned kCreateModule = 0xad1a677d, kCreateFunction = 0xe2436e22, kLaunchChain = 0x24973538;

// The real NvAPI entry points, filled when the model first queries them.
struct NvApi
{
    PFN_QueryInterface query = nullptr;
    PFN_CreateCuModule createModule = nullptr;
    PFN_CreateCuFunction createFunction = nullptr;
    PFN_LaunchChain launch = nullptr;
};
inline NvApi& Real()
{
    static NvApi api;
    return api;
}

// Installs a Detours hook on nvapi_QueryInterface. `wrap(id, raw)` returns the pointer handed to the caller.
template<class Wrap> bool HookNvApi(Wrap wrap)
{
    static Wrap* stored = nullptr;
    static Wrap copy = wrap;
    stored = &copy;
    HMODULE nvapi = LoadLibraryW(L"nvapi64.dll");
    if (!nvapi)
        return false;
    Real().query = reinterpret_cast<PFN_QueryInterface>(GetProcAddress(nvapi, "nvapi_QueryInterface"));
    if (!Real().query)
        return false;
    static PFN_QueryInterface hook = [](unsigned id) -> void*
    {
        void* raw = Real().query(id);
        if (!raw)
            return raw;
        switch (id)
        {
        case kCreateModule:
            Real().createModule = reinterpret_cast<PFN_CreateCuModule>(raw);
            break;
        case kCreateFunction:
            Real().createFunction = reinterpret_cast<PFN_CreateCuFunction>(raw);
            break;
        case kLaunchChain:
            Real().launch = reinterpret_cast<PFN_LaunchChain>(raw);
            break;
        }
        return (*stored)(id, raw);
    };
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(reinterpret_cast<PVOID*>(&Real().query), reinterpret_cast<PVOID>(hook));
    return DetourTransactionCommit() == NO_ERROR;
}

// ---- D3D12 helpers ----
inline void Must(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        char text[160];
        std::snprintf(text, sizeof(text), "%s failed: 0x%08X", what, static_cast<unsigned>(hr));
        throw std::runtime_error(text);
    }
}
struct Surface
{
    ComPtr<ID3D12Resource> texture, upload;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT rows = 0;
    UINT64 rowBytes = 0, bytes = 0;
};
inline ComPtr<ID3D12Resource> MakeBuffer(ID3D12Device* device, D3D12_HEAP_TYPE type, uint64_t bytes,
                                          D3D12_RESOURCE_STATES state, bool uav)
{
    D3D12_HEAP_PROPERTIES heap { type };
    D3D12_RESOURCE_DESC d {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> r;
    Must(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)), "buffer");
    return r;
}
inline Surface MakeSurface(ID3D12Device* device, DXGI_FORMAT format, bool uav, unsigned width, unsigned height)
{
    Surface s;
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES heap { D3D12_HEAP_TYPE_DEFAULT };
    Must(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                         IID_PPV_ARGS(&s.texture)),
         "texture");
    device->GetCopyableFootprints(&desc, 0, 1, 0, &s.footprint, &s.rows, &s.rowBytes, &s.bytes);
    s.upload = MakeBuffer(device, D3D12_HEAP_TYPE_UPLOAD, s.bytes, D3D12_RESOURCE_STATE_GENERIC_READ, false);
    return s;
}
inline void Barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER x {};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    l->ResourceBarrier(1, &x);
}
inline void UavBarrier(ID3D12GraphicsCommandList* l)
{
    D3D12_RESOURCE_BARRIER x {};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    l->ResourceBarrier(1, &x);
}
// kind 0: textured colour (seeded), 1: constant 0.5 depth, 2: zero motion
inline void Upload(ID3D12GraphicsCommandList* l, Surface& s, int kind, unsigned seed = 5)
{
    uint8_t* m = nullptr;
    D3D12_RANGE none { 0, 0 };
    Must(s.upload->Map(0, &none, reinterpret_cast<void**>(&m)), "map");
    std::mt19937 rng(seed);
    for (UINT y = 0; y < s.rows; ++y)
    {
        auto* row = m + y * s.footprint.Footprint.RowPitch;
        std::memset(row, 0, static_cast<size_t>(s.rowBytes));
        if (kind == 0)
        {
            auto* h = reinterpret_cast<uint16_t*>(row);
            for (UINT x = 0; x < s.footprint.Footprint.Width; ++x)
            {
                const uint16_t v = static_cast<uint16_t>(0x3400 + (rng() & 0x3ff));
                h[4 * x] = v;
                h[4 * x + 1] = static_cast<uint16_t>(v ^ 0x55);
                h[4 * x + 2] = static_cast<uint16_t>(v ^ 0xaa);
                h[4 * x + 3] = 0x3c00;
            }
        }
        else if (kind == 1)
        {
            auto* f = reinterpret_cast<float*>(row);
            for (UINT x = 0; x < s.footprint.Footprint.Width; ++x)
                f[x] = 0.5f;
        }
    }
    s.upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
    dst.pResource = s.texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = s.upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = s.footprint;
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
}
inline void WaitQueue(ID3D12Device* device, ID3D12CommandQueue* queue)
{
    ComPtr<ID3D12Fence> fence;
    Must(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Must(queue->Signal(fence.Get(), 1), "signal");
    Must(fence->SetEventOnCompletion(1, event), "event");
    const DWORD w = WaitForSingleObject(event, 30000);
    CloseHandle(event);
    if (w != WAIT_OBJECT_0 || FAILED(device->GetDeviceRemovedReason()))
    {
        std::fprintf(stderr, "GPU work did not retire; terminating.\n");
        std::fflush(nullptr);
        ExitProcess(3);
    }
}
inline ComPtr<ID3D12Device> OpenNvidia(unsigned index, std::wstring* name = nullptr)
{
    ComPtr<IDXGIFactory6> factory;
    Must(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "factory");
    ComPtr<IDXGIAdapter1> adapter;
    unsigned seen = 0;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 d {};
        adapter->GetDesc1(&d);
        if ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || d.VendorId != 0x10de || seen++ != index)
            continue;
        if (name)
            *name = d.Description;
        ComPtr<ID3D12Device> device;
        Must(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
        return device;
    }
    throw std::runtime_error("NVIDIA adapter not found");
}

// ---- Model allocations through NGX's ResourceAllocCallback ----
// The model allocates its buffers and textures through this callback when the host provides one, so the host sees (and
// owns) every resource with its exact size. `tag` labels which feature is being created.
struct Allocation
{
    ComPtr<ID3D12Resource> resource;
    uint64_t va = 0, bytes = 0;
    D3D12_RESOURCE_DESC desc {};
    int state = 0, tag = 0;
};
struct AllocationLog
{
    ID3D12Device* device = nullptr;
    int tag = 0;
    std::vector<Allocation> items;
    unsigned released = 0;
};
inline AllocationLog& Allocations()
{
    static AllocationLog log;
    return log;
}
inline void __cdecl AllocCallback(D3D12_RESOURCE_DESC* desc, int state, D3D12_HEAP_PROPERTIES* heap,
                                  ID3D12Resource** out)
{
    *out = nullptr;
    auto& log = Allocations();
    if (!log.device || !desc || !heap)
        return;
    ComPtr<ID3D12Resource> r;
    if (FAILED(log.device->CreateCommittedResource(heap, D3D12_HEAP_FLAG_NONE, desc,
                                                   static_cast<D3D12_RESOURCE_STATES>(state), nullptr,
                                                   IID_PPV_ARGS(&r))))
        return;
    Allocation a;
    a.resource = r;
    a.desc = *desc;
    a.state = state;
    a.tag = log.tag;
    a.va = desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? r->GetGPUVirtualAddress() : 0;
    a.bytes = log.device->GetResourceAllocationInfo(0, 1, desc).SizeInBytes;
    log.items.push_back(a);
    *out = r.Detach(); // NGX owns this reference; ReleaseCallback returns it
}
inline void __cdecl ReleaseCallback(IUnknown* resource)
{
    if (resource)
    {
        resource->Release();
        ++Allocations().released;
    }
}

// ---- Model session through the forwarder ----
using Init = NVSDK_NGX_Result (*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version,
                                  const NVSDK_NGX_FeatureCommonInfo*);
using GetCaps = NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter**);
using Create = void* (*) (const wchar_t*, const wchar_t*, ID3D12Device*, ID3D12GraphicsCommandList*, void*, unsigned,
                         unsigned, int, float, int, float, float, float, int, int);
using Evaluate = int (*)(ID3D12GraphicsCommandList*, void*, void*, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*,
                         ID3D12Resource*, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
                         unsigned, unsigned, unsigned, int, int, float, int, float, float, float, int, float, float);
struct Model
{
    Create create = nullptr;
    Evaluate evaluate = nullptr;
    const char* (*error)() = nullptr;
    NVSDK_NGX_Parameter* params = nullptr;
    std::wstring modelPath, dataPath;

    void Load(ID3D12Device* device, const wchar_t* core, const wchar_t* forwarder, const wchar_t* model,
              const wchar_t* data)
    {
        HMODULE c = LoadLibraryExW(core, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        HMODULE f = LoadLibraryExW(forwarder, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!c || !f)
            throw std::runtime_error("could not load NGX core or forwarder");
        auto init = reinterpret_cast<Init>(GetProcAddress(c, "NVSDK_NGX_D3D12_Init_Ext"));
        auto caps = reinterpret_cast<GetCaps>(GetProcAddress(c, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
        create = reinterpret_cast<Create>(GetProcAddress(f, "dlssnr_call_create"));
        evaluate = reinterpret_cast<Evaluate>(GetProcAddress(f, "dlssnr_call_evaluate_v2"));
        error = reinterpret_cast<const char* (*) ()>(GetProcAddress(f, "dlssnr_call_error"));
        auto probe = reinterpret_cast<void (*)(void*, const char*, float, int)>(GetProcAddress(f, "dlssnr_call_probe_float"));
        auto setSlot = reinterpret_cast<void (*)(int)>(GetProcAddress(f, "dlssnr_call_set_float_slot"));
        if (!init || !caps || !create || !evaluate || !probe || !setSlot)
            throw std::runtime_error("missing exports");
        if (init(0x24480451ull, data, device, NVSDK_NGX_Version_API, nullptr) != NVSDK_NGX_Result_Success)
            throw std::runtime_error("NGX init failed");
        if (caps(&params) != NVSDK_NGX_Result_Success || !params)
            throw std::runtime_error("NGX parameters failed");
        if (_wgetenv(L"NR_ALLOC_CALLBACK"))
        {
            Allocations().device = device;
            params->Set(NVSDK_NGX_Parameter_ResourceAllocCallback, reinterpret_cast<void*>(&AllocCallback));
            params->Set(NVSDK_NGX_Parameter_ResourceReleaseCallback, reinterpret_cast<void*>(&ReleaseCallback));
        }
        bool found = false;
        for (int s : { 1, 2, 5, 6, 7, 4, 3, 0 })
        {
            float v = 0;
            probe(params, "DLSSNR.HarnessProbe", 0.375f, s);
            if (params->Get("DLSSNR.HarnessProbe", &v) == NVSDK_NGX_Result_Success && v == 0.375f)
            {
                setSlot(s);
                found = true;
                break;
            }
        }
        if (!found)
            throw std::runtime_error("float slot not found");
        modelPath = model;
        dataPath = data;
    }
    void* NewFeature(ID3D12Device* device, ID3D12GraphicsCommandList* list, unsigned w, unsigned h, int preset)
    {
        void* f = create(modelPath.c_str(), dataPath.c_str(), device, list, params, w, h, preset, 0.5f, 2, 0.0f, 0.0f,
                         0.0f, 1, 1);
        if (!f)
            throw std::runtime_error(error && error() ? error() : "create failed");
        return f;
    }
    int Run(ID3D12GraphicsCommandList* list, void* feature, Surface& color, Surface& depth, Surface& motion,
            Surface& out, unsigned w, unsigned h, bool reset)
    {
        return evaluate(list, feature, params, color.texture.Get(), depth.texture.Get(), motion.texture.Get(),
                        out.texture.Get(), w, h, w, h, w, h, 0, 0, 0, 0, 0, reset ? 1 : 0, 0.5f, 2, 0.0f, 0.0f, 0.0f,
                        1, 1.0f, 1.0f);
    }
};

// 256-byte aligned 64-bit parameter words that look like GPU addresses (arena tensors), in blob order.
inline std::vector<uint64_t> AddressWords(const CuLaunch& k)
{
    std::vector<uint64_t> words;
    const auto* bytes = static_cast<const uint8_t*>(k.pParams);
    for (uint32_t o = 0; bytes && o + 8 <= k.paramSize; o += 8)
    {
        uint64_t v = 0;
        std::memcpy(&v, bytes + o, 8);
        if (v >= (1ull << 24) && v < (1ull << 48) && (v & 0xff) == 0)
            words.push_back(v);
    }
    return words;
}
} // namespace NrHarness
