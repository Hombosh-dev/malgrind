// Portable C++ fake for external tools (git, pio, python/python3/py, esptool, echo) used in host tests.
#include "openocd.hpp"
#include "os/paths.hpp"
#include "os/socket.hpp"
#include "os/tools.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;
namespace os = malgrind::host::os;

namespace {

std::vector<std::string> utf8_args(int argc, char** argv) {
    std::vector<std::string> out;
#ifdef _WIN32
    int wargc = 0;
    if (LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc)) {
        out.reserve(static_cast<std::size_t>(wargc));
        for (int i = 0; i < wargc; ++i) out.push_back(os::to_utf8(fs::path(wargv[i])));
        LocalFree(wargv);
        return out;
    }
#endif
    out.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) out.emplace_back(argv[i]);
    return out;
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string join_args(const std::vector<std::string>& args, std::size_t from) {
    std::string out;
    for (std::size_t i = from; i < args.size(); ++i) {
        if (i > from) out.push_back(' ');
        out.append(args[i]);
    }
    return out;
}

void append_log(std::string_view text) {
    if (const auto path = os::get_env("FAKE_LOG"); path && !path->empty()) {
        std::ofstream out(os::from_utf8(*path), std::ios::app | std::ios::binary);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
}

int run_git(const std::vector<std::string>& args) {
    append_log("git " + join_args(args, 1) + "\n" +
               "prompt=" + os::get_env("GIT_TERMINAL_PROMPT").value_or("") + "\n");
    if (os::get_env("FAKE_GIT") == "fail") {
        std::fprintf(stderr, "fatal: repository not found\n");
        return 128;
    }
    if (args.size() <= 1) return 1;
    const fs::path dest = os::from_utf8(args.back());
    std::error_code ec;
    fs::create_directories(dest, ec);
    std::printf("Cloning into '%s'...\n", args.back().c_str());
    if (os::get_env("FAKE_GIT") == "nopio") {
        std::ofstream(dest / "README.md", std::ios::binary) << "readme\n";
        return 0;
    }
    fs::create_directories(dest / "firmware/x", ec);
    std::ofstream(dest / "firmware/x/platformio.ini", std::ios::binary) << "[env:a]\n";
    return 0;
}

int run_pio(const std::vector<std::string>& args) {
    append_log("pio " + join_args(args, 1) +
               " build=" + os::get_env("PLATFORMIO_BUILD_DIR").value_or("") +
               " extra=" + os::get_env("PLATFORMIO_EXTRA_SCRIPTS").value_or("") +
               " agent=" + os::get_env("MALGRIND_AGENT_DIR").value_or("") +
               " sdk=" + os::get_env("MALGRIND_SDKCONFIG").value_or("") +
               " rate=" + os::get_env("MALGRIND_RATE_HZ").value_or("") +
               " flags=" + os::get_env("PLATFORMIO_BUILD_FLAGS").value_or("") + "\n");
    if (os::get_env("FAKE_FAIL") == "pio") {
        std::fprintf(stderr, "error: build failed\n");
        return 1;
    }
    const auto src = os::get_env("FAKE_ESPTOOL_SRC");
    const auto dir = os::get_env("FAKE_ESPTOOL_DIR");
    if (src && !src->empty() && dir && !dir->empty()) {
        std::error_code ec;
        const fs::path dst_dir = os::from_utf8(*dir);
        fs::create_directories(dst_dir, ec);
        fs::copy_file(os::from_utf8(*src), dst_dir / "esptool.py", fs::copy_options::overwrite_existing, ec);
    }
    return 0;
}

int run_esptool(const std::vector<std::string>& args, std::size_t start) {
    append_log("esptool " + join_args(args, start) + "\n");
    std::size_t i = start;
    while (i + 1 < args.size() && (args[i] == "--port" || args[i] == "--baud")) i += 2;
    std::printf("Chip is ESP32-D0WD-V3\n");
    std::printf("MAC: 24:0a:c4:AA:bb:0c\n");
    if (i < args.size()) {
        const std::string_view cmd = args[i];
        if (cmd == "flash_id") {
            std::printf("Detected flash size: 4MB\n");
        } else if (cmd == "read_flash" && i + 3 < args.size()) {
            if (os::get_env("FAKE_FAIL") == "read_flash") {
                std::fprintf(stderr, "A fatal error occurred: timeout\n");
                return 2;
            }
            const std::size_t bytes = std::stoull(args[i + 2]);
            std::ofstream out(os::from_utf8(args[i + 3]), std::ios::binary | std::ios::trunc);
            const std::string zeros(4096, '\0');
            for (std::size_t left = bytes; left > 0;) {
                const std::size_t chunk = std::min(left, zeros.size());
                out.write(zeros.data(), static_cast<std::streamsize>(chunk));
                left -= chunk;
            }
            if (const auto agent = os::get_env("FAKE_AGENT_IMAGE"); agent && !agent->empty()) {
                constexpr std::string_view kMarker = "malgrind-agent-v1";
                out.seekp(1000);
                out.write(kMarker.data(), static_cast<std::streamsize>(kMarker.size()));
            }
        }
    }
    return 0;
}

int run_python(const std::vector<std::string>& args) {
    append_log("python " + join_args(args, 1) + "\n");
    const std::size_t idx = (args.size() > 1 && args[1] == "-3") ? 2 : 1;
    if (args.size() > idx + 1 && args[idx] == "-m") {
        const std::string_view mod = args[idx + 1];
        if (mod == "venv" && args.size() > idx + 2) {
            const fs::path venv_bin = os::venv_bin(os::from_utf8(args[idx + 2]));
            std::error_code ec;
            fs::create_directories(venv_bin, ec);
            if (const fs::path self = os::executable_path(); !self.empty()) {
                fs::copy_file(self, venv_bin / os::exe_name("python"), fs::copy_options::overwrite_existing, ec);
            }
            if (os::get_env("FAKE_FAIL") == "venv") {
                std::fprintf(stderr, "The virtual environment was not created successfully because ensurepip is not available.\n");
                return 1;
            }
            std::ofstream(venv_bin / os::exe_name("pip"), std::ios::binary) << "";
            return 0;
        }
        if (mod == "pip") {
            if (os::get_env("FAKE_FAIL") == "pip") {
                std::fprintf(stderr, "ERROR: connection failed\n");
                return 1;
            }
            if (os::get_env("FAKE_FAIL") == "nopio") return 0;
            const fs::path self = os::executable_path();
            const fs::path arg0_dir = os::from_utf8(args[0]).parent_path();
            const fs::path bin_dir = !arg0_dir.empty() ? arg0_dir : self.parent_path();
            if (!self.empty() && !bin_dir.empty()) {
                std::error_code ec;
                fs::copy_file(self, bin_dir / os::exe_name("pio"), fs::copy_options::overwrite_existing, ec);
            }
            return 0;
        }
    }
    if (args.size() > idx && os::from_utf8(args[idx]).filename() == "esptool.py") {
        return run_esptool(args, idx + 1);
    }
    return 0;
}

int run_echo(const std::vector<std::string>& args) {
    if (const auto out = os::get_env("FAKE_OUT"); out && !out->empty()) {
        std::printf("%s\n", out->c_str());
    }
    for (std::size_t i = 1; i < args.size(); ++i) {
        std::printf("%s\n", args[i].c_str());
    }
    std::fflush(stdout);
    if (const auto err = os::get_env("FAKE_ERR"); err && !err->empty()) {
        std::fprintf(stderr, "%s\n", err->c_str());
    }
    std::fflush(stderr);
    if (const auto raw = os::get_env("FAKE_RAW"); raw && !raw->empty()) {
        std::fwrite(raw->data(), 1, raw->size(), stdout);
    }
    std::fflush(nullptr);
    if (const auto code = os::get_env("FAKE_EXIT"); code && !code->empty()) {
        return std::stoi(*code);
    }
    return 0;
}

std::optional<std::uint64_t> parse_hex_addr(std::string_view s) {
    if (s.starts_with("0x") || s.starts_with("0X")) s.remove_prefix(2);
    if (s.empty()) return std::nullopt;
    std::uint64_t val = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), val, 16);
    if (ec != std::errc{} || ptr != s.data() + s.size()) return std::nullopt;
    return val;
}

int run_addr2line(const std::vector<std::string>& args) {
    fs::path map_path;
    std::vector<std::string> addrs;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "-e" && i + 1 < args.size()) {
            map_path = os::from_utf8(args[++i]);
        } else if (a.starts_with("-e") && a.size() > 2) {
            map_path = os::from_utf8(a.substr(2));
        } else if (!a.empty() && a[0] == '-') {
            continue;
        } else {
            addrs.push_back(a);
        }
    }
    if (map_path.empty()) return 1;
    std::ifstream in(map_path, std::ios::binary);
    if (!in) return 1;

    std::unordered_map<std::uint64_t, std::vector<std::string>> table;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (const auto addr = parse_hex_addr(std::string_view(line).substr(0, eq))) {
            table[*addr].push_back(line.substr(eq + 1));
        }
    }

    for (const std::string& a : addrs) {
        if (const auto addr = parse_hex_addr(a)) {
            if (const auto it = table.find(*addr); it != table.end() && !it->second.empty()) {
                for (const std::string& fn : it->second) {
                    std::printf("%s\nsrc.c:1\n", fn.c_str());
                }
                continue;
            }
        }
        std::printf("??\n??:0\n");
    }
    return 0;
}

int run_openocd(const std::vector<std::string>& args) {
    using namespace std::chrono_literals;
    append_log("openocd " + join_args(args, 1) + "\n");

    if (os::get_env("FAKE_FAIL") == "openocd" || os::get_env("FAKE_OPENOCD_FAIL") == "startup") {
        std::fprintf(stderr, "Open On-Chip Debugger 0.12.0\n");
        std::fprintf(stderr, "Error: unable to open debug probe\n");
        std::fflush(stderr);
        return 1;
    }

    if (const auto delay = os::get_env("FAKE_OPENOCD_STARTUP_SLEEP_MS"); delay && !delay->empty()) {
        int ms = 0;
        const auto [ptr, ec] = std::from_chars(delay->data(), delay->data() + delay->size(), ms);
        if (ec == std::errc{} && ptr == delay->data() + delay->size() && ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        }
    }

    std::uint16_t tcl_port = 0;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if ((args[i] == "-s" || args[i] == "-f") && i + 1 < args.size()) {
            ++i;
            continue;
        }
        if (args[i] == "-c" && i + 1 < args.size()) {
            const std::string_view cmd = args[++i];
            constexpr std::string_view kPrefix = "tcl_port ";
            if (cmd.starts_with(kPrefix)) {
                const auto val = cmd.substr(kPrefix.size());
                unsigned port = 0;
                const auto [ptr, ec] = std::from_chars(val.data(), val.data() + val.size(), port);
                if (ec == std::errc{} && ptr == val.data() + val.size() && port <= 65535) {
                    tcl_port = static_cast<std::uint16_t>(port);
                }
            }
        }
    }

    if (tcl_port == 0) {
        std::fprintf(stderr, "Error: tcl_port not specified\n");
        std::fflush(stderr);
        return 1;
    }

    auto listener = os::TcpListener::bind_loopback(tcl_port);
    if (!listener) {
        std::fprintf(stderr, "Error: couldn't bind tcl to socket\n");
        std::fflush(stderr);
        return 1;
    }

    std::fprintf(stderr, "Open On-Chip Debugger 0.12.0\n");
    std::fprintf(stderr, "Info : Listening on port %u for tcl connections\n", static_cast<unsigned>(tcl_port));
    std::fflush(stderr);

    const auto deadline = std::chrono::steady_clock::now() + 60s;
    const bool ignore_shutdown = os::get_env("FAKE_OPENOCD_IGNORE_SHUTDOWN") == "1";

    while (std::chrono::steady_clock::now() < deadline) {
        auto client = listener->accept(200);
        if (!client) continue;

        malgrind::host::TclRpcCodec codec;
        std::array<char, 4096> buf{};
        while (std::chrono::steady_clock::now() < deadline) {
            const auto n = client->read(buf, 200);
            if (!n) break;
            if (*n == 0) continue;
            for (auto cmd = codec.feed(std::string_view(buf.data(), *n)); cmd; cmd = codec.feed({})) {
                if (*cmd == "shutdown") {
                    if (ignore_shutdown) {
                        while (std::chrono::steady_clock::now() < deadline) {
                            std::this_thread::sleep_for(50ms);
                        }
                        return 0;
                    }
                    (void)client->write_all(malgrind::host::TclRpcCodec::request(""));
                    return 0;
                }
                if (*cmd == "version") {
                    (void)client->write_all(malgrind::host::TclRpcCodec::request("Open On-Chip Debugger 0.12.0"));
                } else if (cmd->starts_with("echo ")) {
                    (void)client->write_all(malgrind::host::TclRpcCodec::request(cmd->substr(5)));
                } else if (*cmd == "mrw" || cmd->starts_with("mrw ")) {
                    (void)client->write_all(malgrind::host::TclRpcCodec::request("0x08001234"));
                } else if (cmd->starts_with("sleep_ms ")) {
                    const std::string_view arg = std::string_view(*cmd).substr(9);
                    int ms = 0;
                    const auto [ptr, ec] = std::from_chars(arg.data(), arg.data() + arg.size(), ms);
                    if (ec == std::errc{} && ptr == arg.data() + arg.size() && ms > 0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                    }
                    (void)client->write_all(malgrind::host::TclRpcCodec::request("slept"));
                } else if (*cmd == "drop") {
                    return 2;
                } else {
                    (void)client->write_all(malgrind::host::TclRpcCodec::request("ok:" + *cmd));
                }
            }
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    const auto args = utf8_args(argc, argv);
    if (args.empty()) return 1;

    std::string role;
    if (const auto env_role = os::get_env("FAKE_TOOL_ROLE"); env_role && !env_role->empty()) {
        role = to_lower(*env_role);
    } else {
        role = to_lower(os::to_utf8(os::from_utf8(args[0]).stem()));
    }

    if (role == "git") return run_git(args);
    if (role == "pio") return run_pio(args);
    if (role == "python" || role == "python3" || role == "py") return run_python(args);
    if (role == "esptool") return run_esptool(args, 1);
    if (role == "echo") return run_echo(args);
    if (role == "addr2line" || role.ends_with("-addr2line")) return run_addr2line(args);
    if (role == "openocd") return run_openocd(args);

    std::fprintf(stderr, "fake_tool: unknown role '%s'\n", role.c_str());
    return 1;
}
