#pragma once

#include "mulcan/profiler.hpp"

namespace mulcan::platform {

// Linux port: SIGALRM (hrtimer) + glibc backtrace(). Single thread, "cycles" are nanoseconds.
struct HostPort {
    [[nodiscard]] static TargetInfo info() noexcept;
    [[nodiscard]] static std::uint32_t cycles() noexcept;
    static void start(Profiler& profiler, std::uint16_t rate_hz) noexcept;
};

} // namespace mulcan::platform
