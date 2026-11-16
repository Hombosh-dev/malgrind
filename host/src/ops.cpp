#include "ops.hpp"

#include "backup.hpp"
#include "discover.hpp"
#include "process.hpp"
#include "store.hpp"
#include "wire.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <thread>
#include <system_error>

namespace malgrind::host {

using namespace std::chrono_literals;

std::runtime_error open_error(const std::string& port, int err) {
    if (err == EBUSY)
        return std::runtime_error("busy: another program (maybe another malgrind, or a serial monitor) is using " + port);
    return std::runtime_error(std::format("cannot open {}: {}", port, std::strerror(err)));
}

Source open_source(const std::string& port) {
    auto source = Source::open(port, kBaud);
    if (!source) throw open_error(port, errno);
    return std::move(*source);
}

Source open_after_upload(const std::string& port) {
    for (int attempt = 0;; ++attempt) {
        auto source = Source::open(port, kBaud);
        if (source) return std::move(*source);
        const int err = errno;
        const bool transient = err == ENOENT || err == ENODEV || err == EIO || err == ENXIO;
        if (!transient || attempt >= 20) throw open_error(port, err);
        std::this_thread::sleep_for(250ms);
    }
}

Prepared prepare_project(const std::filesystem::path& home, const std::filesystem::path& dir, std::string env, std::string port) {
    const auto pio = find_pio(home);
    if (!pio) throw std::runtime_error(kNoPio);
    std::string why;
    auto project = read_project(dir, why);
    if (!project) throw std::runtime_error(why);
    if (env.empty()) env = project->default_env;
    if (std::ranges::find(project->envs, env) == project->envs.end())
        throw std::runtime_error(std::format("platformio.ini has no environment '{}'", env));
    // The agent is an ESP-IDF component for ESP32. Anything else would build and flash, and then simply show nothing.
    const auto platform = project->platforms.find(env);
    if (platform != project->platforms.end() && !platform->second.starts_with("espressif32"))
        throw std::runtime_error(std::format("environment '{}' uses platform '{}': malgrind supports ESP32 (platform espressif32) only", env, platform->second));
    const auto framework = project->frameworks.find(env);
    if (framework != project->frameworks.end() && framework->second.find("espidf") == std::string::npos)
        throw std::runtime_error(std::format("environment '{}' uses framework '{}': the malgrind agent needs the ESP-IDF framework (framework = espidf)", env, framework->second));
    return {*pio, std::move(*project), std::move(env), std::move(port)};
}

void run_pio(const Invocation& invocation, const std::function<void(std::string_view line)>& on_line) {
    const int code = run_lines(invocation.argv, invocation.env, on_line);
    if (code != 0) throw std::runtime_error(code < 0 ? "cannot start pio" : std::format("build or upload failed (pio exit code {})", code));
}

ProbeResult probe_port(const std::string& port) {
    auto source = Source::open(port, kBaud);
    if (!source) {
        if (errno == EBUSY) return {.busy = true};
        throw open_error(port, errno);
    }
    FrameReader reader;
    std::array<std::uint8_t, 4096> buf;
    for (const auto end = std::chrono::steady_clock::now() + 2s; std::chrono::steady_clock::now() < end;) {
        const auto n = source->read(buf, 100);
        if (!n) break;
        if (const auto frames = reader.feed(std::span(buf).first(*n)).frames; !frames.empty())
            return {.agent = true, .arch = frames[0].arch, .ncores = frames[0].ncores, .cpu_mhz = frames[0].cpu_hz / 1e6};
    }
    return {};
}

namespace {

namespace fs = std::filesystem;

// An esptool command, its output forwarded line by line and also returned as one text. Throws if it fails.
std::string run_esptool(const fs::path& pio, const fs::path& home, const std::string& port, std::vector<std::string> args,
                        const LineFn& on_line) {
    const auto inv = esptool(pio, home, port, std::move(args));
    if (!inv) throw std::runtime_error("esptool was not found: PlatformIO has not downloaded it yet (build any ESP32 project once)");
    std::string all;
    const int code = run_lines(inv->argv, inv->env, [&](std::string_view line) {
        all.append(line).append("\n");
        on_line(line);
    });
    if (code != 0) throw std::runtime_error(std::format("esptool failed (exit code {}): is the board connected and not in use?\n{}", code, all));
    return all;
}

struct Board {
    std::string mac;
    std::uint32_t flash_bytes;
};

Board identify(const fs::path& pio, const fs::path& home, const std::string& port, const LineFn& on_line) {
    const auto out = run_esptool(pio, home, port, {"flash_id"}, on_line);
    const auto mac = parse_mac(out);
    const auto size = parse_flash_size(out);
    if (!mac || !size) throw std::runtime_error("cannot tell which board this is (esptool did not report its MAC address and flash size)");
    return {*mac, *size};
}

} // namespace

void flash_agent(const Prepared& p, const fs::path& home, const fs::path& agent_dir, const fs::path& backup_dir, const LineFn& on_line) {
    const auto probe = probe_port(p.port);
    if (probe.busy) throw open_error(p.port, EBUSY);
    // Build first: a compile error then costs the board neither a reset nor a backup, and PlatformIO has downloaded esptool
    // by the time the backup needs it (on a fresh machine it is not there before the first build).
    run_pio(agent_build(p.pio, p.project, p.env, p.port, agent_dir, false), on_line);
    if (probe.agent) {
        on_line("the board already runs malgrind: no backup taken now");
    } else {
        on_line("backing up the board's flash first (this takes a minute)...");
        const auto board = identify(p.pio, home, p.port, on_line);
        const auto part = backup_path(backup_dir, board.mac, utc_now("%Y%m%d-%H%M%S"));
        std::error_code ec;
        fs::create_directories(part->parent_path(), ec);
        if (ec) throw std::runtime_error(std::format("cannot create {}: {}", part->parent_path().string(), ec.message()));
        try {
            run_esptool(p.pio, home, p.port, {"read_flash", "0", std::to_string(board.flash_bytes), part->string(), "--no-progress"}, on_line);
            if (fs::file_size(*part, ec) != board.flash_bytes || ec) throw std::runtime_error("the backup file is incomplete");
        } catch (...) {
            fs::remove(*part, ec);
            throw;
        }
        if (is_agent_image(*part)) { // a board someone else set up with malgrind: the "original" is not worth keeping
            fs::remove(*part, ec);
            on_line("this board already holds a malgrind build: no backup kept");
        } else {
            auto done = *part;
            done.replace_extension(); // "x.bin.part" -> "x.bin"
            fs::rename(*part, done);
            on_line("backup saved: " + done.string());
        }
    }
    run_pio(agent_build(p.pio, p.project, p.env, p.port, agent_dir, true), on_line); // nothing left to compile
}

void restore_flash(const fs::path& pio, const fs::path& home, const std::string& port, const fs::path& backup_dir, const LineFn& on_line) {
    const auto board = identify(pio, home, port, on_line);
    const auto file = latest_backup(backup_dir, board.mac);
    if (!file) throw std::runtime_error(std::format("no backup of this board (MAC {}): malgrind takes one when it first flashes the agent onto a board. If you have this firmware's project, use \"Rebuild my project instead\"", board.mac));
    on_line("restoring " + file->string());
    run_esptool(pio, home, port, {"write_flash", "--flash_mode", "keep", "--flash_freq", "keep", "--flash_size", "keep", "0", file->string()}, on_line);
}

fs::path clone_project(const GithubRepo& repo, const fs::path& dest, const LineFn& on_line) {
    const auto git = find_in_path("git");
    if (!git) throw std::runtime_error("git was not found: install git to open a project from GitHub");
    // protocol.allow=never then https only: whatever the link says, git speaks nothing but https here
    const int code = run_lines({git->string(), "-c", "protocol.allow=never", "-c", "protocol.https.allow=always", "clone", "--depth", "1",
                                "--progress", "--", repo.url, dest.string()},
                               {"GIT_TERMINAL_PROMPT=0"}, on_line);
    std::error_code ec;
    if (code != 0) {
        fs::remove_all(dest, ec);
        throw std::runtime_error(std::format("git clone failed (exit code {}): is the repository public and the link right?", code));
    }
    std::string why;
    const auto project = find_project(dest, why);
    if (!project) {
        fs::remove_all(dest, ec);
        throw std::runtime_error("no platformio.ini found in this repository (looked in its root and in firmware/*)");
    }
    return *project;
}

std::vector<Method> methods(const ProbeResult& probe, bool have_pio, const std::string& project_problem) {
    const std::string busy = "the port is busy: another program is using it";
    std::vector<Method> list;
    list.push_back({"attach", probe.agent, probe.agent ? "" : probe.busy ? busy : "no malgrind agent is running on this board"});
    const bool can_build = have_pio && project_problem.empty() && !probe.busy;
    list.push_back({"new_run", can_build, can_build ? "" : probe.busy ? busy : !have_pio ? kNoPio : project_problem});
    list.push_back({"no_flash", false, "needs a debug probe (JTAG/SWD); not supported yet"});
    return list;
}

} // namespace malgrind::host
