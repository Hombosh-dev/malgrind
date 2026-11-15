#pragma once

#include "symbols.hpp"
#include "wire.hpp"

#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mulcan::host {

class Aggregator {
public:
    using Clock = std::chrono::steady_clock;

    explicit Aggregator(Symbolizer& symbols) : symbols_(symbols) {}

    void add(Frame frame, Clock::time_point now = Clock::now());

    // JSON {tree, stats} for the viewer. window == 0 means everything since start.
    [[nodiscard]] std::string snapshot_json(std::optional<unsigned> core, std::chrono::seconds window,
                                            Clock::time_point now = Clock::now()) const;

    // Folded stacks ("core0;main;f;g 12"), the input of flamegraph.pl and speedscope.
    [[nodiscard]] std::string folded() const;

private:
    struct Node {
        std::string name;
        std::uint64_t value = 0;
        std::vector<Node> children;
    };
    using Key = std::pair<unsigned, std::vector<std::uint32_t>>; // core, stack

    void add_stack(Node& root, const std::vector<std::uint32_t>& pcs, std::uint64_t count) const;
    [[nodiscard]] std::string stats_json(Clock::time_point now) const;

    Symbolizer& symbols_;
    mutable std::mutex mutex_;
    std::deque<std::pair<Clock::time_point, Frame>> recent_; // last kKeep seconds
    std::map<Key, std::uint64_t> total_;
    std::uint64_t dropped_total_ = 0;
    std::optional<Frame> last_;
};

} // namespace mulcan::host
