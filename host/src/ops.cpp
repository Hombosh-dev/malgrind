#include "ops.hpp"

#include "process.hpp"
#include "wire.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <thread>

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

} // namespace malgrind::host
