#pragma once

// Backups of a board's whole flash, taken before the agent is flashed onto it, so "remove agent" can put the original
// firmware back byte for byte (even when nobody has its source). Layout:
//   <dir>/<mac>/<yyyymmdd-hhmmss>.bin      mac as "24-0a-c4-aa-bb-0c"; a file still being written ends in ".part"
// The helpers here are pure (text parsing and file names); running esptool is ops.cpp's job.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace malgrind::host {

// Stored in the agent's rodata (agent/esp32/malgrind/autostart.cpp). A flash image that contains it is our own build,
// and backing that up would only save the agent as if it were the user's firmware.
inline constexpr std::string_view kAgentMarker = "malgrind-agent-v1";

// "MAC: 24:0a:c4:AA:bb:0c" anywhere in esptool's output -> "24-0a-c4-aa-bb-0c".
[[nodiscard]] std::optional<std::string> parse_mac(std::string_view esptool_output);

// "Detected flash size: 4MB" (or KB) anywhere in the output -> bytes. nullopt for "Unknown" and other units.
[[nodiscard]] std::optional<std::uint32_t> parse_flash_size(std::string_view esptool_output);

// Where to write the backup of `mac` taken at `stamp`: <dir>/<mac>/<stamp>.bin.part (the directory is created by the
// caller). nullopt if `mac` is not of the form above, so it can never name a path outside `dir`.
[[nodiscard]] std::optional<std::filesystem::path> backup_path(const std::filesystem::path& dir, std::string_view mac,
                                                               std::string_view stamp);

// The newest finished backup (*.bin) of that board, nullopt if there is none or `mac` is malformed.
[[nodiscard]] std::optional<std::filesystem::path> latest_backup(const std::filesystem::path& dir, std::string_view mac);

// True if the file contains kAgentMarker. False for an unreadable file too.
[[nodiscard]] bool is_agent_image(const std::filesystem::path& file);

} // namespace malgrind::host
