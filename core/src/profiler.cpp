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
    void u8(std::uint32_t v) noexcept { *p++ = static_cast<std::uint8_t>(v); }
    void u16(std::uint32_t v) noexcept { u8(v); u8(v >> 8); }
    void u32(std::uint32_t v) noexcept { u16(v); u16(v >> 16); }
    void fixed_name(const char (&name)[kTaskNameLen]) noexcept {
        bool nul = false;
        for (std::size_t i = 0; i < kTaskNameLen; ++i) {
            if (!nul && name[i] == '\0') nul = true;
            u8(nul ? 0u : static_cast<std::uint8_t>(name[i]));
        }
    }
};

std::uint32_t clamp16(std::uint32_t v) noexcept { return v > 0xFFFFu ? 0xFFFFu : v; }

Out begin_v2_header(std::uint8_t* dst, std::uint8_t kind) noexcept {
    Out o{dst};
    o.u8('M'); o.u8('L'); o.u8('C'); o.u8('1');
    o.u16(0); // patched by finalize_frame
    o.u8(kWireVersion);
    o.u8(kind);
    return o;
}

std::size_t finalize_frame(std::uint8_t* dst, std::uint8_t* body_end) noexcept {
    std::uint8_t* const len_at = dst + 4;
    const std::size_t len = static_cast<std::size_t>(body_end - (dst + 6));
    len_at[0] = static_cast<std::uint8_t>(len);
    len_at[1] = static_cast<std::uint8_t>(len >> 8);
    Out o{body_end};
    o.u32(crc32(len_at, len + 2));
    return static_cast<std::size_t>(o.p - dst);
}

template <class Fn>
std::size_t encode_v2_frame(std::uint8_t* dst, std::size_t cap, std::uint8_t kind,
                            std::size_t payload_len, Fn&& write_payload) noexcept {
    if (!dst || payload_len > 0xFFFFu - 2u) return 0;
    const std::size_t total = 4u + 2u + 2u + payload_len + 4u;
    if (cap < total) return 0;
    Out o = begin_v2_header(dst, kind);
    write_payload(o);
    return finalize_frame(dst, o.p);
}

} // namespace

std::size_t encode_hello(std::uint8_t* dst, std::size_t cap, const HelloPayload& hello) noexcept {
    constexpr std::size_t kPayloadLen = 2 + 1 + 1 + 4 + 2 + 4 + 1 + 2 + 4;
    return encode_v2_frame(dst, cap, kKindHello, kPayloadLen, [&](Out& o) noexcept {
        o.u16(hello.seq);
        o.u8(static_cast<std::uint8_t>(hello.arch));
        o.u8(hello.ncores);
        o.u32(hello.cpu_hz);
        o.u16(hello.agent_ver);
        o.u32(hello.caps);
        o.u8(hello.state);
        o.u16(hello.rate_hz);
        o.u32(hello.analyses);
    });
}

std::size_t encode_heap(std::uint8_t* dst, std::size_t cap, std::uint16_t seq, std::uint32_t t_ms,
                        const HeapRegion* regions, std::size_t n) noexcept {
    if (n > 0xFFu || (n > 0 && !regions)) return 0;
    const std::size_t payload_len = 2u + 4u + 1u + n * 17u;
    return encode_v2_frame(dst, cap, kKindHeap, payload_len, [&](Out& o) noexcept {
        o.u16(seq);
        o.u32(t_ms);
        o.u8(static_cast<std::uint8_t>(n));
        for (std::size_t i = 0; i < n; ++i) {
            o.u8(regions[i].region);
            o.u32(regions[i].total);
            o.u32(regions[i].free);
            o.u32(regions[i].min_free);
            o.u32(regions[i].largest);
        }
    });
}

std::size_t encode_tasks(std::uint8_t* dst, std::size_t cap, std::uint16_t seq, std::uint32_t t_ms,
                         std::uint32_t total_runtime, const TaskSample* tasks, std::size_t n) noexcept {
    if (n > 0xFFu || (n > 0 && !tasks)) return 0;
    const std::size_t payload_len = 2u + 4u + 4u + 1u + n * (4u + kTaskNameLen + 1u + 1u + 1u + 4u + 4u);
    return encode_v2_frame(dst, cap, kKindTasks, payload_len, [&](Out& o) noexcept {
        o.u16(seq);
        o.u32(t_ms);
        o.u32(total_runtime);
        o.u8(static_cast<std::uint8_t>(n));
        for (std::size_t i = 0; i < n; ++i) {
            o.u32(tasks[i].handle);
            o.fixed_name(tasks[i].name);
            o.u8(tasks[i].prio);
            o.u8(tasks[i].core);
            o.u8(tasks[i].state);
            o.u32(tasks[i].runtime);
            o.u32(tasks[i].stack_hwm);
        }
    });
}

std::size_t encode_allocs(std::uint8_t* dst, std::size_t cap, std::uint16_t seq,
                          const AllocEvent* events, std::size_t n) noexcept {
    if (n > 0xFFFFu || (n > 0 && !events)) return 0;
    std::size_t payload_len = 4u; // seq + n
    for (std::size_t i = 0; i < n; ++i) {
        if (events[i].depth > kMaxDepth) return 0;
        const std::size_t rec_len = 14u + 4u * events[i].depth;
        if (rec_len > (0xFFFFu - 2u) - payload_len) return 0;
        payload_len += rec_len;
    }
    return encode_v2_frame(dst, cap, kKindAllocs, payload_len, [&](Out& o) noexcept {
        o.u16(seq);
        o.u16(static_cast<std::uint16_t>(n));
        for (std::size_t i = 0; i < n; ++i) {
            o.u8(events[i].op);
            o.u32(events[i].t_ms);
            o.u32(events[i].addr);
            o.u32(events[i].size);
            o.u8(events[i].depth);
            for (unsigned d = 0; d < events[i].depth; ++d) o.u32(events[i].pcs[d]);
        }
    });
}

std::size_t encode_error(std::uint8_t* dst, std::size_t cap, const ErrorEvent& err) noexcept {
    if (err.depth > kMaxDepth) return 0;
    const std::size_t payload_len = 2u + 4u + 1u + 4u + kTaskNameLen + 1u + 4u * err.depth;
    return encode_v2_frame(dst, cap, kKindError, payload_len, [&](Out& o) noexcept {
        o.u16(err.seq);
        o.u32(err.t_ms);
        o.u8(err.code);
        o.u32(err.arg);
        o.fixed_name(err.name);
        o.u8(err.depth);
        for (unsigned d = 0; d < err.depth; ++d) o.u32(err.pcs[d]);
    });
}

std::size_t Profiler::encode(unsigned core, const StackTable& t, std::uint32_t flush_cycles) noexcept {
    Out o = begin_v2_header(buf_, kKindSamples);
    const std::uint32_t hits = t.samples + t.dropped;
    o.u16(seq_[core]++);
    o.u8(static_cast<std::uint8_t>(info_.arch));
    o.u8(info_.num_cores);
    o.u8(core);
    o.u32(info_.cpu_hz);
    o.u16(rate_hz_.load(std::memory_order_relaxed));
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
    return finalize_frame(buf_, o.p);
}

} // namespace malgrind
