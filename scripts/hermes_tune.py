#!/usr/bin/env python3
"""hermes_tune.py — Predictor calibration suggestion tool.

Reads one or more eval_summary.json files produced by hermes_eval and compares
the aggregate metrics against the calibration targets defined in design.md.
Prints a recommendation table and suggests specific changes to config/schema.yaml.

No external dependencies required.

Usage:
    python3 scripts/hermes_tune.py artifacts/logs/*/eval_summary.json
    python3 scripts/hermes_tune.py --eval-dir artifacts/logs
    python3 scripts/hermes_tune.py --eval-dir artifacts/logs --schema config/schema.yaml
"""

import glob
import json
import os
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Calibration targets (from design.md — Predictor Calibration Cycle)
# ---------------------------------------------------------------------------

TARGETS = {
    "precision":                   {"min": 0.85, "label": "precision >= 0.85"},
    "recall":                      {"min": 0.80, "label": "recall >= 0.80"},
    "false_positive_rate_per_hour":{"max": 5.0,  "label": "FP rate < 5/hr"},
    "mean_lead_time_s":            {"min": 3.0,  "label": "mean lead time >= 3s"},
    "f1":                          {"min": 0.80, "label": "F1 >= 0.80"},
}

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def load_eval_files(paths: list) -> list:
    results = []
    for p in paths:
        try:
            with open(p, encoding="utf-8") as f:
                d = json.load(f)
            if d.get("data_available", False):
                d["_source"] = str(p)
                results.append(d)
        except Exception as e:
            print(f"  [WARN] Could not read {p}: {e}", file=sys.stderr)
    return results


def aggregate(evals: list) -> dict:
    """Average numeric metrics across all eval runs."""
    if not evals:
        return {}
    keys = ["precision", "recall", "f1", "mean_lead_time_s",
            "false_positive_rate_per_hour", "observation_window_s",
            "true_positives", "false_positives", "false_negatives",
            "total_predictions", "total_high_risk_predictions", "total_failure_events"]
    agg = {k: 0.0 for k in keys}
    for e in evals:
        for k in keys:
            agg[k] += float(e.get(k, 0.0))
    n = len(evals)
    for k in keys:
        agg[k] /= n
    agg["run_count"] = n
    return agg


def check_targets(agg: dict) -> list:
    """Return list of (metric, value, target_str, passed, suggestion)."""
    rows = []
    for metric, spec in TARGETS.items():
        val = agg.get(metric, None)
        if val is None:
            rows.append((metric, "N/A", spec["label"], None, "No data"))
            continue
        if "min" in spec:
            passed = val >= spec["min"]
            suggestion = (
                "" if passed else
                f"LOWER threshold — {metric} is {val:.3f}, target >= {spec['min']}"
            )
        else:
            passed = val <= spec["max"]
            suggestion = (
                "" if passed else
                f"RAISE threshold — {metric} is {val:.3f}, target <= {spec['max']}"
            )
        rows.append((metric, val, spec["label"], passed, suggestion))
    return rows


def read_schema_excerpt(schema_path: str) -> dict:
    """Read relevant threshold lines from schema.yaml (simple key: value scan)."""
    relevant = {}
    try:
        with open(schema_path, encoding="utf-8") as f:
            for line in f:
                for key in ("ups_elevated_threshold", "ups_critical_threshold",
                            "risk_high_threshold", "risk_critical_threshold",
                            "vram_slope_fast_threshold", "mem_full_residency_threshold"):
                    if line.strip().startswith(key + ":"):
                        val = line.split(":", 1)[1].strip()
                        relevant[key] = val
    except FileNotFoundError:
        pass
    return relevant


def schema_suggestions(rows: list) -> list:
    """Translate failed target rows into concrete schema.yaml suggestions."""
    suggestions = []
    for metric, val, _, passed, _ in rows:
        if passed or passed is None:
            continue
        if metric == "false_positive_rate_per_hour":
            suggestions.append(
                "  ups_elevated_threshold: raise by 5-10 (currently fires too easily)")
            suggestions.append(
                "  risk_high_threshold:    raise by 0.05 (high-risk band too wide)")
        elif metric == "precision":
            suggestions.append(
                "  ups_critical_threshold: raise by 5 (critical band too broad)")
            suggestions.append(
                "  vram_slope_fast_threshold: raise (fast-slope window too sensitive)")
        elif metric == "recall":
            suggestions.append(
                "  mem_full_residency_threshold: lower by 1-2 cycles (catching pressure later than ideal)")
            suggestions.append(
                "  ups_elevated_threshold: lower by 5 (elevated band catching pressure too late)")
        elif metric == "mean_lead_time_s":
            suggestions.append(
                "  ups_elevated_threshold: lower by 5 (trigger earlier to gain lead time)")
        elif metric == "f1":
            suggestions.append(
                "  Review precision and recall individually — both affect F1")
    return list(dict.fromkeys(suggestions))  # deduplicate while preserving order


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    args = sys.argv[1:]
    eval_dir = None
    schema_path = "config/schema.yaml"
    explicit_files = []

    i = 0
    while i < len(args):
        if args[i] == "--eval-dir" and i + 1 < len(args):
            eval_dir = args[i + 1]; i += 2
        elif args[i] == "--schema" and i + 1 < len(args):
            schema_path = args[i + 1]; i += 2
        elif args[i] in ("--help", "-h"):
            print(__doc__); return 0
        else:
            explicit_files.append(args[i]); i += 1

    # Collect eval files
    if eval_dir:
        explicit_files += glob.glob(os.path.join(eval_dir, "*", "eval_summary.json"))
        explicit_files += glob.glob(os.path.join(eval_dir, "eval_summary.json"))

    if not explicit_files:
        print("hermes_tune: no eval_summary.json files found.")
        print("  Usage: python3 scripts/hermes_tune.py --eval-dir artifacts/logs")
        return 1

    evals = load_eval_files(explicit_files)
    if not evals:
        print("hermes_tune: no runs with data_available=true found.")
        print("  Run hermes_eval on a high-pressure run first.")
        return 1

    agg = aggregate(evals)
    rows = check_targets(agg)
    schema = read_schema_excerpt(schema_path)

    sep = "=" * 64
    print(sep)
    print(f"Hermes Predictor Calibration Report  ({agg['run_count']:.0f} eval run(s))")
    print(sep)
    print(f"  {'Metric':<35} {'Value':>10}  {'Target':<30}  Status")
    print(f"  {'-'*35} {'-'*10}  {'-'*30}  ------")
    all_pass = True
    for metric, val, target_label, passed, _ in rows:
        val_str = f"{val:.3f}" if isinstance(val, float) else str(val)
        status = "PASS" if passed else ("FAIL" if passed is False else "N/A")
        if passed is False:
            all_pass = False
        print(f"  {metric:<35} {val_str:>10}  {target_label:<30}  {status}")

    print()
    if schema:
        print("Current schema.yaml thresholds:")
        for k, v in schema.items():
            print(f"  {k}: {v}")
        print()

    suggestions = schema_suggestions(rows)
    if suggestions:
        print("Suggested config/schema.yaml adjustments:")
        for s in suggestions:
            print(s)
        print()
        print("After adjusting, re-run: bash scripts/smoke_wsl2.sh")
        print("Confirm synthetic fixture still passes 17/17 assertions.")
    else:
        print("All calibration targets met. No threshold changes needed.")

    print(sep)
    return 0 if all_pass else 1


if __name__ == "__main__":
    sys.exit(main())
