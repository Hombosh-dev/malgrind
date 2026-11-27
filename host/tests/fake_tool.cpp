// Portable C++ fake for external tools (git, pio, python/python3/py, esptool, picotool, openocd, echo) used in host tests.
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
#include <format>
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

void append_env_log(const char* env_name, std::string_view text) {
    if (const auto path = os::get_env(env_name); path && !path->empty()) {
        std::ofstream out(os::from_utf8(*path), std::ios::app | std::ios::binary);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
}

std::string_view trim_ws(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

std::vector<std::string> split_ws(std::string_view s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        const std::size_t start = i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i > start) out.emplace_back(s.substr(start, i - start));
    }
    return out;
}

std::vector<std::string> split_sep(std::string_view s, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        const auto pos = s.find(sep, start);
        const std::string_view part =
            trim_ws(pos == std::string_view::npos ? s.substr(start) : s.substr(start, pos - start));
        if (!part.empty()) out.emplace_back(part);
        if (pos == std::string_view::npos) break;
        start = pos + 1;
    }
    return out;
}

std::vector<std::string> split_tcl_words(std::string_view s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i >= s.size()) break;
        std::string word;
        if (s[i] == '"') {
            ++i;
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < s.size()) ++i;
                word.push_back(s[i++]);
            }
            if (i < s.size() && s[i] == '"') ++i;
        } else if (s[i] == '{') {
            ++i;
            int depth = 1;
            while (i < s.size() && depth > 0) {
                if (s[i] == '{') ++depth;
                else if (s[i] == '}') {
                    --depth;
                    if (depth == 0) {
                        ++i;
                        break;
                    }
                }
                word.push_back(s[i++]);
            }
        } else {
            while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) word.push_back(s[i++]);
        }
        out.push_back(std::move(word));
    }
    return out;
}

std::optional<std::uint32_t> parse_u32(std::string_view s) {
    s = trim_ws(s);
    if (s.empty()) return std::nullopt;
    int base = 10;
    if (s.starts_with("0x") || s.starts_with("0X")) {
        s.remove_prefix(2);
        base = 16;
    }
    if (s.empty()) return std::nullopt;
    std::uint32_t val = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), val, base);
    if (ec != std::errc{} || ptr != s.data() + s.size()) return std::nullopt;
    return val;
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
    append_env_log("FAKE_ADDR2LINE_LOG", "addr2line " + join_args(args, 1) + "\n");
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

bool write_fake_flash_image(const fs::path& out_path, std::size_t len) {
    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    std::array<char, 4096> chunk{};
    std::size_t written = 0;
    while (written < len) {
        const std::size_t n = std::min<std::size_t>(chunk.size(), len - written);
        for (std::size_t k = 0; k < n; ++k) {
            chunk[k] = static_cast<char>((written + k) & 0xFFu);
        }
        out.write(chunk.data(), static_cast<std::streamsize>(n));
        written += n;
    }
    if (const auto agent = os::get_env("FAKE_AGENT_IMAGE"); agent && !agent->empty()) {
        const std::string_view marker = (*agent == "v1") ? "malgrind-agent-v1" : "malgrind-agent-v2";
        if (len >= marker.size()) {
            const std::size_t off = (len >= 1000u + marker.size()) ? 1000u : 0u;
            out.seekp(static_cast<std::streamoff>(off));
            out.write(marker.data(), static_cast<std::streamsize>(marker.size()));
        }
    }
    out.flush();
    return static_cast<bool>(out);
}

int run_picotool(const std::vector<std::string>& args) {
    append_log("picotool " + join_args(args, 1) + "\n");
    if (os::get_env("FAKE_PICOTOOL_NO_BOOTSEL") == "1" || os::get_env("FAKE_FAIL") == "picotool_no_bootsel") {
        std::fprintf(stderr, "No accessible RP2040 devices in BOOTSEL mode were found.\n");
        std::fflush(stderr);
        return 1;
    }
    if (os::get_env("FAKE_FAIL") == "picotool") {
        std::fprintf(stderr, "Error: picotool failed\n");
        std::fflush(stderr);
        return 1;
    }
    if (args.size() < 2) {
        std::fprintf(stderr, "picotool: missing command\n");
        return 1;
    }

    const std::string_view sub = args[1];
    if (sub == "info") {
        if (const auto custom = os::get_env("FAKE_PICOTOOL_INFO"); custom && !custom->empty()) {
            std::printf("%s\n", custom->c_str());
            return 0;
        }
        const std::string chip = os::get_env("FAKE_PICOTOOL_CHIP").value_or("RP2040");
        const std::string flash_size = os::get_env("FAKE_PICOTOOL_FLASH_SIZE").value_or("2048K");
        const std::string flash_id = os::get_env("FAKE_PICOTOOL_FLASH_ID").value_or("0xE660583883276128");
        std::printf("Program Information\n");
        std::printf(" name:          firmware\n");
        std::printf("Device Information\n");
        std::printf(" type:               %s\n", chip.c_str());
        std::printf(" package:            QFN-56\n");
        std::printf(" chip revision:      B2\n");
        std::printf(" flash size:         %s\n", flash_size.c_str());
        std::printf(" flash id:           %s\n", flash_id.c_str());
        return 0;
    }

    if (sub == "save") {
        if (os::get_env("FAKE_FAIL") == "picotool_save") {
            std::fprintf(stderr, "Error: picotool save failed\n");
            return 1;
        }
        std::optional<std::uint32_t> start_addr, end_addr;
        std::string out_path;
        for (std::size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "-r" && i + 2 < args.size()) {
                start_addr = parse_u32(args[i + 1]);
                end_addr = parse_u32(args[i + 2]);
                i += 2;
            } else if (!args[i].empty() && args[i][0] == '-') {
                continue;
            } else {
                out_path = args[i];
            }
        }
        if (!start_addr || !end_addr || *end_addr <= *start_addr || out_path.empty()) {
            std::fprintf(stderr, "Error: invalid picotool save arguments\n");
            return 1;
        }
        const std::size_t len = static_cast<std::size_t>(*end_addr - *start_addr);
        if (!write_fake_flash_image(os::from_utf8(out_path), len)) {
            std::fprintf(stderr, "Error: couldn't open %s\n", out_path.c_str());
            return 1;
        }
        std::printf("Saving file: %s (%zu bytes)\n", out_path.c_str(), len);
        return 0;
    }

    if (sub == "load") {
        if (os::get_env("FAKE_FAIL") == "picotool_load") {
            std::fprintf(stderr, "Error: picotool load failed\n");
            return 1;
        }
        std::string image_path;
        bool verify = false;
        bool reboot = false;
        for (std::size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "-v") {
                verify = true;
            } else if (args[i] == "-x") {
                reboot = true;
            } else if (args[i] == "-o" && i + 1 < args.size()) {
                ++i;
            } else if (!args[i].empty() && args[i][0] == '-') {
                continue;
            } else {
                image_path = args[i];
            }
        }
        std::error_code ec;
        if (image_path.empty() || !fs::is_regular_file(os::from_utf8(image_path), ec)) {
            std::fprintf(stderr, "Error: image does not exist: %s\n", image_path.c_str());
            return 1;
        }
        std::printf("Loading into Flash: [==============================]  100%%\n");
        if (verify) std::printf("Verifying Flash: OK\n");
        if (reboot) std::printf("The device was rebooted to start the application.\n");
        return 0;
    }

    std::fprintf(stderr, "Error: unknown picotool subcommand\n");
    return 1;
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

    const bool is_rp2040 = std::ranges::any_of(args, [](const std::string& a) {
        return to_lower(a).find("rp2040") != std::string::npos;
    });
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    const bool ignore_shutdown = os::get_env("FAKE_OPENOCD_IGNORE_SHUTDOWN") == "1";
    const bool demcr_ro = os::get_env("FAKE_OPENOCD_DEMCR_RO") == "1";
    const bool require_halt = os::get_env("FAKE_OPENOCD_REQUIRE_HALT") == "1";
    const bool mrw_fail = os::get_env("FAKE_OPENOCD_MRW_FAIL") == "1";
    const auto batch_override = os::get_env("FAKE_OPENOCD_BATCH_REPLY");
    const std::string target_type = os::get_env("FAKE_OPENOCD_TARGET_TYPE").value_or("cortex_m");

    std::vector<std::string> targets;
    if (const auto env_targets = os::get_env("FAKE_OPENOCD_TARGETS")) {
        targets = split_ws(*env_targets);
    } else if (is_rp2040) {
        targets = {"rp2040.core0", "rp2040.core1"};
    } else {
        targets = {"stm32f4x.cpu"};
    }
    std::size_t current_target = 0;
    bool halted = false;

    std::unordered_map<std::uint32_t, std::uint32_t> mem;
    mem[0xE000ED00u] = is_rp2040 ? 0x410CC601u : 0x410FC241u; // ARM Cortex-M0+ or Cortex-M4 CPUID
    if (const auto cpuid_env = os::get_env("FAKE_OPENOCD_CPUID"); cpuid_env && !cpuid_env->empty()) {
        if (const auto v = parse_u32(*cpuid_env)) mem[0xE000ED00u] = *v;
    }
    mem[0xE000EDFCu] = 0u; // CoreDebug DEMCR
    if (const auto demcr_env = os::get_env("FAKE_OPENOCD_DEMCR"); demcr_env && !demcr_env->empty()) {
        if (const auto v = parse_u32(*demcr_env)) mem[0xE000EDFCu] = *v;
    }

    std::vector<std::string> pcsr_seq = {"0x08001234", "0x08001240", "0xffffffff", "0x08001250", "0x00000000"};
    if (const auto pcsr_env = os::get_env("FAKE_OPENOCD_PCSR"); pcsr_env && !pcsr_env->empty()) {
        auto parsed = pcsr_env->find(',') != std::string::npos ? split_sep(*pcsr_env, ',') : split_ws(*pcsr_env);
        if (!parsed.empty()) pcsr_seq = std::move(parsed);
    }
    std::size_t pcsr_idx = 0;

    std::vector<std::vector<std::string>> pc_seqs(std::max<std::size_t>(1, targets.size()));
    for (std::size_t i = 0; i < pc_seqs.size(); ++i) {
        const std::string key = "FAKE_OPENOCD_REG_PC_" + std::to_string(i);
        if (const auto v = os::get_env(key.c_str()); v && !v->empty()) {
            pc_seqs[i] = split_sep(*v, ',');
        } else if (i == 0) {
            if (const auto v0 = os::get_env("FAKE_OPENOCD_REG_PC"); v0 && !v0->empty()) {
                pc_seqs[i] = split_sep(*v0, ',');
            }
        }
        if (pc_seqs[i].empty()) {
            pc_seqs[i] = (i % 2 == 0)
                             ? std::vector<std::string>{"pc (/32): 0x400d1000", "pc: 0x400d1010", "0xffffffff"}
                             : std::vector<std::string>{"pc: 0x400d2000", "pc (/32): 0x400d2010", "0x400d2020"};
        }
    }
    std::vector<std::size_t> pc_idx(pc_seqs.size(), 0);

    std::optional<std::size_t> drop_after;
    if (const auto drop_env = os::get_env("FAKE_OPENOCD_DROP_AFTER"); drop_env && !drop_env->empty()) {
        std::size_t n = 0;
        const auto [ptr, ec] = std::from_chars(drop_env->data(), drop_env->data() + drop_env->size(), n);
        if (ec == std::errc{} && ptr == drop_env->data() + drop_env->size()) drop_after = n;
    }
    std::size_t rpc_count = 0;

    auto read_mem = [&](std::uint32_t addr) -> std::string {
        if (mrw_fail) return "Error: memory read failed";
        if (addr == 0xE000101Cu) {
            const std::string& val = pcsr_seq[pcsr_idx % pcsr_seq.size()];
            ++pcsr_idx;
            return val;
        }
        if (const auto it = mem.find(addr); it != mem.end()) {
            return std::format("0x{:08x}", it->second);
        }
        return "0x08001234";
    };

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
                append_env_log("FAKE_OPENOCD_RPC_LOG", *cmd + "\n");
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
                if (drop_after && rpc_count >= *drop_after) {
                    return 2;
                }
                ++rpc_count;

                if (*cmd == "version") {
                    (void)client->write_all(malgrind::host::TclRpcCodec::request("Open On-Chip Debugger 0.12.0"));
                } else if (*cmd == "init") {
                    const auto fail = os::get_env("FAKE_OPENOCD_FAIL");
                    (void)client->write_all(malgrind::host::TclRpcCodec::request(
                        (fail && *fail == "init") ? "Error: init failed" : ""));
                } else if (*cmd == "dap info") {
                    const auto fail = os::get_env("FAKE_OPENOCD_FAIL");
                    if (fail && *fail == "dap") {
                        (void)client->write_all(
                            malgrind::host::TclRpcCodec::request("invalid command name \"dap\""));
                    } else if (const auto custom = os::get_env("FAKE_OPENOCD_DAP_INFO");
                               custom && !custom->empty()) {
                        (void)client->write_all(malgrind::host::TclRpcCodec::request(*custom));
                    } else {
                        std::uint32_t idcode = is_rp2040 ? 0x0bc12477u : 0x10076413u;
                        if (const auto id_env = os::get_env("FAKE_OPENOCD_IDCODE"); id_env && !id_env->empty()) {
                            if (const auto parsed = parse_u32(*id_env)) idcode = *parsed;
                        }
                        (void)client->write_all(malgrind::host::TclRpcCodec::request(
                            std::format("AP ID register 0x24770011\nIDCODE 0x{:08x}", idcode)));
                    }
                } else if (*cmd == "flash probe 0") {
                    const auto fail = os::get_env("FAKE_OPENOCD_FAIL");
                    if (fail && *fail == "flash_probe") {
                        (void)client->write_all(malgrind::host::TclRpcCodec::request("Error: flash probe failed"));
                    } else if (const auto custom = os::get_env("FAKE_OPENOCD_FLASH_PROBE");
                               custom && !custom->empty()) {
                        (void)client->write_all(malgrind::host::TclRpcCodec::request(*custom));
                    } else {
                        (void)client->write_all(malgrind::host::TclRpcCodec::request(
                            is_rp2040 ? "flash 'rp2040_flash' found at 0x10000000"
                                      : "flash 'stm32f4x' found at 0x08000000"));
                    }
                } else if (*cmd == "flash banks") {
                    const auto fail = os::get_env("FAKE_OPENOCD_FAIL");
                    if (fail && *fail == "flash_banks") {
                        (void)client->write_all(malgrind::host::TclRpcCodec::request("Error: no flash banks"));
                    } else if (const auto custom = os::get_env("FAKE_OPENOCD_FLASH_BANKS");
                               custom && !custom->empty()) {
                        (void)client->write_all(malgrind::host::TclRpcCodec::request(*custom));
                    } else {
                        std::uint32_t flash_size = is_rp2040 ? 0x00200000u : 0x00080000u;
                        if (const auto sz_env = os::get_env("FAKE_OPENOCD_FLASH_SIZE"); sz_env && !sz_env->empty()) {
                            if (const auto parsed = parse_u32(*sz_env)) flash_size = *parsed;
                        }
                        const std::string reply =
                            is_rp2040
                                ? std::format(
                                      "#0 : rp2040.flash (rp2040_flash) at 0x10000000, size 0x{:08x}, buswidth 0, chipwidth 0",
                                      flash_size)
                                : std::format(
                                      "#0 : stm32f4x.flash (stm32f4x) at 0x08000000, size 0x{:08x}, buswidth 0, chipwidth 0",
                                      flash_size);
                        (void)client->write_all(malgrind::host::TclRpcCodec::request(reply));
                    }
                } else if (cmd->starts_with("dump_image ")) {
                    const auto parts = split_tcl_words(std::string_view(*cmd).substr(11));
                    const auto fail = os::get_env("FAKE_OPENOCD_FAIL");
                    if (fail && *fail == "dump_image") {
                        (void)client->write_all(malgrind::host::TclRpcCodec::request("Error: dump_image failed"));
                    } else if (parts.size() < 3) {
                        (void)client->write_all(
                            malgrind::host::TclRpcCodec::request("Error: wrong # args: dump_image file addr size"));
                    } else {
                        const auto addr = parse_u32(parts[1]);
                        const auto len = parse_u32(parts[2]);
                        if (!addr || !len) {
                            (void)client->write_all(
                                malgrind::host::TclRpcCodec::request("Error: invalid dump_image address or length"));
                        } else if (!write_fake_flash_image(os::from_utf8(parts[0]), static_cast<std::size_t>(*len))) {
                            (void)client->write_all(
                                malgrind::host::TclRpcCodec::request("Error: couldn't open " + parts[0]));
                        } else {
                            (void)client->write_all(malgrind::host::TclRpcCodec::request(
                                std::format("dumped {} bytes in 0.050000s", *len)));
                        }
                    }
                } else if (cmd->starts_with("program ")) {
                    const auto parts = split_tcl_words(std::string_view(*cmd).substr(8));
                    const auto fail = os::get_env("FAKE_OPENOCD_FAIL");
                    if (fail && *fail == "program") {
                        (void)client->write_all(malgrind::host::TclRpcCodec::request("Error: programming failed"));
                    } else if (parts.empty()) {
                        (void)client->write_all(malgrind::host::TclRpcCodec::request("Error: missing image path"));
                    } else {
                        std::error_code ec;
                        if (!fs::is_regular_file(os::from_utf8(parts[0]), ec)) {
                            (void)client->write_all(
                                malgrind::host::TclRpcCodec::request("Error: couldn't open " + parts[0]));
                        } else {
                            (void)client->write_all(malgrind::host::TclRpcCodec::request(
                                "** Programming Started **\n** Programming Finished **\n** Verify Started **\n"
                                "** Verified OK **\n** Resetting Target **"));
                        }
                    }
                } else if (*cmd == "mdw" || cmd->starts_with("mdw ")) {
                    const std::string_view arg = cmd->size() > 4 ? trim_ws(std::string_view(*cmd).substr(4)) : "";
                    const auto addr = parse_u32(arg);
                    if (!addr) {
                        (void)client->write_all(malgrind::host::TclRpcCodec::request("Error: invalid mdw address"));
                    } else if (os::get_env("FAKE_OPENOCD_MDW_FAIL") == "1") {
                        (void)client->write_all(malgrind::host::TclRpcCodec::request("Error: memory read failed"));
                    } else if (*addr == 0xE0042000u || *addr == 0x40015800u || *addr == 0x5C001000u ||
                               *addr == 0x40000000u) {
                        const auto id_env = os::get_env("FAKE_OPENOCD_IDCODE");
                        if (id_env && *id_env == "none") {
                            (void)client->write_all(malgrind::host::TclRpcCodec::request("Error: memory read failed"));
                        } else {
                            std::uint32_t idcode = (*addr == 0x40000000u || is_rp2040) ? 0x20002927u : 0x10076413u;
                            if (id_env && !id_env->empty()) {
                                if (const auto parsed = parse_u32(*id_env)) idcode = *parsed;
                            }
                            (void)client->write_all(
                                malgrind::host::TclRpcCodec::request(std::format("0x{:08x}: {:08x}", *addr, idcode)));
                        }
                    } else if (const auto it = mem.find(*addr); it != mem.end()) {
                        (void)client->write_all(
                            malgrind::host::TclRpcCodec::request(std::format("0x{:08x}: {:08x}", *addr, it->second)));
                    } else {
                        (void)client->write_all(
                            malgrind::host::TclRpcCodec::request(std::format("0x{:08x}: 08001234", *addr)));
                    }
                } else if (cmd->starts_with("echo ")) {
                    (void)client->write_all(malgrind::host::TclRpcCodec::request(cmd->substr(5)));
                } else if (*cmd == "target names") {
                    std::string out;
                    for (std::size_t i = 0; i < targets.size(); ++i) {
                        if (i > 0) out.push_back(' ');
                        out.append(targets[i]);
                    }
                    (void)client->write_all(malgrind::host::TclRpcCodec::request(out));
                } else if (*cmd == "target current") {
                    const std::string cur = current_target < targets.size() ? targets[current_target] : "";
                    (void)client->write_all(malgrind::host::TclRpcCodec::request(cur));
                } else if (cmd->ends_with(" cget -type")) {
                    const std::string_view tname = trim_ws(std::string_view(*cmd).substr(0, cmd->size() - 11));
                    const bool known = std::ranges::find(targets, tname) != targets.end();
                    (void)client->write_all(
                        malgrind::host::TclRpcCodec::request(known ? target_type : "invalid command name"));
                } else if (*cmd == "targets") {
                    const std::string cur = current_target < targets.size() ? targets[current_target] : "";
                    (void)client->write_all(malgrind::host::TclRpcCodec::request(cur));
                } else if (cmd->starts_with("targets ")) {
                    const std::string_view tname = trim_ws(std::string_view(*cmd).substr(8));
                    const auto it = std::ranges::find(targets, tname);
                    if (it != targets.end()) {
                        current_target = static_cast<std::size_t>(it - targets.begin());
                        (void)client->write_all(malgrind::host::TclRpcCodec::request(""));
                    } else {
                        (void)client->write_all(malgrind::host::TclRpcCodec::request("Error: target not found"));
                    }
                } else if (*cmd == "halt") {
                    halted = true;
                    (void)client->write_all(malgrind::host::TclRpcCodec::request(""));
                } else if (*cmd == "resume") {
                    halted = false;
                    (void)client->write_all(malgrind::host::TclRpcCodec::request(""));
                } else if (*cmd == "reg pc") {
                    if (require_halt && !halted) {
                        (void)client->write_all(malgrind::host::TclRpcCodec::request("Target not halted"));
                    } else {
                        const std::size_t idx = current_target < pc_seqs.size() ? current_target : 0;
                        const auto& seq = pc_seqs[idx];
                        const std::string& val = seq[pc_idx[idx] % seq.size()];
                        ++pc_idx[idx];
                        (void)client->write_all(malgrind::host::TclRpcCodec::request(val));
                    }
                } else if (*cmd == "mrw" || cmd->starts_with("mrw ")) {
                    const std::string_view arg = cmd->size() > 4 ? trim_ws(std::string_view(*cmd).substr(4)) : "";
                    const auto addr = parse_u32(arg).value_or(0xE000101Cu);
                    (void)client->write_all(malgrind::host::TclRpcCodec::request(read_mem(addr)));
                } else if (cmd->starts_with("mww ")) {
                    const auto parts = split_ws(std::string_view(*cmd).substr(4));
                    if (parts.size() >= 2) {
                        const auto addr = parse_u32(parts[0]);
                        const auto val = parse_u32(parts[1]);
                        if (addr && val) {
                            if (!(demcr_ro && *addr == 0xE000EDFCu)) {
                                mem[*addr] = *val;
                            }
                            (void)client->write_all(malgrind::host::TclRpcCodec::request(""));
                        } else {
                            (void)client->write_all(malgrind::host::TclRpcCodec::request("Error: invalid mww args"));
                        }
                    } else {
                        (void)client->write_all(malgrind::host::TclRpcCodec::request("Error: invalid mww args"));
                    }
                } else if (cmd->find("for {set i 0} {$i < ") != std::string::npos &&
                           cmd->find("[mrw ") != std::string::npos) {
                    if (batch_override && !batch_override->empty()) {
                        (void)client->write_all(malgrind::host::TclRpcCodec::request(*batch_override));
                    } else if (mrw_fail) {
                        (void)client->write_all(malgrind::host::TclRpcCodec::request("Error: memory read failed"));
                    } else {
                        const std::string_view sv(*cmd);
                        const auto lt_pos = sv.find("$i < ");
                        const auto mrw_pos = sv.find("[mrw ");
                        std::size_t count = 64;
                        std::uint32_t addr = 0xE000101Cu;
                        if (lt_pos != std::string_view::npos) {
                            const auto end = sv.find('}', lt_pos + 5);
                            if (end != std::string_view::npos) {
                                if (const auto parsed_n = parse_u32(sv.substr(lt_pos + 5, end - (lt_pos + 5)))) {
                                    count = std::min<std::size_t>(*parsed_n, 4096);
                                }
                            }
                        }
                        if (mrw_pos != std::string_view::npos) {
                            const auto end = sv.find(']', mrw_pos + 5);
                            if (end != std::string_view::npos) {
                                if (const auto parsed_a = parse_u32(sv.substr(mrw_pos + 5, end - (mrw_pos + 5)))) {
                                    addr = *parsed_a;
                                }
                            }
                        }
                        std::string out;
                        for (std::size_t i = 0; i < count; ++i) {
                            if (i > 0) out.push_back(' ');
                            out.append(read_mem(addr));
                        }
                        (void)client->write_all(malgrind::host::TclRpcCodec::request(out));
                    }
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
    if (role == "picotool") return run_picotool(args);
    if (role == "echo") return run_echo(args);
    if (role == "addr2line" || role.ends_with("-addr2line")) return run_addr2line(args);
    if (role == "openocd") return run_openocd(args);

    std::fprintf(stderr, "fake_tool: unknown role '%s'\n", role.c_str());
    return 1;
}
