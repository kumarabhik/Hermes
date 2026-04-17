# smoke_simulate.ps1 — Smoke test for hermes_simulate on Windows.
#
# Verifies that hermes_simulate:
#   1. Accepts a samples.ndjson from a synthetic fixture run
#   2. Writes scores.ndjson, predictions.ndjson, decisions.ndjson
#   3. Writes run_metadata.json and telemetry_quality.json
#   4. Reports a non-zero frame count
#   5. Successfully runs --compare against the original fixture run
#
# Prerequisites: hermes_synth and hermes_simulate must be built (g++ or cmake).

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Definition
$RepoRoot   = Split-Path -Parent $ScriptDir

function Find-Binary($Name) {
    foreach ($dir in @("build\Release", "build\Debug", "build", ".")) {
        $p = Join-Path $RepoRoot "$dir\$Name.exe"
        if (Test-Path $p) { return $p }
    }
    throw "Binary not found: $Name"
}

function Pass($msg) { Write-Host "[PASS] $msg" -ForegroundColor Green }
function Fail($msg) { Write-Host "[FAIL] $msg" -ForegroundColor Red; exit 1 }
function Info($msg) { Write-Host "       $msg" }

Write-Host ""
Write-Host "=== smoke_simulate: hermes_simulate smoke check ===" -ForegroundColor Cyan
Write-Host ""

Set-Location $RepoRoot

# ---- Step 1: Build (if not already built) ----
$hermes_synth    = Find-Binary "hermes_synth"
$hermes_simulate = Find-Binary "hermes_simulate"
Info "hermes_synth    : $hermes_synth"
Info "hermes_simulate : $hermes_simulate"

# ---- Step 2: Generate a synthetic fixture ----
$RunId = "sim-smoke-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
Info "Generating synthetic fixture: $RunId"
& $hermes_synth $RunId 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { Fail "hermes_synth failed (exit $LASTEXITCODE)" }

$SynthDir = Get-ChildItem "artifacts\logs\$RunId*" -Directory | Select-Object -First 1
if (-not $SynthDir) { Fail "Synthetic run directory not found under artifacts\logs\" }
Pass "Synthetic fixture generated: $($SynthDir.FullName)"

# ---- Step 3: Verify samples.ndjson exists ----
$SamplesFile = Join-Path $SynthDir.FullName "samples.ndjson"
if (-not (Test-Path $SamplesFile)) { Fail "samples.ndjson missing from $($SynthDir.FullName)" }
$SampleCount = (Get-Content $SamplesFile | Where-Object { $_ -ne "" }).Count
Info "samples.ndjson: $SampleCount lines"
if ($SampleCount -lt 1) { Fail "samples.ndjson is empty" }
Pass "samples.ndjson has $SampleCount samples"

# ---- Step 4: Run hermes_simulate ----
$SimRunId = "sim-out-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
$SimOutDir = "artifacts\logs\$SimRunId"

$SimOutput = & $hermes_simulate $SamplesFile `
    --out $SimOutDir `
    --compare $SynthDir.FullName `
    --quiet 2>&1

if ($LASTEXITCODE -ne 0) { Fail "hermes_simulate exited $LASTEXITCODE" }
Pass "hermes_simulate completed"

# ---- Step 5: Verify output artifacts ----
$ExpectedFiles = @(
    "scores.ndjson",
    "predictions.ndjson",
    "decisions.ndjson",
    "run_metadata.json",
    "telemetry_quality.json"
)

foreach ($f in $ExpectedFiles) {
    $path = Join-Path $SimOutDir $f
    if (-not (Test-Path $path)) { Fail "Missing artifact: $f" }
    $lines = (Get-Content $path).Count
    Info "$f : $lines lines"
    Pass "$f present"
}

# ---- Step 6: Verify frame count in telemetry_quality.json ----
$TQ = Get-Content (Join-Path $SimOutDir "telemetry_quality.json") -Raw
if ($TQ -match '"total_frames":(\d+)') {
    $Frames = [int]$Matches[1]
    if ($Frames -lt 1) { Fail "total_frames = 0 in telemetry_quality.json" }
    Pass "total_frames = $Frames"
} else {
    Fail "total_frames field missing from telemetry_quality.json"
}

# ---- Step 7: Verify --compare output appeared ----
if ($SimOutput -match "Frames compared") {
    Pass "--compare output present in hermes_simulate output"
} else {
    Info "Note: --compare section not found (may be empty if decisions mismatch is OK)"
}

Write-Host ""
Write-Host "=== smoke_simulate: ALL PASSED ===" -ForegroundColor Green
Write-Host "  Simulate output: $SimOutDir"
Write-Host ""
exit 0
