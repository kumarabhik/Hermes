param(
    [string]$RunId = "",
    [string]$ArtifactRoot = "artifacts"
)

$ErrorActionPreference = "Stop"

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptRoot

if ([string]::IsNullOrWhiteSpace($RunId)) {
    $RunId = "smoke-bench-launch-" + (Get-Date -Format "yyyyMMdd-HHmmss")
}

$buildDir = Join-Path $repoRoot "build"
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

$benchExe = Join-Path $buildDir "hermes_bench_launch_smoke.exe"
$scenarioPath = Join-Path $buildDir ($RunId + "-launch.yaml")

Push-Location $repoRoot
try {
    $benchSources = @(
        "src/cli/hermes_bench.cpp",
        "src/runtime/scenario_config.cpp"
    )

    & g++ -std=c++17 -Iinclude @benchSources -o $benchExe
    if ($LASTEXITCODE -ne 0) {
        throw "g++ failed while building hermes_bench"
    }

    @"
name: smoke-launch
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

    if (!(Test-Path $scenarioPath)) {
        throw "Benchmark launch scenario was not written: $scenarioPath"
    }

    & $benchExe $scenarioPath --artifact-root $ArtifactRoot --run-id $RunId
    if ($LASTEXITCODE -ne 0) {
        throw "hermes_bench launch smoke failed"
    }

    $benchRoot = Join-Path $ArtifactRoot "bench"
    $planPath = Join-Path $benchRoot ($RunId + "-plan.json")
    $snapshotPath = Join-Path $benchRoot ($RunId + "-scenario.yaml")
    $summaryPath = Join-Path $benchRoot ($RunId + "-summary.json")

    foreach ($path in @($planPath, $snapshotPath, $summaryPath)) {
        if (!(Test-Path $path)) {
            throw "Expected benchmark artifact was not written: $path"
        }
    }

    $summary = Get-Content -Path $summaryPath -Raw | ConvertFrom-Json
    if ($summary.run_id -ne $RunId) {
        throw "Benchmark summary has a mismatched run_id"
    }
    if ($summary.scenario -ne "smoke-launch") {
        throw "Benchmark summary has an unexpected scenario"
    }
    if ([int]$summary.workload_count -ne 4) {
        throw "Benchmark summary has an unexpected workload_count"
    }
    if ([int]$summary.foreground_workloads -lt 1) {
        throw "Benchmark summary did not record a foreground workload"
    }
    if ([int]$summary.background_workloads -lt 1) {
        throw "Benchmark summary did not record background workloads"
    }
    if ([int]$summary.launched -lt 4) {
        throw "Benchmark summary did not launch all workloads"
    }
    if ([int]$summary.jobs_completed -lt 1) {
        throw "Benchmark summary did not record completed workloads"
    }
    if ([int]$summary.timed_out -lt 1) {
        throw "Benchmark summary did not record a timed out workload"
    }
    if ([int]$summary.still_running -ne 0) {
        throw "Benchmark summary still has running workloads"
    }
    if ([int]$summary.duration_ms -le 0) {
        throw "Benchmark summary did not record duration_ms"
    }

    Write-Host "Benchmark launch smoke passed"
    Write-Host "Scenario config: $scenarioPath"
    Write-Host "Plan artifact: $planPath"
    Write-Host "Summary artifact: $summaryPath"
    Write-Host "Launched: $($summary.launched)"
    Write-Host "Jobs completed: $($summary.jobs_completed)"
    Write-Host "Timed out: $($summary.timed_out)"
    Write-Host "Duration ms: $($summary.duration_ms)"
} finally {
    Pop-Location
}
