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
        }
}
