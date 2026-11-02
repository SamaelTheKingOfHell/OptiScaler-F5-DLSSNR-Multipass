# A real two-adapter run is the default. -Warp is a mixed hardware/software fixture only.
param([string]$VcVars, [switch]$Warp, [switch]$ArgumentsOnly)
$ErrorActionPreference = 'Stop'
if ($Warp -and $ArgumentsOnly) { throw 'Choose only one test mode.' }
$repo = Split-Path $PSScriptRoot -Parent
if (!$VcVars) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($vs) { $VcVars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat' }
    }
}
if (!$VcVars -or !(Test-Path -LiteralPath $VcVars)) { throw 'MSVC x64 tools required; supply -VcVars.' }
$out = Join-Path $repo 'artifacts/validation/cross-adapter'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$exe = Join-Path $out 'nr_cross_adapter_roundtrip.exe'
$obj = Join-Path $out 'nr_cross_adapter_roundtrip.obj'
Push-Location $repo
try {
    $protocolExe = Join-Path $out 'nr_cross_adapter_protocol_smoke.exe'
    $protocolObj = Join-Path $out 'nr_cross_adapter_protocol_smoke.obj'
    & cmd /d /c "`"$VcVars`" >nul && cl /nologo /std:c++17 /EHsc /W4 /WX tests/nr_cross_adapter_protocol_smoke.cpp /Fe:`"$protocolExe`" /Fo:`"$protocolObj`""
    if ($LASTEXITCODE -ne 0) { throw 'Cross-adapter protocol test compilation failed.' }
    & $protocolExe
    if ($LASTEXITCODE -ne 0) { throw 'Cross-adapter protocol test failed.' }
    $telemetryExe = Join-Path $out 'nr_gpu_telemetry_smoke.exe'
    $telemetryObj = Join-Path $out 'nr_gpu_telemetry_smoke.obj'
    & cmd /d /c "`"$VcVars`" >nul && cl /nologo /std:c++17 /EHsc /W4 /WX tests/nr_gpu/nr_gpu_telemetry_smoke.cpp /Fe:`"$telemetryExe`" /Fo:`"$telemetryObj`" /link dxgi.lib"
    if ($LASTEXITCODE -ne 0) { throw 'GPU telemetry test compilation failed.' }
    & $telemetryExe
    if ($LASTEXITCODE -ne 0) { throw 'GPU telemetry test failed.' }
    & cmd /d /c "`"$VcVars`" >nul && cl /nologo /std:c++17 /EHsc /W4 /WX tests/nr_gpu/nr_cross_adapter_roundtrip.cpp /Fe:`"$exe`" /Fo:`"$obj`" /link d3d12.lib dxgi.lib user32.lib"
    if ($LASTEXITCODE -ne 0) { throw 'Cross-adapter probe compilation failed.' }
    $testArgs = @()
    if ($Warp) { $testArgs = @('--warp') }
    if ($ArgumentsOnly) { $testArgs = @('--arguments') }
    & $exe @testArgs
    if ($LASTEXITCODE -ne 0) { throw "Cross-adapter probe failed with exit code $LASTEXITCODE." }
} finally { Pop-Location }
