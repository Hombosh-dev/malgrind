#pragma once

// Freestanding Cortex-M port (ARMv6-M / ARMv7-M / ARMv8-M, no vendor SDK, no RTOS, no heap).
//
// Stack capture rationale (leaf + LR, depth 2):
//   On ARM Thumb, compilers do not maintain a uniform frame pointer across optimization levels:
//   -fomit-frame-pointer is default at -O1/-O2/-Os, GCC Thumb uses r7 while AAPCS/Clang may use
//   r11, and -mapcs-frame is unsupported/unreliable for Thumb. Walking raw stack words from a
//   ~1 kHz sampling ISR without DWARF .ARM.exidx tables risks HardFaults on arbitrary registers,
//   while full .ARM.exidx unwinding inside an ISR is too slow for Cortex-M0+/M3/M4.
//   Instead, hardware exception stacking places the exact interrupted PC and LR in the 8-word
//   ExceptionFrame on MSP or PSP, providing deterministic O(1) depth-2 attribution (leaf +
//   immediate caller, filtering out ARM EXC_RETURN 0xFxxxxxxx sentinels).

#include <cstddef>
#include <cstdint>

namespace malgrind::platform {

// Hardware-stacked Cortex-M basic exception frame (MSP or PSP, 8 words = 32 bytes).
struct ExceptionFrame {
    std::uint32_t r0, r1, r2, r3, r12, lr, pc, xpsr;
};

static_assert(sizeof(ExceptionFrame) == 32, "Cortex-M ExceptionFrame must be 8 words");

struct CortexMConfig {
    const char* target_name = "cortexm";
    std::uint32_t cpu_hz = 0;
    std::uint16_t rate_hz = 997;
    bool start_sampling = true;
    std::uint8_t num_cores = 1;
    // True for Cortex-M3/M4/M7/M33 (uses DWT->CYCCNT when cycle_counter is nullptr).
    // False for Cortex-M0/M0+ (cycle_counter reads a 1 us hardware timer, scaled using cpu_hz).
    bool has_dwt_cyccnt = true;

    void* ctx = nullptr;
    void (*write_bytes)(void* ctx, const std::uint8_t* data, std::size_t len) = nullptr;
    std::size_t (*read_bytes)(void* ctx, std::uint8_t* dst, std::size_t cap) = nullptr;
    std::uint32_t (*cycle_counter)(void* ctx) = nullptr;
    std::uint32_t (*millis)(void* ctx) = nullptr;
    void (*on_state_change)(void* ctx, bool sampling, std::uint16_t rate_hz) = nullptr;
};

} // namespace malgrind::platform

using malgrind::platform::CortexMConfig;
using malgrind::platform::ExceptionFrame;

extern "C" {

// Initializes (or re-initializes) the Cortex-M profiler agent. Returns false if config is invalid.
bool malgrind_cortexm_init(const malgrind::platform::CortexMConfig& cfg) noexcept;

// Records one depth-2 sample from the hardware exception frame on `core_id`.
// Safe no-op if uninitialized, sleeping/paused, or `frame == nullptr`.
void malgrind_cortexm_sample_isr(std::uint8_t core_id, const malgrind::platform::ExceptionFrame* frame) noexcept;

// Cooperative poll: reads RX commands, emits sleeping HELLO beacons (1000 ms),
// and flushes buffered sample frames (250 ms).
void malgrind_cortexm_poll() noexcept;

} // extern "C"

namespace malgrind::platform {

using ::malgrind_cortexm_init;
using ::malgrind_cortexm_poll;
using ::malgrind_cortexm_sample_isr;

} // namespace malgrind::platform

#if defined(__arm__) || defined(__thumb__)

#define MALGRIND_CORTEXM_STR_INNER(x) #x
#define MALGRIND_CORTEXM_STR(x) MALGRIND_CORTEXM_STR_INNER(x)

// Defines a naked Cortex-M exception handler using Thumb-1-compatible instructions only
// (M0/M0+ safe: uses `tst r1, r0; bne ...` rather than Thumb-2-only `ite`) that extracts the
// hardware-stacked ExceptionFrame* from MSP (EXC_RETURN bit 2 == 0) or PSP (bit 2 == 1)
// before any compiler prologue modifies SP or LR, then calls `target_fn(core_id, frame)`.
// `target_fn` must have C linkage (`extern "C"`).
#define MALGRIND_CORTEXM_DEFINE_ISR(isr_name, core_id, target_fn)                                \
    extern "C" [[gnu::naked]] void isr_name() noexcept {                                         \
        __asm__ volatile(                                                                        \
            ".syntax unified\n"                                                                  \
            "mov r0, lr\n"                                                                       \
            "movs r1, #4\n"                                                                      \
            "tst r1, r0\n"                                                                       \
            "bne 1f\n"                                                                           \
            "mrs r1, msp\n"                                                                      \
            "b 2f\n"                                                                             \
            "1:\n"                                                                               \
            "mrs r1, psp\n"                                                                      \
            "2:\n"                                                                               \
            "push {r4, lr}\n"                                                                    \
            "movs r0, #" MALGRIND_CORTEXM_STR(core_id) "\n"                                      \
            "bl " MALGRIND_CORTEXM_STR(target_fn) "\n"                                           \
            "pop {r4, pc}\n");                                                                   \
    }

// Ready-to-use naked SysTick/timer ISR trampoline for core 0 calling `malgrind_cortexm_sample_isr`.
// Uses Thumb-1-compatible instructions only so it compiles on M0/M0+ as well as M3/M4/M7/M33.
extern "C" [[gnu::naked]] inline void malgrind_cortexm_systick_isr() noexcept {
    __asm__ volatile(
        ".syntax unified\n"
        "mov r0, lr\n"
        "movs r1, #4\n"
        "tst r1, r0\n"
        "bne 1f\n"
        "mrs r1, msp\n"
        "b 2f\n"
        "1:\n"
        "mrs r1, psp\n"
        "2:\n"
        "push {r4, lr}\n"
        "movs r0, #0\n"
        "bl malgrind_cortexm_sample_isr\n"
        "pop {r4, pc}\n");
}

#endif // defined(__arm__) || defined(__thumb__)
