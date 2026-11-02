# Side-by-side secondary-GPU runtimes

The first pass marked **Start second GPU here** owns the runtime selection for
the entire secondary pass group. The game GPU keeps its existing runtime. All
secondary passes remain sequential on one GPU and the result returns to the game
GPU for composition. Per-pass presets and appearance controls remain independent.

Supply model DLLs separately; this project does not distribute them. Place each
alternative below the OptiScaler or game executable directory:

```text
OptiScaler/NR-Models/ampere-small-v040/nvngx_dlssnr.dll
OptiScaler/NR-Models/another-runtime/nvngx_dlssnr.dll
```

Use the runtime selector inside the first secondary pass card, then choose the
appropriate preset for that runtime in each secondary pass. **Refresh runtime
list** discovers newly added folders. Folder labels accept letters, digits,
hyphens and underscores, up to 64 characters. Empty/`auto` retains the game model.
Restart the game after replacing a model DLL in an existing folder.

```ini
[DlssNr]
MultiGpu=true
SecondaryPassMask=1
SecondaryModel=ampere-small-v040
Preset=2
```

An explicitly selected missing or incompatible runtime does not silently load a
different secondary runtime. The existing failure path keeps the frame on the
game GPU and reports the unavailable worker. A folder name does not establish
compatibility, provenance or performance.

## Ampere candidate

[DLSS5-RTX-3080-laptop](https://github.com/lmxxf/DLSS5-RTX-3080-laptop)
publishes MIT-licensed optimization source for Ampere `sm_86`. Its v0.4.0 runtime
targets the **smaller Preset B / preset 2 model**. Treat it as experimental: its
numerical changes are not universally bit-identical to the compatibility runtime.
Do not select it for Preset A or silently change A to B when comparing speed.

The author's [pinned release record](https://github.com/lmxxf/DLSS5-RTX-3080-laptop/blob/cb716c70edc6351a5ebcbf6d1ca8da0c297fad1f/Development/ptx-patcher/release-rtx30/v040/validation.json)
identifies the model below. The source license does not replace NVIDIA's model
license. Obtain the runtime separately and verify the file before testing it:

```text
nvngx_dlssnr.dll: 266414592 bytes
SHA256: 749daadb43f11b2fc0bf39d92a67809978f27b9e96150b631d50fc8d1ec9e3ca
```

```powershell
Get-FileHash .\OptiScaler\NR-Models\ampere-small-v040\nvngx_dlssnr.dll -Algorithm SHA256
```

## Controlled measurements

A 30-frame synthetic two-GPU fixture at 1280x720 on RTX 5070 Ti + RTX 3060
measured Preset A handoff medians of 30.5854 ms before worker command-object reuse
and 30.502 ms afterward, with identical checksums for all 30 returned frames.
This difference does not establish a speed gain. GPU timestamps attributed about
26.7 ms to the 3060's neural evaluation. The existing compatibility runtime's
Preset B evaluation measured about 15.0 ms in a separate run. A and B produce
different images; these are not interchangeable quality settings or game FPS.
The optimized Ampere candidate has not yet been measured on this GPU pair.

The fixture accepts a separate worker model while preserving the primary model.
It reports CPU handoff and worker GPU stage timings. `-SwapAdapters` is a
diagnostic comparison of the opposite transport direction, not a game setting.

```powershell
.\tests\run_nr_cross_process_neural.ps1 `
  -ForwarderDll .\runtime\nvngx.dll_dlssnr.dll `
  -ModelDll .\runtime\nvngx_dlssnr.dll `
  -SecondaryModelDll .\OptiScaler\NR-Models\ampere-small-v040\nvngx_dlssnr.dll `
  -WorkerExe .\x64\Release\a\OptiScaler\NR-Worker\dlssnr_worker.exe `
  -NoPrimaryPass -SecondaryPasses 1 -Width 1280 -Height 720 -Benchmark -DepthStencil
```
