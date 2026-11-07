#pragma once
// Network split calibration: the transfer plan of a split (DlssNr_NetworkSplit.h) worked out from the model's own
// launches, for whatever resolution, preset and runtime is running. Nothing in here is tied to one model size.
//
// Inputs: one evaluate's kernel chain (function name + raw parameter block per launch), the workspace's address and
// size, and for each kernel the workspace bytes it changed (measured read-only on the helper GPU: a workspace snapshot
// after every kernel). From those:
//
//   Layout   every workspace pointer each kernel takes, with the stage size (padded H x W) that follows it in the
//            parameter block; the synchronization area's size from kernel 0 (cc_cb_clear clears it: pointer, count of
//            32-bit words); the first Swin stage's size. Fingerprint() hashes it, so two instances (game GPU, helper
//            GPU) can be checked to lay their workspaces out identically before any byte crosses.
//   Cut      ChooseCut: the helper starts after the encoder's last full-resolution-family stage (the last "_wait"
//            kernel before the first kernel outside the tinlayout family) and stops before the post block.
//   Plan     Compute: block-level liveness (256-byte blocks). A block crosses forward when a helper kernel references
//            it before any helper kernel wrote it; backward when an end kernel references it before any end kernel
//            wrote it and the helper may have written it. The synchronization area always crosses both ways.
//
// Tensor extents: a stage tensor is H * W * C bytes, C = 32 * firstH / H (channels double as the stage halves);
// pointers with picture-level dims (pre/post blocks) are first-stage tensors; pointers with no dims extend to the next
// known pointer (at most 1 MiB). Extra forward data is harmless (the helper overwrites or reads it); extra backward
// data is not (it replaces the owner's own bytes), so backward is limited to blocks the helper measurably changed. The
// measurement runs on varied inputs so recomputed tensors differ from what they overwrite.

#include "DlssNr_NetworkSplit.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace DlssNr::NetworkSplit::Calibration
{
inline constexpr uint64_t Block = 256;

struct Kernel
{
    std::string name;
    std::vector<uint8_t> params;
};
struct Use
{
    uint64_t offset = 0;  // workspace offset the pointer refers to
    uint32_t h = 0, w = 0; // stage dims that follow the pointer, 0 when none
};
struct Layout
{
    uint64_t workspaceBytes = 0, syncBytes = 0;
    uint32_t firstH = 0, firstW = 0;
    std::vector<std::string> names;
    std::vector<std::vector<Use>> uses;
};
// Changed byte ranges per kernel (offset, bytes), as measured.
using Writes = std::vector<std::vector<std::pair<uint64_t, uint64_t>>>;

inline std::vector<Use> Uses(const std::vector<uint8_t>& p, uint64_t base, uint64_t size)
{
    struct Dims
    {
        size_t at;
        uint32_t h, w;
    };
    std::vector<Dims> dims;
    for (size_t o = 0; o + 8 <= p.size(); o += 4)
    {
        uint32_t h, w;
        std::memcpy(&h, &p[o], 4);
        std::memcpy(&w, &p[o + 4], 4);
        // The upper half of a 64-bit address is a small number too (0x18 for an address at 0x18'xxxxxxxx): never a
        // height. An address's lower half is above any stage size, so that tells them apart.
        if (o % 8 == 4)
        {
            uint32_t below;
            std::memcpy(&below, &p[o - 4], 4);
            if (below > 4096)
                continue;
        }
        if (h >= 8 && h <= 4096 && w >= 8 && w <= 4096 && w > h && h % 4 == 0 && w % 4 == 0)
            dims.push_back({ o, h, w });
    }
    std::vector<Use> out;
    for (size_t o = 0; o + 8 <= p.size(); o += 8)
    {
        uint64_t q;
        std::memcpy(&q, &p[o], 8);
        if (q < base || q >= base + size)
            continue;
        Use u { q - base, 0, 0 };
        for (const auto& d : dims)
            if (d.at > o)
            {
                u.h = d.h;
                u.w = d.w;
                break;
            }
        out.push_back(u);
    }
    return out;
}

// The address kernel 0 (cc_cb_clear) clears from: the start of the model's workspace. 0 when the chain does not
// start with it.
inline uint64_t ClearPointer(const std::vector<Kernel>& ks)
{
    uint64_t p = 0;
    if (!ks.empty() && ks[0].name.find("cb_clear") != std::string::npos && ks[0].params.size() >= 8)
        std::memcpy(&p, ks[0].params.data(), 8);
    return p;
}

// The network part of an evaluate's launches: from the synchronization clear on. Some callers add input copies
// before it (a game evaluate launched 82 kernels starting with cg2r_copy_kernel where the helper launched 80).
inline bool IsClear(const std::string& name) { return name.find("cb_clear") != std::string::npos; }
inline std::vector<Kernel> FromClear(const std::vector<Kernel>& ks)
{
    for (size_t i = 0; i < ks.size(); ++i)
        if (IsClear(ks[i].name))
            return std::vector<Kernel>(ks.begin() + i, ks.end());
    return {};
}

// Returns false (with why) when the chain does not look like the network this was built for.
inline bool Decode(const std::vector<Kernel>& ks, uint64_t base, uint64_t size, Layout& out, std::string& why)
{
    out = {};
    out.workspaceBytes = size;
    if (ks.size() < 8 || ks[0].name.find("cb_clear") == std::string::npos || ks[0].params.size() < 12)
    {
        why = "the chain does not start with the synchronization clear";
        return false;
    }
    uint64_t clearPtr;
    uint32_t words;
    std::memcpy(&clearPtr, &ks[0].params[0], 8);
    std::memcpy(&words, &ks[0].params[8], 4);
    if (clearPtr != base || !words || uint64_t(words) * 4 >= size)
    {
        why = "the synchronization clear does not cover the start of the workspace";
        return false;
    }
    out.syncBytes = (std::max<uint64_t>)((uint64_t(words) * 4 + Block - 1) / Block * Block, 64 * 1024);
    for (const auto& k : ks)
    {
        out.names.push_back(k.name);
        out.uses.push_back(Uses(k.params, base, size));
        if (!out.firstH && k.name.find("swin_1h_32") != std::string::npos &&
            k.name.find("pre_block") == std::string::npos && k.name.find("post_block") == std::string::npos)
            for (const auto& u : out.uses.back())
                if (u.h)
                {
                    out.firstH = u.h;
                    out.firstW = u.w;
                    break;
                }
    }
    if (!out.firstH)
    {
        why = "the first network stage was not found";
        return false;
    }
    return true;
}

// FNV-1a over names, pointer offsets, dims and sizes, from the clear through the post block (output copies after it
// may differ between callers): equal fingerprints mean equal workspace layouts.
inline uint64_t Fingerprint(const Layout& l)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* p, size_t n)
    {
        for (size_t i = 0; i < n; ++i)
        {
            h ^= static_cast<const uint8_t*>(p)[i];
            h *= 1099511628211ull;
        }
    };
    mix(&l.workspaceBytes, 8);
    mix(&l.syncBytes, 8);
    for (size_t k = 0; k < l.names.size(); ++k)
    {
        mix(l.names[k].data(), l.names[k].size());
        for (const auto& u : l.uses[k])
        {
            mix(&u.offset, 8);
            mix(&u.h, 4);
            mix(&u.w, 4);
        }
        if (k > 0 && l.names[k].find("post_block") != std::string::npos)
            break;
    }
    return h;
}

// The layout as text, one kernel per line (name, then offset:HxW per workspace pointer), for comparing two instances.
inline std::string Describe(const Layout& l)
{
    std::string s;
    char item[64];
    std::snprintf(item, sizeof item, "workspace %llu sync %llu first %ux%u\n", (unsigned long long) l.workspaceBytes,
                  (unsigned long long) l.syncBytes, l.firstH, l.firstW);
    s += item;
    for (size_t k = 0; k < l.names.size(); ++k)
    {
        s += std::to_string(k) + " " + l.names[k];
        for (const auto& u : l.uses[k])
        {
            std::snprintf(item, sizeof item, " %llu:%ux%u", (unsigned long long) u.offset, u.h, u.w);
            s += item;
        }
        s += "\n";
    }
    return s;
}

// The raw parameter blocks as 32-bit words in hex, one kernel per line, for comparing two instances' launches.
inline std::string DescribeParams(const std::vector<Kernel>& ks)
{
    std::string s;
    char word[12];
    for (size_t k = 0; k < ks.size(); ++k)
    {
        s += std::to_string(k) + " " + ks[k].name;
        for (size_t o = 0; o + 4 <= ks[k].params.size(); o += 4)
        {
            uint32_t v;
            std::memcpy(&v, &ks[k].params[o], 4);
            std::snprintf(word, sizeof word, " %08x", v);
            s += word;
        }
        s += "\n";
    }
    return s;
}

inline bool ChooseCut(const Layout& l, int& cut, int& resume, std::string& why)
{
    const int n = static_cast<int>(l.names.size());
    resume = cut = -1;
    for (int i = 1; i < n; ++i)
        if (l.names[i].find("post_block") != std::string::npos)
        {
            resume = i;
            break;
        }
    int family = -1;
    for (int i = 2; i < n; ++i)
        if (l.names[i].find("tinlayout") == std::string::npos)
        {
            family = i;
            break;
        }
    for (int i = 1; family > 0 && i < family; ++i)
        if (l.names[i].find("_wait") != std::string::npos)
            cut = i;
    if (resume < 0 || cut < 1 || resume <= cut + 1)
    {
        why = "no split point was found in the kernel chain";
        return false;
    }
    return true;
}

namespace Detail
{
inline void Mark(std::vector<uint8_t>& set, uint64_t offset, uint64_t bytes, uint64_t size)
{
    const uint64_t end = (std::min)(offset + bytes, size);
    for (uint64_t b = offset / Block; b < end / Block; ++b)
        set[b] = 1;
}
inline void Ranges(const std::vector<uint8_t>& set, uint64_t sync, std::vector<Range>& out, uint64_t& total)
{
    std::vector<std::pair<uint64_t, uint64_t>> r { { 0, sync } };
    for (size_t b = 0; b < set.size();)
    {
        if (!set[b])
        {
            ++b;
            continue;
        }
        size_t e = b;
        while (e < set.size() && set[e])
            ++e;
        r.push_back({ b * Block, (e - b) * Block });
        b = e;
    }
    std::sort(r.begin(), r.end());
    std::vector<std::pair<uint64_t, uint64_t>> merged;
    for (auto [o, n] : r)
    {
        const uint64_t a = o & ~(Block - 1), z = (o + n + Block - 1) & ~(Block - 1);
        if (!merged.empty() && a <= merged.back().second)
            merged.back().second = (std::max)(merged.back().second, z);
        else
            merged.push_back({ a, z });
    }
    out.clear();
    total = 0;
    for (auto [a, z] : merged)
    {
        out.push_back({ a, z - a, total });
        total += z - a;
    }
}
} // namespace Detail

// The plan for (cut, resume). False when it would not fit the handshake (too many ranges).
inline bool Compute(const Layout& l, const Writes& writes, int cut, int resume, Plan& plan, std::string& why)
{
    const uint64_t size = l.workspaceBytes, blocks = size / Block;
    const int n = static_cast<int>(l.names.size());
    if (writes.size() != l.names.size() || cut < 1 || resume <= cut + 1 || resume >= n)
    {
        why = "the measured writes do not match the kernel chain";
        return false;
    }
    std::vector<uint64_t> starts;
    for (const auto& us : l.uses)
        for (const auto& u : us)
            starts.push_back(u.offset);
    std::sort(starts.begin(), starts.end());
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
    auto extent = [&](const Use& u) -> uint64_t
    {
        if (u.h && u.h <= l.firstH)
            return uint64_t(u.h) * u.w * (32ull * l.firstH / u.h);
        if (u.h)
            return uint64_t(l.firstH) * l.firstW * 32;
        const auto next = std::upper_bound(starts.begin(), starts.end(), u.offset);
        return next == starts.end() ? Block : (std::min<uint64_t>)(*next - u.offset, 1ull << 20);
    };
    // Walks only each kernel's own tensors and writes, so planning every candidate stays fast at large sizes.
    // Pointers inside the synchronization area count too: some carry a stage tensor's dims (kernels 76 and 77 of
    // Preset B take one at 69632 with first-stage dims), and the area itself crosses regardless.
    auto live = [&](int first, int last, const std::vector<uint8_t>* eligible)
    {
        std::vector<uint8_t> written(blocks), needed(blocks), own(blocks);
        for (int k = first; k < last; ++k)
        {
            for (const auto& [o, m] : writes[k])
                Detail::Mark(own, o, m, size);
            for (const auto& u : l.uses[k])
            {
                const uint64_t end = (std::min)(u.offset + extent(u), size) / Block;
                for (uint64_t b = u.offset / Block; b < end; ++b)
                    if (!written[b] && !own[b] && (!eligible || (*eligible)[b]))
                        needed[b] = 1;
            }
            for (const auto& [o, m] : writes[k])
            {
                const uint64_t end = (std::min)(o + m, size) / Block;
                for (uint64_t b = o / Block; b < end; ++b)
                {
                    written[b] = 1;
                    own[b] = 0;
                }
            }
        }
        return needed;
    };
    // What the helper changed, exactly as measured. Not widened: a returned block replaces the owner's own, so a block
    // the helper never wrote would bring back stale data (e.g. the pre block's output that the post block reads).
    std::vector<uint8_t> helperWrites(blocks);
    for (int k = cut + 1; k < resume; ++k)
        for (const auto& [o, m] : writes[k])
            Detail::Mark(helperWrites, o, m, size);
    const auto forward = live(cut + 1, resume, nullptr);
    const auto backward = live(resume, n, &helperWrites);
    plan = {};
    plan.cut = cut;
    plan.resume = resume;
    plan.workspaceBytes = size;
    Detail::Ranges(forward, l.syncBytes, plan.forward, plan.forwardBytes);
    Detail::Ranges(backward, l.syncBytes, plan.backward, plan.backwardBytes);
    if (!plan.Valid())
    {
        why = "the plan needs more ranges than the handshake carries";
        return false;
    }
    return true;
}

// Every split the chain allows: the helper starts after a kernel that ends a synchronization group ("_wait") and
// hands back after one, or at the post block. (cut, resume) pairs, resume <= the post block.
inline std::vector<std::pair<int, int>> Candidates(const Layout& l)
{
    const int n = static_cast<int>(l.names.size());
    int post = -1;
    for (int i = 1; i < n && post < 0; ++i)
        if (l.names[i].find("post_block") != std::string::npos)
            post = i;
    std::vector<int> waits;
    for (int i = 1; i < post; ++i)
        if (l.names[i].find("_wait") != std::string::npos)
            waits.push_back(i);
    std::vector<int> resumes;
    for (int w : waits)
        if (w + 1 < post)
            resumes.push_back(w + 1);
    if (post > 0)
        resumes.push_back(post);
    std::vector<std::pair<int, int>> out;
    for (int c : waits)
        for (int r : resumes)
            if (r > c + 1)
                out.push_back({ c, r });
    return out;
}

// The plan file text, with the layout fingerprint it was calibrated on ("L"), for the cache and the owner's check.
// Several plans may follow each other in one file (each starts with its "C" line; ParsePlans reads them).
inline std::string Write(const Plan& plan, uint64_t fingerprint, const std::string& comment)
{
    std::string s = comment.empty() ? std::string() : "# " + comment + "\n";
    char line[128];
    std::snprintf(line, sizeof line, "C %d %d\nT %llu\nL %016llx\nW %llu\n", plan.cut, plan.resume,
                  (unsigned long long) plan.helperMicros, (unsigned long long) fingerprint,
                  (unsigned long long) plan.workspaceBytes);
    s += line;
    for (const auto* set : { &plan.forward, &plan.backward })
        for (const auto& r : *set)
        {
            std::snprintf(line, sizeof line, "%c %llu %llu\n", set == &plan.forward ? 'F' : 'B',
                          (unsigned long long) r.offset, (unsigned long long) r.bytes);
            s += line;
        }
    return s;
}
} // namespace DlssNr::NetworkSplit::Calibration
