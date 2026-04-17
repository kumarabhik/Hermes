# Hermes — Frequently Asked Questions

## General

### What problem does Hermes solve?

When multiple ML workloads share a machine — a training job, an inference server, a preprocessing script — they compete for CPU, RAM, and GPU memory. The OS scheduler does not know which process matters most. Hermes watches resource pressure in real time, predicts when a workload is about to crash, and intervenes before it does.

### Does Hermes require a GPU?

No. Hermes works on CPU-only machines using the Tier A config (`config/schema_tier_a.yaml`). GPU metrics (VRAM, utilization) are included in the Unified Pressure Score only when NVML is available. On CPU-only machines those components are zeroed out and the UPS is derived from CPU PSI, memory PSI, and IO PSI.

### Is Hermes production-ready?

The monitoring, prediction, scheduling, and dry-run execution pipeline is fully implemented and tested on Windows via a comprehensive smoke suite. Active-control mode (real SIGSTOP/SIGKILL) has been smoke-tested on Linux. Before using active-control in production, run the benchmark suite on your specific hardware to establish a T4 evidence baseline. See [docs/operator.md](operator.md).

---

## Configuration

### What is Tier A / Tier B / Tier C?

| Tier | Host | Config | Level 2 | GPU |
|---|---|---|---|---|
| A | CPU-only, no PSI | `schema_tier_a.yaml` | disabled | no |
| B | Linux + PSI, optional GPU | `schema_tier_b.yaml` | gated | optional |
| C | Linux + PSI + NVML GPU | `schema_tier_c.yaml` | enabled | required |

Start with Tier B. Enable Tier C only after running `hermesctl nvml` and confirming the NVML fast path is active.

### What is UPS and how is it calculated?

UPS (Unified Pressure Score) is a weighted sum of five resource signals, normalized to 0–100:

```
UPS = w_cpu   × cpu_some_avg10
    + w_mem   × mem_some_avg10
    + w_io    × io_some_avg10
    + w_gpu   × gpu_util_pct
    + w_vram  × vram_used_pct
```

Default weights (Tier B): cpu=0.30, mem=0.25, io=0.08, gpu=0.22, vram=0.15. The dominant signal (highest weighted contribution) is logged with every score record. You can inspect weights with `hermesctl schema`.

### How do I know if my thresholds are too tight or too loose?

Run `hermes_bench config/low_pressure_scenario.yaml --runs 3 --verify-targets` on a quiet machine. If `intervention_count > 1`, your thresholds are too tight — Hermes is triggering on noise. Use `scripts/hermes_diff.py` to compare configs and `scripts/hermes_tune.py` to get specific adjustment suggestions.

### What does the circuit breaker do?

If ≥ 4 Level-2 or Level-3 actions fire within 60 seconds, the scheduler enters a forced 120-second cooldown regardless of UPS. This prevents cascading kill storms where Hermes kills processes faster than pressure can recover. Configurable under `circuit_breaker:` in `schema_tier_c.yaml`.

### How do I add a process to the protected list?

Add the process name to `protected_names` in `config/schema.yaml`:

```yaml
protected_names:
  - my_critical_service
  - nv-hostengine
```

Hermes will never send SIGSTOP or SIGKILL to any process whose name matches a protected name. Changes take effect on the next daemon restart.

---

## Operations

### How do I run Hermes in observe-only mode?

Hermes defaults to observe-only. Just run:

```bash
./hermesd
```

No processes will be touched. All pressure readings, predictions, and decisions are written to `artifacts/logs/<run_id>/` as NDJSON.

### How do I enable active-control?

```bash
HERMES_RUNTIME_MODE=active ./hermesd
```

This enables real Level-1 reprioritization (nice values), Level-2 throttling (SIGSTOP/SIGCONT), and Level-3 kills (SIGTERM/SIGKILL). Only available on Linux. Always test in dry-run first.

### How do I check what Hermes decided during a past run?

```bash
hermes_journal artifacts/logs/<run_id>         # Markdown timeline
hermesctl logs  artifacts/logs/<run_id>        # Raw events tail
hermes_annotate artifacts/logs/<run_id>        # Plain-English rationale per decision
hermes_replay   artifacts/logs/<run_id>        # Structured JSON/CSV summary
```

### How do I check if there is headroom to launch a new workload?

```bash
hermesctl headroom                  # reads latest telemetry_quality.json
hermesctl headroom --ups 52.3       # pass current UPS directly
```

Hermes prints OK / CAUTION / DENY with headroom in UPS points to each threshold.

### What do I do if the daemon crashes?

`hermes_watchdog` monitors the control socket and restarts `hermesd` automatically:

```bash
hermes_watchdog --hermesd-bin ./hermesd --max-restarts 5
```

For production, run `hermes_watchdog` under systemd or supervisord for daemon-level HA.

### How do I export metrics to Grafana?

Run `hermes_export` alongside the daemon:

```bash
hermes_export --port 9090 --socket /tmp/hermesd.sock
```

Then add a Prometheus scrape target at `localhost:9090/metrics`. Metrics include `hermes_ups`, `hermes_risk_score`, `hermes_scheduler_state_info`, and action counters.

---

## Evidence and Benchmarking

### What is a "T0 / T1 / T2 … T5" evidence tier?

| Tier | What it proves |
|---|---|
| T0 | The pipeline runs without errors and produces valid artifacts (testable on Windows) |
| T1 | Live monitors produce non-zero PSI readings on a real Linux host |
| T2 | The predictor fires on real high-pressure events |
| T3 | An active-control intervention executes and the daemon continues |
| T4 | p95 latency or OOM count is measurably better with Hermes than without |
| T5 | strace + perf captures provide kernel-level confirmation |

### How do I collect T1 evidence?

```bash
bash scripts/hermes_quickstart.sh --tier-b
```

This checks PSI availability, builds, and runs the full smoke suite. Then run the daemon under a real workload and confirm `cpu_some_avg10 > 0` in `samples.ndjson`.

### How do I run a benchmark and compare modes?

```bash
hermes_bench config/baseline_scenario.yaml --runs 5 --run-id baseline
hermes_bench config/observe_scenario.yaml  --runs 5 --run-id observe \
    --hermes-bin build/hermesd --replay-bin build/hermes_replay
hermes_bench config/oom_stress_scenario.yaml --runs 3 \
    --hermes-bin build/hermesd --replay-bin build/hermes_replay \
    --auto-compare --verify-targets
```

### Why does hermes_eval show low precision/recall?

This usually means either: (a) the run did not contain real pressure events, so the predictor had nothing to fire on; or (b) the thresholds in `schema.yaml` are too conservative. Run `scripts/hermes_tune.py` on the eval output to get specific adjustment suggestions.

---

## Troubleshooting

### hermesctl shows "daemon not reachable"

The daemon is either not running or using a different socket path. Check:

```bash
HERMES_RUN_ID=test HERMES_MAX_LOOPS=1 ./hermesd    # test one-loop startup
hermesctl --socket /tmp/hermesd.sock ping           # check socket path
```

### hermes_pack shows 0 files copied

The run directory may not have been written yet, or the daemon exited before flushing. Check that `samples.ndjson` exists in the run directory:

```bash
ls artifacts/logs/<run_id>/
hermes_pack artifacts/logs/<run_id> --list
```

### The circuit breaker is tripping every few minutes

Your workload is generating sustained critical pressure. Either the thresholds are too low for this host, or the workload genuinely requires more resources than are available. Options:
1. Raise `elevated` and `critical` thresholds in `schema.yaml` via `hermesctl schema`.
2. Reduce the number of concurrent background workloads.
3. Increase `circuit_breaker_window_ms` to allow more interventions before tripping.
