#include "pch.h"
#include "menu_common.h"
#include "PresentRateMeter.h"
#include "MenuPages.h"
#include "NotificationsPage.h"
#include "PluginsPage.h"
#include <framegen/dlssg/DlssgTelemetry.h>
#include <dlssnr/DlssNr_ExposureScan.h>

#include <algorithm>
#include <cfloat>

#include <dlssnr/DlssNr.h>
#include <native/VirtualUpscalerDriver.h>
#include <native/NativeLowLatency.h>
#include <native/NativeDriverDx11.h>
#include <native/NativeDriverDx12.h>
#include <native/NativeDriverVk.h>
#include <native/VkPresentBridge.h>
#include <output/SdrToHdr_Create.h>

#include "input/input_system.h"

#include "font/Hack_Compressed.h"
#include "font/Inter_Compressed.h"

#include <proxies/XeSS_Proxy.h>
#include <proxies/XeFG_Proxy.h>
#include <proxies/FfxApi_Proxy.h>
#include <upscalers/ffx/FFXFeature.h>
#include <fsr4/FSR4Models.h>
#include <fsr4/FSR4Presets.h>
#include <fsr4/FSR4DriverProviderVk.h>
#include <proxies/Streamline_Proxy.h>

#include <framegen/mfgunlock/Status.h>
#include <framegen/nvngx/Nvngx_FG.h>

#include <nvapi/fakenvapi.h>
#include <hooks/Reflex_Hooks.h>

#include <version_check.h>

#include <upscaler_time/UpscalerTime_Vk.h>

#include <imgui/imgui_internal.h>
#include <imgui/ImGuiNotify.hpp>
#include <imgui/imgui_impl_win32.h>
#include <imgui/imgui_impl_uwp.h>

#include <mutex>
#include <cstdarg>

#include <array>
#include <chrono>
#include <memory>
#include <type_traits>
#include <misc/IdentifyGpu.h>
#include <misc/XeModuleInfo.h>
#include <misc/FrameLimit.h>
#include <hooks/Xell_Hooks.h>
#include <hooks/Amdxc64_Hooks.h>
#include <low_latency/input/input_common.h>
#include <menu/MenuCard.h>
#include <menu/FgStatusRule.h>

enum class UiTargetMode
{
    SDR,
    LinearHDR,
    ScRGB,
    PQ,
    HLG
};

#define MARK_ALL_BACKENDS_CHANGED()                                                                                    \
    for (auto& singleChangeBackend : State::Instance().changeBackend)                                                  \
        singleChangeBackend.second = true;

static float fontSize = 14.0f; // just changing this doesn't make other elements scale ideally
static ImVec2 overlaySize(0.0f, 0.0f);
static ImVec2 overlayPosition(-1000.0f, -1000.0f);
static bool _hdrTonemapApplied = false;
static ImVec4 SdrColors[ImGuiCol_COUNT];

// Modern style: colours that are drawn outside style.Colors (already tone-mapped for HDR output).
struct ModernPalette
{
    ImVec4 card;
    ImVec4 cardHeader;
    ImVec4 cardHeaderHover;
    ImVec4 cardHeaderActive;
    ImVec4 navBar;
    ImVec4 navSelected;
    ImVec4 navHover;
    ImVec4 navActive;
};

static bool gModernActive = false;
static ModernPalette gModernPalette {};
static ImFont* gHackFont = nullptr;
static ImFont* gInterFont = nullptr;
static ImFont* gClassicFont = nullptr;

bool MenuStyle::IsModern() { return gModernActive; }
ImFont* MenuStyle::MonoFont() { return gHackFont; }
const ImVec4& MenuStyle::CardColor() { return gModernPalette.card; }
const ImVec4& MenuStyle::CardHeaderColor() { return gModernPalette.cardHeader; }
const ImVec4& MenuStyle::CardHeaderHoverColor() { return gModernPalette.cardHeaderHover; }
const ImVec4& MenuStyle::CardHeaderActiveColor() { return gModernPalette.cardHeaderActive; }

static bool inputMenu = false;
static bool inputFG = false;
static bool inputFps = false;
static bool inputFpsCycle = false;
static uint64_t lastInputTick = 0;
static uint64_t lastShortcutFireTick = 0;
constexpr uint64_t debounceThreshold = 1000;

static bool hasGamepad = false;
static bool ffxInitTried = false;
static bool xefgInitTried = false;
static std::string windowTitle;
static std::string selectedUpscalerName = "";
static Upscaler currentBackend = Upscaler::Reset;
static std::string currentBackendName = "";
static int refreshRate = 0;
static ImVec2 lastPosition(-1000.0f, -1000.0f);

// How many frames DLSS frame generation adds per real frame (0: off). A game's own DLSS frame generation reports it on
// each evaluate; OptiScaler's own DLSSG output (OptiFG input, e.g. Optical F5Low's upscaler) gets no such report back,
// so there it is the count OptiScaler asked for while its frame generation runs.
static int DlssgInterpolationCount(const State& state)
{
    if (const int reported = state.dlssgDetectedInterpolationCount; reported > 0)
        return reported;

    const auto fg = state.CurrentFG();

    if (state.activeFgOutput == FGOutput::DLSSG && state.activeFgInput == FGInput::Upscaler && fg != nullptr &&
        fg->IsGenerating())
    {
        return (int) fg->GetInterpolatedFrameCount();
    }

    return 0;
}

static ImVec2 splashPosition(-1000.0f, -1000.0f);
static ImVec2 splashSize(0.0f, 0.0f);
static double splashStart = 0.0;
static double splashLimit = 0.0;
static std::vector<std::string> splashText = { "Cope smarter, not harder",
                                               "Coping is strong with this one...",
                                               "This is where the fun begins...",
                                               "Got any more of them scalers?...",
                                               "Fake pixels and even faker frames...",
                                               "Fake frames, get your fake frames...",
                                               "I'm here to kick pixels and chew frames...",
                                               "I find your lack of supersampling disturbing...",
                                               "Frame by frame, I scale-up!",
                                               "Resistance is futile. Your pixels will be upscaled.",
                                               "I've got 99 problems, but low-res ain't one.",
                                               "It's over, DLSS, I have the higher ground!",
                                               "This isn't the resolution you're looking for",
                                               "To infinity and beyond... with ray tracing off",
                                               "I have a bad feeling about this frame pacing",
                                               "It's Dangerous to Go Alone-Take This Upscaler",
                                               "Upscaled beyond recognition.",
                                               "Trust the process. Ignore the shimmer.",
                                               "Real fake frames. Certified.",
                                               "The illusion of performance",
                                               "This upscaler belongs in a museum!",
                                               "Because native rendering is overrated.",
                                               "The more you upscaler, the more you save",
                                               "It's never too late to buy a better GPU",
                                               "We don't need real pixels where we're going",
                                               "Did you know that Intel released XeFG for everyone?",
                                               "MFG totally works with Nukem's 100%% no scam",
                                               "Some of those pixels might even be real!",
                                               "Just don't look too closely at the image",
                                               "Even supports \"software\" XeSS!",
                                               "It's too blurry to go alone, take RCAS with you",
                                               "Thanks nitec, back to you nitec",
                                               "Tested and approved by By-U",
                                               "0.8 was an inside job",
                                               "FSR4 DP4a wenETA, AMD plz",
                                               "OptiCopers, assemble!",
                                               "The Way It's Meant To Be Upscaled",
                                               "Your game may not even crash today",
                                               "Expanded and Enhanced",
                                               "It's only my 5th crash today",
                                               "Latency with FG? But I have good internet",
                                               "Console peasants can't do that",
                                               "Hope you don't have a good eyesight",
                                               "Such an aggressive upscaling? A bold move",
                                               "I almost don't feel the input lag",
                                               "And that's how you get to 60 FPS",
                                               "Together We Upscale",
                                               "For upscalers, by upscalers",
                                               "Opti Sports, it's in the sampling",
                                               "Render in your world. Upscale in ours",
                                               "All your pixels are belong to us",
                                               "Upscaling for the masses, not the classes",
                                               "Generating discord since 2023",
                                               "Enabling DLSS since 2023",
                                               "[REDACTED] never looked better",
                                               "Free and always free",
                                               "Getting unshackled from green chains in progress...",
                                               "Who's Nukem anyway?",
                                               "Compiling shaders... ETA: 05h:49m",
                                               "Did you really just pay 70 EUR for this game?!",
                                               "Guess who forgot about a nullptr check again",
                                               "AI can't outslop this",
                                               "Guess we're pre-alpha build demos now",
                                               "New app on the block - TH",
                                               "One more stutter and I might lose it",
                                               "Mostly stable, unlike the driver",
                                               "Vul... what? ~AMD",
                                               "My 8 points are floating",
                                               "No floating here - I'm strictly between -128 and 127",
                                               "Fake it til you bake it",
                                               "Worst case just turn it off and on",
                                               "*On a generative damage control mode at geometry level*",
                                               "Deep Learning Slop Sampling 5",
                                               "2D AI filters, now powered by just 2x 5090s",
                                               "Neural Slop Sampling with DLSS5",
                                               "DLSS 5 - the way it's meant to be slopped",
                                               "Just when I think I'm out, they scale me back in",
                                               "Like going in the first gear on the highway",
                                               "Nitec's Bizarre Upscaling",
                                               "\"Framegen really attracts some strange clientelle\"",
                                               "How to remove those corny messages?!",
                                               "<Your funny text goes here>" };

static std::string updateNoticeTag;
static std::string updateNoticeUrl;
static float lastMenuScale = 0.0f;
static CustomOptional<uint32_t> comboPreset { 0 };
static int lastKey = 0;
static bool inputDlssNr = false;
static bool inputHdrCompare = false;
static bool capturingKey = false;

template <typename T, size_t N> struct RingBuffer
{
    std::array<T, N> data {};
    size_t head { 0 };
    size_t count { N };
    double sum { 0.0 };

    RingBuffer() { data.fill(static_cast<T>(0)); }

    void Push(T v)
    {
        if (count == N)
        {
            sum -= data[head];
        }
        else
        {
            ++count;
        }
        data[head] = v;
        sum += v;
        head = (head + 1) % N;
    }

    size_t Size() const { return N; }

    T At(size_t i) const
    {
        size_t start = head;
        return data[(start + i) % N];
    }

    float Average() const { return static_cast<float>(sum / static_cast<double>(N)); }
};

const int plotWidth = 360;
static RingBuffer<float, plotWidth> gFrameTimes;
static RingBuffer<float, plotWidth> gUpscalerTimes;

struct FsExistsCache
{
    std::wstring lastPath;
    bool cached { false };
    std::chrono::steady_clock::time_point nextRefresh { std::chrono::steady_clock::time_point::min() };
    std::chrono::milliseconds interval { 2000 };

    bool Get(const std::filesystem::path& path)
    {
        auto now = std::chrono::steady_clock::now();
        if (path != lastPath || now >= nextRefresh)
        {
            lastPath = path;
            cached = std::filesystem::exists(path);
            nextRefresh = now + interval;
        }
        return cached;
    }
};

static FsExistsCache nukemsExists;
static FsExistsCache enablerExists;

struct FlagDefinition
{
    std::string name;
    uint32_t mask;
    std::string description;
};

inline std::string StrFmt(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int len = std::vsnprintf(nullptr, 0, fmt, args);
    va_end(args);
    std::string out(len, '\0');
    va_start(args, fmt);
    std::vsnprintf(out.data(), len + 1, fmt, args);
    va_end(args);
    return out;
}

void MenuCommon::UpdateManualInput(HWND targetHwnd)
{
    OptiInput::BeginFrame(targetHwnd);

    const auto config = Config::Instance();

    const auto currentTick = GetTickCount64();

    // A shortcut fires on key release. Under some input stacks (notably Assetto Corsa + CSP) the
    // same physical tap is observed by more than one acquisition path (WndProc hook, message-queue
    // hook, GetAsyncKeyState poll) on different frames, which produces a second spurious release
    // edge and makes the menu "close then immediately re-open". Require a fresh press edge between
    // fires, plus a short cooldown, so one tap can only toggle once.
    static bool shortcutArmed[256] = {};
    constexpr uint64_t shortcutCooldown = 250;

    auto CheckShortcut = [&](int vk, bool& inputFlag, const char* logMessage)
    {
        if (inputFlag)
            return;

        if (vk <= 0 || vk >= 256)
            return;

        if (OptiInput::IsKeyPressed(vk))
            shortcutArmed[vk] = true;

        if (!shortcutArmed[vk])
            return;

        if (currentTick - lastShortcutFireTick < shortcutCooldown)
            return;

        if (OptiInput::IsKeyReleased(vk))
        {
            shortcutArmed[vk] = false;
            lastShortcutFireTick = currentTick;
            lastKey = vk;
            // receivingWmInputs = false;
            inputFlag = true;
            LOG_DEBUG("{}", logMessage);
        }
    };

    const bool canAcceptInputs = lastInputTick + debounceThreshold < currentTick;

    if (!capturingKey && canAcceptInputs)
    {
        CheckShortcut(config->ShortcutKey.value_or_default(), inputMenu, "Menu key pressed, will be switching menu");
        CheckShortcut(config->FpsShortcutKey.value_or_default(), inputFps, "Menu key pressed, will be switching FPS");
        CheckShortcut(config->FGShortcutKey.value_or_default(), inputFG, "Menu key pressed, will be switching FG mode");
        CheckShortcut(config->FpsCycleShortcutKey.value_or_default(), inputFpsCycle,
                      "Menu key pressed, will be switching FPS mode");
        CheckShortcut(config->DlssNrToggleKey.value_or_default(), inputDlssNr,
                      "Neural Rendering key pressed, will be toggling the pass");
        CheckShortcut(config->HdrCompareKey.value_or_default(), inputHdrCompare,
                      "HDR compare key pressed, will be flipping Convert");
    }
    else if (capturingKey)
    {
        lastInputTick = currentTick;
    }

    lastKey = OptiInput::GetLastPressedKey();
}

// The FPS overlay's first Reflex timings line: the input latency, simulation start to GPU done (or to the end of
// Present when the report has no GPU marks). `estimated`: the report is made of the frame markers (a `~` before the
// number). Nothing while the timing data has no latency.
static void DrawInputLatencyLine(bool estimated)
{
    double ms = 0.0;
    bool reachesGpu = false;

    // No valid value (a loading screen, the first read): a placeholder, so the overlay does not jump
    if (!ReflexHooks::inputLatency(ms, reachesGpu))
    {
        ImGui::TextUnformatted("Input latency: --");
        return;
    }

    ImGui::Text(reachesGpu ? "Input latency: %s%.0f ms" : "Input latency, to Present: %s%.0f ms", estimated ? "~" : "",
                ms);
}

void MenuCommon::ShowTooltip(const char* tip)
{
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(tip);
        ImGui::EndTooltip();
    }
}

void MenuCommon::ShowHelpMarker(const char* tip)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    ShowTooltip(tip);
}

void MenuCommon::ShowResetButton(CustomOptional<bool, NoDefault>* initFlag, std::string buttonName)
{
    ImGui::SameLine();

    ImGui::BeginDisabled(!initFlag->has_value());

    if (ImGui::Button(buttonName.c_str()))
    {
        initFlag->reset();
        ReInitUpscaler();
    }

    ImGui::EndDisabled();
}

inline void MenuCommon::ReInitUpscaler()
{
    if (!State::Instance().currentFeature)
        return;

    if (State::Instance().currentFeature->GetUpscalerType() == Upscaler::DLSSD)
        State::Instance().newBackend = Upscaler::DLSSD;
    else
        State::Instance().newBackend = currentBackend;

    MARK_ALL_BACKENDS_CHANGED();
}

void MenuCommon::SeparatorWithHelpMarker(const char* label, const char* tip)
{
    auto marker = "(?) ";
    ImGui::SeparatorTextEx(0, label, ImGui::FindRenderedTextEnd(label),
                           ImGui::CalcTextSize(marker, ImGui::FindRenderedTextEnd(marker)).x);
    ShowHelpMarker(tip);
}

class Keybind
{
    std::string name;
    int id;
    bool waitingForKey = false;

  public:
    Keybind(std::string name, int id) : name(name), id(id) {}

    static std::string KeyNameFromVirtualKeyCode(USHORT virtualKey)
    {
        if (virtualKey == (USHORT) UnboundKey)
            return "Unbound";

        UINT scanCode = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);

        // Keys like Home would display as Num 0 without this fix
        switch (virtualKey)
        {
        case VK_INSERT:
        case VK_DELETE:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
        case VK_NUMLOCK:
        case VK_DIVIDE:
        case VK_RCONTROL:
        case VK_RMENU:
            scanCode |= 0xE000;
            break;
        }

        LONG lParam = (scanCode & 0xFF) << 16;
        if (scanCode & 0xE000)
            lParam |= 1 << 24;

        wchar_t buf[64] = {};
        if (GetKeyNameTextW(lParam, buf, static_cast<int>(std::size(buf))) != 0)
            return wstring_to_string(buf);

        return "Unknown";
    }

    void Render(CustomOptional<int>& configKey)
    {
        ImGui::PushID(id);
        if (ImGui::Button(name.c_str()))
        {
            waitingForKey = true;
            capturingKey = true;
            lastKey = 0;
        }
        ImGui::PopID();

        if (waitingForKey)
        {
            ImGui::SameLine();
            ImGui::Text("Press any key...");

            if (lastKey == 0 || lastKey == VK_LBUTTON || lastKey == VK_RBUTTON || lastKey == VK_MBUTTON)
                return;

            if (lastKey == VK_ESCAPE)
            {
                waitingForKey = false;
                capturingKey = false;
                return;
            }

            if (lastKey == VK_BACK)
                lastKey = UnboundKey;

            configKey = lastKey;
            waitingForKey = false;
            capturingKey = false;
            return;
        }

        ImGui::SameLine();
        ImGui::Text(KeyNameFromVirtualKeyCode(configKey.value_or_default()).c_str());

        ImGui::SameLine();
        ImGui::PushID(id);
        if (ImGui::Button("R"))
        {
            configKey.reset();
        }
        ImGui::PopID();
    }
};

Upscaler MenuCommon::GetBackendCode(const API api)
{
    if (auto feature = State::Instance().currentFeature)
        return feature->GetUpscalerType();

    Upscaler upscaler;

    if (api == DX11)
        upscaler = Config::Instance()->Dx11Upscaler.value_or_default();
    else if (api == DX12)
        upscaler = Config::Instance()->Dx12Upscaler.value_or_default();
    else
        upscaler = Config::Instance()->VulkanUpscaler.value_or_default();

    return upscaler;
}

void MenuCommon::GetCurrentBackendInfo(const API api, Upscaler& upscaler, std::string* name)
{
    upscaler = GetBackendCode(api);
    *name = UpscalerDisplayName(upscaler, api);
}

void MenuCommon::RenderUpscalerCombo(const API api, Upscaler currentUpscaler, const std::vector<Upscaler>& options)
{
    auto primaryGpu = IdentifyGpu::getPrimaryGpu();

    // Determine display name
    Upscaler targetBackend = State::Instance().newBackend;
    if (targetBackend == Upscaler::Reset)
        targetBackend = currentUpscaler;

    std::string selectedName = UpscalerDisplayName(targetBackend, api);

    if (ImGui::BeginCombo("##UpscalerCombo", selectedName.c_str()))
    {
        for (auto opt : options)
        {
            // Check if GPU is capable of a given backend
            if (opt == Upscaler::DLSS && !primaryGpu.dlssCapable)
                continue;

            // DLSS on D3D12 for a Vulkan game needs the GPU and the DLSS library just the same
            if (opt == Upscaler::DLSS_on12 && api == API::Vulkan &&
                (!primaryGpu.dlssCapable || !State::Instance().NVNGX_DLSS_Path.has_value()))
                continue;

            // Not all Intel GPUs support native DX11 XeSS but don't think we have a good way to check exactly
            if (opt == Upscaler::XeSS && api == API::DX11 && primaryGpu.vendorId != VendorId::Intel)
                continue;

            bool isSelected = (currentUpscaler == opt);
            if (ImGui::Selectable(UpscalerDisplayName(opt, api).c_str(), isSelected))
            {
                State::Instance().newBackend = opt;
            }
        }
        ImGui::EndCombo();
    }
}

void MenuCommon::AddDx11Backends(Upscaler upscaler)
{
    RenderUpscalerCombo(API::DX11, upscaler,
                        { Upscaler::XeSS, Upscaler::FSR22, Upscaler::FSR31, Upscaler::XeSS_on12, Upscaler::FSR21_on12,
                          Upscaler::FSR22_on12, Upscaler::FFX_on12, Upscaler::DLSS, Upscaler::DLSS_on12 });
}

void MenuCommon::AddDx12Backends(Upscaler upscaler)
{
    RenderUpscalerCombo(API::DX12, upscaler,
                        { Upscaler::XeSS, Upscaler::FSR21, Upscaler::FSR22, Upscaler::FFX, Upscaler::DLSS });
}

void MenuCommon::AddVulkanBackends(Upscaler upscaler)
{
    // The Vulkan route "Bridge, game's upscaler" feeds frame generation from a Vulkan-on-D3D12 backend only: the others
    // would leave it without inputs (upscalers/FeatureProvider_Vk.h)
    if (Config::Instance()->DlssNrNativeFrameGenerationOnly.value_or_default())
    {
        RenderUpscalerCombo(API::Vulkan, upscaler, { Upscaler::DLSS_on12, Upscaler::FFX_on12, Upscaler::FSR21_on12 });
        ShowHelpMarker(
            "Vulkan route \"Bridge, game's upscaler\" is on: frame generation runs from the upscaler's D3D12 "
            "copy, so only those are listed. Choose Native swapchain (Frame Generation page) to pick another.");
        return;
    }

    RenderUpscalerCombo(API::Vulkan, upscaler,
                        { Upscaler::XeSS, Upscaler::FSR21, Upscaler::FSR22, Upscaler::FFX, Upscaler::FSR21_on12,
                          Upscaler::FFX_on12, Upscaler::DLSS });
}

template <HasDefaultValue B> void MenuCommon::AddResourceBarrier(std::string name, CustomOptional<int32_t, B>* value)
{
    const char* states[] = { "AUTO",
                             "COMMON",
                             "VERTEX_AND_CONSTANT_BUFFER",
                             "INDEX_BUFFER",
                             "RENDER_TARGET",
                             "UNORDERED_ACCESS",
                             "DEPTH_WRITE",
                             "DEPTH_READ",
                             "NON_PIXEL_SHADER_RESOURCE",
                             "PIXEL_SHADER_RESOURCE",
                             "STREAM_OUT",
                             "INDIRECT_ARGUMENT",
                             "COPY_DEST",
                             "COPY_SOURCE",
                             "RESOLVE_DEST",
                             "RESOLVE_SOURCE",
                             "RAYTRACING_ACCELERATION_STRUCTURE",
                             "SHADING_RATE_SOURCE",
                             "GENERIC_READ",
                             "ALL_SHADER_RESOURCE",
                             "PRESENT",
                             "PREDICATION",
                             "VIDEO_DECODE_READ",
                             "VIDEO_DECODE_WRITE",
                             "VIDEO_PROCESS_READ",
                             "VIDEO_PROCESS_WRITE",
                             "VIDEO_ENCODE_READ",
                             "VIDEO_ENCODE_WRITE" };
    const int values[] = { -1,  0,   1,     2,      4,      8,      16,      32,       64,   128,
                           256, 512, 1024,  2048,   4096,   8192,   4194304, 16777216, 2755, 192,
                           0,   310, 65536, 131072, 262144, 524288, 2097152, 8388608 };

    int selected = value->value_or(-1);

    const char* selectedName = "";

    for (int n = 0; n < 28; n++)
    {
        if (values[n] == selected)
        {
            selectedName = states[n];
            break;
        }
    }

    if (ImGui::BeginCombo(name.c_str(), selectedName))
    {
        if (ImGui::Selectable(states[0], !value->has_value()))
            value->reset();

        for (int n = 1; n < 28; n++)
        {
            if (ImGui::Selectable(states[n], selected == values[n]))
                *value = values[n];
        }

        ImGui::EndCombo();
    }
}

static uint32_t GetPresetIndex(IFeature* feature, bool dlssd = false)
{
    auto ratio = (float) feature->TargetWidth() / (float) feature->RenderWidth();

    if (!dlssd)
    {
        if (State::Instance().dlssPresetsOverridenByOpti)
        {
            LOG_DEBUG("DLSS Presets overridden by Opti, using Opti preset indices with ratio: {}", ratio);

            if (ratio <= (Config::Instance()->QualityRatio_UltraPerformance.value_or_default() + 0.01f))
            {
                return Config::Instance()->RenderPresetForAll.value_or(
                    Config::Instance()->RenderPresetUltraPerformance.value_or_default());
            }
            else if (ratio <= (Config::Instance()->QualityRatio_Performance.value_or_default() + 0.01f))
            {
                return Config::Instance()->RenderPresetForAll.value_or(
                    Config::Instance()->RenderPresetPerformance.value_or_default());
            }
            else if (ratio <= (Config::Instance()->QualityRatio_Balanced.value_or_default() + 0.01f))
            {
                return Config::Instance()->RenderPresetForAll.value_or(
                    Config::Instance()->RenderPresetBalanced.value_or_default());
            }
            else if (ratio <= (Config::Instance()->QualityRatio_Quality.value_or_default() + 0.01f))
            {
                return Config::Instance()->RenderPresetForAll.value_or(
                    Config::Instance()->RenderPresetQuality.value_or_default());
            }
            else if (ratio <= (Config::Instance()->QualityRatio_UltraQuality.value_or_default() + 0.01f))
            {
                return Config::Instance()->RenderPresetForAll.value_or(
                    Config::Instance()->RenderPresetUltraQuality.value_or_default());
            }
            else
            {
                return Config::Instance()->RenderPresetForAll.value_or(
                    Config::Instance()->RenderPresetDLAA.value_or_default());
            }
        }
        else if (State::Instance().dlssPresetsOverriddenExternally)
        {
            LOG_DEBUG("DLSS Presets overridden externally, using external preset index: {}",
                      State::Instance().dlssRenderPresetExternal);

            return State::Instance().dlssRenderPresetExternal;
        }
        else
        {
            if (ratio <= (Config::Instance()->QualityRatio_UltraPerformance.value_or_default() + 0.01f))
            {
                return State::Instance().dlssRenderPresetUltraPerformance;
            }
            else if (ratio <= (Config::Instance()->QualityRatio_Performance.value_or_default() + 0.01f))
            {
                return State::Instance().dlssRenderPresetPerformance;
            }
            else if (ratio <= (Config::Instance()->QualityRatio_Balanced.value_or_default() + 0.01f))
            {
                return State::Instance().dlssRenderPresetBalanced;
            }
            else if (ratio <= (Config::Instance()->QualityRatio_Quality.value_or_default() + 0.01f))
            {
                return State::Instance().dlssRenderPresetQuality;
            }
            else if (ratio <= (Config::Instance()->QualityRatio_UltraQuality.value_or_default() + 0.01f))
            {
                return State::Instance().dlssRenderPresetUltraQuality;
            }
            else
            {
                return State::Instance().dlssRenderPresetDLAA;
            }
        }
    }
    else
    {
        if (State::Instance().dlssdPresetsOverridenByOpti)
        {
            if (ratio <= (Config::Instance()->QualityRatio_UltraPerformance.value_or_default() + 0.01f))
            {
                return Config::Instance()->DLSSDRenderPresetForAll.value_or(
                    Config::Instance()->DLSSDRenderPresetUltraPerformance.value_or_default());
            }
            else if (ratio <= (Config::Instance()->QualityRatio_Performance.value_or_default() + 0.01f))
            {
                return Config::Instance()->DLSSDRenderPresetForAll.value_or(
                    Config::Instance()->DLSSDRenderPresetPerformance.value_or_default());
            }
            else if (ratio <= (Config::Instance()->QualityRatio_Balanced.value_or_default() + 0.01f))
            {
                return Config::Instance()->DLSSDRenderPresetForAll.value_or(
                    Config::Instance()->DLSSDRenderPresetBalanced.value_or_default());
            }
            else if (ratio <= (Config::Instance()->QualityRatio_Quality.value_or_default() + 0.01f))
            {
                return Config::Instance()->DLSSDRenderPresetForAll.value_or(
                    Config::Instance()->DLSSDRenderPresetQuality.value_or_default());
            }
            else if (ratio <= (Config::Instance()->QualityRatio_UltraQuality.value_or_default() + 0.01f))
            {
                return Config::Instance()->DLSSDRenderPresetForAll.value_or(
                    Config::Instance()->DLSSDRenderPresetUltraQuality.value_or_default());
            }
            else
            {
                return Config::Instance()->DLSSDRenderPresetForAll.value_or(
                    Config::Instance()->DLSSDRenderPresetDLAA.value_or_default());
            }
        }
        else if (State::Instance().dlssdPresetsOverriddenExternally)
        {
            return State::Instance().dlssdRenderPresetExternal;
        }
        else
        {
            if (ratio <= (Config::Instance()->QualityRatio_UltraPerformance.value_or_default() + 0.01f))
            {
                return State::Instance().dlssdRenderPresetUltraPerformance;
            }
            else if (ratio <= (Config::Instance()->QualityRatio_Performance.value_or_default() + 0.01f))
            {
                return State::Instance().dlssdRenderPresetPerformance;
            }
            else if (ratio <= (Config::Instance()->QualityRatio_Balanced.value_or_default() + 0.01f))
            {
                return State::Instance().dlssdRenderPresetBalanced;
            }
            else if (ratio <= (Config::Instance()->QualityRatio_Quality.value_or_default() + 0.01f))
            {
                return State::Instance().dlssdRenderPresetQuality;
            }
            else if (ratio <= (Config::Instance()->QualityRatio_UltraQuality.value_or_default() + 0.01f))
            {
                return State::Instance().dlssdRenderPresetUltraQuality;
            }
            else
            {
                return State::Instance().dlssdRenderPresetDLAA;
            }
        }
    }

    return 0;
}

// TODO: disable presets based on the detected DLSS version
template <HasDefaultValue B> void MenuCommon::AddDLSSRenderPreset(std::string name, CustomOptional<uint32_t, B>* value)
{
    // clang-format off
    static const std::vector<MenuOption<uint32_t>> presets = {
        { NVSDK_NGX_DLSS_Hint_Render_Preset_Default, "DEFAULT", 
            "Whatever the game uses" },
        { NVSDK_NGX_DLSS_Hint_Render_Preset_A, "PRESET A",
            "Intended for Performance/Balanced/Quality modes.\nAn older variant best suited to combat ghosting...\nRemoved on recent versions!" },
        { NVSDK_NGX_DLSS_Hint_Render_Preset_B, "PRESET B",
            "Intended for Ultra Performance mode.\nSimilar to Preset A...\nRemoved on recent versions!" },
        { NVSDK_NGX_DLSS_Hint_Render_Preset_C, "PRESET C",
            "Intended for Performance/Balanced/Quality modes.\nGenerally favors current frame information...\nRemoved on recent versions!" },
        { NVSDK_NGX_DLSS_Hint_Render_Preset_D, "PRESET D",
            "Default preset for Performance/Balanced/Quality modes;\ngenerally favors image stability.\nRemoved on recent versions!" },
        { NVSDK_NGX_DLSS_Hint_Render_Preset_E, "PRESET E",
            "DLSS 3.7+, a better D preset\nRemoved on recent versions!" },
        { NVSDK_NGX_DLSS_Hint_Render_Preset_F, "PRESET F",
            "Default preset for Ultra Performance and DLAA modes\nRemoved on recent versions!" },
        { NVSDK_NGX_DLSS_Hint_Render_Preset_G, "PRESET G",
            "Unused" },
        { NVSDK_NGX_DLSS_Hint_Render_Preset_H_Reserved, "PRESET H",
            "Unused" },
        { NVSDK_NGX_DLSS_Hint_Render_Preset_I_Reserved, "PRESET I",
            "Unused" },
        { NVSDK_NGX_DLSS_Hint_Render_Preset_J, "PRESET J",
            "Similar to preset K. Preset J might exhibit slightly\nless ghosting...\n1st Gen Transformer" },
        { NVSDK_NGX_DLSS_Hint_Render_Preset_K, "PRESET K",
            "Default preset for DLAA/Balanced/Quality modes...\n1st Gen Transformer" },
        { NVSDK_NGX_DLSS_Hint_Render_Preset_L, "PRESET L",
            "Default for Ultra Perf mode\n2nd Gen Transformers" },
        { NVSDK_NGX_DLSS_Hint_Render_Preset_M, "PRESET M",
            "Default for Perf mode\n2nd Gen Transformer" },
        { NVSDK_NGX_DLSS_Hint_Render_Preset_N, "PRESET N",
            "Unused" },
        { NVSDK_NGX_DLSS_Hint_Render_Preset_O, "PRESET O",
            "Unused" },
        { NV_PRESET_LATEST, "Latest",
            "Latest supported by the dll" }
    };
    // clang-format on

    PopulateCombo(name, *value, presets);
}

template <HasDefaultValue B> void MenuCommon::AddDLSSDRenderPreset(std::string name, CustomOptional<uint32_t, B>* value)
{
    // We don't have DLSSD definitions so using raw values
    static const std::vector<MenuOption<uint32_t>> presets = {
        { 0, "DEFAULT", "Whatever the game uses" },
        { 1, "PRESET A", "Preset A\nRemoved on recent versions!" },
        { 2, "PRESET B", "Preset B\nRemoved on recent versions!" },
        { 3, "PRESET C", "Preset C\nRemoved on recent versions!" },
        { 4, "PRESET D", "Default model, Transformer" },
        { 5, "PRESET E", "Latest Transformer model\nMust use if DoF guide is needed" },
        { 6, "PRESET F", "Latest Transformer model\nMust use if DoF guide is needed" },
        { NV_PRESET_LATEST, "Latest", "Latest supported by the dll" }
    };

    PopulateCombo(name, *value, presets);
}

template <typename TStorage, typename T>
void MenuCommon::PopulateCombo(const std::string& name, TStorage& currentValue,
                               const std::vector<MenuOption<T>>& options)
{
    if (options.empty())
        return;

    // Assumes that different types mean that TStorage is std::optional
    T currentVal;
    if constexpr (std::is_same_v<TStorage, T>)
        currentVal = currentValue;
    else
        currentVal = currentValue.value_or(options[0].value);

    // Find the label for the currently selected item
    std::string preview = "Unknown";
    for (const auto& opt : options)
    {
        if (opt.value == currentVal)
        {
            preview = opt.label;
            break;
        }
    }

    if (ImGui::BeginCombo(name.c_str(), preview.c_str()))
    {
        for (const auto& opt : options)
        {
            if (opt.hidden)
                continue;

            if (opt.disabled)
                ImGui::BeginDisabled();

            bool isSelected = (currentVal == opt.value);
            if (ImGui::Selectable(opt.label.c_str(), isSelected))
                currentValue = opt.value;

            // Show tooltip for the individual item if it exists
            if (!opt.tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", opt.tooltip.c_str());

            if (opt.disabled)
                ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
}

static UiTargetMode getUiTargetMode()
{
    const auto& state = State::Instance();

    const bool fallback = !Config::Instance()->OverlayMenu.value_or_default();

    if (fallback)
    {
        // We have no reliable swapchain/output color-space information here.
        // Only classify the upscaled working image.
        if (state.currentFeature && state.currentFeature->IsHdr())
            return UiTargetMode::LinearHDR;

        return UiTargetMode::SDR;
    }

    const auto& output = state.outputColorSpace;

    // If SetColorSpace1 has not provided a known/valid color space,
    // fall back conservatively.
    if (!output.valid)
        return UiTargetMode::SDR;

    switch (output.transfer)
    {
    case ColorTransfer::Linear:
        // scRGB: linear Rec.709 RGB.
        //
        // hdrOutputActive is intentionally NOT required here.
        // A scRGB swapchain is still linear even when the physical output
        // is currently SDR. hdrOutputActive only affects the desired
        // reference-white scaling in toneMapColor().
        if (output.model == ColorModel::RGB && output.primaries == ColorPrimaries::Rec709)
        {
            return UiTargetMode::ScRGB;
        }

        break;

    case ColorTransfer::PQ:
        // Direct PQ UI rendering currently assumes RGB PQ / Rec.2020.
        //
        // Do not treat YCbCr PQ as an RGB render target.
        if (output.model == ColorModel::RGB && output.primaries == ColorPrimaries::Rec2020)
        {
            return UiTargetMode::PQ;
        }

        break;

    case ColorTransfer::HLG:
        // Current HLG UI path assumes an RGB render target.
        //
        // DXGI HLG modes are commonly YCbCr, so reject unsupported
        // combinations rather than applying an RGB HLG transform blindly.
        if (output.model == ColorModel::RGB && output.primaries == ColorPrimaries::Rec2020)
        {
            return UiTargetMode::HLG;
        }

        break;

    case ColorTransfer::SRGB:
        return UiTargetMode::SDR;

    case ColorTransfer::Unknown:
    default:
        break;
    }

    // Unsupported model / primaries / transfer combination.
    return UiTargetMode::SDR;
}

static float srgbToLinear(float x)
{
    x = std::clamp(x, 0.0f, 1.0f);

    if (x <= 0.04045f)
        return x / 12.92f;

    return std::pow((x + 0.055f) / 1.055f, 2.4f);
}

static float linearToPQ(float nits)
{
    // SMPTE ST.2084
    constexpr float m1 = 2610.0f / 16384.0f;
    constexpr float m2 = 2523.0f / 32.0f;
    constexpr float c1 = 3424.0f / 4096.0f;
    constexpr float c2 = 2413.0f / 128.0f;
    constexpr float c3 = 2392.0f / 128.0f;

    const float y = std::clamp(nits / 10000.0f, 0.0f, 1.0f);
    const float ym1 = std::pow(y, m1);

    return std::pow((c1 + c2 * ym1) / (1.0f + c3 * ym1), m2);
}

static float linearToHLG(float x)
{
    // BT.2100 HLG OETF
    constexpr float a = 0.17883277f;
    constexpr float b = 0.28466892f;
    constexpr float c = 0.55991073f;

    x = std::max(x, 0.0f);

    if (x <= (1.0f / 12.0f))
        return std::sqrt(3.0f * x);

    return a * std::log(12.0f * x - b) + c;
}

static ImVec4 linear709To2020(float r, float g, float b)
{
    return ImVec4(0.6274040f * r + 0.3292820f * g + 0.0433136f * b, 0.0690970f * r + 0.9195400f * g + 0.0113612f * b,
                  0.0163916f * r + 0.0880132f * g + 0.8955950f * b, 0.0f);
}

static ImVec4 legacyHdrToneMap(const ImVec4& color)
{
    constexpr float exposure = 1.0f;
    constexpr float strength = 1.0f;

    const float peak = std::max(color.x, std::max(color.y, color.z));

    if (peak <= 0.0f)
        return color;

    const float exposedPeak = peak * exposure;
    const float mappedPeak = exposedPeak / (1.0f + exposedPeak);

    const float reinhardScale = mappedPeak / peak;
    const float scale = 1.0f + (reinhardScale - 1.0f) * strength;

    return ImVec4(color.x * scale, color.y * scale, color.z * scale, color.w);
}

static ImVec4 toneMapColor(const ImVec4& color)
{
    const auto mode = getUiTargetMode();

    switch (mode)
    {
    case UiTargetMode::SDR:
        return color;

    case UiTargetMode::LinearHDR:
        return ImVec4(srgbToLinear(color.x), srgbToLinear(color.y), srgbToLinear(color.z), color.w);

    case UiTargetMode::ScRGB:
    {
        constexpr float scRgbReferenceWhiteNits = 80.0f;
        constexpr float hdrUiWhiteNits = 203.0f;

        const float uiWhiteNits = State::Instance().hdrOutputActive ? hdrUiWhiteNits : scRgbReferenceWhiteNits;

        const float scale = uiWhiteNits / scRgbReferenceWhiteNits;

        return ImVec4(srgbToLinear(color.x) * scale, srgbToLinear(color.y) * scale, srgbToLinear(color.z) * scale,
                      color.w);
    }

    case UiTargetMode::PQ:
        // Direct ImGui rendering into a nonlinear PQ target.
        //
        // Proper PQ encoding of vertex colors produces incorrect results
        // with the standard ImGui alpha blend state because blending then
        // happens in PQ space.
        //
        // Keep the known-good legacy compression until PQ rendering is
        // moved to a linear intermediate/composite pass.
        return legacyHdrToneMap(color);

    case UiTargetMode::HLG:
        // Same fundamental nonlinear-blending problem as PQ.
        // Conservative compatibility behavior for now.
        return legacyHdrToneMap(color);

    default:
        return color;
    }
}

static void MenuHdrCheck(ImGuiIO io)
{
    if (!_hdrTonemapApplied)
    {
        ImGuiStyle& style = ImGui::GetStyle();
        const auto mode = getUiTargetMode();

        LOG_INFO("Output HDR: {}, UI Mode: {}", State::Instance().hdrOutputActive, magic_enum::enum_name(mode));

        CopyMemory(SdrColors, style.Colors, sizeof(style.Colors));

        // Apply tone mapping to the ImGui style
        for (int i = 0; i < ImGuiCol_COUNT; ++i)
        {
            ImVec4 color = style.Colors[i];
            style.Colors[i] = toneMapColor(color);
        }

        _hdrTonemapApplied = true;
    }
}

static float MenuResolutionScale(ImGuiIO io)
{
    if (Config::Instance()->MenuScale.has_value())
        return Config::Instance()->MenuScale.value();

    // Calculate menu scale according to display resolution
    float y = State::Instance().screenHeight;

    if (io.DisplaySize.y != 0)
        y = (float) io.DisplaySize.y;

    // 1000p is minimum for 1.0 menu ratio
    float result = (float) ((int) (y / 108.0f)) / 10.0f;

    result = std::round(result * 10.0f) / 10.0f;

    if (result < 0.5f)
        result = 0.5f;

    if (result > 2.0f)
        result = 2.0f;

    return result;
}

inline static std::string GetSourceString(UINT source)
{
    switch (source)
    {
    case 1:
        return "RTV";
    case 2:
        return "SRV";
    case 4:
        return "UAV";
    case 8:
        return "OM";
    case 16:
        return "Ups";
    case 32:
        return "SCR";
    case 64:
        return "SGR";
    default:
        return std::format("{}", source);
    }
}

inline static std::string GetDispatchString(UINT source)
{
    switch (source)
    {
    case 512:
        return "DI";
    case 1024:
        return "DII";
    case 256:
        return "Disp";
    default:
        return std::format("{}", source);
    }
}

void MenuCommon::ApplyThemeStyle()
{
    if (ImGui::GetCurrentContext() == nullptr)
        return;

    ImGuiStyle& style = ImGui::GetStyle();

    auto conf = Config::Instance();
    bool lightTheme = conf->LightTheme.value_or_default();

    const bool modern = conf->ModernTheme.value_or_default();
    gModernActive = modern;

    // Base (unscaled) values; RebuildStyle scales them with Menu Scale.
    if (modern)
    {
        style.WindowRounding = 8.0f;
        style.ChildRounding = 6.0f;
        style.FrameRounding = 5.0f;
        style.PopupRounding = 6.0f;
        style.ScrollbarRounding = 6.0f;
        style.GrabRounding = 4.0f;
        style.TabRounding = 5.0f;

        style.WindowPadding = ImVec2(12.0f, 10.0f);
        style.FramePadding = ImVec2(8.0f, 5.0f);
        style.ItemSpacing = ImVec2(8.0f, 6.0f);
        style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);

        style.ChildBorderSize = 0.0f;
        style.FrameBorderSize = 0.0f;
        style.TabBorderSize = 0.0f;
        style.ScrollbarSize = 8.0f;
    }
    else
    {
        style.WindowRounding = 2.0f;
        style.ChildRounding = 1.0f;
        style.FrameRounding = 2.0f;
        style.PopupRounding = 2.0f;
        style.ScrollbarRounding = 2.0f;
        style.GrabRounding = 2.0f;
        style.TabRounding = 2.0f;

        style.WindowPadding = ImVec2(8.0f, 8.0f);
        style.FramePadding = ImVec2(4.0f, 3.0f);
        style.ItemSpacing = ImVec2(8.0f, 4.0f);
        style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);

        style.ChildBorderSize = 1.0f;
        style.FrameBorderSize = lightTheme ? 1.0f : 0.0f;
        style.TabBorderSize = lightTheme ? 1.0f : 0.0f;
        style.ScrollbarSize = 10.0f;
    }

    style.WindowBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.GrabMinSize = 10.0f;

    auto Clamp01 = [](float v) { return std::max(0.0f, std::min(v, 1.0f)); };

    auto Mix = [](const ImVec4& a, const ImVec4& b, float t, float alpha = 1.0f)
    { return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, alpha); };

    auto Luminance = [](const ImVec4& c) { return c.x * 0.2126f + c.y * 0.7152f + c.z * 0.0722f; };

    auto Saturate = [&](const ImVec4& color, float amount)
    {
        float lum = Luminance(color);

        return ImVec4(Clamp01(lum + (color.x - lum) * amount), Clamp01(lum + (color.y - lum) * amount),
                      Clamp01(lum + (color.z - lum) * amount), color.w);
    };

    ImVec4 accent = ImVec4(conf->MenuAccentColorR.value_or_default(), conf->MenuAccentColorG.value_or_default(),
                           conf->MenuAccentColorB.value_or_default(), 1.0f);

    ImVec4 bgAccent = ImVec4(conf->MenuBGColorR.value_or_default(), conf->MenuBGColorG.value_or_default(),
                             conf->MenuBGColorB.value_or_default(), 1.0f);

    float luminance = Luminance(accent);

    const ImVec4 bgDark = lightTheme ? ImVec4(0.80f, 0.82f, 0.86f, 1.00f) : ImVec4(0.09f, 0.09f, 0.10f, 1.00f);
    const ImVec4 bgMid = lightTheme ? ImVec4(0.89f, 0.91f, 0.95f, 1.00f) : ImVec4(0.11f, 0.11f, 0.12f, 1.00f);
    const ImVec4 bgLight = lightTheme ? ImVec4(0.96f, 0.97f, 0.99f, 1.00f) : ImVec4(0.14f, 0.14f, 0.15f, 1.00f);

    const ImVec4 textPrimary = lightTheme ? ImVec4(0.05f, 0.06f, 0.08f, 1.00f) : ImVec4(0.90f, 0.93f, 0.95f, 1.00f);
    const ImVec4 textDim = lightTheme ? ImVec4(0.22f, 0.25f, 0.31f, 1.00f) : ImVec4(0.54f, 0.58f, 0.62f, 1.00f);

    const ImVec4 borderCol = lightTheme ? ImVec4(0.35f, 0.40f, 0.50f, 1.00f) : ImVec4(0.24f, 0.24f, 0.26f, 1.00f);
    const ImVec4 dimBg = lightTheme ? ImVec4(0.30f, 0.33f, 0.38f, 0.20f) : ImVec4(0.09f, 0.10f, 0.13f, 0.20f);
    const ImVec4 modalDimBg = lightTheme ? ImVec4(0.22f, 0.24f, 0.28f, 0.55f) : ImVec4(0.04f, 0.04f, 0.07f, 0.55f);

    // MenuBGColor: only background/surface tint.
    auto BgTint = [&](const ImVec4& base, float strength = 1.0f, float alpha = 1.0f)
    {
        float t = lightTheme ? (0.180f * strength) : (0.120f * strength);
        return Mix(base, bgAccent, t, alpha);
    };

    // MenuAccentColor: all visible interactive accent colors.
    auto AccentSoft = [&](float alpha = 1.0f)
    { return lightTheme ? Mix(bgLight, accent, 0.14f, alpha) : Mix(bgDark, accent, 0.32f, alpha); };

    auto AccentMed = [&](float alpha = 1.0f)
    { return lightTheme ? Mix(bgLight, accent, 0.42f, alpha) : Mix(bgDark, accent, 0.55f, alpha); };

    auto AccentStrong = [&](float alpha = 1.0f) { return ImVec4(accent.x, accent.y, accent.z, alpha); };

    const ImVec4 bgTitle = AccentSoft();

    auto SurfaceHover = [&](float alpha = 1.0f)
    { return lightTheme ? Mix(bgLight, accent, 0.12f, alpha) : Mix(bgLight, accent, 0.18f, alpha); };

    auto SurfaceActive = [&](float alpha = 1.0f)
    { return lightTheme ? Mix(bgLight, accent, 0.20f, alpha) : Mix(bgLight, accent, 0.28f, alpha); };

    auto TitleActive = [&](float alpha = 1.0f)
    { return lightTheme ? Mix(bgTitle, accent, 0.18f, alpha) : Mix(bgTitle, accent, 0.16f, alpha); };

    auto PlotAccent = [&](float alpha = 1.0f)
    {
        if (lightTheme)
        {
            // Darken slightly for contrast on light bg — no channel floors
            return Mix(accent, ImVec4(0.00f, 0.00f, 0.00f, 1.00f), 0.20f, alpha);
        }

        // Brighten slightly for visibility on dark bg — no channel floors
        return Mix(accent, ImVec4(1.00f, 1.00f, 1.00f, 1.00f), 0.35f, alpha);
    };

    auto PlotAccentHovered = [&](float alpha = 1.0f)
    {
        if (lightTheme)
        {
            return Mix(PlotAccent(alpha), ImVec4(0.00f, 0.00f, 0.00f, 1.00f), 0.15f, alpha);
        }

        return Mix(PlotAccent(alpha), ImVec4(1.00f, 1.00f, 1.00f, 1.00f), 0.25f, alpha);
    };

    auto AccentReadable = [&](float alpha = 1.0f)
    {
        // Apply saturation boost and luminance correction only here,
        // so AccentStrong / AccentMed / AccentSoft stay true to the user's pick.
        ImVec4 a = Saturate(accent, lightTheme ? 1.35f : 1.25f);
        float lum = Luminance(a);

        if (lightTheme && lum > 0.72f)
            a = Mix(a, ImVec4(0.0f, 0.0f, 0.0f, 1.0f), 0.35f, 1.0f);

        if (!lightTheme && lum < 0.25f)
            a = Mix(a, ImVec4(1.0f, 1.0f, 1.0f, 1.0f), 0.30f, 1.0f);

        return ImVec4(a.x, a.y, a.z, alpha);
    };

    ImVec4* c = ImGui::GetStyle().Colors;

    float minAlpha = Config::Instance()->MenuBGColorA.value_or_default() >= 0.5f
                         ? Config::Instance()->MenuBGColorA.value_or_default()
                         : 0.5f;

    c[ImGuiCol_Text] = textPrimary;
    c[ImGuiCol_TextDisabled] = textDim;
    c[ImGuiCol_TextLink] = AccentReadable();

    // MenuBGColor only.
    c[ImGuiCol_WindowBg] = BgTint(bgDark, 1.00f, Config::Instance()->MenuBGColorA.value_or_default());
    c[ImGuiCol_ChildBg] = BgTint(bgMid, 1.10f, minAlpha + 0.1f);
    c[ImGuiCol_PopupBg] =
        lightTheme ? BgTint(bgLight, 0.90f) : BgTint(ImVec4(0.09f, 0.10f, 0.13f, 0.97f), 0.90f, 0.97f);
    c[ImGuiCol_MenuBarBg] = BgTint(bgDark, 0.85f);
    c[ImGuiCol_DockingEmptyBg] = BgTint(bgDark, 0.75f);

    c[ImGuiCol_Border] = borderCol;
    c[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    // Neutral background, not MenuBGColor.
    c[ImGuiCol_FrameBg] = BgTint(bgLight, 0.50f, minAlpha + 0.15f);
    c[ImGuiCol_FrameBgHovered] = SurfaceHover();
    c[ImGuiCol_FrameBgActive] = SurfaceActive();

    c[ImGuiCol_TitleBg] = BgTint(bgTitle, 0.40f);
    c[ImGuiCol_TitleBgActive] = TitleActive();
    c[ImGuiCol_TitleBgCollapsed] = ImVec4(bgTitle.x, bgTitle.y, bgTitle.z, 0.75f);

    c[ImGuiCol_ScrollbarBg] = BgTint(bgDark, 0.60f, minAlpha + 0.2f);
    c[ImGuiCol_ScrollbarGrab] = AccentSoft();
    c[ImGuiCol_ScrollbarGrabHovered] = AccentMed();
    c[ImGuiCol_ScrollbarGrabActive] = AccentStrong();

    c[ImGuiCol_CheckMark] = AccentReadable();
    c[ImGuiCol_SliderGrab] = AccentMed();
    c[ImGuiCol_SliderGrabActive] = AccentReadable();
    c[ImGuiCol_InputTextCursor] = AccentReadable();

    c[ImGuiCol_Button] = AccentSoft();
    c[ImGuiCol_ButtonHovered] = AccentMed();
    c[ImGuiCol_ButtonActive] = AccentStrong();

    c[ImGuiCol_Header] = AccentSoft(0.90f);
    c[ImGuiCol_HeaderHovered] = AccentMed(0.95f);
    c[ImGuiCol_HeaderActive] = AccentStrong();

    c[ImGuiCol_Separator] = borderCol;
    c[ImGuiCol_SeparatorHovered] = AccentMed(0.85f);
    c[ImGuiCol_SeparatorActive] = AccentStrong();

    c[ImGuiCol_ResizeGrip] = AccentSoft(0.30f);
    c[ImGuiCol_ResizeGripHovered] = AccentStrong(0.70f);
    c[ImGuiCol_ResizeGripActive] = AccentStrong(0.95f);

    c[ImGuiCol_Tab] = AccentSoft();
    c[ImGuiCol_TabHovered] = AccentMed();
    c[ImGuiCol_TabSelected] = AccentSoft();
    c[ImGuiCol_TabSelectedOverline] = AccentStrong();
    c[ImGuiCol_TabDimmed] = BgTint(bgDark, 0.60f);
    c[ImGuiCol_TabDimmedSelected] = AccentSoft(0.75f);
    c[ImGuiCol_TabDimmedSelectedOverline] = borderCol;

    c[ImGuiCol_DockingPreview] = AccentStrong(0.70f);

    c[ImGuiCol_PlotLines] = PlotAccent();
    c[ImGuiCol_PlotLinesHovered] = PlotAccentHovered();
    c[ImGuiCol_PlotHistogram] = PlotAccent(0.85f);
    c[ImGuiCol_PlotHistogramHovered] = PlotAccentHovered();

    c[ImGuiCol_TableHeaderBg] = BgTint(bgMid, 0.80f, minAlpha + 0.25f);
    c[ImGuiCol_TableBorderStrong] = borderCol;
    c[ImGuiCol_TableBorderLight] = lightTheme ? ImVec4(0.68f, 0.72f, 0.80f, 1.00f) : AccentSoft();
    c[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = lightTheme ? ImVec4(0.00f, 0.00f, 0.00f, 0.045f) : ImVec4(1.00f, 1.00f, 1.00f, 0.03f);

    c[ImGuiCol_TreeLines] = borderCol;
    c[ImGuiCol_TextSelectedBg] = AccentMed(0.38f);
    c[ImGuiCol_DragDropTarget] = AccentStrong(0.90f);
    c[ImGuiCol_NavCursor] = AccentReadable();
    c[ImGuiCol_NavWindowingHighlight] = AccentStrong(0.70f);
    c[ImGuiCol_NavWindowingDimBg] = dimBg;
    c[ImGuiCol_ModalWindowDimBg] = modalDimBg;

    if (modern)
    {
        // Layers: window (bgDark) < panes (bgMid) < section cards (bgLight); inputs sit inset in the cards.
        // Same alpha as ChildBg, so Background Alpha reaches the cards too.
        const ImVec4 card =
            Mix(BgTint(bgLight, 0.80f), textPrimary, lightTheme ? 0.00f : 0.03f, std::min(minAlpha + 0.1f, 1.0f));
        const ImVec4 inset = BgTint(bgDark, 0.85f);
        const float insetAlpha = std::min(minAlpha + 0.15f, 1.0f);

        c[ImGuiCol_Border] = Mix(bgDark, textPrimary, 0.16f);
        c[ImGuiCol_Separator] = Mix(card, textPrimary, 0.14f);
        c[ImGuiCol_TableBorderStrong] = c[ImGuiCol_Separator];
        c[ImGuiCol_TableBorderLight] = Mix(card, textPrimary, 0.10f);
        c[ImGuiCol_ScrollbarBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

        c[ImGuiCol_FrameBg] = ImVec4(inset.x, inset.y, inset.z, insetAlpha);
        c[ImGuiCol_FrameBgHovered] = Mix(inset, accent, lightTheme ? 0.12f : 0.18f);
        c[ImGuiCol_FrameBgActive] = Mix(inset, accent, lightTheme ? 0.20f : 0.28f);

        c[ImGuiCol_Header] = Mix(card, accent, lightTheme ? 0.14f : 0.18f, 0.90f);
        c[ImGuiCol_HeaderHovered] = Mix(card, accent, lightTheme ? 0.24f : 0.30f, 0.95f);
        c[ImGuiCol_HeaderActive] = Mix(card, accent, lightTheme ? 0.36f : 0.42f);

        gModernPalette.card = toneMapColor(card);
        gModernPalette.cardHeader = toneMapColor(ImVec4(card.x, card.y, card.z, 0.0f));
        gModernPalette.cardHeaderHover = toneMapColor(Mix(card, accent, lightTheme ? 0.12f : 0.18f));
        gModernPalette.cardHeaderActive = toneMapColor(Mix(card, accent, lightTheme ? 0.20f : 0.28f));
        gModernPalette.navBar = toneMapColor(AccentReadable());
        gModernPalette.navSelected = toneMapColor(AccentReadable(lightTheme ? 0.18f : 0.22f));
        gModernPalette.navHover = toneMapColor(AccentReadable(0.26f));
        gModernPalette.navActive = toneMapColor(AccentReadable(0.34f));
    }

    _hdrTonemapApplied = false;
    MenuHdrCheck(ImGui::GetIO());
}

// Every theme, font-affecting and Menu Scale change goes through here so none of them drops the scale.
void MenuCommon::RebuildStyle(float menuScale)
{
    if (ImGui::GetCurrentContext() == nullptr)
        return;

    if (menuScale <= 0.0f)
        menuScale = lastMenuScale > 0.0f ? lastMenuScale : 1.0f;

    ImGuiStyle& style = ImGui::GetStyle();

    // ScaleAllSizes is lossy and changes the stored sizes, so start from a fresh style.
    style = ImGuiStyle();

    ApplyThemeStyle();

    style.ScaleAllSizes(menuScale);
    style.MouseCursorScale = 1.0f;
}

void MenuCommon::ApplyThemeFont()
{
    if (ImGui::GetCurrentContext() == nullptr || gClassicFont == nullptr)
        return;

    const bool useInter = Config::Instance()->ModernTheme.value_or_default() && gInterFont != nullptr;
    ImGui::GetIO().FontDefault = useInter ? gInterFont : gClassicFont;
}

// Live numbers keep the fixed-width font so digits don't jitter when the default is proportional.
static void PushMonoFontSize(float size)
{
    if (gHackFont != nullptr)
        ImGui::PushFont(gHackFont, size);
    else
        ImGui::PushFontSize(size);
}

static double lastTime = 0.0;
static double lastFrameTime = 0.0;
static UINT64 uwpTargetFrame = 0;

void MenuCommon::Present()
{
    _frameCount++;

    auto now = Util::MillisecondsNow();

    if (lastTime > 0.0)
        lastFrameTime = now - lastTime;

    lastTime = now;

    if (_handle != nullptr)
        UpdateManualInput(_handle);
}

struct VersionCheckStatus
{
    bool completed = false;
    bool updateAvailable = false;
    std::string latestTag;
    std::string latestUrl;
    std::string error;
};

static PresentRateMeter fgOutputPresentRate;

// Streamline can present real and generated frames from different threads.
// Measure the actual output-present cadence, not the number of presents
// observed between two (independently paced) menu frames.
void MenuCommon::FGOutputPresent() { fgOutputPresentRate.Present(Util::MillisecondsNow()); }

void MenuCommon::FGOutputPresents(bool presents) { fgOutputPresentRate.SetEnabled(presents); }

struct MenuCommon::RenderMenuContext
{
    State& state;
    decltype(Config::Instance()) config;
    ImGuiIO& io;
    IFeature* currentFeature = nullptr;

    double now = 0.0;
    double frameTime = 0.0;
    double frameRate = 0.0;
    // Current Vulkan FG output rate, kept separate from the long-term history.
    double fgOutputFrameTimeMs = 0.0;
    float menuResScale = 1.0f;
    float fpsScale = 1.0f;
    float averageFrameTime = 0.0f;
    float averageUpscalerFT = 0.0f;

    bool frameTimesCalculated = false;
    bool newFrame = false;

    VersionCheckStatus versionStatus;
    std::string currentVersionText;

    // Cached when the menu is visible and shared by RenderMainMenuWindow section helpers.
    std::unique_ptr<std::decay_t<decltype(IdentifyGpu::getPrimaryGpu())>> primaryGpu;
};

static std::string splashMessage;

void MenuCommon::UpdateRenderTiming(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    auto& now = ctx.now;
    auto& frameTime = ctx.frameTime;
    auto& frameRate = ctx.frameRate;

    if (config->OverlayMenu.value_or_default())
    {
        _frameCount++;

        // FPS & frame time calculation
        if (lastTime > 0.0)
        {
            frameTime = now - lastTime;
            frameRate = 1000.0 / frameTime;
        }

        lastTime = now;

        if (_handle != nullptr)
            UpdateManualInput(_handle);
    }
    else
    {
        if (state.activeFgInput == FGInput::NoFG || state.activeFgOutput == FGOutput::NoFG)
            MenuCommon::Present();

        frameTime = lastFrameTime;
        frameRate = 1000.0 / frameTime;
    }

    // A dxvk game on the Vulkan bridge: the menu is drawn on the game's presents only, frame generation's swapchain
    // presents the generated frames past our hooks. The shown frames come that many times as often.
    if (auto fg = state.currentFG; VkPresentBridge::MenuOnGamePresents() && fg != nullptr && fg->IsGenerating() &&
                                   frameTime > 0.0)
    {
        frameTime /= (double) (fg->GetInterpolatedFrameCount() + 1);
        frameRate = 1000.0 / frameTime;
    }

    ctx.fgOutputFrameTimeMs = fgOutputPresentRate.FrameTime(now);
    if (ctx.fgOutputFrameTimeMs > 0.0)
    {
        frameTime = ctx.fgOutputFrameTimeMs;
        frameRate = 1000.0 / frameTime;
    }

    state.frameTimes.pop_front();
    state.frameTimes.push_back(frameTime);
}

void MenuCommon::UpdateMenuInputMode(RenderMenuContext& ctx)
{
    auto& io = ctx.io;

    // Moved here to prevent gamepad key replay
    if (_isVisible)
    {
        if (hasGamepad)
            io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

        io.ConfigFlags = ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    }
    else
    {
        capturingKey = false;
        hasGamepad = (io.BackendFlags & ImGuiBackendFlags_HasGamepad) != 0;
        io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
        io.ConfigFlags = ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NoKeyboard;
    }
}

void MenuCommon::HandleMenuShortcuts(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    auto& io = ctx.io;

    // Handle Inputs
    {
        if (inputFG)
        {
            inputFG = false;

            if (state.activeFgInput != FGInput::NoFG && state.activeFgOutput != FGOutput::NoFG &&
                (state.FGSwapchainExists() || state.activeFgInput == FGInput::NvngxFG))
            {
                config->FGEnabled = !config->IsFGEnabled();
                LOG_DEBUG("FG toggle key pressed, setting FGEnabled to {}", config->IsFGEnabled());

                if (config->IsFGEnabled())
                    state.fgChanged = true;
            }
        }

        if (inputFps)
        {
            inputFps = false;
            config->ShowFps = !config->ShowFps.value_or_default();
        }

        if (inputDlssNr)
        {
            inputDlssNr = false;
            config->DlssNrEnabled = !config->DlssNrEnabled.value_or_default();
            LOG_DEBUG("Neural Rendering toggle key pressed, setting DlssNrEnabled to {}",
                      config->DlssNrEnabled.value_or_default());

            ImGuiToast toast { ImGuiToastType::Info, 2000 };
            toast.setTitle("DLSS Neural Rendering");
            toast.setContent(config->DlssNrEnabled.value_or_default() ? "On" : "Off");
            ImGui::InsertNotification(toast);
        }

        if (inputHdrCompare)
        {
            inputHdrCompare = false;

            // Only while the picture is being converted: otherwise the key would say the picture changed when nothing did
            const bool hdrActive = SdrToHdr::Status().active;

            if (hdrActive)
                config->HdrOutputConvert = !config->HdrOutputConvert.value_or_default();

            ImGuiToast toast { ImGuiToastType::Info, 2000 };
            toast.setTitle("HDR output");
            toast.setContent(!hdrActive                                      ? "HDR output is not active"
                             : config->HdrOutputConvert.value_or_default() ? "Converted"
                                                                             : "As Windows shows it");
            ImGui::InsertNotification(toast);
        }

        if (inputFpsCycle && config->ShowFps.value_or_default())
            config->FpsOverlayType = (FpsOverlay) ((config->FpsOverlayType.value_or_default() + 1) % FpsOverlay_COUNT);

        if (inputMenu)
        {
            inputMenu = false;
            _isVisible = !_isVisible;

            LOG_DEBUG("Menu key pressed, {0}", _isVisible ? "opening ImGui" : "closing ImGui");

            if (_isVisible)
            {
                io.ClearEventsQueue();
                io.ClearInputKeys();
                io.ClearInputMouse();

                OptiInput::ResetMenuInputTransientState();

                RebuildStyle();

                refreshRate = Util::GetActiveRefreshRate(_handle);

                auto optiPath = std::filesystem::path(Config::Instance()->MainDllPath.value());
                state.artursFgFileAvailable = enablerExists.Get(optiPath / L"dlss-enabler-headless.dll");
                state.nukemsFgFileAvailable = nukemsExists.Get(optiPath / L"dlssg_to_fsr3_amd_is_better.dll");

                if (State::Instance().currentFeature != nullptr)
                {
                    if (State::Instance().currentFeature->GetUpscalerType() == Upscaler::DLSSD)
                        comboPreset = config->DLSSDRenderPresetForAll.value_or_default();
                    else if (State::Instance().currentFeature->GetUpscalerType() == Upscaler::DLSS)
                        comboPreset = config->RenderPresetForAll.value_or_default();
                }
            }
            else
            {
                ImGui::CloseCurrentPopup();

                _showMipmapCalcWindow = false;
                _showHudlessWindow = false;
            }

            io.MouseDrawCursor = _isVisible;
            io.WantCaptureKeyboard = _isVisible;
            io.WantCaptureMouse = _isVisible;
        }

        inputFpsCycle = false;
    }
}

void MenuCommon::UpdateVersionAndStartupNotifications(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    auto& now = ctx.now;
    auto& versionStatus = ctx.versionStatus;

    constexpr double splashTime = 7000.0;
    constexpr int updateNoticeTime = 10000;

    // Version check state is copied while locked, then consumed by the UI render pass.
    {
        std::scoped_lock lock(state.versionCheckMutex);
        versionStatus.completed = state.versionCheckCompleted;
        versionStatus.updateAvailable = state.updateAvailable;
        versionStatus.latestTag = state.latestVersionTag;
        versionStatus.latestUrl = state.latestVersionUrl;
        versionStatus.error = state.versionCheckError;
    }

    ctx.currentVersionText = VersionCheck::CurrentVersionString();

    if (versionStatus.completed && versionStatus.updateAvailable && !versionStatus.latestTag.empty())
    {
        if (updateNoticeTag != versionStatus.latestTag)
        {
            updateNoticeTag = versionStatus.latestTag;
            updateNoticeUrl = versionStatus.latestUrl;
            const auto notice = [&]()
            {
                ImGuiToast updateNotification { ImGuiToastType::Error, updateNoticeTime };
                updateNotification.setTitle("OptiScaler-DLSSNR Update available");
                updateNotification.setContent(
                    "Press %s for more info",
                    Keybind::KeyNameFromVirtualKeyCode(config->ShortcutKey.value_or_default()).c_str());
                ImGui::InsertNotification(updateNotification);
                return true;
            };
            static auto res = notice();
        }
    }

    // One-shot startup warning notifications.
    if (!state.postDone)
    {
        if (state.postCodes & PostCode::SlPluginsAlreadyInMemory)
        {
            auto filename = Util::DllPath().filename().string();
            to_lower_in_place(filename);

            ImGuiToast notification { ImGuiToastType::Warning, 10000 };
            notification.setTitle("Late Streamline hook detected");
            notification.setContent(
                "Consider renaming OptiScaler from %s to other supported name.\nYou may experience issues otherwise.",
                filename.c_str());
            ImGui::InsertNotification(notification);
        }

        if (state.postCodes & PostCode::TryingFsr4Fp8OnUnsupported)
        {
            ImGuiToast notification { ImGuiToastType::Warning, 10000 };
            notification.setTitle("Silly goose detected");
            notification.setContent("FSR 4 FP8 only works on AMD");
            ImGui::InsertNotification(notification);
        }

        state.postDone = true;
    }

    // Initialize splash timing and select the splash text once per process.
    if (splashLimit < 1.0f)
    {
        splashStart = now + 100.0;
        splashLimit = splashStart + splashTime;

        std::srand(static_cast<unsigned>(std::time(nullptr)));
        splashMessage = splashText[std::rand() % splashText.size()];
    }
}

void MenuCommon::BeginMenuFrameIfNeeded(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    auto& io = ctx.io;
    auto& now = ctx.now;
    auto& newFrame = ctx.newFrame;

    // New frame check
    // The lamp is drawn while the menu is closed, which is the whole point of it. Tied to its own
    // setting and nothing else: an overlay that appears because a scan is running, rather than
    // because someone asked for it, is an overlay nobody asked for.
    const bool scanIndicator = config->DlssNrScanMeter.value_or_default() &&
                               DlssNr::ExposureScan::Where() != DlssNr::ExposureScan::Verdict::Off;

    if ((!config->DisableSplash.value_or_default() && now > splashStart && now < splashLimit) ||
        config->ShowFps.value_or_default() || _isVisible || ImGui::notifications.size() > 0 || scanIndicator ||
        NativeMotionDx12::ExpandedPreviewVisible() || NativeMotionDx11::ExpandedPreviewVisible() ||
        NativeMotionVk::ExpandedPreviewVisible() ||
        (config->DlssNrCompare.value_or_default() != 0 && config->DlssNrCompareTags.value_or_default()))
    {
        if (!_isUWP)
        {
            ImGui_ImplWin32_NewFrame();
        }
        else
        {
            ImVec2 displaySize { state.screenWidth, state.screenHeight };
            ImGui_ImplUwp_NewFrame(displaySize);
        }

        OptiInput::FeedImGui(_isVisible);

        MenuHdrCheck(io);
        ImGui::NewFrame();

        newFrame = true;
    }
}

void MenuCommon::RenderSplashWindow(RenderMenuContext& ctx)
{
    auto config = ctx.config;
    auto& io = ctx.io;
    auto& now = ctx.now;

    constexpr double fadeTime = 1000.0;

    // Splash screen
    if (!config->DisableSplash.value_or_default())
    {
        if (now > splashStart && now < splashLimit)
        {

            ImGui::SetNextWindowSize({ 0.0f, 0.0f });
            ImGui::SetNextWindowBgAlpha(config->FpsOverlayAlpha.value_or_default());
            ImGui::SetNextWindowPos(splashPosition, ImGuiCond_Always);

            float windowAlpha = 1.0f;
            if (auto diff = now - splashStart; diff < fadeTime)
                windowAlpha = static_cast<float>(diff / fadeTime);
            else if (auto diff = splashLimit - now; diff < fadeTime)
                windowAlpha = static_cast<float>(diff / fadeTime);

            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, windowAlpha);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 8));
            ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));

            if (!config->OverlaysUseTheme.value_or_default())
            {
                ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Text, toneMapColor(ImVec4(1.0f, 1.0f, 1.0f, 1.0f)));
            }

            if (ImGui::Begin("Splash", nullptr,
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDecoration |
                                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoNav))
            {
                float splashScale = 1.0f;
                float baseScaleHeight = 720.0f;

                if (io.DisplaySize.y > baseScaleHeight)
                    splashScale = io.DisplaySize.y / baseScaleHeight;

                if (config->UseHQFont.value_or_default())
                    ImGui::PushFontSize(std::round(splashScale * fontSize));
                else
                    ImGui::SetWindowFontScale(splashScale);

                ImGui::Text("OptiScaler - %s for menu",
                            Keybind::KeyNameFromVirtualKeyCode(config->ShortcutKey.value_or_default()).c_str());
                ImGui::TextColored(toneMapColor(ImVec4(1.0f, 1.0f, 1.0f, 0.7f)), splashMessage.c_str());

                splashSize = ImGui::GetWindowSize();

                if (config->UseHQFont.value_or_default())
                    ImGui::PopFontSize();

                ImGui::End();

                splashPosition.x = 0.0f; // io.DisplaySize.x - splashWinSize.x;
                splashPosition.y = io.DisplaySize.y - splashSize.y;
            }

            if (!config->OverlaysUseTheme.value_or_default())
                ImGui::PopStyleColor(4);
            else
                ImGui::PopStyleColor(2);

            ImGui::PopStyleVar(2);
        }
    }
}

void MenuCommon::RenderNotifications(RenderMenuContext& ctx)
{
    auto config = ctx.config;
    auto& io = ctx.io;

    // Notifications
    const UiTargetMode uiTargetMode = getUiTargetMode();
    const bool tonemapRequired = uiTargetMode != UiTargetMode::SDR;

    float screenHeight = State::Instance().screenHeight;
    if (io.DisplaySize.y != 0)
        screenHeight = io.DisplaySize.y;

    // Map resolution height to scale, 0.5 for 480p, 2.0 for 1440p
    constexpr float slope = (2.0f - 0.5f) / (1440.f - 480.f);
    float notificationScale = 0.5f + slope * (screenHeight - 480.f);
    notificationScale = std::clamp(notificationScale, 0.5f, 2.0f);

    if (config->UseHQFont.value_or_default())
        ImGui::PushFontSize(std::round(notificationScale * fontSize));

    // No fallback font, SetWindowFontScale needs to be called after Begin()

    DlssNr::UpdateF5LowHint(config);
    DlssNr::UpdateMultiViewHint(config);

    ImGui::RenderNotifications(ImGuiToastPos::TopCenter, notificationScale, tonemapRequired);

    if (config->UseHQFont.value_or_default())
        ImGui::PopFontSize();
}

void MenuCommon::UpdateFrameTimeAverages(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    auto& frameTime = ctx.frameTime;
    auto& frameRate = ctx.frameRate;
    auto& frameTimesCalculated = ctx.frameTimesCalculated;
    auto& menuResScale = ctx.menuResScale;
    auto& fpsScale = ctx.fpsScale;
    auto& averageFrameTime = ctx.averageFrameTime;
    auto& averageUpscalerFT = ctx.averageUpscalerFT;

    // FPS Overlay font
    fpsScale = config->FpsScale.value_or(menuResScale);

    // Update frame time & upscaler time averages
    averageFrameTime = 0.0f;
    averageUpscalerFT = 0.0f;

    if (config->ShowFps.value_or_default() || _isVisible)
    {
        float frameCnt = 0;
        frameTime = 0;
        for (size_t i = 299; i > 199; i--)
        {
            if (state.frameTimes[i] > 0.0)
            {
                frameTime += state.frameTimes[i];
                frameCnt++;
            }
        }

        frameTime /= frameCnt;
        frameRate = 1000.0 / frameTime;
        frameTimesCalculated = true;

        // Vulkan FG output timing is already averaged across the last 125 ms.
        // Use it directly for the current FPS readout; preserve the shared
        // history for the separately labelled average and frame-time graph.
        if (ctx.fgOutputFrameTimeMs > 0.0)
        {
            frameTime = ctx.fgOutputFrameTimeMs;
            frameRate = 1000.0 / frameTime;
        }

        float lastFT = static_cast<float>(state.frameTimes.empty() ? 0.0f : state.frameTimes.back());
        float lastUT = static_cast<float>(state.upscaleTimes.empty() ? 0.0f : state.upscaleTimes.back());

        if (!fgOutputPresentRate.TakeIntervals([](double ms) { gFrameTimes.Push(static_cast<float>(ms)); }))
            gFrameTimes.Push(lastFT);

        gUpscalerTimes.Push(lastUT);

        averageFrameTime = gFrameTimes.Average();
        averageUpscalerFT = gUpscalerTimes.Average();
    }
}

// Labels for the comparison views.
//
// Drawn straight onto the foreground draw list, not as ImGui windows -- the last attempt made them
// draggable windows and the clamping fought the split. Here each label is clipped to its own side of
// the comparison, so in the wipe the moving split reveals and hides it exactly as it does the
// pictures, and there is nothing to drag. Both wipe labels sit in the same top-left corner, each
// clipped to its side, so whichever picture currently owns that corner is the one whose label shows.
void MenuCommon::RenderNrCompareTags()
{
    auto* config = Config::Instance();

    const uint32_t mode = config->DlssNrCompare.value_or_default();

    if (mode == 0 || !config->DlssNrCompareTags.value_or_default())
        return;

    const ImVec2 screen = ImGui::GetIO().DisplaySize;

    if (screen.x < 1.0f || screen.y < 1.0f)
        return;

    const bool swap = config->DlssNrCompareSwap.value_or_default();
    const float split = mode == 1 ? 0.5f
                                  : std::clamp(config->DlssNrCompareSplit.value_or_default(), 0.0f, 1.0f);
    const float splitX = split * screen.x;

    const float scale = std::clamp(config->DlssNrTagScale.value_or_default(), 0.5f, 5.0f);

    // The left side is the untouched frame unless swapped -- matching the shader's
    // showOriginal = (uv.x < split) != swap.
    const char* leftText = swap ? "DLSS NR : ON" : "DLSS NR : OFF";
    const char* rightText = swap ? "DLSS NR : OFF" : "DLSS NR : ON";

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize() * scale;
    const float margin = 10.0f * scale;

    // Both labels flank the divider along the top: the left picture's label is right-aligned just
    // left of the split, the right picture's is left-aligned just right of it. Each is clipped to its
    // own side, so in the wipe the split reveals and hides them along with the images.
    auto drawTag = [&](const char* text, float x, ImVec2 clipMin, ImVec2 clipMax)
    {
        const ImVec2 size = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text);

        // Never let a label run off the visible frame as it grows.
        x = std::min(std::max(x, 0.0f), screen.x - size.x);
        float y = std::min(margin, screen.y - size.y - margin);
        y = std::max(y, 0.0f);

        dl->PushClipRect(clipMin, clipMax, true);
        dl->AddText(font, fontSize, ImVec2(x + 2.0f, y + 2.0f), IM_COL32(0, 0, 0, 210), text);
        dl->AddText(font, fontSize, ImVec2(x, y), IM_COL32(255, 255, 255, 255), text);
        dl->PopClipRect();
    };

    const ImVec2 leftSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, leftText);

    // Left picture's label: right edge a margin in from the split. Right picture's: left edge a margin
    // out from the split.
    drawTag(leftText, splitX - margin - leftSize.x, ImVec2(0.0f, 0.0f), ImVec2(splitX, screen.y));
    drawTag(rightText, splitX + margin, ImVec2(splitX, 0.0f), ImVec2(screen.x, screen.y));
}

void MenuCommon::RenderPerformanceOverlay(RenderMenuContext& ctx)
{
    RenderNrCompareTags();


    auto& state = ctx.state;
    auto config = ctx.config;
    auto& io = ctx.io;
    auto& currentFeature = ctx.currentFeature;
    auto& now = ctx.now;
    auto& frameTime = ctx.frameTime;
    auto& frameRate = ctx.frameRate;
    auto& menuResScale = ctx.menuResScale;
    auto& fpsScale = ctx.fpsScale;
    auto& averageFrameTime = ctx.averageFrameTime;
    auto& averageUpscalerFT = ctx.averageUpscalerFT;

    // If Fps overlay is visible
    if (config->ShowFps.value_or_default())
    {
        bool stylePushed = false;

        const static auto defaultStyle = ImGuiStyle();

        // Rescale the fps overlay every frame because it shares style with the main menu
        if (config->FpsScale.has_value() && config->FpsScale.value() != menuResScale)
        {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, defaultStyle.WindowPadding * fpsScale);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, defaultStyle.FramePadding * fpsScale);
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, defaultStyle.CellPadding * fpsScale);
            ImGui::PushStyleVar(ImGuiStyleVar_SeparatorTextPadding, defaultStyle.SeparatorTextPadding * fpsScale);

            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, defaultStyle.ItemSpacing * fpsScale);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, defaultStyle.ItemInnerSpacing * fpsScale);
            ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, defaultStyle.IndentSpacing * fpsScale);

            stylePushed = true;
        }

        // Set overlay position
        ImGui::SetNextWindowPos(overlayPosition, ImGuiCond_Always);

        // Set overlay window properties
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));  // Transparent border
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0)); // Transparent frame background

        if (!config->OverlaysUseTheme.value_or_default())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, toneMapColor(ImVec4(1.0f, 1.0f, 1.0f, 1.0f)));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
        }

        ImGui::SetNextWindowBgAlpha(config->FpsOverlayAlpha.value_or_default()); // Transparent background

        if (!config->OverlaysUseTheme.value_or_default())
        {
            ImVec4 green(0.0f, 1.0f, 0.0f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_PlotLines, toneMapColor(green));
        }

        if (ImGui::Begin("Performance Overlay", nullptr,
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav))
        {
            std::string api;
            if (IdentifyGpu::getPrimaryGpu().usesDxvk && state.api == DX11)
            {
                if (state.swapchainInteropApi == SwapchainInteropApi::None)
                    api = "DXVK";
                else
                    api = "DXVK w/Dx12";
            }
            else if (IdentifyGpu::getPrimaryGpu().usesVkd3dProton && state.api == DX12)
            {
                api = "VKD3D";
            }
            else
            {
                switch (state.swapchainApi)
                {
                case Vulkan:
                    api = state.swapchainInteropApi == SwapchainInteropApi::VkwDx12 ? "VLK w/DX12" : "VLK";
                    break;

                case DX11:
                    api = "D3D11";
                    break;

                case DX12:
                    if (state.swapchainInteropApi == SwapchainInteropApi::Dx11wDx12)
                        api = "D3D11 w/DX12";
                    else
                        api = "D3D12";

                    break;

                default:
                    switch (state.api)
                    {
                    case Vulkan:
                        api = "VLK";
                        break;

                    case DX11:
                        api = "D3D11";
                        break;

                    case DX12:
                        api = "D3D12";
                        break;

                    default:
                        api = "???";
                        break;
                    }

                    break;
                }
            }

            if (config->UseHQFont.value_or_default())
                PushMonoFontSize(std::round(fpsScale * fontSize));
            else
                ImGui::SetWindowFontScale(fpsScale);

            auto fg = state.CurrentFG();
            auto fgText =
                (fg != nullptr && fg->IsGenerating()) ? (" (" + std::string(fg->Name()) + ")") : std::string();

            const int fakeFramesCount = DlssgInterpolationCount(state);
            auto formatFg = [&](std::string_view name, int maxFakeFrames)
            {
                if (fakeFramesCount > maxFakeFrames)
                    return std::format(" ({} Doesn't support more than {}x)", name, maxFakeFrames);

                else if (fakeFramesCount == 0)
                    return std::format(" ({} off)", name);

                return std::format(" ({} x{})", name, fakeFramesCount + 1);
            };

            const FGNvngxReplacement activeNvngxFg = state.activeFgNvngx;
            if (activeNvngxFg == FGNvngxReplacement::Arturs)
            {
                fgText = formatFg("Enabler", Nvngx_FG::getMaxFakeFramesCount());
            }
            else if (activeNvngxFg == FGNvngxReplacement::Nukems)
            {
                fgText = formatFg("Nukems", Nvngx_FG::getMaxFakeFramesCount());
            }
            else if (activeNvngxFg == FGNvngxReplacement::FFX)
            {
                fgText = formatFg("FFX", Nvngx_FG::getMaxFakeFramesCount());
            }
            else if (activeNvngxFg == FGNvngxReplacement::Combo)
            {
                fgText = formatFg("Combo", Nvngx_FG::getMaxFakeFramesCount());
            }
            else if (state.activeFgOutput == FGOutput::DLSSG && fg)
            {
                fgText = formatFg("DLSSG", fg->GetMaxInterpolationCount());
            }

            const auto overlayType = config->FpsOverlayType.value_or_default();
            const bool hasFeature = currentFeature && !currentFeature->IsFrozen();

            // Prepare Line 1
            std::string featurePart;
            std::string fpsPart;

            if (hasFeature)
            {
                const bool usesDx12CompatLayer = currentFeature->IsWithDx12();

                featurePart = StrFmt(" | %s -> %s %u.%u.%u%s", ApiUpscalerInputName(state.currentInputApiName).c_str(),
                                     currentFeature->ShortName().c_str(), currentFeature->Version().major,
                                     currentFeature->Version().minor, currentFeature->Version().patch,
                                     usesDx12CompatLayer ? " w/Dx12" : "");
            }

            if (fg != nullptr && fg->IsGenerating())
            {
                const double baseFps = frameRate / (double) (fg->GetInterpolatedFrameCount() + 1);

                switch (overlayType)
                {
                case FpsOverlay_JustFPS:
                    fpsPart = StrFmt("%6.1f/%5.1f ", frameRate, baseFps);
                    break;

                case FpsOverlay_Simple:
                    fpsPart = StrFmt("FPS: %6.1f/%5.1f, %7.2f ms", frameRate, baseFps, frameTime);
                    break;

                default:
                    fpsPart = StrFmt("FPS: %6.1f/%5.1f, Avg: %6.1f", frameRate, baseFps, 1000.0f / averageFrameTime);
                    break;
                }
            }
            else
            {
                switch (overlayType)
                {
                case FpsOverlay_JustFPS:
                    fpsPart = StrFmt("%6.1f ", frameRate);
                    break;

                case FpsOverlay_Simple:
                    fpsPart = StrFmt("FPS: %6.1f, %7.2f ms", frameRate, frameTime);
                    break;

                default:
                    fpsPart = StrFmt("FPS: %6.1f, Avg: %6.1f", frameRate, 1000.0f / averageFrameTime);
                    break;
                }
            }

            struct OverlayLine
            {
                std::string text;
                const char* graphId = nullptr;
                RingBuffer<float, plotWidth>* graphData = nullptr;
                float graphMax = 0.0f;
            };

            std::vector<OverlayLine> lines;

            if (overlayType == FpsOverlay_JustFPS)
                lines.push_back({ fpsPart });
            else
                lines.push_back(
                    { StrFmt("%s | %s%s%s", api.c_str(), fpsPart.c_str(), fgText.c_str(), featurePart.c_str()) });

            if (overlayType >= FpsOverlay_Simple && hasFeature && config->FpsOverlayShowRatio.value_or_default() &&
                currentFeature->RenderWidth() > 0)
            {
                const float ratio = (float) currentFeature->TargetWidth() / (float) currentFeature->RenderWidth();

                lines.push_back({ StrFmt("Upscale: %ux%u -> %ux%u (%.2fx)", currentFeature->RenderWidth(),
                                         currentFeature->RenderHeight(), currentFeature->TargetWidth(),
                                         currentFeature->TargetHeight(), ratio) });
            }

            if (overlayType >= FpsOverlay_Detailed)
            {
                OverlayLine line { StrFmt("Frame Time: %7.2f ms, Avg: %7.2f ms", state.frameTimes.back(),
                                          averageFrameTime) };

                if (overlayType >= FpsOverlay_DetailedGraph)
                {
                    line.graphId = "##FrameTimeGraph";
                    line.graphData = &gFrameTimes;
                    line.graphMax = 66.6f;
                }

                lines.push_back(std::move(line));
            }

            if (overlayType >= FpsOverlay_Full)
            {
                OverlayLine line { StrFmt("Upscaler Time: %7.2f ms, Avg: %7.2f ms", state.upscaleTimes.back(),
                                          averageUpscalerFT) };

                if (overlayType >= FpsOverlay_FullGraph)
                {
                    line.graphId = "##UpscalerFrameTimeGraph";
                    line.graphData = &gUpscalerTimes;
                    line.graphMax = 20.0f;
                }

                lines.push_back(std::move(line));
            }

            const bool horizontal = config->FpsOverlayHorizontal.value_or_default();

            ImVec2 plotSize;
            if (horizontal)
            {
                plotSize = { fpsScale * 150, fpsScale * 16 };
            }
            else
            {
                // Graphs match the widest text line
                auto textWidth = 0.0f;
                for (const auto& line : lines)
                    textWidth = std::max(textWidth, ImGui::CalcTextSize(line.text.c_str()).x);

                plotSize = { std::max(textWidth, fpsScale * 300.0f), fpsScale * 30 };
            }

            // Draw the overlay
            for (size_t i = 0; i < lines.size(); i++)
            {
                const auto& line = lines[i];

                if (i > 0)
                {
                    if (horizontal)
                    {
                        ImGui::SameLine(0.0f, 0.0f);
                        ImGui::Text(" | ");
                        ImGui::SameLine(0.0f, 0.0f);
                    }
                    else
                    {
                        ImGui::Spacing();
                    }
                }

                ImGui::TextUnformatted(line.text.c_str());

                if (line.graphData != nullptr)
                {
                    if (horizontal)
                        ImGui::SameLine(0.0f, 0.0f);

                    ImGui::PlotLines(
                        line.graphId, [](void* rb, int idx) -> float
                        { return static_cast<RingBuffer<float, plotWidth>*>(rb)->At(idx); }, line.graphData, plotWidth,
                        0, nullptr, 0.0f, line.graphMax, plotSize);
                }
            }

            if (config->FpsOverlayType.value_or_default() >= FpsOverlay_ReflexTimings)
            {
                constexpr auto delayBetweenPollsMs = 500;
                static auto previousPoll = 0.0;
                static bool gotData = false;

#ifdef LOW_LATENCY_INPUTS
                static TimingData timingData {};

                if (previousPoll <= 0.001 || previousPoll + delayBetweenPollsMs < now)
                {
                    gotData = InputCommon::get_timing_data(timingData);
                    previousPoll = now;
                }

                if (gotData && timingData.timeRange.has_value())
                {
                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    constexpr float offsetForText = 155;

                    const auto& rangeInNs = timingData.timeRange.value().length;

                    UINT64 localFrameCount = 0;

                    if (fg != nullptr)
                        localFrameCount = fg->FrameCount();

                    ImGui::Text("FGId: %llu, RfxId: %llu", localFrameCount, state.reflexFrameId);
                    DrawInputLatencyLine(false);

                    const auto maxWidth =
                        config->FpsOverlayHorizontal.value_or_default() ? ImGui::GetWindowWidth() : plotSize.x;

                    // A row without data (no entry, or no length: the report's times were zero) is not drawn
                    const auto drawTiming = [&](const auto& timingOpt, const char* desc, ImVec4 color) -> bool
                    {
                        if (!timingOpt.has_value() || timingOpt.value().length <= 0.0)
                            return false;

                        auto toneMappedColor = State::Instance().isHdrActive ? toneMapColor(color) : color;

                        const auto& timing = timingOpt.value();
                        float duration = static_cast<float>(timing.length * rangeInNs / 1000.0);

                        ImGui::TextColored(toneMappedColor, "%-12s %4.1fms", desc, duration);

                        auto leftLimit = ImGui::GetItemRectMin().x + offsetForText * fpsScale;

                        auto start = static_cast<float>(leftLimit + (ImGui::GetItemRectMin().x + maxWidth - leftLimit) *
                                                                        timing.position);

                        auto end = static_cast<float>(start + (ImGui::GetItemRectMin().x + maxWidth - leftLimit) *
                                                                  timing.length);

                        auto pos = ImVec2(start, ImGui::GetItemRectMin().y);
                        auto size = ImVec2(end, ImGui::GetItemRectMax().y);

                        drawList->AddRectFilled(pos, size, ImGui::ColorConvertFloat4ToU32(toneMappedColor));
                        return true;
                    };

                    drawTiming(timingData.simulation, "Simulation", ImVec4(0.768f, 0.169f, 0.169f, 1.0f));
                    drawTiming(timingData.renderSubmit, "RenderSubmit", ImVec4(0.235f, 0.705f, 0.294f, 1.0f));
                    drawTiming(timingData.present, "Present", ImVec4(1.0f, 0.88f, 0.098f, 1.0f));
                    bool anyGpuRow = drawTiming(timingData.driver, "Driver", ImVec4(0.263f, 0.388f, 0.847f, 1.0f));
                    anyGpuRow |=
                        drawTiming(timingData.osRenderQueue, "RenderQueue", ImVec4(0.76f, 0.51f, 0.188f, 1.0f));
                    anyGpuRow |= drawTiming(timingData.gpuRender, "GpuRender", ImVec4(0.569f, 0.117f, 0.705f, 1.0f));

                    if (!anyGpuRow)
                        ImGui::TextDisabled("GPU timing not available");
                }
#else
                if (previousPoll <= 0.001 || previousPoll + delayBetweenPollsMs < now)
                {
                    gotData = ReflexHooks::updateTimingData();
                    previousPoll = now;
                }

                auto& timingData = ReflexHooks::timingData;

                if (gotData && timingData[TimingType::TimeRange].has_value())
                {
                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    constexpr float offsetForText = 155;

                    const auto& rangeInNs = timingData[TimingType::TimeRange].value().length;

                    // The frame markers' ID of the frame being rendered (ours, else the game's render submit or
                    // simulation start): State::reflexFrameId moves with every marker and with the latency report
                    uint64_t markerId = native::lowlatency::FrameIdForFrameGeneration();

                    if (markerId == 0)
                        markerId = ReflexHooks::lastMarkerFrameId(RENDERSUBMIT_START);

                    if (markerId == 0)
                        markerId = ReflexHooks::lastMarkerFrameId(SIMULATION_START);

                    // XeFG's Present Id is the ID it matches against the markers; another frame generation's counter
                    // is its own, so it is only shown next to the markers' ID
                    if (fg != nullptr && UsesXeFGApi(state.activeFgOutput) && fg->IsActive())
                    {
                        ImGui::Text("XeFG ID: %u (%s), marker ID: %llu", state.xefgPresentId.load(std::memory_order_relaxed),
                                    state.xefgPresentIdFollowsMarkers.load(std::memory_order_relaxed) ? "follows the markers"
                                                                                                                      : "own numbering",
                                    markerId);
                    }
                    else
                    {
                        ImGui::Text("FG frame: %llu, marker ID: %llu", fg != nullptr ? fg->FrameCount() : 0ull,
                                    markerId);
                    }
                    // fakenvapi (XeFG's XeLL, Anti-Lag 2, LatencyFlex) has only the markers' times: its driver, queue
                    // and GPU entries are made up, so they are not drawn
                    const bool estimated = ReflexHooks::timingEstimated();

                    DrawInputLatencyLine(estimated);

                    if (estimated)
                        ImGui::TextDisabled("From the frame markers (fakenvapi: no GPU times)");

                    const auto maxWidth =
                        config->FpsOverlayHorizontal.value_or_default() ? ImGui::GetWindowWidth() : plotSize.x;

                    // A row without data (no entry, or no length: the report's times were zero) is not drawn
                    const auto drawTiming = [&](TimingType type, const char* desc, ImVec4 color) -> bool
                    {
                        if (!timingData[type].has_value() || timingData[type].value().length <= 0.0)
                            return false;

                        auto toneMappedColor = toneMapColor(color);

                        auto& timing = timingData[type].value();
                        float duration = static_cast<float>(timing.length * rangeInNs / 1000.0);
                        ImGui::TextColored(toneMappedColor, "%-12s %4.1fms", desc, duration);
                        auto leftLimit = ImGui::GetItemRectMin().x + offsetForText * fpsScale;
                        auto start = static_cast<float>(leftLimit + (ImGui::GetItemRectMin().x + maxWidth - leftLimit) *
                                                                        timing.position);
                        auto end = static_cast<float>(start + (ImGui::GetItemRectMin().x + maxWidth - leftLimit) *
                                                                  timing.length);
                        auto pos = ImVec2(start, ImGui::GetItemRectMin().y);
                        auto size = ImVec2(end, ImGui::GetItemRectMax().y);
                        drawList->AddRectFilled(pos, size, ImGui::ColorConvertFloat4ToU32(toneMappedColor));
                        return true;
                    };

                    drawTiming(TimingType::Simulation, "Simulation", ImVec4(0.768f, 0.169f, 0.169f, 1.0f));
                    drawTiming(TimingType::RenderSubmit, "RenderSubmit", ImVec4(0.235f, 0.705f, 0.294f, 1.0f));
                    drawTiming(TimingType::Present, "Present", ImVec4(1.0f, 0.88f, 0.098f, 1.0f));

                    if (!estimated)
                    {
                        bool anyGpuRow = drawTiming(TimingType::Driver, "Driver", ImVec4(0.263f, 0.388f, 0.847f, 1.0f));
                        anyGpuRow |=
                            drawTiming(TimingType::OsRenderQueue, "RenderQueue", ImVec4(0.76f, 0.51f, 0.188f, 1.0f));
                        anyGpuRow |=
                            drawTiming(TimingType::GpuRender, "GpuRender", ImVec4(0.569f, 0.117f, 0.705f, 1.0f));

                        if (!anyGpuRow)
                            ImGui::TextDisabled("GPU timing not available");
                    }
                }
#endif
            }
        }

        // Restore the style
        if (!config->OverlaysUseTheme.value_or_default())
            ImGui::PopStyleColor(5);
        else
            ImGui::PopStyleColor(2);

        // Get size for postioning
        overlaySize = ImGui::GetWindowSize();

        if (config->UseHQFont.value_or_default())
            ImGui::PopFontSize();

        ImGui::End();

        if (stylePushed)
            ImGui::PopStyleVar(7);

        // Left / Right
        if (config->FpsOverlayPosition.value_or_default() == FpsOverlayPos_TopLeft ||
            config->FpsOverlayPosition.value_or_default() == FpsOverlayPos_BottomLeft)
        {
            overlayPosition.x = 0;
        }
        else
        {
            overlayPosition.x = io.DisplaySize.x - overlaySize.x;
        }

        // Top / Bottom
        if (config->FpsOverlayPosition.value_or_default() == FpsOverlayPos_TopLeft ||
            config->FpsOverlayPosition.value_or_default() == FpsOverlayPos_TopRight)
        {
            overlayPosition.y = 0;
        }
        else
        {
            // Prevent overlapping with splash message
            if (!config->DisableSplash.value_or_default() && now > splashStart && now < splashLimit)
                overlayPosition.y = io.DisplaySize.y - overlaySize.y - splashSize.y;
            else
                overlayPosition.y = io.DisplaySize.y - overlaySize.y;
        }
    }
}

// Which upscaler files and hooks were found: for troubleshooting a game whose upscaler call does not arrive. Drawn
// inside a tooltip, by the header's lines (here and DlssNr::RenderHeaderBanner's offer).
static void UpscalerFileChecks()
{
    const auto& state = State::Instance();
    const auto primaryGpu = IdentifyGpu::getPrimaryGpu();

    if (primaryGpu.dlssCapable)
    {
        ImGui::Text("nvngx_dlss : %s", state.NVNGX_DLSS_Path.has_value() ? "Exists" : "Doesn't Exist");
        ImGui::SameLine(0.0f, 16.0f);
        ImGui::Text("nvngx_dlssd : %s", state.NVNGX_DLSSD_Path.has_value() ? "Exists" : "Doesn't Exist");
    }
    else
    {
        ImGui::Text("nvngx.dll: %s", state.nvngxExists ? "Exists" : "Doesn't Exist");
        ImGui::SameLine(0.0f, 16.0f);
        ImGui::Text("nvngx replacement: %s", state.nvngxReplacement.has_value() ? "Exists" : "Doesn't Exist");
    }

    ImGui::Text("libxess: %s", (state.libxessExists || XeSSProxy::Module() != nullptr) ? "Exists" : "Doesn't Exist");

    ImGui::Text("FSR Hooks: %s", state.fsrHooks ? "Exist" : "Don't Exist");
    ImGui::SameLine(0.0f, 16.0f);
    ImGui::Text("FSR 3.1: %s", FfxApiProxy::Dx12Module() != nullptr ? "Exists" : "Doesn't Exist");
    ImGui::SameLine(0.0f, 16.0f);
    ImGui::Text("FSR 3.1 SR: %s", FfxApiProxy::Dx12Module_SR() != nullptr ? "Exists" : "Doesn't Exist");
    ImGui::SameLine(0.0f, 16.0f);
    ImGui::Text("FSR 3.1 FG: %s", FfxApiProxy::Dx12Module_FG() != nullptr ? "Exists" : "Doesn't Exist");
}

void MenuCommon::RenderMainMenuHeaderMessages(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    auto& currentFeature = ctx.currentFeature;
    auto& menuResScale = ctx.menuResScale;
    auto& versionStatus = ctx.versionStatus;
    auto& currentVersionText = ctx.currentVersionText;
    auto& primaryGpu = *ctx.primaryGpu;

    if (!_showMipmapCalcWindow && !_showHudlessWindow && !ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow))
        ImGui::SetWindowFocus();

    if (config->MenuScale.has_value())
    {
        _selectedScale = ((int) (menuResScale * 10.0f)) - 4;
    }
    else
    {
        _selectedScale = 0;
    }

    if (versionStatus.completed)
    {
        if (versionStatus.updateAvailable && !versionStatus.latestTag.empty())
        {
            ImGui::Spacing();
            ImGui::TextColored(toneMapColor(ImVec4(1.f, 0.8f, 0.f, 1.f)), "Update available: %s (current %s)",
                               versionStatus.latestTag.c_str(), currentVersionText.c_str());

            if (!versionStatus.latestUrl.empty())
            {
                ImGui::SameLine();
                ImGui::TextLinkOpenURL("Open release page", versionStatus.latestUrl.c_str());
            }

            ImGui::Spacing();
        }
        else if (!versionStatus.error.empty())
        {
            // versionStatus is copied from State every frame, so clearing it here does not stop the
            // next frame from logging again: log each distinct error once.
            static std::string lastLoggedVersionError;
            if (versionStatus.error != lastLoggedVersionError)
            {
                LOG_ERROR("Version check failed: {0}", versionStatus.error);
                lastLoggedVersionError = versionStatus.error;
            }

            versionStatus.error.clear();
        }
        // Disabled error message
        // else if (!versionStatus.error.empty())
        //{
        //    ImGui::Spacing();
        //    ImGui::TextColored(toneMapColor(ImVec4(1.f, 0.4f, 0.f, 1.f)), "%s", versionStatus.error.c_str());
        //    ImGui::Spacing();
        //}
    }

    // Upscaler state is one line in every case, so the panes below never move when it changes.
    const bool upscalerFiles = state.nvngxExists || state.nvngxReplacement.has_value() ||
                               (state.libxessExists || XeSSProxy::Module() != nullptr);

    std::vector<std::string> upscalers;

    if (state.fsrHooks)
        upscalers.push_back("FSR");

    if (state.nvngxExists || state.nvngxReplacement.has_value() || primaryGpu.dlssCapable)
        upscalers.push_back("DLSS");

    if (state.libxessExists || XeSSProxy::Module() != nullptr)
        upscalers.push_back("XeSS");

    auto joined = upscalers | std::views::join_with(std::string { " or " });

    std::string joinedUpscalers(joined.begin(), joined.end());

    // Optical F5Low's lines (the native modes, menu/HeaderBanner.h) come first; the rule leaves a game's own feature,
    // and the cases with no offer to make, to the lines below.
    const bool f5lowLine = DlssNr::RenderHeaderBanner(
        config,
        currentFeature == nullptr || !currentFeature->IsInited() ? HeaderBanner::Feature::None
        : native::IsVirtualUpscalerFeature(currentFeature)       ? HeaderBanner::Feature::F5Low
        : currentFeature->IsFrozen()                             ? HeaderBanner::Feature::Frozen
                                                                 : HeaderBanner::Feature::Game,
        upscalerFiles, joinedUpscalers, currentFeature != nullptr ? currentFeature->Name() : std::string(),
        toneMapColor(ImVec4(1.f, 0.8f, 0.f, 1.f)), UpscalerFileChecks);

    if (f5lowLine)
    {
        // Drawn by DlssNr::RenderHeaderBanner.
    }
    else if (currentFeature == nullptr || !currentFeature->IsInited())
    {
        if (upscalerFiles)
        {
            ImGui::TextColored(toneMapColor(ImVec4(1.f, 0.8f, 0.f, 1.f)),
                               "Select %s as the game's upscaler and load a save to enable these settings.",
                               joinedUpscalers.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");

            // The file checks are for troubleshooting, so they wait behind the marker.
            if (ImGui::BeginItemTooltip())
            {
                ImGui::TextUnformatted("Upscalers don't always work in menus.");
                ImGui::Spacing();

                UpscalerFileChecks();

                ImGui::EndTooltip();
            }
        }
        else
        {
            ImGui::TextColored(toneMapColor(ImVec4(1.f, 0.4f, 0.35f, 1.f)),
                               "Can't find nvngx.dll, libxess.dll or FSR inputs. Upscaling support will NOT work.");
        }
    }
    else
    {
        // One line, reserved even when empty: frozen flips on and off in menus and loading, and the panes below
        // must not move with it.
        if (currentFeature->IsFrozen())
            ImGui::TextColored(toneMapColor(ImVec4(1.f, 0.8f, 0.f, 1.f)),
                               "%s is active, but not currently used by the game. Please enter the game.",
                               currentFeature->Name().c_str());
        else
            ImGui::NewLine();
    }
}

void MenuCommon::RenderActiveUpscalerSettings(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    auto& currentFeature = ctx.currentFeature;
    auto& menuResScale = ctx.menuResScale;
    auto& primaryGpu = *ctx.primaryGpu;

    if (currentFeature != nullptr && !currentFeature->IsFrozen())
    {
        if (native::IsVirtualUpscalerFeature(currentFeature))
            DlssNr::RenderF5LowUpscalerNote();

        // UPSCALERS -----------------------------
        ImGui::SeparatorText("Upscalers");
        ShowTooltip("Which copium do you choose?");

        GetCurrentBackendInfo(state.api, currentBackend, &currentBackendName);

        std::string spoofingText;

        ImGui::PushItemWidth(180.0f * menuResScale);

        const bool usesDlssd = currentFeature->GetUpscalerType() == Upscaler::DLSSD;
        const bool usesDx12CompatLayer = currentFeature->IsWithDx12();

        switch (state.api)
        {
        case DX11:
            ImGui::Text(primaryGpu.name.c_str());

            ImGui::Text("D3D11 %s| %s %d.%d.%d%s", primaryGpu.usesDxvk ? "(DXVK) " : "",
                        currentFeature->ShortName().c_str(), currentFeature->Version().major,
                        currentFeature->Version().minor, currentFeature->Version().patch,
                        usesDx12CompatLayer ? " w/Dx12" : "");
            ImGui::SameLine(0.0f, 6.0f);
            ImGui::Text("| Input: %s", ApiUpscalerInputName(state.currentInputApiName).c_str());

            ImGui::SameLine(0.0f, 6.0f);
            spoofingText = config->DxgiSpoofing.value_or_default() ? "On" : "Off";
            ImGui::Text("| Spoof: %s", spoofingText.c_str());

            if (!usesDlssd)
                AddDx11Backends(currentBackend);

            break;

        case DX12:
            ImGui::Text(primaryGpu.name.c_str());

            ImGui::Text("D3D12 %s| %s %d.%d.%d", primaryGpu.usesDxvk ? "(DXVK) " : "",
                        currentFeature->ShortName().c_str(), currentFeature->Version().major,
                        currentFeature->Version().minor, currentFeature->Version().patch);
            ImGui::SameLine(0.0f, 6.0f);
            ImGui::Text("| Input: %s", ApiUpscalerInputName(state.currentInputApiName).c_str());

            ImGui::SameLine(0.0f, 6.0f);
            spoofingText = config->DxgiSpoofing.value_or_default() ? "On" : "Off";
            ImGui::Text("| Spoof: %s", spoofingText.c_str());

            if (!usesDlssd)
                AddDx12Backends(currentBackend);

            break;

        default:
            ImGui::Text(primaryGpu.name.c_str());

            ImGui::Text("Vulkan %s| %s %d.%d.%d%s", primaryGpu.usesDxvk ? "(DXVK) " : "",
                        currentFeature->ShortName().c_str(), currentFeature->Version().major,
                        currentFeature->Version().minor, currentFeature->Version().patch,
                        usesDx12CompatLayer ? " w/Dx12" : "");
            ImGui::SameLine(0.0f, 6.0f);
            ImGui::Text("| Input: %s", ApiUpscalerInputName(state.currentInputApiName).c_str());

            auto vlkSpoof = config->VulkanSpoofing.value_or_default();
            auto vlkExtSpoof = config->VulkanExtensionSpoofing.value_or_default();

            if (vlkSpoof && vlkExtSpoof)
                spoofingText = "On + Ext";
            else if (vlkSpoof)
                spoofingText = "On";
            else if (vlkExtSpoof)
                spoofingText = "Just Ext";
            else
                spoofingText = "Off";

            ImGui::SameLine(0.0f, 6.0f);
            ImGui::Text("| Spoof: %s", spoofingText.c_str());

            if (!usesDlssd)
                AddVulkanBackends(currentBackend);
        }

        ImGui::PopItemWidth();

        if (!usesDlssd)
        {
            ImGui::SameLine(0.0f, 6.0f);

            if (ImGui::Button("Change Upscaler##2") && state.newBackend != Upscaler::Reset &&
                state.newBackend != currentBackend)
            {
                if (state.newBackend == Upscaler::XeSS)
                {
                    // Reseting them for xess
                    config->DisableReactiveMask.reset();
                    config->DlssReactiveMaskBias.reset();
                }

                MARK_ALL_BACKENDS_CHANGED();
            }
        }

        if (currentFeature->AccessToReactiveMask())
        {
            ImGui::BeginDisabled(config->DisableReactiveMask.value_or(false));

            auto useAsTransparency = config->FsrUseMaskForTransparency.value_or_default();
            if (ImGui::Checkbox("Use Reactive Mask as Transparency Mask", &useAsTransparency))
                config->FsrUseMaskForTransparency = useAsTransparency;

            ImGui::EndDisabled();
        }

        if (primaryGpu.dlssCapable && !state.NVNGX_DLSS_Path.has_value())
        {
            ImGui::Spacing();
            ImGui::TextColored(toneMapColor(ImVec4(1.f, 0.8f, 0.f, 1.f)), "nvngx_dlss.dll not found, DLSS disabled!");
        }
    }

    if (currentFeature != nullptr && !currentFeature->IsFrozen())
    {
        const bool usesDlssd = currentFeature->GetUpscalerType() == Upscaler::DLSSD;

        // Dx11 with Dx12
        if (state.api == DX11 && currentFeature->IsWithDx12())
        {
            ImGui::Spacing();
            if (auto ch = ScopedCollapsingHeader("Dx11 with Dx12 Settings"); ch.IsHeaderOpen())
            {
                ScopedIndent indent {};
                ImGui::Spacing();

                if (bool dontUseNTShared = config->DontUseNTShared.value_or_default();
                    ImGui::Checkbox("Don't Use NTShared", &dontUseNTShared))
                    config->DontUseNTShared = dontUseNTShared;

                ImGui::Spacing();
                ImGui::Spacing();
            }
        }

        if (state.api == Vulkan && currentFeature->IsWithDx12())
        {
            ImGui::Spacing();
            if (auto ch = ScopedCollapsingHeader("Vulkan with Dx12 Settings"); ch.IsHeaderOpen())
            {
                ScopedIndent indent {};
                ImGui::Spacing();

                if (bool inputsUseCopy = config->VulkanUseCopyForInputs.value_or_default();
                    ImGui::Checkbox("Use CopyResource for Inputs", &inputsUseCopy))
                    config->VulkanUseCopyForInputs = inputsUseCopy;

                if (bool outputUseCopy = config->VulkanUseCopyForOutput.value_or_default();
                    ImGui::Checkbox("Use CopyResource for Output", &outputUseCopy))
                    config->VulkanUseCopyForOutput = outputUseCopy;

                ImGui::Spacing();
                ImGui::Spacing();
            }
        }

        // UPSCALER SPECIFIC -----------------------------

        // XeSS -----------------------------
        if (currentBackend == Upscaler::XeSS && !usesDlssd)
        {
            ImGui::Spacing();
            if (auto ch = ScopedCollapsingHeader("XeSS Settings"); ch.IsHeaderOpen())
            {
                ScopedIndent indent {};
                ImGui::Spacing();

                const char* models[] = { "KPSS", "SPLAT", "MODEL_3", "MODEL_4", "MODEL_5", "MODEL_6" };
                auto configModes = config->NetworkModel.value_or_default();

                if (configModes < 0 || configModes > 5)
                    configModes = 0;

                const char* selectedModel = models[configModes];

                if (ImGui::BeginCombo("Network Models", selectedModel))
                {
                    for (int n = 0; n < 6; n++)
                    {
                        if (ImGui::Selectable(models[n], (config->NetworkModel.value_or_default() == n)))
                        {
                            config->NetworkModel = n;
                            state.newBackend = currentBackend;
                            MARK_ALL_BACKENDS_CHANGED();
                        }
                    }

                    ImGui::EndCombo();
                }
                ShowHelpMarker("Likely doesn't do much");

                if (bool dbg = state.xessDebug; ImGui::Checkbox("Dump (Shift+Del)", &dbg))
                    state.xessDebug = dbg;

                ImGui::SameLine(0.0f, 6.0f);
                int dbgCount = state.xessDebugFrames;

                ImGui::PushItemWidth(95.0f * menuResScale);
                if (ImGui::InputInt("frames", &dbgCount))
                {
                    if (dbgCount < 4)
                        dbgCount = 4;
                    else if (dbgCount > 999)
                        dbgCount = 999;

                    state.xessDebugFrames = dbgCount;
                }

                ImGui::PopItemWidth();

                ImGui::Spacing();
                ImGui::Spacing();
            }
        }

        // FFX -----------------
        if (!usesDlssd && (currentBackend == Upscaler::FFX || currentBackend == Upscaler::FFX_on12))
        {
            ImGui::SeparatorText("FFX Settings");

            if (_ffxUpscalerIndex < 0)
                _ffxUpscalerIndex = config->FfxUpscalerIndex.value_or_default();

            if (currentBackend == Upscaler::FFX ||
                currentBackend == Upscaler::FFX_on12 && state.ffxUpscalerVersionNames.size() > 0)
            {
                ImGui::PushItemWidth(135.0f * menuResScale);

                auto currentName = StrFmt("FSR %s", state.ffxUpscalerVersionNames[_ffxUpscalerIndex]);
                if (ImGui::BeginCombo("FFX Upscaler", currentName.c_str()))
                {
                    for (int n = 0; n < state.ffxUpscalerVersionIds.size(); n++)
                    {
                        auto name = StrFmt("FSR %s##%d", state.ffxUpscalerVersionNames[n], n);
                        if (ImGui::Selectable(name.c_str(), config->FfxUpscalerIndex.value_or_default() == n))
                            _ffxUpscalerIndex = n;
                    }

                    ImGui::EndCombo();
                }
                ImGui::PopItemWidth();

                ShowHelpMarker("List of upscalers reported by FFX SDK");

                ImGui::SameLine(0.0f, 6.0f);

                if (ImGui::Button("Change Upscaler") &&
                    _ffxUpscalerIndex != config->FfxUpscalerIndex.value_or_default())
                {
                    config->FfxUpscalerIndex = _ffxUpscalerIndex;
                    state.newBackend = currentBackend;
                    MARK_ALL_BACKENDS_CHANGED();
                }

                auto majorFsrVersion = currentFeature->Version().major;

                if (majorFsrVersion >= 4)
                {
                    ImGui::Spacing();

                    // Colorspaces
                    const char* colorSpaces[] = { "Linear (Default)", "Non-Linear", "Non-Linear sRGB",
                                                  "Non-Linear PQ" };
                    int currentColorSpace = 0;
                    if (config->FsrNonLinearPQ.value_or_default())
                        currentColorSpace = 3;
                    else if (config->FsrNonLinearSRGB.value_or_default())
                        currentColorSpace = 2;
                    else if (config->FsrNonLinearColorSpace.value_or_default())
                        currentColorSpace = 1;

                    ImGui::SetNextItemWidth(150.0f * menuResScale);
                    if (ImGui::Combo("Input Color Space", &currentColorSpace, colorSpaces, IM_ARRAYSIZE(colorSpaces)))
                    {
                        bool isSrgb = (currentColorSpace == 2);
                        bool isPq = (currentColorSpace == 3);

                        config->FsrNonLinearSRGB = isSrgb;
                        config->FsrNonLinearPQ = isPq;

                        if (isSrgb || isPq)
                        {
                            config->FsrNonLinearColorSpace.set_volatile_value(true);
                        }
                        else if (currentColorSpace == 1) // Just non-Linear
                        {
                            config->FsrNonLinearColorSpace = true;
                        }
                        else // Linear
                        {
                            config->FsrNonLinearColorSpace = false;
                        }

                        state.newBackend = currentBackend;
                        MARK_ALL_BACKENDS_CHANGED();
                    }
                    ShowHelpMarker("Select the input color space that the game uses.\n"
                                   "Non-Linear / sRGB: Might improve FSR4 upscaling quality, might increase ghosting.\n"
                                   "PQ: Rarest, might increase ghosting and break lights.");

                    // FSR 4 Presets
                    const char* presets[] = { "Default",  "Preset 0", "Preset 1", "Preset 2",
                                              "Preset 3", "Preset 4", "Preset 5" };
                    int currentPresetIdx = config->Fsr4Preset.has_value() ? config->Fsr4Preset.value() + 1 : 0;

                    if (currentPresetIdx < 0 || currentPresetIdx >= IM_ARRAYSIZE(presets))
                        currentPresetIdx = 0;

                    ImGui::SetNextItemWidth(150.0f * menuResScale);
                    if (ImGui::Combo("FSR4 Preset", &currentPresetIdx, presets, IM_ARRAYSIZE(presets)))
                    {
                        if (currentPresetIdx == 0)
                            config->Fsr4Preset.reset();
                        else
                            config->Fsr4Preset = currentPresetIdx - 1;

                        state.newBackend = currentBackend;
                        MARK_ALL_BACKENDS_CHANGED();
                    }
                    ShowHelpMarker("Each internal FSR4 preset is tuned for a specific resolution.\n"
                                   "Selecting an FSR4 preset won't change the in-game\nupscaler preset!!!\n\n"
                                   "Preset 0 is meant for FSR Native AA\n"
                                   "Preset 1 is meant for Quality/Ultra Quality\n"
                                   "Preset 2 is meant for Balanced\n"
                                   "Preset 3 is meant for Performance\n"
                                   "Preset 4 is meant for DRS\n"
                                   "Preset 5 is meant for Ultra Performance");

                    // Display the active preset right next to the combo box instead of using a table
                    ImGui::SameLine();
                    if (state.currentFsr4Preset.has_value())
                        ImGui::TextDisabled("(Active: %d)", state.currentFsr4Preset.value());
                    else if (FSR4Presets::IsAnyModuleHooked())
                        ImGui::TextColored(toneMapColor(ImVec4(1.f, 0.8f, 0.f, 1.f)), "(Potential FSR3 fallback)");
                    else
                        ImGui::TextDisabled("(Failed to hook)");

                    // FSR 4 models of the module running the selected upscaler, only listed when the GPU can run more
                    // than one of them
                    HMODULE modelModule = nullptr;
                    const auto ffxIndex = config->FfxUpscalerIndex.value_or_default();
                    // The version list is shared with Vulkan and DX11, only the DX12 ones come from the driver provider
                    const bool dx12Upscaler = (state.api == API::DX12 && currentBackend == Upscaler::FFX) ||
                                              currentBackend == Upscaler::FFX_on12;

                    if (dx12Upscaler && ffxIndex >= 0 && ffxIndex < state.ffxUpscalerVersionIds.size())
                    {
                        modelModule = FfxApiProxy::UpscalerModule(state.ffxUpscalerVersionIds[ffxIndex]);
                    }
                    else if (state.api == API::Vulkan && currentBackend == Upscaler::FFX &&
                             FSR4DriverProviderVk::IsAvailable() && ffxIndex >= 0 &&
                             ffxIndex < state.ffxUpscalerVersionIds.size() &&
                             state.ffxUpscalerVersionIds[ffxIndex] == FSR4DriverProviderVk::VersionId())
                    {
                        modelModule = FSR4DriverProviderVk::Module()->dll;
                    }

                    if (auto models = FSR4Models::SelectableModels(modelModule); !models.empty())
                    {
                        auto& selectedModel = config->Fsr4Model;
                        auto selectedName =
                            selectedModel.has_value() ? FSR4Models::Name(selectedModel.value()) : "Auto";
                        bool modelChanged = false;

                        ImGui::SetNextItemWidth(150.0f * menuResScale);
                        if (ImGui::BeginCombo("FSR4 Model", selectedName))
                        {
                            if (ImGui::Selectable("Auto", !selectedModel.has_value()) && selectedModel.has_value())
                            {
                                selectedModel.reset();
                                modelChanged = true;
                            }

                            for (auto model : models)
                            {
                                bool isSelected = selectedModel.has_value() && selectedModel.value() == model;

                                if (ImGui::Selectable(FSR4Models::Name(model), isSelected) && !isSelected)
                                {
                                    selectedModel = model;
                                    modelChanged = true;
                                }
                            }

                            ImGui::EndCombo();
                        }

                        if (modelChanged)
                        {
                            state.newBackend = currentBackend;
                            MARK_ALL_BACKENDS_CHANGED();
                        }

                        ShowHelpMarker("FSR 4 network to run.\n"
                                       "Auto lets FFX pick the best one the GPU supports.");

                        ImGui::SameLine();
                        if (auto activeModel = FSR4Models::ActiveModel(modelModule))
                            ImGui::TextDisabled("(Active: %s)", FSR4Models::Name(*activeModel));
                        else
                            ImGui::TextColored(toneMapColor(ImVec4(1.f, 0.8f, 0.f, 1.f)), "(Potential FSR3 fallback)");
                    }
                }

                if (majorFsrVersion >= 3)
                {
                    ImGui::Spacing();

                    bool debugView = config->FsrDebugView.value_or_default();
                    if (ImGui::Checkbox("Upscaler Debug View", &debugView))
                    {
                        config->FsrDebugView = debugView;

                        // FSR 4's debug view requires backend reinit
                        if (majorFsrVersion > 3)
                        {
                            state.newBackend = currentBackend;
                            MARK_ALL_BACKENDS_CHANGED();
                        }
                    }

                    if (majorFsrVersion > 3)
                    {
                        ShowHelpMarker("Top left: Dilated Motion Vectors\n"
                                       "Top right: Predicted Blend Factor");
                    }
                    else
                    {
                        ShowHelpMarker("Top left: Dilated Motion Vectors\n"
                                       "Top middle: Protected Areas\n"
                                       "Top right: Dilated Depth\n"
                                       "Middle: Upscaled frame\n"
                                       "Bottom left: Disocclusion mask\n"
                                       "Bottom middle: Reactiveness\n"
                                       "Bottom right: Detail Protection Takedown");
                    }

                    if (majorFsrVersion > 3)
                    {
                        ImGui::SameLine(0.0f, 20.0f * menuResScale);
                        bool fsr4wm = config->Fsr4EnableWatermark.value_or_default();
                        if (ImGui::Checkbox("Watermark", &fsr4wm))
                        {
                            LOG_DEBUG("FSR4 Watermark set to {}", fsr4wm);
                            config->Fsr4EnableWatermark = fsr4wm;
                            Util::SetFlagEnvVar(L"MLSR-WATERMARK", fsr4wm);

                            // The driver reads the variable on context creation
                            state.newBackend = currentBackend;
                            MARK_ALL_BACKENDS_CHANGED();
                        }
                    }
                }

                if (currentFeature->Version() >= feature_version { 3, 1, 1 } &&
                    currentFeature->Version() < feature_version { 4, 0, 0 })
                {
                    ImGui::Spacing();

                    if (currentFeature != nullptr)
                    {
                        ImGui::Text("FSR 3.1 Presets:");

                        ImGui::SameLine(0.0f, 6.0f);

                        // This will be applied by default
                        if (ImGui::Button("Stability"))
                        {
                            auto const scaleRatioX =
                                (float) currentFeature->TargetWidth() / (float) currentFeature->RenderWidth();
                            auto const scaleRatioY =
                                (float) currentFeature->TargetHeight() / (float) currentFeature->RenderHeight();
                            auto const scaleRatio = std::max(scaleRatioX, scaleRatioY);

                            config->FsrVelocity = 0.5f;
                            config->FsrReactiveScale = 0.25f;

                            config->FsrShadingScale.reset();
                            config->FsrAccAddPerFrame.reset();
                            config->FsrMinDisOccAcc.reset();
                            config->FsrShadingScale.set_volatile_value(0.5f / scaleRatio);
                            config->FsrAccAddPerFrame.set_volatile_value(scaleRatio / 10.0f);
                            config->FsrMinDisOccAcc.set_volatile_value(scaleRatio / 20.0f);
                        }

                        ImGui::SameLine(0.0f, 6.0f);

                        if (ImGui::Button("Motion"))
                        {
                            auto const scaleRatioX =
                                (float) currentFeature->TargetWidth() / (float) currentFeature->RenderWidth();
                            auto const scaleRatioY =
                                (float) currentFeature->TargetHeight() / (float) currentFeature->RenderHeight();
                            auto const scaleRatio = std::max(scaleRatioX, scaleRatioY);

                            config->FsrVelocity = 1.0f;
                            config->FsrReactiveScale = 0.5f;

                            config->FsrShadingScale.reset();
                            config->FsrAccAddPerFrame.reset();
                            config->FsrMinDisOccAcc.reset();
                            config->FsrShadingScale.set_volatile_value(1.0f / scaleRatio);
                            config->FsrAccAddPerFrame.set_volatile_value(scaleRatio / 10.0f);
                            config->FsrMinDisOccAcc.set_volatile_value(scaleRatio / 20.0f);
                        }

                        ImGui::SameLine(0.0f, 6.0f);

                        if (ImGui::Button("Default"))
                        {
                            config->FsrVelocity = 1.0f;
                            config->FsrReactiveScale = 1.0f;
                            config->FsrShadingScale = 1.0f;
                            config->FsrAccAddPerFrame = 0.333f;
                            config->FsrMinDisOccAcc = -0.333f;
                        }
                    }

                    ImGui::Spacing();

                    if (auto ch = ScopedCollapsingHeader("FSR 3 Upscaler Manual Tuning"); ch.IsHeaderOpen())
                    {
                        ScopedIndent indent {};
                        ImGui::Spacing();
                        ImGui::Spacing();

                        ImGui::PushItemWidth(220.0f * menuResScale);

                        float velocity = config->FsrVelocity.value_or_default();
                        if (ImGui::SliderFloat("Velocity Factor", &velocity, 0.00f, 1.0f, "%.2f"))
                            config->FsrVelocity = velocity;

                        ShowHelpMarker("Value of 0.0f can improve temporal stability of bright pixels\n"
                                       "Lower values are more stable with ghosting\n"
                                       "Higher values are more pixelly, but less ghosting");

                        if (currentFeature->Version() >= feature_version { 3, 1, 4 })
                        {
                            // Reactive Scale
                            float reactiveScale = config->FsrReactiveScale.value_or_default();
                            if (ImGui::SliderFloat("Reactive Scale", &reactiveScale, 0.0f, 1.0f, "%.3f"))
                                config->FsrReactiveScale = reactiveScale;

                            ShowHelpMarker("Meant for development purpose to test if\n"
                                           "writing a larger value to reactive mask, reduces ghosting.");

                            // Shading Scale
                            float shadingScale = config->FsrShadingScale.value_or_default();
                            if (ImGui::SliderFloat("Shading Scale", &shadingScale, 0.0f, 1.0f, "%.3f"))
                                config->FsrShadingScale = shadingScale;

                            ShowHelpMarker("Increasing this scales FSR3.1 computed shading\n"
                                           "change value at read to have higher reactiveness.");

                            // Accumulation Added Per Frame
                            float accAddPerFrame = config->FsrAccAddPerFrame.value_or_default();
                            if (ImGui::SliderFloat("Acc. Added Per Frame", &accAddPerFrame, 0.0f, 1.0f, "%.3f"))
                                config->FsrAccAddPerFrame = accAddPerFrame;

                            ShowHelpMarker("Corresponds to amount of accumulation added per frame\n"
                                           "at pixel coordinate where disocclusion occured or when\n"
                                           "reactive mask value is > 0.0f. Decreasing this and \n"
                                           "drawing the ghosting object (IE no mv) to reactive mask \n"
                                           "with value close to 1.0f can decrease temporal ghosting.\n"
                                           "Decreasing this could result in more thin feature pixels flickering.");

                            // Min Disocclusion Accumulation
                            float minDisOccAcc = config->FsrMinDisOccAcc.value_or_default();
                            if (ImGui::SliderFloat("Min. Disocclusion Acc.", &minDisOccAcc, -1.0f, 1.0f, "%.3f"))
                                config->FsrMinDisOccAcc = minDisOccAcc;

                            ShowHelpMarker("Increasing this value may reduce white pixel temporal\n"
                                           "flickering around swaying thin objects that are disoccluding \n"
                                           "one another often. Too high value may increase ghosting.");
                        }

                        ImGui::PopItemWidth();

                        ImGui::Spacing();
                        ImGui::Spacing();
                    }
                }
            }
        }

        // DLSS -----------------
        if ((config->DLSSEnabled.value_or_default() && currentBackend == Upscaler::DLSS &&
             currentFeature->Version().major > 2) ||
            usesDlssd)
        {

            if (usesDlssd)
                ImGui::SeparatorText("DLSSD Settings");
            else
                ImGui::SeparatorText("DLSS Settings");

            auto overridden =
                usesDlssd ? state.dlssdPresetsOverriddenExternally : state.dlssPresetsOverriddenExternally;

            if (overridden)
            {
                ImGui::TextColored(toneMapColor(ImVec4(1.f, 0.8f, 0.f, 1.f)), "Presets are overridden externally");
                ShowHelpMarker("This usually happens due to using tools\n"
                               "such as Nvidia App or Nvidia Inspector");
                // ImGui::Text("Selecting setting below will disable that external override\n"
                //             "but you need to Save Settings and restart the game");

                ImGui::Spacing();
            }

            if (usesDlssd)
            {
                if (bool pOverride = config->DLSSDRenderPresetOverride.value_or_default();
                    ImGui::Checkbox("Render Presets Override", &pOverride))
                    config->DLSSDRenderPresetOverride = pOverride;

                ShowHelpMarker("Each render preset has it strengths and weaknesses\n"
                               "Override to potentially improve image quality\n"
                               "Press apply after enable/disable");

                /*
                auto currentPresetIndex = GetPresetIndex(currentFeature, true);

                if (currentPresetIndex == 0)
                    ImGui::Text("Current Preset: Default");
                else
                    ImGui::Text("Current Preset: %c", 64 + currentPresetIndex);
                */

                ImGui::BeginDisabled(!config->DLSSDRenderPresetOverride.value_or_default() /*|| overridden*/);
                ImGui::PushItemWidth(135.0f * menuResScale);

                AddDLSSDRenderPreset("Override Preset", &comboPreset);

                ImGui::PopItemWidth();
                ImGui::EndDisabled();
            }
            else
            {
                if (bool pOverride = config->RenderPresetOverride.value_or_default();
                    ImGui::Checkbox("Render Presets Override", &pOverride))
                    config->RenderPresetOverride = pOverride;

                ShowHelpMarker("Each render preset has it strengths and weaknesses\n"
                               "Override to potentially improve image quality\n"
                               "Press Apply after enable/disable");

                /*
                auto currentPresetIndex = GetPresetIndex(currentFeature, false);

                if (currentPresetIndex == 0)
                    ImGui::Text("Current Preset: Default");
                else
                    ImGui::Text("Current Preset: %c", 64 + currentPresetIndex);
                */

                ImGui::BeginDisabled(!config->RenderPresetOverride.value_or_default() /*|| overridden*/);

                ImGui::PushItemWidth(135.0f * menuResScale);

                AddDLSSRenderPreset("Override Preset", &comboPreset);

                ImGui::PopItemWidth();
                ImGui::EndDisabled();
            }

            ImGui::SameLine(0.0f, 6.0f);

            if (ImGui::Button("Apply Changes"))
            {
                LOG_DEBUG("Applying DLSS/DLSSD preset override changes, preset index: {}",
                          comboPreset.value_or_default());

                if (usesDlssd)
                {
                    config->DLSSDRenderPresetForAll = comboPreset.value_or_default();
                    state.newBackend = Upscaler::DLSSD;
                }
                else
                {
                    config->RenderPresetForAll = comboPreset.value_or_default();
                    state.newBackend = currentBackend;
                }

                MARK_ALL_BACKENDS_CHANGED();
            }

            ImGui::Spacing();

            if (auto ch = ScopedCollapsingHeader(usesDlssd ? "Advanced DLSSD Settings" : "Advanced DLSS Settings");
                ch.IsHeaderOpen())
            {
                ScopedIndent indent {};
                ImGui::Spacing();

                bool appIdOverride = config->UseGenericAppIdWithDlss.value_or_default();
                if (ImGui::Checkbox("Use Generic App Id with DLSS", &appIdOverride))
                    config->UseGenericAppIdWithDlss = appIdOverride;

                ShowHelpMarker("Use generic appid with NGX\n"
                               "Fixes OptiScaler preset override not working with certain games\n"
                               "Requires a game restart");

                ImGui::BeginDisabled(!config->RenderPresetOverride.value_or_default() || overridden);
                ImGui::Spacing();
                ImGui::PushItemWidth(135.0f * menuResScale);

                if (usesDlssd)
                {
                    AddDLSSDRenderPreset("DLAA Preset", &config->DLSSDRenderPresetDLAA);
                    AddDLSSDRenderPreset("UltraQ Preset", &config->DLSSDRenderPresetUltraQuality);
                    AddDLSSDRenderPreset("Quality Preset", &config->DLSSDRenderPresetQuality);
                    AddDLSSDRenderPreset("Balanced Preset", &config->DLSSDRenderPresetBalanced);
                    AddDLSSDRenderPreset("Perf Preset", &config->DLSSDRenderPresetPerformance);
                    AddDLSSDRenderPreset("UltraP Preset", &config->DLSSDRenderPresetUltraPerformance);
                }
                else
                {
                    AddDLSSRenderPreset("DLAA Preset", &config->RenderPresetDLAA);
                    AddDLSSRenderPreset("UltraQ Preset", &config->RenderPresetUltraQuality);
                    AddDLSSRenderPreset("Quality Preset", &config->RenderPresetQuality);
                    AddDLSSRenderPreset("Balanced Preset", &config->RenderPresetBalanced);
                    AddDLSSRenderPreset("Perf Preset", &config->RenderPresetPerformance);
                    AddDLSSRenderPreset("UltraP Preset", &config->RenderPresetUltraPerformance);
                }
                ImGui::PopItemWidth();
                ImGui::EndDisabled();

                ImGui::Spacing();
                ImGui::Spacing();
            }
        }
    }
}

// One line under "Override DLSSG Ratio". The combo says what was requested; this says whether it
// happened: what the game asked for, what was sent on after any override, and what Streamline reports it
// presented at the last check. Nothing is drawn while DLSS-G is off.
static void RenderDlssgTelemetry()
{
    const auto& telemetry = DlssgTelemetry::Get();

    if (!telemetry.optionsSeen.load(std::memory_order_acquire) || !telemetry.active.load(std::memory_order_relaxed))
        return;

    if (!telemetry.stateSeen.load(std::memory_order_acquire))
    {
        ImGui::TextDisabled("DLSSG: waiting for Streamline state...");
        return;
    }

    const unsigned int requestedX = telemetry.requested.load(std::memory_order_relaxed) + 1;
    const unsigned int sentX = telemetry.sent.load(std::memory_order_relaxed) + 1;
    const unsigned int presented = telemetry.presented.load(std::memory_order_relaxed);
    const unsigned int result = telemetry.result.load(std::memory_order_relaxed);

    const bool agrees = result == 0 && presented == sentX;
    const ImVec4 green(0.4f, 0.9f, 0.5f, 1.0f);
    const ImVec4 amber(0.95f, 0.70f, 0.20f, 1.0f);

    ImGui::TextColored(agrees ? green : amber, "Game asked %uX, sent %uX, Streamline presented %u (max seen %u)",
                       requestedX, sentX, presented, telemetry.maxPresented.load(std::memory_order_relaxed));

    if (result != 0)
        ImGui::TextColored(amber, "slDLSSGSetOptions returned sl::Result %u for that request.", result);
}

void MenuCommon::RenderFrameGenerationSelection(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    DlssNr::RenderF5LowFrameGenHint(config, ctx.currentFeature == nullptr || !ctx.currentFeature->IsInited());

    auto& menuResScale = ctx.menuResScale;
    auto& primaryGpu = *ctx.primaryGpu;

    // A Vulkan game gets OptiFG and the outputs through the present bridge (native/VkPresentBridge.h), which is made
    // when the game starts: they are offered while it is up, and before it while Optical F5Low or the Vulkan route
    // (bridge, game's upscaler) can make one (choose, save, restart).
    const bool vulkanNoFg =
        state.swapchainApi == API::Vulkan && state.swapchainInteropApi != SwapchainInteropApi::VkwDx12 &&
        (IdentifyGpu::getPrimaryGpu().usesDxvk ||
         !(config->DlssNrEnabled.value_or_default() || config->DlssNrNativeFrameGenerationOnly.value_or_default()));
    // A native Vulkan game (not through dxvk) can have the FSR and DLSSG outputs on FFX's and Streamline's Vulkan swapchain
    // (framegen/ffx/FSRFG_Vk.h, framegen/dlssg/DLSSG_Vk.h), fed by OptiFG or the game's own DLSSG, without the bridge.
    const bool vulkanNativeFg = state.swapchainApi == API::Vulkan && !IdentifyGpu::getPrimaryGpu().usesDxvk;
    const bool vulkanOutputBlocked = vulkanNoFg && !vulkanNativeFg;

    /// FG INPUTS
    if (state.externalFrameGeneration)
    {
        ImGui::TextWrapped("External FG is active. Set the multiplier in the game or unlocker, not OptiScaler.\n"
                           "Switch it off under Unlock & external FG at the end of this page.");
        return;
    }

    DlssNr::RenderVulkanRoute(config);

    static std::vector<MenuOption<FGInput>> inputOptions;
    inputOptions.clear();

    // clang-format off

    inputOptions = {
        { FGInput::NoFG, "None" },
        { FGInput::Upscaler, "OptiFG (Upscaler)",
            "Upscaler must be enabled\n\nCan be used with any FG Output, but might be imperfect with some\nTo prevent UI glitching, HUDfix required" },
        { FGInput::DLSSG, "DLSSG via Streamline",
            "Can be used with any FG Output\n\nRequires enabling DLSS-FG in game settings\nSupports HUDless out of the box\n\nLimited to games that use Streamline" },
        { FGInput::NvngxFG, "DLSSG via Nvngx",
            "Limited to variants of FSR FG\n\nRequires enabling DLSS-FG in game settings\nSupports HUDless out of the box\nUses Streamline swapchain for pacing" },
        { FGInput::FSRFG, "FSR 3.1 FG",
            "Can be used with any FG Output\n\nRequires enabling FSR-FG in game settings\nSupports HUDless out of the box" },
        { FGInput::FSRFG30, "FSR 3.0 FG",
            "Can be used with any FG Output\n\nRequires enabling FSR-FG in game settings\nSupports HUDless out of the box" },
        { FGInput::XeFG, "XeFG" }
    };

    // clang-format on

    auto constexpr nvngxInputIndex = (uint32_t) FGInput::NvngxFG;

    // XeFG input requirements
    auto constexpr xefgInputIndex = (uint32_t) FGInput::XeFG;
    inputOptions[xefgInputIndex].set_disabled(true, "Support not implemented, they meant FG Output");

    // OptiFG requirements
    auto constexpr optiFgIndex = (uint32_t) FGInput::Upscaler;
    inputOptions[optiFgIndex].set_disabled(vulkanOutputBlocked, "Unsupported API");

    // The native Vulkan FG output reads the upscaler's inputs; FSR FG's Vulkan dll is loaded with the device
    if (state.swapchainApi != API::Vulkan && !inputOptions[optiFgIndex].disabled &&
        state.activeFgOutput == FGOutput::FSRFG && !FfxApiProxy::IsFGReady() && !ffxInitTried)
    {
        ffxInitTried = true;
        FfxApiProxy::InitFfxDx12();
        inputOptions[optiFgIndex].set_disabled(!FfxApiProxy::IsFGReady(), "amd_fidelityfx_dx12.dll is missing");
    }
    else if (!inputOptions[optiFgIndex].disabled && UsesXeFGApi(state.activeFgOutput) && !xefgInitTried &&
             XeFGProxy::Module() == nullptr)
    {
        xefgInitTried = true;
        XeFGProxy::InitXeFG();
        inputOptions[optiFgIndex].set_disabled(XeFGProxy::Module() == nullptr,
                                               state.activeFgOutput == FGOutput::F5Low
                                                   ? "OptiScaler\\plugins\\f5low.dll could not be loaded"
                                                   : "libxess_fg.dll is missing");
    }

    // DLSSG inputs requirements
    auto constexpr dlssgInputIndex = (uint32_t) FGInput::DLSSG;
    // inputOptions[dlssgInputIndex].set_disabled(state.streamlineVersion.major == 0, "Game doesn't use streamline");
    inputOptions[dlssgInputIndex].set_disabled(state.swapchainApi == API::DX11, "Unsupported API");

    // FSRFG inputs requirements
    auto constexpr fsrfgInputIndex = (uint32_t) FGInput::FSRFG;
    inputOptions[fsrfgInputIndex].set_disabled(state.swapchainApi != API::DX12, "Unsupported API");

    // FSRFG30 inputs requirements
    auto constexpr fsrfg30InputIndex = (uint32_t) FGInput::FSRFG30;
    inputOptions[fsrfg30InputIndex].set_disabled(state.swapchainApi != API::DX12, "Unsupported API");

    if (!config->FGInput.has_value())
        config->FGInput = config->FGInput.value_or_default(); // need to have a value before combo

    /// FG OUTPUTS

    static std::vector<MenuOption<FGOutput>> outputOptions;
    outputOptions.clear();

    // clang-format off

    outputOptions = {
        { FGOutput::NoFG, "None" },
        { FGOutput::FSRFG, "FSR FG", "FSR3/4-FG, RDNA4 autoupgrades to FSR4-FG\n\nFSR4-FG sometimes better/worse than XeFG" },
        { FGOutput::DLSSG, "DLSSG", "DLSSG output\ncan be used in conjuction with Nukem's for example" },
        { FGOutput::XeFG, "XeFG", "XeFG - heaviest, but best universal FG\n\nXeFG 3 overall deals best with HUD\n\nEnable UI Composition if HUD ghosting" },
        { FGOutput::F5Low, "F5Low", "F5Low frame generation - our own, up to 6x\n\nSame options as XeFG, plus the Frame Generator\nthat makes the frames (XeFG Tensor: NVIDIA tensor cores)" },
    };

    // clang-format on

    // DLSSG output requirements
    auto constexpr dlssgOutputIndex = (uint32_t) FGOutput::DLSSG;
    const bool supportsDlssg = primaryGpu.nvidiaArchInfo.architecture_id >= NV_GPU_ARCHITECTURE_AD100;
    const bool hasDlssgReplacement =
        state.nukemsFgFileAvailable || state.artursFgFileAvailable || FfxApiProxy::IsFGReady(false);

    if (!supportsDlssg && hasDlssgReplacement)
    {
        outputOptions[dlssgOutputIndex].tooltip =
            "No real DLSSG, unsupported hardware\nOnly Nvngx FG replacements available";
    }

    outputOptions[dlssgOutputIndex].set_disabled(vulkanOutputBlocked, "Unsupported API");
    outputOptions[dlssgOutputIndex].set_disabled(!supportsDlssg && !hasDlssgReplacement,
                                                 "Unsupported hardware and no replacements");

    // For that one case of DX11 DLSSG
    const auto streamlineVersion = state.streamlineVersion;
    const bool nukemsUnsupportedApi =
        state.swapchainApi == API::DX11 &&
        (streamlineVersion == feature_version { 0, 0, 0 } || streamlineVersion > feature_version { 2, 0, 1 });
    inputOptions[nvngxInputIndex].set_disabled(nukemsUnsupportedApi, "Unsupported API");

    // FSR FG output requirements
    auto constexpr fsrfgOutputIndex = (uint32_t) FGOutput::FSRFG;
    outputOptions[fsrfgOutputIndex].set_disabled(vulkanOutputBlocked, "Unsupported API");

    // XeFG output requirements
    auto constexpr xefgOutputIndex = (uint32_t) FGOutput::XeFG;
    outputOptions[xefgOutputIndex].set_disabled(vulkanNoFg, state.swapchainApi == API::Vulkan
                                                                ? "Needs the bridge: Vulkan route or Optical F5Low"
                                                                : "Unsupported API");

    // F5Low output requirements: the XeFG ones, plus an NVIDIA GPU and f5low.dll in the plugins folder
    auto constexpr f5lowOutputIndex = (uint32_t) FGOutput::F5Low;
    static const auto f5lowAvailability = F5Low::Available();
    if (outputOptions[xefgOutputIndex].disabled)
        outputOptions[f5lowOutputIndex].set_disabled(true, outputOptions[xefgOutputIndex].tooltip);
    else
        outputOptions[f5lowOutputIndex].set_disabled(!f5lowAvailability.available, f5lowAvailability.reason);
    // Unsupported FG input selected
    const auto currentInputIndex = (uint32_t) state.activeFgInput;
    if (config->FGInput != FGInput::NoFG && inputOptions.size() > currentInputIndex &&
        inputOptions[currentInputIndex].disabled && state.activeFgInput == config->FGInput)
    {
        LOG_WARN("Resetting FGInput to NoFG: {}", inputOptions[currentInputIndex].label);
        config->FGInput = FGInput::NoFG;

        // Changing active can be dangerous but we are talking about an unsupported mode
        // which shouldn't even actually have taken affect
        state.activeFgInput = FGInput::NoFG;
    }

    // Unsupported FG output selected
    const auto currentOutputIndex = (uint32_t) state.activeFgOutput;
    if (config->FGOutput != FGOutput::NoFG && outputOptions.size() > currentOutputIndex &&
        outputOptions[currentOutputIndex].disabled && state.activeFgOutput == config->FGOutput)
    {
        LOG_WARN("Resetting FGOutput to NoFG: {}", outputOptions[currentOutputIndex].label);
        config->FGOutput = FGOutput::NoFG;
        state.activeFgOutput = FGOutput::NoFG;
    }

    if (!config->FGOutput.has_value())
        config->FGOutput = config->FGOutput.value_or_default(); // need to have a value before combo

    /// FG NVNGX REPLACEMENT

    static std::vector<MenuOption<FGNvngxReplacement>> nvngxOptions;
    nvngxOptions.clear();

    // clang-format off

    nvngxOptions = {
        { FGNvngxReplacement::None, "None (Real DLSSG)", "Real DLSSG, For RTX 40xx and above"},
        { FGNvngxReplacement::Nukems, "Nukem's", "FSR 3 FG" },
        { FGNvngxReplacement::Arturs, "Enabler", "FSR 3 MFG" },
        { FGNvngxReplacement::FFX, "FSR 3/4 FG", "FSR 3/4 FG using the FFX" },
        { FGNvngxReplacement::Combo, "FFX + Enabler", "FFX for the middle fake frame, Enabler for the rest\n\n"
                                                      "2x - FFX\n3x - Enabler\n4x - FFX + Enabler\n5x - Enabler\n6x - FFX + Enabler" },
    };

    // clang-format on

    bool replaceFgOutputWithNvngx = false;
    bool showNvngxFgDowndown = false;

    if (config->FGInput == FGInput::NvngxFG)
    {
        config->FGOutput = FGOutput::NoFG;
        replaceFgOutputWithNvngx = true;
    }
    else if (config->FGOutput == FGOutput::DLSSG)
    {
        showNvngxFgDowndown = true;
    }

    auto constexpr fgNvngxNoneIndex = (uint32_t) FGNvngxReplacement::None;
    nvngxOptions[fgNvngxNoneIndex].set_disabled(!supportsDlssg, "Unsupported hardware");

    if (replaceFgOutputWithNvngx)
    {
        nvngxOptions[fgNvngxNoneIndex].label = "None";
        nvngxOptions[fgNvngxNoneIndex].set_hidden(true);
    }

    auto constexpr fgNvngxNukemsIndex = (uint32_t) FGNvngxReplacement::Nukems;
    nvngxOptions[fgNvngxNukemsIndex].set_disabled(!state.nukemsFgFileAvailable,
                                                  "Missing dlssg_to_fsr3_amd_is_better.dll");

    auto constexpr fgNvngxArtursIndex = (uint32_t) FGNvngxReplacement::Arturs;
    nvngxOptions[fgNvngxArtursIndex].set_disabled(!state.artursFgFileAvailable, "Missing dlss-enabler-headless.dll");

    auto constexpr fgNvngxFfxIndex = (uint32_t) FGNvngxReplacement::FFX;
    nvngxOptions[fgNvngxFfxIndex].set_disabled(!FfxApiProxy::IsFGReady(false),
                                               "Missing amd_fidelityfx_framegeneration_dx12.dll");

    auto constexpr fgNvngxComboIndex = (uint32_t) FGNvngxReplacement::Combo;
    nvngxOptions[fgNvngxComboIndex].set_disabled(
        !FfxApiProxy::IsFGReady(false) || !state.artursFgFileAvailable,
        "Missing amd_fidelityfx_framegeneration_dx12.dll\nor missing dlss-enabler-headless.dll");

    // TODO: Automatically switch to any other option

    if (!config->FGNvngxReplacement.has_value())
        config->FGNvngxReplacement = config->FGNvngxReplacement.value_or_default(); // need to have a value before combo

    if (state.activeFgInput != FGInput::ForceXeLL)
    {
        // The setup card: where the frames come from and what makes them, what that is doing now, and the options
        // that belong to the input whatever the output. The game's DLSS-G and the unlock/external cards come at the
        // end of the page (RenderFrameGenerationFooter).
        MenuCard setupCard("Frame Generation", menuResScale);

        if (setupCard.IsOpen() && ImGui::BeginTable("fgSelection", 2, ImGuiTableFlags_SizingStretchSame))
        {
            ImGui::TableNextColumn();

            PopulateCombo("FG Input", config->FGInput, inputOptions);
            ShowTooltip("The data source to be used for FG\n"
                        "The native FG which the game supports");

            ImGui::TableNextColumn();

            if (replaceFgOutputWithNvngx)
            {
                // Disable None?
                PopulateCombo("FG Nvngx", config->FGNvngxReplacement, nvngxOptions);
                ShowTooltip("What backend to use instead of the real DLSSG");
            }
            else
            {
                PopulateCombo("FG Output", config->FGOutput, outputOptions);
                ShowTooltip("The FG that you will actually be using");
            }

            ImGui::EndTable();
        }

        // Should be on a new line
        if (setupCard.IsOpen() && showNvngxFgDowndown)
        {
            PopulateCombo("FG Nvngx Replacement", config->FGNvngxReplacement, nvngxOptions);
            ShowTooltip("What backend to use instead of the real DLSSG");
        }

        // Try to avoid having None selected when the gpu doesn't support DLSSG + some fallbacks
        if (!supportsDlssg && (replaceFgOutputWithNvngx || showNvngxFgDowndown) &&
            config->FGNvngxReplacement.value_or_default() == FGNvngxReplacement::None)
        {
            if (state.nukemsFgFileAvailable)
                config->FGNvngxReplacement.set_volatile_value(FGNvngxReplacement::Nukems);

            else if (state.artursFgFileAvailable)
                config->FGNvngxReplacement.set_volatile_value(FGNvngxReplacement::Arturs);

            else if (FfxApiProxy::IsFGReady(false))
                config->FGNvngxReplacement.set_volatile_value(FGNvngxReplacement::FFX);
        }

        const bool nvngxFgChanged = (replaceFgOutputWithNvngx || showNvngxFgDowndown) &&
                                    state.activeFgNvngx != config->FGNvngxReplacement.value_or_default();
        state.fgSettingsChanged = state.activeFgOutput != config->FGOutput.value_or_default() ||
                                  state.activeFgInput != config->FGInput.value_or_default() || nvngxFgChanged;

        if (!setupCard.IsOpen())
            return;

        RenderFgStatusLine(ctx);

        if (state.fgSettingsChanged)
        {
            ImGui::TextColored(toneMapColor(ImVec4(1.f, 0.35f, 0.25f, 1.f)), "Restart the game to apply the change");
            ImGui::SameLine(0.0f, 12.0f);
            if (ImGui::SmallButton("Save Settings##fgRestart"))
                config->SaveIni();
            ShowTooltip("Saves OptiScaler.ini now (same as Save Settings at the bottom of the menu)");
        }

        auto fgOutput = state.CurrentFG();
        const bool hudApplies = fgOutput != nullptr &&
                                (state.activeFgOutput == FGOutput::FSRFG || UsesXeFGApi(state.activeFgOutput) ||
                                 state.activeFgOutput == FGOutput::DLSSG) &&
                                state.activeFgInput != FGInput::NoFG && state.activeFgInput != FGInput::NvngxFG;

        // The input's UI and HUDless use, whatever the output
        if (hudApplies)
        {
            if (auto hudGroup = ScopedCollapsingHeader("HUD & UI"); hudGroup.IsHeaderOpen())
            {
                const auto isUsingUIAny = fgOutput->IsUsingUIAny();

                if (ImGui::BeginTable("fgHudUi", 2, ImGuiTableFlags_SizingStretchSame))
                {
                    ImGui::TableNextColumn();
                    ImGui::Checkbox("Show Detected UI", &state.fgHudlessCompare);
                    ShowHelpMarker("Needs HUDless texture to compare with final image.\n"
                                   "UI elements and ONLY UI elements should have a pink tint!");

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::BeginDisabled(!isUsingUIAny);

                    if (bool drawUIOverFG = config->FGDrawUIOverFG.value_or_default();
                        ImGui::Checkbox("Draw UI over", &drawUIOverFG))
                    {
                        config->FGDrawUIOverFG = drawUIOverFG;
                    }
                    ShowHelpMarker("Draws UI resource over the final image\n"
                                   "If no UI visible, enable this!");

                    ImGui::EndDisabled();

                    ImGui::TableNextColumn();
                    ImGui::BeginDisabled(!isUsingUIAny || !config->FGDrawUIOverFG.value_or_default());

                    if (bool uiPremultipliedAlpha = config->FGUIPremultipliedAlpha.value_or_default();
                        ImGui::Checkbox("UI premultiplied alpha", &uiPremultipliedAlpha))
                    {
                        config->FGUIPremultipliedAlpha = uiPremultipliedAlpha;
                    }
                    ShowHelpMarker("Only with Draw UI over. If the UI looks too faint, turn this off");

                    ImGui::EndDisabled();
                    ImGui::EndTable();
                }

                if (!isUsingUIAny)
                    ImGui::TextDisabled("Draw UI over: the FG input sends no UI texture in this game");
            }
        }

        const bool showOutputSpecificFGSettings = state.activeFgInput == FGInput::DLSSG ||
                                                  state.activeFgInput == FGInput::FSRFG ||
                                                  state.activeFgInput == FGInput::FSRFG30;

        const bool showHudCutoff = state.activeFgInput == FGInput::NvngxFG || state.activeFgOutput == FGOutput::FSRFG;

        if (showOutputSpecificFGSettings || showHudCutoff)
        {
            ImGui::Spacing();

            if (auto ch = ScopedCollapsingHeader("Advanced (FG Input)"); ch.IsHeaderOpen())
            {
                ScopedIndent indent {};
                ImGui::Spacing();

                if (showOutputSpecificFGSettings)
                {
                    auto fgOutput = state.CurrentFG();
                    if (fgOutput)
                    {
                        ImGui::BeginDisabled(!fgOutput->IsActive());

                        const auto isUsingUIAny = fgOutput->IsUsingUIAny();
                        const auto isUsingHudlessAny = fgOutput->IsUsingHudlessAny();

                        bool disableUI = config->FGDisableUI.value_or_default();
                        ImGui::BeginDisabled(!isUsingUIAny && !disableUI);

                        if (ImGui::Checkbox("Disable UI texture", &disableUI))
                        {
                            config->FGDisableUI = disableUI;
                            fgOutput->UpdateTarget();
                        }

                        ShowHelpMarker("For when the game sends a UI texture, but you want to disable it");

                        ImGui::EndDisabled();

                        ImGui::SameLine(0.0f, 16.0f);

                        bool disableHudless = config->FGDisableHudless.value_or_default();
                        ImGui::BeginDisabled(!isUsingHudlessAny && !disableHudless);

                        if (ImGui::Checkbox("Disable HUDless", &disableHudless))
                        {
                            config->FGDisableHudless = disableHudless;
                        }

                        ShowHelpMarker("For when the game sends HUDless, but you want to disable it");

                        ImGui::EndDisabled();

                        bool depthValidNow = config->FGDepthValidNow.value_or_default();
                        if (ImGui::Checkbox("Depth as ValidNow", &depthValidNow))
                            config->FGDepthValidNow = depthValidNow;

                        ShowHelpMarker("Will use more VRAM, but Uniscaler needs this\n"
                                       "Maybe some other games might need too");

                        ImGui::SameLine(0.0f, 16.0f);

                        bool velocityValidNow = config->FGVelocityValidNow.value_or_default();
                        if (ImGui::Checkbox("Velocity as ValidNow", &velocityValidNow))
                            config->FGVelocityValidNow = velocityValidNow;

                        ShowHelpMarker("Will use more VRAM, but Uniscaler needs this\n"
                                       "Maybe some other games might need too");

                        bool hudlessValidNow = config->FGHudlessValidNow.value_or_default();
                        if (ImGui::Checkbox("HUDless as ValidNow", &hudlessValidNow))
                            config->FGHudlessValidNow = hudlessValidNow;

                        ShowHelpMarker("Will use more VRAM, but some games might need this");

                        ImGui::SameLine(0.0f, 16.0f);

                        bool firstHudless = config->FGOnlyAcceptFirstHudless.value_or_default();
                        if (ImGui::Checkbox("Accept First HUDless", &firstHudless))
                            config->FGOnlyAcceptFirstHudless = firstHudless;

                        ShowHelpMarker("If source tags more than one HUDless, only use the first one");

                        if (bool skipReset = config->FGSkipReset.value_or_default();
                            ImGui::Checkbox("Skip Reset", &skipReset))
                        {
                            config->FGSkipReset = skipReset;
                        }

                        ShowHelpMarker("Don't use reset signals from FG Inputs");

                        ImGui::EndDisabled();

                        ImGui::PushItemWidth(80.0f * menuResScale);

                        auto frameAhead = config->FGAllowedFrameAhead.value_or_default();
                        if (ImGui::InputInt("Frame Ahead", &frameAhead, 1, 1) && frameAhead > 0 && frameAhead < 4)
                        {
                            config->FGAllowedFrameAhead = frameAhead;
                        }

                        ShowHelpMarker("Number of frames the FG is allowed to be ahead of the game\n"
                                       "Might prevent FG on/off switching, but also might cause issues");

                        ImGui::PopItemWidth();

                        ImGui::SameLine(0.0f, 16.0f);

                        const char* ftSources[] = { "Input", "Opti", "Zero" };
                        const char* ftSourceInfos[] = { "Uses frametimes provided by\nDLSSG or FSR-FG ",
                                                        "Uses frametimes calculated by Opti",
                                                        "Let XeFG to handle frametimes" };

                        auto currentSet = (int) config->FTInput.value_or_default();
                        auto currentSourceCount = UsesXeFGApi(state.activeFgOutput) ? 3 : 2;

                        ImGui::PushItemWidth(95.0f * menuResScale);

                        if (ImGui::BeginCombo("FT Input", ftSources[currentSet]))
                        {
                            for (size_t i = 0; i < currentSourceCount; i++)
                            {

                                if (ImGui::Selectable(ftSources[i], currentSet == i))
                                {
                                    LOG_DEBUG("FTInput has changed {} -> {}", ftSources[currentSet], ftSources[i]);
                                    config->FTInput = (FrameTimeSource) i;
                                }

                                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                                    ImGui::SetTooltip(ftSourceInfos[i]);
                            }

                            ImGui::EndCombo();
                        }

                        ImGui::PopItemWidth();

                        ShowHelpMarker("Select source for frametime\n"
                                       "Might help frame pacing and stutter issues");
                    }
                }

                if (showHudCutoff)
                {
                    float fgHudCutoff = config->FGHudCutoff.value_or_default();
                    if (ImGui::SliderFloat("Hud Cutoff", &fgHudCutoff, 0.00f, 1.0f, "%.2f"))
                        config->FGHudCutoff = fgHudCutoff;

                    ShowHelpMarker("Cutoffs transparency from UI to help with interpolation\n"
                                   "You can use Show Detected UI to see the difference\n0.0 is auto");
                }
            }
        }
    }
}

MenuCommon::XeFGRunChecks MenuCommon::CheckXeFGRun(RenderMenuContext& ctx, IFGFeature* fg)
{
    auto& state = ctx.state;
    auto config = ctx.config;

    XeFGRunChecks checks;
    checks.ignoreChecks = config->FGXeFGIgnoreInitChecks.value_or_default();
    checks.exclusiveFullscreen = state.realExclusiveFullscreen;

    if (fg == nullptr)
        return checks;

    bool nativeAA = false;
    if (state.activeFgInput == FGInput::Upscaler && ctx.currentFeature != nullptr)
        nativeAA = ctx.currentFeature->RenderWidth() == ctx.currentFeature->DisplayWidth();

    checks.correctMVs = fg->IsLowResMV() || nativeAA ||
                        (State::Instance().gameQuirks & GameQuirk::ForceFGRenderSizeMVs) || checks.ignoreChecks;

    checks.restartNeeded = config->FGXeFGDepthInverted.value_or_default() != fg->IsInvertedDepth() ||
                           config->FGXeFGJitteredMV.value_or_default() != fg->IsJitteredMVs() ||
                           config->FGXeFGHighResMV.value_or_default() == fg->IsLowResMV();

    const bool outputIsHdr = getUiTargetMode() != UiTargetMode::SDR;
    checks.borderlessMissing = !checks.restartNeeded && !checks.ignoreChecks && state.realExclusiveFullscreen;
    checks.hdrUnsupported = !checks.restartNeeded && !checks.ignoreChecks && outputIsHdr &&
                            state.currentSwapchainDesc.BufferDesc.Format >= DXGI_FORMAT_R32G32B32A32_TYPELESS &&
                            state.currentSwapchainDesc.BufferDesc.Format <= DXGI_FORMAT_R16G16B16A16_SINT;
    return checks;
}

const char* MenuCommon::XeFGRunChecks::BlockedReason() const
{
    if (!correctMVs)
        return "needs dilated motion vectors turned off";
    if (exclusiveFullscreen)
        return "needs borderless display mode";
    if (hdrUnsupported)
        return "only HDR10 output is supported";
    return nullptr;
}

// The page's one-line answer to "what is frame generation doing right now" (menu/FgStatusRule.h).
void MenuCommon::RenderFgStatusLine(RenderMenuContext& ctx)
{
    auto& state = ctx.state;

    fgstatus::Input in;
    in.output = state.activeFgOutput;
    in.inputSelected = state.activeFgInput != FGInput::NoFG;
    switch (state.activeFgOutput)
    {
    case FGOutput::F5Low:
        in.outputLabel = "F5Low";
        break;
    case FGOutput::XeFG:
        in.outputLabel = "XeFG";
        break;
    case FGOutput::FSRFG:
        in.outputLabel = "FSR FG";
        break;
    case FGOutput::DLSSG:
        in.outputLabel = "DLSSG";
        break;
    default:
        break;
    }

    if (auto fg = state.CurrentFG(); fg != nullptr)
    {
        in.hasFeature = true;
        in.active = fg->IsActive();
        in.paused = fg->IsPaused();
        in.generationOff = fg->GenerationOff();
        in.generating = fg->IsGenerating();
        in.interpolated = static_cast<int>(fg->GetInterpolatedFrameCount());

        if (UsesXeFGApi(state.activeFgOutput))
            in.blockedReason = CheckXeFGRun(ctx, fg).BlockedReason();
    }
    in.followsGame = ctx.config->FGFollowsGame();
    in.shownFps = ctx.frameRate;

    const auto status = fgstatus::Describe(in);
    switch (status.tone)
    {
    case fgstatus::Tone::Good:
        ImGui::TextColored(toneMapColor({ 0.45f, 0.9f, 0.45f, 1.0f }), "%s", status.text.c_str());
        break;
    case fgstatus::Tone::Waiting:
        ImGui::TextColored(toneMapColor({ 1.0f, 0.75f, 0.25f, 1.0f }), "%s", status.text.c_str());
        break;
    default:
        ImGui::TextDisabled("%s", status.text.c_str());
        break;
    }
}

// The end of the Frame Generation page: the game's own DLSS-G and the unlock/external switches, both collapsed
// (set once, rarely touched). The unlock card opens by itself while external frame generation is on, since that
// is where it is switched off.
void MenuCommon::RenderFrameGenerationFooter(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    auto& menuResScale = ctx.menuResScale;

    const bool gameDlssgShown =
        !state.externalFrameGeneration && state.activeFgInput != FGInput::ForceXeLL &&
        ((state.dlssgMfgMax.has_value() && state.dlssgMfgMax.value() >= 1) || state.dlssgGameDMFGSupported) &&
        state.activeFgOutput != FGOutput::DLSSG && state.activeFgInput != FGInput::DLSSG;

    if (gameDlssgShown)
    {
        if (MenuCard card("Game's own DLSS FG", menuResScale, false); card.IsOpen())
        {
            ImGui::TextDisabled("What OptiScaler tells the game's DLSS-G (Streamline), not the FG Output above");

            const bool dlssgInputOrOutput =
                state.activeFgOutput == FGOutput::DLSSG || state.activeFgInput == FGInput::DLSSG;

            ImGui::BeginDisabled(state.dlssgGameDMFGSupported && config->FGDLSSGOverrideForceDMFG.value_or_default());
            if (state.dlssgMfgMax.has_value() && state.dlssgMfgMax.value() >= 1 && !dlssgInputOrOutput)
            {
                auto maxInterpolationCount = state.dlssgMfgMax.value();

                if (maxInterpolationCount >= 1)
                {
                    // Map config value to UI index
                    int currentSet = 0;
                    if (config->FGDLSSGOverrideInterpolationCount.has_value())
                    {
                        currentSet = config->FGDLSSGOverrideInterpolationCount.value() + 1;
                    }

                    std::string currentIntCountStr;
                    if (currentSet == 0)
                        currentIntCountStr = "Default";
                    else if (currentSet == 1)
                        currentIntCountStr = "Off";
                    else
                        currentIntCountStr = std::to_string(currentSet) + "X";

                    ImGui::PushItemWidth(95.0f * menuResScale);

                    if (ImGui::BeginCombo("Game DLSS-G ratio", currentIntCountStr.c_str()))
                    {
                        for (int i = 0; i <= maxInterpolationCount + 1; i++)
                        {
                            std::string modeStr;
                            if (i == 0)
                                modeStr = "Default";
                            else if (i == 1)
                                modeStr = "Off";
                            else
                                modeStr = std::to_string(i) + "X";

                            if (ImGui::Selectable(modeStr.c_str(), (currentSet == i)))
                            {
                                if (i == 0)
                                {
                                    // Default, no override
                                    config->FGDLSSGOverrideInterpolationCount.reset();
                                }
                                else
                                {
                                    // UI index, store value
                                    int framesToGenerate = i - 1;

                                    LOG_DEBUG("DLSSG Interpolation Count set to: {}", framesToGenerate);
                                    config->FGDLSSGOverrideInterpolationCount = framesToGenerate;
                                }

                                StreamlineHooks::updateDlssgOptions();
                            }
                        }

                        ImGui::EndCombo();
                    }

                    ImGui::PopItemWidth();
                }
            }

            ImGui::EndDisabled();

            if (state.dlssgMfgMax.has_value() && state.dlssgMfgMax.value() >= 1 && !dlssgInputOrOutput)
                RenderDlssgTelemetry();

            if (state.dlssgGameDMFGSupported && !dlssgInputOrOutput)
            {
                // Next to the ratio combo / telemetry when they were drawn, else on its own line
                if (state.dlssgMfgMax.has_value() && state.dlssgMfgMax.value() >= 1)
                    ImGui::SameLine(0.0f, 16.0f);

                if (bool dynamicMFG = config->FGDLSSGOverrideForceDMFG.value_or_default();
                    ImGui::Checkbox("Game DLSS-G dynamic", &dynamicMFG))
                {
                    config->FGDLSSGOverrideForceDMFG = dynamicMFG;
                    StreamlineHooks::updateDlssgOptions();
                }

                ImGui::BeginDisabled(state.dlssgLastSetMode != sl::DLSSGMode::eDynamic);
                static float fpsTarget = config->FGDLSSGFramerateTargetDMFG.value_or_default();
                ImGui::SliderFloat("Game DLSS-G target fps", &fpsTarget, 0, 200, "%.0f");

                ShowHelpMarker("The frame rate the game's dynamic DLSS-G aims for; 0 = the display refresh rate.\n"
                               "Press Apply to send it to the game.");

                if (ImGui::SmallButton("Apply##gameDlssg"))
                {
                    config->FGDLSSGFramerateTargetDMFG = fpsTarget;
                    StreamlineHooks::updateDlssgOptions();
                }

                ImGui::SameLine(0.0f, 16.0f);

                if (ImGui::SmallButton("Reset##gameDlssg"))
                {
                    fpsTarget = 0.0f;
                    config->FGDLSSGFramerateTargetDMFG.reset();
                }

                ImGui::EndDisabled();
            }
        }
    }

    if (MenuCard card("Unlock & external FG", menuResScale, state.externalFrameGeneration); card.IsOpen())
    {
        bool external = config->ExternalFrameGeneration.value_or_default();
        if (ImGui::Checkbox("External frame generation / MFG unlocker", &external))
            config->ExternalFrameGeneration = external;
        ShowHelpMarker("Leaves Streamline, Reflex and FG control to the game/external mod."
                       "\nNR and NGX upscaling remain available. Save Settings and restart."
                       "\nDoes not install an unlocker or enable FG in unsupported games.");
        if (external != state.externalFrameGeneration)
            ImGui::TextWrapped("Save Settings and restart to change frame-generation ownership.");

        ImGui::SeparatorText("Built-in MFG unlock");
        const std::array unlockModes = { config->MfgUnlockXeFG.value_or_default(),
                                         config->MfgUnlockRTX40.value_or_default(),
                                         config->MfgUnlockRTX2030.value_or_default() };
        for (size_t index = 0; index < unlockModes.size(); ++index)
        {
            const auto family = static_cast<MfgUnlock::Family>(index);
            const auto status = MfgUnlock::StatusRegistry::Get(family);
            const std::string text = unlockModes[index] == MfgUnlockMode::Off      ? "off"
                                     : status.state == MfgUnlock::FamilyState::Off ? "waiting for a provider"
                                                                                   : MfgUnlock::FormatStatus(status);
            const auto name = MfgUnlock::kFamilyNames[index];
            ImGui::Text("%.*s: %s", static_cast<int>(name.size()), name.data(), text.c_str());
        }
        if (ImGui::SmallButton("Change on the Plugins page"))
            MenuPages::RequestPage(MenuPages::Page::Plugins);
    }
}

// FG Input DLSSG: Frame Generation on/off and its multiplier can follow the game's own DLSS FG setting
// (Enabled unset, framegen/FGFollowGameRule.h). Nothing is drawn where it can't (other inputs, Streamline 1).
void MenuCommon::RenderFGFollowGame()
{
    if (!Config::FGCanFollowGame())
        return;

    auto* config = Config::Instance();
    auto& state = State::Instance();

    bool follow = config->FGFollowsGame();
    if (ImGui::Checkbox("Follow game's DLSS FG", &follow))
    {
        if (follow)
        {
            config->FGEnabled.reset();
        }
        else
        {
            // Keep what runs now; from here the Active box below decides
            config->FGEnabled = config->IsFGEnabled();
        }

        LOG_INFO("Frame Generation {} the game's DLSS FG setting", follow ? "follows" : "no longer follows");

        if (config->IsFGEnabled())
            state.fgChanged = true;
    }
    ShowHelpMarker("On: Frame Generation and its multiplier follow the game's own DLSS Frame Generation\n"
                   "setting. Off: set them here.\n"
                   "The multiplier is capped at what the selected FG output supports.\n"
                   "ini: [FrameGen] Enabled=auto follows the game. Save Settings to keep it.");

    if (!follow)
        return;

    ImGui::SameLine(0.0f, 16.0f);

    const auto game = state.dlssgInputGameRequest.load();
    if (!game.enabled)
    {
        ImGui::TextDisabled("Game: FG off");
        return;
    }

    const int outputMax = state.CurrentFG() != nullptr ? state.CurrentFG()->GetMaxInterpolationCount() : 0;
    const int running = config->FGInterpolationCount(1, outputMax);

    if (running == game.count)
        ImGui::TextColored(toneMapColor(ImVec4(0.f, 1.f, 0.25f, 1.f)), "Game: FG on %dx", game.count + 1);
    else
        ImGui::TextColored(toneMapColor(ImVec4(1.f, 0.8f, 0.f, 1.f)), "Game: FG on %dx, running %dx (output max)",
                           game.count + 1, running + 1);
}

// XeFG's Dynamic MFG: the multiplier follows a target frame rate (XeFG_Dx12::DynamicCount)
void MenuCommon::RenderXeFGDynamicMFG(RenderMenuContext& ctx, int maxInterpolationCount, bool followsGame)
{
    auto config = ctx.config;
    auto fg = ctx.state.currentFG;
    const float menuResScale = ctx.menuResScale;

    // Not while following the game's count: the game picks it then
    ImGui::BeginDisabled(followsGame);

    if (bool dynamic = config->FGXeFGDynamicMFG.value_or_default(); ImGui::Checkbox("Dynamic MFG##xefg", &dynamic))
        config->FGXeFGDynamicMFG = dynamic;

    ShowHelpMarker("Changes the multiplier on the fly to hit the target FPS,\n"
                   "up to the MFG setting (at 2X: frame generation on or off).\n"
                   "Not while following the game's DLSS FG count.");

    const bool dynamicMFG = config->FGXeFGDynamicMFG.value_or_default() && !followsGame;

    if (dynamicMFG && fg != nullptr && fg->IsActive())
    {
        ImGui::SameLine(0.0f, 16.0f);

        if (fg->GenerationOff())
            ImGui::TextColored(toneMapColor(ImVec4(1.0f, 0.647f, 0.0f, 1.0f)), "Now 1X (frame generation off)");
        else
            ImGui::Text("Now %uX", fg->GetInterpolatedFrameCount() + 1);
    }

    if (dynamicMFG)
    {
        // Applied on release, so dragging doesn't start Dynamic MFG over every frame; follows the setting otherwise
        // (an ini reload). Whole numbers; the lowest step (9) is Auto, the lowest target 10.
        constexpr float autoStep = 9.0f;
        static float target = autoStep;
        static bool dragging = false;

        if (!dragging)
            target = std::max(std::round(config->FGXeFGFramerateTargetDMFG.value_or_default()), autoStep);

        ImGui::SetNextItemWidth(150.0f * menuResScale);
        ImGui::SliderFloat("Target FPS##xefg", &target, autoStep, (float) std::max(200, refreshRate),
                           std::round(target) <= autoStep ? "Auto" : "%.0f", ImGuiSliderFlags_AlwaysClamp);

        dragging = ImGui::IsItemActive();

        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            target = std::round(target);

            if (target <= autoStep)
                config->FGXeFGFramerateTargetDMFG.reset();
            else
                config->FGXeFGFramerateTargetDMFG = target;
        }

        ShowHelpMarker("Presented FPS to aim for, never above the FPS limit\n"
                       "(or the refresh rate with V-Sync).\n"
                       "Auto: the FPS limit, or just under the refresh rate.");

        // The cap picks from 2X up: it needs a higher MFG setting
        const bool onOff =
            std::clamp(config->FGXeFGInterpolationCount.value_or_default(), 1, std::max(maxInterpolationCount, 1)) == 1;
        const bool targetAsCap = config->FGXeFGTargetAsCapDMFG.value_or_default() && !onOff;

        if (!onOff)
        {
            ImGui::SameLine(0.0f, 16.0f);

            if (bool cap = targetAsCap; ImGui::Checkbox("Use as FPS limit##xefg", &cap))
                config->FGXeFGTargetAsCapDMFG = cap;

            ShowHelpMarker("Also limits FPS to the target and uses the lowest\n"
                           "multiplier that reaches it.");
        }

        // The cap holds the target with its own rules
        ImGui::BeginDisabled(targetAsCap);

        const char* roundings[] = { "At most target", "Around target", "At least target" };
        int rounding = std::clamp(config->FGXeFGRoundingDMFG.value_or_default(), 0, 2);

        ImGui::SetNextItemWidth(150.0f * menuResScale);
        if (ImGui::Combo("##xefgRounding", &rounding, roundings, IM_ARRAYSIZE(roundings)))
            config->FGXeFGRoundingDMFG = rounding;

        ShowHelpMarker("Where FPS may land, as multipliers only come in steps:\n"
                       "at most, around or at least the target FPS.");

        // At 2X, 1X is the only other step
        if (!onOff)
        {
            ImGui::SameLine(0.0f, 16.0f);

            if (bool allow1X = config->FGXeFGDynamicAllow1X.value_or_default();
                ImGui::Checkbox("Allow 1X##xefg", &allow1X))
            {
                config->FGXeFGDynamicAllow1X = allow1X;
            }

            ShowHelpMarker("Turns frame generation off when the game\n"
                           "alone gets close enough to the target.");
        }

        ImGui::EndDisabled();
    }

    ImGui::EndDisabled();
}

void MenuCommon::RenderFrameGenerationRuntimeSettings(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    auto& currentFeature = ctx.currentFeature;
    auto& menuResScale = ctx.menuResScale;
    auto& primaryGpu = *ctx.primaryGpu;
    auto fgOutput = state.CurrentFG();

    const UiTargetMode uiTargetMode = getUiTargetMode();
    const bool outputIsHdr = uiTargetMode != UiTargetMode::SDR;

    // FSR FG controls
    if (state.activeFgOutput == FGOutput::FSRFG && state.activeFgInput != FGInput::NoFG &&
        state.FGSwapchainExists())
    {
        if (state.activeFgInput != FGInput::Upscaler || (currentFeature != nullptr && !currentFeature->IsFrozen()) &&
                                                            (state.currentVkFG != nullptr || FfxApiProxy::IsFGReady()))
        {
            ImGui::SeparatorText("Frame Generation (FSR FG)");

            // The output resets an index from a game with more FFX versions
            if (_ffxFGIndex < 0 || _ffxFGIndex >= (int) state.ffxFGVersionNames.size())
                _ffxFGIndex = config->FfxFGIndex.value_or_default();

            if (state.ffxFGVersionNames.size() > 0)
            {
                ImGui::PushItemWidth(135.0f * menuResScale);

                auto currentName = StrFmt("FSR %s", state.ffxFGVersionNames[_ffxFGIndex]);
                if (ImGui::BeginCombo("FFX FG", currentName.c_str()))
                {
                    for (int n = 0; n < state.ffxFGVersionIds.size(); n++)
                    {
                        auto name = StrFmt("FSR %s", state.ffxFGVersionNames[n]);
                        if (ImGui::Selectable(name.c_str(), config->FfxFGIndex.value_or_default() == n))
                            _ffxFGIndex = n;
                    }

                    ImGui::EndCombo();
                }
                ImGui::PopItemWidth();

                ShowHelpMarker("List of FGs reported by FFX SDK");

                ImGui::SameLine(0.0f, 6.0f);

                if (ImGui::Button("Change FG") && _ffxFGIndex != config->FfxFGIndex.value_or_default())
                {
                    config->FfxFGIndex = _ffxFGIndex;
                    state.fgChanged = true;
                    state.scChanged = true;
                }
            }

            RenderFGFollowGame();

            ImGui::BeginDisabled(config->FGFollowsGame());
            bool fgActive = config->IsFGEnabled();
            if (ImGui::Checkbox("Active##2", &fgActive))
            {
                config->FGEnabled = fgActive;
                LOG_DEBUG("FGEnabled set FGEnabled: {}", fgActive);

                if (config->IsFGEnabled())
                    state.fgChanged = true;
            }
            ImGui::EndDisabled();
            ShowHelpMarker("Enable Frame Generation");

            bool fgAsync = config->FGAsync.value_or_default();
            if (ImGui::Checkbox("Allow Async", &fgAsync))
            {
                config->FGAsync = fgAsync;

                if (config->IsFGEnabled())
                {
                    state.fgChanged = true;
                    state.scChanged = true;
                    LOG_DEBUG("Async set FGChanged");
                }
            }
            ShowHelpMarker("Enable Async for better FG performance\nMight cause crashes, especially with HUD Fix!");

            if (state.amdFlipMeterCreated)
            {
                ImGui::SameLine(0.0f, 16.0f);

                bool disableFlipMetering = config->FSRFGDisableFlipMetering.value_or_default();
                if (ImGui::Checkbox("Disable HW Flip Metering", &disableFlipMetering))
                {
                    config->FSRFGDisableFlipMetering = disableFlipMetering;
                    Amdxc64Hooks::ApplyFlipMeterOverride();
                }
                ShowHelpMarker("Overrides the AMD hardware flip metering requested by FSR");

                ImGui::SameLine(0.0f, 8.0f);

                if (state.amdFlipMeterMode != 0)
                    ImGui::TextDisabled("(Mode %u)", state.amdFlipMeterMode);
                else
                    ImGui::TextDisabled("(Off)");

                ShowTooltip("Flip metering mode applied in the driver\nFSR only requests it in exclusive or "
                            "borderless fullscreen");
            }

            ImGui::SameLine(0.0f, 16.0f);

            bool fgDV = config->FGDebugView.value_or_default();
            if (ImGui::Checkbox("Debug View##2", &fgDV))
            {
                config->FGDebugView = fgDV;

                if (config->IsFGEnabled())
                {
                    state.fgChanged = true;
                    LOG_DEBUG("DebugView set FGChanged");
                }
            }
            ShowHelpMarker("Enable FSR3.1-FG Debug view\n\n"
                           "Top left: Game Motion Vectors\n"
                           "Top middle: GMV Depth\n"
                           "Top right: Optical Flow MV\n"
                           "Middle: Interpolated frame only\n"
                           "Bottom left: Disocclusion mask\n"
                           "Bottom middle: Interpolation source (w/o UI)\n"
                           "Bottom right: HUDless resource");

            ImGui::SameLine(0.0f, 16.0f);

            if (state.currentFG && state.currentFG->Version().major > 3)
            {
                if (bool fgwm = config->FSRFGEnableWatermark.value_or_default();
                    ImGui::Checkbox("Enable Watermark", &fgwm))
                {
                    LOG_DEBUG("FSRFGEnableWatermark set FGWatermark: {}", fgwm);
                    config->FSRFGEnableWatermark = fgwm;
                    Util::SetFlagEnvVar(L"MLFI-WATERMARK", fgwm);

                    // The driver reads the variable on context creation
                    if (config->IsFGEnabled())
                        state.fgChanged = true;
                }
            }

            ImGui::Spacing();

            if (auto ch = ScopedCollapsingHeader("Extended FSR FG Settings"); ch.IsHeaderOpen())
            {
                ScopedIndent indent {};
                ImGui::Spacing();

                ImGui::Checkbox("FG Only Generated", &state.fgOnlyGenerated);
                ShowHelpMarker("Display only FSR 3.1 Generated frames");

                ImGui::SameLine(0.0f, 16.0f);
                auto debugResetLines = config->FGDebugResetLines.value_or_default();
                if (ImGui::Checkbox("Debug Reset Lines", &debugResetLines))
                {
                    config->FGDebugResetLines = debugResetLines;
                    LOG_DEBUG("Enabled set FGDebugLines: {}", debugResetLines);
                }
                ShowHelpMarker("Enables drawing of Interpolation skip lines");

                auto debugTearLines = config->FGDebugTearLines.value_or_default();
                if (ImGui::Checkbox("Debug Tear Lines", &debugTearLines))
                {
                    config->FGDebugTearLines = debugTearLines;
                    LOG_DEBUG("Enabled set FGDebugLines: {}", debugTearLines);
                }
                ShowHelpMarker("Enables drawing of Tear and Interpolation skip lines");

                ImGui::SameLine(0.0f, 16.0f);
                auto debugPacingLines = config->FGDebugPacingLines.value_or_default();
                if (ImGui::Checkbox("Debug Pacing Lines", &debugPacingLines))
                {
                    config->FGDebugPacingLines = debugPacingLines;
                    LOG_DEBUG("Enabled set FGDebugLines: {}", debugPacingLines);
                }
                ShowHelpMarker("Enables drawing of Pacing lines");

                ImGui::Spacing();
                if (ImGui::TreeNode("FG Rectangle Settings"))
                {
                    ImGui::PushItemWidth(95.0f * menuResScale);
                    int rectLeft = config->FGRectLeft.value_or(0);
                    if (ImGui::InputInt("Rect Left", &rectLeft))
                        config->FGRectLeft = rectLeft;

                    ImGui::SameLine(0.0f, 16.0f);
                    int rectTop = config->FGRectTop.value_or(0);
                    if (ImGui::InputInt("Rect Top", &rectTop))
                        config->FGRectTop = rectTop;

                    int rectWidth = config->FGRectWidth.value_or(0);
                    if (ImGui::InputInt("Rect Width", &rectWidth))
                        config->FGRectWidth = rectWidth;

                    ImGui::SameLine(0.0f, 16.0f);
                    int rectHeight = config->FGRectHeight.value_or(0);
                    if (ImGui::InputInt("Rect Height", &rectHeight))
                        config->FGRectHeight = rectHeight;

                    ImGui::PopItemWidth();
                    ShowHelpMarker("Frame generation rectangle, adjust for letterboxed content");

                    ImGui::BeginDisabled(!config->FGRectLeft.has_value() && !config->FGRectTop.has_value() &&
                                         !config->FGRectWidth.has_value() && !config->FGRectHeight.has_value());

                    if (ImGui::Button("Reset FG Rect"))
                    {
                        config->FGRectLeft.reset();
                        config->FGRectTop.reset();
                        config->FGRectWidth.reset();
                        config->FGRectHeight.reset();
                    }

                    ShowHelpMarker("Resets Frame generation rectangle");

                    ImGui::EndDisabled();
                    ImGui::TreePop();
                }

                // Vulkan FSR-FG reports FFX SDK versions, SDK 1.1.3 is FSR 3.1.3
                auto fg = state.CurrentFG();
                auto fptMinVersion =
                    state.currentVkFG != nullptr ? feature_version { 1, 1, 3 } : feature_version { 3, 1, 3 };
                if (fg != nullptr && strcmp(fg->Name(), "FSR-FG") == 0 && fg->Version() >= fptMinVersion)
                {
                    ImGui::Spacing();

                    if (ImGui::TreeNode("Frame Pacing Tuning"))
                    {
                        auto fptEnabled = config->FGFramePacingTuning.value_or_default();
                        if (ImGui::Checkbox("Enable Tuning", &fptEnabled))
                        {
                            config->FGFramePacingTuning = fptEnabled;
                            state.fsrfgFramePaceTuningChanged = true;
                        }

                        ImGui::BeginDisabled(!config->FGFramePacingTuning.value_or_default());

                        ImGui::PushItemWidth(115.0f * menuResScale);
                        auto fptSafetyMargin = config->FGFPTSafetyMarginInMs.value_or_default();
                        if (ImGui::InputFloat("Safety Margins in ms", &fptSafetyMargin, 0.01f, 0.1f, "%.2f"))
                            config->FGFPTSafetyMarginInMs = fptSafetyMargin;
                        ShowHelpMarker("Safety margins in millisecons\n"
                                       "FSR default value: 0.1ms\n"
                                       "Opti default value: 0.01ms");

                        auto fptVarianceFactor = config->FGFPTVarianceFactor.value_or_default();
                        if (ImGui::SliderFloat("Variance Factor", &fptVarianceFactor, 0.0f, 1.0f, "%.2f"))
                            config->FGFPTVarianceFactor = fptVarianceFactor;
                        ShowHelpMarker("Variance factor\n"
                                       "FSR default value: 0.1\n"
                                       "Opti default value: 0.3");
                        ImGui::PopItemWidth();

                        // FFX's Vulkan swapchain only reads the safety margin and variance factor
                        ImGui::BeginDisabled(state.currentVkFG != nullptr);

                        auto fpHybridSpin = config->FGFPTAllowHybridSpin.value_or_default();
                        if (ImGui::Checkbox("Enable Hybrid Spin", &fpHybridSpin))
                            config->FGFPTAllowHybridSpin = fpHybridSpin;
                        ShowHelpMarker("Allows pacing spinlock to sleep, should reduce CPU usage\n"
                                       "Might cause slow ramp up of FPS");

                        ImGui::PushItemWidth(115.0f * menuResScale);
                        auto fptHybridSpinTime = config->FGFPTHybridSpinTime.value_or_default();
                        if (ImGui::SliderInt("Hybrid Spin Time", &fptHybridSpinTime, 0, 100))
                            config->FGFPTHybridSpinTime = fptHybridSpinTime;
                        ShowHelpMarker("How long to spin if FPTHybridSpin is true. Measured in timer "
                                       "resolution units.\n"
                                       "Not recommended to go below 2. Will result in frequent overshoots");
                        ImGui::PopItemWidth();

                        auto fpWaitForSingleObjectOnFence =
                            config->FGFPTAllowWaitForSingleObjectOnFence.value_or_default();
                        if (ImGui::Checkbox("Enable WaitForSingleObjectOnFence", &fpWaitForSingleObjectOnFence))
                        {
                            config->FGFPTAllowWaitForSingleObjectOnFence = fpWaitForSingleObjectOnFence;
                        }
                        ShowHelpMarker("Allows WaitForSingleObject instead of spinning for fence value");

                        ImGui::EndDisabled();

                        if (ImGui::Button("Apply Timing Changes"))
                            state.fsrfgFramePaceTuningChanged = true;

                        ImGui::EndDisabled();
                        ImGui::TreePop();
                    }
                }

                ImGui::Spacing();
                ImGui::Spacing();
            }
        }
    }

    // XeFG controls (also F5Low, which runs our f5low.dll behind XeFG's API): one card. The checks below run whether
    // the card is open or not, since they turn frame generation off when it cannot run; only the drawing waits for it.
    if (UsesXeFGApi(state.activeFgOutput) && state.activeFgInput != FGInput::NoFG &&
        state.activeFgInput != FGInput::ForceXeLL && state.currentFGSwapchain != nullptr && XeFGProxy::InitXeFG() &&
        fgOutput)
    {
        const bool f5low = state.activeFgOutput == FGOutput::F5Low;
        MenuCard xefgCard(f5low ? "F5Low" : "XeFG", menuResScale);

        const auto checks = CheckXeFGRun(ctx, fgOutput);
        bool ignoreChecks = checks.ignoreChecks;
        const bool correctMVs = checks.correctMVs;

        if (!correctMVs || checks.exclusiveFullscreen)
        {
            config->FGEnabled.reset();

            // Unset now follows the game's DLSS FG with FG Input DLSSG, which could turn it back on: off for this run
            if (Config::FGCanFollowGame())
                config->FGEnabled.set_volatile_value(false);

            config->FGXeFGDebugView.reset();
        }

        const bool restartNeeded = checks.restartNeeded;
        const bool borderlessMissing = checks.borderlessMissing;
        const bool hdrUnsupported = checks.hdrUnsupported;
        const bool cantActivate = checks.CantActivate();

        // Picks up an XeFG / XeLL library loaded after start and logs it, whether the card is open or not
        XeModuleInfo::Describe();

        if (xefgCard.IsOpen())
        {
            // What stops it, first
            if (restartNeeded)
            {
                ImGui::TextColored(toneMapColor(ImVec4(1.f, 0.8f, 0.f, 1.f)),
                                   "Restart the game to apply correct XeFG settings!");
            }
            else
            {
                if (!correctMVs)
                    ImGui::TextColored(toneMapColor(ImVec4(1.f, 0.f, 0.f, 1.f)),
                                       "Requires disabling dilated motion vectors");

                if (borderlessMissing)
                    ImGui::TextColored(toneMapColor(ImVec4(1.f, 0.f, 0.f, 1.f)), "Borderless display mode required!");

                if (hdrUnsupported)
                    ImGui::TextColored(toneMapColor(ImVec4(1.0f, 0.0f, 0.0f, 1.f)), "XeFG only supports HDR10");
            }

            if (!correctMVs || cantActivate || ignoreChecks)
            {
                if (ImGui::Checkbox("Ignore Init Checks", &ignoreChecks))
                    config->FGXeFGIgnoreInitChecks = ignoreChecks;

                ShowHelpMarker("Ignores all prechecks for XeFG\n"
                               "Don't use this option to skip MV size warning for UE games!\n"
                               "It might cause crashes and bad IQ!");
            }

            ImGui::BeginDisabled(!correctMVs || cantActivate);

            RenderFGFollowGame();

            // While following, the game's DLSS FG setting drives these two
            const bool followsGame = config->FGFollowsGame();

            auto maxInterpolationCount = fgOutput->GetMaxInterpolationCount();

            // Dynamic MFG picks the count: the combo is the most it may use
            const bool dynamicMFG = config->FGXeFGDynamicMFG.value_or_default() && !followsGame;

            if (ImGui::BeginTable("xefgMain", 2, ImGuiTableFlags_SizingStretchSame))
            {
                // F5Low: what makes the frames inside it (read each time F5Low creates its context)
                if (f5low)
                {
                    ImGui::TableNextColumn();

                    static const std::vector<MenuOption<F5Low::Generator>> generatorOptions = {
                        { F5Low::Generator::XefgTensor, F5Low::GeneratorLabel(F5Low::Generator::XefgTensor),
                          "Our XeFG frame generation on the NVIDIA tensor cores (f5low.dll)" },
                        { F5Low::Generator::Dlssg, F5Low::GeneratorLabel(F5Low::Generator::Dlssg),
                          "NVIDIA's DLSS-G, run by f5low.dll through its own Streamline copy\n"
                          "Needs an NVIDIA RTX GPU, Streamline and nvngx_dlssg.dll in OptiScaler's streamline folder\n"
                          "The game's own nvngx_dlssg.dll is kept from loading (two DLSS-G copies fault the GPU)\n"
                          "Takes effect after a restart" },
                        { F5Low::Generator::Intel, F5Low::GeneratorLabel(F5Low::Generator::Intel),
                          "Intel's own XeFG makes the frames, F5Low paces them, up to 6x (unlocked in memory)\n"
                          "Needs libxess_fg.dll and libxell.dll in OptiScaler's folder\n"
                          "Takes effect after a restart" },
                    };

                    if (!config->F5LowGenerator.has_value())
                        config->F5LowGenerator =
                            config->F5LowGenerator.value_or_default(); // need to have a value before combo

                    PopulateCombo("Frame Generator", config->F5LowGenerator, generatorOptions);
                    const auto chosenGenerator = config->F5LowGenerator.value_or_default();
                    const auto runningGenerator = F5Low::LatchedGenerator();

                    if (chosenGenerator == F5Low::Generator::Intel)
                        ShowTooltip("What makes the generated frames in F5Low\n\n"
                                    "XeFG (Intel library): Intel's own XeFG makes the frames, F5Low paces them, up "
                                    "to 6x (unlocked in memory). Needs libxess_fg.dll and libxell.dll in "
                                    "OptiScaler's folder. A change here takes effect after a restart.");
                    else if (chosenGenerator == F5Low::Generator::Dlssg)
                        ShowTooltip("What makes the generated frames in F5Low\n\n"
                                    "DLSS-G: NVIDIA's frame generation through a Streamline copy of its own, started "
                                    "with the game. A change here takes effect after a restart.");
                    else
                        ShowTooltip(
                            "What makes the generated frames in F5Low\n\n"
                            "On every start f5low.dll builds this generator from libxess_fg.dll in memory, in the "
                            "background (about half a second); it starts by itself when ready. Nothing is written to "
                            "disk.\nIf it can't, it hands over to Intel's XeFG (up to 4x).\n"
                            "A change here takes effect after a restart.");

                    // The running generator stays as it was at startup
                    if (chosenGenerator != runningGenerator)
                        ImGui::TextColored(
                            toneMapColor(ImVec4(1.0f, 0.8f, 0.0f, 1.f)), "Restart the game to switch to %s (now: %s)",
                            F5Low::GeneratorLabel(chosenGenerator), F5Low::GeneratorLabel(runningGenerator));

                    if (chosenGenerator == F5Low::Generator::Dlssg)
                    {
                        if (const auto missing = F5Low::DlssgFilesMissing(); !missing.empty())
                            ImGui::TextColored(toneMapColor(ImVec4(1.0f, 0.2f, 0.2f, 1.0f)), "%s", missing.c_str());

                        if (config->FGInput.value_or_default() == FGInput::DLSSG)
                        {
                            ImGui::PushTextWrapPos(0.0f);
                            ImGui::TextColored(toneMapColor(ImVec4(1.0f, 0.8f, 0.0f, 1.f)),
                                               "FG Input DLSSG: the game's own DLSS-G stays loaded next to F5Low's; "
                                               "this has faulted the GPU in some games. Use FG Input Upscaler, or "
                                               "Frame Generator XeFG Tensor");
                            ImGui::PopTextWrapPos();
                        }
                    }

                    if (chosenGenerator == F5Low::Generator::Intel)
                        if (const auto missing = F5Low::IntelFilesMissing(); !missing.empty())
                            ImGui::TextColored(toneMapColor(ImVec4(1.0f, 0.2f, 0.2f, 1.0f)), "%s", missing.c_str());

                    const auto f5lowStatus = F5Low::QueryStatus();
                    ImGui::TextColored(toneMapColor(f5lowStatus.state == F5Low::RunState::Own
                                                        ? ImVec4(0.2f, 1.0f, 0.2f, 1.0f)
                                                    : f5lowStatus.state == F5Low::RunState::Unavailable
                                                        ? ImVec4(1.0f, 0.2f, 0.2f, 1.0f)
                                                        : ImVec4(1.0f, 0.8f, 0.0f, 1.0f)),
                                       "%s",
                                       f5lowStatus.state == F5Low::RunState::Undecided && !f5lowStatus.reason.empty()
                                           ? ("starting: " + f5lowStatus.reason).c_str()
                                           : F5Low::RunStateLabel(f5lowStatus.state, runningGenerator));
                    if (!f5lowStatus.reason.empty())
                        ShowTooltip(f5lowStatus.reason.c_str());
                }

                ImGui::TableNextColumn();

                ImGui::BeginDisabled(followsGame);
                bool fgActive = config->IsFGEnabled();
                if (ImGui::Checkbox("Active##3", &fgActive))
                {
                    config->FGEnabled = fgActive;
                    LOG_DEBUG("Enabled set FGEnabled: {}", fgActive);

                    if (config->IsFGEnabled())
                        state.fgChanged = true;
                }
                ImGui::EndDisabled();

                ShowHelpMarker(followsGame ? "Follows the game's DLSS FG setting" : "Turns frame generation on or off");

                if (maxInterpolationCount > 1)
                {
                    ImGui::TableNextColumn();
                    ImGui::BeginDisabled(followsGame);

                    auto currentSet = dynamicMFG ? std::clamp(config->FGXeFGInterpolationCount.value_or_default(), 1,
                                                              maxInterpolationCount) -
                                                       1
                                                 : (int) fgOutput->GetInterpolatedFrameCount() - 1;

                    std::string currentIntCountStr = std::to_string(currentSet + 2) + "X";

                    ImGui::PushItemWidth(95.0f * menuResScale);

                    if (ImGui::BeginCombo(dynamicMFG ? "Max multiplier" : "Multiplier", currentIntCountStr.c_str()))
                    {
                        for (int i = 0; i < maxInterpolationCount; i++)
                        {
                            std::string modeStr = std::to_string(i + 2) + "X";

                            if (ImGui::Selectable(modeStr.c_str(), (currentSet == i)))
                            {
                                LOG_DEBUG("XeFG Interpolation Count set to: {}", i + 1);
                                config->FGXeFGInterpolationCount = i + 1;

                                // Dynamic MFG takes a new most at the next frame, without restarting frame generation
                                if (!dynamicMFG)
                                    state.fgChanged = true;
                            }
                        }

                        ImGui::EndCombo();
                    }

                    ImGui::PopItemWidth();
                    ImGui::EndDisabled();

                    ShowHelpMarker(dynamicMFG ? "The highest multiplier Dynamic MFG may use"
                                              : "Shown frames per rendered frame (2X = one generated frame)");
                }

                ImGui::EndTable();
            }

            RenderXeFGDynamicMFG(ctx, maxInterpolationCount, followsGame);

            ImGui::EndDisabled();

            ImGui::Spacing();

            if (auto group = ScopedCollapsingHeader("HUD & UI##xefg"); group.IsHeaderOpen())
            {
                const bool canCompose = fgOutput->IsUsingHudlessAny() && XeFGProxy::SetUiCompositionState() != nullptr;

                ImGui::BeginDisabled(!canCompose || !correctMVs || cantActivate);
                bool fgCompositeUI = config->FGXeFGUIComposition.value_or_default();
                if (ImGui::Checkbox("UI Composition", &fgCompositeUI))
                    config->FGXeFGUIComposition = fgCompositeUI;

                ShowHelpMarker("Disable HUD/UI interpolation\n"
                               "Reverts back to previous XeFG 2 behaviour\n\n"
                               "Fixes artifacting transparent HUD/UI");
                ImGui::EndDisabled();

                if (!canCompose)
                    ImGui::TextDisabled(!fgOutput->IsUsingHudlessAny()
                                            ? "UI Composition: the FG input sends no HUDless image in this game"
                                            : "UI Composition: this XeFG library does not offer it");
            }

            if (auto group = ScopedCollapsingHeader("Display##xefg"); group.IsHeaderOpen())
            {
                bool fgBorderless = config->FGXeFGForceBorderless.value_or_default();
                if (ImGui::Checkbox("Force Borderless", &fgBorderless))
                    config->FGXeFGForceBorderless = fgBorderless;

                ShowHelpMarker("Forces Borderless display mode\n\n"
                               "For best results, set fullscreen \n"
                               "resolution to your display resolution\n"
                               "Might cause some instability issues.\n\n"
                               "NEEDS GAME RESTART TO BE ACTIVE!");
            }

            if (auto group = ScopedCollapsingHeader("Advanced##xefg"); group.IsHeaderOpen())
            {
                if (ImGui::TreeNode("Rectangle Settings"))
                {
                    ImGui::PushItemWidth(95.0f * menuResScale);
                    int rectLeft = config->FGRectLeft.value_or(0);
                    if (ImGui::InputInt("Rect Left##2", &rectLeft))
                        config->FGRectLeft = rectLeft;

                    ImGui::SameLine(0.0f, 16.0f);
                    int rectTop = config->FGRectTop.value_or(0);
                    if (ImGui::InputInt("Rect Top##2", &rectTop))
                        config->FGRectTop = rectTop;

                    int rectWidth = config->FGRectWidth.value_or(0);
                    if (ImGui::InputInt("Rect Width##2", &rectWidth))
                        config->FGRectWidth = rectWidth;

                    ImGui::SameLine(0.0f, 16.0f);
                    int rectHeight = config->FGRectHeight.value_or(0);
                    if (ImGui::InputInt("Rect Height##2", &rectHeight))
                        config->FGRectHeight = rectHeight;

                    ImGui::PopItemWidth();
                    ShowHelpMarker("Frame generation rectangle, adjust for letterboxed content##2");

                    ImGui::BeginDisabled(!config->FGRectLeft.has_value() && !config->FGRectTop.has_value() &&
                                         !config->FGRectWidth.has_value() && !config->FGRectHeight.has_value());

                    if (ImGui::SmallButton("Reset FG Rect##2"))
                    {
                        config->FGRectLeft.reset();
                        config->FGRectTop.reset();
                        config->FGRectWidth.reset();
                        config->FGRectHeight.reset();
                    }

                    ShowHelpMarker("Resets Frame generation rectangle##2");

                    ImGui::EndDisabled();
                    ImGui::TreePop();
                }
            }

            if (auto group = ScopedCollapsingHeader("Debug##xefg"); group.IsHeaderOpen())
            {
                ImGui::BeginDisabled(!correctMVs || cantActivate);
                bool fgDV = config->FGXeFGDebugView.value_or_default();
                if (ImGui::Checkbox("Debug View##2", &fgDV))
                {
                    config->FGXeFGDebugView = fgDV;

                    if (config->FGXeFGDebugView.value_or_default())
                    {
                        state.fgChanged = true;
                        LOG_DEBUG("DebugView set FGChanged");
                    }
                }
                ShowHelpMarker("Enable XeFG Debug view");
                ImGui::EndDisabled();

                // Disable this for now
                // ImGui::SameLine(0.0f, 16.0f);
                // ImGui::Checkbox("Only Generated##2", &state.fgOnlyGenerated);
                // ShowHelpMarker("Display only XeFG generated frames");
            }

            // Which files are in use (worked out in the background at start, see XeModuleInfo): one dim line, the
            // full lines on hover
            const auto xefgLine = XeModuleInfo::XeFGLine();
            const auto xellLine = XeModuleInfo::XeLLLine();
            const auto shortLine = [](const std::string& line)
            {
                const auto from = line.find(" from ");
                return from == std::string::npos ? line : line.substr(0, from);
            };

            const auto joined = [](const std::string& a, const std::string& b, const char* separator)
            { return a.empty() ? b : (b.empty() ? a : a + separator + b); };

            if (const auto shortText = joined(shortLine(xefgLine), shortLine(xellLine), "   "); !shortText.empty())
            {
                ImGui::Spacing();
                ImGui::TextDisabled("%s", shortText.c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", joined(xefgLine, xellLine, "\n").c_str());
            }
        }
    }

    // DLSSG controls
    if (state.activeFgOutput == FGOutput::DLSSG && state.activeFgInput != FGInput::NoFG &&
        state.FGSwapchainExists() && StreamlineProxy::LoadStreamline() && fgOutput)
    {
        ImGui::SeparatorText("Frame Generation (DLSSG)");

        if (state.activeFgNvngx == FGNvngxReplacement::None && (state.hdrOutputActive && outputIsHdr))
        {
            if (state.currentSwapchainDesc.BufferDesc.Format >= DXGI_FORMAT_R32G32B32A32_TYPELESS &&
                state.currentSwapchainDesc.BufferDesc.Format <= DXGI_FORMAT_R16G16B16A16_SINT)
            {
                ImGui::TextColored(toneMapColor(ImVec4(1.0f, 0.0f, 0.0f, 1.f)), "DLSSG only supports HDR10");
            }
        }

        ImGui::Text("Current DLSSG state:");
        ImGui::SameLine();
        if (auto count = DlssgInterpolationCount(state); count > 0)
        {
            ImGui::TextColored(toneMapColor(ImVec4(0.f, 1.f, 0.25f, 1.f)), std::format("ON {}x", count + 1).c_str());
        }
        else
        {
            ImGui::TextColored(toneMapColor(ImVec4(1.f, 0.f, 0.f, 1.f)), "OFF");
        }

        RenderFGFollowGame();

        // While following, the game's DLSS FG setting drives these two
        const bool followsGame = config->FGFollowsGame();

        ImGui::BeginDisabled(followsGame);
        bool fgActive = config->IsFGEnabled();
        if (ImGui::Checkbox("Active##4", &fgActive))
        {
            config->FGEnabled = fgActive;
            LOG_DEBUG("Enabled set FGEnabled: {}", fgActive);

            if (config->IsFGEnabled())
                state.fgChanged = true;
        }
        ImGui::EndDisabled();

        ShowHelpMarker("Enable Frame Generation");

        auto maxInterpolationCount = fgOutput->GetMaxInterpolationCount();

        if (maxInterpolationCount > 1)
        {
            ImGui::SameLine(0.0f, 16.0f);

            ImGui::BeginDisabled(config->FGDLSSGForceDMFG.value_or_default() || followsGame);

            auto currentSet = fgOutput->GetInterpolatedFrameCount() - 1;

            std::string currentIntCountStr = std::to_string(currentSet + 2) + "X";

            ImGui::PushItemWidth(95.0f * menuResScale);

            if (ImGui::BeginCombo("MFG", currentIntCountStr.c_str()))
            {
                for (int i = 0; i < maxInterpolationCount; i++)
                {
                    std::string modeStr = std::to_string(i + 2) + "X";

                    if (ImGui::Selectable(modeStr.c_str(), (currentSet == i)))
                    {
                        LOG_DEBUG("DLSSG Interpolation Count set to: {}", i + 1);
                        config->FGDLSSGInterpolationCount = i + 1;
                    }
                }

                ImGui::EndCombo();
            }

            ImGui::PopItemWidth();

            ShowHelpMarker("Set DLSSG interpolation count");

            ImGui::EndDisabled();

            if (fgOutput->GetDMFGSupport())
            {
                ImGui::SameLine(0.0f, 16.0f);

                if (bool dynamicMFG = config->FGDLSSGForceDMFG.value_or_default();
                    ImGui::Checkbox("Force Dynamic MFG", &dynamicMFG))
                {
                    config->FGDLSSGForceDMFG = dynamicMFG;
                }

                ImGui::BeginDisabled(!config->FGDLSSGForceDMFG.value_or_default());
                static float fpsTarget = config->FGDLSSGFramerateTargetDMFG.value_or_default();
                ImGui::SliderFloat("DMFG FPS Target", &fpsTarget, 0, 200, "%.0f");

                ShowHelpMarker("An active limit of 0 means auto-detect the display refresh rate");

                if (ImGui::Button("Apply Target"))
                {
                    config->FGDLSSGFramerateTargetDMFG = fpsTarget;
                }

                ImGui::SameLine(0.0f, 16.0f);

                if (ImGui::Button("Reset Target"))
                {
                    fpsTarget = 0.0f;
                    config->FGDLSSGFramerateTargetDMFG.reset();
                }

                ImGui::EndDisabled();
            }
        }

        bool useGamesMarkers = config->FGDLSSGUseGamesReflexMarkers.value_or_default();
        // Vulkan games' markers don't reach OptiScaler's Streamline
        ImGui::BeginDisabled(!ReflexHooks::gameIsSendingMarkers() || state.swapchainApi == API::Vulkan);
        if (ImGui::Checkbox("Use Game's Reflex Markers", &useGamesMarkers))
        {
            config->FGDLSSGUseGamesReflexMarkers = useGamesMarkers;
            LOG_DEBUG("Changed set FGDLSSGUseGamesReflexMarkers: {}", useGamesMarkers);
        }
        ImGui::EndDisabled();
    }

    // OptiFG
    if (state.api != API::Vulkan && state.currentFGSwapchain != nullptr && state.activeFgInput == FGInput::Upscaler)
    {
        SeparatorWithHelpMarker("Frame Generation (OptiFG)", "Using upscaler data for FG");

        if (currentFeature != nullptr && !currentFeature->IsFrozen() &&
            ((state.activeFgOutput == FGOutput::FSRFG && FfxApiProxy::IsFGReady()) ||
             (UsesXeFGApi(state.activeFgOutput) && XeFGProxy::Module() != nullptr) ||
             (state.activeFgOutput == FGOutput::DLSSG && StreamlineProxy::Module() != nullptr)))
        {
            if (!Config::Instance()->FGDisableHUDFix.value_or_default() &&
                state.swapchainInteropApi == SwapchainInteropApi::None)
            {
                bool fgHudfix = config->FGHUDFix.value_or_default();

                if (ImGui::Checkbox("HUDFix", &fgHudfix))
                {
                    config->FGHUDFix = fgHudfix;
                    LOG_DEBUG("Enabled set FGHUDFix: {}", fgHudfix);
                    state.clearCapturedHudlesses = true;
                    state.fgChanged = true;
                }

                ShowHelpMarker("Enable HUD stability fix, might cause crashes!");

                ImGui::BeginDisabled(!config->FGHUDFix.value_or_default());

                ImGui::SameLine(0.0f, 16.0f);
                ImGui::PushItemWidth(95.0f * menuResScale);
                int hudFixLimit = config->FGHUDLimit.value_or_default();
                if (ImGui::InputInt("Limit", &hudFixLimit))
                {
                    if (hudFixLimit < 1)
                        hudFixLimit = 1;
                    else if (hudFixLimit > 999)
                        hudFixLimit = 999;

                    config->FGHUDLimit = hudFixLimit;
                    LOG_DEBUG("Enabled set FGHUDLimit: {}", hudFixLimit);
                }
                ShowHelpMarker("Delay HUDless capture, high values might cause crash!");

                ImGui::SameLine(0.0f, 16.0f);
                if (ImGui::Button("Res##2"))
                    _showHudlessWindow = !_showHudlessWindow;

                ImGui::EndDisabled();

                auto hudExtended = config->FGHUDFixExtended.value_or_default();
                if (ImGui::Checkbox("Extended", &hudExtended))
                {
                    LOG_DEBUG("Enabled set FGHUDFixExtended: {}", hudExtended);
                    config->FGHUDFixExtended = hudExtended;
                }
                ShowHelpMarker("Extended format checks for possible HUDless\nMight cause crashes and slowdowns!");
                ImGui::SameLine(0.0f, 16.0f);

                ImGui::BeginDisabled(!config->FGHUDFix.value_or_default());

                auto immediate = config->FGImmediateCapture.value_or_default();
                if (ImGui::Checkbox("Immediate Capture", &immediate))
                {
                    LOG_DEBUG("Enabled set FGImmediateCapture: {}", immediate);
                    config->FGImmediateCapture = immediate;
                }
                ShowHelpMarker("Enables capturing of resources before shader execution.\nIncrease HUDless "
                               "capture chances, but might cause capturing of unnecessary resources.");

                ImGui::PopItemWidth();

                ImGui::EndDisabled();
            }

            bool depthScale = config->FGEnableDepthScale.value_or_default();
            if (ImGui::Checkbox("Scale Depth to fix DLSS RR", &depthScale))
                config->FGEnableDepthScale = depthScale;
            ShowHelpMarker("Fix for DLSS-D wrong depth inputs");

            bool resourceFlip = config->FGResourceFlip.value_or_default();
            if (ImGui::Checkbox("Flip (Unity)", &resourceFlip))
                config->FGResourceFlip = resourceFlip;
            ShowHelpMarker("Flip Velocity & Depth resources of Unity games");

            ImGui::SameLine(0.0f, 16.0f);

            bool resourceFlipOffset = config->FGResourceFlipOffset.value_or_default();
            if (ImGui::Checkbox("Flip Use Offset", &resourceFlipOffset))
                config->FGResourceFlipOffset = resourceFlipOffset;
            ShowHelpMarker("Use height difference as offset");

            ImGui::Spacing();

            if (auto ch = ScopedCollapsingHeader("Advanced OptiFG Settings"); ch.IsHeaderOpen())
            {
                ScopedIndent indent {};

                if (!Config::Instance()->FGDisableHUDFix.value_or_default() &&
                    state.swapchainInteropApi == SwapchainInteropApi::None)
                {
                    ImGui::Spacing();

                    auto rb = config->FGResourceBlocking.value_or_default();
                    if (ImGui::Checkbox("Resource Blocking", &rb))
                    {
                        config->FGResourceBlocking = rb;
                        LOG_DEBUG("Enabled set FGResourceBlocking: {}", rb);
                    }
                    ShowHelpMarker("Block rarely used resources from using as HUDless \n"
                                   "to prevent flickers and other issues\n\n"
                                   "HUDfix enable/disable will reset the block list!");

                    ImGui::SameLine(0.0f, 16.0f);

                    auto rrc = config->FGRelaxedResolutionCheck.value_or_default();
                    if (ImGui::Checkbox("Relaxed Resource Check", &rrc))
                    {
                        config->FGRelaxedResolutionCheck = rrc;
                        LOG_DEBUG("Enabled set FGRelaxedResolutionCheck: {}", rrc);
                    }
                    ShowHelpMarker("Relax resolution checks for HUDless by 32 pixels \n"
                                   "Helps games which use black borders for some \n"
                                   "resolutions and screen ratios (e.g. Witcher 3)");

                    ImGui::BeginDisabled(state.fgResetCapturedResources);
                    ImGui::PushItemWidth(95.0f * menuResScale);
                    if (ImGui::Checkbox("FG Create List", &state.fgCaptureResources))
                    {
                        if (!state.fgCaptureResources)
                            config->FGHUDLimit = 1;
                        else
                            state.fgOnlyUseCapturedResources = false;
                    }

                    ImGui::SameLine(0.0f, 16.0f);
                    if (ImGui::Checkbox("FG Use List", &state.fgOnlyUseCapturedResources))
                    {
                        if (state.fgCaptureResources)
                        {
                            state.fgCaptureResources = false;
                            config->FGHUDLimit = 1;
                        }
                    }

                    ImGui::SameLine(0.0f, 8.0f);
                    ImGui::Text("(%d)", state.fgCapturedResourceCount);

                    ImGui::PopItemWidth();

                    ImGui::SameLine(0.0f, 16.0f);

                    if (ImGui::Button("Reset List"))
                    {
                        LOG_DEBUG("Resetting captured resource list");

                        state.fgResetCapturedResources = true;
                        state.fgOnlyUseCapturedResources = false;
                    }

                    ImGui::EndDisabled();

                    ImGui::Spacing();
                    ImGui::Spacing();
                    if (ImGui::TreeNode("Tracking Settings"))
                    {
                        auto ath = config->FGAlwaysTrackHeaps.value_or_default();
                        if (ImGui::Checkbox("Always Track Heaps", &ath))
                        {
                            config->FGAlwaysTrackHeaps = ath;
                            LOG_DEBUG("Enabled set FGAlwaysTrackHeaps: {}", ath);
                        }
                        ShowHelpMarker("Always track resources, might cause performance issues\n, but also might "
                                       "fix HUDFix related crashes!");

                        auto disableRTV = config->FGHudfixDisableRTV.value_or_default();
                        if (ImGui::Checkbox("Disable RTV Tracking", &disableRTV))
                            config->FGHudfixDisableRTV = disableRTV;
                        ShowHelpMarker("Disable tracking of CreateRenderTargetView\n"
                                       "This might help filtering of wrong HUDless resources");

                        ImGui::SameLine(0.0f, 16.0f);

                        auto disableSRV = config->FGHudfixDisableSRV.value_or_default();
                        if (ImGui::Checkbox("Disable SRV Tracking", &disableSRV))
                            config->FGHudfixDisableSRV = disableSRV;
                        ShowHelpMarker("Disable tracking of CreateShaderResourceView\n"
                                       "This might help filtering of wrong HUDless resources");

                        auto disableUAV = config->FGHudfixDisableUAV.value_or_default();
                        if (ImGui::Checkbox("Disable UAV Tracking", &disableUAV))
                            config->FGHudfixDisableUAV = disableUAV;
                        ShowHelpMarker("Disable tracking of CreateUnorderedAccessView\n"
                                       "This might help filtering of wrong HUDless resources");

                        ImGui::SameLine(0.0f, 16.0f);

                        auto disableOM = config->FGHudfixDisableOM.value_or_default();
                        if (ImGui::Checkbox("Disable OM Tracking", &disableOM))
                            config->FGHudfixDisableOM = disableOM;
                        ShowHelpMarker("Disable tracking of OMSetRenderTargets\n"
                                       "This might help filtering of wrong HUDless resources");

                        auto disableSCR = config->FGHudfixDisableSCR.value_or_default();
                        if (ImGui::Checkbox("Disable SCR Tracking", &disableSCR))
                            config->FGHudfixDisableSCR = disableSCR;
                        ShowHelpMarker("Disable tracking of SetComputeRootDescriptorTable\n"
                                       "This might help filtering of wrong HUDless resources");

                        ImGui::SameLine(0.0f, 16.0f);

                        auto disableSGR = config->FGHudfixDisableSGR.value_or_default();
                        if (ImGui::Checkbox("Disable SGR Tracking", &disableSGR))
                            config->FGHudfixDisableSGR = disableSGR;
                        ShowHelpMarker("Disable tracking of SetGraphicsRootDescriptorTable\n"
                                       "This might help filtering of wrong HUDless resources");

                        ImGui::Spacing();

                        auto disableDI = config->FGHudfixDisableDI.value_or_default();
                        if (ImGui::Checkbox("Disable DI Tracking", &disableDI))
                            config->FGHudfixDisableDI = disableDI;
                        ShowHelpMarker("Disable tracking of DrawInstanced\n"
                                       "This might help filtering of wrong HUDless resources");

                        ImGui::SameLine(0.0f, 16.0f);

                        auto disableDII = config->FGHudfixDisableDII.value_or_default();
                        if (ImGui::Checkbox("Disable DII Tracking", &disableDII))
                            config->FGHudfixDisableDII = disableDII;
                        ShowHelpMarker("Disable tracking of DrawIndexedInstanced\n"
                                       "This might help filtering of wrong HUDless resources");

                        auto disableDispatch = config->FGHudfixDisableDispatch.value_or_default();
                        if (ImGui::Checkbox("Disable Dispatch Tracking", &disableDispatch))
                            config->FGHudfixDisableDispatch = disableDispatch;
                        ShowHelpMarker("Disable tracking of Dispatch\n"
                                       "This might help filtering of wrong HUDless resources");

                        ImGui::TreePop();
                    }
                }

                ImGui::Spacing();
                if (ImGui::TreeNode("Resource Settings"))
                {
                    bool makeMVCopies = config->FGMakeMVCopy.value_or_default();
                    if (ImGui::Checkbox("FG Make MV Copies", &makeMVCopies))
                        config->FGMakeMVCopy = makeMVCopies;
                    ShowHelpMarker("Make a copy of motion vectors to use with OptiFG\n"
                                   "For preventing corruptions that might happen");

                    bool makeDepthCopies = config->FGMakeDepthCopy.value_or_default();
                    if (ImGui::Checkbox("FG Make Depth Copies", &makeDepthCopies))
                        config->FGMakeDepthCopy = makeDepthCopies;
                    ShowHelpMarker("Make a copy of depth to use with OptiFG\n"
                                   "For preventing corruptions that might happen");

                    ImGui::PushItemWidth(115.0f * menuResScale);
                    float depthScaleMax = config->FGDepthScaleMax.value_or_default();
                    if (ImGui::InputFloat("FG Scale Depth Max", &depthScaleMax, 10.0f, 100.0f, "%.1f"))
                        config->FGDepthScaleMax = depthScaleMax;
                    ShowHelpMarker("Depth values will be divided to this value");
                    ImGui::PopItemWidth();

                    ImGui::TreePop();
                }

                ImGui::Spacing();
                if (ImGui::TreeNode("Syncing Settings"))
                {
                    bool useMutexForPresent = config->FGUseMutexForSwapchain.value_or_default();
                    if (ImGui::Checkbox("FG Use Mutex for Present", &useMutexForPresent))
                        config->FGUseMutexForSwapchain = useMutexForPresent;
                    ShowHelpMarker("Use mutex to prevent desync of FG and crashes\n"
                                   "Disabling might improve the perf but decrease stability");

                    ImGui::TreePop();
                }

                ImGui::Spacing();
                ImGui::Spacing();
            }
        }
        else if (currentFeature == nullptr || currentFeature->IsFrozen())
        {
            ImGui::Text("Upscaler is not active"); // Probably never will be visible
        }
        else if (state.activeFgOutput == FGOutput::FSRFG && !FfxApiProxy::IsFGReady())
        {
            ImGui::TextColored(toneMapColor({ 1.0f, 0.0f, 0.0f, 1.0f }),
                               "amd_fidelityfx_dx12.dll is missing!"); // Probably never will be visible
        }
        else if (UsesXeFGApi(state.activeFgOutput) && XeFGProxy::Module() == nullptr)
        {
            ImGui::TextColored(toneMapColor({ 1.0f, 0.0f, 0.0f, 1.0f }),
                               state.activeFgOutput == FGOutput::F5Low
                                   ? "OptiScaler\\plugins\\f5low.dll is missing!"
                                   : "libxess_fg.dll is missing!"); // Probably never will be visible
        }
    }

    if (state.swapchainApi == API::Vulkan && state.currentVkFG != nullptr && state.activeFgInput == FGInput::Upscaler)
    {
        ImGui::SeparatorText("OptiFG Settings (Vulkan)");

        if (!config->FGDisableHUDFix.value_or_default())
        {
            bool fgHudfix = config->FGHUDFix.value_or_default();
            if (ImGui::Checkbox("HUDFix", &fgHudfix))
                config->FGHUDFix = fgHudfix;
            ShowHelpMarker("Enable HUD stability fix, might cause crashes!");

            ImGui::BeginDisabled(!fgHudfix);

            ImGui::SameLine(0.0f, 16.0f);
            ImGui::PushItemWidth(95.0f * menuResScale);
            int hudFixLimit = config->FGHUDLimit.value_or_default();
            if (ImGui::InputInt("Limit", &hudFixLimit))
                config->FGHUDLimit = std::clamp(hudFixLimit, 1, 999);
            ShowHelpMarker("Delay HUDless capture, high values might cause crash!");
            ImGui::PopItemWidth();

            ImGui::SameLine(0.0f, 16.0f);
            auto hudExtended = config->FGHUDFixExtended.value_or_default();
            if (ImGui::Checkbox("Extended", &hudExtended))
                config->FGHUDFixExtended = hudExtended;
            ShowHelpMarker("Accept HUDless candidates of any format");

            auto uiTexture = config->FGHUDFixUITexture.value_or_default();
            if (ImGui::Checkbox("UI Texture", &uiTexture))
                config->FGHUDFixUITexture = uiTexture;
            ShowHelpMarker("Use the UI texture the game blends on the frame in its last compute pass");

            ImGui::BeginDisabled(!uiTexture);
            ImGui::SameLine(0.0f, 16.0f);

            auto uiUnblend = config->FGHUDFixUIUnblend.value_or(state.activeFgOutput != FGOutput::DLSSG);
            if (ImGui::Checkbox("Unblend UI", &uiUnblend))
                config->FGHUDFixUIUnblend = uiUnblend;
            ShowHelpMarker("Rebuild the HUDless by removing the UI texture from the frame\n"
                        "FSR-FG gets the UI only this way, DLSSG also takes the UI texture directly");

            ImGui::EndDisabled();

            ImGui::EndDisabled();
    
        }

        bool resourceFlip = config->FGResourceFlip.value_or_default();
        if (ImGui::Checkbox("Flip (Unity)", &resourceFlip))
            config->FGResourceFlip = resourceFlip;
        ShowHelpMarker("Flip Velocity & Depth resources of Unity games");

        ImGui::SameLine(0.0f, 16.0f);

        bool resourceFlipOffset = config->FGResourceFlipOffset.value_or_default();
        if (ImGui::Checkbox("Flip Use Offset", &resourceFlipOffset))
            config->FGResourceFlipOffset = resourceFlipOffset;
        ShowHelpMarker("Use height difference as offset");
    }

    const FGNvngxReplacement activeNvngxFg = state.activeFgNvngx;
    if (activeNvngxFg != FGNvngxReplacement::None)
    {
        if (activeNvngxFg == FGNvngxReplacement::Nukems)
        {
            SeparatorWithHelpMarker("Frame Generation (FSR3-FG via Nukem's DLSSG)",
                                    "Requires Nukem's dlssg_to_fsr3 dll");

            if (!state.nukemsFgFileAvailable)
            {
                ImGui::TextColored(toneMapColor(ImVec4(1.f, 0.f, 0.f, 1.f)),
                                   "Please put dlssg_to_fsr3_amd_is_better.dll into OptiScaler folder");
            }
        }
        else if (activeNvngxFg == FGNvngxReplacement::Arturs)
        {
            SeparatorWithHelpMarker("Frame Generation (FSR3-MFG via DLSS Enabler)",
                                    "DLSS Enabler as dlss-enabler-headless.dll");

            if (!state.artursFgFileAvailable)
            {
                ImGui::TextColored(toneMapColor(ImVec4(1.f, 0.f, 0.f, 1.f)),
                                   "Please put dlss-enabler-headless.dll into OptiScaler folder");
            }

            ImGui::TextColored(toneMapColor(ImVec4(1.f, 0.8f, 0.f, 1.f)),
                               "Using a subset of features from DLSS Enabler");
        }
        else if (activeNvngxFg == FGNvngxReplacement::FFX)
        {
            SeparatorWithHelpMarker("Frame Generation (FSRFG via FFX)", "FFX using the DLSSG swapchain");
        }
        else if (activeNvngxFg == FGNvngxReplacement::Combo)
        {
            SeparatorWithHelpMarker("Frame Generation (Enabler + FFX)",
                                    "FFX for middle fake frames, and Enabler for the rest\n\n2x - FFX\n"
                                    "3x - Enabler\n4x - FFX + Enabler\n5x - Enabler\n6x - FFX + Enabler");
        }

        if (state.activeFgInput == FGInput::NvngxFG)
        {

            bool dmfgActive = state.dlssgGameDMFGSupported && config->FGDLSSGOverrideForceDMFG.value_or_default();

            if (!ReflexHooks::isReflexHooked())
            {
                ImGui::TextColored(toneMapColor(ImVec4(1.f, 0.f, 0.f, 1.f)), "Reflex not hooked");
                ImGui::Text("If you are using an AMD/Intel GPU, then make sure you have Fakenvapi");
            }
            else if (ReflexHooks::dlssgFrameCountToGenerate() == 0 && !dmfgActive)
            {
                ImGui::Text("Please select DLSS Frame Generation in the game options\n"
                            "You might need to select DLSS first");
            }

            if (state.swapchainApi == DX12)
            {
                ImGui::Text("Current DLSSG state:");
                ImGui::SameLine();
                if (auto count = state.dlssgDetectedInterpolationCount; count > 0)
                {
                    ImGui::TextColored(toneMapColor(ImVec4(0.f, 1.f, 0.25f, 1.f)),
                                       std::format("ON {}x", count + 1).c_str());
                }
                else
                {
                    ImGui::TextColored(toneMapColor(ImVec4(1.f, 0.f, 0.f, 1.f)), "OFF");
                }

                // Issue mostly shows up on AMD on Windows on pre-RDNA3 in some non-UE games
                // Hide to reduce confusion, config is still read
                const bool isUnrealEngine = State::Instance().NVNGX_Engine == NVSDK_NGX_ENGINE_TYPE_UNREAL ||
                                            State::Instance().gameQuirks & GameQuirk::ForceUnrealEngine;
                const bool isDllProxyNvngxType =
                    activeNvngxFg == FGNvngxReplacement::Nukems || activeNvngxFg == FGNvngxReplacement::Arturs;
                if (isDllProxyNvngxType && !primaryGpu.dlssCapable && primaryGpu.fsr4Support == FSR4Support::None &&
                    !primaryGpu.usesVkd3dProton && !isUnrealEngine)
                {
                    if (bool makeDepthCopy = config->NvngxFGMakeDepthCopy.value_or_default();
                        ImGui::Checkbox("Fix broken visuals", &makeDepthCopy))
                    {
                        config->NvngxFGMakeDepthCopy = makeDepthCopy;
                    }
                    ShowHelpMarker("Makes a copy of the depth buffer\nCan fix broken visuals in some games on AMD "
                                   "GPUs under Windows\nCan cause stutters, so best to use only when necessary");
                }
            }
            else if (state.swapchainApi == Vulkan)
            {
                ImGui::TextColored(toneMapColor(ImVec4(1.f, 0.8f, 0.f, 1.f)),
                                   "DLSSG is purposefully disabled when this menu is visible");
                ImGui::Spacing();
            }
        }

        bool isLoaded = false;
        if (state.swapchainApi == Vulkan)
            isLoaded = Nvngx_FG::isVulkanAvailable();
        if (state.swapchainApi == DX12)
            isLoaded = Nvngx_FG::isDx12Available();

        if (isLoaded)
        {
            if (activeNvngxFg == FGNvngxReplacement::Arturs || activeNvngxFg == FGNvngxReplacement::Combo)
            {
                auto featureVer = Nvngx_FG::version();
                auto antighostingVer = Nvngx_FG::extraVersion();
                ImGui::Text("DE Ver: %d.%d.%d.%d   GB Ver: %d.%d", featureVer.major, featureVer.minor, featureVer.patch,
                            featureVer.reserved, antighostingVer.major, antighostingVer.minor);

                static std::vector<FlagDefinition> common_flags = {
                    { "Antighosting (GB)", 0x00100000, "Enable anti-ghosting correction" },
                    { "Temporal HUD pin", 0x04000000, "Enable temporal HUD pinning (present-backbuffer stability)" }
                };

                static std::vector<FlagDefinition> uncommon_flags = {
                    //{ "Hudless UI mask", 0x02000000, "Use HUD-less as UI mask (DL2 inverted semantics)" },
                    { "HUD interpolation", 0x08000000, "HUD OF interpolation (0=legacy pin-present, 1=OF warp)" },
                    { "Ignore UI texture", 0x10000000, "Ignore dedicated DLSSG.UI texture (force legacy HUD path)" },
                    //{ "Dp4a active", 0x20000000, "OF pipeline using dp4a-accelerated SSD (SM 6.4+)" },
                    { "Pin backbuffer", 0x40000000, "Pin DLSSG.Backbuffer to subframe-1 snapshot across MFG frame" }
                };

                static std::vector<FlagDefinition> debug_flags = {
                    { "Antighosting red tint", 0x00200000, "Debug: red tint on corrected pixels" },
                    { "Antighosting split screen", 0x00400000, "Debug: split screen comparison" },
                    { "Frame index line", 0x00010000, "" },
                    { "HUD detection", 0x00020000, "" },
                    { "Disocclusion tint", 0x00040000, "" },
                    { "Artifacts detection", 0x00080000, "" },
                    { "Camera MV debug", 0x00800000, "Debug: blue tint where camera MV fallback is used" },
                    { "Generic visualization", 0x01000000, "Debug: trapezoid zone visualization" }
                };

                uint32_t temp_flags = config->NvngxFGDispatchFlags.value_or_default();
                bool changed = false;

                ImGui::Text("Raw DispatchFlags:");
                changed |= ImGui::InputScalar("##RawFlags", ImGuiDataType_U32, &temp_flags, NULL, NULL, "%08X",
                                              ImGuiInputTextFlags_CharsHexadecimal);

                ImGui::SameLine(0.0f, 20.0f * menuResScale);
                if (bool showDebug = config->NvngxFGShowDebug.value_or_default();
                    ImGui::Checkbox("Show Debug", &showDebug))
                {
                    config->NvngxFGShowDebug = showDebug;
                }
                ShowHelpMarker("Required for Debug flags to work correctly");

                ImGui::Spacing();

                if (auto ch = ScopedCollapsingHeader("Active DispatchFlags"); ch.IsHeaderOpen())
                {
                    ScopedIndent indent {};

                    auto render_flags = [&](const std::vector<FlagDefinition>& flags)
                    {
                        for (const auto& flag : flags)
                        {
                            changed |= ImGui::CheckboxFlags(flag.name.c_str(), &temp_flags, flag.mask);

                            if (ImGui::IsItemHovered() && !flag.description.empty())
                            {
                                ImGui::SetTooltip("%s", flag.description.c_str());
                            }
                        }
                    };

                    ImGui::TextDisabled("Common");
                    render_flags(common_flags);

                    ImGui::Spacing();
                    ImGui::TextDisabled("Uncommon");
                    render_flags(uncommon_flags);

                    if (config->NvngxFGShowDebug.value_or_default())
                    {
                        ImGui::Spacing();
                        ImGui::TextDisabled("Debug");
                        render_flags(debug_flags);
                    }
                }

                if (changed)
                {
                    config->NvngxFGDispatchFlags = temp_flags;
                }
            }

            if (activeNvngxFg == FGNvngxReplacement::Nukems)
            {
                if (ImGui::Checkbox("Enable Debug View", &state.dlssgDebugView))
                {
                    Nvngx_FG::setDebugView(state.dlssgDebugView);
                }
                if (ImGui::Checkbox("Interpolated frames only", &state.dlssgInterpolatedOnly))
                {
                    Nvngx_FG::setInterpolatedOnly(state.dlssgInterpolatedOnly);
                }
            }

            if (activeNvngxFg == FGNvngxReplacement::FFX || activeNvngxFg == FGNvngxReplacement::Combo)
            {
                if (_ffxFGIndex < 0 || _ffxFGIndex >= (int) state.ffxFGVersionNames.size())
                    _ffxFGIndex = config->FfxFGIndex.value_or_default();

                if (state.ffxFGVersionNames.size() > 0)
                {
                    ImGui::PushItemWidth(135.0f * menuResScale);

                    auto currentName = StrFmt("FSR %s", state.ffxFGVersionNames[_ffxFGIndex]);
                    if (ImGui::BeginCombo("FFX FG", currentName.c_str()))
                    {
                        for (int n = 0; n < state.ffxFGVersionIds.size(); n++)
                        {
                            auto name = StrFmt("FSR %s", state.ffxFGVersionNames[n]);
                            if (ImGui::Selectable(name.c_str(), config->FfxFGIndex.value_or_default() == n))
                                _ffxFGIndex = n;
                        }

                        ImGui::EndCombo();
                    }
                    ImGui::PopItemWidth();

                    ShowHelpMarker("List of FGs reported by FFX SDK");

                    ImGui::SameLine(0.0f, 6.0f);

                    if (ImGui::Button("Change FG") && _ffxFGIndex != config->FfxFGIndex.value_or_default())
                    {
                        config->FfxFGIndex = _ffxFGIndex;
                        state.fgChanged = true;
                    }
                }

                bool fgAsync = config->FGAsync.value_or_default();
                if (ImGui::Checkbox("Allow Async##2", &fgAsync))
                {
                    config->FGAsync = fgAsync;

                    if (config->IsFGEnabled())
                    {
                        state.fgChanged = true;
                        LOG_DEBUG("Async set FGChanged");
                    }
                }
                ShowHelpMarker("Enable Async for better FG performance\nMight cause crashes, especially with HUD Fix!");

                ImGui::SameLine(0.0f, 20.0f * menuResScale);
                bool fgDV = config->FGDebugView.value_or_default();
                if (ImGui::Checkbox("Debug View##3", &fgDV))
                {
                    config->FGDebugView = fgDV;

                    if (config->IsFGEnabled())
                    {
                        state.fgChanged = true;
                        LOG_DEBUG("DebugView set FGChanged");
                    }
                }
                ShowHelpMarker("Enable FSR3.1-FG Debug view\n\n"
                               "Top left: Game Motion Vectors\n"
                               "Top middle: GMV Depth\n"
                               "Top right: Optical Flow MV\n"
                               "Middle: Interpolated frame only\n"
                               "Bottom left: Disocclusion mask\n"
                               "Bottom middle: Interpolation source (w/o UI)\n"
                               "Bottom right: HUDless resource");

                if (Nvngx_FG::version().major > 3)
                {
                    ImGui::SameLine(0.0f, 20.0f * menuResScale);
                    if (bool fgwm = config->FSRFGEnableWatermark.value_or_default();
                        ImGui::Checkbox("Enable Watermark", &fgwm))
                    {
                        LOG_DEBUG("FSRFGEnableWatermark set FGWatermark: {}", fgwm);
                        config->FSRFGEnableWatermark = fgwm;
                        Util::SetFlagEnvVar(L"MLFI-WATERMARK", fgwm);

                        // The driver reads the variable on context creation
                        if (config->IsFGEnabled())
                            state.fgChanged = true;
                    }
                }
            }

            if (bool disableHudless = config->NvngxFGDisableHudless.value_or_default();
                ImGui::Checkbox("Disable HUDless", &disableHudless))
            {
                config->NvngxFGDisableHudless = disableHudless;
            }
            ShowHelpMarker("Might be required for some sets of DispatchFlags");
        }
    }

    // FSR-FG Inputs
    if (state.currentFGSwapchain != nullptr &&
        (state.activeFgInput == FGInput::FSRFG || state.activeFgInput == FGInput::FSRFG30))
    {
        SeparatorWithHelpMarker("Frame Generation (FSR-FG Inputs)", "Select FSR-FG in-game");

        auto fgOutput = reinterpret_cast<IFGFeature_Dx12*>(state.currentFG);
        if (fgOutput != nullptr)
        {
            ImGui::Text("Current FSR-FG state:");
            ImGui::SameLine();
            if (state.fsrfgInputActive)
            {
                if (fgOutput->IsActive())
                    ImGui::TextColored(toneMapColor(ImVec4(0.f, 1.f, 0.25f, 1.f)), "ON");
                else
                    ImGui::TextColored(toneMapColor(ImVec4(1.0f, 0.647f, 0.0f, 1.f)), "ACTIVATE FG");
            }
            else
            {
                ImGui::TextColored(toneMapColor(ImVec4(1.f, 0.f, 0.f, 1.f)), "OFF");
                ImGui::Text("Please select FSR Frame Generation in the game options\n"
                            "You might need to select FSR first");
            }
        }

        bool skipConfig = config->FSRFGSkipConfigForHudless.value_or_default();
        if (ImGui::Checkbox("Skip Config for HUDless", &skipConfig))
            config->FSRFGSkipConfigForHudless = skipConfig;

        ShowHelpMarker("Do not use HUDless set at ffxConfig");

        ImGui::SameLine(0.0f, 6.0f);

        bool skipDispatch = config->FSRFGSkipDispatchForHudless.value_or_default();
        if (ImGui::Checkbox("Skip Dispatch for HUDless", &skipDispatch))
            config->FSRFGSkipDispatchForHudless = skipDispatch;

        ShowHelpMarker("Do not use HUDless set at ffxDispatch");
    }

    // Streamline FG Inputs
    if (state.currentFGSwapchain != nullptr && state.activeFgInput == FGInput::DLSSG)
    {
        SeparatorWithHelpMarker("Frame Generation (Streamline FG Inputs)", "Select DLSS-FG in-game");

        auto fgOutput = reinterpret_cast<IFGFeature_Dx12*>(state.currentFG);

        if (!ReflexHooks::isReflexHooked())
        {
            ImGui::TextColored(toneMapColor(ImVec4(1.f, 0.f, 0.f, 1.f)), "Reflex not hooked");
            ImGui::Text("If you are using an AMD/Intel GPU, then make sure you have fakenvapi");
        }
        else if (fgOutput != nullptr)
        {
            ImGui::Text("Current Streamline FG state:");
            ImGui::SameLine();
            if ((state.fgLastFrame - state.dlssgLastFrame) < 3)
            {
                if (fgOutput->IsActive())
                    ImGui::TextColored(toneMapColor(ImVec4(0.f, 1.f, 0.25f, 1.f)), "ON");
                else
                    ImGui::TextColored(toneMapColor(ImVec4(1.0f, 0.647f, 0.0f, 1.f)), "ACTIVATE FG");
            }
            else
            {
                ImGui::TextColored(toneMapColor(ImVec4(1.f, 0.f, 0.f, 1.f)), "OFF");
                ImGui::Text("Please select DLSS Frame Generation in the game options\n"
                            "You might need to select DLSS first");
            }
        }
    }
}

void MenuCommon::RenderFsrCommonSettings(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    auto& currentFeature = ctx.currentFeature;

    if (currentFeature != nullptr && !currentFeature->IsFrozen())
    {
        // FSR Common -----------------
        if (currentFeature != nullptr && !currentFeature->IsFrozen() &&
            (state.activeFgOutput == FGOutput::FSRFG || IsFsr(currentBackend)))
        {
            SeparatorWithHelpMarker("FSR Common Settings", "Affects both FSR-FG & Upscalers");

            bool useFsrVales = config->FsrUseFsrInputValues.value_or_default();
            if (ImGui::Checkbox("Use FSR Input Values", &useFsrVales))
                config->FsrUseFsrInputValues = useFsrVales;

            ImGui::Spacing();
            if (auto ch = ScopedCollapsingHeader("FoV & Camera Values"); ch.IsHeaderOpen())
            {
                ScopedIndent indent {};
                ImGui::Spacing();

                bool useVFov = config->FsrVerticalFov.has_value() || !config->FsrHorizontalFov.has_value();

                float vfov = config->FsrVerticalFov.value_or_default();
                float hfov = config->FsrHorizontalFov.value_or(90.0f);

                if (useVFov && !config->FsrVerticalFov.has_value())
                    config->FsrVerticalFov = vfov;
                else if (!useVFov && !config->FsrHorizontalFov.has_value())
                    config->FsrHorizontalFov = hfov;

                if (ImGui::RadioButton("Use Vert. Fov", useVFov))
                {
                    config->FsrHorizontalFov.reset();
                    config->FsrVerticalFov = vfov;
                    useVFov = true;
                }

                ImGui::SameLine(0.0f, 6.0f);

                if (ImGui::RadioButton("Use Horz. Fov", !useVFov))
                {
                    config->FsrVerticalFov.reset();
                    config->FsrHorizontalFov = hfov;
                    useVFov = false;
                }

                if (useVFov)
                {
                    if (ImGui::SliderFloat("Vert. FOV", &vfov, 0.0f, 180.0f, "%.1f"))
                        config->FsrVerticalFov = vfov;

                    ShowHelpMarker("Might help achieve better image quality");
                }
                else
                {
                    if (ImGui::SliderFloat("Horz. FOV", &hfov, 0.0f, 180.0f, "%.1f"))
                        config->FsrHorizontalFov = hfov;

                    ShowHelpMarker("Might help achieve better image quality");
                }

                float cameraNear;
                float cameraFar;

                cameraNear = config->FsrCameraNear.value_or_default();
                cameraFar = config->FsrCameraFar.value_or_default();

                if (ImGui::SliderFloat("Camera Near", &cameraNear, 0.1f, 500000.0f, "%.1f"))
                    config->FsrCameraNear = cameraNear;
                ShowHelpMarker("Might help achieve better image quality\n"
                               "And potentially less ghosting");

                if (ImGui::SliderFloat("Camera Far", &cameraFar, 0.1f, 500000.0f, "%.1f"))
                    config->FsrCameraFar = cameraFar;
                ShowHelpMarker("Might help achieve better image quality\n"
                               "And potentially less ghosting");

                if (ImGui::Button("Reset Camera Values"))
                {
                    config->FsrVerticalFov.reset();
                    config->FsrHorizontalFov.reset();
                    config->FsrCameraNear.reset();
                    config->FsrCameraFar.reset();
                }

                ImGui::SameLine(0.0f, 6.0f);
                ImGui::Text("Near: %.1f Far: %.1f",
                            state.lastFsrCameraNear < 500000.0f ? state.lastFsrCameraNear : 500000.0f,
                            state.lastFsrCameraFar < 500000.0f ? state.lastFsrCameraFar : 500000.0f);

                ImGui::Spacing();
                ImGui::Spacing();
            }
        }
    }
}

// Low latency: "Input" (who makes the latency calls) and "Output" (what lowers latency), side by side as on the frame
// generation selection, and one status for both. The rules are in native/NativeLowLatencyRule.h; the text of the status
// comes from native::lowlatency::GetStatus, the same Decision the code runs on.
void MenuCommon::RenderFramerateLowLatency(RenderMenuContext& ctx)
{
    using namespace native::lowlatency;

    auto& state = ctx.state;
    auto config = ctx.config;

    SeparatorWithHelpMarker("Low latency", "Keeps the game's render queue short, so what you do shows up sooner.\n"
                                           "Reflex on NVIDIA; Anti-Lag 2 (Anti-Lag Vk on Vulkan), XeLL or "
                                           "LatencyFlex elsewhere; with XeFG: XeLL, or Reflex or Anti-Lag 2 beside "
                                           "it.");

    const auto status = GetStatus();
    const auto input = config->FramerateLowLatencyInput.value_or_default();
    const bool frameGenerationNeedsReflex = FrameGenerationNeedsReflex();
    const bool forceXellActive = state.activeFgInput == FGInput::ForceXeLL;
    const OutputInputs outputInputs = CurrentOutputInputs();
    const bool outputLocked = OutputLocked(outputInputs);
    const bool inputOff = input == Input::Off;

    if (ImGui::BeginTable("lowLatencySelection", 2, ImGuiTableFlags_SizingStretchSame))
    {
        ImGui::TableNextColumn();

        // clang-format off
        std::vector<MenuOption<Input>> inputOptions = {
            { Input::Auto, "Auto",
                "The game's own Reflex when it makes the calls.\n"
                "Otherwise OptiScaler's, while an Optical F5Low mode, XeFG or FSR FG runs.\n"
                "With Optical F5Low's NR + frame generation it is Game: only the game's own Reflex, "
                "even with XeFG or FSR FG." },
            { Input::Game, "Game",
                "Only the game's own Reflex. OptiScaler makes no latency calls." },
            { Input::OptiScaler, "OptiScaler",
                "OptiScaler's calls in any game without Reflex.\n"
                "It steps aside when the game starts using Reflex." },
            { Input::Off, "Off",
                "No low latency. OptiScaler makes no calls and the game's Reflex is sent with low latency off.\n"
                "OptiScaler's limiter keeps the frame cap." },
        };
        // clang-format on
        inputOptions[(size_t) Input::Off].set_disabled(frameGenerationNeedsReflex, "DLSS FG needs the game's Reflex");

        PopulateCombo("Input##lowlatencyinput", config->FramerateLowLatencyInput, inputOptions);
        ShowHelpMarker("Who makes the latency calls (Reflex, or what stands in for it).\n"
                       "Leave it on Auto unless a game misbehaves: Game or Off if OptiScaler's calls get in the way, "
                       "OptiScaler to start them in a game without Reflex.");

        ImGui::TableNextColumn();

        Output shown = OutputShown(outputInputs, config->ForceXeLL.value_or_default(),
                                   config->FN_ForceLatencyFlex.value_or_default(),
                                   config->FramerateFrameCapOnly.value_or_default());
        const Output shownBefore = shown;

        const char* autoName = AutoEntryName(outputInputs);
        const std::string autoLabel = autoName != nullptr ? autoName : std::string("Auto (") + status.output + ")";
        const bool xefgOutput = outputInputs.xefgOutput;

        // With XeFG the choices are the ones that pair with it (XeLL it always needs, Reflex or Anti-Lag 2 beside it);
        // without it Auto and the forced ones
        // clang-format off
        std::vector<MenuOption<Output>> outputOptions;

        if (xefgOutput)
        {
            outputOptions = {
                { Output::XeLL, "XeLL",
                    "XeFG's own XeLL lowers latency." },
                { Output::Reflex, "NVIDIA Reflex",
                    "Reflex lowers latency, as with FSR FG. XeLL stays in place for XeFG only, with its low latency off." },
                { Output::AntiLag2, "Anti-Lag 2",
                    "Anti-Lag 2 lowers latency. XeLL stays in place for XeFG only, with its low latency off." },
            };
        }
        else
        {
            outputOptions = {
                { Output::Auto, autoLabel,
                    autoName != nullptr
                        ? "Picked for this card: its driver's Reflex lowers latency and keeps the frame cap."
                        : "Picked for this setup: NVIDIA Reflex, or Anti-Lag 2 / XeLL / LatencyFlex on other cards." },
                { Output::XeLL, "XeLL",
                    "XeLL on any card, without frame generation: OptiScaler's frame generation stays off.\n"
                    "Needs a restart." },
                { Output::LatencyFlex, "LatencyFlex",
                    "LatencyFlex instead of Anti-Lag 2 or XeLL." },
                { Output::FrameCapOnly, "Frame cap only",
                    "Only moves the FPS Limit's wait to just before the game reads input. While the cap is reached\n"
                    "that shows input up to a frame sooner; it does nothing else.\n"
                    "Replaces Anti-Lag 2, XeLL or LatencyFlex, which lower latency more: use it when they misbehave\n"
                    "in a game. Does nothing without an FPS Limit." },
            };
        }
        // clang-format on

        for (auto& option : outputOptions)
        {
            const auto choice = OutputAvailability(option.value, outputInputs);
            option.set_hidden(choice.hidden);
            option.set_disabled(!choice.available, choice.why);
        }

        // Nothing to lower while the Input is Off; with XeFG on a setup that has only XeLL there is nothing to choose
        ImGui::BeginDisabled(outputLocked || inputOff);
        PopulateCombo("Output##lowlatencyoutput", shown, outputOptions);
        ImGui::EndDisabled();

        if (inputOff)
            ShowTooltip("Nothing to lower while the Input is Off.");
        else if (outputLocked)
        {
            // The details (Vulkan extensions, which nvapi) are in the log
            ShowTooltip(outputInputs.antiLag2Failed ? "XeFG uses XeLL here: Anti-Lag 2 could not start in this game."
                        : outputInputs.reflexFailed ? "XeFG uses XeLL here: Reflex could not start in this game."
                                                    : "XeFG uses XeLL here: this setup has neither NVIDIA Reflex "
                                                      "nor Anti-Lag 2 to put beside it.");
        }

        if (xefgOutput)
        {
            ShowHelpMarker("What lowers latency while XeFG generates frames. XeFG always keeps an XeLL; Reflex or "
                           "Anti-Lag 2 do the work instead when you pick them.\n"
                           "Leave it as it is unless a game stutters or misbehaves with it.");
        }
        else
        {
            ShowHelpMarker("What lowers latency. The first entry is picked for this setup: leave it there unless a "
                           "game stutters or misbehaves with it.\n"
                           "Frame cap only just moves the FPS Limit's wait to before the game reads input.");
        }

        if (!outputLocked && !inputOff && shown != shownBefore)
        {
            if (xefgOutput)
            {
                config->FGXeFGLowLatencyOutput = shown == Output::Reflex
                                                     ? XefgLatency::Reflex
                                                     : (shown == Output::AntiLag2 ? XefgLatency::AntiLag2
                                                                                  : XefgLatency::XeLL);
            }
            else
            {
                config->ForceXeLL = shown == Output::XeLL;
                config->FN_ForceLatencyFlex = shown == Output::LatencyFlex;
                config->FramerateFrameCapOnly = shown == Output::FrameCapOnly;
            }
        }

        // Boost, NVIDIA Reflex only
        const bool boostAvailable = BoostAvailable(input, outputInputs, forceXellActive,
                                                   config->FN_ForceReflex.value_or_default() != ForceReflex::InGame);
        bool boost = config->DlssNrNativeLowLatencyBoost.value_or_default();

        ImGui::BeginDisabled(!boostAvailable);
        if (ImGui::Checkbox("Boost##lowlatencyboost", &boost))
            config->DlssNrNativeLowLatencyBoost = boost;
        ImGui::EndDisabled();

        std::string boostTip = "The GPU keeps its clocks up between frames: a little lower latency, a little more power.\n"
                               "NVIDIA Reflex only, in OptiScaler's calls.";

        if (!boostAvailable)
        {
            boostTip += RuleForInput(input).setting == Setting::Off
                            ? "\nNot available: the Input is Game or Off, so OptiScaler makes no calls."
                            : "\nNot available: the output is not NVIDIA Reflex.";
        }

        ShowHelpMarker(boostTip.c_str());

        ImGui::EndTable();
    }

    // Force XeLL takes effect at the next start, as it did in the fakenvapi section
    if (config->ForceXeLL.value_or_default() != forceXellActive)
    {
        ImGui::Spacing();
        ImGui::TextColored(toneMapColor(ImVec4(1.f, 0.f, 0.0f, 1.f)), "Save INI and restart to apply the changes");
        ImGui::Spacing();
    }

    // One status for both: who makes the calls and what they reach, then the reason
    std::string output = status.output;

    if (state.rtssReflexInjection)
        output += " (RTSS)";

    // Frame cap only waits whatever the game's low latency setting says
    if ((fakenvapi::isUsingAsMainNvapi() || InputCommon::current_mode() == LowLatencyMode::XeLL) &&
        InputCommon::current_mode() != LowLatencyMode::FpsLimiter && !InputCommon::low_latency_active() &&
        state.reflexLimitsFps)
        output += " (inactive)";

    const char* who = ActiveInputText(status.decision, ReflexHooks::gameCalledReflex(), XellHooks::canLimit(),
                                      frameGenerationNeedsReflex);

    if (status.decision == Decision::LowLatencyOff)
        ImGui::TextUnformatted("Active: Off");
    else if (std::strcmp(who, "None") == 0)
        ImGui::TextUnformatted("Active: nothing makes latency calls");
    else
        ImGui::Text("Active: %s -> %s", who, output.c_str());

    // Run for frame generation alone, the player may never have opened a page that says why
    if (status.decision == Decision::Run && status.runsFor == RunsFor::Xefg)
    {
        ShowHelpMarker("The game has no Reflex, so OptiScaler makes the calls. XeFG always keeps an XeLL, which "
                       "lowers latency only when something sleeps and marks the frames; with Reflex or Anti-Lag 2 as "
                       "the Output they do that instead.");
    }
    else if (status.decision == Decision::Run && status.runsFor == RunsFor::FsrFg)
    {
        ShowHelpMarker("The game has no Reflex, so OptiScaler makes the calls. Frame generation holds a real frame "
                       "back to make the one in between; these calls take that latency back.");
    }

    ImGui::TextWrapped("Status: [%s]: %s", StatusLabel(status.decision), ReasonText(status));

    if (status.offKeptForFrameGeneration)
        ImGui::TextDisabled("DLSS FG needs Reflex: the game's Reflex stays on.");

    // XeFG with Reflex or Anti-Lag 2: say what XeLL is doing, or it looks like a second method running
    if (status.xefgSide != XefgLatency::XeLL)
        ImGui::TextDisabled("XeLL stays in place for XeFG only. Its low latency is off.");

    // Asked for, but this setup or fakenvapi's Force Reflex keeps it with XeLL (the Input being Off says so on its own)
    if (outputInputs.xefgOutput && !inputOff && outputInputs.xefgLatency != XefgLatency::XeLL &&
        status.xefgSide == XefgLatency::XeLL)
    {
        ImGui::TextDisabled(config->FN_ForceReflex.value_or_default() != ForceReflex::InGame
                                ? "The chosen output is not used: Force Reflex is set in the fakenvapi settings."
                                : "The chosen output is not used: this setup does not have it.");
    }

    if (status.decision == Decision::Run && status.hasLatency)
    {
        // Through fakenvapi there are no GPU times: the frame's simulation start to its present's end
        if (status.latencyEstimated)
            ImGui::Text("Simulation to present (estimated from frame markers): %.1f ms", status.latencyMs);
        else
            ImGui::Text("Measured latency: %.1f ms", status.latencyMs);
    }

    // On NVIDIA and AMD XeLL does nothing while XeFG generates no frames (paused, off, exclusive full screen, or
    // fakenvapi's own XeLL with no XeFG)
    if (status.decision == Decision::Run && status.xellIdle)
        ImGui::TextDisabled("XeLL does nothing on this GPU while XeFG generates no frames.");

    // Who keeps the frame cap: one wait per frame (NativeLowLatencyRule.h PlanCap). The cap in force, Dynamic MFG's
    // "Use as FPS limit" included.
    if (const float cap = FrameLimit::CapFps(); cap > 0.0f)
    {
        std::string keeper = "OptiScaler's limiter";
        bool outputKeeps = false; // the low latency output keeps it (not the limiter or the game's XeLL)

        if (status.decision == Decision::Run && status.path[0] != '\0')
        {
            if (status.capOwner != CapOwner::Limiter)
            {
                keeper = status.path;
                outputKeeps = true;
            }
        }
        else if (state.reflexLimitsFps)
        {
            keeper = status.output;
            outputKeeps = true;
        }
        else if (XellHooks::canLimit())
        {
            keeper = "the game's XeLL";
        }

        // Frame cap only is nothing but the cap: say where it waits
        if (outputKeeps && InputCommon::current_mode() == LowLatencyMode::FpsLimiter)
            keeper = "Frame cap only, before the game reads input";

        ImGui::TextDisabled("Frame cap %.0f fps: kept by %s", cap, keeper.c_str());
    }
    else if (InputCommon::current_mode() == LowLatencyMode::FpsLimiter)
    {
        ImGui::TextDisabled("Frame cap only does nothing until an FPS Limit is set.");
    }

    ImGui::Spacing();
}

void MenuCommon::RenderFramerateSettings(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    auto& menuResScale = ctx.menuResScale;

    // Framerate ---------------------
    // While Optical F5Low's low latency runs, it or OptiScaler's limiter keeps the cap (native/NativeLowLatency.h
    // OwnsFrameCap); the section stays up either way, so it does not come and go as frame generation pauses
    const bool f5lowRunning = native::lowlatency::Running();

    if (state.reflexLimitsFps || f5lowRunning || config->OverlayMenu.value_or_default())
    {
        SeparatorWithHelpMarker(
            "Framerate", "Uses Reflex when possible\nOn AMD/Intel cards, you can use Fakenvapi to substitute Reflex");

        if (state.rtssReflexInjection && state.reflexLimitsFps &&
            InputCommon::current_mode() == LowLatencyMode::AntiLag2 &&
            config->FGOutput.value_or_default() == FGOutput::FSRFG)
            ImGui::TextColored(toneMapColor(ImVec4(1.f, 0.8f, 0.f, 1.f)),
                               "Using RTSS Reflex injection with FSR Anti-Lag 2.0 and FSR FG "
                               "might cause issues");

        if (state.reflexShowWarning)
        {
            ImGui::TextColored(toneMapColor(ImVec4(1.f, 0.f, 0.f, 1.f)),
                               "Using Reflex's limit with FSR FG has performance overhead");

            ImGui::Spacing();
        }

        // set initial value
        if (std::isinf(_limitFps))
            _limitFps = config->FramerateLimit.value_or_default();

        ImGui::SliderFloat("FPS Limit", &_limitFps, 0, 200, "%.0f");

        if (ImGui::Button("Apply Limit"))
        {
            config->FramerateLimit = _limitFps;
        }

        ImGui::SameLine(0.0f, 16.0f);

        if (ImGui::Button("Reset Limit"))
        {
            _limitFps = 0.0f;
            config->FramerateLimit = _limitFps;
        }

        ImGui::Spacing();
        if (auto ch = ScopedCollapsingHeader("VRR Frame Cap Calculator"); ch.IsHeaderOpen())
        {
            ScopedIndent indent {};
            ImGui::Spacing();

            ImGui::PushItemWidth(105.0f * menuResScale);
            ImGui::InputInt("Refresh Rate", &refreshRate, 1, 1, ImGuiInputTextFlags_None);
            ImGui::PopItemWidth();

            float refreshRateF = static_cast<float>(refreshRate);
            // it's fine to use with real reflex, we only care about antilag
            auto fpsLimitTech = InputCommon::current_mode();
            constexpr float margin = 0.3f; // in ms
            float frameCap = std::round(10000.f / (1000.f / refreshRateF + margin)) / 10.f;

            if (fpsLimitTech == LowLatencyMode::AntiLag2 || fpsLimitTech == LowLatencyMode::AntiLagVk)
                frameCap = std::round(frameCap);

            ImGui::Text("Calculated Cap: %.1f", frameCap);

            ImGui::SameLine(0.0f, 16.0f);

            if (ImGui::Button("Set as FPS Limit"))
            {
                _limitFps = frameCap;
                config->FramerateLimit = _limitFps;
            }
        }
    }
}

void MenuCommon::RenderFakenvapiSettings(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;

    // What lowers latency (Force XeLL, Force LatencyFlex) is the Output on the low latency section; what stays here is
    // how fakenvapi does it.
    // Using state.reflexLimitsFps as a detection for Reflex being used on Nvidia
    // With XeFG as the output the section is always up: Force Reflex is what the low latency page's notes send the player to
    const bool showFakenvapi = fakenvapi::isUsingAsMainNvapi() || UsesXeFGApi(state.activeFgOutput);

    if (!showFakenvapi)
        return;

    // FAKENVAPI ---------------------------
    ImGui::SeparatorText("fakenvapi");

    // clang-format off
    static const std::vector<MenuOption<LFXMode>> lfx_modes = {
        { LFXMode::Conservative, "Conservative",
            "The safest, but might not reduce latency well" },
        { LFXMode::Aggressive, "Aggressive",
            "Improves latency, but in some cases will lower FPS more than expected" },
        { LFXMode::ReflexIDs, "Reflex ID",
            "Best when can be used, some games are not compatible (e.g. Cyberpunk)\n"
            "and will fallback to Aggressive" }
    };
    // clang-format on

    // Only while LatencyFlex is what lowers latency
    if (InputCommon::current_mode() == LowLatencyMode::LatencyFlex)
        PopulateCombo("LatencyFlex mode", config->FN_LatencyFlexMode, lfx_modes);

    // clang-format off
    static std::vector<MenuOption<ForceReflex>> reflex_modes = { { ForceReflex::InGame, "Follow in-game" },
                                                            { ForceReflex::ForceDisable, "Force Disable" },
                                                            { ForceReflex::ForceEnable, "Force Enable" } };
    // clang-format on

    PopulateCombo("Force Reflex", config->FN_ForceReflex, reflex_modes);
}

// The F5LUT page. Its code lives with NR's menu (it also grades after NR); it runs on any upscaler's output.
void MenuCommon::RenderF5LutSettings(RenderMenuContext& ctx) { DlssNr::RenderF5LutCard(ctx.config, ctx.menuResScale); }

// HDR output for SDR games (output/SdrToHdr_*). Always shown, with or without an upscaler.
void MenuCommon::RenderHdrOutputSettings(RenderMenuContext& ctx)
{
    auto config = ctx.config;

    ImGui::SeparatorText("HDR output");

    bool enabled = config->HdrOutputEnabled.value_or_default();

    if (ImGui::Checkbox("HDR output", &enabled))
        config->HdrOutputEnabled = enabled;

    ShowHelpMarker("Shows SDR games correctly on an HDR display: the picture is converted to HDR at the end, with the "
                   "brightness and gamma of an SDR monitor. It does not add highlights the game never drew.\n"
                   "Needs Windows HDR turned on for the display. Off by default.\n"
                   "Applies at the next start (or at a resolution / window change, in most games).\n"
                   "Direct3D 11 and 12 games (no Vulkan yet). Works with XeFG and FSR frame generation: every frame, "
                   "generated ones included, goes through the same conversion before it is shown. Not available with "
                   "DLSS-G (OptiScaler's DLSS-G output, or a game that uses DLSS-G itself). Older swap chain types are "
                   "made flip model for it. Stands aside for games that already output HDR and for exclusive full screen "
                   "with an older swap chain type.");

    SdrToHdr::StatusSettings settings;
    settings.enabled = enabled;
    settings.hdr10 = config->UseHDR10.value_or_default();
    settings.gameNits = config->HdrOutputGameNits.value_or_default();
    settings.configuredPeak = config->HdrOutputPeakNits.value_or_default();
    settings.nonDxgiApi = State::Instance().swapchainApi == Vulkan;

    const auto status = SdrToHdr::Status();
    const auto line = SdrToHdr::StatusLine(status, settings);

    if (line.colour != SdrToHdr::StatusColour::None)
    {
        const ImVec4 amber(0.95f, 0.70f, 0.20f, 1.0f);
        const ImVec4 green(0.35f, 0.80f, 0.40f, 1.0f);
        ImGui::TextColored(toneMapColor(line.colour == SdrToHdr::StatusColour::Green ? green : amber), "%s",
                           line.text.c_str());
    }

    // The signal is chosen before the chain is made (it applies at the next create / resize), so it shows as soon as HDR
    // output is ticked; the rest only while the picture is being converted (hidden, not greyed, otherwise)
    const auto signalCombo = [&]()
    {
        const char* signals[] = { "scRGB", "HDR10" };
        int signal = settings.hdr10 ? 1 : 0;

        if (ImGui::Combo("Signal", &signal, signals, IM_ARRAYSIZE(signals)))
            config->UseHDR10 = signal == 1;

        ShowHelpMarker("scRGB: 16-bit float, the most precise. HDR10: 10-bit, for displays or setups that prefer it.\n"
                       "Applies at the next resolution / window change.");
    };

    if (!status.active)
    {
        if (enabled)
            signalCombo();

        return;
    }

    if (!line.signalHint.empty())
        ImGui::TextColored(toneMapColor(ImVec4(0.95f, 0.70f, 0.20f, 1.0f)), "%s", line.signalHint.c_str());

    bool convert = config->HdrOutputConvert.value_or_default();

    if (ImGui::Checkbox("Convert SDR to HDR", &convert))
        config->HdrOutputConvert = convert;

    const std::string compareKey = Keybind::KeyNameFromVirtualKeyCode(config->HdrCompareKey.value_or_default());
    const std::string convertTip = std::string("Off shows the game the way Windows shows an SDR game on an HDR display, "
                                               "for comparing. The menu does not save this: it starts as the ini "
                                               "says (on by default).\n"
                                               "Key to flip it while playing (Input page): ") +
                                   compareKey;
    ShowHelpMarker(convertTip.c_str());

    float gameNits = SdrToHdr::ClampGameNits(config->HdrOutputGameNits.value_or_default());

    if (ImGui::SliderFloat("Game brightness", &gameNits, 80.0f, 1000.0f, "%.0f nits"))
        config->HdrOutputGameNits = SdrToHdr::ClampGameNits(gameNits);

    ImGui::SameLine();
    if (ImGui::Button("Reset##hdrgamenits"))
        config->HdrOutputGameNits = 203.0f;

    ShowHelpMarker("How bright the game's white is. 203 nits is the HDR reference white; raise it in a bright room. "
                   "Default 203 nits.");

    const char* gammas[] = { "2.2", "sRGB" };
    int decode = std::clamp(config->HdrOutputDecode.value_or_default(), 0, 1);

    if (ImGui::Combo("Gamma", &decode, gammas, IM_ARRAYSIZE(gammas)))
        config->HdrOutputDecode = decode;

    ShowHelpMarker("2.2: deeper blacks, how most SDR games looked on an SDR monitor.\n"
                   "sRGB: the exact inverse of the sRGB curve, lifts the darkest shades.");

    signalCombo();
}

void MenuCommon::RenderActiveImageSettings(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    auto& currentFeature = ctx.currentFeature;
    auto& menuResScale = ctx.menuResScale;

    bool rcasEnabled = false;

    if (currentFeature != nullptr && !currentFeature->IsFrozen())
    {
        // SHARPNESS -----------------------------
        ImGui::SeparatorText("Sharpness");

        if (bool overrideSharpness = config->OverrideSharpness.value_or_default();
            ImGui::Checkbox("Override", &overrideSharpness))
        {
            config->OverrideSharpness = overrideSharpness;

            if (currentBackend == Upscaler::DLSS && currentFeature->Version().major < 3)
            {
                state.newBackend = currentBackend;
                MARK_ALL_BACKENDS_CHANGED();
            }
        }
        ShowHelpMarker("Ignores the value sent by the game\n"
                       "and uses the value set below");

        ImGui::SameLine(0.0f, 16.0f * menuResScale);

        float featuresCurrentSharpness = currentFeature->Sharpness();
        if (featuresCurrentSharpness > 0.0f)
            ImGui::TextDisabled("(Current sharpness: %.3f)", featuresCurrentSharpness);
        else
            ImGui::TextDisabled("(Current sharpness: disabled)");

        ImGui::BeginDisabled(!config->OverrideSharpness.value_or_default());

        float sharpness = config->Sharpness.value_or_default();

        if (ImGui::SliderFloat("Sharpness", &sharpness, 0.0f, 1.0f))
            config->Sharpness = sharpness;

        ImGui::EndDisabled();

        // RCAS
        // if (state.api == DX12 || state.api == DX11)
        {
            // xess or dlss version >= 2.5.1
            constexpr feature_version requiredDlssVersion = { 2, 5, 1 };
            rcasEnabled = (currentBackend == Upscaler::XeSS ||
                           (currentBackend == Upscaler::DLSS && currentFeature->Version() >= requiredDlssVersion));

            ImGui::Spacing();
            ImGui::Spacing();

            if (bool rcas = config->RcasEnabled.value_or(rcasEnabled); ImGui::Checkbox("Enable RCAS/DA", &rcas))
                config->RcasEnabled = rcas;

            ShowHelpMarker("Enable OptiScaler's sharpening filter\n"
                           "By default uses a sharpening value provided by the game\n"
                           "Select 'Override' under 'Sharpness' and adjust the slider\n"
                           "to change it\n\n"
                           "Some upscalers have their own sharpness filter, so this\n"
                           "option is not always needed");

            ImGui::BeginDisabled(!config->RcasEnabled.value_or(rcasEnabled));

            auto sharpnessShader = (int32_t) Config::Instance()->SharpnessShader.value_or_default();

            if (ImGui::RadioButton("RCAS", &sharpnessShader, (int32_t) SharpenShader::RCAS))
            {
                Config::Instance()->SharpnessShader = SharpenShader::RCAS;
            }

            ShowHelpMarker("Use AMD's RCAS\n"
                           "Modified to add Contrast parameter\n"
                           "and MAS support");

            ImGui::SameLine(0.0f, 6.0f);

            if (ImGui::RadioButton("Depth Aware (RCAS)", &sharpnessShader, (int32_t) SharpenShader::DepthAware))
            {
                Config::Instance()->SharpnessShader = SharpenShader::DepthAware;
            }

            ShowHelpMarker("Use Depth Aware Sharpening (RCAS)\n"
                           "Smarter sharpening with less artifacts,\n"
                           "but also heavier\n\n"
                           "The farther away is the object, the more\n"
                           "sharpening is applied");

            ImGui::SameLine(0.0f, 6.0f);

            if (ImGui::RadioButton("Depth Aware (DAS)", &sharpnessShader,
                                   (int32_t) SharpenShader::LocalContrastDepthAware))
            {
                Config::Instance()->SharpnessShader = SharpenShader::LocalContrastDepthAware;
            }

            ShowHelpMarker("Use Depth Aware Sharpening (DAS)\n"
                           "Depth-aware directional adaptive luma sharpener\n"
                           "Smarter sharpening with less artifacts,\n"
                           "but also heavier\n\n"
                           "The farther away is the object, the more\n"
                           "sharpening is applied");

            ImGui::Spacing();

            if (bool overrideMotionSharpness = config->MotionSharpnessEnabled.value_or_default();
                ImGui::Checkbox("Enable Motion Adaptive Sharpness", &overrideMotionSharpness))
                config->MotionSharpnessEnabled = overrideMotionSharpness;
            ShowHelpMarker("Enables sharpness adjustments according to the motion");

            if (Config::Instance()->SharpnessShader.value_or_default() != SharpenShader::RCAS)
            {
                if (bool overrideMSDebug = config->MotionSharpnessDebug.value_or_default();
                    ImGui::Checkbox("DA + MAS Debug", &overrideMSDebug))
                    config->MotionSharpnessDebug = overrideMSDebug;

                ShowHelpMarker("Enable DA + MAS debug views\n"
                               "Blue tint for DA detected edges\n\n"
                               "More red areas will have more sharpness applied\n"
                               "Green areas will get reduced sharpness");

                if (auto ch = ScopedCollapsingHeader("Advanced DA Parameters"); ch.IsHeaderOpen())
                {
                    ScopedIndent indent {};
                    ImGui::Spacing();

                    if (bool clamp = config->DAClampOutput.value_or(false); ImGui::Checkbox("Clamp Output", &clamp))
                    {
                        if (clamp)
                            config->DAClampOutput = true;
                        else
                            config->DAClampOutput.reset();
                    }

                    ShowHelpMarker("Clamps the final image to the [0, 1] range.\n\n"
                                   "Prevents overshoot artifacts such as bright halos or negative colors.\n"
                                   "Recommended for LDR pipelines; optional for HDR depending on tone-mapping.\n\n"
                                   "When not set OptiScaler controls it via upscalers HDR flag");

                    if (currentFeature->DepthLinear())
                    {
                        float depthBias = config->DADepthBias.value_or(0.0015f);
                        if (ImGui::SliderFloat("Depth Bias", &depthBias, 0.005f, 0.03f, "%.4f"))
                            config->DADepthBias = depthBias;

                        ShowHelpMarker("Ignores small depth differences before edge detection.\n\n"
                                       "Higher values reduce flickering and noise from minor depth changes, but may "
                                       "soften real geometry edges.\n"
                                       "Lower values preserve fine detail but can cause unstable or noisy edge "
                                       "detection.");

                        float depthScale = config->DADepthScale.value_or(250.0f);
                        if (ImGui::SliderFloat("Depth Scale", &depthScale, 100.0f, 600.0f, "%.1f"))
                            config->DADepthScale = depthScale;

                        ShowHelpMarker("Controls how strongly sharpening is reduced across depth edges.\n\n"
                                       "Higher values more aggressively prevent sharpening across object boundaries "
                                       "(reduces halos).\n"
                                       "Lower values allow more sharpening to pass across edges (sharper but "
                                       "riskier).");
                    }
                    else
                    {
                        float depthBias = config->DADepthBias.value_or(0.001f);
                        if (ImGui::SliderFloat("Depth Bias", &depthBias, 0.0001f, 0.003f, "%.4f"))
                            config->DADepthBias = depthBias;

                        ShowHelpMarker("Ignores small depth differences before edge detection.\n\n"
                                       "Higher values reduce flickering and noise from minor depth changes, but may "
                                       "soften real geometry edges.\n"
                                       "Lower values preserve fine detail but can cause unstable or noisy edge "
                                       "detection.");

                        float depthScale = config->DADepthScale.value_or(35.0f);
                        if (ImGui::SliderFloat("Depth Scale", &depthScale, 25.0f, 400.0f, "%.1f"))
                            config->DADepthScale = depthScale;

                        ShowHelpMarker("Controls how strongly sharpening is reduced across depth edges.\n\n"
                                       "Higher values more aggressively prevent sharpening across object boundaries "
                                       "(reduces halos).\n"
                                       "Lower values allow more sharpening to pass across edges (sharper but "
                                       "riskier).");
                    }

                    if (ImGui::Button("Reset Depth Values"))
                    {
                        config->DADepthBias.reset();
                        config->DADepthScale.reset();
                    }
                }
            }
            else
            {
                if (bool contrastEnabled = config->ContrastEnabled.value_or_default();
                    ImGui::Checkbox("Contrast Enabled", &contrastEnabled))
                    config->ContrastEnabled = contrastEnabled;

                ShowHelpMarker("Controls sharpness at high contrast areas.");

                ImGui::BeginDisabled(!config->ContrastEnabled.value_or_default());

                float contrast = config->Contrast.value_or_default();
                if (ImGui::SliderFloat("Contrast", &contrast, -2.0f, 2.0f, "%.2f"))
                    config->Contrast = contrast;

                ShowHelpMarker("Positive values decrease sharpness at high contrast areas.\n"
                               "Negative values increase sharpness at high contrast areas.");

                ImGui::EndDisabled();
            }

            ImGui::Spacing();
            if (auto ch = ScopedCollapsingHeader("Motion Adaptive Sharpness##2"); ch.IsHeaderOpen())
            {
                ScopedIndent indent {};
                ImGui::Spacing();

                ImGui::BeginDisabled(!config->MotionSharpnessEnabled.value_or_default());

                if (Config::Instance()->SharpnessShader.value_or_default() == SharpenShader::RCAS)
                {
                    if (bool overrideMSDebug = config->MotionSharpnessDebug.value_or_default();
                        ImGui::Checkbox("MAS Debug", &overrideMSDebug))
                        config->MotionSharpnessDebug = overrideMSDebug;
                    ShowHelpMarker("Areas that are more red will have more sharpness applied\n"
                                   "Green areas will get reduced sharpness");
                }

                float motionSharpness = config->MotionSharpness.value_or_default();
                ImGui::SliderFloat("MotionSharpness", &motionSharpness, -1.0f, 1.0f, "%.3f");
                config->MotionSharpness = motionSharpness;

                ShowHelpMarker("Maximum amount of sharpness that motion can add or remove.\n\n"
                               "Negative values reduce sharpening in motion (recommended).\n"
                               "Positive values increase sharpening in motion.\n\n"
                               "The final adjustment scales with motion and is capped at this value.");

                float motionThreshod = config->MotionThreshold.value_or_default();
                ImGui::SliderFloat("MotionThreshod", &motionThreshod, 0.0f, 100.0f, "%.2f");
                config->MotionThreshold = motionThreshod;

                ShowHelpMarker("Minimum motion required before motion-based sharpening adjustment begins.\n\n"
                               "Higher values ignore small movements (more stable).\n"
                               "Lower values react to subtle motion (more sensitive).");

                float motionScale = config->MotionScaleLimit.value_or_default();
                ImGui::SliderFloat("MotionRange", &motionScale, 0.01f, 100.0f, "%.2f");
                config->MotionScaleLimit = motionScale;

                ShowHelpMarker("Defines the motion range over which the effect ramps from zero to full strength.\n\n"
                               "Values above the threshold are mapped into this range.\n"
                               "Larger values make the response smoother and more gradual.\n"
                               "Smaller values make the effect react more quickly and aggressively.");

                ImGui::EndDisabled();

                ImGui::Spacing();
                ImGui::Spacing();
            }

            ImGui::EndDisabled();
        }

        // UPSCALE RATIO OVERRIDE -----------------

        auto minSliderLimit = config->ExtendedLimits.value_or_default() ? 0.1f : 1.0f;
        auto maxSliderLimit = config->ExtendedLimits.value_or_default() ? 6.0f : 3.0f;

        ImGui::SeparatorText("Upscale Ratio Override");

        if (bool upOverride = config->UpscaleRatioOverrideEnabled.value_or_default();
            ImGui::Checkbox("Override all", &upOverride))
        {
            config->UpscaleRatioOverrideEnabled = upOverride;

            if (upOverride)
                config->QualityRatioOverrideEnabled = false;
        }
        ShowHelpMarker("Overrides every upscaler preset with the set value\n\n"
                       "1.5x on a 1080p screen means an internal res of 720p\n"
                       "1080 / 1.5 = 720");

        if (bool qOverride = config->QualityRatioOverrideEnabled.value_or_default();
            ImGui::Checkbox("Override per quality preset", &qOverride))
        {
            config->QualityRatioOverrideEnabled = qOverride;

            if (qOverride)
                config->UpscaleRatioOverrideEnabled = false;
        }

        ShowHelpMarker("Lets you override each preset's ratio individually\n"
                       "Note that not every game supports every quality preset\n\n"
                       "1.5x on a 1080p screen means internal resolution of 720p\n"
                       "1080 / 1.5 = 720");

        if (config->UpscaleRatioOverrideEnabled.value_or_default())
        {
            float urOverride = config->UpscaleRatioOverrideValue.value_or_default();
            ImGui::SliderFloat("All Ratios", &urOverride, minSliderLimit, maxSliderLimit, "%.3f");
            config->UpscaleRatioOverrideValue = urOverride;
        }

        if (config->QualityRatioOverrideEnabled.value_or_default())
        {
            float qDlaa = config->QualityRatio_DLAA.value_or_default();
            if (ImGui::SliderFloat("DLAA", &qDlaa, minSliderLimit, maxSliderLimit, "%.3f"))
                config->QualityRatio_DLAA = qDlaa;

            float qUq = config->QualityRatio_UltraQuality.value_or_default();
            if (ImGui::SliderFloat("Ultra Quality", &qUq, minSliderLimit, maxSliderLimit, "%.3f"))
                config->QualityRatio_UltraQuality = qUq;

            float qQ = config->QualityRatio_Quality.value_or_default();
            if (ImGui::SliderFloat("Quality", &qQ, minSliderLimit, maxSliderLimit, "%.3f"))
                config->QualityRatio_Quality = qQ;

            float qB = config->QualityRatio_Balanced.value_or_default();
            if (ImGui::SliderFloat("Balanced", &qB, minSliderLimit, maxSliderLimit, "%.3f"))
                config->QualityRatio_Balanced = qB;

            float qP = config->QualityRatio_Performance.value_or_default();
            if (ImGui::SliderFloat("Performance", &qP, minSliderLimit, maxSliderLimit, "%.3f"))
                config->QualityRatio_Performance = qP;

            float qUp = config->QualityRatio_UltraPerformance.value_or_default();
            if (ImGui::SliderFloat("Ultra Performance", &qUp, minSliderLimit, maxSliderLimit, "%.3f"))
                config->QualityRatio_UltraPerformance = qUp;
        }

        if (currentFeature != nullptr && !currentFeature->IsFrozen())
        {
            // OUTPUT SCALING -----------------------------
            // if (state.api == DX12 || state.api == DX11)
            {
                // if motion vectors are not display size
                ImGui::BeginDisabled(!currentFeature->LowResMV() &&
                                     currentFeature->RenderWidth() != currentFeature->DisplayWidth());

                ImGui::SeparatorText("Output Scaling");

                float defaultRatio = 1.5f;

                if (_ssRatio == 0.0f)
                {
                    _ssRatio = config->OutputScalingMultiplier.value_or(defaultRatio);
                    _ssEnabled = config->OutputScalingEnabled.value_or_default();
                    _ssDownsampler = config->OutputScalingDownscaler.value_or_default();
                }

                ImGui::BeginDisabled((currentBackend == Upscaler::XeSS || currentBackend == Upscaler::DLSS) &&
                                     currentFeature->RenderWidth() > currentFeature->DisplayWidth());
                ImGui::Checkbox("Enable", &_ssEnabled);
                ImGui::EndDisabled();

                ShowHelpMarker("Upscales the image internally to a higher output resolution\n"
                               "then downscales it back to your display resolution\n\n"
                               "Values <1.0 make the upscaler cheaper\n"
                               "Values >1.0 make image sharper at the cost of performance\n\n"
                               "If greyed out, please check Git Wiki - Unreal Engine tweaks\n\n"
                               "Target res and total ratio at the bottom (max. total 3.0!)");

                ImGui::SameLine(0.0f, 6.0f);

                ImGui::BeginDisabled(!_ssEnabled);
                {
                    ImGui::PushItemWidth(95.0f * menuResScale);

                    // clang-format off
                    std::vector<MenuOption<Scaler>> ds_options = {
                        { Scaler::FSR1, "FSR1",
                            "Default option.\nGood enough image quality and very fast." },
                        { Scaler::Bicubic, "Bicubic",
                            "Fastest traditional option.\nProduces a very soft/blurry image, but might be okay for downscaling." },
                        { Scaler::CatmullRom, "Catmull-Rom",
                            "Designed primarily for downscaling.\nRetains good contrast with minimal artefacts, but softer than Lanczos." },
                        { Scaler::Lanczos2, "Lanczos2",
                            "Lighter and faster than Lanczos3.\nLess prone to ringing artefacts, but slightly blurrier." },
                        { Scaler::Lanczos3, "Lanczos3",
                            "Heavier version of Lanczos2.\nOffers the sharpest image, but is the most prone to ringing.\nConsidered the best along with Kaiser3." },
                        { Scaler::Kaiser2, "Kaiser2",
                            "Similar to Lanczos2.\nSmoother and less prone to artefacts than Lanczos, but slightly blurrier." },
                        { Scaler::Kaiser3, "Kaiser3",
                            "Similar to Lanczos3.\nFar less prone to artefacting than Lanczos3, but much heavier on the GPU.\nConsidered the best along with Lanczos3." },
                        { Scaler::Magic, "MAGIC",
                            "Specialised to prevent artifacts.\nEliminates harsh halos for a natural look, but can appear slightly soft." }
                    };
                    // clang-format on

                    const bool isUpsampleRatio = _ssRatio < 1.0f;
                    const std::string disabledReason = "Only FSR1 and Bicubic are supported when Ratio is below 1.0.";

                    for (auto& opt : ds_options)
                    {
                        if (isUpsampleRatio && opt.value > Scaler::Bicubic)
                            opt.set_disabled(true, opt.tooltip + "\n\n" + disabledReason);
                    }

                    if (isUpsampleRatio && _ssDownsampler > Scaler::Bicubic)
                        _ssDownsampler = Scaler::FSR1;

                    PopulateCombo("Downscaler", _ssDownsampler, ds_options);

                    ImGui::PopItemWidth();
                }
                ImGui::EndDisabled();

                bool applyEnabled = _ssEnabled != config->OutputScalingEnabled.value_or_default() ||
                                    _ssRatio != config->OutputScalingMultiplier.value_or(defaultRatio) ||
                                    _ssDownsampler != config->OutputScalingDownscaler.value_or_default();

                ImGui::BeginDisabled(!applyEnabled);
                if (ImGui::Button("Apply Change"))
                {
                    config->OutputScalingEnabled = _ssEnabled;
                    config->OutputScalingMultiplier = _ssRatio;

                    if (_ssRatio < 1.0f && _ssDownsampler > Scaler::Bicubic)
                        _ssDownsampler = Scaler::FSR1;

                    config->OutputScalingDownscaler = _ssDownsampler;

                    const bool usesDlssd = currentFeature->GetUpscalerType() == Upscaler::DLSSD;
                    if (usesDlssd)
                        state.newBackend = Upscaler::DLSSD;
                    else
                        state.newBackend = currentBackend;

                    MARK_ALL_BACKENDS_CHANGED();
                }
                ImGui::EndDisabled();

                ImGui::BeginDisabled(!_ssEnabled || currentFeature->RenderWidth() > currentFeature->DisplayWidth());
                ImGui::SliderFloat("Ratio", &_ssRatio, 0.5f, 3.0f, "%.2f");
                ImGui::EndDisabled();

                if (currentFeature != nullptr && !currentFeature->IsFrozen())
                {
                    ImGui::Text("Output Scaling is %s, Target Res: %dx%d (%.2f)\nJitter Count: %d",
                                config->OutputScalingEnabled.value_or_default() ? "ENABLED" : "DISABLED",
                                (uint32_t) (currentFeature->DisplayWidth() * _ssRatio),
                                (uint32_t) (currentFeature->DisplayHeight() * _ssRatio),
                                ((float) currentFeature->DisplayWidth() * _ssRatio) /
                                    (float) currentFeature->RenderWidth(),
                                currentFeature->JitterCount());
                }

                ImGui::EndDisabled();
            }
        }

        // INIT -----------------------------
        ImGui::SeparatorText("Init Flags");
        if (ImGui::BeginTable("init", 2, ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableNextColumn();

            // AutoExposure is always enabled for XeSS with native Dx11
            bool autoExposureDisabled = state.api == API::DX11 && currentBackend == Upscaler::XeSS;
            ImGui::BeginDisabled(autoExposureDisabled);

            if (bool autoExposure = currentFeature->AutoExposure(); ImGui::Checkbox("Auto Exposure", &autoExposure))
            {
                config->AutoExposure = autoExposure;
                ReInitUpscaler();
            }
            ShowResetButton(&config->AutoExposure, "R");
            ShowHelpMarker("Some Unreal Engine games need this\n\n"
                           "Try using if colours flickering or\n"
                           "objects have ghosting trails");

            ImGui::EndDisabled();

            ImGui::TableNextColumn();
            auto accessToReactiveMask = currentFeature->AccessToReactiveMask();
            ImGui::BeginDisabled(!accessToReactiveMask);

            bool canUseReactiveMask =
                accessToReactiveMask && currentBackend != Upscaler::DLSS &&
                (currentBackend != Upscaler::XeSS || currentFeature->Version() >= feature_version { 2, 0, 1 });

            bool disableReactiveMask = config->DisableReactiveMask.value_or(!canUseReactiveMask);

            if (ImGui::Checkbox("Disable Reactive Mask", &disableReactiveMask))
            {
                config->DisableReactiveMask = disableReactiveMask;

                if (currentBackend == Upscaler::XeSS)
                {
                    state.newBackend = currentBackend;
                    MARK_ALL_BACKENDS_CHANGED();
                }
            }

            ImGui::EndDisabled();

            if (accessToReactiveMask)
                ShowHelpMarker("Allows the use of a Reactive mask\n"
                               "Keep in mind that a Reactive mask sent to DLSS\n"
                               "will not produce a good image in combination with FSR/XeSS");
            else
                ShowHelpMarker("Option disabled because the game doesn't provide a Reactive mask");

            ImGui::EndTable();

            ImGui::Spacing();
            if (auto ch = ScopedCollapsingHeader("Advanced Init Flags"); ch.IsHeaderOpen())
            {
                ScopedIndent indent {};
                ImGui::Spacing();

                if (ImGui::BeginTable("init2", 2, ImGuiTableFlags_SizingStretchProp))
                {
                    ImGui::TableNextColumn();
                    if (bool depth = currentFeature->DepthInverted(); ImGui::Checkbox("Depth Inverted", &depth))
                    {
                        config->DepthInverted = depth;
                        ReInitUpscaler();
                    }
                    ShowResetButton(&config->DepthInverted, "R##2");
                    ShowHelpMarker("You shouldn't need to change it");

                    ImGui::TableNextColumn();
                    if (bool hdr = currentFeature->IsHdr(); ImGui::Checkbox("HDR", &hdr))
                    {
                        config->HDR = hdr;
                        ReInitUpscaler();
                    }
                    ShowResetButton(&config->HDR, "R##1");
                    ShowHelpMarker("Might help with purple hue in some games");

                    ImGui::TableNextColumn();
                    if (bool mv = !currentFeature->LowResMV(); ImGui::Checkbox("Display Res. MV", &mv))
                    {
                        config->DisplayResolution = mv;

                        // Disable output scaling when
                        // Display res MV is active
                        if (mv)
                        {
                            config->OutputScalingEnabled = false;
                            _ssEnabled = false;
                        }

                        ReInitUpscaler();
                    }
                    ShowResetButton(&config->DisplayResolution, "R##4");
                    ShowHelpMarker("Mostly a fix for Unreal Engine games\n"
                                   "Top left part of the screen will be blurry");

                    ImGui::TableNextColumn();

                    if (bool jitter = currentFeature->JitteredMV(); ImGui::Checkbox("Jitter Cancellation", &jitter))
                    {
                        config->JitterCancellation = jitter;
                        ReInitUpscaler();
                    }
                    ShowResetButton(&config->JitterCancellation, "R##3");
                    ShowHelpMarker("Fix for games that send motion data with preapplied jitter");

                    ImGui::TableNextColumn();
                    ImGui::BeginDisabled(currentBackend != Upscaler::FFX && currentBackend != Upscaler::FFX_on12);

                    if (bool drs = currentFeature->DynamicResolution(); ImGui::Checkbox("Dynamic Res. (DRS)", &drs))
                    {
                        config->DynamicResolution = drs;
                        ReInitUpscaler();
                    }
                    ShowResetButton(&config->DynamicResolution, "R##5");
                    ShowHelpMarker("Enable if the game uses dynamic resolution\n"
                                   "Prevents FSR 4 from switching models when the render size changes");

                    ImGui::EndDisabled();

                    ImGui::TableNextColumn();
                    ImGui::EndTable();
                }

                if (currentFeature->AccessToReactiveMask() && currentBackend != Upscaler::DLSS)
                {
                    ImGui::BeginDisabled(config->DisableReactiveMask.value_or(currentBackend == Upscaler::XeSS));

                    bool binaryMask = state.api == Vulkan || currentBackend == Upscaler::XeSS;
                    auto defaultBias = binaryMask ? 0.0f : 0.45f;
                    auto maskBias = config->DlssReactiveMaskBias.value_or(defaultBias);

                    if (!binaryMask)
                    {
                        if (ImGui::SliderFloat("React. Mask Bias", &maskBias, 0.0f, 0.9f, "%.2f"))
                            config->DlssReactiveMaskBias = maskBias;

                        ShowHelpMarker("Values above 0 activate usage of Reactive mask");
                    }
                    else
                    {
                        bool useRM = maskBias > 0.0f;
                        if (ImGui::Checkbox("Use Binary Reactive Mask", &useRM))
                        {
                            if (useRM)
                                config->DlssReactiveMaskBias = 0.45f;
                            else
                                config->DlssReactiveMaskBias.reset();
                        }
                    }

                    ImGui::EndDisabled();
                }
            }
        }
    }
}

void MenuCommon::RenderMagnifierSettings(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;

    // Magnifier -----------------------------
    ImGui::Spacing();
    if (auto ch = ScopedCollapsingHeader("Magnifier"); ch.IsHeaderOpen())
    {
        ScopedIndent indent {};
        ImGui::Spacing();

        bool magnifierEnabled = config->MagnifierEnabled.value_or_default();
        if (ImGui::Checkbox("Enable Magnifier", &magnifierEnabled))
            config->MagnifierEnabled = magnifierEnabled;

        ImGui::BeginDisabled(!magnifierEnabled);

        float magnifierSize = config->MagnifierSize.value_or_default();
        if (ImGui::SliderFloat("Size", &magnifierSize, 5.0f, 50.0f, "%.1f%% of screen"))
            config->MagnifierSize = magnifierSize;

        int zoomFactor = config->MagnifierZoomFactor.value_or_default();
        if (ImGui::SliderInt("Zoom Factor", &zoomFactor, 2, 20, "%dx"))
            config->MagnifierZoomFactor = zoomFactor;

        float borderSize = config->MagnifierBorderSize.value_or_default();
        if (ImGui::SliderFloat("Border Size", &borderSize, 0.0f, 2.0f, "%.2f%% of screen"))
            config->MagnifierBorderSize = borderSize;

        ImGui::Separator();
        ImGui::Text("Positioning");

        bool staticMode = config->MagnifierStaticPosX.has_value() && config->MagnifierStaticPosY.has_value();
        if (staticMode)
        {
            float staticX = config->MagnifierStaticPosX.value();
            if (ImGui::SliderFloat("Static Pos X", &staticX, 0.0f, 100.0f, "%.1f%%"))
                config->MagnifierStaticPosX = staticX;

            float staticY = config->MagnifierStaticPosY.value();
            if (ImGui::SliderFloat("Static Pos Y", &staticY, 0.0f, 100.0f, "%.1f%%"))
                config->MagnifierStaticPosY = staticY;

            if (ImGui::Button("Reset Static Position (Follow Cursor)"))
            {
                config->MagnifierStaticPosX.reset();
                config->MagnifierStaticPosY.reset();
            }
        }
        else
        {
            // Button to initialize static position mode
            if (ImGui::Button("Set Static Position"))
            {
                config->MagnifierStaticPosX = 50.0f;
                config->MagnifierStaticPosY = 50.0f;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(Currently following cursor)");

            float offsetX = config->MagnifierCursorOffsetX.value_or_default();
            if (ImGui::SliderFloat("Cursor Offset X", &offsetX, -300.0f, 300.0f, "%.0f px"))
                config->MagnifierCursorOffsetX = offsetX;

            float offsetY = config->MagnifierCursorOffsetY.value_or_default();
            if (ImGui::SliderFloat("Cursor Offset Y", &offsetY, -300.0f, 300.0f, "%.0f px"))
                config->MagnifierCursorOffsetY = offsetY;
        }

        ImGui::EndDisabled();
        ImGui::Spacing();
    }
}
void MenuCommon::RenderQuirksSettings(RenderMenuContext& ctx)
{
    auto& state = ctx.state;

    // QUIRKS -----------------------------
    if (state.detectedQuirks.size() > 0)
    {
        ImGui::Spacing();
        if (auto ch = ScopedCollapsingHeader("Active Quirks"); ch.IsHeaderOpen())
        {
            ScopedIndent indent {};
            ImGui::Spacing();

            for (const auto& quirk : state.detectedQuirks)
            {
                ImGui::TextWrapped("%s", quirk.c_str());
            }
        }
    }
}

void MenuCommon::RenderAdvancedSettings(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    auto& currentFeature = ctx.currentFeature;

    // ADVANCED SETTINGS -----------------------------
    ImGui::Spacing();
    if (auto ch = ScopedCollapsingHeader("Advanced Settings"); ch.IsHeaderOpen())
    {
        ScopedIndent indent {};
        ImGui::Spacing();

        if (currentFeature != nullptr && !currentFeature->IsFrozen())
        {
            bool extendedLimits = config->ExtendedLimits.value_or_default();
            if (ImGui::Checkbox("Enable Extended Limits", &extendedLimits))
                config->ExtendedLimits = extendedLimits;

            ShowHelpMarker("Extended sliders limit for quality presets\n\n"
                           "Using this option changes resolution detection logic\n"
                           "and might cause issues and crashes!");
        }

        bool pcShaders = config->UsePrecompiledShaders.value_or_default();
        if (ImGui::Checkbox("Use Precompiled Shaders", &pcShaders))
        {
            config->UsePrecompiledShaders = pcShaders;
            state.newBackend = currentBackend;
            MARK_ALL_BACKENDS_CHANGED();
        }

        // DRS
        ImGui::SeparatorText("DRS (Dynamic Resolution Scaling)");
        if (ImGui::BeginTable("drs", 2, ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableNextColumn();
            if (bool drsMin = config->DrsMinOverrideEnabled.value_or_default();
                ImGui::Checkbox("Override Minimum", &drsMin))
                config->DrsMinOverrideEnabled = drsMin;
            ShowHelpMarker("Fix for games ignoring official DRS limits");

            ImGui::TableNextColumn();
            if (bool drsMax = config->DrsMaxOverrideEnabled.value_or_default();
                ImGui::Checkbox("Override Maximum", &drsMax))
                config->DrsMaxOverrideEnabled = drsMax;
            ShowHelpMarker("Fix for games ignoring official DRS limits");

            ImGui::EndTable();
        }

        // Non-DLSS hotfixes -----------------------------
        if (currentFeature != nullptr && !currentFeature->IsFrozen() && currentBackend != Upscaler::DLSS)
        {
            // BARRIERS -----------------------------
            ImGui::Spacing();
            if (auto ch = ScopedCollapsingHeader("Resource Barriers"); ch.IsHeaderOpen())
            {
                ScopedIndent indent {};
                ImGui::Spacing();

                AddResourceBarrier("Color", &config->ColorResourceBarrier);
                AddResourceBarrier("Depth", &config->DepthResourceBarrier);
                AddResourceBarrier("Motion", &config->MVResourceBarrier);
                AddResourceBarrier("Exposure", &config->ExposureResourceBarrier);
                AddResourceBarrier("Mask", &config->MaskResourceBarrier);
                AddResourceBarrier("Output", &config->OutputResourceBarrier);
            }

            // HOTFIXES -----------------------------
            if (state.api == DX12)
            {
                ImGui::Spacing();
                if (auto ch = ScopedCollapsingHeader("Root Signatures"); ch.IsHeaderOpen())
                {
                    ScopedIndent indent {};
                    ImGui::Spacing();

                    if (bool crs = config->RestoreComputeSignature.value_or_default();
                        ImGui::Checkbox("Restore Compute Root Signature", &crs))
                        config->RestoreComputeSignature = crs;

                    if (bool grs = config->RestoreGraphicSignature.value_or_default();
                        ImGui::Checkbox("Restore Graphic Root Signature", &grs))
                        config->RestoreGraphicSignature = grs;
                }
            }
        }
    }
}

void MenuCommon::RenderLoggingSettings(RenderMenuContext& ctx)
{
    auto config = ctx.config;

    // LOGGING -----------------------------
    ImGui::Spacing();
    if (auto ch = ScopedCollapsingHeader("Logging"); ch.IsHeaderOpen())
    {
        ScopedIndent indent {};
        ImGui::Spacing();

        if (config->LogToConsole.value_or_default() || config->LogToFile.value_or_default() ||
            config->LogToNGX.value_or_default())
            spdlog::default_logger()->set_level((spdlog::level::level_enum) config->LogLevel.value_or_default());
        else
            spdlog::default_logger()->set_level(spdlog::level::off);

        if (bool toFile = config->LogToFile.value_or_default(); ImGui::Checkbox("To File", &toFile))
        {
            config->LogToFile = toFile;
            PrepareLogger();
        }

        ImGui::SameLine(0.0f, 6.0f);
        if (bool toConsole = config->LogToConsole.value_or_default(); ImGui::Checkbox("To Console", &toConsole))
        {
            config->LogToConsole = toConsole;
            PrepareLogger();
        }

        const char* logLevels[] = { "Trace", "Debug", "Information", "Warning", "Error" };
        const char* selectedLevel = logLevels[config->LogLevel.value_or_default()];

        if (ImGui::BeginCombo("Log Level", selectedLevel))
        {
            for (int n = 0; n < 5; n++)
            {
                if (ImGui::Selectable(logLevels[n], (config->LogLevel.value_or_default() == n)))
                {
                    config->LogLevel = n;
                    spdlog::default_logger()->set_level(
                        (spdlog::level::level_enum) config->LogLevel.value_or_default());
                }
            }

            ImGui::EndCombo();
        }
    }
}

void MenuCommon::RenderThemeSettings(RenderMenuContext& ctx)
{
    auto config = ctx.config;

    // THEME -----------------------------
    ImGui::Spacing();
    if (auto ch = ScopedCollapsingHeader("Menu Theme and Color"); ch.IsHeaderOpen())
    {
        ScopedIndent indent {};
        ImGui::Spacing();

        bool lightTheme = config->LightTheme.value_or_default();

        const ImVec4 bgDark = lightTheme ? ImVec4(0.80f, 0.82f, 0.86f, 1.00f) : ImVec4(0.09f, 0.09f, 0.10f, 1.00f);
        const ImVec4 bgMid = lightTheme ? ImVec4(0.89f, 0.91f, 0.95f, 1.00f) : ImVec4(0.11f, 0.11f, 0.12f, 1.00f);
        const ImVec4 bgLight = lightTheme ? ImVec4(0.96f, 0.97f, 0.99f, 1.00f) : ImVec4(0.14f, 0.14f, 0.15f, 1.00f);

        auto Mix = [](const ImVec4& a, const ImVec4& b, float t, float alpha = 1.0f)
        { return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, alpha); };

        auto AccentSoft = [&](ImVec4 accent, float alpha = 1.0f)
        { return toneMapColor(lightTheme ? Mix(bgLight, accent, 0.24f, alpha) : Mix(bgDark, accent, 0.32f, alpha)); };

        auto AccentMed = [&](ImVec4 accent, float alpha = 1.0f)
        { return toneMapColor(lightTheme ? Mix(bgLight, accent, 0.42f, alpha) : Mix(bgDark, accent, 0.55f, alpha)); };

        auto AccentStrong = [&](ImVec4 accent, float alpha = 1.0f)
        { return toneMapColor(ImVec4(accent.x, accent.y, accent.z, alpha)); };

        if (ImGui::Checkbox("Light Theme", &lightTheme))
        {
            config->LightTheme = lightTheme;
            RebuildStyle(ctx.menuResScale);
        }

        bool modernTheme = config->ModernTheme.value_or_default();
        if (ImGui::Checkbox("Modern style", &modernTheme))
        {
            config->ModernTheme = modernTheme;
            RebuildStyle(ctx.menuResScale);
            ApplyThemeFont();
        }

        ImGui::SameLine(0.0f, 6.0f);
        ShowHelpMarker("Rounder layout with section cards and the Inter font. The performance overlay keeps its "
                       "fixed-width font.");

        ImGui::SeparatorText("Accent Colour");

        ImGui::Text("Presets:");
        ImGui::SameLine(0.0f, 6.0f);

        ImVec4 colorBlue = { 0.00f, 0.40f, 0.77f, 1.0f };
        ImVec4 colorTeal = { 0.00f, 1.00f, 0.91f, 1.0f };
        ImVec4 colorGray = { 0.54f, 0.54f, 0.54f, 1.0f };
        ImVec4 colorYellow = { 1.00f, 0.89f, 0.00f, 1.0f };
        ImVec4 colorGreen = { 0.25f, 1.00f, 0.00f, 1.0f };
        ImVec4 colorRed = { 1.00f, 0.00f, 0.00f, 1.0f };
        ImVec4 colorOrange = { 1.00f, 0.52f, 0.00f, 1.0f };
        ImVec4 colorPurple = { 0.576f, 0.00f, 1.00f, 1.0f };

        ImVec4 color = {};

        color = colorBlue;
        ImGui::PushStyleColor(ImGuiCol_Button, AccentSoft(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentMed(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentStrong(color));

        if (ImGui::Button("Blue"))
        {
            ImGui::PopStyleColor(3);

            config->MenuAccentColorR = color.x;
            config->MenuAccentColorG = color.y;
            config->MenuAccentColorB = color.z;
            RebuildStyle(ctx.menuResScale);
        }
        else
        {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine(0.0f, 6.0f);

        color = colorTeal;
        ImGui::PushStyleColor(ImGuiCol_Button, AccentSoft(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentMed(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentStrong(color));

        if (ImGui::Button("Teal"))
        {
            ImGui::PopStyleColor(3);

            config->MenuAccentColorR = color.x;
            config->MenuAccentColorG = color.y;
            config->MenuAccentColorB = color.z;
            RebuildStyle(ctx.menuResScale);
        }
        else
        {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine(0.0f, 6.0f);

        color = colorGray;
        ImGui::PushStyleColor(ImGuiCol_Button, AccentSoft(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentMed(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentStrong(color));

        if (ImGui::Button("Gray"))
        {
            ImGui::PopStyleColor(3);

            config->MenuAccentColorR = color.x;
            config->MenuAccentColorG = color.y;
            config->MenuAccentColorB = color.z;
            RebuildStyle(ctx.menuResScale);
        }
        else
        {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine(0.0f, 6.0f);

        color = colorYellow;
        ImGui::PushStyleColor(ImGuiCol_Button, AccentSoft(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentMed(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentStrong(color));

        if (ImGui::Button("Yellow"))
        {
            ImGui::PopStyleColor(3);

            config->MenuAccentColorR = color.x;
            config->MenuAccentColorG = color.y;
            config->MenuAccentColorB = color.z;
            RebuildStyle(ctx.menuResScale);
        }
        else
        {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine(0.0f, 6.0f);

        color = colorGreen;
        ImGui::PushStyleColor(ImGuiCol_Button, AccentSoft(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentMed(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentStrong(color));

        if (ImGui::Button("Green"))
        {
            ImGui::PopStyleColor(3);

            config->MenuAccentColorR = color.x;
            config->MenuAccentColorG = color.y;
            config->MenuAccentColorB = color.z;
            RebuildStyle(ctx.menuResScale);
        }
        else
        {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine(0.0f, 6.0f);

        color = colorRed;
        ImGui::PushStyleColor(ImGuiCol_Button, AccentSoft(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentMed(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentStrong(color));

        if (ImGui::Button("Red"))
        {
            ImGui::PopStyleColor(3);

            config->MenuAccentColorR = color.x;
            config->MenuAccentColorG = color.y;
            config->MenuAccentColorB = color.z;
            RebuildStyle(ctx.menuResScale);
        }
        else
        {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine(0.0f, 6.0f);

        color = colorOrange;
        ImGui::PushStyleColor(ImGuiCol_Button, AccentSoft(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentMed(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentStrong(color));

        if (ImGui::Button("Orange"))
        {
            ImGui::PopStyleColor(3);

            config->MenuAccentColorR = color.x;
            config->MenuAccentColorG = color.y;
            config->MenuAccentColorB = color.z;
            RebuildStyle(ctx.menuResScale);
        }
        else
        {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine(0.0f, 6.0f);

        color = colorPurple;
        ImGui::PushStyleColor(ImGuiCol_Button, AccentSoft(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentMed(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentStrong(color));

        if (ImGui::Button("Purple"))
        {
            ImGui::PopStyleColor(3);

            config->MenuAccentColorR = color.x;
            config->MenuAccentColorG = color.y;
            config->MenuAccentColorB = color.z;
            RebuildStyle(ctx.menuResScale);
        }
        else
        {
            ImGui::PopStyleColor(3);
        }

        float accentColor[3] = { config->MenuAccentColorR.value_or_default(),
                                 config->MenuAccentColorG.value_or_default(),
                                 config->MenuAccentColorB.value_or_default() };

        if (ImGui::ColorEdit3("Custom Accent Color", accentColor))
        {
            config->MenuAccentColorR = accentColor[0];
            config->MenuAccentColorG = accentColor[1];
            config->MenuAccentColorB = accentColor[2];
            RebuildStyle(ctx.menuResScale);
        }

        ImGui::Spacing();

        if (ImGui::Button("Reset Accent Color"))
        {
            config->MenuAccentColorR.reset();
            config->MenuAccentColorG.reset();
            config->MenuAccentColorB.reset();
            RebuildStyle(ctx.menuResScale);
        }

        ImGui::Spacing();

        ImGui::SeparatorText("Background Colour");

        ImGui::Text("Presets:");
        ImGui::SameLine(0.0f, 6.0f);

        color = colorBlue;
        ImGui::PushStyleColor(ImGuiCol_Button, AccentSoft(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentMed(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentStrong(color));

        if (ImGui::Button("Blue##2"))
        {
            ImGui::PopStyleColor(3);

            config->MenuBGColorR = color.x;
            config->MenuBGColorG = color.y;
            config->MenuBGColorB = color.z;
            RebuildStyle(ctx.menuResScale);
        }
        else
        {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine(0.0f, 6.0f);

        color = colorTeal;
        ImGui::PushStyleColor(ImGuiCol_Button, AccentSoft(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentMed(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentStrong(color));

        if (ImGui::Button("Teal##2"))
        {
            ImGui::PopStyleColor(3);

            config->MenuBGColorR = color.x;
            config->MenuBGColorG = color.y;
            config->MenuBGColorB = color.z;
            RebuildStyle(ctx.menuResScale);
        }
        else
        {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine(0.0f, 6.0f);

        color = colorGray;
        ImGui::PushStyleColor(ImGuiCol_Button, AccentSoft(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentMed(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentStrong(color));

        if (ImGui::Button("Gray##2"))
        {
            ImGui::PopStyleColor(3);

            config->MenuBGColorR = color.x;
            config->MenuBGColorG = color.y;
            config->MenuBGColorB = color.z;
            RebuildStyle(ctx.menuResScale);
        }
        else
        {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine(0.0f, 6.0f);

        color = colorYellow;
        ImGui::PushStyleColor(ImGuiCol_Button, AccentSoft(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentMed(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentStrong(color));

        if (ImGui::Button("Yellow##2"))
        {
            ImGui::PopStyleColor(3);

            config->MenuBGColorR = color.x;
            config->MenuBGColorG = color.y;
            config->MenuBGColorB = color.z;
            RebuildStyle(ctx.menuResScale);
        }
        else
        {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine(0.0f, 6.0f);

        color = colorGreen;
        ImGui::PushStyleColor(ImGuiCol_Button, AccentSoft(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentMed(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentStrong(color));

        if (ImGui::Button("Green##2"))
        {
            ImGui::PopStyleColor(3);

            config->MenuBGColorR = color.x;
            config->MenuBGColorG = color.y;
            config->MenuBGColorB = color.z;
            RebuildStyle(ctx.menuResScale);
        }
        else
        {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine(0.0f, 6.0f);

        color = colorRed;
        ImGui::PushStyleColor(ImGuiCol_Button, AccentSoft(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentMed(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentStrong(color));

        if (ImGui::Button("Red##2"))
        {
            ImGui::PopStyleColor(3);

            config->MenuBGColorR = color.x;
            config->MenuBGColorG = color.y;
            config->MenuBGColorB = color.z;
            RebuildStyle(ctx.menuResScale);
        }
        else
        {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine(0.0f, 6.0f);

        color = colorOrange;
        ImGui::PushStyleColor(ImGuiCol_Button, AccentSoft(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentMed(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentStrong(color));

        if (ImGui::Button("Orange##2"))
        {
            ImGui::PopStyleColor(3);

            config->MenuBGColorR = color.x;
            config->MenuBGColorG = color.y;
            config->MenuBGColorB = color.z;
            RebuildStyle(ctx.menuResScale);
        }
        else
        {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine(0.0f, 6.0f);

        color = colorPurple;
        ImGui::PushStyleColor(ImGuiCol_Button, AccentSoft(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentMed(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentStrong(color));

        if (ImGui::Button("Purple##2"))
        {
            ImGui::PopStyleColor(3);

            config->MenuBGColorR = color.x;
            config->MenuBGColorG = color.y;
            config->MenuBGColorB = color.z;
            RebuildStyle(ctx.menuResScale);
        }
        else
        {
            ImGui::PopStyleColor(3);
        }

        float bgColor[3] = { config->MenuBGColorR.value_or_default(), config->MenuBGColorG.value_or_default(),
                             config->MenuBGColorB.value_or_default() };

        if (ImGui::ColorEdit3("Custom BG Colour", bgColor))
        {
            config->MenuBGColorR = bgColor[0];
            config->MenuBGColorG = bgColor[1];
            config->MenuBGColorB = bgColor[2];
            RebuildStyle(ctx.menuResScale);
        }

        ImGui::Spacing();

        auto alpha = config->MenuBGColorA.value_or_default();
        if (ImGui::SliderFloat("Background Alpha", &alpha, 0.0f, 1.0f))
        {
            config->MenuBGColorA = alpha;
            RebuildStyle(ctx.menuResScale);
        }

        ImGui::Spacing();

        if (ImGui::Button("Reset BG Colour"))
        {
            config->MenuBGColorR.reset();
            config->MenuBGColorG.reset();
            config->MenuBGColorB.reset();
            config->MenuBGColorA.reset();
            RebuildStyle(ctx.menuResScale);
        }

        ImGui::Spacing();
    }
}

void MenuCommon::RenderFpsOverlaySettings(RenderMenuContext& ctx)
{
    auto config = ctx.config;

    // FPS OVERLAY -----------------------------
    ImGui::Spacing();
    if (auto ch = ScopedCollapsingHeader("FPS Overlay"); ch.IsHeaderOpen())
    {
        ScopedIndent indent {};
        ImGui::Spacing();

        bool fpsEnabled = config->ShowFps.value_or_default();
        if (ImGui::Checkbox("FPS Overlay Enabled", &fpsEnabled))
            config->ShowFps = fpsEnabled;

        ImGui::SameLine(0.0f, 6.0f);

        bool fpsHorizontal = config->FpsOverlayHorizontal.value_or_default();
        if (ImGui::Checkbox("Horizontal", &fpsHorizontal))
            config->FpsOverlayHorizontal = fpsHorizontal;

        ImGui::SameLine(0.0f, 6.0f);

        bool fpsShowRatio = config->FpsOverlayShowRatio.value_or_default();
        if (ImGui::Checkbox("Upscale Ratio", &fpsShowRatio))
            config->FpsOverlayShowRatio = fpsShowRatio;
        ShowTooltip("Shows the current render -> output resolution and upscale ratio in the overlay");

        const char* fpsPosition[] = { "Top Left", "Top Right", "Bottom Left", "Bottom Right" };
        const char* selectedPosition = fpsPosition[config->FpsOverlayPosition.value_or_default()];

        if (ImGui::BeginCombo("Overlay Position", selectedPosition))
        {
            for (int n = 0; n < std::size(fpsPosition); n++)
            {
                if (ImGui::Selectable(fpsPosition[n], (config->FpsOverlayPosition.value_or_default() == n)))
                    config->FpsOverlayPosition = (FpsOverlayPos) n;
            }

            ImGui::EndCombo();
        }

        const char* fpsType[] = { "Just FPS", "Simple",       "Detailed",      "Detailed + Graph",
                                  "Full",     "Full + Graph", "Reflex timings" };
        const char* selectedType = fpsType[config->FpsOverlayType.value_or_default()];

        if (ImGui::BeginCombo("Overlay Type", selectedType))
        {
            for (int n = 0; n < std::size(fpsType); n++)
            {
                if (ImGui::Selectable(fpsType[n], (config->FpsOverlayType.value_or_default() == n)))
                    config->FpsOverlayType = (FpsOverlay) n;
            }

            ImGui::EndCombo();
        }

        MenuCommon::ShowHelpMarker("Reflex timings: the bars show when each stage of the frame ran. Input latency "
                                   "runs from the start of the simulation to the GPU finishing the frame, or to "
                                   "Present when GPU timing is not available. It does not include the mouse or the "
                                   "monitor.");

        float fpsAlpha = config->FpsOverlayAlpha.value_or_default();
        if (ImGui::SliderFloat("Background Alpha", &fpsAlpha, 0.0f, 1.0f, "%.2f"))
            config->FpsOverlayAlpha = fpsAlpha;

        const char* options[] = { "Same as menu", "0.5", "0.6", "0.7", "0.8", "0.9", "1.0", "1.1", "1.2",
                                  "1.3",          "1.4", "1.5", "1.6", "1.7", "1.8", "1.9", "2.0" };
        int currentIndex = std::max(((int) (config->FpsScale.value_or(0.0f) * 10.0f)) - 4, 0);
        float values[] = { 0.0f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f, 1.1f, 1.2f,
                           1.3f, 1.4f, 1.5f, 1.6f, 1.7f, 1.8f, 1.9f, 2.0f };

        if (ImGui::SliderInt("Scale", &currentIndex, 0, IM_ARRAYSIZE(options) - 1, options[currentIndex],
                             ImGuiSliderFlags_ClampOnInput))
        {
            if (currentIndex == 0)
                config->FpsScale.reset();
            else
                config->FpsScale = values[currentIndex];
        }

        bool useTheme = config->OverlaysUseTheme.value_or_default();
        if (ImGui::Checkbox("Use Theme Colors", &useTheme))
            config->OverlaysUseTheme = useTheme;
    }
}

void MenuCommon::RenderUpscalerInputsSettings(RenderMenuContext& ctx)
{
    auto config = ctx.config;
    auto& currentFeature = ctx.currentFeature;

    // UPSCALER INPUTS -----------------------------
    ImGui::Spacing();
    auto uiStateOpen = currentFeature == nullptr || currentFeature->IsFrozen();
    if (auto ch = ScopedCollapsingHeader("Upscaler Inputs", uiStateOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0);
        ch.IsHeaderOpen())
    {
        ScopedIndent indent {};
        ImGui::Spacing();

        if (config->EnableFsr2Inputs.value_or_default())
        {
            bool fsr2Inputs = config->UseFsr2Inputs.value_or_default();
            bool fsr2Pattern = config->Fsr2Pattern.value_or_default();

            if (ImGui::Checkbox("Use Fsr2 Inputs", &fsr2Inputs))
                config->UseFsr2Inputs = fsr2Inputs;

            if (ImGui::Checkbox("Use Fsr2 Pattern Matching", &fsr2Pattern))
                config->Fsr2Pattern = fsr2Pattern;
            ShowTooltip("This setting will become active on next boot!");
        }

        if (config->EnableFsr3Inputs.value_or_default())
        {
            bool fsr3Inputs = config->UseFsr3Inputs.value_or_default();
            bool fsr3Pattern = config->Fsr3Pattern.value_or_default();

            if (ImGui::Checkbox("Use Fsr3 Inputs", &fsr3Inputs))
                config->UseFsr3Inputs = fsr3Inputs;

            if (ImGui::Checkbox("Use Fsr3 Pattern Matching", &fsr3Pattern))
                config->Fsr3Pattern = fsr3Pattern;
            ShowTooltip("This setting will become active on next boot!");
        }

        if (config->EnableFfxInputs.value_or_default())
        {
            bool ffxInputs = config->UseFfxInputs.value_or_default();

            if (ImGui::Checkbox("Use Ffx Inputs", &ffxInputs))
                config->UseFfxInputs = ffxInputs;
        }
    }
}

void MenuCommon::RenderApiAndTextureSettings(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    auto& currentFeature = ctx.currentFeature;
    auto& menuResScale = ctx.menuResScale;

    // DX11 & DX12 -----------------------------
    if (state.swapchainApi != Vulkan || state.swapchainInteropApi == SwapchainInteropApi::VkwDx12)
    {
        // V-SYNC -----------------------------
        ImGui::Spacing();
        if (auto ch = ScopedCollapsingHeader("V-Sync Settings"); ch.IsHeaderOpen())
        {
            ScopedIndent indent {};
            ImGui::Spacing();

            auto forceVsyncOn = config->ForceVsync.has_value() && config->ForceVsync.value();
            auto forceVsyncOff = config->ForceVsync.has_value() && !config->ForceVsync.value();
            bool vsyncChanged = false;

            if (ImGui::Checkbox("V-Sync On", &forceVsyncOn))
            {
                if (forceVsyncOn)
                {
                    config->ForceVsync = true;
                    vsyncChanged = true;
                }
                else
                {
                    config->ForceVsync.reset();
                    vsyncChanged = true;
                }
            }
            ImGui::SameLine(0.0f, 16.0f);

            if (ImGui::Checkbox("V-Sync Off", &forceVsyncOff))
            {
                if (forceVsyncOff)
                {
                    config->ForceVsync = false;
                    vsyncChanged = true;
                }
                else
                {
                    config->ForceVsync.reset();
                    vsyncChanged = true;
                }
            }
            ImGui::SameLine(0.0f, 16.0f);

            ImGui::BeginDisabled(!forceVsyncOn);

            ImGui::PushItemWidth(50.0f * menuResScale);

            auto vsyncBuf = StrFmt("%d", config->VsyncInterval.value_or_default());
            if (ImGui::BeginCombo("Sync Int.", vsyncBuf.c_str()))
            {
                if (ImGui::Selectable("0", config->VsyncInterval.value_or_default() == 0))
                {
                    config->VsyncInterval = 0;
                    vsyncChanged = true;
                }

                if (ImGui::Selectable("1", config->VsyncInterval.value_or_default() == 1))
                {
                    config->VsyncInterval = 1;
                    vsyncChanged = true;
                }

                if (ImGui::Selectable("2", config->VsyncInterval.value_or_default() == 2))
                {
                    config->VsyncInterval = 2;
                    vsyncChanged = true;
                }

                if (ImGui::Selectable("3", config->VsyncInterval.value_or_default() == 3))
                {
                    config->VsyncInterval = 3;
                    vsyncChanged = true;
                }

                ImGui::EndCombo();
            }
            ImGui::PopItemWidth();

            ShowHelpMarker("Controls the DXGI Present sync interval, which determines how\n"
                           "the swap chain waits for vertical refresh.\n\n"
                           "0  = Present immediately, no VSync wait.\n"
                           "1  = Sync to every refresh, normal VSync.\n"
                           "2+ = Present every N refreshes, reducing effective frame rate.\n\n"
                           "Higher values can reduce tearing but may increase latency and cap FPS.\n"
                           "For most games, use 0 for lowest latency or 1 for normal VSync.");

            ImGui::EndDisabled();
            ImGui::SameLine(0.0f, 16.0f);

            if (ImGui::Button("Reset##10"))
            {
                config->ForceVsync.reset();
                vsyncChanged = true;
            }

            ShowHelpMarker("Force V-Sync On/Off & Sync Interval options");

            // FGHooks::FGPresent sends 1 instead (Intel: XeLL, which XeFG runs with, needs sync interval 1)
            if (config->ForceVsync.value_or(false) && config->VsyncInterval.value_or_default() > 1 &&
                UsesXeFGApi(state.activeFgOutput))
            {
                ImGui::TextDisabled("XeFG presents with sync interval 1: %d is not used while it runs.",
                                    config->VsyncInterval.value_or_default());
            }

            if (vsyncChanged && UsesXeFGApi(state.activeFgOutput) && state.currentFG != nullptr)
            {
                // To prevent XeLL issues
                LOG_DEBUG("V-Sync change detected, forcing XeFG reset");
                state.WAR_xefgRequestFGToggle = true;
            }
        }

        // MIPMAP BIAS & Anisotropy -----------------------------
        ImGui::Spacing();
        if (auto ch = ScopedCollapsingHeader("Mipmap Bias", (currentFeature == nullptr || currentFeature->IsFrozen())
                                                                ? ImGuiTreeNodeFlags_DefaultOpen
                                                                : 0);
            ch.IsHeaderOpen())
        {
            ScopedIndent indent {};
            ImGui::Spacing();
            if (config->MipmapBiasOverride.has_value() && _mipBias == 0.0f)
                _mipBias = config->MipmapBiasOverride.value();

            ImGui::SliderFloat("Mipmap Bias##2", &_mipBias, -15.0f, 15.0f, "%.6f");
            ShowHelpMarker("Can help with blurry textures in broken games\n"
                           "Negative values will make textures sharper\n"
                           "Positive values will make textures more blurry\n\n"
                           "Has a small performance impact");

            ImGui::BeginDisabled(!config->MipmapBiasOverride.has_value());
            {
                ImGui::BeginDisabled(config->MipmapBiasScaleOverride.has_value() &&
                                     config->MipmapBiasScaleOverride.value());
                {
                    bool mbFixed = config->MipmapBiasFixedOverride.value_or_default();
                    if (ImGui::Checkbox("MB Fixed Override", &mbFixed))
                    {
                        config->MipmapBiasScaleOverride.reset();
                        config->MipmapBiasFixedOverride = mbFixed;
                    }

                    ShowHelpMarker("Apply same override value to all textures");
                }
                ImGui::EndDisabled();

                ImGui::SameLine(0.0f, 6.0f);

                ImGui::BeginDisabled(config->MipmapBiasFixedOverride.has_value() &&
                                     config->MipmapBiasFixedOverride.value());
                {
                    bool mbScale = config->MipmapBiasScaleOverride.value_or_default();
                    if (ImGui::Checkbox("MB Scale Override", &mbScale))
                    {
                        config->MipmapBiasFixedOverride.reset();
                        config->MipmapBiasScaleOverride = mbScale;
                    }

                    ShowHelpMarker("Apply override value as scale multiplier\n"
                                   "When using scale mode, please use positive\n"
                                   "override values to increase sharpness!");
                }
                ImGui::EndDisabled();

                bool mbAll = config->MipmapBiasOverrideAll.value_or_default();
                if (ImGui::Checkbox("MB Override All Textures", &mbAll))
                    config->MipmapBiasOverrideAll = mbAll;

                ShowHelpMarker("Override all textures mipmap values\n"
                               "Normally OptiScaler only overrides\n"
                               "below zero mipmap values!");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(config->MipmapBiasOverride.has_value() &&
                                 config->MipmapBiasOverride.value() == _mipBias);
            {
                if (ImGui::Button("Set"))
                {
                    config->MipmapBiasOverride = _mipBias;
                    state.lastMipBias = 100.0f;
                    state.lastMipBiasMax = -100.0f;
                }
            }
            ImGui::EndDisabled();

            ImGui::SameLine(0.0f, 6.0f);

            ImGui::BeginDisabled(!config->MipmapBiasOverride.has_value());
            {
                if (ImGui::Button("Reset"))
                {
                    config->MipmapBiasOverride.reset();
                    _mipBias = 0.0f;
                    state.lastMipBias = 100.0f;
                    state.lastMipBiasMax = -100.0f;
                }
            }
            ImGui::EndDisabled();

            if (currentFeature != nullptr && !currentFeature->IsFrozen())
            {
                ImGui::SameLine(0.0f, 6.0f);

                if (ImGui::Button("Calculate Mipmap Bias"))
                    _showMipmapCalcWindow = true;
            }

            if (config->MipmapBiasOverride.has_value())
            {
                if (config->MipmapBiasFixedOverride.value_or_default())
                {
                    ImGui::Text("Current : %.3f / %.3f, Target: %.3f", state.lastMipBias, state.lastMipBiasMax,
                                config->MipmapBiasOverride.value());
                }
                else if (config->MipmapBiasScaleOverride.value_or_default())
                {
                    ImGui::Text("Current : %.3f / %.3f, Target: Base * %.3f", state.lastMipBias, state.lastMipBiasMax,
                                config->MipmapBiasOverride.value());
                }
                else
                {
                    ImGui::Text("Current : %.3f / %.3f, Target: Base + %.3f", state.lastMipBias, state.lastMipBiasMax,
                                config->MipmapBiasOverride.value());
                }
            }
            else
            {
                ImGui::Text("Current : %.3f / %.3f", state.lastMipBias, state.lastMipBiasMax);
            }

            ImGui::Text("Will be applied after RESOLUTION/PRESET change !!!");
        }

        ImGui::Spacing();
        if (auto ch = ScopedCollapsingHeader(
                "Anisotropic Filtering",
                (currentFeature == nullptr || currentFeature->IsFrozen()) ? ImGuiTreeNodeFlags_DefaultOpen : 0);
            ch.IsHeaderOpen())
        {
            ScopedIndent indent {};
            ImGui::Spacing();
            ImGui::PushItemWidth(65.0f * menuResScale);

            auto selectedAF =
                config->AnisotropyOverride.has_value() ? std::to_string(config->AnisotropyOverride.value()) : "Auto";
            if (ImGui::BeginCombo("Force Anisotropic Filtering", selectedAF.c_str()))
            {
                if (ImGui::Selectable("Auto", !config->AnisotropyOverride.has_value()))
                    config->AnisotropyOverride.reset();

                if (ImGui::Selectable("1", config->AnisotropyOverride.value_or(0) == 1))
                    config->AnisotropyOverride = 1;

                if (ImGui::Selectable("2", config->AnisotropyOverride.value_or(0) == 2))
                    config->AnisotropyOverride = 2;

                if (ImGui::Selectable("4", config->AnisotropyOverride.value_or(0) == 4))
                    config->AnisotropyOverride = 4;

                if (ImGui::Selectable("8", config->AnisotropyOverride.value_or(0) == 8))
                    config->AnisotropyOverride = 8;

                if (ImGui::Selectable("16", config->AnisotropyOverride.value_or(0) == 16))
                    config->AnisotropyOverride = 16;

                ImGui::EndCombo();
            }

            ImGui::PopItemWidth();

            bool afComp = config->AnisotropyModifyComp.value_or_default();
            if (ImGui::Checkbox("Modify Compare", &afComp))
                config->AnisotropyModifyComp = afComp;

            ShowHelpMarker("Update comparison filters");

            ImGui::SameLine(0.0f, 6.0f);

            bool afMinMax = config->AnisotropyModifyMinMax.value_or_default();
            if (ImGui::Checkbox("Modify Min/Max", &afMinMax))
                config->AnisotropyModifyMinMax = afMinMax;

            ShowHelpMarker("Update min/max filters");

            bool afSkipPoint = config->AnisotropySkipPointFilter.value_or_default();
            if (ImGui::Checkbox("Skip Point Filters", &afSkipPoint))
                config->AnisotropySkipPointFilter = afSkipPoint;

            ShowHelpMarker("Skip updating of point filters");

            ImGui::Text("Will might be applied after RESOLUTION/PRESET change !!!");
        }
    }
}

void MenuCommon::RenderKeybindSettings(RenderMenuContext& ctx)
{
    auto config = ctx.config;

    ImGui::Spacing();
    if (auto ch = ScopedCollapsingHeader("Keybinds"); ch.IsHeaderOpen())
    {
        ScopedIndent indent {};
        ImGui::Spacing();

        ImGui::Text("Key combinations are currently NOT supported!");
        ImGui::Text("Escape to cancel, Backspace to unbind");
        ImGui::Spacing();

        static auto menu = Keybind("Menu", 10);
        static auto fpsOverlay = Keybind("FPS Overlay", 11);
        static auto fpsOverlayCycle = Keybind("FPS Overlay Cycle", 12);
        static auto fgEnable = Keybind("Frame Generation", 13);
        static auto dlssNrToggle = Keybind("Neural Rendering", 14);
        static auto hdrCompare = Keybind("HDR compare", 15);

        menu.Render(config->ShortcutKey);
        fpsOverlay.Render(config->FpsShortcutKey);
        fpsOverlayCycle.Render(config->FpsCycleShortcutKey);
        fgEnable.Render(config->FGShortcutKey);
        dlssNrToggle.Render(config->DlssNrToggleKey);
        hdrCompare.Render(config->HdrCompareKey);
    }
}

// Height of the graphs and bottom bar, measured last frame so the page view can leave room for them.
static float mainMenuFooterHeight = 0.0f;

void MenuCommon::RenderMainMenuPages(RenderMenuContext& ctx)
{
    using MenuPages::Page;

    auto config = ctx.config;
    const float menuResScale = ctx.menuResScale;

    static Page page = Page::Upscaler;
    static bool pageLoaded = false;
    static bool nrOpen = false;
    static Page lastDrawnPage = Page::Upscaler;

    if (!pageLoaded)
    {
        pageLoaded = true;
        page = MenuPages::PageFromName(config->MenuPage.value_or(""));
        nrOpen = MenuPages::IsNeuralRendering(page);
        lastDrawnPage = page;
    }

    const auto select = [&](Page next)
    {
        if (next != page)
        {
            page = next;
            config->MenuPage = std::string(MenuPages::Name(next));
        }
    };

    // A page another part of the menu asked for (the header's Optical F5Low button): open its group too.
    if (const auto requested = MenuPages::ConsumeRequest())
    {
        select(*requested);
        nrOpen = nrOpen || MenuPages::IsNeuralRendering(*requested);
    }

    const float footerHeight = mainMenuFooterHeight > 0.0f ? mainMenuFooterHeight : 110.0f * menuResScale;
    const float bodyHeight =
        std::max(ImGui::GetContentRegionAvail().y - footerHeight - ImGui::GetStyle().ItemSpacing.y - 2.0f,
                 120.0f * menuResScale);

    // Navigation pane.
    // Modern borderless panes need the flag to keep their window padding.
    const bool modern = MenuStyle::IsModern();
    const ImGuiChildFlags paneFlags =
        ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened | (modern ? ImGuiChildFlags_AlwaysUseWindowPadding : 0);

    if (ImGui::BeginChild("nav", ImVec2(190.0f * menuResScale, bodyHeight), paneFlags))
    {
        // Modern: soft highlight for the selected/hovered row, plus an accent bar drawn at the selected row's edge.
        if (modern)
        {
            ImGui::PushStyleColor(ImGuiCol_Header, gModernPalette.navSelected);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, gModernPalette.navHover);
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, gModernPalette.navActive);
        }

        const auto accentBar = [&]()
        {
            if (!modern)
                return;

            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRectFilled(min, ImVec2(min.x + 3.0f * menuResScale, max.y),
                                                      ImGui::ColorConvertFloat4ToU32(gModernPalette.navBar),
                                                      1.5f * menuResScale);
        };

        // Leaf tree rows keep the arrow's slot empty, so every top-level label lines up with Neural Rendering's.
        const auto navItem = [&](Page target)
        {
            ImGui::TreeNodeEx(MenuPages::Label(target), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                            ImGuiTreeNodeFlags_SpanAvailWidth |
                                                            (page == target ? ImGuiTreeNodeFlags_Selected : 0));
            if (ImGui::IsItemActivated())
                select(target);

            if (page == target)
                accentBar();
        };

        navItem(Page::Upscaler);
        navItem(Page::FrameGeneration);
        navItem(Page::Framerate);

        // Clicking the label opens Status & Presets; the arrow only expands.
        ImGui::SetNextItemOpen(nrOpen);
        const bool nrSelected = MenuPages::IsNeuralRendering(page);
        const bool nrShownSelected = nrSelected && !nrOpen;
        const bool nrNodeOpen =
            ImGui::TreeNodeEx("Neural Rendering", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                                                      (nrShownSelected ? ImGuiTreeNodeFlags_Selected : 0));
        if (ImGui::IsItemToggledOpen())
        {
            nrOpen = nrNodeOpen;

            // Keyboard/gamepad activation toggles rather than clicks; opening that way also shows Status & Presets.
            if (nrNodeOpen && !ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                select(Page::NrStatus);
        }
        else if (ImGui::IsItemClicked())
        {
            select(Page::NrStatus);
            nrOpen = true;
        }

        if (nrShownSelected)
            accentBar();

        if (nrNodeOpen)
        {
            for (int p = static_cast<int>(Page::NrF5Low); p < static_cast<int>(MenuPages::kPageCount); p++)
                navItem(static_cast<Page>(p));

            ImGui::TreePop();
        }

        navItem(Page::F5Lut);
        navItem(Page::Image);
        navItem(Page::OverlayLook);
        navItem(Page::Input);
        navItem(Page::Misc);
        navItem(Page::Plugins);
        navItem(Page::Notifications);

        if (modern)
            ImGui::PopStyleColor(3);
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Page pane, scrolls on its own.
    if (ImGui::BeginChild("page", ImVec2(0.0f, bodyHeight), paneFlags))
    {
        if (lastDrawnPage != page)
        {
            lastDrawnPage = page;
            ImGui::SetScrollY(0.0f);
        }

        // Each section function is called whole; openFirstHeader makes the header it starts with open by default.
        const auto section = [&](void (*render)(RenderMenuContext&), bool openFirstHeader = false)
        {
            ScopedCollapsingHeader::OpenNextHeader = openFirstHeader;
            render(ctx);
            ScopedCollapsingHeader::OpenNextHeader = false;
        };

        switch (page)
        {
        case Page::Upscaler:
            section(RenderActiveUpscalerSettings);
            section(RenderUpscalerInputsSettings, true);
            break;
        case Page::FrameGeneration:
            section(RenderFrameGenerationSelection);
            section(RenderFrameGenerationRuntimeSettings);
            section(RenderFsrCommonSettings);
            section(RenderFrameGenerationFooter);
            break;
        case Page::Image:
            section(RenderActiveImageSettings);
            section(RenderHdrOutputSettings);
            break;
        case Page::F5Lut:
            section(RenderF5LutSettings);
            break;
        case Page::Framerate:
            section(RenderFramerateLowLatency);
            section(RenderFramerateSettings);
            section(RenderFakenvapiSettings);
            break;
        case Page::OverlayLook:
            section(RenderFpsOverlaySettings, true);
            section(RenderMagnifierSettings, true);
            section(RenderThemeSettings, true);
            break;
        case Page::Input:
            section(RenderKeybindSettings, true);
            break;
        case Page::Misc:
            section(RenderQuirksSettings, true);
            section(RenderAdvancedSettings, true);
            section(RenderLoggingSettings, true);
            section(RenderApiAndTextureSettings, true);
            break;
        case Page::Plugins:
            PluginsPage::Render(ctx.config);
            break;
        case Page::Notifications:
            NotificationsPage::Render();
            break;
        case Page::NrF5Low:
        case Page::NrStatus:
        case Page::NrOptions:
        case Page::NrInput:
        case Page::NrOutput:
        case Page::NrPasses:
        case Page::NrDebug:
            DlssNr::RenderMenu(ctx.config, ctx.menuResScale, page);
            break;
        default:
            IM_ASSERT(false && "page without a case in RenderMainMenuPages");
            break;
        }
    }
    ImGui::EndChild();
}

void MenuCommon::RenderMainMenuGraphs(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto& currentFeature = ctx.currentFeature;
    auto& frameTime = ctx.frameTime;
    auto& frameRate = ctx.frameRate;

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::BeginTable("plots", 2, ImGuiTableFlags_SizingStretchSame))
    {
        ImGui::TableNextColumn();
        ImGui::Text("FrameTime");
        auto ft = StrFmt("%7.2f ms / %6.1f fps", frameTime, frameRate);
        {
            ScopedMonoFont monoFont {};
            ImGui::PlotLines(
                ft.c_str(), [](void* rb, int idx) -> float
                { return static_cast<RingBuffer<float, plotWidth>*>(rb)->At(idx); }, &gFrameTimes, plotWidth);
        }

        if (currentFeature != nullptr && !currentFeature->IsFrozen())
        {
            ImGui::TableNextColumn();
            ImGui::Text("Upscaler");

            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !state.detailedGpuTimes.empty())
            {
                ImGui::BeginTooltip();
                std::optional<ScopedMonoFont> monoFont;
                monoFont.emplace();

                ImGui::TextDisabled("Per shader breakdown:");
                if (ImGui::BeginTable("ShaderTimes", 2, ImGuiTableFlags_SizingStretchProp))
                {
                    bool hasExtra = false;

                    for (auto& [name, time, includedInUpscalerTime] : state.detailedGpuTimes)
                    {
                        if (!includedInUpscalerTime)
                        {
                            hasExtra = true;
                            continue;
                        }

                        auto formattedTime = StrFmt("%7.2f ms", time);

                        ImGui::TableNextColumn();
                        ImGui::Text(name.c_str());

                        ImGui::TableNextColumn();
                        ImGui::Text(formattedTime.c_str());
                    }

                    std::optional<double> nrTime {};
                    nrTime = DlssNr::LastGpuTime();
                    if (hasExtra || nrTime.has_value())
                    {
                        ImGui::TableNextRow();
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("Extra shaders:");
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("");
                        for (auto& [name, time, includedInUpscalerTime] : state.detailedGpuTimes)
                        {
                            if (includedInUpscalerTime)
                                continue;

                            auto formattedTime = StrFmt("%7.2f ms", time);

                            ImGui::TableNextColumn();
                            ImGui::Text(name.c_str());

                            ImGui::TableNextColumn();
                            ImGui::Text(formattedTime.c_str());
                        }

                        if (nrTime.has_value())
                        {
                            ImGui::TableNextColumn();
                            ImGui::Text("Neural Rendering (elapsed)");
                            ImGui::TableNextColumn();
                            ImGui::Text(StrFmt("%.2f ms", nrTime.value()).c_str());
                        }
                    }

                    ImGui::EndTable();
                }

                monoFont.reset(); // the font must be popped inside the tooltip window
                ImGui::EndTooltip();
            }

            auto ups = StrFmt("%7.2f ms", state.upscaleTimes.back());
            {
                ScopedMonoFont monoFont {};
                ImGui::PlotLines(
                    ups.c_str(), [](void* rb, int idx) -> float
                    { return static_cast<RingBuffer<float, plotWidth>*>(rb)->At(idx); }, &gUpscalerTimes, plotWidth);
            }
        }

        ImGui::EndTable();
    }
}

void MenuCommon::RenderMainMenuBottomBar(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    auto& io = ctx.io;
    auto& currentFeature = ctx.currentFeature;
    auto& menuResScale = ctx.menuResScale;

    // BOTTOM LINE ---------------
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (!MenuStyle::IsModern())
    {
        if (currentFeature != nullptr && !currentFeature->IsFrozen())
        {
            ImGui::Text("%dx%d -> %dx%d (%.1f) [%dx%d (%.1f)]", currentFeature->RenderWidth(),
                        currentFeature->RenderHeight(), currentFeature->TargetWidth(), currentFeature->TargetHeight(),
                        (float) currentFeature->TargetWidth() / (float) currentFeature->RenderWidth(),
                        currentFeature->DisplayWidth(), currentFeature->DisplayHeight(),
                        (float) currentFeature->DisplayWidth() / (float) currentFeature->RenderWidth());

            ImGui::SameLine(0.0f, 4.0f);

            ImGui::Text("%d", currentFeature->FrameCount());

            ImGui::SameLine(0.0f, 10.0f);
        }
    }
    else
    {
        // Modern: fixed-width font in a fixed-width slot, so the counter and frozen state never move the controls.
        const float statusX = ImGui::GetCursorPosX();
        ScopedMonoFont monoFont {};
        float slotWidth = ImGui::CalcTextSize("0000x0000 -> 0000x0000 (0.0) [0000x0000 (0.0)] 0000000").x;

        if (currentFeature != nullptr && !currentFeature->IsFrozen())
        {
            const auto status =
                StrFmt("%dx%d -> %dx%d (%.1f) [%dx%d (%.1f)] %d", currentFeature->RenderWidth(),
                       currentFeature->RenderHeight(), currentFeature->TargetWidth(), currentFeature->TargetHeight(),
                       (float) currentFeature->TargetWidth() / (float) currentFeature->RenderWidth(),
                       currentFeature->DisplayWidth(), currentFeature->DisplayHeight(),
                       (float) currentFeature->DisplayWidth() / (float) currentFeature->RenderWidth(),
                       currentFeature->FrameCount());

            ImGui::TextUnformatted(status.c_str());
            slotWidth = std::max(slotWidth, ImGui::GetItemRectSize().x);
        }
        else
        {
            ImGui::Dummy(ImVec2(slotWidth, ImGui::GetTextLineHeight()));
        }

        ImGui::SameLine(statusX + slotWidth + 10.0f * menuResScale);
    }

    ImGui::PushItemWidth(100.0f * menuResScale);

    auto autoText = config->MenuScale.has_value() ? "Auto" : StrFmt("Auto (%3.1f)", menuResScale);
    // clang-format off
    const char* uiScales[] = { autoText.c_str(), "0.5", "0.6", "0.7", "0.8", "0.9", "1.0", "1.1",
                               "1.2", "1.3", "1.4", "1.5", "1.6", "1.7", "1.8", "1.9", "2.0" };
    // clang-format on

    const char* selectedScaleName = uiScales[_selectedScale];

    if (ImGui::BeginCombo("Menu Scale", selectedScaleName))
    {
        for (int n = 0; n < std::size(uiScales); n++)
        {
            if (ImGui::Selectable(uiScales[n], (_selectedScale == n)))
            {
                _selectedScale = n;

                if (n == 0)
                    config->MenuScale.reset();
                else
                    config->MenuScale = 0.4f + (float) n / 10.0f;
            }
        }

        ImGui::EndCombo();
    }

    ImGui::PopItemWidth();

    ImGui::SameLine(0.0f, 15.0f);

    if (ImGui::Button("Save Settings"))
        config->SaveIni();

    ImGui::SameLine(0.0f, 6.0f);

    if (ImGui::Button("Close"))
    {
        _isVisible = false;
        hasGamepad = (io.BackendFlags | ImGuiBackendFlags_HasGamepad) > 0;
        io.BackendFlags &= 30;
        io.ConfigFlags = ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NoKeyboard;

        _showMipmapCalcWindow = false;
        _showHudlessWindow = false;
        io.MouseDrawCursor = false;
        io.WantCaptureKeyboard = false;
        io.WantCaptureMouse = false;
    }

    auto winSize = ImGui::GetWindowSize();
    auto winPos = ImGui::GetWindowPos();

    ImGui::SameLine();

    auto textSize = ImGui::CalcTextSize("Open Wiki (?)");
    auto& style = ImGui::GetStyle();
    textSize.x += style.FramePadding.x * 2.0f;
    textSize.x += style.ItemSpacing.x;

    float avail = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - textSize.x);

    // Make button text underline
    if (ImGui::Button("Open Wiki"))
    {
        auto pIO = &ImGui::GetPlatformIO();
        auto ctx = ImGui::GetCurrentContext();
        pIO->Platform_OpenInShellFn(ctx, "https://github.com/optiscaler/OptiScaler/wiki");
    }
    ShowHelpMarker("Click to open the OptiScaler Wiki page\nin your default browser\n\n"
                   "Compatibility list with known game issues\nand workarounds, FG options explained\n"
                   "and other useful info");

    ImGui::Spacing();
    ImGui::Separator();

    if (state.nvngxIniDetected)
    {
        ImGui::Spacing();
        ImGui::TextColored(toneMapColor(ImVec4(1.f, 0.f, 0.f, 1.f)),
                           "nvngx.ini detected, please move over to using OptiScaler.ini and delete the old config");
        ImGui::Spacing();
    }

    if (lastPosition.x < -900.0f || (lastPosition.x >= winPos.x - 1.0f && lastPosition.y >= winPos.y - 1.0f &&
                                     lastPosition.x <= winPos.x + 1.0f && lastPosition.y <= winPos.y + 1.0f))
    {
        float posX;
        float posY;

        posX = ((float) io.DisplaySize.x - winSize.x) / 2.0f;
        posY = ((float) io.DisplaySize.y - winSize.y) / 2.0f;

        // don't position menu outside of screen
        if (posX < 0.0 || posY < 0.0)
        {
            posX = 50;
            posY = 50;
        }

        ImGui::SetWindowPos(ImVec2 { posX, posY });
        lastPosition.x = posX;
        lastPosition.y = posY;
    }
}

void MenuCommon::RenderMipmapBiasWindow(RenderMenuContext& ctx, ImGuiWindowFlags flags)
{
    auto config = ctx.config;
    auto& io = ctx.io;
    auto& currentFeature = ctx.currentFeature;

    // Metrics window (for debug)
    // ImGui::ShowMetricsWindow();

    // Mipmap calculation window
    if (_showMipmapCalcWindow && currentFeature != nullptr && !currentFeature->IsFrozen() && currentFeature->IsInited())
    {
        auto posX = (io.DisplaySize.x - 450.0f) / 2.0f;
        auto posY = (io.DisplaySize.y - 200.0f) / 2.0f;

        ImGui::SetNextWindowPos(ImVec2 { posX, posY }, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2 { 450.0f, 200.0f }, ImGuiCond_FirstUseEver);

        if (_displayWidth == 0)
        {
            if (config->OutputScalingEnabled.value_or_default())
            {
                _displayWidth = static_cast<uint32_t>(currentFeature->DisplayWidth() *
                                                      config->OutputScalingMultiplier.value_or_default());
            }
            else
            {
                _displayWidth = currentFeature->DisplayWidth();
            }

            _renderWidth = static_cast<uint32_t>(_displayWidth / 3.0f);
            _mipmapUpscalerQuality = 0;
            _mipmapUpscalerRatio = 3.0f;
            _mipBiasCalculated = log2((float) _renderWidth / (float) _displayWidth);
        }

        if (ImGui::Begin("Mipmap Bias", nullptr, flags))
        {
            if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow))
                ImGui::SetWindowFocus();

            if (ImGui::InputScalar("Display Width", ImGuiDataType_U32, &_displayWidth, NULL, NULL, "%u"))
            {
                if (_displayWidth <= 0)
                {
                    if (config->OutputScalingEnabled.value_or_default())
                    {
                        _displayWidth = static_cast<uint32_t>(currentFeature->DisplayWidth() *
                                                              config->OutputScalingMultiplier.value_or_default());
                    }
                    else
                    {
                        _displayWidth = currentFeature->DisplayWidth();
                    }
                }

                _renderWidth = static_cast<uint32_t>(_displayWidth / _mipmapUpscalerRatio);
                _mipBiasCalculated = log2((float) _renderWidth / (float) _displayWidth);
            }

            const char* q[] = { "Ultra Performance", "Performance", "Balanced", "Quality", "Ultra Quality", "DLAA" };
            float fr[] = { 3.0f, 2.0f, 1.7f, 1.5f, 1.3f, 1.0f };
            auto configQ = _mipmapUpscalerQuality;

            const char* selectedQ = q[configQ];

            ImGui::BeginDisabled(config->UpscaleRatioOverrideEnabled.value_or_default());

            if (ImGui::BeginCombo("Upscaler Quality", selectedQ))
            {
                for (int n = 0; n < 6; n++)
                {
                    if (ImGui::Selectable(q[n], (_mipmapUpscalerQuality == n)))
                    {
                        _mipmapUpscalerQuality = n;

                        float ov = -1.0f;

                        if (config->QualityRatioOverrideEnabled.value_or_default())
                        {
                            switch (n)
                            {
                            case 0:
                                ov = config->QualityRatio_UltraPerformance.value_or(-1.0f);
                                break;

                            case 1:
                                ov = config->QualityRatio_Performance.value_or(-1.0f);
                                break;

                            case 2:
                                ov = config->QualityRatio_Balanced.value_or(-1.0f);
                                break;

                            case 3:
                                ov = config->QualityRatio_Quality.value_or(-1.0f);
                                break;

                            case 4:
                                ov = config->QualityRatio_UltraQuality.value_or(-1.0f);
                                break;
                            }
                        }

                        if (ov > 0.0f)
                            _mipmapUpscalerRatio = ov;
                        else
                            _mipmapUpscalerRatio = fr[n];

                        _renderWidth = static_cast<uint32_t>(_displayWidth / _mipmapUpscalerRatio);
                        _mipBiasCalculated = log2((float) _renderWidth / (float) _displayWidth);
                    }
                }

                ImGui::EndCombo();
            }

            ImGui::EndDisabled();

            auto minLimit = config->ExtendedLimits.value_or_default() ? 0.1f : 1.0f;
            auto maxLimit = config->ExtendedLimits.value_or_default() ? 6.0f : 3.0f;
            if (ImGui::SliderFloat("Upscaler Ratio", &_mipmapUpscalerRatio, minLimit, maxLimit, "%.2f"))
            {
                _renderWidth = static_cast<uint32_t>(_displayWidth / _mipmapUpscalerRatio);
                _mipBiasCalculated = log2((float) _renderWidth / (float) _displayWidth);
            }

            if (ImGui::InputScalar("Render Width", ImGuiDataType_U32, &_renderWidth, NULL, NULL, "%u"))
                _mipBiasCalculated = log2((float) _renderWidth / (float) _displayWidth);

            ImGui::SliderFloat("Mipmap Bias", &_mipBiasCalculated, -15.0f, 0.0f, "%.6f");

            // BOTTOM LINE
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::SameLine();
            ImGui::Spacing();

            constexpr float spacing = 6.0f;
            auto textSize = ImGui::CalcTextSize("Use Value");
            textSize += ImGui::CalcTextSize("Close");
            textSize.x += ImGui::GetStyle().FramePadding.x * 5.0f + spacing; // 2 sides * 2 buttons + 1

            float avail = ImGui::GetContentRegionAvail().x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - textSize.x);

            if (ImGui::Button("Use Value"))
            {
                _mipBias = _mipBiasCalculated;
                _showMipmapCalcWindow = false;
            }

            ImGui::SameLine(0.0f, spacing);

            if (ImGui::Button("Close"))
                _showMipmapCalcWindow = false;

            ImGui::Spacing();
            ImGui::Separator();

            ImGui::End();
        }
    }
}

void MenuCommon::RenderHudlessResourcesWindow(RenderMenuContext& ctx, ImGuiWindowFlags flags)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    auto& io = ctx.io;

    auto fg = state.CurrentFG();
    if (_showHudlessWindow && config->FGHUDFix.value_or_default() && fg != nullptr && fg->IsActive())
    {
        auto posX = (io.DisplaySize.x - 400.0f) / 2.0f;
        auto posY = (io.DisplaySize.y - 300.0f) / 2.0f;

        ImGui::SetNextWindowPos(ImVec2 { posX, posY }, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2 { 400.0f, 300.0f });

        if (ImGui::Begin("HUDless Resources", nullptr, flags))
        {
            if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow))
                ImGui::SetWindowFocus();

            int btnCount = 100;

            if (ImGui::BeginTable("HUDlessTable", 2, ImGuiTableFlags_SizingFixedFit))
            {
                ImGui::TableSetupColumn("##1", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("##2", ImGuiTableColumnFlags_WidthFixed);

                ankerl::unordered_dense::map<void*, CapturedHudlessInfo>::iterator it;

                for (it = state.capturedHudlesses.begin(); it != state.capturedHudlesses.end(); it++)
                {
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);

                    ImGui::Text("%08x, %s->%s, Count: %llu, %s", (size_t) it->first,
                                GetSourceString(it->second.captureInfo & 0xFF).c_str(),
                                GetDispatchString(it->second.captureInfo & 0xFF00).c_str(), it->second.usageCount,
                                it->second.enabled ? "Active" : "Passive");

                    ImGui::TableSetColumnIndex(1);

                    btnCount++;
                    std::string text;

                    if (it->second.enabled)
                        text = StrFmt("Disable##%d", btnCount);
                    else
                        text = StrFmt("Enable##%d", btnCount);

                    if (ImGui::Button(text.c_str()))
                    {
                        LOG_DEBUG("HUDless {:X}: {}", (size_t) it->first,
                                  it->second.enabled ? "Disabling" : "Enabling");
                        it->second.enabled = !it->second.enabled;
                    }
                }

                ImGui::EndTable();
            }

            if (ImGui::Button("Clear##4"))
            {
                LOG_DEBUG("Clearing captured HUDless resources");
                state.clearCapturedHudlesses = true;
            }

            ImGui::SameLine(0.0f, 8.0f);

            if (ImGui::Button("Close##4"))
                _showHudlessWindow = false;

            ImGui::End();
        }
    }
}

void MenuCommon::RenderMainMenuWindow(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    auto& frameTime = ctx.frameTime;
    auto& frameRate = ctx.frameRate;
    auto& frameTimesCalculated = ctx.frameTimesCalculated;
    auto& menuResScale = ctx.menuResScale;

    if (!_isVisible)
        return;

    // Check for GPU support once and reuse the result in all menu sections.
    // DXVK might call Vulkan device creation, which would destroy our objects.
    {
        ScopedSkipVulkanHooks skipVulkanHooks {};
        ctx.primaryGpu =
            std::make_unique<std::decay_t<decltype(IdentifyGpu::getPrimaryGpu())>>(IdentifyGpu::getPrimaryGpu());
    }

    // Overlay font
    if (config->UseHQFont.value_or_default())
        ImGui::PushFontSize(std::round(menuResScale * fontSize));

    // If overlay is not visible frame needs to be inited
    if (!frameTimesCalculated)
    {
        float frameCnt = 0;
        frameTime = 0;
        for (size_t i = 299; i > 199; i--)
        {
            if (state.frameTimes[i] > 0.0)
            {
                frameTime += state.frameTimes[i];
                frameCnt++;
            }
        }

        frameTime /= frameCnt;
        frameRate = 1000.0 / frameTime;
    }

    // The detached windows below keep auto-resizing; the main window is resizable and scrolls inside its page pane.
    ImGuiWindowFlags flags = 0;
    flags |= ImGuiWindowFlags_NoSavedSettings;
    flags |= ImGuiWindowFlags_NoCollapse;
    flags |= ImGuiWindowFlags_AlwaysAutoResize;

    ImGuiWindowFlags mainFlags = (flags & ~ImGuiWindowFlags_AlwaysAutoResize) | ImGuiWindowFlags_NoScrollbar |
                                 ImGuiWindowFlags_NoScrollWithMouse;

    // Last size of the main window in pixels; zero until it has been drawn once.
    static ImVec2 lastWindowSize {};

    ImVec2 wantedSize { config->MenuWidth.value_or(960.0f) * menuResScale,
                        config->MenuHeight.value_or(680.0f) * menuResScale };
    // The window can't scroll, so every size it takes (first use, rescale, drag, minimum) stays on screen.
    const ImVec2 maxSize { ctx.io.DisplaySize.x > 0.0f ? ctx.io.DisplaySize.x * 0.95f : FLT_MAX,
                           ctx.io.DisplaySize.y > 0.0f ? ctx.io.DisplaySize.y * 0.95f : FLT_MAX };
    const ImVec2 minSize { std::min(640.0f * menuResScale, maxSize.x), std::min(420.0f * menuResScale, maxSize.y) };
    wantedSize.x = std::clamp(wantedSize.x, minSize.x, maxSize.x);
    wantedSize.y = std::clamp(wantedSize.y, minSize.y, maxSize.y);
    ImGui::SetNextWindowSizeConstraints(minSize, maxSize);
    ImGui::SetNextWindowSize(wantedSize, ImGuiCond_FirstUseEver);

    if (lastWindowSize.x <= 0.0f)
        lastWindowSize = wantedSize;

    if (lastMenuScale != menuResScale)
    {
        const float oldMenuScale = lastMenuScale;
        lastMenuScale = menuResScale;

        // if UI scale is changed rescale the style
        RebuildStyle(menuResScale);

        // Keep the window the same size relative to the text.
        if (oldMenuScale > 0.0f)
        {
            lastWindowSize = { std::clamp(lastWindowSize.x * menuResScale / oldMenuScale, minSize.x, maxSize.x),
                               std::clamp(lastWindowSize.y * menuResScale / oldMenuScale, minSize.y, maxSize.y) };
            ImGui::SetNextWindowSize(lastWindowSize, ImGuiCond_Always);
        }
    }

    // Main menu window
    if (windowTitle.empty())
    {
        windowTitle = StrFmt("%s - %s %s %s %s", VER_PRODUCT_NAME, state.gameExe.c_str(),
                             state.gameName.empty() ? "" : StrFmt("- %s", state.gameName.c_str()).c_str(),
                             (state.detectedQuirks.size() > 0) ? "(Q)" : "", state.isOptiPatcherSucceed ? "(OP)" : "");
    }

    if (ImGui::Begin(windowTitle.c_str(), NULL, mainFlags))
    {
        // Remember the size (relative to Menu Scale) for Save Settings.
        const ImVec2 windowSize = ImGui::GetWindowSize();
        if (std::fabs(windowSize.x - lastWindowSize.x) > 0.5f || std::fabs(windowSize.y - lastWindowSize.y) > 0.5f)
        {
            lastWindowSize = windowSize;
            config->MenuWidth = windowSize.x / menuResScale;
            config->MenuHeight = windowSize.y / menuResScale;
        }

        // Header/status messages shown above the navigation and page panes.
        RenderMainMenuHeaderMessages(ctx);

        // Navigation pane and the selected page.
        RenderMainMenuPages(ctx);

        // Diagnostics and footer actions below the page, full width.
        const float footerTop = ImGui::GetCursorPosY();
        RenderMainMenuGraphs(ctx);
        RenderMainMenuBottomBar(ctx);
        mainMenuFooterHeight = ImGui::GetCursorPosY() - footerTop;

        ImGui::End();
    }

    // Detached utility windows owned by the main menu.
    RenderMipmapBiasWindow(ctx, flags);
    RenderHudlessResourcesWindow(ctx, flags);

    if (config->UseHQFont.value_or_default())
        ImGui::PopFontSize();
}

void KeyUp(UINT vKey)
{
    inputMenu = vKey == Config::Instance()->ShortcutKey.value_or_default();
    inputFps = vKey == Config::Instance()->FpsShortcutKey.value_or_default();
    inputFG = vKey == Config::Instance()->FGShortcutKey.value_or_default();
    inputFpsCycle = vKey == Config::Instance()->FpsCycleShortcutKey.value_or_default();
}

// The lamp, and only the lamp.
//
// Red for dark, green for full light, with its reading beside it. No status sentence: the whole
// point of a light meter is that it is read at a glance while playing, and a paragraph in the corner
// of somebody's game is not that. Everything wordy lives in the menu, which is where someone has
// already decided to stop and read.
//
// Drawn only when its own setting is on. An overlay that appears because a scan happens to be
// running is an overlay nobody asked for.
void RenderExposureScanIndicator(float alpha)
{
    using DlssNr::ExposureScan::Verdict;

    if (!Config::Instance()->DlssNrScanMeter.value_or_default())
        return;

    if (DlssNr::ExposureScan::Where() == Verdict::Off)
        return;

    int which = 0;
    float low = 0.0f, high = 0.0f;
    const float now = DlssNr::ExposureScan::BestValue(&which, &low, &high);

    // Nothing found yet, or no range to place it in: a dim lamp, which says "watching, no reading"
    // without saying it in words.
    const bool reading = now > 0.0f && high > low;

    float lit = 0.0f;

    if (reading)
    {
        // An exposure falls as the scene brightens, so the value reads backwards unless the buffer
        // holds the reciprocal -- the same question the anchor asks, answered from the same setting,
        // because a lamp contradicting the picture would be worse than no lamp.
        lit = (high - now) / (high - low);

        if (Config::Instance()->DlssNrScanInverted.value_or_default())
            lit = 1.0f - lit;

        lit = lit < 0.0f ? 0.0f : (lit > 1.0f ? 1.0f : lit);
    }

    // Red to amber to green. A straight red-to-green fade passes through a muddy brown at the
    // midpoint, and the midpoint is where most of a session is spent.
    const ImVec4 dark(0.90f, 0.22f, 0.20f, 1.0f);
    const ImVec4 mid(0.95f, 0.75f, 0.20f, 1.0f);
    const ImVec4 bright(0.35f, 0.88f, 0.38f, 1.0f);
    const ImVec4 idle(0.45f, 0.45f, 0.45f, 1.0f);

    ImVec4 lamp = idle;

    if (reading)
    {
        const float t = lit < 0.5f ? lit * 2.0f : (lit - 0.5f) * 2.0f;
        const ImVec4& a = lit < 0.5f ? dark : mid;
        const ImVec4& b = lit < 0.5f ? mid : bright;
        lamp = ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1.0f);
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 12.0f, vp->WorkPos.y + 12.0f),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(alpha);

    if (ImGui::Begin("DlssNrExposureScan", nullptr,
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDecoration |
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove))
    {
        const float r = ImGui::GetFontSize() * 0.38f;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const ImVec2 centre(at.x + r, at.y + ImGui::GetTextLineHeight() * 0.5f);

        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddCircleFilled(centre, r, ImGui::GetColorU32(lamp), 20);
        draw->AddCircle(centre, r, ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.6f)), 20, 1.5f);

        ImGui::Dummy(ImVec2(r * 2.0f + 6.0f, ImGui::GetTextLineHeight()));
        ImGui::SameLine();

        ScopedMonoFont monoFont {};

        if (reading)
            ImGui::TextColored(lamp, "%3.0f%%  %.5f", lit * 100.0f, now);
        else
            ImGui::TextColored(idle, "--");
    }

    ImGui::End();
}

bool MenuCommon::RenderMenu()
{
    if (!_isInited)
        return false;

    RenderMenuContext ctx { State::Instance(), Config::Instance(), ImGui::GetIO() };
    ctx.now = Util::MillisecondsNow();
    ctx.currentFeature = ctx.state.currentFeature;

    // 1) Collect timing and input state before any ImGui drawing.
    UpdateRenderTiming(ctx);
    UpdateMenuInputMode(ctx);
    HandleMenuShortcuts(ctx);

    // 2) Prepare one-shot notifications and start a new ImGui frame only when needed.
    UpdateVersionAndStartupNotifications(ctx);
    BeginMenuFrameIfNeeded(ctx);
    OptiInput::EndFrame(_isVisible);

    // 3) Draw lightweight overlay windows first, preserving the original order.
    ctx.menuResScale = MenuResolutionScale(ctx.io);
    RenderSplashWindow(ctx);
    RenderNotifications(ctx);
    UpdateFrameTimeAverages(ctx);
    RenderPerformanceOverlay(ctx);
    RenderExposureScanIndicator(ctx.config->FpsOverlayAlpha.value_or_default());

    // 4) Draw the full settings menu last so popups and child windows keep their existing behavior.
    RenderMainMenuWindow(ctx);

    // The Optical F5Low debug view's enlarged or full screen picture, over the game whether the menu shows or not.
    if (ctx.newFrame)
    {
        NativeMotionDx12::DrawExpandedPreview();
        NativeMotionDx11::DrawExpandedPreview();
        NativeMotionVk::DrawExpandedPreview();
    }

    if (ctx.newFrame)
        ImGui::EndFrame();

    return ctx.newFrame;
}

void MenuCommon::Init(HWND InHwnd, bool isUWP)
{
    // Reset shutdown flag in case of re-init
    State::Instance().isShuttingDown = false;

    HWND oldHandle = nullptr;

    if (_handle != nullptr)
    {
        oldHandle = _handle;
        LOG_DEBUG("Old Handle: {:X}, ImGui Handle: {:X}", (size_t) oldHandle,
                  (size_t) ImGui::GetMainViewport()->PlatformHandleRaw);
    }

    _handle = InHwnd;
    _isVisible = false;
    _isUWP = isUWP;
    lastPosition = { -1000.0f, -1000.0f };

    LOG_DEBUG("Handle: {0:X}", (size_t) _handle);

    // In case d3d12 wasn't yet used up to this point, try to update GPU info late here
    IdentifyGpu::updateD3d12Capabilities();

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGuiIO& io = ImGui::GetIO();
    (void) io;

    hasGamepad = (io.BackendFlags | ImGuiBackendFlags_HasGamepad) > 0;
    io.BackendFlags &= 30;
    io.ConfigFlags = ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NoKeyboard;

    io.MouseDrawCursor = _isVisible;
    io.WantCaptureKeyboard = _isVisible;
    io.WantCaptureMouse = _isVisible;
    io.WantSetMousePos = _isVisible;

    io.IniFilename = io.LogFilename = nullptr;

    bool initResult = false;

    if (io.BackendPlatformUserData == nullptr)
    {
        if (!isUWP)
        {
            initResult = ImGui_ImplWin32_Init(InHwnd);
            LOG_DEBUG("ImGui_ImplWin32_Init result: {0}", initResult);
        }
        else
        {
            initResult = ImGui_ImplUwp_Init(InHwnd);
            ImGui_BindUwpKeyUp(KeyUp);
            LOG_DEBUG("ImGui_ImplUwp_Init result: {0}", initResult);
        }
    }

    if (io.Fonts->Fonts.empty())
    {
        gHackFont = gInterFont = gClassicFont = nullptr;

        if (Config::Instance()->UseHQFont.value_or_default())
        {
            ImFontAtlas* atlas = io.Fonts;
            atlas->Clear();

            // This automatically becomes the next default font
            ImFontConfig fontConfig;

            if (Config::Instance()->FontSize.has_value())
                fontSize = Config::Instance()->FontSize.value();

            if (Config::Instance()->TTFFontPath.has_value())
            {
                gClassicFont =
                    atlas->AddFontFromFileTTF(wstring_to_string(Config::Instance()->TTFFontPath.value()).c_str(),
                                              fontSize, &fontConfig, io.Fonts->GetGlyphRangesDefault());
            }
            else
            {
                gHackFont = atlas->AddFontFromMemoryCompressedBase85TTF(hack_compressed_compressed_data_base85,
                                                                        fontSize, &fontConfig);
                gClassicFont = gHackFont;

                // Modern style text; Hack stays loaded for the live numbers.
                gInterFont = atlas->AddFontFromMemoryCompressedBase85TTF(inter_compressed_compressed_data_base85,
                                                                         fontSize, &fontConfig);
            }

            io.FontDefault = gClassicFont;
        }
    }

    if (!Config::Instance()->OverlayMenu.value_or_default())
    {
        _hdrTonemapApplied = false;
    }

    DWORD hwndPid = 0;
    DWORD hwndTid = GetWindowThreadProcessId(_handle, &hwndPid);

    LOG_DEBUG("HWND: {:X}, IsWindow: {}, HWND PID: {}, Current PID: {}, HWND TID: {}, Current TID: {}",
              (ULONG64) _handle, IsWindow(_handle), hwndPid, GetCurrentProcessId(), hwndTid, GetCurrentThreadId());

    OptiInput::Initialize(_handle, isUWP);

    RebuildStyle();
    ApplyThemeFont();
    _isInited = true;
}

void MenuCommon::Shutdown()
{
    if (!MenuCommon::_isInited)
        return;

    // if (_oWndProc != nullptr)
    //{
    //     auto handle = (HWND) ImGui::GetMainViewport()->PlatformHandleRaw;
    //     SetLastError(0);
    //     auto restoreResult = SetWindowLongPtr(handle, GWLP_WNDPROC, (LONG_PTR) _oWndProc);
    //     auto error = GetLastError();

    //    if (restoreResult == 0 && error != 0)
    //    {
    //        LOG_ERROR("Failed to restore old WndProc. Error: {:X}", error);
    //    }

    //    _oWndProc = nullptr;
    //}

    if (!_isUWP)
        ImGui_ImplWin32_Shutdown();
    else
        ImGui_ImplUwp_Shutdown();

    ImGui::DestroyContext();

    _handle = nullptr;
    _isInited = false;
    _isVisible = false;
}

void MenuCommon::HideMenu()
{
    if (!_isVisible)
        return;

    _isVisible = false;

    ImGuiIO& io = ImGui::GetIO();
    (void) io;

    _showMipmapCalcWindow = false;
    _showHudlessWindow = false;

    io.MouseDrawCursor = _isVisible;
    io.WantCaptureKeyboard = _isVisible;
    io.WantCaptureMouse = _isVisible;
}
