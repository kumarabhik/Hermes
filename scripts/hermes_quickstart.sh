#!/usr/bin/env bash
# hermes_quickstart.sh — One-command Linux T1 evidence setup.
#
# Checks host readiness, builds Hermes with CMake, runs the smoke suite,
# and prints a T0/T1 readiness checklist.  Run this on a Linux host (WSL2 or
# native) to go from a fresh checkout to T0 evidence in one step.
#
# Usage:
#   bash scripts/hermes_quickstart.sh
#   bash scripts/hermes_quickstart.sh --skip-build   (if already built)
#   bash scripts/hermes_quickstart.sh --tier-b       (also check PSI + GPU)
#
# Exit codes:
#   0  All required checks passed (T0 ready)
#   1  One or more required checks failed

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
SKIP_BUILD=0
TIER_B=0

for arg in "$@"; do
    case "$arg" in
        --skip-build) SKIP_BUILD=1 ;;
        --tier-b)     TIER_B=1 ;;
        --help)
            echo "Usage: bash scripts/hermes_quickstart.sh [--skip-build] [--tier-b]"
            exit 0
            ;;
    esac
done

# ── Colour helpers ──────────────────────────────────────────────────────────
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

pass() { echo -e "  ${GREEN}[PASS]${NC} $1"; }
fail() { echo -e "  ${RED}[FAIL]${NC} $1"; FAILURES=$((FAILURES + 1)); }
warn() { echo -e "  ${YELLOW}[WARN]${NC} $1"; }
info() { echo -e "  ${CYAN}[INFO]${NC} $1"; }

FAILURES=0

echo ""
echo -e "${CYAN}=== Hermes Quickstart — T0/T1 Evidence Setup ===${NC}"
echo "Repository : $REPO_ROOT"
echo "Date       : $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
echo ""

# ── Step 1: Host prerequisites ───────────────────────────────────────────────
echo "--- Step 1: Host prerequisites ---"

if command -v g++ &>/dev/null; then
    pass "g++ found: $(g++ --version | head -1)"
else
    fail "g++ not found — install build-essential"
fi

if command -v cmake &>/dev/null; then
    pass "cmake found: $(cmake --version | head -1)"
else
    fail "cmake not found — install cmake"
fi

if command -v python3 &>/dev/null; then
    pass "python3 found: $(python3 --version)"
else
    warn "python3 not found — scripts/hermes_*.py will not run"
fi

echo ""

# ── Step 2: Build ────────────────────────────────────────────────────────────
echo "--- Step 2: Build ---"

if [[ $SKIP_BUILD -eq 1 ]]; then
    info "Skipping build (--skip-build)"
elif [[ -d "$BUILD_DIR" && -f "$BUILD_DIR/hermesd" ]]; then
    info "Build directory exists with hermesd — skipping rebuild (use --skip-build to suppress this)"
    pass "hermesd binary found at $BUILD_DIR/hermesd"
else
    info "Running cmake build in $BUILD_DIR ..."
    mkdir -p "$BUILD_DIR"
    (cd "$BUILD_DIR" && cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON 2>&1 | tail -5)
    (cd "$BUILD_DIR" && make -j"$(nproc)" 2>&1 | tail -10)

    if [[ -f "$BUILD_DIR/hermesd" ]]; then
        pass "hermesd built successfully"
    else
        fail "hermesd not found after build"
    fi
fi

# Verify key binaries.
for BIN in hermesd hermes_synth hermes_replay hermes_simulate hermes_bench hermes_pack; do
    if [[ -f "$BUILD_DIR/$BIN" ]]; then
        pass "$BIN binary present"
    else
        warn "$BIN binary not found in $BUILD_DIR"
    fi
done

echo ""

# ── Step 3: Synthetic smoke (T0) ─────────────────────────────────────────────
echo "--- Step 3: Synthetic smoke (T0 evidence) ---"

SYNTH_ID="quickstart-synth-$$"
"$BUILD_DIR/hermes_synth" "$SYNTH_ID" 2>/dev/null || true
SYNTH_DIR="$REPO_ROOT/artifacts/logs/$SYNTH_ID"

if [[ -f "$SYNTH_DIR/samples.ndjson" ]]; then
    SAMPLE_COUNT=$(wc -l < "$SYNTH_DIR/samples.ndjson")
    pass "hermes_synth wrote $SAMPLE_COUNT samples"
else
    fail "hermes_synth did not write samples.ndjson"
fi

if "$BUILD_DIR/hermes_replay" "$SYNTH_DIR" 2>/dev/null | grep -q "valid=true"; then
    pass "hermes_replay: synthetic run is valid"
else
    fail "hermes_replay: synthetic run reported invalid"
fi

echo ""

# ── Step 4: Daemon one-shot smoke (T0 evidence) ──────────────────────────────
echo "--- Step 4: Daemon one-shot smoke (T0 evidence) ---"

DAEMON_ID="quickstart-daemon-$$"
HERMES_RUN_ID="$DAEMON_ID" HERMES_SCENARIO=observe HERMES_MAX_LOOPS=1 \
    "$BUILD_DIR/hermesd" 2>/dev/null || true

DAEMON_DIR="$REPO_ROOT/artifacts/logs/$DAEMON_ID"

if [[ -f "$DAEMON_DIR/samples.ndjson" ]]; then
    pass "hermesd wrote samples.ndjson"
else
    fail "hermesd did not write samples.ndjson"
fi

for ARTIFACT in run_metadata.json config_snapshot.yaml telemetry_quality.json; do
    if [[ -f "$DAEMON_DIR/$ARTIFACT" ]]; then
        pass "$ARTIFACT present"
    else
        fail "$ARTIFACT missing from daemon run"
    fi
done

if "$BUILD_DIR/hermes_replay" "$DAEMON_DIR" 2>/dev/null | grep -q "samples="; then
    pass "hermes_replay processed daemon artifacts"
else
    warn "hermes_replay could not process daemon artifacts"
fi

echo ""

# ── Step 5: hermes_pack bundle ───────────────────────────────────────────────
echo "--- Step 5: Evidence bundle (hermes_pack) ---"

if [[ -f "$BUILD_DIR/hermes_pack" ]]; then
    "$BUILD_DIR/hermes_pack" "$SYNTH_DIR" --output-dir "$REPO_ROOT/artifacts/evidence_bundles/$SYNTH_ID" 2>/dev/null || true
    if [[ -f "$REPO_ROOT/artifacts/evidence_bundles/$SYNTH_ID/bundle_manifest.json" ]]; then
        pass "hermes_pack created evidence bundle with manifest"
    else
        warn "hermes_pack ran but manifest not found"
    fi
else
    warn "hermes_pack not built — skipping bundle step"
fi

echo ""

# ── Step 6: PSI availability (T1 gate) ───────────────────────────────────────
echo "--- Step 6: T1 gate — PSI availability ---"

if [[ -f /proc/pressure/cpu ]]; then
    CPU_PSI=$(cat /proc/pressure/cpu | head -1)
    pass "PSI available: /proc/pressure/cpu → $CPU_PSI"
    T1_PSI=1
else
    warn "PSI not available — kernel may be < 4.20 or CONFIG_PSI not set"
    warn "T1 evidence requires real PSI readings; observe-only mode will still work"
    T1_PSI=0
fi

if [[ -f /proc/pressure/memory ]]; then
    MEM_PSI=$(cat /proc/pressure/memory | head -1)
    pass "Memory PSI: $MEM_PSI"
else
    warn "/proc/pressure/memory not found"
fi

if [[ -f /proc/pressure/io ]]; then
    pass "/proc/pressure/io available"
else
    warn "/proc/pressure/io not found"
fi

echo ""

# ── Step 7: GPU / NVML (Tier C gate) ─────────────────────────────────────────
if [[ $TIER_B -eq 1 ]]; then
    echo "--- Step 7: Tier B/C gate — GPU/NVML ---"

    if [[ -f "$BUILD_DIR/hermesctl" ]]; then
        if "$BUILD_DIR/hermesctl" nvml 2>/dev/null | grep -q "Device 0"; then
            pass "NVML fast path active — GPU detected"
        else
            warn "NVML library not found or no GPU detected"
            warn "GPU metrics will fall back to nvidia-smi subprocess (slower)"
        fi
    else
        warn "hermesctl not built — skipping NVML check"
    fi

    if command -v nvidia-smi &>/dev/null; then
        pass "nvidia-smi fallback available"
    else
        warn "nvidia-smi not in PATH — GPU stats unavailable without NVML"
    fi

    echo ""
fi

# ── Summary ───────────────────────────────────────────────────────────────────
echo -e "${CYAN}=== Quickstart Summary ===${NC}"
echo ""

if [[ $FAILURES -eq 0 ]]; then
    echo -e "${GREEN}T0 READY${NC} — synthetic and daemon smoke passed. All pipeline artifacts verified."
else
    echo -e "${RED}$FAILURES check(s) FAILED${NC} — see output above."
fi

echo ""
echo "Evidence tier status:"
echo "  T0: Pipeline correct (synthetic + daemon smoke)    → $([ $FAILURES -eq 0 ] && echo 'PASS' || echo 'PARTIAL')"
echo "  T1: Live monitors produce non-zero PSI readings    → $([ ${T1_PSI:-0} -eq 1 ] && echo 'PSI available — run hermesd under load to collect' || echo 'PSI not available on this host')"
echo "  T2: Predictor fires on real pressure events        → run hermes_eval on a high-pressure run"
echo "  T3: Active intervention executes                   → HERMES_RUNTIME_MODE=active hermesd"
echo "  T4: Before/after latency evidence                  → hermes_bench --runs 5 config/oom_stress_scenario.yaml"
echo "  T5: strace + perf defensibility captures           → bash scripts/bench_strace.sh"
echo ""
echo "Next step: run hermesd under a real ML workload, then replay with:"
echo "  $BUILD_DIR/hermes_replay artifacts/logs/<run_id>"
echo "  $BUILD_DIR/hermes_pack   artifacts/logs/<run_id>"
echo ""
echo "Full evidence collection:"
echo "  bash scripts/smoke_phase6.sh"
echo "  bash scripts/collect_wsl2_evidence.sh"
echo ""

exit $((FAILURES > 0 ? 1 : 0))
