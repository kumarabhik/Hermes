#include "hermes/replay/replay_summary.hpp"

#include <cstdlib>
#include <filesystem>
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

void print_usage() {
    std::cout << "Usage: hermes_replay <run-directory> [artifact-root]\n"
              << "\n"
              << "Summarizes Hermes NDJSON artifacts from a daemon run directory.\n"
              << "Writes replay_summary.json and summary.csv beside the run and copies summaries to artifacts/replay/ and artifacts/summaries/.\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
        print_usage();
        return argc < 2 ? 1 : 0;
    }

    const std::filesystem::path run_directory = argv[1];
    const std::filesystem::path artifact_root = argc >= 3
        ? std::filesystem::path(argv[2])
        : std::filesystem::path(env_or("HERMES_ARTIFACT_ROOT", "artifacts"));

    hermes::ReplaySummaryBuilder builder;
    const hermes::ReplaySummary summary = builder.summarize(run_directory);

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
