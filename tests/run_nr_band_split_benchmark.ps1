# Per-pass neural cost on one NVIDIA adapter, for before/after comparisons (for example a tensor-rate change).
# Builds tests/nr_gpu/nr_band_split_benchmark.cpp if needed, then times each runtime at full and small band heights.
# -Runtimes is "label=path to nvngx_dlssnr.dll|preset", several separated by ';'. Results append to -Out.
#
#   .\tests\run_nr_band_split_benchmark.ps1 -Forwarder <nvngx.dll_dlssnr.dll> -Adapter 1 -Label before `
#       -Runtimes 'game-A=<game>\nvngx_dlssnr.dll|1;ampere-B=<game>\OptiScaler\NR-Models\ampere-small-v040\nvngx_dlssnr.dll|2'
param([Parameter(Mandatory = $true)][string]$Forwarder, [Parameter(Mandatory = $true)][string]$Runtimes,
      [int]$Adapter = 1, [int]$Width = 1280, [int]$Height = 720, [int]$Frames = 30,
      [int[]]$Bands = @(720, 368, 96), [string]$Label = 'run', [string]$Out)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$dir = Join-Path $repo 'artifacts/validation/band-split'
New-Item -ItemType Directory -Force -Path $dir, (Join-Path $dir 'data') | Out-Null
if (!$Out) { $Out = Join-Path $dir 'benchmark-history.txt' }
$exe = Join-Path $dir 'nr_band_split_benchmark.exe'
if (!(Test-Path -LiteralPath $exe)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
    Push-Location $repo
    try {
        & cmd /d /c "`"$vcvars`" >nul && cl /nologo /std:c++17 /EHsc /O2 /DDLSSNR_WORKER_TESTING tests\nr_gpu\nr_band_split_benchmark.cpp /Fe:`"$exe`" /Fo:`"$dir\nr_band_split_benchmark.obj`" /link d3d12.lib dxgi.lib user32.lib"
        if ($LASTEXITCODE -ne 0) { throw 'Benchmark compilation failed.' }
    } finally { Pop-Location }
}
$root = (Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Services\nvlddmkm\NGXCore' -Name NGXPath).NGXPath
$core = Join-Path $root '_nvngx.dll'
$heights = @($Bands | Where-Object { $_ -le $Height } | ForEach-Object { "$_" })
$stamp = (Get-Date).ToString('s')
foreach ($entry in ($Runtimes -split ';' | Where-Object { $_ })) {
    $name, $spec = $entry -split '=', 2
    $path, $preset = $spec -split '\|', 2
    # The probe reports progress on stderr; only its stdout result lines are kept.
    $ErrorActionPreference = 'Continue'
    $lines = & $exe $core $Forwarder $path (Join-Path $dir 'data') $Adapter $Width $Height $preset $Frames @heights 2>$null |
        Where-Object { $_ -match '^(band|Adapter)' }
    $ErrorActionPreference = 'Stop'
    foreach ($line in $lines) {
        $text = "$stamp [$Label] $name preset $preset :: $line"
        $text
        Add-Content -LiteralPath $Out -Value $text
    }
}
