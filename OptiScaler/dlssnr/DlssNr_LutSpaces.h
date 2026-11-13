#pragma once

// Look input for the post-NR LUT stack (post-NR LUT epic Story 6, memory/plans/2026-10-31-nr-lut-chain.md): a look made
// for a camera log space (ARRILogC4AWG4-to-..., an S-Log3 look, ...) is handed the game's picture converted into that
// space, so no conversion LUT is needed in front of it. Pure and free of D3D/Vulkan types like DlssNr_LutFile.h, so
// tests/nr_lut_spaces_smoke.cpp runs it on the host.
//
// Two halves:
//  - FrameDecode: what the LUT pass hands the stack (dlssnr_lut.hlsl DecodeToDisplay01) back to Rec.709 light relative
//    to the encode's white point. Exact for Linear HDR (sRGB, then Neutwo on the peak channel) and for PQ up to paper
//    white (the pass clips there); tone-mapped SDR and gamma 2.2 frames have lost their light, so their highlights are
//    lifted by an approximation (mid grey 0.18 stays put, display white becomes kSdrWhiteLight).
//  - The spaces: each vendor's published curve and gamut, from Rec.709 light (D65). ACES AP1 has its own white, reached
//    with a Bradford adaptation as ACES does.
//
// Sources (curve constants and primaries): ARRI "LogC4 Logarithmic Color Space" specification and ALEXA LogC3 (SUP 3.x,
// EI 800, normalised), ACES S-2016-001 (ACEScct) and TB-2014-004 (AP1), Sony "S-Log3/S-Gamut3.Cine" technical summary,
// Panasonic "V-Log/V-Gamut" reference manual, Blackmagic "DaVinci Wide Gamut - Intermediate" whitepaper.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <string>
#include <vector>

namespace DlssNr
{
// The signal a layer reads or writes. Display = the finished picture the pass hands the stack (sRGB-encoded 0..1):
// what an ordinary creative LUT is made for.
enum class LutSpace : int
{
    Display = 0,
    LinearRec709,
    ArriLogC3,
    ArriLogC4,
    AcesCct,
    SLog3,
    VLog,
    DaVinciIntermediate,
    Count
};

// A layer's "Look input" setting: Auto, or a LutSpace value (Display = None, use the picture as it is).
constexpr int kLookInputAuto = -1;

// The ini's words for the setting (PostLutInput1-4): auto, none, linear709, logc3, logc4, acescct, slog3, vlog,
// davinci.
inline const char* LookInputKey(int input)
{
    static const char* const kKeys[] = { "none", "linear709", "logc3", "logc4", "acescct", "slog3", "vlog", "davinci" };
    static_assert(std::size(kKeys) == (size_t) LutSpace::Count);
    return input >= 0 && input < (int) LutSpace::Count ? kKeys[input] : "auto";
}

// Anything it does not know (a typo, "auto") is Auto.
inline int LookInputFromKey(const std::string& key)
{
    std::string k = key;
    std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    for (int i = 0; i < (int) LutSpace::Count; ++i)
        if (k == LookInputKey(i))
            return i;
    return kLookInputAuto;
}

inline const char* LutSpaceName(LutSpace s)
{
    switch (s)
    {
    case LutSpace::LinearRec709:
        return "Linear Rec.709";
    case LutSpace::ArriLogC3:
        return "ARRI LogC3 AWG3";
    case LutSpace::ArriLogC4:
        return "ARRI LogC4 AWG4";
    case LutSpace::AcesCct:
        return "ACEScct AP1";
    case LutSpace::SLog3:
        return "S-Log3 S-Gamut3.Cine";
    case LutSpace::VLog:
        return "V-Log V-Gamut";
    case LutSpace::DaVinciIntermediate:
        return "DaVinci Intermediate DWG";
    default:
        return "sRGB picture";
    }
}

// A LutSpace standing for its own primaries, by name ("AWG4", "BT.709"), for a header that pairs a curve with another
// gamut.
inline const char* LutGamutName(LutSpace s)
{
    switch (s)
    {
    case LutSpace::ArriLogC3:
        return "AWG3";
    case LutSpace::ArriLogC4:
        return "AWG4";
    case LutSpace::AcesCct:
        return "AP1";
    case LutSpace::SLog3:
        return "S-Gamut3.Cine";
    case LutSpace::VLog:
        return "V-Gamut";
    case LutSpace::DaVinciIntermediate:
        return "DaVinci Wide Gamut";
    default:
        return "BT.709";
    }
}

// How the frame reached the stack: mirrors the shader's DecodeToDisplay01 branches (InputEncoding 4 = PQ first, then
// ColourIsLinearHdr, then 3 = gamma 2.2, else tone-mapped sRGB / passthrough).
enum class FrameDecode : int
{
    Srgb = 0,
    Gamma22,
    LinearHdr,
    Pq
};

inline FrameDecode FrameDecodeFor(unsigned int inputEncoding, bool colourIsLinearHdr)
{
    if (inputEncoding == 4u)
        return FrameDecode::Pq;
    if (colourIsLinearHdr)
        return FrameDecode::LinearHdr;
    if (inputEncoding == 3u)
        return FrameDecode::Gamma22;
    return FrameDecode::Srgb;
}

inline const char* FrameDecodeName(FrameDecode d)
{
    switch (d)
    {
    case FrameDecode::LinearHdr:
        return "Linear HDR, exact";
    case FrameDecode::Pq:
        return "PQ, exact up to paper white";
    case FrameDecode::Gamma22:
        return "gamma 2.2 SDR, highlights estimated";
    default:
        return "SDR, highlights estimated";
    }
}

// Where an SDR frame's display white lands in light: the looks of a view-transform pack reach white around 8-16x
// mid-grey-relative light (checked on the AgX/OpenDRT/JP2499 looks, 2026-10-31).
constexpr float kSdrWhiteLight = 16.0f;

namespace LutSpaces
{
inline float SrgbToLinear(float v)
{
    v = std::clamp(v, 0.0f, 1.0f);
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

using Mat3 = float[3][3];

inline void Mul(const Mat3& m, const float in[3], float out[3])
{
    for (int r = 0; r < 3; ++r)
        out[r] = m[r][0] * in[0] + m[r][1] * in[1] + m[r][2] * in[2];
}

inline void MatMul(const Mat3& a, const Mat3& b, Mat3& out)
{
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
}

inline void Invert(const Mat3& m, Mat3& out)
{
    const double a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], f = m[1][2], g = m[2][0], h = m[2][1],
                 i = m[2][2];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    out[0][0] = (float) ((e * i - f * h) / det);
    out[0][1] = (float) ((c * h - b * i) / det);
    out[0][2] = (float) ((b * f - c * e) / det);
    out[1][0] = (float) ((f * g - d * i) / det);
    out[1][1] = (float) ((a * i - c * g) / det);
    out[1][2] = (float) ((c * d - a * f) / det);
    out[2][0] = (float) ((d * h - e * g) / det);
    out[2][1] = (float) ((b * g - a * h) / det);
    out[2][2] = (float) ((a * e - b * d) / det);
}

// RGB -> XYZ from xy primaries and white (the normalised primary matrix).
inline void RgbToXyz(const float xy[3][2], const float white[2], Mat3& out)
{
    Mat3 p;
    for (int c = 0; c < 3; ++c)
    {
        p[0][c] = xy[c][0] / xy[c][1];
        p[1][c] = 1.0f;
        p[2][c] = (1.0f - xy[c][0] - xy[c][1]) / xy[c][1];
    }
    const float w[3] = { white[0] / white[1], 1.0f, (1.0f - white[0] - white[1]) / white[1] };
    Mat3 inv;
    Invert(p, inv);
    float s[3];
    Mul(inv, w, s);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out[r][c] = p[r][c] * s[c];
}

// Bradford chromatic adaptation from white `from` to white `to`.
inline void Bradford(const float from[2], const float to[2], Mat3& out)
{
    static const Mat3 kM = { { 0.8951f, 0.2664f, -0.1614f },
                             { -0.7502f, 1.7135f, 0.0367f },
                             { 0.0389f, -0.0685f, 1.0296f } };
    Mat3 inv;
    Invert(kM, inv);
    const float ws[3] = { from[0] / from[1], 1.0f, (1.0f - from[0] - from[1]) / from[1] };
    const float wd[3] = { to[0] / to[1], 1.0f, (1.0f - to[0] - to[1]) / to[1] };
    float cs[3], cd[3];
    Mul(kM, ws, cs);
    Mul(kM, wd, cd);
    Mat3 scale = { { cd[0] / cs[0], 0, 0 }, { 0, cd[1] / cs[1], 0 }, { 0, 0, cd[2] / cs[2] } };
    Mat3 t;
    MatMul(scale, kM, t);
    MatMul(inv, t, out);
}

struct Gamut
{
    float xy[3][2];
    float white[2];
};

constexpr float kD65[2] = { 0.3127f, 0.3290f };
constexpr float kAcesWhite[2] = { 0.32168f, 0.33767f };

inline const Gamut& GamutOf(LutSpace s)
{
    static const Gamut k709 = { { { 0.64f, 0.33f }, { 0.30f, 0.60f }, { 0.15f, 0.06f } }, { kD65[0], kD65[1] } };
    static const Gamut kAwg3 = { { { 0.6840f, 0.3130f }, { 0.2210f, 0.8480f }, { 0.0861f, -0.1020f } },
                                 { kD65[0], kD65[1] } };
    static const Gamut kAwg4 = { { { 0.7347f, 0.2653f }, { 0.1424f, 0.8576f }, { 0.0991f, -0.0308f } },
                                 { kD65[0], kD65[1] } };
    static const Gamut kAp1 = { { { 0.713f, 0.293f }, { 0.165f, 0.830f }, { 0.128f, 0.044f } },
                                { kAcesWhite[0], kAcesWhite[1] } };
    static const Gamut kSg3c = { { { 0.766f, 0.275f }, { 0.225f, 0.800f }, { 0.089f, -0.087f } },
                                 { kD65[0], kD65[1] } };
    static const Gamut kVg = { { { 0.730f, 0.280f }, { 0.165f, 0.840f }, { 0.100f, -0.030f } }, { kD65[0], kD65[1] } };
    static const Gamut kDwg = { { { 0.8000f, 0.3130f }, { 0.1682f, 0.9877f }, { 0.0790f, -0.1155f } },
                                { kD65[0], kD65[1] } };

    switch (s)
    {
    case LutSpace::ArriLogC3:
        return kAwg3;
    case LutSpace::ArriLogC4:
        return kAwg4;
    case LutSpace::AcesCct:
        return kAp1;
    case LutSpace::SLog3:
        return kSg3c;
    case LutSpace::VLog:
        return kVg;
    case LutSpace::DaVinciIntermediate:
        return kDwg;
    default:
        return k709;
    }
}

// Rec.709 light -> the space's own primaries (and white, for AP1).
inline void FromRec709Matrix(LutSpace s, Mat3& out)
{
    const Gamut& src = GamutOf(LutSpace::LinearRec709);
    const Gamut& dst = GamutOf(s);
    Mat3 toXyz, dstToXyz, fromXyz;
    RgbToXyz(src.xy, src.white, toXyz);
    RgbToXyz(dst.xy, dst.white, dstToXyz);
    Invert(dstToXyz, fromXyz);

    Mat3 adapt = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    if (dst.white[0] != src.white[0] || dst.white[1] != src.white[1])
        Bradford(src.white, dst.white, adapt);

    Mat3 t;
    MatMul(adapt, toXyz, t);
    MatMul(fromXyz, t, out);
}

inline float EncodeCurve(LutSpace s, float x)
{
    switch (s)
    {
    case LutSpace::ArriLogC3:
    {
        // ALEXA LogC3, SUP 3.x, EI 800, scene linear, normalised signal
        constexpr float cut = 0.010591f, a = 5.555556f, b = 0.052272f, c = 0.247190f, d = 0.385537f, e = 5.367655f,
                        f = 0.092809f;
        return x > cut ? c * std::log10(a * x + b) + d : e * x + f;
    }
    case LutSpace::ArriLogC4:
    {
        // the spec's a, b, c; its s and t follow from them (worked out once, not per lattice point)
        constexpr double a = (262144.0 - 16.0) / 117.45, b = (1023.0 - 95.0) / 1023.0, c = 95.0 / 1023.0;
        static const double slope = (7.0 * std::log(2.0) * std::pow(2.0, 7.0 - 14.0 * c / b)) / (a * b);
        static const double t = (std::pow(2.0, 14.0 * (-c / b) + 6.0) - 64.0) / a;
        return x >= t ? (float) ((std::log2(a * x + 64.0) - 6.0) / 14.0 * b + c) : (float) ((x - t) / slope);
    }
    case LutSpace::AcesCct:
        return x <= 0.0078125f ? 10.5402377416545f * x + 0.0729055341958355f : (std::log2(x) + 9.72f) / 17.52f;
    case LutSpace::SLog3:
        return x >= 0.01125f ? (420.0f + std::log10((x + 0.01f) / (0.18f + 0.01f)) * 261.5f) / 1023.0f
                             : (x * (171.2102946929f - 95.0f) / 0.01125f + 95.0f) / 1023.0f;
    case LutSpace::VLog:
        return x < 0.01f ? 5.6f * x + 0.125f : 0.241514f * std::log10(x + 0.00873f) + 0.598206f;
    case LutSpace::DaVinciIntermediate:
        return x <= 0.00262409f ? x * 10.44426855f : (std::log2(x + 0.0075f) + 7.0f) * 0.07329248f;
    default:
        return x;
    }
}
} // namespace LutSpaces

// The display value the LUT pass hands the stack -> Rec.709 light relative to the encode's white (mid grey 0.18).
//
// What "exact" means here, and what is assumed:
//  - Linear HDR undoes the pass's sRGB(Neutwo(light / white)) to float precision.
//  - PQ: the pass hands sRGB of BT.709 light clamped to 0..1 (DecodeToDisplay01: BT.2020 -> BT.709, then LinearToSrgb's
//    saturate), so light above paper white and colours outside BT.709 are already clipped when they arrive here.
//  - Mid grey: a log look expects scene mid grey at 0.18. The game's light is taken as it comes, relative to the
//    encode's white; ITU-R BT.2408 puts HDR reference grey lower (about 0.13 of reference white), so on HDR games the
//    look can sit up to half a stop darker than a camera exposed for 18% grey.
inline void DecodeFrameToLight(FrameDecode d, const float in[3], float out[3])
{
    using namespace LutSpaces;
    float lin[3];
    for (int c = 0; c < 3; ++c)
        lin[c] = d == FrameDecode::Gamma22 ? std::pow(std::clamp(in[c], 0.0f, 1.0f), 2.2f) : SrgbToLinear(in[c]);

    if (d == FrameDecode::Pq)
    {
        std::copy(lin, lin + 3, out);
        return;
    }

    const float m = std::max({ lin[0], lin[1], lin[2] });
    float scale = 1.0f;
    if (d == FrameDecode::LinearHdr)
    {
        // Neutwo^-1 on the peak channel, clamped as NeutwoDecode does (dlssnr_replace_curve.hlsli)
        const float p = std::min(m, 0.999999f);
        if (p > 1e-6f)
            scale = p / std::sqrt(std::max(1.0f - p * p, 1e-8f)) / p;
    }
    else
    {
        // SDR: lift highlights by luminance; Y + (W - 1) Y^5 nearly keeps mid grey (0.18 -> 0.183, +0.02 EV) and
        // sends white to W
        const float y = 0.2126f * lin[0] + 0.7152f * lin[1] + 0.0722f * lin[2];
        if (y > 1e-6f)
            scale = (y + (kSdrWhiteLight - 1.0f) * std::pow(y, 5.0f)) / y;
    }

    for (int c = 0; c < 3; ++c)
        out[c] = lin[c] * scale;
}

// Rec.709 light -> `space`'s curve in `gamut`'s primaries (a LutSpace standing for its own primaries: LinearRec709 =
// BT.709, ArriLogC4 = AWG4, ...). A look's header can name a gamut other than its curve's own (LogC4 in Rec.709).
inline void EncodeLightToSpace(LutSpace space, LutSpace gamut, const float light[3], float out[3])
{
    using namespace LutSpaces;
    if (space == LutSpace::Display)
    {
        std::copy(light, light + 3, out);
        return;
    }

    // Built once per gamut; the bake calls this per lattice point.
    struct Cache
    {
        Mat3 m[(int) LutSpace::Count];
        Cache()
        {
            for (int i = 0; i < (int) LutSpace::Count; ++i)
                FromRec709Matrix((LutSpace) i, m[i]);
        }
    };
    static const Cache cache;

    float g[3];
    Mul(cache.m[std::clamp((int) gamut, 0, (int) LutSpace::Count - 1)], light, g);
    for (int c = 0; c < 3; ++c)
        out[c] = EncodeCurve(space, g[c]);
}

// In the space's own primaries.
inline void EncodeLightToSpace(LutSpace space, const float light[3], float out[3])
{
    EncodeLightToSpace(space, space, light, out);
}

// The conversion a layer gets: the stack's display value -> `space` (in `gamut`'s primaries).
inline void ConvertDisplayToSpace(FrameDecode d, LutSpace space, LutSpace gamut, const float in[3], float out[3])
{
    float light[3];
    DecodeFrameToLight(d, in, light);
    EncodeLightToSpace(space, gamut, light, out);
}

// ---- Auto detection ----

namespace LutSpaces
{
// The words of a name or header value, lower case: split at anything not a letter or digit, between letters and digits,
// at a lower -> upper change, and before the last capital of a capital run that goes on in lower case. "ARRILogC4AWG4"
// -> arri log c 4 awg 4, "S-Log3" -> s log 3, "ACEScct" -> ace scct, "Analog Cinema" -> analog cinema.
inline std::vector<std::string> Words(const std::string& s)
{
    std::vector<std::string> words;
    std::string cur;
    const auto flush = [&]()
    {
        if (!cur.empty())
            words.push_back(cur);
        cur.clear();
    };

    for (size_t i = 0; i < s.size(); ++i)
    {
        const unsigned char ch = (unsigned char) s[i];
        if (!std::isalnum(ch))
        {
            flush();
            continue;
        }

        if (!cur.empty())
        {
            const unsigned char prev = (unsigned char) s[i - 1];
            const bool next = i + 1 < s.size() && std::islower((unsigned char) s[i + 1]);
            if ((std::isdigit(ch) != 0) != (std::isdigit(prev) != 0) || (std::islower(prev) && std::isupper(ch)) ||
                (std::isupper(prev) && std::isupper(ch) && next))
                flush();
        }
        cur += (char) std::tolower(ch);
    }
    flush();
    return words;
}

// True when consecutive words, at least `minWords` of them, join to exactly `token`: "logc" is found in "Log-C" and
// "ARRILogC4" but not inside "Analog Cinema"; "vlog" with minWords 2 is "V-Log" / "VLog", never a "Vlog" (video blog).
inline bool HasWords(const std::vector<std::string>& w, const char* token, size_t minWords = 1)
{
    const std::string t = token;
    for (size_t first = 0; first < w.size(); ++first)
    {
        std::string joined;
        for (size_t last = first; last < w.size() && joined.size() < t.size(); ++last)
        {
            joined += w[last];
            if (joined == t && last - first + 1 >= minWords)
                return true;
        }
    }
    return false;
}

// A log curve named in the words, or Count when none.
inline LutSpace LogCurveIn(const std::vector<std::string>& w)
{
    if (HasWords(w, "logc4"))
        return LutSpace::ArriLogC4;
    if (HasWords(w, "logc3") || HasWords(w, "logc"))
        return LutSpace::ArriLogC3;
    if (HasWords(w, "acescct"))
        return LutSpace::AcesCct;
    if (HasWords(w, "slog3"))
        return LutSpace::SLog3;
    if (HasWords(w, "vlog", 2))
        return LutSpace::VLog;
    if (HasWords(w, "davinciintermediate") || HasWords(w, "davinciwidegamut") || HasWords(w, "dwg"))
        return LutSpace::DaVinciIntermediate;
    return LutSpace::Count;
}

inline bool IsLinear709(const std::vector<std::string>& w)
{
    for (const char* t : { "linrec709", "linearrec709", "linear709", "lin709", "linbt709", "linearbt709" })
        if (HasWords(w, t))
            return true;
    return false;
}

inline bool IsDisplay(const std::vector<std::string>& w)
{
    for (const char* t :
         { "srgb", "rec709", "bt709", "709", "display", "gamma", "rec1886", "bt1886", "1886", "22", "24" })
        if (HasWords(w, t))
            return true;
    return false;
}

// A gamut named in the words (as the LutSpace whose primaries it is), or Count when none.
inline LutSpace GamutIn(const std::vector<std::string>& w)
{
    if (HasWords(w, "awg4") || HasWords(w, "arriwidegamut4") || HasWords(w, "widegamut4"))
        return LutSpace::ArriLogC4;
    if (HasWords(w, "awg3") || HasWords(w, "arriwidegamut3") || HasWords(w, "widegamut3") || HasWords(w, "awg") ||
        HasWords(w, "arriwidegamut"))
        return LutSpace::ArriLogC3;
    if (HasWords(w, "ap1") || HasWords(w, "acescg") || HasWords(w, "acescct"))
        return LutSpace::AcesCct;
    if (HasWords(w, "sgamut3cine"))
        return LutSpace::SLog3;
    if (HasWords(w, "vgamut"))
        return LutSpace::VLog;
    if (HasWords(w, "davinciwidegamut") || HasWords(w, "dwg"))
        return LutSpace::DaVinciIntermediate;
    if (HasWords(w, "rec709") || HasWords(w, "bt709") || HasWords(w, "709") || HasWords(w, "srgb"))
        return LutSpace::LinearRec709;
    return LutSpace::Count;
}
} // namespace LutSpaces

// What the header's "Gamma" and "Color Space" lines say, or false when they name nothing this knows. `*gamut` is the
// gamut the Color Space line names (as the LutSpace whose primaries it is), else the curve's own.
inline bool SpaceFromHeader(const std::string& gamma, const std::string& gamut, LutSpace* out, LutSpace* outGamut)
{
    using namespace LutSpaces;
    const std::vector<std::string> g = Words(gamma);
    const std::vector<std::string> s = Words(gamut);
    if (g.empty())
        return false;

    const LutSpace namedGamut = GamutIn(s);
    const LutSpace log = LogCurveIn(g);
    if (log != LutSpace::Count)
    {
        *out = log;
        *outGamut = namedGamut != LutSpace::Count ? namedGamut : log;
        return true;
    }

    if (HasWords(g, "linear"))
    {
        if (s.empty() || namedGamut == LutSpace::LinearRec709)
        {
            *out = *outGamut = LutSpace::LinearRec709;
            return true;
        }
        return false; // linear light in another gamut (ACES AP0, ...): not one of ours
    }

    if (IsDisplay(g))
    {
        *out = *outGamut = LutSpace::Display;
        return true;
    }
    return false;
}

// What a file name says: "<input>-to-<output>" (the common pack naming), or a log curve named in a name without "-to-"
// as its input. Whole words only (LutSpaces::HasWords). False for a side the name says nothing about.
inline void SpacesFromName(const std::string& stem, bool* inFound, LutSpace* in, bool* outFound, LutSpace* out)
{
    using namespace LutSpaces;
    std::string lower = stem;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char) std::tolower(c); });

    size_t to = std::string::npos;
    for (const char* sep : { "-to-", "_to_", " to " })
        if ((to = lower.find(sep)) != std::string::npos)
            break;

    const std::vector<std::string> left = Words(to == std::string::npos ? stem : stem.substr(0, to));
    *inFound = *outFound = false;

    const LutSpace leftLog = LogCurveIn(left);
    if (leftLog != LutSpace::Count)
    {
        *inFound = true;
        *in = leftLog;
    }
    else if (IsLinear709(left))
    {
        *inFound = true;
        *in = LutSpace::LinearRec709;
    }

    if (to == std::string::npos)
        return;

    const std::vector<std::string> right = Words(stem.substr(to + 4));
    const LutSpace rightLog = LogCurveIn(right);
    if (rightLog != LutSpace::Count)
    {
        *outFound = true;
        *out = rightLog;
    }
    else if (IsDisplay(right))
    {
        *outFound = true;
        *out = LutSpace::Display;
    }
}

// What a layer's look input resolved to at the last Ensure, for the menu.
struct LayerInput
{
    LutSpace space = LutSpace::Display; // what the layer reads
    LutSpace gamut = LutSpace::Display; // in which primaries (a LutSpace standing for its own), `space` unless the
                                        // header names another
    const char* from = "";              // Auto's source: "header", "name", "" (nothing found); "set" when chosen
    bool converted = false;             // the picture is converted into `space` for it
    LutSpace given = LutSpace::Display; // the signal the layer is actually handed (differs from `space`: a mismatch)
};

// A look's input and output as Auto sees them: the header first, then the name; nothing found = the picture
// (an ordinary creative LUT). `inFrom` is "header", "name" or "" (nothing found).
struct LookSpaces
{
    LutSpace in = LutSpace::Display;
    LutSpace inGamut = LutSpace::Display;
    LutSpace out = LutSpace::Display;
    const char* inFrom = "";
};

inline LookSpaces DetectLookSpaces(const std::string& stem, const std::string& inGamma, const std::string& inGamut,
                                   const std::string& outGamma, const std::string& outGamut)
{
    LookSpaces r;
    bool nameIn = false, nameOut = false;
    LutSpace fromNameIn = LutSpace::Display, fromNameOut = LutSpace::Display;
    SpacesFromName(stem, &nameIn, &fromNameIn, &nameOut, &fromNameOut);

    LutSpace h, hGamut;
    if (SpaceFromHeader(inGamma, inGamut, &h, &hGamut))
    {
        r.in = h;
        r.inGamut = hGamut;
        r.inFrom = "header";
    }
    else if (nameIn)
    {
        r.in = r.inGamut = fromNameIn;
        r.inFrom = "name";
    }

    if (SpaceFromHeader(outGamma, outGamut, &h, &hGamut))
        r.out = h;
    else if (nameOut)
        r.out = fromNameOut;
    return r;
}

} // namespace DlssNr
