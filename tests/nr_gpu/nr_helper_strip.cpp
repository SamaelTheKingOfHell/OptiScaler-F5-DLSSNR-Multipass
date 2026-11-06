// Real two-GPU helper-strip fixture: the worker evaluates only the bottom rows of a synthetic picture, the return is
// ordered on the primary queue (QueueWaitReturn) with no CPU wait, and the watchdog releases the queue when the
// worker is killed mid-frame.
// Usage: nr_helper_strip.exe <_nvngx.dll> <forwarder> <model> <worker exe> <data dir> <log> <width> <height> <preset>
// Prints the median helper turnaround per share and checks the returned rows. Synthetic input: transport, ordering and
// timing only, not image quality.
#include "pch.h"
#include "../../OptiScaler/dlssnr/DlssNr_HelperShare.h"
#include "../../OptiScaler/dlssnr/DlssNr_OwnedCommandChain.h"
#include "../../OptiScaler/dlssnr/worker/WorkerGeneration.h"
#include "nr_ngx_evaluate_fixture.h"

using Microsoft::WRL::ComPtr;
using namespace DlssNr::Worker;

namespace
{
void Must(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        char text[160];
        std::snprintf(text, sizeof(text), "%s failed: 0x%08X", what, static_cast<unsigned>(hr));
        throw std::runtime_error(text);
    }
}
void Require(bool ok, const char* what)
{
    if (!ok)
        throw std::runtime_error(what);
}

ComPtr<ID3D12Device> OpenNvidia(unsigned index)
{
    ComPtr<IDXGIFactory6> factory;
    Must(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
    unsigned seen = 0;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc {};
        adapter->GetDesc1(&desc);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || desc.VendorId != 0x10de || seen++ != index)
            continue;
        ComPtr<ID3D12Device> device;
        Must(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
        std::wcout << L"Adapter " << index << L": " << desc.Description << L"\n";
        return device;
    }
    throw std::runtime_error("NVIDIA adapter not found");
}

struct Fence
{
    ComPtr<ID3D12Fence> fence;
    uint64_t value = 0;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    explicit Fence(ID3D12Device* device) { Must(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence"); }
    ~Fence() { CloseHandle(event); }
    uint64_t Signal(ID3D12CommandQueue* queue)
    {
        Must(queue->Signal(fence.Get(), ++value), "Signal");
        return value;
    }
    bool Wait(uint64_t v, DWORD ms)
    {
        if (fence->GetCompletedValue() >= v)
            return true;
        Must(fence->SetEventOnCompletion(v, event), "SetEventOnCompletion");
        return WaitForSingleObject(event, ms) == WAIT_OBJECT_0;
    }
};

void UploadColor(ID3D12GraphicsCommandList* list, NrFixture::Surface& s, uint16_t constant, bool textured)
{
    uint8_t* mapped = nullptr;
    D3D12_RANGE none { 0, 0 };
    Must(s.upload->Map(0, &none, reinterpret_cast<void**>(&mapped)), "Map");
    std::mt19937 rng(11);
    for (UINT y = 0; y < s.rows; ++y)
    {
        auto* half = reinterpret_cast<uint16_t*>(mapped + y * s.footprint.Footprint.RowPitch);
        for (UINT x = 0; x < s.footprint.Footprint.Width; ++x)
        {
            const uint16_t v = textured ? static_cast<uint16_t>(0x3400 + (rng() & 0x3ff)) : constant;
            half[4 * x] = half[4 * x + 1] = half[4 * x + 2] = v;
            half[4 * x + 3] = 0x3c00;
        }
    }
    s.upload->Unmap(0, nullptr);
    auto dst = NrFixture::Texture(s.texture.Get());
    auto src = NrFixture::Buffer(s.upload.Get(), s.footprint);
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
}
void UploadZero(ID3D12GraphicsCommandList* list, NrFixture::Surface& s, float fill)
{
    uint8_t* mapped = nullptr;
    D3D12_RANGE none { 0, 0 };
    Must(s.upload->Map(0, &none, reinterpret_cast<void**>(&mapped)), "Map");
    std::memset(mapped, 0, static_cast<size_t>(s.bytes));
    if (fill != 0)
        for (UINT y = 0; y < s.rows; ++y)
        {
            auto* f = reinterpret_cast<float*>(mapped + y * s.footprint.Footprint.RowPitch);
            for (UINT x = 0; x < s.footprint.Footprint.Width; ++x)
                f[x] = fill;
        }
    s.upload->Unmap(0, nullptr);
    auto dst = NrFixture::Texture(s.texture.Get());
    auto src = NrFixture::Buffer(s.upload.Get(), s.footprint);
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc != 10 && argc != 11)
    {
        std::cerr << "usage: core forwarder model worker data log width height preset\n";
        return 2;
    }
    try
    {
        const unsigned width = std::stoul(argv[7]), height = std::stoul(argv[8]);
        const int preset = std::stoi(argv[9]);
        const unsigned passes = argc == 11 ? std::stoul(argv[10]) : 1u;
        auto primary = OpenNvidia(0);
        auto secondary = OpenNvidia(1);
        D3D12_COMMAND_QUEUE_DESC qd {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> queue;
        Must(primary->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
        Fence retire(primary.Get());

        NrFixture::Surface color, depth, motion, out;
        Require(NrFixture::MakeSurface(primary.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, false, color, width, height) &&
                    NrFixture::MakeSurface(primary.Get(), DXGI_FORMAT_R32_FLOAT, false, depth, width, height) &&
                    NrFixture::MakeSurface(primary.Get(), DXGI_FORMAT_R16G16_FLOAT, false, motion, width, height) &&
                    NrFixture::MakeSurface(primary.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, true, out, width, height),
                "primary textures");
        {
            ComPtr<ID3D12CommandAllocator> a;
            ComPtr<ID3D12GraphicsCommandList> l;
            Must(primary->CreateCommandAllocator(qd.Type, IID_PPV_ARGS(&a)), "alloc");
            Must(primary->CreateCommandList(0, qd.Type, a.Get(), nullptr, IID_PPV_ARGS(&l)), "list");
            UploadColor(l.Get(), color, 0, true);
            UploadZero(l.Get(), depth, 0.5f);
            UploadZero(l.Get(), motion, 0.0f);
            UploadColor(l.Get(), out, 0x3c00, false); // marker 1.0 everywhere: rows above the seam must keep it
            for (auto* s : { &color, &depth, &motion })
                NrFixture::Barrier(l.Get(), s->texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                   D3D12_RESOURCE_STATE_COPY_SOURCE);
            Must(l->Close(), "close");
            ID3D12CommandList* lists[] = { l.Get() };
            queue->ExecuteCommandLists(1, lists);
            Require(retire.Wait(retire.Signal(queue.Get()), 10000), "upload");
        }

        auto run = [&](bool killMidFrame, std::vector<std::pair<unsigned, unsigned>> phases) -> int
        {
            FrameTransfer transfer;
            Must(FrameTransfer::Create(primary.Get(), secondary.Get(), color.texture.Get(), depth.texture.Get(),
                                       motion.texture.Get(), transfer),
                 "FrameTransfer::Create");
            WorkerGeneration generation(std::move(transfer));
            InitialFrameWire settings {};
            settings.passCount = passes;
            for (unsigned i = 0; i < passes; ++i)
            {
                settings.passes[i].preset = preset;
                settings.passes[i].intensity = 0.5f;
                settings.passes[i].style = 2;
            }
            Must(generation.Start(primary.Get(), argv[4], argv[1], argv[2], argv[3], argv[5], argv[6], settings),
                 "worker start");
            HRESULT poll = E_PENDING;
            for (int i = 0; i < 600 && poll == E_PENDING; ++i)
                poll = generation.Poll(100);
            Must(poll, "worker initialization");

            struct Pending
            {
                ComPtr<ID3D12CommandAllocator> allocator;
                ComPtr<ID3D12GraphicsCommandList> list;
                std::unique_ptr<DlssNr::MultiGpu::OwnedCommandChain> chain;
                uint64_t retireValue = 0, workerFrame = 0;
            };
            std::vector<Pending> inFlight;
            uint64_t workerFrames = 0, orderedFrames = 0, checkedFrames = 0;
            auto* doneFence = generation.Transfer().Buffer().PrimaryDone();
            // Retirement is signalled after the return copy. If the GPU wait orders it, the worker's done
            // value for that frame is already complete whenever the retirement is.
            auto checkOrder = [&](const Pending& f)
            {
                ++checkedFrames;
                orderedFrames += doneFence->GetCompletedValue() >= f.workerFrame;
            };
            unsigned lastSeam = height;
            for (auto [rows, frames] : phases)
            {
                std::vector<double> turnaround;
                const unsigned seam = rows >= height ? 0u : height - rows;
                const unsigned first = seam > DlssNr::MultiGpu::HelperShare::Halo
                                           ? seam - DlssNr::MultiGpu::HelperShare::Halo : 0u;
                for (unsigned f = 0; f < frames; ++f)
                {
                    Must(generation.PollForQueue(queue.Get()), "PollForQueue (queue-ordered reuse)");
                    Pending p;
                    Must(primary->CreateCommandAllocator(qd.Type, IID_PPV_ARGS(&p.allocator)), "alloc");
                    Must(primary->CreateCommandList(0, qd.Type, p.allocator.Get(), nullptr, IID_PPV_ARGS(&p.list)),
                         "list");
                    p.chain = std::make_unique<DlssNr::MultiGpu::OwnedCommandChain>();
                    Must(p.chain->Initialize(primary.Get(), queue.Get(), p.list.Get()), "chain");
                    FrameCommand command {};
                    command.guideWidth = command.motionWidth = width;
                    command.guideHeight = command.motionHeight = height;
                    if (seam)
                    {
                        command.rowOffset = first;
                        command.rows = height - first;
                        command.depthY = command.motionY = first;
                        command.guideHeight = command.motionHeight = height - first;
                    }
                    if (seam != lastSeam)
                        command.resetMask = (1u << passes) - 1u;
                    lastSeam = seam;
                    // Kill test: hold the game queue so the outbound copy (and the worker's input) cannot run
                    // before the worker is killed.
                    Fence gate(primary.Get());
                    if (killMidFrame)
                        Must(queue->Wait(gate.fence.Get(), 1), "gate");
                    p.workerFrame = ++workerFrames;
                    Must(generation.SubmitAsync(*p.chain, queue.Get(), command, color.texture.Get(),
                                                depth.texture.Get(), motion.texture.Get()).result,
                         "SubmitAsync");
                    Must(generation.QueueWaitReturn(queue.Get()), "QueueWaitReturn");
                    // Killed after the GPU wait is queued: only the watchdog can release it now.
                    if (killMidFrame)
                        TerminateProcess(generation.ProcessHandle(), 7);
                    auto* list = p.chain->Current(); // `out` stays in COPY_DEST between frames
                    Must(generation.RecordReturn(list, out.texture.Get(), seam, height - seam), "RecordReturn");
                    Must(p.chain->SubmitFinal(), "SubmitFinal");
                    p.retireValue = retire.Signal(queue.Get());
                    Must(generation.TrackRetirement(retire.fence.Get(), p.retireValue, queue.Get()),
                         "TrackRetirement(queue)");
                    if (killMidFrame)
                    {
                        Sleep(200);
                        DWORD exitCode = 0;
                        GetExitCodeProcess(generation.ProcessHandle(), &exitCode);
                        std::cout << "Before opening the gate: retire " << retire.fence->GetCompletedValue() << "/"
                                  << p.retireValue << ", worker exit code " << exitCode << ", watchdog "
                                  << (generation.WatchFailed() ? "failed" : "running") << '\n';
                        const auto opened = std::chrono::steady_clock::now();
                        Must(gate.fence->Signal(1), "open gate");
                        // The queue must not hang on a dead worker: the watchdog signals the done fence.
                        const bool released = retire.Wait(p.retireValue, WorkerWatch::StallLimitMs + 3000);
                        std::cout << "Queue released after " << std::chrono::duration<double, std::milli>(
                                         std::chrono::steady_clock::now() - opened).count() << " ms" << '\n';
                        std::cout << "Killed worker mid-frame: primary queue " << (released ? "released" : "HUNG")
                                  << ", watchdog failure " << (generation.WatchFailed() ? "reported" : "missing")
                                  << ", gate held the frame " << (generation.WatchFailed() ? "until the kill" : "?")
                                  << ", next poll 0x" << std::hex << static_cast<unsigned>(generation.Poll())
                                  << std::dec << "\n";
                        return released && generation.WatchFailed() && FAILED(generation.Poll()) ? 0 : 1;
                    }
                    inFlight.push_back(std::move(p));
                    // Bound CPU run-ahead to two frames, as a game's present queue would.
                    if (inFlight.size() > 2)
                    {
                        Require(retire.Wait(inFlight.front().retireValue, 10000), "frame retirement");
                        checkOrder(inFlight.front());
                        inFlight.erase(inFlight.begin());
                    }
                    WorkerWatch::Sample sample;
                    if (generation.TakeTiming(sample) && f >= 4)
                        turnaround.push_back(sample.latencyMs);
                }
                Require(retire.Wait(retire.value, 10000), "phase drain");
                for (const auto& f : inFlight)
                    checkOrder(f);
                inFlight.clear();
                std::sort(turnaround.begin(), turnaround.end());
                std::cout << "helper " << (seam ? height - seam : height) << " of " << height << " rows: "
                          << (turnaround.empty() ? 0.0 : turnaround[turnaround.size() / 2])
                          << " ms median turnaround (" << turnaround.size() << " samples)\n";
            }
            for (int i = 0; i < 100 && generation.Poll() == E_PENDING; ++i)
                Sleep(10);
            Must(generation.Stop(10000), "worker stop");
            std::cout << "GPU-ordered returns: " << orderedFrames << "/" << checkedFrames
                      << " retired frames had the helper's result complete first" << std::endl;
            return orderedFrames == checkedFrames ? 0 : 1;
        };

        const unsigned half = (height / 2) / 16 * 16, quarter = (height / 4) / 16 * 16;
        int failed = run(false, { { height, 40 }, { half, 40 }, { quarter, 40 }, { 64, 40 } });

        // The last phase returned a 64-row strip: rows above its seam must still hold the 1.0 marker.
        {
            D3D12_RESOURCE_DESC rb {};
            rb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rb.Width = out.bytes;
            rb.Height = rb.DepthOrArraySize = rb.MipLevels = 1;
            rb.SampleDesc.Count = 1;
            rb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES heap { D3D12_HEAP_TYPE_READBACK };
            ComPtr<ID3D12Resource> readback;
            Must(primary->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_COPY_DEST,
                                                  nullptr, IID_PPV_ARGS(&readback)),
                 "readback");
            ComPtr<ID3D12CommandAllocator> a;
            ComPtr<ID3D12GraphicsCommandList> l;
            Must(primary->CreateCommandAllocator(qd.Type, IID_PPV_ARGS(&a)), "alloc");
            Must(primary->CreateCommandList(0, qd.Type, a.Get(), nullptr, IID_PPV_ARGS(&l)), "list");
            NrFixture::Barrier(l.Get(), out.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                               D3D12_RESOURCE_STATE_COPY_SOURCE);
            auto dst = NrFixture::Buffer(readback.Get(), out.footprint);
            auto src = NrFixture::Texture(out.texture.Get());
            l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            NrFixture::Barrier(l.Get(), out.texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                               D3D12_RESOURCE_STATE_COPY_DEST);
            Must(l->Close(), "close");
            ID3D12CommandList* lists[] = { l.Get() };
            queue->ExecuteCommandLists(1, lists);
            Require(retire.Wait(retire.Signal(queue.Get()), 10000), "readback copy");
            uint8_t* mapped = nullptr;
            D3D12_RANGE range { 0, static_cast<SIZE_T>(out.bytes) };
            Must(readback->Map(0, &range, reinterpret_cast<void**>(&mapped)), "map");
            const unsigned seam = height - 64;
            size_t markerAbove = 0, changedBelow = 0, finiteBelow = 0, pixelsAbove = 0, pixelsBelow = 0;
            for (unsigned y = 0; y < height; ++y)
            {
                auto* half16 = reinterpret_cast<const uint16_t*>(mapped + y * out.footprint.Footprint.RowPitch);
                for (unsigned x = 0; x < width; ++x)
                {
                    const uint16_t r = half16[4 * x];
                    if (y < seam)
                    {
                        ++pixelsAbove;
                        markerAbove += r == 0x3c00;
                    }
                    else
                    {
                        ++pixelsBelow;
                        changedBelow += r != 0x3c00;
                        finiteBelow += (r & 0x7c00) != 0x7c00;
                    }
                }
            }
            D3D12_RANGE none { 0, 0 };
            readback->Unmap(0, &none);
            std::cout << "Rows above seam keeping the marker: " << markerAbove << "/" << pixelsAbove
                      << "; strip rows written: " << changedBelow << "/" << pixelsBelow << ", finite " << finiteBelow
                      << "/" << pixelsBelow << "\n";
            // Earlier, larger strips overwrote rows above this seam; only the final strip's ownership is checked:
            // its rows must be written and finite.
            failed |= changedBelow * 10 < pixelsBelow * 9 || finiteBelow != pixelsBelow;
        }

        failed |= run(true, { { 64, 1 } });
        std::cout << (failed ? "Helper strip fixture FAILED\n" : "Helper strip fixture passed\n");
        return failed ? 1 : 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }
}
