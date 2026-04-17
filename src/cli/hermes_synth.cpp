#include "hermes/actions/dry_run_executor.hpp"
#include "hermes/core/types.hpp"
#include "hermes/engine/predictor.hpp"
#include "hermes/engine/pressure_score.hpp"
#include "hermes/engine/scheduler.hpp"
#include "hermes/runtime/event_logger.hpp"
#include "hermes/runtime/run_metadata.hpp"
#include "hermes/runtime/telemetry_quality.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string env_or(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    return value;
}

uint64_t wall_now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

uint64_t mono_now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::string make_run_id() {
    return "synthetic-pressure-" + std::to_string(wall_now_ms());
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

bool write_text_file(const std::filesystem::path& path, const std::string& content, std::string& error) {
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }

    std::ofstream output(path, std::ios::out | std::ios::binary);
    if (!output.is_open()) {
        error = "failed to open " + path.string();
        return false;
    }

    output << content;
    return true;
}

hermes::ProcessSnapshot make_process(
    int pid,
    const std::string& cmd,
    double cpu_pct,
    double rss_mb,
    double gpu_mb,
    hermes::WorkloadClass workload_class,
    bool foreground,
    bool protected_process) {
    hermes::ProcessSnapshot process;
    process.pid = pid;
    process.ppid = 100;
    process.cmd = cmd;
    process.state = "R";
    process.nice = 0;
    process.cpu_pct = cpu_pct;
    process.rss_mb = rss_mb;
    process.gpu_mb = gpu_mb;
    process.workload_class = workload_class;
    process.foreground = foreground;
    process.protected_process = protected_process;
    process.total_cpu_ticks = static_cast<uint64_t>(cpu_pct * 100.0);
    return process;
}

std::vector<hermes::ProcessSnapshot> make_processes(double background_gpu_mb, double background_cpu_pct) {
    return {
        make_process(
            4242,
            "python train_background.py --synthetic",
            background_cpu_pct,
            3072.0,
            background_gpu_mb,
            hermes::WorkloadClass::Training,
            false,
            false),
        make_process(
            5151,
            "python serve_foreground.py --synthetic",
            18.0,
            1024.0,
            1024.0,
            hermes::WorkloadClass::Inference,
            true,
            false),
        make_process(
            99,
            "hermes_synth",
            1.0,
            64.0,
            0.0,
            hermes::WorkloadClass::Background,
            false,
            true),
    };
}

struct SynthFrame {
    double cpu_some;
    double cpu_full;
    double mem_some;
    double mem_full;
    double gpu_util;
    double vram_used;
    double vram_free;
};

std::vector<hermes::PressureSample> frames_to_samples(const std::vector<SynthFrame>& frames) {
    const uint64_t base_wall = wall_now_ms();
    const uint64_t base_mono = mono_now_ms();
    std::vector<hermes::PressureSample> samples;
    samples.reserve(frames.size());
    for (std::size_t index = 0; index < frames.size(); ++index) {
        hermes::PressureSample sample;
        sample.ts_wall = base_wall + static_cast<uint64_t>(index * 1000);
        sample.ts_mono = base_mono + static_cast<uint64_t>(index * 1000);
        sample.cpu_some_avg10 = frames[index].cpu_some;
        sample.cpu_full_avg10 = frames[index].cpu_full;
        sample.mem_some_avg10 = frames[index].mem_some;
        sample.mem_full_avg10 = frames[index].mem_full;
        sample.gpu_util_pct = frames[index].gpu_util;
        sample.vram_used_mb = frames[index].vram_used;
        sample.vram_total_mb = 10000.0;
        sample.vram_free_mb = frames[index].vram_free;
        sample.loadavg_runnable = static_cast<uint32_t>(2 + index);
        samples.push_back(sample);
    }
    return samples;
}

// Default fixture: exercises elevated → throttled → cooldown → recovery.
std::vector<hermes::PressureSample> make_samples() {
    return frames_to_samples({
        {2.0, 0.0, 1.0, 0.0, 12.0, 1200.0, 8800.0},
        {8.0, 0.5, 6.0, 1.0, 45.0, 5000.0, 5000.0},
        {20.0, 1.0, 14.0, 3.0, 95.0, 8000.0, 2000.0},
        {22.0, 2.0, 22.0, 6.0, 96.0, 9700.0, 300.0},
        {20.0, 1.5, 18.0, 5.0, 90.0, 9750.0, 250.0},
        {4.0, 0.0, 3.0, 0.2, 24.0, 4200.0, 5800.0},
        {2.0, 0.0, 1.0, 0.0, 10.0, 1800.0, 8200.0},
        {1.0, 0.0, 0.5, 0.0, 5.0, 1000.0, 9000.0},
    });
}

// Cooldown preset: moderate pressure ramp → sustained elevated → sudden drop → held low.
// Emphasises the cooldown → idle transition without hitting terminate_candidate.
std::vector<hermes::PressureSample> make_cooldown_samples() {
    return frames_to_samples({
        {3.0, 0.0, 2.0, 0.0, 15.0, 1500.0, 8500.0},
        {12.0, 0.5, 8.0, 1.0, 50.0, 5200.0, 4800.0},
        {18.0, 1.0, 12.0, 2.0, 72.0, 7200.0, 2800.0},
        {20.0, 1.5, 15.0, 3.5, 80.0, 8100.0, 1900.0},
        {18.0, 1.0, 13.0, 3.0, 76.0, 7800.0, 2200.0},
        {6.0, 0.0, 4.0, 0.3, 28.0, 3000.0, 7000.0},
        {3.0, 0.0, 2.0, 0.1, 14.0, 1800.0, 8200.0},
        {2.0, 0.0, 1.0, 0.0, 8.0, 1200.0, 8800.0},
        {1.5, 0.0, 0.8, 0.0, 6.0, 1100.0, 8900.0},
        {1.0, 0.0, 0.5, 0.0, 5.0, 1000.0, 9000.0},
    });
}

// All-states preset: exercises every scheduler state and every intervention level
// in a single deterministic run.
//   Normal → Elevated → Throttled (L2) → Cooldown → Recovery → Normal
//   Also fires L1 (reprioritize) and L3 (terminate) actions along the way.
// Designed for state-transition coverage testing.
std::vector<hermes::PressureSample> make_all_states_samples() {
    return frames_to_samples({
        // Normal — low pressure baseline
        {1.0, 0.0, 0.5, 0.0,  5.0,  900.0, 9100.0},
        {1.5, 0.0, 0.8, 0.0,  8.0, 1200.0, 8800.0},
        // Elevated — crosses elevated threshold, triggers L1
        {8.0, 0.3, 5.0, 0.5, 38.0, 4200.0, 5800.0},
        {12.0, 0.5, 9.0, 1.0, 52.0, 5800.0, 4200.0},
        {14.0, 0.8, 11.0, 1.5, 58.0, 6200.0, 3800.0},
        // Throttled — crosses critical, triggers L2 SIGSTOP
        {18.0, 1.5, 16.0, 3.0, 78.0, 8100.0, 1900.0},
        {20.0, 2.0, 18.0, 4.0, 85.0, 8800.0, 1200.0},
        // OOM-imminent — triggers L3 terminate
        {22.0, 3.0, 22.0, 6.0, 96.0, 9700.0,  300.0},
        {23.0, 4.0, 24.0, 7.0, 98.0, 9800.0,  200.0},
        // Cooldown — pressure drops after kill, scheduler enters cooldown
        {14.0, 1.0, 10.0, 2.0, 60.0, 6500.0, 3500.0},
        {8.0,  0.5,  6.0, 1.0, 38.0, 4500.0, 5500.0},
        {5.0,  0.2,  4.0, 0.5, 25.0, 3200.0, 6800.0},
        // Recovery — sustained low pressure, SIGCONT sent
        {3.0,  0.0,  2.0, 0.1, 15.0, 2000.0, 8000.0},
        {2.0,  0.0,  1.5, 0.0, 10.0, 1600.0, 8400.0},
        // Normal — back to baseline
        {1.5,  0.0,  1.0, 0.0,  7.0, 1200.0, 8800.0},
        {1.0,  0.0,  0.5, 0.0,  5.0, 1000.0, 9000.0},
    });
}

// Recovery preset: starts at peak pressure, then drops sharply and stays stable.
// Emphasises the recovery state without retriggering elevated/throttled.
std::vector<hermes::PressureSample> make_recovery_samples() {
    return frames_to_samples({
        {22.0, 2.0, 20.0, 5.0, 95.0, 9600.0, 400.0},
        {20.0, 1.5, 18.0, 4.5, 92.0, 9500.0, 500.0},
        {15.0, 1.0, 12.0, 3.0, 78.0, 8200.0, 1800.0},
        {8.0, 0.5, 6.0, 1.5, 42.0, 5400.0, 4600.0},
        {4.0, 0.0, 3.0, 0.5, 22.0, 3000.0, 7000.0},
        {2.5, 0.0, 1.5, 0.1, 12.0, 1800.0, 8200.0},
        {1.5, 0.0, 1.0, 0.0, 8.0, 1400.0, 8600.0},
        {1.2, 0.0, 0.8, 0.0, 6.0, 1200.0, 8800.0},
        {1.0, 0.0, 0.5, 0.0, 5.0, 1000.0, 9000.0},
        {1.0, 0.0, 0.5, 0.0, 5.0, 1000.0, 9000.0},
    });
}

bool write_manifest(
    const std::filesystem::path& run_directory,
    const std::string& run_id,
    const std::string& scenario,
    const std::string& config_hash,
    std::size_t frame_count,
    std::string& error) {
    std::ostringstream manifest;
    manifest << "{\n"
             << "  \"run_id\": \"" << hermes::json_escape(run_id) << "\",\n"
             << "  \"scenario\": \"" << hermes::json_escape(scenario) << "\",\n"
             << "  \"config_hash\": \"" << hermes::json_escape(config_hash) << "\",\n"
             << "  \"kind\": \"synthetic_pressure_fixture\",\n"
             << "  \"frame_count\": " << frame_count << ",\n"
             << "  \"purpose\": \"Exercise elevated, throttled, cooldown, and recovery scheduler states without live GPU pressure.\",\n"
             << "  \"expected_signals\": [\"level1_reprioritize\", \"level2_throttle\", \"level3_terminate_candidate\", \"cooldown\", \"recovery\"],\n"
             << "  \"minimums\": {\"peak_ups\": 90.0, \"peak_risk_score\": 0.95},\n"
             << "  \"expected_min_decision_actions\": {\"reprioritize\": 1, \"throttle\": 1, \"terminate_candidate\": 1},\n"
             << "  \"expected_min_scheduler_states\": {\"elevated\": 1, \"throttled\": 1, \"cooldown\": 1, \"recovery\": 1},\n"
             << "  \"expected_min_pressure_bands\": {\"critical\": 3},\n"
             << "  \"expected_min_risk_bands\": {\"high\": 1, \"critical\": 1}\n"
             << "}\n";

    return write_text_file(run_directory / "scenario_manifest.json", manifest.str(), error);
}

void print_usage() {
    std::cout << "Usage: hermes_synth [options] [run-id] [artifact-root]\n"
              << "\n"
              << "Options:\n"
              << "  --recovery    Use recovery-focused sample sequence (high pressure → sharp drop → stable)\n"
              << "  --cooldown    Use cooldown-focused sample sequence (moderate ramp → sustained → drop)\n"
              << "  --all-states  Comprehensive preset: Normal→Elevated→Throttled→Cooldown→Recovery→Normal\n"
              << "                Exercises all 5 scheduler states and all 3 intervention levels (L1/L2/L3)\n"
              << "\n"
              << "Generates a deterministic synthetic Hermes pressure run under artifacts/logs/.\n"
              << "Environment overrides: HERMES_RUN_ID, HERMES_ARTIFACT_ROOT, HERMES_CONFIG_PATH, HERMES_CONFIG_HASH.\n";
}

} // namespace

int main(int argc, char** argv) {
    bool preset_recovery   = false;
    bool preset_cooldown   = false;
    bool preset_all_states = false;
    std::vector<std::string> pos_args;

    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else if (arg == "--recovery") {
            preset_recovery = true;
        } else if (arg == "--cooldown") {
            preset_cooldown = true;
        } else if (arg == "--all-states") {
            preset_all_states = true;
        } else {
            pos_args.push_back(arg);
        }
    }

    const std::filesystem::path artifact_root = pos_args.size() >= 2
        ? std::filesystem::path(pos_args[1])
        : std::filesystem::path(env_or("HERMES_ARTIFACT_ROOT", "artifacts"));
    const std::string run_id = !pos_args.empty()
        ? pos_args[0]
        : env_or("HERMES_RUN_ID", make_run_id());

    std::string default_scenario = "synthetic-pressure";
    if (preset_recovery)   { default_scenario = "synthetic-recovery"; }
    if (preset_cooldown)   { default_scenario = "synthetic-cooldown"; }
    if (preset_all_states) { default_scenario = "synthetic-all-states"; }
    const std::string scenario = env_or("HERMES_SCENARIO", default_scenario);
    const std::string config_path = env_or("HERMES_CONFIG_PATH", "config/schema.yaml");
    const std::string config_hash = env_or("HERMES_CONFIG_HASH", default_config_hash(config_path));

    hermes::EventLoggerConfig logger_config;
    logger_config.artifact_root = artifact_root;
    logger_config.run_id = run_id;
    logger_config.scenario = scenario;
    logger_config.config_hash = config_hash;

    hermes::EventLogger event_logger(logger_config);
    if (!event_logger.open()) {
        std::cerr << "Failed to open event logger: " << event_logger.last_error() << std::endl;
        return 2;
    }
    event_logger.log_run_start("synthetic-fixture");

    hermes::RunMetadataConfig metadata_config;
    metadata_config.artifact_root = artifact_root;
    metadata_config.run_directory = event_logger.run_directory();
    metadata_config.config_path = config_path;
    metadata_config.run_id = run_id;
    metadata_config.scenario = scenario;
    metadata_config.config_hash = config_hash;
    metadata_config.runtime_mode = "synthetic-fixture";

    hermes::RunMetadataWriter metadata_writer;
    std::string error;
    if (metadata_writer.write(metadata_config, error)) {
        event_logger.log_event(
            "metadata_written",
            "{\"run_metadata\":\"run_metadata.json\",\"config_snapshot\":\"config_snapshot.yaml\"}");
    } else {
        event_logger.log_event(
            "metadata_error",
            "{\"error\":\"" + hermes::json_escape(error) + "\"}");
        std::cerr << "Metadata warning: " << error << std::endl;
    }

    const std::vector<hermes::PressureSample> samples =
        preset_all_states ? make_all_states_samples() :
        preset_recovery   ? make_recovery_samples()   :
        preset_cooldown   ? make_cooldown_samples()   :
        make_samples();
    if (!write_manifest(event_logger.run_directory(), run_id, scenario, config_hash, samples.size(), error)) {
        event_logger.log_event(
            "scenario_manifest_error",
            "{\"error\":\"" + hermes::json_escape(error) + "\"}");
        std::cerr << "Manifest warning: " << error << std::endl;
    } else {
        event_logger.log_event(
            "scenario_manifest_written",
            "{\"scenario_manifest\":\"scenario_manifest.json\"}");
    }

    hermes::PressureScoreCalculator pressure_score_calculator;
    hermes::OomPredictor predictor;
    hermes::SchedulerConfig scheduler_config;
    scheduler_config.level1_cooldown_ms = 1000;
    scheduler_config.level2_cooldown_ms = 1000;
    scheduler_config.level3_cooldown_ms = 2500;
    scheduler_config.stable_cycles_for_recovery = 2;
    hermes::Scheduler scheduler(scheduler_config);
    hermes::DryRunExecutor dry_run_executor;

    hermes::TelemetryQualityConfig telemetry_config;
    telemetry_config.run_directory = event_logger.run_directory();
    telemetry_config.run_id = run_id;
    telemetry_config.scenario = scenario;
    telemetry_config.config_hash = config_hash;
    telemetry_config.expected_interval_ms = 1000;
    hermes::TelemetryQualityTracker telemetry_quality(telemetry_config);

    std::size_t index = 0;
    for (const hermes::PressureSample& sample : samples) {
        const double background_gpu_mb = index < 5 ? sample.vram_used_mb * 0.65 : 800.0;
        const double background_cpu_pct = index < 5 ? 70.0 + static_cast<double>(index * 2) : 8.0;
        const std::vector<hermes::ProcessSnapshot> processes =
            make_processes(background_gpu_mb, background_cpu_pct);

        event_logger.log_sample(sample, true, true, true, true);
        event_logger.log_processes(sample, processes);

        const hermes::PressureScore score = pressure_score_calculator.compute(sample);
        const hermes::RiskPrediction prediction = predictor.update(sample, processes, score);
        const hermes::InterventionDecision decision = scheduler.evaluate(score, prediction, processes);
        const hermes::InterventionResult result = dry_run_executor.execute(decision);

        event_logger.log_score(score);
        event_logger.log_prediction(prediction);
        event_logger.log_decision(score, prediction, decision);
        event_logger.log_action(decision, result);
        telemetry_quality.observe_loop(
            sample,
            true,
            true,
            true,
            true,
            true,
            processes.size(),
            score,
            prediction,
            decision,
            result);

        if (!telemetry_quality.write(error)) {
            event_logger.log_event(
                "telemetry_quality_error",
                "{\"error\":\"" + hermes::json_escape(error) + "\"}");
        }

        ++index;
    }

    std::ostringstream stop_payload;
    stop_payload << "{\"reason\":\"synthetic_complete\",\"frame_count\":" << samples.size() << "}";
    event_logger.log_event("run_stop", stop_payload.str());

    std::cout << "Synthetic Hermes run generated: " << event_logger.run_directory().string() << std::endl;
    std::cout << "Frames: " << samples.size() << std::endl;
    return 0;
}
