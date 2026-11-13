# Plugins page

The **Plugins** page in the OptiScaler menu (after Misc) shows what the plugin loader did this session and lets you
switch plugins on or off. Every change applies at the **next start**: tick or untick, press Save Settings, restart the
game. A changed row says "restart to apply" until then.

## What is listed

- **Built-in**: the MFG unlock for XeFG, RTX 40 and RTX 20/30. It is part of OptiScaler, so it is listed even when the
  plugins folder is empty. It is **off by default**. Each family has a mode: Off, Detect (only where this GPU needs the
  unlock; XeFG has no GPU check, so Detect acts like On) or On. The row shows what the family actually did (active, stood aside and why, waiting for a provider).
- **Loaded from plugins folder**: every `*.asi` file in the plugins folder. Unticking a file lists its name in
  `[Plugins] DisabledAsi`; the file itself is never renamed or changed. Files with `-loadlate` in the name load after
  `[Plugins] LateAsiPluginsDelay` seconds and show "waiting" until then.
- **Loaders**: Special K, ReShade and the custom amdxc64 for RDNA 2, with whether they really loaded.

FG Output F5Low loads `f5low.dll` (with `f5low.ini`, if present) from the plugins folder (`OptiScaler\plugins\f5low.dll` in the release zip). It is
not an ASI plugin and is not listed here; it is in the release zip (see
[F5LOW-FRAME-GENERATION.md](F5LOW-FRAME-GENERATION.md)).

`Load ASI plugins` (`[Plugins] LoadAsiPlugins`) is the master switch at the top. With it off the folder is still
listed (greyed) so you can see what would load; turn it on to tick or untick files.

## What the states mean

| State | Meaning |
|---|---|
| loaded / loaded late | the file was opened and started |
| waiting | a `-loadlate` file, delay not over yet |
| disabled | named in `DisabledAsi`; the file was not opened |
| not loaded: ASI plugins are off | the master switch is off |
| failed to load (code) | Windows refused the file; `0x7E` module not found, `0xC1` not a valid 64-bit DLL, `0x2` file not found. A loader failure without a Windows code shows only "failed to load" |
| on, not loaded: a frame generation input is on | Special K is on in the ini but was not loaded this session because a frame generation input is selected (any FG input except DLSSG via Nvngx) |
| off | a built-in family or loader is switched off for this session |
| waiting for a provider | the built-in unlock is on, but the game has not loaded the frame generation file it patches yet |
| active: N of M patches | the built-in unlock patched the provider; N of M patches were found |
| stood aside: reason | the built-in unlock found a provider it does not recognise and left it untouched; the log has a `[MFGUnlock] ...` line with details |
| not needed | Detect mode: this GPU does not need the unlock |
| on, not loaded this session | a loader is on but was not needed or not reached (for example the custom amdxc64 on a GPU that is not RDNA 2) |

The log has the same list in one line at start, `[Plugins] N asi: name state; ...`, and a second line after `-loadlate` files have loaded. The log says `skipped` where the page says "not loaded: ASI plugins are off", and `failed 0x7E` without the explanation.

## Built-in MFG unlock and an external unlocker

Use one of them. If the built-in unlock is on and the "External frame generation / MFG unlocker" switch is on, or an
`.asi` with "unlock" in its name is loading, the page shows a warning. It does not pick a winner.

## ini keys

```
[Plugins]
LoadAsiPlugins=auto      ; master switch, default false
DisabledAsi=auto         ; comma separated file names, default none
[MFGUnlock]
XeFG=auto                ; auto/false = off, detect, true = on
RTX40=auto
RTX2030=auto
```
