#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <utility>

namespace malgrind::host {

// USB serial adapters and native USB CDC ports (ttyUSB*, ttyACM*) in dev_dir, sorted.
[[nodiscard]] std::vector<std::string> find_serial_ports(const std::filesystem::path& dev_dir = "/dev");

// Byte stream from a serial port, from stdin when the port is "-", or from a file descriptor with an owner (a demo process).
class Source {
public:
    [[nodiscard]] static std::optional<Source> open(const std::string& port, unsigned baud);
    // Reads `fd` (and closes it). `owner` is released after the descriptor is closed: it is what keeps a child process alive.
    [[nodiscard]] static Source from_fd(int fd, std::shared_ptr<void> owner);

    Source(Source&& other) noexcept : fd_(std::exchange(other.fd_, -1)), owns_(other.owns_), owner_(std::move(other.owner_)) {}
    Source(const Source&) = delete;
    ~Source();

    // Bytes read; 0 if nothing arrived within the timeout; nullopt at end of stream or on error.
    [[nodiscard]] std::optional<std::size_t> read(std::span<std::uint8_t> out, int timeout_ms = 200);

private:
    Source(int fd, bool owns) : fd_(fd), owns_(owns) {}
    int fd_;
    bool owns_;
    std::shared_ptr<void> owner_; // destroyed after the destructor body has closed fd_
};

} // namespace malgrind::host
