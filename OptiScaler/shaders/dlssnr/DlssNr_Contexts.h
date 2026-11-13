#pragma once

#include <cstdint>

// A game can keep more than one DLSS feature alive and evaluate them in turn: Neverness to Everness makes a second one
// for the 3D character preview on its Character, Gift and Party pages, next to the world's, and calls both every frame.
// NR has one model and one history, so the two pictures took turns in it: each got the other's history, and before SR
// the model was rebuilt on every call (their colour formats differ), which froze the game.
//
// This decides, per game DLSS feature, what NR does with it. The main one keeps NR's ordinary state, as in a game with
// one feature. Any other one stands aside (passes untouched), or with SecondaryFeatures = Own model gets a view slot of
// its own (DlssNr_Dx12.cpp: a model, its surfaces and history), or with Share runs through the main state as before.
//
// Not every second feature is a second picture: Assetto Corsa EVO draws its triple-screen views with one feature per
// screen, all into one colour allocation (DlssNr_Views.h sorts those out). A feature whose colour is main's allocation
// is a view of main's picture and goes on as main.
//
// Which feature is main: the first one, unless a picture clearly larger than main's arrives (the world, not a preview
// or a scope), or main has gone quiet (a menu scene's feature the game never released). After the game releases main
// (it makes its feature again on a resolution change, and NR keeps its model through that, as it always did), main goes
// to the first feature that shows main's picture again, is new, or was made just before the release; one already known
// as another picture (the preview, still open) waits a while.
//
// Counted in calls, not presents: the presents are not counted where the swapchain is not wrapped. Nothing here touches
// D3D12. Host-testable (tests/nr_contexts_smoke.cpp).
namespace DlssNr::Contexts
{
// [DlssNr] SecondaryFeatures
enum class Mode : uint32_t
{
    StandAside = 0,
    OwnModel = 1,
    Share = 2,
};

constexpr unsigned kOwnSlots = 3; // features with a model of their own besides the main one
// Calls by other features without one from main (about 120 frames of two NR seams each) before main counts as quiet,
// and how long a known other picture waits for main's successor after a release.
constexpr unsigned long long kQuietCalls = 240;
// A feature first seen at most this many calls before main's release is main's successor (made before the old one went)
constexpr unsigned long long kSuccessorCalls = 240;
// After main's release, a feature that is the only one calling for this many calls is main's successor (a game with one
// feature making it again in another colour format, HDR switched on): about 30 frames
constexpr unsigned long long kSoloCalls = 60;
// A picture with this much more area than main's takes main
constexpr unsigned long long kLargerNum = 5, kLargerDen = 4;

// The colour a call works on: the allocation and its size and format
struct Picture
{
    uint64_t resource = 0;
    unsigned width = 0, height = 0; // the allocation
    int format = 0;
    // The size DLSS makes of it, when the game says (else the allocation): steady under dynamic resolution, unlike
    // the render size
    unsigned outputWidth = 0, outputHeight = 0;

    unsigned long long Area() const
    {
        return outputWidth != 0 ? (unsigned long long) outputWidth * outputHeight : (unsigned long long) width * height;
    }
    // The same resource; size and format only when a call does not say which resource it is (a preview of the
    // world's size and format is another picture)
    bool SameAllocation(const Picture& o) const
    {
        if (resource != 0 && o.resource != 0)
            return resource == o.resource;
        return width != 0 && width == o.width && height == o.height && format == o.format;
    }
};

enum class Kind
{
    Main,
    Own, // slot: 0 .. kOwnSlots - 1
    StandAside,
};

struct Role
{
    Kind kind = Kind::Main;
    unsigned slot = 0;
    bool first = false;    // the first call this feature made in this role (to say it once)
    bool tookMain = false; // main moved to this feature from another picture: its history does not carry over
    int freedSlot = -1;    // an own slot this call gave up (its model is to be parked)
};

// What releasing a game feature leaves to the caller.
struct Released
{
    bool own = false; // its own slot (slot) is to be parked
    unsigned slot = 0;
};

class Map
{
  public:
    // handle 0: the caller does not know the game's feature (the native path): main.
    Role Acquire(uint32_t handle, Mode mode, const Picture& picture)
    {
        Role role;

        if (handle == 0)
            return role;

        ++calls;

        // Share: every feature through the main state, as before; one that had a model of its own gives it back. Main
        // is still followed, so leaving Share does not find it quiet.
        if (mode == Mode::Share)
        {
            if (!haveMain || handle == main)
            {
                main = handle;
                haveMain = true;
                haveReleased = false;
                mainLast = calls;
                mainPicture = picture;
            }
            Forget(handle, role);
            return role;
        }

        if (haveMain && handle == main)
        {
            mainLast = calls;
            mainPicture = picture;
            return role;
        }

        if (!haveMain)
        {
            // Main's successor shows main's picture: the same allocation, or (made again at another size) its colour
            // format, being new or made just before the release. A known other picture, or one in another format,
            // waits kQuietCalls in case main's successor comes.
            // A feature calling alone since the release is it too, whatever its format.
            if (soloHandle == handle)
                ++soloCalls;
            else
            {
                otherCalled = otherCalled || soloHandle != 0;
                soloHandle = handle;
                soloCalls = 1;
            }

            const Entry* known = Find(handle);
            const bool sameFormat = picture.format == releasedPicture.format;
            const bool alone = !otherCalled && soloCalls >= kSoloCalls;
            const bool successor = !haveReleased || picture.SameAllocation(releasedPicture) ||
                                   (sameFormat && (!known || known->firstSeen + kSuccessorCalls >= releasedAt)) ||
                                   alone || calls - releasedAt > kQuietCalls;
            if (successor)
                return TakeMain(handle, picture, known != nullptr);
        }
        else
        {
            // A view of main's picture (one feature per screen of a multi-monitor layout). One that stood aside or had
            // a model of its own while main was another picture gives that up.
            if (picture.SameAllocation(mainPicture))
            {
                mainLast = calls;
                Forget(handle, role);
                return role;
            }

            const bool larger = picture.Area() * kLargerDen > mainPicture.Area() * kLargerNum;
            if (larger || calls - mainLast > kQuietCalls)
                return TakeMain(handle, picture, true);
        }

        return Secondary(handle, mode);
    }

    // The main feature's state is kept for its successor (Acquire). An own slot is handed back to be parked.
    Released Release(uint32_t handle)
    {
        Released r;

        if (haveMain && handle == main)
        {
            haveMain = false;
            haveReleased = true;
            releasedPicture = mainPicture;
            releasedAt = calls;
            soloHandle = 0;
            soloCalls = 0;
            otherCalled = false;
        }

        for (unsigned i = 0; i < kEntries; ++i)
            if (entries[i].handle == handle)
            {
                if (entries[i].own)
                {
                    r.own = true;
                    r.slot = entries[i].slot;
                }
                entries[i] = Entry {};
            }

        return r;
    }

    // Its own model could not be made or failed: it stands aside from now on, whatever the setting. Returns the slot to
    // park, or -1.
    int Failed(uint32_t handle)
    {
        Entry* e = Find(handle);
        if (e == nullptr)
            return -1;
        const int slot = e->own ? (int) e->slot : -1;
        e->own = false;
        e->failed = true;
        return slot;
    }

    // For the menu: the other features standing aside, and those with a model of their own
    unsigned AsideCount() const { return Count(false); }
    unsigned OwnCount() const { return Count(true); }

    void Reset() { *this = Map {}; }

  private:
    struct Entry
    {
        uint32_t handle = 0;
        unsigned long long firstSeen = 0;
        bool own = false;
        unsigned slot = 0;
        bool failed = false; // its own model failed: aside for good
    };
    static constexpr unsigned kEntries = 8;

    Entry* Find(uint32_t handle)
    {
        for (unsigned i = 0; i < kEntries; ++i)
            if (entries[i].handle == handle)
                return &entries[i];
        return nullptr;
    }

    // The feature is no other picture any more: its entry goes, and its own slot with it (to be parked)
    void Forget(uint32_t handle, Role& role)
    {
        if (Entry* e = Find(handle))
        {
            if (e->own)
                role.freedSlot = (int) e->slot;
            *e = Entry {};
        }
    }

    unsigned Count(bool own) const
    {
        unsigned n = 0;
        for (unsigned i = 0; i < kEntries; ++i)
            n += entries[i].handle != 0 && entries[i].own == own ? 1 : 0;
        return n;
    }

    // tookMain: main was another picture's (or this one stood aside until now)
    Role TakeMain(uint32_t handle, const Picture& picture, bool otherPicture)
    {
        Role role;
        role.first = true;
        role.tookMain = otherPicture;

        if (Entry* e = Find(handle))
        {
            if (e->own)
                role.freedSlot = (int) e->slot;
            *e = Entry {};
        }

        // The old main, if still alive, is another feature from now on (it gets its role on its next call)
        main = handle;
        haveMain = true;
        haveReleased = false;
        mainLast = calls;
        mainPicture = picture;
        return role;
    }

    Role Secondary(uint32_t handle, Mode mode)
    {
        Role role;
        role.kind = Kind::StandAside;
        Entry* e = Find(handle);

        if (e == nullptr)
        {
            for (unsigned i = 0; i < kEntries && e == nullptr; ++i)
                if (entries[i].handle == 0)
                {
                    e = &entries[i];
                    e->handle = handle;
                    e->firstSeen = calls;
                    role.first = true;
                }

            if (e == nullptr)
                return role; // more features than remembered: they stand aside, unsaid
        }

        // The setting moved off Own model: the slot goes
        if (e->own && mode != Mode::OwnModel)
        {
            role.freedSlot = (int) e->slot;
            e->own = false;
            role.first = true;
        }

        if (mode == Mode::OwnModel && !e->own && !e->failed)
        {
            for (unsigned s = 0; s < kOwnSlots; ++s)
                if (!SlotUsed(s))
                {
                    e->own = true;
                    e->slot = s;
                    role.first = true;
                    break;
                }
        }

        if (e->own)
        {
            role.kind = Kind::Own;
            role.slot = e->slot;
        }

        return role;
    }

    bool SlotUsed(unsigned slot) const
    {
        for (unsigned i = 0; i < kEntries; ++i)
            if (entries[i].handle != 0 && entries[i].own && entries[i].slot == slot)
                return true;
        return false;
    }

    unsigned long long calls = 0;
    uint32_t main = 0;
    bool haveMain = false;
    unsigned long long mainLast = 0;
    Picture mainPicture {};
    bool haveReleased = false;
    Picture releasedPicture {};
    unsigned long long releasedAt = 0;
    uint32_t soloHandle = 0; // since main's release: the feature calling, how often in a row, and whether another did
    unsigned long long soloCalls = 0;
    bool otherCalled = false;
    Entry entries[kEntries] = {};
};
} // namespace DlssNr::Contexts
