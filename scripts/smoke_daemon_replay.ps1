param(
    [string]$RunId = "",
    [string]$ArtifactRoot = "artifacts"
)

$ErrorActionPreference = "Stop"

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptRoot

if ([string]::IsNullOrWhiteSpace($RunId)) {
    $RunId = "smoke-daemon-" + (Get-Date -Format "yyyyMMdd-HHmmss")
}

$buildDir = Join-Path $repoRoot "build"
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

$daemonExe = Join-Path $buildDir "hermesd_smoke.exe"
$replayExe = Join-Path $buildDir "hermes_replay_daemon_smoke.exe"

$envNames = @(
    "HERMES_RUN_ID",
    "HERMES_SCENARIO",
    "HERMES_MAX_LOOPS",
    "HERMES_RUNTIME_MODE",
    "HERMES_ARTIFACT_ROOT"
)
$previousEnv = @{}
foreach ($name in $envNames) {
    $previousEnv[$name] = [Environment]::GetEnvironmentVariable($name, "Process")
}

function Restore-HermesSmokeEnvironment {
    foreach ($name in $envNames) {
        [Environment]::SetEnvironmentVariable($name, $previousEnv[$name], "Process")
    }
}

Push-Location $repoRoot
try {
    $daemonSources = @(
        "src/runtime/hermesd.cpp",
        "src/monitor/cpu_psi.cpp",
        "src/monitor/mem_psi.cpp",
        "src/monitor/loadavg.cpp",
        "src/monitor/gpu_stats.cpp",
        "src/monitor/io_psi.cpp",
        "src/monitor/vmstat.cpp",
        "src/profiler/proc_stat.cpp",
        "src/profiler/process_mapper.cpp",
        "src/profiler/workload_classifier.cpp",
        "src/engine/pressure_score.cpp",
        "src/engine/predictor.cpp",
        "src/engine/scheduler.cpp",
        "src/actions/dry_run_executor.cpp",
        "src/actions/reprioritize.cpp",
        "src/actions/throttle.cpp",
        "src/actions/kill.cpp",
        "src/actions/active_executor.cpp",
        "src/actions/cgroup.cpp",
        "src/runtime/event_logger.cpp",
        "src/runtime/run_metadata.cpp",
        "src/runtime/telemetry_quality.cpp"
    )

    $replaySources = @(
        "src/cli/hermes_replay.cpp",
        "src/replay/replay_summary.cpp",
        "src/runtime/event_logger.cpp"
    )

    & g++ -std=c++17 -Iinclude @daemonSources -o $daemonExe
    if ($LASTEXITCODE -ne 0) {
        throw "g++ failed while building hermesd"
    }

    & g++ -std=c++17 -Iinclude @replaySources -o $replayExe
    if ($LASTEXITCODE -ne 0) {
        throw "g++ failed while building hermes_replay"
    }

    $env:HERMES_RUN_ID = $RunId
    $env:HERMES_SCENARIO = "daemon-smoke"
    $env:HERMES_MAX_LOOPS = "1"
    $env:HERMES_RUNTIME_MODE = "observe-only"
    $env:HERMES_ARTIFACT_ROOT = $ArtifactRoot

    & $daemonExe
    if ($LASTEXITCODE -ne 0) {
        throw "hermesd smoke run failed"
    }

    $runDirectory = Join-Path (Join-Path $ArtifactRoot "logs") $RunId
    if (!(Test-Path $runDirectory)) {
        throw "Daemon run directory was not written: $runDirectory"
    }

    $requiredFiles = @(
        "run_metadata.json",
        "config_snapshot.yaml",
        "telemetry_quality.json",
        "samples.ndjson",
        "processes.ndjson",
        "scores.ndjson",
        "predictions.ndjson",
        "decisions.ndjson",
        "actions.ndjson",
        "events.ndjson"
    )
    foreach ($fileName in $requiredFiles) {
        $path = Join-Path $runDirectory $fileName
        if (!(Test-Path $path)) {
            throw "Expected daemon artifact was not written: $path"
        }
    }

    $nonEmptyFiles = @(
        "run_metadata.json",
        "config_snapshot.yaml",
        "telemetry_quality.json",
        "samples.ndjson",
        "scores.ndjson",
        "predictions.ndjson",
        "decisions.ndjson",
        "actions.ndjson",
        "events.ndjson"
    )
    foreach ($fileName in $nonEmptyFiles) {
        $path = Join-Path $runDirectory $fileName
        if ((Get-Item $path).Length -le 0) {
            throw "Expected daemon artifact was empty: $path"
        }
    }

    & $replayExe $runDirectory $ArtifactRoot
    if ($LASTEXITCODE -ne 0) {
        throw "hermes_replay failed for daemon smoke run"
    }

    $summaryPath = Join-Path $runDirectory "replay_summary.json"
    $runCsvPath = Join-Path $runDirectory "summary.csv"
    $artifactSummaryPath = Join-Path (Join-Path $ArtifactRoot "summaries") ($RunId + "-summary.json")
    $artifactCsvPath = Join-Path (Join-Path $ArtifactRoot "summaries") ($RunId + "-summary.csv")

    foreach ($path in @($summaryPath, $runCsvPath, $artifactSummaryPath, $artifactCsvPath)) {
        if (!(Test-Path $path)) {
            throw "Expected replay output was not written: $path"
        }
    }

    $summary = Get-Content -Path $summaryPath -Raw | ConvertFrom-Json
    $telemetry = Get-Content -Path (Join-Path $runDirectory "telemetry_quality.json") -Raw | ConvertFrom-Json
    $runCsvRow = Import-Csv -Path $runCsvPath | Select-Object -First 1
    $artifactCsvRow = Import-Csv -Path $artifactCsvPath | Select-Object -First 1

    if ($summary.valid -ne $true) {
        throw "Daemon replay summary is invalid"
    }
    if ($summary.run_id -ne $RunId) {
        throw "Daemon replay summary has a mismatched run_id"
    }
    if ([int]$summary.counts.samples -lt 1) {
        throw "Daemon replay summary did not count samples"
    }
    if ([int]$summary.counts.decisions -lt 1) {
        throw "Daemon replay summary did not count decisions"
    }
    if ([int]$summary.counts.actions -lt 1) {
        throw "Daemon replay summary did not count actions"
    }
    if ($summary.artifacts.run_metadata_present -ne $true) {
        throw "Daemon replay summary did not detect run_metadata.json"
    }
    if ($summary.artifacts.config_snapshot_present -ne $true) {
        throw "Daemon replay summary did not detect config_snapshot.yaml"
    }
    if ($summary.artifacts.telemetry_quality_present -ne $true) {
        throw "Daemon replay summary did not detect telemetry_quality.json"
    }
    if ([int]$telemetry.samples.total -lt 1) {
        throw "Telemetry quality did not record samples"
    }
    if ([int]$telemetry.control_loop.decisions -lt 1) {
        throw "Telemetry quality did not record decisions"
    }
    if ($runCsvRow.run_id -ne $RunId) {
        throw "Run summary CSV has a mismatched run_id"
    }
    if ($artifactCsvRow.run_id -ne $RunId) {
        throw "Artifact summary CSV has a mismatched run_id"
    }
    if ([string]$artifactCsvRow.valid -ne "true") {
        throw "Artifact summary CSV did not record a valid daemon run"
    }

    Write-Host "Daemon replay smoke passed"
    Write-Host "Run directory: $runDirectory"
    Write-Host "Summary JSON: $summaryPath"
    Write-Host "Summary CSV: $artifactCsvPath"
    Write-Host "Samples: $($summary.counts.samples)"
    Write-Host "Decisions: $($summary.counts.decisions)"
    Write-Host "Actions: $($summary.counts.actions)"
    Write-Host "Peak UPS: $($summary.peaks.ups)"
    Write-Host "Peak risk: $($summary.peaks.risk_score)"
} finally {
    Restore-HermesSmokeEnvironment
    Pop-Location
}
