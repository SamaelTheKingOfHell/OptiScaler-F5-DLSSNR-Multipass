#pragma once
#include "F5Lut_Common.h"

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <shaders/Shader_Dx12Utils.h>
#include <shaders/Shader_Dx12.h>

// A heap and constant buffer per frame the GPU may still be reading (3 queued frames).
#define F5LUT_NUM_OF_HEAPS 3

// The F5LUT step of IFeature_Dx12's output chain (F5Lut_Common.h). The upscaler (or the step before) writes Buffer(),
// Dispatch grades it into the next target with NR's own LUT shader (dlssnr_lut.hlsl).
class F5Lut_Dx12 : public Shader_Dx12, public F5Lut_Common
{
  private:
    FrameDescriptorHeap _frameHeaps[F5LUT_NUM_OF_HEAPS];
    ID3D12Resource* _constantBuffers[F5LUT_NUM_OF_HEAPS] {};

    ID3D12Resource* _buffer = nullptr;
    D3D12_RESOURCE_STATES _bufferState = D3D12_RESOURCE_STATE_COMMON;

    // The baked lattice as a 3D texture, and the chain key it was uploaded from.
    ID3D12Resource* _lutTexture = nullptr;
    std::string _lutKey;
    std::string _failedKey; // the bake whose upload failed, not retried every frame

    bool EnsureLutTexture(ID3D12GraphicsCommandList* InCmdList);
    void ReleaseLutTexture();
    void CopyThrough(ID3D12GraphicsCommandList* InCmdList, ID3D12Resource* InResource, ID3D12Resource* OutResource);

  public:
    // Called while the chain is built: whether the step runs this frame (F5Lut_Common::PrepareChain).
    bool ShouldRun(bool InLinearHdr);

    bool CreateBufferResource(ID3D12Device* InDevice, ID3D12Resource* InSource, D3D12_RESOURCE_STATES InState);
    void SetBufferState(ID3D12GraphicsCommandList* InCommandList, D3D12_RESOURCE_STATES InState);

    // Grades InResource into OutResource (the next target, in UAV state as the upscaler's output is). InExposure is the
    // game's 1x1 exposure texture or null. When the grade cannot run the picture is copied through ungraded, so the
    // target always gets this frame.
    bool Dispatch(ID3D12GraphicsCommandList* InCmdList, ID3D12Resource* InResource, ID3D12Resource* OutResource,
                  bool InLinearHdr, ID3D12Resource* InExposure, float InPreExposure);

    ID3D12Resource* Buffer() { return _buffer; }

    F5Lut_Dx12(std::string InName, ID3D12Device* InDevice);

    ~F5Lut_Dx12();
};
