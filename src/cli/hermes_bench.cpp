// hermes_bench: Benchmark scenario launcher stub.
//
// Reads a scenario YAML config, validates it, writes a benchmark plan artifact,
// can launch a bounded benchmark workload run with a summary artifact, and
// can optionally launch hermesd plus hermes_replay around non-baseline runs.
//
// Current status (Phase 4 scaffold):
//   - Scenario config loading and validation are implemented.
//   - Plan artifacts are written under artifacts/bench/.
//   - Bounded workload launch and run summary artifacts are implemented.
//   - Optional Hermes daemon orchestration and replay summary capture are
//     implemented for non-baseline runtime modes.
//   - Rich benchmark evidence collection is still pending.
//
// Usage:
//   hermes_bench <scenario_config.yaml> [--dry-run] [--artifact-root artifacts] [--run-id id]
//                [--hermes-bin path] [--replay-bin path]
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
#include <utility>
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

struct EnvRecord {
    std::string name;
    bool had_value{false};
    std::string value;
};

struct HermesExecution {
    bool requested{false};
    bool launch_ok{false};
    bool replay_attempted{false};
    bool replay_ok{false};
    std::string hermes_bin;
    std::string replay_bin;
    std::string run_id;
    std::string run_directory;
    std::string launch_error;
    std::string replay_error;
    std::filesystem::path replay_summary_path;
    std::filesystem::path replay_csv_path;
    ChildProcess child;
    int replay_exit_code{0};
    bool replay_exit_code_valid{false};
};

struct ReplaySummarySnapshot {
    bool available{false};
    bool valid{false};
    uint64_t samples{0};
    uint64_t decisions{0};
    uint64_t actions{0};
    double peak_ups{0.0};
    double peak_risk_score{0.0};
    double peak_mem_full_avg10{0.0};
    double peak_vram_used_mb{0.0};
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
    return arg == "--artifact-root" || arg == "--run-id" ||
           arg == "--hermes-bin" || arg == "--replay-bin";
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

std::string platform_binary_name(const std::string& base_name) {
#ifdef _WIN32
    return base_name + ".exe";
#else
    return base_name;
#endif
}

std::string resolve_binary_path(
    const std::string& explicit_path,
    const std::filesystem::path& current_executable,
    const std::string& base_name) {
    if (!explicit_path.empty()) {
        return explicit_path;
    }

    std::filesystem::path candidate = current_executable.parent_path() / platform_binary_name(base_name);
    if (!candidate.empty() && std::filesystem::exists(candidate)) {
        return candidate.string();
    }

    return platform_binary_name(base_name);
}

uint64_t benchmark_window_ms(const hermes::BenchmarkScenario& scenario) {
    return static_cast<uint64_t>(std::max(0, scenario.warmup_s + scenario.measurement_s)) * 1000ull;
}

uint64_t hermes_loop_budget(const hermes::BenchmarkScenario& scenario) {
    const uint64_t window_ms = benchmark_window_ms(scenario);
    return std::max<uint64_t>(1, ((window_ms + 499ull) / 500ull) + 2ull);
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

bool runtime_mode_uses_hermes(const std::string& runtime_mode) {
    return runtime_mode != "baseline";
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return "";
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::string extract_json_string(const std::string& json, const std::string& key) {
    auto pos = json.find("\"" + key + "\":\"");
    if (pos != std::string::npos) {
        const std::size_t start = pos + key.size() + 4;
        const std::size_t end = json.find('"', start);
        if (end != std::string::npos) {
            return json.substr(start, end - start);
        }
    }

    const std::string spaced = "\"" + key + "\": \"";
    pos = json.find(spaced);
    if (pos == std::string::npos) {
        return "";
    }
    const std::size_t start = pos + spaced.size();
    const std::size_t end = json.find('"', start);
    return end == std::string::npos ? "" : json.substr(start, end - start);
}

double extract_json_double(const std::string& json, const std::string& key) {
    const auto pos = json.find("\"" + key + "\":");
    if (pos == std::string::npos) {
        return 0.0;
    }
    std::size_t start = pos + key.size() + 3;
    while (start < json.size() && (json[start] == ' ' || json[start] == '\t')) {
        ++start;
    }
    if (start >= json.size() || json[start] == '"') {
        return 0.0;
    }
    try {
        return std::stod(json.substr(start));
    } catch (...) {
        return 0.0;
    }
}

uint64_t extract_json_uint64(const std::string& json, const std::string& key) {
    const auto pos = json.find("\"" + key + "\":");
    if (pos == std::string::npos) {
        return 0;
    }
    std::size_t start = pos + key.size() + 3;
    while (start < json.size() && (json[start] == ' ' || json[start] == '\t')) {
        ++start;
    }
    if (start >= json.size() || json[start] == '"') {
        return 0;
    }
    try {
        return static_cast<uint64_t>(std::stoull(json.substr(start)));
    } catch (...) {
        return 0;
    }
}

bool extract_json_bool(const std::string& json, const std::string& key) {
    const auto pos = json.find("\"" + key + "\":");
    if (pos == std::string::npos) {
        return false;
    }
    std::size_t start = pos + key.size() + 3;
    while (start < json.size() && (json[start] == ' ' || json[start] == '\t')) {
        ++start;
    }
    return start < json.size() && json[start] == 't';
}

std::string shell_quote(const std::string& value) {
#ifdef _WIN32
    std::string out = "\"";
    for (const char ch : value) {
        if (ch == '"') {
            out += "\"\"";
        } else {
            out.push_back(ch);
        }
    }
    out += "\"";
    return out;
#else
    std::string out = "'";
    for (const char ch : value) {
        if (ch == '\'') {
            out += "'\\''";
        } else {
            out.push_back(ch);
        }
    }
    out += "'";
    return out;
#endif
}

bool get_process_env(const std::string& name, std::string& value) {
    const char* raw = std::getenv(name.c_str());
    if (raw == nullptr) {
        value.clear();
        return false;
    }
    value = raw;
    return true;
}

bool set_process_env(const std::string& name, const std::string& value) {
#ifdef _WIN32
    return SetEnvironmentVariableA(name.c_str(), value.c_str()) != FALSE;
#else
    return setenv(name.c_str(), value.c_str(), 1) == 0;
#endif
}

bool unset_process_env(const std::string& name) {
#ifdef _WIN32
    return SetEnvironmentVariableA(name.c_str(), nullptr) != FALSE;
#else
    return unsetenv(name.c_str()) == 0;
#endif
}

std::string join_quoted_command(
    const std::string& program,
    const std::vector<std::string>& args) {
    std::ostringstream oss;
    oss << shell_quote(program);
    for (const std::string& arg : args) {
        oss << " " << shell_quote(arg);
    }
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

std::vector<EnvRecord> apply_environment_overrides(
    const std::vector<std::pair<std::string, std::string>>& overrides,
    std::string& error) {
    std::vector<EnvRecord> previous;
    previous.reserve(overrides.size());

    for (const auto& [name, value] : overrides) {
        EnvRecord record;
        record.name = name;
        record.had_value = get_process_env(name, record.value);
        previous.push_back(record);
        if (!set_process_env(name, value)) {
            error = "failed to set environment variable " + name;
            break;
        }
    }

    if (!error.empty()) {
        for (auto it = previous.rbegin(); it != previous.rend(); ++it) {
            if (it->had_value) {
                set_process_env(it->name, it->value);
            } else {
                unset_process_env(it->name);
            }
        }
        previous.clear();
    }

    return previous;
}

void restore_environment_overrides(const std::vector<EnvRecord>& previous) {
    for (auto it = previous.rbegin(); it != previous.rend(); ++it) {
        if (it->had_value) {
            set_process_env(it->name, it->value);
        } else {
            unset_process_env(it->name);
        }
    }
}

PlanValidation validate_plan(const hermes::BenchmarkScenario& scenario) {
    PlanValidation validation;

    if (scenario.runtime_mode != "baseline" &&
        scenario.runtime_mode != "observe-only" &&
        scenario.runtime_mode != "advisory" &&
        scenario.runtime_mode != "active-control") {
        validation.errors.push_back(
            "runtime_mode must be baseline, observe-only, advisory, or active-control");
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

bool launch_child_command(const std::string& command, ChildProcess& child, std::string& error) {
    child = ChildProcess{};

#ifdef _WIN32
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process_info{};

    std::string command_line = "cmd.exe /C " + command;
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
        return false;
    }

    child.launched = true;
    child.running = true;
    child.start_ts_wall = wall_now_ms();
    child.process_info = process_info;
    return true;
#else
    const pid_t pid = fork();
    if (pid < 0) {
        error = "fork failed";
        return false;
    }

    if (pid == 0) {
        execl("/bin/sh", "sh", "-lc", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    child.launched = true;
    child.running = true;
    child.start_ts_wall = wall_now_ms();
    child.pid = pid;
    return true;
#endif
}

bool launch_program(
    const std::string& program,
    const std::vector<std::string>& args,
    ChildProcess& child,
    std::string& error) {
    child = ChildProcess{};

#ifdef _WIN32
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process_info{};

    std::string command_line = join_quoted_command(program, args);
    std::vector<char> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back('\0');

    const BOOL ok = CreateProcessA(
        program.c_str(),
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
        return false;
    }

    child.launched = true;
    child.running = true;
    child.start_ts_wall = wall_now_ms();
    child.process_info = process_info;
    return true;
#else
    const pid_t pid = fork();
    if (pid < 0) {
        error = "fork failed";
        return false;
    }

    if (pid == 0) {
        std::vector<char*> argv;
        argv.reserve(args.size() + 2);
        argv.push_back(const_cast<char*>(program.c_str()));
        for (const std::string& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        execvp(program.c_str(), argv.data());
        _exit(127);
    }

    child.launched = true;
    child.running = true;
    child.start_ts_wall = wall_now_ms();
    child.pid = pid;
    return true;
#endif
}

void finalize_child_exit(ChildProcess& child, int exit_code) {
    child.running = false;
    child.end_ts_wall = wall_now_ms();
    child.exit_code = exit_code;
    child.exit_code_valid = true;
}

enum class ChildPollResult {
    running,
    exited,
    wait_failed,
};

ChildPollResult poll_child_process(ChildProcess& child) {
    if (!child.launched || !child.running) {
        return ChildPollResult::exited;
    }

#ifdef _WIN32
    const DWORD wait_result = WaitForSingleObject(child.process_info.hProcess, 0);
    if (wait_result == WAIT_TIMEOUT) {
        return ChildPollResult::running;
    }

    DWORD exit_code = 0;
    if (GetExitCodeProcess(child.process_info.hProcess, &exit_code) == FALSE) {
        finalize_child_exit(child, 1);
        close_child_process(child);
        return ChildPollResult::wait_failed;
    }

    finalize_child_exit(child, static_cast<int>(exit_code));
    close_child_process(child);
    return ChildPollResult::exited;
#else
    int status = 0;
    const pid_t result = waitpid(child.pid, &status, WNOHANG);
    if (result == 0) {
        return ChildPollResult::running;
    }
    if (result < 0) {
        finalize_child_exit(child, 1);
        return ChildPollResult::wait_failed;
    }

    int exit_code = 0;
    if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        exit_code = 128 + WTERMSIG(status);
    } else {
        exit_code = 1;
    }
    finalize_child_exit(child, exit_code);
    return ChildPollResult::exited;
#endif
}

void stop_child_process(ChildProcess& child, int terminate_code = 1) {
    if (!child.launched || !child.running) {
        return;
    }

#ifdef _WIN32
    TerminateProcess(child.process_info.hProcess, static_cast<UINT>(terminate_code));
    WaitForSingleObject(child.process_info.hProcess, 2000);
    DWORD exit_code = static_cast<DWORD>(terminate_code);
    GetExitCodeProcess(child.process_info.hProcess, &exit_code);
    finalize_child_exit(child, static_cast<int>(exit_code));
    close_child_process(child);
#else
    kill(child.pid, SIGTERM);
    for (int i = 0; i < 20; ++i) {
        const ChildPollResult result = poll_child_process(child);
        if (result != ChildPollResult::running) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    kill(child.pid, SIGKILL);
    for (int i = 0; i < 20; ++i) {
        const ChildPollResult result = poll_child_process(child);
        if (result != ChildPollResult::running) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    finalize_child_exit(child, terminate_code);
#endif
}

bool wait_for_child_process(ChildProcess& child, uint64_t timeout_ms, std::string& error) {
    const uint64_t start_ms = wall_now_ms();
    while (child.running) {
        const ChildPollResult result = poll_child_process(child);
        if (result == ChildPollResult::exited) {
            return true;
        }
        if (result == ChildPollResult::wait_failed) {
            error = "process wait failed";
            return false;
        }
        if (timeout_ms > 0 && wall_now_ms() - start_ms >= timeout_ms) {
            error = "process wait timed out";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return true;
}

bool run_command_sync(
    const std::string& command,
    uint64_t timeout_ms,
    int& exit_code,
    bool& timed_out,
    std::string& error) {
    exit_code = 0;
    timed_out = false;

    ChildProcess child;
    if (!launch_child_command(command, child, error)) {
        exit_code = 1;
        return false;
    }

    if (!wait_for_child_process(child, timeout_ms, error)) {
        timed_out = (error == "process wait timed out");
        if (child.running) {
            stop_child_process(child);
        }
    }

    if (child.exit_code_valid) {
        exit_code = child.exit_code;
    } else if (timed_out) {
        exit_code = 1;
    }

    return !timed_out && child.exit_code_valid && child.exit_code == 0;
}

bool run_program_sync(
    const std::string& program,
    const std::vector<std::string>& args,
    uint64_t timeout_ms,
    int& exit_code,
    bool& timed_out,
    std::string& error) {
    exit_code = 0;
    timed_out = false;

    ChildProcess child;
    if (!launch_program(program, args, child, error)) {
        exit_code = 1;
        return false;
    }

    if (!wait_for_child_process(child, timeout_ms, error)) {
        timed_out = (error == "process wait timed out");
        if (child.running) {
            stop_child_process(child);
        }
    }

    if (child.exit_code_valid) {
        exit_code = child.exit_code;
    } else if (timed_out) {
        exit_code = 1;
    }

    return !timed_out && child.exit_code_valid && child.exit_code == 0;
}

ReplaySummarySnapshot load_replay_summary_snapshot(const std::filesystem::path& path) {
    ReplaySummarySnapshot snapshot;
    const std::string json = read_text_file(path);
    if (json.empty()) {
        return snapshot;
    }

    snapshot.available = true;
    snapshot.valid = extract_json_bool(json, "valid");
    snapshot.samples = extract_json_uint64(json, "samples");
    snapshot.decisions = extract_json_uint64(json, "decisions");
    snapshot.actions = extract_json_uint64(json, "actions");
    snapshot.peak_ups = extract_json_double(json, "ups");
    snapshot.peak_risk_score = extract_json_double(json, "risk_score");
    snapshot.peak_mem_full_avg10 = extract_json_double(json, "mem_full_avg10");
    snapshot.peak_vram_used_mb = extract_json_double(json, "vram_used_mb");
    return snapshot;
}

bool launch_workload(WorkloadExecution& execution, std::string& error) {
    execution.launch_attempted = true;
    execution.launch_error.clear();
    execution.exit_reason = "running";

    if (!launch_child_command(execution.workload.command, execution.child, error)) {
        execution.launch_error = error;
        execution.exit_reason = "launch-failed";
        return false;
    }

    execution.launch_ok = true;
    return true;
}

void poll_workload(WorkloadExecution& execution) {
    if (!execution.launch_ok || !execution.child.running) {
        return;
    }

    const ChildPollResult result = poll_child_process(execution.child);
    if (result == ChildPollResult::wait_failed) {
        execution.exit_reason = execution.timed_out ? "timed-out" : "wait-failed";
    } else if (result == ChildPollResult::exited) {
        execution.exit_reason = execution.timed_out ? "timed-out" : "completed";
    }
}

void stop_workload(WorkloadExecution& execution, const std::string& reason) {
    if (!execution.launch_ok || !execution.child.running) {
        return;
    }

    execution.forced_stop = true;
    execution.timed_out = (reason == "timed-out");
    stop_child_process(execution.child);
    execution.exit_reason = reason;
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

    const uint64_t run_window_ms = benchmark_window_ms(scenario);

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

bool start_hermes_execution(
    const hermes::BenchmarkScenario& scenario,
    const std::string& artifact_root,
    const std::string& bench_run_id,
    const std::string& hermes_bin,
    HermesExecution& execution,
    std::string& error) {
    execution.requested = true;
    execution.hermes_bin = hermes_bin;
    execution.run_id = bench_run_id + "-hermes";
    execution.run_directory =
        (std::filesystem::path(artifact_root) / "logs" / execution.run_id).string();

    const std::vector<std::pair<std::string, std::string>> overrides = {
        {"HERMES_RUN_ID", execution.run_id},
        {"HERMES_SCENARIO", scenario.name},
        {"HERMES_RUNTIME_MODE", scenario.runtime_mode},
        {"HERMES_ARTIFACT_ROOT", artifact_root},
        {"HERMES_MAX_LOOPS", std::to_string(hermes_loop_budget(scenario))},
    };

    std::string env_error;
    const std::vector<EnvRecord> previous = apply_environment_overrides(overrides, env_error);
    if (!env_error.empty()) {
        error = env_error;
        execution.launch_error = env_error;
        return false;
    }

    const bool launched = launch_program(hermes_bin, {}, execution.child, error);
    restore_environment_overrides(previous);

    if (!launched) {
        execution.launch_error = error;
        return false;
    }

    execution.launch_ok = true;
    return true;
}

bool run_hermes_replay(
    HermesExecution& execution,
    const std::string& artifact_root,
    ReplaySummarySnapshot& snapshot,
    std::string& error) {
    execution.replay_attempted = true;
    execution.replay_summary_path =
        std::filesystem::path(execution.run_directory) / "replay_summary.json";
    execution.replay_csv_path =
        std::filesystem::path(execution.run_directory) / "summary.csv";

    if (!std::filesystem::exists(execution.run_directory)) {
        error = "Hermes run directory was not written: " + execution.run_directory;
        execution.replay_error = error;
        return false;
    }

    bool timed_out = false;
    int exit_code = 0;
    const bool replay_ok = run_program_sync(
        execution.replay_bin,
        {execution.run_directory, artifact_root},
        20000,
        exit_code,
        timed_out,
        error);
    execution.replay_exit_code = exit_code;
    execution.replay_exit_code_valid = true;

    if (std::filesystem::exists(execution.replay_summary_path)) {
        snapshot = load_replay_summary_snapshot(execution.replay_summary_path);
    }

    if (!std::filesystem::exists(execution.replay_summary_path)) {
        if (error.empty()) {
            error = "Hermes replay summary was not written: " +
                    execution.replay_summary_path.string();
        }
        execution.replay_error = error;
        return false;
    }

    if (!std::filesystem::exists(execution.replay_csv_path)) {
        if (error.empty()) {
            error = "Hermes replay CSV was not written: " +
                    execution.replay_csv_path.string();
        }
        execution.replay_error = error;
        return false;
    }

    if (!replay_ok) {
        if (error.empty()) {
            if (timed_out) {
                error = "Hermes replay timed out";
            } else {
                error = "Hermes replay exited with code " + std::to_string(exit_code);
            }
        }
        execution.replay_error = error;
        return false;
    }

    execution.replay_ok = true;
    return true;
}

bool write_execution_summary(
    const hermes::BenchmarkScenario& scenario,
    const PlanValidation& validation,
    const ExecutionSummary& summary,
    const std::vector<WorkloadExecution>& executions,
    const HermesExecution& hermes_execution,
    const ReplaySummarySnapshot& replay_snapshot,
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
        << "  \"hermes\": {\n"
        << "    \"requested\": " << bool_json(hermes_execution.requested) << ",\n"
        << "    \"launch_ok\": " << bool_json(hermes_execution.launch_ok) << ",\n"
        << "    \"binary\": \"" << json_escape(hermes_execution.hermes_bin) << "\",\n"
        << "    \"run_id\": \"" << json_escape(hermes_execution.run_id) << "\",\n"
        << "    \"run_directory\": \"" << json_escape(hermes_execution.run_directory) << "\",\n"
        << "    \"exit_code\": "
        << (hermes_execution.child.exit_code_valid
                ? std::to_string(hermes_execution.child.exit_code)
                : "null")
        << ",\n"
        << "    \"launch_error\": \"" << json_escape(hermes_execution.launch_error) << "\",\n"
        << "    \"replay_attempted\": " << bool_json(hermes_execution.replay_attempted) << ",\n"
        << "    \"replay_ok\": " << bool_json(hermes_execution.replay_ok) << ",\n"
        << "    \"replay_binary\": \"" << json_escape(hermes_execution.replay_bin) << "\",\n"
        << "    \"replay_exit_code\": "
        << (hermes_execution.replay_exit_code_valid
                ? std::to_string(hermes_execution.replay_exit_code)
                : "null")
        << ",\n"
        << "    \"replay_summary\": \"" << json_escape(hermes_execution.replay_summary_path.string()) << "\",\n"
        << "    \"replay_csv\": \"" << json_escape(hermes_execution.replay_csv_path.string()) << "\",\n"
        << "    \"replay_error\": \"" << json_escape(hermes_execution.replay_error) << "\"\n"
        << "  },\n"
        << "  \"replay_snapshot\": {\n"
        << "    \"available\": " << bool_json(replay_snapshot.available) << ",\n"
        << "    \"valid\": " << bool_json(replay_snapshot.valid) << ",\n"
        << "    \"samples\": " << replay_snapshot.samples << ",\n"
        << "    \"decisions\": " << replay_snapshot.decisions << ",\n"
        << "    \"actions\": " << replay_snapshot.actions << ",\n"
        << "    \"peak_ups\": " << replay_snapshot.peak_ups << ",\n"
        << "    \"peak_risk_score\": " << replay_snapshot.peak_risk_score << ",\n"
        << "    \"peak_mem_full_avg10\": " << replay_snapshot.peak_mem_full_avg10 << ",\n"
        << "    \"peak_vram_used_mb\": " << replay_snapshot.peak_vram_used_mb << "\n"
        << "  },\n"
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
                  << "  --hermes-bin PATH   Optional hermesd path for observe/advisory/active runs\n"
                  << "  --replay-bin PATH   Optional hermes_replay path for observe/advisory/active runs\n"
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

    const std::filesystem::path current_executable =
        argc > 0 ? std::filesystem::path(argv[0]) : std::filesystem::path();
    std::string hermes_bin_override;
    std::string replay_bin_override;
    option_value(args, "--hermes-bin", hermes_bin_override);
    option_value(args, "--replay-bin", replay_bin_override);

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

    HermesExecution hermes_execution;
    ReplaySummarySnapshot replay_snapshot;
    bool overall_ok = true;

    if (runtime_mode_uses_hermes(scenario.runtime_mode)) {
        hermes_execution.hermes_bin =
            resolve_binary_path(hermes_bin_override, current_executable, "hermesd");
        hermes_execution.replay_bin =
            resolve_binary_path(replay_bin_override, current_executable, "hermes_replay");

        if (!start_hermes_execution(
                scenario,
                artifact_root,
                run_id,
                hermes_execution.hermes_bin,
                hermes_execution,
                error)) {
            std::cerr << "hermes_bench: failed to launch Hermes daemon: " << error << std::endl;
            return 1;
        }

        std::cout << "Hermes binary    : " << hermes_execution.hermes_bin
                  << "\nReplay binary    : " << hermes_execution.replay_bin
                  << "\nHermes run id    : " << hermes_execution.run_id
                  << "\nHermes run dir   : " << hermes_execution.run_directory << "\n";
    } else {
        std::cout << "Hermes runtime   : disabled for baseline mode\n";
    }

    std::vector<WorkloadExecution> executions;
    executions.reserve(scenario.workloads.size());
    for (const hermes::BenchmarkWorkload& workload : scenario.workloads) {
        WorkloadExecution execution;
        execution.workload = workload;
        executions.push_back(execution);
    }

    ExecutionSummary summary = execute_benchmark(scenario, executions);

    if (hermes_execution.launch_ok) {
        std::string hermes_wait_error;
        if (!wait_for_child_process(hermes_execution.child, 4000, hermes_wait_error)) {
            if (hermes_execution.child.running) {
                stop_child_process(hermes_execution.child);
                summary.notes.push_back("Hermes daemon exceeded shutdown wait and was terminated");
            } else {
                summary.notes.push_back("Hermes daemon wait error: " + hermes_wait_error);
            }
            overall_ok = false;
        }

        if (hermes_execution.child.exit_code_valid && hermes_execution.child.exit_code != 0) {
            summary.notes.push_back(
                "Hermes daemon exited with code " +
                std::to_string(hermes_execution.child.exit_code));
            overall_ok = false;
        }

        if (!run_hermes_replay(hermes_execution, artifact_root, replay_snapshot, error)) {
            summary.notes.push_back("Hermes replay failed: " + error);
            overall_ok = false;
        }
    }

    std::filesystem::path summary_path;
    if (!write_execution_summary(
            scenario,
            validation,
            summary,
            executions,
            hermes_execution,
            replay_snapshot,
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
              << "\nRun duration ms  : " << summary.duration_ms;

    if (hermes_execution.requested) {
        std::cout << "\nHermes exit code : "
                  << (hermes_execution.child.exit_code_valid
                          ? std::to_string(hermes_execution.child.exit_code)
                          : "n/a")
                  << "\nReplay summary   : " << hermes_execution.replay_summary_path.string()
                  << "\nReplay valid     : " << (replay_snapshot.valid ? "true" : "false")
                  << "\nReplay samples   : " << replay_snapshot.samples
                  << "\nReplay decisions : " << replay_snapshot.decisions
                  << "\nReplay actions   : " << replay_snapshot.actions;
    }
    std::cout << "\n";

    return overall_ok ? 0 : 1;
}
