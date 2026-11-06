// Deterministic checks for DlssNr_HelperShare.h with a simulated helper: turnaround = fixed + perRow * rows.
#include "../OptiScaler/dlssnr/DlssNr_HelperShare.h"
#include <cstdio>
#include <cstdlib>

using DlssNr::MultiGpu::HelperShare;

static int failures = 0;
static void Expect(bool ok, const char* what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

// Runs the controller against a helper model for `seconds` at `fps`; returns final rows.
static unsigned Simulate(HelperShare& share, double fixedMs, double perRowMs, double periodMs, double seconds)
{
    double now = 0;
    while (now < seconds)
    {
        const double latency = fixedMs + perRowMs * share.Rows();
        share.Observe(latency, periodMs, now);
        now += periodMs / 1000.0;
    }
    return share.Rows();
}

int main()
{
    {
        HelperShare s;
        s.Configure(720, 0);
        Expect(s.Rows() == 720 && s.Whole(), "automatic starts with whole passes");
        // 3060-like at 720p, Ampere Preset B: ~2 ms fixed + 7.6 ms per 720 rows. 60 fps leaves headroom.
        Simulate(s, 2.0, 7.6 / 720, 16.7, 20);
        Expect(s.Whole(), "fast helper keeps whole passes");
    }
    {
        HelperShare s;
        s.Configure(1440, 0);
        // 1440p: helper ~4 ms + 30 ms per full pass, frame period 20 ms: must shrink to fit 17 ms.
        const unsigned rows = Simulate(s, 4.0, 30.0 / 1440, 20.0, 30);
        const double latency = 4.0 + 30.0 / 1440 * rows;
        Expect(!s.Whole(), "slow helper gets a strip");
        Expect(latency <= 20.0 * HelperShare::Headroom * 1.02, "strip fits within the frame period");
        Expect(latency >= 20.0 * HelperShare::Headroom * HelperShare::GrowBelow * 0.9, "strip is not needlessly small");
        Expect(rows % HelperShare::Grid == 0, "rows on the attention grid");
        Expect(s.HelperFirstRow() + HelperShare::Halo == s.Seam(), "helper evaluates a halo above the seam");
        Expect(s.PrimaryRows() == s.Seam() + HelperShare::Halo, "game GPU evaluates a halo below the seam");
        const auto revision = s.Revision();
        Simulate(s, 4.0, 30.0 / 1440, 20.0, 10);
        Expect(s.Revision() - revision <= 2, "settled share stays put");
    }
    {
        HelperShare s;
        s.Configure(720, 0);
        // Helper too slow even for the minimum strip: stays at the floor, reports saturation.
        Simulate(s, 30.0, 0.01, 16.7, 20);
        Expect(s.Rows() == HelperShare::MinRows && s.Saturated(), "hopeless helper saturates at the floor");
    }
    {
        HelperShare s;
        s.Configure(1000, 25);
        Expect(s.Rows() == 256, "fixed 25 percent aligned to the grid");
        Simulate(s, 50.0, 1.0, 16.7, 5);
        Expect(s.Rows() == 256, "fixed share ignores timing");
    }
    std::printf(failures ? "%d failure(s)\n" : "helper share: all checks passed\n", failures);
    return failures ? 1 : 0;
}
