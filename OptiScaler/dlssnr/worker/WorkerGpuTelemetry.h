#pragma once

#include <windows.h>
#include <d3dkmthk.h>
#include <dxgi1_4.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>

namespace DlssNr::Worker
{
struct GpuReading
{
    std::string name;
    LUID luid {};
    int utilization = -1; // overall GPU utilization; -1 means the driver did not report it
    bool present = false;
};

// NVML is optional. Adapter LUIDs are mapped to PCI addresses through WDDM,
// rather than assuming that DXGI and NVML enumerate devices in the same order.
class GpuTelemetry
{
    using NvDevice = void*;
    struct NvUtilization { unsigned gpu, memory; };
    using NvInit = int (*)();
    using NvByPci = int (*)(const char*, NvDevice*);
    using NvUsage = int (*)(NvDevice, NvUtilization*);
    std::mutex mutex_;
    std::array<GpuReading, 2> devices_ {};
    std::chrono::steady_clock::time_point last_ {};
    HMODULE nvml_ = nullptr;
    NvByPci byPci_ = nullptr;
    NvUsage usage_ = nullptr;
    bool tried_ = false;

    static std::string Narrow(const wchar_t* name)
    {
        char result[256] {};
        return WideCharToMultiByte(CP_UTF8, 0, name, -1, result, sizeof(result), nullptr, nullptr) > 0
                   ? std::string(result) : std::string("Unknown GPU");
    }
    static bool Same(LUID a, LUID b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }
    void Initialize()
    {
        if (tried_) return;
        tried_ = true;
        nvml_ = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!nvml_) return;
        auto init = reinterpret_cast<NvInit>(GetProcAddress(nvml_, "nvmlInit_v2"));
        byPci_ = reinterpret_cast<NvByPci>(GetProcAddress(nvml_, "nvmlDeviceGetHandleByPciBusId_v2"));
        usage_ = reinterpret_cast<NvUsage>(GetProcAddress(nvml_, "nvmlDeviceGetUtilizationRates"));
        if (!init || !byPci_ || !usage_ || init() != 0) { byPci_ = nullptr; usage_ = nullptr; }
    }
    int Read(LUID luid)
    {
        if (!byPci_ || !usage_) return -1;
        HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
        if (!gdi) return -1;
        const auto open = reinterpret_cast<decltype(&D3DKMTOpenAdapterFromLuid)>(
            GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid"));
        const auto query = reinterpret_cast<decltype(&D3DKMTQueryAdapterInfo)>(
            GetProcAddress(gdi, "D3DKMTQueryAdapterInfo"));
        const auto close = reinterpret_cast<decltype(&D3DKMTCloseAdapter)>(
            GetProcAddress(gdi, "D3DKMTCloseAdapter"));
        if (!open || !query || !close) return -1;
        D3DKMT_OPENADAPTERFROMLUID adapter {};
        adapter.AdapterLuid = luid;
        if (open(&adapter) != 0) return -1;
        D3DKMT_ADAPTERADDRESS address {};
        D3DKMT_QUERYADAPTERINFO info {};
        info.hAdapter = adapter.hAdapter;
        info.Type = KMTQAITYPE_ADAPTERADDRESS;
        info.pPrivateDriverData = &address;
        info.PrivateDriverDataSize = sizeof(address);
        const auto result = query(&info);
        D3DKMT_CLOSEADAPTER closing {};
        closing.hAdapter = adapter.hAdapter;
        close(&closing);
        if (result != 0) return -1;
        char pci[32] {};
        std::snprintf(pci, sizeof(pci), "%08X:%02X:%02X.%X", 0u, address.BusNumber,
                      address.DeviceNumber, address.FunctionNumber);
        NvDevice device = nullptr;
        NvUtilization value {};
        return byPci_(pci, &device) == 0 && usage_(device, &value) == 0 && value.gpu <= 100
                   ? static_cast<int>(value.gpu) : -1;
    }

  public:
    void Set(unsigned index, const DXGI_ADAPTER_DESC1& desc)
    {
        if (index >= devices_.size()) return;
        std::lock_guard<std::mutex> lock(mutex_);
        auto& device = devices_[index];
        if (device.present && Same(device.luid, desc.AdapterLuid)) return;
        device = { Narrow(desc.Description), desc.AdapterLuid, -1, true };
        last_ = {};
    }
    std::array<GpuReading, 2> Readings()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        // Sampling happens only while the pass page is visible, at most once a second.
        if (now - last_ >= std::chrono::seconds(1))
        {
            last_ = now;
            if (devices_[0].present || devices_[1].present)
            {
                Initialize();
                for (auto& device : devices_)
                    if (device.present) device.utilization = Read(device.luid);
            }
        }
        return devices_;
    }
};

inline GpuTelemetry& Telemetry()
{
    // Like the logger, this process-lifetime object is intentionally retained:
    // calling into a driver from a DLL-unload destructor is unsafe.
    static auto* telemetry = new GpuTelemetry;
    return *telemetry;
}
} // namespace DlssNr::Worker
