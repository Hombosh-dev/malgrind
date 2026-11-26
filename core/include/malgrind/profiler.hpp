#pragma once

// malgrind core: freestanding C++20. No heap, exceptions, iostream, RTOS or vendor SDK.
//
// A port (platforms/<mcu>/) does three things:
//   1. start a periodic timer per core; in its ISR unwind the stack (innermost frame first)
//      and call Profiler::record() with the PCs and the ISR's own cost in cycles;
//   2. periodically call Profiler::flush(write, settle, last_flush_cycles);
//   3. provide write(bytes) -- usually a UART.
//
// Wire format (all integers little-endian):
//   envelope (unchanged): "MLC1" | len u16 | body[len] | crc32 u32
//                         crc32 (IEEE, same as zlib) covers len + body.
//
//   v1 body (decoded forever by host, never sent by v2 agents):
//     ver=1 | seq u16 | arch u8 | ncores u8 | core u8 | cpu_hz u32 | rate_hz u16 |
//     samples u16 | dropped u16 | isr_cyc_avg u32 | isr_cyc_max u32 | flush_cyc u32 |
//     n_entries u16 | { count u16, depth u8, pc u32[depth] } * n_entries
//
//   v2 body: ver=2 | kind u8 | payload
//     kind 0 SAMPLES:  identical payload to v1 after `ver` (pc[0] is the innermost frame)
//     kind 1 HELLO:    seq u16 | arch u8 | ncores u8 | cpu_hz u32 | agent_ver u16 | caps u32 |
//                      state u8 (0 sleeping, 1 sampling) | rate_hz u16 | analyses u32
//     kind 2 HEAP:     seq u16 | t_ms u32 | n u8 |
//                      { region u8, total u32, free u32, min_free u32, largest u32 } * n
//     kind 3 TASKS:    seq u16 | t_ms u32 | total_runtime u32 | n u8 |
//                      { handle u32, name char[16], prio u8, core u8 (0xff = any),
//                        state u8, runtime u32 (delta since last), stack_hwm u32 (bytes) } * n
//     kind 4 ALLOCS:   seq u16 | n u16 |
//                      { op u8 (0 alloc, 1 free), t_ms u32, addr u32, size u32 (0 for free),
//                        depth u8, pc u32[depth] } * n
//     kind 5 ERROR:    seq u16 | t_ms u32 | code u8 (1 heap corrupt, 2 stack overflow, 3 alloc failed) |
//                      arg u32 | name char[16] | depth u8 | pc u32[depth]
//     kind 16 COMMAND: (host -> agent) cmd u8 (1 start, 2 stop, 3 hello now) | rate_hz u16 | analyses u32
//
//   caps / analyses bits: 0 samples, 1 heap, 2 tasks, 3 allocs, 4 errors
//   heap regions:         0 internal/DRAM, 1 external/PSRAM, 2 IRAM-exec, 255 total (family-neutral)

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
inline constexpr std::size_t kTaskNameLen = 16;

inline constexpr std::uint8_t kWireVersion = 2;

inline constexpr std::uint8_t kKindSamples = 0;
inline constexpr std::uint8_t kKindHello = 1;
inline constexpr std::uint8_t kKindHeap = 2;
inline constexpr std::uint8_t kKindTasks = 3;
inline constexpr std::uint8_t kKindAllocs = 4;
inline constexpr std::uint8_t kKindError = 5;
inline constexpr std::uint8_t kKindCommand = 16;

inline constexpr std::uint32_t kCapSamples = 1u << 0;
inline constexpr std::uint32_t kCapHeap = 1u << 1;
inline constexpr std::uint32_t kCapTasks = 1u << 2;
inline constexpr std::uint32_t kCapAllocs = 1u << 3;
inline constexpr std::uint32_t kCapErrors = 1u << 4;

inline constexpr std::uint8_t kHeapInternal = 0;
inline constexpr std::uint8_t kHeapExternal = 1;
inline constexpr std::uint8_t kHeapIram = 2;
inline constexpr std::uint8_t kHeapTotal = 255;

inline constexpr std::uint8_t kErrHeapCorrupt = 1;
inline constexpr std::uint8_t kErrStackOverflow = 2;
inline constexpr std::uint8_t kErrAllocFailed = 3;

inline constexpr std::uint8_t kCmdStart = 1;
inline constexpr std::uint8_t kCmdStop = 2;
inline constexpr std::uint8_t kCmdHello = 3;

inline constexpr std::size_t kHeaderSize = 4 + 2 + 31;
inline constexpr std::size_t kMaxFrameSize =
    kHeaderSize + kTableSize * (3 + 4 * kMaxDepth) + 4;

std::uint32_t crc32(const std::uint8_t* data, std::size_t n) noexcept;

struct HelloPayload {
    std::uint16_t seq{};
    Arch arch{};
    std::uint8_t ncores{};
    std::uint32_t cpu_hz{};
    std::uint16_t agent_ver{};
    std::uint32_t caps{};
    std::uint8_t state{}; // 0 sleeping, 1 sampling
    std::uint16_t rate_hz{};
    std::uint32_t analyses{};
};

struct HeapRegion {
    std::uint8_t region{};
    std::uint32_t total{};
    std::uint32_t free{};
    std::uint32_t min_free{};
    std::uint32_t largest{};
};

struct TaskSample {
    std::uint32_t handle{};
    char name[kTaskNameLen]{};
    std::uint8_t prio{};
    std::uint8_t core{0xffu};
    std::uint8_t state{};
    std::uint32_t runtime{};
    std::uint32_t stack_hwm{};
};

struct AllocEvent {
    std::uint8_t op{}; // 0 alloc, 1 free
    std::uint32_t t_ms{};
    std::uint32_t addr{};
    std::uint32_t size{}; // 0 for free
    std::uint8_t depth{};
    std::uint32_t pcs[kMaxDepth]{};
};

struct ErrorEvent {
    std::uint16_t seq{};
    std::uint32_t t_ms{};
    std::uint8_t code{};
    std::uint32_t arg{};
    char name[kTaskNameLen]{};
    std::uint8_t depth{};
    std::uint32_t pcs[kMaxDepth]{};
};

// Freestanding v2 encoders: write one framed message into `dst[0..cap)` and return the total
// encoded byte count, or 0 if `dst == nullptr`, `cap` is too small, or inputs are out of range.
std::size_t encode_hello(std::uint8_t* dst, std::size_t cap, const HelloPayload& hello) noexcept;
std::size_t encode_heap(std::uint8_t* dst, std::size_t cap, std::uint16_t seq, std::uint32_t t_ms,
                        const HeapRegion* regions, std::size_t n) noexcept;
std::size_t encode_tasks(std::uint8_t* dst, std::size_t cap, std::uint16_t seq, std::uint32_t t_ms,
                         std::uint32_t total_runtime, const TaskSample* tasks, std::size_t n) noexcept;
std::size_t encode_allocs(std::uint8_t* dst, std::size_t cap, std::uint16_t seq,
                          const AllocEvent* events, std::size_t n) noexcept;
std::size_t encode_error(std::uint8_t* dst, std::size_t cap, const ErrorEvent& err) noexcept;

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
    void set_rate(std::uint16_t rate_hz) noexcept { rate_hz_.store(rate_hz, std::memory_order_relaxed); }
    [[nodiscard]] std::uint16_t rate_hz() const noexcept { return rate_hz_.load(std::memory_order_relaxed); }

    void clear() noexcept {
        for (auto& core_tables : tables_) {
            for (StackTable& t : core_tables) t.clear();
        }
    }

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
    std::atomic<std::uint16_t> rate_hz_;
    std::uint16_t seq_[kMaxCores]{};
    std::atomic<std::uint8_t> active_[kMaxCores]{};
    StackTable tables_[kMaxCores][2]{};
    std::uint8_t buf_[kMaxFrameSize];
};

} // namespace malgrind
