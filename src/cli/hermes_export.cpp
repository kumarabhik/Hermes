// hermes_export: Prometheus metrics exporter for Hermes.
//
// Serves Prometheus text/plain metrics at GET /metrics on --port (default 9090).
// Metrics are sourced from the running hermesd control socket.
// When no daemon is running, falls back to the most recent telemetry_quality.json.
//
// Prometheus scrape config:
//   - job_name: hermes
//     static_configs:
//       - targets: ['localhost:9090']
//
// Exposed metrics:
//   hermes_ups                  Current Unified Pressure Score (0-100)
//   hermes_risk_score           Current risk score (0-1)
//   hermes_sample_count         Total samples collected
//   hermes_drop_count           Samples dropped (EventBus overflow)
//   hermes_scheduler_state      Scheduler state as label
//   hermes_last_action          Last executed action as label
//   hermes_level1_actions_total Level-1 reprioritize actions from telemetry_quality
//   hermes_level2_actions_total Level-2 throttle actions
//   hermes_level3_actions_total Level-3 kill actions
//   hermes_peak_ups             Peak UPS seen in current/last run
//   hermes_peak_risk            Peak risk score seen in current/last run
//   hermes_up                   1 if daemon is reachable, 0 otherwise
//
// Usage:
//   hermes_export [--port 9090] [--socket /tmp/hermesd.sock] [--once]
//
// --once: write metrics to stdout and exit (useful for cron/log ingestion).

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#  include <sys/socket.h>
#  include <sys/un.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <signal.h>
#elif defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
using SockType = SOCKET;
static constexpr SockType BAD_SOCK = INVALID_SOCKET;
static void close_sock(SockType s) { closesocket(s); }
#endif

#ifdef __linux__
using SockType = int;
static constexpr SockType BAD_SOCK = -1;
static void close_sock(SockType s) { ::close(s); }
#endif

namespace {

std::string env_or(const char* n, const std::string& def) {
    const char* v = std::getenv(n);
    return (v && v[0]) ? v : def;
}
bool has_arg(const std::vector<std::string>& a, const std::string& f) {
    for (const auto& x : a) if (x == f) return true;
    return false;
}
std::string get_arg(const std::vector<std::string>& a, const std::string& f,
                    const std::string& def) {
    for (std::size_t i = 0; i + 1 < a.size(); ++i) if (a[i] == f) return a[i + 1];
    return def;
}

std::string jstr(const std::string& j, const std::string& k) {
    const std::string kk = "\"" + k + "\":\"";
    const auto p = j.find(kk);
    if (p == std::string::npos) return "";
    const auto s = p + kk.size(), e = j.find('"', s);
    return e == std::string::npos ? "" : j.substr(s, e - s);
}
double jdbl(const std::string& j, const std::string& k) {
    const std::string kk = "\"" + k + "\":";
    const auto p = j.find(kk);
    if (p == std::string::npos) return 0.0;
    const auto s = p + kk.size();
    if (s < j.size() && j[s] == '"') return 0.0;
    try { return std::stod(j.substr(s)); } catch (...) { return 0.0; }
}

// ---- Fetch live status from daemon ----

std::string fetch_status(const std::string& sock_path) {
#ifdef __linux__
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return "";
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd); return "";
    }
    const char req[] = "{\"kind\":\"status\"}\n";
    ::send(fd, req, sizeof(req) - 1, 0);
    char buf[8192] = {};
    const ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
    ::close(fd);
    return n > 0 ? std::string(buf, static_cast<std::size_t>(n)) : "";
#else
    (void)sock_path; return "";
#endif
}

// ---- Read fallback telemetry_quality.json from most recent run ----

std::string find_latest_tq(const std::string& artifact_root) {
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
                if (t.QuadPart > best_time) {
                    best_time = t.QuadPart;
                    best = logs + "/" + ffd.cFileName + "/telemetry_quality.json";
                }
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
                if (m > best_time) { best_time = m; best = full + "/telemetry_quality.json"; }
            }
        }
        closedir(dir);
    }
#endif
    return best;
}

// ---- Build Prometheus text/plain metrics body ----

std::string build_metrics(const std::string& sock_path, const std::string& artifact_root) {
    const std::string live = fetch_status(sock_path);
    const bool daemon_up = !live.empty();

    std::ostringstream o;
    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    auto gauge = [&](const std::string& name, const std::string& help,
                     double value, const std::string& labels = "") {
        o << "# HELP " << name << " " << help << "\n";
        o << "# TYPE " << name << " gauge\n";
        o << name;
        if (!labels.empty()) o << "{" << labels << "}";
        o << " " << value << " " << ts << "\n";
    };
    auto counter = [&](const std::string& name, const std::string& help,
                       double value, const std::string& labels = "") {
        o << "# HELP " << name << " " << help << "\n";
        o << "# TYPE " << name << " counter\n";
        o << name;
        if (!labels.empty()) o << "{" << labels << "}";
        o << " " << value << " " << ts << "\n";
    };

    // hermes_up
    gauge("hermes_up", "1 if hermesd is reachable via control socket, 0 otherwise",
          daemon_up ? 1.0 : 0.0);

    if (daemon_up) {
        const double ups      = jdbl(live, "ups");
        const double risk     = jdbl(live, "risk_score");
        const double samples  = static_cast<double>(
            [&]() -> uint64_t {
                const std::string kk = "\"sample_count\":";
                const auto p = live.find(kk);
                if (p == std::string::npos) return 0ULL;
                try { return std::stoull(live.substr(p + kk.size())); } catch (...) { return 0ULL; }
            }());
        const double drops = static_cast<double>(
            [&]() -> uint64_t {
                const std::string kk = "\"drop_count\":";
                const auto p = live.find(kk);
                if (p == std::string::npos) return 0ULL;
                try { return std::stoull(live.substr(p + kk.size())); } catch (...) { return 0ULL; }
            }());
        const std::string state  = jstr(live, "scheduler_state");
        const std::string action = jstr(live, "last_action");
        const std::string run_id = jstr(live, "run_id");

        gauge("hermes_ups",
              "Unified Pressure Score (0-100); elevated >= 40, critical >= 70", ups,
              "run_id=\"" + run_id + "\"");
        gauge("hermes_risk_score",
              "OOM risk score from predictor (0-1)", risk,
              "run_id=\"" + run_id + "\"");
        gauge("hermes_sample_count",
              "Total pressure samples collected since daemon start", samples,
              "run_id=\"" + run_id + "\"");
        gauge("hermes_drop_count",
              "Samples dropped due to EventBus overflow", drops,
              "run_id=\"" + run_id + "\"");
        gauge("hermes_scheduler_state_info",
              "Current scheduler state (1=active state)", 1.0,
              "state=\"" + state + "\",run_id=\"" + run_id + "\"");
        gauge("hermes_last_action_info",
              "Last action dispatched by Hermes (1=current action)", 1.0,
              "action=\"" + action + "\",run_id=\"" + run_id + "\"");
    }

    // Fallback: telemetry_quality.json for historical counters.
    const std::string tq_path = find_latest_tq(artifact_root);
    if (!tq_path.empty()) {
        std::ifstream f(tq_path);
        if (f.is_open()) {
            const std::string tq((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
            const double peak_ups  = jdbl(tq, "peak_ups");
            const double peak_risk = jdbl(tq, "peak_risk");
            const double l1 = jdbl(tq, "level1_count");
            const double l2 = jdbl(tq, "level2_count");
            const double l3 = jdbl(tq, "level3_count");
            const std::string run_id = jstr(tq, "run_id");

            gauge("hermes_peak_ups",
                  "Peak UPS observed in the current/last run", peak_ups,
                  "run_id=\"" + run_id + "\"");
            gauge("hermes_peak_risk",
                  "Peak risk score observed in the current/last run", peak_risk,
                  "run_id=\"" + run_id + "\"");
            counter("hermes_level1_actions_total",
                    "Total Level-1 reprioritize actions in last run", l1,
                    "run_id=\"" + run_id + "\"");
            counter("hermes_level2_actions_total",
                    "Total Level-2 throttle actions in last run", l2,
                    "run_id=\"" + run_id + "\"");
            counter("hermes_level3_actions_total",
                    "Total Level-3 kill actions in last run", l3,
                    "run_id=\"" + run_id + "\"");
        }
    }

    return o.str();
}

// ---- Handle one HTTP client ----

void handle_client(SockType client, const std::string& sock_path,
                   const std::string& artifact_root) {
    char buf[1024] = {};
#ifdef __linux__
    const ssize_t n = ::recv(client, buf, sizeof(buf) - 1, 0);
#else
    const int n = ::recv(client, buf, static_cast<int>(sizeof(buf) - 1), 0);
#endif
    if (n <= 0) { close_sock(client); return; }

    const std::string req(buf, static_cast<std::size_t>(n));
    std::string response;

    if (req.find("GET /metrics") != std::string::npos ||
        req.find("GET / ") != std::string::npos ||
        req.find("GET /\r") != std::string::npos) {
        const std::string body = build_metrics(sock_path, artifact_root);
        response = "HTTP/1.0 200 OK\r\n"
                   "Content-Type: text/plain; version=0.0.4\r\n"
                   "Content-Length: " + std::to_string(body.size()) + "\r\n"
                   "Connection: close\r\n\r\n" + body;
    } else {
        const std::string b = "Not Found — use /metrics";
        response = "HTTP/1.0 404 Not Found\r\nContent-Length: " +
                   std::to_string(b.size()) + "\r\nConnection: close\r\n\r\n" + b;
    }

#ifdef __linux__
    ::send(client, response.c_str(), response.size(), 0);
#else
    ::send(client, response.c_str(), static_cast<int>(response.size()), 0);
#endif
    close_sock(client);
}

} // namespace

int main(int argc, char* argv[]) {
    const std::vector<std::string> args(argv + 1, argv + argc);

    const int port = [&]() {
        try { return std::stoi(get_arg(args, "--port", "9090")); } catch (...) { return 9090; }
    }();
    const std::string sock_path = get_arg(args, "--socket",
        env_or("HERMES_SOCKET_PATH", "/tmp/hermesd.sock"));
    const std::string artifact_root = env_or("HERMES_ARTIFACT_ROOT", "artifacts");
    const bool once = has_arg(args, "--once");

    if (has_arg(args, "--help")) {
        std::cout << "Usage: hermes_export [--port 9090] [--socket path] [--once]\n"
                  << "  --once  write metrics to stdout and exit\n";
        return 0;
    }

    if (once) {
        std::cout << build_metrics(sock_path, artifact_root);
        return 0;
    }

#ifdef _WIN32
    WSADATA wsd; WSAStartup(MAKEWORD(2, 2), &wsd);
#else
    signal(SIGPIPE, SIG_IGN);
#endif

    const SockType server = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server == BAD_SOCK) { std::cerr << "hermes_export: socket failed\n"; return 1; }

    int opt = 1;
#ifdef _WIN32
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = INADDR_ANY;
    if (::bind(server, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "hermes_export: bind failed on port " << port << "\n";
        close_sock(server); return 1;
    }
    ::listen(server, 8);

    std::cout << "hermes_export serving Prometheus metrics on http://localhost:" << port << "/metrics\n";
    std::cout << "  Daemon socket : " << sock_path << "\n";
    std::cout << "  Artifacts     : " << artifact_root << "\n";
    std::cout << "  Ctrl-C to stop\n";

    while (true) {
        struct sockaddr_in ca{};
#ifdef _WIN32
        int cl = sizeof(ca);
#else
        socklen_t cl = sizeof(ca);
#endif
        const SockType client = ::accept(server, reinterpret_cast<struct sockaddr*>(&ca), &cl);
        if (client == BAD_SOCK) continue;
        std::thread([client, &sock_path, &artifact_root]() {
            handle_client(client, sock_path, artifact_root);
        }).detach();
    }

    close_sock(server);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
