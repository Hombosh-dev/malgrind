# MCU Flamegraph Profiler

A modular, high-performance C++20 profiling engine and Flamegraph generator designed for microcontrollers (ESP32, STM32, RP2040) and host simulation.

## Architecture

The project is structured into three clean decoupled layers:

1. **`core/`**: Platform-agnostic C++20 profiler engine.
2. **`platforms/`**: Hardware Abstraction Layer (HAL) backends:
   - `platforms/host/`: Native Linux / x86_64 mock backend for rapid development and testing without hardware.
   - `platforms/esp32/`: ESP32 (Xtensa LX6) backend utilizing hardware timers, cycle counters, and windowed stack unwinding.
   - `platforms/stm32/`: (Planned) STM32 ARM Cortex-M DWT & SysTick backend.
3. **`apps/` & `firmware/`**: Executable targets:
   - `apps/host_runner/`: Standalone desktop executable.
   - `firmware/esp32/`: Standalone ESP-IDF project ready for `idf.py build`.

```text
mcu-flamegraph/
├── CMakeLists.txt                 # Root CMake (builds Core + Platforms/Host + Host Apps)
├── core/                          # Pure C++20 Profiler Core
│   ├── CMakeLists.txt
│   ├── include/mcu_flamegraph/
│   │   ├── profiler.hpp           # Template Profiler<PlatformBackend>
│   │   └── target_info.hpp
│   └── src/profiler.cpp
├── platforms/                     # Platform HAL Implementations
│   ├── host/                      # Linux x86_64 simulation backend
│   │   ├── CMakeLists.txt
│   │   ├── include/mcu_flamegraph/platform/host.hpp
│   │   └── src/host.cpp
│   ├── esp32/                     # ESP32 Xtensa HAL backend
│   │   ├── CMakeLists.txt
│   │   ├── include/mcu_flamegraph/platform/esp32.hpp
│   │   └── src/esp32.cpp
│   └── stm32/                     # Future ARM Cortex-M backend
└── apps/
│   └── host_runner/               # Desktop runner (runs right now!)
│       ├── CMakeLists.txt
│       └── main.cpp
└── firmware/
    └── esp32/                     # ESP-IDF firmware project
        ├── CMakeLists.txt
        ├── sdkconfig.defaults
        └── main/
            ├── CMakeLists.txt
            └── main.cpp
```

## How to Build & Run

### 1. On Host (Linux x86_64)

Build and run immediately without embedded hardware:

```bash
cmake -B build
cmake --build build
./build/apps/host_runner/host_runner
```

### 2. On ESP32 Target (ESP-IDF)

Build and flash the dedicated ESP32 firmware:

```bash
cd firmware/esp32
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```
