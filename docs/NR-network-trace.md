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
