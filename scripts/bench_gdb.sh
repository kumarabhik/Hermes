#!/usr/bin/env bash
# bench_gdb.sh — Enable core dumps, run a workload that may crash or be killed,
# then extract a gdb backtrace from any resulting core file.
# Saves the backtrace to artifacts/bench/<run-id>-gdb.txt.
#
# Usage:
#   bash scripts/bench_gdb.sh <workload-command> [run-id] [artifact-root]
#
# Requirements: gdb on PATH (sudo apt-get install gdb)
#
# What it does:
#   1. Sets ulimit -c unlimited to enable core dumps.
#   2. Sets /proc/sys/kernel/core_pattern to write cores to artifacts/bench/.
#   3. Runs the given workload command.
#   4. If the process exits with a signal (exit code >= 128) and a core file exists,
#      runs gdb --batch to extract the backtrace and saves it.
#   5. If no core file is found, records the exit code and notes for the evidence file.
#
# Example — trigger a near-OOM and capture what killed it:
#   bash scripts/bench_gdb.sh "stress-ng --vm 2 --vm-bytes 95% --timeout 30s"
#
# The saved gdb output is the "one gdb analysis" needed for the extended defensibility
# package. Even a clean "no core / exit 0" run produces a useful evidence artifact.

set -euo pipefail

WORKLOAD_CMD="${1:-}"
RUN_ID="${2:-bench-gdb-$(date +%Y%m%d-%H%M%S)}"
ARTIFACT_ROOT="${3:-artifacts}"
BENCH_DIR="${ARTIFACT_ROOT}/bench"

if [[ -z "$WORKLOAD_CMD" ]]; then
    echo "Usage: $0 <workload-command> [run-id] [artifact-root]" >&2
    echo "Example: $0 \"stress-ng --vm 1 --vm-bytes 80% --timeout 20s\" my-run-01" >&2
    exit 1
fi

mkdir -p "$BENCH_DIR"
GDB_OUT="${BENCH_DIR}/${RUN_ID}-gdb.txt"
CORE_PATTERN="${BENCH_DIR}/core.${RUN_ID}.%p"

echo "=== Hermes bench_gdb ==="
echo "Workload    : $WORKLOAD_CMD"
echo "Run ID      : $RUN_ID"
echo "GDB out     : $GDB_OUT"

# Enable core dumps.
ulimit -c unlimited 2>/dev/null || echo "WARNING: could not set ulimit -c unlimited (may need root)"

# Set core pattern if writable (requires root or CAP_SYS_ADMIN on some systems).
if [[ -w /proc/sys/kernel/core_pattern ]]; then
    echo "$CORE_PATTERN" > /proc/sys/kernel/core_pattern
    echo "Core pattern: $CORE_PATTERN"
else
    echo "WARNING: /proc/sys/kernel/core_pattern not writable. Core files may land in CWD."
    CORE_PATTERN="core"
fi

{
    echo "# Hermes bench_gdb evidence"
    echo "# Workload: $WORKLOAD_CMD"
    echo "# Run ID: $RUN_ID"
    echo "# Date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo ""

    EXIT_CODE=0
    bash -c "$WORKLOAD_CMD" || EXIT_CODE=$?

    echo "# Workload exit code: $EXIT_CODE"
    if (( EXIT_CODE >= 128 )); then
        SIG=$(( EXIT_CODE - 128 ))
        echo "# Workload killed by signal $SIG"
        case $SIG in
            9)  echo "# Signal 9 = SIGKILL (OOM killer or manual kill)" ;;
            11) echo "# Signal 11 = SIGSEGV (segmentation fault)" ;;
            6)  echo "# Signal 6 = SIGABRT (assert/abort)" ;;
            *)  echo "# Signal $SIG" ;;
        esac
    fi
    echo ""

    # Look for a core file.
    CORE_FILE=""
    for CANDIDATE in "${BENCH_DIR}/core.${RUN_ID}."* core "core.*"; do
        if [[ -f "$CANDIDATE" ]]; then
            CORE_FILE="$CANDIDATE"
            break
        fi
    done

    if [[ -n "$CORE_FILE" ]] && command -v gdb &>/dev/null; then
        echo "# Core file found: $CORE_FILE"
        echo "# Running gdb backtrace..."
        echo ""
        # Extract the binary name from the workload command (first word).
        BINARY=$(echo "$WORKLOAD_CMD" | awk '{print $1}')
        BINARY_PATH=$(command -v "$BINARY" 2>/dev/null || echo "$BINARY")
        gdb --batch \
            -ex "file $BINARY_PATH" \
            -ex "core-file $CORE_FILE" \
            -ex "bt full" \
            -ex "info registers" \
            -ex "quit" \
            2>&1 || true
        echo ""
        echo "# gdb analysis complete. See backtrace above."
    elif command -v gdb &>/dev/null; then
        echo "# No core file found (process may have exited cleanly or core dumps disabled)."
        echo "# Exit code $EXIT_CODE does not indicate a crash requiring gdb analysis."
        echo "# To force a crash for testing: ulimit -c unlimited && kill -SIGSEGV \$\$"
    else
        echo "# gdb not found. Install with: sudo apt-get install gdb"
    fi
} > "$GDB_OUT" 2>&1

echo "gdb evidence written to: $GDB_OUT"
echo ""
echo "=== bench_gdb DONE ==="
echo "Evidence artifacts:"
echo "  $GDB_OUT"
