#pragma once

// The contract between a game-API adapter and the native input producer (DLSS-NR in a game that makes no upscaler call).
//
// The producer (optical flow, trust mask, guides, DLSS-NR) runs on one D3D12 device and knows nothing of the game's API. An
// adapter (Dx12 today; D3D11, D3D10 and D3D9 later) does the rest: it observes how the game uses depth (native/DepthFinderCore.h),
// and once per presented frame it hands the producer a FrameInput, and takes the FrameOutput back. Everything crosses as D3D12
// resources and fence points, never as pointers into the game's API, so the same contract works when the adapter lives in
// another process (the 32-bit bridge) and the resources are shared handles. See docs/NATIVE-INPUT-ADAPTERS.md.

#include <d3d12.h>
#include <dxgi.h>
#include <cstdint>

namespace native
{

enum class Api
{
    D3D9,
    D3D10,
    D3D11,
    D3D12,
    Vulkan
};

// The frame number native input hands to NR (DlssNr::ApplyNativeInput's epoch): the present count of the API the picture
// came from, `dxgiPresents` (State::frameCount) or `vulkanPresents` (State::vulkanPresentCount). Neither a sum nor the
// larger of the two moves by exactly 1 per frame: a dxvk game ticks both, on different threads, and a native Vulkan game
// bridged to frame generation ticks the DXGI one for every generated frame. Detail reuse reads any other step as a
// skipped frame, and a count that stops moving keeps a new NR model on "Preparing".
inline uint64_t PresentEpoch(Api api, uint64_t dxgiPresents, uint64_t vulkanPresents)
{
    return api == Api::Vulkan ? vulkanPresents : dxgiPresents;
}

// What the picture's values mean. Srgb (SDR), ScRgb (linear, 1.0 = 80 nits) and Pq (HDR10). The producer measures
// motion on the values as they are; NR's native input converts Pq to scRGB and back itself.
enum class ColorSpace
{
    Srgb,
    ScRgb,
    Pq
};

// A point on a D3D12 fence: wait until `value` has been reached before using what it guards. A null fence is "already there".
struct SyncPoint
{
    ID3D12Fence* fence = nullptr;
    uint64_t value = 0;
};

// Whether the adapter could read the game's depth at all (D3D9 depth is often not readable). The producer shows it as a
// status line; without depth it runs on motion only.
enum class DepthReadability
{
    Readable,
    NotReadable,
    Unknown
};

struct FrameInput
{
    static constexpr int kMaxDepthCopies = 8;

    Api api = Api::D3D12;
    uint64_t frame = 0; // the adapter's present count

    // The finished picture, as a D3D12 resource the producer may read and write (NR's result is written over it).
    ID3D12Resource* picture = nullptr;
    DXGI_FORMAT pictureFormat = DXGI_FORMAT_UNKNOWN; // a typed format to read it through (the resource may be typeless)
    D3D12_RESOURCE_STATES pictureState = D3D12_RESOURCE_STATE_COMMON; // the state it is in on entry and must be left in
    ColorSpace colorSpace = ColorSpace::Srgb;
    uint32_t width = 0;
    uint32_t height = 0;

    // The scene's depth: one or more copies of the picked buffer (a game that splits the scene over several lists leaves each
    // copy holding part of it; the nearest surface over all of them is the scene). They rest in
    // NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE and are read through depthView.
    ID3D12Resource* depth[kMaxDepthCopies] = {};
    int depthCount = 0;
    DXGI_FORMAT depthView = DXGI_FORMAT_UNKNOWN; // a typed format that reads the depth: R32_FLOAT, R16_UNORM, ...
    uint32_t depthWidth = 0;
    uint32_t depthHeight = 0;
    bool depthReversed = false; // near is 1.0
    DepthReadability depthReadability = DepthReadability::Readable;

    bool cutHint = false; // the adapter knows this frame is not a continuation (a resize, a loading screen)
    SyncPoint ready;      // the producer waits on this before reading `picture` and `depth`
};

struct FrameOutput
{
    SyncPoint done; // the adapter waits on this before it presents `picture` (the same resource, processed in place)
    bool processed = false;
};

// Why Acquire gave no frame.
enum class AcquireStatus
{
    Ready,              // a frame was filled in
    WaitingForUpscaler, // the game calls an upscaler of its own: this is not the case the producer is for (the status line says so)
    Unavailable         // nothing to process now: no picture could be had, a resource could not be shared
};

// An adapter for one game API. All calls come on the thread that presents unless the adapter's own API says otherwise.
class IFrameSource
{
  public:
    virtual ~IFrameSource() = default;

    virtual Api GetApi() const = 0;

    // The D3D12 device and direct queue the producer runs on, valid after an Acquire that returned Ready. For a D3D12
    // game they are the game's own; a D3D11 adapter has a private pair (the picture is shared across to it).
    virtual ID3D12Device* Device() const = 0;
    virtual ID3D12CommandQueue* Queue() const = 0;

    // Once per presented frame, before the overlay is drawn: when a frame is ready, fill `input`. Missing depth is not a
    // failure (the producer then runs the flow only); the game's picture is left alone whenever the status is not Ready.
    virtual AcquireStatus Acquire(FrameInput& input) = 0;

    // The producer finished with the frame it was given. The adapter gets its picture ready for the game's present: for a
    // zero-copy adapter nothing; for a shared-texture adapter, a copy back into the game's back buffer.
    virtual void Return(const FrameInput& input, const FrameOutput& output) = 0;

    // The picture size or the device changed; drop what depends on them.
    virtual void OnResize() = 0;

    // A present the producer does not run on (nothing uses the motion, NativeDriver's Idle), in place of Acquire: what
    // the adapter must do each present anyway. Holds nothing afterwards, and leaves no state of an earlier frame that
    // the present would act on (the Vulkan adapter's present waits). Bridged Vulkan still copies the picture in: the
    // D3D12 swapchain presents it.
    virtual void Skip() = 0;
};

} // namespace native
