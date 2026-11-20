#pragma once
#include <atomic>
#include <cstdint>

namespace DlssNr::Worker::SplitPassStatus
{
// Renderer-owned snapshot of each pass's network split for the menu. Approximate; never a control input.
// ```cpp
// SplitPassStatus::Set(pass, SplitPassStatus::Running);
// const char* text = SplitPassStatus::Text(pass);
// ```
enum State : uint8_t
{
    Off,         // not chosen for this pass
    Unavailable, // chosen, but the picture does not allow it (see Text)
    Preparing,   // chosen: calibrating, starting its helper, or whole on the game GPU for now
    Running      // this pass's last picture was split
};
inline constexpr unsigned Passes = 32;
inline std::atomic<uint8_t> states[Passes] {};

inline void Set(unsigned pass, State state)
{
    if (pass < Passes)
        states[pass].store(state, std::memory_order_relaxed);
}
inline const char* Text(unsigned pass)
{
    switch (pass < Passes ? states[pass].load(std::memory_order_relaxed) : uint8_t { Off })
    {
    case Running:
        return "split: the second GPU is evaluating the middle of this pass";
    case Preparing:
        return "whole on the game GPU for now (calibrating, starting the second GPU, or no share fits the frame)";
    case Unavailable:
        return "not available right now (needs DirectX 12 Finished Picture; detail reuse, edit cache, spatial "
               "compression and OptiScaler frame generation off)";
    default:
        return "waiting for a picture with NR";
    }
}
} // namespace DlssNr::Worker::SplitPassStatus
