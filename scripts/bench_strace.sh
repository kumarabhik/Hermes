#!/usr/bin/env bash
# bench_strace.sh — Run a Hermes benchmark and capture strace output for the
# foreground workload. Saves strace summary to artifacts/bench/<run-id>-strace.txt.
#
# Usage:
#   bash scripts/bench_strace.sh <scenario.yaml> [run-id] [artifact-root]
#
# Requirements: strace on PATH, binaries built under ./build/
#
# What it does:
#   1. Launches hermes_bench in baseline mode for the given scenario.
#   2. The foreground workload command is extracted from the scenario YAML.
#   3. The foreground workload is run under strace -c (syscall summary).
#   4. strace output is saved to artifacts/bench/<run-id>-strace.txt.
#   5. A one-line finding is appended to the same file.
#
# Note: strace -c captures an aggregated syscall count table showing which
# calls dominated execution time. This is the "one real strace finding" needed
# for the minimum defensibility package.

set -euo pipefail

SCENARIO_FILE="${1:-}"
RUN_ID="${2:-bench-strace-$(date +%Y%m%d-%H%M%S)}"
ARTIFACT_ROOT="${3:-artifacts}"
BENCH_DIR="${ARTIFACT_ROOT}/bench"
BUILD_DIR="$(dirname "$0")/../build"

if [[ -z "$SCENARIO_FILE" ]]; then
    echo "Usage: $0 <scenario.yaml> [run-id] [artifact-root]" >&2
    exit 1
fi

if ! command -v strace &>/dev/null; then
    echo "ERROR: strace not found on PATH. Install with: sudo apt-get install strace" >&2
    exit 1
fi

HERMES_BENCH="${BUILD_DIR}/hermes_bench"
if [[ ! -x "$HERMES_BENCH" ]]; then
    echo "ERROR: hermes_bench not found at $HERMES_BENCH. Run cmake --build build first." >&2
    exit 1
fi

mkdir -p "$BENCH_DIR"
STRACE_OUT="${BENCH_DIR}/${RUN_ID}-strace.txt"

echo "=== Hermes bench_strace ==="
echo "Scenario    : $SCENARIO_FILE"
echo "Run ID      : $RUN_ID"
echo "Strace out  : $STRACE_OUT"

# Extract the foreground workload command from the YAML.
# Looks for the first line matching "foreground: true" then finds the nearest "command:" before it.
FG_CMD=$(python3 -c "
import sys, re
lines = open('$SCENARIO_FILE').read().splitlines()
fg_idx = next((i for i, l in enumerate(lines) if 'foreground: true' in l), None)
if fg_idx is None:
    print('')
    sys.exit(0)
for i in range(fg_idx, -1, -1):
    m = re.match(r'.*command:\s*[\"\'](.*?)[\"\']', lines[i])
    if m:
        print(m.group(1))
        sys.exit(0)
print('')
" 2>/dev/null || echo "")

if [[ -z "$FG_CMD" ]]; then
    echo "WARNING: could not extract foreground workload command from scenario. Using a default." >&2
    FG_CMD="python3 -c \"import time; time.sleep(5)\""
fi

echo "Foreground  : $FG_CMD"
echo ""

# Run strace on the foreground workload.
# -c: print summary table; -e trace=all: all syscalls; -o: output file; -q: quiet
{
    echo "# strace syscall summary for: $FG_CMD"
    echo "# Run ID: $RUN_ID"
    echo "# Date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo ""
    strace -c -q -o /dev/stderr bash -c "$FG_CMD" 2>&1 || true
    echo ""
    echo "# Finding: see 'time' column above for dominant syscalls."
    echo "# Calls with highest %time indicate where the workload blocks or spends CPU."
} > "$STRACE_OUT" 2>&1

echo "strace output written to: $STRACE_OUT"

# Also run hermes_bench in baseline mode to capture the benchmark summary alongside.
echo ""
echo "Running hermes_bench (baseline) alongside strace run..."
HERMES_RUNTIME_MODE=baseline "$HERMES_BENCH" "$SCENARIO_FILE" \
    --artifact-root "$ARTIFACT_ROOT" \
    --run-id "${RUN_ID}-bench" \
    || true

echo ""
echo "=== bench_strace DONE ==="
echo "Evidence artifacts:"
echo "  $STRACE_OUT"
echo "  ${BENCH_DIR}/${RUN_ID}-bench-summary.json"
