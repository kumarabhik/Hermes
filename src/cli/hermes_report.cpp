// hermes_report: Multi-run artifact comparison report.
//
// Scans all run directories under <artifact-root>/logs/, reads the
// replay_summary.json from each, and prints a formatted comparison table.
// Also writes a CSV to <artifact-root>/report.csv for spreadsheet import.
//
// Usage:
//   hermes_report [artifact-root] [--csv path/to/report.csv]
//
// If replay_summary.json is missing from a run directory, hermes_replay is
// suggested in the output but no subprocess is launched.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// ---- Minimal flat-JSON field extractors ----

std::string jstr(const std::string& json, const std::string& key) {
    const std::string search = "\"" + key + "\": \"";
    // Try compact form first
    auto pos = json.find("\"" + key + "\":\"");
    if (pos != std::string::npos) {
        const auto start = pos + key.size() + 3;
        const auto end = json.find('"', start);
        if (end != std::string::npos) return json.substr(start, end - start);
    }
    // Try spaced form
    pos = json.find(search);
    if (pos == std::string::npos) return "";
    const auto start = pos + search.size();
    const auto end = json.find('"', start);
    if (end == std::string::npos) return "";
    return json.substr(start, end - start);
}

double jdbl(const std::string& json, const std::string& key) {
    // Try compact
    auto pos = json.find("\"" + key + "\":");
    if (pos == std::string::npos) return 0.0;
    const auto start = pos + key.size() + 3;
    if (start >= json.size()) return 0.0;
    // Skip whitespace and quotes
    std::size_t i = start;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
    if (i >= json.size() || json[i] == '"') return 0.0;
    try { return std::stod(json.substr(i)); } catch (...) { return 0.0; }
}

uint64_t jull(const std::string& json, const std::string& key) {
    auto pos = json.find("\"" + key + "\":");
    if (pos == std::string::npos) return 0;
    const auto start = pos + key.size() + 3;
    if (start >= json.size()) return 0;
    std::size_t i = start;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
    if (i >= json.size() || json[i] == '"') return 0;
    try { return std::stoull(json.substr(i)); } catch (...) { return 0; }
}

bool jbool(const std::string& json, const std::string& key) {
    auto pos = json.find("\"" + key + "\":");
    if (pos == std::string::npos) return false;
    const auto start = pos + key.size() + 3;
    std::size_t i = start;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
    return (i < json.size() && json[i] == 't');
}

// ---- Run summary record ----

struct RunRecord {
    std::string run_id;
    std::string scenario;
    bool valid{false};
    std::size_t samples{0};
    std::size_t decisions{0};
    std::size_t actions{0};
    double peak_ups{0.0};
    double peak_risk{0.0};
    double peak_mem_full{0.0};
    double peak_gpu_util{0.0};
    double peak_vram_used_mb{0.0};
    double min_vram_free_mb{0.0};
    bool tq_present{false};
    std::filesystem::path run_dir;
    std::string summary_path;
};

RunRecord load_summary(const std::filesystem::path& summary_path,
                       const std::filesystem::path& run_dir) {
    RunRecord rec;
    rec.run_dir     = run_dir;
    rec.summary_path = summary_path.string();

    // Read entire file
    std::ifstream f(summary_path);
    if (!f.is_open()) return rec;
    std::ostringstream oss;
    oss << f.rdbuf();
    const std::string json = oss.str();

    rec.run_id         = jstr(json, "run_id");
    rec.scenario       = jstr(json, "scenario");
    rec.valid          = jbool(json, "valid");
    rec.samples        = static_cast<std::size_t>(jull(json, "samples"));
    rec.decisions      = static_cast<std::size_t>(jull(json, "decisions"));
    rec.actions        = static_cast<std::size_t>(jull(json, "actions"));
    rec.peak_ups       = jdbl(json, "peak_ups");
    rec.peak_risk      = jdbl(json, "peak_risk_score");
    rec.peak_mem_full  = jdbl(json, "peak_mem_full_avg10");
    rec.peak_gpu_util  = jdbl(json, "peak_gpu_util_pct");
    rec.peak_vram_used_mb = jdbl(json, "peak_vram_used_mb");
    rec.min_vram_free_mb  = jdbl(json, "min_vram_free_mb");
    rec.tq_present     = jbool(json, "telemetry_quality_present");
    return rec;
}

std::string trunc(const std::string& s, std::size_t width) {
    if (s.size() <= width) return s;
    return s.substr(0, width - 1) + "~";
}

void print_table(const std::vector<RunRecord>& records) {
    // Column widths
    constexpr int W_RUN    = 28;
    constexpr int W_SCEN   = 18;
    constexpr int W_VALID  =  5;
    constexpr int W_SAMP   =  7;
    constexpr int W_DECIS  =  7;
    constexpr int W_ACT    =  7;
    constexpr int W_UPS    =  7;
    constexpr int W_RISK   =  6;
    constexpr int W_MEM    =  8;
    constexpr int W_GPU    =  7;
    constexpr int W_TQ     =  3;

    auto col = [](const std::string& s, int w) -> std::string {
        if (static_cast<int>(s.size()) >= w) return s.substr(0, w - 1) + " ";
        return s + std::string(static_cast<std::size_t>(w) - s.size(), ' ');
    };

    const std::string sep(W_RUN + W_SCEN + W_VALID + W_SAMP + W_DECIS + W_ACT +
                          W_UPS + W_RISK + W_MEM + W_GPU + W_TQ + 12, '-');
    std::cout << sep << "\n";
    std::cout
        << col("run_id",    W_RUN)   << " | "
        << col("scenario",  W_SCEN)  << " | "
        << col("valid",     W_VALID) << " | "
        << col("samples",   W_SAMP)  << " | "
        << col("decides",   W_DECIS) << " | "
        << col("actions",   W_ACT)   << " | "
        << col("pk_ups",    W_UPS)   << " | "
        << col("pk_risk",   W_RISK)  << " | "
        << col("pk_mem_fl", W_MEM)   << " | "
        << col("pk_gpu%",   W_GPU)   << " | "
        << col("TQ", W_TQ)
        << "\n";
    std::cout << sep << "\n";

    for (const RunRecord& r : records) {
        std::ostringstream ups_s, risk_s, mem_s, gpu_s;
        ups_s  << std::fixed << std::setprecision(1) << r.peak_ups;
        risk_s << std::fixed << std::setprecision(2) << r.peak_risk;
        mem_s  << std::fixed << std::setprecision(1) << r.peak_mem_full;
        gpu_s  << std::fixed << std::setprecision(1) << r.peak_gpu_util;

        std::cout
            << col(trunc(r.run_id,   W_RUN),  W_RUN)  << " | "
            << col(trunc(r.scenario, W_SCEN), W_SCEN) << " | "
            << col(r.valid ? "yes" : "no",    W_VALID) << " | "
            << col(std::to_string(r.samples), W_SAMP)  << " | "
            << col(std::to_string(r.decisions), W_DECIS) << " | "
            << col(std::to_string(r.actions), W_ACT)   << " | "
            << col(ups_s.str(),  W_UPS)  << " | "
            << col(risk_s.str(), W_RISK) << " | "
            << col(mem_s.str(),  W_MEM)  << " | "
            << col(gpu_s.str(),  W_GPU)  << " | "
            << col(r.tq_present ? "ok" : "--", W_TQ)
            << "\n";
    }
    std::cout << sep << "\n";
}

bool write_csv(const std::filesystem::path& csv_path, const std::vector<RunRecord>& records) {
    try {
        std::filesystem::create_directories(csv_path.parent_path());
    } catch (...) {}

    std::ofstream f(csv_path);
    if (!f.is_open()) return false;

    f << "run_id,scenario,valid,samples,decisions,actions,"
      << "peak_ups,peak_risk_score,peak_mem_full_avg10,"
      << "peak_gpu_util_pct,peak_vram_used_mb,min_vram_free_mb,"
      << "telemetry_quality_present,run_directory\n";

    f << std::fixed << std::setprecision(3);
    for (const RunRecord& r : records) {
        f << r.run_id       << ","
          << r.scenario     << ","
          << (r.valid ? "true" : "false") << ","
          << r.samples      << ","
          << r.decisions    << ","
          << r.actions      << ","
          << r.peak_ups     << ","
          << r.peak_risk    << ","
          << r.peak_mem_full << ","
          << r.peak_gpu_util << ","
          << r.peak_vram_used_mb << ","
          << r.min_vram_free_mb  << ","
          << (r.tq_present ? "true" : "false") << ","
          << r.run_dir.string() << "\n";
    }
    return f.good();
}

void print_usage() {
    std::cout
        << "Usage: hermes_report [artifact-root] [--csv path/to/report.csv]\n"
        << "\n"
        << "Reads replay_summary.json from every run directory under\n"
        << "<artifact-root>/logs/ and prints a comparison table.\n"
        << "Also writes a CSV for spreadsheet import.\n"
        << "\n"
        << "Options:\n"
        << "  artifact-root      Artifact root (default: $HERMES_ARTIFACT_ROOT or 'artifacts')\n"
        << "  --csv <path>       CSV output path (default: <artifact-root>/report.csv)\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 &&
        (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
        print_usage();
        return 0;
    }

    // Parse arguments: first positional = artifact_root, --csv <path>
    const char* env_root = std::getenv("HERMES_ARTIFACT_ROOT");
    std::string artifact_root_str = (env_root && env_root[0]) ? env_root : "artifacts";
    std::string csv_path_str;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--csv" && i + 1 < argc) {
            csv_path_str = argv[++i];
        } else if (a.rfind("--", 0) != 0) {
            artifact_root_str = a;
        }
    }

    const std::filesystem::path artifact_root = artifact_root_str;
    const std::filesystem::path logs_dir      = artifact_root / "logs";
    const std::filesystem::path csv_path      = csv_path_str.empty()
        ? artifact_root / "report.csv"
        : std::filesystem::path(csv_path_str);

    if (!std::filesystem::exists(logs_dir)) {
        std::cerr << "hermes_report: logs directory not found: " << logs_dir << "\n";
        std::cerr << "Run hermesd or hermesd_mt first, then hermes_replay on each run.\n";
        return 2;
    }

    // Collect run directories
    std::vector<RunRecord> records;
    std::vector<std::filesystem::path> missing_summary;

    for (const auto& entry : std::filesystem::directory_iterator(logs_dir)) {
        if (!entry.is_directory()) continue;
        const std::filesystem::path summary_path = entry.path() / "replay_summary.json";
        if (!std::filesystem::exists(summary_path)) {
            missing_summary.push_back(entry.path());
            continue;
        }
        RunRecord rec = load_summary(summary_path, entry.path());
        if (rec.run_id.empty()) rec.run_id = entry.path().filename().string();
        records.push_back(std::move(rec));
    }

    // Sort by run_id
    std::sort(records.begin(), records.end(),
        [](const RunRecord& a, const RunRecord& b) { return a.run_id < b.run_id; });

    if (records.empty() && missing_summary.empty()) {
        std::cout << "hermes_report: no run directories found in " << logs_dir << "\n";
        return 0;
    }

    std::cout << "=== Hermes Multi-Run Report ===\n";
    std::cout << "Artifact root : " << artifact_root.string() << "\n";
    std::cout << "Runs found    : " << records.size() + missing_summary.size() << "\n";
    std::cout << "With summary  : " << records.size() << "\n\n";

    if (!records.empty()) {
        print_table(records);
    }

    if (!missing_summary.empty()) {
        std::cout << "\nRun directories without replay_summary.json ("
                  << missing_summary.size() << "):\n";
        for (const auto& p : missing_summary) {
            std::cout << "  " << p.string() << "\n";
        }
        std::cout << "  -> Run: hermes_replay <run-directory> to generate summaries.\n";
    }

    // Write CSV
    if (!records.empty()) {
        if (write_csv(csv_path, records)) {
            std::cout << "\nCSV written to: " << csv_path.string() << "\n";
        } else {
            std::cerr << "hermes_report: failed to write CSV to " << csv_path << "\n";
        }
    }

    // Exit non-zero if any run is invalid
    const bool any_invalid = std::any_of(records.begin(), records.end(),
        [](const RunRecord& r) { return !r.valid; });
    return any_invalid ? 1 : 0;
}
