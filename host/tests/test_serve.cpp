// serve() self-check: the JSON-lines protocol the app talks, driven through in-memory pipes and a pty "board".
#include "pipe_stream.hpp"
#include "pty.hpp"
#include "serve.hpp"
#include "source.hpp"
#include "store.hpp"
#include "stream.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <thread>

using namespace malgrind::host;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

struct Harness {
    fs::path root;
    InPipe in_buf;
    OutPipe out_buf;
    std::istream in{&in_buf};
    std::ostream out{&out_buf};
    std::thread thread;
    int exit_code = -1;
    int next_id = 1;
    std::vector<json> events; // everything without an id that arrived while waiting for a reply

    explicit Harness(const char* name) : root(fs::temp_directory_path() / name) {
        fs::remove_all(root);
        fs::create_directories(root / "home");
        thread = std::thread([this] { exit_code = serve(in, out, {root / "runs", MALGRIND_SOURCE_DIR, root / "home", root / "backups", root / "projects"}); });
    }
    ~Harness() {
        finish();
        fs::remove_all(root);
    }
    void finish() {
        in_buf.close();
        if (thread.joinable()) thread.join();
    }
    json call(json request) {
        const int id = next_id++;
        request["id"] = id;
        in_buf.push(request.dump());
        for (;;) {
            const auto line = out_buf.next();
            assert(line && "no reply from serve");
            auto j = json::parse(*line);
            if (j.contains("id") && j["id"] == id) return j;
            events.push_back(std::move(j));
        }
    }
    void push_raw(const std::string& line) { in_buf.push(line); }
    json next_line() {
        const auto line = out_buf.next();
        assert(line);
        return json::parse(*line);
    }
    // The run list, as an array.
    json runs() { return call({{"cmd", "runs"}})["result"]; }
    // Snapshot value of the whole run (0 until the first frame).
    unsigned samples() {
        const auto r = call({{"cmd", "snapshot"}, {"window_s", 0}});
        assert(r["ok"]);
        return r["result"]["tree"].is_null() ? 0 : r["result"]["tree"]["value"].get<unsigned>();
    }
};

bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

void hello_and_runs() {
    Harness h("malgrind_serve_a");
    const auto hello = h.call({{"cmd", "hello"}});
    assert(hello["ok"] && hello["result"]["version"] == MALGRIND_VERSION && hello["result"]["data_dir"] == (h.root / "runs").string());
    assert(hello["result"].contains("pio")); // a path or null
    assert(h.runs() == json::array());
    assert(h.call({{"cmd", "ports"}})["result"].is_array());
}

void bad_input() {
    Harness h("malgrind_serve_b");
    h.push_raw("not json");
    auto bad = h.next_line();
    assert(bad["ok"] == false && bad["id"].is_null() && contains(bad["error"], "JSON"));
    h.push_raw(R"({"id":7,"cmd":"nope"})");
    auto unknown = h.next_line();
    assert(unknown["id"] == 7 && unknown["ok"] == false && contains(unknown["error"], "unknown command"));
    h.push_raw(R"({"id":8})");
    assert(h.next_line()["ok"] == false); // no cmd
    assert(h.call({{"cmd", "hello"}})["ok"]); // and the loop carries on
    assert(h.call({{"cmd", "open"}, {"run", "../x"}})["ok"] == false); // a run id is never a path
}

void attach_snapshot_stop() {
    Harness h("malgrind_serve_c");
    Pty pty;
    const auto attached = h.call({{"cmd", "attach"}, {"port", pty.slave}, {"name", "t"}});
    assert(attached["ok"] && attached["result"]["run"]["name"] == "t");
    const std::string id = attached["result"]["run"]["id"];
    assert(h.samples() == 0 && h.call({{"cmd", "functions"}, {"window_s", 0}})["result"]["functions"].empty());
    pty.send(make_stream(2)); // 8 samples
    assert(wait_for([&] { return h.samples() == 8; }));
    const auto fn = h.call({{"cmd", "functions"}, {"window_s", 0}, {"focus", "0x400d1fff"}})["result"];
    assert(fn["total"] == 8 && fn["callees"].size() == 2);
    assert(h.call({{"cmd", "snapshot"}, {"window_s", 0}, {"core", 1}})["result"]["tree"]["value"] == 2);

    auto runs = h.runs();
    assert(runs.size() == 1 && runs[0]["id"] == id && runs[0]["live"] == true);
    assert(h.call({{"cmd", "delete"}, {"run", id}})["ok"] == false); // the current run cannot be deleted
    assert(h.call({{"cmd", "stop"}})["ok"]);
    runs = h.runs();
    assert(runs[0]["duration_ms"].get<unsigned>() > 0 && !runs[0].contains("live") && runs[0]["ncores"] == 2);

    // history: open the stored run, the same picture comes back
    const auto opened = h.call({{"cmd", "open"}, {"run", id}});
    assert(opened["ok"] && h.samples() == 8);
    assert(h.call({{"cmd", "update"}, {"run", id}, {"name", "renamed"}, {"notes", "n"}})["result"]["name"] == "renamed");
    assert(h.call({{"cmd", "open"}, {"run", "missing"}})["ok"] == false);
    assert(h.call({{"cmd", "stop"}})["ok"] && h.samples() == 0); // closing the view
    assert(h.call({{"cmd", "delete"}, {"run", id}})["ok"] && h.runs().empty());
}

void busy_port() {
    Harness h("malgrind_serve_d");
    Pty pty;
    if (!exclusive_open_is_enforced()) return void(std::puts("busy_port: skipped, this platform does not enforce exclusive open on a pty"));
    auto other_program = Source::open(pty.slave, 921600); // somebody else has the port
    assert(other_program);
    const auto probe = h.call({{"cmd", "probe"}, {"port", pty.slave}});
    assert(probe["ok"] && probe["result"]["busy"] == true && probe["result"]["agent"] == false);
    const auto attach = h.call({{"cmd", "attach"}, {"port", pty.slave}, {"name", "x"}});
    assert(attach["ok"] == false && contains(attach["error"], "busy"));
    assert(h.runs().empty()); // a failed attach leaves no run behind
}

void probe_finds_agent() {
    Harness h("malgrind_serve_e");
    Pty pty;
    std::thread board([&] {
        std::this_thread::sleep_for(300ms);
        pty.send(make_stream(1));
    });
    const auto probe = h.call({{"cmd", "probe"}, {"port", pty.slave}});
    board.join();
    assert(probe["ok"] && probe["result"]["busy"] == false && probe["result"]["agent"] == true);
    assert(probe["result"]["arch"] == "xtensa" && probe["result"]["ncores"] == 2);
    assert(h.runs().empty()); // probing records nothing
}

const json& method(const json& result, const char* id) {
    for (const auto& m : result["methods"])
        if (m["id"] == id) return m;
    assert(false && "method missing");
    static const json none;
    return none;
}

void probe_lists_methods() {
    Harness h("malgrind_serve_m");
    Pty pty;
    std::thread board([&] {
        std::this_thread::sleep_for(300ms);
        pty.send(make_stream(1));
    });
    const auto with_agent = h.call({{"cmd", "probe"}, {"port", pty.slave}});
    board.join();
    assert(method(with_agent["result"], "attach")["available"] == true);
    assert(method(with_agent["result"], "no_flash")["available"] == false);
    assert(contains(method(with_agent["result"], "no_flash")["why"], "debug probe"));

    // a project that cannot take the agent is named as the reason new_run is unavailable (an Arduino project here)
    const auto proj = h.root / "arduino";
    fs::create_directories(h.root / "home/.platformio/penv/bin");
    std::ofstream(h.root / "home/.platformio/penv/bin/pio") << "#!/bin/sh\n";
    fs::permissions(h.root / "home/.platformio/penv/bin/pio", fs::perms::owner_exec, fs::perm_options::add);
    fs::create_directories(proj);
    std::ofstream(proj / "platformio.ini") << "[env:a]\nplatform = espressif32\nframework = arduino\n";
    setenv("PATH", "/usr/bin:/bin", 1); // the fake pio, not the user's
    Pty quiet; // a pty stays exclusive after the first probe closed it
    const auto silent = h.call({{"cmd", "probe"}, {"port", quiet.slave}, {"project", proj.string()}});
    assert(silent["ok"] && silent["result"]["agent"] == false);
    assert(method(silent["result"], "attach")["available"] == false);
    assert(method(silent["result"], "new_run")["available"] == false && contains(method(silent["result"], "new_run")["why"], "arduino"));
}

void remove_agent_restores_or_says_why_not() {
    Harness h("malgrind_serve_n");
    const auto bad = h.call({{"cmd", "remove_agent"}, {"port", "/dev/null"}, {"how", "burn"}});
    assert(bad["ok"] == false && contains(bad["error"], "restore"));
    // the default is a restore: without PlatformIO (and so without esptool) the reason is given, not a rebuild
    setenv("PATH", "/usr/bin:/bin", 1);
    const auto none = h.call({{"cmd", "remove_agent"}, {"port", "/dev/null"}});
    assert(none["ok"] == false && contains(none["error"], "PlatformIO"));
    assert(h.call({{"cmd", "remove_agent"}, {"port", "/dev/null"}, {"how", "rebuild"}})["ok"] == false); // needs dir, env
}

// A fake pio in the harness's home, so "no PlatformIO" does not hide the project's own reason.
void fake_pio(Harness& h) {
    const auto pio = h.root / "home/.platformio/penv/bin/pio";
    fs::create_directories(pio.parent_path());
    std::ofstream(pio) << "#!/bin/sh\n";
    fs::permissions(pio, fs::perms::owner_exec, fs::perm_options::add);
    setenv("PATH", "/usr/bin:/bin", 1); // not the user's pio
}

json add_project(Harness& h, json args) {
    args["cmd"] = "project_add";
    return h.call(std::move(args));
}

void projects_of_every_kind() {
    Harness h("malgrind_serve_p");
    fake_pio(h);
    const auto mine = h.root / "my-firmware";
    fs::create_directories(mine);
    std::ofstream(mine / "platformio.ini") << "[env:a]\n[env:b]\n";

    // a folder of the user's own: trusted at once, named after the folder, its environments listed
    const auto folder = add_project(h, {{"source", "folder"}, {"dir", mine.string()}});
    assert(folder["ok"] && folder["result"]["source"] == "folder" && folder["result"]["trusted"] == true);
    assert(folder["result"]["name"] == "my-firmware" && folder["result"]["envs"] == json::array({"a", "b"}) && folder["result"]["problem"] == "");
    assert(add_project(h, {{"source", "folder"}, {"dir", (h.root / "nothing").string()}})["ok"] == false);

    // no code: only a name; a file that is not an ELF is refused
    const auto none = add_project(h, {{"source", "none"}, {"name", "Water simulator"}});
    assert(none["ok"] && none["result"]["has_source"] == false && none["result"]["trusted"] == false && none["result"]["envs"].empty());
    std::ofstream(h.root / "not.elf") << "text";
    const auto bad_elf = add_project(h, {{"source", "none"}, {"elf", (h.root / "not.elf").string()}});
    assert(bad_elf["ok"] == false && contains(bad_elf["error"], "ELF"));

    // links: only github https, nothing that git would run
    for (const char* url : {"file:///etc", "ext::sh -c id", "https://example.com/o/r", "-oProxyCommand=x"})
        assert(add_project(h, {{"source", "git"}, {"url", url}})["ok"] == false);

    // nothing can be built without source code, or before a cloned project is trusted
    const std::string none_id = none["result"]["id"];
    const auto no_source = h.call({{"cmd", "new_run"}, {"project", none_id}, {"port", "/dev/null"}, {"name", "x"}});
    assert(no_source["ok"] == false && contains(no_source["error"], "no source code"));
    const auto probe = h.call({{"cmd", "probe"}, {"port", "/dev/null"}, {"project", none_id}});
    assert(probe["ok"] == false); // /dev/null is no serial port: the probe says so before it says anything else
    assert(h.call({{"cmd", "new_run"}, {"project", "nope"}, {"port", "/dev/null"}, {"name", "x"}})["ok"] == false);

    // rename, environment, trust; a wrong environment is refused
    const std::string id = folder["result"]["id"];
    assert(h.call({{"cmd", "project_update"}, {"project", id}, {"name", "renamed"}, {"env", "b"}})["result"]["env"] == "b");
    assert(h.call({{"cmd", "project_update"}, {"project", id}, {"env", "zzz"}})["ok"] == false);

    // removing a project never deletes the user's folder
    assert(h.call({{"cmd", "project_remove"}, {"project", id}})["ok"] && fs::exists(mine / "platformio.ini"));
    assert(h.call({{"cmd", "project_remove"}, {"project", id}})["ok"] == false);
    assert(h.call({{"cmd", "projects"}})["result"].size() == 1);
}

void a_cloned_project_must_be_trusted() {
    Harness h("malgrind_serve_q");
    fake_pio(h);
    const auto bin = h.root / "bin";
    fs::create_directories(bin);
    std::ofstream(bin / "git") << "#!/bin/sh\nfor last; do :; done\nmkdir -p \"$last/firmware/x\" && printf '[env:a]\\n' > \"$last/firmware/x/platformio.ini\"\n";
    fs::permissions(bin / "git", fs::perms::owner_exec | fs::perms::owner_read, fs::perm_options::add);
    setenv("PATH", (bin.string() + ":/usr/bin:/bin").c_str(), 1);

    const auto added = add_project(h, {{"source", "git"}, {"url", "https://github.com/o/repo"}});
    assert(added["ok"] && added["result"]["name"] == "repo" && added["result"]["trusted"] == false && added["result"]["envs"] == json::array({"a"}));
    assert(contains(added["result"]["dir"], "firmware/x") && added["result"]["url"] == "https://github.com/o/repo.git");
    const std::string id = added["result"]["id"];

    // untrusted: it is not built, and the probe says why
    const auto refused = h.call({{"cmd", "new_run"}, {"project", id}, {"port", "/dev/null"}, {"name", "x"}});
    assert(refused["ok"] == false && contains(refused["error"], "not been trusted"));
    Pty pty;
    const auto probe = h.call({{"cmd", "probe"}, {"port", pty.slave}, {"project", id}});
    assert(probe["ok"] && method(probe["result"], "new_run")["available"] == false && contains(method(probe["result"], "new_run")["why"], "trusted"));

    // after the user's yes the same project is buildable as far as the checks go
    assert(h.call({{"cmd", "project_update"}, {"project", id}, {"trusted", true}})["result"]["trusted"] == true);
    Pty pty2;
    const auto after = h.call({{"cmd", "probe"}, {"port", pty2.slave}, {"project", id}});
    assert(method(after["result"], "new_run")["available"] == true);

    // a failed clone leaves no project behind
    std::ofstream(bin / "git") << "#!/bin/sh\nexit 128\n";
    const auto failed = add_project(h, {{"source", "git"}, {"url", "https://github.com/o/other"}});
    assert(failed["ok"] == false && h.call({{"cmd", "projects"}})["result"].size() == 1);
}

void runs_belong_to_projects() {
    Harness h("malgrind_serve_r");
    Pty pty;
    const auto none = add_project(h, {{"source", "none"}, {"name", "Water simulator"}});
    const std::string id = none["result"]["id"];
    assert(h.call({{"cmd", "projects"}})["result"][0]["run_count"] == 0 && h.call({{"cmd", "projects"}})["result"][0]["last_run"].is_null());
    const auto run = h.call({{"cmd", "attach"}, {"port", pty.slave}, {"name", "a"}, {"project", id}});
    assert(run["ok"] && run["result"]["run"]["project_id"] == id);
    assert(h.call({{"cmd", "attach"}, {"port", "/dev/null"}, {"name", "b"}, {"project", "missing"}})["ok"] == false);
    h.call({{"cmd", "stop"}});
    const auto list = h.call({{"cmd", "projects"}})["result"];
    assert(list[0]["run_count"] == 1 && list[0]["last_run"].is_string() && h.runs()[0]["project_id"] == id);
}

void boards_lists_ports_and_probes() {
    Harness h("malgrind_serve_s");
    const auto boards = h.call({{"cmd", "boards"}});
    assert(boards["ok"] && boards["result"]["ports"].is_array() && boards["result"]["probes"].is_array());
    for (const auto& p : boards["result"]["ports"]) assert(p.contains("port") && p.contains("adapter") && p.contains("vid"));
}

void open_while_live_refused() {
    Harness h("malgrind_serve_f");
    Pty pty;
    const auto id = h.call({{"cmd", "attach"}, {"port", pty.slave}, {"name", "a"}})["result"]["run"]["id"];
    const auto second = h.call({{"cmd", "attach"}, {"port", pty.slave}, {"name", "b"}});
    assert(second["ok"] == false);
    const auto open = h.call({{"cmd", "open"}, {"run", id}});
    assert(open["ok"] == false && contains(open["error"], "stop the live run first"));
}

void eof_finishes_run() {
    {
        Harness h("malgrind_serve_g");
        Pty pty;
        h.call({{"cmd", "attach"}, {"port", pty.slave}, {"name", "a"}});
        pty.send(make_stream(1));
        assert(wait_for([&] { return h.samples() == 4; }));
        h.in_buf.close(); // the app window closed
        h.thread.join();
        assert(h.exit_code == 0);
        RunStore store(h.root / "runs");
        assert(store.list().size() == 1 && store.list()[0].duration_ms > 0);
    }
}

} // namespace

int main() {
    hello_and_runs();
    bad_input();
    attach_snapshot_stop();
    busy_port();
    probe_finds_agent();
    probe_lists_methods();
    projects_of_every_kind();
    a_cloned_project_must_be_trusted();
    runs_belong_to_projects();
    boards_lists_ports_and_probes();
    remove_agent_restores_or_says_why_not();
    open_while_live_refused();
    eof_finishes_run();
    std::puts("serve ok");
}
