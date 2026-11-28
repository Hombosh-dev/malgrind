// Diff engine self-check: Aggregator/Session named_stacks, diff percentages, sorting, merged tree, recursion, core filter, and error cases.
#include "aggregate.hpp"
#include "diff.hpp"
#include "malgrind/profiler.hpp"
#include "os/paths.hpp"
#include "session.hpp"
#include "store.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

using namespace malgrind;
using namespace malgrind::host;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

struct StackSpec {
    unsigned core;
    std::vector<std::uint32_t> pcs; // innermost first
    unsigned count;
};

fs::path fresh_root(const char* name) {
    const auto p = fs::temp_directory_path() / os::from_utf8(name);
    fs::remove_all(p);
    return p;
}

std::vector<std::uint8_t> encode_samples(std::uint8_t ncores, const std::vector<StackSpec>& specs) {
    Profiler profiler({"test", Arch::Host, 100'000'000, ncores}, 997);
    for (const auto& spec : specs) {
        for (unsigned i = 0; i < spec.count; ++i) {
            profiler.record(spec.core, spec.pcs.data(), static_cast<unsigned>(spec.pcs.size()), 100);
        }
    }
    std::vector<std::uint8_t> stream;
    profiler.flush([&](const std::uint8_t* d, std::size_t n) { stream.insert(stream.end(), d, d + n); }, [] {}, 5000);
    return stream;
}

std::string create_run(RunStore& store, std::string name, std::uint8_t ncores, const std::vector<StackSpec>& specs) {
    auto writer = store.create({.name = std::move(name)});
    const std::string id = writer.meta().id;
    if (!specs.empty()) {
        const auto bytes = encode_samples(ncores, specs);
        writer.append(0, bytes);
        writer.set_target(Arch::Host, ncores, 100.0);
    }
    writer.finish(100);
    return id;
}

bool approx(double a, double b) { return std::abs(a - b) < 1e-9; }

const json& find_fn(const json& functions, std::string_view name) {
    for (const auto& f : functions) {
        if (f.value("name", "") == name) return f;
    }
    assert(false && "function missing from diff output");
    static const json empty;
    return empty;
}

void aggregator_and_session_named_stacks() {
    Symbolizer symbols({}, {});
    Aggregator agg(symbols);

    Frame f0;
    f0.arch = Arch::Host;
    f0.ncores = 2;
    f0.core = 0;
    f0.cpu_hz = 100'000'000;
    f0.stacks = {{0, {}}, {3, {0x30000, 0x20001, 0x10001}}}; // empty pcs stack must be skipped

    Frame f1;
    f1.arch = Arch::Host;
    f1.ncores = 2;
    f1.core = 1;
    f1.cpu_hz = 100'000'000;
    f1.stacks = {{2, {0x40000, 0x10001}}};

    agg.add(f0, 0ms);
    agg.add(f1, 100ms);

    const auto all = agg.named_stacks();
    assert(all.size() == 2);
    assert(all[0].frames == (std::vector<std::string>{"0x10000", "0x20000", "0x30000"}) && all[0].count == 3);
    assert(all[1].frames == (std::vector<std::string>{"0x10000", "0x40000"}) && all[1].count == 2);

    const auto core1 = agg.named_stacks(1u);
    assert(core1.size() == 1 && core1[0].frames == (std::vector<std::string>{"0x10000", "0x40000"}) && core1[0].count == 2);

    const auto root = fresh_root("malgrind_diff_sess");
    RunStore store(root);
    const auto empty_id = create_run(store, "empty", 2, {});
    const auto replay = Session::replay(store, empty_id, [](const json&) {});
    assert(replay && replay->named_stacks().empty());
    fs::remove_all(root);
}

void shifted_counts_functions_and_merged_tree() {
    const auto root = fresh_root("malgrind_diff_a");
    RunStore store(root);

    // Run A (total = 10): core 0 has main->worker->fast (6), main->worker->slow (2); core 1 has main->helper (2).
    const auto id_a = create_run(store, "baseline", 2,
                                 {
                                     {0, {0x30000, 0x20001, 0x10001}, 6},
                                     {0, {0x40000, 0x20001, 0x10001}, 2},
                                     {1, {0x50000, 0x10001}, 2},
                                 });
    // Run B (total = 20): core 0 has main->worker->fast (2), main->worker->slow (14); core 1 has main->helper (4).
    const auto id_b = create_run(store, "optimized-regression", 2,
                                 {
                                     {0, {0x30000, 0x20001, 0x10001}, 2},
                                     {0, {0x40000, 0x20001, 0x10001}, 14},
                                     {1, {0x50000, 0x10001}, 4},
                                 });

    const auto d = diff_runs(store, id_a, id_b);
    assert(d["a"]["id"] == id_a && d["a"]["name"] == "baseline");
    assert(d["b"]["id"] == id_b && d["b"]["name"] == "optimized-regression");
    assert(d["total_a"] == 10 && d["total_b"] == 20);

    const auto& fns = d["functions"];
    assert(fns.size() == 5);
    // |b_self_pct - a_self_pct| == 50 and |b_incl_pct - a_incl_pct| == 50 for both 0x30000 and 0x40000; tie broken by name.
    assert(fns[0]["name"] == "0x30000");
    assert(fns[1]["name"] == "0x40000");
    // Remaining three have 0 self and 0 incl delta, sorted by name.
    assert(fns[2]["name"] == "0x10000" && fns[3]["name"] == "0x20000" && fns[4]["name"] == "0x50000");

    const auto& fast = find_fn(fns, "0x30000");
    assert(approx(fast["a_self_pct"], 60.0) && approx(fast["b_self_pct"], 10.0));
    assert(approx(fast["a_incl_pct"], 60.0) && approx(fast["b_incl_pct"], 10.0));

    const auto& slow = find_fn(fns, "0x40000");
    assert(approx(slow["a_self_pct"], 20.0) && approx(slow["b_self_pct"], 70.0));
    assert(approx(slow["a_incl_pct"], 20.0) && approx(slow["b_incl_pct"], 70.0));

    const auto& worker = find_fn(fns, "0x20000");
    assert(approx(worker["a_self_pct"], 0.0) && approx(worker["b_self_pct"], 0.0));
    assert(approx(worker["a_incl_pct"], 80.0) && approx(worker["b_incl_pct"], 80.0));

    // Merged tree: root -> 0x10000 -> [0x20000 (max 16), 0x50000 (max 4)], and 0x20000 -> [0x40000 (max 14), 0x30000 (max 6)].
    const auto& tree = d["tree"];
    assert(tree["name"] == "all" && tree["a"] == 10 && tree["b"] == 20 && tree["value"] == 20);
    assert(tree["children"].size() == 1);
    const auto& n_main = tree["children"][0];
    assert(n_main["name"] == "0x10000" && n_main["a"] == 10 && n_main["b"] == 20 && n_main["value"] == 20);
    assert(n_main["children"].size() == 2);
    assert(n_main["children"][0]["name"] == "0x20000" && n_main["children"][0]["a"] == 8 &&
           n_main["children"][0]["b"] == 16 && n_main["children"][0]["value"] == 16);
    assert(n_main["children"][1]["name"] == "0x50000" && n_main["children"][1]["a"] == 2 &&
           n_main["children"][1]["b"] == 4 && n_main["children"][1]["value"] == 4);

    const auto& worker_children = n_main["children"][0]["children"];
    assert(worker_children.size() == 2);
    assert(worker_children[0]["name"] == "0x40000" && worker_children[0]["a"] == 2 &&
           worker_children[0]["b"] == 14 && worker_children[0]["value"] == 14);
    assert(worker_children[1]["name"] == "0x30000" && worker_children[1]["a"] == 6 &&
           worker_children[1]["b"] == 2 && worker_children[1]["value"] == 2);

    // Core filter: core 1 sees only main -> helper.
    const auto d_core1 = diff_runs(store, id_a, id_b, 1u);
    assert(d_core1["total_a"] == 2 && d_core1["total_b"] == 4);
    assert(d_core1["functions"].size() == 2);
    assert(d_core1["tree"]["value"] == 4 && d_core1["tree"]["children"][0]["children"].size() == 1);
    assert(d_core1["tree"]["children"][0]["children"][0]["name"] == "0x50000");

    // Core filter: core 0 excludes helper.
    const auto d_core0 = diff_runs(store, id_a, id_b, 0u);
    assert(d_core0["total_a"] == 8 && d_core0["total_b"] == 16);
    assert(d_core0["functions"].size() == 4);

    fs::remove_all(root);
}

void sort_tie_breakers_for_functions_and_tree() {
    const auto root = fresh_root("malgrind_diff_tie");
    RunStore store(root);

    // Run A (10): main->0x20000->0x60000 (5), main->0x30000->0x70000 (5)
    const auto id_a = create_run(store, "a", 1,
                                 {
                                     {0, {0x60000, 0x20001, 0x10001}, 5},
                                     {0, {0x70000, 0x30001, 0x10001}, 5},
                                 });
    // Run B (10): main->0x20000->0x60000 (5), main->0x30000->0x70000 (2), main->0x30000 (3)
    // Both 0x70000 and 0x30000 have |diff_self| = 30%, while 0x70000 has |diff_incl| = 30% vs 0x30000's 0%.
    const auto id_b = create_run(store, "b", 1,
                                 {
                                     {0, {0x60000, 0x20001, 0x10001}, 5},
                                     {0, {0x70000, 0x30001, 0x10001}, 2},
                                     {0, {0x30000, 0x10001}, 3},
                                 });

    const auto d = diff_runs(store, id_a, id_b);
    assert(d["functions"][0]["name"] == "0x70000");
    assert(d["functions"][1]["name"] == "0x30000");

    // In tree under main (0x10000): 0x20000 (a=5,b=5 -> max=5) and 0x30000 (a=5,b=5 -> max=5), sorted by name asc.
    const auto& main_children = d["tree"]["children"][0]["children"];
    assert(main_children.size() == 2);
    assert(main_children[0]["name"] == "0x20000" && main_children[1]["name"] == "0x30000");

    fs::remove_all(root);
}

void recursion_does_not_double_count_incl_pct() {
    const auto root = fresh_root("malgrind_diff_rec");
    RunStore store(root);

    // Run A: main -> r -> r -> leaf (count 5)
    const auto id_a = create_run(store, "rec-a", 1, {{0, {0x50000, 0x20001, 0x20001, 0x10001}, 5}});
    // Run B: main -> r -> r (count 4, recursive function is also the leaf)
    const auto id_b = create_run(store, "rec-b", 1, {{0, {0x20000, 0x20001, 0x10001}, 4}});

    const auto d = diff_runs(store, id_a, id_b);
    assert(d["total_a"] == 5 && d["total_b"] == 4);
    const auto& rec = find_fn(d["functions"], "0x20000");
    assert(approx(rec["a_self_pct"], 0.0) && approx(rec["b_self_pct"], 100.0));
    assert(approx(rec["a_incl_pct"], 100.0) && approx(rec["b_incl_pct"], 100.0)); // never 200%

    // Tree keeps both recursive levels merged.
    const auto& lvl1 = d["tree"]["children"][0];          // 0x10000
    const auto& lvl2 = lvl1["children"][0];               // first 0x20000
    const auto& lvl3 = lvl2["children"][0];               // second 0x20000
    assert(lvl2["name"] == "0x20000" && lvl2["a"] == 5 && lvl2["b"] == 4);
    assert(lvl3["name"] == "0x20000" && lvl3["a"] == 5 && lvl3["b"] == 4);
    assert(lvl3["children"].size() == 1 && lvl3["children"][0]["name"] == "0x50000" &&
           lvl3["children"][0]["a"] == 5 && lvl3["children"][0]["b"] == 0);

    fs::remove_all(root);
}

void empty_runs_and_missing_runs() {
    const auto root = fresh_root("malgrind_diff_empty");
    RunStore store(root);

    const auto empty_a = create_run(store, "empty-a", 1, {});
    const auto empty_b = create_run(store, "empty-b", 1, {});
    const auto non_empty = create_run(store, "non-empty", 1, {{0, {0x30000, 0x10001}, 4}});

    // Both empty.
    const auto both_empty = diff_runs(store, empty_a, empty_b);
    assert(both_empty["total_a"] == 0 && both_empty["total_b"] == 0);
    assert(both_empty["functions"].is_array() && both_empty["functions"].empty());
    assert(both_empty["tree"] == json({{"name", "all"}, {"a", 0}, {"b", 0}, {"value", 0}, {"children", json::array()}}));

    // Empty A vs non-empty B.
    const auto a_empty = diff_runs(store, empty_a, non_empty);
    assert(a_empty["total_a"] == 0 && a_empty["total_b"] == 4);
    assert(a_empty["tree"]["a"] == 0 && a_empty["tree"]["b"] == 4 && a_empty["tree"]["value"] == 4);
    const auto& leaf = find_fn(a_empty["functions"], "0x30000");
    assert(approx(leaf["a_self_pct"], 0.0) && approx(leaf["a_incl_pct"], 0.0));
    assert(approx(leaf["b_self_pct"], 100.0) && approx(leaf["b_incl_pct"], 100.0));

    // Non-empty A vs empty B.
    const auto b_empty = diff_runs(store, non_empty, empty_b);
    assert(b_empty["total_a"] == 4 && b_empty["total_b"] == 0);
    assert(b_empty["tree"]["a"] == 4 && b_empty["tree"]["b"] == 0 && b_empty["tree"]["value"] == 0);

    // Missing runs throw std::runtime_error.
    auto check_throws = [&](std::string_view a, std::string_view b) {
        bool threw = false;
        try {
            (void)diff_runs(store, a, b);
        } catch (const std::runtime_error& e) {
            threw = std::string_view(e.what()).find("no such run") != std::string_view::npos;
        }
        assert(threw);
    };
    check_throws("missing", non_empty);
    check_throws(non_empty, "missing");
    check_throws("../escape", non_empty);

    fs::remove_all(root);
}

} // namespace

int main() {
    aggregator_and_session_named_stacks();
    shifted_counts_functions_and_merged_tree();
    sort_tie_breakers_for_functions_and_tree();
    recursion_does_not_double_count_incl_pct();
    empty_runs_and_missing_runs();
    std::puts("diff ok");
}
