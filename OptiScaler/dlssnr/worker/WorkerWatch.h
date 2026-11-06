#pragma once

#include <d3d12.h>
#include <windows.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>

namespace DlssNr::Worker
{
// Watches one outstanding worker frame at a time on its own thread.
//
// Two jobs:
// - Timing: the CPU moments the primary's ready signal and the worker's done signal complete. done - ready is
//   the helper's whole turnaround (copies, passes, return). Consecutive ready signals give the GPU frame period.
// - Watchdog: when the primary queue waits on the done fence on the GPU, a dead or stalled worker would block
//   the game's queue forever. On worker exit, or done not following ready within the stall limit, the worker is
//   terminated and the done fence is signalled from the CPU so the queue continues. That frame's helper rows are
//   stale and the generation reports Failed().
// ```cpp
// watch.Start(readyFence, doneFence, process);
// watch.Arm(frameNumber);              // after the outbound signal was queued
// WorkerWatch::Sample s; if (watch.Take(s)) controller.Observe(s.latencyMs, s.periodMs, now);
// ```
class WorkerWatch
{
  public:
    struct Sample
    {
        uint64_t frame = 0;
        double latencyMs = 0, periodMs = 0;
    };
    static constexpr uint64_t RescueValue = 1ull << 62;
    static constexpr DWORD StallLimitMs = 3000;

    WorkerWatch() = default;
    WorkerWatch(const WorkerWatch&) = delete;
    WorkerWatch& operator=(const WorkerWatch&) = delete;
    ~WorkerWatch() { Stop(); }

    bool Start(ID3D12Fence* ready, ID3D12Fence* done, HANDLE process)
    {
        if (thread_.joinable())
            return true;
        if (!ready || !done || !process)
            return false;
        ready_ = ready;
        done_ = done;
        process_ = process;
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        arm_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        readyEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        doneEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!stop_ || !arm_ || !readyEvent_ || !doneEvent_)
        {
            CloseEvents();
            return false;
        }
        try
        {
            thread_ = std::thread([this] { Run(); });
        }
        catch (...)
        {
            CloseEvents();
            return false;
        }
        return true;
    }
    void Arm(uint64_t frame)
    {
        armed_.store(frame, std::memory_order_release);
        if (arm_)
            SetEvent(arm_);
    }
    bool Failed() const { return failed_.load(std::memory_order_acquire); }
    bool Running() const { return thread_.joinable() && !Failed(); }
    // Newest completed sample not yet taken.
    bool Take(Sample& out)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!fresh_)
            return false;
        out = sample_;
        fresh_ = false;
        return true;
    }
    void Stop()
    {
        if (thread_.joinable())
        {
            SetEvent(stop_);
            thread_.join();
        }
        CloseEvents();
    }

  private:
    using Clock = std::chrono::steady_clock;
    Microsoft::WRL::ComPtr<ID3D12Fence> ready_, done_;
    HANDLE process_ = nullptr, stop_ = nullptr, arm_ = nullptr, readyEvent_ = nullptr, doneEvent_ = nullptr;
    std::thread thread_;
    std::atomic<uint64_t> armed_ { 0 };
    std::atomic<bool> failed_ { false };
    std::mutex mutex_;
    Sample sample_ {};
    bool fresh_ = false;

    void CloseEvents()
    {
        for (HANDLE* h : { &stop_, &arm_, &readyEvent_, &doneEvent_ })
            if (*h)
            {
                CloseHandle(*h);
                *h = nullptr;
            }
    }
    void Rescue(bool terminate)
    {
        if (terminate)
            TerminateProcess(process_, 3);
        failed_.store(true, std::memory_order_release);
        // Releases any GPU queue waiting on this fence. The worker no longer writes it.
        done_->Signal(RescueValue);
    }
    // 0 = event, 1 = worker exited, 2 = stop, 3 = timeout
    int WaitFor(HANDLE event, DWORD timeoutMs)
    {
        HANDLE handles[] = { event, process_, stop_ };
        const DWORD r = WaitForMultipleObjects(3, handles, FALSE, timeoutMs);
        if (r == WAIT_OBJECT_0)
            return 0;
        if (r == WAIT_OBJECT_0 + 1)
            return 1;
        if (r == WAIT_OBJECT_0 + 2)
            return 2;
        return 3;
    }
    void Run()
    {
        uint64_t watched = 0, previousFrame = 0;
        Clock::time_point previousReady {};
        for (;;)
        {
            const int armed = WaitFor(arm_, INFINITE);
            if (armed == 2)
                return;
            if (armed == 1)
            {
                Rescue(false);
                return;
            }
            const uint64_t frame = armed_.load(std::memory_order_acquire);
            if (!frame || frame <= watched)
                continue;
            watched = frame;
            if (FAILED(ready_->SetEventOnCompletion(frame, readyEvent_)))
            {
                Rescue(true);
                return;
            }
            const int readyResult = WaitFor(readyEvent_, INFINITE);
            if (readyResult == 2)
                return;
            if (readyResult == 1)
            {
                Rescue(false);
                return;
            }
            const auto readyAt = Clock::now();
            if (FAILED(done_->SetEventOnCompletion(frame, doneEvent_)))
            {
                Rescue(true);
                return;
            }
            const int doneResult = WaitFor(doneEvent_, StallLimitMs);
            if (doneResult == 2)
                return;
            if (doneResult != 0)
            {
                Rescue(doneResult == 3);
                return;
            }
            const auto doneAt = Clock::now();
            Sample s;
            s.frame = frame;
            s.latencyMs = std::chrono::duration<double, std::milli>(doneAt - readyAt).count();
            s.periodMs = previousFrame + 1 == frame
                             ? std::chrono::duration<double, std::milli>(readyAt - previousReady).count()
                             : 0.0;
            previousFrame = frame;
            previousReady = readyAt;
            std::lock_guard<std::mutex> lock(mutex_);
            sample_ = s;
            fresh_ = true;
        }
    }
};
} // namespace DlssNr::Worker
