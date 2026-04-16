# smoke_hermes_doctor.ps1 — Windows/PowerShell host readiness diagnostic for Hermes.
#
# Checks for the tools and artifacts needed to build and run Hermes smoke tests
# on a Windows authoring environment (no PSI, no Linux GPU required).
#
# Tier W checks (Windows smoke environment):
#   - g++ available on PATH (MinGW / MSYS2 / WSL2 g++)
#   - cmake available on PATH
#   - python3 available on PATH
#   - PowerShell version >= 5
#   - All Hermes binaries present in build\ (or build\Release\)
#   - Artifact directories present (artifacts\logs, artifacts\bench, artifacts\summaries)
#   - Key config files present (config\schema.yaml, config\baseline_scenario.yaml, etc.)
#   - Smoke scripts present
#
# Exits with the number of FAIL checks so CI can gate on it.
#
# Usage:
#   .\scripts\smoke_hermes_doctor.ps1
#   .\scripts\smoke_hermes_doctor.ps1 --no-color   (suppress ANSI colours)

param(
    [switch]$NoColor
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "SilentlyContinue"

# ---- Output helpers ----

function Write-Pass([string]$label, [string]$detail = "") {
    if ($NoColor) {
        Write-Host ("  [PASS] {0,-48} {1}" -f $label, $detail)
    } else {
        Write-Host ("  " + [char]27 + "[32m[PASS]" + [char]27 + "[0m {0,-48} {1}" -f $label, $detail)
    }
}

function Write-Warn([string]$label, [string]$detail = "") {
    if ($NoColor) {
        Write-Host ("  [WARN] {0,-48} {1}" -f $label, $detail)
    } else {
        Write-Host ("  " + [char]27 + "[33m[WARN]" + [char]27 + "[0m {0,-48} {1}" -f $label, $detail)
    }
}

function Write-Fail([string]$label, [string]$detail = "") {
    if ($NoColor) {
        Write-Host ("  [FAIL] {0,-48} {1}" -f $label, $detail)
    } else {
        Write-Host ("  " + [char]27 + "[31m[FAIL]" + [char]27 + "[0m {0,-48} {1}" -f $label, $detail)
    }
    $script:FailCount++
}

$script:FailCount = 0
$script:WarnCount = 0

# ---- Resolve repo root ----

$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Push-Location $RepoRoot

Write-Host ""
Write-Host ("=" * 70)
Write-Host "  Hermes Doctor — Windows smoke environment check"
Write-Host ("  Repo: $RepoRoot")
Write-Host ("=" * 70)
Write-Host ""

# ---- Section: Shell / runtime ----

Write-Host "--- Shell / Runtime ---"

# PowerShell version
$psv = $PSVersionTable.PSVersion
if ($psv.Major -ge 5) {
    Write-Pass "PowerShell version" "$($psv.Major).$($psv.Minor)"
} else {
    Write-Warn "PowerShell version" "$($psv.Major).$($psv.Minor) (>= 5 recommended)"
}

# ---- Section: Build tools ----

Write-Host ""
Write-Host "--- Build Tools ---"

# g++
$gppPath = Get-Command g++ -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
if ($gppPath) {
    $gppVer = (& g++ --version 2>&1 | Select-Object -First 1) -replace "`r|`n", ""
    Write-Pass "g++" $gppVer
} else {
    Write-Fail "g++" "not found on PATH — required to build Hermes smoke binaries"
}

# cmake
$cmakePath = Get-Command cmake -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
if ($cmakePath) {
    $cmakeVer = (& cmake --version 2>&1 | Select-Object -First 1) -replace "`r|`n", ""
    Write-Pass "cmake" $cmakeVer
} else {
    Write-Warn "cmake" "not found — needed for CMake-based builds (smoke scripts use g++ directly)"
}

# python3
$pyPath = Get-Command python3 -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
if (-not $pyPath) {
    $pyPath = Get-Command python -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
}
if ($pyPath) {
    $pyVer = (& python3 --version 2>&1 | Select-Object -First 1) -replace "`r|`n", ""
    if (-not $pyVer) { $pyVer = (& python --version 2>&1 | Select-Object -First 1) -replace "`r|`n", "" }
    Write-Pass "python3" $pyVer
} else {
    Write-Fail "python3" "not found — required for populate_readme_results.py, hermes_tune.py, check_evidence_tiers.py"
}

# ---- Section: Hermes binaries ----

Write-Host ""
Write-Host "--- Hermes Binaries ---"

$buildDirs  = @("build", "build\Release", "build\Debug")
$binaries   = @("hermesd", "hermes_replay", "hermes_synth", "hermes_bench",
                "hermes_compare", "hermes_eval", "hermes_fault", "hermes_report",
                "hermes_reeval", "hermesctl")
$binFound   = @{}

foreach ($bin in $binaries) {
    $found = $false
    foreach ($dir in $buildDirs) {
        $path = Join-Path $RepoRoot "$dir\$bin.exe"
        if (Test-Path $path) {
            $found = $true
            $binFound[$bin] = $path
            break
        }
        # Also check without .exe extension (WSL builds copied over)
        $pathNoExt = Join-Path $RepoRoot "$dir\$bin"
        if (Test-Path $pathNoExt) {
            $found = $true
            $binFound[$bin] = $pathNoExt
            break
        }
    }
    if ($found) {
        Write-Pass $bin $binFound[$bin]
    } else {
        Write-Warn $bin "not built yet — run: cd build && cmake .. && make"
    }
}

# ---- Section: Config files ----

Write-Host ""
Write-Host "--- Config Files ---"

$configs = @(
    "config\schema.yaml",
    "config\schema_tier_a.yaml",
    "config\schema_tier_b.yaml",
    "config\baseline_scenario.yaml",
    "config\observe_scenario.yaml",
    "config\oom_stress_scenario.yaml",
    "config\low_pressure_scenario.yaml"
)

foreach ($cfg in $configs) {
    $path = Join-Path $RepoRoot $cfg
    if (Test-Path $path) {
        Write-Pass $cfg
    } else {
        Write-Warn $cfg "missing — run hermes_bench --generate-* to create scenario configs"
    }
}

# ---- Section: Artifact directories ----

Write-Host ""
Write-Host "--- Artifact Directories ---"

$artDirs = @(
    "artifacts\logs",
    "artifacts\bench",
    "artifacts\summaries",
    "artifacts\replay",
    "artifacts\fault_injection"
)

foreach ($dir in $artDirs) {
    $path = Join-Path $RepoRoot $dir
    if (Test-Path $path) {
        $count = (Get-ChildItem $path -ErrorAction SilentlyContinue | Measure-Object).Count
        Write-Pass $dir "($count item(s))"
    } else {
        Write-Warn $dir "not yet created (will be created on first run)"
    }
}

# ---- Section: Smoke scripts ----

Write-Host ""
Write-Host "--- Smoke Scripts ---"

$smokeScripts = @(
    "scripts\smoke_synthetic_replay.ps1",
    "scripts\smoke_daemon_replay.ps1",
    "scripts\smoke_benchmark_plan.ps1",
    "scripts\smoke_benchmark_launch.ps1",
    "scripts\smoke_benchmark_hermes.ps1",
    "scripts\smoke_benchmark_compare.ps1",
    "scripts\smoke_active_control.ps1",
    "scripts\smoke_schema.sh",
    "scripts\run_all_smoke.ps1"
)

foreach ($script in $smokeScripts) {
    $path = Join-Path $RepoRoot $script
    if (Test-Path $path) {
        Write-Pass $script
    } else {
        Write-Warn $script "missing"
    }
}

# ---- Section: Python scripts ----

Write-Host ""
Write-Host "--- Python Scripts ---"

$pyScripts = @(
    "scripts\hermes_tune.py",
    "scripts\check_evidence_tiers.py",
    "scripts\populate_readme_results.py",
    "scripts\hermes_plot.py",
    "plots\hermes_plot.py"
)

foreach ($py in $pyScripts) {
    $path = Join-Path $RepoRoot $py
    if (Test-Path $path) {
        Write-Pass $py
    } else {
        Write-Warn $py "missing"
    }
}

# ---- Section: Documentation ----

Write-Host ""
Write-Host "--- Documentation ---"

$docs = @(
    "README.md",
    "design.md",
    "roadmap.md",
    "RESULTS.md",
    "docs\operator.md",
    "docs\internals.md",
    "docs\tuning_guide.md",
    "docs\calibration_guide.md",
    "docs\wsl2_quickstart.md"
)

foreach ($doc in $docs) {
    $path = Join-Path $RepoRoot $doc
    if (Test-Path $path) {
        Write-Pass $doc
    } else {
        Write-Warn $doc "missing"
    }
}

# ---- Summary ----

Write-Host ""
Write-Host ("=" * 70)

if ($script:FailCount -eq 0) {
    if ($NoColor) {
        Write-Host "  RESULT: All checks passed (0 FAIL).  Smoke environment is ready."
    } else {
        Write-Host ("  " + [char]27 + "[32mRESULT: All checks passed (0 FAIL).  Smoke environment is ready." + [char]27 + "[0m")
    }
    Write-Host ""
    Write-Host "  Next steps:"
    Write-Host "    .\scripts\run_all_smoke.ps1              # run all smoke scripts"
    Write-Host "    python3 scripts\check_evidence_tiers.py  # check T0-T5 status"
    Write-Host "    python3 scripts\populate_readme_results.py  # update README tables"
    Write-Host ""
    Write-Host "  To collect T1-T4 evidence: run bash scripts/smoke_phase6.sh on Linux/WSL2."
} else {
    if ($NoColor) {
        Write-Host ("  RESULT: $($script:FailCount) FAIL check(s) found.  Fix them before running smoke tests.")
    } else {
        Write-Host ("  " + [char]27 + "[31mRESULT: $($script:FailCount) FAIL check(s) found.  Fix them before running smoke tests." + [char]27 + "[0m")
    }
    Write-Host ""
    Write-Host "  Minimum required: g++ on PATH, python3 on PATH."
    Write-Host "  Install g++ via: winget install GnuWin32.Make  (or MSYS2/MinGW)"
}

Write-Host ("=" * 70)
Write-Host ""

Pop-Location
exit $script:FailCount
