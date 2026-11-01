#pragma once
#include "WorkerModelSession.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace DlssNr::Worker
{
using Microsoft::WRL::ComPtr;
struct Surface
{
    ComPtr<ID3D12Resource> texture, upload;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT rows = 0;
    UINT64 rowBytes = 0, bytes = 0;
};
inline Surface MakeSurface(ID3D12Device* device, unsigned width, unsigned height, DXGI_FORMAT format, bool output)
{
    Surface surface;
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = output ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(&surface.texture)),
          "Create worker texture");
    device->GetCopyableFootprints(&desc, 0, 1, 0, &surface.footprint, &surface.rows, &surface.rowBytes, &surface.bytes);
    if (!surface.bytes || surface.bytes == UINT64_MAX)
        throw std::runtime_error("Invalid worker footprint");
    D3D12_RESOURCE_DESC buffer {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = surface.bytes;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ,
                                          nullptr, IID_PPV_ARGS(&surface.upload)),
          "Create worker upload");
    return surface;
}
inline void FillZero(Surface& surface)
{
    D3D12_RANGE noRead { 0, 0 };
    void* mapped = nullptr;
    Check(surface.upload->Map(0, &noRead, &mapped), "Map worker upload");
    std::memset(mapped, 0, static_cast<size_t>(surface.bytes));
    surface.upload->Unmap(0, nullptr);
}
inline D3D12_TEXTURE_COPY_LOCATION Texture(ID3D12Resource* resource)
{
    D3D12_TEXTURE_COPY_LOCATION loc {};
    loc.pResource = resource;
    loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    return loc;
}
inline D3D12_TEXTURE_COPY_LOCATION BufferLocation(ID3D12Resource* resource,
                                                  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint)
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
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    commands->ResourceBarrier(1, &b);
}
inline void CopyUpload(ID3D12GraphicsCommandList* commands, Surface& surface)
{
    auto dst = Texture(surface.texture.Get());
    auto src = BufferLocation(surface.upload.Get(), surface.footprint);
    commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(commands, surface.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}
inline D3D12_TEXTURE_COPY_LOCATION SharedLocation(ID3D12Resource* buffer, const Surface& surface, UINT64 offset)
{
    auto loc = BufferLocation(buffer, surface.footprint);
    loc.PlacedFootprint.Offset = offset;
    return loc;
}
inline void CopyFromShared(ID3D12GraphicsCommandList* commands, ID3D12Resource* shared, Surface& surface, UINT64 offset)
{
    auto src = SharedLocation(shared, surface, offset);
    auto dst = Texture(surface.texture.Get());
    commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(commands, surface.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}
inline void CopyToShared(ID3D12GraphicsCommandList* commands, ID3D12Resource* shared, Surface& surface, UINT64 offset)
{
    auto src = Texture(surface.texture.Get());
    auto dst = SharedLocation(shared, surface, offset);
    commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
}
} // namespace DlssNr::Worker
