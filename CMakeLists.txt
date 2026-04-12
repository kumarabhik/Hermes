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
    src/profiler/proc_stat.cpp
    src/profiler/process_mapper.cpp
    src/profiler/workload_classifier.cpp
    src/engine/pressure_score.cpp
    src/engine/predictor.cpp
    src/engine/scheduler.cpp
    src/actions/dry_run_executor.cpp
    src/runtime/event_logger.cpp
    src/runtime/run_metadata.cpp
    src/runtime/telemetry_quality.cpp
    src/replay/replay_summary.cpp
)
target_include_directories(hermes_core PUBLIC include)

# Main Daemon Executable
add_executable(hermesd src/runtime/hermesd.cpp)
target_link_libraries(hermesd PRIVATE hermes_core)

# Replay Summary CLI
add_executable(hermes_replay src/cli/hermes_replay.cpp)
target_link_libraries(hermes_replay PRIVATE hermes_core)

# Synthetic Fixture Generator CLI
add_executable(hermes_synth src/cli/hermes_synth.cpp)
target_link_libraries(hermes_synth PRIVATE hermes_core)
