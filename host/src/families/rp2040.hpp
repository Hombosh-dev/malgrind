#pragma once

// RP2040 (Raspberry Pi Pico / Cortex-M0+ / PlatformIO raspberrypi) family adapter, OpenOCD SWD, and picotool helpers.
//
// Agent build injects `core/src`, `platforms/cortexm/src`, and `agent/rp2040/autostart.cpp` via PlatformIO's
// `env.BuildSources` into `$BUILD_DIR/malgrind` without modifying the user's project files:
//   PLATFORMIO_BUILD_DIR     <project>/.pio/malgrind/build
//   PLATFORMIO_EXTRA_SCRIPTS pre:<agent-dir>/agent/rp2040/inject.py
//   MALGRIND_AGENT_DIR       <agent-dir>/agent/rp2040
//   MALGRIND_CORE_DIR        <agent-dir>/core
//   MALGRIND_PLATFORMS_DIR   <agent-dir>/platforms
//   MALGRIND_RATE_HZ         <rate_hz>
//   MALGRIND_START_SAMPLING  1

#include "families/family.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace malgrind::host {

inline constexpr unsigned kRp2040AgentBaud = 115200;
inline constexpr std::uint32_t kRp2040FlashBase = 0x10000000u;

// Stored in the agent's rodata (agent/rp2040/autostart.cpp). A flash image containing either marker is
// our own build, and backing that up would save the agent rather than the user's original firmware.
inline constexpr std::string_view kRp2040AgentMarker = "malgrind-agent-v1";
inline constexpr std::string_view kRp2040AgentMarkerV2 = "malgrind-agent-v2";

[[nodiscard]] const Family& rp2040_family();

// Parses `picotool info -a` output into a BoardIdentity (requires non-zero flash size and RP2040/flash metadata).
[[nodiscard]] std::optional<BoardIdentity> parse_picotool_info(std::string_view output);

// Parses flash size in bytes from OpenOCD `flash probe 0`/`flash banks` or `picotool info -a` output.
[[nodiscard]] std::optional<std::uint32_t> parse_rp2040_flash_size_bytes(std::string_view output);

// Builds OpenOCD interface (+ optional adapter serial) + target config arguments for RP2040 debug probes.
// Empty `vid`/`pid` defaults to `interface/cmsis-dap.cfg` with `adapter speed 5000`.
[[nodiscard]] std::vector<std::string> rp2040_openocd_args(std::string_view vid,
                                                            std::string_view pid,
                                                            std::string_view serial = {},
                                                            std::string_view target_cfg = "target/rp2040.cfg");

// Locates `picotool` on PATH, in PlatformIO `tool-picotool*` packages, or in `~/.local/bin` (POSIX).
[[nodiscard]] std::optional<std::filesystem::path> find_picotool(const std::filesystem::path& home);

} // namespace malgrind::host
