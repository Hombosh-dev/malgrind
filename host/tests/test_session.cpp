// Session self-check: a live run on a pty (what a serial port looks like), its recording and its replay.
#include "pty.hpp"
#include "session.hpp"
#include "store.hpp"
#include "stream.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <variant>

using namespace malgrind;
using namespace malgrind::host;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

struct Events {
    std::mutex mutex;
    std::vector<json> list;
    Emit sink() { return [this](const json& e) { std::scoped_lock lock(mutex); list.push_back(e); }; }
    [[nodiscard]] std::vector<json> named(const char* name) {
        std::scoped_lock lock(mutex);
        std::vector<json> out;
        for (const auto& e : list) if (e["event"] == name) out.push_back(e);
        return out;
    }
    [[nodiscard]] std::string logs() {
        std::string all;
        for (const auto& e : named("log")) all += e["text"].get<std::string>();
        return all;
    }
};

bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

fs::path fresh(const char* name) {
    const auto p = fs::temp_directory_path() / name;
    fs::remove_all(p);
    return p;
}

constexpr auto kAll = Aggregator::Ms{0};

// 2 rounds = 4 frames: core 0 sends 3 samples per round, core 1 one, so 8 samples in all.
std::unique_ptr<Session> start(RunStore& store, Pty& pty, Events& ev, const fs::path& project) {
    auto source = SerialSource::open_path(pty.slave, 921600);
    assert(std::holds_alternative<std::unique_ptr<SerialSource>>(source));
    return Session::live(std::get<std::unique_ptr<SerialSource>>(std::move(source)), store.create({.name = "t"}), {},
                         project, ev.sink());
}

void live_records_and_aggregates() {
    const auto root = fresh("malgrind_session_a");
    RunStore store(root / "runs");
    Pty pty;
    Events ev;
    auto session = start(store, pty, ev, root / "project");
    assert(session->live() && !session->snapshot(std::nullopt, kAll, std::nullopt)); // nothing yet
    pty.send(make_stream(2));
    assert(wait_for([&] { auto s = session->snapshot(std::nullopt, kAll, std::nullopt); return s && contains(*s, R"("name":"all","value":8)"); }));
    const auto targets = ev.named("target");
    assert(targets.size() == 1 && targets[0]["ncores"] == 2 && targets[0]["arch"] == "xtensa");
    assert(session->snapshot(1u, kAll, std::nullopt)->find(R"("name":"all","value":2)") != std::string::npos);
    session.reset();
    fs::remove_all(root);
}

void ended_on_eof() {
    const auto root = fresh("malgrind_session_b");
    RunStore store(root / "runs");
    Pty pty;
    Events ev;
    auto session = start(store, pty, ev, root / "project");
    pty.send(make_stream(1));
    assert(wait_for([&] { return session->snapshot(std::nullopt, kAll, std::nullopt).has_value(); }));
    pty.hang_up(); // the board is unplugged
    assert(wait_for([&] { return !ev.named("ended").empty(); }, 1000ms));
    assert(!session->live() && ev.named("ended")[0]["run"] == session->run_id());
    session.reset();
    assert(store.list().size() == 1 && store.list()[0].duration_ms > 0 && store.list()[0].ncores == 2);
    fs::remove_all(root);
}

void no_elf_raw_addresses() {
    const auto root = fresh("malgrind_session_c");
    fs::create_directories(root / "project");
    RunStore store(root / "runs");
    Pty pty;
    Events ev;
    auto session = start(store, pty, ev, root / "project");
    pty.send(make_stream(1));
    assert(wait_for([&] { return session->snapshot(std::nullopt, kAll, std::nullopt).has_value(); }));
    assert(contains(*session->snapshot(std::nullopt, kAll, std::nullopt), R"("name":"0x400d2fff")"));
    const auto notices = ev.named("notice");
    assert(!notices.empty() && contains(notices[0]["text"], "ELF"));
    assert(!store.list()[0].has_elf);
    session.reset();
    fs::remove_all(root);
}

void replay_matches_live() {
    const auto root = fresh("malgrind_session_d");
    RunStore store(root / "runs");
    std::string live_view, id;
    {
        Pty pty;
        Events ev;
        auto session = start(store, pty, ev, root / "project");
        pty.send(make_stream(3));
        assert(wait_for([&] { auto s = session->snapshot(std::nullopt, kAll, std::nullopt); return s && contains(*s, R"("name":"all","value":12)"); }));
        live_view = *session->snapshot(std::nullopt, kAll, std::nullopt);
        id = session->run_id();
    }
    Events ev;
    const auto replay = Session::replay(store, id, ev.sink());
    assert(replay && !replay->live());
    assert(replay->snapshot(std::nullopt, kAll, std::nullopt) == live_view);
    assert(!Session::replay(store, "missing", ev.sink()));
    fs::remove_all(root);
}

void log_passthrough() {
    const auto root = fresh("malgrind_session_e");
    RunStore store(root / "runs");
    Pty pty;
    Events ev;
    auto session = start(store, pty, ev, root / "project");
    pty.send("I (1) app: hi\n");
    assert(wait_for([&] { return contains(ev.logs(), "I (1) app: hi\n"); }));
    session.reset();
    fs::remove_all(root);
}

void survives_a_broken_disk() {
    const auto root = fresh("malgrind_session_f");
    RunStore store(root / "runs");
    Pty pty;
    Events ev;
    auto session = start(store, pty, ev, root / "project");
    fs::remove_all(root / "runs"); // disk full, run deleted, USB stick pulled: writes now fail
    pty.send(make_stream(1));
    assert(wait_for([&] { return session->snapshot(std::nullopt, kAll, std::nullopt).has_value(); })); // still shown live
    assert(wait_for([&] {
        for (const auto& n : ev.named("notice"))
            if (contains(n["text"], "cannot record")) return true;
        return false;
    }));
    session.reset(); // and closing does not throw either
    fs::remove_all(root);
}

} // namespace

int main() {
    live_records_and_aggregates();
    ended_on_eof();
    no_elf_raw_addresses();
    replay_matches_live();
    log_passthrough();
    survives_a_broken_disk();
    std::puts("session ok");
}
