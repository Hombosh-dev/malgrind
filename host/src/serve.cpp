#include "serve.hpp"

#include "ops.hpp"
#include "pio.hpp"
#include "session.hpp"
#include "source.hpp"
#include "store.hpp"
#include "wire.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;

namespace malgrind::host {

namespace {

using json = nlohmann::json;
using namespace std::chrono_literals;

constexpr const char* kVersion = "0.1.0";
std::string str_arg(const json& req, const char* key) {
    if (!req.contains(key) || !req[key].is_string()) throw std::runtime_error(std::string("missing argument: ") + key);
    return req[key].get<std::string>();
}

std::optional<std::string> opt_str_arg(const json& req, const char* key) {
    if (!req.contains(key) || !req[key].is_string()) return std::nullopt;
    return req[key].get<std::string>();
}

class Server {
public:
    Server(std::ostream& out, ServeOptions options) : out_(out), options_(std::move(options)), store_(options_.data_dir) {}

    int run(std::istream& in) {
        for (std::string line; std::getline(in, line);)
            if (!line.empty()) handle_line(line);
        // The app is gone. A build that is still running is waited for first: it may start a run when it finishes,
        // and that run must be closed too. ponytail: kill pio's process group if waiting proves too slow.
        if (worker_.joinable()) worker_.join();
        set_session(nullptr); // closes the live run so it keeps its length
        return 0;
    }

private:
    // ---- output: one line per object, never interleaved ---------------------------------------------------------
    void send(const json& j) {
        std::scoped_lock lock(out_mutex_);
        // the board's console text is not always UTF-8 (boot ROM at another baud rate): replace, never throw
        out_ << j.dump(-1, ' ', false, json::error_handler_t::replace) << '\n' << std::flush;
    }
    void reply(const json& id, json result) { send({{"id", id}, {"ok", true}, {"result", std::move(result)}}); }
    void reply_raw(const json& id, const std::string& result) { // result is JSON text already: no parse and dump of a big tree
        std::scoped_lock lock(out_mutex_);
        out_ << R"({"id":)" << id.dump() << R"(,"ok":true,"result":)" << result << "}\n" << std::flush;
    }
    void fail(const json& id, const std::string& message) { send({{"id", id}, {"ok", false}, {"error", message}}); }
    Emit emitter() { return [this](const json& event) { send(event); }; }

    // ---- the session being shown (live, ended, or opened from history) ------------------------------------------
    std::shared_ptr<Session> current() {
        std::scoped_lock lock(session_mutex_);
        return session_;
    }
    void set_session(std::shared_ptr<Session> next) {
        {
            std::scoped_lock lock(session_mutex_);
            std::swap(session_, next);
        }
        // `next` (the old session) is released here, outside the lock: closing a live run waits for its reader thread
    }
    bool live() {
        const auto s = current();
        return s && s->live();
    }

    // ---- requests ----------------------------------------------------------------------------------------------------
    void handle_line(const std::string& line) {
        const auto req = json::parse(line, nullptr, false);
        if (!req.is_object()) {
            send({{"id", nullptr}, {"ok", false}, {"error", "malformed JSON: expected one JSON object per line"}});
            return;
        }
        const json id = req.contains("id") ? req["id"] : json(nullptr);
        try {
            const auto cmd = str_arg(req, "cmd");
            if (cmd == "snapshot" || cmd == "functions") return reply_raw(id, query(cmd, req));
            if (cmd == "new_run" || cmd == "remove_agent" || cmd == "chip_id") return run_long(id, cmd, req);
            reply(id, command(cmd, req));
        } catch (const std::exception& e) {
            fail(id, e.what());
        }
    }

    json command(const std::string& cmd, const json& req) {
        if (cmd == "hello") {
            const auto pio = find_pio(options_.home);
            return {{"version", kVersion}, {"data_dir", options_.data_dir.string()}, {"pio", pio ? json(pio->string()) : json(nullptr)}};
        }
        if (cmd == "ports") return json(find_serial_ports());
        if (cmd == "probe") return probe(str_arg(req, "port"));
        if (cmd == "project") {
            std::string why;
            const auto project = read_project(str_arg(req, "dir"), why);
            if (!project) throw std::runtime_error(why);
            return {{"dir", project->dir.string()}, {"envs", project->envs}, {"default_env", project->default_env}};
        }
        if (cmd == "attach") return attach(req);
        if (cmd == "stop") {
            set_session(nullptr);
            return json::object();
        }
        if (cmd == "runs") {
            const auto cur = current();
            auto runs = json::array();
            for (const auto& meta : store_.list()) {
                json run = meta;
                if (cur && cur->live() && cur->run_id() == meta.id) run["live"] = true;
                runs.push_back(std::move(run));
            }
            return runs;
        }
        if (cmd == "open") {
            if (live()) throw std::runtime_error("stop the live run first");
            const auto id = str_arg(req, "run");
            auto session = Session::replay(store_, id, emitter());
            if (!session) throw std::runtime_error("no such run: " + id);
            set_session(std::move(session));
            return {{"run", *store_.get(id)}};
        }
        if (cmd == "update") {
            const auto id = str_arg(req, "run");
            const auto meta = store_.update(id, opt_str_arg(req, "name"), opt_str_arg(req, "notes"));
            if (!meta) throw std::runtime_error("no such run: " + id);
            return *meta;
        }
        if (cmd == "delete") {
            const auto id = str_arg(req, "run");
            if (const auto cur = current(); cur && cur->run_id() == id) throw std::runtime_error("this run is open: close it first");
            if (!store_.remove(id)) throw std::runtime_error("no such run: " + id);
            return json::object();
        }
        throw std::runtime_error("unknown command: " + cmd);
    }

    // snapshot and functions answer with the aggregator's JSON text as it is
    std::string query(const std::string& cmd, const json& req) {
        std::optional<unsigned> core;
        if (req.contains("core") && req["core"].is_number_unsigned()) core = req["core"].get<unsigned>();
        const Aggregator::Ms window = std::chrono::seconds(req.value("window_s", 10));
        std::optional<Aggregator::Ms> at;
        if (req.contains("at_ms") && req["at_ms"].is_number()) at = Aggregator::Ms(req["at_ms"].get<long long>());
        const auto session = current();
        if (cmd == "snapshot") {
            auto result = session ? session->snapshot(core, window, at) : std::nullopt;
            return result ? *result : R"({"tree":null})";
        }
        auto result = session ? session->functions(core, window, at, req.value("focus", "")) : std::nullopt;
        return result ? *result : R"({"functions":[]})";
    }

    json probe(const std::string& port) {
        const auto r = probe_port(port);
        json result = {{"busy", r.busy}, {"agent", r.agent}};
        if (r.agent) {
            result["arch"] = std::string(arch_name(r.arch));
            result["ncores"] = r.ncores;
            result["cpu_mhz"] = r.cpu_mhz;
        }
        return result;
    }

    json start_live(Source source, RunMeta meta, fs::path elf, fs::path project) {
        auto writer = store_.create(std::move(meta));
        json run = writer.meta();
        set_session(Session::live(std::move(source), std::move(writer), std::move(elf), std::move(project), emitter()));
        return {{"run", std::move(run)}};
    }

    json attach(const json& req) {
        if (busy_) throw std::runtime_error("busy: a build or flash is running");
        if (live()) throw std::runtime_error("a run is live: stop it first");
        const auto port = str_arg(req, "port");
        const auto project = opt_str_arg(req, "project").value_or("");
        auto source = open_source(port);
        return start_live(std::move(source), {.name = str_arg(req, "name"), .project = project, .port = port},
                          opt_str_arg(req, "elf").value_or(""), project);
    }

    // ---- slow commands, one at a time, on a worker thread ------------------------------------------------------------
    void run_long(const json& id, const std::string& cmd, const json& req) {
        if (busy_.exchange(true)) throw std::runtime_error("busy: another build or flash is running");
        if (worker_.joinable()) worker_.join(); // the previous one has finished (it cleared busy_)
        worker_ = std::thread([this, id, cmd, req] {
            try {
                reply(id, cmd == "new_run" ? new_run(req) : cmd == "remove_agent" ? remove_agent(req) : chip_id_command(req));
            } catch (const std::exception& e) {
                fail(id, e.what());
            }
            busy_ = false;
        });
    }

    Prepared prepare(const json& req) {
        return prepare_project(options_.home, str_arg(req, "dir"), str_arg(req, "env"), str_arg(req, "port"));
    }
    void run_pio_events(const Invocation& inv) {
        run_pio(inv, [this](std::string_view line) { send({{"event", "build"}, {"line", std::string(line)}}); });
    }

    json new_run(const json& req) {
        const auto p = prepare(req);
        if (live()) throw std::runtime_error("stop the live run first");
        run_pio_events(agent_build(p.pio, p.project, p.env, p.port, options_.agent_dir));
        auto source = open_after_upload(p.port);
        return start_live(std::move(source), {.name = str_arg(req, "name"), .project = p.project.dir.string(), .env = p.env, .port = p.port},
                          agent_elf(p.project, p.env), p.project.dir);
    }

    json remove_agent(const json& req) {
        const auto p = prepare(req);
        set_session(nullptr); // the port must be free for the upload
        run_pio_events(plain_flash(p.pio, p.project, p.env, p.port));
        return json::object();
    }

    json chip_id_command(const json& req) {
        if (live()) throw std::runtime_error("stop the live run first");
        const auto pio = find_pio(options_.home);
        if (!pio) throw std::runtime_error(kNoPio);
        const auto chip = chip_id(*pio, str_arg(req, "port"), options_.home);
        if (!chip) throw std::runtime_error("esptool could not identify the chip");
        return {{"chip", *chip}};
    }

    std::ostream& out_;
    std::mutex out_mutex_;
    ServeOptions options_;
    RunStore store_;
    std::mutex session_mutex_;
    std::shared_ptr<Session> session_;
    std::atomic<bool> busy_{false};
    std::thread worker_; // last: joined before the rest goes away
};

} // namespace

int serve(std::istream& in, std::ostream& out, const ServeOptions& options) { return Server(out, options).run(in); }

} // namespace malgrind::host
