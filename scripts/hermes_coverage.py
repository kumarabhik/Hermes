#!/usr/bin/env python3
"""hermes_coverage.py — state-transition coverage matrix.

Scans all state_coverage.json files under artifacts/logs/ (one per run) and
builds a transition matrix: how many times each (from_state -> to_state) pair
was observed, across all runs and per run.

Usage:
  python scripts/hermes_coverage.py
  python scripts/hermes_coverage.py --artifact-root artifacts
  python scripts/hermes_coverage.py --json
  python scripts/hermes_coverage.py --run-id <id>   # single run only
"""

import argparse
import json
import os
import sys
from collections import defaultdict
from pathlib import Path

STATES = ["normal", "elevated", "throttled", "cooldown", "recovery"]

def load_coverage(path: Path) -> dict:
    try:
        with open(path) as f:
            return json.load(f)
    except Exception:
        return {}

def extract_transitions(coverage: dict) -> list[tuple[str, str]]:
    """Return list of (from, to) pairs from a state_coverage.json."""
    transitions = []
    # Format: {"transitions": [{"from": "normal", "to": "elevated"}, ...]}
    for t in coverage.get("transitions", []):
        frm = t.get("from", "")
        to  = t.get("to",   "")
        if frm and to:
            transitions.append((frm, to))
    # Alternate format: {"state_transitions": {"normal->elevated": 3, ...}}
    for k, count in coverage.get("state_transitions", {}).items():
        if "->" in k:
            parts = k.split("->", 1)
            for _ in range(int(count)):
                transitions.append((parts[0].strip(), parts[1].strip()))
    return transitions

def build_matrix(transitions: list[tuple[str, str]], states: list[str]) -> dict:
    matrix = {s: {t: 0 for t in states} for s in states}
    unknown = set()
    for frm, to in transitions:
        if frm not in matrix:
            matrix[frm] = {}
            unknown.add(frm)
        if to not in matrix[frm]:
            matrix[frm][to] = 0
        matrix[frm][to] += 1
    return matrix

def print_matrix(matrix: dict, states: list[str], title: str = "Transition matrix") -> None:
    all_states = list(dict.fromkeys(states + [s for s in matrix if s not in states]))
    col_w = max(len(s) for s in all_states) + 2
    head_w = col_w

    print(f"\n{title}")
    print("-" * (head_w + col_w * len(all_states)))
    header = f"{'from \\ to':<{head_w}}" + "".join(f"{s:>{col_w}}" for s in all_states)
    print(header)
    print("-" * (head_w + col_w * len(all_states)))
    for frm in all_states:
        row = f"{frm:<{head_w}}"
        for to in all_states:
            count = matrix.get(frm, {}).get(to, 0)
            row += f"{count:>{col_w}}"
        print(row)
    print()

def main() -> int:
    parser = argparse.ArgumentParser(description="Hermes state-transition coverage matrix")
    parser.add_argument("--artifact-root", default="artifacts",
                        help="Root directory for artifacts (default: artifacts)")
    parser.add_argument("--json", action="store_true", help="Output as JSON")
    parser.add_argument("--run-id", default="", help="Limit to a single run ID")
    args = parser.parse_args()

    logs_dir = Path(args.artifact_root) / "logs"
    if not logs_dir.is_dir():
        print(f"No logs directory found at {logs_dir}", file=sys.stderr)
        return 1

    aggregate_transitions: list[tuple[str, str]] = []
    per_run: dict[str, list[tuple[str, str]]] = {}

    for run_dir in sorted(logs_dir.iterdir()):
        if not run_dir.is_dir():
            continue
        run_id = run_dir.name
        if args.run_id and run_id != args.run_id:
            continue

        cov_path = run_dir / "state_coverage.json"
        if not cov_path.exists():
            # Fallback: extract transitions from events.ndjson
            ev_path = run_dir / "events.ndjson"
            if ev_path.exists():
                transitions = []
                try:
                    with open(ev_path) as f:
                        for line in f:
                            line = line.strip()
                            if not line:
                                continue
                            try:
                                ev = json.loads(line)
                            except Exception:
                                continue
                            if ev.get("kind") == "state_transition":
                                frm = ev.get("from_state", ev.get("from", ""))
                                to  = ev.get("to_state",   ev.get("to",   ""))
                                if frm and to:
                                    transitions.append((frm, to))
                except Exception:
                    pass
                if transitions:
                    per_run[run_id] = transitions
                    aggregate_transitions.extend(transitions)
            continue

        coverage = load_coverage(cov_path)
        transitions = extract_transitions(coverage)
        per_run[run_id] = transitions
        aggregate_transitions.extend(transitions)

    if not per_run and not aggregate_transitions:
        print("No state transition data found.", file=sys.stderr)
        return 1

    if args.json:
        agg_matrix = build_matrix(aggregate_transitions, STATES)
        out: dict = {
            "aggregate": agg_matrix,
            "runs": {},
        }
        for run_id, transitions in per_run.items():
            out["runs"][run_id] = build_matrix(transitions, STATES)
        print(json.dumps(out, indent=2))
        return 0

    # Human-readable output.
    agg_matrix = build_matrix(aggregate_transitions, STATES)
    print(f"Hermes state-transition coverage")
    print(f"Artifact root : {args.artifact_root}")
    print(f"Runs scanned  : {len(per_run)}")
    print(f"Total transitions: {len(aggregate_transitions)}")

    print_matrix(agg_matrix, STATES, "Aggregate transition matrix (all runs)")

    if len(per_run) > 1:
        for run_id, transitions in per_run.items():
            m = build_matrix(transitions, STATES)
            print_matrix(m, STATES, f"Run: {run_id}  ({len(transitions)} transitions)")

    # Coverage summary: which transitions were NEVER observed?
    print("Coverage gaps (never observed transitions):")
    gaps = []
    for frm in STATES:
        for to in STATES:
            if frm == to:
                continue
            if agg_matrix.get(frm, {}).get(to, 0) == 0:
                gaps.append(f"  {frm} -> {to}")
    if gaps:
        for g in gaps:
            print(g)
    else:
        print("  (all state transitions observed — full coverage!)")

    return 0

if __name__ == "__main__":
    sys.exit(main())
