// OpenOCD discovery, TCL RPC framing codec, mock TcpListener RPC, and end-to-end process lifecycle checks.
#include "env.hpp"
#include "openocd.hpp"
#include "os/paths.hpp"
#include "os/socket.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace malgrind::host;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

bool contains(std::string_view s, std::string_view part) { return s.find(part) != std::string_view::npos; }

fs::path fresh_dir(const char* name) {
    const auto p = fs::canonical(fs::temp_directory_path()) / name;
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

void tcl_rpc_codec_checks() {
    assert(TclRpcCodec::request("version") == "version\x1a");
    assert(TclRpcCodec::request("") == "\x1a");

    TclRpcCodec codec;
    assert(!codec.feed("0x12").has_value());
    assert(!codec.feed("34").has_value());
    assert(codec.feed("\x1a") == std::optional<std::string>{"0x1234"});

    // Empty reply terminated by \x1a must produce empty string (not nullopt).
    assert(codec.feed("\x1a") == std::optional<std::string>{""});

    // Multi-frame buffer & multiline reply.
    assert(codec.feed("line1\nline2\x1asecond\x1apartial") == std::optional<std::string>{"line1\nline2"});
    assert(codec.feed("") == std::optional<std::string>{"second"});
    assert(!codec.feed("").has_value());
    assert(codec.feed("\x1a") == std::optional<std::string>{"partial"});
}

void find_openocd_checks() {
    ScopedEnv save_path("PATH", "");
    ScopedEnv save_core("PLATFORMIO_CORE_DIR", std::nullopt);
    ScopedEnv save_drive("SystemDrive", std::nullopt);

    const auto root = fresh_dir("malgrind_openocd_find");
    const auto home = root / "home";
    fs::create_directories(home);

    // Nothing installed anywhere.
    assert(!find_openocd(home).has_value());

    // 1. PlatformIO package under home/.platformio/packages/tool-openocd with scripts at depth 1.
    const auto pkg = home / ".platformio/packages/tool-openocd";
    const auto pkg_exe = pkg / "bin" / os::exe_name("openocd");
    const auto pkg_scripts = pkg / "scripts";
    write_file(pkg_exe);
    write_file(pkg_scripts / "interface/stlink.cfg");
    auto found = find_openocd(home);
    assert(found.has_value());
    assert(found->exe == pkg_exe);
    assert(found->scripts == pkg_scripts);

    // 2. Depth > 3 scripts directory is ignored; depth 3 (share/openocd/scripts) is found.
    fs::remove_all(pkg_scripts);
    write_file(pkg / "a/b/c/d/interface/stlink.cfg");
    found = find_openocd(home);
    assert(found.has_value());
    assert(found->exe == pkg_exe);
    assert(found->scripts.empty());

    const auto depth3_scripts = pkg / "share/openocd/scripts";
    write_file(depth3_scripts / "interface/stlink.cfg");
    found = find_openocd(home);
    assert(found.has_value());
    assert(found->scripts == depth3_scripts);

    // 3. Explicit package and tool-openocd* fallback under PLATFORMIO_CORE_DIR.
    const auto custom_core = root / "custom_core";
    const auto alt_pkg = custom_core / "packages/tool-openocd-alt";
    const auto alt_exe = alt_pkg / "bin" / os::exe_name("openocd");
    const auto alt_scripts = alt_pkg / "openocd/scripts";
    write_file(alt_exe);
    write_file(alt_scripts / "interface/stlink.cfg");
    os::set_env("PLATFORMIO_CORE_DIR", os::to_utf8(custom_core));

    // Default package "tool-openocd" falls back to tool-openocd* in custom_core before home/.platformio.
    found = find_openocd(home);
    assert(found.has_value());
    assert(found->exe == alt_exe);
    assert(found->scripts == alt_scripts);

    // Explicit non-matching package does not match tool-openocd*.
    assert(!find_openocd(home, "tool-openocd-missing").has_value());
    found = find_openocd(home, "tool-openocd-alt");
    assert(found.has_value() && found->exe == alt_exe);

    // 4. PATH takes precedence over default "tool-openocd", while explicit packages still prefer packages/<package>.
    const auto path_prefix = root / "sys";
    const auto path_exe = path_prefix / "bin" / os::exe_name("openocd");
    const auto path_scripts = path_prefix / "share/openocd/scripts";
    write_file(path_exe);
    if constexpr (os::kind() != os::Kind::Windows) {
        fs::permissions(path_exe, fs::perms::owner_all);
    }
    write_file(path_scripts / "interface/stlink.cfg");
    os::set_env("PATH", os::to_utf8(path_prefix / "bin"));

    found = find_openocd(home);
    assert(found.has_value());
    assert(found->exe == path_exe);
    assert(found->scripts == path_scripts);

    found = find_openocd(home, "tool-openocd-alt");
    assert(found.has_value());
    assert(found->exe == alt_exe);
    assert(found->scripts == alt_scripts);

    // Versioned explicit package also takes precedence over PATH, and falls back to PATH if missing.
    const auto esp_pkg = custom_core / "packages/tool-openocd-esp32@20241016";
    const auto esp_exe = esp_pkg / "bin" / os::exe_name("openocd");
    const auto esp_scripts = esp_pkg / "share/openocd/scripts";
    write_file(esp_exe);
    write_file(esp_scripts / "interface/esp_usb_jtag.cfg");
    write_file(esp_scripts / "target/esp32s3.cfg");

    found = find_openocd(home, "tool-openocd-esp32");
    assert(found.has_value());
    assert(found->exe == esp_exe);
    assert(found->scripts == esp_scripts);

    found = find_openocd(home, "tool-openocd-missing");
    assert(found.has_value());
    assert(found->exe == path_exe);

    fs::remove_all(root);
}

void mock_listener_connect_and_call_checks() {
    // Port 0 and unbound port fail cleanly.
    auto temp_listener = os::TcpListener::bind_loopback(0);
    assert(temp_listener.has_value());
    const std::uint16_t unbound_port = temp_listener->port();
    temp_listener.reset();
    for (std::uint16_t bad_port : {std::uint16_t{0}, unbound_port}) {
        bool threw = false;
        try {
            (void)OpenOcd::connect(bad_port, 100);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        assert(threw);
    }

    auto listener = os::TcpListener::bind_loopback(0);
    assert(listener.has_value());
    const std::uint16_t port = listener->port();

    bool saw_shutdown = false;
    std::thread server([&] {
        auto conn = listener->accept(2000);
        assert(conn.has_value());
        TclRpcCodec codec;
        std::array<char, 512> buf{};
        for (;;) {
            const auto n = conn->read(buf, 2000);
            if (!n) break;
            assert(*n > 0);
            for (auto cmd = codec.feed(std::string_view(buf.data(), *n)); cmd; cmd = codec.feed({})) {
                if (*cmd == "ping") {
                    assert(conn->write_all("po"));
                    std::this_thread::sleep_for(15ms);
                    assert(conn->write_all("ng\x1a"));
                } else if (*cmd == "empty") {
                    assert(conn->write_all("\x1a"));
                } else if (*cmd == "slow") {
                    // Intentionally do not answer so client times out.
                } else if (*cmd == "after_timeout") {
                    assert(conn->write_all("recovered\x1a"));
                } else if (*cmd == "shutdown") {
                    saw_shutdown = true;
                    (void)conn->write_all("\x1a");
                    return;
                }
            }
        }
    });

    {
        OpenOcd ocd = OpenOcd::connect(port, 1000);
        assert(ocd.port() == port);
        assert(!ocd.running()); // external connection has no owned child process
        assert(ocd.call("ping", 1000) == "pong");
        assert(ocd.call("empty", 1000).empty());

        // Timeout throws without breaking subsequent calls or shutdown.
        bool timed_out = false;
        try {
            (void)ocd.call("slow", 40);
        } catch (const std::runtime_error& e) {
            timed_out = contains(e.what(), "timed out");
        }
        assert(timed_out);
        assert(ocd.call("after_timeout", 1000) == "recovered");

        // Embedded delimiter rejected.
        bool bad_arg = false;
        try {
            (void)ocd.call("bad\x1a" "cmd", 100);
        } catch (const std::invalid_argument&) {
            bad_arg = true;
        }
        assert(bad_arg);
    }

    server.join();
    assert(saw_shutdown);

    // Closed server mid-call throws cleanly.
    auto listener2 = os::TcpListener::bind_loopback(0);
    assert(listener2.has_value());
    const std::uint16_t port2 = listener2->port();
    std::thread server2([&] {
        auto conn = listener2->accept(2000);
        assert(conn.has_value());
        std::array<char, 64> buf{};
        (void)conn->read(buf, 1000);
        // Close immediately without replying.
    });
    {
        OpenOcd ocd = OpenOcd::connect(port2, 1000);
        bool threw = false;
        try {
            (void)ocd.call("ping", 1000);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        assert(threw);
    }
    server2.join();
}

void fake_tool_end_to_end_checks() {
    ScopedEnv save_log("FAKE_LOG");
    ScopedEnv save_fail("FAKE_FAIL", std::nullopt);
    ScopedEnv save_openocd_fail("FAKE_OPENOCD_FAIL", std::nullopt);
    ScopedEnv save_sleep("FAKE_OPENOCD_STARTUP_SLEEP_MS", std::nullopt);
    ScopedEnv save_ignore("FAKE_OPENOCD_IGNORE_SHUTDOWN", std::nullopt);

    const auto root = fresh_dir("malgrind_openocd_e2e");
    const auto pkg = root / "pkg";
    const auto exe = pkg / "bin" / os::exe_name("openocd");
    const auto scripts = pkg / "scripts";
    fs::create_directories(exe.parent_path());
    fs::copy_file(MALGRIND_FAKE_TOOL, exe, fs::copy_options::overwrite_existing);
    if constexpr (os::kind() != os::Kind::Windows) {
        fs::permissions(exe, fs::perms::owner_all);
    }
    write_file(scripts / "interface/stlink.cfg");

    const OpenOcdInstall install{exe, scripts};
    const fs::path log_file = root / "fake.log";
    os::set_env("FAKE_LOG", os::to_utf8(log_file));

    // 1. Happy path: start -> call -> graceful ~OpenOcd shutdown.
    {
        std::mutex mu;
        std::vector<std::string> captured;
        const auto t0 = std::chrono::steady_clock::now();
        {
            OpenOcd ocd = OpenOcd::start(install, {"-f", "interface/stlink.cfg", "-f", "target/stm32f4x.cfg"},
                                         [&](std::string_view line) {
                                             std::scoped_lock lock(mu);
                                             captured.emplace_back(line);
                                         });
            assert(ocd.port() != 0);
            assert(ocd.running());
            assert(ocd.call("version") == "Open On-Chip Debugger 0.12.0");
            assert(ocd.call("echo hello world") == "hello world");
            assert(ocd.call("mrw 0xE000101C") == "0x08001234");

            bool timed_out = false;
            try {
                (void)ocd.call("sleep_ms 200", 30);
            } catch (const std::runtime_error& e) {
                timed_out = contains(e.what(), "timed out");
            }
            assert(timed_out);
            assert(!ocd.recent_lines().empty());
        }
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        assert(elapsed < 1500ms); // graceful shutdown exited immediately, did not wait 2 s for kill

        std::scoped_lock lock(mu);
        assert(!captured.empty());
        const std::string cmdlog = slurp(log_file);
        assert(contains(cmdlog, "-s " + os::to_utf8(scripts)));
        assert(contains(cmdlog, "-f interface/stlink.cfg -f target/stm32f4x.cfg"));
        assert(contains(cmdlog, "gdb_port disabled"));
        assert(contains(cmdlog, "telnet_port disabled"));
        assert(contains(cmdlog, "tcl_port "));
    }

    // 2. Startup failure throws with OpenOCD's last lines.
    {
        os::set_env("FAKE_OPENOCD_FAIL", "startup");
        bool threw = false;
        try {
            (void)OpenOcd::start(install, {}, nullptr);
        } catch (const std::runtime_error& e) {
            threw = contains(e.what(), "unable to open debug probe");
        }
        assert(threw);
        os::unset_env("FAKE_OPENOCD_FAIL");
    }

    // 3. Startup timeout throws and kills lingering child.
    {
        os::set_env("FAKE_OPENOCD_STARTUP_SLEEP_MS", "2000");
        const auto t0 = std::chrono::steady_clock::now();
        bool threw = false;
        try {
            (void)OpenOcd::start(install, {}, nullptr, 100);
        } catch (const std::runtime_error& e) {
            threw = contains(e.what(), "timed out");
        }
        assert(threw);
        assert(std::chrono::steady_clock::now() - t0 < 1500ms);
        os::unset_env("FAKE_OPENOCD_STARTUP_SLEEP_MS");
    }

    // 4. Child crashing mid-call throws with exit info and recent lines.
    {
        OpenOcd ocd = OpenOcd::start(install, {}, nullptr);
        bool threw = false;
        try {
            (void)ocd.call("drop", 1000);
        } catch (const std::runtime_error& e) {
            threw = contains(e.what(), "exit code 2") && contains(e.what(), "Open On-Chip Debugger");
        }
        assert(threw);
        assert(!ocd.running());
    }

    // 5. Ignoring "shutdown" triggers ~OpenOcd ~2000 ms wait + fallback kill().
    {
        os::set_env("FAKE_OPENOCD_IGNORE_SHUTDOWN", "1");
        const auto t0 = std::chrono::steady_clock::now();
        {
            OpenOcd ocd = OpenOcd::start(install, {}, nullptr);
            assert(ocd.call("version") == "Open On-Chip Debugger 0.12.0");
        }
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        assert(elapsed >= 1800ms && elapsed < 4000ms);
        os::unset_env("FAKE_OPENOCD_IGNORE_SHUTDOWN");
    }

    fs::remove_all(root);
}

} // namespace

int main() {
    tcl_rpc_codec_checks();
    find_openocd_checks();
    mock_listener_connect_and_call_checks();
    fake_tool_end_to_end_checks();
    std::puts("openocd ok");
}
