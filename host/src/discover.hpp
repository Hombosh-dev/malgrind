#pragma once

// Finding the project and the firmware ELF without being told.

#include "malgrind/target_info.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace malgrind::host {

// PlatformIO project to build: `base` itself, else the only base/firmware/*/platformio.ini.
// On failure `why` explains (none found / several found).
[[nodiscard]] std::optional<std::filesystem::path> find_project(const std::filesystem::path& base, std::string& why);

// Newest firmware ELF built for `arch`, looked up in `project` (if given), `base` and base/firmware/*:
// malgrind's own agent builds (.pio/malgrind/*/firmware.elf) first, then PlatformIO's .pio/build/*/firmware.elf and
// ESP-IDF's build/*.elf. The ELF header's machine type must
// match the architecture the target reports, so a stale or foreign ELF is never picked silently.
[[nodiscard]] std::optional<std::filesystem::path> find_elf(Arch arch, const std::filesystem::path& base,
                                                            const std::filesystem::path& project = {});

// e_machine of an ELF file, nullopt if it is not an ELF.
[[nodiscard]] std::optional<std::uint16_t> elf_machine(const std::filesystem::path& file);

} // namespace malgrind::host
