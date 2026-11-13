#pragma once

// The LUT-apply epic's grade pass on Vulkan (precompile/dlssnr_lut.hlsl, SPIR-V).
//
// Self-contained rather than a mode of DlssNr_Vk: that class has a fixed seven-binding layout built around
// dlssnr.hlsl, and this shader is the first of the family to sample a 3D texture. Its own five bindings (constants,
// source, LUT, sampler, target), its own pipeline and its own rings keep every other pass's layout untouched, as the
// D3D12 side does with its own root signature.
//
// Nothing here exists until a LutFile is set: the owner constructs this lazily, and the pipeline itself is only built
// on the first dispatch. A failure to build is logged once and never retried; the owner then goes on ungraded.
//
// Lifetime. The 3D image and the constants/descriptor rings are read by frames still in flight. A slot is never
// rewritten within kFramesInFlight frames of its last use, and the 3D image (and the staging buffer its upload read
// from) is only replaced or freed through ReleaseLutImage/Destroy, which the owner calls after vkDeviceWaitIdle.

#include "SysUtils.h"
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

#include "DlssNr_LutConstants.h"

class DlssNr_LutVk
{
    // A constant slot and its descriptor set must not be rewritten while a frame still in flight reads them. One grade
    // per frame today; two slots per frame leaves room for a second without a rewrite.
    static constexpr uint32_t kSlotsPerFrame = 2;
    static constexpr uint32_t kFramesInFlight = 3;
    static constexpr uint32_t kSlots = kSlotsPerFrame * kFramesInFlight;

    // Dispatches after an upload by which its staging buffer is certainly retired (far past any frame queue depth).
    static constexpr uint32_t kStagingRetireDispatches = 16;

    VkDevice _device = VK_NULL_HANDLE;
    VkPhysicalDevice _physicalDevice = VK_NULL_HANDLE;
    bool _anyFormatTarget = false;

    // Pipeline and its rings. Built together on the first Dispatch.
    bool _built = false;
    bool _buildFailed = false;
    VkDescriptorSetLayout _setLayout = VK_NULL_HANDLE;
    VkPipelineLayout _pipelineLayout = VK_NULL_HANDLE;
    VkPipeline _pipeline = VK_NULL_HANDLE;
    VkSampler _sampler = VK_NULL_HANDLE;
    VkDescriptorPool _pool = VK_NULL_HANDLE;
    VkDescriptorSet _sets[kSlots] = {};
    VkBuffer _constants = VK_NULL_HANDLE;
    VkDeviceMemory _constantsMemory = VK_NULL_HANDLE;
    void* _constantsMapped = nullptr;
    VkDeviceSize _slotStride = 0;
    uint32_t _slot = 0;

    // The uploaded lattice. Keyed on the loaded path as well as the size: two same-size .cube files must not read as
    // "already uploaded".
    VkImage _lutImage = VK_NULL_HANDLE;
    VkDeviceMemory _lutMemory = VK_NULL_HANDLE;
    VkImageView _lutView = VK_NULL_HANDLE;
    int _lutSize = 0;
    std::string _lutPath;
    std::string _failedKey; // path and size of an upload that failed, so it is not retried every frame

    // The upload's source. Kept until its copy has certainly executed, then freed (it can be ~16 MB).
    VkBuffer _staging = VK_NULL_HANDLE;
    VkDeviceMemory _stagingMemory = VK_NULL_HANDLE;
    uint32_t _dispatchesSinceUpload = 0;

    bool Build();
    void FreeStaging();
    uint32_t FindMemory(uint32_t typeBits, VkMemoryPropertyFlags properties) const;
    bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer* buffer,
                      VkDeviceMemory* memory) const;

  public:
    // `anyFormatTarget`: the target is not our RGBA16F image but one of any format (the post-NR stack grades the
    // game's own image in place); the shader then declares no storage format, which the device must allow
    // (shaderStorageImageWriteWithoutFormat, DlssNr::VkExt::WritesWithoutFormat).
    DlssNr_LutVk(VkDevice device, VkPhysicalDevice physicalDevice, bool anyFormatTarget = false);
    ~DlssNr_LutVk();

    // True once the pipeline and rings exist; builds them on first use. False, and not tried again, if that failed.
    bool Ready();

    // Makes the 3D image hold `rgb` (size^3 triples, red fastest), recording the upload into `cmd` when it has to. A
    // no-op when the image already holds this path at this size. Replacing an image the GPU may still be reading
    // needs the device idle, so the caller checks NeedsDrainFor first and waits. False if the image could not be
    // made (logged once per path and size).
    bool EnsureLutImage(VkCommandBuffer cmd, const float* rgb, int size, const std::string& path);

    // Whether EnsureLutImage(path, size) would replace an existing image, in which case the caller drains the device
    // first.
    bool NeedsDrainFor(int size, const std::string& path) const;

    bool HoldsImage() const { return _lutImage != VK_NULL_HANDLE || _staging != VK_NULL_HANDLE; }

    // Frees the 3D image and the staging buffer (LutFile cleared). The device must be idle.
    void ReleaseLutImage();

    // Grades `sourceView` (in `sourceLayout`) into `targetView` (GENERAL; RGBA16F unless anyFormatTarget) and records
    // the barrier that makes the result readable by the next compute dispatch. `constants` is copied into the next ring
    // slot. `exposureView` is the live 1x1 exposure the encode reads, when its constants say it is used
    // (VK_NULL_HANDLE: none bound).
    bool Dispatch(VkCommandBuffer cmd, VkImageView sourceView, VkImageLayout sourceLayout, VkImageView targetView,
                  const DlssNrLutConstants& constants, VkImageView exposureView = VK_NULL_HANDLE,
                  VkImageLayout exposureLayout = VK_IMAGE_LAYOUT_GENERAL);

    // Frees everything. The device must be idle.
    void Destroy();
};
