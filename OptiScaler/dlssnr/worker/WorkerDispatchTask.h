#pragma once

#include <coroutine>
#include <exception>
#include <utility>

namespace DlssNr::Worker
{
// A single NR recording may yield after its outbound submission. The owner
// resumes it before any other NR recording or resource invalidation.
class DispatchTask
{
  public:
    struct promise_type;
    using Handle = std::coroutine_handle<promise_type>;
    struct promise_type
    {
        std::exception_ptr error;
        DispatchTask get_return_object() { return DispatchTask(Handle::from_promise(*this)); }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { error = std::current_exception(); }
    };

  private:
    Handle handle_ {};
    explicit DispatchTask(Handle handle) : handle_(handle) {}

  public:
    DispatchTask() = default;
    DispatchTask(const DispatchTask&) = delete;
    DispatchTask& operator=(const DispatchTask&) = delete;
    DispatchTask(DispatchTask&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    DispatchTask& operator=(DispatchTask&& other) noexcept
    {
        if (this != &other)
        {
            if (handle_ && handle_.done()) handle_.destroy();
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }
    // An untrackable submission is quarantined by its owner. Never unwind a
    // suspended recording and release objects that may still be used by the GPU.
    ~DispatchTask() { if (handle_ && handle_.done()) handle_.destroy(); }
    bool Pending() const { return handle_ && !handle_.done(); }
    bool Failed() const { return handle_ && handle_.promise().error != nullptr; }
    void Resume() { if (Pending()) handle_.resume(); }
};
} // namespace DlssNr::Worker
