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
#include "../DlssNr_SplitCalibration.h"
#include "../DlssNrVitCopyKernel.h"
#pragma warning(push, 0)
#include <detours/detours.h>
#pragma warning(pop)
#include <functional>
#include <fstream>
#include <map>
#include <random>

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
    // Calibration (wire.split.cut < 0): every kernel's name; the chain of one evaluate; a workspace snapshot after each
    // kernel of [probeFirst, probeLast] (read-only copies into snaps).
    std::map<NvHandle, std::string> names;
    bool recording = false;
    std::vector<DlssNr::NetworkSplit::Calibration::Kernel> recorded;
    int probeIndex = 0, probeFirst = -1, probeLast = -1;
    bool probeStarted = false; // kernels are counted from the network's synchronization clear
    uint64_t workspaceVa = 0, workspaceBytes = 0;
    std::vector<uint64_t> snaps;
    // Kernel timing: a timestamp before kernel 0 and after every kernel (each kernel finishes before the next starts).
    ID3D12QueryHeap* timestamps = nullptr;
    // Model allocations made while creating our single feature: the smallest writable buffer is its workspace.
    ID3D12Device* device = nullptr;
    std::vector<std::pair<ComPtr<ID3D12Resource>, D3D12_RESOURCE_DESC>> allocations;
};
inline State& S()
{
    static State s;
    return s;
}

inline int CopyLaunch(ID3D12GraphicsCommandList* list, uint64_t dst, uint64_t src, uint64_t bytes)
{
    auto& s = S();
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
}
inline void UavBarrier(ID3D12GraphicsCommandList* list)
{
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    list->ResourceBarrier(1, &b);
}
// Calibration launches: each kernel unchanged, recorded, and followed by a snapshot when inside the probe window.
inline int ProbeLaunch(ID3D12GraphicsCommandList* list, const CuLaunch* k, uint32_t count)
{
    auto& s = S();
    for (uint32_t i = 0; i < count; ++i)
    {
        const auto named = s.names.find(k[i].hFunction);
        if (!s.probeStarted)
        {
            if (named == s.names.end() || !DlssNr::NetworkSplit::Calibration::IsClear(named->second))
            {
                if (const int status = s.launch(list, &k[i], 1))
                    return status;
                continue;
            }
            s.probeStarted = true;
        }
        const int index = s.probeIndex++;
        if (s.recording)
        {
            const auto name = s.names.find(k[i].hFunction);
            const auto* p = static_cast<const uint8_t*>(k[i].pParams);
            s.recorded.push_back({ name == s.names.end() ? std::string() : name->second,
                                   std::vector<uint8_t>(p, p + k[i].paramSize) });
        }
        if (s.timestamps && index == 0)
            list->EndQuery(s.timestamps, D3D12_QUERY_TYPE_TIMESTAMP, 0);
        if (const int status = s.launch(list, &k[i], 1))
            return status;
        if (s.timestamps)
        {
            UavBarrier(list);
            list->EndQuery(s.timestamps, D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(index + 1));
        }
        if (index >= s.probeFirst && index <= s.probeLast && size_t(index - s.probeFirst) < s.snaps.size())
        {
            UavBarrier(list);
            if (const int status = CopyLaunch(list, s.snaps[index - s.probeFirst], s.workspaceVa, s.workspaceBytes))
                return status;
            UavBarrier(list);
        }
    }
    return 0;
}

inline int __cdecl Launch(ID3D12GraphicsCommandList* list, const CuLaunch* k, uint32_t count)
{
    auto& s = S();
    if (s.recording || s.probeFirst >= 0 || s.timestamps)
        return ProbeLaunch(list, k, count);
    for (uint32_t i = 0; i < count; ++i)
    {
        const auto named = s.names.find(k[i].hFunction);
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
            },
            named != s.names.end() && DlssNr::NetworkSplit::Calibration::IsClear(named->second));
        if (status)
            return status;
    }
    return 0;
}
inline int __cdecl CreateFunctionNamed(ID3D12Device* device, NvHandle module, const char* name, NvHandle* out)
{
    auto& s = S();
    const int status = s.createFunction(device, module, name, out);
    if (status == 0 && out && *out && name)
        s.names[*out] = name;
    return status;
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
        return reinterpret_cast<void*>(&CreateFunctionNamed);
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

// Fills a texture with pseudo-random values of its format (half floats in [0.06, 1), floats in [0, 1)), so each
// calibration evaluate recomputes every tensor to different bytes than the ones it overwrites.
inline void FillRandom(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* texture, uint32_t seed,
                       std::vector<ComPtr<ID3D12Resource>>& keep)
{
    const auto desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
    UINT rows = 0;
    UINT64 rowBytes = 0, total = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &rowBytes, &total);
    auto upload = Buffer(device, total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
    uint8_t* mapped = nullptr;
    Check(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "Map calibration upload");
    std::mt19937 rng(seed);
    for (UINT y = 0; y < rows; ++y)
    {
        uint8_t* row = mapped + fp.Offset + uint64_t(y) * fp.Footprint.RowPitch;
        if (desc.Format == DXGI_FORMAT_R32_FLOAT)
            for (UINT64 x = 0; x + 4 <= rowBytes; x += 4)
            {
                const float v = float(rng() % 1000000) / 1000000.0f;
                std::memcpy(row + x, &v, 4);
            }
        else
            for (UINT64 x = 0; x + 2 <= rowBytes; x += 2)
            {
                const uint16_t v = static_cast<uint16_t>(0x2C00 + rng() % (0x3BFF - 0x2C00));
                std::memcpy(row + x, &v, 2);
            }
    }
    upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
    dst.pResource = texture;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = fp;
    Transition(list, texture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Transition(list, texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    keep.push_back(upload);
}

// Calibration (DlssNr_SplitCalibration.h), all read-only on this GPU: one recorded evaluate gives the chain and the
// workspace layout; then whole evaluates, each on fresh random inputs, snapshot the workspace after every kernel of a
// window (as many snapshots as fit in about 1.5 GiB; the window's first kernel is the baseline of the next), and the
// changed 256-byte blocks between consecutive snapshots are each kernel's writes. The plan goes to planPath with the
// layout fingerprint, for the owner to check against its own layout.
template<class Retire, class Evaluate>
inline void Calibrate(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12CommandAllocator* alloc,
                      ID3D12GraphicsCommandList* list, Retire& retire, const Evaluate& evaluateOnce,
                      ID3D12Resource* color, ID3D12Resource* depth, ID3D12Resource* motion,
                      ID3D12Resource* workspace, uint64_t workspaceBytes, const std::wstring& planPath)
{
    namespace Cal = DlssNr::NetworkSplit::Calibration;
    auto& s = S();
    s.workspaceVa = workspace->GetGPUVirtualAddress();
    s.workspaceBytes = workspaceBytes;
    uint32_t seed = 1;
    const auto run = [&](bool reset, const std::function<void()>& beforeClose = {})
    {
        std::vector<ComPtr<ID3D12Resource>> uploads;
        Check(alloc->Reset(), "Reset calibration allocator");
        Check(list->Reset(alloc, nullptr), "Reset calibration list");
        for (auto* t : { color, depth, motion })
            FillRandom(device, list, t, seed++, uploads);
        s.probeIndex = 0;
        s.probeStarted = false;
        if (evaluateOnce(reset) != 1)
            throw std::runtime_error("Calibration evaluate failed");
        if (beforeClose)
            beforeClose();
        Check(list->Close(), "Close calibration list");
        ID3D12CommandList* lists[] = { list };
        queue->ExecuteCommandLists(1, lists);
        retire();
    };
    run(true); // warm-up
    s.recorded.clear();
    s.recording = true;
    run(false);
    s.recording = false;
    const auto kernels = s.recorded;
    Cal::Layout layout;
    std::string why;
    int cut = 0, resume = 0;
    if (!Cal::Decode(kernels, s.workspaceVa, workspaceBytes, layout, why) || !Cal::ChooseCut(layout, cut, resume, why))
        throw std::runtime_error("Calibration: " + why);

    // Each kernel's time on this GPU: the median of three evaluates, kernels serialized by UAV barriers (a slight
    // overestimate of the chain, which overlaps neighbours a little).
    const size_t count = kernels.size();
    std::vector<double> micros(count, 0.0);
    {
        ComPtr<ID3D12QueryHeap> heap;
        D3D12_QUERY_HEAP_DESC qh {};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = static_cast<UINT>(count + 1);
        Check(device->CreateQueryHeap(&qh, IID_PPV_ARGS(&heap)), "Create calibration timestamps");
        auto stamps = Buffer(device, (count + 1) * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
        UINT64 frequency = 0;
        Check(queue->GetTimestampFrequency(&frequency), "Timestamp frequency");
        std::vector<std::vector<double>> samples(count);
        s.timestamps = heap.Get();
        for (int r = 0; r < 3; ++r)
        {
            run(false, [&] { list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, qh.Count, stamps.Get(), 0); });
            if (s.probeIndex != static_cast<int>(count))
                throw std::runtime_error("Calibration: the kernel chain changed between evaluates");
            uint64_t* t = nullptr;
            const D3D12_RANGE range { 0, static_cast<SIZE_T>((count + 1) * 8) };
            Check(stamps->Map(0, &range, reinterpret_cast<void**>(&t)), "Map calibration timestamps");
            for (size_t k = 0; k < count; ++k)
                samples[k].push_back(t[k + 1] > t[k] ? double(t[k + 1] - t[k]) * 1e6 / double(frequency) : 0.0);
            const D3D12_RANGE none { 0, 0 };
            stamps->Unmap(0, &none);
        }
        s.timestamps = nullptr;
        double whole = 0.0;
        for (size_t k = 0; k < count; ++k)
        {
            std::sort(samples[k].begin(), samples[k].end());
            micros[k] = samples[k][1];
            whole += micros[k];
        }
        std::cerr << "Split calibration: whole network " << whole / 1000.0 << " ms on this GPU (kernels serialized)\n"
                  << std::flush;
    }

    const uint64_t budget = 1536ull << 20;
    const size_t window = (std::max<size_t>)(2, (std::min<size_t>)(kernels.size(), size_t(budget / workspaceBytes)));
    std::vector<ComPtr<ID3D12Resource>> snaps;
    for (size_t i = 0; i < window; ++i)
    {
        snaps.push_back(Buffer(device, workspaceBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                               true));
        s.snaps.push_back(snaps.back()->GetGPUVirtualAddress());
    }
    auto readback = Buffer(device, workspaceBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
    std::vector<uint8_t> previous(workspaceBytes), current(workspaceBytes);
    const auto read = [&](ID3D12Resource* snap, std::vector<uint8_t>& host)
    {
        Check(alloc->Reset(), "Reset calibration allocator");
        Check(list->Reset(alloc, nullptr), "Reset calibration list");
        Transition(list, snap, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(readback.Get(), 0, snap, 0, workspaceBytes);
        Transition(list, snap, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Check(list->Close(), "Close calibration readback");
        ID3D12CommandList* lists[] = { list };
        queue->ExecuteCommandLists(1, lists);
        retire();
        void* mapped = nullptr;
        const D3D12_RANGE range { 0, static_cast<SIZE_T>(workspaceBytes) };
        Check(readback->Map(0, &range, &mapped), "Map calibration readback");
        std::memcpy(host.data(), mapped, workspaceBytes);
        const D3D12_RANGE none { 0, 0 };
        readback->Unmap(0, &none);
    };
    Cal::Writes writes(kernels.size());
    const int last = static_cast<int>(kernels.size()) - 1;
    for (int first = 0; first < last;)
    {
        s.probeFirst = first;
        s.probeLast = (std::min)(last, first + static_cast<int>(window) - 1);
        run(false);
        if (s.probeIndex != static_cast<int>(kernels.size()))
            throw std::runtime_error("Calibration: the kernel chain changed between evaluates");
        read(snaps[0].Get(), previous);
        for (int k = first + 1; k <= s.probeLast; ++k)
        {
            read(snaps[k - first].Get(), current);
            for (uint64_t b = 0; b < workspaceBytes / Cal::Block;)
            {
                const auto changed = [&](uint64_t x)
                { return std::memcmp(&previous[x * Cal::Block], &current[x * Cal::Block], Cal::Block) != 0; };
                if (!changed(b))
                {
                    ++b;
                    continue;
                }
                uint64_t e = b;
                while (e < workspaceBytes / Cal::Block && changed(e))
                    ++e;
                writes[k].push_back({ b * Cal::Block, (e - b) * Cal::Block });
                b = e;
            }
            std::swap(previous, current);
        }
        std::cerr << "Split calibration: kernels " << first + 1 << ".." << s.probeLast << " measured\n" << std::flush;
        first = s.probeLast;
    }
    s.probeFirst = s.probeLast = -1;
    s.snaps.clear();

    // A plan for every split the chain allows, each with the helper's measured time for its kernels (kernel 0 runs on
    // both sides). The owner picks one by its frame time and the transfer volume.
    const uint64_t fingerprint = Cal::Fingerprint(layout);
    // The layout as text beside the plan, for comparing with the owner's when the fingerprints differ.
    std::ofstream(planPath.substr(0, planPath.find_last_of(L"\\/") + 1) + L"split-layout.txt", std::ios::trunc)
        << Cal::Describe(layout);
    std::ofstream(planPath.substr(0, planPath.find_last_of(L"\\/") + 1) + L"split-params.txt", std::ios::trunc)
        << Cal::DescribeParams(kernels);
    std::ofstream out(planPath, std::ios::trunc);
    out << "# calibrated by the helper: " << kernels.size() << " kernels, workspace " << workspaceBytes << " bytes\n";
    size_t written = 0;
    for (const auto& [c, r] : Cal::Candidates(layout))
    {
        Plan plan;
        if (!Cal::Compute(layout, writes, c, r, plan, why))
            continue;
        double t = micros[0];
        for (int k = c + 1; k < r; ++k)
            t += micros[k];
        plan.helperMicros = static_cast<uint64_t>(t);
        out << Cal::Write(plan, fingerprint, "");
        ++written;
        std::cerr << "  (" << c << ", " << r << "): helper " << t / 1000.0 << " ms, forward " << plan.forwardBytes
                  << ", backward " << plan.backwardBytes << " bytes\n";
    }
    if (!out || !written)
        throw std::runtime_error("Calibration: the plans could not be written");
    std::cerr << "Split calibration: " << written << " candidate splits, layout " << std::hex << fingerprint
              << std::dec << "\n" << std::flush;
}

// Runs the helper's segment for every frame the owner sends. Returns the process exit code.
inline int Run(ID3D12Device* device, DlssNr::MultiGpu::CrossAdapterPeer& peer, const InitialFrameWire& wire,
               const std::function<bool(void*, DWORD)>& readExact, HANDLE parent, HANDLE initialized,
               const wchar_t* corePath, const wchar_t* forwarderPath, const wchar_t* modelPath,
               const wchar_t* dataPath)
{
    const bool calibrate = wire.split.cut < 0;
    Plan plan;
    if (!calibrate && (!DlssNr::NetworkSplit::FromWire(wire.split, plan) ||
                       plan.forwardBytes + plan.backwardBytes > wire.bytes))
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

    const auto evaluateOnce = [&](bool reset)
    {
        return evaluate(list.Get(), feature, params, color.Get(), depth.Get(), motion.Get(), output.Get(), w, h, w, h,
                        w, h, 0, 0, 0, 0, 0, reset ? 1 : 0, tune.intensity, tune.style, tune.structure, tune.tone,
                        tune.skin, tune.autoMask, 1.0f, 1.0f);
    };
    // The workspace is the buffer kernel 0 (cc_cb_clear) clears from, found from one recorded evaluate. (The smallest
    // writable buffer is not it at every size.)
    Check(alloc->Reset(), "Reset split allocator");
    Check(list->Reset(alloc.Get(), nullptr), "Reset split list");
    s.recorded.clear();
    s.recording = true;
    s.probeIndex = 0;
    s.probeStarted = false;
    const int found = evaluateOnce(true);
    s.recording = false;
    if (found != 1)
        throw std::runtime_error("Split worker workspace evaluate failed");
    Check(list->Close(), "Close split workspace evaluate");
    queue->ExecuteCommandLists(1, lists);
    retire();
    const uint64_t clear = DlssNr::NetworkSplit::Calibration::ClearPointer(s.recorded);
    const D3D12_RESOURCE_DESC* workspace = nullptr;
    ID3D12Resource* workspaceResource = nullptr;
    for (const auto& [resource, desc] : s.allocations)
    {
        const uint64_t va = desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? resource->GetGPUVirtualAddress() : 0;
        if (va && clear >= va && clear < va + desc.Width)
        {
            workspace = &desc;
            workspaceResource = resource.Get();
        }
    }
    if (!workspace || clear != workspaceResource->GetGPUVirtualAddress())
    {
        std::cerr << "Split worker: kernel 0 clears from 0x" << std::hex << clear << std::dec << "; buffers:";
        for (const auto& [resource, desc] : s.allocations)
            if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
                std::cerr << " 0x" << std::hex << resource->GetGPUVirtualAddress() << std::dec << "+" << desc.Width;
        std::cerr << "\n" << std::flush;
        throw std::runtime_error("The model workspace was not found");
    }
    if (calibrate)
    {
        NvHandle copyModule = nullptr;
        if (!s.createModule || !s.createFunction ||
            s.createModule(device, DlssNrVitCopyKernel::fatbin,
                           static_cast<uint32_t>(sizeof(DlssNrVitCopyKernel::fatbin)), &copyModule) != 0 ||
            s.createFunction(device, copyModule, "nr_vit_copy16", &s.copy) != 0)
            throw std::runtime_error("Copy kernel unavailable");
        const std::wstring planPath = std::wstring(dataPath) + L"\\split-plan.txt";
        Calibrate(device, queue.Get(), alloc.Get(), list.Get(), retire, evaluateOnce, color.Get(), depth.Get(),
                  motion.Get(), workspaceResource, workspace->Width, planPath);
        if (!SetEvent(initialized))
            throw std::runtime_error("Could not signal split calibration");
        // Stay until the owner has read the plan and sends shutdown (or exits).
        for (;;)
        {
            FrameCommand command {};
            if (!readExact(&command, static_cast<DWORD>(sizeof(command))) || command.kind == CommandKind::Shutdown)
                break;
        }
        std::cerr << "Split calibration finished.\n" << std::flush;
        return 0;
    }
    if (!plan.Fits(workspace->Width))
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
