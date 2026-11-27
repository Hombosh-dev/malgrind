#include "families/rp2040.hpp"

#include "boards.hpp"
#include "openocd.hpp"
#include "os/paths.hpp"
#include "os/process.hpp"
#include "os/tools.hpp"
#include "probe_config.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <format>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

namespace fs = std::filesystem;

namespace malgrind::host {

namespace {

fs::path absolute_path(const fs::path& path) {
    auto abs = fs::absolute(path).lexically_normal();
    if (abs.filename().empty() && abs != abs.root_path()) abs = abs.parent_path();
    return abs;
}

std::string slurp(const fs::path& file) {
    std::ifstream in(file);
    return {std::istreambuf_iterator<char>(in), {}};
}

std::string lower_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string_view trim_ascii(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

bool is_rp2040_usb_cdc_pid(std::string_view pid) {
    // Target RP2040 USB-CDC PIDs only; never 000c (Raspberry Pi Debug Probe, shared across ARM targets).
    constexpr std::array<std::string_view, 5> kTargetPids{"000a", "0003", "0005", "000f", "f00a"};
    return std::ranges::find(kTargetPids, pid) != kTargetPids.end();
}

bool is_raspberrypi_platform(std::string_view platform) {
    return lower_ascii(platform).find("raspberrypi") != std::string::npos;
}

bool is_supported_rp2040_framework(std::string_view raw) {
    std::size_t i = 0;
    while (i < raw.size()) {
        while (i < raw.size() && (raw[i] == ',' || std::isspace(static_cast<unsigned char>(raw[i])))) ++i;
        const std::size_t start = i;
        while (i < raw.size() && raw[i] != ',' && !std::isspace(static_cast<unsigned char>(raw[i]))) ++i;
        if (i > start) {
            const std::string tok = lower_ascii(raw.substr(start, i - start));
            if (tok == "arduino" || tok == "picosdk") return true;
        }
    }
    return false;
}

bool is_safe_serial(std::string_view serial) {
    if (serial.empty() || serial.size() > 128) return false;
    return std::ranges::all_of(serial, [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == ':';
    });
}

bool is_safe_board_id(std::string_view board) {
    if (board.empty() || board.size() > 128) return false;
    return std::ranges::all_of(board, [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '_' || c == '-';
    });
}

std::string normalize_manifest_cfg(std::string_view raw, std::string_view default_dir) {
    std::string val(trim_ascii(raw));
    if (val.empty()) return {};
    std::string err;
    const auto sanitized = sanitize_target_cfg(val, err);
    if (!sanitized || sanitized->empty()) return {};
    val = *sanitized;
    if (val.find('/') == std::string::npos && val.find('\\') == std::string::npos) {
        val = std::string(default_dir) + "/" + val;
    }
    if (!lower_ascii(val).ends_with(".cfg")) {
        val += ".cfg";
    }
    return val;
}

std::string target_from_board_manifest(const fs::path& manifest) {
    std::error_code ec;
    if (!fs::is_regular_file(manifest, ec)) return {};
    const auto parsed = nlohmann::json::parse(slurp(manifest), nullptr, false);
    if (!parsed.is_object()) return {};

    if (parsed.contains("debug") && parsed["debug"].is_object()) {
        const auto& debug = parsed["debug"];
        if (debug.contains("openocd_target") && debug["openocd_target"].is_string()) {
            if (auto cfg = normalize_manifest_cfg(debug["openocd_target"].get<std::string>(), "target"); !cfg.empty()) {
                return cfg;
            }
        }
        if (debug.contains("openocd_board") && debug["openocd_board"].is_string()) {
            if (auto cfg = normalize_manifest_cfg(debug["openocd_board"].get<std::string>(), "board"); !cfg.empty()) {
                return cfg;
            }
        }
    }
    if (parsed.contains("build") && parsed["build"].is_object()) {
        const auto& build = parsed["build"];
        if (build.contains("mcu") && build["mcu"].is_string()) {
            if (lower_ascii(build["mcu"].get<std::string>()).find("rp2040") != std::string::npos) {
                return "target/rp2040.cfg";
            }
        }
    }
    return {};
}

std::string resolve_manifest_target(const fs::path& project_dir, std::string_view board) {
    if (!is_safe_board_id(board)) return {};
    const std::string filename = std::string(board) + ".json";

    if (!project_dir.empty()) {
        if (auto cfg = target_from_board_manifest(project_dir / "boards" / filename); !cfg.empty()) return cfg;
    }

    for (const auto& core : os::platformio_core_dirs(os::home_dir())) {
        const fs::path platforms_dir = core / "platforms";
        if (auto cfg = target_from_board_manifest(platforms_dir / "raspberrypi/boards" / filename); !cfg.empty()) {
            return cfg;
        }
        std::error_code ec;
        std::vector<fs::path> versioned;
        for (const auto& entry : fs::directory_iterator(platforms_dir, ec)) {
            if (!entry.is_directory(ec)) continue;
            const std::string name = os::to_utf8(entry.path().filename());
            if (name != "raspberrypi" && name.find("raspberrypi") != std::string::npos) {
                versioned.push_back(entry.path());
            }
        }
        std::ranges::sort(versioned);
        for (const auto& plat : versioned) {
            if (auto cfg = target_from_board_manifest(plat / "boards" / filename); !cfg.empty()) {
                return cfg;
            }
        }
    }
    return {};
}

bool text_indicates_rp2040(std::string_view raw) {
    const std::string lower = lower_ascii(raw);
    if (lower.empty()) return false;
    if (lower.find("pico32") != std::string::npos || lower.find("esp32") != std::string::npos) return false;
    if (lower.find("rp2040") != std::string::npos || lower.find("rpipico") != std::string::npos ||
        lower.find("raspberrypi") != std::string::npos) {
        return true;
    }
    std::size_t i = 0;
    while (i < lower.size()) {
        while (i < lower.size() && !std::isalnum(static_cast<unsigned char>(lower[i]))) ++i;
        const std::size_t start = i;
        while (i < lower.size() && std::isalnum(static_cast<unsigned char>(lower[i]))) ++i;
        if (i > start) {
            const std::string_view tok = std::string_view(lower).substr(start, i - start);
            if (tok == "pico" || tok == "picow") return true;
        }
    }
    return false;
}

bool is_openocd_error_reply(std::string_view reply) {
    const std::string lower = lower_ascii(trim_ascii(reply));
    return lower.starts_with("error:") || lower.starts_with("invalid command name") ||
           lower.find("couldn't open") != std::string::npos || lower.find("failed") != std::string::npos;
}

std::optional<std::uint32_t> parse_hex_u32_token(std::string_view tok) {
    tok = trim_ascii(tok);
    while (!tok.empty() && (tok.front() == '{' || tok.front() == '(')) tok.remove_prefix(1);
    while (!tok.empty() && (tok.back() == '}' || tok.back() == ')' || tok.back() == ',')) tok.remove_suffix(1);
    if (tok.starts_with("0x") || tok.starts_with("0X")) tok.remove_prefix(2);
    if (tok.empty() || tok.size() > 8) return std::nullopt;
    for (unsigned char c : tok) {
        if (std::isxdigit(c) == 0) return std::nullopt;
    }
    std::uint32_t val = 0;
    const auto [ptr, ec] = std::from_chars(tok.data(), tok.data() + tok.size(), val, 16);
    if (ec != std::errc{} || ptr != tok.data() + tok.size()) return std::nullopt;
    if (val == 0u || val == 0xFFFFFFFFu) return std::nullopt;
    return val;
}

std::optional<std::uint32_t> find_hex_after(std::string_view line, std::size_t pos) {
    while (pos < line.size()) {
        while (pos < line.size() &&
               (std::isspace(static_cast<unsigned char>(line[pos])) || line[pos] == ':' || line[pos] == '=')) {
            ++pos;
        }
        if (pos >= line.size()) break;
        const std::size_t start = pos;
        while (pos < line.size() && !std::isspace(static_cast<unsigned char>(line[pos])) && line[pos] != ',') {
            ++pos;
        }
        if (const auto val = parse_hex_u32_token(line.substr(start, pos - start))) return val;
    }
    return std::nullopt;
}

std::optional<std::uint32_t> parse_idcode_reply(std::string_view output) {
    output = trim_ascii(output);
    if (output.empty()) return std::nullopt;

    std::vector<std::string_view> lines;
    std::size_t pos = 0;
    while (pos <= output.size()) {
        const auto nl = output.find('\n', pos);
        const std::string_view line =
            trim_ascii(nl == std::string_view::npos ? output.substr(pos) : output.substr(pos, nl - pos));
        if (!line.empty() && !is_openocd_error_reply(line)) lines.push_back(line);
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }
    if (lines.empty()) return std::nullopt;

    for (const std::string_view line : lines) {
        const std::string lower = lower_ascii(line);
        for (const std::string_view key : {"idcode", "dpidr"}) {
            if (const auto at = lower.find(key); at != std::string::npos) {
                if (const auto val = find_hex_after(line, at + key.size())) return val;
            }
        }
    }
    for (const std::string_view line : lines) {
        if (const auto colon = line.find(':'); colon != std::string::npos) {
            const std::string_view prefix = trim_ascii(line.substr(0, colon));
            if (prefix.starts_with("0x") || prefix.starts_with("0X") ||
                lower_ascii(prefix).find("tap/device found") != std::string::npos) {
                if (const auto val = find_hex_after(line, colon + 1)) return val;
            }
        }
    }
    for (const std::string_view line : lines) {
        const std::string lower = lower_ascii(line);
        if (const auto at = lower.find("ap id register"); at != std::string::npos) {
            if (const auto val = find_hex_after(line, at + std::string_view("ap id register").size())) return val;
        }
    }
    if (lines.size() == 1 && lines[0].find(' ') == std::string_view::npos) {
        return parse_hex_u32_token(lines[0]);
    }
    return std::nullopt;
}

std::optional<std::uint32_t> parse_size_for_keyword(std::string_view output, std::string_view keyword) {
    std::size_t pos = 0;
    while (pos <= output.size()) {
        const auto nl = output.find('\n', pos);
        const std::string_view line =
            trim_ascii(nl == std::string_view::npos ? output.substr(pos) : output.substr(pos, nl - pos));
        if (!line.empty() && !is_openocd_error_reply(line)) {
            const std::string lower = lower_ascii(line);
            std::size_t search_from = 0;
            while (const auto at = lower.find(keyword, search_from)) {
                if (at == std::string::npos) break;
                search_from = at + keyword.size();
                if (at > 0 && std::isalnum(static_cast<unsigned char>(lower[at - 1]))) continue;
                std::size_t p = at + keyword.size();
                if (p < lower.size() && std::isalnum(static_cast<unsigned char>(lower[p]))) continue;
                while (p < lower.size() &&
                       (std::isspace(static_cast<unsigned char>(lower[p])) || lower[p] == ':' || lower[p] == '=')) {
                    ++p;
                }
                if (p >= lower.size()) continue;

                int base = 10;
                if (lower.substr(p).starts_with("0x")) {
                    p += 2;
                    base = 16;
                }
                const std::size_t num_start = p;
                while (p < lower.size() &&
                       (base == 16 ? std::isxdigit(static_cast<unsigned char>(lower[p])) != 0
                                   : std::isdigit(static_cast<unsigned char>(lower[p])) != 0)) {
                    ++p;
                }
                if (p == num_start) continue;

                std::uint64_t val = 0;
                const auto [ptr, ec] = std::from_chars(lower.data() + num_start, lower.data() + p, val, base);
                if (ec != std::errc{} || ptr != lower.data() + p || val == 0u) continue;

                while (p < lower.size() && std::isspace(static_cast<unsigned char>(lower[p]))) ++p;
                const std::size_t unit_start = p;
                while (p < lower.size() && std::isalpha(static_cast<unsigned char>(lower[p]))) ++p;
                const std::string_view unit = std::string_view(lower).substr(unit_start, p - unit_start);

                std::uint64_t mult = 1;
                if (unit == "k" || unit == "kb" || unit == "kib" || unit == "kbyte" || unit == "kbytes") {
                    mult = 1024ull;
                } else if (unit == "m" || unit == "mb" || unit == "mib" || unit == "mbyte" || unit == "mbytes") {
                    mult = 1024ull * 1024ull;
                } else if (!unit.empty() && unit != "b" && unit != "byte" && unit != "bytes") {
                    continue;
                }
                if (val <= 0xFFFFFFFFull / mult) {
                    return static_cast<std::uint32_t>(val * mult);
                }
            }
        }
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }
    return std::nullopt;
}

std::string parse_hex_id_token(std::string_view raw) {
    raw = trim_ascii(raw);
    std::size_t end = 0;
    while (end < raw.size() && !std::isspace(static_cast<unsigned char>(raw[end])) && raw[end] != ',') ++end;
    std::string_view tok = raw.substr(0, end);
    if (tok.starts_with("0x") || tok.starts_with("0X")) tok.remove_prefix(2);
    if (tok.empty() || tok.size() > 64) return {};
    std::string out;
    out.reserve(tok.size());
    bool all_zero = true;
    bool all_f = true;
    for (unsigned char c : tok) {
        if (std::isxdigit(c) == 0) return {};
        const char lc = static_cast<char>(std::tolower(c));
        if (lc != '0') all_zero = false;
        if (lc != 'f') all_f = false;
        out.push_back(lc);
    }
    if (all_zero || all_f) return {};
    return out;
}

std::string tcl_quote_path(const fs::path& path) {
    const std::string raw = os::to_utf8(path);
    for (unsigned char c : raw) {
        if (c < 0x20 || c == 0x7f) {
            throw std::runtime_error("path contains control characters unsupported by OpenOCD");
        }
    }
    const bool needs_quote = raw.empty() || std::ranges::any_of(raw, [](unsigned char c) {
        return std::isspace(c) != 0 || c == '"' || c == '\\' || c == '$' || c == '[' || c == ']' ||
               c == '{' || c == '}' || c == ';';
    });
    if (!needs_quote) return raw;

    std::string out = "\"";
    for (const char c : raw) {
        if (c == '\\' || c == '"' || c == '$' || c == '[' || c == ']') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

std::string target_override_from_env() {
    if (const auto raw = os::get_env("MALGRIND_RP2040_TARGET"); raw && !raw->empty()) {
        std::string err;
        const auto sanitized = sanitize_target_cfg(*raw, err);
        if (!sanitized) throw std::runtime_error(err);
        return *sanitized;
    }
    return {};
}

std::vector<std::string> select_rp2040_probe_args(std::string_view target_cfg) {
    const auto probes = find_probes();
    for (const auto& p : probes) {
        if (lower_ascii(p.vid) == "2e8a" && lower_ascii(p.pid) == "000c") {
            return rp2040_openocd_args(p.vid, p.pid, p.serial, target_cfg);
        }
    }
    for (const auto& p : probes) {
        const std::string hint = family_hint_for_usb(p.vid, p.pid);
        if (!hint.empty() && hint != "rp2040") continue;
        const std::string v = lower_ascii(p.vid);
        const std::string pid = lower_ascii(p.pid);
        if ((v == "0d28" && pid == "0204") || v == "1366" || v == "15ba") {
            return rp2040_openocd_args(p.vid, p.pid, p.serial, target_cfg);
        }
    }
    return rp2040_openocd_args("", "", "", target_cfg);
}

struct PicotoolResult {
    int exit_code = -1;
    std::string output;
};

PicotoolResult run_picotool(const fs::path& exe, const std::vector<std::string>& args, const LineFn& on_line) {
    std::vector<std::string> argv;
    argv.reserve(args.size() + 1);
    argv.push_back(os::to_utf8(exe));
    argv.insert(argv.end(), args.begin(), args.end());

    PicotoolResult res;
    res.exit_code = os::run_lines(argv, {}, [&](std::string_view line) {
        res.output.append(line).append("\n");
        if (on_line) on_line(line);
    });
    return res;
}

[[noreturn]] void throw_rp2040_access_error(std::string_view op,
                                            std::string_view openocd_err,
                                            const std::optional<fs::path>& picotool_exe,
                                            const std::optional<PicotoolResult>& picotool_res) {
    std::string detail;
    if (!openocd_err.empty()) {
        detail += std::format("OpenOCD SWD: {}", trim_ascii(openocd_err));
    }
    if (!picotool_exe) {
        if (!detail.empty()) detail += "; ";
        detail += "picotool: not found on PATH or in PlatformIO tool-picotool";
    } else if (picotool_res) {
        if (!detail.empty()) detail += "; ";
        const std::string_view trimmed = trim_ascii(picotool_res->output);
        if (!trimmed.empty()) {
            detail += std::format("picotool (exit {}): {}", picotool_res->exit_code, trimmed);
        } else {
            detail += std::format("picotool exited with code {}", picotool_res->exit_code);
        }
    }

    throw std::runtime_error(std::format(
        "cannot {} RP2040 board: neither a SWD debug probe (OpenOCD) nor a BOOTSEL device (picotool) was accessible.\n"
        "Hold the BOOTSEL button while connecting USB (or pressing RESET) to enter BOOTSEL mode, "
        "or connect a CMSIS-DAP debug probe (e.g. Raspberry Pi Debug Probe) over SWD.\n"
        "Details: {}",
        op, detail));
}

template <typename OpenOcdFn, typename PicotoolFn>
auto run_with_fallback(const fs::path& home,
                       std::string_view openocd_pkg,
                       std::string_view op,
                       OpenOcdFn&& run_ocd,
                       PicotoolFn&& run_pico) {
    std::string target_cfg = target_override_from_env();
    if (target_cfg.empty()) target_cfg = "target/rp2040.cfg";

    std::string openocd_err;
    if (const auto install = find_openocd(home, openocd_pkg)) {
        try {
            return run_ocd(*install, target_cfg);
        } catch (const std::exception& e) {
            openocd_err = e.what();
        }
    } else {
        openocd_err = std::format("OpenOCD ({}) was not found", openocd_pkg);
    }

    const auto picotool_exe = find_picotool(home);
    std::optional<PicotoolResult> picotool_res;
    if (picotool_exe) {
        using OpenOcdRet = std::invoke_result_t<OpenOcdFn, const OpenOcdInstall&, const std::string&>;
        if constexpr (std::is_void_v<OpenOcdRet>) {
            picotool_res = run_pico(*picotool_exe);
            if (picotool_res->exit_code == 0) return;
        } else {
            auto result = run_pico(*picotool_exe, picotool_res);
            if (result) return *result;
        }
    }
    throw_rp2040_access_error(op, openocd_err, picotool_exe, picotool_res);
}

struct AgentPaths {
    fs::path root, files, out;
};

AgentPaths agent_paths(const ProjectInfo& project, const fs::path& agent_dir) {
    AgentPaths p;
    p.root = absolute_path(agent_dir);
    p.files = p.root / "agent" / rp2040_family().id();
    if (!fs::exists(p.files / "inject.py") || !fs::exists(p.files / "autostart.cpp")) {
        throw std::runtime_error("the agent files are missing from " + os::to_utf8(p.files));
    }
    p.out = absolute_path(project.dir) / ".pio/malgrind";
    return p;
}

class Rp2040Family final : public Family {
public:
    [[nodiscard]] std::string_view id() const override { return "rp2040"; }
    [[nodiscard]] std::string_view name() const override { return "RP2040 (Raspberry Pi Pico)"; }
    [[nodiscard]] FamilyCaps caps() const override {
        return {.agent = true, .backup = true, .identify = true, .identify_resets_board = false, .memcheck = false};
    }
    [[nodiscard]] bool handles_arch(Arch arch) const override { return arch == Arch::ArmV7M; }

    [[nodiscard]] std::string project_problem(const ProjectInfo& project, const std::string& env) const override {
        if (const auto platform = project.platforms.find(env);
            platform != project.platforms.end() && !is_raspberrypi_platform(platform->second)) {
            return std::format("environment '{}' uses platform '{}': RP2040 supports raspberrypi platforms only",
                               env, platform->second);
        }
        if (const auto framework = project.frameworks.find(env);
            framework != project.frameworks.end() && !is_supported_rp2040_framework(framework->second)) {
            return std::format(
                "environment '{}' uses framework '{}': the RP2040 agent supports arduino or picosdk frameworks only",
                env, framework->second);
        }
        return {};
    }

    [[nodiscard]] unsigned agent_baud() const override { return kRp2040AgentBaud; }

    [[nodiscard]] AgentCommands agent_commands(const ToolContext& tools, const ProjectInfo& project,
                                               const std::string& env, const std::string& port,
                                               const RunSettings& settings, bool write_files) const override {
        (void)write_files;
        const auto p = agent_paths(project, tools.agent_root);
        const fs::path proj_dir = absolute_path(project.dir);
        const unsigned rate_hz = settings.rate_hz != 0 ? settings.rate_hz : kDefaultRateHz;

        std::vector<std::string> build_argv{os::to_utf8(tools.pio), "run", "-d", os::to_utf8(proj_dir), "-e", env};
        std::vector<std::string> upload_argv = build_argv;
        upload_argv.emplace_back("-t");
        upload_argv.emplace_back("upload");
        if (!port.empty()) {
            upload_argv.emplace_back("--upload-port");
            upload_argv.push_back(port);
        }

        std::vector<std::string> env_vars{
            "PLATFORMIO_BUILD_DIR=" + os::to_utf8(p.out / "build"),
            "PLATFORMIO_EXTRA_SCRIPTS=pre:" + os::to_utf8(p.files / "inject.py"),
            "MALGRIND_AGENT_DIR=" + os::to_utf8(p.files),
            "MALGRIND_CORE_DIR=" + os::to_utf8(p.root / "core"),
            "MALGRIND_PLATFORMS_DIR=" + os::to_utf8(p.root / "platforms"),
            "MALGRIND_RATE_HZ=" + std::to_string(rate_hz),
            "MALGRIND_START_SAMPLING=1",
        };

        return {{std::move(build_argv), env_vars},
                {std::move(upload_argv), std::move(env_vars)},
                p.out / "build" / env / "firmware.elf"};
    }

    BoardIdentity identify(const ToolContext& tools, const std::string& /*port*/, const LineFn& on_line) const override {
        return run_with_fallback(
            tools.home,
            openocd_package(),
            "identify",
            [&](const OpenOcdInstall& install, const std::string& target_cfg) -> BoardIdentity {
                OpenOcd ocd = OpenOcd::start(install, select_rp2040_probe_args(target_cfg), on_line);
                const std::string init_reply = ocd.call("init", 5000);
                if (is_openocd_error_reply(init_reply)) {
                    throw std::runtime_error(std::format("OpenOCD init failed: {}", trim_ascii(init_reply)));
                }

                std::optional<std::uint32_t> idcode;
                for (const std::string_view cmd : {"mdw 0x40000000", "dap info", "mdw 0xE000ED00"}) {
                    const std::string reply = ocd.call(cmd, 2000);
                    if (!is_openocd_error_reply(reply)) {
                        if ((idcode = parse_idcode_reply(reply))) break;
                    }
                }

                const std::string probe_reply = ocd.call("flash probe 0", 5000);
                const std::string banks_reply = ocd.call("flash banks", 2000);
                std::optional<std::uint32_t> flash_bytes = parse_rp2040_flash_size_bytes(probe_reply);
                if (!flash_bytes && !is_openocd_error_reply(banks_reply)) {
                    flash_bytes = parse_rp2040_flash_size_bytes(banks_reply);
                }

                if (!idcode || !flash_bytes || *flash_bytes == 0u) {
                    throw std::runtime_error("OpenOCD did not report RP2040 chip ID and flash size");
                }
                return {std::format("{:08x}-{:08x}", *idcode, *flash_bytes), *flash_bytes, "RP2040"};
            },
            [&](const fs::path& picotool_exe, std::optional<PicotoolResult>& res) -> std::optional<BoardIdentity> {
                res = run_picotool(picotool_exe, {"info", "-a"}, on_line);
                if (res->exit_code == 0) return parse_picotool_info(res->output);
                return std::nullopt;
            });
    }

    void read_flash(const ToolContext& tools, const std::string& /*port*/, const BoardIdentity& board,
                    const fs::path& out, const LineFn& on_line) const override {
        if (board.flash_bytes == 0u) {
            throw std::runtime_error("cannot read RP2040 flash: unknown flash size");
        }
        if (static_cast<std::uint64_t>(kRp2040FlashBase) + board.flash_bytes > 0xFFFFFFFFull) {
            throw std::runtime_error("cannot read RP2040 flash: flash size exceeds 32-bit address space");
        }

        run_with_fallback(
            tools.home,
            openocd_package(),
            "read flash from",
            [&](const OpenOcdInstall& install, const std::string& target_cfg) {
                OpenOcd ocd = OpenOcd::start(install, select_rp2040_probe_args(target_cfg), on_line);
                if (const auto init_reply = ocd.call("init", 5000); is_openocd_error_reply(init_reply)) {
                    throw std::runtime_error(std::format("OpenOCD init failed: {}", trim_ascii(init_reply)));
                }
                if (const auto halt_reply = ocd.call("halt", 5000); is_openocd_error_reply(halt_reply)) {
                    throw std::runtime_error(std::format("OpenOCD halt failed: {}", trim_ascii(halt_reply)));
                }
                const std::string dump_cmd =
                    std::format("dump_image {} 0x{:08x} {}", tcl_quote_path(out), kRp2040FlashBase, board.flash_bytes);
                const std::string dump_reply = ocd.call(dump_cmd, 60000);
                try {
                    (void)ocd.call("resume", 5000);
                } catch (...) {
                }
                if (is_openocd_error_reply(dump_reply)) {
                    throw std::runtime_error(std::format("OpenOCD dump_image failed: {}", trim_ascii(dump_reply)));
                }
            },
            [&](const fs::path& picotool_exe) {
                const std::uint32_t end_addr = kRp2040FlashBase + board.flash_bytes;
                return run_picotool(picotool_exe,
                                    {"save", "-r", std::format("0x{:08x}", kRp2040FlashBase),
                                     std::format("0x{:08x}", end_addr), os::to_utf8(out)},
                                    on_line);
            });
    }

    void write_flash(const ToolContext& tools, const std::string& /*port*/, const fs::path& image,
                     const LineFn& on_line) const override {
        std::error_code ec;
        if (!fs::is_regular_file(image, ec)) {
            throw std::runtime_error("cannot write RP2040 flash: image does not exist: " + os::to_utf8(image));
        }

        run_with_fallback(
            tools.home,
            openocd_package(),
            "write flash to",
            [&](const OpenOcdInstall& install, const std::string& target_cfg) {
                OpenOcd ocd = OpenOcd::start(install, select_rp2040_probe_args(target_cfg), on_line);
                const std::string prog_cmd =
                    std::format("program {} 0x{:08x} verify reset", tcl_quote_path(image), kRp2040FlashBase);
                const std::string prog_reply = ocd.call(prog_cmd, 120000);
                if (is_openocd_error_reply(prog_reply)) {
                    throw std::runtime_error(std::format("OpenOCD program failed: {}", trim_ascii(prog_reply)));
                }
            },
            [&](const fs::path& picotool_exe) {
                return run_picotool(picotool_exe, {"load", "-v", "-x", os::to_utf8(image)}, on_line);
            });
    }

    [[nodiscard]] bool is_agent_image(const fs::path& file) const override {
        std::ifstream in(file, std::ios::binary);
        if (!in) return false;
        constexpr std::array markers{kRp2040AgentMarker, kRp2040AgentMarkerV2};
        constexpr std::size_t kKeep = std::max(kRp2040AgentMarker.size(), kRp2040AgentMarkerV2.size()) - 1;
        std::vector<char> buf(1 << 16);
        std::string window;
        while (in.read(buf.data(), static_cast<std::streamsize>(buf.size())) || in.gcount() > 0) {
            window.append(buf.data(), static_cast<std::size_t>(in.gcount()));
            for (const std::string_view marker : markers) {
                if (window.find(marker) != std::string::npos) return true;
            }
            if (window.size() > kKeep) window.erase(0, window.size() - kKeep);
        }
        return false;
    }

    [[nodiscard]] std::vector<std::string> addr2line_names(Arch arch) const override {
        if (arch == Arch::ArmV7M) return {"arm-none-eabi-addr2line"};
        return {};
    }

    [[nodiscard]] std::string family_hint_for_usb(std::string_view vid, std::string_view pid) const override {
        if (lower_ascii(vid) == "2e8a" && is_rp2040_usb_cdc_pid(lower_ascii(pid))) return "rp2040";
        return {};
    }

    [[nodiscard]] std::string usb_adapter_name(std::string_view vid, std::string_view pid) const override {
        if (lower_ascii(vid) == "2e8a" && is_rp2040_usb_cdc_pid(lower_ascii(pid))) {
            return "Raspberry Pi USB serial";
        }
        return {};
    }

    [[nodiscard]] std::string openocd_package() const override { return "tool-openocd"; }

    [[nodiscard]] std::vector<std::string> openocd_interface_args(std::string_view /*vid*/,
                                                                  std::string_view /*pid*/,
                                                                  std::string_view /*description*/) const override {
        return {};
    }

    [[nodiscard]] bool handles_project(const ProjectInfo& project, const std::string& env) const override {
        const std::string effective_env = env.empty() ? project.default_env : env;
        if (!project.envs.empty() && !effective_env.empty() &&
            std::ranges::find(project.envs, effective_env) == project.envs.end()) {
            return false;
        }
        if (const auto platform = project.platforms.find(effective_env); platform != project.platforms.end()) {
            return is_raspberrypi_platform(platform->second);
        }
        return !openocd_target_cfg(project, env).empty();
    }

    [[nodiscard]] std::string openocd_target_cfg(const ProjectInfo& project, const std::string& env) const override {
        const std::string effective_env = env.empty() ? project.default_env : env;
        if (!project.envs.empty() && !effective_env.empty() &&
            std::ranges::find(project.envs, effective_env) == project.envs.end()) {
            return {};
        }
        bool has_rpi_platform = false;
        if (const auto platform = project.platforms.find(effective_env); platform != project.platforms.end()) {
            if (!is_raspberrypi_platform(platform->second)) return {};
            has_rpi_platform = true;
        }

        if (const auto board = project.boards.find(effective_env); board != project.boards.end()) {
            if (auto cfg = resolve_manifest_target(project.dir, board->second); !cfg.empty()) return cfg;
        }
        if (has_rpi_platform) {
            return "target/rp2040.cfg";
        }
        if (const auto mcu = project.mcus.find(effective_env); mcu != project.mcus.end()) {
            if (text_indicates_rp2040(mcu->second)) return "target/rp2040.cfg";
        }
        if (const auto board = project.boards.find(effective_env); board != project.boards.end()) {
            if (text_indicates_rp2040(board->second)) return "target/rp2040.cfg";
        }
        if (!effective_env.empty() && text_indicates_rp2040(effective_env)) {
            return "target/rp2040.cfg";
        }
        return {};
    }

    [[nodiscard]] std::vector<ProbeTargetOption> probe_target_options() const override {
        return {
            {"target/rp2040.cfg", "RP2040 (Cortex-M0+)"},
        };
    }

    [[nodiscard]] ProbeSampleMode probe_sample_mode(std::string_view target_cfg) const override {
        if (lower_ascii(target_cfg).find("rp2040") != std::string::npos) {
            return ProbeSampleMode::HaltResume;
        }
        return ProbeSampleMode::Auto;
    }

    [[nodiscard]] std::optional<Arch> probe_target_arch(std::string_view target_cfg) const override {
        if (lower_ascii(target_cfg).find("rp2040") != std::string::npos) {
            return Arch::ArmV7M;
        }
        return std::nullopt;
    }
};

} // namespace

const Family& rp2040_family() {
    static const Rp2040Family instance;
    return instance;
}

std::optional<std::uint32_t> parse_rp2040_flash_size_bytes(std::string_view output) {
    if (auto bytes = parse_size_for_keyword(output, "flash size")) return bytes;
    return parse_size_for_keyword(output, "size");
}

std::optional<BoardIdentity> parse_picotool_info(std::string_view output) {
    if (trim_ascii(output).empty()) return std::nullopt;
    const auto flash_bytes = parse_rp2040_flash_size_bytes(output);
    if (!flash_bytes || *flash_bytes == 0u) return std::nullopt;

    std::string chip_type;
    std::string flash_id;
    bool saw_pico_indicator = false;

    std::size_t pos = 0;
    while (pos <= output.size()) {
        const auto nl = output.find('\n', pos);
        const std::string_view line =
            trim_ascii(nl == std::string_view::npos ? output.substr(pos) : output.substr(pos, nl - pos));
        if (!line.empty() && !is_openocd_error_reply(line)) {
            if (const auto colon = line.find(':'); colon != std::string::npos) {
                const std::string key = lower_ascii(trim_ascii(line.substr(0, colon)));
                const std::string_view val = trim_ascii(line.substr(colon + 1));
                if (key == "type" && !val.empty()) {
                    chip_type = std::string(val);
                    if (lower_ascii(chip_type).find("rp2040") != std::string::npos) {
                        saw_pico_indicator = true;
                    }
                } else if (key == "flash id" || key == "unique id" || key == "device id") {
                    if (auto hex = parse_hex_id_token(val); !hex.empty()) {
                        flash_id = std::move(hex);
                        saw_pico_indicator = true;
                    }
                } else if (key == "pico_board") {
                    saw_pico_indicator = true;
                }
            }
        }
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }

    if (!saw_pico_indicator) return std::nullopt;
    if (chip_type.empty()) chip_type = "RP2040";

    std::string board_key;
    if (!flash_id.empty()) {
        const std::string suffix = std::format("-{:08x}", *flash_bytes);
        if (flash_id.size() + suffix.size() <= 64) {
            board_key = flash_id + suffix;
        } else {
            board_key = flash_id.substr(0, 64);
        }
    } else {
        board_key = std::format("20002927-{:08x}", *flash_bytes);
    }
    return BoardIdentity{std::move(board_key), *flash_bytes, std::move(chip_type)};
}

std::vector<std::string> rp2040_openocd_args(std::string_view vid,
                                             std::string_view pid,
                                             std::string_view serial,
                                             std::string_view target_cfg) {
    std::string target_err;
    const auto sanitized_target = sanitize_target_cfg(target_cfg, target_err);
    if (!sanitized_target) throw std::runtime_error(target_err);

    const std::string v = lower_ascii(vid);
    const std::string p = lower_ascii(pid);
    if (v.empty() && p.empty()) {
        std::vector<std::string> args{"-f", "interface/cmsis-dap.cfg", "-c", "adapter speed 5000"};
        if (is_safe_serial(serial)) {
            args.emplace_back("-c");
            args.push_back("adapter serial " + std::string(serial));
        }
        if (!sanitized_target->empty()) {
            args.emplace_back("-f");
            args.push_back(*sanitized_target);
        }
        return args;
    }

    Device probe;
    probe.kind = DeviceKind::Probe;
    probe.usb = os::UsbInfo{v, p, "", "", std::string(serial), ""};
    const std::string effective_target = sanitized_target->empty() ? std::string("target/rp2040.cfg") : *sanitized_target;
    const auto res = resolve_probe_config(probe, &rp2040_family(), nullptr, {}, effective_target);
    if (!res.config) throw std::runtime_error(res.error);

    auto args = res.config->openocd_args;
    if (sanitized_target->empty() && args.size() >= 2 && args[args.size() - 2] == "-f" &&
        args.back() == "target/rp2040.cfg") {
        args.pop_back();
        args.pop_back();
    }
    return args;
}

std::optional<fs::path> find_picotool(const fs::path& home) {
    if (auto from_path = os::find_in_path("picotool")) return from_path;

    const std::string bin_name = os::exe_name("picotool");
    auto inspect_pkg = [&](const fs::path& pkg) -> std::optional<fs::path> {
        std::error_code ec;
        if (!fs::is_directory(pkg, ec)) return std::nullopt;
        for (const fs::path& cand : {pkg / bin_name, pkg / "bin" / bin_name, pkg / "picotool" / bin_name}) {
            if (fs::is_regular_file(cand, ec)) return cand;
        }
        return std::nullopt;
    };

    for (const auto& core : os::platformio_core_dirs(home)) {
        const fs::path packages_dir = core / "packages";
        if (auto found = inspect_pkg(packages_dir / "tool-picotool")) return found;

        std::error_code ec;
        std::vector<fs::path> matches;
        for (const auto& entry : fs::directory_iterator(packages_dir, ec)) {
            if (!entry.is_directory(ec)) continue;
            const std::string name = os::to_utf8(entry.path().filename());
            if (name.starts_with("tool-picotool") && name != "tool-picotool") {
                matches.push_back(entry.path());
            }
        }
        std::ranges::sort(matches);
        for (const auto& pkg : matches) {
            if (auto found = inspect_pkg(pkg)) return found;
        }
    }

    if constexpr (os::kind() != os::Kind::Windows) {
        if (!home.empty()) {
            const fs::path local = home / ".local/bin" / bin_name;
            std::error_code ec;
            if (fs::is_regular_file(local, ec)) return local;
        }
    }

    return std::nullopt;
}

} // namespace malgrind::host
