// Session self-check: a live run over MemorySource (plus one POSIX pty check), its recording and its replay.
#include "os/paths.hpp"
#include "session.hpp"
#include "source.hpp"
#include "store.hpp"
#include "stream.hpp"

#ifndef _WIN32
#include "pty.hpp"
#endif

#include <nlohmann/json.hpp>

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

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
    const auto p = fs::temp_directory_path() / os::from_utf8(name);
    fs::remove_all(p);
    return p;
}

constexpr auto kAll = Aggregator::Ms{0};

struct LiveMemory {
    MemorySource* source = nullptr;
    std::unique_ptr<Session> session;
};

// 2 rounds = 4 frames: core 0 sends 3 samples per round, core 1 one, so 8 samples in all.
LiveMemory start_memory(RunStore& store, Events& ev, const fs::path& project) {
    auto source = std::make_unique<MemorySource>();
    auto* raw = source.get();
    auto session = Session::live(std::move(source), store.create({.name = "t"}), {}, project, ev.sink());
    return {raw, std::move(session)};
}

void live_records_and_aggregates() {
    const auto root = fresh("malgrind_session_a");
    RunStore store(root / "runs");
    Events ev;
    auto [src, session] = start_memory(store, ev, root / "project");
    assert(session->live() && !session->snapshot(std::nullopt, kAll, std::nullopt)); // nothing yet
    src->push(make_stream(2));
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
    Events ev;
    auto [src, session] = start_memory(store, ev, root / "project");
    src->push(make_stream(1));
    assert(wait_for([&] { return session->snapshot(std::nullopt, kAll, std::nullopt).has_value(); }));
    std::this_thread::sleep_for(20ms);
    src->close(); // the board is unplugged; Session's reader thread destroys source on EOF
    src = nullptr;
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
    Events ev;
    auto [src, session] = start_memory(store, ev, root / "project");
    src->push(make_stream(1));
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
        Events ev;
        auto [src, session] = start_memory(store, ev, root / "project");
        src->push(make_stream(3));
        assert(wait_for([&] { auto s = session->snapshot(std::nullopt, kAll, std::nullopt); return s && contains(*s, R"("name":"all","value":12)"); }));
        live_view = *session->snapshot(std::nullopt, kAll, std::nullopt);
        id = session->run_id();
    }
    Events ev;
    const auto replay = Session::replay(store, id, ev.sink());
    assert(replay && !replay->live());
    assert(replay->caps() == kCapSamples && replay->agent_state() == 1);
    assert(replay->snapshot(std::nullopt, kAll, std::nullopt) == live_view);
    assert(!Session::replay(store, "missing", ev.sink()));
    fs::remove_all(root);
}

void log_passthrough() {
    const auto root = fresh("malgrind_session_e");
    RunStore store(root / "runs");
    Events ev;
    auto [src, session] = start_memory(store, ev, root / "project");
    src->push(std::string_view("I (1) app: hi\n"));
    assert(wait_for([&] { return contains(ev.logs(), "I (1) app: hi\n"); }));
    session.reset();
    fs::remove_all(root);
}

void survives_a_broken_disk() {
    const auto root = fresh("malgrind_session_f");
    RunStore store(root / "runs");
    Events ev;
    auto [src, session] = start_memory(store, ev, root / "project");
    // Replace meta.json with a non-empty directory so RunWriter::save()'s rename fails even on Windows (where frames.mlg is open).
    const auto run_dir = root / "runs" / os::from_utf8(session->run_id());
    fs::remove(run_dir / "meta.json");
    fs::create_directories(run_dir / "meta.json/block");
    src->push(make_stream(1));
    assert(wait_for([&] { return session->snapshot(std::nullopt, kAll, std::nullopt).has_value(); })); // still shown live
    assert(wait_for([&] {
        for (const auto& n : ev.named("notice"))
            if (contains(n["text"], "cannot record")) return true;
        return false;
    }));
    session.reset(); // and closing does not throw either
    fs::remove_all(root);
}

void large_run_notice_emitted_once() {
    const auto root = fresh("malgrind_session_large");
    RunStore store(root / "runs");
    Events ev;
    auto writer = store.create({.name = "large"});
    const std::uint64_t initial = writer.bytes();
    const auto first = make_stream(1);
    const std::uint64_t threshold = initial + first.size() + 1000;

    auto source = std::make_unique<MemorySource>();
    auto* src = source.get();
    auto session = Session::live(std::move(source), std::move(writer), {}, root / "project", ev.sink(), threshold);

    auto count_large_notices = [&] {
        int count = 0;
        for (const auto& n : ev.named("notice")) {
            if (contains(n["text"].get<std::string>(), "on disk")) ++count;
        }
        return count;
    };

    // Below threshold: frame + tiny log marker must not trigger the large-run notice.
    src->push(first);
    assert(wait_for([&] { return session->snapshot(std::nullopt, kAll, std::nullopt).has_value(); }));
    src->push(std::string_view("ping\n"));
    assert(wait_for([&] { return contains(ev.logs(), "ping\n"); }));
    assert(count_large_notices() == 0);

    // Cross threshold with a larger chunk: emits once.
    const std::string big_a = std::string(1200, 'a') + "\n";
    src->push(std::string_view(big_a));
    assert(wait_for([&] { return count_large_notices() == 1; }));

    // Additional chunks beyond threshold must not emit a second large-run notice.
    const std::string big_b = std::string(1200, 'b') + "\n";
    src->push(std::string_view(big_b));
    src->close();
    src = nullptr;
    assert(wait_for([&] { return !ev.named("ended").empty(); }, 1000ms));
    assert(count_large_notices() == 1);

    session.reset();
    fs::remove_all(root);
}

void v2_hello_autostart_heap_tasks_and_replay() {
    const auto root = fresh("malgrind_session_v2");
    RunStore store(root / "runs");
    Events ev;
    auto source = std::make_unique<MemorySource>("v2-mem");
    auto* src = source.get();
    auto writer = store.create({.name = "v2", .settings = RunSettings{.rate_hz = 500}});
    auto session = Session::live(std::move(source), std::move(writer), {}, root / "project", ev.sink());

    const std::uint32_t adv_caps = kCapSamples | kCapHeap | kCapTasks | kCapAllocs | kCapErrors;
    HelloPayload sleeping{
        .seq = 1,
        .arch = Arch::Xtensa,
        .ncores = 2,
        .cpu_hz = 240'000'000,
        .agent_ver = 2,
        .caps = adv_caps | (1u << 10),
        .state = 0,
        .rate_hz = 997,
        .analyses = 0,
    };
    src->push(make_hello_frame(sleeping));

    // HELLO initializes target immediately (even before sample frames) and auto-sends START at desired rate.
    assert(wait_for([&] { return !ev.named("target").empty() && decode_commands(src->written()).size() == 1; }));
    const auto targets = ev.named("target");
    assert(targets.size() == 1);
    assert(targets[0]["arch"] == "xtensa" && targets[0]["ncores"] == 2);
    assert(targets[0]["caps"] == (adv_caps | (1u << 10)) && targets[0]["state"] == 0 && targets[0]["agent_ver"] == 2);

    const auto init_cmds = decode_commands(src->written());
    assert(init_cmds.size() == 1);
    assert(init_cmds[0].cmd == kCmdStart && init_cmds[0].rate_hz == 500);
    assert(init_cmds[0].analyses == adv_caps);

    const auto pre_snap_raw = session->snapshot(std::nullopt, kAll, std::nullopt);
    assert(pre_snap_raw.has_value());
    const auto pre_snap = json::parse(*pre_snap_raw);
    assert(pre_snap["tree"]["value"] == 0);
    assert(pre_snap["stats"]["caps"] == (adv_caps | (1u << 10)) && pre_snap["stats"]["agent_state"] == 0);

    // Agent wakes up (HELLO state=1) and streams HEAP, TASKS, and v2 SAMPLES.
    HelloPayload sampling = sleeping;
    sampling.seq = 2;
    sampling.state = 1;
    sampling.rate_hz = 500;
    sampling.analyses = kCapSamples | kCapHeap | kCapTasks;
    src->push(make_hello_frame(sampling));

    const HeapRegion h1[] = {
        {kHeapInternal, 200'000, 120'000, 100'000, 64'000},
        {kHeapIram, 64'000, 30'000, 28'000, 16'000},
    };
    const HeapRegion h2[] = {
        {kHeapInternal, 200'000, 110'000, 95'000, 50'000},
    };
    src->push(make_heap_frame(3, 1000, h1));
    src->push(make_heap_frame(4, 2000, h2));

    TaskSample t1[2]{};
    t1[0].handle = 0x10;
    std::memcpy(t1[0].name, "worker", 6);
    t1[0].prio = 5;
    t1[0].core = 0;
    t1[0].state = 1;
    t1[0].runtime = 750;
    t1[0].stack_hwm = 1024;
    t1[1].handle = 0x20;
    std::memcpy(t1[1].name, "net", 3);
    t1[1].prio = 8;
    t1[1].core = 0xff;
    t1[1].state = 2;
    t1[1].runtime = 250;
    t1[1].stack_hwm = 320;
    src->push(make_tasks_frame(5, 2000, 1000, t1));

    src->push(make_stream(1)); // 4 samples
    assert(wait_for([&] {
        auto s = session->snapshot(std::nullopt, kAll, std::nullopt);
        return s && contains(*s, R"("name":"all","value":4)") && session->agent_state() == 1;
    }));

    const auto mem_live = json::parse(session->memory_json(kAll));
    assert(mem_live["regions"].size() == 2);
    assert(mem_live["regions"][0]["name"] == "internal" && mem_live["regions"][0]["series"].size() == 2);
    assert(mem_live["regions"][1]["name"] == "iram" && mem_live["regions"][1]["series"].size() == 1);

    const auto tasks_live = json::parse(session->tasks_json(kAll));
    assert(tasks_live["tasks"].size() == 2);
    assert(tasks_live["tasks"][0]["name"] == "worker" && std::abs(tasks_live["tasks"][0]["cpu_pct"].get<double>() - 75.0) < 1e-6);
    assert(tasks_live["tasks"][0]["warn"] == false && tasks_live["tasks"][0]["core"] == 0);
    assert(tasks_live["tasks"][1]["name"] == "net" && std::abs(tasks_live["tasks"][1]["cpu_pct"].get<double>() - 25.0) < 1e-6);
    assert(tasks_live["tasks"][1]["warn"] == true && tasks_live["tasks"][1]["core"].is_null());

    // agent_stop sends STOP; a subsequent sleeping HELLO does not re-trigger auto-start.
    assert(session->agent_stop());
    HelloPayload reslept = sleeping;
    reslept.seq = 6;
    src->push(make_hello_frame(reslept));
    assert(wait_for([&] { return session->agent_state() == 0; }));
    assert(decode_commands(src->written()).size() == 2);
    assert(decode_commands(src->written())[1].cmd == kCmdStop);

    // Explicit agent_start sends START with requested parameters.
    assert(session->agent_start(250, kCapSamples | kCapHeap));
    const auto all_cmds = decode_commands(src->written());
    assert(all_cmds.size() == 3);
    assert(all_cmds[2].cmd == kCmdStart && all_cmds[2].rate_hz == 250 && all_cmds[2].analyses == (kCapSamples | kCapHeap));

    const auto snap_live = *session->snapshot(std::nullopt, kAll, std::nullopt);
    const std::string id = session->run_id();
    src->close();
    src = nullptr;
    assert(wait_for([&] { return !ev.named("ended").empty(); }, 1000ms));
    assert(!session->agent_start(500, kCapSamples));
    assert(!session->agent_stop());
    session.reset();

    // Replay restores samples, memory, tasks, caps, and agent_state without issuing commands.
    Events replay_ev;
    const auto replay = Session::replay(store, id, replay_ev.sink());
    assert(replay && !replay->live());
    assert(!replay->agent_start(500, kCapSamples));
    assert(!replay->agent_stop());
    assert(replay->caps() == (adv_caps | (1u << 10)) && replay->agent_state() == 0);
    assert(replay->snapshot(std::nullopt, kAll, std::nullopt) == snap_live);
    assert(replay->memory_json(kAll) == mem_live.dump());
    assert(replay->tasks_json(kAll) == tasks_live.dump());
    const auto replay_targets = replay_ev.named("target");
    assert(replay_targets.size() == 1 && replay_targets[0]["caps"] == (adv_caps | (1u << 10)) &&
           replay_targets[0]["agent_ver"] == 2);
    fs::remove_all(root);
}

void v2_memcheck_allocs_errors_and_replay() {
    const auto root = fresh("malgrind_session_memcheck");
    RunStore store(root / "runs");
    Events ev;
    auto source = std::make_unique<MemorySource>("mc-mem");
    auto* src = source.get();
    auto writer = store.create({.name = "mc", .settings = RunSettings{.rate_hz = 500, .check_memory = true}});
    auto session = Session::live(std::move(source), std::move(writer), {}, root / "project", ev.sink());

    const std::uint32_t mc_caps = kCapSamples | kCapHeap | kCapTasks | kCapAllocs | kCapErrors;
    HelloPayload sleeping{
        .seq = 1,
        .arch = Arch::Xtensa,
        .ncores = 2,
        .cpu_hz = 240'000'000,
        .agent_ver = 2,
        .caps = mc_caps,
        .state = 0,
        .rate_hz = 997,
        .analyses = 0,
    };
    src->push(make_hello_frame(sleeping));
    assert(wait_for([&] { return decode_commands(src->written()).size() == 1; }));
    assert(decode_commands(src->written())[0].analyses == mc_caps);

    const AllocEvent allocs[] = {
        {.op = 0, .t_ms = 1000, .addr = 0x3f801000, .size = 256, .depth = 2, .pcs = {0x400d1000, 0x400d2000}},
        {.op = 0, .t_ms = 1200, .addr = 0x3f802000, .size = 64, .depth = 1, .pcs = {0x400d3000}},
        {.op = 1, .t_ms = 2000, .addr = 0x3f802000, .size = 0, .depth = 0, .pcs = {}},
        {.op = 0, .t_ms = 35000, .addr = 0x3f803000, .size = 128, .depth = 1, .pcs = {0x400d4000}},
    };
    src->push(make_allocs_frame(2, allocs));

    ErrorEvent err_corrupt{
        .seq = 3,
        .t_ms = 5000,
        .code = kErrHeapCorrupt,
        .arg = 0x3f809000,
        .name = {},
        .depth = 1,
        .pcs = {0x400d5000},
    };
    std::memcpy(err_corrupt.name, "heap_check", 10);
    src->push(make_error_frame(err_corrupt));

    ErrorEvent err_drop{
        .seq = 4,
        .t_ms = 6000,
        .code = kErrEventsDropped,
        .arg = 9,
        .name = {},
        .depth = 0,
        .pcs = {},
    };
    std::memcpy(err_drop.name, "events", 6);
    src->push(make_error_frame(err_drop));

    assert(wait_for([&] {
        const auto mc = json::parse(session->memcheck_json());
        return mc["live_allocs"] == 2 && mc["dropped"] == 9 && mc["errors"].size() == 1;
    }));

    const auto mc_live = json::parse(session->memcheck_json());
    assert(mc_live["live_bytes"] == 384 && mc_live["live_allocs"] == 2 && mc_live["dropped"] == 9);
    assert(mc_live["leaks"].size() == 2);
    assert(mc_live["leaks"][0]["kind"] == "definitely" && mc_live["leaks"][0]["bytes"] == 256);
    assert(mc_live["leaks"][1]["kind"] == "possibly" && mc_live["leaks"][1]["bytes"] == 128);
    assert(mc_live["errors"][0]["kind"] == "heap_corrupt" && mc_live["errors"][0]["name"] == "heap_check");

    // Scrubbing before the free at 2000ms shows both 0x3f801000 and 0x3f802000 live and no errors yet.
    const auto mc_early = json::parse(session->memcheck_json(1500ms));
    assert(mc_early["live_allocs"] == 2 && mc_early["live_bytes"] == 320 && mc_early["errors"].empty());

    const std::string id = session->run_id();
    src->close();
    src = nullptr;
    assert(wait_for([&] { return !ev.named("ended").empty(); }, 1000ms));
    session.reset();

    Events replay_ev;
    const auto replay = Session::replay(store, id, replay_ev.sink());
    assert(replay && replay->caps() == mc_caps);
    assert(replay->memcheck_json() == mc_live.dump());
    assert(replay->memcheck_json(1500ms) == mc_early.dump());

    // Early ALLOCS/ERROR arriving before HELLO/Frame safely initializes memcheck and survives later setup_target.
    {
        auto early_src_owned = std::make_unique<MemorySource>("early-mc");
        auto* early_src = early_src_owned.get();
        Events early_ev;
        auto early_sess = Session::live(std::move(early_src_owned), store.create({.name = "early"}), {},
                                        root / "project", early_ev.sink());
        const AllocEvent early_alloc[] = {
            {.op = 0, .t_ms = 500, .addr = 0x3f804000, .size = 96, .depth = 1, .pcs = {0x400d6000}},
        };
        early_src->push(make_allocs_frame(1, early_alloc));
        assert(wait_for([&] { return json::parse(early_sess->memcheck_json())["live_bytes"] == 96; }));
        assert(!early_sess->snapshot(std::nullopt, kAll, std::nullopt).has_value());

        early_src->push(make_stream(1));
        assert(wait_for([&] {
            auto s = early_sess->snapshot(std::nullopt, kAll, std::nullopt);
            return s && contains(*s, R"("name":"all","value":4)");
        }));
        assert((early_sess->caps() & (kCapSamples | kCapAllocs)) == (kCapSamples | kCapAllocs));
        assert(json::parse(early_sess->memcheck_json())["live_bytes"] == 96);
        early_sess.reset();
    }

    fs::remove_all(root);
}

#ifndef _WIN32
void pty_serial_records_live() {
    const auto root = fresh("malgrind_session_pty");
    RunStore store(root / "runs");
    Pty pty;
    Events ev;
    auto source = SerialSource::open_path(pty.slave, 921600);
    assert(std::holds_alternative<std::unique_ptr<SerialSource>>(source));
    auto session = Session::live(std::get<std::unique_ptr<SerialSource>>(std::move(source)),
                                 store.create({.name = "pty"}), {}, root / "project", ev.sink());
    pty.send(make_stream(2));
    assert(wait_for([&] { auto s = session->snapshot(std::nullopt, kAll, std::nullopt); return s && contains(*s, R"("name":"all","value":8)"); }));
    std::this_thread::sleep_for(20ms);
    pty.hang_up();
    assert(wait_for([&] { return !ev.named("ended").empty(); }, 1000ms));
    assert(!session->live());
    session.reset();
    assert(store.list().size() == 1 && store.list()[0].duration_ms > 0 && store.list()[0].ncores == 2);
    fs::remove_all(root);
}
#endif

} // namespace

int main() {
    live_records_and_aggregates();
    ended_on_eof();
    no_elf_raw_addresses();
    replay_matches_live();
    log_passthrough();
    survives_a_broken_disk();
    large_run_notice_emitted_once();
    v2_hello_autostart_heap_tasks_and_replay();
    v2_memcheck_allocs_errors_and_replay();
#ifndef _WIN32
    pty_serial_records_live();
#endif
    std::puts("session ok");
}
