# malgrind Stage 1 (desktop app, flamegraph + functions) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the `mulcan` CLI into the `malgrind` desktop app (.deb): start screen, history, New run (build with the agent injected, flash, live), attach without reset, remove agent, live/replayable flamegraph and function table with a time scrubber.

**Architecture:** A C++20 sidecar (`malgrind serve`) owns serial I/O, PlatformIO orchestration, symbolization, aggregation, analyses and the run store. It talks to a Tauri v2 window over **JSON lines on stdin/stdout**. The UI (Svelte 5 + TypeScript) only renders. The agent is today's core and ESP32 port, made into an IDF component that starts itself and is injected at build time without touching the user's files.

**Tech Stack:** C++20, CMake ≥ 3.20, nlohmann_json 3.11.3 (FetchContent), ESP-IDF 5.3.1 via PlatformIO (espressif32@6.9.0), Tauri 2 (`@tauri-apps/cli`, `@tauri-apps/api`, `@tauri-apps/plugin-shell`, `@tauri-apps/plugin-dialog`, all ^2), Svelte 5, Vite, Vitest, d3 7.9.0, d3-flame-graph 4.1.3, pnpm.

**Spec:** `docs/superpowers/specs/2026-11-15-malgrind-app-design.md` (approved 2026-11-15). This plan covers spec stage 1 (section 12, item 1). Stages 2–4 (agent v2 with sleep/beacon, heap and tasks; memcheck; diff/export/dmg) each get their own plan afterwards.

**Step 0 (before Task 1):** create branch `feat/malgrind-app` from the current branch; copy this plan to `docs/superpowers/plans/2026-11-15-malgrind-stage1-app.md` and commit it with the spec.

## Deviations from the spec (approve with this plan)

- **Transport: stdin/stdout JSON lines instead of localhost HTTP + token.** Same "sidecar computes, UI renders" split, but no port, no token, no CORS, nothing reachable by other local programs. `http.{hpp,cpp}` and the browser page are deleted.
- **The old CLI (`run/attach/detect` + browser viewer) is removed** at Task 8. Its logic moves into `serve` commands. `README` documents the app.
- **The Rust shell is ~15 lines** (plugin registration). The UI spawns the sidecar and builds the menu through the Tauri JS APIs.
- **Stage 1 agent is always on** (samples from boot). Sleep/start/stop and the beacon are stage 2. So in stage 1, "board running with agent" means frames arrive within the probe time.

## Global Constraints

- C++20, not C++23: no `string_view::contains`, no `views::enumerate/zip`. GCC ≥ 13 or Clang/libc++ ≥ 17.
- `core/` stays freestanding: no heap, no iostream, no SDK.
- Wire magic `MLC1` and `Arch` values never change.
- All analysis logic is in the C++ sidecar; TypeScript only renders and forwards user input.
- The serial port is opened exclusively (`TIOCEXCL`). A busy port is reported, never shared.
- Never write inside the user's project except `<project>/.pio/malgrind/`.
- Shared board: before every hardware step run `fuser -v /dev/ttyUSB0`; if the user's session holds it, stop and ask, never kill it.
- macOS code paths must keep compiling: CI `macos-latest` runs `ctest`.
- Run data directory: `$XDG_DATA_HOME/malgrind/runs`, else `~/.local/share/malgrind/runs`; macOS `~/Library/Application Support/malgrind/runs`; `--data-dir` overrides.
- The agent's default rate is 997 Hz; the baud rate is 921600.

## Review Focus

1. **Board unplugged mid-run.** The session ends with an `ended` event; the run is saved with the right duration; the UI leaves LIVE. Test: Task 5 `ended_on_eof`.
2. **App force-quit (sidecar SIGKILL) mid-run.** The run still lists, with its duration taken from `frames.mlg`. Test: Task 4 `unfinished_run_has_duration`.
3. **Port held by another program** (the user's own `malgrind` or a serial monitor). `probe` and `attach` return `busy`; no bytes are stolen. Test: Task 8 `busy_port`.
4. **User project with its own `sdkconfig.<env>` and a path with spaces.** Agent keys override, every user key survives, and the user's files are unchanged. Tests: Task 6 `merge_keeps_user_keys`, `paths_with_spaces`; Task 13 hardware check (`git status` clean).
5. **No or mismatched ELF on attach, or a stored run without `firmware.elf`.** Names fall back to raw addresses, a `notice` event explains why, and nothing crashes. Test: Task 5 `no_elf_raw_addresses`.

---

### Task 1: Rename mulcan → malgrind

**Files:** everything containing `mulcan`/`MULCAN` (`git grep -il mulcan`), e.g.
- Move: `core/include/mulcan/` → `core/include/malgrind/`, `platforms/*/include/mulcan/` → `.../malgrind/`
- Modify: `CMakeLists.txt`, `core/CMakeLists.txt`, `host/CMakeLists.txt`, `host/src/*.{hpp,cpp}`, `platforms/**`, `apps/host_runner/*`, `firmware/esp32/**`, `README.md`, tests

**Interfaces:**
- Produces: namespace `malgrind` / `malgrind::host`; targets `malgrind_core`, `malgrind_host`, executable `malgrind`; macros `MALGRIND_ISR`, `MALGRIND_ESP32_SKIP`, `MALGRIND_ESP32_DISPATCH_CYCLES`, `MALGRIND_HAS_FORMAT`; C entry `extern "C" bool malgrind_start(std::uint16_t rate_hz)`; IDF component `malgrind`; CMake project `malgrind`.

- [ ] **Step 1:** `git mv` the include dirs and `firmware/esp32/components/mulcan` → `firmware/esp32/components/malgrind`; replace identifiers in all files. Keep the string `MLC1` and the README sentence "*Micro Vulcan*" becomes "*valgrind for microcontrollers*".
- [ ] **Step 2:** Run `cmake -B build && cmake --build build && ctest --test-dir build`. Expected: `core` and `host` pass.
- [ ] **Step 3:** Run `git grep -il mulcan`. Expected: no output.
- [ ] **Step 4:** Build the demo: `pio run -d firmware/esp32 -e esp32dev`. Expected: SUCCESS.
- [ ] **Step 5:** Commit `refactor: rename mulcan to malgrind`.

### Task 2: Self-starting agent component (spike with hardware acceptance)

Highest-risk task: the spec's "no user source change" rests on it. If injection cannot be made to work, **stop and report to the user** (spec change), do not fall back silently.

**Files:**
- Move: `firmware/esp32/components/malgrind/CMakeLists.txt` → `agent/esp32/malgrind/CMakeLists.txt` (`root` = `${CMAKE_CURRENT_LIST_DIR}/../../..`; add `WHOLE_ARCHIVE`)
- Create: `agent/esp32/malgrind/autostart.cpp`, `agent/esp32/sdkconfig.agent`
- Modify: `platforms/esp32/src/esp32.cpp` (idempotent start), `firmware/esp32/main/main.cpp` (drop `malgrind_start`, `DEMO_PROFILER`, `DEMO_RATE_HZ`), `firmware/esp32/platformio.ini` (drop `esp32dev_baseline`), `firmware/esp32/sdkconfig.defaults` (drop the agent lines, they move to `sdkconfig.agent`), `README.md` (overhead A/B = plain build vs agent build)

**Interfaces:**
- Produces: component dir `agent/esp32` (for `EXTRA_COMPONENT_DIRS`); file `agent/esp32/sdkconfig.agent` with exactly
  `CONFIG_ESP_CONSOLE_UART_CUSTOM=y`, `CONFIG_ESP_CONSOLE_UART_NUM=0`, `CONFIG_ESP_CONSOLE_UART_BAUDRATE=921600`, `CONFIG_GPTIMER_ISR_IRAM_SAFE=y`;
  `malgrind_start()` returns true without restarting when already running; build flag `MALGRIND_RATE_HZ` (default 997).
- Produces (recorded for Task 6): the exact working `pio` command line for an agent build, written as a comment at the top of `host/src/pio.hpp`.

- [ ] **Step 1:** `autostart.cpp`: a `[[gnu::constructor]]` function that creates a one-shot task which calls `malgrind_start(MALGRIND_RATE_HZ)` and deletes itself. Logs an `ESP_LOGE` on failure. The demo `main.cpp` no longer mentions malgrind.
- [ ] **Step 2:** Make `malgrind_start` idempotent (static flag). If the UART0 driver is already installed, reuse it instead of failing (`uart_is_driver_installed`).
- [ ] **Step 3:** By hand, prepare `firmware/esp32/.pio/malgrind/sdkconfig.esp32dev` = `firmware/esp32/sdkconfig.esp32dev` (if it exists) + `sdkconfig.agent`. Then run, from the repo root:
  ```bash
  P=firmware/esp32
  PLATFORMIO_BUILD_DIR=$P/.pio/malgrind pio run -d $P -e esp32dev \
    -O "board_build.cmake_extra_args=-DEXTRA_COMPONENT_DIRS=$PWD/agent/esp32" \
    -O "board_build.esp-idf.sdkconfig_path=$PWD/$P/.pio/malgrind/sdkconfig.esp32dev"
  ```
  Expected: SUCCESS and `$P/.pio/malgrind/esp32dev/firmware.elf` exists. If an option name is wrong for espressif32@6.9.0, find the working mechanism (`-O` options, `PLATFORMIO_BUILD_FLAGS`, an `extra_scripts` file under `agent/esp32/`), keeping the constraint "only `.pio/malgrind` is written".
- [ ] **Step 4:** Run `git status --porcelain firmware/esp32`. Expected: empty (`.pio` is ignored; nothing else changed).
- [ ] **Step 5 (hardware, check `fuser` first):** flash with `-t upload --upload-port /dev/ttyUSB0` added, then `./build/host/malgrind attach --no-open --port /dev/ttyUSB0 --elf firmware/esp32/.pio/malgrind/esp32dev/firmware.elf`. Expected: the "mulcan: xtensa, 2 core(s)" banner (now "malgrind: …") within 5 s; `butterfly` in the folded output after Ctrl-C with `--folded`.
- [ ] **Step 6 (hardware):** `pio run -d firmware/esp32 -e esp32dev -t upload --upload-port /dev/ttyUSB0` (plain = remove agent), then attach again. Expected: the "no malgrind frames yet" hint after 5 s.
- [ ] **Step 7:** Write the working command into the `pio.hpp` header comment (create the file with only that comment) and add one line to the memory note `mulcan-esp32-gotchas.md` naming any non-obvious flag. Commit `feat(agent): self-starting IDF component injected at build time`.

### Task 3: Time-indexed aggregator and function table

**Files:**
- Modify: `host/src/aggregate.hpp`, `host/src/aggregate.cpp`, `host/CMakeLists.txt` (FetchContent nlohmann_json 3.11.3 from `https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz`, link `nlohmann_json::nlohmann_json` PUBLIC to `malgrind_host`)
- Test: `host/tests/test_host.cpp` (update the existing aggregation block)

**Interfaces:**
- Produces:
  ```cpp
  class Aggregator {
  public:
      using Ms = std::chrono::milliseconds;
      explicit Aggregator(Symbolizer& symbols);
      void add(Frame frame, Ms t);   // t = time since run start, non-decreasing
      [[nodiscard]] Ms duration() const;   // t of the last frame, 0 if none
      // window 0 = from the run start; at = nullopt means duration()
      [[nodiscard]] std::string snapshot_json(std::optional<unsigned> core, Ms window, std::optional<Ms> at = {}) const;
      [[nodiscard]] std::string functions_json(std::optional<unsigned> core, Ms window, std::optional<Ms> at,
                                               std::string_view focus) const;
      [[nodiscard]] std::string folded() const;
  };
  ```
  `snapshot_json` → `{"tree":{name,value,children},"stats":{…existing keys…,"duration_ms":N}}`; stats use frames in `[at-5s, at]`.
  `functions_json` → `{"total":N,"functions":[{"name","self","total"}],"callers":[{"name","count"}],"callees":[{"name","count"}]}`; functions sorted by `self` desc, at most 200. `self` counts stacks whose innermost name is the function; `total` counts each stack once even under recursion; callers and callees are the next outer or inner name around every occurrence of `focus` (empty `focus` → empty arrays).
  All frames are kept for the run (`// ponytail: all frames in RAM, iterate per query; add per-second prefix sums if long runs get slow`).

- [ ] **Step 1: Update and add the tests** in `test_host.cpp`. Reuse `make_stream` frames: add frame0 at 0 ms, frame1 at 0 ms, frame2 at 29000 ms.
  ```cpp
  assert(agg.duration() == 29000ms);
  assert(contains(agg.snapshot_json(std::nullopt, 10s), R"("name":"all","value":3)"));        // window before end
  assert(contains(agg.snapshot_json(std::nullopt, 0s), R"("name":"all","value":7)"));
  assert(contains(agg.snapshot_json(std::nullopt, 5s, 1000ms), R"("name":"all","value":4)"));  // scrubbed back
  assert(contains(agg.snapshot_json(1u, 0s), R"("name":"all","value":1)"));
  const auto fn = nlohmann::json::parse(agg.functions_json(std::nullopt, 0s, std::nullopt, "0x400d1fff"));
  assert(fn["total"] == 7);
  assert(fn["functions"][0]["name"] == "0x400d1000" && fn["functions"][0]["self"] == 6);   // leaf a
  assert(fn["callers"][0]["name"] == "0x400d2fff" && fn["callers"][0]["count"] == 7);       // main calls b
  assert(fn["callees"].size() == 2);                                                        // a and c
  // recursion: stack {f, f, main} counts f once in total
  ```
  Add the recursion case with a hand-built `Frame` (pcs `{0x400d1000, 0x400d1000, 0x400d3000}`): expect `total` of `0x400d0fff` (the call-adjusted caller copy) to equal the count once.
- [ ] **Step 2:** Run `cmake --build build && ctest --test-dir build -R host`. Expected: FAIL (no `duration`, no `functions_json`).
- [ ] **Step 3:** Implement. Add `Ms t` to the stored frames; the tree builder and stats take the `[at-window, at]` filter; `functions_json` walks the same expanded name lists `add_stack` uses (factor out `names(pcs) -> std::vector<std::string>` outermost-first so both share it).
- [ ] **Step 4:** Run the tests. Expected: `host ok`.
- [ ] **Step 5:** Commit `feat(host): time-indexed aggregation and function table`.

### Task 4: Run store

**Files:**
- Create: `host/src/store.hpp`, `host/src/store.cpp`
- Test: `host/tests/test_store.cpp` (+ `add_test(NAME store …)`)

**Interfaces:**
- Produces:
  ```cpp
  struct RunMeta {
      std::string id, name, notes, project, env, port, started;   // started: ISO-8601 UTC "2026-11-15T20:01:02Z"
      std::optional<Arch> arch; unsigned ncores = 0; double cpu_mhz = 0;
      std::uint64_t duration_ms = 0; bool has_elf = false;
  };
  void to_json(nlohmann::json&, const RunMeta&);   // keys as above; arch as arch_name() or null
  struct Chunk { std::uint32_t t_ms; std::vector<std::uint8_t> bytes; };   // raw serial read

  class RunWriter {   // one live run; every call is durable on return (flushed)
  public:
      void append(std::uint32_t t_ms, std::span<const std::uint8_t> bytes);
      void set_target(Arch arch, unsigned ncores, double cpu_mhz);
      void set_elf(const std::filesystem::path& elf);   // copies to <run>/firmware.elf
      void finish(std::uint64_t duration_ms);
      [[nodiscard]] const RunMeta& meta() const;
  };
  class RunStore {
  public:
      explicit RunStore(std::filesystem::path root);   // created if missing
      [[nodiscard]] static std::filesystem::path default_root();   // see Global Constraints
      [[nodiscard]] RunWriter create(RunMeta meta);      // fills id (yyyymmdd-hhmmss, "-2"… on collision) and started
      [[nodiscard]] std::vector<RunMeta> list() const;   // newest first
      [[nodiscard]] std::optional<RunMeta> get(std::string_view id) const;
      [[nodiscard]] std::vector<Chunk> chunks(std::string_view id) const;
      [[nodiscard]] std::filesystem::path elf(std::string_view id) const;   // empty if none
      std::optional<RunMeta> update(std::string_view id, std::optional<std::string> name, std::optional<std::string> notes);
      bool remove(std::string_view id);
  };
  ```
  Layout: `<root>/<id>/meta.json`, `frames.mlg` (records `t_ms u32 LE | n u16 LE | bytes[n]`), `firmware.elf`. `meta.json` is rewritten atomically (write `meta.json.tmp`, rename). A truncated trailing record is ignored.

- [ ] **Step 1: Write the tests** `test_store.cpp` (temp root, removed at the end):
  - `create_list_roundtrip`: create, `append(0,"ab")`, `append(250,"cd")`, `set_target(Xtensa,2,160)`, `finish(250)` → `list()[0].duration_ms == 250`, `arch == Xtensa`, `chunks(id)` returns 2 chunks with those bytes and times.
  - `unfinished_run_has_duration`: create + `append(0,…)` + `append(1200,…)` with no `finish` (writer destroyed) → `list()[0].duration_ms == 1200`.
  - `truncated_tail_ignored`: append 2 records, cut 3 bytes off `frames.mlg` → `chunks` returns 1.
  - `update_and_remove`: `update(id,"water-sim v3","note")` persists across a new `RunStore`; `remove(id)` → `list().empty()`.
  - `id_collision`: two `create` in the same second → distinct ids.
  - `elf_copy`: `set_elf(tmpfile)` → `elf(id)` exists, `has_elf == true`.
- [ ] **Step 2:** Run `ctest --test-dir build -R store`. Expected: FAIL (does not compile).
- [ ] **Step 3:** Implement `store.cpp`.
- [ ] **Step 4:** Run. Expected: PASS.
- [ ] **Step 5:** Commit `feat(host): run store`.

### Task 5: Session (live recording and replay)

**Files:**
- Create: `host/src/session.hpp`, `host/src/session.cpp`
- Modify: `host/src/discover.cpp` (`find_elf` also scans `<dir>/.pio/malgrind/*/firmware.elf`), `host/tests/test_host.cpp` (one assert for that)
- Test: `host/tests/test_session.cpp` (+ `add_test(NAME session …)`); uses a pty (`posix_openpt`/`grantpt`/`unlockpt`/`ptsname`, portable to macOS)

**Interfaces:**
- Consumes: `Source` (`source.hpp`), `FrameReader` (`wire.hpp`), `Symbolizer`/`find_addr2line` (`symbols.hpp`), `find_elf` (`discover.hpp`), `Aggregator` (Task 3), `RunStore`/`RunWriter` (Task 4).
- Produces:
  ```cpp
  using Emit = std::function<void(const nlohmann::json& event)>;   // must be thread-safe
  class Session {
  public:
      // Starts a reader thread on `source`. `elf` empty → find_elf(arch, project) at the first frame.
      static std::unique_ptr<Session> live(Source source, RunWriter writer, std::filesystem::path elf,
                                           std::filesystem::path project, Emit emit);
      static std::unique_ptr<Session> replay(const RunStore& store, std::string_view id, Emit emit);   // null if unknown
      ~Session();   // live: stops the thread, writer.finish(duration)
      [[nodiscard]] bool live() const;   // false after `ended` or for replays
      [[nodiscard]] std::string run_id() const;
      // nullopt before the first frame
      [[nodiscard]] std::optional<std::string> snapshot(std::optional<unsigned> core, Aggregator::Ms window, std::optional<Aggregator::Ms> at) const;
      [[nodiscard]] std::optional<std::string> functions(std::optional<unsigned> core, Aggregator::Ms window, std::optional<Aggregator::Ms> at, std::string_view focus) const;
  };
  ```
  Events (one JSON object each): `{"event":"log","text":…}` (non-frame bytes), `{"event":"target","arch","ncores","cpu_mhz","elf":path|null}`, `{"event":"notice","text":…}` (no ELF / no addr2line / no frames after 5 s), `{"event":"ended","run":id,"reason":"eof"|"error"}`. Live time = `steady_clock` since the session start; each chunk is appended to the writer before decoding.

- [ ] **Step 1: Write the tests** (frames from `make_stream`, copied from `test_host.cpp` into a shared `host/tests/stream.hpp`):
  - `live_records_and_aggregates`: write the stream to the pty master; within 2 s `snapshot(nullopt, 0s, nullopt)` contains `"value":7`. A `target` event was emitted with `ncores == 2`.
  - `ended_on_eof`: close the master → an `ended` event within 1 s; `live() == false`; after destruction `store.list()[0].duration_ms` is set.
  - `no_elf_raw_addresses`: empty project dir → a `notice` event containing `"ELF"`; the tree has the name `"0x400d2fff"`.
  - `replay_matches_live`: `Session::replay(store, id)` gives the same `snapshot(nullopt, 0s, nullopt)` tree as the live one.
  - `log_passthrough`: bytes `"I (1) app: hi\n"` before the stream → a `log` event with that text.
- [ ] **Step 2:** Run `ctest --test-dir build -R session`. Expected: FAIL (does not compile).
- [ ] **Step 3:** Implement `session.cpp`. The reader thread loop is today's `attach()` loop from `host/src/main.cpp` minus the HTTP server and browser.
- [ ] **Step 4:** Run `session` and `host`. Expected: PASS.
- [ ] **Step 5:** Commit `feat(host): live and replay sessions`.

### Task 6: PlatformIO integration

**Files:**
- Modify: `host/src/pio.hpp` (keeps Task 2's comment), `host/src/process.hpp`, `host/src/process.cpp`
- Create: `host/src/pio.cpp`
- Test: `host/tests/test_pio.cpp` (+ `add_test(NAME pio …)`)

**Interfaces:**
- Produces:
  ```cpp
  // process.hpp: run with extra environment variables; each stdout/stderr line goes to on_line. Returns the exit code or -1.
  int run_lines(const std::vector<std::string>& argv, const std::vector<std::string>& env_add,
                const std::function<void(std::string_view line)>& on_line);

  // pio.hpp
  [[nodiscard]] std::optional<std::filesystem::path> find_pio(const std::filesystem::path& home);
      // PATH, then home/.platformio/penv/bin/pio, home/.local/bin/pio (GUI apps often lack the shell PATH)
  struct ProjectInfo { std::filesystem::path dir; std::vector<std::string> envs; std::string default_env; };
  [[nodiscard]] std::optional<ProjectInfo> read_project(const std::filesystem::path& dir, std::string& why);
      // parses platformio.ini: [env:NAME] sections, [platformio] default_envs (first one); default = first env if unset
  [[nodiscard]] std::string merge_sdkconfig(std::string_view user, std::string_view agent);
      // agent's CONFIG_X=… lines replace user's CONFIG_X=… and "# CONFIG_X is not set"; other user lines kept in order
  struct Invocation { std::vector<std::string> argv; std::vector<std::string> env; };
  [[nodiscard]] Invocation agent_build(const std::filesystem::path& pio, const ProjectInfo& p, const std::string& env,
                                       const std::string& port, const std::filesystem::path& agent_dir);
      // the Task 2 command with upload; writes <dir>/.pio/malgrind/sdkconfig.<env> (merge of the user's file and agent_dir/agent/esp32/sdkconfig.agent)
  [[nodiscard]] Invocation plain_flash(const std::filesystem::path& pio, const ProjectInfo& p, const std::string& env, const std::string& port);
  [[nodiscard]] std::filesystem::path agent_elf(const ProjectInfo& p, const std::string& env);   // <dir>/.pio/malgrind/<env>/firmware.elf
  [[nodiscard]] std::optional<std::string> chip_id(const std::filesystem::path& pio, const std::string& port, const std::filesystem::path& home);
      // moved from main.cpp detect(): pio's shebang python + esptool.py chip_id; resets the board
  ```
  `find_in_path` moves from `main.cpp` to `pio.cpp`.

- [ ] **Step 1: Write the tests:**
  - `read_envs`: an ini with `[platformio]\ndefault_envs = b, a\n[env:a]\n[env:b]\n[env]\n` → envs `{"a","b"}` (bare `[env]` is not an env), default `"b"`; a missing ini → nullopt and `why` contains `platformio.ini`.
  - `merge_keeps_user_keys`: user `"CONFIG_A=1\n# CONFIG_GPTIMER_ISR_IRAM_SAFE is not set\nCONFIG_ESP_CONSOLE_UART_BAUDRATE=115200\n"` merged with the real `agent/esp32/sdkconfig.agent` → contains `CONFIG_A=1`, `CONFIG_GPTIMER_ISR_IRAM_SAFE=y`, `CONFIG_ESP_CONSOLE_UART_BAUDRATE=921600`; does not contain `115200` or `is not set`.
  - `paths_with_spaces`: `agent_build` for dir `/tmp/my proj/ä` keeps the dir as one argv element; `env` contains `PLATFORMIO_BUILD_DIR=/tmp/my proj/ä/.pio/malgrind`; no argv element is a shell string.
  - `plain_flash_has_no_agent`: no argv/env element mentions `malgrind`.
  - `find_pio_fallback`: empty `PATH` (set in the test) and a fake `home/.platformio/penv/bin/pio` → found.
  - `run_lines_streams`: `run_lines({"sh","-c","echo a; echo b >&2; exit 3"}, {"X=1"}, …)` → lines `a` and `b`, returns 3.
- [ ] **Step 2:** Run `ctest --test-dir build -R pio`. Expected: FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run. Expected: PASS.
- [ ] **Step 5:** Commit `feat(host): PlatformIO build with agent injection`.

### Task 7: `malgrind serve` protocol; remove the old CLI

**Files:**
- Create: `host/src/serve.hpp`, `host/src/serve.cpp`
- Modify: `host/src/main.cpp` (only `malgrind serve [--data-dir D] [--agent-dir D]`; SIGTERM/SIGINT → the same shutdown as stdin EOF), `host/CMakeLists.txt` (drop `http.cpp` and the `flame_html.inc` generation; on Linux link `malgrind` with `-static-libstdc++ -static-libgcc`)
- Delete: `host/src/http.hpp`, `host/src/http.cpp`, `host/web/flame.html`
- Test: `host/tests/test_serve.cpp` (+ `add_test(NAME serve …)`)

**Interfaces:**
- Consumes: Tasks 4–6, `find_serial_ports`, `Source`.
- Produces: `int serve(std::istream& in, std::ostream& out, const ServeOptions& o);` with `struct ServeOptions { std::filesystem::path data_dir, agent_dir, home; };`. Returns when `in` hits EOF, after ending any live session.
  Protocol, one JSON object per line. Request `{"id":N,"cmd":"…",…}` → reply `{"id":N,"ok":true,"result":…}` or `{"id":N,"ok":false,"error":"…"}`. Events have no `id`. `out` is written under one mutex. Long commands (`new_run`, `remove_agent`, `chip_id`) run on one worker thread; a second long command while one runs → error `"busy: another build or flash is running"`. Commands:

  | cmd | args | result |
  |---|---|---|
  | `hello` | – | `{"version":"0.1.0","data_dir","pio":path\|null}` |
  | `ports` | – | `["/dev/ttyUSB0",…]` |
  | `probe` | `port` | `{"busy":bool,"agent":bool,"arch","ncores","cpu_mhz"}`; listens up to 2 s, stops at the first frame |
  | `chip_id` | `port` | `{"chip":"…"}` (resets the board) |
  | `project` | `dir` | `{"dir","envs","default_env"}` |
  | `new_run` | `dir, env, port, name` | `{"run":RunMeta}` once flashed and live; streams `{"event":"build","line"}` |
  | `attach` | `port, name, project?, elf?` | `{"run":RunMeta}` |
  | `stop` | – | `{}` (ends the live session) |
  | `remove_agent` | `dir, env, port` | `{}`; ends a live session on that port first; streams `build` events |
  | `runs` | – | `[RunMeta…]` plus `"live":true` on the current live run |
  | `open` | `id` | `{"run":RunMeta}`; error `"stop the live run first"` while live |
  | `update` | `id, name?, notes?` | `RunMeta` |
  | `delete` | `id` | `{}`; error if it is the current session |
  | `snapshot` | `core?, window_s, at_ms?` | the Session's snapshot JSON, or `{"tree":null}` |
  | `functions` | `core?, window_s, at_ms?, focus?` | the Session's function-table JSON, or `{"functions":[]}` |

  Unknown `cmd` or malformed JSON → `ok:false` with the reason (malformed without an id → `"id":null`), and the loop continues.

- [ ] **Step 1: Write the tests** (drive `serve` through a pipe-backed `std::istream` on a thread, parse `out` lines):
  - `hello_and_runs`: `hello` → `version == "0.1.0"`; `runs` → `[]`.
  - `bad_input`: `not json` → `ok:false`, then `hello` still answers.
  - `attach_snapshot_stop`: attach to a pty slave and write the `make_stream` bytes → `snapshot {window_s:0}` has `"value":7`; `stop` → `runs` shows `duration_ms > 0` and no `live`.
  - `busy_port`: hold the pty slave open with `Source::open` → `probe` returns `busy:true`, and `attach` returns `ok:false` with `"busy"` in `error`.
  - `open_while_live_refused`: during a live attach, `open` → `"stop the live run first"`.
  - `eof_finishes_run`: close `in` during a live attach → `serve` returns and the stored run has `duration_ms > 0`.
- [ ] **Step 2:** Run `ctest --test-dir build -R serve`. Expected: FAIL.
- [ ] **Step 3:** Implement `serve.cpp` and the new `main.cpp`. Delete the old CLI code paths and files.
- [ ] **Step 4:** Run the full suite `ctest --test-dir build --output-on-failure`. Expected: all pass. Also run `printf '{"id":1,"cmd":"hello"}\n' | ./build/host/malgrind serve`. Expected: one `ok:true` line.
- [ ] **Step 5:** Commit `feat(host): malgrind serve JSON-lines protocol, drop browser CLI`.

### Task 8: Tauri app shell and sidecar client

**Files:**
- Create: `app/package.json`, `app/vite.config.ts`, `app/tsconfig.json`, `app/index.html`, `app/app-icon.svg`, `app/scripts/build-sidecar.mjs`, `app/src/main.ts`, `app/src/App.svelte`, `app/src/lib/sidecar.ts`, `app/src/lib/sidecar.test.ts`, `app/src/lib/theme.css`, `app/src-tauri/{Cargo.toml,build.rs,tauri.conf.json,src/main.rs,capabilities/default.json}`, `app/src-tauri/icons/*` (generated by `pnpm tauri icon app-icon.svg`)
- Modify: `.gitignore` (`app/node_modules`, `app/dist`, `app/src-tauri/target`, `app/src-tauri/binaries`)

**Interfaces:**
- Consumes: the Task 7 protocol.
- Produces (`app/src/lib/sidecar.ts`):
  ```ts
  export interface Transport { write(line: string): Promise<void>; onLine(cb: (line: string) => void): void; onExit(cb: (code: number | null) => void): void; }
  export class Sidecar {
    constructor(t: Transport);
    call<T = unknown>(cmd: string, args?: Record<string, unknown>): Promise<T>;   // rejects with Error(error)
    on(event: string, cb: (e: any) => void): () => void;                          // returns an unsubscribe function
  }
  export async function startSidecar(): Promise<Sidecar>;
    // Command.sidecar('binaries/malgrind', ['serve', '--agent-dir', await resolveResource('agent-src')]).spawn();
    // each stdout event is split on '\n', empty pieces skipped, each piece parsed as JSON
  ```
  `tauri.conf.json`: `productName` "malgrind", `identifier` "dev.malgrind.app", `bundle.externalBin` `["binaries/malgrind"]`, `bundle.resources` maps `../../core/` → `agent-src/core/`, `../../platforms/esp32/` → `agent-src/platforms/esp32/`, `../../agent/` → `agent-src/agent/`; `bundle.targets` `["deb"]`; `beforeDevCommand` / `beforeBuildCommand` run `node scripts/build-sidecar.mjs && pnpm vite …`.
  `capabilities/default.json`: `core:default`, `dialog:allow-open`, `shell:allow-spawn` (`{"name":"binaries/malgrind","sidecar":true,"args":true}`), `shell:allow-stdin-write`, `shell:allow-kill`.
  `main.rs`: `tauri::Builder::default().plugin(tauri_plugin_shell::init()).plugin(tauri_plugin_dialog::init()).run(tauri::generate_context!())`.
  `build-sidecar.mjs`: `cmake -B ../build -DCMAKE_BUILD_TYPE=Release && cmake --build ../build --target malgrind`, then copies `../build/host/malgrind` to `src-tauri/binaries/malgrind-<rustc --print host-tuple>`.
  `theme.css`: color tokens from the approved mockups (bg `#14161c`/fg `#e8e6e3` dark; bg `#f6f4f0`/fg `#23211e` light; accent `#ff7a2f`; live `#37d67a`; danger `#ff5a5a`), dark under `@media (prefers-color-scheme: dark)`.

- [ ] **Step 1: Write `sidecar.test.ts`** with a fake `Transport`:
  - replies are matched by `id` even when out of order;
  - `ok:false` rejects with the message;
  - `"{…}\n{…}\n"` in one chunk dispatches two messages, and a chunk without a trailing newline also dispatches;
  - events reach `on()` subscribers and not pending calls;
  - transport exit rejects all pending calls with `"sidecar exited"`.
- [ ] **Step 2:** Run `pnpm -C app vitest run`. Expected: FAIL.
- [ ] **Step 3:** Implement `sidecar.ts`, the scaffold and `App.svelte` showing the `hello` result (temporary text, replaced in Task 9).
- [ ] **Step 4:** Run `pnpm -C app vitest run`. Expected: PASS. Then `pnpm -C app tauri dev`. Expected: a window titled malgrind showing `version 0.1.0` and the pio path.
- [ ] **Step 5:** Commit `feat(app): tauri shell with sidecar client`.

### Task 9: Start screen, history and menu

**Files:**
- Create: `app/src/screens/Start.svelte`, `app/src/lib/menu.ts`
- Modify: `app/src/App.svelte` (screens `start | newrun | run`, holds the `Sidecar`, the remembered project in `localStorage` key `malgrind.project`, guarded with try/catch)

**Interfaces:**
- Consumes: `hello`, `ports`, `probe`, `runs`, `open`, `update`, `delete`, `attach`.
- Produces: `installMenu(actions: { newRun(): void; stop(): void; removeAgent(): void }): Promise<void>` builds the native menu "Run" (New run… `CmdOrCtrl+N`, Stop) and "Board" (Remove agent…) via `@tauri-apps/api/menu` and `setAsAppMenu()`.

- [ ] **Step 1:** `Start.svelte` renders the three approved states: no runs (big "+ New run"); exactly one port with `probe.agent` → card "Your firmware is running · agent found · N cores · X MHz" with **Attach live**, "+ New run" and a red "Remove agent…" link; runs exist → list (name, start date, duration, board), each row opens, renames inline (`update`) and deletes with confirmation (`delete`). A busy port shows "Port busy: used by another program". Without `pio`: New run shows the install hint `pip install platformio` / link, attach still works.
- [ ] **Step 2:** Check in `pnpm -C app tauri dev` with `--data-dir` pointed at a directory holding one run produced by the Task 7 tests (copy it) and no board: the list shows it, rename persists after restarting the app, delete removes it.
- [ ] **Step 3:** Commit `feat(app): start screen, history, menu`.

### Task 10: New run wizard, attach, remove agent

**Files:**
- Create: `app/src/screens/NewRun.svelte`, `app/src/components/RemoveAgent.svelte`
- Modify: `app/src/App.svelte`, `app/src/screens/Start.svelte`

**Interfaces:**
- Consumes: `project`, `ports`, `probe`, `new_run` (+ `build` events), `attach`, `remove_agent`; `open()` from `@tauri-apps/plugin-dialog` with `{directory: true}`.
- Produces: `RemoveAgent.svelte` props `{ sidecar: Sidecar; port: string; project?: string; onDone(): void }`. The dialog text is exactly the spec's: "Rebuilds your project without malgrind and flashes it. The board runs your original firmware again. Your code is not touched. Live profiling stops."; it asks for the folder when `project` is unknown.

- [ ] **Step 1:** The wizard has four steps (Project → Board → Build & flash → Live) as in the approved mockup. Project: folder dialog, env select (default preselected), error text from `project`. Board: auto-pick if one port, list if several, "Identify chip (resets the board)" button calls `chip_id`. Build & flash: indeterminate bar plus the last 200 `build` lines in a monospace log; on failure the log stays and "Back" is offered. Live: switch to the run screen with the returned run.
- [ ] **Step 2:** Attach live (start card) calls `attach {port, name: "<project folder name or 'board'> <date>", project: remembered}` and opens the run screen.
- [ ] **Step 3 (hardware, `fuser` first):** in `tauri dev`: New run on `firmware/esp32` → live flamegraph data arrives (the run view comes in Task 11; until then verify with the `runs` list showing `live`). Remove agent → `runs` shows the run ended, and a probe shows no agent.
- [ ] **Step 4:** Commit `feat(app): new run wizard, attach, remove agent`.

### Task 11: Run view: sidebar, flamegraph, functions, scrubber

**Files:**
- Create: `app/src/screens/Run.svelte`, `app/src/views/Flame.svelte`, `app/src/views/Functions.svelte`, `app/src/lib/blend.ts`, `app/src/lib/blend.test.ts`

**Interfaces:**
- Consumes: `snapshot`, `functions`, `stop`, `update`; the `target`, `log`, `notice`, `ended` events; `RemoveAgent.svelte`.
- Produces: `blend(prev: Node | null, next: Node | null, floor: number, k = 0.25): Node`, ported from `host/web/flame.html` (deleted in Task 7; take it from git history `a4e37a1:host/web/flame.html`).

- [ ] **Step 1: Write `blend.test.ts`:** a parent stays the sum of its children after blending; a node absent from `next` decays and is dropped below `floor`; `blend(null, t)` equals `k`·t values.
- [ ] **Step 2:** Run `pnpm -C app vitest run`. Expected: FAIL. Implement `blend.ts`. Run again. Expected: PASS.
- [ ] **Step 3:** `Run.svelte` uses the approved sidebar layout: items Flamegraph and Functions only (no placeholders for stage 2/3 tabs). Core select and window select (5/10/30/60 s, all) sit in the sidebar. The footer has a red "Remove agent…" link (live runs only). The header shows the editable name, a LIVE dot (live only), samples/s, dropped and overhead from `stats`, a notes button (textarea → `update`) and Stop. The bottom has an `<input type="range">` scrubber over `0…duration_ms`: in live mode it follows the end until dragged; "Back to live" resumes. `notice` events show as a dismissible bar, and `ended` switches LIVE off and shows "Board stopped sending". Polling every 500 ms calls only the visible view's command.
- [ ] **Step 4:** `Flame.svelte`: d3-flame-graph 4.1.3 with `transitionDuration(0)`, `blend` smoothing reset on a core/window/scrub change (as in the old page). `Functions.svelte`: a table sorted by self (self %, total %, name), and a click sets `focus` and shows the callers and callees lists; clicking a caller or callee refocuses.
- [ ] **Step 5:** Check in `tauri dev` by attaching to a pty fed by `host_runner` (`apps/host_runner`, Linux): expect `busy_a ≈ 60 %, busy_b ≈ 30 %` in Functions, a growing flamegraph, scrubbing back changes the picture, and reopening the run from history shows the same whole-run picture.
- [ ] **Step 6:** Commit `feat(app): run view with flamegraph, functions and scrubber`.

### Task 12: Package, document, verify end to end

**Files:**
- Modify: `README.md` (app-first: install the .deb, New run, attach, remove agent, history; architecture diagram with the sidecar; build-from-source commands; the deviations above), `.github/workflows/ci.yml` (add `pnpm -C app install && pnpm -C app vitest run` on ubuntu; ctest stays on both OSes)
- Memory: update `mulcan-project-context.md` (renamed product, app architecture, stdio protocol) and `mulcan-user-workflow.md` (app replaces CLI).

- [ ] **Step 1:** Run `pnpm -C app tauri build --bundles deb`. Expected: `app/src-tauri/target/release/bundle/deb/malgrind_0.1.0_amd64.deb`. Check that `dpkg -c` lists `usr/bin/malgrind`, the sidecar and `agent-src/agent/esp32/sdkconfig.agent`.
- [ ] **Step 2:** Extract the .deb into the scratchpad (`dpkg -x`) and run the binary from there. Expected: the start screen appears and `hello` reports the pio path. (Installing system-wide is the user's call.)
- [ ] **Step 3 (hardware, `fuser` first), from the extracted build:**
  1. New run on `firmware/esp32` → live; `git status --porcelain firmware/esp32` empty.
  2. Close the app, reopen → the start card says the firmware is running; **Attach live** works, and the demo's `iterations/s` log line continues without restarting (no reset).
  3. Unplug USB → `ended`, run saved; replug, attach again.
  4. Remove agent → probe shows no agent; history keeps both runs; reopening one replays with names.
  5. Overhead in the header ≈ 1.5 % (unchanged from mulcan).
- [ ] **Step 4:** Commit `docs: malgrind app README, CI runs UI tests`.

---

## Self-review notes

- Spec coverage (stage 1): rename (T1); agent injection without user changes (T2, T6); token-safe transport (stdio, deviation); attach without reset (T7 `probe`, T12 hardware check); New run 4 steps (T10); remove agent in two places (T9, T10, T11); history summary, scrubbing, name, notes, delete (T4, T9, T11); flamegraph and functions live and replayed (T3, T5, T11); theme follows the OS (T8); .deb (T12). Diff, export, Memory, Tasks, Memcheck, beacon/sleep and .dmg are stages 2–4.
- Types: `RunMeta` fields are used identically in T4, T7 and the UI; `Aggregator::Ms` is used in T3, T5 and T7; the `Sidecar.call` names match the T7 command table.
