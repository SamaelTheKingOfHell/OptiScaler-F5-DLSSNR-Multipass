// Host check of Fsr4AsicRule.h: which FSR 4 model an AMD chip runs, RDNA1 INT8 list in particular.
// No GPU and no game needed.
// cl /std:c++20 /EHsc tests/fsr4_asic_rule_smoke.cpp
#include <cstdio>

// OptiTypes.h (FSR4Support) logs from one of its helpers; no logger here
#define LOG_WARN(...) ((void) 0)

#include "../OptiScaler/misc/Fsr4AsicRule.h"

using namespace Fsr4Asic;
using device_info::AsicType;

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

static FSR4Support Model(uint32_t family, uint32_t revision) { return ModelForAsic(family, revision, 0, true); }

int main()
{
    // RDNA1: Navi 10 (RX 5600/5700) has no hardware INT8, Navi 12 and Navi 14 (RX 5500) do
    CHECK(Model(familyNv, 0x01) == FSR4Support::None);
    CHECK(Model(familyNv, 0x09) == FSR4Support::None);
    CHECK(Model(familyNv, 0x0A) == FSR4Support::INT8Lite); // Navi 12, first revision
    CHECK(Model(familyNv, 0x13) == FSR4Support::INT8Lite);
    CHECK(Model(familyNv, 0x14) == FSR4Support::INT8Lite); // Navi 14, first revision
    CHECK(Model(familyNv, 0x27) == FSR4Support::INT8Lite);

    // Navi 2x starts right after Navi 14: no overlap
    CHECK(Model(familyNv, 0x28) == FSR4Support::INT8_2);
    CHECK(Model(familyNv, 0x4F) == FSR4Support::INT8_2);
    CHECK(Model(familyNv, 0x50) == FSR4Support::None);

    // The device_info view agrees with the driver view, chip by chip
    CHECK(!Rdna1HasInt8(AsicType::kGfx10_1_0));   // Navi 10
    CHECK(!Rdna1HasInt8(AsicType::kGfx10_1_0Xl)); // Navi 10 XL
    CHECK(Rdna1HasInt8(AsicType::kGfx10_1_1));    // Navi 12
    CHECK(Rdna1HasInt8(AsicType::kGfx10_1_2));    // Navi 14
    CHECK(Rdna1HasInt8(AsicType::kGfx10_1_2X));   // Navi 14 (Pro W5500X)
    CHECK(Rdna1HasInt8(AsicType::kGfx10_1_2Xt));  // Navi 14 (Pro 5500 XT)
    CHECK(!Rdna1HasInt8(AsicType::kGfx10_3_0));   // RDNA2 has its own branch

    // The rest of the list is unchanged by the fix
    CHECK(Model(familyNv4, 0x01) == FSR4Support::FP8);
    CHECK(Model(familyNv3, 0x01) == FSR4Support::INT8);                     // Navi 31
    CHECK(Model(familyNv3, 0x20) == FSR4Support::INT8);                     // Navi 32
    CHECK(Model(familyStx, 0xC0) == FSR4Support::INT8);                     // Strix Halo
    CHECK(ModelForAsic(0, 0, 0x7481, false) == FSR4Support::INT8);          // driver fallback without asic info
    CHECK(ModelForAsic(0, 0, 0x7481, true) == FSR4Support::None);
    CHECK(Model(familyStx, 0x01) == FSR4Support::INT8Lite);                 // Strix Point
    CHECK(Model(familyVgh, 0x01) == FSR4Support::INT8Lite);                 // Van Gogh
    CHECK(Model(familyVgh, 0x80) == FSR4Support::None);                     // driver's narrower range
    CHECK(Model(familyPhx, 0x01) == FSR4Support::INT8Lite);                 // Phoenix
    CHECK(Model(familyPhx, 0xF0) == FSR4Support::INT8Lite);                 // Hawk Point 2
    CHECK(Model(familyStx, 0x40) == FSR4Support::INT8Lite);                 // Krackan
    CHECK(Model(familyStx, 0x50) == FSR4Support::None);

    // Driver frame generation is FP8 only: RDNA4 gets it, every INT8 chip (RDNA3 included) does not
    CHECK(DriverFgAllowed(FSR4Support::FP8));
    CHECK(!DriverFgAllowed(FSR4Support::INT8));
    CHECK(!DriverFgAllowed(FSR4Support::INT8_2));
    CHECK(!DriverFgAllowed(FSR4Support::INT8Lite));
    CHECK(!DriverFgAllowed(FSR4Support::None));
    CHECK(DriverFgAllowed(Model(familyNv4, 0x01)));  // Navi 4x
    CHECK(!DriverFgAllowed(Model(familyNv3, 0x01))); // Navi 31
    CHECK(!DriverFgAllowed(Model(familyNv, 0x28)));  // Navi 21
    CHECK(!DriverFgAllowed(Model(familyNv, 0x14)));  // Navi 14

    // Compile-time too, so a change of the table breaks the build of this test
    static_assert(ModelForAsic(familyNv, 0x01, 0, true) == FSR4Support::None);
    static_assert(ModelForAsic(familyNv, 0x14, 0, true) == FSR4Support::INT8Lite);

    if (fails == 0)
        printf("fsr4_asic_rule_smoke: PASS\n");

    return fails == 0 ? 0 : 1;
}
