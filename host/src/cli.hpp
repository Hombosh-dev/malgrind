#pragma once

// The terminal mode of malgrind-cli: the same engine as the app, with a live page in the browser instead of a window.
//
//   malgrind-cli run     build the project with the agent, flash it, then show the live flamegraph
//   malgrind-cli attach  the board already runs the agent: just show it (no reset)
//   malgrind-cli detect  identify the connected chip
//
// Every run is recorded into the same history the app shows.

#include "serve.hpp"

#include <string>
#include <vector>

namespace malgrind::host {

extern const char* const kCliUsage;

// `args` are the command and its options (argv without the program name). Returns the exit code.
int run_cli(const std::vector<std::string>& args, const ServeOptions& defaults);

// Asks a running attach/run to finish (what Ctrl-C does). Safe to call from a signal handler.
void cli_request_stop() noexcept;

} // namespace malgrind::host
