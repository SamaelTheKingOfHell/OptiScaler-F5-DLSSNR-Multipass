#pragma once

// A number for the menu that changes a few times a second, not every frame: the average of the samples since the last
// refresh. A value redrawn every frame changes its digits every frame, and with frame generation on the generated
// frames blend two different numbers into one unreadable, overlapping one. Pure, so a host test can run it
// (tests/nr_status_steady_value_smoke.cpp).

#include <optional>

namespace DlssNr
{

class SteadyValue
{
  public:
    static constexpr double kRefreshMs = 250.0;

    // One menu frame's sample (nullopt: nothing measured, which also forgets what was shown). The value to show.
    std::optional<double> Update(std::optional<double> sample, double nowMs)
    {
        if (!sample.has_value())
        {
            *this = SteadyValue {};
            return std::nullopt;
        }

        _sum += *sample;
        ++_count;

        // The first sample shows at once; later ones every kRefreshMs, as their average
        if (!_shown.has_value() || nowMs - _refreshedAt >= kRefreshMs)
        {
            _shown = _sum / _count;
            _sum = 0.0;
            _count = 0;
            _refreshedAt = nowMs;
        }

        return _shown;
    }

  private:
    std::optional<double> _shown;
    double _sum = 0.0;
    unsigned _count = 0;
    double _refreshedAt = 0.0;
};

} // namespace DlssNr
