# Hermes Internals

This document describes the native collector path, multi-threaded daemon,
replay and fault injection workflow, and the cgroup v2 backend.

---

## Module Layout

```
include/hermes/
  monitor/      — pressure and GPU collectors
  profiler/     — /proc parsers, process mapper, workload classifier
  engine/       — UPS calculator, predictor, scheduler
  actions/      — executors (reprioritize, throttle, kill, cgroup, dry-run)
  runtime/      — event logger, run metadata, telemetry, EventBus, control socket
src/cli/
  hermesd.cpp       — single-threaded daemon
  hermesd_mt.cpp    — multi-threaded daemon (sampler + policy via EventBus)
  hermes_replay.cpp — offline replay and summary
  hermes_synth.cpp  — deterministic synthetic fixture generator
  hermes_eval.cpp   — offline predictor evaluator (precision/recall/F1)
  hermes_reeval.cpp — re-execute saved samples through the real pipeline
  hermes_fault.cpp  — labeled fault injection fixtures
  hermes_bench.cpp  — benchmark scenario harness
  hermes_compare.cpp — benchmark comparison aggregator
  hermes_report.cpp — multi-run replay summary table
  hermesctl.cpp     — live ANSI dashboard via control socket
```

---

## Native Collector Path

The daemon reads directly from Linux pseudo-filesystems on every sample cycle:

| Signal            | Source                     | Fields                                          |
|-------------------|----------------------------|-------------------------------------------------|
| CPU PSI           | `/proc/pressure/cpu`       | `some_avg10`, `some_avg60`, `some_avg300`        |
| Memory PSI        | `/proc/pressure/memory`    | `some_avg10`, `full_avg10`                      |
| IO PSI            | `/proc/pressure/io`        | `io_some_avg10`, `io_full_avg10`                |
| vmstat            | `/proc/vmstat`             | `pgfault` delta, `pgmajfault` delta             |
| Load average      | `/proc/loadavg`            | `load1`, `load5`, `load15`, `runnable`          |
| Per-process stats | `/proc/<pid>/stat`         | CPU time, state, RSS, nice value                |
| GPU stats         | `nvidia-smi` query         | `gpu_util_pct`, `vram_used_mb`, `vram_total_mb` |

All reads are done in C++ without external library dependencies (no libprocps, no libnvidia).
The `nvidia-smi` path spawns a child process; direct NVML integration is planned but not yet
implemented.

All signals are assembled into a `PressureSample` struct each cycle and passed to the UPS
calculator.

---

## UPS (Unified Pressure Score)

The `PressureScoreCalculator` in `engine/pressure_score.*` combines raw signals into a
single bounded score:

```
UPS = w_cpu  * cpu_some_avg10
    + w_mem  * mem_full_avg10
    + w_io   * io_full_avg10
    + w_vram * vram_utilization
    + w_load * load_norm
```

Weights and band thresholds are loaded from `config/schema.yaml`. The calculator is
stateful: it tracks the last N band transitions and emits a `band_transition` event to
`events.ndjson` whenever the UPS band changes.

Bands: `normal` → `elevated` → `critical`.

---

## OOM Predictor

`engine/predictor.*` computes a risk score from:
- Dual-window VRAM slope (3-second fast window + 10-second medium window).
- Per-PID GPU memory growth tracking.
- Four sustained pressure residency counters (CPU full, mem full, IO full, composite).
- Headroom collapse: `vram_free_mb / vram_total_mb` below configured threshold.

Output is a structured prediction record with `risk_score`, `reason_codes`,
`estimated_lead_time_s`, and `recommended_actions`, written to `predictions.ndjson`.

---

## Multi-threaded Daemon (hermesd_mt)

`hermesd_mt` uses two threads connected by `EventBus<T>`:

```
Sampler Thread                    Policy Thread
──────────────                    ─────────────
collect PSI + GPU  →  EventBus  → score + predict + decide + act + log
```

`EventBus<T>` is a bounded MPSC ring buffer in `include/hermes/runtime/event_bus.hpp`.
If the policy thread falls behind, the ring buffer drops the oldest sample (bounded drop,
never blocks the sampler). Drop counts are tracked in `telemetry_quality.json`.

---

## Replay Workflow

```
hermes_replay <run-dir> [artifact-root]
```

1. Reads `run_metadata.json` and `config_snapshot.yaml` from the run directory.
2. Verifies that all expected NDJSON artifact files are present.
3. Computes peak fields from `scores.ndjson` and `predictions.ndjson`.
4. Writes `replay_summary.json` and `summary.csv` under the run directory and
   under `artifacts/replay/` and `artifacts/summaries/`.

```
hermes_reeval <run-dir>
```

Re-reads saved `samples.ndjson` and passes each sample through the real
`PressureScoreCalculator`, `OomPredictor`, and `Scheduler` pipeline.
Computes action/state/band match rates and UPS/risk RMSE against the
originally-logged decisions.

---

## Fault Injection

```
hermes_fault --output-dir artifacts/fault [--scenario NAME]
```

Generates six labeled NDJSON sample fixtures:

| Fixture           | What it simulates                                     |
|-------------------|-------------------------------------------------------|
| `vram_spike`      | Sudden VRAM spike toward OOM headroom threshold       |
| `mem_storm`       | Sustained high memory PSI full                        |
| `cpu_hog`         | Sustained high CPU PSI some                           |
| `io_storm`        | Sustained high IO PSI full + pgmajfault burst         |
| `mixed_pressure`  | Simultaneous CPU + memory + VRAM elevation            |
| `oom_imminent`    | Rapid VRAM growth + memory PSI full convergence       |

Each fixture is a deterministic NDJSON sequence; the same seed produces the
same trace across runs. Use `hermes_reeval` to replay fault fixtures through
the real pipeline and verify coverage of Level 1–3 intervention paths.

---

## Cgroup v2 Backend

`CgroupV2Backend` (in `actions/`) manages per-workload cgroup resources on Linux:

| Control file    | What it limits                                      |
|-----------------|-----------------------------------------------------|
| `cpu.max`       | CPU bandwidth quota (microseconds per period)       |
| `memory.high`   | Soft memory high-water-mark; triggers reclaim early |
| `cpuset.cpus`   | CPU affinity mask for the cgroup                    |

Before applying any limit, the backend reads and saves the current value for each
control file. `restore_all()` restores every saved value and is called automatically
on daemon shutdown.

All cgroup operations are compile-guarded with `#ifdef __linux__` and are no-ops on
non-Linux platforms.

---

## Control Socket

The daemon opens a Unix domain socket at `HERMES_SOCKET_PATH` (default `/tmp/hermesd.sock`).
`hermesctl` connects to this socket and periodically requests a JSON status blob:

```json
{
  "run_id": "...",
  "loops": 42,
  "ups": 0.31,
  "risk_score": 0.07,
  "scheduler_state": "NORMAL",
  "last_action": "none",
  "drop_count": 0
}
```

`hermesctl --once` prints one status snapshot. `hermesctl --interval-ms 500` refreshes
an ANSI terminal dashboard every 500 ms.
