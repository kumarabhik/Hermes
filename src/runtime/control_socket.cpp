#include "hermes/runtime/control_socket.hpp"

#include <sstream>

#ifdef __linux__
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <poll.h>
#endif

namespace hermes {

ControlSocket::ControlSocket(std::string socket_path)
    : socket_path_(std::move(socket_path)) {}

ControlSocket::~ControlSocket() {
    stop();
}

bool ControlSocket::start() {
#ifndef __linux__
    last_error_ = "control socket not supported on this platform";
    return false;
#else
    server_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        last_error_ = std::string("socket() failed: ") + std::strerror(errno);
        return false;
    }

    // Remove stale socket file
    ::unlink(socket_path_.c_str());

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        last_error_ = std::string("bind() failed: ") + std::strerror(errno);
        ::close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    if (::listen(server_fd_, 8) != 0) {
        last_error_ = std::string("listen() failed: ") + std::strerror(errno);
        ::close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    running_ = true;
    accept_thread_ = std::thread([this] { accept_loop(); });
    return true;
#endif
}

void ControlSocket::stop() {
    running_ = false;
#ifdef __linux__
    if (server_fd_ >= 0) {
        ::shutdown(server_fd_, SHUT_RDWR);
        ::close(server_fd_);
        server_fd_ = -1;
        ::unlink(socket_path_.c_str());
    }
#endif
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
}

void ControlSocket::update_status(const DaemonStatus& status) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    current_status_ = status;
}

std::string ControlSocket::build_status_json() const {
    // Called under status_mutex_
    std::ostringstream oss;
    oss << "{\"kind\":\"status\""
        << ",\"ts_wall\":" << current_status_.ts_wall
        << ",\"ups\":" << current_status_.ups
        << ",\"pressure_band\":\"" << current_status_.pressure_band << "\""
        << ",\"risk_score\":" << current_status_.risk_score
        << ",\"risk_band\":\"" << current_status_.risk_band << "\""
        << ",\"scheduler_state\":\"" << current_status_.scheduler_state << "\""
        << ",\"last_action\":\"" << current_status_.last_action << "\""
        << ",\"run_id\":\"" << current_status_.run_id << "\""
        << ",\"sample_count\":" << current_status_.sample_count
        << ",\"drop_count\":" << current_status_.drop_count
        << "}";
    return oss.str();
}

void ControlSocket::accept_loop() {
#ifdef __linux__
    while (running_) {
        struct pollfd pfd{};
        pfd.fd = server_fd_;
        pfd.events = POLLIN;
        const int ready = ::poll(&pfd, 1, 200); // 200ms timeout so we check running_ regularly
        if (ready <= 0) continue;

        struct sockaddr_un client_addr{};
        socklen_t client_len = sizeof(client_addr);
        const int client_fd = ::accept(server_fd_,
            reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) continue;

        handle_client(client_fd);
    }
#endif
}

void ControlSocket::handle_client(int client_fd) {
#ifdef __linux__
    char buf[512] = {};
    const ssize_t n = ::recv(client_fd, buf, sizeof(buf) - 1, 0);

    std::string response;
    if (n > 0) {
        const std::string request(buf, static_cast<std::size_t>(n));
        if (request.find("\"ping\"") != std::string::npos) {
            response = "{\"kind\":\"pong\"}\n";
        } else if (request.find("\"status\"") != std::string::npos) {
            std::lock_guard<std::mutex> lock(status_mutex_);
            response = build_status_json() + "\n";
        } else {
            response = "{\"kind\":\"error\",\"msg\":\"unknown request\"}\n";
        }
    } else {
        response = "{\"kind\":\"error\",\"msg\":\"empty request\"}\n";
    }

    ::send(client_fd, response.c_str(), response.size(), 0);
    ::close(client_fd);
#else
    (void)client_fd;
#endif
}

} // namespace hermes
