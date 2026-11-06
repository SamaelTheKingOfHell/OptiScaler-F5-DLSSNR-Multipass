# DLSS-NR network trace: can the network be split between GPUs?

Status: research measurement, 2026-11-06. Nothing in the renderer depends on it.

## Tool

`tests/nr_gpu/nr_cuda_trace.cpp` loads the model through the forwarder on one
adapter. It hooks `nvapi_QueryInterface` with Detours, the same entry point
`OptiScaler/nvapi/NvApiHooks.cpp` wraps. It then records every
`NvAPI_D3D12_LaunchCuKernelChain` kernel:
- the kernel name (from `NvAPI_D3D12_CreateCuFunction`),
- the launch shape and parameter size,
- the GPU addresses in its parameter blob,
- a timestamp after each launch call.

The model makes no CUDA driver API calls during evaluation, so it cannot be
traced at that level.

The model sub-allocates all tensors from one arena. Every distinct
address-like parameter word in the arena is therefore treated as a tensor whose
size is the gap to the next address, an upper bound. For each cut between
consecutive kernels, the tool sums the tensors used on both sides.

```text
cl /std:c++17 /EHsc /O2 /I OptiScaler\include tests\nr_gpu\nr_cuda_trace.cpp
   /link OptiScaler\library\detours\detours.lib d3d12.lib dxgi.lib user32.lib
nr_cuda_trace.exe <_nvngx.dll> <forwarder> <nvngx_dlssnr.dll> <data dir> <adapter> <w> <h> <preset> <frames> <out>
```

## Network shape (1280x720 model input)

| | Preset A, RTX 5070 Ti | Preset B, RTX 5070 Ti | Preset B, RTX 3060 (Ampere v0.4.0) |
|---|---|---|---|
| Kernels per evaluate | 156 (43 distinct) | 80 (39 distinct) | 80 (39 distinct) |
| Time first to last launch | 2.93 ms | 1.65 ms | 7.68 ms |

The network is a U-Net:
- **Encoder:** a pre-block and fused Swin stages at 1, 2, 4 and 8 heads
  (`swin_1h_32` to `swin_8h_256`), each downsampling.
- **Bottleneck:** split Swin 16-head blocks (`split_swin_16h_*_512`) and global
  ViT blocks (`vit_1d_*`).
- **Decoder:** the same Swin stages upsampling back, a post-block, and a final
  copy.

In Preset A, one ~31.5 MiB arena address is used from the first kernel to the
last. This is most likely the weight block.

Activation data crossing each cut (Preset A, weights excluded, upper bounds):

| Cut | 5070 Ti time before the cut | Crossing per evaluate |
|---|---|---|
| After the first encoder stage | 11% | 8.4 MiB |
| Middle of the ViT bottleneck | 50% | 30.3 MiB |
| Start of the decoder | 74% | 15.4 MiB |
| Before the last decoder stage | 89% | 8.4 MiB |
| After the last decoder stage | 93% | 0.02 MiB |

The encoder's skip tensors (7.9, 3.9, 2.0 and 1.0 MiB at the four
resolutions) live until the matching decoder stage. That is why almost every
cut carries megabytes, more than a whole 1280x720 RGBA16F picture (7 MB).

## Same network on both GPUs

With Preset B, the RTX 3060 running the Ampere v0.4.0 runtime launches exactly
the same 80 kernels in the same order. Names, parameter sizes and launch shapes
are identical, and the names keep their `_fp8` suffix. The patched runtime
changes instructions, not the in-memory tensor layout. A handoff in the middle
of the network is therefore structurally possible. Numerical agreement at a cut
is not verified.

Per-group time, Preset B, 1280x720:

| Group | Kernels | 5070 Ti | 3060 | Ratio |
|---|---|---|---|---|
| pre_block | 1 | 0.148 ms | 0.801 ms | 5.4 |
| swin_1h_32 | 4 | 0.135 | 0.809 | 6.0 |
| swin_2h_64 | 4 | 0.107 | 0.718 | 6.7 |
| swin_4h_128 | 4 | 0.098 | 0.629 | 6.4 |
| swin_8h_256 | 4 | 0.155 | 0.744 | 4.8 |
| split_swin_16h | 17 | 0.197 | 0.640 | 3.3 |
| vit_1d | 42 | 0.620 | 2.376 | 3.8 |
| post_block | 1 | 0.162 | 0.892 | 5.5 |

The 3060's cost is spread over the whole network. There is no single fixed
setup cost to remove.

If the 3060 takes the decoder tail after the 4-head stage (cut 72), it runs
1.84 ms of work per pass and saves the 5070 Ti 18% of a pass (0.30 ms). After
the 1-head stage (cut 76), it runs 1.13 ms and saves 0.20 ms.

## What a split would need

1. **Frame pipelining.** The helper's tail for picture N runs while the game GPU
   renders N+1.
2. **Copy-engine transfers** of the crossing tensors, so they do not use the
   game GPU's direct queue. 8 to 15 MiB per pass at 720p is under 1 GB/s at
   60 pictures per second. The current multi-GPU transfers use the direct
   queue.
3. **A launch filter in the worker.** The helper keeps its own feature, with
   its own weights at the same arena layout. It skips the kernels before the
   cut, and the crossing tensors are written into its arena at the same
   offsets. OptiScaler already filters launches for ViT reuse (`VitDrop` in
   `DlssNrNative.cpp`).
4. **Per-pass resets and history consistency.** The recurrent state lives in
   the arena of whichever GPU runs those kernels.

Expected gain on the RTX 5070 Ti + RTX 3060: about 0.2 to 0.3 ms of game-GPU
time per pass with Preset B at 1280x720, and roughly four times that at
2560x1440. That is one to a few milliseconds per picture across three passes,
plus one picture of latency. A stronger helper moves the cut earlier and gains
proportionally more.

## Prototype results so far (2026-11-06)

`tests/nr_gpu/nr_split_splice.cpp` and `nr_nvapi_harness.h` run three instances
of the same model on one GPU: a reference, an owner and a helper. They compare
the spliced output with the reference over five frames of changing synthetic
input. All results below are Preset B, 1280x720, RTX 5070 Ti.

| Experiment | Result |
|---|---|
| Two full instances (baseline) | Bit-identical every frame: the model is deterministic |
| Helper runs the tail after cut 66 (14 tensors, 50.8 MiB forward) | Frame 1 bit-identical. Frames 2+: max 0.004, mean 0.0005 |
| Helper runs the tail after cut 76 (4 tensors, 32.6 MiB) | Max 0.004, mean 0.0005 from frame 1 |
| Helper runs the tail after cut 72 (7 tensors, 46.0 MiB) | Max 0.05 to 0.07: an input is missing |
| Helper runs a middle segment; owner's last two kernels held back and replayed | Max 0.38: wrong, see below |

What these establish:
- **Splicing within a frame works.** At cut 66, copying the tensors referenced on
  both sides of the cut into a second instance's arena, at the same offsets,
  reproduced the reference bit for bit on the first frame.
- **Arena offsets transfer between devices.** With Preset B, 206 of 213 arena
  addresses differ between the RTX 5070 Ti and the RTX 3060 by one constant,
  and 77 of 80 kernel parameter blobs are identical after that shift. The
  other three are the input/output and constant-buffer kernels; they use each
  device's own handles and need no translation.
- **Temporal state is the open problem.** From the second frame on, the owner's
  encoder output already differs from the reference at the cut, even when the
  crossing tensors are copied back after the tail runs. The encoder therefore
  reads something the tail wrote the previous frame that the tensor-boundary
  heuristic does not capture. Candidates: the last arena tensor, whose size is
  unknown, or regions addressed only through offsets inside other tensors.
- **Holding back kernels and replaying them is not a valid way to emulate a
  mid-evaluate wait.** The model records its own D3D12 commands between kernels,
  including state changes for its I/O textures, and replaying kernels later
  breaks that ordering. A real split needs the game GPU to wait inside the
  evaluate without reordering. One option is a spin-wait kernel on a shared
  flag with a bounded timeout, placed between the owner's kernels. The model
  already synchronizes its own kernels this way (its `*_wait` and
  `*_tilesync` kernels).

Next steps before a cross-GPU prototype:
1. Find the temporal state exactly. Snapshot the whole arena (bounded by the
   real allocation size, which needs D3D12 allocation hooks on every device
   interface) before and after an evaluate, and diff the regions the head
   reads.
2. A spin-wait kernel with a timeout. The CUDA toolkit's `ptxas` builds it,
   like `DlssNrVitCopyKernel.h`.
3. Then the cross-GPU version: the helper worker filters its own launches and
   receives the forward tensors through the existing cross-adapter buffer.

## Robust split design: two owner evaluates, exact buffers (2026-11-06)

**Exact allocations.** With `NR_ALLOC_CALLBACK=1`, the harness passes NGX a
`ResourceAllocCallback`. The model then allocates through the host. Each
feature allocates, at 1280x720 Preset B:
- a 108.69 MiB UAV buffer (weights),
- a 108.69 MiB buffer created in `GENERIC_READ`,
- a 66.31 MiB UAV buffer (activation workspace),
- a 1280x720 RGBA16F texture (temporal history).

Copies can now use exact bounds.

**Where the temporal state lives.** With the helper running the tail, copying
the whole workspace forward made frame 1 bit-identical at every cut tested
(66, 76). Copying it, or every writable buffer, back did not stop the drift
from frame 2. The state carried between frames is therefore the history
texture, which the last kernels write and the first kernel reads.

**Design: the owner evaluates twice per picture.**

```text
owner call 1: kernels [0, cut]        -> forward set -> helper: kernel 0 + (cut, resume)
                                                             |
owner call 2: kernel 0 + [resume, end) <- backward set <-----+
```

Every evaluate keeps the model's own D3D12 ordering. The cross-GPU wait sits
between two evaluate calls, so it can be a queue-level fence guarded by the
existing worker watchdog, with no spin-wait kernel. History stays on the
owner.

**Read-only write-set discovery** (`SPLICE_DISCOVER=1`). A separate probe
instance snapshots its workspace before kernel 1, after the head and after the
segment, over three frames of changing input. A block counts as written when
its content changes. Writing a sentinel pattern into the workspace was tried
first and hung the GPU: the workspace holds the model's synchronization
counters, so it must never be poisoned.

| Cut 66, resume 78, Preset B, 1280x720 | Result |
|---|---|
| Whole workspace both ways | max 0.0012 frame 1, about 0.004 after |
| Discovered sets: written runs the receiving segment references | same output; forward 66.19 MiB (2 runs), backward 15.75 MiB |
| Every written run | same output; forward 66.23 MiB (61 runs), backward 19.71 MiB |
| Control: the helper runs only kernel 77 | max 0.0012 frame 1, about 0.004 after |

The remaining difference comes from evaluating the owner twice, not from the
transfer. It is about 1/255 of the output range. Its source is not identified
yet. Candidates are per-evaluate host-side state, such as a frame counter, or
an end-of-evaluate copy recorded by the first call.

Open items:
1. Shrink the forward set. The head writes almost the whole workspace in
   contiguous runs, so "referenced runs" stays at 66 MiB. The decoder's real
   inputs (encoder skip tensors and bottleneck output) should be 15 to 20 MiB.
   Next is empirical minimization: drop chunks while the output stays
   identical to the whole-set result.
2. Explain or remove the two-call residue.
3. Cross-GPU: the worker filters its launches and receives the sets through
   the cross-adapter buffer. The game GPU's queue waits between the owner's
   two evaluates.

## Cross-GPU prototype (2026-11-06)

`nr_split_owner.exe` runs on the game GPU (RTX 5070 Ti, game runtime) and
`nr_split_helper.exe` on the second GPU (RTX 3060, Ampere v0.4.0 runtime). The
helper is a separate process started through the production `WorkerClient`.
It uses the same cross-adapter heap and fences. Preset B, 1280x720, cut 66,
resume 78. Ranges from the single-GPU minimizer: 15.79 MiB forward (64
ranges), 7.50 MiB back (5 ranges).

| Measurement | Value |
|---|---|
| Error vs single-GPU reference, 12 pictures | max 0.0037 to 0.0046, mean 0.00049 |
| Game GPU, single-GPU evaluate | 1.639 ms |
| Game GPU direct queue with the split (PCIe copies on a copy queue) | 1.402 ms (head 1.205 + end 0.196) |
| Helper GPU: copy in, its segment, copy out | 1.233, 1.745, 0.597 ms |
| Head end to end start (copies + helper + IPC), to be hidden by pipelining | 6.05 ms |

The game GPU saves 0.24 ms (14.5%) of direct-queue NR time per pass with the
helper taking 27% of the network. Earlier cuts give the helper more. At about
4x the game GPU's time, the 3060 fits most of the network into a 60 fps frame
when pictures overlap.

**What the minimized ranges are.** The workspace starts with about 60 KiB of
tiny slots (0.2 to 0.8 KiB), each referenced by a consecutive kernel pair.
These are the model's kernel-to-kernel synchronization flags, and they must
always cross. The data is a few large sub-ranges inside bigger tensors: 6.0,
1.1, 7.6 and 1.1 MiB forward and 2 x 3.75 MiB back. These are the U-Net skips.
No kernel parameter points at their starts.

**The two-call residue is not the split.** Evaluating an unsplit instance one
extra time per picture (`SPLICE_EXTRA=1`) gives the same signature: frame 1
exact, then max 0.004 and mean 0.0005. The second evaluate per picture
advances per-evaluate internal state.

**GPU resets caused by these experiments.** Four on the RTX 5070 Ti, at 13:37,
13:53, 14:09 and once earlier. The causes were a 4 GiB copy beyond the
workspace, sentinel-filling the workspace, and a minimizer trial at cut 9 that
left out data a `wait` kernel depended on. Trial-and-error minimization can
starve the model's wait kernels. It must not run on the display GPU.

## Tensor shape decoding: first findings (2026-11-06)

This is the safe path to transfer sets: compute them from kernel parameters,
with no trial runs that can hang a GPU. The Swin stage kernels (`*_inpview_*`,
`*_ds_wait_*`, `*_upsample_*`, `*_outview_*`) carry tensor pointers followed
by padded dimensions (height, width) as 32-bit fields. At 1280x720, Preset B:

| Stage | Padded H x W in params | Channels (kernel name) | H*W*C bytes |
|---|---|---|---|
| 1h_32 | 384 x 672 | 32 | 8064 KiB |
| 2h_64 | 192 x 336 | 64 | 4032 KiB |
| 4h_128 | 96 x 168 | 128 | 2016 KiB |
| 8h_256 | 48 x 84 | 256 | 1008 KiB |

The 1h_32 stage tensors sit at workspace offsets 32340K, 40404K, 48468K and
56532K. That spacing is exactly 8064 KiB = 384 x 672 x 32 bytes (FP8): one
byte per channel. `pre_block` also carries the picture (720 x 1280) and its
padded size (768 x 1344). Next steps:
1. Decode the remaining kernel families (split_swin, vit_1d) the same way.
2. Compute each tensor's extent from pointer and shape, plus the synchronization
   area (workspace offsets 0 to about 64 KiB).
3. Check the computed sets against the cut-66 minimizer result.
4. Validate them with read-only runs.
