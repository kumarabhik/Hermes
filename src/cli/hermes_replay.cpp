#include "hermes/replay/replay_summary.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
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
              << "                        Useful for locking in a known-good run as a regression baseline.\n"
              << "  --diff <baseline-dir> Compare this run against a baseline run directory side-by-side.\n"
              << "                        Prints peak UPS, risk, action counts, and state distribution deltas.\n";
}

// Compare two replay summaries side-by-side.
void print_diff(const hermes::ReplaySummary& a, const hermes::ReplaySummary& b,
                const std::string& label_a, const std::string& label_b) {
    const std::string sep(72, '=');
    std::cout << sep << "\n";
    std::cout << "hermes_replay --diff\n";
    std::cout << sep << "\n";
    std::cout << "  Baseline : " << label_a << "\n";
    std::cout << "  Compare  : " << label_b << "\n\n";

    auto row = [](const std::string& metric, double va, double vb, bool lower_is_better) {
        const double delta = vb - va;
        std::string verdict = "=";
        if (std::abs(delta) > 0.001) {
            const bool improved = lower_is_better ? delta < 0 : delta > 0;
            verdict = improved ? "BETTER" : "WORSE";
        }
        std::cout << std::left  << std::setw(28) << metric
                  << std::right << std::fixed << std::setprecision(2)
                  << std::setw(12) << va
                  << std::setw(12) << vb
                  << std::setw(10) << (delta >= 0 ? "+" : "") + std::to_string(static_cast<int>(delta * 100) / 100.0).substr(0, 7)
                  << "  " << verdict << "\n";
    };
    auto row_cnt = [](const std::string& metric, std::size_t va, std::size_t vb, bool lower_is_better) {
        const long delta = static_cast<long>(vb) - static_cast<long>(va);
        std::string verdict = "=";
        if (delta != 0) {
            const bool improved = lower_is_better ? delta < 0 : delta > 0;
            verdict = improved ? "BETTER" : "WORSE";
        }
        std::cout << std::left  << std::setw(28) << metric
                  << std::right
                  << std::setw(12) << va
                  << std::setw(12) << vb
                  << std::setw(10) << (delta >= 0 ? "+" : "") + std::to_string(delta)
                  << "  " << verdict << "\n";
    };

    std::cout << std::left  << std::setw(28) << "Metric"
              << std::right << std::setw(12) << "Baseline"
              << std::setw(12) << "Compare"
              << std::setw(10) << "Delta"
              << "  Verdict\n";
    std::cout << std::string(72, '-') << "\n";

    row("peak_ups",           a.peak_ups,           b.peak_ups,           true);
    row("peak_risk_score",    a.peak_risk_score,     b.peak_risk_score,    true);
    row("peak_mem_full_avg10",a.peak_mem_full_avg10, b.peak_mem_full_avg10,true);
    row("peak_gpu_util_pct",  a.peak_gpu_util_pct,   b.peak_gpu_util_pct,  true);
    row_cnt("samples",        a.counts.samples,      b.counts.samples,     false);
    row_cnt("predictions",    a.counts.predictions,  b.counts.predictions, false);
    row_cnt("decisions",      a.counts.decisions,    b.counts.decisions,   false);
    row_cnt("actions",        a.counts.actions,      b.counts.actions,     true);
    row_cnt("parse_errors",   a.counts.parse_errors, b.counts.parse_errors,true);

    // State distribution.
    std::cout << "\nScheduler state distribution:\n";
    std::set<std::string> all_states;
    for (const auto& kv : a.scheduler_states) all_states.insert(kv.first);
    for (const auto& kv : b.scheduler_states) all_states.insert(kv.first);
    for (const auto& s : all_states) {
        const std::size_t ca = a.scheduler_states.count(s) ? a.scheduler_states.at(s) : 0;
        const std::size_t cb = b.scheduler_states.count(s) ? b.scheduler_states.at(s) : 0;
        std::cout << "  " << std::left << std::setw(16) << s
                  << "  baseline=" << ca << "  compare=" << cb << "\n";
    }

    // Action distribution.
    std::cout << "\nAction distribution:\n";
    std::set<std::string> all_actions;
    for (const auto& kv : a.decision_actions) all_actions.insert(kv.first);
    for (const auto& kv : b.decision_actions) all_actions.insert(kv.first);
    if (all_actions.empty()) {
        std::cout << "  (none)\n";
    }
    for (const auto& act : all_actions) {
        const std::size_t ca = a.decision_actions.count(act) ? a.decision_actions.at(act) : 0;
        const std::size_t cb = b.decision_actions.count(act) ? b.decision_actions.at(act) : 0;
        std::cout << "  " << std::left << std::setw(24) << act
                  << "  baseline=" << ca << "  compare=" << cb << "\n";
    }

    std::cout << sep << "\n";
    std::cout << "  Verdict: BETTER/WORSE relative to baseline. = means no change.\n";
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

    // --diff <baseline-dir>: compare this run against a baseline.
    std::string diff_baseline;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--diff") { diff_baseline = argv[i + 1]; break; }
    }

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

    // --diff mode: summarize both runs and print comparison, then exit.
    if (!diff_baseline.empty()) {
        const std::filesystem::path baseline_dir = diff_baseline;
        hermes::ReplaySummaryBuilder baseline_builder;
        const hermes::ReplaySummary  baseline_summary = baseline_builder.summarize(baseline_dir);
        print_diff(baseline_summary, summary,
                   baseline_dir.filename().string(),
                   run_directory.filename().string());
        return 0;
    }

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
