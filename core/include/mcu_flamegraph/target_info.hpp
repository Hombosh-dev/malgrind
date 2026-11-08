#pragma once

#include <cstdint>
#include <string_view>

namespace mcu_flamegraph {

struct TargetInfo {
    std::string_view platform_name;
    std::string_view architecture;
    std::uint32_t cpu_freq_hz;
    std::uint8_t num_cores;
};

} // namespace mcu_flamegraph
