#pragma once

#include "DlssNr_CrossAdapterBuffer.h"

namespace DlssNr::MultiGpu
{
// Shared texture transport through a row-pitched buffer. The caller owns resource state
// transitions, queue ordering, fence values and lifetime. Only single-plane, single-sample,
// single-subresource 2D descriptions accepted by both devices are supported by this prototype.
class CrossAdapterTexture
{
    CrossAdapterBuffer buffer_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint_ {};
    UINT rows_ = 0;
    UINT64 rowBytes_ = 0;

  public:
    // Build equivalent copy footprints on each device. Unsupported or differing footprints are
    // rejected; the caller can preserve the original single-device path instead.
    static HRESULT Create(ID3D12Device* primary, ID3D12Device* secondary, const D3D12_RESOURCE_DESC& image,
                          CrossAdapterTexture& out)
    {
        if (!primary || !secondary || image.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || image.Width == 0 ||
            image.Height == 0 || image.DepthOrArraySize != 1 || image.MipLevels != 1 || image.SampleDesc.Count != 1 ||
            image.SampleDesc.Quality != 0 || image.Format == DXGI_FORMAT_UNKNOWN)
            return E_INVALIDARG;
        D3D12_FEATURE_DATA_FORMAT_INFO formatInfo { image.Format };
        if (FAILED(primary->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &formatInfo, sizeof(formatInfo))) ||
            formatInfo.PlaneCount != 1)
            return E_NOTIMPL;
        D3D12_FEATURE_DATA_FORMAT_INFO secondaryFormat { image.Format };
        if (FAILED(
                secondary->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &secondaryFormat, sizeof(secondaryFormat))) ||
            secondaryFormat.PlaneCount != 1)
            return E_NOTIMPL;
        D3D12_FEATURE_DATA_FORMAT_SUPPORT primarySupport { image.Format };
        D3D12_FEATURE_DATA_FORMAT_SUPPORT secondarySupport { image.Format };
        if (FAILED(
                primary->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &primarySupport, sizeof(primarySupport))) ||
            FAILED(secondary->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &secondarySupport,
                                                  sizeof(secondarySupport))) ||
            !(primarySupport.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) ||
            !(secondarySupport.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D))
            return E_NOTIMPL;
        CrossAdapterTexture result;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT other {};
        UINT otherRows = 0;
        UINT64 otherRowBytes = 0, bytes = 0, otherBytes = 0;
        primary->GetCopyableFootprints(&image, 0, 1, 0, &result.footprint_, &result.rows_, &result.rowBytes_, &bytes);
        secondary->GetCopyableFootprints(&image, 0, 1, 0, &other, &otherRows, &otherRowBytes, &otherBytes);
        const auto& a = result.footprint_.Footprint;
        const auto& b = other.Footprint;
        if (!bytes || !result.rows_ || !result.rowBytes_ || !a.RowPitch ||
            bytes == (std::numeric_limits<UINT64>::max)() || bytes != otherBytes ||
            result.footprint_.Offset != other.Offset || result.rows_ != otherRows ||
            result.rowBytes_ != otherRowBytes || a.Format != b.Format || a.Width != b.Width || a.Height != b.Height ||
            a.Depth != b.Depth || a.RowPitch != b.RowPitch)
            return E_NOTIMPL;
        const HRESULT hr = CrossAdapterBuffer::Create(primary, secondary, bytes, result.buffer_);
        if (FAILED(hr))
            return hr;
        out = std::move(result);
        return S_OK;
    }

    D3D12_TEXTURE_COPY_LOCATION PrimaryLocation() const { return Location(buffer_.PrimaryBuffer()); }
    D3D12_TEXTURE_COPY_LOCATION SecondaryLocation() const { return Location(buffer_.SecondaryBuffer()); }
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& Footprint() const { return footprint_; }
    UINT Rows() const { return rows_; }
    UINT64 RowBytes() const { return rowBytes_; }
    CrossAdapterBuffer& Buffer() { return buffer_; }

  private:
    D3D12_TEXTURE_COPY_LOCATION Location(ID3D12Resource* resource) const
    {
        D3D12_TEXTURE_COPY_LOCATION location {};
        location.pResource = resource;
        location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        location.PlacedFootprint = footprint_;
        return location;
    }
};
} // namespace DlssNr::MultiGpu
