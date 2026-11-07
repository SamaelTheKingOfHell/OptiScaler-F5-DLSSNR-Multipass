#pragma once
#include <cstdint>
#include <string>
#include <vector>
struct ID3D12GraphicsCommandList;
namespace DlssNr::NetworkSplit { struct Plan; }
namespace DlssNr::NetworkSplit::Calibration { struct Kernel; }
namespace DlssNrNative {
void* WrapNvapi(unsigned id,void* original);
void SetEnabled(bool enabled);
void SetPrecision(unsigned precision);
bool IsActive();
std::string Status();
// ViT reuse (see DlssNrVitReuse.h): bracket one model evaluation of `feature`. `every` = how often the ViT bottleneck is computed (1 = always) with the fp8 kernel set, `everyPlain` with the plain fp16 one; `slot` = frame number, the same for every pass of a frame, so the passes compute on the same frame.
// `copySets` (DlssNrVitReuse::kCopyPlain, kCopyFp8): the kernel sets that reuse through our own copy of the ViT output (copy mode; recorded by the D3D12 launch path here and by DlssNrNative_Vk.cpp on Vulkan).
void BeginEvaluate(const void* feature, bool reset, unsigned every, unsigned everyPlain, long long slot, ID3D12GraphicsCommandList* cmd, bool profile, unsigned copySets);
// True when this evaluation turned Reuse off for the session (unexpected launch order, or a copy that could not be recorded); VitStatus says why. Log it once.
bool EndEvaluate(ID3D12GraphicsCommandList* cmd);
// Network split (DlssNr_NetworkSplit.h): the evaluate recorded next on `cmd` launches only the kernels of `role`
// (a NetworkSplit::Role) and records its transfers between `workspace` and the staging buffers. `plan` must outlive it.
// EndSplitEvaluate is true when every transfer of the role was recorded and every launch succeeded.
void BeginSplitEvaluate(ID3D12GraphicsCommandList* cmd, int role, const DlssNr::NetworkSplit::Plan* plan, uint64_t workspace, uint64_t forwardStaging, uint64_t backwardStaging);
bool EndSplitEvaluate();
// Records the launches of the evaluate recorded next on `cmd` (function name and parameter block per kernel, as the
// model issued them), for the split calibration's layout check. EndRecord returns them and stops recording.
void BeginRecord(ID3D12GraphicsCommandList* cmd);
std::vector<DlssNr::NetworkSplit::Calibration::Kernel> EndRecord();
// Kernel census / per-group GPU timing lines finished since the last call (ini [DlssNr] KernelProfile), for the log.
std::vector<std::string> TakeProfileReports();
std::string VitStatus();
// The kernel set of the last ViT run seen: "FP8", "plain FP16" or "not seen yet".
const char* VitKernelSet();
// True when the last ViT run seen used the plain fp16 kernels.
bool VitPlainKernels();
// Vulkan (DlssNrNative_Vk.cpp): the model's VK_NVX_binary_import calls, from the Vulkan hook table
// (VulkanwDx12_Hooks.cpp), so Reuse bottleneck and the kernel set work there too. `function` is a VkCuFunctionNVX,
// `commandBuffer` a VkCommandBuffer.
void VkFunctionCreated(uint64_t function, const char* name);
void VkFunctionDestroyed(uint64_t function);
// A CUDA module was destroyed (any caller's, as on D3D12): no kept bottleneck result can be trusted any more. The kernel
// handles stay: each kernel's own destroy removes it.
void VkModuleDestroyed();
// The device was lost and its kernels abandoned without their destroy calls: forget the kept results and the handles too
// (a new device can hand them out again).
void VkDeviceLost();
// The Vulkan device of the next evaluations (DlssNrFeature_Vk.cpp, before BeginEvaluate): Reuse bottleneck's copies go there.
void VkEvaluateDevice(void* device, void* physicalDevice);
// The device is about to be destroyed and is idle: destroy Reuse bottleneck's own objects on it (VkDeviceLost then forgets the rest).
void VkDeviceDestroying(void* device);
// One launch (`launchInfo` = its VkCuLaunchInfoNVX): records the barrier where a skipped ViT run was, and a restore copy in place of its
// last kernel, and says whether the launch goes out. Inside our own evaluate bracket only; any other launch (the game's own DLSS SR) goes
// out untouched. VkLaunched follows the launch when it went out (a save copy goes right after the run's last kernel).
bool VkLaunch(void* commandBuffer, const void* launchInfo);
void VkLaunched(void* commandBuffer);
}
