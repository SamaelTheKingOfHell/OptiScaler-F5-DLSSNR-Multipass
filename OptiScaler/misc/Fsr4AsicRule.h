#pragma once

// Which FSR 4 model an AMD chip runs. Two views of the same list: the driver's (asic family + revision, as
// getFsr4ModelForAsic reads them from the kernel driver) and device_info's AsicType (IdentifyGpu's D3D12 capability
// check). Pure rules (no OptiScaler state), host-tested in tests/fsr4_asic_rule_smoke.cpp.
#include "../OptiTypes.h"
#include "../include/device_info/device_info.hpp"

#include <cstdint>

namespace Fsr4Asic
{
// Family ids and revision ranges (min <= revision < max) from amdgpu_asic_addr.h
constexpr uint32_t familyNv = 143;  // Navi 1x/2x
constexpr uint32_t familyVgh = 144; // Van Gogh
constexpr uint32_t familyNv3 = 145; // Navi 3x
constexpr uint32_t familyPhx = 148; // Phoenix / Hawk Point
constexpr uint32_t familyStx = 150; // Strix
constexpr uint32_t familyNv4 = 152; // Navi 4x

struct Asic
{
    uint32_t family, min, max;
};

// Navi 10 (0x01-0x0A) is in no list: no hardware INT8 (Rdna1HasInt8)
constexpr Asic navi12 { familyNv, 0x0A, 0x14 };
constexpr Asic navi14 { familyNv, 0x14, 0x28 };
constexpr Asic navi21 { familyNv, 0x28, 0x32 };
constexpr Asic navi22 { familyNv, 0x32, 0x3C };
constexpr Asic navi23 { familyNv, 0x3C, 0x46 };
constexpr Asic navi24 { familyNv, 0x46, 0x50 };
constexpr Asic vanGogh { familyVgh, 0x01, 0x80 }; // Driver checks a narrower range than the header's 0x01-0xFF
constexpr Asic navi31 { familyNv3, 0x01, 0x10 };
constexpr Asic navi33 { familyNv3, 0x10, 0x20 };
constexpr Asic navi32 { familyNv3, 0x20, 0xFF };
constexpr Asic phoenix1 { familyPhx, 0x01, 0x80 };
constexpr Asic phoenix2 { familyPhx, 0x80, 0xC0 };
constexpr Asic hawkPoint1 { familyPhx, 0xC0, 0xF0 };
constexpr Asic hawkPoint2 { familyPhx, 0xF0, 0xFF };
constexpr Asic strix1 { familyStx, 0x01, 0x40 };
constexpr Asic krackan1 { familyStx, 0x40, 0x50 };
constexpr Asic strixHalo { familyStx, 0xC0, 0xD0 };

constexpr bool Is(uint32_t family, uint32_t revision, const Asic& asic)
{
    return family == asic.family && revision >= asic.min && revision < asic.max;
}

// RDNA1 chips with hardware INT8 (dot4): Navi 12 (gfx1011) and Navi 14 (gfx1012). Navi 10 (gfx1010) has none.
constexpr bool Rdna1HasInt8(device_info::AsicType type)
{
    using device_info::AsicType;

    return type == AsicType::kGfx10_1_1 || type == AsicType::kGfx10_1_2 || type == AsicType::kGfx10_1_2X ||
           type == AsicType::kGfx10_1_2Xt;
}

// Checked in the same order as the driver
constexpr FSR4Support ModelForAsic(uint32_t family, uint32_t revision, uint32_t deviceId, bool asicInfoValid)
{
    auto is = [&](const Asic& asic) { return Is(family, revision, asic); };

    // GFX12 (RDNA4)
    if (family == familyNv4)
        return FSR4Support::FP8;

    // Navi 3x (RDNA3) or Strix Halo
    // Driver also falls back to it for device 0x7481 when it can't get the asic info
    if (is(navi31) || is(navi32) || is(navi33) || is(strixHalo) || (!asicInfoValid && deviceId == 0x7481))
        return FSR4Support::INT8;

    // Navi 2x (RDNA2)
    if (is(navi21) || is(navi22) || is(navi23) || is(navi24))
        return FSR4Support::INT8_2;

    if (is(strix1) || is(vanGogh))
        return FSR4Support::INT8Lite;

    // Not officially supported, we are adding support for them
    // Navi 10 is left out as it lacks hardware INT8 (Rdna1HasInt8)
    if (is(phoenix1) || is(phoenix2) || is(hawkPoint1) || is(hawkPoint2) || is(krackan1) || is(navi12) || is(navi14))
        return FSR4Support::INT8Lite;

    return FSR4Support::None;
}

// The driver's FSR 4 frame generation is FP8 only (RDNA4). Upscaling has INT8 models, frame generation has none, so the
// real support (the D3D12 checks, not a spoofed or user-picked model) decides. Same rule on D3D12 and Vulkan.
constexpr bool DriverFgAllowed(FSR4Support realSupport) { return realSupport == FSR4Support::FP8; }
} // namespace Fsr4Asic
