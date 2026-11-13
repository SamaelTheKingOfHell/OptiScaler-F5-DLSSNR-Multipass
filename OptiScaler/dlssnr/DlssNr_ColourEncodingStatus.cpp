#include "pch.h"
#include "DlssNr_ColourEncodingStatus.h"

#include <cstring>
#include <mutex>

namespace DlssNr
{
namespace
{
std::mutex g_mutex;
DlssNrColourEncoding::Choice g_choice;
char g_format[48] = {};
bool g_seen = false;
bool g_finishedPicture = false;
} // namespace

void ReportColourEncoding(const DlssNrColourEncoding::Choice& choice, const char* formatName, const char* path,
                          bool finishedPicture)
{
    const char* format = formatName ? formatName : "";
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const bool same = g_seen && g_choice.encoding == choice.encoding && g_choice.automatic == choice.automatic &&
                          g_choice.mismatch == choice.mismatch && g_choice.refused == choice.refused &&
                          g_choice.reason == choice.reason &&
                          g_finishedPicture == finishedPicture &&
                          std::strncmp(g_format, format, sizeof(g_format) - 1) == 0;
        if (same)
            return;
        g_choice = choice;
        g_finishedPicture = finishedPicture;
        std::strncpy(g_format, format, sizeof(g_format) - 1);
        g_format[sizeof(g_format) - 1] = '\0';
        g_seen = true;
    }

    if (choice.mismatch || choice.refused)
        LOG_WARN("DLSS-NR {}{}colour encoding: {} {}", path ? path : "", path && *path ? " " : "",
                 DlssNrColourEncoding::Name(choice.encoding), DlssNrColourEncoding::MismatchWarning(choice));
}

ColourEncodingStatus ReadColourEncodingStatus()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    ColourEncodingStatus status;
    status.seen = g_seen;
    if (g_seen)
    {
        status.line = DlssNrColourEncoding::Describe(g_choice, g_format);
        status.warning = DlssNrColourEncoding::MismatchWarning(g_choice);
        status.exposureState = DlssNrExposureState::For(g_choice, g_finishedPicture);
        status.gammaConverted = g_choice.encoding == DlssNrColourEncoding::Encoding::Gamma22;
    }
    else
        status.line = "Waiting for a frame.";
    return status;
}
} // namespace DlssNr
