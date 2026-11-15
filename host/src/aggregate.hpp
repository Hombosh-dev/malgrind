#pragma once

#include "symbols.hpp"
#include "wire.hpp"

#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace malgrind::host {

// All frames of one run, indexed by time since the run started. The same object serves a live run (frames keep
// arriving) and a replayed one (all frames added up front); every query can look at any moment `at`.
class Aggregator {
public:
    using Ms = std::chrono::milliseconds;

    explicit Aggregator(Symbolizer& symbols) : symbols_(symbols) {}

    void add(Frame frame, Ms t); // t = time since the run started, non-decreasing
    [[nodiscard]] Ms duration() const; // time of the last frame, 0 if none

    // JSON {tree, stats} for the viewer. Looks at frames in [at - window, at]; window 0 = from the start of the run;
    // `at` defaults to duration().
    [[nodiscard]] std::string snapshot_json(std::optional<unsigned> core, Ms window, std::optional<Ms> at = {}) const;

    // JSON {total, functions[{name,self,total}], callers[{name,count}], callees[{name,count}]} for the same range.
    // callers/callees are the frames right outside/inside every occurrence of `focus` (empty focus: none).
    [[nodiscard]] std::string functions_json(std::optional<unsigned> core, Ms window, std::optional<Ms> at,
                                             std::string_view focus) const;

    // Folded stacks ("core0;main;f;g 12"), the input of flamegraph.pl and speedscope.
    [[nodiscard]] std::string folded() const;

private:
    struct Node {
        std::string name;
        std::uint64_t value = 0;
        std::vector<Node> children;
    };
    using Entry = std::pair<Ms, Frame>;

    // Function names of one stack, outermost first, inlined frames expanded.
    [[nodiscard]] std::vector<std::string> names(const std::vector<std::uint32_t>& pcs) const;
    // Calls f(stack, core) for every stack of the frames in range. Caller holds mutex_.
    template <class F> void for_each_stack(std::optional<unsigned> core, Ms window, std::optional<Ms> at, F&& f) const;
    [[nodiscard]] Ms duration_unlocked() const; // caller holds mutex_
    [[nodiscard]] std::string stats_json(Ms at) const;

    Symbolizer& symbols_;
    mutable std::mutex mutex_;
    // ponytail: every frame stays in RAM and each query walks its range; add per-second prefix sums if long runs get slow
    std::vector<Entry> frames_;
    std::vector<std::uint64_t> dropped_prefix_; // dropped_prefix_[i] = dropped samples in frames_[0..i]
};

} // namespace malgrind::host
