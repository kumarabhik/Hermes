#!/usr/bin/env bash
# smoke_schema.sh — Validate config/schema.yaml field names and value ranges.
#
# Parses schema.yaml (simple key: value format, no external YAML parser needed)
# and checks that all expected keys are present, no unknown keys exist, and
# numeric thresholds are within valid ranges.
#
# Does NOT require any Hermes binary to be built.
#
# Usage:
#   bash scripts/smoke_schema.sh
#   bash scripts/smoke_schema.sh config/schema.yaml
#   bash scripts/smoke_schema.sh --strict   (exit 1 on any warning)

set -euo pipefail

SCHEMA_FILE="${1:-config/schema.yaml}"
STRICT=0
[[ "${1:-}" == "--strict" ]] && { STRICT=1; SCHEMA_FILE="${2:-config/schema.yaml}"; }

PASS_COUNT=0
WARN_COUNT=0
FAIL_COUNT=0

pass() { echo "  [PASS] $*"; ((PASS_COUNT++)); }
warn() { echo "  [WARN] $*"; ((WARN_COUNT++)); }
fail() { echo "  [FAIL] $*" >&2; ((FAIL_COUNT++)); }

# Extract a value from the YAML (flat key: value or nested with 2-space indent).
get_val() {
    local key="$1"
    grep -E "^\s*${key}:" "$SCHEMA_FILE" 2>/dev/null | head -1 | sed 's/.*: *//'
}

echo "=================================================="
echo "Hermes schema.yaml smoke check: $SCHEMA_FILE"
echo "=================================================="

if [[ ! -f "$SCHEMA_FILE" ]]; then
    fail "File not found: $SCHEMA_FILE"
    echo ""; echo "FAIL count: $FAIL_COUNT"; exit 1
fi

pass "File exists: $SCHEMA_FILE"

# ---- Required top-level sections ----
for section in ups_weights thresholds cooldowns actions; do
    if grep -q "^${section}:" "$SCHEMA_FILE" 2>/dev/null; then
        pass "Section present: $section"
    else
        fail "Missing section: $section"
    fi
done

# ---- UPS weight keys and sum ----
echo ""
echo "-- UPS weights --"
W_CPU=$(get_val "cpu_some")
W_MEM_SOME=$(get_val "mem_some")
W_MEM_FULL=$(get_val "mem_full")
W_GPU_UTIL=$(get_val "gpu_util")
W_VRAM=$(get_val "vram_used")

for kv in "cpu_some:$W_CPU" "mem_some:$W_MEM_SOME" "mem_full:$W_MEM_FULL" \
          "gpu_util:$W_GPU_UTIL" "vram_used:$W_VRAM"; do
    k="${kv%%:*}"; v="${kv#*:}"
    if [[ -z "$v" ]]; then
        fail "Missing UPS weight: $k"
    else
        # Check range 0..1
        ok=$(python3 -c "v=float('${v}'); print('ok' if 0<v<=1 else 'bad')" 2>/dev/null || echo "bad")
        if [[ "$ok" == "ok" ]]; then
            pass "ups_weights.$k = $v  (in range 0..1)"
        else
            fail "ups_weights.$k = $v  (out of range 0..1)"
        fi
    fi
done

# Sum should be close to 1.0
if [[ -n "$W_CPU" && -n "$W_MEM_SOME" && -n "$W_MEM_FULL" && -n "$W_GPU_UTIL" && -n "$W_VRAM" ]]; then
    SUM=$(python3 -c "print(round(${W_CPU}+${W_MEM_SOME}+${W_MEM_FULL}+${W_GPU_UTIL}+${W_VRAM},4))" 2>/dev/null || echo "error")
    if python3 -c "import sys; v=float('${SUM}'); sys.exit(0 if abs(v-1.0)<0.01 else 1)" 2>/dev/null; then
        pass "ups_weights sum = $SUM  (close to 1.0)"
    else
        warn "ups_weights sum = $SUM  (expected ~1.0; weights don't sum correctly)"
    fi
fi

# ---- Threshold ranges ----
echo ""
echo "-- Thresholds --"

check_threshold() {
    local key="$1" min="$2" max="$3"
    local val
    val=$(get_val "$key")
    if [[ -z "$val" ]]; then
        fail "Missing threshold: $key"
        return
    fi
    ok=$(python3 -c "v=float('${val}'); print('ok' if ${min}<=v<=${max} else 'bad')" 2>/dev/null || echo "bad")
    if [[ "$ok" == "ok" ]]; then
        pass "thresholds.$key = $val  (in range [${min},${max}])"
    else
        fail "thresholds.$key = $val  (expected [${min},${max}])"
    fi
}

check_threshold "elevated" 20 90
check_threshold "critical" 40 100
check_threshold "elevated_full_avg10" 0.1 20
check_threshold "critical_full_avg10" 0.5 50
check_threshold "high_pct" 50 99
check_threshold "critical_pct" 60 100
check_threshold "high_risk" 0.3 0.99

# Critical must be > elevated
UPS_ELEV=$(get_val "elevated" | head -1)
UPS_CRIT=$(get_val "critical" | head -1)
if [[ -n "$UPS_ELEV" && -n "$UPS_CRIT" ]]; then
    ok=$(python3 -c "print('ok' if float('${UPS_CRIT}')>float('${UPS_ELEV}') else 'bad')" 2>/dev/null || echo "bad")
    if [[ "$ok" == "ok" ]]; then
        pass "ups.critical ($UPS_CRIT) > ups.elevated ($UPS_ELEV)"
    else
        fail "ups.critical ($UPS_CRIT) must be > ups.elevated ($UPS_ELEV)"
    fi
fi

VRAM_HIGH=$(get_val "high_pct")
VRAM_CRIT=$(get_val "critical_pct")
if [[ -n "$VRAM_HIGH" && -n "$VRAM_CRIT" ]]; then
    ok=$(python3 -c "print('ok' if float('${VRAM_CRIT}')>float('${VRAM_HIGH}') else 'bad')" 2>/dev/null || echo "bad")
    if [[ "$ok" == "ok" ]]; then
        pass "vram.critical_pct ($VRAM_CRIT) > vram.high_pct ($VRAM_HIGH)"
    else
        fail "vram.critical_pct ($VRAM_CRIT) must be > vram.high_pct ($VRAM_HIGH)"
    fi
fi

# ---- Cooldowns ----
echo ""
echo "-- Cooldowns --"
for key in level_1_pid_sec level_2_pid_sec level_3_global_sec recovery_hysteresis_sec; do
    val=$(get_val "$key")
    if [[ -z "$val" ]]; then
        warn "Missing cooldown: $key (will use compiled-in default)"
    else
        ok=$(python3 -c "v=float('${val}'); print('ok' if v>0 else 'bad')" 2>/dev/null || echo "bad")
        if [[ "$ok" == "ok" ]]; then
            pass "cooldowns.$key = $val"
        else
            fail "cooldowns.$key = $val  (must be > 0)"
        fi
    fi
done

# level_3 should be >= level_2 (safety margin)
L2=$(get_val "level_2_pid_sec")
L3=$(get_val "level_3_global_sec")
if [[ -n "$L2" && -n "$L3" ]]; then
    ok=$(python3 -c "print('ok' if float('${L3}')>=float('${L2}') else 'bad')" 2>/dev/null || echo "ok")
    if [[ "$ok" == "ok" ]]; then
        pass "level_3_global_sec ($L3) >= level_2_pid_sec ($L2)"
    else
        warn "level_3_global_sec ($L3) < level_2_pid_sec ($L2) — unusual; L3 is normally longer"
    fi
fi

# ---- Actions ----
echo ""
echo "-- Actions --"
for key in enable_level_1 enable_level_2 enable_level_3 observe_only_mode; do
    val=$(get_val "$key")
    if [[ -z "$val" ]]; then
        warn "Missing actions.$key"
    elif [[ "$val" == "true" || "$val" == "false" ]]; then
        pass "actions.$key = $val"
    else
        fail "actions.$key = $val  (must be true or false)"
    fi
done

# Safety: if observe_only_mode=false, warn that active mutations are enabled
OOM=$(get_val "observe_only_mode")
EL3=$(get_val "enable_level_3")
if [[ "$OOM" == "false" && "$EL3" == "true" ]]; then
    warn "observe_only_mode=false AND enable_level_3=true — Hermes will send SIGKILL in active mode"
fi

# ---- Unknown key detection ----
echo ""
echo "-- Unknown keys (informational) --"
KNOWN_KEYS="version ups_weights cpu_some mem_some mem_full gpu_util vram_used thresholds ups elevated critical mem_psi elevated_full_avg10 critical_full_avg10 vram high_pct critical_pct predictor high_risk cooldowns level_1_pid_sec level_2_pid_sec level_3_global_sec recovery_hysteresis_sec actions enable_level_1 enable_level_2 enable_level_3 observe_only_mode"

UNKNOWN=0
while IFS= read -r line; do
    key=$(echo "$line" | sed 's/:.*//' | tr -d ' ')
    [[ -z "$key" || "$key" == "#"* ]] && continue
    if ! echo "$KNOWN_KEYS" | grep -qw "$key"; then
        warn "Unknown key: $key  (may be a new field or typo)"
        ((UNKNOWN++))
    fi
done < "$SCHEMA_FILE"
[[ $UNKNOWN -eq 0 ]] && pass "No unknown keys found"

# ---- Summary ----
echo ""
echo "=================================================="
echo "Schema smoke result: PASS=$PASS_COUNT  WARN=$WARN_COUNT  FAIL=$FAIL_COUNT"
echo "=================================================="

if [[ $FAIL_COUNT -gt 0 ]]; then
    echo "FAIL — fix errors above before running Hermes."
    exit 1
elif [[ $STRICT -eq 1 && $WARN_COUNT -gt 0 ]]; then
    echo "FAIL (--strict) — warnings treated as errors."
    exit 1
else
    echo "PASS — schema is valid."
    exit 0
fi
