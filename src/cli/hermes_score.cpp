// hermes_score: standalone UPS calculator and explainer.
//
// Computes the Unified Pressure Score from raw signal values and prints a
// breakdown showing each component's weighted contribution.  Useful for
// understanding how thresholds work, tuning weights, and debugging why
// Hermes made a particular decision.
//
// Usage:
//   hermes_score --cpu 18.5 --mem 12.0 --io 3.0 --gpu 72.0 --vram 84.0
//   hermes_score --cpu 18.5 --mem 12.0                          (GPU defaults to 0)
//   hermes_score --json                                          (JSON output)
//   hermes_score --config config/schema_tier_c.yaml --cpu 25 --mem 30
//   hermes_score --from-sample artifacts/logs/<run>/samples.ndjson --line 42

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

bool has_arg(int argc, char** argv, const std::string& f) {
    for (int i = 1; i < argc; ++i) if (f == argv[i]) return true;
    return false;
}
std::string get_arg(int argc, char** argv, const std::string& f, const std::string& def) {
    for (int i = 1; i + 1 < argc; ++i) if (f == argv[i]) return argv[i + 1];
    return def;
}
double get_dbl(int argc, char** argv, const std::string& f, double def) {
    const std::string s = get_arg(argc, argv, f, "");
    if (s.empty()) return def;
    try { return std::stod(s); } catch (...) { return def; }
}

struct Weights {
    double cpu{0.30}, mem{0.25}, io{0.08}, gpu{0.22}, vram{0.15};
};

struct Thresholds {
    double elevated{40.0}, critical{70.0};
};

// Read weights and thresholds from a schema YAML file (simple line scan).
void load_schema(const std::string& path, Weights& w, Thresholds& t) {
    std::ifstream f(path);
    if (!f.is_open()) return;
    std::string line;
    bool in_ups = false, in_thresh = false;
    while (std::getline(f, line)) {
        if (line.find("ups_weights:") != std::string::npos) { in_ups = true; in_thresh = false; continue; }
        if (line.find("thresholds:") != std::string::npos) { in_thresh = true; in_ups = false; continue; }
        if (!line.empty() && line[0] != ' ' && line[0] != '\t') { in_ups = in_thresh = false; }
        auto read = [&](const std::string& key) -> double {
            const auto p = line.find(key + ":");
            if (p == std::string::npos) return -1.0;
            try { return std::stod(line.substr(p + key.size() + 1)); } catch (...) { return -1.0; }
        };
        if (in_ups) {
            double v;
            if ((v = read("cpu"))  >= 0) w.cpu  = v;
            if ((v = read("mem"))  >= 0) w.mem  = v;
            if ((v = read("io"))   >= 0) w.io   = v;
            if ((v = read("gpu"))  >= 0) w.gpu  = v;
            if ((v = read("vram")) >= 0) w.vram = v;
        }
        if (in_thresh) {
            double v;
            if ((v = read("elevated")) >= 0) t.elevated = v;
            if ((v = read("critical")) >= 0) t.critical  = v;
        }
    }
}

// Parse a single NDJSON line into signal values.
bool parse_sample_line(const std::string& line,
                       double& cpu, double& mem, double& io,
                       double& gpu, double& vram_pct) {
    auto jd = [&](const std::string& k) -> double {
        const std::string kk = "\"" + k + "\":";
        const auto p = line.find(kk);
        if (p == std::string::npos) return 0.0;
        try { return std::stod(line.substr(p + kk.size())); } catch (...) { return 0.0; }
    };
    cpu = jd("cpu_some_avg10");
    mem = jd("mem_some_avg10");
    io  = jd("io_some_avg10");
    gpu = jd("gpu_util_pct");
    const double vram_used = jd("vram_used_mb");
    const double vram_free = jd("vram_free_mb");
    const double vram_total = vram_used + vram_free;
    vram_pct = (vram_total > 0.0) ? (vram_used / vram_total * 100.0) : 0.0;
    return (cpu + mem + io + gpu + vram_used) > 0.0;
}

struct ScoreResult {
    double cpu_raw, mem_raw, io_raw, gpu_raw, vram_raw;
    double cpu_contrib, mem_contrib, io_contrib, gpu_contrib, vram_contrib;
    double ups;
    std::string band;
    std::string dominant;
    Weights w;
    Thresholds t;
};

ScoreResult compute(double cpu, double mem, double io, double gpu, double vram,
                    const Weights& w, const Thresholds& t) {
    ScoreResult r;
    r.w = w; r.t = t;
    r.cpu_raw = cpu; r.mem_raw = mem; r.io_raw = io;
    r.gpu_raw = gpu; r.vram_raw = vram;

    // Normalise: CPU/mem/IO are already 0–100 (PSI %). GPU util is 0–100. VRAM% is 0–100.
    r.cpu_contrib  = w.cpu  * std::min(cpu,  100.0);
    r.mem_contrib  = w.mem  * std::min(mem,  100.0);
    r.io_contrib   = w.io   * std::min(io,   100.0);
    r.gpu_contrib  = w.gpu  * std::min(gpu,  100.0);
    r.vram_contrib = w.vram * std::min(vram, 100.0);

    r.ups = r.cpu_contrib + r.mem_contrib + r.io_contrib + r.gpu_contrib + r.vram_contrib;

    r.band = r.ups >= t.critical ? "critical" : r.ups >= t.elevated ? "elevated" : "normal";

    // Dominant signal = highest weighted contribution.
    struct Sig { const char* name; double contrib; };
    std::vector<Sig> sigs = {
        {"cpu",  r.cpu_contrib},
        {"mem",  r.mem_contrib},
        {"io",   r.io_contrib},
        {"gpu",  r.gpu_contrib},
        {"vram", r.vram_contrib},
    };
    r.dominant = std::max_element(sigs.begin(), sigs.end(),
        [](const Sig& a, const Sig& b) { return a.contrib < b.contrib; })->name;

    return r;
}

void print_human(const ScoreResult& r, const std::string& label = "") {
    const std::string sep(60, '=');
    std::cout << sep << "\n";
    std::cout << "hermes_score — UPS breakdown";
    if (!label.empty()) std::cout << "  [" << label << "]";
    std::cout << "\n" << sep << "\n";

    std::cout << std::fixed << std::setprecision(2);
    std::cout << std::left << std::setw(12) << "Signal"
              << std::right << std::setw(10) << "Raw value"
              << std::setw(10) << "Weight"
              << std::setw(12) << "Contribution"
              << std::setw(8)  << "% of UPS\n";
    std::cout << std::string(52, '-') << "\n";

    auto row = [&](const char* name, double raw, double w, double contrib) {
        const double pct = (r.ups > 0.0) ? (contrib / r.ups * 100.0) : 0.0;
        std::cout << std::left  << std::setw(12) << name
                  << std::right << std::setw(10) << raw
                  << std::setw(10) << w
                  << std::setw(12) << contrib
                  << std::setw(7)  << pct << "%\n";
    };
    row("cpu_psi",  r.cpu_raw,  r.w.cpu,  r.cpu_contrib);
    row("mem_psi",  r.mem_raw,  r.w.mem,  r.mem_contrib);
    row("io_psi",   r.io_raw,   r.w.io,   r.io_contrib);
    row("gpu_util", r.gpu_raw,  r.w.gpu,  r.gpu_contrib);
    row("vram_pct", r.vram_raw, r.w.vram, r.vram_contrib);
    std::cout << std::string(52, '-') << "\n";

    std::cout << std::left  << std::setw(22) << "UPS"
              << std::right << std::setw(10) << r.ups << "\n";
    std::cout << std::left  << std::setw(22) << "Band"
              << std::right << std::setw(10) << r.band << "\n";
    std::cout << std::left  << std::setw(22) << "Dominant signal"
              << std::right << std::setw(10) << r.dominant << "\n";
    std::cout << std::left  << std::setw(22) << "Elevated threshold"
              << std::right << std::setw(10) << r.t.elevated << "\n";
    std::cout << std::left  << std::setw(22) << "Critical threshold"
              << std::right << std::setw(10) << r.t.critical << "\n";
    std::cout << std::left  << std::setw(22) << "Headroom (elevated)"
              << std::right << std::setw(10) << (r.t.elevated - r.ups) << "\n";
    std::cout << std::left  << std::setw(22) << "Headroom (critical)"
              << std::right << std::setw(10) << (r.t.critical - r.ups) << "\n";
    std::cout << sep << "\n";
}

void print_json(const ScoreResult& r) {
    std::cout << std::fixed << std::setprecision(4);
    std::cout << "{\n";
    std::cout << "  \"ups\": " << r.ups << ",\n";
    std::cout << "  \"band\": \"" << r.band << "\",\n";
    std::cout << "  \"dominant_signal\": \"" << r.dominant << "\",\n";
    std::cout << "  \"components\": {\n";
    std::cout << "    \"cpu\":  {\"raw\": " << r.cpu_raw  << ", \"weight\": " << r.w.cpu  << ", \"contribution\": " << r.cpu_contrib  << "},\n";
    std::cout << "    \"mem\":  {\"raw\": " << r.mem_raw  << ", \"weight\": " << r.w.mem  << ", \"contribution\": " << r.mem_contrib  << "},\n";
    std::cout << "    \"io\":   {\"raw\": " << r.io_raw   << ", \"weight\": " << r.w.io   << ", \"contribution\": " << r.io_contrib   << "},\n";
    std::cout << "    \"gpu\":  {\"raw\": " << r.gpu_raw  << ", \"weight\": " << r.w.gpu  << ", \"contribution\": " << r.gpu_contrib  << "},\n";
    std::cout << "    \"vram\": {\"raw\": " << r.vram_raw << ", \"weight\": " << r.w.vram << ", \"contribution\": " << r.vram_contrib << "}\n";
    std::cout << "  },\n";
    std::cout << "  \"thresholds\": {\"elevated\": " << r.t.elevated << ", \"critical\": " << r.t.critical << "},\n";
    std::cout << "  \"headroom\": {\"elevated\": " << (r.t.elevated - r.ups) << ", \"critical\": " << (r.t.critical - r.ups) << "}\n";
    std::cout << "}\n";
}

} // namespace

int main(int argc, char* argv[]) {
    if (has_arg(argc, argv, "--help")) {
        std::cout <<
            "Usage: hermes_score [signals] [options]\n\n"
            "Compute UPS from raw signal values and explain the breakdown.\n\n"
            "Signals (all default to 0.0):\n"
            "  --cpu  <pct>   CPU PSI some_avg10 (0–100)\n"
            "  --mem  <pct>   Memory PSI some_avg10 (0–100)\n"
            "  --io   <pct>   IO PSI some_avg10 (0–100)\n"
            "  --gpu  <pct>   GPU utilization percent (0–100)\n"
            "  --vram <pct>   VRAM used percent (0–100)\n\n"
            "Options:\n"
            "  --config <path>            Load weights/thresholds from schema YAML\n"
            "  --from-sample <ndjson>     Read a samples.ndjson file; score every line\n"
            "  --line <n>                 With --from-sample, score only line N (1-based)\n"
            "  --json                     Output as JSON\n";
        return 0;
    }

    Weights w;
    Thresholds t;

    // Load schema if given.
    const std::string cfg = get_arg(argc, argv, "--config", "");
    if (!cfg.empty()) load_schema(cfg, w, t);
    // Also try default schema.
    else load_schema("config/schema.yaml", w, t);

    const bool as_json = has_arg(argc, argv, "--json");

    // --from-sample mode: score lines from a samples.ndjson.
    const std::string sample_path = get_arg(argc, argv, "--from-sample", "");
    if (!sample_path.empty()) {
        const int target_line = [&]() {
            const std::string s = get_arg(argc, argv, "--line", "0");
            try { return std::stoi(s); } catch (...) { return 0; }
        }();
        std::ifstream f(sample_path);
        if (!f.is_open()) {
            std::cerr << "hermes_score: cannot open " << sample_path << "\n";
            return 1;
        }
        std::string line;
        int lineno = 0;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            ++lineno;
            if (target_line > 0 && lineno != target_line) continue;
            double cpu = 0, mem = 0, io = 0, gpu = 0, vram = 0;
            if (!parse_sample_line(line, cpu, mem, io, gpu, vram)) continue;
            const ScoreResult r = compute(cpu, mem, io, gpu, vram, w, t);
            if (as_json) { print_json(r); }
            else { print_human(r, "line " + std::to_string(lineno)); }
            if (target_line > 0 && lineno == target_line) break;
        }
        return 0;
    }

    // Direct signal mode.
    const double cpu  = get_dbl(argc, argv, "--cpu",  0.0);
    const double mem  = get_dbl(argc, argv, "--mem",  0.0);
    const double io   = get_dbl(argc, argv, "--io",   0.0);
    const double gpu  = get_dbl(argc, argv, "--gpu",  0.0);
    const double vram = get_dbl(argc, argv, "--vram", 0.0);

    const ScoreResult r = compute(cpu, mem, io, gpu, vram, w, t);
    if (as_json) print_json(r);
    else         print_human(r);

    return (r.band == "critical") ? 2 : (r.band == "elevated") ? 1 : 0;
}
