// Checks OptiScaler/dlssnr/DlssNr_SplitCalibration.h on a recorded evaluate: a launches CSV written by
// tests/nr_gpu/nr_cuda_trace (raw parameter blocks) and a roles.txt written by nr_split_splice (SPLICE_ROLES: measured
// per-kernel writes). The computed plan must cover every byte of a reference plan (one validated on the GPU), and may
// exceed it only inside the synchronization area.
//
// nr_split_calibration <launches.csv> <workspace base hex> <workspace bytes> <roles.txt> <reference plan>
#include "../OptiScaler/dlssnr/DlssNr_SplitCalibration.h"
#include <cstdio>
#include <fstream>
#include <sstream>

using namespace DlssNr::NetworkSplit;
using namespace DlssNr::NetworkSplit::Calibration;

static std::vector<std::string> Fields(const std::string& line)
{
    std::vector<std::string> out(1);
    bool quoted = false;
    for (char c : line)
    {
        if (c == '"')
            quoted = !quoted;
        else if (c == ',' && !quoted)
            out.emplace_back();
        else
            out.back() += c;
    }
    return out;
}

static uint64_t Covered(const std::vector<Range>& mine, uint64_t o, uint64_t n)
{
    uint64_t have = 0;
    for (const auto& r : mine)
    {
        const uint64_t lo = (std::max)(o, r.offset), hi = (std::min)(o + n, r.offset + r.bytes);
        if (hi > lo)
            have += hi - lo;
    }
    return have;
}

int main(int argc, char** argv)
{
    if (argc < 6)
    {
        std::puts("usage: nr_split_calibration <launches.csv> <base hex> <bytes> <roles.txt> <reference plan>");
        return 2;
    }
    const uint64_t base = std::stoull(argv[2], nullptr, 16), size = std::stoull(argv[3]);
    std::ifstream csv(argv[1]);
    std::string line;
    std::getline(csv, line);
    const auto header = Fields(line);
    auto column = [&](const char* name)
    {
        for (size_t i = 0; i < header.size(); ++i)
            if (header[i] == name)
                return i;
        return size_t(-1);
    };
    const size_t cFrame = column("frame"), cKernel = column("kernel"), cRaw = column("raw");
    std::vector<std::pair<std::string, Kernel>> rows;
    while (std::getline(csv, line))
    {
        const auto f = Fields(line);
        Kernel k;
        k.name = f[cKernel];
        for (size_t i = 0; i + 1 < f[cRaw].size(); i += 2)
            k.params.push_back(static_cast<uint8_t>(std::stoul(f[cRaw].substr(i, 2), nullptr, 16)));
        rows.push_back({ f[cFrame], k });
    }
    const std::string last = rows.back().first;
    std::vector<Kernel> ks;
    for (auto& [frame, k] : rows)
        if (frame == last)
            ks.push_back(k);

    Layout layout;
    std::string why;
    if (!Decode(ks, base, size, layout, why))
    {
        std::printf("FAIL: decode: %s\n", why.c_str());
        return 1;
    }
    int cut, resume;
    if (!ChooseCut(layout, cut, resume, why))
    {
        std::printf("FAIL: cut: %s\n", why.c_str());
        return 1;
    }
    Writes writes(ks.size());
    std::ifstream roles(argv[4]);
    while (std::getline(roles, line))
    {
        std::istringstream l(line);
        char kind;
        int k;
        uint64_t o, n;
        if (l >> kind >> k >> o >> n && kind == 'W' && k >= 0 && k < (int) ks.size())
            writes[k].push_back({ o, n });
    }
    Plan plan;
    if (!Compute(layout, writes, cut, resume, plan, why))
    {
        std::printf("FAIL: compute: %s\n", why.c_str());
        return 1;
    }
    std::printf("%zu kernels, sync %llu bytes, first stage %ux%u, cut %d, resume %d, forward %.2f MiB in %zu, backward "
                "%.2f MiB in %zu, fingerprint %016llx\n",
                ks.size(), (unsigned long long) layout.syncBytes, layout.firstH, layout.firstW, cut, resume,
                plan.forwardBytes / 1048576.0, plan.forward.size(), plan.backwardBytes / 1048576.0,
                plan.backward.size(), (unsigned long long) Fingerprint(layout));

    std::ifstream refFile(argv[5]);
    Plan ref;
    // Reference files may lack the C line; give them this cut so they parse.
    std::stringstream refText;
    refText << "C " << cut << " " << resume << "\n" << refFile.rdbuf();
    if (!ParsePlan(refText, ref, why))
    {
        std::printf("FAIL: reference: %s\n", why.c_str());
        return 1;
    }
    int failures = 0;
    for (const auto& [label, mine, theirs] :
         { std::make_tuple("forward", &plan.forward, &ref.forward),
           std::make_tuple("backward", &plan.backward, &ref.backward) })
    {
        uint64_t missing = 0, extra = 0, total = 0;
        for (const auto& r : *theirs)
        {
            total += r.bytes;
            missing += r.bytes - Covered(*mine, r.offset, r.bytes);
        }
        for (const auto& r : *mine)
            if (r.offset + r.bytes > layout.syncBytes)
            {
                const uint64_t o = (std::max)(r.offset, layout.syncBytes), n = r.offset + r.bytes - o;
                extra += n - Covered(*theirs, o, n);
            }
        std::printf("  %s: reference %.2f MiB, missing %llu bytes, extra outside the sync area %llu bytes\n", label,
                    total / 1048576.0, (unsigned long long) missing, (unsigned long long) extra);
        failures += missing != 0 || extra != 0;
    }
    std::stringstream text;
    text << Write(plan, Fingerprint(layout), "test");
    Plan back;
    if (!ParsePlan(text, back, why) || back.layout != Fingerprint(layout) || back.forwardBytes != plan.forwardBytes)
    {
        std::puts("FAIL: written plan does not parse back");
        ++failures;
    }
    std::puts(failures ? "FAIL" : "split calibration: matches the reference plan");
    return failures ? 1 : 0;
}
