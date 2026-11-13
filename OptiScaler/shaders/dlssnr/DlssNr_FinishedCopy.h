#pragma once
#include <d3d12.h>
#include "DlssNr_Guides.h"

namespace DlssNr
{
// Preserve allocation and offsets. The NR spatial pack and NGX inputs use ResolveGuideRegions;
// texels outside that active rectangle are undefined/stale, including in full-allocation debug dumps.
// Any new consumer must use the active rectangle, or request a full copy.
// Depth/stencil copies must cover the complete subresource; retain CopyResource for them
// and for unknown formats (including multi-plane formats).
inline bool CanCopyFinishedGuideRegion(const D3D12_RESOURCE_DESC& desc, GuideRegion region)
{
    if (!region.valid() || region.x > desc.Width || region.y > desc.Height || region.width > desc.Width - region.x ||
        region.height > desc.Height - region.y || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.SampleDesc.Count != 1 || (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0)
        return false;
    switch (desc.Format)
    {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return true;
    default:
        return false;
    }
}

inline void CopyFinishedGuide(ID3D12GraphicsCommandList* cmd, ID3D12Resource* destination, ID3D12Resource* source,
                              const D3D12_RESOURCE_DESC& desc, GuideRegion region)
{
    // `destination` holds one subresource. A source with mips or array slices (a depth pyramid in the depth buffer's
    // lower mips) gives it only its first subresource: CopyResource would need identical subresource counts.
    const bool oneSubresource = desc.MipLevels == 1 && desc.DepthOrArraySize == 1;
    const D3D12_TEXTURE_COPY_LOCATION src { source, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {} };
    const D3D12_TEXTURE_COPY_LOCATION dst { destination, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {} };
    if (!CanCopyFinishedGuideRegion(desc, region) ||
        (region.x == 0 && region.y == 0 && region.width == desc.Width && region.height == desc.Height))
    {
        if (oneSubresource)
            cmd->CopyResource(destination, source);
        else
            cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        return;
    }
    const D3D12_BOX box { region.x, region.y, 0, region.x + region.width, region.y + region.height, 1 };
    cmd->CopyTextureRegion(&dst, region.x, region.y, 0, &src, &box);
}
} // namespace DlssNr
