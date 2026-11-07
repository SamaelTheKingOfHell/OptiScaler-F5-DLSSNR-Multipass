#pragma once
// GPU timestamps for one pass per frame, in a ring of slots, read a few frames later (by then the GPU has finished the
// slot: a swap chain keeps at most three frames in flight). Up to six marks per slot; marks of one slot may sit on
// different command lists of the same queue (the network split records its two halves on two lists).
//
// ```cpp
// timer.Mark(list, slot, 0); evaluate(...); timer.Mark(list, slot, 1);
// timer.Resolve(list, slot);        // on the list with the slot's last mark
// double ms; if (timer.Elapsed(oldSlot, 0, 1, ms)) ...
// ```

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstring>

namespace DlssNr
{
class PassTimer
{
  public:
    static constexpr unsigned Slots = 16, Marks = 6;

    bool Ready() const { return heap_ != nullptr; }
    HRESULT Create(ID3D12Device* device, ID3D12CommandQueue* queue)
    {
        if (heap_)
            return S_OK;
        D3D12_QUERY_HEAP_DESC qh {};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = Slots * Marks;
        HRESULT hr = device->CreateQueryHeap(&qh, IID_PPV_ARGS(&heap_));
        if (FAILED(hr))
            return hr;
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC d {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = Slots * Marks * 8;
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
                                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                        IID_PPV_ARGS(&readback_))) ||
            FAILED(hr = queue->GetTimestampFrequency(&frequency_)))
        {
            heap_.Reset();
            return hr;
        }
        return S_OK;
    }
    void Mark(ID3D12GraphicsCommandList* list, unsigned slot, unsigned mark)
    {
        if (heap_ && list)
            list->EndQuery(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, Index(slot, mark));
    }
    void Resolve(ID3D12GraphicsCommandList* list, unsigned slot)
    {
        if (heap_ && list)
            list->ResolveQueryData(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, Index(slot, 0), Marks,
                                   readback_.Get(), uint64_t(Index(slot, 0)) * 8);
    }
    // Milliseconds from mark `from` to mark `to` of a finished slot; false when the marks are missing or out of order.
    bool Elapsed(unsigned slot, unsigned from, unsigned to, double& ms)
    {
        if (!heap_ || !frequency_)
            return false;
        uint64_t* t = nullptr;
        const D3D12_RANGE range { Index(slot, 0) * 8, (Index(slot, 0) + Marks) * 8 };
        if (FAILED(readback_->Map(0, &range, reinterpret_cast<void**>(&t))))
            return false;
        const uint64_t a = t[Index(slot, from)], b = t[Index(slot, to)];
        const D3D12_RANGE none { 0, 0 };
        readback_->Unmap(0, &none);
        if (!a || b <= a)
            return false;
        ms = double(b - a) * 1000.0 / double(frequency_);
        return ms < 1000.0;
    }

  private:
    static unsigned Index(unsigned slot, unsigned mark) { return (slot % Slots) * Marks + mark; }
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback_;
    uint64_t frequency_ = 0;
};
} // namespace DlssNr
