#pragma once

#include "DlssNr_CrossAdapterBuffer.h"

namespace DlssNr::MultiGpu
{
// Locally owned, unnamed NT handles. They must be duplicated into the intended worker process;
// never publish numeric handles or rely on broad handle inheritance. Destruction closes local handles.
class CrossAdapterHandles
{
    HANDLE heap_ = nullptr, ready_ = nullptr, done_ = nullptr;

  public:
    CrossAdapterHandles() = default;
    CrossAdapterHandles(const CrossAdapterHandles&) = delete;
    CrossAdapterHandles& operator=(const CrossAdapterHandles&) = delete;
    CrossAdapterHandles(CrossAdapterHandles&& other) noexcept { *this = std::move(other); }
    CrossAdapterHandles& operator=(CrossAdapterHandles&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            heap_ = std::exchange(other.heap_, nullptr);
            ready_ = std::exchange(other.ready_, nullptr);
            done_ = std::exchange(other.done_, nullptr);
        }
        return *this;
    }
    ~CrossAdapterHandles() { Reset(); }
    HANDLE Heap() const { return heap_; }
    HANDLE Ready() const { return ready_; }
    HANDLE Done() const { return done_; }

    // Export handles for the existing primary heap and directional fences. `out` stays unchanged
    // if any operation fails. Resources and GPU queues must outlive all uses in both processes.
    static HRESULT Create(const CrossAdapterBuffer& buffer, CrossAdapterHandles& out)
    {
        if (!buffer.PrimaryHeap() || !buffer.PrimaryBuffer() || !buffer.PrimaryReady() || !buffer.PrimaryDone())
            return E_INVALIDARG;
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        HRESULT hr = buffer.PrimaryBuffer()->GetDevice(IID_PPV_ARGS(&device));
        if (FAILED(hr))
            return hr;
        CrossAdapterHandles result;
        hr = device->CreateSharedHandle(buffer.PrimaryHeap(), nullptr, GENERIC_ALL, nullptr, &result.heap_);
        if (FAILED(hr))
            return hr;
        hr = device->CreateSharedHandle(buffer.PrimaryReady(), nullptr, GENERIC_ALL, nullptr, &result.ready_);
        if (FAILED(hr))
            return hr;
        hr = device->CreateSharedHandle(buffer.PrimaryDone(), nullptr, GENERIC_ALL, nullptr, &result.done_);
        if (FAILED(hr))
            return hr;
        out = std::move(result);
        return S_OK;
    }

  private:
    void Reset()
    {
        if (heap_)
            CloseHandle(heap_);
        if (ready_)
            CloseHandle(ready_);
        if (done_)
            CloseHandle(done_);
        heap_ = ready_ = done_ = nullptr;
    }
};

// A worker-side view of a duplicated heap and two directional fences. Opening a handle does not
// transfer ownership of that OS handle; the worker closes its duplicates after this call.
class CrossAdapterPeer
{
    Microsoft::WRL::ComPtr<ID3D12Heap> heap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer_;
    Microsoft::WRL::ComPtr<ID3D12Fence> ready_, done_;

  public:
    static HRESULT Open(ID3D12Device* device, LUID primary, LUID expectedSecondary, uint64_t bytes, HANDLE heap,
                        HANDLE ready, HANDLE done, CrossAdapterPeer& out)
    {
        if (!device || !bytes || !heap || !ready || !done)
            return E_INVALIDARG;
        const LUID actual = device->GetAdapterLuid();
        if ((actual.HighPart == primary.HighPart && actual.LowPart == primary.LowPart) ||
            actual.HighPart != expectedSecondary.HighPart || actual.LowPart != expectedSecondary.LowPart)
            return E_INVALIDARG;
        CrossAdapterPeer result;
        HRESULT hr = device->OpenSharedHandle(heap, IID_PPV_ARGS(&result.heap_));
        if (FAILED(hr))
            return hr;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
        hr = device->CreatePlacedResource(result.heap_.Get(), 0, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                          IID_PPV_ARGS(&result.buffer_));
        if (FAILED(hr))
            return hr;
        hr = device->OpenSharedHandle(ready, IID_PPV_ARGS(&result.ready_));
        if (FAILED(hr))
            return hr;
        hr = device->OpenSharedHandle(done, IID_PPV_ARGS(&result.done_));
        if (FAILED(hr))
            return hr;
        out = std::move(result);
        return S_OK;
    }
    ID3D12Resource* Buffer() const { return buffer_.Get(); }
    ID3D12Fence* Ready() const { return ready_.Get(); }
    ID3D12Fence* Done() const { return done_.Get(); }
};
} // namespace DlssNr::MultiGpu
