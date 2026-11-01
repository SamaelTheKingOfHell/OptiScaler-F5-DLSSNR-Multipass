# Real neural passes in isolated processes, followed by a cross-adapter return to the primary GPU.
param([string]$VcVars, [string]$CoreDll, [string]$WorkerExe, [Parameter(Mandatory=$true)][string]$ForwarderDll,
      [Parameter(Mandatory=$true)][string]$ModelDll, [ValidateRange(1,30)][int]$SecondaryPasses = 2,
      [ValidateRange(1,16384)][int]$Width = 256, [ValidateRange(1,16384)][int]$Height = 144,
      [switch]$NoPrimaryPass, [switch]$FailureTest, [switch]$ResetSecondFrame, [switch]$NoDepth,
      [switch]$HalfGuides, [switch]$Rgba8, [switch]$OffsetGuides, [switch]$BuildOnly)
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
    if (!$WorkerExe) {
        $WorkerExe = Join-Path $out 'nr_neural_worker.exe'
        $workerObj = Join-Path $out 'nr_neural_worker.obj'
        & cmd /d /c "`"$VcVars`" >nul && cl /nologo /std:c++17 /EHsc /W4 /WX OptiScaler/dlssnr/worker/WorkerMain.cpp /Fe:`"$WorkerExe`" /Fo:`"$workerObj`" /link d3d12.lib dxgi.lib user32.lib"
        if ($LASTEXITCODE -ne 0) { throw 'Neural worker compilation failed.' }
    } elseif (!(Test-Path -LiteralPath $WorkerExe -PathType Leaf)) { throw 'The specified worker executable does not exist.' }
    & cmd /d /c "`"$VcVars`" >nul && cl /nologo /std:c++17 /EHsc /W4 /WX tests/nr_gpu/nr_cross_process_neural.cpp /Fe:`"$exe`" /Fo:`"$obj`" /link d3d12.lib dxgi.lib user32.lib"
    if ($LASTEXITCODE -ne 0) { throw 'Neural cross-process probe compilation failed.' }
    if ($BuildOnly) { return }
    $probeArgs = @($CoreDll, $ForwarderDll, $ModelDll, $primaryData, $workerData, $log, $workerExe, $SecondaryPasses.ToString(), $Width.ToString(), $Height.ToString())
    if ($NoPrimaryPass) { $probeArgs += '--no-primary' }
    if ($FailureTest) { $probeArgs += '--failure-test' }
    if ($ResetSecondFrame) { $probeArgs += '--reset-second-frame' }
    if ($NoDepth) { $probeArgs += '--no-depth' }
    if ($HalfGuides) { $probeArgs += '--half-guides' }
    if ($Rgba8) { $probeArgs += '--rgba8' }
    if ($OffsetGuides) { $probeArgs += '--offset-guides' }
    if (Test-Path -LiteralPath $log) { Set-Content -LiteralPath $log -Value '' }
    & $exe @probeArgs
    if ($LASTEXITCODE -ne 0) {
        if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log }
        throw "Neural cross-process probe did not pass (exit code $LASTEXITCODE)."
    }
    if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log }
} finally { Pop-Location }
