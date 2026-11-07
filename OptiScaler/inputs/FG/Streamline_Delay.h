#pragma once
// Native DLSS frame generation one frame late, to match a pipelined neural-rendering picture.
//
// With the network split (or any pipelined second-GPU mode) on the Streamline handoff route, the picture handed to
// DLSS-G at frame N's present is neural rendering's result for frame N-1. DLSS-G then has to interpolate with frame
// N-1's inputs too, or its motion vectors, depth and HUD-less colour describe a different picture than it is given.
//
//   During a frame   the frame-generation tags the game sets (depth, motion vectors, HUD-less colour, UI, ...) and
//                    its constants are recorded. Tags that are only valid now are copied at once on the game's list.
//   At present       the remaining tags (valid until present) are copied on the render queue, into a ring of three
//                    frames. When the picture presented is the previous frame's, DLSS-G is re-tagged with that frame's
//                    copies and given its constants again, before its own present hook runs. Each switch between a
//                    delayed and an undelayed picture sends a history reset, so DLSS-G does not blend across it.
//
// The game's own tags are untouched until present: DLSS ray reconstruction and upscaling read the same depth and
// motion vectors earlier in the frame, and must see the current frame's.
//
// Thread safety: tags and constants arrive on the game's threads, present on the present thread; one mutex.

#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <mutex>
#include <vector>
#include <sl.h>
#include <sl_consts.h>

namespace SlFgDelay
{
using SetTagFn = sl::Result (*)(const sl::ViewportHandle&, const sl::ResourceTag*, uint32_t, sl::CommandBuffer*);
using SetTagForFrameFn = sl::Result (*)(const sl::FrameToken&, const sl::ViewportHandle&, const sl::ResourceTag*,
                                        uint32_t, sl::CommandBuffer*);
using SetConstantsFn = sl::Result (*)(const sl::Constants&, const sl::FrameToken&, const sl::ViewportHandle&);

inline bool IsFgType(sl::BufferType t)
{
    return t == sl::kBufferTypeHUDLessColor || t == sl::kBufferTypeDepth || t == sl::kBufferTypeHiResDepth ||
           t == sl::kBufferTypeLinearDepth || t == sl::kBufferTypeMotionVectors ||
           t == sl::kBufferTypeUIColorAndAlpha || t == sl::kBufferTypeBidirectionalDistortionField;
}

namespace Detail
{
using Microsoft::WRL::ComPtr;
struct Copy
{
    sl::BufferType type = 0;
    ComPtr<ID3D12Resource> resource;
    sl::Extent extent {};
    bool valid = false;
};
struct Frame
{
    std::vector<Copy> copies;
    sl::Constants constants {};
    bool hasConstants = false;
    Copy* Find(sl::BufferType t)
    {
        for (auto& c : copies)
            if (c.type == t)
                return &c;
        return nullptr;
    }
};
// A tag valid until present: copied at present, from the state the game declared.
struct Pending
{
    sl::BufferType type;
    ComPtr<ID3D12Resource> resource;
    uint32_t state;
    sl::Extent extent;
};
struct ListSlot
{
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    uint64_t retired = 0;
};
struct State
{
    std::mutex mutex;
    SetTagFn setTag = nullptr;
    SetTagForFrameFn setTagForFrame = nullptr;
    SetConstantsFn setConstants = nullptr;
    bool enabled = false;
    const sl::FrameToken* token = nullptr;
    bool tokenTags = false; // the game tags per frame token (slSetTagForFrame)
    sl::ViewportHandle viewport { 0 };
    std::array<Frame, 3> ring {};
    unsigned current = 0, frames = 0;
    std::vector<Pending> pending;
    bool wasDelayed = false;
    unsigned delayedPresents = 0, retagFailures = 0;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Fence> fence;
    uint64_t fenceValue = 0;
    std::array<ListSlot, 3> lists {};
    unsigned nextList = 0;
};
inline State& S()
{
    static State* s = new State; // used from hooks that may run during shutdown
    return *s;
}
inline void Barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    if (a == b)
        return;
    D3D12_RESOURCE_BARRIER x {};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    l->ResourceBarrier(1, &x);
}
// This frame's copy of `source` (created or recreated to match it), left in COPY_DEST.
inline Copy* Target(State& s, sl::BufferType type, ID3D12Resource* source, const sl::Extent& extent)
{
    auto& frame = s.ring[s.current];
    Copy* c = frame.Find(type);
    if (!c)
    {
        frame.copies.push_back({});
        c = &frame.copies.back();
        c->type = type;
    }
    const auto want = source->GetDesc();
    if (c->resource)
    {
        const auto have = c->resource->GetDesc();
        if (have.Width != want.Width || have.Height != want.Height || have.Format != want.Format ||
            have.Dimension != want.Dimension || have.MipLevels != want.MipLevels ||
            have.DepthOrArraySize != want.DepthOrArraySize)
            c->resource.Reset();
    }
    if (!c->resource)
    {
        ComPtr<ID3D12Device> device;
        if (FAILED(source->GetDevice(IID_PPV_ARGS(&device))))
            return nullptr;
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        auto desc = want;
        desc.Flags &= ~D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&c->resource))))
            return nullptr;
        if (!s.device)
            s.device = device;
    }
    c->extent = extent;
    c->valid = false;
    return c;
}
inline void RecordCopy(ID3D12GraphicsCommandList* l, Copy& c, ID3D12Resource* source, uint32_t state)
{
    const auto from = static_cast<D3D12_RESOURCE_STATES>(state);
    Barrier(l, source, from, D3D12_RESOURCE_STATE_COPY_SOURCE);
    l->CopyResource(c.resource.Get(), source);
    Barrier(l, source, D3D12_RESOURCE_STATE_COPY_SOURCE, from);
    c.valid = true;
}
} // namespace Detail

// Turns recording on or off (the pipelined handoff route is on). Off clears what was recorded.
inline void SetEnabled(bool on)
{
    auto& s = Detail::S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.enabled == on)
        return;
    s.enabled = on;
    s.pending.clear();
    for (auto& f : s.ring)
    {
        for (auto& c : f.copies)
            c.valid = false;
        f.hasConstants = false;
    }
    s.frames = 0;
}
inline void Register(SetTagFn setTag, SetTagForFrameFn setTagForFrame, SetConstantsFn setConstants)
{
    auto& s = Detail::S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (setTag)
        s.setTag = setTag;
    if (setTagForFrame)
        s.setTagForFrame = setTagForFrame;
    if (setConstants)
        s.setConstants = setConstants;
}

// From slSetTag (token null) and slSetTagForFrame.
inline void OnTags(const sl::FrameToken* token, const sl::ViewportHandle& viewport, const sl::ResourceTag* tags,
                   uint32_t count, sl::CommandBuffer* cmdBuffer)
{
    auto& s = Detail::S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.enabled || !tags)
        return;
    s.viewport = viewport;
    if (token)
    {
        s.token = token;
        s.tokenTags = true;
    }
    auto* list = reinterpret_cast<ID3D12GraphicsCommandList*>(cmdBuffer);
    for (uint32_t i = 0; i < count; ++i)
    {
        const auto& t = tags[i];
        // A copy needs the state the game declared; without one the tag is left to the current frame.
        if (!IsFgType(t.type) || !t.resource || !t.resource->native || t.resource->state == UINT_MAX)
            continue;
        auto* source = reinterpret_cast<ID3D12Resource*>(t.resource->native);
        if (t.lifecycle != sl::eValidUntilPresent && list)
        {
            if (auto* c = Detail::Target(s, t.type, source, t.extent))
                Detail::RecordCopy(list, *c, source, t.resource->state);
            continue;
        }
        bool replaced = false;
        for (auto& p : s.pending)
            if (p.type == t.type)
            {
                p = { t.type, source, t.resource->state, t.extent };
                replaced = true;
            }
        if (!replaced)
            s.pending.push_back({ t.type, source, t.resource->state, t.extent });
    }
}
// From slSetConstants.
inline void OnConstants(const sl::Constants& values, const sl::FrameToken& token, const sl::ViewportHandle& viewport)
{
    auto& s = Detail::S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.enabled)
        return;
    auto& f = s.ring[s.current];
    f.constants = values;
    f.constants.next = nullptr; // chained extensions belong to the game's call
    f.hasConstants = true;
    s.token = &token;
    s.viewport = viewport;
}

// At the game's present, before DLSS-G's own present hook: copy this frame's remaining inputs, then, when the picture
// presented is the previous frame's (`delayed`), hand DLSS-G the previous frame's inputs. Returns false when it could
// not (the caller should reset frame generation's history).
inline bool AtPresent(ID3D12CommandQueue* queue, bool delayed)
{
    auto& s = Detail::S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.enabled || !queue)
        return true;
    bool ok = true;
    // 1. This frame's tags that are valid until present.
    if (!s.pending.empty())
    {
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))))
            return false;
        if (!s.fence && FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.fence))))
            return false;
        auto& slot = s.lists[s.nextList];
        if (!slot.list &&
            (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator))) ||
             FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr,
                                               IID_PPV_ARGS(&slot.list))) ||
             FAILED(slot.list->Close())))
        {
            slot.list.Reset();
            return false;
        }
        if (s.fence->GetCompletedValue() < slot.retired)
            ok = false; // three presents still in flight: skip this frame's copies
        else if (SUCCEEDED(slot.allocator->Reset()) && SUCCEEDED(slot.list->Reset(slot.allocator.Get(), nullptr)))
        {
            for (auto& p : s.pending)
                if (auto* c = Detail::Target(s, p.type, p.resource.Get(), p.extent))
                    Detail::RecordCopy(slot.list.Get(), *c, p.resource.Get(), p.state);
            if (SUCCEEDED(slot.list->Close()))
            {
                ID3D12CommandList* lists[] = { slot.list.Get() };
                queue->ExecuteCommandLists(1, lists);
                queue->Signal(s.fence.Get(), ++s.fenceValue);
                slot.retired = s.fenceValue;
                s.nextList = (s.nextList + 1) % s.lists.size();
            }
        }
        s.pending.clear();
    }
    // 2. A delayed picture: the previous frame's inputs, under this frame's token.
    auto& previous = s.ring[(s.current + s.ring.size() - 1) % s.ring.size()];
    auto& now = s.ring[s.current];
    const bool canDelay = delayed && s.frames > 0 && previous.hasConstants && s.token && s.setConstants &&
                          (s.tokenTags ? s.setTagForFrame != nullptr : s.setTag != nullptr);
    if (delayed && !canDelay)
        ok = false;
    if (canDelay)
    {
        std::vector<sl::Resource> resources;
        std::vector<sl::ResourceTag> tags;
        resources.reserve(previous.copies.size());
        tags.reserve(previous.copies.size());
        for (auto& c : previous.copies)
            if (c.valid && c.resource)
            {
                resources.emplace_back(sl::ResourceType::eTex2d, c.resource.Get(),
                                       static_cast<uint32_t>(D3D12_RESOURCE_STATE_COPY_DEST));
                const auto d = c.resource->GetDesc();
                resources.back().width = static_cast<uint32_t>(d.Width);
                resources.back().height = d.Height;
                resources.back().nativeFormat = d.Format;
                resources.back().mipLevels = d.MipLevels;
                resources.back().arrayLayers = d.DepthOrArraySize;
            }
        size_t r = 0;
        for (auto& c : previous.copies)
            if (c.valid && c.resource)
                tags.emplace_back(&resources[r++], c.type, sl::eValidUntilPresent, &c.extent);
        sl::Result tagged = sl::Result::eOk;
        if (!tags.empty())
            tagged = s.tokenTags ? s.setTagForFrame(*s.token, s.viewport, tags.data(),
                                                    static_cast<uint32_t>(tags.size()), nullptr)
                                 : s.setTag(s.viewport, tags.data(), static_cast<uint32_t>(tags.size()), nullptr);
        auto constants = previous.constants;
        if (!s.wasDelayed)
            constants.reset = sl::Boolean::eTrue; // the first delayed picture: start frame generation's history over
        const auto set = s.setConstants(constants, *s.token, s.viewport);
        if (tagged != sl::Result::eOk || set != sl::Result::eOk)
        {
            ok = false;
            ++s.retagFailures;
        }
        else
            ++s.delayedPresents;
    }
    else if (s.wasDelayed && now.hasConstants && s.token && s.setConstants)
    {
        // Back to the current frame's inputs: start over once more.
        auto constants = now.constants;
        constants.reset = sl::Boolean::eTrue;
        s.setConstants(constants, *s.token, s.viewport);
    }
    s.wasDelayed = canDelay;
    // 3. The next frame records into the next ring entry.
    s.current = (s.current + 1) % s.ring.size();
    for (auto& c : s.ring[s.current].copies)
        c.valid = false;
    s.ring[s.current].hasConstants = false;
    ++s.frames;
    return ok;
}
inline std::pair<unsigned, unsigned> Counts()
{
    auto& s = Detail::S();
    std::lock_guard<std::mutex> lock(s.mutex);
    return { s.delayedPresents, s.retagFailures };
}
} // namespace SlFgDelay
