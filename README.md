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

That script builds `hermes_bench`, generates a default baseline scenario, runs a dry-run benchmark plan, and verifies the plan JSON plus scenario snapshot under `artifacts/bench/`.

To smoke-check bounded benchmark workload launch and summary writing:

```powershell
.\scripts\smoke_benchmark_launch.ps1
```

That script builds `hermes_bench`, runs a short four-workload benchmark scenario with local commands, verifies the plan and scenario snapshot, and checks the benchmark run summary under `artifacts/bench/`.
