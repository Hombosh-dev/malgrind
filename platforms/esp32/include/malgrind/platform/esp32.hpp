#pragma once

#include <cstdint>

// ESP32 (Xtensa / RISC-V, ESP-IDF) port. One call starts everything:
//   - UART0 driver with RX/TX ring buffers (frames, HELLO beacons, commands, and ESP_LOG share it)
//   - one gptimer per core that samples the interrupted stack in its ISR when sampling is active
//   - a low-priority task on core 0 that polls UART0 RX for MLC1 commands (START, STOP, HELLO) every 50 ms:
//       * while active (`start_sampling == true` or after START): flushes sample frames every 250 ms when
//         kCapSamples is enabled, and emits HEAP / TASKS (capped at 32 entries) frames every 1000 ms when enabled
//       * while sleeping (`start_sampling == false` or after STOP): timers remain stopped (0 ISR overhead)
//         and a HELLO beacon (state = 0) is emitted every 1000 ms until a START command arrives
// Returns false if something could not be set up (details are logged). Subsequent calls are no-ops returning true.
extern "C" bool malgrind_start_agent(std::uint16_t rate_hz, bool start_sampling);

// Convenience entry point that starts sampling immediately (`start_sampling = true`).
extern "C" bool malgrind_start(std::uint16_t rate_hz);
