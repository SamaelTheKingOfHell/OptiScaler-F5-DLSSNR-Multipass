#pragma once

// The composition pass on Vulkan.
//
// Same shader as the D3D12 pass, compiled to SPIR-V from the same source: the modes, the white point,
// the proxy encode and the transfer back are all shared, and any behavioural difference between the
// two APIs would be a bug rather than a design. What differs is only how a compute dispatch is
// expressed.
//
// Three things are worth knowing before reading the implementation.
//
// Every binding is written every dispatch. The shader declares all seven resources at file scope and
// branches on gMode, so all of them are statically reachable from the entry point and Vulkan requires
// a valid descriptor for each one whether a given mode reads it or not. Slots a mode has no use for
// get a 1x1 dummy rather than a null handle.
//
// Constants are slotted rather than overwritten. A single mapped uniform buffer would be wrong here:
// encode and resolve run in the same frame with different constants, and the second write would land
// before the first dispatch had read it. The buffer holds a ring of slots and each dispatch takes the
// next, at an offset the device's own alignment rule allows.
//
// Layouts are the caller's to declare and this pass's to respect. It never guesses what state an
// image arrived in.

#include "SysUtils.h"
#include <shaders/Shader_Vk.h>
#include "DlssNr_Common.h"

class DlssNr_Vk : public Shader_Vk
{
    // Enough slots for several dispatches per frame across the frames that can be in flight: a slot's
    // constants and descriptor set must not be rewritten while a frame still in flight reads them. Encode
    // and resolve are two; the meter, its reduce, eye adaptation, the exposure courier, the downsample and,
    // during a Tune run, two copies and the stats pass are the others -- about ten, so sixteen.
    static constexpr uint32_t kSlotsPerFrame = 16;
    static constexpr uint32_t kFramesInFlight = 3;
    static constexpr uint32_t kSlots = kSlotsPerFrame * kFramesInFlight;

    VkDeviceSize _slotStride = 0;   // sizeof(DlssNrConstants), rounded up to the device's alignment
    uint32_t _slot = 0;             // next slot to hand out, wrapping

    // Stands in for a resource a given mode does not read. One pixel, never sampled for its content,
    // present only because Vulkan will not accept an unwritten binding.
    VkImage _dummyImage = VK_NULL_HANDLE;
    VkDeviceMemory _dummyMemory = VK_NULL_HANDLE;
    VkImageView _dummyView = VK_NULL_HANDLE;
    bool _dummyReady = false;

    bool CreateDummy(VkCommandBuffer cmdList);

    void WriteDescriptors(VkDescriptorSet set, VkDeviceSize constantOffset, VkImageView source, VkImageView model,
                          VkImageView original, VkImageView motion, VkImageView target, VkImageView keep,
                          VkImageLayout sourceLayout, VkImageLayout motionLayout, VkImageView depth);

    // Takes a constant slot and its descriptor set, binds `pipeline`, dispatches and puts the barrier after it.
    bool Record(VkCommandBuffer cmdList, VkPipeline pipeline, const DlssNrConstants& constants, uint32_t groupsX,
                uint32_t groupsY, VkImageView source, VkImageView model, VkImageView original, VkImageView motion,
                VkImageView target, VkImageView keep, VkImageLayout sourceLayout, VkImageLayout motionLayout,
                VkImageView depth = VK_NULL_HANDLE);

    // Automatic exposure's eye adaptation (dlssnr_exposure_adapt.hlsl, SPIR-V), on this pass's pipeline layout. Built on
    // first use; not retried once it failed, and the meter then writes the exposure directly, as before.
    VkPipeline _adaptPipeline = VK_NULL_HANDLE;
    bool _adaptFailed = false;

    // "Tune for this scene"'s stats pass (dlssnr_detail_stats.hlsl, SPIR-V), the same way.
    VkPipeline _statsPipeline = VK_NULL_HANDLE;
    bool _statsFailed = false;

    // Builds `pipeline` from `code` on this pass's layout on first use; false, and not tried again, if it fails.
    bool EnsurePipeline(VkPipeline& pipeline, bool& failed, const unsigned char* code, size_t size, const char* what);

  public:
    DlssNr_Vk(std::string InName, VkDevice InDevice, VkPhysicalDevice InPhysicalDevice);
    ~DlssNr_Vk();

    // One dispatch of the composition shader.
    //
    // Any of the four read views may be VK_NULL_HANDLE, in which case the dummy is bound; the two
    // written views may not, because a mode that writes nothing has no reason to run. This records
    // the dispatch and the barrier that follows it, not the transitions that got them there.
    //
    // The source and motion slots are the two that ever carry an image this pass does not own -- the
    // frame the upscaler wrote, and the game's exposure -- and a descriptor has to name the layout
    // its image will be in when the shader runs. Those two are therefore the caller's to state.
    // Everything else is ours and is in the layout this pass put it in.
    //
    // The default is what a resource read by a compute shader is normally in, and is what every slot
    // holding one of our own images uses.
    bool Dispatch(VkCommandBuffer InCmdList, const DlssNrConstants& InConstants, uint32_t InThreadsX,
                  uint32_t InThreadsY, VkImageView InSource, VkImageView InModel, VkImageView InOriginal,
                  VkImageView InMotion, VkImageView InTarget, VkImageView InKeep,
                  VkImageLayout InSourceLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VkImageLayout InMotionLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  // The game's depth for the resolve's depth fade (binding 8): a depth-only view, in
                  // SHADER_READ_ONLY_OPTIMAL. Null binds the placeholder, which is never read.
                  VkImageView InDepth = VK_NULL_HANDLE);

    // Automatic exposure's eye adaptation (DlssNr_ExposureAdapt.h): one thread eases the 1x1 InEased (GENERAL) toward
    // the 1x1 InReading (SHADER_READ_ONLY_OPTIMAL). ExposureAdaptReady builds the pipeline on first use and says
    // whether there is one; DispatchExposureAdapt is false (no-op) without it.
    bool ExposureAdaptReady();
    bool DispatchExposureAdapt(VkCommandBuffer InCmdList, const DlssNrConstants& InConstants, VkImageView InReading,
                               VkImageView InEased);

    // One stats pass of "Tune for this scene" (dlssnr_detail_stats.hlsl): 64x64 groups, one per tile, into the
    // 256x64 RGBA32F InGrid (GENERAL). The four copies are read sampled (SHADER_READ_ONLY_OPTIMAL); InProxy, the
    // RGBA16F picture the model was shown, is read as a storage image (GENERAL) in gKeep's binding, the layout having
    // only four sampled ones. False (no-op) if the pipeline cannot be built.
    bool DispatchDetailStats(VkCommandBuffer InCmdList, const DlssNrConstants& InConstants, VkImageView InOutput,
                             VkImageView InPrevOutput, VkImageView InInput, VkImageView InPrevInput,
                             VkImageView InProxy, VkImageView InGrid);

    // The same shader's copy mode (Mode 1): InConstants.Width x Height texels of InSource (in InSourceLayout) into the
    // RGBA16F InTarget (GENERAL), as they are. InGrid (GENERAL) is bound, not touched.
    bool DispatchStatsCopy(VkCommandBuffer InCmdList, const DlssNrConstants& InConstants, VkImageView InSource,
                           VkImageLayout InSourceLayout, VkImageView InTarget, VkImageView InGrid);
};
