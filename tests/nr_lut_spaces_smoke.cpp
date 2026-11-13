// Host test for the post-NR stack's look input (DlssNr_LutSpaces.h + DlssNr_LutChain.h, post-NR LUT epic Stories 6-7).
// Everything up to 8 runs with no files; the pack checks run when its folder is given.
//
//   1. Each log curve matches its vendor's published values (mid grey, and black / 90% where the vendor tables give them).
//   2. Every gamut's RGB -> XYZ matches its vendor's published matrix (BT.709, AWG3, AWG4, AP1, S-Gamut3.Cine, V-Gamut,
//      DWG), and BT.709 -> AP1 matches ACES's Bradford-adapted matrix.
//   3. Frame decodes: FrameDecodeFor's order (PQ, then Linear HDR, then gamma 2.2); Linear HDR undoes the pass's
//      sRGB(Neutwo(light)); SDR and gamma 2.2 keep mid grey and send white to 16; PQ is the plain sRGB decode.
//   4. Auto detection: names (whole words: "Analog ..." and "... Vlog ..." are not log spaces), header lines (a gamut
//      other than the curve's own is kept, linear light in an unknown gamut is not taken), ini words.
//   5. ResolveLookInputs: conversion only from the picture, "from the LUT above", a mismatch, None and a hand-set
//      space, a strength-0 layer, and a stack that ends in a log signal.
//   6. Linear HDR highlights: a look on Auto, baked, matches the exact conversion + look from 0.01x to 50x white
//      (the log-spaced lattice); the same lattice laid out evenly does not (shows the check can fail).
//   7. Ensure with real files (written to the temp folder): a single converted look keeps its strength in the pass;
//      a setting change re-bakes, Auto -> the same space by hand does not but still updates the menu's text; a
//      frame-decode change re-bakes only a converting stack; a failed layer clears the menu's inputs; "nan" rows are
//      refused.
//   8. Old stacks: a creative LUT is never converted and keeps the single-layer shortcut.
//   9. With the pack: LogC4 AWG4 vs the pack's own LinRec709-to-ArriLogC4AWG4.cube (to that file's own error, ~0.0025
//      at mid grey); every ARRILogC4AWG4-to-* look reads LogC4 AWG4 from its header; a pack look on Auto matches the
//      exact conversion + look on Linear HDR and SDR, and the pack's LinRec709 file + look on Auto does too, to the
//      pack's own error (its shaper is read past 1: a clip there would put highlights ~0.1 off).
//
// Build: cl /nologo /std:c++20 /EHsc /W4 tests\nr_lut_spaces_smoke.cpp
// Run:   nr_lut_spaces_smoke.exe [<pack folder>]   (the AgX/Filmic view-transform pack's LUTs folder)
#include "../OptiScaler/dlssnr/DlssNr_LutChain.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

using namespace DlssNr;

static int fails = 0;
static void expect(bool ok, const std::string& what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what.c_str());
        ++fails;
    }
}

static float Curve(LutSpace s, float x)
{
    const float light[3] = { x, x, x };
    float out[3];
    EncodeLightToSpace(s, light, out);
    return out[1];
}

static std::string Name(LutSpace s) { return LutSpaceName(s); }

static float ToSrgb(float v)
{
    v = std::clamp(v, 0.0f, 1.0f);
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

// The stack's display value for Linear HDR light: what dlssnr_lut.hlsl hands it (sRGB of Neutwo on the peak channel).
static void HandedLinearHdr(const float light[3], float out[3])
{
    const float m = std::max({ light[0], light[1], light[2] });
    const float scale = m > 1e-6f ? (m / std::sqrt(m * m + 1.0f)) / m : 1.0f;
    for (int c = 0; c < 3; ++c)
        out[c] = ToSrgb(light[c] * scale);
}

// A look made for LogC4 input: a smooth S-curve per channel, steepest across the highlights (LogC4 0.45 is ~2.5x white),
// so a lattice that resolves highlights badly shows it.
static Lut3D SyntheticLook(int n)
{
    Lut3D lut;
    lut.size = n;
    lut.rgb.resize((size_t) n * n * n * 3);
    for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r)
            {
                const int idx[3] = { r, g, b };
                for (int c = 0; c < 3; ++c)
                {
                    const float v = (float) idx[c] / (float) (n - 1);
                    lut.rgb[(((size_t) b * n + g) * n + r) * 3 + c] = 1.0f / (1.0f + std::exp(-10.0f * (v - 0.45f)));
                }
            }
    lut.look.in = lut.look.inGamut = LutSpace::ArriLogC4;
    lut.look.out = LutSpace::Display;
    lut.look.inFrom = "header";
    return lut;
}

// A .cube in the temp folder: optional header lines, an n^3 lattice of f(r, g, b).
static std::string WriteCube(const std::string& name, const std::string& header, int n,
                             const std::function<void(float, float, float, float*)>& f)
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "nr_lut_spaces_smoke" / name;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path);
    out << header << "LUT_3D_SIZE " << n << "\n";
    for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r)
            {
                float v[3];
                f(r / float(n - 1), g / float(n - 1), b / float(n - 1), v);
                out << v[0] << " " << v[1] << " " << v[2] << "\n";
            }
    return path.string();
}

static bool Bake(const std::vector<ChainEntry>& entries, FrameDecode decode, DlssNr_LutChainState* state)
{
    return DlssNr_LutChainEnsure(state, entries, decode);
}

// Worst difference between `baked` sampled at the handed display value and the exact chain, over Linear HDR lights.
static float WorstVsExact(const Lut3D& baked, const Lut3D& look, FrameDecode decode, LutSpace space, char* where,
                          size_t whereSize)
{
    const float lights[][3] = { { 0.01f, 0.01f, 0.01f }, { 0.18f, 0.18f, 0.18f }, { 1, 1, 1 },     { 2, 2, 2 },
                                { 4, 4, 4 },             { 6, 6, 6 },             { 10, 10, 10 }, { 20, 20, 20 },
                                { 50, 50, 50 },          { 8, 2, 0.5f },          { 0.5f, 3, 12 }, { 0.6f, 0.2f, 0.03f } };
    float worst = 0.0f;
    for (const auto& light : lights)
    {
        float handed[3], fromBake[3], exact[3];
        if (decode == FrameDecode::LinearHdr)
            HandedLinearHdr(light, handed);
        else
            for (int c = 0; c < 3; ++c)
                handed[c] = ToSrgb(light[c] / 16.0f); // SDR: a picture up to white
        SampleLut(baked, handed, fromBake);
        ApplyConvertedLayer(look, 1.0f, decode, space, space, handed, exact);
        for (int c = 0; c < 3; ++c)
            if (std::abs(fromBake[c] - exact[c]) > worst)
            {
                worst = std::abs(fromBake[c] - exact[c]);
                std::snprintf(where, whereSize, "at light (%g %g %g): %.4f vs exact %.4f", light[0], light[1], light[2],
                              fromBake[c], exact[c]);
            }
    }
    return worst;
}

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // 1. Published values (colour-science's doctests quote the same vendor numbers).
    struct Ref
    {
        LutSpace space;
        float x, want, tolerance;
    };
    const Ref refs[] = {
        { LutSpace::ArriLogC3, 0.18f, 0.3910068f, 2e-6f },
        { LutSpace::ArriLogC3, 0.0f, 0.092809f, 1e-6f },
        { LutSpace::ArriLogC4, 0.18f, 0.2783958f, 2e-6f },
        { LutSpace::ArriLogC4, 0.0f, 95.0f / 1023.0f, 1e-6f },
        { LutSpace::AcesCct, 0.18f, 0.4135884f, 2e-6f },
        { LutSpace::AcesCct, 0.0f, 0.0729055f, 1e-6f },
        { LutSpace::SLog3, 0.18f, 0.4105571f, 2e-6f },
        { LutSpace::SLog3, 0.0f, 95.0f / 1023.0f, 1e-6f },
        { LutSpace::SLog3, 0.9f, 598.0f / 1023.0f, 0.6f / 1023.0f },
        { LutSpace::VLog, 0.18f, 0.4233114f, 2e-6f },
        { LutSpace::VLog, 0.0f, 128.0f / 1023.0f, 0.6f / 1023.0f },
        { LutSpace::VLog, 0.9f, 602.0f / 1023.0f, 0.6f / 1023.0f },
        { LutSpace::DaVinciIntermediate, 0.18f, 0.3360432f, 2e-6f },
        { LutSpace::DaVinciIntermediate, 0.0f, 0.0f, 1e-7f },
    };
    for (const Ref& r : refs)
    {
        const float got = Curve(r.space, r.x);
        expect(std::abs(got - r.want) <= r.tolerance, Name(r.space) + " at " + std::to_string(r.x) + ": " +
                                                          std::to_string(got) + ", want " + std::to_string(r.want));
    }
    std::puts("1. curves match the published values");

    // 2. Gamuts: RGB -> XYZ from the primaries vs the vendors' published matrices; BT.709 -> AP1 vs ACES's (Bradford).
    {
        struct Published
        {
            LutSpace gamut;
            float m[3][3];
        };
        const Published published[] = {
            { LutSpace::LinearRec709,
              { { 0.4123908f, 0.3575843f, 0.1804808f },
                { 0.2126390f, 0.7151687f, 0.0721923f },
                { 0.0193308f, 0.1191948f, 0.9505322f } } },
            { LutSpace::ArriLogC3,
              { { 0.638008f, 0.214704f, 0.097744f },
                { 0.291954f, 0.823841f, -0.115795f },
                { 0.002798f, -0.067034f, 1.153294f } } },
            { LutSpace::ArriLogC4,
              { { 0.7048583f, 0.1297603f, 0.1158373f },
                { 0.2545242f, 0.7814777f, -0.0360019f },
                { 0.0f, 0.0f, 1.0890578f } } },
            { LutSpace::AcesCct,
              { { 0.6624542f, 0.1340042f, 0.1561877f },
                { 0.2722287f, 0.6740818f, 0.0536895f },
                { -0.0055746f, 0.0040607f, 1.0103391f } } },
            { LutSpace::SLog3,
              { { 0.5990839f, 0.2489255f, 0.1024465f },
                { 0.2150758f, 0.8850685f, -0.1001443f },
                { -0.0320658f, -0.0276584f, 1.1487820f } } },
            { LutSpace::VLog,
              { { 0.679644f, 0.152211f, 0.118600f },
                { 0.260686f, 0.774894f, -0.035580f },
                { -0.009310f, -0.004612f, 1.102980f } } },
            { LutSpace::DaVinciIntermediate,
              { { 0.7006224f, 0.1487748f, 0.1010587f },
                { 0.2741185f, 0.8736319f, -0.1477504f },
                { -0.0989629f, -0.1378953f, 1.3259160f } } },
        };
        float worst = 0.0f;
        for (const Published& p : published)
        {
            const LutSpaces::Gamut& g = LutSpaces::GamutOf(p.gamut);
            LutSpaces::Mat3 m;
            LutSpaces::RgbToXyz(g.xy, g.white, m);
            float off = 0.0f;
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    off = std::max(off, std::abs(m[r][c] - p.m[r][c]));
            worst = std::max(worst, off);
            expect(off < 2e-5f, LutGamutName(p.gamut) + std::string(" RGB->XYZ differs from the published matrix by ") +
                                    std::to_string(off));
        }

        const float acesFrom709[3][3] = { { 0.6130974f, 0.3395231f, 0.0473795f },
                                          { 0.0701937f, 0.9163539f, 0.0134524f },
                                          { 0.0206156f, 0.1095698f, 0.8698147f } };
        LutSpaces::Mat3 m;
        LutSpaces::FromRec709Matrix(LutSpace::AcesCct, m);
        float off = 0.0f;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                off = std::max(off, std::abs(m[r][c] - acesFrom709[r][c]));
        std::printf("2. gamuts: worst %.1e vs published RGB->XYZ, BT.709->AP1 (Bradford) off %.1e\n", worst, off);
        expect(off < 1e-4f, "BT.709 -> AP1 is not ACES's Bradford-adapted matrix");
    }

    // 3. Frame decodes.
    {
        expect(FrameDecodeFor(4, true) == FrameDecode::Pq && FrameDecodeFor(3, true) == FrameDecode::LinearHdr &&
                   FrameDecodeFor(3, false) == FrameDecode::Gamma22 && FrameDecodeFor(0, false) == FrameDecode::Srgb &&
                   FrameDecodeFor(1, false) == FrameDecode::Srgb,
               "FrameDecodeFor does not follow the shader's order (PQ, Linear HDR, gamma 2.2, else sRGB)");

        float worst = 0.0f;
        const float lights[][3] = { { 0.18f, 0.18f, 0.18f }, { 1, 1, 1 },     { 4, 4, 4 },
                                    { 0.6f, 0.2f, 0.03f },   { 12, 3, 0.5f }, { 0.01f, 0.02f, 0.05f } };
        for (const auto& light : lights)
        {
            float handed[3], back[3];
            HandedLinearHdr(light, handed);
            DecodeFrameToLight(FrameDecode::LinearHdr, handed, back);
            for (int c = 0; c < 3; ++c)
                worst = std::max(worst, std::abs(back[c] - light[c]) / std::max(light[c], 0.01f));
        }
        std::printf("3. Linear HDR decode: worst relative error %.2e\n", worst);
        expect(worst < 2e-3f, "Linear HDR decode does not undo the pass's sRGB(Neutwo)");

        const float grey[3] = { 0.4613561f, 0.4613561f, 0.4613561f }; // sRGB of 0.18
        const float white[3] = { 1, 1, 1 };
        float g[3], w[3];
        DecodeFrameToLight(FrameDecode::Srgb, grey, g);
        DecodeFrameToLight(FrameDecode::Srgb, white, w);
        std::printf("   SDR: mid grey -> %.4f, white -> %.2f\n", g[1], w[1]);
        expect(std::abs(g[1] - 0.18f) < 0.005f, "SDR decode moves mid grey by more than 0.04 EV");
        expect(std::abs(w[1] - kSdrWhiteLight) < 1e-3f, "SDR decode does not send white to kSdrWhiteLight");

        const float grey22 = std::pow(0.18f, 1.0f / 2.2f);
        const float g22[3] = { grey22, grey22, grey22 };
        DecodeFrameToLight(FrameDecode::Gamma22, g22, g);
        DecodeFrameToLight(FrameDecode::Gamma22, white, w);
        expect(std::abs(g[1] - 0.18f) < 0.005f && std::abs(w[1] - kSdrWhiteLight) < 1e-3f,
               "gamma 2.2 decode does not read 2.2 or does not lift like SDR");

        DecodeFrameToLight(FrameDecode::Pq, grey, g);
        DecodeFrameToLight(FrameDecode::Pq, white, w);
        expect(std::abs(g[1] - 0.18f) < 1e-4f && std::abs(w[1] - 1.0f) < 1e-6f,
               "PQ decode is not the plain sRGB decode (the pass already put it on paper white)");
    }

    // 4. Names, headers, ini words.
    {
        struct NameCase
        {
            const char* stem;
            LutSpace in, out;
            const char* from;
        };
        const NameCase names[] = {
            { "ARRILogC4AWG4-to-AgX-Blender-sRGB", LutSpace::ArriLogC4, LutSpace::Display, "name" },
            { "LinRec709-to-ArriLogC4AWG4", LutSpace::LinearRec709, LutSpace::ArriLogC4, "name" },
            { "SLog3_to_Rec709_Kodak2383", LutSpace::SLog3, LutSpace::Display, "name" },
            { "VLog Film Look", LutSpace::VLog, LutSpace::Display, "name" },
            { "V-Log_to_Rec709", LutSpace::VLog, LutSpace::Display, "name" },
            { "LogC Film", LutSpace::ArriLogC3, LutSpace::Display, "name" },
            { "ACEScct-to-sRGB", LutSpace::AcesCct, LutSpace::Display, "name" },
            { "DaVinci Intermediate Look", LutSpace::DaVinciIntermediate, LutSpace::Display, "name" },
            { "Lakers_Broadcast_33", LutSpace::Display, LutSpace::Display, "" },
            { "LinearHDR-to-ArriLogC4AWG4", LutSpace::Display, LutSpace::ArriLogC4, "" },
            { "sRGB-Expand16-to-ArriLogC4AWG4", LutSpace::Display, LutSpace::ArriLogC4, "" },
            { "Sports-Broadcast-sRGB", LutSpace::Display, LutSpace::Display, "" },
            // ordinary creative names that only contain a log name's letters (Review Pass 2026-10-31)
            { "Analog Cinema", LutSpace::Display, LutSpace::Display, "" },
            { "Vintage Analog Cool", LutSpace::Display, LutSpace::Display, "" },
            { "Travel Vlog Warm", LutSpace::Display, LutSpace::Display, "" },
            { "DevLog Night", LutSpace::Display, LutSpace::Display, "" },
            { "Travlog Summer", LutSpace::Display, LutSpace::Display, "" },
            { "Backdwgreen", LutSpace::Display, LutSpace::Display, "" },
            { "Catalog Crisp", LutSpace::Display, LutSpace::Display, "" },
        };
        for (const NameCase& c : names)
        {
            const LookSpaces got = DetectLookSpaces(c.stem, "", "", "", "");
            expect(got.in == c.in && got.out == c.out && std::string(got.inFrom) == c.from,
                   std::string("name ") + c.stem + ": in " + Name(got.in) + " (" + got.inFrom + "), out " +
                       Name(got.out));
        }

        struct HeaderCase
        {
            const char *gamma, *gamut;
            bool found;
            LutSpace space, gamutSpace;
        };
        const HeaderCase headers[] = {
            { "Arri LogC4", "Arri Wide Gamut 4", true, LutSpace::ArriLogC4, LutSpace::ArriLogC4 },
            { "Arri LogC4", "Rec709", true, LutSpace::ArriLogC4, LutSpace::LinearRec709 },
            { "S-Log3", "S-Gamut3.Cine", true, LutSpace::SLog3, LutSpace::SLog3 },
            { "LogC3", "", true, LutSpace::ArriLogC3, LutSpace::ArriLogC3 },
            { "Linear", "Rec709", true, LutSpace::LinearRec709, LutSpace::LinearRec709 },
            { "Linear", "ACES AP0", false, LutSpace::Display, LutSpace::Display },
            { "Gamma 2.2 (Display)", "Rec.709", true, LutSpace::Display, LutSpace::Display },
            { "", "Rec709", false, LutSpace::Display, LutSpace::Display },
            { "Analog Cinema", "", false, LutSpace::Display, LutSpace::Display },
        };
        for (const HeaderCase& h : headers)
        {
            LutSpace space = LutSpace::Display, gamut = LutSpace::Display;
            const bool found = SpaceFromHeader(h.gamma, h.gamut, &space, &gamut);
            expect(found == h.found && (!found || (space == h.space && gamut == h.gamutSpace)),
                   std::string("header '") + h.gamma + "' / '" + h.gamut + "': " + (found ? "found " : "not found ") +
                       Name(space) + " in " + LutGamutName(gamut));
        }

        // The ini words (PostLutInput1-4) round-trip; anything unknown is Auto.
        for (int i = kLookInputAuto; i < (int) LutSpace::Count; ++i)
            expect(LookInputFromKey(LookInputKey(i)) == i,
                   std::string("ini word ") + LookInputKey(i) + " does not round-trip");
        expect(LookInputFromKey("LogC4") == (int) LutSpace::ArriLogC4 && LookInputFromKey("logc5") == kLookInputAuto &&
                   LookInputFromKey("") == kLookInputAuto,
               "ini words are not read case-blind with Auto for anything else");
        std::puts("4. names, headers and ini words resolve");
    }

    // 5. ResolveLookInputs over in-memory layers.
    {
        const auto lut = [](LutSpace in, LutSpace out)
        {
            Lut3D l;
            l.size = 2;
            l.rgb.assign(24, 0.5f);
            l.look.in = l.look.inGamut = in;
            l.look.out = out;
            l.look.inFrom = in == LutSpace::Display ? "" : "header";
            return l;
        };
        const Lut3D look = lut(LutSpace::ArriLogC4, LutSpace::Display);       // ARRILogC4AWG4-to-...-sRGB
        const Lut3D toLog = lut(LutSpace::LinearRec709, LutSpace::ArriLogC4); // LinRec709-to-ArriLogC4AWG4
        const Lut3D shaped = lut(LutSpace::Display, LutSpace::ArriLogC4);     // a hand-made picture-to-LogC4 file
        const Lut3D creative = lut(LutSpace::Display, LutSpace::Display);

        struct Case
        {
            const char* what;
            std::vector<const Lut3D*> luts;
            std::vector<ChainEntry> entries;
            std::vector<LayerInput> want; // space, gamut, from, converted, given
            LutSpace endsIn;
        };
        const auto e = [](float strength = 1.0f, int input = kLookInputAuto) { return ChainEntry { "x", strength, input }; };
        const auto in = [](LutSpace space, const char* from, bool converted, LutSpace given)
        { return LayerInput { space, space, from, converted, given }; };
        const LutSpace D = LutSpace::Display, L4 = LutSpace::ArriLogC4, L709 = LutSpace::LinearRec709;
        const Case cases[] = {
            { "look alone", { &look }, { e() }, { in(L4, "header", true, L4) }, D },
            { "LinRec709 file + look",
              { &toLog, &look },
              { e(), e() },
              { in(L709, "header", true, L709), in(L4, "header", false, L4) },
              D },
            { "look + creative", { &look, &creative }, { e(), e() }, { in(L4, "header", true, L4), in(D, "", false, D) }, D },
            { "picture-to-LogC4 file + creative (a mismatch)",
              { &shaped, &creative },
              { e(), e() },
              { in(D, "", false, D), in(D, "", false, L4) },
              D },
            { "None on a look", { &look }, { e(1.0f, (int) D) }, { in(D, "set", false, D) }, D },
            { "S-Log3 by hand on a creative LUT",
              { &creative },
              { e(1.0f, (int) LutSpace::SLog3) },
              { in(LutSpace::SLog3, "set", true, LutSpace::SLog3) },
              D },
            { "conversion file at strength 0, then the look",
              { &toLog, &look },
              { e(0.0f), e() },
              { LayerInput {}, in(L4, "header", true, L4) },
              D },
            { "LinRec709 file alone (ends in LogC4)", { &toLog }, { e() }, { in(L709, "header", true, L709) }, L4 },
        };
        for (const Case& c : cases)
        {
            LayerInput got[kMaxLutChainLayers] {};
            const LutSpace ends = ResolveLookInputs(c.entries, c.luts.data(), got);
            bool ok = ends == c.endsIn;
            for (size_t i = 0; i < c.want.size(); ++i)
                ok = ok && got[i].space == c.want[i].space && got[i].gamut == c.want[i].gamut &&
                     std::string(got[i].from) == c.want[i].from && got[i].converted == c.want[i].converted &&
                     got[i].given == c.want[i].given;
            expect(ok, std::string("ResolveLookInputs: ") + c.what);
        }
        std::puts("5. look inputs resolve in stack order");
    }

    // 6. Linear HDR highlights through a baked look on Auto.
    {
        const Lut3D look = SyntheticLook(33);
        std::vector<ChainLayer> layers { ChainLayer { &look, 1.0f, true, LutSpace::ArriLogC4, FrameDecode::LinearHdr,
                                                      LutSpace::ArriLogC4 } };
        Lut3D baked;
        std::string err;
        float strength = 0.0f;
        expect(BakeChain(layers, &baked, &err, &strength) && baked.latticeCoord == kLatticeLinearHdr,
               "a Linear HDR conversion is not baked on the log-spaced lattice");
        char where[160] = "";
        const float worst = WorstVsExact(baked, look, FrameDecode::LinearHdr, LutSpace::ArriLogC4, where, sizeof(where));
        std::printf("6. Linear HDR look on Auto vs exact, 0.01x-50x white: worst %.2e %s\n", worst, where);
        // The decode scales all three channels by the brightest one (hue kept), which puts a crease along the grey
        // diagonal that trilinear lookup blends across; with this look's steep S (about twice AgX's) that is ~0.013
        // at grey 4x. The pack's own AgX look stays within 0.005 (section 9).
        expect(worst < 2e-2f, "Linear HDR highlights through the bake are off the exact conversion + look");

        // The same chain on an even lattice (what the bake used to do): the highlights collapse into the top cell.
        Lut3D even = baked;
        even.latticeCoord = kLatticeEven;
        const int n = even.size;
        for (int b = 0; b < n; ++b)
            for (int g = 0; g < n; ++g)
                for (int r = 0; r < n; ++r)
                {
                    const float v[3] = { r / float(n - 1), g / float(n - 1), b / float(n - 1) };
                    ApplyConvertedLayer(look, 1.0f, FrameDecode::LinearHdr, LutSpace::ArriLogC4, LutSpace::ArriLogC4, v,
                                        &even.rgb[(((size_t) b * n + g) * n + r) * 3]);
                }
        const float evenWorst =
            WorstVsExact(even, look, FrameDecode::LinearHdr, LutSpace::ArriLogC4, where, sizeof(where));
        std::printf("   the same on an even lattice: worst %.2e %s\n", evenWorst, where);
        expect(evenWorst > 0.02f, "the even-lattice control does not show the highlight error (check is blind)");

        // The coordinate round-trips, up to ~100x white: above it the display value is within 1e-5 of 1, finer than a
        // float holds there (on the GPU too), and all of it is white anyway.
        float trip = 0.0f;
        for (int i = 0; i <= 52; ++i)
            trip = std::max(trip, std::abs(LatticeFromDisplay(DisplayFromLattice(i / 64.0f)) - i / 64.0f));
        expect(trip < 1e-4f, "LatticeFromDisplay and DisplayFromLattice are not inverses");

        // SDR keeps the even lattice.
        layers[0].decode = FrameDecode::Srgb;
        expect(BakeChain(layers, &baked, &err, &strength) && baked.latticeCoord == kLatticeEven,
               "an SDR conversion left the even lattice");
        const float sdr = WorstVsExact(baked, look, FrameDecode::Srgb, LutSpace::ArriLogC4, where, sizeof(where));
        std::printf("   SDR look on Auto vs exact: worst %.2e %s\n", sdr, where);
        expect(sdr < 5e-3f, "SDR conversion through the bake is off the exact conversion + look");
    }

    // 7. Ensure with real files.
    {
        const auto lookFn = [](float r, float g, float b, float* v)
        {
            const float in[3] = { r, g, b };
            for (int c = 0; c < 3; ++c)
                v[c] = 1.0f / (1.0f + std::exp(-10.0f * (in[c] - 0.45f)));
        };
        const auto creativeFn = [](float r, float g, float b, float* v)
        {
            v[0] = 0.9f * r + 0.05f;
            v[1] = 0.95f * g;
            v[2] = 0.8f * b + 0.1f;
        };
        const std::string look = WriteCube(
            "Test-Look-sRGB.cube", "#  Input Gamma: Arri LogC4\n#  Input Color Space: Arri Wide Gamut 4\n", 17, lookFn);
        const std::string creative = WriteCube("Analog Cinema.cube", "", 9, creativeFn);
        const std::string broken = WriteCube("Broken.cube", "", 2,
                                             [](float, float, float, float* v)
                                             {
                                                 v[0] = std::nanf("");
                                                 v[1] = v[2] = 0.0f;
                                             });

        Lut3D parsed;
        std::string err;
        expect(!ParseCube(broken, &parsed, &err) && err.find("finite") != std::string::npos,
               "a .cube with a nan row was accepted: " + err);
        expect(ParseCube(look, &parsed, &err) && parsed.look.in == LutSpace::ArriLogC4 &&
                   std::string(parsed.look.inFrom) == "header",
               "a parsed look does not carry what its header says");

        // One converted look: baked at full strength, its strength left to the pass, so its slider bakes nothing.
        DlssNr_LutChainState s, full;
        expect(Bake({ { look, 0.5f } }, FrameDecode::LinearHdr, &s), "converted look at 0.5 did not bake");
        expect(Bake({ { look, 1.0f } }, FrameDecode::LinearHdr, &full), "converted look at 1 did not bake");
        const std::string keyBefore = s.key;
        expect(s.strength == 0.5f && s.inputs[0].converted,
               "a single converted look does not leave its strength to the pass");
        expect(s.baked.rgb == full.baked.rgb, "a single converted look baked its strength in");
        expect(Bake({ { look, 0.8f } }, FrameDecode::LinearHdr, &s) && s.key == keyBefore && s.strength == 0.8f,
               "moving a single converted look's slider re-baked");

        // Auto -> LogC4 by hand: the same conversion, no bake, but the menu's text follows the setting.
        expect(Bake({ { look, 0.8f, (int) LutSpace::ArriLogC4 } }, FrameDecode::LinearHdr, &s) && s.key == keyBefore &&
                   std::string(s.inputs[0].from) == "set",
               "Auto -> the same space by hand re-baked, or the menu kept saying Auto");
        // Auto -> None: a different picture, a bake.
        expect(Bake({ { look, 0.8f, (int) LutSpace::Display } }, FrameDecode::LinearHdr, &s) && s.key != keyBefore &&
                   !s.inputs[0].converted,
               "Auto -> None did not re-bake");
        // A frame-decode change re-bakes a converting stack.
        Bake({ { look, 0.8f } }, FrameDecode::LinearHdr, &s);
        const std::string keyHdr = s.key;
        expect(Bake({ { look, 0.8f } }, FrameDecode::Srgb, &s) && s.key != keyHdr,
               "a frame decode change did not re-bake a converted look");

        // 8. Old stacks: an ordinary creative LUT (even named "Analog ...") is not converted and keeps the shortcut; a
        // creative stack does not re-bake when the frame decode changes.
        DlssNr_LutChainState c;
        expect(Bake({ { creative, 0.7f } }, FrameDecode::LinearHdr, &c) && !c.inputs[0].converted &&
                   c.key.find("|as ") == std::string::npos && c.strength == 0.7f && c.baked.size == 9,
               "a creative LUT got a conversion or lost the single-layer shortcut");
        expect(Bake({ { creative, 0.7f }, { creative, 0.5f } }, FrameDecode::LinearHdr, &c), "creative stack did not bake");
        const std::string keyCreative = c.key;
        expect(Bake({ { creative, 0.7f }, { creative, 0.5f } }, FrameDecode::Srgb, &c) && c.key == keyCreative &&
                   c.baked.latticeCoord == kLatticeEven,
               "a creative stack re-baked on a frame decode change or left the even lattice");

        // A file failing with the same layers on: the last good chain stays, the menu's inputs are cleared.
        DlssNr_LutChainState f;
        Bake({ { look, 1.0f }, { creative, 0.5f } }, FrameDecode::LinearHdr, &f);
        expect(f.inputs[0].converted, "look did not convert before the failure check");
        const std::vector<float> lastGood = f.baked.rgb;
        expect(Bake({ { broken, 1.0f }, { creative, 0.5f } }, FrameDecode::LinearHdr, &f) && f.failedLayer == 0 &&
                   f.baked.rgb == lastGood && !f.inputs[0].converted && !f.inputs[1].converted,
               "a failed layer left the menu showing the last bake's inputs");

        // Layers switched on or off around a failed one: baked from those that can be read, and the menu's inputs say
        // what that bake does.
        DlssNr_LutChainState g;
        Bake({ { look, 1.0f } }, FrameDecode::LinearHdr, &g);
        expect(Bake({ { broken, 1.0f }, { look, 1.0f } }, FrameDecode::LinearHdr, &g) && g.failedLayer == 0 &&
                   !g.inputs[0].converted && g.inputs[1].converted,
               "a layer next to a failed one was not baked, or the menu's inputs do not match the bake");
        std::puts("7-8. Ensure: strength, settings, decode, failures and old stacks behave");
    }

    // 9. The view-transform pack.
    if (argc >= 2)
    {
        const std::filesystem::path pack = argv[1];
        const std::string toLog = (pack / "LinRec709-to-ArriLogC4AWG4.cube").string();

        Lut3D file;
        std::string err;
        const bool parsed = ParseCube(toLog, &file, &err);
        expect(parsed, "pack LinRec709 file: " + err);
        float worst = parsed ? 0.0f : 1.0f;
        const float lights[][3] = { { 0.001f, 0.001f, 0.001f }, { 0.18f, 0.18f, 0.18f }, { 1, 1, 1 },  { 16, 16, 16 },
                                    { 0.6f, 0.2f, 0.03f },      { 0.02f, 0.03f, 0.3f },   { 3, 1, 0.2f } };
        for (const auto& light : lights)
        {
            if (!parsed)
                break;
            float mine[3], theirs[3];
            EncodeLightToSpace(LutSpace::ArriLogC4, light, mine);
            SampleLutFile(file, light, theirs);
            for (int c = 0; c < 3; ++c)
                worst = std::max(worst, std::abs(mine[c] - theirs[c]));
        }
        std::printf("9. LogC4 AWG4 vs the pack's LinRec709-to-ArriLogC4AWG4.cube: worst %.2e\n", worst);
        expect(worst < 4e-3f, "LogC4 AWG4 conversion differs from the pack's file by more than its own error");

        int looks = 0;
        for (const auto& entry : std::filesystem::directory_iterator(pack))
        {
            if (entry.path().extension() != ".cube")
                continue;
            Lut3D lut;
            if (!ParseCube(entry.path().string(), &lut, &err))
                continue;
            const std::string stem = entry.path().stem().string();
            const LookSpaces& got = lut.look;
            if (stem.rfind("ARRILogC4AWG4-to-", 0) == 0)
            {
                ++looks;
                expect(got.in == LutSpace::ArriLogC4 && got.inGamut == LutSpace::ArriLogC4 &&
                           std::string(got.inFrom) == "header" && got.out == LutSpace::Display,
                       stem + ": in " + Name(got.in) + " (" + got.inFrom + "), out " + Name(got.out));
            }
            else if (stem == "LinRec709-to-ArriLogC4AWG4")
                expect(got.in == LutSpace::LinearRec709 && std::string(got.inFrom) == "header" &&
                           got.out == LutSpace::ArriLogC4,
                       stem + ": in " + Name(got.in) + ", out " + Name(got.out));
        }
        std::printf("   pack: %d looks read LogC4 AWG4 from their headers\n", looks);
        expect(looks == 22, "expected the pack's 22 ARRILogC4AWG4-to-* looks");

        // A pack look on Auto vs the exact conversion + look.
        Lut3D look;
        expect(ParseCube((pack / "ARRILogC4AWG4-to-AgX-Resolve-Bombastic-sRGB.cube").string(), &look, &err),
               "pack look: " + err);
        char where[160] = "";
        for (FrameDecode decode : { FrameDecode::LinearHdr, FrameDecode::Srgb })
        {
            DlssNr_LutChainState a;
            expect(Bake({ { (pack / "ARRILogC4AWG4-to-AgX-Resolve-Bombastic-sRGB.cube").string(), 1.0f } }, decode, &a),
                   "pack look did not bake: " + a.error);
            const float off = WorstVsExact(a.baked, look, decode, LutSpace::ArriLogC4, where, sizeof(where));
            std::printf("   pack look on Auto, %s: worst %.2e %s\n   %s\n", FrameDecodeName(decode), off, where,
                        a.description.c_str());
            expect(off < 1e-2f, std::string("pack look on Auto is off the exact chain (") + FrameDecodeName(decode) + ")");
        }

        // The pack's LinRec709 file in front, on Auto: read through its shaper past 1, to the pack's own error.
        DlssNr_LutChainState p;
        expect(Bake({ { toLog, 1.0f }, { (pack / "ARRILogC4AWG4-to-AgX-Resolve-Bombastic-sRGB.cube").string(), 1.0f } },
                    FrameDecode::LinearHdr, &p),
               "pack LinRec709 + look did not bake: " + p.error);
        const float off = WorstVsExact(p.baked, look, FrameDecode::LinearHdr, LutSpace::ArriLogC4, where, sizeof(where));
        std::printf("   pack LinRec709 + look on Auto vs exact: worst %.2e %s\n", off, where);
        expect(off < 3e-2f, "the pack's LinRec709 file + look is off the exact chain by more than the pack's own error");
    }
    else
        std::puts("(no pack folder given: pack checks skipped)");

    std::puts(fails == 0 ? "PASS: look input" : "FAILED");
    return fails == 0 ? 0 : 1;
}
