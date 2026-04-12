#include "hermes/runtime/telemetry_quality.hpp"

#include "hermes/runtime/event_logger.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>
#include <utility>

namespace hermes {
namespace {

uint64_t wall_now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

uint64_t mono_now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

double ratio(std::size_t numerator, std::size_t denominator) {
    if (denominator == 0) {
        return 0.0;
    }
    return static_cast<double>(numerator) / static_cast<double>(denominator);
}

uint64_t abs_diff(uint64_t left, uint64_t right) {
    return left >= right ? left - right : right - left;
}

std::string map_json(const std::map<std::string, std::size_t>& values) {
    std::ostringstream oss;
    oss << "{";
    bool first = true;
    for (const auto& [key, value] : values) {
        if (!first) {
            oss << ",";
        }
        first = false;
        oss << "\"" << json_escape(key) << "\":" << value;
    }
    oss << "}";
    return oss.str();
}

} // namespace

TelemetryQualityTracker::TelemetryQualityTracker(TelemetryQualityConfig config)
    : config_(std::move(config)) {}

void TelemetryQualityTracker::observe_loop(
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
    const InterventionResult& result) {
    ++sample_count_;

    if (cpu_available) {
        ++cpu_available_count_;
    }
    if (mem_available) {
        ++mem_available_count_;
    }
    if (loadavg_available) {
        ++loadavg_available_count_;
    }
    if (gpu_available) {
        ++gpu_available_count_;
    }

    if (process_refreshed) {
        ++process_refresh_count_;
        process_snapshot_records_ += process_count;
        max_processes_seen_ = std::max(max_processes_seen_, process_count);
    }

    if (first_ts_mono_ == 0) {
        first_ts_mono_ = sample.ts_mono;
    }
    last_ts_mono_ = sample.ts_mono;

    if (previous_ts_mono_ != 0 && sample.ts_mono >= previous_ts_mono_) {
        const uint64_t interval_ms = sample.ts_mono - previous_ts_mono_;
        const uint64_t jitter_ms = abs_diff(interval_ms, config_.expected_interval_ms);
        ++interval_count_;
        total_loop_interval_ms_ += interval_ms;
        max_loop_interval_ms_ = std::max(max_loop_interval_ms_, interval_ms);
        total_loop_jitter_ms_ += jitter_ms;
        max_loop_jitter_ms_ = std::max(max_loop_jitter_ms_, jitter_ms);
    }
    previous_ts_mono_ = sample.ts_mono;

    ++decision_count_;
    if (decision.should_execute) {
        ++executable_decision_count_;
    }
    if (decision.scheduler_state_changed) {
        ++state_transition_count_;
    }

    ++action_result_count_;
    if (result.success) {
        ++successful_action_result_count_;
    }

    peak_ups_ = std::max(peak_ups_, score.ups);
    peak_risk_score_ = std::max(peak_risk_score_, prediction.risk_score);

    ++scheduler_states_[to_string(decision.scheduler_state)];
    ++decision_actions_[to_string(decision.action)];
    ++risk_bands_[to_string(prediction.risk_band)];
    ++pressure_bands_[to_string(score.band)];
}

bool TelemetryQualityTracker::write(std::string& error) const {
    if (config_.run_directory.empty()) {
        error = "run directory is required for telemetry quality output";
        return false;
    }

    try {
        std::filesystem::create_directories(config_.run_directory);
    } catch (const std::exception& ex) {
        error = std::string("failed to create telemetry output directory: ") + ex.what();
        return false;
    }

    const std::filesystem::path output_path = config_.run_directory / "telemetry_quality.json";
    std::ofstream output(output_path, std::ios::out | std::ios::binary);
    if (!output.is_open()) {
        error = "failed to open telemetry quality output: " + output_path.string();
        return false;
    }

    const double avg_loop_interval_ms = interval_count_ == 0
        ? 0.0
        : static_cast<double>(total_loop_interval_ms_) / static_cast<double>(interval_count_);
    const double avg_loop_jitter_ms = interval_count_ == 0
        ? 0.0
        : static_cast<double>(total_loop_jitter_ms_) / static_cast<double>(interval_count_);
    const uint64_t duration_mono_ms = last_ts_mono_ >= first_ts_mono_
        ? last_ts_mono_ - first_ts_mono_
        : 0;

    output << "{\n"
           << "  \"run_id\": \"" << json_escape(config_.run_id) << "\",\n"
           << "  \"scenario\": \"" << json_escape(config_.scenario) << "\",\n"
           << "  \"config_hash\": \"" << json_escape(config_.config_hash) << "\",\n"
           << "  \"generated_ts_wall\": " << wall_now_ms() << ",\n"
           << "  \"generated_ts_mono\": " << mono_now_ms() << ",\n"
           << "  \"samples\": {\n"
           << "    \"total\": " << sample_count_ << ",\n"
           << "    \"first_ts_mono\": " << first_ts_mono_ << ",\n"
           << "    \"last_ts_mono\": " << last_ts_mono_ << ",\n"
           << "    \"duration_mono_ms\": " << duration_mono_ms << "\n"
           << "  },\n"
           << "  \"providers\": {\n"
           << "    \"cpu_psi\": {\"available_samples\": " << cpu_available_count_
           << ", \"missing_samples\": " << (sample_count_ - cpu_available_count_)
           << ", \"availability_ratio\": " << ratio(cpu_available_count_, sample_count_) << "},\n"
           << "    \"mem_psi\": {\"available_samples\": " << mem_available_count_
           << ", \"missing_samples\": " << (sample_count_ - mem_available_count_)
           << ", \"availability_ratio\": " << ratio(mem_available_count_, sample_count_) << "},\n"
           << "    \"loadavg\": {\"available_samples\": " << loadavg_available_count_
           << ", \"missing_samples\": " << (sample_count_ - loadavg_available_count_)
           << ", \"availability_ratio\": " << ratio(loadavg_available_count_, sample_count_) << "},\n"
           << "    \"gpu_stats\": {\"available_samples\": " << gpu_available_count_
           << ", \"missing_samples\": " << (sample_count_ - gpu_available_count_)
           << ", \"availability_ratio\": " << ratio(gpu_available_count_, sample_count_) << "}\n"
           << "  },\n"
           << "  \"loop_health\": {\n"
           << "    \"expected_interval_ms\": " << config_.expected_interval_ms << ",\n"
           << "    \"interval_count\": " << interval_count_ << ",\n"
           << "    \"avg_loop_interval_ms\": " << avg_loop_interval_ms << ",\n"
           << "    \"max_loop_interval_ms\": " << max_loop_interval_ms_ << ",\n"
           << "    \"avg_loop_jitter_ms\": " << avg_loop_jitter_ms << ",\n"
           << "    \"max_loop_jitter_ms\": " << max_loop_jitter_ms_ << "\n"
           << "  },\n"
           << "  \"process_mapping\": {\n"
           << "    \"refresh_count\": " << process_refresh_count_ << ",\n"
           << "    \"snapshot_records\": " << process_snapshot_records_ << ",\n"
           << "    \"max_processes_seen\": " << max_processes_seen_ << "\n"
           << "  },\n"
           << "  \"control_loop\": {\n"
           << "    \"decisions\": " << decision_count_ << ",\n"
           << "    \"executable_decisions\": " << executable_decision_count_ << ",\n"
           << "    \"action_results\": " << action_result_count_ << ",\n"
           << "    \"successful_action_results\": " << successful_action_result_count_ << ",\n"
           << "    \"state_transitions\": " << state_transition_count_ << ",\n"
           << "    \"peak_ups\": " << peak_ups_ << ",\n"
           << "    \"peak_risk_score\": " << peak_risk_score_ << "\n"
           << "  },\n"
           << "  \"pressure_bands\": " << map_json(pressure_bands_) << ",\n"
           << "  \"risk_bands\": " << map_json(risk_bands_) << ",\n"
           << "  \"scheduler_states\": " << map_json(scheduler_states_) << ",\n"
           << "  \"decision_actions\": " << map_json(decision_actions_) << "\n"
           << "}\n";

    return true;
}

} // namespace hermes
