# Split-frame (band) multi-GPU: measurements

Status: measurement only. Split-frame scheduling has not been added to the renderer yet.

The idea: instead of moving whole passes between GPUs, divide each picture into
horizontal bands. Each GPU runs every pass on its own band, with a small overlap
at the seam. The split line follows the measured cost of each GPU. This document
records what the RTX 3060 secondary actually costs per band. That cost decides
whether a split can help.

## Tool

`tests/nr_gpu/nr_band_split_benchmark.cpp` runs one neural pass on one adapter.
It runs on synthetic, textured input at the band heights you choose and reports
median GPU timestamp milliseconds. `--subset` creates one feature at the full
height and evaluates smaller heights with it.

```text
nr_band_split_benchmark.exe <_nvngx.dll> <nvngx.dll_dlssnr.dll forwarder> <nvngx_dlssnr.dll model> <data dir>
                            <NVIDIA adapter index> <width> <full height> <preset> <frames> <heights...> [--subset]
```

Build it with `cl /std:c++17 /EHsc /O2 /DDLSSNR_WORKER_TESTING` and link `d3d12.lib dxgi.lib user32.lib`.

## RTX 3060, 2026-11-06 (30 frames, median GPU ms per pass)

| Size | Preset A, game runtime | Preset B, Ampere v0.4.0 runtime |
|---|---|---|
| 1280x720 | 27.09 | 7.44 |
| 1280x544 | 20.47 | |
| 1280x368 | 15.79 | 4.68 |
| 1280x272 | 14.49 | |
| 1280x192 | 14.35 | 4.38 |
| 1280x96 | 14.12 | 4.37 |
| 640x96 | 10.97 | |
| 320x96 | 9.28 | |
| 1920x1088 | 52.60 | |
| 1920x544 | 29.76 | |

Subset evaluation (feature built at 1280x720, Preset A): 720 rows 27.12 ms,
368 rows 15.64 ms and 192 rows 14.21 ms. Every evaluate returned success.

## What this means

- **Most of the 3060's cost is fixed per pass.** With Preset A, a 320x96 band
  still costs 9.3 ms, and any 1280-wide band costs at least about 14 ms. Above
  that, the cost grows by about 22 ms per megapixel. A smaller band cannot go
  below the fixed cost. With three Preset A passes, the 3060 needs at least
  about 42 ms per picture for any band, however small. Unless the game GPU
  needs longer than that for the whole picture, giving the 3060 any Preset A
  work lowers the frame rate. This explains why every pass placement measured
  so far was slower.
- **The Ampere Preset B runtime lowers the fixed cost to about 4.4 ms.** Three
  passes on a small band cost about 13 to 15 ms. A band split can help when the
  game GPU's own three-pass cost is clearly above that. The two bands must run
  the same preset; Preset A on one band and B on the other would show a seam.
- **A feature evaluates fewer rows than it was built with.** The split line can
  therefore move between pictures without rebuilding features or restarting the
  worker. Visual equivalence of subset evaluation is not yet verified.
- **Reducing the fixed per-pass cost on Ampere matters more than changing the
  split.** It does not shrink with smaller work. It behaves like per-call setup,
  for example converting weights or running fixed-size stages. Reducing it makes
  both pass placement and band splitting useful.

## Not measured yet

- RTX 5070 Ti numbers. On 2026-11-06 the 5070 Ti refused new D3D12 devices
  (`E_OUTOFMEMORY`) after an `nvlddmkm` event 153 GPU reset during the previous
  game session. Rerun the tool with adapter index 0 after a reboot.
- Transfer cost for a band, seam visibility and temporal stability when the
  split line moves.
