# Changelog

All notable changes to the Hermes GPU/CPU resource orchestrator are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

---

## [Unreleased] — CW-13

### Added
- **`src/cli/hermes_score.cpp`** — standalone UPS calculator and score explainer.
  Accepts `--cpu/--mem/--io/--gpu/--vram` or `--from-sample <ndjson>` input.
  Outputs a formatted breakdown table or `--json`. Exit codes encode band (0=normal, 1=elevated, 2=critical).
- **`scripts/smoke_all_states.ps1`** — smoke test verifying `hermes_synth --all-states`
  produces samples covering normal / elevated / critical pressure bands and that
  `hermes_replay` summarizes the run without errors.
- **`hermesctl summary`** — one-screen overview of the most recent run directory:
  run_id, host, start time, peak UPS, risk, sample/action counts, and the last 5 events.
- **`CHANGELOG.md`** — this file.
- **`scripts/hermes_coverage.py`** — state-transition coverage matrix.
  Scans all `state_coverage.json` files under `artifacts/logs/` and prints a
  per-run transition matrix plus aggregate coverage across all runs.

### Changed
- `CMakeLists.txt`: added `hermes_score` executable (standalone, no hermes_core link).
- `scripts/run_all_smoke.ps1`: added `all-states` smoke entry.

---

## [0.0.12] — CW-12

### Added
- **`src/cli/hermes_journal.cpp`** — Markdown timeline generator for a run directory.
  Renders band transitions, state transitions, high-risk predictions, and action
  events as a sorted Markdown table. Supports `--stdout` and `--output` flags.
- **`scripts/smoke_pack.ps1`** — smoke test covering hermes_pack bundle creation,
  `--list` dry-run, `--generate-manifest`, and hermes_journal `--stdout`.
- **`scripts/hermes_ci_gate.py`** — Python CI gate for T0–T5 evidence tiers.
  Accepts `--require T0..T5`, `--json`, and `--all` flags. Returns machine-readable
  pass/fail for each tier.
- **`docs/faq.md`** — comprehensive FAQ covering configuration, operations, evidence
  collection, benchmarking, and troubleshooting.
- **`config/scenario_low_memory.yaml`** — CPU+RAM stress scenario (no GPU) for
  testing OOM predictor and L2/L3 action paths on CPU-only hosts.
- **`scripts/hermes_status.sh`** — quick health check script for Linux.
- **`hermesctl history`** — tabular listing of all past runs with peak UPS, actions,
  and validity flag. Cross-platform (WIN32 + POSIX).
- **`hermesctl logs`** — formatted tail of `events.ndjson` with `--tail N` flag.
  Decodes band/state transitions and action events into human-readable lines.
- **`hermes_replay --diff <baseline>`** — side-by-side comparison of two run
  directories showing peak UPS, risk, action counts, and state distribution deltas.
- **`hermes_synth --all-states`** — 16-frame synthetic fixture covering all 5
  scheduler states (Normal → Elevated → Throttled → Cooldown → Recovery → Normal).

### Changed
- `scripts/run_all_smoke.ps1`: added `pack-journal` smoke entry.

---

## [0.0.11] — CW-11

### Added
- **`src/cli/hermes_pack.cpp`** — evidence bundle packager.
  Copies canonical NDJSON artifacts to a named output directory and writes
  `bundle_manifest.json` with per-file hash (FNV-1a), size, and presence flags.
  Supports `--output-dir`, `--list` (dry-run), and multiple run-dir arguments.
- **`scripts/hermes_quickstart.sh`** — Linux T0–T5 setup script.
  Checks build tools, compiles, runs synthetic smoke, daemon smoke, and PSI
  availability check, then prints a T0–T5 evidence checklist.
- **`config/scenario_multimodel.yaml`** — multi-model inference+training scenario:
  inference_server (foreground) + training_job (1.5 GB) + batch_feature_extractor
  + stale_model_replica (512 MB). 120 s, 3 runs, p95 = 8000 ms.
- **`hermesctl watch`** — streaming timestamped status feed (no screen clear).
  Supports `--interval-ms` and `--count N` for bounded output.
- **`hermes_replay --generate-manifest`** — auto-generates `scenario_manifest.json`
  from observed peaks and state distributions, flooring values at 80 % of observed
  so minor noise does not cause future assertion failures.

### Changed
- `CMakeLists.txt`: added `hermes_pack` and `hermes_journal` executables.

---

## [0.0.10] — CW-10

### Added
- **`.github/workflows/ci.yml`** — GitHub Actions CI: build on ubuntu-latest,
  run smoke suite, upload artifacts.
- **`src/cli/hermes_export.cpp`** — Prometheus text-format metrics exporter.
  Serves `/metrics` over HTTP (embedded server, no external dependencies).
- **`docs/architecture.md`** — full system architecture document: Monitor → Profiler
  → Engine → Actions → Runtime pipeline, data flows, and extension points.
- **`src/cli/hermes_budget.cpp`** — VRAM + CPU budget calculator.
  Accepts model parameters and batch size; outputs per-tier memory and compute budget.
- **`src/cli/hermes_annotate.cpp`** — decision audit annotator.
  Merges `decisions.ndjson` with `scores.ndjson` to produce annotated CSV.
- **`src/cli/hermes_watchdog.cpp`** — daemon health watchdog with auto-restart.
  Monitors hermesd via control socket; restarts if heartbeat times out.
- **`config/scenario_inference.yaml`** — baseline inference workload scenario.
- **`config/scenario_training.yaml`** — GPU training workload scenario.
- **`scripts/smoke_*.ps1`** — initial PowerShell smoke suite (8 scripts).

---

## [0.0.9] — CW-9

### Added
- **`src/cli/hermes_web.cpp`** — embedded HTTP server with live browser dashboard.
  Serves auto-refreshing HTML page reading from the control socket.
- **`src/cli/hermes_simulate.cpp`** — full pipeline simulation from saved NDJSON samples.
  Windows-testable; replays samples through pressure_score + scheduler without Linux PSI.
- **`src/cli/hermes_alert.cpp`** — alert watcher.
  Watches control socket and POSTs a webhook on critical state transitions.
- **Circuit breaker** — ≥ 4 L2/L3 actions in 60 s triggers 120 s forced cooldown.
- **`hermesctl top`** — process-level resource view sorted by UPS contribution.
- **`hermesctl headroom`** — shows distance to elevated/critical thresholds.
- **`config/schema_tier_c.yaml`** — Tier-C conservative configuration: lower action
  thresholds and shorter cooldown for latency-sensitive deployments.

---

## [0.0.1 – 0.0.8] — Foundation phases (CW-1 through CW-8)

Core monitor (PSI, loadavg, GPU via NVML), profiler (proc_stat, process_mapper,
workload_classifier), engine (UPS pressure_score, OomPredictor, Scheduler),
actions (reprioritize/throttle/kill with dry-run and active executors), runtime
(hermesd, hermesd_mt, event_logger, run_metadata, telemetry_quality, control_socket,
latency_probe), replay infrastructure (ReplaySummaryBuilder, hermes_replay,
hermes_reeval), benchmark harness (hermes_bench, hermes_report, hermes_compare,
hermes_eval), synthetic fixture generator (hermes_synth), fault injector
(hermes_fault), hermesctl live dashboard.
