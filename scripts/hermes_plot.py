#!/usr/bin/env python3
"""hermes_plot.py — Extract time-series CSVs from Hermes NDJSON artifacts.

Reads a Hermes run directory and produces:
  1. pressure_trace.csv  — UPS, risk, PSI signals over time (from scores.ndjson)
  2. latency_cdf.csv     — foreground workload latency CDF (from latency.json)
  3. decision_trace.csv  — scheduler state and action level over time (from decisions.ndjson)
  4. band_timeline.csv   — UPS band transitions over time (from events.ndjson)

No matplotlib or external dependency required — outputs plain CSV for use with
gnuplot, matplotlib, Excel, or any spreadsheet tool.

Usage:
    python3 scripts/hermes_plot.py <run-dir> [output-dir]
    python3 scripts/hermes_plot.py artifacts/logs/my-run-01

Optional:
    python3 scripts/hermes_plot.py <run-dir> [output-dir] --plot
    (requires matplotlib; falls back gracefully if not installed)

    python3 scripts/hermes_plot.py <run-dir> --summary
    (prints a compact text summary; no files written, no matplotlib needed)
"""

import csv
import json
import os
import sys
from pathlib import Path


# ---------------------------------------------------------------------------
# NDJSON helpers
# ---------------------------------------------------------------------------

def read_ndjson(path: Path) -> list:
    """Read a newline-delimited JSON file, skipping blank/malformed lines."""
    records = []
    if not path.exists():
        return records
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                records.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    return records


def write_csv(path: Path, fieldnames: list, rows: list) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)
    print(f"  wrote {path.name}  ({len(rows)} rows)")


# ---------------------------------------------------------------------------
# 1. Pressure trace (scores.ndjson)
# ---------------------------------------------------------------------------

def extract_pressure_trace(run_dir: Path, out_dir: Path) -> Path:
    scores = read_ndjson(run_dir / "scores.ndjson")
    preds  = read_ndjson(run_dir / "predictions.ndjson")

    # Build a ts -> risk_score map from predictions for joining.
    risk_map = {}
    for p in preds:
        ts = p.get("ts_wall_ms") or p.get("timestamp_ms", 0)
        risk_map[ts] = p.get("risk_score", 0.0)

    rows = []
    for s in scores:
        ts = s.get("ts_wall_ms") or s.get("timestamp_ms", 0)
        row = {
            "ts_wall_ms":        ts,
            "ups":               s.get("ups", 0.0),
            "ups_band":          s.get("band", ""),
            "cpu_some_avg10":    s.get("cpu_some_avg10", 0.0),
            "mem_full_avg10":    s.get("mem_full_avg10", 0.0),
            "io_full_avg10":     s.get("io_full_avg10", 0.0),
            "vram_used_mb":      s.get("vram_used_mb", 0.0),
            "gpu_util_pct":      s.get("gpu_util_pct", 0.0),
            "risk_score":        risk_map.get(ts, 0.0),
        }
        rows.append(row)

    out_path = out_dir / "pressure_trace.csv"
    fields = ["ts_wall_ms", "ups", "ups_band", "cpu_some_avg10",
              "mem_full_avg10", "io_full_avg10", "vram_used_mb",
              "gpu_util_pct", "risk_score"]
    write_csv(out_path, fields, rows)
    return out_path


# ---------------------------------------------------------------------------
# 2. Latency CDF (latency.json from hermes_bench multi-run)
# ---------------------------------------------------------------------------

def extract_latency_cdf(run_dir: Path, out_dir: Path) -> Path:
    # Latency JSON may be in run_dir or artifacts/bench/ — try both.
    lat_path = run_dir / "latency.json"
    if not lat_path.exists():
        # Try bench dir relative to artifacts root.
        bench_candidate = run_dir.parent.parent / "bench" / (run_dir.name + "-latency.json")
        if bench_candidate.exists():
            lat_path = bench_candidate

    out_path = out_dir / "latency_cdf.csv"
    if not lat_path.exists():
        print(f"  [skip] latency_cdf.csv — no latency.json found for {run_dir.name}")
        return out_path

    with open(lat_path, encoding="utf-8") as f:
        data = json.load(f)

    # CDF from percentile fields already in latency.json.
    pcts = [0, 25, 50, 75, 90, 95, 99, 100]
    rows = []
    for pct in pcts:
        key = f"p{pct}_latency_ms" if pct not in (0, 25, 75, 90, 100) else None
        if pct == 0:
            val = data.get("min_latency_ms", 0.0)
        elif pct == 100:
            val = data.get("max_latency_ms", 0.0)
        elif pct == 25:
            # Interpolate between min and p50.
            val = (data.get("min_latency_ms", 0.0) + data.get("p50_latency_ms", 0.0)) / 2
        elif pct == 75:
            val = (data.get("p50_latency_ms", 0.0) + data.get("p95_latency_ms", 0.0)) / 2
        elif pct == 90:
            val = (data.get("p50_latency_ms", 0.0) + data.get("p95_latency_ms", 0.0)) * 0.6
        else:
            val = data.get(f"p{pct}_latency_ms", 0.0)
        rows.append({"percentile": pct, "latency_ms": round(val, 2)})

    write_csv(out_path, ["percentile", "latency_ms"], rows)
    return out_path


# ---------------------------------------------------------------------------
# 3. Decision trace (decisions.ndjson)
# ---------------------------------------------------------------------------

def extract_decision_trace(run_dir: Path, out_dir: Path) -> Path:
    decisions = read_ndjson(run_dir / "decisions.ndjson")
    rows = []
    for d in decisions:
        ts = d.get("ts_wall_ms") or d.get("timestamp_ms", 0)
        rows.append({
            "ts_wall_ms":     ts,
            "scheduler_state": d.get("scheduler_state", ""),
            "action_level":    d.get("action_level", 0),
            "action_type":     d.get("action_type", ""),
            "ups":             d.get("ups", 0.0),
            "risk_score":      d.get("risk_score", 0.0),
            "dry_run":         d.get("dry_run", True),
        })
    out_path = out_dir / "decision_trace.csv"
    fields = ["ts_wall_ms", "scheduler_state", "action_level", "action_type",
              "ups", "risk_score", "dry_run"]
    write_csv(out_path, fields, rows)
    return out_path


# ---------------------------------------------------------------------------
# 4. Band timeline (events.ndjson — band_transition events only)
# ---------------------------------------------------------------------------

def extract_band_timeline(run_dir: Path, out_dir: Path) -> Path:
    events = read_ndjson(run_dir / "events.ndjson")
    rows = []
    for e in events:
        if e.get("event_type") != "band_transition":
            continue
        ts = e.get("ts_wall_ms") or e.get("timestamp_ms", 0)
        rows.append({
            "ts_wall_ms":   ts,
            "prev_band":    e.get("prev_band", ""),
            "new_band":     e.get("new_band", ""),
            "ups":          e.get("ups", 0.0),
            "dominant_signal": e.get("dominant_signal", ""),
        })
    out_path = out_dir / "band_timeline.csv"
    fields = ["ts_wall_ms", "prev_band", "new_band", "ups", "dominant_signal"]
    write_csv(out_path, fields, rows)
    return out_path


# ---------------------------------------------------------------------------
# Optional: matplotlib plots
# ---------------------------------------------------------------------------

def try_plot(pressure_csv: Path, decision_csv: Path, out_dir: Path) -> None:
    try:
        import matplotlib  # type: ignore
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt  # type: ignore
        import csv as csv_mod
    except ImportError:
        print("  [skip] matplotlib not installed — CSV files are ready for manual plotting")
        return

    # Pressure trace plot.
    ts, ups, risk = [], [], []
    with open(pressure_csv, newline="") as f:
        for row in csv_mod.DictReader(f):
            ts.append(float(row["ts_wall_ms"]) / 1000.0)
            ups.append(float(row.get("ups") or 0))
            risk.append(float(row.get("risk_score") or 0))

    if ts:
        fig, ax1 = plt.subplots(figsize=(12, 4))
        ax1.plot(ts, ups, label="UPS", color="steelblue")
        ax1.set_ylabel("UPS (0–100)", color="steelblue")
        ax1.set_xlabel("Time (s)")
        ax2 = ax1.twinx()
        ax2.plot(ts, risk, label="Risk", color="tomato", linestyle="--")
        ax2.set_ylabel("Risk Score (0–1)", color="tomato")
        ax1.set_title("Hermes Pressure Trace")
        fig.tight_layout()
        plot_path = out_dir / "pressure_trace.png"
        plt.savefig(plot_path, dpi=120)
        plt.close()
        print(f"  wrote {plot_path.name}")


# ---------------------------------------------------------------------------
# --summary: compact text summary (no files written, no matplotlib)
# ---------------------------------------------------------------------------

def _mean(values: list) -> float:
    return sum(values) / len(values) if values else 0.0


def print_summary(run_dir: Path) -> int:
    """Print a compact human-readable summary of a Hermes run directory."""
    scores    = read_ndjson(run_dir / "scores.ndjson")
    preds     = read_ndjson(run_dir / "predictions.ndjson")
    decisions = read_ndjson(run_dir / "decisions.ndjson")
    events    = read_ndjson(run_dir / "events.ndjson")
    samples   = read_ndjson(run_dir / "samples.ndjson")

    # Time window
    timestamps = [s.get("ts_wall") or s.get("ts_wall_ms", 0) for s in samples if s]
    if timestamps:
        span_ms = max(timestamps) - min(timestamps)
        span_s  = span_ms / 1000.0 if span_ms > 1_000_000 else span_ms  # handle ms vs s
        time_str = f"{span_s:.1f}s"
    else:
        time_str = "n/a"

    # UPS stats
    ups_vals  = [float(s.get("ups", 0.0)) for s in scores]
    peak_ups  = max(ups_vals) if ups_vals else 0.0
    mean_ups  = _mean(ups_vals)

    # Band distribution
    band_counts: dict = {}
    for s in scores:
        b = s.get("band", "unknown")
        band_counts[b] = band_counts.get(b, 0) + 1

    # Risk
    risk_vals  = [float(p.get("risk_score", 0.0)) for p in preds]
    peak_risk  = max(risk_vals) if risk_vals else 0.0

    # Scheduler state counts
    state_counts: dict = {}
    action_counts: dict = {}
    for d in decisions:
        st = d.get("scheduler_state", "unknown")
        state_counts[st] = state_counts.get(st, 0) + 1
        at = d.get("action_type", "none")
        if at and at != "none":
            action_counts[at] = action_counts.get(at, 0) + 1

    # Band transitions
    band_transitions = sum(1 for e in events if e.get("event_type") == "band_transition")

    # Replay summary fields (if present)
    replay_path = run_dir / "replay_summary.json"
    replay_info = ""
    if replay_path.exists():
        try:
            with open(replay_path, encoding="utf-8") as f:
                rs = json.load(f)
            valid = rs.get("valid", False)
            replay_info = f"  replay_summary.json : valid={valid}"
            if "record_counts" in rs:
                rc = rs["record_counts"]
                replay_info += f"  samples={rc.get('samples', '?')} decisions={rc.get('decisions', '?')}"
        except Exception:
            replay_info = "  replay_summary.json : (parse error)"

    sep = "-" * 56
    print(sep)
    print(f"Hermes Run Summary: {run_dir.name}")
    print(sep)
    print(f"  Run directory : {run_dir}")
    print(f"  Time span     : {time_str}  ({len(samples)} samples, {len(scores)} score records)")
    print(f"  Peak UPS      : {peak_ups:.1f}   mean={mean_ups:.1f}")
    print(f"  Peak risk     : {peak_risk:.3f}")
    print(f"  UPS bands     : " + "  ".join(f"{b}={n}" for b, n in sorted(band_counts.items())))
    print(f"  Scheduler states: " + "  ".join(f"{s}={n}" for s, n in sorted(state_counts.items())))
    if action_counts:
        print(f"  Actions       : " + "  ".join(f"{a}={n}" for a, n in sorted(action_counts.items())))
    else:
        print(f"  Actions       : none")
    print(f"  Band transitions: {band_transitions}")
    if replay_info:
        print(replay_info)
    print(sep)
    return 0


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    args = sys.argv[1:]
    do_plot    = "--plot" in args
    do_summary = "--summary" in args
    args = [a for a in args if a not in ("--plot", "--summary")]

    if not args:
        print(__doc__)
        return 1

    run_dir = Path(args[0]).resolve()

    if not run_dir.is_dir():
        print(f"ERROR: run directory not found: {run_dir}", file=sys.stderr)
        return 1

    if do_summary:
        return print_summary(run_dir)

    out_dir = Path(args[1]).resolve() if len(args) > 1 else run_dir / "plots"
    out_dir.mkdir(parents=True, exist_ok=True)
    print(f"Run directory : {run_dir}")
    print(f"Output dir    : {out_dir}")
    print("")

    pt = extract_pressure_trace(run_dir, out_dir)
    extract_latency_cdf(run_dir, out_dir)
    dt = extract_decision_trace(run_dir, out_dir)
    extract_band_timeline(run_dir, out_dir)

    if do_plot:
        try_plot(pt, dt, out_dir)

    print("")
    print("Done. Import the CSV files into matplotlib, gnuplot, or a spreadsheet.")
    print("To generate PNG plots: python3 scripts/hermes_plot.py <run-dir> --plot")
    print("For a quick text summary: python3 scripts/hermes_plot.py <run-dir> --summary")
    return 0


if __name__ == "__main__":
    sys.exit(main())
