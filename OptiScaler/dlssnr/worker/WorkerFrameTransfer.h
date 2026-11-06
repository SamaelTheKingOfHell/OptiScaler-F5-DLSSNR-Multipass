#pragma once

#include "../DlssNr_CrossAdapterBuffer.h"
#include "WorkerProtocol.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <limits>
#include <utility>

namespace DlssNr::Worker
{
// One shared-buffer generation. The caller owns texture state transitions and keeps every
// referenced resource and command list alive until the relevant GPU fence retires. Access must be
// serialized; do not record another outbound frame until the previous inbound copy has retired.
class FrameTransfer
{
    struct Layout
    {
        D3D12_RESOURCE_DESC desc {};
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
        UINT rows = 0;
        UINT64 rowBytes = 0, bytes = 0, offset = 0;
    };
    Microsoft::WRL::ComPtr<ID3D12Device> primary_;
    MultiGpu::CrossAdapterBuffer buffer_;
    Layout color_, depth_, motion_;
    LUID secondary_ {};

    static bool SameIdentity(ID3D12DeviceChild* child, ID3D12Device* expected)
    {
        Microsoft::WRL::ComPtr<ID3D12Device> owner;
        Microsoft::WRL::ComPtr<IUnknown> a, b;
        return child && expected && SUCCEEDED(child->GetDevice(IID_PPV_ARGS(&owner))) && owner &&
               SUCCEEDED(owner.As(&a)) && SUCCEEDED(expected->QueryInterface(IID_PPV_ARGS(&b))) && a.Get() == b.Get();
    }
    static bool Compatible(ID3D12Resource* texture, const D3D12_RESOURCE_DESC& expected)
    {
        if (!texture)
            return false;
        const auto actual = texture->GetDesc();
        return actual.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && actual.Width == expected.Width &&
               actual.Height == expected.Height && actual.DepthOrArraySize == 1 && actual.MipLevels == 1 &&
               actual.SampleDesc.Count == 1 && actual.SampleDesc.Quality == 0 && actual.Format == expected.Format;
    }
    static bool ColorFormat(DXGI_FORMAT f)
    {
        return f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    static bool DepthFormat(DXGI_FORMAT f)
    {
        return f == DXGI_FORMAT_R32_FLOAT || f == DXGI_FORMAT_R16_FLOAT || f == DXGI_FORMAT_R16_UNORM ||
               f == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    }
    static bool MotionFormat(DXGI_FORMAT f)
    {
        return f == DXGI_FORMAT_R16G16_FLOAT || f == DXGI_FORMAT_R32G32_FLOAT || f == DXGI_FORMAT_R16G16_SNORM;
    }
    static HRESULT GetLayout(ID3D12Device* primary, ID3D12Device* secondary, ID3D12Resource* texture, Layout& result)
    {
        if (!SameIdentity(texture, primary))
            return E_INVALIDARG;
        const auto desc = texture->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || !desc.Width || !desc.Height || desc.Width > 16384 ||
            desc.Height > 16384 || desc.DepthOrArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1 ||
            desc.SampleDesc.Quality != 0)
            return E_NOTIMPL;
        Layout other;
        result.desc = desc;
        primary->GetCopyableFootprints(&desc, 0, 1, 0, &result.footprint, &result.rows, &result.rowBytes,
                                       &result.bytes);
        secondary->GetCopyableFootprints(&desc, 0, 1, 0, &other.footprint, &other.rows, &other.rowBytes, &other.bytes);
        const auto& a = result.footprint.Footprint;
        const auto& b = other.footprint.Footprint;
        if (!result.bytes || result.bytes == (std::numeric_limits<uint64_t>::max)() || result.bytes != other.bytes ||
            result.rows != other.rows || result.rowBytes != other.rowBytes ||
            result.footprint.Offset != other.footprint.Offset || a.Format != b.Format || a.Width != b.Width ||
            a.Height != b.Height || a.Depth != b.Depth || a.RowPitch != b.RowPitch)
            return E_NOTIMPL;
        return S_OK;
    }
    static void Barrier(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                        D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        commands->ResourceBarrier(1, &barrier);
    }
    D3D12_TEXTURE_COPY_LOCATION Shared(const Layout& layout) const
    {
        D3D12_TEXTURE_COPY_LOCATION result {};
        result.pResource = buffer_.PrimaryBuffer();
        result.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        result.PlacedFootprint = layout.footprint;
        result.PlacedFootprint.Offset = layout.offset;
        return result;
    }
    static D3D12_TEXTURE_COPY_LOCATION Texture(ID3D12Resource* resource)
    {
        D3D12_TEXTURE_COPY_LOCATION result {};
        result.pResource = resource;
        result.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        return result;
    }

  public:
    static HRESULT Create(ID3D12Device* primary, ID3D12Device* secondary, ID3D12Resource* color, ID3D12Resource* depth,
                          ID3D12Resource* motion, FrameTransfer& out)
    {
        if (!primary || !secondary)
            return E_INVALIDARG;
        FrameTransfer result;
        if (!color || !motion || !ColorFormat(color->GetDesc().Format) || !MotionFormat(motion->GetDesc().Format) ||
            (depth && !DepthFormat(depth->GetDesc().Format)))
            return E_NOTIMPL;
        HRESULT hr = GetLayout(primary, secondary, color, result.color_);
        if (FAILED(hr))
            return hr;
        if (depth)
        {
            hr = GetLayout(primary, secondary, depth, result.depth_);
            if (FAILED(hr))
                return hr;
        }
        hr = GetLayout(primary, secondary, motion, result.motion_);
        if (FAILED(hr))
            return hr;
        constexpr uint64_t align = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
        constexpr uint64_t max = (std::numeric_limits<uint64_t>::max)();
        if (result.color_.bytes > max - (align - 1))
            return E_INVALIDARG;
        result.depth_.offset = (result.color_.bytes + align - 1) & ~(align - 1);
        if (result.depth_.bytes > max - result.depth_.offset ||
            result.depth_.offset + result.depth_.bytes > max - (align - 1))
            return E_INVALIDARG;
        result.motion_.offset = (result.depth_.offset + result.depth_.bytes + align - 1) & ~(align - 1);
        if (result.motion_.bytes > max - result.motion_.offset)
            return E_INVALIDARG;
        hr = MultiGpu::CrossAdapterBuffer::Create(primary, secondary, result.motion_.offset + result.motion_.bytes,
                                                  result.buffer_);
        if (FAILED(hr))
            return hr;
        result.primary_ = primary;
        result.secondary_ = secondary->GetAdapterLuid();
        out = std::move(result);
        return S_OK;
    }
    const MultiGpu::CrossAdapterBuffer& Buffer() const { return buffer_; }
    InitialFrameWire MakeWire(unsigned passes) const
    {
        InitialFrameWire wire {};
        wire.width = static_cast<uint32_t>(color_.desc.Width);
        wire.height = color_.desc.Height;
        wire.colorFormat = color_.desc.Format;
        wire.depthFormat = depth_.desc.Format;
        wire.depthWidth = static_cast<uint32_t>(depth_.desc.Width);
        wire.depthHeight = depth_.desc.Height;
        wire.motionFormat = motion_.desc.Format;
        wire.motionWidth = static_cast<uint32_t>(motion_.desc.Width);
        wire.motionHeight = motion_.desc.Height;
        wire.frameCount = 0;
        wire.passCount = passes;
        wire.bytes = buffer_.Size();
        wire.colorBytes = color_.bytes;
        wire.depthBytes = depth_.bytes;
        wire.motionBytes = motion_.bytes;
        wire.depthOffset = depth_.offset;
        wire.motionOffset = motion_.offset;
        wire.colorPitch = color_.footprint.Footprint.RowPitch;
        wire.depthPitch = depth_.footprint.Footprint.RowPitch;
        wire.motionPitch = motion_.footprint.Footprint.RowPitch;
        if (primary_)
            wire.primary = primary_->GetAdapterLuid();
        wire.secondary = secondary_;
        return wire;
    }
    // Present textures must be in COPY_SOURCE and the shared buffer must be in COMMON.
    // rows != 0 copies only colour rows [rowOffset, rowOffset + rows), at the same rows of the shared region.
    HRESULT RecordOutbound(ID3D12GraphicsCommandList* commands, ID3D12Resource* color, ID3D12Resource* depth,
                           ID3D12Resource* motion, UINT rowOffset = 0, UINT rows = 0) const
    {
        if (rows && (static_cast<uint64_t>(rowOffset) + rows > color_.desc.Height))
            return E_INVALIDARG;
        const bool hasDepth = depth_.desc.Format != DXGI_FORMAT_UNKNOWN;
        if (!buffer_.PrimaryBuffer() || !SameIdentity(commands, primary_.Get()) ||
            !SameIdentity(color, primary_.Get()) || !SameIdentity(motion, primary_.Get()) ||
            !Compatible(color, color_.desc) || !Compatible(motion, motion_.desc) ||
            (hasDepth ? (!SameIdentity(depth, primary_.Get()) || !Compatible(depth, depth_.desc)) : depth != nullptr))
            return E_INVALIDARG;
        Barrier(commands, buffer_.PrimaryBuffer(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        const auto copy = [&](ID3D12Resource* resource, const Layout& layout)
        {
            auto src = Texture(resource);
            auto dst = Shared(layout);
            commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        };
        if (rows)
        {
            auto src = Texture(color);
            auto dst = Shared(color_);
            const D3D12_BOX box { 0, rowOffset, 0, static_cast<UINT>(color_.desc.Width), rowOffset + rows, 1 };
            commands->CopyTextureRegion(&dst, 0, rowOffset, 0, &src, &box);
        }
        else
            copy(color, color_);
        if (hasDepth)
            copy(depth, depth_);
        copy(motion, motion_);
        Barrier(commands, buffer_.PrimaryBuffer(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        return S_OK;
    }
    // The destination texture must be in COPY_DEST and the shared buffer must be in COMMON.
    // rows != 0 copies only rows [rowOffset, rowOffset + rows) to the same rows of the output.
    HRESULT RecordInbound(ID3D12GraphicsCommandList* commands, ID3D12Resource* output, UINT rowOffset = 0,
                          UINT rows = 0) const
    {
        if (!buffer_.PrimaryBuffer() || !SameIdentity(commands, primary_.Get()) ||
            !SameIdentity(output, primary_.Get()) || !Compatible(output, color_.desc) ||
            (rows && static_cast<uint64_t>(rowOffset) + rows > color_.desc.Height))
            return E_INVALIDARG;
        Barrier(commands, buffer_.PrimaryBuffer(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        auto src = Shared(color_);
        auto dst = Texture(output);
        if (rows)
        {
            const D3D12_BOX box { 0, rowOffset, 0, static_cast<UINT>(color_.desc.Width), rowOffset + rows, 1 };
            commands->CopyTextureRegion(&dst, 0, rowOffset, 0, &src, &box);
        }
        else
            commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Barrier(commands, buffer_.PrimaryBuffer(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        return S_OK;
    }
};
} // namespace DlssNr::Worker
