#pragma once

#include "../../external/nvngx_dlss_sdk/nvsdk_ngx.h"
#include "nr_ngx_evaluate_fixture.h"
#include "nr_cross_adapter_test_helpers.h"

namespace NrFixture
{
using Init = NVSDK_NGX_Result (*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version,
                                  const NVSDK_NGX_FeatureCommonInfo*);
using GetCapabilities = NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter**);
using DestroyParameters = NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter*);
using Shutdown = NVSDK_NGX_Result (*)(ID3D12Device*);
using CreateFeature = void* (*) (const wchar_t*, const wchar_t*, ID3D12Device*, ID3D12GraphicsCommandList*, void*,
                                 unsigned, unsigned, int, float, int, float, float, float, int, int);
using ReleaseFeature = void (*)(void*);
using ProbeFloat = void (*)(void*, const char*, float, int);
using SetFloatSlot = void (*)(int);

// Fixture-only one-device NGX session. The caller must retire every submitted command before destruction.
class ModelSession
{
    HMODULE core_ = nullptr, forwarder_ = nullptr;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    NVSDK_NGX_Parameter* params_ = nullptr;
    void* feature_ = nullptr;
    DestroyParameters destroy_ = nullptr;
    Shutdown shutdown_ = nullptr;
    ReleaseFeature release_ = nullptr;
    Evaluate evaluate_ = nullptr;
    Error error_ = nullptr;
    bool started_ = false;

  public:
    ModelSession(const ModelSession&) = delete;
    ModelSession& operator=(const ModelSession&) = delete;
    ModelSession() = default;
    ~ModelSession()
    {
        std::cerr << "Session releasing model feature\n" << std::flush;
        if (feature_ && release_)
            release_(feature_);
        std::cerr << "Session unloading forwarder\n" << std::flush;
        if (forwarder_)
            FreeLibrary(forwarder_);
        std::cerr << "Session destroying parameters\n" << std::flush;
        if (params_ && destroy_)
            destroy_(params_);
        std::cerr << "Session shutting down core\n" << std::flush;
        if (started_ && shutdown_)
            shutdown_(device_.Get());
        if (core_)
            FreeLibrary(core_);
    }
    void Initialize(ID3D12Device* device, const wchar_t* corePath, const wchar_t* forwarderPath,
                    const wchar_t* modelPath, const wchar_t* dataPath)
    {
        device_ = device;
        core_ = LoadLibraryExW(corePath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!core_)
            throw std::runtime_error("Load NGX core failed");
        auto init = reinterpret_cast<Init>(GetProcAddress(core_, "NVSDK_NGX_D3D12_Init_Ext"));
        auto get = reinterpret_cast<GetCapabilities>(GetProcAddress(core_, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
        destroy_ = reinterpret_cast<DestroyParameters>(GetProcAddress(core_, "NVSDK_NGX_D3D12_DestroyParameters"));
        shutdown_ = reinterpret_cast<Shutdown>(GetProcAddress(core_, "NVSDK_NGX_D3D12_Shutdown1"));
        if (!init || !get || !destroy_ || !shutdown_)
            throw std::runtime_error("Missing NGX core exports");
        if (init(0x24480451ull, dataPath, device, NVSDK_NGX_Version_API, nullptr) != NVSDK_NGX_Result_Success)
            throw std::runtime_error("NGX initialization failed");
        started_ = true;
        if (get(&params_) != NVSDK_NGX_Result_Success || !params_)
            throw std::runtime_error("NGX parameters failed");
        forwarder_ = LoadLibraryExW(forwarderPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!forwarder_)
            throw std::runtime_error("Load model forwarder failed");
        auto create = reinterpret_cast<CreateFeature>(GetProcAddress(forwarder_, "dlssnr_call_create"));
        release_ = reinterpret_cast<ReleaseFeature>(GetProcAddress(forwarder_, "dlssnr_call_release"));
        evaluate_ = reinterpret_cast<Evaluate>(GetProcAddress(forwarder_, "dlssnr_call_evaluate_v2"));
        error_ = reinterpret_cast<Error>(GetProcAddress(forwarder_, "dlssnr_call_error"));
        auto probe = reinterpret_cast<ProbeFloat>(GetProcAddress(forwarder_, "dlssnr_call_probe_float"));
        auto setSlot = reinterpret_cast<SetFloatSlot>(GetProcAddress(forwarder_, "dlssnr_call_set_float_slot"));
        if (!create || !release_ || !evaluate_ || !probe || !setSlot)
            throw std::runtime_error("Missing forwarder exports");
        bool found = false;
        for (int slot : { 1, 2, 5, 6, 7, 4, 3, 0 })
        {
            float readback = 0.0f;
            probe(params_, "DLSSNR.OptiScalerFloatProbe", 0.375f, slot);
            if (params_->Get("DLSSNR.OptiScalerFloatProbe", &readback) == NVSDK_NGX_Result_Success &&
                readback == 0.375f)
            {
                setSlot(slot);
                found = true;
                break;
            }
        }
        if (!found)
            throw std::runtime_error("Float parameter slot not found");
        D3D12_COMMAND_QUEUE_DESC queueDesc {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue_)), "Create model queue");
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        ComPtr<ID3D12Fence> fence;
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
              "Create model allocator");
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                        IID_PPV_ARGS(&commands)),
              "Create model list");
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Create model fence");
        feature_ =
            create(modelPath, dataPath, device, commands.Get(), params_, 256, 144, 0, 1.0f, 0, 0.0f, 0.0f, 0.0f, 1, 1);
        if (!feature_)
            throw std::runtime_error(error_ && error_() ? error_() : "Create model feature failed");
        Check(commands->Close(), "Close model initialization");
        ID3D12CommandList* lists[] = { commands.Get() };
        queue_->ExecuteCommandLists(1, lists);
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event || FAILED(queue_->Signal(fence.Get(), 1)) || FAILED(fence->SetEventOnCompletion(1, event)))
        {
            if (event)
                CloseHandle(event);
            std::cerr << "Submitted model initialization cannot be tracked.\n" << std::flush;
            ExitProcess(3);
        }
        const DWORD waited = WaitForSingleObject(event, 30000);
        CloseHandle(event);
        if (waited != WAIT_OBJECT_0 || FAILED(device->GetDeviceRemovedReason()))
        {
            std::cerr << "Model initialization did not retire.\n" << std::flush;
            ExitProcess(3);
        }
    }
    ID3D12CommandQueue* Queue() const { return queue_.Get(); }
    int Record(ID3D12GraphicsCommandList* commands, ID3D12Resource* color, ID3D12Resource* depth,
               ID3D12Resource* motion, ID3D12Resource* output, bool reset)
    {
        return evaluate_(commands, feature_, params_, color, depth, motion, output, 256, 144, 256, 144, 256, 144, 0, 0,
                         0, 0, 0, reset ? 1 : 0, 1.0f, 0, 0.0f, 0.0f, 0.0f, 1, 1.0f, 1.0f);
    }
    const char* LastError() const { return error_ ? error_() : "no detail"; }
};
} // namespace NrFixture
