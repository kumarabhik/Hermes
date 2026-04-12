#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace hermes {

struct ReplayCounts {
    std::size_t samples{0};
    std::size_t processes{0};
    std::size_t scores{0};
    std::size_t predictions{0};
    std::size_t decisions{0};
    std::size_t actions{0};
    std::size_t events{0};
    std::size_t parse_errors{0};
};

struct ReplaySummary {
    std::string run_id;
    std::string scenario;
    std::string config_hash;
    bool valid{true};
    ReplayCounts counts{};
    uint64_t first_ts_wall{0};
    uint64_t last_ts_wall{0};
    uint64_t first_ts_mono{0};
    uint64_t last_ts_mono{0};
    double peak_ups{0.0};
    double peak_risk_score{0.0};
    double peak_mem_full_avg10{0.0};
    double peak_gpu_util_pct{0.0};
    double peak_vram_used_mb{0.0};
    double min_vram_free_mb{0.0};
    bool saw_vram_free{false};
    bool run_metadata_present{false};
    bool config_snapshot_present{false};
    bool telemetry_quality_present{false};
    bool scenario_manifest_present{false};
    uintmax_t run_metadata_bytes{0};
    uintmax_t config_snapshot_bytes{0};
    uintmax_t telemetry_quality_bytes{0};
    uintmax_t scenario_manifest_bytes{0};
    std::size_t assertions_checked{0};
    std::size_t assertions_passed{0};
    std::size_t assertions_failed{0};
    bool has_expected_min_peak_ups{false};
    bool has_expected_min_peak_risk_score{false};
    double expected_min_peak_ups{0.0};
    double expected_min_peak_risk_score{0.0};
    std::map<std::string, std::size_t> pressure_bands;
    std::map<std::string, std::size_t> risk_bands;
    std::map<std::string, std::size_t> scheduler_states;
    std::map<std::string, std::size_t> process_classes;
    std::map<std::string, std::size_t> decision_actions;
    std::map<std::string, std::size_t> action_results;
    std::map<std::string, std::size_t> event_kinds;
    std::map<std::string, std::size_t> observed_signals;
    std::map<std::string, std::size_t> expected_min_decision_actions;
    std::map<std::string, std::size_t> expected_min_scheduler_states;
    std::map<std::string, std::size_t> expected_min_pressure_bands;
    std::map<std::string, std::size_t> expected_min_risk_bands;
    std::vector<std::string> expected_signals;
    std::vector<std::string> assertion_failures;
    std::vector<std::string> warnings;
};

class ReplaySummaryBuilder {
public:
    ReplaySummary summarize(const std::filesystem::path& run_directory);
    bool write_summary(
        const ReplaySummary& summary,
        const std::filesystem::path& output_path,
        std::string& error) const;

private:
    void read_samples(const std::filesystem::path& run_directory, ReplaySummary& summary) const;
    void read_processes(const std::filesystem::path& run_directory, ReplaySummary& summary) const;
    void read_scores(const std::filesystem::path& run_directory, ReplaySummary& summary) const;
    void read_predictions(const std::filesystem::path& run_directory, ReplaySummary& summary) const;
    void read_decisions(const std::filesystem::path& run_directory, ReplaySummary& summary) const;
    void read_actions(const std::filesystem::path& run_directory, ReplaySummary& summary) const;
    void read_events(const std::filesystem::path& run_directory, ReplaySummary& summary) const;
    void inspect_metadata_artifacts(const std::filesystem::path& run_directory, ReplaySummary& summary) const;
    void validate_scenario_manifest(const std::filesystem::path& run_directory, ReplaySummary& summary) const;
};

std::string replay_summary_json(const ReplaySummary& summary);

} // namespace hermes
