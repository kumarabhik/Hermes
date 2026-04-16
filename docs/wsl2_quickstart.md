# Hermes WSL2 Quickstart

This guide covers building and running Hermes under WSL2 (Windows Subsystem for Linux 2).
WSL2 provides a real Linux kernel, so PSI, `/proc`, and most Hermes features work as-is.

---

## Prerequisites

### WSL2 kernel version

```bash
uname -r
# Must be 5.15 or later for PSI support (Ubuntu 22.04 default: 5.15+)
# Ubuntu 24.04 default: 6.8+
```

If the output is below 5.15, upgrade the WSL2 kernel:

```powershell
# In PowerShell (elevated)
wsl --update
wsl --shutdown
```

### Verify PSI is enabled

```bash
cat /proc/pressure/cpu
# Expected: some avg10=... avg60=... avg300=... total=...
# If the file is missing, PSI is not enabled in this kernel build.
```

### Required packages

```bash
sudo apt update
sudo apt install -y build-essential cmake python3 stress-ng
```

Optional (for evidence capture):

```bash
sudo apt install -y strace linux-tools-common linux-tools-$(uname -r)
```

---

## Build

### CMake (recommended)

```bash
cd /path/to/hermes
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel $(nproc)
```

All binaries are placed under `build/`.

### Direct g++ (lightweight smoke)

```bash
g++ -std=c++17 -Iinclude \
    src/cli/hermes_synth.cpp \
    src/runtime/event_logger.cpp \
    -o build/hermes_synth_smoke

g++ -std=c++17 -Iinclude \
    src/runtime/hermesd.cpp \
    src/monitor/cpu_psi.cpp src/monitor/mem_psi.cpp \
    src/monitor/loadavg.cpp src/monitor/gpu_stats.cpp \
    src/monitor/nvml_backend.cpp \
    src/monitor/io_psi.cpp src/monitor/vmstat.cpp \
    src/profiler/proc_stat.cpp src/profiler/process_mapper.cpp \
    src/profiler/workload_classifier.cpp \
    src/engine/pressure_score.cpp src/engine/predictor.cpp \
    src/engine/scheduler.cpp \
    src/actions/dry_run_executor.cpp src/actions/reprioritize.cpp \
    src/actions/throttle.cpp src/actions/kill.cpp \
    src/actions/active_executor.cpp src/actions/cgroup.cpp \
    src/runtime/event_logger.cpp src/runtime/run_metadata.cpp \
    src/runtime/telemetry_quality.cpp src/runtime/scenario_config.cpp \
    src/runtime/control_socket.cpp src/runtime/latency_probe.cpp \
    -o build/hermesd_smoke
```

---

## Run the smoke suite

The `scripts/smoke_wsl2.sh` script runs all 7 verification steps automatically.
It uses the CMake build directory (`build/`) and patches scenario YAML commands
to use `python3` instead of bare `python`.

```bash
bash scripts/smoke_wsl2.sh
# PASS/FAIL per step; exits 0 on full pass
```

Steps verified:

1. PSI files are readable (`/proc/pressure/cpu`, `memory`, `io`)
2. Synthetic fixture replay produces deterministic NDJSON output
3. One-loop daemon produces `samples.ndjson`, `decisions.ndjson`, `run_metadata.json`
4. Benchmark plan artifact generation
5. Baseline benchmark workload launch
6. Observe-only benchmark with Hermes orchestration and replay
7. `hermes_compare` baseline vs observe-only comparison CSV

---

## Observe-only daemon (quick test)

```bash
HERMES_MAX_LOOPS=5 HERMES_RUNTIME_MODE=observe-only \
    ./build/hermesd artifacts/logs/wsl2-test-01
```

Artifacts written under `artifacts/logs/wsl2-test-01/`:

| File | Contents |
| --- | --- |
| `samples.ndjson` | One pressure sample per loop |
| `scores.ndjson` | UPS, band, dominant signals |
| `decisions.ndjson` | Scheduler state + action per loop |
| `events.ndjson` | Band transitions, state changes |
| `run_metadata.json` | Timestamps, config hash, run id |
| `telemetry_quality.json` | PSI availability per signal |

---

## GPU monitoring (NVML fast path)

On WSL2 with CUDA installed, Hermes uses the NVML fast path automatically:

```bash
nvidia-smi  # verify GPU is visible
ls /usr/lib/wsl/lib/libnvidia-ml.so.1  # NVML library location on WSL2
```

If NVML is not available, Hermes falls back to `nvidia-smi` subprocess calls.
The `telemetry_quality.json` artifact records which GPU path was used:

```json
{ "gpu_available": true, "nvml_fast_path": true }
```

To force the nvidia-smi fallback (for testing), rename or hide the NVML library
path — Hermes degrades gracefully.

---

## Active-control mode (Linux only)

Active-control mode uses `setpriority()`, `SIGSTOP`/`SIGCONT`, and optionally
cgroup v2. These are real Linux kernel calls that **mutate host process state**.

```bash
# Active-control: Hermes will reprioritize and throttle background processes.
HERMES_RUNTIME_MODE=active-control HERMES_MAX_LOOPS=10 \
    ./build/hermesd artifacts/logs/wsl2-active-01
```

Read `docs/operator.md` before running in active-control mode — it explains
the guardrails (PID 1 protection, self-protection, protected-pids list) and
rollback behavior.

### Benchmark with active-control

```bash
./build/hermes_bench active_scenario.yaml \
    --artifact-root artifacts \
    --run-id wsl2-active-01 \
    --hermes-bin ./build/hermesd \
    --replay-bin ./build/hermes_replay \
    --runs 3 \
    --auto-compare
```

---

## perf stat (software counters, WSL2-compatible)

Hardware performance counters are not available in WSL2, but software counters
work:

```bash
perf stat -e task-clock,context-switches,cpu-migrations,page-faults \
    ./build/hermesd artifacts/logs/wsl2-perf-01
# HERMES_MAX_LOOPS=20 recommended to produce enough samples
```

`scripts/bench_perf.sh` wraps this around a full benchmark run:

```bash
bash scripts/bench_perf.sh active_scenario.yaml wsl2-perf-01
# Writes artifacts/bench/wsl2-perf-01-perf.txt
```

---

## strace syscall summary

```bash
strace -c -o artifacts/bench/wsl2-strace-01.txt \
    ./build/hermesd artifacts/logs/wsl2-strace-01
```

`scripts/bench_strace.sh` automates this around a benchmark scenario:

```bash
bash scripts/bench_strace.sh active_scenario.yaml wsl2-strace-01
# Writes artifacts/bench/wsl2-strace-01-strace.txt
```

---

## What does NOT work in WSL2

| Feature | Status | Notes |
| --- | --- | --- |
| PSI (`/proc/pressure/`) | Works (kernel ≥ 5.15) | Verify with `cat /proc/pressure/cpu` |
| NVML GPU stats | Works with CUDA WSL driver | Library at `/usr/lib/wsl/lib/` |
| `nvidia-smi` fallback | Works | If CUDA WSL driver installed |
| `perf` software counters | Works | `task-clock`, `context-switches`, `page-faults` |
| `perf` hardware counters | Does not work | `cpu-cycles`, `instructions` return 0 |
| `strace` | Works | Full syscall tracing |
| `gdb` core dumps | Works | Set `ulimit -c unlimited` first |
| eBPF / `bpftrace` | Limited | Requires privileged WSL2 config |
| cgroup v2 mutations | Works | WSL2 uses cgroup v2 by default |
| `SIGSTOP` / `SIGCONT` | Works | Full Linux signal semantics |
| `setpriority` | Works | `nice` values apply normally |

---

## Troubleshooting

**`cat /proc/pressure/cpu` returns "No such file or directory"**
Kernel is below 5.15 or was compiled without `CONFIG_PSI=y`. Run `wsl --update`.

**`nvidia-smi` not found**
Install the CUDA WSL2 driver from NVIDIA. The WSL2-specific package installs
`libnvidia-ml.so.1` under `/usr/lib/wsl/lib/`.

**hermesd exits immediately with no artifacts**
Check `HERMES_MAX_LOOPS` is set for smoke runs. Without it, the daemon runs
indefinitely. Use `HERMES_MAX_LOOPS=1` for a single-loop test.

**`hermes_bench` workload exits nonzero**
In WSL2, `stress-ng` commands are real. If `stress-ng` is not installed, the
workload launch will fail. Install via `apt install stress-ng` or use `python3`
sleep workloads instead (see scenario YAML examples).
