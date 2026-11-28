// Starts and drives the Cortex-M profiler agent on STM32 without modifying user files.
//
// Initialization runs lazily on the first SysTick interrupt rather than in a [[gnu::constructor]],
// because pre-main constructors execute before SystemClock_Config(), HAL_Init(), and peripheral setup.
// SysTick_Handler is wrapped via `-Wl,--wrap=SysTick_Handler` so we extract the hardware-stacked
// ExceptionFrame before any compiler prologue alters SP/LR and then tail-chain __real_SysTick_Handler.
#include "malgrind/platform/cortexm.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

#if defined(ARDUINO)
#include <Arduino.h>
#elif defined(USE_HAL_DRIVER)
#if __has_include("stm32f0xx_hal.h")
#include "stm32f0xx_hal.h"
#elif __has_include("stm32f1xx_hal.h")
#include "stm32f1xx_hal.h"
#elif __has_include("stm32f2xx_hal.h")
#include "stm32f2xx_hal.h"
#elif __has_include("stm32f3xx_hal.h")
#include "stm32f3xx_hal.h"
#elif __has_include("stm32f4xx_hal.h")
#include "stm32f4xx_hal.h"
#elif __has_include("stm32f7xx_hal.h")
#include "stm32f7xx_hal.h"
#elif __has_include("stm32g0xx_hal.h")
#include "stm32g0xx_hal.h"
#elif __has_include("stm32g4xx_hal.h")
#include "stm32g4xx_hal.h"
#elif __has_include("stm32h7xx_hal.h")
#include "stm32h7xx_hal.h"
#elif __has_include("stm32l0xx_hal.h")
#include "stm32l0xx_hal.h"
#elif __has_include("stm32l1xx_hal.h")
#include "stm32l1xx_hal.h"
#elif __has_include("stm32l4xx_hal.h")
#include "stm32l4xx_hal.h"
#elif __has_include("stm32l5xx_hal.h")
#include "stm32l5xx_hal.h"
#elif __has_include("stm32u5xx_hal.h")
#include "stm32u5xx_hal.h"
#elif __has_include("stm32wbxx_hal.h")
#include "stm32wbxx_hal.h"
#elif __has_include("stm32wlxx_hal.h")
#include "stm32wlxx_hal.h"
#endif
#endif

#ifndef MALGRIND_RATE_HZ
#define MALGRIND_RATE_HZ 997
#endif

#ifndef MALGRIND_START_SAMPLING
#define MALGRIND_START_SAMPLING 1
#endif

// Marker searched by the host (kStm32AgentMarkerV2) so flash backup skips images that already hold the agent.
extern "C" [[gnu::used]] const char malgrind_agent_marker[] = "malgrind-agent-v2";

// Weak hooks allowing firmware/board code to customize UART transport, clock/timebase, or polling.
extern "C" {
[[gnu::weak]] void malgrind_stm32_write(const std::uint8_t* data, std::size_t len);
[[gnu::weak]] std::size_t malgrind_stm32_read(std::uint8_t* dst, std::size_t cap);
[[gnu::weak]] std::uint32_t malgrind_stm32_cpu_hz();
[[gnu::weak]] std::uint32_t malgrind_stm32_millis();
void malgrind_stm32_poll() noexcept;
}

#if defined(USE_HAL_DRIVER) && defined(HAL_UART_MODULE_ENABLED)
extern "C" {
[[gnu::weak]] extern UART_HandleTypeDef huart2;
[[gnu::weak]] extern UART_HandleTypeDef huart3;
[[gnu::weak]] extern UART_HandleTypeDef huart1;
[[gnu::weak]] extern UART_HandleTypeDef hlpuart1;
[[gnu::weak]] UART_HandleTypeDef* malgrind_stm32_uart() {
    UART_HandleTypeDef* const candidates[] = {&huart2, &huart3, &huart1, &hlpuart1};
    for (UART_HandleTypeDef* h : candidates) {
        if (h != nullptr && h->Instance != nullptr && h->gState != HAL_UART_STATE_RESET) {
            return h;
        }
    }
    return nullptr;
}
}
#endif

#if defined(__arm__) || defined(__thumb__)
extern "C" [[gnu::weak]] extern std::uint32_t SystemCoreClock;
#endif

namespace {

constexpr std::uint32_t kIsrPollGateMs = 50;
constexpr std::uint32_t kMainPollLeaseMs = 500;
constexpr std::uint32_t kBootClockReinitWindowMs = 500;
constexpr std::uint16_t kInitRateHz =
    (MALGRIND_RATE_HZ) != 0 ? static_cast<std::uint16_t>(MALGRIND_RATE_HZ) : static_cast<std::uint16_t>(997);
constexpr bool kStartSampling = (MALGRIND_START_SAMPLING) != 0;

#if defined(__ARM_ARCH_6M__) || defined(__ARM_ARCH_8M_BASE__)
constexpr bool kHasDwtCyccnt = false;
#else
constexpr bool kHasDwtCyccnt = true;
#endif

std::atomic<std::uint32_t> g_tick_ms{0};
std::atomic<std::uint32_t> g_last_isr_poll_ms{0};
std::atomic<std::uint32_t> g_last_main_poll_ms{0};
std::atomic<bool> g_has_main_poll{false};
std::atomic<bool> g_initialized{false};
std::atomic<bool> g_in_poll{false};
std::uint32_t g_init_cpu_hz = 0;

void write_cb(void*, const std::uint8_t* data, std::size_t len) {
    if (malgrind_stm32_write != nullptr) {
        malgrind_stm32_write(data, len);
    }
}

std::size_t read_cb(void*, std::uint8_t* dst, std::size_t cap) {
    if (malgrind_stm32_read != nullptr) {
        return malgrind_stm32_read(dst, cap);
    }
    return 0;
}

std::uint32_t millis_cb(void*) {
    if (malgrind_stm32_millis != nullptr) {
        return malgrind_stm32_millis();
    }
    return g_tick_ms.load(std::memory_order_relaxed);
}

std::uint32_t detect_cpu_hz() noexcept {
    if (malgrind_stm32_cpu_hz != nullptr) {
        const std::uint32_t custom = malgrind_stm32_cpu_hz();
        if (custom != 0u) return custom;
    }
#if defined(__arm__) || defined(__thumb__)
    if (&SystemCoreClock != nullptr && SystemCoreClock != 0u) {
        return SystemCoreClock;
    }
#endif
#ifdef F_CPU
    return static_cast<std::uint32_t>(F_CPU);
#else
    return 0u;
#endif
}

bool init_agent(std::uint32_t cpu_hz) noexcept {
    CortexMConfig cfg{};
    cfg.target_name = "stm32";
    cfg.cpu_hz = cpu_hz;
    cfg.rate_hz = kInitRateHz;
    cfg.start_sampling = kStartSampling;
    cfg.num_cores = 1;
    cfg.has_dwt_cyccnt = kHasDwtCyccnt;
    cfg.ctx = nullptr;
    cfg.write_bytes = write_cb;
    cfg.read_bytes = read_cb;
    cfg.cycle_counter = nullptr;
    cfg.millis = millis_cb;
    cfg.on_state_change = nullptr;
    return malgrind_cortexm_init(cfg);
}

} // namespace

extern "C" [[gnu::weak]] void malgrind_stm32_write(const std::uint8_t* data, std::size_t len) {
    if (data == nullptr || len == 0u) return;
#if defined(ARDUINO) && !defined(NO_SERIAL)
    if (!Serial) return;
    Serial.write(data, len);
#elif defined(USE_HAL_DRIVER) && defined(HAL_UART_MODULE_ENABLED)
    UART_HandleTypeDef* const h = malgrind_stm32_uart();
    if (h == nullptr || h->Instance == nullptr || h->gState != HAL_UART_STATE_READY) return;
    std::size_t sent = 0;
    while (sent < len) {
        const std::size_t rem = len - sent;
        const std::uint16_t chunk = rem > 0xFFFFu ? static_cast<std::uint16_t>(0xFFFFu) : static_cast<std::uint16_t>(rem);
        if (HAL_UART_Transmit(h, const_cast<std::uint8_t*>(data + sent), chunk, 100u) != HAL_OK) {
            break;
        }
        sent += chunk;
    }
#else
    (void)data;
    (void)len;
#endif
}

extern "C" [[gnu::weak]] std::size_t malgrind_stm32_read(std::uint8_t* dst, std::size_t cap) {
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
#elif defined(USE_HAL_DRIVER) && defined(HAL_UART_MODULE_ENABLED)
    UART_HandleTypeDef* const h = malgrind_stm32_uart();
    if (h == nullptr || h->Instance == nullptr || h->RxState != HAL_UART_STATE_READY) return 0u;
    std::size_t n = 0;
    while (n < cap) {
        std::uint8_t b = 0;
        if (HAL_UART_Receive(h, &b, 1u, 0u) != HAL_OK) {
            break;
        }
        dst[n++] = b;
    }
    return n;
#else
    (void)dst;
    (void)cap;
    return 0u;
#endif
}

extern "C" void malgrind_stm32_poll() noexcept {
    if (!g_initialized.load(std::memory_order_acquire)) return;
    const std::uint32_t now_ms = g_tick_ms.load(std::memory_order_relaxed);
    g_last_main_poll_ms.store(now_ms, std::memory_order_relaxed);
    g_has_main_poll.store(true, std::memory_order_release);
    if (g_in_poll.exchange(true, std::memory_order_acq_rel)) return;
    malgrind_cortexm_poll();
    g_in_poll.store(false, std::memory_order_release);
}

extern "C" [[gnu::used]] void malgrind_stm32_on_systick(const ExceptionFrame* frame) noexcept {
#if defined(__arm__) || defined(__thumb__)
    // Keep malgrind_agent_marker live under --gc-sections.
    __asm__ volatile("" : : "r"(malgrind_agent_marker));
#else
    (void)malgrind_agent_marker;
#endif

    const std::uint32_t now_ms = g_tick_ms.fetch_add(1u, std::memory_order_relaxed) + 1u;
    const std::uint32_t cpu_hz = detect_cpu_hz();

    if (!g_initialized.load(std::memory_order_acquire)) {
        if (init_agent(cpu_hz)) {
            g_init_cpu_hz = cpu_hz;
            g_last_isr_poll_ms.store(now_ms, std::memory_order_relaxed);
            g_initialized.store(true, std::memory_order_release);
        }
        return;
    }

    // SystemClock_Config() often runs after HAL_Init() enables SysTick; update cpu_hz once the PLL locks.
    if (cpu_hz != 0u && cpu_hz != g_init_cpu_hz && now_ms < kBootClockReinitWindowMs &&
        !g_in_poll.exchange(true, std::memory_order_acq_rel)) {
        if (init_agent(cpu_hz)) {
            g_init_cpu_hz = cpu_hz;
        }
        g_in_poll.store(false, std::memory_order_release);
    }

    malgrind_cortexm_sample_isr(0u, frame);

    // If user code calls malgrind_stm32_poll() from main/idle, keep UART I/O out of SysTick ISR.
    if (g_has_main_poll.load(std::memory_order_acquire)) {
        const std::uint32_t since_main =
            static_cast<std::uint32_t>(now_ms - g_last_main_poll_ms.load(std::memory_order_relaxed));
        if (since_main < kMainPollLeaseMs) {
            return;
        }
    }

    const std::uint32_t last_poll = g_last_isr_poll_ms.load(std::memory_order_relaxed);
    if (static_cast<std::uint32_t>(now_ms - last_poll) >= kIsrPollGateMs) {
        if (!g_in_poll.exchange(true, std::memory_order_acq_rel)) {
            g_last_isr_poll_ms.store(now_ms, std::memory_order_relaxed);
            malgrind_cortexm_poll();
            g_in_poll.store(false, std::memory_order_release);
        }
    }
}

#if defined(__arm__) || defined(__thumb__)
extern "C" void __real_SysTick_Handler() noexcept;

// Thumb-1-compatible trampoline extracting MSP/PSP ExceptionFrame before any prologue modifies SP/LR,
// invoking malgrind_stm32_on_systick, then tail-chaining __real_SysTick_Handler with original EXC_RETURN.
extern "C" [[gnu::naked, gnu::used]] void __wrap_SysTick_Handler() noexcept {
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
        "bl malgrind_stm32_on_systick\n"
        "pop {r0, r1}\n"
        "mov lr, r1\n"
        "ldr r0, 3f\n"
        "bx r0\n"
        ".balign 4\n"
        "3:\n"
        ".word __real_SysTick_Handler\n");
}
#endif

// Referenced via `-Wl,-u,malgrind_stm32_autostart_link` to pull this object into the link.
extern "C" [[gnu::used]] void malgrind_stm32_autostart_link() {}
