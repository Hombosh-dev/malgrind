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
    if (!target_) target_ = TargetSummary{frame.arch, frame.ncores, frame.cpu_hz, frame.rate_hz};
    if (!has_agent_status_) agent_state_ = 1;
    dropped_prefix_.push_back((dropped_prefix_.empty() ? 0 : dropped_prefix_.back()) + frame.dropped);
    frames_.emplace_back(t, std::move(frame));
}

void Aggregator::set_target(Arch arch, unsigned ncores, std::uint32_t cpu_hz, std::uint16_t rate_hz) {
    std::scoped_lock lock(mutex_);
    target_ = TargetSummary{arch, ncores, cpu_hz, rate_hz};
}

void Aggregator::set_agent_status(std::uint32_t caps, std::uint8_t agent_state) {
    std::scoped_lock lock(mutex_);
    caps_ = caps;
    agent_state_ = agent_state;
    has_agent_status_ = true;
}

Aggregator::Ms Aggregator::duration() const {
    std::scoped_lock lock(mutex_);
    return duration_unlocked();
}

Aggregator::Ms Aggregator::duration_unlocked() const { return frames_.empty() ? Ms{0} : frames_.back().first; }

std::vector<std::string> Aggregator::names(const std::vector<std::uint32_t>& pcs) const {
    return symbols_.stack_names(pcs);
}

template <class F>
void Aggregator::for_each_frame(std::optional<unsigned> core, Ms window, std::optional<Ms> at, F&& f) const {
    if (frames_.empty()) return;
    const Ms end = at.value_or(duration_unlocked());
    const auto last = std::ranges::upper_bound(frames_, end, {}, &Entry::first); // frames with t <= end are before it
    for (auto it = frames_.begin(); it != last; ++it) {
        const auto& [t, frame] = *it;
        if (window.count() > 0 && end - t > window) continue;
        if (core && *core != frame.core) continue;
        f(t, frame);
    }
}

template <class F>
void Aggregator::for_each_stack(std::optional<unsigned> core, Ms window, std::optional<Ms> at, F&& f) const {
    for_each_frame(core, window, at, [&](Ms, const Frame& frame) {
        for (const auto& s : frame.stacks) f(s, frame.core);
    });
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
    auto series = nlohmann::json::array();
    if (!focus.empty()) {
        std::map<std::int64_t, std::uint64_t> buckets;
        std::map<std::vector<std::uint32_t>, bool> matches_cache;
        for_each_frame(core, window, at, [&](Ms t, const Frame& frame) {
            const std::int64_t bucket_ms = (t.count() / 1000) * 1000;
            auto& bucket = buckets[bucket_ms];
            for (const auto& s : frame.stacks) {
                auto [it, inserted] = matches_cache.try_emplace(s.pcs, false);
                if (inserted) {
                    const auto n = names(s.pcs);
                    it->second = std::ranges::find(n, focus) != n.end();
                }
                if (it->second) bucket += s.count;
            }
        });
        for (const auto& [bucket_ms, count] : buckets) {
            series.push_back(nlohmann::json::array({bucket_ms, count}));
        }
    }
    return nlohmann::json{{"total", total},
                          {"functions", fn},
                          {"callers", top_counts(callers, kTopRelated)},
                          {"callees", top_counts(callees, kTopRelated)},
                          {"series", std::move(series)}}
        .dump();
}

std::string Aggregator::stats_json(Ms at) const {
    const auto end = std::ranges::upper_bound(frames_, at, {}, &Entry::first);
    if (end == frames_.begin()) {
        if (!target_) {
            return std::format(R"({{"duration_ms":{},"caps":{},"agent_state":{}}})", duration_unlocked().count(),
                               caps_, agent_state_);
        }
        return std::format(
            R"({{"arch":"{}","ncores":{},"cpu_mhz":{},"rate_hz":{},"samples_per_s":0.0,"dropped":0,"overhead_pct":null,"isr_max_cycles":0,"duration_ms":{},"caps":{},"agent_state":{}}})",
            arch_name(target_->arch), target_->ncores, target_->cpu_hz / 1e6, target_->rate_hz,
            duration_unlocked().count(), caps_, agent_state_);
    }
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
        R"({{"arch":"{}","ncores":{},"cpu_mhz":{},"rate_hz":{},"samples_per_s":{:.1f},"dropped":{},"overhead_pct":{},"isr_max_cycles":{},"duration_ms":{},"caps":{},"agent_state":{}}})",
        arch_name(f.arch), f.ncores, f.cpu_hz / 1e6, f.rate_hz, hits_per_s, dropped,
        cores ? std::format("{:.3f}", 100 * overhead / cores) : "null", f.isr_max, duration_unlocked().count(),
        caps_, agent_state_);
}

std::string Aggregator::folded(std::optional<unsigned> core, Ms window, std::optional<Ms> at) const {
    std::scoped_lock lock(mutex_);
    std::map<std::pair<unsigned, std::vector<std::uint32_t>>, std::uint64_t> total; // (core, stack) -> samples
    for_each_stack(core, window, at, [&](const Stack& s, unsigned frame_core) {
        if (!s.pcs.empty() && s.count > 0) total[{frame_core, s.pcs}] += s.count;
    });
    std::string out;
    for (const auto& [key, count] : total) {
        out += std::format("core{}", key.first);
        for (const auto& name : names(key.second)) out += ';' + name;
        out += std::format(" {}\n", count);
    }
    return out;
}

std::string Aggregator::speedscope_json(std::string_view name, std::optional<unsigned> core, Ms window,
                                        std::optional<Ms> at) const {
    std::scoped_lock lock(mutex_);
    std::map<unsigned, StackCounts> per_core;
    for_each_stack(core, window, at, [&](const Stack& s, unsigned frame_core) {
        if (!s.pcs.empty() && s.count > 0) per_core[frame_core][s.pcs] += s.count;
    });

    auto shared_frames = nlohmann::json::array();
    std::map<std::string, std::size_t, std::less<>> frame_index;
    auto index_of = [&](const std::string& fn) -> std::size_t {
        if (const auto it = frame_index.find(fn); it != frame_index.end()) return it->second;
        const auto idx = shared_frames.size();
        frame_index.emplace(fn, idx);
        shared_frames.push_back({{"name", fn}});
        return idx;
    };

    auto profiles = nlohmann::json::array();
    for (const auto& [frame_core, stack_counts] : per_core) {
        auto samples = nlohmann::json::array();
        auto weights = nlohmann::json::array();
        std::uint64_t end_value = 0;
        for (const auto& [pcs, count] : stack_counts) {
            const auto resolved = names(pcs);
            if (resolved.empty()) continue;
            auto sample = nlohmann::json::array();
            for (const auto& fn : resolved) sample.push_back(index_of(fn));
            samples.push_back(std::move(sample));
            weights.push_back(count);
            end_value += count;
        }
        if (samples.empty()) continue;
        profiles.push_back({
            {"type", "sampled"},
            {"name", std::format("core{}", frame_core)},
            {"unit", "none"},
            {"startValue", 0},
            {"endValue", end_value},
            {"samples", std::move(samples)},
            {"weights", std::move(weights)},
        });
    }

    nlohmann::json doc = {
        {"$schema", "https://www.speedscope.app/file-format-schema.json"},
        {"version", "0.0.1"},
        {"exporter", std::string("malgrind ") + MALGRIND_VERSION},
        {"name", std::string(name)},
        {"shared", {{"frames", std::move(shared_frames)}}},
        {"profiles", std::move(profiles)},
    };
    if (!doc["profiles"].empty()) doc["activeProfileIndex"] = 0;
    return doc.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

bool Aggregator::has_samples(std::optional<unsigned> core, Ms window, std::optional<Ms> at) const {
    std::scoped_lock lock(mutex_);
    bool found = false;
    for_each_stack(core, window, at, [&](const Stack& s, unsigned) {
        if (!s.pcs.empty() && s.count > 0) found = true;
    });
    return found;
}

std::vector<NamedStack> Aggregator::named_stacks(std::optional<unsigned> core, Ms window, std::optional<Ms> at) const {
    std::scoped_lock lock(mutex_);
    std::vector<NamedStack> out;
    for (const auto& [pcs, count] : collect(core, window, at)) {
        auto frames = names(pcs);
        if (frames.empty()) continue;
        out.push_back(NamedStack{std::move(frames), count});
    }
    return out;
}

} // namespace malgrind::host
