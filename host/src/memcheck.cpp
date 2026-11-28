#include "memcheck.hpp"

#include "malgrind/profiler.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace malgrind::host {

namespace {

constexpr std::size_t kSteadyStateTopN = 5;

template <class T>
void insert_by_time(std::vector<T>& items, T item) {
    if (items.empty() || item.t_ms >= items.back().t_ms) {
        items.push_back(std::move(item));
        return;
    }
    const auto pos = std::ranges::upper_bound(items, item.t_ms, {}, &T::t_ms);
    items.insert(pos, std::move(item));
}

std::string_view error_kind(std::uint8_t code) noexcept {
    switch (code) {
        case kErrHeapCorrupt: return "heap_corrupt";
        case kErrStackOverflow: return "stack_overflow";
        case kErrAllocFailed: return "alloc_failed";
        default: return "unknown";
    }
}

} // namespace

LeakTracker::LeakTracker(Symbolizer& symbols) : symbols_(symbols) {}

void LeakTracker::add_allocs(const AllocBatch& batch) {
    for (const auto& rec : batch.records) {
        if (!rec.pcs.empty()) symbols_.prepare(rec.pcs);
    }
    std::scoped_lock lock(mutex_);
    for (const auto& rec : batch.records) {
        max_t_ms_ = std::max(max_t_ms_, rec.t_ms);
        insert_by_time(allocs_, rec);
    }
}

void LeakTracker::add_error(const AgentError& err) {
    if (err.code == kErrEventsDropped) {
        std::scoped_lock lock(mutex_);
        dropped_ += err.arg;
        return;
    }
    if (!err.pcs.empty()) symbols_.prepare(err.pcs);
    std::scoped_lock lock(mutex_);
    max_t_ms_ = std::max(max_t_ms_, err.t_ms);
    insert_by_time(errors_, err);
}

void LeakTracker::resymbolize() {
    std::set<std::vector<std::uint32_t>> unique_stacks;
    {
        std::scoped_lock lock(mutex_);
        for (const auto& rec : allocs_) {
            if (!rec.pcs.empty()) unique_stacks.insert(rec.pcs);
        }
        for (const auto& err : errors_) {
            if (!err.pcs.empty()) unique_stacks.insert(err.pcs);
        }
    }
    for (const auto& pcs : unique_stacks) symbols_.prepare(pcs);
}

std::string LeakTracker::json(std::optional<std::chrono::milliseconds> at, unsigned leak_age_s) const {
    std::scoped_lock lock(mutex_);
    const std::uint64_t end_ms =
        at ? static_cast<std::uint64_t>(std::max<long long>(0, at->count())) : static_cast<std::uint64_t>(max_t_ms_);

    struct LiveAlloc {
        std::uint32_t size = 0;
        std::uint32_t t_ms = 0;
        const std::vector<std::uint32_t>* pcs = nullptr;
    };

    std::map<std::vector<std::uint32_t>, std::vector<std::string>> stack_cache;
    auto stack_for = [&](const std::vector<std::uint32_t>& pcs) -> const std::vector<std::string>& {
        auto [it, inserted] = stack_cache.try_emplace(pcs);
        if (inserted) it->second = symbols_.stack_names(pcs);
        return it->second;
    };

    std::unordered_map<std::uint32_t, LiveAlloc> live;
    std::map<std::vector<std::string>, std::uint64_t> freed_by_stack;

    for (const auto& rec : allocs_) {
        if (static_cast<std::uint64_t>(rec.t_ms) > end_ms) break;
        if (rec.op == 0) {
            live[rec.addr] = LiveAlloc{rec.size, rec.t_ms, &rec.pcs};
        } else if (rec.op == 1) {
            if (const auto it = live.find(rec.addr); it != live.end()) {
                ++freed_by_stack[stack_for(*it->second.pcs)];
                live.erase(it);
            }
        }
    }

    struct GroupAcc {
        std::uint64_t count = 0;
        std::uint64_t bytes = 0;
        std::uint32_t oldest_ms = 0;
    };
    std::uint64_t live_bytes = 0;
    const std::uint64_t live_allocs = live.size();
    std::map<std::vector<std::string>, GroupAcc> groups;
    for (const auto& [addr, alloc] : live) {
        (void)addr;
        live_bytes += alloc.size;
        const auto& stack = stack_for(*alloc.pcs);
        auto [it, inserted] = groups.try_emplace(stack);
        if (inserted) {
            it->second = GroupAcc{1, alloc.size, alloc.t_ms};
        } else {
            ++it->second.count;
            it->second.bytes += alloc.size;
            it->second.oldest_ms = std::min(it->second.oldest_ms, alloc.t_ms);
        }
    }

    struct SteadyCandidate {
        std::vector<std::string> stack;
        std::uint64_t freed_count = 0;
    };
    std::vector<SteadyCandidate> steady_candidates;
    for (const auto& [stack, acc] : groups) {
        const auto fit = freed_by_stack.find(stack);
        const std::uint64_t freed_count = fit == freed_by_stack.end() ? 0 : fit->second;
        if (freed_count > 0 && freed_count >= acc.count) {
            steady_candidates.push_back({stack, freed_count});
        }
    }
    std::ranges::sort(steady_candidates, [](const SteadyCandidate& a, const SteadyCandidate& b) {
        if (a.freed_count != b.freed_count) return a.freed_count > b.freed_count;
        return a.stack < b.stack;
    });
    std::set<std::vector<std::string>> steady_top;
    for (std::size_t i = 0; i < std::min(kSteadyStateTopN, steady_candidates.size()); ++i) {
        steady_top.insert(steady_candidates[i].stack);
    }

    struct LeakRow {
        bool definite = false;
        std::vector<std::string> stack;
        std::uint64_t count = 0;
        std::uint64_t bytes = 0;
        std::uint32_t oldest_ms = 0;
    };
    const std::uint64_t age_threshold_ms = static_cast<std::uint64_t>(leak_age_s) * 1000ull;
    std::vector<LeakRow> leak_rows;
    leak_rows.reserve(groups.size());
    for (const auto& [stack, acc] : groups) {
        const bool in_steady = steady_top.contains(stack);
        const bool old_enough = end_ms >= acc.oldest_ms && (end_ms - acc.oldest_ms) >= age_threshold_ms;
        leak_rows.push_back({
            .definite = !in_steady && old_enough,
            .stack = stack,
            .count = acc.count,
            .bytes = acc.bytes,
            .oldest_ms = acc.oldest_ms,
        });
    }
    std::ranges::sort(leak_rows, [](const LeakRow& a, const LeakRow& b) {
        if (a.definite != b.definite) return a.definite > b.definite;
        if (a.bytes != b.bytes) return a.bytes > b.bytes;
        if (a.count != b.count) return a.count > b.count;
        return a.stack < b.stack;
    });

    auto leaks = nlohmann::json::array();
    for (const auto& row : leak_rows) {
        leaks.push_back({
            {"kind", row.definite ? "definitely" : "possibly"},
            {"stack", row.stack},
            {"count", row.count},
            {"bytes", row.bytes},
            {"oldest_ms", row.oldest_ms},
        });
    }

    auto errors = nlohmann::json::array();
    for (const auto& err : errors_) {
        if (static_cast<std::uint64_t>(err.t_ms) > end_ms) break;
        errors.push_back({
            {"t_ms", err.t_ms},
            {"code", err.code},
            {"kind", error_kind(err.code)},
            {"arg", err.arg},
            {"name", err.name},
            {"stack", stack_for(err.pcs)},
        });
    }

    return nlohmann::json{
        {"leaks", std::move(leaks)},
        {"errors", std::move(errors)},
        {"dropped", dropped_},
        {"live_bytes", live_bytes},
        {"live_allocs", live_allocs},
    }
        .dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

} // namespace malgrind::host
