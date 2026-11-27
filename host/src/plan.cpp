#include "plan.hpp"

#include "discover.hpp"
#include "os/paths.hpp"
#include "os/process.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <stdexcept>
#include <utility>

namespace fs = std::filesystem;

namespace malgrind::host {

namespace {

bool safe_word_char(unsigned char c) { return std::isalnum(c) || std::string_view("_@%+=:,./-").find(static_cast<char>(c)) != std::string_view::npos; }

// Replaces the command line of `inv` by the one the user typed. The environment (the agent's) is left alone.
void override_command(Invocation& inv, const std::string& line, const char* what) {
    if (line.empty()) return;
    auto words = split_command(line);
    if (!words || words->empty()) throw std::runtime_error(std::format("{}: this is not a command line (is a quote left open?)", what));
    const auto& program = words->front();
    const bool has_sep = program.find_first_of("/\\") != std::string::npos;
    const bool found = has_sep ? fs::exists(os::from_utf8(program)) : os::find_in_path(program).has_value();
    if (!found) throw std::runtime_error(std::format("{}: '{}' was not found", what, program));
    inv.argv = std::move(*words);
}

const Family* resolve_family(const Prepared& p, const std::string& env) {
    if (p.family) return p.family;
    std::string why;
    const Family* family = family_for_project(p.project, env, why);
    if (!family) throw std::runtime_error(why);
    return family;
}

} // namespace

std::optional<std::vector<std::string>> split_command(std::string_view line) {
    enum class Mode { Plain, Single, Double } mode = Mode::Plain;
    std::vector<std::string> words;
    std::string current;
    bool have_word = false; // an empty pair of quotes is a word too
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        switch (mode) {
            case Mode::Plain:
                if (std::isspace(static_cast<unsigned char>(c))) {
                    if (have_word) words.push_back(std::exchange(current, {})), have_word = false;
                } else if (c == '\'') {
                    mode = Mode::Single, have_word = true;
                } else if (c == '"') {
                    mode = Mode::Double, have_word = true;
                } else if (c == '\\') {
                    if (i + 1 >= line.size()) return std::nullopt;
                    current += line[++i], have_word = true;
                } else {
                    current += c, have_word = true;
                }
                break;
            case Mode::Single:
                if (c == '\'') mode = Mode::Plain;
                else current += c;
                break;
            case Mode::Double:
                if (c == '"') mode = Mode::Plain;
                else if (c == '\\' && i + 1 < line.size() && (line[i + 1] == '"' || line[i + 1] == '\\')) current += line[++i];
                else current += c;
                break;
        }
    }
    if (mode != Mode::Plain) return std::nullopt;
    if (have_word) words.push_back(std::move(current));
    return words;
}

std::string join_command(const std::vector<std::string>& words) {
    std::string out;
    for (const auto& word : words) {
        if (!out.empty()) out += ' ';
        if (!word.empty() && std::ranges::all_of(word, [](unsigned char c) { return safe_word_char(c); })) {
            out += word;
            continue;
        }
        out += '\'';
        for (const char c : word) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
        out += '\'';
    }
    return out;
}

Plan make_plan(const Prepared& p, const fs::path& agent_dir, const RunSettings& s) {
    const std::string env = s.env.empty() ? p.env : s.env;
    if (std::ranges::find(p.project.envs, env) == p.project.envs.end())
        throw std::runtime_error(std::format("platformio.ini has no environment '{}'", env));
    if (s.rate_hz != 0 && (s.rate_hz < kMinRateHz || s.rate_hz > kMaxRateHz))
        throw std::runtime_error(std::format("the sampling rate must be between {} and {} Hz", kMinRateHz, kMaxRateHz));
    if (!s.elf.empty() && !fs::exists(s.elf)) throw std::runtime_error("the firmware ELF does not exist: " + os::to_utf8(s.elf));
    std::string target_err;
    if (!sanitize_target_cfg(s.target_cfg, target_err)) throw std::runtime_error(target_err);
    const auto extra = split_command(s.extra_args);
    if (!extra) throw std::runtime_error("extra arguments: a quote is left open");

    const Family* family = resolve_family(p, env);
    auto cmds = family->agent_commands(ToolContext{{}, p.pio, agent_dir}, p.project, env, p.port, s, false);
    Plan plan{std::move(cmds.build), std::move(cmds.upload), std::move(cmds.elf), {}, {}, env, s.rate_hz, s.check_memory};
    for (auto* inv : {&plan.build, &plan.upload}) {
        inv->argv.insert(inv->argv.end(), extra->begin(), extra->end());
        if (!s.build_flags.empty()) inv->env.push_back("PLATFORMIO_BUILD_FLAGS=" + s.build_flags);
    }
    override_command(plan.build, s.build_command, "build command");
    override_command(plan.upload, s.upload_command, "upload command");
    plan.build_text = join_command(plan.build.argv);
    plan.upload_text = join_command(plan.upload.argv);
    return plan;
}

void write_plan_files(const Prepared& p, const fs::path& agent_dir, const Plan& plan) {
    const Family* family = resolve_family(p, plan.env);
    RunSettings s;
    s.env = plan.env;
    s.rate_hz = plan.rate_hz;
    s.check_memory = plan.check_memory;
    (void)family->agent_commands(ToolContext{{}, p.pio, agent_dir}, p.project, plan.env, p.port, s, true);
}

fs::path find_probe_elf(const ProjectInfo& project, std::string_view env, std::optional<Arch> arch) {
    auto matches_elf = [&](const fs::path& candidate) {
        std::error_code ec;
        if (!fs::is_regular_file(candidate, ec)) return false;
        if (arch) return elf_arch(candidate) == *arch;
        return elf_machine(candidate).has_value();
    };
    if (!env.empty()) {
        for (const fs::path& cand : {project.dir / ".pio/build" / env / "firmware.elf",
                                     project.dir / ".pio/malgrind/build" / env / "firmware.elf"}) {
            if (matches_elf(cand)) return cand;
        }
    }

    std::vector<fs::path> plain_candidates;
    std::vector<fs::path> agent_candidates;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(project.dir / ".pio/build", ec)) {
        plain_candidates.push_back(entry.path() / "firmware.elf");
    }
    for (const auto& entry : fs::directory_iterator(project.dir / "build", ec)) {
        if (entry.path().extension() == ".elf") plain_candidates.push_back(entry.path());
    }
    for (const auto& entry : fs::directory_iterator(project.dir / ".pio/malgrind/build", ec)) {
        agent_candidates.push_back(entry.path() / "firmware.elf");
    }

    auto newest = [&](const std::vector<fs::path>& list) -> std::optional<fs::path> {
        std::optional<fs::path> best;
        fs::file_time_type best_time;
        for (const auto& c : list) {
            if (!matches_elf(c)) continue;
            const auto t = fs::last_write_time(c, ec);
            if (!best || t > best_time) {
                best = c;
                best_time = t;
            }
        }
        return best;
    };

    if (const auto plain = newest(plain_candidates)) return *plain;
    if (const auto agent = newest(agent_candidates)) return *agent;
    return {};
}

ProbePlan make_probe_plan(const ProbePlanRequest& req) {
    std::string env;
    if (req.project) {
        env = req.settings.env.empty() ? req.project->default_env : req.settings.env;
        if (!env.empty() && !req.project->envs.empty() &&
            std::ranges::find(req.project->envs, env) == req.project->envs.end()) {
            throw std::runtime_error(std::format("platformio.ini has no environment '{}'", env));
        }
    }
    if (req.settings.rate_hz != 0 && (req.settings.rate_hz < kMinRateHz || req.settings.rate_hz > kMaxRateHz)) {
        throw std::runtime_error(std::format("the sampling rate must be between {} and {} Hz", kMinRateHz, kMaxRateHz));
    }
    if (!req.settings.elf.empty() && !fs::exists(req.settings.elf)) {
        throw std::runtime_error("the firmware ELF does not exist: " + os::to_utf8(req.settings.elf));
    }

    const auto insp = inspect_probe(req.home,
                                    req.probe,
                                    nullptr,
                                    req.project ? &*req.project : nullptr,
                                    env,
                                    req.settings.target_cfg,
                                    req.assume_openocd_available);
    if (!insp.config.config) throw std::runtime_error(insp.config.error);
    if (!insp.openocd_available) throw std::runtime_error(insp.error);

    ProbePlan plan;
    plan.config = *insp.config.config;
    plan.openocd = insp.openocd;
    plan.env = std::move(env);
    plan.sample_mode = insp.sample_mode;
    plan.rate_hint = insp.rate_hint;
    plan.targets = insp.targets;

    plan.openocd_invocation.argv.push_back(plan.openocd ? os::to_utf8(plan.openocd->exe) : std::string("openocd"));
    if (plan.openocd && !plan.openocd->scripts.empty()) {
        plan.openocd_invocation.argv.push_back("-s");
        plan.openocd_invocation.argv.push_back(os::to_utf8(plan.openocd->scripts));
    }
    plan.openocd_invocation.argv.insert(plan.openocd_invocation.argv.end(),
                                        plan.config.openocd_args.begin(),
                                        plan.config.openocd_args.end());
    plan.command_text = join_command(plan.openocd_invocation.argv);

    if (!req.settings.elf.empty()) {
        plan.elf = req.settings.elf;
    } else if (!req.project_elf.empty() && fs::exists(req.project_elf)) {
        plan.elf = req.project_elf;
    } else if (req.project) {
        plan.elf = find_probe_elf(*req.project, plan.env, plan.config.target_arch);
    }

    plan.note = "No firmware will be modified or flashed. Profiles flat PCs via debug probe.";
    const std::string ocd_where = plan.openocd ? os::to_utf8(plan.openocd->exe) : "custom probe hook";
    const std::string sym_desc = plan.elf.empty() ? "none (raw addresses)" : os::to_utf8(plan.elf);
    plan.summary = std::format("OpenOCD: {} ({}) · Interface: {} · Target: {} · Symbols: {} · {}",
                               plan.config.package,
                               ocd_where,
                               plan.config.interface_desc,
                               plan.config.target_cfg,
                               sym_desc,
                               plan.note);
    return plan;
}

} // namespace malgrind::host
