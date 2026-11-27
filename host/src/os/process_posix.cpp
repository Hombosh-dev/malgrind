#include "os/process.hpp"

#include "os/paths.hpp"
#include "os/process_internal.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <mutex>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

#ifdef __APPLE__
#include <crt_externs.h>
#define MALGRIND_ENVIRON (*_NSGetEnviron())
#else
extern char** environ;
#define MALGRIND_ENVIRON environ
#endif

namespace fs = std::filesystem;

namespace malgrind::host::os {

namespace {

pid_t spawn(const std::vector<std::string>& argv, posix_spawn_file_actions_t* actions, char* const* envp = MALGRIND_ENVIRON) {
    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid = -1;
    return posix_spawnp(&pid, args[0], actions, nullptr, args.data(), envp) == 0 ? pid : -1;
}

int decode_wait_status(int status) noexcept {
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int wait_for_pid(pid_t pid) {
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return decode_wait_status(status);
}

bool make_cloexec_pipe(std::array<int, 2>& fds) {
    if (pipe(fds.data()) != 0) return false;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return true;
}

struct SpawnedPipe {
    pid_t pid = -1;
    int read_fd = -1;
};

std::optional<SpawnedPipe> spawn_merged_pipe(const std::vector<std::string>& argv,
                                             const std::vector<std::string>& env_add) {
    std::array<int, 2> fds{};
    if (argv.empty() || !make_cloexec_pipe(fds)) return std::nullopt;
    // this process's environment, minus the names that env_add sets, plus env_add
    std::vector<std::string> env;
    for (char** e = MALGRIND_ENVIRON; e && *e; ++e) {
        const std::string_view entry = *e;
        const bool replaced = std::ranges::any_of(env_add, [&](const std::string& add) {
            return entry.substr(0, entry.find('=') + 1) == std::string_view(add).substr(0, add.find('=') + 1);
        });
        if (!replaced) env.emplace_back(entry);
    }
    env.insert(env.end(), env_add.begin(), env_add.end());
    std::vector<char*> envp;
    envp.reserve(env.size() + 1);
    for (auto& e : env) envp.push_back(e.data());
    envp.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0); // tools must not wait for input
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_addclose(&actions, fds[1]);
    const pid_t pid = spawn(argv, &actions, envp.data());
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (pid <= 0) {
        close(fds[0]);
        return std::nullopt;
    }
    return SpawnedPipe{pid, fds[0]};
}

void read_fd_lines(int fd, const std::function<void(std::string_view)>& on_line) {
    std::string pending;
    std::array<char, 4096> buf;
    for (;;) {
        const ssize_t n = read(fd, buf.data(), buf.size());
        if (n > 0) {
            pending.append(buf.data(), static_cast<std::size_t>(n));
            detail::flush_lines(pending, on_line, false);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    detail::flush_lines(pending, on_line, true);
}

} // namespace

struct Child::Impl {
    mutable std::mutex mu;
    mutable pid_t pid = -1;
    mutable bool exited = false;
    mutable int exit_code = -1;
    std::mutex join_mu;
    std::thread reader;

    ~Impl() {
        kill();
        join_reader();
    }

    bool try_reap_locked() const {
        if (exited || pid <= 0) return true;
        int status = 0;
        pid_t r = 0;
        do {
            r = ::waitpid(pid, &status, WNOHANG);
        } while (r < 0 && errno == EINTR);
        if (r == pid) {
            exited = true;
            exit_code = decode_wait_status(status);
            pid = -1;
            return true;
        }
        if (r < 0) {
            exited = true;
            exit_code = -1;
            pid = -1;
            return true;
        }
        return false;
    }

    void join_reader() {
        std::scoped_lock lock(join_mu);
        if (reader.joinable()) reader.join();
    }

    bool running() const {
        std::scoped_lock lock(mu);
        return !try_reap_locked();
    }

    std::optional<int> wait_for(int timeout_ms) {
        using clock = std::chrono::steady_clock;
        const auto deadline = timeout_ms > 0 ? clock::now() + std::chrono::milliseconds(timeout_ms) : clock::time_point{};
        auto step = std::chrono::milliseconds(1);
        for (;;) {
            int code = -1;
            bool done = false;
            {
                std::scoped_lock lock(mu);
                done = try_reap_locked();
                code = exit_code;
            }
            if (done) {
                join_reader();
                return code;
            }
            if (timeout_ms <= 0) return std::nullopt;
            const auto now = clock::now();
            if (now >= deadline) return std::nullopt;
            std::this_thread::sleep_for(std::min<clock::duration>(step, deadline - now));
            step = std::min(step * 2, std::chrono::milliseconds(10));
        }
    }

    int wait() {
        auto step = std::chrono::milliseconds(1);
        for (;;) {
            int code = -1;
            bool done = false;
            {
                std::scoped_lock lock(mu);
                done = try_reap_locked();
                code = exit_code;
            }
            if (done) {
                join_reader();
                return code;
            }
            std::this_thread::sleep_for(step);
            step = std::min(step * 2, std::chrono::milliseconds(10));
        }
    }

    void kill() {
        {
            std::scoped_lock lock(mu);
            if (!try_reap_locked()) {
                ::kill(pid, SIGTERM);
            }
        }
        if (wait_for(200)) return;
        {
            std::scoped_lock lock(mu);
            if (!try_reap_locked()) {
                ::kill(pid, SIGKILL);
                int status = 0;
                pid_t r = 0;
                do {
                    r = ::waitpid(pid, &status, 0);
                } while (r < 0 && errno == EINTR);
                exited = true;
                exit_code = (r == pid) ? decode_wait_status(status) : -1;
                pid = -1;
            }
        }
        join_reader();
    }
};

Child::Child() noexcept = default;
Child::Child(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Child::Child(Child&&) noexcept = default;
Child& Child::operator=(Child&&) noexcept = default;
Child::~Child() = default;

std::optional<Child> Child::spawn(const std::vector<std::string>& argv, const std::vector<std::string>& env_add,
                                  std::function<void(std::string_view line)> on_line) {
    auto spawned = spawn_merged_pipe(argv, env_add);
    if (!spawned) return std::nullopt;
    auto impl = std::make_unique<Impl>();
    impl->pid = spawned->pid;
    const int read_fd = spawned->read_fd;
    impl->reader = std::thread([read_fd, cb = std::move(on_line)]() {
        read_fd_lines(read_fd, cb);
        close(read_fd);
    });
    return Child(std::move(impl));
}

bool Child::valid() const noexcept { return impl_ != nullptr; }
bool Child::running() const { return impl_ ? impl_->running() : false; }
void Child::kill() {
    if (impl_) impl_->kill();
}
std::optional<int> Child::wait_for(int timeout_ms) { return impl_ ? impl_->wait_for(timeout_ms) : std::nullopt; }
int Child::wait() { return impl_ ? impl_->wait() : -1; }

std::optional<fs::path> find_in_path(const std::string& name) {
    if (name.empty()) return std::nullopt;
    const auto path = get_env("PATH");
    if (!path) return std::nullopt;
    const auto bin = exe_name(name);
    for (const auto& dir : split_path_list(*path)) {
        const fs::path candidate = dir / bin;
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec) && ::access(candidate.c_str(), X_OK) == 0) return candidate;
    }
    return std::nullopt;
}

std::string capture(const std::vector<std::string>& argv) {
    std::array<int, 2> fds{};
    if (argv.empty() || !make_cloexec_pipe(fds)) return {};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_addclose(&actions, fds[1]);
    const pid_t pid = spawn(argv, &actions);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    std::string out;
    if (pid > 0) {
        std::array<char, 4096> buf;
        for (;;) {
            const ssize_t n = read(fds[0], buf.data(), buf.size());
            if (n > 0) out.append(buf.data(), static_cast<std::size_t>(n));
            else if (n < 0 && errno == EINTR) continue;
            else break;
        }
        wait_for_pid(pid);
    }
    close(fds[0]);
    return out;
}

int run(const std::vector<std::string>& argv) {
    if (argv.empty()) return -1;
    const pid_t pid = spawn(argv, nullptr);
    return pid > 0 ? wait_for_pid(pid) : -1;
}

int run_lines(const std::vector<std::string>& argv, const std::vector<std::string>& env_add,
              const std::function<void(std::string_view line)>& on_line) {
    auto spawned = spawn_merged_pipe(argv, env_add);
    if (!spawned) return -1;
    read_fd_lines(spawned->read_fd, on_line);
    close(spawned->read_fd);
    return wait_for_pid(spawned->pid);
}

} // namespace malgrind::host::os
