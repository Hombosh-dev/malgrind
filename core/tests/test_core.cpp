// Self-check for the core: stack merging, table overflow, frame encoding, CRC, command parsing, event queue.
#include "malgrind/command.hpp"
#include "malgrind/events.hpp"
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

    // CommandParser: framing, chunking, resync on false/overlapping MLC1, bad header/CRC, back-to-back
    {
        auto make_cmd_frame = [](std::uint8_t cmd, std::uint16_t rate_hz, std::uint32_t analyses) {
            std::vector<std::uint8_t> f{'M', 'L', 'C', '1', 9, 0, kWireVersion, kKindCommand, cmd};
            f.push_back(static_cast<std::uint8_t>(rate_hz));
            f.push_back(static_cast<std::uint8_t>(rate_hz >> 8));
            for (int i = 0; i < 4; ++i) f.push_back(static_cast<std::uint8_t>(analyses >> (8 * i)));
            const std::uint32_t crc = crc32(f.data() + 4, 2 + 9);
            for (int i = 0; i < 4; ++i) f.push_back(static_cast<std::uint8_t>(crc >> (8 * i)));
            assert(f.size() == CommandParser::kFrameSize);
            return f;
        };

        auto make_raw_frame = [](std::uint16_t len_field, std::uint8_t ver, std::uint8_t kind,
                                 const std::vector<std::uint8_t>& tail) {
            std::vector<std::uint8_t> f{'M', 'L', 'C', '1',
                                        static_cast<std::uint8_t>(len_field),
                                        static_cast<std::uint8_t>(len_field >> 8),
                                        ver, kind};
            f.insert(f.end(), tail.begin(), tail.end());
            const std::uint32_t crc = crc32(f.data() + 4, f.size() - 4);
            for (int i = 0; i < 4; ++i) f.push_back(static_cast<std::uint8_t>(crc >> (8 * i)));
            return f;
        };

        const auto f_start = make_cmd_frame(kCmdStart, 997, kCapSamples | kCapHeap);
        const auto f_stop = make_cmd_frame(kCmdStop, 0, 0);
        const auto f_hello = make_cmd_frame(kCmdHello, 500, kCapSamples | kCapTasks | kCapErrors);

        // Byte-by-byte feeding
        {
            CommandParser parser;
            Command out{};
            for (std::size_t i = 0; i + 1 < f_start.size(); ++i) {
                assert(!parser.feed(f_start[i], out));
            }
            assert(parser.feed(f_start.back(), out));
            assert(out.cmd == kCmdStart && out.rate_hz == 997 && out.analyses == (kCapSamples | kCapHeap));
        }

        // Chunked feeding + nullptr/zero-size guards + back-to-back commands
        {
            std::vector<std::uint8_t> stream;
            stream.insert(stream.end(), f_start.begin(), f_start.end());
            stream.insert(stream.end(), f_stop.begin(), f_stop.end());
            stream.insert(stream.end(), f_hello.begin(), f_hello.end());

            for (std::size_t chunk : {std::size_t{1}, std::size_t{3}, std::size_t{5}, std::size_t{7}, stream.size()}) {
                CommandParser parser;
                parser.feed(nullptr, 5, [](const Command&) { assert(false); });
                parser.feed(stream.data(), 0, [](const Command&) { assert(false); });

                std::vector<Command> got;
                for (std::size_t i = 0; i < stream.size(); i += chunk) {
                    const std::size_t n = (i + chunk <= stream.size()) ? chunk : (stream.size() - i);
                    parser.feed(stream.data() + i, n, [&](const Command& c) { got.push_back(c); });
                }
                assert(got.size() == 3);
                assert(got[0].cmd == kCmdStart && got[0].rate_hz == 997 && got[0].analyses == (kCapSamples | kCapHeap));
                assert(got[1].cmd == kCmdStop && got[1].rate_hz == 0 && got[1].analyses == 0);
                assert(got[2].cmd == kCmdHello && got[2].rate_hz == 500 && got[2].analyses == (kCapSamples | kCapTasks | kCapErrors));
            }
        }

        // Garbage prefix, partial/false "MLC1", overlapping "MLC1", wrong len/ver/kind, bad CRC resync
        {
            std::vector<std::uint8_t> stream;
            const char noise[] = "boot log\nM ML MLC MLCM MLCMLC1";
            stream.insert(stream.end(), noise, noise + sizeof(noise) - 1);
            // The trailing "MLC1" above is followed immediately by f_start ("MLC1..."):
            // false "MLC1" followed by overlapping real "MLC1" in len bytes!
            stream.insert(stream.end(), f_start.begin(), f_start.end());

            // False "MLC1" + "ML" then real "C1..." (overlap inside len)
            const std::uint8_t false_ml[] = {'M', 'L', 'C', '1', 'M', 'L'};
            stream.insert(stream.end(), false_ml, false_ml + sizeof(false_ml));
            stream.insert(stream.end(), f_stop.begin() + 2, f_stop.end());

            // Wrong len != 9 (len=8, len=0x0109) with valid CRC, followed by f_hello
            const auto bad_len1 = make_raw_frame(8, kWireVersion, kKindCommand, {1, 2, 3, 4, 5, 6});
            const auto bad_len2 = make_raw_frame(0x0109, kWireVersion, kKindCommand, {1, 2, 3, 4, 5, 6, 7});
            stream.insert(stream.end(), bad_len1.begin(), bad_len1.end());
            stream.insert(stream.end(), bad_len2.begin(), bad_len2.end());
            stream.insert(stream.end(), f_hello.begin(), f_hello.end());

            // Wrong ver != kWireVersion (ver=1, ver=3) and wrong kind != kKindCommand (kind=kKindHello, kind='M')
            const auto bad_ver1 = make_raw_frame(9, 1, kKindCommand, {1, 2, 3, 4, 5, 6, 7});
            const auto bad_ver2 = make_raw_frame(9, 3, kKindCommand, {1, 2, 3, 4, 5, 6, 7});
            const auto bad_kind1 = make_raw_frame(9, kWireVersion, kKindHello, {1, 2, 3, 4, 5, 6, 7});
            const auto bad_kind2 = make_raw_frame(9, kWireVersion, static_cast<std::uint8_t>('M'), {1, 2, 3, 4, 5, 6, 7});
            stream.insert(stream.end(), bad_ver1.begin(), bad_ver1.end());
            stream.insert(stream.end(), bad_ver2.begin(), bad_ver2.end());
            stream.insert(stream.end(), bad_kind1.begin(), bad_kind1.end());
            stream.insert(stream.end(), bad_kind2.begin(), bad_kind2.end());
            stream.insert(stream.end(), f_start.begin(), f_start.end());

            // Bad CRC frame whose payload+CRC starts a real valid frame at byte 8 (embedded "MLC1" inside bad frame!)
            {
                const std::uint8_t outer_hdr[] = {'M', 'L', 'C', '1', 9, 0, kWireVersion, kKindCommand};
                stream.insert(stream.end(), outer_hdr, outer_hdr + sizeof(outer_hdr));
                stream.insert(stream.end(), f_stop.begin(), f_stop.end());
            }

            // Bad CRC frame with an embedded false "MLC1" at byte 8 (wrong len) AND a real "MLC1" at byte 15
            {
                const std::uint8_t outer_with_two[] = {
                    'M', 'L', 'C', '1', 9, 0, kWireVersion, kKindCommand,
                    'M', 'L', 'C', '1', 0, 0, 0,
                    'M', 'L', 'C', '1',
                };
                stream.insert(stream.end(), outer_with_two, outer_with_two + sizeof(outer_with_two));
                stream.insert(stream.end(), f_hello.begin() + 4, f_hello.end());
            }

            // Corrupted CRC on a regular frame followed by f_start
            {
                auto bad_crc = f_stop;
                bad_crc.back() ^= 0x01;
                stream.insert(stream.end(), bad_crc.begin(), bad_crc.end());
                stream.insert(stream.end(), f_start.begin(), f_start.end());
            }

            CommandParser parser;
            std::vector<Command> got;
            parser.feed(stream.data(), stream.size(), [&](const Command& c) { got.push_back(c); });
            assert(got.size() == 7);
            assert(got[0].cmd == kCmdStart && got[0].rate_hz == 997);
            assert(got[1].cmd == kCmdStop && got[1].rate_hz == 0);
            assert(got[2].cmd == kCmdHello && got[2].rate_hz == 500);
            assert(got[3].cmd == kCmdStart && got[3].rate_hz == 997);
            assert(got[4].cmd == kCmdStop && got[4].rate_hz == 0);
            assert(got[5].cmd == kCmdHello && got[5].rate_hz == 500);
            assert(got[6].cmd == kCmdStart && got[6].rate_hz == 997);
        }

        // reset() discards partial buffered bytes
        {
            CommandParser parser;
            Command out{};
            for (std::size_t i = 0; i < 10; ++i) assert(!parser.feed(f_start[i], out));
            parser.reset();
            for (std::size_t i = 10; i < f_start.size(); ++i) assert(!parser.feed(f_start[i], out));
            for (std::size_t i = 0; i + 1 < f_stop.size(); ++i) assert(!parser.feed(f_stop[i], out));
            assert(parser.feed(f_stop.back(), out));
            assert(out.cmd == kCmdStop && out.rate_hz == 0 && out.analyses == 0);
        }
    }

    // EventQueue: FIFO wraparound, overflow counting, non-destructive drain, clear
    {
        static_assert(kErrEventsDropped == 4);

        // Default-sized queue sanity check
        {
            EventQueue<> def_q;
            assert(def_q.alloc_count() == 0 && def_q.error_count() == 0);
            assert(def_q.dropped_allocs() == 0 && def_q.dropped_errors() == 0);
            assert(def_q.take_dropped() == 0);
        }

        auto make_alloc = [](std::uint32_t id) {
            AllocEvent ev{};
            ev.op = static_cast<std::uint8_t>(id & 1u);
            ev.t_ms = 1000u + id;
            ev.addr = 0x3f800000u + id * 16u;
            ev.size = ev.op == 0 ? (64u + id) : 0u;
            ev.depth = 2;
            ev.pcs[0] = 0x400d0000u + id;
            ev.pcs[1] = 0x400d1000u + id;
            return ev;
        };

        auto make_err = [](std::uint32_t id) {
            ErrorEvent ev{};
            ev.seq = 99;
            ev.t_ms = 5000u + id;
            ev.code = kErrEventsDropped;
            ev.arg = id;
            std::memcpy(ev.name, "task", 5);
            ev.depth = 1;
            ev.pcs[0] = 0x400e0000u + id;
            return ev;
        };

        // Ring wraparound + overflow for both allocs and errors
        {
            EventQueue<4, 3> q;

            assert(q.push_alloc(make_alloc(1)));
            assert(q.push_alloc(make_alloc(2)));
            assert(q.push_alloc(make_alloc(3)));
            assert(q.alloc_count() == 3);

            AllocEvent popped[4]{};
            assert(q.pop_allocs(nullptr, 2) == 0);
            assert(q.pop_allocs(popped, 0) == 0);
            assert(q.alloc_count() == 3);

            assert(q.pop_allocs(popped, 2) == 2);
            assert(popped[0].t_ms == 1001 && popped[1].t_ms == 1002);
            assert(q.alloc_count() == 1);

            // Push 3 more to wrap tail/head around capacity 4, then overflow by 2
            assert(q.push_alloc(make_alloc(4)));
            assert(q.push_alloc(make_alloc(5)));
            assert(q.push_alloc(make_alloc(6)));
            assert(q.alloc_count() == 4);
            assert(!q.push_alloc(make_alloc(7)));
            assert(!q.push_alloc(make_alloc(8)));
            assert(q.alloc_count() == 4);
            assert(q.dropped_allocs() == 2);

            assert(q.pop_allocs(popped, 4) == 4);
            assert(popped[0].t_ms == 1003 && popped[1].t_ms == 1004 &&
                   popped[2].t_ms == 1005 && popped[3].t_ms == 1006);
            assert(q.alloc_count() == 0);
            assert(q.pop_allocs(popped, 4) == 0);

            // Error ring wraparound + overflow
            assert(q.push_error(make_err(1)));
            assert(q.push_error(make_err(2)));
            ErrorEvent err_out{};
            assert(q.pop_error(err_out) && err_out.arg == 1);
            assert(q.push_error(make_err(3)));
            assert(q.push_error(make_err(4)));
            assert(q.error_count() == 3);
            assert(!q.push_error(make_err(5)));
            assert(q.dropped_errors() == 1);

            assert(q.take_dropped() == 3);
            assert(q.dropped_allocs() == 0 && q.dropped_errors() == 0);
            assert(q.take_dropped() == 0);

            assert(q.pop_error(err_out) && err_out.arg == 2);
            assert(q.pop_error(err_out) && err_out.arg == 3);
            assert(q.pop_error(err_out) && err_out.arg == 4);
            assert(q.error_count() == 0);
            ErrorEvent sentinel{};
            sentinel.arg = 0xCAFEBABEu;
            assert(!q.pop_error(sentinel));
            assert(sentinel.arg == 0xCAFEBABEu);
        }

        // Non-destructive drain_allocs & drain_error across wraparound + encode_* byte parity
        {
            EventQueue<5, 3> q;
            std::vector<std::uint8_t> buf(512);

            // Empty drains return 0
            assert(q.drain_allocs(buf.data(), buf.size(), 1) == 0);
            assert(q.drain_error(buf.data(), buf.size(), 1) == 0);

            // Advance tail so next batch wraps across index 4 -> 0
            for (std::uint32_t i = 0; i < 3; ++i) assert(q.push_alloc(make_alloc(i)));
            AllocEvent discard[3]{};
            assert(q.pop_allocs(discard, 3) == 3);

            const AllocEvent wrapped_allocs[4] = {
                make_alloc(10), make_alloc(11), make_alloc(12), make_alloc(13),
            };
            for (const AllocEvent& ev : wrapped_allocs) assert(q.push_alloc(ev));
            assert(q.alloc_count() == 4);

            // max_batch == 0 is non-destructive
            assert(q.drain_allocs(buf.data(), buf.size(), 7, 0) == 0);
            assert(q.alloc_count() == 4);

            // Compute exact encoded frame for first batch of 3
            std::vector<std::uint8_t> ref_alloc(512);
            const std::size_t expected_alloc_len =
                encode_allocs(ref_alloc.data(), ref_alloc.size(), 42, wrapped_allocs, 3);
            assert(expected_alloc_len > 12);
            ref_alloc.resize(expected_alloc_len);

            // Too-small capacities (nullptr, cap == 4, cap == needed - 1) must preserve all items
            assert(q.drain_allocs(nullptr, expected_alloc_len, 42, 3) == 0);
            assert(q.alloc_count() == 4);
            std::vector<std::uint8_t> small_buf(expected_alloc_len, 0x5A);
            assert(q.drain_allocs(small_buf.data(), 4, 42, 3) == 0);
            assert(q.drain_allocs(small_buf.data(), expected_alloc_len - 1, 42, 3) == 0);
            for (std::uint8_t b : small_buf) assert(b == 0x5A);
            assert(q.alloc_count() == 4);

            // Exact capacity succeeds, matches encode_allocs byte-for-byte, and pops batch of 3
            const std::size_t got_len = q.drain_allocs(buf.data(), expected_alloc_len, 42, 3);
            assert(got_len == expected_alloc_len);
            std::vector<std::uint8_t> drained_frame(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(got_len));
            check_frame_envelope(drained_frame, kKindAllocs);
            assert(drained_frame == ref_alloc);
            assert(q.alloc_count() == 1);

            // Drain remaining 1 alloc event with max_batch > alloc_count()
            const std::size_t rem_expected =
                encode_allocs(ref_alloc.data(), ref_alloc.capacity(), 43, &wrapped_allocs[3], 1);
            const std::size_t rem_got = q.drain_allocs(buf.data(), buf.size(), 43, 32);
            assert(rem_got == rem_expected);
            assert(q.alloc_count() == 0);

            // Error drain: wraparound + small cap preservation + seq override + encode_error parity
            ErrorEvent err0 = make_err(20);
            assert(q.push_error(err0));
            assert(q.push_error(make_err(21)));
            ErrorEvent tmp{};
            assert(q.pop_error(tmp));
            assert(q.push_error(make_err(22)));
            assert(q.error_count() == 2);

            ErrorEvent expected_ev = make_err(21);
            expected_ev.seq = 77;
            std::vector<std::uint8_t> ref_err(128);
            const std::size_t expected_err_len = encode_error(ref_err.data(), ref_err.size(), expected_ev);
            assert(expected_err_len > 12);
            ref_err.resize(expected_err_len);

            assert(q.drain_error(nullptr, expected_err_len, 77) == 0);
            assert(q.drain_error(buf.data(), 4, 77) == 0);
            assert(q.drain_error(buf.data(), expected_err_len - 1, 77) == 0);
            assert(q.error_count() == 2);

            const std::size_t err_got = q.drain_error(buf.data(), expected_err_len, 77);
            assert(err_got == expected_err_len);
            std::vector<std::uint8_t> err_frame(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(err_got));
            check_frame_envelope(err_frame, kKindError);
            assert(err_frame == ref_err);
            assert(q.error_count() == 1);

            assert(q.pop_error(tmp) && tmp.arg == 22);
            assert(q.error_count() == 0);
        }

        // clear() resets counts and dropped counters and allows reuse
        {
            EventQueue<2, 2> q;
            assert(q.push_alloc(make_alloc(1)));
            assert(q.push_alloc(make_alloc(2)));
            assert(!q.push_alloc(make_alloc(3)));
            assert(q.push_error(make_err(1)));
            assert(q.push_error(make_err(2)));
            assert(!q.push_error(make_err(3)));
            assert(q.alloc_count() == 2 && q.error_count() == 2);
            assert(q.dropped_allocs() == 1 && q.dropped_errors() == 1);

            q.clear();
            assert(q.alloc_count() == 0 && q.error_count() == 0);
            assert(q.dropped_allocs() == 0 && q.dropped_errors() == 0);
            assert(q.take_dropped() == 0);

            assert(q.push_alloc(make_alloc(9)));
            assert(q.push_error(make_err(9)));
            assert(q.alloc_count() == 1 && q.error_count() == 1);
        }
    }

    std::puts("core ok");
}
