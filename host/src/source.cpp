#include "source.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <streambuf>
#include <thread>
#include <utility>

namespace malgrind::host {

struct Source::StdinState {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::uint8_t> buf;
    bool eof = false;
    bool closed = false;

    static std::shared_ptr<StdinState> start() {
        auto state = std::make_shared<StdinState>();
        std::thread([state]() {
            std::streambuf* sb = std::cin.rdbuf();
            if (!sb) {
                std::scoped_lock lock(state->mutex);
                state->eof = true;
                state->cv.notify_all();
                return;
            }
            constexpr auto kEof = std::char_traits<char>::eof();
            for (;;) {
                {
                    std::scoped_lock lock(state->mutex);
                    if (state->closed) break;
                }
                const int ch = sb->sbumpc();
                if (ch == kEof) {
                    std::scoped_lock lock(state->mutex);
                    state->eof = true;
                    state->cv.notify_all();
                    break;
                }
                std::array<char, 4096> tmp;
                tmp[0] = static_cast<char>(ch);
                std::size_t count = 1;
                const std::streamsize avail = sb->in_avail();
                if (avail > 0) {
                    const std::streamsize extra = std::min<std::streamsize>(avail, static_cast<std::streamsize>(tmp.size() - 1));
                    const std::streamsize got = sb->sgetn(tmp.data() + 1, extra);
                    if (got > 0) count += static_cast<std::size_t>(got);
                }
                {
                    std::scoped_lock lock(state->mutex);
                    if (state->closed) break;
                    for (std::size_t i = 0; i < count; ++i) {
                        state->buf.push_back(static_cast<std::uint8_t>(tmp[i]));
                    }
                    state->cv.notify_one();
                }
            }
        }).detach();
        return state;
    }
};

std::variant<Source, os::OpenFailure> Source::open_result(const std::string& port, unsigned baud) {
    if (port == "-") return Source(StdinState::start());
    auto res = os::SerialPort::open(port, baud);
    if (auto* serial = std::get_if<os::SerialPort>(&res)) return Source(std::move(*serial));
    return std::get<os::OpenFailure>(std::move(res));
}

std::optional<Source> Source::open(const std::string& port, unsigned baud) {
    auto res = open_result(port, baud);
    if (auto* source = std::get_if<Source>(&res)) return std::move(*source);
    return std::nullopt;
}

Source::Source(Source&&) noexcept = default;

Source& Source::operator=(Source&& other) noexcept {
    if (this != &other) {
        if (auto* st = std::get_if<std::shared_ptr<StdinState>>(&impl_); st && *st) {
            std::scoped_lock lock((*st)->mutex);
            (*st)->closed = true;
            (*st)->cv.notify_all();
        }
        impl_ = std::move(other.impl_);
    }
    return *this;
}

Source::~Source() {
    if (auto* st = std::get_if<std::shared_ptr<StdinState>>(&impl_); st && *st) {
        std::scoped_lock lock((*st)->mutex);
        (*st)->closed = true;
        (*st)->cv.notify_all();
    }
}

std::optional<std::size_t> Source::read(std::span<std::uint8_t> out, int timeout_ms) {
    if (auto* serial = std::get_if<os::SerialPort>(&impl_)) {
        return serial->read(out, timeout_ms);
    }
    auto& state = std::get<std::shared_ptr<StdinState>>(impl_);
    if (!state) return std::nullopt;
    if (out.empty()) return std::size_t{0};

    std::unique_lock lock(state->mutex);
    if (state->buf.empty() && !state->eof) {
        const auto wait = std::chrono::milliseconds(std::max(0, timeout_ms));
        state->cv.wait_for(lock, wait, [&] { return !state->buf.empty() || state->eof; });
    }
    if (state->buf.empty()) {
        return state->eof ? std::nullopt : std::optional<std::size_t>(0);
    }
    const std::size_t n = std::min(out.size(), state->buf.size());
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = state->buf.front();
        state->buf.pop_front();
    }
    return n;
}

bool Source::write(std::span<const std::uint8_t> data) {
    if (auto* serial = std::get_if<os::SerialPort>(&impl_)) {
        return serial->write(data);
    }
    return false;
}

} // namespace malgrind::host
