#pragma once

// When DLSS-NR's parked model features and scratch textures are freed (DlssNr_Dx12.cpp: ParkNrFeature, ParkNrResource,
// the tick at the top of DlssNr_Dx12::Dispatch). Kept apart, with no D3D12 or NGX types, so
// tests/nr_retire_gpu_safety_smoke.cpp runs the same rule against a simulated GPU queue.
//
// A parked item is freed once the GPU has finished every list NR recorded before it was parked (`done`, from
// DlssNr::GpuLifetime), and no sooner than kFrames submission epochs after parking. Counting epochs alone was not
// enough: with several NR calls per frame, each with its own epoch, or a GPU running far behind, an item was freed
// under work still in flight (issue #271). An epoch is what the caller hands Dispatch
// (DlssNrFrameInfo::SubmissionEpoch); the first call with a new value counts every parked item down once.
//
// Should the lists never be reported done (a submit or reset the hooks did not see, or the hooks are not on), the item
// would stay forever: it is freed kStuckFrames epochs after its count ran out, and `free` is told the GPU did not
// confirm it. That last resort is a plain timer again, only a far longer one (632 epochs: over 150 rendered frames even
// with four epochs each).

#include <cstddef>
#include <cstdint>
#include <vector>

namespace DlssNr::Retire
{

constexpr int kFrames = 32;
constexpr int kStuckFrames = 600;

template <typename Item> class List
{
  public:
    void Park(const Item& item) { _parked.push_back({ item, kFrames }); }

    // One count down per submission epoch: a call with the epoch of the last one does nothing. `done(item)` says the
    // GPU finished with it; `free(item, gpuConfirmed)` frees it.
    template <typename DoneFn, typename FreeFn> void Tick(uint64_t epoch, DoneFn&& done, FreeFn&& free)
    {
        if (epoch == _lastEpoch)
            return;

        _lastEpoch = epoch;

        for (size_t i = 0; i < _parked.size();)
        {
            Parked& parked = _parked[i];

            if (parked.framesLeft > -kStuckFrames)
                --parked.framesLeft;

            const bool confirmed = parked.framesLeft <= 0 && done(parked.item);

            if (!confirmed && parked.framesLeft > -kStuckFrames)
            {
                ++i;
                continue;
            }

            free(parked.item, confirmed);
            _parked.erase(_parked.begin() + i);
        }
    }

    // Everything now, whatever its count (DlssNr::Shutdown): `free(item)`.
    template <typename FreeFn> void FreeAll(FreeFn&& free)
    {
        for (auto& parked : _parked)
            free(parked.item);

        _parked.clear();
    }

    size_t Size() const { return _parked.size(); }

  private:
    struct Parked
    {
        Item item;
        int framesLeft;
    };

    std::vector<Parked> _parked;
    uint64_t _lastEpoch = ~0ull;
};

} // namespace DlssNr::Retire
