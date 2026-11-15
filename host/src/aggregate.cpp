#include "aggregate.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <functional>
#include <ranges>
#include <tuple>
#include <utility>

namespace malgrind::host {

namespace {

using namespace std::chrono_literals;
constexpr auto kStats = 5s;      // window for the rate / overhead figures
constexpr std::size_t kTopFunctions = 200, kTopRelated = 50;

void append_json_string(std::string& out, std::string_view s) {
    out += '"';
    for (const char c : s) {
        if (c == '"' || c == '\\') out += '\\', out += c;
        else if (static_cast<unsigned char>(c) < 0x20) out += std::format("\\u{:04x}", static_cast<unsigned>(c));
        else out += c;
    }
    out += '"';
}

template <class Node>
void append_tree(std::string& out, const Node& node) {
    out += "{\"name\":";
    append_json_string(out, node.name);
    out += std::format(",\"value\":{},\"children\":[", node.value);
    for (bool first = true; const auto& child : node.children) {
        if (!std::exchange(first, false)) out += ',';
        append_tree(out, child);
    }
    out += "]}";
}

// The most frequent entries of a name -> count map, largest first (ties by name, so output is stable).
nlohmann::json top_counts(const std::map<std::string, std::uint64_t>& counts, std::size_t limit) {
    std::vector<std::pair<std::string, std::uint64_t>> v(counts.begin(), counts.end());
    std::ranges::stable_sort(v, std::greater{}, &std::pair<std::string, std::uint64_t>::second);
    auto out = nlohmann::json::array();
    for (const auto& [name, count] : v | std::views::take(limit)) out.push_back({{"name", name}, {"count", count}});
    return out;
}

} // namespace

void Aggregator::add(Frame frame, Ms t) {
    symbols_.prepare(frame); // addr2line outside the lock
    std::scoped_lock lock(mutex_);
    dropped_prefix_.push_back((dropped_prefix_.empty() ? 0 : dropped_prefix_.back()) + frame.dropped);
    frames_.emplace_back(t, std::move(frame));
}

Aggregator::Ms Aggregator::duration() const {
    std::scoped_lock lock(mutex_);
    return duration_unlocked();
}

Aggregator::Ms Aggregator::duration_unlocked() const { return frames_.empty() ? Ms{0} : frames_.back().first; }

std::vector<std::string> Aggregator::names(const std::vector<std::uint32_t>& pcs) const {
    std::vector<std::string> out;
    for (const auto i : std::views::iota(std::size_t{0}, pcs.size()) | std::views::reverse) // outermost first
        for (auto& name : symbols_.frames(i, pcs[i]) | std::views::reverse) out.push_back(std::move(name)); // inlined frames too
    return out;
}

template <class F>
void Aggregator::for_each_stack(std::optional<unsigned> core, Ms window, std::optional<Ms> at, F&& f) const {
    if (frames_.empty()) return;
    const Ms end = at.value_or(duration_unlocked());
    auto last = std::ranges::upper_bound(frames_, end, {}, &Entry::first); // frames with t <= end are before it
    for (auto it = frames_.begin(); it != last; ++it) {
        const auto& [t, frame] = *it;
        if (window.count() != 0 && end - t > window) continue;
        if (core && *core != frame.core) continue;
        for (const auto& s : frame.stacks) f(s, frame.core);
    }
}

Aggregator::StackCounts Aggregator::collect(std::optional<unsigned> core, Ms window, std::optional<Ms> at) const {
    StackCounts counts;
    for_each_stack(core, window, at, [&](const Stack& s, unsigned) { counts[s.pcs] += s.count; });
    return counts;
}

std::string Aggregator::snapshot_json(std::optional<unsigned> core, Ms window, std::optional<Ms> at) const {
    std::scoped_lock lock(mutex_);
    Node root{"all", 0, {}};
    for (const auto& [pcs, count] : collect(core, window, at)) {
        root.value += count;
        Node* node = &root;
        for (const auto& name : names(pcs)) {
            auto it = std::ranges::find(node->children, name, &Node::name);
            if (it == node->children.end()) it = node->children.insert(it, Node{name, 0, {}});
            it->value += count;
            node = &*it;
        }
    }
    std::string out = "{\"tree\":";
    append_tree(out, root);
    out += ",\"stats\":" + stats_json(at.value_or(duration_unlocked())) + "}";
    return out;
}

std::string Aggregator::functions_json(std::optional<unsigned> core, Ms window, std::optional<Ms> at,
                                       std::string_view focus) const {
    std::scoped_lock lock(mutex_);
    struct Counts { std::uint64_t self = 0, total = 0; };
    std::map<std::string, Counts> functions;
    std::map<std::string, std::uint64_t> callers, callees;
    std::uint64_t total = 0;
    for (const auto& [pcs, count] : collect(core, window, at)) {
        auto n = names(pcs);
        if (n.empty()) continue;
        total += count;
        functions[n.back()].self += count;
        for (std::size_t i = 0; i < n.size(); ++i) {
            if (n[i] == focus) {
                if (i > 0) callers[n[i - 1]] += count;
                if (i + 1 < n.size()) callees[n[i + 1]] += count;
            }
        }
        std::ranges::sort(n);
        for (auto it = n.begin(); it != n.end(); it = std::ranges::upper_bound(it, n.end(), *it)) functions[*it].total += count;
    }
    std::vector<std::pair<std::string, Counts>> sorted(functions.begin(), functions.end());
    std::ranges::stable_sort(sorted, [](const auto& a, const auto& b) {
        return std::tie(b.second.self, b.second.total) < std::tie(a.second.self, a.second.total);
    });
    auto fn = nlohmann::json::array();
    for (const auto& [name, c] : sorted | std::views::take(kTopFunctions)) fn.push_back({{"name", name}, {"self", c.self}, {"total", c.total}});
    return nlohmann::json{{"total", total}, {"functions", fn}, {"callers", top_counts(callers, kTopRelated)},
                          {"callees", top_counts(callees, kTopRelated)}}
        .dump();
}

std::string Aggregator::stats_json(Ms at) const {
    const auto end = std::ranges::upper_bound(frames_, at, {}, &Entry::first);
    if (end == frames_.begin()) return std::format(R"({{"duration_ms":{}}})", duration_unlocked().count());
    std::map<unsigned, std::vector<const Entry*>> per_core;
    for (auto it = end; it != frames_.begin();) {
        --it;
        if (at - it->first > kStats) break;
        per_core[it->second.core].insert(per_core[it->second.core].begin(), &*it);
    }

    double hits_per_s = 0, overhead = 0;
    unsigned cores = 0;
    for (const auto& [core, frames] : per_core) {
        const double span = std::chrono::duration<double>(frames.back()->first - frames.front()->first).count();
        if (frames.size() < 2 || span <= 0) continue;
        ++cores;
        double hits = 0, cost = 0; // the first frame covers time before the window: skip it
        for (const auto* e : frames | std::views::drop(1)) {
            const Frame& f = e->second;
            hits += f.samples + f.dropped;
            cost += double(f.isr_avg) * (f.samples + f.dropped) + double(f.flush_cyc) / f.ncores;
        }
        hits_per_s += hits / span;
        overhead += cost / (frames.back()->second.cpu_hz * span);
    }
    const Frame& f = std::prev(end)->second;
    const auto dropped = dropped_prefix_[static_cast<std::size_t>(std::prev(end) - frames_.begin())];
    return std::format(
        R"({{"arch":"{}","ncores":{},"cpu_mhz":{},"rate_hz":{},"samples_per_s":{:.1f},"dropped":{},"overhead_pct":{},"isr_max_cycles":{},"duration_ms":{}}})",
        arch_name(f.arch), f.ncores, f.cpu_hz / 1e6, f.rate_hz, hits_per_s, dropped,
        cores ? std::format("{:.3f}", 100 * overhead / cores) : "null", f.isr_max, duration_unlocked().count());
}

std::string Aggregator::folded() const {
    std::scoped_lock lock(mutex_);
    std::map<std::pair<unsigned, std::vector<std::uint32_t>>, std::uint64_t> total; // (core, stack) -> samples
    for_each_stack(std::nullopt, Ms{0}, std::nullopt, [&](const Stack& s, unsigned core) { total[{core, s.pcs}] += s.count; });
    std::string out;
    for (const auto& [key, count] : total) {
        out += std::format("core{}", key.first);
        for (const auto& name : names(key.second)) out += ';' + name;
        out += std::format(" {}\n", count);
    }
    return out;
}

} // namespace malgrind::host
