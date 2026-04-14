#pragma once

#include "hermes/core/types.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace hermes {

struct EventLoggerConfig {
    std::filesystem::path artifact_root{"artifacts"};
    std::string run_id;
    std::string scenario{"observe"};
    std::string config_hash{"unknown"};
    bool enabled{true};
};

class EventLogger {
public:
    explicit EventLogger(EventLoggerConfig config);

    bool open();
    bool is_open() const {
        return opened_;
    }

    const std::filesystem::path& run_directory() const {
        return run_directory_;
    }

    const std::string& last_error() const {
        return last_error_;
    }

    void log_run_start(const std::string& runtime_mode);
    void log_sample(
        const PressureSample& sample,
        bool cpu_available,
        bool mem_available,
        bool loadavg_available,
        bool gpu_available,
        bool io_available = false,
        bool vmstat_available = false);
    void log_processes(const PressureSample& sample, const std::vector<ProcessSnapshot>& processes);
    void log_score(const PressureScore& score);
    void log_prediction(const RiskPrediction& prediction);
    void log_decision(
        const PressureScore& score,
        const RiskPrediction& prediction,
        const InterventionDecision& decision);
    void log_action(const InterventionDecision& decision, const InterventionResult& result);
    void log_event(const std::string& kind, const std::string& payload_json);

private:
    bool open_stream(std::ofstream& stream, const std::string& filename);
    void write_line(std::ofstream& stream, const std::string& line);
    std::string common_fields(uint64_t ts_wall, uint64_t ts_mono) const;

    EventLoggerConfig config_;
    std::filesystem::path run_directory_;
    std::ofstream samples_;
    std::ofstream processes_;
    std::ofstream scores_;
    std::ofstream predictions_;
    std::ofstream decisions_;
    std::ofstream actions_;
    std::ofstream events_;
    std::string last_error_;
    bool opened_{false};
};

std::string json_escape(const std::string& value);
std::string json_string_array(const std::vector<std::string>& values);
std::string json_int_array(const std::vector<int>& values);

} // namespace hermes
