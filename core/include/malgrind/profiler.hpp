#pragma once

// malgrind core: freestanding C++20. No heap, exceptions, iostream, RTOS or vendor SDK.
//
// A port (platforms/<mcu>/) does three things:
//   1. start a periodic timer per core; in its ISR unwind the stack (innermost frame first)
//      and call Profiler::record() with the PCs and the ISR's own cost in cycles;
//   2. periodically call Profiler::flush(write, settle, last_flush_cycles);
//   3. provide write(bytes) -- usually a UART.
//
// Wire frame (all integers little-endian), one frame per core per flush:
//   "MLC1" | len u16 | body[len] | crc32 u32          crc32 (IEEE, same as zlib) covers len+body
//   body = ver u8 | seq u16 | arch u8 | ncores u8 | core u8 | cpu_hz u32 | rate_hz u16 |
//          samples u16 | dropped u16 | isr_cyc_avg u32 | isr_cyc_max u32 | flush_cyc u32 |
//          n_entries u16 | { count u16, depth u8, pc u32[depth] } * n_entries
// pc[0] is the innermost (interrupted) frame.

#include "malgrind/target_info.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

// Code that runs in the sampling ISR. On ESP32 it must live in IRAM; COMDAT copies of inline functions
// cannot (their literal pools end up in flash), so it is force-inlined into the port's IRAM callback.
#ifndef MALGRIND_ISR
#if defined(ESP_PLATFORM)
#define MALGRIND_ISR __attribute__((always_inline))
#else
#define MALGRIND_ISR
#endif
#endif

namespace malgrind {

inline constexpr unsigned kMaxCores = 2;
inline constexpr unsigned kMaxDepth = 16;
inline constexpr unsigned kTableSize = 128; // power of two
inline constexpr unsigned kMaxProbe = 16;   // bounds the time spent inside the ISR
inline constexpr std::size_t kHeaderSize = 4 + 2 + 30;
inline constexpr std::size_t kMaxFrameSize =
    kHeaderSize + kTableSize * (3 + 4 * kMaxDepth) + 4;

std::uint32_t crc32(const std::uint8_t* data, std::size_t n) noexcept;

struct Entry {
    std::uint32_t hash;
    std::uint32_t count; // 0 = free slot
    std::uint8_t depth;
    std::uint32_t pcs[kMaxDepth];
};

// Fixed-size hash of whole stacks -> hit count. Single writer (the ISR of one core).
struct StackTable {
    Entry entries[kTableSize];
    std::uint32_t samples;  // stacks stored
    std::uint32_t dropped;  // stacks lost because the table was full
    std::uint64_t isr_sum;  // sum of ISR cycles over samples + dropped
    std::uint32_t isr_max;

    MALGRIND_ISR void record(const std::uint32_t* pcs, unsigned n, std::uint32_t isr_cycles) noexcept {
        isr_sum += isr_cycles;
        if (isr_cycles > isr_max) isr_max = isr_cycles;
        if (n > kMaxDepth) n = kMaxDepth;
        std::uint32_t h = 2166136261u; // FNV-1a over the PC words
        for (unsigned i = 0; i < n; ++i) h = (h ^ pcs[i]) * 16777619u;
        unsigned slot = h & (kTableSize - 1);
        for (unsigned probe = 0; n != 0 && probe < kMaxProbe; ++probe, slot = (slot + 1) & (kTableSize - 1)) {
            Entry& e = entries[slot];
            if (e.count == 0) {
                e.hash = h;
                e.depth = static_cast<std::uint8_t>(n);
                for (unsigned i = 0; i < n; ++i) e.pcs[i] = pcs[i];
                e.count = 1;
                ++samples;
                return;
            }
            if (e.hash == h && e.depth == n) {
                unsigned i = 0;
                while (i < n && e.pcs[i] == pcs[i]) ++i;
                if (i == n) {
                    ++e.count;
                    ++samples;
                    return;
                }
            }
        }
        ++dropped;
    }

    void clear() noexcept {
        for (Entry& e : entries) e.count = 0;
        samples = dropped = isr_max = 0;
        isr_sum = 0;
    }
};

class Profiler {
public:
    Profiler(TargetInfo info, std::uint16_t rate_hz) noexcept : info_(info), rate_hz_(rate_hz) {}

    [[nodiscard]] const TargetInfo& info() const noexcept { return info_; }

    // ISR: called on `core` with the unwound stack, innermost frame first.
    MALGRIND_ISR void record(unsigned core, const std::uint32_t* pcs, unsigned n,
                           std::uint32_t isr_cycles) noexcept {
        tables_[core][active_[core].load(std::memory_order_acquire)].record(pcs, n, isr_cycles);
    }

    // Swap buffers of every core, let in-flight ISRs finish (settle), then encode every core's
    // idle table into one frame and hand it to write(ptr, len). Tables are cleared afterwards.
    // last_flush_cycles: cost of the previous flush, reported to the host.
    template <class Write, class Settle>
    void flush(Write&& write, Settle&& settle, std::uint32_t last_flush_cycles) noexcept {
        for (unsigned c = 0; c < info_.num_cores; ++c) {
            active_[c].store(active_[c].load(std::memory_order_relaxed) ^ 1, std::memory_order_release);
        }
        settle(); // must wait at least one sample period
        for (unsigned c = 0; c < info_.num_cores; ++c) {
            StackTable& t = tables_[c][active_[c].load(std::memory_order_relaxed) ^ 1];
            write(buf_, encode(c, t, last_flush_cycles));
            t.clear();
        }
    }

private:
    std::size_t encode(unsigned core, const StackTable& t, std::uint32_t flush_cycles) noexcept;

    TargetInfo info_;
    std::uint16_t rate_hz_;
    std::uint16_t seq_[kMaxCores]{};
    std::atomic<std::uint8_t> active_[kMaxCores]{};
    StackTable tables_[kMaxCores][2]{};
    std::uint8_t buf_[kMaxFrameSize];
};

} // namespace malgrind
