# Isolated NGX initialization and optional feature creation; this does not open or alter a game.
param([string]$VcVars, [string]$CoreDll, [string]$ForwarderDll, [string]$ModelDll, [string]$SecondaryModelDll, [switch]$BothAdapters, [switch]$Repeat, [switch]$BuildOnly)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
if ($SecondaryModelDll -and (!$BothAdapters -or !(Test-Path -LiteralPath $SecondaryModelDll -PathType Leaf))) { throw '-SecondaryModelDll requires -BothAdapters and an existing model.' }
if ($BothAdapters -and $Repeat) { throw 'Choose -BothAdapters or -Repeat, not both.' }
if (($BothAdapters -or $Repeat) -and !$ForwarderDll) { throw '-BothAdapters requires -ForwarderDll and -ModelDll.' }
if (([bool]$ForwarderDll) -ne ([bool]$ModelDll)) { throw 'Supply both -ForwarderDll and -ModelDll, or neither.' }
if ($ForwarderDll -and (!(Test-Path -LiteralPath $ForwarderDll -PathType Leaf) -or !(Test-Path -LiteralPath $ModelDll -PathType Leaf))) {
    throw 'The specified forwarder or model does not exist.'
}
if (!$CoreDll) {
    $root = (Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Services\nvlddmkm\NGXCore' -Name NGXPath).NGXPath
    $CoreDll = Join-Path $root '_nvngx.dll'
}
if (!(Test-Path -LiteralPath $CoreDll -PathType Leaf)) { throw 'Specify a valid NGX core DLL using -CoreDll.' }
if (!$VcVars) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($vs) { $VcVars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat' }
    }
}
if (!$VcVars -or !(Test-Path -LiteralPath $VcVars)) { throw 'MSVC x64 tools required; supply -VcVars.' }
$out = Join-Path $repo 'artifacts/validation/ngx-core-probe'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$exe = Join-Path $out 'nr_ngx_init_probe.exe'
$obj = Join-Path $out 'nr_ngx_init_probe.obj'
Push-Location $repo
try {
    & cmd /d /c "`"$VcVars`" >nul && cl /nologo /std:c++17 /EHsc /W4 /WX tests/nr_gpu/nr_ngx_init_probe.cpp /Fe:`"$exe`" /Fo:`"$obj`" /link d3d12.lib dxgi.lib user32.lib"
    if ($LASTEXITCODE -ne 0) { throw 'NGX probe compilation failed.' }
    if ($BuildOnly) { return }
    $probeArgs = @($CoreDll, $out)
    if ($ForwarderDll) { $probeArgs += @($ForwarderDll, $ModelDll) }
    if ($BothAdapters) { $probeArgs += '--both'; if ($SecondaryModelDll) { $probeArgs += $SecondaryModelDll } }
    if ($Repeat) { $probeArgs += '--repeat' }
    & $exe @probeArgs
    if ($LASTEXITCODE -ne 0) { throw "NGX probe did not pass (exit code $LASTEXITCODE)." }
} finally { Pop-Location }
