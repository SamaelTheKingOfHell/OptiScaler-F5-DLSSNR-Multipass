// GPU test for dlssnr/DlssNr_GpuLifetime.cpp, the tracker DLSS-NR's parked features and textures wait on
// (shaders/dlssnr/DlssNr_Dx12.cpp ParkNr, issue #271), no game: a WARP device, and a queue held back by a fence the CPU
// signals when the test says so, so "the GPU is still running it" is certain, not a race.
//   - a retire waits for a submitted list until the GPU has run it AND the list was reset (or destroyed),
//   - a retire made while the list was still being recorded (a rebuild in the middle of a frame) waits for that list too,
//   - a retire owned by an expired owner is dropped unrun, even while a recording it waits on never completes,
//   - a list destroyed instead of reset closes its recording.
//
//   vcvars64, then from the repo root:
//   cl /std:c++20 /EHsc /O2 /I tests\shim\gpu_lifetime tests\nr_gpu_lifetime_gpu.cpp OptiScaler\dlssnr\DlssNr_GpuLifetime.cpp d3d12.lib dxgi.lib ole32.lib

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdio>
#include <memory>

#include "../OptiScaler/dlssnr/DlssNr_GpuLifetime.h"

using Microsoft::WRL::ComPtr;

static int fails = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                                                \
            ++fails;                                                                                                   \
        }                                                                                                              \
    } while (0)

struct Gpu
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12Fence> gate; // the queue waits on it before every list: the CPU decides when the GPU may go on
    ComPtr<ID3D12Fence> idle;
    UINT64 gateValue = 0, idleValue = 0;

    bool Init()
    {
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> warp;

        return SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
               SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) &&
               SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) &&
               SUCCEEDED(device->CreateCommandQueue(&kQueue, IID_PPV_ARGS(&queue))) &&
               SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
               SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))) &&
               SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&idle)));
    }

    ComPtr<ID3D12GraphicsCommandList> NewList()
    {
        ComPtr<ID3D12GraphicsCommandList> list;
        device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list));
        return list;
    }

    // Closes and submits `list` behind the gate, and tells the tracker as the submit hook does.
    void Submit(DlssNr::GpuLifetime& tracker, ID3D12GraphicsCommandList* list)
    {
        list->Close();
        queue->Wait(gate.Get(), ++gateValue);
        ID3D12CommandList* lists[] = { list };
        queue->ExecuteCommandLists(1, lists);
        tracker.Submitted(queue.Get(), 1, lists);
    }

    // Lets the GPU run everything submitted so far and waits until it has.
    void Release()
    {
        gate->Signal(gateValue);
        queue->Signal(idle.Get(), ++idleValue);
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        idle->SetEventOnCompletion(idleValue, event);
        WaitForSingleObject(event, 10000);
        CloseHandle(event);
    }

    static constexpr D3D12_COMMAND_QUEUE_DESC kQueue { D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE,
                                                       0 };
};

int main()
{
    Gpu gpu;

    if (!gpu.Init())
    {
        printf("no WARP device\n");
        return 1;
    }

    // Recorded, submitted, retired: not while the GPU holds it, not after it ran but before the reset, then yes.
    {
        DlssNr::GpuLifetime tracker;
        auto list = gpu.NewList();
        tracker.Record(list.Get());
        gpu.Submit(tracker, list.Get());

        bool ran = false;
        tracker.Retire([&] { ran = true; });

        tracker.Collect();
        CHECK(!ran);

        gpu.Release();
        tracker.Collect();
        CHECK(!ran); // still open: the game has not reset it, and could submit it again

        tracker.ResetRecording(list.Get());
        tracker.Collect();
        CHECK(ran);
    }

    // Retired in the middle of the recording (before the list is closed or submitted): the list it was parked on counts.
    {
        DlssNr::GpuLifetime tracker;
        auto list = gpu.NewList();
        tracker.Record(list.Get());

        bool ran = false;
        tracker.Retire([&] { ran = true; });

        gpu.Submit(tracker, list.Get());
        tracker.Collect();
        CHECK(!ran);

        gpu.Release();
        tracker.ResetRecording(list.Get());
        tracker.Collect();
        CHECK(ran);
    }

    // Owned by an owner that expired: dropped, never run, though its recording never completes.
    {
        DlssNr::GpuLifetime tracker;
        auto list = gpu.NewList();
        tracker.Record(list.Get());
        gpu.Submit(tracker, list.Get());

        bool ran = false;
        auto owner = std::make_shared<int>(0);
        tracker.Retire([&] { ran = true; }, owner);
        owner.reset();
        tracker.Collect();

        gpu.Release();
        tracker.ResetRecording(list.Get());
        tracker.Collect();
        CHECK(!ran);
    }

    // Owned, owner still alive: runs as a plain retire does.
    {
        DlssNr::GpuLifetime tracker;
        auto list = gpu.NewList();
        tracker.Record(list.Get());
        gpu.Submit(tracker, list.Get());

        bool ran = false;
        auto owner = std::make_shared<int>(0);
        tracker.Retire([&] { ran = true; }, owner);

        gpu.Release();
        tracker.ResetRecording(list.Get());
        tracker.Collect();
        CHECK(ran);
    }

    // A list released instead of reset: its recording closes once it is destroyed.
    {
        DlssNr::GpuLifetime tracker;
        auto list = gpu.NewList();
        tracker.Record(list.Get());
        gpu.Submit(tracker, list.Get());

        bool ran = false;
        tracker.Retire([&] { ran = true; });

        gpu.Release();
        list.Reset();
        tracker.Collect();
        CHECK(ran);
    }

    if (fails == 0)
        printf("gpu lifetime: ok\n");

    return fails == 0 ? 0 : 1;
}
