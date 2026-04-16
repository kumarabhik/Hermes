param(
    [string]$RunId = "",
    [string]$ArtifactRoot = "artifacts"
)

$ErrorActionPreference = "Stop"

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptRoot

if ([string]::IsNullOrWhiteSpace($RunId)) {
    $RunId = "smoke-compare-" + (Get-Date -Format "yyyyMMdd-HHmmss")
}

$buildDir = Join-Path $repoRoot "build"
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

# Executables compiled for this smoke run.
$benchExe   = Join-Path $buildDir "hermes_bench_compare_smoke.exe"
$compareExe = Join-Path $buildDir "hermes_compare_smoke.exe"
$daemonExe  = Join-Path $buildDir "hermesd_compare_smoke.exe"
$replayExe  = Join-Path $buildDir "hermes_replay_compare_smoke.exe"

$baselineScenario  = Join-Path $buildDir ($RunId + "-baseline.yaml")
$observeScenario   = Join-Path $buildDir ($RunId + "-observe.yaml")

Push-Location $repoRoot
try {
    # ------------------------------------------------------------------ build
    Write-Host "Building hermes_bench..."
    $benchSources = @("src/cli/hermes_bench.cpp", "src/runtime/scenario_config.cpp")
    & g++ -std=c++17 -O0 -I include -o $benchExe @benchSources
    if ($LASTEXITCODE -ne 0) { throw "hermes_bench build failed" }

    Write-Host "Building hermes_compare..."
    & g++ -std=c++17 -O0 -I include -o $compareExe src/cli/hermes_compare.cpp
    if ($LASTEXITCODE -ne 0) { throw "hermes_compare build failed" }

    Write-Host "Building hermesd..."
    $daemonSources = @(
        "src/monitor/cpu_psi.cpp",
        "src/monitor/mem_psi.cpp",
        "src/monitor/io_psi.cpp",
        "src/monitor/vmstat.cpp",
        "src/monitor/gpu_stats.cpp",
        "src/monitor/load_avg.cpp",
        "src/profiler/proc_stat.cpp",
        "src/profiler/process_mapper.cpp",
        "src/profiler/workload_classifier.cpp",
        "src/engine/pressure_score.cpp",
        "src/engine/predictor.cpp",
        "src/engine/scheduler.cpp",
        "src/actions/reprioritize.cpp",
        "src/actions/throttle.cpp",
        "src/actions/kill_action.cpp",
        "src/actions/dry_run_executor.cpp",
        "src/actions/active_executor.cpp",
        "src/runtime/event_logger.cpp",
        "src/runtime/run_metadata.cpp",
        "src/runtime/telemetry_quality.cpp",
        "src/runtime/scenario_config.cpp",
        "src/runtime/latency_probe.cpp",
        "src/runtime/control_socket.cpp",
        "src/cli/hermesd.cpp"
    )
    & g++ -std=c++17 -O0 -I include -o $daemonExe @daemonSources
    if ($LASTEXITCODE -ne 0) { throw "hermesd build failed" }

    Write-Host "Building hermes_replay..."
    $replaySources = @(
        "src/runtime/scenario_config.cpp",
        "src/cli/hermes_replay.cpp"
    )
    & g++ -std=c++17 -O0 -I include -o $replayExe @replaySources
    if ($LASTEXITCODE -ne 0) { throw "hermes_replay build failed" }

    # -------------------------------------------------- generate scenario configs
    Write-Host "Generating baseline scenario..."
    & $benchExe --generate-baseline $baselineScenario
    if ($LASTEXITCODE -ne 0) { throw "baseline scenario generation failed" }
    if (-not (Test-Path $baselineScenario)) { throw "baseline scenario file not written" }

    Write-Host "Patching baseline scenario for local smoke workloads..."
    $baselineContent = Get-Content $baselineScenario -Raw
    # Replace the linux stress-ng commands with local echo commands for smoke purposes.
    $baselineContent = $baselineContent `
        -replace 'command: "stress-ng[^"]*"', 'command: "echo baseline-workload"' `
        -replace 'command: "python3[^"]*"',   'command: "echo baseline-fg"'
    Set-Content $baselineScenario $baselineContent

    Write-Host "Generating observe-only scenario..."
    & $benchExe --generate-baseline $observeScenario
    if ($LASTEXITCODE -ne 0) { throw "observe scenario generation failed" }
    $observeContent = Get-Content $observeScenario -Raw
    $observeContent = $observeContent `
        -replace 'runtime_mode: baseline', 'runtime_mode: observe-only' `
        -replace 'command: "stress-ng[^"]*"', 'command: "echo observe-workload"' `
        -replace 'command: "python3[^"]*"',   'command: "echo observe-fg"'
    Set-Content $observeScenario $observeContent

    # -------------------------------------------------- run baseline
    $baselineRunId = $RunId + "-baseline"
    Write-Host "Running baseline benchmark (run-id: $baselineRunId)..."
    & $benchExe $baselineScenario `
        --artifact-root $ArtifactRoot `
        --run-id $baselineRunId
    if ($LASTEXITCODE -ne 0) { throw "baseline benchmark run failed" }

    $baselineSummary = Join-Path $ArtifactRoot "bench" ($baselineRunId + "-summary.json")
    if (-not (Test-Path $baselineSummary)) { throw "baseline summary artifact not written: $baselineSummary" }
    Write-Host "Baseline summary: $baselineSummary [OK]"

    # -------------------------------------------------- run observe-only
    $observeRunId = $RunId + "-observe"
    Write-Host "Running observe-only benchmark (run-id: $observeRunId)..."
    & $benchExe $observeScenario `
        --artifact-root $ArtifactRoot `
        --run-id $observeRunId `
        --hermes-bin $daemonExe `
        --replay-bin $replayExe
    if ($LASTEXITCODE -ne 0) { throw "observe benchmark run failed" }

    $observeSummary = Join-Path $ArtifactRoot "bench" ($observeRunId + "-summary.json")
    if (-not (Test-Path $observeSummary)) { throw "observe summary artifact not written: $observeSummary" }
    Write-Host "Observe summary : $observeSummary [OK]"

    # -------------------------------------------------- run hermes_compare
    $compareCsv = Join-Path $ArtifactRoot "bench" ($RunId + "-comparison.csv")
    Write-Host "Running hermes_compare..."
    & $compareExe `
        --bench-dir (Join-Path $ArtifactRoot "bench") `
        --output-csv $compareCsv
    if ($LASTEXITCODE -ne 0) { throw "hermes_compare failed" }

    if (-not (Test-Path $compareCsv)) { throw "comparison CSV not written: $compareCsv" }
    Write-Host "Comparison CSV  : $compareCsv [OK]"

    # -------------------------------------------------- verify CSV has both modes
    $csvContent = Get-Content $compareCsv -Raw
    if ($csvContent -notmatch "baseline") { throw "comparison CSV missing baseline row" }
    if ($csvContent -notmatch "observe-only") { throw "comparison CSV missing observe-only row" }
    Write-Host "CSV contains baseline and observe-only rows [OK]"

    # -------------------------------------------------- verify enriched fields in baseline summary
    $baselineJson = Get-Content $baselineSummary -Raw
    foreach ($field in @("completion_rate", "intervention_count", "oom_count", "degraded_behavior")) {
        if ($baselineJson -notmatch $field) { throw "baseline summary missing field: $field" }
    }
    Write-Host "Enriched summary fields present [OK]"

    Write-Host "`nsmoke_benchmark_compare PASSED (run-id: $RunId)"
} finally {
    Pop-Location
}
