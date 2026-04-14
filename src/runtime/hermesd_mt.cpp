// hermesd_mt: Multi-threaded Hermes daemon using the EventBus ring buffer.
//
// Architecture:
//   Sampler thread:  collects PSI, GPU, vmstat, process snapshots on 500ms cadence.
//                    Pushes PressureSample + ProcessSnapshot batch into the EventBus.
//   Policy thread:   reads batches from the EventBus, runs UPS + predictor + scheduler,
//                    dispatches actions, logs all artifacts.
//
// The sampler never blocks on the policy thread. If the policy thread falls behind,
// the oldest sample is dropped (counted in telemetry_quality drop metrics).
//
// Usage: same env vars as hermesd (HERMES_RUNTIME_MODE, HERMES_RUN_ID, etc.)
//   HERMES_SAMPLER_CADENCE_MS: sampler interval in ms (default 500)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "hermes/actions/active_executor.hpp"
#include "hermes/core/types.hpp"
#include "hermes/engine/predictor.hpp"
#include "hermes/engine/pressure_score.hpp"
#include "hermes/engine/scheduler.hpp"
#include "hermes/monitor/cpu_psi.hpp"
#include "hermes/monitor/gpu_stats.hpp"
#include "hermes/monitor/io_psi.hpp"
#include "hermes/monitor/loadavg.hpp"
#include "hermes/monitor/mem_psi.hpp"
#include "hermes/monitor/vmstat.hpp"
#include "hermes/profiler/process_mapper.hpp"
#include "hermes/profiler/workload_classifier.hpp"
#include "hermes/runtime/event_bus.hpp"
#include "hermes/runtime/event_logger.hpp"
#include "hermes/runtime/run_metadata.hpp"
#include "hermes/runtime/telemetry_quality.hpp"

namespace hermes {

// ---- Shared state bundle passed between sampler and policy ----

struct SamplerBatch {
    PressureSample sample;
    std::vector<ProcessSnapshot> processes;
    bool process_refresh{false};
    bool cpu_ok{false};
    bool mem_ok{false};
    bool io_ok{false};
    bool loadavg_ok{false};
    bool gpu_ok{false};
    bool vmstat_ok{false};
};

// ---- Utility helpers (same as hermesd.cpp) ----

namespace {

std::string env_or(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') return fallback;
    return value;
}

uint64_t parse_max_loops() {
    const char* value = std::getenv("HERMES_MAX_LOOPS");
    if (value == nullptr || value[0] == '\0') return 0;
    try { return std::stoull(value); } catch (...) { return 0; }
}

uint64_t parse_cadence_ms() {
    const char* value = std::getenv("HERMES_SAMPLER_CADENCE_MS");
    if (value == nullptr || value[0] == '\0') return 500;
    try { return std::stoull(value); } catch (...) { return 500; }
}

std::string make_run_id() {
    const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return "run-" + std::to_string(wall_ms);
}

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return "";
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string fnv1a_64_hex(const std::string& value) {
    uint64_t hash = 14695981039346656037ull;
    for (const unsigned char b : value) { hash ^= b; hash *= 1099511628211ull; }
    std::ostringstream oss; oss << std::hex << hash; return oss.str();
}

std::string default_config_hash(const std::string& config_path) {
    const std::string c = read_file(config_path);
    if (c.empty()) return "config-unavailable";
    return "fnv1a64-" + fnv1a_64_hex(c);
}

OperatingMode parse_runtime_mode(const std::string& s) {
    if (s == "active-control") return OperatingMode::ActiveControl;
    if (s == "advisory") return OperatingMode::Advisory;
    return OperatingMode::ObserveOnly;
}

} // namespace

// ---- Sampler thread ----

void sampler_thread_fn(
    EventBus<SamplerBatch>& bus,
    std::atomic<bool>& stop_flag,
    uint64_t cadence_ms,
    uint64_t max_loops) {

    CpuPsiMonitor cpu_monitor;
    MemPsiMonitor mem_monitor;
    IoPsiMonitor io_monitor;
    LoadAvgMonitor loadavg_monitor;
    GpuStatsCollector gpu_collector;
    VmstatMonitor vmstat_monitor;
    ProcessMapper process_mapper;
    WorkloadClassifier workload_classifier;

    auto next_process_refresh = std::chrono::steady_clock::now();
    uint64_t loop_count = 0;

    while (!stop_flag.load(std::memory_order_relaxed)) {
        const auto tick_start = std::chrono::steady_clock::now();

        SamplerBatch batch;
        stamp_pressure_sample(batch.sample);

        batch.cpu_ok     = cpu_monitor.update_sample(batch.sample);
        batch.mem_ok     = mem_monitor.update_sample(batch.sample);
        batch.io_ok      = io_monitor.update_sample(batch.sample);
        batch.loadavg_ok = loadavg_monitor.update_sample(batch.sample);
        batch.gpu_ok     = gpu_collector.update_sample(batch.sample);
        batch.vmstat_ok  = vmstat_monitor.update_sample(batch.sample);

        const auto now_tp = std::chrono::steady_clock::now();
        if (now_tp >= next_process_refresh) {
            batch.processes = process_mapper.collect(gpu_collector.query_process_usage());
            workload_classifier.classify(batch.processes);
            next_process_refresh = now_tp + std::chrono::seconds(1);
            batch.process_refresh = true;
        }

        bus.push(std::move(batch));

        ++loop_count;
        if (max_loops > 0 && loop_count >= max_loops) {
            stop_flag.store(true, std::memory_order_relaxed);
            break;
        }

        const auto elapsed = std::chrono::steady_clock::now() - tick_start;
        const auto sleep_dur = std::chrono::milliseconds(cadence_ms) - elapsed;
        if (sleep_dur > std::chrono::milliseconds(0)) {
            std::this_thread::sleep_for(sleep_dur);
        }
    }
}

// ---- Policy thread ----

void policy_thread_fn(
    EventBus<SamplerBatch>& bus,
    std::atomic<bool>& stop_flag,
    EventLogger& event_logger,
    TelemetryQualityTracker& telemetry_quality,
    OperatingMode runtime_mode) {

    PressureScoreCalculator pressure_score_calculator;
    OomPredictor predictor;
    SchedulerConfig scheduler_config;
    scheduler_config.mode = runtime_mode;
    Scheduler scheduler(scheduler_config);
    ActiveExecutor active_executor;

    std::vector<ProcessSnapshot> last_processes;

    while (!stop_flag.load(std::memory_order_relaxed) || !bus.empty()) {
        auto maybe_batch = bus.pop_wait(std::chrono::milliseconds(100));
        if (!maybe_batch) {
            continue;
        }
        SamplerBatch& batch = *maybe_batch;

        event_logger.log_sample(
            batch.sample,
            batch.cpu_ok, batch.mem_ok, batch.loadavg_ok, batch.gpu_ok,
            batch.io_ok, batch.vmstat_ok);

        if (batch.process_refresh) {
            last_processes = std::move(batch.processes);
            event_logger.log_processes(batch.sample, last_processes);
        }

        const PressureScore score = pressure_score_calculator.compute(batch.sample);
        const RiskPrediction prediction = predictor.update(batch.sample, last_processes, score);
        const InterventionDecision decision = scheduler.evaluate(score, prediction, last_processes);
        const InterventionResult result = active_executor.execute(decision);

        if (decision.scheduler_state_changed &&
            decision.scheduler_state == SchedulerState::Recovery) {
            active_executor.throttle_action().resume_all(decision.ts_mono);
        }

        event_logger.log_score(score);
        event_logger.log_prediction(prediction);
        event_logger.log_decision(score, prediction, decision);
        event_logger.log_action(decision, result);

        telemetry_quality.observe_loop(
            batch.sample,
            batch.cpu_ok, batch.mem_ok, batch.loadavg_ok, batch.gpu_ok,
            batch.process_refresh, last_processes.size(),
            score, prediction, decision, result);

        std::string tq_error;
        telemetry_quality.write(tq_error);

        // Console output
        std::cout << "[MT] ts=" << batch.sample.ts_wall
                  << " ups=" << std::fixed << std::setprecision(1) << score.ups
                  << " band=" << to_string(score.band)
                  << " risk=" << std::fixed << std::setprecision(2) << prediction.risk_score
                  << " state=" << to_string(decision.scheduler_state)
                  << " action=" << to_string(decision.action)
                  << " queue=" << bus.size()
                  << " drops=" << bus.drop_count()
                  << std::endl;
    }
}

} // namespace hermes

int main() {
    using namespace hermes;

    std::cout << "Starting Hermes Daemon (hermesd_mt) — multi-threaded mode" << std::endl;

    const std::string runtime_mode_str = env_or("HERMES_RUNTIME_MODE", "observe-only");
    const OperatingMode runtime_mode = parse_runtime_mode(runtime_mode_str);
    const uint64_t cadence_ms = parse_cadence_ms();
    const uint64_t max_loops = parse_max_loops();

    std::cout << "Runtime mode   : " << runtime_mode_str << std::endl;
    std::cout << "Sampler cadence: " << cadence_ms << " ms" << std::endl;
    if (max_loops > 0) {
        std::cout << "Max loops      : " << max_loops << std::endl;
    }

    EventLoggerConfig event_logger_config;
    event_logger_config.artifact_root = env_or("HERMES_ARTIFACT_ROOT", "artifacts");
    event_logger_config.run_id = env_or("HERMES_RUN_ID", make_run_id());
    event_logger_config.scenario = env_or("HERMES_SCENARIO", "observe-mt");
    const std::string config_path = env_or("HERMES_CONFIG_PATH", "config/schema.yaml");
    event_logger_config.config_hash = env_or("HERMES_CONFIG_HASH", default_config_hash(config_path));

    EventLogger event_logger(event_logger_config);
    TelemetryQualityConfig tq_config;
    tq_config.run_id = event_logger_config.run_id;
    tq_config.scenario = event_logger_config.scenario;
    tq_config.config_hash = event_logger_config.config_hash;

    if (event_logger.open()) {
        event_logger.log_run_start(runtime_mode_str);
        std::cout << "Artifacts: " << event_logger.run_directory().string() << std::endl;
        tq_config.run_directory = event_logger.run_directory();
    } else {
        std::cout << "Artifact logging disabled: " << event_logger.last_error() << std::endl;
    }

    TelemetryQualityTracker telemetry_quality(tq_config);

    // Bounded event bus: 256 slots. At 500ms cadence, 256 slots = 128s of buffer.
    EventBus<SamplerBatch> sample_bus(256);
    std::atomic<bool> stop_flag{false};

    std::thread sampler(sampler_thread_fn,
        std::ref(sample_bus),
        std::ref(stop_flag),
        cadence_ms,
        max_loops);

    std::thread policy(policy_thread_fn,
        std::ref(sample_bus),
        std::ref(stop_flag),
        std::ref(event_logger),
        std::ref(telemetry_quality),
        runtime_mode);

    sampler.join();
    policy.join();

    std::cout << "hermesd_mt: exited cleanly. Drop count: " << sample_bus.drop_count() << std::endl;
    return 0;
}
