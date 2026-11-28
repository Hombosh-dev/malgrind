#pragma once

// Memcheck-like analyses: live-allocation leak tracker (with steady-state churn filtering) and agent error log.

#include "symbols.hpp"
#include "wire.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace malgrind::host {

// Tracks live allocations, steady-state churn, and target memory/stack error events across a run.
class LeakTracker {
public:
    using Ms = std::chrono::milliseconds;

    explicit LeakTracker(Symbolizer& symbols);

    void add_allocs(const AllocBatch& batch);
    void add_error(const AgentError& err);
    void resymbolize();

    // JSON {"leaks":[{"kind":"definitely"|"possibly","stack":[...],"count":u64,"bytes":u64,"oldest_ms":u32}],
    //       "errors":[{"t_ms":u32,"code":u8,"kind":"heap_corrupt"|"stack_overflow"|"alloc_failed"|"unknown",
    //                  "arg":u32,"name":"...","stack":[...]}],
    //       "dropped":u64,"live_bytes":u64,"live_allocs":u64}
    [[nodiscard]] std::string json(std::optional<std::chrono::milliseconds> at = std::nullopt,
                                   unsigned leak_age_s = 30) const;

private:
    Symbolizer& symbols_;
    mutable std::mutex mutex_;
    std::vector<AllocRecord> allocs_;
    std::vector<AgentError> errors_;
    std::uint64_t dropped_ = 0;
    std::uint32_t max_t_ms_ = 0;
};

} // namespace malgrind::host
