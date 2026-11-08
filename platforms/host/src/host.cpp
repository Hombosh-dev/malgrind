#include "mcu_flamegraph/platform/host.hpp"
#include <iostream>

namespace mcu_flamegraph::platform {

TargetInfo HostPlatform::info() noexcept {
    return TargetInfo{
        .platform_name = "Host Simulation",
        .architecture = "x86_64",
        .cpu_freq_hz = 3'000'000'000,
        .num_cores = 8,
    };
}

void HostPlatform::init() noexcept {
    std::cout << "[Host HAL] Initialized software timer mock & ring buffer.\n";
}

void HostPlatform::capture_sample() noexcept {
    std::cout << "[Host HAL] Captured simulated stack trace on x86_64.\n";
}

} // namespace mcu_flamegraph::platform
