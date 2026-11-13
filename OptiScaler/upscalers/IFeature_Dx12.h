#pragma once
#include <d3d12.h>
#include "IFeature.h"

#include "SysUtils.h"
#include <Util.h>
#include <menu/menu_dx12.h>
#include <shaders/output_scaling/OS_Dx12.h>
#include <shaders/rcas/RCAS_Dx12.h>
#include <shaders/bias/Bias_Dx12.h>
#include <shaders/magnifier/Magnifier_Dx12.h>
#include <shaders/f5lut/F5Lut_Dx12.h>
#include <gpu_time/GpuTime_Dx12.h>

class IFeature_Dx12 : public virtual IFeature
{
  private:
    struct ShaderPass
    {
        // Requests the target buffer it needs to write to. Returns the buffer the PREVIOUS stage must write to
        std::function<ID3D12Resource*(ID3D12Resource* nextOutput)> Setup;

        // Runs the shader
        std::function<bool(ID3D12Resource* input, ID3D12Resource* output)> Dispatch;

        // Internal state tracked by the pipeline setup loop
        ID3D12Resource* inputBuffer = nullptr;
        ID3D12Resource* outputBuffer = nullptr;
    };

  protected:
    ID3D12Device* Device = nullptr;
    // The in-upscaler menu (old overlay), shared by every feature and kept past them: menu/MenuLifetimeRule.h
    static inline std::unique_ptr<Menu_Dx12> Imgui = nullptr;
    // Imgui is drawn on the render thread and retired or destroyed on another (Util::DelayedDestroy). Recursive: a
    // feature destroyed while the menu is drawn.
    static inline std::recursive_mutex ImguiMutex;
    // A retired menu is waiting for its delay: no new one until it is gone (ImGui's DX12 backend is a global)
    static inline std::atomic<bool> ImguiRetiring = false;
    // Live D3D12 features: the menu goes with the last one
    static inline std::atomic<int> LiveFeatures = 0;

    // A menu taken out of use, destroyed by Util::DelayedDestroy once its lists are off the GPU
    struct RetiredImgui
    {
        explicit RetiredImgui(std::unique_ptr<Menu_Dx12> menu) : menu(std::move(menu)) {}
        ~RetiredImgui();
        std::unique_ptr<Menu_Dx12> menu;
    };

    // ImguiMutex held
    static void RetireImgui(const char* why);
    std::unique_ptr<OS_Dx12> OutputScaler = nullptr;
    std::unique_ptr<RCAS_Dx12> RCAS = nullptr;
    std::unique_ptr<Bias_Dx12> Bias = nullptr;
    std::unique_ptr<Magnifier_Dx12> Magnifier = nullptr;
    std::unique_ptr<F5Lut_Dx12> F5Lut = nullptr;

    std::unique_ptr<GpuTime_Dx12> UpscalerTime = nullptr;

    void ResourceBarrier(ID3D12GraphicsCommandList* InCommandList, ID3D12Resource* InResource,
                         D3D12_RESOURCE_STATES InBeforeState, D3D12_RESOURCE_STATES InAfterState) const;

    virtual bool InitInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) = 0;
    virtual bool EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) = 0;

  public:
    bool Init(ID3D12Device* InDevice, ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters);
    bool Evaluate(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters);

    API Api() const override { return API::DX12; }
    std::optional<double> ReadUpscalerTime(void* commandQueue) override;
    void ReadDetailedGpuTimes(void* commandQueue, std::vector<DetailedGpuTime>& detailedGpuTimes) override;

    IFeature_Dx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters);

    ~IFeature_Dx12();
};
