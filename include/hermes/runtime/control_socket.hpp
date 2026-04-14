#pragma once

#include "hermes/core/types.hpp"

#include <functional>
#include <string>
#include <thread>

namespace hermes {

// ControlSocket provides a Unix domain socket server that the running hermesd
// daemon uses to expose live status to hermesctl clients.
//
// Protocol: line-delimited JSON over AF_UNIX SOCK_STREAM.
//   Client connects, sends a single-line request JSON, reads a single-line response JSON.
//
// Supported request kinds:
//   {"kind":"status"}    — returns latest UPS, risk, scheduler state, top processes
//   {"kind":"ping"}      — returns {"kind":"pong"}
//   {"kind":"shutdown"}  — signals daemon to stop (only in non-production mode)
//
// The socket path defaults to /tmp/hermesd.sock but can be overridden via
// HERMES_SOCKET_PATH env var.
//
// On non-Linux platforms, start() immediately returns false and the daemon
// continues running without the control socket.

struct DaemonStatus {
    uint64_t ts_wall{0};
    double ups{0.0};
    std::string pressure_band;
    double risk_score{0.0};
    std::string risk_band;
    std::string scheduler_state;
    std::string last_action;
    std::string run_id;
    uint64_t sample_count{0};
    uint64_t drop_count{0};
};

class ControlSocket {
public:
    explicit ControlSocket(std::string socket_path = "/tmp/hermesd.sock");
    ~ControlSocket();

    // Start the accept loop in a background thread. Returns true on Linux when
    // the socket was bound successfully. Returns false on other platforms.
    bool start();

    // Stop the accept loop and close the socket.
    void stop();

    // Update the status snapshot (called by the policy thread after each cycle).
    void update_status(const DaemonStatus& status);

    bool is_running() const { return running_; }
    const std::string& socket_path() const { return socket_path_; }
    const std::string& last_error() const { return last_error_; }

private:
    void accept_loop();
    void handle_client(int client_fd);
    std::string build_status_json() const;

    std::string socket_path_;
    int server_fd_{-1};
    bool running_{false};
    std::string last_error_;
    std::thread accept_thread_;

    mutable std::mutex status_mutex_;
    DaemonStatus current_status_;
};

} // namespace hermes
