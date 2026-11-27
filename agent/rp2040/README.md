# RP2040 Agent (`agent/rp2040/`)

PlatformIO `raspberrypi` build injection and startup built on top of `platforms/cortexm/`.

- **Build (`inject.py`):** Runs as a `pre:` script (`PLATFORMIO_EXTRA_SCRIPTS`) and compiles `core`, `cortexm`, and `autostart.cpp` into `$BUILD_DIR/malgrind/`. Passes `-Wl,--wrap=isr_systick` and `-Wl,-u,malgrind_rp2040_autostart_link`.
- **Timer & wrapping:** `__wrap_isr_systick` extracts `ExceptionFrame*` with ARMv6-M Thumb-1 instructions and chains `__real_isr_systick`. Because Cortex-M0+ lacks `DWT->CYCCNT`, `autostart.cpp` reads the 1 us `TIMER->TIMERAWL` (`0x40054028`) counter and scales deltas using `cpu_hz`.
- **USB-CDC & polling:** Runs lazy init on first SysTick (after TinyUSB/stdio setup) and writes over `malgrind_rp2040_write` / `malgrind_rp2040_read`. Call `malgrind_rp2040_poll()` in `loop()`/`main()`, or fall back to the `PRIMASK`-guarded 50 ms SysTick gate.
- **Flash & probe mode:** Uses OpenOCD SWD (`0x10000000`) when a debug probe is attached, or `picotool` in BOOTSEL mode as a fallback.
