// Host check of DlssNr_LutChain.h: a stack of LUTs baked into one lattice matches applying them one after another.
// No GPU and no game. Optional argument: a folder of .cube files; every pair in it is also compared (the error is
// printed, so the bound can be judged on real looks).
// cl /std:c++20 /EHsc /W4 tests/nr_lut_chain_smoke.cpp
#include "../OptiScaler/dlssnr/DlssNr_LutChain.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>

static int fails = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                                                \
            ++fails;                                                                                                   \
        }                                                                                                              \
    } while (0)

using namespace DlssNr;

// Stands in for Config's CustomOptional in the PostLutEntries check.
template <typename T> struct Opt
{
    T v;
    T value_or_default() const { return v; }
};

// Config's post-NR stack fields, layer 2 switched off.
struct FakePostLutConfig
{
    static constexpr int kPostLutLayers = 4;
    Opt<std::string> DlssNrPostLutFile[4] { { "a.cube" }, { "b.cube" }, { "c.cube" }, { "" } };
    Opt<float> DlssNrPostLutStrength[4] { { 0.7f }, { 0.5f }, { 1.0f }, { 1.0f } };
    Opt<int> DlssNrPostLutInput[4] { { -1 }, { 3 }, { -1 }, { -1 } };
    Opt<bool> DlssNrPostLutEnabled[4] { { true }, { false }, { true }, { true } };
    Opt<bool> DlssNrPostLutStackEnabled { true };
};

// A lattice from a per-point function, with an optional domain.
template <typename F> static Lut3D Make(int n, F f, float lo = 0.0f, float hi = 1.0f)
{
    Lut3D l;
    l.size = n;
    for (int c = 0; c < 3; ++c)
    {
        l.domainMin[c] = lo;
        l.domainMax[c] = hi;
    }
    l.rgb.resize((size_t) n * n * n * 3);
    for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r)
            {
                const float in[3] = { lo + (hi - lo) * r / (n - 1), lo + (hi - lo) * g / (n - 1),
                                      lo + (hi - lo) * b / (n - 1) };
                float out[3];
                f(in, out);
                for (int c = 0; c < 3; ++c)
                    l.rgb[(((size_t) b * n + g) * n + r) * 3 + c] = out[c];
            }
    return l;
}

static Lut3D Identity(int n) { return Make(n, [](const float* i, float* o) { for (int c = 0; c < 3; ++c) o[c] = i[c]; }); }

// Deliberately non-linear: a strong gamma per channel with a cross-channel term.
static Lut3D Curvy(int n, float gamma, float mix)
{
    return Make(n, [=](const float* i, float* o)
                {
                    const float l = (i[0] + i[1] + i[2]) / 3.0f;
                    for (int c = 0; c < 3; ++c)
                        o[c] = std::pow(std::clamp(i[c] * (1 - mix) + l * mix, 0.0f, 1.0f), gamma);
                });
}

static float SequentialError(const std::vector<ChainLayer>& layers, const Lut3D& baked, double* meanOut)
{
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    float worst = 0.0f;
    double sum = 0.0;
    const int samples = 20000;
    for (int s = 0; s < samples; ++s)
    {
        const float in[3] = { u(rng), u(rng), u(rng) };
        float v[3] = { in[0], in[1], in[2] };
        for (const ChainLayer& layer : layers)
        {
            float next[3];
            ApplyLayer(*layer.lut, layer.strength, v, next);
            for (int c = 0; c < 3; ++c)
                v[c] = next[c];
        }
        float got[3];
        SampleLut(baked, in, got);
        for (int c = 0; c < 3; ++c)
        {
            const float e = std::fabs(got[c] - v[c]);
            worst = std::max(worst, e);
            sum += e;
        }
    }
    *meanOut = sum / (samples * 3.0);
    return worst;
}

int main(int argc, char** argv)
{
    std::string err;
    Lut3D out;
    float ps = 0.0f; // the strength the pass applies the bake with

    // Nothing to do: empty, null, zero strength.
    CHECK(!BakeChain({}, &out, &err, &ps));
    const Lut3D id17 = Identity(17);
    CHECK(!BakeChain({ { nullptr, 1.0f } }, &out, &err, &ps));
    CHECK(!BakeChain({ { &id17, 0.0f } }, &out, &err, &ps));
    CHECK(out.size == 0);

    // One layer at full strength is the file itself, bit for bit, whatever its size and domain.
    const Lut3D curvy33 = Curvy(33, 2.2f, 0.3f);
    CHECK(BakeChain({ { &curvy33, 1.0f } }, &out, &err, &ps));
    CHECK(out.size == 33 && out.rgb == curvy33.rgb && ps == 1.0f);
    const Lut3D dom = Make(9, [](const float* i, float* o) { for (int c = 0; c < 3; ++c) o[c] = i[c] * 0.5f; }, -0.25f, 1.25f);
    CHECK(BakeChain({ { &dom, 1.0f } }, &out, &err, &ps));
    CHECK(out.size == 9 && out.domainMin[0] == -0.25f && out.domainMax[2] == 1.25f && out.rgb == dom.rgb);

    // One layer at half strength is still the file itself, with the strength left to the pass: the single-LUT pass
    // exactly, also for display values outside the domain (HDR highlights above 1), which a blended lattice would clip.
    for (const Lut3D* lut : { &curvy33, &dom })
    {
        CHECK(BakeChain({ { lut, 0.5f } }, &out, &err, &ps));
        CHECK(out.rgb == lut->rgb && ps == 0.5f);
        std::mt19937 rng(3);
        std::uniform_real_distribution<float> u(lut->domainMin[0] - 0.5f, lut->domainMax[0] + 0.5f);
        float worst = 0.0f;
        for (int s = 0; s < 5000; ++s)
        {
            const float in[3] = { u(rng), u(rng), u(rng) };
            float want[3], got[3];
            ApplyLayer(*lut, 0.5f, in, want);
            ApplyLayer(out, ps, in, got);
            for (int c = 0; c < 3; ++c)
                worst = std::max(worst, std::fabs(got[c] - want[c]));
        }
        CHECK(worst == 0.0f);
    }

    // An identity layer anywhere in a stack changes nothing: the result is the other layer.
    {
        const Lut3D a = Curvy(33, 1.8f, 0.0f);
        CHECK(BakeChain({ { &id17, 1.0f }, { &a, 1.0f }, { &id17, 1.0f } }, &out, &err, &ps));
        CHECK(out.size == 33);
        double mean = 0.0;
        const float worst = SequentialError({ { &a, 1.0f } }, out, &mean);
        printf("identity-wrapped single: worst %.5f mean %.6f\n", worst, mean);
        CHECK(worst < 2e-3f);
    }

    // Two non-linear layers: the baked lattice follows sequential application. The bound is what a 33 or 65 lattice
    // can do for a gamma pair; printed so it can be judged.
    {
        const Lut3D a = Curvy(33, 2.2f, 0.3f);
        const Lut3D b = Curvy(33, 0.6f, 0.0f);
        CHECK(BakeChain({ { &a, 1.0f }, { &b, 0.7f } }, &out, &err, &ps));
        CHECK(out.size == 33);
        double mean = 0.0;
        const float worst = SequentialError({ { &a, 1.0f }, { &b, 0.7f } }, out, &mean);
        printf("2 layers @33: worst %.5f mean %.6f\n", worst, mean);
        CHECK(worst < 0.02f);
        CHECK(mean < 0.002);

        const Lut3D a65 = Curvy(65, 2.2f, 0.3f);
        CHECK(BakeChain({ { &a65, 1.0f }, { &b, 0.7f } }, &out, &err, &ps));
        CHECK(out.size == 65);
        const float worst65 = SequentialError({ { &a65, 1.0f }, { &b, 0.7f } }, out, &mean);
        printf("2 layers @65: worst %.5f mean %.6f\n", worst65, mean);
        CHECK(worst65 < worst);
    }

    // Mixed sizes and a domain: result is clamped into 17..65, 4 layers allowed, a fifth is not.
    {
        const Lut3D tiny = Curvy(5, 1.4f, 0.1f);
        CHECK(BakeChain({ { &tiny, 1.0f }, { &dom, 1.0f } }, &out, &err, &ps));
        CHECK(out.size == kChainMinSize && ps == 1.0f);
        // The values too: the -0.25..1.25 domain layer is mapped right inside the stack.
        double mean = 0.0;
        const float worstDom = SequentialError({ { &tiny, 1.0f }, { &dom, 1.0f } }, out, &mean);
        printf("tiny + domain layer @17: worst %.5f mean %.6f\n", worstDom, mean);
        CHECK(worstDom < 0.02f);
        CHECK(BakeChain({ { &dom, 0.6f }, { &tiny, 1.0f } }, &out, &err, &ps));
        CHECK(SequentialError({ { &dom, 0.6f }, { &tiny, 1.0f } }, out, &mean) < 0.02f);

        // A non-linear layer over -0.25..1.25 in a stack: the domain map is what makes this close (sampling it as if
        // its domain were 0..1 puts every point a quarter of the way off).
        const Lut3D domCurve = Make(
            33, [](const float* i, float* o) { for (int c = 0; c < 3; ++c) o[c] = std::pow(std::clamp(i[c], 0.0f, 1.25f) / 1.25f, 2.0f); },
            -0.25f, 1.25f);
        const Lut3D gamma = Curvy(33, 0.8f, 0.2f);
        CHECK(BakeChain({ { &domCurve, 1.0f }, { &gamma, 1.0f } }, &out, &err, &ps));
        const float worstCurve = SequentialError({ { &domCurve, 1.0f }, { &gamma, 1.0f } }, out, &mean);
        printf("non-linear domain layer + gamma @33: worst %.5f mean %.6f\n", worstCurve, mean);
        CHECK(worstCurve < 0.02f && worstCurve > 0.0f);
        Lut3D asIf01 = domCurve;
        for (int c = 0; c < 3; ++c)
        {
            asIf01.domainMin[c] = 0.0f;
            asIf01.domainMax[c] = 1.0f;
        }
        CHECK(SequentialError({ { &asIf01, 1.0f }, { &gamma, 1.0f } }, out, &mean) > 0.05f);
        const Lut3D big = Identity(128);
        CHECK(BakeChain({ { &big, 1.0f }, { &tiny, 1.0f } }, &out, &err, &ps));
        CHECK(out.size == kChainMaxSize);
        CHECK(BakeChain({ { &tiny, 1.0f }, { &tiny, 1.0f }, { &tiny, 1.0f }, { &tiny, 1.0f } }, &out, &err, &ps));
        CHECK(!BakeChain({ { &tiny, 1.0f }, { &tiny, 1.0f }, { &tiny, 1.0f }, { &tiny, 1.0f }, { &tiny, 1.0f } }, &out, &err, &ps));
        CHECK(out.size == 0 && !err.empty());
    }

    // The state: parse, bake once per change, keep the last good chain when a layer fails.
    {
        const auto write = [](const char* name, const Lut3D& l)
        {
            const std::string path =
                (std::filesystem::temp_directory_path() / (std::string("nr_lut_chain_smoke_") + name + ".cube")).string();
            std::ofstream f(path);
            f << "LUT_3D_SIZE " << l.size << "\n";
            for (size_t i = 0; i < l.rgb.size(); i += 3)
                f << l.rgb[i] << " " << l.rgb[i + 1] << " " << l.rgb[i + 2] << "\n";
            return path;
        };
        const std::string pa = write("a", Curvy(17, 2.0f, 0.2f));
        const std::string pb = write("b", Curvy(17, 0.7f, 0.0f));
        const std::string bad = (std::filesystem::temp_directory_path() / "nr_lut_chain_smoke_missing.cube").string();

        DlssNr_LutChainState st;
        CHECK(!DlssNr_LutChainEnsure(&st, {}));
        CHECK(!st.wanted && !st.Loaded());

        CHECK(DlssNr_LutChainEnsure(&st, { { pa, 1.0f } }));
        CHECK(st.wanted && st.baked.size == 17 && st.failedLayer < 0 && st.strength == 1.0f);

        // A single layer's strength goes to the pass: no re-bake, so no re-upload.
        const std::string key1 = st.key;
        CHECK(DlssNr_LutChainEnsure(&st, { { pa, 0.4f } }));
        CHECK(st.key == key1 && st.strength == 0.4f);
        CHECK(st.description.find("baked") == std::string::npos && st.description.find("_a.cube (17^3)") != std::string::npos);

        // The one active layer moving to another entry is another LUT: baked again.
        CHECK(DlssNr_LutChainEnsure(&st, { { pa, 0.5f }, { pb, 0.0f } }));
        const std::vector<float> onlyA = st.baked.rgb;
        CHECK(DlssNr_LutChainEnsure(&st, { { pa, 0.0f }, { pb, 0.5f } }));
        CHECK(st.baked.rgb != onlyA && st.strength == 0.5f && st.description.find("_b.cube") != std::string::npos);

        // Every layer at 0: nothing applied, and said why.
        CHECK(!DlssNr_LutChainEnsure(&st, { { pa, 0.0f } }));
        CHECK(!st.error.empty());

        // The file re-exported in place is read again within a time-check period, and only then.
        CHECK(DlssNr_LutChainEnsure(&st, { { pa, 1.0f } }));
        const std::vector<float> before = st.baked.rgb;
        const auto written = std::filesystem::last_write_time(pa);
        write("a", Curvy(17, 1.3f, 0.0f));
        std::filesystem::last_write_time(pa, written + std::chrono::seconds(5));
        int calls = 0;
        while (st.baked.rgb == before && calls < 200)
        {
            DlssNr_LutChainEnsure(&st, { { pa, 1.0f } });
            ++calls;
        }
        printf("re-exported file picked up after %d calls\n", calls);
        CHECK(st.baked.rgb != before && calls <= 61);
        write("a", Curvy(17, 2.0f, 0.2f));
        std::filesystem::last_write_time(pa, written + std::chrono::seconds(10));
        CHECK(DlssNr_LutChainEnsure(&st, {}) == false); // forget it, so the stack below starts from the file as written

        CHECK(DlssNr_LutChainEnsure(&st, { { pa, 1.0f }, { pb, 0.5f } }));
        const std::vector<float> two = st.baked.rgb;
        const std::string key2 = st.key;
        CHECK(DlssNr_LutChainEnsure(&st, { { pa, 1.0f }, { pb, 0.5f } })); // unchanged: not baked again
        CHECK(st.key == key2 && st.baked.rgb == two);
        CHECK(st.strength == 1.0f && st.description.find(" -> ") != std::string::npos &&
              st.description.find("baked at 17^3") != std::string::npos);

        // A strength change in a stack is in the bake: baked again.
        CHECK(DlssNr_LutChainEnsure(&st, { { pa, 1.0f }, { pb, 0.6f } }));
        CHECK(st.key != key2 && st.baked.rgb != two);
        CHECK(DlssNr_LutChainEnsure(&st, { { pa, 1.0f }, { pb, 0.5f } }));
        CHECK(st.baked.rgb == two);

        // A layer at 0 is not in the bake, nor in the description.
        CHECK(DlssNr_LutChainEnsure(&st, { { pa, 1.0f }, { pb, 0.0f } }));
        CHECK(st.description.find("_b.cube") == std::string::npos && st.strength == 1.0f);
        CHECK(DlssNr_LutChainEnsure(&st, { { pa, 1.0f }, { pb, 0.5f } }));

        // A layer that cannot be read: the chain that worked stays, the failure is named.
        CHECK(DlssNr_LutChainEnsure(&st, { { pa, 1.0f }, { bad, 0.5f } }));
        CHECK(st.failedLayer == 1 && st.failedPath == bad && !st.error.empty() && st.baked.rgb == two);

        // Order matters: swapping the layers changes the bake.
        CHECK(DlssNr_LutChainEnsure(&st, { { pb, 0.5f }, { pa, 1.0f } }));
        CHECK(st.failedLayer < 0 && st.baked.rgb != two);

        // Emptied: everything released.
        CHECK(!DlssNr_LutChainEnsure(&st, {}));
        CHECK(!st.wanted && st.baked.size == 0 && st.key.empty());

        // Switches and an unreadable file together: the grade follows the switches, whatever the file does.
        {
            DlssNr_LutChainState s;
            CHECK(DlssNr_LutChainEnsure(&s, { { pa, 1.0f } }));
            const std::vector<float> onlyFileA = s.baked.rgb;

            // The file appears while the rest is as it was: A stays, the failure is named.
            CHECK(DlssNr_LutChainEnsure(&s, { { pa, 1.0f }, { bad, 1.0f } }));
            CHECK(s.failedLayer == 1 && s.baked.rgb == onlyFileA);
            CHECK(DlssNr_LutChainEnsure(&s, { { pa, 1.0f }, { bad, 1.0f } })); // steady: still kept
            CHECK(s.failedLayer == 1 && s.baked.rgb == onlyFileA);

            // Everything off: nothing is applied and nothing warns about the file.
            CHECK(!DlssNr_LutChainEnsure(&s, { { pa, 0.0f }, { bad, 0.0f } }));
            CHECK(s.baked.size == 0 && s.failedLayer < 0);

            // On again: back, with the failure named again.
            CHECK(DlssNr_LutChainEnsure(&s, { { pa, 1.0f }, { bad, 1.0f } }));
            CHECK(s.baked.rgb == onlyFileA && s.failedLayer == 1);

            // A switched off while the broken file stays on: A must go.
            CHECK(!DlssNr_LutChainEnsure(&s, { { pa, 0.0f }, { bad, 1.0f } }));
            CHECK(s.baked.size == 0 && s.failedLayer == 1 && !s.error.empty());

            // The broken file switched off: it does not block A, and does not warn.
            CHECK(DlssNr_LutChainEnsure(&s, { { pa, 1.0f }, { bad, 0.0f } }));
            CHECK(s.baked.rgb == onlyFileA && s.failedLayer < 0 && s.error.empty());

            // Another layer switched while one is broken: the switch takes effect, the broken one is left out.
            CHECK(DlssNr_LutChainEnsure(&s, { { pa, 1.0f }, { bad, 1.0f }, { pb, 1.0f } }));
            CHECK(s.failedLayer == 1 && s.baked.rgb != onlyFileA);
            CHECK(DlssNr_LutChainEnsure(&s, { { pa, 1.0f }, { bad, 1.0f }, { pb, 0.0f } }));
            CHECK(s.failedLayer == 1 && s.baked.rgb == onlyFileA);

            // From a fresh start the layers that can be read are applied, and the failure is named.
            DlssNr_LutChainState fresh;
            CHECK(DlssNr_LutChainEnsure(&fresh, { { pa, 1.0f }, { bad, 1.0f } }));
            CHECK(fresh.baked.rgb == onlyFileA && fresh.failedLayer == 1);

            // Only the broken file: nothing applied, its failure is the message (not "every LUT is off").
            DlssNr_LutChainState onlyBad;
            CHECK(!DlssNr_LutChainEnsure(&onlyBad, { { bad, 1.0f } }));
            CHECK(onlyBad.failedLayer == 0 && onlyBad.error.find("every LUT") == std::string::npos);

            // Off and on again with no broken file: the same key, so the kept GPU texture is what comes back.
            DlssNr_LutChainState toggled;
            CHECK(DlssNr_LutChainEnsure(&toggled, { { pa, 1.0f }, { pb, 0.5f } }));
            const std::string keyOn = toggled.key;
            CHECK(!DlssNr_LutChainEnsure(&toggled, { { pa, 0.0f }, { pb, 0.0f } }));
            CHECK(toggled.baked.size == 0 && toggled.key != keyOn);
            CHECK(DlssNr_LutChainEnsure(&toggled, { { pa, 1.0f }, { pb, 0.5f } }));
            CHECK(toggled.key == keyOn);
        }

        std::filesystem::remove(pa);
        std::filesystem::remove(pb);
    }

    // A layer switched off: PostLutEntries hands it on at strength 0 in its own place, so it is skipped by the bake and
    // the layers after it keep their index; switched on again it is back at its own strength.
    {
        FakePostLutConfig cfg;
        auto e = PostLutEntries(cfg);
        CHECK(e.size() == 4 && e[0].strength == 0.7f && e[1].path == "b.cube" && e[1].strength == 0.0f &&
              e[1].input == 3 && e[2].path == "c.cube" && e[2].strength == 1.0f);
        cfg.DlssNrPostLutEnabled[1].v = true;
        e = PostLutEntries(cfg);
        CHECK(e[1].strength == 0.5f);

        // The whole stack off: every layer at 0, paths and look inputs kept; on again, each back at its own strength.
        cfg.DlssNrPostLutStackEnabled.v = false;
        e = PostLutEntries(cfg);
        CHECK(e[0].strength == 0.0f && e[1].strength == 0.0f && e[2].strength == 0.0f && e[0].path == "a.cube" &&
              e[1].input == 3);
        cfg.DlssNrPostLutStackEnabled.v = true;
        e = PostLutEntries(cfg);
        CHECK(e[0].strength == 0.7f && e[1].strength == 0.5f && e[2].strength == 1.0f);

        cfg.DlssNrPostLutEnabled[0].v = cfg.DlssNrPostLutEnabled[1].v = cfg.DlssNrPostLutEnabled[2].v = false;
        e = PostLutEntries(cfg);
        CHECK(e[0].strength == 0.0f && e[1].strength == 0.0f && e[2].strength == 0.0f);
    }

    // Optional: every pair of real LUTs in a folder.
    if (argc > 1)
    {
        std::vector<Lut3D> luts;
        std::vector<std::string> names;
        for (const auto& e : std::filesystem::directory_iterator(argv[1]))
        {
            std::string ext = e.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char) std::tolower(c); });
            if (ext == ".cube")
            {
                Lut3D l;
                std::string perr;
                if (ParseCube(e.path().string(), &l, &perr))
                {
                    luts.push_back(std::move(l));
                    names.push_back(e.path().filename().string());
                }
            }
        }
        float worstAll = 0.0f;
        double meanSum = 0.0;
        int pairs = 0;
        std::string worstName;
        for (size_t i = 0; i < luts.size(); ++i)
            for (size_t j = 0; j < luts.size(); j += std::max<size_t>(1, luts.size() / 6))
            {
                std::vector<ChainLayer> layers = { { &luts[i], 1.0f }, { &luts[j], 0.8f } };
                Lut3D baked;
                if (!BakeChain(layers, &baked, &err, &ps))
                    continue;
                double mean = 0.0;
                const float worst = SequentialError(layers, baked, &mean);
                meanSum += mean;
                ++pairs;
                if (worst > worstAll)
                {
                    worstAll = worst;
                    worstName = names[i] + " + " + names[j];
                }
            }
        printf("real LUTs: %zu files, %d pairs, worst %.5f (%s), mean of means %.6f\n", luts.size(), pairs, worstAll,
               worstName.c_str(), pairs ? meanSum / pairs : 0.0);
    }

    printf(fails ? "FAILED (%d)\n" : "PASS\n", fails);
    return fails ? 1 : 0;
}
