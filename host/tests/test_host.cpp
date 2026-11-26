// Host tool self-check: frame decoding/resync and the aggregation tree.
#include "aggregate.hpp"
#include "discover.hpp"
#include "os/paths.hpp"
#include "session.hpp"
#include "source.hpp"
#include "store.hpp"
#include "stream.hpp"
#include "wire.hpp"

#include "malgrind/profiler.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <variant>
#include <vector>

using namespace malgrind;
using namespace malgrind::host;
using namespace std::chrono_literals;

namespace {

void push_le(std::vector<std::uint8_t>& out, std::uint32_t v, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

std::vector<std::uint8_t> wrap_raw_body(std::span<const std::uint8_t> body) {
    std::vector<std::uint8_t> frame{'M', 'L', 'C', '1'};
    push_le(frame, static_cast<std::uint32_t>(body.size()), 2);
    frame.insert(frame.end(), body.begin(), body.end());
    const std::uint32_t crc = crc32(frame.data() + 4, body.size() + 2);
    push_le(frame, crc, 4);
    return frame;
}

std::vector<std::uint8_t> make_v1_sample_frame() {
    std::vector<std::uint8_t> body;
    push_le(body, 1, 1);                                  // ver = 1
    push_le(body, 42, 2);                                 // seq
    push_le(body, static_cast<std::uint8_t>(Arch::Xtensa), 1);
    push_le(body, 2, 1);                                  // ncores
    push_le(body, 0, 1);                                  // core
    push_le(body, 240'000'000, 4);                        // cpu_hz
    push_le(body, 997, 2);                                // rate_hz
    push_le(body, 4, 2);                                  // samples
    push_le(body, 1, 2);                                  // dropped
    push_le(body, 120, 4);                                // isr_avg
    push_le(body, 250, 4);                                // isr_max
    push_le(body, 5000, 4);                               // flush_cyc
    push_le(body, 1, 2);                                  // n_entries
    push_le(body, 4, 2);                                  // count
    push_le(body, 2, 1);                                  // depth
    push_le(body, 0x400d1000, 4);
    push_le(body, 0x400d2000, 4);
    return wrap_raw_body(body);
}

void test_v1_and_v2_wire() {
    // 1. Raw v1 sample frame fixture still decodes identically.
    {
        const auto v1 = make_v1_sample_frame();
        const auto res = FrameReader().feed(v1);
        assert(res.frames.size() == 1 && res.messages.empty() && res.text.empty());
        const auto& f = res.frames[0];
        assert(f.seq == 42 && f.arch == Arch::Xtensa && f.ncores == 2 && f.core == 0);
        assert(f.cpu_hz == 240'000'000 && f.rate_hz == 997 && f.samples == 4 && f.dropped == 1);
        assert(f.isr_avg == 120 && f.isr_max == 250 && f.flush_cyc == 5000);
        assert(f.stacks.size() == 1 && f.stacks[0].count == 4);
        assert(f.stacks[0].pcs == (std::vector<std::uint32_t>{0x400d1000, 0x400d2000}));
    }

    // 2. Stored v1 run replays cleanly through RunStore -> Session::replay.
    {
        const auto root = std::filesystem::temp_directory_path() / "malgrind_v1_replay_test";
        std::filesystem::remove_all(root);
        RunStore store(root);
        std::string run_id;
        {
            auto w = store.create({.name = "v1-legacy"});
            run_id = w.meta().id;
            w.append(100, make_v1_sample_frame());
            w.set_target(Arch::Xtensa, 2, 240.0);
            w.finish(100);
        }
        std::vector<nlohmann::json> events;
        const auto replay = Session::replay(store, run_id, [&](const nlohmann::json& e) { events.push_back(e); });
        assert(replay && !replay->live());
        const auto snap = replay->snapshot(std::nullopt, Aggregator::Ms{0}, std::nullopt);
        assert(snap && snap->find(R"("name":"all","value":4)") != std::string::npos);
        assert(replay->folded() == "core0;0x400d1fff;0x400d1000 4\n");
        bool saw_target = false;
        for (const auto& e : events) {
            if (e["event"] == "target") {
                saw_target = true;
                assert(e["arch"] == "xtensa" && e["ncores"] == 2);
            }
        }
        assert(saw_target);
        std::filesystem::remove_all(root);
    }

    // 3. Round-trip every v2 kind (core-encode -> host-decode), whole and chunked.
    std::vector<std::uint8_t> combined;
    auto append_buf = [&](const std::uint8_t* d, std::size_t n) {
        assert(n > 0);
        combined.insert(combined.end(), d, d + n);
    };

    std::array<std::uint8_t, 512> scratch{};

    const HelloPayload hello_in{
        .seq = 10,
        .arch = Arch::RiscV32,
        .ncores = 1,
        .cpu_hz = 160'000'000,
        .agent_ver = 2,
        .caps = kCapSamples | kCapHeap | kCapTasks | kCapAllocs | kCapErrors,
        .state = 0,
        .rate_hz = 500,
        .analyses = kCapHeap | kCapErrors,
    };
    append_buf(scratch.data(), encode_hello(scratch.data(), scratch.size(), hello_in));

    const HeapRegion heap_in[] = {
        {kHeapInternal, 200'000, 110'000, 80'000, 50'000},
        {kHeapIram, 64'000, 32'000, 30'000, 16'000},
    };
    append_buf(scratch.data(), encode_heap(scratch.data(), scratch.size(), 11, 1234, heap_in, 2));

    TaskSample tasks_in[2]{};
    tasks_in[0].handle = 0x3ffb0100;
    std::memcpy(tasks_in[0].name, "main\0dirty_tail", 15);
    tasks_in[0].prio = 1;
    tasks_in[0].core = 0;
    tasks_in[0].state = 1;
    tasks_in[0].runtime = 7500;
    tasks_in[0].stack_hwm = 512;
    tasks_in[1].handle = 0x3ffb0200;
    std::memcpy(tasks_in[1].name, "sixteen_char_nm!", 16);
    tasks_in[1].prio = 3;
    tasks_in[1].core = 0xff;
    tasks_in[1].state = 2;
    tasks_in[1].runtime = 2500;
    tasks_in[1].stack_hwm = 384;
    append_buf(scratch.data(), encode_tasks(scratch.data(), scratch.size(), 12, 2000, 10'000, tasks_in, 2));

    const AllocEvent allocs_in[] = {
        {.op = 0, .t_ms = 2100, .addr = 0x3f800400, .size = 256, .depth = 2, .pcs = {0x42001000, 0x42002000}},
        {.op = 1, .t_ms = 2200, .addr = 0x3f800400, .size = 0, .depth = 1, .pcs = {0x42003000}},
    };
    append_buf(scratch.data(), encode_allocs(scratch.data(), scratch.size(), 13, allocs_in, 2));

    ErrorEvent err_in{
        .seq = 14,
        .t_ms = 2300,
        .code = kErrHeapCorrupt,
        .arg = 0xdeadbeef,
        .name = {},
        .depth = 3,
        .pcs = {0x42004000, 0x42005000, 0x42006000},
    };
    std::memcpy(err_in.name, "net_rx\0junk", 11);
    append_buf(scratch.data(), encode_error(scratch.data(), scratch.size(), err_in));

    // Also include a v2 SAMPLES frame from Profiler in the same stream.
    {
        Profiler p({"riscv", Arch::RiscV32, 160'000'000, 1}, 500);
        const std::uint32_t pcs[] = {0x42007000, 0x42008000};
        p.record(0, pcs, 2, 88);
        p.flush(append_buf, [] {}, 777);
    }

    auto verify_v2_result = [&](const FrameReader::Result& r) {
        assert(r.text.empty());
        assert(r.frames.size() == 1);
        assert(r.frames[0].arch == Arch::RiscV32 && r.frames[0].rate_hz == 500 && r.frames[0].flush_cyc == 777);
        assert(r.frames[0].stacks.size() == 1 && r.frames[0].stacks[0].pcs == (std::vector<std::uint32_t>{0x42007000, 0x42008000}));

        assert(r.messages.size() == 5);
        const auto& h = std::get<Hello>(r.messages[0]);
        assert(h.seq == 10 && h.arch == Arch::RiscV32 && h.ncores == 1 && h.cpu_hz == 160'000'000);
        assert(h.agent_ver == 2 && h.caps == hello_in.caps && h.state == 0 && h.rate_hz == 500 && h.analyses == hello_in.analyses);

        const auto& hp = std::get<HeapStats>(r.messages[1]);
        assert(hp.seq == 11 && hp.t_ms == 1234 && hp.regions.size() == 2);
        assert(hp.regions[0].region == kHeapInternal && hp.regions[0].total == 200'000 && hp.regions[0].free == 110'000);
        assert(hp.regions[1].region == kHeapIram && hp.regions[1].largest == 16'000);

        const auto& ts = std::get<TaskStats>(r.messages[2]);
        assert(ts.seq == 12 && ts.t_ms == 2000 && ts.total_runtime == 10'000 && ts.tasks.size() == 2);
        assert(ts.tasks[0].handle == 0x3ffb0100 && ts.tasks[0].name == "main" && ts.tasks[0].stack_hwm == 512);
        assert(ts.tasks[1].handle == 0x3ffb0200 && ts.tasks[1].name == "sixteen_char_nm!" && ts.tasks[1].core == 0xff);

        const auto& ab = std::get<AllocBatch>(r.messages[3]);
        assert(ab.seq == 13 && ab.records.size() == 2);
        assert(ab.records[0].op == 0 && ab.records[0].addr == 0x3f800400 && ab.records[0].size == 256);
        assert(ab.records[0].pcs == (std::vector<std::uint32_t>{0x42001000, 0x42002000}));
        assert(ab.records[1].op == 1 && ab.records[1].size == 0 && ab.records[1].pcs == (std::vector<std::uint32_t>{0x42003000}));

        const auto& er = std::get<AgentError>(r.messages[4]);
        assert(er.seq == 14 && er.t_ms == 2300 && er.code == kErrHeapCorrupt && er.arg == 0xdeadbeef);
        assert(er.name == "net_rx" && er.pcs == (std::vector<std::uint32_t>{0x42004000, 0x42005000, 0x42006000}));
    };

    verify_v2_result(FrameReader().feed(combined));

    {
        FrameReader chunked;
        FrameReader::Result agg_res;
        for (std::size_t i = 0; i < combined.size(); i += 5) {
            auto part = chunked.feed(std::span(combined).subspan(i, std::min<std::size_t>(5, combined.size() - i)));
            agg_res.frames.insert(agg_res.frames.end(), std::make_move_iterator(part.frames.begin()), std::make_move_iterator(part.frames.end()));
            agg_res.messages.insert(agg_res.messages.end(), std::make_move_iterator(part.messages.begin()), std::make_move_iterator(part.messages.end()));
            agg_res.text += part.text;
        }
        verify_v2_result(agg_res);
    }

    // 4. Valid-CRC v2 frame with unknown kind is skipped silently (not frames, messages, or text).
    {
        const std::uint8_t unk_body[] = {kWireVersion, 99, 0x11, 0x22, 0x33, 0x44};
        const std::uint8_t cmd_body[] = {kWireVersion, kKindCommand, kCmdStart, 0xe5, 0x03, 0x01, 0x00, 0x00, 0x00};
        const auto f_unk = wrap_raw_body(unk_body);
        const auto f_cmd = wrap_raw_body(cmd_body);
        const std::string pre = "before\n", post = "after\n";
        std::vector<std::uint8_t> stream(pre.begin(), pre.end());
        stream.insert(stream.end(), f_unk.begin(), f_unk.end());
        stream.insert(stream.end(), f_cmd.begin(), f_cmd.end());
        stream.insert(stream.end(), post.begin(), post.end());
        const auto res = FrameReader().feed(stream);
        assert(res.frames.empty() && res.messages.empty() && res.text == "before\nafter\n");
    }

    // 5. Short/corrupt valid-CRC payloads and invalid versions are rejected via to_text(1), allowing resync.
    {
        const std::vector<std::vector<std::uint8_t>> bad_bodies = {
            {kWireVersion},                                         // len < 2
            {3, 0, 1, 2, 3},                                        // unknown ver=3
            {0, 0, 1, 2, 3},                                        // invalid ver=0
            {1, 0, 0, 1},                                           // truncated v1
            {kWireVersion, kKindHello, 1, 2, 3},                    // truncated HELLO
            std::vector<std::uint8_t>(24, kWireVersion),            // overlong HELLO (24 bytes with kind=1)
            {kWireVersion, kKindHeap, 1, 0, 0, 0, 0, 0, 2},         // HEAP claiming n=2 with 0 regions
            {kWireVersion, kKindError, 1, 0, 0, 0, 0, 0, 1},        // truncated ERROR
        };
        auto hello_frame = std::vector<std::uint8_t>(scratch.data(),
                                                     scratch.data() + encode_hello(scratch.data(), scratch.size(), hello_in));
        for (auto body : bad_bodies) {
            if (body.size() == 24) body[1] = kKindHello;
            auto bad = wrap_raw_body(body);
            bad.insert(bad.end(), hello_frame.begin(), hello_frame.end());
            const auto res = FrameReader().feed(bad);
            assert(res.frames.empty());
            assert(res.messages.size() == 1 && std::get<Hello>(res.messages[0]).seq == 10);
            assert(res.text.size() >= 4 && res.text.starts_with("MLC1"));
        }
    }

    // 6. encode_command -> CommandParser round-trip and FrameReader ignores COMMAND frames.
    {
        const Command cmds[] = {
            {kCmdStart, 997, kCapSamples | kCapHeap | kCapTasks},
            {kCmdStop, 0, 0},
            {kCmdHello, 250, kCapSamples | kCapAllocs | kCapErrors},
        };

        std::vector<std::uint8_t> wire_stream;
        const std::string garbage = "noise\nMLC1"; // partial/false MLC1 right before encoded command
        wire_stream.insert(wire_stream.end(), garbage.begin(), garbage.end());

        auto first = encode_command(cmds[0]);
        assert(first.size() == CommandParser::kFrameSize);
        assert(std::memcmp(first.data(), "MLC1", 4) == 0);
        assert(first[4] == 9 && first[5] == 0 && first[6] == kWireVersion && first[7] == kKindCommand);
        wire_stream.insert(wire_stream.end(), first.begin(), first.end());

        auto corrupted = encode_command(cmds[1]);
        corrupted[10] ^= 0x80; // corrupt payload byte -> bad CRC
        wire_stream.insert(wire_stream.end(), corrupted.begin(), corrupted.end());

        for (const auto& cmd : cmds) {
            const auto enc = encode_command(cmd);
            wire_stream.insert(wire_stream.end(), enc.begin(), enc.end());
        }

        auto parse_all = [&](std::size_t chunk_size) {
            CommandParser parser;
            std::vector<Command> decoded;
            for (std::size_t i = 0; i < wire_stream.size(); i += chunk_size) {
                const std::size_t n = std::min(chunk_size, wire_stream.size() - i);
                parser.feed(wire_stream.data() + i, n, [&](const Command& c) { decoded.push_back(c); });
            }
            assert(decoded.size() == 4);
            assert(decoded[0].cmd == cmds[0].cmd && decoded[0].rate_hz == cmds[0].rate_hz && decoded[0].analyses == cmds[0].analyses);
            for (std::size_t j = 0; j < 3; ++j) {
                assert(decoded[1 + j].cmd == cmds[j].cmd);
                assert(decoded[1 + j].rate_hz == cmds[j].rate_hz);
                assert(decoded[1 + j].analyses == cmds[j].analyses);
            }
        };
        parse_all(wire_stream.size());
        parse_all(1);
        parse_all(4);

        // If encoded COMMAND frames are fed back into FrameReader, they are ignored (not frames, messages, or text).
        std::vector<std::uint8_t> loopback;
        const std::string pre = "log1\n", mid = "log2\n", post = "log3\n";
        loopback.insert(loopback.end(), pre.begin(), pre.end());
        for (const auto& cmd : cmds) {
            const auto enc = encode_command(cmd);
            loopback.insert(loopback.end(), enc.begin(), enc.end());
            loopback.insert(loopback.end(), mid.begin(), mid.end());
        }
        loopback.insert(loopback.end(), post.begin(), post.end());
        const auto res = FrameReader().feed(loopback);
        assert(res.frames.empty());
        assert(res.messages.empty());
        assert(res.text == "log1\nlog2\nlog2\nlog2\nlog3\n");
    }
}

} // namespace

int main() {
    test_v1_and_v2_wire();
    const auto stream = make_stream(3); // 6 frames: core 0 and core 1 per flush

    // whole stream, and the same stream cut into odd-sized pieces
    auto whole = FrameReader().feed(stream);
    assert(whole.frames.size() == 6 && whole.text.empty());
    assert(whole.frames[0].arch == Arch::Xtensa && whole.frames[0].ncores == 2 && whole.frames[1].core == 1);
    assert(whole.frames[0].stacks.size() == 1 && whole.frames[0].stacks[0].count == 3);
    assert(whole.frames[0].stacks[0].pcs == (std::vector<std::uint32_t>{0x400d1000, 0x400d2000, 0x400d3000}));
    assert(whole.frames[0].flush_cyc == 5000 && whole.frames[0].isr_avg == 100);
    FrameReader chunked;
    std::size_t frames = 0;
    for (std::size_t i = 0; i < stream.size(); i += 7)
        frames += chunked.feed(std::span(stream).subspan(i, std::min<std::size_t>(7, stream.size() - i))).frames.size();
    assert(frames == 6);

    // log text passes through; one corrupted frame is lost, the rest still decode
    const std::string log = "I (123) app: hello\n";
    std::vector<std::uint8_t> damaged(log.begin(), log.end());
    damaged.insert(damaged.end(), stream.begin(), stream.end());
    damaged[log.size() + 20] ^= 0xFF;
    auto resynced = FrameReader().feed(damaged);
    assert(resynced.frames.size() == 5 && resynced.text.starts_with(log));

    // aggregation: no ELF, so names are the raw (call-adjusted) addresses
    Symbolizer symbols({}, {});
    Aggregator agg(symbols);
    agg.add(whole.frames[0], 0ms);      // core 0, early
    agg.add(whole.frames[1], 0ms);      // core 1, early
    agg.add(whole.frames[2], 29000ms);  // core 0, late
    auto contains = [](const std::string& s, const char* part) { return s.find(part) != std::string::npos; };

    assert(agg.duration() == 29000ms);
    assert(contains(agg.snapshot_json(std::nullopt, 10s), R"("name":"all","value":3)"));        // window ends at the last frame
    assert(contains(agg.snapshot_json(std::nullopt, 0s), R"("name":"all","value":7)"));        // 3 + 1 + 3
    assert(contains(agg.snapshot_json(std::nullopt, 5s, 1000ms), R"("name":"all","value":4)")); // scrubbed back: 3 + 1
    assert(contains(agg.snapshot_json(1u, 0s), R"("name":"all","value":1)"));
    assert(contains(agg.snapshot_json(std::nullopt, 0s), R"("duration_ms":29000)"));
    // main (0x400d3000-1) is the root of every stack and carries all of them
    assert(contains(agg.snapshot_json(std::nullopt, 0s), R"("name":"0x400d2fff","value":7)"));

    const auto folded = agg.folded();
    assert(contains(folded, "core0;0x400d2fff;0x400d1fff;0x400d1000 6\n"));
    assert(contains(folded, "core1;0x400d2fff;0x400d1fff;0x400d17ff") == false); // a core-1 leaf is not call-adjusted twice
    assert(contains(folded, "core1;0x400d2fff;0x400d1fff;0x400d1800 1\n"));

    // folded with core/window/at filters
    assert(agg.folded(1u, 0s) == "core1;0x400d2fff;0x400d1fff;0x400d1800 1\n");
    assert(agg.folded(std::nullopt, 10s) == "core0;0x400d2fff;0x400d1fff;0x400d1000 3\n");
    const auto scrubbed_folded = agg.folded(std::nullopt, 5s, 1000ms);
    assert(contains(scrubbed_folded, "core0;0x400d2fff;0x400d1fff;0x400d1000 3\n"));
    assert(contains(scrubbed_folded, "core1;0x400d2fff;0x400d1fff;0x400d1800 1\n"));
    assert(agg.folded(2u, 0s).empty());

    // speedscope_json: schema, deduplicated frames, deterministic multi-core profiles, weights & endValue
    const auto ss = nlohmann::json::parse(agg.speedscope_json("demo-run"));
    assert(ss["$schema"] == "https://www.speedscope.app/file-format-schema.json");
    assert(ss["version"] == "0.0.1");
    assert(ss["exporter"] == std::string("malgrind ") + MALGRIND_VERSION);
    assert(ss["name"] == "demo-run");
    assert(ss["activeProfileIndex"] == 0);

    const auto& frames_json = ss["shared"]["frames"];
    assert(frames_json.size() == 4); // 0x400d2fff, 0x400d1fff, 0x400d1000, 0x400d1800 (shared frames deduplicated)
    std::set<std::string> unique_names;
    for (const auto& fr : frames_json) unique_names.insert(fr["name"].get<std::string>());
    assert(unique_names.size() == 4);

    assert(ss["profiles"].size() == 2);
    const auto& p0 = ss["profiles"][0];
    const auto& p1 = ss["profiles"][1];
    assert(p0["type"] == "sampled" && p0["name"] == "core0" && p0["unit"] == "none" && p0["startValue"] == 0 && p0["endValue"] == 6);
    assert(p0["samples"].size() == 1 && p0["weights"] == nlohmann::json::array({6}));
    assert(p1["type"] == "sampled" && p1["name"] == "core1" && p1["unit"] == "none" && p1["startValue"] == 0 && p1["endValue"] == 1);
    assert(p1["samples"].size() == 1 && p1["weights"] == nlohmann::json::array({1}));
    auto stack_names = [&](const nlohmann::json& sample) {
        std::vector<std::string> out;
        for (const auto& idx : sample) out.push_back(frames_json[idx.get<std::size_t>()]["name"].get<std::string>());
        return out;
    };
    assert(stack_names(p0["samples"][0]) == (std::vector<std::string>{"0x400d2fff", "0x400d1fff", "0x400d1000"}));
    assert(stack_names(p1["samples"][0]) == (std::vector<std::string>{"0x400d2fff", "0x400d1fff", "0x400d1800"}));

    // speedscope_json with filters and empty result
    const auto ss_c1 = nlohmann::json::parse(agg.speedscope_json("c1", 1u, 0s));
    assert(ss_c1["profiles"].size() == 1 && ss_c1["profiles"][0]["name"] == "core1" && ss_c1["profiles"][0]["endValue"] == 1);
    assert(ss_c1["shared"]["frames"].size() == 3 && ss_c1["activeProfileIndex"] == 0);
    const auto ss_empty = nlohmann::json::parse(agg.speedscope_json("empty", 2u, 0s));
    assert(ss_empty["$schema"] == "https://www.speedscope.app/file-format-schema.json");
    assert(ss_empty["shared"]["frames"].empty() && ss_empty["profiles"].empty());
    assert(!ss_empty.contains("activeProfileIndex"));

    // multi-stack per core: weights sum to endValue and stacks are sorted deterministically
    Aggregator multi_agg(symbols);
    Frame f_a, f_b;
    f_a.arch = Arch::Xtensa, f_a.ncores = 1, f_a.core = 0, f_a.cpu_hz = 1;
    f_a.stacks = {{3, {0x400d2000, 0x400d3000}}, {2, {0x400d1000, 0x400d3000}}};
    f_b.arch = Arch::Xtensa, f_b.ncores = 1, f_b.core = 0, f_b.cpu_hz = 1;
    f_b.stacks = {{4, {0x400d1000, 0x400d3000}}};
    multi_agg.add(f_a, 0ms);
    multi_agg.add(f_b, 100ms);
    const auto ss_multi = nlohmann::json::parse(multi_agg.speedscope_json("multi"));
    assert(ss_multi["profiles"].size() == 1);
    assert(ss_multi["profiles"][0]["weights"] == nlohmann::json::array({6, 3}));
    assert(ss_multi["profiles"][0]["endValue"] == 9);

    // function table: b = 0x400d1fff is called by main and calls a (6 samples) and c (1)
    const auto fn = nlohmann::json::parse(agg.functions_json(std::nullopt, 0s, std::nullopt, "0x400d1fff"));
    assert(fn["total"] == 7);
    assert(fn["functions"][0]["name"] == "0x400d1000" && fn["functions"][0]["self"] == 6 && fn["functions"][0]["total"] == 6);
    assert(fn["callers"].size() == 1 && fn["callers"][0]["name"] == "0x400d2fff" && fn["callers"][0]["count"] == 7);
    assert(fn["callees"].size() == 2);
    assert(fn["series"] == nlohmann::json::array({{0, 4}, {29000, 3}}));
    assert(nlohmann::json::parse(agg.functions_json(std::nullopt, 0s, std::nullopt, "0x400d1800"))["series"] ==
           nlohmann::json::array({{0, 1}, {29000, 0}}));
    assert(nlohmann::json::parse(agg.functions_json(std::nullopt, 0s, std::nullopt, ""))["callers"].empty());
    assert(nlohmann::json::parse(agg.functions_json(std::nullopt, 0s, std::nullopt, ""))["series"].empty());
    assert(nlohmann::json::parse(agg.functions_json(1u, 0s, std::nullopt, "0x400d1fff"))["series"] ==
           nlohmann::json::array({{0, 1}}));
    assert(nlohmann::json::parse(agg.functions_json(std::nullopt, 10s, std::nullopt, "0x400d1fff"))["series"] ==
           nlohmann::json::array({{29000, 3}}));
    assert(nlohmann::json::parse(agg.functions_json(std::nullopt, 5s, 1000ms, "0x400d1fff"))["series"] ==
           nlohmann::json::array({{0, 4}}));
    assert(nlohmann::json::parse(agg.functions_json(1u, 0s, std::nullopt, ""))["total"] == 1); // core filter

    // Multiple frames inside the same 1s bucket sum together, and recursion counts once per sample in `series`.
    {
        Aggregator bucket_agg(symbols);
        bucket_agg.set_target(Arch::Xtensa, 1, 240'000'000, 997);
        bucket_agg.set_agent_status(kCapSamples | kCapHeap, 0);
        const auto pre_snap = nlohmann::json::parse(bucket_agg.snapshot_json(std::nullopt, 0s));
        assert(pre_snap["tree"]["value"] == 0);
        assert(pre_snap["stats"]["arch"] == "xtensa" && pre_snap["stats"]["rate_hz"] == 997);
        assert(pre_snap["stats"]["caps"] == (kCapSamples | kCapHeap) && pre_snap["stats"]["agent_state"] == 0);

        Frame f1, f2, f3;
        f1.arch = f2.arch = f3.arch = Arch::Xtensa;
        f1.ncores = f2.ncores = f3.ncores = 1;
        f1.core = f2.core = f3.core = 0;
        f1.cpu_hz = f2.cpu_hz = f3.cpu_hz = 240'000'000;
        f1.rate_hz = f2.rate_hz = f3.rate_hz = 997;
        f1.stacks = {{2, {0x400d1000, 0x400d3000}}};
        f2.stacks = {{3, {0x400d1000, 0x400d3000}}, {4, {0x400d2000, 0x400d3000}}};
        f3.stacks = {{6, {0x400d2000, 0x400d3000}}};
        bucket_agg.set_agent_status(kCapSamples | kCapHeap, 1);
        bucket_agg.add(f1, 1200ms);
        bucket_agg.add(f2, 1800ms);
        bucket_agg.add(f3, 2100ms);
        const auto bfn = nlohmann::json::parse(bucket_agg.functions_json(std::nullopt, 0s, std::nullopt, "0x400d1000"));
        assert(bfn["series"] == nlohmann::json::array({{1000, 5}, {2000, 0}}));
        const auto post_snap = nlohmann::json::parse(bucket_agg.snapshot_json(std::nullopt, 0s));
        assert(post_snap["stats"]["caps"] == (kCapSamples | kCapHeap) && post_snap["stats"]["agent_state"] == 1);
    }

    // recursion: names {main, r, r, leaf}: r counts once in `total`, and calls itself
    Frame rec;
    rec.arch = Arch::Xtensa, rec.ncores = 1, rec.core = 0, rec.cpu_hz = 1, rec.stacks = {{5, {0x400d5000, 0x400d1000, 0x400d1000, 0x400d3000}}};
    Symbolizer rec_symbols({}, {});
    Aggregator rec_agg(rec_symbols);
    rec_agg.add(rec, 0ms);
    const auto rfn = nlohmann::json::parse(rec_agg.functions_json(std::nullopt, 0s, std::nullopt, "0x400d0fff"));
    assert(rfn["total"] == 5);
    assert(rfn["series"] == nlohmann::json::array({{0, 5}}));
    for (const auto& f : rfn["functions"])
        if (f["name"] == "0x400d0fff") assert(f["total"] == 5 && f["self"] == 0);   // 5, not 10
    assert(rfn["callees"].size() == 2); // itself and the leaf

    // project and ELF discovery in a throw-away tree
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "malgrind_test_tree";
    fs::remove_all(root);
    auto fake_elf = [](const fs::path& file, unsigned char machine) {
        fs::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::binary);
        const unsigned char header[20] = {0x7f, 'E', 'L', 'F', 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, machine, 0};
        out.write(reinterpret_cast<const char*>(header), sizeof header);
    };
    std::string why;
    fs::create_directories(root / "firmware/a");
    assert(!find_project(root, why) && why.find("no PlatformIO project") != std::string::npos);
    std::ofstream(root / "firmware/a/platformio.ini");
    assert(find_project(root, why) == root / "firmware/a");
    fs::create_directories(root / "firmware/b");
    std::ofstream(root / "firmware/b/platformio.ini");
    assert(!find_project(root, why) && why.find("several") != std::string::npos);

    fake_elf(root / "firmware/a/.pio/build/esp32dev/firmware.elf", 94);   // Xtensa
    fake_elf(root / "firmware/b/.pio/build/stm/firmware.elf", 40);        // ARM, newer
    fake_elf(root / "build/app.elf", 94);                                  // Xtensa, newest
    fs::last_write_time(root / "build/app.elf", fs::file_time_type::clock::now() + 10s);
    assert(elf_machine(root / "firmware/a/.pio/build/esp32dev/firmware.elf") == 94);
    assert(find_elf(Arch::Xtensa, root) == root / "build/app.elf");         // newest of the matching ones
    assert(find_elf(Arch::ArmV7M, root) == root / "firmware/b/.pio/build/stm/firmware.elf");
    assert(!find_elf(Arch::RiscV32, root));                                  // nothing built for it
    assert(!elf_machine(root / "firmware/a/platformio.ini"));                // not an ELF
    // an agent build (.pio/malgrind) wins over newer ordinary builds: it is the firmware that sends frames
    const auto agent_elf = root / "firmware/a/.pio/malgrind/build/esp32dev/firmware.elf";
    fake_elf(agent_elf, 94);
    fs::last_write_time(agent_elf, fs::file_time_type::clock::now() - 100s);
    assert(find_elf(Arch::Xtensa, root) == agent_elf);
    assert(find_elf(Arch::ArmV7M, root) == root / "firmware/b/.pio/build/stm/firmware.elf");
    fs::remove_all(root);

    // Symbolizer with extra ELFs (address ranges, fallback to primary ELF, and empty primary ELF)
    {
        const auto sym_root = fs::canonical(fs::temp_directory_path()) / "malgrind_test_sym";
        fs::remove_all(sym_root);
        fs::create_directories(sym_root);

        const fs::path tool = sym_root / os::exe_name("target-addr2line");
        fs::copy_file(MALGRIND_FAKE_TOOL, tool, fs::copy_options::overwrite_existing);

        const fs::path main_elf = sym_root / "main.elf";
        const fs::path extra_elf = sym_root / "extra.elf";
        const fs::path alt_extra_elf = sym_root / "alt_extra.elf";
        {
            std::ofstream out(main_elf, std::ios::binary);
            out << "0x40001000=shadowed_by_extra\n";
            out << "0x40005000=main_fallback_fn\n";
            out << "0x400d1fff=app_caller\n";
            out << "0x400d1000=app_leaf\n";
        }
        {
            std::ofstream out(extra_elf, std::ios::binary);
            out << "0x40000000=extra_start\n";
            out << "0x40001000=extra_leaf\n";
            out << "0x4006ffff=extra_end\n";
        }
        {
            std::ofstream out(alt_extra_elf, std::ios::binary);
            out << "0x40001000=alt_extra_leaf\n";
            out << "0x40002000=alt_extra_only\n";
        }

        // Extra ELF resolves range [0x40000000, 0x4006ffff], out-of-range uses main ELF,
        // unresolved in extra ELF falls back to main ELF, and first matching existing ExtraElf wins.
        Symbolizer sym(main_elf, tool,
                       {{0x40000000u, 0x4006ffffu, sym_root / "missing.elf"},
                        {0x40000000u, 0x4006ffffu, extra_elf},
                        {0x40000000u, 0x4006ffffu, alt_extra_elf}});
        Frame f;
        f.arch = Arch::Xtensa;
        f.ncores = 1;
        f.core = 0;
        f.cpu_hz = 1;
        f.stacks = {
            {1, {0x40001000, 0x40005001, 0x400d2000}},
            {1, {0x40000000, 0x4006ffff + 1, 0x40006001}},
            {1, {0x4006ffff, 0x400d1001}},
        };
        sym.prepare(f);
        assert(sym.frames(0, 0x40001000) == (std::vector<std::string>{"extra_leaf"}));
        assert(sym.frames(1, 0x40005001) == (std::vector<std::string>{"main_fallback_fn"}));
        assert(sym.frames(2, 0x400d2000) == (std::vector<std::string>{"app_caller"}));
        assert(sym.frames(0, 0x40000000) == (std::vector<std::string>{"extra_start"}));
        assert(sym.frames(1, 0x4006ffff + 1) == (std::vector<std::string>{"extra_end"}));
        assert(sym.frames(2, 0x40006001) == (std::vector<std::string>{"0x40006000"}));
        assert(sym.frames(0, 0x4006ffff) == (std::vector<std::string>{"extra_end"}));
        assert(sym.frames(1, 0x400d1001) == (std::vector<std::string>{"app_leaf"}));

        // Empty main ELF: extra ELF still symbolizes matching addresses; others remain raw.
        const fs::path plain_tool = sym_root / os::exe_name("addr2line");
        fs::copy_file(MALGRIND_FAKE_TOOL, plain_tool, fs::copy_options::overwrite_existing);
        Symbolizer extra_only({}, plain_tool, {{0x40000000u, 0x4006ffffu, extra_elf}});
        extra_only.prepare(f);
        assert(extra_only.frames(0, 0x40001000) == (std::vector<std::string>{"extra_leaf"}));
        assert(extra_only.frames(1, 0x40005001) == (std::vector<std::string>{"0x40005000"}));
        assert(extra_only.frames(2, 0x400d2000) == (std::vector<std::string>{"0x400d1fff"}));

        fs::remove_all(sym_root);
    }

    std::puts("host ok");
}
