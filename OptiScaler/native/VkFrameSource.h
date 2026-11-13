#pragma once

// The Vulkan adapter's frame source: the game's swapchain image is copied into a texture shared with a private D3D12
// device (native/SharedFrameVk.h), the producer works on it there, and the result is copied back before the present.
// Everything is ordered on the GPU through one D3D12 fence opened as a Vulkan timeline semaphore:
//
//   Acquire, on the present's queue:  wait for the present's semaphores -> copy in -> signal N
//   the producer, on the D3D12 queue: wait N -> optical flow, trust mask, DLSS-NR -> (Return) signal N+1
//   Return, on the present's queue:   wait N+1 -> copy back -> signal the semaphore the present now waits on
//
// So the present waits for NR on the GPU, and the CPU waits for nothing (the command ring's slots are a few frames old
// when reused). When the producer gives no result, Return still hands the present a semaphore, signalled after the copy
// in; the picture is then the game's own.
//
// The hooks (hooks/Vulkan_Hooks.cpp) tell it about the devices' queues and the swapchains, since Vulkan cannot be asked
// afterwards which family a queue is of or what a swapchain's images are.

#include "FrameContract.h"
#include "SharedFrameVk.h"

#include <cstdint>
#include <string>
#include <vector>

namespace native
{

class VkFrameSource : public IFrameSource
{
  public:
    ~VkFrameSource() override { Release(); }

    // The present being processed. `waits` are the present's own wait semaphores: when Acquire returns Ready it has
    // waited on them, and the present must wait on PresentWait() instead once Return has run. Call before Acquire().
    void SetPresent(VkDevice device, VkPhysicalDevice physical, VkQueue queue, VkSwapchainKHR swapchain,
                    uint32_t imageIndex, const VkSemaphore* waits, uint32_t waitCount);

    // After Return: the semaphore the present waits on in place of its own list, or VK_NULL_HANDLE when this frame was
    // not acquired (the present keeps its own list then).
    VkSemaphore PresentWait() const { return _presentWait; }

    // Acquire waited on the present's own semaphores: the present must not wait on them again (they are unsignalled
    // now), only on PresentWait(), or on nothing when that is null.
    bool TookPresentWaits() const { return _tookPresentWaits; }

    // Presents through the bridge (native/VkPresentBridge.h): the processed picture goes to a D3D12 swapchain instead of back
    // into the game's image, so Return copies nothing back (its semaphore still lets the game's hidden present go), Acquire
    // copies in even while the game calls an upscaler (the D3D12 swapchain needs the picture all the same) and waits for the
    // D3D12 side to be done with the last picture. Set before SetPresent.
    void SetBridged(bool bridged) { _bridged = bridged; }

    // The bridge's use of a presented frame, after Acquire (and Return): the shared picture, the shared fence and the value
    // the copy in signalled on it, the producer's done point (null fence: no result), and the next fence value for the
    // bridge's own signal, which Acquire then waits for before the next copy in.
    ID3D12Resource* BridgedPicture() const { return _acquiredPicture; }
    ID3D12Fence* BridgedFence() const { return _fence.Fence12(); }
    uint64_t BridgedCopied() const { return _copied; }
    SyncPoint BridgedProducerDone() const { return _producerDone; }
    uint64_t NextFenceValue() { return _fence.Next(); }
    void NoteBridgeDone(uint64_t value) { _bridgeDone = value; }

    // The copy to the output failed: hands the shared picture back (SharedFrameVk.h HandBackPicture) so that the signal the
    // next copy in waits for (`done`) still comes, and only after the producer is finished with the picture. False when
    // there was no picture to hand back (nothing signals `done`, so nothing may wait for it).
    bool HandBackBridged(uint64_t done);

    // Gives the present its semaphore when the producer did not run for it (Return was not called): the copy in was made,
    // the game's own wait list was taken, and the present must still wait on something.
    void FinishPresent();

    // The D3D12 device and queue on the Vulkan device's adapter, made now rather than at the first frame, for the swapchain
    // on the real window. Kept (not released with the Vulkan side) while `pin` is held.
    bool EnsureD3D12(VkPhysicalDevice physical, std::string& why);
    void PinD3D12(bool pin);
    ID3D12Device* Device12() const { return _device12; }
    ID3D12CommandQueue* Queue12() const { return _queue12; }

    Api GetApi() const override { return Api::Vulkan; }
    AcquireStatus Acquire(FrameInput& input) override;
    void Return(const FrameInput& input, const FrameOutput& output) override;
    void OnResize() override {}
    void Skip() override;

    ID3D12Device* Device() const override { return _device12; }
    ID3D12CommandQueue* Queue() const override { return _queue12; }
    const std::string& Error() const { return _error; }
    // Why the present's device was not taken on (a second Vulkan device, or one without the interop functions); empty
    // while the device in use is the one Optical F5Low runs on. For the menu.
    const std::string& Refusal() const { return _refusal; }

    // Everything made on `device` goes (the game is destroying it).
    void OnDeviceDestroyed(VkDevice device);

    // From the hooks. A device's queues by family, as created.
    static void NoteDevice(VkPhysicalDevice physical, VkDevice device, const VkDeviceCreateInfo& info);
    // The device a queue belongs to and the physical device it was made on, as seen at vkCreateDevice. False for a queue
    // that was not seen (the caller keeps what it had).
    static bool DeviceOfQueue(VkQueue queue, VkDevice* device, VkPhysicalDevice* physical);
    // The physical device a Vulkan device was made on; null when it was not seen.
    static VkPhysicalDevice PhysicalDeviceOf(VkDevice device);
    // The usage a swapchain is made with: TRANSFER_SRC and TRANSFER_DST added when the surface allows them and NR is on.
    static VkImageUsageFlags SwapchainUsage(VkPhysicalDevice physical, const VkSwapchainCreateInfoKHR& info);
    // A swapchain was made: its images, format and colour space. The one it replaces is forgotten.
    static void NoteSwapchain(VkDevice device, VkSwapchainKHR swapchain, const VkSwapchainCreateInfoKHR& info);
    // The picture size of a swapchain seen at creation.
    static bool SwapchainExtent(VkSwapchainKHR swapchain, uint32_t* width, uint32_t* height);

  private:
    static constexpr uint32_t kRing = 3;

    struct Slot
    {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer copyIn = VK_NULL_HANDLE;
        VkCommandBuffer copyOut = VK_NULL_HANDLE;
        VkFence done = VK_NULL_HANDLE; // signalled by the slot's last submit
        bool submitted = false;
    };

    bool EnsureDevices(std::string& why);
    bool EnsureRing(uint32_t family);
    void ReleaseRing();
    void WaitRingIdle();
    void ReleaseDepth();
    void Release();

    // The scene's depth for this frame (resource_tracking/GenericDepth_Vk.h): the finder's buffer into _depthShared in
    // the copy-in command buffer, then on D3D12 into _depthTexture. False when the frame has none.
    bool RecordDepth(VkCommandBuffer copyIn, uint32_t family, FrameInput& input);
    // After the copy-in submit: the D3D12 side of RecordDepth, ordered after `copied` on the D3D12 queue.
    void SubmitDepthCopy(uint64_t copied);

    // The present being processed.
    VkDevice _device = VK_NULL_HANDLE;
    VkPhysicalDevice _physical = VK_NULL_HANDLE;
    VkQueue _queue = VK_NULL_HANDLE;
    VkSwapchainKHR _swapchain = VK_NULL_HANDLE;
    uint32_t _imageIndex = 0;
    const VkSemaphore* _waits = nullptr;
    uint32_t _waitCount = 0;

    // Between Acquire and Return.
    bool _acquired = false;
    bool _tookPresentWaits = false;
    VkImage _image = VK_NULL_HANDLE;
    uint32_t _family = 0;
    uint32_t _slot = 0;
    VkSemaphore _presentWait = VK_NULL_HANDLE;

    // The bridged presents.
    bool _bridged = false;
    bool _pinned = false;
    ID3D12Resource* _acquiredPicture = nullptr; // the shared picture of the frame being processed, null if none
    uint64_t _copied = 0;     // the copy in's value on the shared fence
    uint64_t _bridgeDone = 0; // the last value the bridge signalled on it: the next copy in waits for it
    SyncPoint _producerDone;

    // Made on first use, for _interop.device.
    VkInterop _interop;
    ID3D12Device* _device12 = nullptr;
    ID3D12CommandQueue* _queue12 = nullptr;
    SharedFenceVk _fence;
    SharedImageVk _picture;
    Slot _ring[kRing];
    uint32_t _ringFamily = UINT32_MAX;

    // The depth path. Its D3D12 copy has a fence of its own (not the shared one, whose values Vulkan also signals).
    SharedBufferVk _depthShared;
    Microsoft::WRL::ComPtr<ID3D12Resource> _depthTexture;
    VkDepthCopyFormat _depthFormat;
    uint32_t _depthWidth = 0;
    uint32_t _depthHeight = 0;
    bool _depthPending = false; // RecordDepth recorded this frame's copy; SubmitDepthCopy sends the D3D12 half
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> _depthAllocators[kRing];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> _depthList;
    Microsoft::WRL::ComPtr<ID3D12Fence> _depthFence;
    uint64_t _depthFenceValue = 0;
    uint64_t _depthSlotValue[kRing] = {};
    uint64_t _frame = 0;
    std::string _error;
    std::string _refusal;
    VkDevice _refusedDevice = VK_NULL_HANDLE; // logged once
};

} // namespace native
