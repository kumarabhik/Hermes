cmake_minimum_required(VERSION 3.14)
project(hermes VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Includes
include_directories(${CMAKE_SOURCE_DIR}/include)

# Hermes Core Backend Library
add_library(hermes_core
    src/monitor/cpu_psi.cpp
    src/monitor/mem_psi.cpp
    src/monitor/loadavg.cpp
    src/monitor/gpu_stats.cpp
    src/monitor/nvml_backend.cpp
    src/monitor/io_psi.cpp
    src/monitor/vmstat.cpp
    src/profiler/proc_stat.cpp
    src/profiler/process_mapper.cpp
    src/profiler/workload_classifier.cpp
    src/engine/pressure_score.cpp
    src/engine/predictor.cpp
    src/engine/scheduler.cpp
    src/actions/dry_run_executor.cpp
    src/actions/reprioritize.cpp
    src/actions/throttle.cpp
    src/actions/kill.cpp
    src/actions/active_executor.cpp
    src/actions/cgroup.cpp
    src/runtime/event_logger.cpp
    src/runtime/run_metadata.cpp
    src/runtime/telemetry_quality.cpp
    src/runtime/scenario_config.cpp
    src/runtime/control_socket.cpp
    src/runtime/latency_probe.cpp
    src/replay/replay_summary.cpp
)
target_include_directories(hermes_core PUBLIC include)

# Link pthreads on Linux for event_bus (header-only but mutex/cv require it).
# Link dl for dlopen used by NvmlBackend runtime loading.
if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(hermes_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()

# Main Daemon Executable
add_executable(hermesd src/runtime/hermesd.cpp)
target_link_libraries(hermesd PRIVATE hermes_core)

# Multi-threaded Daemon Executable
add_executable(hermesd_mt src/runtime/hermesd_mt.cpp)
target_link_libraries(hermesd_mt PRIVATE hermes_core)

# Live Control Dashboard CLI
add_executable(hermesctl src/cli/hermesctl.cpp)
if(UNIX)
    target_link_libraries(hermesctl PRIVATE ${CMAKE_DL_LIBS})
endif()

# Replay Summary CLI
add_executable(hermes_replay src/cli/hermes_replay.cpp)
target_link_libraries(hermes_replay PRIVATE hermes_core)

# Deterministic Replay Re-execution CLI
add_executable(hermes_reeval src/cli/hermes_reeval.cpp)
target_link_libraries(hermes_reeval PRIVATE hermes_core)

# Synthetic Fixture Generator CLI
add_executable(hermes_synth src/cli/hermes_synth.cpp)
target_link_libraries(hermes_synth PRIVATE hermes_core)

# Fault Injection Fixture Generator CLI (standalone — no hermes_core link needed)
add_executable(hermes_fault src/cli/hermes_fault.cpp)

# Offline Predictor Evaluator CLI
add_executable(hermes_eval src/cli/hermes_eval.cpp)

# Benchmark Scenario Harness CLI
add_executable(hermes_bench src/cli/hermes_bench.cpp)
target_link_libraries(hermes_bench PRIVATE hermes_core)

# Multi-run Artifact Report Generator CLI (standalone)
add_executable(hermes_report src/cli/hermes_report.cpp)

# Benchmark Comparison Aggregator CLI (standalone)
add_executable(hermes_compare src/cli/hermes_compare.cpp)

# ---------------------------------------------------------------------------
# Install targets
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS
    hermesd
    hermesd_mt
    hermesctl
    hermes_replay
    hermes_reeval
    hermes_synth
    hermes_fault
    hermes_eval
    hermes_bench
    hermes_report
    hermes_compare
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(TARGETS hermes_core
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp"
)

install(FILES config/schema.yaml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/hermes
)

install(PROGRAMS
    scripts/bench_strace.sh
    scripts/bench_perf.sh
    scripts/bench_gdb.sh
    scripts/smoke_wsl2.sh
    DESTINATION ${CMAKE_INSTALL_DATADIR}/hermes/scripts
    OPTIONAL
)
