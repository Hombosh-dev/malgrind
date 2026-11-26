// Self-check for the core: stack merging, table overflow, frame encoding, CRC.
#include "malgrind/profiler.hpp"

#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace malgrind;

static std::uint32_t rd(const std::uint8_t* p, int n) {
    std::uint32_t v = 0;
    for (int i = n - 1; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

static void check_frame_envelope(const std::vector<std::uint8_t>& frame, std::uint8_t expected_kind) {
    assert(frame.size() >= 4 + 2 + 2 + 4);
    assert(std::memcmp(frame.data(), "MLC1", 4) == 0);
    const std::uint32_t len = rd(&frame[4], 2);
    assert(frame.size() == 4 + 2 + len + 4);
    assert(rd(&frame[frame.size() - 4], 4) == crc32(&frame[4], len + 2));
    assert(frame[6] == kWireVersion && frame[7] == expected_kind);
}

int main() {
    // identical stacks merge, different stacks don't; ISR cost is averaged
    StackTable t{};
    const std::uint32_t a[] = {0x400, 0x500, 0x600}, b[] = {0x400, 0x501, 0x600};
    t.record(a, 3, 100);
    t.record(a, 3, 300);
    t.record(b, 3, 200);
    unsigned used = 0;
    for (const Entry& e : t.entries) used += e.count != 0;
    assert(used == 2 && t.samples == 3 && t.dropped == 0 && t.isr_max == 300 && t.isr_sum == 600);

    // far more distinct stacks than slots: the table fills and the rest is counted as dropped
    StackTable full{};
    for (std::uint32_t i = 0; i < 4 * kTableSize; ++i) full.record(&i, 1, 1);
    assert(full.samples <= kTableSize && full.dropped > 0 && full.samples + full.dropped == 4 * kTableSize);

    // encode a flush through Profiler and check the v2 SAMPLES frame layout + CRC
    Profiler p({"test", Arch::Host, 1'000'000, 1}, 997);
    assert(p.rate_hz() == 997);
    p.record(0, a, 3, 50);
    p.record(0, a, 3, 70);
    std::vector<std::uint8_t> frame;
    p.flush([&](const std::uint8_t* d, std::size_t n) { frame.assign(d, d + n); }, [] {}, 1234);
    check_frame_envelope(frame, kKindSamples);
    const std::uint32_t len = rd(&frame[4], 2);
    const std::uint8_t* body = &frame[6];
    assert(body[0] == 2 && body[1] == kKindSamples);
    assert(rd(body + 2, 2) == 0 /*seq*/ && body[4] == static_cast<std::uint8_t>(Arch::Host));
    assert(body[5] == 1 /*ncores*/ && body[6] == 0 /*core*/);
    assert(rd(body + 7, 4) == 1'000'000 && rd(body + 11, 2) == 997);
    assert(rd(body + 13, 2) == 2 && rd(body + 15, 2) == 0); // samples, dropped
    assert(rd(body + 17, 4) == 60 && rd(body + 21, 4) == 70 && rd(body + 25, 4) == 1234);
    assert(rd(body + 29, 2) == 1);                          // one merged entry
    assert(rd(body + 31, 2) == 2 && body[33] == 3 && rd(body + 34, 4) == 0x400);

    // a flipped byte must break the CRC
    frame[20] ^= 0x01;
    assert(rd(&frame[frame.size() - 4], 4) != crc32(&frame[4], len + 2));

    // the table was cleared by the flush: second frame is empty; set_rate updates encoded rate_hz
    p.set_rate(500);
    assert(p.rate_hz() == 500);
    p.flush([&](const std::uint8_t* d, std::size_t n) { frame.assign(d, d + n); }, [] {}, 0);
    check_frame_envelope(frame, kKindSamples);
    assert(rd(&frame[6 + 2], 2) == 1 /*seq*/ && rd(&frame[6 + 11], 2) == 500);
    assert(rd(&frame[6 + 29], 2) == 0 /*n_entries*/);

    // clear() wipes all tables before flush
    p.record(0, a, 3, 42);
    p.clear();
    p.flush([&](const std::uint8_t* d, std::size_t n) { frame.assign(d, d + n); }, [] {}, 0);
    assert(rd(&frame[6 + 13], 2) == 0 && rd(&frame[6 + 29], 2) == 0);

    // worst-case full table at max depth still fits inside kMaxFrameSize
    {
        Profiler max_p({"max", Arch::Xtensa, 240'000'000, 1}, 1000);
        std::uint32_t stack[kMaxDepth];
        for (unsigned d = 0; d < kMaxDepth; ++d) stack[d] = 0x40000000u + d * 4u;
        for (std::uint32_t i = 0; i < 8 * kTableSize; ++i) {
            stack[0] = 0x40000000u + i;
            max_p.record(0, stack, kMaxDepth, 10);
        }
        max_p.flush([&](const std::uint8_t* d, std::size_t n) { frame.assign(d, d + n); }, [] {}, 1);
        assert(frame.size() <= kMaxFrameSize);
        check_frame_envelope(frame, kKindSamples);
    }

    // verify buffer-bounds enforcement: nullptr -> 0, cap == needed - 1 -> 0 without write, cap == needed -> exact
    auto verify_bounds = [&](std::size_t expected_len, auto&& call_encode) {
        assert(call_encode(nullptr, expected_len) == 0);
        std::vector<std::uint8_t> buf(expected_len + 16, 0xA5);
        assert(call_encode(buf.data(), expected_len - 1) == 0);
        for (std::uint8_t b : buf) assert(b == 0xA5);
        const std::size_t written = call_encode(buf.data(), expected_len);
        assert(written == expected_len);
        for (std::size_t i = expected_len; i < buf.size(); ++i) assert(buf[i] == 0xA5);
        return std::vector<std::uint8_t>(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(written));
    };

    // HELLO
    {
        const HelloPayload hello{
            .seq = 7,
            .arch = Arch::Xtensa,
            .ncores = 2,
            .cpu_hz = 240'000'000,
            .agent_ver = 2,
            .caps = kCapSamples | kCapHeap | kCapTasks,
            .state = 1,
            .rate_hz = 997,
            .analyses = kCapSamples | kCapHeap,
        };
        const auto out = verify_bounds(33, [&](std::uint8_t* dst, std::size_t cap) {
            return encode_hello(dst, cap, hello);
        });
        check_frame_envelope(out, kKindHello);
        const std::uint8_t* pl = &out[8];
        assert(rd(pl + 0, 2) == 7 && pl[2] == static_cast<std::uint8_t>(Arch::Xtensa) && pl[3] == 2);
        assert(rd(pl + 4, 4) == 240'000'000 && rd(pl + 8, 2) == 2);
        assert(rd(pl + 10, 4) == (kCapSamples | kCapHeap | kCapTasks) && pl[14] == 1);
        assert(rd(pl + 15, 2) == 997 && rd(pl + 17, 4) == (kCapSamples | kCapHeap));
    }

    // HEAP
    {
        const HeapRegion regions[] = {
            {kHeapInternal, 300'000, 120'000, 90'000, 65'536},
            {kHeapExternal, 4'194'304, 3'000'000, 2'500'000, 2'000'000},
        };
        const auto out = verify_bounds(12 + 7 + 2 * 17, [&](std::uint8_t* dst, std::size_t cap) {
            return encode_heap(dst, cap, 11, 5500, regions, 2);
        });
        check_frame_envelope(out, kKindHeap);
        const std::uint8_t* pl = &out[8];
        assert(rd(pl + 0, 2) == 11 && rd(pl + 2, 4) == 5500 && pl[6] == 2);
        assert(pl[7] == kHeapInternal && rd(pl + 8, 4) == 300'000 && rd(pl + 12, 4) == 120'000);
        assert(pl[24] == kHeapExternal && rd(pl + 25, 4) == 4'194'304 && rd(pl + 37, 4) == 2'000'000);

        std::array<std::uint8_t, 32> tiny{};
        assert(encode_heap(tiny.data(), tiny.size(), 1, 0, nullptr, 1) == 0);
        assert(encode_heap(tiny.data(), tiny.size(), 1, 0, nullptr, 0) == 12 + 7);
    }

    // TASKS
    {
        TaskSample tasks[2]{};
        tasks[0].handle = 0x3ffb1000;
        std::memcpy(tasks[0].name, "loopTask", 9);
        tasks[0].prio = 1;
        tasks[0].core = 1;
        tasks[0].state = 0;
        tasks[0].runtime = 42'000;
        tasks[0].stack_hwm = 1024;
        tasks[1].handle = 0x3ffb2000;
        std::memcpy(tasks[1].name, "0123456789abcdef", 16);
        tasks[1].prio = 5;
        tasks[1].core = 0xff;
        tasks[1].state = 2;
        tasks[1].runtime = 1234;
        tasks[1].stack_hwm = 256;

        const auto out = verify_bounds(12 + 11 + 2 * 31, [&](std::uint8_t* dst, std::size_t cap) {
            return encode_tasks(dst, cap, 12, 6000, 100'000, tasks, 2);
        });
        check_frame_envelope(out, kKindTasks);
        const std::uint8_t* pl = &out[8];
        assert(rd(pl + 0, 2) == 12 && rd(pl + 2, 4) == 6000 && rd(pl + 6, 4) == 100'000 && pl[10] == 2);
        assert(rd(pl + 11, 4) == 0x3ffb1000 && std::memcmp(pl + 15, "loopTask\0\0\0\0\0\0\0\0", 16) == 0);
        assert(pl[31] == 1 && pl[32] == 1 && pl[33] == 0 && rd(pl + 34, 4) == 42'000 && rd(pl + 38, 4) == 1024);
        assert(std::memcmp(pl + 46, "0123456789abcdef", 16) == 0 && pl[63] == 0xff && rd(pl + 69, 4) == 256);

        std::array<std::uint8_t, 32> tiny{};
        assert(encode_tasks(tiny.data(), tiny.size(), 1, 0, 0, nullptr, 1) == 0);
        assert(encode_tasks(tiny.data(), tiny.size(), 1, 0, 0, nullptr, 0) == 12 + 11);
    }

    // ALLOCS
    {
        AllocEvent events[2]{};
        events[0] = {.op = 0, .t_ms = 100, .addr = 0x3f801000, .size = 512, .depth = 2, .pcs = {0x400d1000, 0x400d2000}};
        events[1] = {.op = 1, .t_ms = 150, .addr = 0x3f801000, .size = 0, .depth = 1, .pcs = {0x400d3000}};
        const std::size_t expected = 12 + 4 + (14 + 8) + (14 + 4);
        const auto out = verify_bounds(expected, [&](std::uint8_t* dst, std::size_t cap) {
            return encode_allocs(dst, cap, 13, events, 2);
        });
        check_frame_envelope(out, kKindAllocs);
        const std::uint8_t* pl = &out[8];
        assert(rd(pl + 0, 2) == 13 && rd(pl + 2, 2) == 2);
        assert(pl[4] == 0 && rd(pl + 5, 4) == 100 && rd(pl + 9, 4) == 0x3f801000 && rd(pl + 13, 4) == 512 && pl[17] == 2);
        assert(rd(pl + 18, 4) == 0x400d1000 && rd(pl + 22, 4) == 0x400d2000);
        assert(pl[26] == 1 && rd(pl + 27, 4) == 150 && rd(pl + 35, 4) == 0 && pl[39] == 1 && rd(pl + 40, 4) == 0x400d3000);

        std::array<std::uint8_t, 128> buf{};
        assert(encode_allocs(buf.data(), buf.size(), 1, nullptr, 1) == 0);
        assert(encode_allocs(buf.data(), buf.size(), 1, nullptr, 0) == 12 + 4);
        AllocEvent bad = events[0];
        bad.depth = kMaxDepth + 1;
        assert(encode_allocs(buf.data(), buf.size(), 1, &bad, 1) == 0);
    }

    // ERROR
    {
        ErrorEvent err{
            .seq = 14,
            .t_ms = 9000,
            .code = kErrStackOverflow,
            .arg = 0x3ffb4000,
            .name = {},
            .depth = 2,
            .pcs = {0x400d4000, 0x400d5000},
        };
        std::memcpy(err.name, "worker\0GARBAGE", 14);
        const std::size_t expected = 12 + 28 + 8;
        const auto out = verify_bounds(expected, [&](std::uint8_t* dst, std::size_t cap) {
            return encode_error(dst, cap, err);
        });
        check_frame_envelope(out, kKindError);
        const std::uint8_t* pl = &out[8];
        assert(rd(pl + 0, 2) == 14 && rd(pl + 2, 4) == 9000 && pl[6] == kErrStackOverflow && rd(pl + 7, 4) == 0x3ffb4000);
        assert(std::memcmp(pl + 11, "worker\0\0\0\0\0\0\0\0\0\0", 16) == 0); // tail after NUL zeroed
        assert(pl[27] == 2 && rd(pl + 28, 4) == 0x400d4000 && rd(pl + 32, 4) == 0x400d5000);

        std::array<std::uint8_t, 128> buf{};
        err.depth = kMaxDepth + 1;
        assert(encode_error(buf.data(), buf.size(), err) == 0);
    }

    std::puts("core ok");
}
