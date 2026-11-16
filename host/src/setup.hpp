#pragma once

// Installing PlatformIO for the user. It goes into ~/.platformio/penv (a Python virtual environment of its own, no sudo),
// which is the place find_pio() looks after PATH, and whose Python also runs esptool.

#include "ops.hpp"

#include <filesystem>
#include <optional>
#include <vector>

namespace malgrind::host {

// python3, else python, found on PATH.
[[nodiscard]] std::optional<std::filesystem::path> find_python();

// The two commands: create the environment (skipped when it exists), then pip install platformio into it.
[[nodiscard]] std::vector<Invocation> install_pio_steps(const std::filesystem::path& python, const std::filesystem::path& home);

// Runs the steps, every output line to on_line, and checks that pio exists afterwards. Throws with what to do about the usual
// failures: no venv module ("sudo apt install python3-venv"), no network.
void install_pio(const std::filesystem::path& python, const std::filesystem::path& home, const LineFn& on_line);

} // namespace malgrind::host
