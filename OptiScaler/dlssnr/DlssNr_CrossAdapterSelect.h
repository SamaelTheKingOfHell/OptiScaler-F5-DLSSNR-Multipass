#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <utility>

namespace DlssNr::MultiGpu
{
using CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
struct SelectedAdapter
{
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    DXGI_ADAPTER_DESC1 description {};
};

inline bool SameAdapterLuid(LUID a, LUID b) { return a.HighPart == b.HighPart && a.LowPart == b.LowPart; }

// Select a non-software adapter by its actual LUID, never by display enumeration order. The
// primary LUID must come from the game's device. A requested LUID is exact: no other adapter is
// substituted if it is missing or cannot create a device. A null request tries eligible adapters
// in factory order. Vendor and the unhooked device-creation function are supplied by the caller.
// This checks D3D12 creation only; the caller must independently check model/runtime compatibility.
// `out` stays unchanged on failure.
inline HRESULT SelectSecondary(IDXGIFactory4* factory, LUID primary, const LUID* requested, UINT vendor,
                               CreateDevice createDevice, SelectedAdapter& out)
{
    if (!factory || !createDevice || !vendor || (requested && SameAdapterLuid(*requested, primary)))
        return E_INVALIDARG;
    HRESULT lastFailure = DXGI_ERROR_NOT_FOUND;
    for (UINT i = 0;; ++i)
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        HRESULT hr = factory->EnumAdapters1(i, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND)
            break;
        if (FAILED(hr))
            return hr;
        DXGI_ADAPTER_DESC1 desc {};
        hr = adapter->GetDesc1(&desc);
        if (FAILED(hr))
            return hr;
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || desc.VendorId != vendor ||
            SameAdapterLuid(desc.AdapterLuid, primary) || (requested && !SameAdapterLuid(desc.AdapterLuid, *requested)))
            continue;
        SelectedAdapter candidate;
        hr = createDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&candidate.device));
        if (FAILED(hr) || !candidate.device)
        {
            lastFailure = FAILED(hr) ? hr : E_FAIL;
            if (requested)
                return lastFailure;
            continue;
        }
        candidate.adapter = adapter;
        candidate.description = desc;
        out = std::move(candidate);
        return S_OK;
    }
    return lastFailure;
}
} // namespace DlssNr::MultiGpu
