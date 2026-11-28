#pragma once

#include "wire.hpp"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace malgrind::host {

inline constexpr std::uint32_t kSleepPseudoPc = 0xFFFFFFF0u;

// addr2line for the target's architecture: PATH first, then the toolchains PlatformIO installs.
[[nodiscard]] std::filesystem::path find_addr2line(Arch arch);

// PC -> function name. With no ELF or no tool every name is the raw address.
// ExtraElf entries map non-overlapping or supplementary address ranges (checked in order before the primary ELF).
// prepare() (reader thread) and frames() (HTTP thread) may run concurrently.
struct ExtraElf {
    std::uint32_t start = 0;
    std::uint32_t end = 0;
    std::filesystem::path elf;
};

class Symbolizer {
public:
    Symbolizer(std::filesystem::path elf, std::filesystem::path tool, std::vector<ExtraElf> extra_elfs = {})
        : elf_(std::move(elf)), tool_(std::move(tool)), extra_elfs_(std::move(extra_elfs)) {}

    void configure(std::filesystem::path elf, std::filesystem::path tool, std::vector<ExtraElf> extra_elfs = {});

    void prepare(const Frame& frame); // addr2line calls for all new PCs of the frame
    void prepare(std::span<const std::uint32_t> pcs); // addr2line calls for all new PCs of one stack

    // Function names for one stack entry, innermost first: inlined callees come before the real
    // function that contains them. Without symbols: the raw address.
    [[nodiscard]] std::vector<std::string> frames(std::size_t index, std::uint32_t pc) const;

    // Function names for one stack, outermost first (inlined frames expanded).
    [[nodiscard]] std::vector<std::string> stack_names(std::span<const std::uint32_t> pcs) const;

private:
    void prepare_addrs(std::vector<std::uint32_t> todo);

    std::filesystem::path elf_, tool_;
    std::vector<ExtraElf> extra_elfs_;
    mutable std::mutex mutex_;
    std::unordered_map<std::uint32_t, std::vector<std::string>> cache_;
};

} // namespace malgrind::host
