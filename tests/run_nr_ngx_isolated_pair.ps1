# Two isolated processes hold real model features simultaneously on separate adapters.
# This tests runtime isolation only; it does not connect the game or transfer images between processes.
param([Parameter(Mandatory=$true)][string]$ForwarderDll, [Parameter(Mandatory=$true)][string]$ModelDll)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
if (!(Test-Path -LiteralPath $ForwarderDll -PathType Leaf) -or !(Test-Path -LiteralPath $ModelDll -PathType Leaf)) {
    throw 'Both model and forwarder paths must exist.'
}
& (Join-Path $PSScriptRoot 'run_nr_ngx_init_probe.ps1') -BuildOnly
$exe = Join-Path $repo 'artifacts/validation/ngx-core-probe/nr_ngx_init_probe.exe'
$coreDir = (Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Services\nvlddmkm\NGXCore' -Name NGXPath).NGXPath
$core = Join-Path $coreDir '_nvngx.dll'
$run = Join-Path $repo ('artifacts/validation/ngx-pair-' + [guid]::NewGuid().ToString('N'))
$primaryData = Join-Path $run 'primary'
$secondaryData = Join-Path $run 'secondary'
New-Item -ItemType Directory -Force -Path $primaryData, $secondaryData | Out-Null
$primaryReady = Join-Path $run 'primary.ready'
$secondaryReady = Join-Path $run 'secondary.ready'
$primaryOut = Join-Path $run 'primary.stdout'
$primaryErr = Join-Path $run 'primary.stderr'
$secondaryOut = Join-Path $run 'secondary.stdout'
$secondaryErr = Join-Path $run 'secondary.stderr'
function QuoteArg([string]$value) { return '"' + $value + '"' }
$primary = $null
$secondary = $null
try {
    $primaryArgs = @((QuoteArg $core), (QuoteArg $primaryData), (QuoteArg $ForwarderDll), (QuoteArg $ModelDll), '--primary-hold', (QuoteArg $primaryReady))
    $secondaryArgs = @((QuoteArg $core), (QuoteArg $secondaryData), (QuoteArg $ForwarderDll), (QuoteArg $ModelDll), '--secondary-hold', (QuoteArg $secondaryReady))
    $primary = Start-Process -FilePath $exe -ArgumentList $primaryArgs -WindowStyle Hidden -PassThru -RedirectStandardOutput $primaryOut -RedirectStandardError $primaryErr
    $secondary = Start-Process -FilePath $exe -ArgumentList $secondaryArgs -WindowStyle Hidden -PassThru -RedirectStandardOutput $secondaryOut -RedirectStandardError $secondaryErr
    $deadline = (Get-Date).AddSeconds(25)
    $ready = $false
    do {
        $primary.Refresh(); $secondary.Refresh()
        if ($primary.HasExited -or $secondary.HasExited) { break }
        if ((Test-Path -LiteralPath $primaryReady) -and (Test-Path -LiteralPath $secondaryReady)) { $ready = $true; break }
        Start-Sleep -Milliseconds 200
    } while ((Get-Date) -lt $deadline)
    if (!$ready) { throw 'Both isolated features were not simultaneously ready.' }
    [IO.File]::WriteAllText($primaryReady + '.go', '')
    [IO.File]::WriteAllText($secondaryReady + '.go', '')
    if (!$primary.WaitForExit(30000) -or !$secondary.WaitForExit(30000)) { throw 'An isolated probe timed out.' }
    $primary.Refresh(); $secondary.Refresh()
    Get-Content -LiteralPath $primaryOut, $primaryErr, $secondaryOut, $secondaryErr
    if ($primary.ExitCode -ne 0 -or $secondary.ExitCode -ne 0) { throw "Isolated probes failed: primary=$($primary.ExitCode), secondary=$($secondary.ExitCode)." }
    Write-Output 'Both model features were simultaneously live in isolated processes and each reevaluated and exited cleanly.'
} finally {
    foreach ($child in @($primary, $secondary)) {
        if ($null -ne $child) { $child.Refresh(); if (!$child.HasExited) { Stop-Process -Id $child.Id -Force -ErrorAction SilentlyContinue } }
    }
}
