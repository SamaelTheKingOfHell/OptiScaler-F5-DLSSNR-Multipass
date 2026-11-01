#pragma once

#include <optional>

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
} // namespace DlssNr::MultiGpu
