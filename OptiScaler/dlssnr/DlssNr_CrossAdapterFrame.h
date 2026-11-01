#pragma once

#include "DlssNr_CrossAdapterTexture.h"

namespace DlssNr::MultiGpu
{
// The core neural inputs share one scheduling boundary: color, optional depth and motion are copied
// before the outbound fence, and the edited color is returned before the inbound fence.
// This owns transport resources only; it does not retain the game textures or submit GPU work.
class CrossAdapterFrame
{
    CrossAdapterTexture color_;
    CrossAdapterTexture depth_;
    CrossAdapterTexture motion_;
    bool hasDepth_ = false;

  public:
    // Build all three transports transactionally. If any format or adapter is unsupported,
    // `out` remains unchanged and the caller can use the existing single-device path. Retire any
    // previous use of `out` before replacing it. A null depth description selects the no-depth path.
    static HRESULT Create(ID3D12Device* primary, ID3D12Device* secondary, const D3D12_RESOURCE_DESC& color,
                          const D3D12_RESOURCE_DESC* depth, const D3D12_RESOURCE_DESC& motion, CrossAdapterFrame& out)
    {
        CrossAdapterFrame result;
        HRESULT hr = CrossAdapterTexture::Create(primary, secondary, color, result.color_);
        if (FAILED(hr))
            return hr;
        if (depth)
        {
            hr = CrossAdapterTexture::Create(primary, secondary, *depth, result.depth_);
            if (FAILED(hr))
                return hr;
            result.hasDepth_ = true;
        }
        hr = CrossAdapterTexture::Create(primary, secondary, motion, result.motion_);
        if (FAILED(hr))
            return hr;
        out = std::move(result);
        return S_OK;
    }
    bool HasDepth() const { return hasDepth_; }
    CrossAdapterTexture& Color() { return color_; }
    CrossAdapterTexture& Depth() { return depth_; }
    CrossAdapterTexture& Motion() { return motion_; }
};
} // namespace DlssNr::MultiGpu
