// hermes_compare: Benchmark comparison aggregator.
//
// Reads all *-summary.json benchmark artifacts under artifacts/bench/ (or a
// specified directory) and emits a baseline vs observe-only vs active-control
// comparison table plus a comparison CSV.
//
// Each row in the table represents one run. Columns include:
//   scenario, runtime_mode, run_id, jobs_completed, launched, completion_rate,
//   intervention_count, oom_count, degraded_behavior, duration_ms,
//   peak_ups, peak_risk_score, peak_vram_used_mb
//
// Usage:
//   hermes_compare [--bench-dir DIR] [--output-csv PATH] [--scenario FILTER]

#include <algorithm>
#include <cstdlib>
#include <map>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct BenchRow {
    std::string run_id;
    std::string scenario;
    std::string runtime_mode;
    uint64_t launched{0};
    uint64_t jobs_completed{0};
    uint64_t timed_out{0};
    uint64_t exited_nonzero{0};
    double completion_rate{0.0};
    uint64_t intervention_count{0};
    uint64_t oom_count{0};
    bool degraded_behavior{false};
    uint64_t duration_ms{0};
    double peak_ups{0.0};
    double peak_risk_score{0.0};
    double peak_vram_used_mb{0.0};
    std::string source_file;
};

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return "";
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string extract_string(const std::string& json, const std::string& key) {
    const std::string tag1 = "\"" + key + "\":\"";
    auto pos = json.find(tag1);
    if (pos != std::string::npos) {
        const std::size_t start = pos + tag1.size();
        const std::size_t end = json.find('"', start);
        if (end != std::string::npos) return json.substr(start, end - start);
    }
    const std::string tag2 = "\"" + key + "\": \"";
    pos = json.find(tag2);
    if (pos == std::string::npos) return "";
    const std::size_t start = pos + tag2.size();
    const std::size_t end = json.find('"', start);
    return end == std::string::npos ? "" : json.substr(start, end - start);
}

double extract_double(const std::string& json, const std::string& key) {
    const std::string tag = "\"" + key + "\":";
    auto pos = json.find(tag);
    if (pos == std::string::npos) {
        const std::string tag2 = "\"" + key + "\": ";
        pos = json.find(tag2);
        if (pos == std::string::npos) return 0.0;
        pos += tag2.size() - 1;
    }
    std::size_t start = pos + tag.size();
    while (start < json.size() && (json[start] == ' ' || json[start] == '\t')) ++start;
    if (start >= json.size() || json[start] == '"') return 0.0;
    try { return std::stod(json.substr(start)); } catch (...) { return 0.0; }
}

uint64_t extract_uint64(const std::string& json, const std::string& key) {
    const std::string tag = "\"" + key + "\":";
    auto pos = json.find(tag);
    if (pos == std::string::npos) {
        const std::string tag2 = "\"" + key + "\": ";
        pos = json.find(tag2);
        if (pos == std::string::npos) return 0;
        pos += tag2.size() - 1;
    }
    std::size_t start = pos + tag.size();
    while (start < json.size() && (json[start] == ' ' || json[start] == '\t')) ++start;
    if (start >= json.size() || json[start] == '"') return 0;
    try { return static_cast<uint64_t>(std::stoull(json.substr(start))); } catch (...) { return 0; }
}

bool extract_bool(const std::string& json, const std::string& key) {
    const std::string tag = "\"" + key + "\":";
    auto pos = json.find(tag);
    if (pos == std::string::npos) return false;
    std::size_t start = pos + tag.size();
    while (start < json.size() && (json[start] == ' ' || json[start] == '\t')) ++start;
    return start < json.size() && json[start] == 't';
}

BenchRow parse_summary(const std::filesystem::path& path) {
    BenchRow row;
    row.source_file = path.filename().string();
    const std::string json = read_text_file(path);
    if (json.empty()) return row;

    row.run_id           = extract_string(json, "run_id");
    row.scenario         = extract_string(json, "scenario");
    row.runtime_mode     = extract_string(json, "runtime_mode");
    row.launched         = extract_uint64(json, "launched");
    row.jobs_completed   = extract_uint64(json, "jobs_completed");
    row.timed_out        = extract_uint64(json, "timed_out");
    row.exited_nonzero   = extract_uint64(json, "exited_nonzero");
    row.completion_rate  = extract_double(json, "completion_rate");
    row.intervention_count = extract_uint64(json, "intervention_count");
    row.oom_count        = extract_uint64(json, "oom_count");
    row.degraded_behavior = extract_bool(json, "degraded_behavior");
    row.duration_ms      = extract_uint64(json, "duration_ms");

    // Replay snapshot fields are nested under "replay_snapshot": { ... }
    const auto rs_pos = json.find("\"replay_snapshot\":");
    if (rs_pos != std::string::npos) {
        const auto brace = json.find('{', rs_pos);
        const auto end   = json.find('}', brace);
        if (brace != std::string::npos && end != std::string::npos) {
            const std::string rs = json.substr(brace, end - brace + 1);
            row.peak_ups          = extract_double(rs, "peak_ups");
            row.peak_risk_score   = extract_double(rs, "peak_risk_score");
            row.peak_vram_used_mb = extract_double(rs, "peak_vram_used_mb");
        }
    }
    return row;
}

std::string csv_escape(const std::string& value) {
    if (value.find(',') == std::string::npos &&
        value.find('"') == std::string::npos &&
        value.find('\n') == std::string::npos) {
        return value;
    }
    std::string out = "\"";
    for (const char ch : value) {
        if (ch == '"') out += "\"\"";
        else out.push_back(ch);
    }
    out += "\"";
    return out;
}

// Mode sort order for stable display.
int mode_order(const std::string& mode) {
    if (mode == "baseline")       return 0;
    if (mode == "observe-only")   return 1;
    if (mode == "advisory")       return 2;
    if (mode == "active-control") return 3;
    return 4;
}

} // namespace

int main(int argc, char* argv[]) {
    std::vector<std::string> args(argv + 1, argv + argc);

    std::string bench_dir = "artifacts/bench";
    std::string output_csv;
    std::string scenario_filter;
    std::string summary_json;

    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--bench-dir" && i + 1 < args.size()) {
            bench_dir = args[++i];
        } else if (args[i] == "--output-csv" && i + 1 < args.size()) {
            output_csv = args[++i];
        } else if (args[i] == "--scenario" && i + 1 < args.size()) {
            scenario_filter = args[++i];
        } else if (args[i] == "--summary-json" && i + 1 < args.size()) {
            summary_json = args[++i];
        } else if (args[i] == "--help") {
            std::cout << "Usage: hermes_compare [--bench-dir DIR] [--output-csv PATH]\n"
                      << "                      [--summary-json PATH] [--scenario FILTER]\n"
                      << "\nReads all *-summary.json files under bench-dir and prints a comparison table.\n"
                      << "  --bench-dir DIR      Directory containing benchmark summary JSON files (default: artifacts/bench)\n"
                      << "  --output-csv PATH    Write comparison CSV to this path (default: bench-dir/comparison.csv)\n"
                      << "  --summary-json PATH  Write aggregated per-mode statistics as JSON\n"
                      << "  --scenario FILTER    Only include rows whose scenario name contains FILTER\n";
            return 0;
        }
    }

    if (output_csv.empty()) {
        output_csv = (std::filesystem::path(bench_dir) / "comparison.csv").string();
    }

    // Collect all *-summary.json files.
    std::vector<BenchRow> rows;
    try {
        if (!std::filesystem::exists(bench_dir)) {
            std::cerr << "hermes_compare: bench directory not found: " << bench_dir << std::endl;
            return 1;
        }
        for (const auto& entry : std::filesystem::directory_iterator(bench_dir)) {
            const std::string fname = entry.path().filename().string();
            if (fname.size() > 13 && fname.substr(fname.size() - 13) == "-summary.json") {
                BenchRow row = parse_summary(entry.path());
                if (row.run_id.empty()) continue;
                if (!scenario_filter.empty() && row.scenario.find(scenario_filter) == std::string::npos) continue;
                rows.push_back(row);
            }
        }
    } catch (const std::exception& ex) {
        std::cerr << "hermes_compare: error scanning bench directory: " << ex.what() << std::endl;
        return 1;
    }

    if (rows.empty()) {
        std::cout << "hermes_compare: no benchmark summary files found in " << bench_dir << "\n";
        return 0;
    }

    // Sort: scenario ASC, mode order, run_id ASC.
    std::sort(rows.begin(), rows.end(), [](const BenchRow& a, const BenchRow& b) {
        if (a.scenario != b.scenario) return a.scenario < b.scenario;
        if (a.runtime_mode != b.runtime_mode) return mode_order(a.runtime_mode) < mode_order(b.runtime_mode);
        return a.run_id < b.run_id;
    });

    // Column widths.
    const int W_SCENARIO = 22;
    const int W_MODE     = 15;
    const int W_RUN      = 28;
    const int W_LAUNCHED = 8;
    const int W_COMPL    = 8;
    const int W_RATE     = 7;
    const int W_INTV     = 7;
    const int W_OOM      = 5;
    const int W_DEG      = 5;
    const int W_DUR      = 9;
    const int W_UPS      = 7;

    auto pad = [](const std::string& s, int w) -> std::string {
        if (static_cast<int>(s.size()) >= w) return s.substr(0, w);
        return s + std::string(w - s.size(), ' ');
    };

    auto fmt_double = [](double v, int prec = 2) -> std::string {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(prec) << v;
        return oss.str();
    };

    // Print header.
    std::cout << "\n=== Hermes Benchmark Comparison ===\n\n"
              << pad("scenario", W_SCENARIO) << " "
              << pad("runtime_mode", W_MODE) << " "
              << pad("run_id", W_RUN) << " "
              << pad("launchd", W_LAUNCHED) << " "
              << pad("compltd", W_COMPL) << " "
              << pad("rate", W_RATE) << " "
              << pad("intrvn", W_INTV) << " "
              << pad("oom", W_OOM) << " "
              << pad("degrd", W_DEG) << " "
              << pad("dur_ms", W_DUR) << " "
              << pad("pk_ups", W_UPS) << "\n"
              << std::string(W_SCENARIO + 1 + W_MODE + 1 + W_RUN + 1 + W_LAUNCHED + 1 +
                             W_COMPL + 1 + W_RATE + 1 + W_INTV + 1 + W_OOM + 1 + W_DEG +
                             1 + W_DUR + 1 + W_UPS, '-')
              << "\n";

    for (const BenchRow& row : rows) {
        std::cout << pad(row.scenario, W_SCENARIO) << " "
                  << pad(row.runtime_mode, W_MODE) << " "
                  << pad(row.run_id, W_RUN) << " "
                  << pad(std::to_string(row.launched), W_LAUNCHED) << " "
                  << pad(std::to_string(row.jobs_completed), W_COMPL) << " "
                  << pad(fmt_double(row.completion_rate), W_RATE) << " "
                  << pad(std::to_string(row.intervention_count), W_INTV) << " "
                  << pad(std::to_string(row.oom_count), W_OOM) << " "
                  << pad(row.degraded_behavior ? "yes" : "no", W_DEG) << " "
                  << pad(std::to_string(row.duration_ms), W_DUR) << " "
                  << pad(fmt_double(row.peak_ups), W_UPS) << "\n";
    }
    std::cout << "\n" << rows.size() << " run(s) found.\n";

    // Write CSV.
    try {
        std::filesystem::create_directories(std::filesystem::path(output_csv).parent_path());
    } catch (...) {}

    std::ofstream csv(output_csv);
    if (!csv.is_open()) {
        std::cerr << "hermes_compare: failed to open output CSV: " << output_csv << std::endl;
        return 1;
    }

    csv << "scenario,runtime_mode,run_id,launched,jobs_completed,timed_out,"
        << "exited_nonzero,completion_rate,intervention_count,oom_count,"
        << "degraded_behavior,duration_ms,peak_ups,peak_risk_score,peak_vram_used_mb\n";

    for (const BenchRow& row : rows) {
        csv << csv_escape(row.scenario) << ","
            << csv_escape(row.runtime_mode) << ","
            << csv_escape(row.run_id) << ","
            << row.launched << ","
            << row.jobs_completed << ","
            << row.timed_out << ","
            << row.exited_nonzero << ","
            << fmt_double(row.completion_rate, 4) << ","
            << row.intervention_count << ","
            << row.oom_count << ","
            << (row.degraded_behavior ? "true" : "false") << ","
            << row.duration_ms << ","
            << fmt_double(row.peak_ups, 4) << ","
            << fmt_double(row.peak_risk_score, 4) << ","
            << fmt_double(row.peak_vram_used_mb, 2) << "\n";
    }

    std::cout << "Comparison CSV   : " << output_csv << "\n";

    // --summary-json: write aggregated per-mode statistics to JSON.
    if (!summary_json.empty()) {
        try {
            std::filesystem::create_directories(
                std::filesystem::path(summary_json).parent_path());
        } catch (...) {}

        std::ofstream sj(summary_json);
        if (!sj.is_open()) {
            std::cerr << "hermes_compare: failed to open --summary-json: " << summary_json << "\n";
        } else {
            // Aggregate per mode.
            struct ModeStat {
                std::size_t run_count{0};
                double sum_completion{0.0};
                uint64_t total_oom{0};
                uint64_t total_interventions{0};
                double sum_peak_ups{0.0};
                std::size_t degraded_count{0};
            };
            std::map<std::string, ModeStat> mode_stats;
            for (const BenchRow& r : rows) {
                auto& ms = mode_stats[r.runtime_mode];
                ++ms.run_count;
                ms.sum_completion     += r.completion_rate;
                ms.total_oom          += r.oom_count;
                ms.total_interventions+= r.intervention_count;
                ms.sum_peak_ups       += r.peak_ups;
                if (r.degraded_behavior) ++ms.degraded_count;
            }

            sj << "{\n";
            sj << "  \"run_count\": " << rows.size() << ",\n";
            sj << "  \"modes\": {\n";

            bool first_mode = true;
            for (const auto& kv : mode_stats) {
                if (!first_mode) sj << ",\n";
                const ModeStat& ms = kv.second;
                const double avg_compl = ms.run_count > 0
                    ? ms.sum_completion / static_cast<double>(ms.run_count) : 0.0;
                const double avg_ups = ms.run_count > 0
                    ? ms.sum_peak_ups / static_cast<double>(ms.run_count) : 0.0;
                sj << "    \"" << kv.first << "\": {\n"
                   << "      \"run_count\": " << ms.run_count << ",\n"
                   << "      \"avg_completion_rate\": " << std::fixed << std::setprecision(4) << avg_compl << ",\n"
                   << "      \"total_oom_count\": " << ms.total_oom << ",\n"
                   << "      \"total_interventions\": " << ms.total_interventions << ",\n"
                   << "      \"avg_peak_ups\": " << avg_ups << ",\n"
                   << "      \"degraded_runs\": " << ms.degraded_count << "\n"
                   << "    }";
                first_mode = false;
            }
            sj << "\n  }\n}\n";

            std::cout << "Summary JSON     : " << summary_json << "\n";
        }
    }

    return 0;
}
