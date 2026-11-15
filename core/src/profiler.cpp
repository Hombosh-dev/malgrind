#include "malgrind/profiler.hpp"

namespace malgrind {

std::uint32_t crc32(const std::uint8_t* data, std::size_t n) noexcept {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

namespace {
struct Out {
    std::uint8_t* p;
    void u8(std::uint32_t v) { *p++ = static_cast<std::uint8_t>(v); }
    void u16(std::uint32_t v) { u8(v); u8(v >> 8); }
    void u32(std::uint32_t v) { u16(v); u16(v >> 16); }
};
std::uint32_t clamp16(std::uint32_t v) { return v > 0xFFFFu ? 0xFFFFu : v; }
} // namespace

std::size_t Profiler::encode(unsigned core, const StackTable& t, std::uint32_t flush_cycles) noexcept {
    Out o{buf_};
    o.u8('M'); o.u8('L'); o.u8('C'); o.u8('1');
    std::uint8_t* const len_at = o.p;
    o.u16(0); // patched below
    std::uint8_t* const body = o.p;
    const std::uint32_t hits = t.samples + t.dropped;
    o.u8(1); // ver
    o.u16(seq_[core]++);
    o.u8(static_cast<std::uint8_t>(info_.arch));
    o.u8(info_.num_cores);
    o.u8(core);
    o.u32(info_.cpu_hz);
    o.u16(rate_hz_);
    o.u16(clamp16(t.samples));
    o.u16(clamp16(t.dropped));
    o.u32(hits ? static_cast<std::uint32_t>(t.isr_sum / hits) : 0);
    o.u32(t.isr_max);
    o.u32(flush_cycles);
    std::uint8_t* const n_at = o.p;
    o.u16(0); // patched below
    std::uint32_t n = 0;
    for (const Entry& e : t.entries) {
        if (e.count == 0) continue;
        o.u16(clamp16(e.count));
        o.u8(e.depth);
        for (unsigned i = 0; i < e.depth; ++i) o.u32(e.pcs[i]);
        ++n;
    }
    n_at[0] = static_cast<std::uint8_t>(n);
    n_at[1] = static_cast<std::uint8_t>(n >> 8);
    const std::size_t len = static_cast<std::size_t>(o.p - body);
    len_at[0] = static_cast<std::uint8_t>(len);
    len_at[1] = static_cast<std::uint8_t>(len >> 8);
    o.u32(crc32(len_at, len + 2));
    return static_cast<std::size_t>(o.p - buf_);
}

} // namespace malgrind
