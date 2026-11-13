#pragma once
#include "F5Lut_Common.h"

#include <shaders/Shader_Dx11.h>

// The F5LUT step of IFeature_Dx11's output chain (F5Lut_Common.h): NR's LUT shader (dlssnr_lut.hlsl) built as cs_5_0.
// D3D11 games on a D3D12 upscaler (IFeature_Dx11wDx12) get the D3D12 step inside it instead.
class F5Lut_Dx11 : public Shader_Dx11, public F5Lut_Common
{
  private:
    ID3D11Texture3D* _lutTexture = nullptr;
    ID3D11ShaderResourceView* _srvLut = nullptr;
    std::string _lutKey;
    std::string _failedKey; // the bake whose texture could not be made, not retried every frame

    ID3D11SamplerState* _sampler = nullptr;
    ID3D11ShaderResourceView* _srvExposure = nullptr;
    ID3D11Texture2D* _currentExposure = nullptr;

    bool EnsureLutTexture();
    void ReleaseLutTexture();

  public:
    // Called while the chain is built: whether the step runs this frame (F5Lut_Common::PrepareChain).
    bool ShouldRun(bool InLinearHdr);

    bool CreateBufferResource(ID3D11Device* InDevice, ID3D11Resource* InResource);

    // Grades InResource into OutResource. InExposure is the game's 1x1 exposure texture or null. When the grade cannot
    // run the picture is copied through ungraded, so the target always gets this frame.
    bool Dispatch(ID3D11DeviceContext* InContext, ID3D11Texture2D* InResource, ID3D11Texture2D* OutResource,
                  bool InLinearHdr, ID3D11Texture2D* InExposure, float InPreExposure);

    F5Lut_Dx11(std::string InName, ID3D11Device* InDevice);

    ~F5Lut_Dx11();
};
