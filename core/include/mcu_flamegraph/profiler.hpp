#pragma once

#include "mcu_flamegraph/target_info.hpp"
#include <concepts>
#include <iostream>

namespace mcu_flamegraph {

// Concept that any hardware or simulation platform driver must satisfy
template <typename T>
concept PlatformBackend = requires {
    { T::info() } noexcept -> std::same_as<TargetInfo>;
    { T::init() } noexcept -> std::same_as<void>;
    { T::capture_sample() } noexcept -> std::same_as<void>;
};

// Generic Profiler engine templated on the platform backend
template <PlatformBackend Platform>
class Profiler {
public:
    static void init() noexcept {
        Platform::init();
    }

    static void run_benchmark_cycle() noexcept {
        std::cout << "[Core Engine] Profiling cycle started on " 
                  << Platform::info().platform_name << " (" 
                  << Platform::info().architecture << ")\n";
        Platform::capture_sample();
        std::cout << "[Core Engine] Profiling cycle completed.\n";
    }

    [[nodiscard]] static constexpr TargetInfo target_info() noexcept {
        return Platform::info();
    }
};

} // namespace mcu_flamegraph
