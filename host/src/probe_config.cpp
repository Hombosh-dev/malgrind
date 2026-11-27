#include "probe_config.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <utility>

namespace malgrind::host {

namespace {

std::string lower_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string trim_spaces(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    return std::string(s);
}

bool has_control_char(std::string_view s) {
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7f) return true;
    }
    return false;
}

std::optional<std::string> sanitize_target_cfg(std::string_view raw, std::string& error) {
    if (raw.empty()) return std::string{};
    if (has_control_char(raw)) {
        error = "invalid target config path: control characters are not allowed";
        return std::nullopt;
    }
    std::string trimmed = trim_spaces(raw);
    if (trimmed.empty()) {
        error = "invalid target config path: must not be blank";
        return std::nullopt;
    }
    if (trimmed.front() == '-') {
        error = "invalid target config path: must not start with '-'";
        return std::nullopt;
    }
    return trimmed;
}

bool is_safe_serial(std::string_view serial) {
    if (serial.empty() || serial.size() > 128) return false;
    return std::ranges::all_of(serial, [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == ':';
    });
}

bool is_stlink_pid(std::string_view pid) {
    constexpr std::array<std::string_view, 8> kPids{
        "3744", "3748", "374b", "374d", "374e", "374f", "3752", "3753",
    };
    return std::ranges::find(kPids, pid) != kPids.end();
}

const Family* resolve_family(const Device& probe, std::string_view vid, std::string_view pid,
                             const ProjectInfo* project, const std::string& effective_env,
                             std::string_view target_cfg_override, const Family* explicit_family) {
    if (explicit_family != nullptr) return explicit_family;
    if (project != nullptr) {
        for (const Family* f : all_families()) {
            if (f->handles_project(*project, effective_env)) return f;
        }
    }
    if (!target_cfg_override.empty()) {
        for (const Family* f : all_families()) {
            for (const auto& opt : f->probe_target_options()) {
                if (opt.cfg == target_cfg_override) return f;
            }
        }
    }
    for (const Family* f : all_families()) {
        if (!f->usb_probe_name(vid, pid).empty()) return f;
    }
    if (!probe.family_hint.empty()) {
        if (const Family* hinted = family_by_id(probe.family_hint)) return hinted;
    }
    return nullptr;
}

struct GenericInterface {
    std::string desc;
    std::vector<std::string> args;
};

std::optional<GenericInterface> resolve_generic_interface(std::string_view vid, std::string_view pid,
                                                          std::string& error) {
    if (vid == "0483" && is_stlink_pid(pid)) {
        return GenericInterface{"ST-Link", {"-f", "interface/stlink.cfg"}};
    }
    if (vid == "0d28" && pid == "0204") {
        return GenericInterface{"CMSIS-DAP (DAPLink)", {"-f", "interface/cmsis-dap.cfg"}};
    }
    if (vid == "2e8a" && pid == "000c") {
        return GenericInterface{"Raspberry Pi Debug Probe", {"-f", "interface/cmsis-dap.cfg", "-c", "adapter speed 5000"}};
    }
    if (vid == "1366") {
        return GenericInterface{"SEGGER J-Link", {"-f", "interface/jlink.cfg"}};
    }
    if (vid == "15ba") {
        return GenericInterface{"Olimex debugger", {"-f", "interface/ftdi/olimex-arm-usb-ocd-h.cfg"}};
    }
    if (vid == "0403" && (pid == "6010" || pid == "6014")) {
        error = std::format(
            "FTDI FT2232/FT232H probe ({}:{}) requires a project or target family to select its JTAG adapter config",
            vid, pid);
        return std::nullopt;
    }
    error = std::format("unsupported debug probe (USB {}:{}): no OpenOCD interface configuration is known", vid, pid);
    return std::nullopt;
}

bool contains_serial_arg(const std::vector<std::string>& args) {
    return std::ranges::any_of(args, [](std::string_view arg) {
        return arg.find("adapter serial") != std::string_view::npos;
    });
}

} // namespace

ProbeConfigResult resolve_probe_config(const ProbeConfigRequest& req) {
    if (req.probe.kind != DeviceKind::Probe) {
        return {std::nullopt, "selected device is not a debug probe"};
    }
    if (!req.probe.usb) {
        return {std::nullopt, "debug probe has no USB vendor/product IDs"};
    }

    const std::string vid = lower_ascii(req.probe.usb->vid);
    const std::string pid = lower_ascii(req.probe.usb->pid);
    if (vid.empty() || pid.empty()) {
        return {std::nullopt, "debug probe has incomplete USB vendor/product IDs"};
    }

    std::string effective_env;
    if (req.project != nullptr) {
        effective_env = req.env.empty() ? req.project->default_env : req.env;
        if (effective_env.empty() && !req.project->envs.empty()) {
            effective_env = req.project->envs.front();
        }
        if (!req.env.empty() && !req.project->envs.empty() &&
            std::ranges::find(req.project->envs, req.env) == req.project->envs.end()) {
            return {std::nullopt, std::format("platformio.ini has no environment '{}'", req.env)};
        }
    }

    std::string sanitize_error;
    const auto sanitized_override = sanitize_target_cfg(req.target_cfg_override, sanitize_error);
    if (!sanitized_override) {
        return {std::nullopt, std::move(sanitize_error)};
    }

    const Family* family =
        resolve_family(req.probe, vid, pid, req.project, effective_env, *sanitized_override, req.family);

    ProbeConfig cfg;
    cfg.package = family ? family->openocd_package() : std::string("tool-openocd");
    if (cfg.package.empty()) cfg.package = "tool-openocd";

    const std::string desc_hint = !req.probe.usb->product.empty() ? req.probe.usb->product : req.probe.name;
    if (family != nullptr) {
        if (auto fam_iface = family->openocd_interface_args(vid, pid, desc_hint); !fam_iface.empty()) {
            cfg.openocd_args = std::move(fam_iface);
            if (!req.probe.name.empty()) {
                cfg.interface_desc = req.probe.name;
            } else if (!desc_hint.empty()) {
                cfg.interface_desc = desc_hint;
            } else {
                cfg.interface_desc = std::string(family->name());
            }
        }
    }

    if (cfg.openocd_args.empty()) {
        std::string iface_error;
        auto generic = resolve_generic_interface(vid, pid, iface_error);
        if (!generic) {
            return {std::nullopt, std::move(iface_error)};
        }
        cfg.interface_desc = !req.probe.name.empty() ? req.probe.name : std::move(generic->desc);
        cfg.openocd_args = std::move(generic->args);
    }

    if (is_safe_serial(req.probe.usb->serial) && !contains_serial_arg(cfg.openocd_args)) {
        cfg.openocd_args.emplace_back("-c");
        cfg.openocd_args.push_back("adapter serial " + req.probe.usb->serial);
    }

    if (!sanitized_override->empty()) {
        cfg.target_cfg = *sanitized_override;
    } else if (family != nullptr && req.project != nullptr) {
        cfg.target_cfg = family->openocd_target_cfg(*req.project, effective_env);
    }

    if (cfg.target_cfg.empty()) {
        return {std::nullopt, "choose the chip target config for OpenOCD"};
    }

    cfg.openocd_args.emplace_back("-f");
    cfg.openocd_args.push_back(cfg.target_cfg);
    cfg.sample_mode = family != nullptr ? family->probe_sample_mode(cfg.target_cfg) : ProbeSampleMode::Auto;
    cfg.target_arch = family != nullptr ? family->probe_target_arch(cfg.target_cfg) : std::nullopt;

    return {std::move(cfg), {}};
}

ProbeConfigResult resolve_probe_config(const Device& probe,
                                       const Family* family,
                                       const ProjectInfo* project,
                                       std::string_view env,
                                       std::string_view target_cfg_override) {
    return resolve_probe_config(ProbeConfigRequest{
        probe,
        family,
        project,
        std::string(env),
        std::string(target_cfg_override),
    });
}

} // namespace malgrind::host
