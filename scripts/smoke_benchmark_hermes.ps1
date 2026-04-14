param(
    [string]$RunId = "",
    [string]$ArtifactRoot = "artifacts"
)

$ErrorActionPreference = "Stop"

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptRoot

if ([string]::IsNullOrWhiteSpace($RunId)) {
    $RunId = "smoke-bench-hermes-" + (Get-Date -Format "yyyyMMdd-HHmmss")
}

$buildDir = Join-Path $repoRoot "build"
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

$benchExe = Join-Path $buildDir "hermes_bench_hermes_smoke.exe"
$daemonExe = Join-Path $buildDir "hermesd_bench_smoke.exe"
$replayExe = Join-Path $buildDir "hermes_replay_bench_smoke.exe"
$scenarioPath = Join-Path $buildDir ($RunId + "-observe.yaml")

Push-Location $repoRoot
try {
    $benchSources = @(
        "src/cli/hermes_bench.cpp",
        "src/runtime/scenario_config.cpp"
    )

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

    & g++ -std=c++17 -Iinclude @benchSources -o $benchExe
    if ($LASTEXITCODE -ne 0) {
        throw "g++ failed while building hermes_bench"
    }

    & g++ -std=c++17 -Iinclude @daemonSources -o $daemonExe
    if ($LASTEXITCODE -ne 0) {
        throw "g++ failed while building hermesd"
    }

    & g++ -std=c++17 -Iinclude @replaySources -o $replayExe
    if ($LASTEXITCODE -ne 0) {
        throw "g++ failed while building hermes_replay"
    }

    @"
name: smoke-observe-hermes
runtime_mode: observe-only
warmup_s: 0
measurement_s: 2
repeat_count: 1
ups_elevated_threshold: 40
ups_critical_threshold: 70
expected_max_oom_count: 0
expected_max_p95_latency_ms: 0
expected_min_job_completion_rate: 0
workloads:
  - name: quick_a
    command: powershell -NoProfile -Command "Start-Sleep -Seconds 1"
    foreground: false
    background: true
    duration_s: 3
  - name: quick_b
    command: powershell -NoProfile -Command "Start-Sleep -Seconds 1"
    foreground: false
    background: true
    duration_s: 3
  - name: foreground_echo
    command: powershell -NoProfile -Command "Write-Output ready"
    foreground: true
    background: false
    duration_s: 3
  - name: long_sleep
    command: powershell -NoProfile -Command "Start-Sleep -Seconds 5"
    foreground: false
    background: true
    duration_s: 5
"@ | Set-Content -Path $scenarioPath -NoNewline

    & $benchExe $scenarioPath --artifact-root $ArtifactRoot --run-id $RunId --hermes-bin $daemonExe --replay-bin $replayExe
    if ($LASTEXITCODE -ne 0) {
        throw "hermes_bench Hermes orchestration smoke failed"
    }

    $benchRoot = Join-Path $ArtifactRoot "bench"
    $summaryPath = Join-Path $benchRoot ($RunId + "-summary.json")
    $hermesRunId = $RunId + "-hermes"
    $hermesRunDir = Join-Path (Join-Path $ArtifactRoot "logs") $hermesRunId
    $hermesReplayJson = Join-Path $hermesRunDir "replay_summary.json"
    $hermesReplayCsv = Join-Path $hermesRunDir "summary.csv"

    foreach ($path in @($summaryPath, $hermesRunDir, $hermesReplayJson, $hermesReplayCsv)) {
        if (!(Test-Path $path)) {
            throw "Expected Hermes benchmark artifact was not written: $path"
        }
    }

    $summary = Get-Content -Path $summaryPath -Raw | ConvertFrom-Json
    if ($summary.run_id -ne $RunId) {
        throw "Benchmark summary has a mismatched run_id"
    }
    if ($summary.runtime_mode -ne "observe-only") {
        throw "Benchmark summary has an unexpected runtime_mode"
    }
    if ($summary.hermes.requested -ne $true) {
        throw "Benchmark summary did not record Hermes orchestration"
    }
    if ($summary.hermes.launch_ok -ne $true) {
        throw "Benchmark summary did not record a successful Hermes launch"
    }
    if ($summary.hermes.run_id -ne $hermesRunId) {
        throw "Benchmark summary has an unexpected Hermes run id"
    }
    if ($summary.hermes.replay_attempted -ne $true) {
        throw "Benchmark summary did not record Hermes replay"
    }
    if ($summary.hermes.replay_ok -ne $true) {
        throw "Benchmark summary did not record a successful Hermes replay"
    }
    if ($summary.replay_snapshot.available -ne $true) {
        throw "Benchmark summary did not embed replay snapshot data"
    }
    if ($summary.replay_snapshot.valid -ne $true) {
        throw "Embedded replay snapshot is invalid"
    }
    if ([int]$summary.replay_snapshot.samples -lt 1) {
        throw "Embedded replay snapshot did not record samples"
    }
    if ([int]$summary.replay_snapshot.decisions -lt 1) {
        throw "Embedded replay snapshot did not record decisions"
    }
    if ([int]$summary.replay_snapshot.actions -lt 1) {
        throw "Embedded replay snapshot did not record actions"
    }
    if ([int]$summary.launched -lt 4) {
        throw "Benchmark summary did not launch all workloads"
    }

    Write-Host "Benchmark Hermes smoke passed"
    Write-Host "Benchmark summary: $summaryPath"
    Write-Host "Hermes run directory: $hermesRunDir"
    Write-Host "Replay summary: $hermesReplayJson"
    Write-Host "Launched: $($summary.launched)"
    Write-Host "Jobs completed: $($summary.jobs_completed)"
    Write-Host "Replay samples: $($summary.replay_snapshot.samples)"
    Write-Host "Replay decisions: $($summary.replay_snapshot.decisions)"
    Write-Host "Replay actions: $($summary.replay_snapshot.actions)"
} finally {
    Pop-Location
}
