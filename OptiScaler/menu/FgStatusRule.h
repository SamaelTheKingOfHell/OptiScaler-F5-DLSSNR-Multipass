#pragma once

#include <framegen/FGOutput.h>

#include <cstdio>
#include <string>

// The Frame Generation page's status line: what frame generation is doing right now, in one line. Pure, host-tested
// in tests/fg_status_rule_smoke.cpp; the menu fills Input from State and the current FG feature.
namespace fgstatus
{
enum class Tone
{
    Neutral, // off, nothing to do
    Waiting, // set up, not generating yet (amber)
    Good,    // generating (green)
};

struct Input
{
    FGOutput output = FGOutput::NoFG;
    const char* outputLabel = ""; // "F5Low", "XeFG", "FSR FG", "DLSSG"
    bool inputSelected = false;   // an FG Input other than None
    bool hasFeature = false;      // the output's FG object exists (made with the game's swap chain)
    bool active = false;          // the output's Active toggle
    bool paused = false;
    bool generationOff = false; // Dynamic MFG holding at 1X
    bool generating = false;
    int interpolated = 0;                // generated frames per rendered frame
    double shownFps = 0.0;               // frames on screen per second; 0 = not measured yet
    const char* blockedReason = nullptr; // why the output cannot run now (it turns itself off); null = nothing
    bool followsGame = false;            // Active follows the game's DLSS FG setting
};

struct Status
{
    Tone tone = Tone::Neutral;
    std::string text;
};

inline Status Describe(const Input& in)
{
    const std::string name = in.outputLabel != nullptr ? in.outputLabel : "";

    if (in.output == FGOutput::NoFG)
        return { Tone::Neutral, "Frame generation off" };
    if (!in.inputSelected)
        return { Tone::Neutral, "Off: pick an FG Input" };
    if (!in.hasFeature)
        return { Tone::Waiting, name + ": waiting for the game's swap chain" };
    // Before Active: a blocked output unticks Active itself, so "unticked" would send the player to the wrong switch
    if (in.blockedReason != nullptr)
        return { Tone::Waiting, name + ": off, " + in.blockedReason };
    if (!in.active)
        return { Tone::Neutral, name + (in.followsGame ? ": off (follows the game's DLSS FG setting)"
                                                       : ": off (Active is unticked)") };
    if (in.paused)
        return { Tone::Waiting, name + ": paused" };
    if (in.generationOff)
        return { Tone::Waiting, name + ": 1X (Dynamic MFG holds back)" };
    if (!in.generating)
        return { Tone::Waiting, name + ": waiting for the game's frames" };

    // As the FPS overlay counts it: shown frames per rendered frame
    const int multiplier = (in.interpolated > 0 ? in.interpolated : 0) + 1;
    char buffer[96];
    if (in.shownFps > 0.0)
        snprintf(buffer, sizeof(buffer), "%s: generating %dX   %.0f -> %.0f fps", name.c_str(), multiplier,
                 in.shownFps / multiplier, in.shownFps);
    else
        snprintf(buffer, sizeof(buffer), "%s: generating %dX", name.c_str(), multiplier);
    return { Tone::Good, buffer };
}
} // namespace fgstatus
