#include "hermes/runtime/scenario_config.hpp"

#include <fstream>
#include <sstream>

namespace hermes {
namespace {

std::string trim(const std::string& s) {
    const auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string after_colon(const std::string& line) {
    const auto pos = line.find(':');
    if (pos == std::string::npos) return "";
    return trim(line.substr(pos + 1));
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

} // namespace

bool ScenarioConfigLoader::load(
    const std::string& path,
    BenchmarkScenario& scenario_out,
    std::string& error_out) {

    std::ifstream file(path);
    if (!file.is_open()) {
        error_out = "cannot open scenario config: " + path;
        return false;
    }

    BenchmarkScenario scenario;
    BenchmarkWorkload current_workload;
    bool in_workload = false;

    std::string line;
    while (std::getline(file, line)) {
        // Strip comments
        const auto comment = line.find('#');
        if (comment != std::string::npos) {
            line = line.substr(0, comment);
        }

        const std::string tline = trim(line);
        if (tline.empty()) continue;

        // Workload block detection (starts with "  - name:" or "- name:")
        if (tline == "workloads:" || starts_with(tline, "workloads:")) {
            in_workload = false;
            continue;
        }
        if (starts_with(tline, "- name:") || starts_with(tline, "-name:")) {
            if (in_workload && !current_workload.name.empty()) {
                scenario.workloads.push_back(current_workload);
            }
            current_workload = {};
            in_workload = true;
            current_workload.name = trim(tline.substr(tline.find(':') + 1));
            continue;
        }

        if (in_workload) {
            if (starts_with(tline, "command:"))
                current_workload.command = after_colon(tline);
            else if (starts_with(tline, "foreground:"))
                current_workload.foreground = (after_colon(tline) == "true");
            else if (starts_with(tline, "background:"))
                current_workload.background = (after_colon(tline) == "true");
            else if (starts_with(tline, "duration_s:")) {
                try { current_workload.duration_s = std::stoi(after_colon(tline)); } catch (...) {}
            }
            continue;
        }

        // Top-level keys
        if (starts_with(tline, "name:"))
            scenario.name = after_colon(tline);
        else if (starts_with(tline, "runtime_mode:"))
            scenario.runtime_mode = after_colon(tline);
        else if (starts_with(tline, "warmup_s:")) {
            try { scenario.warmup_s = std::stoi(after_colon(tline)); } catch (...) {}
        } else if (starts_with(tline, "measurement_s:")) {
            try { scenario.measurement_s = std::stoi(after_colon(tline)); } catch (...) {}
        } else if (starts_with(tline, "repeat_count:")) {
            try { scenario.repeat_count = std::stoi(after_colon(tline)); } catch (...) {}
        } else if (starts_with(tline, "ups_elevated_threshold:")) {
            try { scenario.ups_elevated_threshold = std::stod(after_colon(tline)); } catch (...) {}
        } else if (starts_with(tline, "ups_critical_threshold:")) {
            try { scenario.ups_critical_threshold = std::stod(after_colon(tline)); } catch (...) {}
        } else if (starts_with(tline, "expected_max_oom_count:")) {
            try { scenario.expected_max_oom_count = std::stod(after_colon(tline)); } catch (...) {}
        } else if (starts_with(tline, "expected_max_p95_latency_ms:")) {
            try { scenario.expected_max_p95_latency_ms = std::stod(after_colon(tline)); } catch (...) {}
        } else if (starts_with(tline, "expected_min_job_completion_rate:")) {
            try { scenario.expected_min_job_completion_rate = std::stod(after_colon(tline)); } catch (...) {}
        }
    }

    if (in_workload && !current_workload.name.empty()) {
        scenario.workloads.push_back(current_workload);
    }

    if (scenario.name.empty()) {
        error_out = "scenario config missing required field: name";
        return false;
    }

    scenario_out = scenario;
    return true;
}

bool ScenarioConfigLoader::save(
    const std::string& path,
    const BenchmarkScenario& scenario,
    std::string& error_out) {

    std::ofstream out(path);
    if (!out.is_open()) {
        error_out = "cannot write scenario config: " + path;
        return false;
    }

    out << "name: " << scenario.name << "\n"
        << "runtime_mode: " << scenario.runtime_mode << "\n"
        << "warmup_s: " << scenario.warmup_s << "\n"
        << "measurement_s: " << scenario.measurement_s << "\n"
        << "repeat_count: " << scenario.repeat_count << "\n"
        << "ups_elevated_threshold: " << scenario.ups_elevated_threshold << "\n"
        << "ups_critical_threshold: " << scenario.ups_critical_threshold << "\n"
        << "expected_max_oom_count: " << scenario.expected_max_oom_count << "\n"
        << "expected_max_p95_latency_ms: " << scenario.expected_max_p95_latency_ms << "\n"
        << "expected_min_job_completion_rate: " << scenario.expected_min_job_completion_rate << "\n"
        << "workloads:\n";

    for (const BenchmarkWorkload& wl : scenario.workloads) {
        out << "  - name: " << wl.name << "\n"
            << "    command: " << wl.command << "\n"
            << "    foreground: " << (wl.foreground ? "true" : "false") << "\n"
            << "    background: " << (wl.background ? "true" : "false") << "\n"
            << "    duration_s: " << wl.duration_s << "\n";
    }

    return true;
}

BenchmarkScenario ScenarioConfigLoader::default_baseline() {
    BenchmarkScenario s;
    s.name = "baseline-no-hermes";
    s.runtime_mode = "observe-only";
    s.warmup_s = 10;
    s.measurement_s = 120;
    s.repeat_count = 5;

    BenchmarkWorkload ml_train;
    ml_train.name = "ml_train_a";
    ml_train.command = "python train.py --epochs 10";
    ml_train.background = true;
    ml_train.duration_s = 120;

    BenchmarkWorkload ml_train_b;
    ml_train_b.name = "ml_train_b";
    ml_train_b.command = "python train.py --epochs 10 --model resnet50";
    ml_train_b.background = true;
    ml_train_b.duration_s = 120;

    BenchmarkWorkload cpu_stress;
    cpu_stress.name = "cpu_stressor";
    cpu_stress.command = "stress-ng --cpu 4 --timeout 120s";
    cpu_stress.background = true;
    cpu_stress.duration_s = 120;

    BenchmarkWorkload inference;
    inference.name = "inference_server";
    inference.command = "python serve.py --port 8080";
    inference.foreground = true;
    inference.duration_s = 120;

    s.workloads = {ml_train, ml_train_b, cpu_stress, inference};
    return s;
}

BenchmarkScenario ScenarioConfigLoader::default_active() {
    BenchmarkScenario s = default_baseline();
    s.name = "active-control-intervention";
    s.runtime_mode = "active-control";
    s.repeat_count = 5;
    s.expected_max_oom_count = 0.0;
    s.expected_min_job_completion_rate = 0.8;
    return s;
}

} // namespace hermes
