// malgrind-cli: the engine of the malgrind app. `serve` speaks JSON lines to the app (serve.hpp); `run`, `attach` and
// `detect` are the terminal mode with a live page in the browser (cli.hpp).
#include "cli.hpp"
#include "serve.hpp"
#include "store.hpp"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

#ifndef MALGRIND_DEFAULT_AGENT_DIR
#define MALGRIND_DEFAULT_AGENT_DIR ""
#endif

constexpr std::string_view kServeUsage = "malgrind-cli serve [--data-dir DIR] [--agent-dir DIR]\n";

} // namespace

int main(int argc, char** argv) {
    using namespace malgrind::host;
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty() || (args[0] != "serve" && args[0] != "run" && args[0] != "attach" && args[0] != "detect")) {
        std::cerr << kCliUsage;
        return 2;
    }

    const char* home = std::getenv("HOME");
    const char* agent = std::getenv("MALGRIND_AGENT_DIR");
    ServeOptions options{RunStore::default_root(), agent ? agent : MALGRIND_DEFAULT_AGENT_DIR, home ? home : ""};

    std::signal(SIGPIPE, SIG_IGN); // a closed browser connection or app must not kill us before the run is saved
    // SIGINT/SIGTERM end the blocking read of `serve` (no SA_RESTART) and make attach/run finish: the live run is closed properly
    struct sigaction stop{};
    stop.sa_handler = [](int) { cli_request_stop(); };
    sigaction(SIGINT, &stop, nullptr);
    sigaction(SIGTERM, &stop, nullptr);

    if (args[0] != "serve") return run_cli(args, options);
    for (std::size_t i = 1; i < args.size(); i += 2) {
        if (i + 1 >= args.size() || (args[i] != "--data-dir" && args[i] != "--agent-dir")) {
            std::cerr << kServeUsage;
            return 2;
        }
        (args[i] == "--data-dir" ? options.data_dir : options.agent_dir) = args[i + 1];
    }
    return serve(std::cin, std::cout, options);
}
