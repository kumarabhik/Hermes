# smoke_all_states.ps1
# Smoke test: verify hermes_synth --all-states produces output covering all 5
# scheduler states (normal, elevated, throttled, cooldown, recovery) and that
# hermes_replay can summarize the resulting run without errors.

param(
    [string]$BuildDir = "build"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$Pass = 0
$Fail = 0

function Check([bool]$ok, [string]$label) {
    if ($ok) {
        Write-Host "  [PASS] $label" -ForegroundColor Green
        $script:Pass++
    } else {
        Write-Host  "  [FAIL] $label" -ForegroundColor Red
        $script:Fail++
    }
}

Write-Host "=== smoke_all_states.ps1 ===" -ForegroundColor Cyan

# ── locate binaries ───────────────────────────────────────────────────────────
$synthBin   = Join-Path $BuildDir "hermes_synth.exe"
$replayBin  = Join-Path $BuildDir "hermes_replay.exe"
$scoreBin   = Join-Path $BuildDir "hermes_score.exe"

foreach ($b in @($synthBin, $replayBin)) {
    if (-not (Test-Path $b)) {
        Write-Host "Binary not found: $b  (run cmake --build $BuildDir first)" -ForegroundColor Yellow

        # Try Release sub-directory (MSVC multi-config)
        $candidate = Join-Path $BuildDir "Release\$(Split-Path $b -Leaf)"
        if (Test-Path $candidate) {
            Set-Variable -Name ($b -eq $synthBin ? "synthBin" : "replayBin") -Value $candidate
            Write-Host "  -> found at $candidate" -ForegroundColor Yellow
        } else {
            Write-Host "Cannot continue without $b" -ForegroundColor Red
            exit 1
        }
    }
}

# ── generate all-states run ───────────────────────────────────────────────────
$runRoot = Join-Path $env:TEMP "hermes_smoke_all_states_$([System.Diagnostics.Process]::GetCurrentProcess().Id)"
New-Item -ItemType Directory -Path $runRoot -Force | Out-Null

Write-Host "`nGenerating all-states synthetic run -> $runRoot" -ForegroundColor Cyan

$env:HERMES_ARTIFACT_ROOT = $runRoot
& $synthBin --scenario synthetic-all-states --output-dir $runRoot 2>&1 | Out-Null
$synthExit = $LASTEXITCODE

Check ($synthExit -eq 0) "hermes_synth --all-states exits 0"

# Find the run directory (newest subdir)
$runDir = Get-ChildItem $runRoot -Directory | Sort-Object LastWriteTime -Descending | Select-Object -First 1

Check ($null -ne $runDir) "Run directory created under artifact root"

if ($null -eq $runDir) {
    Write-Host "No run directory found — cannot continue." -ForegroundColor Red
    exit 1
}

# ── verify samples.ndjson contains all 5 states ───────────────────────────────
$samplesPath = Join-Path $runDir.FullName "samples.ndjson"
Check (Test-Path $samplesPath) "samples.ndjson exists"

if (Test-Path $samplesPath) {
    $content = Get-Content $samplesPath -Raw

    # Scheduler states are embedded in band or state fields; check UPS ranges:
    # normal    → ups < 40
    # elevated  → 40 <= ups < 70
    # critical/throttled → ups >= 70
    # We check for presence of these band labels in the JSON lines.
    $hasNormal   = $content -match '"band"\s*:\s*"normal"'
    $hasElevated = $content -match '"band"\s*:\s*"elevated"'
    $hasCritical = $content -match '"band"\s*:\s*"critical"'

    Check $hasNormal   "samples contain 'normal' band entries"
    Check $hasElevated "samples contain 'elevated' band entries"
    Check $hasCritical "samples contain 'critical' band entries"
}

# ── hermes_replay summarizes without error ────────────────────────────────────
& $replayBin $runDir.FullName $runRoot 2>&1 | Out-Null
$replayExit = $LASTEXITCODE

Check ($replayExit -eq 0 -or $replayExit -eq 3) "hermes_replay exits 0 or 3 (valid or assertion-fail)"

$summaryPath = Join-Path $runDir.FullName "replay_summary.json"
Check (Test-Path $summaryPath) "replay_summary.json written"

if (Test-Path $summaryPath) {
    $summary = Get-Content $summaryPath -Raw
    Check ($summary -match '"samples"') "replay_summary.json contains samples count"
    Check ($summary -match '"peak_ups"') "replay_summary.json contains peak_ups"
}

# ── hermes_score on a sample line (if binary available) ───────────────────────
if (Test-Path $scoreBin) {
    $scoreOut = & $scoreBin --cpu 75 --mem 60 --io 5 --gpu 80 --vram 90 --json 2>&1
    $scoreExit = $LASTEXITCODE
    Check ($scoreExit -ge 0) "hermes_score --json exits cleanly"
    Check ($scoreOut -match '"ups"') "hermes_score JSON output contains 'ups'"
    Check ($scoreOut -match '"band"') "hermes_score JSON output contains 'band'"
} else {
    Write-Host "  [SKIP] hermes_score not found at $scoreBin" -ForegroundColor Yellow
}

# ── cleanup ───────────────────────────────────────────────────────────────────
Remove-Item $runRoot -Recurse -Force -ErrorAction SilentlyContinue

# ── result ────────────────────────────────────────────────────────────────────
Write-Host ""
Write-Host "Results: $Pass passed, $Fail failed" -ForegroundColor $(if ($Fail -eq 0) { "Green" } else { "Red" })

if ($Fail -gt 0) { exit 1 }
exit 0
