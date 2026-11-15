// Demo firmware: a fake "sensor node" with nested calls, so the flamegraph has some depth.
//   core 0 "app":   read_sensors -> process_signal (fir_filter, fft -> butterfly -> twiddle) -> compress, then sleeps
//   core 1 "radio": handle_packet -> parse_packet -> crc_check, build_reply -> encrypt, then sleeps
// Build the "baseline" env (-DDEMO_PROFILER=0) and compare the iterations/s log line for real overhead.
#include <cstdint>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef DEMO_PROFILER
#define DEMO_PROFILER 1
#endif
#ifndef DEMO_RATE_HZ
#define DEMO_RATE_HZ 997 // prime: no aliasing with the RTOS tick
#endif
#if DEMO_PROFILER
#include "malgrind/platform/esp32.hpp"
#endif

namespace {

constexpr const char* kTag = "demo";
volatile std::uint32_t g_sink;
volatile std::uint32_t g_iterations[2]; // inner-loop iterations per core, a profiler-independent speed gauge

// Burns CPU for `ms` milliseconds; a real (not inlined) function so it shows as the leaf of every busy path.
[[gnu::noinline]] void spin(int ms) {
    const unsigned core = xPortGetCoreID();
    for (const int64_t end = esp_timer_get_time() + ms * 1000; esp_timer_get_time() < end;) {
        for (int i = 0; i < 500; ++i) g_sink = g_sink + 1;
        g_iterations[core] = g_iterations[core] + 500;
    }
}

// ---- core 0: sensor processing ------------------------------------------------------------
[[gnu::noinline]] void adc_read() { spin(4); }
[[gnu::noinline]] void i2c_transfer() { spin(6); }
[[gnu::noinline]] void read_sensors() {
    for (int i = 0; i < 3; ++i) adc_read();
    i2c_transfer();
}
[[gnu::noinline]] void mac_loop() { spin(30); }
[[gnu::noinline]] void fir_filter() {
    mac_loop();
    spin(5);
}
[[gnu::noinline]] void twiddle() { spin(7); }
[[gnu::noinline]] void butterfly() {
    spin(15);
    twiddle();
}
[[gnu::noinline]] void fft() {
    butterfly();
    butterfly();
}
[[gnu::noinline]] void process_signal() {
    fir_filter();
    fft();
}
[[gnu::noinline]] void huffman_encode() { spin(20); }
[[gnu::noinline]] void compress() {
    huffman_encode();
    spin(3);
}

void app_task(void*) {
    for (;;) {
        read_sensors();
        process_signal();
        compress();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ---- core 1: "radio" ----------------------------------------------------------------------
[[gnu::noinline]] void crc_check() { spin(10); }
[[gnu::noinline]] void parse_packet() {
    spin(6);
    crc_check();
}
[[gnu::noinline]] void encrypt() { spin(14); }
[[gnu::noinline]] void build_reply() {
    encrypt();
    spin(4);
}
[[gnu::noinline]] void handle_packet() {
    parse_packet();
    build_reply();
}

void radio_task(void*) {
    for (;;) {
        handle_packet();
        vTaskDelay(pdMS_TO_TICKS(70));
    }
}

} // namespace

extern "C" void app_main() {
#if DEMO_PROFILER
    if (!malgrind_start(DEMO_RATE_HZ)) ESP_LOGE(kTag, "malgrind_start failed");
#endif
    xTaskCreatePinnedToCore(app_task, "app", 4096, nullptr, 5, nullptr, 0);
    xTaskCreatePinnedToCore(radio_task, "radio", 4096, nullptr, 5, nullptr, 1);

    std::uint32_t last[2] = {};
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(kTag, "iterations/s core0=%lu core1=%lu", (g_iterations[0] - last[0]) / 5, (g_iterations[1] - last[1]) / 5);
        last[0] = g_iterations[0];
        last[1] = g_iterations[1];
    }
}
