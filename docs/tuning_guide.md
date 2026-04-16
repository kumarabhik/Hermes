# Hermes Tuning Guide

This guide explains how to safely adjust UPS weights, thresholds, cooldowns,
and protection rules in `config/schema.yaml`.

---

## General Principles

- Always start in `observe-only` mode and collect a baseline run before tightening any threshold.
- Change one parameter at a time; run `hermes_reeval` on saved artifacts to measure impact
  before deploying to a live workload.
- Never lower `level_3_global_sec` below 60 seconds without extensive testing — premature
  kill actions on the wrong process can abort work that takes hours to recover.

---

## UPS Weights

```yaml
ups_weights:
  cpu_some: 0.20
  mem_some: 0.175
  mem_full: 0.175   # mem_some + mem_full = 0.35
  gpu_util: 0.15
  vram_used: 0.30
```

Weights must sum to 1.0. Change them if your workload type makes one signal more predictive
of real degradation:

| Workload type       | Recommended adjustment                                                 |
|---------------------|------------------------------------------------------------------------|
| Memory-bound ML     | Increase `mem_full` to 0.25, reduce `cpu_some` to 0.10.              |
| Compute-bound ML    | Increase `gpu_util` to 0.25, reduce `mem_some` to 0.10.              |
| IO-heavy data prep  | Add explicit `io_full` weight once IO PSI is included in the formula. |
| Balanced inference  | Default weights are reasonable; adjust based on observed band history. |

After changing weights, run `hermes_reeval` on at least one saved run directory to
verify that band transitions move in the expected direction.

---

## UPS Thresholds

```yaml
thresholds:
  ups:
    elevated: 40    # UPS >= 40 → ELEVATED scheduler state
    critical: 70    # UPS >= 70 → CRITICAL scheduler state
```

**Raising `elevated`** reduces false-positive intervention triggers at the cost of reacting
later to real degradation. Useful if you observe excessive Level 1 reprioritizations during
normal variance.

**Lowering `critical`** makes the system more aggressive about Level 2/3 actions. Only do
this after confirming that the predictor's precision at that UPS level is high (check
`eval_summary.json` from `hermes_eval`).

---

## Memory PSI Thresholds

```yaml
thresholds:
  mem_psi:
    elevated_full_avg10: 2.0   # % of time all tasks blocked
    critical_full_avg10: 5.0
```

`mem_full_avg10` above 2% is already a meaningful signal on a lightly-loaded host.
On a host running multiple large ML jobs, normal variance may be higher — raise these
thresholds to avoid spurious policy triggers.

---

## VRAM Thresholds

```yaml
thresholds:
  vram:
    high_pct: 90     # VRAM utilization >= 90% contributes to elevated UPS
    critical_pct: 95 # VRAM utilization >= 95% triggers heightened predictor risk
  predictor:
    high_risk: 0.70  # Risk score >= 0.70 triggers recommended intervention
```

`critical_pct: 95` is conservative. If your model allocates in large chunks you may see
sudden jumps past 95% with no warning. In that case, lower `critical_pct` to 88–90% to
give the predictor more lead time.

---

## Cooldowns

```yaml
cooldowns:
  level_1_pid_sec: 15     # Seconds before re-reprioritizing the same PID
  level_2_pid_sec: 20     # Seconds before re-throttling the same PID
  level_3_global_sec: 300 # Global cooldown between any kill actions
  recovery_hysteresis_sec: 10  # Seconds of low UPS required to declare recovery
```

**`level_1_pid_sec`**: Keep >= 10 seconds. Rapid nice-value changes confuse the kernel
scheduler and rarely help.

**`level_2_pid_sec`**: Keep >= 15 seconds. SIGSTOP/SIGCONT in rapid succession can
cause visible stuttering in foreground work.

**`level_3_global_sec`**: Keep >= 120 seconds in production. 300 seconds (5 minutes) is
the recommended minimum. Killing processes too frequently is rarely the right answer —
if Level 3 is firing often, raise `critical` thresholds and investigate the root cause.

**`recovery_hysteresis_sec`**: Increase this if the scheduler is oscillating between
THROTTLED and RECOVERY states. 30–60 seconds is often more stable than the default 10.

---

## Action Enablement

```yaml
actions:
  enable_level_1: true    # setpriority (nice) rebalancing
  enable_level_2: false   # SIGSTOP/SIGCONT throttle
  enable_level_3: false   # SIGTERM/SIGKILL
  observe_only_mode: true # Overrides all above; forces dry-run regardless of runtime mode
```

Enable levels incrementally:
1. Leave `observe_only_mode: true` until you have reviewed several runs of decisions and
   confirmed the candidate selection logic is targeting the right background processes.
2. Enable `level_1` first. It is reversible (nice values are restored on recovery).
3. Enable `level_2` only after Level 1 alone is insufficient.
4. Enable `level_3` only for headless batch environments where you control the job
   scheduler and can tolerate process terminations.

---

## Protected PIDs and Names

Add any PID or process name that must never be touched:

```yaml
protected_pids:
  - 1      # init/systemd (always protected; also hard-coded in the daemon)
  - 1234   # example: a monitoring sidecar

protected_names:
  - sshd
  - systemd
  - journald
  - nvidia-smi
```

After modifying either list, restart the daemon — the config is loaded at startup.

---

## Multi-GPU Placement Policy

```yaml
multi_gpu:
  vram_aggregation: sum        # sum | max | mean
  per_pid_vram_merge: true
  device_allowlist: []         # empty = all devices
  placement_aware_kills: false
```

These fields only affect Tier C (multi-GPU) hosts. On single-GPU or CPU-only hosts they are
silently ignored.

### `vram_aggregation`

Controls how total VRAM pressure is combined across devices when computing the UPS score:

| Value | Meaning | When to use |
| --- | --- | --- |
| `sum` | Total bytes used across all GPUs divided by total capacity (default) | Conservative; reacts to total VRAM headroom |
| `max` | Worst single-device fill percentage | Useful when one GPU is a bottleneck |
| `mean` | Average fill across all GPUs | Lower sensitivity; suitable for balanced multi-GPU workloads |

### `per_pid_vram_merge`

When `true` (default), `process_mapper` sums VRAM from all devices for each PID. A process
using 2 GB on each of 2 GPUs counts as 4 GB total. Set to `false` if you want to attribute
each process only to its dominant GPU.

### `device_allowlist`

List of device indices to monitor. Empty list (default) monitors all devices. Useful for
excluding a GPU reserved for display or inference-only work:

```yaml
multi_gpu:
  device_allowlist: [0, 1]   # only monitor GPU 0 and GPU 1 on a 4-GPU host
```

### `placement_aware_kills`

**Experimental.** When `true`, Hermes prefers to throttle the process on the GPU with the
highest utilisation rather than selecting globally by VRAM. Leave `false` until T4 evidence
exists showing this improves outcomes on your specific topology.

---

## Verifying Changes

After any config change:

1. Run one daemon cycle in observe-only mode with `HERMES_MAX_LOOPS=10`.
2. Run `hermes_reeval` on a recent saved run directory to compare match rates.
3. Check `eval_summary.json` from `hermes_eval` for precision/recall impact.
4. Check `latency_summary.json` to verify policy loop p95 latency has not increased.
5. Run `bash scripts/smoke_schema.sh` to validate the edited schema before restarting.

Only promote the change to a live active-control run once all five checks pass.
