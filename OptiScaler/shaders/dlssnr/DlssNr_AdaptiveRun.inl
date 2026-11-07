// Included inside Late after its slots and reset helpers. Caller holds g_nrMutex.
namespace Automatic
{
AdaptiveBalance::Controller controller;
std::string workload;
uint64_t workloadVersion = 0, lastPicture = 0;
double lastCompletion = 0;

double Seconds()
{ return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// Conservative preflight avoids delayed-picture copies when NR must use another route.
// ```text
// unsupported option -> keep its normal renderer; no extra pipeline snapshot/copy
// ```
bool ConfiguredRoute(const Config& cfg, bool gameFrameHandoff)
{
    // The pass handoff (MultiGpu) and the network split (NetworkSplit) share this route.
    return (cfg.DlssNrMultiGpu.value_or_default() || cfg.DlssNrNetworkSplit.value_or_default()) &&
        cfg.DlssNrEnabled.value_or_default() &&
        cfg.DlssNrFinishedPicture.value_or_default() && cfg.DlssNrApplyModel.value_or_default() &&
        cfg.DlssNrPasses.value_or_default() <= DefaultMaxPassCount &&
        cfg.DlssNrMultiViewScreens.value_or_default() == 0 && !gameFrameHandoff &&
        State::Instance().swapchainInteropApi == SwapchainInteropApi::None && !g_multiRun && !g_nr.isPort &&
        !cfg.DlssNrRunBeforeSr.value_or_default() && !cfg.DlssNrDeferredDlss.value_or_default() &&
        !cfg.DlssNrDetailReuse.value_or_default() && !cfg.DlssNrEditCache.value_or_default() &&
        !cfg.DlssNrSpatialCompression.value_or_default() && !DlssNr::SuperiorScaling::Enabled(cfg) &&
        !(State::Instance().currentFG && State::Instance().currentFG->IsActive() &&
          !State::Instance().currentFG->IsPaused());
}

void Publish()
{
    using namespace Worker::AdaptiveStatus;
    primaryPasses.store(controller.PrimaryPasses(), std::memory_order_relaxed);
    medianFrameMs.store(static_cast<float>(controller.MedianMs()), std::memory_order_relaxed);
    p95FrameMs.store(static_cast<float>(controller.P95Ms()), std::memory_order_relaxed);
    phase.store(static_cast<uint32_t>(controller.CurrentPhase()), std::memory_order_relaxed);
}

// Workload identity excludes placement and frame-varying exposure/jitter. Those are
// respectively the experimental variable and ordinary game data, not a new trial.
// ```text
// same resolution + pass settings + runtime -> comparable split trials
// ```
unsigned Configure(const Config& cfg, const D3D12_RESOURCE_DESC& desc, const DlssNrFrameInfo& frame,
                   bool supported)
{
    if (!cfg.DlssNrMultiGpuAutoBalance.value_or_default() || !supported)
    {
        controller.Stop(); workload.clear(); lastCompletion = 0; lastPicture = 0;
        Publish();
        if (cfg.DlssNrMultiGpuAutoBalance.value_or_default())
            Worker::AdaptiveStatus::phase.store(static_cast<uint32_t>(AdaptiveBalance::Phase::Waiting),
                                                std::memory_order_relaxed);
        return ~0u;
    }
    const auto total = std::clamp(cfg.DlssNrPasses.value_or_default(), 1u, DefaultMaxPassCount);
    const auto manual = cfg.DlssNrSecondaryPassMask.has_value()
        ? MultiGpu::MakePassPlanFromMask(total, cfg.DlssNrSecondaryPassMask.value(), MaxPassCount)
        : MultiGpu::MakePassPlan(total, std::min(total, cfg.DlssNrPrimaryPasses.value_or_default()), MaxPassCount);
    std::ostringstream key;
    key << device.Get() << ':' << desc.Width << ':' << desc.Height << ':' << desc.Format << ':'
        << frame.RenderSubrectWidth << ':' << frame.RenderSubrectHeight << ':' << frame.OutputWidth << ':'
        << frame.OutputHeight << ':' << total << ':' << std::hexfloat << cfg.DlssNrWorkingScale.value_or_default()
        << ':' << cfg.DlssNrModelResolutionAuto.value_or_default() << ':' << cfg.DlssNrPrecision.value_or_default()
        << ':' << cfg.DlssNrSecondaryModel.value_or_default() << ':' << cfg.DlssNrMultiGpuPipelined.value_or_default()
        << ':' << cfg.DlssNrPassFeedback.value_or_default() << ':' << cfg.DlssNrReversibleMode.value_or_default();
    for (unsigned pass = 0; pass < total; ++pass)
    {
        const auto t = PassTuning(cfg, pass);
        key << ':' << PassPreset(cfg, pass) << ':' << PassStyle(cfg, pass) << ':' << t.intensity << ':'
            << t.structure << ':' << t.tone << ':' << t.skin << ':' << t.autoMask;
    }
    const auto next = key.str();
    if (next != workload)
    {
        workload = next; ++workloadVersion; lastCompletion = 0; lastPicture = 0;
    }
    controller.Begin(total, manual ? manual->primaryPasses : total, workloadVersion, Seconds());
    Worker::AdaptiveStatus::totalPasses.store(total, std::memory_order_relaxed);
    Publish();
    return controller.PrimaryPasses();
}

// Samples describe native picture submission cadence, including remaining CPU waits.
// They are not isolated inference timings and never count generated/duplicate pictures.
// ```text
// warmed matching result -> interval sample; worker startup/fallback -> no score
// ```
void Completed(uint64_t context, uint64_t revision, uint64_t picture, unsigned primary,
               bool ran, const Worker::DispatchContext& dispatch)
{
    if (controller.CurrentPhase() == AdaptiveBalance::Phase::Off || context != controller.Context() ||
        revision != controller.Revision() || primary != controller.PrimaryPasses() || picture <= lastPicture) return;
    const double now = Seconds();
    const double interval = lastCompletion ? (now - lastCompletion) * 1000.0 : 0;
    lastCompletion = now; lastPicture = picture;
    const auto total = Worker::AdaptiveStatus::totalPasses.load(std::memory_order_relaxed);
    const auto status = Worker::currentStatus.load(std::memory_order_relaxed);
    const bool failed = primary < total &&
        (status == Worker::Status::Failed || status == Worker::Status::Unavailable);
    const bool ready = ran && dispatch.balanceEligible && (primary == total || dispatch.needsRetirement);
    const auto before = controller.PrimaryPasses();
    controller.Observe(context, picture, now, interval, ready, failed);
    if (before != controller.PrimaryPasses())
    {
        RequestReset(); lastCompletion = 0;
        LOG_INFO("DLSS-NR automatic balance: trial {} primary / {} secondary passes; presets unchanged",
                 controller.PrimaryPasses(), total - controller.PrimaryPasses());
    }
    Publish();
}
} // namespace Automatic
