# smoke_pack.ps1 — Smoke test for hermes_pack and hermes_replay --generate-manifest.
#
# 1. Builds hermes_synth, hermes_replay, hermes_pack, hermes_journal (if present).
# 2. Generates a synthetic run.
# 3. Runs hermes_pack and verifies bundle_manifest.json is created.
# 4. Runs hermes_replay --generate-manifest and verifies scenario_manifest.json.
# 5. Runs hermes_journal --stdout and verifies Markdown output contains key headers.
# 6. Runs hermes_pack --list on the same run (dry-run mode).

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$RepoRoot  = Split-Path -Parent $ScriptDir

function Find-Binary {
    param([string]$Name)
    foreach ($dir in @("$RepoRoot\build", "$RepoRoot")) {
        $p = Join-Path $dir "$Name.exe"
        if (Test-Path $p) { return $p }
        $p2 = Join-Path $dir $Name
        if (Test-Path $p2) { return $p2 }
    }
    return $null
}

function Compile-If-Missing {
    param([string]$Name, [string[]]$Sources, [string[]]$ExtraFlags = @())
    $bin = Find-Binary $Name
    if ($bin) { return $bin }

    Write-Host "  [build] Compiling $Name ..."
    $outPath = Join-Path $RepoRoot "$Name.exe"
    $srcPaths = $Sources | ForEach-Object { Join-Path $RepoRoot $_ }
    $includes  = "-I$RepoRoot\include"
    $flags     = @("-std=c++17", "-O2", $includes) + $ExtraFlags + $srcPaths + @("-o", $outPath)
    & g++ @flags
    if ($LASTEXITCODE -ne 0) { throw "Build failed for $Name" }
    return $outPath
}

Write-Host ""
Write-Host "=== smoke_pack: hermes_pack + generate-manifest + journal ===" -ForegroundColor Cyan

# ---- Build binaries ----
Write-Host ""
Write-Host "--- Build ---"

$SynthBin   = Compile-If-Missing "hermes_synth" @(
    "src/cli/hermes_synth.cpp",
    "src/engine/pressure_score.cpp","src/engine/predictor.cpp","src/engine/scheduler.cpp",
    "src/actions/dry_run_executor.cpp","src/actions/reprioritize.cpp","src/actions/throttle.cpp",
    "src/actions/kill.cpp","src/actions/active_executor.cpp","src/actions/cgroup.cpp",
    "src/monitor/cpu_psi.cpp","src/monitor/mem_psi.cpp","src/monitor/loadavg.cpp",
    "src/monitor/gpu_stats.cpp","src/monitor/nvml_backend.cpp","src/monitor/rich_proc_reader.cpp",
    "src/monitor/io_psi.cpp","src/monitor/vmstat.cpp",
    "src/profiler/proc_stat.cpp","src/profiler/process_mapper.cpp","src/profiler/workload_classifier.cpp",
    "src/runtime/event_logger.cpp","src/runtime/run_metadata.cpp","src/runtime/telemetry_quality.cpp",
    "src/runtime/scenario_config.cpp","src/runtime/control_socket.cpp","src/runtime/latency_probe.cpp",
    "src/replay/replay_summary.cpp"
)

$ReplayBin  = Compile-If-Missing "hermes_replay" @(
    "src/cli/hermes_replay.cpp",
    "src/engine/pressure_score.cpp","src/engine/predictor.cpp","src/engine/scheduler.cpp",
    "src/actions/dry_run_executor.cpp","src/actions/reprioritize.cpp","src/actions/throttle.cpp",
    "src/actions/kill.cpp","src/actions/active_executor.cpp","src/actions/cgroup.cpp",
    "src/monitor/cpu_psi.cpp","src/monitor/mem_psi.cpp","src/monitor/loadavg.cpp",
    "src/monitor/gpu_stats.cpp","src/monitor/nvml_backend.cpp","src/monitor/rich_proc_reader.cpp",
    "src/monitor/io_psi.cpp","src/monitor/vmstat.cpp",
    "src/profiler/proc_stat.cpp","src/profiler/process_mapper.cpp","src/profiler/workload_classifier.cpp",
    "src/runtime/event_logger.cpp","src/runtime/run_metadata.cpp","src/runtime/telemetry_quality.cpp",
    "src/runtime/scenario_config.cpp","src/runtime/control_socket.cpp","src/runtime/latency_probe.cpp",
    "src/replay/replay_summary.cpp"
)

$PackBin    = Compile-If-Missing "hermes_pack"    @("src/cli/hermes_pack.cpp")
$JournalBin = Compile-If-Missing "hermes_journal" @("src/cli/hermes_journal.cpp")

Write-Host "  [ok] All binaries ready" -ForegroundColor Green

# ---- Step 1: Generate a synthetic run ----
Write-Host ""
Write-Host "--- Step 1: Generate synthetic run ---"

$RunId   = "smoke-pack-$(Get-Date -Format 'yyyyMMddHHmmss')"
$RunDir  = Join-Path $RepoRoot "artifacts\logs\$RunId"
$env:HERMES_RUN_ID = $RunId

& $SynthBin $RunId 2>$null
if ($LASTEXITCODE -ne 0) { throw "hermes_synth failed" }

if (-not (Test-Path (Join-Path $RunDir "samples.ndjson"))) {
    throw "hermes_synth did not write samples.ndjson"
}
$SampleCount = (Get-Content (Join-Path $RunDir "samples.ndjson") | Measure-Object -Line).Lines
Write-Host "  [ok] hermes_synth wrote $SampleCount samples to $RunId" -ForegroundColor Green

# ---- Step 2: hermes_pack ----
Write-Host ""
Write-Host "--- Step 2: hermes_pack ---"

$BundleDir = Join-Path $RepoRoot "artifacts\evidence_bundles\$RunId"
& $PackBin $RunDir --output-dir $BundleDir 2>$null
if ($LASTEXITCODE -ne 0) { throw "hermes_pack failed" }

$ManifestPath = Join-Path $BundleDir "bundle_manifest.json"
if (-not (Test-Path $ManifestPath)) { throw "bundle_manifest.json not created" }
$ManifestJson = Get-Content $ManifestPath -Raw
if ($ManifestJson -notmatch '"run_id"') { throw "bundle_manifest.json missing run_id field" }
if ($ManifestJson -notmatch '"files_found"') { throw "bundle_manifest.json missing files_found" }
Write-Host "  [ok] bundle_manifest.json created with $(($ManifestJson | Select-String '"present": true' -AllMatches).Matches.Count) files" -ForegroundColor Green

# ---- Step 3: hermes_pack --list (dry-run) ----
Write-Host ""
Write-Host "--- Step 3: hermes_pack --list (dry-run) ---"

$ListOut = & $PackBin $RunDir --list 2>&1
if ($LASTEXITCODE -ne 0) { throw "hermes_pack --list failed" }
if ($ListOut -notmatch "Would bundle to") { throw "hermes_pack --list missing 'Would bundle to' line" }
Write-Host "  [ok] hermes_pack --list output verified" -ForegroundColor Green

# ---- Step 4: hermes_replay --generate-manifest ----
Write-Host ""
Write-Host "--- Step 4: hermes_replay --generate-manifest ---"

& $ReplayBin $RunDir --generate-manifest 2>$null
if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 3) {
    throw "hermes_replay --generate-manifest failed (exit $LASTEXITCODE)"
}

$ScenarioManifest = Join-Path $RunDir "scenario_manifest.json"
if (-not (Test-Path $ScenarioManifest)) { throw "scenario_manifest.json not created" }
$SmJson = Get-Content $ScenarioManifest -Raw
if ($SmJson -notmatch '"generated_by"') { throw "scenario_manifest.json missing generated_by field" }
if ($SmJson -notmatch '"min_peak_ups"') { throw "scenario_manifest.json missing min_peak_ups field" }
Write-Host "  [ok] scenario_manifest.json created" -ForegroundColor Green

# ---- Step 5: hermes_journal ----
Write-Host ""
Write-Host "--- Step 5: hermes_journal ---"

$JournalOut = & $JournalBin $RunDir --stdout 2>&1
if ($LASTEXITCODE -ne 0) { throw "hermes_journal failed (exit $LASTEXITCODE)" }
if ($JournalOut -notmatch "# Hermes Run Journal") { throw "hermes_journal output missing title" }
if ($JournalOut -notmatch "## Timeline")          { throw "hermes_journal output missing Timeline section" }
if ($JournalOut -notmatch "## Artifact Inventory"){ throw "hermes_journal output missing Artifact Inventory section" }
Write-Host "  [ok] hermes_journal --stdout output verified" -ForegroundColor Green

# Also write journal.md beside run.
& $JournalBin $RunDir 2>$null | Out-Null
if (Test-Path (Join-Path $RunDir "journal.md")) {
    Write-Host "  [ok] journal.md written to run directory" -ForegroundColor Green
}

# ---- Summary ----
Write-Host ""
Write-Host "SMOKE PASSED: hermes_pack + generate-manifest + hermes_journal" -ForegroundColor Green
Write-Host "  Bundle : $BundleDir"
Write-Host "  Manifest: $ScenarioManifest"
Write-Host "  Journal : $(Join-Path $RunDir 'journal.md')"
Write-Host ""
exit 0
