#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <thread>
#include <vector>

#include "hermes/actions/active_executor.hpp"
#include "hermes/actions/dry_run_executor.hpp"
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
#include "hermes/runtime/event_logger.hpp"
#include "hermes/runtime/run_metadata.hpp"
#include "hermes/runtime/telemetry_quality.hpp"

namespace hermes {
namespace {

std::string format_double(bool available, double value, int precision = 2) {
    if (!available) {
        return "na";
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << value;
    return oss.str();
}

std::string format_uint(bool available, uint32_t value) {
    if (!available) {
        return "na";
    }

    return std::to_string(value);
}

std::string join_strings(const std::vector<std::string>& values, std::size_t limit = 3) {
    if (values.empty()) {
        return "none";
    }

    std::ostringstream oss;
    const std::size_t count = std::min(limit, values.size());
    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0) {
            oss << ",";
        }
        oss << values[index];
    }
    return oss.str();
}

std::string join_ints(const std::vector<int>& values, std::size_t limit = 2) {
    if (values.empty()) {
        return "none";
    }

    std::ostringstream oss;
    const std::size_t count = std::min(limit, values.size());
    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0) {
            oss << ",";
        }
        oss << values[index];
    }
    return oss.str();
}

std::string env_or(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    return value;
}

OperatingMode parse_runtime_mode(const std::string& mode_str) {
    if (mode_str == "active-control") {
        return OperatingMode::ActiveControl;
    }
    if (mode_str == "advisory") {
        return OperatingMode::Advisory;
    }
    return OperatingMode::ObserveOnly;
}

uint64_t parse_max_loops() {
    const char* value = std::getenv("HERMES_MAX_LOOPS");
    if (value == nullptr || value[0] == '\0') {
        return 0;
    }

    try {
        return std::stoull(value);
    } catch (...) {
        return 0;
    }
}

std::string make_run_id() {
    const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return "run-" + std::to_string(wall_ms);
}

std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return "";
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::string fnv1a_64_hex(const std::string& value) {
    uint64_t hash = 14695981039346656037ull;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }

    std::ostringstream oss;
    oss << std::hex << hash;
    return oss.str();
}

std::string default_config_hash(const std::string& config_path) {
    const std::string config = read_file(config_path);
    if (config.empty()) {
        return "config-unavailable";
    }
    return "fnv1a64-" + fnv1a_64_hex(config);
}

void print_top_processes(const std::vector<ProcessSnapshot>& processes) {
    if (processes.empty()) {
        std::cout << "    top processes: none" << std::endl;
        return;
    }

    const std::size_t count = std::min<std::size_t>(3, processes.size());
    std::cout << "    top processes:";
    for (std::size_t i = 0; i < count; ++i) {
        const ProcessSnapshot& process = processes[i];
        std::cout << " [pid=" << process.pid
                  << " class=" << to_string(process.workload_class)
                  << " cpu=" << std::fixed << std::setprecision(1) << process.cpu_pct
                  << "% rss=" << process.rss_mb
                  << "MB gpu=" << process.gpu_mb
                  << "MB cmd=\"" << process.cmd.substr(0, 28) << "\"]";
    }
    std::cout << std::endl;
}

void print_scorecard(
    const PressureScore& score,
    const RiskPrediction& prediction,
    const InterventionDecision& decision,
    const InterventionResult& result) {
    std::cout << "    ups=" << std::fixed << std::setprecision(1) << score.ups
              << " band=" << to_string(score.band)
              << " dominant=" << join_strings(score.dominant_signals)
              << " risk=" << std::fixed << std::setprecision(2) << prediction.risk_score
              << " risk_band=" << to_string(prediction.risk_band)
              << " event=" << prediction.predicted_event
              << " lead=" << std::fixed << std::setprecision(1) << prediction.lead_time_s
              << "s action=" << to_string(decision.action)
              << " level=" << to_string(decision.level)
              << " state=" << to_string(decision.scheduler_state)
              << " targets=" << join_ints(decision.target_pids)
              << " effect=\"" << result.system_effect << "\""
              << std::endl;
}

} // namespace
} // namespace hermes

int main() {
    using namespace hermes;
    using namespace std::chrono_literals;

    std::cout << "Starting Hermes Daemon (hermesd)..." << std::endl;
    std::cout << "Observation mode: enabled" << std::endl;

    const std::string runtime_mode_str = env_or("HERMES_RUNTIME_MODE", "observe-only");
    const OperatingMode runtime_mode = parse_runtime_mode(runtime_mode_str);

    EventLoggerConfig event_logger_config;
    event_logger_config.artifact_root = env_or("HERMES_ARTIFACT_ROOT", "artifacts");
    event_logger_config.run_id = env_or("HERMES_RUN_ID", make_run_id());
    event_logger_config.scenario = env_or("HERMES_SCENARIO", "observe");
    const std::string config_path = env_or("HERMES_CONFIG_PATH", "config/schema.yaml");
    event_logger_config.config_hash = env_or("HERMES_CONFIG_HASH", default_config_hash(config_path));

    EventLogger event_logger(event_logger_config);
    TelemetryQualityConfig telemetry_quality_config;
    telemetry_quality_config.run_id = event_logger_config.run_id;
    telemetry_quality_config.scenario = event_logger_config.scenario;
    telemetry_quality_config.config_hash = event_logger_config.config_hash;

    std::cout << "Runtime mode: " << runtime_mode_str << std::endl;

    if (event_logger.open()) {
        event_logger.log_run_start(runtime_mode_str);
        std::cout << "Artifact logging: " << event_logger.run_directory().string() << std::endl;
        telemetry_quality_config.run_directory = event_logger.run_directory();

        RunMetadataConfig metadata_config;
        metadata_config.artifact_root = event_logger_config.artifact_root;
        metadata_config.run_directory = event_logger.run_directory();
        metadata_config.config_path = config_path;
        metadata_config.run_id = event_logger_config.run_id;
        metadata_config.scenario = event_logger_config.scenario;
        metadata_config.config_hash = event_logger_config.config_hash;
        metadata_config.runtime_mode = runtime_mode_str;

        RunMetadataWriter metadata_writer;
        std::string metadata_error;
        if (metadata_writer.write(metadata_config, metadata_error)) {
            event_logger.log_event(
                "metadata_written",
                "{\"run_metadata\":\"run_metadata.json\",\"config_snapshot\":\"config_snapshot.yaml\"}");
        } else {
            event_logger.log_event(
                "metadata_error",
                "{\"error\":\"" + json_escape(metadata_error) + "\"}");
            std::cout << "Run metadata warning: " << metadata_error << std::endl;
        }
    } else {
        std::cout << "Artifact logging disabled: " << event_logger.last_error() << std::endl;
    }

    TelemetryQualityTracker telemetry_quality(telemetry_quality_config);

    const uint64_t max_loops = parse_max_loops();
    if (max_loops > 0) {
        std::cout << "Smoke loop limit: " << max_loops << std::endl;
    }

    CpuPsiMonitor cpu_monitor;
    MemPsiMonitor mem_monitor;
    IoPsiMonitor io_monitor;
    LoadAvgMonitor loadavg_monitor;
    GpuStatsCollector gpu_collector;
    VmstatMonitor vmstat_monitor;
    ProcessMapper process_mapper;
    WorkloadClassifier workload_classifier;
    PressureScoreCalculator pressure_score_calculator;
    OomPredictor predictor;
    SchedulerConfig scheduler_config;
    scheduler_config.mode = runtime_mode;
    Scheduler scheduler(scheduler_config);
    ActiveExecutor active_executor;

    std::vector<ProcessSnapshot> last_processes;
    auto next_process_refresh = std::chrono::steady_clock::now();
    uint64_t loop_count = 0;

    while (true) {
        PressureSample current_sample;
        stamp_pressure_sample(current_sample);

        const bool cpu_ok = cpu_monitor.update_sample(current_sample);
        const bool mem_ok = mem_monitor.update_sample(current_sample);
        const bool io_ok = io_monitor.update_sample(current_sample);
        const bool loadavg_ok = loadavg_monitor.update_sample(current_sample);
        const bool gpu_ok = gpu_collector.update_sample(current_sample);
        const bool vmstat_ok = vmstat_monitor.update_sample(current_sample);
        event_logger.log_sample(current_sample, cpu_ok, mem_ok, loadavg_ok, gpu_ok, io_ok, vmstat_ok);

        bool refreshed_processes = false;
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_process_refresh) {
            last_processes = process_mapper.collect(gpu_collector.query_process_usage());
            workload_classifier.classify(last_processes);
            next_process_refresh = now + 1s;
            refreshed_processes = true;
        }

        std::cout << "[O] ts=" << current_sample.ts_wall
                  << " cpu.some=" << format_double(cpu_ok, current_sample.cpu_some_avg10)
                  << " cpu.full=" << format_double(cpu_ok, current_sample.cpu_full_avg10)
                  << " mem.some=" << format_double(mem_ok, current_sample.mem_some_avg10)
                  << " mem.full=" << format_double(mem_ok, current_sample.mem_full_avg10)
                  << " runnable=" << format_uint(loadavg_ok, current_sample.loadavg_runnable)
                  << " gpu.util=" << format_double(gpu_ok, current_sample.gpu_util_pct)
                  << " vram.used=" << format_double(gpu_ok, current_sample.vram_used_mb)
                  << "MB vram.free=" << format_double(gpu_ok, current_sample.vram_free_mb)
                  << "MB" << std::endl;

        if (refreshed_processes) {
            print_top_processes(last_processes);
            event_logger.log_processes(current_sample, last_processes);
        }

        const PressureScore score = pressure_score_calculator.compute(current_sample);
        const RiskPrediction prediction = predictor.update(current_sample, last_processes, score);
        const InterventionDecision decision = scheduler.evaluate(score, prediction, last_processes);
        const InterventionResult result = active_executor.execute(decision);

        // On recovery, release any paused processes
        if (decision.scheduler_state_changed &&
            decision.scheduler_state == SchedulerState::Recovery) {
            active_executor.throttle_action().resume_all(decision.ts_mono);
        }
        event_logger.log_score(score);
        event_logger.log_prediction(prediction);
        event_logger.log_decision(score, prediction, decision);
        event_logger.log_action(decision, result);
        telemetry_quality.observe_loop(
            current_sample,
            cpu_ok,
            mem_ok,
            loadavg_ok,
            gpu_ok,
            refreshed_processes,
            last_processes.size(),
            score,
            prediction,
            decision,
            result);
        std::string telemetry_error;
        if (!telemetry_quality.write(telemetry_error)) {
            event_logger.log_event(
                "telemetry_quality_error",
                "{\"error\":\"" + json_escape(telemetry_error) + "\"}");
        }
        print_scorecard(score, prediction, decision, result);

        ++loop_count;
        if (max_loops > 0 && loop_count >= max_loops) {
            std::ostringstream payload;
            payload << "{\"reason\":\"max_loops\",\"loop_count\":" << loop_count << "}";
            event_logger.log_event("run_stop", payload.str());
            break;
        }

        std::this_thread::sleep_for(500ms);
    }

    return 0;
}
