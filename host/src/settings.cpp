#include "settings.hpp"

#include "os/paths.hpp"

#include <nlohmann/json.hpp>

namespace malgrind::host {

namespace {

bool has_control_char(std::string_view s) {
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7f) return true;
    }
    return false;
}

std::string trim_spaces(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    return std::string(s);
}

} // namespace

std::optional<std::string> sanitize_target_cfg(std::string_view raw, std::string& error) {
    error.clear();
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

void to_json(nlohmann::json& j, const RunSettings& s) {
    j = {{"env", s.env},
         {"extra_args", s.extra_args},
         {"build_flags", s.build_flags},
         {"rate_hz", s.rate_hz},
         {"check_memory", s.check_memory},
         {"elf", os::to_utf8(s.elf)},
         {"build_command", s.build_command},
         {"upload_command", s.upload_command},
         {"target_cfg", s.target_cfg}};
}

void from_json(const nlohmann::json& j, RunSettings& s) {
    s = {};
    if (!j.is_object()) return;
    auto text = [&](const char* key) { return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string{}; };
    s.env = text("env");
    s.extra_args = text("extra_args");
    s.build_flags = text("build_flags");
    s.elf = os::from_utf8(text("elf"));
    s.build_command = text("build_command");
    s.upload_command = text("upload_command");
    s.target_cfg = text("target_cfg");
    if (j.contains("rate_hz") && j["rate_hz"].is_number_unsigned()) s.rate_hz = j["rate_hz"].get<unsigned>();
    if (j.contains("check_memory") && j["check_memory"].is_boolean()) s.check_memory = j["check_memory"].get<bool>();
}

} // namespace malgrind::host
