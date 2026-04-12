# Hermes Agent Notes

## Project Shape

- Hermes is a C++17 single-host CPU/GPU resource orchestrator.
- Keep implementation claims tied to repo artifacts, benchmark outputs, or source files.
- Treat `design.md` and `roadmap.md` as the source of truth for scope, milestone status, and handoff rules.
- Historical handoff entries are append-only. Add new entries at the end of `design.md`.

## Build And Verification

- Prefer the CMake path when `cmake` is installed:
  - `cmake -S . -B build`
  - `cmake --build build`
- In lightweight shells without CMake, use direct `g++` compilation for smoke checks.
- The daemon is an infinite loop by default. Use `HERMES_MAX_LOOPS=1` or another small value for smoke runs.
- Do not claim native PSI, NVML, perf, eBPF, strace, or GPU benchmark behavior unless those artifacts exist in the repo.

## Artifact Rules

- Runtime and benchmark artifacts belong under `artifacts/`.
- Structured logs should carry `run_id` and `config_hash`.
- Keep generated logs ignored by git; keep `.gitkeep` placeholders tracked.
- Benchmark and profiling claims should link to saved artifacts such as NDJSON logs, summaries, strace captures, perf captures, replay bundles, or gdb notes.

## Coding Rules

- Keep the backend dependency-light and portable enough to compile in Windows/WSL authoring environments.
- Preserve observe-only behavior as the default.
- Add active host mutations only behind explicit operating modes and guardrails.
- Follow the existing module split:
  - `monitor/`
  - `profiler/`
  - `engine/`
  - `actions/`
  - `runtime/`
  - future `cli/`
