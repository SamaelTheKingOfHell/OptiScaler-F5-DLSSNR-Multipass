#pragma once

// The D3D12 adapter's frame source: zero-copy. The picture is the swap chain's back buffer (it rests in PRESENT) and the depth
// is the finder's slot copies (resource_tracking/GenericDepth_Dx12.cpp is the depth observer of the same adapter). Return() has
// nothing to copy back: the producer processed the back buffer in place on the swap chain's own queue.

#include "FrameContract.h"

namespace native
{

class Dx12FrameSource : public IFrameSource
{
  public:
    ~Dx12FrameSource() override { Release(); }

    // The present being processed: the swap chain whose back buffer is the picture, and its queue and device. Call
    // before Acquire().
    void SetPresent(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue, ID3D12Device* device);
    ID3D12Device* Device() const override { return _device; }
    ID3D12CommandQueue* Queue() const override { return _queue; }

    Api GetApi() const override { return Api::D3D12; }
    AcquireStatus Acquire(FrameInput& input) override;
    void Return(const FrameInput& input, const FrameOutput& output) override;
    void OnResize() override {}
    void Skip() override { Release(); }

  private:
    void Release();

    IDXGISwapChain* _swapChain = nullptr;
    ID3D12CommandQueue* _queue = nullptr;
    ID3D12Device* _device = nullptr;
    ID3D12Resource* _backBuffer = nullptr; // held between Acquire and Return
    uint64_t _frame = 0;
};

} // namespace native
