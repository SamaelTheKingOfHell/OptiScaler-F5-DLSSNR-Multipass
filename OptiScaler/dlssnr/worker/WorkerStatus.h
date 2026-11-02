#pragma once
#include <atomic>

namespace DlssNr::Worker
{
enum class Status
{
    Disabled,
    Waiting,
    Starting,
    Running,
    Unavailable,
    Failed,
    PrimaryOnly
};
inline std::atomic<Status> currentStatus { Status::Disabled };
inline const char* StatusText()
{
    switch (currentStatus.load(std::memory_order_relaxed))
    {
    case Status::Disabled:
        return "Off";
    case Status::Waiting:
        return "Waiting for a supported finished picture or previous frame";
    case Status::Starting:
        return "Preparing the secondary GPU";
    case Status::Running:
        return "Running with the second GPU";
    case Status::Unavailable:
        return "Unavailable for this configuration; using the game GPU";
    case Status::Failed:
        return "Worker stopped; using the game GPU";
    case Status::PrimaryOnly:
        return "All passes are on the game GPU";
    }
    return "Waiting";
}
} // namespace DlssNr::Worker
