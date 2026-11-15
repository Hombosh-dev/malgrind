#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace malgrind::host {

// stdout of the child process (stderr discarded). Empty if it cannot be started.
std::string capture(const std::vector<std::string>& argv);

// Run with inherited stdio. Returns the exit code, or -1 if the process cannot be started.
int run(const std::vector<std::string>& argv);

// Runs argv with `env_add` ("NAME=value") added to this process's environment. stdout and stderr arrive, in order,
// as lines (a lone \r also ends a line, so progress bars become lines). Returns the exit code, or -1 if it cannot
// be started.
int run_lines(const std::vector<std::string>& argv, const std::vector<std::string>& env_add,
              const std::function<void(std::string_view line)>& on_line);

} // namespace malgrind::host
