#include "pch.h"

#include "F5Lut_Dx11.h"

#include <shaders/dlssnr/precompile/dlssnr_lut_Shader_Dx11.h>
#include <dlssnr/DlssNr_LutPack.h>

#include <vector>

bool F5Lut_Dx11::ShouldRun(bool InLinearHdr)
{
    if (!_init)
        return false;

    const bool run = PrepareChain(InLinearHdr);
    if (!_chain.wanted)
    {
        if (_lutTexture != nullptr)
            ReleaseLutTexture();
        _failedKey.clear(); // the same stack added again is tried again
    }
    return run;
}

bool F5Lut_Dx11::CreateBufferResource(ID3D11Device* InDevice, ID3D11Resource* InResource)
{
    return CreateBufferResourceCommon(InDevice, InResource, _buffer, [](D3D11_TEXTURE2D_DESC& desc)
                                      { desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE; });
}

void F5Lut_Dx11::ReleaseLutTexture()
{
    // D3D11 keeps a resource alive while queued work still uses it.
    SAFE_RELEASE(_srvLut);
    SAFE_RELEASE(_lutTexture);
    _lutKey.clear();
}

bool F5Lut_Dx11::EnsureLutTexture()
{
    const DlssNr::Lut3D& lut = _chain.baked;
    if (lut.size < 2 || _device == nullptr)
        return false;

    // Keyed on the bake, not the size: two stacks of the same lattice size are different textures.
    if (_srvLut != nullptr && _lutKey == _chain.key)
        return true;

    // A bake whose upload failed is not tried again every frame (an allocation and a 4 MB pack each time).
    if (_failedKey == _chain.key)
        return false;

    ReleaseLutTexture();
    _failedKey = _chain.key; // cleared below once the texture is made

    const int size = lut.size;
    const size_t rowPitch = (size_t) size * 4u * sizeof(float);
    std::vector<uint8_t> texels(DlssNrLutPack::TightBytesFloat4(size));
    DlssNrLutPack::PackFloat4(lut.rgb.data(), size, texels.data(), rowPitch, rowPitch * (size_t) size);

    D3D11_TEXTURE3D_DESC desc {};
    desc.Width = (UINT) size;
    desc.Height = (UINT) size;
    desc.Depth = (UINT) size;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA data {};
    data.pSysMem = texels.data();
    data.SysMemPitch = (UINT) rowPitch;
    data.SysMemSlicePitch = (UINT) (rowPitch * (size_t) size);

    if (FAILED(_device->CreateTexture3D(&desc, &data, &_lutTexture)) ||
        FAILED(_device->CreateShaderResourceView(_lutTexture, nullptr, &_srvLut)))
    {
        LOG_ERROR("[{0}] the LUT texture could not be made ({1}x{1}x{1})", _name, size);
        ReleaseLutTexture();
        return false;
    }

    _lutKey = _chain.key;
    _failedKey.clear();
    return true;
}

bool F5Lut_Dx11::Dispatch(ID3D11DeviceContext* InContext, ID3D11Texture2D* InResource, ID3D11Texture2D* OutResource,
                          bool InLinearHdr, ID3D11Texture2D* InExposure, float InPreExposure)
{
    if (!_init || _device == nullptr || InContext == nullptr || InResource == nullptr || OutResource == nullptr)
        return false;

    ScopedGpuTime_Dx11 scopedGpuTime(GpuTime.get(), InContext);

    const char* failure = nullptr;
    if (!EnsureLutTexture())
        failure = "could not upload the LUT to the GPU";
    else if (!InitializeSRV(InResource, _currentInResource, _srvInput) ||
             !InitializeUAV(OutResource, _currentOutResource, _uavOutput))
        failure = "could not make the pass's views";

    // An exposure texture that cannot be read leaves the white point at 1.0.
    const bool exposure = InExposure != nullptr && InitializeSRV(InExposure, _currentExposure, _srvExposure);

    D3D11_TEXTURE2D_DESC outDesc {};
    OutResource->GetDesc(&outDesc);
    const DlssNrLutConstants constants =
        ChainConstants(outDesc.Width, outDesc.Height, InLinearHdr, exposure, InPreExposure);

    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (failure == nullptr)
    {
        if (FAILED(InContext->Map(_constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            failure = "could not write the pass's constants";
        else
        {
            memcpy(mapped.pData, &constants, sizeof(constants));
            InContext->Unmap(_constantBuffer, 0);
        }
    }

    if (failure != nullptr)
    {
        InContext->CopyResource(OutResource, InResource);
        Report(false, failure);
        return true;
    }

    // The source stands in when there is no exposure; the constants then say not to read it.
    ID3D11ShaderResourceView* srvs[3] = { _srvInput, _srvLut, exposure ? _srvExposure : _srvInput };
    InContext->CSSetShader(_computeShader, nullptr, 0);
    InContext->CSSetConstantBuffers(0, 1, &_constantBuffer);
    InContext->CSSetShaderResources(0, 3, srvs);
    InContext->CSSetSamplers(0, 1, &_sampler);
    InContext->CSSetUnorderedAccessViews(0, 1, &_uavOutput, nullptr);

    InContext->Dispatch((constants.Width + 7) / 8, (constants.Height + 7) / 8, 1);

    ID3D11UnorderedAccessView* nullUAV = nullptr;
    InContext->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
    ID3D11ShaderResourceView* nullSRV[3] = { nullptr, nullptr, nullptr };
    InContext->CSSetShaderResources(0, 3, nullSRV);

    Report(true);
    return true;
}

F5Lut_Dx11::F5Lut_Dx11(std::string InName, ID3D11Device* InDevice) : Shader_Dx11(InName, InDevice)
{
    if (InDevice == nullptr)
    {
        LOG_ERROR("InDevice is nullptr!");
        return;
    }

    // Precompiled only: the shader includes NR's .hlsli files, which a runtime compile from a string cannot reach.
    HRESULT result = CreateComputeShader(InDevice, _computeShader, reinterpret_cast<const void*>(dlssnr_lut_dx11_cso),
                                         sizeof(dlssnr_lut_dx11_cso), nullptr);
    if (FAILED(result))
    {
        LOG_ERROR("[{0}] CreateComputeShader error: {1:X}", _name, (UINT) result);
        return;
    }

    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.ByteWidth = sizeof(DlssNrLutConstants);
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    result = InDevice->CreateBuffer(&cbDesc, nullptr, &_constantBuffer);
    if (FAILED(result))
    {
        LOG_ERROR("[{0}] CreateBuffer error: {1:X}", _name, (UINT) result);
        return;
    }

    // dlssnr_lut.hlsl's s0: linear, clamped.
    D3D11_SAMPLER_DESC samplerDesc {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

    result = InDevice->CreateSamplerState(&samplerDesc, &_sampler);
    if (FAILED(result))
    {
        LOG_ERROR("[{0}] CreateSamplerState error: {1:X}", _name, (UINT) result);
        return;
    }

    _init = true;
}

F5Lut_Dx11::~F5Lut_Dx11()
{
    if (!_init || State::Instance().isShuttingDown)
        return;

    ReleaseLutTexture();
    SAFE_RELEASE(_sampler);
    SAFE_RELEASE(_srvExposure);
}
