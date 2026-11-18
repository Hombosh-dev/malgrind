#pragma once

#include "os/serial.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>

namespace malgrind::host {

// Byte stream from a serial port, or from stdin when the port is "-".
class Source {
public:
    [[nodiscard]] static std::variant<Source, os::OpenFailure> open_result(const std::string& port, unsigned baud);
    [[nodiscard]] static std::optional<Source> open(const std::string& port, unsigned baud);

    Source(Source&&) noexcept;
    Source& operator=(Source&&) noexcept;
    Source(const Source&) = delete;
    Source& operator=(const Source&) = delete;
    ~Source();

    // Bytes read; 0 if nothing arrived within the timeout; nullopt at end of stream or on error.
    [[nodiscard]] std::optional<std::size_t> read(std::span<std::uint8_t> out, int timeout_ms = 200);
    // All bytes or false (returns false on stdin).
    [[nodiscard]] bool write(std::span<const std::uint8_t> data);

private:
    struct StdinState;
    explicit Source(os::SerialPort serial) noexcept : impl_(std::move(serial)) {}
    explicit Source(std::shared_ptr<StdinState> stdin_state) noexcept : impl_(std::move(stdin_state)) {}

    std::variant<os::SerialPort, std::shared_ptr<StdinState>> impl_;
};

} // namespace malgrind::host
