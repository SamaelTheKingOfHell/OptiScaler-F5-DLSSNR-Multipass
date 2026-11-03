// Included in the D3D12 implementation's anonymous namespace. Every entry is called under g_nrMutex.
namespace MultiGpuRun
{
using DlssNr::Worker::FrameTransfer;
using DlssNr::Worker::InitialFrameWire;
using DlssNr::Worker::WorkerGeneration;
using Microsoft::WRL::ComPtr;

std::shared_ptr<WorkerGeneration> current;
std::weak_ptr<WorkerGeneration> retired;
std::wstring currentKey, failedKey;
bool primaryFeatureSkipped = false;
uint64_t successfulFrames = 0;
double handoffSum = 0.0, handoffMax = 0.0;
unsigned handoffSamples = 0;

void RetireCurrent()
{
    if (!current)
        return;
    current->MarkSkipped();
    retired = current;
    current.reset();
    currentKey.clear();
}
void FailCurrent()
{
    DlssNr::Worker::currentStatus.store(DlssNr::Worker::Status::Failed, std::memory_order_relaxed);
    failedKey = currentKey;
    RetireCurrent();
}
void Skipped()
{
    if (current)
        current->MarkSkipped();
}
void Disabled()
{
    DlssNr::Worker::currentStatus.store(DlssNr::Worker::Status::Disabled, std::memory_order_relaxed);
    RetireCurrent();
    failedKey.clear();
    DlssNr::Worker::handoffMilliseconds.store(0.0f, std::memory_order_relaxed);
}

std::wstring Key(ID3D12Device* primary, ID3D12Resource* color, ID3D12Resource* depth, ID3D12Resource* motion,
                 const Config& cfg, unsigned first, unsigned total)
{
    std::wostringstream out;
    out << reinterpret_cast<uintptr_t>(g_nr.feature) << L':' << primary->GetAdapterLuid().HighPart << L':'
        << primary->GetAdapterLuid().LowPart << L':' << first << L':' << total << L':' << std::hexfloat
        << std::clamp(cfg.DlssNrPassFeedback.value_or_default(), 0.0f, 1.0f)
        << L':' << string_to_wstring(cfg.DlssNrSecondaryModel.value_or_default())
        << L':' << cfg.DlssNrMultiGpuPipelined.value_or_default();
    for (auto* resource : { color, depth, motion })
    {
        if (!resource)
        {
            out << L":none";
            continue;
        }
        const auto d = resource->GetDesc();
        out << L':' << d.Width << L':' << d.Height << L':' << static_cast<unsigned>(d.Format) << L':' << d.MipLevels
            << L':' << d.DepthOrArraySize << L':' << d.SampleDesc.Count;
    }
    for (unsigned pass = first; pass < total; ++pass)
    {
        const auto t = PassTuning(cfg, pass);
        out << L':' << PassPreset(cfg, pass) << L':' << PassStyle(cfg, pass) << L':' << t.intensity << L':'
            << t.structure << L':' << t.tone << L':' << t.skin << L':' << t.autoMask;
    }
    return out.str();
}

// Creates a generation once and polls its startup/retirement without blocking the render path.
// A failure is latched for this exact configuration so it does not spawn a worker every frame.
std::shared_ptr<WorkerGeneration> Prepare(ID3D12Device* primary, ID3D12Resource* color, ID3D12Resource* depth,
                                          ID3D12Resource* motion, const Config& cfg, unsigned first, unsigned total,
                                          ID3D12CommandQueue* orderedQueue = nullptr)
{
    if (!primary || !color || !motion || first >= total || total > DlssNr::MaxPassCount || g_nr.isPort)
        return nullptr;
    const auto key = Key(primary, color, depth, motion, cfg, first, total);
    if (current && key != currentKey)
    {
        const HRESULT status = current->Poll();
        if (status == E_PENDING)
        {
            current->MarkSkipped();
            return nullptr;
        }
        RetireCurrent();
    }
    if (current)
    {
        const HRESULT status = orderedQueue ? current->PollForQueue(orderedQueue) : current->Poll();
        if (status == S_OK)
            return current;
        if (status == E_PENDING)
        {
            current->MarkSkipped();
            return nullptr;
        }
        failedKey = key;
        RetireCurrent();
        DlssNr::Worker::currentStatus.store(DlssNr::Worker::Status::Failed, std::memory_order_relaxed);
        LOG_WARN("DLSS-NR multi-GPU: worker initialization or retirement failed; using the game GPU.");
        return nullptr;
    }
    if (!retired.expired() || failedKey == key)
        return nullptr;
    auto fail = [&](const char* why, DlssNr::Worker::UnavailableReason reason =
                                      DlssNr::Worker::UnavailableReason::Other) -> std::shared_ptr<WorkerGeneration>
    {
        failedKey = key;
        DlssNr::Worker::unavailableReason.store(reason, std::memory_order_relaxed);
        DlssNr::Worker::currentStatus.store(DlssNr::Worker::Status::Unavailable, std::memory_order_relaxed);
        LOG_WARN("DLSS-NR multi-GPU: {}; using the game GPU.", why);
        return nullptr;
    };
    ComPtr<IDXGIFactory4> factory;
    {
        DxgiProxy::ScopedFactoryCreation factoryCreation {};
        ComPtr<IDXGIFactory> base;
        auto createFactory = DxgiProxy::CreateDxgiFactory_();
        if (!createFactory ||
            FAILED(createFactory(__uuidof(IDXGIFactory), reinterpret_cast<IDXGIFactory**>(base.GetAddressOf()))) ||
            FAILED(base.As(&factory)))
            return fail("could not enumerate graphics adapters");
    }
    ComPtr<IDXGIAdapter1> gameAdapter;
    DXGI_ADAPTER_DESC1 gameDescription {};
    if (SUCCEEDED(factory->EnumAdapterByLuid(primary->GetAdapterLuid(), IID_PPV_ARGS(&gameAdapter))) &&
        SUCCEEDED(gameAdapter->GetDesc1(&gameDescription)))
        DlssNr::Worker::Telemetry().Set(0, gameDescription);
    auto createDevice = D3d12Proxy::D3D12CreateDevice_();
    DlssNr::MultiGpu::SelectedAdapter secondary;
    ScopedSkipSpoofingThread skipSpoofing {};
    ScopedCreatingD3DDevice creatingDevice {};
    if (!createDevice || FAILED(DlssNr::MultiGpu::SelectSecondary(factory.Get(), primary->GetAdapterLuid(), nullptr,
                                                                  0x10de, createDevice, secondary)))
        return fail("no compatible secondary NVIDIA adapter was found", DlssNr::Worker::UnavailableReason::NoAdapter);
    DlssNr::Worker::Telemetry().Set(1, secondary.description);
    FrameTransfer transfer;
    const HRESULT transferResult = FrameTransfer::Create(primary, secondary.device.Get(), color, depth, motion, transfer);
    if (FAILED(transferResult))
    {
        const auto c = color->GetDesc();
        const auto d = depth ? depth->GetDesc() : D3D12_RESOURCE_DESC {};
        const auto m = motion->GetDesc();
        LOG_WARN("DLSS-NR multi-GPU transfer failed: HRESULT 0x{:08X}; color {}x{} format {}, depth {}x{} format {}, "
                 "motion {}x{} format {}.", static_cast<unsigned>(transferResult), c.Width, c.Height,
                 static_cast<unsigned>(c.Format), d.Width, d.Height, static_cast<unsigned>(d.Format), m.Width, m.Height,
                 static_cast<unsigned>(m.Format));
        return fail("the frame or guide textures or shared allocation are unsupported",
                    DlssNr::Worker::UnavailableReason::Transfer);
    }
    const auto selectedModel = cfg.DlssNrSecondaryModel.value_or_default();
    const auto model = selectedModel.empty() || selectedModel == "auto" ? FindSelectedModel()
        : DlssNr::Worker::ModelCatalog::Resolve(
            { g_dllDir, Util::ExePath().remove_filename() }, selectedModel);
    // The worker must not run beside the game's dxgi.dll proxy: the Windows
    // loader would inject OptiScaler into its own helper process.
    std::optional<std::filesystem::path> worker;
    const auto exeDir = Util::ExePath().remove_filename();
    for (const auto& candidate : { g_dllDir / L"OptiScaler" / L"NR-Worker" / L"dlssnr_worker.exe",
                                   g_dllDir / L"NR-Worker" / L"dlssnr_worker.exe",
                                   exeDir / L"OptiScaler" / L"NR-Worker" / L"dlssnr_worker.exe" })
    {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error) && !error) { worker = candidate; break; }
    }
    const auto coreDir = Util::NvngxPath();
    if (!model || !worker || !coreDir || !g_nr.forwarder)
        return fail("the worker, model or NVIDIA core is missing", DlssNr::Worker::UnavailableReason::MissingFiles);
    wchar_t forwarderPath[32768] {};
    const DWORD length =
        GetModuleFileNameW(g_nr.forwarder, forwarderPath, static_cast<DWORD>(std::size(forwarderPath)));
    if (!length || length >= std::size(forwarderPath))
        return fail("the model forwarder path is unavailable");
    std::error_code error;
    auto data =
        std::filesystem::temp_directory_path(error) / L"OptiScaler-NR-Worker" / std::to_wstring(GetCurrentProcessId());
    if (error)
        return fail("the worker data directory is unavailable");
    std::filesystem::create_directories(data, error);
    if (error)
        return fail("the worker data directory could not be created");
    auto core = *coreDir / L"_nvngx.dll";
    auto log = data / L"worker.log";
    InitialFrameWire wire = transfer.MakeWire(total - first);
    wire.applicationId = State::Instance().NVNGX_ApplicationId;
    wire.passFeedback = std::clamp(cfg.DlssNrPassFeedback.value_or_default(), 0.0f, 1.0f);
    for (unsigned pass = first; pass < total; ++pass)
    {
        const auto t = PassTuning(cfg, pass);
        auto& p = wire.passes[pass - first];
        p.preset = static_cast<int>(PassPreset(cfg, pass));
        p.style = static_cast<int>(PassStyle(cfg, pass));
        p.intensity = t.intensity;
        p.structure = t.structure;
        p.tone = t.tone;
        p.skin = t.skin;
        p.autoMask = t.autoMask ? 1 : 0;
    }
    auto next = std::make_shared<WorkerGeneration>(std::move(transfer));
    if (FAILED(next->Start(primary, worker->c_str(), core.c_str(), forwarderPath, model->c_str(), data.c_str(),
                           log.c_str(), wire)))
        return fail("the isolated worker could not be started", DlssNr::Worker::UnavailableReason::Startup);
    current = std::move(next);
    currentKey = key;
    successfulFrames = 0;
    handoffSum = handoffMax = 0.0;
    handoffSamples = 0;
    DlssNr::Worker::handoffMilliseconds.store(0.0f, std::memory_order_relaxed);
    DlssNr::Worker::currentStatus.store(DlssNr::Worker::Status::Starting, std::memory_order_relaxed);
    LOG_INFO("DLSS-NR multi-GPU: starting {} secondary passes on {}", total - first,
             wstring_to_string(secondary.description.Description));
    LOG_INFO("DLSS-NR multi-GPU: secondary runtime {}", selectedModel.empty() || selectedModel == "auto"
                                                                 ? "same as game GPU" : selectedModel);
    return nullptr; // model loading is asynchronous; this frame stays on the game GPU
}
} // namespace MultiGpuRun
