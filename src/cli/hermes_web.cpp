// hermes_web: lightweight embedded HTTP server exposing a live browser dashboard.
//
// Opens a TCP socket on --port (default 7070), serves a self-contained HTML
// dashboard at GET /, and provides a JSON status endpoint at GET /api/status
// which proxies the running hermesd control socket.
//
// The dashboard auto-refreshes every --refresh-ms milliseconds (default 1500)
// using plain JavaScript fetch — no build toolchain or npm required.
//
// Usage:
//   hermes_web [--port 7070] [--socket /tmp/hermesd.sock] [--refresh-ms 1500]
//
// Open http://localhost:7070 in your browser after starting hermes_web.
// hermes_web stays running until Ctrl-C.
//
// Compile note: on Windows this requires Winsock2; on Linux uses POSIX sockets.
// For TLS/HTTPS, proxy through nginx or stunnel.

#include <algorithm>
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

bool g_running = true;

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

// ---- Unix domain socket → hermesd status ----

std::string fetch_daemon_status(const std::string& sock_path) {
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
    (void)sock_path;
    // On Windows/non-Linux: return a mock status for dashboard testing.
    return "{\"run_id\":\"no-daemon\","
           "\"ups\":0.0,\"pressure_band\":\"normal\","
           "\"risk_score\":0.0,\"risk_band\":\"low\","
           "\"scheduler_state\":\"Normal\","
           "\"last_action\":\"observe\","
           "\"sample_count\":0,\"drop_count\":0,"
           "\"note\":\"no daemon running on this platform\"}";
#endif
}

// ---- Embedded HTML dashboard ----

std::string dashboard_html(int refresh_ms) {
    return R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>Hermes — Live Dashboard</title>
<style>
  body { font-family: monospace; background: #0d1117; color: #c9d1d9; margin: 0; padding: 20px; }
  h1 { color: #58a6ff; margin-bottom: 4px; }
  .subtitle { color: #8b949e; font-size: 12px; margin-bottom: 24px; }
  .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(280px, 1fr)); gap: 16px; }
  .card { background: #161b22; border: 1px solid #30363d; border-radius: 8px; padding: 16px; }
  .card h2 { font-size: 13px; color: #8b949e; margin: 0 0 12px 0; text-transform: uppercase; letter-spacing: 1px; }
  .metric { display: flex; justify-content: space-between; margin: 6px 0; }
  .metric .label { color: #8b949e; }
  .metric .value { color: #e6edf3; font-weight: bold; }
  .badge { display: inline-block; padding: 2px 8px; border-radius: 4px; font-size: 12px; font-weight: bold; }
  .badge-normal   { background: #1f6feb22; color: #58a6ff; border: 1px solid #1f6feb; }
  .badge-elevated { background: #9e6a0322; color: #d29922; border: 1px solid #9e6a03; }
  .badge-critical { background: #da363322; color: #f85149; border: 1px solid #da3633; }
  .badge-throttled{ background: #da363322; color: #f85149; border: 1px solid #da3633; }
  .badge-cooldown { background: #6e409922; color: #bc8cff; border: 1px solid #6e4099; }
  .badge-recovery { background: #23863622; color: #3fb950; border: 1px solid #238636; }
  .badge-low      { background: #23863622; color: #3fb950; border: 1px solid #238636; }
  .badge-medium   { background: #9e6a0322; color: #d29922; border: 1px solid #9e6a03; }
  .badge-high     { background: #da363322; color: #f85149; border: 1px solid #da3633; }
  .ups-bar-wrap { background: #21262d; border-radius: 4px; height: 16px; margin: 8px 0; overflow: hidden; }
  .ups-bar { height: 100%; border-radius: 4px; transition: width 0.4s ease, background 0.4s ease; }
  .error { color: #f85149; font-size: 12px; margin-top: 8px; }
  .ts { color: #8b949e; font-size: 11px; margin-top: 20px; }
  #status-dot { display: inline-block; width: 8px; height: 8px; border-radius: 50%; background: #3fb950; margin-right: 6px; }
  #status-dot.err { background: #f85149; }
</style>
</head>
<body>
<h1><span id="status-dot"></span>Hermes Live Dashboard</h1>
<p class="subtitle">Auto-refreshes every )HTML" + std::to_string(refresh_ms) + R"HTML(ms &nbsp;|&nbsp; <a href="/api/status" style="color:#58a6ff">Raw JSON</a></p>

<div class="grid">
  <div class="card">
    <h2>Unified Pressure Score</h2>
    <div class="metric"><span class="label">UPS</span><span class="value" id="ups">—</span></div>
    <div class="ups-bar-wrap"><div class="ups-bar" id="ups-bar" style="width:0%"></div></div>
    <div class="metric"><span class="label">Band</span><span class="value" id="p-band">—</span></div>
    <div class="metric"><span class="label">Risk Score</span><span class="value" id="risk">—</span></div>
    <div class="metric"><span class="label">Risk Band</span><span class="value" id="r-band">—</span></div>
  </div>

  <div class="card">
    <h2>Scheduler</h2>
    <div class="metric"><span class="label">State</span><span class="value" id="sched-state">—</span></div>
    <div class="metric"><span class="label">Last Action</span><span class="value" id="last-action">—</span></div>
    <div class="metric"><span class="label">Samples</span><span class="value" id="samples">—</span></div>
    <div class="metric"><span class="label">Drops</span><span class="value" id="drops">—</span></div>
  </div>

  <div class="card">
    <h2>Run Info</h2>
    <div class="metric"><span class="label">Run ID</span><span class="value" id="run-id" style="font-size:11px">—</span></div>
    <div class="metric"><span class="label">Mode</span><span class="value" id="mode">—</span></div>
    <div id="note-row" class="metric" style="display:none">
      <span class="label">Note</span><span class="value error" id="note">—</span>
    </div>
  </div>
</div>
<p class="ts" id="ts">Last update: —</p>

<script>
const refresh = )HTML" + std::to_string(refresh_ms) + R"HTML(;

function badge(text, extra) {
  const cls = (extra || text || "").toLowerCase().replace(/[^a-z]/g,"");
  return `<span class="badge badge-${cls}">${text}</span>`;
}

function ups_color(v) {
  if (v >= 70) return "#f85149";
  if (v >= 40) return "#d29922";
  return "#3fb950";
}

async function refresh_status() {
  const dot = document.getElementById("status-dot");
  try {
    const r = await fetch("/api/status", {cache:"no-store"});
    if (!r.ok) throw new Error("HTTP " + r.status);
    const d = await r.json();
    dot.classList.remove("err");

    const ups = parseFloat(d.ups || 0).toFixed(2);
    document.getElementById("ups").textContent = ups;
    const bar = document.getElementById("ups-bar");
    bar.style.width = Math.min(parseFloat(ups), 100) + "%";
    bar.style.background = ups_color(parseFloat(ups));

    document.getElementById("p-band").innerHTML  = badge(d.pressure_band  || "—");
    document.getElementById("risk").textContent  = parseFloat(d.risk_score || 0).toFixed(3);
    document.getElementById("r-band").innerHTML  = badge(d.risk_band || "—");
    document.getElementById("sched-state").innerHTML = badge(d.scheduler_state || "—");
    document.getElementById("last-action").textContent = d.last_action || "—";
    document.getElementById("samples").textContent = d.sample_count ?? "—";
    document.getElementById("drops").textContent   = d.drop_count ?? "—";
    document.getElementById("run-id").textContent  = d.run_id || "—";
    document.getElementById("mode").textContent    = d.mode || "—";

    const nr = document.getElementById("note-row");
    if (d.note) { nr.style.display="flex"; document.getElementById("note").textContent = d.note; }
    else nr.style.display="none";

    document.getElementById("ts").textContent = "Last update: " + new Date().toLocaleTimeString();
  } catch(e) {
    dot.classList.add("err");
    document.getElementById("ts").textContent = "Error: " + e.message;
  }
}

refresh_status();
setInterval(refresh_status, refresh);
</script>
</body>
</html>)HTML";
}

// ---- Simple HTTP response builders ----

std::string http_ok(const std::string& content_type, const std::string& body) {
    return "HTTP/1.0 200 OK\r\nContent-Type: " + content_type +
           "\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n" + body;
}
std::string http_404() {
    const std::string b = "Not Found";
    return "HTTP/1.0 404 Not Found\r\nContent-Length: " + std::to_string(b.size()) +
           "\r\nConnection: close\r\n\r\n" + b;
}

// ---- Handle one HTTP client connection ----

void handle_client(SockType client, const std::string& dashboard,
                   const std::string& sock_path) {
    char buf[2048] = {};
#ifdef __linux__
    const ssize_t n = ::recv(client, buf, sizeof(buf) - 1, 0);
#else
    const int n = ::recv(client, buf, static_cast<int>(sizeof(buf) - 1), 0);
#endif
    if (n <= 0) { close_sock(client); return; }

    const std::string req(buf, static_cast<std::size_t>(n));
    std::string response;

    if (req.find("GET /api/status") != std::string::npos) {
        const std::string status = fetch_daemon_status(sock_path);
        const std::string body = status.empty()
            ? "{\"error\":\"daemon not reachable\"}" : status;
        response = http_ok("application/json", body);
    } else if (req.find("GET / ") != std::string::npos ||
               req.find("GET / \r") != std::string::npos ||
               req.find("GET /\r") != std::string::npos) {
        response = http_ok("text/html; charset=utf-8", dashboard);
    } else {
        response = http_404();
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
        try { return std::stoi(get_arg(args, "--port", "7070")); } catch (...) { return 7070; }
    }();
    const int refresh_ms = [&]() {
        try { return std::stoi(get_arg(args, "--refresh-ms", "1500")); } catch (...) { return 1500; }
    }();
    const std::string sock_path = get_arg(args, "--socket",
        env_or("HERMES_SOCKET_PATH", "/tmp/hermesd.sock"));

    if (has_arg(args, "--help")) {
        std::cout << "Usage: hermes_web [--port 7070] [--socket path] [--refresh-ms 1500]\n";
        return 0;
    }

#ifdef _WIN32
    WSADATA wsd;
    WSAStartup(MAKEWORD(2, 2), &wsd);
#else
    signal(SIGPIPE, SIG_IGN);
#endif

    const SockType server = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server == BAD_SOCK) {
        std::cerr << "hermes_web: failed to create socket\n";
        return 1;
    }

    int opt = 1;
#ifdef _WIN32
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(server, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "hermes_web: bind failed on port " << port << "\n";
        close_sock(server);
        return 1;
    }
    ::listen(server, 16);

    const std::string dash = dashboard_html(refresh_ms);

    std::cout << "hermes_web listening on http://localhost:" << port << "\n";
    std::cout << "  Control socket : " << sock_path << "\n";
    std::cout << "  Dashboard      : http://localhost:" << port << "/\n";
    std::cout << "  Status API     : http://localhost:" << port << "/api/status\n";
    std::cout << "  Refresh        : " << refresh_ms << " ms\n";
    std::cout << "  Ctrl-C to stop\n\n";

    while (g_running) {
        struct sockaddr_in client_addr{};
#ifdef _WIN32
        int client_len = sizeof(client_addr);
#else
        socklen_t client_len = sizeof(client_addr);
#endif
        const SockType client = ::accept(
            server, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
        if (client == BAD_SOCK) continue;

        // Spawn a detached thread per connection (keep it simple — low traffic).
        std::thread([client, &dash, &sock_path]() {
            handle_client(client, dash, sock_path);
        }).detach();
    }

    close_sock(server);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
