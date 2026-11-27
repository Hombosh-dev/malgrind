#pragma once

// Time-indexed v2 telemetry series: heap usage per capability region and RTOS task CPU / stack statistics.

#include "wire.hpp"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace malgrind::host {

class HeapSeries {
public:
    using Ms = std::chrono::milliseconds;

    void add(HeapStats stats);

    // JSON {"regions":[{"region":u8,"name":"internal"|"external"|"iram"|"total","series":[[t_ms,free,min_free,largest,total],...]}]}
    // covering samples in [end - window, end], where end = at.value_or(latest_t). window <= 0 includes all samples <= end.
    [[nodiscard]] std::string json(Ms window, std::optional<Ms> at = std::nullopt) const;

private:
    mutable std::mutex mutex_;
    std::vector<HeapStats> samples_;
};

class TaskTable {
public:
    using Ms = std::chrono::milliseconds;

    void add(TaskStats stats);

    // JSON {"tasks":[{"handle":u32,"name":...,"prio":u8,"core":u8|null,"state":u8,"cpu_pct":double,"stack_hwm":u32,"warn":bool}]}
    // aggregated over snapshots in [end - window, end], sorted by cpu_pct desc then name asc.
    [[nodiscard]] std::string json(Ms window, std::optional<Ms> at = std::nullopt) const;

private:
    mutable std::mutex mutex_;
    std::vector<TaskStats> snapshots_;
};

} // namespace malgrind::host
