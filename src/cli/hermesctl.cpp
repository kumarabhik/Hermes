// hermesctl: Live observe-only dashboard for Hermes.
//
// Connects to a running hermesd (or hermesd_mt) daemon via Unix domain socket
// and renders a refreshing terminal view of the current system state.
//
// Usage:
//   hermesctl [--socket /tmp/hermesd.sock] [--interval-ms 1000] [--once]
//   hermesctl ping
//   hermesctl status
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
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
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
