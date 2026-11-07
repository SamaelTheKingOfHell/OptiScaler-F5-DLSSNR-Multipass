// Runs the production worker's split calibration (WorkerSplitMain.h, wire.split.cut < 0) on the second NVIDIA adapter
// through the production WorkerClient, and prints the plan it writes. The first adapter only owns the small
// cross-adapter buffer the handshake needs; no kernel runs on it.
// Usage: nr_split_calibrate.exe <_nvngx.dll> <forwarder> <model> <worker exe> <data dir> <log> <width> <height> <preset>
#include "pch.h"
#include "../../OptiScaler/dlssnr/worker/WorkerClient.h"
#include <chrono>
#include <filesystem>
#include <fstream>

using Microsoft::WRL::ComPtr;
using namespace DlssNr::Worker;

static ComPtr<ID3D12Device> OpenNvidia(unsigned index)
{
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
        throw std::runtime_error("CreateDXGIFactory2");
    unsigned seen = 0;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc {};
        adapter->GetDesc1(&desc);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || desc.VendorId != 0x10de || seen++ != index)
            continue;
        ComPtr<ID3D12Device> device;
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
            throw std::runtime_error("D3D12CreateDevice");
        std::wcout << L"Adapter " << index << L": " << desc.Description << L"\n";
        return device;
    }
    throw std::runtime_error("NVIDIA adapter not found");
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 10)
    {
        std::cerr << "usage: core forwarder model worker data log width height preset\n";
        return 2;
    }
    try
    {
        auto primary = OpenNvidia(0);
        auto secondary = OpenNvidia(1);
        DlssNr::MultiGpu::CrossAdapterBuffer transfer;
        if (FAILED(DlssNr::MultiGpu::CrossAdapterBuffer::Create(primary.Get(), secondary.Get(), 65536, transfer)))
            throw std::runtime_error("cross-adapter buffer");
        InitialFrameWire wire {};
        wire.width = std::stoul(argv[7]);
        wire.height = std::stoul(argv[8]);
        wire.frameCount = 0;
        wire.passCount = 1;
        wire.passes[0].preset = std::stoi(argv[9]);
        wire.bytes = transfer.Size();
        wire.primary = primary->GetAdapterLuid();
        wire.secondary = secondary->GetAdapterLuid();
        wire.split.cut = -1;
        std::filesystem::create_directories(argv[5]);
        const auto planPath = std::filesystem::path(argv[5]) / L"split-plan.txt";
        std::filesystem::remove(planPath);
        WorkerClient client;
        const auto started = std::chrono::steady_clock::now();
        if (FAILED(client.Start(primary.Get(), argv[4], argv[1], argv[2], argv[3], argv[5], argv[6], transfer, wire)))
            throw std::runtime_error("worker start");
        HRESULT ready = E_PENDING;
        for (int i = 0; i < 3000 && ready == E_PENDING; ++i)
            ready = client.WaitForReady(100);
        if (FAILED(ready))
            throw std::runtime_error("calibration did not finish (see the worker log)");
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::ifstream plan(planPath);
        if (!plan)
            throw std::runtime_error("no plan written");
        std::cout << "calibration took " << seconds << " s\n" << plan.rdbuf();
        client.Stop(10000);
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
