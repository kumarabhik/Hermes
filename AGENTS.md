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

## Context Window Log Rule

After each context window (token budget exhausted or session end), append a
**Context Window Summary** entry to `design.md` under the Session Handoff Log.

Format:

```markdown
#### YYYY-MM-DD IST - Context Window N Summary
- Context window covered: <brief description of what this window was used for>
- Files changed: <list of files modified or created>
- State on exit: <what was completed, what was mid-flight>
- Verified artifacts: <any artifact paths confirmed to exist>
- Assumptions made: <anything inferred rather than directly verified>
- Next window should: <first action for the next session>
```

This is lighter than a full pass entry. Its purpose is to prevent hallucination
across compressed context — always write it before token exhaustion, not after.

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
