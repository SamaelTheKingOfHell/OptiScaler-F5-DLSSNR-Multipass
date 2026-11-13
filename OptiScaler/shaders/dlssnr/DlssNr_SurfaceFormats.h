#pragma once

// Which of NR's surfaces are stale when a format changes (DlssNr_Dx12.cpp, ReleaseSurfacesIfFormatChanged).
//
// Two formats are in play. The model surfaces (output, the ping-pong and clamp scratches) follow the model: the game's
// colour format, or RGBA16F while Compress screen edges packs the picture. The colour surfaces (the kept proxy
// colorCopy, the shrunk copy, the crop, the LUT scratch, the native pair) are made from and into the game's colour, so
// they follow the game's format. With compression on the model format no longer moves when the game's does, and
// checking only that one left the colour surfaces in the old format: the next copy from the game's new colour was an
// invalid call and the device was removed (NBA 2K27, native finished picture RGBA8 -> the game's DLSS after SR,
// RGBA16F).
//
// Our own surfaces are made in the canonical colour format, not the game's exact one: BGRA8 becomes its RGBA8 twin.
// Every shader reads and writes them through views of their own format, so the channel order sorts itself out, and a
// game that swaps its output between RGBA8 and BGRA8 from frame to frame (Watch Dogs: Legion) no longer looks like a
// format change. That swap used to throw away the model feature and its history every time. The few surfaces copied
// straight to or from the game's texture (CopyResource / CopyTextureRegion need the same format family) keep the exact
// format and check it themselves.
//
// A game can flip between the two float formats in the same way: Neverness to Everness hands the pre-SR colour as
// RGBA16F and R11G11B10F on alternate frames during its loading splash, and each flip rebuilt the model (~1 s a
// frame, then a crash). Once a game has shown both, our surfaces are made in RGBA16F for either: R11G11B10F widens to
// it without loss and the shaders read the game's texture through its own view. Only after the first flip, so a game
// that stays on R11G11B10F keeps the surfaces (and memory) it always had.
//
// Once seen, the flip holds until NR shuts down: a game that flips only during a splash keeps RGBA16F surfaces (twice
// the memory of R11G11B10F) after it, with no change to the picture.
//
// A game that alternates something else the model is built for (a format pair not covered here, two sizes, the
// placement) still makes the model rebuild every call. RebuildGuard stands NR down while that goes on.

namespace DlssNr
{
// DXGI_FORMAT values, kept as ints so this header needs no Windows headers.
constexpr int kR8G8B8A8Typeless = 27, kR8G8B8A8Unorm = 28, kR8G8B8A8UnormSrgb = 29;
constexpr int kB8G8R8A8Unorm = 87, kB8G8R8A8Typeless = 90, kB8G8R8A8UnormSrgb = 91;
constexpr int kR16G16B16A16Float = 10, kR11G11B10Float = 26;

constexpr bool IsFlippingFloatFormat(int format) { return format == kR16G16B16A16Float || format == kR11G11B10Float; }

// The game moved from one of the two float formats to the other.
constexpr bool IsFloatFlip(int last, int now)
{
    return last != now && IsFlippingFloatFormat(last) && IsFlippingFloatFormat(now);
}

// The format our surfaces are made in for a game colour format. BGRA8 moves to RGBA8 (keeping typeless/UNORM/sRGB),
// and R11G11B10F to RGBA16F once the game has flipped between the two (floatFlipSeen); everything else, RGBA8 itself
// included, stays as it is, so games that never swap see the surfaces they always had.
constexpr int CanonicalColourFormat(int format, bool floatFlipSeen = false)
{
    switch (format)
    {
    case kR11G11B10Float:
        return floatFlipSeen ? kR16G16B16A16Float : kR11G11B10Float;
    case kB8G8R8A8Typeless:
        return kR8G8B8A8Typeless;
    case kB8G8R8A8Unorm:
        return kR8G8B8A8Unorm;
    case kB8G8R8A8UnormSrgb:
        return kR8G8B8A8UnormSrgb;
    default:
        return format;
    }
}

// Two game colour formats our surfaces treat as one: only the channel order differs.
constexpr bool SameColourFamily(int a, int b) { return CanonicalColourFormat(a) == CanonicalColourFormat(b); }

// Formats are DXGI_FORMAT values as ints; 0 (UNKNOWN) means "no such surface yet". The wanted formats are canonical.
constexpr bool SurfacesStale(int haveModel, int wantModel, int haveColour, int wantColour)
{
    return (haveModel != 0 && haveModel != wantModel) || (haveColour != 0 && haveColour != wantColour);
}

// What the model is built for on one call: the formats, the sizes and the placement (before or after SR, RR). Two calls
// with the same set-up need no rebuild between them.
struct Setup
{
    int modelFormat = 0, colourFormat = 0;
    unsigned width = 0, height = 0, modelWidth = 0, modelHeight = 0;
    bool beforeUpscale = false, rayReconstruction = false;

    // Spelled out: the host tests build as C++17
    constexpr bool operator==(const Setup& o) const
    {
        return modelFormat == o.modelFormat && colourFormat == o.colourFormat && width == o.width &&
               height == o.height && modelWidth == o.modelWidth && modelHeight == o.modelHeight &&
               beforeUpscale == o.beforeUpscale && rayReconstruction == o.rayReconstruction;
    }
};

// Counts the game's calls that return the model to a set-up it was built for recently (A, B, A or A, B, C, A): the
// signature of a game cycling a few set-ups, where every call builds the model again (a long stall each). kLimit of
// them within kWindow calls and NR stands down (the picture passes untouched) until the game asks for one set-up
// kSettle calls in a row, then builds it once. Steps that do not go back (a dynamic resolution moving on, a slider
// being dragged, a setting changed) are never counted. After a stand-down the next return trips it at once, for a
// while (kWary calls), so a game that settles and then alternates again does not stall another kLimit times first. A
// setting changed and changed back within that time costs one stand-down (kSettle calls), which is accepted.
struct RebuildGuard
{
    static constexpr unsigned kLimit = 3;
    static constexpr unsigned long long kWindow = 16;
    static constexpr unsigned kSettle = 30;
    static constexpr unsigned long long kWary = 600;
    static constexpr unsigned kRecent = 4; // set-ups built before the current one that count as a return

    unsigned long long calls = 0;
    Setup built {};
    bool haveBuilt = false;
    Setup recent[kRecent] = {}; // newest first
    unsigned recentCount = 0;
    unsigned long long returns[kLimit] = {}; // call numbers of the latest returns, oldest first
    unsigned count = 0;
    bool standing = false;
    Setup waiting {};
    unsigned settled = 0;
    unsigned long long resumedAt = 0;
    bool resumed = false;

    // setup: what this call asks for; wouldRebuild: it differs from what the model was built for (the caller knows
    // whether a model exists at all). True when NR runs this call.
    bool Allow(const Setup& setup, bool wouldRebuild)
    {
        ++calls;

        if (standing)
        {
            settled = setup == waiting ? settled + 1 : 1;
            waiting = setup;

            if (settled < kSettle)
                return false;

            standing = false;
            count = 0;
            resumed = true;
            resumedAt = calls;
            Built(setup);
            return true;
        }

        // The model matches this call (or is made for it now): that is the one built
        if (!wouldRebuild)
        {
            built = setup;
            haveBuilt = true;
            return true;
        }

        if (!Recent(setup))
        {
            Built(setup);
            return true;
        }

        if (count == kLimit)
        {
            for (unsigned i = 1; i < kLimit; ++i)
                returns[i - 1] = returns[i];
            --count;
        }
        returns[count++] = calls;

        const bool wary = resumed && calls - resumedAt < kWary;
        if ((count == kLimit && calls - returns[0] < kWindow) || wary)
        {
            // Not built: the model stays as it is until the game settles
            standing = true;
            waiting = setup;
            settled = 1;
            return false;
        }

        Built(setup);
        return true;
    }

  private:
    bool Recent(const Setup& setup) const
    {
        for (unsigned i = 0; i < recentCount; ++i)
            if (recent[i] == setup)
                return true;
        return false;
    }

    void Built(const Setup& setup)
    {
        if (haveBuilt && !(setup == built))
        {
            // The one built so far moves to the front of the recent ones, once
            unsigned i = 0;
            while (i < recentCount && !(recent[i] == built))
                ++i;
            if (i == recentCount && recentCount < kRecent)
                ++recentCount;
            for (unsigned j = i < recentCount ? i : recentCount - 1; j > 0; --j)
                recent[j] = recent[j - 1];
            recent[0] = built;
        }
        built = setup;
        haveBuilt = true;
    }
};

// A surface made at the ordinary (uncompressed) size: stale when that size is not the one wanted now. Under Compress
// screen edges the change check sees only the packed size, which can stay put while the ordinary size moves by a
// 16-pixel step (1920x1080, Balanced, scale 0.50 to 0.504: 960x544 -> 976x544, packed 864x496 either way), so the
// surface has to be asked for itself.
constexpr bool SizeStale(unsigned haveW, unsigned haveH, unsigned wantW, unsigned wantH)
{
    return haveW != 0 && (haveW != wantW || haveH != wantH);
}
} // namespace DlssNr
