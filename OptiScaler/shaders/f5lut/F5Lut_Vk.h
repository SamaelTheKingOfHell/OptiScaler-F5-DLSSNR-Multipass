#pragma once
#include "F5Lut_Common.h"

#include <shaders/Shader_Vk.h>
#include <shaders/dlssnr/DlssNr_LutVk.h>

#include <memory>

// The F5LUT step of IFeature_Vk's output chain (F5Lut_Common.h), on NR's Vulkan LUT pass (DlssNr_LutVk). The game's
// output can be any format, so the pass writes it through a storage image declared without one where the device allows
// that (shaderStorageImageWriteWithoutFormat), else only an RGBA16F output is graded.
class F5Lut_Vk : public Shader_Vk, public F5Lut_Common
{
  private:
    std::unique_ptr<DlssNr_LutVk> _pass;
    bool _anyFormat = false;
    bool _saidInputs = false; // the first grade logs what it read
    bool _failed = false;     // a grade could not be recorded: out of the chain until the stack changes

  public:
    // Called while the chain is built: whether the step runs this frame (F5Lut_Common::PrepareChain).
    bool ShouldRun(bool InLinearHdr);

    // The image the step before must write to, with the lattice uploaded through InCmdBuffer (recorded ahead of the
    // upscaler). A null image when the step cannot run this frame (it is then left out of the chain).
    VkImageInfo Setup(VkCommandBuffer InCmdBuffer, const VkImageInfo& InNextOutput, VkImageUsageFlags InUsage);

    // Grades InResource into OutResource (GENERAL, as the upscaler's output is). InExposure is the game's 1x1 exposure
    // image view or VK_NULL_HANDLE. Always true: a grade that cannot be recorded takes the step out of the chain.
    bool Dispatch(VkCommandBuffer InCmdBuffer, const VkImageInfo& InResource, const VkImageInfo& OutResource,
                  bool InLinearHdr, VkImageView InExposure, float InPreExposure);

    F5Lut_Vk(std::string InName, VkDevice InDevice, VkPhysicalDevice InPhysicalDevice);
    ~F5Lut_Vk();
};
