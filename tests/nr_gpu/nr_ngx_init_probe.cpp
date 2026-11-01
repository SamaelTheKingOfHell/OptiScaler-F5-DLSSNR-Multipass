#include "pch.h"
#include "../../OptiScaler/dlssnr/DlssNr_CrossAdapterSelect.h"
#include "../../external/nvngx_dlss_sdk/nvsdk_ngx.h"

using Microsoft::WRL::ComPtr;
using Init = NVSDK_NGX_Result (*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version,
                                  const NVSDK_NGX_FeatureCommonInfo*);
using GetCapabilities = NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter**);
using DestroyParameters = NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter*);
using Shutdown = NVSDK_NGX_Result (*)(ID3D12Device*);

// A separate process and app-data directory keep this diagnostic out of the game's NGX session.
// It tests only core initialization and parameter allocation, not neural feature creation.
int wmain(int argc, wchar_t** argv)
{
    if (argc != 3)
    {
        std::wcerr << L"Usage: nr_ngx_init_probe <absolute _nvngx.dll> <isolated app-data directory>\n";
        return 2;
    }
    MEMORYSTATUSEX memory { sizeof(memory) };
    if (!GlobalMemoryStatusEx(&memory))
    {
        std::cerr << "Could not check system commit headroom.\n";
        return 2;
    }
    constexpr ULONGLONG minimumHeadroom = 8ull * 1024 * 1024 * 1024;
    if (memory.ullAvailPageFile < minimumHeadroom)
    {
        std::cerr << "NGX probe not run: fewer than 8 GiB of system commit headroom.\n";
        return 2;
    }
    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        std::cerr << "DXGI factory unavailable.\n";
        return 1;
    }
    const POINT origin { 0, 0 };
    const HMONITOR primaryMonitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    LUID primaryLuid {};
    bool foundPrimary = false;
    for (UINT i = 0;; ++i)
    {
        ComPtr<IDXGIAdapter1> adapter;
        hr = factory->EnumAdapters1(i, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND)
            break;
        if (FAILED(hr))
            return 1;
        DXGI_ADAPTER_DESC1 desc {};
        if (FAILED(adapter->GetDesc1(&desc)))
            return 1;
        for (UINT j = 0;; ++j)
        {
            ComPtr<IDXGIOutput> output;
            hr = adapter->EnumOutputs(j, &output);
            if (hr == DXGI_ERROR_NOT_FOUND)
                break;
            if (FAILED(hr))
                return 1;
            DXGI_OUTPUT_DESC outputDesc {};
            if (FAILED(output->GetDesc(&outputDesc)))
                return 1;
            if (outputDesc.AttachedToDesktop && outputDesc.Monitor == primaryMonitor)
            {
                primaryLuid = desc.AdapterLuid;
                foundPrimary = true;
            }
        }
    }
    if (!foundPrimary)
    {
        std::cerr << "Primary display adapter not found.\n";
        return 2;
    }
    DlssNr::MultiGpu::SelectedAdapter secondary;
    hr = DlssNr::MultiGpu::SelectSecondary(factory.Get(), primaryLuid, nullptr, 0x10de, &D3D12CreateDevice, secondary);
    if (FAILED(hr))
    {
        std::cerr << "Secondary D3D12 device unavailable.\n";
        return 2;
    }
    std::wcout << L"Secondary adapter: " << secondary.description.Description << L"\n";
    HMODULE core = LoadLibraryExW(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!core)
    {
        std::cerr << "Could not load the specified NGX core.\n";
        return 1;
    }
    auto init = reinterpret_cast<Init>(GetProcAddress(core, "NVSDK_NGX_D3D12_Init_Ext"));
    auto get = reinterpret_cast<GetCapabilities>(GetProcAddress(core, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
    auto destroy = reinterpret_cast<DestroyParameters>(GetProcAddress(core, "NVSDK_NGX_D3D12_DestroyParameters"));
    auto shutdown = reinterpret_cast<Shutdown>(GetProcAddress(core, "NVSDK_NGX_D3D12_Shutdown1"));
    if (!init || !get || !destroy || !shutdown)
    {
        std::cerr << "NGX core is missing required exports.\n";
        FreeLibrary(core);
        return 1;
    }
    const auto initResult = init(0x24480451ull, argv[2], secondary.device.Get(), NVSDK_NGX_Version_API, nullptr);
    std::cout << "NGX init result: 0x" << std::hex << static_cast<unsigned>(initResult) << "\n";
    if (initResult != NVSDK_NGX_Result_Success)
    {
        FreeLibrary(core);
        return 1;
    }
    NVSDK_NGX_Parameter* parameters = nullptr;
    const auto capResult = get(&parameters);
    std::cout << "NGX capability result: 0x" << std::hex << static_cast<unsigned>(capResult)
              << ", block present: " << (parameters != nullptr) << "\n";
    bool ok = capResult == NVSDK_NGX_Result_Success && parameters;
    if (parameters)
        ok = destroy(parameters) == NVSDK_NGX_Result_Success && ok;
    ok = shutdown(secondary.device.Get()) == NVSDK_NGX_Result_Success && ok;
    FreeLibrary(core);
    return ok ? 0 : 1;
}
