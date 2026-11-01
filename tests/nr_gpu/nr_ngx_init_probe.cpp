#include "pch.h"
#include "../../OptiScaler/dlssnr/DlssNr_CrossAdapterSelect.h"
#include "../../external/nvngx_dlss_sdk/nvsdk_ngx.h"
#include "nr_ngx_evaluate_fixture.h"
#include <cwchar>

using Microsoft::WRL::ComPtr;
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
using ModelError = const char* (*) ();

struct CreatedFeature
{
    HMODULE module = nullptr;
    ReleaseFeature release = nullptr;
    void* feature = nullptr;
    ComPtr<ID3D12CommandQueue> queue;
    NrFixture::Evaluate evaluate = nullptr;
    ModelError error = nullptr;
    void Reset()
    {
        if (feature && release)
        {
            std::cerr << "NGX probe: releasing model feature\n" << std::flush;
            release(feature);
        }
        feature = nullptr;
        if (module)
            FreeLibrary(module);
        module = nullptr;
    }
};

// Creates one feature on the selected device and waits for its initialization commands to retire.
// It also evaluates a synthetic frame after initialization retires. Paths and application data belong to the caller.
static bool ProbeFeature(ID3D12Device* device, NVSDK_NGX_Parameter* parameters, const wchar_t* dataPath,
                         const wchar_t* forwarderPath, const wchar_t* modelPath, CreatedFeature& out)
{
    HMODULE forwarder = LoadLibraryExW(forwarderPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!forwarder)
    {
        std::cerr << "Could not load the specified forwarder.\n";
        return false;
    }
    auto create = reinterpret_cast<CreateFeature>(GetProcAddress(forwarder, "dlssnr_call_create"));
    auto release = reinterpret_cast<ReleaseFeature>(GetProcAddress(forwarder, "dlssnr_call_release"));
    auto evaluate = reinterpret_cast<NrFixture::Evaluate>(GetProcAddress(forwarder, "dlssnr_call_evaluate_v2"));
    auto probe = reinterpret_cast<ProbeFloat>(GetProcAddress(forwarder, "dlssnr_call_probe_float"));
    auto setSlot = reinterpret_cast<SetFloatSlot>(GetProcAddress(forwarder, "dlssnr_call_set_float_slot"));
    auto error = reinterpret_cast<ModelError>(GetProcAddress(forwarder, "dlssnr_call_error"));
    if (!create || !release || !evaluate || !probe || !setSlot)
    {
        std::cerr << "Forwarder lacks required feature exports.\n";
        FreeLibrary(forwarder);
        return false;
    }
    bool foundSlot = false;
    const int candidates[] = { 1, 2, 5, 6, 7, 4, 3, 0 };
    for (int slot : candidates)
    {
        float readback = 0.0f;
        probe(parameters, "DLSSNR.OptiScalerFloatProbe", 0.375f, slot);
        if (parameters->Get("DLSSNR.OptiScalerFloatProbe", &readback) == NVSDK_NGX_Result_Success && readback == 0.375f)
        {
            setSlot(slot);
            foundSlot = true;
            break;
        }
    }
    if (!foundSlot)
    {
        std::cerr << "Could not validate the model parameter float slot.\n";
        FreeLibrary(forwarder);
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC queueDesc {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&commands))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
    {
        std::cerr << "Could not prepare model initialization commands.\n";
        FreeLibrary(forwarder);
        return false;
    }
    std::cerr << "NGX probe: creating one 256x144 neural feature\n" << std::flush;
    void* feature =
        create(modelPath, dataPath, device, commands.Get(), parameters, 256, 144, 0, 1.0f, 0, 0.0f, 0.0f, 0.0f, 1, 1);
    if (!feature)
    {
        std::cerr << "Neural feature creation failed: " << (error ? error() : "no error detail") << "\n";
        FreeLibrary(forwarder);
        return false;
    }
    if (FAILED(commands->Close()))
    {
        release(feature);
        FreeLibrary(forwarder);
        std::cerr << "Could not close model initialization commands.\n";
        return false;
    }
    ID3D12CommandList* lists[] = { commands.Get() };
    queue->ExecuteCommandLists(1, lists);
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event || FAILED(queue->Signal(fence.Get(), 1)) || FAILED(fence->SetEventOnCompletion(1, event)))
    {
        std::cerr << "Model initialization was submitted but could not be tracked; terminating probe.\n" << std::flush;
        if (event)
            CloseHandle(event);
        ExitProcess(3);
    }
    const DWORD wait = WaitForSingleObject(event, 30000);
    CloseHandle(event);
    if (wait != WAIT_OBJECT_0 || FAILED(device->GetDeviceRemovedReason()))
    {
        std::cerr << "Model initialization did not retire cleanly; terminating probe.\n" << std::flush;
        ExitProcess(3);
    }
    const bool evaluated = NrFixture::ProbeEvaluate(device, queue.Get(), feature, parameters, evaluate, error);
    if (!evaluated)
    {
        release(feature);
        FreeLibrary(forwarder);
        return false;
    }
    out.module = forwarder;
    out.release = release;
    out.feature = feature;
    out.queue = queue;
    out.evaluate = evaluate;
    out.error = error;
    return true;
}

// A separate process and app-data directory keep this diagnostic out of the game's NGX session.
// Optional explicit paths enable one feature creation and synthetic evaluation.
int wmain(int argc, wchar_t** argv)
{
    const bool both = (argc == 6 || argc == 7) && std::wcscmp(argv[5], L"--both") == 0;
    const bool repeat = argc == 6 && std::wcscmp(argv[5], L"--repeat") == 0;
    const bool primaryHold = argc == 7 && std::wcscmp(argv[5], L"--primary-hold") == 0;
    const bool secondaryHold = argc == 7 && std::wcscmp(argv[5], L"--secondary-hold") == 0;
    if (argc != 3 && argc != 5 && !both && !repeat && !primaryHold && !secondaryHold)
    {
        std::wcerr
            << L"Usage: nr_ngx_init_probe <absolute _nvngx.dll> <app-data directory> [<forwarder.dll> <model.dll> "
               L"[--both [secondary-model.dll]|--repeat|--primary-hold ready-file|--secondary-hold ready-file]]\n";
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
    ComPtr<ID3D12Device> chosenDevice = secondary.device;
    if (primaryHold)
    {
        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<ID3D12Device> primary;
        if (FAILED(factory->EnumAdapterByLuid(primaryLuid, IID_PPV_ARGS(&adapter))) ||
            FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&primary))))
        {
            std::cerr << "Primary device unavailable.\n";
            return 2;
        }
        chosenDevice = primary;
        std::cout << "Probe device: primary display adapter.\n";
    }
    else
        std::wcout << L"Secondary adapter: " << secondary.description.Description << L"\n";
    std::cerr << "NGX probe: loading driver core\n" << std::flush;
    HMODULE core = LoadLibraryExW(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!core)
    {
        std::cerr << "Could not load the specified NGX core.\n";
        return 1;
    }
    std::cerr << "NGX probe: resolving exports\n" << std::flush;
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
    if (both)
    {
        ComPtr<IDXGIAdapter1> primaryAdapter;
        ComPtr<ID3D12Device> primaryDevice;
        if (FAILED(factory->EnumAdapterByLuid(primaryLuid, IID_PPV_ARGS(&primaryAdapter))) ||
            FAILED(D3D12CreateDevice(primaryAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&primaryDevice))))
        {
            std::cerr << "Primary display D3D12 device unavailable.\n";
            FreeLibrary(core);
            return 2;
        }
        bool primaryStarted = false, secondaryStarted = false;
        NVSDK_NGX_Parameter* primaryParameters = nullptr;
        NVSDK_NGX_Parameter* secondaryParameters = nullptr;
        CreatedFeature primaryFeature, secondaryFeature;
        std::cerr << "NGX probe: initialize primary device\n" << std::flush;
        auto result = init(0x24480451ull, argv[2], primaryDevice.Get(), NVSDK_NGX_Version_API, nullptr);
        primaryStarted = result == NVSDK_NGX_Result_Success;
        bool ok = primaryStarted;
        if (ok)
            ok = get(&primaryParameters) == NVSDK_NGX_Result_Success && primaryParameters;
        if (ok)
            ok = ProbeFeature(primaryDevice.Get(), primaryParameters, argv[2], argv[3], argv[4], primaryFeature);
        if (ok)
        {
            std::cerr << "NGX probe: initialize secondary while primary feature remains live\n" << std::flush;
            result = init(0x24480451ull, argv[2], secondary.device.Get(), NVSDK_NGX_Version_API, nullptr);
            secondaryStarted = result == NVSDK_NGX_Result_Success;
            ok = secondaryStarted;
        }
        if (ok)
            ok = get(&secondaryParameters) == NVSDK_NGX_Result_Success && secondaryParameters;
        if (ok)
            ok = ProbeFeature(secondary.device.Get(), secondaryParameters, argv[2], argv[3],
                              argc == 7 ? argv[6] : argv[4], secondaryFeature);
        if (ok)
        {
            std::cerr << "NGX probe: reevaluate both devices with both features live\n" << std::flush;
            ok =
                NrFixture::ProbeEvaluate(primaryDevice.Get(), primaryFeature.queue.Get(), primaryFeature.feature,
                                         primaryParameters, primaryFeature.evaluate, primaryFeature.error) &&
                NrFixture::ProbeEvaluate(secondary.device.Get(), secondaryFeature.queue.Get(), secondaryFeature.feature,
                                         secondaryParameters, secondaryFeature.evaluate, secondaryFeature.error);
        }
        std::cerr << "NGX probe: releasing primary feature first\n" << std::flush;
        primaryFeature.Reset();
        std::cerr << "NGX probe: releasing secondary feature second\n" << std::flush;
        secondaryFeature.Reset();
        if (secondaryParameters)
            ok = destroy(secondaryParameters) == NVSDK_NGX_Result_Success && ok;
        if (primaryParameters)
            ok = destroy(primaryParameters) == NVSDK_NGX_Result_Success && ok;
        if (secondaryStarted)
            ok = shutdown(secondary.device.Get()) == NVSDK_NGX_Result_Success && ok;
        if (primaryStarted)
            ok = shutdown(primaryDevice.Get()) == NVSDK_NGX_Result_Success && ok;
        FreeLibrary(core);
        if (ok)
            std::cout << "Both device features stayed live and evaluated again in one process.\n";
        return ok ? 0 : 1;
    }
    std::cerr << "NGX probe: initializing core on secondary adapter\n" << std::flush;
    const auto initResult = init(0x24480451ull, argv[2], chosenDevice.Get(), NVSDK_NGX_Version_API, nullptr);
    std::cout << "NGX init result: 0x" << std::hex << static_cast<unsigned>(initResult) << "\n";
    if (initResult != NVSDK_NGX_Result_Success)
    {
        FreeLibrary(core);
        return 1;
    }
    std::cerr << "NGX probe: allocating capability parameters\n" << std::flush;
    NVSDK_NGX_Parameter* parameters = nullptr;
    const auto capResult = get(&parameters);
    std::cout << "NGX capability result: 0x" << std::hex << static_cast<unsigned>(capResult)
              << ", block present: " << (parameters != nullptr) << "\n";
    bool ok = capResult == NVSDK_NGX_Result_Success && parameters;
    CreatedFeature feature;
    if (ok && (argc == 5 || repeat || primaryHold || secondaryHold))
        ok = ProbeFeature(chosenDevice.Get(), parameters, argv[2], argv[3], argv[4], feature);
    if (ok && (primaryHold || secondaryHold))
    {
        HANDLE ready = CreateFileW(argv[6], GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (ready == INVALID_HANDLE_VALUE)
            ok = false;
        else
        {
            CloseHandle(ready);
            std::cerr << "NGX probe: feature ready and waiting for peer\n" << std::flush;
            const std::wstring proceed = std::wstring(argv[6]) + L".go";
            const ULONGLONG deadline = GetTickCount64() + 20000;
            while (GetFileAttributesW(proceed.c_str()) == INVALID_FILE_ATTRIBUTES && GetTickCount64() < deadline)
                Sleep(50);
            if (GetFileAttributesW(proceed.c_str()) == INVALID_FILE_ATTRIBUTES)
                ok = false;
        }
    }
    if (ok && (repeat || primaryHold || secondaryHold))
    {
        std::cerr << "NGX probe: reevaluate the same feature\n" << std::flush;
        ok = NrFixture::ProbeEvaluate(chosenDevice.Get(), feature.queue.Get(), feature.feature, parameters,
                                      feature.evaluate, feature.error);
    }
    feature.Reset();
    if (parameters)
    {
        std::cerr << "NGX probe: destroying capability parameters\n" << std::flush;
        ok = destroy(parameters) == NVSDK_NGX_Result_Success && ok;
    }
    std::cerr << "NGX probe: shutting down core\n" << std::flush;
    ok = shutdown(chosenDevice.Get()) == NVSDK_NGX_Result_Success && ok;
    FreeLibrary(core);
    return ok ? 0 : 1;
}
