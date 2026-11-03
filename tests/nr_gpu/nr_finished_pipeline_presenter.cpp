#include "../../OptiScaler/dlssnr/worker/FinishedPicturePresenter.h"
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
using DlssNr::Worker::FinishedPicturePresenter;

static void Check(HRESULT hr, const char* operation)
{
    if (FAILED(hr)) throw std::runtime_error(operation);
}
static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
static D3D12_RESOURCE_BARRIER Barrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                       D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}
static ComPtr<ID3D12Resource> Texture(ID3D12Device* device, UINT width = 64,
                                      DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = width; d.Height = 32; d.DepthOrArraySize = d.MipLevels = 1;
    d.Format = format; d.SampleDesc.Count = 1;
    ComPtr<ID3D12Resource> resource;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON,
                                          nullptr, IID_PPV_ARGS(&resource)), "Create test texture");
    return resource;
}
static ComPtr<ID3D12Resource> Buffer(ID3D12Device* device, UINT64 bytes, D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = type;
    D3D12_RESOURCE_DESC d {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes; d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> resource;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
        type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&resource)), "Create test buffer");
    return resource;
}
struct Recording
{
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    Recording(ID3D12Device* device)
    {
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Allocator");
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                        IID_PPV_ARGS(&commands)), "Commands");
    }
    void SubmitAndWait(ID3D12Device* device, ID3D12CommandQueue* queue)
    {
        ComPtr<ID3D12Fence> fence;
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Test fence");
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) throw std::runtime_error("Test event");
        Check(commands->Close(), "Close test commands");
        ID3D12CommandList* lists[] = { commands.Get() };
        queue->ExecuteCommandLists(1, lists);
        if (FAILED(queue->Signal(fence.Get(), 1)) || FAILED(fence->SetEventOnCompletion(1, event)) ||
            WaitForSingleObject(event, 30000) != WAIT_OBJECT_0)
        {
            std::cerr << "Unretired test work; exiting without releasing GPU references.\n" << std::flush;
            ExitProcess(3);
        }
        CloseHandle(event);
    }
};
static D3D12_TEXTURE_COPY_LOCATION TexLocation(ID3D12Resource* resource)
{
    D3D12_TEXTURE_COPY_LOCATION loc {};
    loc.pResource = resource;
    loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    return loc;
}
static D3D12_TEXTURE_COPY_LOCATION Footprint(ID3D12Device* device, ID3D12Resource* texture,
                                             ID3D12Resource* buffer, UINT64& bytes)
{
    D3D12_TEXTURE_COPY_LOCATION loc {};
    loc.pResource = buffer;
    loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    const auto d = texture->GetDesc();
    device->GetCopyableFootprints(&d, 0, 1, 0, &loc.PlacedFootprint, nullptr, nullptr, &bytes);
    return loc;
}
static void Fill(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* texture,
                   const std::array<uint8_t, 4>& rgba)
{
    UINT64 bytes = 0;
    auto src = Footprint(device, texture, nullptr, bytes);
    auto upload = Buffer(device, bytes, D3D12_HEAP_TYPE_UPLOAD);
    src.pResource = upload.Get();
    uint8_t* mapped = nullptr;
    D3D12_RANGE noRead { 0, 0 };
    Check(upload->Map(0, &noRead, reinterpret_cast<void**>(&mapped)), "Map upload");
    for (UINT y = 0; y < 32; ++y)
        for (UINT x = 0; x < 64; ++x)
            std::memcpy(mapped + y * src.PlacedFootprint.Footprint.RowPitch + x * 4, rgba.data(), 4);
    upload->Unmap(0, nullptr);
    Recording r(device);
    auto before = Barrier(texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    r.commands->ResourceBarrier(1, &before);
    auto dst = TexLocation(texture);
    r.commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    auto after = Barrier(texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    r.commands->ResourceBarrier(1, &after);
    r.SubmitAndWait(device, queue);
}
static void Verify(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* texture,
                     const std::array<uint8_t, 4>& rgba)
{
    UINT64 bytes = 0;
    auto dst = Footprint(device, texture, nullptr, bytes);
    auto readback = Buffer(device, bytes, D3D12_HEAP_TYPE_READBACK);
    dst.pResource = readback.Get();
    Recording r(device);
    auto before = Barrier(texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    r.commands->ResourceBarrier(1, &before);
    auto src = TexLocation(texture);
    r.commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    auto after = Barrier(texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    r.commands->ResourceBarrier(1, &after);
    r.SubmitAndWait(device, queue);
    uint8_t* mapped = nullptr;
    D3D12_RANGE read { 0, static_cast<SIZE_T>(bytes) }, noWrite { 0, 0 };
    Check(readback->Map(0, &read, reinterpret_cast<void**>(&mapped)), "Map readback");
    bool correct = true;
    for (UINT y = 0; y < 32; ++y)
        for (UINT x = 0; x < 64; ++x)
            correct &= std::memcmp(mapped + y * dst.PlacedFootprint.Footprint.RowPitch + x * 4, rgba.data(), 4) == 0;
    readback->Unmap(0, &noWrite);
    Require(correct, "Presented pixels differ from the completed source picture");
}
static void VerifyDebug(ID3D12Device* device)
{
    ComPtr<ID3D12InfoQueue> info;
    Check(device->QueryInterface(IID_PPV_ARGS(&info)), "Debug info queue");
    for (UINT64 i = 0; i < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i)
    {
        SIZE_T bytes = 0;
        Check(info->GetMessage(i, nullptr, &bytes), "Debug message size");
        std::vector<uint8_t> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        Check(info->GetMessage(i, message, &bytes), "Debug message");
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
            throw std::runtime_error(message->pDescription);
    }
}
int main()
{
    try
    {
        ComPtr<ID3D12Debug> debug;
        Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "D3D12 debug layer is required");
        debug->EnableDebugLayer();
        ComPtr<IDXGIFactory4> factory;
        Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Factory");
        ComPtr<IDXGIAdapter> warp;
        Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP adapter");
        ComPtr<ID3D12Device> device;
        Check(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "WARP device");
        D3D12_COMMAND_QUEUE_DESC desc {};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> queue, otherQueue;
        Check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "Primary queue");
        Check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&otherQueue)), "Other queue");
        auto source = Texture(device.Get()), target = Texture(device.Get());
        const std::array<uint8_t, 4> older { 12, 34, 56, 255 }, newer { 201, 22, 43, 255 };
        Fill(device.Get(), queue.Get(), source.Get(), older);
        Fill(device.Get(), queue.Get(), target.Get(), newer);
        Verify(device.Get(), queue.Get(), target.Get(), newer);
        FinishedPicturePresenter presenter;
        Check(presenter.Initialize(device.Get(), queue.Get()), "Initialize presenter");
        Require(presenter.UsesQueue(queue.Get()) && !presenter.UsesQueue(otherQueue.Get()), "Queue identity mismatch");
        Require(presenter.Drain(INFINITE) == E_INVALIDARG, "Infinite drain accepted");
        Require(presenter.Submit(source.Get(), source.Get()).result == E_INVALIDARG, "Aliased source accepted");
        auto wrongSize = Texture(device.Get(), 65);
        Require(presenter.Submit(wrongSize.Get(), target.Get()).result == E_INVALIDARG, "Wrong size accepted");
        auto wrongFormat = Texture(device.Get(), 64, DXGI_FORMAT_B8G8R8A8_UNORM);
        Require(presenter.Submit(wrongFormat.Get(), target.Get()).result == E_INVALIDARG, "Wrong format accepted");
        bool wrongDeviceTested = false;
        for (UINT i = 0; !wrongDeviceTested; ++i)
        {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
            ComPtr<ID3D12Device> otherDevice;
            if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&otherDevice)))) continue;
            if (otherDevice == device) continue;
            auto foreign = Texture(otherDevice.Get());
            Require(presenter.Submit(foreign.Get(), target.Get()).result == E_INVALIDARG, "Wrong device accepted");
            wrongDeviceTested = true;
        }
        ComPtr<ID3D12Fence> gate;
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "Queue gate");
        struct ReleaseGate { ID3D12Fence* fence; ~ReleaseGate() { fence->Signal(1); } } release { gate.Get() };
        Check(queue->Wait(gate.Get(), 1), "Test-only local queue gate");
        std::array<FinishedPicturePresenter::Submission, 3> submitted;
        for (auto& copy : submitted)
        {
            copy = presenter.Submit(source.Get(), target.Get());
            Check(copy.result, "Submit queued copy");
            Require(copy.fence && copy.value, "Submission returned no retirement marker");
        }
        Require(presenter.Submit(source.Get(), target.Get()).result == E_PENDING, "Full ring did not return E_PENDING");
        Require(presenter.Drain(0) == E_PENDING, "Busy drain did not return E_PENDING");
        Require(presenter.Drain(5) == HRESULT_FROM_WIN32(WAIT_TIMEOUT), "Finite drain did not time out");
        Check(gate->Signal(1), "Release queue gate");
        Check(presenter.Drain(30000), "Drain queued copies");
        for (const auto& copy : submitted)
            Require(copy.fence->GetCompletedValue() >= copy.value, "Retirement value was not completed");
        Verify(device.Get(), queue.Get(), target.Get(), older);
        for (unsigned i = 0; i < 12; ++i)
        {
            Fill(device.Get(), queue.Get(), target.Get(), newer);
            auto copy = presenter.Submit(source.Get(), target.Get());
            Check(copy.result, "Reuse presenter ring");
            Check(presenter.Drain(30000), "Drain reused ring");
            Verify(device.Get(), queue.Get(), target.Get(), older);
        }
        Check(presenter.Collect(), "Collect retired resources");
        VerifyDebug(device.Get());
        std::cout << "WARP presenter: exact completed-picture copy, returned fences, full ring, finite timeout and 12 reuses passed; "
                  << (wrongDeviceTested ? "wrong-device rejection passed.\n" : "wrong-device case unavailable.\n");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
