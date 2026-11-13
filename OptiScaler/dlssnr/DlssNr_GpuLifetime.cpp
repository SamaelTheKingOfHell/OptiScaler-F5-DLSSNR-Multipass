#include "pch.h"
#include "DlssNr_GpuLifetime.h"
#include <Util.h>
#include <algorithm>
#include <vector>
#include <map>
#include <wrl/client.h>
#include <atomic>
#include <mutex>
#include <objbase.h>

namespace DlssNr
{
namespace
{
// Identity only (never dereferenced later); no log line, this runs for every notification.
template <typename T> T* Identity(T* object) { return static_cast<T*>(Util::RealObjectIdentity(object)); }
} // namespace

struct GpuLifetime::Impl
{
    struct Timeline
    {
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        UINT64 value = 0;
        bool failed = false;
        // The device was removed: nothing on this queue executes any more, so everything waiting on it is done.
        std::atomic_bool lost { false };
        bool Lost()
        {
            if (!lost && device && device->GetDeviceRemovedReason() != S_OK)
                lost = true;
            return lost;
        }
    };
    struct Recording
    {
        std::atomic_bool open { true };
        bool signalFailed = false;
        // Only the latest value on each queue is needed, including when the list is replayed.
        std::map<std::shared_ptr<Timeline>, UINT64> completions;
        bool Complete() const { return !open && Finished(); }
        bool Finished() const
        {
            return !signalFailed && std::all_of(completions.begin(), completions.end(),
                                                [](const auto& c)
                                                {
                                                    const auto& [timeline, value] = c;
                                                    if (timeline->lost)
                                                        return true;
                                                    const auto completed = timeline->fence->GetCompletedValue();
                                                    if (completed == UINT64_MAX) // device removed
                                                    {
                                                        timeline->lost = true;
                                                        return true;
                                                    }
                                                    return completed >= value;
                                                });
        }
    };
    // What the destruction watch shares with the tracker. Held by both, so a watch released after the tracker is
    // gone (or the other way round) is still safe. Its mutex is taken on the destroying thread.
    struct Lists
    {
        std::recursive_mutex mutex;
        Destroyed destroyed = nullptr;
        struct Entry
        {
            std::weak_ptr<Recording> recording;
            const void* watch = nullptr; // the watch that owns this entry, identity only
        };
        std::map<const void*, Entry> byIdentity;
    };
    // Command lists can be released instead of Reset. A private IUnknown notification closes that recording
    // without retaining/dereferencing the command list itself. One watch per list for the list's whole life.
    struct ListWatch final : IUnknown
    {
        std::atomic<ULONG> references { 1 };
        std::shared_ptr<Lists> lists;
        const void* identity;
        ListWatch(std::shared_ptr<Lists> use, const void* id) : lists(std::move(use)), identity(id) {}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
        {
            if (!out)
                return E_POINTER;
            *out = nullptr;
            if (iid != __uuidof(IUnknown))
                return E_NOINTERFACE;
            *out = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
        ULONG STDMETHODCALLTYPE Release() override
        {
            const auto remaining = --references;
            if (!remaining)
            {
                Destroyed destroyed = nullptr;
                {
                    std::lock_guard lock(lists->mutex);
                    const auto entry = lists->byIdentity.find(identity);
                    if (entry != lists->byIdentity.end() && entry->second.watch == this)
                    {
                        if (auto use = entry->second.recording.lock())
                            use->open = false;
                        lists->byIdentity.erase(entry);
                        destroyed = lists->destroyed;
                    }
                }
                if (destroyed)
                    destroyed(identity);
                delete this;
            }
            return remaining;
        }
    };

    // Reset notifications may arrive on a different engine thread from Record/Retire. The same mutex guards the
    // identity map, which the destroying thread also takes.
    std::shared_ptr<Lists> lists = std::make_shared<Lists>();
    std::recursive_mutex& mutex = lists->mutex;
    GUID watchKey {};
    bool watchKeyValid = SUCCEEDED(CoCreateGuid(&watchKey));
    using Uses = std::vector<std::shared_ptr<Recording>>;
    struct Retired
    {
        Uses uses;
        std::function<void()> destroy;
        std::weak_ptr<const void> owner;
        bool owned = false;
    };
    std::vector<std::pair<const void*, std::shared_ptr<Recording>>> recordings;
    Uses currentGeneration;
    std::vector<Retired> retired;
    std::vector<std::shared_ptr<Timeline>> timelines;
    bool collecting = false;
    static bool Complete(const Uses& uses)
    {
        return std::all_of(uses.begin(), uses.end(), [](const auto& use) { return use->Complete(); });
    }
    std::shared_ptr<Recording> OpenRecording(const void* commands)
    {
        const auto entry = lists->byIdentity.find(commands);
        if (entry != lists->byIdentity.end())
            if (auto use = entry->second.recording.lock(); use && use->open)
                return use;
        // A list whose destruction cannot be watched is still tracked by its recordings vector entry.
        for (const auto& [identity, use] : recordings)
            if (identity == commands && use->open)
                return use;
        return {};
    }
    std::shared_ptr<Timeline> QueueTimeline(ID3D12CommandQueue* queue)
    {
        if (!queue)
            return {};
        for (const auto& timeline : timelines)
            if (timeline->queue.Get() == queue)
                return timeline;
        auto timeline = std::make_shared<Timeline>();
        timeline->queue = queue; // keep queue identity stable while a recording waits on its fence
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&timeline->device))) ||
            FAILED(timeline->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&timeline->fence))))
            timeline->failed = true;
        timelines.push_back(timeline);
        return timeline;
    }
    // Drops finished recordings and every timeline no recording waits on any more: no queue or device is kept alive
    // by the tracker once its work is done (or its device is gone).
    void Compact()
    {
        std::erase_if(recordings, [](const auto& item) { return item.second->Complete(); });
        std::erase_if(currentGeneration, [](const auto& use) { return use->Complete(); });
        std::erase_if(timelines, [](const auto& timeline) { return timeline.use_count() == 1; });
    }
};

GpuLifetime::GpuLifetime(Destroyed destroyed) : impl(std::make_unique<Impl>()) { impl->lists->destroyed = destroyed; }
GpuLifetime::~GpuLifetime()
{
    Collect();
    // Keep unresolved callbacks and their captures, including runtime/queue ownership.
    // Destroying a callback without running it can still release objects the GPU needs.
    if (!impl->recordings.empty() || !impl->retired.empty())
        (void) impl.release();
}
void GpuLifetime::Record(ID3D12GraphicsCommandList* commands)
{
    if (!commands)
        return;
    commands = Identity(commands);
    std::lock_guard lock(impl->mutex);
    if (auto use = impl->OpenRecording(commands))
    {
        if (std::find(impl->currentGeneration.begin(), impl->currentGeneration.end(), use) ==
            impl->currentGeneration.end())
            impl->currentGeneration.push_back(use);
        return;
    }
    auto use = std::make_shared<Impl::Recording>();
    auto entry = impl->lists->byIdentity.find(commands);
    if (entry == impl->lists->byIdentity.end() && impl->watchKeyValid)
    {
        // First recording on this list: one watch for the list's life, replaced only if the list is destroyed.
        auto* watch = new Impl::ListWatch(impl->lists, commands);
        impl->lists->byIdentity[commands].watch = watch;
        if (FAILED(commands->SetPrivateDataInterface(impl->watchKey, watch)))
            impl->lists->byIdentity.erase(commands); // failure must not pretend the recording can be discarded
        watch->Release();
        entry = impl->lists->byIdentity.find(commands);
    }
    if (entry != impl->lists->byIdentity.end())
        entry->second.recording = use;
    impl->currentGeneration.push_back(use);
    impl->recordings.emplace_back(commands, std::move(use));
}
bool GpuLifetime::Open(ID3D12CommandList* commands)
{
    if (!commands)
        return false;
    commands = Identity(commands);
    std::lock_guard lock(impl->mutex);
    return impl->OpenRecording(commands) != nullptr;
}
void GpuLifetime::Submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    std::lock_guard lock(impl->mutex);
    if (impl->recordings.empty())
        return;
    std::vector<Impl::Recording*> matched;
    for (UINT i = 0; i < count; ++i)
    {
        if (auto use = impl->OpenRecording(Identity(lists[i]));
            use && std::find(matched.begin(), matched.end(), use.get()) == matched.end())
            matched.push_back(use.get());
    }
    if (matched.empty())
        return;
    const auto timeline = impl->QueueTimeline(Identity(queue));
    // One signal covers every matched list in this actual ExecuteCommandLists notification.
    const bool signalled = timeline && !timeline->failed && timeline->value != UINT64_MAX - 1 &&
                           SUCCEEDED(timeline->queue->Signal(timeline->fence.Get(), ++timeline->value));
    if (timeline && !signalled)
        timeline->failed = true;
    for (auto* use : matched)
    {
        if (signalled)
            use->completions[timeline] = timeline->value;
        else if (timeline && timeline->Lost())
            use->completions[timeline] = 0; // nothing will execute on a removed device
        else
            use->signalFailed = true; // unresolved: retained rather than released under the GPU
    }
}
void GpuLifetime::ResetRecording(ID3D12CommandList* commands)
{
    if (!commands)
        return;
    commands = Identity(commands);
    std::lock_guard lock(impl->mutex);
    for (auto& [identity, use] : impl->recordings)
        if (identity == commands)
            use->open = false;
}
void GpuLifetime::Retire(std::function<void()> destroy)
{
    std::lock_guard lock(impl->mutex);
    impl->retired.push_back({ impl->currentGeneration, std::move(destroy) });
}
void GpuLifetime::Retire(std::function<void()> destroy, std::weak_ptr<const void> owner)
{
    std::lock_guard lock(impl->mutex);
    impl->retired.push_back({ impl->currentGeneration, std::move(destroy), std::move(owner), true });
}
void GpuLifetime::BeginGeneration()
{
    std::lock_guard lock(impl->mutex);
    impl->currentGeneration.clear();
}
void GpuLifetime::Collect()
{
    std::lock_guard lock(impl->mutex);
    if (impl->collecting)
        return;
    struct CollectionScope
    {
        bool& active;
        explicit CollectionScope(bool& value) : active(value) { active = true; }
        ~CollectionScope() { active = false; }
    } scope(impl->collecting);
    for (;;)
    {
        std::vector<std::function<void()>> ready;
        std::erase_if(impl->retired, [](const auto& item) { return item.owned && item.owner.expired(); });
        std::erase_if(impl->retired,
                      [&](auto& item)
                      {
                          if (!Impl::Complete(item.uses))
                              return false;
                          ready.push_back(std::move(item.destroy));
                          return true;
                      });
        impl->Compact();
        if (ready.empty())
            break;
        // NGX destruction can re-enter queue/reset hooks and Retire. No callback may
        // run while a retired/recording vector is being compacted or iterated.
        for (auto& destroy : ready)
            destroy();
    }
}
} // namespace DlssNr
