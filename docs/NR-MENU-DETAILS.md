# Neural Rendering menu: the full explanations

The menu's tooltips are kept short: what a control does, and which way to move a slider. This page keeps the
longer explanations they used to carry: how a control works, its limits, and why its default is what it is.

## Automatic exposure: tuned points

Each point is the Model input brightness Tune found for a scene this bright. Automatic uses the nearest points, blended smoothly between two, so tune once in a bright scene and once in a dark one.

Tune again in the same brightness (within 2%) to replace a point. Eight points at most.

With no points the Model input brightness slider applies again.

A point belongs to the meter it was tuned with: after switching Meter, tune again.

## Tune for this scene

Finds the Model input brightness above that gives NR the most detail on the scene on screen.

Tries the slider across its useful range twice over, about 12 frames a step, and checks each step for detail, flicker and clipping, offering a change only when both sweeps agree. Hold the camera still while it runs: the picture gets brighter and darker on purpose. Nothing changes until you press Apply or Save as point. The result is an offset on the exposure, so it keeps following the scene afterwards. With more than one model pass, it runs and measures the first pass only: that is the one that sees the game's picture, so the result holds for any number of passes. With Follow the game's exposure on, it tunes against Automatic's own exposure and learns Follow again during the run, so the result holds once Follow takes over.

With NR before Super Resolution, the run itself happens after SR (the picture changes for a moment) and NR goes back before SR when it ends; the setting is not changed.

## Measure detail

Measures NR's output on the scene on screen at the current settings, for about a second, against the game's own frame:

Detail - how much fine detail NR adds (50% more = half as much again as the game had).

Flicker - how much the picture changes from frame to frame, as a multiple of the game's own change (1x = NR adds no flicker; on a paused, perfectly still frame it is shown as a small number).

Colour - whether NR makes the picture more or less saturated, warmer or cooler.

Shadows - whether NR lifts or darkens the darkest parts, and how much it crushes to black.

To compare settings: on a still scene (a paused replay, photo mode) press it, change ONE setting, press it again. The line below says what changed; differences of a few percent are noise (press it twice without changing anything to see how much). The raw numbers are greyed out and go to OptiScaler.log.

## Optical F5Low: mode

For a game that makes no upscaler call of its own. Optical F5Low works out the motion of the finished picture itself, so NR can still run.

NR only: NR runs on the finished picture. Costs less, no frame generation. Sets NR Pass at: to Finished Picture.

NR + frame generation: also starts OptiScaler's upscaler (your choice in the menu, FSR when none), which gives frame generation its input. Frame generation needs this, and it unlocks the upscaler settings. The upscaler does not process the picture unless you tick **Run the upscaler on the picture**: at the same size it adds no detail, but it can steady shimmer in games with weak anti-aliasing (about 1.5-3 ms more), with NR on or off. Moves NR Pass at: off Finished Picture.

Both use the game's depth when Optical F5Low finds it (Advanced). Depth is optional: without it NR uses motion only. Choosing a mode turns depth on (Off turns it off); if depth was not running when the game started, restart the game to use it.

NR runs only with Enable Neural Rendering on. Changes apply at once.

Not for a game that calls an upscaler of its own: while it does, only Off can be chosen, and a mode already on stands aside.

A Vulkan game that calls DLSS, FSR or XeSS of its own is not an Optical F5Low mode: choose the Vulkan route "Bridge, game's upscaler" on the Frame Generation page instead. Frame generation runs from that call, with the game's own motion and depth; no motion is estimated. The game's upscaler runs through a D3D12 copy of it (DLSS on D3D12 where DLSS can run, FSR otherwise), chosen for you; switching back to "Native swapchain" puts your upscaler back. Choosing NR only or NR + frame generation on this page turns the route off; Off leaves it on.

D3D11, D3D12 and Vulkan games. On Vulkan, frame generation needs the Frame Generation input (OptiFG, Upscaler) and an output set when the game starts; the mode itself can be switched on later.

## Reuse detail between frames

Runs the model every other frame. In between, the last result's detail is moved onto the new frame with the motion vectors, and dropped where depth or colour disagree.

Roughly halves NR's GPU cost at any pass count. Detail can pop where objects move and reveal new areas.

Best with one pass. What a reused frame can get wrong is the part of the picture with no detail to move times how much the model changes the picture, and passes build on each other, so with two or three passes the same dropped areas flicker visibly in fast motion (The Witcher 3; not seen there with frame generation on). Pause while moving fast, under Debug, is what limits it.

D3D12 and Vulkan with NR after SR, and with Optical F5Low (D3D11, D3D12 and Vulkan games), which also tells it where the last frame cannot be trusted. Not with NR before SR, nor in Finished Picture without Optical F5Low. Reuse bottleneck is off while this runs.

It keeps running while frame generation is on (Debug > Keep on with frame generation). Full and reused frames cost differently, so the game's frame times alternate: a limiter just below the average rate evens them out.

## Reset NR on scene cuts the game does not flag

Finds hard cuts in the picture (a camera cut, a new scene) on the frame they happen, by comparing how bright each of nine areas of the picture is with the last frame. Fades, exposure changes and fast pans are not cuts.

Most games tell NR about their cuts. In one that does not, the old scene fades out of NR's picture over several frames. With this on, NR starts over a few frames after such a cut, and Reuse detail between frames drops the moved detail on the cut frame itself.

Off, the cuts are only counted below, which shows whether this game needs it. D3D12 games; not with Optical F5Low, which finds its own cuts. SceneCut=0 in OptiScaler.ini stops the counting too.

## Low latency (Framerate page)

A game running Optical F5Low makes no call to lower its latency, so OptiScaler makes them for it: NVIDIA Reflex on NVIDIA cards, and on other cards Anti-Lag 2, XeLL or LatencyFlex through fakenvapi. It keeps the GPU's queue short, which matters most when the GPU is the limit, as it is with NR and frame generation on. The Optical F5Low page shows the state and links to the Framerate page, where it is set.

Input is who makes the calls. Auto: the game's own Reflex when it makes the calls, otherwise OptiScaler's while an Optical F5Low mode, XeFG or FSR FG runs. Game: only the game's own Reflex. OptiScaler: OptiScaler's, in any game without Reflex. Off: no low latency at all; the game's own Reflex is sent with low latency off, no Boost and no frame cap of its own, and OptiScaler's limiter keeps the cap (not with DLSS FG, which needs Reflex).

Output is what lowers latency: Auto uses what the setup has; XeLL (restart) and LatencyFlex force that method through fakenvapi. With XeFG the choices are XeLL (its own), or NVIDIA Reflex or Anti-Lag 2 beside it when the setup has them; XeLL stays in place for XeFG either way. Boost keeps the GPU's clocks up for NVIDIA Reflex.

OptiScaler stands aside as soon as the game calls Reflex itself. Takes effect at once, except Force XeLL.

## Multipass presets

Set this fork's recommended starting point for the chosen pass count: FP8 precision and game-exposure white point. Upscale Method, Upscale Mode and Final Image Composition are left alone; use the Pre-SR or Post-SR presets for those. 2 Pass also sets Pass 2's overrides; 3 Pass sets Pass 2 and Pass 3's overrides. Overwrites those settings on the other Neural Rendering pages; anything not listed here, including NR Pass at:, is left as you have it.

The green button is the pass count currently in effect; changing the pass count or a pass's Style, Intensity, Local structure, Local tone or Skin structure clears it.

## Pass damping

How much of an extra pass's raw answer the next pass actually receives.

1.0 is what every configuration has always done: the next pass gets the full answer. Every pass after the first is already being shown something the model was never trained on -- its own previous output instead of a raw frame -- so lower values hold each pass closer to that training distribution instead of drifting further from it with every extra pass, at the cost of a smaller cumulative edit.

No effect at Passes = 1: there is no boundary to damp.

## Quality tiers (before / after Super Resolution)

Quality tiers for running NR before Super Resolution at a lower model resolution, from High (100%, best quality) down to Potato (50%, cheapest).

Each sets Upscale Mode, Upscale Method, Model resolution and Final Image Composition (High also sets Detail and Colour strength to 1; Medium, Low and Potato also set Restore Sharpness), and turns Auto model resolution off so the resolution applies.

It also sets NR Pass at: to Before Super Resolution. Anything not listed here is left as you have it.

The green button is the tier currently in effect; changing NR Pass at:, Upscale Mode, Upscale Method, Model resolution or Final Image Composition clears it.

A tier leaves Upscale Mode on Superior Scaling if you selected it, and still counts as in effect then.

## NR Profiles

Saves this game's Neural Rendering settings under a name and loads them back. The first card of Status & Presets,
under Status.

A profile is made to be shared for one game. It holds how NR looks and what it costs (everything the preset rows set,
the passes and their overrides, colour, output, input style, detail reuse, the LUT) and the game's own settings: the
exposure source and its trims and calibration points, Colour Encoding, NR Only (Finished Picture), the Optical F5Low
tuning (depth size and offsets, motion profile) and Compress screen edges. It leaves out what is yours or only for this
session: Enable Neural Rendering, the toggle key, the display's paper white, the Optical F5Low Mode (Off, NR only, NR +
frame generation) and the game's-upscaler route, Compare and the debug views, and the diagnostic switches. Loading a
profile never turns the upscaler or frame generation off.

- Files are `OptiScaler\NRProfiles\<name>.ini`. To share one, send the file; the other player puts it in the same
  folder of their install and presses Rescan.
- Load sets every setting the profile holds. One the file does not have goes to its default, so a profile gives the
  same picture whatever was set before.
- A profile records the game it was saved in. Picking one saved in another game shows that in amber: its exposure
  settings are for that game. Loading it keeps this game's Optical F5Low tuning.
- The LUT is saved by its file name only and loaded only from this game's `OptiScaler\LUTs`. If the file is not there,
  the LUT is turned off and the menu says so.
- Some Optical F5Low settings take effect after a restart; after a load that changed them the menu links to the Optical
  F5Low page, which says which.
- Load is green when the profile's settings are the ones in effect; changing one of them clears it.
- Overwrite (the picked profile) and Delete ask first. Names can use letters, digits, spaces, `-`, `_` and `.`; a
  file whose name has other characters, that is not an NR profile, or that a newer build made, is not listed.
- A profile is a separate file: Save Settings at the bottom of the menu still saves this game's OptiScaler.ini.

Revert NR to saved shows when the NR settings a profile holds have changed since the game started or the last Save
Settings, and puts them back. The settings a profile leaves out are not touched.

## Follow the game's exposure

For games that hand over their frame before applying their own exposure: on by default for those known to (RDR2), off for every other game. Automatic learns how its own metering relates to the game's exposure in the first seconds of play, then follows the game's exposure, so brightness moves exactly with the game: cutscenes, menus, fades. In a game whose own exposure never moves, if the two stay more than 0.75 EV apart for a moment, the calibration eases toward Automatic (0.25 EV a second at most).

The brightness slider keeps its meaning. Leave it off for games that expose their frame themselves (most games): it would apply their exposure twice. On Vulkan it follows a few frames behind the game.

## Re-learn

Learns the calibration against the game's exposure again, for example when it was learned during a cutscene or a loading screen. Plain Automatic is used meanwhile (about 2 s).

Re-learn in an ordinary daylight scene, not snow, night or indoors. Afterwards the calibration eases toward Automatic by itself whenever the two stay more than 0.75 EV apart.

## Meter

How the scene's brightness is read.

Average: the plain average of the frame with the brightest areas counted less (Ignore bright highlights).

Percentile (log): the log-average of the picture between two brightness percentiles, as Unreal and Unity meter. A lamp or the sky cannot pull it up, and the darkest and brightest tails are left out. It reads about differently from Average, so re-check Model input brightness after switching.

## Eye adaptation

How quickly Automatic follows the scene getting brighter, like an eye adapting.

Stops a camera zoom or a brief shot of a bright floor from pumping the brightness and tone of the picture. A cut the game announces is followed at once.

About two thirds of a change is followed after this long. Off follows every frame at once.

Not used while following the game's exposure: the game's own adapts.

## NR Pass at

Choose where in the pipeline NR runs.

After Super Resolution (default): apply NR once SR has upscaled the frame.

Before Super Resolution: apply NR to the smaller pre-upscale image instead. No effect when the game's Ray Reconstruction is active -- RR always runs NR after RR+SR, and unsupported input layouts fall back to after SR.

Finished Picture: apply NR after the game has finished its lighting and effects, which may help with green noise. Works with frame generation on or off in native DirectX 12 games (SDR, HDR10, scRGB), and can also change the HUD and menus.

## Reuse bottleneck

Recomputes the model's coarsest stage (its 32x18 bottleneck) only every other frame and reuses the last result in between, which saves roughly a tenth of the model's GPU time.

That stage changes slowly, so the picture usually barely differs, but fast camera motion can look slightly softer. Scene cuts always recompute. With several passes, all passes compute on the same frame and all reuse on the next.

On by default. Applies immediately, NVIDIA's own model only.

Used when the model runs NVIDIA's FP8 kernels (NVIDIA's DLL and FP8-based builds).

## Depth tolerance

How far this pixel's surface may lie outside the depth the saved detail came from, as a share of the nearer of the two, and still be trusted in full; trust is gone at twice it.

Raise it where detail is dropped on a surface the depth guide does not describe well, such as water or glass.

Fill uses three times this as the depth window it borrows detail over, so this widens that too.

Default 0.051, raised from 0.020 because distant water blinked between the model's picture and the game's own. RDR2 defaults to 0.267 instead: its own distant water needed far more than other games measured so far.

## Keep on with frame generation

Keeps reusing detail while frame generation is on. On by default: generated frames are built from pairs of full and reused frames, which used to flicker at the edges of the screen in fast motion, and Pause while moving fast is what stopped that.

Turn it off to have reuse stand aside whenever frame generation is running.

## Minimum frame rate

Reuse runs only while the rendered frame rate (before frame generation) is at least this; below it every frame runs the model. At low frame rates things move farther between frames and the moved detail trails around moving bodies.

Comes back 15% above the minimum. 0 = no minimum. Default 25.

## Pause while moving fast

Detail can only be moved to where the picture already was: what comes in from off-screen, and what a moving body uncovers, has none, and Fill reaches only a few dozen pixels into it.

Running or turning fast brings in more than that every frame, so the edges of the screen flicker between the model's picture and the game's own. Above this share of the picture, every frame runs the model, until the share has stayed at or under it for a third of a second.

The frames it gives up are the ones reuse looked wrong on. The share is measured on the GPU, so the pause starts two or three frames into a fast turn (more on Vulkan): the first frames of it still flicker. 0 = never paused. Default 10%.

## LUT

A 3D LUT (.cube file -- Adobe/DaVinci/ReShade format) graded onto the NR input image before the model ever sees it. Drop files into OptiScaler\LUTs (the OptiScaler folder beside the game's exe) and press Rescan to list them here, or set LutFile in the ini to any path directly.

This grades the image NR works from, not the final picture -- a strong or unusual grade can affect auto-exposure, skin-tone masking and detail reuse the same way an unusual game colour grade would.

## Compress screen edges

NR works on a smaller picture: the middle of the screen stays at full sharpness and the edges are squeezed into the rest of the space, then NR's result is stretched back. The edges lose a little sharpness and can shimmer a little in motion; the middle is untouched. NR costs less because it has fewer pixels to work on.

Model resolution still scales the whole picture first. Reuse detail between frames does not run while this is applied. Not used with the driver-proxy backend. Applies at once.

## Compress screen edges: Light / Balanced / Strong

How much of the picture stays sharp, and how much of it NR works on. The first number is the sharp middle's share of the width and of the height, the second is the share NR works on; the rest is the squeezed edge. At full Model resolution Light works on about 90% of the pixels, Balanced about 81% and Strong about 64%. How much GPU time that saves has not been measured for every layout yet.

Picking one puts every advanced value back to that layout's own. Custom is what the advanced values below add up to; it cannot be picked.

## Model resolution

NR resolution relative to the image it processes. 50% halves width and height; 100% uses the full size.

Lower values reduce cost and fine detail. Above 100% increases cost. Game output resolution is unchanged.

The model averages its input 2x2 before its main network runs, so that network always works at half of this size: cost follows the halved size, and so does the finest detail it can add.

## Auto model resolution (post-SR only)

When NR runs after SR -- Apply before SR off, or Ray Reconstruction, which always runs it after -- derive the working scale from the render:output ratio the upscaler itself already reconstructed detail at, instead of the slider above.

That output already reconstructed detail at that ratio, so NR running at the same reduced scale costs nothing extra to tune for. No effect while NR runs before SR -- the slider applies as usual.

## Upscale Mode: Superior Scaling

An Upscale Mode of its own (NR Output). Below 100% Model resolution it shrinks the picture with an Area filter, shapes NR's change by frequency between passes and at the end, and runs a private NVIDIA DLSS (DLAA) on that change to keep it steady over time. Once selected it also works at 100% while Compress screen edges shrinks the model's input. **DLSS to temporally stabilize NR** picks that private DLSS's preset: A (faster) or K (steadier in motion); if the runtime does not offer it, its default preset is used.

It needs DirectX 12 (or a DirectX 12 bridge), the game's depth and motion, and NVIDIA DLSS through NGX. Where any of these is missing (Vulkan, a non-NVIDIA card, the vendor-neutral port) Matched residual runs instead and the menu says why; select Superior Scaling again to retry. Upscale Method has no effect with it. Details: [Superior-Scaling-port.md](Superior-Scaling-port.md).

## Final Image Composition

Choose how HDR brightness is mapped for NR.

Soft knee compresses highlights. Reversible curve uses a reversible mapping. Balanced preserves midtones and compresses highlights.

HLG and PQ are the broadcast HDR curves: white sits at 75% (HLG) or 58% (PQ), leaving more room for highlights (HLG up to about 4x white, PQ about 50x), but the model sees midtones differently.

Linear: gives NR the picture exactly the way games with built-in Neural Rendering do, for NVIDIA's own look. Use it with White point source = Game exposure and Starting point = Native look. Very bright coloured lights can shift a little in colour, and some games may show banding in dark areas.

If you used Tune, run it again after changing the curve.

Composed uses the strength control and the Highlight guard below (brightening only; darkening is not capped in Composed). Replace bypasses the strength control (the model's answer applies directly, uncomposited) but the same Highlight guard number still bounds it in both directions -- lower it if Replace flickers or shows banding near bright highlights.

## MFG unlockers

OptiScaler has no built-in MFG unlock since 0.1.32. To use an external unlocker, tick **External frame generation /
MFG unlocker**, Save Settings and restart; see [RTX40-MFG.md](RTX40-MFG.md).

## Optical F5Low: frames unchanged before it counts as still

Where the picture has not changed at all for this many frames (a HUD panel), the camera motion above no longer wins a tie with no motion, so the HUD stays still while the picture pans behind it. Near a line or an edge a panning wall changes, so it keeps moving with the camera; a plain patch far from any is held still too. 0 switches the rule off.
