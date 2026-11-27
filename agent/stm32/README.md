# STM32 Agent (`agent/stm32/`)

PlatformIO `ststm32` build injection and startup built on top of `platforms/cortexm/`.

- **Build (`inject.py`):** Runs as a `pre:` script (`PLATFORMIO_EXTRA_SCRIPTS`) and builds `core`, `cortexm`, and `autostart.cpp` into `$BUILD_DIR/malgrind/` without touching project sources. Passes `-Wl,--wrap=SysTick_Handler` and `-Wl,-u,malgrind_stm32_autostart_link`.
- **Wrapping `SysTick_Handler`:** Captures the hardware `ExceptionFrame` directly on interrupt entry and tail-chains `__real_SysTick_Handler` so HAL/Arduino/RTOS ticks keep working normally. Also avoids guessing `TIMx` peripherals across different STM32 chips.
- **Transport & SWD:** Initializes lazily on the first SysTick and streams depth-2 samples at 115200 baud via weak hooks `malgrind_stm32_write` / `malgrind_stm32_read` (call `malgrind_stm32_poll()` from your loop, or let the 50 ms guarded gate in SysTick handle it). Flash backup, restore, and no-flash profiling use OpenOCD over SWD.
