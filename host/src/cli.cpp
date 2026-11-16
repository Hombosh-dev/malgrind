#include "cli.hpp"

#include "discover.hpp"
#include "http.hpp"
#include "ops.hpp"
#include "process.hpp"
#include "session.hpp"
#include "source.hpp"
#include "store.hpp"
#include "wire.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace std::chrono_literals;

namespace malgrind::host {

namespace {

constexpr std::string_view kPage =
#include "flame_html.inc"
    ;

std::atomic<bool> g_stop{false};

struct Options {
    std::string command;
    std::map<std::string, std::string> values;

    [[nodiscard]] std::string get(const std::string& key, std::string fallback = {}) const {
        const auto it = values.find(key);
        return it == values.end() ? fallback : it->second;
    }
    [[nodiscard]] unsigned get_uint(const std::string& key, unsigned fallback) const {
        unsigned v = fallback;
        const auto s = get(key);
        if (!s.empty()) std::from_chars(s.data(), s.data() + s.size(), v);
        return v;
    }
};

std::optional<Options> parse(const std::vector<std::string>& args) {
    if (args.empty()) return std::nullopt;
    Options o{args[0], {}};
    const bool known_command = o.command == "run" || o.command == "attach" || o.command == "detect";
    if (!known_command) return std::nullopt;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view key = args[i];
        if (key == "--no-open") {
            o.values["no-open"] = "1";
            continue;
        }
        static constexpr std::string_view kValued[] = {"--port", "--elf", "--project", "--env", "--name", "--http-port", "--folded",
                                                       "--data-dir", "--agent-dir"};
        if (std::ranges::find(kValued, key) == std::end(kValued) || i + 1 >= args.size()) return std::nullopt;
        o.values[std::string(key.substr(2))] = args[++i];
    }
    return o;
}

// --port if given, otherwise the only serial port found. Says why when it cannot decide.
std::optional<std::string> resolve_port(const Options& o) {
    if (auto port = o.get("port"); !port.empty()) return port;
    const auto ports = find_serial_ports();
    if (ports.size() == 1) {
        std::cerr << "port: " << ports[0] << " (auto-detected, --port chooses another)\n";
        return ports[0];
    }
    if (ports.empty()) {
        std::cerr << "no serial port found (looked for /dev/ttyUSB* and /dev/ttyACM*): connect the board or pass --port\n";
    } else {
        std::cerr << "several serial ports found, pass --port with one of:\n";
        for (const auto& p : ports) std::cerr << "  " << p << "\n";
    }
    return std::nullopt;
}

std::optional<unsigned> query_uint(std::string_view query, std::string_view key) {
    const auto at = query.find(std::string(key) + "=");
    if (at == std::string_view::npos) return std::nullopt;
    const auto value = query.substr(at + key.size() + 1, query.find('&', at) - at - key.size() - 1);
    unsigned v = 0;
    if (std::from_chars(value.data(), value.data() + value.size(), v).ec != std::errc{}) return std::nullopt;
    return v;
}

// The viewer on the first free port from `first` (only `first` if the user chose it).
void start_server(std::optional<HttpServer>& http, const std::shared_ptr<Session>& session, unsigned first, bool fixed,
                  unsigned& used) {
    for (unsigned port = first; port < first + (fixed ? 1 : 10); ++port) {
        try {
            http.emplace(port, [session](std::string_view path, std::string_view query) -> std::optional<HttpServer::Response> {
                if (path == "/") return HttpServer::Response{"text/html; charset=utf-8", std::string(kPage)};
                if (path != "/data.json") return std::nullopt;
                const auto core = query.find("core=all") != std::string_view::npos ? std::nullopt : query_uint(query, "core");
                const auto window = std::chrono::seconds(query_uint(query, "window").value_or(10));
                auto snapshot = session->snapshot(core, window, std::nullopt);
                return HttpServer::Response{"application/json",
                                            snapshot ? *snapshot : R"({"tree":{"name":"all","value":0,"children":[]},"stats":{}})"};
            });
            used = port;
            return;
        } catch (const std::runtime_error&) {} // port busy: try the next one
    }
    throw std::runtime_error("no free port for the viewer, pass --http-port");
}

void open_browser(const std::string& url) {
#ifdef __APPLE__
    const bool has_display = std::getenv("SSH_CONNECTION") == nullptr;
    const char* opener = "open";
#else
    const bool has_display = std::getenv("DISPLAY") || std::getenv("WAYLAND_DISPLAY");
    const char* opener = "xdg-open";
#endif
    if (has_display) {
        [[maybe_unused]] const int started = std::system((std::string(opener) + " " + url + " >/dev/null 2>&1 &").c_str());
    }
}

std::string today() {
    const std::time_t t = std::time(nullptr);
    std::array<char, 32> buf{};
    std::strftime(buf.data(), buf.size(), "%Y-%m-%d %H:%M", std::localtime(&t));
    return buf.data();
}

// Records `source` as a new run and shows it until Ctrl-C or the board stops sending.
int watch(Source source, const Options& o, const ServeOptions& defaults, const fs::path& project, fs::path elf) {
    RunStore store(o.get("data-dir").empty() ? defaults.data_dir : fs::path(o.get("data-dir")));
    const auto name = o.get("name", (project.empty() ? o.get("port") : project.filename().string()) + " " + today());

    std::shared_ptr<Session> session = Session::live(
        std::move(source), store.create({.name = name, .project = project.string(), .env = o.get("env"), .port = o.get("port")}),
        std::move(elf), project, [](const json& e) {
            const auto kind = e["event"].get<std::string>();
            if (kind == "log") std::cout << e["text"].get<std::string>() << std::flush;
            else if (kind == "notice") std::cerr << "note: " << e["text"].get<std::string>() << "\n";
            else if (kind == "target")
                std::cerr << "malgrind: " << e["arch"].get<std::string>() << ", " << e["ncores"] << " core(s), elf: "
                          << (e["elf"].is_null() ? "none (raw addresses)" : e["elf"].get<std::string>()) << "\n";
            else if (kind == "ended") std::cerr << "the board stopped sending (unplugged?)\n";
        });

    std::optional<HttpServer> http; // destroyed first: the server stops before the session it reads goes away
    unsigned used = 0;
    start_server(http, session, o.get_uint("http-port", 8000), !o.get("http-port").empty(), used);
    const auto url = "http://127.0.0.1:" + std::to_string(used);
    std::cerr << "malgrind: live flamegraph at " << url << "  (Ctrl-C to stop)\n";
    if (o.get("no-open").empty()) open_browser(url);

    while (!g_stop && session->live()) std::this_thread::sleep_for(100ms);
    if (const auto folded = session->folded(); folded && !o.get("folded").empty()) std::ofstream(o.get("folded")) << *folded;
    return 0;
}

int attach(const Options& o, const ServeOptions& defaults) {
    const auto port = resolve_port(o);
    if (!port) return 2;
    Options with_port = o;
    with_port.values["port"] = *port;
    // the old rule: look for the firmware in the current directory and firmware/*
    const fs::path project = o.get("project").empty() ? fs::path(".") : fs::path(o.get("project"));
    return watch(open_source(*port), with_port, defaults, project, o.get("elf"));
}

int run_command(const Options& o, const ServeOptions& defaults) {
    const auto port = resolve_port(o);
    if (!port) return 2;
    fs::path dir = o.get("project");
    if (dir.empty()) {
        std::string why;
        const auto found = find_project(".", why);
        if (!found) {
            std::cerr << why << "\n";
            return 2;
        }
        dir = *found;
        std::cerr << "project: " << dir.string() << " (auto-detected, --project chooses another)\n";
    }
    const fs::path agent_dir = o.get("agent-dir").empty() ? defaults.agent_dir : fs::path(o.get("agent-dir"));
    const auto p = prepare_project(defaults.home, dir, o.get("env"), *port);
    flash_agent(p, defaults.home, agent_dir, defaults.backup_dir, [](std::string_view line) { std::cerr << line << "\n"; });
    Options with = o;
    with.values["port"] = *port;
    with.values["env"] = p.env;
    return watch(open_after_upload(*port), with, defaults, p.project.dir, agent_elf(p.project, p.env));
}

int detect(const Options& o, const ServeOptions& defaults) {
    const auto port = resolve_port(o);
    if (!port) return 2;
    if (const auto pio = find_pio(defaults.home)) {
        if (const auto chip = chip_id(*pio, *port, defaults.home)) { // resets the board
            std::cout << "esptool: " << *chip << "\n";
            return 0;
        }
    }
    std::cout << "esptool could not identify the chip; listening for a malgrind frame...\n";
    const auto probe = probe_port(*port);
    if (!probe.agent) {
        std::cerr << (probe.busy ? "the port is busy\n" : "no chip found\n");
        return 1;
    }
    std::cout << "malgrind frame: arch=" << arch_name(probe.arch) << " cores=" << probe.ncores << " cpu=" << probe.cpu_mhz << " MHz\n";
    return 0;
}

} // namespace

const char* const kCliUsage = R"(malgrind-cli: live flamegraph profiler for microcontrollers (terminal mode; the app uses `serve`)

  malgrind-cli run      build + flash the firmware with the agent, then show the live flamegraph in the browser
  malgrind-cli attach   the board already runs the agent: just show the live flamegraph
  malgrind-cli detect   identify the connected chip (this resets the board)
  malgrind-cli serve    the protocol of the malgrind app, on stdin/stdout

Everything is detected automatically. Override only when needed:
  --port P        serial port (default: the only /dev/ttyUSB* or /dev/ttyACM*)
  --elf FILE      firmware ELF for function names (default: newest matching one under --project, `.` or firmware/*)
  --project DIR   PlatformIO project for `run` (default: . or the only firmware/*/platformio.ini)
  --env NAME      PlatformIO environment (default: the project's default_envs)
  --name NAME     name of the run in the history (default: project folder and date)
  --http-port N   viewer port (default: 8000, or the next free one)
  --no-open       do not open the browser
  --folded FILE   write folded stacks (flamegraph.pl, speedscope) when stopped
  --data-dir DIR  where runs are stored        --agent-dir DIR  where agent/esp32/ is
)";

void cli_request_stop() noexcept { g_stop = true; }

int run_cli(const std::vector<std::string>& args, const ServeOptions& defaults) {
    const auto options = parse(args);
    if (!options) {
        std::cerr << kCliUsage;
        return 2;
    }
    g_stop = false;
    try {
        if (options->command == "detect") return detect(*options, defaults);
        if (options->command == "attach") return attach(*options, defaults);
        return run_command(*options, defaults);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}

} // namespace malgrind::host
