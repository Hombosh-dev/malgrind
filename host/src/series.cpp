#include "series.hpp"

#include "malgrind/profiler.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <map>
#include <utility>

namespace malgrind::host {

namespace {

using Ms = std::chrono::milliseconds;

template <class T>
void insert_by_time(std::vector<T>& items, T item) {
    if (items.empty() || item.t_ms >= items.back().t_ms) {
        items.push_back(std::move(item));
        return;
    }
    const auto pos = std::ranges::upper_bound(items, item.t_ms, {}, &T::t_ms);
    items.insert(pos, std::move(item));
}

template <class T, class Fn>
void for_each_in_window(const std::vector<T>& items, Ms window, std::optional<Ms> at, Fn&& fn) {
    if (items.empty()) return;
    const Ms end = at.value_or(Ms{items.back().t_ms});
    const auto last = std::ranges::upper_bound(items, end.count(), {}, [](const T& item) {
        return static_cast<Ms::rep>(item.t_ms);
    });
    for (auto it = items.begin(); it != last; ++it) {
        const Ms t{it->t_ms};
        if (window.count() > 0 && end - t > window) continue;
        fn(*it);
    }
}

std::string heap_region_name(std::uint8_t region) {
    switch (region) {
        case kHeapInternal: return "internal";
        case kHeapExternal: return "external";
        case kHeapIram: return "iram";
        case kHeapTotal: return "total";
        default: return std::format("region_{}", region);
    }
}

} // namespace

void HeapSeries::add(HeapStats stats) {
    std::scoped_lock lock(mutex_);
    insert_by_time(samples_, std::move(stats));
}

std::string HeapSeries::json(Ms window, std::optional<Ms> at) const {
    std::scoped_lock lock(mutex_);
    struct Point {
        std::uint32_t t_ms;
        HeapRegionInfo info;
    };
    std::map<std::uint8_t, std::vector<Point>> by_region;
    for_each_in_window(samples_, window, at, [&](const HeapStats& sample) {
        for (const auto& r : sample.regions) {
            by_region[r.region].push_back({sample.t_ms, r});
        }
    });

    auto regions = nlohmann::json::array();
    for (const auto& [region_id, points] : by_region) {
        auto series = nlohmann::json::array();
        for (const auto& pt : points) {
            series.push_back(nlohmann::json::array(
                {pt.t_ms, pt.info.free, pt.info.min_free, pt.info.largest, pt.info.total}));
        }
        regions.push_back({
            {"region", region_id},
            {"name", heap_region_name(region_id)},
            {"series", std::move(series)},
        });
    }
    return nlohmann::json{{"regions", std::move(regions)}}.dump();
}

void TaskTable::add(TaskStats stats) {
    std::scoped_lock lock(mutex_);
    insert_by_time(snapshots_, std::move(stats));
}

std::string TaskTable::json(Ms window, std::optional<Ms> at) const {
    std::scoped_lock lock(mutex_);
    struct Acc {
        std::uint32_t handle = 0;
        std::string name;
        std::uint8_t prio = 0;
        std::uint8_t core = 0xffu;
        std::uint8_t state = 0;
        std::uint64_t sum_runtime = 0;
        std::uint32_t min_stack_hwm = 0;
    };

    std::uint64_t sum_total = 0;
    std::map<std::uint32_t, Acc> by_handle;
    for_each_in_window(snapshots_, window, at, [&](const TaskStats& snap) {
        sum_total += snap.total_runtime;
        for (const auto& t : snap.tasks) {
            auto [it, inserted] = by_handle.try_emplace(t.handle);
            auto& acc = it->second;
            if (inserted) {
                acc.handle = t.handle;
                acc.min_stack_hwm = t.stack_hwm;
            } else {
                acc.min_stack_hwm = std::min(acc.min_stack_hwm, t.stack_hwm);
            }
            acc.name = t.name;
            acc.prio = t.prio;
            acc.core = t.core;
            acc.state = t.state;
            acc.sum_runtime += t.runtime;
        }
    });

    struct Row {
        std::uint32_t handle;
        std::string name;
        std::uint8_t prio;
        std::uint8_t core;
        std::uint8_t state;
        double cpu_pct;
        std::uint32_t stack_hwm;
        bool warn;
    };
    std::vector<Row> rows;
    rows.reserve(by_handle.size());
    for (const auto& [_, acc] : by_handle) {
        const double cpu_pct =
            sum_total > 0 ? 100.0 * static_cast<double>(acc.sum_runtime) / static_cast<double>(sum_total) : 0.0;
        rows.push_back({
            .handle = acc.handle,
            .name = acc.name,
            .prio = acc.prio,
            .core = acc.core,
            .state = acc.state,
            .cpu_pct = cpu_pct,
            .stack_hwm = acc.min_stack_hwm,
            .warn = acc.min_stack_hwm < 512u,
        });
    }

    std::ranges::sort(rows, [](const Row& a, const Row& b) {
        if (a.cpu_pct != b.cpu_pct) return a.cpu_pct > b.cpu_pct;
        if (a.name != b.name) return a.name < b.name;
        return a.handle < b.handle;
    });

    auto tasks = nlohmann::json::array();
    for (const auto& r : rows) {
        tasks.push_back({
            {"handle", r.handle},
            {"name", r.name},
            {"prio", r.prio},
            {"core", r.core == 0xffu ? nlohmann::json(nullptr) : nlohmann::json(r.core)},
            {"state", r.state},
            {"cpu_pct", r.cpu_pct},
            {"stack_hwm", r.stack_hwm},
            {"warn", r.warn},
        });
    }
    return nlohmann::json{{"tasks", std::move(tasks)}}.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

} // namespace malgrind::host
