#!/usr/bin/env bash
# collect_wsl2_evidence.sh — Full evidence collection run for WSL2 / Linux.
#
# Runs baseline, observe-only, and active-control benchmark scenarios with
# strace, perf (software counters), and hermes_reeval state coverage to
# produce the minimum defensibility package from a single invocation.
#
# All outputs land under artifacts/evidence/<run_id>/ and in the standard
# artifacts/bench/ and artifacts/logs/ trees.
#
# Usage:
#   bash scripts/collect_wsl2_evidence.sh [run-id] [artifact-root] [build-dir]
#
# Prerequisites:
#   - cmake -S . -B build && cmake --build build
#   - apt install stress-ng strace linux-tools-common linux-tools-$(uname -r)
#
# Exits 0 if all steps pass (individual capture failures are non-fatal and
# are noted in the evidence manifest).

set -uo pipefail   # no -e: capture failures are warnings, not hard exits

RUN_ID="${1:-evidence-$(date +%Y%m%d-%H%M%S)}"
ARTIFACT_ROOT="${2:-artifacts}"
BUILD_DIR="${3:-build}"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

EVIDENCE_DIR="${ARTIFACT_ROOT}/evidence/${RUN_ID}"
mkdir -p "$EVIDENCE_DIR"

# ---- binaries ----
HERMESD="${BUILD_DIR}/hermesd"
HERMES_REPLAY="${BUILD_DIR}/hermes_replay"
HERMES_BENCH="${BUILD_DIR}/hermes_bench"
HERMES_COMPARE="${BUILD_DIR}/hermes_compare"
HERMES_REEVAL="${BUILD_DIR}/hermes_reeval"

pass()  { echo "[PASS]  $*" | tee -a "${EVIDENCE_DIR}/manifest.txt"; }
warn()  { echo "[WARN]  $*" | tee -a "${EVIDENCE_DIR}/manifest.txt"; }
fail()  { echo "[FAIL]  $*" | tee -a "${EVIDENCE_DIR}/manifest.txt"; }
info()  { echo "        $*"; }
step()  { echo ""; echo "=== $* ==="; }

{
  echo "collect_wsl2_evidence"
  echo "run_id: $RUN_ID"
  echo "date: $(date -Iseconds)"
  echo "kernel: $(uname -r)"
  echo "host: $(hostname)"
} > "${EVIDENCE_DIR}/manifest.txt"

echo "=== Hermes WSL2 Evidence Collection ==="
echo "Run id        : $RUN_ID"
echo "Artifact root : $ARTIFACT_ROOT"
echo "Evidence dir  : $EVIDENCE_DIR"
echo "Build dir     : $BUILD_DIR"

# ---- prerequisite check ----
step "Prerequisites"
MISSING=0
for BIN in "$HERMESD" "$HERMES_REPLAY" "$HERMES_BENCH" "$HERMES_COMPARE" "$HERMES_REEVAL"; do
    if [[ ! -x "$BIN" ]]; then
        fail "Binary missing: $BIN"
        MISSING=1
    fi
done
[[ $MISSING -eq 0 ]] || { echo "Build first: cmake -S . -B build && cmake --build build"; exit 1; }
pass "All required binaries present"

# PSI
if [[ -r /proc/pressure/cpu ]]; then
    pass "PSI available: $(cat /proc/pressure/cpu | head -1)"
else
    warn "PSI not available — Hermes will record zero PSI values"
fi

# stress-ng
if command -v stress-ng &>/dev/null; then
    pass "stress-ng: $(stress-ng --version 2>&1 | head -1)"
else
    warn "stress-ng not installed — memory pressure workloads will use Python fallbacks"
fi

# strace
if command -v strace &>/dev/null; then
    pass "strace: $(strace --version 2>&1 | head -1)"
else
    warn "strace not installed — strace capture will be skipped"
fi

# perf
if command -v perf &>/dev/null; then
    pass "perf: $(perf --version 2>&1 | head -1)"
else
    warn "perf not installed — perf capture will be skipped"
fi

# GPU
if command -v nvidia-smi &>/dev/null; then
    GPU_NAME=$(nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | head -1 || echo "")
    [[ -n "$GPU_NAME" ]] && pass "GPU visible: $GPU_NAME" || warn "nvidia-smi present but no GPU found"
else
    warn "nvidia-smi not found — GPU metrics will be zero"
fi

# ---- build short scenario YAMLs ----
step "Generating scenario configs"

BASELINE_YAML="${EVIDENCE_DIR}/baseline.yaml"
OBSERVE_YAML="${EVIDENCE_DIR}/observe.yaml"
ACTIVE_YAML="${EVIDENCE_DIR}/active.yaml"
OOM_YAML="${EVIDENCE_DIR}/oom_stress.yaml"

"$HERMES_BENCH" --generate-baseline  "$BASELINE_YAML" 2>/dev/null
"$HERMES_BENCH" --generate-active    "$ACTIVE_YAML"   2>/dev/null
"$HERMES_BENCH" --generate-oom-stress "$OOM_YAML"     2>/dev/null
cp "$BASELINE_YAML" "$OBSERVE_YAML"

# Patch all scenarios: shorten durations, use echo/python fallbacks for smoke-speed runs.
patch_yaml() {
    local yaml="$1" mode="$2"
    python3 - "$yaml" "$mode" << 'PYEOF'
import re, sys
path, mode = sys.argv[1], sys.argv[2]
content = open(path).read()
# Shorten timing for evidence runs
content = re.sub(r'warmup_s:\s*\d+',        'warmup_s: 0',   content)
content = re.sub(r'measurement_s:\s*\d+',   'measurement_s: 30', content)
content = re.sub(r'repeat_count:\s*\d+',    'repeat_count: 3',   content)
# Keep real commands if stress-ng available, else fall back to python3
try:
    import subprocess
    subprocess.check_call(['which', 'stress-ng'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
except Exception:
    content = re.sub(r'command: "stress-ng[^"]*"',
                     'command: "python3 -c \\"import time; time.sleep(5)\\""', content)
if mode != 'baseline':
    content = content.replace('runtime_mode: baseline', f'runtime_mode: {mode}', 1)
open(path, 'w').write(content)
PYEOF
}

patch_yaml "$BASELINE_YAML" "baseline"
patch_yaml "$OBSERVE_YAML"  "observe-only"
patch_yaml "$ACTIVE_YAML"   "active-control"
patch_yaml "$OOM_YAML"      "active-control"
pass "Scenario YAMLs written to $EVIDENCE_DIR"

# ---- helper: run hermes_reeval + capture state_coverage ----
run_reeval() {
    local run_dir="$1" label="$2"
    if [[ -d "$run_dir" && -f "$run_dir/samples.ndjson" ]]; then
        "$HERMES_REEVAL" "$run_dir" \
            --out "${run_dir}/replay_eval.ndjson" > "${run_dir}/reeval_stdout.txt" 2>&1 && \
            pass "hermes_reeval [$label]: state_coverage.json written" || \
            warn "hermes_reeval [$label]: failed"
        [[ -f "${run_dir}/state_coverage.json" ]] && \
            cp "${run_dir}/state_coverage.json" \
               "${EVIDENCE_DIR}/${label}-state_coverage.json" && \
            pass "State coverage [$label]: copied to evidence dir"
    else
        warn "hermes_reeval [$label]: run directory missing or has no samples"
    fi
}

# ---- Step A: Baseline benchmark ----
step "A: Baseline benchmark (no Hermes)"
BASELINE_RUN="${RUN_ID}-baseline"
"$HERMES_BENCH" "$BASELINE_YAML" \
    --artifact-root "$ARTIFACT_ROOT" \
    --run-id "$BASELINE_RUN" \
    --runs 3 2>&1 | tee "${EVIDENCE_DIR}/baseline-bench.log" || true

BASELINE_SUMMARY="${ARTIFACT_ROOT}/bench/${BASELINE_RUN}-r1-summary.json"
# Single-run fallback
[[ -f "$BASELINE_SUMMARY" ]] || BASELINE_SUMMARY="${ARTIFACT_ROOT}/bench/${BASELINE_RUN}-summary.json"
if [[ -f "$BASELINE_SUMMARY" ]]; then
    cp "$BASELINE_SUMMARY" "${EVIDENCE_DIR}/baseline-summary.json"
    pass "Baseline summary: $BASELINE_SUMMARY"
    BASELINE_P95=$(python3 -c "import json; d=json.load(open('$BASELINE_SUMMARY')); print(d.get('p95_latency_ms','null'))" 2>/dev/null || echo "null")
    info "Baseline p95 latency: ${BASELINE_P95} ms"
else
    warn "Baseline summary not found"
fi

# ---- Step B: strace capture on baseline ----
step "B: strace syscall summary"
if command -v strace &>/dev/null; then
    STRACE_OUT="${EVIDENCE_DIR}/baseline-strace.txt"
    # Run one foreground workload under strace -c for syscall summary
    FG_CMD=$(python3 -c "
import sys, re
try:
    txt = open('$BASELINE_YAML').read()
    m = re.search(r'foreground:\s*true.*?command:\s*\"([^\"]+)\"', txt, re.DOTALL)
    if not m:
        m = re.search(r'command:\s*\"([^\"]+)\"', txt)
    print(m.group(1) if m else 'echo strace-target')
except Exception:
    print('echo strace-target')
" 2>/dev/null)
    info "strace target command: $FG_CMD"
    strace -c -o "$STRACE_OUT" bash -c "$FG_CMD" > /dev/null 2>&1 && \
        pass "strace: syscall summary written to $STRACE_OUT" || \
        warn "strace: capture failed or command exited non-zero"
    [[ -f "$STRACE_OUT" ]] && grep -q "calls" "$STRACE_OUT" && \
        info "$(grep 'total' "$STRACE_OUT" | head -1)"
else
    warn "strace not available — skipping syscall capture"
fi

# ---- Step C: Observe-only benchmark + perf ----
step "C: Observe-only benchmark with Hermes (perf wrapped)"
OBSERVE_RUN="${RUN_ID}-observe"
PERF_OUT="${EVIDENCE_DIR}/observe-perf.txt"

run_bench() {
    "$HERMES_BENCH" "$OBSERVE_YAML" \
        --artifact-root "$ARTIFACT_ROOT" \
        --run-id "$OBSERVE_RUN" \
        --hermes-bin "$HERMESD" \
        --replay-bin "$HERMES_REPLAY" \
        --runs 3 2>&1 | tee "${EVIDENCE_DIR}/observe-bench.log" || true
}

if command -v perf &>/dev/null; then
    perf stat -e task-clock,context-switches,cpu-migrations,page-faults \
        -o "$PERF_OUT" -- bash -c "$(declare -f run_bench); run_bench" 2>&1 || true
    [[ -f "$PERF_OUT" ]] && \
        pass "perf stat: software counters written to $PERF_OUT" || \
        warn "perf stat: output not written"
else
    run_bench
    warn "perf not available — benchmark ran without perf wrapper"
fi

OBSERVE_SUMMARY="${ARTIFACT_ROOT}/bench/${OBSERVE_RUN}-r1-summary.json"
[[ -f "$OBSERVE_SUMMARY" ]] || OBSERVE_SUMMARY="${ARTIFACT_ROOT}/bench/${OBSERVE_RUN}-summary.json"
if [[ -f "$OBSERVE_SUMMARY" ]]; then
    cp "$OBSERVE_SUMMARY" "${EVIDENCE_DIR}/observe-summary.json"
    pass "Observe-only summary: $OBSERVE_SUMMARY"
    OBSERVE_P95=$(python3 -c "import json; d=json.load(open('$OBSERVE_SUMMARY')); print(d.get('p95_latency_ms','null'))" 2>/dev/null || echo "null")
    info "Observe-only p95 latency: ${OBSERVE_P95} ms"
    # Run hermes_reeval on the Hermes daemon run
    HERMES_RUN_DIR="${ARTIFACT_ROOT}/logs/${OBSERVE_RUN}-hermes"
    run_reeval "$HERMES_RUN_DIR" "observe"
else
    warn "Observe-only summary not found"
fi

# ---- Step D: Active-control benchmark ----
step "D: Active-control benchmark with Hermes"
ACTIVE_RUN="${RUN_ID}-active"
"$HERMES_BENCH" "$ACTIVE_YAML" \
    --artifact-root "$ARTIFACT_ROOT" \
    --run-id "$ACTIVE_RUN" \
    --hermes-bin "$HERMESD" \
    --replay-bin "$HERMES_REPLAY" \
    --runs 3 2>&1 | tee "${EVIDENCE_DIR}/active-bench.log" || true

ACTIVE_SUMMARY="${ARTIFACT_ROOT}/bench/${ACTIVE_RUN}-r1-summary.json"
[[ -f "$ACTIVE_SUMMARY" ]] || ACTIVE_SUMMARY="${ARTIFACT_ROOT}/bench/${ACTIVE_RUN}-summary.json"
if [[ -f "$ACTIVE_SUMMARY" ]]; then
    cp "$ACTIVE_SUMMARY" "${EVIDENCE_DIR}/active-summary.json"
    pass "Active-control summary: $ACTIVE_SUMMARY"
    ACTIVE_P95=$(python3 -c "import json; d=json.load(open('$ACTIVE_SUMMARY')); print(d.get('p95_latency_ms','null'))" 2>/dev/null || echo "null")
    ACTIVE_OOM=$(python3 -c "import json; d=json.load(open('$ACTIVE_SUMMARY')); print(d.get('oom_count',0))" 2>/dev/null || echo "?")
    ACTIVE_INTERV=$(python3 -c "import json; d=json.load(open('$ACTIVE_SUMMARY')); print(d.get('intervention_count',0))" 2>/dev/null || echo "?")
    info "Active-control p95 latency: ${ACTIVE_P95} ms"
    info "Active-control OOM kills  : ${ACTIVE_OOM}"
    info "Active-control interventions: ${ACTIVE_INTERV}"
    HERMES_ACTIVE_DIR="${ARTIFACT_ROOT}/logs/${ACTIVE_RUN}-hermes"
    run_reeval "$HERMES_ACTIVE_DIR" "active"
else
    warn "Active-control summary not found"
fi

# ---- Step E: hermes_compare cross-mode table ----
step "E: Cross-mode comparison"
COMPARE_CSV="${EVIDENCE_DIR}/comparison.csv"
"$HERMES_COMPARE" \
    --bench-dir "${ARTIFACT_ROOT}/bench" \
    --output-csv "$COMPARE_CSV" 2>&1 | tee "${EVIDENCE_DIR}/compare.log" || true
if [[ -f "$COMPARE_CSV" ]]; then
    pass "hermes_compare: comparison CSV written"
    info "Comparison table:"
    column -t -s',' "$COMPARE_CSV" 2>/dev/null | head -10 | sed 's/^/        /'
else
    warn "hermes_compare: CSV not written"
fi

# ---- Step F: OOM-stress scenario (optional — only if stress-ng available) ----
step "F: OOM-stress scenario (optional)"
if command -v stress-ng &>/dev/null; then
    OOM_RUN="${RUN_ID}-oom"
    "$HERMES_BENCH" "$OOM_YAML" \
        --artifact-root "$ARTIFACT_ROOT" \
        --run-id "$OOM_RUN" \
        --hermes-bin "$HERMESD" \
        --replay-bin "$HERMES_REPLAY" \
        --runs 2 2>&1 | tee "${EVIDENCE_DIR}/oom-bench.log" || true
    OOM_SUMMARY="${ARTIFACT_ROOT}/bench/${OOM_RUN}-r1-summary.json"
    [[ -f "$OOM_SUMMARY" ]] || OOM_SUMMARY="${ARTIFACT_ROOT}/bench/${OOM_RUN}-summary.json"
    if [[ -f "$OOM_SUMMARY" ]]; then
        cp "$OOM_SUMMARY" "${EVIDENCE_DIR}/oom-summary.json"
        TARGET_MET=$(python3 -c "import json; d=json.load(open('$OOM_SUMMARY')); print(d.get('latency_target_met','null'))" 2>/dev/null || echo "null")
        OOM_COUNT=$(python3 -c "import json; d=json.load(open('$OOM_SUMMARY')); print(d.get('oom_count',0))" 2>/dev/null || echo "?")
        pass "OOM-stress summary written"
        info "Latency target met: $TARGET_MET"
        info "OOM kills: $OOM_COUNT"
    else
        warn "OOM-stress summary not found"
    fi
else
    warn "stress-ng not installed — OOM-stress scenario skipped (install: apt install stress-ng)"
fi

# ---- Final manifest summary ----
step "Evidence collection complete"
echo ""
echo "Evidence directory : $EVIDENCE_DIR"
echo "Files collected:"
ls -1 "$EVIDENCE_DIR/" | sed 's/^/  /'
echo ""

# Print comparison summary if available
if [[ -f "$COMPARE_CSV" ]]; then
    echo "=== Benchmark Comparison ==="
    column -t -s',' "$COMPARE_CSV" 2>/dev/null || cat "$COMPARE_CSV"
    echo ""
fi

echo "Next steps:"
echo "  1. Update RESULTS.md with the run_id '$RUN_ID' and key metrics above"
echo "  2. Run: hermes_report artifacts/logs to see cross-run replay summary"
echo "  3. Review state coverage: cat ${EVIDENCE_DIR}/*-state_coverage.json"
if [[ -f "${EVIDENCE_DIR}/baseline-strace.txt" ]]; then
    echo "  4. Review strace: cat ${EVIDENCE_DIR}/baseline-strace.txt"
fi
if [[ -f "${EVIDENCE_DIR}/observe-perf.txt" ]]; then
    echo "  5. Review perf: cat ${EVIDENCE_DIR}/observe-perf.txt"
fi

echo ""
PASS_COUNT=$(grep -c '^\[PASS\]' "${EVIDENCE_DIR}/manifest.txt" 2>/dev/null || echo 0)
WARN_COUNT=$(grep -c '^\[WARN\]' "${EVIDENCE_DIR}/manifest.txt" 2>/dev/null || echo 0)
FAIL_COUNT=$(grep -c '^\[FAIL\]' "${EVIDENCE_DIR}/manifest.txt" 2>/dev/null || echo 0)
echo "Results: $PASS_COUNT passed, $WARN_COUNT warnings, $FAIL_COUNT failures"
echo "Full manifest: ${EVIDENCE_DIR}/manifest.txt"

[[ $FAIL_COUNT -eq 0 ]]
