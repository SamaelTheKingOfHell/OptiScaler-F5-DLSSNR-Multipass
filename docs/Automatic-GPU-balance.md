# Automatic GPU pass balancing

Status: experimental implementation; full game calibration and performance acceptance pending.

The Passes & Colour page offers **Automatically balance GPU passes**. It compares
the existing primary-prefix/secondary-suffix placements using measured native
picture submission intervals. This includes the option of keeping every pass on
the game GPU. It optimizes measured cadence rather than trying to make both GPU
utilization percentages equal. A single neural pass is indivisible: it can move
between GPUs, but cannot be divided by this scheduler.

```text
3 passes: 3+0, 2+1, 1+2, 0+3 primary+secondary candidates
1 pass:   1+0 or 0+1; no pass is added or removed
```

Enable multi-GPU Finished Picture in a native DirectX 12 game, with frame
generation, detail reuse, edit cache, Superior Scaling and spatial compression
off. At most three passes are supported. Keep a comparable scene while initially
calibrating. The controller does not silently disable those features, change any
preset, select another runtime, lower resolution or change intensity. Select
runtimes compatible with every pass preset that may move to them. In particular,
the experimental Ampere v0.4.0 runtime targets Preset B; see
[separate runtimes](Multi-GPU-runtimes.md).

```ini
[DlssNr]
MultiGpu=true
MultiGpuPipelined=true
MultiGpuAutoBalance=true
```

Each candidate excludes worker startup and at least thirty warmup pictures over
two seconds. It then needs at least sixty valid intervals over three seconds.
Split changes are at least thirty seconds apart, so an initial search takes
several minutes and can temporarily use a slower split. The controller compares
median and p95 intervals. Keeping another split requires at least an eight
percent and one millisecond median improvement without more than ten percent p95
regression. Failed workers are rejected rather than scored from primary fallback.
Pauses, duplicate picture IDs and stale results do not count.

```text
waiting -> warming -> measuring -> switch cooldown -> next candidate
all candidates compared -> keep measured best
resolution/pass/runtime changes -> new calibration
sustained >20% and >2ms slowdown -> bounded recalibration after at least 60 seconds
```

The menu shows calibration phase, effective pass counts and the last measured
median/p95 interval. Manual handoff checkboxes turn automatic balancing off; the
saved manual mask is preserved during calibration. Measurements describe native
picture submission cadence including remaining waits. They do not isolate neural
inference, PCIe copies, GPU execution or input latency. Scene changes and frame
caps can confound comparisons. The controller does not remove the current
pipeline's shared-state serialization or guarantee that a slower GPU improves FPS.

```text
Controlled acceptance:
same scene/camera, total passes, presets, intensity, model and fixed model resolution
FG/reuse/cache off for all compared routes
compare manual placement with automatic settled placement
retain source-frame p50/p95, worker status, visuals and mode/reset/shutdown evidence
```
