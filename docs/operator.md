# Hermes Operator Guide

This document covers deployment assumptions, privilege modes, safety guardrails,
and the benchmark procedure for Hermes on a Linux GPU host.

---

## Deployment Assumptions

- **Host OS**: Linux with cgroup v2 mounted at `/sys/fs/cgroup` and PSI enabled.
- **GPU**: NVIDIA GPU with `nvidia-smi` on `PATH` for stats collection. Direct NVML
  integration is not yet implemented; the daemon falls back to parsing `nvidia-smi`
  query output.
- **Privilege**: The daemon does not require root by default in observe-only or advisory
  mode. Active-control mode requires write access to `/proc/<pid>/` (for `setpriority`)
  and to the target cgroup hierarchy (for `cpu.max`, `memory.high`, `cpuset.cpus`).
  SIGSTOP/SIGCONT and SIGTERM/SIGKILL require the daemon to run as the same UID as the
  target process or as root.
- **Compiler**: C++17, compiled with CMake (Linux) or directly with `g++` (authoring/smoke).
- **Config**: `config/schema.yaml` holds UPS weights, thresholds, and action-level
  enablement. See [tuning_guide.md](tuning_guide.md) for safe adjustment procedure.

---

## Runtime Modes

Select the operating mode with the `HERMES_RUNTIME_MODE` environment variable:

| Mode              | Effect                                                                                              |
|-------------------|-----------------------------------------------------------------------------------------------------|
| `observe-only`    | **Default.** Samples pressure signals, computes UPS and risk, logs all decisions — no host mutation. |
| `advisory`        | As above, plus emits advisory events via the control socket. No process or cgroup changes.          |
| `active-control`  | Real interventions: reprioritize (nice), SIGSTOP/SIGCONT throttle, SIGTERM/SIGKILL, cgroup limits. |

Start conservative: run in `observe-only` for at least one full workload cycle before enabling
`active-control`. Review the generated `decisions.ndjson` and `actions.ndjson` to confirm the
policy is targeting the right processes before granting kill/cgroup permissions.

---

## Safety Guardrails

The following guardrails are enforced in all modes and cannot be overridden at runtime:

1. **PID 1 protection**: The daemon will never send a signal or apply cgroup limits to PID 1.
2. **Self-protection**: The daemon records its own PID at startup and excludes it from every
   action candidate list.
3. **Protected-PIDs list**: Any PID listed in `protected_pids` in `config/schema.yaml` is
   unconditionally skipped.
4. **Protected name patterns**: Any process whose executable name matches a pattern in
   `protected_names` (e.g. `sshd`, `systemd`) is skipped.
5. **Dry-run fallback**: If `HERMES_RUNTIME_MODE` is unset or set to an unrecognised value
   the daemon defaults to observe-only (no-op) execution.
6. **Rollback on recovery**: `ThrottleAction` automatically calls `resume_all()` when the
   scheduler enters the `RECOVERY` state. `CgroupV2Backend.restore_all()` is called on
   daemon shutdown.

---

## Key Environment Variables

| Variable                | Default              | Description                                                     |
|-------------------------|----------------------|-----------------------------------------------------------------|
| `HERMES_RUNTIME_MODE`   | `observe-only`       | Operating mode: observe-only, advisory, active-control.         |
| `HERMES_RUN_ID`         | auto-generated       | Identifier stamped on every artifact for this run.              |
| `HERMES_SCENARIO`       | `default`            | Tag written to `run_metadata.json` for traceability.            |
| `HERMES_ARTIFACT_ROOT`  | `artifacts`          | Root directory for all generated artifacts.                     |
| `HERMES_CONFIG_PATH`    | `config/schema.yaml` | Path to the config YAML.                                        |
| `HERMES_MAX_LOOPS`      | unlimited            | Cap on daemon iterations; use a small value for smoke runs.     |
| `HERMES_SOCKET_PATH`    | `/tmp/hermesd.sock`  | Unix domain socket path for `hermesctl` connection.             |

---

## Benchmark Procedure

### Prerequisites

- Linux host with NVIDIA GPU, `nvidia-smi`, Python 3, and `stress-ng` (or equivalent).
- Hermes compiled with CMake: `cmake -S . -B build && cmake --build build`.
- A scenario YAML file. Generate defaults:
  ```
  hermes_bench --generate-baseline baseline_scenario.yaml
  hermes_bench --generate-active   active_scenario.yaml
  ```

### Step-by-step

1. **Establish a baseline** (no Hermes):
   ```
   hermes_bench baseline_scenario.yaml \
     --artifact-root artifacts \
     --run-id baseline-run-01
   ```
   Repeat 5 times (`--runs 5`) to get stable completion rates.

2. **Observe-only run** (Hermes present, no mutation):
   ```
   hermes_bench active_scenario.yaml \
     --artifact-root artifacts \
     --run-id observe-run-01 \
     --runs 5
   ```
   Compare `completion_rate` and `intervention_count` from the generated `*-summary.json` files.

3. **Active-control run** (Hermes with real interventions — Linux only):
   ```
   HERMES_RUNTIME_MODE=active-control \
   hermes_bench active_scenario.yaml \
     --artifact-root artifacts \
     --run-id active-run-01 \
     --runs 5
   ```

4. **Generate comparison table**:
   ```
   hermes_compare --bench-dir artifacts/bench --output-csv artifacts/bench/comparison.csv
   ```

5. **Re-evaluate decisions offline**:
   ```
   hermes_reeval artifacts/logs/<run-id>
   ```

### Artifact Locations

| Artifact                              | Description                                              |
|---------------------------------------|----------------------------------------------------------|
| `artifacts/logs/<run-id>/`            | Per-run NDJSON logs (samples, scores, decisions, events) |
| `artifacts/bench/<run-id>-summary.json` | Benchmark summary with completion_rate and peaks         |
| `artifacts/bench/<run-id>-plan.json`  | Plan artifact with scenario snapshot                     |
| `artifacts/bench/comparison.csv`      | Cross-mode comparison table from `hermes_compare`        |
| `artifacts/summaries/`                | Replay CSV rows, one per run                             |

---

## Smoke Verification (Windows / authoring shells)

All smoke scripts are under `scripts/` and use direct `g++` compilation without CMake:

```powershell
.\scripts\smoke_synth_replay.ps1
.\scripts\smoke_daemon_replay.ps1
.\scripts\smoke_benchmark_plan.ps1
.\scripts\smoke_benchmark_launch.ps1
.\scripts\smoke_benchmark_hermes.ps1
.\scripts\smoke_benchmark_compare.ps1
```

These validate artifact generation and binary plumbing without requiring a Linux GPU.
They do not produce performance evidence.
