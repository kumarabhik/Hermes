#!/usr/bin/env bash
# hermes_doctor.sh — Host readiness diagnostic for Hermes.
#
# Checks whether the current host satisfies the requirements to run each tier
# of Hermes operations and prints a colour-coded readiness table.
#
# Tier A (WSL2 / Linux VM):     /proc basics, Python 3, build tools
# Tier B (Native Linux, no GPU): PSI support, stress-ng, cgroup v2
# Tier C (Linux + NVIDIA GPU):   NVML / CUDA, nvidia-smi
#
# Usage:
#   bash scripts/hermes_doctor.sh
#   bash scripts/hermes_doctor.sh --build-dir build   # also checks binaries
#   bash scripts/hermes_doctor.sh --quiet             # only print failures

set -uo pipefail

BUILD_DIR="build"
QUIET=0

for arg in "$@"; do
    case "$arg" in
        --build-dir) BUILD_DIR="${2:-build}"; shift 2 || true ;;
        --quiet) QUIET=1 ;;
    esac
done

# ---- colour helpers ----
RED='\033[0;31m'
GRN='\033[0;32m'
YEL='\033[1;33m'
DIM='\033[2m'
NC='\033[0m'

PASS_COUNT=0
WARN_COUNT=0
FAIL_COUNT=0

check() {
    local label="$1"
    local result="$2"   # pass | warn | fail
    local detail="$3"

    case "$result" in
        pass) ((PASS_COUNT++)); [[ $QUIET -eq 1 ]] && return
              printf "  ${GRN}[PASS]${NC} %-42s ${DIM}%s${NC}\n" "$label" "$detail" ;;
        warn) ((WARN_COUNT++))
              printf "  ${YEL}[WARN]${NC} %-42s ${DIM}%s${NC}\n" "$label" "$detail" ;;
        fail) ((FAIL_COUNT++))
              printf "  ${RED}[FAIL]${NC} %-42s ${DIM}%s${NC}\n" "$label" "$detail" ;;
    esac
}

section() { echo ""; echo "── $* ──"; }

echo "======================================================"
echo " Hermes Host Readiness Diagnostic"
echo "======================================================"
uname -a 2>/dev/null || true
echo ""

# ------------------------------------------------------------------ Tier A
section "Tier A — Build and /proc basics"

# OS
if [[ "$(uname -s)" == "Linux" ]]; then
    check "OS: Linux" pass "$(uname -sr)"
else
    check "OS: Linux" fail "$(uname -s) detected — Hermes runtime requires Linux"
fi

# /proc
if [[ -d /proc ]]; then
    check "/proc filesystem" pass "present"
else
    check "/proc filesystem" fail "not found — required for all monitors"
fi

# /proc/loadavg
if [[ -f /proc/loadavg ]]; then
    check "/proc/loadavg" pass "$(cat /proc/loadavg)"
else
    check "/proc/loadavg" fail "not readable"
fi

# Python 3
if command -v python3 &>/dev/null; then
    PY_VER=$(python3 --version 2>&1)
    check "python3" pass "$PY_VER"
else
    check "python3" fail "not found — required for fidelity workloads and scripts"
fi

# g++ (direct build path)
if command -v g++ &>/dev/null; then
    GCC_VER=$(g++ --version | head -1)
    check "g++" pass "$GCC_VER"
elif command -v clang++ &>/dev/null; then
    CLANG_VER=$(clang++ --version | head -1)
    check "clang++" pass "$CLANG_VER"
else
    check "C++ compiler (g++ or clang++)" warn "not found — needed for direct build"
fi

# cmake
if command -v cmake &>/dev/null; then
    CMAKE_VER=$(cmake --version | head -1)
    check "cmake" pass "$CMAKE_VER"
else
    check "cmake" warn "not found — needed for CMake build (direct g++ build still works)"
fi

# ------------------------------------------------------------------ Tier B
section "Tier B — PSI, cgroup v2, stress tooling"

# PSI cpu
if [[ -f /proc/pressure/cpu ]]; then
    check "PSI: /proc/pressure/cpu" pass "$(head -1 /proc/pressure/cpu)"
else
    check "PSI: /proc/pressure/cpu" fail "not present — kernel 4.20+ required for PSI monitors"
fi

# PSI memory
if [[ -f /proc/pressure/memory ]]; then
    check "PSI: /proc/pressure/memory" pass "$(head -1 /proc/pressure/memory)"
else
    check "PSI: /proc/pressure/memory" fail "not present"
fi

# PSI io
if [[ -f /proc/pressure/io ]]; then
    check "PSI: /proc/pressure/io" pass "$(head -1 /proc/pressure/io)"
else
    check "PSI: /proc/pressure/io" warn "not present — IO PSI monitor will be skipped"
fi

# cgroup v2
if [[ -f /sys/fs/cgroup/cgroup.controllers ]]; then
    CONTROLLERS=$(cat /sys/fs/cgroup/cgroup.controllers 2>/dev/null)
    check "cgroup v2 mounted" pass "controllers: $CONTROLLERS"
    if echo "$CONTROLLERS" | grep -q "memory"; then
        check "cgroup v2: memory controller" pass ""
    else
        check "cgroup v2: memory controller" warn "not available — memory.high throttling won't work"
    fi
    if echo "$CONTROLLERS" | grep -q "cpu"; then
        check "cgroup v2: cpu controller" pass ""
    else
        check "cgroup v2: cpu controller" warn "not available — cpu.max throttling won't work"
    fi
else
    check "cgroup v2 mounted" warn "not found — cgroup backend requires cgroup v2 at /sys/fs/cgroup"
fi

# stress-ng
if command -v stress-ng &>/dev/null; then
    STRESS_VER=$(stress-ng --version 2>&1 | head -1)
    check "stress-ng" pass "$STRESS_VER"
else
    check "stress-ng" warn "not found — install: apt install stress-ng (needed for fidelity workloads)"
fi

# perf
if command -v perf &>/dev/null; then
    PERF_VER=$(perf --version 2>&1 | head -1)
    # Check if perf can actually run (paranoid setting)
    PARANOID=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo "unknown")
    if [[ "$PARANOID" != "unknown" && "$PARANOID" -gt 2 ]]; then
        check "perf" warn "$PERF_VER — perf_event_paranoid=$PARANOID (may block event capture)"
    else
        check "perf" pass "$PERF_VER — perf_event_paranoid=$PARANOID"
    fi
else
    check "perf" warn "not found — install: apt install linux-perf (needed for T5 evidence)"
fi

# strace
if command -v strace &>/dev/null; then
    STRACE_VER=$(strace --version 2>&1 | head -1)
    check "strace" pass "$STRACE_VER"
else
    check "strace" warn "not found — install: apt install strace (needed for T5 evidence)"
fi

# gdb
if command -v gdb &>/dev/null; then
    GDB_VER=$(gdb --version 2>&1 | head -1)
    check "gdb" pass "$GDB_VER"
else
    check "gdb" warn "not found — install: apt install gdb (optional, extended defensibility)"
fi

# ------------------------------------------------------------------ Tier C
section "Tier C — NVIDIA GPU / NVML"

# nvidia-smi
if command -v nvidia-smi &>/dev/null; then
    SMI_OUT=$(nvidia-smi --query-gpu=name,memory.total --format=csv,noheader 2>/dev/null | head -1)
    if [[ -n "$SMI_OUT" ]]; then
        check "nvidia-smi" pass "$SMI_OUT"
    else
        check "nvidia-smi" warn "binary found but no GPU reported"
    fi
else
    check "nvidia-smi" warn "not found — GPU monitoring will use fallback path"
fi

# NVML library
NVML_FOUND=0
for lib_path in /usr/lib/x86_64-linux-gnu/libnvidia-ml.so.1 \
                /usr/lib/libnvidia-ml.so.1 \
                /usr/local/cuda/lib64/libnvidia-ml.so.1; do
    if [[ -f "$lib_path" ]]; then
        check "libnvidia-ml.so.1" pass "$lib_path"
        NVML_FOUND=1
        break
    fi
done
if [[ $NVML_FOUND -eq 0 ]]; then
    check "libnvidia-ml.so.1" warn "not found in standard paths — NVML fast path will be unavailable"
fi

# PyTorch CUDA (Tier C inference workloads)
if python3 -c "import torch; assert torch.cuda.is_available(), 'no CUDA'" 2>/dev/null; then
    TORCH_VER=$(python3 -c "import torch; print(torch.__version__)" 2>/dev/null)
    CUDA_VER=$(python3 -c "import torch; print(torch.version.cuda)" 2>/dev/null)
    check "PyTorch CUDA" pass "torch $TORCH_VER, CUDA $CUDA_VER"
elif python3 -c "import torch" 2>/dev/null; then
    check "PyTorch CUDA" warn "torch installed but CUDA not available — GPU inference loop needs CUDA"
else
    check "PyTorch CUDA" warn "torch not installed — GPU inference loop (Tier C) will not run"
fi

# ------------------------------------------------------------------ Hermes binaries
section "Hermes binaries (--build-dir $BUILD_DIR)"

for bin in hermesd hermesd_mt hermes_replay hermes_eval hermes_bench hermes_compare hermesctl; do
    BIN_PATH="${BUILD_DIR}/${bin}"
    if [[ -x "$BIN_PATH" ]]; then
        SIZE=$(du -sh "$BIN_PATH" 2>/dev/null | cut -f1)
        check "$bin" pass "$BIN_PATH ($SIZE)"
    else
        check "$bin" warn "not found at $BIN_PATH — run: cmake --build $BUILD_DIR"
    fi
done

# ------------------------------------------------------------------ Summary
echo ""
echo "======================================================"
echo " Summary"
echo "======================================================"
printf "  ${GRN}PASS${NC}: %d  ${YEL}WARN${NC}: %d  ${RED}FAIL${NC}: %d\n" \
    "$PASS_COUNT" "$WARN_COUNT" "$FAIL_COUNT"
echo ""

if [[ $FAIL_COUNT -gt 0 ]]; then
    echo "  Status: NOT READY — fix FAIL items before running benchmarks."
    TIER="none"
elif [[ $WARN_COUNT -eq 0 ]]; then
    echo "  Status: FULLY READY — all Tier A/B/C capabilities available."
    TIER="C"
else
    # Check which tier we can run
    PSI_OK=0
    [[ -f /proc/pressure/cpu ]] && PSI_OK=1
    GPU_OK=0
    command -v nvidia-smi &>/dev/null && GPU_OK=1
    if [[ $PSI_OK -eq 1 && $GPU_OK -eq 1 ]]; then
        echo "  Status: READY (Tier B/C) — PSI and GPU available; some optional tools missing."
        TIER="B/C"
    elif [[ $PSI_OK -eq 1 ]]; then
        echo "  Status: READY (Tier B) — PSI available; GPU/CUDA tools missing (WARN)."
        TIER="B"
    else
        echo "  Status: READY (Tier A) — basic /proc available; PSI/GPU missing (WARN)."
        TIER="A"
    fi
fi

echo ""
echo "  Next step:"
if [[ "${TIER:-none}" == "none" ]]; then
    echo "    Fix FAIL items, then re-run this script."
elif [[ "$TIER" == "A" ]]; then
    echo "    Install stress-ng for Tier B: apt install stress-ng"
    echo "    Then run: bash scripts/smoke_wsl2.sh"
else
    echo "    Run: bash scripts/smoke_phase6.sh"
    echo "    Then: python3 scripts/check_evidence_tiers.py"
fi
echo "======================================================"
exit $FAIL_COUNT
