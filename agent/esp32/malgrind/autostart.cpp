// Starts the profiler by itself, so the user's firmware needs no malgrind call.
// A constructor runs before the scheduler, where malgrind_start() cannot wait, so it only creates a one-shot task.
#include "malgrind/platform/esp32.hpp"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef MALGRIND_RATE_HZ
#define MALGRIND_RATE_HZ 997 // prime: no aliasing with the RTOS tick
#endif

namespace {

void start_task(void*) {
    if (!malgrind_start(MALGRIND_RATE_HZ)) ESP_LOGE("malgrind", "agent failed to start");
    vTaskDelete(nullptr);
}

[[gnu::constructor]] void autostart() {
    xTaskCreate(start_task, "malgrind_boot", 4096, nullptr, configMAX_PRIORITIES - 2, nullptr);
}

} // namespace

// Referenced by `-u` in this component's CMakeLists.txt: pulls this object file into the link.
extern "C" void malgrind_autostart_link() {}
