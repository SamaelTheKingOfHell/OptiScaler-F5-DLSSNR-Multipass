#pragma once

#pragma warning(push)
#pragma warning(disable : 4324) // The production constant buffer is intentionally 256-byte aligned.
#include "../../shaders/dlssnr/DlssNr_Common.h"
#pragma warning(pop)
#include "../../shaders/dlssnr/precompile/DlssNr_Shader.h"

namespace DlssNr::Worker
{
// Dispatch the same precompiled inter-pass shader and constants as the application.
// The caller must retire a dispatch before changing these descriptors or destroying this object.
class InterpassClamp
{
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> pipeline_;
    ComPtr<ID3D12DescriptorHeap> heap_;
    ComPtr<ID3D12Resource> constants_;
    UINT stride_ = 0, width_ = 0, height_ = 0;

  public:
    explicit InterpassClamp(ID3D12Device* device, UINT width = 256, UINT height = 144, float feedback = 1.0f)
        : device_(device), width_(width), height_(height)
    {
        D3D12_DESCRIPTOR_RANGE ranges[2] {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 6;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 2;
        ranges[1].OffsetInDescriptorsFromTableStart = 6;
        D3D12_ROOT_PARAMETER params[2] {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable = { 2, ranges };
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        D3D12_STATIC_SAMPLER_DESC sampler {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        D3D12_ROOT_SIGNATURE_DESC rootDesc { 2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ComPtr<ID3DBlob> signature, errors;
        Check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errors),
              "Serialize clamp root");
        Check(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                          IID_PPV_ARGS(&root_)),
              "Create clamp root");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc {};
        pipelineDesc.pRootSignature = root_.Get();
        pipelineDesc.CS = { DlssNr_cso, sizeof(DlssNr_cso) };
        Check(device->CreateComputePipelineState(&pipelineDesc, IID_PPV_ARGS(&pipeline_)), "Create clamp pipeline");
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc {};
        heapDesc.NumDescriptors = 8;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap_)), "Create clamp descriptors");
        stride_ = device->GetDescriptorHandleIncrementSize(heapDesc.Type);
        D3D12_HEAP_PROPERTIES uploadHeap {};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        uploadHeap.CreationNodeMask = uploadHeap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC bufferDesc {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = sizeof(DlssNrConstants);
        bufferDesc.Height = bufferDesc.DepthOrArraySize = bufferDesc.MipLevels = 1;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Check(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&constants_)),
              "Create clamp constants");
        DlssNrConstants constants {};
        constants.Mode = DlssNrMode_ClampProxy;
        constants.Width = width_;
        constants.Height = height_;
        constants.PassFeedback = feedback;
        D3D12_RANGE noRead { 0, 0 };
        void* mapped = nullptr;
        Check(constants_->Map(0, &noRead, &mapped), "Map clamp constants");
        std::memcpy(mapped, &constants, sizeof(constants));
        constants_->Unmap(0, nullptr);
    }
    void Record(ID3D12GraphicsCommandList* commands, ID3D12Resource* raw, ID3D12Resource* proxy, ID3D12Resource* output)
    {
        auto cpu = heap_->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < 6; ++i)
        {
            ID3D12Resource* input = i == 1 ? proxy : raw;
            D3D12_SHADER_RESOURCE_VIEW_DESC view {};
            view.Format = input->GetDesc().Format;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MipLevels = 1;
            device_->CreateShaderResourceView(input, &view, { cpu.ptr + SIZE_T(i) * stride_ });
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
        uav.Format = output->GetDesc().Format;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        for (UINT i = 6; i < 8; ++i)
            device_->CreateUnorderedAccessView(output, nullptr, &uav, { cpu.ptr + SIZE_T(i) * stride_ });
        ID3D12DescriptorHeap* heaps[] = { heap_.Get() };
        commands->SetDescriptorHeaps(1, heaps);
        commands->SetComputeRootSignature(root_.Get());
        commands->SetPipelineState(pipeline_.Get());
        commands->SetComputeRootDescriptorTable(0, heap_->GetGPUDescriptorHandleForHeapStart());
        commands->SetComputeRootConstantBufferView(1, constants_->GetGPUVirtualAddress());
        commands->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);
    }
};

} // namespace DlssNr::Worker
