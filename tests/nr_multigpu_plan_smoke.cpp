#include "../OptiScaler/dlssnr/DlssNr_MultiGpuPlan.h"

#include <cassert>

int main()
{
    using DlssNr::MultiGpu::MakePassPlan;
    constexpr unsigned limit = 30;
    assert(!MakePassPlan(0, 0, limit));
    assert(!MakePassPlan(limit + 1, 0, limit));
    assert(!MakePassPlan(1, 2, limit));
    assert(!MakePassPlan(1, 0, 0));
    using DlssNr::MultiGpu::MakePassPlanFromMask;
    assert(!MakePassPlanFromMask(0, 1, limit));
    assert(!MakePassPlanFromMask(33, 1, 33));
    assert(MakePassPlanFromMask(3, 0, limit)->primaryPasses == 3);
    assert(MakePassPlanFromMask(3, 0b110, limit)->primaryPasses == 1);
    assert(MakePassPlanFromMask(3, 0b101, limit)->primaryPasses == 0);
    assert(MakePassPlanFromMask(3, 0b1000, limit)->primaryPasses == 3);
    for (unsigned total = 1; total <= limit; ++total)
        for (unsigned primary = 0; primary <= total; ++primary)
        {
            const auto plan = MakePassPlan(total, primary, limit);
            assert(plan);
            assert(plan->HasSecondary() == (primary < total));
            unsigned switches = 0;
            for (unsigned pass = 0; pass < total; ++pass)
            {
                assert(plan->IsPrimary(pass) == (pass < primary));
                if (pass > 0 && plan->IsPrimary(pass) != plan->IsPrimary(pass - 1))
                    ++switches;
            }
            assert(switches <= 1);
            if (primary < total)
            {
                const uint32_t first = uint32_t { 1 } << primary;
                const uint32_t later = primary + 1 < total ? uint32_t { 1 } << (primary + 1) : 0;
                const auto fromMask = MakePassPlanFromMask(total, first | later, limit);
                assert(fromMask && fromMask->primaryPasses == primary);
                for (unsigned pass = 0; pass < total; ++pass)
                    assert(fromMask->IsPrimary(pass) == (pass < primary));
            }
        }
}
