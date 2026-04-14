param(
    [string]$RunId = "",
    [string]$ArtifactRoot = "artifacts"
)

$ErrorActionPreference = "Stop"

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptRoot

if ([string]::IsNullOrWhiteSpace($RunId)) {
    $RunId = "smoke-bench-plan-" + (Get-Date -Format "yyyyMMdd-HHmmss")
}

$buildDir = Join-Path $repoRoot "build"
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

$benchExe = Join-Path $buildDir "hermes_bench_smoke.exe"
$scenarioPath = Join-Path $buildDir ($RunId + "-baseline.yaml")

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

    & $benchExe --generate-baseline $scenarioPath
    if ($LASTEXITCODE -ne 0) {
        throw "hermes_bench failed to generate a baseline scenario"
    }
    if (!(Test-Path $scenarioPath)) {
        throw "Generated scenario config was not written: $scenarioPath"
    }

    & $benchExe $scenarioPath --dry-run --artifact-root $ArtifactRoot --run-id $RunId
    if ($LASTEXITCODE -ne 0) {
        throw "hermes_bench dry-run plan failed"
    }

    $benchRoot = Join-Path $ArtifactRoot "bench"
    $planPath = Join-Path $benchRoot ($RunId + "-plan.json")
    $snapshotPath = Join-Path $benchRoot ($RunId + "-scenario.yaml")

    if (!(Test-Path $planPath)) {
        throw "Benchmark plan artifact was not written: $planPath"
    }
    if (!(Test-Path $snapshotPath)) {
        throw "Benchmark scenario snapshot was not written: $snapshotPath"
    }

    $plan = Get-Content -Path $planPath -Raw | ConvertFrom-Json
    if ($plan.run_id -ne $RunId) {
        throw "Benchmark plan has a mismatched run_id"
    }
    if ($plan.scenario -ne "baseline-no-hermes") {
        throw "Benchmark plan has an unexpected scenario"
    }
    if ($plan.dry_run -ne $true) {
        throw "Benchmark plan did not record dry_run=true"
    }
    if ([int]$plan.workload_count -lt 4) {
        throw "Benchmark plan did not include the expected workload mix"
    }
    if ([int]$plan.foreground_workloads -lt 1) {
        throw "Benchmark plan did not include a foreground workload"
    }
    if ([int]$plan.background_workloads -lt 1) {
        throw "Benchmark plan did not include background workloads"
    }
    if ([int]$plan.planned_workload_seconds -le 0) {
        throw "Benchmark plan did not record planned workload seconds"
    }
    if ($plan.errors.Count -ne 0) {
        throw "Benchmark plan recorded validation errors"
    }

    Write-Host "Benchmark plan smoke passed"
    Write-Host "Scenario config: $scenarioPath"
    Write-Host "Plan artifact: $planPath"
    Write-Host "Scenario snapshot: $snapshotPath"
    Write-Host "Workloads: $($plan.workload_count)"
    Write-Host "Foreground workloads: $($plan.foreground_workloads)"
    Write-Host "Background workloads: $($plan.background_workloads)"
    Write-Host "Planned workload seconds: $($plan.planned_workload_seconds)"
} finally {
    Pop-Location
}
