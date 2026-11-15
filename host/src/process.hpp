#pragma once

#include <string>
#include <vector>

namespace mulcan::host {

// stdout of the child process (stderr discarded). Empty if it cannot be started.
std::string capture(const std::vector<std::string>& argv);

// Run with inherited stdio. Returns the exit code, or -1 if the process cannot be started.
int run(const std::vector<std::string>& argv);

} // namespace mulcan::host
