# Hermes Roadmap

## Status Legend

- `[x]` Implemented and evidenced in repo files or benchmark artifacts
- `[~]` Specified, partially scaffolded, or discussed, but not fully implemented
- `[ ]` Not started in repo

## Current Snapshot

Current repo state verified after the first native backend scaffold pass:

- A git repository is initialized.
- Root metadata exists in `README.md`, `.gitignore`, `design.md`, and `roadmap.md`.
- A C++ scaffold exists in `include/`, `src/`, and `config/schema.yaml`.
- An observe-only daemon loop exists in `src/runtime/hermesd.cpp`.
- Native monitor, profiler, scoring, prediction, scheduler, and dry-run action code now exist in the repo.
- A runtime event logger writes per-run NDJSON artifacts for samples, processes, scores, predictions, decisions, actions, and generic events.
- Daemon runs now write `run_metadata.json` and `config_snapshot.yaml` beside the NDJSON stream.
- Daemon runs now write `telemetry_quality.json` with provider availability, loop-health, process-refresh, and control-loop counters.
- Artifact placeholder directories exist for logs, summaries, plots, replay data, and profiling captures.
- A replay summary CLI can validate and summarize a run directory into `replay_summary.json`, including metadata/config snapshot and telemetry-quality artifact presence plus optional `scenario_manifest.json` signal, count, and threshold assertions.
- A synthetic fixture CLI can generate deterministic pressure traces that exercise Level 1, Level 2, Level 3, cooldown, and recovery scheduler paths without live GPU pressure.
- A PowerShell smoke script can build the synthetic fixture/replay tools, generate a fixture run, replay it, and fail on manifest assertion errors.
- Direct compilation was verified with `g++`; `cmake` was not available in this shell, so the CMake path was not exercised here.
- No benchmark run outputs, replay bundles, `strace` captures, `perf` captures, or active-control actions exist yet.

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
- [x] Synthetic replay smoke script exists for local `g++` verification.

## Phase 1: Observability and Attribution

- [x] CPU PSI monitor streams `/proc/pressure/cpu` metrics on a fixed cadence.
- [x] Memory PSI monitor streams `/proc/pressure/memory` metrics on a fixed cadence.
- [~] GPU collector records device utilization, VRAM used/free, and per-process GPU memory through a lightweight `nvidia-smi` query path; direct NVML integration is still pending.
- [~] Process mapper correlates GPU-attributed PIDs with `/proc` metadata into a unified per-process table; the current path is wired through the `nvidia-smi` collector rather than NVML.
- [x] Optional C++ utility parses `/proc/<pid>/stat` and emits compact process state for Hermes ingestion.
- [~] Process attribution can ingest the helper output while preserving the same `ProcessSnapshot` semantics across fast and rich `/proc` readers; only the fast path exists today.
- [~] Native C++ collector daemon runs as a first-class sampling backend; multi-threaded workers are still pending.
- [ ] Native collector uses a bounded ring buffer or queue and IPC to publish snapshots to local control, CLI, and replay consumers.
- [ ] Optional eBPF probes capture run queue latency, page faults, context switches, and futex wait behavior as aligned kernel-trace samples.
- [x] Workload classifier labels processes as training, inference, background, or idle using transparent heuristics.
- [~] Observe-only CLI view renders live host pressure and per-PID state without mutating the system; the current implementation is daemon-console output rather than a dedicated dashboard.

## Phase 2: UPS and Prediction

- [x] UPS normalization logic converts raw CPU, memory, and GPU signals into bounded components.
- [~] UPS emits explainable component contributions, dominant signals, and band transitions; component fields, dominant signals, and persistent score artifacts exist, but explicit band-transition events are still pending.
- [x] Predictor computes VRAM growth slopes, memory pressure trends, and headroom collapse indicators.
- [x] Predictor emits structured risk records with reason codes, lead time, and recommended actions.
- [ ] Offline evaluator measures predictor precision, recall, F1, lead time, and false positive rate from captured runs.

## Phase 3: Intervention Engine

- [x] Scheduler combines UPS, prediction output, workload class, and cooldown state into one policy decision.
- [~] Scheduler state machine implements `NORMAL`, `ELEVATED`, `THROTTLED`, `RECOVERY`, and `COOLDOWN` transitions with explicit event logging; core transitions and state-transition NDJSON events exist, but recovery/resume behavior still needs stress validation.
- [~] Level 1 reprioritization actions exist as structured dry-run decisions; real `nice` mutation and operator warnings are still pending.
- [~] Level 2 throttling actions exist as structured dry-run decisions; pause/resume and optional privileged cgroup controls are still pending.
- [ ] cgroup v2 backend manages `cpu.max`, `memory.high`, `memory.max`, and `cpuset.cpus` for privileged interventions.
- [~] Level 3 hard-control path can terminate an eligible lowest-priority process after policy checks; the current implementation selects a termination candidate in dry-run mode only.
- [~] Dry-run, advisory, and active-control modes all execute through the same decision path; dry-run mode is implemented first and active mutation is still pending.
- [~] Every action emits a structured rationale and result record through NDJSON artifacts; reversal conditions are still pending.

## Phase 4: Benchmark Harness and Evaluation

- [ ] Benchmark harness launches the required workload mix: `2` ML jobs, `1` CPU or memory stressor, and `1` foreground inference path.
- [ ] Baseline mode runs without Hermes and captures comparison artifacts.
- [ ] Observe-only mode captures detection quality without host mutation.
- [ ] Active-intervention mode captures before/after control impact with the same frozen thresholds.
- [ ] Each primary scenario runs at least `5` times, with `10` runs for the headline scenario.
- [ ] Summary tables include scenario, run id, OOM count, p95 latency, peak memory PSI full, peak VRAM usage, intervention counts, jobs completed, and degraded-behavior notes.
- [ ] Plots include time-series pressure traces, foreground latency CDF, and predictor confusion/lead-time summaries.
- [ ] One stressed benchmark run captures a real `strace` example showing blocking or repeated waits.
- [ ] One benchmark run captures a real `perf stat` or `perf top` profile and records at least one concrete observation.
- [ ] Debug traces are timestamp-aligned with PSI, VRAM, UPS, and intervention events.
- [~] Simulation/replay tooling can summarize saved run traces, generate deterministic synthetic pressure fixtures, and assert fixture manifest signal/count/threshold expectations without requiring a live GPU; deterministic scheduler re-execution from saved traces is still pending.
- [ ] Fault-injection suite can trigger VRAM spikes, memory reclaim storms, CPU hogs, process churn, and scheduler oscillation with labeled events.
- [ ] At least one controlled failure is analyzed with `gdb`, including a saved backtrace or core-dump note.
- [ ] Optional eBPF traces are aligned with PSI, VRAM, UPS, and intervention events when kernel tracing is enabled.
- [ ] README-ready before/after claims are derived from generated artifacts rather than manual interpretation.

## Phase 5: Operator UX, Replay, and Documentation

- [ ] Live CLI dashboard exposes UPS, predictor state, per-PID attribution, and recent decisions.
- [~] Replay workflow can inspect saved event and sample logs, verify metadata/config snapshot and telemetry-quality artifact presence, assert synthetic pressure fixture signal/count/threshold manifests, and emit a summary artifact; full decision reconstruction and policy re-execution are still pending.
- [ ] Operator documentation explains deployment assumptions, privilege modes, safety guardrails, and benchmark procedure.
- [ ] Operator documentation explains the native collector path, kernel observability options, replay mode, fault injection, and cgroup backend behavior.
- [ ] README or operator documentation summarizes at least one real `strace` finding and one real `perf` finding with links to evidence artifacts.
- [ ] Minimum defensibility package exists: an initial native C++ collector milestone for `/proc/<pid>/stat` is implemented, one stressed `strace` capture is saved, and one `perf` capture is documented with a concrete observation.
- [ ] Extended defensibility package exists: native collector, replay evidence, and one kernel-observability or `gdb` artifact are present for advanced claims.
- [ ] README summarizes measured outcomes with direct links to supporting artifacts.
- [ ] Incident or tuning guide explains how to adjust weights, thresholds, and protection rules safely.

## Stretch Goals

- [ ] Add I/O PSI to the control model and extend UPS beyond CPU, memory, and GPU.
- [ ] Support multi-GPU attribution and placement-aware scheduling decisions.
- [ ] Add richer cgroup v2 controls such as `memory.high` and CPU quota tuning with rollback.
- [ ] Build a lightweight web dashboard on top of the same event stream used by the CLI.
- [ ] Support benchmark replay comparisons across config versions.

## Roadmap Update Rules

- Only mark `[x]` when a repo artifact or benchmark result exists and can be pointed to directly.
- Use `[~]` when the design is complete or a partial scaffold exists, but the capability is not fully implemented.
- Do not upgrade a checkbox based on conversations, intentions, or TODO comments alone.
- Every status change should be mirrored in the `Session Handoff Log` in `design.md`.
- If a session stops early or approaches token/context exhaustion, append a verified summary to `design.md` before changing roadmap state further.
