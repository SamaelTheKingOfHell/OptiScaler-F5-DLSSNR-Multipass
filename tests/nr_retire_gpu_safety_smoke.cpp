// Host check of DlssNr::Retire (shaders/dlssnr/DlssNr_Retire.h) with a tracker that keeps DlssNr::GpuLifetime's contract:
// a parked DLSS-NR feature or scratch texture must not be freed while GPU work that used it is still in flight. Freeing
// one under the GPU is a page fault on the freed resource and a device hang (issue #271: DRED names a freed
// nv.ngx.dlssnr.resource while the Optical F5Low list, which records the NR evaluate, was running).
//
// The simulated game renders a frame per step. Each NR caller records on its own list from a ring (reset before it is
// recorded again, as a game does), and its Dispatch counts the parked items down with its own epoch. With frame
// generation the frame count moves on every displayed frame, several per rendered frame; the rule counts one step per
// new epoch value however far it moved, so that makes no difference (the table shows it). Every few frames a
// feature is rebuilt in the middle of a recording, after this frame's evaluate was recorded on the still open list, and
// the old one parked. The lists are submitted at the end of the frame; the GPU finishes a frame's work `lag` rendered
// frames later. A free of an item some unfinished or unsubmitted list used is counted as unsafe. No GPU and no game.
// cl /std:c++20 /EHsc /W4 tests/nr_retire_gpu_safety_smoke.cpp
#include "../OptiScaler/shaders/dlssnr/DlssNr_Retire.h"

#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <vector>

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

// How the GPU-done answer is made.
enum class Done
{
    Tracker,       // GpuLifetime's contract (what production does)
    FenceOnly,     // broken: a fence after each submit, no wait for the reset (misses the open, unsubmitted list)
    EpochsOnly,    // the old rule: done as soon as asked
    NeverReported, // the hooks are not on: nothing is ever confirmed
};

// GpuLifetime's contract (dlssnr/DlssNr_GpuLifetime.h): a recording stays open until its list is reset; it is complete
// once closed and its last submit has finished (a recording never submitted counts as finished, as there).
// Retire snapshots every incomplete recording; its flag is set at a Collect once all of them are complete.
struct Tracker
{
    struct Recording
    {
        bool open = true;
        int submitted = 0; // the serial of its last submit, 0 for none
    };

    std::vector<Recording> recordings;
    std::map<int, int> openOf; // list -> its open recording
    struct Retired
    {
        std::vector<int> uses;
        std::shared_ptr<bool> flag;
    };
    std::vector<Retired> retired;
    const int* finished = nullptr; // the queue's finished serial
    bool fenceOnly = false;        // Done::FenceOnly

    bool Complete(int r) const
    {
        if (fenceOnly)
            return recordings[r].submitted != 0 && *finished >= recordings[r].submitted;

        return !recordings[r].open && (recordings[r].submitted == 0 || *finished >= recordings[r].submitted);
    }

    void Record(int list)
    {
        if (openOf.count(list) == 0)
        {
            openOf[list] = (int) recordings.size();
            recordings.push_back({});
        }
    }

    void Submitted(int list, int serial)
    {
        if (openOf.count(list))
            recordings[openOf[list]].submitted = serial;
    }

    void Reset(int list)
    {
        if (openOf.count(list))
        {
            recordings[openOf[list]].open = false;
            openOf.erase(list);
        }
    }

    std::shared_ptr<bool> Retire()
    {
        Retired r;
        r.flag = std::make_shared<bool>(false);

        for (int i = 0; i < (int) recordings.size(); ++i)
            if (!Complete(i) && !(fenceOnly && recordings[i].submitted == 0))
                r.uses.push_back(i);

        retired.push_back(r);
        return r.flag;
    }

    void Collect()
    {
        for (size_t i = 0; i < retired.size();)
        {
            bool all = true;

            for (int r : retired[i].uses)
                all = all && Complete(r);

            if (!all)
            {
                ++i;
                continue;
            }

            *retired[i].flag = true;
            retired.erase(retired.begin() + i);
        }
    }
};

struct Item
{
    int id = 0;
    std::shared_ptr<bool> done; // null: never confirmed
    int parkedAtTick = 0;
};

struct Run
{
    int unsafe = 0;      // frees of an item an unfinished or unsubmitted list still used
    int freed = 0;       // all frees
    int unconfirmed = 0; // frees the GPU did not confirm (kStuckFrames ran out)
    int early = 0;       // confirmed frees sooner than kFrames counts after parking
    int leftover = 0;    // still parked at the end
};

struct Setup
{
    int callers = 1;          // NR calls per rendered frame, each on its own list
    int epochsPerFrame = 1;   // displayed frames per rendered frame (frame generation's multiplier)
    bool sharedEpoch = false; // every caller hands the same epoch (one frame count), not one of its own
    int lag = 1;              // rendered frames the GPU runs behind
    Done done = Done::Tracker;
    int rebuildEvery = 10;
    int frames = 1500;
};

static Run Simulate(const Setup& s)
{
    DlssNr::Retire::List<Item> list;
    Tracker tracker;
    Run run;

    struct Submit
    {
        int frame;
        int serial;
        std::set<int> uses;
    };
    std::deque<Submit> inFlight;
    int finished = 0, serial = 0;
    tracker.finished = &finished;
    tracker.fenceOnly = s.done == Done::FenceOnly;

    // What each open list has recorded and not submitted yet.
    std::map<int, std::set<int>> recordedOn;
    const int ring = s.lag + 2; // enough lists in flight that the game never waits on the GPU
    int feature = 1, ticks = 0;
    uint64_t lastEpoch = ~0ull;

    const auto inUse = [&](int id)
    {
        for (const auto& submit : inFlight)
            if (submit.uses.count(id))
                return true;

        for (const auto& [l, uses] : recordedOn)
            if (uses.count(id))
                return true;

        return false;
    };

    const auto isDone = [&](const Item& item)
    {
        if (s.done == Done::EpochsOnly)
            return true;

        return item.done != nullptr && *item.done;
    };

    const auto free = [&](const Item& item, bool gpuConfirmed)
    {
        ++run.freed;
        run.unconfirmed += gpuConfirmed ? 0 : 1;
        run.unsafe += inUse(item.id) ? 1 : 0;
        run.early += gpuConfirmed && ticks - item.parkedAtTick < DlssNr::Retire::kFrames ? 1 : 0;
    };

    for (int f = 0; f < s.frames; ++f)
    {
        while (!inFlight.empty() && inFlight.front().frame <= f - s.lag)
        {
            finished = inFlight.front().serial;
            inFlight.pop_front();
        }

        for (int c = 0; c < s.callers; ++c)
        {
            const int cmd = c * 1000 + f % ring;
            tracker.Reset(cmd); // the game resets the list before recording it again

            // DlssNr_Dx12::Dispatch: WatchNrList (the list recorded, a new shared flag), then the tick.
            const bool tracked = s.done == Done::Tracker || s.done == Done::FenceOnly;
            if (tracked)
                tracker.Record(cmd);

            std::shared_ptr<bool> parkDone;
            const uint64_t epoch =
                (s.sharedEpoch ? 0ull : 1'000'000ull * (c + 1)) + (uint64_t) f * s.epochsPerFrame;
            if (epoch != lastEpoch)
                ++ticks;
            lastEpoch = epoch;

            tracker.Collect();
            list.Tick(epoch, isDone, free);

            // The evaluate on the current feature, then (in the middle of this recording) a rebuild parks it.
            recordedOn[cmd].insert(feature);

            if (c == s.callers - 1 && f % s.rebuildEvery == s.rebuildEvery - 1)
            {
                if (tracked && parkDone == nullptr)
                    parkDone = tracker.Retire();

                list.Park({ feature++, parkDone, ticks });
                recordedOn[cmd].insert(feature); // the new feature's first evaluate, on the same list
            }
        }

        for (auto& [cmd, uses] : recordedOn)
        {
            inFlight.push_back({ f, ++serial, uses });
            tracker.Submitted(cmd, serial);
        }

        recordedOn.clear();
    }

    run.leftover = (int) list.Size();
    return run;
}

// The smallest lag (in rendered frames) at which a setup frees an item under the GPU, or 0 up to `maxLag`.
static int FirstUnsafeLag(Setup s, int maxLag)
{
    for (s.lag = 1; s.lag <= maxLag; ++s.lag)
        if (Simulate(s).unsafe > 0)
            return s.lag;

    return 0;
}

int main()
{
    constexpr int kMaxLag = 48;

    // What the old rule (epochs only) did: the GPU lag at which it frees under in-flight work.
    printf("old rule (epochs only) | first GPU lag, rendered frames, that frees under the GPU\n");
    printf("NR calls/frame |   FG 1x |   FG 2x |   FG 4x\n");
    for (int callers = 1; callers <= 4; ++callers)
    {
        printf("%14d", callers);
        for (int fg : { 1, 2, 4 })
        {
            Setup s;
            s.callers = callers;
            s.epochsPerFrame = fg;
            s.done = Done::EpochsOnly;
            const int lag = FirstUnsafeLag(s, kMaxLag);
            printf(" | %7d", lag);
            // The sweep finds it: the old rule is unsafe at some lag in every setup.
            CHECK(lag > 0);
        }
        printf("\n");
    }

    // The fix: never under the GPU, any setup, any lag; items get freed, confirmed by the GPU, no sooner than kFrames.
    for (int callers = 1; callers <= 4; ++callers)
        for (int fg : { 1, 2, 4 })
            for (bool shared : { false, true })
                for (int lag = 1; lag <= kMaxLag; ++lag)
                {
                    Setup s;
                    s.callers = callers;
                    s.epochsPerFrame = fg;
                    s.sharedEpoch = shared;
                    s.lag = lag;
                    const Run run = Simulate(s);

                    if (run.unsafe != 0 || run.freed == 0 || run.unconfirmed != 0 || run.early != 0)
                    {
                        printf("FAIL: %d calls/frame, FG %dx, %s epochs, GPU %d frames behind: %d unsafe, %d freed, "
                               "%d unconfirmed, %d early\n",
                               callers, fg, shared ? "shared" : "own", lag, run.unsafe, run.freed, run.unconfirmed,
                               run.early);
                        ++fails;
                    }
                }

    // Callers that share one epoch count down once per frame, not once per call: with 4 callers an item still waits
    // kFrames rendered frames (at 1x), so at a lag the GPU never finished it.
    {
        Setup s;
        s.callers = 4;
        s.sharedEpoch = true;
        s.done = Done::EpochsOnly;
        CHECK(FirstUnsafeLag(s, kMaxLag) == DlssNr::Retire::kFrames + 1);
    }

    // Nothing piles up: every parked item but the last few is freed.
    {
        Setup s;
        s.callers = 2;
        s.lag = 3;
        const Run run = Simulate(s);
        CHECK(run.leftover <= DlssNr::Retire::kFrames);
    }

    // A plain fence after each submit misses the list the rebuild happened on (still open, not submitted): the test sees
    // it once the GPU is further behind than the kFrames floor, which hides it below that. GpuLifetime's wait for the
    // reset is what covers that list.
    {
        Setup s;
        s.done = Done::FenceOnly;
        s.lag = DlssNr::Retire::kFrames + 8;
        CHECK(Simulate(s).unsafe > 0);
    }

    // The hooks are not on: nothing is confirmed, items still go (by time, told so), and not under the GPU at the lags
    // and rates a game runs at. The time is a last resort, not a promise: far enough behind it would free under the GPU.
    for (int callers = 1; callers <= 2; ++callers)
        for (int fg : { 1, 4 })
            for (int lag = 1; lag <= 8; ++lag)
            {
                Setup s;
                s.callers = callers;
                s.epochsPerFrame = fg;
                s.lag = lag;
                s.done = Done::NeverReported;
                const Run run = Simulate(s);
                CHECK(run.unsafe == 0);
                CHECK(run.freed > 0);
                CHECK(run.unconfirmed == run.freed);
            }

    // Shutdown's free-all frees everything, whatever its count.
    {
        DlssNr::Retire::List<Item> list;
        list.Park({ 1 });
        list.Park({ 2 });
        int freed = 0;
        list.FreeAll([&](const Item&) { ++freed; });
        CHECK(freed == 2);
        CHECK(list.Size() == 0);
    }

    if (fails == 0)
        printf("retire gpu safety: ok\n");

    return fails == 0 ? 0 : 1;
}
