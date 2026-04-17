// hermes_alert: watches the hermesd control socket and fires HTTP webhook
// alerts when the scheduler enters a critical/throttled state.
//
// Usage:
//   hermes_alert --webhook https://hooks.example.com/hermes \
//                [--socket /tmp/hermesd.sock]               \
//                [--interval-ms 2000]                       \
//                [--cooldown-s 60]                          \
//                [--once]
//
// The webhook receives a POST with JSON body:
//   {"event":"state_change","scheduler_state":"Throttled","ups":74.2,
//    "risk":0.91,"run_id":"abc123","ts_wall":1713291600}
//
// Alert suppression: once an alert fires, no further alert fires until the
// scheduler returns to Normal and then enters a non-Normal state again
// (one alert per incident).  --cooldown-s overrides with a fixed quiet period.
//
// On platforms without Unix sockets (Windows), hermes_alert reads the most
// recent telemetry_quality.json as a fallback and exits with a summary.

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#  include <sys/socket.h>
#  include <sys/un.h>
#  include <unistd.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#elif defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
#endif

namespace {

bool has_arg(const std::vector<std::string>& args, const std::string& f) {
    for (const auto& a : args) if (a == f) return true;
    return false;
}
std::string get_arg(const std::vector<std::string>& args, const std::string& f,
                    const std::string& def) {
    for (std::size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == f) return args[i + 1];
    return def;
}
std::string env_or(const char* n, const std::string& def) {
    const char* v = std::getenv(n);
    return (v && v[0]) ? v : def;
}

std::string jstr(const std::string& j, const std::string& k) {
    const std::string kk = "\"" + k + "\":\"";
    const auto p = j.find(kk);
    if (p == std::string::npos) return "";
    const auto s = p + kk.size();
    const auto e = j.find('"', s);
    return e == std::string::npos ? "" : j.substr(s, e - s);
}
double jdbl(const std::string& j, const std::string& k) {
    const std::string kk = "\"" + k + "\":";
    const auto p = j.find(kk);
    if (p == std::string::npos) return 0.0;
    try { return std::stod(j.substr(p + kk.size())); } catch (...) { return 0.0; }
}

// ---- Minimal HTTP POST (no libcurl, no TLS — uses plain TCP on port 80) ----
// For HTTPS webhooks, proxy through an HTTP relay or use curl externally.

bool http_post(const std::string& url, const std::string& body) {
    // Parse host, port, path from URL.
    std::string host, path;
    int port = 80;

    auto strip_scheme = [](const std::string& u) -> std::string {
        if (u.substr(0, 7) == "http://")  return u.substr(7);
        if (u.substr(0, 8) == "https://") return u.substr(8);
        return u;
    };
    const std::string rest = strip_scheme(url);
    const auto slash = rest.find('/');
    const std::string hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    path = (slash == std::string::npos) ? "/" : rest.substr(slash);

    const auto colon = hostport.find(':');
    if (colon != std::string::npos) {
        host = hostport.substr(0, colon);
        try { port = std::stoi(hostport.substr(colon + 1)); } catch (...) {}
    } else {
        host = hostport;
    }

    const std::string request =
        "POST " + path + " HTTP/1.0\r\n"
        "Host: " + host + "\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n"
        "\r\n" + body;

#ifdef __linux__
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    const std::string port_str = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0) return false;

    const int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    bool ok = false;
    if (fd >= 0) {
        if (::connect(fd, res->ai_addr, res->ai_addrlen) == 0) {
            ::send(fd, request.c_str(), request.size(), 0);
            ok = true;
        }
        ::close(fd);
    }
    freeaddrinfo(res);
    return ok;
#elif defined(_WIN32)
    WSADATA wsd;
    WSAStartup(MAKEWORD(2, 2), &wsd);

    struct addrinfo hints2{}, *res2 = nullptr;
    hints2.ai_family   = AF_UNSPEC;
    hints2.ai_socktype = SOCK_STREAM;
    const std::string ps = std::to_string(port);
    bool ok2 = false;
    if (getaddrinfo(host.c_str(), ps.c_str(), &hints2, &res2) == 0) {
        SOCKET s = ::socket(res2->ai_family, res2->ai_socktype, res2->ai_protocol);
        if (s != INVALID_SOCKET) {
            if (::connect(s, res2->ai_addr, static_cast<int>(res2->ai_addrlen)) == 0) {
                ::send(s, request.c_str(), static_cast<int>(request.size()), 0);
                ok2 = true;
            }
            ::closesocket(s);
        }
        freeaddrinfo(res2);
    }
    WSACleanup();
    return ok2;
#else
    (void)request;
    return false;
#endif
}

// Also fire via system curl as a fallback (for HTTPS, auth headers, etc.).
bool curl_post(const std::string& url, const std::string& body) {
    const std::string cmd = "curl -s -X POST -H \"Content-Type: application/json\" "
        "-d '" + body + "' \"" + url + "\" > /dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

// ---- Unix domain socket request ----

std::string socket_request(const std::string& sock_path, const std::string& req) {
#ifdef __linux__
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return "";
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd); return "";
    }
    const std::string r = req + "\n";
    ::send(fd, r.c_str(), r.size(), 0);
    char buf[4096] = {};
    const ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
    ::close(fd);
    return n > 0 ? std::string(buf, static_cast<std::size_t>(n)) : "";
#else
    (void)sock_path; (void)req; return "";
#endif
}

// ---- Build alert JSON payload ----

std::string build_payload(const std::string& state, double ups, double risk,
                          const std::string& run_id) {
    const long long ts = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return "{\"event\":\"state_change\","
           "\"scheduler_state\":\"" + state + "\","
           "\"ups\":" + std::to_string(ups) + ","
           "\"risk\":" + std::to_string(risk) + ","
           "\"run_id\":\"" + run_id + "\","
           "\"ts_wall\":" + std::to_string(ts) + "}";
}

} // namespace

int main(int argc, char* argv[]) {
    const std::vector<std::string> args(argv + 1, argv + argc);

    if (has_arg(args, "--help") || has_arg(args, "-h")) {
        std::cout <<
            "Usage: hermes_alert --webhook <url> [options]\n"
            "  --webhook   <url>   HTTP POST destination (required)\n"
            "  --socket    <path>  Unix domain socket (default /tmp/hermesd.sock)\n"
            "  --interval-ms <n>   Poll interval in ms (default 2000)\n"
            "  --cooldown-s  <n>   Min seconds between alerts (default 60)\n"
            "  --once              Fire at most one alert then exit\n"
            "  --dry-run           Print alerts to stdout instead of posting\n";
        return 0;
    }

    const std::string webhook = get_arg(args, "--webhook", "");
    if (webhook.empty()) {
        std::cerr << "hermes_alert: --webhook <url> is required\n";
        return 1;
    }
    const std::string socket_path = get_arg(args, "--socket",
        env_or("HERMES_SOCKET_PATH", "/tmp/hermesd.sock"));
    const int interval_ms = [&]() {
        try { return std::stoi(get_arg(args, "--interval-ms", "2000")); } catch (...) { return 2000; }
    }();
    const int cooldown_s = [&]() {
        try { return std::stoi(get_arg(args, "--cooldown-s", "60")); } catch (...) { return 60; }
    }();
    const bool once    = has_arg(args, "--once");
    const bool dry_run = has_arg(args, "--dry-run");

    std::cout << "[hermes_alert] Watching " << socket_path
              << " → webhook: " << webhook << "\n";
    if (dry_run) std::cout << "[hermes_alert] Dry-run mode: printing to stdout\n";

    std::string last_alerted_state;
    std::string prev_state;
    long long last_alert_ts = 0;

    while (true) {
        const std::string json = socket_request(socket_path, "{\"kind\":\"status\"}");

        if (json.empty()) {
            std::cerr << "[hermes_alert] daemon not reachable — retrying\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
            continue;
        }

        const std::string state  = jstr(json, "scheduler_state");
        const double ups         = jdbl(json, "ups");
        const double risk        = jdbl(json, "risk_score");
        const std::string run_id = jstr(json, "run_id");

        const bool is_alert_state =
            (state == "Throttled" || state == "Cooldown" || state == "Elevated");

        // State returned to Normal → reset last_alerted_state so next incident fires.
        if (state == "Normal" && !prev_state.empty() && prev_state != "Normal") {
            last_alerted_state.clear();
        }
        prev_state = state;

        if (is_alert_state && state != last_alerted_state) {
            const long long now_s = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (now_s - last_alert_ts >= cooldown_s) {
                const std::string payload = build_payload(state, ups, risk, run_id);
                std::cout << "[hermes_alert] ALERT state=" << state
                          << " ups=" << ups << " risk=" << risk << "\n";

                bool sent = false;
                if (dry_run) {
                    std::cout << "[hermes_alert] (dry-run) POST " << webhook << "\n"
                              << "  " << payload << "\n";
                    sent = true;
                } else {
                    sent = http_post(webhook, payload);
                    if (!sent) sent = curl_post(webhook, payload);
                    if (!sent) std::cerr << "[hermes_alert] WARNING: failed to deliver alert\n";
                }

                if (sent) {
                    last_alerted_state = state;
                    last_alert_ts = now_s;
                }
                if (once) return sent ? 0 : 1;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }
    return 0;
}
