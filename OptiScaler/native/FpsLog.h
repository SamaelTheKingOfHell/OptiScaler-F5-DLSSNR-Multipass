#pragma once

// The frame rate after each change of a setting that moves it, for the log: the game's own frames a second and the
// frames frame generation sends to be shown, measured once the change has settled, with the settings it was measured
// under. Pure, so a host test can run it (tests/f5low_fps_log_smoke.cpp). The caller reads the clock, the config and
// XeFG's shown-frame totals (State::xefgShown, framegen/xefg/XeFG_ShownRate.h) and logs the line. D3D12 games only: it
// is fed once per game present from NativeMotionDx12's present step (native/NativeDriverDx12.cpp).
//
// What is compared is what the player set (Settings), never what runs at the moment (Live): frame generation's own
// pauses, toggles and Dynamic MFG's count, or Auto model resolution's ratio, would otherwise restart the measurement
// before it could finish. What runs goes into the measurement line.

#include <cmath>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace native::fpslog
{

// What the player set that moves the frame rate. Compared whole: any field changing starts a new measurement.
struct Settings
{
    bool nr = false;           // Enable Neural Rendering
    float modelScale = 1.0f;   // [DlssNr] WorkingScale, the slider
    bool modelAuto = false;    // [DlssNr] ModelResolutionAuto
    bool upscalerPass = false; // Optical F5Low's "Run the upscaler on the picture"
    const char* mode = "Off";  // the Optical F5Low mode shown on its page
    const char* fgOutput = "none";
    bool fgEnabled = false;  // frame generation switched on
    uint32_t fgCount = 1;    // the multiplier asked for (interpolations + 1)
    bool dynamicMfg = false; // XeFG's Dynamic MFG picks the count per frame, up to fgCount
    float capFps = 0.0f;     // FramerateLimit, 0 none

    bool operator==(const Settings& o) const
    {
        return nr == o.nr && modelScale == o.modelScale && modelAuto == o.modelAuto && upscalerPass == o.upscalerPass &&
               std::string_view(mode) == o.mode && std::string_view(fgOutput) == o.fgOutput &&
               fgEnabled == o.fgEnabled && fgCount == o.fgCount && dynamicMfg == o.dynamicMfg && capFps == o.capFps;
    }

    // A value the ini cannot give as a number would never compare equal (a NaN), and log a change every present
    Settings Finite() const
    {
        Settings s = *this;
        s.modelScale = std::isfinite(s.modelScale) ? s.modelScale : 0.0f;
        s.capFps = std::isfinite(s.capFps) ? s.capFps : 0.0f;
        return s;
    }

    std::string Describe() const
    {
        const std::string model = modelAuto ? std::string("auto") : std::format("{:.2f}", modelScale);
        const std::string fg = !fgEnabled   ? std::string(" off")
                               : dynamicMfg ? std::format(" up to x{} (Dynamic MFG)", fgCount)
                                            : std::format(" x{}", fgCount);
        return std::format("NR {}, model {}, upscaler pass {}, Optical F5Low {}, frame generation {}{}, cap {}",
                           nr ? "on" : "off", model, upscalerPass ? "on" : "off", mode, fgOutput, fg,
                           capFps > 0.0f ? std::format("{:.0f} fps", capFps) : std::string("none"));
    }
};

// What runs this present, for the measurement line only.
struct Live
{
    bool fgRunning = false;             // frame generation active and not paused
    uint32_t fgMultiplier = 1;          // its count now, when running
    std::optional<uint64_t> shownTotal; // XeFG's frames sent to be shown, a running total; nullopt with no XeFG
    uint64_t shownMask = ~0ull;         // the width shownTotal wraps at
    int modelPercent = 0;               // the model resolution NR applied last frame (0: not known)
};

class Tracker
{
  public:
    static constexpr double kSettleMs = 1000.0;  // after a change: rebuilds and pauses are not the setting's frame rate
    static constexpr double kMeasureMs = 3000.0; // the first measurement: settle + measure fit in a 5 s menu step
    static constexpr double kRepeatMs = 30000.0; // then one line this often while nothing changes

    // One game present. A line to log when a change is seen or a measurement closes.
    std::optional<std::string> OnPresent(double nowMs, const Settings& rawSettings, const Live& live)
    {
        const Settings settings = rawSettings.Finite();

        if (!_have || !(settings == _settings))
        {
            const bool first = !_have;
            _have = true;
            _settings = settings;
            _logged = false;
            Restart(nowMs + kSettleMs);
            return first ? std::nullopt
                         : std::optional<std::string>(
                               std::format("FPS: settings changed to ({}); measuring from {:.0f} s after the change",
                                           settings.Describe(), kSettleMs / 1000.0));
        }

        if (nowMs < _start)
            return std::nullopt;

        if (!_started)
        {
            _started = true;
            _start = nowMs;
            _presents = 0;
            _shownStart = live.shownTotal;
            _fgRanThroughout = live.fgRunning;
            _fgIdleThroughout = !live.fgRunning;
            return std::nullopt;
        }

        ++_presents;
        _fgRanThroughout = _fgRanThroughout && live.fgRunning;
        _fgIdleThroughout = _fgIdleThroughout && !live.fgRunning;
        const double elapsed = nowMs - _start;

        if (elapsed < (_logged ? kRepeatMs : kMeasureMs))
            return std::nullopt;

        const double seconds = elapsed / 1000.0;
        const double game = _presents / seconds;
        std::string shown;

        // XeFG's own count: a window in which it sent nothing reads 0, not unknown (that would hide a stall)
        if (live.shownTotal.has_value() && _shownStart.has_value())
            shown = std::format("{:.1f}", (double) ((*live.shownTotal - *_shownStart) & live.shownMask) / seconds);
        else if (_fgIdleThroughout)
            shown = std::format("{:.1f}", game);
        else
            shown = "?";

        const std::string running = _fgRanThroughout    ? std::format("frame generation running x{}", live.fgMultiplier)
                                    : _fgIdleThroughout ? std::string("frame generation not running")
                                                        : std::string("frame generation on and off");
        const std::string model =
            live.modelPercent > 0 ? std::format(", model applied {}%", live.modelPercent) : std::string();

        const std::string line = std::format("FPS: game {:.1f}, shown {} over {:.1f} s ({}; {}{})", game, shown,
                                             seconds, settings.Describe(), running, model);
        _logged = true;
        Restart(nowMs);
        return line;
    }

  private:
    void Restart(double startMs)
    {
        _start = startMs;
        _started = false;
        _presents = 0;
        _shownStart.reset();
    }

    Settings _settings;
    bool _have = false;
    bool _logged = false; // a line went out for these settings: the next one waits kRepeatMs
    bool _started = false;
    double _start = 0.0;
    uint64_t _presents = 0;
    std::optional<uint64_t> _shownStart;
    bool _fgRanThroughout = false;
    bool _fgIdleThroughout = false;
};

} // namespace native::fpslog
