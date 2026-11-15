#include "wire.hpp"

#include "malgrind/profiler.hpp" // crc32

#include <algorithm>
#include <array>
#include <optional>

namespace malgrind::host {

namespace {

constexpr std::array<std::uint8_t, 4> kMagic{'M', 'L', 'C', '1'};
constexpr std::size_t kFixedBody = 30;
constexpr std::size_t kMaxBody = 16 * 1024;

// Bounds-checked little-endian reader; any overrun clears `ok`.
struct Cursor {
    std::span<const std::uint8_t> data;
    std::size_t pos = 0;
    bool ok = true;

    std::uint32_t take(unsigned bytes) {
        if (pos + bytes > data.size()) return ok = false, 0;
        std::uint32_t v = 0;
        for (unsigned i = bytes; i-- > 0;) v = (v << 8) | data[pos + i];
        pos += bytes;
        return v;
    }
};

std::optional<Frame> decode(std::span<const std::uint8_t> body) {
    Cursor c{body};
    if (c.take(1) != 1) return std::nullopt; // version
    Frame f;
    f.seq = static_cast<std::uint16_t>(c.take(2));
    f.arch = static_cast<Arch>(c.take(1));
    f.ncores = static_cast<std::uint8_t>(c.take(1));
    f.core = static_cast<std::uint8_t>(c.take(1));
    f.cpu_hz = c.take(4);
    f.rate_hz = static_cast<std::uint16_t>(c.take(2));
    f.samples = static_cast<std::uint16_t>(c.take(2));
    f.dropped = static_cast<std::uint16_t>(c.take(2));
    f.isr_avg = c.take(4);
    f.isr_max = c.take(4);
    f.flush_cyc = c.take(4);
    for (auto n = c.take(2); n > 0 && c.ok; --n) {
        Stack s{static_cast<std::uint16_t>(c.take(2)), {}};
        for (auto depth = c.take(1); depth > 0 && c.ok; --depth) s.pcs.push_back(c.take(4));
        f.stacks.push_back(std::move(s));
    }
    if (!c.ok) return std::nullopt;
    return f;
}

} // namespace

std::string_view arch_name(Arch arch) noexcept {
    switch (arch) {
        case Arch::Host: return "host";
        case Arch::Xtensa: return "xtensa";
        case Arch::ArmV7M: return "armv7m";
        case Arch::RiscV32: return "riscv32";
    }
    return "unknown";
}

FrameReader::Result FrameReader::feed(std::span<const std::uint8_t> data) {
    buf_.insert(buf_.end(), data.begin(), data.end());
    Result result;
    std::size_t pos = 0;
    auto rest = [&] { return std::span<const std::uint8_t>(buf_).subspan(pos); };
    auto to_text = [&](std::size_t n) {
        result.text.append(reinterpret_cast<const char*>(buf_.data() + pos), n);
        pos += n;
    };

    for (;;) {
        auto r = rest();
        const auto magic = std::search(r.begin(), r.end(), kMagic.begin(), kMagic.end());
        if (magic == r.end()) {
            // keep the longest tail that could still become a magic
            std::size_t keep = 0;
            for (std::size_t k = std::min<std::size_t>(3, r.size()); k > 0 && !keep; --k)
                if (std::equal(r.end() - k, r.end(), kMagic.begin())) keep = k;
            to_text(r.size() - keep);
            break;
        }
        to_text(static_cast<std::size_t>(magic - r.begin()));
        r = rest();
        if (r.size() < 6) break;
        const std::size_t len = r[4] | (r[5] << 8);
        if (len < kFixedBody || len > kMaxBody) { to_text(1); continue; } // false magic
        const std::size_t total = 6 + len + 4;
        if (r.size() < total) break;
        const std::uint32_t crc = r[6 + len] | (r[7 + len] << 8) | (r[8 + len] << 16) | (std::uint32_t{r[9 + len]} << 24);
        if (malgrind::crc32(r.data() + 4, len + 2) != crc) { to_text(1); continue; } // corrupted: resync
        if (auto frame = decode(r.subspan(6, len))) {
            result.frames.push_back(std::move(*frame));
            pos += total;
        } else {
            to_text(1);
        }
    }
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos));
    return result;
}

} // namespace malgrind::host
