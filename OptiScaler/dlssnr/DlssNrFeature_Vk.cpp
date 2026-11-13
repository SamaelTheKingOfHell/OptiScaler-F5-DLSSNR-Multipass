#include "pch.h"
#include "DlssNr_SuperiorScaling.h"

#include "DlssNrFeature_Vk.h"
#include "DlssNrFeature_Dx12.h"
#include "PassProfiles.h"

#include <Config.h>
#include <State.h>
#include <Util.h>
#include <NVNGX_Parameter.h>

#include <shaders/dlssnr/DlssNr_Vk.h>
#include <shaders/dlssnr/DlssNr_LutVk.h>
#include <dlssnr/DlssNr_Lut.h>
#include <dlssnr/DlssNr_LutChain.h>
#include <dlssnr/DlssNr_LutStatus.h>
#include <shaders/dlssnr/DlssNr_Guides.h>
#include <shaders/dlssnr/DlssNr_GameScale.h>
#include <shaders/dlssnr/DlssNr_TrimAnchors.h>
#include <shaders/dlssnr/DlssNr_AutoTrimDefault.h>
#include <shaders/dlssnr/DlssNr_FollowGame.h>
#include <shaders/dlssnr/DlssNr_ExposureAdapt.h>
#include <shaders/dlssnr/DlssNr_ExposureFoundBlend.h>
#include <shaders/dlssnr/DlssNr_ExposureMeter.h>
#include <shaders/dlssnr/DlssNr_ExposureCalibrate_Run.h>
#include <dlssnr/DlssNrNative.h>
#include <dlssnr/DlssNrVitReuse.h>
#include <dlssnr/DlssNr_ExposureScan_Vk.h>
#include <dlssnr/DlssNr_GameDefaults.h>
#include <dlssnr/DlssNr_VkExtensions.h>
#include <dlssnr/DlssNrDetailReuseHost.h>
#include <shaders/dlssnr/DlssNrDetailReuse_Vk.h>
#include <shaders/dlssnr/DlssNrSpatial_Vk.h>
#include <shaders/dlssnr/DlssNr_Spatial.h>
#include <shaders/output_scaling/OS_Vk.h>
#include <shaders/sgsr1/SGSR1_Vk.h>

#include <dlssnr/DlssNr_RetryRequest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include "../shaders/dlssnr/DlssNr_ColourEncoding.h"
#include "../shaders/dlssnr/DlssNr_ProxyCurve.h"
#include "DlssNr_ColourEncodingStatus.h"

namespace DlssNr
{

namespace
{

// The forwarder's Vulkan surface. The model checks its caller's module path and requires nvngx.dll in
// it, whichever API is being used, so these calls go through the same shim the D3D12 path does.
using PFN_VkProbe = int(__cdecl*)(const wchar_t*);
using PFN_VkInit = int(__cdecl*)(const wchar_t*, const wchar_t*, void*, void*, void*, int);
using PFN_VkCreate = void*(__cdecl*)(void*, void*, unsigned int, unsigned int, int, float, int, float, float, float,
                                     int, int);
using PFN_VkEvaluate = int(__cdecl*)(void*, void*, void*, void*, void*, void*, void*, unsigned int, unsigned int,
                                     unsigned int, unsigned int, unsigned int, unsigned int, unsigned int, unsigned int,
                                     unsigned int, unsigned int, int, int, float, int, float, float, float, int, float,
                                     float);
using PFN_VkRelease = void(__cdecl*)(void*);
using PFN_VkShutdown = int(__cdecl*)(void*);
using PFN_VkForget = void(__cdecl*)();
// The parameter block's float setter: the same probe the D3D12 path runs (DlssNr_Dx12.cpp, DiscoverFloatSlot).
using PFN_VkSetFloatSlot = void(__cdecl*)(int);
using PFN_VkProbeFloat = void(__cdecl*)(void*, const char*, float, int);

// One image this pass owns: the storage, the view, and the NGX wrapper that describes it. Kept
// together because they are created, resized and destroyed as one thing.
struct OwnedImage
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    NVSDK_NGX_Resource_VK ngx {};
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;

    bool Valid() const { return image != VK_NULL_HANDLE && view != VK_NULL_HANDLE; }
};

struct VkState
{
    bool failed = false;
    const char* reason = "";

    HMODULE forwarder = nullptr;
    PFN_VkProbe probe = nullptr;
    PFN_VkInit init = nullptr;
    PFN_VkCreate create = nullptr;
    PFN_VkEvaluate evaluate = nullptr;
    PFN_VkRelease release = nullptr;
    PFN_VkShutdown shutdown = nullptr; // optional: forwarders before the Vulkan init fix lack these two
    PFN_VkForget forget = nullptr;
    PFN_VkSetFloatSlot setFloatSlot = nullptr;
    PFN_VkProbeFloat probeFloat = nullptr;
    bool floatSlotKnown = false;

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;

    bool ngxInitialised = false;
    // The parameter block came from the game's NGX core (not OptiScaler's own table), so destroying it needs the
    // core to still be up.
    bool paramsFromCore = false;
    // Devices lost under NR this session. A re-init after one is the case nothing has tested yet.
    unsigned deviceLosses = 0;
    void* feature = nullptr;
    void* laterFeatures[DlssNr::MaxPassCount] {};
    Profiles::NrPassTuning builtTuning[DlssNr::MaxPassCount] {};
    unsigned int builtPreset[DlssNr::MaxPassCount] {};
    unsigned int builtStyle[DlssNr::MaxPassCount] {};
    unsigned int activePasses = 0;
    VkEvent creationReady = VK_NULL_HANDLE;
    bool creationPending = false;
    NVSDK_NGX_Parameter* capabilityParams = nullptr;

    // What the model writes, the proxy it is shown, and the frame as the upscaler left it.
    OwnedImage output;
    OwnedImage scratch;
    OwnedImage proxy;
    OwnedImage keep;
    OwnedImage preColor;
    bool beforeSr = false;
    bool beforeSrPlacement = false; // before SR without a Tune moving it (EvaluateBeforeUpscaleVk)
    bool rayReconstruction = false;

    // The proxy at the model's working size, when that is below the frame. The model -- 98% of the
    // cost -- then runs on this instead of the full proxy, which is the whole point of the working
    // scale slider. Unused (and never created) at scale 1, so the default path is unchanged.
    OwnedImage proxySmall;

    // Supersampling (working scale > 1): the model runs above native, superUp enlarges the proxy to
    // that size and superDown averages the answer (output) back into outputNative at native for a 1:1
    // composite. nrScaler is the filter both were built with, so a changed DlssNrScalingDownscaler
    // rebuilds them. Unused and never created at scale <= 1.
    OwnedImage outputNative;
    std::unique_ptr<OS_Vk> superUp;
    std::unique_ptr<OS_Vk> superDown;
    Scaler nrScaler = Scaler::Count;

    // Reduced up-leg (working scale < 1, DlssNrReducedUpscaleMethod == 1): the DX12-side mirror
    // of this is NrState::sgsr1UpAnswer. outputNative above covers the answer side, shared with
    // the >1 supersample leg. Built for one Dispatch()/frame, for the same reason superUp/
    // superDown are already two OS_Vk instances: a single instance would have both CPU-side
    // constant writes land before either GPU dispatch executes.
    std::unique_ptr<SGSR1_Vk> sgsr1UpAnswer;

    std::unique_ptr<DlssNr_Vk> pass;

    // The LUT-apply epic: [DlssNr] LutFile grades the frame the encode reads, into lutScratch, so the model sees the
    // grade while the meter above it and the untouched game frame in `keep` do not. Nothing here exists until a
    // LutFile is set -- the pass is built on first use and the scratch is freed again when the file is cleared.
    DlssNr_LutState lut;
    std::unique_ptr<DlssNr_LutVk> lutPass;
    OwnedImage lutScratch;
    bool lutScratchFailed = false;

    // The post-NR LUT stack (memory/plans/2026-10-31-nr-lut-chain.md): its layers baked into one lattice, and a pass of
    // its own (its own 3D image) so the Inputs LUT's and this one's lattices never replace each other. Graded in place
    // on the picture NR just wrote. Nothing exists until the stack holds a file.
    DlssNr_LutChainState postLut;
    std::unique_ptr<DlssNr_LutVk> postLutPass;

    // Experimental input styles (DlssNr_InputStyles.h): the model's picture restyled, standing in for the model input and
    // the resolve's proxy; styledB is the second image they alternate with when more than one is on. Made on first use at
    // the model picture's size; freed with the other sized images.
    OwnedImage styled;
    OwnedImage styledB;

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t workWidth = 0;
    uint32_t workHeight = 0;
    bool reset = true;
    unsigned long long frames = 0;

    // Timing. A pair of timestamps per frame across a ring, read back three frames later: a query
    // read the frame it was written stalls the CPU on the GPU, which would cost more than the pass
    // it is measuring. Vulkan reports ticks, and timestampPeriod is how many nanoseconds a tick is.
    VkQueryPool queryPool = VK_NULL_HANDLE;
    float timestampPeriod = 0.0f;
    unsigned long long timedFrames = 0;
    std::optional<double> lastGpuTime;

    // Whether the game hands over an exposure texture, and what it said when it did.
    bool exposureOffered = false;

    // The game's own exposure, read off its 1x1 texture, and the scale it multiplied its buffer by.
    //
    // gameExposure holds its last good value rather than resetting when a frame arrives without a
    // texture: GTA V dropped it three times in one session on the D3D12 path, and falling back to a
    // default on those frames is a flicker, not a fallback.
    float gameExposure = 0.0f;
    float gamePreExposure = 1.0f;

    // Whether the game's exposure image reads cleanly through the courier's layout guess, so the encode, resolve and LUT
    // may read it live in the shader (DlssNr_ExposureState.h LiveGameGate), as D3D12 does, instead of the host value a
    // few frames behind. Counted on Game exposure's own readbacks; started over when the source changes.
    DlssNrExposureState::LiveGameGate gameLiveGate;
    bool gameLiveSaid = false;

    // The exposure's courier: an 8x8 R32_FLOAT image the meter writes, and a ring of host-visible
    // buffers it is copied into. Only texel (0,0) is ever read -- the rest of the grid belongs to the
    // frame-statistics meter that was removed from the shared shader, and 8x8 is here only so that a
    // single 8x8 thread group lands entirely inside the image.
    OwnedImage meter;
    VkBuffer meterReadback[4] = {};
    VkDeviceMemory meterReadbackMemory[4] = {};
    void* meterMapped[4] = {};
    unsigned long long meterFrames = 0;

    // Automatic exposure (white point source 3): the 64x64 meter's tile means reduced on the GPU to a
    // 1x1 image the encode and resolve read the same frame, bound in the motion slot. Its value also
    // rides home on the meter's ring, for the menu and for capturing Trim anchors only.
    OwnedImage autoExposure;
    bool autoExposureActive = false; // written this frame, so the dispatches may bind it
    float autoExposureValue = 0.0f;
    float autoExposurePreExposure = 1.0f;
    unsigned long long autoExposureFrames = 0;

    // Eye adaptation (shaders/dlssnr/DlssNr_ExposureAdapt.h): the meter's own reading lands in autoExposureRaw and a
    // one-texel pass eases autoExposure toward it, so autoExposure above is the eased value and everything downstream
    // reads that. The reading rides home in a slot's third float (`meterHasRaw`), for the log.
    // A "Tune for this scene" run evaluates the first pass alone (DlssNr_ExposureCalibrate_Vk.inl); the later passes
    // skipped frames meanwhile, so their history starts over when they come back.
    bool laterPassesNeedReset = false;

    OwnedImage autoExposureRaw;
    float autoExposureRawValue = 0.0f;
    bool autoExposureAdapting = false;
    bool meterHasRaw[4] = {};
    DlssNrExposureAdapt::Adapter autoExposureAdapter;

    // Which source last fed the ring, so one source's numbers are never read as another's, and what
    // each slot holds: 0 nothing believable, 1 the game's exposure, 2 the automatic one.
    uint32_t exposureReadbackSource = 0;
    uint32_t meterExposureKind[4] = {};
    float meterExposurePreExposure[4] = {};

    // Automatic following the game's own exposure on an unexposed frame (shaders/dlssnr/DlssNr_FollowGame.h). A slot
    // written with `meterPairHasGame` also carries the game's exposure in its second float, read from the game's image
    // through the same layout guess Game exposure uses -- so each value is checked on the host before it counts, and
    // `pairReads`/`pairValid` say how often the guess held. Following works from the host value, a few frames behind the
    // game: this backend has no live path for the game's own image.
    bool meterPairHasGame[4] = {};
    float pairGameExposure = 0.0f;
    float pairPreExposure = 1.0f;
    unsigned long long pairReads = 0;
    unsigned long long pairValid = 0;
    unsigned long long pairValidAt = 0; // meterFrames when the last valid game value arrived
    bool pairValiditySaid = false;
    bool followingGame = false;
    // Auto's found exposure (DlssNr_ExposureScan.h FoundExposure): the base white point it gives this frame, 0 when
    // none is in force. Read back a few frames behind, so while it holds the host's white point is in force. Eased in
    // and out (DlssNr_ExposureFoundBlend.h).
    float foundBaseWhitePoint = 0.0f;
    DlssNrExposureFoundBlend::Blend foundBlend;
};

// The grid the meter writes, and the size of one readback. 64 * 64 * sizeof(float). The game's
// exposure only ever uses texel (0,0); automatic exposure needs the whole 64x64 grid of tile means.
constexpr uint32_t kMeterSide = 64;
constexpr VkDeviceSize kMeterBytes = kMeterSide * kMeterSide * sizeof(float);

// Four, so the slot being read is four frames behind the slot being written and the read never waits
// on the GPU. Same depth as the D3D12 meter's ring, for the same reason.
constexpr unsigned long long kMeterSlots = 4;

// Four frames of pairs. Three would do, four keeps the modulo cheap and the slot being written well
// clear of the slot being read.
constexpr uint32_t kTimingSlots = 4;

VkState g_vk;
std::mutex g_vkMutex;

// Set while this thread holds g_vkMutex in the evaluate path. A device or NGX shutdown reached from inside it (the
// model, the NGX core) must not take the lock again.
thread_local bool t_vkLockHeld = false;

struct VkLockMark
{
    VkLockMark() { t_vkLockHeld = true; }
    ~VkLockMark() { t_vkLockHeld = false; }
};

// What a retry cannot fix: the model has no usable Vulkan surface, or NGX will not start on this device. Anything else
// (a file that was missing, a surface that could not be allocated, a failed dispatch) a retry may.
bool g_vkFailurePermanent = false;
DlssNr::RetryRequest g_vkRetry;
unsigned int g_vkRetryHandled = 0;

void Fail(const char* why, bool permanent = false)
{
    if (g_vk.failed)
        return;

    g_vk.failed = true;
    g_vk.reason = why;
    g_vkFailurePermanent = permanent;
    LOG_ERROR("DLSS-NR Vulkan unavailable: {}", why);
}

// ---------------------------------------------------------------------------------------------
// Images this pass owns
// ---------------------------------------------------------------------------------------------

void DestroyImage(OwnedImage& img)
{
    if (g_vk.device == VK_NULL_HANDLE)
        return;

    if (img.view != VK_NULL_HANDLE)
        vkDestroyImageView(g_vk.device, img.view, nullptr);

    if (img.image != VK_NULL_HANDLE)
        vkDestroyImage(g_vk.device, img.image, nullptr);

    if (img.memory != VK_NULL_HANDLE)
        vkFreeMemory(g_vk.device, img.memory, nullptr);

    img = OwnedImage {};
}

uint32_t FindMemoryTypeIndex(uint32_t typeBits, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProps {};
    vkGetPhysicalDeviceMemoryProperties(g_vk.physicalDevice, &memProps);

    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
    {
        if ((typeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    }

    return UINT32_MAX;
}

// STORAGE and SAMPLED both, because every one of these is written by one dispatch and read by the
// next; TRANSFER_SRC so a capture can copy it out without a second surface.
// Build the OS_Vk resample descriptor for one of our own images. OS_Vk reads Width/Height/Format from
// this (the NR override makes it size from the images, not the current feature).
static VkImageInfo ImageInfoOf(const OwnedImage& img)
{
    VkImageInfo info {};
    info.ImageView = img.view;
    info.Image = img.image;
    info.SubresourceRange = VkImageSubresourceRange { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    info.Format = img.format;
    info.Width = img.width;
    info.Height = img.height;
    return info;
}

bool CreateImage(OwnedImage& img, uint32_t width, uint32_t height, VkFormat format, bool readWrite)
{
    DestroyImage(img);

    VkImageCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = { width, height, 1 };
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(g_vk.device, &info, nullptr, &img.image) != VK_SUCCESS)
    {
        LOG_ERROR("DLSS-NR Vulkan: could not create a {}x{} image", width, height);
        return false;
    }

    VkMemoryRequirements req {};
    vkGetImageMemoryRequirements(g_vk.device, img.image, &req);

    VkMemoryAllocateInfo alloc {};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = FindMemoryTypeIndex(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (alloc.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(g_vk.device, &alloc, nullptr, &img.memory) != VK_SUCCESS ||
        vkBindImageMemory(g_vk.device, img.image, img.memory, 0) != VK_SUCCESS)
    {
        LOG_ERROR("DLSS-NR Vulkan: could not back a {}x{} image", width, height);
        DestroyImage(img);
        return false;
    }

    VkImageViewCreateInfo view {};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = img.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    if (vkCreateImageView(g_vk.device, &view, nullptr, &img.view) != VK_SUCCESS)
    {
        LOG_ERROR("DLSS-NR Vulkan: could not view a {}x{} image", width, height);
        DestroyImage(img);
        return false;
    }

    img.width = width;
    img.height = height;
    img.format = format;
    img.layout = VK_IMAGE_LAYOUT_UNDEFINED;

    // The NGX wrapper. Filled once, because none of it changes until the image is recreated.
    img.ngx.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
    img.ngx.Resource.ImageViewInfo.ImageView = img.view;
    img.ngx.Resource.ImageViewInfo.Image = img.image;
    img.ngx.Resource.ImageViewInfo.SubresourceRange = view.subresourceRange;
    img.ngx.Resource.ImageViewInfo.Format = format;
    img.ngx.Resource.ImageViewInfo.Width = width;
    img.ngx.Resource.ImageViewInfo.Height = height;
    img.ngx.ReadWrite = readWrite;

    return true;
}

// The ring of host-visible buffers the meter's grid is copied into, created once and mapped for
// good. HOST_COHERENT so the read needs no invalidate; it is universally available for a buffer this
// small and the alternative is a vkInvalidateMappedMemoryRanges on a path that runs every frame.
bool CreateMeterReadback()
{
    for (unsigned long long i = 0; i < kMeterSlots; ++i)
    {
        if (g_vk.meterReadback[i] != VK_NULL_HANDLE)
            continue;

        VkBufferCreateInfo info {};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = kMeterBytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(g_vk.device, &info, nullptr, &g_vk.meterReadback[i]) != VK_SUCCESS)
        {
            LOG_WARN("DLSS-NR Vulkan: could not create the exposure readback buffer");
            return false;
        }

        VkMemoryRequirements req {};
        vkGetBufferMemoryRequirements(g_vk.device, g_vk.meterReadback[i], &req);

        VkMemoryAllocateInfo alloc {};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = FindMemoryTypeIndex(
            req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if (alloc.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(g_vk.device, &alloc, nullptr, &g_vk.meterReadbackMemory[i]) != VK_SUCCESS ||
            vkBindBufferMemory(g_vk.device, g_vk.meterReadback[i], g_vk.meterReadbackMemory[i], 0) != VK_SUCCESS ||
            vkMapMemory(g_vk.device, g_vk.meterReadbackMemory[i], 0, kMeterBytes, 0, &g_vk.meterMapped[i]) !=
                VK_SUCCESS)
        {
            LOG_WARN("DLSS-NR Vulkan: could not back the exposure readback buffer");
            return false;
        }
    }

    return true;
}

void DestroyMeterReadback()
{
    for (unsigned long long i = 0; i < kMeterSlots; ++i)
    {
        if (g_vk.meterReadbackMemory[i] != VK_NULL_HANDLE)
        {
            if (g_vk.meterMapped[i] != nullptr)
                vkUnmapMemory(g_vk.device, g_vk.meterReadbackMemory[i]);

            vkFreeMemory(g_vk.device, g_vk.meterReadbackMemory[i], nullptr);
        }

        if (g_vk.meterReadback[i] != VK_NULL_HANDLE)
            vkDestroyBuffer(g_vk.device, g_vk.meterReadback[i], nullptr);

        g_vk.meterMapped[i] = nullptr;
        g_vk.meterReadbackMemory[i] = VK_NULL_HANDLE;
        g_vk.meterReadback[i] = VK_NULL_HANDLE;
    }

    g_vk.meterFrames = 0;
    // The next meter reads a new image (device rebuilt, or the game's in another layout): prove it again
    g_vk.gameLiveGate.Reset();
    g_vk.gameLiveSaid = false;
}

// A layout transition with the access masks that go with it. Vulkan has no equivalent of D3D12's
// state promotion, so every read and every write says which layout it needs and this is how it gets
// there. Tracked per image so a no-op transition is not recorded.
void Transition(VkCommandBuffer cmd, OwnedImage& img, VkImageLayout to)
{
    if (img.image == VK_NULL_HANDLE || img.layout == to)
        return;

    VkImageMemoryBarrier barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = img.layout;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = img.image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    const auto access = [](VkImageLayout layout) -> VkAccessFlags {
        switch (layout)
        {
        case VK_IMAGE_LAYOUT_UNDEFINED: return 0;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return VK_ACCESS_TRANSFER_READ_BIT;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return VK_ACCESS_TRANSFER_WRITE_BIT;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return VK_ACCESS_SHADER_READ_BIT;
        default: return VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        }
    };
    barrier.srcAccessMask = access(img.layout);
    barrier.dstAccessMask = access(to);

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);

    img.layout = to;
}

// A resource the game owns. Its layout is the game's business, so this records the transition and
// puts it back exactly as it was rather than tracking it.
void TransitionForeign(VkCommandBuffer cmd, VkImage image, VkImageSubresourceRange range, VkImageLayout from,
                       VkImageLayout to)
{
    if (image == VK_NULL_HANDLE || from == to)
        return;

    VkImageMemoryBarrier barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = range;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
}

// ---------------------------------------------------------------------------------------------
// Bring-up
// ---------------------------------------------------------------------------------------------

bool LoadForwarder()
{
    if (g_vk.forwarder != nullptr)
        return g_vk.init != nullptr && g_vk.create != nullptr && g_vk.evaluate != nullptr;

    auto path = Util::FindFilePath(Util::DllPath().remove_filename(), "nvngx.dll_dlssnr.dll");

    if (!path.has_value())
        path = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx.dll_dlssnr.dll");

    if (!path.has_value())
    {
        Fail("nvngx.dll_dlssnr.dll was not found beside OptiScaler or the game");
        return false;
    }

    g_vk.forwarder = LoadLibraryW(path->wstring().c_str());

    if (g_vk.forwarder == nullptr)
    {
        Fail("the forwarder would not load");
        return false;
    }

    g_vk.probe = (PFN_VkProbe) GetProcAddress(g_vk.forwarder, "dlssnr_vk_probe");
    g_vk.init = (PFN_VkInit) GetProcAddress(g_vk.forwarder, "dlssnr_vk_init");
    g_vk.create = (PFN_VkCreate) GetProcAddress(g_vk.forwarder, "dlssnr_vk_create");
    g_vk.evaluate = (PFN_VkEvaluate) GetProcAddress(g_vk.forwarder, "dlssnr_vk_evaluate_v2");
    g_vk.release = (PFN_VkRelease) GetProcAddress(g_vk.forwarder, "dlssnr_vk_release");
    g_vk.shutdown = (PFN_VkShutdown) GetProcAddress(g_vk.forwarder, "dlssnr_vk_shutdown");
    g_vk.forget = (PFN_VkForget) GetProcAddress(g_vk.forwarder, "dlssnr_vk_forget");
    g_vk.setFloatSlot = (PFN_VkSetFloatSlot) GetProcAddress(g_vk.forwarder, "dlssnr_call_set_float_slot");
    g_vk.probeFloat = (PFN_VkProbeFloat) GetProcAddress(g_vk.forwarder, "dlssnr_call_probe_float");

    if (g_vk.init == nullptr || g_vk.create == nullptr || g_vk.evaluate == nullptr)
    {
        Fail("Update nvngx.dll_dlssnr.dll from the complete release (NR v2 exports required)");
        return false;
    }

    // Forwarders from before the Vulkan init fix hand NGX a wrong SDK version (0) and a bogus feature-info pointer,
    // and never initialise NGX again on a recreated device. They still run; say so once.
    if (g_vk.shutdown == nullptr || g_vk.forget == nullptr)
        LOG_WARN("DLSS-NR Vulkan: this nvngx.dll_dlssnr.dll predates the Vulkan init fix; update it from the release "
                 "(NGX is initialised with a wrong SDK version and not again after a device change)");

    return true;
}

// Which vtable slot this parameter block keeps floats in, as on D3D12 (DlssNr_Dx12.cpp, DiscoverFloatSlot): the
// driver's own block does not keep them at the header's slot 1, where every float reads back as
// FAIL_UnsupportedParameter -- which on Vulkan dropped the motion vector scale, intensity and the local/skin
// strengths. Run before anything is written to the block.
void DiscoverFloatSlotVk(NVSDK_NGX_Parameter* params)
{
    if (g_vk.floatSlotKnown || params == nullptr || g_vk.probeFloat == nullptr || g_vk.setFloatSlot == nullptr)
        return;

    g_vk.floatSlotKnown = true;

    static const char* kProbeKey = "DLSSNR.OptiScalerFloatProbe";
    static const int kCandidates[] = { 1, 2, 5, 6, 7, 4, 3, 0 };
    const float expected = 0.375f; // exact in binary, so the round trip is exact or it is wrong

    for (int slot : kCandidates)
    {
        float readBack = 0.0f;
        g_vk.probeFloat(params, kProbeKey, expected, slot);

        if (params->Get(kProbeKey, &readBack) == NVSDK_NGX_Result_Success && readBack == expected)
        {
            g_vk.setFloatSlot(slot);
            LOG_INFO("DLSS-NR Vulkan: float parameters go through vtable slot {}", slot);
            return;
        }
    }

    LOG_ERROR("DLSS-NR Vulkan: could not find the float setter: the motion vector scale, intensity, local structure, "
              "local tone and skin structure will have no effect. The uint parameters still apply.");
}

// Whether a format can hold linear, open-ended light. A frame the game already tone mapped has white
// at 1 and must not be encoded a second time; an 8-bit or normalised format cannot be scene-referred
// whatever the game says. The D3D12 path asks the same question of DXGI formats.
bool FormatCanHoldLinearHdr(VkFormat format)
{
    switch (format)
    {
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32G32B32A32_SFLOAT:
    case VK_FORMAT_R16G16B16_SFLOAT:
    case VK_FORMAT_R32G32B32_SFLOAT:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
        return true;
    default:
        return false;
    }
}

// The create flags the game gave its own upscaler, which is where HDR and inverted depth are stated.
// Read from the parameter block rather than configured, because they describe the game's buffers and
// getting either wrong is silent: an encoded frame encoded twice, or depth read backwards.
unsigned int GameCreateFlags(NVSDK_NGX_Parameter* params)
{
    unsigned int flags = 0;

    if (params != nullptr)
        params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags);

    return flags;
}

std::optional<std::filesystem::path> FindSnippet()
{
    auto snippet = Util::FindFilePath(Util::DllPath().remove_filename(), "nvngx_dlssnr.dll");

    if (!snippet.has_value())
        snippet = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");

    return snippet;
}

#include "DlssNr_ExposureCalibrate_Vk.inl"
#include "DlssNr_DetailReuse_Vk.inl"
#include "DlssNr_Spatial_Vk.inl"

// The game's depth for the resolve's depth fade (shaders/dlssnr/DlssNr_DepthFade.h). Read as a sampled image in
// SHADER_READ_ONLY_OPTIMAL and never moved, as the detail reuse and Compress screen edges read it, through a depth-only
// view when the game's own view covers stencil too (a view with both aspects cannot be sampled). That view is made for
// the frame and retired like theirs.
namespace DepthFadeVk
{
OwnedImage depthOnly;
bool warned = false;

// The depth image view to bind, or null (no depth, or one that cannot be read: the fade then does nothing).
VkImageView View(VkDevice device, const NVSDK_NGX_Resource_VK* res)
{
    DetailReuseVk::Retire(depthOnly);

    if (res == nullptr || res->Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW)
        return VK_NULL_HANDLE;

    const auto& info = res->Resource.ImageViewInfo;
    if (info.ImageView == VK_NULL_HANDLE)
        return VK_NULL_HANDLE;

    if (info.SubresourceRange.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT ||
        info.SubresourceRange.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT)
        return info.ImageView;

    if (info.Image == VK_NULL_HANDLE || (info.SubresourceRange.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) == 0)
        return VK_NULL_HANDLE;

    VkImageViewCreateInfo view {};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = info.Image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = info.Format;
    view.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, info.SubresourceRange.baseMipLevel, 1,
                              info.SubresourceRange.baseArrayLayer, 1 };

    if (vkCreateImageView(device, &view, nullptr, &depthOnly.view) != VK_SUCCESS)
    {
        depthOnly.view = VK_NULL_HANDLE;
        if (!warned)
        {
            warned = true;
            LOG_WARN("DLSS-NR Vulkan depth fade: could not make a depth-only view of the game's depth");
        }
        return VK_NULL_HANDLE;
    }

    return depthOnly.view;
}

// The fade is off: let go of last frame's view.
void Idle() { DetailReuseVk::Retire(depthOnly); }

// Shutdown. deviceAlive: the device is idle and the view is destroyed; otherwise it went with the device.
void Release(bool deviceAlive)
{
    if (deviceAlive)
        DestroyImage(depthOnly);
    else
        depthOnly = OwnedImage {};
}
} // namespace DepthFadeVk

} // namespace

// ---------------------------------------------------------------------------------------------

void ReleaseNrObjects(); // below, with ShutdownVk

bool IsRunningVk() { return g_vk.feature != nullptr && !g_vk.failed; }

const char* FailureReasonVk() { return g_vk.failed ? g_vk.reason : ""; }

bool RetryableVk() { return g_vk.failed && !g_vkFailurePermanent; }

void RequestRetryVk() { g_vkRetry.Request(); }

// Read from the menu thread without the lock: the model's size is two words that only change in the resize block, and a
// torn read costs one wrong frame of a text line.
void CurrentModelSizeVk(unsigned int& width, unsigned int& height)
{
    width = g_vk.workWidth;
    height = g_vk.workHeight;
}

unsigned long long FramesVk() { return g_vk.frames; }

bool ExposureOfferedVk() { return g_vk.exposureOffered; }

ExposureStatus AutoExposureStatusVk()
{
    ExposureStatus s {};
    s.seenFrames = g_vk.autoExposureFrames;
    s.offeredNow = g_vk.autoExposureActive;
    s.everOffered = g_vk.autoExposureFrames != 0;
    s.exposure = g_vk.autoExposureValue;
    s.preExposure = g_vk.autoExposurePreExposure;
    return s;
}

FollowGameStatus FollowGameExposureStatusVk()
{
    FollowGameStatus s {};
    s.gameExposureSeen = g_vk.pairGameExposure > 0.0f;
    s.following = g_vk.followingGame;
    s.foundInForce = g_vk.foundBaseWhitePoint > 0.0f;
    return s;
}

ExposureStatus GameExposureStatusVk()
{
    ExposureStatus s {};
    s.seenFrames = g_vk.frames;
    s.offeredNow = g_vk.exposureOffered;
    s.everOffered = g_vk.exposureOffered;
    s.exposure = g_vk.gameExposure;
    s.preExposure = g_vk.gamePreExposure;
    return s;
}

std::optional<double> LastGpuTimeVk() { return g_vk.lastGpuTime; }

DetailReuseInfo DetailReuseStatusVk() { return DetailReuseVk::Published(); }

Spatial::Published EdgeCompressionStatusVk() { return EdgeCompressionVk::Published(); }

unsigned long long VkFrameClock()
{
    const unsigned long long presents = State::Instance().vulkanPresentCount.load(std::memory_order_relaxed);
    return presents != 0 ? presents : g_vk.frames;
}

// The post-NR LUT stack on `view`, the picture NR just wrote (the game's image after SR, our own pre-SR image before
// it). In place: the pass reads and writes one pixel per thread and nothing around it, and the image is already in
// GENERAL for the resolve that wrote it. Only called after a resolve that ran, so a failed evaluation leaves the game's
// image alone.
static void ApplyPostLutVk(VkCommandBuffer cmdBuffer, VkDevice device, VkPhysicalDevice physicalDevice,
                           const Config& cfg, VkImageView view, bool ownTarget, uint32_t width, uint32_t height,
                           uint32_t shaderConversion, bool linearHdr, const DlssNrConstants& encode,
                           VkImageView liveExposureView)
{
    const auto entries = DlssNr::PostLutEntries(cfg);
    const bool wanted =
        std::any_of(entries.begin(), entries.end(), [](const DlssNr::ChainEntry& e) { return !e.path.empty(); });
    DlssNr::PostLutStatus status;
    status.wanted = wanted;

    if (!wanted)
    {
        if (g_vk.postLutPass && g_vk.postLutPass->HoldsImage())
        {
            // Earlier frames may still read the image; one drain, then every frame without a stack costs nothing.
            vkDeviceWaitIdle(device);
            g_vk.postLutPass->ReleaseLutImage();
        }

        DlssNr_LutChainEnsure(&g_vk.postLut, {});
        DlssNr::ReportPostLutStatus(status);
        return;
    }

    bool graded = false;
    const bool usable =
        DlssNr_LutChainEnsure(&g_vk.postLut, entries, DlssNr::FrameDecodeFor(shaderConversion, linearHdr));

    // The game's image after SR can be any format, so the pass writes it through a storage image declared without one,
    // which the device must allow (the resolve that just wrote it needs the same). Our own pre-SR image is RGBA16F.
    const bool anyFormat = VkExt::WritesWithoutFormat(device);
    if (usable && !ownTarget && !anyFormat)
        status.note = "Not applied after Super Resolution: the game's Vulkan device was created without a feature it "
                      "needs (restart the game with NR on).";
    else if (usable)
    {
        if (!g_vk.postLutPass)
            g_vk.postLutPass = std::make_unique<DlssNr_LutVk>(device, physicalDevice, anyFormat);

        const DlssNr::Lut3D& lut = g_vk.postLut.baked;

        if (g_vk.postLutPass->Ready() && g_vk.postLutPass->NeedsDrainFor(lut.size, g_vk.postLut.key))
            vkDeviceWaitIdle(device);

        if (g_vk.postLutPass->Ready() &&
            g_vk.postLutPass->EnsureLutImage(cmdBuffer, lut.rgb.data(), lut.size, g_vk.postLut.key))
        {
            DlssNrLutConstants constants {};
            constants.Width = width;
            constants.Height = height;
            constants.Strength = g_vk.postLut.strength; // a single layer's own; a baked stack holds its strengths
            constants.DomainMinR = lut.domainMin[0];
            constants.DomainMinG = lut.domainMin[1];
            constants.DomainMinB = lut.domainMin[2];
            constants.DomainMaxR = lut.domainMax[0];
            constants.DomainMaxG = lut.domainMax[1];
            constants.DomainMaxB = lut.domainMax[2];
            constants.LutSize = (uint32_t) lut.size;
            constants.LatticeCoord = (uint32_t) lut.latticeCoord;
            constants.InputEncoding = shaderConversion;
            constants.ColourIsLinearHdr = linearHdr ? 1u : 0u;
            CopyLiveWhitePoint(constants, encode);
            if (liveExposureView == VK_NULL_HANDLE)
                constants.UseGameExposure = constants.UseExposureWhitePoint = 0u;

            // The resolve's writes must be visible to this pass's reads.
            VkMemoryBarrier barrier {};
            barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 1, &barrier, 0, nullptr, 0, nullptr);

            graded = g_vk.postLutPass->Dispatch(cmdBuffer, view, VK_IMAGE_LAYOUT_GENERAL, view, constants,
                                                liveExposureView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    }

    const DlssNr_LutChainState& chain = g_vk.postLut;
    status.loaded = graded && chain.Loaded();
    status.chain = chain.description;
    static_assert(std::size(decltype(status.inputs) {}) == DlssNr::kMaxLutChainLayers);
    std::copy(std::begin(chain.inputs), std::end(chain.inputs), status.inputs);
    status.endsIn = chain.endsIn;
    if (!chain.error.empty())
    {
        status.failedPath = chain.failedLayer >= 0 ? chain.failedPath : std::string();
        status.error = chain.error;
    }
    DlssNr::ReportPostLutStatus(status);
}

static void EvaluateAtSeamVk(VkCommandBuffer cmdBuffer, NVSDK_NGX_Parameter* params, VkInstance instance,
                             VkPhysicalDevice physicalDevice, VkDevice device, bool beforeSr, bool rayReconstruction,
                             bool& applied, bool* handled = nullptr)
{
    applied = false;
    auto& cfg = *Config::Instance();

    if (cfg.DlssNrDeferredDlss.value_or_default() && !rayReconstruction)
    {
        static bool warnedDeferred = false;
        if (!warnedDeferred)
        {
            LOG_WARN("DLSS-NR DeferredDLSS requires the D3D12 path or a D3D12 bridge; "
                     "native Vulkan leaves the clean SR frame unchanged");
            warnedDeferred = true;
        }
        return;
    }

    if (!cfg.DlssNrEnabled.value_or_default())
        return;

    if (cfg.DlssNrFinishedPicture.value_or_default())
    {
        // Finished-picture composition requires a native D3D12 swapchain; say so where the post-NR stack is shown.
        DlssNr::PostLutStatus status;
        status.wanted = std::any_of(cfg.DlssNrPostLutFile, cfg.DlssNrPostLutFile + Config::kPostLutLayers,
                                    [](const auto& file) { return !file.value_or_default().empty(); });
        if (status.wanted)
            status.note = "Not applied: Finished Picture mode does not run NR on Vulkan.";
        DlssNr::ReportPostLutStatus(status);
        return;
    }

    if (cmdBuffer == VK_NULL_HANDLE || params == nullptr || device == VK_NULL_HANDLE ||
        physicalDevice == VK_NULL_HANDLE)
        return;

    std::lock_guard<std::mutex> lock(g_vkMutex);
    VkLockMark lockMark;

    // A retry the menu asked for: here, on the thread that records NR, never from the menu. Drain the device, let go of
    // what NR made (ReleaseNrObjects, which also clears the compression fallback), and start over; a failure that
    // cannot be retried stays. If the device will not go idle the retry itself fails and says so.
    const RetryStep retryStep = DecideRetry(g_vkRetry.Consume(g_vkRetryHandled), g_vk.failed, g_vkFailurePermanent,
                                            g_vk.device != VK_NULL_HANDLE, g_vk.device != device);

    // The game made a new device since the failure: the old one is gone with everything NR made on it, so no call goes to
    // it (no wait, no destroy); the handles are forgotten and the code below adopts the new device.
    if (retryStep == RetryStep::Abandon)
    {
        LOG_INFO("DLSS-NR Vulkan: retrying after \"{}\" on a new device", g_vk.reason);
        ShutdownVk(false);
        g_vk.failed = false;
        g_vk.reason = "";
    }
    else if (retryStep == RetryStep::Release)
    {
        if (g_vk.device != VK_NULL_HANDLE && vkDeviceWaitIdle(g_vk.device) != VK_SUCCESS)
        {
            g_vk.failed = false; // Fail() keeps the first reason otherwise
            Fail("the Vulkan device could not retire work for retry");
            return;
        }

        LOG_INFO("DLSS-NR Vulkan: retrying after \"{}\"", g_vk.reason);
        ReleaseNrObjects();
        g_vk.failed = false;
        g_vk.reason = "";
    }

    if (g_vk.failed)
        return;

    // The game's own resources, already wrapped: NGX hands Vulkan resources over as
    // NVSDK_NGX_Resource_VK, so only this pass's own images need building.
    NVSDK_NGX_Resource_VK* colour = nullptr;
    NVSDK_NGX_Resource_VK* depth = nullptr;
    NVSDK_NGX_Resource_VK* motion = nullptr;

    params->Get(beforeSr ? NVSDK_NGX_Parameter_Color : NVSDK_NGX_Parameter_Output, (void**) &colour);
    params->Get(NVSDK_NGX_Parameter_Depth, (void**) &depth);
    params->Get(NVSDK_NGX_Parameter_MotionVectors, (void**) &motion);

    // The game's exposure, now read rather than only counted.
    //
    // What blocked this was the layout: a descriptor names the layout its image will be in when the
    // shader runs, NVIDIA's Vulkan header does not use the word "layout" once, and a barrier is no
    // safer because it needs the layout it is coming from. Three things in this tree answer it, and
    // they agree. FSR2Feature_Vk hands this same texture to FidelityFX as COMPUTE_READ, which its
    // Vulkan backend maps to SHADER_READ_ONLY_OPTIMAL, on a path that works in these games. The
    // D3D12-on-Vulkan bridge transitions the game's exposure image out of SHADER_READ_ONLY_OPTIMAL,
    // on a path that works. And the header stating nothing means there is no contract to break --
    // the convention is the contract.
    //
    // So it is bound in SHADER_READ_ONLY_OPTIMAL and no barrier is recorded: this never transitions a
    // resource it does not own. If a game turns out to leave it somewhere else the cost is a wrong
    // number, not a lost device, and the gate on the readback throws a wrong number away.
    NVSDK_NGX_Resource_VK* exposure = nullptr;
    float preExposure = 1.0f;
    const bool havePre =
        params->Get(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, &preExposure) == NVSDK_NGX_Result_Success;

    params->Get(NVSDK_NGX_Parameter_ExposureTexture, (void**) &exposure);

    static bool saidExposure = false;

    if (!saidExposure)
    {
        saidExposure = true;
        LOG_INFO("DLSS-NR Vulkan: exposure from the game: DLSS.Pre.Exposure {}, ExposureTexture {}",
                 havePre ? std::to_string(preExposure) : std::string("not supplied"),
                 exposure != nullptr ? "supplied" : "not supplied");
    }

    g_vk.exposureOffered = exposure != nullptr;
    // For WhitePointSource auto, only a texture the reader below can read (an image view), from a game that did not
    // leave exposure to the upscaler (AutoExposure flag), counts as the game's exposure, as on D3D12
    const bool gameExposureReadable = exposure != nullptr && exposure->Type == NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW &&
                                      exposure->Resource.ImageViewInfo.ImageView != VK_NULL_HANDLE &&
                                      (GameCreateFlags(params) & NVSDK_NGX_DLSS_Feature_Flags_AutoExposure) == 0;
    DlssNr::NoteExposureFrame(cfg, gameExposureReadable, g_vk.gameExposure > 1e-6f);

    if (havePre && std::isfinite(preExposure) && preExposure > 0.0f)
        g_vk.gamePreExposure = preExposure;

    // Take the grid written four frames ago. Retired by now, so this reads mapped memory rather than
    // waiting on the GPU -- which is the whole reason for the ring.
    if (g_vk.meterFrames >= kMeterSlots)
    {
        const unsigned long long readSlot = g_vk.meterFrames % kMeterSlots;
        const void* mapped = g_vk.meterMapped[readSlot];

        if (mapped != nullptr)
        {
            float measured = 0.0f;
            std::memcpy(&measured, mapped, sizeof(float));
            float pairedGame = 0.0f;
            std::memcpy(&pairedGame, (const char*) mapped + sizeof(float), sizeof(float));
            float rawReading = 0.0f;
            std::memcpy(&rawReading, (const char*) mapped + 2 * sizeof(float), sizeof(float));

            // Believed only if it could be an exposure. A texel read through a layout the game did
            // not leave it in, or a slot the game stopped filling, fails here and the last good
            // value stands. Which exposure it is depends on what wrote the slot.
            if (g_vk.meterExposureKind[readSlot] == 1u)
            {
                const bool wasTrusted = g_vk.gameLiveGate.Trusted();
                g_vk.gameLiveGate.Count(std::isfinite(measured) && measured > 0.0f);
                if (wasTrusted && !g_vk.gameLiveGate.Trusted())
                {
                    g_vk.gameLiveSaid = false;
                    LOG_WARN("DLSS-NR Vulkan: the game's exposure stopped reading cleanly; the host value "
                             "stands until it has read cleanly again");
                }
            }

            if (std::isfinite(measured) && measured > 0.0f)
            {
                if (g_vk.meterExposureKind[readSlot] == 1u)
                {
                    g_vk.gameExposure = measured;
                }
                else if (g_vk.meterExposureKind[readSlot] == 2u)
                {
                    g_vk.autoExposureValue = measured;
                    g_vk.autoExposurePreExposure = g_vk.meterExposurePreExposure[readSlot];
                    g_vk.autoExposureRawValue =
                        g_vk.meterHasRaw[readSlot] && std::isfinite(rawReading) ? rawReading : measured;

                    DlssNr::ReportAutoExposureDefaults();

                    // The game's exposure from the same frame. Believed on the same terms as Game exposure's own
                    // courier: a read through a layout the game did not leave the image in fails here.
                    if (g_vk.meterPairHasGame[readSlot])
                    {
                        const bool valid = std::isfinite(pairedGame) && pairedGame > 0.0f && pairedGame < 1e8f;
                        ++g_vk.pairReads;

                        if (valid)
                        {
                            ++g_vk.pairValid;
                            g_vk.pairGameExposure = pairedGame;
                            g_vk.pairPreExposure = g_vk.meterExposurePreExposure[readSlot];
                            g_vk.pairValidAt = g_vk.meterFrames;

                            // Against the meter's own reading of that frame, not eye adaptation's eased value.
                            const float autoReading =
                                g_vk.meterHasRaw[readSlot] && std::isfinite(rawReading) && rawReading > 0.0f
                                    ? rawReading
                                    : g_vk.autoExposureValue;

                            if (DlssNr::FollowGameOn(*Config::Instance()) &&
                                DlssNrFollowGame::Instance().Feed(g_vk.autoExposurePreExposure / autoReading,
                                                                  g_vk.pairPreExposure / g_vk.pairGameExposure))
                                LOG_INFO("DLSS-NR automatic exposure: calibrated against the game's own exposure: "
                                         "{:+.2f} EV (Automatic's base white point is {:.3g}x the game's); follows the "
                                         "game's exposure from here while AutoExposureFollowGame is on (Vulkan: a few "
                                         "frames behind the game)",
                                         DlssNrFollowGame::Instance().OffsetEv(), DlssNrFollowGame::Instance().Scale());
                            else if (DlssNr::FollowGameOn(*Config::Instance()))
                                DlssNr::SayFollowTrack(DlssNrFollowGame::Instance().Track(
                                    g_vk.autoExposurePreExposure / autoReading, g_vk.pairPreExposure / g_vk.pairGameExposure,
                                    GetTickCount64(),
                                    DlssNrExposureCalibrate::HoldsFollow(DlssNrExposureCalibrate::TheRun(), GetTickCount64())));
                        }

                        if (!g_vk.pairValiditySaid && g_vk.pairReads >= DlssNrFollowGame::kWindow)
                        {
                            g_vk.pairValiditySaid = true;
                            LOG_INFO("DLSS-NR Vulkan: the game's exposure image read as a valid exposure in {} of {} "
                                     "readings (last {})",
                                     g_vk.pairValid, g_vk.pairReads, pairedGame);
                        }
                    }

                    // Diagnostic (ini FrameStats), the Vulkan counterpart of D3D12's frame stats line for the frame
                    // type: every ~2 s Automatic's base white point, and the game's beside it while they are paired.
                    static unsigned statsReadings = 0;

                    if (cfg.DlssNrFrameStats.value_or_default() && ++statsReadings % 120u == 0u)
                    {
                        const float autoBase = g_vk.autoExposurePreExposure / g_vk.autoExposureValue;
                        const bool paired = g_vk.meterPairHasGame[readSlot] && g_vk.pairGameExposure > 0.0f;
                        const float gameBase = paired ? g_vk.pairPreExposure / g_vk.pairGameExposure : 0.0f;
                        LOG_INFO("DLSS-NR frame stats (Vulkan): Automatic base white point {:.4g}{}; game exposure {} "
                                 "(base white point {:.4g}, Automatic/game {:.3g}{}); known unexposed game: {}, "
                                 "following the game's exposure: {}",
                                 autoBase,
                                 g_vk.meterHasRaw[readSlot]
                                     ? std::format(" (exposure {:.5g}, eye adaptation {:.1f} s brighter / {:.1f} s darker, meter reading {:.5g})",
                                                   g_vk.autoExposureValue,
                                                   DlssNrExposureAdapt::Seconds(
                                                       cfg.DlssNrAutoExposureAdaptBrighterSeconds.value_or_default(),
                                                       DlssNrExposureAdapt::kDefaultBrighterSeconds),
                                                   DlssNrExposureAdapt::Seconds(
                                                       cfg.DlssNrAutoExposureAdaptDarkerSeconds.value_or_default()),
                                                   g_vk.autoExposureRawValue)
                                     : std::string(),
                                 paired ? g_vk.pairGameExposure : 0.0f, gameBase,
                                 paired && gameBase > 0.0f ? autoBase / gameBase : 0.0f,
                                 paired ? "" : "; read only while following or scanning",
                                 DlssNr::KnownUnexposedGame() ? "yes" : "no",
                                 DlssNr::FollowGameOn(cfg) ? "on" : "off");
                    }
                }
            }
        }
    }

    // Said when it moves by more than a fiftieth, not every frame. Enough to see in a log that the
    // number is the game's and that it tracks the scene, without a line per frame.
    static float loggedExposure = -1.0f;

    if (g_vk.gameExposure > 1e-6f &&
        std::abs(loggedExposure - g_vk.gameExposure) > std::max(0.02f * g_vk.gameExposure, 1e-5f))
    {
        loggedExposure = g_vk.gameExposure;
        LOG_INFO("DLSS-NR Vulkan: the game's exposure is {}, pre-exposure {}, so white point {}",
                 g_vk.gameExposure, g_vk.gamePreExposure, g_vk.gamePreExposure / g_vk.gameExposure);
    }

    if (colour == nullptr || depth == nullptr || motion == nullptr)
    {
        static bool said = false;

        if (!said)
        {
            said = true;
            LOG_INFO("DLSS-NR Vulkan: the parameter block carried no {}",
                     colour == nullptr ? "output" : (depth == nullptr ? "depth" : "motion vectors"));
        }

        return;
    }

    if (colour->Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW ||
        depth->Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW ||
        motion->Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW ||
        colour->Resource.ImageViewInfo.ImageView == VK_NULL_HANDLE ||
        depth->Resource.ImageViewInfo.ImageView == VK_NULL_HANDLE ||
        motion->Resource.ImageViewInfo.ImageView == VK_NULL_HANDLE)
        return;

    uint32_t width = colour->Resource.ImageViewInfo.Width;
    uint32_t height = colour->Resource.ImageViewInfo.Height;
    uint32_t renderWidth = 0, renderHeight = 0, baseX = 0, baseY = 0;
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &renderWidth);
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &renderHeight);
    if (beforeSr)
    {
        params->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, &baseX);
        params->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, &baseY);
        // Origin-zero padded inputs are common with dynamic resolution. Never use a preset table.
        if (baseX || baseY || ((renderWidth == 0) != (renderHeight == 0)) ||
            renderWidth > width || renderHeight > height)
            return; // caller falls back to post-SR, without editing the input
        if (renderWidth && renderHeight)
        {
            width = renderWidth;
            height = renderHeight;
        }
    }
    const unsigned int createFlags = GameCreateFlags(params);
    uint32_t depthX = 0, depthY = 0, motionX = 0, motionY = 0;
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, &depthX);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, &depthY);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, &motionX);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, &motionY);
    NVSDK_NGX_Resource_VK* output = nullptr;
    params->Get(NVSDK_NGX_Parameter_Output, (void**) &output);
    uint32_t outputWidth = 0, outputHeight = 0;
    if (output != nullptr && output->Type == NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW)
    {
        outputWidth = output->Resource.ImageViewInfo.Width;
        outputHeight = output->Resource.ImageViewInfo.Height;
    }
    uint32_t declaredWidth = 0, declaredHeight = 0;
    params->Get(NVSDK_NGX_Parameter_OutWidth, &declaredWidth);
    params->Get(NVSDK_NGX_Parameter_OutHeight, &declaredHeight);
    if (declaredWidth && declaredHeight)
    {
        outputWidth = declaredWidth;
        outputHeight = declaredHeight;
    }
    const auto guides = ResolveGuideRegions(
        { depth->Resource.ImageViewInfo.Width, depth->Resource.ImageViewInfo.Height },
        { motion->Resource.ImageViewInfo.Width, motion->Resource.ImageViewInfo.Height },
        { renderWidth, renderHeight }, { outputWidth, outputHeight },
        (createFlags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0, depthX, depthY, motionX, motionY);
    if (!guides.depth.valid() || !guides.motion.valid())
        return;
    const auto guideWidth = guides.depth.width, guideHeight = guides.depth.height;

    if (width == 0 || height == 0)
        return;
    if (handled)
        *handled = true; // includes warm-up: do not recreate post-SR resources in this command buffer

    // The model's working size. The slider is a fraction of the frame; at 1 it is the frame, and the
    // reduced path below never runs, so the default is byte-for-byte what it was.
    // Above 1 the model supersamples (up to 2x): the proxy is enlarged, the model runs above native,
    // and superDown averages the answer back. Vulkan matches the D3D12 cap.
    float workScale = cfg.DlssNrWorkingScale.value_or_default();
    workScale = std::isfinite(workScale) ? std::clamp(workScale, 0.25f, 2.0f) : 1.0f;
    const uint32_t workWidth = std::max(1u, (uint32_t) (width * workScale + 0.5f));
    const uint32_t workHeight = std::max(1u, (uint32_t) (height * workScale + 0.5f));
    const bool reduced = workWidth != width || workHeight != height;
    const unsigned int passes = std::clamp(cfg.DlssNrPasses.value_or_default(),
                                           1u, cfg.DlssNrUnlockPasses.value_or_default() ? DlssNr::MaxPassCount
                                                                                       : DlssNr::DefaultMaxPassCount);

    g_vk.instance = instance;
    g_vk.physicalDevice = physicalDevice;

    // A device change invalidates everything. The OLD device is presumed dead here -- the game
    // destroyed it, which already freed every resource made on it -- so abandon those handles rather
    // than call vkDestroy*/wait-idle on a dead device (that would be use-after-free). Rebuild fresh.
    if (g_vk.device != device)
    {
        ShutdownVk(false);
        g_vk.device = device;
        g_vk.instance = instance;
        g_vk.physicalDevice = physicalDevice;
    }

    if (!LoadForwarder())
        return;

    // Initialise NGX on this device, once. The snippet path is the model itself; the forwarder loads
    // it so the caller gate sees a module named nvngx.dll.
    if (g_vk.creationPending)
    {
        // A second NGX evaluate need not mean the previous command buffer was submitted.
        // Poll the GPU marker without waiting; repeated calls during warm-up stay clean.
        if (vkGetEventStatus(device, g_vk.creationReady) != VK_EVENT_SET)
            return;
        g_vk.creationPending = false;
    }

    if (!g_vk.ngxInitialised)
    {
        auto snippet = FindSnippet();

        if (!snippet.has_value())
        {
            Fail("nvngx_dlssnr.dll was not found beside OptiScaler or the game");
            return;
        }

        const int probe = g_vk.probe != nullptr ? g_vk.probe(snippet->wstring().c_str()) : 0;

        // Four bits, one per entry point. Anything short of fifteen means the model's Vulkan surface
        // is not entirely reachable and there is no point going further.
        if (probe != 15)
        {
            LOG_ERROR("DLSS-NR Vulkan: the model's Vulkan surface is incomplete (probe {})", probe);
            Fail("the model does not expose a complete Vulkan surface", true);
            return;
        }

        const int result =
            g_vk.init(snippet->wstring().c_str(), State::Instance().NVNGX_ApplicationDataPath.c_str(),
                      (void*) instance, (void*) physicalDevice, (void*) device, 0x0000015);

        if (result != 1)
        {
            LOG_ERROR("DLSS-NR Vulkan: NVSDK_NGX_VULKAN_Init_Ext returned {}{}", result,
                      g_vk.deviceLosses > 0 ? " (a re-init after the previous device was lost)" : "");
            Fail("the model would not initialise on this Vulkan device", true);
            return;
        }

        g_vk.ngxInitialised = true;
        LOG_INFO("DLSS-NR Vulkan: the model initialised on this device");
    }

    if (g_vk.capabilityParams == nullptr)
    {
        if (NVSDK_NGX_VULKAN_AllocateParameters(&g_vk.capabilityParams) != NVSDK_NGX_Result_Success ||
            g_vk.capabilityParams == nullptr)
        {
            Fail("a parameter block could not be allocated");
            return;
        }

        uint32_t allocType = NGX_AllocTypes::Unknown;
        g_vk.capabilityParams->Get(NGX_AllocTypes::AllocKey.data(), &allocType);
        g_vk.paramsFromCore = allocType == NGX_AllocTypes::NVDynamic;

        // Before anything else is written to it, work out where this block keeps floats.
        DiscoverFloatSlotVk(g_vk.capabilityParams);
    }

    if (g_vk.queryPool == VK_NULL_HANDLE)
    {
        VkPhysicalDeviceProperties props {};
        vkGetPhysicalDeviceProperties(physicalDevice, &props);

        // A period of zero means the device does not support timestamps on this queue. The pass runs
        // regardless; it simply reports no cost, which is what the D3D12 path does when its heap is
        // unavailable.
        g_vk.timestampPeriod = props.limits.timestampPeriod;

        if (g_vk.timestampPeriod > 0.0f)
        {
            VkQueryPoolCreateInfo info {};
            info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            info.queryType = VK_QUERY_TYPE_TIMESTAMP;
            info.queryCount = kTimingSlots * 2;

            if (vkCreateQueryPool(device, &info, nullptr, &g_vk.queryPool) != VK_SUCCESS)
            {
                g_vk.queryPool = VK_NULL_HANDLE;
                LOG_INFO("DLSS-NR Vulkan: no timestamp pool, the pass will not report its cost");
            }
        }
    }

    if (g_vk.pass == nullptr)
    {
        g_vk.pass = std::make_unique<DlssNr_Vk>("Neural Rendering", device, physicalDevice);

        if (!g_vk.pass->IsInit())
        {
            g_vk.pass.reset();
            Fail("the composition pass could not be created");
            return;
        }
    }

    // Compress screen edges (DlssNr_Spatial_Vk.inl): whether the model works on a packed picture this frame, and at what
    // size. The model's surfaces, its features and the guides it is handed follow the packed size; the encode, the
    // resolve and the frame stay at the ordinary ones.
    bool spatialReset = false;
    bool spatial = EdgeCompressionVk::Begin(
        cfg, width, height, workScale,
        EdgeCompressionVk::SignatureOf(colour->Resource.ImageViewInfo.Format, depth, motion), spatialReset);

    if (spatialReset)
        g_vk.reset = true;

    if (spatial && !EdgeCompressionVk::PassReady(device, physicalDevice))
    {
        EdgeCompressionVk::TurnOff(Spatial::Status::TurnedOffResources);
        g_vk.reset = true;
        spatial = false;
    }

    // The Replace curves take the model's answer as the picture and never read the proxy, so the unpack's loss at the edges
    // would land on screen with nothing to cancel it. The unpack then adds it back, measured against the picture the
    // uncompressed path would have shown the model (mode 103), which is built below the way that path builds it.
    const bool replaceCorrection =
        spatial && DlssNrProxyCurve::IsReplace(cfg.DlssNrReversibleMode.value_or_default());

    const uint32_t modelWidth = spatial ? EdgeCompressionVk::Layout().modelW : workWidth;
    const uint32_t modelHeight = spatial ? EdgeCompressionVk::Layout().modelH : workHeight;

    // Resize. The feature is built for a size and has to be rebuilt when the frame OR the working
    // size changes -- moving the slider is a rebuild, which is why it is compared here.
    bool profileChanged = g_vk.activePasses != passes;
    for (unsigned int pass = 0; pass < passes; ++pass)
        profileChanged |= g_vk.builtTuning[pass] != Profiles::PassTuning(cfg, pass) ||
                          g_vk.builtPreset[pass] != Profiles::PassPreset(cfg, pass) ||
                          g_vk.builtStyle[pass] != Profiles::PassStyle(cfg, pass);
    if (g_vk.width != width || g_vk.height != height || g_vk.workWidth != modelWidth ||
        g_vk.workHeight != modelHeight || g_vk.beforeSr != beforeSr ||
        g_vk.rayReconstruction != rayReconstruction || profileChanged ||
        EdgeCompressionVk::Stale(spatial, EdgeCompressionVk::Layout(), workScale > 1.0f, replaceCorrection))
    {
        // This block releases the feature and frees the surfaces below IMMEDIATELY. A frame-size
        // change is already fenced by the game -- it recreates the swapchain around it -- but moving
        // the working-scale slider is not: the game is mid-flight and previous frames' command
        // buffers still reference the feature and images about to be destroyed. Freeing a Vulkan
        // resource that in-flight GPU work still touches is device removal (ERR_GFX_STATE, reproduced
        // on RDR2 and Enshrouded by dragging the model-resolution slider). Drain the device first.
        // Only the rare resize path reaches here, so the CPU stall is a one-off hitch, not per-frame.
        if (g_vk.device != VK_NULL_HANDLE)
            vkDeviceWaitIdle(g_vk.device);

        if (g_vk.feature != nullptr && g_vk.release != nullptr)
        {
            g_vk.release(g_vk.feature);
            g_vk.feature = nullptr;
        }
        for (auto& feature : g_vk.laterFeatures)
        {
            if (feature && g_vk.release)
                g_vk.release(feature);
            feature = nullptr;
        }
        DestroyImage(g_vk.scratch);
        DestroyImage(g_vk.lutScratch); // recreated at the new size when a LutFile still wants it
        g_vk.lutScratchFailed = false;
        DestroyImage(g_vk.styled); // recreated at the model picture's size when an input style still wants it
        DestroyImage(g_vk.styledB);

        const VkFormat working = VK_FORMAT_R16G16B16A16_SFLOAT;

        // The meter is a fixed 64x64 whatever the frame is, so it is only built the once -- but it is
        // built alongside the rest so that a failure here is caught by the same check.
        const bool meterReady = (g_vk.meter.Valid() || CreateImage(g_vk.meter, kMeterSide, kMeterSide,
                                                                   VK_FORMAT_R32_SFLOAT, true)) &&
                                (g_vk.autoExposure.Valid() ||
                                 CreateImage(g_vk.autoExposure, 1, 1, VK_FORMAT_R32_SFLOAT, true)) &&
                                CreateMeterReadback();

        if (!meterReady)
            LOG_WARN("DLSS-NR Vulkan: no exposure meter; the white point stays on the slider and "
                     "automatic exposure is unavailable");

        // Eye adaptation's reading. Without it the meter writes autoExposure itself, as before.
        if (meterReady && !g_vk.autoExposureRaw.Valid() &&
            !CreateImage(g_vk.autoExposureRaw, 1, 1, VK_FORMAT_R32_SFLOAT, true))
            LOG_WARN("DLSS-NR Vulkan: could not allocate the eye adaptation image; Automatic follows every frame at "
                     "once");

        g_vk.autoExposureAdapter.Invalidate();

        DestroyImage(g_vk.proxySmall);
        DestroyImage(g_vk.outputNative);

        // output is the model's target, so it is the working size. proxy and keep are full: proxy is
        // the source the downsample reads, keep is the untouched frame the resolve composites onto.
        // outputNative is the native buffer either the supersample down-leg averages the answer into,
        // or the reduced up-leg's SGSR1 answer enlarge writes into -- shared between the two legs
        // since workScale is a single scalar (never both > 1 and < 1 in the same frame).
        // Packed, the model input comes from the colour pack instead of a shrunk copy, and the supersample down-leg
        // averages into Compress screen edges' own native pair.
        EdgeCompressionVk::DestroyImages();

        const bool ok = CreateImage(g_vk.output, modelWidth, modelHeight, working, true) &&
                        (passes == 1 || CreateImage(g_vk.scratch, modelWidth, modelHeight, working, true)) &&
                        CreateImage(g_vk.proxy, width, height, working, true) &&
                        CreateImage(g_vk.keep, width, height, working, true) &&
                        (!beforeSr || CreateImage(g_vk.preColor, width, height, working, false)) &&
                        (!reduced || (spatial && !replaceCorrection) ||
                         CreateImage(g_vk.proxySmall, workWidth, workHeight, working, true)) &&
                        (workScale == 1.0f || (spatial && workScale > 1.0f) ||
                         CreateImage(g_vk.outputNative, width, height, working, true)) &&
                        (!spatial ||
                         EdgeCompressionVk::CreateImages(EdgeCompressionVk::Layout(), workScale > 1.0f, replaceCorrection));

        if (!ok)
        {
            Fail("the pass could not allocate its own surfaces");
            return;
        }

        g_vk.width = width;
        g_vk.height = height;
        g_vk.workWidth = modelWidth;
        g_vk.workHeight = modelHeight;
        g_vk.beforeSr = beforeSr;
        g_vk.rayReconstruction = rayReconstruction;
        g_vk.activePasses = passes;
        for (unsigned int pass = 0; pass < passes; ++pass)
        {
            g_vk.builtTuning[pass] = Profiles::PassTuning(cfg, pass);
            g_vk.builtPreset[pass] = Profiles::PassPreset(cfg, pass);
            g_vk.builtStyle[pass] = Profiles::PassStyle(cfg, pass);
        }
        g_vk.reset = true;
    }

    bool created = false;
    for (unsigned int pass = 0; pass < passes; ++pass)
    {
        void*& feature = pass == 0 ? g_vk.feature : g_vk.laterFeatures[pass];
        if (feature)
            continue;
        const auto tuning = Profiles::PassTuning(cfg, pass);
        feature = g_vk.create((void*) cmdBuffer, g_vk.capabilityParams, modelWidth, modelHeight,
                             (int) Profiles::PassPreset(cfg, pass), tuning.intensity,
                             (int) Profiles::PassStyle(cfg, pass), tuning.structure, tuning.tone,
                             tuning.skin, tuning.autoMask ? 1 : 0, 1);
        if (!feature)
        {
            Fail("the model would not build a feature on this device");
            return;
        }

        LOG_INFO("DLSS-NR Vulkan: pass {} built at {}x{} (frame {}x{}, {} SR)", pass + 1,
                 modelWidth, modelHeight, width, height, beforeSr ? "before" : "after");
        created = true;
        g_vk.reset = true;
    }
    // NGX creation may record GPU uploads. Do not evaluate until a subsequent submission,
    // just as on D3D12. Leave this warm-up frame clean instead of evaluating unready weights.
    if (created)
    {
        if (g_vk.creationReady == VK_NULL_HANDLE)
        {
            VkEventCreateInfo info { VK_STRUCTURE_TYPE_EVENT_CREATE_INFO };
            if (vkCreateEvent(device, &info, nullptr, &g_vk.creationReady) != VK_SUCCESS)
            {
                Fail("could not allocate the model creation marker");
                return;
            }
        }
        else
            vkResetEvent(device, g_vk.creationReady); // rebuild above drained previous GPU users
        vkCmdSetEvent(cmdBuffer, g_vk.creationReady, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        g_vk.creationPending = true;
        return;
    }

    // -----------------------------------------------------------------------------------------
    // Encode: the frame the upscaler wrote -> a display-referred proxy, plus an untouched copy
    // -----------------------------------------------------------------------------------------

    const bool gameSaysHdr = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0;
    const bool depthInverted = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;

    // The game asking the upscaler to forget its history -- a cut, a teleport, a load. Same omission
    // as the D3D12 path had: the model's history was only ever reset by things that happened to us,
    // never by anything that happened in the game.
    {
        unsigned int gameReset = 0;

        if (params->Get(NVSDK_NGX_Parameter_Reset, &gameReset) == NVSDK_NGX_Result_Success &&
            gameReset != 0)
        {
            g_vk.reset = true;

            static unsigned long long resets = 0;
            ++resets;

            if (resets <= 3 || resets % 100 == 0)
                LOG_INFO("DLSS-NR Vulkan: the game asked for a history reset ({} so far)", resets);
        }
    }

    // Both have to agree. A game can set the HDR flag on a buffer that cannot hold open-ended light,
    // and encoding an already tone-mapped frame a second time looks washed out and banded.
    // [DlssNr] ColourEncoding can overrule both (DlssNr_ColourEncoding.h); Auto is exactly this rule.
    const auto colourChoice = DlssNrColourEncoding::Resolve(cfg.DlssNrColourEncoding.value_or_default(), gameSaysHdr,
                                                            FormatCanHoldLinearHdr(colour->Resource.ImageViewInfo.Format));
    const bool linearHdr = colourChoice.LinearHdr();
    const uint32_t shaderConversion = DlssNrColourEncoding::ShaderConversion(colourChoice.encoding);
    // Forced PQ is display light: its white is the game's HDR paper white (203 nits = 1.0 after the decode), whatever
    // the game's or Automatic's exposure says (the D3D12 path's ApplyColourEncoding does the same).
    const bool displayWhite = colourChoice.encoding == DlssNrColourEncoding::Encoding::Pq;
    {
        char colourFormat[32];
        std::snprintf(colourFormat, sizeof(colourFormat), "VkFormat %d", (int) colour->Resource.ImageViewInfo.Format);
        DlssNr::ReportColourEncoding(colourChoice, colourFormat, "Vulkan", false);
    }

    // The same rule as the D3D12 path, deliberately spelled the same way: the game divides its frame
    // by preExposure and multiplies by exposure, so undoing that is the divisor this pass wants, and
    // the slider becomes a trim on top rather than the answer.
    //
    // The trim is bounded here, at the point of use, rather than at the slider. Someone who found 64
    // by hand on the manual path and then switches the exposure source on keeps that 64 in their ini;
    // bounding it in the menu would leave the picture wrong for a reason the menu no longer showed.
    // Their value stays in the config untouched, so switching back to manual restores it.
    float whitePoint = cfg.DlssNrWhitePointScale.value_or_default();

    // What the debug views are scaled by on a linear HDR frame: the base white point, before the Trim, so they sit
    // at the frame's own brightness and the Trim still shows in them. See DebugViewScale in dlssnr.hlsl.
    float debugWhitePoint = 0.0f;

    // The ring carries the game's exposure or the automatic one; a change of source starts it over.
    const uint32_t requestedWhitePointSource = DlssNr::WhitePointSourceEffective(cfg);

    if (requestedWhitePointSource != g_vk.exposureReadbackSource)
    {
        g_vk.exposureReadbackSource = requestedWhitePointSource;
        g_vk.meterFrames = 0;
        g_vk.gameExposure = 0.0f;
        g_vk.autoExposureValue = 0.0f;
        g_vk.autoExposurePreExposure = 1.0f;
        g_vk.autoExposureRawValue = 0.0f;
        g_vk.autoExposureAdapter.Invalidate();
        for (uint32_t& kind : g_vk.meterExposureKind)
            kind = 0u;
        for (bool& pair : g_vk.meterPairHasGame)
            pair = false;
        g_vk.pairGameExposure = 0.0f;
        g_vk.pairValidAt = 0;
        g_vk.gameLiveGate.Reset();
        g_vk.gameLiveSaid = false;
    }

    // The exposure scan (DlssNr_ExposureScan_Vk.h): its values copied on NR's command buffer, and the frame's bookkeeping
    // with what is known about the frame's brightness, as the D3D12 pass gives it (DlssNr_Dx12.cpp)
    {
        DlssNr::ExposureScan::FrameExposure scanFrame;
        scanFrame.handed = exposure != nullptr && exposure->Type == NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW
                               ? (const void*) exposure->Resource.ImageViewInfo.Image
                               : nullptr;
        scanFrame.handedApi = DlssNr::ExposureScan::Api::Vulkan;
        scanFrame.meter = requestedWhitePointSource == 3 ? g_vk.autoExposureRawValue : 0.0f;
        scanFrame.game = requestedWhitePointSource == 1   ? g_vk.gameExposure
                         : requestedWhitePointSource == 3 ? g_vk.pairGameExposure
                                                          : 0.0f;
        // The pre-exposure of the frame the meter read (it is read back a few frames late), not this frame's
        scanFrame.pre = requestedWhitePointSource == 3 ? g_vk.autoExposurePreExposure : g_vk.gamePreExposure;
        DlssNr::ExposureScanVk::Tick(cmdBuffer, device, physicalDevice, scanFrame);

        if (scanFrame.handed != nullptr)
        {
            const auto& view = exposure->Resource.ImageViewInfo;
            DlssNr::ExposureScanVk::ExplainHanded(view.Image, view.Width, view.Height, view.Format,
                                                  view.SubresourceRange.baseMipLevel,
                                                  view.SubresourceRange.baseArrayLayer);
        }
    }

    // Following the game's exposure (DlssNr_FollowGame.h): on (DlssNr_GameDefaults.h), the calibration locked, and a
    // valid reading of the game's exposure no more than eight readbacks old. Otherwise Automatic's own value stands.
    {
        const bool follow = requestedWhitePointSource == 3 && linearHdr && exposure != nullptr &&
                            DlssNr::FollowGameOn(cfg) &&
                            DlssNrFollowGame::Instance().Locked() && g_vk.pairGameExposure > 1e-8f &&
                            g_vk.meterFrames - g_vk.pairValidAt <= 8;

        if (follow != g_vk.followingGame)
            LOG_INFO("DLSS-NR automatic exposure (Vulkan): {} the game's exposure", follow ? "following" : "no longer following");

        g_vk.followingGame = follow;

        // Auto's found exposure (story 5): the game's own movement at Automatic's level, only where Automatic runs.
        // Moving to and from it is eased over a second, in stops, as on D3D12 (DlssNr_ExposureFoundBlend.h). Auto's
        // scan is D3D12-only for now (DlssNr_ExposureScan.h), so this stays at 0 until a Vulkan game is checked.
        float found = 0.0f;
        float foundPre = 1.0f;
        const float target = requestedWhitePointSource == 3 && linearHdr && !displayWhite && !follow &&
                                     DlssNr::ExposureScan::FoundExposure(&found, &foundPre) && found > 1e-8f
                                 ? foundPre / found
                                 : 0.0f;
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const float own =
            g_vk.autoExposureValue > 1e-8f ? g_vk.autoExposurePreExposure / g_vk.autoExposureValue : 0.0f;
        g_vk.foundBaseWhitePoint =
            g_vk.foundBlend.Step(target, own, now, requestedWhitePointSource == 3 && linearHdr && !displayWhite);

        switch (g_vk.foundBlend.LastEvent())
        {
        case DlssNrExposureFoundBlend::Event::TookOver:
            LOG_INFO("DLSS-NR automatic exposure (Vulkan): the game's own exposure (found by the scan) moves the white "
                     "point from here: {:.4g}, from {:.4g} ({:+.2f} EV), eased over a second",
                     target, g_vk.foundBaseWhitePoint, std::log2(g_vk.foundBaseWhitePoint / target));
            break;
        case DlssNrExposureFoundBlend::Event::Leaving:
            LOG_INFO("DLSS-NR automatic exposure (Vulkan): the game's exposure is gone; back to Automatic's own meter, "
                     "eased");
            break;
        case DlssNrExposureFoundBlend::Event::Left:
            LOG_INFO("DLSS-NR automatic exposure (Vulkan): Automatic's own meter again");
            break;
        default:
            break;
        }
    }

    if (requestedWhitePointSource == 1 && g_vk.gameExposure > 1e-6f && !displayWhite)
    {
        // The Trim is the slider, or interpolated from the Trim anchors at this base white point when
        // there are any. Resolved here on the CPU from the host readback; once the game's image reads cleanly the
        // shader recomputes it from the live image instead (liveGame below), and this value is its fallback.
        // What the Trim multiplies follows the scale (DlssNr_GameScale.h); the points stay keyed by the game's
        // exposure in both scales.
        const float baseWhitePoint = DlssNrGameScale::WhiteBase(DlssNr::GameExposureScaleEffective(cfg),
                                                                g_vk.gamePreExposure, g_vk.gameExposure);
        const float anchorKey = DlssNrGameScale::AnchorKey(g_vk.gamePreExposure, g_vk.gameExposure);
        const auto anchors = DlssNrTrim::Parse(cfg.DlssNrGameExposureTrimAnchors.value_or_default());
        const float trim = DlssNrTrim::TrimForKey(anchorKey, cfg.DlssNrWhitePointTrim.value_or_default(), anchors,
                                                  cfg.DlssNrGameExposureTrimPreview.value_or_default());
        whitePoint = std::clamp(baseWhitePoint * trim, 0.01f, 4096.0f);
        debugWhitePoint = std::clamp(baseWhitePoint, 0.01f, 4096.0f);
    }
    else if (requestedWhitePointSource == 3 && (g_vk.autoExposureValue > 1e-8f || g_vk.foundBaseWhitePoint > 0.0f))
    {
        // The fallback and the menu's number, from the readback a few frames behind. The shader
        // recomputes this from the live 1x1 image when it is bound. Following the game, this is the value in
        // force: the game's base white point times the calibration, and the shader is told not to recompute it.
        const float baseWhitePoint =
            g_vk.followingGame                ? g_vk.pairPreExposure / g_vk.pairGameExposure *
                                                    DlssNrFollowGame::Instance().Scale()
            : g_vk.foundBaseWhitePoint > 0.0f ? g_vk.foundBaseWhitePoint
                                              : g_vk.autoExposurePreExposure / g_vk.autoExposureValue;
        const auto anchors = DlssNrTrim::Parse(cfg.DlssNrAutoExposureTrimAnchors.value_or_default());
        const float trim = DlssNrTrim::TrimForKey(baseWhitePoint, DlssNr::AutoTrimEffective(cfg),
                                                  anchors, cfg.DlssNrAutoExposureTrimPreview.value_or_default());
        whitePoint = std::clamp(baseWhitePoint * trim, 0.01f, 4096.0f);
        debugWhitePoint = std::clamp(baseWhitePoint, 0.01f, 4096.0f);
    }

    if (displayWhite)
    {
        whitePoint = DlssNrExposureState::PinnedWhitePoint(cfg.DlssNrDisplayPaperWhiteNits.value_or_default(),
                                                           DlssNrExposureState::kNitsPerPqDecodedUnit);
        debugWhitePoint = whitePoint;
    }

    static bool saidEncoding = false;
    static uint32_t saidSetting = 0;

    if (!saidEncoding || saidSetting != cfg.DlssNrColourEncoding.value_or_default())
    {
        saidEncoding = true;
        saidSetting = cfg.DlssNrColourEncoding.value_or_default();
        if (colourChoice.automatic)
            LOG_INFO("DLSS-NR Vulkan: the game's buffer is {} (flag {}, format {}), depth {}",
                     linearHdr ? "linear HDR" : "already tone-mapped", gameSaysHdr ? "set" : "clear",
                     (int) colour->Resource.ImageViewInfo.Format, depthInverted ? "inverted" : "normal");
        else
            LOG_INFO("DLSS-NR Vulkan: colour encoding forced to {} (flag {}, format {}), depth {}",
                     DlssNrColourEncoding::Name(colourChoice.encoding), gameSaysHdr ? "set" : "clear",
                     (int) colour->Resource.ImageViewInfo.Format, depthInverted ? "inverted" : "normal");
    }

    DlssNrConstants encode {};
    encode.Mode = DlssNrMode_Encode;
    encode.Width = width;
    encode.Height = height;
    encode.WhitePoint = whitePoint;
    encode.Passthrough = linearHdr ? 0u : 1u;
    encode.InputEncoding = shaderConversion; // carried into the resolve, which starts as a copy
    encode.ReversibleMode = cfg.DlssNrReversibleMode.value_or_default();
    encode.ApplyModel = cfg.DlssNrApplyModel.value_or_default() ? 1u : 0u;
    encode.TransferStrength = cfg.DlssNrTransferStrength.value_or_default();
    const auto strength = [](float v) { return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 1.0f; };
    encode.SkinProtection = cfg.DlssNrSkinProtection.value_or_default();
    encode.ShowSkinMask = cfg.DlssNrShowSkinMask.value_or_default();
    encode.SkinDetail = strength(cfg.DlssNrSkinDetail.value_or_default());
    encode.SkinColour = cfg.DlssNrSkinToneEnabled.value_or_default() ? strength(cfg.DlssNrSkinColour.value_or_default()) : 0.0f;
    encode.EnvironmentDetail = strength(cfg.DlssNrEnvironmentDetail.value_or_default());
    encode.EnvironmentColour = strength(cfg.DlssNrEnvironmentColour.value_or_default());
    encode.ColourStrength = cfg.DlssNrColourStrength.value_or_default();
    encode.ReplaceDetailStrength = cfg.DlssNrReplaceDetailStrength.value_or_default();
    encode.ModelWorkScale = (reduced && workScale < 1.0f) ? workScale : 1.0f;
    encode.MaxRatio = std::clamp(cfg.DlssNrMaxRatio.value_or_default(), 1.0f, 8.0f);
    encode.Transfer = cfg.DlssNrTransfer.value_or_default();
    // Superior Scaling is D3D12-only (its private DLSS and PSOs). Native Vulkan runs Matched residual instead, said once.
    if (DlssNr::SuperiorScaling::Enabled(encode.Transfer))
    {
        static std::once_flag warned;
        std::call_once(warned, []
                       { LOG_WARN("Superior Scaling needs DirectX 12 or a DirectX 12 bridge; Vulkan uses Matched residual"); });
        encode.Transfer = DlssNr::SuperiorScaling::FallbackTransfer;
    }
    // Tone-mapped frames keep the Paper white scale; a linear HDR frame is shown at its base white point, or at the
    // white point in force when no exposure is known yet.
    encode.DebugScale = !linearHdr                ? cfg.DlssNrWhitePointScale.value_or_default()
                        : debugWhitePoint > 0.0f ? debugWhitePoint
                                                 : whitePoint;
    encode.GuideWidth = guideWidth;
    encode.GuideHeight = guideHeight;

    // The exposure fields the shader uses to recompute the white point from a live exposure image. The
    // resolve below is copied from this, so it carries them too. Automatic exposure switches the live
    // path on further down, once the meter has actually run.
    {
        const bool automatic = requestedWhitePointSource == 3;
        const auto anchors = DlssNrTrim::Parse(automatic ? cfg.DlssNrAutoExposureTrimAnchors.value_or_default()
                                                         : cfg.DlssNrGameExposureTrimAnchors.value_or_default());
        encode.PreExposure = g_vk.gamePreExposure;
        DlssNrTrim::FillConstants(encode,
                                  automatic ? DlssNr::AutoTrimEffective(cfg)
                                            : cfg.DlssNrWhitePointTrim.value_or_default(),
                                  anchors,
                                  automatic ? cfg.DlssNrAutoExposureTrimPreview.value_or_default()
                                            : cfg.DlssNrGameExposureTrimPreview.value_or_default(),
                                  cfg.DlssNrAutoExposureShadowProtection.value_or_default());
    }

    const VkImageSubresourceRange colourRange = colour->Resource.ImageViewInfo.SubresourceRange;

    // Open the measurement. Reset immediately before writing: a query pool slot must be reset before
    // it is written again, and doing it here rather than at the end keeps the two in one place.
    const uint32_t timingSlot = (uint32_t) (g_vk.timedFrames % kTimingSlots);

    if (g_vk.queryPool != VK_NULL_HANDLE)
    {
        vkCmdResetQueryPool(cmdBuffer, g_vk.queryPool, timingSlot * 2, 2);
        vkCmdWriteTimestamp(cmdBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_vk.queryPool, timingSlot * 2);
    }

    // The game's colour is read here and written at the end. Its layout on arrival is GENERAL, which
    // is what NGX requires of a resource it is handed, so it is left alone.
    Transition(cmdBuffer, g_vk.proxy, VK_IMAGE_LAYOUT_GENERAL);
    Transition(cmdBuffer, g_vk.keep, VK_IMAGE_LAYOUT_GENERAL);

    // Automatic exposure (source 3): meter the frame the encode is about to read, reduce the 4096 tile
    // means to one value, and hand it to the encode and resolve in the motion slot so this frame uses
    // this frame's exposure. Only on linear HDR: an already tone-mapped frame has no scene to meter.
    // The image read is the upscaler's fresh output, before anything of ours has written to it.
    g_vk.autoExposureActive = false;
    g_vk.autoExposureAdapting = false;

    if (requestedWhitePointSource == 3 && linearHdr && !displayWhite && g_vk.meter.Valid() && g_vk.autoExposure.Valid())
    {
        const VkImageLayout meterInputLayout =
            beforeSr ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;

        DlssNrConstants meterParams {};
        meterParams.Mode = DlssNrMode_Meter;
        meterParams.Width = kMeterSide;
        meterParams.Height = kMeterSide;
        meterParams.MeterCopiesExposure = 0;
        meterParams.InputEncoding = shaderConversion; // reads the game's frame

        Transition(cmdBuffer, g_vk.meter, VK_IMAGE_LAYOUT_GENERAL);

        if (g_vk.pass->Dispatch(cmdBuffer, meterParams, kMeterSide, kMeterSide,
                                colour->Resource.ImageViewInfo.ImageView, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                VK_NULL_HANDLE, g_vk.meter.view, VK_NULL_HANDLE, meterInputLayout))
        {
            Transition(cmdBuffer, g_vk.meter, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            Transition(cmdBuffer, g_vk.autoExposure, VK_IMAGE_LAYOUT_GENERAL);

            DlssNrConstants reduce {};
            reduce.Mode = DlssNrMode_AutoExposure;
            reduce.Width = 1;
            reduce.Height = 1;
            reduce.PreExposure = g_vk.gamePreExposure;
            reduce.ExposureSourceWidth = width;
            reduce.ExposureSourceHeight = height;
            reduce.AutoExposureShadowProtection =
                std::clamp(cfg.DlssNrAutoExposureShadowProtection.value_or_default(), 0.0f, 100.0f);
            SetAutoExposureMeter(reduce, cfg.DlssNrAutoExposureMeter.value_or_default() != DlssNrExposureMeter::kAverage,
                                 cfg.DlssNrAutoExposureMeterLowPercent.value_or_default(),
                                 cfg.DlssNrAutoExposureMeterHighPercent.value_or_default());

            // Eye adaptation (DlssNr_ExposureAdapt.h): the reading goes to autoExposureRaw and a one-texel pass eases
            // autoExposure toward it. Without that image or the pass, the reduce writes autoExposure itself, as
            // before; the frames it does so leave a gap the adapter snaps across.
            const float brighterSeconds = DlssNrExposureAdapt::Seconds(
                cfg.DlssNrAutoExposureAdaptBrighterSeconds.value_or_default(), DlssNrExposureAdapt::kDefaultBrighterSeconds);
            const float darkerSeconds =
                DlssNrExposureAdapt::Seconds(cfg.DlssNrAutoExposureAdaptDarkerSeconds.value_or_default());
            const bool adapting = (brighterSeconds > 0.0f || darkerSeconds > 0.0f) && g_vk.autoExposureRaw.Valid() &&
                                  g_vk.pass->ExposureAdaptReady();

            if (adapting)
                Transition(cmdBuffer, g_vk.autoExposureRaw, VK_IMAGE_LAYOUT_GENERAL);

            if (g_vk.pass->Dispatch(cmdBuffer, reduce, 1, 1, g_vk.meter.view, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                    VK_NULL_HANDLE, adapting ? g_vk.autoExposureRaw.view : g_vk.autoExposure.view,
                                    VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
            {
                if (adapting)
                {
                    Transition(cmdBuffer, g_vk.autoExposureRaw, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

                    // g_vk.frames counts NR frames, so one Automatic skipped is a gap; g_vk.reset is the game's cut
                    // (and a rebuilt feature).
                    const double now =
                        std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
                    const DlssNrExposureAdapt::Step step =
                        g_vk.autoExposureAdapter.Next(g_vk.frames, now, brighterSeconds, darkerSeconds, g_vk.reset);

                    // Overlays the first fields (dlssnr_exposure_adapt.hlsl): WhitePoint carries the brighter blend, Width
                    // the snap, Height the darker blend.
                    DlssNrConstants adaptParams {};
                    adaptParams.Mode = DlssNrMode_AutoExposure;
                    adaptParams.WhitePoint = step.blendBrighter;
                    adaptParams.Width = step.snap ? 1u : 0u;
                    adaptParams.Height = step.DarkerBits();

                    if (g_vk.pass->DispatchExposureAdapt(cmdBuffer, adaptParams, g_vk.autoExposureRaw.view,
                                                         g_vk.autoExposure.view))
                        g_vk.autoExposureAdapting = true;
                    else
                        g_vk.autoExposureAdapter.Invalidate();
                }

                Transition(cmdBuffer, g_vk.autoExposure, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                g_vk.autoExposureActive = true;
                encode.UseExposureWhitePoint = g_vk.followingGame || g_vk.foundBaseWhitePoint > 0.0f ? 0u : 1u;

                // Queue the value for readback, for the menu and anchor capture only.
                const unsigned long long slot = g_vk.meterFrames % kMeterSlots;

                if (g_vk.meterReadback[slot] != VK_NULL_HANDLE)
                {
                    g_vk.meterExposureKind[slot] = 2u;
                    g_vk.meterExposurePreExposure[slot] =
                        std::isfinite(g_vk.gamePreExposure) && g_vk.gamePreExposure > 1e-6f ? g_vk.gamePreExposure
                                                                                            : 1.0f;

                    Transition(cmdBuffer, g_vk.autoExposure, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

                    VkBufferImageCopy region {};
                    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                    region.imageExtent = { 1, 1, 1 };
                    vkCmdCopyImageToBuffer(cmdBuffer, g_vk.autoExposure.image,
                                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_vk.meterReadback[slot], 1,
                                           &region);

                    VkBufferMemoryBarrier toHost {};
                    toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                    toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                    toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    toHost.buffer = g_vk.meterReadback[slot];
                    toHost.offset = 0;
                    toHost.size = sizeof(float);
                    vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                                         0, nullptr, 1, &toHost, 0, nullptr);

                    // Back to the layout the encode and resolve bind it in.
                    Transition(cmdBuffer, g_vk.autoExposure, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

                    // The reading before eye adaptation, in the slot's third float, for the log.
                    g_vk.meterHasRaw[slot] = g_vk.autoExposureAdapting;

                    if (g_vk.autoExposureAdapting)
                    {
                        Transition(cmdBuffer, g_vk.autoExposureRaw, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

                        VkBufferImageCopy rawRegion = region;
                        rawRegion.bufferOffset = 2 * sizeof(float);
                        vkCmdCopyImageToBuffer(cmdBuffer, g_vk.autoExposureRaw.image,
                                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_vk.meterReadback[slot], 1,
                                               &rawRegion);

                        VkBufferMemoryBarrier rawToHost = toHost;
                        rawToHost.offset = 2 * sizeof(float);
                        rawToHost.size = sizeof(float);
                        vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                                             0, nullptr, 1, &rawToHost, 0, nullptr);
                    }

                    // The game's exposure beside Automatic's, for following it (DlssNr_FollowGame.h) and as the exposure
                    // scan's known answer (as on D3D12, DlssNr_Dx12.cpp). The game's image is read through the same
                    // layout guess Game exposure's courier uses, so it is only touched where it can matter: while
                    // following is on or the scan runs. The meter's tiles are reduced already.
                    g_vk.meterPairHasGame[slot] = false;

                    if (exposure != nullptr && exposure->Type == NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW &&
                        exposure->Resource.ImageViewInfo.ImageView != VK_NULL_HANDLE &&
                        (DlssNr::FollowGameOn(cfg) || DlssNr::ExposureScan::Scanning()))
                    {
                        DlssNrConstants courier {};
                        courier.Mode = DlssNrMode_Meter;
                        courier.Width = kMeterSide;
                        courier.Height = kMeterSide;
                        courier.MeterCopiesExposure = 1;

                        Transition(cmdBuffer, g_vk.meter, VK_IMAGE_LAYOUT_GENERAL);

                        if (g_vk.pass->Dispatch(cmdBuffer, courier, kMeterSide, kMeterSide, VK_NULL_HANDLE,
                                                VK_NULL_HANDLE, VK_NULL_HANDLE,
                                                exposure->Resource.ImageViewInfo.ImageView, g_vk.meter.view,
                                                VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
                        {
                            Transition(cmdBuffer, g_vk.meter, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

                            VkBufferImageCopy pair {};
                            pair.bufferOffset = sizeof(float);
                            pair.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                            pair.imageExtent = { 1, 1, 1 };
                            vkCmdCopyImageToBuffer(cmdBuffer, g_vk.meter.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                   g_vk.meterReadback[slot], 1, &pair);

                            VkBufferMemoryBarrier pairToHost = toHost;
                            pairToHost.offset = sizeof(float);
                            pairToHost.size = sizeof(float);
                            vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                                 0, 0, nullptr, 1, &pairToHost, 0, nullptr);

                            g_vk.meterPairHasGame[slot] = true;
                        }
                    }

                    g_vk.meterFrames++;
                    g_vk.autoExposureFrames++;
                }
            }
        }
    }

    // Game exposure read live: the host value is a readback 3-4 frames old, so on a cut the old
    // white point stood for those frames, a flash or dip D3D12 does not have. Once the game's image has read cleanly
    // through the courier's layout guess (LiveGameGate), it is bound in the motion slot like Automatic's and the shader
    // recomputes PreExposure / exposure x the Trim from it, falling back to the host value for an implausible texel. Not
    // under Native look, whose base is 1 whatever the exposure (DlssNr_GameScale.h), nor on display light.
    VkImageView liveExposureView = g_vk.autoExposureActive ? g_vk.autoExposure.view : VK_NULL_HANDLE;
    {
        const bool gameImage = exposure != nullptr && exposure->Type == NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW &&
                               exposure->Resource.ImageViewInfo.ImageView != VK_NULL_HANDLE;
        const bool liveGame = requestedWhitePointSource == 1 && linearHdr && !displayWhite && gameImage &&
                              DlssNrGameScale::ShaderReadsGameExposure(DlssNr::GameExposureScaleEffective(cfg)) &&
                              g_vk.gameLiveGate.Trusted();

        if (liveGame)
        {
            liveExposureView = exposure->Resource.ImageViewInfo.ImageView;
            encode.UseExposureWhitePoint = 1u;
            encode.ExposureBaseScale = 1.0f;

            if (!g_vk.gameLiveSaid)
            {
                g_vk.gameLiveSaid = true;
                LOG_INFO("DLSS-NR Vulkan: the game's exposure reads live ({} of {} reads believable)",
                         g_vk.gameLiveGate.valid, g_vk.gameLiveGate.reads);
            }
        }
    }

    // "Tune for this scene" (DlssNr_ExposureCalibrate_Vk.inl), after Automatic's meter so its availability is this
    // frame's: a run's step pins the white point of the encode and resolve (the resolve and the downsample copy the
    // encode's constants), and the live exposure is not recomputed in the shader, so every source holds still for
    // the run. While no run is on and the menu is not looking, only a timestamp.
    const bool calibrationGameExposure = exposure != nullptr &&
                                         exposure->Type == NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW &&
                                         exposure->Resource.ImageViewInfo.ImageView != VK_NULL_HANDLE;
    const float calibrationWhitePoint = CalibrationVkBeginFrame(cfg, width, height, linearHdr,
                                                                g_vk.autoExposureActive, calibrationGameExposure,
                                                                DlssNrColourEncoding::ShaderConverts(shaderConversion));
    const bool calibrationPinned = calibrationWhitePoint > 0.0f;

    if (calibrationPinned)
    {
        encode.WhitePoint = calibrationWhitePoint;
        encode.UseExposureWhitePoint = 0u;
    }

    // The LUT-apply epic: grade the frame through a loaded .cube file before the model sees it. Placed after the
    // Automatic meter above, which must keep reading the clean frame (a grade shifts apparent brightness by a
    // content-dependent amount, so metering it made exposure drift on D3D12), and right before the encode, which is
    // the only other reader of the game's colour ahead of the model. The encode writes `keep` from the same source it
    // reads, so handing it the graded image makes `keep` carry the grade too, and the game's resource is never
    // transitioned or written here -- only read, in the layout it arrived in. Skipped entirely without a LutFile.
    VkImageView encodeSource = colour->Resource.ImageViewInfo.ImageView;
    VkImageLayout encodeSourceLayout = beforeSr ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
    {
        const std::string lutPath = cfg.DlssNrLutFile.value_or_default();

        if (!lutPath.empty())
        {
            const float lutStrength = std::clamp(cfg.DlssNrLutStrength.value_or_default(), 0.0f, 1.0f);

            if (!g_vk.lutScratch.Valid() && !g_vk.lutScratchFailed)
            {
                g_vk.lutScratchFailed = !CreateImage(g_vk.lutScratch, width, height, VK_FORMAT_R16G16B16A16_SFLOAT, true);

                if (g_vk.lutScratchFailed)
                    LOG_ERROR("DLSS-NR Vulkan: could not allocate the LUT pass's scratch target; LutFile is ignored");
            }

            if (g_vk.lutScratchFailed)
            {
                DlssNr::ReportLutStatus(true, g_vk.lut.loadedPath, g_vk.lut.lut.size, true,
                                        "could not allocate the LUT pass's scratch target", lutPath,
                                        cfg.DlssNrLutStrength.value_or_default());
            }
            else
            {
                bool graded = false;
                DlssNr_LutEnsureParsed(&g_vk.lut, lutPath);

                if (g_vk.lut.Loaded())
                {
                    if (!g_vk.lutPass)
                        g_vk.lutPass = std::make_unique<DlssNr_LutVk>(device, physicalDevice);

                    // Replacing the 3D image (another file, or another size) while earlier frames still sample the
                    // old one would free it under them; a rare, user-initiated change, so a drain is fine.
                    if (g_vk.lutPass->Ready() && g_vk.lutPass->NeedsDrainFor(g_vk.lut.lut.size, g_vk.lut.loadedPath))
                        vkDeviceWaitIdle(device);

                    if (g_vk.lutPass->Ready() && g_vk.lutPass->EnsureLutImage(cmdBuffer, g_vk.lut.lut.rgb.data(),
                                                                              g_vk.lut.lut.size, g_vk.lut.loadedPath))
                    {
                        Transition(cmdBuffer, g_vk.lutScratch, VK_IMAGE_LAYOUT_GENERAL);

                        DlssNrLutConstants lutConstants {};
                        lutConstants.Width = width;
                        lutConstants.Height = height;
                        lutConstants.Strength = lutStrength;
                        lutConstants.DomainMinR = g_vk.lut.lut.domainMin[0];
                        lutConstants.DomainMinG = g_vk.lut.lut.domainMin[1];
                        lutConstants.DomainMinB = g_vk.lut.lut.domainMin[2];
                        lutConstants.DomainMaxR = g_vk.lut.lut.domainMax[0];
                        lutConstants.DomainMaxG = g_vk.lut.lut.domainMax[1];
                        lutConstants.DomainMaxB = g_vk.lut.lut.domainMax[2];
                        lutConstants.LutSize = (uint32_t) g_vk.lut.lut.size;
                        lutConstants.LatticeCoord = (uint32_t) g_vk.lut.lut.latticeCoord;
                        lutConstants.InputEncoding = shaderConversion;
                        lutConstants.ColourIsLinearHdr = linearHdr ? 1u : 0u;
                        // The white point the encode below uses (Tune's pinned value included), so linear HDR lands
                        // in the LUT's 0-1 domain where the model's proxy puts it; the Trim alone would leave a game
                        // whose frame is scaled by its exposure far above white. With the encode's live fields and
                        // image, so where the encode recomputes it this frame the LUT does too.
                        CopyLiveWhitePoint(lutConstants, encode);
                        if (liveExposureView == VK_NULL_HANDLE)
                            lutConstants.UseGameExposure = lutConstants.UseExposureWhitePoint = 0u;

                        graded = g_vk.lutPass->Dispatch(cmdBuffer, colour->Resource.ImageViewInfo.ImageView,
                                                        encodeSourceLayout, g_vk.lutScratch.view, lutConstants,
                                                        liveExposureView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    }
                }

                DlssNr::ReportLutStatus(true, g_vk.lut.loadedPath, g_vk.lut.lut.size, g_vk.lut.failed, g_vk.lut.error,
                                        g_vk.lut.attemptedPath, lutStrength);

                // A file that did not parse, or a pass that did not build, leaves the frame ungraded -- never a failure
                // of the whole pass.
                if (graded)
                {
                    encodeSource = g_vk.lutScratch.view;
                    encodeSourceLayout = VK_IMAGE_LAYOUT_GENERAL;
                }
            }
        }
        else
        {
            if (g_vk.lutScratch.Valid() || (g_vk.lutPass && g_vk.lutPass->HoldsImage()))
            {
                // LutFile was cleared: neither the scratch nor the 3D image (up to ~16 MB for a 128^3 lattice) is
                // wanted by anything else. Earlier frames may still read them, so drain once; one hitch, and every
                // later frame without a LutFile costs nothing.
                vkDeviceWaitIdle(device);
                DestroyImage(g_vk.lutScratch);

                if (g_vk.lutPass)
                    g_vk.lutPass->ReleaseLutImage();
            }

            g_vk.lutScratchFailed = false;
            DlssNr::ReportLutStatus(false, "", 0, false, "", "", cfg.DlssNrLutStrength.value_or_default());
        }
    }

    // Read in GENERAL, which is the layout it is actually in.
    //
    // This slot used to take the default and declare SHADER_READ_ONLY_OPTIMAL, which disagreed with
    // the comment four lines up and with the resolve below -- the resolve writes this same image as a
    // storage image, which is only legal in GENERAL, and nothing transitions it in between. It is the
    // upscaler's output, a storage image the upscaler has just written, so GENERAL is what it is.
    // Inert on the only hardware this model runs on, wrong everywhere it is read.
    if (!g_vk.pass->Dispatch(cmdBuffer, encode, width, height, encodeSource, VK_NULL_HANDLE, VK_NULL_HANDLE,
                             liveExposureView,
                             g_vk.proxy.view, g_vk.keep.view, encodeSourceLayout))
    {
        Fail("the encode dispatch failed");
        return;
    }

    CalibrationVkCopyInput(cmdBuffer, width, height);

    // The model's input: the full proxy, or a downsampled copy of it when the working scale is below
    // the frame. Mirrors the D3D12 path -- the encode always writes a full proxy, and a separate
    // downsample makes the small one the model actually reads.
    OwnedImage* modelInput = &g_vk.proxy;

    if (reduced && (!spatial || replaceCorrection) && g_vk.proxySmall.Valid())
    {
        bool built = false;

        if (workScale > 1.0f)
        {
            // Supersample: upscale the proxy to the super-native working size with the chosen filter so
            // the model sees a clean input. Rebuild both scalers when the NR downscaler changed (baked
            // at construction). proxy -> SHADER_READ_ONLY (sampled), proxySmall -> GENERAL (storage).
            const Scaler wantScaler = cfg.DlssNrScalingDownscaler.value_or_default();
            if (g_vk.nrScaler != wantScaler)
            {
                // Rebuilding frees the old scalers' pipelines/descriptors. The filter dropdown changes
                // no size, so this does NOT go through the resize block's drain -- and prior frames'
                // submitted command buffers still bind these pipelines. Freeing them under in-flight GPU
                // work is device removal (the same hazard the resize path drains for). Drain first. A
                // filter change is rare, so the one-off stall is a hitch, not a per-frame cost.
                if (g_vk.device != VK_NULL_HANDLE)
                    vkDeviceWaitIdle(g_vk.device);
                g_vk.superUp.reset();
                g_vk.superDown.reset();
                g_vk.nrScaler = wantScaler;
            }
            if (!g_vk.superUp)
                g_vk.superUp = std::make_unique<OS_Vk>("DLSS-NR VK supersample up", device, physicalDevice,
                                                       true, wantScaler);
            if (!g_vk.superDown)
                g_vk.superDown = std::make_unique<OS_Vk>("DLSS-NR VK supersample down", device,
                                                         physicalDevice, false, wantScaler);

            Transition(cmdBuffer, g_vk.proxy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            Transition(cmdBuffer, g_vk.proxySmall, VK_IMAGE_LAYOUT_GENERAL);

            VkImageInfo upin = ImageInfoOf(g_vk.proxy);
            VkImageInfo upout = ImageInfoOf(g_vk.proxySmall);

            if (g_vk.superUp && g_vk.superUp->IsInit() && g_vk.superUp->Dispatch(cmdBuffer, upin, upout))
                built = true;
            else
            {
                static bool warnedVkSuper = false;
                if (!warnedVkSuper)
                {
                    warnedVkSuper = true;
                    LOG_WARN("DLSS-NR Vulkan supersample: upscaler unavailable, falling back to box enlarge.");
                }
            }
        }

        if (!built)
        {
            DlssNrConstants down = encode;
            down.Mode = DlssNrMode_Downsample;
            down.Width = workWidth;
            down.Height = workHeight;

            Transition(cmdBuffer, g_vk.proxy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            Transition(cmdBuffer, g_vk.proxySmall, VK_IMAGE_LAYOUT_GENERAL);

            if (!g_vk.pass->Dispatch(cmdBuffer, down, workWidth, workHeight, g_vk.proxy.view, VK_NULL_HANDLE,
                                     VK_NULL_HANDLE, VK_NULL_HANDLE, g_vk.proxySmall.view, VK_NULL_HANDLE,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
            {
                Fail("the downsample dispatch failed");
                return;
            }
        }

        modelInput = &g_vk.proxySmall;
    }

    // -----------------------------------------------------------------------------------------
    // The meter: the game's 1x1 exposure -> texel (0,0) of the grid -> a buffer the CPU can read
    // -----------------------------------------------------------------------------------------

    // The motion slot carries it, because the meter has no use for motion vectors and the shader is
    // one shader with a fixed set of bindings. The source slot is left empty and gets the dummy.
    //
    // Gated on the setting that consumes the answer, which is not merely tidy. This is the only place
    // the pass binds a resource it does not own on a guess about its layout, and the guess is good
    // but it is still a guess. A user who has not asked for the exposure source never has the game's
    // image touched at all, so if some engine does leave it somewhere unexpected, the blast radius is
    // people who turned the thing on rather than everyone on Vulkan.
    if (DlssNr::WhitePointSourceEffective(cfg) == 1 && exposure != nullptr &&
        exposure->Type == NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW &&
        exposure->Resource.ImageViewInfo.ImageView != VK_NULL_HANDLE && g_vk.meter.Valid())
    {
        const unsigned long long slot = g_vk.meterFrames % kMeterSlots;

        if (g_vk.meterReadback[slot] != VK_NULL_HANDLE)
        {
            DlssNrConstants meter = encode;
            meter.Mode = DlssNrMode_Meter;
            meter.Width = kMeterSide;
            meter.Height = kMeterSide;
            // A courier for the game's exposure into tile (0,0), not an average of tile pixels.
            meter.MeterCopiesExposure = 1;
            g_vk.meterExposureKind[slot] = 1u;
            g_vk.meterExposurePreExposure[slot] = g_vk.gamePreExposure;

            Transition(cmdBuffer, g_vk.meter, VK_IMAGE_LAYOUT_GENERAL);

            if (g_vk.pass->Dispatch(cmdBuffer, meter, kMeterSide, kMeterSide, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                    VK_NULL_HANDLE, exposure->Resource.ImageViewInfo.ImageView, g_vk.meter.view,
                                    VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
            {
                Transition(cmdBuffer, g_vk.meter, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

                VkBufferImageCopy region {};
                region.bufferOffset = 0;
                region.bufferRowLength = 0;
                region.bufferImageHeight = 0;
                region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                region.imageOffset = { 0, 0, 0 };
                region.imageExtent = { kMeterSide, kMeterSide, 1 };

                vkCmdCopyImageToBuffer(cmdBuffer, g_vk.meter.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       g_vk.meterReadback[slot], 1, &region);

                // The copy has to be visible to a host read, and only the host will read it.
                VkBufferMemoryBarrier toHost {};
                toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toHost.buffer = g_vk.meterReadback[slot];
                toHost.offset = 0;
                toHost.size = kMeterBytes;

                vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                                     nullptr, 1, &toHost, 0, nullptr);

                g_vk.meterFrames++;
            }
        }
    }

    // -----------------------------------------------------------------------------------------
    // The model
    // -----------------------------------------------------------------------------------------

    float mvX = 1.0f, mvY = 1.0f;
    params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &mvX);
    params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &mvY);
    const float gameMvX = mvX, gameMvY = mvY;

    // Compress screen edges: pack the proxy, the depth and the motion into the smaller picture the model works on. The
    // vectors are taken to native pixels with the game's scale first, and the packed ones are exact in packed pixels,
    // so the model's scale is 1. A failed pack gives the frame up (the game's picture stays as the upscaler left it);
    // compression is what is turned off.
    NVSDK_NGX_Resource_VK* modelDepth = depth;
    NVSDK_NGX_Resource_VK* modelMotion = motion;

    // What the uncompressed path would show the model at this Model resolution, on the ordinary grid: the encoded proxy
    // itself at 100%, else the picture built above the way that path builds it (box downsample below 100%, the NR
    // upscaler above). Only Replace needs it.
    OwnedImage* spatialReference = modelInput;

    if (spatial)
    {
        using DlssNr::Spatial::MakeConstants;
        const auto& spatialLayout = EdgeCompressionVk::Layout();
        const auto depthRead = EdgeCompressionVk::DepthRead(device, depth);
        const DlssNrSpatial_Vk::Read motionRead { motion->Resource.ImageViewInfo.ImageView,
                                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

        Transition(cmdBuffer, g_vk.proxy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Transition(cmdBuffer, EdgeCompressionVk::color, VK_IMAGE_LAYOUT_GENERAL);
        Transition(cmdBuffer, EdgeCompressionVk::depth, VK_IMAGE_LAYOUT_GENERAL);
        Transition(cmdBuffer, EdgeCompressionVk::motion, VK_IMAGE_LAYOUT_GENERAL);

        const DlssNrSpatial_Vk::Read colourReads[3] = { { g_vk.proxy.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                                                         {}, {} };
        const DlssNrSpatial_Vk::Read guideReads[3] = { {}, depthRead, motionRead };

        const bool packed =
            depthRead.view != VK_NULL_HANDLE &&
            EdgeCompressionVk::pass->Dispatch(cmdBuffer, MakeConstants(spatialLayout, 100, guides, 1.0f, 1.0f),
                                              colourReads, EdgeCompressionVk::color.view, VK_NULL_HANDLE) &&
            EdgeCompressionVk::pass->Dispatch(cmdBuffer, MakeConstants(spatialLayout, 101, guides, gameMvX, gameMvY),
                                              guideReads, EdgeCompressionVk::depth.view, EdgeCompressionVk::motion.view);

        if (!packed)
        {
            EdgeCompressionVk::TurnOff(Spatial::Status::TurnedOffDispatch);
            g_vk.reset = true;
            return;
        }

        modelInput = &EdgeCompressionVk::color;
        modelDepth = &EdgeCompressionVk::depth.ngx;
        modelMotion = &EdgeCompressionVk::motion.ngx;
        Transition(cmdBuffer, EdgeCompressionVk::depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Transition(cmdBuffer, EdgeCompressionVk::motion, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        mvX = mvY = 1.0f;
    }
    else
    {
        // Match D3D12: preserve the game's vector encoding, then adjust only for the NR working scale.
        mvX *= (float) workWidth / width;
        mvY *= (float) workHeight / height;
    }

    // Experimental input styles (Config.h DlssNrInputFlatten ..., DlssNr_InputStyles.h), as on D3D12: restyle the model's
    // picture once it is final (after the shrink and the edge packing), each style that is on in turn, each on the one
    // before's result, alternating between two images. The last result stands in for the model input and the resolve's
    // proxy (ordinaryProxy below, and through the edge unpacking), so Composed hands back only what the model painted on
    // top of it. Skipped above 100% model resolution, where the resolve compares against the full proxy instead.
    bool inputRestyled = false;
    {
        const DlssNrInput::Styles styles = cfg.DlssNrInputStyles();
        DlssNrInput::Pass passes[2];
        const int passCount = DlssNrInput::Passes(styles, passes);

        const char* skip = passCount == 0 ? nullptr : workScale > 1.0f ? "model resolution above 100%" : nullptr;
        int ran = 0;

        if (passCount > 0 && skip == nullptr)
        {
            OwnedImage* targets[2] = { &g_vk.styled, &g_vk.styledB };
            const int targetCount = passCount > 1 ? 2 : 1;
            bool waited = false;

            for (int t = 0; t < targetCount; ++t)
            {
                OwnedImage& target = *targets[t];

                if (target.Valid() && (target.width != modelInput->width || target.height != modelInput->height ||
                                       target.format != modelInput->format))
                {
                    // Earlier frames may still read it (a switch of Compress screen edges changes the model picture's
                    // size); one wait covers both images
                    if (!waited)
                        vkDeviceWaitIdle(device);

                    waited = true;
                    DestroyImage(target);
                }

                if (!target.Valid())
                    CreateImage(target, modelInput->width, modelInput->height, modelInput->format, true);
            }

            OwnedImage* source = modelInput;

            for (int k = 0; k < passCount; ++k)
            {
                OwnedImage& target = *targets[k % 2];

                if (!target.Valid())
                {
                    skip = "its image could not be created";
                    break;
                }

                DlssNrConstants paint = encode;
                paint.Mode = StyleMode(passes[k].style);
                paint.Width = source->width;
                paint.Height = source->height;
                paint.TransferStrength = passes[k].strength;
                paint.SkinProtection = styles.facesOnly ? 1u : 0u; // the style passes read it as "faces only"

                Transition(cmdBuffer, *source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                Transition(cmdBuffer, target, VK_IMAGE_LAYOUT_GENERAL);

                // A failed dispatch: the model gets the picture up to the last style that ran
                if (!g_vk.pass->Dispatch(cmdBuffer, paint, source->width, source->height, source->view, VK_NULL_HANDLE,
                                         VK_NULL_HANDLE, VK_NULL_HANDLE, target.view, VK_NULL_HANDLE,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
                {
                    skip = "its dispatch failed";
                    break;
                }

                source = &target;
                ++ran;
            }

            if (ran > 0)
            {
                modelInput = source;
                inputRestyled = true;
            }
        }

        // Said when it changes, so a log shows each A/B toggle.
        static std::string said = "-";
        std::string state;

        for (int k = 0; k < ran; ++k)
            state += std::format("{}{} {:.2f}", k == 0 ? "" : ", ", StyleName(passes[k].style), passes[k].strength);

        if (skip != nullptr)
            state += std::format("{}skipped ({})", ran > 0 ? "; then " : "", skip);

        if (state != said)
        {
            said = state;

            if (ran > 0)
                LOG_INFO("DLSS-NR Vulkan input style: {} ({}x{}{}{})", state, modelInput->width, modelInput->height,
                         styles.facesOnly ? ", faces only" : "", spatial ? ", edge-compressed picture" : "");
            else if (skip != nullptr)
                LOG_INFO("DLSS-NR Vulkan input style: {}", state);
            else
                LOG_INFO("DLSS-NR Vulkan input style: off");
        }
    }

    OwnedImage* answer = &g_vk.output;
    OwnedImage* input = modelInput;
    int evaluated = 1;

    // Reuse detail between frames (DlssNr_DetailReuse_Vk.inl): every other frame skips the model and moves the previous
    // frame's detail onto this frame's input instead. On such a frame the answer is in g_vk.output, in GENERAL.
    DetailReuseVk::Frame reuseFrame;
    reuseFrame.cfg = &cfg;
    reuseFrame.beforeUpscale = beforeSr;
    reuseFrame.workWidth = workWidth;
    reuseFrame.workHeight = workHeight;
    reuseFrame.motionWidth = guides.motion.width;
    reuseFrame.motionHeight = guides.motion.height;
    reuseFrame.motionBaseX = guides.motion.x;
    reuseFrame.motionBaseY = guides.motion.y;
    reuseFrame.motionAllocWidth = motion->Resource.ImageViewInfo.Width;
    reuseFrame.motionAllocHeight = motion->Resource.ImageViewInfo.Height;
    reuseFrame.depthWidth = guideWidth;
    reuseFrame.depthHeight = guideHeight;
    reuseFrame.depthBaseX = guides.depth.x;
    reuseFrame.depthBaseY = guides.depth.y;
    reuseFrame.depthInverted = depthInverted;
    reuseFrame.passthrough = !linearHdr;
    reuseFrame.reversibleMode = encode.ReversibleMode;
    reuseFrame.mvScaleX = gameMvX;
    reuseFrame.mvScaleY = gameMvY;
    reuseFrame.modelReset = g_vk.reset;
    reuseFrame.edgeCompression = spatial; // the two do not run together; Reuse detail says so in the menu
    // A Tune step pins the white point; a Measure detail run measures Reuse bottleneck as it runs.
    reuseFrame.blocked = calibrationPinned;
    reuseFrame.vulkan = true;
    reuseFrame.present = VkFrameClock();
    // NR's own frames, which step exactly once per evaluate: the present count is read on this thread while presents
    // happen on the game's, so two NR frames can see it move by 0 or 2 and the cadence would take that for a gap.
    reuseFrame.frameNumber = g_vk.frames;
    {
        unsigned long long revision = (unsigned long long) (uintptr_t) g_vk.feature;
        for (const unsigned long long part : { (unsigned long long) passes, (unsigned long long) workWidth,
                                               (unsigned long long) workHeight })
            revision = revision * 1000003ull ^ part;
        reuseFrame.revision = revision;
    }
    reuseFrame.cmd = cmdBuffer;
    reuseFrame.device = device;
    reuseFrame.physicalDevice = physicalDevice;
    reuseFrame.answerFormat = g_vk.output.format;
    reuseFrame.modelInput = modelInput;
    reuseFrame.output = &g_vk.output;
    reuseFrame.motion = motion;
    reuseFrame.depth = depth;
    const DetailReuseVk::Plan reusePlan = DetailReuseVk::BeforeModel(reuseFrame);

    if (reusePlan.resetModel)
        g_vk.reset = true;

    // A Tune run measures the first pass alone (see the D3D12 side for why); the later passes sit it out. A reused
    // frame runs none.
    const unsigned int runPasses = reusePlan.reused ? 0u : calibrationPinned ? 1u : passes;

    // Not on a reused frame: every pass skips it alike, and the next full frame gives them all composed vectors.
    if (!reusePlan.reused && runPasses < passes)
        g_vk.laterPassesNeedReset = true;

    for (unsigned int pass = 0; pass < runPasses; ++pass)
    {
        const bool passReset = g_vk.reset || (pass > 0 && g_vk.laterPassesNeedReset);
        Transition(cmdBuffer, *input, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Transition(cmdBuffer, *answer, VK_IMAGE_LAYOUT_GENERAL);
        const auto tuning = Profiles::PassTuning(cfg, pass);
        void* const passFeature = pass == 0 ? g_vk.feature : g_vk.laterFeatures[pass];
        // The evaluate bracket (DlssNrVitReuse.h, the hooks in DlssNrNative_Vk.cpp). Reuse bottleneck runs in copy mode
        // on both kernel sets here: with the ViT run skipped, its kept result was overwritten between frames on Vulkan
        // (RDR2: dark rooms flashed, at 1 pass too, also when the run's last kernel was kept) by something outside the
        // model's own launches, so the decoder gets our own copy, restored right before it reads it. While detail reuse
        // runs the bottleneck is computed every time (as on D3D12). No command list: the kernel profiler is D3D12's.
        const bool vitEveryFrame = reusePlan.active;
        DlssNrNative::VkEvaluateDevice(g_vk.device, g_vk.physicalDevice);
        DlssNrNative::BeginEvaluate(passFeature, passReset,
                                    vitEveryFrame ? 1u : std::clamp(cfg.DlssNrVitEvery.value_or_default(), 1u, 2u),
                                    vitEveryFrame ? 1u : std::clamp(cfg.DlssNrVitEveryPlain.value_or_default(), 1u, 2u),
                                    (long long) (g_vk.frames & 0x3FFFFFFFFFFFFFFFull), nullptr, false,
                                    DlssNrVitReuse::kCopyPlain | DlssNrVitReuse::kCopyFp8);
        evaluated = g_vk.evaluate(
            (void*) cmdBuffer, passFeature, g_vk.capabilityParams,
            &input->ngx, modelDepth, spatial ? modelMotion : reusePlan.motion, &answer->ngx, modelWidth, modelHeight,
            spatial ? modelWidth : guideWidth, spatial ? modelHeight : guideHeight,
            spatial ? modelWidth : guides.motion.width, spatial ? modelHeight : guides.motion.height,
            spatial ? 0u : guides.depth.x, spatial ? 0u : guides.depth.y,
            spatial ? 0u : reusePlan.motionBaseX, spatial ? 0u : reusePlan.motionBaseY, depthInverted ? 1 : 0,
            passReset ? 1 : 0, tuning.intensity,
            (int) Profiles::PassStyle(cfg, pass), tuning.structure, tuning.tone, tuning.skin,
            tuning.autoMask ? 1 : 0, mvX, mvY);
        if (DlssNrNative::EndEvaluate(nullptr))
            LOG_WARN("DLSS-NR Vulkan: Reuse bottleneck is {}", DlssNrNative::VitStatus());
        if (evaluated != 1)
            break;
        if (pass + 1 < runPasses)
        {
            input = answer;
            answer = answer == &g_vk.output ? &g_vk.scratch : &g_vk.output;
        }
    }

    // Once: did the floats reach the block? Before the float-slot probe ran on Vulkan they did not, and the model ran
    // with its default motion vector scale.
    static bool floatsChecked = false;
    if (!floatsChecked && evaluated == 1)
    {
        floatsChecked = true;
        float readX = 0.0f, readY = 0.0f;
        const bool okX = g_vk.capabilityParams->Get("DLSSNR.MVecScaleX", &readX) == NVSDK_NGX_Result_Success;
        const bool okY = g_vk.capabilityParams->Get("DLSSNR.MVecScaleY", &readY) == NVSDK_NGX_Result_Success;
        if (okX && okY && readX == mvX && readY == mvY)
            LOG_INFO("DLSS-NR Vulkan: the model got its motion vector scale ({:.4f}, {:.4f})", readX, readY);
        else
            LOG_WARN("DLSS-NR Vulkan: the motion vector scale did not reach the model (set {:.4f}, {:.4f}; read {} "
                     "{:.4f}, {:.4f})", mvX, mvY, okX && okY ? "back" : "nothing", readX, readY);
    }

    if (evaluated == 1 && runPasses > 1)
        g_vk.laterPassesNeedReset = false;

    // Steady a full frame's answer and save this frame's history (DlssNr_DetailReuse_Vk.inl).
    DetailReuseVk::AfterModel(reuseFrame, reusePlan, evaluated == 1, answer);

    g_vk.reset = false;
    g_vk.frames++;

    // Reuse bottleneck and the kernel set need the model's kernel launches to come through OptiScaler's Vulkan hook
    // table (VulkanwDx12_Hooks.cpp). If the model reached them some other way, say so once instead of doing nothing.
    static bool vitUnseenReported = false;
    if (!vitUnseenReported && g_vk.frames >= 120 && strcmp(DlssNrNative::VitKernelSet(), "not seen yet") == 0)
    {
        vitUnseenReported = true;
        LOG_WARN("DLSS-NR Vulkan: no ViT run of the model seen in 120 frames; the model's kernel launches do not come "
                 "through OptiScaler's hooks here, so Reuse bottleneck and the kernel set are unavailable");
    }

    if (evaluated != 1)
    {
        LOG_ERROR("DLSS-NR Vulkan: evaluate returned {}", evaluated);

        if (spatial)
        {
            // The model would not take the packed picture. NR is fine; compression is what is turned off.
            EdgeCompressionVk::TurnOff(Spatial::Status::TurnedOffModel);
            g_vk.reset = true;
            return;
        }

        Fail("the model refused to evaluate");
        return;
    }

    // Compress screen edges: the packed model input and the model's answer back to the ordinary grid, both through the
    // same resampling, so what the resolve takes from the pair is the model's change. The resolve, the SGSR1 up-leg and
    // the supersample down-leg below work on these two instead of the packed ones.
    OwnedImage* ordinaryProxy = modelInput;
    OwnedImage* ordinaryAnswer = answer;

    if (spatial)
    {
        Transition(cmdBuffer, *modelInput, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Transition(cmdBuffer, *answer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Transition(cmdBuffer, EdgeCompressionVk::proxy, VK_IMAGE_LAYOUT_GENERAL);
        Transition(cmdBuffer, EdgeCompressionVk::answer, VK_IMAGE_LAYOUT_GENERAL);

        if (replaceCorrection)
            Transition(cmdBuffer, *spatialReference, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        const DlssNrSpatial_Vk::Read reads[3] = { { modelInput->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                                                   { answer->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                                                   { replaceCorrection ? spatialReference->view : VK_NULL_HANDLE,
                                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };

        if (!EdgeCompressionVk::pass->Dispatch(
                cmdBuffer,
                DlssNr::Spatial::MakeConstants(EdgeCompressionVk::Layout(), replaceCorrection ? 103 : 102, guides, 1.0f,
                                               1.0f),
                reads,
                EdgeCompressionVk::proxy.view, EdgeCompressionVk::answer.view))
        {
            EdgeCompressionVk::TurnOff(Spatial::Status::TurnedOffDispatch);
            g_vk.reset = true;
            return;
        }

        ordinaryProxy = &EdgeCompressionVk::proxy;
        ordinaryAnswer = &EdgeCompressionVk::answer;
    }

    // -----------------------------------------------------------------------------------------
    // Resolve: proxy + the model's answer + the untouched copy -> the frame
    // -----------------------------------------------------------------------------------------

    DlssNrConstants resolve = encode;
    resolve.Mode = DlssNrMode_Resolve;

    // The depth fade. Off, or a depth that cannot be read, leaves the block zero and the resolve as it was.
    const DlssNrDepthFade::Settings depthFade = DlssNrDepthFade::SettingsFrom(cfg);
    VkImageView fadeDepth = VK_NULL_HANDLE;
    if (depthFade.enabled)
        fadeDepth = DepthFadeVk::View(device, depth);
    else
        DepthFadeVk::Idle();
    resolve.DepthFade = DlssNrDepthFade::MakeBlock(depthFade, fadeDepth != VK_NULL_HANDLE, depthInverted,
                                                   { guideWidth, guideHeight, guides.depth.x, guides.depth.y });
    DlssNrDepthFade::Report(depthFade, fadeDepth != VK_NULL_HANDLE);
    if ((resolve.DepthFade.Flags & DlssNrDepthFade::kActive) == 0)
        fadeDepth = VK_NULL_HANDLE;
    // Replace's detail injection puts back the game frame's own fine texture, which is what a style removed: off while
    // the picture was restyled, so Replace shows the model's painting of the restyled picture.
    if (inputRestyled)
        resolve.ReplaceDetailStrength = 0.0f;

    // Supersampling down-leg (Vulkan). Average the Nx model answer back to native with the chosen
    // filter so the resolve composites a native answer against the native proxy 1:1 -- not the single
    // bilinear tap the Nx answer would otherwise get, which aliases the model's detail into noise. On
    // failure it falls back to the Nx pair (modelInput + output), the old behaviour.
    OwnedImage* resolveProxy = ordinaryProxy;
    OwnedImage* resolveAnswer = ordinaryAnswer;

    // Reduced up-leg (working scale < 1), mirroring D3D12's NrState::sgsr1UpAnswer block exactly.
    // DlssNrReducedUpscaleMethod: 0 Bilinear (answer not enlarged -- resolveAnswer above already
    // defaults to that), 1 SGSR1 answer. Built for one Dispatch()/frame -- see g_vk's own
    // sgsr1UpAnswer comment for why (a single instance would have its CPU-side constant write land
    // before the GPU dispatch executes).
    bool sgsrAnswerOk = false;
    const uint32_t upscaleMethod = cfg.DlssNrReducedUpscaleMethod.value_or_default();
    const bool wantsSgsr1Answer = upscaleMethod == 1;

    // Gated on `reduced` (the actual rounded-size flag), not just workScale < 1.0f -- a workScale
    // that rounds back to the native size would otherwise engage SGSR1 at 1:1, wasted work with no
    // correctness guarantee of being identity-preserving at unity. Matches D3D12's identical gate.
    if (reduced && workScale < 1.0f && wantsSgsr1Answer)
    {
        const float sgsr1EdgeThreshold = 0.300f;
        const float sgsr1EdgeSharpness = 2.00f;

        if (g_vk.outputNative.Valid())
        {
            if (!g_vk.sgsr1UpAnswer)
                g_vk.sgsr1UpAnswer =
                    std::make_unique<SGSR1_Vk>("DLSS-NR VK SGSR1 up (answer)", device, physicalDevice);

            if (g_vk.sgsr1UpAnswer && g_vk.sgsr1UpAnswer->IsInit())
            {
                Transition(cmdBuffer, *ordinaryAnswer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                Transition(cmdBuffer, g_vk.outputNative, VK_IMAGE_LAYOUT_GENERAL);

                VkImageInfo upAnswerIn = ImageInfoOf(*ordinaryAnswer);
                VkImageInfo upAnswerOut = ImageInfoOf(g_vk.outputNative);

                if (g_vk.sgsr1UpAnswer->Dispatch(cmdBuffer, upAnswerIn, upAnswerOut, resolve.ReversibleMode,
                                                 resolve.Passthrough, sgsr1EdgeThreshold, sgsr1EdgeSharpness))
                    sgsrAnswerOk = true;
            }
        }

        // INFO-level, change-gated like D3D12's own up-leg log, so a log scan can confirm Vulkan
        // parity the same way it already can for D3D12.
        static bool hasLoggedSgsrUp = false;
        static uint32_t lastLoggedState = 0xFFFFFFFFu;
        const uint32_t state = sgsrAnswerOk ? 1u : 0u;
        if (!hasLoggedSgsrUp || lastLoggedState != state)
        {
            LOG_INFO("DLSS-NR Vulkan SGSR1 up-leg: answer {} (method {}, workScale {:.3f}, "
                     "{}x{} -> {}x{})",
                     sgsrAnswerOk ? "engaged" : "NOT engaged", upscaleMethod,
                     workScale, workWidth, workHeight, width, height);
            lastLoggedState = state;
            hasLoggedSgsrUp = true;
        }
    }
    else if (reduced && workScale < 1.0f)
    {
        // Parity with the SGSR1 branch's own log above, change-gated the same way, so a log scan
        // can confirm the Bilinear choice actually took effect without needing a breakpoint.
        static bool hasLoggedBilinear = false;
        if (!hasLoggedBilinear)
        {
            LOG_INFO("DLSS-NR Vulkan reduced up-leg: Bilinear selected, SGSR1 skipped entirely "
                     "(workScale {:.3f}, {}x{} -> {}x{})",
                     workScale, workWidth, workHeight, width, height);
            hasLoggedBilinear = true;
        }
    }

    if (sgsrAnswerOk)
        resolveAnswer = &g_vk.outputNative;

    if (spatial && workScale > 1.0f)
    {
        // The proxy goes through the same filter as the answer, so the resolve compares like with like. If either
        // fails, the unpacked pair goes to the resolve as it is (both halves through the resolve's own tap).
        const Scaler wantScaler = cfg.DlssNrScalingDownscaler.value_or_default();
        if (g_vk.nrScaler != wantScaler)
        {
            // As the filter change below: the old scalers' pipelines are still bound by frames in flight.
            if (g_vk.device != VK_NULL_HANDLE)
                vkDeviceWaitIdle(g_vk.device);
            g_vk.superUp.reset();
            g_vk.superDown.reset();
            EdgeCompressionVk::proxyDown.reset();
            EdgeCompressionVk::proxyDownScaler = Scaler::Count;
            g_vk.nrScaler = wantScaler;
        }
        if (!g_vk.superDown)
            g_vk.superDown = std::make_unique<OS_Vk>("DLSS-NR VK supersample down", device, physicalDevice, false,
                                                     wantScaler);
        // The proxy's filter has to be the answer's: compression off and on again around a change of the NR downscaler
        // would otherwise leave it on the old one (superDown is rebuilt by the other path). Drained first, as above.
        if (EdgeCompressionVk::proxyDown && EdgeCompressionVk::proxyDownScaler != wantScaler)
        {
            if (g_vk.device != VK_NULL_HANDLE)
                vkDeviceWaitIdle(g_vk.device);
            EdgeCompressionVk::proxyDown.reset();
        }
        if (!EdgeCompressionVk::proxyDown)
        {
            EdgeCompressionVk::proxyDown = std::make_unique<OS_Vk>("DLSS-NR VK compress screen edges proxy down", device,
                                                                   physicalDevice, false, wantScaler);
            EdgeCompressionVk::proxyDownScaler = wantScaler;
        }

        if (g_vk.superDown && g_vk.superDown->IsInit() && EdgeCompressionVk::proxyDown &&
            EdgeCompressionVk::proxyDown->IsInit() && EdgeCompressionVk::proxyNative.Valid() &&
            EdgeCompressionVk::answerNative.Valid())
        {
            Transition(cmdBuffer, *ordinaryProxy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            Transition(cmdBuffer, *ordinaryAnswer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            Transition(cmdBuffer, EdgeCompressionVk::proxyNative, VK_IMAGE_LAYOUT_GENERAL);
            Transition(cmdBuffer, EdgeCompressionVk::answerNative, VK_IMAGE_LAYOUT_GENERAL);

            VkImageInfo proxyIn = ImageInfoOf(*ordinaryProxy);
            VkImageInfo proxyOut = ImageInfoOf(EdgeCompressionVk::proxyNative);
            VkImageInfo answerIn = ImageInfoOf(*ordinaryAnswer);
            VkImageInfo answerOut = ImageInfoOf(EdgeCompressionVk::answerNative);

            if (EdgeCompressionVk::proxyDown->Dispatch(cmdBuffer, proxyIn, proxyOut) &&
                g_vk.superDown->Dispatch(cmdBuffer, answerIn, answerOut))
            {
                resolveProxy = &EdgeCompressionVk::proxyNative;
                resolveAnswer = &EdgeCompressionVk::answerNative;
            }
        }
    }
    else if (workScale > 1.0f && g_vk.superDown && g_vk.superDown->IsInit() && g_vk.outputNative.Valid())
    {
        Transition(cmdBuffer, *answer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Transition(cmdBuffer, g_vk.outputNative, VK_IMAGE_LAYOUT_GENERAL);

        VkImageInfo dsin = ImageInfoOf(*answer);
        VkImageInfo dsout = ImageInfoOf(g_vk.outputNative);

        if (g_vk.superDown->Dispatch(cmdBuffer, dsin, dsout))
        {
            resolveProxy = &g_vk.proxy;
            resolveAnswer = &g_vk.outputNative;
        }
    }

    Transition(cmdBuffer, *resolveProxy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cmdBuffer, *resolveAnswer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cmdBuffer, g_vk.keep, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    if (beforeSr)
        Transition(cmdBuffer, g_vk.preColor, VK_IMAGE_LAYOUT_GENERAL);
    if (!g_vk.pass->Dispatch(cmdBuffer, resolve, width, height, resolveProxy->view, resolveAnswer->view, g_vk.keep.view,
                             liveExposureView, beforeSr ? g_vk.preColor.view : colour->Resource.ImageViewInfo.ImageView,
                             VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, fadeDepth))
    {
        Fail("the resolve dispatch failed");
        return;
    }

    ApplyPostLutVk(cmdBuffer, device, physicalDevice, cfg,
                   beforeSr ? g_vk.preColor.view : colour->Resource.ImageViewInfo.ImageView, beforeSr, width, height,
                   shaderConversion, linearHdr, encode, liveExposureView);

    if (beforeSr)
        Transition(cmdBuffer, g_vk.preColor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    CalibrationVkMeasure(cmdBuffer, beforeSr ? g_vk.preColor.view : colour->Resource.ImageViewInfo.ImageView,
                         beforeSr ? g_vk.preColor.layout : VK_IMAGE_LAYOUT_GENERAL, *ordinaryProxy, width, height);
    applied = true;

    // Close it, and read the pair from three frames ago -- retired by now, so the read does not wait.
    if (g_vk.queryPool != VK_NULL_HANDLE)
    {
        vkCmdWriteTimestamp(cmdBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_vk.queryPool, timingSlot * 2 + 1);
        g_vk.timedFrames++;

        if (g_vk.timedFrames > kTimingSlots)
        {
            const uint32_t readSlot = (uint32_t) (g_vk.timedFrames % kTimingSlots);
            uint64_t ticks[2] = {};

            // Without WAIT: a slot this old is retired, and if it somehow is not, NOT_READY is the
            // right answer rather than a stall.
            if (vkGetQueryPoolResults(device, g_vk.queryPool, readSlot * 2, 2, sizeof(ticks), ticks, sizeof(uint64_t),
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
                ticks[1] > ticks[0])
            {
                const double ms = (double) (ticks[1] - ticks[0]) * (double) g_vk.timestampPeriod / 1e6;

                // A pass that appears to have taken over a second did not; the queue was reset under
                // it or the pair straddled a device change.
                if (ms > 0.0 && ms < 1000.0)
                {
                    g_vk.lastGpuTime = ms;
                    // Each frame's pair is read once, so full and reused frames both land here.
                    DetailReuseVk::RecordGpuTime(ms);
                }
            }
        }
    }

    static bool reported = false;

    if (!reported && g_vk.frames > 2)
    {
        reported = true;
        LOG_INFO("DLSS-NR Vulkan: running {} SR at {}x{}, guides {}x{}", beforeSr ? "before" : "after",
                 width, height, guideWidth, guideHeight);
    }
}

NVSDK_NGX_Resource_VK* EvaluateBeforeUpscaleVk(VkCommandBuffer cmd, NVSDK_NGX_Parameter* params,
                                             VkInstance instance, VkPhysicalDevice pd, VkDevice device, bool& handled,
                                             bool rayReconstruction)
{
    handled = false;
    if (!Config::Instance()->DlssNrRunBeforeSr.value_or_default())
        return nullptr;
    // A Tune runs after SR while Before SR is set, and NR goes back before SR when it ends (TuneRunsAfterSr).
    if (DlssNrExposureCalibrate::TuneRunsAfterSr(DlssNrExposureCalibrate::TheRun(), GetTickCount64()))
        return nullptr;
    // Where NR runs without a Tune, for the run's wait (CalibrationVkSituation).
    g_vk.beforeSrPlacement = !rayReconstruction;
    bool applied = false;
    EvaluateAtSeamVk(cmd, params, instance, pd, device, true, rayReconstruction, applied, &handled);
    return applied ? &g_vk.preColor.ngx : nullptr;
}

void EvaluateAfterUpscaleVk(VkCommandBuffer cmd, NVSDK_NGX_Parameter* params, VkInstance instance,
                            VkPhysicalDevice pd, VkDevice device, bool rayReconstruction, bool ranBefore)
{
    if (ranBefore)
        return; // per-evaluate result, not a global frame counter that can suppress a different feature
    bool applied = false;
    EvaluateAtSeamVk(cmd, params, instance, pd, device, false, rayReconstruction, applied);
}

// Everything NR itself made on a live device: the model's features, its images, passes and rings. Not NGX on the device,
// the parameter block, the forwarder or the device handle, and nothing the present path owns (Optical F5Low's frame
// source, the Vulkan-on-D3D12 bridge): those are not in g_vk. The device must be idle. Shared by the full shutdown and by
// Retry, which is why Retry is not ShutdownVk: shutting NGX down under a game that is still using the device would be
// worse than the failure being retried.
void ReleaseNrObjects()
{
    CalibrationVkShutdown(true);
    DetailReuseVk::Release(true);
    EdgeCompressionVk::Release(true);
    DepthFadeVk::Release(true);

    if (g_vk.feature != nullptr && g_vk.release != nullptr)
        g_vk.release(g_vk.feature);

    g_vk.feature = nullptr;

    for (auto& feature : g_vk.laterFeatures)
    {
        if (feature && g_vk.release)
            g_vk.release(feature);
        feature = nullptr;
    }
    g_vk.activePasses = 0;
    if (g_vk.creationReady != VK_NULL_HANDLE)
        vkDestroyEvent(g_vk.device, g_vk.creationReady, nullptr);
    g_vk.creationReady = VK_NULL_HANDLE;
    g_vk.creationPending = false;

    DestroyImage(g_vk.output);
    DestroyImage(g_vk.scratch);
    DestroyImage(g_vk.proxy);
    DestroyImage(g_vk.proxySmall);
    DestroyImage(g_vk.outputNative);
    DestroyImage(g_vk.keep);
    DestroyImage(g_vk.preColor);
    DestroyImage(g_vk.meter);
    DestroyImage(g_vk.autoExposure);
    DestroyImage(g_vk.autoExposureRaw);
    DestroyImage(g_vk.lutScratch);
    g_vk.lutScratchFailed = false;
    DestroyImage(g_vk.styled);
    DestroyImage(g_vk.styledB);
    g_vk.autoExposureAdapter.Invalidate();
    DestroyMeterReadback();

    g_vk.pass.reset();
    g_vk.lutPass.reset(); // frees the 3D image, its staging buffer and the pass's own rings
    g_vk.postLutPass.reset();
    g_vk.superUp.reset();
    g_vk.superDown.reset();
    g_vk.sgsr1UpAnswer.reset();
    g_vk.nrScaler = Scaler::Count;
    g_vk.width = 0; // the next frame rebuilds its surfaces and features
    g_vk.height = 0;
    g_vk.workWidth = 0;
    g_vk.workHeight = 0;
    g_vk.reset = true;
}

void ShutdownVk(bool deviceAlive)
{
    if (!deviceAlive)
    {
        // The device these handles belong to is gone (a device change was detected). Destroying a
        // VkDevice already frees every resource created on it, so touch NOTHING on the old device --
        // no wait-idle, no vkDestroy*, no NGX release, and crucially no DlssNr_Vk destructor (it would
        // vkDestroy its pipelines on the dead device). Abandon the handles; the driver reclaimed them
        // when the device died. The one-off CPU-side leak of the pass object is the rare cost of a
        // device recreation, and far cheaper than the use-after-free it replaces. Zeroing the
        // OwnedImage/meter handles matters: the resize path gates on `.Valid()`, so a stale non-null
        // handle from the dead device would be reused on the NEW device and crash.
        g_vk.pass.release();
        g_vk.lutPass.release(); // same reason: its destructor would vkDestroy on the dead device
        g_vk.postLutPass.release();
        g_vk.superUp.release();
        g_vk.superDown.release();
        g_vk.sgsr1UpAnswer.release();
        g_vk.nrScaler = Scaler::Count;
        g_vk.feature = nullptr;
        for (auto& feature : g_vk.laterFeatures)
            feature = nullptr;
        g_vk.activePasses = 0;
        g_vk.creationReady = VK_NULL_HANDLE;
        g_vk.creationPending = false;
        g_vk.capabilityParams = nullptr;
        g_vk.queryPool = VK_NULL_HANDLE;
        g_vk.output = OwnedImage {};
        g_vk.scratch = OwnedImage {};
        g_vk.proxy = OwnedImage {};
        g_vk.proxySmall = OwnedImage {};
        g_vk.outputNative = OwnedImage {};
        g_vk.keep = OwnedImage {};
        g_vk.preColor = OwnedImage {};
        g_vk.meter = OwnedImage {};
        g_vk.autoExposure = OwnedImage {};
        g_vk.autoExposureRaw = OwnedImage {};
        g_vk.lutScratch = OwnedImage {};
        g_vk.lutScratchFailed = false;
        g_vk.styled = OwnedImage {};
        g_vk.styledB = OwnedImage {};
        g_vk.autoExposureAdapter.Invalidate();
        CalibrationVkShutdown(false);
        DetailReuseVk::Release(false);
        EdgeCompressionVk::Release(false);
        DepthFadeVk::Release(false);
        // The model's kernels went with the device without their destroy calls.
        DlssNrNative::VkDeviceLost();

        for (int i = 0; i < 4; ++i)
        {
            g_vk.meterReadback[i] = VK_NULL_HANDLE;
            g_vk.meterReadbackMemory[i] = VK_NULL_HANDLE;
            g_vk.meterMapped[i] = nullptr;
        }

        g_vk.device = VK_NULL_HANDLE;
        g_vk.width = 0;
        g_vk.height = 0;
        g_vk.workWidth = 0;
        g_vk.workHeight = 0;
        g_vk.timedFrames = 0;
        g_vk.meterFrames = 0;
        g_vk.gameLiveGate.Reset();
        g_vk.gameLiveSaid = false;
        g_vk.lastGpuTime.reset();
        g_vk.ngxInitialised = false;
        g_vk.floatSlotKnown = false; // the next block is a new one
        g_vk.paramsFromCore = false;
        ++g_vk.deviceLosses;
        // NGX was initialised on the dead device: the forwarder forgets it (no call on it) so the new one is
        // initialised, even if it comes back with the same handle value.
        if (g_vk.forget != nullptr)
            g_vk.forget();
        g_vk.reset = true;
        return;
    }

    // The device is alive (real teardown): drain before freeing so nothing the GPU is still using is
    // destroyed under it, the same rule as the resize path.
    if (g_vk.device != VK_NULL_HANDLE)
        vkDeviceWaitIdle(g_vk.device);

    ReleaseNrObjects();

    if (g_vk.capabilityParams != nullptr)
    {
        // A block from the game's core is the core's to free: once the game has shut its NGX down, calling into it
        // is a use after free, so the block is left behind instead.
        if (!g_vk.paramsFromCore || VkExt::NgxCoreUp())
            NVSDK_NGX_VULKAN_DestroyParameters(g_vk.capabilityParams);
        else
            LOG_INFO("DLSS-NR Vulkan: the game's NGX is already shut down; its parameter block is left to it");

        g_vk.capabilityParams = nullptr;
        g_vk.paramsFromCore = false;
    }

    if (g_vk.queryPool != VK_NULL_HANDLE && g_vk.device != VK_NULL_HANDLE)
    {
        vkDestroyQueryPool(g_vk.device, g_vk.queryPool, nullptr);
        g_vk.queryPool = VK_NULL_HANDLE;
    }

    g_vk.timedFrames = 0;
    g_vk.lastGpuTime.reset();

    // Features and the parameter block are gone: NGX lets go of this device (NVSDK_NGX_VULKAN_Shutdown1).
    if (g_vk.device != VK_NULL_HANDLE && g_vk.ngxInitialised && g_vk.shutdown != nullptr)
    {
        // 0: not called (the model has no Shutdown1, or NGX was not initialised on this device by the forwarder).
        const int result = g_vk.shutdown((void*) g_vk.device);
        if (result != 1 && result != 0)
            LOG_WARN("DLSS-NR Vulkan: NGX shutdown on this device returned {}", result);
    }

    // Whatever Shutdown1 did, the forwarder stops tracking this device; the next one is initialised.
    if (g_vk.forget != nullptr)
        g_vk.forget();

    g_vk.device = VK_NULL_HANDLE;
    g_vk.width = 0;
    g_vk.height = 0;
    g_vk.ngxInitialised = false;
    g_vk.floatSlotKnown = false;
    g_vk.reset = true;
}

void ShutdownVkForDevice(VkDevice device, const char* why)
{
    // At process exit other threads are gone (one may have held the lock) and the loader lock is held; the process
    // takes everything with it.
    if (State::Instance().isShuttingDown)
        return;

    if (t_vkLockHeld)
    {
        LOG_WARN("DLSS-NR Vulkan: {} from inside the NR pass; left for the pass to notice", why);
        return;
    }

    std::lock_guard<std::mutex> lock(g_vkMutex);

    if (g_vk.device == VK_NULL_HANDLE || (device != VK_NULL_HANDLE && g_vk.device != device))
        return;

    LOG_INFO("DLSS-NR Vulkan: {}; releasing the model and NGX first", why);
    const VkDevice destroyed = g_vk.device;
    ShutdownVk(true);
    // Reuse bottleneck's own copy buffers and kernel are ours to destroy while the device is idle; the model's kernels go with the device.
    DlssNrNative::VkDeviceDestroying(destroyed);
    DlssNrNative::VkDeviceLost();
}

} // namespace DlssNr
