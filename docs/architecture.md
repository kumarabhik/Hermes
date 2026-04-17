# Hermes Architecture

## Overview

Hermes is a single-host control-plane for ML workloads. It treats CPU pressure,
system memory pressure, IO pressure, GPU utilisation, and VRAM as one coordinated
scheduling problem and intervenes with layered controls before an OOM event occurs.

---

## Full Pipeline

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                            HERMES DAEMON (hermesd / hermesd_mt)             │
│                                                                             │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │                        MONITOR LAYER                                 │   │
│  │                                                                      │   │
│  │  /proc/pressure/cpu  ──► CpuPsiMonitor   ─┐                          │   │
│  │  /proc/pressure/mem  ──► MemPsiMonitor   ─┤                          │   │
│  │  /proc/pressure/io   ──► IoPsiMonitor    ─┤                          │   │
│  │  /proc/vmstat        ──► VmstatMonitor   ─┤──► PressureSample        │   │
│  │  /proc/loadavg       ──► LoadavgMonitor  ─┤    (ts_wall, ts_mono,    │   │
│  │  nvidia-smi / NVML   ──► GpuStatsCollect─┤     psi fields,           │   │
│  │  /proc/<pid>/stat    ──► ProcStatParser  ─┘     vram_used_mb, …)     │   │
│  │  /proc/<pid>/status  ──► RichProcReader                              │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                          │                                                  │
│                          ▼                                                  │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │                       PROFILER LAYER                                 │   │
│  │                                                                      │   │
│  │  NvmlBackend (dlopen) ──► query_all_processes() ─┐                   │   │
│  │  nvidia-smi fallback  ──────────────────────────►│                   │   │
│  │                                                   │                  │   │
│  │  ProcessMapper ◄──────────────────────────────────┘                  │   │
│  │    • Merges /proc PID metadata with GPU-per-PID attribution          │   │
│  │    • WorkloadClassifier labels: training / inference / background    │   │
│  │    • Marks foreground and protected PIDs                             │   │
│  │    • Emits: ProcessSnapshot[] → processes.ndjson                     │   │
│  └──────────────────────────────────────────────────────────────────────┘   │ 
│                          │                                                  │
│                          ▼                                                  │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │                        ENGINE LAYER                                   │  │
│  │                                                                       │  │
│  │  PressureScoreCalculator                                             │   │
│  │    • Normalises PSI + GPU + VRAM into weighted UPS (0-100)           │   │
│  │    • Tracks band transitions (normal / elevated / critical)          │   │
│  │    • Emits: PressureScore → scores.ndjson, events.ndjson             │   │
│  │                          │                                            │  │
│  │                          ▼                                            │  │
│  │  OomPredictor                                                        │   │
│  │    • Dual-window VRAM slopes (3 s fast + 10 s medium)                │   │
│  │    • 4 sustained pressure residency counters                         │   │
│  │    • Per-PID GPU growth tracking across windows                      │   │
│  │    • Emits: RiskPrediction → predictions.ndjson                      │   │
│  │             (risk_score, risk_band, lead_time_s, reason_codes)       │   │
│  │                          │                                            │  │
│  │                          ▼                                            │  │
│  │  Scheduler (state machine)                                           │   │
│  │    States: Normal ─► Elevated ─► Throttled ─► Cooldown ─► Recovery  │    │
│  │    • Combines UPS band + risk band + workload class + cooldowns       │  │
│  │    • Circuit breaker: ≥4 L2/L3 actions in 60 s → forced cooldown     │   │
│  │    • Emits: InterventionDecision → decisions.ndjson                  │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                          │                                                  │
│                          ▼                                                  │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │                        ACTION LAYER                                   │  │
│  │                                                                       │  │
│  │  ActiveExecutor (mode dispatch)                                       │  │
│  │    ├── observe-only  ──► DryRunExecutor (log only, no mutation)       │  │
│  │    ├── advisory      ──► DryRunExecutor + advisory log                │  │
│  │    └── active-control                                                 │  │
│  │         ├── Level 1: ReprioritizeAction (setpriority / nice)         │   │
│  │         ├── Level 2: ThrottleAction (SIGSTOP / SIGCONT)               │  │
│  │         │           CgroupV2Backend (cpu.max, memory.high, cpuset)   │   │
│  │         └── Level 3: KillAction (SIGTERM / SIGKILL)                  │   │
│  │                      sort_by_placement() for multi-GPU targeting     │   │
│  │    Emits: InterventionResult → actions.ndjson                        │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                          │                                                  │
│                          ▼                                                  │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │                        RUNTIME LAYER                                  │  │
│  │                                                                       │  │
│  │  EventLogger     → per-run NDJSON artifact set (see below)           │   │
│  │  RunMetadata     → run_metadata.json                                 │   │
│  │  TelemetryQuality→ telemetry_quality.json                            │   │
│  │  LatencyProbe    → latency_summary.json (p50/p95/p99/max loop time)  │   │
│  │  ControlSocket   → Unix domain socket → hermesctl / hermes_web       │   │
│  │  EventBus<T>     → bounded MPSC ring buffer (hermesd_mt sampler↔policy)│ │
│  └──────────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## Multi-threaded Daemon (hermesd_mt)

```
Thread 1: Sampler                     Thread 2: Policy
────────────────                      ──────────────────────────────
Monitor Layer                         EventBus<PressureSample> pop()
  │                                         │
  ├─ CpuPsiMonitor.read()             Engine Layer (UPS + Predictor)
  ├─ MemPsiMonitor.read()                   │
  ├─ IoPsiMonitor.read()              Scheduler.evaluate()
  ├─ VmstatMonitor.read()                   │
  ├─ GpuStatsCollector.read()         ActiveExecutor.execute()
  ├─ ProcessMapper.refresh()                │
  └─ stamp_pressure_sample()          EventLogger.log_*(…)
        │
        ▼
  EventBus<PressureSample>.push()
  (bounded ring buffer — drops on overflow, tracks drop_count)
```

---

## Artifact Layout

```
artifacts/
├── logs/
│   └── <run_id>/
│       ├── run_metadata.json       Host facts, feature probes, timestamps
│       ├── config_snapshot.yaml    Config file used for this run
│       ├── telemetry_quality.json  Provider availability, peaks, action counts
│       ├── latency_summary.json    Policy loop p50/p95/p99/max latency
│       ├── samples.ndjson          Raw PressureSample per loop iteration
│       ├── processes.ndjson        ProcessSnapshot[] per refresh cycle
│       ├── scores.ndjson           PressureScore per sample (UPS, band)
│       ├── predictions.ndjson      RiskPrediction per sample
│       ├── decisions.ndjson        InterventionDecision per sample
│       ├── actions.ndjson          InterventionResult per executed action
│       ├── events.ndjson           Band transitions, state changes, custom events
│       ├── scenario_manifest.json  Assertion expectations (synthetic fixtures)
│       ├── replay_summary.json     Written by hermes_replay
│       ├── replay_eval.ndjson      Written by hermes_reeval
│       ├── state_coverage.json     Written by hermes_reeval
│       └── eval_summary.json       Written by hermes_eval
├── bench/
│   ├── <run_id>-plan.json          Scenario plan artifact
│   ├── <run_id>-summary.json       Benchmark run summary (p95, OOM, completion)
│   ├── <run_id>-latency.json       Foreground workload latency across --runs N
│   └── comparison.csv              hermes_compare output
├── replay/                         hermes_replay summary copies
├── summaries/                      hermes_replay CSV copies
└── evidence/
    └── <run_id>/                   collect_wsl2_evidence.sh output tree
```

---

## CLI Tools

| Binary | Purpose |
|---|---|
| `hermesd` | Single-threaded daemon (observe-only / active-control) |
| `hermesd_mt` | Multi-threaded daemon (sampler + policy threads via EventBus) |
| `hermesctl` | Live terminal dashboard + subcommands: top, headroom, schema, bench, diff, eval, nvml |
| `hermes_replay` | Validates run artifacts and writes replay_summary.json |
| `hermes_reeval` | Re-runs samples through real pipeline; computes match rates and state_coverage.json |
| `hermes_synth` | Generates deterministic synthetic pressure fixtures |
| `hermes_fault` | Generates labeled fault-injection fixtures (8 scenarios) |
| `hermes_simulate` | Full pipeline from any samples.ndjson — Windows-testable |
| `hermes_eval` | Offline predictor evaluation: precision, recall, F1, FP rate/hr |
| `hermes_bench` | Benchmark harness: scenario launch, multi-run, --auto-compare, --smoke-all |
| `hermes_compare` | Cross-mode summary aggregator (baseline vs observe vs active) |
| `hermes_report` | Multi-run artifact report table + CSV |
| `hermes_alert` | Webhook notifier on Throttled/Cooldown/Elevated state entry |
| `hermes_web` | Embedded HTTP server — live browser dashboard at :7070 |
| `hermes_export` | Prometheus metrics exporter at :9090/metrics |
| `hermes_watchdog` | hermesd health monitor with auto-restart |

---

## Key Design Invariants

1. **No single point of failure** — missing telemetry providers (no PSI, no GPU) return zero values; UPS scores accordingly; daemon continues.
2. **Mode safety** — `active-control` mutations are compile-guarded for Linux; Windows always executes dry-run path.
3. **Cooldown prevents thrashing** — L1/L2 per-PID cooldowns + L3 global cooldown + circuit breaker prevent cascading kills.
4. **Reversal conditions** — every action emits `reversal_condition` describing the exact pressure/cycle threshold for undo.
5. **Deterministic replay** — `hermes_reeval` re-runs the same samples through the same pipeline and computes match rates; fixtures are hash-seeded.
6. **Evidence tiers** — T0 (pipeline correct) → T1 (live monitors) → T2 (predictor fires) → T3 (intervention fires) → T4 (intervention helps) → T5 (defensible captures).

---

## Scheduler State Machine

```
                      ┌─────────────────────────────┐
                      │          Normal              │◄─────────────────┐
                      └──────────────┬──────────────┘                  │
                    UPS elevated or   │                       stable_cycles >= N
                    risk medium       │                                  │
                                      ▼                                  │
                      ┌─────────────────────────────┐                  │
                      │         Elevated             │──────────────────┤
                      └──────────────┬──────────────┘                  │
                    UPS critical or   │                       stable_cycles >= N
                    risk high         │                                  │
                                      ▼                                  │
                      ┌─────────────────────────────┐                  │
                      │         Throttled            │                  │
                      └──────────────┬──────────────┘                  │
                   Level 3 fired or   │                                  │
                   L3 cooldown active │                                  │
                                      ▼                                  │
                      ┌─────────────────────────────┐                  │
                      │          Cooldown            │──────────────────┤
                      └──────────────┬──────────────┘   stable_cycles >= N
                    stable_cycles     │                                  │
                    >= N              ▼                                  │
                      ┌─────────────────────────────┐                  │
                      │          Recovery            │──────────────────┘
                      └─────────────────────────────┘
                        (pressure spikes → Elevated)

  Circuit breaker: ≥4 L2/L3 in 60s → forced Cooldown for 120s
```
