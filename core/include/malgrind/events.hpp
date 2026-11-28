#pragma once

// Freestanding fixed-capacity FIFO event queues for alloc/free and error events.
// External synchronization (e.g., platform critical section) is required when
// producers and consumers run concurrently; this header contains no locks.

#include "malgrind/profiler.hpp"

#include <cstddef>
#include <cstdint>

namespace malgrind {

namespace detail {

struct RingCursor {
    std::size_t head{};
    std::size_t tail{};
    std::size_t count{};
};

bool push_alloc_impl(AllocEvent* ring, std::size_t ring_cap, RingCursor& cursor,
                     std::uint32_t& dropped, const AllocEvent& ev) noexcept;
std::size_t pop_allocs_impl(const AllocEvent* ring, std::size_t ring_cap, RingCursor& cursor,
                            AllocEvent* out, std::size_t max_n) noexcept;
bool push_error_impl(ErrorEvent* ring, std::size_t ring_cap, RingCursor& cursor,
                     std::uint32_t& dropped, const ErrorEvent& ev) noexcept;
bool pop_error_impl(const ErrorEvent* ring, std::size_t ring_cap, RingCursor& cursor,
                    ErrorEvent& out) noexcept;

std::size_t drain_allocs_impl(const AllocEvent* ring, std::size_t ring_cap, RingCursor& cursor,
                              std::uint8_t* dst, std::size_t cap, std::uint16_t seq,
                              std::size_t max_batch) noexcept;
std::size_t drain_error_impl(const ErrorEvent* ring, std::size_t ring_cap, RingCursor& cursor,
                             std::uint8_t* dst, std::size_t cap, std::uint16_t seq) noexcept;

} // namespace detail

/// Fixed-capacity ring queues for AllocEvent and ErrorEvent.
/// Callers must serialize concurrent access externally.
template <std::size_t AllocCap = 256, std::size_t ErrorCap = 16>
class EventQueue {
    static_assert(AllocCap > 0, "AllocCap must be > 0");
    static_assert(ErrorCap > 0, "ErrorCap must be > 0");

public:
    bool push_alloc(const AllocEvent& ev) noexcept {
        return detail::push_alloc_impl(allocs_, AllocCap, alloc_cur_, dropped_allocs_, ev);
    }

    bool push_error(const ErrorEvent& ev) noexcept {
        return detail::push_error_impl(errors_, ErrorCap, error_cur_, dropped_errors_, ev);
    }

    std::size_t pop_allocs(AllocEvent* out, std::size_t max_n) noexcept {
        return detail::pop_allocs_impl(allocs_, AllocCap, alloc_cur_, out, max_n);
    }

    bool pop_error(ErrorEvent& out) noexcept {
        return detail::pop_error_impl(errors_, ErrorCap, error_cur_, out);
    }

    /// Returns total dropped alloc + error events since the last call and resets both counters.
    [[nodiscard]] std::uint32_t take_dropped() noexcept {
        const std::uint64_t sum =
            static_cast<std::uint64_t>(dropped_allocs_) + dropped_errors_;
        dropped_allocs_ = 0;
        dropped_errors_ = 0;
        return sum > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<std::uint32_t>(sum);
    }

    [[nodiscard]] std::uint32_t dropped_allocs() const noexcept { return dropped_allocs_; }
    [[nodiscard]] std::uint32_t dropped_errors() const noexcept { return dropped_errors_; }
    [[nodiscard]] std::size_t alloc_count() const noexcept { return alloc_cur_.count; }
    [[nodiscard]] std::size_t error_count() const noexcept { return error_cur_.count; }

    void clear() noexcept {
        alloc_cur_ = {};
        error_cur_ = {};
        dropped_allocs_ = 0;
        dropped_errors_ = 0;
    }

    /// Encodes up to `max_batch` oldest alloc events into an MLC1 frame.
    /// Returns 0 without advancing the queue if empty, `max_batch == 0`, or encoding fails.
    std::size_t drain_allocs(std::uint8_t* dst, std::size_t cap, std::uint16_t seq,
                             std::size_t max_batch = 32) noexcept {
        return detail::drain_allocs_impl(allocs_, AllocCap, alloc_cur_, dst, cap, seq, max_batch);
    }

    /// Encodes the oldest queued error event with sequence `seq` into an MLC1 frame.
    /// Returns 0 without advancing the queue if empty or encoding fails.
    std::size_t drain_error(std::uint8_t* dst, std::size_t cap, std::uint16_t seq) noexcept {
        return detail::drain_error_impl(errors_, ErrorCap, error_cur_, dst, cap, seq);
    }

private:
    AllocEvent allocs_[AllocCap]{};
    ErrorEvent errors_[ErrorCap]{};
    detail::RingCursor alloc_cur_{};
    detail::RingCursor error_cur_{};
    std::uint32_t dropped_allocs_{};
    std::uint32_t dropped_errors_{};
};

} // namespace malgrind
