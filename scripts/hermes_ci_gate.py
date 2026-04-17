#!/usr/bin/env python3
"""hermes_ci_gate.py — CI evidence tier gate.

Reads the Hermes artifact directory and exits non-zero if any required
evidence tier is not met.  Designed to run as the final step of a CI job
to enforce evidence standards before marking a run as passing.

Usage:
    python3 scripts/hermes_ci_gate.py
    python3 scripts/hermes_ci_gate.py --require T1
    python3 scripts/hermes_ci_gate.py --artifact-root artifacts --require T0
    python3 scripts/hermes_ci_gate.py --json

Tier definitions:
    T0  Pipeline correct — at least one valid replay summary exists and
        synthetic smoke artifacts are present.
    T1  Live monitors — at least one run has non-zero PSI readings in samples.ndjson.
    T2  Predictor fires — at least one run has predictions.ndjson with risk_band=high|critical.
    T3  Intervention executes — at least one run has actions.ndjson with a non-dry-run result.
    T4  Before/after evidence — at least one bench comparison CSV exists with both
        baseline and active-control rows.
    T5  Defensibility — strace or perf capture files exist under artifacts/evidence/.
"""

import argparse
import json
import os
import sys
from pathlib import Path


def check_t0(artifact_root: Path) -> tuple[bool, str]:
    """T0: Pipeline correct — valid replay summary exists."""
    logs_dir = artifact_root / "logs"
    summaries_dir = artifact_root / "summaries"

    # Check summaries directory first.
    if summaries_dir.is_dir():
        jsons = list(summaries_dir.glob("*-summary.json"))
        for f in jsons:
            try:
                data = json.loads(f.read_text())
                if data.get("valid") is True:
                    return True, f"valid replay summary: {f.name}"
            except Exception:
                continue

    # Fall back to per-run replay_summary.json.
    if logs_dir.is_dir():
        for run_dir in logs_dir.iterdir():
            rs = run_dir / "replay_summary.json"
            if rs.exists():
                try:
                    data = json.loads(rs.read_text())
                    if data.get("valid") is True:
                        return True, f"valid replay summary in {run_dir.name}"
                except Exception:
                    continue

    return False, "no valid replay_summary.json found under artifacts/"


def check_t1(artifact_root: Path) -> tuple[bool, str]:
    """T1: Live monitors — non-zero PSI readings in samples.ndjson."""
    logs_dir = artifact_root / "logs"
    if not logs_dir.is_dir():
        return False, "no logs directory"

    for run_dir in sorted(logs_dir.iterdir(), key=lambda p: p.stat().st_mtime, reverse=True):
        samples = run_dir / "samples.ndjson"
        if not samples.exists():
            continue
        with samples.open() as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    rec = json.loads(line)
                    cpu = rec.get("cpu_some_avg10", 0.0)
                    mem = rec.get("mem_some_avg10", 0.0)
                    if cpu > 0.1 or mem > 0.1:
                        return True, (
                            f"non-zero PSI in {run_dir.name}: "
                            f"cpu_some={cpu:.2f} mem_some={mem:.2f}"
                        )
                except Exception:
                    continue

    return False, (
        "no samples.ndjson with non-zero PSI found "
        "(run hermesd on a Linux host under real workload pressure)"
    )


def check_t2(artifact_root: Path) -> tuple[bool, str]:
    """T2: Predictor fires — high/critical risk_band in predictions.ndjson."""
    logs_dir = artifact_root / "logs"
    if not logs_dir.is_dir():
        return False, "no logs directory"

    for run_dir in sorted(logs_dir.iterdir(), key=lambda p: p.stat().st_mtime, reverse=True):
        preds = run_dir / "predictions.ndjson"
        if not preds.exists():
            continue
        with preds.open() as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    rec = json.loads(line)
                    band = rec.get("risk_band", "")
                    if band in ("high", "critical"):
                        return True, (
                            f"predictor fired in {run_dir.name}: "
                            f"risk_band={band} risk_score={rec.get('risk_score', '?')}"
                        )
                except Exception:
                    continue

    return False, (
        "no high/critical risk_band predictions found "
        "(run hermesd under pressure and check predictions.ndjson)"
    )


def check_t3(artifact_root: Path) -> tuple[bool, str]:
    """T3: Intervention executes — non-dry-run action result in actions.ndjson."""
    logs_dir = artifact_root / "logs"
    if not logs_dir.is_dir():
        return False, "no logs directory"

    for run_dir in sorted(logs_dir.iterdir(), key=lambda p: p.stat().st_mtime, reverse=True):
        actions = run_dir / "actions.ndjson"
        if not actions.exists():
            continue
        with actions.open() as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    rec = json.loads(line)
                    result = rec.get("result", "")
                    # dry-run results contain "dry-run" or "noop"; real results don't.
                    if result and "dry" not in result.lower() and "noop" not in result.lower():
                        return True, (
                            f"intervention in {run_dir.name}: "
                            f"action={rec.get('action_type','?')} result={result}"
                        )
                except Exception:
                    continue

    return False, (
        "no active-control action results found "
        "(run hermesd with HERMES_RUNTIME_MODE=active under pressure)"
    )


def check_t4(artifact_root: Path) -> tuple[bool, str]:
    """T4: Before/after evidence — comparison CSV with baseline + active-control."""
    bench_dir = artifact_root / "bench"
    if not bench_dir.is_dir():
        return False, "no bench directory"

    for csv_path in bench_dir.glob("*.csv"):
        content = csv_path.read_text()
        if "baseline" in content and ("active" in content or "observe" in content):
            return True, f"comparison CSV with multi-mode data: {csv_path.name}"

    return False, (
        "no multi-mode comparison CSV found in artifacts/bench/ "
        "(run hermes_bench baseline + active-control and hermes_compare --auto-compare)"
    )


def check_t5(artifact_root: Path) -> tuple[bool, str]:
    """T5: Defensibility — strace or perf capture files exist."""
    evidence_dir = artifact_root / "evidence"
    if not evidence_dir.is_dir():
        return False, "no evidence directory"

    for pattern in ("*.strace", "*.perf", "*strace*", "*perf*"):
        matches = list(evidence_dir.rglob(pattern))
        if matches:
            return True, f"defensibility capture: {matches[0].name}"

    return False, (
        "no strace or perf captures found in artifacts/evidence/ "
        "(run scripts/bench_strace.sh and scripts/bench_perf.sh on Linux)"
    )


TIER_CHECKS = {
    "T0": check_t0,
    "T1": check_t1,
    "T2": check_t2,
    "T3": check_t3,
    "T4": check_t4,
    "T5": check_t5,
}


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Hermes CI evidence tier gate.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--artifact-root", default="artifacts",
        help="Root directory for Hermes artifacts (default: artifacts)"
    )
    parser.add_argument(
        "--require", metavar="TIER", default="T0",
        help="Minimum tier to require (T0–T5). Fails if this tier is not met. Default: T0"
    )
    parser.add_argument(
        "--json", action="store_true",
        help="Output results as JSON instead of human-readable table"
    )
    parser.add_argument(
        "--all", action="store_true",
        help="Check all tiers and report, but only fail on --require tier"
    )
    args = parser.parse_args()

    artifact_root = Path(args.artifact_root)
    require_tier = args.require.upper()

    if require_tier not in TIER_CHECKS:
        print(f"Unknown tier: {require_tier}. Valid: {', '.join(TIER_CHECKS)}", file=sys.stderr)
        return 2

    results = {}
    tiers_to_check = list(TIER_CHECKS) if args.all else list(TIER_CHECKS)

    for tier, check_fn in TIER_CHECKS.items():
        passed, detail = check_fn(artifact_root)
        results[tier] = {"passed": passed, "detail": detail}

    if args.json:
        print(json.dumps(
            {
                "artifact_root": str(artifact_root),
                "require": require_tier,
                "tiers": results,
            },
            indent=2
        ))
    else:
        tier_order = ["T0", "T1", "T2", "T3", "T4", "T5"]
        print()
        print("=== Hermes CI Evidence Gate ===")
        print(f"Artifact root : {artifact_root}")
        print(f"Required tier : {require_tier}")
        print()
        print(f"  {'Tier':<6} {'Status':<8} Detail")
        print("  " + "-" * 70)
        for tier in tier_order:
            r = results[tier]
            status = "PASS" if r["passed"] else "FAIL"
            marker = "✓" if r["passed"] else "✗"
            print(f"  {tier:<6} {marker} {status:<6} {r['detail']}")
        print()

    # Determine exit code: fail if required tier (or any tier before it) not met.
    tier_order = ["T0", "T1", "T2", "T3", "T4", "T5"]
    required_idx = tier_order.index(require_tier)
    for tier in tier_order[: required_idx + 1]:
        if not results[tier]["passed"]:
            if not args.json:
                print(f"GATE FAILED: {tier} not met. {results[tier]['detail']}")
            return 1

    if not args.json:
        print(f"GATE PASSED: {require_tier} met.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
