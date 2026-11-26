// HeapSeries and TaskTable self-check: region naming, ordering, windowing, CPU deltas, and stack warnings.
#include "malgrind/profiler.hpp"
#include "series.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>

using namespace malgrind;
using namespace malgrind::host;
using namespace std::chrono_literals;
using json = nlohmann::json;

namespace {

void heap_series_checks() {
    HeapSeries hs;
    assert(json::parse(hs.json(10s)) == json({{"regions", json::array()}}));

    // Add out-of-order timestamps and unsorted region IDs to verify sorting.
    hs.add(HeapStats{
        .seq = 2,
        .t_ms = 2000,
        .regions = {
            {kHeapTotal, 300'000, 150'000, 120'000, 80'000},
            {kHeapInternal, 200'000, 100'000, 90'000, 60'000},
        },
    });
    hs.add(HeapStats{
        .seq = 1,
        .t_ms = 1000,
        .regions = {
            {42, 1024, 512, 256, 128},
            {kHeapIram, 64'000, 32'000, 30'000, 16'000},
            {kHeapExternal, 4'000'000, 3'500'000, 3'400'000, 2'000'000},
            {kHeapInternal, 200'000, 120'000, 110'000, 70'000},
        },
    });
    hs.add(HeapStats{
        .seq = 3,
        .t_ms = 5000,
        .regions = {
            {kHeapInternal, 200'000, 80'000, 75'000, 40'000},
        },
    });

    const auto all = json::parse(hs.json(0s));
    assert(all["regions"].size() == 5);
    assert(all["regions"][0]["region"] == kHeapInternal && all["regions"][0]["name"] == "internal");
    assert(all["regions"][1]["region"] == kHeapExternal && all["regions"][1]["name"] == "external");
    assert(all["regions"][2]["region"] == kHeapIram && all["regions"][2]["name"] == "iram");
    assert(all["regions"][3]["region"] == 42 && all["regions"][3]["name"] == "region_42");
    assert(all["regions"][4]["region"] == kHeapTotal && all["regions"][4]["name"] == "total");

    // Points are ordered by t_ms and formatted as [t_ms, free, min_free, largest, total].
    const auto& internal_pts = all["regions"][0]["series"];
    assert(internal_pts.size() == 3);
    assert(internal_pts[0] == json::array({1000, 120'000, 110'000, 70'000, 200'000}));
    assert(internal_pts[1] == json::array({2000, 100'000, 90'000, 60'000, 200'000}));
    assert(internal_pts[2] == json::array({5000, 80'000, 75'000, 40'000, 200'000}));

    // Window [2000, 5000] from latest (5000ms) with window = 3s.
    const auto win = json::parse(hs.json(3s));
    assert(win["regions"].size() == 2);
    assert(win["regions"][0]["name"] == "internal" && win["regions"][0]["series"].size() == 2);
    assert(win["regions"][0]["series"][0][0] == 2000 && win["regions"][0]["series"][1][0] == 5000);
    assert(win["regions"][1]["name"] == "total" && win["regions"][1]["series"].size() == 1);

    // Scrubbed at = 2500ms with window = 1500ms -> [1000, 2500].
    const auto scrubbed = json::parse(hs.json(1500ms, 2500ms));
    assert(scrubbed["regions"].size() == 5);
    assert(scrubbed["regions"][0]["series"].size() == 2);
    assert(scrubbed["regions"][0]["series"][0][0] == 1000 && scrubbed["regions"][0]["series"][1][0] == 2000);

    // Scrubbed before first sample -> empty.
    assert(json::parse(hs.json(1s, 500ms)) == json({{"regions", json::array()}}));
}

void task_table_checks() {
    TaskTable tt;
    assert(json::parse(tt.json(10s)) == json({{"tasks", json::array()}}));

    // Snapshot at 1000ms: zero total_runtime delta (initial report).
    tt.add(TaskStats{
        .seq = 1,
        .t_ms = 1000,
        .total_runtime = 0,
        .tasks = {
            {0x1001, "alpha", 5, 0, 1, 0, 1024},
            {0x1002, "beta", 3, 0xff, 2, 0, 600},
            {0x1003, "gamma", 1, 1, 0, 0, 512},
        },
    });

    const auto initial = json::parse(tt.json(0s));
    assert(initial["tasks"].size() == 3);
    for (const auto& t : initial["tasks"]) assert(t["cpu_pct"] == 0.0);
    // Equal cpu_pct (0.0) -> sorted ascending by name: alpha, beta, gamma.
    assert(initial["tasks"][0]["name"] == "alpha");
    assert(initial["tasks"][1]["name"] == "beta" && initial["tasks"][1]["core"].is_null());
    assert(initial["tasks"][2]["name"] == "gamma" && initial["tasks"][2]["stack_hwm"] == 512 && initial["tasks"][2]["warn"] == false);

    // Snapshot at 2000ms: total_runtime = 1000; alpha=300, beta=300, gamma=400; beta drops stack_hwm to 400 (<512).
    tt.add(TaskStats{
        .seq = 2,
        .t_ms = 2000,
        .total_runtime = 1000,
        .tasks = {
            {0x1001, "alpha_renamed", 6, 0, 2, 300, 900},
            {0x1002, "beta", 4, 0xff, 1, 300, 400},
            {0x1003, "gamma", 1, 1, 0, 400, 512},
        },
    });

    // Snapshot at 4000ms: total_runtime = 1000; alpha=500, beta=100, gamma=400; beta recovers stack_hwm=512 (latest),
    // but min across [1000, 4000] is still 400 (<512 -> warn=true).
    tt.add(TaskStats{
        .seq = 3,
        .t_ms = 4000,
        .total_runtime = 1000,
        .tasks = {
            {0x1001, "alpha_renamed", 7, 1, 1, 500, 1000},
            {0x1002, "beta", 4, 0xff, 3, 100, 512},
            {0x1003, "gamma", 1, 1, 0, 400, 511},
        },
    });

    // All snapshots [1000..4000]: sum_total = 2000 -> alpha=800 (40%), gamma=800 (40%), beta=400 (20%).
    // alpha_renamed and gamma tie at 40% -> sorted ascending by name: alpha_renamed before gamma, then beta.
    const auto all = json::parse(tt.json(0s));
    assert(all["tasks"].size() == 3);
    const auto& t0 = all["tasks"][0];
    const auto& t1 = all["tasks"][1];
    const auto& t2 = all["tasks"][2];
    assert(t0["handle"] == 0x1001 && t0["name"] == "alpha_renamed" && t0["prio"] == 7 && t0["core"] == 1 && t0["state"] == 1);
    assert(std::abs(t0["cpu_pct"].get<double>() - 40.0) < 1e-6);
    assert(t0["stack_hwm"] == 900 && t0["warn"] == false);

    assert(t1["handle"] == 0x1003 && t1["name"] == "gamma");
    assert(std::abs(t1["cpu_pct"].get<double>() - 40.0) < 1e-6);
    assert(t1["stack_hwm"] == 511 && t1["warn"] == true);

    assert(t2["handle"] == 0x1002 && t2["name"] == "beta" && t2["core"].is_null() && t2["state"] == 3);
    assert(std::abs(t2["cpu_pct"].get<double>() - 20.0) < 1e-6);
    assert(t2["stack_hwm"] == 400 && t2["warn"] == true);

    // Window = 1s at latest (4000ms) covers only snapshot at 4000ms: alpha=50%, gamma=40%, beta=10%, beta stack_hwm=512 (no warn).
    const auto recent = json::parse(tt.json(1s));
    assert(recent["tasks"][0]["name"] == "alpha_renamed" && std::abs(recent["tasks"][0]["cpu_pct"].get<double>() - 50.0) < 1e-6);
    assert(recent["tasks"][1]["name"] == "gamma" && std::abs(recent["tasks"][1]["cpu_pct"].get<double>() - 40.0) < 1e-6);
    assert(recent["tasks"][2]["name"] == "beta" && std::abs(recent["tasks"][2]["cpu_pct"].get<double>() - 10.0) < 1e-6);
    assert(recent["tasks"][2]["stack_hwm"] == 512 && recent["tasks"][2]["warn"] == false);

    // Scrubbed at = 2500ms with window = 2s covers [1000, 2000]: gamma=40%, alpha_renamed=30%, beta=30%.
    const auto scrubbed = json::parse(tt.json(2s, 2500ms));
    assert(scrubbed["tasks"][0]["name"] == "gamma" && std::abs(scrubbed["tasks"][0]["cpu_pct"].get<double>() - 40.0) < 1e-6);
    assert(scrubbed["tasks"][0]["warn"] == false);
    assert(scrubbed["tasks"][1]["name"] == "alpha_renamed" && std::abs(scrubbed["tasks"][1]["cpu_pct"].get<double>() - 30.0) < 1e-6);
    assert(scrubbed["tasks"][2]["name"] == "beta" && std::abs(scrubbed["tasks"][2]["cpu_pct"].get<double>() - 30.0) < 1e-6);
    assert(scrubbed["tasks"][2]["warn"] == true);
}

} // namespace

int main() {
    heap_series_checks();
    task_table_checks();
    std::puts("series ok");
}
