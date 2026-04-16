// hermesctl: Live observe-only dashboard for Hermes.
//
// Connects to a running hermesd (or hermesd_mt) daemon via Unix domain socket
// and renders a refreshing terminal view of the current system state.
//
// Usage:
//   hermesctl [--socket /tmp/hermesd.sock] [--interval-ms 1000] [--once]
//   hermesctl ping
//   hermesctl status
//   hermesctl nvml          Check NVML availability and print device summary
//   hermesctl eval [dir]    Show offline predictor evaluation for a run directory
//   hermesctl bench         List recent benchmark run summaries from artifacts/bench/
//   hermesctl diff          Compare two eval_summary.json files side by side
//
// On non-Linux platforms, falls back to reading the most recent run directory
// from artifacts/logs/ and printing a static summary.

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <dirent.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <dlfcn.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    if (v == nullptr || v[0] == '\0') return fallback;
    return v;
}

bool has_arg(const std::vector<std::string>& args, const std::string& flag) {
    for (const auto& a : args) { if (a == flag) return true; }
    return false;
}

std::string get_arg(const std::vector<std::string>& args, const std::string& flag, const std::string& def) {
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == flag) return args[i + 1];
    }
    return def;
}

// ---- Extract a string field from flat JSON ----
std::string jstr(const std::string& json, const std::string& key) {
    const std::string search = "\"" + key + "\":\"";
    const auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    const auto start = pos + search.size();
    const auto end = json.find('"', start);
    if (end == std::string::npos) return "";
    return json.substr(start, end - start);
}

double jdbl(const std::string& json, const std::string& key) {
    const std::string search = "\"" + key + "\":";
    const auto pos = json.find(search);
    if (pos == std::string::npos) return 0.0;
    const auto start = pos + search.size();
    try { return std::stod(json.substr(start)); } catch (...) { return 0.0; }
}

uint64_t jull(const std::string& json, const std::string& key) {
    const std::string search = "\"" + key + "\":";
    const auto pos = json.find(search);
    if (pos == std::string::npos) return 0;
    const auto start = pos + search.size();
    try { return std::stoull(json.substr(start)); } catch (...) { return 0; }
}

// ---- Socket request/response ----

std::string socket_request(const std::string& socket_path, const std::string& request) {
#ifdef __linux__
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return "";

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return "";
    }

    const std::string req = request + "\n";
    ::send(fd, req.c_str(), req.size(), 0);

    char buf[4096] = {};
    const ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
    ::close(fd);

    if (n > 0) return std::string(buf, static_cast<std::size_t>(n));
    return "";
#else
    (void)socket_path; (void)request;
    return "";
#endif
}

// ---- Band coloring ----

std::string band_indicator(const std::string& band) {
    if (band == "critical") return "[CRIT]";
    if (band == "elevated") return "[ELEV]";
    return "[NORM]";
}

// ---- Dashboard render ----

void render_status(const std::string& json) {
    const double ups = jdbl(json, "ups");
    const std::string pband = jstr(json, "pressure_band");
    const double risk = jdbl(json, "risk_score");
    const std::string rband = jstr(json, "risk_band");
    const std::string state = jstr(json, "scheduler_state");
    const std::string action = jstr(json, "last_action");
    const std::string run_id = jstr(json, "run_id");
    const uint64_t samples = jull(json, "sample_count");
    const uint64_t drops = jull(json, "drop_count");

    // Clear screen with ANSI codes
    std::cout << "\033[2J\033[H";
    std::cout << "=== Hermes Live Dashboard ==============================\n";
    std::cout << "Run ID      : " << run_id << "\n";
    std::cout << "UPS         : " << ups << "  " << band_indicator(pband) << " " << pband << "\n";
    std::cout << "Risk        : " << risk << "  [" << rband << "]\n";
    std::cout << "Scheduler   : " << state << "\n";
    std::cout << "Last action : " << action << "\n";
    std::cout << "Samples     : " << samples << "  Drops: " << drops;
    if (samples > 0) {
        const double drop_pct = 100.0 * static_cast<double>(drops) / static_cast<double>(samples + drops);
        std::cout << " (" << drop_pct << "%)";
    }
    std::cout << "\n";
    std::cout << "========================================================\n";
    std::cout << "(Press Ctrl-C to exit)\n";
    std::cout.flush();
}

// ---- Offline fallback: read latest replay_summary.json ----

void offline_summary(const std::string& artifact_root) {
    // Find the most recently modified replay summary
    const std::string replay_dir = artifact_root + "/replay";
    std::cout << "[hermesctl] No running daemon found.\n";
    std::cout << "[hermesctl] Looking for replay summaries in: " << replay_dir << "\n";
    std::cout << "[hermesctl] Run hermesd to get a live feed, or use hermes_replay for artifact inspection.\n";
}

// ---- nvml subcommand: probe NVML availability and print device info ----

int cmd_nvml() {
    // Minimal NVML types (stable ABI, no nvml.h needed).
    using nvmlReturn_t = int;
    using nvmlDevice_t = void*;
    static constexpr nvmlReturn_t NVML_SUCCESS = 0;
    static constexpr unsigned int NVML_DEVICE_NAME_BUF = 96;

    struct NvmlMem { unsigned long long total, free, used; };
    struct NvmlUtil { unsigned int gpu, memory; };

    using PfnInit   = nvmlReturn_t (*)();
    using PfnShut   = nvmlReturn_t (*)();
    using PfnCount  = nvmlReturn_t (*)(unsigned int*);
    using PfnHandle = nvmlReturn_t (*)(unsigned int, nvmlDevice_t*);
    using PfnName   = nvmlReturn_t (*)(nvmlDevice_t, char*, unsigned int);
    using PfnMem    = nvmlReturn_t (*)(nvmlDevice_t, NvmlMem*);
    using PfnUtil   = nvmlReturn_t (*)(nvmlDevice_t, NvmlUtil*);
    using PfnTemp   = nvmlReturn_t (*)(nvmlDevice_t, unsigned int, unsigned int*);

    // Load library.
    void* lib = nullptr;
#if defined(__linux__)
    for (const char* name : {"libnvidia-ml.so.1", "libnvidia-ml.so"}) {
        lib = dlopen(name, RTLD_LAZY | RTLD_LOCAL);
        if (lib) break;
    }
    auto sym = [&](const char* n) -> void* { return lib ? dlsym(lib, n) : nullptr; };
#elif defined(_WIN32)
    lib = static_cast<void*>(LoadLibraryA("nvml.dll"));
    auto sym = [&](const char* n) -> void* {
        return lib ? reinterpret_cast<void*>(
            GetProcAddress(static_cast<HMODULE>(lib), n)) : nullptr;
    };
#else
    auto sym = [&](const char*) -> void* { return nullptr; };
#endif

    std::cout << "=== NVML Status ===\n";

    if (!lib) {
        std::cout << "NVML library  : NOT FOUND\n";
        std::cout << "  (Hermes will use nvidia-smi subprocess fallback)\n";
#if defined(__linux__)
        std::cout << "  Searched     : libnvidia-ml.so.1, libnvidia-ml.so\n";
        std::cout << "  WSL2 path    : /usr/lib/wsl/lib/libnvidia-ml.so.1\n";
#elif defined(_WIN32)
        std::cout << "  Searched     : nvml.dll\n";
#endif
        // Check nvidia-smi as a fallback indicator.
        const int rc = std::system("nvidia-smi --query-gpu=name --format=csv,noheader > /dev/null 2>&1");
        std::cout << "nvidia-smi    : " << (rc == 0 ? "available" : "not found") << "\n";
        return 1;
    }

    std::cout << "NVML library  : loaded\n";

    // Resolve and call init.
    auto fn_init  = reinterpret_cast<PfnInit>(sym("nvmlInit_v2"));
    if (!fn_init) fn_init = reinterpret_cast<PfnInit>(sym("nvmlInit"));
    auto fn_shut  = reinterpret_cast<PfnShut>(sym("nvmlShutdown"));
    auto fn_count = reinterpret_cast<PfnCount>(sym("nvmlDeviceGetCount_v2"));
    if (!fn_count) fn_count = reinterpret_cast<PfnCount>(sym("nvmlDeviceGetCount"));
    auto fn_hdl   = reinterpret_cast<PfnHandle>(sym("nvmlDeviceGetHandleByIndex_v2"));
    if (!fn_hdl) fn_hdl = reinterpret_cast<PfnHandle>(sym("nvmlDeviceGetHandleByIndex"));
    auto fn_name  = reinterpret_cast<PfnName>(sym("nvmlDeviceGetName"));
    auto fn_mem   = reinterpret_cast<PfnMem>(sym("nvmlDeviceGetMemoryInfo"));
    auto fn_util  = reinterpret_cast<PfnUtil>(sym("nvmlDeviceGetUtilizationRates"));
    auto fn_temp  = reinterpret_cast<PfnTemp>(sym("nvmlDeviceGetTemperature"));

    if (!fn_init) {
        std::cout << "NVML init     : symbol not found\n";
#ifdef __linux__
        if (lib) dlclose(lib);
#elif defined(_WIN32)
        if (lib) FreeLibrary(static_cast<HMODULE>(lib));
#endif
        return 1;
    }

    const nvmlReturn_t init_rc = fn_init();
    if (init_rc != NVML_SUCCESS) {
        std::cout << "NVML init     : FAILED (rc=" << init_rc << ")\n";
#ifdef __linux__
        dlclose(lib);
#elif defined(_WIN32)
        FreeLibrary(static_cast<HMODULE>(lib));
#endif
        return 1;
    }
    std::cout << "NVML init     : OK\n";

    unsigned int count = 0;
    if (fn_count && fn_count(&count) == NVML_SUCCESS) {
        std::cout << "Device count  : " << count << "\n";
    }

    for (unsigned int i = 0; i < count; ++i) {
        nvmlDevice_t handle = nullptr;
        if (!fn_hdl || fn_hdl(i, &handle) != NVML_SUCCESS || !handle) continue;

        std::cout << "\nDevice " << i << ":\n";

        if (fn_name) {
            char name[NVML_DEVICE_NAME_BUF] = {};
            if (fn_name(handle, name, NVML_DEVICE_NAME_BUF) == NVML_SUCCESS)
                std::cout << "  Name         : " << name << "\n";
        }
        if (fn_mem) {
            NvmlMem mem{};
            if (fn_mem(handle, &mem) == NVML_SUCCESS) {
                std::cout << "  VRAM total   : " << (mem.total / 1048576) << " MB\n";
                std::cout << "  VRAM used    : " << (mem.used  / 1048576) << " MB\n";
                std::cout << "  VRAM free    : " << (mem.free  / 1048576) << " MB\n";
            }
        }
        if (fn_util) {
            NvmlUtil util{};
            if (fn_util(handle, &util) == NVML_SUCCESS) {
                std::cout << "  GPU util     : " << util.gpu    << "%\n";
                std::cout << "  Mem util     : " << util.memory << "%\n";
            }
        }
        if (fn_temp) {
            unsigned int temp = 0;
            if (fn_temp(handle, 0, &temp) == NVML_SUCCESS)
                std::cout << "  Temperature  : " << temp << " C\n";
        }
    }

    if (fn_shut) fn_shut();
#if defined(__linux__)
    dlclose(lib);
#elif defined(_WIN32)
    FreeLibrary(static_cast<HMODULE>(lib));
#endif
    std::cout << "\nHermes fast path: NVML (no nvidia-smi subprocess needed)\n";
    return 0;
}

// ---- eval subcommand: show offline predictor evaluation for a run ----

int cmd_eval(const std::vector<std::string>& args, const std::string& artifact_root) {
    // Resolve run directory: explicit path argument, or most recent in artifacts/logs/.
    std::string run_dir_str;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (!args[i].empty() && args[i][0] != '-') {
            run_dir_str = args[i];
            break;
        }
    }

    if (run_dir_str.empty()) {
        // Find most recent directory in artifact_root/logs/.
        const std::string logs_dir = artifact_root + "/logs";
        std::string best;
        uint64_t best_time = 0;

        // Simple scan using C stdio — no filesystem header to avoid extra deps.
#if defined(_WIN32)
        WIN32_FIND_DATAA ffd;
        const std::string pattern = logs_dir + "\\*";
        HANDLE hFind = FindFirstFileA(pattern.c_str(), &ffd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                    ffd.cFileName[0] != '.') {
                    ULARGE_INTEGER t;
                    t.LowPart  = ffd.ftLastWriteTime.dwLowDateTime;
                    t.HighPart = ffd.ftLastWriteTime.dwHighDateTime;
                    if (t.QuadPart > best_time) {
                        best_time = t.QuadPart;
                        best = logs_dir + "/" + ffd.cFileName;
                    }
                }
            } while (FindNextFileA(hFind, &ffd));
            FindClose(hFind);
        }
#else
        // POSIX: use opendir
        DIR* dir = opendir(logs_dir.c_str());
        if (dir) {
            struct dirent* ent;
            while ((ent = readdir(dir)) != nullptr) {
                if (ent->d_name[0] == '.') continue;
                const std::string full = logs_dir + "/" + ent->d_name;
                struct stat st{};
                if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                    const uint64_t mtime = static_cast<uint64_t>(st.st_mtime);
                    if (mtime > best_time) { best_time = mtime; best = full; }
                }
            }
            closedir(dir);
        }
#endif
        if (best.empty()) {
            std::cerr << "hermesctl eval: no run directories found in " << logs_dir << "\n";
            std::cerr << "  Usage: hermesctl eval [run-dir]\n";
            return 1;
        }
        run_dir_str = best;
    }

    // Check for eval_summary.json (written by hermes_eval).
    const std::string eval_json_path = run_dir_str + "/eval_summary.json";
    {
        std::ifstream f(eval_json_path);
        if (f.is_open()) {
            const std::string json((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
            const bool data_avail = (json.find("\"data_available\":true") != std::string::npos);
            const double auc_roc  = jdbl(json, "auc_roc");
            const double accuracy = jdbl(json, "accuracy");
            const uint64_t total  = jull(json, "total_predictions");
            std::cout << "=== hermesctl eval: " << run_dir_str << " ===\n";
            if (!data_avail) {
                std::cout << "  data_available : false (no predictions recorded)\n";
                return 0;
            }
            std::cout << "  total_predictions : " << total << "\n";
            std::cout << "  accuracy          : " << accuracy << "\n";
            if (auc_roc > 0.0) std::cout << "  auc_roc         : " << auc_roc << "\n";
            return 0;
        }
    }

    // No eval_summary.json — derive a compact summary from predictions.ndjson directly.
    const std::string preds_path = run_dir_str + "/predictions.ndjson";
    std::ifstream f(preds_path);
    if (!f.is_open()) {
        std::cerr << "hermesctl eval: no predictions.ndjson in " << run_dir_str << "\n";
        std::cerr << "  Run hermes_eval to produce eval_summary.json first.\n";
        return 1;
    }

    unsigned int total = 0;
    unsigned int high  = 0;
    unsigned int crit  = 0;
    double peak_risk   = 0.0;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        ++total;
        const double r = jdbl(line, "risk_score");
        if (r > peak_risk) peak_risk = r;
        const std::string band = jstr(line, "risk_band");
        if (band == "high")     ++high;
        if (band == "critical") ++crit;
    }

    std::cout << "=== hermesctl eval: " << run_dir_str << " ===\n";
    std::cout << "  (eval_summary.json not found — summarising predictions.ndjson)\n";
    std::cout << "  total_predictions : " << total << "\n";
    std::cout << "  peak_risk_score   : " << peak_risk << "\n";
    std::cout << "  high_risk_frames  : " << high << "\n";
    std::cout << "  critical_frames   : " << crit << "\n";
    std::cout << "  Tip: run hermes_eval <run-dir> to compute accuracy metrics.\n";
    return 0;
}

// ---- bench subcommand: list recent benchmark run summaries ----

int cmd_bench(const std::string& artifact_root) {
    const std::string bench_dir = artifact_root + "/bench";

    struct SummaryEntry {
        std::string run_id;
        std::string scenario;
        std::string mode;
        double p95_ms{-1.0};
        double completion{-1.0};
        int oom_count{-1};
        int interventions{-1};
        std::string lat_target_met; // "true"/"false"/"—"
        std::string filename;
    };

    std::vector<SummaryEntry> entries;

    auto jdbl = [](const std::string& s, const std::string& k) -> double {
        const std::string search = "\"" + k + "\":";
        const auto p = s.find(search);
        if (p == std::string::npos) return -1.0;
        const auto start = s.find_first_not_of(" \t", p + search.size());
        if (start == std::string::npos) return -1.0;
        if (s[start] == 'n') return -1.0;
        try { return std::stod(s.substr(start)); } catch (...) { return -1.0; }
    };
    auto jstr = [](const std::string& s, const std::string& k) -> std::string {
        const std::string search = "\"" + k + "\":\"";
        const auto p = s.find(search);
        if (p == std::string::npos) return "";
        const auto start = p + search.size();
        const auto end = s.find('"', start);
        return end == std::string::npos ? "" : s.substr(start, end - start);
    };
    auto jint = [&jdbl](const std::string& s, const std::string& k) -> int {
        const double v = jdbl(s, k);
        return v < 0.0 ? -1 : static_cast<int>(v);
    };

#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    const std::string pattern = bench_dir + "\\*-summary.json";
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::string fname(fd.cFileName);
            if (fname.size() > 13) {
                std::ifstream f(bench_dir + "/" + fname);
                if (f.is_open()) {
                    std::string content((std::istreambuf_iterator<char>(f)),
                                        std::istreambuf_iterator<char>());
                    SummaryEntry e;
                    e.filename = fname;
                    e.run_id   = jstr(content, "run_id");
                    e.scenario = jstr(content, "scenario_name");
                    e.mode     = jstr(content, "runtime_mode");
                    e.p95_ms   = jdbl(content, "p95_latency_ms");
                    e.completion  = jdbl(content, "completion_rate");
                    e.oom_count   = jint(content, "oom_count");
                    e.interventions = jint(content, "intervention_count");
                    const auto lp = content.find("\"latency_target_met\":");
                    if (lp != std::string::npos) {
                        const auto vs = content.find_first_not_of(" \t", lp + 21);
                        if (vs != std::string::npos) {
                            if (content.substr(vs, 4) == "true")  e.lat_target_met = "PASS";
                            else if (content.substr(vs, 5) == "false") e.lat_target_met = "FAIL";
                            else e.lat_target_met = "—";
                        }
                    } else { e.lat_target_met = "—"; }
                    if (e.run_id.empty()) e.run_id = fname.substr(0, fname.size() - 13);
                    entries.push_back(std::move(e));
                }
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR* dir = opendir(bench_dir.c_str());
    if (dir) {
        struct dirent* ent;
        while ((ent = readdir(dir)) != nullptr) {
            const std::string fname(ent->d_name);
            if (fname.size() <= 13 || fname.substr(fname.size() - 13) != "-summary.json")
                continue;
            std::ifstream f(bench_dir + "/" + fname);
            if (!f.is_open()) continue;
            std::string content((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
            SummaryEntry e;
            e.filename = fname;
            e.run_id   = jstr(content, "run_id");
            e.scenario = jstr(content, "scenario_name");
            e.mode     = jstr(content, "runtime_mode");
            e.p95_ms   = jdbl(content, "p95_latency_ms");
            e.completion  = jdbl(content, "completion_rate");
            e.oom_count   = jint(content, "oom_count");
            e.interventions = jint(content, "intervention_count");
            const auto lp = content.find("\"latency_target_met\":");
            if (lp != std::string::npos) {
                const auto vs = content.find_first_not_of(" \t", lp + 21);
                if (vs != std::string::npos) {
                    if (content.substr(vs, 4) == "true")  e.lat_target_met = "PASS";
                    else if (content.substr(vs, 5) == "false") e.lat_target_met = "FAIL";
                    else e.lat_target_met = "—";
                }
            } else { e.lat_target_met = "—"; }
            if (e.run_id.empty()) e.run_id = fname.substr(0, fname.size() - 13);
            entries.push_back(std::move(e));
        }
        closedir(dir);
    }
#endif

    if (entries.empty()) {
        std::cout << "hermesctl bench: no *-summary.json found in " << bench_dir << "\n";
        std::cout << "  Run: hermes_bench <scenario.yaml> --run-id <id>\n";
        return 0;
    }

    std::sort(entries.begin(), entries.end(),
        [](const SummaryEntry& a, const SummaryEntry& b) {
            return a.filename < b.filename;
        });

    const std::string sep(88, '=');
    std::cout << sep << "\n";
    std::cout << "hermesctl bench — " << entries.size() << " run(s) in " << bench_dir << "\n";
    std::cout << sep << "\n";
    std::cout << std::left
              << std::setw(36) << "Run ID"
              << std::setw(14) << "Mode"
              << std::right
              << std::setw(10) << "p95 (ms)"
              << std::setw(8)  << "Done%"
              << std::setw(6)  << "OOM"
              << std::setw(8)  << "Actions"
              << std::setw(6)  << "Lat\n";
    std::cout << std::string(88, '-') << "\n";

    std::cout << std::fixed << std::setprecision(1);
    for (const auto& e : entries) {
        const std::string p95_str  = e.p95_ms >= 0.0   ? std::to_string(static_cast<int>(e.p95_ms)) : "—";
        const std::string done_str = e.completion >= 0.0
            ? std::to_string(static_cast<int>(e.completion * 100)) + "%" : "—";
        const std::string oom_str  = e.oom_count >= 0   ? std::to_string(e.oom_count) : "—";
        const std::string int_str  = e.interventions >= 0 ? std::to_string(e.interventions) : "—";
        const std::string rid = e.run_id.size() > 35 ? e.run_id.substr(0, 32) + "..." : e.run_id;
        const std::string mode = e.mode.empty() ? "—" : e.mode;

        std::cout << std::left
                  << std::setw(36) << rid
                  << std::setw(14) << mode
                  << std::right
                  << std::setw(10) << p95_str
                  << std::setw(8)  << done_str
                  << std::setw(6)  << oom_str
                  << std::setw(8)  << int_str
                  << std::setw(6)  << e.lat_target_met
                  << "\n";
    }
    std::cout << sep << "\n";
    return 0;
}

// ---- diff subcommand: side-by-side eval_summary comparison ----

int cmd_diff(const std::vector<std::string>& args, const std::string& artifact_root) {
    // Usage: hermesctl diff <path-or-run-A> <path-or-run-B>
    // Paths can be: explicit eval_summary.json path, or a run directory containing one,
    // or a short run-id suffix resolved under artifacts/logs/.

    auto resolve_eval = [&](const std::string& arg) -> std::string {
        // 1. Direct file path
        {
            std::ifstream f(arg);
            if (f.is_open()) return arg;
        }
        // 2. Directory containing eval_summary.json
        {
            const std::string p = arg + "/eval_summary.json";
            std::ifstream f(p);
            if (f.is_open()) return p;
        }
        // 3. Run ID under artifacts/logs/
        {
            const std::string p = artifact_root + "/logs/" + arg + "/eval_summary.json";
            std::ifstream f(p);
            if (f.is_open()) return p;
        }
        return "";
    };

    // Collect the two positional args after "diff"
    std::vector<std::string> positionals;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (!args[i].empty() && args[i][0] != '-')
            positionals.push_back(args[i]);
    }

    if (positionals.size() < 2) {
        std::cerr << "hermesctl diff: requires two paths/run-ids\n"
                  << "  Usage: hermesctl diff <eval-A> <eval-B>\n"
                  << "         hermesctl diff artifacts/logs/run1/eval_summary.json \\\n"
                  << "                       artifacts/logs/run2/eval_summary.json\n";
        return 1;
    }

    const std::string path_a = resolve_eval(positionals[0]);
    const std::string path_b = resolve_eval(positionals[1]);

    if (path_a.empty()) {
        std::cerr << "hermesctl diff: cannot find eval_summary.json for: " << positionals[0] << "\n";
        return 1;
    }
    if (path_b.empty()) {
        std::cerr << "hermesctl diff: cannot find eval_summary.json for: " << positionals[1] << "\n";
        return 1;
    }

    auto read_file = [](const std::string& p) -> std::string {
        std::ifstream f(p);
        return std::string((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
    };

    const std::string ca = read_file(path_a);
    const std::string cb = read_file(path_b);

    auto jdbl = [](const std::string& s, const std::string& k) -> double {
        const std::string search = "\"" + k + "\":";
        const auto p = s.find(search);
        if (p == std::string::npos) return -1.0;
        const auto start = s.find_first_not_of(" \t", p + search.size());
        if (start == std::string::npos) return -1.0;
        if (s[start] == 'n') return -1.0;
        try { return std::stod(s.substr(start)); } catch (...) { return -1.0; }
    };
    auto jbool = [](const std::string& s, const std::string& k) -> std::string {
        const std::string search = "\"" + k + "\":";
        const auto p = s.find(search);
        if (p == std::string::npos) return "—";
        const auto start = s.find_first_not_of(" \t", p + search.size());
        if (start == std::string::npos) return "—";
        if (s.substr(start, 4) == "true")  return "true";
        if (s.substr(start, 5) == "false") return "false";
        return "—";
    };

    struct Row {
        std::string metric;
        double va;
        double vb;
        bool lower_is_better;
        std::string target_note;
    };

    const std::vector<Row> rows = {
        {"precision",                   jdbl(ca,"precision"),                   jdbl(cb,"precision"),                   false, ">= 0.85"},
        {"recall",                      jdbl(ca,"recall"),                      jdbl(cb,"recall"),                      false, ">= 0.80"},
        {"f1",                          jdbl(ca,"f1"),                          jdbl(cb,"f1"),                          false, ">= 0.80"},
        {"mean_lead_time_s",            jdbl(ca,"mean_lead_time_s"),            jdbl(cb,"mean_lead_time_s"),            false, ">= 3.0 s"},
        {"false_positive_rate_per_hour",jdbl(ca,"false_positive_rate_per_hour"),jdbl(cb,"false_positive_rate_per_hour"),true,  "< 5/hr"},
        {"true_positives",              jdbl(ca,"true_positives"),              jdbl(cb,"true_positives"),              false, ""},
        {"false_positives",             jdbl(ca,"false_positives"),             jdbl(cb,"false_positives"),             true,  ""},
        {"false_negatives",             jdbl(ca,"false_negatives"),             jdbl(cb,"false_negatives"),             true,  ""},
        {"total_predictions",           jdbl(ca,"total_predictions"),           jdbl(cb,"total_predictions"),           false, ""},
        {"observation_window_s",        jdbl(ca,"observation_window_s"),        jdbl(cb,"observation_window_s"),        false, ""},
    };

    const std::string label_a = positionals[0].size() > 28
        ? "..." + positionals[0].substr(positionals[0].size() - 25) : positionals[0];
    const std::string label_b = positionals[1].size() > 28
        ? "..." + positionals[1].substr(positionals[1].size() - 25) : positionals[1];

    const std::string data_a = jbool(ca, "data_available");
    const std::string data_b = jbool(cb, "data_available");

    const std::string sep(80, '=');
    std::cout << sep << "\n";
    std::cout << "hermesctl diff — eval_summary comparison\n";
    std::cout << sep << "\n";
    std::cout << "  A: " << path_a << "  (data_available=" << data_a << ")\n";
    std::cout << "  B: " << path_b << "  (data_available=" << data_b << ")\n\n";

    std::cout << std::left  << std::setw(32) << "Metric"
              << std::right << std::setw(12) << label_a
              << std::setw(12) << label_b
              << std::setw(10) << "Delta"
              << std::setw(8)  << "Better"
              << std::setw(16) << "Target\n";
    std::cout << std::string(80, '-') << "\n";

    std::cout << std::fixed << std::setprecision(3);
    for (const auto& row : rows) {
        const bool has_a = row.va >= 0.0;
        const bool has_b = row.vb >= 0.0;
        const std::string sa = has_a ? [&]{ std::ostringstream os; os << std::fixed << std::setprecision(3) << row.va; return os.str(); }() : "—";
        const std::string sb = has_b ? [&]{ std::ostringstream os; os << std::fixed << std::setprecision(3) << row.vb; return os.str(); }() : "—";

        std::string delta_str = "—";
        std::string better    = "—";
        if (has_a && has_b) {
            const double delta = row.vb - row.va;
            std::ostringstream ds;
            ds << std::fixed << std::setprecision(3) << (delta >= 0 ? "+" : "") << delta;
            delta_str = ds.str();
            if (delta == 0.0)         better = "=";
            else if (row.lower_is_better) better = delta < 0.0 ? "B" : "A";
            else                          better = delta > 0.0 ? "B" : "A";
        }

        std::cout << std::left  << std::setw(32) << row.metric
                  << std::right << std::setw(12) << sa
                  << std::setw(12) << sb
                  << std::setw(10) << delta_str
                  << std::setw(8)  << better
                  << std::setw(16) << row.target_note << "\n";
    }
    std::cout << sep << "\n";
    std::cout << "  Legend: Better = which file is better (A or B); = means equal.\n";
    std::cout << "          Targets from design.md Predictor Calibration Cycle.\n";
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    const std::vector<std::string> args(argv + 1, argv + argc);

    const std::string socket_path = get_arg(args, "--socket",
        env_or("HERMES_SOCKET_PATH", "/tmp/hermesd.sock"));
    const int interval_ms = [&]() {
        const std::string v = get_arg(args, "--interval-ms", "1000");
        try { return std::stoi(v); } catch (...) { return 1000; }
    }();
    const bool once = has_arg(args, "--once");
    const std::string artifact_root = env_or("HERMES_ARTIFACT_ROOT", "artifacts");

    // Sub-commands
    if (!args.empty() && args[0] == "nvml") {
        return cmd_nvml();
    }

    if (!args.empty() && args[0] == "eval") {
        return cmd_eval(args, artifact_root);
    }

    if (!args.empty() && args[0] == "bench") {
        return cmd_bench(artifact_root);
    }

    if (!args.empty() && args[0] == "diff") {
        return cmd_diff(args, artifact_root);
    }

    if (!args.empty() && args[0] == "ping") {
        const std::string response = socket_request(socket_path, "{\"kind\":\"ping\"}");
        if (response.empty()) {
            std::cerr << "hermesctl: daemon not reachable at " << socket_path << std::endl;
            return 1;
        }
        std::cout << "hermesctl: " << response;
        return 0;
    }

    if (!args.empty() && args[0] == "status") {
        const std::string response = socket_request(socket_path, "{\"kind\":\"status\"}");
        if (response.empty()) {
            std::cerr << "hermesctl: daemon not reachable at " << socket_path << std::endl;
            offline_summary(artifact_root);
            return 1;
        }
        render_status(response);
        return 0;
    }

    // Live refresh loop
    while (true) {
        const std::string response = socket_request(socket_path, "{\"kind\":\"status\"}");
        if (response.empty()) {
            offline_summary(artifact_root);
            if (once) return 1;
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
            continue;
        }

        render_status(response);
        if (once) return 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }

    return 0;
}
