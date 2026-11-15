// malgrind: the sidecar of the malgrind app. `malgrind serve` speaks JSON lines on stdin/stdout (see serve.hpp).
#include "serve.hpp"
#include "store.hpp"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

constexpr std::string_view kUsage = R"(malgrind: live profiler for microcontrollers (sidecar of the malgrind app)

  malgrind serve [--data-dir DIR] [--agent-dir DIR]

Reads one JSON request per line on stdin and answers on stdout; the protocol is described in host/src/serve.hpp.
  --data-dir DIR    where runs are stored (default: the user data directory, .../malgrind/runs)
  --agent-dir DIR   directory that contains agent/esp32/ (default: $MALGRIND_AGENT_DIR)
)";

} // namespace

int main(int argc, char** argv) {
    using namespace malgrind::host;
    if (argc < 2 || std::string_view(argv[1]) != "serve") {
        std::cerr << kUsage;
        return 2;
    }
    const char* home = std::getenv("HOME");
    const char* agent = std::getenv("MALGRIND_AGENT_DIR");
    ServeOptions options{RunStore::default_root(), agent ? agent : "", home ? home : ""};
    for (int i = 2; i < argc; i += 2) {
        const std::string_view key = argv[i];
        if (i + 1 >= argc || (key != "--data-dir" && key != "--agent-dir")) {
            std::cerr << kUsage;
            return 2;
        }
        (key == "--data-dir" ? options.data_dir : options.agent_dir) = argv[i + 1];
    }

    std::signal(SIGPIPE, SIG_IGN); // the app closing its end must not kill us before the run is saved
    // SIGINT/SIGTERM end the blocking read on stdin (no SA_RESTART), so serve() closes the live run and returns
    struct sigaction stop{};
    stop.sa_handler = [](int) {};
    sigaction(SIGINT, &stop, nullptr);
    sigaction(SIGTERM, &stop, nullptr);
    return serve(std::cin, std::cout, options);
}
