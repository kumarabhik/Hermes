// hermes_report: Multi-run artifact comparison report.
//
// Scans all run directories under <artifact-root>/logs/, reads the
// replay_summary.json from each, and prints a formatted comparison table.
// Also scans <artifact-root>/bench/*-summary.json and appends a benchmark
// comparison section. Writes a combined CSV to <artifact-root>/report.csv.
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

// ---- State coverage record (from state_coverage.json written by hermes_reeval) ----

struct StateCovRecord {
    std::string run_id;
    std::string run_dir;
    std::size_t states_visited{0};
    std::size_t states_total{0};
    std::size_t transitions_observed{0};
    // Per-state frame counts
    uint64_t cnt_normal{0};
    uint64_t cnt_elevated{0};
    uint64_t cnt_throttled{0};
    uint64_t cnt_recovery{0};
    uint64_t cnt_cooldown{0};
};

// Parse a bare uint64 from flat JSON (e.g. "\"normal\": 42").
static uint64_t cov_ull(const std::string& json, const std::string& key) {
    const std::string search = "\"" + key + "\": ";
    auto pos = json.find(search);
    if (pos == std::string::npos) { search; pos = json.find("\"" + key + "\":"); }
    if (pos == std::string::npos) return 0;
    auto start = json.find_first_not_of(" ", pos + search.size());
    if (start == std::string::npos) return 0;
    try { return std::stoull(json.substr(start)); } catch (...) { return 0; }
}

StateCovRecord load_state_coverage(const std::filesystem::path& path,
                                    const std::string& run_id) {
    StateCovRecord rec;
    rec.run_id  = run_id;
    rec.run_dir = path.parent_path().string();

    std::ifstream f(path);
    if (!f.is_open()) return rec;
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    // Top-level fields
    auto top_ull = [&](const std::string& key) -> std::size_t {
        const std::string s1 = "\"" + key + "\": ";
        const std::string s2 = "\"" + key + "\":";
        auto pos = content.find(s1);
        if (pos == std::string::npos) pos = content.find(s2);
        if (pos == std::string::npos) return 0;
        const std::string pfx = (content.find(s1) != std::string::npos) ? s1 : s2;
        const auto start = content.find_first_not_of(" ", pos + pfx.size());
        if (start == std::string::npos) return 0;
        try { return static_cast<std::size_t>(std::stoull(content.substr(start))); }
        catch (...) { return 0; }
    };

    rec.states_visited       = top_ull("states_visited");
    rec.states_total         = top_ull("states_total");
    rec.transitions_observed = top_ull("transitions_observed");

    // Per-state counts inside "state_counts" object
    const std::string sc_key = "\"state_counts\": {";
    auto sc_pos = content.find(sc_key);
    if (sc_pos == std::string::npos) sc_pos = content.find("\"state_counts\":{");
    if (sc_pos != std::string::npos) {
        const std::string sc = content.substr(sc_pos);
        rec.cnt_normal    = cov_ull(sc, "normal");
        rec.cnt_elevated  = cov_ull(sc, "elevated");
        rec.cnt_throttled = cov_ull(sc, "throttled");
        rec.cnt_recovery  = cov_ull(sc, "recovery");
        rec.cnt_cooldown  = cov_ull(sc, "cooldown");
    }

    return rec;
}

void print_coverage_table(const std::vector<StateCovRecord>& records) {
    constexpr int W_RUN  = 26;
    constexpr int W_N    = 8;
    auto col = [](const std::string& s, int w) {
        if (static_cast<int>(s.size()) >= w) return s.substr(0, w - 1) + "~";
        return s + std::string(static_cast<std::size_t>(w - static_cast<int>(s.size())), ' ');
    };
    auto nc = [&](uint64_t n, int w) { return col(std::to_string(n), w); };

    const std::string sep(W_RUN + 6*(W_N+3) + 2, '-');

    std::cout << col("run_id",     W_RUN) << " | "
              << col("vis/tot",   W_N)  << " | "
              << col("trans",     W_N)  << " | "
              << col("normal",    W_N)  << " | "
              << col("elevated",  W_N)  << " | "
              << col("throttled", W_N)  << " | "
              << col("recovery",  W_N)  << " | "
              << col("cooldown",  W_N)  << "\n";
    std::cout << sep << "\n";

    for (const StateCovRecord& r : records) {
        const std::string vis =
            std::to_string(r.states_visited) + "/" + std::to_string(r.states_total);
        std::cout << col(r.run_id, W_RUN)       << " | "
                  << col(vis, W_N)              << " | "
                  << nc(r.transitions_observed, W_N) << " | "
                  << nc(r.cnt_normal,    W_N)   << " | "
                  << nc(r.cnt_elevated,  W_N)   << " | "
                  << nc(r.cnt_throttled, W_N)   << " | "
                  << nc(r.cnt_recovery,  W_N)   << " | "
                  << nc(r.cnt_cooldown,  W_N)   << "\n";
    }
    std::cout << sep << "\n";
}

bool write_coverage_csv(const std::filesystem::path& csv_path,
                        const std::vector<StateCovRecord>& records) {
    std::ofstream f(csv_path, std::ios::app);
    if (!f.is_open()) return false;
    f << "\n# state_coverage\n";
    f << "run_id,states_visited,states_total,transitions_observed,"
      << "cnt_normal,cnt_elevated,cnt_throttled,cnt_recovery,cnt_cooldown\n";
    for (const StateCovRecord& r : records) {
        f << r.run_id << ","
          << r.states_visited << ","
          << r.states_total << ","
          << r.transitions_observed << ","
          << r.cnt_normal << ","
          << r.cnt_elevated << ","
          << r.cnt_throttled << ","
          << r.cnt_recovery << ","
          << r.cnt_cooldown << "\n";
    }
    return f.good();
}

// ---- Benchmark summary record ----

struct BenchRecord {
    std::string run_id;
    std::string scenario;
    std::string runtime_mode;
    uint64_t launched{0};
    uint64_t jobs_completed{0};
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

BenchRecord load_bench_summary(const std::filesystem::path& path) {
    BenchRecord rec;
    rec.source_file = path.filename().string();
    std::ifstream f(path);
    if (!f.is_open()) return rec;
    std::ostringstream oss;
    oss << f.rdbuf();
    const std::string json = oss.str();

    rec.run_id             = jstr(json, "run_id");
    rec.scenario           = jstr(json, "scenario");
    rec.runtime_mode       = jstr(json, "runtime_mode");
    rec.launched           = jull(json, "launched");
    rec.jobs_completed     = jull(json, "jobs_completed");
    rec.completion_rate    = jdbl(json, "completion_rate");
    rec.intervention_count = jull(json, "intervention_count");
    rec.oom_count          = jull(json, "oom_count");
    rec.degraded_behavior  = jbool(json, "degraded_behavior");
    rec.duration_ms        = jull(json, "duration_ms");

    // Replay snapshot nested fields.
    const auto rs_pos = json.find("\"replay_snapshot\":");
    if (rs_pos != std::string::npos) {
        const auto brace = json.find('{', rs_pos);
        const auto end   = json.find('}', brace);
        if (brace != std::string::npos && end != std::string::npos) {
            const std::string rs = json.substr(brace, end - brace + 1);
            rec.peak_ups          = jdbl(rs, "peak_ups");
            rec.peak_risk_score   = jdbl(rs, "peak_risk_score");
            rec.peak_vram_used_mb = jdbl(rs, "peak_vram_used_mb");
        }
    }
    return rec;
}

void print_bench_table(const std::vector<BenchRecord>& records) {
    constexpr int W_RUN  = 28;
    constexpr int W_SCEN = 16;
    constexpr int W_MODE = 14;
    constexpr int W_LAUN =  7;
    constexpr int W_COMP =  7;
    constexpr int W_RATE =  6;
    constexpr int W_INTV =  6;
    constexpr int W_OOM  =  5;
    constexpr int W_DEG  =  5;
    constexpr int W_DUR  =  8;
    constexpr int W_UPS  =  6;

    auto col = [](const std::string& s, int w) -> std::string {
        if (static_cast<int>(s.size()) >= w) return s.substr(0, w - 1) + " ";
        return s + std::string(static_cast<std::size_t>(w) - s.size(), ' ');
    };
    auto fmtd = [](double v, int p) -> std::string {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(p) << v;
        return oss.str();
    };

    const std::string sep(W_RUN + W_SCEN + W_MODE + W_LAUN + W_COMP + W_RATE +
                          W_INTV + W_OOM + W_DEG + W_DUR + W_UPS + 22, '-');
    std::cout << sep << "\n";
    std::cout
        << col("run_id",   W_RUN)  << " | "
        << col("scenario", W_SCEN) << " | "
        << col("mode",     W_MODE) << " | "
        << col("launchd",  W_LAUN) << " | "
        << col("compltd",  W_COMP) << " | "
        << col("rate",     W_RATE) << " | "
        << col("intrvn",   W_INTV) << " | "
        << col("oom",      W_OOM)  << " | "
        << col("degrd",    W_DEG)  << " | "
        << col("dur_ms",   W_DUR)  << " | "
        << col("pk_ups",   W_UPS)  << "\n";
    std::cout << sep << "\n";

    for (const BenchRecord& r : records) {
        std::cout
            << col(r.run_id.size() > static_cast<std::size_t>(W_RUN)
                   ? r.run_id.substr(0, W_RUN - 1) + "~" : r.run_id, W_RUN) << " | "
            << col(r.scenario.size() > static_cast<std::size_t>(W_SCEN)
                   ? r.scenario.substr(0, W_SCEN - 1) + "~" : r.scenario, W_SCEN) << " | "
            << col(r.runtime_mode, W_MODE) << " | "
            << col(std::to_string(r.launched),           W_LAUN) << " | "
            << col(std::to_string(r.jobs_completed),     W_COMP) << " | "
            << col(fmtd(r.completion_rate, 2),           W_RATE) << " | "
            << col(std::to_string(r.intervention_count), W_INTV) << " | "
            << col(std::to_string(r.oom_count),          W_OOM)  << " | "
            << col(r.degraded_behavior ? "yes" : "no",  W_DEG)  << " | "
            << col(std::to_string(r.duration_ms),        W_DUR)  << " | "
            << col(fmtd(r.peak_ups, 1),                  W_UPS)  << "\n";
    }
    std::cout << sep << "\n";
}

bool write_bench_csv(const std::filesystem::path& csv_path,
                     const std::vector<BenchRecord>& records) {
    std::ofstream f(csv_path, std::ios::app);
    if (!f.is_open()) return false;
    f << "\n# benchmark_runs\n";
    f << "run_id,scenario,runtime_mode,launched,jobs_completed,completion_rate,"
      << "intervention_count,oom_count,degraded_behavior,duration_ms,"
      << "peak_ups,peak_risk_score,peak_vram_used_mb\n";
    f << std::fixed << std::setprecision(4);
    for (const BenchRecord& r : records) {
        f << r.run_id             << ","
          << r.scenario           << ","
          << r.runtime_mode       << ","
          << r.launched           << ","
          << r.jobs_completed     << ","
          << r.completion_rate    << ","
          << r.intervention_count << ","
          << r.oom_count          << ","
          << (r.degraded_behavior ? "true" : "false") << ","
          << r.duration_ms        << ","
          << r.peak_ups           << ","
          << r.peak_risk_score    << ","
          << r.peak_vram_used_mb  << "\n";
    }
    return f.good();
}

void print_usage() {
    std::cout
        << "Usage: hermes_report [artifact-root] [--csv path/to/report.csv]\n"
        << "\n"
        << "Reads replay_summary.json from every run directory under\n"
        << "<artifact-root>/logs/ and prints a comparison table.\n"
        << "Also reads *-summary.json from <artifact-root>/bench/ and appends\n"
        << "a benchmark comparison section. Writes a combined CSV.\n"
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

    // Write replay CSV first (creates/overwrites the file).
    if (!records.empty()) {
        if (write_csv(csv_path, records)) {
            std::cout << "\nCSV written to: " << csv_path.string() << "\n";
        } else {
            std::cerr << "hermes_report: failed to write CSV to " << csv_path << "\n";
        }
    }

    // ---- Benchmark summary section ----
    const std::filesystem::path bench_dir = artifact_root / "bench";
    std::vector<BenchRecord> bench_records;

    if (std::filesystem::exists(bench_dir)) {
        for (const auto& entry : std::filesystem::directory_iterator(bench_dir)) {
            const std::string fname = entry.path().filename().string();
            if (fname.size() > 13 && fname.substr(fname.size() - 13) == "-summary.json") {
                BenchRecord rec = load_bench_summary(entry.path());
                if (!rec.run_id.empty()) {
                    bench_records.push_back(rec);
                }
            }
        }
        std::sort(bench_records.begin(), bench_records.end(),
            [](const BenchRecord& a, const BenchRecord& b) {
                if (a.scenario != b.scenario) return a.scenario < b.scenario;
                if (a.runtime_mode != b.runtime_mode) return a.runtime_mode < b.runtime_mode;
                return a.run_id < b.run_id;
            });
    }

    if (!bench_records.empty()) {
        std::cout << "\n=== Benchmark Runs ===\n";
        std::cout << "Bench summaries : " << bench_records.size() << "\n\n";
        print_bench_table(bench_records);

        // Append benchmark rows to the CSV.
        if (write_bench_csv(csv_path, bench_records)) {
            std::cout << "\nBenchmark rows appended to: " << csv_path.string() << "\n";
        } else {
            std::cerr << "hermes_report: failed to append benchmark CSV rows\n";
        }
    }

    // ---- State coverage section ----
    // Collect state_coverage.json files from all run directories.
    std::vector<StateCovRecord> cov_records;
    if (std::filesystem::exists(logs_dir)) {
        for (const auto& entry : std::filesystem::directory_iterator(logs_dir)) {
            if (!entry.is_directory()) continue;
            const std::filesystem::path cov_path = entry.path() / "state_coverage.json";
            if (!std::filesystem::exists(cov_path)) continue;
            const std::string rid = entry.path().filename().string();
            StateCovRecord rec = load_state_coverage(cov_path, rid);
            if (rec.states_total > 0) cov_records.push_back(std::move(rec));
        }
        std::sort(cov_records.begin(), cov_records.end(),
            [](const StateCovRecord& a, const StateCovRecord& b) {
                return a.run_id < b.run_id;
            });
    }

    if (!cov_records.empty()) {
        std::cout << "\n=== Scheduler State Coverage ===\n";
        std::cout << "Runs with coverage : " << cov_records.size() << "\n\n";
        print_coverage_table(cov_records);

        if (write_coverage_csv(csv_path, cov_records)) {
            std::cout << "\nCoverage rows appended to: " << csv_path.string() << "\n";
        } else {
            std::cerr << "hermes_report: failed to append coverage CSV rows\n";
        }
    }

    // Exit non-zero if any replay run is invalid.
    const bool any_invalid = std::any_of(records.begin(), records.end(),
        [](const RunRecord& r) { return !r.valid; });
    return any_invalid ? 1 : 0;
}
