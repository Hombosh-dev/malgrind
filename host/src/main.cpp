// mulcan: detect | run | attach
#include "aggregate.hpp"
#include "discover.hpp"
#include "http.hpp"
#include "process.hpp"
#include "source.hpp"
#include "symbols.hpp"
#include "wire.hpp"

#include <array>
#include <charconv>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace mulcan::host;

namespace {

constexpr std::string_view kPage =
#include "flame_html.inc"
    ;

volatile std::sig_atomic_t g_stop = 0;

constexpr std::string_view kUsage = R"(mulcan: live flamegraph profiler for microcontrollers

  mulcan run      build + flash the firmware, then show the live flamegraph
  mulcan attach   the firmware already runs libmulcan: just show the live flamegraph
  mulcan detect   identify the connected chip (this resets the board)

Everything is detected automatically. Override only when needed:
  --port P        serial port (default: the only /dev/ttyUSB* or /dev/ttyACM*; "-" reads stdin)
  --elf FILE      firmware ELF for function names (default: newest matching .pio/build/*/firmware.elf or build/*.elf)
  --project DIR   PlatformIO project for `run` (default: . or the only firmware/*/platformio.ini)
  --env NAME      PlatformIO environment (default: the project's default_envs)
  --flash-cmd C   your own build+flash command instead of PlatformIO
  --http-port N   viewer port (default: 8000, or the next free one)
  --no-open       do not open the browser
  --baud N  --addr2line TOOL  --folded FILE
)";

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

std::optional<Options> parse(int argc, char** argv) {
    if (argc < 2) return std::nullopt;
    Options o{argv[1], {}};
    for (int i = 2; i < argc; ++i) {
        const std::string_view key = argv[i];
        if (key == "--no-open") { o.values["no-open"] = "1"; continue; }
        if (!key.starts_with("--") || i + 1 >= argc) return std::nullopt;
        o.values[std::string(key.substr(2))] = argv[++i];
    }
    return o;
}

// --port if given, otherwise the only serial port found. Prints why when it cannot decide.
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

// Serves the viewer on the first free port from `first` (only `first` if the user chose it).
void start_server(std::optional<HttpServer>& http, Aggregator& agg, unsigned first, bool fixed, unsigned& used) {
    for (unsigned port = first; port < first + (fixed ? 1 : 10); ++port) {
        try {
            http.emplace(port, [&agg](std::string_view path, std::string_view query) -> std::optional<HttpServer::Response> {
                if (path == "/") return HttpServer::Response{"text/html; charset=utf-8", std::string(kPage)};
                if (path != "/data.json") return std::nullopt;
                const auto core = query.find("core=all") != std::string_view::npos ? std::nullopt : query_uint(query, "core");
                const auto window = std::chrono::seconds(query_uint(query, "window").value_or(10));
                return HttpServer::Response{"application/json", agg.snapshot_json(core, window)};
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

int attach(const Options& o) {
    const std::string port = o.get("port");
    auto source = Source::open(port, o.get_uint("baud", 921600));
    if (!source) {
        std::cerr << "cannot open " << port << ": " << std::strerror(errno) << "\n";
        if (errno == EBUSY) std::cerr << "another program (maybe another mulcan, or a serial monitor) is using it\n";
        return 1;
    }

    // destroyed in reverse order: the server stops before the data it reads goes away
    std::optional<Symbolizer> symbols;
    std::optional<Aggregator> aggregator;
    std::optional<HttpServer> http;

    FrameReader reader;
    std::array<std::uint8_t, 4096> buf;
    const auto started = std::chrono::steady_clock::now();
    bool hinted = false;
    while (!g_stop) {
        const auto n = source->read(buf);
        if (!n) break;
        auto [frames, text] = reader.feed(std::span(buf).first(*n));
        if (!text.empty()) std::cout << text << std::flush;
        if (!aggregator && !hinted && std::chrono::steady_clock::now() - started > std::chrono::seconds(5)) {
            hinted = true;
            std::cerr << "no mulcan frames yet: is libmulcan running on the board, and is the baud rate right (--baud)?\n";
        }
        for (auto& frame : frames) {
            if (!aggregator) {
                fs::path elf = o.get("elf");
                if (elf.empty()) {
                    if (auto found = find_elf(frame.arch, ".", o.get("project"))) {
                        elf = *found;
                        std::cerr << "elf: " << elf.string() << " (auto-detected, --elf chooses another)\n";
                    } else {
                        std::cerr << "note: no " << arch_name(frame.arch) << " firmware ELF found (looked in .pio/build and build/ here and in "
                                     "firmware/*): functions show as raw addresses, pass --elf\n";
                    }
                }
                fs::path tool = o.get("addr2line");
                if (tool.empty()) tool = find_addr2line(frame.arch);
                if (!elf.empty() && tool.empty())
                    std::cerr << "warning: no addr2line found for " << arch_name(frame.arch) << ", use --addr2line\n";
                symbols.emplace(elf, tool);
                aggregator.emplace(*symbols);
                unsigned used = 0;
                start_server(http, *aggregator, o.get_uint("http-port", 8000), !o.get("http-port").empty(), used);
                const auto url = std::format("http://127.0.0.1:{}", used);
                std::cerr << std::format("\nmulcan: {}, {} core(s) -> {}\n", arch_name(frame.arch), frame.ncores, url);
                if (o.get("no-open").empty()) open_browser(url);
            }
            aggregator->add(std::move(frame));
        }
    }
    if (aggregator && !o.get("folded").empty()) std::ofstream(o.get("folded")) << aggregator->folded();
    return 0;
}

std::optional<fs::path> find_in_path(const std::string& name) {
    std::istringstream dirs(std::getenv("PATH") ? std::getenv("PATH") : "");
    for (std::string dir; std::getline(dirs, dir, ':');)
        if (std::error_code ec; fs::exists(fs::path(dir) / name, ec)) return fs::path(dir) / name;
    return std::nullopt;
}

int detect(const Options& o) {
    const std::string port = o.get("port");
    const fs::path home = std::getenv("HOME") ? std::getenv("HOME") : "";
    const fs::path esptool = home / ".platformio/packages/tool-esptoolpy/esptool.py";
    // PlatformIO's own Python has pyserial; its launcher script names it in the shebang line
    std::string shebang;
    if (const auto pio = find_in_path("pio")) std::getline(std::ifstream(*pio) >> std::ws, shebang);
    if (shebang.starts_with("#!") && fs::exists(esptool)) {
        std::vector<std::string> cmd; // "#!/usr/bin/env python3" is two words
        std::istringstream words(shebang.substr(2));
        for (std::string w; words >> w;) cmd.push_back(w);
        for (auto& a : std::vector<std::string>{esptool.string(), "--port", port, "chip_id"}) cmd.push_back(a);
        const auto out = capture(cmd);
        if (const auto at = out.find("Chip is "); at != std::string::npos) {
            std::cout << "esptool: " << out.substr(at + 8, out.find('\n', at) - at - 8) << "\n";
            return 0;
        }
    }
    std::cout << "esptool could not identify the chip; listening for a mulcan frame...\n";
    auto source = Source::open(port, o.get_uint("baud", 921600));
    if (!source) return 1;
    FrameReader reader;
    std::array<std::uint8_t, 4096> buf;
    for (const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(4); std::chrono::steady_clock::now() < end;) {
        const auto n = source->read(buf);
        if (!n) break;
        if (auto [frames, text] = reader.feed(std::span(buf).first(*n)); !frames.empty()) {
            std::cout << std::format("mulcan frame: arch={} cores={} cpu={:g} MHz\n", arch_name(frames[0].arch),
                                     frames[0].ncores, frames[0].cpu_hz / 1e6);
            return 0;
        }
    }
    std::cerr << "no chip found\n";
    return 1;
}

int run_command(Options o) {
    std::vector<std::string> cmd;
    if (!o.get("flash-cmd").empty()) {
        cmd = {"/bin/sh", "-c", o.get("flash-cmd")};
    } else {
        fs::path project = o.get("project");
        if (project.empty()) {
            std::string why;
            const auto found = find_project(".", why);
            if (!found) {
                std::cerr << why << "\n";
                return 2;
            }
            project = *found;
            std::cerr << "project: " << project.string() << " (auto-detected, --project chooses another)\n";
        }
        o.values["project"] = project.string();
        cmd = {"pio", "run", "-t", "upload", "-d", project.string(), "--upload-port", o.get("port")};
        if (!o.get("env").empty()) cmd.insert(cmd.begin() + 2, {"-e", o.get("env")});
    }
    std::cerr << "+ ";
    for (const auto& a : cmd) std::cerr << a << ' ';
    std::cerr << "\n";
    if (run(cmd) != 0) return 1;
    return attach(o);
}

} // namespace

int main(int argc, char** argv) {
    auto options = parse(argc, argv);
    if (!options) {
        std::cerr << kUsage;
        return 2;
    }
    if (options->command != "detect" && options->command != "attach" && options->command != "run") {
        std::cerr << kUsage;
        return 2;
    }
    const auto port = resolve_port(*options);
    if (!port) return 2;
    options->values["port"] = *port;
    std::signal(SIGINT, [](int) { g_stop = 1; });
    std::signal(SIGPIPE, SIG_IGN); // a closed browser connection must not kill the tool (macOS has no MSG_NOSIGNAL)
    try {
        if (options->command == "detect") return detect(*options);
        if (options->command == "attach") return attach(*options);
        if (options->command == "run") return run_command(*options);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    std::cerr << kUsage;
    return 2;
}
