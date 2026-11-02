#include "../../OptiScaler/dlssnr/worker/WorkerGpuTelemetry.h"
#include <wrl/client.h>
#include <cassert>
#include <cstdio>
#include <cwchar>

int main()
{
    using Microsoft::WRL::ComPtr;
    DlssNr::Worker::GpuTelemetry telemetry;
    DXGI_ADAPTER_DESC1 missing {};
    wcscpy_s(missing.Description, L"Missing test adapter");
    missing.AdapterLuid.LowPart = 0xFFFFFFFFu;
    missing.AdapterLuid.HighPart = 0x7FFFFFFF;
    telemetry.Set(0, missing);
    auto initial = telemetry.Readings();
    assert(initial[0].present && initial[0].name == "Missing test adapter" && initial[0].utilization == -1);
    assert(!initial[1].present);

    ComPtr<IDXGIFactory4> factory;
    assert(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
    unsigned count = 0;
    for (UINT i = 0; count < 2; ++i)
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc {};
        assert(SUCCEEDED(adapter->GetDesc1(&desc)));
        if (desc.VendorId != 0x10de || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        telemetry.Set(count++, desc);
    }
    const auto readings = telemetry.Readings();
    for (unsigned i = 0; i < count; ++i)
    {
        assert(readings[i].present && !readings[i].name.empty());
        assert(readings[i].utilization >= -1 && readings[i].utilization <= 100);
        std::printf("%s: %d%% (-1 means unavailable)\n", readings[i].name.c_str(), readings[i].utilization);
    }
    std::puts("GPU adapter identity, unavailable fallback and utilization bounds passed.");
}
