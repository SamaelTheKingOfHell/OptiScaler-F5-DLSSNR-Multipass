// Host test for misc/NotificationHistory.h (the Notifications page's data) and misc/LibraryPathCheck.h (the check that
// XeFGPath was honoured).
//
// Build: cl /nologo /std:c++20 /EHsc /W4 tests\notification_history_smoke.cpp
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include "../OptiScaler/misc/LibraryPathCheck.h"
#include "../OptiScaler/misc/NotificationHistory.h"

using namespace std::chrono;

static int fails = 0;
static void Check(bool ok, const std::string& label)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", label.c_str());
        ++fails;
    }
}

static bool Contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

static void History()
{
    using namespace NotificationHistory;
    const auto t0 = system_clock::now();

    Check(Snapshot().empty() && Total() == 0, "nothing at the start");

    Add(2, "Falling back", "Falling back to FSR 2.1.2", t0);
    Add(3, "", "Upscaler failed to run!", t0 + seconds(1));
    auto list = Snapshot();
    Check(list.size() == 2 && list[0].id < list[1].id, "two different toasts are two entries, oldest first");
    Check(list[0].title == "Falling back" && list[1].content == "Upscaler failed to run!", "title and content are kept");

    // The same toast back to back counts up instead of listing again.
    Add(3, "", "Upscaler failed to run!", t0 + seconds(5));
    Add(3, "", "Upscaler failed to run!", t0 + seconds(9));
    list = Snapshot();
    Check(list.size() == 2 && list[1].count == 3, "a repeated toast is one entry with a count");
    Check(list[1].first == t0 + seconds(1) && list[1].last == t0 + seconds(9), "first and last time of the repeats");
    Check(Total() == 4, "Total counts repeats");

    // Same text but another type, or not back to back: new entries.
    Add(2, "", "Upscaler failed to run!", t0 + seconds(10));
    Add(3, "", "Upscaler failed to run!", t0 + seconds(11));
    Check(Snapshot().size() == 4, "another type, or a different toast in between, starts a new entry");

    Add(1, nullptr, nullptr, t0);
    Check(Snapshot().back().title.empty() && Snapshot().back().content.empty(), "null strings are empty");

    Clear();
    Check(Snapshot().empty() && Total() == 7, "Clear empties the list but not the total");

    // The oldest entries go first once the list is full.
    for (size_t i = 0; i < kMaxEntries + 25; ++i)
        Add(1, "t", ("toast " + std::to_string(i)).c_str(), t0);
    list = Snapshot();
    Check(list.size() == kMaxEntries, "the list is capped");
    Check(list.front().content == "toast 25" && list.back().content == "toast " + std::to_string(kMaxEntries + 24),
          "the oldest are dropped, the newest kept");

    // Toasts come from many threads.
    Clear();
    std::vector<std::thread> threads;
    for (int n = 0; n < 8; ++n)
        threads.emplace_back(
            [n]
            {
                for (int i = 0; i < 500; ++i)
                    Add(1, "thread", ("n" + std::to_string(n) + "-" + std::to_string(i)).c_str());
            });
    for (auto& t : threads)
        t.join();
    Check(Snapshot().size() == kMaxEntries, "concurrent adds stay within the cap");
}

static void Paths()
{
    using namespace LibraryPathCheck;
    namespace fs = std::filesystem;

    const fs::path root = fs::temp_directory_path() / "optiscaler_pathcheck_smoke";
    fs::remove_all(root);
    fs::create_directories(root / "xefg_cuda");
    fs::create_directories(root / "OptiScaler");
    std::ofstream(root / "xefg_cuda" / "xefg_cuda.dll") << "x";
    std::ofstream(root / "OptiScaler" / "libxess_fg.dll") << "x";

    const std::wstring ours = (root / "xefg_cuda" / "xefg_cuda.dll").wstring();
    const std::wstring intel = (root / "OptiScaler" / "libxess_fg.dll").wstring();

    // Nothing configured
    Check(Check("XeFG", L"libxess_fg.dll", L"", intel).result == Result::NotSet, "empty path: nothing to check");
    Check(Check("XeFG", L"libxess_fg.dll", L"auto", intel).result == Result::NotSet, "auto: nothing to check");
    Check(Check("XeFG", L"libxess_fg.dll", L"AUTO", intel).result == Result::NotSet, "AUTO too");

    // The configured file was loaded
    auto v = Check("XeFG", L"libxess_fg.dll", ours, ours);
    Check(v.result == Result::Used && v.message.empty(), "the configured file was loaded");
    Check(Check("XeFG", L"libxess_fg.dll", (root / "xefg_cuda" / ".." / "xefg_cuda" / "xefg_cuda.dll").wstring(), ours)
                  .result == Result::Used,
          "the same file spelled another way is still the same file");

    // A folder: the library's own file name in it
    Check(Check("XeFG", L"libxess_fg.dll", (root / "OptiScaler").wstring(), intel).result == Result::Used,
          "a configured folder whose libxess_fg.dll was loaded");

    // The configured file does not exist: the usual mistake
    v = Check("XeFG", L"libxess_fg.dll", (root / "nowhere" / "xefg_cuda.dll").wstring(), intel);
    Check(v.result == Result::NotFound, "missing file");
    Check(Contains(v.message, "XeFGPath") && Contains(v.message, "nowhere") && Contains(v.message, "libxess_fg.dll"),
          "the message names the setting, the path and what was used instead");
    Check(!Contains(v.message, "relative"), "an absolute path gets no relative-path hint");

    v = Check("XeFG", L"libxess_fg.dll", L".\\OptiScaler\\xefg_cuda\\xefg_cuda.dll", intel);
    Check(v.result == Result::NotFound && Contains(v.message, "full path"),
          "a relative path that is not found says to use the full path");

    v = Check("XeFG", L"libxess_fg.dll", (root / "nowhere" / "xefg_cuda.dll").wstring(), L"");
    Check(v.result == Result::NotFound && Contains(v.message, "could not be loaded"), "nothing loaded at all");

    // It exists, but another file was loaded
    v = Check("XeFG", L"libxess_fg.dll", ours, intel);
    Check(v.result == Result::NotUsed && Contains(v.message, "was not loaded"), "exists but not the one loaded");
    Check(Check("XeFG", L"libxess_fg.dll", ours, L"").result == Result::NotUsed, "exists, none loaded");

    fs::remove_all(root);
}

int main()
{
    History();
    Paths();

    if (fails == 0)
        std::printf("PASS\n");
    return fails == 0 ? 0 : 1;
}
