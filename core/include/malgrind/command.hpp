#pragma once

// Freestanding host-to-agent COMMAND frame parser (no heap, no exceptions).

#include <cstddef>
#include <cstdint>

namespace malgrind {

struct Command {
    std::uint8_t cmd = 0;
    std::uint16_t rate_hz = 0;
    std::uint32_t analyses = 0;
};

class CommandParser {
public:
    static constexpr std::size_t kFrameSize = 19; // "MLC1"(4) + len(2) + body(9) + crc32(4)

    [[nodiscard]] bool feed(std::uint8_t byte, Command& out) noexcept;

    template <class Fn>
    void feed(const std::uint8_t* bytes, std::size_t n, Fn&& fn) {
        if (!bytes) return;
        for (std::size_t i = 0; i < n; ++i) {
            Command cmd{};
            if (feed(bytes[i], cmd)) fn(cmd);
        }
    }

    void reset() noexcept;

private:
    void resync() noexcept;

    std::uint8_t buf_[kFrameSize]{};
    std::size_t len_ = 0;
};

} // namespace malgrind
