#include "http.hpp"

#include <arpa/inet.h>
#include <format>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 // macOS has none; main() ignores SIGPIPE instead
#endif

namespace mulcan::host {

namespace {
void send_all(int fd, std::string_view data) {
    while (!data.empty()) {
        const ssize_t n = send(fd, data.data(), data.size(), MSG_NOSIGNAL);
        if (n <= 0) return;
        data.remove_prefix(static_cast<std::size_t>(n));
    }
}
} // namespace

HttpServer::HttpServer(unsigned port, Handler handler) : handler_(std::move(handler)) {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    const int yes = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(listen_fd_, 8) != 0) {
        close(listen_fd_);
        throw std::runtime_error(std::format("cannot listen on 127.0.0.1:{}", port));
    }
    thread_ = std::thread([this] { serve(); });
}

HttpServer::~HttpServer() {
    stop_ = true;
    thread_.join();
    close(listen_fd_);
}

void HttpServer::serve() {
    while (!stop_) {
        pollfd p{listen_fd_, POLLIN, 0};
        if (poll(&p, 1, 200) <= 0) continue;
        const int client = accept(listen_fd_, nullptr, nullptr);
        if (client < 0) continue;
        const timeval timeout{1, 0};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);

        std::string request(8192, '\0');
        std::size_t got = 0;
        while (got < request.size()) {
            const ssize_t n = recv(client, request.data() + got, request.size() - got, 0);
            if (n <= 0) break;
            got += static_cast<std::size_t>(n);
            if (std::string_view(request.data(), got).find("\r\n\r\n") != std::string_view::npos) break;
        }
        const std::string_view head(request.data(), got);
        std::optional<Response> response;
        if (head.starts_with("GET ")) {
            const auto target = head.substr(4, head.find(' ', 4) - 4);
            const auto q = target.find('?');
            response = handler_(target.substr(0, q), q == std::string_view::npos ? "" : target.substr(q + 1));
        }
        if (response) {
            send_all(client, std::format("HTTP/1.1 200 OK\r\nContent-Type: {}\r\nContent-Length: {}\r\n"
                                         "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                                         response->content_type, response->body.size()));
            send_all(client, response->body);
        } else {
            send_all(client, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        }
        close(client);
    }
}

} // namespace mulcan::host
