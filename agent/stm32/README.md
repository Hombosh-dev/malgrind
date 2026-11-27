# STM32 Agent (`agent/stm32/`)

PlatformIO `ststm32` injection and runtime startup on top of `platforms/cortexm/`.

## Build Injection (`inject.py`)

`inject.py` runs as a PlatformIO SCons `pre:` script (`PLATFORMIO_EXTRA_SCRIPTS=pre:.../agent/stm32/inject.py`):

- Compiles `core/src` (`profiler.cpp`, `command.cpp`), `platforms/cortexm/src/cortexm.cpp`, and `agent/stm32/autostart.cpp` via `env.BuildSources(...)` into `$BUILD_DIR/malgrind/...` without modifying any project files.
- Resolves `MALGRIND_AGENT_DIR`, `MALGRIND_CORE_DIR`, and `MALGRIND_PLATFORMS_DIR`, falling back to sibling `../../core` and `../../platforms` so packaged app resources (`agent-src/`) work identically to the source tree.
- Passes `-Wl,--wrap=SysTick_Handler` and `-Wl,-u,malgrind_stm32_autostart_link`.

## Why `--wrap=SysTick_Handler`?

- **Exact `ExceptionFrame` capture + tick chaining**: `__wrap_SysTick_Handler` extracts the hardware-stacked `ExceptionFrame*` (`r0..r3, r12, lr, pc, xpsr`) from `MSP` or `PSP` using Thumb-1-safe instructions before any compiler prologue alters `SP`/`LR`, invokes `malgrind_cortexm_sample_isr(0, frame)`, and tail-chains `__real_SysTick_Handler` with the original `EXC_RETURN` so Cube HAL `uwTick`, Arduino `millis()`, and RTOS ticks continue undisturbed.
- **Why not weak `HAL_IncTick()`**: `HAL_IncTick()` is a nested C call inside `SysTick_Handler`; by the time it runs, `SP`/`LR` belong to `SysTick_Handler` rather than the interrupted application thread, preventing reliable `ExceptionFrame` capture across optimization levels, and non-HAL (Arduino / bare-metal) ticks would be missed.
- **Why not a dedicated `TIMx` timer**: `TIMx` peripheral availability, IRQn names, and clock enables differ across STM32 families and frequently conflict with user PWM/encoder/HAL-timebase timers. `SysTick` exists uniformly on every Cortex-M core (note: SysTick sampling follows the firmware's tick rate, typically 1000 Hz; custom projects needing an arbitrary prime timer can still call `malgrind_cortexm_sample_isr` from a dedicated `TIMx` ISR).

## Transport & Cooperative Polling (ST-Link SWD vs UART)

- **Agent stream (UART / ST-Link VCP)**: `autostart.cpp` initializes `malgrind_cortexm_init` lazily on the first SysTick (avoiding pre-`main` clock/peripheral ordering issues) and streams framed depth-2 samples over weak hooks `malgrind_stm32_write` / `malgrind_stm32_read` at `115200` baud (defaulting to Arduino `Serial` or STM32Cube HAL `huart2`/`huart3`/`huart1`/`hlpuart1`).
- **ISR vs main safety**: `malgrind_stm32_poll()` can be called from main/idle code to perform UART I/O outside interrupt context; when user code does not call it, a 50 ms gate in `SysTick` drives `malgrind_cortexm_poll()` under a `g_in_poll` re-entrancy guard.
- **Debug probe / flash ops (ST-Link SWD)**: Flash identification, backup (`read_flash`), restore (`write_flash`), and zero-flash (`no_flash`) profiling (`DWT PCSR` on M3/M4/M7/M33, `HaltResume` on M0/M0+) run over SWD/JTAG through OpenOCD (`tool-openocd`).
