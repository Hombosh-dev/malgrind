#pragma once

// Blocking in-memory streams, so a test can drive serve(std::istream&, std::ostream&) from another thread.

#include <condition_variable>
#include <deque>
#include <istream>
#include <mutex>
#include <optional>
#include <ostream>
#include <streambuf>
#include <string>

// Input: the test pushes lines, serve's getline blocks until one arrives; close() makes it see end of file.
class InPipe : public std::streambuf {
public:
    void push(const std::string& line) {
        {
            std::scoped_lock lock(mutex_);
            data_ += line + '\n';
        }
        cv_.notify_all();
    }
    void close() {
        {
            std::scoped_lock lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();
    }

protected:
    int_type underflow() override {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return !data_.empty() || closed_; });
        if (data_.empty()) return traits_type::eof();
        current_ = std::move(data_);
        data_.clear();
        setg(current_.data(), current_.data(), current_.data() + current_.size());
        return traits_type::to_int_type(*gptr());
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::string data_, current_;
    bool closed_ = false;
};

// Output: complete lines become available to the test as they are written.
class OutPipe : public std::streambuf {
public:
    // Next line, or nullopt after `limit` without one.
    std::optional<std::string> next(std::chrono::milliseconds limit = std::chrono::milliseconds(5000)) {
        std::unique_lock lock(mutex_);
        if (!cv_.wait_for(lock, limit, [&] { return !lines_.empty(); })) return std::nullopt;
        auto line = std::move(lines_.front());
        lines_.pop_front();
        return line;
    }

protected:
    int_type overflow(int_type c) override {
        if (c == traits_type::eof()) return c;
        std::scoped_lock lock(mutex_);
        if (c == '\n') {
            lines_.push_back(std::move(partial_));
            partial_.clear();
            cv_.notify_all();
        } else {
            partial_ += static_cast<char>(c);
        }
        return c;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::string> lines_;
    std::string partial_;
};
