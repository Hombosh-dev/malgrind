#include "malgrind/events.hpp"

namespace malgrind::detail {

namespace {

struct Out {
    std::uint8_t* p;
    void u8(std::uint32_t v) noexcept { *p++ = static_cast<std::uint8_t>(v); }
    void u16(std::uint32_t v) noexcept { u8(v); u8(v >> 8); }
    void u32(std::uint32_t v) noexcept { u16(v); u16(v >> 16); }
};

template <class T>
bool push_item(T* ring, std::size_t cap, RingCursor& c, std::uint32_t& dropped,
               const T& item) noexcept {
    if (c.count >= cap) {
        if (dropped < 0xFFFFFFFFu) ++dropped;
        return false;
    }
    ring[c.head] = item;
    c.head = (c.head + 1 == cap) ? 0 : (c.head + 1);
    ++c.count;
    return true;
}

template <class T>
bool pop_one(const T* ring, std::size_t cap, RingCursor& c, T& out) noexcept {
    if (c.count == 0) return false;
    out = ring[c.tail];
    c.tail = (c.tail + 1 == cap) ? 0 : (c.tail + 1);
    --c.count;
    return true;
}

} // namespace

bool push_alloc_impl(AllocEvent* ring, std::size_t ring_cap, RingCursor& cursor,
                     std::uint32_t& dropped, const AllocEvent& ev) noexcept {
    return push_item(ring, ring_cap, cursor, dropped, ev);
}

std::size_t pop_allocs_impl(const AllocEvent* ring, std::size_t ring_cap, RingCursor& cursor,
                            AllocEvent* out, std::size_t max_n) noexcept {
    if (!out || max_n == 0) return 0;
    std::size_t n = 0;
    while (n < max_n && pop_one(ring, ring_cap, cursor, out[n])) {
        ++n;
    }
    return n;
}

bool push_error_impl(ErrorEvent* ring, std::size_t ring_cap, RingCursor& cursor,
                     std::uint32_t& dropped, const ErrorEvent& ev) noexcept {
    return push_item(ring, ring_cap, cursor, dropped, ev);
}

bool pop_error_impl(const ErrorEvent* ring, std::size_t ring_cap, RingCursor& cursor,
                    ErrorEvent& out) noexcept {
    return pop_one(ring, ring_cap, cursor, out);
}

std::size_t drain_allocs_impl(const AllocEvent* ring, std::size_t ring_cap, RingCursor& cursor,
                              std::uint8_t* dst, std::size_t cap, std::uint16_t seq,
                              std::size_t max_batch) noexcept {
    if (cursor.count == 0 || max_batch == 0) return 0;
    const std::size_t n = (cursor.count < max_batch) ? cursor.count : max_batch;
    if (n > 0xFFFFu) return 0;

    std::size_t payload_len = 4u; // seq + n
    std::size_t idx = cursor.tail;
    for (std::size_t i = 0; i < n; ++i) {
        const AllocEvent& ev = ring[idx];
        if (ev.depth > kMaxDepth) return 0;
        const std::size_t rec_len = 14u + 4u * ev.depth;
        if (rec_len > (0xFFFFu - 2u) - payload_len) return 0;
        payload_len += rec_len;
        idx = (idx + 1 == ring_cap) ? 0 : (idx + 1);
    }

    const std::size_t body_len = 2u + payload_len;
    const std::size_t total = 4u + 2u + body_len + 4u;
    if (!dst || cap < total) return 0;

    Out o{dst};
    o.u8('M'); o.u8('L'); o.u8('C'); o.u8('1');
    o.u16(static_cast<std::uint32_t>(body_len));
    o.u8(kWireVersion);
    o.u8(kKindAllocs);
    o.u16(seq);
    o.u16(static_cast<std::uint32_t>(n));

    idx = cursor.tail;
    for (std::size_t i = 0; i < n; ++i) {
        const AllocEvent& ev = ring[idx];
        o.u8(ev.op);
        o.u32(ev.t_ms);
        o.u32(ev.addr);
        o.u32(ev.size);
        o.u8(ev.depth);
        for (unsigned d = 0; d < ev.depth; ++d) o.u32(ev.pcs[d]);
        idx = (idx + 1 == ring_cap) ? 0 : (idx + 1);
    }

    o.u32(crc32(dst + 4, body_len + 2u));

    cursor.tail = idx;
    cursor.count -= n;
    return total;
}

std::size_t drain_error_impl(const ErrorEvent* ring, std::size_t ring_cap, RingCursor& cursor,
                             std::uint8_t* dst, std::size_t cap, std::uint16_t seq) noexcept {
    if (cursor.count == 0) return 0;
    ErrorEvent ev = ring[cursor.tail];
    ev.seq = seq;
    const std::size_t written = encode_error(dst, cap, ev);
    if (written == 0) return 0;
    cursor.tail = (cursor.tail + 1 == ring_cap) ? 0 : (cursor.tail + 1);
    --cursor.count;
    return written;
}

} // namespace malgrind::detail
