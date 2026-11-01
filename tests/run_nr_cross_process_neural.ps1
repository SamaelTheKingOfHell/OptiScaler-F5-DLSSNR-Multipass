# Real neural passes in isolated processes, followed by a cross-adapter return to the primary GPU.
param([string]$VcVars, [string]$CoreDll, [Parameter(Mandatory=$true)][string]$ForwarderDll,
      [Parameter(Mandatory=$true)][string]$ModelDll, [switch]$NoPrimaryPass, [switch]$BuildOnly)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
if (!$CoreDll) {
    $root = (Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Services\nvlddmkm\NGXCore' -Name NGXPath).NGXPath
    $CoreDll = Join-Path $root '_nvngx.dll'
}
foreach ($dll in @($CoreDll, $ForwarderDll, $ModelDll)) {
    if (!(Test-Path -LiteralPath $dll -PathType Leaf)) { throw "Missing DLL: $dll" }
}
if (!$VcVars) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($vs) { $VcVars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat' }
    }
}
if (!$VcVars -or !(Test-Path -LiteralPath $VcVars)) { throw 'MSVC x64 tools required; supply -VcVars.' }
$out = Join-Path $repo 'artifacts/validation/cross-process-neural'
$primaryData = Join-Path $out 'primary-data'
$workerData = Join-Path $out 'worker-data'
New-Item -ItemType Directory -Force -Path $out, $primaryData, $workerData | Out-Null
$exe = Join-Path $out 'nr_cross_process_neural.exe'
$obj = Join-Path $out 'nr_cross_process_neural.obj'
$log = Join-Path $out 'worker.log'
Push-Location $repo
try {
    & cmd /d /c "`"$VcVars`" >nul && cl /nologo /std:c++17 /EHsc /W4 /WX tests/nr_gpu/nr_cross_process_neural.cpp /Fe:`"$exe`" /Fo:`"$obj`" /link d3d12.lib dxgi.lib user32.lib"
    if ($LASTEXITCODE -ne 0) { throw 'Neural cross-process probe compilation failed.' }
    if ($BuildOnly) { return }
    $probeArgs = @($CoreDll, $ForwarderDll, $ModelDll, $primaryData, $workerData, $log)
    if ($NoPrimaryPass) { $probeArgs += '--no-primary' }
    & $exe @probeArgs
    if ($LASTEXITCODE -ne 0) {
        if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log }
        throw "Neural cross-process probe did not pass (exit code $LASTEXITCODE)."
    }
    if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log }
} finally { Pop-Location }
