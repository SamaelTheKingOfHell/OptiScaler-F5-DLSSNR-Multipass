# F5Low frame generation

**F5Low** is an FG Output of its own, next to FSR FG, DLSSG and XeFG. It is our own frame generation, up to 6x.

## What you need

- An NVIDIA GPU. On other GPUs F5Low is greyed out in the menu.
- `f5low.dll` in `OptiScaler\plugins\` (`<game>\OptiScaler\plugins\f5low.dll`; a different `[Plugins] Path` moves
  it, and `<game>\plugins\` applies without an OptiScaler subfolder). It is part of the release zip, already there; without it F5Low is greyed out. An `f5low.ini` next to it is optional (the defaults
  are fine). It used to be called `xefg_cuda.dll`: rename an old copy to `f5low.dll`; an old `xefg_cuda.ini` keeps
  working until you put an `f5low.ini` next to it.
- Intel's `libxess_fg.dll`, version **1.3.4.95** or **1.3.1.78**, in OptiScaler's folder (or `XeFGPath`, or next to
  `f5low.dll`). The zip ships 1.3.4.95. f5low.dll contains none of Intel's code: **on every start it builds its frame generator
  from your `libxess_fg.dll`** in memory, in the background (about half a second); frame generation starts by itself
  when it is ready (until then only the real frames are shown). Nothing is written to disk, so there is no cache
  folder (old `f5low_cache` folders from earlier versions are removed by f5low itself); its log, if you turn it on,
  goes to `%LOCALAPPDATA%\f5low\f5low.log` when the plugins folder is not writable. With another `libxess_fg.dll` version, or if the build fails, f5low.dll hands over to
  Intel's own XeFG (up to 4x, as with FG Output XeFG). The Frame Generation page shows which one runs: "own frame
  generator running", "running on Intel's XeFG" or "unavailable", with the reason on hover. Needs a D3D12 game.

## Turning it on

1. Open the OptiScaler menu, Frame Generation: pick an **FG Input** and set **FG Output** to **F5Low**.
2. **Frame Generator** appears under it: **XeFG Tensor** (our XeFG frame generation on the tensor cores) or
   **DLSS-G (NVIDIA)** or **XeFG (Intel library)**.
3. Save Settings and restart the game.

In the ini: `[FrameGen] FGOutput=f5low`, `[F5Low] FrameGenerator=auto` (= xefg_tensor), `dlssg` or `intel`.

### XeFG (Intel library) as the Frame Generator

`intel` runs Intel's own XeFG (`libxess_fg.dll`) with `libxell.dll`, both from OptiScaler's folder; f5low.dll paces the
frames and unlocks up to 6x in memory (nothing on disk is changed). Intel's library itself runs on any GPU vendor; F5Low
as a whole is still offered on NVIDIA GPUs only. The menu shows a red line when `libxess_fg.dll` or `libxell.dll` is
missing. The DLSS-G redirect and `KeepGameDlssg` below only apply to the DLSS-G generator, not to this one.

### DLSS-G as the Frame Generator

`dlssg` needs an NVIDIA RTX GPU, and Streamline (`sl.interposer.dll` and the `sl.*` plugins) plus `nvngx_dlssg.dll` in
OptiScaler's `streamline` folder. f5low.dll runs NVIDIA's DLSS-G through a private copy of them in
`plugins\f5low_streamline`. The menu shows what is missing.

A game that ships its own `nvngx_dlssg.dll` would load a second DLSS-G next to it, and the GPU faulted at level load in
a game that did. So while FG Output is F5Low and the generator is DLSS-G, OptiScaler keeps every `nvngx_dlssg.dll`
outside `f5low_streamline` from loading and says so in the log ("kept the game's own nvngx_dlssg.dll from loading").
When F5Low's own copy is already loaded, a load of the game's copy by path (or of the driver's) is handed that one
instead, and a plain `nvngx_dlssg.dll` load by the game's Streamline gets it from the loader: the game shares F5Low's
DLSS-G, which is intended. The block only applies when F5Low can run here (NVIDIA GPU, `f5low.dll`, Streamline and
`nvngx_dlssg.dll` in OptiScaler's `streamline` folder). `[F5Low] KeepGameDlssg=true` turns the block off. It is also not done when FG Input is DLSSG, because the game's DLSS-G is
the input then; the log warns that two copies may fault.

With the DLSS-G generator and FG Input other than DLSSG, OptiScaler also forces the game's own DLSS-G off in
`slDLSSGSetOptions` (logged once): on RTX 20-40 f5low patches the shared `nvngx_dlssg` only when its own context
starts, so the game must not turn DLSS-G on. FG Input DLSSG and `KeepGameDlssg=true` leave the game's options alone.
This is only done when F5Low can run here (NVIDIA GPU, `f5low.dll`, Streamline and `nvngx_dlssg.dll` found), and the
game's `slAllocateResources` for DLSS-G is skipped too. Streamline 1 games and the Steam overlay path are not covered by
the force-off, and the game's own FG counter may show 1x while F5Low generates.

## Options

F5Low uses the same options as XeFG (the "Frame Generation (F5Low)" section, `[XeFG]` in the ini): multiplier,
UI composition, borderless, latency output and so on. `[XeFG] MaxInterpolations` caps the multiplier (5 = up to 6x).

Changing between XeFG and F5Low needs a restart: only one of the two libraries can be loaded per game session.

## Checking that it runs

The overlay says `F5Low 2x` (or 3x ... 6x). With `LogToFile=true` the log has:

```
F5Low: loaded ...\plugins\f5low.dll
F5Low: Frame Generator XeFG Tensor (XEFG_CUDA_GENERATOR=own)
Max supported interpolations: 5 (F5Low library reports 5, MaxInterpolations 5)
```

If it cannot start at all, a notice appears and no frames are generated: set FG Output to XeFG and restart.
With `LogToFile=true` the log then says why (`F5Low: ...`).

`f5low.ini` in the plugins folder has its own settings (pacing, logging); the defaults are fine.
