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
} // namespace DlssNr::MultiGpu
