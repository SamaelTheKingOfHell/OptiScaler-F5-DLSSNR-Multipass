# Credits

This fork was itself forked from [wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass), which builds on [Dagherbou/OptiScaler_DLSSNR](https://github.com/Dagherbou/OptiScaler_DLSSNR) and [OptiScaler](https://github.com/optiscaler/OptiScaler). OptiScaler began with [PotatoOfDoom's CyberFSR2](https://github.com/PotatoOfDoom/CyberFSR2).

Colour processing is derived from [RenoDX by clshortfuse](https://github.com/clshortfuse/renodx). See the [RenoDX attribution and licence](../Licenses/RenoDX_ATTRIBUTION.txt) for details.

## DLSS-NR exposure controls

Automatic exposure from the HDR frame, the wider exposure Trim range and the Trim Anchor points are @mattjaas's work, from [wilsjo2's PR #77](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/pull/77), adapted onto this fork's code.

## Superior Scaling

The Superior Scaling Enlargement mode (custom Area downsample, private DLSS stabilisation of the NR residual, guided
residual reconstruction and its calibrated frequency gains) is @mattjaas's work, from
[PR #230](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass/pull/230), integrated into this fork's pipeline
and reviewed here. Their commits are kept in this fork's history.

## Depth buffer detection

The rules the native depth finder uses to pick the scene's depth buffer (how draws are weighed, the aspect-ratio test, which frames are ignored, the snapshot taken before a clear) are adapted from the [Generic Depth add-on in ReShade](https://github.com/crosire/reshade/blob/main/examples/09-depth/generic_depth_addon.cpp) by Patrick Mours (BSD-3-Clause). See the [licence](../Licenses/ReShade_GenericDepth_LICENSE.txt).

## Motion and trust mask

The optical flow for the native input producer is our own (a luma pyramid, block matching and a few Lucas-Kanade steps), written from the standard method; the public descriptions of the ReShade optical-flow shaders were read for ideas and none of their code is used. The flow upgrade (trying the last frame's flow and the neighbouring coarse cells as candidates, a deeper pyramid, and smoothing that follows the picture's edges) took its ideas from the public description of [LumeniteFX](https://github.com/umar-afzaal/LumeniteFX)'s LumaFlow (its licence reserves all rights, so none of its code or constants are used; its own notice names AMD FidelityFX Optical Flow, Zenteon's motion vectors and Video++ as its sources). The trust mask follows the checks of the validation in [DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder)'s DLSS5_Feed.fx (MIT, Copyright (c) 2026 Jean-Laurent ROUZIES): reproject with the motion, and distrust a pixel where the depth, the motion or the luma there disagrees with here. See the [licence](../Licenses/DLSS5Feeder_LICENSE.txt). The same-frame scene-cut detector and the perceptual luma of the native flow took their ideas from the source of [AMD FidelityFX Optical Flow](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) (MIT, Copyright (C) 2024 Advanced Micro Devices, Inc.): brightness histograms in a 3x3 grid of tiles, smoothed and compared with the last frame's by a symmetric KL divergence, tried over sideways shifts so a brightness change is not a cut; a luma that follows the CIE lightness curve for HDR input (after dividing by a white point), with gamma-encoded SDR used as it is; and no motion winning a near tie on the finest level (kept behind a setting, off). They are written here from those ideas (the detector over a logarithmic luma and a wider range of shifts), and none of AMD's code is used.

## Shutdown safety

Process-exit hardening (a heap-leaked state singleton, an atomic shutdown flag, an `ExitProcess`-aware
`DllMain`, and safe feature teardown during shutdown) is adapted from [wilsjo2's fork](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass), commit [`dac290ed`](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/commit/dac290ed17ed88962a7f9ee63a41b7500a0fc923).

## Built-in MFG unlock

The provider-recognition helper in `OptiScaler/framegen/mfgunlock/Provider.h` is adapted from [KleberMotta's fork](https://github.com/KleberMotta/OptiScaler-DLSS5-MFG-RTX40) (MIT), a port of the MFG Unlock ReShade addon by [Dreamt](https://github.com/ImDreamt/MFGAdaUnlock-RenoDx) and [mavismmg](https://github.com/mavismmg/MFGAdaUnlock-RenoDx). The structural locators use MinHook's Hacker Disassembler Engine 64 to verify x64 branch and string-reference anchors. License notices for both are reproduced in [MFGUnlock_LICENSE.txt](../Licenses/MFGUnlock_LICENSE.txt). The PE image, transaction and checker framework is original to this fork. Design ideas from [dashdogy's MFG-Unlocker](https://github.com/dashdogy/MFG-Unlocker) are independently implemented; no source code was copied.

## KCD2 HDR compatibility

The binary quirk patch that keeps Kingdom Come: Deliverance II's native HDR10 output working with frame generation active is adapted from [wilsjo2's fork](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass), commit [`34dfe6d9`](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/commit/34dfe6d9757837d79a8931b04af0e3955cbe29a7) ("Keep KCD2 HDR output compatible with native DLSSG").

## XeFG Dynamic MFG

Dynamic MFG for the XeFG output (the multiplier picked every frame to follow a target frame rate, and "Use as FPS limit") is ported from [upstream OptiScaler](https://github.com/optiscaler/OptiScaler) (GPL-3.0), by Michael Robles (@dashdogy), commits `b7536f77` and `071bb0a6`, with the menu from FakeMichau's `b35346e9`. Its count controllers are taken as they are; the hooks are adapted to this fork's frame cap (one cap value fed to every limiter path, PlanCap keeping who holds it).

## Streamline dual-runtime isolation

The fix keeping the game's own Streamline plugins (Common, Reflex, PCL) separate from OptiScaler's
private DLSS Frame Generation runtime when NVIDIA's override selection would otherwise point both
at the same already-loaded module is adapted from [wilsjo2's fork](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass), commit [`ae9a50fa`](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/commit/ae9a50fa12b1e81600d0394108d523dad8b7fd23) ("Isolate game Streamline plugins from active DLSSG output").

## Upstream nightly fixes (late October 2026)

Four fixes are ported from [upstream OptiScaler](https://github.com/optiscaler/OptiScaler) (GPL-3.0) and adapted to this fork:
the FSR 4 INT8 list for RDNA1 (FakeMichau, `ab379b25`); accepting DXGI success statuses such as
`DXGI_STATUS_OCCLUDED` when swapchains are created (felipegakkeku, `6a7b0336`); offering the driver's FSR 4 frame
generation on Vulkan only with real FP8 (FakeMichau, the Vulkan half of `65ee5d64` and `b19cfd29`); and keeping a
swapchain on another window out of frame generation (cdozdil, release/0.9 `192a59bb` and `0fc1ef63`, the other-window
part only). The rules behind them, the per-chain wrapper handling and the host tests are this fork's own.

## Inter font

The menu's Modern style uses [Inter](https://github.com/rsms/inter) by Rasmus Andersson and the Inter Project
Authors (v4.1, Regular), embedded in the DLL under the SIL Open Font License 1.1. See the
[licence](../Licenses/Inter_OFL.txt).

## LUT looks

The files in `OptiScaler\LUTs` are baked for the DLSS-NR LUT feature from the [PixelManager OCIO config by Joe Genco](https://github.com/Joegenco/PixelManager), which bundles these projects. Each file names its source in its header.

- `AgX-Blender*.cube`: [Eary Chow's AgX](https://github.com/EaryChow/AgX), after [Troy James Sobotka's AgX](https://github.com/sobotka/AgX).
- `AgX-Kraken*.cube`: [Troy James Sobotka's AgX](https://github.com/sobotka/AgX).
- `AgX-Resolve*.cube`: [Troy James Sobotka's AgX Resolve](https://github.com/sobotka/AgX-Resolve).
- `JP2499DRT*.cube`: [Juan Pablo Zambrano's DRT DCTLs](https://github.com/JuanPabloZambrano/DCTL).
- `identity_33.cube`, `shift_33.cube`: made for this project.

None of these repositories states a licence, so they are included with credit only, as PixelManager itself does. If an author would rather they were not distributed, open an issue and the file will be removed. The GPL-3.0 and AGPL-3.0 projects PixelManager also bundles (OpenDRT and JzDT, flim) are deliberately not included.

## Highlight guard: brightening only

Bounding the Composed path's Highlight guard to brightening only, leaving darkening uncapped, is
@mattjaas's work, from [wilsjo2's PR #94](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/pull/94),
adapted onto this fork's code. This fork's Replace-mode guard (`ApplyReplaceGuard`) is unrelated to
that PR and keeps its own, still-symmetric bound for a different reason.

## Motion and Detail Reuse ideas

Several ideas in the optical flow and in Detail Reuse come from reading [AeonSR](https://github.com/BarbatosAWLS/AeonSR) (MIT, Barbatos AWLS, commit `85a26f8`): a brightness-proof matching cost, matching only within a pixel's own surface, a whole-picture motion candidate in the flow, rejecting moved detail where the motion at its source disagrees, and rounding away sub-step differences when steadying reused frames. They are implemented from the ideas in this fork's own code; no AeonSR code was copied.

## Edit cache (experimental, set in the ini only)

The edit cache (`EditCache` in the ini: the model one frame in N, its change to the picture carried along the game's motion in between, checked against a fingerprint of the frame and its surroundings, dropped pixels filled from their own surface, and a crossfade when the model runs again) follows the design notes of [Skynizz's optiscaler-dlss5](https://github.com/Skynizz/optiscaler-dlss5) (GPL-3.0), edit-cache.md. It is written here from those notes, and none of its code is used. The vector chain read once from the model's frame, the crossfade that follows the interval and the check of the crossfade against the picture are this fork's own, measured offline.

## Compress screen edges

The layout and mapping behind Compress screen edges (the sharp middle, the squeezed edges, and the rational curve that packs and unpacks them) are adapted from [Yuri Grib / @BeliyG3's Optimizer FPS for DLSS5](https://github.com/BeliyG3/optimizer-fps-dlss5/tree/64902dd6a02460e5f6b778504ec2a4005faf4d9c) (MIT, Copyright (c) 2026 Yuri Grib), pinned at commit `64902dd6`. The way to apply it to NR (pack the colour, depth and motion, run the model on the packed picture, unpack the model's input and answer through the same resampling) follows [wilsjo2's fork](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass), v0.8.9 (OptiScaler-DLSSNR-PreSR-Multipass, commit [`be229250`](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/commit/be2292501f)), adapted onto this fork's own NR code. See the [licence](../Licenses/PeripheralWarp_LICENSE.txt).

## OptiScaler contributors

These credits are retained from the original OptiScaler README:

- @PotatoOfDoom for CyberFSR2.
- @Artur for DLSS Enabler and help with the NVNGX API.
- @LukeFZ and @Nukem for their mods and shared knowledge.
- @FakeMichau for support, testing and features.
- @QM for testing and access to games.
- @TheRazerMD for testing and support.
- @Cryio, @krispy, @krisshietala, @Lordubuntu, @scz and @Veeqo for the earlier compatibility matrix.
- The DLSS2FSR community for its support.

This project uses [FreeType](https://gitlab.freedesktop.org/freetype/freetype), licensed under the [FTL](https://gitlab.freedesktop.org/freetype/freetype/-/blob/master/docs/FTL.TXT). Other notices are in [Licenses](../Licenses).

## Upstream sponsorship

The original OptiScaler project credits [SignPath.io](https://signpath.io/) for Windows code signing and the [SignPath Foundation](https://signpath.org/) for its certificate.

To support the original developers: [cdozdil on GitHub Sponsors](https://github.com/sponsors/cdozdil?frequency=one-time) and [nitec on Buy Me a Coffee](https://buymeacoffee.com/nitec).
