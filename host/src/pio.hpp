#pragma once

// PlatformIO integration: finding pio, reading the project, building with the agent injected, flashing without it.
//
// How the agent gets in (found with a spike):
//
// Agent build, found working on espressif32@6.9.0 / ESP-IDF 5.3.1. The user's project is not modified; all output goes
// to <project>/.pio/malgrind/. Environment of the `pio run` call:
//   PLATFORMIO_BUILD_DIR     <project>/.pio/malgrind/build
//   PLATFORMIO_EXTRA_SCRIPTS pre:<agent-dir>/agent/esp32/inject.py   (adds the `malgrind` IDF component and points the
//                                                                      build at MALGRIND_SDKCONFIG)
//   MALGRIND_AGENT_DIR       <agent-dir>/agent/esp32
//   MALGRIND_SDKCONFIG       <project>/.pio/malgrind/sdkconfig.<env>   (NOT inside the build dir; the user's sdkconfig.<env> with the keys of
//                                                                      agent/esp32/sdkconfig.agent replaced, see below)
// Command: pio run -d <project> -e <env> -t upload --upload-port <port>
// (`pio run` has no -O option; options reach the IDF builder only through the pre-script.)
//
// Findings that shape the code:
//  * sdkconfig merge: an agent key replaces BOTH forms of the user's line (`CONFIG_X=..` and `# CONFIG_X is not set`);
//    sdkconfig.agent lists `# CONFIG_ESP_CONSOLE_UART_DEFAULT is not set` too, because DEFAULT/CUSTOM are one Kconfig
//    choice and a user's DEFAULT=y would otherwise win. Without it the console stayed at 115200.
//  * PlatformIO deletes its whole build directory when it has no project.checksum (every first build). The seeded
//    sdkconfig therefore lives next to the build directory, not in it. (A spike that reused an old directory hid this.)
//  * Paths given to pio must be absolute: `-d <project>` makes pio change directory. A relative project path once
//    created <project>/<project>/.pio/malgrind and flashed a firmware built from default settings.
//  * PlatformIO's IDF builder refuses a project path containing a space (espidf.py checks BUILD_DIR).
//  * IDF's WHOLE_ARCHIVE is unusable under PlatformIO (it sorts link flags, so --whole-archive lands around everything);
//    the agent object is forced into the link with `-u malgrind_autostart_link` instead.

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace malgrind::host {

// The `pio` executable: PATH first, then PlatformIO's own installs under `home` (a GUI app often lacks the shell's PATH).
[[nodiscard]] std::optional<std::filesystem::path> find_pio(const std::filesystem::path& home);

struct ProjectInfo {
    std::filesystem::path dir;
    std::vector<std::string> envs; // [env:NAME] sections, in file order
    std::string default_env;       // first of [platformio] default_envs that exists, else the first env
    // `platform` and `framework` of each env (its own, else the shared [env] section's); an env that says nothing has no entry
    // (it may inherit through `extends`, which is not followed)
    std::map<std::string, std::string> platforms, frameworks;
};

// Reads dir/platformio.ini. On failure `why` says what is wrong (no file, no env, a space in the path).
[[nodiscard]] std::optional<ProjectInfo> read_project(const std::filesystem::path& dir, std::string& why);

// The user's sdkconfig with the agent's keys applied: every `CONFIG_X=..` or `# CONFIG_X is not set` line whose key the
// agent sets is dropped, then the agent's key lines are appended. Other user lines stay, in order.
[[nodiscard]] std::string merge_sdkconfig(std::string_view user, std::string_view agent);

struct Invocation {
    std::vector<std::string> argv;
    std::vector<std::string> env; // "NAME=value", added to the environment of the child
};

// Builds the project with the agent and uploads it. `agent_dir` holds agent/esp32/ (the app's resources, or the repo).
// Writes <project>/.pio/malgrind/sdkconfig.<env>; nothing else of the project is touched. Throws std::runtime_error
// if the agent files are missing.
[[nodiscard]] Invocation agent_build(const std::filesystem::path& pio, const ProjectInfo& project, const std::string& env,
                                     const std::string& port, const std::filesystem::path& agent_dir);

// The user's own firmware, built and uploaded as PlatformIO would: this is what "remove agent" runs.
[[nodiscard]] Invocation plain_flash(const std::filesystem::path& pio, const ProjectInfo& project, const std::string& env,
                                     const std::string& port);

// The ELF agent_build produces (it symbolizes the frames the flashed board sends).
[[nodiscard]] std::filesystem::path agent_elf(const ProjectInfo& project, const std::string& env);

// esptool.py (PlatformIO's copy, run with PlatformIO's Python) as `esptool.py --port P --baud 921600 <args>`.
// nullopt if the launcher's interpreter or the esptool package cannot be found. esptool resets the board, and every
// command prints "Chip is ..." and "MAC: ..." when it connects.
[[nodiscard]] std::optional<Invocation> esptool(const std::filesystem::path& pio, const std::filesystem::path& home,
                                                const std::string& port, std::vector<std::string> args);

// "ESP32-D0WD-V3 (revision v3.1)" etc. through esptool, which resets the board. nullopt if esptool cannot be run.
[[nodiscard]] std::optional<std::string> chip_id(const std::filesystem::path& pio, const std::string& port,
                                                 const std::filesystem::path& home);

} // namespace malgrind::host
