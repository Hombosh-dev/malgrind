#include "mcu_flamegraph/profiler.hpp"
#include "mcu_flamegraph/platform/esp32.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "esp32_app";

extern "C" void app_main() {
    ESP_LOGI(TAG, "Starting MCU Flamegraph on ESP32...");

    // Compile-time binding of core engine to the ESP32 hardware platform
    using ProfilerEsp32 = mcu_flamegraph::Profiler<mcu_flamegraph::platform::Esp32Platform>;

    ProfilerEsp32::init();
    ProfilerEsp32::run_benchmark_cycle();

    ESP_LOGI(TAG, "ESP32 execution finished.");
}
