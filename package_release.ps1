# Assemble a release zip.
#
# The build output directory is not the release: it also holds import libraries, export files, debug
# symbols, and whatever earlier experiments left behind. Shipping that folder wholesale is how a
# release ends up containing a DLL nobody meant to publish, so this copies an explicit list and
# refuses anything not on it.
#
# What is deliberately NOT here: nvngx_dlssnr.dll. That is NVIDIA's, it is not ours to redistribute,
# and the user supplies their own copy per game folder. Only the ~108 KB forwarder ships.
#
# -PortBackendDll is the one exception to "only the forwarder ships": a vendor-neutral build of the
# same nvngx.dll_dlssnr.dll interface that runs the model itself (no NVIDIA NGX core needed, so it
# works on AMD/Intel). This script does not build it. Supplying the path is what opts it in; it never
# replaces the default forwarder at the package root, only adds it under Optional\.
#
# -F5LowKit is the f5low kit: a folder or a zip (then -F5LowKitSha256 is required), made by the f5low-fg repo
# (tools/make_kit.ps1, f5low-kit-<ver>.zip, also a release asset there). It holds f5low.dll, Intel's libxess_fg.dll
# 1.3.4.95, LICENSE, XeSS_LICENSE.txt, README-KIT.txt and kit_hashes.txt (lines "<sha256>  <file>"; f5low.dll and
# libxess_fg.dll must be listed and match). Default: the git-ignored folder external\f5low. F5Low ships by default: a
# release without a kit FAILS unless -NoF5Low is given. f5low.dll goes into OptiScaler\plugins\ (OptiScaler's default [Plugins] Path), where FG Output F5Low loads it
# by itself; it carries no Intel content and builds its frame generator on first use from the player's
# libxess_fg.dll (1.3.4.95 or 1.3.1.78). The package therefore ships the kit's libxess_fg.dll 1.3.4.95 as
# OptiScaler\libxess_fg.dll, replacing the 1.3.1.78 that a local MSBuild output still copies from external\xess\bin.
# No DLL of the kit is ever committed to this repository.

param(
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]*$')]
    [string]$Version = "v0.7.7",
    [switch]$SkipBuild,
    [switch]$IncludeDlssFrameGeneration,
    [switch]$AcceptNvidiaLicenses,
    [string]$HybridAssetsDirectory,
    [string]$StreamlineArchive,
    [string]$PortBackendDll,
    [string]$PortBackendIni,
    [string]$F5LowKit,
    [string]$F5LowKitSha256,
    [switch]$NoF5Low
)

$ErrorActionPreference = "Stop"

# The update notice compares the newest GitHub release against the version baked into the DLL (resource.h).
# A release whose tag does not match it makes every copy of that release announce an update to itself.
if ($PSBoundParameters.ContainsKey('Version') -and $Version -match '(\d+\.\d+\.\d+)') {
    $tagged = $Matches[1]
    $header = Get-Content -LiteralPath "$PSScriptRoot\OptiScaler\resource.h" -Raw
    $baked = foreach ($part in 'MAJOR', 'MINOR', 'HOTFIX') {
        if ($header -match "#define NR_RELEASE_${part}_VERSION\s+(\d+)") { $Matches[1] } else { throw "NR_RELEASE_${part}_VERSION not found in OptiScaler\resource.h" }
    }
    if (($baked -join '.') -ne $tagged) {
        throw "Version $Version does not match NR_RELEASE_*_VERSION in OptiScaler\resource.h ($($baked -join '.')). Bump resource.h first, or the release will tell its own users to update."
    }
}

# Derived rather than hardcoded, so this packages whichever checkout it is sitting in. There is more
# than one now -- the experiment runs in a git worktree beside the main tree, and a hardcoded root
# silently packages the other one's build output while reporting success.
$root = Split-Path -Parent $PSCommandPath
$flavour = if ($IncludeDlssFrameGeneration) { '-with-dlss-fg' } else { '' }
$stage = "$root\release\$Version$flavour"
$zip = "$root\release\OptiScaler-DLSSNR-F5-$Version$flavour.zip"
if ((Test-Path -LiteralPath $stage) -or (Test-Path -LiteralPath $zip)) {
    throw 'Release output already exists. Choose a new -Version; existing packages are never deleted or overwritten.'
}
if ($IncludeDlssFrameGeneration -and -not $AcceptNvidiaLicenses) {
    throw 'Bundling NVIDIA binaries requires -AcceptNvidiaLicenses. Read docs/DLSS-FRAME-GENERATION.md first.'
}
if ($IncludeDlssFrameGeneration) {
    Write-Warning 'LOCAL USE ONLY: this DLL-containing package has not been cleared for redistribution. Publish the downloader-only variant instead; see docs/DLSS-FRAME-GENERATION.md.'
}

# ---- f5low kit: resolve and validate BEFORE anything is built or staged ----------------------------------------------
# A refused kit must leave no release\<version> folder behind (the version would be burned) and cost no build.
# The libxess_fg.dll F5Low ships. f5low.dll builds its frame generator from the player's copy and knows only this build
# and 1.3.1.78. To bump it: put the new Intel file in the kit, then change BOTH values (version and SHA-256) here.
$f5lowIntelVersion = '1.3.4.95'
$f5lowIntelSha256 = '752B1491350A29FB2850FA4D0D14F62D79980E0B3CD067D147BB2469C1EC3182'

$kitData = $null
$kitPlugins = @()
$kitLicenses = [ordered]@{}
$kitIntelVersion = $f5lowIntelVersion
if ($NoF5Low -and $F5LowKit) { throw '-NoF5Low and -F5LowKit contradict each other' }
if (-not $F5LowKit -and -not $NoF5Low) { $F5LowKit = "$root\external\f5low" }
if ($F5LowKit) {
    if (-not (Test-Path -LiteralPath $F5LowKit)) {
        throw "REFUSING: no f5low kit at $F5LowKit. Put the kit (f5low-kit-<ver>.zip from f5low-fg, unpacked) there, pass -F5LowKit, or pass -NoF5Low to package without F5Low."
    }
    $isZip = Test-Path -LiteralPath $F5LowKit -PathType Leaf
    if ($isZip) {
        if ($F5LowKitSha256 -notmatch '^[0-9A-Fa-f]{64}$') { throw 'A kit zip needs -F5LowKitSha256 (64 hex digits)' }
        if ((Get-FileHash -Algorithm SHA256 -LiteralPath $F5LowKit).Hash -ne $F5LowKitSha256.ToUpperInvariant()) {
            throw 'REFUSING: the f5low kit zip does not match -F5LowKitSha256'
        }
    }
    elseif ($F5LowKitSha256) {
        throw '-F5LowKitSha256 belongs to a kit zip; a kit folder is checked through its kit_hashes.txt'
    }

    $kitTemp = $null
    try {
        $kitDir = $F5LowKit
        if ($isZip) {
            $kitTemp = Join-Path ([System.IO.Path]::GetTempPath()) ("f5lowkit_" + [guid]::NewGuid().ToString('N'))
            Expand-Archive -LiteralPath $F5LowKit -DestinationPath $kitTemp
            $dll = @(Get-ChildItem -LiteralPath $kitTemp -Recurse -File -Filter 'f5low.dll')
            if ($dll.Count -ne 1) { throw "REFUSING: expected one f5low.dll in the kit zip, found $($dll.Count)" }
            $kitDir = $dll[0].DirectoryName
        }

        foreach ($f in @('f5low.dll', 'libxess_fg.dll', 'LICENSE', 'XeSS_LICENSE.txt', 'zstd_LICENSE.txt', 'kit_hashes.txt')) {
            if (-not (Test-Path -LiteralPath (Join-Path $kitDir $f) -PathType Leaf)) {
                throw "REFUSING: the f5low kit is missing ${f}: $F5LowKit"
            }
        }

        # Every file the kit lists must match; the two DLLs and zstd's licence must be listed
        $listed = @{}
        foreach ($line in Get-Content -LiteralPath (Join-Path $kitDir 'kit_hashes.txt')) {
            if ($line -match '^\s*([0-9A-Fa-f]{64})\s+\*?(\S+)\s*$') { $listed[$Matches[2]] = $Matches[1].ToUpperInvariant() }
        }
        foreach ($f in @('f5low.dll', 'libxess_fg.dll', 'zstd_LICENSE.txt')) {
            if (-not $listed.ContainsKey($f)) { throw "REFUSING: kit_hashes.txt does not list $f" }
        }
        foreach ($f in $listed.Keys) {
            $path = Join-Path $kitDir $f
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "REFUSING: kit_hashes.txt lists $f, which is not in the kit" }
            if ((Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash -ne $listed[$f]) {
                throw "REFUSING: $f does not match kit_hashes.txt"
            }
        }

        # libxess_fg.dll: exactly the pinned build, and Intel's own unmodified file (its signature says so)
        $intel = Join-Path $kitDir 'libxess_fg.dll'
        if ((Get-FileHash -Algorithm SHA256 -LiteralPath $intel).Hash -ne $f5lowIntelSha256) {
            throw "REFUSING: the kit's libxess_fg.dll is not Intel's $f5lowIntelVersion ($f5lowIntelSha256)"
        }
        if ((Get-Item -LiteralPath $intel).VersionInfo.FileVersion -ne $f5lowIntelVersion) {
            throw "REFUSING: the kit's libxess_fg.dll does not report file version $f5lowIntelVersion"
        }
        $intelSignature = Get-AuthenticodeSignature -LiteralPath $intel
        if ($intelSignature.Status -ne 'Valid' -or $intelSignature.SignerCertificate.Subject -notmatch '(^|,\s*)O=Intel Corporation(,|$)') {
            throw "REFUSING: the kit's libxess_fg.dll is not validly signed by Intel ($($intelSignature.Status))"
        }

        # XeFG's API plus our own export, so Intel's library or an unrelated file under the wrong name is caught.
        $cudaText = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes((Join-Path $kitDir 'f5low.dll')))
        $cudaMissing = @(@('xefgSwapChainD3D12CreateContext', 'xefgCudaXellCountsRenderedOnly') | Where-Object { $cudaText.IndexOf($_) -lt 0 })
        if ($cudaMissing.Count -gt 0) {
            throw "REFUSING: f5low.dll is missing required exports: $($cudaMissing -join ', ')"
        }
        # Same rule as the port backend: no PDB path or local build path in a public binary.
        $cudaLeaks = @(@('.pdb', ('\Users\' + [Environment]::UserName + '\'), $root) |
            Where-Object { $cudaText.IndexOf($_, [StringComparison]::OrdinalIgnoreCase) -ge 0 })
        if ($cudaLeaks.Count -gt 0) {
            throw "REFUSING: f5low.dll appears to contain a local path or identifying string ($($cudaLeaks -join ', '))."
        }

        # Read what will be staged into memory, so the extracted copy can go as soon as this block ends
        $kitData = @{}
        $kitPlugins = @('f5low.dll')
        if (Test-Path -LiteralPath (Join-Path $kitDir 'f5low.ini') -PathType Leaf) { $kitPlugins += 'f5low.ini' }
        $names = @($kitPlugins) + @('libxess_fg.dll', 'LICENSE', 'XeSS_LICENSE.txt', 'README-KIT.txt')
        $names += @($listed.Keys | Where-Object { $_ -like '*_LICENSE.txt' -and $_ -ne 'XeSS_LICENSE.txt' })
        foreach ($f in ($names | Select-Object -Unique)) {
            $path = Join-Path $kitDir $f
            if (Test-Path -LiteralPath $path -PathType Leaf) { $kitData[$f] = [System.IO.File]::ReadAllBytes($path) }
        }
        $kitLicenses['f5low_LICENSE.txt'] = 'LICENSE'
        $kitLicenses['XeSS_LICENSE.txt'] = 'XeSS_LICENSE.txt'
        if ($kitData.ContainsKey('README-KIT.txt')) { $kitLicenses['f5low_README-KIT.txt'] = 'README-KIT.txt' }
        foreach ($f in ($kitData.Keys | Where-Object { $_ -like '*_LICENSE.txt' -and $_ -ne 'XeSS_LICENSE.txt' } | Sort-Object)) {
            $kitLicenses["f5low_$f"] = $f
        }
    }
    finally {
        if ($kitTemp -and (Test-Path -LiteralPath $kitTemp)) { Remove-Item -LiteralPath $kitTemp -Recurse -Force }
    }
}

if (-not $SkipBuild) {
    $msb = (Get-Command MSBuild.exe -ErrorAction SilentlyContinue).Source
    if (-not $msb) {
        $msb = @(
            "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe",
            "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe",
            "C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe",
            "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe"
        ) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    }
    if (-not $msb) {
        throw 'MSBuild.exe was not found. Install Visual Studio C++ build tools or use -SkipBuild with a verified existing build.'
    }

    foreach ($proj in @("$root\OptiScaler\dlssnr\forwarder\dlssnr_forwarder.vcxproj", "$root\OptiScaler.sln")) {
        $out = & $msb $proj /p:Configuration=Release /p:Platform=x64 /v:minimal /m 2>&1
        $err = $out | Select-String "error "
        if ($err) { Write-Host "FAILED: $proj"; $err | Select-Object -First 6; exit 1 }
    }
    Write-Host "built"
}

$src = "$root\x64\Release\a"

# The forwarder has two possible outputs: the shared folder when built through the solution, its own folder when its
# project is built alone. Take the newer one, so an old build left in either cannot ship. The export check below and the
# version check further down guard against a stale one too.
$forwarder = @("$root\OptiScaler\dlssnr\forwarder\x64\Release\a\nvngx.dll_dlssnr.dll", "$root\x64\Release\a\nvngx.dll_dlssnr.dll") |
    Where-Object { Test-Path -LiteralPath $_ } |
    Sort-Object { (Get-Item -LiteralPath $_).LastWriteTimeUtc } -Descending |
    Select-Object -First 1
if (-not $forwarder) { throw 'REFUSING: nvngx.dll_dlssnr.dll is not built' }
Write-Host "forwarder: $forwarder"

$exports = @("dlssnr_call_create", "dlssnr_call_evaluate_v2", "dlssnr_call_set_extras",
             "dlssnr_vk_probe", "dlssnr_vk_init", "dlssnr_vk_create", "dlssnr_vk_evaluate_v2", "dlssnr_vk_release")
$bytes = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($forwarder))
$missing = @($exports | Where-Object { $bytes.IndexOf($_) -lt 0 })

if ($missing.Count -gt 0) {
    Write-Host "STALE forwarder: missing $($missing -join ', ')"
    exit 1
}

# Same rule as OptiScaler.dll below: a PDB path names a private build directory.
if ($bytes.IndexOf('.pdb', [StringComparison]::OrdinalIgnoreCase) -ge 0) {
    throw 'REFUSING: the forwarder carries an embedded PDB path. Release sets GenerateDebugInformation=false.'
}

New-Item -ItemType Directory -Force -Path $stage | Out-Null

# Files, then folders. Anything not named here does not ship. Runtime files and user-facing
# instructions come from the checkout rather than the build directory so a stale post-build copy
# cannot put old GPU guidance or an old INI into a fresh package.
$buildFiles = @(
    "OptiScaler.dll",
    "!! EXTRACT ALL FILES TO GAME FOLDER !!"
)

$sourceFiles = @(
    "OptiScaler.ini",
    "setup_windows.bat",
    "setup_linux.sh",
    "get_streamline.ps1",
    "README.md",
    "INSTALL-DLSSNR.md",
    "TROUBLESHOOTING.md",
    "LICENSE"
)

foreach ($f in $buildFiles) {
    $source = "$src\$f"
    if (-not (Test-Path -LiteralPath $source)) {
        throw "Required build output is missing: $source"
    }
    Copy-Item -LiteralPath $source -Destination "$stage\$f" -Force
}

foreach ($f in $sourceFiles) {
    $source = "$root\$f"
    if (-not (Test-Path -LiteralPath $source)) {
        throw "Required release file is missing: $source"
    }
    Copy-Item -LiteralPath $source -Destination "$stage\$f" -Force
}

foreach ($d in @("Licenses", "OptiScaler")) {
    $source = "$src\$d"
    if (-not (Test-Path -LiteralPath $source -PathType Container)) {
        throw "Required dependency directory is missing: $source"
    }
    Copy-Item -LiteralPath $source -Destination "$stage\$d" -Recurse -Force
}

Copy-Item $forwarder "$stage\nvngx.dll_dlssnr.dll" -Force
Copy-Item -LiteralPath "$root\docs" -Destination "$stage\docs" -Recurse -Force
New-Item -ItemType Directory -Path "$stage\redist\streamline" -Force | Out-Null
Copy-Item -LiteralPath "$root\redist\streamline\manifest.json" -Destination "$stage\redist\streamline\manifest.json"

# An old test stack in the build directory must not silently enter a normal release. The optional
# full application package gets only the pinned official production files, with their own licences.
if (Test-Path -LiteralPath "$stage\OptiScaler\streamline") {
    throw 'REFUSING: the build output contains an unmanaged Streamline stack. Move it aside and use -IncludeDlssFrameGeneration.'
}
if ($IncludeDlssFrameGeneration) {
    & "$root\get_streamline.ps1" -Destination "$stage\OptiScaler\streamline" `
        -ArchivePath $StreamlineArchive -AcceptNvidiaLicenses
}

# No logging and no crash dumps by default: the ini ships LogToFile=auto, LogLevel=auto and CrashDump=auto (all off). A
# report needs LogToFile=true (and CrashDump=true for a crash); the install notes and the issue template say how.
$iniPath = "$stage\OptiScaler.ini"
$ini = Get-Content $iniPath -Raw
$check = Select-String -Path $iniPath -Pattern '^LogToFile=|^LogLevel=|^CrashDump=' | ForEach-Object { $_.Line }
Write-Host "log settings: $($check -join ', ')"
foreach ($line in $check) {
    if ($line -notmatch '=auto$') { throw "REFUSING: the release ini must ship logging and crash dumps off: $line" }
}

$targetProcess = Select-String -Path $iniPath -Pattern '^TargetProcessName=' | Select-Object -First 1
if ($targetProcess.Line -ne 'TargetProcessName=auto') {
    throw "REFUSING: portable package has a game-specific process filter: $($targetProcess.Line)"
}
Write-Host "process filter: portable (TargetProcessName=auto)"

# Belt and braces: nothing that is a build artifact, nothing from the abandoned warp work, no ASI plugin
# of any kind (the package ships OptiScaler.dll only, and plugin loading is off by default), and no research
# tracer or its captures (nrtrace*: hooks the CUDA driver and dumps modules next to itself) may survive into
# the zip regardless of how it got into the staging folder.
Get-ChildItem $stage -Recurse -Include *.exp, *.lib, *.pdb, *.ilk, *latewarp*, *.asi, *nrtrace* | Remove-Item -Recurse -Force

# No feature may ship switched on by accident.
#
# A global regex on "^Enabled=auto" once turned on five sections at once -- output scaling,
# sharpening, the magnifier and two more -- while trying to enable one, because the ini has six keys
# called Enabled in six different sections. That was in a test install rather than a release, and
# only because nothing was checking. This checks.
$on = Select-String -Path "$stage\OptiScaler.ini" -Pattern '^Enabled=true'

if ($on) {
    Write-Host "REFUSING: the packaged ini has features switched on:"
    $on | ForEach-Object { "  line $($_.LineNumber): $($_.Line)" }
    exit 1
}

Write-Host "ini verified: nothing switched on by default"

foreach ($key in @('FinishedPicture', 'DeferredDLSS', 'ResidualFG', 'ResidualFGApproxCamera', 'UnlockPasses')) {
    if ($ini -match "(?mi)^$key=true\s*$") {
        throw "REFUSING: experimental option $key is enabled in the portable package"
    }
}

# The proprietary runtime must never slip into a public artifact. Its two approved hashes are
# documentation/diagnostic inputs only; users obtain the GPU-appropriate file themselves.
if (Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object { $_.Name -ieq 'nvngx_dlssnr.dll' }) {
    throw 'REFUSING: proprietary nvngx_dlssnr.dll is present in the staging directory'
}
if (-not $IncludeDlssFrameGeneration) {
    $nvidiaRuntime = Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object {
        $_.Name -match '^(nvngx_dlss.*|sl\..*)\.dll$'
    }
    if ($nvidiaRuntime) { throw 'REFUSING: NVIDIA runtime DLLs found in the public downloader-only package' }
}

$crossGenHash = 'E67DEE209320CDAFE0E93E45675D7AA34323A53ACC57A72B2E40A181581C989A'
# The README links to the setup guide; exact runtime hashes belong in the guide and installer.
foreach ($requiredTextFile in @("$stage\INSTALL-DLSSNR.md", "$stage\setup_windows.bat")) {
    if ((Get-Content -LiteralPath $requiredTextFile -Raw).IndexOf($crossGenHash, [StringComparison]::OrdinalIgnoreCase) -lt 0) {
        throw "REFUSING: cross-generation runtime hash is missing from $requiredTextFile"
    }
}
Write-Host "cross-generation guidance: present and hash-pinned"

# A scanner verdict on this package is a recurring event, not a one-off: on 2026-10-01 a build was blocked
# as Trojan:Win32/Tecabans.ST!cl and the same bytes scanned clean about four hours later, corrected
# cloud-side with no local change. The guide section is how a user learns to check the hash and wait rather
# than switch protection off, so a release must not ship without it.
$avHeading = '## If antivirus flags the download'
if ((Get-Content -LiteralPath "$stage\INSTALL-DLSSNR.md" -Raw).IndexOf($avHeading, [StringComparison]::Ordinal) -lt 0) {
    throw "REFUSING: '$avHeading' is missing from $stage\INSTALL-DLSSNR.md"
}
Write-Host "antivirus guidance: present"

if ($HybridAssetsDirectory) {
    $manifest = Get-Content -LiteralPath (Join-Path $HybridAssetsDirectory 'asset-manifest.json') -Raw | ConvertFrom-Json
    foreach ($item in $manifest.files) {
        $assetRoot = [IO.Path]::GetFullPath((Join-Path $HybridAssetsDirectory 'OptiScaler/nvfp4/hybrid'))
        $source = [IO.Path]::GetFullPath((Join-Path $assetRoot $item.path))
        if (-not $source.StartsWith($assetRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid hybrid asset path' }
        if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne $item.sha256) { throw "Hybrid asset hash mismatch: $source" }
        $target = Join-Path "$stage/OptiScaler/nvfp4/hybrid" $item.path
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
        Copy-Item -LiteralPath $source -Destination $target
    }
}

if ($PortBackendDll) {
    if (-not (Test-Path -LiteralPath $PortBackendDll -PathType Leaf)) {
        throw "PortBackendDll not found: $PortBackendDll"
    }

    $portBytes = [System.IO.File]::ReadAllBytes($PortBackendDll)
    $portText = [System.Text.Encoding]::ASCII.GetString($portBytes)

    # Same export contract as the forwarder, so a stale or unrelated file is caught before it ships.
    $portExports = @("dlssnr_call_create", "dlssnr_call_evaluate_v2", "dlssnr_backend_id")
    $portMissing = @($portExports | Where-Object { $portText.IndexOf($_) -lt 0 })
    if ($portMissing.Count -gt 0) {
        throw "PortBackendDll is missing required exports: $($portMissing -join ', ')"
    }

    # A local dev-machine path or PDB reference baked into the binary would name a private build
    # environment in a public release. Match path context, not a bare substring -- the CRT's own
    # locale tables contain plain month names like "January", which a bare username check like
    # "Jan" would otherwise flag.
    $leakPatterns = @('.pdb', ('\Users\' + [Environment]::UserName + '\'), $root)
    $leaks = @($leakPatterns | Where-Object { $portText.IndexOf($_, [StringComparison]::OrdinalIgnoreCase) -ge 0 })
    if ($leaks.Count -gt 0) {
        throw "REFUSING: PortBackendDll appears to contain a local path or identifying string ($($leaks -join ', ')). Rebuild without embedding one, or strip it, before packaging."
    }

    # Own subfolder, never the package root: the default forwarder is what a normal user needs, and
    # this is an opt-in swap for GPUs it cannot run on.
    $optionalDir = "$stage\Optional"
    New-Item -ItemType Directory -Force -Path $optionalDir | Out-Null
    Copy-Item -LiteralPath $PortBackendDll -Destination "$optionalDir\nvngx.dll_dlssnr.dll" -Force

    # nr_port.ini configures the port runtime itself (fp16/dml/attn16/lin16/vit_every/mv_flip) and is read
    # from the same folder the DLL sits in. Optional because a bare DLL still runs -- on its slower,
    # unoptimised defaults -- but every build this project has actually shipped for testing has carried
    # a tuned one alongside it, so a release without it silently regresses whoever uses the port.
    $portIniShipped = $false
    if ($PortBackendIni) {
        if (-not (Test-Path -LiteralPath $PortBackendIni -PathType Leaf)) {
            throw "PortBackendIni not found: $PortBackendIni"
        }

        $iniText = Get-Content -LiteralPath $PortBackendIni -Raw
        $iniLeaks = @($leakPatterns | Where-Object { $iniText.IndexOf($_, [StringComparison]::OrdinalIgnoreCase) -ge 0 })
        if ($iniLeaks.Count -gt 0) {
            throw "REFUSING: PortBackendIni appears to contain a local path or identifying string ($($iniLeaks -join ', ')). Strip it before packaging."
        }

        Copy-Item -LiteralPath $PortBackendIni -Destination "$optionalDir\nr_port.ini" -Force
        $portIniShipped = $true
        Write-Host "vendor-neutral port backend: nr_port.ini staged alongside it"
    } else {
        Write-Warning "PortBackendDll supplied without -PortBackendIni: shipping the DLL on its unoptimised defaults."
    }

    $portReadme = @"
This is an alternate nvngx.dll_dlssnr.dll build. Unlike the one at the package root, it runs the
Neural Rendering model itself (D3D12 compute + DirectML) instead of going through the NVIDIA NGX
core, so it also works on AMD and Intel GPUs.

To use it: copy this file$(if ($portIniShipped) { " and nr_port.ini" }) over the nvngx.dll_dlssnr.dll in the game folder$(if ($portIniShipped) { "" }), replacing
the default one. nvngx_dlssnr.dll (the separate runtime file) is still required either way; see
INSTALL-DLSSNR.md.
$(if ($portIniShipped) { "`nnr_port.ini next to it carries the tuned settings this build was tested with (fp16/dml/attn16/lin16`nacceleration, vit_every=2). Without it the DLL still runs, just on slower unoptimised defaults.`n" } else { "" })
To confirm it loaded, open the Insert overlay's Neural Rendering menu: the status line reads
"Model backend: vendor-neutral port" instead of "Model backend: NVIDIA NGX".

Not validated on real AMD or Intel hardware.
"@
    Set-Content -LiteralPath "$optionalDir\README.txt" -Value $portReadme -Encoding utf8 -NoNewline
    Write-Host "vendor-neutral port backend: staged under Optional\ (not the default)"
}

if ($kitData) {
    # Kit checks ran at the top, before anything was built or staged; only the copying is left
    $pluginsDir = "$stage\OptiScaler\plugins"
    New-Item -ItemType Directory -Force -Path $pluginsDir | Out-Null
    foreach ($f in $kitPlugins) {
        [System.IO.File]::WriteAllBytes("$pluginsDir\$f", $kitData[$f])
    }
    [System.IO.File]::WriteAllBytes("$stage\OptiScaler\libxess_fg.dll", $kitData['libxess_fg.dll'])
    foreach ($entry in $kitLicenses.GetEnumerator()) {
        [System.IO.File]::WriteAllBytes("$stage\Licenses\$($entry.Key)", $kitData[$entry.Value])
    }
    # Self-check: OptiScaler loads f5low.dll from <OptiScaler folder>\plugins, never from a plugins folder at the zip root
    if (-not (Test-Path -LiteralPath "$stage\OptiScaler\plugins\f5low.dll" -PathType Leaf) -or
        (Test-Path -LiteralPath "$stage\plugins")) {
        throw 'REFUSING: f5low.dll must be staged as OptiScaler\plugins\f5low.dll (and no plugins folder at the package root)'
    }
    Write-Host "f5low kit: OptiScaler\plugins\ ($($kitPlugins -join ', ')), Intel library OptiScaler\libxess_fg.dll $kitIntelVersion, licences: $($kitLicenses.Keys -join ', ')"
}

# Frame generation ships off and F5Low on its default generator: OptiScaler\plugins\f5low.dll is only loaded when a player
# picks FG Output F5Low.
$packagedIni = Get-Content -LiteralPath "$stage\OptiScaler.ini" -Raw
foreach ($key in @('FGOutput', 'FrameGenerator')) {
    if ($packagedIni -notmatch "(?m)^$key=auto\r?$") {
        throw "REFUSING: the packaged ini must ship $key=auto"
    }
}

# The shipped DLL is unsigned, and a scanner's static model reads these properties. They are all correct
# today and cost nothing to keep that way; what this catches is a future build configuration quietly
# dropping one -- a debug build's PDB path, or ASLR turned off -- and adding an avoidable signal on top of
# the unavoidable ones. Checked on the staged copy, so it describes what actually ships.
$stagedDll = "$stage\OptiScaler.dll"
$peBytes = [System.IO.File]::ReadAllBytes($stagedDll)
$peHeader = [BitConverter]::ToUInt32($peBytes, 0x3c)
$dllCharacteristics = [BitConverter]::ToUInt16($peBytes, $peHeader + 4 + 20 + 70)

foreach ($flag in @{ 'HIGH_ENTROPY_VA' = 0x0020; 'DYNAMICBASE' = 0x0040; 'NX_COMPAT' = 0x0100 }.GetEnumerator()) {
    if (($dllCharacteristics -band $flag.Value) -eq 0) {
        throw ("REFUSING: staged OptiScaler.dll has {0} cleared (DllCharacteristics 0x{1:X4})" -f $flag.Key, $dllCharacteristics)
    }
}

# A PDB path names a private build directory and marks the binary as a debug build.
if ([System.Text.Encoding]::ASCII.GetString($peBytes).IndexOf('.pdb', [StringComparison]::OrdinalIgnoreCase) -ge 0) {
    throw 'REFUSING: staged OptiScaler.dll carries an embedded PDB path. Release sets GenerateDebugInformation=false.'
}

# Empty version-resource fields are themselves a signal, and this is what setup_windows.bat reads to
# recognise an existing install.
$versionInfo = (Get-Item -LiteralPath $stagedDll).VersionInfo
foreach ($field in 'CompanyName', 'FileDescription', 'ProductName', 'OriginalFilename', 'LegalCopyright', 'InternalName') {
    if ([string]::IsNullOrWhiteSpace($versionInfo.$field)) {
        throw "REFUSING: staged OptiScaler.dll has an empty $field in its version resource"
    }
}
# The forwarder carries the release number too, so a player can tell which release a game folder's copy came from.
$forwarderVersion = (Get-Item -LiteralPath "$stage\nvngx.dll_dlssnr.dll").VersionInfo.FileVersion
if ([string]::IsNullOrWhiteSpace($forwarderVersion)) {
    throw 'REFUSING: staged nvngx.dll_dlssnr.dll has no version in its version resource'
}
if ($forwarderVersion -ne $versionInfo.FileVersion) {
    throw "REFUSING: staged nvngx.dll_dlssnr.dll is version $forwarderVersion, OptiScaler.dll is $($versionInfo.FileVersion): a stale build"
}
Write-Host ("binary hygiene: DllCharacteristics 0x{0:X4}, version resource complete, no PDB path" -f $dllCharacteristics)

# Hash every shipped file after the staging tree is final. Use forward slashes so the list is easy
# to verify from PowerShell, 7-Zip, Linux, or Wine.
$checksumLines = Get-ChildItem -LiteralPath $stage -Recurse -File |
    Where-Object { $_.Name -ne 'SHA256SUMS.txt' } |
    Sort-Object FullName |
    ForEach-Object {
        $stageClean = $stage.TrimEnd('\', '/')
        $relative = if ($_.FullName.StartsWith($stageClean, [System.StringComparison]::OrdinalIgnoreCase)) {
            $_.FullName.Substring($stageClean.Length).TrimStart('\', '/').Replace('\', '/')
        } else {
            $_.Name
        }
        "{0} *{1}" -f (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash, $relative
    }
[IO.File]::WriteAllLines("$stage\SHA256SUMS.txt", $checksumLines, [Text.UTF8Encoding]::new($false))
Write-Host "checksums: $($checksumLines.Count) files"

Compress-Archive -Path "$stage\*" -DestinationPath $zip -CompressionLevel Optimal

Write-Host ""
Write-Host "staged at $stage"
Get-ChildItem $stage | ForEach-Object { "  {0,-42} {1,10:N0}" -f $_.Name, $_.Length }
Write-Host ""
Write-Host ("zip: {0}  ({1:N1} MB)" -f $zip, ((Get-Item $zip).Length / 1MB))
