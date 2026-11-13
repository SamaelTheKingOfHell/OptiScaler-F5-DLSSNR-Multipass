#include "pch.h"

#include "VkFrameSource.h"
#include "DepthFinderCore.h"
#include "VkDeviceRules.h"

#include <dlssnr/DlssNr_ExposureScan_Vk.h>
#include <resource_tracking/GenericDepth_Vk.h>

#include <Config.h>
#include <proxies/D3D12_Proxy.h>
#include <proxies/DXGI_Proxy.h>

#include <format>
#include <mutex>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace native
{

namespace
{

// What the hooks saw being made. Vulkan cannot be asked afterwards which family a queue is of, or what a swapchain's
// images are, without the game's own calls.
struct SwapchainRecord
{
    VkDevice device = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent {};
    VkImageUsageFlags usage = 0;
    std::vector<VkImage> images;
    std::vector<VkSemaphore> presentWaits; // one per image: what its present waits on in place of the game's list
};

// A semaphore of a replaced swapchain: a present may still wait on it, so it goes a few presents later.
struct Retired
{
    VkDevice device = VK_NULL_HANDLE;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    uint64_t after = 0; // the Vulkan present count after which it is destroyed
};

constexpr uint64_t kRetireAfterPresents = 8;

std::mutex g_registryMutex;
// The queues and the physical device of every Vulkan device the game made: a game that makes more devices after its swapchain
// (a probe per feature level, as dxvk does) must be answered per device, not with whichever was made last.
VkDeviceRegistry<VkDevice, VkPhysicalDevice, VkQueue> g_devices;
std::unordered_map<VkSwapchainKHR, SwapchainRecord> g_swapchains;
std::vector<Retired> g_retired;

uint64_t PresentCount() { return State::Instance().vulkanPresentCount.load(std::memory_order_relaxed); }

void RetireLocked(SwapchainRecord& record)
{
    for (auto semaphore : record.presentWaits)
    {
        if (semaphore != VK_NULL_HANDLE)
            g_retired.push_back({ record.device, semaphore, PresentCount() + kRetireAfterPresents });
    }

    record.presentWaits.clear();
}

void DestroyRetiredLocked(bool all, VkDevice onlyDevice = VK_NULL_HANDLE)
{
    const uint64_t now = PresentCount();

    std::erase_if(g_retired,
                  [&](const Retired& r)
                  {
                      if (onlyDevice != VK_NULL_HANDLE && r.device != onlyDevice)
                          return false;

                      if (!all && now < r.after)
                          return false;

                      vkDestroySemaphore(r.device, r.semaphore, nullptr);
                      return true;
                  });
}

ColorSpace ToColorSpace(VkColorSpaceKHR space, VkFormat format)
{
    if (space == VK_COLOR_SPACE_HDR10_ST2084_EXT)
        return ColorSpace::Pq;

    // A float swapchain in SRGB_NONLINEAR holds sRGB-encoded values (Vulkan, unlike DXGI, does not make it scRGB).
    (void) format;

    if (space == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT)
        return ColorSpace::ScRgb;

    return ColorSpace::Srgb;
}

} // namespace

void VkFrameSource::NoteDevice(VkPhysicalDevice physical, VkDevice device, const VkDeviceCreateInfo& info)
{
    std::lock_guard lock(g_registryMutex);
    g_devices.NoteDevice(device, physical);

    for (uint32_t i = 0; i < info.queueCreateInfoCount; ++i)
    {
        const auto& queues = info.pQueueCreateInfos[i];

        // Queues made with flags (protected ones) are only reachable through vkGetDeviceQueue2; the game does not
        // present on those.
        if (queues.flags != 0)
            continue;

        for (uint32_t index = 0; index < queues.queueCount; ++index)
        {
            VkQueue queue = VK_NULL_HANDLE;
            vkGetDeviceQueue(device, queues.queueFamilyIndex, index, &queue);

            if (queue != VK_NULL_HANDLE)
                g_devices.NoteQueue(queue, device, queues.queueFamilyIndex);
        }
    }
}

bool VkFrameSource::DeviceOfQueue(VkQueue queue, VkDevice* device, VkPhysicalDevice* physical)
{
    std::lock_guard lock(g_registryMutex);
    VkDeviceRegistry<VkDevice, VkPhysicalDevice, VkQueue>::Entry entry;

    if (!g_devices.OfQueue(queue, &entry) || entry.physical == VK_NULL_HANDLE)
        return false;

    *device = entry.device;
    *physical = entry.physical;
    return true;
}

VkPhysicalDevice VkFrameSource::PhysicalDeviceOf(VkDevice device)
{
    std::lock_guard lock(g_registryMutex);
    return g_devices.PhysicalOf(device);
}

VkImageUsageFlags VkFrameSource::SwapchainUsage(VkPhysicalDevice physical, const VkSwapchainCreateInfoKHR& info)
{
    constexpr VkImageUsageFlags kCopies = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    // Not gated on DlssNrEnabled: a swapchain's image usage is fixed for its whole life, and this call is the one
    // chance to ask for it. Turning Neural Rendering on later, after the game already made its swapchain without
    // DlssNrEnabled true at that moment, used to mean Optical F5Low could never copy a picture out of it for the
    // rest of that swapchain's life -- NR looked like it did nothing until the game remade its swapchain (a resize,
    // a fullscreen toggle, or a restart). Asking for the two usage bits is free when the surface supports them,
    // which the capability check below already guards.
    if (physical == VK_NULL_HANDLE || (info.imageUsage & kCopies) == kCopies)
        return info.imageUsage;

    VkSurfaceCapabilitiesKHR caps {};

    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, info.surface, &caps) != VK_SUCCESS ||
        (caps.supportedUsageFlags & kCopies) != kCopies)
    {
        LOG_INFO("Native motion (Vulkan): the surface does not allow copies of the swapchain's images (supported usage "
                 "0x{:X}); Optical F5Low cannot run on it",
                 (unsigned) caps.supportedUsageFlags);
        return info.imageUsage;
    }

    return info.imageUsage | kCopies;
}

bool VkFrameSource::SwapchainExtent(VkSwapchainKHR swapchain, uint32_t* width, uint32_t* height)
{
    std::lock_guard lock(g_registryMutex);
    const auto it = g_swapchains.find(swapchain);

    if (it == g_swapchains.end())
        return false;

    *width = it->second.extent.width;
    *height = it->second.extent.height;
    return true;
}

void VkFrameSource::NoteSwapchain(VkDevice device, VkSwapchainKHR swapchain, const VkSwapchainCreateInfoKHR& info)
{
    SwapchainRecord record;
    record.device = device;
    record.format = info.imageFormat;
    record.space = info.imageColorSpace;
    record.extent = info.imageExtent;
    record.usage = info.imageUsage;

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
    record.images.resize(count);
    vkGetSwapchainImagesKHR(device, swapchain, &count, record.images.data());
    record.images.resize(count);
    record.presentWaits.resize(count, VK_NULL_HANDLE);

    std::lock_guard lock(g_registryMutex);

    if (info.oldSwapchain != VK_NULL_HANDLE)
    {
        if (auto it = g_swapchains.find(info.oldSwapchain); it != g_swapchains.end())
        {
            RetireLocked(it->second);
            g_swapchains.erase(it);
        }
    }

    // A handle the driver hands out again replaces whatever was recorded under it.
    if (auto it = g_swapchains.find(swapchain); it != g_swapchains.end())
        RetireLocked(it->second);

    g_swapchains[swapchain] = std::move(record);
}

void VkFrameSource::SetPresent(VkDevice device, VkPhysicalDevice physical, VkQueue queue, VkSwapchainKHR swapchain,
                               uint32_t imageIndex, const VkSemaphore* waits, uint32_t waitCount)
{
    _device = device;
    _physical = physical;
    _queue = queue;
    _swapchain = swapchain;
    _imageIndex = imageIndex;
    _waits = waits;
    _waitCount = waitCount;
}

bool VkFrameSource::EnsureD3D12(VkPhysicalDevice physical, std::string& why)
{
    if (_device12 != nullptr)
        return true;

    LUID luid {};

    if (!VkPhysicalDeviceLuid(physical, luid))
    {
        why = "the Vulkan driver does not say which adapter the device is on";
        return false;
    }

    ScopedSkipSpoofingGlobal skipSpoofing {};
    ScopedSkipVulkanHooks skipVulkanHooks {};

    ComPtr<IDXGIFactory2> factory2;
    HRESULT hr = DxgiProxy::Module() == nullptr
                     ? CreateDXGIFactory2(0, IID_PPV_ARGS(&factory2))
                     : DxgiProxy::CreateDxgiFactory2_()(0, __uuidof(IDXGIFactory2), &factory2);

    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> adapter;

    if (SUCCEEDED(hr))
        hr = factory2.As(&factory);

    if (SUCCEEDED(hr))
        hr = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));

    if (SUCCEEDED(hr))
    {
        hr = D3d12Proxy::Module() == nullptr
                 ? D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&_device12))
                 : D3d12Proxy::D3D12CreateDevice_()(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&_device12));
    }

    if (FAILED(hr) || _device12 == nullptr)
    {
        why = std::format("could not make a D3D12 device on the Vulkan device's adapter (0x{:X})", (unsigned) hr);
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC desc {};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = _device12->CreateCommandQueue(&desc, IID_PPV_ARGS(&_queue12));

    if (FAILED(hr))
    {
        why = std::format("could not make the D3D12 queue (0x{:X})", (unsigned) hr);
        _device12->Release();
        _device12 = nullptr;
        return false;
    }

    LOG_INFO("Native motion (Vulkan): D3D12 device made on the Vulkan device's adapter");
    return true;
}

bool VkFrameSource::EnsureDevices(std::string& why)
{
    // Optical F5Low runs on one Vulkan device. A game that makes more devices (probes, a second window) while this one
    // lives gets them left alone: loading the other device's functions here used to throw away the working interop and
    // then fail on a device that was already gone.
    const auto verdict = JudgeInteropDevice(_interop.device, _device);

    if (verdict == InteropVerdict::Refuse)
    {
        // Remembered until that device is destroyed (OnDeviceDestroyed): a game that really presents from two devices
        // alternates between them every frame, and the log and the menu must not alternate with it.
        if (_refusedDevice != _device)
        {
            _refusedDevice = _device;
            _refusal = "The game also shows pictures from a second graphics device. Optical F5Low stays on the first one.";
            LOG_WARN("Native motion (Vulkan): a present from another Vulkan device ({:X}) is left alone; Optical F5Low "
                     "keeps running on device {:X}",
                     (size_t) _device, (size_t) _interop.device);
        }

        why = _refusal;
        return false;
    }

    if (verdict == InteropVerdict::Load)
    {
        // Loaded into a temporary: a failure leaves whatever is held as it was
        VkInterop loaded;

        if (!loaded.Load(_physical, _device, why))
        {
            _refusal = "This graphics device cannot share its pictures with Optical F5Low.";
            return false;
        }

        Release();
        _interop = loaded;
    }

    // A second device's refusal stays up while that device lives; a failed load on this one is over
    if (_refusedDevice == VK_NULL_HANDLE)
        _refusal.clear();

    if (!EnsureD3D12(_physical, why))
        return false;

    if (!_fence.Ready() && !_fence.Create(_device12, _interop))
    {
        why = _fence.Error();
        return false;
    }

    return true;
}

bool VkFrameSource::EnsureRing(uint32_t family)
{
    if (_ringFamily == family)
        return true;

    ReleaseRing();

    for (auto& slot : _ring)
    {
        VkCommandPoolCreateInfo poolInfo {};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = family;

        VkCommandBuffer buffers[2] {};
        VkCommandBufferAllocateInfo allocate {};
        allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 2;

        VkFenceCreateInfo fenceInfo {};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

        if (vkCreateCommandPool(_device, &poolInfo, nullptr, &slot.pool) != VK_SUCCESS ||
            (allocate.commandPool = slot.pool,
             vkAllocateCommandBuffers(_device, &allocate, buffers) != VK_SUCCESS) ||
            vkCreateFence(_device, &fenceInfo, nullptr, &slot.done) != VK_SUCCESS)
        {
            _error = "could not make the Vulkan command buffers";
            ReleaseRing();
            return false;
        }

        slot.copyIn = buffers[0];
        slot.copyOut = buffers[1];
    }

    _ringFamily = family;
    return true;
}

void VkFrameSource::WaitRingIdle()
{
    for (auto& slot : _ring)
    {
        if (slot.submitted)
        {
            vkWaitForFences(_device, 1, &slot.done, VK_TRUE, UINT64_MAX);
            slot.submitted = false;
        }
    }

    // And the D3D12 side, which may still read the shared picture.
    if (_queue12 != nullptr && _fence.Ready())
    {
        const uint64_t value = _fence.Next();
        _queue12->Signal(_fence.Fence12(), value);

        if (_fence.Fence12()->GetCompletedValue() < value)
        {
            HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            _fence.Fence12()->SetEventOnCompletion(value, event);
            WaitForSingleObject(event, 5000);
            CloseHandle(event);
        }
    }
}

void VkFrameSource::ReleaseRing()
{
    WaitRingIdle();

    for (auto& slot : _ring)
    {
        if (slot.done != VK_NULL_HANDLE)
            vkDestroyFence(_device, slot.done, nullptr);

        if (slot.pool != VK_NULL_HANDLE)
            vkDestroyCommandPool(_device, slot.pool, nullptr);

        slot = Slot {};
    }

    _ringFamily = UINT32_MAX;
}

void VkFrameSource::Release()
{
    if (_interop.device != VK_NULL_HANDLE)
    {
        // The ring and the shared objects belong to the device they were made on, which may not be _device any more.
        const VkDevice current = _device;
        _device = _interop.device;
        ReleaseRing();
        ReleaseDepth();
        _picture.Reset();
        _fence.Reset();
        _device = current;
    }

    // The swapchain on the game's window is made on this device and queue: they stay while the bridge holds them.
    if (!_pinned)
    {
        if (_queue12 != nullptr)
            _queue12->Release();

        if (_device12 != nullptr)
            _device12->Release();

        _queue12 = nullptr;
        _device12 = nullptr;
    }

    _interop = VkInterop {};
    _acquired = false;
    _acquiredPicture = nullptr;
}

void VkFrameSource::PinD3D12(bool pin)
{
    _pinned = pin;

    // Nothing on the Vulkan side uses them: no reason to keep them.
    if (!pin && _interop.device == VK_NULL_HANDLE)
        Release();
}

void VkFrameSource::OnDeviceDestroyed(VkDevice device)
{
    // What this source holds goes only with the device it holds it for (VkDeviceRules.h)
    if (ReleasedWithDevice(device, _interop.device, _device, _device12 != nullptr))
        Release();

    if (_device == device)
        _device = VK_NULL_HANDLE;

    if (_refusedDevice == device)
    {
        _refusedDevice = VK_NULL_HANDLE;
        _refusal.clear();
    }

    std::lock_guard lock(g_registryMutex);

    for (auto it = g_swapchains.begin(); it != g_swapchains.end();)
    {
        if (it->second.device == device)
        {
            RetireLocked(it->second);
            it = g_swapchains.erase(it);
        }
        else
            ++it;
    }

    DestroyRetiredLocked(true, device);
    g_devices.Forget(device);
}

void VkFrameSource::Skip()
{
    // Bridged, as a present with no producer does (NativeMotionVk::OnPresent): the picture is copied in all the same
    if (_bridged)
    {
        FrameInput unused;
        Acquire(unused);
        return;
    }

    // Not bridged: nothing of an earlier frame may reach this present (its waits would be an old, spent semaphore)
    _acquired = false;
    _tookPresentWaits = false;
    _presentWait = VK_NULL_HANDLE;
    _acquiredPicture = nullptr;
    _producerDone = SyncPoint {};
}

AcquireStatus VkFrameSource::Acquire(FrameInput& input)
{
    _acquired = false;
    _tookPresentWaits = false;
    _presentWait = VK_NULL_HANDLE;
    _acquiredPicture = nullptr;
    _producerDone = SyncPoint {};

    if (_device == VK_NULL_HANDLE || _queue == VK_NULL_HANDLE || _swapchain == VK_NULL_HANDLE)
        return AcquireStatus::Unavailable;

    // Bridged, the picture is copied in all the same: the D3D12 swapchain presents it, producer or not.
    const bool waiting = GameUpscalerCalledRecently();

    if (waiting && !_bridged)
        return AcquireStatus::WaitingForUpscaler;

    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent {};
    VkImageUsageFlags usage = 0;
    uint32_t family = UINT32_MAX;

    {
        std::lock_guard lock(g_registryMutex);
        DestroyRetiredLocked(false);

        if (auto it = g_swapchains.find(_swapchain); it != g_swapchains.end() && _imageIndex < it->second.images.size())
        {
            image = it->second.images[_imageIndex];
            format = it->second.format;
            space = it->second.space;
            extent = it->second.extent;
            usage = it->second.usage;
        }

        if (VkDeviceRegistry<VkDevice, VkPhysicalDevice, VkQueue>::Entry entry; g_devices.OfQueue(_queue, &entry))
            family = entry.family;
    }

    if (image == VK_NULL_HANDLE)
    {
        _error = "the swapchain was made before OptiScaler could see it";
        return AcquireStatus::Unavailable;
    }

    constexpr VkImageUsageFlags kCopies = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    if ((usage & kCopies) != kCopies)
    {
        _error = "the swapchain's pictures cannot be copied (the surface does not allow it)";
        return AcquireStatus::Unavailable;
    }

    DXGI_FORMAT dxgiFormat = DXGI_FORMAT_UNKNOWN;
    VkFormat sharedFormat = VK_FORMAT_UNDEFINED;

    if (!VkSharedPictureFormat(format, &dxgiFormat, &sharedFormat))
    {
        _error = std::format("the swapchain's format ({}) is not one Optical F5Low takes", (int) format);
        return AcquireStatus::Unavailable;
    }

    if (family == UINT32_MAX)
    {
        _error = "the present's queue was not seen at device creation";
        return AcquireStatus::Unavailable;
    }

    if (!EnsureDevices(_error))
        return AcquireStatus::Unavailable;

    if (!EnsureRing(family))
        return AcquireStatus::Unavailable;

    if (!_picture.Matches(extent.width, extent.height, sharedFormat))
    {
        WaitRingIdle();

        if (!_picture.Create(_device12, _interop, extent.width, extent.height, dxgiFormat, sharedFormat, true))
        {
            _error = _picture.Error();
            return AcquireStatus::Unavailable;
        }

        LOG_INFO("Native motion (Vulkan): sharing the picture at {}x{}, format {}", extent.width, extent.height,
                 (int) format);
    }

    _slot = (uint32_t) (_frame % kRing);
    Slot& slot = _ring[_slot];

    if (slot.submitted)
    {
        vkWaitForFences(_device, 1, &slot.done, VK_TRUE, UINT64_MAX);
        slot.submitted = false;
    }

    vkResetFences(_device, 1, &slot.done);
    vkResetCommandPool(_device, slot.pool, 0);

    VkCommandBufferBeginInfo begin {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(slot.copyIn, &begin);
    RecordCopyToShared(slot.copyIn, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, _picture, family);
    FrameInput depth;
    const bool haveDepth = !waiting && RecordDepth(slot.copyIn, family, depth);
    // The exposure scan's values, on the game's queue (NR runs on D3D12 here, whose pass writes the frame's bookkeeping)
    DlssNr::ExposureScanVk::Record(slot.copyIn, _device, _physical, true);
    vkEndCommandBuffer(slot.copyIn);

    // The present's semaphores are binary: their values are ignored, but the arrays must match the counts. Bridged, the
    // shared picture is also the D3D12 swapchain's source: the copy in waits until D3D12 is done with the last one.
    const uint64_t copied = _fence.Next();
    std::vector<VkSemaphore> waitSemaphores(_waits, _waits + _waitCount);
    std::vector<uint64_t> waitValues(_waitCount, 0);

    if (_bridged && _bridgeDone != 0)
    {
        waitSemaphores.push_back(_fence.Semaphore());
        waitValues.push_back(_bridgeDone);
    }

    std::vector<VkPipelineStageFlags> waitStages(waitSemaphores.size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    const VkSemaphore signal = _fence.Semaphore();

    VkTimelineSemaphoreSubmitInfo values {};
    values.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    values.waitSemaphoreValueCount = (uint32_t) waitValues.size();
    values.pWaitSemaphoreValues = waitValues.data();
    values.signalSemaphoreValueCount = 1;
    values.pSignalSemaphoreValues = &copied;

    VkSubmitInfo submit {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.pNext = &values;
    submit.waitSemaphoreCount = (uint32_t) waitSemaphores.size();
    submit.pWaitSemaphores = waitSemaphores.data();
    submit.pWaitDstStageMask = waitStages.data();
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &slot.copyIn;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &signal;

    if (const VkResult result = vkQueueSubmit(_queue, 1, &submit, VK_NULL_HANDLE); result != VK_SUCCESS)
    {
        _error = std::format("the copy of the picture could not be submitted ({})", (int) result);
        _depthPending = false;
        return AcquireStatus::Unavailable;
    }

    SubmitDepthCopy(copied);
    DlssNr::ExposureScanVk::Submitted(_device, _fence.Semaphore(), copied);
    _acquired = true;
    _tookPresentWaits = true;
    _error.clear();
    _image = image;
    _family = family;
    _copied = copied;
    _acquiredPicture = _picture.Res12();

    // Bridged and the game calls an upscaler: copied in for the D3D12 swapchain, nothing for the producer.
    // The ring slot follows the frame count: it advances here too, or every such frame waits on the one before it.
    if (waiting)
    {
        ++_frame;
        return AcquireStatus::WaitingForUpscaler;
    }

    input = FrameInput {};
    input.api = Api::Vulkan;
    input.frame = ++_frame;
    input.picture = _picture.Res12();
    input.pictureFormat = dxgiFormat;
    input.pictureState = D3D12_RESOURCE_STATE_COMMON;
    input.colorSpace = ToColorSpace(space, format);
    input.width = extent.width;
    input.height = extent.height;
    input.depthReadability = GenericDepthVk::NotReadableReason() != nullptr ? DepthReadability::NotReadable
                             : GenericDepthVk::Installed()                  ? DepthReadability::Readable
                                                                            : DepthReadability::Unknown;

    if (haveDepth)
    {
        input.depth[0] = depth.depth[0];
        input.depthCount = 1;
        input.depthView = depth.depthView;
        input.depthWidth = depth.depthWidth;
        input.depthHeight = depth.depthHeight;
        input.depthReversed = depth.depthReversed;
    }

    input.ready = SyncPoint { _fence.Fence12(), copied };
    return AcquireStatus::Ready;
}

bool VkFrameSource::RecordDepth(VkCommandBuffer copyIn, uint32_t family, FrameInput& input)
{
    _depthPending = false;

    const auto snap = GenericDepthVk::BestSnapshot();
    VkDepthCopyFormat format;

    if (!snap.valid || !VkDepthCopyFormatOf(snap.format, &format))
        return false;

    const bool sizeChanged = _depthWidth != snap.width || _depthHeight != snap.height ||
                             _depthFormat.texture != format.texture;

    if (_depthShared.Size() < snap.size || sizeChanged || _depthTexture == nullptr)
    {
        // Both may still be read by the D3D12 queue for an earlier frame.
        WaitRingIdle();

        if (_depthShared.Size() < snap.size && !_depthShared.Create(_device12, _interop, snap.size))
        {
            static bool logged = false;

            if (!logged)
            {
                logged = true;
                LOG_WARN("Native motion (Vulkan): sharing the depth failed: {}", _depthShared.Error());
            }

            return false;
        }

        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = snap.width;
        desc.Height = snap.height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format.texture;
        desc.SampleDesc.Count = 1;

        _depthTexture.Reset();

        if (FAILED(_device12->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(&_depthTexture))))
            return false;

        if (_depthList == nullptr)
        {
            for (auto& allocator : _depthAllocators)
            {
                if (FAILED(_device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                             IID_PPV_ARGS(&allocator))))
                    return false;
            }

            if (FAILED(_device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _depthAllocators[0].Get(),
                                                    nullptr, IID_PPV_ARGS(&_depthList))) ||
                FAILED(_device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_depthFence))))
                return false;

            _depthList->Close();
        }

        _depthFormat = format;
        _depthWidth = snap.width;
        _depthHeight = snap.height;
        LOG_INFO("Native motion (Vulkan): sharing the depth at {}x{}, Vulkan format {}", snap.width, snap.height,
                 (int) snap.format);
    }

    // The finder's copy was written in the game's command buffers, before the present's semaphores: visible here.
    const VkBufferCopy whole { 0, 0, snap.size };
    vkCmdCopyBuffer(copyIn, snap.buffer, _depthShared.Buffer(), 1, &whole);

    // Handed to the D3D12 queue, as the picture is.
    VkBufferMemoryBarrier release {};
    release.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    release.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    release.srcQueueFamilyIndex = family;
    release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    release.buffer = _depthShared.Buffer();
    release.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(copyIn, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 1,
                         &release, 0, nullptr);

    _depthPending = true;
    input.depth[0] = _depthTexture.Get();
    input.depthView = format.view;
    input.depthWidth = snap.width;
    input.depthHeight = snap.height;
    input.depthReversed = snap.reversed;
    return true;
}

void VkFrameSource::SubmitDepthCopy(uint64_t copied)
{
    if (!_depthPending)
        return;

    _depthPending = false;

    // The slot's allocator is free once its last copy has run.
    if (_depthFence->GetCompletedValue() < _depthSlotValue[_slot])
    {
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        _depthFence->SetEventOnCompletion(_depthSlotValue[_slot], event);
        WaitForSingleObject(event, 5000);
        CloseHandle(event);
    }

    auto* allocator = _depthAllocators[_slot].Get();
    allocator->Reset();
    _depthList->Reset(allocator, nullptr);

    const auto toCopy = [&](D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
    {
        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = _depthTexture.Get();
        barrier.Transition.StateBefore = from;
        barrier.Transition.StateAfter = to;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        _depthList->ResourceBarrier(1, &barrier);
    };

    constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    toCopy(kRead, D3D12_RESOURCE_STATE_COPY_DEST);
    RecordBufferToDepthTexture(_depthList.Get(), _depthShared.Res12(), _depthTexture.Get(), _depthFormat, _depthWidth,
                               _depthHeight);
    toCopy(D3D12_RESOURCE_STATE_COPY_DEST, kRead);
    _depthList->Close();

    // Before the producer's work on the same queue, which waits on the same point.
    _queue12->Wait(_fence.Fence12(), copied);
    ID3D12CommandList* lists[] = { _depthList.Get() };
    _queue12->ExecuteCommandLists(1, lists);
    _depthSlotValue[_slot] = ++_depthFenceValue;
    _queue12->Signal(_depthFence.Get(), _depthFenceValue);
}

void VkFrameSource::ReleaseDepth()
{
    if (_depthFence != nullptr && _depthFence->GetCompletedValue() < _depthFenceValue)
    {
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        _depthFence->SetEventOnCompletion(_depthFenceValue, event);
        WaitForSingleObject(event, 5000);
        CloseHandle(event);
    }

    _depthShared.Reset();
    _depthTexture.Reset();
    _depthList.Reset();

    for (auto& allocator : _depthAllocators)
        allocator.Reset();

    _depthFence.Reset();
    _depthFenceValue = 0;

    for (auto& value : _depthSlotValue)
        value = 0;

    _depthWidth = _depthHeight = 0;
    _depthFormat = VkDepthCopyFormat {};
}

void VkFrameSource::Return(const FrameInput& input, const FrameOutput& output)
{
    (void) input;

    if (!_acquired)
        return;

    _acquired = false;
    Slot& slot = _ring[_slot];

    VkSemaphore presentWait = VK_NULL_HANDLE;

    {
        std::lock_guard lock(g_registryMutex);

        if (auto it = g_swapchains.find(_swapchain); it != g_swapchains.end() &&
                                                     _imageIndex < it->second.presentWaits.size())
        {
            VkSemaphore& semaphore = it->second.presentWaits[_imageIndex];

            if (semaphore == VK_NULL_HANDLE)
            {
                VkSemaphoreCreateInfo info {};
                info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
                vkCreateSemaphore(_device, &info, nullptr, &semaphore);
            }

            presentWait = semaphore;
        }
    }

    // The producer's done point is on its own fence, which Vulkan cannot see: a signal of ours on the same D3D12 queue,
    // right after its work, stands for it.
    // Bridged, the processed picture stays on the D3D12 side for the swapchain: nothing is copied back, the present only
    // waits for the copy in.
    if (_bridged)
        _producerDone = output.done;

    const bool processed = !_bridged && output.done.fence != nullptr && _queue12 != nullptr;
    const uint64_t done = processed ? _fence.Next() : 0;

    if (processed)
    {
        _queue12->Signal(_fence.Fence12(), done);

        VkCommandBufferBeginInfo begin {};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(slot.copyOut, &begin);
        RecordCopyFromShared(slot.copyOut, _picture, _image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, _family);
        vkEndCommandBuffer(slot.copyOut);
    }

    // Without a result the submit carries no work: its signal still comes after the copy in, earlier on this queue.
    const VkSemaphore timeline = _fence.Semaphore();
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const uint64_t binaryValue = 0;

    VkTimelineSemaphoreSubmitInfo values {};
    values.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    values.waitSemaphoreValueCount = processed ? 1 : 0;
    values.pWaitSemaphoreValues = &done;
    values.signalSemaphoreValueCount = presentWait != VK_NULL_HANDLE ? 1 : 0;
    values.pSignalSemaphoreValues = &binaryValue;

    VkSubmitInfo submit {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.pNext = &values;
    submit.waitSemaphoreCount = processed ? 1 : 0;
    submit.pWaitSemaphores = &timeline;
    submit.pWaitDstStageMask = &stage;
    submit.commandBufferCount = processed ? 1 : 0;
    submit.pCommandBuffers = &slot.copyOut;
    submit.signalSemaphoreCount = presentWait != VK_NULL_HANDLE ? 1 : 0;
    submit.pSignalSemaphores = &presentWait;

    const VkResult result = vkQueueSubmit(_queue, 1, &submit, slot.done);
    slot.submitted = result == VK_SUCCESS;

    if (result != VK_SUCCESS)
    {
        static bool logged = false;

        if (!logged)
        {
            logged = true;
            LOG_ERROR("Native motion (Vulkan): the copy back could not be submitted ({})", (int) result);
        }

        presentWait = VK_NULL_HANDLE;
    }

    _presentWait = presentWait;
}

bool VkFrameSource::HandBackBridged(uint64_t done)
{
    // _acquiredPicture is cleared by every Acquire and set only once the copy in was submitted
    if (!_bridged || _acquiredPicture == nullptr || _queue12 == nullptr || _fence.Fence12() == nullptr)
        return false;

    HandBackPicture(_queue12, _fence.Fence12(), _copied, _producerDone.fence, _producerDone.value, done);
    return true;
}

void VkFrameSource::FinishPresent()
{
    if (_acquired)
        Return(FrameInput {}, FrameOutput {});
}

} // namespace native
