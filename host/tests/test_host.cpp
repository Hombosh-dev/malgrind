// Host tool self-check: frame decoding/resync and the aggregation tree.
#include "aggregate.hpp"
#include "discover.hpp"
#include "os/paths.hpp"
#include "source.hpp"
#include "stream.hpp"
#include "wire.hpp"

#include "malgrind/profiler.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace malgrind;
using namespace malgrind::host;
using namespace std::chrono_literals;

int main() {
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
    assert(nlohmann::json::parse(agg.functions_json(std::nullopt, 0s, std::nullopt, ""))["callers"].empty());
    assert(nlohmann::json::parse(agg.functions_json(1u, 0s, std::nullopt, ""))["total"] == 1); // core filter

    // recursion: names {main, r, r, leaf}: r counts once in `total`, and calls itself
    Frame rec;
    rec.arch = Arch::Xtensa, rec.ncores = 1, rec.core = 0, rec.cpu_hz = 1, rec.stacks = {{5, {0x400d5000, 0x400d1000, 0x400d1000, 0x400d3000}}};
    Symbolizer rec_symbols({}, {});
    Aggregator rec_agg(rec_symbols);
    rec_agg.add(rec, 0ms);
    const auto rfn = nlohmann::json::parse(rec_agg.functions_json(std::nullopt, 0s, std::nullopt, "0x400d0fff"));
    assert(rfn["total"] == 5);
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
