#pragma once

#include <optional>
#include <cstdint>

namespace DlssNr::MultiGpu
{
// An ordered pass split. The game GPU owns [0, primaryPasses), then the secondary owns the rest.
// This describes placement only; it does not submit work or transport resources.
struct PassPlan
{
    unsigned totalPasses;
    unsigned primaryPasses;

    bool IsPrimary(unsigned pass) const { return pass < primaryPasses; }
    bool HasSecondary() const { return primaryPasses < totalPasses; }
};

// Reject invalid requests instead of silently changing the user's pass placement.
inline std::optional<PassPlan> MakePassPlan(unsigned totalPasses, unsigned primaryPasses, unsigned maxPasses)
{
    if (totalPasses == 0 || totalPasses > maxPasses || primaryPasses > totalPasses)
        return std::nullopt;
    return PassPlan { totalPasses, primaryPasses };
}

// Each checked pass requests the handoff. The earliest checked pass wins, so
// every pass after the handoff stays on the secondary device.
inline std::optional<PassPlan> MakePassPlanFromMask(unsigned totalPasses, uint32_t mask, unsigned maxPasses)
{
    if (totalPasses == 0 || totalPasses > maxPasses || totalPasses > 32)
        return std::nullopt;
    for (unsigned pass = 0; pass < totalPasses; ++pass)
        if (mask & (uint32_t { 1 } << pass))
            return PassPlan { totalPasses, pass };
    return PassPlan { totalPasses, totalPasses };
}

// Where one pass runs, as the Passes page offers it.
enum class PassDevice : unsigned
{
    Game,   // the game GPU alone
    Second, // the second GPU alone
    Split   // both: the second GPU evaluates the middle of this pass's network
};

// The two stored masks behind the per-pass choice. `secondary` is SecondaryPassMask (its first set bit starts the
// second GPU, which keeps every later pass). `split` is NetworkSplitPassMask (bit i = pass i + 1 is split).
struct DeviceMasks
{
    uint32_t secondary = 0;
    uint32_t split = 0;
};

inline uint32_t PassBits(unsigned passes) { return passes >= 32 ? ~uint32_t { 0 } : (uint32_t { 1 } << passes) - 1u; }

// The passes whose network is split. A missing mask keeps the older single-pass setting (1-based `legacyPass`).
inline uint32_t SplitPassMask(bool enabled, std::optional<uint32_t> mask, unsigned legacyPass)
{
    if (!enabled)
        return 0;
    if (mask)
        return *mask;
    return legacyPass >= 1 && legacyPass <= 32 ? uint32_t { 1 } << (legacyPass - 1) : 0u;
}

// What a pass does with these masks. A split anywhere takes the place of the second GPU's whole passes: the second
// GPU does one kind of work in a frame.
inline PassDevice DeviceOf(DeviceMasks masks, unsigned pass, unsigned totalPasses)
{
    if (pass >= totalPasses || pass >= 32)
        return PassDevice::Game;
    const uint32_t bit = uint32_t { 1 } << pass;
    if (masks.split & PassBits(totalPasses))
        return (masks.split & bit) ? PassDevice::Split : PassDevice::Game;
    return (masks.secondary & PassBits(pass + 1)) ? PassDevice::Second : PassDevice::Game;
}

// The masks after choosing a device for one pass, changing as little else as the rules allow:
//   Second: this pass and every later one move to the second GPU, and no pass stays split.
//   Game:   this pass and every earlier one leave the second GPU; this pass is not split.
//   Split:  this pass is split; no pass stays on the second GPU alone.
inline DeviceMasks ChooseDevice(DeviceMasks masks, unsigned pass, PassDevice device, unsigned maxPasses)
{
    if (pass >= maxPasses || pass >= 32)
        return masks;
    const uint32_t bit = uint32_t { 1 } << pass;
    unsigned first = maxPasses; // the pass that starts the second GPU
    for (unsigned i = 0; i < maxPasses && i < 32; ++i)
        if (masks.secondary & (uint32_t { 1 } << i))
        {
            first = i;
            break;
        }
    switch (device)
    {
    case PassDevice::Second:
        first = first < pass ? first : pass;
        masks.split = 0;
        break;
    case PassDevice::Game:
        first = first > pass ? first : pass + 1;
        masks.split &= ~bit;
        break;
    case PassDevice::Split:
        first = maxPasses;
        masks.split |= bit;
        break;
    }
    masks.secondary = first < maxPasses && first < 32 ? uint32_t { 1 } << first : 0u;
    return masks;
}
} // namespace DlssNr::MultiGpu
