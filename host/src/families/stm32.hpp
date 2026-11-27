#pragma once

// STM32 (Cortex-M / PlatformIO ststm32) family adapter and OpenOCD helpers.
//
// Agent build injects `core/src`, `platforms/cortexm/src`, and `agent/stm32/autostart.cpp` via PlatformIO's
// `env.BuildSources` into `$BUILD_DIR/malgrind` without modifying the user's project files:
//   PLATFORMIO_BUILD_DIR     <project>/.pio/malgrind/build
//   PLATFORMIO_EXTRA_SCRIPTS pre:<agent-dir>/agent/stm32/inject.py
//   MALGRIND_AGENT_DIR       <agent-dir>/agent/stm32
//   MALGRIND_CORE_DIR        <agent-dir>/core
//   MALGRIND_PLATFORMS_DIR   <agent-dir>/platforms
//   MALGRIND_RATE_HZ         <rate_hz>
//   MALGRIND_START_SAMPLING  1

#include "families/family.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace malgrind::host {

inline constexpr unsigned kStm32AgentBaud = 115200;
inline constexpr std::uint32_t kStm32FlashBase = 0x08000000u;

// Stored in the agent's rodata (agent/stm32/autostart.cpp). A flash image containing either marker is
// our own build, and backing that up would save the agent rather than the user's original firmware.
inline constexpr std::string_view kStm32AgentMarker = "malgrind-agent-v1";
inline constexpr std::string_view kStm32AgentMarkerV2 = "malgrind-agent-v2";

[[nodiscard]] const Family& stm32_family();

// Parses a 32-bit IDCODE / DBGMCU_IDCODE from OpenOCD `mdw`, `mrw`, or `dap info` output.
// Rejects 0 and 0xffffffff as invalid/unconnected values.
[[nodiscard]] std::optional<std::uint32_t> parse_openocd_idcode(std::string_view output);

// Parses flash size in bytes from OpenOCD `flash banks`, `flash probe 0`, or `flash info 0` output.
[[nodiscard]] std::optional<std::uint32_t> parse_openocd_flash_size_bytes(std::string_view output);

// Formats a lowercase hex/dash BoardIdentity::key ([0-9a-f-]+) compatible with backup path validation.
[[nodiscard]] std::string format_stm32_board_key(std::uint32_t idcode, std::uint32_t flash_bytes = 0);

// Builds OpenOCD interface (+ optional adapter serial) + target config arguments for STM32 debug probes.
// Empty `vid`/`pid` defaults to `interface/stlink.cfg`.
[[nodiscard]] std::vector<std::string> stm32_openocd_args(std::string_view vid,
                                                          std::string_view pid,
                                                          std::string_view serial = {},
                                                          std::string_view target_cfg = "target/stm32f4x.cfg");

} // namespace malgrind::host
