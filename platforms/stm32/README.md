# STM32 / Cortex-M port

The shared Cortex-M runtime lives in [`platforms/cortexm/`](../cortexm/) (`Arch::ArmV7M`), and is shared by STM32 (Cortex-M0/M0+/M3/M4/M7/M33) and RP2040 (Cortex-M0+).

## Design

- **Exception entry & timer ISR:** A periodic timer (SysTick or `TIMx`) uses the Thumb-1-compatible naked trampoline (`malgrind_cortexm_systick_isr` / `MALGRIND_CORTEXM_DEFINE_ISR`) to extract the hardware-stacked `ExceptionFrame*` from `MSP` or `PSP` before any compiler prologue alters `SP`/`LR`, then calls `malgrind_cortexm_sample_isr(core_id, frame)`.
- **Why leaf + LR (depth 2) instead of Thumb frame-pointer walks:**
  - Under `-mthumb`, compilers omit frame pointers by default (`-fomit-frame-pointer` at `-O1`/`-O2`/`-Os`), GCC Thumb uses `r7` whereas AAPCS/Clang may use `r11`, and `-mapcs-frame` is unsupported/unreliable for Thumb.
  - Walking raw stack words from a ~1 kHz ISR without full DWARF `.ARM.exidx` tables risks `HardFault`/`MemManage` faults on arbitrary stack data or general-purpose uses of `r7`/`r11`, while full `.ARM.exidx` unwinding inside an ISR is too slow for Cortex-M0+/M3/M4.
  - Hardware exception stacking guarantees the exact interrupted `pc` (`pc & ~1u`) and `lr` (`lr & ~1u`, filtering out `0` and ARM `EXC_RETURN` `0xFxxxxxxx` sentinels) in O(1) time with zero risk of fault.
- **Cycle timing:** On Cortex-M3/M4/M7/M33 (`has_dwt_cyccnt = true`), `malgrind_cortexm_init` enables `CoreDebug->DEMCR.TRCENA` and `DWT->CYCCNT` (unlocking `DWT->LAR` on M7). On Cortex-M0/M0+ (`has_dwt_cyccnt = false`), `cycle_counter` reads a 1 µs hardware timer and scales elapsed µs to CPU cycles using `cpu_hz` in 64-bit arithmetic.
- **Cooperative polling & Agent v2:** `malgrind_cortexm_poll()` drains RX bytes through `CommandParser` (`START`/`STOP`/`HELLO`), invokes `on_state_change` so board code can start/stop/reconfigure the sampling timer, emits sleeping `HELLO` beacons every 1000 ms, and flushes `Profiler` frames every 250 ms via `write_bytes` (UART or USB-CDC).

## STM32 integration (`agent/stm32/`)

STM32 PlatformIO build injection (`agent/stm32/` with `env.BuildSources` and constructor-free startup) and the host adapter (`host/src/families/stm32.cpp` with `ststm32` support and OpenOCD flash backup/restore) build on top of `platforms/cortexm/` (`T10.2`).

Zero-overhead alternative for probe mode: PC sampling through `DWT_PCSR` (no halt) or SWO (see orbuculum), which provides a flat profile only.
