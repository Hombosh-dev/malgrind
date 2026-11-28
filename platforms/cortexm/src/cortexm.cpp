#include "malgrind/platform/cortexm.hpp"

#include "malgrind/command.hpp"
#include "malgrind/profiler.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

namespace {

constexpr std::uint32_t kFlushMs = 250;
constexpr std::uint32_t kHelloMs = 1000;
constexpr std::uint16_t kAgentVersion = 2;
constexpr std::uint16_t kMinRateHz = 10;
constexpr std::uint16_t kMaxRateHz = 5000;
constexpr std::uint16_t kDefaultRateHz = 997;
constexpr std::uint32_t kSupportedCaps = malgrind::kCapSamples;
constexpr std::uint8_t kStateSleeping = 0;
constexpr std::uint8_t kStateSampling = 1;

alignas(malgrind::Profiler) std::uint8_t g_profiler_storage[sizeof(malgrind::Profiler)];
malgrind::Profiler* g_profiler = nullptr;
malgrind::CommandParser g_parser;
CortexMConfig g_cfg{};
std::atomic<bool> g_initialized{false};
std::atomic<bool> g_sampling{false};
std::uint16_t g_default_rate_hz = kDefaultRateHz;
std::uint16_t g_hello_seq = 0;
std::uint32_t g_flush_cycles = 0;
std::uint32_t g_last_flush_ms = 0;
std::uint32_t g_last_hello_ms = 0;

constexpr std::uint16_t clamp_rate(std::uint16_t rate_hz) noexcept {
    if (rate_hz < kMinRateHz) return kMinRateHz;
    if (rate_hz > kMaxRateHz) return kMaxRateHz;
    return rate_hz;
}

inline std::uint32_t read_cycles() noexcept {
    if (g_cfg.cycle_counter != nullptr) {
        return g_cfg.cycle_counter(g_cfg.ctx);
    }
#if (defined(__arm__) || defined(__thumb__)) && !defined(__ARM_ARCH_6M__)
    if (g_cfg.has_dwt_cyccnt) {
        return *reinterpret_cast<const volatile std::uint32_t*>(0xE0001004u);
    }
#endif
    return 0u;
}

inline std::uint32_t raw_delta_to_cycles(std::uint32_t delta) noexcept {
    if (g_cfg.has_dwt_cyccnt) {
        return delta;
    }
    // Scale 1 us timer delta to CPU cycles in 64-bit arithmetic to avoid overflow.
    const std::uint64_t scaled =
        (static_cast<std::uint64_t>(delta) * static_cast<std::uint64_t>(g_cfg.cpu_hz)) / 1'000'000ULL;
    return scaled > 0xFFFFFFFFULL ? 0xFFFFFFFFu : static_cast<std::uint32_t>(scaled);
}

void send_hello(bool sampling) noexcept {
    if (g_profiler == nullptr || g_cfg.write_bytes == nullptr) return;
    const malgrind::TargetInfo& info = g_profiler->info();
    std::uint8_t buf[64]{};
    const malgrind::HelloPayload hello{
        .seq = g_hello_seq++,
        .arch = info.arch,
        .ncores = info.num_cores,
        .cpu_hz = info.cpu_hz,
        .agent_ver = kAgentVersion,
        .caps = kSupportedCaps,
        .state = static_cast<std::uint8_t>(sampling ? kStateSampling : kStateSleeping),
        .rate_hz = g_profiler->rate_hz(),
        .analyses = sampling ? kSupportedCaps : 0u,
    };
    const std::size_t n = malgrind::encode_hello(buf, sizeof(buf), hello);
    if (n > 0u) g_cfg.write_bytes(g_cfg.ctx, buf, n);
}

void flush_samples() noexcept {
    const std::uint32_t start = read_cycles();
    std::uint32_t waiting_raw = 0; // UART/transport blocking time is excluded from CPU flush cost.
    g_profiler->flush(
        [&](const std::uint8_t* d, std::size_t n) noexcept {
            const std::uint32_t tw = read_cycles();
            g_cfg.write_bytes(g_cfg.ctx, d, n);
            waiting_raw += static_cast<std::uint32_t>(read_cycles() - tw);
        },
        []() noexcept {
            // Single-core ISRs preempt and finish before poll() resumes; on dual-core targets,
            // order the active buffer swap before reading the idle table.
            std::atomic_thread_fence(std::memory_order_seq_cst);
        },
        g_flush_cycles);
    const std::uint32_t total_raw = static_cast<std::uint32_t>(read_cycles() - start);
    const std::uint32_t cpu_raw =
        total_raw >= waiting_raw ? static_cast<std::uint32_t>(total_raw - waiting_raw) : 0u;
    g_flush_cycles = raw_delta_to_cycles(cpu_raw);
}

void handle_command(const malgrind::Command& cmd) noexcept {
    switch (cmd.cmd) {
        case malgrind::kCmdStart: {
            const std::uint16_t rate = clamp_rate(cmd.rate_hz != 0u ? cmd.rate_hz : g_default_rate_hz);
            g_sampling.store(false, std::memory_order_release);
            g_profiler->clear();
            g_profiler->set_rate(rate);
            g_flush_cycles = 0;
            if (g_cfg.on_state_change != nullptr) {
                g_cfg.on_state_change(g_cfg.ctx, true, rate);
            }
            g_last_flush_ms = g_cfg.millis(g_cfg.ctx);
            g_sampling.store(true, std::memory_order_release);
            send_hello(true);
            break;
        }
        case malgrind::kCmdStop: {
            g_sampling.store(false, std::memory_order_release);
            g_profiler->clear();
            g_flush_cycles = 0;
            if (g_cfg.on_state_change != nullptr) {
                g_cfg.on_state_change(g_cfg.ctx, false, g_profiler->rate_hz());
            }
            g_last_hello_ms = g_cfg.millis(g_cfg.ctx);
            send_hello(false);
            break;
        }
        case malgrind::kCmdHello: {
            g_last_hello_ms = g_cfg.millis(g_cfg.ctx);
            send_hello(g_sampling.load(std::memory_order_acquire));
            break;
        }
        default:
            break;
    }
}

} // namespace

extern "C" bool malgrind_cortexm_init(const CortexMConfig& cfg) noexcept {
    if (cfg.write_bytes == nullptr || cfg.millis == nullptr) return false;
    if (cfg.num_cores == 0u || cfg.num_cores > malgrind::kMaxCores) return false;

    g_initialized.store(false, std::memory_order_release);
    g_sampling.store(false, std::memory_order_release);

#if (defined(__arm__) || defined(__thumb__)) && !defined(__ARM_ARCH_6M__)
    if (cfg.has_dwt_cyccnt) {
        // Enable CoreDebug DEMCR.TRCENA (bit 24) before writing DWT registers.
        volatile std::uint32_t& demcr = *reinterpret_cast<volatile std::uint32_t*>(0xE000EDFCu);
        demcr |= (1u << 24u);
        // Cortex-M7 (CPUID PartNo 0xC27) locks DWT writes until LAR is unlocked.
        const std::uint32_t cpuid = *reinterpret_cast<const volatile std::uint32_t*>(0xE000ED00u);
        if (((cpuid >> 4u) & 0xFFFu) == 0xC27u) {
            *reinterpret_cast<volatile std::uint32_t*>(0xE0001FB0u) = 0xC5ACCE55u;
        }
        *reinterpret_cast<volatile std::uint32_t*>(0xE0001004u) = 0u;
        volatile std::uint32_t& dwt_ctrl = *reinterpret_cast<volatile std::uint32_t*>(0xE0001000u);
        dwt_ctrl |= 1u;
    }
#endif

    if (g_profiler != nullptr) {
        g_profiler->~Profiler();
        g_profiler = nullptr;
    }

    g_cfg = cfg;
    const char* name = (cfg.target_name != nullptr && cfg.target_name[0] != '\0') ? cfg.target_name : "cortexm";
    const std::uint16_t init_rate = clamp_rate(cfg.rate_hz != 0u ? cfg.rate_hz : kDefaultRateHz);
    g_default_rate_hz = init_rate;

    // Placement new into static .bss storage (zero heap allocation).
    g_profiler = ::new (static_cast<void*>(g_profiler_storage))
        malgrind::Profiler(malgrind::TargetInfo{name, malgrind::Arch::ArmV7M, cfg.cpu_hz, cfg.num_cores}, init_rate);

    g_parser.reset();
    g_hello_seq = 0;
    g_flush_cycles = 0;
    const std::uint32_t now_ms = g_cfg.millis(g_cfg.ctx);
    g_last_flush_ms = now_ms;
    // Offset by kHelloMs so the first sleeping poll() emits a HELLO beacon immediately.
    g_last_hello_ms = static_cast<std::uint32_t>(now_ms - kHelloMs);

    g_sampling.store(cfg.start_sampling, std::memory_order_release);
    g_initialized.store(true, std::memory_order_release);
    return true;
}

extern "C" void malgrind_cortexm_sample_isr(std::uint8_t core_id, const ExceptionFrame* frame) noexcept {
    if (!g_initialized.load(std::memory_order_acquire)) return;
    if (!g_sampling.load(std::memory_order_acquire)) return;
    if (frame == nullptr || g_profiler == nullptr) return;
    if (core_id >= g_profiler->info().num_cores || core_id >= malgrind::kMaxCores) return;

    const std::uint32_t t0 = read_cycles();

    std::uint32_t pcs[2]{};
    pcs[0] = frame->pc & ~1u;
    unsigned depth = 1;
    // Ignore 0 and ARM EXC_RETURN sentinels (0xFxxxxxxx) so only real return addresses remain.
    if (frame->lr != 0u && (frame->lr & 0xF0000000u) != 0xF0000000u) {
        pcs[1] = frame->lr & ~1u;
        depth = 2;
    }

    const std::uint32_t t1 = read_cycles();
    const std::uint32_t cycles = raw_delta_to_cycles(static_cast<std::uint32_t>(t1 - t0));
    g_profiler->record(core_id, pcs, depth, cycles);
}

extern "C" void malgrind_cortexm_poll() noexcept {
    if (!g_initialized.load(std::memory_order_acquire) || g_profiler == nullptr) return;

    if (g_cfg.read_bytes != nullptr) {
        std::uint8_t rx[64]{};
        for (int reads = 0; reads < 16; ++reads) {
            const std::size_t n = g_cfg.read_bytes(g_cfg.ctx, rx, sizeof(rx));
            if (n == 0u) break;
            const std::size_t bounded = n <= sizeof(rx) ? n : sizeof(rx);
            g_parser.feed(rx, bounded, handle_command);
            if (bounded < sizeof(rx)) break;
        }
    }

    const std::uint32_t now_ms = g_cfg.millis(g_cfg.ctx);
    if (g_sampling.load(std::memory_order_acquire)) {
        if (static_cast<std::uint32_t>(now_ms - g_last_flush_ms) >= kFlushMs) {
            flush_samples();
            g_last_flush_ms = g_cfg.millis(g_cfg.ctx);
        }
    } else {
        if (static_cast<std::uint32_t>(now_ms - g_last_hello_ms) >= kHelloMs) {
            send_hello(false);
            g_last_hello_ms = now_ms;
        }
    }
}
