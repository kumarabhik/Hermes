# Hermes Roadmap

## Status Legend

- `[x]` Implemented and evidenced in repo files or benchmark artifacts
- `[~]` Specified, partially scaffolded, or discussed, but not fully implemented
- `[ ]` Not started in repo

## Current Snapshot

Current repo state after IO/vmstat monitors, richer predictor, cgroup v2 backend, multi-threaded daemon, control socket, hermesctl, hermes_reeval, hermes_fault, hermes_report, and latency probe pass:

- A git repository is initialized.
- Root metadata exists in `README.md`, `.gitignore`, `design.md`, and `roadmap.md`.
- A C++ scaffold exists in `include/`, `src/`, and `config/schema.yaml`.
- A single-threaded observe-only daemon (`hermesd`) and a multi-threaded daemon (`hermesd_mt`) both exist.
- `hermesd_mt` uses a sampler thread + policy thread connected by `EventBus<T>` bounded ring buffer.
- `HERMES_RUNTIME_MODE` selects observe-only, advisory, or active-control mode for both daemons.
- Native monitors exist for CPU PSI, memory PSI, IO PSI (`/proc/pressure/io`), vmstat (`/proc/vmstat`), GPU stats, and load average.
- IO PSI (`io_some_avg10`, `io_full_avg10`) and vmstat (`pgfault`, `pgmajfault`) are included in `PressureSample` and UPS scoring.
- OomPredictor uses dual-window VRAM slopes (3s fast + 10s medium), per-PID GPU growth tracking, and four sustained pressure residency counters.
- Real Level 1 (`ReprioritizeAction`), Level 2 (`ThrottleAction`), and Level 3 (`KillAction`) executors exist with Linux compile guards.
- Cgroup v2 backend (`CgroupV2Backend`) exists with `cpu.max`, `memory.high`, `cpuset.cpus`, `attach_pid`, and `restore_all()` rollback.
- `ActiveExecutor` dispatches to real executors in active-control mode; falls back to dry-run otherwise.
- Scheduler triggers `resume_all()` on `ThrottleAction` on recovery state transition.
- Every action result includes a `reversal_condition` field in `actions.ndjson`.
- UPS band changes emit `band_transition` events in `events.ndjson`.
- `PressureScoreCalculator` is stateful and tracks band history.
- Unix domain socket control interface (`ControlSocket`) serves live status JSON; path configurable via `HERMES_SOCKET_PATH`.
- `hermesctl` live ANSI terminal dashboard connects via control socket; supports ping, status, live-refresh, and --once/--interval-ms options.
- `hermes_reeval` re-runs saved samples through the real pipeline and computes action/state/band match rates and UPS/risk RMSE.
- `hermes_fault` generates six labeled fault injection sample fixtures (vram_spike, mem_storm, cpu_hog, io_storm, mixed_pressure, oom_imminent).
- `hermes_report` reads all run replay summaries and prints a comparison table and CSV.
- `LatencyProbe` tracks per-loop policy thread latency (p50/p95/p99/max) and writes `latency_summary.json`.
- An offline predictor evaluator (`hermes_eval`) computes precision, recall, F1, and mean lead time from run artifacts.
- A benchmark scenario config loader (`ScenarioConfigLoader`) and harness stub (`hermes_bench`) exist; harness workload launch is pending.
- A runtime event logger writes per-run NDJSON artifacts for samples, processes, scores, predictions, decisions, actions, and generic events.
- Daemon runs write `run_metadata.json`, `config_snapshot.yaml`, and `telemetry_quality.json`.
- A replay summary CLI validates run directories and emits `replay_summary.json` plus per-run `summary.csv` rows under the run directory, `artifacts/replay/`, and `artifacts/summaries/`.
- PowerShell smoke scripts verify both deterministic synthetic replay and one-loop observe-mode daemon artifact generation through the direct `g++` path.
- A synthetic fixture CLI generates deterministic pressure traces covering Level 1-3, cooldown, and recovery paths.
- No benchmark run outputs with real ML jobs, `strace` captures, `perf` captures, eBPF traces, or `gdb` evidence exist yet.

## Phase 0: Project Bootstrap

- [x] Root-level `design.md` defines v1 scope, architecture boundaries, benchmark methodology, and session handoff rules.
- [x] Root-level `roadmap.md` defines status semantics and milestone-oriented progress tracking.
- [x] `design.md` includes hardware-aware assumptions, low-overhead sampling notes, and debugging/profiling strategy.
- [x] `design.md` includes native runtime, eBPF observability, `gdb` failure analysis, simulation/replay, scheduler state machine, fault injection, and cgroup v2 backend sections.
- [~] Problem framing, high-level architecture, and intervention policy are design-complete but not scaffolded in code.
- [~] Metrics taxonomy, benchmark scenarios, and expected result framing are specified but not yet backed by generated artifacts.
- [~] Git repository is initialized with `README.md` and `.gitignore`; license and first commit are still pending.
- [~] C++ project scaffold exists with `include/`, `src/`, `src/runtime/`, `src/cli/`, and `CMakeLists.txt`; daemon, replay, and synthetic fixture executables are scaffolded, while packaging is still pending.
- [x] Config schema exists for UPS weights, thresholds, cooldowns, and action-level enablement.
- [x] Artifact directory layout exists for logs, summaries, plots, replay data, and profiling captures.
- [x] Runtime run metadata and config snapshot artifacts are written for daemon runs.
- [x] Runtime telemetry-quality artifacts are written for daemon runs.
- [x] Synthetic replay and one-loop daemon replay smoke scripts exist for local `g++` verification.

## Phase 1: Observability and Attribution

- [x] CPU PSI monitor streams `/proc/pressure/cpu` metrics on a fixed cadence.
- [x] Memory PSI monitor streams `/proc/pressure/memory` metrics on a fixed cadence.
- [~] GPU collector records device utilization, VRAM used/free, and per-process GPU memory through a lightweight `nvidia-smi` query path; direct NVML integration is still pending.
- [~] Process mapper correlates GPU-attributed PIDs with `/proc` metadata into a unified per-process table; the current path is wired through the `nvidia-smi` collector rather than NVML.
- [x] Optional C++ utility parses `/proc/<pid>/stat` and emits compact process state for Hermes ingestion.
- [~] Process attribution can ingest the helper output while preserving the same `ProcessSnapshot` semantics across fast and rich `/proc` readers; only the fast path exists today.
- [x] Native C++ collector daemon runs as a first-class sampling backend; `hermesd_mt` runs sampler and policy on separate threads connected by `EventBus<T>`.
- [x] A thread-safe bounded MPSC ring buffer (`EventBus<T>`) exists in `include/hermes/runtime/event_bus.hpp`; `hermesd_mt` uses it to decouple sampler and policy threads.
- [x] IO PSI monitor (`IoPsiMonitor`) reads `/proc/pressure/io` and contributes `io_some_avg10` and `io_full_avg10` to `PressureSample`.
- [x] vmstat monitor (`VmstatMonitor`) reads `/proc/vmstat` for `pgfault` and `pgmajfault` per-interval deltas.
- [ ] Optional eBPF probes capture run queue latency, page faults, context switches, and futex wait behavior as aligned kernel-trace samples.
- [x] Workload classifier labels processes as training, inference, background, or idle using transparent heuristics.
- [x] Observe-only CLI view (`hermesctl`) renders live UPS, risk, scheduler state, and drop counts via Unix domain socket control interface; supports live refresh and one-shot modes.

## Phase 2: UPS and Prediction

- [x] UPS normalization logic converts raw CPU, memory, and GPU signals into bounded components.
- [x] UPS emits explainable component contributions, dominant signals, and band transitions; `band_transition` events are now emitted in `events.ndjson` whenever the UPS band changes, including previous band, new band, score, and dominant signals.
- [x] Predictor computes VRAM growth slopes, memory pressure trends, and headroom collapse indicators.
- [x] Predictor emits structured risk records with reason codes, lead time, and recommended actions.
- [x] Offline evaluator (`hermes_eval`) measures predictor precision, recall, F1, mean lead time, true/false positives and false negatives from captured `predictions.ndjson` and `events.ndjson` artifacts; output written to `eval_summary.json`.

## Phase 3: Intervention Engine

- [x] Scheduler combines UPS, prediction output, workload class, and cooldown state into one policy decision.
- [~] Scheduler state machine implements `NORMAL`, `ELEVATED`, `THROTTLED`, `RECOVERY`, and `COOLDOWN` transitions with explicit event logging; core transitions and state-transition NDJSON events exist, but recovery/resume behavior still needs stress validation.
- [x] Level 1 reprioritization action (`ReprioritizeAction`) calls `setpriority()`/`getpriority()` on Linux to raise the nice value of target processes; saves original nice values for rollback; compile-guarded for cross-platform builds.
- [x] Level 2 throttling action (`ThrottleAction`) sends SIGSTOP/SIGCONT to pause and resume background processes; tracks paused PID set; automatically calls `resume_all()` when scheduler enters recovery state; compile-guarded for Linux.
- [x] cgroup v2 backend (`CgroupV2Backend`) manages `cpu.max`, `memory.high`, and `cpuset.cpus`; saves previous values for rollback via `restore_all()`; all operations Linux compile-guarded.
- [x] Level 3 hard-control path (`KillAction`) sends SIGTERM or SIGKILL to terminate an eligible candidate after guardrail checks (PID <= 1, Hermes own PID, protected-pids list, protected name patterns); compile-guarded for Linux.
- [x] Dry-run, advisory, and active-control modes all execute through the same decision path via `ActiveExecutor`; mode selected by `HERMES_RUNTIME_MODE` environment variable; dry-run is default; active mutation is available on Linux.
- [x] Every action emits a structured rationale, result, and `reversal_condition` field through NDJSON artifacts; reversal conditions describe the exact pressure/cycle conditions required before an action should or can be undone.

## Phase 4: Benchmark Harness and Evaluation

- [~] Benchmark harness scaffold (`hermes_bench`) loads and validates scenario YAML configs; can generate default baseline and active-control scenario templates; workload launch and parallel process management are still pending.
- [ ] Baseline mode runs without Hermes and captures comparison artifacts.
- [ ] Observe-only mode captures detection quality without host mutation.
- [ ] Active-intervention mode captures before/after control impact with the same frozen thresholds.
- [ ] Each primary scenario runs at least `5` times, with `10` runs for the headline scenario.
- [ ] Summary tables include scenario, run id, OOM count, p95 latency, peak memory PSI full, peak VRAM usage, intervention counts, jobs completed, and degraded-behavior notes.
- [ ] Plots include time-series pressure traces, foreground latency CDF, and predictor confusion/lead-time summaries.
- [ ] One stressed benchmark run captures a real `strace` example showing blocking or repeated waits.
- [ ] One benchmark run captures a real `perf stat` or `perf top` profile and records at least one concrete observation.
- [ ] Debug traces are timestamp-aligned with PSI, VRAM, UPS, and intervention events.
- [x] Simulation/replay tooling can summarize saved run traces, generate deterministic synthetic pressure fixtures, and assert fixture manifest signal/count/threshold expectations without requiring a live GPU; `hermes_reeval` re-executes samples through the real pipeline and computes action/state/band match rates and RMSE.
- [x] Fault-injection suite (`hermes_fault`) generates labeled NDJSON sample fixtures for six scenarios: vram_spike, mem_storm, cpu_hog, io_storm, mixed_pressure, and oom_imminent.
- [x] Multi-run artifact comparison report (`hermes_report`) reads replay summaries from all run directories and emits a formatted table and CSV.
- [x] Policy loop latency probe (`LatencyProbe`) tracks p50/p95/p99/max iteration time and writes `latency_summary.json` at daemon shutdown.
- [ ] At least one controlled failure is analyzed with `gdb`, including a saved backtrace or core-dump note.
- [ ] Optional eBPF traces are aligned with PSI, VRAM, UPS, and intervention events when kernel tracing is enabled.
- [ ] README-ready before/after claims are derived from generated artifacts rather than manual interpretation.

## Phase 5: Operator UX, Replay, and Documentation

- [x] Live CLI dashboard (`hermesctl`) exposes UPS, risk, scheduler state, last action, and drop counts via Unix domain socket; supports ping, status, live-refresh, and --once modes.
- [x] Replay workflow can inspect saved event and sample logs, verify metadata/config snapshot and telemetry-quality artifact presence, assert synthetic pressure fixture signal/count/threshold manifests, emit JSON/CSV summary artifacts, and re-execute decisions through the real pipeline with match-rate reporting (`hermes_reeval`).
- [ ] Operator documentation explains deployment assumptions, privilege modes, safety guardrails, and benchmark procedure.
- [ ] Operator documentation explains the native collector path, kernel observability options, replay mode, fault injection, and cgroup backend behavior.
- [ ] README or operator documentation summarizes at least one real `strace` finding and one real `perf` finding with links to evidence artifacts.
- [ ] Minimum defensibility package exists: an initial native C++ collector milestone for `/proc/<pid>/stat` is implemented, one stressed `strace` capture is saved, and one `perf` capture is documented with a concrete observation.
- [ ] Extended defensibility package exists: native collector, replay evidence, and one kernel-observability or `gdb` artifact are present for advanced claims.
- [ ] README summarizes measured outcomes with direct links to supporting artifacts.
- [ ] Incident or tuning guide explains how to adjust weights, thresholds, and protection rules safely.

## Stretch Goals

- [x] Add I/O PSI to the control model and extend UPS beyond CPU, memory, and GPU.
- [ ] Support multi-GPU attribution and placement-aware scheduling decisions.
- [x] Add richer cgroup v2 controls such as `memory.high` and CPU quota tuning with rollback.
- [ ] Build a lightweight web dashboard on top of the same event stream used by the CLI.
- [x] Support benchmark replay comparisons across config versions (hermes_report CSV + hermes_reeval RMSE).

## Roadmap Update Rules

- Only mark `[x]` when a repo artifact or benchmark result exists and can be pointed to directly.
- Use `[~]` when the design is complete or a partial scaffold exists, but the capability is not fully implemented.
- Do not upgrade a checkbox based on conversations, intentions, or TODO comments alone.
- Every status change should be mirrored in the `Session Handoff Log` in `design.md`.
- If a session stops early or approaches token/context exhaustion, append a verified summary to `design.md` before changing roadmap state further.
