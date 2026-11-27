# RP2040 Agent (`agent/rp2040/`)

PlatformIO `raspberrypi` injection and runtime startup on top of `platforms/cortexm/`.

## Build Injection (`inject.py`)

`inject.py` runs as a PlatformIO SCons `pre:` script (`PLATFORMIO_EXTRA_SCRIPTS=pre:.../agent/rp2040/inject.py`):

- Compiles `core/src` (`profiler.cpp`, `command.cpp`), `platforms/cortexm/src/cortexm.cpp`, and `agent/rp2040/autostart.cpp` via `env.BuildSources(...)` into `$BUILD_DIR/malgrind/...` without modifying any project files.
- Resolves `MALGRIND_AGENT_DIR`, `MALGRIND_CORE_DIR`, and `MALGRIND_PLATFORMS_DIR`, falling back to sibling `../../core` and `../../platforms` so packaged app resources (`agent-src/`) work identically to the source tree.
- Passes `-Wl,--wrap=isr_systick` and `-Wl,-u,malgrind_rp2040_autostart_link`.

## Cortex-M0+ 1 us Timer Scaling & `--wrap=isr_systick`

- **Why `--wrap=isr_systick`**: On RP2040 (`pico-sdk` / `arduino-pico`), the Cortex-M0+ vector table routes SysTick to `isr_systick`. `__wrap_isr_systick` extracts the hardware-stacked `ExceptionFrame*` (`r0..r3, r12, lr, pc, xpsr`) from `MSP` or `PSP` using ARMv6-M Thumb-1-safe instructions before any compiler prologue alters `SP`/`LR`, calls `malgrind_cortexm_sample_isr(0, frame)`, and tail-chains `__real_isr_systick` with the original `EXC_RETURN`.
- **1 us hardware timer (`TIMER->TIMERAWL` at `0x40054028`)**: ARMv6-M Cortex-M0+ cores have no `DWT->CYCCNT` register. `autostart.cpp` sets `has_dwt_cyccnt = false` and reads the raw, side-effect-free 1 us `TIMER->TIMERAWL` counter; `platforms/cortexm` scales 32-bit microsecond deltas into CPU cycles using `cpu_hz` (`clock_get_hz(clk_sys)` / `SystemCoreClock` / `F_CPU` / `125 MHz`).

## USB-CDC Transport & Cooperative Polling

- **Safe lazy init**: `malgrind_cortexm_init` runs lazily on the first SysTick rather than in a pre-`main` constructor, avoiding USB-CDC (TinyUSB / `Serial` / `pico_stdio_usb`) initialization ordering races.
- **USB-CDC bridge**: Streams framed depth-2 samples over weak hooks `malgrind_rp2040_write` / `malgrind_rp2040_read` (defaulting to Arduino `Serial` or `pico-sdk` `putchar_raw` / `getchar_timeout_us(0)` with `stdio_usb_connected()` guard when USB stdio is active).
- **ISR vs main safety**: `malgrind_rp2040_poll()` can be called from `loop()`/`main()` to perform USB-CDC I/O outside interrupt context; otherwise, a 50 ms gate in `isr_systick` drives `malgrind_cortexm_poll()` under a `PRIMASK`-guarded `g_in_poll` re-entrancy guard (no `libatomic` RMW helpers required on ARMv6-M).

## Flash Backup, Restore, and Zero-Flash (`no_flash`)

- **OpenOCD SWD (`0x10000000`)**: Uses generic CMSIS-DAP / Raspberry Pi Debug Probe (`2e8a:000c`, `adapter speed 5000`) / J-Link probes with `target/rp2040.cfg` for `identify`, `read_flash` (`dump_image`), `write_flash` (`program ... verify reset`), and dual-core `HaltResume` PC sampling (`no_flash`).
- **`picotool` BOOTSEL fallback**: When OpenOCD is unavailable or no SWD probe is connected, `identify`, `read_flash` (`save -r 0x10000000 <end_hex> <out>`), and `write_flash` (`load -v -x <image>`) fall back to `picotool` (PATH or PlatformIO `tool-picotool*`) with the board in BOOTSEL mode (`BOOTSEL` held during USB plug-in or `RESET`).
