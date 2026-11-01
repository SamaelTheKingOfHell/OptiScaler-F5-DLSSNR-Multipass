#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <limits>
#include <utility>

namespace DlssNr::MultiGpu
{
// A shared, GPU-copyable buffer and two directional fences. Owners must keep this object alive
// until all submitted uses have retired and serialize reuse themselves. This is transport only;
// it does not submit commands or make an unsubmitted game command list visible to another queue.
class CrossAdapterBuffer
{
    using Resource = Microsoft::WRL::ComPtr<ID3D12Resource>;
    using Heap = Microsoft::WRL::ComPtr<ID3D12Heap>;
    using Fence = Microsoft::WRL::ComPtr<ID3D12Fence>;
    Heap primaryHeap_, secondaryHeap_;
    Resource primaryBuffer_, secondaryBuffer_;
    Fence primaryReady_, secondaryReady_, secondaryDone_, primaryDone_;
    uint64_t size_ = 0;

    static HRESULT ShareFence(ID3D12Device* owner, ID3D12Device* other, Fence& owned, Fence& opened)
    {
        auto flags = D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER;
        HRESULT result = owner->CreateFence(0, flags, IID_PPV_ARGS(&owned));
        if (FAILED(result))
            return result;
        HANDLE handle = nullptr;
        result = owner->CreateSharedHandle(owned.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
        if (FAILED(result))
            return result;
        result = other->OpenSharedHandle(handle, IID_PPV_ARGS(&opened));
        CloseHandle(handle);
        return result;
    }

  public:
    // Allocate on the primary, then open equivalent placed buffers and directional fences on
    // the secondary. On failure, `out` is unchanged. Byte counts are rounded up to heap alignment.
    static HRESULT Create(ID3D12Device* primary, ID3D12Device* secondary, uint64_t bytes, CrossAdapterBuffer& out)
    {
        constexpr uint64_t alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        if (!primary || !secondary || bytes == 0 || bytes > (std::numeric_limits<uint64_t>::max)() - (alignment - 1))
            return E_INVALIDARG;
        const LUID a = primary->GetAdapterLuid(), b = secondary->GetAdapterLuid();
        if (a.LowPart == b.LowPart && a.HighPart == b.HighPart)
            return E_INVALIDARG;
        CrossAdapterBuffer result;
        result.size_ = bytes;
        D3D12_HEAP_DESC heap {};
        heap.SizeInBytes = (bytes + alignment - 1) & ~(alignment - 1);
        heap.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.Properties.CreationNodeMask = 1;
        heap.Properties.VisibleNodeMask = 1;
        heap.Alignment = alignment;
        heap.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
        HRESULT hr = primary->CreateHeap(&heap, IID_PPV_ARGS(&result.primaryHeap_));
        if (FAILED(hr))
            return hr;
        HANDLE handle = nullptr;
        hr = primary->CreateSharedHandle(result.primaryHeap_.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
        if (FAILED(hr))
            return hr;
        hr = secondary->OpenSharedHandle(handle, IID_PPV_ARGS(&result.secondaryHeap_));
        CloseHandle(handle);
        if (FAILED(hr))
            return hr;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
        hr = primary->CreatePlacedResource(result.primaryHeap_.Get(), 0, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                           IID_PPV_ARGS(&result.primaryBuffer_));
        if (FAILED(hr))
            return hr;
        hr = secondary->CreatePlacedResource(result.secondaryHeap_.Get(), 0, &desc, D3D12_RESOURCE_STATE_COMMON,
                                             nullptr, IID_PPV_ARGS(&result.secondaryBuffer_));
        if (FAILED(hr))
            return hr;
        hr = ShareFence(primary, secondary, result.primaryReady_, result.secondaryReady_);
        if (FAILED(hr))
            return hr;
        hr = ShareFence(secondary, primary, result.secondaryDone_, result.primaryDone_);
        if (FAILED(hr))
            return hr;
        out = std::move(result);
        return S_OK;
    }

    uint64_t Size() const { return size_; }
    ID3D12Resource* PrimaryBuffer() const { return primaryBuffer_.Get(); }
    ID3D12Resource* SecondaryBuffer() const { return secondaryBuffer_.Get(); }
    ID3D12Fence* PrimaryReady() const { return primaryReady_.Get(); }
    ID3D12Fence* SecondaryReady() const { return secondaryReady_.Get(); }
    ID3D12Fence* SecondaryDone() const { return secondaryDone_.Get(); }
    ID3D12Fence* PrimaryDone() const { return primaryDone_.Get(); }
};
} // namespace DlssNr::MultiGpu
