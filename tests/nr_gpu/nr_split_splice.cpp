// Same-GPU proof that one DLSS-NR evaluate can hand a middle segment of its network to a second model instance.
//
// The network is a U-Net whose first kernel (pre_block) reads history that its last kernels (post_block, copy) wrote the
// previous frame, through textures that are not in the tensor arena. So the owner instance keeps both ends:
//   owner:  kernels [0, cut] and [resume, end), the latter deferred until the helper's segment has been recorded;
//   helper: kernel 0 (constant setup) and kernels (cut, resume), on its own feature and arena.
// At the cut, tensors the owner wrote that the segment reads are copied into the helper's arena (same layout, shifted
// base). After the segment, tensors it wrote that the owner's remaining kernels read are copied back. A third instance
// runs every kernel as the reference; the owner's output is compared with it every frame on changing input.
//
// Usage: nr_split_splice.exe <_nvngx.dll> <forwarder> <nvngx_dlssnr.dll> <data dir> <adapter> <width> <height>
//                            <preset> <cut> <resume> <frames>
//        cut 0 runs two full instances (instance-to-instance baseline). resume 0 = the last two kernels.
// Build: cl /std:c++17 /EHsc /O2 /I OptiScaler\include tests\nr_gpu\nr_split_splice.cpp
//        /link OptiScaler\library\detours\detours.lib d3d12.lib dxgi.lib user32.lib
#include "nr_nvapi_harness.h"
#include "../../OptiScaler/dlssnr/DlssNrVitCopyKernel.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

using namespace NrHarness;

namespace
{
enum class Role
{
    Full,
    Trace,
    Owner,
    Helper
};
struct Transfer
{
    uint64_t ownerVa = 0, helperVa = 0, bytes = 0, offset = 0;
};
struct Deferred
{
    CuLaunch launch {};
    std::vector<uint8_t> params;
};
Role g_role = Role::Full;
int g_index = 0, g_cut = 0, g_resume = 0;
bool g_injected = false;
std::vector<std::vector<uint64_t>>* g_trace = nullptr;
std::vector<Transfer> g_forward, g_backward;
uint64_t g_forwardVa = 0, g_backwardVa = 0;
std::vector<Deferred> g_deferred;
NvHandle g_copyFunction = nullptr;
unsigned g_copies = 0;

int Copy(ID3D12GraphicsCommandList* list, uint64_t dst, uint64_t src, uint64_t bytes)
{
    struct Params
    {
        uint64_t dst, src, n;
    } p { dst, src, bytes / 16 };
    CuLaunch k {};
    k.hFunction = g_copyFunction;
    k.gridDim = { static_cast<uint32_t>(std::min<uint64_t>((p.n + 255) / 256, 1024)), 1, 1 };
    k.blockDim = { 256, 1, 1 };
    k.pParams = &p;
    k.paramSize = sizeof(p);
    ++g_copies;
    return Real().launch(list, &k, 1);
}
// Copies a transfer set between an arena (owner's or helper's) and a staging buffer, fenced by UAV barriers.
int CopyAll(ID3D12GraphicsCommandList* list, const std::vector<Transfer>& set, uint64_t staging, bool toStaging,
            bool ownerSide)
{
    UavBarrier(list);
    for (const auto& t : set)
    {
        const uint64_t arena = ownerSide ? t.ownerVa : t.helperVa;
        const int r = toStaging ? Copy(list, staging + t.offset, arena, t.bytes)
                                : Copy(list, arena, staging + t.offset, t.bytes);
        if (r)
            return r;
    }
    UavBarrier(list);
    return 0;
}

int __cdecl Launch(ID3D12GraphicsCommandList* list, const CuLaunch* k, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i, ++g_index)
    {
        if (g_role == Role::Trace && g_trace)
        {
            std::vector<uint64_t> words;
            const auto* bytes = static_cast<const uint8_t*>(k[i].pParams);
            for (uint32_t o = 0; bytes && o + 8 <= k[i].paramSize; o += 8)
            {
                uint64_t v = 0;
                std::memcpy(&v, bytes + o, 8);
                if (v >= (1ull << 24) && v < (1ull << 40))
                    words.push_back(v);
            }
            if (g_trace->size() <= static_cast<size_t>(g_index))
                g_trace->resize(g_index + 1);
            (*g_trace)[g_index] = std::move(words);
        }
        if (g_role == Role::Owner && g_index > g_cut)
        {
            if (!g_injected)
            {
                if (int r = CopyAll(list, g_forward, g_forwardVa, true, true))
                    return r;
                g_injected = true;
            }
            if (g_index >= g_resume)
            {
                Deferred d;
                d.launch = k[i];
                d.params.assign(static_cast<const uint8_t*>(k[i].pParams),
                                static_cast<const uint8_t*>(k[i].pParams) + k[i].paramSize);
                g_deferred.push_back(std::move(d));
            }
            continue;
        }
        if (g_role == Role::Helper && ((g_index > 0 && g_index <= g_cut) || g_index >= g_resume))
            continue;
        if (g_role == Role::Helper && g_index > g_cut && !g_injected)
        {
            if (int r = CopyAll(list, g_forward, g_forwardVa, false, false))
                return r;
            g_injected = true;
        }
        if (int r = Real().launch(list, &k[i], 1))
            return r;
        if (g_role == Role::Helper && g_index + 1 == g_resume)
            if (int r = CopyAll(list, g_backward, g_backwardVa, true, false))
                return r;
    }
    return 0;
}
// The owner's held-back end of the network, after the helper's segment results are copied into its arena.
int ReplayDeferred(ID3D12GraphicsCommandList* list)
{
    if (int r = CopyAll(list, g_backward, g_backwardVa, false, true))
        return r;
    for (auto& d : g_deferred)
    {
        d.launch.pParams = d.params.data();
        if (int r = Real().launch(list, &d.launch, 1))
            return r;
    }
    g_deferred.clear();
    return 0;
}
void* Wrap(unsigned id, void* raw)
{
    return id == kLaunchChain ? reinterpret_cast<void*>(&Launch) : raw;
}

std::vector<uint16_t> Readback(ID3D12Device* device, ID3D12CommandQueue* queue, Surface& s)
{
    auto rb = MakeBuffer(device, D3D12_HEAP_TYPE_READBACK, s.bytes, D3D12_RESOURCE_STATE_COPY_DEST, false);
    ComPtr<ID3D12CommandAllocator> a;
    ComPtr<ID3D12GraphicsCommandList> l;
    Must(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)), "alloc");
    Must(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get(), nullptr, IID_PPV_ARGS(&l)), "list");
    Barrier(l.Get(), s.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
    dst.pResource = rb.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = s.footprint;
    src.pResource = s.texture.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(l.Get(), s.texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Must(l->Close(), "close");
    ID3D12CommandList* lists[] = { l.Get() };
    queue->ExecuteCommandLists(1, lists);
    WaitQueue(device, queue);
    std::vector<uint16_t> out;
    uint8_t* m = nullptr;
    D3D12_RANGE range { 0, static_cast<SIZE_T>(s.bytes) };
    Must(rb->Map(0, &range, reinterpret_cast<void**>(&m)), "map");
    for (UINT y = 0; y < s.rows; ++y)
    {
        auto* row = reinterpret_cast<const uint16_t*>(m + y * s.footprint.Footprint.RowPitch);
        out.insert(out.end(), row, row + s.footprint.Footprint.Width * 4);
    }
    D3D12_RANGE none { 0, 0 };
    rb->Unmap(0, &none);
    return out;
}
float Half(uint16_t h)
{
    const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 31, man = h & 1023;
    float v = exp == 0 ? std::ldexp(static_cast<float>(man), -24)
            : exp == 31 ? INFINITY
                        : std::ldexp(static_cast<float>(man | 1024), static_cast<int>(exp) - 25);
    return sign ? -v : v;
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc != 12)
    {
        std::fprintf(stderr, "usage: core forwarder model data adapter width height preset cut resume frames\n");
        return 2;
    }
    try
    {
        const unsigned adapter = std::stoul(argv[5]), w = std::stoul(argv[6]), h = std::stoul(argv[7]);
        const int preset = std::stoi(argv[8]);
        g_cut = std::stoi(argv[9]);
        g_resume = std::stoi(argv[10]);
        const int frames = std::stoi(argv[11]);
        std::wstring name;
        auto device = OpenNvidia(adapter, &name);
        if (!HookNvApi(&Wrap))
            throw std::runtime_error("could not hook NvAPI");
        D3D12_COMMAND_QUEUE_DESC qd {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> queue;
        Must(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12GraphicsCommandList> list;
        Must(device->CreateCommandAllocator(qd.Type, IID_PPV_ARGS(&alloc)), "alloc");
        Must(device->CreateCommandList(0, qd.Type, alloc.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
        ID3D12CommandList* lists[] = { list.Get() };
        auto begin = [&]()
        {
            Must(alloc->Reset(), "reset");
            Must(list->Reset(alloc.Get(), nullptr), "reset list");
        };
        auto submit = [&]()
        {
            Must(list->Close(), "close");
            queue->ExecuteCommandLists(1, lists);
            WaitQueue(device.Get(), queue.Get());
        };

        Model model;
        model.Load(device.Get(), argv[1], argv[2], argv[3], argv[4]);
        Surface color = MakeSurface(device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, false, w, h);
        Surface depth = MakeSurface(device.Get(), DXGI_FORMAT_R32_FLOAT, false, w, h);
        Surface motion = MakeSurface(device.Get(), DXGI_FORMAT_R16G16_FLOAT, false, w, h);
        Surface outRef = MakeSurface(device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, true, w, h);
        Surface outOwner = MakeSurface(device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, true, w, h);
        Surface outHelper = MakeSurface(device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, true, w, h);
        Upload(list.Get(), color, 0);
        Upload(list.Get(), depth, 1);
        Upload(list.Get(), motion, 2);
        for (auto* s : { &color, &depth, &motion })
            Barrier(list.Get(), s->texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for (auto* s : { &outRef, &outOwner, &outHelper })
            Barrier(list.Get(), s->texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        void* ref = model.NewFeature(device.Get(), list.Get(), w, h, preset);
        void* owner = model.NewFeature(device.Get(), list.Get(), w, h, preset);
        void* helper = model.NewFeature(device.Get(), list.Get(), w, h, preset);
        submit();

        NvHandle module = nullptr;
        if (!Real().createModule || !Real().createFunction ||
            Real().createModule(device.Get(), DlssNrVitCopyKernel::fatbin, sizeof(DlssNrVitCopyKernel::fatbin),
                                &module) != 0 ||
            Real().createFunction(device.Get(), module, "nr_vit_copy16", &g_copyFunction) != 0)
            throw std::runtime_error("copy kernel unavailable");

        // One traced evaluate per instance: addresses by kernel position.
        std::vector<std::vector<uint64_t>> traceOwner, traceHelper;
        begin();
        for (auto [feature, trace, out] :
             { std::tuple { owner, &traceOwner, &outOwner }, { helper, &traceHelper, &outHelper } })
        {
            g_role = Role::Trace;
            g_trace = trace;
            g_index = 0;
            if (model.Run(list.Get(), feature, color, depth, motion, *out, w, h, true) != 1)
                throw std::runtime_error("trace evaluate failed");
        }
        g_trace = nullptr;
        g_role = Role::Full;
        submit();
        const int kernels = static_cast<int>(traceOwner.size());
        const bool baseline = g_cut == 0;
        if (g_resume <= 0)
            g_resume = kernels - 2;
        if (traceHelper.size() != traceOwner.size() ||
            (!baseline && (g_cut < 1 || g_resume <= g_cut + 1 || g_resume >= kernels)))
            throw std::runtime_error("kernel counts differ, or cut/resume out of range");

        // Owner -> helper address map; tensor starts are 256-byte aligned words in the largest address cluster.
        std::map<uint64_t, uint64_t> toHelper;
        for (int i = 0; i < kernels; ++i)
            for (size_t j = 0; j < traceOwner[i].size() && j < traceHelper[i].size(); ++j)
                toHelper.emplace(traceOwner[i][j], traceHelper[i][j]);
        std::vector<uint64_t> arena;
        for (auto& [a, b] : toHelper)
            if ((a & 0xff) == 0)
                arena.push_back(a);
        std::sort(arena.begin(), arena.end());
        {
            size_t bestStart = 0, bestCount = 0;
            for (size_t i = 0; i < arena.size();)
            {
                size_t j = i;
                while (j + 1 < arena.size() && arena[j + 1] - arena[j] < (256ull << 20))
                    ++j;
                if (j - i + 1 > bestCount)
                {
                    bestStart = i;
                    bestCount = j - i + 1;
                }
                i = j + 1;
            }
            arena = std::vector<uint64_t>(arena.begin() + bestStart, arena.begin() + bestStart + bestCount);
        }
        // Segments using each tensor: 1 = owner head [0, cut], 2 = helper (cut, resume), 4 = owner end [resume, end).
        // Any address inside a tensor counts as a use of it.
        std::map<uint64_t, unsigned> segments;
        for (int i = 0; i < kernels; ++i)
            for (uint64_t a : traceOwner[i])
            {
                auto it = std::upper_bound(arena.begin(), arena.end(), a);
                if (it == arena.begin() || (it == arena.end() && a != *(it - 1)))
                    continue;
                segments[*(it - 1)] |= i <= g_cut ? 1u : i < g_resume ? 2u : 4u;
            }
        uint64_t forwardBytes = 0, backwardBytes = 0;
        for (size_t i = 0; !baseline && i + 1 < arena.size(); ++i)
        {
            const uint64_t a = arena[i];
            const unsigned s = segments.count(a) ? segments[a] : 0u;
            Transfer t;
            t.ownerVa = a;
            t.helperVa = toHelper[a];
            t.bytes = (arena[i + 1] - a) & ~15ull;
            if ((s & 1) && (s & 2))
            {
                t.offset = forwardBytes;
                forwardBytes += (t.bytes + 255) & ~255ull;
                g_forward.push_back(t);
            }
            if ((s & 2) && (s & 4))
            {
                t.offset = backwardBytes;
                backwardBytes += (t.bytes + 255) & ~255ull;
                g_backward.push_back(t);
            }
        }
        auto forwardBuffer = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, std::max<uint64_t>(forwardBytes, 256),
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        auto backwardBuffer = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT,
                                         std::max<uint64_t>(backwardBytes, 256), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                         true);
        g_forwardVa = forwardBuffer->GetGPUVirtualAddress();
        g_backwardVa = backwardBuffer->GetGPUVirtualAddress();
        std::printf("Adapter %ls, %ux%u preset %d: %d kernels; owner [0..%d] + [%d..%d], helper [%d..%d]\n",
                    name.c_str(), w, h, preset, kernels, g_cut, g_resume, kernels - 1, g_cut + 1, g_resume - 1);
        std::printf("Forward %zu tensors %.2f MiB, backward %zu tensors %.2f MiB per evaluate\n", g_forward.size(),
                    forwardBytes / 1048576.0, g_backward.size(), backwardBytes / 1048576.0);

        int failures = 0;
        for (int f = 1; f <= frames; ++f)
        {
            begin();
            Barrier(list.Get(), color.texture.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_DEST);
            Upload(list.Get(), color, 0, 100 + f); // a new picture every frame exercises temporal state
            Barrier(list.Get(), color.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            g_copies = 0;
            for (auto [role, feature, out] : { std::tuple { Role::Full, ref, &outRef },
                                               { baseline ? Role::Full : Role::Owner, owner, &outOwner },
                                               { baseline ? Role::Full : Role::Helper, helper, &outHelper } })
            {
                g_role = role;
                g_index = 0;
                g_injected = false;
                if (model.Run(list.Get(), feature, color, depth, motion, *out, w, h, f == 1) != 1)
                    throw std::runtime_error("evaluate failed");
            }
            g_role = Role::Full;
            if (!baseline && ReplayDeferred(list.Get()) != 0)
                throw std::runtime_error("replay failed");
            submit();
            const auto a = Readback(device.Get(), queue.Get(), outRef);
            const auto b = Readback(device.Get(), queue.Get(), baseline ? outHelper : outOwner);
            double maxDiff = 0, sum = 0;
            size_t exact = 0, n = 0;
            for (size_t i = 0; i < a.size(); ++i)
            {
                if (i % 4 == 3)
                    continue;
                const double d = std::fabs(double(Half(a[i])) - double(Half(b[i])));
                maxDiff = std::max(maxDiff, d);
                sum += d;
                exact += a[i] == b[i];
                ++n;
            }
            std::printf("frame %2d: max |diff| %.6f, mean %.8f, bit-identical %.4f%%, %u copy kernels\n", f, maxDiff,
                        sum / n, 100.0 * exact / n, g_copies);
            failures += maxDiff > 0.01;
        }
        std::printf(failures ? "Split output differs from the reference in %d frame(s)\n"
                             : "Split output matches the reference\n",
                    failures);
        std::fflush(nullptr);
        ExitProcess(failures ? 1 : 0);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        std::fflush(nullptr);
        return 1;
    }
}
