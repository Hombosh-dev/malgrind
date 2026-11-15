#pragma once

// Test helpers shared by the tests that need a serial port without a board.

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

// The "board": the master side of a pty. Code under test opens `slave` exactly as it opens /dev/ttyUSB0.
struct Pty {
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    std::string slave;
    Pty() {
        assert(master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0);
        slave = ptsname(master);
    }
    ~Pty() { hang_up(); }
    void send(const std::vector<std::uint8_t>& bytes) const {
        assert(write(master, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
    }
    void send(const std::string& text) const { send(std::vector<std::uint8_t>(text.begin(), text.end())); }
    void hang_up() {
        if (master >= 0) close(master), master = -1;
    }
};

template <class Pred>
bool wait_for(Pred pred, std::chrono::milliseconds limit = std::chrono::milliseconds(3000)) {
    using namespace std::chrono_literals;
    for (const auto end = std::chrono::steady_clock::now() + limit; std::chrono::steady_clock::now() < end;
         std::this_thread::sleep_for(10ms))
        if (pred()) return true;
    return false;
}
