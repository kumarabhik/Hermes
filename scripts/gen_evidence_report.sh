#!/usr/bin/env bash
# gen_evidence_report.sh — One-shot evidence summary generator.
#
# Runs hermes_plot.py --summary, hermes_report, and hermes_tune.py in sequence
# and writes a combined plain-text evidence report to artifacts/evidence_report.txt.
#
# Also prints check_evidence_tiers.py tier status.
#
# Usage:
#   bash scripts/gen_evidence_report.sh
#   bash scripts/gen_evidence_report.sh --artifact-root artifacts --build-dir build

set -uo pipefail

ARTIFACT_ROOT="${ARTIFACT_ROOT:-artifacts}"
BUILD_DIR="build"

for arg in "$@"; do
    case "$arg" in
        --artifact-root) ARTIFACT_ROOT="${2:-artifacts}"; shift 2 || true ;;
        --build-dir)     BUILD_DIR="${2:-build}"; shift 2 || true ;;
    esac
done

REPORT_FILE="${ARTIFACT_ROOT}/evidence_report.txt"
mkdir -p "$ARTIFACT_ROOT"

TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')

echo "======================================================"
echo " Hermes Evidence Report Generator"
echo " Artifact root : $ARTIFACT_ROOT"
echo " Output        : $REPORT_FILE"
echo "======================================================"
echo ""

{
    echo "Hermes Evidence Report"
    echo "Generated: $TIMESTAMP"
    echo "Artifact root: $ARTIFACT_ROOT"
    echo ""
    echo "======================================================"
    echo "SECTION 1 — Evidence Tier Status"
    echo "======================================================"
} > "$REPORT_FILE"

# 1. Evidence tier status
if python3 scripts/check_evidence_tiers.py --artifacts-dir "$ARTIFACT_ROOT" 2>&1 | tee -a "$REPORT_FILE"; then
    echo ""
else
    echo "[WARN] check_evidence_tiers.py returned non-zero (some tiers not met)"
fi

# 2. hermes_plot --summary for all run dirs
{
    echo ""
    echo "======================================================"
    echo "SECTION 2 — Run Summaries (hermes_plot.py --summary)"
    echo "======================================================"
} >> "$REPORT_FILE"

RUN_COUNT=0
for run_dir in "${ARTIFACT_ROOT}/logs"/*/; do
    [[ -d "$run_dir" ]] || continue
    ndjson="${run_dir}samples.ndjson"
    [[ -f "$ndjson" ]] || ndjson="${run_dir}decisions.ndjson"
    [[ -f "$ndjson" ]] || continue
    echo "  Summarising: $run_dir"
    {
        echo ""
        echo "--- $run_dir ---"
        python3 scripts/hermes_plot.py --summary "$run_dir" 2>&1 || echo "[WARN] hermes_plot.py failed for $run_dir"
    } >> "$REPORT_FILE"
    ((RUN_COUNT++))
done

if [[ $RUN_COUNT -eq 0 ]]; then
    echo "  [SKIP] No run directories with NDJSON artifacts found." | tee -a "$REPORT_FILE"
fi

# 3. hermes_report (multi-run comparison)
{
    echo ""
    echo "======================================================"
    echo "SECTION 3 — Multi-run Comparison (hermes_report)"
    echo "======================================================"
} >> "$REPORT_FILE"

REPORT_BIN="${BUILD_DIR}/hermes_report"
if [[ -x "$REPORT_BIN" ]]; then
    echo "  Running hermes_report..."
    HERMES_ARTIFACT_ROOT="$ARTIFACT_ROOT" "$REPORT_BIN" 2>&1 | tee -a "$REPORT_FILE" || \
        echo "[WARN] hermes_report returned non-zero" | tee -a "$REPORT_FILE"
else
    echo "  [SKIP] hermes_report not found at $REPORT_BIN" | tee -a "$REPORT_FILE"
fi

# 4. hermes_tune.py calibration check
{
    echo ""
    echo "======================================================"
    echo "SECTION 4 — Predictor Calibration (hermes_tune.py)"
    echo "======================================================"
} >> "$REPORT_FILE"

if python3 scripts/hermes_tune.py --eval-dir "$ARTIFACT_ROOT/logs" 2>&1 | tee -a "$REPORT_FILE"; then
    echo ""
else
    echo "  [INFO] hermes_tune.py: not all targets met (see above)" | tee -a "$REPORT_FILE"
fi

echo ""
echo "======================================================"
echo " Report written to: $REPORT_FILE"
echo "======================================================"
