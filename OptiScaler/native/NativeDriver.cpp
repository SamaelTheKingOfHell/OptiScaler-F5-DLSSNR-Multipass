#include "pch.h"

#include "NativeDriver.h"
#include "VirtualUpscalerDriver.h"

#include <Config.h>
#include <dlssnr/DlssNrFeature_Dx12.h>
#include <dlssnr/DlssNr_NativeMode.h>

#include <imgui/imgui.h>

#include <cmath>

namespace native
{

namespace
{
constexpr float kPreviewMaxSpeed = 24.0f; // pixels per frame that show as full brightness
} // namespace

void NativeDriver::ReleaseVirtualUpscaler()
{
    if (_virtualUpscaler != nullptr)
        _virtualUpscaler->Release();
}

void NativeDriver::PauseVirtualUpscaler()
{
    if (_virtualUpscaler != nullptr)
        _virtualUpscaler->Pause();
}

bool NativeDriver::MotionUsed()
{
    auto* cfg = Config::Instance();
    return DlssNrNativeMode::MotionUsed(
        { cfg->DlssNrNativeUpscaler.value_or_default(), cfg->DlssNrNativeInput.value_or_default(),
          cfg->DlssNrEnabled.value_or_default(), cfg->DlssNrFinishedPicture.value_or_default(),
          cfg->DlssNrNativeDebugView.value_or_default() });
}

bool NativeDriver::Enabled()
{
    if (!Config::Instance()->DlssNrNativeMotion.value_or_default())
    {
        ReleaseVirtualUpscaler();
        _status = Status::Off;
        return false;
    }

    return true;
}

void NativeDriver::PauseForNativeFg()
{
    if (_status == Status::PausedForNativeFg)
        return;
    if (_producer)
        _producer->Reset();
    PauseVirtualUpscaler();
    _nativeRan = false;
    _status = Status::PausedForNativeFg;
}

void NativeDriver::LogDiagnostics()
{
    if (_diagPresents % 300 != 0)
        return;

    LOG_INFO("{} frames: of {} presents, picture ready {}, waiting for the game's upscaler {}, "
             "submitted {}, flow valid {} (matched with depth {}), trust mask ran {}, native input applied {}, scene "
             "cuts {}, virtual upscaler {}",
             _tag, _diagPresents, _diagReady, _diagWaiting, _diagSubmitted, _diagFlowValid, _diagDepth, _diagTrust,
             _diagNative, _cuts, _virtualUpscaler != nullptr && _virtualUpscaler->Active() ? "active" : "not active");

    if (_producer != nullptr && _producer->GpuStats().Frames() > 0)
        LOG_INFO("{}: {}", _tag, _producer->GpuStats().Report());
}

NativeDriver::RunResult NativeDriver::RunFrame(IFrameSource& source, bool flowPreview, int flowPreviewView)
{
    RunResult out;

    // Nothing reads the motion this frame (NR only with Neural Rendering or Finished Picture off, the motion-only
    // keys): none of it runs. The frame source still does its per-present part (Skip: the Vulkan bridge copies the
    // picture in, held back buffers are let go), the motion history is dropped once, so it starts clean when something
    // wants it, and the hold on a game upscaler starts over.
    if (!MotionUsed())
    {
        source.Skip();

        if (_status != Status::Idle)
        {
            LOG_INFO("{}: idle, nothing uses the motion (no Neural Rendering on the picture, no frame generation feed, "
                     "no debug pictures)",
                     _tag);

            if (_producer)
                _producer->Reset();
        }

        _takeover.Update(false, Util::MillisecondsNow());
        ReleaseVirtualUpscaler();
        DlssNr::NativeInputSkipped();
        _nativeRan = false;
        _status = Status::Idle;
        return out;
    }

    if (_status == Status::Idle)
        _status = Status::Off;

    // FG has stopped. Do not keep the FG pause label while waiting for a new native frame.
    if (_status == Status::PausedForNativeFg)
        _status = Status::Off;

    FrameInput input;
    const auto acquired = source.Acquire(input);

    ++_diagPresents;
    _diagReady += acquired == AcquireStatus::Ready ? 1 : 0;
    _diagWaiting += acquired == AcquireStatus::WaitingForUpscaler ? 1 : 0;

    const bool gameTookOver = _takeover.Update(acquired == AcquireStatus::WaitingForUpscaler, Util::MillisecondsNow());

    if (acquired == AcquireStatus::WaitingForUpscaler)
    {
        // The game's own upscaler call: ours pauses, and goes (with frame generation's context, as a game's feature
        // would) only once the game's calls have run for a while. A menu or cutscene that calls it for a moment would
        // otherwise cost a rebuild and a gap in frame generation each time (VirtualUpscalerHold.h).
        if (gameTookOver)
            ReleaseVirtualUpscaler();
        else
            PauseVirtualUpscaler();

        _status = Status::Waiting;

        if (_producer)
            _producer->Reset();

        LogDiagnostics();
        return out;
    }

    if (acquired != AcquireStatus::Ready)
    {
        LogDiagnostics();
        return out;
    }

    ID3D12Device* device = source.Device();

    if (_producer == nullptr || _producer->Device() != device)
    {
        auto fresh = std::make_unique<NativeProducer>();

        const double initStart = Util::MillisecondsNow();

        if (!fresh->Init(device))
        {
            _failure = fresh->Error();
            _status = Status::Failed;
            LOG_ERROR("{}: {}", _tag, _failure);
            source.Return(input, FrameOutput {});
            return out;
        }

        // The upscaler backend belongs to the old device too.
        if (_virtualUpscaler != nullptr)
        {
            delete _virtualUpscaler;
            _virtualUpscaler = nullptr;
        }

        _producer = std::move(fresh);
        ApplyMotionProfile();
        LOG_INFO("{}: optical flow and trust mask ready{} (in {:.1f} ms, on the present thread)", _tag, _readyNote,
                 Util::MillisecondsNow() - initStart);
    }

    // Presents the picture to an upscaler backend as a synthetic call instead of running DLSS-NR on it. Takes priority
    // when both are on. Under a D3D11 game's frame generation swap chain (Dx11wDx12SC) it runs from OnFGPresent instead
    // of the menu's present.
    const bool useVirtualUpscaler = Config::Instance()->DlssNrNativeUpscaler.value_or_default();

    if (useVirtualUpscaler && _virtualUpscaler == nullptr)
        _virtualUpscaler = new VirtualUpscalerDriver();
    else if (!useVirtualUpscaler)
        ReleaseVirtualUpscaler();

    NativeProducer::Options options;
    options.apply = useVirtualUpscaler || Config::Instance()->DlssNrNativeInput.value_or_default();
    options.flowPreview = flowPreview;
    options.previewMaxSpeed = kPreviewMaxSpeed;
    options.previewView = flowPreviewView;

    // The depth alignment changes what the estimate runs on; the depth view's look is the view's alone.
    auto* config = Config::Instance();
    // A value the ini cannot give as a number is no alignment (a NaN would also never compare equal, and reset the
    // trust mask every frame).
    const auto finite = [](float value, float none) { return std::isfinite(value) ? value : none; };
    options.depthTransform[0] = finite(config->DlssNrNativeDepthScaleX.value_or_default(), 1.0f);
    options.depthTransform[1] = finite(config->DlssNrNativeDepthScaleY.value_or_default(), 1.0f);
    options.depthTransform[2] = finite(config->DlssNrNativeDepthOffsetX.value_or_default(), 0.0f);
    options.depthTransform[3] = finite(config->DlssNrNativeDepthOffsetY.value_or_default(), 0.0f);
    auto& look = _producer->Flow()->DebugTuning();
    look.depthLinearize = config->DlssNrNativeDepthLinearize.value_or_default();
    look.depthMultiplier = config->DlssNrNativeDepthMultiplier.value_or_default();
    look.depthFarPlane = config->DlssNrNativeDepthFarPlane.value_or_default();

    const auto apply = [this, &source, useVirtualUpscaler](ID3D12GraphicsCommandList* cmd, const NativeFrame& frame)
    {
        _pictureHdr10.store(frame.space == ColorSpace::Pq, std::memory_order_relaxed);

        const DXGI_COLOR_SPACE_TYPE type = frame.space == ColorSpace::ScRgb ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                                           : frame.space == ColorSpace::Pq  ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                                                                            : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        const uint64_t epoch =
            PresentEpoch(source.GetApi(), State::Instance().frameCount,
                         State::Instance().vulkanPresentCount.load(std::memory_order_relaxed));

        // The virtual upscaler runs NR on the picture itself while its backend only feeds frame generation, so it takes
        // what NR Only mode takes: the queue, the colour space and the epoch.
        if (useVirtualUpscaler)
            return _virtualUpscaler->Run(cmd, frame, source.Queue(), type, epoch);

        return DlssNr::ApplyNativeInput(source.Queue(), cmd, frame.color, frame.depth, frame.motion,
                                        frame.historyDistrust, frame.depthReversed, frame.reset, type, frame.pictureState,
                                        epoch);
    };

    FrameOutput output;
    const auto result = _producer->Run(source.Queue(), input, options, apply, output);
    source.Return(input, output);

    // Nothing ran on native input this frame (no NR mode, or the producer was not ready): NR's history must not reach across
    if (!result.nativeRan)
        DlssNr::NativeInputSkipped();

    if (result.stoppedAt != nullptr)
    {
        ++_stopped;

        if (_stopped == 1 || _stopped == 100 || _stopped % 1000 == 0)
            LOG_WARN("{}: the producer stopped at \"{}\" (HRESULT 0x{:X}, device removed reason 0x{:X}), "
                     "{} frames so far",
                     _tag, result.stoppedAt, (unsigned) result.stoppedHr, (unsigned) result.deviceRemoved, _stopped);
    }

    _diagFlowValid += result.flowValid ? 1 : 0;
    _diagDepth += result.flowValid && _producer->Flow() != nullptr && _producer->Flow()->UsedDepth() ? 1 : 0;
    _diagTrust += result.trustRan ? 1 : 0;
    _diagNative += result.nativeRan ? 1 : 0;
    _diagSubmitted += result.submitted ? 1 : 0;

    LogDiagnostics();

    _nativeRan = result.nativeRan;

    if (result.trustRan)
        _trustFrame = _frame;

    if (result.flowValid)
    {
        // How often the depth finder has a copy for the mask: the log shows it every 600 frames.
        ++_seen;
        _ran += result.trustRan ? 1 : 0;

        if (_seen == 600)
        {
            LOG_INFO("{}: the trust mask ran in {} of {} frames", _tag, _ran, _seen);
            _seen = _ran = 0;
        }
    }

    if (result.queueChanged && _queueChanges++ < 8)
        LOG_INFO("{}: the game's queue changed (frame generation's game queue came or went, often at a screen "
                 "change); {} (logged the first 8 times)",
                 _tag,
                 result.submitted ? "the last frame's work was done, this one ran"
                                  : "this frame is skipped until the last one's work is done");

    if (result.sceneCut)
    {
        ++_cuts;
        LOG_INFO("{}: scene cut seen ({:.0f}% of the picture distrusted), histories reset", _tag,
                 result.distrustedShare * 100.0f);
    }

    if (result.submitted)
    {
        ++_frame;
        _status = Status::Running;
    }

    out.ran = true;
    out.submitted = result.submitted;
    return out;
}

bool NativeDriver::NrOnlyRunning() const
{
    return _status == Status::Running && _nativeRan && !Config::Instance()->DlssNrNativeUpscaler.value_or_default();
}

void NativeDriver::DrawStatus()
{
    switch (_status)
    {
    case Status::Off:
        ImGui::TextDisabled("Waiting for the first frame.");
        return;
    case Status::Idle:
        ImGui::TextDisabled(
            "Idle: nothing uses the motion (no Neural Rendering on the picture, no frame generation feed), so none of "
            "it runs.");
        return;
    case Status::Waiting:
        ImGui::TextDisabled("Standing aside: the game is calling an upscaler.");
        return;
    case Status::PausedForNativeFg:
        ImGui::TextDisabled("Paused: native frame generation does not provide a native-input application picture.");
        return;
    case Status::Failed:
        ImGui::TextDisabled("Could not start: %s", _failure.c_str());
        return;
    case Status::Running:
        break;
    }

    if (Config::Instance()->DlssNrNativeUpscaler.value_or_default())
    {
        if (_nativeRan && _virtualUpscaler != nullptr && _virtualUpscaler->Active())
        {
            const char* backend = _virtualUpscaler->BackendName().c_str();

            if (_virtualUpscaler->NrInPlace())
                ImGui::TextDisabled("NR on the game's picture (no upscaler pass); %s gives frame generation its input.",
                                    backend);
            else if (_virtualUpscaler->BackendPass())
                ImGui::TextDisabled("%s is running as a stabiliser on this picture.", backend);
            else if (!Config::Instance()->DlssNrEnabled.value_or_default())
                ImGui::TextDisabled("NR is off; %s gives frame generation its input (no upscaler pass).", backend);
            else
            {
                // NR is on but did not run on this frame: say why, as NR Only mode does.
                const std::string reason = DlssNr::FinishedPictureStatus();
                ImGui::TextDisabled("NR is not running: %s", reason.empty() ? "waiting." : reason.c_str());
            }
        }
        else if (_virtualUpscaler != nullptr && !_virtualUpscaler->Error().empty())
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "Upscaler for frame generation: %s",
                               _virtualUpscaler->Error().c_str());
        else
            ImGui::TextDisabled("Upscaler for frame generation: waiting for the motion estimate.");
    }
    else if (_nativeRan)
    {
        ImGui::TextDisabled(_pictureHdr10.load(std::memory_order_relaxed) ? "NR running on this picture (HDR10)."
                                                                          : "NR running on this picture.");
    }
    else
    {
        const std::string reason = DlssNr::FinishedPictureStatus();
        ImGui::TextDisabled("%s", reason.empty() ? "NR: waiting for the motion estimate." : reason.c_str());
    }
}

void NativeDriver::ApplyMotionProfile()
{
    if (!_producer)
        return;

    auto config = Config::Instance();
    const auto profile = config->DlssNrNativeMotionProfile.value_or_default();
    const std::string custom = config->DlssNrNativeMotionProfileCustom.value_or_default();
    native::motion::ApplyProfile(profile, custom, _producer->Flow()->Tuning(), _producer->Trust()->Tuning());
    LOG_INFO("{}: motion profile {}", _tag, native::motion::ProfileIniText(profile));
}

void NativeDriver::DrawFlowTuning()
{
    using native::motion::Profile;
    auto config = Config::Instance();

    // The profile can be picked before a mode runs: it is applied when the motion estimate starts. One not offered yet
    // (an ini naming it) runs as Default, and the combo says so.
    const Profile current = config->DlssNrNativeMotionProfile.value_or_default();
    const Profile shown = native::motion::ProfileOffered(current) ? current : Profile::Default;
    ImGui::SetNextItemWidth(220.0f);

    if (ImGui::BeginCombo("Motion profile##motionprofile", native::motion::ProfileMenuName(shown)))
    {
        for (const Profile profile : native::motion::kAllProfiles)
        {
            if (!native::motion::ProfileOffered(profile))
                continue;

            if (ImGui::Selectable(native::motion::ProfileMenuName(profile), profile == shown) && profile != current)
            {
                config->DlssNrNativeMotionProfile = profile;
                ApplyMotionProfile();
            }
        }

        ImGui::EndCombo();
    }

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", "Settings of the motion estimate for a kind of game. Default suits most games. "
                                "Moving a control under Flow tuning saves your values as Custom, kept for this game.");

    // The depth finder's size and the alignment: for a game whose depth is not found, or does not line up.
    if (ImGui::TreeNode("Depth selection and alignment##nativedepthalign"))
    {
        int size[2] = { (int) config->DlssNrNativeDepthWidth.value_or_default(),
                        (int) config->DlssNrNativeDepthHeight.value_or_default() };

        // Applied on Enter: every new size starts the pick over, which typing digit by digit would do four times.
        if (ImGui::InputInt2("Depth size (0 = automatic)##depthsize", size, ImGuiInputTextFlags_EnterReturnsTrue))
        {
            config->DlssNrNativeDepthWidth = (uint32_t) (std::max) (0, size[0]);
            config->DlssNrNativeDepthHeight = (uint32_t) (std::max) (0, size[1]);
        }

        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", "Only a depth buffer of this size is picked (0 = any width or height). For a game "
                                    "whose scene depth is not found; pick the scene's, not a shadow map's.");

        float scale[2] = { config->DlssNrNativeDepthScaleX.value_or_default(),
                           config->DlssNrNativeDepthScaleY.value_or_default() };

        if (ImGui::DragFloat2("Depth scale X/Y##depthscale", scale, 0.001f, 0.01f, 10.0f, "%.4f"))
        {
            config->DlssNrNativeDepthScaleX = std::clamp(scale[0], 0.01f, 10.0f);
            config->DlssNrNativeDepthScaleY = std::clamp(scale[1], 0.01f, 10.0f);
        }

        float offset[2] = { config->DlssNrNativeDepthOffsetX.value_or_default(),
                            config->DlssNrNativeDepthOffsetY.value_or_default() };

        if (ImGui::DragFloat2("Depth offset X/Y (pixels)##depthoffset", offset, 1.0f, -10000.0f, 10000.0f, "%.1f"))
        {
            config->DlssNrNativeDepthOffsetX = std::clamp(offset[0], -10000.0f, 10000.0f);
            config->DlssNrNativeDepthOffsetY = std::clamp(offset[1], -10000.0f, 10000.0f);
        }

        if (ImGui::Button("Reset alignment##depthalignreset"))
        {
            config->DlssNrNativeDepthScaleX = 1.0f;
            config->DlssNrNativeDepthScaleY = 1.0f;
            config->DlssNrNativeDepthOffsetX = 0.0f;
            config->DlssNrNativeDepthOffsetY = 0.0f;
        }

        ImGui::TextWrapped(
            "The alignment moves the depth the motion estimate and Neural Rendering get (none at 1, 1, 0, 0; a "
            "positive X offset moves it right, a positive Y offset up). Check it in the \"Normals / depth\" view.");

        bool linearize = config->DlssNrNativeDepthLinearize.value_or_default();

        if (ImGui::Checkbox("Linearized depth in the view##depthlinear", &linearize))
            config->DlssNrNativeDepthLinearize = linearize;

        float multiplier = config->DlssNrNativeDepthMultiplier.value_or_default();

        if (ImGui::DragFloat("View multiplier##depthmultiply", &multiplier, 0.1f, 0.0f, 1000000.0f, "%.3f"))
            config->DlssNrNativeDepthMultiplier = (std::max) (0.0f, multiplier);

        float farPlane = config->DlssNrNativeDepthFarPlane.value_or_default();

        if (ImGui::DragFloat("View far plane##depthfar", &farPlane, 1.0f, 1.001f, 1000000.0f, "%.3f"))
            config->DlssNrNativeDepthFarPlane = (std::max) (1.001f, farPlane);

        ImGui::TextDisabled("The view settings change only the \"Normals / depth\" view, not the depth used.");
        ImGui::TreePop();
    }

    // Always shown and open on Optical F5Low's page; the settings live in the running motion estimate, so there is
    // nothing to tune until a mode runs.
    if (!ImGui::TreeNodeEx("Flow tuning (to compare, applies at once)##flowtuning", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    if (!_producer)
    {
        ImGui::TextDisabled("Shown once a mode is running.");
        ImGui::TreePop();
        return;
    }

    auto& tune = _producer->Flow()->Tuning();
    auto& guides = _producer->Trust()->Tuning();
    // What the controls below start from, to tell when one of them moved
    const std::string before = native::motion::WriteKnobs(tune, guides);

    ImGui::SetNextItemWidth(160.0f);
    // AlwaysClamp: a typed value stays in the knob's range, so Custom reads back what ran
    constexpr ImGuiSliderFlags clampTyped = ImGuiSliderFlags_AlwaysClamp;
    ImGui::SliderInt("Smoothing radius (0 = off)##flowsmooth", &tune.smoothRadius, 0, 4, "%d", clampTyped);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderInt("Search radius##flowsearch", &tune.radius, 1, 3, "%d", clampTyped);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderInt("Coarse cells as candidates##flowcells", &tune.coarseCells, 1, 9, "%d", clampTyped);
    ImGui::Checkbox("Last frame's flow as a candidate##flowhistory", &tune.useHistory);
    ImGui::Checkbox("Match within a surface (uses depth)##flowdepth", &tune.depthMatching);

    ImGui::Checkbox("Sharp motion at outlines (uses depth)##flowguidesurface", &guides.guideSurface);

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", "Each pixel at the outline of a character or object takes its own surface's motion, "
                                "not a blend with what's behind it. Frame generation draws fewer see-through edges.");
    ImGui::Checkbox("Background motion behind moving things (uses depth)##flowuncover", &guides.uncoverBackground);

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", "Background that comes out from behind a character (on a camera turn, or as it "
                                "walks) moves with the background, instead of a guess. Frame generation smears less "
                                "behind it.");
    ImGui::Checkbox("Match within what looks alike##flowlook", &tune.lookWeights);

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", "Matches motion only against neighbours of similar brightness, so a moving edge "
                                "doesn't drag its motion onto what's beside it.");
    ImGui::Checkbox("Camera motion where the picture is flat##flowglobal", &tune.globalCandidate);

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", "Gives flat areas (walls, sky) the whole picture's motion. Off: they get none.");
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderInt("Frames unchanged before it counts as still (0 = off)##flowstill", &tune.stillFrames, 0, 60, "%d",
                     clampTyped);

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", "After this many unchanged frames, an area (a HUD panel) stays still while the "
                                "picture pans. Higher: waits longer. 0: off.");
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("Still: largest change allowed##flowstilleps", &tune.stillEpsilon, 0.0005f, 0.05f, "%.4f",
                       ImGuiSliderFlags_Logarithmic | clampTyped);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("Still: what the camera must win by##flowstillmargin", &tune.stillMargin, 0.0f, 0.2f, "%.3f",
                       clampTyped);
    ImGui::Checkbox("Cheaper sub-pixel refinement##flowinverse", &tune.inverseRefinement);

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", "Faster motion refinement; less exact on thin lines and grain.");
    ImGui::Checkbox("Perceptual luma (HDR and SDR)##flowluma", &tune.perceptualLuma);

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", "Matches motion on perceived lightness, so dark detail counts like bright (HDR and "
                                "SDR). Off: the older method.");
    bool preferStill = tune.zeroMargin > 0.0f;

    if (ImGui::Checkbox("Prefer no motion in flat ground (experimental)##flowzero", &preferStill))
        tune.zeroMargin = preferStill ? 0.001f : 0.0f;

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", "Keeps flat areas still when no motion fits almost as well. Holds HUD panels still, "
                                "but plain walls may not follow the camera. Off by default.");
    ImGui::Checkbox("Find a hard cut on its own frame##flowscene", &tune.sceneCutDetector);

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", "Detects camera cuts on the frame they happen, so old motion isn't trusted across "
                                "them. Fades and exposure changes don't count.");
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("Confidence knee##flowknee", &tune.confidenceKnee, 0.0005f, 0.05f, "%.4f",
                       ImGuiSliderFlags_Logarithmic);

    if (ImGui::Button("Close to the earlier build##flowold"))
    {
        tune.smoothRadius = 0;
        tune.radius = 2;
        tune.coarseCells = 1;
        tune.useHistory = false;
    }

    ImGui::SameLine();

    if (ImGui::Button("Defaults##flowdefaults"))
        native::motion::ApplyProfile(Profile::Default, {}, tune, guides);

    // A control moved: the values are the Custom profile now (saved with the ini), or Default again when they are its
    // values
    if (const std::string after = native::motion::WriteKnobs(tune, guides); after != before)
    {
        OpticalFlowDx12::Settings flowDefaults;
        TrustMaskDx12::Settings trustDefaults;
        const bool isDefault = after == native::motion::WriteKnobs(flowDefaults, trustDefaults);
        config->DlssNrNativeMotionProfile = isDefault ? Profile::Default : Profile::Custom;

        if (!isDefault)
            config->DlssNrNativeMotionProfileCustom = after;
    }

    ImGui::TreePop();
}

void NativeDriver::DrawTrustViewCombo()
{
    if (!_producer)
        return;

    static const char* kViews[] = {
        "Final mask", "Depth check",        "Revealed-surface check", "Flow consistency check",
        "Luma check", "Outside the picture"
    };
    int view = _producer->Trust()->Tuning().debugView;

    ImGui::SetNextItemWidth(220.0f);

    if (ImGui::Combo("Show##trustview", &view, kViews, IM_ARRAYSIZE(kViews)))
        _producer->Trust()->Tuning().debugView = view;
}

} // namespace native
