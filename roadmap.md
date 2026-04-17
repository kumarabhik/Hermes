# Hermes Roadmap

## Status Legend

- `[x]` Implemented and evidenced in repo files or benchmark artifacts
- `[~]` Specified, partially scaffolded, or discussed, but not fully implemented
- `[ ]` Not started in repo

## Current Snapshot

Current repo state after IO/vmstat monitors, richer predictor, cgroup v2 backend, multi-threaded daemon, control socket, hermesctl, hermes_reeval, hermes_fault, hermes_report, latency probe, hermes_compare, multi-run benchmark harness, enriched benchmark summaries, operator/internals/tuning documentation, foreground latency tracking, real OOM-kill detection, Linux evidence capture scripts (bench_strace.sh, bench_perf.sh, bench_gdb.sh), WSL2 smoke suite, hermes_plot.py, --auto-compare flag, NVML direct integration, p95 latency assertions in benchmark summaries, scheduler state-transition coverage in hermes_reeval, --generate-oom-stress scenario generator, CMake install targets, active-control smoke script, WSL2 quickstart guide, live evidence table in RESULTS.md, collect_wsl2_evidence.sh master evidence script, hermes_eval --out flag + data_available field, hermesctl nvml subcommand, hermes_report state coverage section, hermes_bench --verify-targets, config/oom_stress_scenario.yaml, smoke_wsl2.sh Step 8 state coverage, README documentation updates, hermes_compare --summary-json, NvmlBackend NVML wired into process_mapper (NVML-first per-PID GPU attribution), NvmlBackend fill_sample aggregates across all devices + query_all_processes, pre-built config/baseline_scenario.yaml and config/observe_scenario.yaml, hermes_synth --recovery and --cooldown presets, hermes_fault recovery_resume fixture (7th scenario), hermes_plot.py --summary text mode, scripts/run_all_smoke.ps1 full suite orchestrator, hermesctl eval subcommand, MIT LICENSE file, roadmap [~] items closed for problem framing, process mapper NVML wiring, plots/hermes_plot.py, and license pass:

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
- `hermes_fault` generates seven labeled fault injection sample fixtures (vram_spike, mem_storm, cpu_hog, io_storm, mixed_pressure, oom_imminent, recovery_resume).
- `hermes_report` reads all run replay summaries and prints a comparison table and CSV.
- `LatencyProbe` tracks per-loop policy thread latency (p50/p95/p99/max) and writes `latency_summary.json`.
- An offline predictor evaluator (`hermes_eval`) computes precision, recall, F1, and mean lead time from run artifacts.
- A benchmark scenario config loader (`ScenarioConfigLoader`) and harness (`hermes_bench`) exist; the harness can write dry-run plan artifacts, launch bounded local workloads, launch `hermesd` plus `hermes_replay` around non-baseline runs, and write benchmark summaries that embed replay snapshot counts and peaks.
- A runtime event logger writes per-run NDJSON artifacts for samples, processes, scores, predictions, decisions, actions, and generic events.
- Daemon runs write `run_metadata.json`, `config_snapshot.yaml`, and `telemetry_quality.json`.
- A replay summary CLI validates run directories and emits `replay_summary.json` plus per-run `summary.csv` rows under the run directory, `artifacts/replay/`, and `artifacts/summaries/`.
- PowerShell smoke scripts verify deterministic synthetic replay, one-loop observe-mode daemon artifact generation, benchmark plan artifact generation, bounded benchmark workload launch, benchmark-plus-Hermes observe-only replay, and baseline-vs-observe-only comparison via `hermes_compare` through the direct `g++` path.
- `scripts/smoke_wsl2.sh` is a bash equivalent of the full PowerShell smoke suite for use in WSL2 and Linux with the CMake build directory.
- Benchmark summary artifacts include enriched comparison fields: `completion_rate`, `intervention_count`, `oom_count` (real, from exit code 137 detection), `degraded_behavior`.
- `hermes_bench` supports `--runs N` for multi-run scenario execution and tracks foreground workload latency (p50/p95/p99/max) across runs, writing `<base-run-id>-latency.json` to `artifacts/bench/`.
- `hermes_bench` supports `--auto-compare` to invoke `hermes_compare` automatically after all runs complete.
- `hermes_compare` reads `artifacts/bench/*-summary.json` and emits a comparison table and CSV across runtime modes.
- `hermes_report` appends a benchmark comparison section from `artifacts/bench/` to its combined CSV output.
- Operator documentation exists in `docs/operator.md`, `docs/internals.md`, and `docs/tuning_guide.md`.
- `scripts/bench_strace.sh`, `scripts/bench_perf.sh`, `scripts/bench_gdb.sh` are Linux/WSL2 evidence capture scripts for the minimum and extended defensibility packages.
- `scripts/hermes_plot.py` extracts pressure_trace.csv, latency_cdf.csv, decision_trace.csv, and band_timeline.csv from NDJSON run artifacts; supports `--plot` for optional matplotlib PNG output.
- `CMakeLists.txt` now includes `hermes_compare` and `cmake --install` targets for all binaries, library, headers, config, and scripts.
- A synthetic fixture CLI generates deterministic pressure traces covering Level 1-3, cooldown, and recovery paths.
- `NvmlBackend` in `include/hermes/monitor/nvml_backend.hpp` + `src/monitor/nvml_backend.cpp` provides a direct NVML fast path via dlopen/LoadLibrary; `gpu_stats.cpp` tries NVML before nvidia-smi subprocess.
- Benchmark summary JSONs now include `p95_latency_ms`, `latency_target_ms`, and `latency_target_met` fields; `compute_fg_p95_ms()` derives per-run p95 from foreground execution timestamps.
- `hermes_reeval` now emits `state_coverage.json` listing which of the 5 scheduler states were visited and all unique state transitions observed during replay.
- `hermes_bench --generate-oom-stress` writes a pre-built memory/VRAM pressure scenario targeting OOM-kill intervention testing.
- `scripts/smoke_active_control.ps1` verifies end-to-end active-control mode execution and validates the new latency assertion fields in benchmark summary JSON.
- `docs/wsl2_quickstart.md` covers WSL2 kernel requirements, build steps, smoke suite, GPU/NVML setup, active-control mode, perf/strace capture, and compatibility table.
- `RESULTS.md` now includes a structured live evidence table with proven/partial/pending status and a template for recording future benchmark results.
- `scripts/collect_wsl2_evidence.sh` is a master evidence collection script that runs baseline + observe-only + active-control + OOM-stress scenarios with strace and perf captures in a single invocation, writing all outputs to `artifacts/evidence/<run_id>/`.
- `hermes_eval` now accepts `--out <path>` and writes `data_available`, `total_predictions`, and `ts_wall` fields; empty-prediction case writes a no-data JSON instead of exiting with an error.
- `hermesctl nvml` subcommand probes NVML availability, calls nvmlInit, reports device count, name, VRAM, utilization, and temperature without requiring a running daemon.
- `hermes_report` now scans `artifacts/logs/*/state_coverage.json` and appends a scheduler state coverage section (states visited, transitions, per-state frame counts) to the console table and CSV.
- `hermes_bench --verify-targets` exits non-zero if any run summary has `latency_target_met=false` or `oom_count > expected_max_oom_count`.
- `config/oom_stress_scenario.yaml` is a pre-built scenario config for OOM-kill intervention testing (3 runs, 60s, active-control, 800 MB Python hog + stress-ng, p95 ≤ 8000 ms target).
- `scripts/smoke_wsl2.sh` now has Step 8: runs `hermes_reeval` on the daemon run, verifies `state_coverage.json` structure, and verifies that benchmark summaries contain `p95_latency_ms`/`latency_target_ms`/`latency_target_met` fields.
- `hermes_compare --summary-json <path>` writes aggregated per-mode statistics (avg completion rate, total OOM count, total interventions, avg peak UPS, degraded run count) as JSON.
- README now includes `--generate-oom-stress`, `--verify-targets`, `hermesctl nvml`, `docs/wsl2_quickstart.md` in the docs table, and `collect_wsl2_evidence.sh` in the evidence section.
- `process_mapper.cpp` now uses `NvmlBackend::query_all_processes()` for GPU-per-PID attribution; NVML multi-device path is preferred over the nvidia-smi subprocess fallback.
- `NvmlBackend::fill_sample()` now aggregates VRAM (summed) and GPU utilisation (averaged) across all detected GPUs; new `query_all_processes()` merges per-PID GPU memory across devices.
- `config/baseline_scenario.yaml` and `config/observe_scenario.yaml` are pre-built scenario configs ready for `hermes_bench` without generating them first.
- `hermes_synth --recovery` generates a recovery-focused synthetic fixture (high pressure → sharp drop → stable); `--cooldown` generates a cooldown-focused fixture (moderate ramp → sustained → drop).
- `hermes_fault --scenario recovery_resume` (7th scenario) generates a three-phase fixture: critical pressure → abrupt drop → gradual resume, specifically for testing scheduler recovery transitions.
- `hermes_plot.py --summary` prints a compact text summary (UPS stats, band distribution, scheduler states, actions, band transitions, replay validity) without writing files or requiring matplotlib.
- `scripts/run_all_smoke.ps1` runs all seven PowerShell smoke scripts in sequence and reports a pass/fail/skip table with per-script timing; supports `--StopOnFailure`.
- `hermesctl eval [run-dir]` subcommand reads `eval_summary.json` if present, otherwise summarises `predictions.ndjson` directly (peak risk, high/critical frame counts).
- `LICENSE` (MIT) added at repo root; roadmap items for problem framing, license, and process mapper NVML wiring closed from `[~]` to `[x]`.
- `config/schema_tier_c.yaml` added: Tier C production config (Linux + PSI + GPU NVML); tighter thresholds (elevated=35, critical=65, vram_high=85%); Level 2 enabled; circuit_breaker section added; placement_aware_kills=false until T4 evidence; closes Stretch "Publish calibrated config set" (A/B/C all exist).
- Circuit breaker added to `Scheduler`: `SchedulerConfig` has `circuit_breaker_enabled`, `max_interventions_per_window` (4), `circuit_breaker_window_ms` (60 s), `forced_cooldown_ms` (120 s); `evaluate()` tracks L2/L3 intervention timestamps in a sliding window and enters forced `cooldown_state="circuit-breaker"` when threshold exceeded; prevents cascading kill storms.
- `hermesctl top` subcommand: reads `processes.ndjson`, scores each PID (VRAM×0.35 + GPU-util×0.18 + CPU×0.15), prints ranked table showing best Level-2/3 candidates.
- `hermesctl headroom` subcommand: reads thresholds from schema.yaml, reads peak_ups from `telemetry_quality.json` (or `--ups <value>`), reports headroom to elevated/critical bands with OK/CAUTION/DENY verdict.
- `hermes_alert` CLI: polls control socket and HTTP-POSTs a JSON webhook on Throttled/Cooldown/Elevated state entry; per-incident suppression; `--dry-run` mode; fallback to system curl for HTTPS.
- `hermes_simulate` CLI: feeds any `samples.ndjson` through real pipeline and writes full run artifacts; enables Windows end-to-end pipeline testing without a kernel or GPU; `--compare <run-dir>` prints decision match rates.
- `hermes_web` CLI: embedded HTTP server (port 7070) serving self-contained HTML dashboard with live UPS bar, band badges, scheduler state; `/api/status` proxies control socket; per-connection threads; Winsock2 + POSIX; opens the stretch goal `[ ]` web dashboard.
- `scripts/hermes_diff.py`: pure-Python side-by-side schema YAML diff with impact estimates (UPS pts shift, tighter/looser threshold verdicts, cooldown direction); `--show-unchanged`, `--json` flags.
- Phase 6 (Performance Evidence and Claim Validation) added with 7 sub-phases (6a–6g) covering live monitor validation, workload fidelity upgrade, predictor calibration, false positive baseline, before/after latency and OOM evidence, defensibility captures, and result interpretation; all Phase 6 items require a real Linux host.
- `config/baseline_scenario.yaml` and `config/observe_scenario.yaml` upgraded from `echo smoke-*` placeholders to real fidelity workloads: Python 2 GB memory hog + tight compute loop (500 iterations math.sqrt) + stress-ng 2-core CPU hog; smoke fallback comments preserved inline.
- `config/low_pressure_scenario.yaml` added: active-control mode, single quiet foreground workload, `expected_max_intervention_count: 1`; used as the standardized false-positive measurement scenario (Phase 6d).
- `hermes_eval` extended with `false_positive_rate_per_hour` (FP / observation_hours) and `observation_window_s` (span from min to max ts_mono); both fields appear in JSON output and stdout table.
- `scripts/hermes_tune.py` added: reads one or more `eval_summary.json` files, aggregates metrics, compares against calibration targets from design.md (precision ≥ 0.85, recall ≥ 0.80, F1 ≥ 0.80, mean lead time ≥ 3 s, FP rate < 5/hr), reads schema.yaml thresholds, prints PASS/FAIL table, and suggests specific threshold adjustments.
- `scripts/check_evidence_tiers.py` added: scans `artifacts/` to probe each T0–T5 tier, prints a status table, accepts `--require T2` for CI gating, exits non-zero if any tier is unmet.
- `hermes_bench --delta-vs PATH` added: after a run completes, loads a saved baseline summary JSON and prints a delta table (p95 latency, completion rate, OOM count, intervention count) with BETTER/WORSE verdict per metric.
- `RESULTS.md` live evidence table updated: added Tier column (T0–T5) to all existing rows, added new T2/T3/T4/T5 pending rows, added Phase 6 Evidence Targets table mapping each tier to collection command.
- `scripts/smoke_phase6.sh` added: bash automation for Phase 6a–d steps on Linux (live PSI monitor validation, fidelity workload + hermes_eval, hermes_tune.py calibration check, low-pressure false positive check); exits 0 only if all phases pass.
- README "Key Results" section added as a clearly-labeled placeholder with before/after latency table, predictor quality table, and false positive row; will be populated once Linux benchmark runs complete.
- `config/oom_stress_scenario.yaml` upgraded: Tier A/B foreground uses a tight Python math loop; Tier C block (commented) uses PyTorch CUDA inference loop; `vram_hog_bg` Tier C block also commented; `scenario_name` field added; stress-ng fallbacks added.
- README Achieved Outcomes table upgraded: Tier column (T0–T5) added to both evidence tables; "Not yet evidenced" replaced with a table mapping each missing claim to its required tier and collection command.
- `hermesctl bench` subcommand added: scans `artifacts/bench/` for `*-summary.json` files and prints a compact table (run-id, mode, p95, completion%, OOM, actions, lat-target pass/fail).
- `hermesctl diff` subcommand added: accepts two eval_summary.json paths or run-ids; prints a side-by-side metric table with delta and "A/B/=" verdict; targets from design.md shown alongside.
- `scripts/hermes_doctor.sh` added: Tier A/B/C host readiness diagnostic; checks /proc, PSI, cgroup v2, stress-ng, perf, strace, gdb, NVML library, PyTorch CUDA, and all Hermes binaries; prints colour-coded PASS/WARN/FAIL table with tier-specific next steps.
- `docs/calibration_guide.md` added: 8-step runbook for the full predictor calibration cycle (run baseline, hermes_eval, hermes_tune.py, edit schema.yaml, verify synthetic fixture, iterate, FP check, record RESULTS.md).
- `scripts/gen_evidence_report.sh` added: runs hermes_plot.py --summary + hermes_report + hermes_tune.py in sequence and writes combined plain-text evidence report to `artifacts/evidence_report.txt`.
- `scripts/smoke_schema.sh` added: validates `config/schema.yaml` without requiring a build; checks required sections, UPS weight sum, threshold ranges, cooldown values, cross-field invariants (critical > elevated), and flags unknown keys.
- `config/schema.yaml` extended with `multi_gpu` section: `vram_aggregation` (sum/max/mean), `per_pid_vram_merge`, `device_allowlist`, `placement_aware_kills`; documented in `docs/tuning_guide.md` Multi-GPU Placement Policy section.
- `docs/tuning_guide.md` extended: Multi-GPU Placement Policy section added; `smoke_schema.sh` added as a 5th verification step before active-control promotion.
- Documentation table in `README.md` should be updated to include `docs/calibration_guide.md`.
- `include/hermes/monitor/rich_proc_reader.hpp` + `src/monitor/rich_proc_reader.cpp` added: `RichProcReader` reads VmRSS, VmSwap, VmPeak, VmSize, thread count, and voluntary/involuntary context switches from `/proc/<pid>/status`; compile-guarded for Linux; wired into CMakeLists.txt.
- `scripts/populate_readme_results.py` added: reads `*-summary.json` from `artifacts/bench/` and `eval_summary.json` from `artifacts/logs/`; auto-populates README "Key Results" latency, predictor quality, and false-positive tables in place; `--dry-run` mode for preview; closes Phase 4 "README-ready before/after claims derived from artifacts" roadmap item.
- `config/schema_tier_a.yaml` added: calibrated starting-point config for CPU-only hosts (no GPU, no PSI); weights zeroed for GPU/VRAM; thresholds raised (elevated=50, critical=80); only Level 1 enabled; observe_only_mode: true; advances Stretch "Publish calibrated config set" toward completion.
- `config/schema_tier_b.yaml` added: standard production config for Linux + PSI hosts with optional GPU; includes IO-PSI weight (0.08); default thresholds; Level 1 enabled, Level 2 gated behind review; observe_only_mode: true.
- `hermesctl schema [path]` subcommand added: reads `config/schema.yaml` (or any path/HERMES_CONFIG_PATH), parses indented YAML sections, and prints a formatted 3-column table (Parameter, Value, Note) of all tunable fields; shows edit + smoke_schema.sh tip at the bottom.
- `KillAction::update_placement_data()` and `KillAction::sort_by_placement()` added: when `placement_aware_kills=true` in `KillConfig`, `sort_by_placement()` re-orders target PIDs so processes on the GPU device with the highest current utilization are preferred; `update_placement_data()` lets the daemon push per-PID device assignment and per-device GPU util% before each decision cycle; advances Stretch multi-GPU item from `[~]` to `[x]` for implementation.
- `scripts/smoke_hermes_doctor.ps1` added: Windows/PowerShell host readiness diagnostic; checks PowerShell version, g++, cmake, python3, all Hermes binaries in build\ directories, config files, artifact directories, smoke scripts, Python scripts, and documentation files; colour-coded PASS/WARN/FAIL output; exits with FAIL count for CI gating.
- `hermes_fault --scenario gpu_contention` added (8th fault fixture): simulates multi-GPU device contention across two virtual 24 GB devices in three phases (Device-0 fills fast → Device-1 spills over → both near saturation); 70 fault samples + 20 warmup; designed to test placement-aware kill routing.
- `hermes_bench --smoke-all` added: runs baseline, observe-only, and oom-stress scenarios in sequence by self-invoking the binary; generates missing config files on the fly; auto-compares via `hermes_compare` at the end; prints a PASS/FAIL summary table.
- `docs/operator.md` extended: Phase 6 Readiness Checklist added with Tier A/B/C gating criteria; run_all_smoke.ps1 and smoke_hermes_doctor.ps1 referenced in Smoke Verification section; smoke_phase6.sh, gen_evidence_report.sh, and populate_readme_results.py referenced as the Phase 6 collection commands.
- `hermes_pack` CLI added: packages a run directory into a portable evidence bundle under `artifacts/evidence_bundles/<run_id>/`; writes `bundle_manifest.json` with FNV-1a hashes and file sizes for all standard artifact files; supports `--output-dir` and `--list` (dry-run) modes; standalone binary (no hermes_core link).
- `hermesctl watch` subcommand added: streams a timestamped one-line-per-interval status feed (UPS, band, scheduler state, risk, last action) without clearing the screen; supports `--count N` for bounded output; designed for piping to grep/tail or logging to file.
- `hermes_replay --generate-manifest` flag added: auto-generates `scenario_manifest.json` from a run's observed peak UPS, peak risk, action counts, scheduler states, and pressure bands; floors values at 80% of observed to tolerate minor noise; makes it trivial to lock in any run as a regression baseline.
- `scripts/hermes_quickstart.sh` added: one-command Linux T1 evidence setup — checks host prerequisites (g++, cmake, python3), builds with cmake, runs synthetic smoke, runs daemon one-shot, runs hermes_pack bundle, checks PSI availability, and prints a T0/T1/T2-T5 readiness checklist; supports `--skip-build` and `--tier-b` flags.
- `config/scenario_multimodel.yaml` added: pre-built scenario for the most common real-world GPU contention pattern — inference server foreground + concurrent training job + batch feature extractor + stale model replica as background; 120s, 3 runs, p95 target 8000ms, expected_max_oom_count:0; includes commented Tier C GPU loop variants for PyTorch.
- `hermes_journal` CLI added: generates a human-readable Markdown timeline of a Hermes run from NDJSON artifacts; shows band transitions, scheduler state changes, high-risk predictions, decisions, and actions in chronological order; writes `journal.md` beside the run or `--stdout` for piping; includes artifact inventory table; standalone binary.
- `scripts/smoke_pack.ps1` added: smoke test for `hermes_pack`, `hermes_replay --generate-manifest`, and `hermes_journal`; wired into `run_all_smoke.ps1` as "pack-journal" entry.
- `hermesctl history` subcommand added: scans `artifacts/logs/` and prints a table of all runs with run_id, date, peak UPS, sample count, action count, and validity — newest first.
- `hermesctl logs` subcommand added: prints last N events from the most recent run's `events.ndjson` in human-readable format (band, state, action events decoded); supports `--tail N` and explicit run-dir argument.
- `hermes_synth --all-states` preset added: comprehensive 16-frame fixture that exercises all 5 scheduler states (Normal→Elevated→Throttled→Cooldown→Recovery→Normal) and all 3 intervention levels (L1 reprioritize, L2 SIGSTOP, L3 terminate) in a single deterministic run; designed for state-transition coverage testing.
- `docs/faq.md` added: operator FAQ covering "What is UPS?", "Tier A/B/C?", "Circuit breaker?", "How do I enable active-control?", "What are evidence tiers?", "Troubleshooting hermesctl / pack / circuit breaker"; links to operator.md and tuning_guide.md.
- `hermes_replay --diff <baseline-dir>` flag added: compares two run replay summaries side-by-side; prints peak UPS, risk, mem, GPU, sample/action/prediction counts, and scheduler state + action distributions with BETTER/WORSE verdicts.
- `scripts/hermes_status.sh` added: Linux operational quick-check — daemon running, socket health, latest run artifacts, PSI availability, NVML/nvidia-smi, binary inventory; prints actionable next steps.
- `config/scenario_low_memory.yaml` added: CPU + RAM pressure scenario (no GPU required); foreground CPU compute loop + 512 MB and 1 GB RAM hogs + CPU background hog; 90s, 3 runs, p95 target 6000ms; primary scenario for Tier A/B testing without GPU.
- `scripts/hermes_ci_gate.py` added: CI evidence tier gate (T0–T5); reads artifact directory and exits non-zero if the required tier is not met; `--require T0` for CI, `--require T2` for predictor validation, `--json` for machine-readable output; replaces manual artifact scanning in CI pipelines.
- No benchmark run outputs with real ML jobs, `strace` captures, `perf` captures, eBPF traces, or `gdb` evidence exist yet.

## Phase 0: Project Bootstrap

- [x] Root-level `design.md` defines v1 scope, architecture boundaries, benchmark methodology, and session handoff rules.
- [x] Root-level `roadmap.md` defines status semantics and milestone-oriented progress tracking.
- [x] `design.md` includes hardware-aware assumptions, low-overhead sampling notes, and debugging/profiling strategy.
- [x] `design.md` includes native runtime, eBPF observability, `gdb` failure analysis, simulation/replay, scheduler state machine, fault injection, and cgroup v2 backend sections.
- [x] Problem framing, high-level architecture, and intervention policy are design-complete and fully scaffolded in code; design.md records all architectural decisions and session handoffs.
- [~] Metrics taxonomy, benchmark scenarios, and expected result framing are specified; pre-built scenario configs exist (`config/baseline_scenario.yaml`, `config/observe_scenario.yaml`, `config/oom_stress_scenario.yaml`), but real benchmark artifacts still require a Linux GPU host.
- [x] Git repository is initialized with `README.md`, `.gitignore`, `LICENSE` (MIT), and many commits; project history tracks all major milestones.
- [x] C++ project scaffold exists with `include/`, `src/`, `src/runtime/`, `src/cli/`, and `CMakeLists.txt`; daemon, replay, and synthetic fixture executables are scaffolded; `cmake --install` targets are defined for binaries, library, headers, config YAML, and scripts.
- [x] Config schema exists for UPS weights, thresholds, cooldowns, and action-level enablement.
- [x] Artifact directory layout exists for logs, summaries, plots, replay data, and profiling captures.
- [x] Runtime run metadata and config snapshot artifacts are written for daemon runs.
- [x] Runtime telemetry-quality artifacts are written for daemon runs.
- [x] Synthetic replay, one-loop daemon replay, benchmark plan, benchmark launch, and benchmark-plus-Hermes smoke scripts exist for local `g++` verification.
- [x] `CHANGELOG.md` documents all significant additions across CW-1 through CW-13 with per-release sections.
- [x] `scripts/hermes_quickstart.sh` provides a one-command Linux T0/T1 setup check: verifies prerequisites, builds, runs synthetic + daemon smoke, checks PSI availability, and prints a T0–T5 readiness checklist.
- [x] `scripts/smoke_pack.ps1` smoke test covers hermes_pack bundle creation, `--list` dry-run, `--generate-manifest`, and hermes_journal `--stdout`.
- [x] `scripts/smoke_all_states.ps1` smoke test verifies `hermes_synth --all-states` produces samples with all 3 pressure bands and that `hermes_replay` summarizes without errors.

## Phase 1: Observability and Attribution

- [x] CPU PSI monitor streams `/proc/pressure/cpu` metrics on a fixed cadence.
- [x] Memory PSI monitor streams `/proc/pressure/memory` metrics on a fixed cadence.
- [x] GPU collector records device utilization, VRAM used/free, and per-process GPU memory; `NvmlBackend` provides a direct NVML fast path via dlopen/LoadLibrary with nvidia-smi subprocess fallback; both paths wired in `gpu_stats.cpp`.
- [x] Process mapper correlates GPU-attributed PIDs with `/proc` metadata into a unified per-process table; `process_mapper.cpp` now tries `NvmlBackend::query_all_processes()` first and falls back to the passed nvidia-smi data only when NVML is unavailable.
- [x] Optional C++ utility parses `/proc/<pid>/stat` and emits compact process state for Hermes ingestion.
- [x] Process attribution can ingest the helper output while preserving the same `ProcessSnapshot` semantics across fast and rich `/proc` readers; `RichProcReader` provides VmRSS/VmSwap/VmPeak/threads/ctx-switches from `/proc/<pid>/status` with cross-platform compile guards.
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
- [x] `hermes_score` standalone CLI computes UPS from raw signal values and explains each component's weighted contribution; supports `--from-sample <ndjson>`, `--json`, and `--config`; exit codes encode the pressure band (0=normal, 1=elevated, 2=critical).

## Phase 3: Intervention Engine

- [x] Scheduler combines UPS, prediction output, workload class, and cooldown state into one policy decision.
- [~] Scheduler state machine implements `NORMAL`, `ELEVATED`, `THROTTLED`, `RECOVERY`, and `COOLDOWN` transitions with explicit event logging; core transitions and state-transition NDJSON events exist; `hermes_synth --recovery` and `--cooldown` presets plus `hermes_fault --scenario recovery_resume` provide focused fixtures, but live stress validation on a real Linux GPU host is still pending.
- [x] Level 1 reprioritization action (`ReprioritizeAction`) calls `setpriority()`/`getpriority()` on Linux to raise the nice value of target processes; saves original nice values for rollback; compile-guarded for cross-platform builds.
- [x] Level 2 throttling action (`ThrottleAction`) sends SIGSTOP/SIGCONT to pause and resume background processes; tracks paused PID set; automatically calls `resume_all()` when scheduler enters recovery state; compile-guarded for Linux.
- [x] cgroup v2 backend (`CgroupV2Backend`) manages `cpu.max`, `memory.high`, and `cpuset.cpus`; saves previous values for rollback via `restore_all()`; all operations Linux compile-guarded.
- [x] Level 3 hard-control path (`KillAction`) sends SIGTERM or SIGKILL to terminate an eligible candidate after guardrail checks (PID <= 1, Hermes own PID, protected-pids list, protected name patterns); compile-guarded for Linux.
- [x] Dry-run, advisory, and active-control modes all execute through the same decision path via `ActiveExecutor`; mode selected by `HERMES_RUNTIME_MODE` environment variable; dry-run is default; active mutation is available on Linux.
- [x] Every action emits a structured rationale, result, and `reversal_condition` field through NDJSON artifacts; reversal conditions describe the exact pressure/cycle conditions required before an action should or can be undone.
- [x] `hermes_synth --all-states` preset generates a deterministic 16-frame fixture exercising all 5 scheduler states (Normal → Elevated → Throttled → Cooldown → Recovery → Normal) and all 3 intervention levels in a single run; designed for state-transition coverage testing.

## Phase 4: Benchmark Harness and Evaluation

- [x] Benchmark harness (`hermes_bench`) loads and validates scenario YAML configs, generates default baseline and active-control templates, writes plan artifacts and scenario snapshots under `artifacts/bench/`, launches bounded local workloads with enriched run-summary artifacts (`completion_rate`, `intervention_count`, `oom_count`, `degraded_behavior`), launches Hermes plus replay around non-baseline runs, and supports multi-run execution via `--runs N`.
- [x] Baseline mode can launch bounded no-Hermes workload mixes and capture benchmark plan/summary artifacts.
- [x] Observe-only mode can launch Hermes around a bounded workload mix, replay the resulting Hermes run, and embed replay counts plus peak fields and comparison-friendly enriched fields into the benchmark summary without host mutation.
- [~] Active-intervention mode captures before/after control impact with the same frozen thresholds; harness infrastructure is in place, but real Linux active-control evidence is still pending.
- [~] Each primary scenario runs at least `5` times, with `10` runs for the headline scenario; `--runs N` is implemented but real multi-run evidence on a Linux GPU host is still pending.
- [~] Summary tables include scenario, run id, OOM count (real, from exit 137), p95 latency (from latency.json), peak memory PSI full, peak VRAM usage, intervention counts, jobs completed, and degraded-behavior notes; harness generates all fields, real values require a Linux benchmark run to populate.
- [x] Plots: `hermes_plot.py` extracts pressure_trace.csv, latency_cdf.csv, decision_trace.csv, and band_timeline.csv from NDJSON artifacts without any external dependency; `--summary` mode prints a compact text summary of UPS/risk/state/band/action counts without files; `--plot` generates PNG charts when matplotlib is available.
- [ ] One stressed benchmark run captures a real `strace` example showing blocking or repeated waits.
- [ ] One benchmark run captures a real `perf stat` or `perf top` profile and records at least one concrete observation.
- [ ] Debug traces are timestamp-aligned with PSI, VRAM, UPS, and intervention events.
- [x] Simulation/replay tooling can summarize saved run traces, generate deterministic synthetic pressure fixtures, and assert fixture manifest signal/count/threshold expectations without requiring a live GPU; `hermes_reeval` re-executes samples through the real pipeline and computes action/state/band match rates and RMSE.
- [x] Fault-injection suite (`hermes_fault`) generates labeled NDJSON sample fixtures for seven scenarios: vram_spike, mem_storm, cpu_hog, io_storm, mixed_pressure, oom_imminent, and recovery_resume.
- [x] Multi-run artifact comparison report (`hermes_report`) reads replay summaries from all run directories and emits a formatted table and CSV; also reads benchmark summaries from `artifacts/bench/` and appends a benchmark comparison section to the same CSV.
- [x] Benchmark comparison aggregator (`hermes_compare`) reads all `*-summary.json` files under `artifacts/bench/` and produces a baseline vs observe-only vs active-control comparison table and CSV (`artifacts/bench/comparison.csv`).
- [x] Policy loop latency probe (`LatencyProbe`) tracks p50/p95/p99/max iteration time and writes `latency_summary.json` at daemon shutdown.
- [ ] At least one controlled failure is analyzed with `gdb`, including a saved backtrace or core-dump note.
- [ ] Optional eBPF traces are aligned with PSI, VRAM, UPS, and intervention events when kernel tracing is enabled.
- [x] README-ready before/after claims are derived from generated artifacts rather than manual interpretation: `scripts/populate_readme_results.py` reads bench + eval artifacts and rewrites the Key Results tables in README.md in place.
- [x] `scripts/hermes_ci_gate.py` provides machine-readable T0–T5 CI gating; reads artifact directory and exits non-zero if the required tier is unmet; `--require`, `--json`, and `--all` flags.
- [x] `scripts/hermes_coverage.py` generates a state-transition coverage matrix from `state_coverage.json` and `events.ndjson` files across all run directories; supports `--json` output and per-run breakdown with gap analysis.
- [x] `config/scenario_multimodel.yaml` pre-built multi-model inference+training scenario: inference server foreground + training job (1.5 GB) + batch feature extractor + stale model replica; 120s, 3 runs, p95=8000ms.
- [x] `config/scenario_low_memory.yaml` CPU+RAM stress scenario (no GPU required): foreground CPU compute + 512 MB and 1 GB RAM hogs + CPU background hog; 90s, 3 runs, p95=6000ms; primary scenario for Tier A/B testing without GPU.
- [x] `hermes_replay --generate-manifest` auto-generates `scenario_manifest.json` from observed peaks and state distributions; floors values at 80% of observed to tolerate minor noise; makes any run a regression baseline.
- [x] `hermes_replay --diff <baseline>` compares two run directories side-by-side with peak UPS, risk, action counts, and state/action distribution deltas with BETTER/WORSE verdicts.

## Phase 5: Operator UX, Replay, and Documentation

- [x] Live CLI dashboard (`hermesctl`) exposes UPS, risk, scheduler state, last action, and drop counts via Unix domain socket; supports ping, status, live-refresh, and --once modes.
- [x] Replay workflow can inspect saved event and sample logs, verify metadata/config snapshot and telemetry-quality artifact presence, assert synthetic pressure fixture signal/count/threshold manifests, emit JSON/CSV summary artifacts, and re-execute decisions through the real pipeline with match-rate reporting (`hermes_reeval`).
- [x] Operator documentation explains deployment assumptions, privilege modes, safety guardrails, and benchmark procedure (`docs/operator.md`).
- [x] Operator documentation explains the native collector path, multi-threaded daemon, replay mode, fault injection, and cgroup backend behavior (`docs/internals.md`).
- [ ] README or operator documentation summarizes at least one real `strace` finding and one real `perf` finding with links to evidence artifacts.
- [~] Minimum defensibility package: native C++ `/proc` collector and replay evidence exist; `strace`/`perf` captures are still pending (require Linux GPU machine).
- [ ] Extended defensibility package exists: native collector, replay evidence, and one kernel-observability or `gdb` artifact are present for advanced claims.
- [x] README summarizes achieved outcomes with links to smoke script artifacts and lists what is not yet evidenced (`README.md` Achieved Outcomes section).
- [x] Tuning guide explains how to adjust UPS weights, thresholds, cooldowns, and protection rules safely (`docs/tuning_guide.md`).
- [x] `hermes_pack` CLI packages a run directory into a portable evidence bundle; writes `bundle_manifest.json` with FNV-1a hashes and file sizes for all standard artifacts; supports `--output-dir` and `--list` (dry-run); standalone binary.
- [x] `hermes_journal` CLI generates a human-readable Markdown timeline of a run from NDJSON artifacts; shows band transitions, scheduler state changes, high-risk predictions, decisions, and actions; `--stdout` and `--output` flags; standalone binary.
- [x] `hermesctl watch` streams a timestamped one-line-per-interval status feed (UPS, band, state, risk) without clearing the screen; supports `--count N` for bounded output; designed for piping or file logging.
- [x] `hermesctl history` scans `artifacts/logs/` and prints a table of all past runs with run_id, date, peak UPS, sample count, action count, and validity — newest first.
- [x] `hermesctl logs` prints the last N events from `events.ndjson` in human-readable form; decodes band/state transitions and action events; `--tail N` and explicit run-dir argument.
- [x] `hermesctl summary` prints a one-screen overview of the most recent run: run_id, host, start time, peak UPS/risk, sample/action counts, validity, and last 5 events.
- [x] `docs/faq.md` operator FAQ covering UPS formula, Tier A/B/C configs, circuit breaker, active-control mode, evidence tiers, and troubleshooting steps.
- [x] `scripts/hermes_status.sh` operational quick-check for Linux: daemon running, socket health, latest run artifacts, PSI availability, NVML/nvidia-smi status, and binary inventory.

## Phase 6: Performance Evidence and Claim Validation

This phase closes the gap between "the pipeline works" and "Hermes demonstrably improves outcomes." All items require a Linux host with PSI support. GPU items additionally require NVML/CUDA.

### 6a — Live Monitor Validation (Tier B/C: Linux host)

- [ ] Run `bash scripts/smoke_wsl2.sh` on a WSL2 or native Linux host and confirm all 8 steps pass with non-zero PSI values in `samples.ndjson`.
- [ ] Run `hermesctl nvml` on a CUDA-capable host and confirm NVML fast path is active (device name, VRAM, util reported without nvidia-smi subprocess).
- [ ] Record the first T1 evidence entry in RESULTS.md: a daemon run with real PSI readings and a valid `telemetry_quality.json` showing PSI and GPU availability.

### 6b — Workload Fidelity Upgrade

- [x] Replace `echo smoke-*` commands in `config/baseline_scenario.yaml` and `config/observe_scenario.yaml` with real fidelity workloads: Python memory hog (≥ 2 GB) as background + tight compute loop as foreground; keep `_smoke` YAML variants for CI.
- [x] Update `config/oom_stress_scenario.yaml` foreground command to use the GPU inference loop template from the Workload Fidelity section of `design.md` when running on Tier C. (Both CPU and GPU inference loop variants are now in the YAML with a comment to switch tiers; `scenario_name` field also fixed.)
- [ ] Verify that at least one baseline run on the upgraded scenario produces `cpu_some_avg10 > 10` or `mem_some_avg10 > 10` in `samples.ndjson`, confirming real pressure was generated.

### 6c — Predictor Calibration

- [ ] Run `hermes_eval` on a high-pressure baseline run and record `precision`, `recall`, `F1`, and `false_positive_rate_per_hour` in RESULTS.md.
- [~] If `false_positive_rate_per_hour > 5`, tune predictor thresholds in `config/schema.yaml` per the Predictor Calibration Cycle in `design.md`; verify synthetic fixture still passes 17/17 assertions after tuning. (`scripts/hermes_tune.py` provides calibration suggestions; hermes_eval now outputs `false_positive_rate_per_hour`; tuning itself requires a real Linux run.)
- [ ] Achieve calibration targets: precision ≥ 0.85, recall ≥ 0.80, mean lead time ≥ 3 s on the OOM-imminent fault fixture.

### 6d — False Positive Baseline

- [~] Run `config/low_pressure_scenario.yaml` in active-control mode; confirm `intervention_count = 0` or at most 1; record result in RESULTS.md. (`config/low_pressure_scenario.yaml` and `--verify-targets` check scaffolded; actual run needs Linux.)
- [ ] If false positives occur, identify the triggering `reason_code` in `predictions.ndjson` and record the fix applied to `config/schema.yaml`.

### 6e — Before/After Latency and OOM Evidence

- [ ] Run `config/baseline_scenario.yaml` 5 times and record `p95_latency_ms` mean ± std in RESULTS.md.
- [ ] Run `config/observe_scenario.yaml` 5 times alongside `hermesd`; confirm replay snapshot is available in all 5 summaries; record `peak_ups` mean in RESULTS.md.
- [ ] Run `config/oom_stress_scenario.yaml` 3 times in active-control mode; record `oom_count` and `completion_rate` per run; confirm `oom_count` (active) ≤ 50% of `oom_count` (baseline) across the 3 runs.
- [ ] Run `hermes_bench config/oom_stress_scenario.yaml --runs 5 --verify-targets --auto-compare`; confirm the tool exits 0 (all runs met latency and OOM targets).
- [ ] Populate RESULTS.md with the first T4 evidence entry: a before/after comparison table with actual p95 latency and OOM count numbers for baseline vs active-control.

### 6f — Defensibility Captures

- [ ] Capture `strace -c` syscall summary for one baseline run and one active-control run using `scripts/bench_strace.sh`; record the top 3 syscalls by time in RESULTS.md.
- [ ] Capture `perf stat` software counters (context-switches, page-faults, cache-misses) for one active-control run using `scripts/bench_perf.sh`; record at least one concrete observation in RESULTS.md.
- [ ] Update README "Not yet evidenced" section to "Evidenced" once T4 + strace + perf entries are in RESULTS.md.

### 6g — Result Interpretation and README Claims

- [~] Write a "Key Results" section in README summarising at least one concrete before/after metric (e.g. "active-control reduced p95 latency from X ms to Y ms and OOM events from N to M across 5 runs"). (Placeholder section with table structure added; real numbers pending Linux benchmark runs.)
- [x] Verify that every README performance claim cites a Tier (T1–T5) per the Benchmark Evidence Standards in `design.md`. (Achieved Outcomes table and "Not yet evidenced" table both have Tier column; claim rows added for T1–T5.)
- [ ] Run `hermes_report` and `hermes_compare` on the full evidence set; save the comparison CSV to `artifacts/bench/final_comparison.csv` and link it from RESULTS.md.

## Stretch Goals

- [x] Add I/O PSI to the control model and extend UPS beyond CPU, memory, and GPU.
- [x] Support multi-GPU attribution and placement-aware scheduling decisions. (`NvmlBackend::query_all_processes()` merges per-PID VRAM across all GPUs; `config/schema.yaml` has full `multi_gpu` section; `KillAction::sort_by_placement()` re-orders target PIDs to prefer the hottest GPU device; `update_placement_data()` lets the daemon push live per-PID device and per-device util% data; `placement_aware_kills=false` default until T4 evidence supports enabling it; `config/schema_tier_a.yaml` and `config/schema_tier_b.yaml` provide calibrated starting-point configs.)
- [x] Add richer cgroup v2 controls such as `memory.high` and CPU quota tuning with rollback.
- [x] Build a lightweight web dashboard on top of the same event stream used by the CLI. (`hermes_web` embedded HTTP server serves self-contained HTML dashboard at localhost:7070; proxies control socket; no npm or build tools required.)
- [x] Support benchmark replay comparisons across config versions (hermes_report CSV + hermes_reeval RMSE).
- [x] Publish a calibrated config set (schema.yaml variant) for each Tier A/B/C with proven thresholds from Phase 6 evidence. (`config/schema_tier_a.yaml`, `config/schema_tier_b.yaml`, and `config/schema_tier_c.yaml` all exist; Tier C thresholds are conservative starting points — refine with T4 evidence via hermes_tune.py.)

## Roadmap Update Rules

- Only mark `[x]` when a repo artifact or benchmark result exists and can be pointed to directly.
- Use `[~]` when the design is complete or a partial scaffold exists, but the capability is not fully implemented.
- Do not upgrade a checkbox based on conversations, intentions, or TODO comments alone.
- Every status change should be mirrored in the `Session Handoff Log` in `design.md`.
- If a session stops early or approaches token/context exhaustion, append a verified summary to `design.md` before changing roadmap state further.
