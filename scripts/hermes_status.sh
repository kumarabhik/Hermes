#!/usr/bin/env bash
# hermes_status.sh — Quick operational status check for a running Hermes deployment.
#
# Checks: daemon running, socket health, latest run artifacts, peak UPS from
# the most recent run, hermes_pack bundle availability, and binary versions.
#
# Usage:
#   bash scripts/hermes_status.sh
#   bash scripts/hermes_status.sh --socket /tmp/hermesd.sock
#   bash scripts/hermes_status.sh --artifact-root /var/hermes/artifacts

set -euo pipefail

SOCKET="${HERMES_SOCKET_PATH:-/tmp/hermesd.sock}"
ARTIFACT_ROOT="${HERMES_ARTIFACT_ROOT:-artifacts}"
BUILD_DIR="${BUILD_DIR:-build}"

for arg in "$@"; do
    case "$arg" in
        --socket)         shift; SOCKET="$1" ;;
        --artifact-root)  shift; ARTIFACT_ROOT="$1" ;;
        --build-dir)      shift; BUILD_DIR="$1" ;;
        --help)
            echo "Usage: bash scripts/hermes_status.sh [--socket PATH] [--artifact-root PATH]"
            exit 0
            ;;
    esac
done

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

ok()   { echo -e "  ${GREEN}[ok]${NC}   $1"; }
warn() { echo -e "  ${YELLOW}[warn]${NC} $1"; }
fail() { echo -e "  ${RED}[fail]${NC} $1"; }
info() { echo -e "  ${CYAN}[info]${NC} $1"; }

echo ""
echo -e "${CYAN}=== Hermes Status ===${NC}"
echo "Time        : $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
echo "Socket      : $SOCKET"
echo "Artifacts   : $ARTIFACT_ROOT"
echo ""

# ── Daemon check ─────────────────────────────────────────────────────────────
echo "--- Daemon ---"

if [[ -S "$SOCKET" ]]; then
    ok "Socket exists: $SOCKET"
    if command -v nc &>/dev/null; then
        PING_RESP=$(echo '{"kind":"ping"}' | nc -U "$SOCKET" -q1 2>/dev/null || true)
        if [[ "$PING_RESP" == *"pong"* ]]; then
            ok "Daemon responding to ping"
            STATUS_RESP=$(echo '{"kind":"status"}' | nc -U "$SOCKET" -q1 2>/dev/null || true)
            if [[ -n "$STATUS_RESP" ]]; then
                UPS_LIVE=$(echo "$STATUS_RESP" | grep -oP '"ups":\K[0-9.]+' | head -1 || echo "?")
                STATE_LIVE=$(echo "$STATUS_RESP" | grep -oP '"scheduler_state":"\K[^"]+' | head -1 || echo "?")
                info "Live UPS=$UPS_LIVE  State=$STATE_LIVE"
            fi
        else
            warn "Socket exists but daemon not responding to ping"
        fi
    else
        warn "nc not available — cannot probe socket (install netcat)"
    fi
else
    fail "Socket not found: $SOCKET  (is hermesd running?)"
fi

# Check process table for hermesd.
if pgrep -x hermesd &>/dev/null; then
    HERMESD_PID=$(pgrep -x hermesd | head -1)
    ok "hermesd process running (PID $HERMESD_PID)"
else
    fail "hermesd process not found in process table"
fi

echo ""

# ── Latest run artifacts ──────────────────────────────────────────────────────
echo "--- Latest Run ---"

LOGS_DIR="$ARTIFACT_ROOT/logs"
if [[ ! -d "$LOGS_DIR" ]]; then
    warn "Logs directory not found: $LOGS_DIR"
else
    LATEST_RUN=$(ls -1td "$LOGS_DIR"/*/ 2>/dev/null | head -1 | sed 's|/$||')
    if [[ -z "$LATEST_RUN" ]]; then
        warn "No run directories in $LOGS_DIR"
    else
        RUN_ID=$(basename "$LATEST_RUN")
        info "Latest run: $RUN_ID"

        for artifact in samples.ndjson scores.ndjson decisions.ndjson actions.ndjson run_metadata.json telemetry_quality.json; do
            if [[ -f "$LATEST_RUN/$artifact" ]]; then
                SIZE=$(stat -c%s "$LATEST_RUN/$artifact" 2>/dev/null || echo "?")
                ok "$artifact ($SIZE bytes)"
            else
                warn "$artifact missing"
            fi
        done

        # Peak UPS.
        if [[ -f "$LATEST_RUN/telemetry_quality.json" ]]; then
            PEAK_UPS=$(grep -oP '"peak_ups":\K[0-9.]+' "$LATEST_RUN/telemetry_quality.json" 2>/dev/null || echo "?")
            info "Peak UPS in latest run: $PEAK_UPS"
        fi

        # Replay summary validity.
        if [[ -f "$LATEST_RUN/replay_summary.json" ]]; then
            if grep -q '"valid":true' "$LATEST_RUN/replay_summary.json"; then
                ok "replay_summary.json valid"
            else
                warn "replay_summary.json present but not valid"
            fi
        else
            warn "replay_summary.json not present (run hermes_replay to generate)"
        fi

        # Evidence bundle.
        BUNDLE_DIR="$ARTIFACT_ROOT/evidence_bundles/$RUN_ID"
        if [[ -f "$BUNDLE_DIR/bundle_manifest.json" ]]; then
            ok "Evidence bundle present: $BUNDLE_DIR"
        else
            warn "No evidence bundle (run: hermes_pack $LATEST_RUN)"
        fi
    fi
fi

echo ""

# ── PSI availability ──────────────────────────────────────────────────────────
echo "--- System ---"

if [[ -f /proc/pressure/cpu ]]; then
    CPU_PSI_VAL=$(awk '{print $1}' /proc/pressure/cpu 2>/dev/null | head -1)
    ok "CPU PSI: $CPU_PSI_VAL"
else
    warn "/proc/pressure/cpu not available (kernel < 4.20 or CONFIG_PSI not set)"
fi

if [[ -f /proc/pressure/memory ]]; then
    ok "Memory PSI available"
else
    warn "Memory PSI not available"
fi

# NVML / GPU check.
if command -v nvidia-smi &>/dev/null; then
    GPU_NAME=$(nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | head -1 || echo "unknown")
    ok "GPU: $GPU_NAME (via nvidia-smi)"
else
    warn "nvidia-smi not found — GPU stats unavailable"
fi

echo ""

# ── Binaries ──────────────────────────────────────────────────────────────────
echo "--- Binaries ---"

for bin in hermesd hermesctl hermes_replay hermes_pack hermes_journal hermes_simulate hermes_bench; do
    if command -v "$bin" &>/dev/null || [[ -x "$BUILD_DIR/$bin" ]]; then
        ok "$bin found"
    else
        warn "$bin not in PATH or $BUILD_DIR/"
    fi
done

echo ""
echo -e "${CYAN}=== Done ===${NC}"
echo "For live monitoring: hermesctl --socket $SOCKET"
echo "For run history:     hermesctl history"
echo "For a run journal:   hermes_journal $ARTIFACT_ROOT/logs/<run_id>"
echo ""
