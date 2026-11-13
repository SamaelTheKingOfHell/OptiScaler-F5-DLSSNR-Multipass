// Host test for menu/MenuPages.h: the main menu's page ids, their stable ini names, and the fallback for a missing or
// unknown saved page.
//
// Build: cl /nologo /std:c++20 /EHsc /W4 tests\menu_pages_smoke.cpp
#include <cstdio>
#include <set>
#include <string>
#include "../OptiScaler/menu/MenuPages.h"

using namespace MenuPages;

static int fails = 0;
static void Check(bool ok, const std::string& label)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", label.c_str());
        ++fails;
    }
}

int main()
{
    std::set<std::string> names;
    std::set<std::string> labels;

    for (size_t i = 0; i < kPageCount; ++i)
    {
        const Page page = static_cast<Page>(i);
        const std::string name = Name(page);
        const std::string label = Label(page);

        Check(!name.empty() && !label.empty(), "page " + std::to_string(i) + " has a name and a label");
        Check(PageFromName(name) == page, "page '" + name + "' round-trips through its ini name");
        Check(names.insert(name).second, "ini name '" + name + "' is unique");
        Check(labels.insert(label).second, "label '" + label + "' is unique");
        Check(IsNeuralRendering(page) == (name.rfind("nr.", 0) == 0),
              "NR pages are exactly the nr.* names ('" + name + "')");
    }

    Check(kPageCount == 17, "10 categories plus 7 Neural Rendering sub-pages");
    Check(std::string(Name(Page::Plugins)) == "plugins" && std::string(Label(Page::Plugins)) == "Plugins",
          "the Plugins page has its name and label");
    Check(static_cast<int>(Page::Plugins) == static_cast<int>(Page::Misc) + 1 &&
              static_cast<int>(Page::Notifications) == static_cast<int>(Page::Plugins) + 1 &&
              static_cast<int>(Page::F5Lut) == static_cast<int>(Page::Notifications) + 1 &&
              static_cast<int>(Page::F5Lut) + 1 == static_cast<int>(Page::NrF5Low),
          "Plugins, Notifications and F5LUT sit between Misc and the Neural Rendering group");
    Check(std::string(Name(Page::F5Lut)) == "f5lut" && std::string(Label(Page::F5Lut)) == "F5LUT" &&
              !IsNeuralRendering(Page::F5Lut) && PageFromName("f5lut") == Page::F5Lut,
          "the F5LUT page has its name and label and is not in the Neural Rendering group");
    Check(std::string(Name(Page::Notifications)) == "notifications" &&
              std::string(Label(Page::Notifications)) == "Notifications" && !IsNeuralRendering(Page::Notifications),
          "the Notifications page has its name and label");
    Check(!IsNeuralRendering(Page::Plugins), "Plugins is not a Neural Rendering page");
    Check(PageFromName("plugins") == Page::Plugins && PageFromName("misc") == Page::Misc,
          "saved page names still open the same pages");
    Check(std::string(Name(Page::NrF5Low)) == "nr.f5low", "the Optical F5Low page has its ini name");
    Check(static_cast<int>(Page::NrF5Low) + 1 == static_cast<int>(Page::NrStatus),
          "Optical F5Low comes first in the Neural Rendering group, right before Status & Presets");
    Check(!IsNeuralRendering(Page::Misc) && IsNeuralRendering(Page::NrF5Low), "the group starts at Optical F5Low");
    Check(PageFromName("nr.status") == Page::NrStatus, "an older saved nr.status still opens Status & Presets");
    Check(std::string(Name(Page::NrOutput)) == "nr.output", "the NR Output page keeps its documented ini name");
    Check(std::string(Name(Page::Upscaler)) == "upscaler", "the Upscaler page keeps its documented ini name");

    // Missing or unknown saved pages fall back to Upscaler.
    Check(PageFromName("") == Page::Upscaler, "empty name falls back to Upscaler");
    Check(PageFromName("auto") == Page::Upscaler, "'auto' (nothing saved) falls back to Upscaler");
    Check(PageFromName("nr.nonsense") == Page::Upscaler, "unknown NR name falls back to Upscaler");
    Check(PageFromName("Upscaler") == Page::Upscaler, "names are lower case; other spellings fall back");
    Check(PageFromName("nr.OUTPUT") == Page::Upscaler, "a wrong-case NR name falls back too");

    // Out-of-range ids are safe to name.
    Check(std::string(Name(static_cast<Page>(kPageCount))) == "upscaler", "out-of-range page names as Upscaler");

    // A page request is taken once.
    Check(!ConsumeRequest().has_value(), "no request waits at the start");
    RequestPage(Page::NrDebug);
    {
        const auto first = ConsumeRequest();
        Check(first.has_value() && *first == Page::NrDebug, "a request is handed over");
        Check(!ConsumeRequest().has_value(), "a request is consumed once");
    }
    RequestPage(Page::Misc);
    RequestPage(Page::NrOutput);
    {
        const auto last = ConsumeRequest();
        Check(last.has_value() && *last == Page::NrOutput, "the latest request wins");
    }

    if (fails == 0)
        std::printf("menu_pages_smoke: all checks passed (%zu pages)\n", kPageCount);

    return fails == 0 ? 0 : 1;
}
