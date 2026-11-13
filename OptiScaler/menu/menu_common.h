#pragma once

#include "SysUtils.h"
#include <Config.h>

#include <imgui/imgui.h>

class IFGFeature;

class ScopedIndent
{
  public:
    explicit ScopedIndent(float indent = 16.0f) : m_indent(indent) { ImGui::Indent(m_indent); }

    ~ScopedIndent() { ImGui::Unindent(m_indent); }

  private:
    float m_indent;
};

// Modern style state, defined in menu_common.cpp.
namespace MenuStyle
{
bool IsModern();
const ImVec4& CardColor();
const ImVec4& CardHeaderColor();
const ImVec4& CardHeaderHoverColor();
const ImVec4& CardHeaderActiveColor();
ImFont* MonoFont(); // Hack; null with a custom TTF or HQ font off
} // namespace MenuStyle

// Live numbers keep the fixed-width font so digits don't jitter when the default is proportional.
class ScopedMonoFont
{
  public:
    ScopedMonoFont()
    {
        ImFont* mono = MenuStyle::MonoFont();
        _pushed = mono != nullptr && ImGui::GetFont() != mono;
        if (_pushed)
            ImGui::PushFont(mono, -1.0f);
    }

    ~ScopedMonoFont()
    {
        if (_pushed)
            ImGui::PopFont();
    }

  private:
    bool _pushed = false;
};

class ScopedCollapsingHeader
{
  public:
    explicit ScopedCollapsingHeader(const char* label, ImGuiTreeNodeFlags flags = 0)
    {
        // The page view asks for the next header to start open (one shot).
        if (OpenNextHeader)
        {
            flags |= ImGuiTreeNodeFlags_DefaultOpen;
            OpenNextHeader = false;
        }

        ImGui::PushID(label);

        // A top-level section is a card in both themes: a 1 px border in the theme's border colour and inner padding,
        // so its controls do not sit against the next section. Modern also gives it its card colours (and sets
        // ChildBorderSize 0 for other children); a header nested in a card stays flat.
        const bool modern = MenuStyle::IsModern();
        const bool card = _depth == 0;
        ImGuiChildFlags childFlags = ImGuiChildFlags_AutoResizeY;

        if (card)
        {
            childFlags |= ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders;
            ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
        }

        if (modern)
            ImGui::PushStyleColor(ImGuiCol_ChildBg, card ? MenuStyle::CardColor() : ImVec4(0.0f, 0.0f, 0.0f, 0.0f));

        ImGui::BeginChild("##CollapsingHeaderChild", ImVec2(0, 0), childFlags,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        if (modern)
            ImGui::PopStyleColor();
        if (card)
            ImGui::PopStyleVar();

        const bool cardColours = card && modern;
        if (cardColours)
        {
            ImGui::PushStyleColor(ImGuiCol_Header, MenuStyle::CardHeaderColor());
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, MenuStyle::CardHeaderHoverColor());
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, MenuStyle::CardHeaderActiveColor());
        }

        _headerOpen = ImGui::CollapsingHeader(label, flags);

        if (cardColours)
            ImGui::PopStyleColor(3);

        _depth++;
        _active = true;
    }

    bool IsHeaderOpen() const { return _headerOpen; }

    inline static bool OpenNextHeader = false;

    ~ScopedCollapsingHeader()
    {
        if (_active)
        {
            _depth--;
            ImGui::EndChild();
            ImGui::PopID();
        }
    }

  private:
    inline static int _depth = 0;
    bool _active = false;
    bool _headerOpen = false;
};

template <typename T> struct MenuOption
{
    T value;
    std::string label;
    std::string tooltip;
    bool disabled = false;
    bool hidden = false;

    MenuOption& set_disabled(bool condition, const std::string& reason = "")
    {
        if (condition)
        {
            disabled = true;
            if (!reason.empty())
                tooltip = reason;
        }
        return *this;
    }

    MenuOption& set_hidden(bool condition)
    {
        if (condition)
        {
            hidden = true;
        }
        return *this;
    }
};

class MenuCommon
{
  private:
    // internal values
    inline static HWND _handle = nullptr;
    // inline static WNDPROC _oWndProc = nullptr;
    inline static bool _isVisible = false;
    inline static bool _isInited = false;
    inline static bool _isUWP = false;

    // mipmap calculations
    inline static bool _showMipmapCalcWindow = false;
    inline static bool _showHudlessWindow = false;
    inline static float _mipBias = 0.0f;
    inline static float _mipBiasCalculated = 0.0f;
    inline static uint32_t _mipmapUpscalerQuality = 0;
    inline static float _mipmapUpscalerRatio = 0;
    inline static uint32_t _displayWidth = 0;
    inline static uint32_t _renderWidth = 0;

    inline static UINT64 _frameCount = 0;

    // reflex
    inline static float _limitFps = std::numeric_limits<float>::infinity();

    // ffx
    inline static int _ffxUpscalerIndex = -1;
    inline static int _ffxFGIndex = -1;

    // output scaling
    inline static float _ssRatio = 0.0f;
    inline static bool _ssEnabled = false;
    inline static Scaler _ssDownsampler = Scaler::FSR1;

    // ui scale
    inline static int _selectedScale = 0;

    // overlay states
    inline static bool _dx11Ready = false;
    inline static bool _dx12Ready = false;
    inline static bool _vulkanReady = false;

    inline static void ShowTooltip(const char* tip);

    inline static void ShowHelpMarker(const char* tip);
    inline static void ShowResetButton(CustomOptional<bool, NoDefault>* initFlag, std::string buttonName);
    inline static void ReInitUpscaler();

    inline static void SeparatorWithHelpMarker(const char* label, const char* tip);

    static Upscaler GetBackendCode(const API api);
    static void GetCurrentBackendInfo(const API api, Upscaler& upscaler, std::string* name);
    static void RenderUpscalerCombo(const API api, Upscaler currentUpscaler, const std::vector<Upscaler>& options);
    static void AddDx11Backends(Upscaler upscaler);
    static void AddDx12Backends(Upscaler upscaler);
    static void AddVulkanBackends(Upscaler upscaler);
    template <HasDefaultValue B> static void AddResourceBarrier(std::string name, CustomOptional<int32_t, B>* value);
    template <HasDefaultValue B> static void AddDLSSRenderPreset(std::string name, CustomOptional<uint32_t, B>* value);
    template <HasDefaultValue B> static void AddDLSSDRenderPreset(std::string name, CustomOptional<uint32_t, B>* value);
    template <typename TStorage, typename T>
    static void PopulateCombo(const std::string& name, TStorage& currentValue,
                              const std::vector<MenuOption<T>>& options);

    struct RenderMenuContext;

    // What keeps XeFG / F5Low from running, worked out from the current state without drawing: the XeFG card acts on
    // it (turns frame generation off, greys controls) and the page's status line names it.
    struct XeFGRunChecks
    {
        bool ignoreChecks = false;
        bool correctMVs = true;
        bool restartNeeded = false;
        bool exclusiveFullscreen = false; // turns frame generation off even with the checks ignored
        bool borderlessMissing = false;
        bool hdrUnsupported = false;

        bool CantActivate() const { return borderlessMissing || hdrUnsupported; }
        // Why frame generation cannot run now, for the status line; null when it can
        const char* BlockedReason() const;
    };
    static XeFGRunChecks CheckXeFGRun(RenderMenuContext& ctx, IFGFeature* fg);

    // RenderMenu orchestration helpers. These keep the public RenderMenu() flow short
    // while preserving the original ImGui layout and draw order.
    static void UpdateRenderTiming(RenderMenuContext& ctx);
    static void UpdateMenuInputMode(RenderMenuContext& ctx);
    static void HandleMenuShortcuts(RenderMenuContext& ctx);
    static void UpdateVersionAndStartupNotifications(RenderMenuContext& ctx);
    static void BeginMenuFrameIfNeeded(RenderMenuContext& ctx);
    static void RenderSplashWindow(RenderMenuContext& ctx);
    static void RenderNotifications(RenderMenuContext& ctx);
    static void UpdateFrameTimeAverages(RenderMenuContext& ctx);
    static void RenderPerformanceOverlay(RenderMenuContext& ctx);

    // Labels for the Neural Rendering comparison views. Drawn every frame, into the frame plane,
    // so a screenshot keeps them and the wipe reveals and hides them like the images.
    static void RenderNrCompareTags();
    static void RenderMainMenuWindow(RenderMenuContext& ctx);

    // RenderMainMenuWindow section helpers. These keep the main window flow readable
    // without changing the existing ImGui layout, labels, or setting side effects.
    static void RenderMainMenuHeaderMessages(RenderMenuContext& ctx);
    static void RenderMainMenuPages(RenderMenuContext& ctx);
    static void RenderActiveUpscalerSettings(RenderMenuContext& ctx);
    static void RenderFrameGenerationSelection(RenderMenuContext& ctx);
    static void RenderFrameGenerationRuntimeSettings(RenderMenuContext& ctx);
    static void RenderFrameGenerationFooter(RenderMenuContext& ctx);
    static void RenderFgStatusLine(RenderMenuContext& ctx);
    static void RenderXeFGDynamicMFG(RenderMenuContext& ctx, int maxInterpolationCount, bool followsGame);
    static void RenderFGFollowGame();
    static void RenderFsrCommonSettings(RenderMenuContext& ctx);
    static void RenderFramerateLowLatency(RenderMenuContext& ctx);
    static void RenderFramerateSettings(RenderMenuContext& ctx);
    static void RenderFakenvapiSettings(RenderMenuContext& ctx);
    static void RenderActiveImageSettings(RenderMenuContext& ctx);
    static void RenderF5LutSettings(RenderMenuContext& ctx);
    static void RenderHdrOutputSettings(RenderMenuContext& ctx);
    static void RenderMagnifierSettings(RenderMenuContext& ctx);
    static void RenderQuirksSettings(RenderMenuContext& ctx);
    static void RenderAdvancedSettings(RenderMenuContext& ctx);
    static void RenderLoggingSettings(RenderMenuContext& ctx);
    static void RenderThemeSettings(RenderMenuContext& ctx);
    static void RenderFpsOverlaySettings(RenderMenuContext& ctx);
    static void RenderUpscalerInputsSettings(RenderMenuContext& ctx);
    static void RenderApiAndTextureSettings(RenderMenuContext& ctx);
    static void RenderKeybindSettings(RenderMenuContext& ctx);
    static void RenderMainMenuGraphs(RenderMenuContext& ctx);
    static void RenderMainMenuBottomBar(RenderMenuContext& ctx);
    static void RenderMipmapBiasWindow(RenderMenuContext& ctx, ImGuiWindowFlags flags);
    static void RenderHudlessResourcesWindow(RenderMenuContext& ctx, ImGuiWindowFlags flags);

    static void UpdateManualInput(HWND targetHwnd);

  public:
    static void Dx11Inited() { _dx11Ready = true; }
    static void Dx12Inited() { _dx12Ready = true; }
    static void VulkanInited() { _vulkanReady = true; }
    static bool IsInited() { return _isInited; }
    static bool IsVisible() { return _isVisible; }
    static HWND Handle() { return _handle; }

    static bool RenderMenu();
    static void Init(HWND InHwnd, bool isUWP);
    static void Shutdown();
    static void HideMenu();
    static void Present();

    // Vulkan FG outputs present the generated frames on their own, the game's presents only show the real ones.
    // FGOutputPresent is called for each frame they present, FGOutputPresents says if the game's frames go through
    // them.
    static void FGOutputPresent();
    static void FGOutputPresents(bool presents);
    static void ApplyThemeStyle();
    static void RebuildStyle(float menuScale = 0.0f);
    static void ApplyThemeFont();
};
