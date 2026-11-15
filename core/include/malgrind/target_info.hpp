#pragma once

#include <cstdint>

namespace malgrind {

// Values are part of the wire format (see docs in profiler.hpp). Never renumber.
enum class Arch : std::uint8_t { Host = 0, Xtensa = 1, ArmV7M = 2, RiscV32 = 3 };

struct TargetInfo {
    const char* name;
    Arch arch;
    std::uint32_t cpu_hz;
    std::uint8_t num_cores;
};

} // namespace malgrind
