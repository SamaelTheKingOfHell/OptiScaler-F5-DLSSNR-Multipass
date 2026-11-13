#include "pch.h"

#include "DlssNr_LutVk.h"

#include "precompile/dlssnr_lut_Shader_Vk.h"
#include "precompile/dlssnr_lut_anyformat_Shader_Vk.h"
#include <dlssnr/DlssNr_LutPack.h>

#include <algorithm>
#include <cstring>

DlssNr_LutVk::DlssNr_LutVk(VkDevice device, VkPhysicalDevice physicalDevice, bool anyFormatTarget)
    : _device(device), _physicalDevice(physicalDevice), _anyFormatTarget(anyFormatTarget)
{
}

DlssNr_LutVk::~DlssNr_LutVk() { Destroy(); }

uint32_t DlssNr_LutVk::FindMemory(uint32_t typeBits, VkMemoryPropertyFlags properties) const
{
    VkPhysicalDeviceMemoryProperties memProps {};
    vkGetPhysicalDeviceMemoryProperties(_physicalDevice, &memProps);

    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
    {
        if ((typeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    }

    return UINT32_MAX;
}

bool DlssNr_LutVk::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
                                VkBuffer* buffer, VkDeviceMemory* memory) const
{
    VkBufferCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(_device, &info, nullptr, buffer) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req {};
    vkGetBufferMemoryRequirements(_device, *buffer, &req);

    VkMemoryAllocateInfo alloc {};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = FindMemory(req.memoryTypeBits, properties);

    if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(_device, &alloc, nullptr, memory) != VK_SUCCESS ||
        vkBindBufferMemory(_device, *buffer, *memory, 0) != VK_SUCCESS)
    {
        if (*memory != VK_NULL_HANDLE)
            vkFreeMemory(_device, *memory, nullptr);

        vkDestroyBuffer(_device, *buffer, nullptr);
        *buffer = VK_NULL_HANDLE;
        *memory = VK_NULL_HANDLE;
        return false;
    }

    return true;
}

bool DlssNr_LutVk::Build()
{
    if (_built)
        return true;

    if (_buildFailed || _device == VK_NULL_HANDLE || _physicalDevice == VK_NULL_HANDLE)
        return false;

    const auto fail = [this](const char* what)
    {
        _buildFailed = true; // Destroy() leaves this set, so the build is not retried
        LOG_WARN("DLSS-NR Vulkan: the LUT pass's {} could not be built; LutFile is ignored", what);
        Destroy();
        return false;
    };

    // Bindings as dlssnr_lut.hlsl declares them under VK_MODE: constants, source, LUT, sampler, target, and the live
    // exposure the encode reads (the source again when there is none; the shader then does not read it). The texture
    // and sampler are separate descriptors because the shader declares them separately.
    VkDescriptorSetLayoutBinding bindings[6] {};
    const VkDescriptorType types[6] = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER,
                                        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE };
    for (uint32_t i = 0; i < 6; ++i)
    {
        bindings[i].binding = i;
        bindings[i].descriptorType = types[i];
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 6;
    layoutInfo.pBindings = bindings;

    if (vkCreateDescriptorSetLayout(_device, &layoutInfo, nullptr, &_setLayout) != VK_SUCCESS)
        return fail("descriptor set layout");

    VkPipelineLayoutCreateInfo pipelineLayoutInfo {};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &_setLayout;

    if (vkCreatePipelineLayout(_device, &pipelineLayoutInfo, nullptr, &_pipelineLayout) != VK_SUCCESS)
        return fail("pipeline layout");

    // The cube's own lattice is the whole point of the sampler: linear between lattice points, clamped at the edge.
    VkSamplerCreateInfo samplerInfo {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;

    if (vkCreateSampler(_device, &samplerInfo, nullptr, &_sampler) != VK_SUCCESS)
        return fail("sampler");

    // Constant ring: a uniform buffer binding can only be offset to the device's alignment, so the stride is the
    // struct rounded up to it (256 already satisfies every alignment a device reports).
    VkPhysicalDeviceProperties props {};
    vkGetPhysicalDeviceProperties(_physicalDevice, &props);

    const VkDeviceSize alignment = std::max<VkDeviceSize>(props.limits.minUniformBufferOffsetAlignment, 1);
    _slotStride = ((sizeof(DlssNrLutConstants) + alignment - 1) / alignment) * alignment;

    if (!CreateBuffer(_slotStride * kSlots, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &_constants,
                      &_constantsMemory) ||
        vkMapMemory(_device, _constantsMemory, 0, _slotStride * kSlots, 0, &_constantsMapped) != VK_SUCCESS)
        return fail("constant ring");

    const VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kSlots },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3 * kSlots },
        { VK_DESCRIPTOR_TYPE_SAMPLER, kSlots },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kSlots },
    };

    VkDescriptorPoolCreateInfo poolInfo {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kSlots;
    poolInfo.poolSizeCount = 4;
    poolInfo.pPoolSizes = poolSizes;

    if (vkCreateDescriptorPool(_device, &poolInfo, nullptr, &_pool) != VK_SUCCESS)
        return fail("descriptor pool");

    // One set per slot, so two dispatches in flight never share bindings.
    VkDescriptorSetLayout layouts[kSlots];
    std::fill(std::begin(layouts), std::end(layouts), _setLayout);

    VkDescriptorSetAllocateInfo allocInfo {};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = _pool;
    allocInfo.descriptorSetCount = kSlots;
    allocInfo.pSetLayouts = layouts;

    if (vkAllocateDescriptorSets(_device, &allocInfo, _sets) != VK_SUCCESS)
        return fail("descriptor sets");

    VkShaderModuleCreateInfo moduleInfo {};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = _anyFormatTarget ? sizeof(dlssnr_lut_anyformat_spv) : sizeof(dlssnr_lut_spv);
    moduleInfo.pCode = reinterpret_cast<const uint32_t*>(_anyFormatTarget ? dlssnr_lut_anyformat_spv : dlssnr_lut_spv);

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(_device, &moduleInfo, nullptr, &module) != VK_SUCCESS)
        return fail("shader module");

    VkComputePipelineCreateInfo pipelineInfo {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = module;
    pipelineInfo.stage.pName = "CSMain";
    pipelineInfo.layout = _pipelineLayout;

    const VkResult created = vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &_pipeline);
    vkDestroyShaderModule(_device, module, nullptr);

    if (created != VK_SUCCESS)
        return fail("compute pipeline");

    _built = true;
    return true;
}

bool DlssNr_LutVk::Ready() { return Build(); }

void DlssNr_LutVk::FreeStaging()
{
    if (_staging != VK_NULL_HANDLE)
        vkDestroyBuffer(_device, _staging, nullptr);

    if (_stagingMemory != VK_NULL_HANDLE)
        vkFreeMemory(_device, _stagingMemory, nullptr);

    _staging = VK_NULL_HANDLE;
    _stagingMemory = VK_NULL_HANDLE;
}

bool DlssNr_LutVk::NeedsDrainFor(int size, const std::string& path) const
{
    if (_lutImage == VK_NULL_HANDLE && _staging == VK_NULL_HANDLE)
        return false;

    return _lutImage == VK_NULL_HANDLE || _lutSize != size || _lutPath != path;
}

void DlssNr_LutVk::ReleaseLutImage()
{
    if (_device == VK_NULL_HANDLE)
        return;

    if (_lutView != VK_NULL_HANDLE)
        vkDestroyImageView(_device, _lutView, nullptr);

    if (_lutImage != VK_NULL_HANDLE)
        vkDestroyImage(_device, _lutImage, nullptr);

    if (_lutMemory != VK_NULL_HANDLE)
        vkFreeMemory(_device, _lutMemory, nullptr);

    FreeStaging();

    _lutView = VK_NULL_HANDLE;
    _lutImage = VK_NULL_HANDLE;
    _lutMemory = VK_NULL_HANDLE;
    _lutSize = 0;
    _lutPath.clear();
    _failedKey.clear();
}

bool DlssNr_LutVk::EnsureLutImage(VkCommandBuffer cmd, const float* rgb, int size, const std::string& path)
{
    if (cmd == VK_NULL_HANDLE || rgb == nullptr || size < 2 || _device == VK_NULL_HANDLE)
        return false;

    if (_lutImage != VK_NULL_HANDLE && _lutSize == size && _lutPath == path)
        return true; // already uploaded, and it is still this exact file

    const std::string key = path + "#" + std::to_string(size);
    if (key == _failedKey)
        return false;

    // The caller drained the device if this replaces an image (NeedsDrainFor).
    ReleaseLutImage();

    const auto fail = [&](const char* why)
    {
        LOG_ERROR("DLSS-NR Vulkan: the LUT image ({0}x{0}x{0}) could not be made: {1}; LutFile is ignored", size, why);
        ReleaseLutImage();
        _failedKey = key;
        return false;
    };

    // Full float texels, read point by point: the shader interpolates them itself (dlssnr_lut.hlsl LutTrilinear), so
    // the device needs no linear filtering of this format.
    const VkFormat format = VK_FORMAT_R32G32B32A32_SFLOAT;

    VkFormatProperties formatProps {};
    vkGetPhysicalDeviceFormatProperties(_physicalDevice, format, &formatProps);
    const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((formatProps.optimalTilingFeatures & needed) != needed)
        return fail("RGBA32F cannot be read as a 3D image on this device");

    VkImageFormatProperties imageProps {};
    if (vkGetPhysicalDeviceImageFormatProperties(_physicalDevice, format, VK_IMAGE_TYPE_3D, VK_IMAGE_TILING_OPTIMAL,
                                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0,
                                                 &imageProps) != VK_SUCCESS ||
        imageProps.maxExtent.width < (uint32_t) size || imageProps.maxExtent.height < (uint32_t) size ||
        imageProps.maxExtent.depth < (uint32_t) size)
        return fail("the device does not support a 3D image this large");

    VkImageCreateInfo imageInfo {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_3D;
    imageInfo.format = format;
    imageInfo.extent = { (uint32_t) size, (uint32_t) size, (uint32_t) size };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(_device, &imageInfo, nullptr, &_lutImage) != VK_SUCCESS)
        return fail("vkCreateImage");

    VkMemoryRequirements req {};
    vkGetImageMemoryRequirements(_device, _lutImage, &req);

    VkMemoryAllocateInfo alloc {};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(_device, &alloc, nullptr, &_lutMemory) != VK_SUCCESS ||
        vkBindImageMemory(_device, _lutImage, _lutMemory, 0) != VK_SUCCESS)
        return fail("device memory");

    VkImageViewCreateInfo viewInfo {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = _lutImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
    viewInfo.format = format;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    if (vkCreateImageView(_device, &viewInfo, nullptr, &_lutView) != VK_SUCCESS)
        return fail("image view");

    // Tightly packed float4 texels, w = 1.0, red fastest -- exactly a 3D image's own order.
    const VkDeviceSize bytes = (VkDeviceSize) DlssNrLutPack::TightBytesFloat4(size);

    void* mapped = nullptr;
    if (!CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &_staging,
                      &_stagingMemory) ||
        vkMapMemory(_device, _stagingMemory, 0, bytes, 0, &mapped) != VK_SUCCESS)
        return fail("staging buffer");

    const size_t rowPitch = (size_t) size * 4u * sizeof(float);
    DlssNrLutPack::PackFloat4(rgb, size, static_cast<uint8_t*>(mapped), rowPitch, rowPitch * (size_t) size);
    vkUnmapMemory(_device, _stagingMemory);

    VkImageMemoryBarrier toCopy {};
    toCopy.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toCopy.srcAccessMask = 0;
    toCopy.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toCopy.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toCopy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toCopy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toCopy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toCopy.image = _lutImage;
    toCopy.subresourceRange = viewInfo.subresourceRange;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &toCopy);

    VkBufferImageCopy region {};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { (uint32_t) size, (uint32_t) size, (uint32_t) size };
    vkCmdCopyBufferToImage(cmd, _staging, _lutImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier toRead = toCopy;
    toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &toRead);

    _lutSize = size;
    _lutPath = path;
    _dispatchesSinceUpload = 0;
    return true;
}

bool DlssNr_LutVk::Dispatch(VkCommandBuffer cmd, VkImageView sourceView, VkImageLayout sourceLayout,
                            VkImageView targetView, const DlssNrLutConstants& constants, VkImageView exposureView,
                            VkImageLayout exposureLayout)
{
    if (cmd == VK_NULL_HANDLE || sourceView == VK_NULL_HANDLE || targetView == VK_NULL_HANDLE ||
        _lutView == VK_NULL_HANDLE || !Build())
        return false;

    // The staging buffer's copy was recorded kStagingRetireDispatches frames ago; it has executed.
    if (_staging != VK_NULL_HANDLE && ++_dispatchesSinceUpload > kStagingRetireDispatches)
        FreeStaging();

    const uint32_t slot = _slot;
    _slot = (_slot + 1) % kSlots;

    const VkDeviceSize offset = _slotStride * slot;
    std::memcpy(static_cast<char*>(_constantsMapped) + offset, &constants, sizeof(DlssNrLutConstants));

    const VkDescriptorBufferInfo bufferInfo { _constants, offset, sizeof(DlssNrLutConstants) };
    const VkDescriptorImageInfo sourceInfo { VK_NULL_HANDLE, sourceView, sourceLayout };
    const VkDescriptorImageInfo lutInfo { VK_NULL_HANDLE, _lutView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    const VkDescriptorImageInfo samplerInfo { _sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    const VkDescriptorImageInfo targetInfo { VK_NULL_HANDLE, targetView, VK_IMAGE_LAYOUT_GENERAL };
    const VkDescriptorImageInfo exposureInfo { VK_NULL_HANDLE,
                                               exposureView != VK_NULL_HANDLE ? exposureView : sourceView,
                                               exposureView != VK_NULL_HANDLE ? exposureLayout : sourceLayout };

    const VkWriteDescriptorSet writes[] = {
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _sets[slot], 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
          nullptr, &bufferInfo, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _sets[slot], 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          &sourceInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _sets[slot], 2, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          &lutInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _sets[slot], 3, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,
          &samplerInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _sets[slot], 4, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          &targetInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _sets[slot], 5, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          &exposureInfo, nullptr, nullptr },
    };
    vkUpdateDescriptorSets(_device, 6, writes, 0, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _pipelineLayout, 0, 1, &_sets[slot], 0, nullptr);

    // [numthreads(8, 8, 1)]
    vkCmdDispatch(cmd, (constants.Width + 7) / 8, (constants.Height + 7) / 8, 1);

    // The graded image is read by the next dispatch (the encode); a missing barrier shows up as a frame of stale
    // grade, not as an error.
    VkMemoryBarrier barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                         &barrier, 0, nullptr, 0, nullptr);

    return true;
}

void DlssNr_LutVk::Destroy()
{
    if (_device == VK_NULL_HANDLE)
        return;

    ReleaseLutImage();

    if (_pipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(_device, _pipeline, nullptr);

    if (_pool != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(_device, _pool, nullptr); // frees its sets

    if (_constantsMapped != nullptr)
        vkUnmapMemory(_device, _constantsMemory);

    if (_constants != VK_NULL_HANDLE)
        vkDestroyBuffer(_device, _constants, nullptr);

    if (_constantsMemory != VK_NULL_HANDLE)
        vkFreeMemory(_device, _constantsMemory, nullptr);

    if (_sampler != VK_NULL_HANDLE)
        vkDestroySampler(_device, _sampler, nullptr);

    if (_pipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(_device, _pipelineLayout, nullptr);

    if (_setLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(_device, _setLayout, nullptr);

    _pipeline = VK_NULL_HANDLE;
    _pool = VK_NULL_HANDLE;
    _constantsMapped = nullptr;
    _constants = VK_NULL_HANDLE;
    _constantsMemory = VK_NULL_HANDLE;
    _sampler = VK_NULL_HANDLE;
    _pipelineLayout = VK_NULL_HANDLE;
    _setLayout = VK_NULL_HANDLE;
    std::fill(std::begin(_sets), std::end(_sets), VK_NULL_HANDLE);
    _slot = 0;
    _built = false;
}
