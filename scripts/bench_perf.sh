#!/usr/bin/env bash
# bench_perf.sh — Run a Hermes benchmark under perf stat and save the profile.
# Captures software performance counters around the full benchmark window.
# Saves perf output to artifacts/bench/<run-id>-perf.txt.
#
# Usage:
#   bash scripts/bench_perf.sh <scenario.yaml> [run-id] [artifact-root]
#
# Requirements: perf on PATH (sudo apt-get install linux-tools-common linux-tools-generic)
#               In WSL2, hardware counters (cycles, cache-misses) are unavailable.
#               Software counters (task-clock, context-switches, page-faults) always work.
#
# What it does:
#   1. Launches hermes_bench for the scenario (observe-only if hermesd present).
#   2. Wraps the benchmark invocation with perf stat -e software-events.
#   3. Saves the perf output and a parsed one-line finding to the artifact file.
#
# The saved perf stat output is the "one real perf finding" needed for the
# minimum defensibility package. Focus on:
#   - task-clock: total CPU time consumed
#   - context-switches: frequency of scheduler preemptions
#   - page-faults: minor + major fault rate (VRAM pressure indicator)
#   - major-faults: disk-backed page faults (memory pressure indicator)

set -euo pipefail

SCENARIO_FILE="${1:-}"
RUN_ID="${2:-bench-perf-$(date +%Y%m%d-%H%M%S)}"
ARTIFACT_ROOT="${3:-artifacts}"
BENCH_DIR="${ARTIFACT_ROOT}/bench"
BUILD_DIR="$(dirname "$0")/../build"

if [[ -z "$SCENARIO_FILE" ]]; then
    echo "Usage: $0 <scenario.yaml> [run-id] [artifact-root]" >&2
    exit 1
fi

if ! command -v perf &>/dev/null; then
    echo "ERROR: perf not found. Install with: sudo apt-get install linux-tools-common linux-tools-generic" >&2
    exit 1
fi

HERMES_BENCH="${BUILD_DIR}/hermes_bench"
HERMESD="${BUILD_DIR}/hermesd"
HERMES_REPLAY="${BUILD_DIR}/hermes_replay"

if [[ ! -x "$HERMES_BENCH" ]]; then
    echo "ERROR: hermes_bench not found at $HERMES_BENCH. Run cmake --build build first." >&2
    exit 1
fi

mkdir -p "$BENCH_DIR"
PERF_OUT="${BENCH_DIR}/${RUN_ID}-perf.txt"

echo "=== Hermes bench_perf ==="
echo "Scenario    : $SCENARIO_FILE"
echo "Run ID      : $RUN_ID"
echo "Perf out    : $PERF_OUT"

# Software counters that work in WSL2 and bare-metal Linux.
PERF_EVENTS="task-clock,context-switches,cpu-migrations,page-faults,minor-faults,major-faults"

# Build the hermes_bench command.
BENCH_ARGS=("$SCENARIO_FILE" "--artifact-root" "$ARTIFACT_ROOT" "--run-id" "${RUN_ID}-bench")
if [[ -x "$HERMESD" && -x "$HERMES_REPLAY" ]]; then
    BENCH_ARGS+=("--hermes-bin" "$HERMESD" "--replay-bin" "$HERMES_REPLAY")
fi

{
    echo "# perf stat output for hermes_bench"
    echo "# Scenario: $SCENARIO_FILE"
    echo "# Run ID: $RUN_ID"
    echo "# Date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "# Events: $PERF_EVENTS"
    echo ""
    perf stat -e "$PERF_EVENTS" -- "$HERMES_BENCH" "${BENCH_ARGS[@]}" 2>&1 || true
    echo ""
    echo "# --- Hermes bench_perf findings ---"
    echo "# Interpret the output above:"
    echo "#   task-clock (msec)   : total wall-clock CPU time for the benchmark process"
    echo "#   context-switches    : scheduler preemptions — high count = contention"
    echo "#   page-faults         : total minor + major faults during benchmark window"
    echo "#   major-faults        : disk-backed faults — nonzero indicates memory pressure"
} > "$PERF_OUT" 2>&1

echo "perf output written to: $PERF_OUT"
echo ""
echo "=== bench_perf DONE ==="
echo "Evidence artifacts:"
echo "  $PERF_OUT"
echo "  ${BENCH_DIR}/${RUN_ID}-bench-summary.json"
