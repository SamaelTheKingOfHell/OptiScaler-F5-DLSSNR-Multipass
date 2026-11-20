# Plan: a Preset A runtime tuned for RTX 30 (Ampere, `sm_86`)

Status: **found by measurement (2026-11-14)**. The published Ampere v0.4.0 runtime, documented for Preset B only, also runs Preset A about twice as fast as the compatibility runtime. Its Preset A picture is not validated.

## What exists today

| Runtime | Version | Presets | On an RTX 3060 |
|---|---|---|---|
| Compatibility runtime (`cdfa6c1e…`, 276 MB) | 310.9.16.0 | A and B | Preset A about 26.7 ms, Preset B about 15.0 ms of neural time at 1280x720 (docs/Multi-GPU-runtimes.md) |
| [DLSS5-RTX-3080-laptop](https://github.com/lmxxf/DLSS5-RTX-3080-laptop) v0.4.0 (`749daadb…`, 266 MB) | 310.9.16.0 | tuned and validated for **Preset B only** | Preset B about 7.6 ms at 1280x720, about half the compatibility runtime |
| NVIDIA 310.8 (`e16bcf15…`) | 310.8.0.0 | one built-in model | not an Ampere build |

The Ampere project states that it keeps only the small model for RTX 30, and its 38 tuned kernels, benchmarks and
bit checks all cover Preset 2. Its patcher (`Development/ptx-patcher/build_sm86_dlls.py`, MIT) is open and rebuilds
all 15 kernel containers of the original 310.9.16.0 DLL, so the main model's kernels are in reach of the same method.

## Why Ampere is slow at Preset A, and what "using its tensor cores" means

- The model's kernels are written for FP8 tensor-core instructions (`mma … e4m3`) that only RTX 40/50 have. Ampere
  has FP16 (`m16n8k16`) and INT8 tensor-core instructions, not FP8.
- The compatibility runtime converts FP8 to FP16 with long integer sequences; the kernels grow so much that several
  containers only fit at a low compiler optimisation level.
- The Ampere project replaces those sequences with two exact FP16 identities (4 instructions each), which lets every
  container compile at full optimisation, then removes shuffles and register spills around the FP16 tensor-core
  multiply. That is the best use of Ampere's tensor cores that keeps the model's numbers; an INT8 path would change
  the picture and is out of scope.

## Measured 2026-11-13 and 2026-11-14 (steps 1 and 2 done)

RTX 3060, model input 1280x720, one pass. "Trace" is `tests/nr_gpu/nr_cuda_trace.cpp` (8 evaluates, the last one
timed, the 3060 alone). "Handoff" is `tests/run_nr_cross_process_neural.ps1` (30 frames, RTX 5070 Ti as game GPU,
median GPU time of the neural pass, two runs each).

| Runtime on the 3060 | Preset A, trace | Preset A, handoff | Preset B, trace |
|---|---|---|---|
| NVIDIA 310.8 (`e16bcf15…`) | model creation fails | model creation fails | not applicable |
| Compatibility 310.9 (`cdfa6c1e…`) | 27.7 ms | 31.2 and 43.5 ms | 16.0 ms |
| Ampere v0.4.0 (`749daadb…`) | **13.7 ms** | 13.6 and 28.0 ms | 8.2 ms |

**The Ampere v0.4.0 runtime is already about twice as fast at Preset A.** Both runtimes launch the same 156 kernels
(43 distinct) for Preset A, and all but two small copy kernels are the FP8 kernels the Ampere project rewrote, so
its tuning reaches the main model too. By kernel family, Preset A, trace:

| Family | Ampere v0.4.0 | Compatibility | Faster by |
|---|---|---|---|
| `cc_tinlayout_*` (windowed layers) | 8.87 ms | 18.90 ms | 2.1x |
| `cc_split_swin_*` | 2.33 ms | 5.75 ms | 2.5x |
| `cc_vit_*` | 2.38 ms | 3.00 ms | 1.3x |

What this does not establish:

- **Picture quality at Preset A.** The Ampere project validated Preset B only, and its output is not bit-identical
  to the compatibility runtime (each runtime's checksums repeat exactly between runs, and differ between runtimes).
  Compare the two runtimes in the game before trusting it.
- **Steady timing.** The handoff runs vary by a factor of two between identical runs. The 3060 sat idle at 210 MHz
  and 14 W between runs and nothing else was using it, so the slow runs are probably the card not leaving its idle
  clocks during a short test; a game keeps it loaded. Not verified.
- An earlier handoff run measured 46.6 ms for this runtime at Preset A with only 8.3 GiB of free memory; that
  figure was wrong for normal conditions.

The remaining headroom is in the `cc_vit_*` kernels (1.3x so far) and the eight-head windowed layer
(`cc_tinlayout_fused_swin_8h_256_8_chained_fp8`, 1.7 ms, the largest single item). Steps 3 and 4 below now apply
only to squeezing those, and step 5 is already possible with the v0.4.0 runtime.

## Steps

1. **Test the existing Ampere runtime with Preset A first.** It was built from a DLL that contains both models, and
   the two models may share most kernels. Run `tests/run_nr_cross_process_neural.ps1` with
   `-SecondaryModelDll …\ampere-small-v040\nvngx_dlssnr.dll` and pass 1 at Preset A, 1280x720 and the game's model
   size. Record: does it initialise, neural milliseconds, and the output checksum against the compatibility runtime
   at Preset A. If it runs, is faster, and the picture matches or is visually equal, the runtime is found and steps
   2 to 4 shrink to validation.
2. **List the kernels Preset A launches.** `tests/nr_gpu/nr_cuda_trace.cpp` (docs/NR-network-trace.md) already
   records the launch chain of a model. Trace Preset A and Preset B on the 3060, and diff the kernel names against
   the 38 the Ampere project tuned. The difference is the work list.
3. **Extend the patcher to those kernels.** From the original 310.9.16.0 DLL, apply the same transformations to the
   Preset A only entries: the two FP16 identities, the even/odd K16 operand split, direct use of values that are
   already FP16, and per-container register caps chosen by measurement. Keep every container at full optimisation
   and inside its original slot.
4. **Validate.** Bit comparison against the compatibility runtime at Preset A on fixed inputs; the project's sky
   gradient audit (its earlier upstream produced bands); timing at 1280x720, 1920x1080 and the game's model size,
   30 or more frames, paired runs.
5. **Install and select.** Place it as `OptiScaler/NR-Models/ampere-main-<version>/nvngx_dlssnr.dll`, choose it as
   the second GPU runtime on the Passes page, and set the second-GPU passes to Preset A.

## Expectations and limits

- Estimate, not a measurement: if Preset A gains what Preset B gained on this 3060 (15.0 to 7.6 ms), it lands near
  13 to 14 ms at 1280x720, against 26.7 ms today.
- The network split needs the second GPU's runtime to lay the network out exactly like the game GPU's runtime; the
  calibration checks this and refuses otherwise. A 310.9 Ampere runtime beside a 310.8 game runtime is expected to be
  refused for the split and to work for whole passes on the second GPU. Using 310.9 runtimes on both GPUs is the
  configuration to test for the split.
- NVIDIA's model and weights are not redistributable. Build the runtime locally from your own copy; publish only
  patcher changes.
- Steps 1, 2 and 4 need a local C++ toolchain to build the test fixtures; this checkout's last validation machine
  had one, the current one does not.

## Mortal Shell 2 test (2026-11-19 and 2026-11-20)

Game: 3840x2160 output, NR model 2803x1577, three Preset A passes, Superior Scaling. The RTX 5070 Ti runs all three
passes in about 26 ms (27 fps in the main menu scene).

In game, pass 3 on the RTX 3060 (Ampere v0.4.0 runtime, automatic row share) ran for about 25 s at 16 fps, then the
3060 hit a driver timeout (`LiveKernelEvent 0x141`, `Tdr`, `nvlddmkm`, Ampere) that ended the game.

Outside the game, at the same size, nothing hung:

| Test, 3060, Preset A, 2803x1577 | Result | 3060 time |
|---|---|---|
| `nr_cuda_trace`, 3060 alone, 30 evaluates | stable | 61.3 ms per pass |
| `run_nr_cross_process_neural.ps1`, whole pass, 30 frames | stable | 97.96 ms handoff median |
| same, `-Rgb10 -Frames 600` (the game's 10-bit picture) | stable | 97.29 ms median, 148.9 ms p95 |
| `nr_helper_strip`, whole / 784 / 384 / 64 rows | stable, strip rows correct | 69.5 / 40.2 / 24.6 / 19.2 ms |

The hang is not reproduced, so its cause is not known. What the numbers do establish: at this size one Preset A pass
costs the 3060 about seven times what it costs the 5070 Ti (61 against about 8.8 ms). The largest share that returns
within the game's 37 ms frame is about 384 rows, which takes about 2 ms of work off the 5070 Ti per frame, before the
cost of copying the guides and picture across PCIe 3.0 x8. The expected gain at these settings is a few percent at
best.

In game again (2026-11-20), pass 3 on the 3060 with a fixed 24% row share (rows 1161 to 1577 of 1577): 21.7 fps for the
first 30 s, then 16.5, 16.4 and 15.3 fps, against 27.0 to 27.1 fps for the 5070 Ti alone in the same scene. No driver
timeout in about two minutes. A quarter of one pass on the 3060 still costs the frame far more than it saves: the game
GPU copies the picture and both guides at full size across PCIe and then waits for the strip. At these settings the
second GPU lowers the frame rate at every share tried (whole pass and 24%).

## Network split in Mortal Shell 2 (2026-11-20)

The game's NVIDIA 310.8 model and the Ampere v0.4.0 runtime launch the same 156 kernels, but pass different tensor
pointers, so their split layout fingerprints differ (51731450db78f7f5 against 81395abfed46800b, decoded from
`nr_cuda_trace` with the split's own decoder). The compatibility 310.9 runtime on the RTX 5070 Ti has the Ampere
runtime's fingerprint, and is faster there at 2803x1577 Preset A: 8.29 ms per pass against 9.94 ms for 310.8.

With the compatibility runtime as the game's model, the Ampere runtime on the 3060 and Superior Scaling on, the split
calibrated (55 shares) and ran on pass 1. Each share it chose came back late (the game GPU waited 1 to 6 ms) and saved
at most 0.08 ms, so it stepped down to no share and kept the pass whole. FPS stayed at 26.3 to 27.1.

Why: the overlap gives the helper only the game GPU's render of the next frame (about 14 ms at 36.9 ms frames with
22 ms of NR) before the end of the split pass is due. The best share on paper, (56, 140), would save the 5070 Ti about
2.7 ms of its 8.3 ms pass, needs about 22 ms of 3060 kernels plus the transfers, and does not fit 14 ms.
Also, the 310.9 runtime lowered NR from about 26 to 22 ms per frame without changing the frame rate (27 fps), so in
that scene the frame rate was not set by NR on the 5070 Ti.

## Measured search for the split share (2026-11-20)

The split now picks its share by trying it: shares whose estimate fits about 1.4 times the helper's measured window are
tried for about 150 judged frames each, best estimated helper work first, and judged on the game GPU's timestamps as
saved time minus the time the game GPU waited. The best share with a net gain of at least 0.05 ms is kept, re-checked
every 900 frames and remembered for the next start (`<plan>.pass<N>.best` beside the cached plan); with none, the pass
stays whole and the search repeats after 90 s.

Mortal Shell 2, pass 1, 8 shares, two searches: every share made the game GPU wait about 3 ms (net -2.3 to -4.1 ms), and
the best saving was share (56, 98) at 0.62 to 0.80 ms. The helper's window measured 11.2 to 12.0 ms.

The 3060's worker timing shows why: its kernel time roughly doubles within each trial (share (23, 57): 4.7, 8.2,
9.8 ms per 60 frames), and its clocks, sampled every 0.5 s, fall from 1852 to 1920 MHz at the start of a trial to 400 to
1200 MHz (P3 and P5) within 3 to 5 s. At about 10 ms of work per 37 ms frame the driver treats the 3060 as lightly
loaded. Copies (0.4 to 3 ms each way) and the handoffs between the processes (about 1.5 ms) are small by comparison.
At boost clocks share (56, 98) would take about 10.1 ms on the 3060, close to the window.

## Full clocks and a GPU-side wait (2026-11-20)

`HelperFullClocks` writes "Power management mode: Prefer maximum performance" for `dlssnr_worker.exe` in a driver
profile of its own, from the worker's `--clock-profile on|off` mode, before a helper starts. With it the 3060 held 1717
to 1875 MHz (P0) through the search instead of falling to 400 to 1200 MHz, and the game GPU's waits fell from about 3 ms
to 0.01 to 1.3 ms for most shares. Small shares still save nothing: splitting a pass costs the game GPU fixed work in
the second half that cancels a small middle. Share (56, 98) saved 0.57 ms but came back 1.3 ms late (net -0.73 ms).

The split worker then queued its wait for the owner's data on the 3060's queue instead of waking its CPU thread first.
Share (56, 98): saved 0.61 ms, waited 0.02 ms, net +0.59 ms per frame, and the search kept it (3060 ready to done
10.9 ms against an 11.5 ms window). Its re-check 14 s later found 3.0 ms of waiting and dropped it, while the scene
and settings were changing; a kept share is now judged over at least 300 frames before it can be dropped early. The
margin is about 0.5 ms, so the gain is about 2 percent at best and depends on the scene.

## Second driver timeout on the RTX 3060 (2026-11-20 15:08)

In the user's session the model scale went from 0.73 to 0.89 to 1.00 within two seconds, with `SecondaryModel =
compat-3109`. Each size started a split calibration on the 3060; the 3418x1922 one was replaced by a 3840x2160 one
(worker ended with TerminateProcess). The 4K calibration failed at 15:08:13 and Windows logged
`LiveKernelEvent 0x141 Tdr nvlddmkm Ampere` at 15:08:16 and 15:08:32. Afterwards `nvidia-smi` reported the 3060 lost
until a reboot, and NGX would not initialise in the next game start (NR and the game's DLSS unavailable).

Which step hung the GPU is not proven. Changes: a calibration starts only after the model's size and preset have held
for 5 s, and a running calibration is never ended mid-run; the next one waits for it. Not yet run.
