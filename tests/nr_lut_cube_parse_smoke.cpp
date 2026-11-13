// Host check of DlssNr_LutFile.h: parsing a .cube LUT file into an in-memory 3D lattice. No GPU and no
// game -- writes small temp .cube files beside the binary and parses them back.
// cl /std:c++20 /EHsc /W4 tests/nr_lut_cube_parse_smoke.cpp
#include "../OptiScaler/dlssnr/DlssNr_LutFile.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace DlssNr;

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

// Writes `contents` to a temp file and returns its path. Each call uses its own name so tests never
// collide; the file is removed by the caller once it is done with it.
static std::string WriteTemp(const char* name, const std::string& contents)
{
    const std::string path = std::string("nr_lut_cube_parse_smoke_") + name + ".cube";
    std::ofstream out(path);
    out << contents;
    out.close();
    return path;
}

int main(int argc, char** argv)
{
    // A valid 2x2x2 file round-trips: size, domain, and every RGB triple in file order.
    {
        const std::string path = WriteTemp("valid", "TITLE \"test\"\n"
                                                    "# a comment\n"
                                                    "LUT_3D_SIZE 2\n"
                                                    "\n"
                                                    "0.0 0.0 0.0\n"
                                                    "1.0 0.0 0.0\n"
                                                    "0.0 1.0 0.0\n"
                                                    "1.0 1.0 0.0\n"
                                                    "0.0 0.0 1.0\n"
                                                    "1.0 0.0 1.0\n"
                                                    "0.0 1.0 1.0\n"
                                                    "1.0 1.0 1.0\n");

        Lut3D lut;
        std::string error;
        CHECK(ParseCube(path, &lut, &error));
        CHECK(lut.size == 2);
        CHECK(lut.domainMin[0] == 0.0f && lut.domainMin[1] == 0.0f && lut.domainMin[2] == 0.0f);
        CHECK(lut.domainMax[0] == 1.0f && lut.domainMax[1] == 1.0f && lut.domainMax[2] == 1.0f);
        CHECK(lut.rgb.size() == 8 * 3);
        // Fourth row: 1.0 1.0 0.0
        CHECK(lut.rgb[3 * 3 + 0] == 1.0f && lut.rgb[3 * 3 + 1] == 1.0f && lut.rgb[3 * 3 + 2] == 0.0f);
        // Last row: 1.0 1.0 1.0
        CHECK(lut.rgb[7 * 3 + 0] == 1.0f && lut.rgb[7 * 3 + 1] == 1.0f && lut.rgb[7 * 3 + 2] == 1.0f);

        std::filesystem::remove(path);
    }

    // DOMAIN_MIN / DOMAIN_MAX are read when present.
    {
        const std::string path = WriteTemp("domain", "LUT_3D_SIZE 2\n"
                                                     "DOMAIN_MIN 0.1 0.2 0.3\n"
                                                     "DOMAIN_MAX 0.9 0.8 0.7\n"
                                                     "0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n");

        Lut3D lut;
        std::string error;
        CHECK(ParseCube(path, &lut, &error));
        CHECK(lut.domainMin[0] == 0.1f && lut.domainMin[1] == 0.2f && lut.domainMin[2] == 0.3f);
        CHECK(lut.domainMax[0] == 0.9f && lut.domainMax[1] == 0.8f && lut.domainMax[2] == 0.7f);

        std::filesystem::remove(path);
    }

    // A 1D-only LUT is a per-channel curve: baked into a plain lattice over 0..1 that applies that curve.
    {
        // Squares each channel (5 points over 0..1, linear between them).
        const std::string path =
            WriteTemp("1d", "LUT_1D_SIZE 5\n0 0 0\n0.0625 0.0625 0.0625\n0.25 0.25 0.25\n0.5625 0.5625 0.5625\n1 1 1\n");

        Lut3D lut;
        std::string error;
        CHECK(ParseCube(path, &lut, &error));
        CHECK(lut.size == kShaperBakeSize && lut.shaperSize == 5 && lut.fileSize == 0);
        CHECK(DescribeLutSize(lut) == "1D 5");
        const float in[3] = { 0.5f, 0.25f, 1.0f };
        float out[3];
        SampleLut(lut, in, out);
        CHECK(std::fabs(out[0] - 0.25f) < 1e-4f && std::fabs(out[1] - 0.0625f) < 1e-4f && std::fabs(out[2] - 1.0f) < 1e-5f);

        std::filesystem::remove(path);
    }

    // A 1D shaper in front of a 3D table (the Resolve layout): shaper rows first, then the table's. The bake equals
    // shaper -> table sampled directly, and LUT_1D_INPUT_RANGE stretches the shaper's input.
    {
        // Shaper over 0..2 (so display 0..1 uses its first half): halves the value. Table: swaps red and blue.
        std::string text = "LUT_1D_SIZE 3\nLUT_1D_INPUT_RANGE 0 2\n0 0 0\n0.5 0.5 0.5\n1 1 1\nLUT_3D_SIZE 2\n";
        for (int b = 0; b < 2; ++b)
            for (int g = 0; g < 2; ++g)
                for (int r = 0; r < 2; ++r)
                    text += std::to_string(b) + " " + std::to_string(g) + " " + std::to_string(r) + "\n";
        const std::string path = WriteTemp("shaper3d", text);

        Lut3D lut;
        std::string error;
        CHECK(ParseCube(path, &lut, &error));
        CHECK(lut.size == kShaperBakeSize && lut.shaperSize == 3 && lut.fileSize == 2);
        CHECK(DescribeLutSize(lut) == "1D 3 + 2^3");
        const float in[3] = { 0.8f, 0.4f, 0.2f };
        float out[3];
        SampleLut(lut, in, out);
        CHECK(std::fabs(out[0] - 0.1f) < 1e-4f && std::fabs(out[1] - 0.2f) < 1e-4f && std::fabs(out[2] - 0.4f) < 1e-4f);

        std::filesystem::remove(path);
    }

    // A shaper with too few rows, and one with no rows of its own before the table ends, are rejected.
    {
        const std::string path = WriteTemp("shortshaper", "LUT_1D_SIZE 4\n0 0 0\n1 1 1\n");
        Lut3D lut;
        std::string error;
        CHECK(!ParseCube(path, &lut, &error));
        CHECK(error.find("LUT_1D_SIZE") != std::string::npos);
        std::filesystem::remove(path);

        const std::string bad = WriteTemp("badrange", "LUT_1D_SIZE 2\nLUT_1D_INPUT_RANGE 1 0\n0 0 0\n1 1 1\n");
        CHECK(!ParseCube(bad, &lut, &error));
        CHECK(error.find("LUT_1D_INPUT_RANGE") != std::string::npos);
        std::filesystem::remove(bad);
    }

    // A row count that doesn't match LUT_3D_SIZE is rejected (too few here).
    {
        const std::string path = WriteTemp("short", "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n");

        Lut3D lut;
        std::string error;
        CHECK(!ParseCube(path, &lut, &error));
        CHECK(!error.empty());

        std::filesystem::remove(path);
    }

    // Too many rows for the declared size is also rejected, not silently truncated.
    {
        std::string body = "LUT_3D_SIZE 2\n";
        for (int i = 0; i < 9; ++i)
            body += "0 0 0\n"; // one more than 2^3 = 8
        const std::string path = WriteTemp("long", body);

        Lut3D lut;
        std::string error;
        CHECK(!ParseCube(path, &lut, &error));

        std::filesystem::remove(path);
    }

    // No LUT_3D_SIZE at all.
    {
        const std::string path = WriteTemp("nosize", "0 0 0\n1 1 1\n");

        Lut3D lut;
        std::string error;
        CHECK(!ParseCube(path, &lut, &error));
        CHECK(!error.empty());

        std::filesystem::remove(path);
    }

    // LUT_3D_SIZE outside the supported range.
    {
        const std::string path = WriteTemp("toobig", "LUT_3D_SIZE 999\n");

        Lut3D lut;
        std::string error;
        CHECK(!ParseCube(path, &lut, &error));

        std::filesystem::remove(path);
    }

    // LUT_3D_SIZE with a non-numeric value, not just a missing or out-of-range one.
    {
        const std::string path = WriteTemp("notanumber", "LUT_3D_SIZE banana\n");

        Lut3D lut;
        std::string error;
        CHECK(!ParseCube(path, &lut, &error));

        std::filesystem::remove(path);
    }

    // A missing file is its own clear error, not a crash.
    {
        Lut3D lut;
        std::string error;
        CHECK(!ParseCube("nr_lut_cube_parse_smoke_does_not_exist.cube", &lut, &error));
        CHECK(!error.empty());
    }

    // Optional: a folder of .cube files; each must parse, and the report says which ones carry a shaper.
    if (argc > 1)
    {
        int loaded = 0, shaped = 0;
        for (const auto& e : std::filesystem::directory_iterator(argv[1]))
        {
            if (e.path().extension() != ".cube")
                continue;
            Lut3D lut;
            std::string error;
            if (!ParseCube(e.path().string(), &lut, &error))
            {
                printf("  %s: %s\n", e.path().filename().string().c_str(), error.c_str());
                ++fails;
                continue;
            }
            ++loaded;
            if (lut.shaperSize != 0)
            {
                ++shaped;
                printf("  %s: %s, baked at %d^3\n", e.path().filename().string().c_str(), DescribeLutSize(lut).c_str(),
                       lut.size);
            }
        }
        printf("folder: %d files parsed, %d with a 1D shaper\n", loaded, shaped);
    }

    printf(fails ? "FAILED: %d\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
