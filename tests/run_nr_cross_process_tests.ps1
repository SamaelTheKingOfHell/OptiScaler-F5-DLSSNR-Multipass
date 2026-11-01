# Isolated child-process cross-adapter buffer round trip on the primary display GPU and an NVIDIA secondary.
param([string]$VcVars)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
if (!$VcVars) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($vs) { $VcVars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat' }
    }
}
if (!$VcVars -or !(Test-Path -LiteralPath $VcVars)) { throw 'MSVC x64 tools required; supply -VcVars.' }
$out = Join-Path $repo 'artifacts/validation/cross-process'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$exe = Join-Path $out 'nr_cross_process_roundtrip.exe'
$obj = Join-Path $out 'nr_cross_process_roundtrip.obj'
Push-Location $repo
try {
    & cmd /d /c "`"$VcVars`" >nul && cl /nologo /std:c++17 /EHsc /W4 /WX tests/nr_gpu/nr_cross_process_roundtrip.cpp /Fe:`"$exe`" /Fo:`"$obj`" /link d3d12.lib dxgi.lib user32.lib"
    if ($LASTEXITCODE -ne 0) { throw 'Cross-process probe compilation failed.' }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "Cross-process probe failed with exit code $LASTEXITCODE." }
    & $exe --failure-test
    if ($LASTEXITCODE -ne 0) { throw "Cross-process failure-path probe failed with exit code $LASTEXITCODE." }
    $waitExe = Join-Path $out 'nr_worker_fence_wait_test.exe'
    $waitObj = Join-Path $out 'nr_worker_fence_wait_test.obj'
    & cmd /d /c "`"$VcVars`" >nul && cl /nologo /std:c++17 /EHsc /W4 /WX tests/nr_gpu/nr_worker_fence_wait_test.cpp /Fe:`"$waitExe`" /Fo:`"$waitObj`" /link d3d12.lib dxgi.lib user32.lib"
    if ($LASTEXITCODE -ne 0) { throw 'Fence wait probe compilation failed.' }
    & $waitExe
    if ($LASTEXITCODE -ne 0) { throw "Fence wait probe failed with exit code $LASTEXITCODE." }
} finally { Pop-Location }
