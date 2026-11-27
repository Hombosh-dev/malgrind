#pragma once

// Chip-agnostic resolution of OpenOCD package, interface, and target config arguments
// from a connected USB debug probe and optional family / PlatformIO project / user override.

#include "devices.hpp"
#include "families/family.hpp"
#include "pio.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace malgrind::host {

struct ProbeConfigRequest {
    Device probe;
    const Family* family = nullptr;         // optional: inferred from project or probe when null
    const ProjectInfo* project = nullptr;   // optional: PlatformIO project for target inference
    std::string env{};                      // optional: project environment (empty -> project->default_env)
    std::string target_cfg_override{};      // optional: explicit OpenOCD target/board config path
};

struct ProbeConfig {
    std::string package;                    // OpenOCD package name (e.g. "tool-openocd")
    std::string interface_desc;             // Human-readable interface description (e.g. "ST-Link")
    std::string target_cfg;                 // Selected OpenOCD target config (e.g. "target/<chip>.cfg")
    std::vector<std::string> openocd_args;  // Arguments passed to OpenOCD (-f <iface>, optional -c ..., -f <target>)
    ProbeSampleMode sample_mode = ProbeSampleMode::Auto;
    std::optional<Arch> target_arch{};

    bool operator==(const ProbeConfig&) const = default;
};

struct ProbeConfigResult {
    std::optional<ProbeConfig> config;
    std::string error;                      // Non-empty explanation when config is nullopt
};

[[nodiscard]] const Family* resolve_probe_family(const Device& probe,
                                                 const ProjectInfo* project = nullptr,
                                                 std::string_view env = {},
                                                 std::string_view target_cfg_override = {},
                                                 const Family* explicit_family = nullptr);

[[nodiscard]] ProbeConfigResult resolve_probe_config(const ProbeConfigRequest& req);

[[nodiscard]] ProbeConfigResult resolve_probe_config(const Device& probe,
                                                     const Family* family = nullptr,
                                                     const ProjectInfo* project = nullptr,
                                                     std::string_view env = {},
                                                     std::string_view target_cfg_override = {});

} // namespace malgrind::host
