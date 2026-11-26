#include "diff.hpp"

#include "session.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace malgrind::host {

namespace {

struct FnPair {
    std::uint64_t a_self = 0;
    std::uint64_t b_self = 0;
    std::uint64_t a_incl = 0;
    std::uint64_t b_incl = 0;
};

struct DiffNode {
    std::string name;
    std::uint64_t a = 0;
    std::uint64_t b = 0;
    std::vector<DiffNode> children;
};

struct DiffFn {
    std::string name;
    double a_self_pct = 0.0;
    double b_self_pct = 0.0;
    double a_incl_pct = 0.0;
    double b_incl_pct = 0.0;
};

double pct(std::uint64_t count, std::uint64_t total) {
    return total == 0 ? 0.0 : 100.0 * static_cast<double>(count) / static_cast<double>(total);
}

FnPair& fn_entry(std::map<std::string, FnPair, std::less<>>& functions, std::string_view name) {
    if (const auto it = functions.find(name); it != functions.end()) return it->second;
    return functions.emplace(std::string(name), FnPair{}).first->second;
}

std::vector<NamedStack> replay_stacks(const RunStore& store, std::string_view id, std::optional<unsigned> core) {
    const auto session = Session::replay(store, id, [](const nlohmann::json&) {});
    if (!session) throw std::runtime_error("no such run: " + std::string(id));
    return session->named_stacks(core);
}

void accumulate_run(const std::vector<NamedStack>& stacks, std::uint64_t& total,
                    std::map<std::string, FnPair, std::less<>>& functions, DiffNode& root,
                    std::uint64_t FnPair::*self_field, std::uint64_t FnPair::*incl_field,
                    std::uint64_t DiffNode::*node_field) {
    for (const auto& stack : stacks) {
        if (stack.frames.empty()) continue;
        total += stack.count;
        fn_entry(functions, stack.frames.back()).*self_field += stack.count;

        // Deduplicate frame names within a stack so recursive calls are counted once in inclusive totals.
        std::vector<std::string_view> unique_names(stack.frames.begin(), stack.frames.end());
        std::ranges::sort(unique_names);
        for (auto it = unique_names.begin(); it != unique_names.end();
             it = std::ranges::upper_bound(it, unique_names.end(), *it)) {
            fn_entry(functions, *it).*incl_field += stack.count;
        }

        root.*node_field += stack.count;
        DiffNode* node = &root;
        for (const auto& frame : stack.frames) {
            auto it = std::ranges::find(node->children, frame, &DiffNode::name);
            if (it == node->children.end()) it = node->children.insert(it, DiffNode{frame, 0, 0, {}});
            node = &*it;
            node->*node_field += stack.count;
        }
    }
}

nlohmann::json tree_to_json(DiffNode& node) {
    std::ranges::sort(node.children, [](const DiffNode& x, const DiffNode& y) {
        const auto mx = std::max(x.b, x.a);
        const auto my = std::max(y.b, y.a);
        if (mx != my) return mx > my;
        return x.name < y.name;
    });
    auto children = nlohmann::json::array();
    for (auto& child : node.children) children.push_back(tree_to_json(child));
    return {
        {"name", std::move(node.name)},
        {"a", node.a},
        {"b", node.b},
        {"value", node.b},
        {"children", std::move(children)},
    };
}

} // namespace

nlohmann::json diff_runs(const RunStore& store, std::string_view id_a, std::string_view id_b,
                         std::optional<unsigned> core) {
    const auto meta_a = store.get(id_a);
    if (!meta_a) throw std::runtime_error("no such run: " + std::string(id_a));
    const auto meta_b = store.get(id_b);
    if (!meta_b) throw std::runtime_error("no such run: " + std::string(id_b));

    const auto stacks_a = replay_stacks(store, id_a, core);
    const auto stacks_b = replay_stacks(store, id_b, core);

    std::uint64_t total_a = 0;
    std::uint64_t total_b = 0;
    std::map<std::string, FnPair, std::less<>> functions;
    DiffNode root{"all", 0, 0, {}};

    accumulate_run(stacks_a, total_a, functions, root, &FnPair::a_self, &FnPair::a_incl, &DiffNode::a);
    accumulate_run(stacks_b, total_b, functions, root, &FnPair::b_self, &FnPair::b_incl, &DiffNode::b);

    std::vector<DiffFn> rows;
    rows.reserve(functions.size());
    for (const auto& [name, counts] : functions) {
        rows.push_back({
            .name = name,
            .a_self_pct = pct(counts.a_self, total_a),
            .b_self_pct = pct(counts.b_self, total_b),
            .a_incl_pct = pct(counts.a_incl, total_a),
            .b_incl_pct = pct(counts.b_incl, total_b),
        });
    }
    std::ranges::sort(rows, [](const DiffFn& x, const DiffFn& y) {
        const double dx_self = std::abs(x.b_self_pct - x.a_self_pct);
        const double dy_self = std::abs(y.b_self_pct - y.a_self_pct);
        if (dx_self != dy_self) return dx_self > dy_self;
        const double dx_incl = std::abs(x.b_incl_pct - x.a_incl_pct);
        const double dy_incl = std::abs(y.b_incl_pct - y.a_incl_pct);
        if (dx_incl != dy_incl) return dx_incl > dy_incl;
        return x.name < y.name;
    });

    auto fn_json = nlohmann::json::array();
    for (const auto& f : rows) {
        fn_json.push_back({
            {"name", f.name},
            {"a_self_pct", f.a_self_pct},
            {"b_self_pct", f.b_self_pct},
            {"a_incl_pct", f.a_incl_pct},
            {"b_incl_pct", f.b_incl_pct},
        });
    }

    return {
        {"a", *meta_a},
        {"b", *meta_b},
        {"total_a", total_a},
        {"total_b", total_b},
        {"functions", std::move(fn_json)},
        {"tree", tree_to_json(root)},
    };
}

} // namespace malgrind::host
