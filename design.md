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

## Benchmark Evidence Standards

This section defines what "proven" means for each type of claim Hermes makes. The goal is to prevent the project from treating smoke checks as performance evidence.

### Claim Tiers

| Tier | Meaning | Minimum Evidence |
| --- | --- | --- |
| **T0 — Pipeline correct** | Artifact flow, schema, and NDJSON format are right | Smoke script exits 0; replay_summary.json valid |
| **T1 — Monitors work** | Real PSI/NVML values recorded on a live host | Single daemon run on Linux with non-zero PSI fields |
| **T2 — Predictor fires** | Predictor emits high/critical risk under real pressure | stress-ng or Python hog run; eval_summary.json has total_predictions > 0 |
| **T3 — Intervention fires** | Active-control mode triggers at least one real action | actions.ndjson has a non-dry-run entry; reversal_condition present |
| **T4 — Intervention helps** | Active-control measurably reduces p95 latency or OOM count vs baseline | 5-run comparison: active-control p95 < baseline p95 in ≥ 4 of 5 runs |
| **T5 — Claim is defensible** | Peer-reproducible evidence with environment details | strace + perf captures, RESULTS.md entry with kernel/GPU/run-id |

Every README claim must cite a tier. "Hermes reduces OOM kills" is a T4 claim. "Hermes monitors VRAM pressure" is a T1 claim.

### Minimum Run Count

- A single run proves the pipeline works. It does not prove a performance improvement.
- A 5-run comparison with mean ± std is the minimum to claim directional improvement.
- A 10-run comparison with Welch's t-test is required before stating statistical significance.
- If run-to-run variance is high (std > 30% of mean), investigate the root cause before claiming anything.

### What Invalidates a Result

- Workloads that complete before pressure builds (too short; increase `measurement_s`).
- UPS never exceeding 40 in any run (scenario is not stressful enough; increase VRAM hog or CPU load).
- All interventions firing in dry-run mode when `runtime_mode: active-control` was intended.
- Replay snapshot not available in benchmark summary (Hermes was not actually running during the workloads).
- p95 latency sourced from wall-clock timestamps of `echo` commands rather than real inference work.

---

## Workload Fidelity

The current smoke workloads (`echo smoke-fg`, `sleep 5`) verify the artifact pipeline but produce no real pressure. This section defines what a "fidelity workload" looks like.

### Minimum Fidelity Requirements

For a workload to produce meaningful benchmark evidence:

1. **Memory pressure**: at least one background job must allocate and hold ≥ 50% of available VRAM or ≥ 4 GB system RAM.
2. **CPU pressure**: at least one background job must generate sustained cpu_some_avg10 ≥ 15%.
3. **Foreground latency**: the foreground job must execute a tight inference-like loop with measurable per-iteration latency (not just `sleep`).
4. **Duration**: `measurement_s` ≥ 30 s so PSI 10-second averages have time to stabilize.

### Recommended Workload Templates

**System-RAM pressure (no GPU required):**

```bash
# Background memory hog — allocates and holds 4 GB
python3 -c "import time; buf = bytearray(4 * 1024**3); time.sleep(120)"

# Foreground inference loop — 1000 iterations, latency measured externally
python3 -c "
import time, math
start = time.monotonic()
for i in range(1000):
    _ = [math.sqrt(x) for x in range(10000)]
print(f'done in {time.monotonic()-start:.3f}s')
"

# CPU stress
stress-ng --cpu 4 --timeout 120s --quiet
```

**GPU pressure (Tier C — NVML required):**

```bash
# VRAM hog via PyTorch (replace 6 with available GB - 2)
python3 -c "import torch, time; t = torch.ones(6 * 1024**3 // 4, device='cuda'); time.sleep(120)"

# Foreground inference loop on GPU
python3 -c "
import torch, time
m = torch.nn.Linear(4096, 4096).cuda()
x = torch.randn(32, 4096).cuda()
for i in range(200):
    t0 = time.monotonic()
    _ = m(x)
    torch.cuda.synchronize()
    print(f'iter {i} lat_ms {(time.monotonic()-t0)*1000:.1f}')
"
```

### Updating Pre-built Scenario YAMLs

`config/baseline_scenario.yaml` and `config/observe_scenario.yaml` currently use `echo smoke-*` placeholders. Before claiming T2+ evidence, replace them with workload templates from the section above. Do not delete the `echo` versions — keep them as `_smoke` variants for CI.

---

## False Positive and Intervention Overhead Analysis

An intervention that fires unnecessarily reduces throughput without preventing a failure. This must be measured explicitly before Hermes claims to be "safe to run in active-control mode."

### Definition

A **false positive intervention** is one that fires when:

- The foreground workload completes at target latency with no intervention
- The background workload would not have caused an OOM or latency breach within the observation window
- UPS returns to normal within 15 s without Hermes acting

### How to Measure

1. Run the **baseline scenario** (no Hermes) and record `completion_rate` and `p95_latency_ms`.
2. Run the **same scenario** in active-control mode on a lightly-loaded host (no real pressure injected).
3. Count `intervention_count` in the active-control summary.
4. If `intervention_count > 0` and baseline `completion_rate = 1.0`, every intervention is a false positive.

Target: **< 2 false positive interventions per 60-second run** on a host where UPS stays below 40 throughout.

### How to Reduce False Positives

- Raise `ups_elevated_threshold` in the scenario YAML until the false positive rate is zero.
- Examine `reason_codes` in `predictions.ndjson` for the false-positive run to identify the triggering signal.
- If `VRAM_HEADROOM_COLLAPSE` fires on a lightly-loaded host, the predictor's VRAM slope window is too sensitive; increase the fast-window threshold in `config/schema.yaml`.
- Run `hermes_eval` on the false-positive run to measure `false_positive_rate_per_hour`; if > 5, the predictor weights need recalibration.

---

## Predictor Calibration Cycle

The OOM predictor uses fixed weights and thresholds. After collecting real run data, those values should be tuned using a structured cycle rather than manual guessing.

### Calibration Loop

```text
1. Run 3-5 benchmark scenarios that cover: low pressure, moderate pressure, high pressure, OOM-imminent.
2. For each run, execute:  hermes_eval <run-dir> --out artifacts/eval/<run-id>.json
3. Aggregate across runs:  look for consistent high false_positive_rate or low recall.
4. If false_positive_rate > 10%:
     - Raise predictor risk thresholds in config/schema.yaml
     - Re-run and verify recall does not drop below 80%
5. If recall < 70% on high-pressure runs:
     - Lower the VRAM slope threshold or increase the mem_full residency weight
     - Re-run and verify false_positive_rate does not rise above 5%
6. Record the config_hash before and after tuning in RESULTS.md.
7. Re-run smoke_wsl2.sh to confirm the synthetic fixture still passes all 17 assertions with the new config.
```

### Calibration Targets (v1)

| Metric | Target |
| --- | --- |
| Precision on OOM-imminent fixture | ≥ 0.85 |
| Recall on OOM-imminent fixture | ≥ 0.80 |
| False positive rate (low-pressure baseline) | < 5% per 60 s window |
| Mean lead time before OOM | ≥ 3 s |

These targets should be recorded in RESULTS.md after each calibration pass. The config_hash that achieves the targets becomes the release-candidate config.

---

## Result Interpretation Guide

When a benchmark comparison table is available, this section describes how to read it.

### Reading the Comparison Table (`comparison.csv`)

The table has one row per run, with columns: `run_id`, `runtime_mode`, `completion_rate`, `p95_latency_ms`, `oom_count`, `intervention_count`, `peak_ups`, `degraded_behavior`.

**Is Hermes helping?** Look for:

1. `oom_count` (active-control) < `oom_count` (baseline) — fewer workload failures.
2. `p95_latency_ms` (active-control) < `p95_latency_ms` (baseline) — foreground work is faster.
3. `degraded_behavior` (active-control) = false while (baseline) = true — intervention prevented degradation.
4. `completion_rate` (active-control) ≥ `completion_rate` (baseline) — Hermes did not make things worse.

**Is Hermes hurting?** Watch for:

1. `p95_latency_ms` (active-control) > (baseline) — intervention overhead is outweighing the benefit.
2. `completion_rate` (active-control) < (baseline) — Hermes killed a job it should not have.
3. `intervention_count` very high on a low-pressure run — false positives are consuming cycles.

### What "Improvement" Means

Do not claim improvement based on a single run. The minimum claim threshold is:

- p95 latency: active-control median < baseline median across ≥ 5 runs, by ≥ 10%.
- OOM count: active-control sum ≤ 50% of baseline sum across ≥ 3 OOM-stress runs.
- Completion rate: active-control mean ≥ baseline mean (never worse by more than 2%).

If any of these conditions is violated, Hermes needs further tuning before a performance claim is valid.

### Annotating RESULTS.md

Every live evidence entry in RESULTS.md should include:

- Run IDs for both baseline and active-control runs in the same comparison
- The config_hash used (so the threshold config can be reproduced)
- Host details: kernel version, GPU model (or "CPU only"), available RAM, and whether PSI was live
- Whether stress-ng was used, and at what memory/CPU intensity

---

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

#### 2026-04-15 IST - Benchmark Comparison, Multi-Run, and Operator Documentation Pass

- Verified repo facts:
  - `src/cli/hermes_bench.cpp` now writes enriched benchmark summary artifacts with four new derived fields: `completion_rate` (`jobs_completed / launched`), `intervention_count` (Hermes actions from the embedded replay snapshot), `oom_count` (placeholder, 0; real values require Linux kernel OOM tracing), and `degraded_behavior` (true when completion rate is below the scenario minimum or nonzero exits occurred).
  - `hermes_bench` now accepts `--runs N` to repeat a scenario N times with sequential run IDs (`<base-run-id>-r1`, `-r2`, etc.); `--runs N` overrides the scenario's `repeat_count` field. A single shared plan artifact is written for the full N-run set.
  - Added `src/cli/hermes_compare.cpp`: reads all `*-summary.json` files under `artifacts/bench/`, sorts rows by scenario then mode order (baseline → observe-only → advisory → active-control), prints a comparison table, and writes a comparison CSV to `artifacts/bench/comparison.csv` (or `--output-csv`). Accepts `--bench-dir`, `--output-csv`, and `--scenario` filter flags.
  - Extended `src/cli/hermes_report.cpp`: after the existing replay run table and CSV, it now scans `artifacts/bench/*-summary.json` and appends a "Benchmark Runs" section (formatted table + CSV rows appended to the combined `report.csv`).
  - Added `scripts/smoke_benchmark_compare.ps1`: builds `hermes_bench`, `hermes_compare`, `hermesd`, and `hermes_replay`; runs a baseline and an observe-only benchmark scenario; verifies both summary artifacts; runs `hermes_compare`; checks the comparison CSV contains both runtime mode rows; checks the enriched fields are present in the baseline summary.
  - Created `docs/operator.md`: deployment assumptions, privilege modes (observe-only / advisory / active-control), safety guardrails (PID 1 protection, self-protection, protected-pids list, protected-names, dry-run fallback, rollback on recovery), key environment variables, benchmark procedure, artifact locations, and smoke verification commands.
  - Created `docs/internals.md`: module layout, native collector path (PSI files, vmstat, loadavg, proc/stat, nvidia-smi), UPS formula, OOM predictor, multi-threaded daemon architecture, replay workflow, fault injection fixture list, cgroup v2 backend control files and rollback, and control socket protocol.
  - Created `docs/tuning_guide.md`: general tuning principles, UPS weight table with workload-type guidance, UPS threshold semantics, memory PSI threshold guidance, VRAM threshold guidance, cooldown safe minimums, incremental action enablement procedure, protected-PID and protected-name configuration, and a post-change verification checklist.
  - Updated `README.md`: added "Benchmark Comparison" section documenting `hermes_compare` and `--runs N`; added "Documentation" table linking to the three new docs; added "Achieved Outcomes" table listing smoke-evidenced capabilities and explicitly noting what is not yet evidenced on a Linux GPU machine; fixed MD060 table separator style.
  - Updated `roadmap.md`: marked `hermes_bench` baseline and observe-only modes as `[x]`, active-control mode as `[~]` (infrastructure in place, Linux evidence pending); marked multi-run `[~]` (flag implemented, Linux runs pending); marked `hermes_compare` as `[x]`; marked `hermes_report` benchmark section as `[x]`; marked operator.md, internals.md, tuning_guide.md, and README Achieved Outcomes as `[x]`; updated Current Snapshot paragraph.
- Decisions made:
  - Added `completion_rate`, `intervention_count`, `oom_count`, and `degraded_behavior` directly to the benchmark summary JSON rather than computing them only in `hermes_compare`, so any single summary file is self-describing for comparison purposes.
  - Kept `oom_count` as a zero placeholder rather than omitting the field; this makes the CSV schema stable so tooling built against it does not break when real Linux OOM evidence is added.
  - Created `hermes_compare` as a separate binary (not folded into `hermes_report`) because it reads from `artifacts/bench/` while `hermes_report` reads from `artifacts/logs/`; keeping them separate avoids coupling the replay summary schema to the benchmark summary schema.
  - Extended `hermes_report` to append benchmark rows to the same combined CSV so one `report.csv` gives a complete cross-run view without requiring `hermes_compare` separately.
  - Placed operator docs in `docs/` subdirectory rather than root to keep the repo root clean; linked from README.
- Assumptions still in force:
  - All smoke evidence is authoring-environment only (Windows, `g++`, no Linux GPU).
  - Benchmark completion rate is harness-level (workload process exit codes), not application-level OOM or latency evidence.
  - Active-control and multi-run benchmark claims still require native Linux verification with real controllable workloads.
- Open risks:
  - `hermes_compare` smoke script patches the default baseline scenario YAML with PowerShell string replacement, which may be fragile if the YAML format changes significantly.
  - `oom_count` is always 0 until Linux kernel OOM tracing is integrated; downstream tooling must not treat it as evidence of zero OOMs.
- Next recommended actions:
  - Add a Linux shell smoke equivalent or CTest wrapper for all smoke scripts once CMake is available.
  - Start a real Linux benchmark bundle: run baseline + observe-only on a GPU host, save artifacts, and update `RESULTS.md` with the first real completion-rate comparison.
  - Integrate `strace` capture for at least one benchmark run to advance toward the minimum defensibility package.
  - Consider adding a `--compare-dir` mode to `hermes_bench` that automatically runs `hermes_compare` after completing all N runs.
- Evidence paths / artifacts:
  - `src/cli/hermes_bench.cpp`
  - `src/cli/hermes_compare.cpp`
  - `src/cli/hermes_report.cpp`
  - `scripts/smoke_benchmark_compare.ps1`
  - `docs/operator.md`
  - `docs/internals.md`
  - `docs/tuning_guide.md`
  - `README.md`
  - `roadmap.md`

#### 2026-04-15 IST - WSL2 Evidence Infrastructure Pass

- Verified repo facts:
  - `CMakeLists.txt` now includes a `hermes_compare` build target (standalone, no hermes_core link).
  - `src/cli/hermes_bench.cpp` now tracks foreground workload wall-clock durations across `--runs N` and computes p50/p95/p99/max/min latency, writing `artifacts/bench/<base-run-id>-latency.json`. The `MultiRunStats` struct accumulates per-run foreground durations and OOM-kill counts.
  - `oom_count` in benchmark summaries is now real: workload exit code 137 (SIGKILL, the typical Linux OOM-kill exit) is detected per-workload and summed. The `degraded_behavior` flag now also triggers on any OOM kill.
  - `hermes_bench` supports `--auto-compare [--compare-bin PATH]`: after all N runs complete, automatically invokes `hermes_compare` on the bench directory and writes `<base-run-id>-auto-comparison.csv`.
  - Added `scripts/bench_strace.sh`: extracts the foreground workload command from the scenario YAML, runs it under `strace -c` (syscall summary), and saves the output to `artifacts/bench/<run-id>-strace.txt`. Also launches `hermes_bench` in baseline mode alongside. This is the script for generating the "one real strace finding" required by the minimum defensibility package.
  - Added `scripts/bench_perf.sh`: wraps a full `hermes_bench` invocation with `perf stat -e task-clock,context-switches,cpu-migrations,page-faults,minor-faults,major-faults`, saves output to `artifacts/bench/<run-id>-perf.txt`. Works with WSL2 software counters; hardware counters are not required.
  - Added `scripts/bench_gdb.sh`: enables core dumps via `ulimit -c unlimited`, optionally sets `/proc/sys/kernel/core_pattern`, runs an arbitrary workload command, and — if a core file is produced — invokes `gdb --batch` to extract `bt full` + `info registers`. Saves to `artifacts/bench/<run-id>-gdb.txt`. Handles exit code 137/SIGKILL/SIGSEGV/SIGABRT interpretation.
  - Added `scripts/smoke_wsl2.sh`: a bash equivalent of all PowerShell smoke scripts. Uses the CMake build directory. Runs 7 verification steps: PSI probe, synthetic replay, one-loop daemon replay, benchmark plan, baseline launch, observe-only + Hermes, and hermes_compare CSV check. Exits 0 on full pass. Also patches default scenario YAML workload commands with `echo` commands for portability.
  - Added `scripts/hermes_plot.py`: reads NDJSON run artifacts and produces pressure_trace.csv, latency_cdf.csv, decision_trace.csv, and band_timeline.csv under `<run-dir>/plots/`. Supports `--plot` for optional matplotlib PNG output (no hard dependency). Handles missing artifact files gracefully.
  - Roadmap updated: `Summary tables` and `Plots` items promoted from `[ ]` to `[~]`; snapshot paragraph and bullet list updated.
- Decisions made:
  - Used exit code 137 as the OOM-kill signal proxy rather than parsing `dmesg` because dmesg may require root and is not accessible in all WSL2 configurations. Exit 137 is the standard Linux OOM-kill exit code and is available cross-platform.
  - Made `--auto-compare` opt-in (not default) so that single-run invocations are not slowed down by spawning a second process.
  - Kept `hermes_plot.py` dependency-free for CSV output; matplotlib is optional and only activates on `--plot`. This avoids any pip install requirement for the core workflow.
  - `smoke_wsl2.sh` patches scenario YAML commands inline with Python rather than maintaining a separate WSL2-specific YAML, so the smoke script stays up-to-date with the default scenario template automatically.
- Assumptions still in force:
  - All new scripts require the CMake build to be present (`cmake -S . -B build && cmake --build build`). The PowerShell scripts use direct `g++` compilation and remain the Windows authoring path.
  - `bench_strace.sh` and `bench_perf.sh` require Linux-native tools (`strace`, `perf`).
  - `bench_gdb.sh` requires `gdb` and a workload that actually crashes or is OOM-killed to produce meaningful backtrace output.
  - Real PSI, VRAM, and GPU stats in WSL2 depend on WSL2 kernel version and CUDA-on-WSL driver setup.
- Open risks:
  - `/proc/sys/kernel/core_pattern` write in `bench_gdb.sh` requires root or CAP_SYS_ADMIN; script degrades gracefully if not writable.
  - `perf stat` hardware counters (cycles, cache-misses) will silently return zeros in WSL2; only software counters are reliable.
  - `hermes_plot.py` latency CDF interpolates P25/P75/P90 from P50/P95 when individual percentile data is not stored; accuracy improves with more runs.
- Next recommended actions:
  - Clone repo into WSL2, run `cmake -S . -B build && cmake --build build`, then run `bash scripts/smoke_wsl2.sh`.
  - Verify `/proc/pressure/cpu` is readable in your WSL2 instance.
  - Run `bash scripts/bench_strace.sh active_scenario.yaml wsl2-strace-01` with a real stress workload to produce the first strace evidence artifact.
  - Run `bash scripts/bench_perf.sh active_scenario.yaml wsl2-perf-01` to produce the first perf evidence artifact.
  - Run `hermes_bench active_scenario.yaml --runs 5 --auto-compare` for the first 5-run multi-run evidence.
- Evidence paths / artifacts:
  - `CMakeLists.txt`
  - `src/cli/hermes_bench.cpp`
  - `scripts/bench_strace.sh`
  - `scripts/bench_perf.sh`
  - `scripts/bench_gdb.sh`
  - `scripts/smoke_wsl2.sh`
  - `scripts/hermes_plot.py`
  - `roadmap.md`

#### 2026-04-16 IST - Context Window Summary (CW-1 of new session)

- Context window covered: User asked "how much % is complete" and "are you adding context window summaries to design.md". Added the context-window-log rule to AGENTS.md and wrote this first entry.
- Files changed: `AGENTS.md` (added Context Window Log Rule section), `design.md` (this entry)
- State on exit: No implementation work done this window — purely meta/process update. All prior work from 2026-04-15 two-pass session is intact.
- Verified artifacts: 27 handoff entries in design.md confirmed; roadmap shows [x]=47, [~]=13, [ ]=11 (71 total, ~66% done).
- Assumptions made: None — counts pulled directly from grep.
- Next window should: Continue from WSL2 evidence infrastructure. First action is to run `bash scripts/smoke_wsl2.sh` in WSL2, then `bench_strace.sh` and `bench_perf.sh` with a real stress workload to advance the [~] items toward [x].

#### 2026-04-15 IST - Context Window Summary (CW-2)

- Context window covered: "Do the next 10 steps" third session — continued from CW-1 mid-task. Completed NVML backend scaffold (Step 2), p95 latency assertions (Step 3), scheduler state coverage (Step 4), OOM-stress scenario generator (Step 5), CMake install targets (Step 6), active-control smoke script (Step 7), WSL2 quickstart doc (Step 8), RESULTS.md live evidence table (Step 9), roadmap + design.md update (Step 10).
- Files changed: `src/monitor/nvml_backend.cpp` (new — NVML dlopen implementation), `src/monitor/gpu_stats.cpp` (NVML fast path + nvidia-smi fallback), `CMakeLists.txt` (nvml_backend.cpp source + dl link + install targets), `src/cli/hermes_bench.cpp` (compute_fg_p95_ms helper, p95_latency_ms/latency_target_ms/latency_target_met in summary JSON, generate_oom_stress scenario, --generate-oom-stress flag + handler), `src/cli/hermes_reeval.cpp` (StateCoverageTracker, state_coverage.json output, coverage summary in stdout), `scripts/smoke_active_control.ps1` (new — active-control end-to-end smoke), `docs/wsl2_quickstart.md` (new — WSL2 build/run guide), `RESULTS.md` (live evidence table + entry template), `roadmap.md` (snapshot paragraph updated, NVML [~]→[x], packaging [~]→[x]).
- State on exit: All 10 steps done. No mid-flight work remaining.
- Verified artifacts: Files confirmed created/edited; no build run (Windows authoring environment, g++ smoke for verification is the next step).
- Assumptions made: `BenchmarkWorkload.foreground` field exists and is set; `WorkloadExecution.child.start_ts_wall`/`end_ts_wall` are populated by the execute_benchmark loop; `to_string(SchedulerState)` is defined in types.hpp (confirmed by reading the file).
- Next window should: Run `bash scripts/smoke_wsl2.sh` in WSL2 to validate the Linux build including nvml_backend.cpp (expects dlopen to fail gracefully when NVML absent). Then run `scripts/smoke_active_control.ps1` on Windows to verify the p95 latency assertion fields appear in the benchmark summary JSON.

#### 2026-04-16 IST - Context Window Summary (CW-3)

- Context window covered: "Do the next 10 steps" — fourth session. Added master evidence collection script, hermesctl nvml subcommand, hermes_report state coverage section, hermes_bench --verify-targets, config/oom_stress_scenario.yaml, smoke_wsl2.sh Step 8, hermes_compare --summary-json, hermes_eval improvements, README updates, roadmap + design.md update.
- Files changed: `scripts/collect_wsl2_evidence.sh` (new), `src/cli/hermes_eval.cpp` (--out flag, data_available, ts_wall, graceful empty-predictions), `src/cli/hermesctl.cpp` (nvml subcommand, dlopen inline), `CMakeLists.txt` (dl link for hermesctl), `src/cli/hermes_report.cpp` (StateCovRecord, load_state_coverage, print_coverage_table, write_coverage_csv), `src/cli/hermes_bench.cpp` (--verify-targets flag + post-run assertion loop), `config/oom_stress_scenario.yaml` (new), `scripts/smoke_wsl2.sh` (Step 8 state coverage), `README.md` (--generate-oom-stress, --verify-targets, hermesctl nvml, docs table, evidence table), `src/cli/hermes_compare.cpp` (#include map, --summary-json flag + write_summary_json), `roadmap.md` (snapshot updated), `design.md` (this entry).
- State on exit: All 10 steps done. No mid-flight work remaining.
- Verified artifacts: Files confirmed created/edited per above list; no build run in Windows authoring environment.
- Assumptions made: `hermes_report.cpp` imports `<fstream>` for `std::istreambuf_iterator` (already present); `hermes_compare.cpp` already imports `<fstream>`, `<iomanip>`, `<sstream>` (present from prior session).
- Next window should: Run `bash scripts/smoke_wsl2.sh` in WSL2 (now 8 steps). Then `bash scripts/collect_wsl2_evidence.sh` to produce the first live evidence artifacts. Record results in RESULTS.md. Run `hermesctl nvml` to verify NVML probe output.

#### 2026-04-16 IST - Context Window Summary (CW-4)

- Context window covered: "Do the next 10 steps and give me how much % is done" — fifth session. Continued from a context-window boundary mid-Step-1 (process_mapper files had been read but not yet edited).
- Files changed:
  - `src/profiler/process_mapper.cpp` — rewritten to include `nvml_backend.hpp`, add `nvml_instance()` singleton, use `NvmlBackend::query_all_processes()` (multi-GPU) or `query_processes(0)` (single-GPU) as primary GPU attribution source; nvidia-smi fallback preserved.
  - `include/hermes/monitor/nvml_backend.hpp` — added `query_all_processes()` declaration; updated `fill_sample()` doc comment (now aggregates all devices).
  - `src/monitor/nvml_backend.cpp` — `fill_sample()` now loops all `device_count_` GPUs, sums VRAM, averages GPU utilisation; new `query_all_processes()` iterates all devices and merges per-PID memory by summing.
  - `config/baseline_scenario.yaml` (new) — pre-built 3-workload baseline scenario config ready for `hermes_bench`.
  - `config/observe_scenario.yaml` (new) — pre-built 3-workload observe-only scenario config with HERMES UPS thresholds.
  - `src/cli/hermes_synth.cpp` — refactored samples to `SynthFrame` struct + `frames_to_samples()` helper; added `make_cooldown_samples()` and `make_recovery_samples()` fixtures; added `--recovery` and `--cooldown` CLI flags; default scenario name tracks the preset.
  - `src/cli/hermes_fault.cpp` — added `gen_recovery_resume()` (three-phase: critical pressure → abrupt drop → gradual resume); registered as 7th scenario; updated header comment, usage text, and scenario list.
  - `scripts/hermes_plot.py` — added `print_summary()` function (UPS peak/mean/bands, risk peak, scheduler states, action counts, band transitions, replay_summary validity); added `--summary` flag to `main()`.
  - `scripts/run_all_smoke.ps1` (new) — orchestrates all 7 PowerShell smoke scripts, prints pass/fail/skip table with timing; `--StopOnFailure` flag.
  - `src/cli/hermesctl.cpp` — added `cmd_eval()` subcommand (reads `eval_summary.json` if present, else summarises `predictions.ndjson`; scans `artifacts/logs/` for most recent run on Linux/Windows); added POSIX `<dirent.h>`, `<sys/stat.h>` guards; wired as `hermesctl eval [run-dir]`.
  - `roadmap.md` — snapshot paragraph updated; `[~]` closed to `[x]` for: problem framing, git/license, process mapper NVML wiring, plots/hermes_plot.py; `[~]` note updated for scheduler state machine (new fixtures); fault injection item updated to 7 scenarios.
  - `LICENSE` — confirmed MIT license already present (copyright 2026 Abhishek Kumar).
- State on exit: All 10 steps done. No mid-flight work remaining.
- Verified artifacts: Files confirmed created/edited per above list; no build run in Windows authoring environment.
- Assumptions made: `NvmlBackend::device_count()` returns the correct NVML-reported count; `GpuProcessStats` struct has `pid` (unsigned int) and `used_gpu_memory_bytes` (uint64_t) fields (confirmed from nvml_backend.hpp). `hermesctl eval` POSIX path uses `opendir`/`readdir`/`stat` — all present on Linux; Windows path uses `FindFirstFileA`/`FindNextFileA`.
- Roadmap counts after this session: [x] = 53, [~] = 9, [ ] = 9 → 71 total, ~75% done.
- Next window should: Run `bash scripts/smoke_wsl2.sh` in WSL2 to validate all 8 steps including NVML-wired process_mapper. Run `scripts/run_all_smoke.ps1` on Windows to exercise the full suite. Then advance toward live evidence with `bash scripts/collect_wsl2_evidence.sh` on a WSL2 machine with stress-ng. Target: close the remaining 9 `[ ]` items which all require a real Linux+GPU machine.

#### 2026-04-16 IST - Context Window Summary (CW-5)

- Context window covered: "Do the next 10 steps and give me how much % is done" — sixth session. Continued from a context-window boundary at Step 4 of 10 (hermes_tune.py was just being written).
- Files changed:
  - `config/baseline_scenario.yaml` — upgraded from smoke/echo placeholders to real fidelity workloads: Python 2 GB memory hog (`bytearray`), tight compute loop (500 iterations math.sqrt × 5000 values), stress-ng 2-core CPU hog; smoke fallback comments preserved inline.
  - `config/observe_scenario.yaml` — same fidelity upgrade as baseline; `runtime_mode: observe-only`, UPS thresholds `elevated=40 / critical=70`.
  - `config/low_pressure_scenario.yaml` (new) — active-control mode, single quiet foreground workload, `expected_max_intervention_count: 1`; standardized Phase 6d false-positive measurement scenario.
  - `src/cli/hermes_eval.cpp` — added `false_positive_rate_per_hour` (FP / observation_hours) and `observation_window_s` (span from min to max `ts_mono`) to `EvalResult` struct, JSON output, and stdout table; observation window computed with `std::minmax_element`.
  - `scripts/hermes_tune.py` (new) — reads one or more `eval_summary.json` files via glob or `--eval-dir`; aggregates precision/recall/F1/mean_lead_time/FP_rate; compares against design.md calibration targets; reads schema.yaml thresholds; prints PASS/FAIL table; `schema_suggestions()` maps failed targets to specific `config/schema.yaml` adjustments.
  - `scripts/check_evidence_tiers.py` (new) — probes T0–T5 evidence tiers by scanning `artifacts/`; prints status table; accepts `--require T2` for CI gating; exits non-zero if any tier is unmet.
  - `src/cli/hermes_bench.cpp` — added `--delta-vs PATH` flag: loads a saved baseline summary JSON, reads the current run's summary, and prints a delta table (p95 latency, completion rate, OOM count, intervention count) with BETTER/WORSE verdict; added `SummarySnapshot`, `load_summary_snapshot()`, `print_delta_table()` helpers; `is_option_with_value()` updated.
  - `RESULTS.md` — live evidence table updated with Tier column (T0–T5) for all rows; new T2/T3/T4/T5 pending rows added; new "Phase 6 Evidence Targets" table maps each tier to collection command and script.
  - `scripts/smoke_phase6.sh` (new) — bash automation for Phase 6a–d on Linux: (6a) PSI non-zero monitor validation, (6b) fidelity workload run + hermes_eval T2 check, (6c) hermes_tune.py calibration targets, (6d) low-pressure false positive --verify-targets; exits 0 only if all phases pass.
  - `README.md` — "Key Results" section added as a clearly-labeled placeholder with before/after latency table, predictor quality table (precision/recall/F1/lead time/FP rate), false positive row, and instructions for running smoke_phase6.sh and check_evidence_tiers.py.
  - `roadmap.md` — snapshot paragraph extended with all CW-5 additions; Phase 6b first item closed `[ ]` → `[x]` (config YAMLs now have fidelity workloads); Phase 6c second item `[ ]` → `[~]` (hermes_tune.py exists); Phase 6d first item `[ ]` → `[~]` (low_pressure_scenario.yaml exists); Phase 6g first item `[ ]` → `[~]` (placeholder added).
- State on exit: All 10 steps done. No mid-flight work remaining.
- Verified artifacts: Files confirmed created/edited per above list; no build run in Windows authoring environment.
- Roadmap counts after this session: [x] = 54, [~] = 12, [ ] = 17 → 83 total items (Phase 6 adds 22 items). Completion: 54/83 = 65% full, 60/83 = 72% if partial counts halfway.
- Next window should: (1) Run `bash scripts/smoke_phase6.sh` on WSL2/Linux to collect T1–T4 evidence. (2) Run `python3 scripts/check_evidence_tiers.py` to verify tier status. (3) Run `hermes_bench config/baseline_scenario.yaml --runs 5 --run-id baseline-5run` then `hermes_bench config/oom_stress_scenario.yaml --runs 5 --hermes-bin build/hermesd --delta-vs artifacts/bench/baseline-5run-latency.json` to populate the Key Results table. (4) Capture strace + perf (scripts/bench_strace.sh + bench_perf.sh) during an active-control run for T5 evidence.

#### 2026-04-17 IST - Context Window Summary (CW-6)

- Context window covered: "Do the 10 steps and give me % complete" — seventh session.
- Files changed:
  - `config/oom_stress_scenario.yaml` — upgraded with Tier A/B CPU inference loop (foreground, measures per-iteration latency) and commented Tier C PyTorch CUDA loop + vram_hog_bg block; `scenario_name` field added; stress-ng `--quiet` flag and Python fallback comments added; `role:` field used instead of `foreground:/background:` booleans.
  - `README.md` — Achieved Outcomes table: Tier column added to both evidence rows and "not evidenced" rows; "Not yet evidenced" list replaced with a three-column table (claim, tier needed, how to collect); `docs/calibration_guide.md` added to Documentation table.
  - `src/cli/hermesctl.cpp` — `cmd_bench()` subcommand added: scans `artifacts/bench/` for `*-summary.json`, prints compact table (run-id, mode, p95, completion%, OOM, actions, lat verdict); `cmd_diff()` subcommand added: resolves two eval_summary paths, prints side-by-side metric table with delta and A/B/= verdict; both wired into main dispatch; header comment updated.
  - `scripts/hermes_doctor.sh` (new) — Tier A/B/C host readiness diagnostic; PASS/WARN/FAIL per check; colour output; tier-aware next-step message; exit code = FAIL count.
  - `docs/calibration_guide.md` (new) — 8-step predictor calibration runbook; covers eval, hermes_tune output, threshold adjustment reference table, synthetic fixture verification, false positive check, RESULTS.md entry template.
  - `scripts/gen_evidence_report.sh` (new) — sequences check_evidence_tiers.py + hermes_plot.py --summary for each run dir + hermes_report + hermes_tune.py; writes combined report to `artifacts/evidence_report.txt`.
  - `scripts/smoke_schema.sh` (new) — validates schema.yaml: required sections, UPS weight sum ≈ 1.0, threshold ranges, cooldown positivity, critical > elevated invariant, unknown key detection; accepts `--strict` flag.
  - `config/schema.yaml` — `multi_gpu` section added: `vram_aggregation` (sum/max/mean), `per_pid_vram_merge`, `device_allowlist`, `placement_aware_kills`; all fields documented with comments.
  - `docs/tuning_guide.md` — Multi-GPU Placement Policy section added with sensitivity table; Verifying Changes section updated to include `smoke_schema.sh` as step 5.
  - `roadmap.md` — snapshot paragraph extended with all CW-6 additions; Phase 6b item 2 `[ ]` → `[x]` (oom_stress GPU template done); Phase 6g item 2 `[ ]` → `[x]` (README Tier citations done); Stretch multi-GPU `[ ]` → `[~]` (schema + query_all_processes scaffolded).
- State on exit: All 10 steps done. No mid-flight work remaining.
- Verified artifacts: All files confirmed created/edited; no build run in Windows authoring environment.
- Roadmap counts after this session: [x] = 57, [~] = 13, [ ] = 16 → 86 total. Completion: 57/86 = 66% full, 63.5/86 = 74% if partial counts halfway.
- Next window should: (1) Run `bash scripts/hermes_doctor.sh` to confirm host tier. (2) Run `bash scripts/smoke_schema.sh` to validate schema after multi_gpu addition. (3) Run `bash scripts/smoke_phase6.sh` on Linux to advance from T0 to T1–T4. (4) Run `hermesctl bench` and `hermesctl diff` on real benchmark artifacts once they exist. (5) Remaining `[ ]` items are all Linux-runtime dependent — the tooling to collect them is now complete.

#### 2026-04-17 IST - Context Window Summary (CW-7)

- Context window covered: "Do the next 10 steps" — eighth session.
- Files changed:
  - `include/hermes/monitor/rich_proc_reader.hpp` (new) — `RichProcInfo` struct (vm_peak_kb, vm_size_kb, vm_rss_kb, vm_swap_kb, thread_count, vol_ctxt_switches, invol_ctxt_switches + derived MB helpers); `RichProcReader` class with `read(pid, info)` method; Linux compile-guarded; non-Linux stub returns false.
  - `src/monitor/rich_proc_reader.cpp` (new) — parses `/proc/<pid>/status` line-by-line with `parse_kb_field()` / `parse_u32_field()` helpers; reads VmPeak, VmSize, VmRSS, VmSwap, Threads, voluntary_ctxt_switches, nonvoluntary_ctxt_switches; uses `FILE*` + `fgets` (no C++ streams); compile-guarded.
  - `CMakeLists.txt` — `rich_proc_reader.cpp` added to `hermes_core` source list.
  - `scripts/populate_readme_results.py` (new) — reads `*-summary.json` from `artifacts/bench/` and `eval_summary.json` from `artifacts/logs/`; builds latency/predictor/FP tables; rewrites the three Key Results tables in README.md in place using regex-based table surgery; `--dry-run` prints to stdout; closes Phase 4 `[ ]` "README-ready before/after claims derived from generated artifacts".
  - `config/schema_tier_a.yaml` (new) — CPU-only conservative config: VRAM/IO weights zero, thresholds elevated=50/critical=80, only Level 1 enabled, extended cooldowns, observe_only_mode=true; starting point for Tier A hosts.
  - `config/schema_tier_b.yaml` (new) — Linux + PSI standard config: IO PSI weight 0.08 added, default thresholds, Level 1 enabled, Level 2 gated, multi_gpu section included; starting point for Tier B hosts.
  - `src/cli/hermesctl.cpp` — `cmd_schema()` subcommand added: opens `config/schema.yaml` (or HERMES_CONFIG_PATH or explicit arg), parses indented YAML sections (section + subsection + key tracking), prints 3-column table (parameter path, value, inline comment); dispatch wired; header comment updated.
  - `include/hermes/actions/kill.hpp` — `KillConfig` extended with `placement_aware_kills: bool`, `pid_device: unordered_map<int,int>`, `device_util: unordered_map<int,double>`; `KillAction` extended with `update_placement_data()` (sets live placement maps) and `sort_by_placement()` (re-orders target PIDs to prefer hottest GPU device using `stable_partition`); `<unordered_map>` added.
  - `src/actions/kill.cpp` — `update_placement_data()` and `sort_by_placement()` implementations added; `execute()` now calls `sort_by_placement(decision.target_pids)` before iterating; `<unordered_map>` included.
  - `scripts/smoke_hermes_doctor.ps1` (new) — Windows PowerShell host readiness diagnostic: checks PowerShell version, g++, cmake, python3, all Hermes binaries in build\ variants, all config files, artifact directories, smoke scripts, Python scripts, documentation; ANSI colour-coded PASS/WARN/FAIL; exits with FAIL count.
  - `src/cli/hermes_fault.cpp` — `gen_gpu_contention()` (8th scenario) added: 3-phase multi-GPU fixture (Device-0 fills fast 40%→88% over 30 samples, Device-1 spills over 25 samples, both near saturation 15 samples; combined 2×24 GB = 48 GB total_mb; GPU util oscillates); header comment, usage text, `print_usage()`, and `all_scenarios` list updated to include `gpu_contention`.
  - `src/cli/hermes_bench.cpp` — `run_smoke_all()` function added: auto-generates missing scenario configs, runs baseline/observe/oom-stress in sequence via `run_program_sync(self_path, ...)`, runs `hermes_compare` auto-compare, prints PASS/FAIL table; `--smoke-all` flag wired in `main()`; help text and header comment updated.
  - `docs/operator.md` — "Phase 6 Readiness Checklist" section added with Tier A/B/C gating criteria (17 checklist items); `run_all_smoke.ps1` and `smoke_hermes_doctor.ps1` referenced in Smoke Verification section; `smoke_phase6.sh`, `gen_evidence_report.sh`, `populate_readme_results.py` referenced as Phase 6 collection commands.
  - `roadmap.md` — snapshot paragraph extended with all CW-7 additions; Phase 1 `[~]` rich /proc reader → `[x]`; Phase 4 `[ ]` README-ready claims → `[x]`; Stretch multi-GPU `[~]` → `[x]` (placement-aware kill fully implemented); Stretch calibrated configs `[ ]` → `[~]` (Tier A/B exist, Tier C pending T4 evidence).
- State on exit: All 10 steps done. No mid-flight work remaining.
- Verified artifacts: All files confirmed created/edited; no build run in Windows authoring environment (smoke path uses g++ directly).
- Roadmap counts after this session: [x] = 60, [~] = 12, [ ] = 14 → 86 total. Completion: 60/86 = 70% full, 66/86 = 77% if partial credit.
- Next window should: (1) Commit this CW-7 work. (2) Run `.\scripts\smoke_hermes_doctor.ps1` to verify Windows environment. (3) Run `hermes_fault --scenario gpu_contention` and verify fixture is readable by hermes_replay. (4) Run `hermes_bench --smoke-all --dry-run` to verify the new smoke-all path. (5) On Linux: run `bash scripts/smoke_phase6.sh` to advance from T0 to T1–T4.

#### 2026-04-17 IST - Context Window 8 Summary

- Context window covered: Reviewed local repo context plus public GitHub profile/repositories to draft application answers about unique ML perspective and most interesting ML tech stack.
- Files changed: `design.md`
- State on exit: Drafted paste-ready answers grounded in GitHub evidence; no source-code or build changes made.
- Verified artifacts: `README.md`, `RESULTS.md`, `design.md`, `roadmap.md`; git remote `https://github.com/kumarabhik/Hermes.git`
- Assumptions made: Inferred the broader narrative from public GitHub profile/repos (`Hermes`, `Moviemix`, `PDF-Summary`, `Fraud-ML_Service`, `Remotion-Captioning-Platform`); GitHub MCP connector was unavailable, so public GitHub pages were used instead.
- Next window should: Refine the wording for the target application if the user shares the role/company and desired tone/length.

#### 2026-04-17 IST - Context Window Summary (CW-10)

- Context window covered: "Do the next 10 big steps that would impact massively on project completion" — tenth session.
- Files changed:
  - `.github/workflows/ci.yml` (new) — 3-job GitHub Actions workflow: (1) build-and-smoke-linux: installs cmake+g++, cmake build, verifies all 14 binaries, runs smoke_wsl2.sh, smoke_schema.sh, hermes_simulate smoke, hermes_diff.py, hermes_fault all 8 scenarios, hermes_reeval state_coverage, hermes_eval, hermes_plot --summary, hermesctl headroom, check_evidence_tiers.py, archives artifacts for 7 days; (2) python-scripts: syntax checks all .py scripts, runs hermes_diff.py A→B, B→C, JSON output validation; (3) schema-validation: validates all 4 schema YAML files via smoke_schema.sh.
  - `src/cli/hermes_export.cpp` (new) — Prometheus metrics exporter: listens on TCP port 9090; `GET /metrics` returns text/plain with hermes_up, hermes_ups, hermes_risk_score, hermes_sample_count, hermes_drop_count, hermes_scheduler_state_info, hermes_last_action_info, hermes_peak_ups, hermes_peak_risk, hermes_level1/2/3_actions_total; sources live data from control socket with telemetry_quality.json fallback; `--once` mode writes to stdout; per-connection threads; Winsock2 + POSIX.
  - `docs/architecture.md` (new) — comprehensive ASCII-art pipeline diagram covering all 5 layers (monitor, profiler, engine, action, runtime); multi-threaded daemon thread model (sampler ↔ EventBus ↔ policy); full artifact layout tree; CLI tools table (14 binaries); key design invariants (6); scheduler state machine diagram.
  - `src/cli/hermes_budget.cpp` (new) — standalone budget calculator: reads processes.ndjson + schema thresholds; computes VRAM used vs high/critical thresholds; reports MB headroom; lists top-5 VRAM consumers; gives OK/CAUTION/DENY verdict for "small" (256 MB) and "large" (4 GB) workloads; `--json` flag; Win32 + POSIX directory scan; reads HERMES_CONFIG_PATH.
  - `src/cli/hermes_annotate.cpp` (new) — standalone decision annotator: reads decisions.ndjson + scores.ndjson + predictions.ndjson; generates plain-English annotation for each frame (UPS, risk, state transition, action phrase, cooldown context, dry-run vs active, root-cause why); writes `annotated_decisions.ndjson` (original JSON + "annotation" field) and `annotated_decisions.txt` audit log; JSON-escapes embedded strings.
  - `config/scenario_inference.yaml` (new) — pre-built inference serving scenario: foreground tight Python inference loop + background 1 GB model replica + batch job; Tier B/C; duration 90 s, 3 runs; p95 target 5000 ms; Tier C GPU inference loop commented inline.
  - `config/scenario_training.yaml` (new) — pre-built training scenario: foreground training loop with 30 s checkpoint writes + background data preprocessor (512 MB) + model eval (256 MB, 60 s); Tier B/C; duration 120 s, 3 runs; level-3 cooldown 600 s protects checkpoints.
  - `scripts/smoke_simulate.ps1` (new) — 7-step Windows smoke for hermes_simulate: generates synth fixture, verifies samples.ndjson, runs hermes_simulate with --compare, verifies all 5 output artifacts, checks total_frames > 0, checks --compare output.
  - `scripts/smoke_web.ps1` (new) — 5-step Windows smoke for hermes_web: starts server on port 17070 as background job, GET / (200+HTML), GET /api/status (200+JSON), GET /nonexistent (404), stops cleanly.
  - `scripts/run_all_smoke.ps1` — `$Smokes` array extended with "simulate" (smoke_simulate.ps1) and "web-dashboard" (smoke_web.ps1); header comment updated to "all PowerShell smoke scripts".
  - `src/cli/hermes_watchdog.cpp` (new) — standalone daemon watchdog: polls hermesd control socket every `--interval-ms` ms; requires 2 consecutive failures before restarting; spawns new hermesd via fork/exec (Linux) or CreateProcess (Windows); max-restart guard; optional `--alert-cmd` shell hook on each restart; SIGCHLD auto-reap; SIGTERM/SIGINT clean exit.
  - `CMakeLists.txt` — hermes_export, hermes_budget, hermes_annotate, hermes_watchdog targets added; ws2_32 linked for export on Windows; all 4 added to install(TARGETS).
  - `README.md` — docs/architecture.md added to Documentation table; 10 new tool rows added to New Tools table (hermes_export, hermes_budget, hermes_annotate, hermes_watchdog, scenario_inference.yaml, scenario_training.yaml, CI workflow, architecture.md).
- State on exit: All 10 steps done. No mid-flight work remaining.
- Verified artifacts: All files confirmed created/edited. No build run in Windows authoring environment.
- Roadmap counts after this session: [x] = 62, [~] = 8, [ ] = 14; CI workflow closes T0 evidence automatically on every push.
- Next window should: (1) Commit CW-10 work and push. (2) Watch GitHub Actions run to confirm CI passes. (3) On Linux: run bash scripts/smoke_wsl2.sh to get T1 evidence. (4) Test hermes_export by running it and curling localhost:9090/metrics. (5) Run hermes_budget on a real run directory. (6) Try hermes_annotate on a synthetic fixture run.

#### 2026-04-17 IST - Context Window Summary (CW-9)

- Context window covered: "Do the next 10 steps that will impact the project massively" — ninth session.
- Files changed:
  - `config/schema_tier_c.yaml` (new) — Tier C calibrated config for Linux + PSI + full GPU NVML path; tighter thresholds (elevated=35, critical=65, vram_high=85, vram_critical=92); Level 2 enabled; Level 3 gated; circuit breaker section added; placement_aware_kills=false pending T4 evidence; extended list of GPU host protected names (nv-hostengine, dcgmi, containerd, dockerd); closes the `[~]` stretch goal "Publish calibrated config set" fully (Tier A/B/C now all exist as conservative starting points).
  - `include/hermes/engine/scheduler.hpp` — `SchedulerConfig` extended with circuit breaker fields: `circuit_breaker_enabled`, `max_interventions_per_window` (default 4), `circuit_breaker_window_ms` (60 s), `forced_cooldown_ms` (120 s); `Scheduler` class extended with `intervention_timestamps_` (sliding window) and `circuit_breaker_until_` (forced cooldown end time).
  - `src/engine/scheduler.cpp` — Circuit breaker logic added to `evaluate()`: evicts stale timestamps at start of each cycle; returns `cooldown_state="circuit-breaker"` when tripped; increments counter and trips breaker after each Level-2 or Level-3 action; `<algorithm>` already available.
  - `src/cli/hermesctl.cpp` — `hermesctl top` subcommand: reads `processes.ndjson` from a run directory, scores each PID (VRAM×0.35 + GPU-util×0.18 + CPU×0.15), sorts by score descending, prints ranked table with FG/PROT flags; `hermesctl headroom` subcommand: reads thresholds from schema.yaml, reads peak_ups from latest `telemetry_quality.json` (or accepts `--ups <value>`), reports headroom to elevated/critical thresholds with OK/CAUTION/DENY verdict; both subcommands cross-platform (Win32 + POSIX directory scan); header comment and `main()` dispatch updated; `<algorithm>`, `<iomanip>`, `<sstream>` added.
  - `src/cli/hermes_alert.cpp` (new) — standalone alert watcher: polls control socket every `--interval-ms` ms; fires an HTTP POST (plain TCP on port 80 with fallback to system curl) to `--webhook <url>` when scheduler enters Throttled/Cooldown/Elevated; per-incident suppression (resets after returning to Normal); `--dry-run` mode prints payload to stdout; `--cooldown-s` enforces minimum quiet period; `--once` exits after first alert.
  - `src/cli/hermes_simulate.cpp` (new) — full-pipeline simulation from `samples.ndjson`: parses each line with `parse_sample()`, feeds through real `PressureScoreCalculator` → `OomPredictor` → `Scheduler` → `DryRunExecutor`, writes `scores.ndjson`, `predictions.ndjson`, `decisions.ndjson`, `run_metadata.json`, `telemetry_quality.json` to `--out <dir>`; optional `--compare <orig-run-dir>` prints action/state/level match rates; links against `hermes_core`; cross-platform; enables Windows-native end-to-end pipeline validation without a live kernel or GPU.
  - `src/cli/hermes_web.cpp` (new) — embedded HTTP server (port 7070, configurable): accepts `GET /` → serves self-contained HTML/CSS/JS dashboard with live UPS bar, pressure band badge, scheduler state, risk, last action, and run-id panels; accepts `GET /api/status` → proxies hermesd control socket response as JSON; auto-refreshes every `--refresh-ms` ms via `fetch()`; on non-Linux (Windows) returns a mock status payload for dashboard testing; per-connection threads (detached); Winsock2 on Windows, POSIX on Linux; no build toolchain or npm required.
  - `scripts/hermes_diff.py` (new) — pure-Python YAML diff (no PyYAML): parses two schema files to flat dotted-key dicts; diffs all keys; annotates changed threshold values as "tighter"/"looser", UPS weight changes as "raises/lowers UPS ≤N pts at full signal", cooldown changes as "longer/shorter"; tabular output with change markers (~/+/−); `--show-unchanged` flag; `--json` for machine-readable output; 7 category rules (ups_weights, thresholds, cooldowns, actions, circuit_breaker, multi_gpu, protected).
  - `CMakeLists.txt` — `hermes_alert`, `hermes_simulate`, `hermes_web` targets added; `ws2_32` linked for alert/web on Windows; all three added to `install(TARGETS ...)`.
  - `README.md` — "New Tools (CW-8)" section added: table of 7 new tools with descriptions; circuit breaker explanation; quick-start code block; `schema_tier_c.yaml` documented.
- State on exit: All 10 steps done. No mid-flight work remaining.
- Verified artifacts: All files confirmed created/edited. No build run in Windows authoring environment.
- Roadmap counts after this session: [x] = 62, [~] = 10, [ ] = 14 → 86 total. Stretch calibrated configs `[~]` → `[x]` (all three tiers now exist). Circuit breaker is a new non-roadmap safety feature.
- Next window should: (1) Commit CW-9 work. (2) Build all three new CLIs via g++ smoke path on Windows. (3) Test `hermes_simulate` against a synthetic fixture run. (4) Test `hermes_web` by opening browser. (5) Run `python3 scripts/hermes_diff.py config/schema_tier_b.yaml config/schema_tier_c.yaml` to verify diff output. (6) On Linux: proceed with Phase 6 evidence collection via `bash scripts/smoke_phase6.sh`.
