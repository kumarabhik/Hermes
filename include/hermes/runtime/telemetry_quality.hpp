#pragma once

#include "hermes/core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

namespace hermes {

struct TelemetryQualityConfig {
    std::filesystem::path run_directory;
    std::string run_id;
    std::string scenario{"observe"};
    std::string config_hash{"unknown"};
    uint64_t expected_interval_ms{500};
};

class TelemetryQualityTracker {
public:
    explicit TelemetryQualityTracker(TelemetryQualityConfig config);

    void observe_loop(
        const PressureSample& sample,
        bool cpu_available,
        bool mem_available,
        bool loadavg_available,
        bool gpu_available,
        bool process_refreshed,
        std::size_t process_count,
        const PressureScore& score,
        const RiskPrediction& prediction,
        const InterventionDecision& decision,
        const InterventionResult& result);

    bool write(std::string& error) const;

private:
    TelemetryQualityConfig config_;
    std::size_t sample_count_{0};
    std::size_t cpu_available_count_{0};
    std::size_t mem_available_count_{0};
    std::size_t loadavg_available_count_{0};
    std::size_t gpu_available_count_{0};
    std::size_t process_refresh_count_{0};
    std::size_t process_snapshot_records_{0};
    std::size_t max_processes_seen_{0};
    std::size_t decision_count_{0};
    std::size_t executable_decision_count_{0};
    std::size_t action_result_count_{0};
    std::size_t successful_action_result_count_{0};
    std::size_t state_transition_count_{0};
    uint64_t first_ts_mono_{0};
    uint64_t last_ts_mono_{0};
    uint64_t previous_ts_mono_{0};
    uint64_t interval_count_{0};
    uint64_t total_loop_interval_ms_{0};
    uint64_t max_loop_interval_ms_{0};
    uint64_t total_loop_jitter_ms_{0};
    uint64_t max_loop_jitter_ms_{0};
    double peak_ups_{0.0};
    double peak_risk_score_{0.0};
    std::map<std::string, std::size_t> scheduler_states_;
    std::map<std::string, std::size_t> decision_actions_;
    std::map<std::string, std::size_t> risk_bands_;
    std::map<std::string, std::size_t> pressure_bands_;
};

} // namespace hermes
