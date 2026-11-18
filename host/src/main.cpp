// malgrind-cli: the engine of the malgrind app. `serve` speaks JSON lines to the app (serve.hpp); `run`, `attach` and
// `detect` are the terminal mode with a live page in the browser (cli.hpp).
#include "cli.hpp"
#include "os/paths.hpp"
#include "serve.hpp"
#include "projects.hpp"
#include "store.hpp"

#include <csignal>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

#ifndef MALGRIND_DEFAULT_AGENT_DIR
#define MALGRIND_DEFAULT_AGENT_DIR ""
#endif

// Where agent/esp32/ is: $MALGRIND_AGENT_DIR, else the app's resources next to this executable (.deb: /usr/lib/malgrind,
// .app: Contents/Resources), else the repository this binary was built from.
std::filesystem::path default_agent_dir() {
    namespace fs = std::filesystem;
    namespace os = malgrind::host::os;
    if (const auto env = os::get_env("MALGRIND_AGENT_DIR"); env && !env->empty()) return os::from_utf8(*env);
    if (const auto exe = os::executable_path().parent_path(); !exe.empty()) {
        for (const char* rel : {"agent-src", "../lib/malgrind/agent-src", "../Resources/agent-src"}) {
            const auto candidate = (exe / rel).lexically_normal();
            if (std::error_code ec; fs::exists(candidate / "agent/esp32/inject.py", ec)) return candidate;
        }
    }
    return os::from_utf8(MALGRIND_DEFAULT_AGENT_DIR);
}

constexpr std::string_view kServeUsage = "malgrind-cli serve [--data-dir DIR] [--agent-dir DIR]\n";

} // namespace

int main(int argc, char** argv) {
    using namespace malgrind::host;
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.size() == 1 && (args[0] == "--version" || args[0] == "version")) {
        std::cout << "malgrind-cli " << MALGRIND_VERSION << "\n";
        return 0;
    }
    if (args.empty() || (args[0] != "serve" && args[0] != "run" && args[0] != "attach" && args[0] != "detect" && args[0] != "restore")) {
        std::cerr << kCliUsage;
        return 2;
    }

    // backups belong to boards, not to a runs directory: --data-dir does not move them
    ServeOptions options{RunStore::default_root(), default_agent_dir(), os::home_dir(),
                         RunStore::default_root().parent_path() / "backups", ProjectStore::default_root()};

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
        (args[i] == "--data-dir" ? options.data_dir : options.agent_dir) = os::from_utf8(args[i + 1]);
    }
    return serve(std::cin, std::cout, options);
}
