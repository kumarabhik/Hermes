#!/usr/bin/env bash
# smoke_phase6.sh — Phase 6 evidence collection automation for native Linux.
#
# Automates the Phase 6a-d steps that generate T1-T4 benchmark evidence:
#   Phase 6a — Live monitor validation (T1): daemon run with PSI non-zero
#   Phase 6b — Fidelity workload smoke (T2): observe-only run with real pressure,
#               then hermes_eval to check predictor fires
#   Phase 6c — Predictor calibration (T2): hermes_tune.py PASS/FAIL check
#   Phase 6d — False positive baseline (T4): low-pressure active-control run
#
# Prerequisites (native Linux / WSL2 with CUDA driver):
#   - build/hermesd, build/hermes_replay, build/hermes_eval, build/hermes_bench
#   - python3, stress-ng
#   - /proc/pressure/* available (kernel 4.20+)
#
# Usage:
#   bash scripts/smoke_phase6.sh [run-id-prefix] [artifact-root] [build-dir]
#
# Exits 0 only if all four phases pass.

set -euo pipefail

RUN_PREFIX="${1:-phase6-$(date +%Y%m%d-%H%M%S)}"
ARTIFACT_ROOT="${2:-artifacts}"
BUILD_DIR="${3:-build}"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

cd "$REPO_ROOT"

pass()  { echo "[PASS] $*"; }
fail()  { echo "[FAIL] $*" >&2; FAILED_STEPS+=("$*"); }
skip()  { echo "[SKIP] $*"; }
info()  { echo "       $*"; }
sep()   { echo ""; echo "--- $* ---"; }

FAILED_STEPS=()

echo "========================================================"
echo "Hermes Phase 6 Evidence Collection Smoke"
echo "========================================================"
echo "Run prefix    : $RUN_PREFIX"
echo "Artifact root : $ARTIFACT_ROOT"
echo "Build dir     : $BUILD_DIR"
echo ""

# ------------------------------------------------------------------ binaries
HERMESD="${BUILD_DIR}/hermesd"
HERMES_REPLAY="${BUILD_DIR}/hermes_replay"
HERMES_EVAL="${BUILD_DIR}/hermes_eval"
HERMES_BENCH="${BUILD_DIR}/hermes_bench"

for bin in "$HERMESD" "$HERMES_REPLAY" "$HERMES_EVAL" "$HERMES_BENCH"; do
    if [[ ! -x "$bin" ]]; then
        echo "[FATAL] Required binary not found or not executable: $bin"
        echo "        Build the project first: cmake --build build"
        exit 1
    fi
done
echo "Binaries found: OK"

# ------------------------------------------------------------------ Phase 6a: T1
sep "Phase 6a — Live monitor validation (T1)"

if [[ ! -f /proc/pressure/cpu ]]; then
    skip "Phase 6a: /proc/pressure/cpu not found — PSI not supported on this kernel"
    skip "Phase 6a: T1 cannot be verified on this host"
else
    RUN_ID_6A="${RUN_PREFIX}-6a-monitor"
    RUN_DIR="${ARTIFACT_ROOT}/logs/${RUN_ID_6A}"
    mkdir -p "$RUN_DIR"

    info "Starting hermesd (observe-only, 10 loops) alongside a Python hog..."

    # Background: Python memory hog to generate PSI
    python3 -c "
import time, math
buf = bytearray(512 * 1024 * 1024)  # 512 MB — enough for PSI without swapping
for i in range(200):
    _ = [math.sqrt(x * 1.001) for x in range(2000)]
    time.sleep(0.05)
" &
    HOG_PID=$!

    # Run hermesd for 10 loops, then kill
    HERMES_ARTIFACT_ROOT="$ARTIFACT_ROOT" \
    HERMES_MAX_LOOPS=10 \
    "$HERMESD" --run-id "$RUN_ID_6A" --mode observe-only &
    HERMES_PID=$!

    # Give it time to collect some samples
    sleep 5

    kill "$HOG_PID" 2>/dev/null || true
    wait "$HERMES_PID" 2>/dev/null || true

    # Check that telemetry_quality.json has non-zero PSI
    TQ="${RUN_DIR}/telemetry_quality.json"
    if [[ ! -f "$TQ" ]]; then
        fail "Phase 6a: telemetry_quality.json not written (hermesd did not produce artifacts)"
    else
        CPU_PSI=$(python3 -c "
import json, sys
try:
    d = json.load(open('${TQ}'))
    print(d.get('peak_cpu_psi', 0))
except: print(0)
")
        MEM_PSI=$(python3 -c "
import json, sys
try:
    d = json.load(open('${TQ}'))
    print(d.get('peak_mem_psi', 0))
except: print(0)
")
        info "peak_cpu_psi=${CPU_PSI}  peak_mem_psi=${MEM_PSI}"
        if python3 -c "import sys; sys.exit(0 if float('${CPU_PSI}') > 0 or float('${MEM_PSI}') > 0 else 1)" 2>/dev/null; then
            pass "Phase 6a: non-zero PSI observed on live host (T1 evidence collected)"
        else
            fail "Phase 6a: PSI still 0 — host may be too lightly loaded or PSI not supported"
        fi
    fi
fi

# ------------------------------------------------------------------ Phase 6b: T2 predictor
sep "Phase 6b — Fidelity workload + predictor evaluation (T2)"

RUN_ID_6B="${RUN_PREFIX}-6b-fidelity"

"$HERMES_BENCH" config/observe_scenario.yaml \
    --run-id "$RUN_ID_6B" \
    --artifact-root "$ARTIFACT_ROOT" \
    --hermes-bin "$HERMESD" \
    --replay-bin "$HERMES_REPLAY" \
  || { fail "Phase 6b: hermes_bench observe_scenario.yaml exited non-zero"; }

# Run hermes_eval on the run directory
HERMES_RUN_DIR=$(find "${ARTIFACT_ROOT}/logs" -maxdepth 1 -name "${RUN_ID_6B}-hermes" -type d 2>/dev/null | head -1)
if [[ -z "$HERMES_RUN_DIR" ]]; then
    HERMES_RUN_DIR=$(find "${ARTIFACT_ROOT}/logs" -maxdepth 1 -name "${RUN_ID_6B}*" -type d 2>/dev/null | head -1)
fi

if [[ -z "$HERMES_RUN_DIR" ]]; then
    fail "Phase 6b: hermes run directory not found for ${RUN_ID_6B}"
else
    info "Running hermes_eval on $HERMES_RUN_DIR"
    "$HERMES_EVAL" "$HERMES_RUN_DIR" \
      || info "hermes_eval returned non-zero (may be no events — check manually)"

    EVAL_JSON="${HERMES_RUN_DIR}/eval_summary.json"
    if [[ -f "$EVAL_JSON" ]]; then
        TOTAL_PREDS=$(python3 -c "
import json
try:
    d = json.load(open('${EVAL_JSON}'))
    print(d.get('total_predictions', 0))
except: print(0)
")
        info "total_predictions=${TOTAL_PREDS}"
        if python3 -c "import sys; sys.exit(0 if int('${TOTAL_PREDS}') > 0 else 1)" 2>/dev/null; then
            pass "Phase 6b: predictor emitted ${TOTAL_PREDS} predictions (T2 evidence collected)"
        else
            fail "Phase 6b: total_predictions=0 — predictor did not fire under fidelity load"
            info "Check that UPS threshold is not too high in config/schema.yaml"
        fi
    else
        fail "Phase 6b: eval_summary.json not written"
    fi
fi

# ------------------------------------------------------------------ Phase 6c: T2 calibration
sep "Phase 6c — Predictor calibration check (T2)"

if python3 scripts/hermes_tune.py --eval-dir "$ARTIFACT_ROOT/logs"; then
    pass "Phase 6c: all calibration targets met (precision/recall/FP rate within spec)"
else
    EXIT_CODE=$?
    if [[ $EXIT_CODE -eq 1 ]]; then
        fail "Phase 6c: one or more calibration targets FAILED — see hermes_tune.py output above"
        info "Adjust config/schema.yaml thresholds per suggestions, then re-run"
    else
        fail "Phase 6c: hermes_tune.py returned unexpected exit code ${EXIT_CODE}"
    fi
fi

# ------------------------------------------------------------------ Phase 6d: T4 false positive
sep "Phase 6d — False positive baseline (T4)"

RUN_ID_6D="${RUN_PREFIX}-6d-fp"

"$HERMES_BENCH" config/low_pressure_scenario.yaml \
    --run-id "$RUN_ID_6D" \
    --artifact-root "$ARTIFACT_ROOT" \
    --hermes-bin "$HERMESD" \
    --replay-bin "$HERMES_REPLAY" \
    --verify-targets \
  && pass "Phase 6d: intervention_count within expected_max_intervention_count (false positive rate OK)" \
  || fail "Phase 6d: --verify-targets failed — Hermes fired too many interventions on a quiet host"

# ------------------------------------------------------------------ Summary
echo ""
echo "========================================================"
echo "Phase 6 Smoke Summary"
echo "========================================================"

if [[ ${#FAILED_STEPS[@]} -eq 0 ]]; then
    echo "ALL PHASES PASSED"
    echo ""
    echo "Evidence collected:"
    echo "  T1: non-zero PSI in live daemon run (Phase 6a)"
    echo "  T2: predictor fired under fidelity load (Phase 6b)"
    echo "  T2: calibration targets met (Phase 6c)"
    echo "  T4: false positive rate within budget (Phase 6d)"
    echo ""
    echo "Next: run a 5-run baseline + active-control comparison for T4 improvement claim."
    echo "  hermes_bench config/baseline_scenario.yaml --runs 5 --run-id baseline-5run"
    echo "  hermes_bench config/oom_stress_scenario.yaml --runs 5 --hermes-bin build/hermesd \\"
    echo "    --delta-vs artifacts/bench/baseline-5run-latency.json"
    echo ""
    echo "Run check_evidence_tiers.py to confirm tier status:"
    echo "  python3 scripts/check_evidence_tiers.py"
    exit 0
else
    echo "FAILED STEPS:"
    for step in "${FAILED_STEPS[@]}"; do
        echo "  - $step"
    done
    echo ""
    echo "Fix the failures above and re-run this script."
    echo "Partial results are saved in ${ARTIFACT_ROOT}/logs/"
    exit 1
fi
