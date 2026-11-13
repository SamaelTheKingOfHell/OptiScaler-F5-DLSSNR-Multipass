#pragma once

#include <d3d12.h>
#include <functional>
#include <memory>

namespace DlssNr
{
// NR-local lifetime tracking. Record/submit/reset/retire/collect calls are serialized internally.
// Owners must still guard their resources and outlive all calls. Lock order when owned by NR:
// owner registry -> NR state -> tracker.
//
// Destruction callbacks run only from Collect(), which the owner calls from its own recording path. Submit and reset
// notifications (which arrive inside the game's ExecuteCommandLists / Reset hooks, on any thread) only mark state, so
// nothing is released inside a hook.
class GpuLifetime
{
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    // Called, on whatever thread destroys it, when a recorded command list is destroyed (released rather than Reset):
    // the owner stops watching that identity. Never called for a list that is merely recorded again.
    using Destroyed = void (*)(const void* identity);
    explicit GpuLifetime(Destroyed destroyed = nullptr);
    ~GpuLifetime();
    GpuLifetime(const GpuLifetime&) = delete;
    GpuLifetime& operator=(const GpuLifetime&) = delete;
    void Record(ID3D12GraphicsCommandList* commands);
    // One reusable monotonic fence per queue, kept only while a recording still waits on it; aliases are normalized
    // at every notification. Only call after the real ExecuteCommandLists or a successful command-list Reset.
    void Submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);
    void ResetRecording(ID3D12CommandList* commands);
    // Whether this list (as the submit hook sees it) still has an open recording, which it may submit again.
    bool Open(ID3D12CommandList* commands);
    // Start tracking a replacement resource set. Older recordings still receive submit/reset
    // notifications, but only recordings used again belong to the new generation.
    void BeginGeneration();
    // Queued only; runs from a later Collect() once every recording of the current generation has completed.
    // Unresolved callbacks, including their captured ownership, are retained at destruction.
    void Retire(std::function<void()> destroy);
    // The same, for a callback that only reports to `owner`: once `owner` has expired, Collect() drops it without
    // running it, so a recording that never completes does not keep it (and the recordings it waits on) for good.
    void Retire(std::function<void()> destroy, std::weak_ptr<const void> owner);
    // Runs the destruction callbacks whose GPU work has completed. A removed device counts as completed.
    void Collect();
};
} // namespace DlssNr
