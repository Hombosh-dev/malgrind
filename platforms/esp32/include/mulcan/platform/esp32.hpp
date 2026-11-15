#pragma once

#include <cstdint>

// ESP32 (Xtensa, ESP-IDF) port. One call from app_main() starts everything:
//   - UART0 driver with a TX ring buffer (frames and ESP_LOG share it, interrupt driven)
//   - one gptimer per core that samples the interrupted stack in its ISR
//   - a low-priority task on core 0 that sends one frame per core every 250 ms
// Returns false if something could not be set up (details are logged).
extern "C" bool mulcan_start(std::uint16_t rate_hz);
