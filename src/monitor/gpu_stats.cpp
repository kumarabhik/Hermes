#include "hermes/monitor/gpu_stats.hpp"

#include <array>
#include <cstdio>
#include <sstream>

#if defined(_WIN32)
#define popen _popen
#define pclose _pclose
#endif

namespace hermes {
namespace {

std::string trim(const std::string& value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool parse_csv_doubles(const std::string& line, std::vector<double>& values) {
    std::stringstream ss(line);
    std::string token;

    while (std::getline(ss, token, ',')) {
        const std::string trimmed = trim(token);
        if (trimmed.empty()) {
            return false;
        }

        try {
            values.push_back(std::stod(trimmed));
        } catch (...) {
            return false;
        }
    }

    return !values.empty();
}

} // namespace

bool GpuStatsCollector::run_command(const std::string& command, std::string& output) {
    output.clear();
    last_error_.clear();

    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        last_error_ = "Failed to launch GPU query command";
        return false;
    }

    std::array<char, 256> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }

    const int close_code = pclose(pipe);
    if (output.empty()) {
        last_error_ = "GPU query produced no output";
        return false;
    }

    if (close_code != 0 && output.empty()) {
        last_error_ = "GPU query returned a non-zero exit code";
        return false;
    }

    return true;
}

bool GpuStatsCollector::update_sample(PressureSample& sample) {
    const std::string command =
        "nvidia-smi --query-gpu=memory.used,memory.total,memory.free,utilization.gpu --format=csv,noheader,nounits";

    std::string output;
    if (!run_command(command, output)) {
        return false;
    }

    std::stringstream lines(output);
    std::string line;
    double total_used_mb = 0.0;
    double total_total_mb = 0.0;
    double total_free_mb = 0.0;
    double total_util_pct = 0.0;
    int gpu_count = 0;

    while (std::getline(lines, line)) {
        if (trim(line).empty()) {
            continue;
        }

        std::vector<double> values;
        if (!parse_csv_doubles(line, values) || values.size() < 4) {
            continue;
        }

        total_used_mb += values[0];
        total_total_mb += values[1];
        total_free_mb += values[2];
        total_util_pct += values[3];
        ++gpu_count;
    }

    if (gpu_count == 0) {
        last_error_ = "No parseable GPU device metrics were returned";
        return false;
    }

    sample.vram_used_mb = total_used_mb;
    sample.vram_total_mb = total_total_mb;
    sample.vram_free_mb = total_free_mb;
    sample.gpu_util_pct = total_util_pct / static_cast<double>(gpu_count);
    return true;
}

std::vector<GpuProcessUsage> GpuStatsCollector::query_process_usage() {
    const std::string command =
        "nvidia-smi --query-compute-apps=pid,used_gpu_memory --format=csv,noheader,nounits";

    std::string output;
    if (!run_command(command, output)) {
        return {};
    }

    std::vector<GpuProcessUsage> usages;
    std::stringstream lines(output);
    std::string line;

    while (std::getline(lines, line)) {
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed.find("No running processes found") != std::string::npos) {
            continue;
        }

        std::stringstream ss(trimmed);
        std::string pid_token;
        std::string memory_token;
        if (!std::getline(ss, pid_token, ',') || !std::getline(ss, memory_token, ',')) {
            continue;
        }

        try {
            GpuProcessUsage usage;
            usage.pid = std::stoi(trim(pid_token));
            usage.gpu_mb = std::stod(trim(memory_token));
            usages.push_back(usage);
        } catch (...) {
            continue;
        }
    }

    return usages;
}

} // namespace hermes
