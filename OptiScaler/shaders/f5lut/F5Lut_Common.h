#pragma once

// F5LUT as a step of the OptiScaler upscaler's output chain (memory/plans/2026-11-01-f5lut-image-page.md Story 3): the
// LUT stack ([DlssNr] PostLutFile1-4) graded on the upscaler's output while NR is not grading it. NR's own stage grades
// right after NR instead (DlssNr_Dx12 / DlssNrFeature_Vk), and the two never both grade a frame: this step stands aside
// while NR is on and has graded in the last quarter second. The CPU half, shared by the D3D12, D3D11 and Vulkan steps:
// the bake (DlssNr_LutChain.h, the same as NR's) and the menu's status.

#include <Config.h>
#include <dlssnr/DlssNr_LutChain.h>
#include <dlssnr/DlssNr_LutStatus.h>
#include <shaders/dlssnr/DlssNr_LutConstants.h>

#include <algorithm>
#include <string>

class F5Lut_Common
{
    unsigned long long _lastReportMs = 0;
    bool _lastReportGraded = false;

    // Whether NR's own stage is the one grading the stack: NR on and its model up on either backend (decided before NR
    // runs this frame, so a hitch or NR's own frame order cannot let both grade), or NR's stage graded in the last
    // quarter second (a D3D11 game's NR through the D3D12 bridge). NR switching on can still grade one frame twice: its
    // model comes up during that frame's NR call, after this step. F5Lut_Common.cpp.
    static bool NrGradesTheStack();

  protected:
    DlssNr_LutChainState _chain;

    // Whether the step runs this frame. CPU only, called while the chain is built, so a step that is in the chain
    // always has a lattice to grade with. False with an empty stack (the API step then lets go of its texture), while
    // NR grades, or when no layer is on or could be read (reported).
    bool PrepareChain(bool linearHdr)
    {
        const Config& cfg = *Config::Instance();
        const auto entries = DlssNr::PostLutEntries(cfg);
        const bool wanted =
            std::any_of(entries.begin(), entries.end(), [](const DlssNr::ChainEntry& e) { return !e.path.empty(); });

        if (!wanted)
        {
            if (_chain.wanted)
                DlssNr_LutChainEnsure(&_chain, {});
            return false;
        }

        if (NrGradesTheStack())
            return false;

        if (DlssNr_LutChainEnsure(&_chain, entries,
                                  linearHdr ? DlssNr::FrameDecode::LinearHdr : DlssNr::FrameDecode::Srgb))
            return true;

        Report(false);
        return false;
    }

    // The pass's constants for the baked chain. The upscaler's output is either tone-mapped (display 0-1, read as it
    // is) or, with the HDR flag, linear light divided by a white point: PreExposure / the game's exposure when it
    // hands one (the exposure that would take it to its normal brightness, as NR's encode reads it), else 1.0.
    DlssNrLutConstants ChainConstants(unsigned int width, unsigned int height, bool linearHdr, bool exposureBound,
                                      float preExposure) const
    {
        const DlssNr::Lut3D& lut = _chain.baked;
        DlssNrLutConstants constants {};
        constants.Width = width;
        constants.Height = height;
        constants.Strength = _chain.strength; // a single layer's own; a baked stack holds its strengths
        constants.DomainMinR = lut.domainMin[0];
        constants.DomainMinG = lut.domainMin[1];
        constants.DomainMinB = lut.domainMin[2];
        constants.DomainMaxR = lut.domainMax[0];
        constants.DomainMaxG = lut.domainMax[1];
        constants.DomainMaxB = lut.domainMax[2];
        constants.LutSize = (uint32_t) lut.size;
        constants.LatticeCoord = (uint32_t) lut.latticeCoord;
        constants.InputEncoding = 0u;
        constants.ColourIsLinearHdr = linearHdr ? 1u : 0u;
        constants.Trim = 1.0f;
        constants.PreExposure = preExposure > 0.0f ? preExposure : 1.0f;
        constants.UseExposureWhitePoint = linearHdr && exposureBound ? 1u : 0u;
        constants.ExposureBaseScale = 1.0f;
        constants.ExposureTrim = 1.0f;
        return constants;
    }

    // The menu's status when the step ran (or failed to): on a change, else five times a second (the menu calls a
    // report older than a second stale), so a steady frame does not copy the chain's text under the status lock.
    void Report(bool graded, const std::string& error = {})
    {
        const unsigned long long now = GetTickCount64();
        if (graded && _lastReportGraded && error.empty() && _chain.error.empty() && now - _lastReportMs < 200)
            return;
        _lastReportMs = now;
        _lastReportGraded = graded;

        DlssNr::PostLutStatus status;
        status.stage = DlssNr::PostLutStage::UpscalerOutput;
        status.wanted = _chain.wanted;
        status.loaded = graded && _chain.Loaded();
        status.chain = _chain.description;
        static_assert(std::size(decltype(status.inputs) {}) == DlssNr::kMaxLutChainLayers);
        std::copy(std::begin(_chain.inputs), std::end(_chain.inputs), status.inputs);
        status.endsIn = _chain.endsIn;
        if (!_chain.error.empty())
        {
            status.failedPath = _chain.failedLayer >= 0 ? _chain.failedPath : std::string();
            status.error = _chain.error;
        }
        else if (!error.empty())
            status.error = error;
        DlssNr::ReportPostLutStatus(status);
    }
};
