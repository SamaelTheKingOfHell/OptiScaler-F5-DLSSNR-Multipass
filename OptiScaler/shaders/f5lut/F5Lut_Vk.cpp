#include "pch.h"

#include "F5Lut_Vk.h"

#include <dlssnr/DlssNr_VkExtensions.h>

bool F5Lut_Vk::ShouldRun(bool InLinearHdr)
{
    if (!_init)
        return false;

    const std::string key = _chain.key;
    const bool run = PrepareChain(InLinearHdr);
    if (_chain.key != key)
        _failed = false; // a changed stack is tried again
    if (!_chain.wanted && _pass && _pass->HoldsImage())
    {
        // Earlier frames may still read the lattice; one drain, then every frame without a stack costs nothing. Like
        // NR's Vulkan LUT, this drains from the render thread, only when the stack changes.
        vkDeviceWaitIdle(_device);
        _pass->ReleaseLutImage();
    }
    return run && !_failed;
}

VkImageInfo F5Lut_Vk::Setup(VkCommandBuffer InCmdBuffer, const VkImageInfo& InNextOutput, VkImageUsageFlags InUsage)
{
    if (!_anyFormat && InNextOutput.Format != VK_FORMAT_R16G16B16A16_SFLOAT)
    {
        Report(false, "Not applied: this output's format needs a Vulkan device feature the game did not turn on.");
        return {};
    }

    if (!_pass)
        _pass = std::make_unique<DlssNr_LutVk>(_device, _physicalDevice, _anyFormat);

    const DlssNr::Lut3D& lut = _chain.baked;
    if (_pass->Ready() && _pass->NeedsDrainFor(lut.size, _chain.key))
        vkDeviceWaitIdle(_device);

    if (!_pass->Ready() || !_pass->EnsureLutImage(InCmdBuffer, lut.rgb.data(), lut.size, _chain.key))
    {
        Report(false, "could not upload the LUT to the GPU");
        return {};
    }

    if (!CreateImageResource(InNextOutput.Width, InNextOutput.Height, InNextOutput.Format, InUsage))
        return {};

    SetImageLayout(InCmdBuffer, GetImage(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                   InNextOutput.SubresourceRange);

    VkImageInfo info = InNextOutput;
    info.Image = GetImage();
    info.ImageView = GetImageView();
    return info;
}

bool F5Lut_Vk::Dispatch(VkCommandBuffer InCmdBuffer, const VkImageInfo& InResource, const VkImageInfo& OutResource,
                        bool InLinearHdr, VkImageView InExposure, float InPreExposure)
{
    if (!_init || !_pass || InCmdBuffer == VK_NULL_HANDLE)
        return false;

    SetImageLayout(InCmdBuffer, InResource.Image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   InResource.SubresourceRange);

    // The game's exposure is read as an upscaler input, in SHADER_READ_ONLY_OPTIMAL (as RCAS reads the game's motion
    // and NR's Vulkan LUT its exposure); an implausible texel leaves the white point at 1.0 (dlssnr_whitepoint.hlsli).
    const bool exposure = InLinearHdr && InExposure != VK_NULL_HANDLE;
    if (!_saidInputs)
    {
        _saidInputs = true;
        LOG_INFO("[{}] Vulkan: HDR {}, game exposure {}, PreExposure {}", _name, InLinearHdr,
                 exposure ? "read" : "none (white point 1.0)", InPreExposure);
    }
    const DlssNrLutConstants constants =
        ChainConstants(OutResource.Width, OutResource.Height, InLinearHdr, exposure, InPreExposure);
    const bool graded = _pass->Dispatch(InCmdBuffer, InResource.ImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                        OutResource.ImageView, constants, exposure ? InExposure : VK_NULL_HANDLE,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!graded)
    {
        // The game's output did not get this frame. Copying our image into it would need transfer use the game may not
        // have given it, so the step leaves the chain instead: from the next frame the upscaler writes the output
        // itself again (one stale frame, not an upscaler failure).
        _failed = true;
        LOG_ERROR("[{}] the Vulkan grade could not be recorded; F5LUT stands aside until the stack changes", _name);
        Report(false, "The Vulkan grade failed; F5LUT stands aside until the stack changes.");
        return true;
    }

    Report(true);
    return true;
}

F5Lut_Vk::F5Lut_Vk(std::string InName, VkDevice InDevice, VkPhysicalDevice InPhysicalDevice)
    : Shader_Vk(InName, InDevice, InPhysicalDevice)
{
    if (InDevice == VK_NULL_HANDLE || InPhysicalDevice == VK_NULL_HANDLE)
        return;

    _anyFormat = DlssNr::VkExt::WritesWithoutFormat(InDevice);
    _init = true;
}

F5Lut_Vk::~F5Lut_Vk()
{
    if (!_pass || State::Instance().isShuttingDown)
    {
        _pass.release();
        return;
    }

    // The lattice and rings may still be read by frames in flight; the pass frees them as it goes.
    vkDeviceWaitIdle(_device);
}
