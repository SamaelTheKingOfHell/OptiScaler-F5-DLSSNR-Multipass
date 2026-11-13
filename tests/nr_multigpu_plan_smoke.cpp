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
    // Per-pass device choice (the Passes page's selector).
    using namespace DlssNr::MultiGpu;
    assert(PassBits(0) == 0 && PassBits(3) == 0b111 && PassBits(32) == ~uint32_t { 0 });
    assert(SplitPassMask(false, 0b11u, 1) == 0);
    assert(SplitPassMask(true, std::nullopt, 2) == 0b10);
    assert(SplitPassMask(true, 0b101u, 2) == 0b101);
    assert(SplitPassMask(true, std::nullopt, 0) == 0);
    DeviceMasks masks {};
    for (unsigned pass = 0; pass < 3; ++pass)
        assert(DeviceOf(masks, pass, 3) == PassDevice::Game);
    // Second GPU on pass 2 takes pass 3 with it.
    masks = ChooseDevice(masks, 1, PassDevice::Second, limit);
    assert(DeviceOf(masks, 0, 3) == PassDevice::Game && DeviceOf(masks, 1, 3) == PassDevice::Second &&
           DeviceOf(masks, 2, 3) == PassDevice::Second);
    assert(MakePassPlanFromMask(3, masks.secondary, limit)->primaryPasses == 1);
    // Game GPU on pass 2 brings the earlier passes back and leaves pass 3 where it was.
    masks = ChooseDevice(masks, 1, PassDevice::Game, limit);
    assert(DeviceOf(masks, 1, 3) == PassDevice::Game && DeviceOf(masks, 2, 3) == PassDevice::Second);
    // The second GPU alone, then the game GPU alone.
    masks = ChooseDevice(masks, 0, PassDevice::Second, limit);
    for (unsigned pass = 0; pass < 3; ++pass)
        assert(DeviceOf(masks, pass, 3) == PassDevice::Second);
    masks = ChooseDevice(masks, 2, PassDevice::Game, limit);
    assert(masks.secondary == 0 && masks.split == 0);
    // Any number of passes can be split; a split clears whole second-GPU passes and the reverse.
    masks = ChooseDevice({ 0b10, 0 }, 0, PassDevice::Split, limit);
    masks = ChooseDevice(masks, 2, PassDevice::Split, limit);
    assert(masks.secondary == 0 && masks.split == 0b101);
    assert(DeviceOf(masks, 0, 3) == PassDevice::Split && DeviceOf(masks, 1, 3) == PassDevice::Game &&
           DeviceOf(masks, 2, 3) == PassDevice::Split);
    masks = ChooseDevice(masks, 0, PassDevice::Game, limit);
    assert(masks.split == 0b100 && DeviceOf(masks, 0, 3) == PassDevice::Game);
    masks = ChooseDevice(masks, 1, PassDevice::Second, limit);
    assert(masks.split == 0 && DeviceOf(masks, 2, 3) == PassDevice::Second);
    // Out of range changes nothing.
    assert(ChooseDevice(masks, limit, PassDevice::Split, limit).split == 0);
    assert(DeviceOf({ 0, 0b1 }, 5, 3) == PassDevice::Game);
}
