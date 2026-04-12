param(
    [string]$RunId = "",
    [string]$ArtifactRoot = "artifacts"
)

$ErrorActionPreference = "Stop"

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptRoot

if ([string]::IsNullOrWhiteSpace($RunId)) {
    $RunId = "smoke-synth-" + (Get-Date -Format "yyyyMMdd-HHmmss")
}

$buildDir = Join-Path $repoRoot "build"
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

$synthExe = Join-Path $buildDir "hermes_synth_smoke.exe"
$replayExe = Join-Path $buildDir "hermes_replay_smoke.exe"

Push-Location $repoRoot
try {
    $synthSources = @(
        "src/cli/hermes_synth.cpp",
        "src/runtime/event_logger.cpp",
        "src/runtime/run_metadata.cpp",
        "src/runtime/telemetry_quality.cpp",
        "src/actions/dry_run_executor.cpp",
        "src/engine/predictor.cpp",
        "src/engine/pressure_score.cpp",
        "src/engine/scheduler.cpp",
        "src/replay/replay_summary.cpp"
    )

    $replaySources = @(
        "src/cli/hermes_replay.cpp",
        "src/replay/replay_summary.cpp",
        "src/runtime/event_logger.cpp"
    )

    & g++ -std=c++17 -Iinclude @synthSources -o $synthExe
    if ($LASTEXITCODE -ne 0) {
        throw "g++ failed while building hermes_synth"
    }

    & g++ -std=c++17 -Iinclude @replaySources -o $replayExe
    if ($LASTEXITCODE -ne 0) {
        throw "g++ failed while building hermes_replay"
    }

    & $synthExe $RunId $ArtifactRoot
    if ($LASTEXITCODE -ne 0) {
        throw "hermes_synth failed"
    }

    $runDirectory = Join-Path (Join-Path $ArtifactRoot "logs") $RunId
    & $replayExe $runDirectory $ArtifactRoot
    if ($LASTEXITCODE -ne 0) {
        throw "hermes_replay failed"
    }

    $summaryPath = Join-Path $runDirectory "replay_summary.json"
    if (!(Test-Path $summaryPath)) {
        throw "Replay summary was not written: $summaryPath"
    }

    $summary = Get-Content -Path $summaryPath -Raw | ConvertFrom-Json
    if ($summary.valid -ne $true) {
        throw "Replay summary is invalid"
    }
    if ([int]$summary.manifest_assertions.checked -lt 1) {
        throw "No manifest assertions were checked"
    }
    if ([int]$summary.manifest_assertions.failed -ne 0) {
        throw "Manifest assertions failed"
    }
    if ([int]$summary.manifest_assertions.passed -ne [int]$summary.manifest_assertions.checked) {
        throw "Manifest assertion pass count does not match checked count"
    }

    Write-Host "Synthetic replay smoke passed"
    Write-Host "Run directory: $runDirectory"
    Write-Host "Assertions: $($summary.manifest_assertions.passed)/$($summary.manifest_assertions.checked)"
    Write-Host "Peak UPS: $($summary.peaks.ups)"
    Write-Host "Peak risk: $($summary.peaks.risk_score)"
} finally {
    Pop-Location
}
