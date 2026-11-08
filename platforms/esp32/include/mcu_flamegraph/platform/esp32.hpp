#pragma once

#include "mcu_flamegraph/target_info.hpp"

namespace mcu_flamegraph::platform {

struct Esp32Platform {
    [[nodiscard]] static TargetInfo info() noexcept;
    static void init() noexcept;
    static void capture_sample() noexcept;
};

} // namespace mcu_flamegraph::platform
