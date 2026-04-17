#include "hermes/replay/replay_summary.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

std::string env_or(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    return value;
}

bool has_arg(int argc, char** argv, const std::string& flag) {
    for (int i = 1; i < argc; ++i) if (flag == argv[i]) return true;
    return false;
}

void print_usage() {
    std::cout << "Usage: hermes_replay <run-directory> [artifact-root] [options]\n"
              << "\n"
              << "Summarizes Hermes NDJSON artifacts from a daemon run directory.\n"
              << "Writes replay_summary.json and summary.csv beside the run and copies summaries to artifacts/replay/ and artifacts/summaries/.\n"
              << "\n"
              << "Options:\n"
              << "  --generate-manifest   Auto-generate scenario_manifest.json from observed peaks/states/actions.\n"
              << "                        Useful for locking in a known-good run as a regression baseline.\n";
}

// Write a scenario_manifest.json derived from the replay summary.
// The manifest encodes the observed peaks and state distributions as minimum
// expectations, so future replays of the same config will assert at least
// this level of coverage.
bool write_generated_manifest(const hermes::ReplaySummary& summary,
                               const std::filesystem::path& run_dir,
                               std::string& error) {
    const std::filesystem::path out = run_dir / "scenario_manifest.json";

    std::ofstream f(out);
    if (!f.is_open()) {
        error = "Cannot open " + out.string() + " for writing";
        return false;
    }

    // Floor peak values slightly so minor noise doesn't cause assertion failures.
    const double min_peak_ups  = summary.peak_ups  * 0.80;
    const double min_peak_risk = summary.peak_risk_score * 0.75;

    f << "{\n";
    f << "  \"generated_by\": \"hermes_replay --generate-manifest\",\n";
    f << "  \"source_run_id\": \"" << summary.run_id << "\",\n";
    f << "  \"min_peak_ups\": " << min_peak_ups << ",\n";
    f << "  \"min_peak_risk_score\": " << min_peak_risk << ",\n";

    // Minimum action counts at 80% of observed.
    if (!summary.decision_actions.empty()) {
        f << "  \"min_action_counts\": {\n";
        bool first = true;
        for (const auto& kv : summary.decision_actions) {
            if (kv.second == 0) continue;
            if (!first) f << ",\n";
            first = false;
            const std::size_t floor_count = (kv.second * 4) / 5;  // 80%
            f << "    \"" << kv.first << "\": " << floor_count;
        }
        f << "\n  },\n";
    }

    // Minimum scheduler state counts (must have seen each state at least once).
    if (!summary.scheduler_states.empty()) {
        f << "  \"min_scheduler_state_counts\": {\n";
        bool first = true;
        for (const auto& kv : summary.scheduler_states) {
            if (kv.second == 0) continue;
            if (!first) f << ",\n";
            first = false;
            f << "    \"" << kv.first << "\": 1";
        }
        f << "\n  },\n";
    }

    // Minimum pressure band counts.
    if (!summary.pressure_bands.empty()) {
        f << "  \"min_pressure_band_counts\": {\n";
        bool first = true;
        for (const auto& kv : summary.pressure_bands) {
            if (kv.second == 0) continue;
            if (!first) f << ",\n";
            first = false;
            f << "    \"" << kv.first << "\": 1";
        }
        f << "\n  },\n";
    }

    f << "  \"assertions_source\": \"auto-generated from run_id=" << summary.run_id << "\"\n";
    f << "}\n";

    return f.good();
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
        print_usage();
        return argc < 2 ? 1 : 0;
    }

    const bool generate_manifest = has_arg(argc, argv, "--generate-manifest");

    const std::filesystem::path run_directory = argv[1];
    // artifact-root is positional arg 2 only if it doesn't start with '--'
    std::filesystem::path artifact_root;
    if (argc >= 3 && argv[2][0] != '-') {
        artifact_root = std::filesystem::path(argv[2]);
    } else {
        artifact_root = std::filesystem::path(env_or("HERMES_ARTIFACT_ROOT", "artifacts"));
    }

    hermes::ReplaySummaryBuilder builder;
    const hermes::ReplaySummary summary = builder.summarize(run_directory);

    if (generate_manifest) {
        std::string merr;
        if (write_generated_manifest(summary, run_directory, merr)) {
            std::cout << "Generated " << (run_directory / "scenario_manifest.json").string() << "\n";
        } else {
            std::cerr << "Failed to generate manifest: " << merr << "\n";
            return 2;
        }
    }

    std::string error;
    const std::filesystem::path run_summary_path = run_directory / "replay_summary.json";
    if (!builder.write_summary(summary, run_summary_path, error)) {
        std::cerr << "Failed to write run summary: " << error << std::endl;
        return 2;
    }

    std::filesystem::path replay_summary_path;
    std::filesystem::path artifact_summary_path;
    std::filesystem::path run_csv_path = run_directory / "summary.csv";
    if (!builder.write_summary_csv(summary, run_csv_path, error)) {
        std::cerr << "Failed to write run summary CSV: " << error << std::endl;
        return 2;
    }

    std::filesystem::path replay_csv_path;
    std::filesystem::path artifact_csv_path;
    if (!summary.run_id.empty()) {
        replay_summary_path = artifact_root / "replay" / (summary.run_id + "-summary.json");
        if (!builder.write_summary(summary, replay_summary_path, error)) {
            std::cerr << "Failed to write replay summary copy: " << error << std::endl;
            return 2;
        }

        replay_csv_path = artifact_root / "replay" / (summary.run_id + "-summary.csv");
        if (!builder.write_summary_csv(summary, replay_csv_path, error)) {
            std::cerr << "Failed to write replay summary CSV copy: " << error << std::endl;
            return 2;
        }

        artifact_summary_path = artifact_root / "summaries" / (summary.run_id + "-summary.json");
        if (!builder.write_summary(summary, artifact_summary_path, error)) {
            std::cerr << "Failed to write artifact summary copy: " << error << std::endl;
            return 2;
        }

        artifact_csv_path = artifact_root / "summaries" / (summary.run_id + "-summary.csv");
        if (!builder.write_summary_csv(summary, artifact_csv_path, error)) {
            std::cerr << "Failed to write artifact summary CSV copy: " << error << std::endl;
            return 2;
        }
    }

    std::cout << "Replay summary for run_id=" << (summary.run_id.empty() ? "unknown" : summary.run_id)
              << " valid=" << (summary.valid ? "true" : "false")
              << " samples=" << summary.counts.samples
              << " decisions=" << summary.counts.decisions
              << " actions=" << summary.counts.actions
              << " assertions=" << summary.assertions_passed << "/" << summary.assertions_checked
              << " peak_ups=" << summary.peak_ups
              << " peak_risk=" << summary.peak_risk_score
              << std::endl;
    std::cout << "Wrote " << run_summary_path.string() << std::endl;
    std::cout << "Wrote " << run_csv_path.string() << std::endl;
    if (!replay_summary_path.empty()) {
        std::cout << "Wrote " << replay_summary_path.string() << std::endl;
    }
    if (!replay_csv_path.empty()) {
        std::cout << "Wrote " << replay_csv_path.string() << std::endl;
    }
    if (!artifact_summary_path.empty()) {
        std::cout << "Wrote " << artifact_summary_path.string() << std::endl;
    }
    if (!artifact_csv_path.empty()) {
        std::cout << "Wrote " << artifact_csv_path.string() << std::endl;
    }

    if (!summary.warnings.empty()) {
        std::cout << "Warnings:" << std::endl;
        for (const std::string& warning : summary.warnings) {
            std::cout << "  - " << warning << std::endl;
        }
    }

    if (!summary.assertion_failures.empty()) {
        std::cout << "Assertion failures:" << std::endl;
        for (const std::string& failure : summary.assertion_failures) {
            std::cout << "  - " << failure << std::endl;
        }
    }

    return summary.valid ? 0 : 3;
}
