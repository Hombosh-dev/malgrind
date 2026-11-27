// End-to-end and unit tests for OpenOcdSource (DWT PCSR, Halt/Resume multi-core, Auto fallback,
// target-name sanitization, Symbolizer [sleep] pseudo-PC, disconnect/timeout/malformed handling).
#include "aggregate.hpp"
#include "env.hpp"
#include "families/esp32.hpp"
#include "openocd_source.hpp"
#include "os/paths.hpp"
#include "os/socket.hpp"
#include "probe_config.hpp"
#include "session.hpp"
#include "store.hpp"
#include "stream.hpp"
#include "symbols.hpp"
#include "wire.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace malgrind;
using namespace malgrind::host;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

bool contains(std::string_view s, std::string_view part) { return s.find(part) != std::string_view::npos; }

fs::path fresh_dir(const char* name) {
    const auto p = fs::canonical(fs::temp_directory_path()) / os::from_utf8(name);
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

void write_file(const fs::path& file, std::string_view text = "") {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string slurp(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

std::size_t count_occurrences(std::string_view text, std::string_view needle) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string_view::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

OpenOcdInstall fake_openocd_install(const fs::path& dir) {
    const fs::path exe = dir / "bin" / os::exe_name("openocd");
    const fs::path scripts = dir / "scripts";
    fs::create_directories(exe.parent_path());
    fs::copy_file(MALGRIND_FAKE_TOOL, exe, fs::copy_options::overwrite_existing);
    if constexpr (os::kind() != os::Kind::Windows) {
        fs::permissions(exe, fs::perms::owner_all);
    }
    write_file(scripts / "interface/stlink.cfg");
    return {exe, scripts};
}

std::vector<std::uint8_t> drain_until_eof(ByteSource& src, std::chrono::milliseconds limit = 3000ms) {
    std::vector<std::uint8_t> bytes;
    std::array<std::uint8_t, 1024> buf{};
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto n = src.read(buf, 50);
        if (!n) return bytes;
        if (*n > 0) bytes.insert(bytes.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n));
    }
    return bytes;
}

std::vector<std::uint8_t> read_at_least(ByteSource& src, std::size_t min_bytes, std::chrono::milliseconds limit = 3000ms) {
    std::vector<std::uint8_t> bytes;
    std::array<std::uint8_t, 1024> buf{};
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (bytes.size() < min_bytes && std::chrono::steady_clock::now() < deadline) {
        const auto n = src.read(buf, 50);
        if (!n) break;
        if (*n > 0) bytes.insert(bytes.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n));
    }
    return bytes;
}

void parser_and_symbolizer_checks() {
    assert(parse_openocd_u32("0x08001234") == 0x08001234u);
    assert(parse_openocd_u32(" 0XFFFFFFFF\r\n") == 0xFFFFFFFFu);
    assert(parse_openocd_u32("134222388") == 0x08001234u);
    assert(parse_openocd_u32("4294967295") == 0xFFFFFFFFu);
    assert(parse_openocd_u32("{0x08001234}") == 0x08001234u);
    assert(!parse_openocd_u32(""));
    assert(!parse_openocd_u32("Error: failed"));
    assert(!parse_openocd_u32("0x100000000"));
    assert(!parse_openocd_u32("-1"));

    assert(parse_openocd_pc("pc (/32): 0x400d1234\n") == 0x400d1234u);
    assert(parse_openocd_pc("pc: 0x400D1240 (dirty)\r\n") == 0x400d1240u);
    assert(parse_openocd_pc("0x08001250") == 0x08001250u);
    assert(parse_openocd_pc("pc: 400d1260") == 0x400d1260u);
    assert(parse_openocd_pc("pc (/32): 0xffffffff") == 0xFFFFFFFFu);
    assert(!parse_openocd_pc(""));
    assert(!parse_openocd_pc("Target not halted"));
    assert(!parse_openocd_pc("Error: Target not examined"));
    assert(!parse_openocd_pc("ok:reg pc"));

    assert((parse_openocd_target_names("esp32.cpu0 esp32.cpu1\n") ==
            std::vector<std::string>{"esp32.cpu0", "esp32.cpu1"}));
    assert(parse_openocd_target_names("  \r\n ")->empty());
    assert(!parse_openocd_target_names("cpu0;shutdown"));
    assert(!parse_openocd_target_names("cpu0 [exit]"));
    assert(!parse_openocd_target_names("cpu0 $env"));
    assert(!parse_openocd_target_names("-c"));
    assert(!parse_openocd_target_names(std::string_view("cpu0\x1b[A")));
    assert(!parse_openocd_target_names(std::string_view("cpu0\0bad", 8)));

    assert((parse_openocd_pcsr_batch("0x08001234 134222400 0xffffffff") ==
            std::vector<std::uint32_t>{0x08001234u, 0x08001240u, 0xFFFFFFFFu}));
    assert(!parse_openocd_pcsr_batch(""));
    assert(!parse_openocd_pcsr_batch("0x08001234 Error:"));

    // Symbolizer kSleepPseudoPc without ELF/tool
    Symbolizer bare({}, {});
    assert(bare.frames(0, kSleepPseudoPc) == (std::vector<std::string>{"[sleep]"}));
    assert(bare.frames(3, kSleepPseudoPc) == (std::vector<std::string>{"[sleep]"}));
    assert(bare.stack_names(std::array{kSleepPseudoPc}) == (std::vector<std::string>{"[sleep]"}));
}

void dwt_pcsr_end_to_end_checks() {
    ScopedEnv save_rpc("FAKE_OPENOCD_RPC_LOG");
    ScopedEnv save_pcsr("FAKE_OPENOCD_PCSR");
    ScopedEnv save_type("FAKE_OPENOCD_TARGET_TYPE");
    ScopedEnv save_targets("FAKE_OPENOCD_TARGETS");
    ScopedEnv save_cpuid("FAKE_OPENOCD_CPUID");
    ScopedEnv save_demcr_ro("FAKE_OPENOCD_DEMCR_RO");
    ScopedEnv save_a2l("FAKE_ADDR2LINE_LOG");

    const auto root = fresh_dir("malgrind_ocd_src_dwt");
    const auto install = fake_openocd_install(root / "ocd");
    const auto rpc_log = root / "rpc.log";
    const auto a2l_log = root / "a2l.log";
    os::set_env("FAKE_OPENOCD_RPC_LOG", os::to_utf8(rpc_log));
    os::set_env("FAKE_ADDR2LINE_LOG", os::to_utf8(a2l_log));
    os::set_env("FAKE_OPENOCD_TARGETS", "stm32f4x.cpu");
    os::set_env("FAKE_OPENOCD_TARGET_TYPE", "cortex_m");
    // Mix hex, decimal (134222400 = 0x08001240), sleep (0xffffffff), and invalid zero (0x00000000).
    os::set_env("FAKE_OPENOCD_PCSR", "0x08001234,134222400,0xffffffff,0x00000000");

    // 1. Direct OpenOcd::start + OpenOcdSource -> FrameReader -> Profiler/Aggregator + Symbolizer
    {
        auto src = OpenOcdSource::start(install, {"-f", "interface/stlink.cfg", "-f", "target/stm32f4x.cfg"},
                                        {.sample_mode = ProbeSampleMode::Auto,
                                         .poll_hz = 2000,
                                         .dwt_batch_size = 8,
                                         .flush_interval = 40ms,
                                         .label = "stlink-dwt"});
        assert(src->describe() == "stlink-dwt");

        // Wait until enough framed data arrives (Hello + at least 2 sample frames), then close cleanly.
        assert(wait_for([&] { return src->effective_sample_mode() == ProbeSampleMode::DwtPcsr; }));
        std::this_thread::sleep_for(120ms);
        src->close();
        assert(src->last_error().empty());
        assert(src->detected_arch() == Arch::ArmV7M);
        assert(src->detected_cores() == 1);

        const auto stream = drain_until_eof(*src);
        std::array<std::uint8_t, 16> eof_buf{};
        assert(!src->read(eof_buf, 10).has_value());

        const auto parsed = FrameReader().feed(stream);
        assert(parsed.text.empty());
        assert(parsed.messages.size() == 1);
        const auto& hello = std::get<Hello>(parsed.messages[0]);
        assert(hello.arch == Arch::ArmV7M && hello.ncores == 1 && hello.cpu_hz == 0);
        assert(hello.caps == kCapSamples && hello.state == 1);
        assert(!parsed.frames.empty());

        // Symbolize with fake addr2line and verify [sleep], resolved PCs, no 0x00000000, and no addr2line call for 0xfffffff0.
        const fs::path a2l = root / os::exe_name("arm-none-eabi-addr2line");
        fs::copy_file(MALGRIND_FAKE_TOOL, a2l, fs::copy_options::overwrite_existing);
        if constexpr (os::kind() != os::Kind::Windows) fs::permissions(a2l, fs::perms::owner_all);
        const fs::path elf = root / "app.elf";
        write_file(elf, "0x08001234=app_tick\n0x08001240=app_idle\n");

        Symbolizer sym(elf, a2l);
        Aggregator agg(sym);
        Aggregator::Ms t{0};
        for (const auto& f : parsed.frames) {
            assert(f.arch == Arch::ArmV7M && f.ncores == 1 && f.core == 0);
            for (const auto& st : f.stacks) {
                assert(st.pcs.size() == 1);
                assert(st.pcs[0] != 0u);
            }
            agg.add(f, t);
            t += 40ms;
        }
        const auto folded = agg.folded();
        assert(contains(folded, "core0;app_tick "));
        assert(contains(folded, "core0;app_idle "));
        assert(contains(folded, "core0;[sleep] "));
        assert(!contains(folded, "0x0\n"));

        const auto snap = nlohmann::json::parse(agg.snapshot_json(std::nullopt, 0s));
        assert(snap["stats"]["overhead_pct"].is_null());

        const std::string a2l_calls = slurp(a2l_log);
        assert(contains(a2l_calls, "0x8001234"));
        assert(contains(a2l_calls, "0x8001240"));
        assert(!contains(a2l_calls, "fffffff0"));

        const std::string rpc = slurp(rpc_log);
        assert(count_occurrences(rpc, "mww 0xE000EDFC 0x01000000") == 1);
        assert(contains(rpc, "for {set i 0} {$i < 8}"));
        assert(!contains(rpc, "halt\n"));
    }

    // 2. End-to-end into Session::live + RunStore + Session::replay
    {
        RunStore store(root / "runs");
        std::mutex ev_mu;
        std::vector<nlohmann::json> events;
        auto sink = [&](const nlohmann::json& e) {
            std::scoped_lock lock(ev_mu);
            events.push_back(e);
        };

        auto src = OpenOcdSource::start(install, {"-f", "interface/stlink.cfg", "-f", "target/stm32f4x.cfg"},
                                        {.sample_mode = ProbeSampleMode::DwtPcsr,
                                         .dwt_batch_size = 4,
                                         .flush_interval = 40ms});
        auto* raw_src = src.get();
        auto session = Session::live(std::move(src), store.create({.name = "probe-dwt"}), {}, {}, sink);
        assert(wait_for([&] {
            const auto f = session->folded();
            return f && contains(*f, "core0;[sleep] ") && contains(*f, "core0;0x8001234 ");
        }));
        assert(session->caps() == kCapSamples && session->agent_state() == 1);

        raw_src->close();
        assert(wait_for([&] {
            std::scoped_lock lock(ev_mu);
            for (const auto& e : events) {
                if (e["event"] == "ended") return true;
            }
            return false;
        }));
        const std::string run_id = session->run_id();
        const auto live_folded = *session->folded();
        session.reset();

        const auto replay = Session::replay(store, run_id, sink);
        assert(replay && replay->folded() == live_folded);
    }

    fs::remove_all(root);
}

void halt_resume_dual_core_and_auto_fallback_checks() {
    ScopedEnv save_rpc("FAKE_OPENOCD_RPC_LOG");
    ScopedEnv save_targets("FAKE_OPENOCD_TARGETS");
    ScopedEnv save_type("FAKE_OPENOCD_TARGET_TYPE");
    ScopedEnv save_cpuid("FAKE_OPENOCD_CPUID");
    ScopedEnv save_demcr_ro("FAKE_OPENOCD_DEMCR_RO");
    ScopedEnv save_req_halt("FAKE_OPENOCD_REQUIRE_HALT");
    ScopedEnv save_pc0("FAKE_OPENOCD_REG_PC_0");
    ScopedEnv save_pc1("FAKE_OPENOCD_REG_PC_1");
    ScopedEnv save_path("PATH");

    const auto root = fresh_dir("malgrind_ocd_src_halt");
    const auto install = fake_openocd_install(root / "ocd");

    // Put fake xtensa-esp32-elf-addr2line on PATH so Session resolves ELF symbols automatically.
    const fs::path bin_dir = root / "bin";
    fs::create_directories(bin_dir);
    const fs::path a2l = bin_dir / os::exe_name("xtensa-esp32-elf-addr2line");
    fs::copy_file(MALGRIND_FAKE_TOOL, a2l, fs::copy_options::overwrite_existing);
    if constexpr (os::kind() != os::Kind::Windows) fs::permissions(a2l, fs::perms::owner_all);
    const std::string old_path = os::get_env("PATH").value_or("");
    os::set_env("PATH", os::to_utf8(bin_dir) + (old_path.empty() ? "" : (os::kind() == os::Kind::Windows ? ";" : ":") + old_path));

    const fs::path elf = root / "firmware.elf";
    write_file(elf,
               "0x400d1000=core0_work\n"
               "0x400d1010=core0_helper\n"
               "0x400d2000=core1_net\n"
               "0x400d2010=core1_rx\n");

    // 1. Dual-core Halt/Resume via ProbeConfig (Esp32Family sets HaltResume + Xtensa) into Session
    {
        const fs::path rpc_log = root / "dual_rpc.log";
        os::set_env("FAKE_OPENOCD_RPC_LOG", os::to_utf8(rpc_log));
        os::set_env("FAKE_OPENOCD_TARGETS", "esp32.cpu0 esp32.cpu1");
        os::set_env("FAKE_OPENOCD_TARGET_TYPE", "esp32");
        os::set_env("FAKE_OPENOCD_REQUIRE_HALT", "1");
        os::set_env("FAKE_OPENOCD_REG_PC_0", "pc (/32): 0x400d1000,pc: 0x400d1010,0xffffffff");
        os::set_env("FAKE_OPENOCD_REG_PC_1", "pc: 0x400d2000,pc (/32): 0x400d2010");

        Device probe;
        probe.id = "probe-303a-1001";
        probe.kind = DeviceKind::Probe;
        probe.name = "ESP USB JTAG";
        probe.usb = os::UsbInfo{"303a", "1001", "SN123", "1-1", "ESP USB JTAG", "Espressif"};
        const auto cfg = resolve_probe_config(probe, &esp32_family(), nullptr, {}, "target/esp32.cfg");
        assert(cfg.config.has_value());
        assert(cfg.config->sample_mode == ProbeSampleMode::HaltResume);
        assert(cfg.config->target_arch == Arch::Xtensa);

        auto src = OpenOcdSource::start_with_config(install, *cfg.config,
                                                    {.elf = elf, .poll_hz = 100, .flush_interval = 40ms});
        auto* raw_src = src.get();
        RunStore store(root / "runs");
        std::vector<nlohmann::json> events;
        std::mutex ev_mu;
        auto session = Session::live(std::move(src), store.create({.name = "dual-core"}), elf, {},
                                     [&](const nlohmann::json& e) {
                                         std::scoped_lock lock(ev_mu);
                                         events.push_back(e);
                                     });

        assert(wait_for([&] {
            const auto f = session->folded();
            return f && contains(*f, "core0;core0_work ") && contains(*f, "core0;[sleep] ") &&
                   contains(*f, "core1;core1_net ") && contains(*f, "core1;core1_rx ");
        }));
        assert(raw_src->effective_sample_mode() == ProbeSampleMode::HaltResume);
        assert(raw_src->detected_arch() == Arch::Xtensa);
        assert(raw_src->detected_cores() == 2);
        raw_src->close();
        session.reset();

        {
            std::scoped_lock lock(ev_mu);
            bool saw_target = false;
            for (const auto& e : events) {
                if (e["event"] == "target") {
                    saw_target = true;
                    assert(e["arch"] == "xtensa" && e["ncores"] == 2 && e["caps"] == kCapSamples && e["state"] == 1);
                }
            }
            assert(saw_target);
        }

        const std::string rpc = slurp(rpc_log);
        assert(contains(rpc, "target names\n"));
        assert(contains(rpc, "halt\n"));
        assert(contains(rpc, "targets esp32.cpu0\n"));
        assert(contains(rpc, "targets esp32.cpu1\n"));
        assert(contains(rpc, "reg pc\n"));
        assert(contains(rpc, "resume\n"));
        assert(!contains(rpc, "0xE000EDFC")); // must not touch ARM CoreDebug on Xtensa
    }

    // 2. Auto fallback on Cortex-M0+ (CPUID 0x410CC601) -> multi-core HaltResume
    {
        const fs::path rpc_log = root / "m0_rpc.log";
        os::set_env("FAKE_OPENOCD_RPC_LOG", os::to_utf8(rpc_log));
        os::set_env("FAKE_OPENOCD_TARGETS", "rp2040.core0 rp2040.core1");
        os::set_env("FAKE_OPENOCD_TARGET_TYPE", "cortex_m");
        os::set_env("FAKE_OPENOCD_CPUID", "0x410CC601");
        os::set_env("FAKE_OPENOCD_REG_PC_0", "0x20000100");
        os::set_env("FAKE_OPENOCD_REG_PC_1", "0x20000200");

        auto src = OpenOcdSource::start(install, {}, {.sample_mode = ProbeSampleMode::Auto, .poll_hz = 100, .flush_interval = 40ms});
        assert(wait_for([&] { return src->effective_sample_mode() != ProbeSampleMode::Auto; }));
        assert(src->effective_sample_mode() == ProbeSampleMode::HaltResume);
        assert(src->detected_arch() == Arch::ArmV7M);
        assert(src->detected_cores() == 2);
        std::this_thread::sleep_for(60ms);
        src->close();
        const auto parsed = FrameReader().feed(drain_until_eof(*src));
        assert(!parsed.frames.empty());
        assert(!contains(slurp(rpc_log), "mww 0xE000EDFC"));
    }

    // 3. Auto fallback when DEMCR.TRCENA cannot be enabled (read-only)
    {
        os::unset_env("FAKE_OPENOCD_CPUID");
        os::set_env("FAKE_OPENOCD_DEMCR_RO", "1");
        os::set_env("FAKE_OPENOCD_TARGETS", "stm32.cpu");
        os::set_env("FAKE_OPENOCD_TARGET_TYPE", "hla_target");
        auto src = OpenOcdSource::start(install, {}, {.sample_mode = ProbeSampleMode::Auto, .poll_hz = 100, .flush_interval = 40ms});
        assert(wait_for([&] { return src->effective_sample_mode() != ProbeSampleMode::Auto; }));
        assert(src->effective_sample_mode() == ProbeSampleMode::HaltResume);
        src->close();
    }

    // 4. Auto on riscv target type -> HaltResume + Arch::RiscV32 without touching CoreDebug
    {
        const fs::path rpc_log = root / "rv_rpc.log";
        os::unset_env("FAKE_OPENOCD_DEMCR_RO");
        os::set_env("FAKE_OPENOCD_RPC_LOG", os::to_utf8(rpc_log));
        os::set_env("FAKE_OPENOCD_TARGETS", "riscv.cpu");
        os::set_env("FAKE_OPENOCD_TARGET_TYPE", "riscv");
        auto src = OpenOcdSource::start(install, {}, {.sample_mode = ProbeSampleMode::Auto, .poll_hz = 100, .flush_interval = 40ms});
        assert(wait_for([&] { return src->effective_sample_mode() != ProbeSampleMode::Auto; }));
        assert(src->effective_sample_mode() == ProbeSampleMode::HaltResume);
        assert(src->detected_arch() == Arch::RiscV32);
        assert(src->detected_cores() == 1);
        src->close();
        assert(!contains(slurp(rpc_log), "0xE000EDFC"));
        assert(!contains(slurp(rpc_log), "0xE000ED00"));
    }

    // 5. Empty "target names" falls back to single unnamed current target (no "targets <name>" call)
    {
        const fs::path rpc_log = root / "empty_targets_rpc.log";
        os::set_env("FAKE_OPENOCD_RPC_LOG", os::to_utf8(rpc_log));
        os::set_env("FAKE_OPENOCD_TARGETS", "");
        os::set_env("FAKE_OPENOCD_REG_PC_0", "pc: 0x08004444");
        auto src = OpenOcdSource::start(install, {}, {.sample_mode = ProbeSampleMode::HaltResume, .poll_hz = 100, .flush_interval = 40ms});
        std::this_thread::sleep_for(60ms);
        src->close();
        assert(src->detected_cores() == 1);
        const auto parsed = FrameReader().feed(drain_until_eof(*src));
        assert(!parsed.frames.empty());
        const std::string rpc = slurp(rpc_log);
        assert(contains(rpc, "target names\n"));
        assert(contains(rpc, "reg pc\n"));
        assert(!contains(rpc, "targets "));
    }

    fs::remove_all(root);
}

void error_disconnect_timeout_and_injection_checks() {
    ScopedEnv save_drop("FAKE_OPENOCD_DROP_AFTER");
    ScopedEnv save_batch("FAKE_OPENOCD_BATCH_REPLY");
    ScopedEnv save_targets("FAKE_OPENOCD_TARGETS");
    ScopedEnv save_type("FAKE_OPENOCD_TARGET_TYPE");

    const auto root = fresh_dir("malgrind_ocd_src_err");
    const auto install = fake_openocd_install(root / "ocd");

    // 1. Malformed batch reply -> error recorded and EOF
    {
        os::set_env("FAKE_OPENOCD_TARGETS", "stm32f4x.cpu");
        os::set_env("FAKE_OPENOCD_TARGET_TYPE", "cortex_m");
        os::set_env("FAKE_OPENOCD_BATCH_REPLY", "Error: bad target state");
        auto src = OpenOcdSource::start(install, {}, {.sample_mode = ProbeSampleMode::DwtPcsr, .flush_interval = 40ms});
        const auto bytes = drain_until_eof(*src);
        assert(!src->last_error().empty());
        assert(contains(src->last_error(), "unexpected OpenOCD DWT PCSR reply"));
        // Hello was pushed before the first failed batch; stream remains valid MLC1 + clean EOF.
        const auto parsed = FrameReader().feed(bytes);
        assert(parsed.messages.size() == 1 && std::holds_alternative<Hello>(parsed.messages[0]));
    }

    // 2. Unexpected mid-run OpenOCD crash/disconnect -> drains queued bytes, records error, returns EOF
    {
        os::unset_env("FAKE_OPENOCD_BATCH_REPLY");
        os::set_env("FAKE_OPENOCD_DROP_AFTER", "6"); // succeeds setup + several batches, then exits 2
        auto src = OpenOcdSource::start(install, {},
                                        {.sample_mode = ProbeSampleMode::DwtPcsr,
                                         .dwt_batch_size = 4,
                                         .flush_interval = 20ms});
        const auto bytes = drain_until_eof(*src, 3000ms);
        assert(!src->last_error().empty());
        const auto parsed = FrameReader().feed(bytes);
        assert(parsed.messages.size() == 1);
        assert(!parsed.frames.empty()); // partial samples collected before crash were flushed before EOF
    }

    // 3. Mock TcpListener: RPC timeout & unsafe target name injection guard
    {
        os::unset_env("FAKE_OPENOCD_DROP_AFTER");
        auto listener = os::TcpListener::bind_loopback(0);
        assert(listener.has_value());
        const std::uint16_t port = listener->port();

        std::atomic<bool> saw_injected_targets{false};
        std::thread server([&] {
            auto conn = listener->accept(2000);
            assert(conn.has_value());
            TclRpcCodec codec;
            std::array<char, 512> buf{};
            for (;;) {
                const auto n = conn->read(buf, 2000);
                if (!n || *n == 0) break;
                for (auto cmd = codec.feed(std::string_view(buf.data(), *n)); cmd; cmd = codec.feed({})) {
                    if (*cmd == "target names") {
                        (void)conn->write_all(TclRpcCodec::request("cpu0;shutdown cpu1"));
                    } else if (cmd->starts_with("targets ")) {
                        saw_injected_targets = true;
                        (void)conn->write_all(TclRpcCodec::request(""));
                    } else if (*cmd == "shutdown") {
                        (void)conn->write_all(TclRpcCodec::request(""));
                        return;
                    } else {
                        (void)conn->write_all(TclRpcCodec::request(""));
                    }
                }
            }
        });

        {
            OpenOcd ocd = OpenOcd::connect(port, 1000);
            OpenOcdSource src(std::move(ocd), {.sample_mode = ProbeSampleMode::HaltResume, .rpc_timeout_ms = 200});
            const auto bytes = drain_until_eof(src);
            assert(bytes.empty());
            assert(contains(src.last_error(), "unsafe target names"));
            assert(!saw_injected_targets);
        }
        server.join();
    }

    {
        auto listener = os::TcpListener::bind_loopback(0);
        assert(listener.has_value());
        const std::uint16_t port = listener->port();
        std::thread server([&] {
            auto conn = listener->accept(2000);
            assert(conn.has_value());
            TclRpcCodec codec;
            std::array<char, 512> buf{};
            for (;;) {
                const auto n = conn->read(buf, 500);
                if (!n) break;
                if (*n == 0) continue;
                for (auto cmd = codec.feed(std::string_view(buf.data(), *n)); cmd; cmd = codec.feed({})) {
                    if (*cmd == "shutdown") {
                        (void)conn->write_all(TclRpcCodec::request(""));
                        return;
                    }
                    // Intentionally hang on target names to trigger RPC timeout.
                }
            }
        });

        {
            OpenOcd ocd = OpenOcd::connect(port, 1000);
            OpenOcdSource src(std::move(ocd), {.sample_mode = ProbeSampleMode::HaltResume, .rpc_timeout_ms = 50});
            const auto bytes = drain_until_eof(src, 2000ms);
            assert(bytes.empty());
            assert(contains(src.last_error(), "timed out"));
        }
        server.join();
    }

    // 4. Clean destructor shutdown while actively sampling (must wake quickly and not wait full flush interval)
    {
        os::set_env("FAKE_OPENOCD_TARGETS", "stm32f4x.cpu");
        os::set_env("FAKE_OPENOCD_TARGET_TYPE", "cortex_m");
        const auto t0 = std::chrono::steady_clock::now();
        {
            auto src = OpenOcdSource::start(install, {},
                                            {.sample_mode = ProbeSampleMode::HaltResume,
                                             .poll_hz = 2,
                                             .flush_interval = 2000ms});
            // Ensure Hello arrived so sampler thread is inside sleep_until on a 500ms tick.
            assert(!read_at_least(*src, 10, 1500ms).empty());
        }
        assert(std::chrono::steady_clock::now() - t0 < 1200ms);
    }

    fs::remove_all(root);
}

} // namespace

int main() {
    parser_and_symbolizer_checks();
    dwt_pcsr_end_to_end_checks();
    halt_resume_dual_core_and_auto_fallback_checks();
    error_disconnect_timeout_and_injection_checks();
    std::puts("openocd_source ok");
}
