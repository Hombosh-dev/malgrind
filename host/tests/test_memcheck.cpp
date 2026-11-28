// LeakTracker and Symbolizer stack_names self-check: alloc/free replay, leak age, steady-state churn, scrubbing, errors.
#include "malgrind/profiler.hpp"
#include "memcheck.hpp"
#include "os/paths.hpp"
#include "os/tools.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace malgrind;
using namespace malgrind::host;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

AllocRecord make_alloc(std::uint32_t t_ms, std::uint32_t addr, std::uint32_t size, std::vector<std::uint32_t> pcs) {
    return AllocRecord{.op = 0, .t_ms = t_ms, .addr = addr, .size = size, .pcs = std::move(pcs)};
}

AllocRecord make_free(std::uint32_t t_ms, std::uint32_t addr) {
    return AllocRecord{.op = 1, .t_ms = t_ms, .addr = addr, .size = 0, .pcs = {}};
}

void empty_and_basic_alloc_free() {
    Symbolizer sym({}, {});
    LeakTracker tracker(sym);

    assert(json::parse(tracker.json()) ==
           json({{"leaks", json::array()}, {"errors", json::array()}, {"dropped", 0}, {"live_bytes", 0}, {"live_allocs", 0}}));

    // Alloc + matching free clears live state; unmatched free is ignored cleanly.
    tracker.add_allocs({.seq = 1,
                        .records = {
                            make_alloc(100, 0x3f801000, 256, {0x400d1000, 0x400d2000}),
                            make_free(200, 0x3f801000),
                            make_free(250, 0x3f809999),
                        }});

    const auto after_free = json::parse(tracker.json());
    assert(after_free["leaks"].empty() && after_free["errors"].empty());
    assert(after_free["dropped"] == 0 && after_free["live_bytes"] == 0 && after_free["live_allocs"] == 0);

    // Scrubbing back before the free (at_ms = 150) reveals the live allocation.
    const auto before_free = json::parse(tracker.json(150ms));
    assert(before_free["live_bytes"] == 256 && before_free["live_allocs"] == 1);
    assert(before_free["leaks"].size() == 1);
    assert(before_free["leaks"][0]["kind"] == "possibly");
    assert(before_free["leaks"][0]["count"] == 1 && before_free["leaks"][0]["bytes"] == 256);
    assert(before_free["leaks"][0]["oldest_ms"] == 100);
    assert(before_free["leaks"][0]["stack"] == json::array({"0x400d1fff", "0x400d1000"}));
}

void leak_age_grouping_and_sorting() {
    Symbolizer sym({}, {});
    LeakTracker tracker(sym);

    // Stack A: 2 leaks, oldest at 1000ms, total 384 bytes -> at 31000ms age is 30000ms (>= 30s) -> "definitely".
    // Stack B: 1 leak at 1000ms, 512 bytes -> "definitely", larger bytes so sorts before Stack A.
    // Stack C: 1 leak at 5000ms, 1024 bytes -> at 31000ms age is 26000ms (< 30s) -> "possibly", sorts after "definitely".
    tracker.add_allocs({.seq = 1,
                        .records = {
                            make_alloc(1000, 0x1000, 128, {0x400d1000}),
                            make_alloc(2000, 0x1080, 256, {0x400d1000}),
                            make_alloc(1000, 0x2000, 512, {0x400d2000}),
                            make_alloc(5000, 0x3000, 1024, {0x400d3000}),
                            // Short-lived allocation that advances max_t_ms_ to 31000ms and frees itself.
                            make_alloc(30900, 0x4000, 64, {0x400d4000}),
                            make_free(31000, 0x4000),
                        }});

    const auto res = json::parse(tracker.json());
    assert(res["live_allocs"] == 4 && res["live_bytes"] == (128 + 256 + 512 + 1024));
    assert(res["leaks"].size() == 3);

    // First: Stack B ("definitely", 512 bytes)
    assert(res["leaks"][0]["kind"] == "definitely" && res["leaks"][0]["bytes"] == 512 && res["leaks"][0]["count"] == 1);
    assert(res["leaks"][0]["oldest_ms"] == 1000 && res["leaks"][0]["stack"] == json::array({"0x400d2000"}));

    // Second: Stack A ("definitely", 384 bytes, count 2, oldest 1000ms)
    assert(res["leaks"][1]["kind"] == "definitely" && res["leaks"][1]["bytes"] == 384 && res["leaks"][1]["count"] == 2);
    assert(res["leaks"][1]["oldest_ms"] == 1000 && res["leaks"][1]["stack"] == json::array({"0x400d1000"}));

    // Third: Stack C ("possibly", 1024 bytes, age 26s < 30s)
    assert(res["leaks"][2]["kind"] == "possibly" && res["leaks"][2]["bytes"] == 1024 && res["leaks"][2]["count"] == 1);
    assert(res["leaks"][2]["oldest_ms"] == 5000 && res["leaks"][2]["stack"] == json::array({"0x400d3000"}));

    // Boundary: at_ms = 30999ms -> age of Stack A/B is 29999ms (< 30000ms) -> all "possibly", sorted by bytes desc.
    const auto boundary = json::parse(tracker.json(30999ms));
    assert(boundary["leaks"][0]["kind"] == "possibly" && boundary["leaks"][0]["bytes"] == 1024);
    assert(boundary["leaks"][1]["kind"] == "possibly" && boundary["leaks"][1]["bytes"] == 512);
    assert(boundary["leaks"][2]["kind"] == "possibly" && boundary["leaks"][2]["bytes"] == 384);

    // Custom leak_age_s = 25s -> Stack C (age 26s) also becomes "definitely", sorting first due to 1024 bytes.
    const auto custom_age = json::parse(tracker.json(std::nullopt, 25));
    assert(custom_age["leaks"][0]["kind"] == "definitely" && custom_age["leaks"][0]["bytes"] == 1024);
    assert(custom_age["leaks"][1]["kind"] == "definitely" && custom_age["leaks"][1]["bytes"] == 512);
    assert(custom_age["leaks"][2]["kind"] == "definitely" && custom_age["leaks"][2]["bytes"] == 384);
}

void steady_state_top_n_filtering() {
    Symbolizer sym({}, {});
    LeakTracker tracker(sym);

    AllocBatch batch{.seq = 1, .records = {}};
    std::uint32_t next_addr = 0x10000;

    // Create 3 completely-freed stacks with massive churn (live_count == 0); they must NOT crowd out live steady-state stacks.
    for (std::uint32_t s = 0; s < 3; ++s) {
        const std::vector<std::uint32_t> pcs{0x400e0000u + s * 0x10u};
        for (int i = 0; i < 50; ++i) {
            const auto addr = next_addr;
            next_addr += 16;
            batch.records.push_back(make_alloc(100, addr, 32, pcs));
            batch.records.push_back(make_free(101, addr));
        }
    }

    // Create 6 stacks that each have 1 old live allocation (t=500) AND freed_count = 10, 9, 8, 7, 6, 5 (>= live_count).
    // Top 5 (freed_count 10..6) enter steady-state -> "possibly"; the 6th (freed_count 5) -> "definitely".
    for (std::uint32_t s = 0; s < 6; ++s) {
        const std::vector<std::uint32_t> pcs{0x400d1000u + s * 0x100u};
        const std::uint32_t live_addr = next_addr;
        next_addr += 16;
        batch.records.push_back(make_alloc(500, live_addr, 64 + s, pcs));
        const int frees = 10 - static_cast<int>(s);
        for (int f = 0; f < frees; ++f) {
            const auto addr = next_addr;
            next_addr += 16;
            batch.records.push_back(make_alloc(600, addr, 32, pcs));
            batch.records.push_back(make_free(700, addr));
        }
    }

    // Create 1 stack with freed_count = 2 < live_count = 3 -> NOT steady-state -> "definitely".
    {
        const std::vector<std::uint32_t> pcs{0x400d9000};
        for (int f = 0; f < 2; ++f) {
            const auto addr = next_addr;
            next_addr += 16;
            batch.records.push_back(make_alloc(500, addr, 16, pcs));
            batch.records.push_back(make_free(600, addr));
        }
        for (int k = 0; k < 3; ++k) {
            batch.records.push_back(make_alloc(500, next_addr, 100, pcs));
            next_addr += 16;
        }
    }

    // Advance max_t_ms_ beyond 30s.
    batch.records.push_back(make_alloc(35000, next_addr, 16, {0x400d9999}));
    batch.records.push_back(make_free(35001, next_addr));

    tracker.add_allocs(batch);
    const auto res = json::parse(tracker.json());
    assert(res["leaks"].size() == 7);

    // First two must be the two "definitely" leaks: 0x400d9000 (300 bytes) and the 6th steady stack 0x400d1500 (69 bytes).
    assert(res["leaks"][0]["kind"] == "definitely" && res["leaks"][0]["bytes"] == 300 &&
           res["leaks"][0]["stack"] == json::array({"0x400d9000"}));
    assert(res["leaks"][1]["kind"] == "definitely" && res["leaks"][1]["bytes"] == 69 &&
           res["leaks"][1]["stack"] == json::array({"0x400d1500"}));

    // Remaining 5 must be "possibly" (top-5 steady-state churners).
    for (std::size_t i = 2; i < 7; ++i) {
        assert(res["leaks"][i]["kind"] == "possibly");
    }
}

void errors_and_dropped_events() {
    Symbolizer sym({}, {});
    LeakTracker tracker(sym);

    tracker.add_error({1, 1000, kErrHeapCorrupt, 0x3ffb0000, "heap_check", {0x400d1000, 0x400d2000}});
    tracker.add_error({2, 2000, kErrEventsDropped, 7, "events", {}});
    tracker.add_error({3, 3000, kErrStackOverflow, 0x3ffb1234, "sensor", {0x400d3000}});
    tracker.add_error({4, 4000, kErrAllocFailed, 4096, "net", {0x400d4000}});
    tracker.add_error({5, 5000, 99, 42, "custom", {}});
    tracker.add_error({6, 6000, kErrEventsDropped, 5, "events", {}});

    const auto full = json::parse(tracker.json());
    assert(full["dropped"] == 12);
    assert(full["errors"].size() == 4);

    assert(full["errors"][0]["t_ms"] == 1000 && full["errors"][0]["code"] == kErrHeapCorrupt &&
           full["errors"][0]["kind"] == "heap_corrupt" && full["errors"][0]["arg"] == 0x3ffb0000 &&
           full["errors"][0]["name"] == "heap_check" &&
           full["errors"][0]["stack"] == json::array({"0x400d1fff", "0x400d1000"}));

    assert(full["errors"][1]["t_ms"] == 3000 && full["errors"][1]["code"] == kErrStackOverflow &&
           full["errors"][1]["kind"] == "stack_overflow" && full["errors"][1]["name"] == "sensor");

    assert(full["errors"][2]["t_ms"] == 4000 && full["errors"][2]["code"] == kErrAllocFailed &&
           full["errors"][2]["kind"] == "alloc_failed" && full["errors"][2]["arg"] == 4096);

    assert(full["errors"][3]["t_ms"] == 5000 && full["errors"][3]["code"] == 99 &&
           full["errors"][3]["kind"] == "unknown" && full["errors"][3]["stack"] == json::array());

    // Scrubbing to at_ms = 3500ms only emits errors up to 3500ms.
    const auto scrubbed = json::parse(tracker.json(3500ms));
    assert(scrubbed["errors"].size() == 2);
    assert(scrubbed["errors"][0]["kind"] == "heap_corrupt");
    assert(scrubbed["errors"][1]["kind"] == "stack_overflow");
    assert(scrubbed["dropped"] == 12);
}

void symbolized_and_inlined_stacks() {
    const auto root = fs::canonical(fs::temp_directory_path()) / "malgrind_test_memcheck_sym";
    fs::remove_all(root);
    fs::create_directories(root);

    const fs::path tool = root / os::exe_name("addr2line");
    fs::copy_file(MALGRIND_FAKE_TOOL, tool, fs::copy_options::overwrite_existing);

    const fs::path elf = root / "fw.elf";
    {
        std::ofstream out(elf, std::ios::binary);
        // 0x400d1000 has two inlined frames (innermost first): helper_inline -> worker
        out << "0x400d1000=helper_inline\n";
        out << "0x400d1000=worker\n";
        out << "0x400d1fff=app_main\n";
    }

    Symbolizer sym(elf, tool);
    LeakTracker tracker(sym);

    tracker.add_allocs({.seq = 1,
                        .records = {
                            make_alloc(100, 0x3f801000, 128, {0x400d1000, 0x400d2000}),
                            make_alloc(35000, 0x3f802000, 64, {0x400d1000, 0x400d2000}),
                            make_free(35100, 0x3f802000),
                        }});
    tracker.add_error({2, 35200, kErrAllocFailed, 512, "app", {0x400d1000, 0x400d2000}});

    const auto res = json::parse(tracker.json());
    const auto expected_stack = json::array({"app_main", "worker", "helper_inline"});
    assert(res["leaks"].size() == 1);
    assert(res["leaks"][0]["kind"] == "possibly"); // 1 freed >= 1 live (steady-state)
    assert(res["leaks"][0]["stack"] == expected_stack);
    assert(res["errors"].size() == 1 && res["errors"][0]["stack"] == expected_stack);
    assert(sym.stack_names(std::array<std::uint32_t, 2>{0x400d1000, 0x400d2000}) ==
           (std::vector<std::string>{"app_main", "worker", "helper_inline"}));

    fs::remove_all(root);
}

} // namespace

int main() {
    empty_and_basic_alloc_free();
    leak_age_grouping_and_sorting();
    steady_state_top_n_filtering();
    errors_and_dropped_events();
    symbolized_and_inlined_stacks();
    std::puts("memcheck ok");
}
