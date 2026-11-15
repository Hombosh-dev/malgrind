// Self-check for the core: stack merging, table overflow, frame encoding, CRC.
#include "malgrind/profiler.hpp"

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

    // encode a flush through Profiler and check the frame layout + CRC
    Profiler p({"test", Arch::Host, 1'000'000, 1}, 997);
    p.record(0, a, 3, 50);
    p.record(0, a, 3, 70);
    std::vector<std::uint8_t> frame;
    p.flush([&](const std::uint8_t* d, std::size_t n) { frame.assign(d, d + n); }, [] {}, 1234);
    assert(frame.size() > 8 && std::memcmp(frame.data(), "MLC1", 4) == 0);
    const std::uint32_t len = rd(&frame[4], 2);
    assert(frame.size() == 4 + 2 + len + 4);
    assert(rd(&frame[frame.size() - 4], 4) == crc32(&frame[4], len + 2));
    const std::uint8_t* body = &frame[6];
    assert(body[0] == 1 && body[4] == 1 /*ncores*/ && rd(body + 6, 4) == 1'000'000 && rd(body + 10, 2) == 997);
    assert(rd(body + 12, 2) == 2 && rd(body + 14, 2) == 0); // samples, dropped
    assert(rd(body + 16, 4) == 60 && rd(body + 20, 4) == 70 && rd(body + 24, 4) == 1234);
    assert(rd(body + 28, 2) == 1);                          // one merged entry
    assert(rd(body + 30, 2) == 2 && body[32] == 3 && rd(body + 33, 4) == 0x400);

    // a flipped byte must break the CRC
    frame[20] ^= 0x01;
    assert(rd(&frame[frame.size() - 4], 4) != crc32(&frame[4], len + 2));

    // the table was cleared by the flush: second frame is empty
    p.flush([&](const std::uint8_t* d, std::size_t n) { frame.assign(d, d + n); }, [] {}, 0);
    assert(rd(&frame[6 + 28], 2) == 0 && rd(&frame[6 + 1], 2) == 1 /*seq*/);

    std::puts("core ok");
}
