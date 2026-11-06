// Helper side of the cross-GPU DLSS-NR split prototype. Started by nr_split_owner through WorkerClient (same command line
// and handle handoff as the production worker), it runs on the helper adapter with its own model instance:
//   kernel 0 (counter clear) and kernels (cut, resume) of every evaluate, all other kernels filtered out;
//   before kernel cut + 1, the forward ranges arriving in the shared buffer are copied into its workspace;
//   after kernel resume - 1, the backward ranges are copied out of its workspace into the shared buffer.
// Workspace offsets are identical on both GPUs (docs/NR-network-trace.md). The split is described by split-config.txt in
// the data directory (its working directory):  "C <cut> <resume>", then "F <offset> <bytes>" and "B <offset> <bytes>".
// Shared buffer layout: forward ranges packed from 0, backward ranges packed after them.
#include "nr_nvapi_harness.h"
#include "../../OptiScaler/dlssnr/DlssNrVitCopyKernel.h"
#include "../../OptiScaler/dlssnr/DlssNr_CrossAdapterIpc.h"
#include "../../OptiScaler/dlssnr/DlssNr_WorkerFenceWait.h"
#include "../../OptiScaler/dlssnr/worker/WorkerProtocol.h"

#include <algorithm>
#include <fstream>
#include <sstream>

using namespace NrHarness;

namespace
{
struct Range
{
    uint64_t offset = 0, bytes = 0, packed = 0;
};
bool g_active = false, g_injected = false;
int g_index = 0, g_cut = 0, g_resume = 0;
std::vector<Range> g_forward, g_backward;
uint64_t g_wsVa = 0, g_fwdVa = 0, g_bwdVa = 0;
NvHandle g_copy = nullptr;

int Copy(ID3D12GraphicsCommandList* list, uint64_t dst, uint64_t src, uint64_t bytes)
{
    struct
    {
        uint64_t dst, src, n;
    } p { dst, src, bytes / 16 };
    CuLaunch k {};
    k.hFunction = g_copy;
    k.gridDim = { static_cast<uint32_t>(std::min<uint64_t>((p.n + 255) / 256, 1024)), 1, 1 };
    k.blockDim = { 256, 1, 1 };
    k.pParams = &p;
    k.paramSize = sizeof(p);
    return Real().launch(list, &k, 1);
}
int CopySet(ID3D12GraphicsCommandList* list, const std::vector<Range>& set, uint64_t staging, bool intoWorkspace)
{
    UavBarrier(list);
    for (const auto& r : set)
        if (int e = intoWorkspace ? Copy(list, g_wsVa + r.offset, staging + r.packed, r.bytes)
                                  : Copy(list, staging + r.packed, g_wsVa + r.offset, r.bytes))
            return e;
    UavBarrier(list);
    return 0;
}
int __cdecl Launch(ID3D12GraphicsCommandList* list, const CuLaunch* k, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i, ++g_index)
    {
        if (g_active && ((g_index > 0 && g_index <= g_cut) || g_index >= g_resume))
            continue;
        if (g_active && g_index > g_cut && !g_injected)
        {
            if (int e = CopySet(list, g_forward, g_fwdVa, true))
                return e;
            g_injected = true;
        }
        if (int e = Real().launch(list, &k[i], 1))
            return e;
        if (g_active && g_index + 1 == g_resume)
            if (int e = CopySet(list, g_backward, g_bwdVa, false))
                return e;
    }
    return 0;
}
void* Wrap(unsigned id, void* raw)
{
    return id == kLaunchChain ? reinterpret_cast<void*>(&Launch) : raw;
}
double Median(std::vector<double> v)
{
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc != 8 || std::wstring(argv[1]) != L"--worker")
        return 2;
    FILE* unused = nullptr;
    _wfreopen_s(&unused, argv[7], L"w", stderr);
    try
    {
        HANDLE pipe = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wcstoui64(argv[2], nullptr, 10)));
        auto readExact = [&](void* buffer, DWORD bytes)
        {
            DWORD got = 0;
            while (got < bytes)
            {
                DWORD part = 0;
                if (!ReadFile(pipe, static_cast<uint8_t*>(buffer) + got, bytes - got, &part, nullptr) || !part)
                    return false;
                got += part;
            }
            return true;
        };
        DlssNr::Worker::InitialFrameWire wire {};
        if (!readExact(&wire, sizeof(wire)) || wire.magic != DlssNr::Worker::ProtocolMagic || wire.size != sizeof(wire))
            throw std::runtime_error("bad handshake");
        const HANDLE parent = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(wire.parent));
        const HANDLE initialized = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(wire.initialized));

        ComPtr<IDXGIFactory4> factory;
        Must(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
        ComPtr<IDXGIAdapter1> adapter;
        Must(factory->EnumAdapterByLuid(wire.secondary, IID_PPV_ARGS(&adapter)), "adapter");
        ComPtr<ID3D12Device> device;
        Must(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
        DlssNr::MultiGpu::CrossAdapterPeer peer;
        Must(DlssNr::MultiGpu::CrossAdapterPeer::Open(device.Get(), wire.primary, wire.secondary, wire.bytes,
                                                      reinterpret_cast<HANDLE>(static_cast<uintptr_t>(wire.heap)),
                                                      reinterpret_cast<HANDLE>(static_cast<uintptr_t>(wire.ready)),
                                                      reinterpret_cast<HANDLE>(static_cast<uintptr_t>(wire.done)), peer),
             "open shared buffer");

        // Split description.
        std::ifstream config("split-config.txt");
        std::string line;
        uint64_t fwdBytes = 0, bwdBytes = 0;
        while (std::getline(config, line))
        {
            std::istringstream in(line);
            char kind = 0;
            in >> kind;
            if (kind == 'C')
                in >> g_cut >> g_resume;
            else if (kind == 'F' || kind == 'B')
            {
                Range r;
                in >> r.offset >> r.bytes;
                auto& total = kind == 'F' ? fwdBytes : bwdBytes;
                r.packed = total;
                total += r.bytes;
                (kind == 'F' ? g_forward : g_backward).push_back(r);
            }
        }
        if (!g_cut || g_resume <= g_cut || fwdBytes + bwdBytes > wire.bytes)
            throw std::runtime_error("bad split-config.txt");

        if (!HookNvApi(&Wrap))
            throw std::runtime_error("NvAPI hook");
        _wputenv(L"NR_ALLOC_CALLBACK=1");
        D3D12_COMMAND_QUEUE_DESC qd {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> queue;
        Must(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12GraphicsCommandList> list;
        Must(device->CreateCommandAllocator(qd.Type, IID_PPV_ARGS(&alloc)), "alloc");
        Must(device->CreateCommandList(0, qd.Type, alloc.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
        Model model;
        model.Load(device.Get(), argv[3], argv[4], argv[5], argv[6]);
        const unsigned w = wire.width, h = wire.height;
        Surface color = MakeSurface(device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, false, w, h);
        Surface depth = MakeSurface(device.Get(), DXGI_FORMAT_R32_FLOAT, false, w, h);
        Surface motion = MakeSurface(device.Get(), DXGI_FORMAT_R16G16_FLOAT, false, w, h);
        Surface out = MakeSurface(device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, true, w, h);
        for (auto* s : { &color, &depth, &motion })
            Barrier(list.Get(), s->texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(list.Get(), out.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Allocations().tag = 1;
        void* feature = model.NewFeature(device.Get(), list.Get(), w, h, wire.passes[0].preset);
        Allocations().tag = 0;
        Must(list->Close(), "close");
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        WaitQueue(device.Get(), queue.Get());

        const Allocation* ws = nullptr;
        for (const auto& a : Allocations().items)
            if (a.tag == 1 && a.desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
                (a.desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) && (!ws || a.desc.Width < ws->desc.Width))
                ws = &a;
        if (!ws)
            throw std::runtime_error("model workspace not seen");
        g_wsVa = ws->va;
        for (const auto* set : { &g_forward, &g_backward })
            for (const auto& r : *set)
                if (r.offset + r.bytes > ws->desc.Width)
                    throw std::runtime_error("a range lies outside this GPU's workspace: layouts differ");
        std::fprintf(stderr, "Helper workspace %.2f MiB; segment (%d, %d); forward %.2f MiB, backward %.2f MiB\n",
                     ws->desc.Width / 1048576.0, g_cut, g_resume, fwdBytes / 1048576.0, bwdBytes / 1048576.0);

        NvHandle module = nullptr;
        if (Real().createModule(device.Get(), DlssNrVitCopyKernel::fatbin, sizeof(DlssNrVitCopyKernel::fatbin),
                                &module) != 0 ||
            Real().createFunction(device.Get(), module, "nr_vit_copy16", &g_copy) != 0)
            throw std::runtime_error("copy kernel");
        auto fwd = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, std::max<uint64_t>(fwdBytes, 256),
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        auto bwd = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, std::max<uint64_t>(bwdBytes, 256),
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        g_fwdVa = fwd->GetGPUVirtualAddress();
        g_bwdVa = bwd->GetGPUVirtualAddress();

        ComPtr<ID3D12QueryHeap> queries;
        D3D12_QUERY_HEAP_DESC qh { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 4, 0 };
        Must(device->CreateQueryHeap(&qh, IID_PPV_ARGS(&queries)), "queries");
        auto stamps = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 32, D3D12_RESOURCE_STATE_COPY_DEST, false);
        UINT64 frequency = 1;
        queue->GetTimestampFrequency(&frequency);
        std::vector<double> copyIn, segment, copyOut;
        if (!SetEvent(initialized))
            throw std::runtime_error("initialized event");

        ID3D12Resource* shared = peer.Buffer();
        for (uint64_t frame = 1;; ++frame)
        {
            DlssNr::Worker::FrameCommand command {};
            if (!readExact(&command, sizeof(command)) || command.kind == DlssNr::Worker::CommandKind::Shutdown)
                break;
            Must(alloc->Reset(), "reset");
            Must(list->Reset(alloc.Get(), nullptr), "reset list");
            list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
            Barrier(list.Get(), shared, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(list.Get(), fwd.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyBufferRegion(fwd.Get(), 0, shared, 0, fwdBytes);
            Barrier(list.Get(), fwd.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Barrier(list.Get(), shared, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            g_active = true;
            g_index = 0;
            g_injected = false;
            if (model.Run(list.Get(), feature, color, depth, motion, out, w, h, (command.resetMask & 1) != 0) != 1)
                throw std::runtime_error("helper evaluate failed");
            g_active = false;
            list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2);
            Barrier(list.Get(), bwd.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(list.Get(), shared, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyBufferRegion(shared, fwdBytes, bwd.Get(), 0, bwdBytes);
            Barrier(list.Get(), shared, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            Barrier(list.Get(), bwd.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 3);
            list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 4, stamps.Get(), 0);
            Must(list->Close(), "close");
            if (DlssNr::MultiGpu::WaitForFenceOrPeer(peer.Ready(), frame, parent, 30000) !=
                DlssNr::MultiGpu::WorkerFenceWaitResult::Ready)
                throw std::runtime_error("owner did not send the frame");
            queue->ExecuteCommandLists(1, lists);
            Must(queue->Signal(peer.Done(), frame), "signal done");
            WaitQueue(device.Get(), queue.Get());
            uint64_t* t = nullptr;
            D3D12_RANGE range { 0, 32 };
            Must(stamps->Map(0, &range, reinterpret_cast<void**>(&t)), "map");
            copyIn.push_back(double(t[1] - t[0]) * 1000.0 / frequency);
            segment.push_back(double(t[2] - t[1]) * 1000.0 / frequency);
            copyOut.push_back(double(t[3] - t[2]) * 1000.0 / frequency);
            D3D12_RANGE none { 0, 0 };
            stamps->Unmap(0, &none);
        }
        std::fprintf(stderr, "Helper GPU median: copy in %.3f ms, segment %.3f ms, copy out %.3f ms over %zu frames\n",
                     Median(copyIn), Median(segment), Median(copyOut), segment.size());
        std::fflush(nullptr);
        ExitProcess(0);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "helper error: %s\n", e.what());
        std::fflush(nullptr);
        return 1;
    }
}
