// Starts and drives the Cortex-M profiler agent on RP2040 without modifying user files.
//
// Initialization runs lazily on the first SysTick interrupt rather than in a [[gnu::constructor]],
// because pre-main constructors execute before USB-CDC (TinyUSB / Arduino Serial / pico_stdio_usb)
// and system clocks are ready. isr_systick is wrapped via `-Wl,--wrap=isr_systick` using
// ARMv6-M Thumb-1-safe instructions so we extract the hardware-stacked ExceptionFrame before any
// compiler prologue alters SP/LR and then tail-chain __real_isr_systick.
#include "malgrind/platform/cortexm.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

#if defined(ARDUINO)
#include <Arduino.h>
#elif defined(PICO_ON_DEVICE)
#if __has_include("pico/stdlib.h")
#include "pico/stdlib.h"
#endif
#if __has_include("pico/stdio.h")
#include "pico/stdio.h"
#endif
#if defined(LIB_PICO_STDIO_USB) && LIB_PICO_STDIO_USB && __has_include("pico/stdio_usb.h")
#include "pico/stdio_usb.h"
#endif
#endif

#if (defined(ARDUINO) || defined(PICO_ON_DEVICE)) && __has_include("hardware/clocks.h")
#include "hardware/clocks.h"
#endif

#ifndef MALGRIND_RATE_HZ
#define MALGRIND_RATE_HZ 997
#endif

#ifndef MALGRIND_START_SAMPLING
#define MALGRIND_START_SAMPLING 1
#endif

// Marker searched by the host (kRp2040AgentMarkerV2) so flash backup skips images that already hold the agent.
extern "C" [[gnu::used]] const char malgrind_agent_marker[] = "malgrind-agent-v2";

// Weak hooks allowing firmware/board code to customize USB-CDC/UART transport, clock/timebase, or polling.
extern "C" {
[[gnu::weak]] void malgrind_rp2040_write(const std::uint8_t* data, std::size_t len);
[[gnu::weak]] std::size_t malgrind_rp2040_read(std::uint8_t* dst, std::size_t cap);
[[gnu::weak]] std::uint32_t malgrind_rp2040_cpu_hz();
[[gnu::weak]] std::uint32_t malgrind_rp2040_millis();
void malgrind_rp2040_poll() noexcept;
}

#if defined(__arm__) || defined(__thumb__)
extern "C" [[gnu::weak]] extern std::uint32_t SystemCoreClock;
#endif

namespace {

constexpr std::uint32_t kIsrPollGateMs = 50;
constexpr std::uint32_t kMainPollLeaseMs = 500;
constexpr std::uint32_t kBootClockReinitWindowMs = 500;
constexpr std::uint32_t kDefaultRp2040CpuHz = 125'000'000u;
constexpr std::uint32_t kRp2040TimerRawlAddr = 0x40054028u;
constexpr std::uint32_t kRp2040SioCpuidAddr = 0xD0000000u;
constexpr std::uint16_t kInitRateHz =
    (MALGRIND_RATE_HZ) != 0 ? static_cast<std::uint16_t>(MALGRIND_RATE_HZ) : static_cast<std::uint16_t>(997);
constexpr bool kStartSampling = (MALGRIND_START_SAMPLING) != 0;

std::atomic<std::uint32_t> g_tick_ms{0};
std::atomic<std::uint32_t> g_last_isr_poll_ms{0};
std::atomic<std::uint32_t> g_last_main_poll_ms{0};
std::atomic<bool> g_has_main_poll{false};
std::atomic<bool> g_initialized{false};
std::atomic<bool> g_in_poll{false};
std::uint32_t g_init_cpu_hz = 0;

bool try_enter_poll() noexcept {
#if defined(__arm__) || defined(__thumb__)
    // ARMv6-M lacks LDREX/STREX; mask interrupts briefly instead of emitting libatomic RMW calls.
    std::uint32_t primask = 0;
    __asm__ volatile("mrs %0, primask\ncpsid i" : "=r"(primask)::"memory");
    const bool busy = g_in_poll.load(std::memory_order_relaxed);
    if (!busy) {
        g_in_poll.store(true, std::memory_order_relaxed);
    }
    __asm__ volatile("msr primask, %0" ::"r"(primask) : "memory");
    return !busy;
#else
    return !g_in_poll.exchange(true, std::memory_order_acq_rel);
#endif
}

void exit_poll() noexcept {
    g_in_poll.store(false, std::memory_order_release);
}

std::uint32_t current_core_id() noexcept {
#if defined(__arm__) || defined(__thumb__)
    return *reinterpret_cast<const volatile std::uint32_t*>(kRp2040SioCpuidAddr) & 1u;
#else
    return 0u;
#endif
}

void write_cb(void*, const std::uint8_t* data, std::size_t len) {
    if (malgrind_rp2040_write != nullptr) {
        malgrind_rp2040_write(data, len);
    }
}

std::size_t read_cb(void*, std::uint8_t* dst, std::size_t cap) {
    if (malgrind_rp2040_read != nullptr) {
        return malgrind_rp2040_read(dst, cap);
    }
    return 0;
}

std::uint32_t cycle_counter_cb(void*) {
#if defined(__arm__) || defined(__thumb__)
    // RP2040 TIMER->TIMERAWL (1 us free-running hardware timer, no latch side effects).
    return *reinterpret_cast<const volatile std::uint32_t*>(kRp2040TimerRawlAddr);
#else
    return 0u;
#endif
}

std::uint32_t millis_cb(void*) {
    if (malgrind_rp2040_millis != nullptr) {
        return malgrind_rp2040_millis();
    }
    return g_tick_ms.load(std::memory_order_relaxed);
}

std::uint32_t detect_cpu_hz() noexcept {
    if (malgrind_rp2040_cpu_hz != nullptr) {
        const std::uint32_t custom = malgrind_rp2040_cpu_hz();
        if (custom != 0u) return custom;
    }
#if (defined(ARDUINO) || defined(PICO_ON_DEVICE)) && __has_include("hardware/clocks.h")
    const std::uint32_t sys_hz = clock_get_hz(clk_sys);
    if (sys_hz != 0u) return sys_hz;
#endif
#if defined(__arm__) || defined(__thumb__)
    if (&SystemCoreClock != nullptr && SystemCoreClock != 0u) {
        return SystemCoreClock;
    }
#endif
#ifdef F_CPU
    return static_cast<std::uint32_t>(F_CPU);
#else
    return kDefaultRp2040CpuHz;
#endif
}

bool init_agent(std::uint32_t cpu_hz) noexcept {
    CortexMConfig cfg{};
    cfg.target_name = "rp2040";
    cfg.cpu_hz = cpu_hz;
    cfg.rate_hz = kInitRateHz;
    cfg.start_sampling = kStartSampling;
    cfg.num_cores = 1;
    // Cortex-M0+ has no DWT CYCCNT; platforms/cortexm scales 1 us TIMERAWL deltas using cpu_hz.
    cfg.has_dwt_cyccnt = false;
    cfg.ctx = nullptr;
    cfg.write_bytes = write_cb;
    cfg.read_bytes = read_cb;
    cfg.cycle_counter = cycle_counter_cb;
    cfg.millis = millis_cb;
    cfg.on_state_change = nullptr;
    return malgrind_cortexm_init(cfg);
}

} // namespace

extern "C" [[gnu::weak]] void malgrind_rp2040_write(const std::uint8_t* data, std::size_t len) {
    if (data == nullptr || len == 0u) return;
#if defined(ARDUINO) && !defined(NO_SERIAL)
    if (!Serial) return;
    Serial.write(data, len);
#elif defined(PICO_ON_DEVICE)
#if defined(LIB_PICO_STDIO_USB) && LIB_PICO_STDIO_USB && __has_include("pico/stdio_usb.h")
    if (!stdio_usb_connected()) return;
#endif
#if __has_include("pico/stdio.h")
    for (std::size_t i = 0; i < len; ++i) {
        putchar_raw(static_cast<int>(data[i]));
    }
#else
    (void)data;
    (void)len;
#endif
#else
    (void)data;
    (void)len;
#endif
}

extern "C" [[gnu::weak]] std::size_t malgrind_rp2040_read(std::uint8_t* dst, std::size_t cap) {
    if (dst == nullptr || cap == 0u) return 0u;
#if defined(ARDUINO) && !defined(NO_SERIAL)
    if (!Serial) return 0u;
    std::size_t n = 0;
    while (n < cap && Serial.available() > 0) {
        const int ch = Serial.read();
        if (ch < 0) break;
        dst[n++] = static_cast<std::uint8_t>(ch);
    }
    return n;
#elif defined(PICO_ON_DEVICE) && __has_include("pico/stdio.h")
#if defined(LIB_PICO_STDIO_USB) && LIB_PICO_STDIO_USB && __has_include("pico/stdio_usb.h")
    if (!stdio_usb_connected()) return 0u;
#endif
    std::size_t n = 0;
    while (n < cap) {
        const int ch = getchar_timeout_us(0u);
        if (ch < 0) break;
        dst[n++] = static_cast<std::uint8_t>(ch);
    }
    return n;
#else
    (void)dst;
    (void)cap;
    return 0u;
#endif
}

extern "C" void malgrind_rp2040_poll() noexcept {
    if (!g_initialized.load(std::memory_order_acquire)) return;
    const std::uint32_t now_ms = g_tick_ms.load(std::memory_order_relaxed);
    g_last_main_poll_ms.store(now_ms, std::memory_order_relaxed);
    g_has_main_poll.store(true, std::memory_order_release);
    if (!try_enter_poll()) return;
    malgrind_cortexm_poll();
    exit_poll();
}

extern "C" [[gnu::used]] void malgrind_rp2040_on_systick(const ExceptionFrame* frame) noexcept {
#if defined(__arm__) || defined(__thumb__)
    // Keep malgrind_agent_marker live under --gc-sections.
    __asm__ volatile("" : : "r"(malgrind_agent_marker));
#else
    (void)malgrind_agent_marker;
#endif

    if (current_core_id() != 0u) return;

    const std::uint32_t now_ms = g_tick_ms.load(std::memory_order_relaxed) + 1u;
    g_tick_ms.store(now_ms, std::memory_order_relaxed);
    const std::uint32_t cpu_hz = detect_cpu_hz();

    if (!g_initialized.load(std::memory_order_acquire)) {
        if (init_agent(cpu_hz)) {
            g_init_cpu_hz = cpu_hz;
            g_last_isr_poll_ms.store(now_ms, std::memory_order_relaxed);
            g_initialized.store(true, std::memory_order_release);
        }
        return;
    }

    // Re-seed cpu_hz if PLL / system clock initialization finishes after early SysTick startup.
    if (cpu_hz != 0u && cpu_hz != g_init_cpu_hz && now_ms < kBootClockReinitWindowMs && try_enter_poll()) {
        if (init_agent(cpu_hz)) {
            g_init_cpu_hz = cpu_hz;
        }
        exit_poll();
    }

    malgrind_cortexm_sample_isr(0u, frame);

    // If user code calls malgrind_rp2040_poll() from main/loop, keep USB-CDC I/O out of SysTick ISR.
    if (g_has_main_poll.load(std::memory_order_acquire)) {
        const std::uint32_t since_main =
            static_cast<std::uint32_t>(now_ms - g_last_main_poll_ms.load(std::memory_order_relaxed));
        if (since_main < kMainPollLeaseMs) {
            return;
        }
    }

    const std::uint32_t last_poll = g_last_isr_poll_ms.load(std::memory_order_relaxed);
    if (static_cast<std::uint32_t>(now_ms - last_poll) >= kIsrPollGateMs && try_enter_poll()) {
        g_last_isr_poll_ms.store(now_ms, std::memory_order_relaxed);
        malgrind_cortexm_poll();
        exit_poll();
    }
}

#if defined(__arm__) || defined(__thumb__)
extern "C" void __real_isr_systick() noexcept;

// ARMv6-M Thumb-1-compatible trampoline extracting MSP/PSP ExceptionFrame before any prologue modifies
// SP/LR, invoking malgrind_rp2040_on_systick, then tail-chaining __real_isr_systick with original EXC_RETURN.
extern "C" [[gnu::naked, gnu::used]] void __wrap_isr_systick() noexcept {
    __asm__ volatile(
        ".syntax unified\n"
        "mov r0, lr\n"
        "movs r1, #4\n"
        "tst r1, r0\n"
        "bne 1f\n"
        "mrs r0, msp\n"
        "b 2f\n"
        "1:\n"
        "mrs r0, psp\n"
        "2:\n"
        "push {r3, lr}\n"
        "bl malgrind_rp2040_on_systick\n"
        "pop {r0, r1}\n"
        "mov lr, r1\n"
        "ldr r0, 3f\n"
        "bx r0\n"
        ".balign 4\n"
        "3:\n"
        ".word __real_isr_systick\n");
}
#endif

// Referenced via `-Wl,-u,malgrind_rp2040_autostart_link` to pull this object into the link.
extern "C" [[gnu::used]] void malgrind_rp2040_autostart_link() {}
