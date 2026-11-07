#pragma once
// Where a DLSS-NR feature keeps its workspace. The network split (DlssNr_NetworkSplit.h) copies byte ranges in and out
// of the model's workspace buffer, which the model allocates itself while a feature is created. With NGX's
// ResourceAllocCallback installed on the capability parameters, those allocations come through here instead, so the
// buffers are known. Which one is the workspace is decided from the model's own launches: the buffer its first kernel
// (cc_cb_clear) clears from (see DlssNr_SplitCalibration.h); the smallest writable buffer is not it at every size.
//
// ```cpp
// DlssNr::ModelAllocations::Install(params);            // once, before the creates whose workspace is needed
// DlssNr::ModelAllocations::BeginCreate(device);
// feature = create(...);
// auto buffers = DlssNr::ModelAllocations::EndCreate(); // empty when nothing came through the callback
// ```
// Resources are created with the device given to BeginCreate (or the last one), which is the device NGX creates on.

#include <d3d12.h>
#include <mutex>
#include <vector>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_params.h>
#include <wrl/client.h>

namespace DlssNr::ModelAllocations
{
struct Workspace
{
    uint64_t address = 0, bytes = 0;
};

namespace Detail
{
struct State
{
    std::mutex mutex;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    bool capturing = false;
    std::vector<Workspace> buffers;
};
inline State& S()
{
    static State* s = new State; // callbacks may run during shutdown
    return *s;
}
inline void __cdecl Allocate(D3D12_RESOURCE_DESC* desc, int state, D3D12_HEAP_PROPERTIES* heap, ID3D12Resource** out)
{
    if (!out)
        return;
    *out = nullptr;
    auto& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.device || !desc || !heap)
        return;
    Microsoft::WRL::ComPtr<ID3D12Resource> r;
    if (FAILED(s.device->CreateCommittedResource(heap, D3D12_HEAP_FLAG_NONE, desc,
                                                 static_cast<D3D12_RESOURCE_STATES>(state), nullptr,
                                                 IID_PPV_ARGS(&r))))
        return;
    if (s.capturing && desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        s.buffers.push_back({ r->GetGPUVirtualAddress(), desc->Width });
    *out = r.Detach(); // NGX holds this reference and returns it through Release
}
inline void __cdecl Release(IUnknown* resource)
{
    if (resource)
        resource->Release();
}
} // namespace Detail

// Installs the callbacks on `params`. Every feature created with them afterwards allocates through here.
inline bool Install(NVSDK_NGX_Parameter* params)
{
    if (!params)
        return false;
    params->Set(NVSDK_NGX_Parameter_ResourceAllocCallback, reinterpret_cast<void*>(&Detail::Allocate));
    params->Set(NVSDK_NGX_Parameter_ResourceReleaseCallback, reinterpret_cast<void*>(&Detail::Release));
    return true;
}
inline void BeginCreate(ID3D12Device* device)
{
    auto& s = Detail::S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (device)
        s.device = device;
    s.capturing = true;
    s.buffers.clear();
}
inline std::vector<Workspace> EndCreate()
{
    auto& s = Detail::S();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.capturing = false;
    return std::move(s.buffers);
}
// The buffer that starts at `address`, or {} when none of `buffers` does.
inline Workspace StartingAt(const std::vector<Workspace>& buffers, uint64_t address)
{
    for (const auto& b : buffers)
        if (address && b.address == address)
            return b;
    return {};
}
} // namespace DlssNr::ModelAllocations
