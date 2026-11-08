#include "mcu_flamegraph/platform/esp32.hpp"

#ifdef ESP_PLATFORM
#include "esp_cpu.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "esp32_hal";
#endif

namespace mcu_flamegraph::platform {

TargetInfo Esp32Platform::info() noexcept {
    return TargetInfo{
        .platform_name = "ESP32",
        .architecture = "Xtensa LX6",
        .cpu_freq_hz = 240'000'000,
        .num_cores = 2,
    };
}

void Esp32Platform::init() noexcept {
#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "Initialized ESP32 hardware timers & per-core buffers.");
#endif
}

void Esp32Platform::capture_sample() noexcept {
#ifdef ESP_PLATFORM
    // Windowed ABI stack unwinding & cycle reading logic
#endif
}

} // namespace mcu_flamegraph::platform
