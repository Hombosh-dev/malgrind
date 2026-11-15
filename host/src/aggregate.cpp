#include "aggregate.hpp"

#include <algorithm>
#include <format>
#include <ranges>
#include <utility>

namespace mulcan::host {

namespace {

using namespace std::chrono_literals;
constexpr auto kKeep = 120s;  // longest window the page can ask for
constexpr auto kStats = 5s;   // window for the rate / overhead figures

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

} // namespace

void Aggregator::add(Frame frame, Clock::time_point now) {
    symbols_.prepare(frame); // addr2line outside the lock
    std::scoped_lock lock(mutex_);
    for (const auto& s : frame.stacks) total_[{frame.core, s.pcs}] += s.count;
    dropped_total_ += frame.dropped;
    last_ = frame;
    recent_.emplace_back(now, std::move(frame));
    while (now - recent_.front().first > kKeep) recent_.pop_front();
}

void Aggregator::add_stack(Node& root, const std::vector<std::uint32_t>& pcs, std::uint64_t count) const {
    root.value += count;
    Node* node = &root;
    for (const auto i : std::views::iota(std::size_t{0}, pcs.size()) | std::views::reverse) { // outermost first
        for (const auto& name : symbols_.frames(i, pcs[i]) | std::views::reverse) { // inlined frames, outermost first
            auto it = std::ranges::find(node->children, name, &Node::name);
            if (it == node->children.end()) it = node->children.insert(it, Node{name, 0, {}});
            it->value += count;
            node = &*it;
        }
    }
}

std::string Aggregator::snapshot_json(std::optional<unsigned> core, std::chrono::seconds window, Clock::time_point now) const {
    std::scoped_lock lock(mutex_);
    Node root{"all", 0, {}};
    if (window.count() == 0) {
        for (const auto& [key, count] : total_)
            if (!core || *core == key.first) add_stack(root, key.second, count);
    } else {
        for (const auto& [t, frame] : recent_)
            if (now - t <= window && (!core || *core == frame.core))
                for (const auto& s : frame.stacks) add_stack(root, s.pcs, s.count);
    }
    std::string out = "{\"tree\":";
    append_tree(out, root);
    out += ",\"stats\":" + stats_json(now) + "}";
    return out;
}

std::string Aggregator::stats_json(Clock::time_point now) const {
    if (!last_) return "{}";
    std::map<unsigned, std::vector<const std::pair<Clock::time_point, Frame>*>> per_core;
    for (const auto& entry : recent_)
        if (now - entry.first <= kStats) per_core[entry.second.core].push_back(&entry);

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
    const Frame& f = *last_;
    return std::format(
        R"({{"arch":"{}","ncores":{},"cpu_mhz":{},"rate_hz":{},"samples_per_s":{:.1f},"dropped":{},"overhead_pct":{},"isr_max_cycles":{}}})",
        arch_name(f.arch), f.ncores, f.cpu_hz / 1e6, f.rate_hz, hits_per_s, dropped_total_,
        cores ? std::format("{:.3f}", 100 * overhead / cores) : "null", f.isr_max);
}

std::string Aggregator::folded() const {
    std::scoped_lock lock(mutex_);
    std::string out;
    for (const auto& [key, count] : total_) {
        out += std::format("core{}", key.first);
        for (const auto i : std::views::iota(std::size_t{0}, key.second.size()) | std::views::reverse)
            for (const auto& name : symbols_.frames(i, key.second[i]) | std::views::reverse) out += ';' + name;
        out += std::format(" {}\n", count);
    }
    return out;
}

} // namespace mulcan::host
