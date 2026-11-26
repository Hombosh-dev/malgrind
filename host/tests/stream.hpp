#pragma once

// Test data shared by the host tests: a byte stream made by the real encoder in core/.

#include "malgrind/command.hpp"
#include "malgrind/profiler.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <span>
#include <thread>
#include <vector>

// `flushes` rounds of a 2-core Xtensa target = 2 * flushes frames. Per round: core 0 reports a <- b <- main three
// times (stack 0x400d1000, 0x400d2000, 0x400d3000), core 1 reports c <- b <- main once (0x400d1800 as the leaf).
inline std::vector<std::uint8_t> make_stream(unsigned flushes) {
    using namespace malgrind;
    Profiler p({"test", Arch::Xtensa, 240'000'000, 2}, 997);
    const std::uint32_t leaf_a[] = {0x400d1000, 0x400d2000, 0x400d3000};
    const std::uint32_t leaf_c[] = {0x400d1800, 0x400d2000, 0x400d3000};
    std::vector<std::uint8_t> stream;
    for (unsigned i = 0; i < flushes; ++i) {
        for (int k = 0; k < 3; ++k) p.record(0, leaf_a, 3, 100);
        p.record(1, leaf_c, 3, 100);
        p.flush([&](const std::uint8_t* d, std::size_t n) { stream.insert(stream.end(), d, d + n); }, [] {}, 5000);
    }
    return stream;
}

inline std::vector<std::uint8_t> make_hello_frame(const malgrind::HelloPayload& hello) {
    std::array<std::uint8_t, 64> buf{};
    const std::size_t n = malgrind::encode_hello(buf.data(), buf.size(), hello);
    assert(n > 0);
    return {buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n)};
}

inline std::vector<std::uint8_t> make_heap_frame(std::uint16_t seq, std::uint32_t t_ms,
                                                 std::span<const malgrind::HeapRegion> regions) {
    std::array<std::uint8_t, 256> buf{};
    const std::size_t n = malgrind::encode_heap(buf.data(), buf.size(), seq, t_ms, regions.data(), regions.size());
    assert(n > 0);
    return {buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n)};
}

inline std::vector<std::uint8_t> make_tasks_frame(std::uint16_t seq, std::uint32_t t_ms, std::uint32_t total_runtime,
                                                  std::span<const malgrind::TaskSample> tasks) {
    std::array<std::uint8_t, 1280> buf{};
    const std::size_t n =
        malgrind::encode_tasks(buf.data(), buf.size(), seq, t_ms, total_runtime, tasks.data(), tasks.size());
    assert(n > 0);
    return {buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n)};
}

inline std::vector<malgrind::Command> decode_commands(std::span<const std::uint8_t> bytes) {
    malgrind::CommandParser parser;
    std::vector<malgrind::Command> out;
    parser.feed(bytes.data(), bytes.size(), [&](const malgrind::Command& cmd) { out.push_back(cmd); });
    return out;
}

template <class Pred>
bool wait_for(Pred pred, std::chrono::milliseconds limit = std::chrono::milliseconds(3000)) {
    using namespace std::chrono_literals;
    for (const auto end = std::chrono::steady_clock::now() + limit; std::chrono::steady_clock::now() < end;
         std::this_thread::sleep_for(10ms))
        if (pred()) return true;
    return false;
}
