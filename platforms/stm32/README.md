# STM32 / Cortex-M port

The shared Cortex-M runtime lives in `platforms/cortexm/` (`Arch::ArmV7M`) and is used by both STM32 and RP2040.

- **Exception frame capture:** A Thumb-1 naked trampoline extracts `ExceptionFrame*` from `MSP` or `PSP` before any compiler prologue alters `SP`/`LR`, then calls `malgrind_cortexm_sample_isr(core_id, frame)`.
- **Depth-2 (`pc` + `lr`):** Under `-mthumb`, compilers omit frame pointers by default and raw stack walks from an ISR risk faults on arbitrary words. Reading `pc` and `lr` from hardware exception stacking is O(1) and safe.
- **Cycle counter:** Cortex-M3/M4/M7/M33 uses `DWT->CYCCNT` (`DEMCR.TRCENA`). Cortex-M0/M0+ (`has_dwt_cyccnt = false`) scales a 1 us timer delta by `cpu_hz`.
- **Polling:** `malgrind_cortexm_poll()` processes `START`/`STOP`/`HELLO` commands, sends sleep beacons every 1000 ms, and flushes samples every 250 ms. In probe mode, OpenOCD can also use `DWT_PCSR` without halting the core for flat sampling.
