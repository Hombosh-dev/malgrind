// OS serial port abstraction self-check: exclusive open, raw I/O, timeouts, hangup, and error mapping.
#include "ops.hpp"
#include "os/paths.hpp"
#include "os/serial.hpp"
#include "source.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#ifndef _WIN32
#include "pty.hpp"
#include <sys/select.h>
#include <unistd.h>
#endif

using namespace malgrind::host;

namespace {

void open_error_formatting_checks() {
    const auto busy = open_error("portA", {os::OpenError::Busy, false, "in use"});
    assert(std::string_view(busy.what()).find("busy:") != std::string_view::npos);
    assert(std::string_view(busy.what()).find("portA") != std::string_view::npos);

    const auto denied = open_error("portB", {os::OpenError::Denied, false, "permission denied"});
    assert(std::string_view(denied.what()).find("Grant access") != std::string_view::npos);
    assert(std::string_view(denied.what()).find("portB") != std::string_view::npos);

    const auto missing = open_error("portC", {os::OpenError::NotFound, true, "no such file"});
    assert(std::string_view(missing.what()) == "cannot open portC: no such file");
}

void source_stdin_roundtrip() {
    auto* orig = std::cin.rdbuf();
    std::stringbuf input("hello-stdin");
    std::cin.rdbuf(&input);

    auto res = Source::open_result("-", 921600);
    assert(std::holds_alternative<Source>(res));
    auto src = std::get<Source>(std::move(res));
    assert(!src.write({}));

    std::string collected;
    std::array<std::uint8_t, 32> buf{};
    for (;;) {
        const auto n = src.read(buf, 1000);
        if (!n) break;
        collected.append(reinterpret_cast<const char*>(buf.data()), *n);
    }
    assert(collected == "hello-stdin");
    std::cin.rdbuf(orig);
}

#ifndef _WIN32

std::vector<std::uint8_t> read_exact_from_master(int fd, std::size_t expected) {
    std::vector<std::uint8_t> out;
    while (out.size() < expected) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(fd, &readable);
        timeval timeout{1, 0};
        const int ready = ::select(fd + 1, &readable, nullptr, nullptr, &timeout);
        assert(ready > 0);
        std::array<std::uint8_t, 256> buf{};
        const ssize_t n = ::read(fd, buf.data(), std::min(buf.size(), expected - out.size()));
        assert(n > 0);
        out.insert(out.end(), buf.begin(), buf.begin() + n);
    }
    return out;
}

std::vector<std::uint8_t> read_exact_from_serial(os::SerialPort& port, std::size_t expected) {
    std::vector<std::uint8_t> out;
    while (out.size() < expected) {
        std::array<std::uint8_t, 256> buf{};
        const auto n = port.read(buf, 1000);
        assert(n.has_value());
        assert(*n > 0);
        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n));
    }
    return out;
}

void pty_roundtrip_timeout_move_and_hangup() {
    Pty pty;
    auto res = os::SerialPort::open(pty.slave, 921600);
    assert(std::holds_alternative<os::SerialPort>(res));
    auto port = std::get<os::SerialPort>(std::move(res));

    // Nothing sent yet -> timeout returns 0 (not nullopt).
    std::array<std::uint8_t, 32> buf{};
    const auto timed_out = port.read(buf, 20);
    assert(timed_out.has_value() && *timed_out == 0);

    // Move construction and move assignment transfer ownership cleanly.
    os::SerialPort moved(std::move(port));
    os::SerialPort target = std::move(moved);

    // Host -> device write.
    const std::vector<std::uint8_t> tx = {0x01, 0x02, 'h', 'i', 0x00, 0xFF};
    assert(target.write(tx));
    assert(read_exact_from_master(pty.master, tx.size()) == tx);

    // Device -> host read.
    const std::string rx = "hello-serial\n";
    pty.send(rx);
    const auto got = read_exact_from_serial(target, rx.size());
    assert(std::string(got.begin(), got.end()) == rx);

    // Closing the peer returns nullopt on read.
    pty.hang_up();
    const auto after_hangup = target.read(buf, 1000);
    assert(!after_hangup.has_value());
}

void exclusive_open_returns_busy() {
    if (!exclusive_open_is_enforced()) {
        std::puts("exclusive_open_returns_busy: skipped, this platform does not enforce TIOCEXCL on a pty");
        return;
    }
    Pty pty;
    auto first = os::SerialPort::open(pty.slave, 921600);
    assert(std::holds_alternative<os::SerialPort>(first));

    auto second = os::SerialPort::open(pty.slave, 921600);
    assert(std::holds_alternative<os::OpenFailure>(second));
    const auto& failure = std::get<os::OpenFailure>(second);
    assert(failure.kind == os::OpenError::Busy);
    assert(!failure.transient);
    assert(!failure.detail.empty());
}

void error_cases_posix() {
    const std::string missing_path = "/tmp/malgrind_nonexistent_serial_port_99999";
    auto missing = os::SerialPort::open(missing_path, 921600);
    assert(std::holds_alternative<os::OpenFailure>(missing));
    const auto& mf = std::get<os::OpenFailure>(missing);
    assert(mf.kind == os::OpenError::NotFound);
    assert(mf.transient);
    assert(!mf.detail.empty());

    auto bad0 = os::SerialPort::open(missing_path, 0);
    assert(std::holds_alternative<os::OpenFailure>(bad0));
    assert(std::get<os::OpenFailure>(bad0).kind == os::OpenError::BadBaud);
    assert(!std::get<os::OpenFailure>(bad0).transient);

    if constexpr (os::kind() == os::Kind::Linux) {
        auto bad_linux = os::SerialPort::open(missing_path, 12345);
        assert(std::holds_alternative<os::OpenFailure>(bad_linux));
        assert(std::get<os::OpenFailure>(bad_linux).kind == os::OpenError::BadBaud);
    }

    // Source::open_result wrapper check.
    Pty pty;
    auto src_res = Source::open_result(pty.slave, 921600);
    assert(std::holds_alternative<Source>(src_res));
    auto src = std::get<Source>(std::move(src_res));
    const std::vector<std::uint8_t> ping = {'o', 'k'};
    assert(src.write(ping));
    assert(read_exact_from_master(pty.master, ping.size()) == ping);
    pty.send("pong");
    std::array<std::uint8_t, 16> rbuf{};
    std::string received;
    while (received.size() < 4) {
        const auto n = src.read(rbuf, 1000);
        assert(n.has_value() && *n > 0);
        received.append(reinterpret_cast<const char*>(rbuf.data()), *n);
    }
    assert(received == "pong");

    auto src_missing = Source::open_result(missing_path, 921600);
    assert(std::holds_alternative<os::OpenFailure>(src_missing));
    assert(std::get<os::OpenFailure>(src_missing).kind == os::OpenError::NotFound);
    assert(std::get<os::OpenFailure>(src_missing).transient);

    bool threw_missing = false;
    try {
        (void)open_source(missing_path);
    } catch (const std::runtime_error& e) {
        threw_missing = std::string_view(e.what()).starts_with("cannot open ");
    }
    assert(threw_missing);

    if (exclusive_open_is_enforced()) {
        Pty held;
        auto holder = os::SerialPort::open(held.slave, 921600);
        assert(std::holds_alternative<os::SerialPort>(holder));
        assert(probe_port(held.slave).busy);
        bool threw_busy = false;
        try {
            (void)open_source(held.slave);
        } catch (const std::runtime_error& e) {
            threw_busy = std::string_view(e.what()).starts_with("busy:");
        }
        assert(threw_busy);
    }
}

#else

void windows_stub_checks() {
    auto missing = os::SerialPort::open("\\\\.\\NONEXISTENT_COM99", 921600);
    assert(std::holds_alternative<os::OpenFailure>(missing));
    const auto& mf = std::get<os::OpenFailure>(missing);
    assert(mf.kind == os::OpenError::NotFound);
    assert(mf.transient);
    assert(!mf.detail.empty());

    auto bad = os::SerialPort::open("COM99", 0);
    assert(std::holds_alternative<os::OpenFailure>(bad));
    assert(std::get<os::OpenFailure>(bad).kind == os::OpenError::BadBaud);
}

#endif

} // namespace

int main() {
    open_error_formatting_checks();
    source_stdin_roundtrip();
#ifndef _WIN32
    pty_roundtrip_timeout_move_and_hangup();
    exclusive_open_returns_busy();
    error_cases_posix();
#else
    windows_stub_checks();
#endif
    std::puts("os_serial ok");
}
