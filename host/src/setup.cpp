#include "setup.hpp"

#include "process.hpp"

#include <format>
#include <stdexcept>

namespace fs = std::filesystem;

namespace malgrind::host {

namespace {
fs::path env_dir(const fs::path& home) { return home / ".platformio/penv"; }
} // namespace

std::optional<fs::path> find_python() {
    if (auto python = find_in_path("python3")) return python;
    return find_in_path("python");
}

std::vector<Invocation> install_pio_steps(const fs::path& python, const fs::path& home) {
    const auto env = env_dir(home);
    std::vector<Invocation> steps;
    // "has a python" is not enough: where the venv package is missing, `venv` fails after it made bin/python, and that
    // half-made environment has no pip. Only an environment that can install packages is reused.
    if (!fs::exists(env / "bin/python") || !fs::exists(env / "bin/pip")) steps.push_back({{python.string(), "-m", "venv", env.string()}, {}});
    steps.push_back({{(env / "bin/python").string(), "-m", "pip", "install", "--disable-pip-version-check", "platformio"}, {}});
    return steps;
}

void install_pio(const fs::path& python, const fs::path& home, const LineFn& on_line) {
    const bool env_existed = fs::exists(env_dir(home));
    const auto steps = install_pio_steps(python, home);
    for (const auto& step : steps) {
        std::string output;
        const int code = run_lines(step.argv, step.env, [&](std::string_view line) {
            output.append(line).append("\n");
            on_line(line);
        });
        if (code == 0) continue;
        const bool venv_step = step.argv.size() > 2 && step.argv[1] == "-m" && step.argv[2] == "venv";
        if (venv_step && !env_existed) { // what we half-made would make the next try fail the same way
            std::error_code ec;
            fs::remove_all(env_dir(home), ec);
        }
        if (venv_step && (output.find("ensurepip") != std::string::npos || output.find("python3-venv") != std::string::npos))
            throw std::runtime_error("Python cannot create virtual environments here: install the venv package (on Ubuntu: sudo apt install python3-venv) and try again");
        if (venv_step) throw std::runtime_error(std::format("could not create {} (python exit code {})", env_dir(home).string(), code));
        throw std::runtime_error(std::format("pip could not install PlatformIO (exit code {}): is the computer online?", code));
    }
    if (!fs::exists(env_dir(home) / "bin/pio")) throw std::runtime_error("the install finished but there is no pio in " + env_dir(home).string());
}

} // namespace malgrind::host
