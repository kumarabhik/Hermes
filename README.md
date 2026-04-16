# Hermes: Unified CPU-GPU Resource Orchestrator

Hermes is a single-host control-plane for machine learning workloads that treats CPU pressure, system memory pressure, GPU utilization, and GPU memory pressure as one coordinated scheduling problem over local ML workloads.

## Vision

The v1 success condition is to reduce avoidable failures and severe pressure dwell time while protecting a foreground ML workload under contention.

See `design.md` for architectural concepts and `roadmap.md` for the current progress.

## Build Setup

Requires a native Linux target with PSI support or can act as an observe-only daemon in WSL.

```bash
mkdir build
cd build
cmake ..
make
```

## Running the Daemon

```bash
./hermesd
```

Hermes defaults to an observe-only mode, and safely handles missing telemetry providers (like `PSI` on Windows machines).

## Runtime Artifacts

Each daemon start creates a run directory under `artifacts/logs/<run_id>/` and writes dependency-free NDJSON files:

- `run_metadata.json`
- `config_snapshot.yaml`
- `telemetry_quality.json`
- `samples.ndjson`
- `processes.ndjson`
- `scores.ndjson`
- `predictions.ndjson`
- `decisions.ndjson`
- `actions.ndjson`
- `events.ndjson`

Every structured record carries `run_id`, `scenario`, and `config_hash`. By default, Hermes derives a run id from wall-clock time and computes a simple hash from `config/schema.yaml`. `run_metadata.json` records host/runtime facts and feature probes, `config_snapshot.yaml` preserves the config file used for the run, and `telemetry_quality.json` tracks provider availability, loop cadence, process refreshes, decisions, actions, and peak pressure/risk observed so far.

Useful environment overrides:

```bash
HERMES_RUN_ID=dev-smoke HERMES_SCENARIO=observe HERMES_MAX_LOOPS=1 ./hermesd
```

`HERMES_CONFIG_PATH` can point at a different config file for the snapshot and default hash. `HERMES_MAX_LOOPS` is intended for smoke checks so the daemon can verify one or more control-loop iterations without running forever.

## Replay Summary

`hermes_replay` reads a Hermes run directory and produces a compact validation summary:

```bash
./hermes_replay artifacts/logs/dev-smoke
```

It writes `replay_summary.json` and `summary.csv` beside the run, then copies the JSON and CSV summaries to `artifacts/replay/<run_id>-summary.*` and `artifacts/summaries/<run_id>-summary.*`. The current replay tool validates record identity fields and summarizes counts, time window, pressure/risk peaks, scheduler states, actions, event kinds, manifest assertions, and whether metadata, config snapshot, and telemetry quality artifacts are present. Full deterministic scheduler re-execution is still a later milestone.

If a run contains `scenario_manifest.json`, `hermes_replay` treats its expectations as assertions. It can check named signals, minimum peak UPS, minimum peak risk, minimum action counts, minimum scheduler state counts, and minimum pressure or risk band counts. Missing expectations mark the summary invalid and make the CLI exit nonzero.

## Synthetic Fixture

`hermes_synth` generates a deterministic pressure fixture without needing live GPU contention:

```bash
./hermes_synth synthetic-demo
./hermes_replay artifacts/logs/synthetic-demo
```

The fixture writes the same run artifacts as the daemon plus `scenario_manifest.json`. It drives the real UPS, predictor, scheduler, dry-run executor, event logger, metadata writer, and telemetry quality tracker with synthetic samples that exercise elevated, throttled, cooldown, and recovery paths. This is useful for replay and schema checks, but it is not benchmark evidence.

## Smoke Check

On Windows or a lightweight shell with `g++`, run the synthetic replay smoke check:

```powershell
.\scripts\smoke_synthetic_replay.ps1
```

The script builds `hermes_synth` and `hermes_replay`, generates a synthetic run, replays it, checks the JSON and CSV summary copies in `artifacts/summaries/`, and fails if the manifest assertions do not pass.

To smoke-check the real daemon artifact path without leaving the daemon running forever:

```powershell
.\scripts\smoke_daemon_replay.ps1
```

That script builds `hermesd` and `hermes_replay`, runs the daemon with `HERMES_MAX_LOOPS=1` in observe-only mode, verifies the daemon NDJSON and metadata artifacts, replays the run, and checks the JSON and CSV summaries.

To smoke-check benchmark scenario planning without launching real workloads:

```powershell
.\scripts\smoke_benchmark_plan.ps1
```

That script builds `hermes_bench`, generates a default `baseline` scenario, runs a dry-run benchmark plan, and verifies the plan JSON plus scenario snapshot under `artifacts/bench/`.

To smoke-check bounded benchmark workload launch and summary writing:

```powershell
.\scripts\smoke_benchmark_launch.ps1
```

That script builds `hermes_bench`, runs a short four-workload `baseline` scenario with local commands, verifies the plan and scenario snapshot, and checks the benchmark run summary under `artifacts/bench/`.

To smoke-check a benchmark run with Hermes in observe-only mode plus replay capture:

```powershell
.\scripts\smoke_benchmark_hermes.ps1
```

That script builds `hermes_bench`, `hermesd`, and `hermes_replay`, runs a short four-workload `observe-only` scenario, verifies the Hermes run directory under `artifacts/logs/`, and checks that the benchmark summary embeds replay counts and peak fields from the replay output.

To smoke-check `hermes_compare` across a baseline and an observe-only run:

```powershell
.\scripts\smoke_benchmark_compare.ps1
```

That script builds `hermes_bench`, `hermes_compare`, `hermesd`, and `hermes_replay`, runs one baseline and one observe-only benchmark, then verifies that `hermes_compare` produces a comparison CSV containing both modes and that the enriched summary fields (`completion_rate`, `intervention_count`, `oom_count`, `degraded_behavior`) are present.

## Benchmark Comparison

`hermes_compare` reads all `*-summary.json` files under `artifacts/bench/` and produces a
comparison table across baseline, observe-only, and active-control runs:

```bash
hermes_compare --bench-dir artifacts/bench --output-csv artifacts/bench/comparison.csv
```

The summary artifacts themselves are enriched with derived comparison fields:

| Field | Description |
| --- | --- |
| `completion_rate` | `jobs_completed / launched` for this run. |
| `intervention_count` | Number of Hermes actions observed via the embedded replay snapshot. |
| `oom_count` | Placeholder; real values require Linux kernel OOM tracing. |
| `degraded_behavior` | `true` when `completion_rate < min_target` or nonzero exits occurred. |

Multi-run benchmarks: use `--runs N` to repeat a scenario N times with sequential run IDs:

```bash
hermes_bench baseline_scenario.yaml --runs 5 --run-id baseline
```

Generate scenario configs from the CLI:

```bash
hermes_bench --generate-baseline   baseline_scenario.yaml
hermes_bench --generate-active     active_scenario.yaml
hermes_bench --generate-oom-stress oom_stress_scenario.yaml
```

A pre-built OOM-stress scenario is also available at [config/oom_stress_scenario.yaml](config/oom_stress_scenario.yaml).

Use `--verify-targets` to exit non-zero if any run misses latency or OOM assertions:

```bash
hermes_bench config/oom_stress_scenario.yaml --runs 3 \
    --hermes-bin build/hermesd --replay-bin build/hermes_replay \
    --auto-compare --verify-targets
```

Check NVML availability and device info without a running daemon:

```bash
hermesctl nvml
```

## Documentation

| Document | Contents |
| --- | --- |
| [docs/operator.md](docs/operator.md) | Deployment assumptions, privilege modes, safety guardrails, benchmark procedure |
| [docs/internals.md](docs/internals.md) | Native collector path, multi-threaded daemon, replay, fault injection, cgroup backend |
| [docs/tuning_guide.md](docs/tuning_guide.md) | UPS weights, thresholds, cooldowns, protection rules — safe adjustment procedure |
| [docs/wsl2_quickstart.md](docs/wsl2_quickstart.md) | WSL2 build guide, PSI/NVML setup, smoke suite, perf/strace capture, compatibility table |
| [design.md](design.md) | Architecture, intervention policy, session handoff log |
| [roadmap.md](roadmap.md) | Phase-by-phase status with `[x]`/`[~]`/`[ ]` evidence tracking |

## Achieved Outcomes (Authoring Environment)

The following outcomes are evidenced by artifacts in this repo and smoke run outputs.
All smoke runs use direct `g++` compilation on Windows (no Linux GPU required).

| Capability | Evidence artifact or binary |
| --- | --- |
| Observe-only daemon artifact set | `smoke_daemon_replay.ps1` → `artifacts/logs/*/` |
| Deterministic synthetic replay | `smoke_synthetic_replay.ps1` → `artifacts/logs/synthetic-*/` |
| Benchmark plan artifacts | `smoke_benchmark_plan.ps1` → `artifacts/bench/*-plan.json` |
| Bounded workload launch + summary | `smoke_benchmark_launch.ps1` → `artifacts/bench/*-summary.json` |
| Benchmark + Hermes observe-only | `smoke_benchmark_hermes.ps1` → `artifacts/bench/*-summary.json` with embedded replay snapshot |
| Baseline vs observe-only comparison | `smoke_benchmark_compare.ps1` → `artifacts/bench/comparison.csv` |
| Offline predictor evaluation | `hermes_eval` → `eval_summary.json` |
| Fault injection fixtures | `hermes_fault` → `artifacts/fault/` |
| Multi-run replay comparison | `hermes_report` → console table + CSV |

**Also evidenced (smoke environment):**

| Capability | Evidence artifact or binary |
| --- | --- |
| NVML direct GPU fast path | `hermesctl nvml` → device info without nvidia-smi subprocess |
| Active-control end-to-end smoke | `smoke_active_control.ps1` → `artifacts/bench/*-summary.json` with `latency_target_met` |
| Scheduler state transition coverage | `hermes_reeval` → `artifacts/logs/*/state_coverage.json` |
| p95 latency assertion in summaries | `hermes_bench` → `p95_latency_ms`, `latency_target_ms`, `latency_target_met` in summary JSON |

**Not yet evidenced** (requires Linux / WSL2 with real workloads):

- Real PSI readings under production workload pressure
- NVML fast path with a real GPU (CUDA for WSL2 or native Linux)
- Active-control intervention reducing p95 latency vs. baseline
- `strace` / `perf stat` / `gdb` captures from live benchmark runs

Run `bash scripts/collect_wsl2_evidence.sh` to collect all evidence in one pass on WSL2.

## Key Results

> **Status (2026-04-16): T0 pipeline complete; T1-T5 pending Linux run.**
> This section will be filled in once benchmark runs complete on a native Linux / WSL2 GPU host.
> All values below are placeholders. Replace them with real numbers and artifact paths.

### Before / After: p95 Foreground Latency

| Scenario | Mode | p95 Latency (ms) | OOM Kills | Completion Rate | Hermes Actions |
| --- | --- | --- | --- | --- | --- |
| `baseline_scenario.yaml` | baseline (no Hermes) | — | — | — | n/a |
| `observe_scenario.yaml` | observe-only | — | — | — | 0 (observe) |
| `oom_stress_scenario.yaml` | active-control | — | — | — | — |

_Run `hermes_bench --runs 5 --delta-vs artifacts/bench/<baseline-run>-latency.json` to populate this table._

### Predictor Quality (eval_summary.json)

| Metric | Value | Target | Status |
| --- | --- | --- | --- |
| Precision | — | ≥ 0.85 | pending |
| Recall | — | ≥ 0.80 | pending |
| F1 | — | ≥ 0.80 | pending |
| Mean lead time (s) | — | ≥ 3.0 s | pending |
| False positive rate (/hr) | — | < 5 / hr | pending |

_Run `python3 scripts/hermes_tune.py --eval-dir artifacts/logs` to generate this table._

### False Positive Baseline (low-pressure host)

| Scenario | Active-control interventions | Target |
| --- | --- | --- |
| `low_pressure_scenario.yaml` | — | ≤ 1 |

_Run `hermes_bench config/low_pressure_scenario.yaml --verify-targets` to populate this row._

### Evidence Tier Status

_Run `python3 scripts/check_evidence_tiers.py` to see which T0-T5 tiers are currently met._

To collect all T1-T4 evidence in one pass on a Linux host:

```bash
bash scripts/smoke_phase6.sh
```

Full evidence details and artifact paths: [RESULTS.md](RESULTS.md)
