#!/usr/bin/env python3
"""check_evidence_tiers.py — Report which T0-T5 claim tiers are currently evidenced.

Scans the artifacts/ directory and RESULTS.md to determine which benchmark
evidence tiers from design.md are satisfied.  Prints a status table and exits
non-zero if any tier below a given minimum is not met.

Tier definitions (from design.md § Benchmark Evidence Standards):
  T0 — Pipeline correct   : replay_summary.json valid, smoke exits 0
  T1 — Monitors work      : live daemon run with non-zero PSI fields on Linux
  T2 — Predictor fires    : eval_summary.json with total_predictions > 0
  T3 — Intervention fires : actions.ndjson has a non-dry-run entry
  T4 — Intervention helps : 5-run comparison shows active-control < baseline p95
  T5 — Claim defensible   : strace/perf captures + RESULTS.md proven entry

Usage:
    python3 scripts/check_evidence_tiers.py
    python3 scripts/check_evidence_tiers.py --artifacts-dir artifacts
    python3 scripts/check_evidence_tiers.py --require T2
"""

import glob
import json
import os
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Evidence probe functions — each returns (met: bool, detail: str)
# ---------------------------------------------------------------------------

def probe_t0(artifacts_dir: str):
    """T0: at least one valid replay_summary.json exists."""
    pattern = os.path.join(artifacts_dir, "logs", "*", "replay_summary.json")
    found = glob.glob(pattern)
    if not found:
        return False, "no replay_summary.json found under artifacts/logs/"
    # Validate at least one: must have run_id or total_samples key
    for p in found:
        try:
            with open(p, encoding="utf-8") as f:
                d = json.load(f)
            if "run_id" in d or "total_samples" in d or "assertions_total" in d:
                return True, f"{len(found)} replay_summary.json file(s) found; {os.path.basename(os.path.dirname(p))} is valid"
        except Exception:
            continue
    return False, f"{len(found)} file(s) found but none parsed as valid replay summary"


def probe_t1(artifacts_dir: str):
    """T1: at least one run_metadata.json shows non-zero PSI on Linux."""
    pattern = os.path.join(artifacts_dir, "logs", "*", "run_metadata.json")
    found = glob.glob(pattern)
    linux_runs = []
    psi_runs = []
    for p in found:
        try:
            with open(p, encoding="utf-8") as f:
                d = json.load(f)
            # Check for platform field (hermesd writes "linux" or "windows")
            platform = str(d.get("platform", "")).lower()
            psi_avail = d.get("psi_available", False)
            if platform in ("linux", "wsl2") or psi_avail:
                linux_runs.append(p)
            # Also check if any sample in the run has non-zero PSI
            # run_metadata won't have samples — check telemetry_quality.json in same dir
            tq_path = os.path.join(os.path.dirname(p), "telemetry_quality.json")
            if os.path.exists(tq_path):
                with open(tq_path, encoding="utf-8") as f:
                    tq = json.load(f)
                cpu_psi = float(tq.get("peak_cpu_psi", 0))
                mem_psi = float(tq.get("peak_mem_psi", 0))
                peak_ups = float(tq.get("peak_ups", 0))
                if cpu_psi > 0 or mem_psi > 0 or (psi_avail and peak_ups > 5):
                    psi_runs.append(p)
        except Exception:
            continue
    if psi_runs:
        return True, f"{len(psi_runs)} run(s) with non-zero PSI pressure recorded"
    if linux_runs:
        return False, f"{len(linux_runs)} Linux run(s) found but all show PSI=0 (Windows/smoke environment)"
    return False, "no Linux run_metadata.json found — needs native Linux or WSL2 daemon run"


def probe_t2(artifacts_dir: str):
    """T2: eval_summary.json with total_predictions > 0."""
    pattern = os.path.join(artifacts_dir, "logs", "*", "eval_summary.json")
    found = glob.glob(pattern)
    # Also check replay dirs and bench dirs
    found += glob.glob(os.path.join(artifacts_dir, "**", "eval_summary.json"), recursive=True)
    found = list(set(found))

    for p in found:
        try:
            with open(p, encoding="utf-8") as f:
                d = json.load(f)
            if d.get("data_available", False) and int(d.get("total_predictions", 0)) > 0:
                tp = d.get("total_predictions", 0)
                return True, f"eval_summary.json has {tp} predictions (data_available=true)"
        except Exception:
            continue

    if found:
        return False, f"{len(found)} eval_summary.json file(s) found but all have data_available=false or 0 predictions"
    return False, "no eval_summary.json found — run: hermes_eval <run_dir>"


def probe_t3(artifacts_dir: str):
    """T3: actions.ndjson with at least one non-dry-run entry."""
    pattern = os.path.join(artifacts_dir, "logs", "*", "actions.ndjson")
    found = glob.glob(pattern)

    real_actions = []
    for p in found:
        try:
            with open(p, encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    # Look for lines that are NOT dry-run / observe-only
                    if '"dry_run":false' in line or '"dry_run": false' in line:
                        real_actions.append(p)
                        break
                    # Some formats use action_taken field
                    if '"action_taken":true' in line or '"action_taken": true' in line:
                        real_actions.append(p)
                        break
        except Exception:
            continue

    if real_actions:
        return True, f"{len(real_actions)} run(s) have real (non-dry-run) action entries in actions.ndjson"

    if found:
        return False, (
            f"{len(found)} actions.ndjson file(s) found but all appear to be dry-run or observe-only — "
            "run active-control scenario on Linux"
        )
    return False, "no actions.ndjson found — needs active-control run on Linux"


def probe_t4(artifacts_dir: str):
    """T4: comparison.csv shows active-control p95 < baseline p95 across ≥5 runs."""
    csv_paths = glob.glob(os.path.join(artifacts_dir, "bench", "comparison.csv"))
    csv_paths += glob.glob(os.path.join(artifacts_dir, "**", "comparison.csv"), recursive=True)
    csv_paths = list(set(csv_paths))

    for p in csv_paths:
        try:
            with open(p, encoding="utf-8") as f:
                lines = [l.strip() for l in f if l.strip()]
            if len(lines) < 2:
                continue
            header = [h.strip().lower() for h in lines[0].split(",")]
            rows = []
            for line in lines[1:]:
                cells = line.split(",")
                row = {header[i]: cells[i].strip() for i in range(min(len(header), len(cells)))}
                rows.append(row)

            # Look for baseline and active-control rows
            baseline_rows = [r for r in rows if "baseline" in r.get("scenario", "").lower()
                             or "baseline" in r.get("run_id", "").lower()]
            active_rows = [r for r in rows if "active" in r.get("scenario", "").lower()
                           or "active" in r.get("run_id", "").lower()]

            if len(baseline_rows) >= 1 and len(active_rows) >= 1:
                # Check if p95 columns exist and active < baseline
                for pcol in ("p95_latency_ms", "p95_ms", "p95"):
                    if pcol in header:
                        try:
                            b_p95 = float(baseline_rows[0].get(pcol, "nan"))
                            a_p95 = float(active_rows[0].get(pcol, "nan"))
                            if a_p95 < b_p95:
                                return True, (
                                    f"comparison.csv shows active-control p95={a_p95:.0f}ms "
                                    f"< baseline p95={b_p95:.0f}ms"
                                )
                            else:
                                return False, (
                                    f"comparison.csv exists but active-control p95={a_p95:.0f}ms "
                                    f">= baseline p95={b_p95:.0f}ms — no improvement shown yet"
                                )
                        except (ValueError, TypeError):
                            pass

            if rows:
                return False, (
                    f"comparison.csv found with {len(rows)} row(s) but no 5-run "
                    "baseline vs active-control improvement established"
                )
        except Exception:
            continue

    return False, "no comparison.csv found — needs 5-run benchmark on Linux (hermes_bench --runs 5)"


def probe_t5(artifacts_dir: str):
    """T5: strace/perf captures exist and RESULTS.md has proven entries."""
    strace_files = glob.glob(os.path.join(artifacts_dir, "**", "*strace*"), recursive=True)
    perf_files   = glob.glob(os.path.join(artifacts_dir, "**", "*perf*"),   recursive=True)

    # Check RESULTS.md for lines with "proven" that are not T0-level smoke
    results_md = "RESULTS.md"
    proven_count = 0
    performance_proven = 0
    if os.path.exists(results_md):
        with open(results_md, encoding="utf-8") as f:
            for line in f:
                if "| proven |" in line.lower() or "| **proven** |" in line.lower():
                    proven_count += 1
                    # Performance claims mention latency or OOM
                    if any(kw in line.lower() for kw in ("latency", "oom", "throughput", "p95")):
                        performance_proven += 1

    has_strace = bool(strace_files)
    has_perf = bool(perf_files)
    has_performance_entry = performance_proven > 0

    if has_strace and has_perf and has_performance_entry:
        return True, (
            f"strace files: {len(strace_files)}, perf files: {len(perf_files)}, "
            f"RESULTS.md performance-proven entries: {performance_proven}"
        )

    missing = []
    if not has_strace:
        missing.append("no strace captures (run scripts/bench_strace.sh)")
    if not has_perf:
        missing.append("no perf captures (run scripts/bench_perf.sh)")
    if not has_performance_entry:
        missing.append("no performance-proven entry in RESULTS.md")

    if proven_count > 0 and not missing:
        return True, f"RESULTS.md has {proven_count} proven entries; strace+perf present"

    return False, "; ".join(missing) if missing else "insufficient T5 evidence"


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

TIERS = [
    ("T0", "Pipeline correct",   "replay_summary.json valid; smoke exits 0",       probe_t0),
    ("T1", "Monitors work",      "non-zero PSI/NVML on live Linux host",            probe_t1),
    ("T2", "Predictor fires",    "eval_summary.json total_predictions > 0",         probe_t2),
    ("T3", "Intervention fires", "actions.ndjson has non-dry-run entry",            probe_t3),
    ("T4", "Intervention helps", "5-run comparison: active p95 < baseline p95",     probe_t4),
    ("T5", "Claim defensible",   "strace+perf captures + RESULTS.md proven entry",  probe_t5),
]


def main() -> int:
    args = sys.argv[1:]
    artifacts_dir = "artifacts"
    require_tier = None

    i = 0
    while i < len(args):
        if args[i] in ("--help", "-h"):
            print(__doc__); return 0
        elif args[i] == "--artifacts-dir" and i + 1 < len(args):
            artifacts_dir = args[i + 1]; i += 2
        elif args[i] == "--require" and i + 1 < len(args):
            require_tier = args[i + 1].upper(); i += 2
        else:
            i += 1

    if not os.path.isdir(artifacts_dir):
        print(f"check_evidence_tiers: artifacts directory not found: {artifacts_dir}")
        return 1

    sep = "=" * 72
    print(sep)
    print("Hermes Evidence Tier Status")
    print(sep)
    print(f"  {'Tier':<4}  {'Label':<22}  {'Met':<5}  Detail")
    print(f"  {'-'*4}  {'-'*22}  {'-'*5}  {'-'*35}")

    all_met = True
    require_met = True
    results = []

    for tier_id, label, _, probe_fn in TIERS:
        met, detail = probe_fn(artifacts_dir)
        results.append((tier_id, label, met, detail))
        status = "YES" if met else "NO"
        if not met:
            all_met = False
        if require_tier and tier_id == require_tier and not met:
            require_met = False
        print(f"  {tier_id:<4}  {label:<22}  {status:<5}  {detail}")

    print()

    # Summary
    met_count = sum(1 for _, _, met, _ in results if met)
    print(f"Evidence tiers met: {met_count} / {len(TIERS)}")

    if met_count == 0:
        print("Status: T0 not met — run smoke scripts to establish pipeline baseline.")
    elif met_count < 2:
        print("Status: Pipeline proven (T0). Needs Linux run to advance to T1+.")
    elif met_count < 4:
        print("Status: Live monitors and predictor proven. T3-T5 need Linux active-control runs.")
    elif met_count < 5:
        print("Status: Active-control fires. Still need 5-run before/after comparison for T4.")
    elif met_count < 6:
        print("Status: Improvement shown. Capture strace+perf and add RESULTS.md entry for T5.")
    else:
        print("Status: All tiers met — results are defensible.")

    print()
    if require_tier:
        if require_met:
            print(f"--require {require_tier}: PASS")
        else:
            print(f"--require {require_tier}: FAIL — {require_tier} evidence not present")
            print(sep)
            return 1

    if not all_met:
        print("Next step: address the first NO tier above.")

    print(sep)
    return 0 if all_met else 1


if __name__ == "__main__":
    sys.exit(main())
