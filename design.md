# Hermes: Unified CPU-GPU Resource Orchestrator for Local ML Workloads

## Title / Abstract

Hermes is a single-host control-plane for machine learning workloads that treats CPU pressure, system memory pressure, GPU utilization, and GPU memory pressure as one coordinated scheduling problem instead of four unrelated signals. The system is designed to sample Linux PSI and NVIDIA telemetry at low latency, attribute GPU memory to processes, predict imminent failure states such as GPU out-of-memory (OOM) and latency spikes, and intervene with progressively stronger controls before the host becomes unstable.

This design targets a hybrid operating model:

- Development can happen from Windows or WSL2.
- The runtime and benchmark target is a native Linux host with `/proc`, PSI support, and NVIDIA NVML available.
- Hermes runs in observe-only mode by default and can enable privileged controls when the operator allows them.

The v1 success condition is not "make the GPU faster." The success condition is to reduce avoidable failures and severe pressure dwell time while protecting a foreground ML workload under contention.

## Problem Statement

Modern local ML systems routinely fail for reasons that are obvious in hindsight but invisible to standard schedulers:

- CPU contention causes runnable tasks to stall even when the GPU is nominally busy.
- Memory pressure builds gradually until reclaim storms or allocator stalls destabilize the host.
- GPU VRAM is consumed by multiple processes that are not coordinated with each other.
- Existing tools expose CPU, RAM, and GPU state separately, so operators react after OOM or tail-latency collapse has already occurred.

Hermes addresses this by acting as a unified resource orchestrator for local ML workloads. It combines cross-device pressure into one decision system, attributes GPU usage to real processes, predicts instability before it becomes fatal, and applies layered interventions that preserve useful work whenever possible.

## Goals

- Build a low-latency host-level control loop for concurrent ML workloads on a single Linux machine.
- Combine CPU PSI, memory PSI, GPU utilization, and VRAM pressure into a Unified Pressure Score (UPS).
- Correlate GPU memory usage with Linux PIDs and augment each process with CPU, memory, and scheduling context.
- Predict imminent GPU OOM or latency spikes early enough for intervention to matter.
- Apply multi-level interventions ranging from reprioritization to hard-stop actions, with explicit safety guardrails.
- Produce benchmark artifacts that show before/after behavior under controlled contention scenarios.
- Log enough reasoning and state transitions to replay decisions and explain why Hermes acted.

## Non-Goals

- Cluster orchestration, container scheduling, or Kubernetes replacement behavior.
- Full distributed scheduling across multiple hosts.
- Perfect per-process GPU utilization attribution under every NVIDIA runtime mode.
- Deep model-level optimization, kernel tuning, or custom CUDA allocator modifications in v1.
- Automatic batch-size rewriting inside arbitrary ML frameworks.
- Production-hardening claims before benchmark artifacts exist.

## Deployment Assumptions

- Runtime target: Linux host with `/proc/pressure/cpu`, `/proc/pressure/memory`, `/proc`, signal support, and NVIDIA NVML available.
- Development model: Windows or WSL2 is acceptable for code authoring and light validation, but PSI-backed measurements and GPU contention benchmarks must run on a native Linux GPU machine.
- Hermes v1 scope: single-host local ML workloads only; no distributed or cluster-aware scheduling.
- Default privileges: Hermes should run safely in observe-only mode without elevated permissions.
- Optional privileged controls: cgroup v2 memory and CPU controls, advanced scheduler actions, or aggressive throttling may require root or delegated capabilities and must remain optional.
- Timekeeping: all benchmark artifacts should record wall-clock timestamps with timezone and monotonic timestamps for local ordering.

### Instrumentation Tiers and Metric Availability

Hermes should treat metric support as environment-tiered rather than universal. The design must detect unavailable collectors explicitly and log fallback behavior instead of silently assuming that PSI, full NVML, eBPF, or `perf` are always present.

| Tier | Environment | Intended Signals |
| --- | --- | --- |
| `Tier A` | WSL2 or Linux VM with partial kernel/GPU support | `/proc` basics, `/proc/loadavg`, per-process stats, application latency/throughput, replay/simulation, device-level VRAM totals when available, optional `perf` |
| `Tier B` | Native Linux without NVIDIA GPU | full `/proc`, cgroup v2, `perf`, eBPF scheduler tools, stronger CPU and memory contention signals |
| `Tier C` | Native Linux with NVIDIA GPU | PSI, full NVML per-process memory attribution, optional DCGM-style telemetry, richer GPU-aware benchmark artifacts |

Feature-detection requirements:

- Detect and record whether PSI files are present before enabling PSI-driven logic.
- Detect whether NVML supports device-level totals, utilization, and active-process queries; degrade gracefully when WSL limitations apply.
- Detect whether eBPF tracing and `runqlat` are available before enabling kernel-latency metrics.
- Detect whether `perf` access is blocked by host policy such as `perf_event_paranoid` and record that restriction in the run metadata.
- Treat substitute metrics such as `/proc/loadavg`, major faults, application-side VRAM logs, and replay traces as first-class fallbacks in lower tiers.

## System Architecture

Hermes follows a six-surface C++ architecture so the control loop can evolve independently from the sensors and action layer while keeping the backend fully native on Linux benchmark hosts:

```text
include/hermes/
  monitor/
  profiler/
  engine/
  actions/
  runtime/
  cli/

src/
  monitor/
    cpu_psi.cpp
    mem_psi.cpp
    gpu_stats.cpp
  profiler/
    process_mapper.cpp
    workload_classifier.cpp
  engine/
    pressure_score.cpp
    predictor.cpp
    scheduler.cpp
  actions/
    reprioritize.cpp
    throttle.cpp
    kill.cpp
  runtime/
    hermesd.cpp
    event_bus.cpp
    control_socket.cpp
  cli/
    hermesctl.cpp
    hermes_replay.cpp
```

### Top-Level Responsibilities

| Module | Responsibility |
| --- | --- |
| `monitor/` | Poll low-level signals from PSI and NVML and convert them into timestamped samples. |
| `profiler/` | Attribute host and GPU activity to processes and classify workload intent. |
| `engine/` | Convert raw samples into UPS, risk predictions, and intervention decisions. |
| `actions/` | Apply safe system actions and return structured action results. |
| `runtime/` | Own daemon lifecycle, bounded queues, local control sockets, configuration, and integration of sensors, policy, and actions. |
| `cli/` | Provide operator commands, observe-only views, replay entrypoints, and report tooling. |

### Canonical Record Types

These records are the primary interfaces between subsystems. The exact implementation can use C++ structs/classes plus serialized NDJSON or binary IPC messages, but the field intent should remain stable.

| Record | Core Fields | Purpose |
| --- | --- | --- |
| `PressureSample` | `ts_wall`, `ts_mono`, `cpu_some_avg10`, `cpu_full_avg10`, `mem_some_avg10`, `mem_full_avg10`, `gpu_util_pct`, `vram_used_mb`, `vram_total_mb`, `vram_free_mb` | Host-level time-series sample used by scoring and prediction. |
| `ProcessSnapshot` | `pid`, `ppid`, `cmd`, `state`, `nice`, `cpu_pct`, `rss_mb`, `gpu_mb`, `workload_class`, `foreground`, `protected` | Unified per-process view built from `/proc` and NVML. |
| `PressureScore` | `ts_mono`, `ups`, `band`, `components`, `dominant_signals` | Explainable score emitted by the UPS calculator. |
| `RiskPrediction` | `ts_mono`, `risk_score`, `risk_band`, `predicted_event`, `lead_time_s`, `reason_codes`, `target_pids`, `recommended_action` | Forecast of likely instability and the action class that would mitigate it. |
| `InterventionDecision` | `ts_mono`, `level`, `action`, `target_pids`, `cooldown_state`, `why`, `mode` | Output of the policy engine before the action layer runs. |
| `InterventionResult` | `ts_mono`, `decision_id`, `success`, `error`, `system_effect`, `reverted` | Outcome of executing or simulating a control action. |
| `EventRecord` | `ts_wall`, `kind`, `payload`, `run_id`, `scenario`, `config_hash` | Append-only replay and benchmark log format. |
| `KernelTraceSample` | `ts_mono`, `runqlat_us`, `minor_faults`, `major_faults`, `ctx_switches`, `futex_wait_us`, `source` | Optional kernel-observability record emitted by eBPF or equivalent tracing. |
| `ReplayFrame` | `frame_id`, `scenario`, `sample_ref`, `process_ref`, `decision_ref`, `expected_outcome` | Offline simulation and replay input unit for policy evaluation. |

## Control Loop and Timing Model

Hermes should use short cadences and explicit hysteresis so it reacts early without oscillating.

- Sampling cadence: collect PSI and GPU samples every `500 ms`.
- Process attribution cadence: refresh per-PID mapping every `1 s`; allow an immediate refresh when a GPU pressure threshold is crossed.
- Rolling windows:
  - short window: `5 s` for fast slope estimates
  - medium window: `30 s` for smoothing and band transitions
  - baseline window: `120 s` for host-local baseline comparison
- Predictor update cadence: recompute `RiskPrediction` every `1 s`.
- Scheduler cadence: evaluate actions every `1 s`, but permit immediate escalation if the predictor enters critical risk and cooldown rules allow it.
- Cooldowns:
  - per-PID Level 1 cooldown: `15 s`
  - per-PID Level 2 cooldown: `20 s`
  - global Level 3 cooldown: `300 s`
- Recovery hysteresis:
  - do not resume paused work until UPS is below the elevated band and risk is below high for at least `10 s`
  - require `3` consecutive low-risk evaluations before de-escalating
- Operating modes:
  - `observe-only`: collect metrics and emit proposed decisions, never mutate the host
  - `advisory`: emit shell/operator recommendations and optional warnings
  - `active-control`: apply actions allowed by configured privileges and guardrails

## Scheduler State Machine

Hermes should model scheduler behavior as an explicit state machine rather than as a loose collection of threshold-triggered actions. This keeps escalation, recovery, and cooldown logic understandable under stress.

| State | Meaning | Typical Entry Condition | Typical Exit Condition |
| --- | --- | --- | --- |
| `NORMAL` | Healthy operation with no pending intervention. | UPS below elevated band and predictor in low risk. | Enter `ELEVATED` if pressure or risk rises. |
| `ELEVATED` | Contention is building and Level 1 actions may be considered. | UPS enters elevated band, predictor enters medium/high risk, or kernel traces show rising scheduler pain. | Return to `NORMAL` after sustained stability, or escalate to `THROTTLED`. |
| `THROTTLED` | One or more Level 2 controls are actively constraining background work. | Level 2 intervention activates or privileged backend applies quota/placement control. | Move to `RECOVERY` when pressure subsides, or `COOLDOWN` after a hard action. |
| `RECOVERY` | Hermes is restoring work gradually while watching for relapse. | UPS and risk stay below elevated thresholds for the hysteresis window. | Return to `NORMAL` after sustained stability or back to `ELEVATED` if pressure rebounds. |
| `COOLDOWN` | Hermes temporarily suppresses repeated aggressive actions to avoid oscillation. | Level 2 or Level 3 action was recently applied. | Return to `RECOVERY` or `ELEVATED` after cooldown expiry and re-evaluation. |

State transitions should be logged explicitly in the event stream, including the trigger condition, the prior state, the next state, and the evidence that justified the transition.

## Subsystem Design

### Component Responsibilities and I/O Contracts

| Component | Responsibilities | Inputs | Outputs |
| --- | --- | --- | --- |
| CPU PSI monitor | Read `/proc/pressure/cpu`, parse `some/full`, normalize raw text into numeric fields, surface read failures. | Linux PSI files | Partial `PressureSample` fields for CPU pressure |
| Memory PSI monitor | Read `/proc/pressure/memory`, parse `some/full`, expose reclaim-driven stall intensity. | Linux PSI files | Partial `PressureSample` fields for memory pressure |
| GPU stats collector | Query NVML for device utilization, VRAM used/free, and per-process memory usage when available. | NVML device handles | Partial `PressureSample` plus per-device and per-process GPU memory tables |
| Process mapper | Join NVML PID lists with `/proc/<pid>` state, CPU%, RSS, command line, and scheduler metadata. Per-PID state may be sourced from the fast `/proc` parser or richer proc readers, with identical downstream semantics. | NVML process list, `/proc`, fast stat parser output | `ProcessSnapshot[]` |
| Workload classifier | Label processes as `training`, `inference`, `background`, or `idle` using name and utilization heuristics. | `ProcessSnapshot[]`, recent history | Enriched `ProcessSnapshot[]` with `workload_class`, `foreground`, and priority hints |
| UPS calculator | Normalize current host signals, apply weights, determine score band, and log contributions. | `PressureSample` | `PressureScore` |
| OOM predictor | Estimate near-term failure risk using slopes, headroom, and sustained pressure. | Recent `PressureSample`, `ProcessSnapshot[]`, `PressureScore` history | `RiskPrediction` |
| Scheduler / policy engine | Choose the lowest-risk effective action while respecting cooldowns and protection rules. | `PressureScore`, `RiskPrediction`, `ProcessSnapshot[]`, config | `InterventionDecision` |
| Action executors | Apply or simulate `nice`, pause/resume, cgroup adjustments, or process termination. | `InterventionDecision` | `InterventionResult` |
| Event logger / replay writer | Persist samples, predictions, actions, and benchmark metadata in append-only form. | All structured records | NDJSON/CSV artifacts and replayable event streams |

### Classification Heuristics

The first version should use transparent heuristics rather than opaque models:

- `training`: sustained GPU memory residency, repetitive GPU utilization bursts, Python or training-framework command names, longer wall-clock duration
- `inference`: request-shaped bursts, lower steady-state VRAM than training, optional foreground tag from CLI/config
- `background`: low priority or explicitly marked non-latency-sensitive jobs
- `idle`: negligible CPU and GPU activity for a sustained interval

Classification output is advisory. It influences policy weighting, state-machine transitions, and kill protection, but should not be treated as ground truth.

## Hardware-Aware Scheduling Assumptions

Hermes treats system performance as a function of both software scheduling and underlying hardware behavior.

### Key Assumptions

- CPU progress depends not only on utilization but also on cache locality and memory-access latency.
- Sustained high CPU activity with weak forward progress may indicate memory stalls, cache misses, or scheduler inefficiency.
- GPU VRAM is a hard-capacity resource, and burst allocations can trigger rapid OOM conditions before coarse monitoring tools react.

### Policy Implications

- Latency-sensitive foreground workloads should be protected from cache-inefficient background work.
- Sustained memory-pressure signals should be weighted more heavily than raw utilization when Hermes chooses between slowdown and intervention.
- GPU memory headroom collapse should be treated as a first-class failure precursor, not as a late warning signal.

Hermes does not model hardware at the microarchitectural level in v1, but it explicitly incorporates memory-hierarchy effects and GPU memory constraints into host-level scheduling decisions.

## Low-Overhead Sampling Path

Hermes keeps the backend hot path in C++ so cadence stability, parsing cost, and scheduling decisions do not depend on a scripting runtime.

- C++ is the backend language for orchestration, policy, logging, replay ingestion, and action dispatch.
- The low-overhead path begins with hot-path `/proc` parsing and expands into the native collector/runtime defined below.
- Native parsing exists to improve cadence stability, lower parser overhead, and keep process-state collection closer to system-level performance expectations.

### Initial Helper Contract

The first C++ helper should support:

- reading `/proc/<pid>/stat`
- extracting process state, RSS, and CPU time
- returning compact structured output for ingestion by the scheduler loop

Minimum output fields:

- `pid`
- `state`
- `rss`
- `utime`
- `stime`
- `total_cpu_time`

This helper is an internal interface only. It should feed the same `ProcessSnapshot` shape used by the rest of the C++ pipeline so policy, logging, and replay logic do not branch on parser implementation.

## Native C++ Runtime Path

Hermes should treat the native runtime as the backend itself on benchmark and high-pressure hosts, not as a convenience wrapper around another language.

### Runtime Model

- A C++ daemon such as `hermesd` owns low-latency sampling, pressure scoring, prediction, action orchestration, and event logging on the Linux host.
- The daemon uses a multi-threaded sampling engine so process-state collection, queue management, policy evaluation, and action dispatch do not contend on one loop.
- The initial native milestone may begin with `/proc/<pid>/stat` parsing, but the design should assume the C++ runtime remains the primary backend for process and kernel-adjacent telemetry.

### Internal Structure

- sampler thread: reads `/proc`, PSI-adjacent host state, and native process metadata on the fast cadence
- aggregator thread: normalizes raw samples into compact snapshot batches
- policy thread: computes UPS, risk, scheduler state, and candidate interventions
- transport thread: publishes snapshots and decisions to local operator or replay clients
- bounded ring buffer or queue: absorbs short bursts without stalling the sampler

### IPC Contract

- default IPC: Unix domain socket for structured message exchange between `hermesd`, local CLI tools, and replay consumers
- higher-throughput option: shared memory transport for replay or high-rate debug modes
- failure behavior: if a collector is unavailable, Hermes should degrade to a lower instrumentation tier inside the same native daemon rather than switching backend languages

The C++ runtime remains responsible for policy, explainability, replay, and operator UX integration. External workloads may still be Python-based ML jobs, but Hermes itself should not depend on Python for backend behavior.

## Unified Pressure Score (UPS)

UPS is Hermes' primary host-level control signal. It compresses heterogeneous pressure sources into one score while preserving the component breakdown that produced it.

### Default Normalization

```text
n_cpu      = clamp(cpu_some_avg10 / 25.0, 0.0, 1.0)
n_mem      = max(
               clamp(mem_full_avg10 / 5.0, 0.0, 1.0),
               0.5 * clamp(mem_some_avg10 / 20.0, 0.0, 1.0)
             )
n_gpu_util = clamp(gpu_util_pct / 100.0, 0.0, 1.0)
n_vram     = clamp(vram_used_mb / vram_total_mb, 0.0, 1.0)
```

### Initial Weighted Formula

```text
UPS = 100 * (
  0.20 * n_cpu +
  0.35 * n_mem +
  0.15 * n_gpu_util +
  0.30 * n_vram
)
```

These defaults intentionally overweight memory and VRAM pressure because reclaim storms and VRAM exhaustion are the fastest paths to catastrophic instability in the target workload class. Weights must remain configurable so benchmark tuning can be explicit and versioned.

### Bands

| Band | Range | Meaning |
| --- | --- | --- |
| Normal | `0-39` | Routine pressure; do not intervene. |
| Elevated | `40-69` | Contention is developing; advisory or Level 1 actions may be appropriate. |
| Critical | `70-100` | Failure risk is material; Level 2 or Level 3 actions may be required if the predictor agrees. |

### Explainability Requirement

Every `PressureScore` record must log:

- raw signal values
- normalized components
- final weights in effect
- dominant signal contributors
- band transition reason if the band changed

No scheduler decision should be emitted without a recoverable chain from raw sample to UPS output.

## Predictive OOM Detection

Predictive detection is the feature that makes Hermes more than a threshold-triggered monitor.

### Objective

- Primary objective: provide `3-10 s` of actionable lead time before a likely GPU OOM or severe latency spike.
- Secondary objective: minimize false positives that would reduce throughput without preventing failures.

### Minimum Feature Set

- `d(VRAM)/dt` over `3 s` and `10 s` windows
- free VRAM headroom trend and headroom collapse rate
- sustained UPS critical-band residency
- memory PSI trend, especially `mem_full_avg10`
- GPU process concurrency count
- per-PID GPU memory expansion trend
- recent intervention history and cooldown saturation

### Output Contract

`RiskPrediction` should expose at least:

- `risk_score`: normalized `0.0-1.0`
- `risk_band`: `low`, `medium`, `high`, `critical`
- `predicted_event`: `gpu_oom`, `latency_spike`, or `mixed_pressure_collapse`
- `lead_time_s`: estimated seconds before failure if no action is taken
- `reason_codes`: stable machine-readable reasons such as `VRAM_HEADROOM_COLLAPSE`, `MEM_FULL_PSI_RISING`, `MULTI_PROC_GPU_CONTENTION`
- `recommended_action`: `observe`, `reprioritize`, `throttle`, or `terminate_candidate`

### Evaluation Requirements

Predictor quality must be measured explicitly:

- precision
- recall
- F1
- mean lead time before actual OOM or latency breach
- false positive rate per hour
- false negative count for severe events

The predictor should never be described as accurate or production-ready without these measurements.

## Intervention Ladder and Safety Policy

Hermes must prefer the least destructive action that materially lowers risk.

### Level 1: Soft Control

Typical actions:

- increase `nice` value for non-foreground jobs
- reduce scheduler preference for background work
- emit batch-size or concurrency reduction hints
- warn the operator when observe-only mode predicts intervention need

Entry guidance:

- UPS in elevated band with rising trend
- predictor in medium or high risk
- enough headroom remains that graceful slowdown is plausible

### Level 2: Throttling

Typical actions:

- `SIGSTOP` / `SIGCONT` pause-resume of background jobs
- optional cgroup v2 controls when privileged, such as `memory.high` or CPU quota tightening
- short-lived throttling windows for GPU-heavy background work

Entry guidance:

- UPS critical or near-critical
- predictor recommends `throttle`
- Level 1 action was insufficient or cooldown-blocked

### Level 3: Hard Control

Typical actions:

- terminate the lowest-priority reclaimable process after explicit policy checks

Entry guidance:

- predictor marks imminent OOM or collapse
- lower intervention levels failed, are disallowed, or would not act quickly enough
- target process is not protected and is the lowest-cost kill candidate

### Kill Candidate Policy

The kill candidate ranking should favor processes that are:

- non-foreground
- non-protected
- high VRAM consumers
- low recent-progress contributors
- recent repeat offenders for pressure spikes

### Guardrails

- Dry-run mode must be available for every action path.
- System-critical PIDs, Hermes itself, the user shell, and explicitly protected workloads must never be killed.
- No repeated action on the same PID before its cooldown expires.
- No more than one Level 3 action every `300 s` unless the operator explicitly overrides it.
- Every action must emit a structured rationale and a reversal condition when applicable.
- Operator override must support:
  - protected PID list
  - protected process-name patterns
  - globally disabled action levels
  - foreground workload declaration

## cgroup v2 Control Backend

Hermes should treat cgroup v2 as the primary privileged control backend on Linux hosts rather than as an unnamed optional feature.

### Backend Objectives

- apply bounded CPU throttling without immediately killing useful work
- trigger reclaim pressure before host-wide OOM conditions spread
- isolate foreground and background workloads onto different CPU sets when the host topology allows it
- make privileged intervention behavior explicit, reversible, and auditable

### Target Controls

| Control | Hermes Use |
| --- | --- |
| `cpu.max` | Limit background CPU entitlement during elevated or throttled states. |
| `memory.high` | Apply reclaim pressure and throttling before host-wide memory collapse. |
| `memory.max` | Reserve for hard ceilings in controlled tests or strongly sandboxed workloads; do not use as a default soft-control tool. |
| `cpuset.cpus` | Isolate foreground work from disruptive background tasks where CPU topology makes that beneficial. |

### Policy Integration

- Level 1 may prepare cgroup placement or emit advisory actions without hard limits.
- Level 2 should preferentially use `cpu.max`, `memory.high`, or `cpuset.cpus` before jumping to hard termination.
- Level 3 remains process termination by policy, with `memory.max` reserved for controlled experiments or tightly scoped sandboxing.
- Every cgroup mutation must emit the previous value, the applied value, the target cgroup, and the reversal condition.

## Telemetry, Metrics, and Artifacts

Hermes needs three metric layers so the control loop, the benchmark suite, and the operator experience can all be evaluated independently.

Every metric in Hermes should fit one of three roles:

- decision input: used directly by scoring, prediction, classification, or policy
- safety guardrail: constrains risky behavior, validates assumptions, or limits controller damage
- headline outcome: the before/after number used to judge whether Hermes actually helped

Anything else is optional diagnostic context. This keeps the metric set industry-style instead of dashboard-heavy and decision-light.

### Control Metrics

These metrics drive runtime decisions:

- CPU PSI `some/full` (`avg10`, with optional `avg60` and `total`)
- memory PSI `some/full` (`avg10`, with optional `avg60` and `total`)
- GPU utilization percent
- VRAM used, free, total, and headroom percent
- VRAM growth rate
- process count using the GPU
- per-PID GPU MB, CPU%, RSS MB, state, class, and priority
- current UPS, band, and dominant contributors
- current risk score, risk band, and lead time
- cooldown state per action and per PID

### Evaluation Metrics

These metrics support claims in README, reports, and presentations.

| Metric Group | Required Metrics |
| --- | --- |
| Stability | GPU OOM count, process crash count, run completion rate, time-to-recovery after pressure spike |
| Latency / QoS | p50, p95, p99 foreground inference latency, time above bad-latency threshold |
| Pressure | CPU PSI `some/full`, memory PSI `some/full`, seconds above danger thresholds |
| GPU | total VRAM used, peak VRAM, VRAM headroom, GPU utilization, VRAM growth rate |
| Attribution | per-PID GPU MB, CPU%, RSS, process state, workload class, scheduling priority |
| Predictor Quality | precision, recall, F1, mean lead time, false positive rate per hour, false negatives |
| Intervention Quality | intervention count by level, success rate, avoided-OOM rate, rollback count, cooldown hits |
| Throughput | jobs/hour, tokens/sec or images/sec, batch completion time |
| Fairness / Impact | foreground QoS improvement versus background slowdown |
| Kernel / Profiling | run queue latency, page faults, context switches, futex wait duration, backtrace availability for controlled failures |
| Overhead / Telemetry Quality | controller CPU overhead, loop jitter, telemetry drop rate, replay fidelity error |

### Metric Catalogue

The table below gives Hermes a precise metrics vocabulary with collection method, cadence, and realistic environment expectations.

#### GPU, VRAM, and Attribution

| Metric ID | Role | Units | Collection Method | Suggested Sampling | Availability |
| --- | --- | --- | --- | --- | --- |
| `gpu.vram.used_bytes` | decision input, guardrail | bytes | NVML device memory query or `nvidia-smi --query-gpu=memory.used --format=csv` | `0.5-1 s` | Tier A partial, Tier C preferred |
| `gpu.vram.free_bytes` | decision input, guardrail | bytes | NVML device memory query or `nvidia-smi --query-gpu=memory.free --format=csv` | `0.5-1 s` | Tier A partial, Tier C preferred |
| `gpu.vram.headroom_pct` | guardrail | percent | derived from used/free/total VRAM | `0.5-1 s` | same as above |
| `gpu.vram.slope_mb_s` | decision input | MB/s | derived from VRAM samples over `3 s` and `10 s` windows | `1 s` | Tier A partial, Tier C preferred |
| `gpu.util.gpu_pct` | decision input, diagnostic | percent | NVML utilization rates or DCGM fields | `0.5-1 s` | often limited in WSL, Tier C preferred |
| `gpu.proc.used_bytes{pid}` | decision input | bytes | NVML compute-running-process APIs with per-process memory attribution | `1 s` | native Linux GPU only in practice |

#### CPU, Memory, and Scheduler Pressure

| Metric ID | Role | Units | Collection Method | Suggested Sampling | Availability |
| --- | --- | --- | --- | --- | --- |
| `psi.cpu.some.avg10` | decision input | percent stalled wall time | `/proc/pressure/cpu` | `0.5-1 s` | Tier C and native PSI hosts |
| `psi.cpu.full.avg10` | guardrail | percent stalled wall time | `/proc/pressure/cpu` | `0.5-1 s` | Tier C and native PSI hosts |
| `psi.mem.some.avg10` | decision input | percent stalled wall time | `/proc/pressure/memory` | `0.5-1 s` | Tier C and native PSI hosts |
| `psi.mem.full.avg10` | guardrail | percent stalled wall time | `/proc/pressure/memory` | `0.5-1 s` | Tier C and native PSI hosts |
| `psi.io.some.avg10` | diagnostic, future decision input | percent stalled wall time | `/proc/pressure/io` | `0.5-1 s` | native Linux with PSI |
| `sched.runqlat.p50_ms/p99_ms` | decision input, diagnostic | ms | eBPF `runqlat`/scheduler tracepoints | `5 s` histogram intervals | Tier B/C with root and BPF support |
| `proc.loadavg.runnable` | fallback decision input | count | `/proc/loadavg` runnable field | `1 s` | Tier A/B/C |
| `cpu.util_pct` | diagnostic context | percent | derived from `/proc/stat` deltas | `0.5-1 s` | Tier A/B/C |
| `mem.rss_bytes{pid}` | decision input | bytes | `/proc/<pid>/status` or `/proc/<pid>/stat` | `1 s` | Tier A/B/C |
| `vm.major_faults_s` | fallback decision input | faults/s | `/proc/vmstat` or per-process stats | `1-5 s` | Tier A/B/C |
| `perf.cache_misses` | diagnostic validation | count | `perf stat` hardware counters | per-run or `1-5 s` windows | Tier A optional, Tier B/C preferred |
| `perf.ipc` | diagnostic validation | ratio | `perf stat -e cycles,instructions` | per-run or `1-5 s` windows | Tier A optional, Tier B/C preferred |

#### Service Outcomes, Reliability, and Controller Quality

| Metric ID | Role | Units | Collection Method | Suggested Sampling | Availability |
| --- | --- | --- | --- | --- | --- |
| `svc.latency_ms.p50/p95/p99` | headline outcome | ms | application instrumentation and probe logs | `1-5 s` rollups | Tier A/B/C |
| `svc.rps` | headline outcome | requests/s | client probe or service counters | `1-5 s` rollups | Tier A/B/C |
| `gpu.oom.count` | headline outcome, reliability guardrail | count | app logs, exceptions, exit codes, optional `dmesg` on Linux | per-run | Tier A/B/C |
| `action.count{level}` | evaluation metric | count | Hermes decision/action logs | event-driven | Tier A/B/C |
| `action.success_rate` | evaluation metric | ratio | Hermes action result logs | per-run | Tier A/B/C |
| `hermes.loop_jitter_ms` | safety guardrail | ms | internal timing deltas vs target cadence | per-run plus histogram | Tier A/B/C |
| `telemetry.drop_rate` | safety guardrail | ratio | expected vs recorded sample accounting | per-run | Tier A/B/C |
| `replay.vram_mape_pct` | replay fidelity metric | percent | compare replayed vs measured VRAM trajectories | per replay set | Tier A/B/C |
| `replay.event_time_error_ms` | replay fidelity metric | ms | compare predicted vs actual event timing in replay | per replay set | Tier A/B/C |

### Operator Metrics and Required Artifacts

The operator-facing dataset should support both live debugging and later replay.

Additional debugging and profiling evidence should be collected when benchmarking pressure-heavy scenarios:

- syscall blocking evidence from `strace`, including futex waits, I/O stalls, or repeated memory-related syscalls
- CPU scheduling and microperformance evidence from `perf stat` or `perf top`, including cycles, context switching, or cache-related inefficiencies
- timestamp alignment metadata that ties those debug captures back to PSI spikes, VRAM pressure, UPS changes, and intervention events

Required artifacts per benchmark run:

- scenario config in `YAML` or `JSON` defining workload commands, durations, thresholds, and enabled control levels
- `samples.csv` or `samples.ndjson` with periodic `PressureSample` records
- `latency.ndjson` with request or probe latency records
- `processes.csv` or `processes.ndjson` with `ProcessSnapshot` records
- `decisions.ndjson` with proposed and executed `InterventionDecision` records
- `actions.ndjson` with intervention results and errors
- `events.ndjson` with action results, warnings, benchmark annotations, and state transitions
- `summary.csv` with one row per run
- replay fidelity output such as `replay_summary.json` or `replay_eval.ndjson`
- telemetry-quality accounting such as `telemetry_quality.json`
- configuration snapshot including thresholds, weights, and enabled action levels
- host metadata snapshot including GPU model, driver version, kernel version, and runtime mode
- at least one `strace` capture for a stressed workload in the benchmark suite
- at least one `perf stat` or `perf top` capture for a benchmark run
- optional eBPF trace capture showing run queue latency, page faults, context switches, or futex wait behavior when kernel tracing is enabled
- replay bundle with aligned samples, process snapshots, decisions, and expected outcomes for offline policy evaluation
- backtrace or core-dump notes for any controlled failure analyzed with `gdb`
- trace-alignment metadata or event references that correlate debug captures with Hermes telemetry

All structured artifacts should carry `run_id` and `config_hash` so replay, reports, and dashboards can join records without ambiguity.

Operator and debug summaries should also call out:

- evidence of syscall-level blocking during pressure windows
- context-switch pressure or scheduler churn when the host is overloaded
- cache- or memory-related slowdown observations that help explain why Hermes intervened
- run queue latency, page-fault pressure, or futex wait evidence when kernel tracing is enabled
- thread-level or core-dump findings when `gdb` is used for controlled failure analysis

## Industry-Grade Metrics and Targets

The following targets are environment-tier and scenario dependent. They are not universal promises; they are measurable goals that become defensible only when backed by saved artifacts and stable benchmark scenarios.

| Target Area | Minimum Pass Target | Strong Target | How to Measure |
| --- | --- | --- | --- |
| Foreground p95 latency under contention | `10-15%` reduction vs baseline | `20-30%` reduction vs baseline | compare probe-derived p95/p99 across baseline vs active-control runs |
| Severe-pressure dwell time | `15%` reduction in time above danger threshold | `30%` reduction | use PSI dwell time on native Linux or explicit surrogate thresholds in Tier A |
| GPU OOM avoidance | reduce OOMs in a controlled VRAM ramp scenario | eliminate OOMs in at least `70%` of controlled ramp runs without foreground kills | count CUDA OOMs and avoided-OOM events from risk+action traces |
| Predictor median lead time | `>= 3 s` in replay | `5-10 s` in replay | compare predicted event time vs actual event time |
| Predictor quality | precision and recall `>= 0.6` for critical events | precision and recall `>= 0.75` on replay set | labeled trace confusion matrix |
| False positives | `<= 1 per 10 min` on replay | `<= 1 per 30 min` on replay | count predicted critical events with no matching actual event |
| Controller overhead | `<= 1%` CPU average | `<= 0.5%` CPU average with low jitter | measure Hermes CPU usage plus loop jitter distribution |
| Telemetry quality | `>= 99%` expected samples recorded | `>= 99.9%` expected samples recorded | expected-vs-actual sample accounting and drop-rate logs |

Target interpretation by tier:

- Tier A should emphasize latency, device-level VRAM headroom, replay fidelity, controller overhead, and clean artifact generation.
- Tier B should add stronger CPU and memory contention evidence via `perf`, eBPF, and cgroup-aware experiments.
- Tier C should add PSI, full NVML per-process attribution, and the strongest GPU-aware scheduler claims.

## Debugging and Profiling Strategy

Hermes includes an explicit debugging and profiling layer to validate scheduling decisions against observed system behavior rather than relying only on heuristic thresholds.

- `strace` is used to inspect syscall-level blocking behavior for selected workloads, including futex waits, I/O stalls, and repeated memory-related syscalls.
- `perf` is used to measure CPU cycles, task scheduling behavior, and cache-related inefficiencies during high-pressure windows.
- eBPF is used when available to capture kernel-level evidence such as run queue latency, page faults, context switches, and futex wait behavior without relying exclusively on ptrace-style tooling.
- `gdb` is used for thread-level debugging, crash analysis, breakpoint-driven inspection, and postmortem core-dump analysis on native components or controlled workload failures.
- Debug traces are timestamp-aligned with PSI spikes, VRAM pressure, and intervention events so each scheduling decision can be explained using observable system evidence.
- eBPF tools such as `runqlat` may require root privileges and kernel BPF support, so Hermes should record when kernel-level tracing was unavailable for a run.
- `perf` access may be restricted by host security settings such as `perf_event_paranoid`; those restrictions should be captured in run metadata rather than treated as silent omissions.

This layer exists to answer two questions:

1. Was the process actually under resource contention?
2. Did Hermes intervene for the correct reason?

## eBPF Kernel Observability Layer

Hermes should reserve an optional kernel-observability path that captures evidence below the process level when user-space metrics are not enough to explain instability.

### Intended Signals

- run queue latency
- minor and major page faults
- voluntary and involuntary context switches
- futex wait durations and wakeup behavior

### Role in the System

- explain whether a CPU-heavy process is making forward progress or is mostly stalled behind scheduling or memory effects
- distinguish reclaim-driven slowdown from plain high utilization
- provide kernel-level evidence that supports or rejects Hermes intervention decisions

This layer is optional because host permissions and kernel support vary, but the design should assume its outputs can be ingested as `KernelTraceSample` records and aligned with UPS, predictions, and actions.

## gdb-Driven Failure Analysis

Hermes should include a postmortem debugging path for failures that cannot be explained through metrics alone.

- use `gdb` to inspect deadlocks, race conditions, and crashes in the native collector or controlled benchmark workloads
- capture thread-level backtraces, core-dump summaries, and breakpoint-based observations during controlled debugging sessions
- keep `gdb` out of the real-time control loop; it is a failure-analysis tool, not an always-on sampling dependency

This path matters most when the project introduces a native multi-threaded component, because performance claims are not credible if native crashes or hangs cannot be investigated rigorously.

## Simulation and Replay Mode

Hermes should support an offline mode that replays saved traces and synthetic workloads without requiring a live GPU host for every policy iteration.

### Capabilities

- trace replay from saved `PressureSample`, `ProcessSnapshot`, `KernelTraceSample`, and `InterventionDecision` artifacts
- policy testing against historical runs without mutating a live machine
- synthetic workload generation for CPU pressure, memory pressure, GPU pressure, or mixed-pressure cases
- side-by-side comparison of multiple policy versions on the same replay trace

### Determinism and Trace Generators

- Given the same replay trace and the same `config_hash`, Hermes decisions should be deterministic.
- Synthetic trace generators should support VRAM step functions, VRAM ramps, and stochastic noise with periodic spikes so allocator variance and burst allocation can be modeled explicitly.
- Replay should accept both measured traces and purely synthetic traces so Tier A environments can validate policy behavior without needing full native GPU telemetry.

### Replay Fidelity Evaluation

- Compare replayed VRAM and latency distributions against measured artifacts when real runs exist.
- Report replay fidelity with metrics such as `replay.vram_mape_pct` and `replay.event_time_error_ms`.
- Treat replay as a first-class evaluation mode, especially when WSL or VM environments cannot expose full PSI or NVML process attribution.

Replay mode is important for safe iteration: it allows policy changes to be evaluated against known bad scenarios before they are trusted on a live benchmark host.

## Fault Injection Framework

Hermes should include a controlled fault-injection layer so the scheduler is tested against repeatable failure patterns rather than accidental lab conditions.

### Required Injection Families

- VRAM spike
- memory reclaim storm
- CPU hog
- process churn
- scheduler oscillation
- missing NVML samples
- delayed telemetry samples
- malformed `/proc` reads
- throttle/unthrottle oscillation
- cooldown bugs

### Design Intent

- support both live-host controlled injections and offline simulation inputs
- label every injected fault in the event stream so benchmark analysis can separate organic pressure from synthetic stress
- keep all injections bounded and reversible, with explicit cooldown and cleanup rules
- separate telemetry faults, policy faults, and workload faults so the operator can tell whether Hermes is resilient to bad workloads, bad signals, or bad control logic

Fault injection is how Hermes proves that the scheduler state machine and control backend behave correctly under worst-case transitions instead of only under normal contention.

## Minimum Evidence for Defensible Claims

Hermes should not be described as research-grade, production-like, or interview-defensible unless a small minimum evidence package exists in the repo or benchmark artifacts.

At minimum, that package should include:

1. An initial native C++ collector milestone that parses `/proc/<pid>/stat` and returns at least `pid`, `rss`, `state`, and CPU time fields used by Hermes process inspection.
2. One saved `strace` example from a stressed workload that shows blocking behavior or repeated waits, such as futex stalls, I/O stalls, or repeated memory-related syscalls.
3. One saved `perf stat` or `perf top` capture from a benchmark run, plus at least one concrete observation such as elevated cycles, context switching, cache-related slowdown, or scheduler churn.

This requirement exists to keep the project honest: the design may describe hardware-aware policy and low-overhead sampling, but those claims only become strong when backed by at least one helper implementation and at least one real profiling example from a Linux benchmark host.

## Benchmark Methodology

Benchmarking must show that Hermes changes the behavior of the host under controlled contention, not just that it emits logs.

### Required Scenarios

Every primary workload scenario should be executed in these three modes:

| Mode | Meaning |
| --- | --- |
| Baseline | No Hermes process running; establishes uncontrolled behavior. |
| Observe-only | Hermes runs but does not change host state; validates detection and decision quality. |
| Active interventions | Hermes applies allowed controls; measures real impact. |

### Workload Mix

Each main scenario must include:

- `2` ML jobs that contend for GPU or host memory
- `1` CPU-heavy or memory-heavy stressor
- `1` foreground inference path or latency-sensitive service

Recommended initial workload families:

- GPU memory contention: two PyTorch jobs with increasing batch size or tensor size
- mixed host pressure: one GPU job plus one CPU-heavy stressor plus one memory allocator stressor
- foreground protection: a latency-probed inference service while background training competes

### Repetition and Threshold Discipline

- Minimum repeated runs per scenario: `5`
- Preferred repeated runs for the primary headline scenario: `10`
- Thresholds must be frozen before the first benchmark run and stored with the config snapshot.

Suggested initial experiment thresholds:

- UPS elevated at `40`, critical at `70`
- VRAM pressure high at `> 90%`, critical at `> 95%`
- memory PSI elevated when `mem_full_avg10 > 2`, critical when `mem_full_avg10 > 5`
- high predictor risk at `risk_score >= 0.70`

### Scenario Packaging and Run Identity

Every benchmark scenario should be packaged as a `YAML` or `JSON` config that includes:

- workload commands
- warmup and contention durations
- thresholds and weights
- enabled control levels
- environment tier and required collectors

Each run must emit:

- a unique `run_id`
- a stable `config_hash`
- structured logs that carry both fields on every record

### Stable Artifact Schemas

Minimum NDJSON contracts:

- `samples.ndjson`: `{ts_wall, ts_mono, run_id, config_hash, vram_used_mb, vram_free_mb, gpu_util_pct, cpu_some_avg10, mem_full_avg10, loadavg_runnable, ...}`
- `latency.ndjson`: `{ts_wall, run_id, config_hash, request_id, latency_ms, status_code}`
- `decisions.ndjson`: `{ts_mono, run_id, config_hash, ups, risk_score, predicted_event, lead_time_s, action, state}`
- `actions.ndjson`: `{ts_mono, run_id, config_hash, action, target_pid, success, error}`

### Required Benchmark Table Fields

Every summary row must include:

- scenario name
- run id
- workload mix
- OOM count
- p95 latency
- peak memory PSI full
- peak VRAM usage
- intervention count by level
- jobs completed
- notes on degraded behavior

### Required Plots and Charts

- time-series plot of CPU PSI, memory PSI, VRAM usage, UPS, and interventions
- foreground latency distribution or CDF
- predictor confusion matrix and lead-time summary
- before/after summary table suitable for README reuse

### Benchmark Report Questions

Every in-repo report or run summary should answer:

- what scenario ran and for how long
- what the pre/post values were for OOM count, p95 latency, severe-pressure dwell time, and controller overhead
- what actions were taken and how often
- what the predictor precision, recall, and median lead time were on the replay set

### Minimum Dashboard Panels

- pressure timeline: VRAM used/free, headroom percent, PSI when available, runnable count, and intervention markers
- latency: p50/p95/p99 time series plus latency CDF for baseline, observe-only, and active-control modes
- predictor quality: confusion matrix for critical-event labels and lead-time histogram
- overhead and telemetry quality: Hermes CPU percent, loop jitter histogram, and telemetry drop rate
- optional benchmark timeline view: warmup, contention window, decision points, and applied actions

### Result Framing

Benchmark conclusions must be written as hypotheses confirmed or rejected by artifacts, not as guaranteed outcomes. The expected wins for v1 are:

- fewer or zero avoidable GPU OOMs in the controlled benchmark
- lower severe-pressure dwell time
- better p95 and p99 foreground latency under contention
- equal or slightly improved productivity without faking success through excessive kills

## Failure Modes and Limitations

- NVML per-process utilization is less reliable than per-process memory attribution under concurrent execution; v1 should trust GPU memory mapping more than exact process-level GPU utilization.
- Some OOM events can unfold faster than the control loop if the workload allocates in large bursts.
- WSL2 is a development convenience, not a source of benchmark truth for PSI and GPU contention claims.
- WSL kernels may lack `CONFIG_PSI`, which means `/proc/pressure/*` can be absent and Hermes must fall back to surrogate contention metrics.
- WSL NVML support may omit GPU utilization and active compute-process queries; device-level VRAM totals may be the only reliable GPU signal in that environment.
- Privileged cgroup controls may be unavailable on many developer machines; observe-only behavior must remain useful without them.
- eBPF tracing may require root privileges and kernel BPF support; kernel-latency metrics should be treated as optional rather than assumed.
- `perf` may be limited by host security/access controls such as `perf_event_paranoid`, so hierarchy-efficiency metrics can be unavailable even on Linux hosts.
- Heuristic workload classification can mislabel unknown processes and must stay explainable.
- Hard interventions can protect the foreground path while reducing aggregate throughput; that tradeoff must be visible in metrics.
- Multi-GPU, MIG, and CUDA MPS behavior are out of scope for v1 unless explicitly added later.
- I/O PSI is not part of the initial UPS formula, so disk-driven stalls may be under-modeled in v1.
- DCGM and DCGM-exporter-style telemetry are future industry-alignment paths, not required v1 dependencies for local Hermes validation.

## Metrics Provenance and Source Priorities

Hermes should prioritize official or authoritative sources for metric semantics, collector behavior, and environment limitations.

- NVML API reference: authoritative for device and process GPU telemetry semantics.
- `nvidia-smi` documentation: authoritative for supported `--query-gpu` patterns and practical low-overhead query strategies.
- CUDA on WSL documentation: authoritative for WSL-specific NVML limitations and honest scope boundaries.
- Linux PSI documentation: authoritative for `some/full`, averaging windows, cumulative totals, and trigger semantics.
- `/proc` documentation and procfs manpages: authoritative for `/proc/loadavg`, per-process stats, and general proc semantics.
- `runqlat` / bpftrace docs: authoritative for scheduler-latency tracing behavior and privilege expectations.
- `perf_event_open` and perf security docs: authoritative for hardware-counter semantics and access restrictions.
- DCGM and DCGM exporter docs: authoritative for industry-style GPU telemetry and future integration shape.

## Session Handoff Log

This section exists to keep future sessions grounded in verified state instead of memory.

### Handoff Rules

- Append only. Never rewrite older entries to make the project look more complete than it was.
- Only verified repo state, code artifacts, or benchmark artifacts may be used to claim completion.
- Conversational intent, brainstorms, and unexecuted plans do not count as implemented work.
- If a session is nearing token exhaustion, context exhaustion, or any other forced stop, the active session must append a short verified handoff entry before ending.
- Every roadmap status change should be mirrored here with evidence.
- Evidence references may include code paths, benchmark artifacts, `strace` logs, `perf` outputs, native collector traces, or C++ helper traces.
- Evidence references may also include eBPF traces, replay bundles, fault-injection logs, or `gdb` backtraces and core-dump notes.

### Entry Template

```md
#### YYYY-MM-DD HH:MM TZ - Session Summary
- Verified repo facts:
  - ...
- Decisions made:
  - ...
- Assumptions still in force:
  - ...
- Open risks:
  - ...
- Next recommended actions:
  - ...
- Evidence paths / artifacts:
  - ...
```

### Entries

#### 2026-04-01 00:00 IST - Bootstrap Documentation Pass
- Verified repo facts:
  - The workspace started this session effectively empty.
  - No `.git` repository was initialized in the project root at verification time.
  - No source modules, benchmark scripts, or result artifacts existed before this docs pass.
- Decisions made:
  - `design.md` and `roadmap.md` were created as the initial source of truth for project scope and progress tracking.
  - Hermes v1 is defined as a single-host Linux orchestrator with a hybrid privilege model and observe-only support.
  - WSL2 is treated as a development environment, while real benchmarks are reserved for a native Linux NVIDIA host.
- Assumptions still in force:
  - Future implementation will follow the `monitor/`, `profiler/`, `engine/`, `actions/`, and `cli/` module split unless the docs are updated with evidence.
  - Metrics and roadmap items remain design-complete, not implementation-complete, until code or artifacts exist.
- Open risks:
  - No benchmark host, scripts, or workload harness exist yet.
  - GPU attribution details may vary depending on driver/runtime mode and need validation on target hardware.
- Next recommended actions:
  - Initialize the repo scaffold and create the module directories defined above.
  - Implement observe-only monitors first so PSI and NVML data can be logged before interventions are enabled.
  - Build a reproducible benchmark harness before claiming scheduler effectiveness.
- Evidence paths / artifacts:
  - `design.md`
  - `roadmap.md`

#### 2026-04-01 17:06 IST - Profiling and Sampling Doc Expansion
- Verified repo facts:
  - `design.md` and `roadmap.md` existed before this update.
  - The repo still contains documentation only; no source modules, benchmark captures, or helper binaries were added in this pass.
  - No `.git` repository was initialized at verification time.
- Decisions made:
  - Added hardware-aware scheduling assumptions to make memory hierarchy and VRAM headroom part of the stated control rationale.
  - Added a low-overhead sampling section that defines an optional C++ `/proc/<pid>/stat` helper with a fixed minimal output contract.
  - Added an explicit debugging and profiling strategy centered on `strace`, `perf`, and timestamp alignment with Hermes telemetry.
- Assumptions still in force:
  - Python remains the Hermes control-plane language, with C++ limited to optional hot-path introspection.
  - `strace` and `perf` are validation tools for Linux benchmarks, not always-on production dependencies.
  - No implementation claim is valid until code or benchmark artifacts exist in the repo.
- Open risks:
  - The C++ helper interface is documented but not yet prototyped.
  - No real `strace` or `perf` artifacts exist yet, so the profiling story remains a design commitment only.
- Next recommended actions:
  - Build the optional C++ process-stat helper and keep its output compatible with `ProcessSnapshot`.
  - Capture one real stressed-workload `strace` run and one real `perf` profile during the first benchmark harness pass.
  - Link future evidence artifacts back to roadmap items as they become real.
- Evidence paths / artifacts:
  - `design.md`
  - `roadmap.md`

#### 2026-04-01 17:18 IST - Defensibility Note Added
- Verified repo facts:
  - The docs already described the optional C++ helper and the need for `strace` and `perf` artifacts before this update.
  - Those implementation and evidence items still do not exist in the repo at verification time.
- Decisions made:
  - Added an explicit design-doc note defining the minimum evidence needed before making strong project claims.
  - Kept the requirement narrow: one real helper implementation, one real `strace` example, and one real `perf` example.
- Assumptions still in force:
  - Documentation coverage does not count as implementation.
  - Benchmark and profiling claims remain provisional until backed by saved artifacts.
- Open risks:
  - Strong resume or interview claims could still get ahead of the repo if the evidence package is not built next.
- Next recommended actions:
  - Implement the tiny C++ `/proc/<pid>/stat` helper.
  - Save one stressed-workload `strace` trace and one benchmark `perf` capture with notes.
  - Link those artifacts from the roadmap and future README.
- Evidence paths / artifacts:
  - `design.md`
  - `roadmap.md`

#### 2026-04-01 17:28 IST - Advanced Systems Framing Pass
- Verified repo facts:
  - The repo still contains design and roadmap documentation only.
  - No native daemon, eBPF probes, replay engine, fault-injection harness, or cgroup backend implementation exists yet.
  - No `gdb`, `strace`, `perf`, or replay artifacts exist in the repo at verification time.
- Decisions made:
  - Reframed the C++ path as a first-class native runtime component instead of a minor helper-only note.
  - Added explicit sections for scheduler state machine, cgroup v2 backend, eBPF observability, `gdb`-driven failure analysis, simulation/replay mode, and fault injection.
  - Extended telemetry artifacts so kernel traces, replay bundles, and postmortem debugging evidence are part of the documented evaluation model.
- Assumptions still in force:
  - Python remains the control plane, but native sampling is now treated as a core Linux runtime path for advanced deployments.
  - Kernel tracing and `gdb` remain optional capabilities tied to host support and controlled debugging workflows.
  - Documentation coverage does not upgrade implementation status.
- Open risks:
  - The architecture is now stronger on paper than in repo reality, so the roadmap must stay strict about unfinished implementation.
  - Native runtime complexity introduces future concurrency and failure-analysis work that the repo does not yet cover.
- Next recommended actions:
  - Add roadmap items for native collector, eBPF observability, replay/fault injection, scheduler state machine, and cgroup backend work.
  - Build the smallest native collector milestone next so the design stops feeling purely aspirational.
  - Capture real Linux-host artifacts once the first benchmark harness exists.
- Evidence paths / artifacts:
  - `design.md`
  - `roadmap.md`

#### 2026-04-01 17:43 IST - Industry Metrics PDF Integration
- Verified repo facts:
  - `design.md` existed before this update and already defined Hermes architecture, metrics groups, replay, and benchmark structure.
  - No new code, collectors, benchmark artifacts, or roadmap status changes were introduced in this pass.
- Decisions made:
  - Integrated the PDF as structured design guidance rather than embedding it verbatim.
  - Added instrumentation tiers, a named metric catalogue, explicit pass/strong targets, stricter artifact contracts, benchmark packaging rules, and a provenance appendix.
  - Kept all new targets framed as measurable goals that depend on environment tier and saved evidence.
- Assumptions still in force:
  - WSL/VM support remains partial and fallback-driven.
  - Native Linux GPU hosts remain the source of strongest PSI/NVML/eBPF claims.
  - Documentation upgrades do not imply implementation progress.
- Open risks:
  - The design now has a more rigorous metrics contract than the repo can currently satisfy.
  - Future implementation must enforce feature detection carefully to avoid pretending Tier C signals exist in Tier A environments.
- Next recommended actions:
  - Mirror the new metric IDs and artifact names in the eventual implementation scaffold.
  - Build the benchmark harness around `run_id`, `config_hash`, and NDJSON outputs from day one.
  - Add a roadmap sync later if you want the new metrics tiers and artifact contracts reflected in milestone wording.
- Evidence paths / artifacts:
  - `design.md`
  - `C:\\Users\\kumar\\Downloads\\Industry-Grade Metrics and Targets for a Hermes-Style GPU-Aware Scheduler.pdf`

#### 2026-04-01 20:39 IST - Backend Language Decision Shift to C++
- Verified repo facts:
  - The repo still contains documentation only; no backend implementation language has been committed in code.
  - Before this update, parts of the design and roadmap still described Python as the control-plane language even though newer sections already emphasized native runtime behavior.
- Decisions made:
  - Hermes backend is now explicitly defined as a C++ project rather than a Python project with native helpers.
  - System architecture, low-overhead sampling, and native runtime sections were rewritten so policy, logging, replay orchestration, and action dispatch all live in the C++ backend.
  - External workloads may still be Python ML jobs, but Hermes itself no longer depends on Python as a backend assumption.
- Assumptions still in force:
  - Native Linux remains the source of strongest observability and benchmark claims.
  - The repo still needs a future scaffold update to reflect the new C++-first layout in code.
- Open risks:
  - Historical handoff entries still mention Python as the control plane because the log is append-only; current design sections supersede that assumption.
  - Build-system, packaging, and library choices for the C++ project are still undecided in the docs.
- Next recommended actions:
  - Update the roadmap so scaffold and runtime milestones no longer point toward a Python project.
  - Add a concrete C++ project layout and build-system choice when you are ready to scaffold the repo.
  - Keep benchmark tooling and artifact contracts language-agnostic unless a native implementation detail matters.
- Evidence paths / artifacts:
  - `design.md`

#### 2026-04-02 00:56 IST - First Native Backend Control Loop Pass
- Verified repo facts:
  - The repo now contains a native C++ scaffold with `CMakeLists.txt`, `include/`, `src/`, `README.md`, `.gitignore`, and `config/schema.yaml`.
  - Monitor implementations exist for CPU PSI, memory PSI, load average fallback, and GPU stats collection through a lightweight `nvidia-smi` path.
  - Profiler implementations exist for `/proc/<pid>/stat` parsing, process mapping, and workload classification.
  - Engine implementations now exist for UPS calculation, predictive risk scoring, scheduler decisioning, and dry-run action execution.
  - `src/runtime/hermesd.cpp` wires these components into an observe-only loop.
  - Direct compilation succeeded with `g++`; `cmake` was unavailable in the current shell, so the CMake build path was not verified here.
- Decisions made:
  - Implemented the first end-to-end native control loop instead of extending documentation only.
  - Chose an observe-only plus dry-run execution path first so decisions can be inspected before host-mutating actions are added.
  - Kept GPU attribution on a lightweight `nvidia-smi` path for now, deferring direct NVML integration to a later milestone.
- Assumptions still in force:
  - Native Linux remains the intended runtime for strongest PSI and `/proc` coverage.
  - The current scheduler and action layers are scaffolding for policy validation, not production-grade intervention yet.
  - Benchmark, replay, `strace`, `perf`, `gdb`, and eBPF evidence are still absent.
- Open risks:
  - The GPU collector still relies on `nvidia-smi` rather than direct NVML bindings.
  - There is no ring buffer, IPC path, persistent event log, or benchmark harness yet.
  - Active host mutation, cgroup backends, and rollback logic are not implemented.
- Next recommended actions:
  - Replace or supplement the current GPU collector with a direct NVML-backed path.
  - Add event logging and NDJSON artifacts for samples, predictions, decisions, and actions.
  - Implement real Level 1 and Level 2 executors behind the existing dry-run interface.
  - Add a replay harness and benchmark runner before tightening thresholds further.
- Evidence paths / artifacts:
  - `CMakeLists.txt`
  - `include/hermes/core/types.hpp`
  - `include/hermes/engine/pressure_score.hpp`
  - `include/hermes/engine/predictor.hpp`
  - `include/hermes/engine/scheduler.hpp`
  - `include/hermes/actions/dry_run_executor.hpp`
  - `src/runtime/hermesd.cpp`
  - `src/engine/pressure_score.cpp`
  - `src/engine/predictor.cpp`
  - `src/engine/scheduler.cpp`
  - `src/actions/dry_run_executor.cpp`
  - `roadmap.md`

#### 2026-04-12 22:35 IST - Runtime NDJSON Artifact Logging Pass
- Verified repo facts:
  - Added `AGENTS.md` with project-specific build, verification, artifact, and handoff guidance.
  - Added artifact placeholder directories for logs, summaries, plots, replay data, and profiling captures.
  - Added a dependency-free C++ runtime event logger that opens a per-run directory under `artifacts/logs/<run_id>/`.
  - `src/runtime/hermesd.cpp` now writes `samples.ndjson`, `processes.ndjson`, `scores.ndjson`, `predictions.ndjson`, `decisions.ndjson`, `actions.ndjson`, and `events.ndjson` during observe-only runs.
  - Every emitted structured record carries `run_id`, `scenario`, and `config_hash`.
  - `HERMES_MAX_LOOPS` now allows bounded smoke runs without changing the default infinite daemon behavior.
  - Direct compilation succeeded with `g++`; `cmake` is still unavailable in this shell.
  - A one-loop smoke run completed with `HERMES_RUN_ID=codex-smoke-20260412-2235` and produced parseable NDJSON records.
- Decisions made:
  - Kept the logging path C++17-only and dependency-free so it works before package/dependency choices are settled.
  - Logged scheduler state transitions as generic `events.ndjson` records when the scheduler state changes.
  - Kept generated run logs ignored by git while tracking `.gitkeep` placeholders for the artifact layout.
- Assumptions still in force:
  - The current logs are control-loop smoke artifacts, not benchmark evidence.
  - Replay, benchmark harnesses, active host mutation, cgroup backends, direct NVML integration, `strace`, `perf`, `gdb`, and eBPF evidence are still pending.
  - Native Linux GPU hosts remain the source of strongest PSI/NVML and benchmark claims.
- Open risks:
  - There is no bounded ring buffer or IPC publisher yet.
  - NDJSON schemas are initial and may need tightening once replay and CLI consumers exist.
  - Console output and artifact logging are still coupled inside `hermesd`.
- Next recommended actions:
  - Add a small replay reader that can consume `samples.ndjson`, `predictions.ndjson`, `decisions.ndjson`, and `actions.ndjson`.
  - Add a benchmark harness that writes run metadata, scenario config snapshots, and summary rows next to the existing NDJSON files.
  - Implement real Level 1 advisory/reprioritization behavior behind the existing decision path.
- Evidence paths / artifacts:
  - `AGENTS.md`
  - `.gitignore`
  - `artifacts/logs/.gitkeep`
  - `artifacts/summaries/.gitkeep`
  - `artifacts/plots/.gitkeep`
  - `artifacts/replay/.gitkeep`
  - `artifacts/profiles/.gitkeep`
  - `include/hermes/runtime/event_logger.hpp`
  - `src/runtime/event_logger.cpp`
  - `src/runtime/hermesd.cpp`
  - `include/hermes/core/types.hpp`
  - `src/engine/scheduler.cpp`
  - `CMakeLists.txt`
  - `README.md`
  - `roadmap.md`
  - `artifacts/logs/codex-smoke-20260412-2235/`

#### 2026-04-12 23:09 IST - Replay Summary CLI Pass
- Verified repo facts:
  - Added `include/hermes/replay/replay_summary.hpp` and `src/replay/replay_summary.cpp`.
  - Added `src/cli/hermes_replay.cpp` as the first CLI entrypoint.
  - Updated `CMakeLists.txt` so `hermes_core` includes replay summary code and builds a `hermes_replay` executable.
  - `hermes_replay` reads a run directory, validates common `run_id`, `scenario`, and `config_hash` fields, summarizes counts, time window, peak pressure/risk values, scheduler states, actions, process classes, and event kinds.
  - `hermes_replay` writes `replay_summary.json` beside the run and copies the same summary to `artifacts/replay/<run_id>-summary.json`.
  - `README.md` now documents the replay summary command and states that deterministic scheduler re-execution is still pending.
  - Direct compilation of the replay CLI succeeded with `g++`; `cmake` remains unavailable in this shell.
  - Smoke verification against `artifacts/logs/codex-smoke-20260412-2235/` succeeded and produced a valid summary with `samples=1`, `decisions=1`, `actions=1`, and `events=3`.
- Decisions made:
  - Implemented a summary and validation tool first instead of a full scheduler replay engine.
  - Kept the NDJSON reader dependency-free and scoped to the flat Hermes artifact fields currently emitted by the daemon.
  - Wrote summaries under both the run directory and `artifacts/replay/` so later tooling can discover replay outputs consistently.
- Assumptions still in force:
  - Replay summary artifacts are validation and inspection outputs, not benchmark evidence.
  - Full deterministic policy replay still needs saved replay frames, config loading, and scheduler re-execution.
  - Benchmark harnesses, active control, cgroup backends, direct NVML integration, `strace`, `perf`, `gdb`, and eBPF evidence remain pending.
- Open risks:
  - The NDJSON reader is intentionally small and should be replaced or hardened if schemas become deeply nested.
  - Empty `processes.ndjson` files are accepted for low-activity or unsupported environments, but richer validation will need scenario-aware expectations.
  - Replay summaries do not yet compare expected and actual decisions or compute replay fidelity metrics.
- Next recommended actions:
  - Add a scenario/run metadata writer so each daemon or benchmark run records host metadata and config snapshots next to the NDJSON files.
  - Add a synthetic trace or tiny fixture run to exercise non-observe actions and state transitions in replay summaries.
  - Begin a minimal benchmark harness that can launch a bounded scenario and produce summary rows from these artifacts.
- Evidence paths / artifacts:
  - `include/hermes/replay/replay_summary.hpp`
  - `src/replay/replay_summary.cpp`
  - `src/cli/hermes_replay.cpp`
  - `CMakeLists.txt`
  - `README.md`
  - `roadmap.md`
  - `artifacts/logs/codex-smoke-20260412-2235/replay_summary.json`
  - `artifacts/replay/codex-smoke-20260412-2235-summary.json`

#### 2026-04-13 00:57 IST - Run Metadata And Config Snapshot Pass
- Verified repo facts:
  - Added `include/hermes/runtime/run_metadata.hpp` and `src/runtime/run_metadata.cpp`.
  - `src/runtime/hermesd.cpp` now writes `run_metadata.json` and `config_snapshot.yaml` into each run directory after event logging opens.
  - `run_metadata.json` records run identity, runtime mode, artifact/config paths, host facts, compiler facts, and feature probes for `/proc`, PSI, `/proc/loadavg`, config source, and config snapshot availability.
  - `HERMES_CONFIG_PATH` is now honored for both config snapshotting and the default config hash path.
  - `hermes_replay` summaries now report whether `run_metadata.json` and `config_snapshot.yaml` are present and their byte sizes.
  - `README.md` and `roadmap.md` were updated to document and track the metadata/config snapshot artifacts.
  - Direct `g++` compilation succeeded for both `hermesd` and `hermes_replay`; `cmake` is still unavailable in this shell.
  - A fresh one-loop smoke run with `HERMES_RUN_ID=codex-smoke-20260413-metadata` produced `run_metadata.json`, `config_snapshot.yaml`, NDJSON logs, and a valid replay summary with metadata and config snapshot presence set to true.
- Decisions made:
  - Kept run metadata writing separate from event logging so benchmark harnesses and future CLI flows can reuse it.
  - Saved the config snapshot as YAML rather than converting it to JSON, preserving the source config exactly.
  - Treated missing metadata/config snapshot as replay warnings rather than fatal errors so older logs remain inspectable.
- Assumptions still in force:
  - These smoke artifacts validate run-context capture but are not benchmark evidence.
  - Full host metadata for native Linux GPU runs still needs richer probes such as GPU model, driver version, kernel version, and runtime mode details.
  - Full deterministic replay, benchmark harnesses, active control, cgroup backends, direct NVML integration, `strace`, `perf`, `gdb`, and eBPF evidence remain pending.
- Open risks:
  - Feature probes are filesystem-level checks only and do not yet validate end-to-end telemetry quality.
  - `run_metadata.json` does not yet include NVIDIA driver/GPU model details.
  - `telemetry_quality.json` and summary table generation are still absent.
- Next recommended actions:
  - Add a telemetry-quality artifact that records collector availability and sample counts from each run.
  - Add a scenario/run metadata file for benchmark harness inputs and expected workload commands.
  - Add a synthetic pressure fixture to exercise non-normal bands and replay summary counts.
- Evidence paths / artifacts:
  - `include/hermes/runtime/run_metadata.hpp`
  - `src/runtime/run_metadata.cpp`
  - `src/runtime/hermesd.cpp`
  - `include/hermes/replay/replay_summary.hpp`
  - `src/replay/replay_summary.cpp`
  - `CMakeLists.txt`
  - `README.md`
  - `roadmap.md`
  - `artifacts/logs/codex-smoke-20260413-metadata/run_metadata.json`
  - `artifacts/logs/codex-smoke-20260413-metadata/config_snapshot.yaml`
  - `artifacts/logs/codex-smoke-20260413-metadata/replay_summary.json`
  - `artifacts/replay/codex-smoke-20260413-metadata-summary.json`

#### 2026-04-13 01:11 IST - Telemetry Quality Artifact Pass
- Verified repo facts:
  - Added `include/hermes/runtime/telemetry_quality.hpp` and `src/runtime/telemetry_quality.cpp`.
  - `src/runtime/hermesd.cpp` now updates a telemetry quality tracker on every control-loop iteration.
  - Each daemon run writes `telemetry_quality.json` beside the NDJSON logs, run metadata, and config snapshot.
  - `telemetry_quality.json` includes sample counts, provider availability ratios, loop interval and jitter metrics, process refresh counts, decision/action counts, state transition counts, peak UPS, peak risk, pressure-band counts, risk-band counts, scheduler-state counts, and decision-action counts.
  - Replay summaries now report whether `telemetry_quality.json` is present and its byte size.
  - `README.md` and `roadmap.md` were updated to document and track telemetry-quality artifacts.
  - Direct `g++` compilation succeeded for both `hermesd` and `hermes_replay`; `cmake` is still unavailable in this shell.
  - A fresh two-loop smoke run with `HERMES_RUN_ID=codex-smoke-20260413-telemetry` produced a valid replay summary and `telemetry_quality.json` with `samples=2`, `gpu_stats.availability_ratio=1`, `cpu_psi.availability_ratio=0`, `interval_count=1`, and `decisions=2`.
- Decisions made:
  - Wrote telemetry quality as an updating run artifact rather than waiting for daemon shutdown, so bounded and interrupted observe runs can still leave useful quality state.
  - Kept missing providers as measured availability ratios instead of treating unsupported local telemetry as a runtime failure.
  - Kept replay validation limited to presence/size checks for `telemetry_quality.json`; deeper quality thresholds should come with benchmark scenarios.
- Assumptions still in force:
  - The smoke run validates artifact generation and parsing only; it is not benchmark evidence.
  - Loop interval/jitter currently includes collector overhead such as `nvidia-smi` latency.
  - Full deterministic replay, scenario fixtures, benchmark harnesses, active control, cgroup backends, direct NVML integration, `strace`, `perf`, `gdb`, and eBPF evidence remain pending.
- Open risks:
  - Telemetry quality does not yet record per-provider error strings or drop reasons.
  - Quality thresholds are not scenario-aware yet, so the artifact reports facts rather than pass/fail health.
  - Long-running daemon shutdown still needs signal handling if final-only artifacts are added later.
- Next recommended actions:
  - Add synthetic trace fixtures that exercise elevated, critical, throttled, and cooldown states without needing live GPU pressure.
  - Add per-provider first/last error details where collectors expose them.
  - Start a minimal benchmark harness that launches a bounded observe-only run and copies replay summaries into `artifacts/summaries/`.
- Evidence paths / artifacts:
  - `include/hermes/runtime/telemetry_quality.hpp`
  - `src/runtime/telemetry_quality.cpp`
  - `src/runtime/hermesd.cpp`
  - `include/hermes/replay/replay_summary.hpp`
  - `src/replay/replay_summary.cpp`
  - `CMakeLists.txt`
  - `README.md`
  - `roadmap.md`
  - `artifacts/logs/codex-smoke-20260413-telemetry/telemetry_quality.json`
  - `artifacts/logs/codex-smoke-20260413-telemetry/replay_summary.json`
  - `artifacts/replay/codex-smoke-20260413-telemetry-summary.json`

#### 2026-04-13 01:27 IST - Synthetic Pressure Fixture CLI Pass
- Verified repo facts:
  - Added `src/cli/hermes_synth.cpp`.
  - `CMakeLists.txt` now defines a `hermes_synth` executable linked against `hermes_core`.
  - `hermes_synth` generates deterministic synthetic `PressureSample` frames and synthetic `ProcessSnapshot` records without requiring live GPU pressure.
  - The fixture drives the real UPS calculator, OOM predictor, scheduler, dry-run executor, event logger, run metadata writer, and telemetry quality tracker.
  - The generated run directory includes `scenario_manifest.json` in addition to the same NDJSON, metadata, config snapshot, telemetry quality, and replay summary artifacts used by daemon runs.
  - `README.md` now documents `hermes_synth` and frames it as replay/schema coverage rather than benchmark evidence.
  - Direct `g++` compilation succeeded for `hermes_synth` and `hermes_replay`; `cmake` is still unavailable in this shell.
  - A synthetic fixture run with `run_id=codex-synth-20260413-level2` replayed successfully with `samples=8`, `decisions=8`, `actions=8`, `peak_ups=96.1`, and `peak_risk=1`.
  - The fixture summary included `reprioritize=1`, `throttle=1`, `terminate_candidate=1`, `cooldown=3`, `throttled=1`, `recovery=1`, and no replay warnings.
- Decisions made:
  - Implemented fixture generation through the real engine path instead of hand-writing decision logs.
  - Tuned the synthetic frames to hit Level 1, Level 2, Level 3, cooldown, recovery, and normal states in one short trace.
  - Kept the fixture in observe-only/dry-run behavior, so it exercises decisions without mutating the host.
- Assumptions still in force:
  - Synthetic fixtures are replay and schema evidence, not host benchmark evidence.
  - The fixture does not replace live pressure benchmarks, `strace`, `perf`, `gdb`, eBPF, cgroup, or direct NVML work.
  - Deterministic scheduler re-execution from saved traces is still pending.
- Open risks:
  - Fixture expectations are documented in `scenario_manifest.json`, but there is not yet an automated assertion command that fails if expected actions are missing.
  - The synthetic process model is intentionally simple and may need additional variants for future policy cases.
  - Existing generated run directories can be appended to if a caller reuses a run id.
- Next recommended actions:
  - Add replay assertions for synthetic manifests so fixture regressions fail automatically.
  - Add a tiny benchmark harness wrapper that runs `hermes_synth`, runs `hermes_replay`, and copies summaries into `artifacts/summaries/`.
  - Add direct checks for action/state counts in CI-style smoke commands once the repo has a formal test target.
- Evidence paths / artifacts:
  - `src/cli/hermes_synth.cpp`
  - `CMakeLists.txt`
  - `README.md`
  - `roadmap.md`
  - `artifacts/logs/codex-synth-20260413-level2/scenario_manifest.json`
  - `artifacts/logs/codex-synth-20260413-level2/decisions.ndjson`
  - `artifacts/logs/codex-synth-20260413-level2/telemetry_quality.json`
  - `artifacts/logs/codex-synth-20260413-level2/replay_summary.json`
  - `artifacts/replay/codex-synth-20260413-level2-summary.json`

#### 2026-04-13 01:33 IST - Replay Manifest Assertion Pass
- Verified repo facts:
  - `include/hermes/replay/replay_summary.hpp` now tracks optional scenario manifest presence, expected signals, observed signals, and assertion pass/fail counts.
  - `src/replay/replay_summary.cpp` now detects `scenario_manifest.json`, parses `expected_signals`, derives observed signals from `decisions.ndjson`, and marks the summary invalid when a manifest expectation is missing.
  - `src/cli/hermes_replay.cpp` now prints assertion pass counts and lists assertion failures.
  - `README.md` documents that `scenario_manifest.json` expectations are replay assertions and make the CLI exit nonzero when missing.
  - `roadmap.md` now reflects replay manifest assertion coverage.
  - Direct `g++` compilation of `hermes_replay` succeeded; `cmake` remains unavailable in this shell.
  - Replaying `artifacts/logs/codex-synth-20260413-level2/` passed with `assertions=5/5`.
  - A copied negative fixture at `artifacts/logs/codex-synth-20260413-assertion-fail/` with one bogus expected signal exited with code `3` and reported `missing expected signal: missing_signal_for_test`.
- Decisions made:
  - Kept scenario manifests optional so normal daemon logs without fixture expectations remain replayable.
  - Derived fixture signals from recorded decisions instead of trusting telemetry summaries, keeping assertions grounded in the decision artifact.
  - Made assertion failures set `valid=false`, so fixture regressions can be caught by command exit code.
- Assumptions still in force:
  - These assertions validate synthetic fixture coverage, not live benchmark performance.
  - Full scheduler re-execution from saved traces remains pending.
  - Generated negative-test artifacts are disposable and do not count as benchmark evidence.
- Open risks:
  - Manifest parsing is intentionally lightweight and only supports the current simple `expected_signals` array shape.
  - Expected signals are currently name-based and do not yet support numeric thresholds such as minimum UPS or exact action counts.
  - The replay CLI does not yet have named subcommands or a formal test runner.
- Next recommended actions:
  - Add threshold/count assertions to `scenario_manifest.json`, such as minimum `peak_ups`, minimum action counts, and expected state counts.
  - Add a small smoke-test wrapper that runs `hermes_synth`, `hermes_replay`, and checks the exit code.
  - Start writing summary artifacts into `artifacts/summaries/` for fixture and future benchmark runs.
- Evidence paths / artifacts:
  - `include/hermes/replay/replay_summary.hpp`
  - `src/replay/replay_summary.cpp`
  - `src/cli/hermes_replay.cpp`
  - `README.md`
  - `roadmap.md`
  - `artifacts/logs/codex-synth-20260413-level2/replay_summary.json`
  - `artifacts/logs/codex-synth-20260413-assertion-fail/replay_summary.json`

#### 2026-04-13 01:43 IST - Plain-Language Explanation And Rich Replay Assertions Pass
- Verified repo facts:
  - Added `HERMES_EXPLANATION.md` as a plain-language article that explains what Hermes is, why it exists, what has been built so far, what the runtime artifacts mean, how replay and synthetic fixtures work, and what is still not implemented.
  - `src/cli/hermes_synth.cpp` now emits richer `scenario_manifest.json` expectations: named signals, minimum peak UPS, minimum peak risk, minimum decision action counts, minimum scheduler state counts, minimum pressure band counts, and minimum risk band counts.
  - `include/hermes/replay/replay_summary.hpp` and `src/replay/replay_summary.cpp` now track and validate richer manifest expectations.
  - `replay_summary.json` now includes expected minimums, expected action/state/band counts, observed signals, and assertion failures in the `manifest_assertions` block.
  - `README.md` now documents that manifest assertions can check named signals, thresholds, action counts, state counts, and band counts.
  - `roadmap.md` now reflects signal/count/threshold assertion coverage.
  - Direct `g++` compilation succeeded for `hermes_synth` and `hermes_replay`; `cmake` remains unavailable in this shell.
  - A fresh fixture run with `run_id=codex-synth-20260413-rich-assert` replayed successfully with `assertions=17/17`, `peak_ups=96.1`, and `peak_risk=1`.
  - A copied negative fixture at `artifacts/logs/codex-synth-20260413-rich-assert-fail/` exited with code `3` and reported failures for `peak_ups >= 120` and `throttle` count `>= 2`.
- Decisions made:
  - Wrote the explanation document as prose rather than a checklist so it is easier for a new reader to understand the project story.
  - Kept rich manifest assertions as minimum expectations rather than exact counts, so harmless extra events do not break fixtures.
  - Kept failure behavior tied to `summary.valid=false` and CLI exit code `3`, matching the previous manifest assertion behavior.
- Assumptions still in force:
  - Synthetic fixtures and assertions are regression evidence, not live benchmark proof.
  - Full scheduler re-execution from saved traces is still pending.
  - CMake needs verification once installed in the shell or on a native build host.
- Open risks:
  - Manifest parsing remains lightweight and still assumes simple JSON object shapes.
  - Assertions do not yet support exact counts, maximum counts, latency metrics, or time-window expectations.
  - Negative fixture directories are generated verification artifacts and should not be mistaken for benchmark runs.
- Next recommended actions:
  - Add a small smoke-test command or script that runs `hermes_synth`, runs `hermes_replay`, and fails automatically on assertion errors.
  - Write fixture summaries into `artifacts/summaries/` so replay outputs have a benchmark-adjacent summary lane.
  - Add exact-count and maximum-count assertion support only if a future fixture needs stricter checks.
- Evidence paths / artifacts:
  - `HERMES_EXPLANATION.md`
  - `src/cli/hermes_synth.cpp`
  - `include/hermes/replay/replay_summary.hpp`
  - `src/replay/replay_summary.cpp`
  - `README.md`
  - `roadmap.md`
  - `artifacts/logs/codex-synth-20260413-rich-assert/scenario_manifest.json`
  - `artifacts/logs/codex-synth-20260413-rich-assert/replay_summary.json`
  - `artifacts/logs/codex-synth-20260413-rich-assert-fail/replay_summary.json`

#### 2026-04-13 01:48 IST - Synthetic Replay Smoke Script Pass
- Verified repo facts:
  - Added `scripts/smoke_synthetic_replay.ps1`.
  - The script builds `hermes_synth` and `hermes_replay` with direct `g++` commands, generates a synthetic run, replays it, parses `replay_summary.json`, and fails if manifest assertions are missing or failing.
  - `README.md` now documents the smoke command.
  - `roadmap.md` now tracks the local synthetic replay smoke script.
  - Running `.\scripts\smoke_synthetic_replay.ps1 -RunId codex-smoke-script-20260413` succeeded.
  - The smoke run produced `assertions=17/17`, `peak_ups=96.1`, and `peak_risk=1`.
- Decisions made:
  - Kept the smoke wrapper in PowerShell because the current development shell is PowerShell on Windows.
  - Kept the script on the direct `g++` path because CMake is still unavailable in this shell.
  - Made the script check the generated summary JSON directly instead of trusting console output alone.
- Assumptions still in force:
  - This smoke script verifies synthetic replay behavior only, not live host pressure or benchmark performance.
  - CMake still needs verification in an environment where it is installed.
  - Generated smoke run artifacts remain ignored evidence outputs, not source files.
- Open risks:
  - The smoke script currently assumes `g++` is on PATH.
  - There is not yet an equivalent shell script for native Linux environments.
  - There is not yet a formal test target that invokes this script through CMake or CI.
- Next recommended actions:
  - Add a native shell version or CTest target once CMake is available.
  - Add a summary-copy step into `artifacts/summaries/` for smoke and future benchmark runs.
  - Add direct smoke checks for daemon one-loop artifact generation.
- Evidence paths / artifacts:
  - `scripts/smoke_synthetic_replay.ps1`
  - `README.md`
  - `roadmap.md`
  - `artifacts/logs/codex-smoke-script-20260413/replay_summary.json`
  - `artifacts/replay/codex-smoke-script-20260413-summary.json`

#### 2026-04-13 02:17 IST - Replay Summary Copy Pass
- Verified repo facts:
  - Before this pass, the full initial Hermes scaffold was committed and pushed to the private GitHub repository `kumarabhik/Hermes` on branch `main`.
  - `HERMES_EXPLANATION.md` is ignored by `.gitignore` and was not included in the GitHub commit.
  - `src/cli/hermes_replay.cpp` now writes replay summaries to the run directory, `artifacts/replay/<run_id>-summary.json`, and `artifacts/summaries/<run_id>-summary.json`.
  - `scripts/smoke_synthetic_replay.ps1` now verifies that the `artifacts/summaries/` copy exists and matches the run id.
  - `README.md` documents the summary copy path and updated smoke behavior.
  - `roadmap.md` now records summary-copy behavior.
- Decisions made:
  - Kept `artifacts/replay/` as the replay-specific copy while adding `artifacts/summaries/` as the general summary lane for future smoke and benchmark outputs.
  - Made the smoke script validate the summary copy directly instead of relying only on replay exit code.
- Assumptions still in force:
  - Generated summary artifacts remain ignored by git.
  - This is still synthetic replay validation, not live benchmark evidence.
  - CMake remains unavailable in this shell, so direct `g++` smoke verification is still the local path.
- Open risks:
  - No CSV summary table exists yet.
  - There is not yet a Linux shell equivalent of the PowerShell smoke script.
  - The GitHub repository was created private by default because public visibility was not requested.
- Next recommended actions:
  - Add a compact CSV summary writer under `artifacts/summaries/` for fixture and future benchmark rows.
  - Add a Linux shell smoke script or CTest wrapper once CMake is available.
  - Add a daemon one-loop smoke script that verifies live observe artifact generation.
- Evidence paths / artifacts:
  - `src/cli/hermes_replay.cpp`
  - `scripts/smoke_synthetic_replay.ps1`
  - `README.md`
  - `roadmap.md`

#### 2026-04-14 IST - Replay Summary CSV Pass
- Verified repo facts:
  - `include/hermes/replay/replay_summary.hpp` now exposes CSV summary writing and CSV row serialization helpers.
  - `src/replay/replay_summary.cpp` now writes a compact one-row CSV with run identity, artifact counts, time window, pressure/risk peaks, pressure/risk/state/action counts, artifact presence flags, assertion counts, and warning/failure counts.
  - `src/cli/hermes_replay.cpp` now writes `summary.csv` beside the run directory and copies CSV summaries to `artifacts/replay/<run_id>-summary.csv` and `artifacts/summaries/<run_id>-summary.csv`.
  - `scripts/smoke_synthetic_replay.ps1` now verifies the run CSV and artifact CSV copy, checks matching `run_id`, requires `valid=true`, and fails if CSV assertion failures are nonzero.
  - `README.md` documents the JSON and CSV summary outputs, and `roadmap.md` records replay JSON/CSV summary behavior.
  - Running `.\scripts\smoke_synthetic_replay.ps1 -RunId codex-summary-csv-20260414` succeeded with `assertions=17/17`, `peak_ups=96.1`, and `peak_risk=1`.
  - The smoke run wrote `artifacts/logs/codex-summary-csv-20260414/summary.csv`, `artifacts/replay/codex-summary-csv-20260414-summary.csv`, and `artifacts/summaries/codex-summary-csv-20260414-summary.csv`.
  - `cmake` is still unavailable in this PowerShell environment, so verification used the direct `g++` smoke path.
- Decisions made:
  - Kept the per-run CSV as a single header plus one data row so fixture and future benchmark tools can consume it without parsing nested JSON.
  - Copied CSVs to both `artifacts/replay/` and `artifacts/summaries/` to match the existing JSON summary discovery pattern.
  - Limited this CSV to replay-observed fields; real OOM counts, foreground latency, jobs completed, and degraded-behavior notes still belong to the benchmark harness/reporting layer.
- Assumptions still in force:
  - Generated CSV and JSON summaries remain ignored artifacts unless a future evidence bundle intentionally tracks selected outputs.
  - Synthetic fixture verification is regression evidence for replay behavior, not live benchmark proof.
  - The CMake build path still needs verification in an environment where CMake is installed.
- Open risks:
  - The CSV schema is intentionally compact and may need extension when real benchmark metrics are available.
  - The PowerShell smoke script still assumes `g++` is available on PATH.
  - There is not yet a Linux shell smoke script or CTest target for replay CSV verification.
- Next recommended actions:
  - Add a daemon one-loop smoke script that verifies live observe artifact generation and replay CSV creation.
  - Add a Linux shell smoke wrapper or CTest target once CMake is available.
  - Continue the benchmark harness so required benchmark table fields like OOM count, latency, jobs completed, and notes become artifact-backed.
- Evidence paths / artifacts:
  - `include/hermes/replay/replay_summary.hpp`
  - `src/replay/replay_summary.cpp`
  - `src/cli/hermes_replay.cpp`
  - `scripts/smoke_synthetic_replay.ps1`
  - `README.md`
  - `roadmap.md`
  - `artifacts/logs/codex-summary-csv-20260414/replay_summary.json`
  - `artifacts/logs/codex-summary-csv-20260414/summary.csv`
  - `artifacts/replay/codex-summary-csv-20260414-summary.csv`
  - `artifacts/summaries/codex-summary-csv-20260414-summary.csv`

#### 2026-04-14 IST - One-Loop Daemon Replay Smoke Pass
- Verified repo facts:
  - Added `scripts/smoke_daemon_replay.ps1`.
  - The script builds `hermesd` and `hermes_replay` with direct `g++` commands, runs `hermesd` with `HERMES_MAX_LOOPS=1`, `HERMES_RUNTIME_MODE=observe-only`, and `HERMES_SCENARIO=daemon-smoke`, then replays the daemon run directory.
  - The script verifies daemon artifacts are present: `run_metadata.json`, `config_snapshot.yaml`, `telemetry_quality.json`, `samples.ndjson`, `processes.ndjson`, `scores.ndjson`, `predictions.ndjson`, `decisions.ndjson`, `actions.ndjson`, and `events.ndjson`.
  - The script verifies non-empty daemon artifacts for metadata, telemetry, samples, scores, predictions, decisions, actions, and events. `processes.ndjson` may be empty when no processes are mapped in the smoke environment.
  - The script verifies replay JSON and CSV outputs in the run directory and in `artifacts/summaries/`.
  - `README.md` now documents both the synthetic replay smoke script and the daemon replay smoke script.
  - `roadmap.md` now records local `g++` smoke verification for both synthetic replay and one-loop daemon replay.
  - Running `.\scripts\smoke_daemon_replay.ps1 -RunId codex-daemon-smoke-20260414` succeeded.
  - The verified daemon smoke run had `samples=1`, `decisions=1`, `actions=1`, `valid=true`, `peak_ups=0`, and `peak_risk=0` in this Windows authoring environment.
- Decisions made:
  - Kept the daemon smoke script separate from the synthetic fixture script because one validates deterministic policy coverage while the other validates live daemon artifact plumbing.
  - Used observe-only mode and `HERMES_MAX_LOOPS=1` so the script is safe and bounded by default.
  - Treated empty `processes.ndjson` as acceptable for the smoke script because this environment may not expose Linux `/proc` process details.
- Assumptions still in force:
  - This smoke test validates daemon artifact generation and replay parsing, not real pressure behavior or benchmark impact.
  - Direct `g++` verification remains the local path until CMake is available.
  - Generated smoke artifacts remain ignored by git.
- Open risks:
  - The script currently targets PowerShell and assumes `g++` is on PATH.
  - The one-loop smoke run does not exercise scheduler pressure transitions.
  - Native Linux verification is still needed for real PSI, `/proc`, and active-control paths.
- Next recommended actions:
  - Add a Linux shell equivalent or CTest wrapper once CMake can be exercised.
  - Start extending `hermes_bench` from config validation into bounded workload launching.
  - Add benchmark artifact checks for OOM count, latency, jobs completed, and degraded-behavior notes.
- Evidence paths / artifacts:
  - `scripts/smoke_daemon_replay.ps1`
  - `README.md`
  - `roadmap.md`
  - `artifacts/logs/codex-daemon-smoke-20260414/replay_summary.json`
  - `artifacts/logs/codex-daemon-smoke-20260414/summary.csv`
  - `artifacts/logs/codex-daemon-smoke-20260414/telemetry_quality.json`
  - `artifacts/summaries/codex-daemon-smoke-20260414-summary.csv`

#### 2026-04-14 IST - Benchmark Plan Artifact Pass
- Verified repo facts:
  - Added `RESULTS.md` as a local-only results summary and added it to `.gitignore` so it is not committed to GitHub.
  - Added `artifacts/bench/.gitkeep` and ignore rules for generated benchmark plan artifacts under `artifacts/bench/`.
  - `src/cli/hermes_bench.cpp` now validates benchmark scenario plans, counts foreground/background workloads, computes planned workload seconds, and writes a JSON plan artifact plus normalized scenario snapshot.
  - `hermes_bench` now accepts `--artifact-root` and `--run-id` for deterministic plan artifact paths.
  - Added `scripts/smoke_benchmark_plan.ps1`.
  - The benchmark plan smoke script builds `hermes_bench` with direct `g++`, generates a default baseline scenario, runs a dry-run plan, verifies `artifacts/bench/<run_id>-plan.json`, verifies `artifacts/bench/<run_id>-scenario.yaml`, and checks the plan fields.
  - `README.md` documents the benchmark plan smoke script.
  - `roadmap.md` records benchmark plan artifact generation while keeping workload launch and benchmark evidence open.
  - Running `.\scripts\smoke_benchmark_plan.ps1 -RunId codex-bench-plan-20260414` succeeded.
  - The verified plan recorded `workload_count=4`, `foreground_workloads=1`, `background_workloads=3`, and `planned_workload_seconds=2400`.
- Decisions made:
  - Wrote benchmark plan artifacts before workload launch so future benchmark runs have an auditable scenario plan and config snapshot.
  - Kept generated benchmark plan outputs ignored by git, matching the rest of the artifact policy.
  - Treated `RESULTS.md` as a local progress note because the user asked not to commit results to GitHub.
- Assumptions still in force:
  - Benchmark plan artifacts are planning evidence, not performance evidence.
  - Real benchmark claims still require native Linux GPU runs with saved run outputs.
  - Direct `g++` smoke verification remains the local path until CMake is available.
- Open risks:
  - `hermes_bench` still does not launch or supervise workloads.
  - No OOM count, p95 latency, job completion, `strace`, or `perf` evidence exists yet.
  - The scenario parser is intentionally simple and not a full YAML parser.
- Next recommended actions:
  - Add bounded workload launch supervision to `hermes_bench`.
  - Add benchmark run-summary artifacts for OOM count, latency placeholders, jobs completed, and degraded-behavior notes.
  - Add a Linux shell or CTest wrapper for smoke checks when CMake is available.
- Evidence paths / artifacts:
  - `.gitignore`
  - `artifacts/bench/.gitkeep`
  - `src/cli/hermes_bench.cpp`
  - `scripts/smoke_benchmark_plan.ps1`
  - `README.md`
  - `roadmap.md`
  - `artifacts/bench/codex-bench-plan-20260414-plan.json`
  - `artifacts/bench/codex-bench-plan-20260414-scenario.yaml`

#### 2026-04-14 IST - Benchmark Launch Summary Pass
- Verified repo facts:
  - `src/cli/hermes_bench.cpp` now launches bounded workloads, supervises them for the benchmark window, terminates overruns, and writes `artifacts/bench/<run_id>-summary.json`.
  - The benchmark summary artifact records run identity, runtime mode, duration, workload counts, launched count, launch failures, completed jobs, timed-out jobs, nonzero exits, notes, and per-workload exit details.
  - Added `scripts/smoke_benchmark_launch.ps1`.
  - The benchmark launch smoke script builds `hermes_bench`, writes a four-workload local scenario, runs the benchmark harness, and verifies the plan, scenario snapshot, and summary artifacts.
  - `README.md` now documents the benchmark launch smoke script.
  - `roadmap.md` now reflects bounded benchmark workload launch and partial baseline-mode progress while keeping full benchmark evidence open.
  - Running `.\scripts\smoke_benchmark_launch.ps1 -RunId codex-bench-launch-20260414` succeeded.
  - The verified launch smoke run recorded `launched=4`, `launch_failed=0`, `jobs_completed=3`, `timed_out=1`, and `duration_ms=2100`.
- Decisions made:
  - Kept workload launch bounded by both the benchmark window and each workload's declared `duration_s`.
  - Wrote benchmark run summaries under `artifacts/bench/` instead of overloading replay summary artifacts meant for daemon and fixture runs.
  - Used harmless local PowerShell commands in the smoke script so workload launch behavior can be verified without external benchmark dependencies.
- Assumptions still in force:
  - These benchmark run summaries are harness-level artifacts, not proof of better OOM behavior or lower latency.
  - Real benchmark claims still require native Linux GPU runs with Hermes in the loop.
  - `RESULTS.md` remains local-only and ignored by git.
- Open risks:
  - `hermes_bench` still does not launch `hermesd` or coordinate Hermes runtime modes around workloads.
  - The current harness does not yet collect OOM counts, p95 latency, or profiling captures.
  - The smoke launch scenario is Windows-oriented because the current authoring shell is PowerShell.
- Next recommended actions:
  - Add Hermes daemon orchestration around benchmark workload runs.
  - Add benchmark run-summary fields for OOM count, jobs completed vs expected, and degraded-behavior notes.
  - Add Linux-native smoke coverage for workload launch and Hermes orchestration once CMake/native Linux is available.
- Evidence paths / artifacts:
  - `src/cli/hermes_bench.cpp`
  - `scripts/smoke_benchmark_launch.ps1`
  - `README.md`
  - `roadmap.md`
  - `artifacts/bench/codex-bench-launch-20260414-plan.json`
  - `artifacts/bench/codex-bench-launch-20260414-scenario.yaml`
  - `artifacts/bench/codex-bench-launch-20260414-summary.json`

#### 2026-04-15 IST - Benchmark Hermes Replay Orchestration Pass
- Verified repo facts:
  - `src/cli/hermes_bench.cpp` now treats `baseline` as a first-class benchmark runtime mode and only launches Hermes for non-baseline runs.
  - `hermes_bench` now accepts `--hermes-bin` and `--replay-bin` overrides, resolves sibling binaries by default, launches `hermesd` for non-baseline scenarios, waits for the Hermes run to finish, and then invokes `hermes_replay` on the generated run directory.
  - The benchmark summary artifact now records Hermes launch status, Hermes/replay binary paths, the Hermes run id and run directory, replay exit status, replay output paths, and an embedded replay snapshot with `samples`, `decisions`, `actions`, `peak_ups`, `peak_risk_score`, `peak_mem_full_avg10`, and `peak_vram_used_mb`.
  - `include/hermes/runtime/scenario_config.hpp` and `src/runtime/scenario_config.cpp` now expose `baseline` as the default no-Hermes scenario mode.
  - Added `scripts/smoke_benchmark_hermes.ps1`.
  - The new smoke script builds `hermes_bench`, `hermesd`, and `hermes_replay` with direct `g++`, runs a short four-workload `observe-only` benchmark scenario, verifies the Hermes run directory and replay outputs, and checks that the benchmark summary embeds replay snapshot fields.
  - `scripts/smoke_benchmark_launch.ps1` now uses `runtime_mode: baseline` so it remains a launch-only harness smoke instead of implicitly depending on Hermes binaries.
  - `README.md` now documents the benchmark-plus-Hermes smoke script and clarifies the difference between `baseline` and `observe-only` benchmark scenarios.
  - `roadmap.md` now records benchmark harness Hermes orchestration and partial observe-only benchmark coverage.
  - Running `.\scripts\smoke_benchmark_hermes.ps1 -RunId codex-bench-hermes-20260415c` succeeded.
  - The verified orchestration smoke run recorded `launched=4`, `jobs_completed=3`, `timed_out=1`, `replay_samples=6`, `replay_decisions=6`, `replay_actions=6`, and `replay_valid=true`.
- Decisions made:
  - Split baseline benchmark mode from observe-only mode so benchmark intent is explicit: `baseline` means no Hermes process, while `observe-only` means Hermes is running but not mutating the host.
  - Kept workload launch through the shell, but launched `hermesd` and `hermes_replay` as direct child processes to avoid Windows `cmd /C` quoting edge cases for exact binary-plus-argument execution.
  - Embedded a small replay snapshot directly into the benchmark summary so future comparison tooling can read one artifact without re-opening multiple per-run files first.
- Assumptions still in force:
  - This slice proves bounded Hermes orchestration plus replay capture, not benchmark impact claims.
  - The benchmark harness still does not collect real foreground latency, OOM counts, `strace`, or `perf` evidence.
  - Direct `g++` smoke verification remains the local path until CMake is available in the authoring shell.
- Open risks:
  - The new benchmark Hermes smoke is PowerShell-oriented and assumes `g++` is on PATH.
  - Observe-only benchmark output currently embeds replay peaks and counts, but it does not yet compute comparison tables across baseline versus observe-only versus active-control runs.
  - Active-control benchmark claims still need Linux-native verification with real controllable workloads.
- Next recommended actions:
  - Add comparison-friendly benchmark summary fields for OOM count, completion-rate targets, and degraded-behavior notes derived from generated artifacts.
  - Add a benchmark summary aggregator that reads multiple benchmark JSON files and produces baseline versus observe-only versus active-control comparison tables.
  - Start saving at least one real Linux benchmark bundle with Hermes observe-only mode in the loop.
- Evidence paths / artifacts:
  - `include/hermes/runtime/scenario_config.hpp`
  - `src/runtime/scenario_config.cpp`
  - `src/cli/hermes_bench.cpp`
  - `scripts/smoke_benchmark_launch.ps1`
  - `scripts/smoke_benchmark_hermes.ps1`
  - `README.md`
  - `roadmap.md`
  - `artifacts/bench/codex-bench-hermes-20260415c-summary.json`
  - `artifacts/logs/codex-bench-hermes-20260415c-hermes/replay_summary.json`
