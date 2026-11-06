# Split-frame (band) multi-GPU: measurements

Status: measured on both GPUs. Split-frame scheduling was not added: the measured best case does not justify it (see Decision).

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

## RTX 5070 Ti, 2026-11-06 (game runtime, median GPU ms per pass)

| Size | Preset A | Preset B |
|---|---|---|
| 1280x720 | 2.93 | 1.64 |
| 1280x544 | 2.58 | |
| 1280x368 | 2.33 | |
| 1280x272 | 2.27 | |
| 1280x192 | 2.26 | |
| 1280x96 | 2.25 | |
| 1920x1088 | 4.24 | |
| 3840x2160 | 15.27 | 8.79 |
| 3840x1888 | | 7.69 |
| 3840x1088 | 7.86 | |
| 3840x544 | 4.50 | |

RTX 3060 at 3840-wide bands: Preset A 272 rows 34.81 ms. Ampere Preset B: 544
rows 15.35 ms and 272 rows 9.60 ms.

## Decision

The 5070 Ti does a whole 1280x720 Preset A pass in 2.93 ms. The 3060 needs
14 ms or more for any band of the same picture, and the 5070 Ti's own floor is
2.25 ms. At the current 1280x720 model size, removing rows from the 5070 Ti
saves at most about 0.7 ms per pass, or 2 ms for three passes. That is less
than the transfer and synchronization cost.

At a 3840x2160 model size, Preset A on the 3060 costs more for 272 rows than
the 5070 Ti costs for the whole picture. The best Preset B case gives the 3060
272 rows: three passes take about 28.8 ms. That saves the 5070 Ti about 3.3 ms
of its 26.4 ms. A same-frame split would be slower. An across-frame pipeline
could gain at most about 10 percent. It would add a frame of latency and the
risk of a visible seam.

The two GPUs differ by roughly 5 to 10 times in this model's throughput. The
remaining 3060 gap is mostly hardware: it has no FP8 tensor cores, and the
Ampere Preset B runtime is already about 4.5 times slower than the 5070 Ti at
720p. Better Ampere kernels would mainly reduce the fixed per-pass cost, which
is still worth investigating for 3060-only users. They would not make the 3060
a large share of this pair. For this GPU pair, the best setting for neural
rendering is the game GPU alone (`MultiGpu=false`). That means
primary-only placement.

## Real two-GPU handoff: one pass on each GPU, 2026-11-06

`run_nr_cross_process_neural.ps1 -SecondaryPasses 1 -Width 1280 -Height 720 -Benchmark -DepthStencil`.
The 5070 Ti ran the primary pass with the game runtime. The 3060 ran its pass
with the Ampere v0.4.0 runtime. Thirty frames returned, and the worker exited
cleanly.

```text
Handoff latency median 13.00 ms, p95 13.25 ms
3060: input copies 1.15 ms, neural pass 7.61 ms, return copy 0.56 ms, queue completion 9.58 ms
```

With pipelining, the 3060 can keep up to roughly 100 pictures per second when
it takes the last pass. The 5070 Ti stops running that pass: 1.64 ms with
Preset B or 2.93 ms with Preset A. It still records the outbound and return
copies and the pipelined picture copy, so expect a small net gain at 1280x720
and one extra picture of latency. Compare the in-game results with the same
pass presets and multi-GPU off.
