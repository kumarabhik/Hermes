// hermes_budget: VRAM + CPU budget calculator.
//
// Reads the most recent processes.ndjson and telemetry_quality.json, then
// computes how much VRAM and CPU capacity is left before Hermes would trigger
// an elevated or critical intervention.  Useful for answering "can I launch
// another training job right now?"
//
// Usage:
//   hermes_budget [run-dir]
//   hermes_budget [run-dir] --vram-total-mb 24576   # override if NVML unavailable
//   hermes_budget [run-dir] --config config/schema_tier_c.yaml
//   hermes_budget --json                             # machine-readable output
//
// Output includes:
//   - Current VRAM used (MB) + headroom to high/critical thresholds
//   - Top-3 VRAM consumers (candidates for eviction)
//   - CPU budget remaining before elevated threshold
//   - Safe-to-launch verdict for a "small" (256 MB) and "large" (4 GB) workload

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <dirent.h>
#  include <sys/stat.h>
#endif

namespace {

bool has_arg(const std::vector<std::string>& a, const std::string& f) {
    for (const auto& x : a) if (x == f) return true;
    return false;
}
std::string get_arg(const std::vector<std::string>& a, const std::string& f,
                    const std::string& def) {
    for (std::size_t i = 0; i + 1 < a.size(); ++i) if (a[i] == f) return a[i + 1];
    return def;
}

double jf(const std::string& j, const std::string& k) {
    const std::string kk = "\"" + k + "\":";
    const auto p = j.find(kk);
    if (p == std::string::npos) return 0.0;
    const auto s = p + kk.size();
    if (s < j.size() && j[s] == '"') return 0.0;
    try { return std::stod(j.substr(s)); } catch (...) { return 0.0; }
}
std::string js(const std::string& j, const std::string& k) {
    const std::string kk = "\"" + k + "\":\"";
    const auto p = j.find(kk);
    if (p == std::string::npos) return "";
    const auto s = p + kk.size(), e = j.find('"', s);
    return e == std::string::npos ? "" : j.substr(s, e - s);
}
bool jb(const std::string& j, const std::string& k) {
    const std::string kk = "\"" + k + "\":";
    const auto p = j.find(kk);
    if (p == std::string::npos) return false;
    const auto s = j.find_first_not_of(" \t", p + kk.size());
    return s != std::string::npos && j.substr(s, 4) == "true";
}

std::string find_latest_run(const std::string& artifact_root) {
    const std::string logs = artifact_root + "/logs";
    std::string best;
    uint64_t best_time = 0;
#ifdef _WIN32
    WIN32_FIND_DATAA ffd;
    HANDLE h = FindFirstFileA((logs + "\\*").c_str(), &ffd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && ffd.cFileName[0] != '.') {
                ULARGE_INTEGER t; t.LowPart = ffd.ftLastWriteTime.dwLowDateTime;
                t.HighPart = ffd.ftLastWriteTime.dwHighDateTime;
                if (t.QuadPart > best_time) { best_time = t.QuadPart; best = logs + "/" + ffd.cFileName; }
            }
        } while (FindNextFileA(h, &ffd));
        FindClose(h);
    }
#else
    DIR* dir = opendir(logs.c_str());
    if (dir) {
        struct dirent* ent;
        while ((ent = readdir(dir)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            const std::string full = logs + "/" + ent->d_name;
            struct stat st{};
            if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                const uint64_t m = static_cast<uint64_t>(st.st_mtime);
                if (m > best_time) { best_time = m; best = full; }
            }
        }
        closedir(dir);
    }
#endif
    return best;
}

// Parse vram thresholds from schema YAML.
void read_thresholds(const std::string& cfg, double& vram_high_pct, double& vram_crit_pct,
                     double& ups_elevated, double& ups_critical) {
    std::ifstream f(cfg);
    std::string line;
    bool in_vram = false, in_ups = false;
    while (std::getline(f, line)) {
        if (line.find("vram:") != std::string::npos) { in_vram = true; in_ups = false; continue; }
        if (line.find("ups:") != std::string::npos)  { in_ups = true; in_vram = false; continue; }
        if (in_vram && line.find("high_pct:") != std::string::npos) {
            const auto p = line.find(':');
            try { vram_high_pct = std::stod(line.substr(p + 1)); } catch (...) {}
        }
        if (in_vram && line.find("critical_pct:") != std::string::npos) {
            const auto p = line.find(':');
            try { vram_crit_pct = std::stod(line.substr(p + 1)); } catch (...) {}
        }
        if (in_ups && line.find("elevated:") != std::string::npos) {
            const auto p = line.find(':');
            try { ups_elevated = std::stod(line.substr(p + 1)); } catch (...) {}
        }
        if (in_ups && line.find("critical:") != std::string::npos) {
            const auto p = line.find(':');
            try { ups_critical = std::stod(line.substr(p + 1)); } catch (...) {}
        }
    }
}

struct ProcEntry {
    int pid{0};
    std::string name;
    double gpu_mb{0.0};
    double cpu_pct{0.0};
    bool foreground{false};
    bool protected_proc{false};
};

} // namespace

int main(int argc, char* argv[]) {
    const std::vector<std::string> args(argv + 1, argv + argc);

    if (has_arg(args, "--help")) {
        std::cout << "Usage: hermes_budget [run-dir] [--vram-total-mb N] [--config path] [--json]\n";
        return 0;
    }

    const bool json_out = has_arg(args, "--json");
    const std::string artifact_root = "artifacts";

    std::string run_dir;
    for (const auto& a : args) {
        if (!a.empty() && a[0] != '-') { run_dir = a; break; }
    }
    if (run_dir.empty()) run_dir = find_latest_run(artifact_root);
    if (run_dir.empty()) {
        std::cerr << "hermes_budget: no run directory found. Run hermesd first.\n"; return 1;
    }

    const char* cfg_env = std::getenv("HERMES_CONFIG_PATH");
    const std::string cfg = get_arg(args, "--config",
        (cfg_env && cfg_env[0]) ? cfg_env : "config/schema.yaml");

    double vram_high_pct = 90.0, vram_crit_pct = 95.0;
    double ups_elevated = 40.0, ups_critical = 70.0;
    read_thresholds(cfg, vram_high_pct, vram_crit_pct, ups_elevated, ups_critical);

    // Read VRAM total from processes.ndjson or override.
    double vram_total_mb = [&]() {
        const std::string s = get_arg(args, "--vram-total-mb", "");
        if (!s.empty()) try { return std::stod(s); } catch (...) {}
        return 0.0;
    }();

    // Read processes.ndjson — last-seen per PID wins.
    std::vector<ProcEntry> procs;
    double total_gpu_mb = 0.0;
    double total_cpu_pct = 0.0;
    {
        std::ifstream f(run_dir + "/processes.ndjson");
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            const int pid = static_cast<int>(jf(line, "pid"));
            if (pid <= 0) continue;
            if (vram_total_mb <= 0.0) {
                const double vt = jf(line, "vram_total_mb");
                if (vt > vram_total_mb) vram_total_mb = vt;
            }
            auto it = std::find_if(procs.begin(), procs.end(),
                [pid](const ProcEntry& e) { return e.pid == pid; });
            if (it == procs.end()) { procs.push_back({}); it = procs.end() - 1; }
            it->pid = pid;
            it->name = js(line, "name");
            it->gpu_mb = jf(line, "gpu_mb");
            it->cpu_pct = jf(line, "cpu_pct");
            it->foreground = jb(line, "foreground");
            it->protected_proc = jb(line, "protected_process");
        }
    }

    for (const auto& p : procs) { total_gpu_mb += p.gpu_mb; total_cpu_pct += p.cpu_pct; }
    if (vram_total_mb <= 0.0) vram_total_mb = 8192.0;  // 8 GB fallback

    const double vram_used_pct = (total_gpu_mb / vram_total_mb) * 100.0;
    const double vram_high_mb  = (vram_high_pct  / 100.0) * vram_total_mb;
    const double vram_crit_mb  = (vram_crit_pct  / 100.0) * vram_total_mb;
    const double vram_headroom_high_mb = vram_high_mb  - total_gpu_mb;
    const double vram_headroom_crit_mb = vram_crit_mb  - total_gpu_mb;

    // Sort processes by VRAM descending for eviction candidates.
    std::sort(procs.begin(), procs.end(),
        [](const ProcEntry& a, const ProcEntry& b) { return a.gpu_mb > b.gpu_mb; });

    // Verdict for small (256 MB) and large (4 GB) workloads.
    auto verdict = [](double headroom_mb, double workload_mb) -> std::string {
        if (headroom_mb < 0)          return "DENY (already over threshold)";
        if (headroom_mb < workload_mb) return "DENY (insufficient headroom)";
        if (headroom_mb < workload_mb * 1.5) return "CAUTION (tight)";
        return "OK";
    };

    if (json_out) {
        std::cout << std::fixed << std::setprecision(1);
        std::cout << "{\n"
            << "  \"run_dir\": \"" << run_dir << "\",\n"
            << "  \"vram_total_mb\": " << vram_total_mb << ",\n"
            << "  \"vram_used_mb\": "  << total_gpu_mb  << ",\n"
            << "  \"vram_used_pct\": " << vram_used_pct << ",\n"
            << "  \"vram_headroom_to_high_mb\": " << vram_headroom_high_mb << ",\n"
            << "  \"vram_headroom_to_critical_mb\": " << vram_headroom_crit_mb << ",\n"
            << "  \"total_cpu_pct\": " << total_cpu_pct << ",\n"
            << "  \"ups_elevated_threshold\": " << ups_elevated << ",\n"
            << "  \"ups_critical_threshold\": " << ups_critical << "\n"
            << "}\n";
        return 0;
    }

    const std::string sep(66, '=');
    std::cout << sep << "\n";
    std::cout << "hermes_budget — workload capacity report\n";
    std::cout << "  Run dir : " << run_dir << "\n";
    std::cout << "  Config  : " << cfg    << "\n";
    std::cout << sep << "\n\n";

    std::cout << std::fixed << std::setprecision(0);
    std::cout << "VRAM Budget\n";
    std::cout << "  Total              : " << vram_total_mb << " MB\n";
    std::cout << "  Used               : " << total_gpu_mb  << " MB (" << std::setprecision(1) << vram_used_pct << "%)\n";
    std::cout << std::setprecision(0);
    std::cout << "  High threshold     : " << vram_high_mb  << " MB (" << vram_high_pct << "%) — headroom: " << vram_headroom_high_mb << " MB\n";
    std::cout << "  Critical threshold : " << vram_crit_mb  << " MB (" << vram_crit_pct << "%) — headroom: " << vram_headroom_crit_mb << " MB\n";

    std::cout << "\nTop VRAM consumers (eviction candidates):\n";
    int shown = 0;
    for (const auto& p : procs) {
        if (p.gpu_mb <= 0.0) continue;
        const std::string tag = p.protected_proc ? " [PROT]" : p.foreground ? " [FG]" : "";
        std::cout << "  PID " << std::setw(6) << p.pid
                  << "  " << std::setw(20) << std::left << p.name
                  << std::right << std::setw(8) << static_cast<int>(p.gpu_mb) << " MB"
                  << tag << "\n";
        if (++shown >= 5) break;
    }

    std::cout << "\nLaunch feasibility:\n";
    std::cout << "  Small workload (256 MB VRAM)  : " << verdict(vram_headroom_high_mb, 256)  << "\n";
    std::cout << "  Large workload (4096 MB VRAM) : " << verdict(vram_headroom_high_mb, 4096) << "\n";
    std::cout << "\n  CPU total in use : " << std::setprecision(1) << total_cpu_pct << "%\n";
    std::cout << "  UPS thresholds   : elevated=" << ups_elevated << "  critical=" << ups_critical << "\n";
    std::cout << sep << "\n";
    std::cout << "  Tip: run hermesctl headroom for live UPS admission check.\n";
    return 0;
}
