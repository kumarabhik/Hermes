// hermes_bench: Benchmark scenario launcher stub.
//
// Reads a scenario YAML config, validates it, writes a benchmark plan artifact,
// and can launch a bounded benchmark workload run with a summary artifact.
//
// Current status (Phase 4 scaffold):
//   - Scenario config loading and validation are implemented.
//   - Plan artifacts are written under artifacts/bench/.
//   - Bounded workload launch and run summary artifacts are implemented.
//   - Full benchmark harness with Hermes orchestration, live metric capture,
//     and benchmark evidence collection is still pending.
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
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

struct PlanValidation {
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    std::size_t foreground_workloads{0};
    std::size_t background_workloads{0};
    uint64_t total_workload_seconds{0};
};

struct ChildProcess {
    bool launched{false};
    bool running{false};
    uint64_t start_ts_wall{0};
    uint64_t end_ts_wall{0};
    int exit_code{0};
    bool exit_code_valid{false};

#ifdef _WIN32
    PROCESS_INFORMATION process_info{};
#else
    pid_t pid{-1};
#endif
};

struct WorkloadExecution {
    hermes::BenchmarkWorkload workload;
    ChildProcess child;
    bool launch_attempted{false};
    bool launch_ok{false};
    bool timed_out{false};
    bool forced_stop{false};
    std::string launch_error;
    std::string exit_reason{"not-started"};
};

struct ExecutionSummary {
    uint64_t started_ts_wall{0};
    uint64_t finished_ts_wall{0};
    uint64_t duration_ms{0};
    std::size_t launched{0};
    std::size_t launch_failed{0};
    std::size_t jobs_completed{0};
    std::size_t timed_out{0};
    std::size_t exited_nonzero{0};
    std::size_t still_running{0};
    std::vector<std::string> notes;
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

std::string bool_json(bool value) {
    return value ? "true" : "false";
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

void close_child_process(ChildProcess& child) {
#ifdef _WIN32
    if (child.process_info.hThread != nullptr) {
        CloseHandle(child.process_info.hThread);
        child.process_info.hThread = nullptr;
    }
    if (child.process_info.hProcess != nullptr) {
        CloseHandle(child.process_info.hProcess);
        child.process_info.hProcess = nullptr;
    }
#else
    (void)child;
#endif
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

bool launch_workload(WorkloadExecution& execution, std::string& error) {
    execution.launch_attempted = true;
    execution.launch_error.clear();
    execution.exit_reason = "running";

#ifdef _WIN32
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process_info{};

    std::string command_line = "cmd.exe /C " + execution.workload.command;
    std::vector<char> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back('\0');

    const BOOL ok = CreateProcessA(
        nullptr,
        mutable_command.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startup,
        &process_info);
    if (ok == FALSE) {
        std::ostringstream oss;
        oss << "CreateProcess failed with error " << GetLastError();
        error = oss.str();
        execution.launch_error = error;
        execution.exit_reason = "launch-failed";
        return false;
    }

    execution.child.launched = true;
    execution.child.running = true;
    execution.child.start_ts_wall = wall_now_ms();
    execution.child.process_info = process_info;
    execution.launch_ok = true;
    return true;
#else
    const pid_t pid = fork();
    if (pid < 0) {
        error = "fork failed";
        execution.launch_error = error;
        execution.exit_reason = "launch-failed";
        return false;
    }

    if (pid == 0) {
        execl("/bin/sh", "sh", "-lc", execution.workload.command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    execution.child.launched = true;
    execution.child.running = true;
    execution.child.start_ts_wall = wall_now_ms();
    execution.child.pid = pid;
    execution.launch_ok = true;
    return true;
#endif
}

void finalize_exit(WorkloadExecution& execution, int exit_code, const std::string& reason) {
    execution.child.running = false;
    execution.child.end_ts_wall = wall_now_ms();
    execution.child.exit_code = exit_code;
    execution.child.exit_code_valid = true;
    execution.exit_reason = reason;
}

void poll_workload(WorkloadExecution& execution) {
    if (!execution.launch_ok || !execution.child.running) {
        return;
    }

#ifdef _WIN32
    const DWORD wait_result = WaitForSingleObject(execution.child.process_info.hProcess, 0);
    if (wait_result == WAIT_TIMEOUT) {
        return;
    }

    DWORD exit_code = 0;
    if (GetExitCodeProcess(execution.child.process_info.hProcess, &exit_code) == FALSE) {
        finalize_exit(execution, 1, execution.timed_out ? "timed-out" : "unknown");
    } else {
        finalize_exit(
            execution,
            static_cast<int>(exit_code),
            execution.timed_out ? "timed-out" : "completed");
    }
    close_child_process(execution.child);
#else
    int status = 0;
    const pid_t result = waitpid(execution.child.pid, &status, WNOHANG);
    if (result == 0) {
        return;
    }
    if (result < 0) {
        finalize_exit(execution, 1, execution.timed_out ? "timed-out" : "wait-failed");
        return;
    }

    int exit_code = 0;
    if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        exit_code = 128 + WTERMSIG(status);
    } else {
        exit_code = 1;
    }
    finalize_exit(
        execution,
        exit_code,
        execution.timed_out ? "timed-out" : "completed");
#endif
}

void stop_workload(WorkloadExecution& execution, const std::string& reason) {
    if (!execution.launch_ok || !execution.child.running) {
        return;
    }

    execution.forced_stop = true;
    execution.timed_out = (reason == "timed-out");

#ifdef _WIN32
    TerminateProcess(execution.child.process_info.hProcess, 1);
    WaitForSingleObject(execution.child.process_info.hProcess, 2000);
    DWORD exit_code = 1;
    GetExitCodeProcess(execution.child.process_info.hProcess, &exit_code);
    finalize_exit(execution, static_cast<int>(exit_code), reason);
    close_child_process(execution.child);
#else
    kill(execution.child.pid, SIGTERM);
    for (int i = 0; i < 20; ++i) {
        poll_workload(execution);
        if (!execution.child.running) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    kill(execution.child.pid, SIGKILL);
    for (int i = 0; i < 20; ++i) {
        poll_workload(execution);
        if (!execution.child.running) {
            execution.exit_reason = reason;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    finalize_exit(execution, 1, reason);
#endif
}

ExecutionSummary execute_benchmark(
    const hermes::BenchmarkScenario& scenario,
    std::vector<WorkloadExecution>& executions) {
    ExecutionSummary summary;
    summary.started_ts_wall = wall_now_ms();

    std::string error;
    for (WorkloadExecution& execution : executions) {
        if (!launch_workload(execution, error)) {
            summary.launch_failed += 1;
            summary.notes.push_back(
                "launch failed for " + execution.workload.name + ": " + execution.launch_error);
        } else {
            summary.launched += 1;
        }
    }

    const uint64_t run_window_ms =
        static_cast<uint64_t>(std::max(0, scenario.warmup_s + scenario.measurement_s)) * 1000ull;

    while (true) {
        bool any_running = false;
        const uint64_t now = wall_now_ms();

        for (WorkloadExecution& execution : executions) {
            poll_workload(execution);
            if (!execution.launch_ok || !execution.child.running) {
                continue;
            }

            any_running = true;
            const uint64_t workload_runtime_ms = now - execution.child.start_ts_wall;
            const uint64_t workload_limit_ms =
                static_cast<uint64_t>(std::max(1, execution.workload.duration_s)) * 1000ull;
            if (workload_runtime_ms >= workload_limit_ms) {
                stop_workload(execution, "timed-out");
            }
        }

        if (!any_running) {
            break;
        }

        if (run_window_ms > 0 && now - summary.started_ts_wall >= run_window_ms) {
            for (WorkloadExecution& execution : executions) {
                if (execution.launch_ok && execution.child.running) {
                    stop_workload(execution, "timed-out");
                }
            }
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    for (WorkloadExecution& execution : executions) {
        poll_workload(execution);
        if (!execution.launch_ok) {
            continue;
        }
        if (execution.child.running) {
            stop_workload(execution, "timed-out");
        }
        if (execution.timed_out) {
            summary.timed_out += 1;
        } else if (execution.child.exit_code_valid && execution.child.exit_code == 0) {
            summary.jobs_completed += 1;
        } else if (execution.child.exit_code_valid) {
            summary.exited_nonzero += 1;
        }
        if (execution.child.running) {
            summary.still_running += 1;
        }
    }

    summary.finished_ts_wall = wall_now_ms();
    summary.duration_ms = summary.finished_ts_wall - summary.started_ts_wall;
    return summary;
}

bool write_execution_summary(
    const hermes::BenchmarkScenario& scenario,
    const PlanValidation& validation,
    const ExecutionSummary& summary,
    const std::vector<WorkloadExecution>& executions,
    const std::string& artifact_root,
    const std::string& run_id,
    const std::filesystem::path& plan_path,
    const std::filesystem::path& scenario_snapshot_path,
    std::filesystem::path& output_path,
    std::string& error) {
    try {
        const std::filesystem::path bench_dir = std::filesystem::path(artifact_root) / "bench";
        std::filesystem::create_directories(bench_dir);
        output_path = bench_dir / (run_id + "-summary.json");
    } catch (const std::exception& ex) {
        error = std::string("failed to create benchmark summary directory: ") + ex.what();
        return false;
    }

    std::vector<std::string> notes = summary.notes;
    for (const std::string& warning : validation.warnings) {
        notes.push_back("plan warning: " + warning);
    }

    std::ofstream out(output_path);
    if (!out.is_open()) {
        error = "failed to open benchmark summary artifact: " + output_path.string();
        return false;
    }

    out << "{\n"
        << "  \"run_id\": \"" << json_escape(run_id) << "\",\n"
        << "  \"scenario\": \"" << json_escape(scenario.name) << "\",\n"
        << "  \"runtime_mode\": \"" << json_escape(scenario.runtime_mode) << "\",\n"
        << "  \"plan_artifact\": \"" << json_escape(plan_path.string()) << "\",\n"
        << "  \"scenario_snapshot\": \"" << json_escape(scenario_snapshot_path.string()) << "\",\n"
        << "  \"warmup_s\": " << scenario.warmup_s << ",\n"
        << "  \"measurement_s\": " << scenario.measurement_s << ",\n"
        << "  \"repeat_count\": " << scenario.repeat_count << ",\n"
        << "  \"duration_ms\": " << summary.duration_ms << ",\n"
        << "  \"started_ts_wall\": " << summary.started_ts_wall << ",\n"
        << "  \"finished_ts_wall\": " << summary.finished_ts_wall << ",\n"
        << "  \"workload_count\": " << scenario.workloads.size() << ",\n"
        << "  \"foreground_workloads\": " << validation.foreground_workloads << ",\n"
        << "  \"background_workloads\": " << validation.background_workloads << ",\n"
        << "  \"planned_workload_seconds\": " << validation.total_workload_seconds << ",\n"
        << "  \"launched\": " << summary.launched << ",\n"
        << "  \"launch_failed\": " << summary.launch_failed << ",\n"
        << "  \"jobs_completed\": " << summary.jobs_completed << ",\n"
        << "  \"timed_out\": " << summary.timed_out << ",\n"
        << "  \"exited_nonzero\": " << summary.exited_nonzero << ",\n"
        << "  \"still_running\": " << summary.still_running << ",\n"
        << "  \"notes\": " << string_array_json(notes) << ",\n"
        << "  \"workloads\": [\n";

    for (std::size_t i = 0; i < executions.size(); ++i) {
        const WorkloadExecution& execution = executions[i];
        out << "    {"
            << "\"name\":\"" << json_escape(execution.workload.name) << "\","
            << "\"command\":\"" << json_escape(execution.workload.command) << "\","
            << "\"foreground\":" << bool_json(execution.workload.foreground) << ","
            << "\"background\":" << bool_json(execution.workload.background) << ","
            << "\"duration_s\":" << execution.workload.duration_s << ","
            << "\"launch_ok\":" << bool_json(execution.launch_ok) << ","
            << "\"timed_out\":" << bool_json(execution.timed_out) << ","
            << "\"forced_stop\":" << bool_json(execution.forced_stop) << ","
            << "\"exit_code\":"
            << (execution.child.exit_code_valid ? std::to_string(execution.child.exit_code) : "null") << ","
            << "\"start_ts_wall\":" << execution.child.start_ts_wall << ","
            << "\"end_ts_wall\":" << execution.child.end_ts_wall << ","
            << "\"launch_error\":\"" << json_escape(execution.launch_error) << "\","
            << "\"exit_reason\":\"" << json_escape(execution.exit_reason) << "\""
            << "}";
        if (i + 1 < executions.size()) {
            out << ",";
        }
        out << "\n";
    }

    out << "  ]\n"
        << "}\n";

    return out.good();
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

    std::vector<WorkloadExecution> executions;
    executions.reserve(scenario.workloads.size());
    for (const hermes::BenchmarkWorkload& workload : scenario.workloads) {
        WorkloadExecution execution;
        execution.workload = workload;
        executions.push_back(execution);
    }

    const ExecutionSummary summary = execute_benchmark(scenario, executions);
    std::filesystem::path summary_path;
    if (!write_execution_summary(
            scenario,
            validation,
            summary,
            executions,
            artifact_root,
            run_id,
            plan_path,
            scenario_snapshot_path,
            summary_path,
            error)) {
        std::cerr << "hermes_bench: failed to write execution summary: " << error << std::endl;
        return 1;
    }

    std::cout << "\nRun summary      : " << summary_path.string()
              << "\nLaunched         : " << summary.launched
              << "\nLaunch failed    : " << summary.launch_failed
              << "\nJobs completed   : " << summary.jobs_completed
              << "\nTimed out        : " << summary.timed_out
              << "\nExited nonzero   : " << summary.exited_nonzero
              << "\nRun duration ms  : " << summary.duration_ms << "\n";

    return 0;
}
