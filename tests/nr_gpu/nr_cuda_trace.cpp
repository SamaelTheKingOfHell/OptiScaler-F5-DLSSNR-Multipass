// Traces the GPU kernels inside one DLSS-NR evaluate, to judge whether the network can be split between GPUs.
//
// The model DLL records its network on the D3D12 command list through NvAPI (NvAPI_D3D12_CreateCuModule,
// NvAPI_D3D12_CreateCuFunction, NvAPI_D3D12_LaunchCuKernelChain; see OptiScaler/dlssnr/DlssNrKernelProfile.h). This tool
// loads the model through the forwarder on one adapter, hooks nvapi_QueryInterface and the D3D12 resource-creation
// methods with Detours, and records for every kernel: its name, launch shape, parameter blob, the D3D12 buffers whose GPU
// virtual addresses appear in the parameters, and a timestamp after each launch call. For each cut between consecutive
// launches it then reports the bytes of buffers used on both sides of the cut (the data a second GPU would need), and how
// much of the measured time lies before the cut.
//
// Usage: nr_cuda_trace.exe <_nvngx.dll> <nvngx.dll_dlssnr.dll forwarder> <nvngx_dlssnr.dll model> <data dir>
//                          <adapter index> <width> <height> <preset> <frames> <out prefix>
// Writes <out prefix>-launches.csv, -buffers.csv and -summary.txt. Synthetic input: structure and timing only.
//
// Build: cl /std:c++17 /EHsc /O2 /I OptiScaler\include tests\nr_gpu\nr_cuda_trace.cpp
//        /link OptiScaler\library\detours\detours.lib d3d12.lib dxgi.lib user32.lib
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <detours/detours.h>
#include "../../external/nvngx_dlss_sdk/nvsdk_ngx.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
// ---- NvAPI cubin launch types (external/nvapi/nvapi.h) -----------------------------------------------------------
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

PFN_QueryInterface o_QueryInterface = nullptr;
PFN_CreateCuModule o_CreateCuModule = nullptr;
PFN_CreateCuFunction o_CreateCuFunction = nullptr;
PFN_LaunchChain o_LaunchChain = nullptr;

// ---- Trace state ------------------------------------------------------------------------------------------------
struct Buffer
{
    int id = 0;
    uint64_t va = 0, size = 0;
    int phase = 0; // 0 = setup (feature creation), n = created during evaluate n
    std::string heap;
};
struct Kernel
{
    int frame = 0, call = 0, index = 0;
    std::string name;
    Dim3 grid {}, block {};
    uint32_t shared = 0, paramSize = 0;
    std::vector<int> buffers;
    std::vector<uint64_t> words; // 256-byte aligned parameter values that look like GPU addresses
};
std::recursive_mutex g_mutex;
std::vector<Buffer> g_buffers;
std::map<uint64_t, int> g_byVa;
std::unordered_map<NvHandle, std::string> g_names;
std::vector<std::pair<uint32_t, int>> g_modules; // size, frame
std::vector<Kernel> g_kernels;
int g_frame = 0, g_call = 0;
// timestamps: one before the first call of a frame and one after every call
ComPtr<ID3D12QueryHeap> g_queries;
uint32_t g_queryNext = 0;
constexpr uint32_t kMaxQueries = 8192;
std::vector<std::pair<int, uint32_t>> g_callQuery; // (call, query index after it) for the current frame
unsigned g_createCalls[8] {};      // resource-creation hook calls per slot (diagnostic)
std::map<uint64_t, int> g_paramWords; // 256-byte aligned 64-bit parameter values that look like GPU addresses

int FindBuffer(uint64_t value)
{
    auto it = g_byVa.upper_bound(value);
    if (it == g_byVa.begin())
        return -1;
    --it;
    const Buffer& b = g_buffers[it->second];
    return value >= b.va && value < b.va + b.size ? b.id : -1;
}

void* __cdecl hk_QueryInterface(unsigned id);
int __cdecl hk_CreateCuModule(ID3D12Device* d, const void* blob, uint32_t size, NvHandle* module)
{
    const int r = o_CreateCuModule(d, blob, size, module);
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_modules.push_back({ size, g_frame });
    return r;
}
int __cdecl hk_CreateCuFunction(ID3D12Device* d, NvHandle module, const char* name, NvHandle* fn)
{
    const int r = o_CreateCuFunction(d, module, name, fn);
    if (r == 0 && fn && name)
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        g_names[*fn] = name;
    }
    return r;
}
int __cdecl hk_LaunchChain(ID3D12GraphicsCommandList* list, const CuLaunch* k, uint32_t count)
{
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (g_frame > 0 && g_queries && g_queryNext == 0)
            list->EndQuery(g_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, g_queryNext++);
        for (uint32_t i = 0; k && i < count; ++i)
        {
            Kernel e;
            e.frame = g_frame;
            e.call = g_call;
            e.index = static_cast<int>(g_kernels.size());
            auto n = g_names.find(k[i].hFunction);
            e.name = n != g_names.end() ? n->second : "?";
            e.grid = k[i].gridDim;
            e.block = k[i].blockDim;
            e.shared = k[i].dynSharedMemBytes;
            e.paramSize = k[i].paramSize;
            const auto* bytes = static_cast<const uint8_t*>(k[i].pParams);
            for (uint32_t o = 0; bytes && o + 8 <= k[i].paramSize; o += 4) // 4-byte steps: packed layouts
            {
                uint64_t v = 0;
                std::memcpy(&v, bytes + o, 8);
                if (g_frame > 0 && v > (1ull << 32) && v < (1ull << 48) && (v & 0xff) == 0 && (o % 8) == 0)
                {
                    ++g_paramWords[v];
                    e.words.push_back(v);
                }
                const int id = FindBuffer(v);
                if (id >= 0 && std::find(e.buffers.begin(), e.buffers.end(), id) == e.buffers.end())
                    e.buffers.push_back(id);
            }
            g_kernels.push_back(std::move(e));
        }
    }
    const int r = o_LaunchChain(list, k, count);
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (g_frame > 0 && g_queries && g_queryNext < kMaxQueries)
    {
        list->EndQuery(g_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, g_queryNext);
        g_callQuery.push_back({ g_call, g_queryNext });
        ++g_queryNext;
    }
    ++g_call;
    return r;
}
void* __cdecl hk_QueryInterface(unsigned id)
{
    void* raw = o_QueryInterface(id);
    if (!raw)
        return raw;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    switch (id)
    {
    case kCreateModule:
        o_CreateCuModule = reinterpret_cast<PFN_CreateCuModule>(raw);
        return reinterpret_cast<void*>(&hk_CreateCuModule);
    case kCreateFunction:
        o_CreateCuFunction = reinterpret_cast<PFN_CreateCuFunction>(raw);
        return reinterpret_cast<void*>(&hk_CreateCuFunction);
    case kLaunchChain:
        o_LaunchChain = reinterpret_cast<PFN_LaunchChain>(raw);
        return reinterpret_cast<void*>(&hk_LaunchChain);
    default:
        return raw;
    }
}

// ---- D3D12 resource creation (vtable slots from the Windows SDK d3d12.h ID3D12Device14Vtbl) -----------------------
// All of these take only integer/pointer arguments; one generic 12-slot pass-through serves them (x64 ABI).
using PFN_Generic = HRESULT(STDMETHODCALLTYPE*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                                uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
struct CreateSlot
{
    unsigned vtable;  // index in the device vtable
    unsigned ppv;     // argument index (0 = this) of the void** result
    const char* heap; // committed or placed
    PFN_Generic original = nullptr;
};
CreateSlot g_slots[] = {
    { 29, 7, "committed" },   // CreateCommittedResource
    { 31, 7, "placed" },      // CreatePlacedResource
    { 56, 8, "committed1" },  // CreateCommittedResource1
    { 74, 8, "committed2" },  // CreateCommittedResource2
    { 75, 7, "placed1" },     // CreatePlacedResource1
    { 81, 10, "committed3" }, // CreateCommittedResource3
    { 82, 9, "placed2" },     // CreatePlacedResource2
};
void Record(unsigned slot, uint64_t descArg, uint64_t ppvArg, HRESULT hr)
{
    if (FAILED(hr) || !descArg || !ppvArg)
        return;
    // D3D12_RESOURCE_DESC and D3D12_RESOURCE_DESC1 share the leading Dimension/Alignment/Width fields.
    const auto* desc = reinterpret_cast<const D3D12_RESOURCE_DESC*>(descArg);
    if (desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
        return;
    auto* unknown = *reinterpret_cast<IUnknown**>(ppvArg);
    if (!unknown)
        return;
    ComPtr<ID3D12Resource> resource;
    if (FAILED(unknown->QueryInterface(IID_PPV_ARGS(&resource))))
        return;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Buffer b;
    b.id = static_cast<int>(g_buffers.size());
    b.va = resource->GetGPUVirtualAddress();
    b.size = desc->Width;
    b.phase = g_frame;
    b.heap = g_slots[slot].heap;
    if (b.va)
    {
        g_buffers.push_back(b);
        g_byVa[b.va] = b.id;
    }
}
template<unsigned I>
HRESULT STDMETHODCALLTYPE CreateHook(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                                     uint64_t a6, uint64_t a7, uint64_t a8, uint64_t a9, uint64_t a10, uint64_t a11)
{
    const HRESULT hr = g_slots[I].original(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11);
    ++g_createCalls[I];
    const uint64_t args[] = { a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11 };
    Record(I, args[3], args[g_slots[I].ppv], hr);
    return hr;
}
PVOID g_hooks[] = { reinterpret_cast<PVOID>(&CreateHook<0>), reinterpret_cast<PVOID>(&CreateHook<1>),
                    reinterpret_cast<PVOID>(&CreateHook<2>), reinterpret_cast<PVOID>(&CreateHook<3>),
                    reinterpret_cast<PVOID>(&CreateHook<4>), reinterpret_cast<PVOID>(&CreateHook<5>),
                    reinterpret_cast<PVOID>(&CreateHook<6>) };

bool InstallHooks(ID3D12Device* device)
{
    HMODULE nvapi = LoadLibraryW(L"nvapi64.dll");
    if (!nvapi)
        return false;
    o_QueryInterface = reinterpret_cast<PFN_QueryInterface>(GetProcAddress(nvapi, "nvapi_QueryInterface"));
    if (!o_QueryInterface)
        return false;
    ComPtr<ID3D12Device> latest;
    unsigned vtableSize = 44; // ID3D12Device
    for (auto [iid, size] : { std::pair { __uuidof(ID3D12Device10), 84u }, { __uuidof(ID3D12Device8), 77u },
                              { __uuidof(ID3D12Device4), 60u } })
        if (SUCCEEDED(device->QueryInterface(iid, reinterpret_cast<void**>(latest.ReleaseAndGetAddressOf()))))
        {
            vtableSize = size;
            break;
        }
    if (!latest)
        latest = device;
    void** vtable = *reinterpret_cast<void***>(latest.Get());
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(reinterpret_cast<PVOID*>(&o_QueryInterface), reinterpret_cast<PVOID>(&hk_QueryInterface));
    std::set<void*> attached;
    for (unsigned i = 0; i < sizeof(g_slots) / sizeof(g_slots[0]); ++i)
    {
        if (g_slots[i].vtable >= vtableSize || attached.count(vtable[g_slots[i].vtable]))
            continue;
        attached.insert(vtable[g_slots[i].vtable]);
        g_slots[i].original = reinterpret_cast<PFN_Generic>(vtable[g_slots[i].vtable]);
        DetourAttach(reinterpret_cast<PVOID*>(&g_slots[i].original), g_hooks[i]);
    }
    return DetourTransactionCommit() == NO_ERROR;
}

// ---- D3D12 and model harness --------------------------------------------------------------------------------------
void Must(HRESULT hr, const char* what)
{
    if (FAILED(hr))
        throw std::runtime_error(what);
}
struct Surface
{
    ComPtr<ID3D12Resource> texture, upload;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT rows = 0;
    UINT64 rowBytes = 0, bytes = 0;
};
Surface MakeSurface(ID3D12Device* device, DXGI_FORMAT format, bool uav, unsigned width, unsigned height)
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
    D3D12_RESOURCE_DESC buffer {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = s.bytes;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    Must(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ,
                                         nullptr, IID_PPV_ARGS(&s.upload)),
         "upload");
    return s;
}
void Barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER x {};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    l->ResourceBarrier(1, &x);
}
void Upload(ID3D12GraphicsCommandList* l, Surface& s, int kind)
{
    uint8_t* m = nullptr;
    D3D12_RANGE none { 0, 0 };
    Must(s.upload->Map(0, &none, reinterpret_cast<void**>(&m)), "map");
    std::mt19937 rng(5);
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
void WaitQueue(ID3D12Device* device, ID3D12CommandQueue* queue)
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

using Init = NVSDK_NGX_Result (*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version,
                                  const NVSDK_NGX_FeatureCommonInfo*);
using GetCaps = NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter**);
using Create = void* (*) (const wchar_t*, const wchar_t*, ID3D12Device*, ID3D12GraphicsCommandList*, void*, unsigned,
                         unsigned, int, float, int, float, float, float, int, int);
using Evaluate = int (*)(ID3D12GraphicsCommandList*, void*, void*, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*,
                         ID3D12Resource*, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
                         unsigned, unsigned, unsigned, int, int, float, int, float, float, float, int, float, float);
using ProbeFloat = void (*)(void*, const char*, float, int);
using SetSlot = void (*)(int);
using ErrorText = const char* (*) ();

std::string Human(double bytes)
{
    char t[64];
    if (bytes >= 1 << 20)
        std::snprintf(t, sizeof(t), "%.2f MiB", bytes / (1 << 20));
    else
        std::snprintf(t, sizeof(t), "%.1f KiB", bytes / 1024);
    return t;
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc != 11)
    {
        std::fprintf(stderr, "usage: core forwarder model data adapter width height preset frames outprefix\n");
        return 2;
    }
    try
    {
        const unsigned adapterIndex = std::stoul(argv[5]), width = std::stoul(argv[6]), height = std::stoul(argv[7]);
        const int preset = std::stoi(argv[8]);
        const int frames = std::max(1, std::stoi(argv[9]));
        const std::wstring out = argv[10];

        ComPtr<IDXGIFactory6> factory;
        Must(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "factory");
        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<ID3D12Device> device;
        unsigned seen = 0;
        std::wstring adapterName;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 d {};
            adapter->GetDesc1(&d);
            if ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || d.VendorId != 0x10de || seen++ != adapterIndex)
                continue;
            adapterName = d.Description;
            Must(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
            break;
        }
        if (!device)
            throw std::runtime_error("adapter not found");
        if (!InstallHooks(device.Get()))
            throw std::runtime_error("could not install hooks");

        D3D12_COMMAND_QUEUE_DESC qd {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> queue;
        Must(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
        UINT64 frequency = 0;
        Must(queue->GetTimestampFrequency(&frequency), "frequency");
        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12GraphicsCommandList> list;
        Must(device->CreateCommandAllocator(qd.Type, IID_PPV_ARGS(&alloc)), "alloc");
        Must(device->CreateCommandList(0, qd.Type, alloc.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
        D3D12_QUERY_HEAP_DESC qh { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kMaxQueries, 0 };
        Must(device->CreateQueryHeap(&qh, IID_PPV_ARGS(&g_queries)), "query heap");
        ComPtr<ID3D12Resource> readback;
        {
            D3D12_HEAP_PROPERTIES heap { D3D12_HEAP_TYPE_READBACK };
            D3D12_RESOURCE_DESC rb {};
            rb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rb.Width = kMaxQueries * 8;
            rb.Height = rb.DepthOrArraySize = rb.MipLevels = 1;
            rb.SampleDesc.Count = 1;
            rb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            Must(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&readback)),
                 "readback");
        }
        // Our own harness buffers are not the model's: forget anything recorded so far.
        g_buffers.clear();
        g_byVa.clear();

        HMODULE core = LoadLibraryExW(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        HMODULE forwarder = LoadLibraryExW(argv[2], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!core || !forwarder)
            throw std::runtime_error("could not load NGX core or forwarder");
        auto init = reinterpret_cast<Init>(GetProcAddress(core, "NVSDK_NGX_D3D12_Init_Ext"));
        auto caps = reinterpret_cast<GetCaps>(GetProcAddress(core, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
        auto create = reinterpret_cast<Create>(GetProcAddress(forwarder, "dlssnr_call_create"));
        auto evaluate = reinterpret_cast<Evaluate>(GetProcAddress(forwarder, "dlssnr_call_evaluate_v2"));
        auto probe = reinterpret_cast<ProbeFloat>(GetProcAddress(forwarder, "dlssnr_call_probe_float"));
        auto setSlot = reinterpret_cast<SetSlot>(GetProcAddress(forwarder, "dlssnr_call_set_float_slot"));
        auto error = reinterpret_cast<ErrorText>(GetProcAddress(forwarder, "dlssnr_call_error"));
        if (!init || !caps || !create || !evaluate || !probe || !setSlot)
            throw std::runtime_error("missing exports");
        if (init(0x24480451ull, argv[4], device.Get(), NVSDK_NGX_Version_API, nullptr) != NVSDK_NGX_Result_Success)
            throw std::runtime_error("NGX init failed");
        NVSDK_NGX_Parameter* params = nullptr;
        if (caps(&params) != NVSDK_NGX_Result_Success || !params)
            throw std::runtime_error("NGX parameters failed");
        bool slot = false;
        for (int s : { 1, 2, 5, 6, 7, 4, 3, 0 })
        {
            float v = 0;
            probe(params, "DLSSNR.TraceProbe", 0.375f, s);
            if (params->Get("DLSSNR.TraceProbe", &v) == NVSDK_NGX_Result_Success && v == 0.375f)
            {
                setSlot(s);
                slot = true;
                break;
            }
        }
        if (!slot)
            throw std::runtime_error("float slot not found");

        Surface color = MakeSurface(device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, false, width, height);
        Surface depth = MakeSurface(device.Get(), DXGI_FORMAT_R32_FLOAT, false, width, height);
        Surface motion = MakeSurface(device.Get(), DXGI_FORMAT_R16G16_FLOAT, false, width, height);
        Surface output = MakeSurface(device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, true, width, height);
        g_buffers.clear(); // upload buffers above are ours
        g_byVa.clear();
        Upload(list.Get(), color, 0);
        Upload(list.Get(), depth, 1);
        Upload(list.Get(), motion, 2);
        for (auto* s : { &color, &depth, &motion })
            Barrier(list.Get(), s->texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(list.Get(), output.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        void* feature = create(argv[3], argv[4], device.Get(), list.Get(), params, width, height, preset, 0.5f, 2, 0.0f,
                               0.0f, 0.0f, 1, 1);
        if (!feature)
            throw std::runtime_error(error && error() ? error() : "create failed");
        Must(list->Close(), "close");
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        WaitQueue(device.Get(), queue.Get());

        std::vector<double> callMs; // last frame: time between consecutive timestamps, per call
        for (int f = 1; f <= frames; ++f)
        {
            Must(alloc->Reset(), "reset");
            Must(list->Reset(alloc.Get(), nullptr), "reset list");
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                g_frame = f;
                g_call = 0;
                g_queryNext = 0;
                g_callQuery.clear();
            }
            const int r = evaluate(list.Get(), feature, params, color.texture.Get(), depth.texture.Get(),
                                   motion.texture.Get(), output.texture.Get(), width, height, width, height, width,
                                   height, 0, 0, 0, 0, 0, f == 1, 0.5f, 2, 0.0f, 0.0f, 0.0f, 1, 1.0f, 1.0f);
            if (r != 1)
                throw std::runtime_error(std::string("evaluate failed: ") + (error ? error() : ""));
            if (g_queryNext)
                list->ResolveQueryData(g_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, g_queryNext, readback.Get(), 0);
            Must(list->Close(), "close");
            queue->ExecuteCommandLists(1, lists);
            WaitQueue(device.Get(), queue.Get());
            if (f == frames && g_queryNext > 1)
            {
                uint64_t* t = nullptr;
                D3D12_RANGE range { 0, g_queryNext * 8 };
                Must(readback->Map(0, &range, reinterpret_cast<void**>(&t)), "map timestamps");
                callMs.assign(g_call, 0.0);
                uint32_t previous = 0;
                for (auto [call, q] : g_callQuery)
                {
                    callMs[call] = t[q] >= t[previous] ? double(t[q] - t[previous]) * 1000.0 / double(frequency) : 0.0;
                    previous = q;
                }
                D3D12_RANGE none { 0, 0 };
                readback->Unmap(0, &none);
            }
        }

        // The model sub-allocates its tensors from one arena, so D3D12 resources do not delimit them. When no model
        // buffers were seen, every distinct address-like parameter word in the largest address cluster is taken as a
        // tensor whose size is the gap to the next distinct word (an upper bound; the last one is unknown, size 0).
        bool arenaTensors = false;
        if (g_buffers.empty() && !g_paramWords.empty())
        {
            std::vector<uint64_t> all;
            for (auto& [v, c] : g_paramWords)
                all.push_back(v);
            size_t bestStart = 0, bestCount = 0;
            for (size_t i = 0; i < all.size();)
            {
                size_t j = i;
                while (j + 1 < all.size() && all[j + 1] - all[j] < (64ull << 20))
                    ++j;
                if (j - i + 1 > bestCount)
                {
                    bestCount = j - i + 1;
                    bestStart = i;
                }
                i = j + 1;
            }
            for (size_t i = bestStart; i < bestStart + bestCount; ++i)
            {
                Buffer b;
                b.id = static_cast<int>(g_buffers.size());
                b.va = all[i];
                b.size = i + 1 < bestStart + bestCount ? all[i + 1] - all[i] : 0;
                b.heap = "arena";
                b.phase = 1;
                g_buffers.push_back(b);
                g_byVa[b.va] = b.id;
            }
            for (auto& e : g_kernels)
            {
                e.buffers.clear();
                for (uint64_t v : e.words)
                {
                    auto it = g_byVa.find(v); // exact tensor starts only
                    if (it != g_byVa.end() &&
                        std::find(e.buffers.begin(), e.buffers.end(), it->second) == e.buffers.end())
                        e.buffers.push_back(it->second);
                }
            }
            arenaTensors = true;
        }

        // ---- Reports ----
        const int last = frames;
        std::vector<const Kernel*> k;
        for (const auto& e : g_kernels)
            if (e.frame == last)
                k.push_back(&e);
        // per-kernel time: a call's time split evenly over its kernels (chains overlap; a ranking, not exact)
        std::vector<double> kernelMs(k.size(), 0.0);
        {
            std::map<int, int> perCall;
            for (const auto* e : k)
                ++perCall[e->call];
            for (size_t i = 0; i < k.size(); ++i)
                if (k[i]->call < static_cast<int>(callMs.size()))
                    kernelMs[i] = callMs[k[i]->call] / perCall[k[i]->call];
        }
        double total = 0;
        for (double v : kernelMs)
            total += v;

        std::ofstream launches(out + L"-launches.csv");
        launches << "frame,call,index,kernel,grid,block,shared,param_bytes,est_ms,buffers\n";
        for (size_t i = 0; i < g_kernels.size(); ++i)
        {
            const auto& e = g_kernels[i];
            double ms = 0;
            for (size_t j = 0; j < k.size(); ++j)
                if (k[j] == &e)
                    ms = kernelMs[j];
            launches << e.frame << ',' << e.call << ',' << e.index << ",\"" << e.name << "\"," << e.grid.x << 'x'
                     << e.grid.y << 'x' << e.grid.z << ',' << e.block.x << 'x' << e.block.y << 'x' << e.block.z << ','
                     << e.shared << ',' << e.paramSize << ',' << ms << ",\"";
            for (size_t b = 0; b < e.buffers.size(); ++b)
                launches << (b ? " " : "") << e.buffers[b];
            launches << "\"\n";
        }
        std::ofstream buffers(out + L"-buffers.csv");
        buffers << "id,va,size,heap,phase\n";
        for (const auto& b : g_buffers)
            buffers << b.id << ",0x" << std::hex << b.va << std::dec << ',' << b.size << ',' << b.heap << ','
                    << b.phase << '\n';

        // Buffers referenced in this frame. A buffer referenced by kernels on both sides of a cut must exist on both
        // GPUs. Weights are among them but never change: they would be copied once, not per frame. Without write
        // tracking, weights are approximated as buffers referenced by exactly one kernel name that is not a copy...
        // which is unreliable, so the report gives both the total and the largest single buffers crossing the cut.
        const size_t n = k.size();
        std::vector<int> firstUse(g_buffers.size(), -1), lastUse(g_buffers.size(), -1);
        for (size_t i = 0; i < n; ++i)
            for (int id : k[i]->buffers)
            {
                if (firstUse[id] < 0)
                    firstUse[id] = static_cast<int>(i);
                lastUse[id] = static_cast<int>(i);
            }
        std::vector<double> crossing(n ? n - 1 : 0, 0.0);
        for (size_t id = 0; id < g_buffers.size(); ++id)
            for (int c = firstUse[id]; firstUse[id] >= 0 && c < lastUse[id]; ++c)
                crossing[c] += double(g_buffers[id].size);
        double usedBytes = 0;
        size_t usedCount = 0;
        for (size_t id = 0; id < g_buffers.size(); ++id)
            if (firstUse[id] >= 0)
            {
                usedBytes += double(g_buffers[id].size);
                ++usedCount;
            }

        std::map<std::string, std::pair<int, double>> byName;
        for (size_t i = 0; i < n; ++i)
        {
            auto& e = byName[k[i]->name];
            ++e.first;
            e.second += kernelMs[i];
        }

        FILE* s = _wfopen((out + L"-summary.txt").c_str(), L"w");
        auto both = [&](const char* fmt, auto... args)
        {
            std::printf(fmt, args...);
            if (s)
                std::fprintf(s, fmt, args...);
        };
        both("Adapter: %ls\nModel input %ux%u, preset %d, %d evaluates traced\n", adapterName.c_str(), width, height,
             preset, frames);
        both("Cubin modules created: %zu; kernel functions: %zu\n", g_modules.size(), g_names.size());
        std::map<int, std::pair<int, int>> perFrame; // frame -> (calls, kernels)
        for (const auto& e : g_kernels)
        {
            ++perFrame[e.frame].second;
            perFrame[e.frame].first = std::max(perFrame[e.frame].first, e.call + 1);
        }
        for (auto& [f, ck] : perFrame)
            both("  %s %d: %d launch calls, %d kernels\n", f ? "evaluate" : "setup", f, ck.first, ck.second);
        both("Evaluate %d: %zu kernels, %zu distinct, measured %.3f ms between first and last launch call\n", last, n,
             byName.size(), total);
        both("%s: %zu (%s); referenced this evaluate: %zu (%s)\n",
             arenaTensors ? "Arena tensors (sizes = gap to the next address, upper bounds)" : "D3D12 buffers",
             g_buffers.size(),
             Human([&] { double t = 0; for (auto& b : g_buffers) t += double(b.size); return t; }()).c_str(), usedCount,
             Human(usedBytes).c_str());

        both("Resource-creation hook calls per slot:");
        for (unsigned i = 0; i < sizeof(g_slots) / sizeof(g_slots[0]); ++i)
            both(" %s=%u", g_slots[i].heap, g_createCalls[i]);
        both("%cDistinct address-like parameter words over all evaluates: %zu%c", 10, g_paramWords.size(), 10);
        {
            std::vector<uint64_t> words;
            for (auto& [v, c] : g_paramWords)
                words.push_back(v);
            // group into address ranges separated by gaps > 64 MiB
            for (size_t i = 0; i < words.size();)
            {
                size_t j = i;
                while (j + 1 < words.size() && words[j + 1] - words[j] < (64ull << 20))
                    ++j;
                both("  range 0x%llx .. 0x%llx (%s span, %zu words)%c", (unsigned long long) words[i],
                     (unsigned long long) words[j], Human(double(words[j] - words[i])).c_str(), j - i + 1, 10);
                i = j + 1;
            }
        }
        std::vector<std::pair<std::string, std::pair<int, double>>> top(byName.begin(), byName.end());
        std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second.second > b.second.second; });
        both("\nKernels by estimated time (evaluate %d):\n", last);
        for (size_t i = 0; i < top.size() && i < 30; ++i)
            both("  %7.3f ms %5.1f%%  x%-4d %s\n", top[i].second.second,
                 total ? 100 * top[i].second.second / total : 0.0, top[i].second.first, top[i].first.c_str());

        both("\nCuts (the second GPU would run everything after the cut):\n");
        both("  %-12s %-8s %-30s %-14s %s\n", "time before", "kernel", "next kernel", "crossing", "largest crossing");
        double cumulative = 0;
        int nextPct = 50;
        for (size_t i = 0; i + 1 < n && nextPct <= 95; ++i)
        {
            cumulative += kernelMs[i];
            if (total <= 0 || cumulative < total * nextPct / 100.0)
                continue;
            // smallest crossing within the next 3% of time
            size_t best = i;
            double c2 = cumulative;
            for (size_t j = i; j + 1 < n && c2 <= total * (nextPct + 3) / 100.0; ++j)
            {
                if (j > i)
                    c2 += kernelMs[j];
                if (crossing[j] < crossing[best])
                    best = j;
            }
            double largest = 0;
            for (size_t id = 0; id < g_buffers.size(); ++id)
                if (firstUse[id] >= 0 && firstUse[id] <= static_cast<int>(best) &&
                    lastUse[id] > static_cast<int>(best))
                    largest = std::max(largest, double(g_buffers[id].size));
            double before = 0;
            for (size_t j = 0; j <= best; ++j)
                before += kernelMs[j];
            both("  %5.1f%% %4.2fms #%-6zu %-30.30s %-14s %s\n", 100 * before / total, before, best,
                 k[best + 1]->name.c_str(), Human(crossing[best]).c_str(), Human(largest).c_str());
            nextPct += 5;
        }
        if (s)
            std::fclose(s);
        std::printf("Wrote %ls-launches.csv, -buffers.csv, -summary.txt\n", out.c_str());
        std::fflush(nullptr);
        // The feature and NGX are intentionally not torn down: the queue has retired; exit without unloading.
        ExitProcess(0);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        std::fflush(nullptr);
        return 1;
    }
}
