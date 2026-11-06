// Owner side of the cross-GPU DLSS-NR split prototype (game GPU). Per picture:
//   1. owner evaluate #1 runs kernels [0, cut]; the forward ranges of its workspace go to the shared buffer;
//   2. the queue signals "ready"; nr_split_helper (helper GPU, own process, started through WorkerClient) runs
//      kernel 0 and (cut, resume) on its own instance and writes the backward ranges into the shared buffer;
//   3. this queue waits on the helper's "done" fence (a queue-level wait), pulls the backward ranges into the workspace,
//      and owner evaluate #2 runs kernel 0 and [resume, end): the output kernels and the history update stay here.
// A full reference instance on the owner GPU gives the error of every picture. GPU timestamps time each owner stage.
// The ranges are workspace offsets from the single-GPU minimizer (nr_split_splice SPLICE_MINIMIZE: split-sets.txt).
//
// Usage: nr_split_owner.exe <_nvngx.dll> <forwarder> <owner model> <helper model> <helper exe> <data dir> <width>
//                           <height> <preset> <cut> <resume> <frames> <split-sets.txt>
#include "nr_nvapi_harness.h"
#include "../../OptiScaler/dlssnr/DlssNrVitCopyKernel.h"
#include "../../OptiScaler/dlssnr/worker/WorkerClient.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>
#include <tuple>

using namespace NrHarness;

namespace
{
enum class Role
{
    Full,
    Head,
    End
};
struct Range
{
    uint64_t offset = 0, bytes = 0, packed = 0;
};
Role g_role = Role::Full;
bool g_injected = false;
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
        if (g_role == Role::Head && g_index > g_cut)
        {
            if (!g_injected)
            {
                if (int e = CopySet(list, g_forward, g_fwdVa, false))
                    return e;
                g_injected = true;
            }
            continue;
        }
        if (g_role == Role::End && g_index > 0 && g_index < g_resume)
            continue;
        if (g_role == Role::End && g_index >= g_resume && !g_injected)
        {
            if (int e = CopySet(list, g_backward, g_bwdVa, true))
                return e;
            g_injected = true;
        }
        if (int e = Real().launch(list, &k[i], 1))
            return e;
    }
    return 0;
}
void* Wrap(unsigned id, void* raw)
{
    return id == kLaunchChain ? reinterpret_cast<void*>(&Launch) : raw;
}
float Half(uint16_t h)
{
    const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 31, man = h & 1023;
    float v = exp == 0 ? std::ldexp(static_cast<float>(man), -24)
            : exp == 31 ? INFINITY
                        : std::ldexp(static_cast<float>(man | 1024), static_cast<int>(exp) - 25);
    return sign ? -v : v;
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
    if (argc != 14)
    {
        std::fprintf(stderr, "usage: core forwarder ownerModel helperModel helperExe data w h preset cut resume frames sets\n");
        return 2;
    }
    try
    {
        const unsigned w = std::stoul(argv[7]), h = std::stoul(argv[8]);
        const int preset = std::stoi(argv[9]);
        g_cut = std::stoi(argv[10]);
        g_resume = std::stoi(argv[11]);
        const int frames = std::stoi(argv[12]);
        const std::wstring data = argv[6];

        // Ranges, packed in order into the forward and backward parts of the shared buffer.
        uint64_t fwdBytes = 0, bwdBytes = 0;
        {
            std::ifstream in(argv[13]);
            std::string line;
            while (std::getline(in, line))
            {
                std::istringstream l(line);
                char kind = 0;
                Range r;
                l >> kind >> r.offset >> r.bytes;
                if (kind != 'F' && kind != 'B')
                    continue;
                auto& total = kind == 'F' ? fwdBytes : bwdBytes;
                r.packed = total;
                total += r.bytes;
                (kind == 'F' ? g_forward : g_backward).push_back(r);
            }
            std::ofstream config(data + L"\\split-config.txt");
            config << "C " << g_cut << ' ' << g_resume << '\n';
            for (const auto& r : g_forward)
                config << "F " << r.offset << ' ' << r.bytes << '\n';
            for (const auto& r : g_backward)
                config << "B " << r.offset << ' ' << r.bytes << '\n';
        }
        if (g_forward.empty() || g_backward.empty())
            throw std::runtime_error("no ranges in the sets file");

        std::wstring ownerName, helperName;
        auto device = OpenNvidia(0, &ownerName);
        auto secondary = OpenNvidia(1, &helperName);
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
        ID3D12CommandList* lists[] = { list.Get() };
        // PCIe transfers run on a copy queue so the direct queue only carries network work. xfer orders the queues:
        // odd values = head finished (direct -> copy), even values = backward data landed (copy -> direct).
        D3D12_COMMAND_QUEUE_DESC cd {};
        cd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        ComPtr<ID3D12CommandQueue> copyQueue;
        Must(device->CreateCommandQueue(&cd, IID_PPV_ARGS(&copyQueue)), "copy queue");
        ComPtr<ID3D12CommandAllocator> copyAllocOut, copyAllocIn;
        ComPtr<ID3D12GraphicsCommandList> copyOut, copyIn;
        Must(device->CreateCommandAllocator(cd.Type, IID_PPV_ARGS(&copyAllocOut)), "copy alloc");
        Must(device->CreateCommandAllocator(cd.Type, IID_PPV_ARGS(&copyAllocIn)), "copy alloc");
        Must(device->CreateCommandList(0, cd.Type, copyAllocOut.Get(), nullptr, IID_PPV_ARGS(&copyOut)), "copy list");
        Must(device->CreateCommandList(0, cd.Type, copyAllocIn.Get(), nullptr, IID_PPV_ARGS(&copyIn)), "copy list");
        Must(copyOut->Close(), "close");
        Must(copyIn->Close(), "close");
        ComPtr<ID3D12Fence> xfer;
        Must(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&xfer)), "xfer fence");
        ComPtr<ID3D12Fence> retire;
        Must(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&retire)), "fence");
        HANDLE retireEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        uint64_t retireValue = 0;
        auto begin = [&]()
        {
            Must(alloc->Reset(), "reset");
            Must(list->Reset(alloc.Get(), nullptr), "reset list");
        };

        Model model;
        model.Load(device.Get(), argv[1], argv[2], argv[3], data.c_str());
        Surface color = MakeSurface(device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, false, w, h);
        Surface depth = MakeSurface(device.Get(), DXGI_FORMAT_R32_FLOAT, false, w, h);
        Surface motion = MakeSurface(device.Get(), DXGI_FORMAT_R16G16_FLOAT, false, w, h);
        Surface outRef = MakeSurface(device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, true, w, h);
        Surface outOwner = MakeSurface(device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, true, w, h);
        Upload(list.Get(), color, 0);
        Upload(list.Get(), depth, 1);
        Upload(list.Get(), motion, 2);
        for (auto* s : { &color, &depth, &motion })
            Barrier(list.Get(), s->texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for (auto* s : { &outRef, &outOwner })
            Barrier(list.Get(), s->texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Allocations().tag = 1;
        void* ref = model.NewFeature(device.Get(), list.Get(), w, h, preset);
        Allocations().tag = 2;
        void* owner = model.NewFeature(device.Get(), list.Get(), w, h, preset);
        Allocations().tag = 0;
        Must(list->Close(), "close");
        queue->ExecuteCommandLists(1, lists);
        WaitQueue(device.Get(), queue.Get());
        const Allocation* ws = nullptr;
        for (const auto& a : Allocations().items)
            if (a.tag == 2 && a.desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
                (a.desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) && (!ws || a.desc.Width < ws->desc.Width))
                ws = &a;
        if (!ws)
            throw std::runtime_error("owner workspace not seen");
        g_wsVa = ws->va;

        NvHandle module = nullptr;
        if (Real().createModule(device.Get(), DlssNrVitCopyKernel::fatbin, sizeof(DlssNrVitCopyKernel::fatbin),
                                &module) != 0 ||
            Real().createFunction(device.Get(), module, "nr_vit_copy16", &g_copy) != 0)
            throw std::runtime_error("copy kernel");
        auto fwd = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, fwdBytes, D3D12_RESOURCE_STATE_COMMON, true);
        auto bwd = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, bwdBytes, D3D12_RESOURCE_STATE_COMMON, true);
        g_fwdVa = fwd->GetGPUVirtualAddress();
        g_bwdVa = bwd->GetGPUVirtualAddress();

        // Shared buffer and helper process.
        DlssNr::MultiGpu::CrossAdapterBuffer transfer;
        Must(DlssNr::MultiGpu::CrossAdapterBuffer::Create(device.Get(), secondary.Get(), fwdBytes + bwdBytes, transfer),
             "shared buffer");
        DlssNr::Worker::InitialFrameWire wire {};
        wire.width = w;
        wire.height = h;
        wire.frameCount = 0;
        wire.passCount = 1;
        wire.passes[0].preset = preset;
        wire.bytes = transfer.Size();
        wire.primary = device->GetAdapterLuid();
        wire.secondary = secondary->GetAdapterLuid();
        DlssNr::Worker::WorkerClient client;
        Must(client.Start(device.Get(), argv[5], argv[1], argv[2], argv[4], data.c_str(),
                          (data + L"\\helper.log").c_str(), transfer, wire),
             "start helper");
        HRESULT ready = E_PENDING;
        for (int i = 0; i < 1200 && ready == E_PENDING; ++i)
            ready = client.WaitForReady(100);
        Must(ready, "helper initialization");

        ComPtr<ID3D12QueryHeap> queries;
        D3D12_QUERY_HEAP_DESC qh { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 8, 0 };
        Must(device->CreateQueryHeap(&qh, IID_PPV_ARGS(&queries)), "queries");
        auto stamps = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 64, D3D12_RESOURCE_STATE_COPY_DEST, false);
        UINT64 frequency = 1;
        queue->GetTimestampFrequency(&frequency);
        std::vector<double> refMs, headMs, outMs, waitMs, inMs, endMs, wallMs;
        ID3D12Resource* shared = transfer.PrimaryBuffer();
        std::printf("Owner %ls, helper %ls, %ux%u preset %d: owner [0..%d] + [%d..], helper (%d, %d); forward %.2f MiB, "
                    "backward %.2f MiB\n",
                    ownerName.c_str(), helperName.c_str(), w, h, preset, g_cut, g_resume, g_cut, g_resume,
                    fwdBytes / 1048576.0, bwdBytes / 1048576.0);
        int failures = 0;
        for (int f = 1; f <= frames; ++f)
        {
            const auto started = std::chrono::steady_clock::now();
            // List A: input, reference, owner head, forward ranges out.
            begin();
            Barrier(list.Get(), color.texture.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_DEST);
            Upload(list.Get(), color, 0, 100 + f);
            Barrier(list.Get(), color.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
            g_role = Role::Full;
            g_index = 0;
            if (model.Run(list.Get(), ref, color, depth, motion, outRef, w, h, f == 1) != 1)
                throw std::runtime_error("reference evaluate failed");
            list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            if (f == 1)
                Barrier(list.Get(), fwd.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            g_role = Role::Head;
            g_index = 0;
            g_injected = false;
            if (model.Run(list.Get(), owner, color, depth, motion, outOwner, w, h, f == 1) != 1)
                throw std::runtime_error("owner head evaluate failed");
            list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2);
            Barrier(list.Get(), fwd.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            Must(list->Close(), "close A");
            queue->ExecuteCommandLists(1, lists);
            Must(queue->Signal(xfer.Get(), 2ull * f - 1), "head done");
            // Copy queue: forward data to the helper's shared buffer, then "ready".
            Must(copyAllocOut->Reset(), "copy reset");
            Must(copyOut->Reset(copyAllocOut.Get(), nullptr), "copy list reset");
            copyOut->CopyBufferRegion(shared, 0, fwd.Get(), 0, fwdBytes);
            Must(copyOut->Close(), "copy close");
            Must(copyQueue->Wait(xfer.Get(), 2ull * f - 1), "copy wait head");
            ID3D12CommandList* outLists[] = { copyOut.Get() };
            copyQueue->ExecuteCommandLists(1, outLists);
            Must(copyQueue->Signal(transfer.PrimaryReady(), f), "ready");
            DlssNr::Worker::FrameCommand command {};
            command.frameNumber = static_cast<uint64_t>(f);
            command.resetMask = f == 1 ? 1u : 0u;
            command.guideWidth = command.motionWidth = w;
            command.guideHeight = command.motionHeight = h;
            Must(client.SendFrame(command), "send frame");
            // Copy queue waits for the helper (queue-level, no CPU block, no GPU spin) and pulls the backward data.
            Must(copyAllocIn->Reset(), "copy reset");
            Must(copyIn->Reset(copyAllocIn.Get(), nullptr), "copy list reset");
            copyIn->CopyBufferRegion(bwd.Get(), 0, shared, fwdBytes, bwdBytes);
            Must(copyIn->Close(), "copy close");
            Must(copyQueue->Wait(transfer.PrimaryDone(), f), "copy wait helper");
            ID3D12CommandList* inLists[] = { copyIn.Get() };
            copyQueue->ExecuteCommandLists(1, inLists);
            Must(copyQueue->Signal(xfer.Get(), 2ull * f), "backward landed");
            Must(queue->Wait(xfer.Get(), 2ull * f), "direct wait backward");
            // List B: owner end (output kernels, history update) after the backward data landed.
            WaitQueue(device.Get(), queue.Get()); // list A and the copies retired: the allocator can be reset
            begin();
            list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 3);
            Barrier(list.Get(), bwd.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Barrier(list.Get(), fwd.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 4);
            g_role = Role::End;
            g_index = 0;
            g_injected = false;
            if (model.Run(list.Get(), owner, color, depth, motion, outOwner, w, h, false) != 1)
                throw std::runtime_error("owner end evaluate failed");
            g_role = Role::Full;
            list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 5);
            Barrier(list.Get(), bwd.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 6, stamps.Get(), 0);
            Must(list->Close(), "close B");
            queue->ExecuteCommandLists(1, lists);
            Must(queue->Signal(retire.Get(), ++retireValue), "retire");
            Must(retire->SetEventOnCompletion(retireValue, retireEvent), "retire event");
            if (WaitForSingleObject(retireEvent, 10000) != WAIT_OBJECT_0)
                throw std::runtime_error("picture did not retire (helper stalled?)");
            wallMs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
            if (client.WaitForReturn(static_cast<uint64_t>(f), 1000) != DlssNr::MultiGpu::WorkerFenceWaitResult::Ready ||
                FAILED(client.AcknowledgeReturn(static_cast<uint64_t>(f), retire.Get(), retireValue)))
                throw std::runtime_error("helper return bookkeeping failed");
            uint64_t* t = nullptr;
            D3D12_RANGE range { 0, 48 };
            Must(stamps->Map(0, &range, reinterpret_cast<void**>(&t)), "map");
            auto ms = [&](int a, int b) { return double(t[b] - t[a]) * 1000.0 / frequency; };
            refMs.push_back(ms(0, 1));
            headMs.push_back(ms(1, 2));
            inMs.push_back(ms(3, 4));
            endMs.push_back(ms(4, 5));
            waitMs.push_back(ms(2, 3));
            D3D12_RANGE none { 0, 0 };
            stamps->Unmap(0, &none);

            // Error against the reference.
            auto readback = [&](Surface& s)
            {
                auto rb = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_READBACK, s.bytes, D3D12_RESOURCE_STATE_COPY_DEST, false);
                begin();
                Barrier(list.Get(), s.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
                dst.pResource = rb.Get();
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint = s.footprint;
                src.pResource = s.texture.Get();
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                Barrier(list.Get(), s.texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                Must(list->Close(), "close readback");
                queue->ExecuteCommandLists(1, lists);
                WaitQueue(device.Get(), queue.Get());
                std::vector<uint16_t> v;
                uint8_t* m = nullptr;
                D3D12_RANGE r { 0, static_cast<SIZE_T>(s.bytes) };
                Must(rb->Map(0, &r, reinterpret_cast<void**>(&m)), "map");
                for (UINT y = 0; y < s.rows; ++y)
                {
                    auto* row = reinterpret_cast<const uint16_t*>(m + y * s.footprint.Footprint.RowPitch);
                    v.insert(v.end(), row, row + s.footprint.Footprint.Width * 4);
                }
                D3D12_RANGE z { 0, 0 };
                rb->Unmap(0, &z);
                return v;
            };
            const auto a = readback(outRef);
            const auto b = readback(outOwner);
            double mx = 0, sum = 0;
            size_t n = 0;
            for (size_t i = 0; i < a.size(); ++i)
                if (i % 4 != 3)
                {
                    const double d = std::fabs(double(Half(a[i])) - double(Half(b[i])));
                    mx = std::max(mx, d);
                    sum += d;
                    ++n;
                }
            std::printf("frame %2d: max |diff| %.5f, mean %.7f vs single-GPU reference; wall %.2f ms\n", f, mx, sum / n,
                        wallMs.back());
            failures += mx > 0.05;
        }
        std::printf("Owner direct queue median: full reference %.3f ms; head %.3f ms; end %.3f ms (+%.3f ms barriers); "
                    "head-to-end gap %.3f ms (copies on the copy queue + helper)\n",
                    Median(refMs), Median(headMs), Median(endMs), Median(inMs), Median(waitMs));
        std::printf("Direct-queue NR time per picture: split %.3f ms vs single GPU %.3f ms\n",
                    Median(headMs) + Median(endMs) + Median(inMs), Median(refMs));
        std::printf("Picture wall time median %.2f ms (sequential prototype: no overlap between pictures)\n",
                    Median(wallMs));
        client.Stop(5000);
        std::ifstream log(data + L"\\helper.log");
        for (std::string line; std::getline(log, line);)
            if (line.find("Helper") != std::string::npos || line.find("error") != std::string::npos)
                std::printf("  [helper] %s\n", line.c_str());
        std::printf(failures ? "Cross-GPU split exceeded the error bound in %d picture(s)\n"
                             : "Cross-GPU split within bounds\n",
                    failures);
        std::fflush(nullptr);
        ExitProcess(failures ? 1 : 0);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "owner error: %s\n", e.what());
        std::fflush(nullptr);
        return 1;
    }
}
