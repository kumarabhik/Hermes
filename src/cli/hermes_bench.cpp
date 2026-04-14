// hermes_bench: Benchmark scenario launcher stub.
//
// Reads a scenario YAML config, validates it, prints a launch plan,
// and (when --dry-run is not specified) spawns the described workloads.
//
// Current status (Phase 4 scaffold):
//   - Scenario config loading and validation are implemented.
//   - Workload launch via system() is scaffolded and compile-guarded.
//   - Full benchmark harness with parallel workload management, timing,
//     and artifact capture is still pending.
//
// Usage:
//   hermes_bench <scenario_config.yaml> [--dry-run] [--generate-baseline] [--generate-active]

#include "hermes/runtime/scenario_config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

bool has_flag(const std::vector<std::string>& args, const std::string& flag) {
    for (const auto& a : args) {
        if (a == flag) return true;
    }
    return false;
}

void print_scenario(const hermes::BenchmarkScenario& s) {
    std::cout << "Scenario         : " << s.name << "\n"
              << "Runtime mode     : " << s.runtime_mode << "\n"
              << "Warmup           : " << s.warmup_s << "s\n"
              << "Measurement      : " << s.measurement_s << "s\n"
              << "Repeat count     : " << s.repeat_count << "\n"
              << "UPS thresholds   : elevated=" << s.ups_elevated_threshold
              << " critical=" << s.ups_critical_threshold << "\n"
              << "Workloads (" << s.workloads.size() << "):\n";
    for (const auto& w : s.workloads) {
        std::cout << "  [" << w.name << "] "
                  << (w.foreground ? "foreground" : w.background ? "background" : "default")
                  << " duration=" << w.duration_s << "s\n"
                  << "    cmd: " << w.command << "\n";
    }
    if (s.expected_max_oom_count > 0 || s.expected_max_p95_latency_ms > 0 ||
        s.expected_min_job_completion_rate > 0) {
        std::cout << "Assertions:\n"
                  << "  max OOM count   : " << s.expected_max_oom_count << "\n"
                  << "  max p95 latency : " << s.expected_max_p95_latency_ms << " ms\n"
                  << "  min completion  : " << s.expected_min_job_completion_rate << "\n";
    }
}

// Generate a scenario config file at the given path.
void generate_scenario(const std::string& path, bool active) {
    hermes::ScenarioConfigLoader loader;
    const hermes::BenchmarkScenario scenario =
        active ? loader.default_active() : loader.default_baseline();
    std::string error;
    if (loader.save(path, scenario, error)) {
        std::cout << "hermes_bench: wrote scenario config to " << path << std::endl;
    } else {
        std::cerr << "hermes_bench: failed to write config: " << error << std::endl;
    }
}

} // namespace

int main(int argc, char* argv[]) {
    std::vector<std::string> args(argv + 1, argv + argc);

    if (args.empty() || has_flag(args, "--help")) {
        std::cout << "Usage: hermes_bench <scenario.yaml> [options]\n"
                  << "       hermes_bench --generate-baseline [output.yaml]\n"
                  << "       hermes_bench --generate-active   [output.yaml]\n"
                  << "\nOptions:\n"
                  << "  --dry-run           Print plan without launching workloads\n"
                  << "  --generate-baseline Write a default baseline scenario config\n"
                  << "  --generate-active   Write a default active-control scenario config\n";
        return args.empty() ? 1 : 0;
    }

    if (has_flag(args, "--generate-baseline")) {
        const std::string path = (args.size() >= 2 && args[1][0] != '-') ? args[1] : "baseline_scenario.yaml";
        generate_scenario(path, false);
        return 0;
    }

    if (has_flag(args, "--generate-active")) {
        const std::string path = (args.size() >= 2 && args[1][0] != '-') ? args[1] : "active_scenario.yaml";
        generate_scenario(path, true);
        return 0;
    }

    const std::string config_path = args[0];
    const bool dry_run = has_flag(args, "--dry-run");

    hermes::ScenarioConfigLoader loader;
    hermes::BenchmarkScenario scenario;
    std::string error;
    if (!loader.load(config_path, scenario, error)) {
        std::cerr << "hermes_bench: failed to load scenario: " << error << std::endl;
        return 1;
    }

    std::cout << "hermes_bench: loaded scenario from " << config_path << "\n\n";
    print_scenario(scenario);

    if (dry_run) {
        std::cout << "\n[dry-run] No workloads launched. Remove --dry-run to run the benchmark.\n";
        return 0;
    }

    // Workload launch is scaffolded here.
    // A full implementation will use platform-native process APIs (posix_spawn on Linux)
    // with per-PID tracking, a measurement timer, and artifact capture.
    // The current scaffold uses system() for simplicity and will be replaced.
    std::cout << "\n[hermes_bench] Workload launch is pending full harness implementation.\n"
              << "Use --dry-run to validate scenario config in the meantime.\n"
              << "See Phase 4 in roadmap.md for the full benchmark harness plan.\n";

    return 0;
}
