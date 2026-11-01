#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace NrFixture
{
using Evaluate = int (*)(ID3D12GraphicsCommandList*, void*, void*, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*,
                         ID3D12Resource*, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
                         unsigned, unsigned, unsigned, int, int, float, int, float, float, float, int, float, float);
using Error = const char* (*) ();
using Microsoft::WRL::ComPtr;

struct Surface
{
    ComPtr<ID3D12Resource> texture;
    ComPtr<ID3D12Resource> upload;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT rows = 0;
    UINT64 rowBytes = 0;
    UINT64 bytes = 0;
};

inline bool MakeSurface(ID3D12Device* device, DXGI_FORMAT format, bool output, Surface& surface)
{
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 256;
    desc.Height = 144;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = output ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, IID_PPV_ARGS(&surface.texture))))
        return false;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &surface.footprint, &surface.rows, &surface.rowBytes, &surface.bytes);
    if (!surface.bytes || surface.bytes == UINT64_MAX)
        return false;
    D3D12_RESOURCE_DESC buffer {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = surface.bytes;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&surface.upload)));
}

inline D3D12_TEXTURE_COPY_LOCATION Texture(ID3D12Resource* resource)
{
    D3D12_TEXTURE_COPY_LOCATION loc {};
    loc.pResource = resource;
    loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    return loc;
}
inline D3D12_TEXTURE_COPY_LOCATION Buffer(ID3D12Resource* resource, D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint)
{
    D3D12_TEXTURE_COPY_LOCATION loc {};
    loc.pResource = resource;
    loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    loc.PlacedFootprint = footprint;
    return loc;
}
inline void Barrier(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                    D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    commands->ResourceBarrier(1, &barrier);
}

// Evaluates one already-initialized feature on synthetic inputs and reads its output back. A
// successful result establishes only that this model ran on this adapter, not visual quality or game integration.
inline bool ProbeEvaluate(ID3D12Device* device, ID3D12CommandQueue* queue, void* feature, void* parameters,
                          Evaluate evaluate, Error error)
{
    Surface color, depth, motion, output;
    if (!MakeSurface(device, DXGI_FORMAT_R16G16B16A16_FLOAT, false, color) ||
        !MakeSurface(device, DXGI_FORMAT_R32_FLOAT, false, depth) ||
        !MakeSurface(device, DXGI_FORMAT_R16G16_FLOAT, false, motion) ||
        !MakeSurface(device, DXGI_FORMAT_R16G16B16A16_FLOAT, true, output))
    {
        std::cerr << "Could not allocate synthetic model textures.\n";
        return false;
    }
    Surface* surfaces[] = { &color, &depth, &motion, &output };
    D3D12_RANGE noRead { 0, 0 };
    for (unsigned i = 0; i < 4; ++i)
    {
        uint8_t* mapped = nullptr;
        if (FAILED(surfaces[i]->upload->Map(0, &noRead, reinterpret_cast<void**>(&mapped))))
            return false;
        std::memset(mapped, 0, static_cast<size_t>(surfaces[i]->bytes));
        for (UINT y = 0; y < surfaces[i]->rows; ++y)
        {
            auto* row = mapped + y * surfaces[i]->footprint.Footprint.RowPitch;
            if (i == 0)
            {
                auto* half = reinterpret_cast<uint16_t*>(row);
                for (unsigned x = 0; x < 256; ++x)
                {
                    half[4 * x] = half[4 * x + 1] = half[4 * x + 2] = 0x3800; // 0.5
                    half[4 * x + 3] = 0x3c00;                                 // 1.0
                }
            }
            else if (i == 1)
            {
                auto* floats = reinterpret_cast<float*>(row);
                for (unsigned x = 0; x < 256; ++x)
                    floats[x] = 0.5f;
            }
        }
        surfaces[i]->upload->Unmap(0, nullptr);
    }
    D3D12_RESOURCE_DESC readbackDesc {};
    readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readbackDesc.Width = output.bytes;
    readbackDesc.Height = readbackDesc.DepthOrArraySize = readbackDesc.MipLevels = 1;
    readbackDesc.SampleDesc.Count = 1;
    readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES readbackHeap {};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    readbackHeap.CreationNodeMask = readbackHeap.VisibleNodeMask = 1;
    ComPtr<ID3D12Resource> readback;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&commands))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        return false;
    for (unsigned i = 0; i < 4; ++i)
    {
        auto dst = Texture(surfaces[i]->texture.Get());
        auto src = Buffer(surfaces[i]->upload.Get(), surfaces[i]->footprint);
        commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Barrier(commands.Get(), surfaces[i]->texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                i == 3 ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    std::cerr << "NGX probe: evaluating one synthetic frame\n" << std::flush;
    const int result = evaluate(commands.Get(), feature, parameters, color.texture.Get(), depth.texture.Get(),
                                motion.texture.Get(), output.texture.Get(), 256, 144, 256, 144, 256, 144, 0, 0, 0, 0, 0,
                                1, 1.0f, 0, 0.0f, 0.0f, 0.0f, 1, 1.0f, 1.0f);
    std::cerr << "NGX probe: evaluate call returned 0x" << std::hex << static_cast<unsigned>(result) << "\n"
              << std::flush;
    if (result != 1)
    {
        std::cerr << "Neural evaluation failed: 0x" << std::hex << static_cast<unsigned>(result) << " "
                  << (error ? error() : "") << "\n";
        return false;
    }
    Barrier(commands.Get(), output.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    auto dst = Buffer(readback.Get(), output.footprint);
    auto src = Texture(output.texture.Get());
    commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    if (FAILED(commands->Close()))
        return false;
    ID3D12CommandList* lists[] = { commands.Get() };
    queue->ExecuteCommandLists(1, lists);
    std::cerr << "NGX probe: evaluation submitted\n" << std::flush;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event || FAILED(queue->Signal(fence.Get(), 1)) || FAILED(fence->SetEventOnCompletion(1, event)))
    {
        if (event)
            CloseHandle(event);
        std::cerr << "Submitted evaluation could not be tracked; terminating probe.\n" << std::flush;
        ExitProcess(3);
    }
    const DWORD wait = WaitForSingleObject(event, 30000);
    CloseHandle(event);
    if (wait != WAIT_OBJECT_0 || FAILED(device->GetDeviceRemovedReason()))
    {
        std::cerr << "Evaluation did not retire cleanly; terminating probe.\n" << std::flush;
        ExitProcess(3);
    }
    std::cerr << "NGX probe: evaluation GPU work retired\n" << std::flush;
    D3D12_RANGE readRange { 0, static_cast<SIZE_T>(output.bytes) };
    uint8_t* mapped = nullptr;
    std::cerr << "NGX probe: mapping output readback\n" << std::flush;
    if (FAILED(readback->Map(0, &readRange, reinterpret_cast<void**>(&mapped))))
        return false;
    std::cerr << "NGX probe: scanning output readback rows=" << output.rows << " bytes=" << output.bytes
              << " pitch=" << output.footprint.Footprint.RowPitch << "\n"
              << std::flush;
    bool finite = true, nonzero = false;
    for (UINT y = 0; y < output.rows; ++y)
    {
        auto* row = reinterpret_cast<const uint16_t*>(mapped + y * output.footprint.Footprint.RowPitch);
        for (unsigned x = 0; x < 256 * 3; ++x)
        {
            // Skip alpha; inspect the first three channels of each pixel.
            const unsigned pixel = x / 3, channel = x % 3;
            const uint16_t value = row[pixel * 4 + channel];
            finite = finite && (value & 0x7c00u) != 0x7c00u;
            nonzero = nonzero || (value & 0x7fffu) != 0;
        }
    }
    std::cerr << "NGX probe: output scan complete\n" << std::flush;
    readback->Unmap(0, &noRead);
    std::cerr << "NGX probe: output unmapped\n" << std::flush;
    if (!finite || !nonzero)
    {
        std::cerr << "Model output contained non-finite values or no RGB output.\n";
        return false;
    }
    std::cout << "Neural evaluation retired with finite, nonzero RGB output on synthetic input.\n" << std::flush;
    std::cerr << "NGX probe: evaluation fixture returning\n" << std::flush;
    return true;
}
} // namespace NrFixture
