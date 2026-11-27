// OS process spawning, MSVCRT quoting, PATH lookup and fd non-leak self-check.
#include "os/paths.hpp"
#include "os/process.hpp"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace malgrind::host;
namespace fs = std::filesystem;

namespace {

class ScopedEnv {
public:
    explicit ScopedEnv(std::string name) : name_(std::move(name)), saved_(os::get_env(name_)) {}
    ~ScopedEnv() {
        if (saved_) os::set_env(name_, *saved_);
        else os::unset_env(name_);
    }

private:
    std::string name_;
    std::optional<std::string> saved_;
};

fs::path fresh_dir(const char* name) {
    const auto p = fs::canonical(fs::temp_directory_path()) / name;
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

void windows_command_line_checks() {
    assert(os::windows_command_line({}).empty());
    assert(os::windows_command_line({""}) == L"\"\"");
    assert(os::windows_command_line({"plain", "abc/def"}) == L"plain abc/def");
    assert(os::windows_command_line({"a b", "c\"d", "e\\\\"}) == L"\"a b\" \"c\\\"d\" e\\\\");
    assert(os::windows_command_line({"a b\\", "x\\\"y", "tab\there"}) == L"\"a b\\\\\" \"x\\\\\\\"y\" \"tab\there\"");
    assert(os::windows_command_line({"\xd0\xbf\xd1\x80\xd0\xbe\xd1\x94\xd0\xba\xd1\x82 \xc3\xbc"}) == L"\"\u043f\u0440\u043e\u0454\u043a\u0442 \u00fc\"");
}

void run_lines_checks() {
    std::vector<std::string> lines;
    const int code = os::run_lines(
        {MALGRIND_HELPER_ECHO, "a b", "c\"d", "e\\\\", "\xd0\xbf\xd1\x80\xd0\xbe\xd1\x94\xd0\xba\xd1\x82 \xc3\xbc"},
        {"HELPER_VAR=hello world"},
        [&](std::string_view line) { lines.emplace_back(line); });
    assert(code == 0);
    assert((lines == std::vector<std::string>{"a b", "c\"d", "e\\\\", "\xd0\xbf\xd1\x80\xd0\xbe\xd1\x94\xd0\xba\xd1\x82 \xc3\xbc", "HELPER_VAR=hello world"}));

    // Also verify stderr merging, \r line splitting, and non-zero exit code without a shell.
    std::vector<std::string> streamed;
    const int exit_code = os::run_lines(
        {MALGRIND_HELPER_ECHO, "first"},
        {"HELPER_VAR=v2", "HELPER_RAW=p1\rp2\n", "HELPER_ERR=err line", "HELPER_EXIT=7"},
        [&](std::string_view line) { streamed.emplace_back(line); });
    assert(exit_code == 7);
    assert((streamed == std::vector<std::string>{"first", "HELPER_VAR=v2", "p1", "p2", "err line"}));

    assert(os::run_lines({}, {}, [](std::string_view) {}) == -1);
    assert(os::run_lines({"/no/such/malgrind_missing_program"}, {}, [](std::string_view) {}) != 0);
}

void capture_and_run_checks() {
    ScopedEnv save_err("HELPER_ERR");
    ScopedEnv save_exit("HELPER_EXIT");
    ScopedEnv save_var("HELPER_VAR");
    os::unset_env("HELPER_VAR");
    os::unset_env("HELPER_EXIT");
    os::set_env("HELPER_ERR", "discarded stderr");

    const std::string out = os::capture({MALGRIND_HELPER_ECHO, "", "a b", "c\"d", "e\\\\", "\xd0\xbf\xd1\x80\xd0\xbe\xd1\x94\xd0\xba\xd1\x82 \xc3\xbc"});
    assert(out == "\na b\nc\"d\ne\\\\\n\xd0\xbf\xd1\x80\xd0\xbe\xd1\x94\xd0\xba\xd1\x82 \xc3\xbc\nHELPER_VAR=\n");

    os::unset_env("HELPER_ERR");
    assert(os::capture({}).empty());
    assert(os::capture({"/no/such/malgrind_missing_program"}).empty());

    assert(os::run({MALGRIND_HELPER_ECHO, "run-ok"}) == 0);
    os::set_env("HELPER_EXIT", "7");
    assert(os::run({MALGRIND_HELPER_ECHO, "run-exit"}) == 7);
    os::unset_env("HELPER_EXIT");

    assert(os::run({}) == -1);
    assert(os::run({"/no/such/malgrind_missing_program"}) != 0);
}

void find_in_path_and_batch_checks() {
    ScopedEnv save_path("PATH");
    const auto dir = fresh_dir("malgrind_os_process_path");

    // dir1/mytool is a directory (must be rejected), dir2/mytool is non-executable on POSIX, dir3/mytool[.exe] is valid.
    fs::create_directories(dir / "dir1/mytool");
    fs::create_directories(dir / "dir2");
    fs::create_directories(dir / "dir3");
    {
        std::ofstream(dir / "dir2/mytool") << "not executable\n";
        std::ofstream(dir / "dir3" / os::exe_name("mytool")) << "#!/bin/sh\n";
    }
    if constexpr (os::kind() != os::Kind::Windows) {
        fs::permissions(dir / "dir2/mytool", fs::perms::owner_read | fs::perms::owner_write);
        fs::permissions(dir / "dir3/mytool", fs::perms::owner_all);
    }

    const std::string sep(1, os::kPathListSep);
    os::set_env("PATH", os::to_utf8(dir / "dir1") + sep + os::to_utf8(dir / "dir2") + sep + os::to_utf8(dir / "dir3"));

    assert(os::find_in_path("mytool") == dir / "dir3" / os::exe_name("mytool"));
    assert(!os::find_in_path("malgrind_missing_tool").has_value());
    assert(!os::find_in_path("").has_value());

    if constexpr (os::kind() == os::Kind::Windows) {
        {
            std::ofstream(dir / "dir1/onlybat.bat") << "@echo off\n";
            std::ofstream(dir / "dir1/onlycmd.CMD") << "@echo off\n";
        }
        assert(!os::find_in_path("onlybat").has_value());
        assert(!os::find_in_path("onlybat.bat").has_value());
        assert(!os::find_in_path("onlycmd").has_value());
        assert(!os::find_in_path("onlycmd.cmd").has_value());
        assert(os::run({os::to_utf8(dir / "dir1/onlybat.bat")}) == -1);
        assert(os::run_lines({os::to_utf8(dir / "dir1/onlycmd.CMD")}, {}, [](std::string_view) {}) == -1);
        assert(os::capture({os::to_utf8(dir / "dir1/onlybat.bat")}).empty());
    }

    fs::remove_all(dir);
}

void child_checks() {
    using namespace std::chrono_literals;

    // Default / invalid / missing / empty-command Child
    {
        os::Child empty;
        assert(!empty.valid());
        assert(!empty.running());
        assert(!empty.wait_for(0).has_value());
        assert(empty.wait() == -1);
        empty.kill();
        assert(!os::Child::spawn({}, {}, nullptr).has_value());
        assert(!os::Child::spawn({"/no/such/malgrind_missing_program"}, {}, nullptr).has_value());
    }

    // Live streaming before exit, wait_for timeout, move, exit code, and null callback safety
    {
        std::mutex mu;
        std::condition_variable cv;
        std::vector<std::string> lines;
        auto child = os::Child::spawn(
            {MALGRIND_HELPER_ECHO, "line1", "--sleep-ms", "150", "line2"},
            {"HELPER_VAR=vchild", "HELPER_ERR=errline", "HELPER_EXIT=5"},
            [&](std::string_view line) {
                std::scoped_lock lock(mu);
                lines.emplace_back(line);
                cv.notify_all();
            });
        assert(child.has_value());
        assert(child->valid());

        {
            std::unique_lock lock(mu);
            const bool got_first = cv.wait_for(lock, 1000ms, [&] { return !lines.empty(); });
            assert(got_first);
            assert(lines[0] == "line1");
        }
        assert(child->running());
        assert(!child->wait_for(10).has_value());

        os::Child moved = std::move(*child);
        assert(!child->valid());
        assert(moved.valid());
        assert(moved.running());

        const auto code = moved.wait_for(2000);
        assert(code.has_value() && *code == 5);
        assert(!moved.running());
        assert(moved.wait() == 5);
        moved.kill(); // no-op after exit
        assert((lines == std::vector<std::string>{"line1", "line2", "HELPER_VAR=vchild", "errline"}));
    }

    // Null on_line callback drains output safely without crashing
    {
        auto child = os::Child::spawn({MALGRIND_HELPER_ECHO, "ignored"}, {}, nullptr);
        assert(child.has_value());
        assert(child->wait() == 0);
    }

    // kill() terminates a long-sleeping child promptly
    {
        std::mutex mu;
        std::condition_variable cv;
        bool ready = false;
        auto child = os::Child::spawn({MALGRIND_HELPER_ECHO, "ready", "--sleep-ms", "10000"}, {}, [&](std::string_view l) {
            if (l == "ready") {
                std::scoped_lock lock(mu);
                ready = true;
                cv.notify_all();
            }
        });
        assert(child.has_value());
        {
            std::unique_lock lock(mu);
            assert(cv.wait_for(lock, 1000ms, [&] { return ready; }));
        }
        assert(child->running());
        const auto t0 = std::chrono::steady_clock::now();
        child->kill();
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        assert(!child->running());
        assert(child->wait() != 0);
        assert(elapsed < 2000ms);
    }

    // POSIX: SIGTERM ignored -> kill() escalates to SIGKILL after ~200 ms
    if constexpr (os::kind() != os::Kind::Windows) {
        std::mutex mu;
        std::condition_variable cv;
        bool armed = false;
        auto child = os::Child::spawn(
            {MALGRIND_HELPER_ECHO, "--ignore-term", "armed", "--sleep-ms", "10000"}, {}, [&](std::string_view l) {
                if (l == "armed") {
                    std::scoped_lock lock(mu);
                    armed = true;
                    cv.notify_all();
                }
            });
        assert(child.has_value());
        {
            std::unique_lock lock(mu);
            assert(cv.wait_for(lock, 1000ms, [&] { return armed; }));
        }
        const auto t0 = std::chrono::steady_clock::now();
        child->kill();
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        assert(!child->running());
        assert(child->wait() == -1);
        assert(elapsed >= 150ms && elapsed < 2000ms);
    }

    // Destructor and move-assignment kill still-running children cleanly
    {
        auto c1 = os::Child::spawn({MALGRIND_HELPER_ECHO, "--sleep-ms", "10000"}, {}, nullptr);
        auto c2 = os::Child::spawn({MALGRIND_HELPER_ECHO, "--sleep-ms", "10000"}, {}, nullptr);
        assert(c1.has_value() && c2.has_value());
        *c1 = std::move(*c2);
        assert(!c2->valid());
        assert(c1->valid());
    }
}

void fd_non_leak_checks() {
#ifndef _WIN32
    const int before = ::open("/dev/null", O_RDONLY);
    assert(before >= 0);
    ::close(before);

    for (int i = 0; i < 10; ++i) {
        assert(!os::capture({MALGRIND_HELPER_ECHO, "fd"}).empty());
        assert(os::capture({"/no/such/malgrind_missing_program"}).empty());
        assert(os::run_lines({MALGRIND_HELPER_ECHO, "fd"}, {"HELPER_VAR=ok"}, [](std::string_view) {}) == 0);
        assert(os::run_lines({"/no/such/malgrind_missing_program"}, {}, [](std::string_view) {}) != 0);
        auto child = os::Child::spawn({MALGRIND_HELPER_ECHO, "fd"}, {}, nullptr);
        assert(child.has_value() && child->wait() == 0);
        assert(!os::Child::spawn({"/no/such/malgrind_missing_program"}, {}, nullptr).has_value());
    }

    const int after = ::open("/dev/null", O_RDONLY);
    assert(after >= 0);
    ::close(after);
    assert(after == before);
#endif
}

} // namespace

int main() {
    windows_command_line_checks();
    run_lines_checks();
    capture_and_run_checks();
    find_in_path_and_batch_checks();
    child_checks();
    fd_non_leak_checks();
    std::puts("os_process ok");
}
