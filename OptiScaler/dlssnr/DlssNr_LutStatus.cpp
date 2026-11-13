#include "pch.h"
#include "DlssNr_LutStatus.h"

#include <mutex>

namespace DlssNr
{
namespace
{
std::mutex g_mutex;
LutStatus g_status;
PostLutStatus g_postStatus;
unsigned long long g_gradedAfterNrMs = 0;
} // namespace

void ReportLutStatus(bool wanted, const std::string& loadedPath, int size, bool failed, const std::string& error,
                     const std::string& attemptedPath, float strength)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_status.seen = true;
    g_status.wanted = wanted;
    g_status.loaded = wanted && !loadedPath.empty();
    g_status.loadedPath = loadedPath;
    g_status.size = size;
    g_status.failed = failed;
    g_status.error = error;
    g_status.attemptedPath = attemptedPath;
    g_status.strength = strength;
}

LutStatus ReadLutStatus()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_status;
}

void ReportPostLutStatus(const PostLutStatus& status)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_postStatus = status;
    g_postStatus.seen = true;
    g_postStatus.reportedMs = GetTickCount64();
    if (status.stage == PostLutStage::AfterNr && status.loaded)
        g_gradedAfterNrMs = g_postStatus.reportedMs;
}

bool PostLutGradedAfterNrWithin(unsigned long long ms)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_gradedAfterNrMs != 0 && GetTickCount64() - g_gradedAfterNrMs < ms;
}

PostLutStatus ReadPostLutStatus()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_postStatus;
}
} // namespace DlssNr
