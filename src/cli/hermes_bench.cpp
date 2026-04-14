// hermes_bench: Benchmark scenario launcher stub.
//
// Reads a scenario YAML config, validates it, prints a launch plan,
// and writes a benchmark plan artifact for later replay/report tooling.
//
// Current status (Phase 4 scaffold):
//   - Scenario config loading and validation are implemented.
//   - Plan artifacts are written under artifacts/bench/.
//   - Full benchmark harness with parallel workload management, timing,
//     and artifact capture is still pending.
//
// Usage:
//   hermes_bench <scenario_config.yaml> [--dry-run] [--artifact-root artifacts] [--run-id id]
//   hermes_bench --generate-baseline [output.yaml]
//   hermes_bench --generate-active [output.yaml]

#include "hermes/runtime/scenario_config.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct PlanValidation {
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    std::size_t foreground_workloads{0};
    std::size_t background_workloads{0};
    uint64_t total_workload_seconds{0};
};

bool has_flag(const std::vector<std::string>& args, const std::string& flag) {
    for (const auto& a : args) {
        if (a == flag) return true;
    }
    return false;
}

std::string env_or(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    return value;
}

bool option_value(const std::vector<std::string>& args, const std::string& option, std::string& value) {
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == option) {
            value = args[i + 1];
            return true;
        }
    }
    return false;
}

bool is_option_with_value(const std::string& arg) {
    return arg == "--artifact-root" || arg == "--run-id";
}

std::string first_positional_arg(const std::vector<std::string>& args) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (is_option_with_value(args[i])) {
            ++i;
            continue;
        }
        if (!args[i].empty() && args[i][0] != '-') {
            return args[i];
        }
    }
    return "";
}

std::string generation_output_path(
    const std::vector<std::string>& args,
    const std::string& flag,
    const std::string& fallback) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] != flag) {
            continue;
        }
        if (i + 1 < args.size() && !args[i + 1].empty() && args[i + 1][0] != '-') {
            return args[i + 1];
        }
        return fallback;
    }
    return fallback;
}

uint64_t wall_now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string sanitize_id_part(const std::string& value) {
    std::string out;
    for (const unsigned char ch : value) {
        if (std::isalnum(ch) != 0 || ch == '-' || ch == '_') {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('-');
        }
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    return out.empty() ? "scenario" : out;
}

std::string default_run_id(const hermes::BenchmarkScenario& scenario) {
    return "bench-" + sanitize_id_part(scenario.name) + "-" + std::to_string(wall_now_ms());
}

std::string json_escape(const std::string& value) {
    std::ostringstream oss;
    for (const char ch : value) {
        switch (ch) {
        case '\\':
            oss << "\\\\";
            break;
        case '"':
            oss << "\\\"";
            break;
        case '\n':
            oss << "\\n";
            break;
        case '\r':
            oss << "\\r";
            break;
        case '\t':
            oss << "\\t";
            break;
        default:
            oss << ch;
            break;
        }
    }
    return oss.str();
}

std::string string_array_json(const std::vector<std::string>& values) {
    std::ostringstream oss;
    oss << "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            oss << ",";
        }
        oss << "\"" << json_escape(values[i]) << "\"";
    }
    oss << "]";
    return oss.str();
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

PlanValidation validate_plan(const hermes::BenchmarkScenario& scenario) {
    PlanValidation validation;

    if (scenario.runtime_mode != "observe-only" &&
        scenario.runtime_mode != "advisory" &&
        scenario.runtime_mode != "active-control") {
        validation.errors.push_back("runtime_mode must be observe-only, advisory, or active-control");
    }
    if (scenario.warmup_s < 0) {
        validation.errors.push_back("warmup_s must be >= 0");
    }
    if (scenario.measurement_s <= 0) {
        validation.errors.push_back("measurement_s must be > 0");
    }
    if (scenario.repeat_count <= 0) {
        validation.errors.push_back("repeat_count must be > 0");
    }
    if (scenario.workloads.empty()) {
        validation.errors.push_back("at least one workload is required");
    }

    uint64_t workload_seconds = 0;
    for (const hermes::BenchmarkWorkload& workload : scenario.workloads) {
        if (workload.name.empty()) {
            validation.errors.push_back("workload name must not be empty");
        }
        if (workload.command.empty()) {
            validation.errors.push_back("workload command must not be empty: " + workload.name);
        }
        if (workload.duration_s <= 0) {
            validation.errors.push_back("workload duration_s must be > 0: " + workload.name);
        } else {
            workload_seconds += static_cast<uint64_t>(workload.duration_s);
        }
        if (workload.foreground) {
            ++validation.foreground_workloads;
        }
        if (workload.background) {
            ++validation.background_workloads;
        }
    }

    if (!scenario.workloads.empty() && validation.foreground_workloads == 0) {
        validation.warnings.push_back("no foreground workload is marked for protection");
    }
    if (!scenario.workloads.empty() && validation.background_workloads == 0) {
        validation.warnings.push_back("no background workload is marked as controllable");
    }
    if (scenario.workloads.size() < 4) {
        validation.warnings.push_back("benchmark mix has fewer than four workloads");
    }

    validation.total_workload_seconds =
        workload_seconds * static_cast<uint64_t>(std::max(1, scenario.repeat_count));
    return validation;
}

bool write_plan_artifacts(
    const hermes::BenchmarkScenario& scenario,
    const PlanValidation& validation,
    const std::string& config_path,
    const std::string& artifact_root,
    const std::string& run_id,
    bool dry_run,
    std::filesystem::path& plan_path,
    std::filesystem::path& scenario_snapshot_path,
    std::string& error) {
    try {
        const std::filesystem::path bench_dir = std::filesystem::path(artifact_root) / "bench";
        std::filesystem::create_directories(bench_dir);
        plan_path = bench_dir / (run_id + "-plan.json");
        scenario_snapshot_path = bench_dir / (run_id + "-scenario.yaml");
    } catch (const std::exception& ex) {
        error = std::string("failed to create benchmark artifact directory: ") + ex.what();
        return false;
    }

    hermes::ScenarioConfigLoader loader;
    if (!loader.save(scenario_snapshot_path.string(), scenario, error)) {
        return false;
    }

    std::ofstream out(plan_path);
    if (!out.is_open()) {
        error = "failed to open benchmark plan artifact: " + plan_path.string();
        return false;
    }

    out << "{\n"
        << "  \"run_id\": \"" << json_escape(run_id) << "\",\n"
        << "  \"scenario\": \"" << json_escape(scenario.name) << "\",\n"
        << "  \"runtime_mode\": \"" << json_escape(scenario.runtime_mode) << "\",\n"
        << "  \"dry_run\": " << (dry_run ? "true" : "false") << ",\n"
        << "  \"source_config\": \"" << json_escape(config_path) << "\",\n"
        << "  \"scenario_snapshot\": \"" << json_escape(scenario_snapshot_path.string()) << "\",\n"
        << "  \"artifact_root\": \"" << json_escape(artifact_root) << "\",\n"
        << "  \"warmup_s\": " << scenario.warmup_s << ",\n"
        << "  \"measurement_s\": " << scenario.measurement_s << ",\n"
        << "  \"repeat_count\": " << scenario.repeat_count << ",\n"
        << "  \"workload_count\": " << scenario.workloads.size() << ",\n"
        << "  \"foreground_workloads\": " << validation.foreground_workloads << ",\n"
        << "  \"background_workloads\": " << validation.background_workloads << ",\n"
        << "  \"planned_workload_seconds\": " << validation.total_workload_seconds << ",\n"
        << "  \"warnings\": " << string_array_json(validation.warnings) << ",\n"
        << "  \"errors\": " << string_array_json(validation.errors) << ",\n"
        << "  \"workloads\": [\n";

    for (std::size_t i = 0; i < scenario.workloads.size(); ++i) {
        const hermes::BenchmarkWorkload& workload = scenario.workloads[i];
        out << "    {"
            << "\"name\":\"" << json_escape(workload.name) << "\","
            << "\"command\":\"" << json_escape(workload.command) << "\","
            << "\"foreground\":" << (workload.foreground ? "true" : "false") << ","
            << "\"background\":" << (workload.background ? "true" : "false") << ","
            << "\"duration_s\":" << workload.duration_s
            << "}";
        if (i + 1 < scenario.workloads.size()) {
            out << ",";
        }
        out << "\n";
    }

    out << "  ]\n"
        << "}\n";

    return out.good();
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
                  << "  --artifact-root DIR Artifact root for plan output (default: $HERMES_ARTIFACT_ROOT or artifacts)\n"
                  << "  --run-id ID         Run id for plan artifacts (default: generated from scenario name)\n"
                  << "  --generate-baseline Write a default baseline scenario config\n"
                  << "  --generate-active   Write a default active-control scenario config\n";
        return args.empty() ? 1 : 0;
    }

    if (has_flag(args, "--generate-baseline")) {
        const std::string path = generation_output_path(args, "--generate-baseline", "baseline_scenario.yaml");
        generate_scenario(path, false);
        return 0;
    }

    if (has_flag(args, "--generate-active")) {
        const std::string path = generation_output_path(args, "--generate-active", "active_scenario.yaml");
        generate_scenario(path, true);
        return 0;
    }

    const std::string config_path = first_positional_arg(args);
    if (config_path.empty()) {
        std::cerr << "hermes_bench: missing scenario config path" << std::endl;
        return 1;
    }

    const bool dry_run = has_flag(args, "--dry-run");
    std::string artifact_root = env_or("HERMES_ARTIFACT_ROOT", "artifacts");
    option_value(args, "--artifact-root", artifact_root);

    hermes::ScenarioConfigLoader loader;
    hermes::BenchmarkScenario scenario;
    std::string error;
    if (!loader.load(config_path, scenario, error)) {
        std::cerr << "hermes_bench: failed to load scenario: " << error << std::endl;
        return 1;
    }

    std::cout << "hermes_bench: loaded scenario from " << config_path << "\n\n";
    print_scenario(scenario);

    PlanValidation validation = validate_plan(scenario);

    std::string run_id;
    option_value(args, "--run-id", run_id);
    if (run_id.empty()) {
        run_id = default_run_id(scenario);
    }

    std::filesystem::path plan_path;
    std::filesystem::path scenario_snapshot_path;
    if (!write_plan_artifacts(
            scenario,
            validation,
            config_path,
            artifact_root,
            run_id,
            dry_run,
            plan_path,
            scenario_snapshot_path,
            error)) {
        std::cerr << "hermes_bench: failed to write plan artifact: " << error << std::endl;
        return 1;
    }

    std::cout << "\nPlan run id      : " << run_id
              << "\nPlan artifact    : " << plan_path.string()
              << "\nScenario snapshot: " << scenario_snapshot_path.string() << "\n";

    if (!validation.warnings.empty()) {
        std::cout << "\nPlan warnings:\n";
        for (const std::string& warning : validation.warnings) {
            std::cout << "  - " << warning << "\n";
        }
    }
    if (!validation.errors.empty()) {
        std::cerr << "\nPlan errors:\n";
        for (const std::string& plan_error : validation.errors) {
            std::cerr << "  - " << plan_error << "\n";
        }
        return 2;
    }

    if (dry_run) {
        std::cout << "\n[dry-run] No workloads launched. Plan artifacts were written for review.\n";
        return 0;
    }

    std::cout << "\n[hermes_bench] Workload launch is pending full harness implementation.\n"
              << "This pass wrote a benchmark plan artifact but did not start workloads.\n"
              << "See Phase 4 in roadmap.md for the full benchmark harness plan.\n";

    return 0;
}
