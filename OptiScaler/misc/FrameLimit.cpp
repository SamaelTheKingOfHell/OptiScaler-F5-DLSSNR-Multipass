#include "pch.h"
#include "FrameLimit.h"

#include "Config.h"
#include "State.h"
// #include "hooks/D3D11Hooks.h"

inline uint64_t FrameLimit::get_timestamp()
{
    FILETIME fileTime;
    GetSystemTimePreciseAsFileTime(&fileTime);

    uint64_t time = (static_cast<uint64_t>(fileTime.dwHighDateTime) << 32) | fileTime.dwLowDateTime;

    return time * 100;
}

// https://learn.microsoft.com/en-us/windows/win32/sync/using-waitable-timer-objects
inline int FrameLimit::timer_sleep(int64_t hundred_ns)
{
    static HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due_time;

    due_time.QuadPart = -hundred_ns;

    if (!timer)
        return 1;

    if (!SetWaitableTimerEx(timer, &due_time, 0, NULL, NULL, NULL, 0))
        return 2;

    if (WaitForSingleObject(timer, 1000) != WAIT_OBJECT_0)
        return 3;

    return 0;
};

inline int FrameLimit::busywait_sleep(int64_t ns)
{
    auto current_time = get_timestamp();
    auto wait_until = current_time + ns;
    while (current_time < wait_until)
    {
        current_time = get_timestamp();
    }
    return 0;
}

inline int FrameLimit::combined_sleep(int64_t ns)
{
    constexpr int64_t busywait_threshold = 2'000'000; // 2ms
    int status {};
    auto current_time = get_timestamp();
    if (ns <= busywait_threshold)
        status = busywait_sleep(ns);
    else
        status = timer_sleep((ns - busywait_threshold) / 100);

    if (int64_t sleep_deviation = ns - (get_timestamp() - current_time); sleep_deviation > 0 && !status)
        status = busywait_sleep(sleep_deviation);

    return status;
}

namespace
{
std::atomic<double> sleptMs = 0.0;
}

float FrameLimit::CapFps()
{
    auto& state = State::Instance();
    const float limit = Config::Instance()->FramerateLimit.value_or_default();
    const float dynamic = state.xefgDynamicCapFps.load(std::memory_order_relaxed);
    const auto fg = state.currentFG;

    // While XeFG is on, through its short pauses (UI composition or hudless changes pause it for 10 frames): switching
    // back to the FPS limit and again would resend every sleep mode twice a pause. Not when XeFG passes frames through
    // (real exclusive full screen) or generates none (1X, which the cap never picks): it would cap the game's own
    // frames.
    if (dynamic <= 0.0f || !UsesXeFGApi(state.activeFgOutput) || fg == nullptr || !fg->IsActive() ||
        fg->GenerationOff() || state.realExclusiveFullscreen)
    {
        return limit;
    }

    return limit > 0.0f ? std::min(limit, dynamic) : dynamic;
}

double FrameLimit::TakeSleptMs() { return sleptMs.exchange(0.0); }

void FrameLimit::sleep(uint32_t shownPerRendered)
{
    if (auto fpsCap = CapFps(); fpsCap != 0.0f)
    {
        uint64_t min_interval_us = std::clamp((uint64_t) (1'000'000 / fpsCap), 0ULL, 100'000'000ULL);

        if (shownPerRendered > 1)
            min_interval_us *= shownPerRendered;

        static uint64_t previous_frame_time = 0;
        uint64_t current_time = get_timestamp();
        uint64_t frame_time = current_time - previous_frame_time;
        if (frame_time < 1000 * min_interval_us)
        {
            if (auto res = combined_sleep(min_interval_us * 1000 - frame_time); res)
                LOG_ERROR("Sleep command failed: {}", res);
        }
        previous_frame_time = get_timestamp();
        sleptMs.fetch_add((double) (previous_frame_time - current_time) / 1'000'000.0);
    }
}
