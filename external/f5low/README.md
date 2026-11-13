# Local f5low kit folder (git-ignored)

`package_release.ps1` looks here for the F5Low kit (override with `-F5LowKit <folder or zip>`). Nothing in this
folder except this README is committed: no DLL of the kit is ever part of this repository's history.

The kit is `f5low-kit-<ver>.zip`, made by the f5low-fg repo (`tools/make_kit.ps1`; later also a release asset of
janblade/f5low-fg). Unpack it here. It holds:

| File | What |
| --- | --- |
| `f5low.dll` | F5Low frame generation (GPL-3.0). Staged as `OptiScaler\plugins\f5low.dll`. Contains no Intel code; builds its frame generator from the player's `libxess_fg.dll` on every start, in memory (nothing written to disk) |
| `libxess_fg.dll` | Intel XeSS-FG 1.3.4.95, unmodified, Intel-signed. Replaces the 1.3.1.78 in `OptiScaler\` |
| `LICENSE`, `XeSS_LICENSE.txt`, `README-KIT.txt` | licences and the source URL; staged under `Licenses\` |
| `kit_hashes.txt` | lines `<sha256>  <file>`; `f5low.dll` and `libxess_fg.dll` must be listed and are checked before packaging |

A release without a kit fails; `-NoF5Low` packages without F5Low (and keeps libxess_fg 1.3.1.78). A local MSBuild
output still copies 1.3.1.78 from `external\xess\bin`: only the release package uses the kit's 1.3.4.95.
f5low.dll works with `libxess_fg.dll` 1.3.4.95 and 1.3.1.78 only (`F5LowRules.h`).
