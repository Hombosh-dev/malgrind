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
        thread = std::thread([this] { exit_code = serve(in, out, {root / "runs", MALGRIND_SOURCE_DIR, root / "home"}); });
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
    open_while_live_refused();
    eof_finishes_run();
    std::puts("serve ok");
}
