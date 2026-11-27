#include "families/stm32.hpp"

#include "boards.hpp"
#include "openocd.hpp"
#include "os/paths.hpp"
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

bool is_stlink_vcp_pid(std::string_view pid) {
    constexpr std::array<std::string_view, 5> kVcpPids{"374b", "374e", "374f", "3752", "3753"};
    return std::ranges::find(kVcpPids, pid) != kVcpPids.end();
}

bool is_supported_stm32_framework(std::string_view raw) {
    std::size_t i = 0;
    while (i < raw.size()) {
        while (i < raw.size() && (raw[i] == ',' || std::isspace(static_cast<unsigned char>(raw[i])))) ++i;
        const std::size_t start = i;
        while (i < raw.size() && raw[i] != ',' && !std::isspace(static_cast<unsigned char>(raw[i]))) ++i;
        if (i > start) {
            const std::string tok = lower_ascii(raw.substr(start, i - start));
            if (tok == "stm32cube" || tok == "arduino") return true;
        }
    }
    return false;
}

std::vector<std::string_view> split_alnum_tokens(std::string_view s) {
    std::vector<std::string_view> tokens;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && !std::isalnum(static_cast<unsigned char>(s[i]))) ++i;
        const std::size_t start = i;
        while (i < s.size() && std::isalnum(static_cast<unsigned char>(s[i]))) ++i;
        if (i > start) tokens.push_back(s.substr(start, i - start));
    }
    return tokens;
}

struct SeriesTarget {
    std::string_view series;
    std::string_view cfg;
};

constexpr std::array<SeriesTarget, 17> kSeriesTargets{{
    {"f0", "target/stm32f0x.cfg"},
    {"f1", "target/stm32f1x.cfg"},
    {"f2", "target/stm32f2x.cfg"},
    {"f3", "target/stm32f3x.cfg"},
    {"f4", "target/stm32f4x.cfg"},
    {"f7", "target/stm32f7x.cfg"},
    {"g0", "target/stm32g0x.cfg"},
    {"g4", "target/stm32g4x.cfg"},
    {"h5", "target/stm32h5x.cfg"},
    {"h7", "target/stm32h7x.cfg"},
    {"l0", "target/stm32l0.cfg"},
    {"l1", "target/stm32l1.cfg"},
    {"l4", "target/stm32l4x.cfg"},
    {"l5", "target/stm32l5x.cfg"},
    {"u5", "target/stm32u5x.cfg"},
    {"wb", "target/stm32wbx.cfg"},
    {"wl", "target/stm32wlx.cfg"},
}};

std::string infer_stm32_target_from_text(std::string_view raw) {
    const std::string lower = lower_ascii(raw);
    if (lower.empty()) return {};

    for (const auto& [ser, cfg] : kSeriesTargets) {
        if (lower.find("stm32" + std::string(ser)) != std::string::npos) return std::string(cfg);
    }

    const bool has_st_board_prefix = lower.find("nucleo") != std::string::npos ||
                                     lower.find("disco") != std::string::npos ||
                                     lower.find("blackpill") != std::string::npos ||
                                     lower.find("bluepill") != std::string::npos;

    for (const std::string_view tok : split_alnum_tokens(lower)) {
        for (const auto& [ser, cfg] : kSeriesTargets) {
            if (!tok.starts_with(ser)) continue;
            if (tok.size() >= ser.size() + 2 &&
                std::isdigit(static_cast<unsigned char>(tok[ser.size()])) &&
                std::isdigit(static_cast<unsigned char>(tok[ser.size() + 1]))) {
                return std::string(cfg);
            }
            if (has_st_board_prefix && tok == ser) return std::string(cfg);
        }
    }

    if (lower.find("bluepill") != std::string::npos || lower.find("maple") != std::string::npos) {
        return "target/stm32f1x.cfg";
    }
    if (lower.find("blackpill") != std::string::npos) {
        return "target/stm32f4x.cfg";
    }
    return {};
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
            if (auto cfg = infer_stm32_target_from_text(build["mcu"].get<std::string>()); !cfg.empty()) {
                return cfg;
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
        if (auto cfg = target_from_board_manifest(platforms_dir / "ststm32/boards" / filename); !cfg.empty()) {
            return cfg;
        }
        std::error_code ec;
        std::vector<fs::path> versioned;
        for (const auto& entry : fs::directory_iterator(platforms_dir, ec)) {
            if (!entry.is_directory(ec)) continue;
            if (os::to_utf8(entry.path().filename()).starts_with("ststm32@")) {
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
        while (pos < line.size() && (std::isspace(static_cast<unsigned char>(line[pos])) || line[pos] == ':' || line[pos] == '=')) {
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

bool is_openocd_error_reply(std::string_view reply) {
    const std::string lower = lower_ascii(trim_ascii(reply));
    return lower.starts_with("error:") || lower.starts_with("invalid command name") ||
           lower.find("couldn't open") != std::string::npos || lower.find("failed") != std::string::npos;
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
    if (const auto raw = os::get_env("MALGRIND_STM32_TARGET"); raw && !raw->empty()) {
        std::string err;
        const auto sanitized = sanitize_target_cfg(*raw, err);
        if (!sanitized) throw std::runtime_error(err);
        return *sanitized;
    }
    return {};
}

std::vector<std::string> select_stm32_probe_args(std::string_view target_cfg) {
    const auto probes = find_probes();
    for (const auto& p : probes) {
        if (lower_ascii(p.vid) == "0483") {
            return stm32_openocd_args(p.vid, p.pid, p.serial, target_cfg);
        }
    }
    for (const auto& p : probes) {
        const std::string hint = family_hint_for_usb(p.vid, p.pid);
        if (!hint.empty() && hint != "stm32") continue;
        const std::string v = lower_ascii(p.vid);
        const std::string pid = lower_ascii(p.pid);
        if ((v == "0d28" && pid == "0204") || (v == "2e8a" && pid == "000c") || v == "1366" || v == "15ba") {
            return stm32_openocd_args(p.vid, p.pid, p.serial, target_cfg);
        }
    }
    return stm32_openocd_args("", "", "", target_cfg);
}

std::string stm32_chip_from_idcode(std::uint32_t idcode, std::string_view banks_reply) {
    const unsigned dev_id = idcode & 0xFFFu;
    switch (dev_id) {
        case 0x440: case 0x442: case 0x444: case 0x445: case 0x448: return "STM32F0";
        case 0x410: case 0x412: case 0x414: case 0x418: case 0x420: case 0x428: case 0x430: return "STM32F1";
        case 0x411: return "STM32F2";
        case 0x422: case 0x432: case 0x438: case 0x439: case 0x446: return "STM32F3";
        case 0x413: case 0x419: case 0x421: case 0x423: case 0x431: case 0x433: case 0x434: case 0x441:
        case 0x458: case 0x463: return "STM32F4";
        case 0x449: case 0x451: case 0x452: return "STM32F7";
        case 0x456: case 0x460: case 0x466: case 0x467: return "STM32G0";
        case 0x468: case 0x469: case 0x479: return "STM32G4";
        case 0x450: case 0x480: case 0x483: return "STM32H7";
        case 0x417: case 0x425: case 0x447: case 0x457: return "STM32L0";
        case 0x416: case 0x427: case 0x429: case 0x436: case 0x437: return "STM32L1";
        case 0x415: case 0x435: case 0x461: case 0x462: case 0x464: case 0x470: case 0x471: return "STM32L4";
        case 0x472: return "STM32L5";
        case 0x481: case 0x482: return "STM32U5";
        default: break;
    }
    const std::string lower_banks = lower_ascii(banks_reply);
    for (const auto& [ser, cfg] : kSeriesTargets) {
        (void)cfg;
        if (lower_banks.find("stm32" + std::string(ser)) != std::string::npos) {
            std::string up = "STM32";
            for (const char c : ser) up.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
            return up;
        }
    }
    return std::format("STM32 (0x{:08x})", idcode);
}

std::string target_cfg_for_board(const BoardIdentity& board) {
    if (const auto env_cfg = target_override_from_env(); !env_cfg.empty()) return env_cfg;
    if (auto cfg = infer_stm32_target_from_text(board.chip); !cfg.empty()) return cfg;
    return "target/stm32f4x.cfg";
}

OpenOcdInstall require_openocd(const fs::path& home, std::string_view package) {
    if (auto install = find_openocd(home, package)) return *install;
    throw std::runtime_error(std::format(
        "OpenOCD ({}) was not found: install PlatformIO package '{}' or openocd on PATH", package, package));
}

struct AgentPaths {
    fs::path root, files, out;
};

AgentPaths agent_paths(const ProjectInfo& project, const fs::path& agent_dir) {
    AgentPaths p;
    p.root = absolute_path(agent_dir);
    p.files = p.root / "agent" / stm32_family().id();
    if (!fs::exists(p.files / "inject.py") || !fs::exists(p.files / "autostart.cpp")) {
        throw std::runtime_error("the agent files are missing from " + os::to_utf8(p.files));
    }
    p.out = absolute_path(project.dir) / ".pio/malgrind";
    return p;
}

class Stm32Family final : public Family {
public:
    [[nodiscard]] std::string_view id() const override { return "stm32"; }
    [[nodiscard]] std::string_view name() const override { return "STM32 (Cortex-M)"; }
    [[nodiscard]] FamilyCaps caps() const override {
        return {.agent = true, .backup = true, .identify = true, .identify_resets_board = false, .memcheck = false};
    }
    [[nodiscard]] bool handles_arch(Arch arch) const override { return arch == Arch::ArmV7M; }

    [[nodiscard]] std::string project_problem(const ProjectInfo& project, const std::string& env) const override {
        if (const auto platform = project.platforms.find(env);
            platform != project.platforms.end() && !platform->second.starts_with("ststm32")) {
            return std::format("environment '{}' uses platform '{}': STM32 supports platform ststm32 only",
                               env, platform->second);
        }
        if (const auto framework = project.frameworks.find(env);
            framework != project.frameworks.end() && !is_supported_stm32_framework(framework->second)) {
            return std::format(
                "environment '{}' uses framework '{}': the STM32 agent supports stm32cube or arduino frameworks only",
                env, framework->second);
        }
        return {};
    }

    [[nodiscard]] unsigned agent_baud() const override { return kStm32AgentBaud; }

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
        const auto install = require_openocd(tools.home, openocd_package());
        const std::string override_cfg = target_override_from_env();

        std::vector<std::string> target_candidates;
        if (!override_cfg.empty()) {
            target_candidates.push_back(override_cfg);
        } else {
            target_candidates = {
                "target/stm32f4x.cfg", "target/stm32f1x.cfg", "target/stm32f0x.cfg",
                "target/stm32l4x.cfg", "target/stm32h7x.cfg", "target/stm32g4x.cfg",
                "target/stm32g0x.cfg", "target/stm32l0.cfg",  "target/stm32f3x.cfg",
                "target/stm32f7x.cfg",
            };
        }

        std::optional<OpenOcd> ocd;
        std::string selected_cfg;
        std::string last_init_err;
        for (const auto& target_cfg : target_candidates) {
            OpenOcd candidate = OpenOcd::start(install, select_stm32_probe_args(target_cfg), on_line);
            try {
                const std::string init_reply = candidate.call("init", 5000);
                if (!is_openocd_error_reply(init_reply)) {
                    ocd.emplace(std::move(candidate));
                    selected_cfg = target_cfg;
                    break;
                }
                last_init_err = init_reply;
            } catch (const std::exception& e) {
                last_init_err = e.what();
                if (target_candidates.size() == 1) throw;
            }
        }
        if (!ocd) {
            throw std::runtime_error(std::format("OpenOCD init failed while identifying STM32 board: {}",
                                                 trim_ascii(last_init_err)));
        }

        const auto try_idcode_cmd = [&](std::string_view cmd) -> std::optional<std::uint32_t> {
            const std::string reply = ocd->call(cmd, 2000);
            if (is_openocd_error_reply(reply)) return std::nullopt;
            return parse_openocd_idcode(reply);
        };

        std::vector<std::string_view> idcode_cmds;
        const std::string lower_cfg = lower_ascii(selected_cfg);
        if (lower_cfg.find("f0") != std::string::npos || lower_cfg.find("g0") != std::string::npos ||
            lower_cfg.find("l0") != std::string::npos) {
            idcode_cmds = {"mdw 0x40015800", "mdw 0xE0042000", "mdw 0x5C001000", "dap info", "mdw 0xE000ED00"};
        } else if (lower_cfg.find("h7") != std::string::npos) {
            idcode_cmds = {"mdw 0x5C001000", "mdw 0xE0042000", "mdw 0x40015800", "dap info", "mdw 0xE000ED00"};
        } else {
            idcode_cmds = {"mdw 0xE0042000", "mdw 0x40015800", "mdw 0x5C001000", "dap info", "mdw 0xE000ED00"};
        }

        std::optional<std::uint32_t> idcode;
        for (const std::string_view cmd : idcode_cmds) {
            if ((idcode = try_idcode_cmd(cmd))) break;
        }

        const std::string probe_reply = ocd->call("flash probe 0", 5000);
        const std::string banks_reply = ocd->call("flash banks", 2000);

        std::optional<std::uint32_t> flash_bytes = parse_openocd_flash_size_bytes(probe_reply);
        if (!flash_bytes && !is_openocd_error_reply(banks_reply)) {
            flash_bytes = parse_openocd_flash_size_bytes(banks_reply);
        }

        if (!idcode || !flash_bytes || *flash_bytes == 0u) {
            throw std::runtime_error("cannot identify STM32 board (OpenOCD did not report IDCODE and flash size)");
        }

        const std::string key = format_stm32_board_key(*idcode, *flash_bytes);
        if (key.empty()) {
            throw std::runtime_error("cannot format STM32 board identity key");
        }
        const std::string chip = stm32_chip_from_idcode(*idcode, banks_reply);
        return {key, *flash_bytes, chip};
    }

    void read_flash(const ToolContext& tools, const std::string& /*port*/, const BoardIdentity& board,
                    const fs::path& out, const LineFn& on_line) const override {
        if (board.flash_bytes == 0u) {
            throw std::runtime_error("cannot read STM32 flash: unknown flash size");
        }
        const auto install = require_openocd(tools.home, openocd_package());
        const std::string target_cfg = target_cfg_for_board(board);
        OpenOcd ocd = OpenOcd::start(install, select_stm32_probe_args(target_cfg), on_line);

        if (const auto init_reply = ocd.call("init", 5000); is_openocd_error_reply(init_reply)) {
            throw std::runtime_error(std::format("OpenOCD init failed before reading flash: {}", trim_ascii(init_reply)));
        }
        if (const auto halt_reply = ocd.call("halt", 5000); is_openocd_error_reply(halt_reply)) {
            throw std::runtime_error(std::format("OpenOCD halt failed before reading flash: {}", trim_ascii(halt_reply)));
        }

        const std::string dump_cmd =
            std::format("dump_image {} 0x{:08x} {}", tcl_quote_path(out), kStm32FlashBase, board.flash_bytes);
        const std::string dump_reply = ocd.call(dump_cmd, 60000);
        try {
            (void)ocd.call("resume", 5000);
        } catch (...) {
        }
        if (is_openocd_error_reply(dump_reply)) {
            throw std::runtime_error(std::format("OpenOCD dump_image failed: {}", trim_ascii(dump_reply)));
        }
    }

    void write_flash(const ToolContext& tools, const std::string& /*port*/, const fs::path& image,
                     const LineFn& on_line) const override {
        std::error_code ec;
        if (!fs::is_regular_file(image, ec)) {
            throw std::runtime_error("cannot write STM32 flash: image does not exist: " + os::to_utf8(image));
        }
        const auto install = require_openocd(tools.home, openocd_package());
        std::string target_cfg = target_override_from_env();
        if (target_cfg.empty()) target_cfg = "target/stm32f4x.cfg";
        OpenOcd ocd = OpenOcd::start(install, select_stm32_probe_args(target_cfg), on_line);

        const std::string prog_cmd =
            std::format("program {} 0x{:08x} verify reset", tcl_quote_path(image), kStm32FlashBase);
        const std::string prog_reply = ocd.call(prog_cmd, 120000);
        if (is_openocd_error_reply(prog_reply)) {
            throw std::runtime_error(std::format("OpenOCD program failed: {}", trim_ascii(prog_reply)));
        }
    }

    [[nodiscard]] bool is_agent_image(const fs::path& file) const override {
        std::ifstream in(file, std::ios::binary);
        if (!in) return false;
        constexpr std::array markers{kStm32AgentMarker, kStm32AgentMarkerV2};
        constexpr std::size_t kKeep = std::max(kStm32AgentMarker.size(), kStm32AgentMarkerV2.size()) - 1;
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
        if (lower_ascii(vid) == "0483" && is_stlink_vcp_pid(lower_ascii(pid))) return "stm32";
        return {};
    }

    [[nodiscard]] std::string usb_adapter_name(std::string_view vid, std::string_view pid) const override {
        if (lower_ascii(vid) == "0483" && is_stlink_vcp_pid(lower_ascii(pid))) return "ST-Link Virtual COM Port";
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
            return platform->second.starts_with("ststm32");
        }
        return !openocd_target_cfg(project, env).empty();
    }

    [[nodiscard]] std::string openocd_target_cfg(const ProjectInfo& project, const std::string& env) const override {
        const std::string effective_env = env.empty() ? project.default_env : env;
        if (!project.envs.empty() && !effective_env.empty() &&
            std::ranges::find(project.envs, effective_env) == project.envs.end()) {
            return {};
        }
        if (const auto platform = project.platforms.find(effective_env);
            platform != project.platforms.end() && !platform->second.starts_with("ststm32")) {
            return {};
        }

        if (const auto board = project.boards.find(effective_env); board != project.boards.end()) {
            if (auto cfg = resolve_manifest_target(project.dir, board->second); !cfg.empty()) return cfg;
        }
        if (const auto mcu = project.mcus.find(effective_env); mcu != project.mcus.end()) {
            if (auto cfg = infer_stm32_target_from_text(mcu->second); !cfg.empty()) return cfg;
        }
        if (const auto board = project.boards.find(effective_env); board != project.boards.end()) {
            if (auto cfg = infer_stm32_target_from_text(board->second); !cfg.empty()) return cfg;
        }
        if (!effective_env.empty()) {
            if (auto cfg = infer_stm32_target_from_text(effective_env); !cfg.empty()) return cfg;
        }
        return {};
    }

    [[nodiscard]] std::vector<ProbeTargetOption> probe_target_options() const override {
        return {
            {"target/stm32f1x.cfg", "STM32F1 (Cortex-M3)"},
            {"target/stm32f4x.cfg", "STM32F4 (Cortex-M4)"},
            {"target/stm32l4x.cfg", "STM32L4 (Cortex-M4)"},
            {"target/stm32h7x.cfg", "STM32H7 (Cortex-M7)"},
            {"target/stm32g4x.cfg", "STM32G4 (Cortex-M4)"},
            {"target/stm32f0x.cfg", "STM32F0 (Cortex-M0)"},
        };
    }

    [[nodiscard]] ProbeSampleMode probe_sample_mode(std::string_view target_cfg) const override {
        const std::string lower = lower_ascii(target_cfg);
        if (lower.empty()) return ProbeSampleMode::Auto;
        for (const std::string_view m0 : {"stm32f0", "stm32g0", "stm32l0", "nucleo_f0", "nucleo_g0", "nucleo_l0",
                                          "disco_f0", "disco_g0", "disco_l0"}) {
            if (lower.find(m0) != std::string::npos) return ProbeSampleMode::HaltResume;
        }
        for (const std::string_view dwt : {"stm32f1", "stm32f2", "stm32f3", "stm32f4", "stm32f7", "stm32g4", "stm32h5",
                                           "stm32h7", "stm32l1", "stm32l4", "stm32l5", "stm32u5", "stm32wb", "stm32wl",
                                           "nucleo_f1", "nucleo_f3", "nucleo_f4", "nucleo_f7", "nucleo_g4",
                                           "nucleo_h7", "nucleo_l4"}) {
            if (lower.find(dwt) != std::string::npos) return ProbeSampleMode::DwtPcsr;
        }
        return ProbeSampleMode::Auto;
    }

    [[nodiscard]] std::optional<Arch> probe_target_arch(std::string_view target_cfg) const override {
        const std::string lower = lower_ascii(target_cfg);
        if (lower.find("stm32") != std::string::npos || lower.find("st_nucleo") != std::string::npos ||
            lower.find("st_discovery") != std::string::npos) {
            return Arch::ArmV7M;
        }
        return std::nullopt;
    }
};

} // namespace

const Family& stm32_family() {
    static const Stm32Family instance;
    return instance;
}

std::optional<std::uint32_t> parse_openocd_idcode(std::string_view output) {
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

std::optional<std::uint32_t> parse_openocd_flash_size_bytes(std::string_view output) {
    std::size_t pos = 0;
    while (pos <= output.size()) {
        const auto nl = output.find('\n', pos);
        const std::string_view line =
            trim_ascii(nl == std::string_view::npos ? output.substr(pos) : output.substr(pos, nl - pos));
        if (!line.empty() && !is_openocd_error_reply(line)) {
            const std::string lower = lower_ascii(line);
            std::size_t search_from = 0;
            while (const auto at = lower.find("size", search_from)) {
                if (at == std::string::npos) break;
                search_from = at + 4;
                if (at > 0 && std::isalnum(static_cast<unsigned char>(lower[at - 1]))) continue;
                std::size_t p = at + 4;
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
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return std::nullopt;
}

std::string format_stm32_board_key(std::uint32_t idcode, std::uint32_t flash_bytes) {
    if (idcode == 0u) return {};
    if (flash_bytes == 0u) return std::format("{:08x}", idcode);
    return std::format("{:08x}-{:08x}", idcode, flash_bytes);
}

std::vector<std::string> stm32_openocd_args(std::string_view vid,
                                            std::string_view pid,
                                            std::string_view serial,
                                            std::string_view target_cfg) {
    std::string target_err;
    const auto sanitized_target = sanitize_target_cfg(target_cfg, target_err);
    if (!sanitized_target) throw std::runtime_error(target_err);

    const std::string v = lower_ascii(vid);
    const std::string p = lower_ascii(pid);
    if (v.empty() && p.empty()) {
        std::vector<std::string> args{"-f", "interface/stlink.cfg"};
        const bool safe_sn = !serial.empty() && serial.size() <= 128 &&
                             std::ranges::all_of(serial, [](unsigned char c) {
                                 return std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == ':';
                             });
        if (safe_sn) {
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
    const std::string effective_target = sanitized_target->empty() ? std::string("target/stm32f4x.cfg") : *sanitized_target;
    const auto res = resolve_probe_config(probe, &stm32_family(), nullptr, {}, effective_target);
    if (!res.config) throw std::runtime_error(res.error);

    auto args = res.config->openocd_args;
    if (sanitized_target->empty() && args.size() >= 2 && args[args.size() - 2] == "-f" &&
        args.back() == "target/stm32f4x.cfg") {
        args.pop_back();
        args.pop_back();
    }
    return args;
}

} // namespace malgrind::host
