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
    Helper,
    OwnerEnd, // second evaluate of the owner: kernel 0 and [resume, end), after the helper's results are copied in
    Probe     // write-set discovery: read-only snapshots of the workspace before and after each segment
};
// Write-set discovery state (SPLICE_DISCOVER): which workspace bytes each segment writes.
struct ProbeState
{
    uint64_t wsVa = 0, wsBytes = 0, snapStartVa = 0, snapHeadVa = 0, snapMidVa = 0;
};
ProbeState g_probe;
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
bool g_injected = false, g_twoCalls = true;
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
            if (!g_twoCalls && g_index >= g_resume)
            {
                Deferred d;
                d.launch = k[i];
                d.params.assign(static_cast<const uint8_t*>(k[i].pParams),
                                static_cast<const uint8_t*>(k[i].pParams) + k[i].paramSize);
                g_deferred.push_back(std::move(d));
            }
            continue;
        }
        if (g_role == Role::Probe)
        {
            // Never write the workspace here: it holds the model's own synchronization counters.
            if (g_index == 1)
            {
                UavBarrier(list);
                if (int r = Copy(list, g_probe.snapStartVa, g_probe.wsVa, g_probe.wsBytes))
                    return r;
                UavBarrier(list);
            }
            if (int r = Real().launch(list, &k[i], 1))
                return r;
            if (g_index == g_cut || g_index + 1 == g_resume)
            {
                UavBarrier(list);
                if (int r = Copy(list, g_index == g_cut ? g_probe.snapHeadVa : g_probe.snapMidVa, g_probe.wsVa,
                                 g_probe.wsBytes))
                    return r;
                UavBarrier(list);
            }
            continue;
        }
        if (g_role == Role::Helper && ((g_index > 0 && g_index <= g_cut) || g_index >= g_resume))
            continue;
        if (g_role == Role::OwnerEnd && g_index > 0 && g_index < g_resume)
            continue;
        if (g_role == Role::OwnerEnd && g_index >= g_resume && !g_injected)
        {
            if (int r = CopyAll(list, g_backward, g_backwardVa, false, true))
                return r;
            g_injected = true;
        }
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
// SPLICE_LOGCMD: the model's own non-kernel commands on our list (copies, barriers, clears, dispatches), with the
// kernel index they precede, so a second evaluate per picture can be checked for repeated history work.
struct CommandEvent
{
    const char* what;
    void* dst;
    void* src;
    int kernel;
    Role role;
};
std::vector<CommandEvent> g_commands;
bool g_logCommands = false;
using PFN_Slot = void(STDMETHODCALLTYPE*)(void*, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
PFN_Slot g_listOriginal[64] {};
template<int Slot, int Dst, int Src>
void STDMETHODCALLTYPE ListHook(void* self, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6,
                                uint64_t a7)
{
    if (g_logCommands)
    {
        static const char* names[64] {};
        names[14] = "Dispatch";
        names[15] = "CopyBufferRegion";
        names[16] = "CopyTextureRegion";
        names[17] = "CopyResource";
        names[26] = "ResourceBarrier";
        names[49] = "ClearUavUint";
        names[50] = "ClearUavFloat";
        const uint64_t args[] = { 0, a1, a2, a3, a4, a5, a6, a7 };
        void* dst = Dst ? reinterpret_cast<void*>(args[Dst]) : nullptr;
        void* src = Src ? reinterpret_cast<void*>(args[Src]) : nullptr;
        if (Slot == 16) // copy locations: pResource is the first member
        {
            dst = dst ? *reinterpret_cast<void**>(dst) : nullptr;
            src = src ? *reinterpret_cast<void**>(src) : nullptr;
        }
        if (Slot == 26 && a1 && a2) // first barrier's resource
        {
            const auto* b = reinterpret_cast<const D3D12_RESOURCE_BARRIER*>(a2);
            dst = b->Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ? b->Transition.pResource
                : b->Type == D3D12_RESOURCE_BARRIER_TYPE_UAV       ? b->UAV.pResource
                                                                   : b->Aliasing.pResourceAfter;
        }
        g_commands.push_back({ names[Slot], dst, src, g_index, g_role });
    }
    g_listOriginal[Slot](self, a1, a2, a3, a4, a5, a6, a7);
}
void HookList(ID3D12GraphicsCommandList* list)
{
    void** vtable = *reinterpret_cast<void***>(list);
    struct Entry
    {
        int slot;
        PVOID hook;
    } entries[] = {
        { 14, reinterpret_cast<PVOID>(&ListHook<14, 0, 0>) }, { 15, reinterpret_cast<PVOID>(&ListHook<15, 1, 3>) },
        { 16, reinterpret_cast<PVOID>(&ListHook<16, 1, 5>) }, { 17, reinterpret_cast<PVOID>(&ListHook<17, 1, 2>) },
        { 26, reinterpret_cast<PVOID>(&ListHook<26, 0, 0>) }, { 49, reinterpret_cast<PVOID>(&ListHook<49, 3, 0>) },
        { 50, reinterpret_cast<PVOID>(&ListHook<50, 3, 0>) },
    };
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    for (const auto& e : entries)
    {
        g_listOriginal[e.slot] = reinterpret_cast<PFN_Slot>(vtable[e.slot]);
        DetourAttach(reinterpret_cast<PVOID*>(&g_listOriginal[e.slot]), e.hook);
    }
    DetourTransactionCommit();
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
        if (_wgetenv(L"SPLICE_LOGCMD"))
            HookList(list.Get());
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
        Allocations().tag = 1;
        void* ref = model.NewFeature(device.Get(), list.Get(), w, h, preset);
        Allocations().tag = 2;
        void* owner = model.NewFeature(device.Get(), list.Get(), w, h, preset);
        Allocations().tag = 3;
        void* helper = model.NewFeature(device.Get(), list.Get(), w, h, preset);
        const bool discover = _wgetenv(L"SPLICE_DISCOVER") != nullptr;
        void* probe = nullptr;
        if (discover)
        {
            Allocations().tag = 4;
            probe = model.NewFeature(device.Get(), list.Get(), w, h, preset);
        }
        Allocations().tag = 0;
        submit();
        {
            const auto& items = Allocations().items;
            std::printf("Model allocations through the NGX callback: %zu\n", items.size());
            for (const auto& a : items)
                std::printf("  feature %d: %s %llux%u fmt %u, %.2f MiB, va 0x%llx, flags 0x%x, state 0x%x\n", a.tag,
                            a.desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? "buffer " : "texture",
                            (unsigned long long) a.desc.Width, a.desc.Height, (unsigned) a.desc.Format,
                            a.bytes / 1048576.0, (unsigned long long) a.va, (unsigned) a.desc.Flags, a.state);
        }

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
        if (g_resume == 0)
            g_resume = kernels - 2;
        if (g_resume < 0)
            g_resume = kernels; // tail mode: the helper runs everything after the cut, including the output kernels
        const bool tailMode = g_resume == kernels;
        g_twoCalls = !_wgetenv(L"SPLICE_DEFER");
        if (traceHelper.size() != traceOwner.size() ||
            (!baseline && (g_cut < 1 || g_resume <= g_cut + 1 || g_resume > kernels)))
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
            for (const auto& alloc : Allocations().items) // exact bounds when the allocation callback is active
                if (alloc.tag == 2 && alloc.va && a >= alloc.va && a < alloc.va + alloc.desc.Width)
                    t.bytes = std::min<uint64_t>(t.bytes, (alloc.va + alloc.desc.Width - a) & ~15ull);
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
        // SPLICE_WHOLE=small|all: transfer whole model buffers (exact bounds from the allocation callback) instead of
        // the per-tensor sets: "small" = the smaller writable buffer (the activation workspace), "all" = every writable
        // buffer. Forward at the cut; backward after the helper's segment.
        if (const wchar_t* whole = _wgetenv(L"SPLICE_WHOLE"); whole && !baseline)
        {
            std::vector<const Allocation*> ownerBuffers, helperBuffers;
            for (const auto& a : Allocations().items)
                if (a.desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
                    (a.desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
                    (a.tag == 2 ? ownerBuffers : a.tag == 3 ? helperBuffers : ownerBuffers).push_back(&a);
            ownerBuffers.erase(std::remove_if(ownerBuffers.begin(), ownerBuffers.end(),
                                              [](const Allocation* a) { return a->tag != 2; }),
                               ownerBuffers.end());
            if (ownerBuffers.empty() || ownerBuffers.size() != helperBuffers.size())
                throw std::runtime_error("SPLICE_WHOLE needs NR_ALLOC_CALLBACK and matching allocations");
            uint64_t smallest = ~0ull;
            for (const auto* a : ownerBuffers)
                smallest = std::min(smallest, a->desc.Width);
            g_forward.clear();
            g_backward.clear();
            forwardBytes = backwardBytes = 0;
            for (size_t i = 0; i < ownerBuffers.size(); ++i)
            {
                if (std::wstring(whole) == L"small" && ownerBuffers[i]->desc.Width != smallest)
                    continue;
                Transfer t;
                t.ownerVa = ownerBuffers[i]->va;
                t.helperVa = helperBuffers[i]->va;
                t.bytes = ownerBuffers[i]->desc.Width & ~15ull;
                t.offset = forwardBytes;
                forwardBytes += (t.bytes + 255) & ~255ull;
                g_forward.push_back(t);
                if (!_wgetenv(L"SPLICE_NO_BACK"))
                {
                    t.offset = backwardBytes;
                    backwardBytes += (t.bytes + 255) & ~255ull;
                    g_backward.push_back(t);
                }
            }
        }
        std::vector<uint8_t> dHead, dMid;
        const Allocation* dOwner = nullptr;
        const Allocation* dHelper = nullptr;
        uint64_t dBytes = 0;
        if (discover && !baseline)
        {
            // Workspace of each instance: its smallest writable buffer (the activation arena).
            auto workspace = [](int tag) -> const Allocation*
            {
                const Allocation* best = nullptr;
                for (const auto& a : Allocations().items)
                    if (a.tag == tag && a.desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
                        (a.desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) &&
                        (!best || a.desc.Width < best->desc.Width))
                        best = &a;
                return best;
            };
            const Allocation* wsOwner = workspace(2);
            const Allocation* wsHelper = workspace(3);
            const Allocation* wsProbe = workspace(4);
            if (!wsOwner || !wsHelper || !wsProbe)
                throw std::runtime_error("SPLICE_DISCOVER needs NR_ALLOC_CALLBACK");
            const uint64_t bytes = wsOwner->desc.Width & ~255ull;
            g_probe.wsVa = wsProbe->va;
            g_probe.wsBytes = bytes;
            auto snapStart = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
            g_probe.snapStartVa = snapStart->GetGPUVirtualAddress();
            auto snapHead = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                       true);
            auto snapMid = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                      true);
            g_probe.snapHeadVa = snapHead->GetGPUVirtualAddress();
            g_probe.snapMidVa = snapMid->GetGPUVirtualAddress();
            auto readBuffer = [&](ID3D12Resource* src)
            {
                auto rb = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST, false);
                begin();
                Barrier(list.Get(), src, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                list->CopyBufferRegion(rb.Get(), 0, src, 0, bytes);
                Barrier(list.Get(), src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                submit();
                std::vector<uint8_t> out(static_cast<size_t>(bytes));
                void* m = nullptr;
                D3D12_RANGE range { 0, static_cast<SIZE_T>(bytes) };
                Must(rb->Map(0, &range, &m), "map");
                std::memcpy(out.data(), m, out.size());
                D3D12_RANGE none { 0, 0 };
                rb->Unmap(0, &none);
                return out;
            };
            constexpr uint64_t block = 256;
            const size_t blocks = static_cast<size_t>(bytes / block);
            std::vector<uint8_t> headWrites(blocks, 0), midWrites(blocks, 0);
            // Several frames of changing input: a block counts as written by a segment when its content changes across
            // that segment in any of them.
            for (int pf = 1; pf <= 3; ++pf)
            {
                begin();
                Barrier(list.Get(), color.texture.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_COPY_DEST);
                Upload(list.Get(), color, 0, 900 + pf);
                Barrier(list.Get(), color.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                g_role = Role::Probe;
                g_index = 0;
                if (model.Run(list.Get(), probe, color, depth, motion, outHelper, w, h, pf == 1) != 1)
                    throw std::runtime_error("probe evaluate failed");
                g_role = Role::Full;
                submit();
                const auto a = readBuffer(snapStart.Get());
                const auto b = readBuffer(snapHead.Get());
                const auto c = readBuffer(snapMid.Get());
                for (size_t k = 0; k < blocks; ++k)
                    for (size_t o = 0; o < block; ++o)
                    {
                        const size_t x = k * block + o;
                        headWrites[k] |= a[x] != b[x];
                        midWrites[k] |= b[x] != c[x];
                    }
            }
            // Workspace offsets referenced by the helper's segment and by the owner's end kernels.
            std::vector<uint8_t> midRefs(blocks, 0), endRefs(blocks, 0);
            for (int i = g_cut + 1; i < kernels; ++i)
                for (uint64_t v : traceOwner[i])
                    if (v >= wsOwner->va && v < wsOwner->va + bytes)
                        (i < g_resume ? midRefs : endRefs)[static_cast<size_t>((v - wsOwner->va) / block)] = 1;
            // A transferred region is a maximal run of written blocks that contains a referenced block. With
            // SPLICE_SETS=written every written run is transferred.
            const bool allWritten = _wgetenv(L"SPLICE_SETS") && std::wstring(_wgetenv(L"SPLICE_SETS")) == L"written";
            auto runs = [&](const std::vector<uint8_t>& written, const std::vector<uint8_t>& refs,
                            std::vector<Transfer>& out, uint64_t& total)
            {
                out.clear();
                total = 0;
                size_t runCount = 0;
                for (size_t k = 0; k < blocks;)
                {
                    if (!written[k])
                    {
                        ++k;
                        continue;
                    }
                    size_t e = k;
                    bool referenced = false;
                    while (e < blocks && written[e])
                        referenced |= refs[e++] != 0;
                    ++runCount;
                    if (referenced || allWritten)
                    {
                        Transfer t;
                        t.ownerVa = wsOwner->va + k * block;
                        t.helperVa = wsHelper->va + k * block;
                        t.bytes = (e - k) * block;
                        t.offset = k * block; // staging mirrors the workspace layout
                        total += t.bytes;
                        out.push_back(t);
                    }
                    k = e;
                }
                return runCount;
            };
            const size_t headRuns = runs(headWrites, midRefs, g_forward, forwardBytes);
            const size_t midRuns = runs(midWrites, endRefs, g_backward, backwardBytes);
            size_t headTotal = 0, midTotal = 0;
            for (size_t k = 0; k < blocks; ++k)
            {
                headTotal += headWrites[k];
                midTotal += midWrites[k];
            }
            dHead = headWrites;
            dMid = midWrites;
            dOwner = wsOwner;
            dHelper = wsHelper;
            dBytes = bytes;
            std::printf("Selected: forward %.2f MiB, backward %.2f MiB\n", forwardBytes / 1048576.0,
                        backwardBytes / 1048576.0);
            forwardBytes = backwardBytes = bytes;
            std::printf("Discovered writes: head %.2f MiB in %zu runs, segment %.2f MiB in %zu runs\n",
                        headTotal * block / 1048576.0, headRuns, midTotal * block / 1048576.0, midRuns);
        }
        auto forwardBuffer = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, std::max<uint64_t>(forwardBytes, 256),
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        auto backwardBuffer = MakeBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT,
                                         std::max<uint64_t>(backwardBytes, 256), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                         true);
        g_forwardVa = forwardBuffer->GetGPUVirtualAddress();
        g_backwardVa = backwardBuffer->GetGPUVirtualAddress();
        if (discover && !baseline && !tailMode && _wgetenv(L"SPLICE_MINIMIZE"))
        {
            // Empirical minimization: drop a chunk from a set when the owner's output over a reset frame and a normal
            // frame stays bit-identical to the output with every written block transferred. The block range kernel 0
            // (the counter clear) references is pinned in both directions: dropping the model's synchronization
            // counters would leave the receiving side's wait kernels waiting forever.
            constexpr uint64_t block = 256;
            const size_t blocks = static_cast<size_t>(dBytes / block);
            std::vector<uint8_t> pinned(blocks, 0), fwd = dHead, bwd = dMid;
            for (uint64_t v : traceOwner[0])
                if (v >= dOwner->va && v < dOwner->va + dBytes)
                    for (size_t k = static_cast<size_t>((v - dOwner->va) / block);
                         k < std::min(blocks, static_cast<size_t>((v - dOwner->va) / block) + 256); ++k)
                        pinned[k] = 1;
            auto build = [&](const std::vector<uint8_t>& m, std::vector<Transfer>& out)
            {
                out.clear();
                for (size_t k = 0; k < blocks;)
                {
                    if (!m[k])
                    {
                        ++k;
                        continue;
                    }
                    size_t e = k;
                    while (e < blocks && m[e])
                        ++e;
                    Transfer t;
                    t.ownerVa = dOwner->va + k * block;
                    t.helperVa = dHelper->va + k * block;
                    t.bytes = (e - k) * block;
                    t.offset = k * block;
                    out.push_back(t);
                    k = e;
                }
            };
            auto run = [&]()
            {
                std::vector<std::pair<double, double>> outs;
                // Same starting memory for every candidate: one full evaluate of both instances first.
                begin();
                for (void* feature : { owner, helper })
                {
                    g_role = Role::Full;
                    g_index = 0;
                    if (model.Run(list.Get(), feature, color, depth, motion, outHelper, w, h, true) != 1)
                        throw std::runtime_error("normalize evaluate failed");
                }
                submit();
                for (int f = 1; f <= 2; ++f)
                {
                    begin();
                    Barrier(list.Get(), color.texture.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_COPY_DEST);
                    Upload(list.Get(), color, 0, 700 + f);
                    Barrier(list.Get(), color.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    for (auto [role, feature, reset] : { std::tuple { Role::Full, ref, f == 1 },
                                                         { Role::Owner, owner, f == 1 },
                                                         { Role::Helper, helper, f == 1 },
                                                         { Role::OwnerEnd, owner, false } })
                    {
                        g_role = role;
                        g_index = 0;
                        g_injected = false;
                        if (model.Run(list.Get(), feature, color, depth, motion,
                                      role == Role::Full ? outRef : role == Role::Helper ? outHelper : outOwner, w, h,
                                      reset) != 1)
                            throw std::runtime_error("minimize evaluate failed");
                    }
                    g_role = Role::Full;
                    submit();
                    // error against the reference: max and mean absolute difference (half-float RGB)
                    const auto a = Readback(device.Get(), queue.Get(), outRef);
                    const auto b = Readback(device.Get(), queue.Get(), outOwner);
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
                    outs.push_back({ mx, sum / n });
                }
                return outs;
            };
            auto mib = [&](const std::vector<uint8_t>& m)
            {
                size_t c = 0;
                for (uint8_t x : m)
                    c += x;
                return c * block / 1048576.0;
            };
            build(fwd, g_forward);
            build(bwd, g_backward);
            const auto base = run();
            // Missing data shows up as large errors (0.37 max, 0.13 mean); the two-call residue is about 0.004.
            auto acceptable = [&](const std::vector<std::pair<double, double>>& r)
            {
                for (size_t f = 0; f < r.size(); ++f)
                    if (r[f].first > base[f].first * 1.5 + 1e-4 || r[f].second > base[f].second * 1.5 + 1e-6)
                        return false;
                return true;
            };
            std::printf("Baseline error: frame 1 max %.5f mean %.7f, frame 2 max %.5f mean %.7f%c", base[0].first,
                        base[0].second, base[1].first, base[1].second, 10);
            auto shrink = [&](std::vector<uint8_t>& m, const char* label, size_t chunk)
            {
                size_t tried = 0, dropped = 0;
                for (size_t c0 = 0; c0 < blocks; c0 += chunk)
                {
                    const size_t c1 = std::min(blocks, c0 + chunk);
                    bool any = false;
                    for (size_t k = c0; k < c1; ++k)
                        any |= m[k] && !pinned[k];
                    if (!any)
                        continue;
                    const auto saved = m;
                    for (size_t k = c0; k < c1; ++k)
                        if (!pinned[k])
                            m[k] = 0;
                    build(fwd, g_forward);
                    build(bwd, g_backward);
                    ++tried;
                    if (acceptable(run()))
                        ++dropped;
                    else
                        m = saved;
                }
                build(fwd, g_forward);
                build(bwd, g_backward);
                std::printf("  %s, %zu KiB chunks: dropped %zu of %zu, now %.2f MiB\n", label,
                            static_cast<size_t>(chunk * block / 1024), dropped, tried, mib(m));
                std::fflush(stdout);
            };
            std::printf("Minimizing from forward %.2f MiB, backward %.2f MiB (pinned %.2f MiB)\n", mib(fwd), mib(bwd),
                        mib(pinned));
            shrink(fwd, "forward", 4096);
            shrink(bwd, "backward", 4096);
            shrink(fwd, "forward", 256);
            shrink(bwd, "backward", 256);
            std::printf("Minimal sets: forward %.2f MiB in %zu ranges, backward %.2f MiB in %zu ranges\n", mib(fwd),
                        g_forward.size(), mib(bwd), g_backward.size());
            if (FILE* out = _wfopen(L"split-sets.txt", L"w"))
            {
                for (const auto& t : g_forward)
                    std::fprintf(out, "F %llu %llu\n", (unsigned long long) t.offset, (unsigned long long) t.bytes);
                for (const auto& t : g_backward)
                    std::fprintf(out, "B %llu %llu\n", (unsigned long long) t.offset, (unsigned long long) t.bytes);
                std::fclose(out);
            }
        }
        std::printf("Adapter %ls, %ux%u preset %d: %d kernels; owner [0..%d] + [%d..%d], helper [%d..%d]\n",
                    name.c_str(), w, h, preset, kernels, g_cut, g_resume, kernels - 1, g_cut + 1, g_resume - 1);
        std::printf("Forward %zu tensors %.2f MiB, backward %zu tensors %.2f MiB per evaluate\n", g_forward.size(),
                    forwardBytes / 1048576.0, g_backward.size(), backwardBytes / 1048576.0);

        int failures = 0;
        g_logCommands = _wgetenv(L"SPLICE_LOGCMD") != nullptr;
        g_commands.clear();
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
            if (!baseline && !tailMode && g_twoCalls)
            {
                // A real split puts the queue-level wait for the helper here, between the owner's two evaluates.
                g_role = Role::OwnerEnd;
                g_index = 0;
                g_injected = false;
                if (model.Run(list.Get(), owner, color, depth, motion, outOwner, w, h, false) != 1)
                    throw std::runtime_error("owner end evaluate failed");
            }
            g_role = Role::Full;
            if (!baseline && (tailMode || !g_twoCalls) && ReplayDeferred(list.Get()) != 0)
                throw std::runtime_error("replay failed");
            submit();
            if (g_logCommands)
            {
                auto label = [&](void* r) -> std::string
                {
                    if (!r)
                        return "-";
                    for (const auto& al : Allocations().items)
                        if (al.resource.Get() == r)
                        {
                            const char* kind = al.desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ? "history-tex"
                                             : al.desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                                 ? (al.desc.Width < 80000000 ? "workspace" : "weights")
                                                 : "readonly-buf";
                            return std::string(kind) + "#" + std::to_string(al.tag);
                        }
                    for (auto [sf, n] : { std::pair { &color, "color" }, { &depth, "depth" }, { &motion, "motion" },
                                          { &outRef, "out-ref" }, { &outOwner, "out-owner" }, { &outHelper, "out-helper" } })
                        if (sf->texture.Get() == r)
                            return n;
                    return "other";
                };
                const char* roles[] = { "full", "trace", "owner", "helper", "owner-end", "probe" };
                std::printf("  model commands, frame %d:\n", f);
                for (const auto& e : g_commands)
                    if (std::string(e.what) != "ResourceBarrier" || label(e.dst).find("history") != std::string::npos)
                        std::printf("    %-9s before kernel %2d: %-17s dst %-14s src %s\n", roles[int(e.role)], e.kernel,
                                    e.what, label(e.dst).c_str(), label(e.src).c_str());
                g_commands.clear();
                g_logCommands = f < 2;
            }
            const auto a = Readback(device.Get(), queue.Get(), outRef);
            const auto b = Readback(device.Get(), queue.Get(), baseline || tailMode ? outHelper : outOwner);
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
