// Included in the D3D12 implementation's anonymous namespace. Every entry is called under g_nrMutex.
// Network split (dlssnr/DlssNr_NetworkSplit.h): the helper GPU evaluates the middle of one pass's network. This file
// works out the plan for whatever model is running and keeps one helper per configuration; DispatchResumable records
// the two evaluates around it.
//
// The plan is not tied to a resolution (dlssnr/DlssNr_SplitCalibration.h):
//   1. While the split is on, each feature's buffers are captured as it is created (DlssNr_ModelAllocations.h).
//   2. One ordinary evaluate of the split pass is recorded (DlssNrNative::BeginRecord); its first kernel names the
//      workspace, and the chain gives the layout and its fingerprint.
//   3. A plan cached under that fingerprint (and the helper runtime) is used as it is. Otherwise the helper worker
//      calibrates on its own GPU (read-only snapshots, about 5 to 15 seconds while the game keeps running), and its
//      plan is used only when the helper's layout fingerprint equals ours. Then it is cached.
// NetworkSplitPlan names a fixed plan file instead (relative names under OptiScaler/NR-Split/).
namespace NetworkSplitRun
{
using DlssNr::Worker::InitialFrameWire;
using DlssNr::Worker::SplitGeneration;
using Microsoft::WRL::ComPtr;
namespace Cal = DlssNr::NetworkSplit::Calibration;

std::shared_ptr<SplitGeneration> current;
std::wstring currentKey, failedKey;
bool installed = false;

// Choosing the split by speed. The helper's share must come back within the frame (the split is pipelined: the end of
// the pass is recorded one frame later), so its turnaround is estimated as its measured kernel time plus the data
// crossing PCIe twice each way (game GPU -> shared memory -> helper and back) plus fixed overhead, and the largest
// share whose estimate fits the frame period (with margin) is chosen. Once running, frames get faster; a share that no
// longer fits steps down to a smaller one.
constexpr double PcieBytesPerMs = 12.0e6; // about 12 GB/s per link and direction
constexpr double OverheadMs = 0.5;
constexpr double FitMargin = 0.85, StepDownAt = 0.95;
std::vector<double> intervals; // milliseconds between this pass's frames
std::chrono::steady_clock::time_point lastFrame {};
DlssNr::NetworkSplit::Plan currentPlan;
uint64_t helperCap = ~0ull; // only shares below this helper time (after a step down)
unsigned framesRunning = 0;

double Estimate(const DlssNr::NetworkSplit::Plan& p)
{
    return p.helperMicros / 1000.0 + 2.0 * double(p.forwardBytes + p.backwardBytes) / PcieBytesPerMs + OverheadMs;
}
void Observe()
{
    const auto now = std::chrono::steady_clock::now();
    if (lastFrame.time_since_epoch().count())
    {
        const double ms = std::chrono::duration<double, std::milli>(now - lastFrame).count();
        if (ms > 0.0 && ms < 250.0)
            intervals.push_back(ms);
        if (intervals.size() > 120)
            intervals.erase(intervals.begin());
    }
    lastFrame = now;
}
// The median frame period, 0 until enough frames were seen.
double PeriodMs()
{
    if (intervals.size() < 60)
        return 0.0;
    auto sorted = intervals;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    return sorted[sorted.size() / 2];
}

struct Feature
{
    std::vector<DlssNr::ModelAllocations::Workspace> buffers;
    DlssNr::ModelAllocations::Workspace workspace;
    uint64_t fingerprint = 0;
    bool recorded = false; // the layout was looked at (fingerprint 0 = it did not decode)
};
std::map<const void*, Feature> features;

// A calibration run of the helper worker, for one layout.
struct Calibration
{
    DlssNr::MultiGpu::CrossAdapterBuffer transfer;
    DlssNr::Worker::WorkerClient client;
    std::filesystem::path plan, cache;
    uint64_t fingerprint = 0;
    std::wstring key;
};
std::unique_ptr<Calibration> calibration;

// Around g_nr.create: when the split is on, the model allocates through our callback so its buffers are known.
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
    auto buffers = DlssNr::ModelAllocations::EndCreate();
    if (!feature)
        return;
    features[feature] = Feature {};
    if (!buffers.empty())
        features[feature].buffers = std::move(buffers);
    else
        features.erase(feature);
}
// True while this feature's layout is unknown and its next ordinary evaluate should be recorded.
bool NeedsRecord(const void* feature)
{
    const auto it = features.find(feature);
    return it != features.end() && !it->second.recorded;
}
void Recorded(const void* feature, const std::vector<Cal::Kernel>& kernels)
{
    const auto it = features.find(feature);
    if (it == features.end())
        return;
    auto& f = it->second;
    f.recorded = true;
    f.workspace = DlssNr::ModelAllocations::StartingAt(f.buffers, Cal::ClearPointer(kernels));
    Cal::Layout layout;
    std::string why = "the workspace buffer was not among the model's allocations";
    if (f.workspace.address && Cal::Decode(kernels, f.workspace.address, f.workspace.bytes, layout, why))
    {
        f.fingerprint = Cal::Fingerprint(layout);
        LOG_INFO("DLSS-NR network split: {} kernels, workspace {} bytes, synchronization {} bytes, layout {:016x}",
                 kernels.size(), f.workspace.bytes, layout.syncBytes, f.fingerprint);
    }
    else
        LOG_WARN("DLSS-NR network split: the model's layout could not be read ({}); it stays on the game GPU.", why);
}
// The workspace of a feature whose layout is known, or {}.
DlssNr::ModelAllocations::Workspace WorkspaceOf(const void* feature)
{
    const auto it = features.find(feature);
    return it == features.end() || !it->second.fingerprint ? DlssNr::ModelAllocations::Workspace {}
                                                           : it->second.workspace;
}
bool KnownWithoutBuffers(const void* feature) { return features.find(feature) == features.end(); }

std::optional<std::filesystem::path> FixedPlanPath(const Config& cfg)
{
    const auto chosen = cfg.DlssNrNetworkSplitPlan.value_or_default();
    if (chosen.empty() || chosen == "auto")
        return std::nullopt;
    const std::filesystem::path given(chosen);
    if (given.is_absolute())
        return given;
    for (const auto& base : { g_dllDir / L"OptiScaler" / L"NR-Split", g_dllDir / L"NR-Split",
                              Util::ExePath().remove_filename() / L"OptiScaler" / L"NR-Split" })
    {
        std::error_code error;
        if (std::filesystem::is_regular_file(base / given, error) && !error)
            return base / given;
    }
    return given; // reported as unreadable below
}
std::filesystem::path CacheDir()
{
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    std::error_code error;
    return (local && *local ? std::filesystem::path(local) : std::filesystem::temp_directory_path(error)) /
           L"OptiScaler" / L"NR-Split";
}

void Retire()
{
    if (current)
        current->Stop(0);
    current.reset();
    currentKey.clear();
}

// Everything the helper needs, found once per start.
struct Helper
{
    DlssNr::MultiGpu::SelectedAdapter adapter;
    std::filesystem::path worker, model, core, data;
    std::wstring forwarder;
};
std::optional<Helper> FindHelper(ID3D12Device* primary, const Config& cfg, std::string& why)
{
    Helper h;
    ComPtr<IDXGIFactory4> factory;
    {
        DxgiProxy::ScopedFactoryCreation factoryCreation {};
        ComPtr<IDXGIFactory> base;
        auto createFactory = DxgiProxy::CreateDxgiFactory_();
        if (!createFactory ||
            FAILED(createFactory(__uuidof(IDXGIFactory), reinterpret_cast<IDXGIFactory**>(base.GetAddressOf()))) ||
            FAILED(base.As(&factory)))
            return why = "could not enumerate graphics adapters", std::nullopt;
    }
    auto createDevice = D3d12Proxy::D3D12CreateDevice_();
    ScopedSkipSpoofingThread skipSpoofing {};
    ScopedCreatingD3DDevice creatingDevice {};
    if (!createDevice || FAILED(DlssNr::MultiGpu::SelectSecondary(factory.Get(), primary->GetAdapterLuid(), nullptr,
                                                                  0x10de, createDevice, h.adapter)))
        return why = "no compatible secondary NVIDIA adapter was found", std::nullopt;
    const auto selectedModel = cfg.DlssNrSecondaryModel.value_or_default();
    const auto model = selectedModel.empty() || selectedModel == "auto"
        ? FindSelectedModel()
        : DlssNr::Worker::ModelCatalog::Resolve({ g_dllDir, Util::ExePath().remove_filename() }, selectedModel);
    for (const auto& candidate : { g_dllDir / L"OptiScaler" / L"NR-Worker" / L"dlssnr_worker.exe",
                                   g_dllDir / L"NR-Worker" / L"dlssnr_worker.exe",
                                   Util::ExePath().remove_filename() / L"OptiScaler" / L"NR-Worker" /
                                       L"dlssnr_worker.exe" })
    {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error) && !error)
        {
            h.worker = candidate;
            break;
        }
    }
    const auto coreDir = Util::NvngxPath();
    if (!model || h.worker.empty() || !coreDir || !g_nr.forwarder)
        return why = "the worker, model or NVIDIA core is missing", std::nullopt;
    h.model = *model;
    h.core = *coreDir / L"_nvngx.dll";
    wchar_t forwarderPath[32768] {};
    const DWORD length =
        GetModuleFileNameW(g_nr.forwarder, forwarderPath, static_cast<DWORD>(std::size(forwarderPath)));
    if (!length || length >= std::size(forwarderPath))
        return why = "the model forwarder path is unavailable", std::nullopt;
    h.forwarder = forwarderPath;
    std::error_code error;
    h.data = std::filesystem::temp_directory_path(error) / L"OptiScaler-NR-Worker" /
             (std::to_wstring(GetCurrentProcessId()) + L"-split");
    if (!error)
        std::filesystem::create_directories(h.data, error);
    if (error)
        return why = "the worker data directory could not be created", std::nullopt;
    return h;
}
InitialFrameWire Wire(const Config& cfg, unsigned pass, unsigned width, unsigned height)
{
    InitialFrameWire wire {};
    wire.width = width;
    wire.height = height;
    wire.applicationId = State::Instance().NVNGX_ApplicationId;
    const auto t = PassTuning(cfg, pass);
    auto& p = wire.passes[0];
    p.preset = static_cast<int>(PassPreset(cfg, pass));
    p.style = static_cast<int>(PassStyle(cfg, pass));
    p.intensity = t.intensity;
    p.structure = t.structure;
    p.tone = t.tone;
    p.skin = t.skin;
    p.autoMask = t.autoMask ? 1 : 0;
    return wire;
}
bool ReadPlan(const std::filesystem::path& path, DlssNr::NetworkSplit::Plan& plan, std::string& why)
{
    std::ifstream in(path);
    if (!in)
        return why = "unreadable", false;
    return DlssNr::NetworkSplit::ParsePlan(in, plan, why);
}

// The ready generation for this pass, or nullptr while the plan is worked out or the helper starts (or after a
// failure for this configuration).
std::shared_ptr<SplitGeneration> Prepare(ID3D12Device* primary, const Config& cfg, unsigned pass, unsigned width,
                                         unsigned height, const void* feature)
{
    const auto info = features.find(feature);
    if (!primary || g_nr.isPort || info == features.end() || !info->second.fingerprint)
        return nullptr;
    Observe();
    const auto& f = info->second;
    const auto fixed = FixedPlanPath(cfg);
    std::wostringstream k;
    k << width << L'x' << height << L':' << pass << L':' << PassPreset(cfg, pass) << L':' << std::hex
      << f.fingerprint << L':' << (fixed ? fixed->wstring() : L"auto") << L':'
      << string_to_wstring(cfg.DlssNrSecondaryModel.value_or_default());
    const auto key = k.str();
    if (current && currentKey.rfind(key, 0) != 0)
    {
        Retire();
        helperCap = ~0ull;
    }
    if (current)
    {
        const HRESULT status = current->Poll();
        if (status == S_OK)
        {
            // With the split running the frame is faster; a share that no longer returns in time steps down.
            const double period = ++framesRunning > 180 ? PeriodMs() : 0.0;
            if (currentPlan.helperMicros && period > 0.0 && Estimate(currentPlan) > StepDownAt * period)
            {
                LOG_INFO("DLSS-NR network split: kernels ({}, {}) take about {:.1f} ms on the helper, more than the "
                         "frame now allows ({:.1f} ms); choosing a smaller share",
                         currentPlan.cut, currentPlan.resume, Estimate(currentPlan), period);
                helperCap = currentPlan.helperMicros;
                Retire();
                intervals.clear();
                return nullptr;
            }
            return current;
        }
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
        calibration.reset();
        LOG_WARN("DLSS-NR network split: {}; using the game GPU.", why);
        return nullptr;
    };

    // A calibration in progress: wait for its plan without blocking.
    if (calibration && calibration->key == key)
    {
        const HRESULT ready = calibration->client.WaitForReady(0);
        if (ready == E_PENDING)
            return nullptr;
        if (FAILED(ready))
            return fail("the helper's calibration failed (see worker.log in the worker data folder)");
        DlssNr::NetworkSplit::Plan plan;
        std::string why;
        if (!ReadPlan(calibration->plan, plan, why))
            return fail("the helper's calibration plan: " + why);
        if (plan.layout != calibration->fingerprint)
            return fail("the helper GPU's runtime lays the network out differently; pick a compatible "
                        "SecondaryModel");
        std::error_code error;
        std::filesystem::create_directories(calibration->cache.parent_path(), error);
        std::filesystem::copy_file(calibration->plan, calibration->cache,
                                   std::filesystem::copy_options::overwrite_existing, error);
        LOG_INFO("DLSS-NR network split: calibrated: kernels ({}, {}) on the helper, {} KiB out, {} KiB back{}",
                 plan.cut, plan.resume, plan.forwardBytes / 1024, plan.backwardBytes / 1024,
                 error ? " (not cached)" : "");
        calibration->client.Stop(5000);
        calibration.reset();
        if (error)
            return fail("the calibrated plan could not be cached");
        return nullptr; // the helper starts on the next frame, from the cache
    }
    if (calibration)
        calibration.reset(); // another configuration's run

    std::string why;
    auto helper = FindHelper(primary, cfg, why);
    if (!helper)
        return fail(why);
    std::error_code sizeError;
    const auto modelBytes = std::filesystem::file_size(helper->model, sizeError);
    wchar_t name[64];
    swprintf(name, std::size(name), L"%016llx-%llx.txt", static_cast<unsigned long long>(f.fingerprint),
             static_cast<unsigned long long>(sizeError ? 0 : modelBytes));
    const auto cache = CacheDir() / name;
    const auto planPath = fixed ? *fixed : cache;
    std::error_code error;
    if (!fixed && !std::filesystem::is_regular_file(cache, error))
    {
        auto run = std::make_unique<Calibration>();
        run->key = key;
        run->fingerprint = f.fingerprint;
        run->cache = cache;
        run->plan = helper->data / L"split-plan.txt";
        std::filesystem::remove(run->plan, error);
        if (FAILED(DlssNr::MultiGpu::CrossAdapterBuffer::Create(primary, helper->adapter.device.Get(), 65536,
                                                                run->transfer)))
            return fail("the calibration handshake buffer could not be created");
        auto wire = Wire(cfg, pass, width, height);
        wire.frameCount = 0;
        wire.passCount = 1;
        wire.bytes = run->transfer.Size();
        wire.primary = primary->GetAdapterLuid();
        wire.secondary = helper->adapter.device->GetAdapterLuid();
        wire.split.cut = -1;
        const auto log = helper->data / L"worker.log";
        if (FAILED(run->client.Start(primary, helper->worker.c_str(), helper->core.c_str(), helper->forwarder.c_str(),
                                     helper->model.c_str(), helper->data.c_str(), log.c_str(), run->transfer, wire)))
            return fail("the helper could not be started for calibration");
        calibration = std::move(run);
        LOG_INFO("DLSS-NR network split: calibrating pass {} at {}x{} on {} (one time for this model and size)",
                 pass + 1, width, height, wstring_to_string(helper->adapter.description.Description));
        return nullptr;
    }

    std::vector<DlssNr::NetworkSplit::Plan> plans;
    {
        std::ifstream in(planPath);
        if (!in)
            return fail("plan " + planPath.string() + ": unreadable");
        plans = DlssNr::NetworkSplit::ParsePlans(in);
    }
    plans.erase(std::remove_if(plans.begin(), plans.end(),
                               [&](const DlssNr::NetworkSplit::Plan& p)
                               { return !p.Fits(f.workspace.bytes) || (p.layout && p.layout != f.fingerprint); }),
                plans.end());
    if (plans.empty())
        return fail("plan " + planPath.string() + " has no split for this model's workspace and layout");
    // A fixed single plan without timing is used as it is; otherwise choose by the measured frame period.
    const DlssNr::NetworkSplit::Plan* chosen = nullptr;
    if (plans.size() == 1 && !plans[0].helperMicros)
        chosen = &plans[0];
    else
    {
        const double period = PeriodMs();
        if (period <= 0.0)
            return nullptr; // still measuring the frame
        for (const auto& p : plans)
            if (p.helperMicros < helperCap && Estimate(p) <= FitMargin * period &&
                (!chosen || p.helperMicros > chosen->helperMicros))
                chosen = &p;
        if (!chosen)
            return fail("no share of the network returns from the helper within the frame (" +
                        std::to_string(period).substr(0, 4) + " ms); it stays on the game GPU");
        LOG_INFO("DLSS-NR network split: frame {:.1f} ms; helper share ({}, {}) estimated {:.1f} ms ({:.1f} ms of "
                 "kernels, {} KiB out, {} KiB back)",
                 period, chosen->cut, chosen->resume, Estimate(*chosen), chosen->helperMicros / 1000.0,
                 chosen->forwardBytes / 1024, chosen->backwardBytes / 1024);
    }
    const auto plan = *chosen;
    auto next = std::make_shared<SplitGeneration>();
    if (FAILED(next->Create(primary, helper->adapter.device.Get(), plan)))
        return fail("the shared buffer, copy queue or staging could not be created");
    const auto log = helper->data / L"worker.log";
    if (FAILED(next->Start(helper->worker.c_str(), helper->core.c_str(), helper->forwarder.c_str(),
                           helper->model.c_str(), helper->data.c_str(), log.c_str(), Wire(cfg, pass, width, height))))
        return fail("the helper could not be started");
    current = std::move(next);
    currentKey = key + L':' + std::to_wstring(plan.cut) + L'-' + std::to_wstring(plan.resume);
    currentPlan = plan;
    framesRunning = 0;
    LOG_INFO("DLSS-NR network split: pass {} kernels ({}, {}) on {}, plan {} ({} KiB out, {} KiB back)", pass + 1,
             plan.cut, plan.resume, wstring_to_string(helper->adapter.description.Description), planPath.string(),
             plan.forwardBytes / 1024, plan.backwardBytes / 1024);
    return nullptr; // the helper loads its model asynchronously; this frame stays on the game GPU
}
} // namespace NetworkSplitRun
