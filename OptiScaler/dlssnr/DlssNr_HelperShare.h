#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace DlssNr::MultiGpu
{
// How many bottom rows of the helper GPU's passes the helper evaluates. The game GPU evaluates the rest of those
// passes itself, as a top band. Rows == height means the helper takes its passes whole (no strip).
//
// Automatic mode measures the helper's turnaround (outbound ready -> return done) against the GPU frame period.
// The helper's answer is consumed about one frame after it was sent, so it must finish within that period:
// shrink quickly when it is late, grow slowly when it has clear headroom. Fixed mode keeps a percentage.
// ```cpp
// share.Configure(modelHeight, cfgPercent);        // 0 = automatic, 1..100 = fixed share of the rows
// share.Observe(latencyMs, periodMs, nowSeconds);  // one sample per returned helper frame
// unsigned rows = share.Rows();                    // helper rows this frame; Revision() changes when it moves
// ```
class HelperShare
{
  public:
    static constexpr unsigned Grid = 16;      // model attention window
    static constexpr unsigned Halo = 32;      // overlap each side evaluates past the seam
    static constexpr unsigned MinRows = 64;
    static constexpr double Headroom = 0.85;  // target turnaround as a fraction of the frame period
    static constexpr double GrowBelow = 0.70; // grow only below this fraction of the target

    void Configure(unsigned height, unsigned percent)
    {
        percent = std::min(percent, 100u);
        if (height == height_ && percent == percent_)
            return;
        height_ = height;
        percent_ = percent;
        const unsigned start = percent ? Align(static_cast<unsigned>(std::lround(height * (percent / 100.0)))) : height;
        Set(start, -1.0);
        ClearMeasurements();
    }
    // A turnaround/period sample of the frame most recently sent with the current Rows().
    void Observe(double latencyMs, double periodMs, double now)
    {
        if (!height_ || percent_ || !std::isfinite(latencyMs) || latencyMs <= 0 || latencyMs > 1000)
            return;
        if (std::isfinite(periodMs) && periodMs > 0.5 && periodMs < 250)
            period_ = period_ > 0 ? period_ * 0.9 + periodMs * 0.1 : periodMs;
        latency_ = samples_ ? latency_ * 0.75 + latencyMs * 0.25 : latencyMs;
        ++samples_;
        if (period_ <= 0)
            return;
        const double target = period_ * Headroom;
        lateFor_ = latency_ > target ? lateFor_ + 1 : 0;
        if (lateFor_ >= 4 && samples_ >= 6 && now - changed_ >= 0.25 && rows_ > Floor())
        {
            // Turnaround has a fixed part that does not shrink with rows, so scale a little past proportional.
            const double ratio = std::clamp(target / latency_, 0.5, 0.95);
            Set(std::max(Floor(), Align(static_cast<unsigned>(rows_ * ratio * 0.95))), now);
            ClearMeasurements();
        }
        else if (latency_ < target * GrowBelow && samples_ >= 30 && now - changed_ >= 1.0 && rows_ < height_)
        {
            Set(std::min(height_, Align(rows_ + std::max(Grid, rows_ / 10))), now);
            ClearMeasurements();
        }
    }
    unsigned Rows() const { return rows_; }
    bool Whole() const { return rows_ >= height_; }
    // Helper strip: evaluated rows start here (halo included); the game GPU keeps rows above Seam().
    unsigned Seam() const { return height_ - std::min(rows_, height_); }
    unsigned HelperFirstRow() const { return Seam() > Halo ? Seam() - Halo : 0; }
    unsigned PrimaryRows() const { return std::min(height_, Seam() + Halo); }
    uint64_t Revision() const { return revision_; }
    bool Saturated() const { return rows_ <= Floor() && lateFor_ >= 4; }
    double LatencyMs() const { return latency_; }
    double PeriodMs() const { return period_; }

  private:
    unsigned height_ = 0, percent_ = 0, rows_ = 0, samples_ = 0, lateFor_ = 0;
    double latency_ = 0, period_ = 0, changed_ = -1.0;
    uint64_t revision_ = 0;

    unsigned Floor() const { return std::min(height_, MinRows); }
    unsigned Align(unsigned rows) const
    {
        rows = (rows + Grid / 2) / Grid * Grid;
        return std::clamp(rows, Floor(), height_);
    }
    void Set(unsigned rows, double now)
    {
        rows = std::clamp(rows, Floor(), height_);
        if (rows != rows_)
            ++revision_;
        rows_ = rows;
        if (now >= 0)
            changed_ = now;
    }
    void ClearMeasurements()
    {
        samples_ = 0;
        lateFor_ = 0;
        latency_ = 0;
    }
};
} // namespace DlssNr::MultiGpu
