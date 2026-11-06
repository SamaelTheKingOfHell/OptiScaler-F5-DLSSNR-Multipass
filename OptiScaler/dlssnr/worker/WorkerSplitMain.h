#pragma once
// Worker network-split mode (protocol 9, InitialFrameWire::split.cut > 0): this process evaluates the helper's segment
// of every picture on its own model instance (DlssNr_NetworkSplit.h, docs/NR-network-trace.md).
//
//   per frame: forward ranges, packed at the start of the shared buffer, are copied into a local staging buffer; one
//   evaluate runs with the Segment filter (kernel 0 and (cut, resume); the ranges enter the workspace before cut + 1,
//   the backward ranges leave it after resume - 1); the backward staging is copied to the shared buffer after the
//   forward part; then the done fence is signalled. The owner's ready fence gates submission, as in the normal worker.
//
// The model's kernels reach the GPU through NvAPI_D3D12_LaunchCuKernelChain, hooked here (Detours on
// nvapi_QueryInterface); its buffers come from NGX's ResourceAllocCallback, so the workspace address is known.
// Included by WorkerMain.cpp only.

#include "../DlssNr_NetworkSplit.h"
#include "../DlssNrVitCopyKernel.h"
#pragma warning(push, 0)
#include <detours/detours.h>
#pragma warning(pop)
#include <functional>

namespace SplitWorker
{
using DlssNr::NetworkSplit::Filter;
using DlssNr::NetworkSplit::Plan;
using DlssNr::NetworkSplit::Role;

using NvHandle = void*;
struct Dim3
{
    uint32_t x, y, z;
};
struct CuLaunch // NVAPI_CU_KERNEL_LAUNCH_PARAMS
{
    NvHandle hFunction;
    Dim3 gridDim;
    Dim3 blockDim;
    uint32_t dynSharedMemBytes;
    const void* pParams;
    uint32_t paramSize;
};
using PFN_Query = void*(__cdecl*)(unsigned);
using PFN_CreateModule = int(__cdecl*)(ID3D12Device*, const void*, uint32_t, NvHandle*);
using PFN_CreateFunction = int(__cdecl*)(ID3D12Device*, NvHandle, const char*, NvHandle*);
using PFN_Launch = int(__cdecl*)(ID3D12GraphicsCommandList*, const CuLaunch*, uint32_t);

struct State
{
    PFN_Query query = nullptr;
    PFN_CreateModule createModule = nullptr;
    PFN_CreateFunction createFunction = nullptr;
    PFN_Launch launch = nullptr;
    Filter filter;
    NvHandle copy = nullptr;
    // Model allocations made while creating our single feature: the smallest writable buffer is its workspace.
    ID3D12Device* device = nullptr;
    std::vector<std::pair<ComPtr<ID3D12Resource>, D3D12_RESOURCE_DESC>> allocations;
};
inline State& S()
{
    static State s;
    return s;
}

inline int __cdecl Launch(ID3D12GraphicsCommandList* list, const CuLaunch* k, uint32_t count)
{
    auto& s = S();
    for (uint32_t i = 0; i < count; ++i)
    {
        const int status = s.filter.Kernel(
            [&] { return s.launch(list, &k[i], 1); },
            [&](uint64_t dst, uint64_t src, uint64_t bytes)
            {
                struct
                {
                    uint64_t dst, src, n;
                } p { dst, src, bytes / 16 };
                CuLaunch c {};
                c.hFunction = s.copy;
                c.gridDim = { static_cast<uint32_t>((std::min<uint64_t>)((p.n + 255) / 256, 1024)), 1, 1 };
                c.blockDim = { 256, 1, 1 };
                c.pParams = &p;
                c.paramSize = static_cast<uint32_t>(sizeof(p));
                return s.launch(list, &c, 1);
            },
            [&]
            {
                D3D12_RESOURCE_BARRIER b {};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                list->ResourceBarrier(1, &b);
            });
        if (status)
            return status;
    }
    return 0;
}
inline void* __cdecl Query(unsigned id)
{
    auto& s = S();
    void* raw = s.query(id);
    if (!raw)
        return raw;
    switch (id)
    {
    case 0xad1a677d:
        s.createModule = reinterpret_cast<PFN_CreateModule>(raw);
        return raw;
    case 0xe2436e22:
        s.createFunction = reinterpret_cast<PFN_CreateFunction>(raw);
        return raw;
    case 0x24973538:
        s.launch = reinterpret_cast<PFN_Launch>(raw);
        return reinterpret_cast<void*>(&Launch);
    default:
        return raw;
    }
}
inline void __cdecl Allocate(D3D12_RESOURCE_DESC* desc, int state, D3D12_HEAP_PROPERTIES* heap, ID3D12Resource** out)
{
    *out = nullptr;
    auto& s = S();
    ComPtr<ID3D12Resource> r;
    if (!s.device || !desc || !heap ||
        FAILED(s.device->CreateCommittedResource(heap, D3D12_HEAP_FLAG_NONE, desc,
                                                 static_cast<D3D12_RESOURCE_STATES>(state), nullptr,
                                                 IID_PPV_ARGS(&r))))
        return;
    s.allocations.push_back({ r, *desc });
    *out = r.Detach(); // NGX's reference; Release returns it
}
inline void __cdecl Release(IUnknown* resource)
{
    if (resource)
        resource->Release();
}

inline ComPtr<ID3D12Resource> Buffer(ID3D12Device* device, uint64_t bytes, D3D12_HEAP_TYPE type,
                                     D3D12_RESOURCE_STATES state, bool uav)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = type;
    D3D12_RESOURCE_DESC d {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = (std::max<uint64_t>)(bytes, 256);
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> r;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)),
          "Create split buffer");
    return r;
}
inline ComPtr<ID3D12Resource> Texture(ID3D12Device* device, unsigned w, unsigned h, DXGI_FORMAT format, bool uav,
                                      D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> r;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)),
          "Create split texture");
    return r;
}
inline void Transition(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a,
                       D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER x {};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    l->ResourceBarrier(1, &x);
}

using CreateFeature = void* (*) (const wchar_t*, const wchar_t*, ID3D12Device*, ID3D12GraphicsCommandList*, void*,
                                 unsigned, unsigned, int, float, int, float, float, float, int, int);
using EvaluateFeature = int (*)(ID3D12GraphicsCommandList*, void*, void*, ID3D12Resource*, ID3D12Resource*,
                                ID3D12Resource*, ID3D12Resource*, unsigned, unsigned, unsigned, unsigned, unsigned,
                                unsigned, unsigned, unsigned, unsigned, unsigned, int, int, float, int, float, float,
                                float, int, float, float);

// Runs the helper's segment for every frame the owner sends. Returns the process exit code.
inline int Run(ID3D12Device* device, DlssNr::MultiGpu::CrossAdapterPeer& peer, const InitialFrameWire& wire,
               const std::function<bool(void*, DWORD)>& readExact, HANDLE parent, HANDLE initialized,
               const wchar_t* corePath, const wchar_t* forwarderPath, const wchar_t* modelPath,
               const wchar_t* dataPath)
{
    Plan plan;
    if (!DlssNr::NetworkSplit::FromWire(wire.split, plan) ||
        plan.forwardBytes + plan.backwardBytes > wire.bytes)
        throw std::runtime_error("Invalid network split plan");
    auto& s = S();
    s.device = device;

    // NvAPI hook before the model can query its entry points.
    HMODULE nvapi = LoadLibraryW(L"nvapi64.dll");
    if (!nvapi)
        throw std::runtime_error("nvapi64.dll unavailable");
    s.query = reinterpret_cast<PFN_Query>(GetProcAddress(nvapi, "nvapi_QueryInterface"));
    if (!s.query)
        throw std::runtime_error("nvapi_QueryInterface unavailable");
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(reinterpret_cast<PVOID*>(&s.query), reinterpret_cast<PVOID>(&Query));
    if (DetourTransactionCommit() != NO_ERROR)
        throw std::runtime_error("NvAPI hook failed");

    // NGX session with the allocation callback, one feature of the owner's preset at the owner's model size.
    HMODULE core = LoadLibraryExW(corePath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    HMODULE forwarder = LoadLibraryExW(forwarderPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!core || !forwarder)
        throw std::runtime_error("Load NGX core or forwarder failed");
    auto init = reinterpret_cast<Init>(GetProcAddress(core, "NVSDK_NGX_D3D12_Init_Ext"));
    auto caps = reinterpret_cast<GetCapabilities>(GetProcAddress(core, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
    auto create = reinterpret_cast<CreateFeature>(GetProcAddress(forwarder, "dlssnr_call_create"));
    auto evaluate = reinterpret_cast<EvaluateFeature>(GetProcAddress(forwarder, "dlssnr_call_evaluate_v2"));
    auto probe = reinterpret_cast<ProbeFloat>(GetProcAddress(forwarder, "dlssnr_call_probe_float"));
    auto setSlot = reinterpret_cast<SetFloatSlot>(GetProcAddress(forwarder, "dlssnr_call_set_float_slot"));
    if (!init || !caps || !create || !evaluate || !probe || !setSlot)
        throw std::runtime_error("Missing NGX or forwarder exports");
    if (init(wire.applicationId, dataPath, device, NVSDK_NGX_Version_API, nullptr) != NVSDK_NGX_Result_Success)
        throw std::runtime_error("NGX initialization failed");
    NVSDK_NGX_Parameter* params = nullptr;
    if (caps(&params) != NVSDK_NGX_Result_Success || !params)
        throw std::runtime_error("NGX parameters failed");
    bool slot = false;
    for (int candidate : { 1, 2, 5, 6, 7, 4, 3, 0 })
    {
        float readback = 0.0f;
        probe(params, "DLSSNR.OptiScalerFloatProbe", 0.375f, candidate);
        if (params->Get("DLSSNR.OptiScalerFloatProbe", &readback) == NVSDK_NGX_Result_Success && readback == 0.375f)
        {
            setSlot(candidate);
            slot = true;
            break;
        }
    }
    if (!slot)
        throw std::runtime_error("Float parameter slot not found");
    params->Set(NVSDK_NGX_Parameter_ResourceAllocCallback, reinterpret_cast<void*>(&Allocate));
    params->Set(NVSDK_NGX_Parameter_ResourceReleaseCallback, reinterpret_cast<void*>(&Release));

    D3D12_COMMAND_QUEUE_DESC qd {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "Create split queue");
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    Check(device->CreateCommandAllocator(qd.Type, IID_PPV_ARGS(&alloc)), "Create split allocator");
    Check(device->CreateCommandList(0, qd.Type, alloc.Get(), nullptr, IID_PPV_ARGS(&list)), "Create split list");
    ComPtr<ID3D12Fence> local;
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&local)), "Create split fence");
    HANDLE localEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    uint64_t localValue = 0;
    auto retire = [&]()
    {
        if (FAILED(queue->Signal(local.Get(), ++localValue)) ||
            FAILED(local->SetEventOnCompletion(localValue, localEvent)) ||
            WaitForSingleObject(localEvent, 30000) != WAIT_OBJECT_0 || FAILED(device->GetDeviceRemovedReason()))
        {
            std::cerr << "Split worker submission did not retire; terminating.\n" << std::flush;
            ExitProcess(3);
        }
    };

    // Evaluate inputs: the segment reads only the workspace, but the evaluate call needs its textures.
    const unsigned w = wire.width, h = wire.height;
    auto color = Texture(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, false,
                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto depth = Texture(device, w, h, DXGI_FORMAT_R32_FLOAT, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto motion = Texture(device, w, h, DXGI_FORMAT_R16G16_FLOAT, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto output = Texture(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const auto& tune = wire.passes[0];
    void* feature = create(modelPath, dataPath, device, list.Get(), params, w, h, tune.preset, tune.intensity,
                           tune.style, tune.structure, tune.tone, tune.skin, tune.autoMask, tune.uiCorrection);
    if (!feature)
        throw std::runtime_error("Split worker feature creation failed");
    Check(list->Close(), "Close split initialization");
    ID3D12CommandList* lists[] = { list.Get() };
    queue->ExecuteCommandLists(1, lists);
    retire();

    const D3D12_RESOURCE_DESC* workspace = nullptr;
    ID3D12Resource* workspaceResource = nullptr;
    for (const auto& [resource, desc] : s.allocations)
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
            (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) && (!workspace || desc.Width < workspace->Width))
        {
            workspace = &desc;
            workspaceResource = resource.Get();
        }
    if (!workspace || !plan.Fits(workspace->Width))
        throw std::runtime_error("The model workspace does not match the split plan (resolution, preset or runtime)");
    std::cerr << "Split worker: workspace " << workspace->Width << " bytes, segment (" << plan.cut << ", "
              << plan.resume << "), forward " << plan.forwardBytes << " bytes, backward " << plan.backwardBytes
              << " bytes\n" << std::flush;

    NvHandle module = nullptr;
    if (!s.createModule || !s.createFunction ||
        s.createModule(device, DlssNrVitCopyKernel::fatbin, static_cast<uint32_t>(sizeof(DlssNrVitCopyKernel::fatbin)),
                       &module) != 0 ||
        s.createFunction(device, module, "nr_vit_copy16", &s.copy) != 0)
        throw std::runtime_error("Copy kernel unavailable");
    auto forward = Buffer(device, plan.forwardBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    auto backward = Buffer(device, plan.backwardBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           true);
    if (!SetEvent(initialized))
        throw std::runtime_error("Could not signal split worker initialization");

    ID3D12Resource* shared = peer.Buffer();
    for (uint64_t frame = 1;; ++frame)
    {
        FrameCommand command {};
        if (!readExact(&command, static_cast<DWORD>(sizeof(command))))
        {
            if (WaitForSingleObject(parent, 0) == WAIT_OBJECT_0)
                break;
            throw std::runtime_error("Split worker control pipe closed unexpectedly");
        }
        if (command.size != sizeof(command))
            throw std::runtime_error("Invalid split frame control size");
        if (command.kind == CommandKind::Shutdown)
            break;
        if (command.kind != CommandKind::Frame || command.frameNumber != frame)
            throw std::runtime_error("Invalid split frame control");
        Check(alloc->Reset(), "Reset split allocator");
        Check(list->Reset(alloc.Get(), nullptr), "Reset split list");
        Transition(list.Get(), shared, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(list.Get(), forward.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(forward.Get(), 0, shared, 0, plan.forwardBytes);
        Transition(list.Get(), forward.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(list.Get(), shared, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        s.filter.Begin(Role::Segment, &plan, workspaceResource->GetGPUVirtualAddress(),
                       forward->GetGPUVirtualAddress(), backward->GetGPUVirtualAddress());
        const int status = evaluate(list.Get(), feature, params, color.Get(), depth.Get(), motion.Get(), output.Get(),
                                    w, h, w, h, w, h, 0, 0, 0, 0, 0, (command.resetMask & 1u) ? 1 : 0,
                                    command.passes[0].intensity, command.passes[0].style, command.passes[0].structure,
                                    command.passes[0].tone, command.passes[0].skin, command.passes[0].autoMask, 1.0f,
                                    1.0f);
        if (status != 1 || !s.filter.End())
            throw std::runtime_error("Split worker segment evaluate failed");
        Transition(list.Get(), backward.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(list.Get(), shared, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(shared, plan.forwardBytes, backward.Get(), 0, plan.backwardBytes);
        Transition(list.Get(), shared, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        Transition(list.Get(), backward.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Check(list->Close(), "Close split list");
        if (DlssNr::MultiGpu::WaitForFenceOrPeer(peer.Ready(), frame, parent, 30000) !=
            DlssNr::MultiGpu::WorkerFenceWaitResult::Ready)
            throw std::runtime_error("Owner exited or failed before sending the split frame");
        queue->ExecuteCommandLists(1, lists);
        if (FAILED(queue->Signal(peer.Done(), frame)))
            throw std::runtime_error("Split worker could not signal done");
        retire();
    }
    std::cerr << "Split worker finished.\n" << std::flush;
    return 0;
}
} // namespace SplitWorker
