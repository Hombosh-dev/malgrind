#include "symbols.hpp"

#include "families/family.hpp"
#include "os/paths.hpp"
#include "os/process.hpp"
#include "os/tools.hpp"

#include <algorithm>
#include <format>
#include <ranges>
#include <sstream>

namespace fs = std::filesystem;

namespace malgrind::host {

namespace {

// Return addresses point after the call; step back one byte so the call stays inside its function.
std::uint32_t lookup_addr(std::size_t index, std::uint32_t pc) { return index == 0 ? pc : pc - 1; }

void append_missing(const std::unordered_map<std::uint32_t, std::vector<std::string>>& cache,
                    std::span<const std::uint32_t> pcs, std::vector<std::uint32_t>& todo) {
    for (std::size_t i = 0; i < pcs.size(); ++i) {
        if (pcs[i] == kSleepPseudoPc) continue;
        if (const auto a = lookup_addr(i, pcs[i]); !cache.contains(a)) todo.push_back(a);
    }
}

std::vector<std::string> tool_names(Arch arch) {
    if (arch == Arch::Host) return {"addr2line"};
    if (const Family* family = family_for_arch(arch)) return family->addr2line_names(arch);
    return {};
}

// "(anonymous namespace)::ns::f(int, char*)" -> "ns::f". addr2line names the same function with and without
// its parameter list (symbol table vs. DWARF), and parameters only make flamegraph labels long.
std::string short_name(std::string name) {
    constexpr std::string_view kAnon = "(anonymous namespace)::";
    for (auto at = name.find(kAnon); at != std::string::npos; at = name.find(kAnon)) name.erase(at, kAnon.size());
    int depth = 0; // inside <...> or {...}: template arguments, lambdas
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (c == '<' || c == '{') ++depth;
        else if (c == '>' || c == '}') --depth;
        else if (c == '(' && depth == 0 && !name.substr(0, i).ends_with("operator")) return name.substr(0, i);
    }
    return name;
}

std::vector<std::vector<std::string>> query_addr2line(const fs::path& tool, const fs::path& elf,
                                                      const std::vector<std::uint32_t>& addrs) {
    std::vector<std::vector<std::string>> chains(addrs.size());
    if (tool.empty() || elf.empty() || addrs.empty()) return chains;

    // -i prints every inlined frame of an address with nothing between addresses, so each address is
    // followed by an invalid one: its "??" answer marks where the previous chain ends.
    constexpr std::size_t kChunk = 128; // keep the command line short
    constexpr const char* kSentinel = "0xffffffff";
    const std::string tool_utf8 = os::to_utf8(tool);
    const std::string elf_utf8 = os::to_utf8(elf);
    for (std::size_t first = 0; first < addrs.size(); first += kChunk) {
        const auto last = std::min(first + kChunk, addrs.size());
        std::vector<std::string> argv{tool_utf8, "-i", "-f", "-C", "-e", elf_utf8};
        for (auto i = first; i < last; ++i) {
            argv.push_back(std::format("0x{:x}", addrs[i]));
            argv.push_back(kSentinel);
        }
        std::istringstream out(os::capture(argv));
        std::string func, location; // addr2line prints "function" then "file:line" per frame
        for (auto i = first; i < last; ++i) {
            bool first_pair = true;
            while (std::getline(out, func) && std::getline(out, location)) {
                if (!func.empty() && func.back() == '\r') func.pop_back();
                if (func == "??" && !first_pair) break; // the sentinel's answer
                first_pair = false;
                if (func != "??") chains[i].push_back(short_name(func));
            }
        }
    }
    return chains;
}

} // namespace

fs::path find_addr2line(Arch arch) {
    std::vector<fs::path> dirs;
    if (const auto path = os::get_env("PATH")) dirs = os::split_path_list(*path);
    for (const auto& core : os::platformio_core_dirs(os::home_dir())) {
        std::error_code ec;
        for (const auto& pkg : fs::directory_iterator(core / "packages", ec))
            if (os::to_utf8(pkg.path().filename()).starts_with("toolchain-")) dirs.push_back(pkg.path() / "bin");
    }
    for (const auto& name : tool_names(arch)) {
        const auto bin = os::exe_name(name);
        for (const auto& dir : dirs)
            if (std::error_code ec; fs::exists(dir / bin, ec)) return dir / bin;
    }
    return {};
}

void Symbolizer::configure(fs::path elf, fs::path tool, std::vector<ExtraElf> extra_elfs) {
    std::scoped_lock lock(mutex_);
    elf_ = std::move(elf);
    tool_ = std::move(tool);
    extra_elfs_ = std::move(extra_elfs);
    cache_.clear();
}

void Symbolizer::prepare(const Frame& frame) {
    std::vector<std::uint32_t> todo;
    {
        std::scoped_lock lock(mutex_);
        for (const auto& stack : frame.stacks) append_missing(cache_, stack.pcs, todo);
    }
    prepare_addrs(std::move(todo));
}

void Symbolizer::prepare(std::span<const std::uint32_t> pcs) {
    if (pcs.empty()) return;
    std::vector<std::uint32_t> todo;
    {
        std::scoped_lock lock(mutex_);
        append_missing(cache_, pcs, todo);
    }
    prepare_addrs(std::move(todo));
}

void Symbolizer::prepare_addrs(std::vector<std::uint32_t> todo) {
    std::ranges::sort(todo);
    todo.erase(std::ranges::unique(todo).begin(), todo.end());
    if (todo.empty()) return;

    fs::path elf, tool;
    std::vector<ExtraElf> extra_elfs;
    {
        std::scoped_lock lock(mutex_);
        elf = elf_;
        tool = tool_;
        extra_elfs = extra_elfs_;
    }

    std::vector<std::vector<std::string>> chains(todo.size());
    if (!tool.empty()) {
        std::vector<bool> extra_available(extra_elfs.size(), false);
        for (std::size_t j = 0; j < extra_elfs.size(); ++j) {
            std::error_code ec;
            extra_available[j] = !extra_elfs[j].elf.empty() && fs::exists(extra_elfs[j].elf, ec);
        }

        struct ExtraGroup {
            fs::path elf;
            std::vector<std::size_t> indices;
            std::vector<std::uint32_t> addrs;
        };
        std::vector<ExtraGroup> extra_groups;
        for (std::size_t i = 0; i < todo.size(); ++i) {
            const std::uint32_t a = todo[i];
            for (std::size_t j = 0; j < extra_elfs.size(); ++j) {
                if (extra_available[j] && a >= extra_elfs[j].start && a <= extra_elfs[j].end) {
                    const fs::path& target = extra_elfs[j].elf;
                    auto it = std::ranges::find_if(extra_groups, [&](const ExtraGroup& g) { return g.elf == target; });
                    if (it == extra_groups.end()) {
                        extra_groups.push_back({target, {i}, {a}});
                    } else {
                        it->indices.push_back(i);
                        it->addrs.push_back(a);
                    }
                    break;
                }
            }
        }

        for (const auto& group : extra_groups) {
            auto resolved = query_addr2line(tool, group.elf, group.addrs);
            for (std::size_t k = 0; k < group.indices.size(); ++k) {
                chains[group.indices[k]] = std::move(resolved[k]);
            }
        }

        if (!elf.empty()) {
            std::vector<std::size_t> primary_indices;
            std::vector<std::uint32_t> primary_addrs;
            for (std::size_t i = 0; i < todo.size(); ++i) {
                if (chains[i].empty()) {
                    primary_indices.push_back(i);
                    primary_addrs.push_back(todo[i]);
                }
            }
            if (!primary_addrs.empty()) {
                auto resolved = query_addr2line(tool, elf, primary_addrs);
                for (std::size_t k = 0; k < primary_indices.size(); ++k) {
                    chains[primary_indices[k]] = std::move(resolved[k]);
                }
            }
        }
    }
    std::scoped_lock lock(mutex_);
    for (std::size_t i = 0; i < todo.size(); ++i)
        cache_[todo[i]] = chains[i].empty() ? std::vector<std::string>{std::format("0x{:x}", todo[i])} : chains[i];
}

std::vector<std::string> Symbolizer::frames(std::size_t index, std::uint32_t pc) const {
    if (pc == kSleepPseudoPc) return {"[sleep]"};
    std::scoped_lock lock(mutex_);
    const auto it = cache_.find(lookup_addr(index, pc));
    return it != cache_.end() ? it->second : std::vector<std::string>{std::format("0x{:x}", pc)};
}

std::vector<std::string> Symbolizer::stack_names(std::span<const std::uint32_t> pcs) const {
    std::vector<std::string> out;
    std::scoped_lock lock(mutex_);
    for (const auto i : std::views::iota(std::size_t{0}, pcs.size()) | std::views::reverse) {
        if (pcs[i] == kSleepPseudoPc) {
            out.emplace_back("[sleep]");
        } else if (const auto it = cache_.find(lookup_addr(i, pcs[i])); it != cache_.end()) {
            for (const auto& name : it->second | std::views::reverse) out.push_back(name);
        } else {
            out.push_back(std::format("0x{:x}", pcs[i]));
        }
    }
    return out;
}

} // namespace malgrind::host
