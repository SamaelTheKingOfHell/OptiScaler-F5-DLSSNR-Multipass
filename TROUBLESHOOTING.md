# OptiScaler-DLSSNR troubleshooting guide

For players of OptiScaler-DLSSNR (the F5 builds). Covers problems we have diagnosed in real reports, the log lines that
show each one, and what to do.

## How to use this guide with an AI assistant

1. Close the game. In `OptiScaler.ini` set `[Log] LogToFile = true` and `LogLevel = 2`.
2. Start the game, make the problem happen once, then close the game.
3. Upload two files to the assistant: this guide and `OptiScaler.log` from the game folder (the folder OptiScaler is
   in). Not the MFGUnlocker or RTXMFG log: it has nothing from OptiScaler in it.
4. Paste the prompt below, plus one sentence on what you see (for example "the game closes at start").

> Use the troubleshooting guide to check my OptiScaler.log. For each guide entry whose log signs appear in my log, quote
> the matching log line, name the entry, and tell me its fix. If no entry matches, say so plainly and tell me what to send
> to the developers. Do not guess a cause that is not in the guide.

Afterwards set `LogToFile = false` again. The log grows fast and can slow the game. If your log is larger than about
5 MB, it was probably written at a more detailed log level: set `LogLevel = 2` and make a new one.

## Instructions for the AI assistant reading this guide

- Match on the **Log signs** of each entry. An entry applies only when one of its log signs (or the same message with
  different numbers, names or paths) is in the player's log. Quote the line you matched.
- Several entries can apply at once. List them in the order they appear in the log; the earliest one is often the cause
  of the later ones.
- Entries marked **Not solved yet** have no known cause. Say that, and give their "Send the developers" list. Do not offer
  a cause of your own for them.
- If nothing matches, say "this problem is not in the guide", do the **First checks** below, and list what to send to
  the developers (at the end of this guide). Do not invent settings: only suggest ini keys and menu items named in this
  guide.
- The log's first line names the version, for example `OptiScaler v0.1.32-final (9f835081) loaded`. An entry marked
  "Fixed in" a later version than the player's applies; tell them to update.

---

## First checks (do these for every log)

**Version.** The first line says which build runs (`OptiScaler v0.1.32-final ...`). A line like
`OptiScaler-DLSSNR is up to date (current 0.1.32)` confirms it is the latest. If the player says they run one version and
the log shows another, an old DLL is still in the game folder.

**Plugins.** Lines like `LoadAsiPlugins Loaded: ...\plugins\MFGUnlocker.asi` show third-party plugins. They are not
part of OptiScaler. When the problem is unclear, test once with `[Plugins] LoadAsiPlugins = false`.

**Which DLL name OptiScaler runs as.** `CheckWorkingMode OptiScaler working as winmm.dll` (or dxgi.dll, version.dll, ...).
Next to dxvk, OptiScaler must not be `dxgi.dll` (see the dxvk entries).

**Game's exe.** `CheckQuirks Game's Exe: <name>.exe` names the program OptiScaler loaded into. If it is a launcher, not
the game, the log is from the wrong process.

---

## Setup check: settings that don't fit the game

Many "it does nothing" reports come from settings made for a different kind of game, often an `OptiScaler.ini` copied
from another game. Check them in this order. The settings in use are listed in the log's `Config parameters:` block near
the top (`Section.Key: value`, for example `Upscalers.Dx12Upscaler: dlss` or `DlssNr.NativeUpscaler: true`); a key that
isn't listed there is on its default. If the player also uploads `OptiScaler.ini`, read the values there.

### Step 1: which graphics API the game uses
- **Vulkan:** lines from `hkvkCreateDevice` or `hkvkCreateSwapchainKHR`.
- **DirectX 11:** `Failed to get ID3D12CommandQueue from pDevice, creating Dx11 swapchain!`.
- **DirectX 11 on dxvk** (dxvk turns DirectX 11 into Vulkan): a DirectX 11 game with dxvk's `d3d11.dll` in the game
  folder. The log's `dxvk: true / false` in the GPU list is not always right; ask the player.
- **DirectX 12:** none of the above, and the game's swapchain is made on DirectX 12.
- Every log has a few DirectX 12 lines (OptiScaler makes DirectX 12 devices of its own), so a `hkD3D12CreateDevice` line
  alone does not mean the game is DirectX 12.

### Step 2: does the game call an upscaler of its own?
- **Yes** when the log shows the game's upscaler being created, for example `InitDLSS Creating DLSS feature`,
  `InitDLSSD Creating DLSSD feature`, or FSR / XeSS feature lines, after the game starts.
- **No** (most older games, emulators, many indie games): then only **Optical F5Low** can give DLSS-NR and frame
  generation anything to work with. Its lines start with `Native motion` and `Virtual upscaler:`. If neither appears and
  the game has no upscaler, Optical F5Low is off.

### Step 3: check the settings for that kind of game

**All games**
- DLSS-NR needs `[DlssNr] Enabled = true`. On NVIDIA it needs an RTX card (see the GTX entry); AMD and Intel cards need the
  vendor-neutral port (see its section).
- Frame generation needs `[FrameGen] Enabled = true` and an output: `FGOutput = xefg` or `fsrfg` (or `dlssg`). With
  `FGOutput = nofg` nothing is generated.

**Game with its own upscaler, DirectX 12**
- `[Upscalers] Dx12Upscaler` picks what replaces the game's upscaler (`dlss`, `ffx`, `xess`, `fsr21`, `fsr22`).
- `[FrameGen] FGInput = fsrfg` and `fsrfg30` work only in DirectX 12 games.

**Game with its own upscaler, DirectX 11**
- DLSS-NR runs only through the DirectX 11-on-12 backends: `[Upscalers] Dx11Upscaler` must be one whose name ends in
  `_12`; the ini recommends `dlss_12`. With `dlss`, `fsr22`, `fsr31` or `xess` (the plain DirectX 11 backends) DLSS-NR
  never runs.
- `FGInput = dlssg` is not available in DirectX 11 games; `fsrfg` and `fsrfg30` need DirectX 12.

**Game with its own upscaler, Vulkan**
- DLSS-NR works with the game's DLSS, FSR or XeSS. With a `[Upscalers] VulkanUpscaler` ending in `_12` (`ffx_12`,
  `fsr21_12`, ...) it runs on DirectX 12; with the plain Vulkan ones it runs on Vulkan itself.
- XeFG, or frame generation from the game's upscaler on the bridge, needs the Vulkan route "Bridge, game's upscaler"
  (Frame Generation page):
  `[DlssNr] NativeFrameGenerationOnly = true`, together with `FGInput = upscaler` and an `FGOutput`, set before the game
  starts. Without it the log says
  `frame generation is chosen, but neither "NR + frame generation" nor the Vulkan route "Bridge, game's upscaler" is on: the game presents as it is`.
  For a game with its own upscaler the answer is the Vulkan route: the "NR + frame generation" mode is for games without
  one, and Optical F5Low stands aside for good once the game calls its own upscaler. NR stays on or off by itself.
- The other Vulkan route, "Native swapchain" (FSR frame generation or DLSS-G on the Vulkan swapchain, no XeFG), is made
  when the game's device is made: switching to it from the bridge takes effect after a restart.

**Game without its own upscaler (any API): Optical F5Low**
- "NR + frame generation" mode needs all of: `[DlssNr] Enabled = true`, `NativeMotion = true`, `NativeUpscaler = true`,
  `NativeDepthFinder = true` (optional but better), `NativeInput = false`, `FinishedPicture = false`, and for frame
  generation `[FrameGen] FGInput = upscaler` plus an `FGOutput`. Choosing the mode on the Optical F5Low page sets these.
- In a **Vulkan** game, frame generation works only in this mode (or in the Vulkan route "Bridge, game's upscaler" above): the same "neither ... is on"
  line appears when it is missing.
- `FinishedPicture = true` is for DirectX 12 games only. In a Vulkan game it keeps DLSS-NR from starting (the menu shows
  "Waiting for a finished picture.").
- A copied `NativeFrameGenerationOnly = true` does nothing useful in a game without an upscaler: it is for Vulkan games
  that call their own.

**DirectX 11 game on dxvk**
- OptiScaler must not be named `dxgi.dll` (dxvk needs that name).
- Frame generation needs `[DlssNr] NativeDxvkVulkan = true`. The DirectX 11 frame generation bridge cannot work on dxvk.

**Settings that only work on one API (harmless elsewhere, but a sign of a copied ini)**
- DirectX 12 only: `FinishedPicture`, `NativeDepthOverlay`, `NativeDebugView` (the menu's depth and motion previews),
  `NativeDepthResolveInList`, `EditCache`.
- Vulkan only: `NativeDepthVkCopyUsage`, `NativeFrameGenerationOnly`.
- dxvk only: `NativeDxvkVulkan`, `NativeLowLatencyDxvkGameThread`.
- `VitEvery` / `VitEveryPlain` ("Reuse bottleneck") are ignored in Vulkan games: it is always computed there.

---

## Starting the game

### The game closes at start with dxvk (DirectX 11 game)
- Log signs: `Dx11wDx12 swapchain creation failed: real 80004002, fg 80004005`, then `Unhandled exception C0000005`.
- Cause: dxvk's `d3d11.dll` is running with Windows' own `dxgi.dll`, which cannot make a swapchain for it. Usually
  dxvk's `dxgi.dll` is missing from the game folder. In this case the GPU list near the top of the log says `dxvk: false`
  even though dxvk is installed: check the game folder for a `d3d11.dll` instead. If there is none, dxvk is not involved
  and this entry does not apply.
- Fix: put both dxvk DLLs (`d3d11.dll` and `dxgi.dll`, 64-bit, same version) next to the game's exe. OptiScaler must
  then use another name, such as `winmm.dll` or `version.dll`. For frame generation under dxvk, set
  `[DlssNr] NativeDxvkVulkan = true`. If it still crashes, test without OptiScaler: if it crashes then too, the problem is
  dxvk or the game. Close RTSS when testing (its overlay hooks dxvk).

### A Vulkan game or emulator closes at start ("could not create device") — Not solved yet in a release
- Log signs: `DLSS-NR Vulkan: vkCreateDevice returned -3 with N extensions requested`. (This line is only written when
  DLSS-NR is enabled; with it off the log simply stops after `hkvkCreateDevice ... Adding Vk w/Dx12 Vulkan extensions`
  and no `Vulkan Reflex: VK_NV_low_latency2 is on for this device` follows.)
- Seen: the KytyPS5 emulator on an RTX 5070 Ti, v0.1.32.
- Cause found for that case: the NVIDIA driver refused the program's device when OptiScaler added one of NVIDIA's
  extensions (`VK_NVX_binary_import`). The program starts without OptiScaler.
- Fix: none in v0.1.32. A fixed build is in progress. Send the developers the log and the program's name.

### Assassin's Creed (Odyssey, Unity, Syndicate) shows a D3D11CreateDevice error at start
- Fixed for these games by a built-in quirk. For any other game with this error: `[Hooks] D3D11FeatureLevelElevation = false`.

### Resident Evil 3 with dxvk crashes at start
- The Steam main version of the game always runs DirectX 12, whatever its config says; dxvk then breaks frame
  generation. DirectX 11 needs the game's `dx11_non-rt` Steam beta.

### BeamNG (Vulkan) stalls, then closes
- Fixed in v0.1.29: update. BeamNG's helper process overwrites `OptiScaler.log` when it exits, so to get a useful log set
  `[Log] SingleFile = false`.

### The antivirus flags the OptiScaler DLL
- No log sign. A Windows Defender result ending in `!cl` or `!ml` is an automatic cloud verdict and often clears within
  hours. OptiScaler works by loading under a system DLL's name and patching the game in memory, which is what scanners
  react to. Download only from the official release page; restore the file and add an exclusion, or re-scan a few hours
  later.

---

## Upscaler

### DLSS is selected but it switches to FSR or XeSS by itself — Not solved yet
- Log signs: `DLSSFeatureDx12::InitDLSS _CreateFeature result: BAD00007`, then `falling back to FSR 2.1.2` or
  `changing backend to XeSS`, and no `init successful for DLSS` anywhere.
- Send the developers: the log, and a second log of the same game with frame generation output off.
- Note: in a game without its own upscaler (Optical F5Low, see below) the same `BAD00007` line followed by
  `Virtual upscaler: the DLSS backend failed to initialise` and `Virtual upscaler: FSR 3.X ready` is harmless: with NR on,
  that backend only feeds frame generation.

### Crash when switching upscaler (Resident Evil 2, old overlay menu)
- Fixed in v0.1.29. On older builds keep `[Menu] OverlayMenu` on `auto`.

---

## DLSS-NR (Neural Rendering)

### DLSS-NR does nothing on a GTX card (GTX 10 or 16 series)
- Log signs: `NVNGXProxy::InitDx12 result: BAD00001`, then `DLSS-NR unavailable: the NGX core would not initialise`.
  The GPU list near the top already says `dlss: false`.
- Cause: NVIDIA's DLSS-NR model needs an RTX card. GTX cards are refused by NVIDIA's own runtime. Not a bug.
- Fix: none for NVIDIA's model on GTX cards.

### DLSS-NR fails to start: "create failed ... FAIL_PlatformError"
- Log signs: `DLSS-NR create failed: init 0x1 (Success), create 0xBAD00002 (FAIL_PlatformError)`, and later
  `DLSS-NR did not run: it already failed this session` or `Virtual upscaler: Evaluate N -> no pass (NR did not run)`.
- Check first: the `DLSS-NR forwarder loaded from <path>\nvngx.dll_dlssnr.dll` line shows which forwarder file is used.
  That file must come from the same OptiScaler-DLSSNR release as the OptiScaler DLL. An older copy left in the game folder
  from an earlier version is a known cause. Replace it with the one from your release zip.
- If the forwarder is current: send the developers the log, the file version of `nvngx_dlssnr.dll` (right-click,
  Properties, Details) and the GPU.

### Brightness pumps or flickers on fades and scene cuts
- Log signs: `DLSS-NR composition: paper white` jumping between very small and very large values within a few frames, or
  `DLSS-NR game exposure` lines with values swinging by a huge factor around `scene cut found`.
- Cause (likely): the white point follows the game's exposure (`WhitePointSource = 1`, "Game exposure") with no easing, and
  the game's exposure swings wildly during fades.
- Fix: in the DLSS-NR menu set the white point source to Automatic exposure and set Adapt brighter / Adapt darker to about
  0.5-1.0 seconds. Or keep Game exposure and accept the pumping on fades.

### DLSS-NR seems to do nothing on an SDR game
- Applies to builds before v0.1.30 with `[DlssNr] ColourEncoding = 1` (forced Linear HDR) on a normal SDR game. Set
  Colour Encoding back to `auto`, or update.

### Red picture on an AMD card (game uses FSR, HDR on)
- Cause: the game told FSR its colour was not HDR while it was. Set the DLSS-NR input / Colour Encoding to Linear HDR.

### Hitches and a "fresh" look every few seconds with DLSS-NR (Watch Dogs: Legion)
- Log signs: many `DLSS-NR rebuilding surfaces: model format 27 -> 90` / `90 -> 27` lines (often one frame apart),
  each followed by `DLSS-NR model feature created`.
- Cause: the game switches its output between two colour layouts (RGBA8 and BGRA8) from frame to frame, and DLSS-NR
  rebuilt itself on every switch.
- Fixed in v0.1.33: the log says once `the game's output changes between format 27 and 90 (RGBA8/BGRA8); the model and its
  history are kept`. On older builds there is no workaround; update.
- A few `rebuilding surfaces` lines with other formats (for example `28 -> 10` at the main menu and at load) are normal:
  the game really changed its picture format.

### Higher input latency with DLSS-NR on
- Expected: the added latency is about the GPU time DLSS-NR takes per frame (two passes cost about twice one). Measure with
  `LogLevel = 2`: detailed logging adds latency of its own.

### Frame generation or DLSS-NR stops after pressing the backslash key
- The DLSS-NR on/off key is backslash by default. Turning NR off this way also drops Optical F5Low's
  "NR + frame generation" mode, so frame generation stops too. Press it again, or set another key with
  `[DlssNr] ToggleKey` in `OptiScaler.ini`. The log notes each press.

---

## AMD and Intel cards: the vendor-neutral port

The port is a separate optional download (`Optional-Vendor-Neutral-DLSS5-Forwarder.zip`). It writes its own log,
`nr_port.log`, next to its DLL.

### The port runs the main network (slow) instead of the small one
- Log signs (in `nr_port.log`): `network: main (71 blocks, ...)`. The intended setup shows
  `network: small (310.9 CC_SMALL, 31 blocks, ...)`.
- Cause: the port found an older `nvngx_dlssnr.dll` (310.8) instead of the 310.9 one. It searches OptiScaler's folder,
  then its subfolders, then the game's exe folder, and some games ship their own older copy in a subfolder (NBA 2K27:
  `data\streamline`).
- Fix: put the 310.9 `nvngx_dlssnr.dll` directly in the folder with OptiScaler and the game's exe.

---

## Frame generation, frame cap and low latency

### XeFG offers only 2x
- Log signs: `XeFG_Dx12::CreateSwapchain Max supported interpolations: 1`, and later
  `Requested interpolation count is higher than max supported, setting to max: 1`.
- Cause: XeFG reports its own maximum for the card and refuses more. Only an unlock plugin can raise it.
- On an NVIDIA GPU: FG Output F5Low (our own frame generation from the plugins folder) offers up to 6x;
  see docs/F5LOW-FRAME-GENERATION.md.

### F5Low is greyed out, or falls back to no frame generation
- Log signs: `F5Low: F5Low frame generation needs an NVIDIA GPU ...; FG Output set to None for this session` or
  `F5Low: f5low.dll is not in the plugins folder; FG Output set to None for this session`.
- Cause: F5Low's XeFG Tensor generator runs on NVIDIA tensor cores, and its library must be at
  `OptiScaler\plugins\f5low.dll` (the plugins folder of OptiScaler's folder; `[Plugins] Path` moves it). f5low.dll is in the release zip; if you removed it, put it back.
- `F5Low: libxess_fg.dll is missing`: F5Low builds its frame generator from Intel's libxess_fg.dll. The zip ships
  version 1.3.4.95 in OptiScaler's folder; keep it there.
- The Frame Generation page says "running on Intel's XeFG (up to 4x)": f5low.dll could not build its own generator
  (the hover text gives the reason). Only libxess_fg.dll 1.3.4.95 and 1.3.1.78 work; put the shipped 1.3.4.95 back.
- `F5Low could not start its frame generation` (toast): switch FG Output to XeFG and restart; the log has the reason.
- On every start f5low.dll builds its frame generator from libxess_fg.dll in memory, in the background (about half a
  second); frame generation starts by itself when it is ready. Nothing is written to disk. The Frame Generation page
  says "building its frame generator in the background" meanwhile; it should clear within about a second. If it stays,
  the status hover text and f5low.log say why. Old `f5low_cache` folders from earlier versions are removed by f5low
  itself. `f5low.log` goes to `%LOCALAPPDATA%\f5low` when the plugins folder is not writable.

### "Slow motion", or the frame rate far below the cap with XeFG 3x or 4x
- Expected: the frame cap counts all shown frames, real and generated. A cap of 120 at 4x means 30 rendered frames per
  second, which feels slow. Raise the cap or lower the multiplier.

### The frame cap is not followed with XeFG
- V-Sync on: XeFG then follows the screen's refresh rate. Since v0.1.31 OptiScaler's own limiter takes over the cap in that
  case. In RDR2 (Vulkan) turn the game's own V-Sync off.
- The GPU cannot reach the cap: check the real frame rate first.

### Stutter with the MFGUnlocker plugin, smooth with the old XeFGUnlock plugin
- Log signs: `LoadAsiPlugins Loaded: ...\MFGUnlocker.asi`.
- Cause: MFGUnlocker paces XeFG by itself, and OptiScaler also paces the same frames, so two timers fight. Measured in
  NBA 2K27: 103 fps and uneven frame times with its pacing, 119 fps and even frame times without.
- Fix: turn OptiScaler's pacing off (next entry), or turn MFGUnlocker's pacing off
  (`RTXMFG_XESS_OPTISCALER_PACING=0` in MFGUnlocker 1.5.0), or use the old XeFGUnlock plugin.

### How to turn off OptiScaler's own pacing
- Menu: Framerate page -> Low latency -> Input = **Game**. Keep an FPS limit if you want a cap; set it to 0 and press
  Apply Limit for no cap. Save INI writes `[Framerate] LowLatencyInput = game`. Works without a restart.
- Side effect: no low latency in games without their own Reflex.

### Low latency shows "[Off]" while "Active: Game -> XeLL" is shown
- Wording only (v0.1.32): the game's own Reflex drives XeFG's latency; the "[Off]" refers only to OptiScaler's own calls.
  Low latency works. Clearer wording comes in v0.1.33.

### The Witcher 3 does not start with frame generation input "dlssg" — Not solved yet
- Log signs: exceptions right after `sl.dlss_g` is ignored, then an access violation.
- Workaround: frame generation input "Upscaler", or `[FrameGen] StreamlineIgnoreOTA = false`.

### The game hangs ("device hung") seconds after start on an RTX 30 card, with the game's own DLSS frame generation
- Log signs: `nvngx_dlssg.dll found at ...\OptiScaler`, `DLSS-G interpolation state changed from disabled to enabled`,
  then `Wait on gpu fence timed out after 500ms` repeating, then `Unhandled exception 887A0006`.
- What we know: NVIDIA's DLSS frame generation is not supported on RTX 30 cards, but a `nvngx_dlssg.dll` placed in the
  OptiScaler folder (none ships in our zip) lets the game turn it on. In the one report, something also made the game see
  the card as a newer generation; we have not found what.
- Fix: remove `nvngx_dlssg.dll` from the OptiScaler folder, or turn DLSS Frame Generation off in the game. For frame
  generation on RTX 30 use OptiScaler's own frame generation with the FSR FG or XeFG output. Send the developers which
  unlock tools you use, if any.

---

## Games without their own upscaler (Optical F5Low)

Optical F5Low is OptiScaler's own motion estimate for games that make no DLSS, FSR or XeSS call. It is what lets DLSS-NR
and frame generation work in those games. Its modes are on the Optical F5Low page of the menu.

### Optical F5Low costs frames even with NR and frame generation unused
- Expected: in "NR + frame generation" mode the picture copy, motion estimate and virtual upscaler run every frame. Set
  the Optical F5Low mode to Off when you don't use it.

### "Frame count jumped too much" on every other frame, frame generation never generates — Not solved yet
- Log signs: `IFGFeature::StartNewFrame Frame count jumped too much! _frameCount: N, _lastDispatchedFrame: M` thousands of
  times, together with `Virtual upscaler: Evaluate N -> no pass` or `-> pass-through`. (A few of these lines right after
  frame generation starts are normal.)
- Seen: a DirectX 11 game on the native path with XeFG.
- Send the developers: whether the FPS overlay shows generated frames, and a log made with `LogLevel = 1`.

### Black blocky areas over the 3D scene (sky fine), frame generation on — Not solved yet
- No log sign known. Send the developers a short video, and tell them whether it goes away with frame generation off,
  with `[DlssNr] NativeMotion = false`, or with `[DlssNr] NativeDepthFinder = false` (one at a time).

### "No snapshot" warnings at start
- Log signs: `the depth finder has no snapshot for this frame (N frames running without one)`.
- Expected in menus before gameplay: no depth buffer qualifies yet, and Optical F5Low runs without depth until one does.
  Many emulators and some games never give one; motion then comes from the picture alone.

### Reuse detail between frames does nothing
- Expected while Compress screen edges is on: the two exclude each other. The line under the Reuse checkbox says why.

### DLSS-NR never starts in a Vulkan game, the menu says "Waiting for a finished picture."
- Cause: `[DlssNr] FinishedPicture = true` needs a DirectX 12 swapchain, which a Vulkan game doesn't have. Set
  `FinishedPicture = false` (the Optical F5Low "NR + frame generation" mode does this for you).

---

## HDR output (SDR games on an HDR display)

HDR output (`[HDR] Enabled = true`, or "HDR output" in the menu) shows an SDR game correctly inside an HDR signal: right
brightness and gamma, no washed-out look. It does not add brighter highlights the game never had. Turning it on or off,
and the signal (scRGB / HDR10), apply at the next game start or resolution / window change. The menu line under it says
whether it is active and, if not, why.

### HDR output says "standing aside"
- Log signs: `HDR output: standing aside, ...` (the rest of the line is the reason).
- `Windows HDR is off for this display`: turn on "Use HDR" in Windows display settings for the monitor the game is on.
- `the game already outputs HDR`: the game has its own HDR; use the game's HDR setting instead.
- `this game's swap chain is an older type that can only be changed when the game starts`: restart the game with HDR
  output and Windows HDR on.
- `the game uses exclusive full screen with an older swap chain type` or `the frame generation swap chain is in
  exclusive full screen`: switch the game to borderless or windowed (or, for the second one, turn frame generation's
  ForceBorderless option back on if you switched it off).
- `not available with DLSS-G (...)`: HDR output works with XeFG and FSR frame generation, not with DLSS-G (OptiScaler's
  DLSS-G output, or a game that uses DLSS-G itself). Use XeFG or FSR FG as the frame generation output.
- `not available for this swap chain while frame generation is on (works with XeFG and FSR FG)`: the game's swap chain
  is not one HDR output can convert under frame generation. Send the log to the developers.
- `not available with dxvk (Vulkan) games yet`: dxvk and Vulkan games are not supported yet.

### The window becomes borderless (no title bar) with HDR output and frame generation
- Log signs: `the frame generation library asked for exclusive full screen; its HDR swap chain is made windowed and kept
  borderless`, then `Frame generation chain kept borderless over the monitor`.
- Expected since v0.1.33: with frame generation, HDR output does not use exclusive full screen (it caused a missing menu
  and a crash on exit in testing). The window covers the monitor without borders, the game still sees full screen, and
  Alt+Enter is switched off for the HDR swap chain.

### Menu missing, or a crash on exit, with HDR output and XeFG (test builds before v0.1.33)
- Log signs: `CreateSwapChainForHwnd failed: 87A0001` right after `Game is creating fullscreen swapchain`, or
  `realExclusiveFullscreen: true` on every frame (debug log level), then an exception 0x87A on exit.
- Fixed in v0.1.33 (see the entry above). Update.

### Screenshots of HDR look too bright or washed out
- Not a problem with the game picture. Plain screenshot tools (Steam's F12, Print Screen into an image editor) save the
  HDR picture as SDR without converting it. Use Win+Alt+Print Screen (Xbox Game Bar): it saves a `.jxr` (the real HDR
  image) and a converted `.png` in `Videos\Captures`.

### Grey blacks or crushed shadows on the whole desktop
- Not OptiScaler. In the NVIDIA Control Panel ("Change resolution", "Use NVIDIA color settings"), Output dynamic range
  must match the display: Full for a PC monitor (and Full / High / Normal in the monitor's own menu), or Limited on both
  for a TV that expects it. A mismatch makes blacks grey (GPU Limited, display Full) or crushes shadows (the other way).

## Window, display and resolution

### The game no longer offers the monitor's resolution
- Windows reported no display modes for the monitor. Not OptiScaler: press Win+Ctrl+Shift+B (graphics driver reset) or
  replug the monitor.

### DirectX 11 game with XeFG crashes on fullscreen or a resolution change, or "device removed" after Alt+Enter
- Fixed in v0.1.32 (several causes; seen in Mad Max). On older builds: set the game to windowed or borderless in its own
  options, and turn DLSS-NR off while changing resolution.

### Error lines on Alt+Enter or resize with XeFG on a DirectX 11 game — Not solved yet (recovers by itself)
- Log signs: `SwapChainFlags changed from 42 to 0`, `Preventing flag change for XeFG!`,
  `XeFG Log: Native ResizeBuffers1 failed with 0x80070057`, then `the game's swapchain is now WxH`.
- Harmless if the game keeps running.

### The window keeps changing size at start with dxvk (two monitors)
- dxvk's own behaviour with monitors of different orientation, also without OptiScaler. Run without dxvk, or with only
  the main monitor on.

### The game stutters every few seconds while logging is on
- Fixed in v0.1.28. On any build, turn file logging off when you are not reporting a problem.

---

## What to send the developers

1. `OptiScaler.log` from the game folder, made at `LogLevel = 2` (see the top of this guide).
2. `OptiScaler_crash.dmp` if the game crashed (it is written next to the log). Since v0.1.32 it is only written with
   `[Log] CrashDump = true` in `OptiScaler.ini`: set it, make the crash happen again, then send the dump.
3. `OptiScaler.ini`.
4. Your GPU and driver version, and the list of files in `OptiScaler\plugins\`.
5. Whether dxvk, RTSS, ReShade or Special K are installed.
6. For picture problems: a screenshot or short video, and when it happens (menu, cutscene, gameplay, after Alt+Tab).
7. For the vendor-neutral port: `nr_port.log` as well.
