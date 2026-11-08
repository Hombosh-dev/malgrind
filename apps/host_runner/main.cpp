#include "mcu_flamegraph/profiler.hpp"
#include "mcu_flamegraph/platform/host.hpp"
#include <iostream>

int main() {
    std::cout << "Starting MCU Flamegraph on Host...\n";

    // Compile-time binding of core engine to the Host simulation platform
    using ProfilerHost = mcu_flamegraph::Profiler<mcu_flamegraph::platform::HostPlatform>;

    ProfilerHost::init();
    ProfilerHost::run_benchmark_cycle();

    std::cout << "Host execution finished.\n";
    return 0;
}
