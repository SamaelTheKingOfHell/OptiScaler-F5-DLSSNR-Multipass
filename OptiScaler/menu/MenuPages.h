#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

// Pages of the main menu's navigation pane. Pure data, no ImGui, so it can be unit tested on the host.
namespace MenuPages
{
// Neural Rendering is a nav group, not a page: clicking it opens NrStatus. Optical F5Low is listed first in the group;
// saved page names are by name, so an older MenuPage value (nr.status ...) still opens the same page.
enum class Page : unsigned char
{
    Upscaler,
    FrameGeneration,
    Image,
    Framerate,
    OverlayLook,
    Input,
    Misc,
    Plugins, // after Misc, before the Neural Rendering group (enum order is not the nav order; see menu_common.cpp)
    Notifications, // every toast shown so far; right after Plugins
    F5Lut,   // the LUT stack (after NR, or on the upscaler's output); shown right below the Neural Rendering group
    NrF5Low, // first under Neural Rendering: NR for a game that makes no upscaler call
    NrStatus,
    NrOptions,
    NrInput,
    NrOutput,
    NrPasses,
    NrDebug,
};

struct PageInfo
{
    const char* name;  // stable ini name, stored in [Menu] MenuPage
    const char* label; // shown in the nav pane
};

// Same order as Page.
inline constexpr PageInfo kPages[] = {
    { "upscaler", "Upscaler" },
    { "framegen", "Frame Generation" },
    { "image", "Image" },
    { "framerate", "Framerate" },
    { "overlay", "Overlay & Look" },
    { "input", "Input" },
    { "misc", "Misc" },
    { "plugins", "Plugins" },
    { "notifications", "Notifications" },
    { "f5lut", "F5LUT" },
    { "nr.f5low", "Optical F5Low" },
    { "nr.status", "Status & Presets" },
    { "nr.options", "NR Options" },
    { "nr.input", "NR Input" },
    { "nr.output", "NR Output" },
    { "nr.passes", "Passes & Colour" },
    { "nr.debug", "Compare & Debug" },
};

inline constexpr size_t kPageCount = sizeof(kPages) / sizeof(kPages[0]);

inline constexpr bool IsNeuralRendering(Page page) { return page >= Page::NrF5Low; }

inline constexpr const PageInfo& Info(Page page)
{
    const size_t index = static_cast<size_t>(page);
    return kPages[index < kPageCount ? index : 0];
}

inline constexpr const char* Name(Page page) { return Info(page).name; }
inline constexpr const char* Label(Page page) { return Info(page).label; }

// Missing or unknown names fall back to Upscaler.
inline constexpr Page PageFromName(std::string_view name)
{
    for (size_t i = 0; i < kPageCount; ++i)
    {
        if (name == kPages[i].name)
            return static_cast<Page>(i);
    }

    return Page::Upscaler;
}

// A page another part of the menu wants shown (the header's Optical F5Low button, cross-links). The page choice itself
// is a static inside MenuCommon::RenderMainMenuPages, so the request waits here until that function takes it, once, on
// its next draw.
inline std::optional<Page>& PendingRequest()
{
    static std::optional<Page> request;
    return request;
}

inline void RequestPage(Page page) { PendingRequest() = page; }

// The requested page, once: a second call returns nothing until the next RequestPage.
inline std::optional<Page> ConsumeRequest()
{
    const std::optional<Page> request = PendingRequest();
    PendingRequest().reset();
    return request;
}
} // namespace MenuPages
