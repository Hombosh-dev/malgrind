#pragma once

#include "wire.hpp"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mulcan::host {

// addr2line for the target's architecture: PATH first, then the toolchains PlatformIO installs.
[[nodiscard]] std::filesystem::path find_addr2line(Arch arch);

// PC -> function name. With no ELF or no tool every name is the raw address.
// prepare() (reader thread) and name() (HTTP thread) may run concurrently.
class Symbolizer {
public:
    Symbolizer(std::filesystem::path elf, std::filesystem::path tool) : elf_(std::move(elf)), tool_(std::move(tool)) {}

    void prepare(const Frame& frame); // one addr2line call for all new PCs of the frame

    // Function names for one stack entry, innermost first: inlined callees come before the real
    // function that contains them. Without symbols: the raw address.
    [[nodiscard]] std::vector<std::string> frames(std::size_t index, std::uint32_t pc) const;

private:
    std::filesystem::path elf_, tool_;
    mutable std::mutex mutex_;
    std::unordered_map<std::uint32_t, std::vector<std::string>> cache_;
};

} // namespace mulcan::host
