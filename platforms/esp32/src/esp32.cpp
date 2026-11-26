#include "malgrind/platform/esp32.hpp"

#include "malgrind/command.hpp"
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

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace {

using namespace malgrind;

constexpr const char* kTag = "malgrind";
constexpr uart_port_t kUart = UART_NUM_0;
constexpr std::uint32_t kFlushMs = 250;
constexpr std::uint32_t kHelloMs = 1000;
constexpr std::uint32_t kPollMs = 50;
constexpr std::uint16_t kAgentVersion = 2;
constexpr std::uint16_t kMinRateHz = 10;
constexpr std::uint16_t kMaxRateHz = 5000;
constexpr std::uint16_t kDefaultRateHz = 997;

#ifdef __riscv
constexpr Arch kArch = Arch::RiscV32;
#else
constexpr Arch kArch = Arch::Xtensa;
#endif

static_assert(portNUM_PROCESSORS <= kMaxCores, "portNUM_PROCESSORS exceeds kMaxCores");

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

Profiler* g_profiler = nullptr;
std::atomic<bool> g_sampling{false};
gptimer_handle_t g_timers[kMaxCores]{};
bool g_timer_running[kMaxCores]{};
std::atomic<unsigned> g_timers_ready{0};
std::uint16_t g_default_rate_hz = kDefaultRateHz;

constexpr std::uint16_t clamp_rate(std::uint16_t rate_hz) noexcept {
    if (rate_hz < kMinRateHz) return kMinRateHz;
    if (rate_hz > kMaxRateHz) return kMaxRateHz;
    return rate_hz;
}

std::uint32_t elapsed_ms(TickType_t now, TickType_t since) noexcept {
    return static_cast<std::uint32_t>(pdTICKS_TO_MS(static_cast<TickType_t>(now - since)));
}

// Runs in the timer interrupt of the core it samples. Everything it touches is IRAM or DRAM.
bool IRAM_ATTR on_alarm(gptimer_handle_t, const gptimer_alarm_event_data_t*, void*) {
    if (!g_sampling.load(std::memory_order_acquire)) return false;
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
    bool start_sampling;
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
        gptimer_register_event_callbacks(timer, &cbs, nullptr) == ESP_OK && gptimer_enable(timer) == ESP_OK) {
        g_timers[job.core] = timer;
        if (job.start_sampling) {
            if (gptimer_start(timer) == ESP_OK) {
                g_timer_running[job.core] = true;
                ESP_LOGI(kTag, "sampling core %u at %u Hz", job.core, 1'000'000u / (1'000'000u / job.rate_hz));
            } else {
                ESP_LOGE(kTag, "timer start failed on core %u", job.core);
            }
        } else {
            ESP_LOGI(kTag, "timer ready on core %u (sleeping)", job.core);
        }
    } else {
        if (timer != nullptr) gptimer_del_timer(timer);
        ESP_LOGE(kTag, "timer setup failed on core %u", job.core);
    }
    g_timers_ready.fetch_add(1, std::memory_order_release);
    vTaskDelete(nullptr);
}

void send_hello(std::uint16_t seq, bool sampling, std::uint32_t analyses) {
    const TargetInfo& info = g_profiler->info();
    std::uint8_t buf[64];
    const HelloPayload hello{
        .seq = seq,
        .arch = info.arch,
        .ncores = info.num_cores,
        .cpu_hz = info.cpu_hz,
        .agent_ver = kAgentVersion,
        .caps = kCapSamples,
        .state = static_cast<std::uint8_t>(sampling ? 1 : 0),
        .rate_hz = g_profiler->rate_hz(),
        .analyses = analyses,
    };
    const std::size_t n = encode_hello(buf, sizeof(buf), hello);
    if (n > 0) uart_write_bytes(kUart, buf, n);
}

void stop_timers() {
    for (unsigned core = 0; core < portNUM_PROCESSORS; ++core) {
        if (g_timers[core] && g_timer_running[core]) {
            if (gptimer_stop(g_timers[core]) != ESP_OK) {
                ESP_LOGE(kTag, "timer stop failed on core %u", core);
            }
            g_timer_running[core] = false;
        }
    }
}

void configure_timers(std::uint16_t rate_hz) {
    gptimer_alarm_config_t alarm{};
    alarm.alarm_count = 1'000'000u / rate_hz;
    alarm.reload_count = 0;
    alarm.flags.auto_reload_on_alarm = true;
    for (unsigned core = 0; core < portNUM_PROCESSORS; ++core) {
        if (!g_timers[core]) continue;
        // Reset counter before lowering alarm_count so the counter cannot sit past the new threshold.
        gptimer_set_raw_count(g_timers[core], 0);
        if (gptimer_set_alarm_action(g_timers[core], &alarm) != ESP_OK) {
            ESP_LOGE(kTag, "timer alarm update failed on core %u", core);
        }
    }
}

void start_timers() {
    for (unsigned core = 0; core < portNUM_PROCESSORS; ++core) {
        if (g_timers[core] && !g_timer_running[core]) {
            if (gptimer_start(g_timers[core]) == ESP_OK) {
                g_timer_running[core] = true;
            } else {
                ESP_LOGE(kTag, "timer start failed on core %u", core);
            }
        }
    }
}

void flush_samples(std::uint32_t& flush_cycles) {
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

void flusher_task(void*) {
    bool sampling = g_sampling.load(std::memory_order_acquire);
    std::uint32_t analyses = sampling ? kCapSamples : 0;
    std::uint16_t hello_seq = 0;
    std::uint32_t flush_cycles = 0;
    CommandParser parser;

    const TickType_t poll_ticks = pdMS_TO_TICKS(kPollMs) > 0 ? pdMS_TO_TICKS(kPollMs) : 1;
    TickType_t last_flush = xTaskGetTickCount();
    TickType_t last_hello = last_flush;

    send_hello(hello_seq++, sampling, analyses);

    for (;;) {
        vTaskDelay(poll_ticks);

        std::uint8_t rx[64];
        for (int reads = 0; reads < 4; ++reads) {
            const int n = uart_read_bytes(kUart, rx, static_cast<std::uint32_t>(sizeof(rx)), 0);
            if (n <= 0) break;
            parser.feed(rx, static_cast<std::size_t>(n), [&](const Command& cmd) {
                switch (cmd.cmd) {
                    case kCmdStart: {
                        const std::uint16_t rate = clamp_rate(cmd.rate_hz ? cmd.rate_hz : g_default_rate_hz);
                        analyses = cmd.analyses ? (cmd.analyses & kCapSamples) : kCapSamples;
                        if (sampling) {
                            g_sampling.store(false, std::memory_order_release);
                            stop_timers();
                            vTaskDelay(2); // let any in-flight ISR exit before clearing tables
                        }
                        configure_timers(rate);
                        g_profiler->clear();
                        g_profiler->set_rate(rate);
                        sampling = true;
                        g_sampling.store(true, std::memory_order_release);
                        start_timers();
                        last_flush = xTaskGetTickCount();
                        send_hello(hello_seq++, sampling, analyses);
                        break;
                    }
                    case kCmdStop: {
                        if (sampling) {
                            sampling = false;
                            g_sampling.store(false, std::memory_order_release);
                            stop_timers();
                            vTaskDelay(2); // let any in-flight ISR exit before clearing tables
                            g_profiler->clear();
                        }
                        analyses = 0;
                        last_hello = xTaskGetTickCount();
                        send_hello(hello_seq++, sampling, analyses);
                        break;
                    }
                    case kCmdHello: {
                        last_hello = xTaskGetTickCount();
                        send_hello(hello_seq++, sampling, analyses);
                        break;
                    }
                    default:
                        break;
                }
            });
            if (n < static_cast<int>(sizeof(rx))) break;
        }

        const TickType_t now = xTaskGetTickCount();
        if (sampling) {
            if (elapsed_ms(now, last_flush) >= kFlushMs) {
                flush_samples(flush_cycles);
                last_flush = xTaskGetTickCount();
            }
        } else {
            if (elapsed_ms(now, last_hello) >= kHelloMs) {
                send_hello(hello_seq++, sampling, analyses);
                last_hello = xTaskGetTickCount();
            }
        }
    }
}

} // namespace

extern "C" bool malgrind_start_agent(std::uint16_t rate_hz, bool start_sampling) {
    static bool started = false; // the agent starts itself; a second call (from user code) must not start it twice
    if (started) return true;
    started = true;

    const std::uint16_t init_rate = clamp_rate(rate_hz ? rate_hz : kDefaultRateHz);
    g_default_rate_hz = init_rate;
    static Profiler profiler({"ESP32", kArch, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1'000'000u, portNUM_PROCESSORS},
                             init_rate);
    g_profiler = &profiler;
    g_sampling.store(start_sampling, std::memory_order_release);

    // Console + frames share UART0 through the interrupt-driven driver; plain stdout would busy-wait.
    if (!uart_is_driver_installed(kUart) && uart_driver_install(kUart, 256, 4096, 0, nullptr, 0) != ESP_OK) {
        ESP_LOGE(kTag, "UART0 driver install failed");
        return false;
    }
    uart_vfs_dev_use_driver(kUart);

    static TimerJob jobs[portNUM_PROCESSORS];
    for (unsigned core = 0; core < portNUM_PROCESSORS; ++core) {
        jobs[core] = {core, init_rate, start_sampling};
        if (xTaskCreatePinnedToCore(timer_task, "malgrind_tmr", 3072, &jobs[core], configMAX_PRIORITIES - 1, nullptr,
                                    core) != pdPASS)
            return false;
    }
    while (g_timers_ready.load(std::memory_order_acquire) < portNUM_PROCESSORS) vTaskDelay(1);
    return xTaskCreatePinnedToCore(flusher_task, "malgrind_flush", 4096, nullptr, 1, nullptr, 0) == pdPASS;
}

extern "C" bool malgrind_start(std::uint16_t rate_hz) {
    return malgrind_start_agent(rate_hz, true);
}
