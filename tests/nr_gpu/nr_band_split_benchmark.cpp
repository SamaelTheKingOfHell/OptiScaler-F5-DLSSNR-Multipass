// Measures one neural pass on one adapter at several band heights, to size a split-frame workload.
// Usage: nr_band_split_benchmark.exe <core> <forwarder> <model> <data dir> <adapter index> <width> <full height>
//        <preset> <frames> <band heights...> [--subset]
// Without --subset each band gets its own feature created at that height. With --subset one feature is created at
// the full height and evaluated with smaller heights (establishes whether a split can move without a rebuild).
// Prints median GPU milliseconds per band and whether every evaluate succeeded. Synthetic input; timing only.
#include "pch.h"
#include "../../OptiScaler/dlssnr/worker/WorkerModelSession.h"
#include "nr_ngx_evaluate_fixture.h"

using Microsoft::WRL::ComPtr;

namespace
{
void Must(HRESULT hr, const char* what)
{
    if (FAILED(hr))
        throw std::runtime_error(std::string(what) + " failed");
}

ComPtr<ID3D12Device> OpenAdapter(unsigned index, std::wstring& name)
{
    ComPtr<IDXGIFactory6> factory;
    Must(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
    unsigned seen = 0;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc {};
        adapter->GetDesc1(&desc);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || desc.VendorId != 0x10de)
            continue;
        if (seen++ != index)
            continue;
        name = desc.Description;
        ComPtr<ID3D12Device> device;
        const HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        std::wcerr << L"DXGI adapter " << i << L" " << desc.Description << L" create 0x" << std::hex
                   << static_cast<unsigned>(hr) << std::dec << L'\n';
        Must(hr, "D3D12CreateDevice");
        return device;
    }
    throw std::runtime_error("adapter index not found");
}

struct Timer
{
    ComPtr<ID3D12QueryHeap> heap;
    ComPtr<ID3D12Resource> readback;
    explicit Timer(ID3D12Device* device)
    {
        D3D12_QUERY_HEAP_DESC desc { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2, 0 };
        Must(device->CreateQueryHeap(&desc, IID_PPV_ARGS(&heap)), "CreateQueryHeap");
        D3D12_HEAP_PROPERTIES props { D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC buffer {};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = 16;
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Must(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                             nullptr, IID_PPV_ARGS(&readback)),
             "Create timestamp readback");
    }
};

void WaitIdle(ID3D12Device* device, ID3D12CommandQueue* queue)
{
    ComPtr<ID3D12Fence> fence;
    Must(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Must(queue->Signal(fence.Get(), 1), "Signal");
    Must(fence->SetEventOnCompletion(1, event), "SetEventOnCompletion");
    const DWORD waited = WaitForSingleObject(event, 30000);
    CloseHandle(event);
    if (waited != WAIT_OBJECT_0 || FAILED(device->GetDeviceRemovedReason()))
    {
        std::cerr << "GPU work did not retire; terminating.\n" << std::flush;
        ExitProcess(3);
    }
}

struct Inputs
{
    NrFixture::Surface color, depth, motion, output;
};

void Upload(ID3D12GraphicsCommandList* list, NrFixture::Surface& s, bool isColor, D3D12_RESOURCE_STATES next)
{
    uint8_t* mapped = nullptr;
    D3D12_RANGE none { 0, 0 };
    Must(s.upload->Map(0, &none, reinterpret_cast<void**>(&mapped)), "Map");
    std::mt19937 rng(7);
    for (UINT y = 0; y < s.rows; ++y)
    {
        auto* row = mapped + y * s.footprint.Footprint.RowPitch;
        std::memset(row, 0, static_cast<size_t>(s.rowBytes));
        if (isColor)
        {
            auto* half = reinterpret_cast<uint16_t*>(row);
            for (UINT x = 0; x < s.footprint.Footprint.Width; ++x)
            {
                // Varied texture so the model does real work; values in [0.25, 0.75).
                const uint16_t v = static_cast<uint16_t>(0x3400 + (rng() & 0x3ff));
                half[4 * x] = v;
                half[4 * x + 1] = static_cast<uint16_t>(v ^ 0x55);
                half[4 * x + 2] = static_cast<uint16_t>(v ^ 0xaa);
                half[4 * x + 3] = 0x3c00;
            }
        }
    }
    s.upload->Unmap(0, nullptr);
    auto dst = NrFixture::Texture(s.texture.Get());
    auto src = NrFixture::Buffer(s.upload.Get(), s.footprint);
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    NrFixture::Barrier(list, s.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, next);
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 11)
    {
        std::cerr << "usage: core forwarder model data adapter width fullHeight preset frames heights... [--subset]\n";
        return 2;
    }
    try
    {
        const unsigned adapter = std::stoul(argv[5]), width = std::stoul(argv[6]), full = std::stoul(argv[7]);
        const int preset = std::stoi(argv[8]);
        const unsigned frames = std::stoul(argv[9]);
        bool subset = false;
        std::vector<unsigned> heights;
        for (int i = 10; i < argc; ++i)
        {
            if (std::wstring(argv[i]) == L"--subset")
                subset = true;
            else
                heights.push_back(std::stoul(argv[i]));
        }
        std::wstring name;
        auto device = OpenAdapter(adapter, name);
        std::wcout << L"Adapter " << adapter << L": " << name << L"\n";

        DlssNr::Worker::FeatureTuning tuning;
        tuning.preset = preset;
        tuning.intensity = 0.5f;
        tuning.style = 2;

        auto run = [&](DlssNr::Worker::ModelSession& session, void* /*unused*/, unsigned featureH, unsigned h)
        {
            Inputs in;
            if (!NrFixture::MakeSurface(device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, false, in.color, width, featureH) ||
                !NrFixture::MakeSurface(device.Get(), DXGI_FORMAT_R32_FLOAT, false, in.depth, width, featureH) ||
                !NrFixture::MakeSurface(device.Get(), DXGI_FORMAT_R16G16_FLOAT, false, in.motion, width, featureH) ||
                !NrFixture::MakeSurface(device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, true, in.output, width, featureH))
                throw std::runtime_error("textures");
            ComPtr<ID3D12CommandAllocator> alloc;
            ComPtr<ID3D12GraphicsCommandList> list;
            Must(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "alloc");
            Must(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                           IID_PPV_ARGS(&list)),
                 "list");
            Upload(list.Get(), in.color, true, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Upload(list.Get(), in.depth, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Upload(list.Get(), in.motion, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            NrFixture::Barrier(list.Get(), in.output.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Must(list->Close(), "close");
            ID3D12CommandList* lists[] = { list.Get() };
            session.Queue()->ExecuteCommandLists(1, lists);
            WaitIdle(device.Get(), session.Queue());

            Timer timer(device.Get());
            UINT64 frequency = 0;
            Must(session.Queue()->GetTimestampFrequency(&frequency), "frequency");
            std::vector<double> samples;
            bool ok = true;
            for (unsigned f = 0; f < frames + 5; ++f)
            {
                Must(alloc->Reset(), "alloc reset");
                Must(list->Reset(alloc.Get(), nullptr), "list reset");
                list->EndQuery(timer.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
                DlssNr::Worker::Evaluation e {};
                e.color = in.color.texture.Get();
                e.depth = in.depth.texture.Get();
                e.motion = in.motion.texture.Get();
                e.output = in.output.texture.Get();
                e.width = width;
                e.height = h;
                e.guideWidth = e.motionWidth = width;
                e.guideHeight = e.motionHeight = h;
                e.reset = f == 0;
                e.intensity = 0.5f;
                e.style = 2;
                int result = 0;
                if (h == featureH)
                    result = session.Record(list.Get(), e, 0);
                else
                    result = session.RecordUnchecked(list.Get(), e, 0);
                ok = ok && result == 1;
                list->EndQuery(timer.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
                list->ResolveQueryData(timer.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, timer.readback.Get(), 0);
                Must(list->Close(), "close");
                session.Queue()->ExecuteCommandLists(1, lists);
                WaitIdle(device.Get(), session.Queue());
                uint64_t* stamps = nullptr;
                D3D12_RANGE range { 0, 16 };
                Must(timer.readback->Map(0, &range, reinterpret_cast<void**>(&stamps)), "map stamps");
                const double ms = double(stamps[1] - stamps[0]) * 1000.0 / double(frequency);
                D3D12_RANGE none { 0, 0 };
                timer.readback->Unmap(0, &none);
                if (f >= 5)
                    samples.push_back(ms);
            }
            std::sort(samples.begin(), samples.end());
            const double median = samples[samples.size() / 2];
            const double p90 = samples[(samples.size() * 9) / 10];
            std::cout << "band " << width << "x" << h << " (feature " << width << "x" << featureH << "): median "
                      << median << " ms, p90 " << p90 << " ms, evaluate " << (ok ? "ok" : "FAILED") << "\n"
                      << std::flush;
        };

        if (subset)
        {
            DlssNr::Worker::ModelSession session;
            session.Initialize(device.Get(), argv[1], argv[2], argv[3], argv[4], width, full, { tuning },
                               0x24480451ull);
            for (unsigned h : heights)
                run(session, nullptr, full, h);
        }
        else
            for (unsigned h : heights)
            {
                DlssNr::Worker::ModelSession session;
                session.Initialize(device.Get(), argv[1], argv[2], argv[3], argv[4], width, h, { tuning },
                                   0x24480451ull);
                run(session, nullptr, h, h);
            }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }
}
