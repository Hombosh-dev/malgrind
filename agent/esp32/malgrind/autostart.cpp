// Starts the profiler by itself, so the user's firmware needs no malgrind call.
// A constructor runs before the scheduler, where malgrind_start() cannot wait, so it only creates a one-shot task.
#include "malgrind/platform/esp32.hpp"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

// The rate comes from the sdkconfig (Kconfig.projbuild next to this file), which malgrind sets from the run settings.
#ifndef MALGRIND_RATE_HZ
#ifdef CONFIG_MALGRIND_RATE_HZ
#define MALGRIND_RATE_HZ CONFIG_MALGRIND_RATE_HZ
#else
#define MALGRIND_RATE_HZ 997 // prime: no aliasing with the RTOS tick
#endif
#endif

// Lets the host tell a flash image holding this agent from the user's own firmware (host/src/backup.hpp, kAgentMarker):
// a board that already runs malgrind must not have its agent saved as if it were the original.
extern "C" const char malgrind_agent_marker[] = "malgrind-agent-v1";

namespace {

void start_task(void*) {
    if (!malgrind_start(MALGRIND_RATE_HZ)) ESP_LOGE("malgrind", "agent failed to start");
    vTaskDelete(nullptr);
}

[[gnu::constructor]] void autostart() {
    // --gc-sections would drop the marker (only a reference keeps a section); the constructor is never dropped
    asm volatile("" : : "r"(malgrind_agent_marker));
    xTaskCreate(start_task, "malgrind_boot", 4096, nullptr, configMAX_PRIORITIES - 2, nullptr);
}

} // namespace

// Referenced by `-u` in this component's CMakeLists.txt: pulls this object file into the link.
extern "C" void malgrind_autostart_link() {}
