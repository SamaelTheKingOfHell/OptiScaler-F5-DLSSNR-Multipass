#pragma once
// Where a DLSS-NR feature keeps its workspace. The network split (DlssNr_NetworkSplit.h) copies byte ranges in and out
// of the model's workspace buffer, which the model allocates itself. With NGX's ResourceAllocCallback installed on the
// capability parameters, those allocations come through here instead, so every live model buffer is known, whenever it
// is allocated (some runtimes allocate the workspace while the feature is created, others on its first evaluate).
// Which buffer is the workspace is decided from the model's own launches: the one its first kernel (cc_cb_clear)
// clears from (see DlssNr_SplitCalibration.h).
//
// ```cpp
// DlssNr::ModelAllocations::Install(params);   // once, before the creates whose workspace is needed
// DlssNr::ModelAllocations::UseDevice(device); // the device NGX creates on
// feature = create(...); evaluate(...);
// auto ws = DlssNr::ModelAllocations::StartingAt(address); // {} when no live buffer starts there
// ```

#include <d3d12.h>
#include <map>
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
    std::map<const void*, Workspace> buffers; // live buffers handed to NGX, by resource pointer
    size_t allocated = 0;                      // everything allocated through the callback so far
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
    ++s.allocated;
    if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        s.buffers[r.Get()] = { r->GetGPUVirtualAddress(), desc->Width };
    *out = r.Detach(); // NGX holds this reference and returns it through Release
}
inline void __cdecl Release(IUnknown* resource)
{
    if (!resource)
        return;
    {
        auto& s = S();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.buffers.erase(static_cast<const void*>(static_cast<ID3D12Resource*>(static_cast<void*>(resource))));
    }
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
inline void UseDevice(ID3D12Device* device)
{
    auto& s = Detail::S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (device)
        s.device = device;
}
// The live model buffer that starts at `address`, or {} when none does.
inline Workspace StartingAt(uint64_t address)
{
    auto& s = Detail::S();
    std::lock_guard<std::mutex> lock(s.mutex);
    for (const auto& [resource, b] : s.buffers)
        if (address && b.address == address)
            return b;
    return {};
}
// For diagnostics: live buffers, and allocations seen in total.
inline std::pair<size_t, size_t> Counts()
{
    auto& s = Detail::S();
    std::lock_guard<std::mutex> lock(s.mutex);
    return { s.buffers.size(), s.allocated };
}
} // namespace DlssNr::ModelAllocations
