#include "pch.h"

#include "NativeDriverDx12.h"

#include <native/Dx12FrameSource.h>
#include <native/FlowDiagnosticUi.h>
#include <native/FpsLog.h>
#include <native/NativeDriver.h>

#include <Config.h>
#include <dlssnr/DlssNr_StreamlinePicture.h>
#include <dlssnr/DlssNrFeature_Dx12.h>
#include <dlssnr/DlssNr_NativeMode.h>
#include <framegen/FGOutput.h>
#include <framegen/xefg/XeFG_ShownRate.h>

#include <menu/menu_overlay_dx.h>
#include <resource_tracking/GenericDepth_Dx12.h>

#include <imgui/imgui.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>

namespace
{

native::NativeDriver g_driver("Native motion", "");
native::Dx12FrameSource g_source;
native::fpslog::Tracker g_fps; // under g_runMutex

const char* ModeName(DlssNrNativeMode::Shown shown)
{
    switch (shown)
    {
    case DlssNrNativeMode::Shown::NrOnly:
        return "NR only";
    case DlssNrNativeMode::Shown::NrAndFrameGeneration:
        return "NR + frame generation";
    case DlssNrNativeMode::Shown::MotionOnly:
        return "motion only";
    default:
        return "Off";
    }
}

const char* OutputName(FGOutput output)
{
    switch (output)
    {
    case FGOutput::FSRFG:
        return "FSR FG";
    case FGOutput::DLSSG:
        return "DLSS-G";
    case FGOutput::XeFG:
        return "XeFG";
    case FGOutput::F5Low:
        return "F5Low";
    default:
        return "none";
    }
}

// The frame rate after each change of a setting that moves it (native/FpsLog.h), whatever the Optical F5Low mode: one
// game present of a D3D12 game, under g_runMutex. Compared: what the player set; in the line: what runs now.
void LogFps()
{
    auto* config = Config::Instance();
    auto& state = State::Instance();
    const bool xefgApi = UsesXeFGApi(state.activeFgOutput);

    native::fpslog::Settings settings;
    settings.nr = config->DlssNrEnabled.value_or_default();
    settings.modelScale = config->DlssNrWorkingScale.value_or_default();
    settings.modelAuto = config->DlssNrModelResolutionAuto.value_or_default();
    settings.upscalerPass = config->DlssNrNativeUpscalerPass.value_or_default();
    settings.mode = ModeName(DlssNrNativeMode::FromKeys(
        { config->DlssNrNativeDepthFinder.value_or_default(), config->DlssNrNativeMotion.value_or_default(),
          config->DlssNrNativeInput.value_or_default(), config->DlssNrNativeUpscaler.value_or_default() }));
    settings.fgOutput = OutputName(state.activeFgOutput);
    settings.fgEnabled = state.activeFgOutput != FGOutput::NoFG && config->IsFGEnabled();
    // XeFG's API (XeFG, F5Low) takes the count asked for; FSR FG makes one frame; DLSS-G's count is its own
    settings.fgCount = xefgApi ? (uint32_t) (std::max) (config->FGXeFGInterpolationCount.value_or_default(), 1) + 1 : 2;
    settings.dynamicMfg = xefgApi && config->FGXeFGDynamicMFG.value_or_default();
    settings.capFps = config->FramerateLimit.value_or_default();

    // Read as the present hooks around it read it (wrapped_swapchain.cpp, FG_Hooks.cpp)
    native::fpslog::Live live;
    auto* fg = state.currentFG;
    live.fgRunning = fg != nullptr && fg->IsActive() && !fg->IsPaused();
    live.fgMultiplier = live.fgRunning ? fg->GetInterpolatedFrameCount() + 1 : 1;

    if (xefgApi && fg != nullptr)
        live.shownTotal = state.xefgShown.load(std::memory_order_relaxed) >> xefg::kShownPresentBits;

    live.shownMask = xefg::kShownFrameMask;
    live.modelPercent = settings.nr ? DlssNr::CurrentModelResolutionPercent() : 0;

    if (auto line = g_fps.OnPresent(Util::MillisecondsNow(), settings, live))
        LOG_INFO("{}", *line);
}

// Under frame generation the menu's present hook sees the real swap chain after frame generation ran, on its presenter's
// thread and queue; FGPresent drives the step instead. The menu's hook stands aside while FGPresent ran within this many of
// its presents (frame generation presents several frames for each of the game's).
constexpr uint64_t kFgPresentGrace = 8;
std::atomic<uint64_t> g_menuPresents { 0 };
std::atomic<uint64_t> g_menuPresentsAtFg { 0 };
std::atomic<bool> g_fgDriven { false };

// The two hooks run on different threads while frame generation starts up. The menu's only tries it: its thread is frame
// generation's presenter, which must never wait on the game thread.
std::mutex g_runMutex;

native::FlowDiagnosticUi g_ui; // the debug view; it asks for the picture while it is shown

// The menu's descriptors for the two preview pictures.
struct PreviewView
{
    bool allocated = false;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu {};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu {};
    ID3D12Resource* resource = nullptr;
};

ID3D12DescriptorHeap* g_srvHeap = nullptr;
PreviewView g_flowView;
PreviewView g_maskView;

// A texture for the menu: a descriptor in the menu's heap (made once, and again if the heap or the texture changes).
// `grayFromRed` shows a one-channel texture as gray. 0 when there is no heap or descriptor yet.
ImTextureID TextureId(PreviewView& view, ID3D12Resource* texture, DXGI_FORMAT format, bool grayFromRed)
{
    ID3D12DescriptorHeap* heap = MenuOverlayDx::SrvHeap();

    ID3D12Device* device = g_driver.Producer() != nullptr ? g_driver.Producer()->Device() : nullptr;

    if (heap == nullptr || texture == nullptr || device == nullptr)
        return 0;

    if (heap != g_srvHeap)
    {
        g_srvHeap = heap;
        g_flowView = PreviewView {};
        g_maskView = PreviewView {};
    }

    if (!view.allocated && MenuOverlayDx::AllocSrv(&view.cpu, &view.gpu))
        view.allocated = true;

    if (!view.allocated)
        return 0;

    if (texture != view.resource)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.Format = format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping =
            grayFromRed ? D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
                              D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
                              D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
                              D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
                              D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1)
                        : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(texture, &srv, view.cpu);
        view.resource = texture;
    }

    return (ImTextureID) view.gpu.ptr;
}

bool ShowTexture(PreviewView& view, ID3D12Resource* texture, DXGI_FORMAT format, bool grayFromRed, float width,
                 float height)
{
    const ImTextureID id = TextureId(view, texture, format, grayFromRed);

    if (id != 0)
        ImGui::Image(id, ImVec2(width, height));

    return id != 0;
}

// The debug view's picture (RGBA8), for the shared controls.
ImTextureID FlowTexture(ID3D12Resource* texture)
{
    return TextureId(g_flowView, texture, DXGI_FORMAT_R8G8B8A8_UNORM, false);
}

void RunFrame(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue, ID3D12Device* device)
{
    if (!g_driver.Enabled())
        return;

    if (swapChain == nullptr || queue == nullptr || device == nullptr || g_driver.Failed())
        return;

    // This producer reads the lower display picture, not Streamline's application buffer.
    // Native FG can supply generated frames even with a registered app hook. With FG Off,
    // keep the ordinary native-input path (the upscaler-source takeover check still applies).
    if (DlssNr::StreamlinePicture::NativeFgRunning())
    {
        g_driver.PauseForNativeFg();
        DlssNr::NativeInputSkipped();
        if (!DlssNr::StreamlinePicture::Owns(swapChain))
            DlssNr::FinishedPictureUnavailable("Paused: native input cannot use generated display frames.");
        return;
    }

    g_source.SetPresent(swapChain, queue, device);

    g_driver.RunFrame(g_source, g_ui.TakeRequest(), g_ui.View());
}

} // namespace

namespace NativeMotionDx12
{

void OnPresent(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue, ID3D12Device* device)
{
    // A D3D11 game's frame generation swap chain is the interop's D3D12 one: its picture is the game's, already handled by
    // NativeMotionDx11 from Dx11wDx12SC::Present, and this present runs inside frame generation's own present, under the
    // lock the virtual upscaler's UpscaleStart (-> EvaluateState) takes again: that deadlocked.
    if (State::Instance().swapchainInteropApi != SwapchainInteropApi::None)
        return;

    const uint64_t presents = ++g_menuPresents;

    if (g_fgDriven.load() && presents - g_menuPresentsAtFg.load() <= kFgPresentGrace)
        return;

    std::unique_lock lock(g_runMutex, std::try_to_lock);

    if (!lock.owns_lock())
        return;

    g_fgDriven = false;
    LogFps();
    GenericDepthDx12::OnPresent(swapChain);
    RunFrame(swapChain, queue, device);
}

void OnFGPresent(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue, ID3D12Device* device)
{
    g_menuPresentsAtFg = g_menuPresents.load();
    g_fgDriven = true;

    std::lock_guard lock(g_runMutex);
    LogFps();
    GenericDepthDx12::OnPresent(swapChain);
    RunFrame(swapChain, queue, device);
}

void DrawStatus()
{
    g_driver.DrawStatus();
}

bool NrOnlyRunning() { return g_driver.NrOnlyRunning(); }

void DrawAdvancedUi()
{
    const bool debugView = Config::Instance()->DlssNrNativeDebugView.value_or_default();

    g_driver.DrawFlowTuning();

    // The pictures and their controls are debugging aids: off by default, nothing is recorded for them while they are hidden.
    if (!debugView)
    {
        g_ui.Reset();
        return;
    }

    g_ui.DrawAdvanced(g_driver, FlowTexture);

    const float boxWidth = 360.0f;
    const float boxHeight = boxWidth * 9.0f / 16.0f;

    // A frame can have no mask (no motion estimate yet); the picture then keeps the last one rather than going black.
    const bool trustRecent = g_driver.TrustRecent();

    if (g_driver.GetStatus() == native::NativeDriver::Status::Running)
    {
        if (trustRecent)
            ImGui::TextDisabled("Trust: white is where the last frame cannot be trusted (%llu cuts seen).",
                                (unsigned long long) g_driver.Cuts());
        else
            ImGui::TextDisabled("Trust: waiting for the motion estimate.");
    }
    else
        ImGui::TextDisabled("Trust: -");

    g_driver.DrawTrustViewCombo();

    bool maskDrawn = false;

    if (g_driver.GetStatus() == native::NativeDriver::Status::Running && trustRecent && g_driver.Producer())
        maskDrawn = ShowTexture(g_maskView, g_driver.Producer()->Trust()->Mask(), DXGI_FORMAT_R8_UNORM, true, boxWidth, boxHeight);

    if (!maskDrawn)
        ImGui::Dummy(ImVec2(boxWidth, boxHeight));
}

bool ExpandedPreviewVisible() { return g_ui.Expanded(); }

void DrawExpandedPreview() { g_ui.DrawExpanded(g_driver, FlowTexture); }

} // namespace NativeMotionDx12
