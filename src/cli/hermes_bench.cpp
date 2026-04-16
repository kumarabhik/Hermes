// hermes_bench: Benchmark scenario launcher.
//
// Reads a scenario YAML config, validates it, writes a benchmark plan artifact,
// launches bounded workloads with optional Hermes daemon orchestration and
// replay summary capture, and writes enriched benchmark summary artifacts.
//
// Summary artifacts include: completion_rate, intervention_count (from replay
// snapshot actions), oom_count (placeholder), degraded_behavior flag, and
// comparison-ready fields for baseline vs observe-only vs active-control runs.
//
// Usage:
//   hermes_bench <scenario_config.yaml> [--dry-run] [--artifact-root artifacts] [--run-id id]
//                [--hermes-bin path] [--replay-bin path] [--runs N]
//   hermes_bench --generate-baseline [output.yaml]
//   hermes_bench --generate-active [output.yaml]
//   hermes_bench --generate-oom-stress [output.yaml]
//   hermes_bench --smoke-all [--dry-run] [--artifact-root DIR] [--hermes-bin PATH]
//                            [--replay-bin PATH] [--compare-bin PATH]

#include "hermes/runtime/scenario_config.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <iomanip>
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

struct MultiRunStats {
    std::vector<double> fg_latency_ms;   // foreground workload durations across runs
    std::size_t total_oom_kills{0};      // workloads killed with SIGKILL (exit 137)
    std::size_t total_runs{0};
    std::size_t total_launched{0};
    std::size_t total_completed{0};

    void record_run(const std::vector<WorkloadExecution>& executions) {
        ++total_runs;
        for (const WorkloadExecution& e : executions) {
            if (!e.launch_ok) continue;
            ++total_launched;
            const uint64_t dur = (e.child.end_ts_wall > e.child.start_ts_wall)
                ? e.child.end_ts_wall - e.child.start_ts_wall : 0u;
            if (e.workload.foreground) {
                fg_latency_ms.push_back(static_cast<double>(dur));
            }
            if (e.child.exit_code_valid && e.child.exit_code == 0 && !e.timed_out) {
                ++total_completed;
            }
            // exit code 137 = SIGKILL, typically OOM kill on Linux
            if (e.child.exit_code_valid && e.child.exit_code == 137) {
                ++total_oom_kills;
            }
        }
    }

    double percentile(double pct) const {
        if (fg_latency_ms.empty()) return 0.0;
        std::vector<double> sorted = fg_latency_ms;
        std::sort(sorted.begin(), sorted.end());
        const std::size_t idx = static_cast<std::size_t>(pct / 100.0 * (sorted.size() - 1));
        return sorted[std::min(idx, sorted.size() - 1)];
    }
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
           arg == "--hermes-bin" || arg == "--replay-bin" || arg == "--runs" ||
           arg == "--compare-bin" || arg == "--delta-vs";
}

// ---------------------------------------------------------------------------
// --delta-vs helpers: load a summary JSON field and print a delta table.
// ---------------------------------------------------------------------------

double extract_json_double(const std::string& content, const std::string& key,
                           double fallback = -1.0) {
    const std::string search = "\"" + key + "\":";
    const auto pos = content.find(search);
    if (pos == std::string::npos) return fallback;
    const auto start = pos + search.size();
    // Skip whitespace
    const auto val_start = content.find_first_not_of(" \t", start);
    if (val_start == std::string::npos) return fallback;
    if (content[val_start] == 'n') return fallback; // null
    try { return std::stod(content.substr(val_start)); } catch (...) { return fallback; }
}

struct SummarySnapshot {
    bool loaded{false};
    double p95_latency_ms{-1.0};
    double completion_rate{-1.0};
    double oom_count{-1.0};
    double intervention_count{-1.0};
    std::string run_id;
};

SummarySnapshot load_summary_snapshot(const std::string& path) {
    SummarySnapshot snap;
    std::ifstream f(path);
    if (!f.is_open()) return snap;
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    snap.p95_latency_ms    = extract_json_double(content, "p95_latency_ms");
    snap.completion_rate   = extract_json_double(content, "completion_rate");
    snap.oom_count         = extract_json_double(content, "oom_count");
    snap.intervention_count= extract_json_double(content, "intervention_count");
    // Extract run_id string
    const std::string rid_search = "\"run_id\":\"";
    const auto rpos = content.find(rid_search);
    if (rpos != std::string::npos) {
        const auto s = rpos + rid_search.size();
        const auto e = content.find('"', s);
        if (e != std::string::npos) snap.run_id = content.substr(s, e - s);
    }
    snap.loaded = (snap.p95_latency_ms >= 0.0 || snap.completion_rate >= 0.0);
    return snap;
}

void print_delta_table(
    const SummarySnapshot& baseline,
    const SummarySnapshot& current,
    const std::string& current_run_id) {

    const std::string sep(64, '=');
    std::cout << "\n" << sep << "\n";
    std::cout << "Delta vs Baseline  (current: " << current_run_id
              << "  baseline: " << baseline.run_id << ")\n";
    std::cout << sep << "\n";

    struct Row {
        std::string metric;
        double base;
        double curr;
        bool lower_is_better;
    };
    const std::vector<Row> rows = {
        {"p95_latency_ms",     baseline.p95_latency_ms,     current.p95_latency_ms,     true},
        {"completion_rate",    baseline.completion_rate,     current.completion_rate,     false},
        {"oom_count",          baseline.oom_count,           current.oom_count,           true},
        {"intervention_count", baseline.intervention_count,  current.intervention_count,  true},
    };

    std::cout << "  " << std::left << std::setw(24) << "Metric"
              << std::right << std::setw(12) << "Baseline"
              << std::setw(12) << "Current"
              << std::setw(12) << "Delta"
              << "  Verdict\n";
    std::cout << "  " << std::string(22, '-') << "  "
              << std::string(10, '-') << "  "
              << std::string(10, '-') << "  "
              << std::string(10, '-') << "  -------\n";

    std::cout << std::fixed << std::setprecision(2);
    for (const auto& row : rows) {
        if (row.base < 0.0 || row.curr < 0.0) {
            std::cout << "  " << std::left << std::setw(24) << row.metric
                      << "  " << std::right << std::setw(10) << "N/A"
                      << "  " << std::setw(10) << "N/A"
                      << "  " << std::setw(10) << "N/A"
                      << "  N/A\n";
            continue;
        }
        const double delta = row.curr - row.base;
        const bool improved = row.lower_is_better ? (delta < 0.0) : (delta > 0.0);
        const bool neutral  = delta == 0.0;
        const std::string verdict = neutral ? "=" : (improved ? "BETTER" : "WORSE");
        std::string delta_str = (delta >= 0.0 ? "+" : "") + std::to_string(delta).substr(
            0, std::to_string(delta).find('.') + 3);
        std::cout << "  " << std::left << std::setw(24) << row.metric
                  << "  " << std::right << std::setw(10) << row.base
                  << "  " << std::setw(10) << row.curr
                  << "  " << std::setw(10) << delta_str
                  << "  " << verdict << "\n";
    }
    std::cout << sep << "\n";
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

// Compute p95 completion latency (ms) across foreground workloads in one run.
// Returns -1.0 when no valid foreground timing is available.
double compute_fg_p95_ms(const std::vector<WorkloadExecution>& executions) {
    std::vector<double> durations;
    for (const WorkloadExecution& e : executions) {
        if (!e.workload.foreground) continue;
        if (!e.launch_ok) continue;
        if (e.child.start_ts_wall == 0 || e.child.end_ts_wall < e.child.start_ts_wall) continue;
        durations.push_back(static_cast<double>(e.child.end_ts_wall - e.child.start_ts_wall));
    }
    if (durations.empty()) return -1.0;
    std::sort(durations.begin(), durations.end());
    const double idx   = 0.95 * static_cast<double>(durations.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(idx);
    const std::size_t hi = (lo + 1 < durations.size()) ? lo + 1 : lo;
    const double frac  = idx - static_cast<double>(lo);
    return durations[lo] * (1.0 - frac) + durations[hi] * frac;
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
    double p95_latency_ms,         // -1.0 = not measured
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

    // Derived comparison-friendly fields.
    const double completion_rate =
        (summary.launched > 0)
            ? static_cast<double>(summary.jobs_completed) / static_cast<double>(summary.launched)
            : 0.0;
    // intervention_count: number of actions Hermes took this run (from replay snapshot).
    const uint64_t intervention_count = replay_snapshot.available ? replay_snapshot.actions : 0u;
    // oom_count: exit code 137 means SIGKILL, the typical Linux OOM-kill signal.
    uint64_t oom_count_real = 0;
    for (const WorkloadExecution& e : executions) {
        if (e.child.exit_code_valid && e.child.exit_code == 137) {
            ++oom_count_real;
        }
    }
    // degraded_behavior: true when completion rate is below scenario target or nonzero exits occurred.
    const double min_completion = scenario.expected_min_job_completion_rate > 0.0
        ? scenario.expected_min_job_completion_rate
        : 0.8;
    const bool degraded_behavior =
        (completion_rate < min_completion) || (summary.exited_nonzero > 0) || (oom_count_real > 0);

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
        << "  \"completion_rate\": " << completion_rate << ",\n"
        << "  \"intervention_count\": " << intervention_count << ",\n"
        << "  \"oom_count\": " << oom_count_real << ",\n"
        << "  \"degraded_behavior\": " << bool_json(degraded_behavior) << ",\n"
        << "  \"p95_latency_ms\": "
        << (p95_latency_ms >= 0.0 ? std::to_string(p95_latency_ms) : "null") << ",\n"
        << "  \"latency_target_ms\": "
        << (scenario.expected_max_p95_latency_ms > 0.0
                ? std::to_string(scenario.expected_max_p95_latency_ms)
                : "null") << ",\n"
        << "  \"latency_target_met\": "
        << (p95_latency_ms >= 0.0 && scenario.expected_max_p95_latency_ms > 0.0
                ? bool_json(p95_latency_ms <= scenario.expected_max_p95_latency_ms)
                : "null") << ",\n"
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

bool write_latency_summary(
    const std::string& artifact_root,
    const std::string& base_run_id,
    const MultiRunStats& stats,
    std::string& error) {
    if (stats.fg_latency_ms.empty()) return true;

    try {
        const std::filesystem::path bench_dir = std::filesystem::path(artifact_root) / "bench";
        std::filesystem::create_directories(bench_dir);
        const std::filesystem::path out_path = bench_dir / (base_run_id + "-latency.json");

        std::ofstream out(out_path);
        if (!out.is_open()) {
            error = "failed to open latency summary: " + out_path.string();
            return false;
        }

        out << std::fixed << std::setprecision(2);
        out << "{\n"
            << "  \"run_id\": \"" << json_escape(base_run_id) << "\",\n"
            << "  \"total_runs\": " << stats.total_runs << ",\n"
            << "  \"fg_latency_samples\": " << stats.fg_latency_ms.size() << ",\n"
            << "  \"p50_latency_ms\": " << stats.percentile(50.0) << ",\n"
            << "  \"p95_latency_ms\": " << stats.percentile(95.0) << ",\n"
            << "  \"p99_latency_ms\": " << stats.percentile(99.0) << ",\n"
            << "  \"max_latency_ms\": " << stats.percentile(100.0) << ",\n"
            << "  \"min_latency_ms\": " << stats.percentile(0.0) << ",\n"
            << "  \"total_launched\": " << stats.total_launched << ",\n"
            << "  \"total_completed\": " << stats.total_completed << ",\n"
            << "  \"oom_kills\": " << stats.total_oom_kills << "\n"
            << "}\n";
        return out.good();
    } catch (const std::exception& ex) {
        error = std::string("latency summary write error: ") + ex.what();
        return false;
    }
}

// Generate an OOM-stress scenario: heavy memory + GPU pressure designed to
// trigger OOM-kill events when run without Hermes, while active-control mode
// should keep the foreground inference job alive.
void generate_oom_stress(const std::string& path) {
    hermes::BenchmarkScenario s;
    s.name = "oom-stress-intervention";
    s.runtime_mode = "active-control";
    s.warmup_s = 5;
    s.measurement_s = 60;
    s.repeat_count = 3;
    s.ups_critical_threshold = 0.75;

    // Assertions: Hermes should prevent all OOM kills and meet latency target.
    s.expected_max_oom_count              = 0.0;
    s.expected_max_p95_latency_ms         = 8000.0;
    s.expected_min_job_completion_rate    = 0.85;

    // Foreground: latency-sensitive inference server (protected by Hermes).
    hermes::BenchmarkWorkload inference;
    inference.name       = "inference_fg";
    inference.command    = "python -c \""
        "import time, sys\n"
        "buf = []\n"
        "for i in range(20):\n"
        "    buf.append(bytearray(50 * 1024 * 1024))\n"  // 50 MB chunks → 1 GB total
        "    time.sleep(0.5)\n"
        "print('inference done')\"";
    inference.foreground  = true;
    inference.duration_s  = 60;

    // Background: memory hog 1 — stress-ng virtual memory.
    hermes::BenchmarkWorkload mem_hog_a;
    mem_hog_a.name       = "mem_hog_a";
    mem_hog_a.command    = "stress-ng --vm 2 --vm-bytes 512M --timeout 60s";
    mem_hog_a.background = true;
    mem_hog_a.duration_s = 60;

    // Background: memory hog 2 — Python large allocation.
    hermes::BenchmarkWorkload mem_hog_b;
    mem_hog_b.name       = "mem_hog_b";
    mem_hog_b.command    = "python -c \""
        "import time\n"
        "buf = bytearray(800 * 1024 * 1024)\n"  // 800 MB allocation
        "time.sleep(55)\"";
    mem_hog_b.background = true;
    mem_hog_b.duration_s = 60;

    // Background: CPU pressure to elevate PSI scores.
    hermes::BenchmarkWorkload cpu_stress;
    cpu_stress.name      = "cpu_pressure";
    cpu_stress.command   = "stress-ng --cpu 2 --timeout 60s";
    cpu_stress.background = true;
    cpu_stress.duration_s = 60;

    s.workloads = {inference, mem_hog_a, mem_hog_b, cpu_stress};

    hermes::ScenarioConfigLoader loader;
    std::string error;
    if (loader.save(path, s, error)) {
        std::cout << "hermes_bench: wrote OOM-stress scenario to " << path << "\n"
                  << "  Mode          : " << s.runtime_mode << "\n"
                  << "  Runs          : " << s.repeat_count << "\n"
                  << "  p95 target    : " << s.expected_max_p95_latency_ms << " ms\n"
                  << "  OOM target    : " << static_cast<int>(s.expected_max_oom_count)
                  << " (Hermes must prevent all OOM kills)\n"
                  << "  Completion min: " << s.expected_min_job_completion_rate << "\n";
    } else {
        std::cerr << "hermes_bench: failed to write OOM-stress config: " << error << "\n";
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

// ---------------------------------------------------------------------------
// --smoke-all: run all pre-built scenarios in sequence, then auto-compare.
// ---------------------------------------------------------------------------

int run_smoke_all(
    const std::string& self_path,
    const std::vector<std::string>& args,
    const std::string& artifact_root)
{
    const bool dry_run = has_flag(args, "--dry-run");

    // Forward optional path overrides from the caller.
    std::string hermes_bin, replay_bin, compare_bin;
    option_value(args, "--hermes-bin",  hermes_bin);
    option_value(args, "--replay-bin",  replay_bin);
    option_value(args, "--compare-bin", compare_bin);

    // Pre-built scenario configs — generate missing ones on the fly.
    struct SmokeTarget {
        std::string config_path;
        std::string run_id_prefix;
        std::string label;
    };

    const std::vector<SmokeTarget> targets = {
        {"config/baseline_scenario.yaml",   "smoke-baseline",   "Baseline"},
        {"config/observe_scenario.yaml",    "smoke-observe",    "Observe-only"},
        {"config/oom_stress_scenario.yaml", "smoke-oom-stress", "OOM-stress"},
    };

    // Auto-generate any missing configs.
    {
        hermes::ScenarioConfigLoader loader;
        std::string err;
        if (!std::filesystem::exists("config/baseline_scenario.yaml"))
            loader.save("config/baseline_scenario.yaml", loader.default_baseline(), err);
        if (!std::filesystem::exists("config/observe_scenario.yaml"))
            loader.save("config/observe_scenario.yaml", loader.default_active(), err);
        if (!std::filesystem::exists("config/oom_stress_scenario.yaml")) {
            generate_oom_stress("config/oom_stress_scenario.yaml");
        }
    }

    struct RunResult {
        std::string label;
        int exit_code{-1};
        bool timed_out{false};
        uint64_t duration_ms{0};
    };

    std::vector<RunResult> results;

    const std::string sep(70, '=');
    std::cout << "\n" << sep << "\n";
    std::cout << "hermes_bench --smoke-all: running " << targets.size() << " scenario(s)\n";
    std::cout << sep << "\n\n";

    for (const SmokeTarget& target : targets) {
        std::cout << "--- Scenario: " << target.label << " (" << target.config_path << ") ---\n";

        std::vector<std::string> child_args = {
            target.config_path,
            "--artifact-root", artifact_root,
            "--run-id", target.run_id_prefix,
        };
        if (dry_run)           child_args.push_back("--dry-run");
        if (!hermes_bin.empty()) { child_args.push_back("--hermes-bin");  child_args.push_back(hermes_bin); }
        if (!replay_bin.empty()) { child_args.push_back("--replay-bin");  child_args.push_back(replay_bin); }

        RunResult r;
        r.label = target.label;
        const uint64_t t0 = wall_now_ms();
        std::string error;
        const bool ok = run_program_sync(self_path, child_args, 300000,
                                         r.exit_code, r.timed_out, error);
        r.duration_ms = wall_now_ms() - t0;
        (void)ok;
        results.push_back(r);

        std::cout << "  [" << target.label << "] exit=" << r.exit_code
                  << " timed_out=" << (r.timed_out ? "yes" : "no")
                  << " elapsed=" << (r.duration_ms / 1000) << "s\n\n";
    }

    // Auto-compare across all runs.
    std::cout << "--- Auto-compare ---\n";
    const std::string compare_bin_path =
        compare_bin.empty()
            ? (std::filesystem::path(self_path).parent_path() / "hermes_compare").string()
            : compare_bin;
    const std::string output_csv = artifact_root + "/bench/smoke_all_comparison.csv";

    int cmp_exit = -1;
    bool cmp_timed_out = false;
    std::string cmp_err;
    run_program_sync(compare_bin_path,
                     {"--bench-dir", artifact_root + "/bench",
                      "--output-csv", output_csv},
                     30000, cmp_exit, cmp_timed_out, cmp_err);
    if (cmp_exit == 0) {
        std::cout << "  comparison CSV written to " << output_csv << "\n";
    } else {
        std::cout << "  hermes_compare not available or no summaries found (skip)\n";
    }

    // Summary table.
    int failed = 0;
    std::cout << "\n" << sep << "\n";
    std::cout << "hermes_bench --smoke-all: summary\n";
    std::cout << sep << "\n";
    std::cout << std::left << std::setw(20) << "Scenario"
              << std::setw(8) << "Exit"
              << std::setw(8) << "Timeout"
              << std::setw(10) << "Elapsed\n";
    std::cout << std::string(46, '-') << "\n";
    for (const RunResult& r : results) {
        const bool pass = !r.timed_out && r.exit_code == 0;
        if (!pass) ++failed;
        std::cout << std::left << std::setw(20) << r.label
                  << std::setw(8) << r.exit_code
                  << std::setw(8) << (r.timed_out ? "YES" : "no")
                  << std::setw(10) << (std::to_string(r.duration_ms / 1000) + "s")
                  << (pass ? "  PASS" : "  FAIL") << "\n";
    }
    std::cout << sep << "\n";
    if (failed == 0) {
        std::cout << "Result: ALL PASS\n";
    } else {
        std::cout << "Result: " << failed << " scenario(s) FAILED\n";
    }
    std::cout << sep << "\n\n";
    return failed > 0 ? 1 : 0;
}

} // namespace

int main(int argc, char* argv[]) {
    std::vector<std::string> args(argv + 1, argv + argc);

    if (args.empty() || has_flag(args, "--help")) {
        std::cout << "Usage: hermes_bench <scenario.yaml> [options]\n"
                  << "       hermes_bench --generate-baseline   [output.yaml]\n"
                  << "       hermes_bench --generate-active     [output.yaml]\n"
                  << "       hermes_bench --generate-oom-stress [output.yaml]\n"
                  << "       hermes_bench --smoke-all           [options]\n"
                  << "\nOptions:\n"
                  << "  --dry-run           Print plan without launching workloads\n"
                  << "  --artifact-root DIR Artifact root for plan output (default: $HERMES_ARTIFACT_ROOT or artifacts)\n"
                  << "  --run-id ID         Base run id for artifacts (default: generated from scenario name)\n"
                  << "  --runs N            Repeat the scenario N times (overrides scenario repeat_count)\n"
                  << "  --hermes-bin PATH   Optional hermesd path for observe/advisory/active runs\n"
                  << "  --replay-bin PATH   Optional hermes_replay path for observe/advisory/active runs\n"
                  << "  --auto-compare      Run hermes_compare after all runs complete\n"
                  << "  --compare-bin PATH  Path to hermes_compare binary (default: sibling of this binary)\n"
                  << "  --verify-targets    Exit non-zero if any run misses latency or OOM assertions\n"
                  << "  --delta-vs PATH     Compare this run's summary against a saved baseline summary JSON\n"
                  << "  --generate-baseline   Write a default baseline scenario config\n"
                  << "  --generate-active     Write a default active-control scenario config\n"
                  << "  --generate-oom-stress Write a memory/VRAM pressure scenario for OOM-kill intervention testing\n"
                  << "  --smoke-all           Run baseline, observe, and oom-stress scenarios in sequence; auto-compare\n";
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

    if (has_flag(args, "--generate-oom-stress")) {
        const std::string path = generation_output_path(args, "--generate-oom-stress", "oom_stress_scenario.yaml");
        generate_oom_stress(path);
        return 0;
    }

    if (has_flag(args, "--smoke-all")) {
        const std::string artifact_root = [&]() {
            std::string ar = env_or("HERMES_ARTIFACT_ROOT", "artifacts");
            option_value(args, "--artifact-root", ar);
            return ar;
        }();
        const std::string self_path = (argc > 0) ? argv[0] : "hermes_bench";
        return run_smoke_all(self_path, args, artifact_root);
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

    std::string base_run_id;
    option_value(args, "--run-id", base_run_id);
    if (base_run_id.empty()) {
        base_run_id = default_run_id(scenario);
    }

    // --runs N overrides scenario repeat_count for multi-run execution.
    int total_runs = scenario.repeat_count;
    {
        std::string runs_str;
        if (option_value(args, "--runs", runs_str)) {
            try {
                const int n = std::stoi(runs_str);
                if (n > 0) {
                    total_runs = n;
                }
            } catch (...) {}
        }
    }

    const std::filesystem::path current_executable =
        argc > 0 ? std::filesystem::path(argv[0]) : std::filesystem::path();
    std::string hermes_bin_override;
    std::string replay_bin_override;
    std::string compare_bin_override;
    const bool auto_compare    = has_flag(args, "--auto-compare");
    const bool verify_targets  = has_flag(args, "--verify-targets");
    option_value(args, "--hermes-bin", hermes_bin_override);
    option_value(args, "--replay-bin", replay_bin_override);
    option_value(args, "--compare-bin", compare_bin_override);

    // Write a single plan artifact that covers all runs.
    const std::string plan_run_id = base_run_id + (total_runs > 1 ? "-plan" : "");
    std::filesystem::path plan_path;
    std::filesystem::path scenario_snapshot_path;
    if (!write_plan_artifacts(
            scenario,
            validation,
            config_path,
            artifact_root,
            plan_run_id,
            dry_run,
            plan_path,
            scenario_snapshot_path,
            error)) {
        std::cerr << "hermes_bench: failed to write plan artifact: " << error << std::endl;
        return 1;
    }

    std::cout << "\nPlan run id      : " << plan_run_id
              << "\nPlan artifact    : " << plan_path.string()
              << "\nScenario snapshot: " << scenario_snapshot_path.string()
              << "\nTotal runs       : " << total_runs << "\n";

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

    bool overall_ok = true;
    MultiRunStats multi_stats;

    for (int run_index = 0; run_index < total_runs; ++run_index) {
        const std::string run_id = (total_runs > 1)
            ? base_run_id + "-r" + std::to_string(run_index + 1)
            : base_run_id;

        if (total_runs > 1) {
            std::cout << "\n--- Run " << (run_index + 1) << "/" << total_runs
                      << " (" << run_id << ") ---\n";
        }

        HermesExecution hermes_execution;
        ReplaySummarySnapshot replay_snapshot;

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
                overall_ok = false;
                continue;
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
        multi_stats.record_run(executions);

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
                compute_fg_p95_ms(executions),
                summary_path,
                error)) {
            std::cerr << "hermes_bench: failed to write execution summary: " << error << std::endl;
            overall_ok = false;
            continue;
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
    }

    // Auto-compare: invoke hermes_compare on the bench directory after all runs.
    if (auto_compare) {
        const std::string compare_bin =
            resolve_binary_path(compare_bin_override, current_executable, "hermes_compare");
        const std::filesystem::path bench_dir =
            std::filesystem::path(artifact_root) / "bench";
        const std::filesystem::path compare_csv =
            bench_dir / (base_run_id + "-auto-comparison.csv");

        std::cout << "\n--- hermes_compare (auto) ---\n";
        std::string compare_error;
        bool compare_timed_out = false;
        int compare_exit = 0;
        const bool compare_ok = run_program_sync(
            compare_bin,
            {"--bench-dir", bench_dir.string(),
             "--output-csv", compare_csv.string()},
            30000,
            compare_exit,
            compare_timed_out,
            compare_error);

        if (!compare_ok) {
            std::cerr << "hermes_bench: hermes_compare failed"
                      << (compare_timed_out ? " (timed out)" : "")
                      << ": " << compare_error << "\n";
        } else {
            std::cout << "Comparison CSV   : " << compare_csv.string() << "\n";
        }
    }

    // Write multi-run latency summary if more than one run or any foreground data collected.
    if (!multi_stats.fg_latency_ms.empty()) {
        std::string lat_error;
        if (write_latency_summary(artifact_root, base_run_id, multi_stats, lat_error)) {
            std::cout << "\nLatency summary  : "
                      << (std::filesystem::path(artifact_root) / "bench" / (base_run_id + "-latency.json")).string()
                      << "\np50 fg latency   : " << std::fixed << std::setprecision(0)
                      << multi_stats.percentile(50.0) << " ms"
                      << "\np95 fg latency   : " << multi_stats.percentile(95.0) << " ms"
                      << "\nOOM kills total  : " << multi_stats.total_oom_kills << "\n";
        } else {
            std::cerr << "hermes_bench: failed to write latency summary: " << lat_error << "\n";
        }
    }

    // --delta-vs: compare this run's summary against a saved baseline summary.
    {
        std::string delta_vs_path;
        if (option_value(args, "--delta-vs", delta_vs_path)) {
            // Load baseline from the provided path.
            const SummarySnapshot baseline = load_summary_snapshot(delta_vs_path);
            if (!baseline.loaded) {
                std::cerr << "hermes_bench: --delta-vs: could not load summary from "
                          << delta_vs_path << "\n";
            } else {
                // Find the most recently written summary for this run.
                const std::string last_run_id = (total_runs > 1)
                    ? base_run_id + "-r" + std::to_string(total_runs)
                    : base_run_id;
                const std::filesystem::path summary_path =
                    std::filesystem::path(artifact_root) / "bench"
                    / (last_run_id + "-summary.json");
                const SummarySnapshot current = load_summary_snapshot(summary_path.string());
                if (!current.loaded) {
                    // Fall back to latency summary if per-run summary not found.
                    const std::filesystem::path lat_path =
                        std::filesystem::path(artifact_root) / "bench"
                        / (base_run_id + "-latency.json");
                    const SummarySnapshot lat_snap = load_summary_snapshot(lat_path.string());
                    if (lat_snap.loaded) {
                        print_delta_table(baseline, lat_snap, base_run_id + " (latency)");
                    } else {
                        std::cerr << "hermes_bench: --delta-vs: current run summary not found at "
                                  << summary_path.string() << "\n";
                    }
                } else {
                    print_delta_table(baseline, current, last_run_id);
                }
            }
        }
    }

    // --verify-targets: check every summary in artifacts/bench/ against scenario assertions.
    if (verify_targets) {
        bool targets_ok = true;
        const std::filesystem::path bench_dir =
            std::filesystem::path(artifact_root) / "bench";

        if (std::filesystem::exists(bench_dir)) {
            for (const auto& entry : std::filesystem::directory_iterator(bench_dir)) {
                const std::string fname = entry.path().filename().string();
                if (fname.size() <= 13 || fname.substr(fname.size() - 13) != "-summary.json")
                    continue;

                std::ifstream f(entry.path());
                if (!f.is_open()) continue;
                std::string content((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());

                // Check latency_target_met (must be true or null/absent when no target set).
                const auto met_pos = content.find("\"latency_target_met\":");
                if (met_pos != std::string::npos) {
                    const auto val_start = content.find_first_not_of(" ", met_pos + 21);
                    if (val_start != std::string::npos &&
                        content.substr(val_start, 5) == "false") {
                        const auto rid_pos = content.find("\"run_id\":\"");
                        std::string rid = fname;
                        if (rid_pos != std::string::npos) {
                            const auto s = rid_pos + 10;
                            const auto e = content.find('"', s);
                            if (e != std::string::npos) rid = content.substr(s, e - s);
                        }
                        std::cerr << "hermes_bench: [FAIL target] " << rid
                                  << " — latency_target_met=false\n";
                        targets_ok = false;
                    }
                }

                // Check oom_count against expected_max_oom_count (0 = must have 0 kills).
                if (scenario.expected_max_oom_count >= 0.0) {
                    const auto oom_pos = content.find("\"oom_count\":");
                    if (oom_pos != std::string::npos) {
                        const auto vs = oom_pos + 12;
                        uint64_t oom_count = 0;
                        try { oom_count = std::stoull(content.substr(vs)); } catch (...) {}
                        if (static_cast<double>(oom_count) > scenario.expected_max_oom_count) {
                            std::cerr << "hermes_bench: [FAIL target] " << fname
                                      << " — oom_count=" << oom_count
                                      << " exceeds expected_max=" << scenario.expected_max_oom_count << "\n";
                            targets_ok = false;
                        }
                    }
                }
            }
        }

        if (targets_ok) {
            std::cout << "\nTarget verification: PASS (all latency and OOM assertions met)\n";
        } else {
            std::cout << "\nTarget verification: FAIL (see errors above)\n";
            return 1;
        }
    }

    return overall_ok ? 0 : 1;
}
