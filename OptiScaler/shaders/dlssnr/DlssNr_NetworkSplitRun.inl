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
// the pass is recorded one frame later). Its turnaround is estimated from its measured kernel time and the data crossing
// PCIe twice each way, with margins taken from measured runs (a 2560x1440 share came back about 1.4 times slower than
// a plain 12 GB/s estimate). Among the shares that fit, the one with the most helper work net of transfer cost wins:
// on the game GPU every MiB moved costs about 0.007 ms (0.7 ms per 100 MB measured), which is worth about 0.05 ms of
// helper work at the measured speed ratio.
//
// Once running, two measurements decide, not the estimate: if the helper's half is late on more than one frame in ten,
// or the frame is not faster than before the split, the share steps down; when no smaller share is left, the pass
// stays whole on the game GPU. (A late helper makes the game GPU wait, so the frame stretches to the helper's pace and
// an estimate compared with that frame would always look like it fits.)
constexpr double PcieBytesPerMs = 8.0e6;
constexpr double KernelMargin = 1.25, OverheadMs = 0.5;
constexpr double FitMargin = 0.85;
constexpr double TransferCostPerMiB = 0.05; // helper ms of benefit lost per MiB moved
std::vector<double> intervals; // milliseconds between this pass's frames
std::chrono::steady_clock::time_point lastFrame {};
DlssNr::NetworkSplit::Plan currentPlan;
uint64_t helperCap = ~0ull; // only shares below this helper time (after a step down)
unsigned framesRunning = 0;
double periodBefore = 0.0;  // stable frame period with the pass whole, measured before the split started
unsigned lateChecks = 0, lateMisses = 0; // split frames measured, and those whose wait held the game GPU
double lateWaitSum = 0.0;
// Game-GPU time the running share saves per frame (the pass whole minus its two halves), measured.
double gainSum = 0.0;
unsigned gainCount = 0;
// After giving up, the split tries again later: the frame rate changes with the scene.
std::chrono::steady_clock::time_point retryAt {};
// The measured search (Prepare). The estimate below only shortlists shares: the helper's real turnaround depends on the
// PCIe link, both copy queues and the helper's clocks, and in a game it came back about 1.5 times slower than estimated.
// So each shortlisted share is tried for a few seconds and judged on what the game GPU actually gained: the split
// pass's time whole minus its two halves, minus the time the game GPU waited for the helper. The best share with a net
// gain is kept (and remembered for the next start); without one, the pass stays whole and the search repeats later.
std::map<std::pair<int, int>, double> tried; // share -> net game-GPU milliseconds per frame
std::pair<int, int> best { 0, 0 };
double bestNet = 0.0;
bool settled = false;   // running the best share after the search
double windowMs = 0.0;  // game-GPU time from the end of the head to the wait for the helper, measured
constexpr double MinNet = 0.05; // ms per frame a share must gain to be kept
std::wstring searchKey; // the configuration the search results belong to
// A retired share's helper may still have its last picture in flight; the next helper starts once it is gone, so two
// helpers never share the GPU, the worker files or the transfer at once.
std::weak_ptr<DlssNr::Worker::SplitGeneration> retiredGeneration;
unsigned failedTrials = 0; // consecutive trials whose helper failed; three end the search for this configuration
constexpr unsigned WarmUpFrames = 90; // the helper's first frames after it starts are not judged

// Measurements of the split pass, logged every few seconds: its game-GPU time whole and, when split, its two halves,
// and how long the game GPU's queue waited for the helper (a timestamp right before the wait and one at the start of the
// end half). GPU timestamps, read a few frames late. The CPU records a frame's end half one or two frames ahead of the
// GPU, so whether the helper had finished at that moment says nothing; only the GPU-side wait does.
namespace Stats
{
DlssNr::PassTimer timer;
unsigned nextSlot = 0;
struct Use
{
    unsigned slot;
    bool split, stamped;
};
std::vector<Use> pending;
double wholeSum = 0, headSum = 0, endSum = 0, waitSum = 0;
unsigned wholeCount = 0, splitCount = 0, waitCount = 0, waitedCount = 0;
std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
constexpr double WaitedMs = 0.5; // a wait longer than this held the game GPU
// A slot for this frame's split pass, or ~0u when timestamps are unavailable.
unsigned Slot(ID3D12Device* device, ID3D12CommandQueue* queue)
{
    if (!timer.Ready() && (!device || !queue || FAILED(timer.Create(device, queue))))
        return ~0u;
    return nextSlot++;
}
void Used(unsigned slot, bool split, bool stamped = false)
{
    if (slot != ~0u)
        pending.push_back({ slot, split, stamped });
}
void Collect(unsigned pass, double periodMs, const DlssNr::NetworkSplit::Plan* plan)
{
    // Read slots used at least six frames ago (well past the frames in flight), never more than the ring holds.
    while (pending.size() > 6)
    {
        const auto use = pending.front();
        pending.erase(pending.begin());
        double a = 0, b = 0, w = 0;
        if (!use.split && timer.Elapsed(use.slot, 0, 1, a))
        {
            wholeSum += a;
            ++wholeCount;
        }
        else if (use.split && timer.Elapsed(use.slot, 0, 1, a) && timer.Elapsed(use.slot, 2, 3, b))
        {
            headSum += a;
            endSum += b;
            ++splitCount;
            const bool judged = framesRunning > WarmUpFrames + 6;
            if (judged && wholeCount)
            {
                gainSum += wholeSum / wholeCount - a - b;
                ++gainCount;
            }
            double window = 0;
            if (use.stamped && timer.Elapsed(use.slot, 1, 4, window))
                windowMs = windowMs > 0 ? windowMs * 0.9 + window * 0.1 : window;
            if (use.stamped && timer.Elapsed(use.slot, 4, 2, w))
            {
                waitSum += w;
                ++waitCount;
                waitedCount += w > WaitedMs ? 1u : 0u;
                if (judged)
                {
                    ++lateChecks;
                    lateMisses += w > WaitedMs ? 1u : 0u;
                    lateWaitSum += w;
                }
            }
        }
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - lastReport < std::chrono::seconds(5) || (!wholeCount && !splitCount))
        return;
    lastReport = now;
    const double whole = wholeCount ? wholeSum / wholeCount : 0.0;
    const double head = splitCount ? headSum / splitCount : 0.0, end = splitCount ? endSum / splitCount : 0.0;
    if (splitCount)
    {
        char saving[96] = "", wait[96] = "";
        if (wholeCount)
            std::snprintf(saving, sizeof saving, ", whole %.2f ms, saving %.2f ms", whole, whole - head - end);
        if (waitCount)
            std::snprintf(wait, sizeof wait, "; game GPU waited %.2f ms on average, over %.1f ms on %u of %u frames",
                          waitSum / waitCount, WaitedMs, waitedCount, waitCount);
        // Native frame generation handed the previous frame's inputs (inputs/FG/Streamline_Delay.h), when it runs.
        char fg[192] = "";
        static std::pair<unsigned, unsigned> fgBefore {};
        const auto fgNow = SlFgDelay::Counts();
        if (fgNow != fgBefore)
        {
            const char* miss = SlFgDelay::LastMiss();
            std::snprintf(fg, sizeof fg, "; frame generation delayed on %u presents (%u not matched%s%s)",
                          fgNow.first - fgBefore.first, fgNow.second - fgBefore.second,
                          fgNow.second != fgBefore.second && miss ? ": " : "",
                          fgNow.second != fgBefore.second && miss ? miss : "");
        }
        fgBefore = fgNow;
        LOG_INFO("DLSS-NR network split: pass {}, frame {:.1f} ms; split pass on the game GPU {:.2f} + {:.2f} ms "
                 "({} frames){}{}{}; share ({}, {})",
                 pass + 1, periodMs, head, end, splitCount, saving, wait, fg, plan ? plan->cut : 0, plan ? plan->resume : 0);
    }
    else
        LOG_INFO("DLSS-NR network split: pass {}, frame {:.1f} ms; split pass whole on the game GPU {:.2f} ms "
                 "({} frames)",
                 pass + 1, periodMs, whole, wholeCount);
    headSum = endSum = waitSum = 0;
    splitCount = waitCount = waitedCount = 0;
    // The whole-pass figure is kept: it is the baseline the halves are compared with.
    if (wholeCount > 600)
    {
        wholeSum /= 2;
        wholeCount /= 2;
    }
}
} // namespace Stats

double Estimate(const DlssNr::NetworkSplit::Plan& p)
{
    return KernelMargin * p.helperMicros / 1000.0 + 2.0 * double(p.forwardBytes + p.backwardBytes) / PcieBytesPerMs +
           OverheadMs;
}
// Helper work net of what moving its data costs the game GPU, in helper milliseconds.
double Score(const DlssNr::NetworkSplit::Plan& p)
{
    return p.helperMicros / 1000.0 - TransferCostPerMiB * double(p.forwardBytes + p.backwardBytes) / 1048576.0;
}
void Observe()
{
    const auto now = std::chrono::steady_clock::now();
    if (lastFrame.time_since_epoch().count())
    {
        const double ms = std::chrono::duration<double, std::milli>(now - lastFrame).count();
        if (ms > 0.0 && ms < 250.0)
            intervals.push_back(ms);
        if (intervals.size() > 180)
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
// The median frame period once it is steady (the middle half of the last frames within 25 percent of each other, so a
// loading screen or a hitch is not taken for the game's pace), else 0.
double StablePeriodMs()
{
    if (intervals.size() < 120)
        return 0.0;
    auto sorted = intervals;
    std::sort(sorted.begin(), sorted.end());
    const double q1 = sorted[sorted.size() / 4], median = sorted[sorted.size() / 2], q3 = sorted[sorted.size() * 3 / 4];
    return q3 <= 1.25 * q1 ? median : 0.0;
}
void Retire();
// Gives up the running share for a smaller one (chosen again from the whole frame's pace).
void StepDown(const char* why)
{
    LOG_INFO("DLSS-NR network split: share ({}, {}) {}; trying a smaller one", currentPlan.cut, currentPlan.resume, why);
    helperCap = currentPlan.helperMicros;
    Retire();
    intervals.clear();
    lateChecks = lateMisses = 0;
    lateWaitSum = 0.0;
    gainSum = 0.0;
    gainCount = 0;
}

struct Feature
{
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
    DlssNr::ModelAllocations::UseDevice(device);
    if (!installed)
        installed = DlssNr::ModelAllocations::Install(g_nr.capabilityParams);
}
std::filesystem::path CacheDir();
// A feature created with the callbacks in place: its buffers (allocated now or on its first evaluate) are tracked.
void AfterCreate(const Config& cfg, const void* feature)
{
    if (!cfg.DlssNrNetworkSplit.value_or_default() || !feature)
        return;
    if (installed)
        features[feature] = Feature {};
    else
        features.erase(feature);
}
// True while this feature's layout is unknown and its next ordinary evaluate should be recorded.
bool NeedsRecord(const void* feature)
{
    const auto it = features.find(feature);
    return it != features.end() && !it->second.recorded;
}
void Recorded(const void* feature, const std::vector<Cal::Kernel>& launches)
{
    const auto it = features.find(feature);
    if (it == features.end())
        return;
    auto& f = it->second;
    f.recorded = true;
    // The network starts at its synchronization clear; the game's evaluate may launch input copies before it.
    const auto kernels = Cal::FromClear(launches);
    if (kernels.size() != launches.size())
        LOG_INFO("DLSS-NR network split: {} launches before the network's first kernel run unchanged",
                 launches.size() - kernels.size());
    const uint64_t clear = Cal::ClearPointer(kernels);
    f.workspace = DlssNr::ModelAllocations::StartingAt(clear);
    Cal::Layout layout;
    const auto [live, allocated] = DlssNr::ModelAllocations::Counts();
    std::string why = !clear ? "none of the " + std::to_string(launches.size()) +
                                   " launches is the synchronization clear (first: " +
                                   (launches.empty() ? std::string("none") : launches[0].name) + ")"
                             : "no model buffer starts where the first kernel clears (" + std::to_string(live) +
                                   " live buffers, " + std::to_string(allocated) +
                                   " allocations through the callback)";
    if (f.workspace.address && Cal::Decode(kernels, f.workspace.address, f.workspace.bytes, layout, why))
    {
        f.fingerprint = Cal::Fingerprint(layout);
        {
            std::error_code error;
            std::filesystem::create_directories(CacheDir(), error);
            std::ofstream(CacheDir() / L"game-layout.txt", std::ios::trunc) << Cal::Describe(layout);
            std::ofstream(CacheDir() / L"game-params.txt", std::ios::trunc) << Cal::DescribeParams(kernels);
        }
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
    retiredGeneration = current;
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
std::wstring foundKey;
std::optional<Helper> found;
void Select(unsigned pass);
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
    Select(pass);
    Observe();
    Stats::Collect(pass, PeriodMs(), current ? &currentPlan : nullptr);
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
            // Measured, not estimated: a helper that is late, or a frame that is no faster, means a smaller share.
            if (++framesRunning == 1)
            {
                intervals.clear(); // the frame with the split, from here on
                lateChecks = lateMisses = 0;
                lateWaitSum = 0.0;
                gainSum = 0.0;
                gainCount = 0;
            }
            if (currentPlan.helperMicros)
            {
                // Judged on the game GPU's own timestamps: what the pass saved, minus what the game GPU waited. A
                // trial is short; the kept share is re-judged over longer windows, as scenes change. A share that
                // clearly holds the game GPU is cut short.
                const unsigned judge = settled ? 900u : 150u;
                const bool hurting = lateChecks >= 30 && lateWaitSum / lateChecks > 3.0;
                if (hurting || (gainCount >= judge && (lateChecks >= judge / 2 || !Stats::timer.Ready())))
                {
                    const double gain = gainCount ? gainSum / gainCount : -1.0;
                    const double wait = lateChecks ? lateWaitSum / lateChecks : 0.0;
                    const double net = gain - wait;
                    const std::pair<int, int> share { currentPlan.cut, currentPlan.resume };
                    tried[share] = net;
                    failedTrials = 0;
                    LOG_INFO("DLSS-NR network split: pass {} share ({}, {}): saved {:.2f} ms, waited {:.2f} ms, net {:+.2f} ms "
                             "per frame (window for the helper {:.1f} ms){}",
                             pass + 1, share.first, share.second, gain, wait, net, windowMs,
                             settled ? " (kept share, re-checked)" : "");
                    if (net >= MinNet && (net > bestNet || share == best))
                    {
                        best = share;
                        bestNet = net;
                    }
                    else if (share == best)
                    {
                        best = { 0, 0 };
                        bestNet = 0.0;
                    }
                    lateChecks = lateMisses = 0;
                    lateWaitSum = 0.0;
                    gainSum = 0.0;
                    gainCount = 0;
                    if (settled && share == best)
                        return current; // still the best: keep it
                    // Next trial, or the best share, chosen below on the next frame.
                    settled = false;
                    Retire();
                    return nullptr;
                }
            }
            return current;
        }
        if (status == E_PENDING)
            return nullptr;
        Retire();
        if (!settled && currentPlan.helperMicros && ++failedTrials < 3)
        {
            // One failed trial ends that share, not the search.
            tried[{ currentPlan.cut, currentPlan.resume }] = -1000.0;
            LOG_WARN("DLSS-NR network split: the helper for share ({}, {}) failed or stopped; trying another",
                     currentPlan.cut, currentPlan.resume);
            return nullptr;
        }
        failedKey = key;
        LOG_WARN("DLSS-NR network split: the helper failed or stopped; using the game GPU.");
        return nullptr;
    }
    if (failedKey == key)
    {
        if (!retryAt.time_since_epoch().count() || std::chrono::steady_clock::now() < retryAt)
            return nullptr;
        LOG_INFO("DLSS-NR network split: trying the split again at the current frame rate");
        failedKey.clear();
        helperCap = ~0ull;
        retryAt = {};
        intervals.clear();
    }
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
        // One plan per candidate split, all calibrated on the same layout.
        std::vector<DlssNr::NetworkSplit::Plan> calibrated;
        {
            std::ifstream in(calibration->plan);
            calibrated = DlssNr::NetworkSplit::ParsePlans(in);
        }
        if (calibrated.empty())
            return fail("the helper's calibration wrote no usable plan");
        if (calibrated[0].layout != calibration->fingerprint)
        {
            char ids[64];
            std::snprintf(ids, sizeof ids, "%016llx here, %016llx on the helper",
                          static_cast<unsigned long long>(calibration->fingerprint),
                          static_cast<unsigned long long>(calibrated[0].layout));
            return fail("the helper GPU's runtime lays the network out differently from pass " +
                        std::to_string(pass + 1) + " (preset " + std::to_string(PassPreset(cfg, pass)) + "; " + ids +
                        "); use a SecondaryModel that runs this preset, or split a pass with a preset it runs");
        }
        std::error_code error;
        std::filesystem::create_directories(calibration->cache.parent_path(), error);
        std::filesystem::copy_file(calibration->plan, calibration->cache,
                                   std::filesystem::copy_options::overwrite_existing, error);
        LOG_INFO("DLSS-NR network split: calibrated {} candidate splits on the helper{}", calibrated.size(),
                 error ? " (not cached)" : "");
        calibration->client.Stop(5000);
        calibration.reset();
        if (error)
            return fail("the calibrated plan could not be cached");
        return nullptr; // the helper starts on the next frame, from the cache
    }
    if (calibration)
        calibration.reset(); // another configuration's run

    // The helper (adapter, a device on it, the files) is looked up once per configuration: creating devices on every
    // frame under the NR lock can deadlock with the present thread, and it did after a step down.
    std::string why;
    if (foundKey != key || !found)
    {
        found = FindHelper(primary, cfg, why);
        foundKey = key;
        if (!found)
            return fail(why);
        // Each split pass has its own helper process, plan file and log.
        std::error_code dirError;
        found->data /= L"pass" + std::to_wstring(pass + 1);
        std::filesystem::create_directories(found->data, dirError);
        if (dirError)
        {
            found.reset();
            return fail("the worker data directory could not be created");
        }
    }
    auto& helper = found;
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
        // A steady frame below 66 ms: a loading screen is steady too (94 ms in Requiem), and a share sized for it
        // would be far too large for the game itself.
        const double period = StablePeriodMs();
        if (period <= 0.0 || period > 66.0)
            return nullptr; // still measuring the game's own steady pace
        if (!retiredGeneration.expired())
            return nullptr; // the previous share's helper is still finishing its last picture
        periodBefore = period;
        if (searchKey != key)
        {
            // Another model, size, preset or runtime: start the search over, from the share remembered for it.
            searchKey = key;
            tried.clear();
            best = { 0, 0 };
            bestNet = 0.0;
            settled = false;
            windowMs = 0.0;
        }
        const auto memory = std::filesystem::path(planPath).replace_extension(L".pass" + std::to_wstring(pass + 1) +
                                                                              L".best");
        const auto find = [&](std::pair<int, int> share) -> const DlssNr::NetworkSplit::Plan*
        {
            for (const auto& p : plans)
                if (p.cut == share.first && p.resume == share.second)
                    return &p;
            return nullptr;
        };
        // The helper's time: measured while a share ran, else the frame minus NR's own GPU time this frame.
        const double budget = windowMs > 0.0 ? windowMs
                              : g_lastGpuTime && *g_lastGpuTime < period ? period - *g_lastGpuTime
                                                                         : FitMargin * period;
        if (tried.empty())
        {
            // The share that won last time is tried first.
            int c = 0, r = 0;
            if (std::ifstream in(memory); in >> c >> r)
                chosen = find({ c, r });
        }
        if (!chosen)
            for (const auto& p : plans) // the shortlist: generous, since the trial decides
                if (!tried.count({ p.cut, p.resume }) && Estimate(p) <= 1.4 * budget && Score(p) > 0.0 &&
                    (!chosen || Score(p) > Score(*chosen)))
                    chosen = &p;
        if (!chosen && best.first)
        {
            chosen = find(best);
            settled = chosen != nullptr;
            if (settled)
            {
                LOG_INFO("DLSS-NR network split: pass {} keeps share ({}, {}), net {:+.2f} ms per frame, after trying "
                         "{} shares", pass + 1, best.first, best.second, bestNet, tried.size());
                std::error_code error;
                std::filesystem::create_directories(memory.parent_path(), error);
                std::ofstream(memory, std::ios::trunc) << best.first << ' ' << best.second << '\n';
            }
        }
        if (!chosen)
        {
            retryAt = std::chrono::steady_clock::now() + std::chrono::seconds(90);
            const auto count = tried.size();
            tried.clear(); // search again later: the scene and the frame rate change
            windowMs = 0.0;
            return fail(count ? "none of the " + std::to_string(count) +
                                    " shares tried gained game-GPU time; the pass stays whole and the search repeats "
                                    "in 90 s"
                              : "no share returns within the helper's window (" + std::to_string(budget).substr(0, 4) +
                                    " ms); the pass stays whole");
        }
        LOG_INFO("DLSS-NR network split: pass {} {} share ({}, {}): window {:.1f} ms, estimated {:.1f} ms ({:.1f} ms "
                 "of kernels, {} KiB out, {} KiB back)",
                 pass + 1, settled ? "keeping" : "trying", chosen->cut, chosen->resume, budget, Estimate(*chosen),
                 chosen->helperMicros / 1000.0, chosen->forwardBytes / 1024, chosen->backwardBytes / 1024);
    }
    const auto plan = *chosen;
    auto next = std::make_shared<SplitGeneration>();
    if (FAILED(next->Create(primary, helper->adapter.device.Get(), plan)))
        return fail("the shared buffer, copy queue or staging could not be created");
    const auto log = helper->data / (L"worker-" + std::to_wstring(plan.cut) + L"-" + std::to_wstring(plan.resume) + L".log");
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

// More than one pass can be split. Everything above that belongs to the split of one pass (its helper, plan,
// measurements, timer and calibration) is the selected pass's; the other passes' are parked here and exchanged in
// before a pass is handled. Select before touching any of it for a pass, and again after a suspension.
struct PassState
{
    std::shared_ptr<SplitGeneration> current;
    std::wstring currentKey, failedKey, foundKey;
    std::optional<Helper> found;
    std::vector<double> intervals;
    std::chrono::steady_clock::time_point lastFrame {}, retryAt {};
    DlssNr::NetworkSplit::Plan currentPlan;
    uint64_t helperCap = ~0ull;
    unsigned framesRunning = 0, lateChecks = 0, lateMisses = 0, gainCount = 0;
    double periodBefore = 0.0, lateWaitSum = 0.0, gainSum = 0.0;
    std::unique_ptr<Calibration> calibration;
    std::map<std::pair<int, int>, double> tried;
    std::pair<int, int> best { 0, 0 };
    double bestNet = 0.0, windowMs = 0.0;
    bool settled = false;
    std::wstring searchKey;
    std::weak_ptr<SplitGeneration> retiredGeneration;
    unsigned failedTrials = 0;
    DlssNr::PassTimer timer;
    unsigned nextSlot = 0;
    std::vector<Stats::Use> pending;
    double wholeSum = 0, headSum = 0, endSum = 0, waitSum = 0;
    unsigned wholeCount = 0, splitCount = 0, waitCount = 0, waitedCount = 0;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};
std::array<PassState, DlssNr::MaxPassCount> parked;
unsigned selected = 0;
void Exchange(PassState& s)
{
    using std::swap;
    swap(s.current, current);
    swap(s.currentKey, currentKey);
    swap(s.failedKey, failedKey);
    swap(s.foundKey, foundKey);
    swap(s.found, found);
    swap(s.intervals, intervals);
    swap(s.lastFrame, lastFrame);
    swap(s.retryAt, retryAt);
    swap(s.currentPlan, currentPlan);
    swap(s.helperCap, helperCap);
    swap(s.framesRunning, framesRunning);
    swap(s.lateChecks, lateChecks);
    swap(s.lateMisses, lateMisses);
    swap(s.gainCount, gainCount);
    swap(s.periodBefore, periodBefore);
    swap(s.lateWaitSum, lateWaitSum);
    swap(s.gainSum, gainSum);
    swap(s.calibration, calibration);
    swap(s.tried, tried);
    swap(s.best, best);
    swap(s.bestNet, bestNet);
    swap(s.windowMs, windowMs);
    swap(s.settled, settled);
    swap(s.searchKey, searchKey);
    swap(s.retiredGeneration, retiredGeneration);
    swap(s.failedTrials, failedTrials);
    swap(s.timer, Stats::timer);
    swap(s.nextSlot, Stats::nextSlot);
    swap(s.pending, Stats::pending);
    swap(s.wholeSum, Stats::wholeSum);
    swap(s.headSum, Stats::headSum);
    swap(s.endSum, Stats::endSum);
    swap(s.waitSum, Stats::waitSum);
    swap(s.wholeCount, Stats::wholeCount);
    swap(s.splitCount, Stats::splitCount);
    swap(s.waitCount, Stats::waitCount);
    swap(s.waitedCount, Stats::waitedCount);
    swap(s.lastReport, Stats::lastReport);
}
void Select(unsigned pass)
{
    if (pass == selected || pass >= parked.size())
        return;
    Exchange(parked[selected]);
    Exchange(parked[pass]);
    selected = pass;
}
// A pass that is no longer split gives its helper back (its measurements start over when it is split again).
void Release(unsigned pass)
{
    if (pass >= parked.size())
        return;
    Select(pass);
    if (!current && !calibration)
        return;
    LOG_INFO("DLSS-NR network split: pass {} is no longer split; its helper stops", pass + 1);
    calibration.reset();
    Retire();
    failedKey.clear();
    helperCap = ~0ull;
    searchKey.clear();
    settled = false;
}
} // namespace NetworkSplitRun
