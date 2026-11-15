#pragma once

// Decoder for the mulcan wire frame (layout documented in core/include/mulcan/profiler.hpp).

#include "mulcan/target_info.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mulcan::host {

struct Stack {
    std::uint16_t count;
    std::vector<std::uint32_t> pcs; // pcs[0] is the innermost frame
};

struct Frame {
    std::uint16_t seq{};
    Arch arch{};
    std::uint8_t ncores{}, core{};
    std::uint32_t cpu_hz{};
    std::uint16_t rate_hz{}, samples{}, dropped{};
    std::uint32_t isr_avg{}, isr_max{}, flush_cyc{};
    std::vector<Stack> stacks;
};

[[nodiscard]] std::string_view arch_name(Arch arch) noexcept;

// Splits a byte stream into frames and everything else. Bytes that are not part of a valid frame
// come back as `text`, so ESP_LOG output sharing the UART still reaches the terminal.
class FrameReader {
public:
    struct Result {
        std::vector<Frame> frames;
        std::string text;
    };
    [[nodiscard]] Result feed(std::span<const std::uint8_t> data);

private:
    std::vector<std::uint8_t> buf_;
};

} // namespace mulcan::host
