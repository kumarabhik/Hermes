#!/usr/bin/env python3
"""populate_readme_results.py — Auto-populate README Key Results tables from bench artifacts.

Reads *-summary.json files from artifacts/bench/ and eval_summary.json from
artifacts/logs/ and replaces the placeholder rows in the README "Key Results"
section with real numbers.

Closes Phase 4 roadmap item: "README-ready before/after claims are derived from
generated artifacts rather than manual interpretation."

Usage:
    python3 scripts/populate_readme_results.py [--bench-dir DIR] [--logs-dir DIR]
                                               [--readme PATH] [--dry-run]

Flags:
    --bench-dir DIR    Directory containing *-summary.json files (default: artifacts/bench)
    --logs-dir  DIR    Directory containing run-id sub-dirs with eval_summary.json (default: artifacts/logs)
    --readme    PATH   README file to update in place (default: README.md)
    --dry-run          Print the new README content to stdout; do not overwrite.
"""

import argparse
import glob
import json
import os
import re
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple


# ---------------------------------------------------------------------------
# JSON helpers
# ---------------------------------------------------------------------------

def load_json(path: str) -> Optional[Dict[str, Any]]:
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


# ---------------------------------------------------------------------------
# Bench summary aggregation
# ---------------------------------------------------------------------------

def collect_bench_summaries(bench_dir: str) -> Dict[str, List[Dict[str, Any]]]:
    """Return {mode: [summary_dict, ...]} grouped by runtime_mode."""
    result: Dict[str, List[Dict[str, Any]]] = defaultdict(list)
    pattern = os.path.join(bench_dir, "*-summary.json")
    for path in sorted(glob.glob(pattern)):
        data = load_json(path)
        if data is None:
            continue
        mode = data.get("runtime_mode", "unknown")
        result[mode].append(data)
    return result


def safe_mean(values: List[float]) -> Optional[float]:
    vals = [v for v in values if v is not None and v >= 0]
    return sum(vals) / len(vals) if vals else None


def safe_std(values: List[float]) -> Optional[float]:
    m = safe_mean(values)
    if m is None or len(values) < 2:
        return None
    variance = sum((v - m) ** 2 for v in values if v is not None and v >= 0)
    return (variance / len(values)) ** 0.5


def fmt_maybe(v: Optional[float], decimals: int = 0) -> str:
    if v is None:
        return "—"
    if decimals == 0:
        return str(int(round(v)))
    return f"{v:.{decimals}f}"


def mode_row(
    scenario_name: str,
    mode: str,
    summaries: List[Dict[str, Any]],
) -> str:
    p95_vals = [s.get("p95_latency_ms") for s in summaries
                if s.get("p95_latency_ms") is not None and s["p95_latency_ms"] >= 0]
    oom_vals  = [s.get("oom_count", 0) for s in summaries]
    comp_vals = [s.get("completion_rate") for s in summaries
                 if s.get("completion_rate") is not None and s["completion_rate"] >= 0]
    action_vals = [s.get("intervention_count", 0) for s in summaries]

    n       = len(summaries)
    p95_m   = safe_mean(p95_vals)
    p95_s   = safe_std(p95_vals)
    oom_tot = sum(oom_vals) if oom_vals else None
    comp_m  = safe_mean(comp_vals)
    act_tot = sum(action_vals) if action_vals else None

    p95_str  = (f"{fmt_maybe(p95_m)} ± {fmt_maybe(p95_s)}" if p95_s is not None
                else fmt_maybe(p95_m)) if p95_m is not None else "—"
    oom_str  = fmt_maybe(float(oom_tot)) if oom_tot is not None else "—"
    comp_str = (f"{comp_m * 100:.0f}%" if comp_m is not None else "—")
    act_str  = fmt_maybe(float(act_tot)) if act_tot is not None and mode != "baseline" else "n/a"
    runs_tag = f" ({n} run{'s' if n != 1 else ''})"

    return (
        f"| `{scenario_name}` | {mode}{runs_tag} | {p95_str} | {oom_str} | "
        f"{comp_str} | {act_str} |"
    )


def build_latency_table(by_mode: Dict[str, List[Dict[str, Any]]]) -> str:
    """Build the Before/After p95 Latency table markdown."""
    header = (
        "| Scenario | Mode | p95 Latency (ms) | OOM Kills | Completion Rate | Hermes Actions |\n"
        "| --- | --- | --- | --- | --- | --- |"
    )
    rows = []

    # Determine order: baseline first, then observe-only, then active-control, then others.
    order = ["baseline", "observe-only", "active-control"]
    modes_seen = list(by_mode.keys())
    for m in order:
        if m in modes_seen:
            modes_seen.remove(m)
    ordered_modes = [m for m in order if m in by_mode] + modes_seen

    if not by_mode:
        rows.append("| _(No bench runs found in artifacts/bench/)_ | — | — | — | — | n/a |")
    else:
        for mode in ordered_modes:
            summaries = by_mode[mode]
            # Group by scenario_name.
            by_scenario: Dict[str, List[Dict[str, Any]]] = defaultdict(list)
            for s in summaries:
                sc = s.get("scenario_name") or s.get("scenario") or "unknown"
                by_scenario[sc].append(s)
            for sc, sc_summaries in sorted(by_scenario.items()):
                rows.append(mode_row(sc, mode, sc_summaries))

    return header + "\n" + "\n".join(rows)


# ---------------------------------------------------------------------------
# Predictor quality table
# ---------------------------------------------------------------------------

def collect_eval_summaries(logs_dir: str) -> List[Dict[str, Any]]:
    evals = []
    for run_dir in sorted(glob.glob(os.path.join(logs_dir, "*"))):
        path = os.path.join(run_dir, "eval_summary.json")
        data = load_json(path)
        if data and data.get("data_available") is True:
            evals.append(data)
    return evals


def build_predictor_table(evals: List[Dict[str, Any]]) -> str:
    header = (
        "| Metric | Value | Target | Status |\n"
        "| --- | --- | --- | --- |"
    )

    if not evals:
        return (
            header + "\n"
            "| Precision | — | ≥ 0.85 | pending |\n"
            "| Recall | — | ≥ 0.80 | pending |\n"
            "| F1 | — | ≥ 0.80 | pending |\n"
            "| Mean lead time (s) | — | ≥ 3.0 s | pending |\n"
            "| False positive rate (/hr) | — | < 5 / hr | pending |"
        )

    # Average over all data-available eval summaries.
    def avg(key: str) -> Optional[float]:
        vals = [e[key] for e in evals if e.get(key) is not None and e[key] >= 0]
        return sum(vals) / len(vals) if vals else None

    precision  = avg("precision")
    recall     = avg("recall")
    f1         = avg("f1")
    lead_time  = avg("mean_lead_time_s")
    fp_rate    = avg("false_positive_rate_per_hour")
    n          = len(evals)

    def status(val: Optional[float], target: float, above: bool) -> str:
        if val is None:
            return "pending"
        ok = val >= target if above else val < target
        return "PASS" if ok else "FAIL"

    def fmt3(v: Optional[float]) -> str:
        return f"{v:.3f}" if v is not None else "—"

    tag = f" ({n} eval{'s' if n != 1 else ''})"
    rows = [
        f"| Precision{tag} | {fmt3(precision)} | ≥ 0.85 | {status(precision, 0.85, True)} |",
        f"| Recall | {fmt3(recall)} | ≥ 0.80 | {status(recall, 0.80, True)} |",
        f"| F1 | {fmt3(f1)} | ≥ 0.80 | {status(f1, 0.80, True)} |",
        f"| Mean lead time (s) | {fmt3(lead_time)} | ≥ 3.0 s | {status(lead_time, 3.0, True)} |",
        f"| False positive rate (/hr) | {fmt3(fp_rate)} | < 5 / hr | {status(fp_rate, 5.0, False)} |",
    ]
    return header + "\n" + "\n".join(rows)


# ---------------------------------------------------------------------------
# False Positive Baseline table
# ---------------------------------------------------------------------------

def build_fp_table(by_mode: Dict[str, List[Dict[str, Any]]]) -> str:
    header = (
        "| Scenario | Active-control interventions | Target |\n"
        "| --- | --- | --- |"
    )
    # Look for active-control runs of low_pressure or similar scenario.
    lp_runs: List[Dict[str, Any]] = []
    for mode, summaries in by_mode.items():
        if "active" not in mode:
            continue
        for s in summaries:
            sc = (s.get("scenario_name") or s.get("scenario") or "").lower()
            if "low" in sc or "pressure" in sc or "fp" in sc:
                lp_runs.append(s)

    if not lp_runs:
        return header + "\n| `low_pressure_scenario.yaml` | — | ≤ 1 |"

    total_interventions = sum(s.get("intervention_count", 0) for s in lp_runs)
    n = len(lp_runs)
    status = "PASS" if total_interventions <= 1 else "FAIL"
    return (
        header + "\n"
        f"| `low_pressure_scenario.yaml` ({n} run{'s' if n != 1 else ''}) "
        f"| {total_interventions} | ≤ 1 | {status} |"
    )


# ---------------------------------------------------------------------------
# README surgery
# ---------------------------------------------------------------------------

# Sentinel comments that bracket sections we can replace.
LATENCY_START  = "<!-- KEY_RESULTS_LATENCY_START -->"
LATENCY_END    = "<!-- KEY_RESULTS_LATENCY_END -->"
PRED_START     = "<!-- KEY_RESULTS_PREDICTOR_START -->"
PRED_END       = "<!-- KEY_RESULTS_PREDICTOR_END -->"
FP_START       = "<!-- KEY_RESULTS_FP_START -->"
FP_END         = "<!-- KEY_RESULTS_FP_END -->"

# Patterns that identify the existing placeholder table rows so we can replace
# them even without sentinel comments in the README.
_LATENCY_HEADER_PAT = re.compile(
    r"\| Scenario \| Mode \| p95 Latency.*?\n\| --- \|.*?\n((?:\|.*\n)*)",
    re.MULTILINE,
)
_PREDICTOR_HEADER_PAT = re.compile(
    r"\| Metric \| Value \| Target \| Status \|\n\| --- \|.*?\n((?:\|.*\n)*)",
    re.MULTILINE,
)
_FP_HEADER_PAT = re.compile(
    r"\| Scenario \| Active-control interventions \| Target \|\n\| --- \|.*?\n((?:\|.*\n)*)",
    re.MULTILINE,
)


def replace_table(content: str, new_table: str, header_pat: re.Pattern) -> str:
    """Replace an existing markdown table matched by header_pat with new_table + newline."""
    m = header_pat.search(content)
    if not m:
        return content  # table not found — don't touch

    # The full match includes header rows + existing data rows; replace the
    # data rows (group 1) while keeping the header intact.
    header_section = m.group(0)[: m.start(1) - m.start(0)]
    # Build the replacement: keep header rows, replace body rows.
    new_table_lines = new_table.split("\n")
    # Skip the header lines of new_table (already in header_section).
    header_lines = header_section.rstrip("\n").split("\n")
    body_lines   = new_table_lines[len(header_lines):]
    replacement  = header_section + "\n".join(body_lines) + "\n"
    return content[: m.start()] + replacement + content[m.end():]


def update_readme(
    readme_path: str,
    latency_table: str,
    predictor_table: str,
    fp_table: str,
    dry_run: bool,
) -> bool:
    try:
        with open(readme_path, encoding="utf-8") as f:
            content = f.read()
    except FileNotFoundError:
        print(f"ERROR: README not found at {readme_path}", file=sys.stderr)
        return False

    original = content

    content = replace_table(content, latency_table,  _LATENCY_HEADER_PAT)
    content = replace_table(content, predictor_table, _PREDICTOR_HEADER_PAT)
    content = replace_table(content, fp_table,        _FP_HEADER_PAT)

    if content == original:
        print("populate_readme_results: no matching table sections found in README; no changes made.")
        print("  Expected markdown tables with headers:")
        print("    '| Scenario | Mode | p95 Latency ...'")
        print("    '| Metric | Value | Target | Status |'")
        print("    '| Scenario | Active-control interventions | Target |'")
        return True

    if dry_run:
        print(content)
        return True

    with open(readme_path, "w", encoding="utf-8") as f:
        f.write(content)

    print(f"populate_readme_results: updated {readme_path}")
    return True


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bench-dir", default="artifacts/bench",
                        help="Directory containing *-summary.json files")
    parser.add_argument("--logs-dir",  default="artifacts/logs",
                        help="Directory containing run sub-dirs with eval_summary.json")
    parser.add_argument("--readme",    default="README.md",
                        help="README file to update in place")
    parser.add_argument("--dry-run",   action="store_true",
                        help="Print new content to stdout; do not overwrite README")
    args = parser.parse_args()

    by_mode  = collect_bench_summaries(args.bench_dir)
    evals    = collect_eval_summaries(args.logs_dir)

    n_bench  = sum(len(v) for v in by_mode.values())
    n_evals  = len(evals)

    print(f"populate_readme_results: found {n_bench} bench summary/summaries, "
          f"{n_evals} eval summary/summaries")

    latency_table   = build_latency_table(by_mode)
    predictor_table = build_predictor_table(evals)
    fp_table        = build_fp_table(by_mode)

    ok = update_readme(args.readme, latency_table, predictor_table, fp_table, args.dry_run)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
