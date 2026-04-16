param(
    [string]$RunId = "",
    [string]$ArtifactRoot = "artifacts"
)

$ErrorActionPreference = "Stop"

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptRoot

if ([string]::IsNullOrWhiteSpace($RunId)) {
    $RunId = "smoke-active-ctrl-" + (Get-Date -Format "yyyyMMdd-HHmmss")
}

$buildDir = Join-Path $repoRoot "build"
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

$benchExe  = Join-Path $buildDir "hermes_bench_active_smoke.exe"
$daemonExe = Join-Path $buildDir "hermesd_active_smoke.exe"
$replayExe = Join-Path $buildDir "hermes_replay_active_smoke.exe"
$scenarioPath = Join-Path $buildDir ($RunId + "-active.yaml")

Push-Location $repoRoot
try {
    # ---- Compile hermes_bench ----
    $benchSources = @(
        "src/cli/hermes_bench.cpp",
        "src/runtime/scenario_config.cpp",
        "src/runtime/latency_probe.cpp",
        "src/runtime/run_metadata.cpp",
        "src/runtime/telemetry_quality.cpp",
        "src/runtime/event_logger.cpp"
    )

    # ---- Compile hermesd ----
    $daemonSources = @(
        "src/runtime/hermesd.cpp",
        "src/monitor/cpu_psi.cpp",
        "src/monitor/mem_psi.cpp",
        "src/monitor/loadavg.cpp",
        "src/monitor/gpu_stats.cpp",
        "src/monitor/nvml_backend.cpp",
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
        "src/runtime/telemetry_quality.cpp",
        "src/runtime/scenario_config.cpp",
        "src/runtime/control_socket.cpp",
        "src/runtime/latency_probe.cpp"
    )

    # ---- Compile hermes_replay ----
    $replaySources = @(
        "src/cli/hermes_replay.cpp",
        "src/replay/replay_summary.cpp",
        "src/runtime/event_logger.cpp"
    )

    Write-Host "Building hermes_bench..."
    & g++ -std=c++17 -Iinclude @benchSources -o $benchExe
    if ($LASTEXITCODE -ne 0) { throw "g++ failed building hermes_bench" }

    Write-Host "Building hermesd (active-control smoke)..."
    & g++ -std=c++17 -Iinclude @daemonSources -o $daemonExe
    if ($LASTEXITCODE -ne 0) { throw "g++ failed building hermesd" }

    Write-Host "Building hermes_replay..."
    & g++ -std=c++17 -Iinclude @replaySources -o $replayExe
    if ($LASTEXITCODE -ne 0) { throw "g++ failed building hermes_replay" }

    # ---- Write a minimal active-control scenario (short for smoke) ----
    @"
name: smoke-active-control
runtime_mode: active-control
warmup_s: 0
measurement_s: 2
repeat_count: 1
ups_elevated_threshold: 40
ups_critical_threshold: 70
expected_max_oom_count: 0
expected_max_p95_latency_ms: 10000
expected_min_job_completion_rate: 0.5
workloads:
  - name: bg_pressure
    command: powershell -NoProfile -Command "Start-Sleep -Seconds 1"
    foreground: false
    background: true
    duration_s: 3
  - name: fg_job
    command: powershell -NoProfile -Command "Write-Output active-fg-done"
    foreground: true
    background: false
    duration_s: 3
"@ | Set-Content -Path $scenarioPath -NoNewline

    # ---- Launch bench in active-control mode ----
    # On Windows, active-control actions (setpriority, SIGSTOP, cgroup) are
    # compile-guarded and fall back to dry-run — execution path is still exercised.
    Write-Host "Running hermes_bench (active-control mode)..."
    & $benchExe $scenarioPath `
        --artifact-root $ArtifactRoot `
        --run-id $RunId `
        --hermes-bin $daemonExe `
        --replay-bin $replayExe
    if ($LASTEXITCODE -ne 0) { throw "hermes_bench active-control smoke failed" }

    # ---- Verify artifacts ----
    $benchRoot   = Join-Path $ArtifactRoot "bench"
    $summaryPath = Join-Path $benchRoot ($RunId + "-summary.json")

    if (!(Test-Path $summaryPath)) {
        throw "Expected benchmark summary not found: $summaryPath"
    }

    $summary = Get-Content -Path $summaryPath -Raw | ConvertFrom-Json

    if ($summary.run_id -ne $RunId) {
        throw "Benchmark summary run_id mismatch: got '$($summary.run_id)'"
    }
    if ($summary.runtime_mode -ne "active-control") {
        throw "Benchmark summary runtime_mode should be 'active-control', got '$($summary.runtime_mode)'"
    }
    if ($summary.hermes.requested -ne $true) {
        throw "Benchmark summary should record Hermes orchestration"
    }
    if ($summary.hermes.launch_ok -ne $true) {
        throw "Benchmark summary should record a successful Hermes launch"
    }

    # Verify new latency assertion fields are present (even if null for this short run).
    $hasPct = ($null -ne $summary.PSObject.Properties["p95_latency_ms"])
    $hasTarget = ($null -ne $summary.PSObject.Properties["latency_target_ms"])
    $hasMet = ($null -ne $summary.PSObject.Properties["latency_target_met"])
    if (!$hasPct -or !$hasTarget -or !$hasMet) {
        throw "Benchmark summary missing latency assertion fields (p95_latency_ms / latency_target_ms / latency_target_met)"
    }

    # Verify hermes run directory with event artifacts.
    $hermesRunId  = $RunId + "-hermes"
    $hermesRunDir = Join-Path (Join-Path $ArtifactRoot "logs") $hermesRunId
    if (!(Test-Path $hermesRunDir)) {
        throw "Hermes run directory not found: $hermesRunDir"
    }

    Write-Host ""
    Write-Host "Active-control smoke PASSED"
    Write-Host "  Summary        : $summaryPath"
    Write-Host "  Runtime mode   : $($summary.runtime_mode)"
    Write-Host "  Hermes run dir : $hermesRunDir"
    Write-Host "  Launched       : $($summary.launched)"
    Write-Host "  Completed      : $($summary.jobs_completed)"
    Write-Host "  OOM count      : $($summary.oom_count)"
    Write-Host "  p95 latency ms : $($summary.p95_latency_ms)"
    Write-Host "  Latency target : $($summary.latency_target_ms) ms"
    Write-Host "  Target met     : $($summary.latency_target_met)"
} finally {
    Pop-Location
}
