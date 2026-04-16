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
