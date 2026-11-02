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
enum class UnavailableReason { Other, NoAdapter, Transfer, MissingFiles, Startup };
inline std::atomic<UnavailableReason> unavailableReason { UnavailableReason::Other };
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
        switch (unavailableReason.load(std::memory_order_relaxed))
        {
        case UnavailableReason::NoAdapter: return "No compatible second NVIDIA GPU; using the game GPU";
        case UnavailableReason::Transfer: return "Frame, guide or GPU transfer unsupported; see log; using the game GPU";
        case UnavailableReason::MissingFiles: return "Worker or model files missing; using the game GPU";
        case UnavailableReason::Startup: return "Could not start the second GPU worker; using the game GPU";
        default: return "Unavailable; see log; using the game GPU";
        }
    case Status::Failed:
        return "Worker stopped; using the game GPU";
    case Status::PrimaryOnly:
        return "All passes are on the game GPU";
    }
    return "Waiting";
}
} // namespace DlssNr::Worker
