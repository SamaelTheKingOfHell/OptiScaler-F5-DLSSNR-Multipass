#include "pch.h"
#include "../../OptiScaler/dlssnr/DlssNr_CrossAdapterTexture.h"
#include "../../OptiScaler/dlssnr/DlssNr_CrossAdapterSubmit.h"
#include <d3d12sdklayers.h>
#include <iomanip>
#include <optional>
#include <sstream>

using Microsoft::WRL::ComPtr;

static void Check(HRESULT hr, const char* operation)
{
    if (FAILED(hr))
    {
        std::ostringstream message;
        message << operation << " failed: 0x" << std::hex << static_cast<unsigned long>(hr);
        throw std::runtime_error(message.str());
    }
}

struct Queue
{
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    explicit Queue(ID3D12Device* device)
    {
        D3D12_COMMAND_QUEUE_DESC desc {};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
        Check(device->CreateCommandAllocator(desc.Type, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");
    }
    ComPtr<ID3D12GraphicsCommandList> List(ID3D12Device* device)
    {
        ComPtr<ID3D12GraphicsCommandList> list;
        Check(
            device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)),
            "CreateCommandList");
        return list;
    }
    void Submit(ID3D12GraphicsCommandList* list)
    {
        Check(list->Close(), "Close command list");
        ID3D12CommandList* lists[] = { list };
        queue->ExecuteCommandLists(1, lists);
    }
};

static ComPtr<ID3D12Resource> Buffer(ID3D12Device* device, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES state,
                                     uint64_t bytes)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = heapType;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> resource;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource)),
          "CreateCommittedResource");
    return resource;
}

static void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                       D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
}

static ComPtr<ID3D12Resource> Texture(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    ComPtr<ID3D12Resource> texture;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(&texture)),
          "Create texture");
    return texture;
}

static D3D12_TEXTURE_COPY_LOCATION TextureLocation(ID3D12Resource* texture)
{
    D3D12_TEXTURE_COPY_LOCATION location {};
    location.pResource = texture;
    location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    location.SubresourceIndex = 0;
    return location;
}

static D3D12_TEXTURE_COPY_LOCATION BufferLocation(ID3D12Resource* buffer,
                                                  const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint)
{
    D3D12_TEXTURE_COPY_LOCATION location {};
    location.pResource = buffer;
    location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    location.PlacedFootprint = footprint;
    return location;
}

static void SubmitSplit(DlssNr::MultiGpu::CrossAdapterSubmit& submission, ID3D12GraphicsCommandList* before,
                        ID3D12GraphicsCommandList* work, ID3D12GraphicsCommandList* after, uint64_t expectedTicket)
{
    const auto bad = submission.Submit(work, before, after);
    if (bad.result != E_INVALIDARG || bad.phase != DlssNr::MultiGpu::SubmissionPhase::Validate || bad.ticket != 0)
        throw std::runtime_error("Misassigned command-list devices were not rejected before submission");
    const auto result = submission.Submit(before, work, after);
    Check(result.result, "Submit cross-adapter lists");
    if (result.phase != DlssNr::MultiGpu::SubmissionPhase::Complete || result.ticket != expectedTicket)
        throw std::runtime_error("Unexpected cross-adapter submission status");
}

static void TextureRoundTrip(ID3D12Device* primary, ID3D12Device* secondary, Queue& primaryQueue, Queue& secondaryQueue,
                             DXGI_FORMAT format)
{
    uint8_t* data = nullptr;
    D3D12_RANGE noRead { 0, 0 };
    // A padded texture footprint exercises the copies needed for actual image resources.
    D3D12_RESOURCE_DESC image {};
    image.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    image.Width = 31;
    image.Height = 17;
    image.DepthOrArraySize = image.MipLevels = 1;
    image.Format = format;
    image.SampleDesc.Count = 1;
    image.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    DlssNr::MultiGpu::CrossAdapterTexture imageTransfer;
    Check(DlssNr::MultiGpu::CrossAdapterTexture::Create(primary, secondary, image, imageTransfer),
          "Create image transport");
    const auto primaryFootprint = imageTransfer.Footprint();
    const auto primaryRows = imageTransfer.Rows();
    const auto primaryRowBytes = imageTransfer.RowBytes();
    const auto primaryBytes = imageTransfer.Buffer().Size();
    auto imageUpload = Buffer(primary, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, primaryBytes);
    auto imageResult = Buffer(primary, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, primaryBytes);
    auto primaryImage = Texture(primary, image);
    auto secondaryImage = Texture(secondary, image);
    auto resultImage = Texture(primary, image);
    Check(imageUpload->Map(0, &noRead, reinterpret_cast<void**>(&data)), "Map image input");
    std::memset(data, 0, static_cast<size_t>(primaryBytes));
    for (UINT y = 0; y < primaryRows; ++y)
        for (UINT64 x = 0; x < primaryRowBytes; ++x)
            data[y * primaryFootprint.Footprint.RowPitch + x] = static_cast<uint8_t>((x * 7u + y * 11u) & 255u);
    imageUpload->Unmap(0, nullptr);
    const auto uploadLocation = BufferLocation(imageUpload.Get(), primaryFootprint);
    const auto primaryLocation = TextureLocation(primaryImage.Get());
    const auto primaryTransferLocation = imageTransfer.PrimaryLocation();
    const auto secondaryTransferLocation = imageTransfer.SecondaryLocation();
    const auto secondaryLocation = TextureLocation(secondaryImage.Get());
    const auto resultLocation = TextureLocation(resultImage.Get());
    const auto resultBufferLocation = BufferLocation(imageResult.Get(), primaryFootprint);
    auto imageOutbound = primaryQueue.List(primary);
    imageOutbound->CopyTextureRegion(&primaryLocation, 0, 0, 0, &uploadLocation, nullptr);
    Transition(imageOutbound.Get(), primaryImage.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(imageOutbound.Get(), imageTransfer.Buffer().PrimaryBuffer(), D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_COPY_DEST);
    imageOutbound->CopyTextureRegion(&primaryTransferLocation, 0, 0, 0, &primaryLocation, nullptr);
    Transition(imageOutbound.Get(), imageTransfer.Buffer().PrimaryBuffer(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_COMMON);
    Check(imageOutbound->Close(), "Close primary image outbound");
    auto imageProcess = secondaryQueue.List(secondary);
    Transition(imageProcess.Get(), imageTransfer.Buffer().SecondaryBuffer(), D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    imageProcess->CopyTextureRegion(&secondaryLocation, 0, 0, 0, &secondaryTransferLocation, nullptr);
    Transition(imageProcess.Get(), secondaryImage.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(imageProcess.Get(), imageTransfer.Buffer().SecondaryBuffer(), D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_COPY_DEST);
    imageProcess->CopyTextureRegion(&secondaryTransferLocation, 0, 0, 0, &secondaryLocation, nullptr);
    Transition(imageProcess.Get(), imageTransfer.Buffer().SecondaryBuffer(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_COMMON);
    Check(imageProcess->Close(), "Close secondary image work");
    auto imageInbound = primaryQueue.List(primary);
    Transition(imageInbound.Get(), imageTransfer.Buffer().PrimaryBuffer(), D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    imageInbound->CopyTextureRegion(&resultLocation, 0, 0, 0, &primaryTransferLocation, nullptr);
    Transition(imageInbound.Get(), imageTransfer.Buffer().PrimaryBuffer(), D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_COMMON);
    Transition(imageInbound.Get(), resultImage.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    imageInbound->CopyTextureRegion(&resultBufferLocation, 0, 0, 0, &resultLocation, nullptr);
    Check(imageInbound->Close(), "Close primary image return");
    DlssNr::MultiGpu::CrossAdapterSubmit imageSubmission(primaryQueue.queue.Get(), secondaryQueue.queue.Get(),
                                                         imageTransfer.Buffer());
    SubmitSplit(imageSubmission, imageOutbound.Get(), imageProcess.Get(), imageInbound.Get(), 1);
    ComPtr<ID3D12Fence> finished;
    Check(primary->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&finished)), "Create image completion fence");
    Check(primaryQueue.queue->Signal(finished.Get(), 1), "Signal image completion");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event)
        throw std::runtime_error("CreateEvent failed");
    HRESULT hr = finished->SetEventOnCompletion(1, event);
    if (FAILED(hr))
    {
        CloseHandle(event);
        Check(hr, "Set image event");
    }
    DWORD waited = WaitForSingleObject(event, 10000);
    CloseHandle(event);
    if (waited != WAIT_OBJECT_0)
        throw std::runtime_error("GPU image round trip timed out");
    D3D12_RANGE imageRange { 0, static_cast<SIZE_T>(primaryBytes) };
    Check(imageResult->Map(0, &imageRange, reinterpret_cast<void**>(&data)), "Map image result");
    for (UINT y = 0; y < primaryRows; ++y)
        for (UINT64 x = 0; x < primaryRowBytes; ++x)
            if (data[y * primaryFootprint.Footprint.RowPitch + x] != static_cast<uint8_t>((x * 7u + y * 11u) & 255u))
            {
                imageResult->Unmap(0, &noRead);
                throw std::runtime_error("Round trip image pixels differ");
            }
    imageResult->Unmap(0, &noRead);
    std::cout << "Padded texture round trip passed: " << image.Width << "x" << image.Height
              << " format=" << static_cast<unsigned>(format) << ".\n";
}

int main(int argc, char** argv)
{
    try
    {
        const bool argumentsOnly = argc == 2 && std::strcmp(argv[1], "--arguments") == 0;
        const bool useWarp = argc == 2 && std::strcmp(argv[1], "--warp") == 0;
        if (argc != 1 && !argumentsOnly && !useWarp)
        {
            std::cerr << "Usage: nr_cross_adapter_roundtrip [--arguments|--warp]\n";
            return 2;
        }
        ComPtr<ID3D12Debug> debug;
        const bool debugEnabled = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        if (debugEnabled)
            debug->EnableDebugLayer();
        std::cout << "D3D12 debug layer " << (debugEnabled ? "enabled" : "unavailable") << "\n";
        ComPtr<IDXGIFactory4> factory;
        Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
        std::vector<ComPtr<ID3D12Device>> devices;
        std::optional<LUID> displayLuid;
        const POINT origin { 0, 0 };
        const HMONITOR primaryMonitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
        for (UINT i = 0;; ++i)
        {
            ComPtr<IDXGIAdapter1> adapter;
            HRESULT hr = factory->EnumAdapters1(i, &adapter);
            if (hr == DXGI_ERROR_NOT_FOUND)
                break;
            Check(hr, "EnumAdapters1");
            DXGI_ADAPTER_DESC1 desc {};
            Check(adapter->GetDesc1(&desc), "GetDesc1");
            std::wcout << L"Enumerated adapter: " << desc.Description << L" vendor=" << std::hex << desc.VendorId
                       << std::dec << L"\n"
                       << std::flush;
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || desc.VendorId != 0x10de)
                continue;
            for (UINT outputIndex = 0;; ++outputIndex)
            {
                ComPtr<IDXGIOutput> output;
                const HRESULT outputResult = adapter->EnumOutputs(outputIndex, &output);
                if (outputResult == DXGI_ERROR_NOT_FOUND)
                    break;
                Check(outputResult, "EnumOutputs");
                DXGI_OUTPUT_DESC outputDesc {};
                Check(output->GetDesc(&outputDesc), "Get output description");
                if (outputDesc.AttachedToDesktop && outputDesc.Monitor == primaryMonitor)
                    displayLuid = desc.AdapterLuid;
            }
            ComPtr<ID3D12Device> device;
            const HRESULT createResult =
                D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
            if (FAILED(createResult))
                std::cerr << "D3D12CreateDevice failed: 0x" << std::hex << static_cast<unsigned long>(createResult)
                          << std::dec << "\n";
            if (SUCCEEDED(createResult))
            {
                std::wcout << L"Adapter " << devices.size() << L": " << desc.Description << L"\n";
                devices.push_back(device);
            }
        }
        if (devices.empty())
        {
            std::cerr << "A D3D12-capable NVIDIA adapter is required\n";
            return 2;
        }
        auto* available = devices[0].Get();
        DlssNr::MultiGpu::CrossAdapterBuffer invalid;
        using Transport = DlssNr::MultiGpu::CrossAdapterBuffer;
        if (Transport::Create(nullptr, available, 4096, invalid) != E_INVALIDARG ||
            Transport::Create(available, nullptr, 4096, invalid) != E_INVALIDARG ||
            Transport::Create(available, available, 4096, invalid) != E_INVALIDARG ||
            Transport::Create(available, available, 0, invalid) != E_INVALIDARG ||
            Transport::Create(available, available, (std::numeric_limits<uint64_t>::max)(), invalid) != E_INVALIDARG ||
            invalid.PrimaryBuffer() || invalid.SecondaryBuffer() || invalid.Size() != 0)
            throw std::runtime_error("Invalid transport request did not fail cleanly");
        DlssNr::MultiGpu::CrossAdapterTexture invalidTexture;
        D3D12_RESOURCE_DESC invalidImage {};
        if (DlssNr::MultiGpu::CrossAdapterTexture::Create(available, available, invalidImage, invalidTexture) !=
                E_INVALIDARG ||
            invalidTexture.Buffer().PrimaryBuffer() || invalidTexture.Buffer().Size() != 0)
            throw std::runtime_error("Invalid texture request did not fail cleanly");
        std::cout << "Invalid transport requests rejected without publishing resources.\n";
        if (argumentsOnly)
            return 0;
        auto primaryMatch = devices.end();
        if (displayLuid)
            primaryMatch =
                std::find_if(devices.begin(), devices.end(),
                             [&](const ComPtr<ID3D12Device>& device)
                             {
                                 const LUID luid = device->GetAdapterLuid();
                                 return luid.HighPart == displayLuid->HighPart && luid.LowPart == displayLuid->LowPart;
                             });
        if (primaryMatch != devices.end())
            std::iter_swap(devices.begin(), primaryMatch);
        else if (!useWarp)
        {
            std::cerr << "The primary display adapter was not available as a D3D12 device; hardware test not run.\n";
            return 2;
        }
        else
            std::cout << "Primary display device unavailable; WARP fixture uses another available GPU.\n";
        if (useWarp)
        {
            ComPtr<IDXGIAdapter> warp;
            Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "EnumWarpAdapter");
            ComPtr<ID3D12Device> software;
            Check(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&software)), "Create WARP device");
            devices.resize(1);
            devices.push_back(software);
            std::cout << "Secondary adapter: WARP software (not a two-GPU hardware test).\n";
        }
        if (devices.size() != 2)
        {
            std::cerr << "Two D3D12-capable NVIDIA adapters required\n";
            return 2;
        }
        auto* primary = devices[0].Get();
        auto* secondary = devices[1].Get();
        constexpr uint64_t bytes = 4096;
        DlssNr::MultiGpu::CrossAdapterBuffer transfer;
        Check(DlssNr::MultiGpu::CrossAdapterBuffer::Create(primary, secondary, bytes, transfer),
              "Create cross-adapter buffer");
        if (transfer.Size() != bytes)
            throw std::runtime_error("Wrong buffer size");
        Queue primaryQueue(primary), secondaryQueue(secondary);
        auto upload = Buffer(primary, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, bytes);
        auto patch = Buffer(secondary, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, 4);
        auto local = Buffer(secondary, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, bytes);
        auto readback = Buffer(primary, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, bytes);
        uint8_t* data = nullptr;
        D3D12_RANGE noRead { 0, 0 };
        Check(upload->Map(0, &noRead, reinterpret_cast<void**>(&data)), "Map input");
        for (uint64_t i = 0; i < bytes; ++i)
            data[i] = static_cast<uint8_t>((i * 13u + 7u) & 255u);
        upload->Unmap(0, nullptr);
        Check(patch->Map(0, &noRead, reinterpret_cast<void**>(&data)), "Map secondary patch");
        const uint8_t replacement[] = { 0xde, 0xad, 0xbe, 0xef };
        std::memcpy(data, replacement, sizeof(replacement));
        patch->Unmap(0, nullptr);
        auto outbound = primaryQueue.List(primary);
        Transition(outbound.Get(), transfer.PrimaryBuffer(), D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_COPY_DEST);
        outbound->CopyBufferRegion(transfer.PrimaryBuffer(), 0, upload.Get(), 0, bytes);
        Transition(outbound.Get(), transfer.PrimaryBuffer(), D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_COMMON);
        Check(outbound->Close(), "Close primary outbound");
        auto process = secondaryQueue.List(secondary);
        Transition(process.Get(), transfer.SecondaryBuffer(), D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        process->CopyBufferRegion(local.Get(), 0, transfer.SecondaryBuffer(), 0, bytes);
        process->CopyBufferRegion(local.Get(), 0, patch.Get(), 0, sizeof(replacement));
        Transition(process.Get(), local.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(process.Get(), transfer.SecondaryBuffer(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_COPY_DEST);
        process->CopyBufferRegion(transfer.SecondaryBuffer(), 0, local.Get(), 0, bytes);
        Transition(process.Get(), transfer.SecondaryBuffer(), D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_COMMON);
        Transition(process.Get(), local.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        Check(process->Close(), "Close secondary work");
        auto inbound = primaryQueue.List(primary);
        Transition(inbound.Get(), transfer.PrimaryBuffer(), D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        inbound->CopyBufferRegion(readback.Get(), 0, transfer.PrimaryBuffer(), 0, bytes);
        Transition(inbound.Get(), transfer.PrimaryBuffer(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_COMMON);
        Check(inbound->Close(), "Close primary return");
        DlssNr::MultiGpu::CrossAdapterSubmit submission(primaryQueue.queue.Get(), secondaryQueue.queue.Get(), transfer);
        SubmitSplit(submission, outbound.Get(), process.Get(), inbound.Get(), 1);
        ComPtr<ID3D12Fence> finished;
        Check(primary->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&finished)), "Create completion fence");
        Check(primaryQueue.queue->Signal(finished.Get(), 1), "Signal completion");
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event)
            throw std::runtime_error("CreateEvent failed");
        HRESULT hr = finished->SetEventOnCompletion(1, event);
        if (FAILED(hr))
        {
            CloseHandle(event);
            Check(hr, "SetEventOnCompletion");
        }
        DWORD waited = WaitForSingleObject(event, 10000);
        CloseHandle(event);
        if (waited != WAIT_OBJECT_0)
            throw std::runtime_error("GPU round trip timed out");
        D3D12_RANGE range { 0, static_cast<SIZE_T>(bytes) };
        Check(readback->Map(0, &range, reinterpret_cast<void**>(&data)), "Map result");
        for (uint64_t i = 0; i < bytes; ++i)
        {
            const auto expected =
                i < sizeof(replacement) ? replacement[i] : static_cast<uint8_t>((i * 13u + 7u) & 255u);
            if (data[i] != expected)
            {
                readback->Unmap(0, &noRead);
                throw std::runtime_error("Round trip contents differ");
            }
        }
        readback->Unmap(0, &noRead);

        // Reuse the same closed command lists and transport only after the first frame retires.
        Check(upload->Map(0, &noRead, reinterpret_cast<void**>(&data)), "Map second input");
        for (uint64_t i = 0; i < bytes; ++i)
            data[i] = static_cast<uint8_t>((i * 9u + 3u) & 255u);
        upload->Unmap(0, nullptr);
        SubmitSplit(submission, outbound.Get(), process.Get(), inbound.Get(), 2);
        Check(primaryQueue.queue->Signal(finished.Get(), 2), "Signal second completion");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event)
            throw std::runtime_error("CreateEvent failed");
        hr = finished->SetEventOnCompletion(2, event);
        if (FAILED(hr))
        {
            CloseHandle(event);
            Check(hr, "Set second completion event");
        }
        waited = WaitForSingleObject(event, 10000);
        CloseHandle(event);
        if (waited != WAIT_OBJECT_0)
            throw std::runtime_error("Second GPU round trip timed out");
        Check(readback->Map(0, &range, reinterpret_cast<void**>(&data)), "Map second result");
        for (uint64_t i = 0; i < bytes; ++i)
        {
            const auto expected = i < sizeof(replacement) ? replacement[i] : static_cast<uint8_t>((i * 9u + 3u) & 255u);
            if (data[i] != expected)
            {
                readback->Unmap(0, &noRead);
                throw std::runtime_error("Second round trip differs");
            }
        }
        readback->Unmap(0, &noRead);

        TextureRoundTrip(primary, secondary, primaryQueue, secondaryQueue, DXGI_FORMAT_R8G8B8A8_UNORM);
        TextureRoundTrip(primary, secondary, primaryQueue, secondaryQueue, DXGI_FORMAT_R16G16B16A16_FLOAT);
        TextureRoundTrip(primary, secondary, primaryQueue, secondaryQueue, DXGI_FORMAT_R32_FLOAT);
        TextureRoundTrip(primary, secondary, primaryQueue, secondaryQueue, DXGI_FORMAT_R16G16_FLOAT);
        for (const auto& device : devices)
        {
            ComPtr<ID3D12InfoQueue> messages;
            if (FAILED(device.As(&messages)))
                continue;
            const auto count = messages->GetNumStoredMessagesAllowedByRetrievalFilter();
            for (UINT64 index = 0; index < count; ++index)
            {
                SIZE_T length = 0;
                Check(messages->GetMessage(index, nullptr, &length), "Query debug message");
                std::vector<uint8_t> storage(length);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                Check(messages->GetMessage(index, message, &length), "Get debug message");
                if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
                    message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)
                    throw std::runtime_error(std::string("D3D12 debug layer: ") + message->pDescription);
                if (message->Severity == D3D12_MESSAGE_SEVERITY_WARNING)
                    std::cerr << "D3D12 debug warning: " << message->pDescription << "\n";
            }
        }
        std::cout << "Cross-adapter GPU round trip passed: 4096 bytes, one outbound and one return.\n";
        return 0;
    }
    catch (const std::exception& ex)
    {
        std::cerr << ex.what() << "\n";
        return 1;
    }
}
