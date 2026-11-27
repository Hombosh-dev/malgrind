# malgrind

Live profiler for microcontrollers (like valgrind/perf, but over UART or a debug probe). Shows a live flamegraph, functions table, heap memory charts, FreeRTOS task stats, basic leak/memory error checks, and run comparison/diff. Works on ESP32 (ESP-IDF), STM32, RP2040, and OpenOCD debug probes across Linux, macOS, and Windows.

![live flamegraph](docs/app-flamegraph.png)

## How it works

- `agent/` + `platforms/` + `core/`: small C++ agent that PlatformIO injects at build time without editing your source files. Samples at ~1000 Hz per core via timer ISR, or you can use OpenOCD probe mode with no flashing at all.
- `host/` (`malgrind-cli`): C++20 engine that handles OS serial/USB ports, PlatformIO builds, flash backups, v1/v2 frame decoding, symbolization (`addr2line`), aggregation, diffing, export, and run history.
- `app/`: Tauri + Svelte/TypeScript desktop app that renders what `malgrind-cli serve` sends over JSON lines.

## Install / build

Prebuilt packages (`.deb` for Linux, `.dmg` for macOS, `.exe` NSIS installer for Windows) are built by `.github/workflows/release.yml` on version tags (`v*`). On macOS you may need `xattr -dr com.apple.quarantine /Applications/malgrind.app` on first run since the app is ad-hoc signed.

To build from source you need:
- CMake >= 3.20 and a C++20 compiler with `<format>` (GCC 13+, Clang 15+, or MSVC 17.8+)
- Node 22, `pnpm`, Rust toolchain (plus `libwebkit2gtk-4.1-dev`, `libxdo-dev`, `libssl-dev`, `libayatana-appindicator3-dev`, `librsvg2-dev` dependecies on Linux)

```bash
# engine + tests
cmake -B build && cmake --build build -j && ctest --test-dir build --output-on-failure

# desktop app
cd app && pnpm install
pnpm test
pnpm check
pnpm tauri dev      # run dev build
pnpm tauri build    # build installer bundle
```

## Using it

In the desktop app you can open a local project folder, clone a public GitHub repo (must be trusted before building), or pick a board without source code and optionally attach an `.elf` for symbol names.

- Modes: **Attach** (board already has the agent), **Build & flash** (injects the agent at build time and flashes), or **Debug probe** (flat PC sampling over OpenOCD, nothing flashed).
- Before flashing for the first time, malgrind dumps the board's flash to its backup directory. **Remove agent** writes the origianl image back byte for byte, or rebuilds cleanly without the agent.
- Runs can be renamed, annotated with notes, compared side-by-side in **Diff**, or exported as `.folded`, `.speedscope.json`, `.svg`, or `.png`.
- If PlatformIO is missing, the app can install it into `~/.platformio/penv` with Python 3 (no root needed). On Linux, if a serial port gives permission denied, **Grant access** uses `pkexec` to set an ACL and add you to `dialout`.

Terminal mode (`malgrind-cli`) opens a simple live viewer in the browser and shares run history with the app:

```bash
malgrind-cli devices          # list serial boards and debug probes
malgrind-cli run              # build with agent, flash, open live view
malgrind-cli attach           # connect to an already-running agent
malgrind-cli detect           # identify chip (resets the board)
malgrind-cli restore          # restore the pre-agent flash backup
```

Useful flags: `--device`, `--project`, `--env`, `--elf`, `--http-port`, `--folded`, `--name`, `--no-open`.

## Limits

- ESP32 projects need `framework = espidf` and a project path without spaces (PlatformIO limit).
- Sampling overhead on ESP32 is around 1.5% per core at 997 Hz. Turning on `Check memory errors` traces alloc/free calls in fixed rings and uses more RAM and CPU.
- Code inside critical sections or higher-priority ISRs gets blamed on whatever runs right after; inlined functions won't show up as seperate frames, and max stack depth is 16.
- Cortex-M (STM32/RP2040) agent captures depth-2 stacks (`pc` + `lr`) from SysTick; OpenOCD debug probe mode records flat PCs only (DWT PCSR on M3+ or halt/resume).
- Since it's statistical sampling, let it run 20-30 s for stable numbers.

## Repo layout

```
core/       freestanding C++20 profiler, wire encoder, command parser, event queues
platforms/  target ports (host SIGALRM, esp32, cortexm)
agent/      PlatformIO build injection scripts + autostart per family (esp32, stm32, rp2040)
host/       malgrind-cli C++20 engine, os/ abstractions, families/, tests
app/        Tauri Rust shell + Svelte/TypeScript UI
firmware/   example ESP-IDF demo firmware
apps/       Linux host demo runner
scripts/    version.mjs, check-portability.mjs, cpp-share.mjs
```

## Adding a new MCU

Keep OS code in `host/src/os/` and chip logic in `host/src/families/`, `agent/`, and `platforms/` (verify with `node scripts/check-portability.mjs`):
1. Add the timer/ISR target port in `platforms/<id>/`.
2. Add `agent/<id>/family.json` and build injection script.
3. Subclass `Family` in `host/src/families/` and register it in `host/src/families/registry.cpp`.
