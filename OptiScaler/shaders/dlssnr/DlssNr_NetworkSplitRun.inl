// Included in the D3D12 implementation's anonymous namespace. Every entry is called under g_nrMutex.
// Network split (dlssnr/DlssNr_NetworkSplit.h): the helper GPU evaluates the middle of one pass's network. This file
// finds the plan and the helper, starts it, and keeps one generation per configuration; DispatchResumable records the
// two evaluates around it.
namespace NetworkSplitRun
{
using DlssNr::Worker::InitialFrameWire;
using DlssNr::Worker::SplitGeneration;
using Microsoft::WRL::ComPtr;

std::shared_ptr<SplitGeneration> current;
std::wstring currentKey, failedKey;
// Each feature's workspace, captured while it was created (DlssNr_ModelAllocations.h).
std::map<const void*, DlssNr::ModelAllocations::Workspace> workspaces;
bool installed = false;

// Around g_nr.create: when the split is on, the model allocates through our callback so its workspace is known.
void BeforeCreate(const Config& cfg, ID3D12Device* device)
{
    if (!cfg.DlssNrNetworkSplit.value_or_default())
        return;
    if (!installed)
        installed = DlssNr::ModelAllocations::Install(g_nr.capabilityParams);
    DlssNr::ModelAllocations::BeginCreate(device);
}
void AfterCreate(const Config& cfg, const void* feature)
{
    if (!cfg.DlssNrNetworkSplit.value_or_default())
        return;
    const auto ws = DlssNr::ModelAllocations::EndCreate();
    if (feature && ws.address)
        workspaces[feature] = ws;
    else if (feature)
        workspaces.erase(feature);
}
DlssNr::ModelAllocations::Workspace WorkspaceOf(const void* feature)
{
    const auto it = workspaces.find(feature);
    return it == workspaces.end() ? DlssNr::ModelAllocations::Workspace {} : it->second;
}

std::optional<std::filesystem::path> PlanPath(const Config& cfg, unsigned pass, unsigned width, unsigned height)
{
    // Auto: <w>x<h>-<preset>.txt, then <w>x<h>.txt. The plan's workspace size rejects another preset or runtime.
    std::vector<std::string> names;
    const auto chosen = cfg.DlssNrNetworkSplitPlan.value_or_default();
    if (chosen.empty() || chosen == "auto")
    {
        const auto size = std::to_string(width) + "x" + std::to_string(height);
        names = { size + "-" + std::to_string(PassPreset(cfg, pass)) + ".txt", size + ".txt" };
    }
    else
        names = { chosen };
    for (const auto& name : names)
    {
        const std::filesystem::path given(name);
        if (given.is_absolute())
            return given;
        for (const auto& base : { g_dllDir / L"OptiScaler" / L"NR-Split", g_dllDir / L"NR-Split",
                                  Util::ExePath().remove_filename() / L"OptiScaler" / L"NR-Split" })
        {
            std::error_code error;
            if (std::filesystem::is_regular_file(base / given, error) && !error)
                return base / given;
        }
    }
    return std::nullopt;
}

void Retire()
{
    if (current)
        current->Stop(0);
    current.reset();
    currentKey.clear();
}

// The ready generation for this pass, or nullptr while it starts (or after it failed for this configuration). The
// workspace size must match the plan's calibration.
std::shared_ptr<SplitGeneration> Prepare(ID3D12Device* primary, const Config& cfg, unsigned pass, unsigned width,
                                         unsigned height, uint64_t workspaceBytes)
{
    if (!primary || !workspaceBytes || g_nr.isPort)
        return nullptr;
    const auto planPath = PlanPath(cfg, pass, width, height);
    std::wostringstream k;
    k << width << L'x' << height << L':' << pass << L':' << PassPreset(cfg, pass) << L':' << workspaceBytes << L':'
      << (planPath ? planPath->wstring() : L"-") << L':'
      << string_to_wstring(cfg.DlssNrSecondaryModel.value_or_default());
    const auto key = k.str();
    if (current && key != currentKey)
        Retire();
    if (current)
    {
        const HRESULT status = current->Poll();
        if (status == S_OK)
            return current;
        if (status == E_PENDING)
            return nullptr;
        failedKey = key;
        Retire();
        LOG_WARN("DLSS-NR network split: the helper failed or stopped; using the game GPU.");
        return nullptr;
    }
    if (failedKey == key)
        return nullptr;
    auto fail = [&](const std::string& why) -> std::shared_ptr<SplitGeneration>
    {
        failedKey = key;
        LOG_WARN("DLSS-NR network split: {}; using the game GPU.", why);
        return nullptr;
    };
    if (!planPath)
        return fail("no plan file for " + std::to_string(width) + "x" + std::to_string(height) + ", preset " +
                    std::to_string(PassPreset(cfg, pass)) + ", workspace " + std::to_string(workspaceBytes) +
                    " bytes (NR-Split folder)");
    DlssNr::NetworkSplit::Plan plan;
    {
        std::ifstream in(*planPath);
        std::string why;
        if (!in || !DlssNr::NetworkSplit::ParsePlan(in, plan, why))
            return fail("plan " + planPath->string() + ": " + (in ? why : std::string("unreadable")));
    }
    if (!plan.Fits(workspaceBytes))
        return fail("plan " + planPath->string() + " was calibrated for another workspace (" +
                    std::to_string(plan.workspaceBytes) + " bytes, the model has " + std::to_string(workspaceBytes) +
                    ")");
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
    auto createDevice = D3d12Proxy::D3D12CreateDevice_();
    DlssNr::MultiGpu::SelectedAdapter secondary;
    ScopedSkipSpoofingThread skipSpoofing {};
    ScopedCreatingD3DDevice creatingDevice {};
    if (!createDevice || FAILED(DlssNr::MultiGpu::SelectSecondary(factory.Get(), primary->GetAdapterLuid(), nullptr,
                                                                  0x10de, createDevice, secondary)))
        return fail("no compatible secondary NVIDIA adapter was found");
    const auto selectedModel = cfg.DlssNrSecondaryModel.value_or_default();
    const auto model = selectedModel.empty() || selectedModel == "auto"
        ? FindSelectedModel()
        : DlssNr::Worker::ModelCatalog::Resolve({ g_dllDir, Util::ExePath().remove_filename() }, selectedModel);
    std::optional<std::filesystem::path> worker;
    for (const auto& candidate : { g_dllDir / L"OptiScaler" / L"NR-Worker" / L"dlssnr_worker.exe",
                                   g_dllDir / L"NR-Worker" / L"dlssnr_worker.exe",
                                   Util::ExePath().remove_filename() / L"OptiScaler" / L"NR-Worker" /
                                       L"dlssnr_worker.exe" })
    {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error) && !error)
        {
            worker = candidate;
            break;
        }
    }
    const auto coreDir = Util::NvngxPath();
    if (!model || !worker || !coreDir || !g_nr.forwarder)
        return fail("the worker, model or NVIDIA core is missing");
    wchar_t forwarderPath[32768] {};
    const DWORD length =
        GetModuleFileNameW(g_nr.forwarder, forwarderPath, static_cast<DWORD>(std::size(forwarderPath)));
    if (!length || length >= std::size(forwarderPath))
        return fail("the model forwarder path is unavailable");
    std::error_code error;
    auto data = std::filesystem::temp_directory_path(error) / L"OptiScaler-NR-Worker" /
                (std::to_wstring(GetCurrentProcessId()) + L"-split");
    if (!error)
        std::filesystem::create_directories(data, error);
    if (error)
        return fail("the worker data directory could not be created");
    auto next = std::make_shared<SplitGeneration>();
    HRESULT hr = next->Create(primary, secondary.device.Get(), plan);
    if (FAILED(hr))
        return fail("the shared buffer, copy queue or staging could not be created");
    InitialFrameWire wire {};
    wire.width = width;
    wire.height = height;
    wire.applicationId = State::Instance().NVNGX_ApplicationId;
    {
        const auto t = PassTuning(cfg, pass);
        auto& p = wire.passes[0];
        p.preset = static_cast<int>(PassPreset(cfg, pass));
        p.style = static_cast<int>(PassStyle(cfg, pass));
        p.intensity = t.intensity;
        p.structure = t.structure;
        p.tone = t.tone;
        p.skin = t.skin;
        p.autoMask = t.autoMask ? 1 : 0;
    }
    const auto core = *coreDir / L"_nvngx.dll";
    const auto log = data / L"worker.log";
    hr = next->Start(worker->c_str(), core.c_str(), forwarderPath, model->c_str(), data.c_str(), log.c_str(), wire);
    if (FAILED(hr))
        return fail("the helper could not be started");
    current = std::move(next);
    currentKey = key;
    LOG_INFO("DLSS-NR network split: pass {} kernels ({}, {}) on {}, plan {} ({} KiB out, {} KiB back)", pass + 1,
             plan.cut, plan.resume, wstring_to_string(secondary.description.Description), planPath->string(),
             plan.forwardBytes / 1024, plan.backwardBytes / 1024);
    return nullptr; // the helper loads its model asynchronously; this frame stays on the game GPU
}
} // namespace NetworkSplitRun
