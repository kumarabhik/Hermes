# run_all_smoke.ps1 — Run every Hermes smoke check in sequence.
#
# Executes all PowerShell smoke scripts and reports a pass/fail/skip table.
# Each script is expected to exit 0 on success and non-zero on failure.
#
# Usage:
#   .\scripts\run_all_smoke.ps1
#   .\scripts\run_all_smoke.ps1 -StopOnFailure
#
# Options:
#   -StopOnFailure   Abort the suite at the first failing smoke check.

param(
    [switch]$StopOnFailure
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Continue"

$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Definition
$RepoRoot    = Split-Path -Parent $ScriptDir

$Smokes = @(
    @{ Name = "synthetic-replay";    Script = "smoke_synthetic_replay.ps1" },
    @{ Name = "daemon-replay";       Script = "smoke_daemon_replay.ps1" },
    @{ Name = "benchmark-plan";      Script = "smoke_benchmark_plan.ps1" },
    @{ Name = "benchmark-launch";    Script = "smoke_benchmark_launch.ps1" },
    @{ Name = "benchmark-hermes";    Script = "smoke_benchmark_hermes.ps1" },
    @{ Name = "benchmark-compare";   Script = "smoke_benchmark_compare.ps1" },
    @{ Name = "active-control";      Script = "smoke_active_control.ps1" },
    @{ Name = "simulate";            Script = "smoke_simulate.ps1" },
    @{ Name = "web-dashboard";       Script = "smoke_web.ps1" }
)

$Results = @()
$TotalStart = [DateTime]::UtcNow

Write-Host ""
Write-Host "=== Hermes Full Smoke Suite ===" -ForegroundColor Cyan
Write-Host "Repository : $RepoRoot"
Write-Host "Started    : $($TotalStart.ToString('yyyy-MM-dd HH:mm:ss')) UTC"
Write-Host ""

foreach ($Smoke in $Smokes) {
    $ScriptPath = Join-Path $ScriptDir $Smoke.Script
    if (-not (Test-Path $ScriptPath)) {
        Write-Host "  [SKIP] $($Smoke.Name) — script not found: $ScriptPath" -ForegroundColor Yellow
        $Results += [PSCustomObject]@{ Name = $Smoke.Name; Status = "SKIP"; DurationSec = 0 }
        continue
    }

    Write-Host "--- $($Smoke.Name) ---" -ForegroundColor White
    $Start = [DateTime]::UtcNow
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $ScriptPath
    $ExitCode = $LASTEXITCODE
    $Duration = ([DateTime]::UtcNow - $Start).TotalSeconds

    if ($ExitCode -eq 0) {
        Write-Host "  [PASS] $($Smoke.Name)  ($([math]::Round($Duration, 1))s)" -ForegroundColor Green
        $Results += [PSCustomObject]@{ Name = $Smoke.Name; Status = "PASS"; DurationSec = [math]::Round($Duration, 1) }
    } else {
        Write-Host "  [FAIL] $($Smoke.Name)  exit=$ExitCode  ($([math]::Round($Duration, 1))s)" -ForegroundColor Red
        $Results += [PSCustomObject]@{ Name = $Smoke.Name; Status = "FAIL"; DurationSec = [math]::Round($Duration, 1) }

        if ($StopOnFailure) {
            Write-Host ""
            Write-Host "Aborting suite: -StopOnFailure set and $($Smoke.Name) failed." -ForegroundColor Red
            break
        }
    }
    Write-Host ""
}

$TotalDuration = ([DateTime]::UtcNow - $TotalStart).TotalSeconds
$Passed  = ($Results | Where-Object { $_.Status -eq "PASS" }).Count
$Failed  = ($Results | Where-Object { $_.Status -eq "FAIL" }).Count
$Skipped = ($Results | Where-Object { $_.Status -eq "SKIP" }).Count

Write-Host "=== Smoke Suite Summary ===" -ForegroundColor Cyan
Write-Host ""
foreach ($R in $Results) {
    $Color = switch ($R.Status) {
        "PASS" { "Green"  }
        "FAIL" { "Red"    }
        "SKIP" { "Yellow" }
        default { "White" }
    }
    Write-Host ("  [{0}]  {1,-30}  {2,5}s" -f $R.Status, $R.Name, $R.DurationSec) -ForegroundColor $Color
}

Write-Host ""
Write-Host ("Passed: {0}   Failed: {1}   Skipped: {2}   Total: {3,0:F1}s" -f `
    $Passed, $Failed, $Skipped, $TotalDuration) -ForegroundColor Cyan
Write-Host ""

if ($Failed -gt 0) {
    Write-Host "SUITE FAILED" -ForegroundColor Red
    exit 1
} else {
    Write-Host "SUITE PASSED" -ForegroundColor Green
    exit 0
}
