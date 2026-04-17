// hermes_watchdog: health monitor for hermesd with automatic restart.
//
// Polls the hermesd control socket at a fixed interval. If the daemon becomes
// unreachable (socket timeout / connection refused), the watchdog:
//   1. Logs the failure with timestamp.
//   2. Waits --restart-delay-s seconds (default 3).
//   3. Spawns a new hermesd process using --hermesd-bin (default: ./hermesd).
//   4. Resets the failure counter and continues polling.
//
// Optionally calls --alert-cmd on restart (e.g. a curl webhook).
//
// Usage:
//   hermes_watchdog [--hermesd-bin ./hermesd] [--socket /tmp/hermesd.sock]
//                   [--interval-ms 2000] [--restart-delay-s 3]
//                   [--max-restarts 5] [--alert-cmd "curl -s https://..."]
//
// hermes_watchdog itself is intentionally simple and single-threaded.
// Run it under systemd, supervisord, or a screen session for daemon-level HA.

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#  include <sys/socket.h>
#  include <sys/un.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  include <signal.h>
#elif defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

namespace {

bool g_running = true;

bool has_arg(const std::vector<std::string>& a, const std::string& f) {
    for (const auto& x : a) if (x == f) return true;
    return false;
}
std::string get_arg(const std::vector<std::string>& a, const std::string& f,
                    const std::string& def) {
    for (std::size_t i = 0; i + 1 < a.size(); ++i) if (a[i] == f) return a[i + 1];
    return def;
}
std::string env_or(const char* n, const std::string& def) {
    const char* v = std::getenv(n);
    return (v && v[0]) ? v : def;
}

std::string now_str() {
    const auto t = std::chrono::system_clock::now();
    const auto ts = std::chrono::duration_cast<std::chrono::seconds>(
        t.time_since_epoch()).count();
    const std::time_t tt = static_cast<std::time_t>(ts);
    char buf[32] = {};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&tt));
    return buf;
}

// ---- Probe the control socket ----

bool daemon_alive(const std::string& sock_path) {
#ifdef __linux__
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;

    // Non-blocking connect with short timeout via select().
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd); return false;
    }

    const char req[] = "{\"kind\":\"ping\"}\n";
    ::send(fd, req, sizeof(req) - 1, 0);

    char buf[64] = {};
    const ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
    ::close(fd);
    return n > 0;
#else
    (void)sock_path;
    // On Windows, assume alive (no Unix socket support).
    return true;
#endif
}

// ---- Spawn hermesd ----

bool spawn_daemon(const std::string& bin) {
#ifdef __linux__
    const pid_t pid = ::fork();
    if (pid < 0) {
        std::cerr << "[watchdog] fork failed\n";
        return false;
    }
    if (pid == 0) {
        // Child: exec hermesd.
        ::setsid();
        ::execlp(bin.c_str(), bin.c_str(), nullptr);
        ::_exit(127);
    }
    // Parent: detach child.
    std::cout << "[watchdog] " << now_str() << " spawned hermesd pid=" << pid << "\n";
    return true;
#elif defined(_WIN32)
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessA(
        nullptr, const_cast<char*>(bin.c_str()),
        nullptr, nullptr, FALSE,
        DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
        nullptr, nullptr, &si, &pi);
    if (!ok) {
        std::cerr << "[watchdog] CreateProcess failed for: " << bin << "\n";
        return false;
    }
    std::cout << "[watchdog] " << now_str()
              << " spawned hermesd pid=" << pi.dwProcessId << "\n";
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
#else
    std::cerr << "[watchdog] spawn not supported on this platform\n";
    return false;
#endif
}

} // namespace

int main(int argc, char* argv[]) {
    const std::vector<std::string> args(argv + 1, argv + argc);

    if (has_arg(args, "--help")) {
        std::cout <<
            "Usage: hermes_watchdog [options]\n"
            "  --hermesd-bin  <path>  Path to hermesd binary (default: ./hermesd)\n"
            "  --socket       <path>  Control socket (default: /tmp/hermesd.sock)\n"
            "  --interval-ms  <n>     Poll interval ms (default: 2000)\n"
            "  --restart-delay-s <n>  Seconds to wait before restart (default: 3)\n"
            "  --max-restarts <n>     Max restart attempts before giving up (default: 5; 0=unlimited)\n"
            "  --alert-cmd    <cmd>   Shell command to run on each restart\n";
        return 0;
    }

    const std::string hermesd_bin = get_arg(args, "--hermesd-bin", "./hermesd");
    const std::string sock_path   = get_arg(args, "--socket",
        env_or("HERMES_SOCKET_PATH", "/tmp/hermesd.sock"));
    const int interval_ms = [&]() {
        try { return std::stoi(get_arg(args, "--interval-ms", "2000")); } catch (...) { return 2000; }
    }();
    const int restart_delay_s = [&]() {
        try { return std::stoi(get_arg(args, "--restart-delay-s", "3")); } catch (...) { return 3; }
    }();
    const int max_restarts = [&]() {
        try { return std::stoi(get_arg(args, "--max-restarts", "5")); } catch (...) { return 5; }
    }();
    const std::string alert_cmd = get_arg(args, "--alert-cmd", "");

#ifdef __linux__
    signal(SIGCHLD, SIG_IGN);  // auto-reap child processes
    signal(SIGTERM, [](int) { g_running = false; });
    signal(SIGINT,  [](int) { g_running = false; });
#endif

    std::cout << "[watchdog] " << now_str() << " started\n";
    std::cout << "[watchdog] monitoring hermesd via " << sock_path << "\n";
    std::cout << "[watchdog] restart binary: " << hermesd_bin << "\n";
    std::cout << "[watchdog] poll interval: " << interval_ms << " ms\n";
    std::cout << "[watchdog] max restarts: " << (max_restarts == 0 ? "unlimited" : std::to_string(max_restarts)) << "\n\n";

    int consecutive_failures = 0;
    int total_restarts = 0;

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));

        if (daemon_alive(sock_path)) {
            consecutive_failures = 0;
            continue;
        }

        ++consecutive_failures;
        std::cerr << "[watchdog] " << now_str()
                  << " daemon unreachable (consecutive failures: " << consecutive_failures << ")\n";

        if (consecutive_failures < 2) {
            // Transient blip — wait one more cycle before restarting.
            continue;
        }

        if (max_restarts > 0 && total_restarts >= max_restarts) {
            std::cerr << "[watchdog] " << now_str()
                      << " max restarts reached (" << max_restarts << "). Giving up.\n";
            return 1;
        }

        std::cout << "[watchdog] " << now_str()
                  << " restarting hermesd (attempt " << (total_restarts + 1) << ")...\n";

        std::this_thread::sleep_for(std::chrono::seconds(restart_delay_s));

        if (spawn_daemon(hermesd_bin)) {
            ++total_restarts;
            consecutive_failures = 0;
            // Allow daemon startup time before probing again.
            std::this_thread::sleep_for(std::chrono::seconds(2));

            if (!alert_cmd.empty()) {
                std::system(alert_cmd.c_str());
            }
        }
    }

    std::cout << "[watchdog] " << now_str()
              << " exiting. Total restarts: " << total_restarts << "\n";
    return 0;
}
