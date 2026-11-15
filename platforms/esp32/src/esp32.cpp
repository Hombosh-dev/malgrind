#include "malgrind/platform/esp32.hpp"

#include "malgrind/profiler.hpp"

#include "driver/gptimer.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_cpu.h"
#include "esp_cpu_utils.h"
#include "esp_debug_helpers.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

using namespace malgrind;

constexpr const char* kTag = "malgrind";
constexpr uart_port_t kUart = UART_NUM_0;
constexpr std::uint32_t kFlushMs = 250;
// Innermost frames that belong to the sampler, not to the program: gptimer_default_isr and
// _xt_lowint1 (seen in raw stacks on ESP-IDF 5.3.1). If an IDF update changes the interrupt path,
// the viewer shows the extra frames at the leaves; adjust with -DMALGRIND_ESP32_SKIP=n.
#ifndef MALGRIND_ESP32_SKIP
#define MALGRIND_ESP32_SKIP 2
#endif
constexpr unsigned kSkip = MALGRIND_ESP32_SKIP;

// Interrupt entry/exit (_xt_lowint1 context save, gptimer_default_isr, return) runs outside the
// callback and cannot be timed from inside it. Calibration: ~1400 cycles at 160 MHz, found by A/B
// against the baseline firmware (see README, "measuring overhead"). Recalibrate if IDF or the
// CPU clock changes.
#ifndef MALGRIND_ESP32_DISPATCH_CYCLES
#define MALGRIND_ESP32_DISPATCH_CYCLES 1400
#endif

Profiler* g_profiler;
volatile unsigned g_timers_ready;

// Runs in the timer interrupt of the core it samples. Everything it touches is IRAM or DRAM.
bool IRAM_ATTR on_alarm(gptimer_handle_t, const gptimer_alarm_event_data_t*, void*) {
    const std::uint32_t t0 = esp_cpu_get_cycle_count();
    esp_backtrace_frame_t frame;
    esp_backtrace_get_start(&frame.pc, &frame.sp, &frame.next_pc);
    frame.exc_frame = nullptr;

    std::uint32_t pcs[kMaxDepth];
    unsigned n = 0, seen = 0;
    bool ok = esp_stack_ptr_is_sane(frame.sp);
    while (ok && n < kMaxDepth) {
        if (seen++ >= kSkip) pcs[n++] = esp_cpu_process_stack_pc(frame.pc);
        if (frame.next_pc == 0 || !esp_backtrace_get_next_frame(&frame)) break;
    }
    g_profiler->record(xPortGetCoreID(), pcs, n, esp_cpu_get_cycle_count() - t0 + MALGRIND_ESP32_DISPATCH_CYCLES);
    return false;
}

// One of these runs pinned to each core: the timer interrupt is allocated on the core that enables it.
struct TimerJob {
    unsigned core;
    std::uint16_t rate_hz;
};

void timer_task(void* arg) {
    const TimerJob job = *static_cast<TimerJob*>(arg);
    gptimer_handle_t timer = nullptr;
    gptimer_config_t cfg{};
    cfg.clk_src = GPTIMER_CLK_SRC_DEFAULT;
    cfg.direction = GPTIMER_COUNT_UP;
    cfg.resolution_hz = 1'000'000;
    gptimer_alarm_config_t alarm{};
    alarm.alarm_count = 1'000'000u / job.rate_hz;
    alarm.flags.auto_reload_on_alarm = true;
    gptimer_event_callbacks_t cbs{};
    cbs.on_alarm = on_alarm;
    if (gptimer_new_timer(&cfg, &timer) == ESP_OK && gptimer_set_alarm_action(timer, &alarm) == ESP_OK &&
        gptimer_register_event_callbacks(timer, &cbs, nullptr) == ESP_OK && gptimer_enable(timer) == ESP_OK &&
        gptimer_start(timer) == ESP_OK) {
        ESP_LOGI(kTag, "sampling core %u at %u Hz", job.core, 1'000'000u / (1'000'000u / job.rate_hz));
    } else {
        ESP_LOGE(kTag, "timer setup failed on core %u", job.core);
    }
    g_timers_ready = g_timers_ready + 1;
    vTaskDelete(nullptr);
}

void flusher_task(void*) {
    std::uint32_t flush_cycles = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(kFlushMs));
        const std::uint32_t start = esp_cpu_get_cycle_count();
        std::uint32_t waiting = 0; // time spent blocked in the UART or in settle: not CPU cost
        g_profiler->flush(
            [&](const std::uint8_t* d, std::size_t n) {
                const std::uint32_t t = esp_cpu_get_cycle_count();
                uart_write_bytes(kUart, d, n); // blocks only if the TX ring buffer is full
                waiting += esp_cpu_get_cycle_count() - t;
            },
            [&] {
                const std::uint32_t t = esp_cpu_get_cycle_count();
                vTaskDelay(2 + pdMS_TO_TICKS(5)); // longer than one sample period on any tick rate
                waiting += esp_cpu_get_cycle_count() - t;
            },
            flush_cycles);
        flush_cycles = esp_cpu_get_cycle_count() - start - waiting;
    }
}

} // namespace

extern "C" bool malgrind_start(std::uint16_t rate_hz) {
    static bool started = false; // the agent starts itself; a second call (from user code) must not start it twice
    if (started) return true;
    started = true;
    static Profiler profiler({"ESP32", Arch::Xtensa, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1'000'000u, portNUM_PROCESSORS},
                             rate_hz);
    g_profiler = &profiler;

    // Console + frames share UART0 through the interrupt-driven driver; plain stdout would busy-wait.
    if (!uart_is_driver_installed(kUart)) uart_driver_install(kUart, 256, 4096, 0, nullptr, 0);
    uart_vfs_dev_use_driver(kUart);

    static TimerJob jobs[portNUM_PROCESSORS];
    for (unsigned core = 0; core < portNUM_PROCESSORS; ++core) {
        jobs[core] = {core, rate_hz};
        if (xTaskCreatePinnedToCore(timer_task, "malgrind_tmr", 3072, &jobs[core], configMAX_PRIORITIES - 1, nullptr,
                                    core) != pdPASS)
            return false;
    }
    while (g_timers_ready < portNUM_PROCESSORS) vTaskDelay(1);
    return xTaskCreatePinnedToCore(flusher_task, "malgrind_flush", 4096, nullptr, 1, nullptr, 0) == pdPASS;
}
