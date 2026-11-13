#pragma once

// GPU time of Optical F5Low's frame step in a game, by stage, from timestamps on the producer's own command list
// (native/NativeProducer.cpp). Pure, so a host test can run it (tests/f5low_gpu_stage_timing_smoke.cpp): the producer
// hands it each finished frame's raw timestamps and the queue's tick rate, the driver logs Report().
//
// Stages, in recording order, between one timestamp and the next:
//   motion   - the depth alignment and the optical flow;
//   trust    - the trust mask, the consumer's guides, and the copy of the flow's scene-cut flag;
//   consumer - NR on the picture or the virtual upscaler (its backend pass when one runs, and what it hands frame
//              generation), plus the menu's debug view when one is up and the picture's barrier back.
// No barrier sits at a stage boundary and a timestamp does not wait for earlier dispatches, so time near a boundary can
// land in the neighbour; the total is exact. The consumer is averaged over the frames it ran in. The frame source's
// picture and depth copies run before the list and are not in it.

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <string>

namespace native::gputiming
{

inline constexpr uint32_t kStages = 3;
inline constexpr uint32_t kStamps = kStages + 1; // one before the first stage, one after each
inline constexpr uint32_t kConsumer = 2;

inline const char* StageName(uint32_t stage)
{
    switch (stage)
    {
    case 0:
        return "motion";
    case 1:
        return "trust mask + guides";
    case kConsumer:
        return "consumer";
    default:
        return "?";
    }
}

class Stats
{
  public:
    // One frame's timestamps (ticks) at `ticksPerSecond`; `consumerRan`: NR or the virtual upscaler ran in it. A frame
    // whose stamps go backwards (a queue that does not keep them in order, a wrapped counter) or with no tick rate is
    // not counted.
    void Add(const std::array<uint64_t, kStamps>& ticks, uint64_t ticksPerSecond, bool consumerRan)
    {
        if (ticksPerSecond == 0)
            return;

        for (uint32_t i = 1; i < kStamps; ++i)
            if (ticks[i] < ticks[i - 1])
                return;

        const double msPerTick = 1000.0 / (double) ticksPerSecond;

        for (uint32_t s = 0; s < kStages; ++s)
        {
            if (s == kConsumer && !consumerRan)
                continue;

            const double ms = (double) (ticks[s + 1] - ticks[s]) * msPerTick;
            _sum[s] += ms;
            _max[s] = (std::max) (_max[s], ms);
        }

        const double total = (double) (ticks[kStamps - 1] - ticks[0]) * msPerTick;
        _totalSum += total;
        _totalMax = (std::max) (_totalMax, total);
        ++_frames;
        _consumerFrames += consumerRan ? 1 : 0;
    }

    uint32_t Frames() const { return _frames; }

    // Average and longest per stage since the last report, then starts a new window. Empty with no frames counted.
    std::string Report()
    {
        if (_frames == 0)
            return {};

        std::string s = std::format("GPU time over {} frames, average / longest:", _frames);

        for (uint32_t i = 0; i < kStages; ++i)
        {
            if (i != kConsumer)
                s += std::format(" {} {:.3f} / {:.3f} ms,", StageName(i), _sum[i] / _frames, _max[i]);
            else if (_consumerFrames == 0)
                s += std::format(" {} did not run,", StageName(i));
            else
                s += std::format(" {} {:.3f} / {:.3f} ms (in the {} frames it ran),", StageName(i),
                                 _sum[i] / _consumerFrames, _max[i], _consumerFrames);
        }

        s += std::format(" total {:.3f} / {:.3f} ms", _totalSum / _frames, _totalMax);
        *this = Stats {};
        return s;
    }

  private:
    std::array<double, kStages> _sum {};
    std::array<double, kStages> _max {};
    double _totalSum = 0.0;
    double _totalMax = 0.0;
    uint32_t _frames = 0;
    uint32_t _consumerFrames = 0;
};

} // namespace native::gputiming
