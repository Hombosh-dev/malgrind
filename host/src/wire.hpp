#pragma once

// Decoder for the malgrind wire frame (layout documented in core/include/malgrind/profiler.hpp).

#include "malgrind/target_info.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace malgrind::host {

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

struct Hello {
    std::uint16_t seq{};
    Arch arch{};
    std::uint8_t ncores{};
    std::uint32_t cpu_hz{};
    std::uint16_t agent_ver{};
    std::uint32_t caps{};
    std::uint8_t state{};
    std::uint16_t rate_hz{};
    std::uint32_t analyses{};
};

struct HeapRegionInfo {
    std::uint8_t region{};
    std::uint32_t total{};
    std::uint32_t free{};
    std::uint32_t min_free{};
    std::uint32_t largest{};
};

struct HeapStats {
    std::uint16_t seq{};
    std::uint32_t t_ms{};
    std::vector<HeapRegionInfo> regions;
};

struct TaskInfo {
    std::uint32_t handle{};
    std::string name;
    std::uint8_t prio{};
    std::uint8_t core{};
    std::uint8_t state{};
    std::uint32_t runtime{};
    std::uint32_t stack_hwm{};
};

struct TaskStats {
    std::uint16_t seq{};
    std::uint32_t t_ms{};
    std::uint32_t total_runtime{};
    std::vector<TaskInfo> tasks;
};

struct AllocRecord {
    std::uint8_t op{};
    std::uint32_t t_ms{};
    std::uint32_t addr{};
    std::uint32_t size{};
    std::vector<std::uint32_t> pcs;
};

struct AllocBatch {
    std::uint16_t seq{};
    std::vector<AllocRecord> records;
};

struct AgentError {
    std::uint16_t seq{};
    std::uint32_t t_ms{};
    std::uint8_t code{};
    std::uint32_t arg{};
    std::string name;
    std::vector<std::uint32_t> pcs;
};

using AgentMessage = std::variant<Hello, HeapStats, TaskStats, AllocBatch, AgentError>;

[[nodiscard]] std::string_view arch_name(Arch arch) noexcept;

// Splits a byte stream into frames, v2 control/telemetry messages, and everything else. Bytes that
// are not part of a valid frame come back as `text`, so log output sharing the UART still reaches the terminal.
class FrameReader {
public:
    struct Result {
        std::vector<Frame> frames;
        std::vector<AgentMessage> messages;
        std::string text;
    };
    [[nodiscard]] Result feed(std::span<const std::uint8_t> data);

private:
    std::vector<std::uint8_t> buf_;
};

} // namespace malgrind::host
