#pragma once

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>

namespace DlssNr::Worker
{
// Copies an owned completed picture to a presentation texture on one real
// primary queue. Callers serialize access and keep the source immutable until
// the returned fence completes. Neither side may be a live neural output.
// ```cpp
// auto shown = presenter.Submit(completedPicture, backbuffer);
// if (shown.result == E_PENDING) KeepTheCurrentPicture();
// ```
class FinishedPicturePresenter
{
    using Resource = Microsoft::WRL::ComPtr<ID3D12Resource>;
    struct Slot
    {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commands;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        Resource source, target;
        uint64_t value = 0;
        bool submitted = false, untracked = false;
    };
    struct Impl
    {
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        std::array<Slot, 3> slots;
        uint64_t nextValue = 1;
        size_t nextSlot = 0;
        HRESULT failure = S_OK;
        bool Pending() const
        {
            for (const auto& slot : slots)
                if (slot.submitted) return true;
            return false;
        }
    };
    std::unique_ptr<Impl> impl_;

    static bool SameIdentity(IUnknown* a, IUnknown* b)
    {
        Microsoft::WRL::ComPtr<IUnknown> left, right;
        return a && b && SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&left))) &&
               SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&right))) && left.Get() == right.Get();
    }
    static bool OwnedBy(ID3D12DeviceChild* child, ID3D12Device* device)
    {
        Microsoft::WRL::ComPtr<ID3D12Device> owner;
        return child && SUCCEEDED(child->GetDevice(IID_PPV_ARGS(&owner))) && SameIdentity(owner.Get(), device);
    }
    static bool Compatible(const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b)
    {
        return a.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && b.Dimension == a.Dimension &&
               a.Width && a.Height && a.Width == b.Width && a.Height == b.Height && a.Format == b.Format &&
               a.MipLevels == 1 && b.MipLevels == 1 && a.DepthOrArraySize == 1 && b.DepthOrArraySize == 1 &&
               a.SampleDesc.Count == 1 && b.SampleDesc.Count == 1 && !a.SampleDesc.Quality && !b.SampleDesc.Quality;
    }
    static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                              D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        return barrier;
    }

  public:
    struct Submission
    {
        HRESULT result = E_FAIL;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        uint64_t value = 0;
    };
    FinishedPicturePresenter() = default;
    FinishedPicturePresenter(const FinishedPicturePresenter&) = delete;
    FinishedPicturePresenter& operator=(const FinishedPicturePresenter&) = delete;
    ~FinishedPicturePresenter()
    {
        if (!impl_) return;
        Collect();
        // No destructor wait and no destruction of possibly live GPU objects.
        // An untracked/unfinished submission retains this complete allocation
        // until OS process teardown. Successful callers Drain before teardown.
        if (impl_->Pending()) (void) impl_.release();
    }

    // Configure one direct queue, before any submission.
    // ```cpp
    // Check(presenter.Initialize(device, realPrimaryQueue));
    // ```
    HRESULT Initialize(ID3D12Device* device, ID3D12CommandQueue* realQueue)
    {
        if (impl_) return E_UNEXPECTED;
        if (!device || !realQueue || realQueue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
            !OwnedBy(realQueue, device)) return E_INVALIDARG;
        auto next = std::make_unique<Impl>();
        next->device = device;
        next->queue = realQueue;
        HRESULT hr = device->GetDeviceRemovedReason();
        if (FAILED(hr)) return hr;
        for (auto& slot : next->slots)
        {
            hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator));
            if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                slot.allocator.Get(), nullptr, IID_PPV_ARGS(&slot.commands));
            if (SUCCEEDED(hr)) hr = slot.commands->Close();
            if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&slot.fence));
            if (FAILED(hr)) return hr;
        }
        impl_ = std::move(next);
        return S_OK;
    }

    // Queue identity is exact, even when another queue uses the same adapter.
    // ```cpp
    // if (!presenter.UsesQueue(realQueue)) RecreateAfterDraining();
    // ```
    bool UsesQueue(ID3D12CommandQueue* realQueue) const
    {
        return impl_ && SameIdentity(impl_->queue.Get(), realQueue);
    }

    // Release resources only after their own queue completion marker is seen.
    // ```cpp
    // presenter.Collect();
    // ```
    HRESULT Collect()
    {
        if (!impl_) return E_UNEXPECTED;
        for (auto& slot : impl_->slots)
        {
            if (!slot.submitted || slot.untracked) continue;
            const auto completed = slot.fence->GetCompletedValue();
            if (completed == (std::numeric_limits<uint64_t>::max)())
                impl_->failure = DXGI_ERROR_DEVICE_REMOVED;
            else if (completed >= slot.value)
            {
                slot.source.Reset();
                slot.target.Reset();
                slot.submitted = false;
            }
        }
        return impl_->failure;
    }

    // Both textures arrive and leave in PRESENT/COMMON. No CPU stall is added
    // to Submit; a full ring returns E_PENDING before recording any commands.
    // ```cpp
    // auto copy = presenter.Submit(completeSource, backbufferTarget);
    // if (copy.result == S_OK) RetainSourceUntil(copy.fence, copy.value);
    // ```
    Submission Submit(ID3D12Resource* completeSource, ID3D12Resource* backbufferTarget)
    {
        Submission out;
        if (!impl_) { out.result = E_UNEXPECTED; return out; }
        if (!completeSource || !backbufferTarget || SameIdentity(completeSource, backbufferTarget) ||
            !OwnedBy(completeSource, impl_->device.Get()) || !OwnedBy(backbufferTarget, impl_->device.Get()) ||
            !Compatible(completeSource->GetDesc(), backbufferTarget->GetDesc()))
        { out.result = E_INVALIDARG; return out; }
        out.result = Collect();
        if (FAILED(out.result)) return out;
        out.result = impl_->device->GetDeviceRemovedReason();
        if (FAILED(out.result)) { impl_->failure = out.result; return out; }
        Slot* chosen = nullptr;
        for (size_t i = 0; i < impl_->slots.size(); ++i)
        {
            const auto index = (impl_->nextSlot + i) % impl_->slots.size();
            if (!impl_->slots[index].submitted)
            { chosen = &impl_->slots[index]; impl_->nextSlot = (index + 1) % impl_->slots.size(); break; }
        }
        if (!chosen) { out.result = E_PENDING; return out; }
        if (impl_->nextValue == (std::numeric_limits<uint64_t>::max)())
        { out.result = HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW); return out; }
        auto& slot = *chosen;
        HRESULT hr = slot.allocator->Reset();
        if (SUCCEEDED(hr)) hr = slot.commands->Reset(slot.allocator.Get(), nullptr);
        if (FAILED(hr)) { impl_->failure = out.result = hr; return out; }
        D3D12_RESOURCE_BARRIER before[] = {
            Transition(completeSource, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE),
            Transition(backbufferTarget, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST) };
        slot.commands->ResourceBarrier(2, before);
        slot.commands->CopyResource(backbufferTarget, completeSource);
        D3D12_RESOURCE_BARRIER after[] = {
            Transition(completeSource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT),
            Transition(backbufferTarget, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT) };
        slot.commands->ResourceBarrier(2, after);
        hr = slot.commands->Close();
        if (FAILED(hr)) { impl_->failure = out.result = hr; return out; }
        slot.source = completeSource;
        // The back buffer is not held: its swap chain keeps it alive while the game presents, and a reference kept
        // here until the next Submit stops the game from destroying that swap chain when it recreates it (enabling
        // frame generation does): the new swap chain for the same window then fails with E_ACCESSDENIED.
        slot.target.Reset();
        slot.value = impl_->nextValue++;
        slot.submitted = true;
        ID3D12CommandList* lists[] = { slot.commands.Get() };
        impl_->queue->ExecuteCommandLists(1, lists);
        hr = impl_->queue->Signal(slot.fence.Get(), slot.value);
        if (FAILED(hr)) slot.untracked = true;
        if (SUCCEEDED(hr)) hr = impl_->device->GetDeviceRemovedReason();
        if (FAILED(hr)) { impl_->failure = out.result = hr; return out; }
        out.result = S_OK;
        out.fence = slot.fence;
        out.value = slot.value;
        return out;
    }

    // Resize/teardown only: bounded polling avoids registering an event that
    // could remain pending after a timeout. A timeout keeps all live references.
    // ```cpp
    // if (presenter.Drain(5000) != S_OK) RetainForProcessTeardown();
    // ```
    HRESULT Drain(DWORD timeoutMs)
    {
        if (!impl_) return S_OK;
        if (timeoutMs == INFINITE) return E_INVALIDARG;
        const auto began = GetTickCount64();
        for (;;)
        {
            const HRESULT hr = Collect();
            if (FAILED(hr)) return hr;
            if (!impl_->Pending()) return S_OK;
            if (!timeoutMs) return E_PENDING;
            if (GetTickCount64() - began >= timeoutMs) return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
            Sleep(1);
        }
    }
};
} // namespace DlssNr::Worker
