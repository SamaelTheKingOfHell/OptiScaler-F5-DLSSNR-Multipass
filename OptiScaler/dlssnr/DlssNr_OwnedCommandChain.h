#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <new>
#include <utility>
#include <vector>

namespace DlssNr::MultiGpu
{
// A chain of command lists owned by the caller, never an arbitrary game command list. The caller
// keeps this object AND the initial allocator alive until an independently tracked queue fence has
// retired all submitted work. No GPU wait is inserted here. Caller serializes all access.
class OwnedCommandChain
{
    using Device = Microsoft::WRL::ComPtr<ID3D12Device>;
    using Queue = Microsoft::WRL::ComPtr<ID3D12CommandQueue>;
    using List = Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>;
    using Allocator = Microsoft::WRL::ComPtr<ID3D12CommandAllocator>;
    struct Continuation
    {
        Allocator allocator;
        List list;
    };
    Device device_;
    Queue queue_;
    List first_;
    std::vector<Continuation> continuations_;
    bool closed_ = false;
    bool poisoned_ = false;
    unsigned submitted_ = 0;

    static bool SameDevice(ID3D12DeviceChild* child, ID3D12Device* expected)
    {
        Device owner;
        if (!child || FAILED(child->GetDevice(IID_PPV_ARGS(&owner))) || !owner || !expected)
            return false;
        Microsoft::WRL::ComPtr<IUnknown> a, b;
        return SUCCEEDED(owner.As(&a)) && SUCCEEDED(expected->QueryInterface(IID_PPV_ARGS(&b))) && a.Get() == b.Get();
    }

  public:
    OwnedCommandChain(const OwnedCommandChain&) = delete;
    OwnedCommandChain& operator=(const OwnedCommandChain&) = delete;
    OwnedCommandChain() = default;

    HRESULT Initialize(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* first)
    {
        if (device_ || !device || !queue || !first || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
            first->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT || !SameDevice(queue, device) ||
            !SameDevice(first, device))
            return E_INVALIDARG;
        const HRESULT status = device->GetDeviceRemovedReason();
        if (FAILED(status))
            return status;
        device_ = device;
        queue_ = queue;
        first_ = first;
        return S_OK;
    }
    ID3D12GraphicsCommandList* Current() const
    {
        if (!device_ || closed_ || poisoned_)
            return nullptr;
        return continuations_.empty() ? first_.Get() : continuations_.back().list.Get();
    }
    unsigned SubmittedCount() const { return submitted_; }
    bool Poisoned() const { return poisoned_; }

    // Allocate the next list first. If preparation fails the current list remains open. Once closed,
    // a failure poisons this chain; submitted resources must still be retired by the caller.
    HRESULT Split()
    {
        if (!Current())
            return E_ABORT;
        Continuation next;
        HRESULT hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&next.allocator));
        if (FAILED(hr))
            return hr;
        hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, next.allocator.Get(), nullptr,
                                        IID_PPV_ARGS(&next.list));
        if (FAILED(hr))
            return hr;
        ID3D12GraphicsCommandList* current = Current();
        try
        {
            continuations_.push_back(std::move(next));
        }
        catch (const std::bad_alloc&)
        {
            return E_OUTOFMEMORY;
        }
        hr = current->Close();
        if (FAILED(hr))
        {
            poisoned_ = true;
            return hr;
        }
        ID3D12CommandList* lists[] = { current };
        queue_->ExecuteCommandLists(1, lists);
        ++submitted_;
        hr = device_->GetDeviceRemovedReason();
        if (FAILED(hr))
            poisoned_ = true;
        return hr;
    }

    // Closes and submits the last list. The caller still must signal and observe its own retirement fence.
    HRESULT SubmitFinal()
    {
        ID3D12GraphicsCommandList* current = Current();
        if (!current)
            return E_ABORT;
        HRESULT hr = current->Close();
        if (FAILED(hr))
        {
            poisoned_ = true;
            return hr;
        }
        closed_ = true;
        ID3D12CommandList* lists[] = { current };
        queue_->ExecuteCommandLists(1, lists);
        ++submitted_;
        hr = device_->GetDeviceRemovedReason();
        if (FAILED(hr))
            poisoned_ = true;
        return hr;
    }
};
} // namespace DlssNr::MultiGpu
