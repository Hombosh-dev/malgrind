#pragma once

// Steps that both front-ends need: the app's JSON protocol (serve.cpp) and the terminal commands (cli.cpp).

#include "malgrind/target_info.hpp"
#include "pio.hpp"
#include "source.hpp"

#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace malgrind::host {

inline constexpr unsigned kBaud = 921600; // what the agent uses

inline constexpr const char* kNoPio =
    "PlatformIO (pio) was not found: install it with `pip install platformio` (attaching to a running board works without it)";

// The error for a port that could not be opened; EBUSY becomes "busy: ...".
[[nodiscard]] std::runtime_error open_error(const std::string& port, int err);

// Opens the port exclusively. Throws open_error.
[[nodiscard]] Source open_source(const std::string& port);

// After an upload the board resets and its USB port can vanish for a moment: keeps trying for about 5 s.
[[nodiscard]] Source open_after_upload(const std::string& port);

struct Prepared {
    std::filesystem::path pio;
    ProjectInfo project;
    std::string env, port;
};
// pio, the project and its environment, checked. `env` empty: the project's default. Throws with a reason.
[[nodiscard]] Prepared prepare_project(const std::filesystem::path& home, const std::filesystem::path& dir, std::string env,
                                       std::string port);

// Runs a pio invocation, handing every output line to on_line. Throws if it fails.
void run_pio(const Invocation& invocation, const std::function<void(std::string_view line)>& on_line);

struct ProbeResult {
    bool busy = false;  // somebody else has the port
    bool agent = false; // a malgrind frame arrived
    Arch arch{};
    unsigned ncores = 0;
    double cpu_mhz = 0;
};
// Listens to the port for up to 2 s for a malgrind frame. Never records and never resets the board. Throws open_error.
[[nodiscard]] ProbeResult probe_port(const std::string& port);

} // namespace malgrind::host
