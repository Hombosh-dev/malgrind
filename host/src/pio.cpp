#include "pio.hpp"

#include "process.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace malgrind::host {

namespace {


// pio runs with `-d <project>`, which changes its directory: every path handed to it must be absolute.
fs::path absolute_path(const fs::path& path) {
    auto abs = fs::absolute(path).lexically_normal();
    if (abs.filename().empty() && abs != abs.root_path()) abs = abs.parent_path(); // "/a/b/" -> "/a/b"
    return abs;
}

std::string slurp(const fs::path& file) {
    std::ifstream in(file);
    return {std::istreambuf_iterator<char>(in), {}};
}

std::string trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return std::string(s);
}

std::vector<std::string> lines_of(std::string_view text) {
    std::vector<std::string> lines;
    std::istringstream in{std::string(text)};
    for (std::string line; std::getline(in, line);) lines.push_back(line);
    return lines;
}

// "CONFIG_X=1" and "# CONFIG_X is not set" both set CONFIG_X; anything else is no setting.
std::optional<std::string> sdkconfig_key(const std::string& line) {
    constexpr std::string_view kNotSet = " is not set";
    if (line.starts_with("CONFIG_")) return line.substr(0, line.find('='));
    if (line.starts_with("# CONFIG_") && line.ends_with(kNotSet)) return line.substr(2, line.size() - 2 - kNotSet.size());
    return std::nullopt;
}

} // namespace

std::optional<fs::path> find_in_path(const std::string& name) {
    std::istringstream dirs(std::getenv("PATH") ? std::getenv("PATH") : "");
    for (std::string dir; std::getline(dirs, dir, ':');)
        if (std::error_code ec; !dir.empty() && fs::exists(fs::path(dir) / name, ec)) return fs::path(dir) / name;
    return std::nullopt;
}

std::optional<fs::path> find_pio(const fs::path& home) {
    if (auto found = find_in_path("pio")) return found;
    for (const char* rel : {".platformio/penv/bin/pio", ".local/bin/pio"})
        if (std::error_code ec; fs::exists(home / rel, ec)) return home / rel;
    return std::nullopt;
}

std::optional<ProjectInfo> read_project(const fs::path& given, std::string& why) {
    const fs::path dir = absolute_path(given);
    // espidf.py of PlatformIO: "Detected a whitespace character in project paths" -> stop early with a clear reason
    if (dir.string().find(' ') != std::string::npos) {
        why = "PlatformIO cannot build an ESP-IDF project in a folder whose path contains a space: " + dir.string();
        return std::nullopt;
    }
    std::ifstream in(dir / "platformio.ini");
    if (!in) {
        why = "no platformio.ini in " + dir.string();
        return std::nullopt;
    }
    ProjectInfo info{dir, {}, {}};
    std::vector<std::string> default_envs;
    std::string section;
    std::map<std::string, std::map<std::string, std::string>> settings; // section -> key -> value
    bool in_default_envs = false;
    for (std::string raw; std::getline(in, raw);) {
        const bool indented = !raw.empty() && std::isspace(static_cast<unsigned char>(raw.front()));
        const auto line = trim(raw);
        auto add_names = [&](std::string_view names) {
            std::istringstream words{std::string(names)};
            for (std::string w; std::getline(words, w, ',');)
                for (std::istringstream parts(w); parts >> w;) default_envs.push_back(w);
        };
        if (line.starts_with("[")) {
            section = line.substr(1, line.find(']') - 1);
            in_default_envs = false;
            if (section.starts_with("env:")) info.envs.push_back(trim(section.substr(4)));
        } else if (section == "platformio" && line.starts_with("default_envs")) {
            const auto eq = line.find('=');
            if (eq != std::string::npos) add_names(line.substr(eq + 1));
            in_default_envs = true;
        } else if (in_default_envs && indented && !line.empty() && line.find('=') == std::string::npos) {
            add_names(line); // a multi-line value
        } else {
            in_default_envs = false;
            if (const auto eq = line.find('='); eq != std::string::npos && !indented) settings[section][trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
        }
    }
    for (const auto& env : info.envs) {
        for (const char* key : {"platform", "framework"}) {
            auto& target = std::string_view(key) == "platform" ? info.platforms : info.frameworks;
            for (const auto& section : {"env:" + env, std::string("env")}) {
                const auto s = settings.find(section);
                if (s == settings.end() || !s->second.contains(key)) continue;
                target[env] = s->second.at(key);
                break;
            }
        }
    }
    if (info.envs.empty()) {
        why = "platformio.ini in " + dir.string() + " has no [env:...] section";
        return std::nullopt;
    }
    info.default_env = info.envs.front();
    for (const auto& name : default_envs) {
        if (std::ranges::find(info.envs, name) != info.envs.end()) {
            info.default_env = name;
            break;
        }
    }
    return info;
}

std::string merge_sdkconfig(std::string_view user, std::string_view agent) {
    std::vector<std::string> agent_lines;
    std::set<std::string> agent_keys;
    for (auto& line : lines_of(agent)) {
        if (const auto key = sdkconfig_key(line)) {
            agent_keys.insert(*key);
            agent_lines.push_back(std::move(line));
        }
    }
    std::string out;
    for (const auto& line : lines_of(user)) {
        const auto key = sdkconfig_key(line);
        if (!key || !agent_keys.contains(*key)) out += line + '\n';
    }
    for (const auto& line : agent_lines) out += line + '\n';
    return out;
}

namespace {

struct AgentPaths {
    fs::path files, out, sdkconfig;
};

AgentPaths agent_paths(const ProjectInfo& project, const std::string& env, const fs::path& agent_dir) {
    AgentPaths p;
    p.files = absolute_path(agent_dir) / "agent/esp32";
    if (!fs::exists(p.files / "sdkconfig.agent") || !fs::exists(p.files / "inject.py"))
        throw std::runtime_error("the agent files are missing from " + p.files.string());
    p.out = project.dir / ".pio/malgrind";
    p.sdkconfig = p.out / ("sdkconfig." + env);
    return p;
}

// The part of the agent that CMake reads once, when it configures. PlatformIO does not configure again when a component's
// CMakeLists or Kconfig changes, so a build directory made with other files would keep the old configuration (a Kconfig option
// that a newer malgrind added would simply not exist). The stamp says which files the directory was made with.
std::string agent_stamp(const fs::path& files) {
    std::uint64_t hash = 1469598103934665603ull; // FNV-1a
    for (const char* name : {"malgrind/CMakeLists.txt", "malgrind/Kconfig.projbuild", "inject.py", "sdkconfig.agent"})
        for (const unsigned char c : slurp(files / name)) hash = (hash ^ c) * 1099511628211ull;
    return std::format("{:016x}", hash);
}

} // namespace

void write_agent_sdkconfig(const ProjectInfo& project, const std::string& env, const fs::path& agent_dir, unsigned rate_hz) {
    const auto p = agent_paths(project, env, agent_dir);
    auto agent_keys = slurp(p.files / "sdkconfig.agent");
    if (agent_keys.empty()) throw std::runtime_error("the agent files are missing from " + p.files.string());
    fs::create_directories(p.out);
    if (const auto stamp = agent_stamp(p.files); slurp(p.out / "agent.stamp") != stamp) {
        std::error_code ec;
        fs::remove_all(p.out / "build", ec); // configured with other agent files: start clean (a few minutes, once per malgrind update)
        std::ofstream(p.out / "agent.stamp", std::ios::trunc) << stamp;
    }
    // The rate travels as an sdkconfig key, not as an environment variable: IDF reconfigures and recompiles the agent when
    // the sdkconfig changes, but would keep a stale object when only an environment variable does.
    if (rate_hz != 0) agent_keys += "\nCONFIG_MALGRIND_RATE_HZ=" + std::to_string(rate_hz) + "\n";
    fs::create_directories(p.out);
    // an unchanged file keeps its time stamp: a newer sdkconfig makes ESP-IDF reconfigure and rebuild everything
    const auto merged = merge_sdkconfig(slurp(project.dir / ("sdkconfig." + env)), agent_keys);
    if (slurp(p.sdkconfig) != merged) std::ofstream(p.sdkconfig, std::ios::trunc) << merged;
    if (!fs::exists(p.sdkconfig)) throw std::runtime_error("cannot write " + p.sdkconfig.string());
}

Invocation agent_command(const fs::path& pio, const ProjectInfo& project, const std::string& env, const std::string& port,
                         const fs::path& agent_dir, bool upload) {
    const auto p = agent_paths(project, env, agent_dir);
    std::vector<std::string> argv{pio.string(), "run", "-d", project.dir.string(), "-e", env};
    if (upload) argv.insert(argv.end(), {"-t", "upload", "--upload-port", port});
    return {std::move(argv),
            {"PLATFORMIO_BUILD_DIR=" + (p.out / "build").string(), "PLATFORMIO_EXTRA_SCRIPTS=pre:" + (p.files / "inject.py").string(),
             "MALGRIND_AGENT_DIR=" + p.files.string(), "MALGRIND_SDKCONFIG=" + p.sdkconfig.string()}};
}

Invocation agent_build(const fs::path& pio, const ProjectInfo& project, const std::string& env, const std::string& port,
                       const fs::path& agent_dir, bool upload) {
    write_agent_sdkconfig(project, env, agent_dir);
    return agent_command(pio, project, env, port, agent_dir, upload);
}

Invocation plain_flash(const fs::path& pio, const ProjectInfo& project, const std::string& env, const std::string& port) {
    return {{pio.string(), "run", "-d", project.dir.string(), "-e", env, "-t", "upload", "--upload-port", port}, {}};
}

fs::path agent_elf(const ProjectInfo& project, const std::string& env) {
    return project.dir / ".pio/malgrind/build" / env / "firmware.elf";
}

std::optional<Invocation> esptool(const fs::path& pio, const fs::path& home, const std::string& port, std::vector<std::string> args) {
    const fs::path script_path = home / ".platformio/packages/tool-esptoolpy/esptool.py";
    // PlatformIO's own Python has pyserial; its launcher script names it in the shebang line
    std::ifstream script(pio);
    std::string shebang;
    std::getline(script >> std::ws, shebang);
    if (!shebang.starts_with("#!") || !fs::exists(script_path)) return std::nullopt;
    Invocation inv; // "#!/usr/bin/env python3" is two words
    std::istringstream words(shebang.substr(2));
    for (std::string w; words >> w;) inv.argv.push_back(w);
    for (auto& a : std::vector<std::string>{script_path.string(), "--port", port, "--baud", "921600"}) inv.argv.push_back(std::move(a));
    for (auto& a : args) inv.argv.push_back(std::move(a));
    return inv;
}

std::optional<std::string> chip_id(const fs::path& pio, const std::string& port, const fs::path& home) {
    const auto inv = esptool(pio, home, port, {"chip_id"});
    if (!inv) return std::nullopt;
    const auto out = capture(inv->argv);
    const auto at = out.find("Chip is ");
    if (at == std::string::npos) return std::nullopt;
    return out.substr(at + 8, out.find('\n', at) - at - 8);
}

} // namespace malgrind::host
