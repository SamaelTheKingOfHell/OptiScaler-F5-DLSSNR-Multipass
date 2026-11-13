#pragma once

// The Colour encoding override's last frame, for the menu and the log. Every path that runs the NR pass (D3D12
// evaluate, deferred SR, Finished Picture, Vulkan) reports its choice here once per frame; the menu reads it back.
// The choice itself is decided by shaders/dlssnr/DlssNr_ColourEncoding.h.

#include <shaders/dlssnr/DlssNr_ColourEncoding.h>
#include <shaders/dlssnr/DlssNr_ExposureState.h>

#include <string>

namespace DlssNr
{
// Render thread. Remembers the choice and, when it changed and a forced choice does not fit the buffer, logs the
// warning once. path names the caller in that line ("", "Vulkan", "finished picture"). Cheap when nothing changed:
// one lock and a comparison, no allocation. finishedPicture: the choice is Finished Picture's (the screen's), which
// decides whether the NR Exposure controls act on the frame (DlssNr_ExposureState.h).
void ReportColourEncoding(const DlssNrColourEncoding::Choice& choice, const char* formatName, const char* path,
                          bool finishedPicture);

struct ColourEncodingStatus
{
    bool seen = false;
    std::string line;    // DlssNrColourEncoding::Describe
    std::string warning; // DlssNrColourEncoding::MismatchWarning, empty when the choice fits
    // Whether the NR Exposure controls act on the last frame; LinearHdr until a frame is seen.
    DlssNrExposureState::State exposureState = DlssNrExposureState::State::LinearHdr;
    // The choice is gamma 2.2: the pass converts the picture at both edges (an exact round trip in intent, but not the
    // frame's own values). False until a frame is seen.
    bool gammaConverted = false;
};

// Menu thread.
ColourEncodingStatus ReadColourEncodingStatus();
} // namespace DlssNr
