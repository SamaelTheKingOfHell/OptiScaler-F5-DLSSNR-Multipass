#pragma once

// The LUT-apply pass's last frame (dlssnr-lut-apply epic, Story 4), for the menu's status line. Every
// backend that runs DispatchLut (currently D3D12 only; Vulkan joins in Story 3) reports here once per
// frame, whether a LUT was wanted, loaded, or failed; the menu reads it back. Mirrors
// DlssNr_ColourEncodingStatus.h's split (report from the render thread, read from the menu).

#include "DlssNr_LutSpaces.h"

#include <string>

namespace DlssNr
{
// Render thread. Cheap when nothing changed: one lock and a comparison, no allocation.
void ReportLutStatus(bool wanted, const std::string& loadedPath, int size, bool failed, const std::string& error,
                     const std::string& attemptedPath, float strength);

struct LutStatus
{
    bool seen = false;   // at least one frame has reported
    bool wanted = false; // LutFile was non-empty on the last reporting frame
    bool loaded = false; // a LUT is actually active (wanted and parsed successfully at some point)
    std::string loadedPath;
    int size = 0;
    bool failed = false; // attemptedPath (below) failed to parse -- independent of loaded/loadedPath,
                         // same contract as DlssNr_LutState
    std::string error;
    std::string attemptedPath;
    float strength = 1.0f;
};

// Where the F5LUT stack graded (memory/plans/2026-11-01-f5lut-image-page.md): right after NR when NR runs, else as a
// step in the OptiScaler upscaler's output chain. Never both in one frame.
enum class PostLutStage
{
    AfterNr,
    UpscalerOutput,
};

// The F5LUT stack's last frame, for its card. Same split as above.
struct PostLutStatus
{
    bool seen = false; // at least one frame has reported
    PostLutStage stage = PostLutStage::AfterNr;
    unsigned long long reportedMs = 0; // GetTickCount64 at the report: a stage that stopped running goes stale
    bool wanted = false;               // the stack held at least one file
    bool loaded = false;               // a chain is active
    std::string chain;                 // DlssNr_DescribeChain
    std::string failedPath;
    std::string error;
    std::string note;                    // why it is not applied when it is wanted (a mode that does not run it)
    LayerInput inputs[4] {};             // each layer's look input (DlssNr_LutChain.h kMaxLutChainLayers)
    LutSpace endsIn = LutSpace::Display; // the stack's output: not the picture = it ends in a log signal
};

// Render thread. A frame where the stack is empty reports that too, so the card never shows a stale chain.
void ReportPostLutStatus(const PostLutStatus& status);

// Menu thread.
LutStatus ReadLutStatus();
PostLutStatus ReadPostLutStatus();

// Whether NR's stage graded the stack within the last `ms` milliseconds (a loaded AfterNr report). The upscaler chain's
// step stands aside while it does, so a frame is graded once: NR may run after the chain in the same frame, so the
// last frames are all there is to go by.
bool PostLutGradedAfterNrWithin(unsigned long long ms);
} // namespace DlssNr
