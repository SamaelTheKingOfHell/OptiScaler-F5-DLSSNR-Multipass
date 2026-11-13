#pragma once

// The D3D11 adapter's frame source: the game's picture and the depth finder's copy (resource_tracking/GenericDepth_Dx11.h) are
// shared across to the D3D12 device the producer runs on (native/SharedFrame.h), and the processed picture is shared back. Uses
// its own D3D12 device/queue, paired with the game's D3D11 device the same way IFeature_Dx11wDx12 pairs one for a game that does
// call an upscaler (with_dx12/with_dx12.h); no game call is needed to start it here.

#include "FrameContract.h"
#include "MsaaPictureDx11.h"
#include "SharedFrame.h"

#include <d3d11_4.h>
#include <wrl/client.h>

namespace native
{

class Dx11FrameSource : public IFrameSource
{
  public:
    // The present being processed: the swap chain and the game's D3D11 device. Call before Acquire(). `copyBack`: the
    // result goes back into the D3D11 back buffer (Return). False under frame generation's swap chain (Dx11wDx12SC),
    // which copies ProcessedPicture() out on the D3D12 side and shows nothing of the D3D11 back buffer.
    void SetPresent(IDXGISwapChain* swapChain, ID3D11Device* device11, bool copyBack = true);

    Api GetApi() const override { return Api::D3D11; }
    AcquireStatus Acquire(FrameInput& input) override;
    void Return(const FrameInput& input, const FrameOutput& output) override;
    void OnResize() override;
    void Skip() override { _backBuffer.Reset(); }

    ID3D12Device* Device() const override { return _device12; }
    ID3D12CommandQueue* Queue() const override { return _queue12; }
    const std::string& Error() const { return _error; }

    // The D3D12-side picture, valid once Acquire has succeeded: the producer's consumer (DLSS-NR or
    // VirtualUpscalerDriver) writes into it in place, so after Run/Return this is that result, rests in
    // D3D12_RESOURCE_STATE_COMMON. A caller with its own use for the D3D12 side of this frame (a D3D11-under-frame-
    // generation present, which needs it copied into the real D3D12 back buffer) reads it from here rather than
    // waiting for Return's D3D11 copy-back, which nothing shows in that case.
    ID3D12Resource* ProcessedPicture() const { return _picture.Res12(); }

  private:
    bool EnsureDevices(ID3D11Device* device11);

    IDXGISwapChain* _swapChain = nullptr;
    ID3D11Device* _device11 = nullptr;
    ID3D11Device* _realDevice11 = nullptr; // the device behind _device11 when that is a proxy (Streamline's), else it
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> _context11;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> _context4;
    ID3D12Device* _device12 = nullptr;    // owned by WithDx12, not released here
    ID3D12CommandQueue* _queue12 = nullptr;

    SharedFence _fence;
    SharedTexture _picture;
    SharedTexture _depth;
    // A multisampled swap chain (the game's MSAA setting) cannot be copied to or from the single-sample picture: the picture
    // is resolved in, and the result is drawn back into every sample. Unused for a single-sample back buffer.
    MsaaPictureDx11 _msaa;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _backBuffer; // held between Acquire and Return

    uint64_t _frame = 0;
    std::string _error;

    bool _copyBack = true;
    // D3D12 work of an earlier frame may still read the shared picture and depth (no copy-back waited for it): the next
    // Acquire makes the D3D11 context wait for it before overwriting them
    bool _sharedInUse = false;
};

} // namespace native
