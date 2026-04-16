#!/usr/bin/env bash
# smoke_wsl2.sh — Full smoke suite for WSL2 / Linux using CMake build output.
#
# Runs the equivalent of all PowerShell smoke scripts using bash and the CMake
# build directory. Verifies:
#   1. PSI availability (/proc/pressure/cpu)
#   2. Synthetic fixture replay
#   3. One-loop daemon replay (observe-only)
#   4. Benchmark plan artifact generation
#   5. Baseline benchmark launch + summary
#   6. Observe-only benchmark with Hermes + replay
#   7. hermes_compare comparison CSV
#   8. hermes_reeval state coverage verification
#
# Usage:
#   bash scripts/smoke_wsl2.sh [run-id-prefix] [artifact-root] [build-dir]
#
# Exits 0 if all checks pass. On failure, prints which step failed.

set -euo pipefail

RUN_PREFIX="${1:-smoke-wsl2-$(date +%Y%m%d-%H%M%S)}"
ARTIFACT_ROOT="${2:-artifacts}"
BUILD_DIR="${3:-build}"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

cd "$REPO_ROOT"

pass() { echo "[PASS] $*"; }
fail() { echo "[FAIL] $*" >&2; exit 1; }
info() { echo "       $*"; }

echo "=== Hermes WSL2 Smoke Suite ==="
echo "Run prefix    : $RUN_PREFIX"
echo "Artifact root : $ARTIFACT_ROOT"
echo "Build dir     : $BUILD_DIR"
echo ""

# ------------------------------------------------------------------ binaries
HERMESD="${BUILD_DIR}/hermesd"
HERMESD_MT="${BUILD_DIR}/hermesd_mt"
HERMES_REPLAY="${BUILD_DIR}/hermes_replay"
HERMES_SYNTH="${BUILD_DIR}/hermes_synth"
HERMES_BENCH="${BUILD_DIR}/hermes_bench"
HERMES_COMPARE="${BUILD_DIR}/hermes_compare"
HERMES_REPORT="${BUILD_DIR}/hermes_report"
HERMES_REEVAL="${BUILD_DIR}/hermes_reeval"

for BIN in "$HERMESD" "$HERMES_REPLAY" "$HERMES_SYNTH" "$HERMES_BENCH" "$HERMES_COMPARE" "$HERMES_REEVAL"; do
    if [[ ! -x "$BIN" ]]; then
        fail "Binary not found or not executable: $BIN — run: cmake -S . -B build && cmake --build build"
    fi
done
pass "All required binaries found in $BUILD_DIR"

# ------------------------------------------------------------------ 1: PSI check
echo ""
echo "--- Step 1: PSI availability ---"
if [[ -r /proc/pressure/cpu ]]; then
    PSI_CONTENT=$(cat /proc/pressure/cpu)
    pass "/proc/pressure/cpu is readable"
    info "$(echo "$PSI_CONTENT" | head -1)"
else
    echo "[WARN] /proc/pressure/cpu not found. Hermes will run without real PSI data."
    echo "       (Acceptable for smoke — daemon gracefully handles missing PSI)"
fi

if [[ -r /proc/pressure/memory ]]; then
    pass "/proc/pressure/memory is readable"
else
    echo "[WARN] /proc/pressure/memory not found."
fi

# ------------------------------------------------------------------ 2: Synthetic replay
echo ""
echo "--- Step 2: Synthetic fixture replay ---"
SYNTH_RUN_ID="${RUN_PREFIX}-synth"
HERMES_MAX_LOOPS=1 HERMES_RUN_ID="$SYNTH_RUN_ID" \
    HERMES_ARTIFACT_ROOT="$ARTIFACT_ROOT" \
    "$HERMES_SYNTH" "$SYNTH_RUN_ID" > /dev/null 2>&1 || fail "hermes_synth failed"

SYNTH_DIR="${ARTIFACT_ROOT}/logs/${SYNTH_RUN_ID}"
[[ -f "${SYNTH_DIR}/samples.ndjson" ]] || fail "synthetic samples.ndjson not found"
[[ -f "${SYNTH_DIR}/run_metadata.json" ]] || fail "synthetic run_metadata.json not found"

"$HERMES_REPLAY" "$SYNTH_DIR" "$ARTIFACT_ROOT" > /dev/null 2>&1 || fail "hermes_replay on synth run failed"
[[ -f "${SYNTH_DIR}/replay_summary.json" ]] || fail "synthetic replay_summary.json not found"
VALID=$(python3 -c "import json,sys; d=json.load(open('${SYNTH_DIR}/replay_summary.json')); sys.exit(0 if d.get('valid') else 1)" 2>/dev/null) || fail "synthetic replay summary is invalid"
pass "Synthetic fixture replay: replay_summary valid"

# ------------------------------------------------------------------ 3: Daemon replay
echo ""
echo "--- Step 3: One-loop daemon replay (observe-only) ---"
DAEMON_RUN_ID="${RUN_PREFIX}-daemon"
HERMES_MAX_LOOPS=1 \
HERMES_RUNTIME_MODE=observe-only \
HERMES_RUN_ID="$DAEMON_RUN_ID" \
HERMES_SCENARIO=smoke-wsl2 \
HERMES_ARTIFACT_ROOT="$ARTIFACT_ROOT" \
    "$HERMESD" > /dev/null 2>&1 || fail "hermesd one-loop run failed"

DAEMON_DIR="${ARTIFACT_ROOT}/logs/${DAEMON_RUN_ID}"
for ARTIFACT in run_metadata.json config_snapshot.yaml telemetry_quality.json \
                samples.ndjson scores.ndjson predictions.ndjson decisions.ndjson \
                actions.ndjson events.ndjson; do
    [[ -f "${DAEMON_DIR}/${ARTIFACT}" ]] || fail "daemon artifact missing: $ARTIFACT"
done
pass "Daemon artifacts: all 9 NDJSON/JSON files present"

"$HERMES_REPLAY" "$DAEMON_DIR" "$ARTIFACT_ROOT" > /dev/null 2>&1 || fail "hermes_replay on daemon run failed"
[[ -f "${DAEMON_DIR}/replay_summary.json" ]] || fail "daemon replay_summary.json not found"
pass "Daemon replay: replay_summary written"

# ------------------------------------------------------------------ 4: Benchmark plan
echo ""
echo "--- Step 4: Benchmark plan artifact ---"
PLAN_RUN_ID="${RUN_PREFIX}-plan"
BASELINE_YAML="${BUILD_DIR}/${PLAN_RUN_ID}-baseline.yaml"
"$HERMES_BENCH" --generate-baseline "$BASELINE_YAML" > /dev/null 2>&1 || fail "generate-baseline failed"

# Patch commands to use local echo for WSL2 smoke (avoids needing stress-ng).
python3 - "$BASELINE_YAML" << 'PYEOF'
import re, sys
path = sys.argv[1]
content = open(path).read()
content = re.sub(r'command: "stress-ng[^"]*"', 'command: "echo smoke-bg-wsl2"', content)
content = re.sub(r'command: "python3[^"]*"', 'command: "echo smoke-fg-wsl2"', content)
open(path, 'w').write(content)
PYEOF

"$HERMES_BENCH" "$BASELINE_YAML" \
    --dry-run \
    --artifact-root "$ARTIFACT_ROOT" \
    --run-id "$PLAN_RUN_ID" > /dev/null 2>&1 || fail "benchmark dry-run plan failed"

[[ -f "${ARTIFACT_ROOT}/bench/${PLAN_RUN_ID}-plan.json" ]] || fail "plan artifact not found"
[[ -f "${ARTIFACT_ROOT}/bench/${PLAN_RUN_ID}-scenario.yaml" ]] || fail "scenario snapshot not found"
pass "Benchmark plan artifacts written"

# ------------------------------------------------------------------ 5: Baseline launch
echo ""
echo "--- Step 5: Baseline benchmark launch ---"
BASELINE_RUN_ID="${RUN_PREFIX}-baseline"
"$HERMES_BENCH" "$BASELINE_YAML" \
    --artifact-root "$ARTIFACT_ROOT" \
    --run-id "$BASELINE_RUN_ID" > /dev/null 2>&1 || true  # non-zero ok if workloads time out

BASELINE_SUMMARY="${ARTIFACT_ROOT}/bench/${BASELINE_RUN_ID}-summary.json"
[[ -f "$BASELINE_SUMMARY" ]] || fail "baseline summary artifact not found"

for FIELD in completion_rate intervention_count oom_count degraded_behavior; do
    grep -q "\"$FIELD\"" "$BASELINE_SUMMARY" || fail "baseline summary missing field: $FIELD"
done
pass "Baseline benchmark: summary artifact with enriched fields present"

# ------------------------------------------------------------------ 6: Observe-only + Hermes
echo ""
echo "--- Step 6: Observe-only benchmark with Hermes ---"
OBSERVE_YAML="${BUILD_DIR}/${PLAN_RUN_ID}-observe.yaml"
cp "$BASELINE_YAML" "$OBSERVE_YAML"
python3 - "$OBSERVE_YAML" << 'PYEOF'
import sys
path = sys.argv[1]
content = open(path).read()
content = content.replace('runtime_mode: baseline', 'runtime_mode: observe-only', 1)
open(path, 'w').write(content)
PYEOF

OBSERVE_RUN_ID="${RUN_PREFIX}-observe"
"$HERMES_BENCH" "$OBSERVE_YAML" \
    --artifact-root "$ARTIFACT_ROOT" \
    --run-id "$OBSERVE_RUN_ID" \
    --hermes-bin "$HERMESD" \
    --replay-bin "$HERMES_REPLAY" > /dev/null 2>&1 || true

OBSERVE_SUMMARY="${ARTIFACT_ROOT}/bench/${OBSERVE_RUN_ID}-summary.json"
[[ -f "$OBSERVE_SUMMARY" ]] || fail "observe-only summary artifact not found"

REPLAY_VALID=$(python3 -c "
import json, sys
d = json.load(open('$OBSERVE_SUMMARY'))
rs = d.get('replay_snapshot', {})
print(rs.get('available', False))
" 2>/dev/null || echo "False")
[[ "$REPLAY_VALID" != "False" ]] || echo "[WARN] replay snapshot not available in observe summary"
pass "Observe-only benchmark: Hermes + replay summary written"

# ------------------------------------------------------------------ 7: hermes_compare
echo ""
echo "--- Step 7: hermes_compare comparison CSV ---"
COMPARE_CSV="${ARTIFACT_ROOT}/bench/${RUN_PREFIX}-comparison.csv"
"$HERMES_COMPARE" \
    --bench-dir "${ARTIFACT_ROOT}/bench" \
    --output-csv "$COMPARE_CSV" > /dev/null 2>&1 || fail "hermes_compare failed"

[[ -f "$COMPARE_CSV" ]] || fail "comparison CSV not written: $COMPARE_CSV"
grep -q "baseline" "$COMPARE_CSV" || fail "comparison CSV missing baseline row"
grep -q "observe-only" "$COMPARE_CSV" || fail "comparison CSV missing observe-only row"
pass "hermes_compare: comparison CSV with baseline and observe-only rows"

# ------------------------------------------------------------------ 8: hermes_reeval state coverage
echo ""
echo "--- Step 8: hermes_reeval state coverage ---"

REEVAL_OUT="${ARTIFACT_ROOT}/logs/${DAEMON_RUN_ID}/replay_eval.ndjson"
"$HERMES_REEVAL" "${ARTIFACT_ROOT}/logs/${DAEMON_RUN_ID}" \
    --out "$REEVAL_OUT" > /dev/null 2>&1 || fail "hermes_reeval failed on daemon run"

COVERAGE_JSON="${ARTIFACT_ROOT}/logs/${DAEMON_RUN_ID}/state_coverage.json"
[[ -f "$COVERAGE_JSON" ]] || fail "state_coverage.json not written by hermes_reeval"

# Verify it has the expected structure
python3 -c "
import json, sys
d = json.load(open('$COVERAGE_JSON'))
assert 'states_visited' in d, 'missing states_visited'
assert 'states_total' in d, 'missing states_total'
assert 'state_counts' in d, 'missing state_counts'
assert 'transitions' in d, 'missing transitions'
print('states_visited:', d['states_visited'], '/', d['states_total'])
" || fail "state_coverage.json has unexpected structure"

pass "hermes_reeval: state_coverage.json written and valid"

# Also verify p95_latency_ms and latency_target_met fields exist in baseline summary
BASELINE_SUMMARY="${ARTIFACT_ROOT}/bench/${BASELINE_RUN_ID}-summary.json"
if [[ -f "$BASELINE_SUMMARY" ]]; then
    for FIELD in p95_latency_ms latency_target_ms latency_target_met; do
        grep -q "\"${FIELD}\"" "$BASELINE_SUMMARY" || fail "baseline summary missing latency assertion field: $FIELD"
    done
    pass "Benchmark summary: p95_latency_ms/latency_target_ms/latency_target_met fields present"
fi

# ------------------------------------------------------------------ summary
echo ""
echo "=== WSL2 Smoke Suite PASSED (8/8 steps) ==="
echo ""
echo "Artifacts written:"
echo "  ${ARTIFACT_ROOT}/logs/${SYNTH_RUN_ID}/"
echo "  ${ARTIFACT_ROOT}/logs/${DAEMON_RUN_ID}/"
echo "  ${COVERAGE_JSON}"
echo "  ${ARTIFACT_ROOT}/bench/${PLAN_RUN_ID}-plan.json"
echo "  ${ARTIFACT_ROOT}/bench/${BASELINE_RUN_ID}-summary.json"
echo "  ${ARTIFACT_ROOT}/bench/${OBSERVE_RUN_ID}-summary.json"
echo "  ${COMPARE_CSV}"
echo ""
echo "Next: bash scripts/collect_wsl2_evidence.sh to run the full evidence collection suite"
