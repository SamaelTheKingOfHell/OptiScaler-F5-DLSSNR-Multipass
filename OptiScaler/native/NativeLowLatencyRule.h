#pragma once

// When Optical F5Low's own low latency (native/NativeLowLatency.cpp) runs, and what the menu says when it does not. No
// D3D, no ImGui, no Config: a host test can include it.

#include <cstdint>

namespace native::lowlatency
{
// [DlssNr] NativeLowLatency. Auto: with an Optical F5Low mode running, XeFG (its XeLL needs someone to feed it), or FSR
// frame generation generating frames (it adds latency, and a game without Reflex has nothing against it). On: also
// without any of them, in any game that makes no Reflex call of its own. Off: never.
enum class Setting : uint32_t
{
    Auto,
    On,
    Off
};

// [Framerate] LowLatencyInput, the Framerate page's "Input": who makes the latency calls. Auto: the game's own Reflex
// when it makes the calls, else OptiScaler's where Setting::Auto runs us. Game: only the game's own Reflex, OptiScaler
// makes none. OptiScaler: Setting::On. Off: no low latency at all: none of ours, and the game's own Reflex is sent with
// low latency off (BlocksGameLowLatency).
enum class Input : uint32_t
{
    Auto,
    Game,
    OptiScaler,
    Off
};

struct InputRule
{
    Setting setting = Setting::Auto;   // what Decide runs from
    bool blockGameLowLatency = false;  // the game's Reflex gets low latency off (before the DLSS FG exception)
};

inline InputRule RuleForInput(Input input)
{
    switch (input)
    {
    case Input::Game:
        return { Setting::Off, false };
    case Input::OptiScaler:
        return { Setting::On, false };
    case Input::Off:
        return { Setting::Off, true };
    default:
        return { Setting::Auto, false };
    }
}

// Auto is Game while Optical F5Low's virtual upscaler (the NR + frame generation mode) runs: only the game's own Reflex,
// OptiScaler makes no latency calls next to it, even for XeFG or FSR FG (which then lower nothing in a game without
// Reflex). OptiScaler, picked by hand, still starts ours. Standing aside for a game upscaler, Auto is Auto again.
inline Input EffectiveInput(Input input, bool virtualUpscaler)
{
    return input == Input::Auto && virtualUpscaler ? Input::Game : input;
}

// DLSS frame generation cannot run without Reflex low latency on: Off leaves the game's Reflex alone then
inline bool BlocksGameLowLatency(Input input, bool frameGenerationNeedsReflex)
{
    return RuleForInput(input).blockGameLowLatency && !frameGenerationNeedsReflex;
}

// Equal text; null is nothing
inline bool SameWord(const char* text, const char* word)
{
    if (text == nullptr)
        return false;

    while (*text != '\0' && *text == *word)
        ++text, ++word;

    return *text == '\0' && *word == '\0';
}

inline const char* InputIniText(Input input)
{
    switch (input)
    {
    case Input::Game:
        return "game";
    case Input::OptiScaler:
        return "optiscaler";
    case Input::Off:
        return "off";
    default:
        return "auto";
    }
}

// `text` is lower case (Config::readString lowers it). False for anything else, `out` is then left alone.
inline bool InputFromIniText(const char* text, Input* out)
{
    constexpr Input all[] = { Input::Auto, Input::Game, Input::OptiScaler, Input::Off };

    for (const Input input : all)
    {
        if (SameWord(text, InputIniText(input)))
        {
            *out = input;
            return true;
        }
    }

    return false;
}

// The key from before the Framerate page's Input ([DlssNr] NativeLowLatency): auto, true (OptiScaler) or false (Game)
inline bool InputFromLegacyText(const char* text, Input* out)
{
    if (SameWord(text, "auto"))
        *out = Input::Auto;
    else if (SameWord(text, "true"))
        *out = Input::OptiScaler;
    else if (SameWord(text, "false"))
        *out = Input::Game;
    else
        return false;

    return true;
}

// What the old key is written as, so an older build reading a newer ini still does what the player chose: it has no
// Off, so Off and Game both read as false (it makes no calls of its own either way)
inline const char* LegacySettingText(Input input)
{
    switch (input)
    {
    case Input::OptiScaler:
        return "true";
    case Input::Game:
    case Input::Off:
        return "false";
    default:
        return "auto";
    }
}

// The new key if it holds a word we know (an explicit "auto" is a choice, whatever the old key says), else the old one,
// else Auto. `inputKeyPresent`: the key is in the ini at all. Both texts may be null (not in the ini).
inline Input InputFromIniValues(bool inputKeyPresent, const char* inputText, const char* legacyText)
{
    Input input = Input::Auto;

    if (inputKeyPresent && InputFromIniText(inputText, &input))
        return input;

    if (InputFromLegacyText(legacyText, &input))
        return input;

    return Input::Auto;
}

// [XeFG] LowLatencyOutput, what lowers latency while XeFG is the frame generation output. XeLL: XeFG's XeLL does it
// (default, the only thing there was). Reflex or Anti-Lag 2: the driver's Reflex (real nvapi) or Anti-Lag 2 (fakenvapi on
// an AMD card, Anti-Lag Vk on Vulkan) does, and XeLL stays in place for XeFG only, with its low latency off and no sleep
// (native/XefgReflexRule.h).
enum class XefgLatency : uint32_t
{
    XeLL,
    Reflex,
    AntiLag2
};

inline const char* XefgLatencyIniText(XefgLatency latency)
{
    switch (latency)
    {
    case XefgLatency::Reflex:
        return "reflex";
    case XefgLatency::AntiLag2:
        return "antilag2";
    default:
        return "xell";
    }
}

// `text` is lower case (Config::readString lowers it). False for anything else, `out` is then left alone.
inline bool XefgLatencyFromIniText(const char* text, XefgLatency* out)
{
    constexpr XefgLatency all[] = { XefgLatency::XeLL, XefgLatency::Reflex, XefgLatency::AntiLag2 };

    for (const XefgLatency latency : all)
    {
        if (SameWord(text, XefgLatencyIniText(latency)))
        {
            *out = latency;
            return true;
        }
    }

    return false;
}

struct Inputs
{
    Setting setting = Setting::Auto;
    bool blockGameLowLatency = false;       // Input Off, and DLSS frame generation does not need the game's Reflex
    bool f5lowRunning = false;              // an Optical F5Low mode is on and not standing aside for a game upscaler
    bool xefgRunning = false;               // XeFG is the frame generation output and has its swap chain context
    bool fsrFgRunning = false;              // FSR FG is generating, on a D3D12 game or a D3D11 game through the D3D12 bridge
    bool gameCallsReflex = false;           // the game has made a Reflex call (SetSleepMode, Sleep or a marker)
    bool gameRunsXell = false;              // the game runs XeLL of its own (with low latency on, so it can limit fps)
    bool forceReflexDisabled = false;       // fakenvapi's Force Reflex is Force Disable
    bool forceXell = false;                 // fakenvapi's Force XeLL (it takes the frame generation slot and runs XeLL)
    bool otherFrameGenerationOwner = false; // frame generation that runs Reflex itself (DLSS FG, an external owner)
    bool apiAvailable = true;               // the Reflex function table could be found
};

enum class Decision
{
    Run,
    SettingOff,
    LowLatencyOff,
    NoF5Low,
    GameRunsReflex,
    GameRunsXell,
    ForceReflexDisabled,
    ForceXell,
    FrameGenerationOwnsReflex,
    NoApi
};

// Order matters: what the player can change comes first, then what stops us for another reason.
inline Decision Decide(const Inputs& in)
{
    if (in.setting == Setting::Off)
        return in.blockGameLowLatency ? Decision::LowLatencyOff : Decision::SettingOff;

    // XeFG cannot work without XeLL (Intel), and XeLL lowers nothing unless someone sleeps and marks frames: with no
    // game Reflex to do it, we do. FSR FG holds a real frame back to generate the one before it: we take that back
    // where the game does not.
    if (in.setting == Setting::Auto && !in.f5lowRunning && !in.xefgRunning && !in.fsrFgRunning)
        return Decision::NoF5Low;

    if (in.gameCallsReflex)
        return Decision::GameRunsReflex;

    // Its XeLL already lowers latency and keeps the cap (XellHooks::update): a second method next to it is a second wait
    if (in.gameRunsXell)
        return Decision::GameRunsXell;

    if (in.forceReflexDisabled)
        return Decision::ForceReflexDisabled;

    if (in.forceXell)
        return Decision::ForceXell;

    if (in.otherFrameGenerationOwner)
        return Decision::FrameGenerationOwnsReflex;

    if (!in.apiAvailable)
        return Decision::NoApi;

    return Decision::Run;
}

// What Auto runs us for, for the log and the menu: an Optical F5Low mode first, then the frame generation that needs us
enum class RunsFor
{
    F5Low,
    Xefg,
    FsrFg
};

inline RunsFor RunReason(const Inputs& in)
{
    if (in.f5lowRunning)
        return RunsFor::F5Low;

    if (in.xefgRunning)
        return RunsFor::Xefg;

    // On with nothing running keeps the Optical F5Low name, as before
    return in.fsrFgRunning ? RunsFor::FsrFg : RunsFor::F5Low;
}

// The Framerate page's "Output": what lowers latency. Auto leaves it to the setup (NVIDIA Reflex on real nvapi, else
// InputCommon's Anti-Lag 2 / Anti-Lag Vk, XeLL or LatencyFlex; XeLL with XeFG); XeLL is the Force XeLL setting (a
// restart, it takes the frame generation slot); LatencyFlex is the Force LatencyFlex setting; Frame cap only is
// [Framerate] FrameCapOnly (InputCommon's FpsLimiter: it only waits for the cap, where the game is about to read
// input). With XeFG as the frame generation output the entries are XeLL (its own), Reflex and Anti-Lag 2 ([XeFG]
// LowLatencyOutput); Reflex and Anti-Lag 2 are offered only then (without XeFG, Auto already is Reflex or Anti-Lag 2).
enum class Output
{
    Auto,
    XeLL,
    LatencyFlex,
    Reflex,
    AntiLag2,
    FrameCapOnly
};

struct OutputInputs
{
    bool fakenvapiMain = false; // fakenvapi is the game's nvapi (not the driver's)
    bool xefgOutput = false;    // XeFG is the frame generation output
    bool amdGpu = false;        // an AMD card is the primary GPU (Anti-Lag 2 and Anti-Lag Vk are AMD's)
    bool nvidiaGpu = false;     // an NVIDIA card is the primary GPU (Reflex is NVIDIA's)
    // The driver's Reflex is (or will be) the output without XeFG: an NVIDIA card on its own nvapi, or InputCommon runs
    // Reflex anyway (a Vulkan device with VK_NV_low_latency2 under fakenvapi; OutputRule.h PlanOutput's realNvapi).
    // Nothing forced replaces it there.
    bool driverReflex = false;
    bool antiLag2Failed = false; // the Anti-Lag 2 output next to XeFG's XeLL could not start (XeFG falls back to XeLL)
    bool reflexFailed = false; // the Reflex output next to XeFG's XeLL could not start (a Vulkan device without either
                               // VK_NV_low_latency2 or NVIDIA's NvAPI Vulkan Reflex): XeFG falls back to XeLL
    XefgLatency xefgLatency = XefgLatency::XeLL; // [XeFG] LowLatencyOutput as chosen, whether the setup has it or not
};

struct OutputChoice
{
    bool available = true;
    const char* why = ""; // the reason when it is not
    // The reason holds for the whole session (the card, the nvapi, the driver's Reflex): the menu hides the entry
    // rather than greying it. Greyed stays for what can pass (an output that could not start).
    bool hidden = false;
};

// With XeFG: whether the setup has this way of lowering latency. Reflex is the driver's, so it needs the real nvapi on
// an NVIDIA card; Anti-Lag 2 is fakenvapi's on an AMD card. Neither once its output failed to start.
inline bool XefgLatencyAvailable(XefgLatency latency, const OutputInputs& in)
{
    switch (latency)
    {
    case XefgLatency::Reflex:
        return !in.fakenvapiMain && in.nvidiaGpu && !in.reflexFailed;
    case XefgLatency::AntiLag2:
        return in.fakenvapiMain && in.amdGpu && !in.antiLag2Failed;
    default:
        return true;
    }
}

// XeFG with XeLL as the only way (no NVIDIA Reflex, no AMD Anti-Lag 2): the output is XeLL and cannot be changed
inline bool OutputLocked(const OutputInputs& in)
{
    return in.xefgOutput && !XefgLatencyAvailable(XefgLatency::Reflex, in) &&
           !XefgLatencyAvailable(XefgLatency::AntiLag2, in);
}

inline OutputChoice OutputAvailability(Output output, const OutputInputs& in)
{
    if (in.xefgOutput)
    {
        switch (output)
        {
        case Output::XeLL:
            return { true, "" };
        case Output::Reflex:
            if (XefgLatencyAvailable(XefgLatency::Reflex, in))
                return { true, "" };

            if (!in.fakenvapiMain && in.nvidiaGpu && in.reflexFailed)
                return { false, "Reflex could not start in this game" };

            return { false, in.nvidiaGpu ? "Needs the driver's nvapi, not fakenvapi" : "Needs an NVIDIA card", true };
        case Output::AntiLag2:
            if (XefgLatencyAvailable(XefgLatency::AntiLag2, in))
                return { true, "" };

            if (in.fakenvapiMain && in.amdGpu && in.antiLag2Failed)
                return { false, "Anti-Lag 2 could not start in this game" };

            return { false, in.amdGpu ? "Needs fakenvapi as the game's nvapi" : "Needs an AMD card", true };
        default:
            return { false, "XeFG uses XeLL, Reflex or Anti-Lag 2", true };
        }
    }

    if (output == Output::Reflex || output == Output::AntiLag2)
        return { false, "Only with XeFG as the frame generation output", true };

    // The driver's Reflex waits at the same point and lowers latency too (OutputRule.h PlanOutput keeps it)
    if ((output == Output::LatencyFlex || output == Output::FrameCapOnly) && in.driverReflex)
        return { false, "NVIDIA Reflex runs here: it lowers latency and keeps the cap itself", true };

    if (output == Output::LatencyFlex && !in.fakenvapiMain)
        return { false, "Needs fakenvapi as the game's nvapi", true };

    return { true, "" };
}

// The Auto entry's name (without XeFG). Where the driver's Reflex is the output nothing else can be picked for it, so
// the entry is that method by name; elsewhere "Auto (<what runs>)", as it could read like the forced XeLL or
// LatencyFlex entry beside it. Null: the caller builds "Auto (...)".
inline const char* AutoEntryName(const OutputInputs& in) { return in.driverReflex ? "NVIDIA Reflex" : nullptr; }

// What the combo shows: with XeFG its choice if the setup has it, else from the flags it writes (Force XeLL and Force
// LatencyFlex in [fakenvapi], [Framerate] FrameCapOnly): XeLL wins, as it takes the slot, then Frame cap only, as in
// PlanOutput, and nothing forced where the driver's Reflex runs
inline Output OutputShown(const OutputInputs& in, bool forceXell, bool forceLatencyFlex, bool frameCapOnly = false)
{
    if (in.xefgOutput)
    {
        if (in.xefgLatency == XefgLatency::Reflex && XefgLatencyAvailable(XefgLatency::Reflex, in))
            return Output::Reflex;

        if (in.xefgLatency == XefgLatency::AntiLag2 && XefgLatencyAvailable(XefgLatency::AntiLag2, in))
            return Output::AntiLag2;

        return Output::XeLL;
    }

    if (forceXell)
        return Output::XeLL;

    // As PlanOutput: before LatencyFlex, never on NVIDIA Reflex (Auto shows there)
    if (frameCapOnly)
        return OutputAvailability(Output::FrameCapOnly, in).available ? Output::FrameCapOnly : Output::Auto;

    // Forced in the ini on a setup that does not offer it (real nvapi): Auto shows, not a greyed entry
    return forceLatencyFlex && OutputAvailability(Output::LatencyFlex, in).available ? Output::LatencyFlex
                                                                                     : Output::Auto;
}

// Boost is NVIDIA Reflex's, and OptiScaler's calls carry it: real Reflex on an NVIDIA card as the output (no fakenvapi,
// no Force XeLL; with XeFG only when Reflex is the side in force: the choice, and not fakenvapi's Force Reflex, which
// keeps XeLL) and OptiScaler allowed to make the calls
inline bool BoostAvailable(Input input, const OutputInputs& in, bool forceXell, bool forceReflexSet = false)
{
    const bool reflexOutput = in.xefgOutput ? (in.xefgLatency == XefgLatency::Reflex &&
                                               XefgLatencyAvailable(XefgLatency::Reflex, in) && !forceReflexSet)
                                            : (!in.fakenvapiMain && in.nvidiaGpu);
    return RuleForInput(input).setting != Setting::Off && reflexOutput && !forceXell;
}

// Which Reflex surface a call goes through: the D3D-flavoured NVAPI entry points (NvAPI_D3D_*, also what XeFG's routing
// to XeLL hangs off) or the Vulkan-flavoured ones (NvAPI_Vulkan_*). Each call carries its own: nothing is remembered
// between calls, so a D3D12 device pointer can never reach a Vulkan entry point or the other way round.
enum class Api
{
    D3D,
    Vulkan
};

struct Target
{
    Api api = Api::D3D;
    const void* device = nullptr; // null: nothing to call
};

// A Vulkan game's present while the frame-generation bridge is up is the bridge's D3D12 swapchain's, on the bridge's
// private D3D12 device: its low latency goes through the D3D surface on that device (so XeFG's XeLL routing reaches it,
// as for a D3D11 game's hidden present) and no Vulkan NVAPI call is made. Without the bridge a Vulkan game's own
// VkDevice goes through the Vulkan surface. A D3D game's present is always D3D.
//
// `splitOnVulkan`: XeFG has Reflex or Anti-Lag 2 as its Output (XefgLatency). XeLL on the bridge's device only keeps XeFG
// running then, so the game's VkDevice and the Vulkan surface (the driver's Reflex, or Anti-Lag Vk) are where latency is
// lowered.
inline Target PresentTarget(bool vulkanGame, bool bridged, const void* gameDevice, const void* bridgeDevice12,
                            bool splitOnVulkan = false)
{
    if (!vulkanGame)
        return { Api::D3D, gameDevice };

    if (bridged && !splitOnVulkan)
        return { Api::D3D, bridgeDevice12 };

    return { Api::Vulkan, gameDevice };
}

// Where the Reflex pair (PresentBegin before the present, PresentEnd with the Sleep after it) runs for a D3D11 game on dxvk
// with the Vulkan present bridge. dxvk presents to Vulkan from its own thread, so the Sleep at the Vulkan present paces
// dxvk's thread and the game's thread runs ahead until dxvk's frame latency limit stops it. At the game's own D3D11
// Present the Sleep holds the game's thread, as it does in a plain D3D11 game.
enum class SleepSite
{
    VulkanPresent, // hkvkQueuePresentKHR, as for a native Vulkan game
    GamePresent    // the wrapped D3D11 swap chain's Present, on the game's thread
};

// [DlssNr] NativeLowLatencyDxvkGameThread (`keyOn`) only counts for dxvk through the Vulkan bridge; everything else keeps
// the site it has always had.
inline SleepSite SleepSiteFor(bool dxvkThroughVulkan, bool keyOn)
{
    return dxvkThroughVulkan && keyOn ? SleepSite::GamePresent : SleepSite::VulkanPresent;
}

// One entry per presented frame of a dxvk game with the Vulkan bridge: the game's D3D11 Present pushes the frame's entry
// (the XeLL frame ID its markers carry, and where the Reflex pair runs for it), and dxvk's vkQueuePresentKHR for that frame
// pops it, first in first out. dxvk presents a few frames behind the game and on another thread, so each side has to
// follow the frame's own entry, not whatever the game is doing now:
//   - the Vulkan present runs the pair only for an entry that says VulkanPresent, so a frame is paired exactly once
//     whatever happens to the site setting while it is in flight;
//   - the Vulkan present hands XeFG the ID of the frame it presents (a game-site entry's), not the newest.
// It is never cleared when the site changes (the frames in flight are still owed their pop). Where the two sides drift
// apart there is a defined way back:
//   - a push past kSize drops the oldest entry, and a pop drops entries older than kStaleMs: dxvk presents a frame within
//     a few frames of its push, so an older entry is for a Present that dxvk never presented (minimised, occluded), and
//     the entries after it are the ones that line up with dxvk's next presents again;
//   - a pop with nothing queued (or the entry dropped) pairs at the Vulkan present, as before this existed;
//   - the bridge's output was made again (`generation`, VkPresentBridge::OutputGeneration): what is queued was pushed for
//     frames presented unbridged, so it is dropped, with no frames owed.
struct FrameLedger
{
    struct Entry
    {
        uint32_t frameId = 0; // 0: nobody feeds XeLL for this frame
        SleepSite site = SleepSite::VulkanPresent;
        uint64_t seq = 0;
        double at = 0.0; // ms, when it was pushed (0: not tracked)
    };

    static constexpr int kSize = 6; // dxvk holds 3 frames by default, plus the one being pushed, plus a spare
    static constexpr double kStaleMs = 500.0; // 3 frames of dxvk's default latency are 100 ms even at 30 fps

    Entry entries[kSize] = {};
    int count = 0;
    int head = 0;
    uint32_t generation = 0;
    uint64_t nextSeq = 1;
    bool resync = false; // an entry was dropped or found missing after pops had worked, or the output was made again
    bool everPopped = false;

    // The bridge's output generation of the caller (never goes back): a newer one starts the ledger over
    void Sync(uint32_t newGeneration)
    {
        if (newGeneration <= generation)
            return;

        if (count > 0)
            resync = true;

        generation = newGeneration;
        Clear();
    }

    // The new entry's sequence number, for Undo
    uint64_t Push(uint32_t frameId, SleepSite site, uint32_t callerGeneration = 0, double nowMs = 0.0)
    {
        Sync(callerGeneration);

        if (count == kSize)
        {
            head = (head + 1) % kSize;
            --count;
            resync = true;
        }

        Entry& entry = entries[(head + count) % kSize];
        entry.frameId = frameId;
        entry.site = site;
        entry.seq = nextSeq++;
        entry.at = nowMs;
        ++count;
        return entry.seq;
    }

    bool Pop(Entry* out, uint32_t callerGeneration = 0, double nowMs = 0.0)
    {
        Sync(callerGeneration);

        // Entries for Presents dxvk never presented: the frames after them are the ones dxvk presents next
        while (count > 0 && nowMs > 0.0 && entries[head].at > 0.0 && nowMs - entries[head].at > kStaleMs)
        {
            head = (head + 1) % kSize;
            --count;
            resync = true;
        }

        if (count == 0)
        {
            // Empty before the first pop is just the start (the game's presents may not have been counted yet)
            if (everPopped)
                resync = true;

            return false;
        }

        *out = entries[head];
        head = (head + 1) % kSize;
        --count;
        everPopped = true;
        return true;
    }

    // The game's Present did not go through (occluded, failed): dxvk will not present that frame. False when dxvk already
    // popped it.
    bool Undo(uint64_t seq)
    {
        for (int i = 0; i < count; ++i)
        {
            if (entries[(head + i) % kSize].seq != seq)
                continue;

            for (int j = i; j + 1 < count; ++j)
                entries[(head + j) % kSize] = entries[(head + j + 1) % kSize];

            --count;
            return true;
        }

        return false;
    }

    void Clear()
    {
        count = 0;
        head = 0;
        everPopped = false;
    }

    bool TakeResync()
    {
        const bool was = resync;
        resync = false;
        return was;
    }
};

// The game's Present starts a frame paired at its site. The ID the entry carries is read with the site lock held, right
// before the pair's Begin: the Vulkan site's End for an older frame (dxvk's thread, right after a switch) moves the frame
// ID on, and an ID read before the lock could be one behind the markers the Begin sends. `takeLock` returns the held
// lock, `readId` the ID the markers will carry, `push` makes the entry (returns its sequence number), `begin` is the pair's
// Begin.
template <class TakeLock, class ReadId, class Push, class Begin>
auto BeginGameSiteFrame(TakeLock&& takeLock, ReadId&& readId, Push&& push, Begin&& begin)
{
    [[maybe_unused]] auto lock = takeLock();
    const auto id = readId();
    const auto seq = push(id);
    begin();
    return seq;
}

// The game's Present began the pair and its device is gone before the End (dxvk remade its swapchain inside the Present and
// the bridge let go of the device): no call can go to the device, but the pair's state must not be left half way (PRESENT_END
// owed, this frame's submit markers marked sent) and the frame ID moves on as the End would have moved it, or the next frame
// reuses this one's.
template <class Started, class Submitted, class FrameId>
void AbandonPair(Started& presentStarted, Submitted& submitSent, FrameId& frameId)
{
    presentStarted = false;
    submitSent = false;
    ++frameId;
}

// Plain words for the menu when we do not run. Run has no text here: the path in use names it.
// Where a present was seen. With OptiScaler's frame generation on a D3D12 game, the game presents to frame
// generation's swap chain (FGHooks::FGPresent) and frame generation then presents every shown frame, real and
// generated, through the wrapped swap chain on its own thread: only the first is the game's frame.
enum class PresentSource
{
    SwapChain,      // the wrapped swap chain (no frame generation swap chain), or the D3D11 bridge's game present
    FrameGeneration // FGHooks::FGPresent, the game's present to frame generation
};

// How long a FrameGeneration present keeps SwapChain presents ignored: frame generation's swap chain can go away
// (turned off, swap chain recreated), and then the wrapped swap chain's presents are the game's again.
inline constexpr double kFrameGenerationPresentHoldMs = 1000.0;

// A SwapChain present is skipped while FrameGeneration presents come in. `lastFrameGenerationMs` is 0 when none came.
inline bool SwapChainPresentIgnored(double nowMs, double lastFrameGenerationMs)
{
    return lastFrameGenerationMs > 0.0 && nowMs - lastFrameGenerationMs < kFrameGenerationPresentHoldMs;
}

// The menu's status line: "Status: [<StatusLabel>]: <reason>". On: it runs (the reason is the method in use). Standing
// Aside: something else owns latency here (the game's Reflex, a fakenvapi setting, frame generation). Off: nothing to
// run on, or switched off.
inline const char* StatusLabel(Decision decision)
{
    switch (decision)
    {
    case Decision::Run:
        return "On";
    case Decision::GameRunsReflex:
    case Decision::GameRunsXell:
    case Decision::ForceReflexDisabled:
    case Decision::ForceXell:
    case Decision::FrameGenerationOwnsReflex:
        return "Standing aside";
    default:
        return "Off";
    }
}

// The decision both pages show. With the Input on Game (or Auto acting as Game with NR + frame generation) Decide stops
// at SettingOff before it looks at the game, so a game making its own Reflex calls read as "[Off]" next to "Active: Game
// -> XeLL". For the menu that is the game's Reflex at work, and we stand aside for it. Off keeps its own wording (the
// game's Reflex may be kept on for DLSS FG, which has its own line). The code runs on Decide's answer, not this one.
inline Decision ShownDecision(Decision decision, Input input, bool gameCallsReflex)
{
    return decision == Decision::SettingOff && input != Input::Off && gameCallsReflex ? Decision::GameRunsReflex
                                                                                      : decision;
}

// The Framerate page's "Active: <input> -> <output>": who makes the latency calls right now. The game's Reflex counts
// whenever the game makes calls, whatever we decided about ours. Frame generation that runs Reflex itself (DLSS FG) makes
// calls whatever the Input says, and so does a game's own XeLL; both are facts, not read off the order Decide checks in.
inline const char* ActiveInputText(Decision decision, bool gameCallsReflex, bool gameRunsXell = false,
                                   bool frameGenerationNeedsReflex = false)
{
    switch (decision)
    {
    case Decision::Run:
        return "OptiScaler";
    case Decision::LowLatencyOff:
        return "Off";
    case Decision::FrameGenerationOwnsReflex:
        return "Frame generation";
    default:
        break;
    }

    if (gameCallsReflex)
        return "Game";

    if (frameGenerationNeedsReflex)
        return "Frame generation";

    return gameRunsXell ? "Game" : "None";
}

// The reason when it does not run. Run has none here: the method in use names it. `gameReflexFeedsXell`: the game's own
// Reflex calls are sent to XeFG's XeLL (native/XefgReflexRule.h), so its Reflex settings are not the thing to change;
// `forceReflexDisabled`: fakenvapi's Force Disable, which XeLL then gets them with (low latency off).
// `xefgSide`: with XeFG, what lowers latency (XefgLatency): Reflex or Anti-Lag 2 take the game's Reflex calls, XeLL only
// keeps XeFG running (native/XefgReflexRule.h)
inline const char* DecisionText(Decision decision, bool gameReflexFeedsXell = false, bool forceReflexDisabled = false,
                                XefgLatency xefgSide = XefgLatency::XeLL)
{
    switch (decision)
    {
    case Decision::SettingOff:
        return "OptiScaler makes no latency calls: the Input is Game (Auto acts as Game with NR + frame generation) "
               "or Off";
    case Decision::LowLatencyOff:
        return "Input is Off: no low latency, the game's Reflex is sent with it off";
    case Decision::NoF5Low:
        return "No Optical F5Low mode, XeFG or FSR frame generation is running";
    case Decision::GameRunsReflex:
        if (xefgSide == XefgLatency::Reflex)
            return "The game's own Reflex lowers latency; XeLL only keeps XeFG running";

        if (xefgSide == XefgLatency::AntiLag2)
            return "The game's Reflex calls go to Anti-Lag 2; XeLL only keeps XeFG running";

        if (!gameReflexFeedsXell)
            return "Use the game's own Reflex settings";

        return forceReflexDisabled ? "The game's Reflex feeds XeFG's XeLL, low latency off by Force Disable"
                                   : "The game's Reflex feeds XeFG's XeLL";
    case Decision::GameRunsXell:
        return "The game runs XeLL itself";
    case Decision::ForceReflexDisabled:
        return "Force Reflex is set to Force Disable in the fakenvapi settings";
    case Decision::ForceXell:
        return "Force XeLL is on in the fakenvapi settings and already runs XeLL";
    case Decision::FrameGenerationOwnsReflex:
        return "The frame generation in use runs Reflex itself";
    case Decision::NoApi:
        return "No Reflex or fakenvapi interface could be found";
    default:
        return "";
    }
}

// What our calls reach: the method decides how a frame cap is counted.
enum class Method
{
    RealReflex,  // the driver's Reflex (NVIDIA, real nvapi)
    Fakenvapi,   // fakenvapi as the game's nvapi: Anti-Lag 2, XeLL or LatencyFlex
    XellWithXefg, // XeFG's XeLL (fakenvapi's context, forced by XeFG on every GPU)
    // XeFG with Reflex or Anti-Lag 2 as the output (XefgLatency): XeLL stays in place for XeFG's frame IDs with its low
    // latency off, the driver's Reflex / Anti-Lag 2 sleeps and keeps the cap
    RealReflexWithXefg,
    FakenvapiWithXefg
};

inline Method MethodWithXefg(XefgLatency latency)
{
    switch (latency)
    {
    case XefgLatency::Reflex:
        return Method::RealReflexWithXefg;
    case XefgLatency::AntiLag2:
        return Method::FakenvapiWithXefg;
    default:
        return Method::XellWithXefg;
    }
}

// XeFG follows the frame ID of the markers sent (framegen/xefg/XeFGFrameId.h), whichever way latency is lowered
inline bool CarriesXefgFrameIds(Method method)
{
    return method == Method::XellWithXefg || method == Method::RealReflexWithXefg ||
           method == Method::FakenvapiWithXefg;
}

// XeFG's XeLL is only in place for XeFG: it does not sleep and does not hold the cap
inline bool LatencySplitFromXell(Method method)
{
    return method == Method::RealReflexWithXefg || method == Method::FakenvapiWithXefg;
}

// Who keeps [Framerate] FramerateLimit while we run. One waiter per frame: the low-latency method itself when it can
// (NVIDIA and Intel both keep the render queue empty best with their own limit; Intel discourages a second limiter
// next to XeLL), OptiScaler's limiter only where the method's own limit costs more.
enum class CapOwner
{
    NotOurs,    // we do not run: the limiter and the game's own Reflex decide, as before
    LowLatency, // our SetSleepMode carries the cap; OptiScaler's limiter stands aside
    Limiter     // OptiScaler's limiter keeps it (our SetSleepMode carries no limit)
};

struct CapInputs
{
    bool running = false;            // our low latency runs (Decision::Run)
    float capFps = 0.0f;             // FramerateLimit, the shown frame rate; 0: no cap
    Method method = Method::RealReflex;
    bool fsrFgActive = false;        // FSR frame generation is generating frames
    bool limiterBeforeSleep = false; // the limiter's wait comes before our Sleep on this present path (FGPresent)
    bool d3d11Bridge = false;        // a D3D11 game through the D3D11-to-D3D12 bridge (SwapchainInteropApi::Dx11wDx12)
    bool intelGpu = false;           // XeLL works on its own only on Intel GPUs
    bool xefgGenerating = false;     // XeFG is generating frames (not paused, not off, not in exclusive full screen)
    bool fakenvapiXell = false;      // Method::Fakenvapi has picked XeLL (Anti-Lag 2 missing: fakenvapi falls back)
    uint32_t shownPerRendered = 1;   // frames XeFG shows per frame the game renders (1 + generated); 1 when it generates none
    bool xefgVsync = false;          // frame generation presents with V-Sync (State::fgPresentVsync)
    bool xellCountsShown = true;     // XeLL counts shown frames: the XeFG library tells it about its own (XeFGProxy)
    bool driverMultipliesCap = false; // DLSS-G behind XeFG's API: the driver multiplies the interval (XeFGProxy)
};

// Frames shown per frame the game renders: 1 + the generated ones, but only while frame generation generates. Stood
// down waiting for frame data (FG only with no upscaler call feeding it) it shows the game's frames alone, so the cap
// must not be divided by frames it does not make (IFGFeature::IsGenerating, not IsActive).
inline uint32_t ShownPerRendered(bool generating, uint32_t interpolated)
{
    return generating ? 1 + interpolated : 1;
}

// A cap on shown frames, as a cap on the game's own (rendered) frames
inline float CapPerRenderedFrame(float capFps, uint32_t shownPerRendered)
{
    return shownPerRendered > 1 ? capFps / (float) shownPerRendered : capFps;
}

// XeLL on an NVIDIA or AMD GPU does nothing unless XeFG generates frames (Intel: "XeLL will not perform latency
// reduction"): neither the latency nor the cap can be left to it then. fakenvapi's own XeLL never has XeFG with it.
inline bool XellIdle(const CapInputs& in)
{
    if (in.intelGpu)
        return false;

    if (in.method == Method::XellWithXefg)
        return !in.xefgGenerating;

    return in.method == Method::Fakenvapi && in.fakenvapiXell;
}

// XeFG with Reflex or Anti-Lag 2 as the output (the split): who keeps the frame cap. One rule for both our own calls
// (PlanCap) and the game's own Reflex (ReflexHooks::update). NVIDIA Reflex accepted the interval but the shown fps did
// not follow it on the D3D11-to-D3D12 bridge (Assetto Corsa), so OptiScaler's limiter keeps the cap there; on D3D12
// (NBA 2K27, the game's Reflex) and the Vulkan bridge (RDR2) the driver holds it, with the cap divided by the frames XeFG
// shows per rendered one. Anti-Lag 2 keeps its own cap everywhere.
inline bool SplitLimiterKeepsCap(XefgLatency side, bool d3d11Bridge)
{
    return side == XefgLatency::Reflex && d3d11Bridge;
}

// Whether XeFG's XeLL can keep the frame cap: not with V-Sync. XeLL then lines its frames up with the refresh rate
// instead (Arkham Knight, NVIDIA: cap 90 with XeFG 2x gave 60 rendered on a 120 Hz screen, 56 gave about 33); with
// V-Sync off it held the cap. OptiScaler's limiter keeps it then, XeLL gets no interval and still lowers latency.
inline bool XellKeepsCap(bool xefgVsync) { return !xefgVsync; }

struct CapPlan
{
    CapOwner owner = CapOwner::NotOurs;
    uint32_t minimumIntervalUs = 0; // for our SetSleepMode; 0: no limit there

    bool operator==(const CapPlan&) const = default;
};

// Clamped as misc/FrameLimit does: a tiny ini cap must not overflow the conversion
inline uint32_t IntervalUs(float fps)
{
    if (fps <= 0.0f)
        return 0u;

    const float us = 1000000.0f / fps + 0.5f;
    return us >= 100000000.0f ? 100000000u : (uint32_t) us;
}

inline CapPlan PlanCap(const CapInputs& in)
{
    if (!in.running)
        return {};

    // Real Reflex's limit with FSR frame generation costs performance (upstream's warning in the Framerate menu); the
    // limiter keeps it where its wait comes before our Sleep. Elsewhere (the Vulkan bridge, whose limiter waits inside
    // the D3D12 present after our Sleep) Reflex's limit is still better than waiting after Sleep.
    if (in.method == Method::RealReflex && in.fsrFgActive && in.limiterBeforeSleep)
        return { CapOwner::Limiter, 0 };

    // XeFG with Reflex or Anti-Lag 2 as the output: they pace the game's own frames, XeFG shows several per frame. XeLL
    // never held the cap, so its being idle (XeFG paused) changes nothing. The sleep is where they want the wait, so they
    // keep the cap, except where the driver was seen not to hold it (SplitLimiterKeepsCap).
    if (LatencySplitFromXell(in.method))
    {
        if (SplitLimiterKeepsCap(in.method == Method::RealReflexWithXefg ? XefgLatency::Reflex : XefgLatency::AntiLag2,
                                 in.d3d11Bridge))
            return { CapOwner::Limiter, 0 };

        if (in.capFps <= 0.0f)
            return { CapOwner::LowLatency, 0 };

        // DLSS-G behind XeFG's API: the driver turns the shown-frame interval into one per rendered frame itself
        if (in.method == Method::RealReflexWithXefg && in.driverMultipliesCap)
            return { CapOwner::LowLatency, IntervalUs(in.capFps) };

        return { CapOwner::LowLatency, IntervalUs(CapPerRenderedFrame(in.capFps, in.shownPerRendered)) };
    }

    // XeLL that does nothing on this GPU (XeFG paused or off, or fakenvapi's XeLL with no XeFG): the limiter keeps it
    if (XellIdle(in))
        return { CapOwner::Limiter, 0 };

    // XeFG's XeLL with V-Sync goes by the refresh rate, not the cap
    if (in.method == Method::XellWithXefg && !XellKeepsCap(in.xefgVsync))
        return { CapOwner::Limiter, 0 };

    if (in.capFps <= 0.0f)
        return { CapOwner::LowLatency, 0 };

    // XeLL counts shown frames (rendered + generated) while XeFG generates; with XeFG paused on an Intel GPU shown
    // frames are the rendered ones, so the same interval holds. A library that does not tell XeLL about its frames
    // (xefg_cuda): XeLL counts the game's own, as Reflex does in the split (XL23)
    if (in.method == Method::XellWithXefg)
        return { CapOwner::LowLatency,
                 IntervalUs(in.xellCountsShown ? in.capFps : CapPerRenderedFrame(in.capFps, in.shownPerRendered)) };

    // Reflex, Anti-Lag 2 and LatencyFlex pace the game's own frames: FSR frame generation doubles them
    return { CapOwner::LowLatency, IntervalUs(in.fsrFgActive ? in.capFps / 2.0f : in.capFps) };
}

// Who keeps the cap after our SetSleepMode: a method that refused it never took the limit, so the limiter keeps it
// until a resend goes through
inline CapOwner OwnerAfterCall(const CapPlan& plan, bool refused)
{
    return refused && plan.owner != CapOwner::NotOurs ? CapOwner::Limiter : plan.owner;
}

// A new plan is taken once it has held for `kCapSettlePresents` presents: frame generation pauses for a few frames
// (its toggles, a changed interpolation count; IFGFeature::UpdateTarget pauses 10) would otherwise hand the cap back
// and forth, each time with a sleep mode change (Intel: GPU idle first, R11). The first plan is taken at once. XeLL
// that does nothing (XellIdle) gives the cap to the limiter at once all the same (NativeLowLatency.cpp PresentEnd).
inline constexpr int kCapSettlePresents = 16;

struct CapSettle
{
    bool started = false;
    CapPlan applied;
    CapPlan pending;
    int held = 0;                       // presents `pending` has held
    Method method = Method::RealReflex; // the method `applied` was planned for (the overload with a method)
    uint32_t shown = 1;                 // frames XeFG showed per rendered one at the last call (SettleCapForCount)
};

inline CapPlan SettleCap(CapSettle& settle, const CapPlan& planned, int needed = kCapSettlePresents)
{
    if (!settle.started || needed <= 1)
    {
        settle = { true, planned, planned, 0 };
        return planned;
    }

    if (planned == settle.applied)
    {
        settle.pending = planned;
        settle.held = 0;
        return settle.applied;
    }

    if (planned == settle.pending)
        ++settle.held;
    else
        settle.pending = planned, settle.held = 1;

    if (settle.held >= needed)
    {
        settle.applied = planned;
        settle.held = 0;
    }

    return settle.applied;
}

// The same for the method in use. A changed method (XeFG's Output switched) counts frames its own way, XeLL shown ones
// and the others rendered ones: its plan is taken at once, the interval settled for the method before would be wrong
// for it (Arkham Knight: XeLL kept Reflex's per-rendered 25000 us, half the cap, for 0.7 s after the switch).
inline CapPlan SettleCap(CapSettle& settle, const CapPlan& planned, Method method, int needed = kCapSettlePresents)
{
    if (settle.started && settle.method != method)
        settle.started = false;

    const auto cap = SettleCap(settle, planned, needed);
    settle.method = method;
    return cap;
}

// A change of XeFG's count while it keeps generating, the same method keeping the cap (XeFG's Dynamic MFG steps every
// few seconds). Settled, the split's interval (the cap per rendered frame) would pace the new count by the old one for
// `kCapSettlePresents` frames, and Dynamic MFG's check of the step (about 12 frames) would read that as the step
// failing and undo it. A pause or 1X shows one frame per rendered one: not a count step, it still settles.
inline bool CountStep(const CapSettle& settle, const CapPlan& planned, Method method, uint32_t shownPerRendered)
{
    return settle.started && settle.method == method && planned.owner == settle.applied.owner && settle.shown > 1 &&
           shownPerRendered > 1 && shownPerRendered != settle.shown;
}

// SettleCap with a method, a count step taken at once (CountStep)
inline CapPlan SettleCapForCount(CapSettle& settle, const CapPlan& planned, Method method, uint32_t shownPerRendered)
{
    const bool step = CountStep(settle, planned, method, shownPerRendered);
    const auto cap = SettleCap(settle, planned, method, step ? 1 : kCapSettlePresents);
    settle.shown = shownPerRendered;
    return cap;
}

// What one SetSleepMode of ours carried, and whether the method took it
struct SleepModeCall
{
    Api api = Api::D3D;
    const void* device = nullptr;
    uint32_t minimumIntervalUs = 0;
    bool boost = false;
    uint32_t xellGeneration = 0; // InputCommon::xefg_xell_generation while XeFG's XeLL is the method, else 0
    uint32_t side = 0;           // XefgLatency in force (XeLL, Reflex or Anti-Lag 2; XeLL without XeFG)
};

inline bool SameSleepMode(const SleepModeCall& a, const SleepModeCall& b)
{
    return a.api == b.api && a.device == b.device && a.minimumIntervalUs == b.minimumIntervalUs &&
           a.boost == b.boost && a.xellGeneration == b.xellGeneration && a.side == b.side;
}

// Our SetSleepMode goes out once, then again whenever what it carries changes: a new device, the cap, Boost, or a new
// XeLL context from XeFG (made with no limit, XeFG_Dx12). A refused one is tried again, at most once a `retryDue`.
// `last` is null before the first.
inline bool NeedsSleepModeResend(const SleepModeCall* last, bool lastRefused, const SleepModeCall& now, bool retryDue)
{
    if (last == nullptr || !SameSleepMode(*last, now))
        return true;

    return lastRefused && retryDue;
}

// How often a refused SetSleepMode is tried again
inline constexpr double kSleepModeRetryMs = 1000.0;

}// namespace native::lowlatency
