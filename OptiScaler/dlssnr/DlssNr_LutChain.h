#pragma once

// A stack of LUTs applied one after another, baked into ONE lattice (post-NR LUT epic,
// memory/plans/2026-10-31-nr-lut-chain.md). The GPU pass keeps sampling a single 3D texture, so stacking costs one
// fetch per pixel at any depth and needs no shader or constant change. Pure and free of D3D/Vulkan types like
// DlssNr_LutFile.h, so tests/nr_lut_chain_smoke.cpp runs it on the host.
//
// Per layer this is exactly what dlssnr_lut.hlsl does to a pixel: clamp the display value into the layer's domain,
// trilinear-sample the lattice, then blend original -> graded by the layer's strength. The blend uses the unclamped
// input, as the shader does.
//
// Look input (Story 6): a layer made for a log space reads the picture converted into it (DlssNr_LutSpaces.h), computed
// here per lattice point, so the shader still only samples one lattice.

#include "DlssNr_Lut.h"
#include "DlssNr_LutSpaces.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace DlssNr
{
constexpr int kMaxLutChainLayers = 4;

// Baked lattice size bounds for a stack of two or more layers: at least 17 so a stack of small LUTs is not resampled
// coarser than a typical one, at most 65 (the largest LUT anyone ships) so the texture stays small.
constexpr int kChainMinSize = 17;
constexpr int kChainMaxSize = 65;

// One layer on a display value, as the shader does it.
inline void ApplyLayer(const Lut3D& lut, float strength, const float in[3], float out[3])
{
    float graded[3];
    SampleLut(lut, in, graded);
    const float s = std::clamp(strength, 0.0f, 1.0f);

    for (int c = 0; c < 3; ++c)
        out[c] = in[c] + (graded[c] - in[c]) * s;
}

// One layer with its input converted from the picture into `space` (in `gamut`'s primaries) first: the file is read
// through its own shaper and table (SampleLutFile, any input), and its strength fades from the picture to the graded
// result.
inline void ApplyConvertedLayer(const Lut3D& lut, float strength, FrameDecode decode, LutSpace space, LutSpace gamut,
                                const float in[3], float out[3])
{
    float converted[3], graded[3];
    ConvertDisplayToSpace(decode, space, gamut, in, converted);
    SampleLutFile(lut, converted, graded);
    const float s = std::clamp(strength, 0.0f, 1.0f);

    for (int c = 0; c < 3; ++c)
        out[c] = in[c] + (graded[c] - in[c]) * s;
}

struct ChainLayer
{
    const Lut3D* lut = nullptr; // null or empty: skipped
    float strength = 1.0f;      // 0 is skipped too (it leaves the picture untouched)

    // Look input: when set, the layer reads the picture converted into `space` in `gamut`'s primaries (decoded per
    // `decode`).
    bool convert = false;
    LutSpace space = LutSpace::Display;
    FrameDecode decode = FrameDecode::Srgb;
    LutSpace gamut = LutSpace::Display;
};

// Composes `layers`, first to last, into *out, and gives in *passStrength the strength the GPU pass applies it with.
// Returns false with *out cleared when no layer does anything.
//
// One effective layer is the file itself, bit for bit, at its own size and domain, with its strength left to the pass:
// exactly the single-LUT pass, including display values outside the layer's domain (a lerp toward the unclamped input,
// which a lattice cannot hold). Two or more are sampled on a grid of clamp(largest member size, kChainMinSize,
// kChainMaxSize) points per side over display 0..1 (the domain the shader's input and output both live in), each point
// carried through every layer in turn with its strength, and the pass applies the result at strength 1. A layer with a
// converted input is always baked (alone too), at kChainMaxSize: the conversion is a curve the lattice has to hold.
// Alone it is baked at full strength and the pass applies its strength (a blend from the picture, as the bake would),
// so moving its slider bakes nothing.
inline bool BakeChain(const std::vector<ChainLayer>& layers, Lut3D* out, std::string* error, float* passStrength)
{
    *out = Lut3D();
    *passStrength = 1.0f;
    std::vector<ChainLayer> active;

    for (const ChainLayer& layer : layers)
    {
        if (layer.lut == nullptr || layer.lut->size < 2 ||
            layer.lut->rgb.size() < (size_t) layer.lut->size * layer.lut->size * layer.lut->size * 3)
            continue;
        if (!(layer.strength > 0.0f))
            continue;
        active.push_back(layer);
    }

    if (active.empty())
        return false;

    if (active.size() > (size_t) kMaxLutChainLayers)
    {
        if (error != nullptr)
            *error = "more than " + std::to_string(kMaxLutChainLayers) + " LUTs in the stack";
        return false;
    }

    int n = 0;
    bool converts = false;
    for (const ChainLayer& layer : active)
    {
        n = std::max(n, layer.lut->size);
        converts = converts || layer.convert;
    }
    n = converts ? kChainMaxSize : std::clamp(n, kChainMinSize, kChainMaxSize);

    if (active.size() == 1 && converts)
    {
        // the pass blends picture -> baked by the strength, exactly what the bake would hold
        *passStrength = std::clamp(active[0].strength, 0.0f, 1.0f);
        active[0].strength = 1.0f;
    }
    else if (active.size() == 1)
    {
        *out = *active[0].lut;
        *passStrength = std::clamp(active[0].strength, 0.0f, 1.0f);
        return true;
    }

    out->size = n;
    out->rgb.resize((size_t) n * n * n * 3);

    // A Linear HDR frame converted for a log look spans ~20 stops of light, most of them squeezed into the top of
    // display 0..1: the lattice is laid out over log light there (kLatticeLinearHdr), so highlights keep their
    // roll-off.
    const auto convertsLinearHdr = [](const ChainLayer& l) { return l.convert && l.decode == FrameDecode::LinearHdr; };
    out->latticeCoord = std::any_of(active.begin(), active.end(), convertsLinearHdr) ? kLatticeLinearHdr : kLatticeEven;
    std::vector<float> axis((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        const float u = (float) i / (float) (n - 1);
        axis[(size_t) i] = out->latticeCoord == kLatticeLinearHdr ? DisplayFromLattice(u) : u;
    }

    for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r)
            {
                float v[3] = { axis[(size_t) r], axis[(size_t) g], axis[(size_t) b] };

                for (const ChainLayer& layer : active)
                {
                    float next[3];
                    if (layer.convert)
                        ApplyConvertedLayer(*layer.lut, layer.strength, layer.decode, layer.space, layer.gamut, v,
                                            next);
                    else
                        ApplyLayer(*layer.lut, layer.strength, v, next);
                    v[0] = next[0];
                    v[1] = next[1];
                    v[2] = next[2];
                }

                float* dst = &out->rgb[(((size_t) b * n + g) * n + r) * 3];
                dst[0] = v[0];
                dst[1] = v[1];
                dst[2] = v[2];
            }

    return true;
}

// One entry of the stack as the settings hold it. An empty path is an absent layer.
struct ChainEntry
{
    std::string path;
    float strength = 1.0f;
    int input = kLookInputAuto; // Look input: kLookInputAuto or a LutSpace value (Display = None)
};

// Look input for each entry in order (one per entry, absent or at strength 0 included, those left as Display): the
// stack starts as the picture; a layer reading a log space gets the conversion while the signal is still the picture,
// and its output (header or name, else the picture) is what the next layer is handed. Returns what the last layer
// hands on: not the picture means the stack ends in a log signal (no view transform after a conversion LUT).
inline LutSpace ResolveLookInputs(const std::vector<ChainEntry>& entries, const Lut3D* const* luts, LayerInput* out)
{
    LutSpace current = LutSpace::Display;
    for (size_t i = 0; i < entries.size(); ++i)
    {
        out[i] = LayerInput();
        if (luts[i] == nullptr || luts[i]->size < 2 || !(entries[i].strength > 0.0f))
            continue;

        const LookSpaces& found = luts[i]->look;
        const int input = entries[i].input;
        LayerInput& r = out[i];
        if (input >= 0 && input < (int) LutSpace::Count)
        {
            r.space = r.gamut = (LutSpace) input;
            r.from = "set";
        }
        else
        {
            r.space = found.in;
            r.gamut = found.inGamut;
            r.from = found.inFrom;
        }

        r.converted = r.space != LutSpace::Display && current == LutSpace::Display;
        r.given = r.converted ? r.space : current;
        current = found.out;
    }
    return current;
}

// The post-NR stack as Config holds it (templated so this header stays free of Config.h). A layer switched off stays an
// entry at strength 0: skipped by the bake, and every later layer keeps its index (the menu's rows and the status's
// look inputs line up by it). The whole stack switched off is every layer off: the files stay parsed, so switching it
// back on only bakes.
template <typename ConfigT> inline std::vector<ChainEntry> PostLutEntries(const ConfigT& cfg)
{
    const bool stackOn = cfg.DlssNrPostLutStackEnabled.value_or_default();
    std::vector<ChainEntry> entries;
    for (int i = 0; i < ConfigT::kPostLutLayers; ++i)
    {
        const bool on = stackOn && cfg.DlssNrPostLutEnabled[i].value_or_default();
        entries.push_back({ cfg.DlssNrPostLutFile[i].value_or_default(),
                            on ? cfg.DlssNrPostLutStrength[i].value_or_default() : 0.0f,
                            cfg.DlssNrPostLutInput[i].value_or_default() });
    }
    return entries;
}

} // namespace DlssNr

// The parsed layers, the baked result and the last problem, for one stack. Same contract as DlssNr_LutState: a layer
// that fails to parse does not blank out a chain that was working, it is reported by `failedLayer` / `error`. Layers
// switched on or off after that do take effect: the chain is baked again without the layer that cannot be read.
struct DlssNr_LutChainState
{
    DlssNr_LutState layers[DlssNr::kMaxLutChainLayers];
    std::filesystem::file_time_type writeTimes[DlssNr::kMaxLutChainLayers] {}; // of each layer's file when last looked
    int ensuresUntilTimeCheck = 0; // the files' write times are looked at once every kTimeCheckEvery Ensure calls

    // What `baked` was made from: ordered "path|write time|on|strength", the strength left out for a single layer,
    // whose strength the pass applies (so moving its slider re-uploads nothing). "on" (strength above 0) stays in, so
    // the one active layer moving to another entry is a new bake.
    std::string key;
    std::string bakedOn; // which entries were on ('1' or '0', in order) when last baked, read or not
    DlssNr::Lut3D baked;
    float strength = 1.0f;                                    // the strength the pass applies `baked` with
    DlssNr::LayerInput inputs[DlssNr::kMaxLutChainLayers] {}; // each kept entry's look input at the last Ensure
    DlssNr::LutSpace endsIn = DlssNr::LutSpace::Display;      // what the stack hands on: not the picture = a log signal
    DlssNr::FrameDecode decode = DlssNr::FrameDecode::Srgb;   // the frame the last bake converted for
    std::string description;                                  // DlssNr_DescribeChain's text, made when `baked` is
    bool wanted = false;
    int failedLayer = -1; // index into the entries given to the last Ensure, -1 when none failed
    std::string failedPath;
    std::string error;

    bool Loaded() const { return baked.size >= 2; }
};

inline std::string DlssNr_DescribeChain(const DlssNr_LutChainState& state,
                                        const std::vector<DlssNr::ChainEntry>& present);

// Parses the entries (a path already tried is not read again unless its file was written since) and re-bakes only when
// the ordered paths, their files, the strengths or the look inputs changed (and the frame's decode, when a layer is
// converted). Returns whether a chain is usable. No entries clears everything.
inline bool DlssNr_LutChainEnsure(DlssNr_LutChainState* state, const std::vector<DlssNr::ChainEntry>& entries,
                                  DlssNr::FrameDecode decode = DlssNr::FrameDecode::Srgb)
{
    using namespace DlssNr;
    // About once a second at 60 fps: a .cube re-exported in place is picked up without a file query every frame.
    constexpr int kTimeCheckEvery = 60;

    if (state == nullptr)
        return false;

    std::vector<ChainEntry> present;
    for (const ChainEntry& e : entries)
        if (!e.path.empty() && present.size() < (size_t) kMaxLutChainLayers)
            present.push_back(e);

    state->wanted = !present.empty();
    state->failedLayer = -1;
    state->failedPath.clear();
    state->error.clear();

    if (present.empty())
    {
        for (DlssNr_LutState& layer : state->layers)
            DlssNr_LutEnsureParsed(&layer, "");
        state->key.clear();
        state->bakedOn.clear();
        state->baked = Lut3D();
        state->strength = 1.0f;
        state->description.clear();
        state->ensuresUntilTimeCheck = 0;
        for (LayerInput& in : state->inputs)
            in = LayerInput();
        state->endsIn = LutSpace::Display;
        return false;
    }

    const bool checkTimes = --state->ensuresUntilTimeCheck <= 0;
    if (checkTimes)
        state->ensuresUntilTimeCheck = kTimeCheckEvery;

    for (size_t i = 0; i < kMaxLutChainLayers; ++i)
        if (i >= present.size())
            DlssNr_LutEnsureParsed(&state->layers[i], "");

    for (size_t i = 0; i < present.size(); ++i)
    {
        // A new path, or every kTimeCheckEvery calls: note the file's write time. A file written since it was parsed is
        // parsed again (forgetting the attempt is what makes DlssNr_LutEnsureParsed read it).
        DlssNr_LutState& layer = state->layers[i];
        if (layer.attemptedPath != present[i].path || checkTimes)
        {
            std::error_code ec;
            const auto written = std::filesystem::last_write_time(present[i].path, ec);
            const auto noted = ec ? std::filesystem::file_time_type {} : written;
            if (layer.attemptedPath == present[i].path && noted != state->writeTimes[i])
                layer.attemptedPath.clear();
            state->writeTimes[i] = noted;
        }

        DlssNr_LutEnsureParsed(&layer, present[i].path);

        // A layer that is off is not in the chain, so its file cannot fail the chain (or warn).
        if (state->layers[i].failed && present[i].strength > 0.0f && state->failedLayer < 0)
        {
            state->failedLayer = (int) i;
            state->failedPath = state->layers[i].attemptedPath;
            state->error = state->layers[i].error;
        }
    }

    std::string onNow;
    for (const ChainEntry& e : present)
        onNow += e.strength > 0.0f ? '1' : '0';

    // A file that breaks changes nothing the player asked for, so the last good chain stays. Switching layers on or off
    // does: that is baked below from the layers that can be read, or the grade would ignore the switch.
    if (state->failedLayer >= 0 && onNow == state->bakedOn)
    {
        // Which layer read what no longer matches the entries the menu shows.
        for (LayerInput& in : state->inputs)
            in = LayerInput();
        state->endsIn = LutSpace::Display;
        return state->Loaded(); // keep the last good chain
    }
    state->bakedOn = onNow;

    std::vector<ChainEntry> baking = present;
    for (size_t i = 0; i < baking.size(); ++i)
        if (state->layers[i].failed)
            baking[i].strength = 0.0f;

    const size_t effective =
        (size_t) std::count_if(baking.begin(), baking.end(), [](const ChainEntry& e) { return e.strength > 0.0f; });

    const Lut3D* luts[kMaxLutChainLayers] {};
    for (size_t i = 0; i < baking.size(); ++i)
        luts[i] = state->layers[i].Loaded() ? &state->layers[i].lut : nullptr;
    LayerInput inputs[kMaxLutChainLayers] {};
    // Every call, not only on a bake: a setting that changes no conversion (Auto -> the same space by hand) bakes
    // nothing but still changes what the menu says.
    state->endsIn = ResolveLookInputs(baking, luts, inputs);
    std::copy(inputs, inputs + kMaxLutChainLayers, state->inputs);
    const bool converts =
        std::any_of(inputs, inputs + baking.size(), [](const LayerInput& in) { return in.converted; });

    // One layer's strength is the pass's, converted or not (BakeChain); a stack's is in the bake.
    const bool strengthBaked = effective != 1;
    std::string key = converts ? "decode " + std::to_string((int) decode) + "\n" : std::string();
    for (size_t i = 0; i < baking.size(); ++i)
    {
        key += baking[i].path + "|" + std::to_string(state->writeTimes[i].time_since_epoch().count()) +
               (baking[i].strength > 0.0f ? "|on" : "|off");
        if (strengthBaked)
            key += "|" + std::to_string(std::clamp(baking[i].strength, 0.0f, 1.0f));
        if (inputs[i].converted)
            key += "|as " + std::to_string((int) inputs[i].space) + "/" + std::to_string((int) inputs[i].gamut);
        key += "\n";
    }

    if (key != state->key)
    {
        std::vector<ChainLayer> layers;
        for (size_t i = 0; i < baking.size(); ++i)
            layers.push_back({ &state->layers[i].lut, baking[i].strength, inputs[i].converted, inputs[i].space, decode,
                               inputs[i].gamut });

        std::string err;
        Lut3D baked;
        float strength = 1.0f;
        const bool ok = BakeChain(layers, &baked, &err, &strength);
        state->key = key;
        state->baked = ok ? std::move(baked) : Lut3D();
        state->strength = strength;
        state->decode = decode;
        state->description = DlssNr_DescribeChain(*state, baking);
    }
    else if (!strengthBaked)
    {
        // A single layer's strength changes without a re-bake: the pass applies it.
        for (const ChainEntry& e : baking)
            if (e.strength > 0.0f)
                state->strength = std::clamp(e.strength, 0.0f, 1.0f);
    }

    if (effective == 0 && state->failedLayer < 0)
        state->error = "every LUT is off or at strength 0";

    return state->Loaded();
}

// "A (33^3) -> B (65^3), baked at 33^3" for the menu's status line: the layers of `present` (the entries the last
// Ensure kept) with a strength above 0, the ones in the bake, a converted one with its space ("B (33^3, as ARRI LogC4
// AWG4)") and how exact the frame's decode is. Empty when nothing is loaded. Made once per bake and kept in
// state.description, not per frame.
inline std::string DlssNr_DescribeChain(const DlssNr_LutChainState& state,
                                        const std::vector<DlssNr::ChainEntry>& present)
{
    if (!state.Loaded())
        return std::string();

    std::string text;
    int shown = 0;
    bool converted = false;
    for (size_t i = 0; i < present.size() && i < (size_t) DlssNr::kMaxLutChainLayers; ++i)
    {
        const DlssNr_LutState& layer = state.layers[i];
        if (!layer.Loaded() || !(present[i].strength > 0.0f))
            continue;
        if (!text.empty())
            text += " -> ";
        const DlssNr::LayerInput& in = state.inputs[i];
        text += std::filesystem::path(layer.loadedPath).filename().string() + " (" +
                DlssNr::DescribeLutSize(layer.lut) +
                (in.converted
                     ? std::string(", as ") + DlssNr::LutSpaceName(in.space) +
                           (in.gamut != in.space ? std::string(" in ") + DlssNr::LutGamutName(in.gamut) : std::string())
                     : std::string()) +
                ")";
        ++shown;
        converted = converted || in.converted;
    }
    if (shown > 1 || converted)
        text += ", baked at " + std::to_string(state.baked.size) + "^3";
    if (converted)
        text += std::string(" (") + DlssNr::FrameDecodeName(state.decode) + ")";
    return text;
}
