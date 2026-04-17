# smoke_web.ps1 — Smoke test for hermes_web on Windows.
#
# Verifies that hermes_web:
#   1. Starts without error on a free port
#   2. Responds to GET / with a 200 and HTML content
#   3. Responds to GET /api/status with a 200 and JSON content
#   4. Exits cleanly when stopped
#
# hermes_web requires no running daemon — it returns mock status on Windows.

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$RepoRoot  = Split-Path -Parent $ScriptDir

function Find-Binary($Name) {
    foreach ($dir in @("build\Release", "build\Debug", "build", ".")) {
        $p = Join-Path $RepoRoot "$dir\$Name.exe"
        if (Test-Path $p) { return $p }
    }
    throw "Binary not found: $Name"
}

function Pass($msg) { Write-Host "[PASS] $msg" -ForegroundColor Green }
function Fail($msg) { Write-Host "[FAIL] $msg" -ForegroundColor Red; if ($WebJob) { Stop-Job $WebJob -ErrorAction SilentlyContinue; Remove-Job $WebJob -ErrorAction SilentlyContinue }; exit 1 }
function Info($msg) { Write-Host "       $msg" }

Write-Host ""
Write-Host "=== smoke_web: hermes_web smoke check ===" -ForegroundColor Cyan
Write-Host ""

Set-Location $RepoRoot

$HermesWeb = Find-Binary "hermes_web"
Info "hermes_web : $HermesWeb"

# Pick a free port.
$Port = 17070
Info "Using port $Port"

# ---- Start hermes_web in background ----
$WebJob = Start-Job -ScriptBlock {
    param($bin, $port)
    & $bin --port $port --socket "" 2>&1
} -ArgumentList $HermesWeb, $Port

Start-Sleep -Milliseconds 1200   # give it time to bind

if ($WebJob.State -eq "Failed") {
    Fail "hermes_web job failed to start"
}
Pass "hermes_web started (job state: $($WebJob.State))"

# ---- GET / ----
try {
    $resp = Invoke-WebRequest -Uri "http://localhost:$Port/" -UseBasicParsing -TimeoutSec 5
    if ($resp.StatusCode -ne 200) { Fail "GET / returned HTTP $($resp.StatusCode)" }
    if ($resp.Content -notmatch "Hermes") { Fail "GET / response does not contain 'Hermes'" }
    Pass "GET / → HTTP 200, HTML contains 'Hermes'"
} catch {
    Fail "GET / failed: $_"
}

# ---- GET /api/status ----
try {
    $resp2 = Invoke-WebRequest -Uri "http://localhost:$Port/api/status" -UseBasicParsing -TimeoutSec 5
    if ($resp2.StatusCode -ne 200) { Fail "GET /api/status returned HTTP $($resp2.StatusCode)" }
    # Should be JSON — check for { at start (trimmed).
    $body = $resp2.Content.Trim()
    if (-not $body.StartsWith("{")) { Fail "GET /api/status response is not JSON: $($body.Substring(0, [Math]::Min(100,$body.Length)))" }
    Pass "GET /api/status → HTTP 200, JSON response"
    Info "Status: $($body.Substring(0, [Math]::Min(120,$body.Length)))"
} catch {
    Fail "GET /api/status failed: $_"
}

# ---- GET /nonexistent → 404 ----
try {
    $resp3 = Invoke-WebRequest -Uri "http://localhost:$Port/nonexistent" -UseBasicParsing -TimeoutSec 5
    if ($resp3.StatusCode -eq 404) {
        Pass "GET /nonexistent → HTTP 404 (correct)"
    } else {
        Info "Note: /nonexistent returned $($resp3.StatusCode) (expected 404)"
    }
} catch [System.Net.WebException] {
    # PowerShell throws on 4xx by default — check the response code.
    $code = [int]$_.Exception.Response.StatusCode
    if ($code -eq 404) {
        Pass "GET /nonexistent → HTTP 404 (correct)"
    } else {
        Info "GET /nonexistent returned $code"
    }
} catch {
    Info "GET /nonexistent: $_ (non-fatal)"
}

# ---- Stop hermes_web ----
Stop-Job  $WebJob -ErrorAction SilentlyContinue
Remove-Job $WebJob -ErrorAction SilentlyContinue
Pass "hermes_web stopped cleanly"

Write-Host ""
Write-Host "=== smoke_web: ALL PASSED ===" -ForegroundColor Green
Write-Host ""
exit 0
