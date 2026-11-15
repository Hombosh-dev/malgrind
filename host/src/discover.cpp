#include "discover.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace malgrind::host {

namespace {

bool machine_matches(Arch arch, std::uint16_t machine) {
    switch (arch) {
        case Arch::Host: return machine == 62 || machine == 183 || machine == 3; // x86-64, aarch64, x86
        case Arch::Xtensa: return machine == 94;
        case Arch::ArmV7M: return machine == 40;
        case Arch::RiscV32: return machine == 243;
    }
    return false;
}

// Directories that may hold a build: base and base/firmware/*.
std::vector<fs::path> search_dirs(const fs::path& base) {
    std::vector<fs::path> dirs{base};
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(base / "firmware", ec))
        if (entry.is_directory()) dirs.push_back(entry.path());
    std::ranges::sort(dirs);
    return dirs;
}

} // namespace

std::optional<std::uint16_t> elf_machine(const fs::path& file) {
    std::array<unsigned char, 20> h{};
    std::ifstream in(file, std::ios::binary);
    if (!in.read(reinterpret_cast<char*>(h.data()), h.size())) return std::nullopt;
    if (h[0] != 0x7f || h[1] != 'E' || h[2] != 'L' || h[3] != 'F') return std::nullopt;
    const bool little = h[5] != 2; // EI_DATA: 1 = little endian, 2 = big endian
    return static_cast<std::uint16_t>(little ? h[18] | (h[19] << 8) : (h[18] << 8) | h[19]);
}

std::optional<fs::path> find_project(const fs::path& base, std::string& why) {
    if (fs::exists(base / "platformio.ini")) return base;
    std::vector<fs::path> found;
    for (const auto& dir : search_dirs(base))
        if (dir != base && fs::exists(dir / "platformio.ini")) found.push_back(dir);
    if (found.size() == 1) return found[0];
    why = found.empty() ? "no PlatformIO project found (looked for platformio.ini in . and firmware/*): "
                          "run from the project or pass --project or --flash-cmd"
                        : "several PlatformIO projects found, pass --project:";
    for (const auto& p : found) why += "\n  " + p.string();
    return std::nullopt;
}

std::optional<fs::path> find_elf(Arch arch, const fs::path& base, const fs::path& project) {
    std::vector<fs::path> dirs = search_dirs(base);
    if (!project.empty()) dirs.push_back(project);

    // Builds made by malgrind itself come first: firmware that sends frames carries the agent, and only those builds do.
    std::vector<fs::path> agent_builds, other_builds;
    std::error_code ec;
    for (const auto& dir : dirs) {
        for (const auto& env : fs::directory_iterator(dir / ".pio/malgrind/build", ec))
            agent_builds.push_back(env.path() / "firmware.elf");
        for (const auto& env : fs::directory_iterator(dir / ".pio/build", ec))
            other_builds.push_back(env.path() / "firmware.elf");
        for (const auto& file : fs::directory_iterator(dir / "build", ec))
            if (file.path().extension() == ".elf") other_builds.push_back(file.path());
    }
    auto newest = [&](const std::vector<fs::path>& candidates) {
        std::optional<fs::path> best;
        fs::file_time_type best_time;
        for (const auto& c : candidates) {
            const auto machine = elf_machine(c);
            if (!machine || !machine_matches(arch, *machine)) continue;
            const auto time = fs::last_write_time(c, ec);
            if (!best || time > best_time) best = c, best_time = time;
        }
        return best;
    };
    if (const auto agent = newest(agent_builds)) return agent;
    return newest(other_builds);
}

} // namespace malgrind::host
