#include "malgrind/command.hpp"

#include "malgrind/profiler.hpp"

namespace malgrind {

namespace {

constexpr std::uint8_t kCommandBodyLen = 9; // ver(1) + kind(1) + cmd(1) + rate_hz(2) + analyses(4)

bool candidate_valid(const std::uint8_t* buf, std::size_t rem) noexcept {
    constexpr std::uint8_t kMagic[4] = {'M', 'L', 'C', '1'};
    const std::size_t magic_n = rem < 4u ? rem : 4u;
    for (std::size_t i = 0; i < magic_n; ++i) {
        if (buf[i] != kMagic[i]) return false;
    }
    if (rem >= 5u && buf[4] != kCommandBodyLen) return false;
    if (rem >= 6u && buf[5] != 0u) return false;
    if (rem >= 7u && buf[6] != kWireVersion) return false;
    if (rem >= 8u && buf[7] != kKindCommand) return false;
    return true;
}

std::uint16_t read_u16_le(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[0]) |
                                      (static_cast<std::uint16_t>(p[1]) << 8u));
}

std::uint32_t read_u32_le(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8u) |
           (static_cast<std::uint32_t>(p[2]) << 16u) |
           (static_cast<std::uint32_t>(p[3]) << 24u);
}

} // namespace

bool CommandParser::feed(std::uint8_t byte, Command& out) noexcept {
    if (len_ >= kFrameSize) len_ = 0;
    buf_[len_++] = byte;

    if (!candidate_valid(buf_, len_)) {
        resync();
        return false;
    }
    if (len_ < kFrameSize) return false;

    const std::uint32_t expected_crc = read_u32_le(buf_ + 15);
    if (crc32(buf_ + 4, kCommandBodyLen + 2u) != expected_crc) {
        resync();
        return false;
    }

    out.cmd = buf_[8];
    out.rate_hz = read_u16_le(buf_ + 9);
    out.analyses = read_u32_le(buf_ + 11);
    len_ = 0;
    return true;
}

void CommandParser::reset() noexcept {
    len_ = 0;
}

void CommandParser::resync() noexcept {
    for (std::size_t pos = 1; pos < len_; ++pos) {
        const std::size_t rem = len_ - pos;
        if (candidate_valid(buf_ + pos, rem)) {
            for (std::size_t i = 0; i < rem; ++i) buf_[i] = buf_[pos + i];
            len_ = rem;
            return;
        }
    }
    len_ = 0;
}

} // namespace malgrind
