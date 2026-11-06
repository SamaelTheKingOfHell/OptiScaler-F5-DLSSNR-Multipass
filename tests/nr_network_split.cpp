// Deterministic checks for OptiScaler/dlssnr/DlssNr_NetworkSplit.h: plan parsing, the handshake form, and which kernels
// and transfers each role records over a simulated 80-kernel evaluate.
#include "../OptiScaler/dlssnr/DlssNr_NetworkSplit.h"
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

using namespace DlssNr::NetworkSplit;

static int failures = 0;
static void Expect(bool ok, const char* what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

struct Trace
{
    std::vector<int> launched;
    std::vector<std::string> copies; // "dst<-src"
    int barriers = 0;
};

static Trace Run(Filter& f, Role role, const Plan& plan, int kernels)
{
    Trace t;
    f.Begin(role, &plan, 0x1000000, 0x9000000, 0xA000000);
    for (int i = 0; i < kernels; ++i)
        f.Kernel([&] { t.launched.push_back(i); return 0; },
                 [&](uint64_t dst, uint64_t src, uint64_t) {
                     t.copies.push_back(std::to_string(dst) + "<-" + std::to_string(src));
                     return 0;
                 },
                 [&] { ++t.barriers; });
    Expect(f.End(), "the role recorded its transfers");
    return t;
}

int main()
{
    std::istringstream file("# test plan\nC 9 78\nW 1048576\nF 0 65536\nF 131072 4096\nB 0 65536\nB 262144 8192\n");
    Plan plan;
    std::string why;
    Expect(ParsePlan(file, plan, why), "plan parses");
    Expect(plan.cut == 9 && plan.resume == 78, "cut and resume");
    Expect(plan.forwardBytes == 65536 + 4096 && plan.backwardBytes == 65536 + 8192, "packed totals");
    Expect(plan.forward[1].packed == 65536 && plan.backward[1].packed == 65536, "packed offsets");
    Expect(plan.Fits(1048576) && !plan.Fits(2097152), "workspace size check");

    Plan back;
    Expect(FromWire(ToWire(plan), back), "handshake round trip");
    Expect(back.forward.size() == 2 && back.backward[1].offset == 262144 && back.backwardBytes == plan.backwardBytes,
           "handshake keeps ranges");

    std::istringstream bad("C 9 10\nF 0 16\nB 0 16\n");
    Plan rejected;
    Expect(!ParsePlan(bad, rejected, why), "resume must leave a segment after the cut");

    Filter f;
    auto head = Run(f, Role::Head, plan, 80);
    Expect(head.launched.size() == 10 && head.launched.back() == 9, "head launches [0, cut]");
    Expect(head.copies.size() == 2 && head.copies[0] == std::to_string(0x9000000) + "<-" + std::to_string(0x1000000),
           "head copies forward ranges out of its workspace");

    auto segment = Run(f, Role::Segment, plan, 80);
    Expect(segment.launched.front() == 0 && segment.launched[1] == 10 && segment.launched.back() == 77,
           "segment launches kernel 0 and (cut, resume)");
    Expect(segment.launched.size() == 1 + 68, "segment kernel count");
    Expect(segment.copies.size() == 4, "segment copies forward in and backward out");
    Expect(segment.copies[0] == std::to_string(0x1000000) + "<-" + std::to_string(0x9000000), "forward lands in workspace");

    auto end = Run(f, Role::End, plan, 80);
    Expect(end.launched.size() == 3 && end.launched[0] == 0 && end.launched[1] == 78 && end.launched[2] == 79,
           "end launches kernel 0 and [resume, end)");
    Expect(end.copies.size() == 2 && end.copies[1] == std::to_string(0x1000000 + 262144) + "<-" +
                                                         std::to_string(0xA000000 + 65536),
           "backward lands in workspace");

    auto off = Run(f, Role::Off, plan, 80);
    Expect(off.launched.size() == 80 && off.copies.empty(), "off passes everything");

    f.Begin(Role::Head, &plan, 0x1000000, 1, 2);
    for (int i = 0; i < 5; ++i)
        f.Kernel([] { return 0; }, [](uint64_t, uint64_t, uint64_t) { return 0; }, [] {});
    Expect(!f.End(), "a chain shorter than the cut reports an incomplete split");

    std::printf(failures ? "%d failure(s)\n" : "network split: all checks passed\n", failures);
    return failures ? 1 : 0;
}
