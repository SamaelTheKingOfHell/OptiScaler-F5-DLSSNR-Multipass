#pragma once
// Where a DLSS-NR feature keeps its workspace. The network split (DlssNr_NetworkSplit.h) copies byte ranges in and out
// of the model's workspace buffer, which the model allocates itself while a feature is created. With NGX's
// ResourceAllocCallback installed on the capability parameters, those allocations come through here instead, so the
// buffer is known: of the buffers one create allocates, the workspace is the smallest writable one (the weight copy
// and its read-only twin are larger; the history is a texture).
//
// ```cpp
// DlssNr::ModelAllocations::Install(params);            // once, before the creates whose workspace is needed
// DlssNr::ModelAllocations::BeginCreate(device);
// feature = create(...);
// auto ws = DlssNr::ModelAllocations::EndCreate();      // ws.address == 0 when nothing came through the callback
// ```
// Resources are created with the device given to BeginCreate (or the last one), which is the device NGX creates on.

#include <d3d12.h>
#include <mutex>
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
    Workspace smallest;
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
    if (s.capturing && desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
        (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) &&
        (!s.smallest.address || desc->Width < s.smallest.bytes))
        s.smallest = { r->GetGPUVirtualAddress(), desc->Width };
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
    s.smallest = {};
}
inline Workspace EndCreate()
{
    auto& s = Detail::S();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.capturing = false;
    return s.smallest;
}
} // namespace DlssNr::ModelAllocations
