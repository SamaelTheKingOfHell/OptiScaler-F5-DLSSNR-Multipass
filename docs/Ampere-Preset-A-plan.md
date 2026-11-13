# Plan: a Preset A runtime tuned for RTX 30 (Ampere, `sm_86`)

Status: **plan only**. No tuned Preset A runtime was found (searched 2026-11-13).

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
